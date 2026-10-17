# plan の層の棚卸し: 実装待ちの器と、概念の拡張で増えただけの層を分ける

対象読者: エンジン担当(描画コンパイラ / renderer)。
ステータス: 改善案メモ(2026-10-16。議論の記録。WP 化・レビュー前)。
前提: [design_render_stage_as_canvas_filter.md](design_render_stage_as_canvas_filter.md)(アンカー)、
[design_render_target_memory_reduction.md](design_render_target_memory_reduction.md)(RT の計測)、
[design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md)(`ExecutionPlan` の目標語彙、2026-07-23)、
マニュアル [第6章 §6.6 プランダンプ](manual/06_rendering.md)、[v1 開発計画](v1_development_plan.md)「保守上の整理候補」。
数字は `test/fixtures/devstudio/example_frame_plan.json`(example の実機の `get_frame_plan`、約 190 KB)。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- アンカーと同じ性質のもの(仕組みはあるが今は意味を持たない)を plan と runtime から拾い、**2 種類に分けた**。
  - **(1) 実装待ちの器**: 予定しているエンジン機能(第 2 backend = Metal、複数 queue、tile GPU、外部 provider、計測)が入れば
    役目が出るもの。残す。ただし plan ダンプと Studio には既定で出さない。
  - **(2) 概念の拡張で増えたが意味をなしていないもの**: 消費者がいない、または元の情報からいつでも再生成できるもの。**掃除する**。
- **決定(2026-10-17)**: 第 2 backend(Metal)は自前で書かず、**mac は MoltenVK で動かす**(実証計画は
  [design_macos_moltenvk_proof.md](design_macos_moltenvk_proof.md))。これに伴い `execution_plan` は (2) に戻り、**削除する**。
  掃除で消すのは `execution_plan` と `levels`。物理計画のダンプは**概念は 3 つとも本物**で、重複している表だけを一本化する。
  `declared_bytes` は埋める。段階 2(`CompiledRenderPipeline`)は消すのではなく **typed な論理 graph として育てる**(Metal は理由から外れるが、純ロジック test と incremental の理由は残る)。
  transient / tile_local と scope 合流(3-2 / 3-3)は Apple GPU(tile ベース、memoryless)で効くので、**mac 実証で役目が出る器**に格上げ。
- 多くは `get_frame_plan` の鍵に触るので **v1**。先に「plan の各節には消費者があるか、無ければ診断と明示するか消す」を規則にする。

## 1. 判定基準

| 問い | (1) 器 | (2) 拡張の残り |
|---|---|---|
| 今、実行か判断に使う消費者がいるか | いない(将来の機能が使う) | いない |
| 消しても失う情報があるか | ある(入力・設定・provider の API・目標語彙) | **ない**(元の plan から導ける) |
| その機能は予定にあるか | ある(Metal、複数 queue、tile GPU、provider、計測) | 特定の機能に結びつかない |
| test や provider の実装があるか | ある(`headless_native_scope_test` 等) | 自分自身との一致検証だけ |

## 2. 項目ごとの判定

### 2-0 決定の状態(2026-10-17)

| # | 層 | 明言された決定 | 推奨 | 残っている判断 |
|---|---|---|---|---|
| 2-1 | `execution_plan` | **削除**(第 2 backend は書かない。mac は MoltenVK) | 同じ | なし(順番は §6) |
| 2-2 | `levels` | 無し(「並列化で使えないか」の問いのみ) | 契約から外す。複数 queue の計画器は ASAP だけでは足りず、自分で層を作る | 完全に消すか、`diagnostics` に表示専用で残すか |
| 2-3 | 物理計画のダンプ 3 部 | 無し(「概念が分かれるか」の問いのみ) | 3 概念は残し、complete と直下の重複する表を 1 回にする。鍵が変わるので 2-1 の削除と同じ v1 WP に同梱 | やるか、放置か(効果は往復 27 KB と読みやすさ) |
| 2-4 | `declared_bytes` = 0 | 無し(「arena とは」の問いのみ) | 埋める(RT メモ段 0 と同じ WP、v0 可) | 埋めるか、列ごと消すか |
| 2-5 | anchor と順序の二重化 | **段タグ + canvas のフィルター階層へ**(段メモ) | 同じ | 段メモ §7 の残り 1 件 |
| 2-6 | 段階 2 の typed policy | 無し(「他に役立つか」の問いのみ) | 消さず、typed 論理 graph に育てる。Metal の理由は落ちたので、残る理由は純ロジック test(GPU 無しの CI)、incremental、Studio の typed 表示の 3 つ。単独では起こさず、preset レシピ化が段階 2 に触るときに一緒に | 育てるか、現状維持か |
| 2-7 | 文書のずれ | **メモに残す** | 文書修正だけ | なし |


### 2-1 `execution_plan`(WP238a) — (2) 削除(2026-10-17 決定。MoltenVK 採用で第 2 backend の理由が消えた)

- 何か: plan ダンプに併記される backend 非依存 IR(`src/project/executionplan.hpp`)。dialect、implementation、endpoint、
  capability、resource use、依存、bridge、fingerprint。
- 出自: [design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md) §2 の目標語彙 `ExecutionPlan` の
  frame 版 `FrameExecutionPlan`。同設計では **FramePlan の方が「互換 artifact として段階移行する」側**で、
  Metal は「共通 logical / execution contract から別の physical plan を作る兄弟 backend」(決定 12)。
- 今の実態: [`makeCompatibilityFrameExecutionPlan`](../src/core/renderingpass/frameexecutionadapter.cpp#L560) が FramePlan から
  表引きで導き、[`:568`](../src/core/renderingpass/frameexecutionadapter.cpp#L568) が FramePlan と一致を検証、以後はダンプだけ。
  設計の意図と**権威の向きが逆**(FramePlan が権威、`execution_plan` が写し)のまま止まっている。約 1,900 行、ダンプ 30 KB。
- 判定: 削除。Metal backend を書かない(MoltenVK で動かす)と決めたので、目標語彙として残す理由が無い。
  [design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md) は「兄弟 backend は当面作らない」と
  ステータスに追記し、`ExecutionPlan` / `ExecutionLinker` / `BackendProbe` の節は履歴として残す。
- 削除の対象(`develop` で全列挙):

| 層 | ファイル | 中身 |
|---|---|---|
| project | `src/project/executionplan.{hpp,cpp}`、`executionplanwire.{hpp,cpp}`(CMake 2 行) | 型、canonicalize、fingerprint、wire の検証 |
| core | `src/core/renderingpass/frameexecutionadapter.{hpp,cpp}`(CMake 1 行)、`framegraphruntime.{hpp,cpp}`(`execution_plan` の保持・互換生成・検証)、`rendercompilerprogram.{hpp,cpp}`(variant の `execution_plans` と一致検証)、`vulkanrendercompilerprogram.cpp`(`compileFrameExecutionPlan`)、`frameplanner.hpp:118`(dialect selection のコメント)、`renderer.cpp` のダンプ鍵 | 約 1,900 行 + 呼び出し |
| Studio | `frameplanmodel.{hpp,cpp}`(`FramePlanExecutionPlan`、`validateExecutionPlanWire`)、`frameplanwidget.cpp:4286 / 4655`(Execution plan の節: dialect、endpoint、resource uses)、`frameplangraphics.cpp:2617-2769, 3692`(unavailable の panel、`pelicanExecutionPlanState`)、`graphcanvasmodel.cpp:6375`(`graphBackend` と帯の L3「backend の名前」) | 帯の L3 と barrier 行の access 注記([第15章](manual/15_studio.md) 1317 / 1323 行)は消える。barrier の access は FramePlan の barrier から直接出せるので、要るなら置き換え |
| test | `frameexecutionplan_test`、`rendercompilerprogram_test`、`renderpipelinetransaction_test`、`vulkannativescopeexecutor_test`、`featurecompose_test`、`devstudio_frameplan_test`、`devstudio_frameplan_graph_test`、`devstudio_graphcanvas_test`、`devstudio_nodelayout_test`(該当節)、fixture `example_frame_plan.json` の `execution_plan` | |
| docs | 第6章(WP238a の段落、FP-1 の列挙)、第13章 2561 行、第15章 1317 / 1323 行、`11_status`、source-code-guide 06、`render_mechanism_coverage`、`render_evidence_ledger`、`link_anchors.json`、design 3 本のステータス | |

- `view_execution_plan`(`targetrenderplanning.hpp:508`、multi-view の実行回数)は別物。触らない。
- `get_frame_plan` の鍵が減るので v1。Studio の scenario(`pelicanExecutionPlanState` を読むもの)も同じ WP で直す。

### 2-2 `levels` — (2) 掃除(契約から外す。計算は要るときに書き直す)

- 何か: [`computeLevels()`](../src/core/renderingpass/frameplanner.cpp#L1598) が依存辺から node の深さ(ASAP)を出し、
  [`:1899`](../src/core/renderingpass/frameplanner.cpp#L1899) で層にまとめて [`:2125`](../src/core/renderingpass/frameplanner.cpp#L2125) で JSON へ。
- 消費者: runtime・Studio・web に無し。2-1 の一致検証が `level` を比べるだけ。マニュアル 06:1146 は「機械最適化で層を作る」と説明。
- 並列化のときに使えるか: 使えるのは半分だけ。queue への振り分けには ASAP(= `levels`)と ALAP の両方、資源の寿命(同時確保の山)、
  queue の capability、同期点(timeline semaphore、queue ownership transfer)のコストが要る。設計 §4.2 では
  「domain 内外の happens-before と並行可能性」は `TargetExecutionCompiler` の担当で、そこが自分の入力を作る。
  `computeLevels` は 10 行で、その時に書き直す方が安い。
- 判定: ダンプの `levels` と node の `level` を契約から外す。残すなら `diagnostics` 配下で表示専用。マニュアルの説明を直す。

### 2-3 物理計画のダンプ — 概念は 3 つとも本物。重複する表だけ一本化(2)

- 3 つの中身は**同一ではない**(前回の説明は誤り)。

| 節 | 何か | 持つもの(example) |
|---|---|---|
| `ejectable_physical_fragment` | project が **Freeze で所有・編集する最小の決定**(`vulkan_physical_fragments` に貼る単位) | resource は `logical_resource / representation / format` の 3 鍵、scope は `id / nodes`、attachment は `load_op / store_op`、`scope_edit_mode` |
| `ejectable_complete_physical_plan` | runtime が **検証して消費する完全 package**(NativeScope の data contract、fingerprint の単位) | resource 17 鍵(extent、lifetime、pattern、reason …)、scope 10 鍵(view_execution、region_tags …)、`native_scopes`、`required_physical_features`、`external_depth_export` |
| `physical_target_plan` 直下 | planner の**説明**(なぜその計画か) | complete と同じ `resources` / `attachments` / `scopes`(`regions` だけ違う)+ `backend_selection`、`decisions`、`planning_opportunities`、`lowering_graph`、`resolution_plan`、`graph_transforms`、`view_execution_plan`、pin package |

- 概念が分かれる場面: fragment と complete は**常に**違う(投影が違う)。complete と直下は共通の表が構成上同一で、
  違うのは pin / freeze を貼ったとき(直下の `applied_pin_package` に貼ったものが出て、complete は検証後の結果)。
- 重複しているのは complete と直下の `resources` / `attachments` / `scopes`(約 27 KB が 2 回)。fragment は小さく、重複ではない。
- 判定: 3 概念は残す。ダンプでは共通の表を 1 回だけ出し、complete は「直下の表 + 自分だけの鍵」として組む(または直下が
  complete を内包し、周りに説明を足す形)。FP-1 の「同じ世代は使い回す」は保てる。Studio の Freeze / Eject again は fragment を
  読むので影響しない。

### 2-4 `declared_bytes` = 0 — 埋める(器は正しい)

- arena とは: [`RenderPipelineGpuArena`](../src/core/renderingpass/renderpipelinegpuarena.hpp)。**公開中の runtime 世代が所有する
  GPU 側オブジェクトの台帳**。owner scope(`render_pipeline/flat` = 本体、`render_pipeline/variants` = variant)ごとに、
  登録(kind: render_target / frame_graph_buffer / shader_bundle / pipeline / fullscreen_pass / compute_task / debug 系 /
  shadow_depth / velocity / acceleration_structure、handle、name、`declared_bytes`)と、型消去した lease(`shared_ptr<const void>`)を持つ。
  lease は同名差し替え(hot reload)のあと、前の世代のオブジェクトを参照中の frame が退くまで生かす。
  「owner を再コンパイルするとその scope だけ置き換わり、無関係な scope と program は使える」が狙い。example は 89 登録
  (RT 21、shader bundle 34、pipeline 17、fullscreen pass 17)。
- 今: [`renderpipelinegpuarena.cpp:198`](../src/core/renderingpass/renderpipelinegpuarena.cpp#L198) で `declared_bytes = 0` 固定。
  Studio の Frame Plan がその 0 を bytes 列に出す([`frameplanwidget.cpp:5183`](../src/devstudio/view/frameplanwidget.cpp#L5183))。
  つまり arena は「所有の台帳」としては動いていて、「VRAM の台帳」としては空。
- 判定: RT メモ段 0 の計測(format × extent × layers × mips、buffer の size)で埋める。埋めるまで列は出さない。

### 2-5 anchor ノードと順序の二重化 — [段メモ](design_render_stage_as_canvas_filter.md) §3.2〜3.3 の通り

### 2-6 段階 2(`CompiledRenderPipeline` の typed policy) — 消すのではなく backend 境界として育てる

- 今: resolve と core の間で運ぶのは draw sorting / material routing / feature instances の 3 つ。render node の typed 化は
  core 側の [`parseRenderNodeFromJson`](../src/core/renderingpass/frameplanner.cpp#L669) がやり直している(段階 3)。
  だから段階 2 が薄く見える。
- 役に立つ場面:
  1. **Metal**: 設計の `CanonicalLogicalGraph`(immutable / hashable、device 非依存)に当たる場所。node の typed 化と
     構造検証を core から段階 2 に上げれば、Vulkan core と Metal core が同じ typed 入力を受ける。2-1 の (a) と対になる。
  2. **純ロジック test**: typed な論理 graph を GPU 無しで固定できる(今は core の JSON parse を通さないと node の型が出ない)。
  3. **incremental / hot reload**: 段階 2 の出力に stable hash があれば、変わっていない rendering pass の再 prepare を省ける。
  4. **Studio**: typed policy(draw sort、material route)を JSON の再解釈なしに表示・編集できる。
- 判定: 段階 2 を「typed な論理 graph + policy」に育て、core の JSON parse を消す。preset レシピ化と同じ時期。

### 2-7 文書と実態のずれ — 文書修正

- WAW: ガイドは「出さない」、コードに `addWriteAfterWriteBarriers`。
- v0.1.0 のタグ: 文書は未付与、実際は付いている。
- `levels` の「機械最適化」の説明(2-2)。
- `execution_plan` の「観測面」は正しいが、設計文書との権威の向きの違い(2-1)を書く。

## 3. (1) 器として残す(ただし既定で見せない)

| # | 層 | 証拠 | 待っている機能 | 今やること |
|---|---|---|---|---|
| 3-1 | compute queue | [`core.cpp:233-282`](../src/core/vkcore/core.cpp#L233) で選び `:1110` で取得、command pool も作る。submit なし | 複数 queue | 触らない。コードマップに「未使用」1 行 |
| 3-2 | backend 候補 transient / tile_local と `probeCandidate` | example で 0 / 0。desktop では sampler 読みがあると選ばれない | **Apple GPU(MoltenVK、memoryless texture)**、mobile | 候補の確率と理由はダンプに残し、**選ばれなかった候補の resource 全文は出さない**(2-3 と同じ WP) |
| 3-3 | physical scope(pass の合流単位) | 30 node → 30 scope、全部 1 node。合流条件は「前が `store`、次が `load`」([`:2027`](../src/project/targetrenderplanning.cpp#L2027))。`once` / `view_select` は隔離 | **Apple GPU** の render pass 合流、NativeScope の単位 | 残す。Studio では 1 node の scope を node と別に見せない |
| 3-4 | `native_scope_executors`(WP238d) | provider の API と `headless_native_scope_test`。ツリー内に実 provider なし | 外部 provider | 残す。「🚧 source-level の拡張」を維持 |
| 3-5 | arena の owner scope / lease | 2-4 参照 | hot reload と variants(実在) | 残す。2-4 で bytes を入れる |
| 3-6 | `hazard_stress` / `isolate` profile | [`targetplanning.cpp:739-991`](../src/project/targetplanning.cpp#L739) | test の揺さぶり(実在) | 残す |

## 4. 規則(再発を防ぐ)

1. **plan の各節には消費者を書く。** 無ければ `diagnostics` 配下に置いて表示専用と明記するか、出さない。
2. **写しは作らない。** 別の形が要る消費者が来たとき、その消費者が元から導く。一致検証で自分を守る層は持たない。
   目標語彙へ移行するなら、権威を移すところまでを 1 つの WP にする(2-1 の教訓)。
3. **将来の器は入力と API を残し、出力は既定で隠す。** ダンプと Studio は「今効いているもの」を見せる。
4. **0 固定の field は出さない。** 埋められる見込みがあるなら埋める WP を先に切る。

## 5. 段階

| 段 | 内容 | 振り分け | 検証 |
|---|---|---|---|
| 0 | 文書の修正(2-7)、コードマップに 3-1 の「未使用」 | v0 可 | 文書のみ |
| 1 | 2-4: `declared_bytes` を計測で埋める(RT メモ段 0 と同じ WP) | v0 可(鍵は増えない) | planner 単体、手計算 120 / 81 MiB と一致 |
| 2 | 2-3 と 3-2 / 3-3: 重複する表の一本化、選ばれなかった候補と 1 node scope を既定で隠す | v1(鍵が変わる) | Frame Plan / Freeze / Eject again の scenario、web 鏡像 |
| 3 | 2-1: `execution_plan` の削除(上の表)。2-2: `levels` を契約から外す | v1 | golden 不変、GPU gate、Studio scenario |
| 4 | 2-6: 段階 2 を typed 論理 graph に育て、core の JSON parse を消す。2-5 と preset レシピ化に合流 | v1 | 純ロジック test、golden 不変 |

1 は Windows 無しでも planner 単体で進む。2〜4 は `get_frame_plan` を読む Studio と web が動く環境(Windows の `studio_cpu`)で確認する。

## 6. 未決(2026-10-17 更新)

解けたもの:

- **2-1 を消したあとの帯の L3(backend の名前)** → 第 2 backend を作らないので L3 は消す。barrier 行の access 注記は FramePlan の barrier(kind と resource)で足りる。
  access mask が要るなら runtime の `makeTransitionInfo`([`render_target_layout_tracker.cpp:9`](../src/core/vkcore/render_target_layout_tracker.cpp#L9))から出す。
- **2-2 の `level`** → Studio の自動配置([`nodelayout.cpp`](../src/devstudio/view/nodelayout.cpp))は自前で列を計算していて plan の `level` を読まない。契約から外して問題ない。
- **2-3 の形** → web 鏡像は plan を読まない(鏡像は engine resource の方)。直下が complete を 1 部内包し、周りに説明(`backend_selection` / `decisions` / `planning_opportunities` …)を置く形にする。fragment は別のまま。
- **3-2 / 3-3 を隠す** → Freeze は fragment、Unpin は pin package、`hazard_stress` は planner の profile で、どれも「選ばれなかった候補の resource 全文」を読まない。候補名と理由(`backend_selection`)は残す。

残るもの:

- **削除の順番**。2-1 の削除は Studio の Frame Plan と scenario に触るので、`develop` → `develop_v1` のマージ後に置くのは確定。
  カーネル抽出([canvas メモ](design_studio_node_canvas_kernel.md))より前か後かは、どちらも `frameplangraphics.cpp` に触るので同じ人が続けてやる前提で前に置く案。
