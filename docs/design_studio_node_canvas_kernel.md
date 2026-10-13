# Studio のノードキャンバス操作カーネル: Graph と Frame Plan の操作層を一本化する

対象読者: Studio 担当。
ステータス: 改善案メモ(2026-10-13。議論の記録。WP 化・レビュー前)。
前提: N-6(配線の制御点)、N-8(Frame Plan のノード編集)、
[v1 開発計画](v1_development_plan.md)「一つのグラフモデルは意味の共通化であり、一枚のキャンバスではない」(§125 付近)と
「保守上の整理候補」(関連機能を触る際に責務と共通経路を整理。行数だけを理由に一括分割しない)。
行番号は `develop`(v0.1.0 + WR-1)時点のもの。

## 0. 要約

- Graph キャンバスで、中継点(reroute)を**枠選択で複数選んでも Delete / Tie together が効かない**。
  原因は、Qt の item 選択(枠ドラッグ)と scene 独自の選択リスト(`selected_reroutes_`)が繋がっていないこと。
  Frame Plan キャンバスには同期(`recordSelection()`)があり、Graph には無い。
- 根本は、Graph と Frame Plan の 2 キャンバスが**「描く部品」は共有し「操作する層」を二重に持っている**こと。
  WP 単位(N-6 → N-8)で積んだ結果で、片方にだけある機能・片方にだけ無いバグが構造的に生まれる。
- 提案: view を 1 つにするのではなく、**操作カーネル(選択・ジェスチャ・制御点・キー・メニュー・その場編集)を 1 つ**にし、
  文書への反映は adapter で注入する。入口は今回のバグ(選択モデルの抽出)。

## 1. 不具合と原因

### 1.1 現象

Graph キャンバスで reroute を枠ドラッグで 2 つ以上選ぶ → 選択枠は描かれる → Delete を押しても消えない
(Tie together も同様)。Ctrl+クリックで 1 つずつ足した場合は消える。

### 1.2 原因

| 系統 | 中身 | 読む側 |
|---|---|---|
| Qt の item 選択 | reroute の丸は `ItemIsSelectable`([graphcanvasdrawing.cpp:662](../src/devstudio/view/graphcanvasdrawing.cpp#L662))。view は `RubberBandDrag`([graphcanvaswidget.cpp:10506](../src/devstudio/view/graphcanvaswidget.cpp#L10506))なので枠で Qt 的に選択され、選択枠も描かれる | 誰も読まない |
| scene 独自の `selected_reroutes_` | `GraphCanvasScene` のリスト。足す経路は Ctrl+クリックの `selectReroute(additive)`([:1149](../src/devstudio/view/graphcanvaswidget.cpp#L1149))だけ | `deleteControlSelection()`(Delete)、Tie together、強調表示 `applyEdgeEmphasis` |

`GraphCanvasScene` のコンストラクタ([:603](../src/devstudio/view/graphcanvaswidget.cpp#L603))には
`QGraphicsScene::selectionChanged` への接続が無い。Frame Plan 側は
[frameplangraphics.cpp:2303](../src/devstudio/view/frameplangraphics.cpp#L2303) で接続し、
`recordSelection()`(同 `:5017`)が `selectedItems()` を自前の set に写す。
N-6 の仕様([implementation_plan.md「N-6」](implementation_plan.md))も scene test も Ctrl 追加選択だけを想定している。

同じ理由で、Graph の node は `selected_node_`(単数)なので、枠で複数選んでまとめて移動 / 削除もできない。

## 2. 二重実装の現状

| | Graph(`GraphCanvasScene`) | Frame Plan(`FramePlanGraphicsScene`) |
|---|---|---|
| 規模 | `graphcanvaswidget.cpp` 10,758 行、メソッド 113 | `frameplangraphics.cpp` 5,199 行、メソッド 58 |
| 同名メソッド(両方に別実装、17 本) | `mousePressEvent` / `mouseMoveEvent` / `mouseReleaseEvent` / `mouseDoubleClickEvent` / `keyPressEvent` / `contextMenuEvent` / `selectEdge` / `applyEdgeEmphasis` / `autoLayout` / `inlineEditText` / `setInlineEditText` / `commitInlineEdit` / `cancelInlineEdit` / `restoreInlineEdit` / `scheduleSceneRectUpdate` / `applySceneRectNow` / `setMenuActions` | |
| 選択モデル | `selected_node_`(単数)、`selected_edge_` / `selected_edges_`、`selected_reroutes_`、`selected_ties_`、`selected_carry_`、`selected_pass_input_`。枠選択の同期**なし** | `selected_node_` + `selected_nodes_`(set)、`selected_edge_`。`selectionChanged` 同期**あり** |
| reroute / tie | N-6 で実装 | N-8「N-6 の item をそのまま使う」だが scene 側のロジックは再実装(reroute / tie 言及 60 箇所) |
| 共有しているもの | `nodelayout`(自動配置)、`nodegraphics`(item の描画)、`NodeCanvasView` | |

「描く部品」は共有され、「操作する層」が二重。

## 3. 提案: 操作カーネルの一本化

```text
NodeCanvasScene(共通の操作カーネル。QGraphicsScene 派生)
  - 選択モデル: node(複数)/ edge / reroute / tie。枠選択・Ctrl 追加・selectionChanged 同期を 1 箇所で
  - ジェスチャ: node drag、control point drag、接続ドラッグ、double-click 挿入、キー(Delete / Undo / Redo)
  - 制御点: reroute / tie の挿入・移動・削除・Tie together / Untie と、layout 永続化のフック(debounce 含む)
  - その場編集、scene rect、context menu の振り分け
        ↑ 文書への反映は adapter(interface)で注入
GraphCanvasScene       … adapter: `edit_graph_document` 系 / `set_layout{scope}` / 定義・ループ・fold・scope
FramePlanGraphicsScene … adapter: `edit_preset_pass` / `remove_authored_pass` / `remove_compute_task` / `set_layout{frame_plan}`
```

- カーネルは「何を選んだか・どう動かしたか」だけを知り、「文書にどう反映するか」は adapter が答える。
- 既に `selectionChanged` / `gestureEnded` / `nodeMenuEntries` / `foldItemMenu` が `std::function` で外出しされているので、
  コードベースの `XxxDependencies` / コールバック注入の流儀に沿う。
- v1 計画の「一枚のキャンバスにしない」とは矛盾しない(view は 2 つのまま、操作の実装が 1 つ)。

## 4. 段階(触るところから。一括分割はしない)

| 段 | 内容 | 直るもの | 振り分け |
|---|---|---|---|
| 1 | **選択モデルの抽出**: `recordSelection` 相当を共通化し、Graph でも `selectionChanged` から `selected_reroutes_` / `selected_ties_` / node 複数へ写す。scope が混ざる選択は `selection_scope_` のものだけ採る。rebuild 中はガード | 今回の不具合(reroute / tie の枠選択 → Delete / Tie together)。node の複数選択は範囲を見て同段か別段 | 不具合部分は **v0**(Studio の挙動、凍結面に触れない)。共通化の器は v1 でもよい |
| 2 | 制御点(reroute / tie)の操作を共通化。N-6 / N-8 の二重実装を 1 つに | Frame Plan と Graph の挙動差 | v1 |
| 3 | ジェスチャとキー処理(mouse* / key* の 6 本)を共通化。残りは adapter へ | | v1 |
| 4 | その場編集・scene rect・context menu | | v1 |

安全網: scene test(`devstudio_graphcanvas_scene_test` / `devstudio_frameplan_graph_test`)と
Studio scenario(`graph_reroute_tie` / `frameplan_edit_roundtrip`)。各段で挙動不変を test で固定しながら移す。

段 1 の test: `setSelectionArea()` で reroute を 2 つ囲む → Delete → 両方消えて `set_layout` が 1 回
(対照: `selectionChanged` の接続を外すと赤)。

## 5. 前提と注意

- `develop_v1`(canvas / Layers dock の SG 系)は `graphcanvaswidget.cpp` に `develop` との衝突を 1 hunk(82 行)持つ。
  カーネル抽出は **`develop` → `develop_v1` のマージを済ませてから**始める(二重に解決しないため)。
- Qt 6.10 が要るので、Linux のクラウド環境ではビルド・テストできない。Windows の `studio_cpu` で確認する。
- `graphcanvaswidget.cpp` は 10k 行超で、`graphcanvas*fields.cpp`(render / parameter / loop / compute / value / material /
  folded pass)に detail 列が分かれている。カーネル抽出は scene のイベント・選択・制御点に限り、fields 系は触らない。

## 6. 未決

- カーネルの置き場と名前(`nodecanvasscene.{hpp,cpp}` を `nodegraphics` / `nodelayout` / `nodecanvasview` の並びに置く案)。
- node の複数選択を段 1 に含めるか(Graph の `selected_node_` を複数化すると detail 列・Inspector 連携に波及する)。
- adapter の形(純粋仮想の interface か、`std::function` の束か)。既存の外出しコールバックとの整合を優先する。
