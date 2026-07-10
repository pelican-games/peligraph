# ECS ライフサイクル設計 v2 再レビュー

対象: `docs/design_ecs_lifecycle.md` v2 (2026-07-10)

根拠は 2026-07-10 時点の現行ツリーと、v1 レビュー
`docs/design_reviews/2026-07-10_ecs_lifecycle_review_codex.md` との照合による。
重大度は S0 = このまま実装すると correctness を壊す、S1 = R5-core の統合ゲート前に
設計・受け入れ条件へ追加必須、S2 = 実装・デバッグ品質を固定する補強、とする。

## 判定

**条件付き Accept。** v1 の大枠の欠陥は正しい方向へ修正され、alignment-aware storage、
世代表の永続化、即時可視の維持、単一統合ゲートというアーキテクチャは受理できる。
ただし以下は任意の改善ではなく、R5-core 着手前に設計本文へ反映する条件である。
反映前の実装・マージは承認しない。

1. **C1 — `trivially_default_constructible` だけを根拠にした値構築の「省略」を削除する。**
   全 slot で `T{}` の意味と object lifetime を実際に成立させる。高速路を設けるなら、
   `T{}` と同値であることを別の明示 trait/callback とテストで証明する。
2. **C2 — ECS の構造変更を非再入にし、transaction/relocate/clear 中の再帰的な
   allocate/remove/clear を mutation guard で変更前に拒否する。** シングルスレッドは
   callback からの再入を防がない。rollback は count だけでなく ID/free-list、version、
   新規 chunk と各 cache を含む状態を復元する。
3. **C3 — batch API を世代 ID と複数 chunk に対応する形へ再設計する。** 一つの
   `first EntityId` と連続 component pointer を返す現 API は、free-list 再利用および
   `count > CHUNK_CAPACITY` と両立しない。全 ID を返し、component span は chunk ごとの
   populate callback 中だけ有効にするか、batch を明示的に chunk 分割する。
4. **C4 — `EntityId{}` を必ず invalid にし、generation の状態遷移・枯渇・失敗時の
   復元を規範化する。** stale API の結果も全層で統一し、remove の `bool` を
   `ECSCore` → `GameObjects` → `GameContext` まで伝播する。
5. **C5 — typed 登録を serializer の有無から分離する。** 現 typed registerer は
   `ISerializable` 制約付きなので、`EntityId` や benchmark 型をそのまま通せない。
   lifecycle/alignment metadata を常に型から生成し、JSON callback は optional とする。
6. **C6 — PhysWorld binding の正確な資源形、stale prune と名前再利用、全終了経路の
   teardown を定義する。** 正常終了だけでなく scene load/loop の例外時にも、依存
   module が生存している間に no-throw clear が一度走ることを保証する。
7. **C7 — benchmark は削除せず typed/bounded batch へ書き直し、性能予算をゲートに
   固定する。** §7 には chunk 境界、再入、move-only、default-invalid、例外終了の
   test と、MSVC ASan とは別の UBSan job を追加する。

## 1. v1 指摘との照合

| v1 指摘 | v2 の状態 | 判定 |
|---|---|---|
| S0 ライフサイクル | 4 特性、値構築、noexcept move/destroy/deinit、copy fallback 廃止、modelview 修正、transaction、raw API 廃止、teardown を採用 | **部分解消**。§1-1 の「省略」と transaction の再入・rollback 範囲が未完成 |
| S0 世代 ID | live/generation/ref table、free-list、clear 後も表を保持、wrap 時 retire、型統一、parent、invalid、全 resolve、live list 廃止を採用 | **ほぼ解消**。default-invalid、batch の戻り値、状態遷移と stale policy が不足 |
| S1 PhysWorld | snapshot を撤回し query 時 resolve で即時可視を維持 | **解消**。ただし variant は binding 全体ではなく transform source だけを置換すべき |
| alignment | alignment metadata、aligned allocation、`vector<uint8_t>` 廃止、capacity 検査を採用 | **解消** |
| 分割 | R5-core を単一ゲート、modelview 後続、camera 独立へ修正 | **解消** |
| 検証 | イベント列、fault injection、sanitizer、ABA、決定性を追加 | **ほぼ解消**。C7 のケースが不足 |
| pointer/event 規約 | component pointer の長期保持と event payload への格納を禁止、`SceneUnloaded` 非採用を決定 | **解消**。system query の debug epoch はなお有用だが必須条件とはしない |

従って「v1 の全指摘を全面受理した」という説明は設計方針については正しいが、
ライフサイクル S0 は §1-1 の一文により完全解消にはなっていない。また v1 §5 が求めた
builder の copy-assignable 制約と raw batch の具体的な置換 API は v2 に落ちている。

## 2. S0 — 値構築の契約と高速路が矛盾する

§1-1 は「常に `T{}` 相当」と定義する一方、`trivial default constructible` なら
memset/省略可としている (`docs/design_ecs_lifecycle.md:23-33`)。後半は前半を保証しない。

例えば `struct C { int value; };` は trivially default constructible だが、`C{}` は
`value == 0` であり、未初期化 storage 上で構築を省略した `value` は indeterminate である。
memset も、一般の pointer、enum、浮動小数、padding を含む型について `T{}` と同じ
値表現だとは規定できない。aligned raw storage へ移行した後は、従来の
`vector<uint8_t>::resize` が偶然行っていたゼロ埋めすらなくなる
(`src/core/userpublic/details/ecs/chunk.hpp:14-35`)。

規範は次のいずれかに絞るべきである。

- 原則として全要素を引数なしの `std::construct_at(ptr)` 相当で value-construct する。
  range callback にして compiler/library の最適化を許す。
- 省略または byte zero を使う型は、`is_trivially_default_constructible` とは別の
  engine-owned trait（例: `value_init_is_all_bits_zero<T>`）を明示 opt-in し、未 populate の
  `.addComponent<T>()` でも `T{}` と同値になることを型ごとにテストする。

「後で全フィールドを上書きするから省略」は generic builder/serializer では成立しない。
現 builder は値を渡さない `.addComponent<T>()` を公開している
(`src/core/userpublic/gameobjects.hpp:88-103`)。correctness を性能予算で緩めてはならない。

また typed builder の populate は現状 copy assignment である
(`src/core/userpublic/gameobjects.hpp:54-61`)。登録条件を move constructible だけにするなら、
`addComponent(const T&)` は copy-assignable な型にだけ制約するか、forwarding/emplace API を
別に設ける必要がある。v1 で挙げた move-only component のカナリアも §7 へ戻すべきである。

## 3. S0 — transaction は「シングルスレッド」だけでは rollback できない

§1-2 の順序と publish 遅延は正しい。しかし `populate` と `init` は user callback であり、
そこから別 entity の allocate/remove/clear を呼べる。remove/clear の `deinit` や destructor
からの再入も同じである。単一 thread でも普通の関数呼び出しとして再入できるので、
「他 remove は起きない」は自明ではない。

途中の remove が同じ chunk の末尾を target へ relocate すると、transaction が記録した
`old_count`、構築済み範囲、component pointer が別 entity を指し得る。nested allocation が
新 chunk を足す場合も chunk/cache の状態が変わる。従って次を契約にする。

1. allocate/remove/clear/relocate を共通の structural mutation guard で囲む。
2. construct/populate/init/deinit/destroy callback 中の structural API 再入は、状態を変える前に
   deterministic に拒否する。将来 nested creation が必要なら command queue へ遅延する。
3. rollback は逆順に行う。完了した init を逆順 deinit、構築済み object を逆順 destroy する。
4. throw した `init` 自身は「throw 時に app resource を一つも残さない」という strong
   exception guarantee を持つ。そうしないなら「init 開始済み」を deinit 対象に含め、
   deinit が部分 init に耐える別契約が必要である。
5. 既存 chunk では count と component version を復元する。transaction が fresh chunk を
   作った場合は `chunks_storage`、`archetype_to_chunks`、全 system の chunk cache まで戻すか、
   空 chunk を残す弱い保証を明示してその決定性・メモリ上限をテストする。
6. reused ID の reservation 失敗では free-list の順序を完全に戻す。fresh index なら
   未 publish entry を pop し、失敗した作成が後続 ID 列を変えないことを固定する。

現在の §7 の「init が throw」だけでは、init 内で資源獲得後に throw するケースを検出できない。
外部 resource count まで検査する fault を一つ入れるべきである。

## 4. S1 — batch allocation と世代 ID の API が両立していない

現 `allocateEntity(..., count)` は一つの先頭 `EntityId` と component ごとの連続 base pointer を
返す (`src/core/ecs/core.hpp:18-22`, `src/core/userpublic/details/ecs/coretemplate.cpp:33-110`)。
benchmark は 40,000/40,000/20,000 件を一呼びで要求する
(`src/core/ecs/benchmark.cpp:129-175`)。一方 chunk capacity は 4,096 である
(`src/core/userpublic/details/ecs/chunk.hpp:48-50`)。

v2 の「上限検査」だけなら数万件 batch は reject される。また free-list から ID を再利用すると、
batch の ID は index も generation も連続とは限らず、「先頭 ID」から残りを導出できない。
複数 chunk へ分割すれば component array も一つの span にはできない。

推奨 API は次の分離である。

- 単体作成: 一 entity の transaction を行い一 ID を返す。
- bulk 作成: count を内部で chunk 分割し、`span<EntityId>` または ID vector を返す。
  populate は `span<T>` を chunk ごとの callback に渡し、callback 後に pointer を保持させない。
- low-level chunk batch: `count <= remaining_capacity` を precondition として private に置く。

4095/4096/4097、100,000、既存 free slot と fresh slot が混ざる batch、途中の k 番目で
constructor/populate/init が throw する batch を受け入れテストにする必要がある。

## 5. S1 — EntityId の状態機械と stale policy

### 5.1 default construction は invalid でなければならない

v2 は invalid を `index == UINT32_MAX` と決めたが、`EntityId{}` の値を決めていない
(`docs/design_ecs_lifecycle.md:101-109`)。単純な aggregate `{uint32 index, uint32 generation}` の
既定値が `{0,0}` なら、value-constructed `LocalTransformComponent::parent` が最初の entity を
指す。`parent` は serializer に含まれていないため (`src/core/userpublic/components/localtransform.hpp:10-20`)、
scene load でもこの値が残る。

canonical 定義は一箇所に置き、少なくとも次を固定するべきである。

```cpp
struct EntityId {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
};
using GameObjectId = EntityId;
```

`EntityId{} == invalidGameObjectId`、`sizeof(EntityId) == 8`、trivially-copyable、比較/hash/表示を
test する。`GameObjectId` と `EntityId` は似た二 struct ではなく同じ canonical 型/alias にする。

### 5.2 generation 遷移を疑似コード相当まで固定する

live remove/clear は ref を無効化して `live=false` とし、`generation == UINT32_MAX` なら retire、
それ以外は generation を一度だけ増やして free-list へ積む。allocate は free slot の generation を
増やさず、その値を handle に写す。`id_table.size() == UINT32_MAX` では invalid index を採番せず
明示的に capacity error とする。clear の走査順と push 順も index 順など一つに固定する。

§3-2 は「全 live の generation を進める」を規範にしているのに、§8.2 は world epoch をなお
未決としている (`docs/design_ecs_lifecycle.md:114-120`, `:175-180`)。32/32 と retire の証明が
変わるので、R5-core では前者に決定し未決から外すべきである。

### 5.3 stale remove = no-op + bool は妥当

この選択は deterministic で、cleanup の idempotence にも向くため賛成する。通常 API に
unconditional debug assert を足すと debug/release の有効入力が変わるので推奨しない。
代わりに `[[nodiscard]] bool`、任意の diagnostic counter/log、厳格さが必要な test 用の
`removeOrThrow` を使うのがよい。

ただし bool は最下層だけでは役に立たない。現行 public API はすべて `void` である
(`src/core/ecs/core.hpp:22`, `src/core/userpublic/gameobjects.hpp:119-124`,
`src/core/userpublic/gamecontext.hpp:36-39`)。三層を bool に変更し、invalid/out-of-range/dead/
generation mismatch はすべて `false`、live removal のみ `true` とする。

remove 以外も表にするべきである。推奨は `tryComponent` = null、必須 component accessor =
名前/ID 入り例外、set/mark = 失敗を返すか例外、PhysWorld = skip+prune、RPC = object 名入り
application error である。これにより「全経路 resolve」が単なる実装標語ではなくなる。

## 6. S1 — PhysWorld/SceneLoader binding の資源と名前

`variant<GameObjectId, PhysWorldTransform>` は現行 `Binding` 全体を置換するのではなく、
`transform` pointer と `static_transform` の二フィールドだけを置換する。現行の duplicate check、
collider ID、raycast 結果は `Binding::name` に依存する
(`src/core/phys/physworld.hpp:32-40`, `src/core/phys/physworld.cpp:215-246`)。設計に次の形を明記すれば
現行の名前検索と整合する。

```cpp
struct Binding {
    std::string name;
    ColliderComponent collider;
    std::variant<GameObjectId, PhysWorldTransform> transform_source;
};
```

static transform は値で保持し、entity branch だけ query ごとに resolve する。dead ID の遅延 prune は
`collectColliders() const` のどこで mutation するか、また stale binding と同名の新 binding を
追加するときに古い entry を先に prune するかを決める。そうしないと query 前の名前再利用が
duplicate error になる。

SceneLoader は現在どおり名前を map key とし、value を ID 一つにすればよい。ただし
`hasObjectTransform` も map membership だけでなく resolve し、dead ID は false/prune、
`objectTransform`/`applyObjectTransform` は名前入り stale error にする。RPC は parse 時と
flush 時の間にも entity が消え得るため、flush 時 resolve も必須である
(`src/core/communication/rpcserver.cpp:180-215`)。

## 7. S1 — teardown は例外経路を含める

明示 teardown phase の採用は正しい。ただし現 `PelicanCore::run()` では module container が
`try` block 内の local であり、scene load/loop が throw すると catch へ入る前に container が
逆順破棄される (`src/core/userpublic/pelican_core.cpp:27-45`, `src/core/container.hpp:25-43`)。
単に `loop.run()` の次へ `removeAll()` を足すだけでは異常終了時に実行されない。

teardown は scope guard/runner の no-throw phase とし、少なくとも次を満たすべきである。

1. 正常 window 終了、headless 終了、RPC 終了、initial scene load 失敗、loop/update/render 例外の
   すべてから一度だけ呼ばれる。
2. job/GPU の利用停止・wait と ECS clear の順序を決め、deinit が使う renderer 等は clear 完了まで
   生存させる。
3. teardown 自体は throw せず、失敗は記録して残り component/module の cleanup を継続する。

例外を注入し、`deinit → destroy → dependency module destroy` のイベント列を確認する test が必要である。

## 8. benchmark と性能予算

§8.1 の benchmark 削除案には**反対**する。raw registration の最後の利用者を消す目的は、
benchmark 自体を消さず typed metadata builder と新 bulk API へ移せば達成できる。むしろ現 benchmark は
数万件 workload を持つ唯一の資産であり、値構築と resolve 導入の回帰を測る材料である。

ただし現 benchmark は capacity を超える一括 allocation を行い、correctness assertion もないため、
その数値をそのまま baseline にしてはいけない。次のように書き直す。

- Release build、固定 seed、warm-up 後の複数回計測。単一の最速値ではなく median/p95 を保存する。
- 1/4096/4097/100,000 entity、POD-only、`std::string`、over-aligned、free-list 再利用混在を分ける。
- construct+populate+init、remove/relocate、resolve hit/miss、PhysWorld query を別々に測る。
- 全件の ID uniqueness/liveness、初期値、component 値も assert し、速度だけの executable にしない。
- R5 実装前に有効な bounded/chunked baseline を取り、許容率または絶対 budget を WP に記録する。

全 component の値構築は object lifetime のため必要で、数万件 batch だから未初期化へ戻す選択肢はない。
POD の value-init は主にメモリ帯域コストになるため、まず range construction と chunk 分割を計測し、
必要なら C1 の明示 opt-in 高速路だけを追加する。`resolve` は `id_table[index]` の O(1) 添字アクセスであり、
§8.3 の「ハッシュ/添字コスト」のうち hash は hot path に入れない方がよい。

## 9. GameObjectId struct 化の移行漏れ

v2 が v1 §3.3 の一覧を WP 作業リストにするとした点は有効で、example の `= 0`、parent の `= 0`、
implicit EntityId component/query、ECSCore の添字アクセス、benchmark、SceneLoader の二作成経路は
拾えている。ただし v2 で新しく選んだ API により、以下を追加する必要がある。

1. `src/core/userpublic/details/ecs/entity.hpp` を canonical 定義元にし、
   `src/core/userpublic/gameobjects.hpp:15-17` の別 alias を撤去する。default-invalid もここで固定する。
2. `ECSCore::remove`、`GameObjects::remove`、`GameContext::removeObject` の bool 伝播と、その呼出側。
3. `allocateEntity(count)` の戻り値と `src/core/ecs/benchmark.cpp` の「first ID/連続 pointer」前提。
4. `PhysWorld::bindCollider` の pointer 引数と、pointer を直接 bind している
   `test/physworld_test.cpp:144-173`。この test は ECS entity の ID を bind し、component を書き換えた
   直後の hit を検査する形へ移す。static branch の独立 test も残す。
5. `LocalTransformComponent{}` の parent が invalid になる test。明示 `.parent = 0` の二箇所を直すだけでは
   scene serializer 経路を守れない。
6. typed 登録の非 serializable overload。現 `registerComponent<T>` は
   `ISerializable<T, JsonArchiveLoader>` を要求する
   (`src/core/userpublic/details/component/registerer.hpp:24-45`)。
7. `deinit noexcept` の宣言だけでなく、その呼出先も含む監査。
   `SimpleModelViewComponent::deinit` は `PolygonInstanceContainer::removeModelInstance` を呼ぶため
   (`src/core/ecs/predefined/modelview.cpp:8-12`)、callback type と transitive operation の
   no-throw 性を揃える。

## 10. §7 へ追加する受け入れテスト

- trivially-default-constructible な `int` member が populate なしでも `T{}` の値になる。
- move-only/non-copy-assignable component の relocation と、builder API の compile-time 制約。
- lifecycle callback から allocate/remove/clear を再入させ、変更前に拒否され state が一致する。
- batch の chunk 境界、free/fresh ID 混在、各 phase の k 番目 fault と完全 rollback。
- `EntityId{}`/parent の invalid、remove/clear 後の generation、MAX generation retire、index 枯渇。
- stale remove の全分類と `[[nodiscard]] bool`、stale accessor policy。
- dead PhysWorld binding の skip/prune、同名 rebind、immediate visibility、static branch。
- 正常終了と例外終了の teardown 順序。
- alignment test は ASan だけでなく UBSan の alignment check でも実行する。MSVC の
  `/fsanitize=address` は UBSan ではないため、Clang/GCC 系の undefined-behavior sanitizer job を
  別に用意する。

以上の条件を設計本文と R5-core の受け入れ条件へ入れれば、v1 の二つの S0 と PhysWorld S1 は
実装可能な形で解消される。特に C1～C4 はコードレビュー時の解釈に委ねず、実装前の規範にすること。
