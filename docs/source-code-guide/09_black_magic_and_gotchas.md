# 第9章 黒魔術・制約・変更時の注意

[索引へ戻る](README.md) / [前章](08_class_interface_index.md)

ここでいう「黒魔術」は、コードが不可解という意味ではありません。macro、template、型消去、static 初期化、GPU 非同期寿命など、実際の処理が呼び出し箇所から離れて見える実装を指します。この章では、展開後に何が起きるかと、調査時点での制約を明示します。

## 9.1 `DECLARE_MODULE` / `GET_MODULE`: 小さな service locator

定義は [`container.hpp`](../../src/core/container.hpp#L15-L16) です。

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

`GET_MODULE(Foo)` の初回は `optional.emplace()` で default constructor を呼び、型と cleanup function を global `cleaners` stack へ積みます。2回目以降は同じ object reference を返します。[`FastModuleContainer::get()`](../../src/core/container.hpp#L149) が全処理です。

### 破棄順

[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L44) の先頭で local `FastModuleContainer` を作ります。その destructor は、初期化と逆順に module の `optional.reset()` を行います。

```text
GET_MODULE(A) -> A constructor内でGET_MODULE(B)
初期化: Aのemplace開始 -> B登録 -> A登録
cleaners: [B, A]
破棄: A -> B
```

依存を constructor 内で取得すれば、通常は dependent が先に壊れます。ただし GPU/ECS は destructor だけへ任せず、[`RuntimeTeardownGuard`](../../src/core/appflow/teardown.cpp#L31) が明示 cleanup を先に行います。

### 注意点

- dependency は constructor 本体の `GET_MODULE` に隠れます。include graph だけでは実行時依存が分かりません。
- `cleaners` と `optional.emplace/reset` は現在 [`std::recursive_mutex state_mutex`](../../src/core/container.hpp#L62) で保護されています(執筆時点の「mutex なし」は失効)。ただし複数 runtime 同時実行を想定しない設計自体は変わりません。
- **[`FastModuleContainer::freezeCreation()`](../../src/core/container.hpp#L199)**(Loop 開始直前、[loop.cpp#L374](../../src/core/appflow/loop.cpp#L374) で呼ばれる)以降は新規 module 生成が禁止されます。「render 中に初めて `GET_MODULE` する」コードは freeze 後に失敗するため、`Renderer::prepareRuntimeModules()` / `prepareFrameStateModules()` のように起動時に依存を先解決するパターンが必須です。生成しない読み取りには [`tryGet<T>()`](../../src/core/container.hpp#L144) があります。shutdown 側には `beginShutdown()`(#L213)が加わりました。
- module reference/pointer は `PelicanCore::run()` の外へ保持してはいけません。container destructor 後は無効です。
- destructor から新しい `GET_MODULE` を呼ぶと、teardown 中に module を再生成し得ます。destructor は既に所有する dependency を使うか、明示 teardown で完結させる方が安全です。
- `get()` は dependency injection seam ではありません。pure algorithm をテストしたい場合は、frame planner のように module から切り離した free function/value 層を作る設計が合います。

## 9.2 typed handle は何を守り、何を守らないか

[`PELICAN_DEFINE_HANDLE`](../../src/core/handle.hpp#L21) は `struct PassId : BasicHandle<PassId,int>` のような薄い型を作ります。`PassId` と `RenderingPassId` の取り違えを compile 時に防ぎつつ、内部値は整数1個です。

一方、一般的な [`ResourceContainer`](../../src/core/resourcecontainer.hpp#L13) は monotonic counter と `unordered_map` だけです。

- handle は再利用しません。
- `unreg()` 後の handle に generation はありません。
- stale handle の `get()` は `unordered_map::at()` 例外になります。
- counter overflow の明示検査はありません。
- `BasicHandle` は base 整数への暗黙 conversion を持つため、logging/indexing は楽ですが、整数へ落とした後の型安全性は失われます。

世代付きの識別子は増えました。ECS の `EntityId`(index + generation)に加え、[`ModelInstanceId`](../../src/core/renderer/modelinstance.hpp#L11) が `index + generation + scene_epoch` になり(WP146 / INSTANCE0)、[`RegistrationToken`](../../src/core/userpublic/details/reload/registrationowner.hpp#L30) と `RegistrationOwner` も identity + generation です。

**それでも `ResourceContainer` 系の GPU ハンドルだけは依然として世代なし** です。GPU resource handle と、世代付きの Entity / ModelInstance / Registration handle を同じ寿命モデルだと考えないでください。

### `ModelInstanceId` の落とし穴

`ModelInstanceId` は 3 フィールドの比較可能な値型で、`toString()` は `"index:generation@scene_epoch"` を返します([modelinstance.hpp#L21](../../src/core/renderer/modelinstance.hpp#L21))。

- `scene_epoch` が進むのは [`PolygonInstanceContainer::clear()`](../../src/core/renderer/polygoninstancecontainer.cpp#L521)(scene clear / 再ロード)の **1 か所だけ** です([`++scene_epoch`](../../src/core/renderer/polygoninstancecontainer.cpp#L545))。個別 slot が死んで再利用されるときに進むのは [`instance_generations[index]`](../../src/core/renderer/polygoninstancecontainer.cpp#L536)、つまり `generation` の方です。いずれも animation generation や model asset content revision からは独立です。
- 生存確認は [`isModelInstanceAlive(id)`](../../src/core/renderer/polygoninstancecontainer.hpp#L317)。
- [`removeModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L298) は `void` ではなく **`bool`** を返します。戻り値を無視すると「消したつもりで消えていない」を見逃します。
- `instanceCountForTesting()` は **live 数** を返すよう変わり、スロット総数は `slotCountForTesting()` です。両者の差は空きスロットです。

登録は 3 段階に分かれました(WP144 / TRANSIENT0)。[`preflightModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L292)(Vulkan 資源確保前の容量拒否)→ [`stageModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L293) → [`publishModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L296)(`noexcept`、**唯一の no-fail 公開点**)です。詳細は §9.17。

## 9.3 `PELICAN_REGISTER_EVENT` / `PELICAN_REGISTER_SYSTEM` / `PELICAN_REGISTER_BEHAVIOR`

これらは runtime reflection ではなく、C++ の static object constructor と compile-time overload lookup の組み合わせです。

### event 登録の展開

[`PELICAN_REGISTER_EVENT_IMPL`](../../src/core/userpublic/details/event/registerer.hpp#L242) は概念的に2つを生成します(macro 本体は [`PELICAN_REGISTER_EVENT`](../../src/core/userpublic/details/event/registerer.hpp#L257))。

1. `EventCatalogTag<N>` に対する `pelicanEventCatalogEntry(...) -> EventCatalogEntry<MyEvent>` overload。
2. anonymous namespace の static object。constructor で `registerEvent<MyEvent>("MyEvent")`。

`N` は `__COUNTER__` です。

### system 登録の展開

[`PELICAN_REGISTER_SYSTEM_IMPL`](../../src/core/userpublic/details/system/registerer.hpp#L155) も static object を作ります(macro 本体は [`PELICAN_REGISTER_SYSTEM`](../../src/core/userpublic/details/system/registerer.hpp#L173))。その constructor は、system macro より前に見えている event catalog entry を `0..N-1` まで compile-time に探索します。`System` に `onEvent(const Event&, GameContext&)` があれば function pointer table へ追加し、最後に system 本体を登録します。生成される static object は登録の戻り値である [`RegistrationToken token;`](../../src/core/userpublic/details/system/registerer.hpp#L158) をメンバとして保持します。

```cpp
PELICAN_REGISTER_EVENT(Damage)
PELICAN_REGISTER_SYSTEM(CombatSystem, 100)
```

この順序なら `CombatSystem::onEvent(const Damage&, ...)` を発見できます。

### behavior 登録の展開

[`PELICAN_REGISTER_BEHAVIOR(Type, stable_name, schema_version)`](../../src/core/userpublic/details/behavior/registerer.hpp#L319)(IMPL は [#L298](../../src/core/userpublic/details/behavior/registerer.hpp#L298))も **まったく同じ `__COUNTER__` + catalog 走査方式** です。[`collectBehaviorEventHandlers<Type>()`](../../src/core/userpublic/details/behavior/registerer.hpp#L99) が `onEvent(const Event&, BehaviorContext&)` を探します。

したがって **翻訳単位の順序の罠がそのまま適用されます**。event 宣言 header は behavior の `.cpp` の先頭で include してください。加えて 2 点、System とは違う注意があります。

- handler の第2引数は `BehaviorContext&` です。`GameContext&` 版を書くと、compile は通るのに **catalog に載らず、呼ばれません**。
- `onEvent` は `Behavior` の virtual メンバではありません。`override` を付けられない代わりに、typo が黙って無視されます。

### system 登録の第3のフック

[`HasGameSystemQueuedEvent`](../../src/core/userpublic/details/system/registerer.hpp#L48)(`void dispatchQueuedEvent(const QueuedEvent&, GameContext&)`)が加わりました。これにより「update も onEvent も無い System は登録エラー」の条件は `!has_queued_event && event_handlers.empty()` に変わっています([registerer.hpp#L106-L110](../../src/core/userpublic/details/system/registerer.hpp#L106))。

エラー文言自体は据え置きです。

```text
registered game systems must define update(ctx) or onEvent(event, ctx)
```

文言と実条件がややズレているので、`dispatchQueuedEvent` だけを持つ System を書くときは「このメッセージが出たら 3 つとも無い」と読み替えてください。

### translation unit と宣言順の罠

- event macro は system macro より前に、同じ translation unit の lookup から見える必要があります。通常は event 宣言と登録を header に置き、その header を system `.cpp` で先に include します。
- 別の `.cpp` だけで登録された event は、system 側の compile-time catalog からは見えません。
- system macro より後に event macro を置いても、その system の handler list には入りません。
- object file が最終 executable へ link されなければ static constructor も走りません。
- `__COUNTER__` は translation unit ごとであり、process-wide event ID ではありません。runtime の同一性は `std::type_index` と名前で判定します。

runtime update 順は static 初期化順ではなく、[`sortGameSystemRegistrations()`](../../src/core/userpublic/details/system/registerer.cpp#L33) が `(order, name)` で決めます。ここは決定論的です。Behavior もこの全順序に order 50 の 1 点として参加します([第5章 §5.13](05_gameplay_and_services.md))。

registry の各登録には [`RegistrationOwner`](../../src/core/userpublic/details/system/registerer.hpp#L33)(engine / game DLL)が付きます。game DLL reload では [`unregisterGameSystems(owner)`](../../src/core/userpublic/details/system/registerer.hpp#L142) が旧 DLL の static 登録を外し、新 DLL の static 初期化が再登録します。個別解除用に [`unregisterGameSystem(token)`](../../src/core/userpublic/details/system/registerer.hpp#L141) もあります。また event 型は [`EventPayloadSchema`](../../src/core/userpublic/details/event/payloadschema.hpp#L53) で宣言的 payload schema を持てるようになり、`fail_*` fixture は compile-time 検証([`run_event_schema_compile.cmake`](../../test/run_event_schema_compile.cmake))になっています。

### event 名と RPC payload

[`eventDisplayName()`](../../src/core/userpublic/details/event/registerer.cpp#L20) は namespace qualifier を落とします。`foo::Changed` と `bar::Changed` は同じ `Changed` になり、異なる型なら duplicate error です。

RPC の `inject_event` 用 JSON loader は [`registerEvent<Event>()`](../../src/core/userpublic/details/event/registerer.hpp#L62) で選ばれます。

- default constructible かつ `ref(JsonArchiveLoader&)` がある: payload を field へ load。
- default constructible だが `ref` がない: default event を作り、渡された payload は使わない。
- default constructible でない: name injection 不可。

C++ の `GameContext::emit(event)` は copy した値をそのまま queue に置くため、この JSON 制約とは別です。

### system instance の寿命

[`gameSystemInstance<System>()`](../../src/core/userpublic/details/system/registerer.hpp#L53) は function-local static です。module container と違い、`PelicanCore::run()` ごとには再生成されません。同一 process で engine を複数回 run する test/tool では、System の member state が明示 reset されない限り次の run に残ります。ただしこの注意が残るのは **engine 側の System のみ**です。game System は DLL 内 static なので、DLL reload 後は新インスタンスになり、member state は持ち越されません。

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

[`ComponentInfoManager::getIndexFromComponentId()`](../../src/core/ecs/componentinfo.cpp#L83) は値としては ID をそのまま index に使いますが、**現在は `get(id)` 経由になりました**。未登録スロットの読み出しは [`getFromIndex()`](../../src/core/ecs/componentinfo.cpp#L86) が `std::out_of_range("component index N is not registered")` を投げます。黙って壊れた metadata を返すことはありません。さらに archetype mask は [`MAX_COMPONENTS = 64`](../../src/core/userpublic/details/ecs/componentdeclare.hpp#L21) の `uint64_t` です。

したがって Component ID は次を満たす必要があります。

- process 全体で一意。
- `0..63` の範囲。
- 同じ型は全 translation unit で同じ ID specialization を見る。

**登録側の検査は全面的に強化されました**(WP163 / ECS1)。[`ComponentInfoManager::registerComponent()`](../../src/core/ecs/componentinfo.cpp#L20) は以下をすべて例外で拒否します。旧版の「黙って slot を上書きする」挙動はもうありません。

| 条件 | 文言 |
|---|---|
| 空名 | `component registration name must not be empty` |
| ID >= 64 | `component '<name>' id N exceeds the registration limit (<64)` |
| ID 重複 | `component '<name>' duplicates id N already registered by '<other>'` |
| 名前重複 | `component name '<name>' duplicates registered id N while incoming id is M` |
| lifecycle metadata 欠落 | `component '<name>' registration requires typed lifecycle metadata` |

ID 割当を engine-wide schema として管理する必要がある点は変わりませんが、間違いは **起動時に落ちて分かる** ようになりました。

登録解除 API も入りました。[`unregisterComponent(token)`](../../src/core/userpublic/details/component/registerer.hpp#L90) / [`unregisterComponents(owner)`](../../src/core/userpublic/details/component/registerer.hpp#L91) / `componentRegistrationCount(owner)` です。ただし [`ComponentInfoManager::unregisterComponent()`](../../src/core/ecs/componentinfo.cpp#L64) は **依存 System が残っていると拒否** します。

```text
cannot unregister component '<name>': dependent ECS system '<sys>' remains
```

### custom Component の現状

型 ID 宣言 macro は公開 header にありますが、runtime 登録入口は [`internal::getComponentRegisterer()`](../../src/core/userpublic/details/component/registerer.hpp#L89) です。production で自動登録されるのは [`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L20) の built-in 群です。

game code 向けの安定した `PELICAN_REGISTER_COMPONENT` public macro/boot hook は **現在も存在しません**(リポジトリ全体を grep しても不在)。`DECLARE_COMPONENT` しただけでは `ComponentInfoManager` に metadata が入らず、create/scene load できません。テストは internal API を直接呼んで登録しています。[`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L125) が例です。

なお `registerComponent<T>()` の戻り値は `void` から [`RegistrationToken`](../../src/core/userpublic/details/component/registerer.hpp#L36) へ変わりました。

## 9.6 Archetype/SoA chunk と pointer lifetime

各 [`VariedArray`](../../src/core/userpublic/details/ecs/chunk.hpp#L17) は constructor で `stride * 4096` bytes を一度に aligned allocation します。[`chunk.cpp`](../../src/core/userpublic/details/ecs/chunk.cpp#L17) を参照してください。同じ component の値は連続し、system は `T* + count` で batch 処理できます。

storage 自体は chunk の生存中に再 allocation されません。しかし pointer が永久に同じ entity を指すわけではありません。

- entity remove は穴へ末尾 entity を move する swap-delete。
- 別 entity の remove でも、自分が末尾なら自分の値が別 address へ移る。
- scene load/clear は全 component を破棄。
- `GameObjectId` は generation で再解決できるが、生 pointer には generation がない。

したがって component pointer/reference は、structural mutation をまたいで cache しないでください。必要時に EntityId から [`tryComponent()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L343) で再取得します。

create/remove/clear の再入は [`MutationScope`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L25) が拒否します。component `init()` / `deinit()` callback からさらに structural mutation すると、部分更新を防ぐため例外になります。

## 9.7 内部 ECS scheduler の並列性

内部 ECS system は template 引数の pointer constness から read/write component index を抽出します。[`registerSystem()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L398) で `const T*` は read、`T*` は write です。現在の signature は instance 参照を受け取り `SystemId` を返す形で、登録時に [`static_assert`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L399) が「batch か per-chunk のどちらかの process 形」を必須にします。

**WP148 / ECS0 でこの節は大きく変わりました。** 以前の「read/write は conflict graph を作らない」「cycle は例外にならない」という記述は失効しています。

実行計画は [`internal::buildECSExecutionPlan(nodes, hazard_policy)`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L206) が作ります。[`ECSCoreTemplatePublic::update()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L660) は計画を Kahn 法で level 化し、level ごとに「全 System の `prepare_func` を owner thread で実行 → 全部を `JobSystem` へ schedule → `wait()`」を行います。

```text
level 0: A, B, C  -> parallel jobs -> wait
level 1: D, E     -> parallel jobs -> wait
```

### hazard は自動検出される

[`conflictingComponents()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L170) が「同じ component index を触り、少なくとも片方が write」の組を競合として抽出します。ただし **競合の検出であって、依存 edge の自動導出ではありません**。扱いは policy 次第です。

| policy | 挙動 |
|---|---|
| `automatic_serialization`(既定) | 登録 ID の小さい方を先に実行する暗黙 edge を足し、**WARNING ログを出す** |
| `strict` | 全 hazard を集めて 1 本の例外にする |

> **設計決定:** policy の切替トリガは **`--strict-assets`** です([coretemplate.cpp#L664-L670](../../src/core/userpublic/details/ecs/coretemplate.cpp#L664))。コメントが理由を書いています。「`--strict-assets` は既存の起動時 strict/determinism ゲートであり、それを再利用することで WP148 を scheduler だけの変更境界に収めた」。名前から ECS scheduler を連想しにくいので注意してください。

自動直列化の WARNING 文言です。

```text
ECS auto serialization: '{}' before '{}' for component(s) {}; add an explicit dependency edge (strict mode rejects this hazard)
```

strict 側の例外はすべての hazard を `; ` で連結し、末尾に `; add dependency edges or use automatic serialization` が付きます。個々の hazard 行は次の形です。

```text
ECS unordered component hazard between systems '<A>' and '<B>' on component(s) '<name>'
```

### 現在の注意点

- **無警告の race はもう起きません。** ただし逆に、**自動直列化は WARNING でしか通知されません**。ログを見ていないと「なぜか並列化されない」「なぜか順序が登録順に固定された」ことに気づけません。性能を気にするなら WARNING を潰して明示 edge を書いてください。
- **cycle は例外になりました。** `makeExecutionLevels()` が `executed_count != nodes.size()` を検査し、`ECS dependency cycle detected; unexecuted systems: '<name>' ...` を投げます([coretemplate.cpp#L138-L147](../../src/core/userpublic/details/ecs/coretemplate.cpp#L138))。
- **存在しない System への依存、重複依存も起動時エラー** です([coretemplate.cpp#L220-L240](../../src/core/userpublic/details/ecs/coretemplate.cpp#L220))。`... depends on missing system id N; system would be unexecuted` / `... declares dependency on system '<name>' more than once; system would be unexecuted`。
- 計画作成時に node は `node.id` で sort されるため([coretemplate.cpp#L209-L212](../../src/core/userpublic/details/ecs/coretemplate.cpp#L209))、同 level 内の並びは登録順で決定的です。**それでも同 level は並列実行されるので、開始順に依存しないでください。**
- 実行計画は `execution_levels` にキャッシュされ、`execution_plan_dirty` か policy 変化のときだけ再構築されます。`registerSystem()` / `unregisterSystem()` が dirty を立てます。
- system が batch 版 `process(std::vector<ChunkView<...>>)` と per-chunk 版 `process(tuple,count)` の両方を定義すると、独立した2つの `if constexpr` により **両方が呼ばれます**(この点は変わっていません)。

### 組み込み System の依存はほぼ全順序になった

[`predefined.cpp#L31-L47`](../../src/core/ecs/predefined.cpp#L31) の現在の依存です(全て `registerSystemForce`)。

| System | 依存 |
|---|---|
| `LocalTransformSystem` | なし |
| `SimpleModelViewUpdateSystem` | なし |
| `AnimationSystem` | `{model_update}` |
| `SimpleModelViewTransformSystem` | `{local_transform, model_update, animation}` |
| `CameraSystem` | `{local_transform, model_transform}` |
| `SpriteViewRenderSystem` | `{local_transform, camera}` |

### prepare フェーズ: worker job 内の `GET_MODULE` を避ける新しい作法

level 実行前に、scheduler は各 System の [`prepare_func`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L735) を owner thread で呼びます。System は `prepareEcsWorkerDependencies()`(例: [`camerasystem.cpp#L9`](../../src/core/ecs/predefined/camerasystem.cpp#L9))で `GET_MODULE` を owner thread 上で済ませます。9.1 の `freezeCreation()` により worker job 内からの新規 module 生成は失敗するため、この prepare 契約が新しい落とし穴であり作法です。

game system registry はこれとは別機構で、現在は `(order,name)` 順の直列 update です。2種類の「System」を混同しないでください。

### change detection の意味

non-force system は matching chunk の component version と `last_run_tick` を比較します。component を raw pointer から直接書き換えただけでは version が上がりません。公開 mutation は [`setComponent()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L353) または [`markComponentChanged()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L637) を通す必要があります。

一方、built-in system の多くは `registerSystemForce` なので毎 frame 動きます。最適化時に force を外すなら、すべての mutation 経路が version を更新するか先に確認してください。

## 9.8 Camera、light、collider はすべて同じ ECS component ではない

scene の `components` 配列に見えても runtime binding は一様ではありません。[`prepareSceneBindings()`](../../src/core/loader/scene.cpp#L91) が分岐します。系統は **4 つ**になりました。

| scene name | runtime 経路 |
|---|---|
| `transform`, `simplemodelview`, `camera` | ComponentInfo 経由で ECS chunk へ作成 |
| `light` | ECS へ入れず `LightLoadEntry` として `LightContainer::load()` |
| `collider` | ECS へ入れず `ColliderComponent` を parse し `PhysWorld::bindCollider()`。`PELICAN_WITH_PHYSICS` OFF の build では collider を含む scene は明示エラー([`scene.cpp#L294-L301`](../../src/core/loader/scene.cpp#L294)) |
| `behavior` | ECS へ入れず [`prepareSceneBehaviorAttachments()`](../../src/core/gamelogic/behaviorarena.cpp#L70) 経由で `BehaviorAttachmentArena` へ([`scene.cpp#L155`](../../src/core/loader/scene.cpp#L155))。`type` は非空文字列必須。game DLL 未ロードなら **pending** 扱いで警告のみ |

`ColliderComponent` に `init/deinit` があっても、現在の scene loader は special case です。`ECSCoreTemplatePublic::tryComponent<ColliderComponent>()` で取れる通常 ECS component だとは考えないでください。behavior も同様で、ECS の component として問い合わせても見つかりません。

behavior の公開は [`arena.publishSceneAttachments()`](../../src/core/loader/scene.cpp#L451) の 1 点で、失敗すると [`clearRuntimeScene()`](../../src/core/loader/scene.cpp#L454) してから rethrow します。「behavior 型名を間違えると scene が半分だけロードされる」ということはありません。

### ライトのマジックネームは撤去された(WP142 / LIGHT0)

以前は engine が `"KeyLight"` / `"FillLight"` / `"PointLight1"` / `"SpotLight1"` といった名前を特別扱いし、`LightContainer::updateAnimation(float time)` が勝手にアニメーションさせていました。**この挙動と、原本値を保持していた `m_OriginalDirectionalLights` などの配列は削除されました。**

- 代替は公開 API 4 本([`GameContext::setDirectionalLightDirection()` ほか](../../src/core/userpublic/gamecontext.hpp#L50))で、ライトの時間変化は **ユーザー空間の責務** になりました。実例は [`projects/example/code/playercontrol.cpp`](../../projects/example/code/playercontrol.cpp#L31) の `updateLightAnimation()` です。
- setter の結果は `Renderer` の per-frame [`updateFrameLights()`](../../src/core/vkcore/renderer.cpp#L187)(呼び出しは [#L1295](../../src/core/vkcore/renderer.cpp#L1295))で GPU バッファへ反映されます。
- 「アップグレード後にライトが動かなくなった」は仕様です。scene 名に依存した暗黙アニメーションを期待しているコードを探してください。

代わりに **上限超過の警告** が入りました。[`collectLightCapWarnings()`](../../src/core/light/lightcontainer.hpp#L21) が `MAX_DIRECTIONAL_LIGHTS` / `MAX_POINT_LIGHTS` / `MAX_SPOT_LIGHTS` を超えた分について次を出します([lightcontainer.cpp#L52](../../src/core/light/lightcontainer.cpp#L52))。

```text
Light cap exceeded: <type> light #<ordinal> '<name>' will not be rendered (cap <N>)
```

`<name>` が空なら `<unnamed>` です。「ライトを足したのに 1 個だけ描かれない」ときはこの WARNING を先に探してください。テストは [`test/lightpolicy_test.cpp`](../../test/lightpolicy_test.cpp) です。

### camera の二重経路

- `Camera::loadSceneCameras()` が scene document を再走査し、projection、controller、名前付き camera を module 内に構築。[`camera.cpp#L645`](../../src/core/renderer/camera.cpp#L645)
- 同じ object の `camera` marker と `transform` は ECS にも入り、forced [`CameraSystem`](../../src/core/ecs/predefined/camerasystem.cpp#L7) が最初の camera transform を module camera へ反映。

名前付き camera/controller と「最初の ECS camera」の責務が重なるため、camera 変更では両方を追う必要があります。`CameraSystem::process()` は現在 [`count == 0` で早期 return](../../src/core/ecs/predefined/camerasystem.cpp#L14) するようになりましたが、「先頭1件のみ使用」は変わっていません。複数 camera entity を扱う修正ではここを重点的にテストしてください。

## 9.9 Frame graph が保証するもの、しないもの

第6章の要点を、変更時の安全条件として再掲します。

### planner

- 自動 edge は宣言順で「直前 writer → reader」の RAW。
- WAW は明示 edge で全 writer を順序付けないと [`validateWritesAreOrdered()`](../../src/core/renderingpass/frameplanner.cpp#L545) が拒否。
- WAR は自動 edge なし。
- `after` / `before` は control edge。
- cycle は例外。
- stable topological order は作るが、[`levels`](../../src/core/renderingpass/frameplanner.cpp#L723) は現在並列実行に使わない。

### barrier

- ordered edge の同 resource write→read を barrier record にする。
- 実行側 [`bufferReadAfterWriteBarrier()`](../../src/core/renderingpass/computetask.cpp#L501) は storage buffer だけに `vk::BufferMemoryBarrier` を出す。
- image は layout tracker に依存。tracker のキーは `(rt_id, surface_index)` になり、history 付き target の現/旧 surface を別々に追跡します([`render_target_layout_tracker.cpp#L72`](../../src/core/vkcore/render_target_layout_tracker.cpp#L72))。
- layout が変われば layout transition が memory dependency を含む。
- storage image が `GENERAL`→`GENERAL` のままなら tracker は早期 return するため、compute→compute の image RAW 専用 barrier は現在も出ない([同 #L76](../../src/core/vkcore/render_target_layout_tracker.cpp#L76))。

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

compute task は dedicated compute queue へ submit せず、graphics frame command buffer に記録します。一方 [`pickQueues()`](../../src/core/vkcore/core.cpp#L141) の fallback は graphics と compute を別 family として受理できます。現 frame graph compute は graphics queue に compute capability があることを実質仮定していますが、fallback path はそれを必須検証していません。async compute を実装する場合は command pool/submit だけでなく queue family ownership transfer も必要です。

## 9.11 Shader reflection と hot reload の境界

reflection は descriptor layout と pipeline layout を source/SPIR-V から自動生成します。便利ですが、C++ 側の resource contract がなくなるわけではありません。

- set 0/1/2/3 の意味は [`pelican_sets.hpp`](../../src/core/shader/pelican_sets.hpp#L7) の convention。
- fullscreen/compute binder は特定 descriptor type を要求。
- material renderer は engine vertex layout/push constant の構造を前提にする。
- reflection は field の semantic を理解せず、set/binding/type/count/name だけを見る。

hot reload は shader compile と pipeline rebuild を transactional にします。しかし descriptor layout を変更する edit は、shader body だけの edit より危険です。

- reload の publish は render 冒頭で [`consumeShaderReloadPublication()`](../../src/core/vkcore/renderer.cpp#L857) が consume し、[`rebindFullscreenInputs()`](../../src/core/vkcore/renderer.cpp#L841) が fullscreen input descriptor を明示 rebind します(執筆時点の `handleShaderHotReload()` は分割されました)。
- compute descriptor set は [`registerComputeTask()`](../../src/core/renderingpass/computetask.cpp#L407) 時に一度作り、hot reload path では作り直していません。
- material は [`prepareSurfaceMaterialReload()`](../../src/core/material/materialcontainer.hpp#L190) により surface/material 連動 reload に対応しました。UI/debug の descriptor ownership は各 container に分散したままです。

したがって hot reload の安全な基本範囲は、既存 set/binding/type と push constant layout を保った shader body の変更です。layout-changing reload を正式対応するなら、pipeline 使用者ごとの descriptor rebuild notification が必要です。

## 9.12 GPU object は「C++で不要」になった時点では壊せない

pipeline、image view、buffer などは、CPU では旧 object に見えても GPU が前 frame の command から参照中かもしれません。[`DeletionQueueCore`](../../src/core/vkcore/deletionqueue.hpp#L16) は resource type を virtual base へ型消去し、defer frame を記録します。

2 frames-in-flight 後に [`releaseEligible()`](../../src/core/vkcore/deletionqueue.cpp#L45) が `optional<T>.reset()` して本物の RAII destructor を呼びます。hot reload の [`replacePipeline()`](../../src/core/shader/pipelinefactory.cpp#L450) が代表例です。

変更時の原則は次です。

- GPU が参照し得る旧 object を local temporary の destructor に任せない。
- new object を公開した後、old object を deletion queue へ移す。
- queue に入れる object が dependent object より先に破棄されても Vulkan 規約上安全か確認する。
- shutdown は wait-idle → pending flush → module destruction の順を維持する。

### teardown 開始後の `defer()` はエラー

`DeletionQueueCore` に [`accepting` / `draining`](../../src/core/vkcore/deletionqueue.hpp#L40) フラグと [`requireAccepting()`](../../src/core/vkcore/deletionqueue.hpp#L44) が入りました。`defer()` は先頭でこれを呼ぶため、**受け入れ停止後に defer するとエラーになります**。

teardown の最終段は phase で分岐します。

| phase | 動作 |
|---|---|
| `ModuleRuntimePhase::shutting_down` | [`drainForTeardown()`](../../src/core/vkcore/deletionqueue.hpp#L65)。以後 `defer()` 不可 |
| それ以外(`runtime_reset`) | `flushAll()`。queue は再利用可能なまま |

したがって「module の destructor から GPU object を defer する」コードは、terminal shutdown では失敗します。destructor は既に所有している資源で完結させるか、明示 teardown 段階へ移してください。受け入れ状態の確認は `acceptingResources()`(core)/ `acceptingResourcesForTesting()`(module ラッパ)です。

## 9.13 宣言・schema はあるが、runtime が未完成または別経路のもの

ソースを読むときに「型がある = 利用可能」と誤解しやすい箇所です。

| 項目 | 調査時点の状態 | コード |
|---|---|---|
| `JsonArchiveLoader` | 実装済み。scene component/event JSON load に使用 | [`jsonarchive.cpp`](../../src/core/userpublic/serialize/jsonarchive.cpp#L6) |
| `JsonArchiveSaver` | `prop` 宣言のみで、この repository 内に定義なし | [`jsonarchive.hpp`](../../src/core/userpublic/serialize/jsonarchive.hpp#L30) |
| `BinaryArchive` | `prop` 宣言のみで、この repository 内に定義なし | [`binaryarchive.hpp`](../../src/core/userpublic/serialize/binaryarchive.hpp#L10) |
| pose action | **実装済み**(WP130/132)。pose は `poses` map から返り、未サンプルなら default `ActionPose`。flat 環境では pose サンプルが来ないので default が返る点に注意 | [`actionmap.cpp`](../../src/core/os/actionmap.cpp#L683) |
| `.surface` / material format | **runtime 接続済み**(WP116/117/122)。`.surface` は surfacecompiler で pipeline に、`.material.json` は lowering を経て `MaterialContainer` へ | [`surfacecompiler.hpp`](../../src/core/shader/surfacecompiler.hpp) / [`materiallowering.hpp`](../../src/project/materiallowering.hpp) |
| Studio project editor | Qt/QML prototype。load/save、scene editing、engine IPC は未接続。**ただしエンジン側の編集面(編集 RPC / ImGui inspector)は実装済み**なので、対比して読むこと | [`MainWindow`](../../src/devstudio/view/mainwindow.cpp#L10) |
| swapchain capture | surface が TRANSFER_SRC を持てば windowed でも readback 実装済み。不可時のみ `capture unavailable_windowed` 例外 | [`swapchainframetarget.cpp`](../../src/core/vkcore/swapchainframetarget.cpp#L435) |
| frame graph levels | 計算/JSON 出力のみ。runtime は直列 node loop | [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L574) |
| custom Component public registration | ID macro はあるが安定 public boot hook なし。ただし登録解除 API と重複拒否は入った | [`component/registerer.hpp`](../../src/core/userpublic/details/component/registerer.hpp#L20) |
| behavior attachment | ✅実装済み(WP155 / 162 / 167) | [`behaviorarena.hpp`](../../src/core/gamelogic/behaviorarena.hpp#L109) |
| 物理 trigger event | ✅実装済み(WP179) | [`PhysWorld::updateTriggers()`](../../src/core/phys/physworld.cpp#L548) |
| 編集 RPC(query / snapshot / edit / undo / preview / journal) | ✅実装済み(WP153〜172) | [`editorcommandservice.hpp`](../../src/core/communication/editorcommandservice.hpp#L221) |
| ImGui inspector / asset browser | ✅実装済み(WP159 / 164 / 167)。ただし `--rpc` / headless / replay / golden / XR では無効 | [`inspector.hpp`](../../src/core/imgui/inspector.hpp#L105) |
| preview graph(第3 variant) | 🚧実装済みだが CPU 模式ラスタ(WP172)。隔離契約が本体で、見た目の忠実度は保証しない | [`previewgraph.hpp`](../../src/core/renderingpass/previewgraph.hpp#L15) |
| RenderDoc capture | 🚧受動のみ(WP140)。**エンジンは RenderDoc をロードしない** | [`renderdoccapture.hpp`](../../src/core/renderdoc/renderdoccapture.hpp#L66) |
| VRMA decode / retarget / AnimationSource | ✅実装済み(WP176 / 177 / 178) | [`vrmadecoder.hpp`](../../src/core/loader/vrmadecoder.hpp) / [`vrmaretarget.hpp`](../../src/core/animation/vrmaretarget.hpp) |
| `.vrma` の root motion 抽出 | 📐設計スロットのみ。`VrmaRootMotionPolicy` は `preserve_hips_translation` の 1 値だけ | [`vrmaretarget.hpp#L21`](../../src/core/animation/vrmaretarget.hpp#L21) |
| authoring 側のオブジェクト宣言 identity | 🚧部分。`stage()` は「object declaration identity を後続 WP まで意図的に固定」 | [`authoringscenedocument.hpp#L110`](../../src/core/loader/authoringscenedocument.hpp#L110) |

optional build feature には stub 実装もあります。たとえば SeqPlayer/VAT/RPC/audio/physics/renderdoc は build option により実装または disabled behavior が選ばれます。header が同じでも build artifact の能力は [`build_features.hpp`](../../src/core/build_features.hpp#L1) と各 `*_stub.cpp` を確認してください。

## 9.14 症状から読む場所を決める

| 症状 | 最初の確認 | 次の確認 |
|---|---|---|
| 起動中に module constructor 例外 | module initialization log、[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L44) | constructor 内の `GET_MODULE` 依存 chain |
| 起動後の `GET_MODULE` で例外 | `freezeCreation()` 後の新規 module 生成でないか | `prepareRuntimeModules()` / prepare フェーズへの依存先解決の移動 |
| entity が突然無効 | [`EntityId` generation](../../src/core/userpublic/details/ecs/entity.hpp#L12)、scene transition | remove/clear と stale handle test |
| component pointer の値が別 entity になる | [`swap-delete`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) | pointer を structural mutation 越しに保持していないか |
| ECS system が動かない | matching component mask、force/version | explicit dependencies の cycle、empty chunk |
| ECS system が時々壊れる | 同 level の read/write conflict | `depends_list`、raw mutation、JobSystem race |
| ECS System が急に直列化された / 順序が変わった | ログの `ECS auto serialization: ...` WARNING | `read_indices` / `write_indices` の重なり、`predefined.cpp` の依存 |
| `--strict-assets` を付けた途端に起動しない | `ECS unordered component hazard between systems ...` | 明示 `depends_list` を足す |
| game system event が来ない | event macro が system macro より前に可視か | `onEvent` の完全な型 signature、event 短縮名 |
| behavior が動かない / `Unknown behavior type` | game DLL がロード済みか(`get_status.reload`) | scene の `behavior.type` と `PELICAN_REGISTER_BEHAVIOR` の stable name |
| behavior の `onEvent` が呼ばれない | event macro が behavior macro より前に可視か | 第2引数が `BehaviorContext&` か(`GameContext&` 版ではない) |
| `OverlapEnter` が 1 フレーム遅れる | `PhysicsTriggerSystem` は order `INT_MAX` | event は通常キュー経由なので配送は次フレーム |
| ライトが動かなくなった | `LightContainer::updateAnimation()` は撤去済み | `GameContext::set*Light*()` をユーザーコードで呼ぶ |
| ライトが 1 個だけ描かれない | ログの `Light cap exceeded: ...` | `MAX_*_LIGHTS` |
| ModelInstance の描画が別モデルに化ける | `ModelInstanceId{index, generation, scene_epoch}` の generation | `isModelInstanceAlive()`、scene 再ロードによる epoch 進行 |
| `save_scene` が `RuntimeOnlyData` で失敗 | `load_gltf` で transient モデルを足していないか | `SceneLoader::hasRuntimeOnlyChanges()` |
| `edit` が `stale_revision` | `get_scene_revision` の `EditorWatchToken` | preview lease が `preview_epoch` を進めていないか |
| windowed で RPC 応答が来ない | フレームが進んでいるか | queue busy 応答(`-32000` / `reason:"busy"`)が来ていないか |
| RPC event payload が空 | event に `ref(JsonArchiveLoader&)` があるか | default-only JSON loader branch |
| frame graph の順が違う | [`currentFramePlanJson()`](../../src/core/vkcore/renderer.cpp#L1148) | reads/writes、after/before、declaration index |
| XR だけ表示が壊れる | `#xr` variant の feature 除外(`xr_excluded_features`) | `graph_variant_transition_trace`、XR feature policy |
| TAA の ghosting・再投影が乱れる | temporal reset のトリガ(set_time / camera 不連続 / resize / view 数 / variant 切替) | `resetTemporalHistory()`、previous object/skin/morph buffer |
| game DLL reload 後に状態が消える/残る | `RegistrationOwner` と DLL 内 static の寿命 | engine 側 System の function-local static(こちらは残る) |
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
- flat と `#xr` の両 graph variant で成立するか。
- temporal history(history RT、object/skin/morph/override history)の reset 経路を更新したか。
- view 数変化時の FrameResources slot 再構成(device idle 待ち)を守ったか。
- plan fixture、execution trace、golden image を確認したか。

### public API/RPC を変更する

- `GameContext` から内部 module 型を漏らしていないか。
- frame boundary のどこで反映されるかを定義したか。
- error を invalid params と application error のどちらにするか決めたか。
- stdout へ protocol 外文字列を出していないか。
- 公開する関数/型に `PELICAN_API` を付けて DLL export したか。
- game DLL ABI(`gameLogicAbiVersion`)を壊していないか。
- pure parser test と actual player subprocess test の両方があるか。

## 9.16 logical frame と XR の不変条件(WP128〜135)

### logical frame の不変条件

`renderLogicalFrame()` は次を破ると例外にします([`renderer.cpp#L1322-L1337`](../../src/core/vkcore/renderer.cpp#L1322)): 全 view が同じ in-flight frame index を共有すること、全 view の extent が等しいこと、target の color format がコンパイル済み graph と一致すること。FrameUBO slot は `in_flight × view_count + view` の式で選ばれます(WP128 レポート: [`docs/design_reviews/2026-07-17_wp128_report.md`](../design_reviews/2026-07-17_wp128_report.md))。

### XR mirror は「drop 可能な optional sink」

[`try_render_begin()`](../../src/core/vkcore/frametarget.hpp#L24) が false を返すのはフレームドロップであり、描画失敗ではありません(WP133)。mirror 経路にエラー処理を足すときに、この false を error に昇格させないでください。

### XR の forced-off は決定的

headless / RPC / golden / replay では XR は決定的に off です([`xrForcedOffDriver()`](../../src/core/xractivation.hpp#L55))。`--xr on` とこれらの組み合わせはエラー方向です([`resolveXrActivation()`](../../src/core/xractivation.hpp#L89))。「headless テストで XR コードが動かない」のはこの activation 規約によるものです。

## 9.17 編集 transaction の落とし穴(prepare / publish / CAS / ticket)

WP144〜WP172 で、変更のプロトコルがコードベース全体で統一されました。この節が第9章で最も重要な追加です。

### 1. prepare は throw してよいが、publish は絶対に失敗できない

[`ECSArchetypeMigrationAdapter`](../../src/core/ecs/archetypemigration.hpp#L44) のコメントが規範です。

> prepare() may throw but must not change published state. rollback() is invoked for
> every attempted prepare (including the one that threw). Publication must not fail.

同じ形が各所にあります。

| 場所 | API |
|---|---|
| ECS archetype | [`ECSArchetypeMigration::prepareAdd/prepareRemove`](../../src/core/ecs/archetypemigration.hpp#L118) → token の `publish()` / `rollback()` / `finish()`(全て `noexcept`) |
| ECS entity | [`ECSEntityMutation::prepareCreate/prepareDestroy`](../../src/core/ecs/archetypemigration.hpp#L106) |
| ECS 既存値 | `prepareComponentValue()` / `publishComponentValue()` / `rollbackComponentValue()` |
| model instance | `preflightModelInstance()` → `stageModelInstance()` → `publishModelInstance()` |
| behavior | [`PreparedBehaviorAttachmentEdits`](../../src/core/gamelogic/behaviorarena.hpp#L85) の `publish()` / `rollback()` / `finish()`(全て `noexcept`) |
| physics | [`PhysWorld::prepareBindings()`](../../src/core/phys/physworld.hpp#L64) → [`publishPrepared()`](../../src/core/phys/physworld.hpp#L68)(`noexcept`) |
| 編集投影 | [`EditorProjectionPublicationMode{StagedNoexcept, InverseToken}`](../../src/core/loader/editorprojectiontransaction.hpp#L34) |

> **設計決定:** 新しい adapter を足すときは **prepare / rollback / publish の三点セットを必ず作ってください**。「途中まで適用された状態」を許す実装を1つ混ぜるだけで、編集・reload・scene 遷移の原子性が全体として崩れます。

`load_gltf` の単一公開点がわかりやすい実例です([`scene.cpp#L652-L654`](../../src/core/loader/scene.cpp#L652))。

```cpp
// No operation below allocates: this is the single publication point for
// the entity's slot, draw commands, resources, and optional name.
instances.publishModelInstance(std::move(staged_instance));
```

名前バインディングも `unordered_map::node_type` を先に `extract()` して確保しておき、公開時に allocation が起きないようにしています。

### 2. 投影 adapter は 8 種で閉じている

[`EditorProjectionAdapterKind`](../../src/core/loader/editorprojectiontransaction.hpp#L20) は `EcsExistingValue` / `EcsArchetype` / `TransformClosure` / `RendererModel` / `Camera` / `Light` / `PhysWorld` / `BehaviorAttachment` の 8 種です。`archetypemigration.hpp` のコメントが境界を書いています。

> This is only a composition boundary. Light, collider, behavior, and other special
> attachments remain separate adapters owned by their respective projection WPs.

つまり ECS archetype 移行は「ECS の中だけ」を見ており、light / collider / behavior は別 adapter が責任を持ちます。

### 3. CAS は `SceneRevision` で行う。ただし revision だけでは足りない

`edit` は `base_revision` を伴い、ズレていれば `EditorEditErrorCode::stale_revision` です。**watch トークンは [`EditorWatchToken{scene_revision, preview_epoch}`](../../src/core/communication/editorcommandservice.hpp#L177) の 2 要素** で、preview の open/commit も epoch を進めます。`get_scene_revision` の戻り値を丸ごと持ち回ってください。

### 4. preview は lease(ticket)

`open_preview` → `update_preview` → `commit_preview` / `abort_preview` の流れです。関連するエラーコードは [`EditorEditErrorCode`](../../src/core/communication/editorjournal.hpp#L46) の `preview_lease_conflict` / `preview_lease_busy` / `not_lease_owner` / `ticket_not_found` / `undo_conflict` です。composition root 用の緊急口として `EditorCommandService::forceAbortPreview(reason)` があります。

### 5. 編集ゲートは 5 ビット。「受理時に開いていた」は実行してよい理由にならない

[`EditorGateReason`](../../src/core/communication/editorjournal.hpp#L20) は `replay` / `golden` / `strict` / `reload_scene_transition` / `preview_lease_conflict` のビットフラグです。[`EditorGateObservation`](../../src/core/communication/editorjournal.hpp#L32) の `transition_epoch` のコメントが規範です。

> Incremented by the composition root when a reload or scene transition
> passes between acceptance and execution, even if the gate is open again.

behavior コールバック実行中 / DLL リロード中の追加ゲートは [`internal::applyBehaviorEditConcurrencyGate()`](../../src/core/communication/editorruntimefactory.hpp#L16) です。

### 6. 編集コミットはフレーム境界に 1 点だけ

[`invokeEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L124) が reload 公開の後・`freeze_events` の直前に走ります。フックは **単一所有**で、2 回目の [`installEditorCommitQueueHook()`](../../src/core/appflow/framephase.hpp#L40) は `false` を返します。未設置なら zero-state no-op で、モジュールを作りません。

### 7. 保存拒否の理由コード

[`EditorCommandErrorCode`](../../src/core/communication/editorcommandservice.hpp#L24) は 13 種です。特に注意すべきものを挙げます。

- **`RuntimeOnlyData`**: [`SceneLoader::hasRuntimeOnlyChanges()`](../../src/core/loader/scene.hpp#L54) が真のとき、つまり `load_gltf` で持ち込んだ transient モデルがあるときに出ます。「RPC で読み込んだモデルは保存できない」という意味です。
- `ExternalModification`: ディスク上の scene が外部で書き換わっていた。
- 上限は [`maxSceneSnapshotBytes = 64 MiB`](../../src/core/communication/editorcommandservice.hpp#L22)、JSON 整数の安全上限は [`maxExactEditorJsonInteger = 9007199254740991`](../../src/core/communication/editorcommandservice.hpp#L21)。

### 8. snapshot は「ファイル内容」ではなく semantic bytes

`export_scene_snapshot` の `semantic_scene_bytes` は **キー順が正規化された文字列** です。実物([`test/fixtures/editor_command_service/export_scene_snapshot_v1.json`](../../test/fixtures/editor_command_service/export_scene_snapshot_v1.json) の `expected_response`)を見ると `scenes` → `schema` → `version` の順になっています。

```json
{
  "schema_version": 1,
  "scene_revision": 42,
  "current_scene_id": "main",
  "semantic_scene_bytes": "{\"scenes\":{\"main\":{\"objects\":[]}},\"schema\":\"pelican.scene\",\"version\":1}",
  "digest": {"algorithm": "sha256", "hex": "353a3317a58437bb4c5a0d746a29afa378d1d743532ed699b8740e0ecc4bcedf"},
  "pending_ticket_ids": [],
  "preview_epoch": 7
}
```

外部ツールで digest を再計算するときは `AuthoringSceneDocument::encodeSemantic()` と同じ正規化が必要です。**ファイルを読んで sha256 を取っても一致しません。**

## 9.18 windowed RPC ホストの落とし穴

- `--rpc` は `--headless` 無しでも通るようになりましたが、**ImGui UI は無効になります**([`isImGuiRuntimeEnabled()`](../../src/core/imgui/imguiruntime.cpp#L9) が `!config.rpc` を要求)。windowed RPC と ImGui inspector は排他です。
- リクエストは **フレーム境界でしか処理されません**。windowed は毎フレーム進むので通常は問題になりませんが、ウィンドウ最小化などでフレームが止まると応答も止まります。
- queue 容量 [`64`](../../src/core/communication/rpcserver.hpp#L99) を超えると即座に `-32000` / `data.reason == "busy"` が返ります。この応答は **reader スレッドから** 出るため、engine スレッドの応答と行が混ざり得ます。順序保証は JSON-RPC の `id` に依存してください。
- reader スレッドは `std::istream` にキャンセル手段がないため、ブロックしたまま `detach()` されることがあります。テストで `WindowedRpcHost` を使い捨てるときは入力ストリームを閉じて `readerFinished()` を待ってください。本番は process 寿命の `std::cin` 前提です。

## 9.19 preview / `render_preview` の隔離

- `render_preview` は **`Renderer::renderLogicalFrame()` を通りません**([`previewgraph.hpp#L12-L14`](../../src/core/renderingpass/previewgraph.hpp#L12) のコメント)。したがって temporal history、FrameResources slot、layout tracker などのライブ状態を汚しません。
- 汚していないことの証明が [`previewStateInventory()`](../../src/core/vkcore/previewexecutor.hpp#L59)(「並び順も診断契約の一部」)と [`Renderer::previewIsolationStateJson()`](../../src/core/vkcore/renderer.cpp#L1060) です。
- 上限は 2048px / 16 MiB([previewexecutor.hpp#L54-L55](../../src/core/vkcore/previewexecutor.hpp#L54))。超過は `PreviewCaptureTooLarge` です。
- `PreviewCaptureRequest::graph_generation` が `PreviewGraphProgram::generation` と食い違えば `std::invalid_argument("preview graph generation mismatch")` で拒否されます(stale preview の防止)。
- **現在の出力は CPU 模式ラスタです**([第6章 §6.19](06_rendering_vulkan_shader.md))。material も shader も評価しないので、`render_preview` の画像を最終描画の代用と見なさないでください。

---

Pelican の複雑さは、ECS と Vulkan そのものよりも「compile-time 型情報を runtime table へ落とす境界」と「CPU 上の寿命を frame/GPU 上の寿命へ写す境界」に集まっています。その2か所では、便利な macro や RAII の表面だけでなく、登録時刻、pointer の有効期間、barrier、破棄順まで追うのが安全です。
