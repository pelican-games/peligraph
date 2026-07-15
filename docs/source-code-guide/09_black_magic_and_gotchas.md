# 第9章 黒魔術・制約・変更時の注意

[索引へ戻る](README.md) / [前章](08_class_interface_index.md)

ここでいう「黒魔術」は、コードが不可解という意味ではありません。macro、template、型消去、static 初期化、GPU 非同期寿命など、実際の処理が呼び出し箇所から離れて見える実装を指します。この章では、展開後に何が起きるかと、調査時点での制約を明示します。

## 9.1 `DECLARE_MODULE` / `GET_MODULE`: 小さな service locator

定義は [`container.hpp`](../../src/core/container.hpp#L8) です。

```cpp
#define DECLARE_MODULE(name) class name : public ModuleBase<name>
#define GET_MODULE(name) FastModuleContainer::get<name>()
```

`DECLARE_MODULE(Foo)` は概念的に次を足します。

```cpp
class Foo : public ModuleBase<Foo> { ... };

// ModuleBase<Foo> の中
static std::optional<Foo>& __get() {
    static std::optional<Foo> obj;
    return obj;
}
```

`GET_MODULE(Foo)` の初回は `optional.emplace()` で default constructor を呼び、型と cleanup function を global `cleaners` stack へ積みます。2回目以降は同じ object reference を返します。[`FastModuleContainer::get()`](../../src/core/container.hpp#L25) が全処理です。

### 破棄順

[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L32) の先頭で local `FastModuleContainer` を作ります。その destructor は、初期化と逆順に module の `optional.reset()` を行います。

```text
GET_MODULE(A) -> A constructor内でGET_MODULE(B)
初期化: Aのemplace開始 -> B登録 -> A登録
cleaners: [B, A]
破棄: A -> B
```

依存を constructor 内で取得すれば、通常は dependent が先に壊れます。ただし GPU/ECS は destructor だけへ任せず、[`RuntimeTeardownGuard`](../../src/core/appflow/teardown.cpp#L25) が明示 cleanup を先に行います。

### 注意点

- dependency は constructor 本体の `GET_MODULE` に隠れます。include graph だけでは実行時依存が分かりません。
- `cleaners` vector と `optional.emplace/reset` に mutex はありません。[`FastModuleContainer`](../../src/core/container.hpp#L20) は concurrent first access や複数 runtime 同時実行を想定していません。
- module reference/pointer は `PelicanCore::run()` の外へ保持してはいけません。container destructor 後は無効です。
- destructor から新しい `GET_MODULE` を呼ぶと、teardown 中に module を再生成し得ます。destructor は既に所有する dependency を使うか、明示 teardown で完結させる方が安全です。
- `get()` は dependency injection seam ではありません。pure algorithm をテストしたい場合は、frame planner のように module から切り離した free function/value 層を作る設計が合います。

## 9.2 typed handle は何を守り、何を守らないか

[`PELICAN_DEFINE_HANDLE`](../../src/core/handle.hpp#L21) は `struct PassId : BasicHandle<PassId,int>` のような薄い型を作ります。`PassId` と `RenderingPassId` の取り違えを compile 時に防ぎつつ、内部値は整数1個です。

一方、一般的な [`ResourceContainer`](../../src/core/resourcecontainer.hpp#L6) は monotonic counter と `unordered_map` だけです。

- handle は再利用しません。
- `unreg()` 後の handle に generation はありません。
- stale handle の `get()` は `unordered_map::at()` 例外になります。
- counter overflow の明示検査はありません。
- `BasicHandle` は base 整数への暗黙 conversion を持つため、logging/indexing は楽ですが、整数へ落とした後の型安全性は失われます。

ECS の `EntityId` だけは別で、index に generation を付けて stale entity を正常に拒否します。GPU resource handle と Entity handle を同じ寿命モデルだと考えないでください。

## 9.3 `PELICAN_REGISTER_EVENT` と `PELICAN_REGISTER_SYSTEM`

これらは runtime reflection ではなく、C++ の static object constructor と compile-time overload lookup の組み合わせです。

### event 登録の展開

[`PELICAN_REGISTER_EVENT_IMPL`](../../src/core/userpublic/details/event/registerer.hpp#L121) は概念的に2つを生成します。

1. `EventCatalogTag<N>` に対する `pelicanEventCatalogEntry(...) -> EventCatalogEntry<MyEvent>` overload。
2. anonymous namespace の static object。constructor で `registerEvent<MyEvent>("MyEvent")`。

`N` は `__COUNTER__` です。

### system 登録の展開

[`PELICAN_REGISTER_SYSTEM_IMPL`](../../src/core/userpublic/details/system/registerer.hpp#L128) も static object を作ります。その constructor は、system macro より前に見えている event catalog entry を `0..N-1` まで compile-time に探索します。`System` に `onEvent(const Event&, GameContext&)` があれば function pointer table へ追加し、最後に system 本体を登録します。

```cpp
PELICAN_REGISTER_EVENT(Damage)
PELICAN_REGISTER_SYSTEM(CombatSystem, 100)
```

この順序なら `CombatSystem::onEvent(const Damage&, ...)` を発見できます。

### translation unit と宣言順の罠

- event macro は system macro より前に、同じ translation unit の lookup から見える必要があります。通常は event 宣言と登録を header に置き、その header を system `.cpp` で先に include します。
- 別の `.cpp` だけで登録された event は、system 側の compile-time catalog からは見えません。
- system macro より後に event macro を置いても、その system の handler list には入りません。
- object file が最終 executable へ link されなければ static constructor も走りません。
- `__COUNTER__` は translation unit ごとであり、process-wide event ID ではありません。runtime の同一性は `std::type_index` と名前で判定します。

runtime update 順は static 初期化順ではなく、[`sortGameSystemRegistrations()`](../../src/core/userpublic/details/system/registerer.cpp#L22) が `(order, name)` で決めます。ここは決定論的です。

### event 名と RPC payload

[`eventDisplayName()`](../../src/core/userpublic/details/event/registerer.cpp#L13) は namespace qualifier を落とします。`foo::Changed` と `bar::Changed` は同じ `Changed` になり、異なる型なら duplicate error です。

RPC の `inject_event` 用 JSON loader は [`registerEvent<Event>()`](../../src/core/userpublic/details/event/registerer.hpp#L49) で選ばれます。

- default constructible かつ `ref(JsonArchiveLoader&)` がある: payload を field へ load。
- default constructible だが `ref` がない: default event を作り、渡された payload は使わない。
- default constructible でない: name injection 不可。

C++ の `GameContext::emit(event)` は copy した値をそのまま queue に置くため、この JSON 制約とは別です。

### system instance の寿命

[`gameSystemInstance<System>()`](../../src/core/userpublic/details/system/registerer.hpp#L42) は function-local static です。module container と違い、`PelicanCore::run()` ごとには再生成されません。同一 process で engine を複数回 run する test/tool では、System の member state が明示 reset されない限り次の run に残ります。

## 9.4 `GameObjects::add()` の typestate builder

[`GameObjects`](../../src/core/userpublic/gameobjects.hpp#L19) は fluent API に見えますが、内部では呼ぶたびに別 template 型を返します。

```cpp
auto id = GameObjects::add()
    .addComponent<LocalTransformComponent>()
    .addComponent(SimpleModelViewComponent{...})
    .finish();
```

型の中に次を埋め込みます。

- `ComponentIdHolder<...>`: compile-time の Component ID 列。
- `IndexHolder<...>`: 値を渡した component が pointer 配列の何番目か。
- `tuple<...>`: populate 時に代入する component data。

`finish()` で初めて ID span と populate lambda へ落ち、[`GameObjects::create()`](../../src/core/userpublic/gameobjects.cpp#L8) から ECS の transactional create へ入ります。template の目的は、runtime に型情報が消えた `void*` 配列へ、compile-time に正しい型と index で代入することです。

### reference lifetime の罠

値付き `addComponent(const T&)` は、値を copy して builder に保存するのではなく [`std::tuple<const T&>`](../../src/core/userpublic/gameobjects.hpp#L70) を保存します。

```cpp
// 安全: temporaryが破棄される前、同じfull-expressionでfinishする
auto id = GameObjects::add().addComponent(MyComponent{...}).finish();

// 危険: builderだけ保存すると内部referenceがdanglingになる
auto builder = GameObjects::add().addComponent(MyComponent{...});
auto id2 = builder.finish();
```

builder を後で使うなら、component value を別の lvalue として `finish()` まで生存させてください。重複 Component ID は最終的に [`createEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L93) が例外にします。

## 9.5 Component type erasure と lifecycle callback

ECS chunk は C++ 型を知りません。登録時に [`registerComponent<T>()`](../../src/core/userpublic/details/component/registerer.hpp#L34) が次を callback に変えます。

```text
T
├─ sizeof / alignof
├─ default construct
├─ destroy noexcept
├─ relocate noexcept
├─ optional init
├─ optional deinit noexcept
└─ optional ref(JsonArchiveLoader&)
```

non-trivial component の swap-delete は assignment ではなく、destination へ move-construct し source を destroy します。[`relocate` lambda](../../src/core/userpublic/details/component/registerer.hpp#L53) と [`VariedArray::removeAt()`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) が対になっています。

制約は compile-time に固定されます。

- default constructible。
- nothrow move constructible。
- nothrow destructible。
- `deinit()` があるなら `noexcept`。

これにより create rollback と remove/teardown を `noexcept` cleanup として実装できます。`init()` だけは例外を許し、既に init 済みの component を逆順 deinit して transaction を戻します。

### Component ID は dense index でもある

[`ComponentInfoManager::getIndexFromComponentId()`](../../src/core/ecs/componentinfo.cpp#L20) は ID をそのまま `size_t` へ cast します。さらに archetype mask は [`MAX_COMPONENTS = 64`](../../src/core/userpublic/details/ecs/componentdeclare.hpp#L21) の `uint64_t` です。

したがって Component ID は次を満たす必要があります。

- process 全体で一意。
- `0..63` の範囲。
- 同じ型は全 translation unit で同じ ID specialization を見る。

登録側の [`registerComponent()`](../../src/core/ecs/componentinfo.cpp#L9) は重複 ID を拒否せず slot を上書きし、古い name map entry を自動削除しません。ID 割当は API 利用者ではなく engine-wide schema として管理する必要があります。

### custom Component の現状

型 ID 宣言 macro は公開 header にありますが、runtime 登録入口は [`internal::getComponentRegisterer()`](../../src/core/userpublic/details/component/registerer.hpp#L87) です。production で自動登録されるのは [`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L17) の built-in 群です。

調査時点では game code 向けの安定した `PELICAN_REGISTER_COMPONENT` public macro/boot hook はありません。`DECLARE_COMPONENT` しただけでは `ComponentInfoManager` に metadata が入らず、create/scene load できません。テストは internal API を直接呼んで登録しています。[`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L125) が例です。

## 9.6 Archetype/SoA chunk と pointer lifetime

各 [`VariedArray`](../../src/core/userpublic/details/ecs/chunk.hpp#L17) は constructor で `stride * 4096` bytes を一度に aligned allocation します。[`chunk.cpp`](../../src/core/userpublic/details/ecs/chunk.cpp#L17) を参照してください。同じ component の値は連続し、system は `T* + count` で batch 処理できます。

storage 自体は chunk の生存中に再 allocation されません。しかし pointer が永久に同じ entity を指すわけではありません。

- entity remove は穴へ末尾 entity を move する swap-delete。
- 別 entity の remove でも、自分が末尾なら自分の値が別 address へ移る。
- scene load/clear は全 component を破棄。
- `GameObjectId` は generation で再解決できるが、生 pointer には generation がない。

したがって component pointer/reference は、structural mutation をまたいで cache しないでください。必要時に EntityId から [`tryComponent()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L104) で再取得します。

create/remove/clear の再入は [`MutationScope`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L25) が拒否します。component `init()` / `deinit()` callback からさらに structural mutation すると、部分更新を防ぐため例外になります。

## 9.7 内部 ECS scheduler の並列性

内部 ECS system は template 引数の pointer constness から read/write component index を抽出します。[`registerSystem()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L149) で `const T*` は read、`T*` は write です。

ただし現在、`write_indices` は実行後の version 更新に使われる一方、`read_indices` は保存されるだけで scheduler から参照されません。change 判定は要求 Component 全体の `component_indices` を見ます。system 間の競合 edge は自動生成せず、並列 level を決めるのは明示 `depends_list` だけです。

[`ECSCoreTemplatePublic::update()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L386) は dependency graph を Kahn 法で level 化し、同 level の system を `JobSystem` へすべて schedule してから wait します。

```text
level 0: A, B, C  -> parallel jobs -> wait
level 1: D, E     -> parallel jobs -> wait
```

注意点は次です。

- 同じ component を書く2 system でも、依存を明示しなければ同 level で race し得ます。
- `read_indices` / `write_indices` が conflict graph を自動構築するわけではありません。
- graph cycle がある場合、cycle 内 system は zero-degree queue に入らず、現実装は「全 system を取り出したか」を検証しません。frame graph planner と違い、cycle error ではなく cycle 部分が実行されない挙動です。
- dependency のない system の列挙元は `unordered_map` です。同 level の開始順を意味のある順序として使わないでください。
- system が batch 版 `process(std::vector<ChunkView<...>>)` と per-chunk 版 `process(tuple,count)` の両方を定義すると、[`p_func` の独立した2つの `if constexpr`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L201) により両方が呼ばれます。

game system registry はこれとは別機構で、現在は `(order,name)` 順の直列 update です。2種類の「System」を混同しないでください。

### change detection の意味

non-force system は matching chunk の component version と `last_run_tick` を比較します。component を raw pointer から直接書き換えただけでは version が上がりません。公開 mutation は [`setComponent()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L112) または [`markComponentChanged()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L365) を通す必要があります。

一方、built-in system の多くは `registerSystemForce` なので毎 frame 動きます。最適化時に force を外すなら、すべての mutation 経路が version を更新するか先に確認してください。

## 9.8 Camera、light、collider はすべて同じ ECS component ではない

scene の `components` 配列に見えても runtime binding は一様ではありません。[`prepareSceneBindings()`](../../src/core/loader/scene.cpp#L72) が分岐します。

| scene name | runtime 経路 |
|---|---|
| `transform`, `simplemodelview`, `camera` | ComponentInfo 経由で ECS chunk へ作成 |
| `light` | ECS へ入れず `LightLoadEntry` として `LightContainer::load()` |
| `collider` | ECS へ入れず `ColliderComponent` を parse し `PhysWorld::bindCollider()` |

`ColliderComponent` に `init/deinit` があっても、現在の scene loader は special case です。`ECSCoreTemplatePublic::tryComponent<ColliderComponent>()` で取れる通常 ECS component だとは考えないでください。

camera はさらに二重経路です。

- `Camera::loadSceneCameras()` が scene document を再走査し、projection、controller、名前付き camera を module 内に構築。[`camera.cpp`](../../src/core/renderer/camera.cpp#L453)
- 同じ object の `camera` marker と `transform` は ECS にも入り、forced [`CameraSystem`](../../src/core/ecs/predefined/camerasystem.cpp#L7) が最初の camera transform を module camera へ反映。

名前付き camera/controller と「最初の ECS camera」の責務が重なるため、camera 変更では両方を追う必要があります。また `CameraSystem::process()` は現在 `count` を使わず先頭1件を読む実装です。camera archetype の空 chunk が残る可能性を含め、empty view を扱う修正ではここを重点的にテストしてください。

## 9.9 Frame graph が保証するもの、しないもの

第6章の要点を、変更時の安全条件として再掲します。

### planner

- 自動 edge は宣言順で「直前 writer → reader」の RAW。
- WAW は明示 edge で全 writer を順序付けないと [`validateWritesAreOrdered()`](../../src/core/renderingpass/frameplanner.cpp#L444) が拒否。
- WAR は自動 edge なし。
- `after` / `before` は control edge。
- cycle は例外。
- stable topological order は作るが、[`levels`](../../src/core/renderingpass/frameplanner.cpp#L632) は現在並列実行に使わない。

### barrier

- ordered edge の同 resource write→read を barrier record にする。
- 実行側 [`bufferReadAfterWriteBarrier()`](../../src/core/renderingpass/computetask.cpp#L501) は storage buffer だけに `vk::BufferMemoryBarrier` を出す。
- image は layout tracker に依存。
- layout が変われば layout transition が memory dependency を含む。
- storage image が `GENERAL`→`GENERAL` のままなら tracker は早期 return するため、compute→compute の image RAW 専用 barrier は調査時点で出ない。

graph の node 順が正しいことと、Vulkan memory visibility が正しいことは別問題です。新 resource type を足すときは planner edge、runtime resource binding、stage/access mask、queue ownership の4点を一緒に設計します。

## 9.10 Compute 設定の「parse 済み」と「実行済み」を区別する

調査時点の実装範囲です。

| 設定 | parse | runtime behavior |
|---|---:|---|
| `dispatch.groups` | 済 | `vkCmdDispatch(x,y,z)` に使用 |
| `dispatch.groups_from` | 済 | 自動算出には未接続 |
| `dispatch.local_size` | 済 | 自動算出には未接続。shader reflection の local size とも結合しない |
| `schedule: per_frame` | 済 | 対応 |
| その他 schedule | 検出 | 明示 error |
| buffer `size > 0` | 済 | device-local storage buffer を一度確保 |
| buffer `size == 0` | 済 | graph 名だけ。実 buffer はなし |
| `lifetime: persistent` | 済 | 実質全 buffer が container lifetime |
| `lifetime: transient` | 済 | frame ごとの確保/recycle は未実装 |

根拠は [`parseDispatch()`](../../src/core/renderingpass/computetask.cpp#L152)、[`registerBuffers()`](../../src/core/renderingpass/computetask.cpp#L287)、[`registerComputeTask()`](../../src/core/renderingpass/computetask.cpp#L407) です。

compute task は dedicated compute queue へ submit せず、graphics frame command buffer に記録します。一方 [`pickQueues()`](../../src/core/vkcore/core.cpp#L54) の fallback は graphics と compute を別 family として受理できます。現 frame graph compute は graphics queue に compute capability があることを実質仮定していますが、fallback path はそれを必須検証していません。async compute を実装する場合は command pool/submit だけでなく queue family ownership transfer も必要です。

## 9.11 Shader reflection と hot reload の境界

reflection は descriptor layout と pipeline layout を source/SPIR-V から自動生成します。便利ですが、C++ 側の resource contract がなくなるわけではありません。

- set 0/1/2/3 の意味は [`pelican_sets.hpp`](../../src/core/shader/pelican_sets.hpp#L7) の convention。
- fullscreen/compute binder は特定 descriptor type を要求。
- material renderer は engine vertex layout/push constant の構造を前提にする。
- reflection は field の semantic を理解せず、set/binding/type/count/name だけを見る。

hot reload は shader compile と pipeline rebuild を transactional にします。しかし descriptor layout を変更する edit は、shader body だけの edit より危険です。

- [`handleShaderHotReload()`](../../src/core/vkcore/renderer.cpp#L418) は pipeline rebuild 後、fullscreen input descriptor を明示 rebind。
- compute descriptor set は [`registerComputeTask()`](../../src/core/renderingpass/computetask.cpp#L407) 時に一度作り、hot reload path では作り直していません。
- material/UI/debug の descriptor ownership も各 container に分散します。

したがって hot reload の安全な基本範囲は、既存 set/binding/type と push constant layout を保った shader body の変更です。layout-changing reload を正式対応するなら、pipeline 使用者ごとの descriptor rebuild notification が必要です。

## 9.12 GPU object は「C++で不要」になった時点では壊せない

pipeline、image view、buffer などは、CPU では旧 object に見えても GPU が前 frame の command から参照中かもしれません。[`DeletionQueueCore`](../../src/core/vkcore/deletionqueue.hpp#L16) は resource type を virtual base へ型消去し、defer frame を記録します。

2 frames-in-flight 後に [`releaseEligible()`](../../src/core/vkcore/deletionqueue.cpp#L45) が `optional<T>.reset()` して本物の RAII destructor を呼びます。hot reload の [`replacePipeline()`](../../src/core/shader/pipelinefactory.cpp#L392) が代表例です。

変更時の原則は次です。

- GPU が参照し得る旧 object を local temporary の destructor に任せない。
- new object を公開した後、old object を deletion queue へ移す。
- queue に入れる object が dependent object より先に破棄されても Vulkan 規約上安全か確認する。
- shutdown は wait-idle → pending flush → module destruction の順を維持する。

## 9.13 宣言・schema はあるが、runtime が未完成または別経路のもの

ソースを読むときに「型がある = 利用可能」と誤解しやすい箇所です。

| 項目 | 調査時点の状態 | コード |
|---|---|---|
| `JsonArchiveLoader` | 実装済み。scene component/event JSON load に使用 | [`jsonarchive.cpp`](../../src/core/userpublic/serialize/jsonarchive.cpp#L6) |
| `JsonArchiveSaver` | `prop` 宣言のみで、この repository 内に定義なし | [`jsonarchive.hpp`](../../src/core/userpublic/serialize/jsonarchive.hpp#L30) |
| `BinaryArchive` | `prop` 宣言のみで、この repository 内に定義なし | [`binaryarchive.hpp`](../../src/core/userpublic/serialize/binaryarchive.hpp#L10) |
| pose action | schema/type は parse 可能だが OpenXR binding 解決は明示 error | [`InputActionFrame::pose()`](../../src/core/os/actionmap.cpp#L515) |
| `.surface` / material format | pure parser と tests は存在。`src/core` runtime material loader への接続は調査時点でなし | [`surfaceformat.hpp`](../../src/project/surfaceformat.hpp#L48) / [`materialformat.hpp`](../../src/project/materialformat.hpp#L41) |
| Studio project editor | Qt/QML prototype。load/save、scene editing、engine IPC は未接続 | [`MainWindow`](../../src/devstudio/view/mainwindow.cpp#L10) |
| swapchain capture | `readbackLastFrameRGBA8()` は例外。capture は headless 用 | [`swapchainframetarget.cpp`](../../src/core/vkcore/swapchainframetarget.cpp#L316) |
| frame graph levels | 計算/JSON 出力のみ。runtime は直列 node loop | [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L317) |
| custom Component public registration | ID macro はあるが安定 public boot hook なし | [`component/registerer.hpp`](../../src/core/userpublic/details/component/registerer.hpp#L19) |

optional build feature には stub 実装もあります。たとえば SeqPlayer/VAT/RPC/audio は build option により実装または disabled behavior が選ばれます。header が同じでも build artifact の能力は [`build_features.hpp`](../../src/core/build_features.hpp#L1) と各 `*_stub.cpp` を確認してください。

## 9.14 症状から読む場所を決める

| 症状 | 最初の確認 | 次の確認 |
|---|---|---|
| 起動中に module constructor 例外 | module initialization log、[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L32) | constructor 内の `GET_MODULE` 依存 chain |
| entity が突然無効 | [`EntityId` generation](../../src/core/userpublic/details/ecs/entity.hpp#L12)、scene transition | remove/clear と stale handle test |
| component pointer の値が別 entity になる | [`swap-delete`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) | pointer を structural mutation 越しに保持していないか |
| ECS system が動かない | matching component mask、force/version | explicit dependencies の cycle、empty chunk |
| ECS system が時々壊れる | 同 level の read/write conflict | `depends_list`、raw mutation、JobSystem race |
| game system event が来ない | event macro が system macro より前に可視か | `onEvent` の完全な型 signature、event 短縮名 |
| RPC event payload が空 | event に `ref(JsonArchiveLoader&)` があるか | default-only JSON loader branch |
| frame graph の順が違う | [`currentFramePlanJson()`](../../src/core/vkcore/renderer.cpp#L449) | reads/writes、after/before、declaration index |
| Vulkan validation の RAW error | buffer/image、stage/access、layout | `GENERAL→GENERAL` image case、queue family |
| shader reload 後だけ壊れる | compile log、reflection diff | descriptor/push layout を変更していないか |
| resize 後だけ texture が古い | target recreate と fullscreen rebind | 該当 pass が独自 descriptor を cache していないか |
| headless capture が真っ黒 | frame plan と execution trace | target layout、pass output、golden fixture |
| shutdown crash | wait-idle と deletion queue | ECS `deinit()` が既に壊れた GPU module を触っていないか |

## 9.15 大きな変更の安全チェックリスト

### Component/ECS を変更する

- Component ID は一意かつ64未満か。
- lifecycle callbacks は construct された object にだけ呼ばれるか。
- init failure の逆順 rollback が保たれるか。
- remove の swap entity に対して `id_table.ref.array_index` を更新するか。
- raw write 後に component version を更新するか。
- 並列 system の read/write conflict に明示 dependency があるか。
- empty chunk と zero entity batch を処理できるか。

### rendering resource/pass を変更する

- JSON definition、validation、runtime compilation、execution dispatch の4層を更新したか。
- resize 後の image view rebind があるか。
- hot reload 後の descriptor rebuild があるか。
- old GPU object を遅延破棄したか。
- buffer と image の両方に正しい stage/access barrier があるか。
- headless と swapchain の両 frame target で成立するか。
- plan fixture、execution trace、golden image を確認したか。

### public API/RPC を変更する

- `GameContext` から内部 module 型を漏らしていないか。
- frame boundary のどこで反映されるかを定義したか。
- error を invalid params と application error のどちらにするか決めたか。
- stdout へ protocol 外文字列を出していないか。
- pure parser test と actual player subprocess test の両方があるか。

Pelican の複雑さは、ECS と Vulkan そのものよりも「compile-time 型情報を runtime table へ落とす境界」と「CPU 上の寿命を frame/GPU 上の寿命へ写す境界」に集まっています。その2か所では、便利な macro や RAII の表面だけでなく、登録時刻、pointer の有効期間、barrier、破棄順まで追うのが安全です。
