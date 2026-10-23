# 段(canonical anchor)を canvas のフィルター階層へ: 合成・plan のノードから「見せ方の層」へ

対象読者: エンジン担当(描画コンパイラ)と Studio 担当。
ステータス: 改善案メモ(2026-10-16。議論の記録。WP 化・レビュー前)。
前提: マニュアル [第6章 canonical anchor](manual/06_rendering.md#canonical-anchor)、
[第15章 Graph タブ「領域の filter」「下の層を node で見る」](manual/15_studio.md)、
[design_graph_layers.md](design_graph_layers.md) §7(GL-1 の `Show:`、GL-2b の帯と Layers)、
[design_render_preset_feature_recipe.md](design_render_preset_feature_recipe.md) §3 点 1(scene 段の anchor)、
[design_render_pipeline_extensibility.md](design_render_pipeline_extensibility.md)。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- canonical anchor(`sprite` / `post_main` / `tonemap` / `post_ldr` / `pelican_ui` / `debug_draw` / `debug_text` / `imgui`)は
  いま **5 つの役割を 1 つの仕組みで兼ねている**: 挿入位置の名前、フレーム順の骨格、HDR / LDR の境界、`__anchor_sprite` の実仕事、
  runtime の anchor ノード実行。plan には `__anchor_*` の 8 ノードが常に並び、GPU 時間の表にも出る。
- このうち**残る価値は「どの段の pass か」で探す・まとめる・隠すというナビゲーション**で、それは合成や plan のノードではなく
  Studio の Graph タブの **`Show:` フィルター(領域: Render / Pose / Value / Audio / UI)の、Render の下の階層**に置くのが正しい。
  Render だけにしたとき帯と Layers が出る構造が既にあり、段はその並びに入る。
- 提案: pass に **`stage` タグ**(値は今のアンカー名 + scene 段)を持たせ、(1) `Show: Render ▸ <stage>` の二段フィルターと
  Frame Plan の帯分けに使い、(2) `insert:` を省いた pass の既定の置き場に使う。**順序は資源依存と `frame_node_order` / 宣言順が決め、
  HDR / LDR の境界は資源の型が守り、`__anchor_sprite` は sprite feature の本物の pass になり、runtime の anchor ノードは消える。**
- 段が見せ方だけになると、「段を跨いで並べられない」(アンカーの重い点 (a))も「段のノードが plan を汚す」も無くなる。
  scene 段の pass 名結合(`sky_ambient` の `after:deferred_lighting` など)は本案では直らず、型付き資源の仕事のまま。

## 1. 今のアンカーの役割と証拠

| # | 役割 | どこで | 本案での行き先 |
|---|---|---|---|
| 1 | **挿入位置の名前**: feature の `insert: after:post_main`、authored pass の `canonical_anchor` | [`featurecompose.cpp:34`](../src/project/featurecompose.cpp#L34) のアンカー表、[`insertRenderPassRunAtAnchor()`](../src/project/featurecompose.cpp#L2302)、[`canonicalBucket()`](../src/project/featurecompose.cpp#L297) | `stage` タグ。`insert:` が無い pass の既定の置き場 |
| 2 | **フレーム順の骨格**: 8 bucket に振り分けて並べ、`enforceCanonicalOrder` が宣言順を近全順序に鎖で結ぶ | [`canonicalizePasses()`](../src/project/featurecompose.cpp#L486)、[`enforceCanonicalOrder()`](../src/project/featurecompose.cpp#L411) | 資源依存 + `frame_node_order`(FO-1)/ 宣言順。段は順序を決めない |
| 3 | **HDR / LDR の境界**: swapchain を書く pass は hdr on で bucket 1(post_main)、off で bucket 3(post_ldr)。`scene_ldr_in` の差し込み | [`canonicalBucket():321`](../src/project/featurecompose.cpp#L321)、[`initializeCanonicalColorPipeline()`](../src/project/featurecompose.cpp#L692) | 資源の型(`pelican.render.color_signal@1` と LDR 側の型)。型不一致は今まで通り名前付きエラー |
| 4 | **`__anchor_sprite` の実仕事**: anchor ノードの実行時に sprite scene を描く | [`renderer.cpp:2409`](../src/core/vkcore/renderer.cpp#L2409) | sprite feature の本物の pass(`sprite_draw`)。入力は最新の scene color / depth を資源として受ける |
| 5 | **runtime の anchor ノード**: plan の `kind = anchor`、実行列に載る、GPU 時間の `anchor_has_work` | [`frameplanner.cpp:736`](../src/core/renderingpass/frameplanner.cpp#L736)、[`framegraphruntime.cpp:202`](../src/core/renderingpass/framegraphruntime.cpp#L202)、[`renderer.cpp:1393`](../src/core/vkcore/renderer.cpp#L1393)、[`vulkanrendercompilerprogram.cpp:144`](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L144) | 廃止。pass は全部 plan のノードなので置き場のノードは要らない |

- アンカーの全順序は「エンジンの契約で並べ替え不可、挿すのは自由」([第6章](manual/06_rendering.md#canonical-anchor))。
  設計決定(2026-07-12)では機構語彙のひとつとして**凍結面**([同 §設計決定](manual/06_rendering.md))。
- GPU コストは無い(anchor ノードは command を出さない。`__anchor_sprite` だけ draw)。重いのは**柔軟性**: 段を跨ぐ順序が
  書けない、段ごとの一括ノードが plan と Frame Plan に常に出る、`loop.canonical_anchor` と `insert` が排他で
  ([`graphcanvasmodel.cpp:4270`](../src/devstudio/model/graphcanvasmodel.cpp#L4270))「段」と「位置」が同じ軸に混ざる。

## 2. 今の `Show:` フィルターの構造(受け皿)

- 領域は [`GraphCanvasModel::domains_shown_`](../src/devstudio/model/graphcanvasmodel.hpp#L1395) の 5 つ。node は
  [`opDomain(op)`](../src/devstudio/model/graphcanvasmodel.cpp#L6984) で op の接頭辞から領域に振り分けられ、
  [`nodeShown`](../src/devstudio/model/graphcanvasmodel.cpp#L7012) が `domainShown(opDomain(node.op))` で出し分ける。
  状態はセッションだけ、文書には書かない([第15章「領域の filter」](manual/15_studio.md))。
- 隠した領域へ渡る辺は [`domainStubs()`](../src/devstudio/model/graphcanvasmodel.cpp#L7019) が領域名のスタブに切る。
- **Render だけにすると** [`layersShown()`](../src/devstudio/model/graphcanvasmodel.cpp#L6486) が立ち、node の下に
  [`layerBand()`](../src/devstudio/model/graphcanvasmodel.cpp#L6496) の帯(copies / barriers / aliased / backend / fold の決定 / deactivated / GPU 時間)と
  Layers 節が出る。つまり Render 領域の下には**既にもう一段**があり、そこは「plan で何になったか」を見せる層。
- loop には [`loopAnchors()`](../src/devstudio/model/graphcanvasmodel.cpp#L4262) / `setLoopAnchor()` という
  「どの段に住むか」の UI 概念が既にある。これは段タグそのもの。
- Frame Plan は anchor ノードを [`isAnchor()`](../src/devstudio/view/frameplangraphics.cpp#L371) で見分けて枠線だけ変えて描く。
  段ごとの**まとまり**としては描いていない。

## 3. 提案

### 3.1 データ: pass の `stage` タグ

```text
stage の値(フレーム順。今のアンカー名 + scene 段)
  scene_geometry | scene_lighting | scene_forward      … 新設(preset レシピ化メモ §3 点 1 の scene 段)
  sprite | post_main | tonemap | post_ldr | pelican_ui | debug_draw | debug_text | imgui   … 今のアンカー名をそのまま

書く場所
  feature の pass:   feature JSON の pass に "stage"。無ければ feature の既定(feature 全体に 1 つ)
  authored pass:     今の "canonical_anchor" を "stage" に改名(v1 の読み替え。v0 では別名として受ける)
  preset の pass:    preset 側で宣言
  無いとき:          合成が導く。`insert: after:<pass>` なら相手の段、swapchain を書くなら tonemap 以降の型から、
                     それも無ければ `unstaged`(フィルターでは Render の最後に「Unstaged」として出す)

意味
  - フィルター・帯分け・既定の置き場に使う **presentation + placement の属性**
  - 順序の制約ではない。段の順と資源依存が食い違っても、依存が勝つ(段の順は Studio の並び順の既定でしかない)
```

### 3.2 compose(`src/project/featurecompose.cpp`)

- `insert:` を省いた pass は、自分の `stage` の既定位置(その段の宣言済み pass の末尾)に入る。`insert: after:<pass>` は今まで通り。
  `after:<stage名>` は「その段の末尾」の意味で残す(`after:post_main` を書く既存 feature を壊さない)。
- `__anchor_*` ノードは作らない。`canonicalizePasses` の bucket 振り分けは「段の既定位置の計算」として残り、
  `enforceCanonicalOrder` の鎖(宣言順 → 近全順序)は **FO-1 の `frame_node_order` に一本化**する
  (段の中も段の間も、明示順 + 資源依存で決める。段の境界に暗黙の鎖を張らない)。
- HDR / LDR の境界は `canonicalBucket():321` の「swapchain を書くなら hdr で bucket 1/3」をやめ、
  pass の入出力の型で決める。`scene_ldr_in` の差し込みは tonemap feature 側の `required_targets` へ。
- `__anchor_sprite` は sprite feature の pass になる。`renderer.cpp:2409` の「最新の scene color / depth を掴む」は
  資源の参照(`latest_scene_color` 相当を `render.target_ref` で受ける)に置き換える。

### 3.3 plan / runtime

- `FramePlanNodeKind::anchor` を落とす。`frameplanner.cpp:736`、`framegraphruntime.cpp:202`、`renderer.cpp:1393/2405`、
  `vulkanrendercompilerprogram.cpp:144/428` の分岐が消える。GPU 時間の表から `__anchor_*` の行が消える。
- `get_frame_plan` のノードに `stage` を**属性として**載せる(`order` と並ぶ 1 鍵)。Studio と web 鏡像はこれで帯分けする。
  `frame_node_order` の UI(Order…)は段の中の既定順を段順 + 宣言順で出し、跨ぐ指定も受ける。

### 3.4 Studio

- Graph タブ: `Show: Render ▸ [Scene] [Sprite] [Post main] [Tonemap] [Post LDR] [UI] [Debug] [Unstaged]` の二段フィルター。
  領域の check の下に、Render が on のときだけ段の check が並ぶ(Scene は 3 段をまとめた 1 check でよい)。
  隠した段へ渡る辺は `domainStubs()` と同じ規則で段名のスタブに切る(キーを `opDomain` から `stage` に替えるだけ)。
  状態はセッションだけ、文書には書かない(領域と同じ)。
- 帯(`layerBand`)の先頭に段名を出す(既定と同じなら出さない、という帯の流儀に合わせ、`unstaged` のときだけ出す案もある)。
- Frame Plan: Logical graph を段ごとの帯(swimlane)に分ける。anchor ノード専用の描き分け(`isAnchor`)は不要になる。
  Physical plan はそのまま。
- loop の `canonical_anchor` フィールドは `stage` に改名し、`insert` との排他を外す(段と位置は別の軸)。

## 4. 互換と凍結面

- アンカーの**全順序は凍結面**。本案は「順序を決める仕組み」を外すので v1 の変更で、v0 には入れない。
  v0 で入れられるのは Studio 側の**読むだけ**の部分(plan ノードを `canonical_anchor` / bucket で段に分類して二段フィルターを出す)。
  これは今の plan の情報だけで作れる。
- 既存の feature JSON(`engine://features/*`)の `insert: after:post_main` は、`after:<stage名>` を「段の末尾」と読むことで無改造で動く。
  authored pass の `canonical_anchor` は `stage` の別名として読む(書き戻しは新名)。
- web 鏡像(`web/`)は plan の `stage` 属性を読めればよい。anchor ノードが消えることで、描画している側に空ノードを除く処理があれば削れる。
- 決定性: 段は順序に関与しないので golden に影響しない。ただし §3.2 の「暗黙の鎖を `frame_node_order` に一本化」は
  同じ plan を出すことを fixture で固定する(段階 2)。

## 5. 段階

| 段 | 内容 | 振り分け | 検証 |
|---|---|---|---|
| 0 | Studio 読むだけ: plan ノードを段に分類し、`Show: Render ▸ stage` と Frame Plan の帯を出す。anchor ノードは段の見出しとして扱い、個別ノードとしては描かない | **v0 可**(表示のみ) | `devstudio_graphcanvas_scene_test` に段フィルターの出し分け、`frameplan_graph_test` に帯 |
| 1 | `stage` タグの導入: pass / feature / loop に `stage`、`canonical_anchor` を別名に。`get_frame_plan` に `stage` 属性 | v1 | compose の単体テスト(段の導出)、plan fixture 差分は属性追加のみ |
| 2 | 順序の一本化: `enforceCanonicalOrder` の鎖を `frame_node_order` + 宣言順に寄せ、`__anchor_*` を作らない。HDR 境界を型へ。`__anchor_sprite` を pass に | v1(凍結面の版) | golden 不変、plan fixture(`example_frame_plan.json`)から anchor ノード 8 個が消える以外同じ順序、GPU gate |
| 3 | runtime の `FramePlanNodeKind::anchor` と分岐の削除、GPU 時間表・web 鏡像の追随 | v1 | headless の `get_frame_plan` / `get_status` の形、web の表示 |

0 は Studio 単独で進められ、Qt 6.10 が要るので Windows の `studio_cpu` で確認する。1〜3 は `develop` → `develop_v1` のマージ後。

## 6. 代替案(比較用)

| 案 | 中身 | 評価 |
|---|---|---|
| 現状 + scene 段アンカー追加([preset レシピ化メモ §3 点 1](design_render_preset_feature_recipe.md)) | アンカーを増やして疎結合にする | 役割 1 は改善するが、役割 2〜5 の重さ(段を跨げない、plan を汚す、HDR 境界が位置に依存)はそのまま。凍結面の全順序も伸びる |
| アンカーを完全に廃止(タグも無し) | 順序は依存だけ | feature を「どこに入れるか」を書く語彙が消え、`insert: after:<pass名>` の結合に戻る。探す・隠す単位も失う |
| **本案: タグ化して canvas のフィルター階層へ** | 段 = 見せ方 + 既定の置き場。順序と境界は依存・明示順・型へ | 役割 1 の語彙と探す単位を残しつつ 2〜5 を外せる。preset レシピ化(feature 単位の追随)と相性がよい |

## 7. 未決(2026-10-17 更新)

推す案を付けたもの:

- **scene 段の数** → 3 段(`geometry` / `lighting` / `forward`)。フィルターでは `Render ▸ Scene ▸ geometry / lighting / forward` の
  3 階層にし、forward だけの preset では空の段を出さない。base feature の粒度([preset レシピ化メモ](design_render_preset_feature_recipe.md) §6)と揃う。
- **`unstaged` の置き場** → 依存先(最も近い producer)の段に置く。producer が無ければ Render の末尾。フィルターには「Unstaged」として出す。
- **`after:<stage名>` を残すか** → `stage` に一本化する。`insert:` は pass 名だけ。`after:<stage名>` は 1 版だけ別名として読み、書き戻しは `stage`。
  同梱 feature は同じ WP で書き換える。
- **Order… UI** → 段の順は並び順の既定でしかないので、段を跨ぐ指定に警告は出さない。「段は見せ方、順序は依存と明示順」の分担を UI でも崩さない。
- **`__anchor_sprite` を pass にしたときの sprite 無しフレーム** → 既存の node 非活性(`deactivated_graph_nodes`、`value_graph->nodeActive` のスキップ)に乗せる。
  `anchor_has_work` 相当は「pass が描くものを持たない」として時間表から外す。

残るもの:

- `stage` の既定を feature 全体に 1 つ持たせるか、pass ごとに必須にするか(書きやすさと曖昧さの釣り合い)。
