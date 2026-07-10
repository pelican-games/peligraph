# ECS ライフサイクル設計への敵対的レビュー

対象: `docs/design_ecs_lifecycle.md` v1 (2026-07-10)

根拠はすべて 2026-07-10 時点の現行ツリーで確認した。重大度は S0 = 実装開始を止める欠陥、S1 = マージ前に必須の設計修正、S2 = 受け入れ条件の不足、とする。

## 結論

### 重大指摘 Top 3

1. **S0 — R5a のストレージ／ライフサイクル契約が未完成で、別の UB と部分初期化を作る。** `is_trivially_copyable` は default construction 省略の根拠にならず、現ストレージは alignment を表現できず、`allocateRaw` には populate/init 失敗時の rollback もない。さらに現役の `SimpleModelViewUpdateComponent` はユーザー定義 copy assignment のため暗黙 move constructor が生成されず、提案の `noexcept move` 条件をそのまま満たさない (`docs/design_ecs_lifecycle.md:29-42`, `src/core/userpublic/components/modelview.hpp:11-29`)。
2. **S0 — 世代付き ID は公開型の置換だけでは成立せず、特に scene clear で直ちに ABA が復活する。** 現在の `id_to_ref` は ID を添字にする append-only vector で、remove は死んだ entry を無効化しない (`src/core/userpublic/details/ecs/coretemplate.hpp:47-52`, `src/core/userpublic/details/ecs/coretemplate.cpp:70-106`, `src/core/userpublic/details/ecs/coretemplate.cpp:113-134`)。`clearEntities()` は表ごと clear するため (`src/core/userpublic/details/ecs/coretemplate.cpp:136-145`)、同じ実装方針のままなら旧 scene の `{0,0}` が新 scene の `{0,0}` に一致する。世代表の永続、live/tombstone、free list、全 API の resolve を設計に入れない限り「構造的に ABA 排除」は誤りである。
3. **S1 — PhysWorld の「次フレーム可視」固定は WP47 の既存契約を破る。** 現行テストは bind 済み transform を変更した直後、フレーム境界なしの `raycastClosest` で新位置を hit することを要求している (`test/physworld_test.cpp:144-173`)。実装も query ごとに transform を読む (`src/core/phys/physworld.cpp:235-256`)。フレーム頭 snapshot はこのテストと同フレーム query を壊すうえ、main loop のどの位置を「頭」とするかも未定義である (`src/core/appflow/loop.cpp:156-170`, `src/core/appflow/loop.cpp:198-215`)。

### 総合判定

**ECS 設計 v1 は Reject。R5a/R5b の着手・個別マージを承認できない。** alignment、生成 transaction、teardown、ID table、snapshot phase を v2 で規範化し、R5a と R5b は少なくとも同一の統合ゲートで検証すべきである。R5c は modelview と camera を分離し、modelview 側の型修正だけは R5a より前または同時に必要である。

**WP61 は条件付き Accept。** parser を触らずデータだけを正規形へ変換する R2 という切り方は安全である。ただし §7 の追加対象・ビット同値・`aspect` 条件・互換テスト除外を受け入れ条件へ加えること。

## 1. ライフサイクル規約

### 1.1 serializer の再定義は方向として正しいが、生成 transaction がない

現行 scene load は `allocateRaw` で byte slot を取り (`src/core/loader/scene.cpp:204-216`)、serializer callback を実行してから `initComponent` を呼ぶ (`src/core/ecs/componentinfo.cpp:18-31`)。従って、allocate 時に C++ object を先に construct し、serializer を「構築済み object への代入」にする修正自体は正しい。

しかし実経路は三つあり、application-level `init` の位置が既に一致していない。

- typed builder: allocate → copy assignment → `commit`/init (`src/core/userpublic/gameobjects.hpp:77-84`, `src/core/userpublic/gameobjects.cpp:25-28`)
- scene serializer: allocate → serializer → init (`src/core/loader/scene.cpp:211-223`, `src/core/ecs/componentinfo.cpp:18-27`)
- transient glTF: allocate → init → member assignment (`src/core/loader/scene.cpp:313-335`)

提案は serializer だけを定義し、三経路を統一していない。しかも `GameObjects::alloc` は commit より前に ID を live list へ publish する (`src/core/userpublic/gameobjects.cpp:19-23`)。copy assignment、JSON parse/ref、`init()` のいずれかが throw すると、C++ construct 済み／application init 済みが混在した entity が残る。

必要な規約は `construct all → populate all → init all → handle publish` の transaction である。途中失敗時は「init 済みだけ deinit、construct 済みだけ destroy、chunk count と ID slot を rollback」とする。`allocateRaw` を公開したままにするなら component ごとの app-init bit が必要であり、そうでなければ raw API を transaction 内部へ隠すべきである。

### 1.2 「trivial は construct 省略可」は不正確

`is_trivial` を `std::is_trivially_copyable_v<T>` 一個で表す案 (`docs/design_ecs_lifecycle.md:21-34`) は、次の別概念を混同している。

- byte relocation が可能か
- default/value construction を省略できるか
- destruction を省略できるか
- application `deinit` が必要か

trivially-copyable でも default member initializer や非 trivial default constructor を持てる。逆に現実装の `vector<uint8_t>::resize` は追加 byte をゼロにする (`src/core/userpublic/details/ecs/chunk.hpp:14-32`) ため、現在の「未指定値」は偶然 all-bits-zero である。builder の `.addComponent<T>()` は値を代入せず init だけ呼ぶ経路を公開している (`src/core/userpublic/gameobjects.hpp:88-103`) ので、「必ず後続代入される」という前提も偽である。

登録要件として少なくとも default constructibility を明示し、全 component を `T{}` 相当で value-construct する方が現在のゼロ初期化に最も近い。高速化するなら `trivially_copyable`、`trivially_default_constructible`、`trivially_destructible`、`has_deinit` を分離すること。all-bits-zero が有効値であることを一般の C++ 型へ仮定してはならない。

### 1.3 noexcept 条件は現在の登録型で直ちに割れる

`std::string` 自体の move constructor は通常の allocator では noexcept であり、そこは障害ではない。現実の障害は `SimpleModelViewUpdateComponent` が独自 copy assignment を宣言していること (`src/core/userpublic/components/modelview.hpp:17-26`) である。これにより暗黙 move constructor は生成されず、rvalue からの構築は allocation し得る `std::string` の copy constructor に落ちるため、`std::is_nothrow_move_constructible_v<SimpleModelViewUpdateComponent>` は false になる。

R5a で static_assert を入れるなら、R5c 待ちにはできない。独自 assignment を撤去して special members を正しく定義するか、modelview 統合を R5a に取り込む必要がある。

また設計は default construction、`deinit`、copy fallback の例外を扱っていない。登録 callback の `init`/`deinit` lambda 自体も noexcept ではなく (`src/core/userpublic/details/component/registerer.hpp:25-45`)、component 側の `deinit` も noexcept 宣言されていない (`src/core/ecs/predefined/modelview.hpp:10-16`, `src/core/userpublic/components/modelview.hpp:28-29`)。remove の途中で deinit が throw すれば slot は半破壊になる。deinit を noexcept 契約に含めるか、例外を捕捉して fatal にする規約が必要である。

「move 不可なら copy construct」も、別管理の `deinit` 資源を持つ型には一般に安全でない。copy が独立した app resource を作る型では、source を deinit せず destroy する規約が source resource を leak する。noexcept move を必須にするなら copy fallback は削除し、必要なら型ごとの明示 `relocate` callback とするべきである。

### 1.4 登録入口が保証を迂回できる

typed registerer は現在 size と init/deinit/serializer しか `ComponentInfo` に渡さない (`src/core/userpublic/details/component/registerer.cpp:15-24`)。一方、`ComponentInfoManager::registerComponent(ComponentInfo)` は public で、型情報なしに直接登録できる (`src/core/ecs/componentinfo.hpp:16-39`)。実際に EntityId は直接登録され (`src/core/ecs/predefined.cpp:18-25`)、benchmark も四型を直接登録している (`src/core/ecs/benchmark.cpp:94-115`)。

「登録 = 保証」を成立させるには raw registration を廃止して全型を typed registration に通すか、raw registration を POD-only の明示 API にして lifecycle/alignment metadata の完全指定を要求する必要がある。現状の API のまま callback 欠落を「trivial」とみなすのは再び silent UB になる。

### 1.5 engine teardown が clear 規約から漏れている

scene switch は bindings/PhysWorld を clear してから `GameObjects::removeAll()` を呼ぶ (`src/core/loader/scene.cpp:259-264`)。しかし通常の engine 終了は明示 `removeAll()` を呼ばず、module container が逆順に module を reset するだけである (`src/core/userpublic/pelican_core.cpp:27-45`, `src/core/container.hpp:25-42`)。`ECSCoreTemplatePublic` に destructor 規約もない (`src/core/userpublic/details/ecs/coretemplate.hpp:39-69`)。

単純に ECS destructor から deinit を呼ぶのも危険である。`SimpleModelViewComponent::deinit` は `PolygonInstanceContainer` を取得する (`src/core/ecs/predefined/modelview.cpp:8-12`) が、後から初期化された renderer module は ECS より先に破棄され得る。teardown 中に `GET_MODULE` が module を再生成する可能性すらある。loop 終了後、依存 module が生きている明示 phase で scene/ECS を clear することを engine lifecycle に追加すべきである。

## 2. アラインメント — 未決 2 の回答

**slot size の align 切上げだけでは足りない。現在の `vector<uint8_t>` を component storage に使う設計を変える必要がある。**

現行 metadata は size しか持たず alignment を持たない (`src/core/ecs/componentinfo.hpp:16-24`)。`VariedArray` は `vector<uint8_t>` の base に `stride * index` を足すだけである (`src/core/userpublic/details/ecs/chunk.hpp:14-35`)。`stride = sizeof(T)` なら C++ の `sizeof(T) % alignof(T) == 0` により二番目以降の相対 offset は揃うので、通常は stride 切上げ自体は不要である。問題は base address であり、byte allocator の契約から `alignas(32)` や `alignas(64)` を満たすことは導けない。base がずれていれば全 slot がずれる。

必要な変更は以下である。

1. `ComponentInfo` に `size` と独立した `alignment = alignof(T)` を追加する。
2. typed registerer から alignment を渡す。EntityId を含む直接登録も同じ metadata を持たせる。
3. component array は alignment-aware allocation (`operator new(bytes, std::align_val_t{alignment})` 等)を用い、固定 capacity 内で手動 lifetime を管理する。
4. `alignas(64)` かつ `std::string` を持つカナリアを最低二 slot 作り、全 pointer の alignment、remove 中間/末尾、clear を sanitizer 付きで検証する。

`CHUNK_CAPACITY` 分を constructor で reserve するため通常の count=1 では後続 resize による base 移動は避けられる (`src/core/userpublic/details/ecs/chunk.cpp:20-23`)。ただし fresh chunk に対する batch count の上限は検査されず (`src/core/userpublic/details/ecs/coretemplate.cpp:55-89`)、benchmark は一回に数万 entity を渡す (`src/core/ecs/benchmark.cpp:129-171`)。capacity の意味も API 契約として固定すべきである。

## 3. 世代付き ID

### 3.1 `id_to_ref` との統合方法が設計にない

現在の ID は `id_to_ref.size()` そのものであり、新規 ID と参照表 entry を同時 append する (`src/core/userpublic/details/ecs/coretemplate.cpp:70-106`)。remove は bounds/liveness を検査せず `id_to_ref[id]` を読む (`src/core/userpublic/details/ecs/coretemplate.cpp:113-121`)。非末尾 remove 後も死んだ ID の entry は移動先を指したままなので、死んだ A が moved B を resolve できる。`tryComponentRaw` の bounds/chunk checks も generation/liveness を見ない (`src/core/userpublic/details/ecs/coretemplate.cpp:147-169`)。

必要なのは `id_to_ref[index] = {optional ref, generation, live}` と free-index stack であり、次を全て同じ `resolve(GameObjectId)` に通すことである。

- remove
- try/component access
- mark changed
- `GameObjects::localTransform` / `setLocalTransform`
- SceneLoader と PhysWorld の参照
- chunk 末尾移動時の table 更新

`GameObjects::liveObjects()` の別 vector (`src/core/userpublic/gameobjects.cpp:14-17`) と真実が二重化するため、これも table の live state へ統合する方がよい。現行 remove は live vector に見つからなくても ECS remove を必ず呼ぶ (`src/core/userpublic/gameobjects.cpp:35-40`) ため、stale handle の二回 remove を明確な失敗か no-op のどちらにするかも規範化が必要である。

`clearEntities()` では generation table を捨ててはならない。全 live slot の generation を進めて free list に積む、または world epoch を ID に含める必要がある。これは scene switch の stale ID を無効化するための受け入れテストに必須である。

### 3.2 `EntityId` と `GameObjectId` の二重定義が未決

公開 alias は `GameObjectId = uint64_t`、内部 alias は別ヘッダの `EntityId = uint64_t` である (`src/core/userpublic/gameobjects.hpp:13-17`, `src/core/userpublic/details/ecs/entity.hpp:5-8`)。設計は「公開面は GameObjectId を置換」と書く一方、PhysWorld/SceneLoader が保持するものを EntityId と呼んでいる (`docs/design_ecs_lifecycle.md:49-75`)。

ここは次のどちらかを明記しなければならない。

- `GameObjectId` と内部 `EntityId` を同じ `{index,generation}` value type にする。
- chunk 内部には index だけを持たせ、公開 handle を resolve して内部 index へ変換する。

前者なら implicit EntityId component の登録・配列・末尾移動 (`src/core/ecs/predefined.cpp:20-25`, `src/core/userpublic/details/ecs/coretemplate.cpp:98-106`, `src/core/userpublic/details/ecs/coretemplate.cpp:117-133`)、EntityId query (`src/core/ecs/predefined.cpp:38-39`, `src/core/ecs/predefined/localtransformsystem.hpp:20-22`)、`LocalTransformComponent::parent` (`src/core/userpublic/components/localtransform.hpp:10-20`) まで変更対象である。後者なら両者を同じ名前で設計文書に書いてはならない。特に `parent` を raw index のまま残せば hierarchy 参照だけ ABA が残る。

### 3.3 GameObjectId 置換で直接コンパイルが割れる箇所

単純な struct 化で少なくとも以下が割れる、または明示 adapter が必要になる。

- ECS の `uint64_t EntityId` 戻り値から `GameObjectId` への変換、および逆方向の remove/component access (`src/core/userpublic/gameobjects.cpp:19-23`, `src/core/userpublic/gameobjects.cpp:35-40`, `src/core/userpublic/gameobjects.cpp:52-65`)
- live vector の `std::find`。比較演算子が必要 (`src/core/userpublic/gameobjects.cpp:35-39`)
- example の `GameObjectId object = 0` (`projects/example/code/playercontrol.cpp:8-10`)
- EntityId も struct 化する場合の `.parent = 0` (`projects/example/code/playercontrol.cpp:13-18`, `test/gamesystem_test.cpp:98-104`)
- ECSCore の public signature と全 index access (`src/core/ecs/core.hpp:18-22`, `src/core/userpublic/details/ecs/coretemplate.hpp:67-85`)
- benchmark の EntityId component/query。コメント内には `% 100` も残る (`src/core/ecs/benchmark.cpp:59-69`, `src/core/ecs/benchmark.cpp:94-126`)

SceneLoader は現在 `allocateRaw` の戻り値を二箇所で捨てているため (`src/core/loader/scene.cpp:204-230`, `src/core/loader/scene.cpp:313-339`)、ID 化では両方を捕捉する必要がある。`ObjectBinding` は transform と simple-model-view の二ポインタを持つ (`src/core/loader/scene.hpp:23-34`) ので、resolve 後に両 component を取り直すこと。PhysWorld には entity のない static collider も存在する。scene は collider-only object を identity transform で bind できる (`src/core/loader/scene.cpp:122-134`, `src/core/loader/scene.cpp:225-230`) ため、Binding は必須 EntityId ではなく `optional<GameObjectId>` または static/entity の variant が必要である。

無効値も未定義である。最初の実体が `{0,0}` なら既存の `0` sentinel と衝突する。`invalidGameObjectId` を別定義し、index+1 packing、`UINT32_MAX` index、または `optional<GameObjectId>` のいずれかへ移行すべきである。

### 3.4 32/32 の判定

**48/16 より 32/32 が妥当だが、wrap を「確率」として許容してはならない。** 48-bit index の表はメモリ上ほぼ使い切れない一方、LIFO は同じ free slot の再利用を集中させる。16-bit generation は 60 reuse/秒でも約 18 分で wrap する。32-bit でも 100 万 reuse/秒なら約 71.6 分、60 reuse/秒なら約 2.27 年であり、wrap 時の stale handle 一致は「低確率」ではなく同じ generation へ戻った時点で確定する。

推奨は 32/32 を採用し、generation が最大値に達した slot は永久 retire して新 index を使うことである。これなら 32-bit index の余裕を ABA 防止へ使え、設計が掲げる「構造的排除」と整合する。

## 4. PhysWorld snapshot の意味論

### 4.1 WP47 と明示的に矛盾する

WP47 テストは transform を `x=10` から `x=3` へ書き換え、直後の GameContext query が新位置を返すことを固定している (`test/physworld_test.cpp:144-173`)。次フレーム snapshot に変えるならこの test は壊れる。これは実装詳細ではなく public API の観測可能な変更である。

main loop では ECS system update、game system update、SeqPlayer、scene pending load の順で進む (`src/core/appflow/loop.cpp:156-170`, `src/core/appflow/loop.cpp:198-215`)。snapshot を ECS update 前に作れば ECS が更新した transform も一フレーム遅れる。game system 前に作れば `setLocalTransform → raycast` は旧値を見る。scene load は update 終端なので、新 scene collider がどの query から見えるかも別途定義が要る。

選択肢は二つである。

1. **即時可視を維持:** ID resolve した current transform から query ごとに collider を構築する。まず安全性を直し、性能問題は計測後に cache/invalidation で解く。
2. **snapshot を breaking contract として採用:** `beginPhysicsFrame()` の厳密な位置、update/query phase、scene load 後の可視時点、debug draw と query が同じ snapshot を見ることを API 文書とテストで固定する。既存 WP47 test は「次 frame まで旧値、次 frame から新値」へ意図的に変更し、R5b を挙動無変化とは扱わない。

現在の public `GameContext::createObject` は Transform と LocalTransform しか付けず (`src/core/userpublic/gamecontext.cpp:71-77`)、runtime collider 作成 API もない (`src/core/userpublic/gamecontext.hpp:36-42`)。従って「public game code の create collider → raycast」は現時点では表現できない。しかし将来の collider API を次フレーム遅延へ固定することになるし、既存の bind済み transform 更新→raycast は確実に壊れる。

## 5. 見落とした lifetime／詰め替え経路

調査範囲内で component byte relocation の直接 `memcpy` は remove の一箇所である (`src/core/userpublic/details/ecs/coretemplate.cpp:123-130`)。target=末尾でも現在は self-`memcpy` してから shrink するため、設計の末尾分岐は必要である。既存の archetype migration/add-to-existing API は見当たらない。

ただし、設計の「全経路」には以下が不足している。

| 経路 | 現実装 | 必要な扱い |
|---|---|---|
| builder copy | raw slot へ `operator=` (`src/core/userpublic/gameobjects.hpp:54-61`) | construct 後に限定。copy-assignable でない型に対する API 制約も追加 |
| serializer | callback assignment → init (`src/core/ecs/componentinfo.cpp:18-31`) | transaction と rollback |
| transient glTF | init → assignment (`src/core/loader/scene.cpp:324-335`) | 標準順へ統一 |
| clear | chunk vector を直接 clear (`src/core/userpublic/details/ecs/coretemplate.cpp:136-145`) | app-init 状態を見て deinit/destroy |
| module destruction | ECS の明示 clear なし (`src/core/userpublic/pelican_core.cpp:27-45`) | 依存 module 生存中の teardown phase |
| raw batch allocate | 連続 component pointer を外へ返す (`src/core/ecs/benchmark.cpp:129-175`) | 全要素 construct、部分失敗 rollback、capacity 検査 |
| system query | chunk array の typed raw pointer を渡す (`src/core/userpublic/details/ecs/coretemplate.hpp:165-191`) | フレームを跨ぐ保持禁止を contributor 規約だけでなく debug assertion/epoch でも検討 |
| SceneLoader | Transform/SimpleModelView の `void*` を長期保持 (`src/core/loader/scene.hpp:23-34`) | 一つの ID から毎回両 component を resolve |
| PhysWorld | Transform pointer を長期保持 (`src/core/phys/physworld.hpp:32-46`) | optional ID/static variant |

イベント層は ECS の byte storage をコピーしていない。payload は `make_shared<EventType>` で型付き copy/move construction され (`src/core/userpublic/details/event/registerer.hpp:48-92`)、dispatch 時だけ `void*` へ型消去される (`src/core/userpublic/details/system/registerer.cpp:42-49`)。従って今回の relocation 修正対象ではない。ただし static_assert は Event 自身が pointer 型でないことしか保証せず、pointer member までは禁止しない (`src/core/userpublic/details/event/registerer.hpp:48-52`, `src/core/userpublic/details/event/registerer.hpp:78-82`)。contributor 規約には「event payload に component pointer を入れない」も必要である。

なお設計が触れる `SceneUnloaded` は現コードに存在しない。定義・登録されている scene event は `SceneLoaded` だけであり (`src/core/userpublic/events.hpp:7-23`)、loader も load 完了後にそれだけを emit する (`src/core/loader/scene.cpp:232-234`)。clear と SceneUnloaded の関係を未決にする以前に、イベントを新設するのか「存在しないので対象外」なのか決める必要がある。

## 6. R5a/b/c の分割と検証

### 6.1 現在の依存表は逆向きか、過剰である

- **R5b は R5a を技術的には必要としない。** ID table と外部 pointer の resolve 化は現在の byte relocation のままでも実装でき、先に入れれば Q2 の UAF を封じ込められる。逆に R5a だけを先に入れても remove で slot address が変わる事実は同じなので、PhysWorld/SceneLoader の dangling pointer は残る。
- **R5a は現行 modelview 型修正を必要とする。** `SimpleModelViewUpdateComponent` が noexcept move 条件を満たさないため、modelview を全て R5c に後送りできない (`src/core/userpublic/components/modelview.hpp:11-29`)。
- **R5c の camera 統合は R5a/R5b の必須従属ではない。** modelview 統合と camera 二重所有を同じ段階に束ねる根拠が薄い (`docs/design_ecs_lifecycle.md:82-88`)。

実装 commit は分けてもよいが、release/main へのゲートは次のように組み直すべきである。

1. ID table + SceneLoader/PhysWorld resolve 化を先に入れるか、R5a と同じ integration branch で atomic にマージする。
2. alignment-aware storage、LifecycleOps、生成 transaction、teardown、`SimpleModelViewUpdateComponent` special members を一つの不可分ゲートにする。
3. modelview 統合はその直後、camera 統合は独立 WP にする。

### 6.2 golden 全維持で証明できることは狭い

既存 test には `GameObjects::remove`、`removeAll`、`clearEntities`、cb_deinit の直接テストがない。ECS を明示登録する game-system test も create/update までで remove しない (`test/gamesystem_test.cpp:165-185`)。従って golden pass が示すのは、現行登録型を使った限られた描画経路の出力が許容差内ということだけである。次は証明できない。

- constructor/destructor/deinit の回数と順序
- malformed JSON/init throw の rollback
- stale ID、double remove、scene clear 後の ABA
- alignment
- leak/UAF
- custom component、move-only component、over-aligned component
- teardown 順

さらに golden harness 自体は tolerance 比較であり (`test/golden_image_test.cpp:1352-1430`)、全ケースが bit-exact とは限らない。カナリアに必要なのは count だけでなく、イベント列で `construct → init → deinit → destroy` と `source move → source destroy` の順序を assert すること、throwing populate/init の fault injection、ASan/UBSan、scene clear と module teardown の別テストである。

## 7. WP61 の検査

### 7.1 データ限定の方針は安全

loader は source ごとに canonical key を先に探し、なければ alias を読む (`src/core/loader/basicconfig.cpp:88-104`)。`fov_y` alias だけ degree→radian 変換し (`src/core/loader/basicconfig.cpp:127-159`)、canonical `yfov` はそのまま float として読む。従って parser を変えず、project/default/template/test data を atomic に変換する R2 は互換性を狭めない。

Q4/WP61 の列挙にある runtime/test data は現ツリーの実出現と一致する。

- `projects/example/project.json:16-18`
- `src/core/resources/default_config.json:12-14`
- `src/devcli/projectinit.cpp:102-106`
- golden generator 6 箇所 (`test/golden_image_test.cpp:148`, `:171`, `:194`, `:217`, `:240`, `:263`)
- CMake generator 11 本 (`test/run_build_units_smoke.cmake:127`, `test/run_compute_headless.cmake:29`, `test/run_frame_plan_dump_headless.cmake:33`, `test/run_gpu_timing_headless.cmake:31`, `test/run_project_code_smoke.cmake:105`, `test/run_rpc_headless.cmake:36`, `test/run_rpc_inject_event_headless.cmake:33`, `test/run_rpc_inject_input_headless.cmake:39`, `test/run_rpc_scene_flow_headless.cmake:35`, `test/run_seqplayer_headless.cmake:37`, `test/run_vatplayer_headless.cmake:37`)

### 7.2 見落としと曖昧さ

1. **規範文書の例が漏れている。** `docs/design_project_format.md:120-145` は `project.json` v1 の例を示し、`default_config.json` と同形を維持すると明記しながら、camera は旧 `fov_y/near/far` のままである (`docs/design_project_format.md:134-145`)。runtime data ではないが、ユーザーがコピーする正規例なので WP61 対象に加えるべきである。
2. **`test/camera_test.cpp:82` は変換してはいけない。** test 名と assertion は legacy alias 受理を明示的に検証している (`test/camera_test.cpp:195-214`)。WP61 の「camera test のデータ部を対象」と「別名受理 test は残す」はこの一箇所について矛盾する。R2 ではその object を明示除外し、canonical project data は既に `test/camera_test.cpp:56-69` にある、と作業票へ書くべきである。
3. **project init の結合 test は旧 key のままでも通る。** generated project の起動を確認する `test/run_devcli_project_init.cmake:16-22`, `:101-111` は parser が alias を受理し続けるため、template 変換漏れを検出しない。生成された `project.json` の camera keys を JSON として検査する assertion を加えるべきである。

### 7.3 度→ラジアンの精度

旧経路は JSON をまず `float` にし、`float(pi/180)` を掛ける (`src/core/loader/basicconfig.cpp:107-128`)。新経路は decimal literal を直接 `float` に丸める。この二つが同じ実数式でも、任意の出力桁数で同じ float bit になる保証はない。

今回列挙された値については、次の decimal は旧計算と同じ binary32 へ丸まる。

| degree | canonical decimal | binary32 |
|---:|---:|---:|
| 45 | `0.7853981633974483` (短縮 `0.78539816339` も可) | `0x3f490fdb` |
| 50 | `0.872664625997` | `0x3f5f66f3` |
| 60 | `1.04719755120` | `0x3f860a92` |

受け入れ条件は「式が正しい」「golden が通る」だけでなく、旧 alias と新 canonical を別々に parse した `CameraProjectionSpec::yfov` の `std::bit_cast<uint32_t>` が一致する unit test にするべきである。golden の一部には非ゼロ tolerance があり、1 pixel も変わらないことの一般証明にはならない (`test/golden_image_test.cpp:1352-1430`)。

### 7.4 `aspect` の扱い

現行 project/default/template は aspect を持たず、parser は `aspect` を optional として読む (`src/core/loader/basicconfig.cpp:193-198`)。欠落時は viewport aspect を用い、resize で更新される (`src/core/renderer/camera.cpp:420-440`)。従って WP61 の converter は window width/height から `aspect` を**新設してはならない**。追加すると現在の viewport-following camera が固定 aspect になり、resize 意味論が変わる。

`aspect` は既存なら値を保持、欠落なら欠落を保持、orthographic には追加しない、を受け入れ条件に入れること。なお CLI/playback の `fov_y` は JSON camera key ではなく degree 単位の別 API である (`src/core/launchconfig.hpp:13-17`, `src/core/renderer/camera.cpp:513-518`)。機械的な全リポジトリ置換の対象外である。

### 7.5 WP61 の修正版受け入れ条件

1. 上記 runtime/test data と `docs/design_project_format.md:134` を同時変更する。
2. `test/camera_test.cpp:82` の legacy alias case は変更しない。
3. 45/50/60 degree の旧/new parse 結果を binary32 bit 一致で固定する。
4. `aspect` の absent/present を変えない。
5. generated `project.json` の key を検査し、`yfov/znear/zfar` の存在と `fov_y/near/far` の不在を確認する。
6. grep は camera JSON node/data 範囲に限定する。C++ の `EngineLaunchCameraOverride::fov_y` 等を誤って変更しない。
7. 全 test と golden を通し、canonical/legacy camera unit test を両方維持する。

この修正後なら WP61 は R2 として安全であり、rpc/collider/shader/parser を同時に触らない境界も妥当である。
