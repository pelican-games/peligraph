# 第4章 ECS徹底解剖

[索引へ戻る](README.md) / [前章](03_project_and_loading.md)

## 4.1 最初に「二つのECS」という誤解を解く

PelicanのECS関連コードは二箇所に分かれています。

- [`src/core/ecs/`](../../src/core/ecs) — moduleとしてのファサード、Componentメタデータ、組み込みComponent/System
- [`src/core/userpublic/details/ecs/`](../../src/core/userpublic/details/ecs) — Entity、Chunk、ECS実体テンプレート

これは独立した二つのWorldが動いているわけではありません。[`ECSCore`](../../src/core/ecs/core.hpp#L13) がメンバとして一個の [`ECSCoreTemplatePublic`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L97) を持ち、呼び出しを委譲します。

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

型からIDへの対応は [`ComponentIdByType<T>`](../../src/core/userpublic/details/ecs/componentdeclare.hpp#L8) のtemplate特殊化です。

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
| 18 | `SpriteViewComponent` | `sprite_view` | 2D sprite表示（登録は [`registerSpriteViewComponent()`](../../src/core/userpublic/components/spriteview.cpp#L107) 経由） |

IDは単なる外部識別子ではありません。ECS内部では、次の二つの番号を使い分けます。

- **Component ID**: `DECLARE_COMPONENT`で型に与える宣言上の識別子。上の表やscene名と対応する、外向きの番号です。
- **dense index**: 配列の添字そのもの。`ComponentInfoManager`の`infos[index]`、Chunkの`component_arrays[index]`と`component_versions[index]`、maskの`1ULL << index`は、すべてこちらで引きます（[`chunk.hpp` 内](../../src/core/userpublic/details/ecs/chunk.hpp#L61) の `// Indexed by dense component index.`）。

両者を繋ぐのが [`ComponentInfoManager::getIndexFromComponentId()`](../../src/core/ecs/componentinfo.cpp#L86) で、現在の実装はIDをそのまま添字として返します。つまりIDと添字は**値としては同じですが、概念としては別物**です。IDに欠番（上の表の4〜15）があれば添字にもそのまま穴が空くので、「dense（0から詰まっている）」は名前が示す想定であって、現状の実装が満たしている保証ではありません。コード側の区別も徹底されておらず、[`ECSComponentChunk::has()`](../../src/core/userpublic/details/ecs/chunk.hpp#L83) は引数の型が`ComponentId`ですが、呼び出し側は全てdense indexを渡しています。型名ではなく用途で読んでください。

ただし**未登録スロットの読み出しは例外になります**。実体は [`getFromIndex()`](../../src/core/ecs/componentinfo.cpp#L89) で、`token` が未設定なら `std::out_of_range("component index N is not registered")` を投げます。「IDがそのままindex」という値の関係は保ちつつ、穴の空いたスロットを黙って読むことはできません。

### runtimeの型消去メタデータ

[`ComponentInfo`](../../src/core/ecs/componentinfo.hpp#L21) は次を保持します。

- `size`, `alignment`, `name`
- default construct
- destroy
- relocate（末尾要素での穴埋め用、§4.7）
- optional `init()`
- optional `deinit() noexcept`
- optional JSON `ref(JsonArchiveLoader&)`
- [`internal::RegistrationOwner owner`](../../src/core/ecs/componentinfo.hpp#L33) と [`internal::RegistrationToken token`](../../src/core/ecs/componentinfo.hpp#L34)（登録解除のための識別、§4.15）

登録は [`UserComponentRegistererTemplatePublic::registerComponent<T>()`](../../src/core/userpublic/details/component/registerer.hpp#L36) です。template内で型付きlambdaを関数ポインタへ変換し、`ComponentInfoManager`へ型消去（type erasure — 具体的な型`T`を`void*`と関数ポインタの組へ畳み、受け取る側が`T`を知らないまま構築・破棄・移動だけは正しく行えるようにする手法）して渡します。戻り値は `void` ではなく `RegistrationToken` です。

compile時制約は次です。

- default construct可能
- noexcept move construct可能
- noexcept destruct可能
- `deinit()`があるならnoexcept

これらは削除・rollback・teardownを例外なしで完遂するための契約です。

### trivial型と非trivial型のrelocate

登録コードの [`relocate` callback](../../src/core/userpublic/details/component/registerer.hpp#L53) は二分岐します。

- trivially copyable（自明にcopy可能 — bit列をそのまま複製しても意味が壊れない型。`std::is_trivially_copyable_v`で判定します）: `memcpy`
- それ以外: destinationへmove constructし、sourceをdestroy

`std::string`や`std::optional`を持つ`SimpleModelViewComponent`も、末尾要素の移動で安全に詰め直せます。テストは [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L207) です。

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

[`releaseId()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L348) はlive/refを消し、generationを1増やしてindexを`free_indices`へ積みます。この`free_indices`がfree list（空きリスト — 使い終わった枠を実際に解放せず、空いた番号だけを別の入れ物へ貯めておいて次の確保でそこから取り出す方式）で、再利用はvector末尾からなのでLIFO（last in, first out）です。これにより同じ操作列は同じindex再利用順になります（[`Free-list LIFO test`](../../test/ecs_lifecycle_test.cpp#L449)）。

generationが`UINT32_MAX`に達したindexはwrapさせず、永久retireしてfree listへ戻しません。

## 4.4 ArchetypeとChunk

### Archetype

同じComponent ID集合を持つentity群が同じarchetypeです。mapは次の形です。

```cpp
unordered_map<vector<ComponentId>, vector<ChunkIndex>, VectorHash>
```

key作成時はComponent IDをsortするため、呼び出し側で指定した順序が違っても同じarchetypeとして見つかります（[`createEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L365)）。一方、Chunk内部の配列順序は作成時のmetadata順を保持します。

### ChunkはSoA

[`ECSComponentChunk`](../../src/core/userpublic/details/ecs/chunk.hpp#L19)（Chunk — 同じarchetypeのentityだけを固定長でまとめて置くブロック。SystemへComponent配列を渡す単位でもあります）はComponentごとの配列を別々に持ちます。

```text
Chunk: [EntityId, Transform, ModelView]

EntityId[]  : E0 E1 E2 ...
Transform[] : T0 T1 T2 ...
ModelView[] : M0 M1 M2 ...
```

entityごとにstructを並べるAoS（array of structures）ではなく、Systemが同じComponentを連続走査しやすいSoA（structure of arrays — Componentごとに独立した配列を持つ配置。走査対象のComponentだけが連続して並ぶので、使わないComponentがcacheを圧迫しません）です。

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

64という数はmask型 [`ComponentMask = uint64_t`](../../src/core/userpublic/details/ecs/componentdeclare.hpp#L22) が1語であることから来ます。matchingが`(chunk_mask & required) == required`という1回のAND＋比較で済み、ChunkにもSystemにも可変長のmask領域が要りません。増やすなら、`ComponentMask`と [`MAX_COMPONENTS`](../../src/core/userpublic/details/ecs/componentdeclare.hpp#L21)、登録時の上限チェック（§4.16）、Chunk構築時の`index >= 64`チェック（[`chunk.cpp` 内](../../src/core/userpublic/details/ecs/chunk.cpp#L108)）を揃えて変える必要があります。

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

`try`の中でthrowが起きると（`populate`や`init()`はもちろん、Chunk確保の失敗も含みます）、[`catch` block](../../src/core/userpublic/details/ecs/coretemplate.cpp#L519) が**次の順で**巻き戻します。番号がそのまま実行順で、各段の内部を逆順にたどります。

1. 成功済みinitに対応する`deinit()`を逆順実行。
2. 各Chunkの追加末尾をdestroy。
3. 今回新設したChunkをerase。
4. `id_table`と`free_indices`を生成前copyへ戻す。
5. archetype mapとSystem matching cacheを再構築。
6. 元の例外を再throw。

強い例外保証（strong exception guarantee — 操作が途中で例外になっても副作用を一切残さず、呼ぶ前の状態がそのまま観測できる、という例外安全の水準。「壊れたまま止まらない」だけの基本保証より一段強い要求です）を優先して、生成前のID table/free list全体をcopyしています。大量entity・巨大ID tableで例外可能なComponentを作る場合、このcopy costも性能評価対象です。

> 🧩 **難所 — 生成transactionの三段構え**([`createEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L365) の `try` と [catch節](../../src/core/userpublic/details/ecs/coretemplate.cpp#L519))
>
> **何をする所か**: Chunk領域の確保・ID割当・`populate`・`init()`・公開を一括で行い、途中の任意の例外で「呼ぶ前の状態」へ完全に戻します。
>
> **素朴に読むと**: `try` の中に巻き戻し対象が3種類（Chunk末尾の構築済み要素・`init()`済みComponent・`id_table`/`free_indices`）混在していて、しかもそれぞれ戻し方が違います。素朴に「例外が来たら作ったentityを `remove()` する」と書くと、まだ `live = false` のentityは `resolve()` に弾かれて消せず、Chunkに幽霊行が残ります。さらに [`ECSComponentChunk::allocate()`](../../src/core/userpublic/details/ecs/chunk.cpp#L160) は、途中の配列で失敗すると**それまでに伸ばした配列を自分で戻してから再throwします**（[`chunk.cpp` 内](../../src/core/userpublic/details/ecs/chunk.cpp#L173)）。`chunk.count`も各`VariedArray`の`count`も呼ぶ前の値のままなので、外側がここで `rollbackTail(batch_count)` を呼ぶと、消えるのは今回追加した分ではなく**元から居た末尾のentity**です（`count`が足りなければそのまま下へ突き抜け、まだ何も構築していない領域を`destroy_one`します）。それを防ぐのが「`Allocation` を `count = 0` で先に `push_back` し、`allocate()` が返ってから `allocation.count = batch_count` を代入する」という一見冗長な2行（[該当箇所](../../src/core/userpublic/details/ecs/coretemplate.cpp#L448)）です。
>
> **骨子**:
> ```text
> Allocation{count=0} を記録 → chunk.allocate() → count を代入
> EntityId を書き込む。ただし entry.live = false のまま
> populate(...) → init() を entity順×Component順、{deinit, ptr} を平坦に積む
> --- ここまで来て初めて --- live = true に公開 → version を global_tick へ
> catch: initialized を逆順 deinit → allocations を逆順 rollbackTail
>        新設Chunkを erase → id_table/free_indices を copy から復元 → rebuildChunkCaches()
> ```
>
> **手がかり**: `duplicate_check` は重複検出用のsort済みcopyですが、そのまま**archetype keyとして再利用**されます（[検索側](../../src/core/userpublic/details/ecs/coretemplate.cpp#L428)・[登録側](../../src/core/userpublic/details/ecs/coretemplate.cpp#L440)）。名前から用途が読めません。`initialized` が持つのは`EntityId`ではなく `{deinit関数ポインタ, void*}` の生ポインタで、この時点ではまだ末尾要素での穴埋め（§4.7）が一度も起きていない＝アドレスが安定している、という前提に乗っています。テストは [`ecs_lifecycle_test.cpp` 内](../../test/ecs_lifecycle_test.cpp#L324) "Populate and init failures roll back storage IDs and resources" と [同ファイル内](../../test/ecs_lifecycle_test.cpp#L366) "Bulk creation splits chunks and faults atomically"。
>
> **不変条件**: `populate`/`init` の実行中、対象entityは `live = false`（公開APIから観測させない）。`allocation.count` は `chunk.allocate()` が成功した後にだけ代入する。`initialized` は成功した分だけを順に積み、巻き戻しは必ず逆順。

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

実装は [`gameobjects.hpp`](../../src/core/userpublic/gameobjects.hpp#L20) です。

### compile時に蓄積しているもの

- `ComponentIdHolder<...>`: 追加した全ComponentのID列
- `IndexHolder<...>`: 初期値を渡したComponentだけの位置列
- `ComponentDataTuple`: 初期値へのreference tuple

型なし`addComponent<T>()`はIDだけを追加します。値あり版はIDに加え、「全Component列の何番目へこの値をcopyするか」と値referenceを蓄積します。

### `finish()`で実行時へ橋渡し

[`finish()`](../../src/core/userpublic/gameobjects.hpp#L79)（[2オーバーロード](../../src/core/userpublic/gameobjects.hpp#L100)）はcompile時ID packを`span`にし、ECSのpopulate callbackを作ります。callback内のfold expression（畳み込み式 — `(f(pack), ...)` のように書いて、可変個のtemplate引数それぞれへ同じ式を順に適用するC++17の構文）が、保存した位置indexを使って`void*`配列を正しい`T*`へcastし、値をcopy assignmentします（[`GameObjects::copy()`](../../src/core/userpublic/gameobjects.hpp#L82)）。

要するに、流暢なbuilder APIの各段階で型が変わり、最後にだけruntimeの`vector<ComponentId> + void*`世界へ落としています。

### 寿命上の注意

値tupleは`const T&`を保持します。通常の一続きの式では`finish()`までtemporaryが生存しますが、builder contextを長期保存したり、参照元を先に破棄したりしないでください。意図された使い方は一式のchainです。

## 4.7 削除は末尾要素での穴埋め

[`ECSCoreTemplatePublic::remove()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L550) はO(Component数)で削除します。一般にswap-and-pop（swap-remove）と呼ばれる手法ですが、実装は入れ替えではありません。削除位置をdestroyしてから末尾要素をそこへrelocateし、`count`を1減らす片道の移動で、末尾側へ書き戻すものは何もありません。

```text
削除前: [A, B, C, D]
               ↑ Bを削除

各Component配列で:
1. B.deinit()
2. B.destroy()
3. DをBの位置へrelocate（relocate自身がDの旧位置をdestroyします）
4. --count

削除後: [A, D, C]
```

実際の配列処理は [`VariedArray::removeAt()`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) です。ECS側は削除前に末尾の`EntityId`を読み、移動したDの`id_table.ref.array_index`をBの旧位置へ更新します。

最後に削除対象IDをreleaseしてgenerationを進めます。

> 🧩 **難所 — deinitを呼ぶ経路と呼ばない経路**([`VariedArray::removeAt()`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) / [`clear()`](../../src/core/userpublic/details/ecs/chunk.cpp#L84) / [`rollbackTail()`](../../src/core/userpublic/details/ecs/chunk.cpp#L62))
>
> **何をする所か**: Component配列から要素を取り除く3経路です。見た目はほぼ同じループなのに、`deinit_one` を呼ぶものと呼ばないものがあります。
>
> **素朴に読むと**: `removeAt` と `clear` は `deinit_one` を呼びますが、`rollbackTail` は**意図的に呼びません**。`rollbackTail` は「constructはしたが `init()` はまだ／もう取り消した」要素を捨てる経路なので、ここで `deinit` を足すと §4.5 のcatch節が持つ `initialized` リストの分と合わせて**二重deinit**（GPU instanceを二回remove）になります。逆に [`~VariedArray()`](../../src/core/userpublic/details/ecs/chunk.cpp#L30) は `rollbackTail(count)` を呼ぶだけなので、**Chunkをただ破棄するとdeinitは一切走りません**。§4.8 の `clearEntities()` が `chunks_storage.clear()` の前に明示的に `chunk.clear()` を回している（[`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L581)）のはそのためで、[`~ECSCoreTemplatePublic()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L293) は `clearEntities()` を呼ばないので、teardown順を守らずにECSを破棄すると `SimpleModelViewComponent::deinit()` が飛ばされます。もう一つ、`removeAt` の `if (index != last)` ガード（[`chunk.cpp` 内](../../src/core/userpublic/details/ecs/chunk.cpp#L78)）は最適化ではありません。外すと `relocate_one(dst, src)` が dst == src で呼ばれ、**直前にdestroyしたばかりの自分自身からmove construct**することになります。
>
> **骨子**:
> ```text
> removeAt(index): deinit → destroy → [index!=last なら] relocate(index ← last) → --count
> clear():         末尾から deinit → destroy → --count
> rollbackTail(n): 末尾から --count → destroy のみ        ← deinit しない
> ~VariedArray():  rollbackTail(count) → operator delete  ← deinit しない
> ```
>
> **手がかり**: Chunk側の3関数（[`rollbackTail`](../../src/core/userpublic/details/ecs/chunk.cpp#L189) / [`removeAt`](../../src/core/userpublic/details/ecs/chunk.cpp#L197) / [`clear`](../../src/core/userpublic/details/ecs/chunk.cpp#L205)）はいずれも `indices.rbegin()` から回します。Componentの破棄順をindex降順に固定して、ログや副作用の順序を決定的にするためです。`deinit_one` は `init()`/`deinit()` を持たないComponentでnullになりうるので、毎回nullptrチェックが入ります。テストは [`ecs_lifecycle_test.cpp` 内](../../test/ecs_lifecycle_test.cpp#L259) "Modelview GPU instance deinit covers remove clear and teardown" と [同ファイル内](../../test/ecs_lifecycle_test.cpp#L285) "Aligned non-trivial component observes lifecycle and relocation order"。
>
> **不変条件**: §4.16 の不変条件5（`init()` 成功済みComponentは全経路でちょうど一度だけ `deinit()`）。`rollbackTail` に `deinit` を足す変更はこれを静かに破ります。destroyと `--count` の順序は経路ごとに逆で（`removeAt` / `clear` はdestroy→`--count`、`rollbackTail` は `--count`→destroy）、揃っているのは結果だけです。守るべきは順序そのものではなく「関数を抜けた時点で `count` の範囲に生きたオブジェクトだけが並ぶ」ことで、relocateのdstは必ずdestroy済み＝オブジェクトの居ない領域。

### 生ポインタ保持が危険な理由

Chunk自体は固定capacityですが、別entityを削除すると末尾entityが移動します。Componentへのpointer/referenceをフレームを跨いで保存すると、同じアドレスが別entityを指す可能性があります。長期保持するのは`EntityId`で、必要時に`tryComponent()`し直すのが契約です。

## 4.8 clearとteardown

[`clearEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L578) は全Chunkをclearし、Chunk/archetypeを破棄し、Systemのmatching cacheと`last_run_tick`をresetします。全live IDをreleaseするので、clear前のIDはgeneration不一致になります。

Chunk clearはComponent indexの逆順で各要素を`deinit → destroy`します（[`ECSComponentChunk::clear()`](../../src/core/userpublic/details/ecs/chunk.cpp#L205)）。GPU instanceを持つ`SimpleModelViewComponent::deinit()`は [`PolygonInstanceContainer::removeModelInstance()`](../../src/core/ecs/predefined/modelview.cpp#L11) を呼びます。そのため第2章の明示teardown順が重要です。

## 4.9 Componentアクセスと変更version

- [`tryComponent<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L354): stale/Componentなしなら`nullptr`
- [`component<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L358): なければ詳細付き例外
- [`setComponent<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L364): copy assignment後にversion更新
- [`markComponentChanged()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L640): pointer経由で直接変更した場合の手動通知

ChunkはComponent indexごとに`component_versions[index]`を一個持ちます。entityごとのversionではなく、**Chunk内のそのComponent列全体のversion**です。どれか一entityを書き換えると、そのChunkを読むSystemはChunk全体を再処理します。

直接pointerを書き換えて`markComponentChanged()`を忘れると、非force Systemは変更を見逃します。公開 [`GameObjects::setLocalTransform()`](../../src/core/userpublic/gameobjects.cpp#L55) はLocalTransformと内部Transformの両方を更新・markし、描画instanceへも即時反映します。

## 4.10 内部ECS Systemの登録

template本体は [`registerSystem<TSystem, TComponents...>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L409) です。現在のsignatureは `registerSystem(TSystem &system, std::vector<SystemId> &&depends_list, bool force_update)` で、**Systemのinstance参照を受け取り`SystemId`を返します**。ECSCore側の入口は [`registerSystemForce()`](../../src/core/ecs/core.hpp#L45) です。返る`SystemId`は後続Systemの依存指定に使えます。

### 登録の副作用

`SystemId` を返すだけではありません。

1. wrapperへ **`std::string name`** を持たせ、`wrapper.name = typeid(TSystem).name();` を代入します（[`coretemplate.hpp` 内](../../src/core/userpublic/details/ecs/coretemplate.hpp#L429)）。この名前が§4.12のhazard/cycleエラー文言に出ます。
2. [`internal::registerECSSystemComponentDependencies()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L27) を呼び、Componentからの**逆参照テーブル**へ登録します（[`coretemplate.hpp` 内](../../src/core/userpublic/details/ecs/coretemplate.hpp#L576)）。これがComponent登録解除時の「依存Systemが残っている」判定に使われます（§4.15）。
3. `execution_plan_dirty = true` を立て、実行計画キャッシュを無効化します（[`coretemplate.hpp` 内](../../src/core/userpublic/details/ecs/coretemplate.hpp#L556)）。

対称に、[`unregisterSystem()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L654) は逆参照テーブルから外して `execution_plan_dirty = true` を立て、デストラクタ [`~ECSCoreTemplatePublic()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L293) が `internal::unregisterECSCoreComponentDependencies(this)` を呼びます。

### 対応process形

二つのConceptでduck typingします。

```cpp
// 全matching chunkをまとめて一回
void process(std::span<ChunkView<TComponents...>> chunks);

// chunkごとに一回
void process(std::tuple<TComponents*...> arrays, size_t count);
```

判定は [`HasBatchProcess`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L77) と [`HasPerChunkProcess`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L82) です。登録時の [`static_assert`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L410) で「少なくともどちらか」のprocess形が必須になりました。

実装は二つの独立した`if constexpr`なので、両方のsignatureを同時に実装すると両方呼ばれます。通常はどちらか一方だけを実装します。

### `const`でread/writeを宣言

Component packの型が次を同時に表します。

- `const T`: read-only
- `T`: write

登録時にpointer型を組み立て、constを外してComponent IDを求めつつ、read/write indexへ分類します（[`process_component`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L421)）。System実装へ渡るtupleは宣言どおり`const T*`または`T*`になります。

> 🧩 **難所 — 型パックとindicesの位置対応**([`registerSystem()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L409) の [fold](../../src/core/userpublic/details/ecs/coretemplate.hpp#L425) と消費側の [index_sequenceラムダ](../../src/core/userpublic/details/ecs/coretemplate.hpp#L496))
>
> **何をする所か**: 型パックから (1) dense index列、(2) read/write分類、(3) matching maskを作り、実行時に `void*` を正しい `T*` へ戻します。
>
> **素朴に読むと**: foldの `(process_component(static_cast<TComponents*>(nullptr)), ...)` は**値を渡していません**。null pointerは型を運ぶためだけの実引数で、受け側は `auto* ptr` から `remove_pointer` → `remove_const` してIDを引きます。「なぜdereferenceしないのか」ではなく「なぜpointerなのか」を掴まないと読めません。より危険なのは消費側です。`chunk.getRef(indices[Is]).ptr` を `std::tuple_element_t<Is, std::tuple<TComponents...>>*` へ `static_cast` している、つまり **`component_indices` の並び順と `TComponents...` の並び順が位置で一対一に対応している**ことが暗黙の前提になっています。この対応を保証しているのは、fold式のカンマ演算子が左から右への評価順を持つことと、`comp_indices.push_back(idx)` が末尾へ積むことの二つだけです（[`coretemplate.hpp` 内](../../src/core/userpublic/details/ecs/coretemplate.hpp#L410)）。型の側には何の裏付けもありません。誰かが「archetype keyと同じようにsortしよう」「重複を潰そう」と `comp_indices` に手を入れると、全Chunk配列が別の型としてreinterpretされ、**コンパイルエラーも実行時チェックも出ないまま**壊れます。read/writeの分類も同じfoldで決まりますが、そちらは `read_indices` / `write_indices`（実体は `std::vector<size_t>` ですが、並び順に意味はありません）として §4.12 へ渡り、hazard判定はcomponent indexをキーにした `std::map` で畳まれる（[`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L684) / [`conflicts` の畳み込み](../../src/core/userpublic/details/ecs/coretemplate.cpp#L172)）ため、**並び順には依存しません**。hazard検出を巻き添えにするのは順序変更ではなく、重複除去のように集合そのものを変える改変です。
>
> **骨子**:
> ```text
> fold: TComponents... を左から順に
>         idx = getIndexFromComponentId_Ref(ComponentIdByType<remove_const_t<T>>::value)
>         comp_indices.push_back(idx)         ← 位置 = パック内の位置
>         matching_mask |= 1ULL << idx / is_const<T> ? read_indices : write_indices
> 実行時: { static_cast<tuple_element_t<Is, tuple<TComponents...>>*>(chunk.getRef(indices[Is]).ptr)... }
>                                                        ↑ 位置 Is で突き合わせ
>
> 例: registerSystem<Sys, const TransformComponent, SpriteViewComponent>
>       パック位置        0                          1
>       comp_indices  [   1,                        18 ]   ← push_back順＝パック順
>       実行時 Is=0 → getRef(1)  を const TransformComponent* へ
>              Is=1 → getRef(18) を       SpriteViewComponent* へ
>     ここで comp_indices を [18, 1] へ並べ替えると、Sprite配列を Transform として読みます
> ```
>
> **手がかり**: [`getRef()`](../../src/core/userpublic/details/ecs/chunk.cpp#L138) はdense indexキーなので**Chunk側の配列順は無関係**で、効いているのは `indices[]` の並びだけです。同じComponentを `const T` と `T` の両方でパックに書くと、readとwriteの両方にindexが入り、§4.12 の `conflictingComponents()` はwriteありとみなします。テストは [`ECS scheduler keeps read read systems parallel`](../../test/ecs_scheduler_test.cpp#L73) "ECS scheduler keeps read read systems parallel"（`ECSSystemGraphNode` を手組みして `writes=false` を直接与え、スケジューラ単体がread-readを並列に残すことを見る）。`const` 宣言から分類を実際に通すのは [`ECS runtime applies auto serialization before worker scheduling`](../../test/ecs_scheduler_test.cpp#L164) / [`ECS runtime strict launch rejects an unordered hazard`](../../test/ecs_scheduler_test.cpp#L186) の `registerSystem<SchedulerReader, const SchedulerProbeComponent>` 側です。
>
> **不変条件**: `component_indices` は**パック順のまま**保持する（sort・unique・安定化のいずれも禁止）。`matching_mask` が `1ULL << idx` なので dense indexは0〜63（§4.4 / §4.16）。

### matching cache

required Component maskを作り、既存Chunkのmaskが包含すれば`matching_chunk_indices`へ追加します。新Chunk生成時も [`updateSystemChunkCache()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L323) が全Systemへ照合します。

Chunkが空になっても個別削除ではChunk自体を消さないため、matching cacheに空Chunkが残る可能性があります。Systemは`count == 0`も安全に扱う必要があります。

## 4.11 変更検知

System wrapperは`last_run_tick`を持ちます。ECS全体の [`global_tick`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L402) は`update()`冒頭で1増えます。

per-chunk型では、required Component列の最大versionが`last_run_tick`未満ならskipします。実行後、write宣言した列のversionを現在tickへ更新します。

batch型では、全matching Chunkのどれかに変更があれば一回だけ全viewを渡します。実行後は全matching Chunkのwrite列を更新します。

`force_update`ならversionに関係なく毎回実行します。現在の組み込みSystemは全て [`registerSystemForce`](../../src/core/ecs/predefined.cpp#L32) で登録されています。

## 4.12 依存グラフと並列実行 ✅実装済み

WP148（ECS0）でスケジューラが書き換わりました。実行計画を作るのは [`internal::buildECSExecutionPlan(nodes, hazard_policy)`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L206) で、[`ECSCoreTemplatePublic::update()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L663) がそれを呼びます。

```text
Level 0: A  B  C  ── workerへ並列schedule ── wait
Level 1: D  E     ── workerへ並列schedule ── wait
Level 2: F        ── workerへschedule      ── wait
```

同levelのSystemを [`JobSystem`](../../src/core/job_system.cpp#L62) へscheduleし、level末尾で全完了を待ちます。worker例外はmain threadへ再throwされます。`JobSystem`自体は変わっていません。

### read/writeからhazardを検出する

[`conflictingComponents()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L170) が、二つのSystemが**同じcomponent indexへアクセスし、片方でもwriteしている**組み合わせを競合とみなします。ただしこれは*依存edgeの自動導出*ではなく、*hazardの検出*です。既に依存関係で順序が付いている組（[`isOrderedBefore()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L151) が到達可能性で判定）は競合になりません。

検出されたhazardの扱いはpolicy次第です。

> 🧩 **難所 — hazard検出と計画の決定性**([`buildECSExecutionPlan()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L206) / [`isOrderedBefore()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L151))
>
> **何をする所か**: 宣言済み依存で順序が付いていないread/write衝突をhazardとして拾い、policyに応じて暗黙edgeを足すか例外にします。
>
> **素朴に読むと**: 罠が三つあります。(1) `isOrderedBefore(before, after, deps)` は**逆向きに歩きます**。`dependencies.at(current)` は「currentが依存している相手」＝前任者集合なので、この関数は `after` から前任者を遡って `before` に届くかを見ています。前向き探索だと思って読むと符号が反転します。そもそも順方向（誰が自分に依存しているか）の表はここにありません。`depended_by` を組むのは [`makeExecutionLevels()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L97) の中だけで、`buildECSExecutionPlan()` が持っているのは前任者集合の `dependencies` だけです。`A ← B ← C`（Cの依存がB、Bの依存がA）なら、`isOrderedBefore(A, C)` は C の依存 `{B}` → B の依存 `{A}` と遡って true を返します。(2) hazardループは `dependencies` を**書き換えながら**走ります（[暗黙edgeの挿入](../../src/core/userpublic/details/ecs/coretemplate.cpp#L265)）。追加した結果は以降のペアの `isOrderedBefore()` 判定に即座に効くので、「先に全hazardを集めてから一括でedgeを足す」実装へ変えると、後続ペアが既に順序付いたことに気付けず余計な直列化が増えます。追加edgeは必ず小さいid→大きいid（[冒頭のsort](../../src/core/userpublic/details/ecs/coretemplate.cpp#L208) でid昇順にsort済み）で、この向きの一貫性が循環を作らない支えです。(3) 入力順が非決定です。`update()` は `unordered_map` の `systems` を走査して `graph_nodes` を作る（[組み立て箇所](../../src/core/userpublic/details/ecs/coretemplate.cpp#L677)）ため、plan builder側のsortが必須になります。`zero_degree` が `std::set`、`conflicts` が `std::map<size_t, std::string>`（component index順）なのも「level内の順序とエラー文言に並ぶComponent名の順を実行ごとに変えない」ためで、`unordered_*` へ置き換えるとgolden testが揺れます。
>
> **骨子**:
> ```text
> nodes を id 昇順 sort → 重複id / 存在しない依存 / 重複依存 を拒否
> makeExecutionLevels(...)          ← 循環をここで先に検出（戻り値は捨てる）
> for (left,right) id昇順の全ペア:
>     衝突なし or どちらかの向きに到達可能 → skip
>     strict → メッセージを溜める / automatic → deps[right] += left（＝ left が先）
> strict のメッセージが溜まっていれば "; " 連結で1本の例外
> makeExecutionLevels(...)          ← 今度は本番。処理件数 != ノード数 なら循環
> ```
>
> **手がかり**: [`(void)makeExecutionLevels(...)`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L243) は戻り値を捨てる「検証専用の空打ち」で、到達可能性を使う前にグラフの妥当性を確定させています。循環検出は「levelに入らなかった」ではなく `executed_count != nodes.size()` の件数一致で見るので、未実行ノード名を列挙できます。テストは [`ECS scheduler accepts hazards ordered by a transitive dependency`](../../test/ecs_scheduler_test.cpp#L85)（推移的到達可能性）、[`ECS auto serialization uses registration order`](../../test/ecs_scheduler_test.cpp#L111)（左＝小さいidが先）、[`ECS scheduler rejects dependency cycles and names unexecuted nodes`](../../test/ecs_scheduler_test.cpp#L138)（循環と未実行ノード名）。
>
> **不変条件**: 自動直列化の向きは常に「登録IDの小さい方が先」（移行互換のための仕様であって実装都合ではありません）。level内の順序とエラー文言中の名前順は決定的に保つ（sorted containerを維持する）。計画キャッシュは `execution_plan_dirty` とpolicy変化でのみ再構築し、Chunkの増減では再構築しない。

### hazard policyは2種

[`internal::ECSHazardPolicy`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L35) は次の二つです。

| policy | 挙動 |
|---|---|
| `automatic_serialization`（既定） | 登録IDの小さい方を先に実行する暗黙edgeを足し、**WARNINGログを出す** |
| `strict` | 全hazardを集めて1本の例外にする |

`automatic_serialization` のログ文言は次です（[`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L717)）。

```text
ECS auto serialization: '{}' before '{}' for component(s) {}; add an explicit dependency edge (strict mode rejects this hazard)
```

`strict` の例外はhazardを `; ` で連結し、末尾に `; add dependency edges or use automatic serialization` が付きます（[`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L276)）。

### policyの切替は `--strict-assets`

専用フラグはありません（[`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L664)）。

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
| 循環依存 | `ECS dependency cycle detected; unexecuted systems: '<name>' ...`（[`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L138)） |
| 存在しないSystemへの依存 | `... depends on missing system id N; system would be unexecuted`（[送出箇所](../../src/core/userpublic/details/ecs/coretemplate.cpp#L229)） |
| 同じSystemへの重複依存 | `... declares dependency on system '<name>' more than once; system would be unexecuted`（[送出箇所](../../src/core/userpublic/details/ecs/coretemplate.cpp#L237)） |
| System IDの重複 | `ECS execution graph contains duplicate system id N`（[送出箇所](../../src/core/userpublic/details/ecs/coretemplate.cpp#L218)） |

循環依存は「実行levelへ入らないまま黙って無視される」のではなく、[`makeExecutionLevels()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L97) が処理件数と全System数の一致を検査して例外にします。

### 実行計画のキャッシュ

計画は `execution_levels` にキャッシュされ、`execution_plan_dirty`（System登録/解除で立つ、§4.10）かpolicyが変わったときだけ再構築します（[`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L673)）。

### prepareフェーズ

level実行前に、そのlevelの**全System**の`prepare_func`がowner threadで呼ばれ、その後にまとめて`JobSystem`へscheduleし、`wait()`します（[`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L735)）。Systemは`prepareEcsWorkerDependencies()`を実装して`GET_MODULE`をowner thread上で済ませ、worker job中のmodule生成（freeze後はエラー）を避けます。例は [`CameraSystem::prepareEcsWorkerDependencies()`](../../src/core/ecs/predefined/camerasystem.cpp#L9) です。

### 組み込みSystemの依存

現在の組み込み登録はほぼ全順序まで強化されています（[`predefined.cpp` 内](../../src/core/ecs/predefined.cpp#L31)）。

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
| [`LocalTransformSystem`](../../src/core/ecs/predefined/localtransformsystem.cpp#L52) | `EntityId, Transform, LocalTransform` | 公開transformをGLM内部transformへcopy |
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
| `light` | [`LightContainer`](../../src/core/light/lightcontainer.hpp#L26) |
| `collider` | [`PhysWorld`](../../src/core/phys/physworld.hpp#L38) |
| `behavior` | [`BehaviorAttachmentArena`](../../src/core/gamelogic/behaviorarena.hpp#L111) |

### collider

[`ColliderComponent`](../../src/core/userpublic/components/collider.hpp#L14) という名前ですが、組み込みComponent ID宣言・ECS登録には含まれません。scene loaderが`name == "collider"`を特別扱いし（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L151)）、`phys_world.bindCollider()`（[同](../../src/core/loader/scene.cpp#L398)）でbindingとして保存します。値のdecode自体は[第3章](03_project_and_loading.md)のcomponent codec経由です（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L81)）。

### behavior arena ✅実装済み

behaviorも**ECS Componentではありません**。`BehaviorAttachmentArena` はECS Chunkとは無関係な独自のarenaで、次を保持します（[`behaviorarena.hpp` 内](../../src/core/gamelogic/behaviorarena.hpp#L113)）。

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

一方で、Componentの**登録解除**は整備されました（[`unregisterComponent()`](../../src/core/userpublic/details/component/registerer.hpp#L90)）。

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

> 🧩 **難所 — generation 0 を跨がない台帳**([`nextGeneration()`](../../src/core/userpublic/details/reload/registrationowner.cpp#L40) / [`ownerIsCurrentLocked()`](../../src/core/userpublic/details/reload/registrationowner.cpp#L50) / [`releaseRegistrationOwner()`](../../src/core/userpublic/details/reload/registrationowner.cpp#L89))
>
> **何をする所か**: System / Component / Event / Behaviorの登録をDLLリロード単位で追跡する台帳で、`RegistrationOwner`（identity + generationの64bit）と `RegistrationToken` を発行・失効させます。
>
> **素朴に読むと**: 一行ずつが小さいのに、それぞれ別の理由で「そう書かないと壊れる」行です。`if (++generation == 0) ++generation;` はgenerationの **0 を「無効」に予約**しているためで、[`RegistrationToken::operator bool()`](../../src/core/userpublic/details/reload/registrationowner.hpp#L47) が `identity_ != 0 && generation_ != 0` で判定する以上、wrapして0へ戻った瞬間に有効なtokenが空tokenへ化けます（identityが1-basedで `slot[identity - 1]` に格納されるのも同じ理由）。`if (generation == 0) return true;` は穴ではなく**後方互換の意図的な口**で、`allocateRegistrationOwner()` を通さない生の数値owner（テストfixtureやengine内部サービス）を通すためです — ソースのコメントが規範です。`releaseRegistrationOwner()` が生きたtokenを1つでも見つけたら**黙って解放しない**のは、identity再利用によって生きたtokenのowner照合が別のDLL世代に当たるABA（ABA問題 — 対象がA→B→Aと元の値に戻ったせいで「一度も変わっていない」と誤判定してしまう類の不具合。ここではidentityが解放と再確保で一周し、同じ番号が別世代のownerへ配られる状況を指します）を防ぐため。`static auto *value = new RegistrationState;` をわざとdeleteしないのは、[`~ComponentInfoManager()`](../../src/core/ecs/componentinfo.cpp#L11) がtokenをreleaseするので、台帳が先に死ぬとuse-after-freeになるからです。
>
> **骨子**:
> ```text
> owner = (generation << 32) | identity   identity は1-based、generation 0 は「旧式・常にcurrent」
> token = {identity, generation}          どちらかが0なら偽（空token）
> acquire     : owner が current でなければ throw
> release(tok): identity範囲 / active / generation一致 / kind一致 を全部見て false を返せる
> releaseOwner: 生きた token が残っている限り no-op
> ```
>
> **手がかり**: `releaseRegistrationToken()` が `kind` まで照合するのは、identity再利用後に「Systemのtokenでcomponentを消す」誤爆を防ぐためです。戻り値 `bool` を捨てる呼び出し（`(void)internal::releaseRegistrationToken(...)`）は「既に失効していても正常」の意味。[`registrationTokens()`](../../src/core/userpublic/details/reload/registrationowner.cpp#L161) が末尾から前へ走査するのは、purge側が逆順に消すことを期待しているからです。テストは [`Registration tokens drive deterministic owner purge`](../../test/registration_lifetime_test.cpp#L74) "Registration tokens drive deterministic owner purge" と [`Registration owner slot reuse rejects a stale generation`](../../test/registration_lifetime_test.cpp#L177) "Registration owner slot reuse rejects a stale generation"。
>
> **不変条件**: generationは0を跨がない（identity 0 と generation 0 は「無効」に予約）。台帳のstaticは解放しない。owner解放は「そのownerのtokenが0件」になってから。

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

[`ECSArchetypeMigration::prepareAdd()`](../../src/core/ecs/archetypemigration.hpp#L124) / [`prepareRemove()`](../../src/core/ecs/archetypemigration.hpp#L128) が [`ECSArchetypeMigrationToken`](../../src/core/ecs/archetypemigration.hpp#L52) を返します。トークンが持つのは `publish() noexcept` / `rollback() noexcept` / `finish() noexcept` / `published()` / `stagedComponent()` / `removedComponent()` です。

entityの生成/破棄も同じ形です。[`ECSEntityMutation::prepareCreate()`](../../src/core/ecs/archetypemigration.hpp#L111) / [`prepareDestroy()`](../../src/core/ecs/archetypemigration.hpp#L114) が [`ECSEntityMutationToken`](../../src/core/ecs/archetypemigration.hpp#L82) を返します。

> 🧩 **難所 — 移行publishのrelocate舞踏**([`State::publish()`](../../src/core/ecs/archetypemigration.cpp#L92) / [`rollbackPublished()`](../../src/core/ecs/archetypemigration.cpp#L228))
>
> **何をする所か**: prepare済み（容量1）の新archetype Chunkを `chunks_storage` の末尾へ公開し、旧Chunkの当該行から全Componentを新Chunkへmoveし、旧Chunkの穴を末尾行で埋めます。`rollbackPublished()` はそれを一手ずつ逆再生します。
>
> **素朴に読むと**: これは「entityを移す」コードではなく「**Component配列ごとに2回のrelocateを行い、chunkの `count` を手で辻褄合わせする**」コードで、しかも順方向と逆方向で意味の違う変数が同名です。`publish()` の `source_last_row = source.size() - 1` に対し `rollbackPublished()` は `source.size()` で **-1 がありません**（publishが既に `--source.count` 済みなので `size()` がそのまま元の末尾行indexになる）。片方だけ見るとoff-by-oneに見えます。各ループで `target_array.destroy_one(target_ptr)` を先に呼ぶのは、target[0] がprepareの `allocate(..., 1)` で**default construct済み**だからで、省くとオブジェクトを上書き構築してリークします。`add` のとき追加Componentは `source.indices` に含まれないためこのループが触らず、staged値（populate + init済み）がそのまま生き残ります。`remove` では逆に `continue` で明示的に飛ばし、外した値を `removed_component_chunk[0]` へ退避してから [`finishPublished()`](../../src/core/ecs/archetypemigration.cpp#L351) が `deinit` します。`id_table` の付け替えが2件（移動したentity自身と、穴埋めに動いた `backfilled_entity`）なのも忘れやすい点です。
>
> **骨子**:
> ```text
> 旧Chunk [ A  B  C  D ]      B を移行
>              ↑row     ↑last
>  1) B → 新Chunk[0]   2) D → row   3) --count（配列ごと + chunk自身）
> 旧Chunk [ A  D  C ]         新Chunk [ B ]
> ```
>
> **手がかり**: `rollbackPublished()` 冒頭の `assert(published_target_chunk_index + 1 == core->chunks_storage.size())` が本質で、**このtokenは `chunks_storage` の末尾を占有し続けている**前提で撤収します（割り込みの構造変更は `MutationScope` が禁止しています）。rollbackはadapterへ**逆kindの `publish()`** を渡します（`rollback()` ではありません）。既に公開済みのものを打ち消す＝逆向きの公開、という設計です。[`publishDestroy()`](../../src/core/ecs/archetypemigration.cpp#L759) は `releaseId()` と同じ規則（generationが `UINT32_MAX` ならretire）を**手で書き直している**ので、`releaseId()` を変えるならここも変えます。テストは [`ecs_migration_test.cpp` 内](../../test/ecs_migration_test.cpp#L431) と [逆向きtokenの解放](../../test/ecs_migration_test.cpp#L482)。
>
> **不変条件**: `publish()` / `rollbackPublished()` / `finishPublished()` は全て `noexcept`（この中にthrowしうる操作を新たに書かない）。relocateのdstは必ずdestroy済み。ただしcountの増減はrelocate回数と一対一ではなく、`publish()` は1配列あたり最大2回relocateして `--source_array.count` の1件だけ（target側はprepareの `allocate(..., 1)` のまま触らない）、`rollbackPublished()` は2回のrelocateに対し `++source_array.count` と `--target_array.count` の2件です。公開後のtokenは `chunks_storage` の末尾を所有するので、publishとrollbackの間に他の構造変更を挟まない。rollback後は `component_versions` まで元の値へ戻す（変更検知に痕跡を残さない）。

### adapterの契約

[`ECSArchetypeMigrationAdapter`](../../src/core/ecs/archetypemigration.hpp#L44) のコメントが規範です。

> prepare() may throw but must not change published state. rollback() is invoked for
> every attempted prepare (including the one that threw). Publication must not fail.

`rollback()` が**throwした prepare も含めて**全ての試行に対して呼ばれる点が要点です。adapter側は「prepareに入った時点でrollbackが来る」前提で書けます。light / collider / behavior など特殊なアタッチメントは、それぞれの投影WPが所有する別adapterのままで、ここは合成境界にすぎません。

> 🧩 **難所 — publishを落とさないreserve**([`prepare()`](../../src/core/ecs/archetypemigration.cpp#L451) 末尾 / [`prepareCreate()`](../../src/core/ecs/archetypemigration.cpp#L976) 末尾)
>
> **何をする所か**: 公開に必要な**器**をprepare側で先に押さえます。`chunks_storage.reserve(+1)`、`archetype_to_chunks.try_emplace(key)`、`archetype->second.reserve(+1)`、マッチする全Systemの `matching_chunk_indices.reserve(+1)` の4点が両者に共通で、`prepareCreate()` はさらに `id_table.reserve(+1)`（新規index時のみ）と `free_indices.reserve(+1)` を足した6点です。後の2点は `publishCreate()` の [`id_table.emplace_back()`](../../src/core/ecs/archetypemigration.cpp#L640) と `publishDestroy()` の [`free_indices.push_back()`](../../src/core/ecs/archetypemigration.cpp#L687) を無失敗にするためのもので、`prepareDestroy()` 側にも同じ `free_indices.reserve(+1)`（[`prepareDestroy()` 側](../../src/core/ecs/archetypemigration.cpp#L1104)）が置かれています。
>
> **素朴に読むと**: 性能チューニングの4行に見えますが、**これが上のadapter規約を成立させている実体**です。`publish()` は `noexcept` で `emplace_back` / `push_back` / mapへの挿入をそのまま呼ぶので、reserveと `try_emplace` がなければ、publish中の再確保やrehashが `bad_alloc` を投げ、`noexcept` 関数からの伝播で `std::terminate` します。派生する非自明な点が二つ。(1) `try_emplace` はarchetype mapに**空エントリを残しうる**ので、`inserted_target_archetype` フラグと [`eraseUnpublishedArchetype()`](../../src/core/ecs/archetypemigration.cpp#L64) で「自分が作ったなら、空のときだけ消す」を判定しています（他人が既に持っていたkeyを消してはいけません）。(2) `chunks_storage.reserve()` はvectorを再確保しうるので、それ以前に取った `ECSComponentChunk&` は無効になります。prepareが `auto &source = core.chunks_storage[...]` を使い終えてからreserveを呼ぶ順序は必然です。一方Componentの**実体pointer**（`staged_component` / `removed_live_component`）は、各 `VariedArray` が独立したheap blockを持つ（[`chunk.cpp` 内](../../src/core/userpublic/details/ecs/chunk.cpp#L27)）おかげで再確保の影響を受けません。
>
> **骨子**:
> ```text
> prepare(): 検証 → target_chunk 構築 → allocate(1) → populate/init   ← ここまで throw 可
>            source_versions を控える
>            ---- publish が使う器を全部押さえる（reserve / try_emplace）----
>            adapters.push_back(adapter) → adapter->prepare(...) を前から順に
> publish(): 押さえた器へ入れるだけ。確保しない = 失敗しない
> ```
>
> **手がかり**: `state->mutation` は `MutationScope` を **prepareからpublish/finish（またはrollback）まで握りっぱなし**なので、prepared tokenを持っている間ECSの構造変更は全て `logic_error` になります（前の難所の「末尾を占有し続ける」前提はこれで成立します）。[`discardPrepared()`](../../src/core/ecs/archetypemigration.cpp#L73) はadapterを**逆順**にrollbackし、`state->adapters.push_back(adapter)` を `adapter->prepare()` の**前**に置いているのは、上の「throwしたprepare自身にもrollbackが来る」規約を満たすためです。順序を入れ替えると規約が破れます。tokenのデストラクタは `rollback()` を呼ぶので、`finish()` を忘れたtokenは自動で巻き戻ります。テストは [`ecs_migration_test.cpp` 内](../../test/ecs_migration_test.cpp#L215) / [adapter失敗の巻き戻し](../../test/ecs_migration_test.cpp#L261) / [cacheとversionの復元](../../test/ecs_migration_test.cpp#L390)。
>
> **不変条件**: publish経路に「確保しうる操作」を足さない（足すならprepare側に対応するreserveを足す）。`try_emplace` で自分が挿入したarchetype keyだけを、空のときだけ消す。adapterは「登録してからprepareを呼ぶ」、rollbackは必ず逆順。

### 既存値のprepare/publish

archetypeを変えない「値の差し替え」にも同じ形が入りました（[`coretemplate.hpp`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L217)）。

| 型/関数 | 役割 |
|---|---|
| [`PreparedComponentValue<T>`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L217) / [`prepareComponentValue()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L281) | 値のcopyを先に済ませる |
| [`PreparedComponentSwap<T>`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L228) / [`prepareComponentSwap()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L311) | swapで公開する版 |
| [`publishComponentValue()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L335) / [`rollbackComponentValue()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L345) | no-throwな公開と巻き戻し |

> **設計決定:** ヘッダのコメント通り、**投げうるcopyは公開の前に完了**し、publish/rollbackは no-throw 代入と厳密なversion交換だけを行います。§4.11の変更検知が使う `component_versions` も、rollback時に元の値へ正確に戻します。「失敗したら変更検知にも痕跡を残さない」という水準です。

### 隔離テスト用のスナップショット

[`isolationSnapshot()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L183) が [`IsolationSnapshot`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L162) として `global_tick` / `free_indices` / entity slot / chunkのmask+versionを露出します。transactionの前後でこれを比較すれば、「rollbackが本当に元へ戻したか」を外から検証できます。

テストは [`test/ecs_migration_test.cpp`](../../test/ecs_migration_test.cpp)、[`test/ecs_scheduler_test.cpp`](../../test/ecs_scheduler_test.cpp)、[`test/registration_lifetime_test.cpp`](../../test/registration_lifetime_test.cpp) です。
