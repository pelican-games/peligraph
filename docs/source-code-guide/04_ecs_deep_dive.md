# 第4章 ECS徹底解剖

[索引へ戻る](README.md) / [前章](03_project_and_loading.md)

## 4.1 最初に「二つのECS」という誤解を解く

PelicanのECS関連コードは二箇所に分かれています。

- [`src/core/ecs/`](../../src/core/ecs) — moduleとしてのファサード、Componentメタデータ、組み込みComponent/System
- [`src/core/userpublic/details/ecs/`](../../src/core/userpublic/details/ecs) — Entity、Chunk、ECS実体テンプレート

これは独立した二つのWorldが動いているわけではありません。[`ECSCore`](../../src/core/ecs/core.hpp#L12) がメンバとして一個の [`ECSCoreTemplatePublic`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L40) を持ち、呼び出しを委譲します。

```text
GameObjects / SceneLoader / 内部System
              ↓
          ECSCore module
              ↓ sub
     ECSCoreTemplatePublic
       ├─ id_table
       ├─ chunks_storage
       ├─ archetype_to_chunks
       └─ systems
```

もう一つ別に存在するのは、`PELICAN_REGISTER_SYSTEM`で登録する**ゲームSystem registry**です。これはECS Chunk queryではなく`GameContext`を受け取る別機構です。第5章で扱います。

## 4.2 Component IDとComponentInfo

### compile時の型→ID

型からIDへの対応は [`ComponentIdByType<T>`](../../src/core/userpublic/details/ecs/componentdeclare.hpp#L7) のtemplate特殊化です。

```cpp
DECLARE_COMPONENT(型, 数値ID)
```

組み込みIDは [`components/predefined.hpp`](../../src/core/userpublic/components/predefined.hpp#L8) にあります。

| ID | 型 | scene名 | 用途 |
|---:|---|---|---|
| 0 | `EntityId` | `eid` | 全entityへ自動付与 |
| 1 | `TransformComponent` | `transform` | engine内部GLM transform |
| 2 | `SimpleModelViewComponent` | `simplemodelview` | model名とGPU instance ID |
| 3 | `CameraComponent` | `camera` | 内部ECS camera marker（現状dummy） |
| 16 | `LocalTransformComponent` | `localtransform` | 公開API用transform |
| 17 | `AnimationComponent` | `animation` | アニメーション再生状態 |
| 18 | `SpriteViewComponent` | `spriteview` | 2D sprite表示（登録は`registerSpriteViewComponent()`経由） |

IDは単なる外部識別子ではありません。[`ComponentInfoManager::getIndexFromComponentId()`](../../src/core/ecs/componentinfo.cpp#L20) は現在IDをそのまま`size_t`へcastします。従って、IDがそのまま次の「dense index」とmask bit位置になります。

### runtimeの型消去メタデータ

[`ComponentInfo`](../../src/core/ecs/componentinfo.hpp#L20) は次を保持します。

- `size`, `alignment`, `name`
- default construct
- destroy
- relocate（末尾swap用）
- optional `init()`
- optional `deinit() noexcept`
- optional JSON `ref(JsonArchiveLoader&)`

登録は [`UserComponentRegistererTemplatePublic::registerComponent<T>()`](../../src/core/userpublic/details/component/registerer.hpp#L34) です。template内で型付きlambdaを関数ポインタへ変換し、`ComponentInfoManager`へ型消去して渡します。

compile時制約は次です。

- default construct可能
- noexcept move construct可能
- noexcept destruct可能
- `deinit()`があるならnoexcept

これらは削除・rollback・teardownを例外なしで完遂するための契約です。

### trivial型と非trivial型のrelocate

登録コードの [`relocate` callback](../../src/core/userpublic/details/component/registerer.hpp#L53) は二分岐します。

- trivially copyable: `memcpy`
- それ以外: destinationへmove constructし、sourceをdestroy

`std::string`や`std::optional`を持つ`SimpleModelViewComponent`も安全に末尾swapできます。テストは [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L176) です。

## 4.3 EntityId: index + generation

canonical定義は [`EntityId`](../../src/core/userpublic/details/ecs/entity.hpp#L12) です。

```cpp
struct EntityId {
    uint32_t index;
    uint32_t generation;
};
```

`GameObjectId`はこのaliasです。invalid値は`index == UINT32_MAX`です。

### id table

ECS実体は [`IdEntry`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L51) のvectorを持ちます。

```text
id_table[index]
├─ ref = { chunk_index, array_index }
├─ generation
└─ live
```

[`resolve()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L36) は次を全て確認してから位置を返します。

1. indexがinvalidでない
2. indexがtable範囲内
3. entryがlive
4. generation一致
5. refあり
6. chunk/array indexが現在も範囲内

削除済みIDを保持していても、同じindexが再利用された時点でgenerationが変わるため、新しいentityへ誤接続しません。

### free list

[`releaseId()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L76) はlive/refを消し、generationを1増やしてindexを`free_indices`へ積みます。再利用はvector末尾からなのでLIFOです。これにより同じ操作列は同じindex再利用順になります（[`Free-list LIFO test`](../../test/ecs_lifecycle_test.cpp#L418)）。

generationが`UINT32_MAX`に達したindexはwrapさせず、永久retireしてfree listへ戻しません。

## 4.4 ArchetypeとChunk

### Archetype

同じComponent ID集合を持つentity群が同じarchetypeです。mapは次の形です。

```cpp
unordered_map<vector<ComponentId>, vector<ChunkIndex>, VectorHash>
```

key作成時はComponent IDをsortするため、呼び出し側で指定した順序が違っても同じarchetypeとして見つかります（[`createEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L104)）。一方、Chunk内部の配列順序は作成時のmetadata順を保持します。

### ChunkはSoA

[`ECSComponentChunk`](../../src/core/userpublic/details/ecs/chunk.hpp#L15) はComponentごとの配列を別々に持ちます。

```text
Chunk: [EntityId, Transform, ModelView]

EntityId[]  : E0 E1 E2 ...
Transform[] : T0 T1 T2 ...
ModelView[] : M0 M1 M2 ...
```

entityごとにstructを並べるAoSではなく、Systemが同じComponentを連続走査しやすいSoAです。

### 固定capacity 4096

一Chunkの上限は [`CHUNK_CAPACITY = 4096`](../../src/core/userpublic/details/ecs/chunk.hpp#L52) です。各Component配列の [`VariedArray`](../../src/core/userpublic/details/ecs/chunk.hpp#L17) はChunk構築時に `stride * 4096` byteをalignment付き`operator new`で一度確保します（[`VariedArray` constructor](../../src/core/userpublic/details/ecs/chunk.cpp#L18)）。

この方式には次の性質があります。

- entity追加で既存配列がreallocateされない。
- Componentごとの先頭pointerをSystemへそのまま渡せる。
- 使っていないcapacity分も最初からメモリを確保する。
- archetypeが多く、Componentが大きい場合は空きメモリが増える。

### mask

Chunkは64 bitのComponent maskを持ちます。Component indexごとに`1ULL << index`を立て、Systemのrequired maskを包含するかでmatchingします（[`ECSComponentChunk` constructor](../../src/core/userpublic/details/ecs/chunk.cpp#L95)）。

制約は最大64 indexです。自動追加される`EntityId`を含めてmaskに収まる必要があります。現在indexはIDそのものなので、登録Component IDも0〜63に収める必要があります。

## 4.5 Entityの一括生成はtransaction

最重要実装は [`ECSCoreTemplatePublic::createEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L93) です。

### 正常経路

1. [`MutationScope`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L25) で構造変更の再入を禁止。
2. 呼び出し側Component列の先頭へ`EntityId`を自動追加。
3. sort済みcopyで重複IDを検出。
4. 全IDが登録済みか`ComponentInfoManager`で確認。
5. free list再利用数と新規ID数を事前計算し、capacity overflowを確認。
6. 同archetypeで空きのあるChunkを探し、なければ新Chunkを作る。
7. 最大4096件ずつChunk配列をdefault constructして領域を確保。
8. IDを割り当てるが、まだ`live = false`のまま。
9. 呼び出し側`populate` callbackで初期値を代入。
10. 各Componentのoptional `init()`をentity順・Component順に呼ぶ。
11. 全て成功後にIDを`live = true`へ公開。
12. 全Component versionを現在tickへ更新。

ポイントは、populate/init中のentityが外部からliveとして観測されないことです。

### rollback経路

populateまたはinitがthrowすると [`catch` block](../../src/core/userpublic/details/ecs/coretemplate.cpp#L247) が次を逆順に戻します。

1. 成功済みinitに対応する`deinit()`を逆順実行。
2. 各Chunkの追加末尾をdestroy。
3. 今回新設したChunkをerase。
4. `id_table`と`free_indices`を生成前copyへ戻す。
5. archetype mapとSystem matching cacheを再構築。
6. 元の例外を再throw。

強い例外保証を優先して、生成前のID table/free list全体をcopyしています。大量entity・巨大ID tableで例外可能なComponentを作る場合、このcopy costも性能評価対象です。

### 構造変更の再入禁止

`init()`内から`createEntity()`を呼ぶと`mutation_active`で`logic_error`になります。中途半端なtransactionへ別transactionを重ねないためです。テストは [`Structural callback reentry`](../../test/ecs_lifecycle_test.cpp#L321) です。

## 4.6 GameObjects builderのtemplate黒魔術

公開側の典型コードは次です。

```cpp
GameObjects::add()
    .addComponent<TransformComponent>()
    .addComponent<LocalTransformComponent>(transform)
    .addComponent<SimpleModelViewComponent>(model_view)
    .finish();
```

実装は [`gameobjects.hpp`](../../src/core/userpublic/gameobjects.hpp#L19) です。

### compile時に蓄積しているもの

- `ComponentIdHolder<...>`: 追加した全ComponentのID列
- `IndexHolder<...>`: 初期値を渡したComponentだけの位置列
- `ComponentDataTuple`: 初期値へのreference tuple

型なし`addComponent<T>()`はIDだけを追加します。値あり版はIDに加え、「全Component列の何番目へこの値をcopyするか」と値referenceを蓄積します。

### `finish()`で実行時へ橋渡し

[`finish()`](../../src/core/userpublic/gameobjects.hpp#L79)（[2オーバーロード](../../src/core/userpublic/gameobjects.hpp#L100)）はcompile時ID packを`span`にし、ECSのpopulate callbackを作ります。callback内のfold expressionが、保存した位置indexを使って`void*`配列を正しい`T*`へcastし、値をcopy assignmentします（[`GameObjects::copy()`](../../src/core/userpublic/gameobjects.hpp#L53)）。

要するに、流暢なbuilder APIの各段階で型が変わり、最後にだけruntimeの`vector<ComponentId> + void*`世界へ落としています。

### 寿命上の注意

値tupleは`const T&`を保持します。通常の一続きの式では`finish()`までtemporaryが生存しますが、builder contextを長期保存したり、参照元を先に破棄したりしないでください。意図された使い方は一式のchainです。

## 4.7 削除は末尾swap

[`ECSCoreTemplatePublic::remove()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L278) はO(Component数)で削除します。

```text
削除前: [A, B, C, D]
               ↑ Bを削除

各Component配列で:
1. B.deinit()
2. B.destroy()
3. DをBの位置へrelocate
4. Dの旧位置をdestroy

削除後: [A, D, C]
```

実際の配列処理は [`VariedArray::removeAt()`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) です。ECS側は削除前に末尾の`EntityId`を読み、移動したDの`id_table.ref.array_index`をBの旧位置へ更新します。

最後に削除対象IDをreleaseしてgenerationを進めます。

### 生ポインタ保持が危険な理由

Chunk自体は固定capacityですが、別entityを削除すると末尾entityが移動します。Componentへのpointer/referenceをフレームを跨いで保存すると、同じアドレスが別entityを指す可能性があります。長期保持するのは`EntityId`で、必要時に`tryComponent()`し直すのが契約です。

## 4.8 clearとteardown

[`clearEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L303) は全Chunkをclearし、Chunk/archetypeを破棄し、Systemのmatching cacheと`last_run_tick`をresetします。全live IDをreleaseするので、clear前のIDはgeneration不一致になります。

Chunk clearはComponent indexの逆順で各要素を`deinit → destroy`します（[`ECSComponentChunk::clear()`](../../src/core/userpublic/details/ecs/chunk.cpp#L179)）。GPU instanceを持つ`SimpleModelViewComponent::deinit()`は [`PolygonInstanceContainer::removeModelInstance()`](../../src/core/ecs/predefined/modelview.cpp#L8) を呼びます。そのため第2章の明示teardown順が重要です。

## 4.9 Componentアクセスと変更version

- [`tryComponent<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L102): stale/Componentなしなら`nullptr`
- [`component<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L106): なければ詳細付き例外
- [`setComponent<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L111): copy assignment後にversion更新
- [`markComponentChanged()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L365): pointer経由で直接変更した場合の手動通知

ChunkはComponent indexごとに`component_versions[index]`を一個持ちます。entityごとのversionではなく、**Chunk内のそのComponent列全体のversion**です。どれか一entityを書き換えると、そのChunkを読むSystemはChunk全体を再処理します。

直接pointerを書き換えて`markComponentChanged()`を忘れると、非force Systemは変更を見逃します。公開 [`GameObjects::setLocalTransform()`](../../src/core/userpublic/gameobjects.cpp#L38) はLocalTransformと内部Transformの両方を更新・markし、描画instanceへも即時反映します。

## 4.10 内部ECS Systemの登録

template本体は [`registerSystem<TSystem, TComponents...>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L166) です。現在のsignatureは `registerSystem(TSystem &system, std::vector<SystemId> &&depends_list, bool force_update)` で、**Systemのinstance参照を受け取り`SystemId`を返します**。ECSCore側の入口は [`registerSystemForce()`](../../src/core/ecs/core.hpp#L35) です。返る`SystemId`は後続Systemの依存指定に使えます。

### 対応process形

二つのConceptでduck typingします。

```cpp
// 全matching chunkをまとめて一回
void process(std::span<ChunkView<TComponents...>> chunks);

// chunkごとに一回
void process(std::tuple<TComponents*...> arrays, size_t count);
```

判定は [`HasBatchProcess`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L30) と [`HasPerChunkProcess`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L35) です。登録時の [`static_assert`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L167) で「少なくともどちらか」のprocess形が必須になりました。

実装は二つの独立した`if constexpr`なので、両方のsignatureを同時に実装すると両方呼ばれます。通常はどちらか一方だけを実装します。

### `const`でread/writeを宣言

Component packの型が次を同時に表します。

- `const T`: read-only
- `T`: write

登録時にpointer型を組み立て、constを外してComponent IDを求めつつ、read/write indexへ分類します（[`process_component`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L160)）。System実装へ渡るtupleは宣言どおり`const T*`または`T*`になります。

### matching cache

required Component maskを作り、既存Chunkのmaskが包含すれば`matching_chunk_indices`へ追加します。新Chunk生成時も [`updateSystemChunkCache()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L51) が全Systemへ照合します。

Chunkが空になっても個別削除ではChunk自体を消さないため、matching cacheに空Chunkが残る可能性があります。Systemは`count == 0`も安全に扱う必要があります。

## 4.11 変更検知

System wrapperは`last_run_tick`を持ちます。ECS全体の [`global_tick`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L145) は`update()`冒頭で1増えます。

per-chunk型では、required Component列の最大versionが`last_run_tick`未満ならskipします。実行後、write宣言した列のversionを現在tickへ更新します。

batch型では、全matching Chunkのどれかに変更があれば一回だけ全viewを渡します。実行後は全matching Chunkのwrite列を更新します。

`force_update`ならversionに関係なく毎回実行します。現在の組み込みSystemは全て [`registerSystemForce`](../../src/core/ecs/predefined.cpp#L32) で登録されています。

## 4.12 依存グラフと並列実行

[`ECSCoreTemplatePublic::update()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L386) はSystemの明示`depends_list`からトポロジカルlevelを作ります。

```text
Level 0: A  B  C  ── workerへ並列schedule ── wait
Level 1: D  E     ── workerへ並列schedule ── wait
Level 2: F        ── workerへschedule      ── wait
```

同levelのSystemを [`JobSystem`](../../src/core/job_system.cpp#L31) へscheduleし、level末尾で全完了を待ちます。worker例外はmain threadへ再throwされます。

### prepareフェーズ

level実行前に、各Systemの`prepare_func`がowner threadで呼ばれます（[coretemplate.cpp](../../src/core/userpublic/details/ecs/coretemplate.cpp#L424)）。Systemは`prepareEcsWorkerDependencies()`を実装して`GET_MODULE`をowner thread上で済ませ、worker job中のmodule生成（freeze後はエラー）を避けます。例は [`CameraSystem::prepareEcsWorkerDependencies()`](../../src/core/ecs/predefined/camerasystem.cpp#L9) です。

### 現在の重要な制約

1. **read/writeから依存を自動導出しません。** `const`分類は変更version更新に使われますが、data race回避のedgeは自動追加されません。
2. 同じComponentへ書くSystem、またはwrite/readするSystemは、呼び出し側が明示依存を設定しなければ同levelで並列実行され得ます。
3. 同levelの順序は`unordered_map`列挙とworker schedulingに依存します。順序が必要ならdependencyが必要です。
4. 現実装はトポロジカル処理件数が全System数と一致するかを最後に検査していません。循環依存のSystemは実行levelへ入らず、明示的なcycle errorになりません。

現在の組み込みSystem登録には明示依存が張られています（[predefined.cpp](../../src/core/ecs/predefined.cpp#L32)）: `AnimationSystem` ← model_update、`SimpleModelViewTransformSystem` ← {local_transform, model_update}、`CameraSystem` ← local_transform、`SpriteViewRenderSystem` ← local_transform。全てforce updateです。Component集合は重なるため、内部Systemを追加する場合は依存とread/write競合を明示的に監査する必要があります。

## 4.13 組み込みSystem

[`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L20) が登録します。

| System | Query | 処理 |
|---|---|---|
| [`LocalTransformSystem`](../../src/core/ecs/predefined/localtransformsystem.cpp#L7) | `EntityId, Transform, LocalTransform` | 公開transformをGLM内部transformへcopy |
| [`SimpleModelViewUpdateSystem`](../../src/core/ecs/predefined/modelviewupdatesystem.cpp#L23) | `SimpleModelView` | dirtyなmodel名からGPU instanceを生成 |
| [`AnimationSystem`](../../src/core/ecs/predefined/animationsystem.cpp) | `Animation, SimpleModelView` | アニメーション再生状態をmodel instanceへ反映 |
| [`SimpleModelViewTransformSystem`](../../src/core/ecs/predefined/modelviewtransformsystem.cpp#L7) | `Transform, SimpleModelView` | GPU instanceへTRS反映 |
| [`CameraSystem`](../../src/core/ecs/predefined/camerasystem.cpp#L9) | `Transform, Camera` | 最初のcamera Componentをactive Cameraへ反映 |
| [`SpriteViewRenderSystem`](../../src/core/ecs/predefined/spriteviewsystem.cpp) | `EntityId, Transform, SpriteView` | sprite表示をSpriteSceneへ反映 |

`CameraSystem`は`count == 0`で早期returnするようになりました（[camerasystem.cpp](../../src/core/ecs/predefined/camerasystem.cpp#L14)）。ただし「先頭1件のみ使用」（`i < 1`固定のループ）は変わりません。scene cameraの主経路は別の [`Camera::loadSceneCameras()`](../../src/core/renderer/camera.cpp#L505) でも管理されており、camera関連には旧内部ECS経路と新scene camera経路が併存しています。

## 4.14 Colliderは現在ECS Componentとして保存されない

[`ColliderComponent`](../../src/core/userpublic/components/collider.hpp#L13) という名前ですが、組み込みComponent ID宣言・ECS登録には含まれません。scene loaderが`name == "collider"`を特別扱いし、[`PhysWorld`](../../src/core/loader/scene.cpp#L92) へbindingとして保存します。

従って、現在の意味で「Component structである」ことと「ECS Chunk内に格納される」ことは同義ではありません。

## 4.15 ユーザー独自Componentの現在地

`DECLARE_COMPONENT`と型付きregistererは実装されていますが、通常のゲームコード向けに安定した自動Component登録マクロはまだ公開されていません（`PELICAN_REGISTER_COMPONENT`はリポジトリ全体に不在）。実際の登録箇所は組み込み [`predefined.cpp`](../../src/core/ecs/predefined.cpp#L20)、benchmark、lifecycle testです。

なお、game System/Eventの登録はDLL化後、[`RegistrationOwner`](../../src/core/userpublic/details/system/registerer.hpp#L31) 単位でreload時に`unregisterGameSystems(owner)`されるようになりましたが、Component登録は依然としてengine側のみです。

そのため、現状の公開`GameObjects` builderで安全に使えるのは、エンジンがID宣言とruntime登録を済ませた型が中心です。独自Component対応を製品機能として追加するなら、次を一体で設計する必要があります。

- ID衝突管理
- translation unitを跨ぐ自動登録
- scene名との対応
- init/deinit/JSON契約
- 配布ヘッダの範囲
- 登録完了前のentity生成を防ぐ初期化順

## 4.16 ECSを変更するときの不変条件

1. 全live `EntityId`は、Chunk内の`EntityId`列と`id_table.ref`の双方で一致する。
2. stale IDは新entityを絶対にresolveしない。
3. 各Component配列の`count`はChunkの`count`と一致する。
4. 同Chunk内の全Component配列は同じentity順序を持つ。
5. `init()`成功済みComponentは、失敗・削除・clearの全経路で一度だけ`deinit()`される。
6. relocation後にsource objectはdestroy済み、destinationだけがlive。
7. 構造変更中のnested構造変更を許さない。
8. pointerを長期IDとして扱わない。
9. raw write後はversionをmarkする。
10. 並列Systemの競合は明示dependencyで防ぐ。

主要な実行可能仕様は [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L160) に集中しています。ECS改修時はこのファイルを先に読むのが最短です。
