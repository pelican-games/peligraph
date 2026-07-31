# 描画機構の証拠等級台帳(v1)

対象読者: エンジン担当、WP の受け入れ判定をする人、そして「この機構は実装済みか」を
他の文書から引く人。

ステータス: **v1.1(2026-07-31)**。判定は commit `d58f841`(branch
`agent/wp240a-preset-default`)の WP240a 実装に対して行いました。本書は
[`render_mechanism_coverage.md`](render_mechanism_coverage.md) を置き換えるものではなく、
その ○ / △ / ✕ 判定の**根拠表**です。

## 0. なぜこの台帳が要るか

「実装済み」という一語には、少なくとも次の 6 つの異なる状態が混ざっています。

- JSON パーサが受理するだけ
- 純 CPU の決定性テストが通るだけ
- headless Vulkan で実際に走ったところまで
- headless Vulkan で画素や buffer の値まで確認したところまで
- 実デバイス / 実 HMD / 実 platform の受け入れゲートを通ったところまで
- project 空間の宣言だけで再現できることを示したところまで

この区別は文書上の便宜ではなく、**設計上の区別**です。WP238c は意図的に
「検証できること」で止めており、[`design_heterogeneous_execution_graph.md`](design_heterogeneous_execution_graph.md)
は次のように書いています。

> 「packageを検証できること」と「production runtimeで実行できること」は別のgateとして観測できる。

同じ性格の宣言が [`design_wsi_epoch_recovery.md`](design_wsi_epoch_recovery.md) §14.5 にもあります。

> 未実行platformは「対応済み」とせず、status / reportにgate待ちと明記する。

規則は書かれているのに、**「今どの機構がどの等級まで証明されているか」を横断で読む表が
どこにもありません**。[`design_render_graph_compiler.md`](design_render_graph_compiler.md) §12 の
`gate:` は WP ごとに縦に並ぶだけで、機構をまたいで比較できません。本書がその表です。

### 0.1 他文書との分担

| 問い | 正本 |
|---|---|
| その技法をユーザー空間で書けるか | [`render_mechanism_coverage.md`](render_mechanism_coverage.md) §2 / §3 |
| 書けるとして扱いやすいか | [`render_authoring_ergonomics.md`](render_authoring_ergonomics.md) |
| その機構はどこまで証明されているか | **本書** |
| 各 WP の受け入れ条件そのもの | [`design_render_graph_compiler.md`](design_render_graph_compiler.md) §12 |

### 0.2 引用の作法

本書は **`path:line` を使いません**。222 コミットのリファクタで既存文書の行番号引用は
ほぼ全滅しており、指し先が別の WP のコードへ置き換わっている例もあります。本書は
代わりに **ファイル名 + TEST_CASE のタイトル / タグ / シンボル名**で引きます。これらは
`grep` で再取得でき、行が動いても壊れません。

## 1. 等級の定義

| 等級 | 意味 | 典型的な証拠 |
|---|---|---|
| **E0** | parse / schema を受理するだけ | パーサの単体テスト、正常系 1 本 |
| **E1** | 純 CPU の決定性テストがある | 正準化・fingerprint・拒否条件を含む CPU テスト |
| **E2** | headless Vulkan で実行され、plan / metadata の一致まで確認済み | `LABELS gpu` のテストで image/view/plan を検証 |
| **E3** | headless Vulkan で**画素または buffer の値**まで検証済み | readback した値への具体的条件、golden 画像比較 |
| **E4** | 実デバイス / 実 HMD / 実 platform のゲートを通過 | Simulator / HMD / RDP / display 切替の受け入れ記録 |
| **E5** | project 空間の宣言だけで再現でき、それが実行され結果まで検証されている | project-owned feature / surface / shader による dogfood |

補助記号:

- **†** — byte 固定の証拠がある。`test/golden/inventory.json` に登録された `expected.png`、
  または fixture との byte 一致テストで守られているものだけに付けます。
- **+E4 / +E5** — 等級欄に併記します。

### 1.1 等級は一直線ではありません

E0 → E1 → E2 → E3 は積み上げですが、**E4 と E5 はその上に並ぶ二つの独立した軸**です。

- E4(実機ゲート)は E3 を前提としません。逆に E3 まで積んでも実機記録がゼロのものが
  大半です(§3)。
- E5 は [`render_mechanism_coverage.md`](render_mechanism_coverage.md) §4 の dogfood 条件と
  同じ意味に揃えました。すなわち **project 空間で書けるだけでは E5 になりません**。
  実際に走らせて画素 / buffer 結果まで見て初めて E5 です。実例として、generic raster pass は
  `projects/sprite_demo/passes/main.json` に project 空間の宣言として存在しますが、
  それを実行するテストが 1 本もないので E5 ではありません(§4.1)。

### 1.2 E2 / E3 に共通する但し書き

**E2 / E3 の証拠はすべて CI では一度も実行されていません。**
`test/ci/run_cpu_gate.py` は `ctest -LE gpu` を実行するため、`gpu` ラベルの付いたテストは
CPU gate から除外されます。[`ci.md`](ci.md) 自身が「Vulkan を保証した runner で golden、
validation layer、player を fail-on-no-GPU で実行する責務は将来の CI2 に置く」と明言しており、
CI2 は存在しません。GPU ラベル付きの Catch2 実行体は `test/CMakeLists.txt` の
`pelican_define_test(... GPU ...)` が 18 本、`GOLDEN GPU` の golden 実行体が 4 本、さらに
player を起動する process integration が `LABELS gpu` で登録されています(同ファイルに
`LABELS gpu` は 23 箇所)。**いずれも開発者ローカルの `ctest -L gpu` 依存**です。

したがって本書の E2 / E3 / E4 は「誰かの手元で一度通った」ことの記録であり、常設の回帰網では
ありません。† も同様で、**「byte 固定の基準が存在する」ことを意味するだけ**です。基準との
比較そのもの(`golden_cases_test` の画像比較、`golden_framegraph_test` の trace 比較)は
`gpu` ラベル配下なので CI では走りません。CPU gate が常時守るのは、CPU テスト群、
`contract_boundary_gate.py`、そして `golden_inventory.py` による inventory の整合と
`expected.png` の sha256 一致(= baseline ファイルが黙って差し替わっていないこと)です。

ローカル受け入れ記録として、WP240a 完了時の 2026-07-31 に
`ctest -C Debug -L gpu` を全数実行し、**122 / 122 passed、既知 4 skipped**
（実時間 405 秒）を確認しました。同じ HEAD の CPU gate は
**942 / 942 passed、環境依存 1 skipped**です。これは常設 CI2 の代わりではありませんが、
本台帳の E2 / E3 根拠が同一 HEAD で再実行可能な状態へ戻ったことを示します。

## 2. 一覧表

### 2.1 ViewFamily と secondary view

| 機構 | 等級 | 根拠 | 次の等級に必要なもの |
|---|---|---|---|
| ViewFamily 基盤(stable ID / token / temporal history) | **E3† +E5** | CPU: `test/viewfamily_test.cpp` 11 TEST_CASE(token 安定性、cardinality、main/secondary 分離、実行順変更に耐える history、membership 変化)。GPU: golden `shadow_on` / `morph_skinned_shadow` が `$shadow/directional` を実行し `expected.png` で byte 固定。trace は `test/fixtures/renderer_execution_traces.json` | secondary family を含む byte 固定ケース。現状 trace fixture 664 node は全て `single_view` |
| directional CSM(cascade) | **E3 +E5** | CPU: `viewfamily_test` "directional cascade provider creates stable camera-relative family views"、`featurecompose_test` の cascade count / layer、`multiview_execution_test` の LightUBO pack/unpack。GPU: `golden_harness` の `shadow_b_layer_cascaded` mode が `shadow_map` の `array_layers == 3`、LightUBO の split 単調増加と最終 20.0、`shadow_depth` の 3 回 sequential 実行、**layer ごとの depth readback**、cascade 別 draw compaction を検証。E5 の根拠は cascade 設定が project 空間の `parameters`(`cascade_count` 等)と project-owned な `shadow_probe` feature だけで書かれていること、および同じ feature の project 複製が `shadow_b_layer_project` で画素一致すること | `test/golden/shadow_b_layer_cascaded/` を作って inventory へ登録し byte 固定する(現在このディレクトリは存在せず、画像への主張は「サイズが engine と同じ」「off と異なる」だけ) |
| planar reflection | **E3 +E5** | CPU: `viewfamily_test` の identity/clip plane/winding と oblique 正射影 fallback、`featurecompose_test` "planar reflection feature builds a clipped secondary-family render slice"。GPU: `golden_cases_test` "planar reflection executes a clipped secondary view family on the GPU" が 3 variant を回し、64x64 / 2 layer / 7 mip / storage usage、clustered selection buffer の word 単位検証、mip 連鎖の dispatch group 列、`$reflection/planar` の 15 node、forward/deferred の byte 差分を検証 | `test/golden/planar_reflection*/` を作って byte 固定する。現在は `test/golden/shadow_off` のディレクトリを借りているだけで画像比較は一切走らない |
| 交換可能な reflection prefilter package | **E3 +E5** | 同 harness が `#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS` の両側で解決先を検証(`engine://render_algorithms/planar_reflection/standard_prefilter` と `project://shaders/custom_planar_prefilter`)。build purge は CI1 の `test/run_build_units_smoke.cmake` の `verify_standard_render_algorithms_absent()` | 差し替え後の**出力画素**を固定する証拠。現在は「解決先が変わったこと」までしか見ていない |
| runtime cube render target(資源形状 / face attachment / sampled cube view) | **E3** | CPU: `targetrenderplanning_test` / `renderingsamplecount_test` の `[wp236]`、`surfacecompiler_test`。GPU: `multiview_execution_test` が実 device 上で `wp236_cube_target` を確保し `eCubeCompatible`・6 layer・3 mip を確認、face attachment の rendering info と 6 面の layer 分離、cube view の descriptor 解決までを検証。さらに 6 枚の face attachment view へ実際に draw を発行し、image → host buffer コピーで face 別の byte 列を読み戻して、各 face が隣の face と一致しないことを検証 | golden 化(†)と、face 画素を特定の期待値へ固定すること。実デバイス gate も未実施 |
| 交換可能な six-face cube capture(feature) | **E1** | CPU のみ: `viewfamily_test` "standard cube capture publishes six stable Vulkan face views"、`featurecompose_test` "cube capture feature composes one replaceable six-face hybrid slice"。**`test/golden_harness.cpp` に `cube` の文字列は 0 件**。`multiview_execution_test` は cube target の 6 面へ実際に描いて face 別 readback を比較していますが、それはテストが attachment view と pipeline を直接組んだものであって、**`$face/` view provider と featurecompose を通るこの feature の経路を実行するテストは存在しません**(`$face/` は `src/core/render_algorithms/cube_capture/cubecaptureview.hpp` にしか現れず、`test/` には出てきません) | **feature 経路を通して** headless で 6 面を描き、face ごとの画素または sampled 結果を検証する GPU テスト 1 本 |
| ViewFamily-local clustered light selection | **E3 +E5** | GPU: planar harness の selection buffer word 単位検証(family token と FrameUBO の一致を含む)、`headless_render_test` "clustered lighting uploads and selects more than 32 lights for every hybrid consumer" | cube capture 側 integration の GPU 証拠(現状 `featurecompose_test` の合成確認のみ) |
| secondary family の multiview | **なし** | 根拠なし。物理 scope は必ず 1-view テンプレートで作られ、secondary は sequential 展開のみです | 設計側の決定が先です。[`design_render_graph_compiler.md`](design_render_graph_compiler.md) §3.6 の残件 |
| point / spot shadow の view provider | **なし** | 根拠なし。実装がありません(coverage.md G6b) | 実装 |

### 2.2 draw queue と GPU draw

| 機構 | 等級 | 根拠 | 次の等級に必要なもの |
|---|---|---|---|
| draw sort policy(組み込み provider) | **E3† +E5** | CPU: `drawqueuebuilder_test` 10 TEST_CASE(legacy byte 一致、分割時の stride/offset 不変、決定性、phase queue、zero-to-one 錐台 culling の保守性)、`renderpolicyregistry_test` 4 TEST_CASE。GPU: `golden_harness` が XR occlusion 経路と OpenXR TAA 遷移経路で `draw_sort` を authoring して実行。golden 画像側は半透明を含むケースが byte 固定 | 組み込み provider については残件なし |
| draw sort provider(game DLL の公開 C ABI) | **E1** | `test/render_policy_dll_test.cpp` + `render_policy_game_dll_e2e`(v1 / v2 / bad_abi の 3 DLL fixture、`TIMEOUT 30`)。**GPU ラベルは付いておらず Vulkan device を作りません** | 差し替えた provider が実際の描画順を変えたことを headless Vulkan で見る |
| transparent phase draw queue / family-local 再ソート | **E3† +E5** | CPU: `drawqueuebuilder_test` "RPE5 phase queues preserve opaque batching and sort transparent back to front"。GPU: planar harness が `viewFamilyDrawOrderForTesting()` で反射カメラからの並びが main と異なることを検証。golden `snapshot_refraction` などが byte 固定 | family-local 再ソートの byte 固定 |
| GPU-written draw / depth pyramid occlusion / draw segment | **E3 +E5** | GPU: `golden_harness` の `runGpuDrawIndirect` / `runGpuOcclusionCulling` / `runGpuSegmentedOcclusionCulling` / `runGpuSegmentedOcclusionXr` / `runGpuSegmentedOcclusionHotReload`。CPU: `drawqueuebuilder_test` "WP210d draw queue segments assign disjoint GPU output ranges per fixed state" | `gpu_draw_*` の各 mode を inventory へ登録して byte 固定する(現在 mode は harness にあるが golden ディレクトリが無い) |
| GPU draw の損益分岐計測 | **E2** | `golden_timing_test` "GPU draw timing records a workload sweep without absolute CI thresholds"。テスト自身が `"absolute_duration_thresholds": false` を宣言しており、**性能回帰ゲートではありません** | 目的が「レポート出力」なので等級を上げる前に、何を回帰と見なすかの決定が要ります |

### 2.3 compiler / plan / physical boundary

| 機構 | 等級 | 根拠 | 次の等級に必要なもの |
|---|---|---|---|
| typed pipeline resolve / immutable `CompiledRenderPipeline` | **E3† +E5** | CPU: `renderpipeline_resolve_test` 15 TEST_CASE。GPU: `golden_framegraph_test` "Renderer execution matches plan order and captured traces" が `test/fixtures/renderer_execution_traces.json` と `canonical_frame_plan_trace.txt` に対して実行順・level・node 集合を固定。Project dogfood: `projects/animgraph_demo/passes/main.json` が `hybrid_v1` preset を選び、`animgraph_demo_preset_headless_player` が runtime frame plan の directional shadow / deferred / forward / snapshot / present 展開と PNG 出力を検査 | preset 選択経路の残件なし。project-owned preset 定義は別機構 |
| 論理グラフ compiler(型 / SSA 値 / footprint / intent) | **E1** | CPU のみ: `logicalrendergraph_test` 10 TEST_CASE。論理グラフは runtime generation に保存されず、`compileVulkanTargetPlan()` の入力としてのみ生きます | 論理グラフ dump の fixture 固定。現在 `test/fixtures/` に該当 fixture はありません |
| target planning(desktop / tile-local / transient / alias) | **E3** | CPU: `targetplanning_test` 10、`targetrenderplanning_test` 34(hazard stress の seed 再現、pin/eject round-trip を含む)。GPU: `headless_render_test` "runtime target planner executes a fused tile-local scope" / "executes aliased image lifetimes" / "dependency-safe physical scopes reorder and fuse real Vulkan rendering" | tile-local / transient / alias を有効にした golden ケースがありません。加えて resize 越しの再結合テストもありません |
| plan pin / physical fragment の eject と貼り戻し | **E3** | GPU: `headless_render_test` "hybrid_v1 preset registers and renders a headless frame" が `ejectable_pin_package` / `ejectable_physical_fragment` の JSON round-trip と fingerprint 一致を実行時に検証 | 一般 scope / queue-barrier fragment。[`implementation_plan.md`](implementation_plan.md) は WP204 を「general scope/queueと実機GPU gate待ち」としています |
| typed sample count / 実 MSAA | **E3** | CPU: `samplecountplanning_test` 6、`renderingsamplecount_test` 19。GPU: 同 hybrid ケースが `msaa: {samples: 4, fallback: lower_supported, scope: geometry}` で走り、`selected_samples > 1` と実 image の sample 数一致、画素 readback まで確認 | **MSAA を有効にした golden ケースが 1 件もありません**(`test/golden/*/case.json` に sample 指定はゼロ) |
| common `FrameExecutionPlan`(WP238a) | **E1** | CPU: `frameexecutionplan_test` 4 TEST_CASE(render/compute/copy の 1 語彙への収束、非連結成分を許す決定的正準化、raster dialect、閉じていない endpoint の拒否)。GPU 経路でも compile され `validateFrameExecutionPlanCompatibility()` で FramePlan と突き合わされますが、**実行を駆動しません**。出力先は `get_frame_plan` の `execution_plan` セクションだけです | `execution_plan` の dump fixture を作って byte 固定する。等級を E2 以上へ上げるには「実行を駆動する」設計判断が先です |
| generic raster pass ABI(WP238b) | **E1** | CPU のみ: `rasterpass_test` 4 TEST_CASE(正準化、attachment 数の上限なし、不正な typed operation / state の拒否、fingerprint 変化)、`frameexecutionplan_test` "generic raster dialect remains visible in the common execution plan"、`frameplanner_test` `[frameplanner][raster][resource-port][wp238b]`、`renderingpass_helpers_test` の `[renderingpass][raster][wp238b]` と `[renderingpass][raster][vulkan][wp238b]`(後者は Vulkan adapter への lowering を見ますが device を作りません)。**`"type": "raster"` は GPU ラベル付きテストに一度も現れません** | headless Vulkan で raster pass を 1 本描いて画素を確認する。`projects/sprite_demo` はどのテストからも参照されていないため、現状の dogfood は authoring 止まりです(§4.1) |
| complete physical plan package(WP238c) | **E3** | CPU: `targetrenderplanning_test` の verifier 群(eject → JSON round-trip → `verifyVulkanCompletePhysicalPlanPackage` → fingerprint、および被覆・依存順・lifetime/alias・attachment 契約・capability・鮮度の各拒否)。GPU: `headless_native_scope_test.cpp` の WP238e ケースが `ejectVulkanCompletePhysicalPlanPackage()` で自動 plan を取り出し、`installVerifiedVulkanCompletePhysicalPlanPackage()` の厳格検証(その graph の logical graph / topology / automatic plan と format capability・有効 device extension に対する照合)を通してから、その plan で実際に描いた frame の画素を readback します | golden 化(†)と、実デバイス gate(E4)。なお JSON からの入口が無く、`RenderCompilerProgram` を C++ で書いた人しか流し込めない点は変わりません |
| `NativeScope` executor(WP238d / WP238e) | **E3** | CPU: `vulkannativescopeexecutor_test` 4 TEST_CASE(builtin marker の prepare、provider snapshot と generation lease、provider 欠落 / capability drift の拒否、complete-plan と runtime-plan の drift 拒否)。GPU: `headless_native_scope_test.cpp`(GPU ラベル付き `headless_render_test` 実行体へ `target_sources` で同梱)唯一の TEST_CASE「WP238e NativeScope records Vulkan commands and rebuilds through renderer generations」(タグ `[wp238e][headless][render][native-scope][vulkan][validation][capture]`)が、**テストが自分で所有する** provider `pelican.test.vulkan.clear_attachment@1` で実 Vulkan の `beginRendering` clear を記録し、`readbackLastFrameRGBA8()` の 16x16 中心画素が第 1 世代で `RGBA(255,0,255,255)`、第 2 世代で `RGBA(0,255,0,255)` になること、世代交代後の executor 破棄回数(`destroyed_executors == 2`)までを検証します。validation message が空であることも要求しますが、**この assertion は validation layer が取得できたときだけ実行される条件付き**です(`if (validation)` の内側)。**既定の Vulkan compiler program は依然 `verified_complete_physical_plans` を埋めません**(埋めるのは委譲 `RenderCompilerProgram` を C++ で書いたテストだけで、この provider は project 空間からも game DLL からも書けません)。同梱 builtin provider は `builtin.vulkan.noop_marker@1` の空 marker のままです | golden 化(inventory 登録)による † と、実デバイス gate(E4)。WP238e レポート §Remaining boundary が残件を挙げています。公開 game-DLL callback ABI は未凍結、DLL 境界を越える Vulkan handle / allocation / pipeline service は未定義、`VK_ERROR_DEVICE_LOST` 注入は同レポート自身が「This work does **not** inject a real `VK_ERROR_DEVICE_LOST`; that remains a separate destructive or mock-device gate」として未実施です |
| `NativeScope` の公開 game-DLL ABI | **なし** | 根拠なし。`src/core/userpublic/render/` には draw sort / graph transform / pass implementation / render strategy / subgraph replacement の 5 ヘッダしかなく、NativeScope 用のヘッダはありません。source-level の C++ 拡張です | 実装。`implementation_plan.md` は WP238e 行を「✅ 完了(2026-07-31)…公開game-DLL ABIとdevice-loss注入は後続」とし、本文でも「公開game-DLL ABIと意図的な`VK_ERROR_DEVICE_LOST`注入は未実装である」としています |

### 2.4 拡張 provider(公開 C ABI)

これらは **「builtin provider を通る経路」と「差し替えた provider」で等級が違います**。
表は 2 行に分けず、根拠欄で書き分けます。

| 機構 | 等級 | 根拠 | 次の等級に必要なもの |
|---|---|---|---|
| PassImplementation provider(fullscreen 限定) | **E3†(builtin、間接)/ E1(差し替え)** | builtin: すべての fullscreen pass が `resolveRenderingPassImplementations()` を通るため golden 全ケースが間接証拠になります(provider 境界を名指しで検証してはいません)。差し替え: `passimplementationregistry_test` 5 TEST_CASE(contract 保持、stale handle 拒否、失敗の atomic 性、snapshot lease)のみで、GPU テストに `implementation.provider` を書いた config はありません | 差し替えた shader pair で描いた画素の検証 |
| Subgraph replacement(tagged region) | **E2(builtin)/ E1(差し替え)** | builtin: `headless_render_test` の GPU arena ケースが `regions` と `region_replacements` を authoring して実行。差し替え: `subgraphreplacementregistry_test` 6 TEST_CASE(境界契約、外部 materialize、非連続 region の拒否、lease) | 展開した subgraph が実際に描いた結果の検証 |
| Global GraphTransform | **E2(builtin)/ E1(差し替え)** | builtin: hybrid ケースと GPU arena ケースが `graph_transforms` を authoring し、`provider == builtinLogicalGraphTransformProvider` を確認。差し替え: `graphtransformregistry_test` 5 TEST_CASE(境界保存、順序付き chain、無効候補の非破壊拒否、lease) | 構造を変える provider を GPU 経路で走らせる |
| RenderStrategy | **E2(builtin)/ E1(差し替え)** | builtin: hybrid ケースが `render_strategy: {name: headless.hybrid_authored}` を authoring し `provider == builtinAuthoredRenderStrategyProvider` を確認。差し替え: `renderstrategyregistry_test` 5 TEST_CASE(seed 全置換、XR facade、preset 後実行と provenance) | seed を置き換える provider を GPU 経路で走らせる |
| runtime ViewFamily provider registry | **E3 +E5** | `viewfamily_test` "runtime view-family providers expose a generic optional package boundary" が `PELICAN_WITH_STANDARD_RENDER_ALGORITHMS` の OFF 側(`REQUIRE(planar == nullptr)`)まで検証。GPU 側は planar / CSM の実行が証拠 | OFF ビルドでの CTest。CI1 の build-unit smoke は build のみで、OFF 側の `#else` 分岐は CI で一度も実行されていません |

### 2.5 material / 出力 ABI

| 機構 | 等級 | 根拠 | 次の等級に必要なもの |
|---|---|---|---|
| 任意長 typed material output(WP218)+ output 別 attachment state(WP219) | **E3 +E5** | GPU: `headless_render_test` "WP218 and WP219 project material writes typed G-buffer targets with independent attachment state" が project-owned surface / shader で書き、中心と隅の画素値に上下限を課します | golden ケース化(現在 byte 固定はされていません) |
| material same-pixel local-read(WP220) | **E3 +E5** | GPU: `headless_render_test` "material same-pixel resource executes through the tile-local input ABI"(project-owned `.surface` の `resource_ports` から sampler / input attachment のどちらへ降りても同じ accessor) | golden ケース化 |
| material screen input(refraction / depth) | **E3† +E5** | golden `snapshot_refraction` が inventory 登録済みで byte 固定 | 残件なし |
| material / compute の typed resource port | **E3 +E5** | GPU: `headless_render_test` "compute output displaces material vertices through typed resource ports"、golden `compute_buffer` | 残件なし(`compute_buffer` は byte 固定) |
| static texture dimension(KTX2 cube 等) | **E3 +E5** | GPU: `headless_render_test` "KTX2 cubemap material samples the declared face through Vulkan" | 残件なし |
| typed image subresource(mip / layer)と depth pyramid | **E3 +E5** | GPU: `headless_render_test` "typed image subresources execute a two-stage depth pyramid and rebind after resize"(resize 後の dispatch group 再計算と stale candidate 拒否を含む) | 残件なし |
| raster attachment subresource(WP235) | **E2** | CPU: `renderingpass_helpers_test` / `frameplanner_test` / `targetrenderplanning_test` の `[wp235]`。GPU: `multiview_execution_test` が実 device 上で `wp235_subresource_target`(fixed 3 mip / 複数 layer)を確保し、`wp235_attachment_probe` pass の `base_mip_level: 1` / `base_array_layer: 2` な raster attachment view が single-view の左右 invocation と multiview invocation それぞれで正しい subresource view へ解決されること、および pass target extent が mip 1 相当(幅・高さとも 1/2)へ縮むことを検証 | 特定 mip / layer へ raster 出力した結果を読み戻す GPU テスト |
| pass-local named material variant(WP206b) | **E3 +E5** | GPU: `headless_render_test` "project-owned material variant renders a second opaque pass" | golden ケース化 |
| material tag / filter による選別(WP206a) | **E1** | CPU: `drawqueuebuilder_test` の WP206a 2 TEST_CASE(登録順・ソート順に対する不変性、flat と 2-view 公開後の filter 残存)。CPU テストなので CI で常設実行されます | 実描画での選別結果の検証 |

### 2.6 出力寿命 / XR / temporal

| 機構 | 等級 | 根拠 | 次の等級に必要なもの |
|---|---|---|---|
| WSI epoch recovery(WP215/216/217) | **E1 +E4(部分)** | CPU: `swapchainrecovery_test` 19 TEST_CASE、`frametarget_test` / `outputcompilefacts_test` / `submissionlifetime_test`。**すべて CPU シミュレーション**です。E4 側: `wsi_fault_window_player` が実 window に対する ordered fault injection を実行しますが、**Windows 限定・`Debug` 限定**で、`test/run_wsi_fault_window.cmake` は他 config では `return()` するため CTest 上は緑のまま素通りします。使う project は `projects/example`、`--xr off` | [`design_wsi_epoch_recovery.md`](design_wsi_epoch_recovery.md) §14.5 の手動 platform gate(RDP 接続 / 切断、display mode 変更、monitor 移動、DPI 変更、X11 / Wayland)。1 つも実施記録がありません |
| XR 基盤(XR0〜XR4: discovery / session / action / view space / feature policy / activation) | **E1** | CPU のみ: `xrdiscovery_test` / `xrsession_test` / `xraction_test` / `xrviewspace_test` / `xrfeaturepolicy_test` / `xractivation_test` / `xrcompositiontarget_test`(いずれも `pelican_define_test(... pelican_openxr)` で GPU ラベル無し)。`vrm_xr_demo_test` は GPU ラベル付きですが、TEST_CASE は VRM fixture の決定性・geometry・activation 契約であって XR 実行経路ではありません | 実 OpenXR ランタイム(Simulator を含む)での実行。現状すべて fake / injected table です |
| XR multiview 実行(WP203b) | **E3** | GPU: `multiview_execution_test` "typed Vulkan multiview renders two frame records in one execution"(**このファイル唯一の TEST_CASE**)。golden 側は `golden_temporal_test` の stereo / OpenXR TAA 遷移ケース | golden 画像で XR / multiview 実行されるケースが 1 件もありません。`renderer_execution_traces.json` の 664 node は**全て** `single_view` です |
| OpenXR array swapchain / composition depth(WP203c) | **E1** | CPU: `xrcompositiontarget_test` などが fake / injected table で失敗経路を検証。`graphvariantpolicy_test` / `targetrenderplanning_test` の `[wp203c]` | 実 OpenXR ランタイムを使うテストは存在しません。`implementation_plan.md` が「Simulator/実機 gate待ち」としている通りで、これが E4 の本丸です |
| XR per-view GPU culling | **E3** | GPU: `golden_harness` の `runGpuSegmentedOcclusionXr` が sequential(CPU)/ sequential(GPU)/ auto(multiview)の 3 経路を比較し、`view_begin_count`、`uses_multiview`、`mixed_execution`、pyramid の `view_layout` まで確認 | byte 固定 |
| temporal(history / velocity / projection jitter / TAA) | **E3†** | golden inventory に `taa_*` 7 ケース、`temporal_accumulation`、`vat_playback` が登録済み。`golden_temporal_test` は「jitter-only Halton と等価テーブルの byte 一致」「TAA static accumulation が 2 回の独立実行で byte 一致」を要求。`test/contract_boundary_gate.py` が projection jitter の consumer inventory を CPU gate で守ります | 残件なし。本書で最も証拠が厚い領域です |
| pipeline generation の atomic 公開 / rollback / hot reload | **E3 +E5** | CPU: `renderpipelinetransaction_test` 11 TEST_CASE。GPU: `headless_render_test` "WP196 pipeline watcher coalesces dependencies and preserves the active generation on failure" と "WP194 rollback and WP195 scope replacement preserve generation-owned GPU resources"、`golden_harness` の `runGpuSegmentedOcclusionHotReload` / `runFullscreenRebind` | reload で **view family の集合が変わる**経路(planar reflection を実行中に有効化 / 無効化する等)、および cascade 数を実行中に変える経路の証拠がありません |

## 3. E4 が空欄のもの — 実機・実 platform で一度も動いていない機構

**この節が本書で最も価値のある部分です。**

HEAD 時点で E4 に届いているのは **WSI epoch recovery の Windows live fault gate だけ**で、
それも `Debug` 構成限定・`projects/example`・`--xr off` という限定付きです。
それ以外の描画機構は、**実デバイス / 実 HMD / 実 platform の受け入れ記録が一つもありません**。

| 機構 | 現在の最高等級 | E4 が空欄である理由(一次情報) |
|---|---|---|
| XR 基盤(XR0〜XR4)と OpenXR array swapchain / composition depth(WP203c) | E1 | `implementation_plan.md`「実装済み・Simulator/実機 gate待ち」。実 OpenXR ランタイムを使うテストはリポジトリに存在せず、XR 系はすべて fake / injected table |
| XR multiview 実行 | E3 | 同上。`design_render_graph_compiler.md` も Meta XR Simulator / 物理 HMD / 対象 GPU の実測 gate を WP203c の外部受け入れ作業として残しています |
| physical plan eject / direct authoring(WP204) | E3 | `implementation_plan.md`「general scope/queueと実機GPU gate待ち」 |
| `NativeScope` executor(WP238d / WP238e) | E3 | WP238e レポート §Remaining boundary。headless では画素 readback まで到達しました(validation message 0 の要求は layer が取れたときだけ)が、公開 game-DLL callback ABI は未凍結、DLL 境界を越える Vulkan handle / service は未定義、device-loss 注入は「a separate destructive or mock-device gate」として未実施です。実デバイス / 実 platform の受け入れ記録はゼロ |
| complete physical plan(WP238c) | E3 | 同上。WP238e で headless Vulkan の eject → verify → install → 実描画 → 画素 readback までは通りました。runtime へ流し込む JSON 入口は依然無く(§4.3)、実デバイス gate の記録もありません |
| VRS / foveation(WP212) | なし | `implementation_plan.md`「Quest SA2 device/計測待ち」。実装自体がありません |
| `dist-bake`(WP211) | なし | 「並行候補・配布/Quest前必須」。実装自体がありません |
| WSI の RDP / display 切替 / Linux / Quest | E4(Windows Debug のみ) | `design_wsi_epoch_recovery.md` §14.5 の手動 gate が未実施 |
| MSAA / tile-local / alias / cube capture / planar reflection / CSM | E1〜E3 | いずれも headless 止まり。実機 GPU の format / capability 差を踏む経路(fallback、alias、transient)ほど実機記録がありません |

### 3.1 E4 以前の穴 — E2 / E3 も常設ではありません

§1.2 の通り、E2 / E3 の証拠は CI で一度も実行されません。CI が常時回すのは
`test/ci/run_cpu_gate.py` の CPU テスト群と、`golden_inventory.py` / `contract_boundary_gate.py`
の 2 つの構造 gate だけです。前者は `test/golden/inventory.json` の 52 ケースと
`expected.png` の sha256 一致を確認しますが、**baseline が黙って差し替わっていないこと**を
守るだけで、描画結果が baseline と一致するかは見ません(それは GPU 側の
`golden_cases_test` の仕事です)。

さらに golden の解像度は既定 16x16(`test/golden/README.md`)で、明示的に extent を持つ
ケースは 7 件しかありません。**cascade shadow や reflection の陰影品質は 16x16 の golden では
原理的に押さえられません**。† が付いていても品質回帰を捕まえるとは限らない、という
三重の但し書きが要ります。

### 3.2 新機能に対する公開 ABI の網

`test/contract_boundary_gate.py` が守るのは OpenPBR pin / projection jitter inventory /
`engineMvp` v1 の 3 つだけです。WP183 以降に増えた 5 本の公開 C ABI ヘッダ
(`src/core/userpublic/render/*_abi_v1.hpp`)は **この gate の対象外**で、網は各 registry テストの
`[abi][lifetime]` TEST_CASE と `render_policy_game_dll_e2e` に限られます。

## 4. dogfood(E5)の現状

### 4.1 `projects/` 配下で実際に使われている機構

リポジトリの project は 4 つ(`example` / `sprite_demo` / `vrm_xr_demo` / `animgraph_demo`)です。
rendering config が参照する feature は次で全部です。

| project | features | 備考 |
|---|---|---|
| `example` | `engine://features/ui.json` | 他は fullscreen の SSAO / bloom チェーンを手書き |
| `animgraph_demo` | `engine://features/shadow_directional.json` | `engine://render_pipelines/hybrid_v1.json` を project 空間から選択 |
| `sprite_demo` | `engine://features/sprite.json` | `ssao_clear` が **`"type": "raster"`**(WP238b の dogfood) |
| `vrm_xr_demo` | (なし) | |

WP240a により、`projects/` 配下から `hybrid_v1` preset、directional CSM ViewFamily、
builtin draw sort、deferred / forward material routing、opaque snapshot を使う経路が初めて入りました。
planar reflection / cube capture、clustered lighting、TAA、graph transform、render strategy、
physical fragment、plan pin は引き続き project 空間での使用が 0 件です。

`animgraph_demo_preset_headless_player` は committed project を直接起動し、preset の runtime
frame plan 展開と PNG 出力を検査します。したがって preset 選択経路は authoring だけでなく
常設の project-space process integration を持ちます。一方、従来の
`rpc_headless_player` / `wsi_fault_window_player` / frame-plan dump 系は引き続き
`projects/example` か `projects/vrm_xr_demo` を使います。
`projects/sprite_demo` はどのテストからも参照されていないので、そこに書かれた raster pass も
自動テストでは一度も実行されません。

### 4.2 テストが書く一時 project での dogfood

従来の E5 は、`headless_render_test` と `golden_harness` が一時ディレクトリに project ツリー
(`project.json` / `pipeline.json` / `shaders/*.surface` / `*.frag`)を書き出す形が中心でした。
WP240a では committed project を直接起動する process integration も加わりました。どちらも
**`src/` を触らずに機能が成立すること**を確認する証拠として、次の表にまとめます。

| 機構 | project 側に置かれるもの | 検証 |
|---|---|---|
| directional shadow 受光 | `project://features/shadow_directional.json`(engine feature の複製) | golden `shadow_b_layer_project` と `shadow_b_layer_engine` の **画素完全一致**、かつ `shadow_b_layer_off` と相違。3 件とも inventory 登録済み |
| planar reflection prefilter | `project://shaders/custom_planar_prefilter` | 標準 package ON / OFF の両側で解決先を検証 |
| material typed output / attachment state | project-owned `.surface` + `.frag` | 画素値の上下限 |
| material same-pixel local read | project-owned `.surface` の `resource_ports` | 同一 accessor が sampler / input attachment へ降りても成立 |
| compute → material vertex displacement | project-owned compute shader | golden `compute_buffer`(byte 固定) |
| pass-local material variant | project-owned surface / pass | 2 本目の opaque pass が描かれること |
| MSAA + hybrid preset | `project://features/shadow_directional.json` + `render_strategy` / `graph_transforms` の authoring | sample 数と画素 |
| engine preset 選択 | committed `projects/animgraph_demo/passes/main.json` の `pipeline.preset` + feature 参照 | `animgraph_demo_preset_headless_player` が directional shadow / deferred / forward / snapshot / present の runtime plan 展開と PNG 出力を検査 |

### 4.3 E5 に達していないもの

| 機構 | 状態 |
|---|---|
| generic raster pass(WP238b) | `projects/sprite_demo` に project 空間の宣言はあるが、**実行して結果を見るテストが 0 件**。authoring だけの dogfood |
| cube capture | project 空間の宣言も検証も無し |
| complete physical plan / NativeScope | **そもそも project 空間から書けません**。preset を使う rendering config の top-level 許可キーは `pipeline` / `features` / `snapshots` / `shader_defines` / `draw_sort` / `graph_transforms` / `render_strategy` / `target_planning` / `vulkan_plan_pins` / `vulkan_physical_fragments` / `xr` の 11 個で、complete physical plan に相当するキーはありません。使えるのは `RenderCompilerProgram` を C++ で書いた人だけです。E5 は原理的に到達不能で、これは設計どおりです。**部分置換の `vulkan_physical_fragments` は JSON から書けるのに、完全置換は書けない**という段差があります |
| graph transform / subgraph replacement / render strategy / pass implementation の**差し替え** | builtin provider の authoring は E2 まで到達。差し替え provider は game DLL 側の実装が要るため、CPU registry テストの fixture のみ |
| VRS / dist-bake | 実装なし |

### 4.4 E5 を読むときの注意

E5 は「engine feature を参照した」ことではなく「**engine を触らずに再現できる**ことを
示した」ことです。両者は別物です。たとえば TAA は golden で厚く固定されていますが、
それは engine 同梱 feature を有効化した結果であって、project 空間で TAA 相当を組み直せる
ことの証明ではありません。§4.2 の表で「project 側に置かれるもの」欄を設けたのはこのためです。

## 5. 台帳の更新ルール

### 5.1 WP 完了時の手続き

1. **等級を上げるのは、根拠が `test/` か `docs/design_reviews/` に実在してからです。**
   実装コミットが入っただけでは上げません。
2. 根拠欄には **TEST_CASE のタイトルかタグ、golden ケース名、レポートのファイル名**を書きます。
   `path:line` は書きません(§0.2)。
3. † を付けられるのは、`test/golden/inventory.json` に登録された `expected.png` があるか、
   `test/fixtures/` の byte 比較で守られている場合だけです。harness に mode があるだけでは
   付けられません。
4. E4 を付けるときは、**どの platform / どの device / どの構成**かを根拠欄に書きます。
   「Windows Debug のみ」のような限定は必ず残します。
   [`design_wsi_epoch_recovery.md`](design_wsi_epoch_recovery.md) §14.5 の
   「未実行platformは『対応済み』とせず、status / reportにgate待ちと明記する」を
   本書全体の規則として採用します。
5. E5 を付けるときは、project 側に何を置いたかを §4.2 の表へ 1 行足します。
6. 等級が上がったら、[`render_mechanism_coverage.md`](render_mechanism_coverage.md) の
   該当行(○ / △ / ✕ と G 番号)を再判定する必要があるか確認します。本書は coverage の
   根拠表なので、**片方だけ動かしてはいけません**。

### 5.2 等級を下げる場合

テストが削除された、golden ケースが inventory から外れた、mode が harness にしか無くなった、
という場合は等級を下げます。下げた理由を根拠欄へ残します。

### 5.3 「根拠なし」の扱い

テストが見つからなければ **E0 ではなく「なし」**と書き、根拠欄に「根拠なし」と明記します。
E0 は「parse / schema を受理するテストが実在する」ことを主張する等級なので、
何も無いものに付けてはいけません。

### 5.4 判定基準時点

本書の全判定は commit `d58f841` に対するものです。台帳を更新するときは冒頭のステータス行の
commit を差し替え、差分の出た行だけを触ります。**作業ツリーの未コミット変更は等級の根拠に
しません。**
