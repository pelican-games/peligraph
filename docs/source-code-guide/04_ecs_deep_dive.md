# 第4章 ECS徹底解剖

[索引へ戻る](README.md) / [前章](03_project_and_loading.md)

## 4.1 最初に「二つのECS」という誤解を解く

PelicanのECS関連コードは二箇所に分かれています。

- [`src/core/ecs/`](../../src/core/ecs) — moduleとしてのファサード、Componentメタデータ、組み込みComponent/System
- [`src/core/userpublic/details/ecs/`](../../src/core/userpublic/details/ecs) — Entity、Chunk、ECS実体テンプレート

これは独立した二つのWorldが動いているわけではありません。[`ECSCore`](../../src/core/ecs/core.hpp#L12) がメンバとして一個の [`ECSCoreTemplatePublic`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L97) を持ち、呼び出しを委譲します。

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

組み込みIDは [`components/predefined.hpp`](../../src/core/userpublic/components/predefined.hpp#L12) にあります。

| ID | 型 | scene名 | 用途 |
|---:|---|---|---|
| 0 | `EntityId` | `eid` | 全entityへ自動付与 |
| 1 | `TransformComponent` | `transform` | engine内部GLM transform |
| 2 | `SimpleModelViewComponent` | `simplemodelview` | model名とGPU instance ID |
| 3 | `CameraComponent` | `camera` | 内部ECS camera marker（現状dummy） |
| 16 | `LocalTransformComponent` | `localtransform` | 公開API用transform |
| 17 | `AnimationComponent` | `animation` | アニメーション再生状態 |
| 18 | `SpriteViewComponent` | `sprite_view` | 2D sprite表示（登録は [`registerSpriteViewComponent()`](../../src/core/userpublic/components/spriteview.cpp#L108) 経由） |

IDは単なる外部識別子ではありません。[`ComponentInfoManager::getIndexFromComponentId()`](../../src/core/ecs/componentinfo.cpp#L86) は現在IDをそのまま`size_t`へcastします。従って、IDがそのまま次の「dense index」とmask bit位置になります。

ただし**未登録スロットの読み出しは例外になります**。実体は [`getFromIndex()`](../../src/core/ecs/componentinfo.cpp#L89) で、`token` が未設定なら `std::out_of_range("component index N is not registered")` を投げます。「IDがそのままindex」という値の関係は保ちつつ、穴の空いたスロットを黙って読むことはできません。

### runtimeの型消去メタデータ

[`ComponentInfo`](../../src/core/ecs/componentinfo.hpp#L20) は次を保持します。

- `size`, `alignment`, `name`
- default construct
- destroy
- relocate（末尾swap用）
- optional `init()`
- optional `deinit() noexcept`
- optional JSON `ref(JsonArchiveLoader&)`
- [`internal::RegistrationOwner owner`](../../src/core/ecs/componentinfo.hpp#L33) と [`internal::RegistrationToken token`](../../src/core/ecs/componentinfo.hpp#L34)（登録解除のための識別、§4.15）

登録は [`UserComponentRegistererTemplatePublic::registerComponent<T>()`](../../src/core/userpublic/details/component/registerer.hpp#L36) です。template内で型付きlambdaを関数ポインタへ変換し、`ComponentInfoManager`へ型消去して渡します。戻り値は `void` ではなく `RegistrationToken` です。

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

`std::string`や`std::optional`を持つ`SimpleModelViewComponent`も安全に末尾swapできます。テストは [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L207) です。

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

ECS実体は [`IdEntry`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L113) のvectorを持ちます。

```text
id_table[index]
├─ ref = { chunk_index, array_index }
├─ generation
└─ live
```

[`resolve()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L308) は次を全て確認してから位置を返します。

1. indexがinvalidでない
2. indexがtable範囲内
3. entryがlive
4. generation一致
5. refあり
6. chunk/array indexが現在も範囲内

削除済みIDを保持していても、同じindexが再利用された時点でgenerationが変わるため、新しいentityへ誤接続しません。

### free list

[`releaseId()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L348) はlive/refを消し、generationを1増やしてindexを`free_indices`へ積みます。再利用はvector末尾からなのでLIFOです。これにより同じ操作列は同じindex再利用順になります（[`Free-list LIFO test`](../../test/ecs_lifecycle_test.cpp#L449)）。

generationが`UINT32_MAX`に達したindexはwrapさせず、永久retireしてfree listへ戻しません。

## 4.4 ArchetypeとChunk

### Archetype

同じComponent ID集合を持つentity群が同じarchetypeです。mapは次の形です。

```cpp
unordered_map<vector<ComponentId>, vector<ChunkIndex>, VectorHash>
```

key作成時はComponent IDをsortするため、呼び出し側で指定した順序が違っても同じarchetypeとして見つかります（[`createEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L365)）。一方、Chunk内部の配列順序は作成時のmetadata順を保持します。

### ChunkはSoA

[`ECSComponentChunk`](../../src/core/userpublic/details/ecs/chunk.hpp#L19) はComponentごとの配列を別々に持ちます。

```text
Chunk: [EntityId, Transform, ModelView]

EntityId[]  : E0 E1 E2 ...
Transform[] : T0 T1 T2 ...
ModelView[] : M0 M1 M2 ...
```

entityごとにstructを並べるAoSではなく、Systemが同じComponentを連続走査しやすいSoAです。

### 固定capacity 4096

一Chunkの上限は [`CHUNK_CAPACITY = 4096`](../../src/core/userpublic/details/ecs/chunk.hpp#L69) です。各Component配列の [`VariedArray`](../../src/core/userpublic/details/ecs/chunk.hpp#L25) はChunk構築時に `stride * 4096` byteをalignment付き`operator new`で一度確保します（[`VariedArray` constructor](../../src/core/userpublic/details/ecs/chunk.cpp#L18)）。

この方式には次の性質があります。

- entity追加で既存配列がreallocateされない。
- Componentごとの先頭pointerをSystemへそのまま渡せる。
- 使っていないcapacity分も最初からメモリを確保する。
- archetypeが多く、Componentが大きい場合は空きメモリが増える。

### mask

Chunkは64 bitのComponent maskを持ちます。Component indexごとに`1ULL << index`を立て、Systemのrequired maskを包含するかでmatchingします（[`ECSComponentChunk` constructor](../../src/core/userpublic/details/ecs/chunk.cpp#L95)）。

制約は最大64 indexです。自動追加される`EntityId`を含めてmaskに収まる必要があります。現在indexはIDそのものなので、登録Component IDも0〜63に収める必要があります。

## 4.5 Entityの一括生成はtransaction

最重要実装は [`ECSCoreTemplatePublic::createEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L365) です。

### 正常経路

1. [`MutationScope`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L297) で構造変更の再入を禁止。
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

populateまたはinitがthrowすると [`catch` block](../../src/core/userpublic/details/ecs/coretemplate.cpp#L519) が次を逆順に戻します。

1. 成功済みinitに対応する`deinit()`を逆順実行。
2. 各Chunkの追加末尾をdestroy。
3. 今回新設したChunkをerase。
4. `id_table`と`free_indices`を生成前copyへ戻す。
5. archetype mapとSystem matching cacheを再構築。
6. 元の例外を再throw。

強い例外保証を優先して、生成前のID table/free list全体をcopyしています。大量entity・巨大ID tableで例外可能なComponentを作る場合、このcopy costも性能評価対象です。

### 構造変更の再入禁止

`init()`内から`createEntity()`を呼ぶと`mutation_active`で`logic_error`になります。中途半端なtransactionへ別transactionを重ねないためです。テストは [`Structural callback reentry`](../../test/ecs_lifecycle_test.cpp#L352) です。

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

[`ECSCoreTemplatePublic::remove()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L550) はO(Component数)で削除します。

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

[`clearEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L575) は全Chunkをclearし、Chunk/archetypeを破棄し、Systemのmatching cacheと`last_run_tick`をresetします。全live IDをreleaseするので、clear前のIDはgeneration不一致になります。

Chunk clearはComponent indexの逆順で各要素を`deinit → destroy`します（[`ECSComponentChunk::clear()`](../../src/core/userpublic/details/ecs/chunk.cpp#L198)）。GPU instanceを持つ`SimpleModelViewComponent::deinit()`は [`PolygonInstanceContainer::removeModelInstance()`](../../src/core/ecs/predefined/modelview.cpp#L11) を呼びます。そのため第2章の明示teardown順が重要です。

## 4.9 Componentアクセスと変更version

- [`tryComponent<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L343): stale/Componentなしなら`nullptr`
- [`component<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L347): なければ詳細付き例外
- [`setComponent<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L353): copy assignment後にversion更新
- [`markComponentChanged()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L637): pointer経由で直接変更した場合の手動通知

ChunkはComponent indexごとに`component_versions[index]`を一個持ちます。entityごとのversionではなく、**Chunk内のそのComponent列全体のversion**です。どれか一entityを書き換えると、そのChunkを読むSystemはChunk全体を再処理します。

直接pointerを書き換えて`markComponentChanged()`を忘れると、非force Systemは変更を見逃します。公開 [`GameObjects::setLocalTransform()`](../../src/core/userpublic/gameobjects.cpp#L55) はLocalTransformと内部Transformの両方を更新・markし、描画instanceへも即時反映します。

## 4.10 内部ECS Systemの登録

template本体は [`registerSystem<TSystem, TComponents...>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L398) です。現在のsignatureは `registerSystem(TSystem &system, std::vector<SystemId> &&depends_list, bool force_update)` で、**Systemのinstance参照を受け取り`SystemId`を返します**。ECSCore側の入口は [`registerSystemForce()`](../../src/core/ecs/core.hpp#L45) です。返る`SystemId`は後続Systemの依存指定に使えます。

### 登録の副作用

`SystemId` を返すだけではありません。

1. wrapperへ **`std::string name`** を持たせ、`wrapper.name = typeid(TSystem).name();` を代入します（[coretemplate.hpp#L429](../../src/core/userpublic/details/ecs/coretemplate.hpp#L429)）。この名前が§4.12のhazard/cycleエラー文言に出ます。
2. [`internal::registerECSSystemComponentDependencies()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L27) を呼び、Componentからの**逆参照テーブル**へ登録します（[coretemplate.hpp#L565](../../src/core/userpublic/details/ecs/coretemplate.hpp#L565)）。これがComponent登録解除時の「依存Systemが残っている」判定に使われます（§4.15）。
3. `execution_plan_dirty = true` を立て、実行計画キャッシュを無効化します（[coretemplate.hpp#L556](../../src/core/userpublic/details/ecs/coretemplate.hpp#L556)）。

対称に、[`unregisterSystem()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L651) は逆参照テーブルから外して `execution_plan_dirty = true` を立て、デストラクタ [`~ECSCoreTemplatePublic()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L293) が `internal::unregisterECSCoreComponentDependencies(this)` を呼びます。

### 対応process形

二つのConceptでduck typingします。

```cpp
// 全matching chunkをまとめて一回
void process(std::span<ChunkView<TComponents...>> chunks);

// chunkごとに一回
void process(std::tuple<TComponents*...> arrays, size_t count);
```

判定は [`HasBatchProcess`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L77) と [`HasPerChunkProcess`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L82) です。登録時の [`static_assert`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L399) で「少なくともどちらか」のprocess形が必須になりました。

実装は二つの独立した`if constexpr`なので、両方のsignatureを同時に実装すると両方呼ばれます。通常はどちらか一方だけを実装します。

### `const`でread/writeを宣言

Component packの型が次を同時に表します。

- `const T`: read-only
- `T`: write

登録時にpointer型を組み立て、constを外してComponent IDを求めつつ、read/write indexへ分類します（[`process_component`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L410)）。System実装へ渡るtupleは宣言どおり`const T*`または`T*`になります。

### matching cache

required Component maskを作り、既存Chunkのmaskが包含すれば`matching_chunk_indices`へ追加します。新Chunk生成時も [`updateSystemChunkCache()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L323) が全Systemへ照合します。

Chunkが空になっても個別削除ではChunk自体を消さないため、matching cacheに空Chunkが残る可能性があります。Systemは`count == 0`も安全に扱う必要があります。

## 4.11 変更検知

System wrapperは`last_run_tick`を持ちます。ECS全体の [`global_tick`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L391) は`update()`冒頭で1増えます。

per-chunk型では、required Component列の最大versionが`last_run_tick`未満ならskipします。実行後、write宣言した列のversionを現在tickへ更新します。

batch型では、全matching Chunkのどれかに変更があれば一回だけ全viewを渡します。実行後は全matching Chunkのwrite列を更新します。

`force_update`ならversionに関係なく毎回実行します。現在の組み込みSystemは全て [`registerSystemForce`](../../src/core/ecs/predefined.cpp#L32) で登録されています。

## 4.12 依存グラフと並列実行 ✅実装済み

WP148（ECS0）でスケジューラが書き換わりました。実行計画を作るのは [`internal::buildECSExecutionPlan(nodes, hazard_policy)`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L206) で、[`ECSCoreTemplatePublic::update()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L660) がそれを呼びます。

```text
Level 0: A  B  C  ── workerへ並列schedule ── wait
Level 1: D  E     ── workerへ並列schedule ── wait
Level 2: F        ── workerへschedule      ── wait
```

同levelのSystemを [`JobSystem`](../../src/core/job_system.cpp#L31) へscheduleし、level末尾で全完了を待ちます。worker例外はmain threadへ再throwされます。`JobSystem`自体は変わっていません。

### read/writeからhazardを検出する

[`conflictingComponents()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L170) が、二つのSystemが**同じcomponent indexへアクセスし、片方でもwriteしている**組み合わせを競合とみなします。ただしこれは*依存edgeの自動導出*ではなく、*hazardの検出*です。既に依存関係で順序が付いている組（[`isOrderedBefore()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L151) が到達可能性で判定）は競合になりません。

検出されたhazardの扱いはpolicy次第です。

### hazard policyは2種

[`internal::ECSHazardPolicy`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L35) は次の二つです。

| policy | 挙動 |
|---|---|
| `automatic_serialization`（既定） | 登録IDの小さい方を先に実行する暗黙edgeを足し、**WARNINGログを出す** |
| `strict` | 全hazardを集めて1本の例外にする |

`automatic_serialization` のログ文言は次です（[coretemplate.cpp#L717](../../src/core/userpublic/details/ecs/coretemplate.cpp#L717)）。

```text
ECS auto serialization: '{}' before '{}' for component(s) {}; add an explicit dependency edge (strict mode rejects this hazard)
```

`strict` の例外はhazardを `; ` で連結し、末尾に `; add dependency edges or use automatic serialization` が付きます（[coretemplate.cpp#L276-L285](../../src/core/userpublic/details/ecs/coretemplate.cpp#L276)）。

### policyの切替は `--strict-assets`

専用フラグはありません（[coretemplate.cpp#L664-L671](../../src/core/userpublic/details/ecs/coretemplate.cpp#L664)）。

```cpp
const auto hazard_policy = [] {
    const auto *launch_config = FastModuleContainer::tryGet<EngineLaunchConfig>();
    // --strict-assets is the existing startup-wide strict/determinism gate.
    // Reusing it keeps WP148 inside the scheduler-only change boundary.
    return launch_config != nullptr && launch_config->strict_assets
               ? internal::ECSHazardPolicy::strict
               : internal::ECSHazardPolicy::automatic_serialization;
}();
```

> **設計決定:** 既存の起動時strict/決定性ゲートである `--strict-assets` を再利用しています。コメントが理由を明記している通り、WP148の変更範囲をスケジューラの中に閉じるための選択です。CIやgolden testはstrictで走るため、hazardは開発時にwarning、検証時にerrorとして現れます。

### グラフの妥当性検査

以下はすべてエラーです。

| 状況 | 例外メッセージ |
|---|---|
| 循環依存 | `ECS dependency cycle detected; unexecuted systems: '<name>' ...`（[coretemplate.cpp#L138-L147](../../src/core/userpublic/details/ecs/coretemplate.cpp#L138)） |
| 存在しないSystemへの依存 | `... depends on missing system id N; system would be unexecuted`（[#L229](../../src/core/userpublic/details/ecs/coretemplate.cpp#L229)） |
| 同じSystemへの重複依存 | `... declares dependency on system '<name>' more than once; system would be unexecuted`（[#L237](../../src/core/userpublic/details/ecs/coretemplate.cpp#L237)） |
| System IDの重複 | `ECS execution graph contains duplicate system id N`（[#L218](../../src/core/userpublic/details/ecs/coretemplate.cpp#L218)） |

循環依存は「実行levelへ入らないまま黙って無視される」のではなく、[`makeExecutionLevels()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L97) が処理件数と全System数の一致を検査して例外にします。

### 実行計画のキャッシュ

計画は `execution_levels` にキャッシュされ、`execution_plan_dirty`（System登録/解除で立つ、§4.10）かpolicyが変わったときだけ再構築します（[coretemplate.cpp#L673](../../src/core/userpublic/details/ecs/coretemplate.cpp#L673)）。

### prepareフェーズ

level実行前に、そのlevelの**全System**の`prepare_func`がowner threadで呼ばれ、その後にまとめて`JobSystem`へscheduleし、`wait()`します（[coretemplate.cpp#L732-L744](../../src/core/userpublic/details/ecs/coretemplate.cpp#L732)）。Systemは`prepareEcsWorkerDependencies()`を実装して`GET_MODULE`をowner thread上で済ませ、worker job中のmodule生成（freeze後はエラー）を避けます。例は [`CameraSystem::prepareEcsWorkerDependencies()`](../../src/core/ecs/predefined/camerasystem.cpp#L9) です。

### 組み込みSystemの依存

現在の組み込み登録はほぼ全順序まで強化されています（[predefined.cpp#L31-L47](../../src/core/ecs/predefined.cpp#L31)）。

| System | 依存 |
|---|---|
| `LocalTransformSystem` | なし |
| `SimpleModelViewUpdateSystem` | なし |
| `AnimationSystem` | `{model_update}` |
| `SimpleModelViewTransformSystem` | `{local_transform, model_update, animation}` |
| `CameraSystem` | `{local_transform, model_transform}` |
| `SpriteViewRenderSystem` | `{local_transform, camera}` |

全て `registerSystemForce` です。Component集合は重なるため、内部Systemを追加する場合は依存とread/write競合を明示的に監査してください。監査を忘れても、`automatic_serialization` ならWARNINGで、`strict` ならエラーで気付けます。

テストは [`test/ecs_scheduler_test.cpp`](../../test/ecs_scheduler_test.cpp) です。

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

`CameraSystem`は`count == 0`で早期returnするようになりました（[camerasystem.cpp](../../src/core/ecs/predefined/camerasystem.cpp#L14)）。ただし「先頭1件のみ使用」（`i < 1`固定のループ）は変わりません。scene cameraの主経路は別の [`Camera::loadSceneCameras()`](../../src/core/renderer/camera.cpp#L645) でも管理されており、camera関連には旧内部ECS経路と新scene camera経路が併存しています。

## 4.14 ECS Componentとして保存されないもの

「scene JSONのcomponent配列に書ける」ことと「ECS Chunk内に格納される」ことは同義ではありません。現在、次の三つがECS外の所有者へ行きます。

| scene component名 | 実際の所有者 |
|---|---|
| `light` | [`LightContainer`](../../src/core/light/lightcontainer.hpp#L17) |
| `collider` | [`PhysWorld`](../../src/core/phys/physworld.hpp#L33) |
| `behavior` | [`BehaviorAttachmentArena`](../../src/core/gamelogic/behaviorarena.hpp#L109) |

### collider

[`ColliderComponent`](../../src/core/userpublic/components/collider.hpp#L13) という名前ですが、組み込みComponent ID宣言・ECS登録には含まれません。scene loaderが`name == "collider"`を特別扱いし（[scene.cpp#L151](../../src/core/loader/scene.cpp#L151)）、`phys_world.bindCollider()`（[同 #L398-L402](../../src/core/loader/scene.cpp#L398)）でbindingとして保存します。値のdecode自体は[第3章](03_project_and_loading.md)のcomponent codec経由です（[scene.cpp#L81-L85](../../src/core/loader/scene.cpp#L81)）。

### behavior arena ✅実装済み

behaviorも**ECS Componentではありません**。`BehaviorAttachmentArena` はECS Chunkとは無関係な独自のarenaで、次を保持します（[behaviorarena.hpp#L113-L119](../../src/core/gamelogic/behaviorarena.hpp#L113)）。

- `BehaviorAttachmentInfo`（identityとオブジェクト対応）
- 元の `raw_component` JSON
- 型消去された `internal::RawBehaviorInstance` と `BehaviorDestroyFn`
- `initialized` フラグと `activation_delay`

ハンドルは [`BehaviorAttachmentHandle`](../../src/core/userpublic/behavior.hpp#L13) で、`EntityId` とは別系統です。

> **設計決定:** behaviorをECS Componentにしなかったのは、寿命の所有者がgame logic DLLだからです。DLLのアンロード/リロードでインスタンスを一斉に破棄する必要があり、その境界をECSのarchetype/Chunk寿命と混ぜると、どちらの規則が勝つのかが曖昧になります。scene JSONから見れば他のcomponentと同じ形ですが、runtime表現は別の所有者です。ロードの受理規則は[第3章](03_project_and_loading.md)に、実行時の呼び出し規約は[第5章](05_gameplay_and_services.md)にあります。

## 4.15 ユーザー独自Componentの現在地

`DECLARE_COMPONENT`と型付きregistererは実装されていますが、通常のゲームコード向けに安定した自動Component登録マクロはまだ公開されていません（`PELICAN_REGISTER_COMPONENT`はリポジトリ全体に不在）。実際の登録箇所は組み込み [`predefined.cpp`](../../src/core/ecs/predefined.cpp#L20)、benchmark、lifecycle testです。

なお、game System/Eventの登録はDLL化後、[`RegistrationOwner`](../../src/core/userpublic/details/reload/registrationowner.hpp#L11) 単位でreload時に`unregisterGameSystems(owner)`されるようになりましたが、Componentを**登録する**側は依然としてengine側のみです。

### 登録解除APIは入った（WP163 / ECS1）

一方で、Componentの**登録解除**は整備されました（[component/registerer.hpp#L90-L92](../../src/core/userpublic/details/component/registerer.hpp#L90)）。

```cpp
void unregisterComponent(RegistrationToken token);
void unregisterComponents(RegistrationOwner owner) noexcept;
std::size_t componentRegistrationCount(RegistrationOwner owner) noexcept;
```

これに伴い [`registerComponent<T>()`](../../src/core/userpublic/details/component/registerer.hpp#L36) の戻り値が `void` → `RegistrationToken` になりました。

解除は無条件ではありません。[`ComponentInfoManager::unregisterComponent()`](../../src/core/ecs/componentinfo.cpp#L64) は、そのComponentへ依存するECS Systemが残っていると拒否します。

```text
cannot unregister component '<name>': dependent ECS system '<sys>' remains
```

依存の逆参照テーブルは§4.10のSystem登録時に作られたものです。テストは [`test/registration_lifetime_test.cpp`](../../test/registration_lifetime_test.cpp) です。

そのため、現状の公開`GameObjects` builderで安全に使えるのは、エンジンがID宣言とruntime登録を済ませた型が中心です。独自Component対応を製品機能として追加するなら、次を一体で設計する必要があります。

- ID衝突管理（下記 §4.16 のとおり、engine側で例外として強制されるようになりました）
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
10. 並列Systemの競合は明示dependencyで防ぐ（§4.12のhazard検出は保険であって、設計の代わりではありません）。
11. Component登録の一意性はengine側が例外で守る（下記）。

### 登録時の拒否条件

[`ComponentInfoManager::registerComponent()`](../../src/core/ecs/componentinfo.cpp#L20) は、以前は黙って上書きしていた不正登録を**すべて例外で拒否**するようになりました。

| 条件 | 例外メッセージ |
|---|---|
| 空名 | `component registration name must not be empty` |
| ID >= 64 | `component '<name>' id N exceeds the registration limit (<64)` |
| ID重複 | `component '<name>' duplicates id N already registered by '<other>'` |
| 名前重複 | `component name '<name>' duplicates registered id N while incoming id is M` |
| lifecycle metadata欠落 | `component '<name>' registration requires typed lifecycle metadata` |

「ID >= 64」は§4.4のmask制約と同じ根拠です。

主要な実行可能仕様は [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L191) に集中しています。ECS改修時はこのファイルを先に読むのが最短です。

## 4.17 Archetype移行transaction（ECS-MUT0） ✅実装済み

WP152で、既存entityへのComponent追加/削除が「途中で失敗しても公開状態を壊さない」形になりました。宣言は [`archetypemigration.hpp`](../../src/core/ecs/archetypemigration.hpp) です。

### 入口

普段使うのは `ECSCore` の二つです。

- [`ECSCore::addComponent(entity, component, populate, adapters)`](../../src/core/ecs/core.hpp#L28)
- [`ECSCore::removeComponent(entity, component, adapters)`](../../src/core/ecs/core.hpp#L33)

### 低レベルのトークン

[`ECSArchetypeMigration::prepareAdd()`](../../src/core/ecs/archetypemigration.hpp#L123) / [`prepareRemove()`](../../src/core/ecs/archetypemigration.hpp#L127) が [`ECSArchetypeMigrationToken`](../../src/core/ecs/archetypemigration.hpp#L52) を返します。トークンが持つのは `publish() noexcept` / `rollback() noexcept` / `finish() noexcept` / `published()` / `stagedComponent()` / `removedComponent()` です。

entityの生成/破棄も同じ形です。[`ECSEntityMutation::prepareCreate()`](../../src/core/ecs/archetypemigration.hpp#L110) / [`prepareDestroy()`](../../src/core/ecs/archetypemigration.hpp#L114) が [`ECSEntityMutationToken`](../../src/core/ecs/archetypemigration.hpp#L82) を返します。

### adapterの契約

[`ECSArchetypeMigrationAdapter`](../../src/core/ecs/archetypemigration.hpp#L44) のコメントが規範です。

> prepare() may throw but must not change published state. rollback() is invoked for
> every attempted prepare (including the one that threw). Publication must not fail.

`rollback()` が**throwした prepare も含めて**全ての試行に対して呼ばれる点が要点です。adapter側は「prepareに入った時点でrollbackが来る」前提で書けます。light / collider / behavior など特殊なアタッチメントは、それぞれの投影WPが所有する別adapterのままで、ここは合成境界にすぎません。

### 既存値のprepare/publish

archetypeを変えない「値の差し替え」にも同じ形が入りました（[`coretemplate.hpp`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L217)）。

| 型/関数 | 役割 |
|---|---|
| [`PreparedComponentValue<T>`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L217) / [`prepareComponentValue()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L269) | 値のcopyを先に済ませる |
| [`PreparedComponentSwap<T>`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L228) / [`prepareComponentSwap()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L299) | swapで公開する版 |
| [`publishComponentValue()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L324) / [`rollbackComponentValue()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L334) | no-throwな公開と巻き戻し |

> **設計決定:** ヘッダのコメント通り、**投げうるcopyは公開の前に完了**し、publish/rollbackは no-throw 代入と厳密なversion交換だけを行います。§4.11の変更検知が使う `component_versions` も、rollback時に元の値へ正確に戻します。「失敗したら変更検知にも痕跡を残さない」という水準です。

### 隔離テスト用のスナップショット

[`isolationSnapshot()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L183) が [`IsolationSnapshot`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L162) として `global_tick` / `free_indices` / entity slot / chunkのmask+versionを露出します。transactionの前後でこれを比較すれば、「rollbackが本当に元へ戻したか」を外から検証できます。

テストは [`test/ecs_migration_test.cpp`](../../test/ecs_migration_test.cpp)、[`test/ecs_scheduler_test.cpp`](../../test/ecs_scheduler_test.cpp)、[`test/registration_lifetime_test.cpp`](../../test/registration_lifetime_test.cpp) です。
