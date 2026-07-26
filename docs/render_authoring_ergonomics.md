# 描画 authoring の使い勝手と回りくどさの棚卸し(v1)

対象読者: エンジン担当、および feature / material / シェーダをユーザー空間で書く人。

ステータス: v1(2026-07-19)。実コード実測に基づく指摘。関連文書:
[`render_mechanism_coverage.md`](render_mechanism_coverage.md)(**何が書けるか**の分析。
本書は**書けるものが書きやすいか**の分析)。

## 0. この文書の位置づけ

カバレッジ分析(前掲)は「技法がユーザー空間で到達可能か」を判定した。本書はその一段
手前、**到達可能なものを実際に書くときの摩擦**を扱う。指摘は 2 系統に分ける。

- **A. 使い勝手** — feature / material を書く人が実際に踏む問題
- **B. 回りくどさ** — 内部設計の構造的な重さ(読む・変更するコスト)

各項目に根拠(file:line)と直し方を付けた。優先順位は §C。

## A. 使い勝手の問題

### A-1. feature を 1 個でも使うと出荷ビルドで動かない ★最重要

feature インスタンスが 1 つでも存在し、`runtime_shader_compiler_enabled` が false の
場合、composer は例外を投げる。

- 根拠: [featurecompose.cpp:1581](../src/project/featurecompose.cpp:1581)
- 一方 `dist-bake`(バリアント焼き出し)は未実装であることを
  [design_build_tiers.md:99](design_build_tiers.md) が自認している

**帰結**: shaderc を積まない配布構成では、**feature を使うプロジェクトが起動できない**。
「feature 層 = ユーザー空間」という理念の成果物が出荷物に乗らないため、理念と実装の
乖離としては本書で最大である。開発ビルドで作った絵が配布できない、という形で必ず
表面化する。

**直し方**: `dist-bake`(stem × 実使用 define 集合の事前焼き)を実装する。規模は大きい
が回避不能。暫定策として「feature 使用時は dist 構成で明示エラー」ではなく
**「dist 構成を選んだ時点で必要な variant 一覧を出力する」**だけでも実害が減る。

### A-2. `material_range: {start, count}` でオブジェクトを選ぶ

material パスの描画対象は material の**内部インデックス範囲**で絞る。

- 根拠: [materialpassinfojsonparser.cpp:30](../src/core/renderingpass/materialpassinfojsonparser.cpp:30)

**帰結**: ユーザーは自分のマテリアルが何番になるか知る手段がなく、マテリアルを 1 つ
足すと範囲が全部ずれる。生インデックスを公開 API にした典型的な悪例。輪郭線パス・
キャラのみの SSS パス・反射に映すオブジェクト選別など、**選択的パスを書く技法すべての
足を引っ張る**(カバレッジ分析の G14)。

**直し方**: layer / tag による選別を導入し、`material_range` は内部専用に戻す。
tag は material 側の宣言(`"tags": ["character", "outline"]`)+ パス側の
`"draw_tags": ["character"]` で足りる。

### A-3. RT サイズが「誰かが display と書いたか」に暗黙依存

`extent_scale` の基準寸法は、`format_class: "display"` を持つ RT から拾われる。

- 根拠: [frameplanner.cpp:589](../src/core/renderingpass/frameplanner.cpp:589)〜605

**帰結**: 自分の RT の解像度が**他の RT の属性**で決まる。config を読んでも基準が
どこから来たか追いにくく、display 指定を持つ RT を消すと無関係な feature の解像度が
壊れる。

**直し方**: 基準を明示させる — `"extent_of": "swapchain"` / `"extent_of": "scene_color"`
+ `"extent_scale": 0.5`。既定値だけ現行動作に合わせれば互換で入る。

### A-4. descriptor binding 番号を手計算させている

custom texture の binding は算術で決まる。

- 根拠: [shader_contract.md:206](shader_contract.md) — split sampler は
  `set 2 binding 7+2i` / sampler `8+2i`、combined fallback は `7+i`

**帰結**: C 層(生シェーダ)を書く人が宣言順から binding 番号を計算する。1 個挿入すると
以降が全部ずれ、しかもズレは実行時の壊れた絵として現れる(コンパイルは通る)。

**直し方**: 生成アクセサ `<stem>.params.glsl` の方針は既に設計にあるので、**binding
定数もそこに生成する**(`#define MY_TEX_BINDING 9` 相当)。手計算を消す。

### A-5. insert が文字列ミニ言語 + 位置挿入で、順序が暗黙

`"insert": "before:lighting_pass"` を parse し、pass 配列へ positional insert する。

- 根拠: [featurecompose.cpp:551](../src/project/featurecompose.cpp:551)、
  `insert:end` のみ「パスちょうど 1 個」の特殊規則
  [featurecompose.cpp:1378](../src/project/featurecompose.cpp:1378)

**帰結**: **同じアンカーに 2 つの feature が挿した場合、順序は config の feature 宣言順に
暗黙依存**する。TAA と bloom の前後関係を決めたい人が「features 配列の行順」で戦う。
順序が config の別の場所に隠れているため、feature 単体を読んでも挙動が確定しない。

**直し方**: 明示順序(`"order": 100`)を持たせ、**同一アンカー・同一 order の衝突は
起動時エラー**にする。暗黙順序に依存した挙動を作らせない。

### A-6. compute と graphics で依存宣言の文法が違い、互いに繋がらない

| 種別 | 依存の宣言方法 |
|---|---|
| compute task | `reads[]` / `writes[]` / `after[]` / `before[]` |
| graphics pass | `input[]` / `output{}` / `insert` |

- 根拠: [computetask.cpp:342](../src/core/renderingpass/computetask.cpp:342) と
  feature の pass 宣言([shadow_directional.json](../src/core/resources/features/shadow_directional.json))

**帰結**: 同じ「依存」の概念に語彙が二重にあり、覚える量が倍になる。さらに致命的なのは
**両者が接続できない**(compute が書いた buffer を graphics パスが読めない = カバレッジ
分析の G2)。この非対称が G2 の原因そのものである。

**直し方**: 依存語彙を `reads` / `writes` に統一し、pass の `input`/`output` はその糖衣と
する。**同時に graphics 側の buffer input を開放**すれば、使い勝手の改善と機構ギャップの
解消が 1 つの WP で片付く。

## B. 回りくどさ(内部設計)

### B-1. 「計画」を表す型が 160 個

`src/project` + `src/core/renderingpass` のヘッダ 23 本のうち、名前に
plan / graph / pipeline / pass / program / request / resolved / compiled を含む型が
**160 個**。`Compiled〜` が 19 種、`FrameGraph〜` が 8 種。

**帰結**: 層の分離自体は正しい設計判断だが、**層の数だけ名前が増える**運用になっている。
1 つの意味論変更が何段の型変換を通るのか追うコストが高く、新規参入(将来の自分を含む)
の障壁になる。

**直し方**: 型を減らすのは危険なので、**層ごとの命名規約を固定し索引を 1 枚作る**方が
現実的(`Logical〜` / `Target〜` / `Vulkan〜` / `Compiled〜` の 4 接頭辞に整理し、
どの層の語彙かを名前で判別できるようにする)。

### B-2. ほぼ同名の enum が 2 つ、変換は各所で inline

- `VulkanScopeViewExecution { single_view, sequential, multiview }`
  ([vulkanviewplanning.hpp:28](../src/project/vulkanviewplanning.hpp:28))
- `GraphicsPipelineViewExecution { single_view, multiview }`
  ([graphicsviewcontract.hpp:8](../src/core/shader/graphicsviewcontract.hpp:8))

層が違う(scope は `sequential` を持つ)ので存在は妥当だが、**変換関数が無く各所で
case を書いている**([vulkanviewplanning.cpp:150](../src/project/vulkanviewplanning.cpp:150)
等)。読むたびに「今どちらの view execution か」を確認させられる。

**直し方**: 名前で層を明示(`ScopeViewExecution` / `PipelineViewExecution`)し、
変換を 1 箇所の関数に集約する。小さいが読み負荷への効果は大きい。

### B-3. 設計文書の status 欄が WP 台帳になっている

`design_render_pipeline_extensibility.md` の冒頭は RPE6b0 / 6b1 / 6c0 / 6c1 / 7 / 8 / 9 /
10a / 10b1 / 10b2 / 10b3 / 11a / 11b / 12a Phase A … と実装履歴の羅列。
`design_render_graph_compiler.md` と `design_heterogeneous_execution_graph.md` も同様で、
**3 文書が互いに「あちらが正」と宣言し合っている**。

**帰結**: 「今の契約は何か」を知るために実装履歴を読まされる。入口が存在しないため、
新しく feature を書こうとする人が最初に迷子になる。

**直し方**: 実装台帳で既に行った **active / archive 分離と同じ処置**をする。本文は
現在形の契約だけ、履歴は `docs/design_reviews/` か archive セクションへ。加えて
**「feature を書くならまずこれ」の 1 枚**(authoring リファレンス)を新設する。

### B-4. 理念と実装のズレ: pass kind は今も閉じた if 連鎖

heterogeneous 設計の決定 6 は「execution domain を閉じた enum にしない」だが、実体は
`makePassInfo(type_str)` の if 連鎖で、未知 type は `Unknown pass type` で throw。

- 根拠: [renderingpassjsonhelpers.cpp:83](../src/core/renderingpass/renderingpassjsonhelpers.cpp:83)
- material contract も 4 種の閉集合
  ([renderpipeline.hpp:46](../src/project/renderpipeline.hpp:46))

`PassImplementation` registry(RPE11a)で**実装**は差し替え可能になったが、**種類**は
依然エンジン専管である。宣言(設計文書)と実態(コード)が食い違っている箇所として
記録しておく。

**直し方**: どちらかに揃える。①実際に pass kind を開く(registry 化)か、②設計文書
側で「v1 では pass kind は閉集合、開くのは実装差し替えまで」と**現状を正しく書く**。
②のほうが安全で、①は需要が出てから。

## C. 優先順位(効果 ÷ 労力)

| 順 | 施策 | 対象 | 規模 | 効果 |
|---|---|---|---|---|
| 1 | **feature authoring リファレンス 1 枚**(現在形のみ)+ 3 設計文書の履歴を archive へ | B-3 | 半日 | 入口の復活。今すぐ効く |
| 2 | **insert の明示順序 + 衝突エラー** | A-5 | 小 | 暗黙依存の除去 |
| 3 | **`extent_of` の明示指定** | A-3 | 小 | 暗黙結合の除去 |
| 4 | **binding 定数の生成**(`<stem>.params.glsl` に含める) | A-4 | 小 | 手計算事故の根絶 |
| 5 | **view execution enum の改名 + 変換集約** | B-2 | 小 | 読み負荷 |
| 6 | **依存語彙の統一 + graphics の buffer input 開放** | A-6 / G2 | 中 | 使い勝手と機構ギャップを同時解消。**機構側でも最優先候補** |
| 7 | **layer / tag による選別** | A-2 / G14 | 中 | 選択的パス技法が解禁 |
| 8 | **設計文書の pass kind 記述を実態に合わせる** | B-4 | 小 | 宣言と実装の一致 |
| 9 | **dist-bake** | A-1 | 大 | 理念の実効化。避けられない |
| — | 型命名規約の整理 + 索引 | B-1 | 中 | 中長期の保守性 |

**1〜5 の合計で 1〜2 日**。ここだけで「触りにくさ」の主要因が消える。6 は
カバレッジ分析側でも解禁数最大の項目なので、機構拡張と使い勝手改善の交点として
最も投資効率が良い。

## D. 判定の限界

- 静的読解のみ。実際に feature を書くと別の摩擦(エラーメッセージの分かりにくさ、
  失敗時の診断情報の粒度)が出る可能性が高い
- material 側の authoring(`.surface` の書き味)は本書では未評価。B 層 hook の
  使い勝手は実際に BRDF を 1 本書いて確かめるのが早い
- エラーメッセージの質は本書の対象外だが、`Unknown pass type` のように
  **候補一覧を出さない**メッセージが散見される。authoring リファレンス整備と
  同時に見直す価値がある
