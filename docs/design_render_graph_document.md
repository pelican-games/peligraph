# TypedRenderGraphDocument 設計 —— render graph の parse 一本化(v1)

状態: **v1 草稿。敵対的設計レビュー前**(orchestration.md §A: 最大 3 巡 → 決定差分)。
台帳の R1 正本は**上段(設計 + 仕様 + コード)**を要求しており、
本文書がその設計段である。**設計レビュー合格前の実装着手を禁止**。

## 0. 要求

同じ rendering config JSON を読むパーサを**一つ**にする。現状は
`PassDefinition` 系 typed parser と、`frameplanner.cpp` 内の raw JSON parser
(`requireString` / `parseStringList` ほか)が**同じ文書の第二のパーサ**であり、
WP303・WP326 で 2 回、実バグを素通りさせた形そのもの。

恒常制約: D0(`pelican_project` に Vulkan 型を持ち込まない)/ fail-fast /
出荷 4 project の frame plan・golden・cache key **byte 不変** / パージ性を下げない。

## 1. 仕様の種(WP371 v1 レビュー。全て台帳に原文あり)

- **致命の循環**: `PassDefinition` は**物理 target plan 確定後**に、concrete metadata
  (sample count・array layer・format)を使って構築される runtime 型
  (renderingpassconfigregistration.cpp:423 のコメントが明記)。
  planner の入力にはなれない —— **field 追加では解けない**
- **表現力不足**: `PassDefinition` は compute / anchor / snapshot node を表せない。
  出荷 example に **snapshot node 2 つ**(main_rendering_config.json:217)、
  `clustered_lighting` は出荷 core resource の compute node
- **raw API の呼び手は planner 本体以外に 3 つ**: production compiler
  (vulkanrendercompilerprogram.cpp:620)/ graph transform registry(:332)/
  subgraph replacement registry(:337)。それに公開 raw entrypoint
  (frameplanner.hpp:162)
- **WP354 fixtures はオラクルにならない**: `familyFramePlan()` は 15 node を
  手で構築しており planner/parser を通らない。`framePlanToJson` は footprint・
  resource access・attachment・region・resolution domain を出力しないが、
  それらは lowering が消費する
- **旧拒否 → 新受理の既知反例**: `"after": 7`(raw は非 string/array を拒否、
  typed は `after` を読まない)/ `"input_footprints": {"ghost": ...}`(raw は
  current read でない名前を拒否、typed は無視)/ compute の `read_footprints` 同様
- planner が必要とするが typed 側に無いもの(レビューの棚卸し表):
  authored resource 名・declaration index・`after`/`before`・anchor/snapshot node・
  compute node・footprint override・`region_tags`・(fullscreen の)resource access 反映

## 2. 設計

### 2-1. 背骨: 二段構造

```
JSON ──(一度だけ parse: pelican_project)──▶ TypedRenderGraphDocument
                                               │
                                               ▼ planner(論理)
                                        physical target plan
                                               │
                                               ▼ lowering(core)
                          runtime PassDefinition / ComputeTaskDefinition
                          (concrete metadata はここで初めて注入)
```

- **`TypedRenderGraphDocument` は `pelican_project` 所有・Vulkan 非依存**。
  内容: resource 宣言(名前・format 文字列・usage・history)+
  **宣言順を保った `variant<render, compute, anchor, snapshot>` の node 列** +
  graph 級 field(`after`/`before` edge・declaration index・footprint・
  `region_tags`・resource_ports・snapshot byte size)
- **既存の `PassDefinition`/`ComputeTaskDefinition` は消さない**。役割を
  「物理 metadata 注入後の runtime 型」に純化し、**document からの lowering でのみ**
  生成する(第三の流儀を作らない)
- graph transform / subgraph replacement の provider は **document を入出力**する
  (JSON patch をやめる)。raw 公開 entrypoint は削除

### 2-2. 検証の合併(strictness 表)

raw parser と typed parser の**検証の和集合**を document parser が引き継ぐ。
とくに既知反例 3 種(`after` の型 / footprint の未知名 / compute の
`read_footprints`)は**表として列挙**し、旧と同じ named 違反で拒否する。
「typed が黙って無視していた field」の一覧を作り、**全てを読み取るか named 拒否か**の
どちらかに割り当てる(黙殺を残さない)。

### 2-3. RT 車線との関係

RT1b〜RT2b の layout / hit_shaders / ray_bindings の解析は既に
project/leaf 側に分離済みであり、**本設計の範囲外**(壊さないことを受け入れで確認)。
`compute_tasks` の node 化は「既に CPU phase にある `ComputeTaskDefinition` を
document の node として参照する」形にし、**JSON を再読しない**。

### 2-4. wp336 との関係

保留中の worktree wp336(shader 宣言 footprint)は本設計の隣接領域だが範囲外。
document は現行の `input_footprints`/`read_footprints` を**意味そのまま**運ぶ。
footprint 3 経路の統合はソケット整理(台帳 6117 行)の側。wp336 は参考実装。

## 3. 出荷単位(段割りの目安)

- **G1**: document + parser + planner の consume + raw parser の物理削除 +
  3 caller の移行。**挙動は byte 不変**(これ自体が受け入れ)
- **G2(= R2、台帳の裁定どおり G1 直後)**: project 層での深い検証
  (現在 runtime まで行かないと出ないエラーを load 時 named に前倒し)

G1 は「挙動を変えない基盤」に見えるが、**旧拒否→新受理の反例 3 種を named で
閉じる**ので挙動変更を含む(縦切り成立)。

## 4. 受け入れオラクル(必須)

- **exact fixture 8 件を名前付きで凍結**(frameplanner_test の既存 8 件。
  更新用環境変数は受け入れ実行では禁止)+ **不足分を追加**:
  compute node・RT compute・anchor・snapshot・footprint・resource port・XR
- **段ごとの exact 比較**: TypedRenderGraphDocument → CompiledLogicalRenderGraph →
  physical target plan → execution plan → frame plan → cache key を**別々に**
  親版と比較(frame plan だけの比較は footprint 脱落を見逃す —— レビュー実証済み)
- **等価性は両方向**: 旧拒否→新受理(反例 3 種 + 探索)と**旧受理→新拒否**
  (過剰拒否。正当な近傍入力を同じテストで)
- 出荷 4 project + core resources(features 18 + render_pipelines)の全 config を
  実 parser に通し、frame plan・GPU golden・cache key が親版と一致
- 削除の実測: raw parse 関数群が物理的に消え、typed 経路を無効化すると
  planner が動かない(ソース文字列検査は禁止)

## 5. 未決事項(レビューで攻撃してほしい点)

1. document の node variant に **material pass の特殊性**(material_resources 等)を
   どこまで載せるか —— ソケット整理(別設計)との境界線
2. graph transform provider の入出力を document 化する際、既存 provider の
   後方互換(登録 API の変更範囲)
3. `framePlanToJson` の出力拡張(footprint 等)を G1 でやるか G2 でやるか ——
   拡張しないと段ごと比較の一部が wire に乗らない
4. XR の array-layer assignment が lowering 側に残ることの確認
   (document は論理のみ、という境界が実データで成立するか)
