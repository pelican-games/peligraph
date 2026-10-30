# 第6章 描画・Vulkan・Shader

最終確認: 2026-10-10

[索引へ戻る](README.md) / [前章](05_gameplay_and_services.md) / [次章](07_tools_rpc_tests.md)

この章は、設定ファイルに書かれた「この順で、この画像へ描く」が、最終的に Vulkan のコマンドへ変換されるまでを追います。Pelican の描画層は、単純な `Renderer` 1 クラスではありません。まず地図として、次の 5 層を頭に置いてください。

| 順 | 層 | 役割 |
|---|---|---|
| 1 | コンパイラ program | rendering JSON を GPU に触らずに型付き定義へ落とす |
| 2 | 計画 | その定義から論理グラフ / 物理ターゲットプラン / FramePlan を作る |
| 3 | GPU 登録と世代 | 画像・shader・pipeline・descriptor を作り、1 回の publish で差し替える |
| 4 | pass dispatch | pass 種別ごとの renderer(Material / Fullscreen / UI …)へ振り分ける |
| 5 | Vulkan backend | device、frame target、image layout、コマンド記録を扱う |

**先に 1 文だけ定義しておく語。** 以下は第6章の本文が説明なしに使い始める語です。深い扱いは各節にあります。

- **面** — 「編集面」「公開面」は操作の入口・対象範囲で、「cube の面」「表面」は幾何の face / surface です。
- **feature** — rendering config に pass・render target・shader define・宣言をまとめて足し引きできる合成単位。`engine://features/*.json` か project 側の JSON として書かれ、外すとその feature が持ち込んだ pass と target が丸ごと消えます([用語集](../manual/14_glossary.md#feature))。
- **frame graph** — 1 フレームで実行する pass / compute task を node、`reads` / `writes` / `after` / `before` を辺とする有向グラフ。実行順と barrier はここから決まります(§6.3、[用語集](../manual/14_glossary.md#frame-graphフレームグラフ))。
- **view family** — 同じ graph を複数の視点で実行するときの「視点の束」の論理名(`$main`、`$shadow/directional` など)。視点数(cardinality)を決めるのは runtime 側の provider です(§6.4、[用語集](../manual/14_glossary.md#view-familyビューファミリ))。
- **論理プラン / 物理プラン** — 論理プラン(`CompiledLogicalRenderGraph`)は resource を SSA 値と read footprint だけで表した device 非依存の表現、物理プラン(`VulkanTargetPlan`)はそこへ実フォーマット・sample 数・scope・view 実行を載せた device 依存の表現です(§6.1)。
- **SSA** — static single assignment。「1 つの値は 1 回しか書かれない」形のこと。ここでは render target 名に版を付けた `{resource, version}` を 1 個の値として扱い、辺を名前ではなく値で張ります(§6.1 の難所)。
- **format_class** — render target 宣言に書く色の役割(`scene` / `display` / `data` / `explicit(...)`)。実フォーマットは authored `format` ではなくこれと HDR の有無から決まります(§6.1 の難所)。
- **scope-execution-major** — コマンド記録のループ順が「scope → 実行(view) → その scope の node 全部」であること。「view ごとに全 node」ではありません(§6.1 の難所)。
- **置換可能な完全パッケージ** — コンパイラが自動生成した物理プランの代わりに、外部が丸ごと差し込める物理プラン一式。検証を通した証拠は別の marker 型が持ちます(§6.1 の難所)。

層ごとの責務と入口は次のとおりです。

| 層 | 主な責務 | 入口 |
|---|---|---|
| コンパイラ program | rendering JSON を variant family ごとまとめて受け、feature 合成・変換 registry・型付き定義への parse・計画までを **GPU に触らずに** 済ませる | [`registerRenderingPassConfigVariantsData()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1483) → [`runRenderCompilerProgram()`](../../src/core/renderingpass/rendercompilerprogram.cpp#L274) |
| 計画(2系統が並行) | 論理グラフ+物理ターゲットプラン / FramePlan | [`compileDefaultVulkanVariant()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L368) |
| GPU 登録と世代 publish | 画像実体・buffer・shader・pipeline・descriptor を作り、不変な publication root を差し替える | [`registerPreparedRenderingPassConfigVariant()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L377) / [`prepareGeneration()`](../../src/core/renderingpass/framegraphruntime.cpp#L342) |
| pass dispatch | pass 種別を Material、Fullscreen、UI などの renderer へ振り分ける | [`renderDynamicPassDrawCalls()`](../../src/core/vkcore/render_pass_dispatch.cpp#L195) |
| Vulkan backend | device、frame target、image layout、pipeline、GPU resource を扱う | [`VulkanManageCore::VulkanManageCore()`](../../src/core/vkcore/core.cpp#L1027) |

**ただし、この 5 層は一本道ではありません。**「JSON → 論理グラフ → 変換 → 物理プラン → Vulkan」という直線で読むと必ず外します。実際は [`FrameGraphDefinition`](../../src/project/frameplan.hpp#L117) が **ハブ** で、そこから 2 本が **並行に** 枝分かれします。
以前は 3 本目に `FrameExecutionPlan`(WP238a。FramePlan から導いた backend 非依存の写しで、FramePlan との一致検証と診断 JSON への出力だけをし、コマンド記録は駆動しませんでした)がありましたが、✅RC-2(`e15e31bb5`)で型・生成・一致検証・dump の `execution_plan` ごと削除しました(経緯は [design_plan_layer_inventory.md](../design_plan_layer_inventory.md) §2-1)。

```mermaid
flowchart TB
    Input["rendering JSON (ProjectBasicConfig)"]
    Register["registerRenderingPassConfigVariantsData()<br/>flat / #xr / preview を 1 つの program へまとめて渡す"]
    subgraph CPU["RenderCompilerProgram — GPU に一切触らない CPU 相"]
        Resolve["(1) resolveRenderPipeline() → compileRenderPipeline()<br/>(2) 変換 registry"]
        Definition["(3) 型付き定義へ parse<br/>★ FrameGraphDefinition ★"]
        Targets["(4a) compileRenderingTargetPlansForVulkanDevice()"]
        Logical["CompiledLogicalRenderGraph<br/>SSA 値・型・read footprint"]
        Physical["VulkanTargetPlan<br/>scope / format / sample 数 / view execution / alias"]
        Planner["(4b) framePlansByName()<br/>= planFrameGraph()"]
        Plan["FramePlan<br/>nodes / barriers<br/>★実行順の権威★"]
        Resolve --> Definition
        Definition --> Targets --> Logical --> Physical
        Definition --> Planner --> Plan
    end
    Validate["physical_package->validate(frame_graph_names)<br/>GPU 変更前に必ず通す"]
    GPU["GPU 登録トランザクション<br/>gpuRegistrationMutex で直列化<br/>下記の 1〜5"]
    Render["Renderer::renderLogicalFrame()<br/>view family 解決 → scope schedule 構築 → node 記録"]
    Input --> Register --> Resolve
    CPU --> Validate --> GPU --> Render
```

- **authoring 解決**: `resolveRenderPipeline()` の後に `compileRenderPipeline()` が `CompiledRenderPipeline` を作る。

GPU 登録トランザクションから publication root までの順序:

1. `prepareGeneration`
2. `validate`
3. `before_publish`
4. `publish(単一 CAS)`
5. `RendererRuntimeGeneration` — 不変な publication root。

- **CompiledRenderPipeline**: feature / route / sample / target policy。
- **変換 registry**: render strategy / graph transform / subgraph replace。いずれも config JSON → config JSON で、論理グラフは書き換えない。
- **参照ファイル**: renderingpassconfigregistration.cpp、frameplanner.hpp(`framePlanToJson`。core の計画用 parse は ✅RC-3c1b で消えた)、`src/project/rendergraphparse.hpp` / `rendergraphir.hpp`(計画の唯一の JSON parser と IR。§6.7 の「中立な描画グラフ」)、`src/project/frameplan.hpp`(FramePlan の型と planner。✅RC-3c1a で core から移動)、framegraphruntime.hpp。

2 本の関係で、先に知っておくべきことが 3 つあります。

- **実行順・barrier の権威は [`planFrameGraph()`](../../src/project/frameplan.cpp#L473) です**。論理グラフ経路は planner を置き換えていません。論理/物理プランが決めるのは「その順序に載る**形・フォーマット・scope・view 実行**」の側です(§6.3 と併読)。
- **論理グラフは runtime に保存されません**。`compileLogicalFrameGraphShadow()` を呼ぶのは project の [`toLogicalGraph()`](../../src/project/rendergraphir.cpp#L723) だけで(✅RC-3c1b)、その本番の呼び手は変換 registry の契約検証([`logicalGraphsOf()`](../../src/core/renderingpass/graphtransformregistry.cpp#L238) と subgraph の同名の関数)、editor の restart の前の検査、[物理ターゲット計画が論理 graph を作る所](../../src/core/renderingpass/renderingsamplecount.cpp#L1650) です。`CompiledFrameGraphExecution`([`CompiledFrameGraphExecution`](../../src/core/renderingpass/framegraphruntime.hpp#L112))が持つのは `plan`(FramePlan)/ `target_plan` / `native_scopes` といった物理側のほうで、論理グラフは入っていません。ただし WP238e 以降、論理グラフは**コンパイル成果物の中では**生き残ります — `RenderingTargetPlanVerificationContext::logical_graph` が `shared_ptr` で保持し、完全物理プランの検証入力になります(§6.1 の難所「物理プランは『置換可能な完全パッケージ』」)。runtime 世代へ publish されない、という意味は変わりません。
- **backend は enum ではなく文字列**です。`RenderCompilerBackendPhysicalPackage::backend()` は `std::string_view` を返し、[`rendercompilerprogram.hpp` 内](../../src/core/renderingpass/rendercompilerprogram.hpp#L41) のコメントが理由を明示しています。

> Physical packages are an open backend boundary. A future Metal package can
> derive from this class without adding a fake Metal alternative to a central
> RHI variant.

## 6.1 設定から1フレームの実行計画ができるまで

**この節の語**: **lowering** は宣言を backend が扱う形へ落とす処理、**IR** はその途中の中間表現で、[共通グラフの流れ](03_project_and_loading.md#graph-dataflow)を参照してください。

起点は [`loadDefaultRenderingPassFromConfig()`](../../src/core/vkcore/renderer_config.cpp#L586) です。`ProjectBasicConfig` が保持する rendering JSON と現在の出力サイズを使い、[`registerRenderingPassConfigVariantsData()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1483) へ入ります。ここから先の合成・変換・計画は、この関数が自分で行うのではなく **1 つの compiler program へ委譲されます**(冒頭の層表)。

### graph variant: flat、`#xr`、preview

現在の実体は [`loadRenderGraphVariantsFromConfig()`](../../src/core/vkcore/renderer_config.cpp) です。flat 用の rendering pass に加え、XR active 時は同じ設定から suffix `#xr` 付きの rendering pass をもう一つ合成します。WP192 以降、XR/preview の feature 除外と検証は callback ではなく、project compiler の [`CompiledGraphVariantPolicy`](../../src/project/graphvariantpolicy.hpp) が所有します。

`compileGraphVariantPolicy()` は history/jitter、view family/execution、resource layout、terminal、mirror、suffix を一つの値へ解決し、除外には typed reason を残します。除外名は引き続き `xr_excluded_features` として記録されます。`Renderer` は [`flat_rendering_pass_id` / `xr_rendering_pass_id`](../../src/core/vkcore/renderer.hpp) を持ち、[`selectGraphVariant()`](../../src/core/vkcore/renderer.hpp) で切り替えます。

WP172 で **第3の variant「preview」** が加わりました。同じ `loadRenderGraphVariantsFromConfig()` が起動時に flat / xr と同じ 1 回の registration を呼び([family を登録する所](../../src/core/vkcore/renderer_config.cpp#L753))、preview の program もそこで作られます。registration は compiler program の preview の variant(data_only)の出力(composed config と IR)を [`previewGraphProgramOf()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1586) に渡し、これが [`makePreviewGraphProgram()`](../../src/core/renderingpass/previewgraph.cpp#L117) で program を作るか、作れなければ(PV-1)unavailable の program にします。compiler の data_only の枝が参照の解決(`resolveRenderGraphReferences()`)を通すので、preview の program は必ずそれを通ります。✅RC-3c1b から preview の program を作る道はこの 1 つだけです。前にあった単独の入口 `precompilePreviewGraph()` は RC-3c1b で消えました(消す前の時点で本番の呼び手は無く、呼んでいたのは test でした。test は registration と同じ道を通る形に置き換わっています)。ただし preview は他の 2 つとは**種類が違います**。ヘッダのコメントが規範です([`previewgraph.hpp` 内](../../src/core/renderingpass/previewgraph.hpp#L19))。

> A third, startup-compiled graph program.  Unlike flat/xr RenderingPassId it
> is data-only: render_preview executes it against request-local resources and
> therefore never enters Renderer::renderLogicalFrame.

つまり preview には `RenderingPassId` も `CompiledRenderingPass` もありません。flat / `#xr` が「GPU object まで compile し終えた実行物」なのに対し、preview は**合成と検証だけを終えた設定データ**で止まります(shader も pipeline も descriptor も作りません)。[`PreviewGraphProgram`](../../src/core/renderingpass/previewgraph.hpp) は `name` / `generation` / `pass_names` / `excluded_feature_names` / `graph_variant_policy` / `composed_config` を持つ値です。

feature decision、swapchain→request-local capture の変換、設定検証は [`graphvariantpolicy.cpp`](../../src/project/graphvariantpolicy.cpp) の同じ builtin policy 経路を通ります。`Renderer` は [`preview_graph_program`](../../src/core/vkcore/renderer.hpp) を保持し、[`previewGraphProgram()`](../../src/core/vkcore/renderer.hpp) と [`previewIsolationStateJson()`](../../src/core/vkcore/renderer.cpp) で公開します。実行側は §6.19 を参照してください。

**preview を組めないとき(✅PV-1、`a173e70c`)。**`PreviewGraphProgram` に `available`(既定 `true`)と `unavailable_reason` が加わりました。`validatePreviewConfig()` の拒否は [`PreviewUnsupportedAuthoringError`](../../src/project/graphvariantpolicy.hpp#L235)(`feature` / `target` / `graph` / `pass` を持つ `runtime_error`。文は前と同じ)になり、[Vulkan の compiler program の PV-1 の分岐](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L607) は preview の `data_only` の要求が投げた例外を `RenderCompilerProgramVariantOutput::unavailable` に入れて返します(`validateVariantOutput()` は unavailable の出力が `data_only` で、ほかが空であることを検査します)。

✅RG-3(`7a12fc2a`)から、この分岐が受け止めるのは `PreviewUnsupportedAuthoringError` と、同じ呼び出しで preview でない変種が組めた後(局所の `runtime_variant_composed` が立った後)の失敗だけで、それ以外は flat と同じように投げ直します([RG-3 で投げ直す所](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L632))。PV-1 が全部を受け止めていたため、`does not own field 'gpu_draw_source'` のような authoring の拒否が外に出なくなっていた件の直しです(commit の記録)。

登録の段は、それか `makePreviewGraphProgram()` の失敗を [`previewUnsupportedAuthoringReason()`](../../src/core/renderingpass/previewgraph.cpp#L71) で reason にし、[`unavailablePreviewGraphProgram()`](../../src/core/renderingpass/previewgraph.cpp#L105) を保持します。reason の `feature` は例外の型で決まり、`PreviewUnsupportedAuthoringError` 以外は `composition` で、`GraphRegistryError` か `<code>: <fields>` の形なら `cause` が付きます。

状態の JSON は `previewGraphStatusJson()` の 1 か所で作り、`get_status`・`get_frame_plan`・render の編集の committed(`RenderConfigRuntimeSnapshot::preview` 経由)が同じ値を出します。編集の preflight の [`preflightRenderGraphVariants()`](../../src/core/renderingpass/previewgraph.cpp#L171) は preview を組まなくなりました(flat と、起動時に XR を始めた engine の xr だけ)。利用者から見た形は manual 第6章 §6.13 の PV-1 の段落です。

> 🧩 **難所 — preview 除外は 1 語差**([`unsafePreviewDirectPassSurface()`](../../src/project/graphvariantpolicy.cpp) / [`validatePreviewConfig()`](../../src/project/graphvariantpolicy.cpp))
>
> **何をする所か**: preview graph から時間依存(TAA / velocity / jitter)・UI・mirror・present を含むものを部分文字列一致で締め出し、base graph の正規終端 pass だけを残します。
>
> **素朴に読むと**: マーカー配列が 2 つあり、**違いは `"present"` の有無だけ**です(8 個 vs 7 個)。[`unsafePreviewName()`](../../src/project/graphvariantpolicy.cpp)(8 個)が掛かるのは feature 名・feature の render target 名に加えて **feature が宣言した pass の `name` / `type` / `insert`** で、`unsafePreviewDirectPassSurface()`(7 個)が掛かるのは**合成後 config の pass 名/型だけ**です。つまり feature 由来の pass 名は `present` を含む厳しい側(8 個)で落とされ、緩い側(7 個)は合成後 config の pass にしか掛かりません。正規パイプラインの終端は慣習的に `present` を含む名前なので、合成後の pass 側だけ緩めてあります。片方に揃えて「重複を整理」すると、preview が終端 pass ごと落ちて何も描かないか、present 系 feature を通してしまうかのどちらかに倒れます。除外が **feature 単位で原子的**なのも意図で、合成後に pass を削ると insert の挿入先が宙に浮いて composer の依存/挿入先の検証が無意味になるからです。副作用として、除外された feature は `hdr_enabled` の判定にも `projection_jitter` provider の登録にも参加しません(`composeRenderFeatureConfig()` の feature ループが判定前に `continue` する)。**feature を 1 つ外すと canonical パイプラインの形そのものが変わります**。
>
> **骨子**:
> ```text
> unsafePreviewName              = {taa, velocity, motion_vector, projection_jitter, ui, imgui, mirror, present}
> unsafePreviewDirectPassSurface = {taa, velocity, motion_vector, projection_jitter, ui, imgui, mirror}
> retained_terminal = 出力が preview_capture|display && 名前が "present" で終わる
>                     && "mirror" も "ui" も含まない
> ```
>
> - **unsafePreviewDirectPassSurface**: `present` が無い。
>
> **✅RC-5s で 3 つ目の配列**: [`unsafePreviewPassName(value, include_present)`](../../src/project/graphvariantpolicy.cpp#L131) は `{taa, velocity, motion_vector, projection_jitter, mirror}`(+ `include_present` なら `present`)で、**`ui` / `imgui` を持ちません**。pass の**名前**はこれで見ます: feature が宣言した pass の名前は `include_present = true`([`declaresUnsafePreviewPass()`](../../src/project/graphvariantpolicy.cpp#L211))、合成後 config の pass の名前は `false`(`validatePreviewConfig()`)。pass の**型**は今までどおり `unsafePreviewName()` / `unsafePreviewDirectPassSurface()` です。UI は名前ではなく実装の型(`ui` / `imgui` の pass)で見る、がコードの注記で、`decideGraphVariantFeature()` の `user_interface` も `declaresPassType(feature, "ui")` か `"imgui"` だけになりました([判定](../../src/project/graphvariantpolicy.cpp#L643))。消えた `declaresUiAnchor()` は `insert` が `after:pelican_ui` / `before:pelican_ui` と完全一致する feature を UI と見ていました。これで `pelican_ui` という名前の実在する fullscreen の pass とそれに依存する feature が flat / preview / XR で残ります(test は第7章の表)。依存の伝播([`usedExcludedFeature()`](../../src/project/graphvariantpolicy.cpp#L797))も、`insert` の名前が旧い anchor 名でも特別扱いせず、除外された feature の pass なら依存にします。
>
> **手がかり**: [`resolveRenderPipeline()`](../../src/project/renderpipeline.cpp) の compose → `transformGraphVariantConfig()` → `validateGraphVariantConfig()` は順序が仕様です。先に `swapchain` を `preview_capture` へ書き換えるからこそ `retained_terminal` が成立しえます。`makePreviewGraphProgram()` が呼ぶ `generationOf()` が **FNV-1a**(Fowler–Noll–Vo ハッシュの 1a 版 — 1 バイトごとに「XOR してから固定の素数を掛ける」を繰り返すだけの非暗号学的ハッシュ。依存ライブラリなしで数行で書けるので、設定が変わったかどうかの判定に使われます)を **53 bit にマスクし、0 なら 1 に繰り上げる**のは、JSON の number(double)で正確に表せる上限と「program 無し」の予約値のためで、飾りではありません。テストは [`editorpreview_test.cpp`](../../test/editorpreview_test.cpp)。
>
> **不変条件**: 2 つのマーカー配列の差分は意図的です。片方を編集したら、もう片方の意味を明文化すること。除外は typed policy decision を composer の feature-envelope 境界へ適用し(composer の検証を残す)、合成後の削除に置き換えないこと。

<a id="preset-envelope-既定の-rendering-config-は-pipelinepreset--features-の-2-キーwp240a"></a>

### preset の展開経路

preset の schema・合成・fork の公開契約は[第6章](../manual/06_rendering.md#render-preset)を参照してください。

[`resolveRenderPipelinePreset()`](../../src/project/renderpipeline.cpp#L1140) は [`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L3211) の冒頭で呼ばれ、feature 解析より前に展開します。`defaultGraphPassFamily()` も既定 family を取得するため展開しますが、その結果は family 名だけに使い、文書を書き換えません。

**レシピ(`@2`)の展開(✅RC-5a `4001e48af`。実験)。**利用者から見た契約は [第6章「preset のレシピ」](../manual/06_rendering.md#render-recipe)、設計の経緯は [recipe メモ §7](../design_render_preset_feature_recipe.md#rc5-implementation) です。

- **入口は版の分岐 1 つ**: `resolveRenderPipelinePreset()` は envelope の検査の後、`version == 2` のときだけ [`expandRenderPipelineRecipe()`](../../src/project/renderpipelinerecipe.cpp#L68) に `config` と root の `fragment_overrides` を渡し、返った config で preset の `config` を置き換えてから、root の overlay から `fragment_overrides` を消します([版の分岐](../../src/project/renderpipeline.cpp#L1207))。その後の正規化・root の許可キーの検査・配列の追記などは `@1` の道のままです。`@1` の root と preset を選ばない root の `fragment_overrides` は `render_pipeline.fragment_overrides_need_recipe@1`。
- **展開は純関数**([`renderpipelinerecipe.cpp`](../../src/project/renderpipelinerecipe.cpp#L68)、`pelican_project` に入る): 許可キーの検査 → `base` の family ごとに断片を読み([`loadFragment()`](../../src/project/renderpipelinerecipe.cpp#L31)。schema・version・キー・配列の形)→ `fragment_overrides` を元の位置で適用(`null` は飛ばす。置き換え先の `name` の一致を検査)→ 実効の断片の `render_targets` と `passes` を順に連結 → 全 family を通した pass 名と target 名の重複の検査(`recipe_duplicate_name@1`)→ `required_targets` の検査(`fragment_missing_target@1`)。断片の読み込みには preset と同じ loader を使います。
- **所有は compiler の JSON の外**: 展開は config のほかに、pass 名 / target 名 → [`RenderPipelineFragmentOrigin`](../../src/project/renderpipeline.hpp#L199)(`{name, reference}`)の 2 つの表を [`RenderPipelinePresetResolution`](../../src/project/renderpipeline.hpp#L215) で返します。compose は `synchronizeRenderPipelineProvenance()` で `source` を決めた直後に、その表から `RenderPassProvenance::fragment` / `RenderResourceProvenance::fragment` を付けます([付ける所](../../src/project/featurecompose.cpp#L3345))。`source` の決め方は変えず(断片の pass も preset の pass と同じ `source`)、[plan の JSON に `fragment` を出す所](../../src/core/renderingpass/frameplanner.cpp#L160)が `get_frame_plan` の node と resource に任意の `fragment` 欄として出します。`ResourcePattern.provenance` と fingerprint には入りません。
- **editor の preset の bytes**: `@2` では loader が断片も読むので、「最後に読んだ文書の bytes を preset の bytes として持つ」作りだと最後の断片で上書きされます。RC-5a のコードレビューで、`editorruntimefactory.cpp` の `load_pipeline_json` は root が選ぶ preset の参照を読んだときだけ `pipeline_preset_bytes` を保存する形に直りました([選ぶ preset の参照を取る所](../../src/core/communication/editorruntimefactory.cpp#L2274))。test の `makeProjectService` の helper も同じ形に直しています(RC-5b)。

hybrid の描画経路では deferred と forward が同じ target へ重ねて描き、半透明は snapshot_copy が退避した画像を screen input として読みます。

<a id="render-generation-flow"></a>

登録処理の順序には意味があります。

1. feature を基本設定へ合成する(preset を選んでいれば、その展開はこの直前で済んでいます)。
2. render target 定義を parse して、画像と view を作る。
3. frame graph buffer 定義を parse して、必要な buffer を作る。
4. target 名を ID、format、image view へ解決する resolver を作る。
5. render pass と compute task の純粋な定義を parse する。
6. shader、pipeline、descriptor を作り、runtime 用の `CompiledPass` / `CompiledComputeTask` にする。
7. frame graph を計画し、名前を実際の pass/task index に bind する。

実コードではこの順序が [`renderingpassconfigregistration.cpp` の一続きの処理](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L235) になっています。先に target と buffer を作るのは、pass の format、descriptor image view、compute resource の存在確認に必要だからです。

<a id="composition-order"></a>

> 🧩 **難所 — 合成の順の決まり方(✅RC-5s で区画と鎖を撤去)**([`initializeCanonicalColorPipeline()`](../../src/project/featurecompose.cpp#L522) / [`insertPassByAnchor()`](../../src/project/featurecompose.cpp#L2121) / [`placeRenderGraphPassRun()`](../../src/project/rendergraphcomposition.cpp#L1438))
>
> **何をする所か**: 手順 1 の中身です。base の pass の列に HDR の付け替えと終端 `output_transform` を足し、feature の `insert` と graph の置き場所を配列上の実位置へ解決して、planner が読む明示依存(`after` / `before`)に落とします。RC-5s(develop_v1 `1e05031d7` / `e17754510`)の前は、ここで base の pass を 8 つの canonical anchor の区画(bucket)へ振り分け(`canonicalBucket()` / `canonicalizePasses()`)、空のノード `__anchor_*` を作り、合成の最後に区画の中を宣言順の `after` の鎖で結んでいました(`enforceCanonicalOrder()`)。3 つとも消えました。利用者の決定(2026-10-28)は、鎖が決めていた細かい順は守らず、読み替えの層も作らない、です。
>
> **素朴に読むと**: 引っ掛かる所は 4 つです。
>
> 1. **配列の位置と辺は別物**: feature の `after:X` は X の直後(添字 + 1)に宣言して `after: [X]` を 1 本足すだけ、`before:X` は X の直前に宣言して `before: [X]` を足すだけです。同じ X に後から挿した feature の pass ほど X の近くに宣言されます。planner は辺で決まらない所を宣言の順([`topologicalOrder()`](../../src/project/frameplan.cpp#L299) の `declaration_index`)で選ぶので、依存の無い 2 つの feature の pass は、後から挿した方が先に走りえます。RC-5s の前は canonical anchor へ挿したときだけ区画の末尾まで添字を進めて宣言の順を保っていましたが、その非対称も消えました。
> 2. **graph の run とループの run は辺の付け方が違う**: graph の pass の run(同じ family・同じ向き・同じ pass を名指す)は、各 pass に指定した側の辺だけを足して続けて宣言します([`run.loop` の分岐](../../src/project/rendergraphcomposition.cpp#L1463))。run の中を宣言順の鎖で結ばないので、依存の無い 2 つは順が決まりません。ループの run は [`insertRenderPassRunAtAnchor()`](../../src/project/featurecompose.cpp#L2080) が全部の回に「直前の pass への `after`」と「直後の pass への `before`」を足し、さらに [`resolveGraphDocumentAfterComposition()`](../../src/project/rendergraphcomposition.cpp#L1635) が `pass_replacements` で run が動いた後の最終の位置で付け直します(先に付けると置き換えで閉路ができた、と実装者の報告)。
> 3. **終端の保証は辺**: [`appendOutputTransforms()`](../../src/project/featurecompose.cpp#L336) は family の末尾に置くだけで、待つ辺は [`enforceTerminalAfterComputeTasks()`](../../src/project/featurecompose.cpp#L360) が family の他の全 pass と全 compute task に足し、[`finalizeRenderGraph()`](../../src/project/rendergraphir.cpp#L1046) が provider と合成した task が出そろった後にもう一度全 node へ足します。配列の末尾にあることには頼りません(ImGui の `imgui_pass` は [`appendImGuiPassToCanonicalGraphs()`](../../src/core/imgui/imguiruntime.cpp#L26) が `output_transform` の**直前**に挿し、それまでの全 pass を待たせ、`output_transform` に `imgui_pass` を待たせます)。
> 4. **HDR の終端**: [`prepareHdrSceneOutput()`](../../src/project/featurecompose.cpp#L299) は feature を足す前に呼ばれ、type が `ui` / `debug_draw` / `gizmo` / `debug_text` でない**最後の** swapchain writer を scene の終端として、その出力を `scene_ldr_in` へ付け替えます。swapchain へ書く役目は `end` で挿される feature 側の `hdr_tonemap` が引き継ぎます。区画は関与しません。
>
> **骨子**:
> ```text
> initializeCanonicalColorPipeline(config, hdr):
>   addFormatClassesAndDisplay       # display を足す
>   hdr なら prepareHdrSceneOutput   # 最後の scene の swapchain writer → scene_ldr_in
>   appendOutputTransforms           # family ごとに末尾へ output_transform
> addFeaturePasses: insert が
>   begin    → 先頭(family が 1 本のときだけ)
>   end      → output_transform の直前(同上)
>   after:X  → X の直後、pass.after  += X
>   before:X → X の直前、pass.before += X
>   X が無い: 旧 anchor 名なら canonical_anchor_removed@1、それ以外は insertion_anchor_missing@1
> 合成の最後: enforceTerminalAfterComputeTasks   # output_transform.after += 他の全 pass と全 task
> finalizeRenderGraph の最後: output_transform.after += 全 node; applyFrameNodeOrder
> ```
>
> **具体例**: `hybrid_v1`(`… forward_transparent, scene_present`)に同梱の [`velocity.json`](../../src/core/resources/features/velocity.json)(`"insert": "begin"`)と [`taa.json`](../../src/core/resources/features/taa.json)(`taa_resolve` が `"before:scene_present"`、`taa_composite` が `"after:taa_resolve"`)を、この順で合成すると配列はこう動きます。
>
> | 段 | 挿入したもの・指定 | その後の配列(抜粋) |
> |---|---|---|
> | 0: 初期化の直後 | — | `deferred_geometry, …, forward_transparent, scene_present, output_transform` |
> | 1 | `velocity_pass` / `begin` | `velocity_pass, deferred_geometry, …, scene_present, output_transform` |
> | 2 | `taa_resolve` / `before:scene_present` | `… forward_transparent, taa_resolve, scene_present, …`(`taa_resolve.before += scene_present`) |
> | 3 | `taa_composite` / `after:taa_resolve` | `… taa_resolve, taa_composite, scene_present, …`(`taa_composite.after += taa_resolve`) |
>
> RC-5s の前は `velocity_pass` が `before:post_main`、`taa_resolve` が `after:post_main` で、依存名には `__anchor_post_main` が入っていました。
>
> **手がかり**: [`findRenderPassAnchorMatches()`](../../src/project/featurecompose.cpp#L2064) は pass の名前だけを見ます(前は第 1 段で `type == "canonical_anchor"` の node を見て、無いときだけ pass 名)。旧い anchor 名と同じ名前の実 pass があればそれへの配置です。旧い書き方の拒否は [`canonicalanchorremoved.hpp`](../../src/project/canonicalanchorremoved.hpp#L9)(第3章「render feature」)。予約語は pass 名 `output_transform` と `__anchor_` 接頭辞、render target 名 `display` です(衝突は例外)。[`retargetSwapchainAliases()`](../../src/project/featurecompose.cpp#L349) は `output_transform` **以外**の pass の `swapchain` を `display` に置換するので、合成後に `swapchain` を書くのは終端だけになります。テストは [`rc5s_anchor_removal_test.inc`](../../test/rc5s_anchor_removal_test.inc#L18)(`rendercompilerprogram_test` に入る)と `featurecompose_test` の「RC-5s every loop member waits at both boundaries despite a compute wait」。
>
> **不変条件**: 順を決めるのは辺(資源の依存・`after` / `before`・`frame_node_order`)と宣言の順だけです。エンジンに推測の規則を足さないこと。見た目を保つための直しは、engine の feature と出荷の文書の側に明示の `after` / 資源の依存 / `insert` を書いて行いました(RC-5s の仕様の裁定 S1。例: `example` の `scene_present`、`sprite_demo` の明示の binding)。

**`frame_node_order` の当て方(✅FO-1 `a4d7760a` → ✅RC-5s)。**FO-1 は `enforceCanonicalOrder(passes, listed)` が区画の中で並びの名指した pass を並びの順に埋め直して鎖にしていました。RC-5s で鎖そのものが無くなったので、並びは compile の [`applyFrameNodeOrder()`](../../src/project/rendergraphir.cpp#L996) が名指した node の隣どうしに `after` を 1 本ずつ足すだけです(下の「`frame_node_order` と依存の照合」)。

- `passes` の配列は今も並べ替えません。planner の自動の data edge は宣言順の `last_writer` から張られる(§6.3)ので、配列を動かすと読む・書くの依存が変わってしまうためです。
- 並びに `__anchor_` で始まる名前があれば、[`parseFrameNodeOrder()`](../../src/project/rendergraphparse.cpp#L1924) が読み飛ばし(重複の検査より前)、[`resolveRenderPipeline()`](../../src/project/renderpipeline.cpp#L1446) が名前ごとに `RenderPipelineDiagnosticKind::anchor_ignored` の diagnostic(`detail_fields` は `{severity: "warning", graph, node}`)を足します。`get_frame_plan` の `diagnostics` では kind が `frame_node_order.anchor_ignored@1` です([kind の名前](../../src/core/renderingpass/frameplanner.cpp#L100))。

**合成の出口で見る image の鍵(✅FO-1)。**`synchronizeRenderPipelineProvenance()` の `render_targets[]` の走査が、EB-1 の buffer の鍵の検査の隣で [`requireKnownFrameGraphImageKeys()`](../../src/project/renderpipeline.cpp#L103) を呼びます([image の鍵の検査を呼ぶ所(FO-1)](../../src/project/renderpipeline.cpp#L2366))。知らない鍵は `graph.image_key_unknown@1 {image, key, allowed}` の `GraphRegistryError` です。許す 18 個は `frame_graph_image_keys` の配列で、render target の parser と format の解決器が読む鍵に、IA-1 の `initial` と、feature の binding の制約(`validateTargetBinding`)が読む `sample_count` と、EN-2 の `source`(`render.image` が読む project の画像ファイル)を足したものです(コードのコメント)。

> 🧩 **難所 — format_class が format を上書き**([`resolveRenderTargetFormatClassesV2()`](../../src/core/renderingpass/rendertargetjsonparser.cpp#L17))
>
> **何をする所か**: 手順 1 と 2 のちょうど間で走ります。render target 宣言の `format_class`(`scene` / `display` / `data` / `explicit(...)`)を HDR の有無と frame target の実サイズから実フォーマットへ解決する、カノニカルなカラーパイプラインの唯一の決定点です。
>
> **素朴に読むと**: 引っ掛かる所は 3 つです。
>
> 1. **authored format が捨てられる**: JSON に `"format": "B8G8R8A8_UNORM"` と書いてあるのに **採用されない**ことがあります。`scene` と `display` では `target["format"]` が**無条件に上書き**され、authored 値は捨てられます(残るのは `data` と `explicit(...)` だけ。[`display` は `B8G8R8A8_SRGB` 固定](../../src/core/renderingpass/rendertargetjsonparser.cpp#L42)、[`scene` は HDR の有無で `R16G16B16A16_SFLOAT` / `B8G8R8A8_SRGB`](../../src/core/renderingpass/rendertargetjsonparser.cpp#L46))。
> 2. **名前と中身が食い違って見える実例**: HDR 有効時に `prepareHdrSceneOutput()` が足す `scene_ldr_in` は `B8G8R8A8_UNORM` と書かれていますが `format_class: "scene"` なので `R16G16B16A16_SFLOAT` に化けます — **`ldr` を含む名前なのに float16** です。名前のほうは嘘ではなく「LDR 化(= tonemap)の**入力**」の意で、中身は HDR の scene です(`hdr_tonemap` がこれを読んで `display` へ書きます)。嘘になるのは authored format だけです。
> 3. **使われない引数**: この関数は `frame_target_format` を引数に取りながら**本体で一度も参照していません**。呼び出し側は `getSwapchainFormat()` を渡していますが `display` は常に `B8G8R8A8_SRGB` 固定で、swapchain 側が UNORM に落ちた差は終端の shader fallback が吸収します。「引数が使われていないのはバグでは?」で止まらないでください。
>
> **骨子**:
> ```mermaid
> flowchart TB
>     Scene["scene pass"]
>     Display["display: B8G8R8A8_SRGB<br/>HW が OETF + 8bit 量子化"]
>     Output["output_transform.frag"]
>     Swapchain{"swapchain の形式"}
>     Srgb["SRGB: そのまま"]
>     Unorm["UNORM: shader が linearToSrgb()"]
>     Scene -->|"linear"| Display
>     Display -->|"sample = HW が EOTF(→ linear)"| Output
>     Output --> Swapchain
>     Swapchain --> Srgb
>     Swapchain --> Unorm
> ```
>
> **手がかり**: つまり **linear → 8bit sRGB → linear → 8bit sRGB** の往復が 1 回入ります(骨子の **OETF / EOTF** は opto-electronic / electro-optical transfer function の略で、前者は linear 値を sRGB のガンマ曲線へ載せる符号化、後者は符号化された値を linear へ戻す復号です。sRGB フォーマットの image へ書く / から読むと、どちらもハードウェアが自動で掛けます)。[`color_pipeline_test.cpp`](../../test/color_pipeline_test.cpp) が hardware 経路と shader fallback 経路の差を **±1 LSB**(least significant bit — 最下位ビット 1 つぶん、つまり 8 bit なら 256 階調で 1 段の差)で許容しているのはこのためで、「無駄だから `display` を UNORM に」と最適化すると中間段の量子化が linear 空間になり暗部が壊れます。`format_class` 省略時の推論 [`inferFormatClass()`](../../src/project/featurecompose.cpp#L237) は **名前の部分一致**(`normal` / `depth` / `shadow` / `ssao` / `worldpos` / `material` を含めば `data`)という素朴な規則なので、target 名を変えると色空間が変わりえます。
>
> **不変条件**: `scene` / `display` の実フォーマットを決めるのはこの関数だけです。authored `format` に意味を持たせないこと。中間の `display` は sRGB エンコード済み 8 bit のまま(linear 8 bit にしない)。

```mermaid
flowchart LR
    JSON["rendering JSON"]
    Feature["feature composition"]
    Target["RenderTargetContainer"]
    Buffer["FrameGraphResourceContainer"]
    Def["Pass / Compute definitions"]
    Compiled["CompiledRenderingPass"]
    Plan["FramePlan"]
    Runtime["CompiledFrameGraphExecution"]

    JSON --> Feature
    Feature --> Target
    Feature --> Buffer
    Feature --> Def
    Target --> Compiled
    Buffer --> Compiled
    Def --> Compiled
    Feature --> Plan
    Plan --> Runtime
    Compiled --> Runtime
```

### rendering config の root は `pelican.graph` だけ(P-2a-4a / P-2a-4b / MG-2b)

rendering config は、共通グラフ文書(`schema` / `version` / `definitions` / `nodes` / `outputs` の 5 キー)の `rendering` メンバの下に入ります。出荷 project 7 本と雛形はすべてこの形です。

P-2a-4b までは、`features` / `render_targets` / `rendering_passes` … を最上位に直接並べた **legacy root** も受けていました。MG-2b でその読み手を撤去し、legacy root は読み込み・overlay・ray tracing の設定・編集文書のどこでも `render_config.legacy_root@1`([`RenderConfigLegacyRoot`](../../src/project/renderconfigdocument.hpp#L56))で拒否されます。

読むときの要点は「authored key の住所を決める `if` を各所が持たない」ことです。住所を決めるのは [`renderconfigsection.hpp`](../../src/project/renderconfigsection.hpp#L12) の **3 つの名前(4 定義)**だけで(`isRenderGraphRoot()` は `ordered_json` 用の [overload](../../src/project/renderconfigsection.hpp#L27) を持ちます)、他の reader / writer はここを呼びます。

| 関数 | 役割 |
|---|---|
| [`isRenderGraphRoot()`](../../src/project/renderconfigsection.hpp#L22) | `schema == "pelican.graph"` かどうか |
| [`renderingSectionOf()`](../../src/project/renderconfigsection.hpp#L36) | 節そのものを返す。graph root でなければ `RenderConfigLegacyRoot`、object の `rendering` が無ければ `RenderConfigSectionUnlocated` を投げる |
| [`renderingSectionOrEmpty()`](../../src/project/renderconfigsection.hpp#L49) | **読み取り専用の寛容版**。節が無ければ空 object を返す(legacy root は断る) |

P-2a-4b にあった 4 本目の `wrapLegacyRenderingConfig()`(legacy config を graph root で包む関数)は MG-2b で撤去されました。

つまり **読みは節が無くても失敗せず、編集だけが名前付きで拒否される**という非対称が意図です。編集側の入口は [`AuthoredRenderConfigDocument::withSectionValue()`](../../src/project/renderconfigdocument.cpp#L809) で、byte を保ったまま `rendering.<key>` を差し替えます(graph root 自身のキーは root に書きます)。その byte 正確な locator の読みどころは [第9章 §9.24](09_black_magic_and_gotchas.md) の難所「byte 正確な節 locator と絶対 offset」にあります。

### feature のスカラーパラメータ: `binding: define` と `binding: buffer`(P-2a-2)

feature は `parameters.scalars[]` で数値パラメータを宣言できます。v0 で足されたのは、その値が**どの経路でシェーダへ届くか**の宣言で、[`ScalarParameterBinding`](../../src/project/featurecompose.cpp#L649) の 2 値です。

- `define`(既定) — 合成時に shader define へ焼き込みます。変更は再コンパイルを伴います。
- `buffer` — feature 自身が宣言した frame graph buffer を初期化します。値は毎フレーム、共通グラフの `render.scalar` sink が上書きできます。

同梱の [`engine://features/bloom.json`](../../src/core/resources/features/bloom.json) が両方の実例です。scalar は 3 つ(`threshold` / `knee` が `define`、`intensity` が `binding: "buffer"`)、buffer は `bloom.intensity`(`host_source: "runtime_f32"`、`size: 4`)1 本、render target 9 枚、pass は 13 本です。✅RC-5s から、最初の `HighLuminanceExtraction` が `insert: "end"`、残りの 12 本が 1 つ前の pass への `after:<pass>` で並びます(RC-5s の前は全 pass が `canonical_anchor: "post_ldr"` を宣言していました。§6.1 の難所「合成の順の決まり方」)。

✅RS-8 / RS-11(`5d065436`)から、bloom の shader は feature の外でも compile できます。`bloom_highpass.frag` は `PELICAN_FEATURE_BLOOM_THRESHOLD` / `PELICAN_FEATURE_BLOOM_KNEE` が無ければ `bloom.json` と同じ既定値 0.4 / 0.7 を自分で定義し([既定値](../../src/core/resources/bloom_highpass.frag#L12))、組み込みの SPIR-V も define を渡さずに作ります(`src/core/resources/CMakeLists.txt` の `embed_shader(bloom_highpass.frag)`)。

`bloom_composite.frag` は `bloomIntensity` の port が無ければ既定 2.0 を使います([分岐](../../src/core/resources/bloom_composite.frag#L24))。shader の既定値と `bloom.json` の既定値は 2 か所に書かれているので、一致は test が source から読んで比べます([test](../../test/vi1_variadic_inputs_test.cpp#L1468))。

> 🧩 **難所 — `binding: buffer` は 5 つの宣言が噛み合って初めて成立する**([binding の parse](../../src/project/featurecompose.cpp#L958) / [buffer の seed](../../src/project/featurecompose.cpp#L1589))
>
> **何をする所か**: `binding: "buffer"` と書かれた scalar の解決値を、feature が宣言した frame graph buffer の初期値として書き込みます。
>
> **素朴に読むと**: 「`buffer` にすれば実行時に変えられる」で終わりに見えますが、合成器は 5 条件を課し、1 つでも欠けると**無言で効かないのではなく合成時の named error** になります。(1) `shader_define: true` との併用は拒否 — define が、buffer が存在する理由そのものを凍結してしまうからです([コメント](../../src/project/featurecompose.cpp#L971))。(2) scalar の型が `float` であること(`binding 'buffer' requires scalar type float`、[型の門](../../src/project/featurecompose.cpp#L983))。(3) feature が `<feature 名>.<scalar 名>` という名前の buffer を宣言していること。この名前は [`featureScalarBufferName()`](../../src/project/featurecompose.cpp#L665) が導出し、`render.scalar{target}` が名指しする文字列と同一です — **二度書かない**のが規約です。(4) その buffer が `host_source: "runtime_f32"` かつ `size` がちょうど 4 であること。(5) 文書が `initial_f32` を**書いていない**こと。初期値は scalar から導出するものなので、authored な二重宣言は [「`initial_f32` is derived from the scalar, not authored」](../../src/project/featurecompose.cpp#L1613) で拒否されます。
>
> **手がかり**: 解決した値が buffer へ渡る経路は 2 段です。まず [scalar 解決のループ](../../src/project/featurecompose.cpp#L1474)が `(buffer 名, float 値)` を貯め、合成の最後に[その列を feature の `buffers` エントリへ書き戻し](../../src/project/featurecompose.cpp#L1618)ます。したがって「feature 文書を目で読んでも `initial_f32` が無い」のは正常で、**合成後の config にだけ現れます**。
>
> **不変条件**: `initial_f32` を feature 文書へ手で書かないこと。buffer 名の導出規則を変えるときは `render.scalar{target}` の綴りと同時に変えること(片側だけ変えると、上書きが届かない buffer が黙って残ります)。`shader_define` との排他を緩めないこと。

### 定義と実行物を分ける理由

中心型は core の `renderingpass.hpp`([`PassDefinition`](../../src/core/renderingpass/renderingpass.hpp#L265) ほか)にまとまっています。

- `PassDefinition` は target、load/store、clear、pass 種別を持つ、Vulkan object を含まない定義です。
- `CompiledPass` は定義と、登録済み renderer object を指す `PassId` の組です。
- `ComputeTaskDefinition` は shader、reads/writes、dispatch、schedule と view を持ちます。順序は持ちません(✅RC-4 で `after` / `before` の欄を削った。順は IR の辺と FramePlan)。
- `CompiledComputeTask` は定義と `ComputeTaskId` の組です。
- `CompiledRenderingPass` は render pass 群と compute task 群の実行可能なまとまりです。

これにより JSON parse と Vulkan object 生成が分離され、planner は GPU object を知らずにテストできます。

✅RC-4 から、このうち backend に依存しない leaf の型は project にあります: shader 参照([`shaderreference.hpp`](../../src/project/shaderreference.hpp#L40))、fullscreen / raster / 補助 pass / UI の info([`passinfo.hpp`](../../src/project/passinfo.hpp#L15))、[`PassLoopIndex`](../../src/project/passloopindex.hpp#L11)、`ComputeTaskDefinition`([`computetaskdefinition.hpp`](../../src/project/computetaskdefinition.hpp#L62))。`renderingpass.hpp` はそれらを include し、`PassInfo` の variant・`PassDefinition`・`CompiledComputeTask` は同じ型をそのまま持ちます(値の持ち主は変わっていない)。core に残るのは material の info と binding(generation の ID を持つ)、`ImGuiPassInfo`、`PassDefinition` 本体(`rasterization_samples` は `vk::SampleCountFlagBits`)です。置き場の一覧は §6.7 の「[実行用の leaf の型](#execution-leaf-types)」。

### ID の3種類を混同しない

[`PELICAN_DEFINE_HANDLE`](../../src/core/handle.hpp#L21) により、整数を別々の型に包んでいます。

| 型 | 指すもの |
|---|---|
| `RenderingPassId` | 複数 node を含む描画設定全体 |
| `PassId` | Material や Fullscreen など、個別 renderer 内の登録物 |
| `GlobalRenderTargetId` | offscreen target。`-1` はなし、`-2` は swapchain |
| `ComputeTaskId` | `ComputeTaskContainer` 内の task |

特殊値の意味は `renderingpass.hpp` の [`invalidRenderTargetIdValue`](../../src/core/renderingpass/renderingpass.hpp#L47) / `swapchainRenderTargetIdValue` と `noRenderTargetId()` などの関数に集約されています。整数値が同じでも型が違えば誤って渡せない、軽量な nominal typing です。

### コンパイラ program の内側

冒頭の層表の 1〜2 段目、つまり「GPU に触らない CPU 相」で何が決まるかを、読みにくい順に 4 つ挙げます。ここは設計意図が [`docs/design_render_graph_compiler.md`](../design_render_graph_compiler.md) に、拡張点の契約が [`docs/design_render_pipeline_extensibility.md`](../design_render_pipeline_extensibility.md) にあるので、以下は **なぜコードが読みにくいか** に絞ります。

> 🧩 **難所 — 型付きプランと config JSON は最後まで並走する**([`compileDefaultLogicalVariant()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L212) / [`compileRenderPipeline()`](../../src/project/renderpipeline.hpp#L696))
>
> **何をする所か**: authoring JSON を 1 回だけ解決して、**2 つの成果物**を同時に持ち回ります。型付きの [`CompiledRenderPipeline`](../../src/project/renderpipeline.hpp#L642)(runtime が読む)と、合成済みの config JSON(`ResolvedRenderPipeline::composed_config`。このあと `parseRenderGraph()` と provider の ABI へ流れる)です。✅RC-3c2b2 から、この JSON は compiler の中の一時の値で、compiler program の出力には載りません(`normalized_config` は消え、registration は IR だけを受けます)。
>
> **素朴に読むと**: `compileRenderPipeline()` を「JSON を型へ変換して終わり」と読むと外します。罠が 3 つあります。第一に、その入力である [`ResolvedRenderPipeline`](../../src/project/renderpipeline.hpp#L437) は **半分が JSON のまま**で(`projection_jitter` / `feature_instances` / `material_routing` / `draw_sort` が `nlohmann::json`)、型になるのは `compileRenderPipeline()` を通った後だけです。第二に、その `compileRenderPipeline()` は **変換 registry より前に呼ばれます**([呼び出し位置](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L297))。✅RC-3c2b2 から、呼ぶのは runtime なら `compose_runtime_config` の後で、改訂 0 の IR(`initial_graph`)を受け取り、material routing の各 route の schema / state をその IR から取ります(前は `compose_runtime_config` の前に JSON だけで呼んでいた)。graph transform / subgraph replacement はそのあとで **config JSON を書き換え**(✅RC-3c1b から、応答ごとに `parseRenderGraph()` で 1 回だけ IR にして次の要求へ渡します。§6.7 の「中立な描画グラフ」)、選ばれた provider の provenance は `compiled_pipeline_value.graph_transforms` / `.render_strategy` として型側へ **後から差し戻されます**([provenance を刻む所](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L355))。RC-3c1b から、同じ provenance は最終改訂の IR の各 `GraphIR` にも書かれ、論理 graph と物理計画の fingerprint はそちらから取ります。つまり「型を見れば config が分かる」も「config を見れば型が分かる」も成立しません。片側にだけフィールドを足すと、runtime の挙動と dump JSON が静かに食い違います。第三に、型から JSON へ戻す口は [`serializeCompiledRenderPipelineMetadata()`](../../src/project/renderpipeline.hpp#L707) **1 本だけ**で、そのコメントが規範です。
>
> > Dump-only compatibility serializer.  Runtime code must consume the typed
> > fields above rather than reading keys from this representation.
>
> **骨子**:
> ```mermaid
> flowchart TB
>     Resolve["resolveRenderPipeline(authored)<br/>ResolvedRenderPipeline<br/>composed_config(JSON) + 一部は型、一部は JSON"]
>     Initial["runtime なら compose_runtime_config(JSON)<br/>parseRenderGraph で改訂 0 の IR(initial_graph)"]
>     Compile["compileRenderPipeline(resolved, &initial_graph)<br/>CompiledRenderPipeline(全部型)<br/>初期 config の provenance もここで同期<br/>routing の schema / state は IR から"]
>     Preview{"preview(data_only)?"}
>     Return["早期 return<br/>IR は改訂 0 に resolveRenderGraphReferences だけ"]
>     Transform["runtime の JSON 変換<br/>下記の 1〜3"]
>     Record["選択結果を CompiledRenderPipeline と IR の GraphIR へ後追いで刻む"]
>     Sync["runtime: finalizeRenderGraph(IR)の後、synchronizeRenderGraphProvenance(IR)で最終 config 順へ再同期<br/>XR なら改名後の compute 名を使う(作者名で既存の provenance を引く)"]
>     Resolve --> Initial --> Compile --> Preview
>     Preview -->|"はい"| Return
>     Preview -->|"いいえ"| Transform --> Record --> Sync
> ```
>
> runtime の JSON 変換の順序:
>
> 1. `compose_runtime_config`(✅RC-3c2b2 から `compileRenderPipeline()` より前。preview には掛からない)
> 2. `graph transform`
> 3. `subgraph replacement`
>
> - **compose_runtime_config / graph transform / subgraph replacement**: JSON が変わる。compose_runtime_config の後に 1 回、provider の応答ごとに 1 回、`parseRenderGraph()` が IR を作る(RC-3c1b)。
>
> **手がかり**: preview variant が「合成と検証だけを終えた設定データ」で止まるのは、この関数の `data_only` 早期 return がその位置にあるからです(コメントが規範: 「resolve feature and strategy policy here, but do not apply runtime host additions or enter transform/subgraph/device planning that assumes concrete target storage」)。`normalize_config` フックが差さるのも `runtime_package` のときだけで([フックを差す分岐](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L260))、`format_class` の解決(§6.1)が preview に掛からない理由がここにあります。
>
> **不変条件**: runtime は型付きフィールドだけを読むこと(dump JSON をパースし直さない)。型と JSON の両方に意味を持つ値を足すときは、変換 registry の**後**に provenance を刻む側へ寄せること。

> 🧩 **難所 — 論理グラフは値に版を打つ(SSA)**([`compileLogicalFrameGraphShadow()`](../../src/project/logicalframegraphshadow.cpp#L39) / [`LogicalValueId`](../../src/project/logicalrendergraph.hpp#L67))
>
> **何をする所か**: `FrameGraphDefinition` の `reads` / `writes`(**リソース名**)を、`{resource, version}` という **値 ID** へ変換します。同じ `scene_color` でも書かれるたびに別の値になり、edge は名前ではなく値で張られます。
>
> **素朴に読むと**: planner(名前ベース)と物理計画(値ベース)の間に **二重の名前空間**が挟まっていることに気づかないと、「同じ RT なのになぜ別ライフタイムと判定できるのか」が読めません。version は write のたびに `current_versions` を ++ して進み([write ポートの版付け](../../src/project/logicalframegraphshadow.cpp#L460))、`load` 付き attachment の read+write は **`input_output` ポート 1 本に融合**されて 1 回のインクリメントになります([read+write の融合判定](../../src/project/logicalframegraphshadow.cpp#L351))。import は 4 分類([`LogicalValueImportKind`](../../src/project/logicalrendergraph.hpp#L80))で、`previous_epoch` が `@history`(§6.3 の難所「`@history` は edge を作らない」と同じ事実を論理層から見たもの)、`external` が swapchain、`legacy_implicit` は「旧 JSON が版を書いていない」ことの記録です。そして **read footprint は最適化ヒントではなく物理表現の決定入力**です。宣言があればそれを、`load` 付き attachment なら `same_pixel`、無ければ保守的に `arbitrary` を採り([footprint の既定選択](../../src/project/logicalframegraphshadow.cpp#L379))、`arbitrary` が 1 つでも出ると `legacy_read_footprint_conservative` decision が付きます。`same_pixel` でなければ tile-local にも scope 融合にも落ちません([第10章](10_background_knowledge.md) §10.1「LAZILY_ALLOCATED メモリと TRANSIENT_ATTACHMENT」)。
>
> **骨子**:
>
> | FrameGraphDefinition(名前) | CompiledLogicalRenderGraph(値) |
> |---|---|
> | `writes: scene_color` | 出力値 `{scene_color, v1}` |
> | `reads: scene_color` | 入力値 `{scene_color, v1}`。版が一致した組だけ edge |
> | `reads: scene_color@history` | `import{{scene_color, v0}, previous_epoch}` |
> | load 付き attachment の read+write | inout ポート 1 本(`v1 → v2`) |
>
> **手がかり**: グラフ末尾に必ず積まれる decision `"shadow_graph_only"` / `"logical graph is diagnostic-only and does not own runtime execution"`([その decision の生成](../../src/project/logicalframegraphshadow.cpp#L479))を **そのまま信じないでください**。この論理グラフは現在 [`compileVulkanTargetPlan()`](../../src/project/targetrenderplanning.hpp#L396) の唯一の入力で([`compileVulkanTargetPlan()` を呼ぶ所](../../src/core/renderingpass/renderingsamplecount.cpp#L1671))、そこで決まった format / representation / sample 数 / scope が実際の画像割り当てに反映されます。文字列のほうが実態に追いついていない箇所です。
>
> **不変条件**: `reads_history` は必ず import になり、フレーム内 edge を作らないこと。footprint を「分からないから `same_pixel`」で埋めないこと — 保守的な既定は `arbitrary` の側です。

> 🧩 **難所 — 物理プランは「置換可能な完全パッケージ」**([`verifyVulkanCompletePhysicalPlanPackage()`](../../src/project/vulkancompletephysicalplan.hpp#L128) / [`applyVerifiedVulkanCompletePhysicalPlanPackage()`](../../src/project/vulkancompletephysicalplan.hpp#L141))
>
> **何をする所か**: 「コンパイラが自動で計画した `VulkanTargetPlan`」と「外から差し込まれた完全パッケージ」を、**同じ型に着地させます**。
>
> **素朴に読むと**: `VulkanTargetPlan` が唯一の真実に見えますが、実際には `automatic_plan`(コンパイラ由来の provenance と環境判断を保持)+ `verified`(検証済みの物理フィールドだけ差し替え)の**合成物になりえます**。しかも検証に通った証拠は**別の型** [`VerifiedVulkanCompletePhysicalPlanPackage`](../../src/project/vulkancompletephysicalplan.hpp#L101) が持ち、**型そのものが marker** になっています。これに気づかないと、なぜ検証関数の戻り値をわざわざ持ち回るのかが読めません。コメントが規範です。
>
> > Produces a marker type only after graph coverage, dependency order,
> > lifetime/alias legality, attachment contracts, device capabilities,
> > environment freshness, and NativeScope boundaries have all closed.
>
> **骨子**:
> ```mermaid
> flowchart TB
>     Inputs["論理グラフ + topology snapshot<br/>+ 自動 VulkanTargetPlan + 候補 package"]
>     Verify["verify(..., format_capabilities, enabled_device_extensions)"]
>     Failure["例外(境界が閉じない)"]
>     Verified["VerifiedPackage<br/>marker + fingerprint + diagnostics"]
>     Apply["apply(automatic, verified)"]
>     Plan["実行される VulkanTargetPlan"]
>     Validate["validateVulkanTargetPlanMatchesCompletePhysicalPackage()<br/>GPU 登録の直前に drift 再検査"]
>     Prepare["prepareVulkanNativeScopeExecutors()<br/>verified がある graph だけ走る"]
>     Inputs --> Verify
>     Verify --> Failure
>     Verify --> Verified --> Apply --> Plan --> Validate --> Prepare
> ```
>
> **手がかり**: `verify()` は論理グラフ・topology・自動プラン・format capability・**その device で実際に有効化された extension 名**という「private な device facts」を全部要求します。外部 compiler にそれを再構成させないため、WP238e で [`RenderingTargetPlanVerificationContext`](../../src/core/renderingpass/renderingsamplecount.hpp#L126) が導入されました。[`compileRenderingTargetPlans()`](../../src/core/renderingpass/renderingsamplecount.cpp#L1513) が plan を作るのと同じループで、その plan を作った**正確な入力**を `RenderingTargetPlanCompilation::verification_contexts` に並べて残します([verification context を積む所](../../src/core/renderingpass/renderingsamplecount.cpp#L1879))。`enabled_device_extensions` だけは target planner ではなく Vulkan compiler 層が後から埋め([`compileDefaultVulkanVariant()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L368) の [verification context を埋めるループ](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L542))、その値は [`getEnabledDeviceExtensions()`](../../src/core/vkcore/core.hpp#L120) が返す実際の enable 済み配列です。差し替える側は [`requireVulkanTargetPlanVerificationContext()`](../../src/core/renderingpass/vulkanrendercompilerpackage.hpp#L116) で graph 名から引き、[`installVerifiedVulkanCompletePhysicalPlanPackage()`](../../src/core/renderingpass/vulkanrendercompilerpackage.hpp#L123) に候補 package を渡すだけで、verify → apply → index 更新が 1 回の package 変更として行われます。
>
> 判定は 🚧 のままです。**既定の Vulkan compiler program はこの経路を使いません**。`verified_complete_physical_plans` を埋める `installVerifiedVulkanCompletePhysicalPlanPackage()` を呼ぶのは現状テストだけで([`headless_native_scope_test.cpp` 内](../../test/headless_native_scope_test.cpp#L413))、engine 同梱の provider は [`builtinVulkanNoopMarkerNativeScope`](../../src/core/renderingpass/vulkannativescopeexecutor.hpp#L32)(`builtin.vulkan.noop_marker@1`)という空 marker だけです。本番で NativeScope 実行が発火するのは **独自の `RenderCompilerProgram` を差した時だけ**で、「Vulkan コマンドが provider に差し替え可能になった」と無条件に読むと過大評価になります。ただし WP238e で「差せば実 Vulkan コマンドが出る」ことまでは実測で閉じました。唯一の TEST_CASE が [`headless_native_scope_test.cpp`](../../test/headless_native_scope_test.cpp) の `"WP238e NativeScope records Vulkan commands and rebuilds through renderer generations"` で、テスト所有 provider `pelican.test.vulkan.clear_attachment@1` が `beginRendering` の `loadOp = eClear` で実際に塗り、16x16 headless の中心画素を readback して色を確認し、compiler を差し替えた 2 世代目で色が変わること・in-flight lease が退役してから旧 executor が破棄されることまで同じ TEST_CASE で見ています。NativeScope の `implementation_config` は宣言境界の外から不透明で、engine 側はそれを解釈しません([`vulkancompletephysicalplan.hpp` 内](../../src/project/vulkancompletephysicalplan.hpp#L16))。
>
> **不変条件**: `verify` を通していない package を `apply` しないこと。`automatic_plan` 側の provenance(compiler program 名・環境事実)を verified 側で上書きしないこと。verification context は plan を作った当のループで積むこと — 後から作り直すと、次の難所の fingerprint 検査が通らなくなります。

同じ物理プランを **JSON のまま**外部から受け取る側(Studio と in-engine の ImGui viewer)は、型ではなく [`validatePhysicalTargetPlanWire()`](../../src/project/physicaltargetplanwire.cpp#L756) が検査します。この関数は決して throw せず、壊れた文書は名前付きの理由コード付きで `unavailable` になります([契約のコメント](../../src/project/physicaltargetplanwire.hpp#L53))。読むときに引っかかるのは**名前集合の比較**で、素朴には「lowering graph の resource 名 == caller の論理 resource 名 == 計画された物理 resource 名」の 3 つが一致するはずに見えます。

実際にはそうならない正当な差が 1 種類だけあり、それが **elided** です。planner は Vulkan resource を作らない論理 resource(image でも buffer でもない semantic value / object set / stream)に対して `pelican.plan.semantic_resource_elided@1` という決定を記録して `continue` し([`targetrenderplanning.cpp` の決定](../../src/project/targetrenderplanning.cpp#L2709))、wire 検査側はその `subject` を集めて[比較の前に引き算](../../src/project/physicaltargetplanwire.cpp#L398)します。

引き算が掛かるのは 2 箇所だけで、caller の論理 resource 集合との照合([`requireSameSet` の呼び出し](../../src/project/physicaltargetplanwire.cpp#L483))と lowering graph の resource 名との照合([比較](../../src/project/physicaltargetplanwire.cpp#L532))です。どちらも引き算するのは**論理側の 1 オペランドだけ**で、物理側の集合には elided な名前がそもそも入りません。**elided は「無視した」ではなく「この段では Vulkan 資源にならないと記録した」**なので、決定の列から消すと物理プランの受理が落ちます。

> 🧩 **難所 — automatic plan と compiled plan は別物**([`compileRenderingTargetPlans()`](../../src/core/renderingpass/renderingsamplecount.cpp#L1513) / [`installVerifiedVulkanCompletePhysicalPlanPackage()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L911))
>
> **何をする所か**: 上の verification context が持つ 2 本のプラン、`automatic_plan` と `compiled_plan` の役割分担です。WP239a は、この区別が無かったために rendering pipeline の reload が全面的に拒否された回帰の修正です。
>
> **✅RC-6b2 から**: physical fragment が無くなり、compile の時点では 2 本はいつも同じ object です([`const auto automatic_plan = plan;`](../../src/core/renderingpass/renderingsamplecount.cpp#L1748))。別物になるのは complete physical plan を C++ の口で install した後だけで、install は `target_plans` の行を complete package の計画に差し替え、`automatic_plan` は検査の土台として残します。install は「今の `target_plans` の行が `automatic_plan` と同じ object か」を確かめ、違えば `Vulkan compiler target plan was modified before complete plan verification` で断ります([断る所](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L938)。test「RC-6b2 complete stale refusal and install identity survive legacy removal」)。
>
> **RC-6b2 の前(履歴)**: rendering config が `vulkan_physical_fragments` を持つと、まず**自動プランを丸ごと 1 本作ってから**、`linkVulkanPhysicalFragment()` が fragment を載せた別のプランを作りました。`VulkanTargetPlan::automatic_plan_fingerprint` は**「自分自身の指紋」ではなく「土台になった自動プランの指紋」**で、リンク後もその値を持ち越したので、**fragment-linked plan に対して `vulkanAutomaticTargetPlanFingerprint()` を再計算すると必ず食い違いました**。WP238e が足した検証は当初この 1 本しか持たず、linked plan を `automatic_plan` として保存していたので、fragment を持つ config は必ず落ちました。初回を fragment 無しで登録し、`pipeline.json` を fragment 入りに書き換えてから reload する GPU テストでだけ踏まれたので「reload 固有のバグ」に見えましたが、正確には「fragment を持つ config 固有のバグ」でした。
>
> **骨子**(今):
> ```text
> plan_value     = compileVulkanTargetPlan(...)
> compiled_plan  = shared_ptr(plan_value)
> automatic_plan = compiled_plan                # 同じ object
> # complete install(C++ の口)
> require target_plans[graph] == automatic_plan # 同一性
> verify(automatic_plan, package)               # 土台は automatic_plan
> target_plans[graph] = complete package の計画   # automatic_plan は残す
> ```
>
> **手がかり**: 修正の前段として、9 個の述語を 1 つの `if` に OR で並べて同じ 1 文を投げていた検証が、**条件ごとの別メッセージへ分割**されました([`validateVulkanPhysicalPackage()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L675))。「どの不変条件が破れたか名指しする」ためのもので、短くまとめ直さないでください。分割後は、`logical_graph_fingerprint` と `automatic_plan_fingerprint` を `automatic_plan` 側で、「indexed な target plan と一致するか」を `compiled_plan` 側で検査します。CPU 回帰は [`renderingsamplecount_test.cpp`](../../test/renderingsamplecount_test.cpp) の "rendering target bridge retains automatic attachments and complete verification context" が担当し、compile の直後は `automatic_plan == compiled_plan` で両者の `automatic_plan_fingerprint` が一致することを固定しています(✅RC-6b2 の前の名前は "rendering target bridge links attachment operations into the physical plan" で、fragment を link して 2 本が別になることを見ていました)。
>
> **不変条件**: `verify()` へ渡すのは `automatic_plan` のみで、差し替えた後の計画を土台にしないこと。2 本が別の object になるのは complete physical plan を install した graph だけであること。

> 🧩 **難所 — スケジュールは scope-execution-major**([`buildLogicalFrameViewFamilySchedule()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L120) / [`selectLogicalFrameSequentialViewSchedule()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L427))
>
> **何をする所か**: 物理プランの scope 列と、実行時に確定する view family の view 数から、コマンド記録の単位([`LogicalFrameNodeInvocation`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L23))の列を作ります。plan と実行の境目にあたる header-only の関数です。
>
> **素朴に読むと**: ループの入れ子順が **直感と逆**です。「view ごとに全 node」ではなく **「scope ごとに、execution(= view)ごとに、その scope の node 全部」**。ヘッダ冒頭のコメントが規範です。
>
> > Scheduling is scope-execution-major: every node in a fused scope records for
> > one view before the next sequential view begins. This is required to keep a
> > native dynamic-rendering scope open across all of its nodes.
>
> ここでいう scope を「開く / 閉じる」は、この scheduler ではなくコマンド記録側で起きます。scheduler が作る各 [`LogicalFrameNodeInvocation`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L23) は `scope_node_index` / `scope_node_count` を持ち、[`beginsScopeExecution()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L47)(= `scope_node_index == 0`)の invocation で `Renderer` が [`beginRenderingScope()`](../../src/core/vkcore/renderer.cpp#L2164)、[`endsScopeExecution()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L50)(= `scope_node_index + 1 == scope_node_count`)の invocation で [`endRenderingScope()`](../../src/core/vkcore/renderer.cpp#L2340) を呼びます。つまり fused scope は **(scope, execution=view) の組ごとに 1 回開いて 1 回閉じ**、その間にその scope の node が全部記録されます。
>
> もう 1 つが **main と secondary の検査の非対称**です。`$main` の scope は plan 側の `view_execution` / `view_count` / `execution_count` / `view_mask` が**すべて整合していないと例外**([`$main` 側の検査](../../src/core/renderingpass/viewexecutionscheduler.hpp#L281))。対して secondary family の scope は **必ず single-view のテンプレート 1 個**でなければならず、実 view 数は runtime の family cardinality が `sequential` へ展開します([secondary 側の検査](../../src/core/renderingpass/viewexecutionscheduler.hpp#L256))。したがって「shadow cascade 4 枚が物理プランに焼かれている」は誤りで、物理プランは shadow が何カスケードかを**知りません**。この非対称(main は plan が決める / secondary は runtime が決める)を知らないと「同じ scope なのに検査が違う」が読めません。
>
> **骨子**:
> ```text
> mapping = frame_graph.scope_mapping        # 登録で 1 回作った scope ↔ node の対応(✅RC-3d3)
> for scope_index, scope in target_plan.scopes:
>   range = mapping.scopes[scope_index]      # FramePlan 順の実行 node の配列の [first, count)
>   family = range の全 node が属する唯一の view family(混在は例外)
>   main か?      : execution_count = (sequential ? view_count : 1) / plan 値と一致検査
>   secondary か? : plan は single_view テンプレート必須
>                   runtime cardinality > 1 なら sequential へ展開
>   for execution_index in 0..execution_count:
>     for node in nodes[range.first .. range.first + range.count):
>       emit LogicalFrameNodeInvocation{node, scope, scope_node_index, execution, view, family}
> (未所属・重複・別順の node は登録で断られている)
> ```
>
> - **scope のループ**: 最外は scope。scope 内 node は schedule 上で連続。
> - **前計算(✅RC-3d3)**: 前はこの関数が毎フレーム `node_by_name`(node 名 → 添字)・`scope_by_node`・`scope_nodes` と coverage 用の `scheduled_nodes` を作り、unknown な node・重複した所属・どの scope にも属さない node を例外にしていました。今は `CompiledFrameGraphExecution` 全体を受け、登録の [`compileFrameGraphScopeMapping()`](../../src/core/renderingpass/framegraphruntime.cpp#L15) が作った [`FrameGraphScopeMapping`](../../src/core/renderingpass/framegraphruntime.hpp#L94)(scope ごとの `[first, count)` と、node ごとの scope の添字・scope の中の位置)の数値の区間を読むだけです([読む所](../../src/core/renderingpass/viewexecutionscheduler.hpp#L126))。名前の照合と coverage の検査は登録の 1 か所(下の「計画と実行 ID の結合」)に移り、毎フレーム残るのは view の数・axis・`view_select`・secondary family の展開・0 件の placeholder など、フレームの値で変わるものです。切り替え前後で既存の CPU test の 6 構成の schedule(8 個、invocation 30 個、11 欄)がバイト一致でした(実装者の報告)。
>
> **手がかり**: per-view target(flat / 従来の swapchain)は `selectLogicalFrameSequentialViewSchedule()` が 1 view 分を抜き出しますが、**multiview scope が混ざっていたら即例外**です(`"per-view target cannot execute a multiview scope"`、[その throw](../../src/core/renderingpass/viewexecutionscheduler.hpp#L466))。view family 自体は「論理関係であって Vulkan の実行モードではない」と型側が明言しており([`logicalrendergraph.hpp` 内](../../src/project/logicalrendergraph.hpp#L184))、sequential / multiview / shared のどれになるかは物理 lowering が決めます。実行側の消費は §6.4 です。
>
> **不変条件**: 1 node は 1 scope にしか属さないこと(二重登録も未登録も、✅RC-3d3 からは登録の時点で `render_compiler.physical_order_mismatches_frame_plan@1` の拒否)。scope を展開した node の列は FramePlan の順と同じであること(同じ拒否)。1 つの scope に複数の view family を混ぜないこと。scope 内 invocation は schedule 上で連続していること — 実行側はこの連続性と physical scope の node 並びの一致を毎フレーム再検査します。

### provider registry の一覧(EL-6)

冒頭の図の (2) 変換 registry(render strategy / graph transform / subgraph replace)に pass implementation と draw sort を加えた 5 つは、どれも名前で provider を引く registry で、中身は共通の `provider_registry_common::ProviderRegistry` です。✅EL-6(`5582bb93`)から、5 つの registry の facade が同じ形の一覧を返します。

| registry | 一覧を返す所 | 文書の欄 |
|---|---|---|
| `RenderPolicyRegistry`(draw sort) | [`RenderPolicyRegistry::drawSortProviderListing()`](../../src/core/renderer/renderpolicyregistry.cpp#L347) | `draw_sort.{opaque,transparent}.provider` |
| `PassImplementationRegistry` | [`PassImplementationRegistry::providerListing()`](../../src/core/renderingpass/passimplementationregistry.cpp#L584) | pass の `implementation.provider` |
| `SubgraphReplacementRegistry` | [`SubgraphReplacementRegistry::providerListing()`](../../src/core/renderingpass/subgraphreplacementregistry.cpp#L1189) | `region_replacements[].provider` |
| `GraphTransformRegistry` | [`GraphTransformRegistry::providerListing()`](../../src/core/renderingpass/graphtransformregistry.cpp#L1177) | `graph_transforms[].provider` |
| `RenderStrategyRegistry` | [`RenderStrategyRegistry::providerListing()`](../../src/core/renderingpass/renderstrategyregistry.cpp#L740) | `render_strategy.provider` |

- どれも [`ProviderRegistry::listing()`](../../src/core/renderingpass/providerregistrycommon.hpp#L406) に委ねます。shared lock の下で `visibleProviders()`(active で、名前で引いたときに自分が当たる slot だけ。名前順)を回るので、game の provider が engine のものを同じ名前で覆っていれば game の方だけが出ます。要素は [`RenderProviderListing`](../../src/core/renderingpass/providerlisting.hpp#L14) `{name, engine_owned, provider_version}` で、`engine_owned` は slot の owner が `engineRegistrationOwner` かどうかです。
- EL-1 で `SubgraphReplacementRegistry` だけが持っていた一覧(`SubgraphReplacementProviderListing`)は、この共通の型の別名になりました(`using SubgraphReplacementProviderListing = RenderProviderListing`)。
- registry は ABI に説明や parameter の宣言の欄を持たないので、一覧にもありません(`providerlisting.hpp` の注記)。
- RPC は `get_registered_providers`(第7章 §7.7 の編集系の表)で、[`registeredProvidersJson()`](../../src/core/communication/engineauthoringlists.cpp#L687) が 5 つを `{name, source: engine|game, provider_version}` に直します。呼ぶたびに読むので、game logic の reload(owner の activate / release)は次の呼び出しに出ます。`VulkanNativeScopeExecutorRegistry`・`RenderViewFamilyProviderRegistry`・`CompilerProviderRegistry` はこの一覧の対象外です。

## 6.2 PassInfo: 継承ではなく `std::variant` で pass を表す

pass の種類は virtual class 階層ではなく、[`PassInfo`](../../src/core/renderingpass/renderingpass.hpp) という `std::variant` です(通常 10 種、ImGui build は 11 種)。

| variant | 実際の仕事 |
|---|---|
| `MaterialPassInfo` | glTF 由来の material/mesh instance を indirect draw |
| `FullscreenPassInfo` | 全画面三角形で post-process、入力 target/buffer を読む |
| `GenericRasterPassInfo` | project authoring の draw / fixed state / shader を portable raster contract として実行 |
| `DebugDrawPassInfo` | line geometry。physics collider の可視化もここへ投入 |
| `DebugTextPassInfo` | debug glyph の描画 |
| `ShadowDepthPassInfo` | depth-only の material draw |
| [`VelocityPassInfo`](../../src/project/passinfo.hpp#L73) | TAA 用の screen-space velocity 描画。対応 renderer は [`velocitypasscontainer.hpp`](../../src/core/renderer/velocitypasscontainer.hpp) |
| [`PickingPassInfo`](../../src/project/passinfo.hpp) | `R32_UINT` へ model instance slot + 1 を書く視覚ピッキング描画。通常は [`picking.json`](../../src/core/resources/features/picking.json) が挿入 |
| [`GizmoPassInfo`](../../src/project/passinfo.hpp) | WP258 の選択 transform へ world-axis の移動・回転・拡縮 line geometry を重ねる。通常は [`gizmo.json`](../../src/core/resources/features/gizmo.json) が挿入 |
| `UiPassInfo` | UI container の内容を描画 |
| [`ImGuiPassInfo`](../../src/core/renderingpass/renderingpass.hpp#L207) | 開発者 UI(ImGui)。executor 内の分岐で処理(`PELICAN_WITH_IMGUI` 時のみ variant に含まれる) |

型の置き場は 2 つに分かれます(✅RC-4)。`FullscreenPassInfo` から `UiPassInfo` までの 9 種は backend に依存しない値だけなので project の [`passinfo.hpp`](../../src/project/passinfo.hpp#L15) にあり、`MaterialPassInfo`(generation の target / buffer の ID を持つ binding を含む)と `ImGuiPassInfo`(`PELICAN_WITH_IMGUI` が core の定義)と variant 自体は core の `renderingpass.hpp` にあります。core の variant は project の型をそのまま要素にします。

振り分けは [`renderDynamicPassDrawCalls()`](../../src/core/vkcore/render_pass_dispatch.cpp#L195) にあります。型を追加するときは、IR の info と `parseRenderGraph()` の読み、IR → `PassDefinition` の `passDefinitionOf()`(✅RC-3c2b2。§6.7 の「[pass の道](#pass-ir)」)、runtime compiler、dispatch を同時に増やす必要があります。variant なので「未知の派生型」が紛れず、compile 時に分岐漏れを見つけやすい一方、機能追加は open class hierarchy より明示的です。

**番号付きの入力の設定 `input_settings`(✅RS-14)。**fullscreen 系(`output_transform` を含む)と generic raster の pass の、`input` の画像ごとのサンプリングと subresource は、variant ではなく [`PassDefinition::input_target_settings`](../../src/core/renderingpass/renderingpass.hpp#L283)(`input_targets` と同じ位置。短くてよい)に入ります。

✅RC-3c2b2 から読むのは `parseRenderGraph()` の [`passExecutionDeclaration()`](../../src/project/rendergraphparse.cpp#L747) で(IR の `NodeIR::input_settings`。`passDefinitionOf()` がそのまま写す。前は core の `parsePassInputSettings()`)、`input_settings` の要素を、`null` なら既定(空の `ShaderResourceInputSetting`)、それ以外は名前付きの resource port と同じ [`parseShaderResourceInputSetting()`](../../src/project/shaderresourceport.cpp#L599)(中は port の `parseSampling()` と `parseOptionalImageSubresource()`)で読みます。

ほかの type の pass、配列でない値、画像の入力より長い配列は `render_graph.node_field_invalid@1` で断ります(`reason` は `requires_fullscreen_or_raster` / `not_an_array` / `too_many_entries`)。runtime compiler では、fullscreen と raster のどちらの compile も [`compileFullscreenResourceInterface()`](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L1708) を通り、その中で [`applyPassInputSettings()`](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L1608) が最後に設定のある入力のサンプリングと subresource を上書きします。

resource port の無い pass では `input_sampling`(fullscreen だけ)か既定を置いた後([port の無い pass で呼ぶ所](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L1736))、port のある pass では port ごとの値を置いた後([port のある pass で呼ぶ所](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L2048))で、port が束ねる入力に設定があれば断ります。書き手は通常 graph の lowering です(第3章「graph の番号付きの入力の読み方」)。

## 6.3 Frame graph planner のアルゴリズム

frame graph node は `render` または `compute` で、名前、宣言順、`reads`、`writes`、`after`、`before` を持ちます。planner は [`planFrameGraph()`](../../src/project/frameplan.cpp#L473) で次を行います。

1. node 名の一意性を検証する。
2. reads/writes が宣言済み target/buffer かを検証する。
3. 自動 data edge と明示 edge を作る。
4. 推移閉包を作り、複数 writer が順序付け済みか検証する。
5. 安定トポロジカルソート(**トポロジカルソート** — 有向グラフのすべての辺 a→b について a が b より前に来るように頂点を一列に並べる操作。ここでは「依存先が必ず先に実行される」node 順を作ります。「安定」は、依存関係で順序が決まらない組を宣言順で一意に決めるという意味です)を行う。
6. 実行順(node の `order`)と、read-after-write barrier 情報を保存する(node の level は ✅RC-2 で無くなりました。下の「level は無い」)。

拒否はすべて名前付きの `GraphRegistryError` で、8 種の code と detail は [`frameplan.hpp` の `planFrameGraph()` の宣言](../../src/project/frameplan.hpp#L201) のコメントが一覧です(✅RC-3c1b で旧の文 `Cycle detected in frame graph` などは消えました)。順の決まらない 2 writer(手順 4、`render_graph.ambiguous_writes@1`)だけは最終の `frame_node_order` で合法になりうるので、provider の途中の改訂を見る [`requireFrameGraphStructure()`](../../src/project/frameplan.hpp#L239) は手順 4 を除いた検査だけをします(`toLogicalGraph()` がこれを呼ぶ。§6.7 の「中立な描画グラフ」)。

### 自動で作られるのは「直前の writer → 後続 reader」

[`buildEdges()`](../../src/project/frameplan.cpp#L167) は宣言順に node を走査し、resource ごとの `last_writer` を覚えます。reader が現れたら、その時点の直前 writer から reader へ RAW edge を張ります。

```mermaid
flowchart LR
    A["A writes color"]
    B["B reads color"]
    C["C writes color"]
    A -->|"自動追加"| B
```

- **C writes color**: 自動では B → C や A → C を追加しない。

`after` と `before` は別途、明示 edge として追加されます。その後 [`addBarriersForOrderedResourceEdges()`](../../src/project/frameplan.cpp#L147) が「順序 edge があり、from が書き、to が同じ resource を読む」組を barrier 情報へ変換します。

ここは重要です。現実装は一般的な hazard graph(hazard = 同じ resource に対する読み書きの組のうち、順序が入れ替わると結果が変わってしまうもの。下の 3 種です)をすべて自動生成するわけではありません。

- RAW（write → read）: 直前 writer から自動 edge。
- WAW（write → write）: 自動 edgeなし。どちら向きか `after` / `before` などで明示しないと [`validateWritesAreOrdered()`](../../src/project/frameplan.cpp#L248) が例外にします。
- WAR（read → write）: 自動 edgeなし。保存したい古い値がある場合は明示順序が必要です。

> 🧩 **難所 — writes-writes の曖昧検出**([`transitiveClosure()`](../../src/project/frameplan.cpp#L231) / [`validateWritesAreOrdered()`](../../src/project/frameplan.cpp#L248))
>
> **何をする所か**: 同じ resource に 2 つ以上の node が書くとき、どちらが先か決まっているかを plan 生成時に検査します。
>
> **素朴に読むと**: `transitiveClosure()` は 3 重ループだけの関数で、変数名もコメントもアルゴリズム名を明かしません。実体は **Floyd–Warshall 法**(フロイド・ウォーシャル法 — グラフの全頂点対について「経由してもよい中継点」を 1 つずつ増やしながら到達可否を更新していく古典的な動的計画法。ここでは距離ではなく到達できるか否かだけを求める「推移閉包」版です)で、正しさの根拠は「**中継点 `k` のループが最外であること**」ただ 1 点です。理由は不変条件で、`k` の反復を 1 つ終えるたびに「中継点として 0..k だけを使う到達関係」が完成している状態を保てるからです。`k` を内側へ動かすとこの不変条件が作れず、まだ更新されていない `reach[i][k]` を読むため閉包が不完全になりますが、**多くのグラフでは正しい答えが出てしまう**ため、テストをすり抜けた瞬間に「WAW を検出しないまま plan が通る」という静かな壊れ方をします。実害は、同じ RT に書く 2 パスの順序が tie-break(= JSON の記述順)任せになること — 設定を並べ替えただけで絵が変わります。planner は WAW/WAR の edge を自動生成しないので、**この検査だけが最後の砦**です。
>
> **骨子**:
> ```text
> for k in nodes:
>   for i, j: reach[i][j] |= reach[i][k] && reach[k][j]
> for 各 resource の writer ペア (a,b):
>   if !reach(a,b) && !reach(b,a): refuse render_graph.ambiguous_writes@1 {graph, resource, first, second}
> ```
>
> - **k のループ**: 最外が k。ここが命。
>
> **手がかり**: 閉路があると両方向とも到達可能になり、この検査は**通ってしまいます**。閉路は後段の `topologicalOrder()` が `render_graph.cycle@1` で捕まえるので、エラー文言の優先順位はこの呼び出し順で決まります。テストは [`frameplanner_test.cpp`](../../test/frameplanner_test.cpp) の invalid fixture 一覧。
>
> **不変条件**: `transitiveClosure()` に渡すのは**直接 edge の行列だけ**(barrier 由来のものを混ぜない)。検査は topological sort より前に置く。

> 🧩 **難所 — barrier は後追いで作る**([`buildEdges()`](../../src/project/frameplan.cpp#L167) / [`addBarriersForOrderedResourceEdges()`](../../src/project/frameplan.cpp#L147))
>
> **何をする所か**: resource ごとの「直前の writer → 後続 reader」から自動 edge と RAW barrier を作り、そのあとで **すべての順序 edge**(`after` / `before` 由来を含む)を走査して、from が書き to が読む resource に barrier を足します。
>
> **素朴に読むと**: 罠が 4 つ重なっています。第一に `addBarriersForOrderedResourceEdges()` は `for (const auto &edge : planner_edges.edges)` と同じ vector を range-for します。✅RC-3d1 からこのループが呼ぶのは [`addBarrier()`](../../src/project/frameplan.cpp#L126) だけで、edge も由来も足しません(コードの注記「既にある順序の組への barrier で、宣言順の読みの依存(と診断の由来)をもう 1 つ足すのではない」)。前は `addDataEdge()` 経由で `addEdge()` を呼び、`exists[from][to]` が真なので `push_back` に届かない、という前提で安全でした。ここに `addEdge()` を足すと、iterator の無効化(UB)に加えて、FO-1 の診断の由来が 1 つ増えて detail が変わります。第二に `addBarrier()` の重複チェックが要ります(自動 RAW の barrier は `buildEdges` の読みの所で 1 度積まれ、同じ組がここでもう 1 度来る)。第三に barrier は plan 上の順序を前提にした index へ落ちるので、登録時([`framegraphruntime.cpp` 内](../../src/core/renderingpass/framegraphruntime.cpp#L234))と毎フレーム実行時([`renderer.cpp` 内](../../src/core/vkcore/renderer.cpp#L923))で **同じ不変条件を二重チェック**します。第四に(✅RC-3d1)、edge は端点 `(from, to)` で重複を除いて 1 本にし、同じ端点の読み・`after`・`before` は [`Edge::origins`](../../src/project/frameplan.cpp#L25) に由来([`EdgeOrigin`](../../src/project/frameplan.cpp#L17)。理由・資源・診断の探索の順位)として積みます。端点を引く `edge_by_target` は引くためだけの表で、順は `edges` の挿入順です(表を回して順を作ると planner の順が変わる)。
>
> **骨子**:
> ```text
> buildEdges(definition, node_index, use = planning | diagnostics):
>   for node in 宣言順:
>     reads:  last_writer[r] があれば addEdge(last_writer[r], node, 由来 {reads, r, (node, 何番目)})
>             planning なら addBarrier(read_after_write, last_writer[r], node, r)
>     writes: last_writer[w] = node
>   for node in 宣言順: after / before を addEdge(由来 {after | before, (node, reads の数 + 何番目)})
>   planning なら addBarriersForOrderedResourceEdges   # after/before 由来の edge もここで barrier 化
> ```
>
> - **addEdge**: 同じ端点の edge があれば由来だけ足し、無ければ edge を足す。自己参照は planning なら `render_graph.self_dependency@1`、diagnostics なら黙って捨てる。
> - **diagnostics**: FO-1 の照合(下の「`frame_node_order` と依存の照合」)が使う道。barrier を作らず、unknown / self の参照を断らない(構造の検査に任せる)。RC-3d1 のコードレビューで、この道でも barrier を作っていたのを直しました(統合側の記録では二乗の手間)。
>
> **手がかり**: `last_writer` は **宣言順**の直前 writer であって、topological sort 後の直前 writer ではありません。JSON を並べ替えると自動 edge の張られ方が変わります。テストは [`frameplanner_test.cpp`](../../test/frameplanner_test.cpp) の "frame planner emits barriers for explicit compute to render resource edges"。
>
> **不変条件**: `edges` を走査しながら `addEdge` を呼ぶ経路を増やさないこと(増やすなら index ループへ書き換える)。barrier の `kind` は `"read_after_write"` のみで、他は登録時に例外になります。

### 安定トポロジカルソート

依存がない node の順序はランダムではありません。[`topologicalOrder()`](../../src/project/frameplan.cpp#L299) は ready set を `(declaration_index, node_index)` で並べ、設定に書いた順を tie-breaker にします。cycle なら全 node を取り出せないため例外になります。

> 🧩 **難所 — 決定性は set のキー**([`topologicalOrder()`](../../src/project/frameplan.cpp#L299))
>
> **何をする所か**: 入次数 0 の node 集合から実行順を確定させます。
>
> **素朴に読むと**: ready 集合が `std::queue` ではなく **`std::set<std::pair<size_t, size_t>>`** で、first が `declaration_index`、second が node index です。set の順序がそのまま「設定に書いた順」の tie-break になっており、**決定性はこのキー設計そのもの**です。queue や stack に替えると、依存のない node の順序が edge 挿入順に依存し、frame plan JSON・GPU timing の node ordinal・debug label 文字列といった golden が環境ごとにぶれます。動くけれど再現しない、という壊れ方をします。
>
> **骨子**:
> ```text
> ready = {(declaration_index, i) | indegree[i] == 0}   # 決定的な優先度付きキュー
> while ready: 最小を取り出し order へ追加し、後続の indegree を減らし 0 なら ready へ
> if order.size() != count: refuse render_graph.cycle@1 {graph}
> ```
>
> **手がかり**: `const auto [unused_index, node_index] = *ready.begin(); (void)unused_index;` は、構造化束縛の**個々の名前**には属性を付けられない(宣言全体への `[[maybe_unused]]` は C++17 から可能ですが、名前ごとの属性は C++26 の P0609R3 から)ため、片方だけを未使用と印付けできないことへの回避で、それ以上の意味はありません。
>
> **不変条件**: `declaration_index` は passes と compute_tasks を通した**通し番号**です。pass 単位でリセットすると tie-break が壊れます。

**`hazard_stress` の撹拌は planner で(✅RC-3d2)。**[`FramePlanningOptions`](../../src/project/frameplan.hpp#L197) の `hazard_stress_seed` があると、`topologicalOrder()` は ready set の先頭ではなく、[`stablePlanningRank()`](../../src/project/planningrank.hpp#L9)`(seed, それまでに取り出した node の数, node の名前)` が最小の node(同じなら名前の小さい方)を取ります([選ぶ所](../../src/project/frameplan.cpp#L320))。依存は indegree のとおりに守られ、seed が無ければ上の set のキーの順のままです(0 も seed として扱います)。

- src で seed を渡すのは compiler の runtime の variant の道 `compileDefaultVulkanVariant()` だけで(src の検索)、graph の target planning の profile が `hazard_stress` のときに `profile.seed` を [`planRenderGraph()`](../../src/project/rendergraphir.cpp#L376) へ渡します([渡す所](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L482))。
- 前は物理計画の analyzer(`analyzeLogicalPlanningOpportunities()`)が自分で node の順を選び直し、そこで seed の撹拌をしていました。RC-3d2 で analyzer は順を入力として受け([宣言](../../src/project/targetplanning.hpp#L408))、撹拌するのは並列 / 融合 / alias の候補の並びと alias の間引きだけです。rank の関数は planner と analyzer で同じ 1 つ(`planningrank.hpp`)です。
- test: `frameplanner_test` の「RC-3d2 planner stress seeds vary ready nodes while preserving dependencies」と、`rendercompilerprogram_test` の「RC-3d2 production compiler forwards stress seeds to the FramePlan and physical plan」(seed 0〜7 で順が複数になり、同じ seed では物理の dump 全体が同じ。seed を渡さない対照で 1 種類になって赤)。

**ループの直列が閉路に見えていた(✅LC-1、`38327a9ed`)。**loop A の出力を loop B の入力に直に繋ぐ(入れ子でない)と、planner が `Cycle detected in frame graph: <graph>`(当時の文。✅RC-3c1b からは [`topologicalOrder()` の拒否](../../src/project/frameplan.cpp#L342) の `render_graph.cycle@1 {graph}`)で断っていました。閉路は planner ではなく、合成の後の段で回を並べる所が作っていました(LC-1 の実装者の報告)。

- **原因**: lowering は A と B の両方に「直前の root の graph pass」(例では `seed`)を置き場所として残します。[`expandRenderGraphLoops()`](../../src/project/rendergraphcomposition.cpp#L2314) が A の回を `seed` の直後に入れた後、B の回も `seed` の直後に入れると、配列は `seed → B/0 → B/1 → A/0 → A/1` になります。A の最後の回 → B の最初の回は A の出力の読みによる正しい辺、B の中と A の中は要素の順の `after`、そして B/1 → A/0 は逆転した宣言順に `enforceCanonicalOrder()`(当時の canonical の鎖。✅RC-5s で撤去。§6.1 の難所「合成の順の決まり方」)が足す辺で、4 本で閉じます。image は回ごとに別(`a/0/image` など)で、名前の衝突ではありません。
- **直し方**: 近傍の pass を「置き場所の候補」に格下げし、[`insertAmongScenePasses()`](../../src/project/rendergraphcomposition.cpp#L2269) の書き手の下限(`floor`。先に置いた run の書き手を追い越さない)を当ててから入れます([近傍を候補として渡す所](../../src/project/rendergraphcomposition.cpp#L2567))。候補があるときは読み手の前へ動かす処理をしません。scene の pass が読むときに塊ごと動かすのは合成の既存の処理の仕事で、ここで loop だけを動かすと root の pass とループの塊が割れるためです(コードの注記)。ループの文書の形・carry の意味・`graph.loop_nested@1` は変わりません。
- **閉路の文**: 本当に A → B → A と繋いだときは、展開器の [`Expander`](../../src/project/graphcomposition.cpp#L643) が instance とループの展開の経路を持つようになり、`cyclic subgraph connection: a -> b -> a` と経路を言います([経路を作る所](../../src/project/graphcomposition.cpp#L725))。logical graph の閉路の拒否([`validateCompiledLogicalRenderGraph()`](../../src/project/logicalrendergraph.cpp#L274))も DFS で実際に閉じた node の列を探し、`logical graph contains a dependency cycle: <graph> (<n1> -> … -> <n1>)` と付けます([探す所](../../src/project/logicalrendergraph.cpp#L511))。閉路の下流にあるだけの node は並べません。
- test: `featurecompose_test` の「LC-1 serial graph loops compose and plan with shared or distinct bodies and carry」(body の共有 / 別 × carry の有 / 無の 4 通り)と「LC-1 serial loop cycles identify the closed node path」(CPU)、`headless_render_test` の「LC-1 serial loops render through both runs into the final image」(GPU)、Studio の scenario `lc1_serial_loops`。

### `frame_node_order` と依存の照合(✅FO-1 `a4d7760a`)

`frame_node_order` は compile の [`applyFrameNodeOrder()`](../../src/project/rendergraphir.cpp#L996) が、並びの隣どうしに `after` を足して当てます(✅RC-3c1b から project の IR → IR の関数で、`finalizeRenderGraph()` の最後の段。provider の列と AS の node の挿入の後なので、provider が足した node も並びに書けます。§6.7 の「中立な描画グラフ」)。FO-1 から、足す前に [`requireFrameNodeOrderAgreesWithDependencies()`](../../src/project/frameplan.cpp#L591) が並びと依存を照らします(この関数は RC-3c1a から project の `frameplan.cpp` にあります)。

- 依存は planner と同じ [`buildEdges()`](../../src/project/frameplan.cpp#L167) の辺です(✅RC-3d1。上の「barrier は後追いで作る」の diagnostics の道)。前は別の helper `frameNodeDependencies()` が同じ規則で辺を組み直していましたが、RC-3d1 で消え、`FrameNodeDependency` / `frameNodeDependencies` は src・test に 0 件です。辺の由来は理由(`reads` / `after` / `before`)と、`reads` なら資源の名前を持ちます。`after` には composer が足した辺(compute task 待ち、`output_transform` の終端の辺、snapshot の copy の辺など。✅RC-5s の前は canonical の鎖と anchor も)も入るので、書いた `after` と区別しません。
- 探索は、node ごとの出る辺の由来を診断の順位(その node の reads → after → before の宣言の順)で並べ替えてから辿ります。planner の格納の順(全 node の reads が先、その後に after / before)のまま辿ると、同じ長さの道や同じ端点の由来が複数あるときに選ぶ 1 本が変わります。RC-3d1 はこの並べ替えで前の helper の detail を保ち、切り替え前後で detail 22 件が全欄一致、並べ替えを外す対照で新しい test「frame node conflict details preserve provenance and traversal priority」の 6 ケース中 5 が赤でした(実装者の報告)。
- 並びの 2 番目以降の node から幅優先で辿り、並びでそれより前の node に届けば、`frame_node_order.conflicts_dependency@1 {graph, first, second, reason, resource?, via?, anchor?}` の `GraphRegistryError` を投げます。`reason` は道の最初の辺の理由で、道の途中に output transform の node があれば `output_terminal` に替え(`resource` は消す)、`anchor` にその node の名前を入れます([替える所](../../src/project/frameplan.cpp#L653))。✅RC-5s の前は anchor か output transform の node で `canonical_order` でした。
- 照合は `frameGraphDefinitionOf()` が導く最終改訂の辺を見ます(graph transform・subgraph の差し替え・AS の node の挿入の後。RC-3c1b の前の core の道でも、適用は provider の列と AS の挿入の後でした)。FO-1 の実装者の報告は「後の段で足される辺(graph transform など)と矛盾する並びは planner の `Cycle detected in frame graph`」と書いていましたが、どの辺を指すかはコードで未確認です。閉路の拒否は今は `render_graph.cycle@1` です。
- 照合を compile の `applyFrameNodeOrder()` に置いたので、編集の候補でも起動・reload でも同じ code になる、と実装者は報告しています。FO-1 の test は編集の道だけで、RC-3c1b で本番の compiler program を通す test([「RC-3c1b B2 frame_node_order names the node a provider added」](../../test/rendercompilerprogram_ir_test.cpp#L92)。provider が足した node を並びに書き、2 通りの並びで FramePlan の順が変わる)が足されました。
- 名前付きの AS の操作の順も同じ照合を通ります。`rendercompilerprogram_test.cpp` の 7 か所は「`applyFrameNodeOrder()` は通り、後の `planFrameGraph()` が `Cycle` で落ちる」を見ていたので、FO-1 で「`applyFrameNodeOrder()` がこの code で投げる」に期待を変えました([第7章](07_tools_rpc_tests.md) の test の表)。

### level は無い(✅RC-2)

以前は `computeLevels()` が依存段数(最長経路長)を計算し、node の `level` と `FramePlan::levels` に入れていましたが、実行側の [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1575) は level を見ずに `frame_graph.nodes` を一本の loop で順番に実行していて、level は実行にも判断にも使われていませんでした。RC-2(`e15e31bb5`)で関数・`FramePlanNode::level`・`FramePlan::levels` を消し、dump の `levels` と node の `level` も出しません。並列化(複数 queue)の WP が来たら、その WP が依存辺から ASAP / ALAP を自分の入力として計算する、が設計メモの判定です([design_plan_layer_inventory.md](../design_plan_layer_inventory.md) §2-2)。

plan の `level` で列と副題を決めていた ImGui の Plan Viewer は、plan の順に node を見て、barrier で待つ相手の列の 1 つ右に置き、副題に `#<order>` を出します([列を決める所](../../src/core/imgui/planviewer.cpp#L702))。Studio の自動配置([`nodelayout.cpp`](../../src/devstudio/view/nodelayout.cpp))は前から自前で最長経路の列を計算していて、plan の `level` を読んでいませんでした。

### 計画と実行 ID の結合

planner は名前しか知りません。[`FrameGraphRuntimeContainer::registerExecutionPlan()`](../../src/core/renderingpass/framegraphruntime.cpp#L839) が各 plan node の名前を `CompiledRenderingPass::passes` / `compute_tasks` から探し、実配列 index と incoming barrier を持つ `CompiledFrameGraphExecution` を作ります。実行時には plan と実行 node の name/kind がまだ一致しているかも [`executePlannedFrameGraph()` 冒頭](../../src/core/vkcore/renderer.cpp#L1787) で再確認します。

**物理の順の検査と scope ↔ node の前計算(✅RC-3d2・✅RC-3d3)。**同じ登録の [`compileExecution()`](../../src/core/renderingpass/framegraphruntime.cpp#L105) が、FramePlan の構造の検査(`validateFramePlanStructure()`)の後、target plan があれば [`compileFrameGraphScopeMapping()`](../../src/core/renderingpass/framegraphruntime.cpp#L15) を 1 回呼びます。

- この関数は target plan の scope を展開した列(`physicalNodeOrderOf()`)を FramePlan の順と [`requirePhysicalNodeOrderMatchesFramePlan()`](../../src/project/frameplan.cpp#L525) で照らし(compiler の共通の仕上げと同じ code `render_compiler.physical_order_mismatches_frame_plan@1`、同じ優先順)、通れば同じ区間から node ごとの scope の添字と scope の中の位置を作ります。検査と対応表は 1 つの関数で、名前の照合を 2 度書いていません。
- `prepareGeneration()` も互換の `registerExecutionPlan()` も `compileExecution()` を通るので、compiler の共通の仕上げを通らずに直接登録する道もこの検査を受けます(test「RC-3d2 direct generation and compatibility registration validate physical node coverage and order」)。
- 結果は [`CompiledFrameGraphExecution::scope_mapping`](../../src/core/renderingpass/framegraphruntime.hpp#L126) に入ります。target plan が無い登録(renderer の fallback の schedule)は `nullopt`、空の graph の空の target plan は空の対応表です(test「RC-3d3 empty target plans have a mapping and fallback registrations have none」)。
- 作った回数は private な計数で数え、test は friend の [`FrameGraphRuntimeTestAccess`](../../test/framegraphruntime_testaccess.hpp#L7) で読みます。同じ generation で view の数と `view_select` の選択を変えて描いても登録の 1 回だけ、を CPU(「RC-3d3 registered scope mapping is reused as frame cardinality and selection change」)と GPU(「RC-3d3 frames reuse registered scope mapping while views selection and once execution change」。4 フレーム、view 数 1 → 2 → 1 → 1)で見ます。

なお現在の frame graph 定義には、history 付き render target の前フレーム面を読む入力(`history_read`、fixture は [`fixtures/frameplanner/plans/history_read.json`](../../test/fixtures/frameplanner/plans/history_read.json))と、`snapshot_copy` node(frameplanner.cpp#L160-L164)も入ります。

> 🧩 **難所 — `@history` は edge を作らない**([`historyReference()`](../../src/project/rendergraphparse.cpp#L276) / [`RenderTargetContainer::surfaceIndex()`](../../src/core/renderingpass/rendertargetcontainer.cpp#L1268))
>
> **何をする所か**: 入力名の `@history` サフィックスを剥がし、`reads` ではなく `reads_history` に入れます。`buildEdges()` は `reads_history` を一切見ないので、history 読みは edge も barrier も生みません。
>
> **素朴に読むと**: 「読んでいるのに依存が無い」は planner だけを見ていると不整合にしか見えません。理由は物理層にあります。history 付き target は画像を 2 枚持ち、`surfaceIndex()` が `history_frame_index`(毎フレーム `^= 1`)で現在面と旧面を切り替えます。つまり `X@history` が読むのは、このフレームに書かれる `X` とは **別の VkImage** であり、フレーム内 hazard が存在しません。素朴に「`reads` へ混ぜる」修正をすると、宣言順で**先行する writer がいる**構成で偽の RAW edge が生え、実際には触っていない側の image を指す resource 名ベースの barrier まで付きます(fixture の TAA 構成には `temporal_accum` の先行 writer がいないので edge は 0 本 — 症状が出ないぶん見落としやすい所です)。なお **self dependency にはなりません**: `buildEdges()` は 1 node 分の `reads` を先に処理してから、その node の `writes` を `last_writer` へ記録します([`frameplan.cpp` 内](../../src/project/frameplan.cpp#L88))。同じ resource を read かつ write しても `last_writer[X] == i` にならないからで、現にパス既定または添付上書きから解決した `load_op: "Load"` の添付は、自分の output を `reads` と `writes` の両方に持ったまま plan が通ります(実効 load_op を解決するのは、実行側では [`colorAttachmentOperations()`](../../src/core/renderingpass/renderingpass.hpp#L364) / `depthAttachmentOperations()`、計画側では ✅RC-3c1b から `parseRenderGraph()` です。「Load なら同一ピクセルの read でもある」という宣言は、core の計画用 parse の `appendAttachment()` 1 か所に集約されていたものが、RC-3c1b でその parse ごと消え、今は parser の [`attach`](../../src/project/rendergraphparse.cpp#L1129) が attachment の use に access `read_write` と footprint `same_pixel` を付け、`frameGraphDefinitionOf()` がそれを `reads` と `writes` に写します)。
>
> **骨子**:
> ```mermaid
> flowchart TB
>     subgraph Frame["frame N: images[0] / images[1]"]
>         Write["write X"] --> Current["surfaceIndex(false) = h"]
>         Read["read X@history"] --> Previous["surfaceIndex(true) = h^1<br/>別イメージ。edge 不要"]
>     end
>     Advance["advanceHistoryFrame()<br/>history_frame_index ^= 1<br/>logical frame の最後"]
>     Frame --> Advance
> ```
>
> **手がかり**: layout tracker のキーが `(rt_id.value << 1) | surface` で、**論理 target ではなく物理面ごとに layout を持つ**(§6.7)ため、現在面と旧面が別 layout でも矛盾しません。history 付き target の `initialLayout()` が `eShaderReadOnlyOptimal` なのは、初回フレームでも読めるよう作成時にクリア済みという前提です。テストは [`frameplanner_test.cpp`](../../test/frameplanner_test.cpp) の "history reads are serialized without an intra-frame dependency"(barrier に `temporal_accum` が出ないことを明示的に要求)。
>
> **不変条件**: `reads` と `reads_history` は別配列のまま保ち、edge 生成は `reads` のみ。extent 再生成や history reset のあとは `layout_tracker.reset()` が必須です — image を作り直すと実 layout は Undefined に戻るので、古い追跡値を残すと同一 layout の早期 return で barrier が丸ごと省略されます。

### graph 順序と Vulkan memory visibility の境界 {#frame-graph-memory-boundaries}

- ordered edge の同 resource write→read を barrier record にする。
- 実行側 [`bufferReadAfterWriteBarrier()`](../../src/core/renderingpass/computetaskcontainer.cpp#L2023) は storage buffer に `vk::BufferMemoryBarrier` を出す。compute consumerの`command_layout: "compute_dispatch"`とrender consumerの`indexed_draw` / `draw_count`では、通常のshader readに加えて`DrawIndirect` / `IndirectCommandRead`をdestinationへ含める。
- image は layout tracker に依存。tracker のキーは `(rt_id, surface_index)` になり、history 付き target の現/旧 surface を別々に追跡します([`render_target_layout_tracker.cpp` 内](../../src/core/vkcore/render_target_layout_tracker.cpp#L54))。
- layout が変われば layout transition が memory dependency を含む。
- storage image が `GENERAL`→`GENERAL` のままなら tracker は早期 return するため、compute→compute の image RAW 専用 barrier は現在も出ない([同](../../src/core/vkcore/render_target_layout_tracker.cpp#L75))。

graph の node 順が正しいことと、Vulkan memory visibility が正しいことは別問題です。新 resource type を足すときは planner edge、runtime resource binding、stage/access mask、queue ownership の4点を一緒に設計します。

### Floyd–Warshall 法と推移閉包(到達可能性の全点対計算)の使用箇所 {#background-usage-13}

**このリポジトリでは**: `src/project/frameplan.cpp`(RC-3c1a で core の `frameplanner.cpp` から移動)の `transitiveClosure()` が到達可否版で、`validateWritesAreOrdered()` が「同じ resource に書く 2 node のどちらが先か決まっているか」を `reach(a,b) || reach(b,a)` で検査します。frame graph は node 数が数十なので O(V³) が問題にならず、密行列のほうが読みやすい、という選択です。設計上の意味と壊し方は [第6章](06_rendering_vulkan_shader.md) §6.3 の難所「writes-writes の曖昧検出」にあります。

## 6.4 logical frame: `renderLogicalFrame()` と `render()` の1フレーム

WP128 で描画の中心は **logical frame** になりました。フレームグラフを GPU コマンドへ変換するのは [`Renderer::renderLogicalFrame()`](../../src/core/vkcore/renderer.cpp#L5484) で、flat 画面用の [`Renderer::render()`](../../src/core/vkcore/renderer.cpp#L7191) はその 1-view アダプタです。`renderLogicalFrame()` 本体は 26 行しかなく、private な[入れ子クラス `Renderer::LogicalFrameRecorder`](../../src/core/vkcore/renderer.cpp#L5498) の step を順に呼ぶだけの driver です（[呼び出しの列](../../src/core/vkcore/renderer.cpp#L7095)）。

以前 1 つの関数スコープを共有していたラムダ群はこの recorder のメンバ関数（`collectPassPlan()` / `resolveViewFamilies()` / `recordAccelerationStructures()` / `recordExecutions()` / `submitFrame()` / `publishFrame()` ほか）になり、ラムダが捕まえていた値は「どの step 以降が有効か」をコメントで名指ししたフィールドになりました。

```mermaid
flowchart TB
    Render["Renderer::render()<br/>flat 用アダプタ"]
    Select["selectGraphVariant(flat)"]
    Target["FlatLogicalFrameTarget<br/>RenderTarget を包む"]
    Camera["Camera から RenderViewFamily($main/$mono) を生成"]
    Logical["renderLogicalFrame(target, view_family)"]
    Render --> Select --> Target --> Camera --> Logical
```

`Renderer::renderLogicalFrame(target, view_families)` の driver は、次の順に処理します。

```mermaid
flowchart TB
    Prepare["once の前処理<br/>下記の 1〜9"]
    Views{"各 $main view が残っている?"}
    Zero{"view 開始後、view 0?"}
    Resize["resize 処理 + instance_container.triggerUpdate()"]
    Execute["executeRenderingPasses(…)"]
    XR{"XR variant の view 0?"}
    Mirror["mirror 中間コピーを記録"]
    EndView["target.endView(view_index)"]
    EndFrame["target.endLogicalFrame()<br/>once の終了処理(下記)"]
    Prepare --> Views
    Views -->|"はい"| Zero
    Zero -->|"はい"| Resize --> Execute
    Zero -->|"いいえ"| Execute
    Execute --> XR
    XR -->|"はい"| Mirror --> EndView
    XR -->|"いいえ"| EndView
    EndView --> Views
    Views -->|"いいえ"| EndFrame
```

- **view 開始**: `target.beginView(view_index)`、`FrameRenderContext(in_flight_frame_index 付き)` の順で、view 0 かを判定する前に行う。
- **once の終了処理**: `target.endLogicalFrame()` の後、RT history flip → instance temporal history advance → family 別 snapshot commit の順。

once の前処理の順序:

1. `DeletionQueue::beginFrame`
2. compiled node が要求する named family を解決。
3. 変化の検知で temporal reset。
4. `FrameResources.beginLogicalFrame(main_view_count, all_family_view_count)`
5. shader reload publication consume。
6. fullscreen input rebind。
7. `updateFrameLights(light_container, resolved families)`
8. `target.beginLogicalFrame(view_count)`
9. family-level projection modifier を適用し、全 family の snapshot / UBO slot を構築。

- **named family 解決**: 標準 `$shadow/directional` は LightContainer provider を補完。
- **temporal reset**: family/view identity・実行順変化 / timeSetRevision / camera discontinuityRevision を検知。
- **updateFrameLights**: shadow draw と LightUBO へ同じ family-selected 行列を渡す。
- **view 0 の更新**: object/skin/morph/material override の GPU 状態を凍結。
- **executeRenderingPasses**: invocation の view_family + view_index で FrameUBO/matrix を選択。secondary single-view scope は main view 0 で一回だけ実行。

providerはtarget acquisitionより前にnon-jitteredな
[`RenderViewFamilies`](../../src/core/renderer/viewfamily.hpp)を完成させます。最低限
`$main`が必要で、従来の単一`RenderViewFamily` overloadはこれを自動で包みます。
CameraとOpenXRは別providerですが、Rendererへ入った後のcardinality検証、
projection modifier、temporal snapshot処理は共通です。compiled graphが
`$shadow/directional`を要求し、callerが同名familyを渡さなかった場合だけ標準
LightContainer providerを補完します。callerが同名familyを渡せば行列を置換できます。
`view_id`はframe間のidentityで、配列位置は当該frameの実行順にすぎません。

logical frame には不変条件があり、破ると例外になります。**検査は `renderer.cpp` に集まっているわけではありません** — view family の形は `src/core/renderer/` 側、scope の形は物理プラン側で見ています。

| 文言 | 条件 | 投げる場所 |
|---|---|---|
| `Renderer logical frame requires a compiled render pipeline` | 現在の `RenderingPassId` に compile 済み pipeline が無い | [`Renderer::renderLogicalFrame()`](../../src/core/vkcore/renderer.cpp#L5484) |
| `render view family ... requires at least one view` | family の `views` が空 | [`viewfamily.cpp` 内](../../src/core/renderer/viewfamily.cpp#L43) |
| `render view family ... contains a view without a stable view_id` | provider が view identity を供給しない | [`viewfamily.cpp` 内](../../src/core/renderer/viewfamily.cpp#L57) |
| `render view family ... contains duplicate view_id` | 同じ family 内の identity が重複 | [`viewfamily.cpp` 内](../../src/core/renderer/viewfamily.cpp#L62) |
| `compiled frame graph node ... requires unavailable view family` | pass/task の `view_family` に対応する provider が無い | [`viewfamilyproviderregistry.cpp` 内](../../src/core/renderer/viewfamilyproviderregistry.cpp#L132) |
| `secondary view-family physical scope must be a single-view template` | secondary family の scope が single-view テンプレートでない | [`buildLogicalFrameViewFamilySchedule()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L120) |
| `Renderer logical-frame views must share one in-flight frame index` | 全 view で in-flight index が同一 | [in-flight index の検査](../../src/core/vkcore/renderer.cpp#L6662) |
| `Renderer logical-frame v1 requires equal per-view extents` | 全 view で extent が同一 | [per-view extent の検査](../../src/core/vkcore/renderer.cpp#L6666) |
| `Renderer logical-frame target format does not match the compiled flat graph` | target color format が compile 済み graph と一致 | [flat graph との format 一致検査](../../src/core/vkcore/renderer.cpp#L6691) |

描画先の抽象は [`ILogicalFrameTarget`](../../src/core/vkcore/renderer.hpp)(`beginLogicalFrame` / `beginView` / `endView` / `endLogicalFrame`)です。view入力は
[`RenderViewParameters` / `RenderViewFamily` / `RenderViewFamilies`](../../src/core/renderer/viewfamily.hpp)が運びます。
`first_person_view`フラグはXR eyeでVRM firstPersonジオメトリを切り替えるためのものです。
実装はflatが`FlatLogicalFrameTarget`(renderer.cpp内部、`RenderTarget`を包む)、XRが
[`OpenXr::XrCompositionTarget`](../../src/core/openxr/openxrcompositiontarget.hpp)です。

各 node の実行は [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1575) です。実行に要る入力(frame graph、module 群、view index、authored schedule、target format ほか)は個別引数ではなく 1 個の [`FrameGraphExecutionRequest`](../../src/core/vkcore/renderer.cpp#L1547) にまとめて渡されます。node ごとに以下をします。

1. incoming buffer barrier を発行する。
2. GPU timing が有効なら `barriers` subrange の timestamp を記録する([barriers subrange の記録](../../src/core/vkcore/renderer.cpp#L2019))。
3. render node なら `RenderPassExecutor::execute()`、compute node なら resource transition と `dispatch()` を呼ぶ。
4. `body` subrange の終了 timestamp を記録する([body subrange の終了記録](../../src/core/vkcore/renderer.cpp#L2496))。

`--gpu-labels` 有効時は 1〜4 全体が debug-utils のコマンドラベルで囲まれます(§6.16)。

> 🧩 **難所 — sprite の pass(✅RC-5s)**([`executePlannedFrameGraph()` の sprite の分岐](../../src/core/vkcore/renderer.cpp#L2258))
>
> **何をする所か**: type `sprite` の render node(sprite feature の `sprite_draw`)に着いたとき、compiled pass が持つ `output_color[0]` と `output_depth` を attachment にして `SpriteRenderer::render()` を呼びます。RC-5s の前は空のノード `__anchor_sprite` の分岐で、それまでに実行した render pass の出力を `latest_scene_color` / `latest_scene_depth` で追い、その時点の最新の color / depth に描いていました。追う変数ごと消えています。
>
> **素朴に読むと**: 引っ掛かる所は 3 つです。
>
> 1. **普通の render pass とは別の分岐**: `RenderPassExecutor` を通らず、`render_pass->definition.isSprite()` の分岐が `transitionPassOutputsToAttachmentLayouts()` を自分で呼び、`createColorAttachments()` / `createDepthAttachment()` / `getRenderPassTargetExtent()` で attachment の view と extent を作ります(XR の layer の view も共通の関数で取る形に、コードレビューで直した、と実装者の報告)。rendering の begin / end は `SpriteRenderer::render()` 側が持つので、この pass は融合した scope に入れません。計画は `sprite` / `ui` / `imgui` を単独の scope にし([`isolatedNodeConstraints()`](../../src/core/renderingpass/renderingsamplecount.cpp#L151))、計画や complete package(✅RC-6b2 の前は physical fragment も)が融合していれば `render_graph.scope_implementation_unfusible@1` で断り([計画](../../src/core/renderingpass/renderingsamplecount.cpp#L1742)、[package の install](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L955))、runtime の compile も `Scope-owning pass implementation cannot execute inside a fused physical rendering scope` で断ります([runtime の compile](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L699))。
> 2. **描く物が無いとき**: `resolveSpriteRenderModules()` は feature が有効でも `SpriteScene` の `frame().logical_count` が 0 なら空を返し(前は `commandCountForTesting()`)、分岐は trace だけ積んで何も描きません。node は実行の列に残ります。timing の `body` は `visible_count > 0` のときだけ supported です([`plannedTimingNode()`](../../src/core/vkcore/renderer.cpp#L1290))。
> 3. **描く先は IR が知っている**: 描く先は feature の parameter で、IR の use(color / depth の read_write)として計画に乗るので、前後の barrier と lifetime は普通の pass と同じ道で決まります(`passDefinitionOf()` の `SpritePassInfo`、shape の規則は color 1 枚 + depth 必須、attachment の既定は load / store)。範囲の外の attachment(use の layer 数が scope の view 数と違う、MSAA の scope で mip が 0 でない)は計画で `render_graph.sprite_attachment_range_unsupported@1` です([範囲の外を断る所](../../src/core/renderingpass/renderingsamplecount.cpp#L1732))。
>
> **骨子**:
> ```text
> sprite の node:
>   trace に renderNodeTrace
>   sprite.scene が無い(描く物なし)→ 終わり
>   color = output_color[0](具体か swapchain)、depth = output_depth(具体)でなければ throw
>   transitionPassOutputsToAttachmentLayouts
>   colors = createColorAttachments、depth = createDepthAttachment、extent = getRenderPassTargetExtent
>   SpriteRenderer::render(color / depth の view、format、resolve の view、samples、resolve mode)
>   trace.sprite_draw = {color, depth, depth_test: true, depth_write: false, blend: straight_alpha, policy}
> ```
>
> **手がかり**: samples と color の resolve mode は color target の値(swapchain なら 1 sample / resolve なし)です。テスト用の `Renderer::readRenderTargetForTesting()` は、MSAA の target でも読み出す resolve の image が single-sample であることを見る形に直りました(前は metadata の sample 数を見ていて、MSAA の resolve を読めなかった)。
>
> **不変条件**: sprite の描く先は compiled pass の attachment だけです。「最新の scene color」のような暗黙の推測に戻さないこと(戻すと、指定先と最新の writer の decoy が違う fixture の画素比較が赤。RC-5s の対照)。

**逐次 XR の共有 family の完了(✅RC-5s)。**`FrameGraphExecutionRequest` に `completed_logical_nodes` が足されました。sequential の XR で、左眼の前に 1 度だけ記録する family の node(例: directional の影の `shadow_depth`)を、右眼の記録が「未実行」と見て barrier の検査(`Compiled frame graph barrier source was not executed before target`)で落ちていたのを、論理フレームの中で完了を引き継ぐ形で直したものです([引き継ぐ所](../../src/core/vkcore/renderer.cpp#L1685))。その眼で記録し直す node は完了を消すので、眼ごとの順の検査は残ります。RC-5s の runtime の matrix(出荷 7 project + 16 構成)で見つかった、と実装者の報告。検査の文は `<from> -> <to> (<resource>)` を付けるようになりました。

`currentFramePlanJson()` と testing trace は [`Renderer` の診断用メソッド](../../src/core/vkcore/renderer.hpp#L341) です。RPC の `get_frame_plan` やテストから、設定がどの順に解釈されたかを GPU debugger なしで確認できます。multi-view 時の execution trace は view ごとの配列形状になります。

**世代で決まる部分の持ち回し(✅FP-1、`eab549b6`)。**`currentFramePlanJson()` は、返り値を 2 つに分けて組みます。

- **世代で決まる部分**: [`framePlanGenerationPartJson()`](../../src/core/vkcore/renderer.cpp#L4224)(無名 namespace の関数)が、公開した世代 `RendererRuntimeGeneration` と program だけから組みます。`framePlanToJson()` の plan(ループ・`diagnostics`・fold の決定)、`runtime_generation`、`gpu_owner_scope`、`retained_resource_lease_count`、`window_output`、`gpu_resource_arena`、`physical_target_plan` と `appendFramePlanPhysicalNodeReferences()` の `from_nodes`、`native_scope_executors`、`sample_count_plan` です。frame・scene・file・runtime の設定は読みません(コードの注記)。
- **持つ所**: [`Renderer::FramePlanGenerationPart`](../../src/core/vkcore/renderer.hpp#L296)(`mutable` の member `frame_plan_generation_part`)が、世代の `weak_ptr`・世代の番号・`RenderingPassId`・root の参照(`ProjectBasicConfig::renderingConfigReference()`)と、組んだ `nlohmann::json` を持ちます。4 つのどれかが違うか、まだ組んでいなければ組み直します([FP-1 で持っている plan を比べる所](../../src/core/vkcore/renderer.cpp#L4542))。`builds` は組んだ回数で、test が読みます。`currentFramePlanJson()` は frame の thread(frame の境界の RPC と plan viewer)だけで走るので lock は取りません(ヘッダの注記)。
- **呼ぶたびに足す部分**: 持っている plan の写しに、`shader_resolution`(source の file が在るか)、`preview`、`no_selected_view`(LP-Va2)、PS-1 の `last_run_generation` / `last_run_frame`、`runtime_callable_records`、VI-1 の `pass_inputs` と `unused_binding`、LC-1 の `reads_loop_index`、material filter の解決、MP-1 の `drawn_materials`、LP-3a の draw_loop の要素、`ray_query_acceleration_structures`、`runtime_resolution`、RC-1 の `memory_summary` と物理 image の `bytes`(下の「物理 image の bytes と `memory_summary`」)、`lighting_data_runtime`、`sprite`、IA-1 の `initial_asset` を足します。返り値の bytes は FP-1 の前と同じです(commit の記録では前後 6 組の返り値を比べ、test は持っている plan を捨てて組み直した呼び出しの応答の 1 行と比べます)。
- 迷うものは呼ぶたびの側に入れる、が仕様書の方針です(古い値を返さないため)。
- test: `engine_primitives_test` の「FP-1 get_frame_plan builds its generation's part once and reads a frame's part on every call」(GPU。同じ世代の 2 回で組むのは 1 回・bytes 一致、`rerun_once` の後は組まずに `last_run_frame` が変わる、`reload_render_documents` の後は 1 回組み直す。test 用の口は `test/renderer_testaccess.hpp` の `dropFramePlanGenerationPart()` / `framePlanGenerationPartBuilds()`)。

### 物理 image の bytes と `memory_summary`(✅RC-1、`631caac21` / ✅RC-1b、`504a6492a`)

`get_frame_plan` の `physical_target_plan.resources[].bytes` と最上位の `memory_summary {total_bytes, unused_bytes, unused_resources, peak_live_bytes, peak_node, peak_graph}`(形と意味は manual [第6章](../manual/06_rendering.md#physical-plan-memory) §6.6)は、1 枚の bytes を数える式を planner の純ロジックに 1 か所だけ持ち、renderer と GPU arena はそこへ値を渡して答えを写すだけです。RC-1b で合計の出どころが計画から runtime の登録に移りました。runtime は render target の定義を全部登録し(どの pass も使わないものも)、alias の組は全 graph が合意したときだけ残すので、合計は 1 つの計画からは言えません。

- **式の置き場**: [`targetrendermemory.hpp`](../../src/project/targetrendermemory.hpp#L18)(`pelican_project`)。Vulkan の enum に依存せず、format は project の [`ImageFormat`](../../src/project/imageformat.hpp#L15)(✅RC-3a1)で受けます(`ImageResourceByteShape::format`。RC-3a1 の前は物理計画の綴りの文字列 `"R16G16B16A16Sfloat"` でした。語彙は下の「image の語彙」)。
  - [`imageFormatEstimatedTexelBytes()`](../../src/project/imageformat.cpp#L262): texel の大きさは format の性質表 [`kImageFormats`](../../src/project/imageformat.cpp#L17)(46 行)の `estimated_texel_bytes` の列です。RC-1 で renderer.cpp の switch を `targetrendermemory.cpp` の表に移し、RC-3a1 でその表を性質表の列に寄せました(値は 46 行とも同じ。RC-3a1 の実装者の報告)。renderer の [`formatTexelBytes()`](../../src/core/vkcore/renderer.cpp#L1097) は `ImageFormat` を受けてこの列を返すだけです(snapshot の `byte_size`・readback・IA-1 の `initial` の大きさの照合が使います)。引数が enum なので「表に無い format」は型の上で起きません。render target の JSON parser が引く一覧([`authoredImageFormatNames()`](../../src/project/imageformat.cpp#L164)、作者が書ける 45 種)が全部 0 より大きい bytes を持つことは test が固定します(RC-1b)。`D32_SFLOAT_S8_UINT` の 8 は Vulkan の texel block の 5 ではなく、実装が置く大きさの見積もりです(ヘッダの注記)。
  - [`imageResourceBytes()`](../../src/project/targetrendermemory.cpp#L13): mip の全段の和 × layers × 面(history は 2)。MSAA は `RenderTargetContainer` が作るとおり、面ごとに 1 sample の image(mip 込み)と samples 倍の image(mip 0 だけ)を数えます。
  - [`vulkanPhysicalResourceBytes()`](../../src/project/targetrendermemory.cpp#L52): 物理計画の資源と、確保した extent・面の数から数えます。`full_chain` の mip はその extent から解き直します。
  - [`summarizeVulkanTargetPlanMemory()`](../../src/project/targetrendermemory.cpp#L74): 1 つの graph の計画の、資源ごとの bytes と peak。bytes は呼び手の関数(`VulkanPhysicalResourceByteCounter`)に聞き、聞くのは `vulkanPhysicalResourceIsPlannedImage()` が受ける資源(materialized / transient / tile_local の image)だけです。RC-1b から counter は `std::uint64_t` を返し、数えられない呼び手は資源の名前を付けて throw します(summary が計画の一部だけを覆うことはありません。空の counter は `invalid_argument`)。peak は order ごとの生存の和の最大を最初に取った order で、`peak_node` はその order の `lowering_graph.nodes` の名前です。RC-1 にあった total(計画の `alias_groups` の組を 1 回)は RC-1b で summary から外しました。
  - [`totalRegisteredTargetMemory()`](../../src/project/targetrendermemory.cpp#L121)(RC-1b): `RegisteredTargetMemory {name, bytes, alias_shared_bytes, alias_group, used}` の列から `RegisteredTargetMemoryTotal {total_bytes, unused_bytes, unused_resources}` を数えます。`alias_group`(registry の alias token)ごとに `alias_shared_bytes` の最大を 1 回、残り(`bytes - alias_shared_bytes`、MSAA の image)は住人ごとです。共有分が自分の bytes を超えると `invalid_argument`。`unused_bytes` は使われない住人の自分の分と、住人が 1 つも使われない組の共有分です。1 件の `RegisteredTargetMemory` は [`registeredRenderTargetMemory()`](../../src/core/renderingpass/rendertargetcontainer.cpp#L1707)([宣言](../../src/core/renderingpass/rendertargetmemory.hpp#L16))が `RenderTargetMetadata` から作ります(共有分 = samples を 1 にした metadata の `renderTargetBytes()`)。
  - [`appendVulkanTargetPlanMemoryJson()`](../../src/project/targetrendermemory.cpp#L161): `memory_summary` を足し(total / unused は `RegisteredTargetMemoryTotal` から、peak と `peak_graph`(= `plan.graph`)は summary と計画から)、`physical_target_plan.resources[]` の要素を `logical_resource` で引いて `bytes` を足します。
- **renderer**: [`currentFramePlanJson()` の RC-1 / RC-1b の分](../../src/core/vkcore/renderer.cpp#L4823)は FP-1 の持ち回しの外にあり、呼ぶたびに [`appendFramePlanMemoryJson()`](../../src/core/vkcore/renderer.cpp#L4423)(無名 namespace)で数えます。
  - 登録の事実は `RegisteredRenderTargetLookup`([`RegisteredRenderTargetFacts`](../../src/core/vkcore/renderer.hpp#L69) = `getMetadata()` と `aliasGroup()` の答え)で引きます。本番は `RenderTargetContainer` から(`registrationsSince({})` に無い id は nullopt)、CPU だけの test は [`RendererFramePlanCpuSeam::render_targets`](../../src/core/vkcore/renderer.hpp#L82) の代役から引きます。どちらも無ければ memory の部分は出ません。
  - 使用の判定: 世代の全 program の物理計画の planned image を `render_target_bindings` で引いた id の集合です(OpenXR の mirror の中間は常に使用)。
  - 合計: 世代の `gpu_arena` の全 scope の `render_target` の登録を handle で 1 回ずつ、今の metadata と alias token で `registeredRenderTargetMemory()` にして `totalRegisteredTargetMemory()` へ渡します。同時に、dump の `gpu_resource_arena.scopes[].resources[]` の render target の `declared_bytes` をその bytes で書き換えます(FP-1 の持ち回しから写した JSON の上で。arena の構造体の値は変えません)。
  - peak と資源ごとの `bytes`: 今の program の計画を `summarizeVulkanTargetPlanMemory()` で数えます。資源の名前を `render_target_bindings` で引き、concrete な render target なら [`renderTargetBytes()`](../../src/core/renderingpass/rendertargetmetadata.hpp#L42) を返します。
  - binding が無い(`unbound`)・登録が無い(`not_registered`)ときは `throwFramePlanMemoryUnmeasured()`([定義](../../src/core/vkcore/renderer.cpp#L4407)) が `frame_plan.memory_unmeasured@1: {"resource": ..., "reason": ...}` の `runtime_error` を投げます。RC-1 では binding の無い資源を黙って数えませんでした。正しい状態では起きません(`validateRuntimePhysicalPlan` が定義の無い計画の image を拒否し、登録は定義を全部登録し、program の `render_target_bindings` は登録の後の `currentNameBindings()` の写し。RC-1b の実装者の報告の調査)。
- **GPU arena**: [`RenderPipelineGpuRegistrationArena::preparedScope()`](../../src/core/renderingpass/renderpipelinegpuarena.cpp#L563) の [RC-1 の分](../../src/core/renderingpass/renderpipelinegpuarena.cpp#L582)が、render target の登録の `declared_bytes` に同じ `renderTargetBytes()` を入れます。これは登録時の値で、登録の後に同じ id のまま image を作り直す [`RenderTargetContainer::publishPreparedExtent()`](../../src/core/renderingpass/rendertargetcontainer.cpp#L1178)(window 以外の論理 target の resize)と [`RenderTargetContainer::setRuntimeArrayLayers()`](../../src/core/renderingpass/rendertargetcontainer.cpp#L1066)(影の view provider が layer 数を変える)は arena に触りません。RC-1b から `get_frame_plan` の dump だけは上の書き換えで今の値になります。arena の構造体を直接読む消費者を新しく足すと、登録時の値を読むことになります。
- **Studio**: [`FramePlanMemorySummary`](../../src/devstudio/model/frameplanmodel.hpp#L738) と `FramePlanResource::bytes`。鍵の無い plan(RC-1 より前、物理計画の無い plan)も開き、あるのに形が違えば invalid です([読む所](../../src/devstudio/model/frameplanmodel.cpp#L2052))。読むのは `total_bytes` / `peak_live_bytes` / `peak_node` だけで、RC-1b の `unused_bytes` / `unused_resources` / `peak_graph` は読まずに無視します。表示は manual [第15章](../manual/15_studio.md) §15.8「Frame Plan のメモリの数」。
- 残した穴(RC-1b の実装者の報告): `unbound` の分岐を通す test は無い(`not_registered` だけ)。owner scope を置き換えたあと、置き換えられた scope の render target が別 owner の program の lease で生きている間は arena から消えているので total に入らない(数え漏れの方向。普段の 1 owner では起きない)。未使用の render target を確保していること自体の除去は RC-7 の範囲です。
- test(GPU 不要):
  - `targetrenderplanning_test` の [「RC-1 image bytes count every mip, layer and history surface and the multisampled image」](../../test/targetrenderplanning_test.cpp#L2784)、[「RC-1 memory summary counts an alias group once and finds the node most bytes are alive at」](../../test/targetrenderplanning_test.cpp#L2840)(RC-1b で、counter の throw が名前付きで伝わることも見ます)、[「RC-1 the memory total counts each allocation registration made once and names the targets no graph uses」](../../test/targetrenderplanning_test.cpp#L2966)(RC-1b)。
  - `frameplanner_test` の [「RC-1 every format a render target can be authored with has a texel size」](../../test/frameplanner_test.cpp#L2897)(RC-1b)、[example の 1920×1080](../../test/frameplanner_test.cpp#L3002)(total 125,776,320 / peak 85,017,600 at `lighting_pass`。RC-1b から total は登録の代役 [`registerRenderTargetsWithoutDevice()`](../../test/registered_render_targets_test_support.hpp#L80) → `totalRegisteredTargetMemory()` で数え、値は同じ)、[「RC-1 get_frame_plan reports what the published generation registered, as it is now, through the renderer's frame plan」](../../test/frameplanner_test.cpp#L3138)(RC-1b、tag `[production-seam]`。世代を `prepareGeneration` → `publishPreparedGeneration` で公開し、`Renderer{RendererFramePlanCpuSeam{lookup}}` の `currentFramePlanJson()` を通します。未使用の 64×64 R8 を足した total・unused、layers を変えた後の arena / `bytes` / total / peak、登録を消したときの `memory_unmeasured@1`)。この test は arena への登録そのもの(Vulkan device が要る)は通しません。
  - `renderingsamplecount_test` の [「RC-1 when graph variants do not share an alias group, the memory total counts its targets as two allocations」](../../test/renderingsamplecount_test.cpp#L1545)(RC-1b)。
  - GPU: `headless_render_test` の「hybrid_v1 preset registers and renders a headless frame」(arena と plan の bytes の照合)を含む、`get_frame_plan` を呼ぶ GPU test 10 本は RC-1b の後に統合側で実行して緑です。

<a id="target-resource-lifetimes"></a>

### 資源の寿命と保持の理由(✅RC-6c、`be795c9a5`)

物理計画の資源の寿命 [`TargetResourceLifetime`](../../src/project/targetrenderplanning.hpp#L90) `{used, first_use, last_use, retain}` は、lowering graph の node の順の番号です。RC-6c から `last_use` は graph の中の最後の use で、graph の外で読まれる資源だけが理由(`retain`)つきで末尾の node まで延びます(形と意味・example の数字は manual [第6章](../manual/06_rendering.md#physical-plan-lifetime) §6.6、設計は [RT メモ §2](../design_render_target_memory_reduction.md#rc6c-lifetime))。RC-6c の前は planner と complete verifier がそれぞれ first / last を取り、`required` / `external` / `require_store` の資源を一律に末尾へ延ばしていました。その 2 つのコードは消え、次の 1 つの関数に寄りました。

- **寿命の関数**: [`calculateTargetResourceLifetimes()`](../../src/project/targetrenderplanning.cpp#L1594)(header で公開)。入力は canonical graph・検査済みの `FramePlanNodeOrder`・資源名ごとの理由 `TargetResourceRetainReasons`(`std::map<std::string, std::vector<std::string>>`)。node の順に use を見て `used / first_use / last_use` を取り、使われた資源で理由が空でない物だけ `retain` を辞書順・重複なしにして `last_use` を末尾にします。使われない資源の理由は捨てます。知らない理由は [`isTargetResourceRetainReason()`](../../src/project/targetrenderplanning.cpp#L1587) で `unknown resource retain reason: <reason>` の throw です。
- **理由を作る所(IR から)**: [`renderingTargetRetainReasons()`](../../src/core/renderingpass/renderingsamplecount.cpp#L1478) が graph で使われる image の `history` → `history`、`persistent` か `source` → `persistent` を付け、`swapchain` を書く use に `present`(と、[`RenderingTargetExternalReaders`](../../src/core/renderingpass/renderingsamplecount.hpp#L115) の `capture_output` が真なら `capture`)、shape が picking の node が color attachment として書く `picking_id` に `picking`、`xr_mirror` が真で `display` が使われていれば `display` に `xr_mirror` を付けます。[`compileRenderingTargetPlans()`](../../src/core/renderingpass/renderingsamplecount.cpp#L1513) が graph ごとに呼び、`VulkanTargetPlanRequest::retain_reasons` に入れます([入れる所](../../src/core/renderingpass/renderingsamplecount.cpp#L1690))。
  - `xr_mirror` と `capture_output` は [`compileDefaultVulkanVariant()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L368) が渡します。`xr_mirror` は解決済みの variant の `mirror_output == left_eye`、`capture_output` は `RenderCompilerProgramVariantRequest::capture_output` で、登録の準備([決める所](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L984))が flat の variant で実際の FrameTarget の `compile_facts.capture_available` が真のときに立てます。
  - `external_depth_export` だけは planner の中で付きます。[`compileVulkanTargetPlan()`](../../src/project/targetrenderplanning.cpp#L3765) が既存の `selectExternalDepthExport()` で元を選んだ後、その資源に理由を足して寿命を計算し直し、lowering の資源へ写します([その所](../../src/project/targetrenderplanning.cpp#L3819))。
- **planner**: [`makeTargetLoweringGraph()`](../../src/project/targetrenderplanning.cpp#L1625) が最初に寿命を計算し、lowering の資源を作るときに入れます(引数の理由は既定で空)。選んだ計画の `VulkanTargetPlan::retain_reasons` には request の理由(depth export の分を足した後)を残します。この欄は寿命の出力ではなく信頼する入力で、JSON には書きません(header の注記)。
- **complete verifier**: [`verifyVulkanCompletePhysicalPlanPackage()`](../../src/project/vulkancompletephysicalplan.cpp#L1542) は検査済みの順から `FramePlanNodeOrder` を作り直し、`automatic_plan.retain_reasons` で同じ関数を呼びます([その所](../../src/project/vulkancompletephysicalplan.cpp#L1579))。`deriveUses()` は寿命を取らなくなり、access・最も広い read の footprint・transfer の有無だけを集めます。package の資源の寿命は `TargetResourceLifetime` の `==`(`retain` を含む)で比べ、違えば `Vulkan complete physical resource has stale lifetime or read-footprint metadata` です。package の申告も自動計画の寿命も写しません。
- **JSON**: `retain` は空でなければ書き、空なら鍵ごと書きません。書く所は物理計画の [`vulkanTargetPlanToJson()`](../../src/project/targetrenderplanning.cpp#L4077)、lowering graph の [`targetLoweringGraphToJson()`](../../src/project/targetrenderplanning.cpp#L1846)、complete package の `resourceToJson()`([その所](../../src/project/vulkancompletephysicalplan.cpp#L439))。complete の `resourceFromJson()`([読む所](../../src/project/vulkancompletephysicalplan.cpp#L516))は文字列の配列で、`used` が真・空でない・重複なし・既知の理由だけを受け、並べ替えて持ちます。Studio の wire 検査は `retain` のある JSON をそのまま受けます(Studio のコードは変わっていません)。
- **fingerprint**: [`appendLifetimeFingerprint()`](../../src/project/vulkanphysicalfragment.cpp#L301) が `retain` が空でないときだけ数と各理由を足します。理由の無い資源の fingerprint の入力は前と同じで、RC-6c で値が変わるのは寿命か理由が変わった計画です。
- **変えていない所**: alias の候補(寿命が重ならない組。[`deriveLegalAliasCandidates()`](../../src/project/targetrenderplanning.cpp#L3175))・runtime の alias の適格性・登録・representation・attachment の ops・peak の数え方([`summarizeVulkanTargetPlanMemory()`](../../src/project/targetrendermemory.cpp#L74) の閉区間)。出荷の構成で寿命が縮んだ資源は runtime の alias の対象外なので、alias の組は変わりません(test の注記)。
- 理由にしない物(snapshot・cube の種類だけ・XR が有効なだけ・RenderDoc・preview・ImGui・timing・NativeScope・buffer の readback)と、RC-7 に残した物(領域の使い回し・実メモリの削減・名前の readback の opt-in)は manual の節のとおりです。
- test(GPU 不要):
  - `targetrenderplanning_test` の [「RC-6c E1 reasons retain only used resources beyond their final graph use」](../../test/targetrenderplanning_test.cpp#L2593)(7 つの理由ごとの延長、使われない資源、2 つの理由の重なり)、[「RC-6c E2 complete independently verifies shortened lifetimes and retain reasons」](../../test/targetrenderplanning_test.cpp#L2617)(往復・検査、`last_use` / `retain` を書き換えた package の拒否、dump の欄の有無、壊れた `retain` の拒否)、[「RC-6c E3 required aliasable images without store gain only nonoverlapping aliases」](../../test/targetrenderplanning_test.cpp#L2660)。
  - `renderingsamplecount_test` の [「RC-6c E1 compiler facts select history persistent source initial picking mirror and output readers」](../../test/renderingsamplecount_test.cpp#L2065)(IR と読み手の事実から理由を作る所)。
  - `frameplanner_test` の [「RC-6c E3 shipping projects keep allocation eligibility representations and attachment operations」](../../test/frameplanner_test.cpp#L2914)(出荷 7 project)と、example の 1920×1080 の test の [RC-6c E4 の後半の生存量](../../test/frameplanner_test.cpp#L3103)。
  - `devstudio_frameplan_test` の [「RC-6c Frame Plan accepts and preserves lifetime retain in the physical wire」](../../test/devstudio_frameplan_test.cpp#L774)。RC-5 の 16 構成の A5 は資源ごとの最後の use の node と理由を表で検査してから比べます([表](../../test/rc5_recipe_compiler_test.inc#L340))。

### once の node を記録しない frame(PS-1)

✅PS-1(`9b3c9566`)の `schedule: "once"` の pass / task は、frame plan の node に `once` の印を持ち(`FrameGraphNodeDefinition::once` → `FramePlanNode::once` → `FrameGraphExecutionNode::once`)、**走る frame かどうかを renderer が frame ごとに決めます**。書き方と規則は [manual 第6章](../manual/06_rendering.md) §6.18「一度だけ走る pass と、frame をまたぐ image」です。

1. `renderLogicalFrame()`
2. `publishFrameValueGraph / stagePoseAttributesAndLights`: host が buffer に書く。digest もここで。
3. `planOnceNodes()`: once の node を `planOnceRuns()` に渡し、走らない node の index に印を付ける。
4. `planLogicalFrameSchedule()`: 以下は今までどおり。
5. `recordExecutions()`: `executePlannedFrameGraph()` が印の付いた node を飛ばす。
6. `publishFrame()`
7. `retainOnceRuns()`: 走った node の記録を `Renderer::once_runs` に残す。

- **決める所**: [`Renderer::LogicalFrameRecorder::planOnceNodes()`](../../src/core/vkcore/renderer.cpp#L6903) が、once の node のうち owner が活性なものを `OnceScheduleNode {name, reads, writes}`(plan の node の読み書き)にして、[`planOnceRuns()`](../../src/core/renderingpass/oncescheduler.cpp#L5) に渡します。`planOnceRuns()` は device を持たない純粋な関数で、理由(`generation` / `requested` / `upstream` / `output_recreated` / `input_changed`)を node ごとに返します([理由の表](../../src/core/renderingpass/oncescheduler.hpp#L15))。`upstream` は、同じ frame で plan の先に走ると決まった once の node の書くものを読む node です(plan の順に 1 回なめるので、連鎖は前から後ろへ伝わります)。
- **比べる値**(`OnceResourceFacts`): `identity` は resource の名前から今の `VkImage`(concrete な render target)か `VkBuffer` の handle の値、`host_digest` は [`FrameGraphResourceContainer::hostWriteDigest()`](../../src/core/renderingpass/framegraphbuffercontainer.hpp#L101) です。digest は [`writeHostBuffer()`](../../src/core/renderingpass/framegraphbuffercontainer.cpp#L272) が書くたびに bytes と長さの FNV-1a 64 を取り([取る所](../../src/core/renderingpass/framegraphbuffercontainer.cpp#L303))、`copyBufferState()` が作り直しをまたいで写します。host が一度も書いていない buffer は値を持たず、比べません。
- **飛ばす所**: [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1575) は `FrameGraphExecutionRequest::once_skipped`(node の index ごとの 0 / 1)を見て、印の付いた node を記録せずに `completedWithoutRecording` にします([その分岐](../../src/core/vkcore/renderer.cpp#L1734))。GPU timing の query には barriers と body の始めと終わりを続けて書き、testing trace には `{"recorded": false, "once_skipped": true}` を積みます。node の index で見るので、view が何個でも同じ node は全部の view で飛びます。
- **記録**: [`retainOnceRuns()`](../../src/core/vkcore/renderer.cpp#L6953) が publish の後に [`onceRunRecord()`](../../src/core/renderingpass/oncescheduler.cpp#L62)(世代、`engine_time.evaluationIndex()`、書いた resource の handle、読んだ host buffer の digest)を `Renderer::once_runs[<program>][<node>]` に入れ、`once_rerun_requests` からその名前を消します。`rerun_once` はこの集合に名前を入れるだけです([第7章](07_tools_rpc_tests.md) の `rerun_once` の行)。
- **scope**: 飛ばした node は scope を始めないので、target planning は once の node を必ず自分だけの物理 scope に isolate します([`isolatedNodeConstraints()`](../../src/core/renderingpass/renderingsamplecount.cpp#L151)。✅RC-5s で `onceIsolatedNodeConstraints()` から改名し、rendering の begin / end を自分で持つ `sprite` / `ui` / `imgui` の node も isolate する。書いた `target_planning` の node の制約に `isolate: true` を足すか、足りなければ新しく足す)。
- **compute**: once の compute task は、走るときは per_frame と同じ扱いで view の独立な候補に入ります([view の独立な候補に入れる所](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L73)。RC-3c1b から IR の `NodeIR.schedule` で判定)。
- **穴**: `view_select` に合う view が無かった once の node も `once_ran` に入ったまま記録されます(仕様書の記述。コードの読みでも `retainOnceRuns()` は `no_selected_view` を見ていません)。

### persistent の image の初期内容と KTX2 の float(IA-1)

✅IA-1(`c6690eba`)の `render.image` の `initial: {asset}` は、lowering → 世代を組む段 → frame の頭の copy の 3 か所を通ります。書き方と拒否は [manual 第6章](../manual/06_rendering.md) §6.18「persistent の image を画像資産から始める」です。

| 段 | 処理順 |
|---|---|
| 1: lowering | `lowerRenderGraphDocumentForComposition()` |
| 2: `RegistrationStages::prepare()` | 1. `resolveGraphImageInitialAssets()`<br/>2. `reader(node, asset)`<br/>3. `resolveExistingFile`<br/>4. `assetFileSha256(IA-2)`<br/>5. `loadImageFile`<br/>6. level 0 の texel と manifest の sha256<br/>7. 照合: `graph.image_initial_mismatch@1`<br/>8. `attachProgramGraphState()`: 活性な node の分を `program.image_initial_assets` へ |
| 3: `renderLogicalFrame()` | 1. `buildDrawSortViews()`<br/>2. `uploadImageInitialAssets()`<br/>3. `planOnceNodes()`: once はこの後(copy の上に書ける) |

- **lowerRenderGraphDocumentForComposition**: checkRenderImageInitial()、usage に TRANSFER_DST、extent_scale の既定 1.0。
- **resolveGraphImageInitialAssets**: 文書を分割して initial のある render.image ごとに reader を呼ぶ。
- **照合**: width / height を省いた image に asset の大きさを書く。
- **uploadImageInitialAssets**: (VkImage, 世代, asset) が覚えたものと違う target にだけ copy。

- **lowering**: [IA-1 の lowering の分岐](../../src/project/rendergraphcomposition.cpp#L883)が [`checkRenderImageInitial()`](../../src/project/graphpartition.cpp#L552) を呼び、`usage` に `TRANSFER_DST` を足し、`extent_scale` が無ければ 1.0 を入れます。`initial` は target の宣言に残り、compile は `initial` のある target を、buffer と同じく frame の頭から中身があるものとして pass の入力の検査に渡します(✅RC-3c1b から IR の `ImageResource.initial` / `.source` で判定。[IA-1 / EN-2 の書き手の無い読みを通す所](../../src/project/rendergraphir.cpp#L388))。
- **読む・照らし合わせる**: [`resolveGraphImageInitialAssets()`](../../src/project/graphpartition.cpp#L582) は、root と definition のどこにも `initial` が無ければ文書をそのまま返します。あれば `partitionGraphDocument()` の rendering の node を回り、ループの body の node(`origin.loop` が空でない)を断り、reader の答え([`GraphImageInitialAsset`](../../src/project/graphpartition.hpp#L64) の width / height / format / `color_space_declared`)と比べます。format は `SRGB` を `UNORM` に置き換えた形で比べ、形が同じで sRGB の別が違うのは `color_space_declared` のとき(KTX2)だけ `color_space` です。width / height を省いた image には、`graph.origins` で authored の node を引いて asset の大きさを書きます(同じ definition の node は 1 回)。reader は [`RegistrationStages::prepare()` の IA-1 の段](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L943)のラムダで、`loadImageFile()` の結果が 2D・1 層・depth 1・非圧縮でなければ `std::invalid_argument` を投げ、level 0 の bytes を [`RenderImageInitialAsset`](../../src/core/renderingpass/framegraphruntime.hpp#L44) に写します。`sha256` は file を持つ asset store の manifest を読んで引き([`assetManifestSha256()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L78))、読めなければ `nullopt` です(検証の報告はこの関数の仕事ではない、とコードの注記)。
- **copy**: [`Renderer::LogicalFrameRecorder::uploadImageInitialAssets()`](../../src/core/vkcore/renderer.cpp#L6855) は、`Renderer::image_initial_uploads[<program>][<target>]` の `{image, generation, asset}` と今のものが 1 つでも違う target にだけ copy します。target の大きさ・sample 数・層・format の形(SRGB を UNORM と見る)・bytes の数が asset と合わなければ warning を出して飛ばします。copy の前に `waitIdle()` し(前の世代の frame が読んでいる image を保った場合のため)、`VulkanUtils::safeTransferMemoryToImage()` で UNDEFINED → TRANSFER_DST に写して、layout tracker に `assumeLayout(TRANSFER_DST)` を伝えます。persistent の target は PS-1 の tracker の規則で layout を覚えます。
- **監視**: [`renderPipelineWatchSources()` の IA-1 の分](../../src/core/vkcore/renderer.cpp#L362)が asset の file を render pipeline の監視の鍵に入れます。
- **reload の比較(✅IA-2、`cdcdebc1`)**: reader は decode の前に file の中身を `assetFileSha256()` で hash し([decode の前の hash IA-2](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L953))、`RenderImageInitialAsset::content_sha256` に入れます。`--rpc` の engine では watcher が render pipeline に届かないので、`reload_render_documents` がこの値と今の file を比べて、asset だけが変わった世代も組み直します。editor 側の流れは [第7章](07_tools_rpc_tests.md) §7.7 の「`reload_render_documents` の ticket」です。
- **値の graph が読む表(✅IA-3、`26473e75`)**: `value.sample_table` の表は image ではなく値の graph が構築の時に読むので、上の reader を通りません。代わりに `program.frame_graph.value_graph` の [`Graph::sampleTables()`](../../src/core/userpublic/valuegraph.cpp#L881) が表(`SampleTable` の `file` / `manifest` と 2 つの sha256)を返し、2 か所がそれを読みます。比較の依存 `published_assets`(IA-2 の `published_initial_assets` を改名。型は `RenderConfigPublishedAsset`)の本番の実装は、image の initial asset に続けて表と manifest を表の参照の名前で 2 行ずつ足します([IA-3 の published_assets の行](../../src/core/communication/editorruntimefactory.cpp#L2467))。監視は [`renderPipelineWatchSources()` の IA-3 の分](../../src/core/vkcore/renderer.cpp#L368)が flat の program の表と manifest の file を鍵に入れます。
- **frame plan**: [`Renderer::currentFramePlanJson()` の IA-1 の分](../../src/core/vkcore/renderer.cpp#L5007)が `physical_target_plan.resources[]` の `logical_resource` で引き、`initial_asset: {asset, sha256}` を足します。

**KTX2 の読み書き。**[`ktx2.cpp` の format の表](../../src/core/loader/ktx2.cpp#L71)が、format ごとに VkFormat の番号・名前・sRGB・圧縮・texel の bytes・`typeSize`・成分の数と bit 数・float かを持ち、[`parseKtx2()`](../../src/core/loader/ktx2.cpp#L220) の検査(`typeSize`、DFD の `bytesPlane0`、level の bytes の数と alignment)はこの表から引きます。level の alignment は非圧縮なら texel の bytes と 4 の最小公倍数、BC は 16 です。mip は 1 段か全段で、途中までは断ります。

書く方は [`writeKtx2()`](../../src/core/loader/ktx2.cpp#L393) で、非圧縮の format だけを、1 段・2D・supercompression なし・key/value なしで、Khronos の basic DFD(float は signed float の sample、sRGB は transfer function 2 で alpha は linear)を付けて書きます。名前と format の変換は `ktx2FormatName()` / `ktx2FormatFromName()`、texel の bytes は `ktx2TexelBytes()`(BC は 0)です([宣言](../../src/core/loader/ktx2.hpp#L73))。

`imageloader` は 1 / 2 成分の float の `ImagePixelFormat` を足し、KTX2 の読み込みで `LoadedImage::color_space_declared = true` にします([IA-1 で立てる所](../../src/core/loader/imageloader.cpp#L92))。CLI の `pelican_cli image ktx2` は `writeKtx2()` を呼ぶだけです(第7章)。test: `ia1_ktx2_cli_test` の [「IA-1 the KTX2 writer's single-level files load with their format, extent and texels」](../../test/ia1_ktx2_cli_test.cpp#L54)(8 つの format で 5×3 を書いて読み戻す)。

### ID バッファの同一フレーム token と同期 readback(WP262)

[`picking.vert`](../../src/core/resources/picking.vert) と
[`picking_skinned.vert`](../../src/core/resources/picking_skinned.vert) は、既存の indirect draw 契約で
`firstInstance` に入る model instance slot を `gl_BaseInstance + 1` として出力します。
`0` は clear 済み背景です。slot は再利用されるため、その整数を後の ECS 状態へ直接当てません。

[`Renderer::renderLogicalFrame()`](../../src/core/vkcore/renderer.cpp) は `picking_pass` を実行する
フレームの `PolygonInstanceContainer` 更新直後に、全 live slot の
`{index, generation, scene_epoch}` を snapshot します。フレーム送信が成功した場合だけ
`picking_id`、extent、frame index と一緒に publish します。次の feature-off フレームでは
snapshot vector も解放するため、feature を外した実行に継続コストを残しません。

[`Renderer::readPickingPixel()`](../../src/core/vkcore/renderer.cpp) は current graph に実在する
`picking_pass` / `picking_id` と publish 済みフレームが一致することを検査し、target を一時的に
`TRANSFER_SRC_OPTIMAL` へ遷移して 4 byte だけ host-visible staging buffer へコピーし、元の layout
へ戻します。同じ graphics queue の one-time command を同期 wait するので、呼び出し終了時には
指定ピクセルと同じ完了フレームの token が確定しています。低頻度のクリック／デバッグ API で
決定性を優先する v1 契約であり、連続 hover 向けの非同期 ring はまだありません。

RPC adapter の [`pickingReadbackJson()`](../../src/core/communication/rpcserver.cpp) は exact token を
`SceneRuntimeObjectBinding` の `SimpleModelViewComponent::model_instance_id` と照合し、authoring
document から WP258 の `(scene_id, declaration_index)` と `authoring_object_id` を返します。
照合できない transient model は runtime token を保持したまま authoring field を `null` にします。
GPU integer を declaration index として直接書かないのは、slot ごとの draw batching を保ちつつ、
generation と scene epoch で再利用の誤対応を防ぐためです。

### ギズモ幾何、表示要求、状態なし hit query(WP274)

ギズモの中心は [`gizmo.hpp`](../../src/core/renderer/gizmo.hpp) と
[`gizmo.cpp`](../../src/core/renderer/gizmo.cpp) です。`buildGizmoGeometry()` は mode、world pivot、
non-jittered view-projection、output extent、content scale だけを受ける pure geometry 関数で、
描画と `query_gizmo_handle` が同じ `GizmoSegment` 列を共有します。各操作可能 segment の
`GizmoDragProjection` は、値が増える画面単位ベクトルと論理 px 当たり変化量を保持します。
`hitTestGizmoDrag()` は最短線分の handle とこの drag 契約を返します。

handle は world X/Y/Z の `translate_*` / `rotate_*` / `scale_*` です。線の pipeline は
[`PipelineFactory`](../../src/core/shader/pipelinefactory.cpp) の `lineWidth = 1.0f` を変えず、hit 半径を
`10 logical px * content scale` として独立させています。content scale は framebuffer/logical
window extent の X/Y 比の平均を `0.5..4.0` に clamp し、window を持たない headless では 1 です。
これにより `wideLines` feature を要求せず、2x DPI では 20 物理 px の掴み代になります。
視線とほぼ平行で投影長が 8 論理 px 未満になる移動・拡縮軸は、pivot 上の菱形・矩形へ
フォールバックします。

描画可能な線分を全 9 handle が持つことを CPU テストで固定していますが、
投影方向を持たないフォールバック segment には drag 契約を付けず、RPC hit は `null` にします。
移動の変化量は pivot における軸別 pixel 微分から求めるため、同じ pointer 差分でも遠い target ほど
world 移動が大きくなります。回転は正回転方向へ生成した輪の各 segment 接線、拡縮は投影軸を使います。

[`resolveGizmoTargetTransform()`](../../src/core/renderer/gizmo.cpp) は外部から受けた
タグ付き `GizmoSelection` を分岐します。`kind:declaration` は `(scene_id, declaration_index)` を
authoring document の既存 `AuthoringObjectId`、`SceneRuntimeObjectBinding`、`TransformComponent` の順に
辿る従来経路です。`kind:runtime` は世代付き `GameObjectId` から authoring document と
`SceneLoader` を経由せず ECS の `TransformComponent` へ直接向かいます。どちらも parent を含む
runtime world transform の pivot を使います。

`Gizmo` module が mutex 下に保持するのは `set_gizmo` が設定した**表示要求**
`{selection, mode}` だけです。hover/pressed/active handle/drag delta は型にもありません。この要求に
serialize 経路はなく、実行をまたいで不安定な runtime ID を永続化しません。
`query_gizmo_handle` はリクエスト自身の selection/mode/coordinate から毎回 pure geometry を作り、
module の表示要求を参照も変更もしません。contract 2 は幾何計算に使った `content_scale` を明示し、
handle は `id` / `axis` / `drag_direction` / `value_per_logical_pixel` を公開するため、任意の client が
掴み半径から scale を逆算せず engine と同じ射影結果を使えます。

GPU 登録は picking/debug overlay と同じ feature lifetime に従います。
`parseRenderGraph()` が `type:gizmo` を IR の `AuxiliaryNodeInfo` に読み、[`passDefinitionOf()`](../../src/core/renderingpass/passdefinitionfromir.cpp#L80) が
`GizmoPassInfo` にし(✅RC-3c2b2。前は core の `gizmopassinfojsonparser.cpp`)、[`RenderingPassRuntimeCompiler`](../../src/core/renderingpass/renderingpassruntimecompiler.cpp)
が line-list pipeline を登録し、[`renderDynamicPassDrawCalls()`](../../src/core/vkcore/render_pass_dispatch.cpp)
が描画します。

[`RenderPipelineGpuArena`](../../src/core/renderingpass/renderpipelinegpuarena.cpp) の
checkpoint/rollback/retire に pass handle を参加させているため、graph 差し替え失敗でも leak せず、
feature を外した世代では pass/pipeline 登録が残りません。最初から feature-off なら `Gizmo`
module 自体を解決しないため、descriptor pool と upload buffer も作りません。

[`gizmo.json`](../../src/core/resources/features/gizmo.json) は中間 RT を必要とせず canonical `display` へ
直接 load+overlay する `gizmo_pass` だけを `before:debug_text` に挿入します。参照しなければ parser、
module 解決、GPU 登録、pass のすべてが起動せず、feature-private target も存在しません。

### logical frame の不変条件 {#logical-frame-invariants}

`renderLogicalFrame()` は次を破ると例外にします([`renderer.cpp` 内](../../src/core/vkcore/renderer.cpp#L1575)): 全 view が同じ in-flight frame index を共有すること、全 view の extent が等しいこと、target の color format がコンパイル済み graph と一致すること。FrameUBO slot は `in_flight × view_count + view` の式で選ばれます(WP128 レポート: [`docs/design_reviews/2026-07-17_wp128_report.md`](../design_reviews/2026-07-17_wp128_report.md))。

### [`plan` と `target_plan` の照合](../../src/core/vkcore/renderer.cpp#L1787) — 2 つのプランが並ぶ理由 {#reading-note-011}

`plan` はバックエンド非依存の論理 FramePlan で**常に存在し**、実行ノードの name/kind 一致検証に使います。`target_plan` は物理スコープ / 解像度計画を持つ**任意項目**で、null で**かつ** authored schedule も渡されていないときだけ [fallback schedule](../../src/core/vkcore/renderer.cpp#L1586) をその場で組み立てます(どちらか一方でもあれば渡された schedule で走ります)。[`fused_rendering_scope`](../../src/core/vkcore/renderer.cpp#L1841) は「native scope executor が無い」かつ「render パスで compiled pass が fused 指定」のときのみ true = プロバイダ所有スコープとエンジン所有の dynamic-rendering 融合スコープを**排他**にする条件です。プランの層構造そのものは[第6章「物理プランは『置換可能な完全パッケージ』」](06_rendering_vulkan_shader.md)が本体です。

### ["The provider owns all ordering"](../../src/core/vkcore/renderer.cpp#L2055) — 責任分界 {#reading-note-012}

native scope は 1 つのコールバックが物理スコープ全体を記録するので、source ノードが同一 native scope 内なら executor は barrier を出しません。エンジンが張るのは**スコープ境界をまたぐ依存だけ**です。前提として[スコープ区間の連続性検証](../../src/core/vkcore/renderer.cpp#L1858)が scope_node_index / scope_node_count / execution_index を全部照合し、プロバイダに渡す区間が「スケジュール上で連続した、ちょうどそのノード集合」であることを保証しています。

### [fused scope の内部依存](../../src/core/vkcore/renderer.cpp#L2121) — なぜ dynamic rendering で実行できないか {#reading-note-013}

fused scope は 1 つの dynamic rendering インスタンス内で全ノードを記録するため、内部依存を解く image layout 遷移バリアを**レンダリング中に発行できません**。例外は `passReadsBarrierAsLocalAttachment` / `isPhysicalScopeAttachment` が真のケース(同一スコープの attachment を local read で読む)で、これは dynamic rendering local read が保証するので barrier 不要です。判定は [fused scope 側の検証](../../src/core/vkcore/renderer.cpp#L1927)にあります。

### [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1575) — main family と他 family で schedule 構築が違う {#reading-note-014}

main family だけが view ごとに記録されます(sequential 実行・`logical_view_count` を受け取る)。他 family は view 非依存で single_view / view 数 1 として組まれるので、`view_index != 0` を入れると**同じノードを view 数だけ二重記録**します。スケジュールの並び自体は[第6章「スケジュールは scope-execution-major」](06_rendering_vulkan_shader.md)が本体です。

### [`transaction->generation == 0`](../../src/core/vkcore/renderer.cpp#L3747) — 番兵としての 0 {#reading-note-015}

[`after_publish` フック](../../src/core/vkcore/renderer.cpp#L3723)が未実行、つまりランタイム世代がまだ publish されていない状態を表す**番兵**です。その場合は [`stagedGeneration()`](../../src/core/vkcore/renderer.cpp#L3746) を代用世代として採用し、staged と一致するときだけ staged 扱い(commit ではなく保留)にします。同じ番兵は [`StagedRuntimeRollbackGuard`](../../src/core/vkcore/renderer.cpp#L3077) 側でも「staged 無し = ロールバック不要」として使われます。

### [`beginLogicalFrameTarget()`](../../src/core/vkcore/renderer.cpp#L6553) — 出力待ちで飛ばした frame は staged な世代を巻き戻さない {#reading-note-016}

記録 object は staged な世代を巻き戻す guard を持ち、frame が途中で失敗すれば巻き戻します。ただし窓の出力にまだ image が無くて frame を飛ばす場合(`OutputTemporarilyUnavailable`)は、GPU の作業も確定もしていないので **guard を外して残します**。ray tracing の起動世代は最初に提出した frame で初めて公開されるので、ここで巻き戻すと公開済みの世代が 1 つも無くなり、次の frame が `Renderer logical frame requires a compiled render pipeline` で落ちます。Studio に埋め込んだ player は shader cache が効くと窓の出力より先に最初の frame が来るので、この経路を実際に通ります(`7cb39996`)。出力の条件が変わった場合(`OutputRelowerRequired`)は今まで通り巻き戻して作り直します。公開前の世代は非公開のままで、`get_frame_plan` はその間「plan が登録されていない」と答えます(WP384 の契約)。

3 つの行き先(公開 / 次の frame へ持ち越し / 巻き戻し)は [`StagedRuntimeRollbackGuard`](../../src/core/vkcore/renderer.cpp#L3077) の上のコメントにまとめてあり、呼び手は `published()` と `keepForNextFrame()` で行き先を名指しします。どちらでもない終わり方は destructor が巻き戻します。この経路はふつう実窓でしか通らないので、GPU テスト「a frame skipped for want of an output image keeps the staged ray-tracing startup generation」は [`RenderTargetTestAccess`](../../test/rendertarget_testaccess.hpp#L14) で次の N frame を「出力に image が無い」にして、2 frame 飛ばしても staged の世代が残り、次の frame が同じ世代を公開することを確かめます。`keepForNextFrame()` を外すと赤になります。

### [`buildLogicalFrameViewFamilySchedule()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L120) — multiview と sequential の差 {#reading-note-017}

multiview は 1 回の記録で全 view を出すので schedule は view 非依存、sequential は view ごとに [`selectLogicalFrameSequentialViewSchedule()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L427) が別 schedule を選び、同じノードを view 数だけ実行します。cardinality が要るのは、**main family の view 数が物理 target plan の view 数と一致することを検証**し、二次 family(shadow / reflection)の物理スコープは単一 view のテンプレートなので**それを何回 sequential に展開するか**をスケジュール側で決める必要があるからです([コメント](../../src/core/renderingpass/viewexecutionscheduler.hpp#L258)「Runtime family cardinality expands that template sequentially」)。

### [`uploadMaterialAbsoluteOverrides()`](../../src/core/renderer/polygoninstancecontainer.cpp#L2188) — instance_identity が 1-indexed {#reading-note-022}

header は `maxModelInstances` 個の密配列で `headers[instance_identity - 1]` として索引します。**0 は「オーバーライド無し / 未設定」の sentinel** なので、`instance_identity == 0` は範囲外として例外になります。morph frame identity([`instance_identity = index + 1`](../../src/core/renderer/polygoninstancecontainer.cpp#L660))も同じ 1-indexed 規約です。

### [phase × sort_view_index の直積](../../src/core/renderer/polygoninstancecontainer.cpp#L1296) — variants を事前展開する目的 {#reading-note-023}

コメント "Compile phase/view variants without changing the live inventory" のとおりです。opaque と transparent は別 sort provider でソートされ、透過ソートは view の origin / forward に依存するので **sort view ごとに順序が変わります**。全組み合わせを事前コンパイルし、`CompiledDrawQueueSet::combine` が 1 本の indirect buffer に range リベースして平坦化します。ソート鍵の作り方は[第6章「draw queue は provider に鍵だけ作らせる」](06_rendering_vulkan_shader.md)が本体です。

### vk::DrawIndexedIndirectCommand の各フィールドの使用箇所 {#background-usage-01}

**このリポジトリでは**: `makeDrawItemSnapshot()`(polygoninstancecontainer.cpp:256)が 5 項目を名前付きの `DrawIndexedArguments`(`index_count` / `instance_count = 1` / `first_index = index_offset` / `vertex_offset = vert_offset` / `first_instance = instance.index`)に保存し(274〜281 行)、`drawqueuebuilder.cpp` の `materialize()`(215 行)が同じ順で集約初期化して `vk::DrawIndexedIndirectCommand{...}` に変換します(217〜223 行)。

model 登録(391 行)も hot-reload の rebuild(873 行)も同じ helper を通ります。名前付きの中間表を挟むことで 5 つの数値の並びを GPU 構造体の直書きまで持ち込まず、取り違えを防いでいます。とくに `first_instance = instance.index`(スロット番号がシェーダの `gl_BaseInstance` になる約束)を 1 か所に固定しているので、staging とリロードで別々に書いてずれることがありません。

## 6.5 Dynamic Rendering と pass 実行

[`RenderPassExecutor::execute()`](../../src/core/vkcore/render_pass_executor.cpp#L315) は次の順で動きます。

1. pass の入力 target を shader-read layout へ遷移する。
2. 出力 target を color/depth attachment layout へ遷移する。
3. attachment 情報を組み立てる。
4. `vkCmdBeginRendering` 相当の `beginRendering()` を呼ぶ。
5. viewport/scissor を動的設定する。
6. variant に応じた draw call を記録する。
7. `endRendering()` を呼ぶ。

つまり旧来の `VkRenderPass` / `VkFramebuffer` object を組み立てる方式ではなく、Vulkan Dynamic Rendering を使います。logical device 生成時にも [`vk::PhysicalDeviceDynamicRenderingFeatures` を有効化](../../src/core/vkcore/core.cpp#L787) しています。

UI pass だけは [`RenderPassExecutor` の特別分岐](../../src/core/vkcore/render_pass_executor.cpp#L330) で早期 return します。UI renderer 自身が rendering scope を管理するため、通常 pass と同じ `beginRendering()` を二重に呼ばないためです。同様に ImGui pass も [専用分岐](../../src/core/vkcore/render_pass_executor.cpp#L26) で処理されます。

> 🧩 **難所 — uint16 の天井が 16384**([`buildDrawBatch()`](../../src/core/ui/drawcommands.cpp#L10))
>
> **何をする所か**: UI pass が投げる quad コマンド列を塗り順に並べ、隣接する同一 `DrawKey` を 1 本の [`DrawRun`](../../src/core/ui/drawcommands.hpp#L54) にまとめ、頂点と 16 bit 索引を展開します。
>
> **素朴に読むと**: 並べ替えの基準は `(layer, decl_seq)` = **塗り順**であって key ではありません([`std::stable_sort()`](../../src/core/ui/drawcommands.cpp#L11))。run を切る条件は [この 1 行](../../src/core/ui/drawcommands.cpp#L32) の `runs.back().key != quad.key` **だけ**なので、ソート後でも同じ key の run が複数できます。「無駄だからまとめよう」と非隣接の同一 key を束ねると重なり順が入れ替わり、バッチ最適化のつもりで Z 順を壊します(`DrawKey::operator==` は pipeline / texture_page / sampler / clip_id しか見ないので、`texture` 文字列や scissor が違っても同一 key になりえます — [`drawcommands.hpp` 内](../../src/core/ui/drawcommands.hpp#L38))。もう 1 つが [`static_assert(maxQuads * 4 == 65536)`](../../src/core/ui/drawcommands.hpp#L29) です。索引は `std::uint16_t`、quad は 4 頂点なので、16384 quad が**ちょうど**表現限界(最後の `base` は 65532)。上限検査([`maxQuads` 超過の throw](../../src/core/ui/drawcommands.cpp#L14))が頂点構築より**前**に置いてあるのはこのためで、順序を入れ替えると [`base` を作る行](../../src/core/ui/drawcommands.cpp#L36) の `static_cast<std::uint16_t>` が黙って巻き、画面外の三角形やゴミが出ます。
>
> **骨子**:
> ```text
> stable_sort(layer, decl_seq)                  # 塗り順。key は見ない
> if (commands.size() > maxQuads) throw
> for quad in quads:
>   runs.back().key != quad.key なら run を追加  # 隣接のみ併合
>   base = uint16(vertices.size())              # 最大 16383*4 = 65532
> ```
>
> - **commands.size() の検査**: 必ず頂点構築より前。
>
> **手がかり**: 上限超過の例外が報告する widget 名は `commands.back().widget_id`、つまり**ソート後の末尾** = 最前面の widget であって、quad を増やした原因の widget とは限りません。スプライト側の [`maxQuadsPerChunk = 16384`](../../src/core/userpublic/sprite/spriteworld.hpp#L18) が同じ値で切り、超過を例外ではなく chunk 分割で処理するのも同じ 16 bit 索引の制約です([`buildQuadIndices()`](../../src/core/userpublic/sprite/spriteworld.cpp#L107)、[chunk 分割ループ](../../src/core/userpublic/sprite/spriteworld.cpp#L140))。テストは [`ui_foundation_test.cpp`](../../test/ui_foundation_test.cpp) の "Draw commands are stably sorted and only adjacent equal keys merge" と、A/B/A が 3 run に割れることを固定する U1 の正規テストです。
>
> **不変条件**: `maxQuads * 4` が uint16 の表現域を超えないこと(`maxQuads` を増やすなら索引を 32 bit にする)。run の併合は隣接のみ。上限検査は頂点構築より前。

### Material 描画のデータ経路

モデル描画は概ね次の所有分担です。

```mermaid
flowchart LR
    GLTF["glTF / ModelTemplate"]
    Vert["VertBufContainer<br/>shared vertex/index pools"]
    Material["MaterialContainer<br/>texture/pipeline/descriptor"]
    Instance["PolygonInstanceContainer<br/>transform/indirect commands"]
    Draw["MaterialRenderer<br/>drawIndexedIndirect"]

    GLTF --> Vert
    GLTF --> Material
    GLTF --> Instance
    Vert --> Draw
    Material --> Draw
    Instance --> Draw
```

[`MaterialRenderer`](../../src/core/renderer/materialrender.cpp#L537) は material ごとに pipeline と descriptor を bind し、[`drawIndexedIndirect`](../../src/core/renderer/materialrender.cpp#L407) を発行します。GPU へ渡す geometry、material、instance transform を別 container に分けているため、「モデル1個 = Vulkan buffer 一式」にはなっていません。共通 vertex/index pool と material 単位の draw range を使う構成です。

**面の表は view ごとに決まります(✅CC-1、`c8367b87`)。** material・velocity・shadow depth の pipeline は `GraphicsPipelineDesc::dynamic_front_face = true` で作られ(既定の値は反時計回り)、front face を dynamic state(`VK_DYNAMIC_STATE_FRONT_FACE`。Vulkan 1.3 の core)で持ちます([material の desc](../../src/core/material/materialcontainer.cpp#L418))。

renderer は view ごとに [`viewIsMirrored()`](../../src/core/renderer/clipspace.hpp#L110) で鏡像かを判定して `MaterialRendererDependencies::mirrored_view` に入れ([main の view](../../src/core/vkcore/renderer.cpp#L2562) と secondary family の各 view)、`setViewFrontFace()` が pipeline を bind した直後に `setFrontFace` を記録します(鏡像なら時計回り、でなければ反時計回り。[記録する所](../../src/core/renderer/materialrender.cpp#L34))。bind の直後なのは、間に dynamic state を持たない pipeline(fullscreen や UI)が bind されるとその値が未定義になるからです(コードの注記)。

鏡像になるのは cube の面への描画(cube capture と点光源の影)だけで、[cube の view](../../src/core/render_algorithms/cube_capture/cubecaptureview.cpp#L83) は共通の射影を通さず、Vulkan の cube の面の規約の行列のまま描きます。pipeline を 2 種類持つ案ではなく dynamic state を採ったので、pipeline の数は増えていません。CC-1 より前はこの 3 つの pipeline が `vk::FrontFace::eClockwise` 固定でした(画面が world の鏡像だったことの辻褄合わせ。[第9章 §9.8](09_black_magic_and_gotchas.md))。`raster` pass は作者の `front_face` を pipeline に固定で入れ、切り替えません。

✅CC-2(`1501685a`)の `clip_y: "up"` も pipeline には触らず、`renderFullscreenPass()` が描く直前に [`setClipYViewport()`](../../src/core/vkcore/render_pass_dispatch.cpp#L45) で負の height の viewport(`{0, height, width, -height}`)を設定し、描いた後に正の viewport へ戻すだけです(fused の rendering scope は viewport を scope に 1 回しか設定しないため、戻さないと後の pass に漏れる。コードの注記)。Vulkan は viewport を通した後の framebuffer の座標で面を判定するので、`front_face` / `cull` は `clip_y` がどちらでも出来上がった画像の上で見た巻き方を指します。`clip_y` は `RasterPassContract` の pass 直下の field で(`raster_state` は fullscreen と共有するので入れない)、fingerprint にも入ります([`RasterClipY`](../../src/project/rasterpass.hpp#L55))。

skinning palette(スキニング行列パレット — ボーンごとの変換行列を 1 本の配列に並べたもの。頂点側は行列そのものではなく配列の添字と重みだけを持ち、シェーダで合成します)、morph weight、per-instance material override の GPU バッファも [`PolygonInstanceContainer`](../../src/core/renderer/polygoninstancecontainer.hpp#L382) が所有します。skin palette / morph weight には前フレーム分(previous バッファ)があり TAA velocity の入力になります。material override は per-frame history を持ちます(WP122/122b。golden: material_instance_override / material_absolute_override)。

**頂点の mode は部品が決め、material は 1 つのまま(✅SK-1、`e265e5658`)。** 「skin 付きか」の真実が material と部品の 2 か所にあり、skin 付きの部品に project material を当てると skin 用の頂点 buffer を skin 無しの layout で読んでいました。SK-1 はそれを部品の側だけに寄せました。

- **型**: [`VertexMode`](../../src/core/material/materialcontainer.hpp#L40) は `standard` / `skinned` の 2 値で、`vertexMode(bool)` が部品の `skinned` から引きます(同じヘッダの 2 行後。間の行は 2 つの mode を回す配列 `vertexModes`)。`MaterialInfo` の bool の `skinned` は無くなり、skin 用の頂点 shader を [`MaterialInfo::skinned_vert_shader`](../../src/core/material/material.hpp#L41)(`std::optional<ShaderBundleId>`)で持ちます。値の無い material は skinned の mode を持ちません(VAT の派生だけがそう。[`registerVatMaterial()`](../../src/core/model/gltf.cpp#L866) が `reset()` する)。
- **pipeline は mode ごとに、登録のときに全部作る**: [`MaterialContainer::registerMaterial()`](../../src/core/material/materialcontainer.cpp#L777) は [`vertexModes` を回して](../../src/core/material/materialcontainer.cpp#L901)、`skinned_vert_shader` があれば skinned の pipeline も作り、`InternalMaterialInfo::pipeline`([`std::array<std::optional<PipelineHandle>, 2>` の宣言](../../src/core/material/materialcontainer.hpp#L147))に入れます。描くときに遅れて作ることはしません。pipeline の鍵([`makePipelineKey()`](../../src/core/material/materialcontainer.cpp#L101))と desc([`makeMaterialPipelineDesc()`](../../src/core/material/materialcontainer.cpp#L398))は mode を受け、skinned は `use_skinned_vertex_layout`(JOINTS_0 / WEIGHTS_0 付きの専用の頂点 buffer。[シェーダ契約](../shader_contract.md) の Vertex input)で作ります。
- **mode が変えるのは頂点 shader と頂点 layout だけ**: descriptor set・値の SSBO・GPU の material index は 1 組です。2 つの pipeline の set 2(material)と set 1(pass input)の descriptor set layout が反映で一致しなければ、登録を [`incompatible standard/skinned descriptor set` で断ります](../../src/core/material/materialcontainer.cpp#L939)。共有の descriptor を作る側(`stageTextureRebind` などの読み手)は standard の pipeline を、描く側(`bindMaterialDraw()` / `materialPipeline()` / `pipelineLayout()` / `drawOverridePipelineDesc()`)は渡された mode の pipeline を使います(SK-1 の実装者の報告の一覧。`bindMaterialDraw()` などの宣言は [`MaterialContainer` の mode を受ける関数群](../../src/core/material/materialcontainer.hpp#L513))。
- **頂点 shader の出どころ**: project material は [`makeMaterialInfo()`](../../src/core/material/projectmaterialasset.cpp#L130) が同じ surface を 2 回 compile し、2 回目に `PELICAN_SKINNED` を足します([足す行](../../src/core/material/projectmaterialasset.cpp#L145))。glTF の material は組み込みの `standardVertShader()` / `skinnedVertShader()` を持ち([`loadMaterial()` の中](../../src/core/model/gltf.cpp#L2077))、surface が `vertex_displace_v1` の hook を持つときだけ両方の mode を surface から compile します([その分岐](../../src/core/model/gltf.cpp#L2107))。組み込みの material も `skinnedVertShader()` を持ちます([`StandardMaterialResource::builtinMaterialInfo()`](../../src/core/material/standardmaterialresource.cpp#L213))。`PELICAN_SKINNED` は compiler の持ち物で、material 文書の `defines` に書くと [`isCompilerOwnedSurfaceDefine()`](../../src/core/shader/shaderlibrary.cpp#L167) に当たって断られます。
- **消えたもの**: glTF の loader が skin 付きの部品のために作っていた material の双子(`skinnedMaterial()` と `skinned_material_variants`。頂点 shader だけを差し替えた写しを負の ID で登録していた)と、組み込みの `pelican.builtin.skinned_fallback`(material の無い skinned primitive 用)です。skin 付きの部品も元の material の ID をそのまま使うので、`ModelTemplate::MaterialPrimitives` の 1 つの group に skin 付きと skin 無しの部品が同居します(部品ごとに `skinned` を持つので描画は困りません)。負の ID の派生は VAT だけに残ります([第3章 §3.8](03_project_and_loading.md#38-asset-modelから描画instanceまで) の難所「-1を空けて負へ伸ばす」)。
- **draw item を作るときに mode を確かめる**: [`makeDrawItemSnapshot()`](../../src/core/renderer/polygoninstancecontainer.cpp#L370) は、実効の material が部品の mode の pipeline を持たなければ material・mesh・primitive を名指しで throw します([検査の行](../../src/core/renderer/polygoninstancecontainer.cpp#L382))。描画の記録の途中では投げません。全部の非 VAT の material が skinned を持つので、ここに届くのは skinned を持たない VAT の material を skin 付きの部品が使う場合だけです(コードの読み)。
- **VAT の部品の material は差し替えられない**: VAT は texture と bounds を material に焼いているので、VAT の部品(`vat_deformed`)に VAT でない material を当てることを 3 経路で断ります。model の bindings は [`applyPrimitiveMaterialBindings()` の VAT の検査](../../src/core/model/modeltemplate.cpp#L160)で例外、scene の object の上書きは [`ResolvedInstanceMaterials::effective()` の VAT の検査](../../src/core/renderer/polygoninstancecontainer.cpp#L456)で warning を出して model の material に戻し(この 2 つは当てる material が VAT を持つかを見る)、RPC の `bind_material` は当てる material を見ずに、VAT の部品を指す指定をすべて [`material.binding_invalid@1` で断ります](../../src/core/communication/materialdocumentrpc.cpp#L1754)。文はどれも `VAT primitives keep their baked material` です。VAT を部品の側へ移すのは v1 です。
- **描くときの mode**: draw item の `DrawPipelineMaterialKey::skinned` は部品の `skinned` のままで、`renderMaterialDraws()` の `draw_step` が [`bindMaterialDraw(..., vertexMode(draw_call.skinned))`](../../src/core/renderer/materialrender.cpp#L305) で mode を渡します。draw queue の range は前から material・`source_material_index`・`skinned` の変化で閉じる(下の難所「draw queue は provider に鍵だけ作らせる」)ので、1 つの range の中で mode は変わりません。
- test: `headless_render_test` の「SK1 skinned material bindings depth coverage and reload」(GPU。姿勢を付けた skin の depth の覆い・3 つの割り当ての経路・VAT の 3 経路・reload の前後の `VkPipeline`)、`materialbinding_test` の「SK1 material bindings retain primitive skin mode and refuse unbaked VAT」、`materiallowering_test` の「SK1 draw-loop subjects distinguish geometry mode for one material」。

**受け皿の無い material はマゼンタで描く(✅MU-1、`4b4fbfda0`)。** material の route・shader contract・`exact_pass` を受ける material pass が今の構成に無いとき、[`registerMaterial()` の MU-1 の分岐](../../src/core/material/materialcontainer.cpp#L805)は登録を断らず、代役の pass で描きます。

- **代役の選び方**: [`RenderingPassContainer::substituteMaterialPass()`](../../src/core/renderingpass/renderingpasscontainer.cpp#L208) が、同じ shader contract を受ける material pass を frame の順で探し、無ければ最初の material pass を返します。material pass が 1 つも無ければ代役は無く、material は受け皿の無いまま登録されます(pass がゼロの構成は通常の世代として起動する UD-1 の契約のまま)。
- **描き方**: fragment は [`StandardMaterialResource::errorFragmentShader()`](../../src/core/material/standardmaterialresource.cpp#L26) が作る `generated://material_route_error.surface`(base color と emissive が `(1,0,1)`、lighting の hook も一定のマゼンタ)で、代役の pass の output schema で compile し、route・schema・頂点の ABI ごとに module の寿命の cache に置きます([差し替える行](../../src/core/material/materialcontainer.cpp#L823))。頂点 shader は書いた material のもの(standard / skinned の両方)のままで、blend を切り、depth test / write を入れ、fragment だけが使う screen input と resource port を外します。
- **記録**: `InternalMaterialInfo::unserved`([`UnservedMaterial` の宣言](../../src/core/material/materialcontainer.hpp#L121))に書いた route・contract・`exact_pass` と代役の route・pass・理由の文を残し、draw queue と draw loop の subject は代役の route を使います。`list_materials` / `get_material` は書いた route と `unserved` を返し([`MaterialContainer::materialRouteStatus()`](../../src/core/material/materialcontainer.cpp#L1961))、[`MaterialContainer::publishUnservedProblems()`](../../src/core/material/materialcontainer.cpp#L1977) が Problems の domain `material_routes` に `material.route_unserved@1`(severity error)を material ごとに 1 行出します。登録・variant の登録・解放のたびに出し直します。受ける pass を足しただけでは、登録済みの material は代役のままです(世代の planner の [`candidateRoute()`](../../src/core/material/materialgenerationplanner.cpp#L756) は、authored の route default が変わらない限り代役の route を使い続ける)。material が登録し直されたとき(material の編集、engine の起動し直し)に通常の pass へ載り、行が消えます。
- test: `materialdocumentrpc_test` の「MU-1 materialdocumentrpc unserved materials stay live and draw magenta until served」(GPU。Blank 相当の構成に blend の material を当ててマゼンタの画素・Problems の 1 行・両 mode の pipeline、`forward_transparent` の pass をディスクに足して engine を起動し直すと青に戻る)、`renderingpass_helpers_test` の「MU-1 material substitute prefers a compatible contract then frame order and preserves no-pass」。

**morph の頂点の記録が差し替わったら descriptor を書き直します(✅MV-1、`f84a7d14`)。** morph の頂点ごとの記録(metadata)と delta の buffer は `VertBufContainer` が持ち、`PolygonInstanceContainer` の 4 つの descriptor set(static / skinned × 変形用 / material 用。set 3 の `PELICAN_SET_FREE`)がそれを bind します。

MV-1 から metadata は頂点 buffer と一緒に伸びて buffer ごと差し替わる([第3章](03_project_and_loading.md) の「morph の頂点の記録は共有の頂点 buffer と同じ件数です」)ので、[`triggerUpdate(frame_plan)` の頭で呼ぶ所](../../src/core/renderer/polygoninstancecontainer.cpp#L1178)から [`refreshDeformationDescriptors()`](../../src/core/renderer/polygoninstancecontainer.cpp#L281) が `VertBufContainer::morphMetadataGeneration()` を見て、set を書いた時の世代から動いていれば新しい pool に 4 つの set を作り直します(constructor も同じ関数で最初の set を作る)。

古い pool と set は、それを読みうる frame のために [`DeletionQueue` へ渡します](../../src/core/renderer/polygoninstancecontainer.cpp#L357)(古い metadata buffer も `deferOldBuffer()` で同じように退役)。ray の変形(`raydeformation.cpp`)は変形のたびに descriptor を作るので変わっていません。shader の側は `PELICAN_MAX_MORPH_VERTICES` を撤去し、範囲の検査を [bind された buffer の長さ](../../src/core/resources/shaders/include/pelican_morph.glsl#L66)(`pelicanMorphMetadata.vertices.length()`)で行います。

残る穴(実装者の報告): 書き直すのは `triggerUpdate(frame_plan)` だけで、set を bind する `materialrender.cpp` の 3 か所(`bindMaterialInstanceResources()` 1 つと `bindDeformation()` 2 つ)はその時に持っている set を使います。`triggerUpdate(frame_plan)` の後、同じ frame の描画の前に頂点 buffer が伸びる経路があるかは未確認です。

**object ごとの material は draw item に入ります(✅MT-2、`44b7b50a`)。**上の material override(WP122 の GPU バッファ)とは別に、MT-2 では scene の object が「どの material で描くか」そのものを替えます。持ち方と解き方は次のとおりです。

- **持ち物**: instance を stage する関数(`stageModelInstance()` / `stagePreparedSceneModel()` / `placeModelInstance()`)は、省略できる引数 `std::shared_ptr<const InstanceMaterialOverrides>`(material の名前の列。null は上書き無し)を受けます。slot の seed を経て slot table の [`ModelInstanceSlotTable::material_override_specs` の MT-2 の列](../../src/core/renderer/polygoninstancecontainer.hpp#L337) に、instance ごとに名前のまま残します(`arrayCount` は 17 から 18)。
- **解き方**: [`resolveInstanceMaterials()`](../../src/core/renderer/polygoninstancecontainer.cpp#L480) が、名前を model の `named_materials`、次に `ProjectMaterialAssetContainer::namedMaterials()` で `GlobalMaterialId` に解きます(`findNamedMaterial()`)。解けない名前は警告を出して飛ばします。[`ResolvedInstanceMaterials::effective()` の効く順 MT-2](../../src/core/renderer/polygoninstancecontainer.cpp#L451) は「その primitive の行 > 全 primitive の行 > model の material(binding、無ければ glTF)」です。
- **draw item**: [`appendModelDrawItems()`](../../src/core/renderer/polygoninstancecontainer.cpp#L514) が primitive ごとに実効の material を決め、`makeDrawItemSnapshot()` に渡します。`pipeline_material_key.material`、`material_document_reference`、`material_name`、`route` / `phase`(`MaterialContainer::routeForMaterial()`)、ray の material の世代と lease が、全部この実効の material から作られます。draw queue の range は material の変化で閉じる(下の難所)ので、同じ model で material の違う instance は別の range で描かれ、MP-1 の `drawn_materials` も実効の material で数えます。
- **material だけを替える**: [`PolygonInstanceContainer::setPreparedSceneModelMaterials()`](../../src/core/renderer/polygoninstancecontainer.cpp#L1037) は、生きている instance の draw item を候補の inventory の中で作り直し、各 item の `declaration_ordinal` を元のまま保ちます(同点の鍵の並びが動かないように。コードの注記)。slot と animation の状態は残ります。editor の投影だけがこれを使います(第3章 §3.11)。
- **model の reload**: [`prepareModelInstanceRebuild()` の MT-2 の解き直し](../../src/core/renderer/polygoninstancecontainer.cpp#L1498) は、残しておいた名前の列を新しい template に対して解き直します。この経路の test はありません(未確認)。
- **読み戻し**: [`PolygonInstanceContainer::instancePrimitiveMaterials()`](../../src/core/renderer/polygoninstancecontainer.cpp#L1071) は、生きている instance の draw item から (mesh, primitive) ごとに material と名前を 1 つずつ返します(複数の node が描く mesh も 1 件)。`get_object_materials` の `drawn` です。

> 🧩 **難所 — 量子化するのはアンカーだけ**([`classifyStrictSprite()`](../../src/core/userpublic/sprite/pixelpolicy.cpp#L98) / [`quantizePixelBoundary()`](../../src/core/userpublic/sprite/pixelpolicy.cpp#L153))
>
> **何をする所か**: material 経路の隣にある 2D スプライト経路の要です。pixel-perfect(strict)の成立条件を CPU で判定し、成立したものだけ頂点シェーダで「アンカー 1 点を framebuffer のピクセル境界へスナップ」します。
>
> **素朴に読むと**: 見た目が単純なのに理由が深い所が 3 つ重なっています。(1) `quantizePixelBoundary()` は 1 行 `std::floor(x + 0.5)` です。`std::round()` は half away from zero(`-0.5 → -1`、`0.5 → 1`)なので、原点をまたぐスプライトで丸め方向が反転します。`floor(x+0.5)` なら画面のどこでも同じ規則になり、カメラを動かしてもスプライトが 1 px 揺れません。(2) スナップするのは**アンカー 1 点だけ**で、得られた差分を `clip.xy += deltaNdc * clip.w` として 4 頂点に同じ量だけ足します(§6.14 のジッタと同じ「w 倍で足すと深度非依存の定数 NDC シフトになる」。**NDC** は normalized device coordinates の略で、透視除算のあとの画面座標系のことです。Vulkan では x/y が -1〜+1、z が 0〜1 に収まります)。頂点ごとに独立してスナップするとクアッドが変形してテクセル比が整数でなくなり、にじみます。(3) `classifyStrictSprite()` の basis 判定は**同じ 1 つの `if` の中で `nearlyZero` の極性が混在**します — 前 2 項は「軸が潰れていないこと」、後 4 項は「軸が漏れていないこと」で、どちらも `rotated_or_tilted` にまとめられます。
>
> **骨子**:
> ```text
> GPU(sprite.vert):
>   anchorFB  = (anchorNDC*0.5 + 0.5) * resolution.xy
>   snappedFB = floor(anchorFB + 0.5)
>   deltaNDC  = (snappedFB - anchorFB) * 2 * resolution.zw   # zw = 1/w, 1/h
>   clip.xy  += deltaNDC * clip.w                            # 4 頂点に同じ量
> ```
>
> **手がかり**: `resolution` は `(w, h, 1/w, 1/h)` で、`.zw` が逆数だと知らないと `deltaNdc` の式が読めません。アンカーは `gpu.snap_anchor = {-pivot.x, pivot.y}`、つまり画像の左上の角(画像の空間の (0,0))を、pivot を原点・+y を上にした sprite のローカルで表した点であって、中心でも pivot でもありません(✅CC-1 `c8367b87` で quad の y を反転し、画像の上の行が local +y 側に出るようになったので、y の符号が `-pivot.y` から `pivot.y` に変わった。[quad を作る所](../../src/core/renderer/spriterenderer.cpp#L83))。GPU に渡るのは `pixel_snap` の 0/1 だけで、判定ロジックは全部 CPU 側です。FrameUBO の `projection` はジッタ済みなので、TAA 有効時のスナップはジッタ込みの NDC で行われます。テストは [`sprite_foundation_test.cpp`](../../test/sprite_foundation_test.cpp)(`quantizePixelBoundary(3.49)==3` / `(3.50)==4` が丸め境界を固定)。
>
> **不変条件**: 丸めは `floor(x+0.5)`(`std::round` に替えない)。スナップ量はスプライト内で一定に保つ。条件を緩めるときは対応する `PixelSnapReason` を残したまま緩めること — 黙って eligible にしないのがこの API の設計です。

> 🧩 **難所 — firstInstance は索引**([`stageModelInstance()` の `DrawIndexedIndirectCommand`](../../src/core/renderer/polygoninstancecontainer.cpp#L409))
>
> **何をする所か**: モデル 1 個を staging するとき、primitive ごとに indirect 描画コマンド(GPU が読むバッファへ描画引数を並べておき、CPU からは「このバッファのここから N 個」とだけ指示する方式)を 1 つずつ積む所です。
>
> **素朴に読むと**: `instanceCount` は常に `1` で、`firstInstance` には `staged->id.index` が入ります([snapshot の組み立て](../../src/core/renderer/polygoninstancecontainer.cpp#L1738))。Vulkan の意味での firstInstance は「インスタンス番号の開始値」ですが、ここは 1 個しか描かないので開始値としては何の意味もありません。実際にはこのフィールドは、**GPU 側 SSBO**(shader storage buffer object — シェーダから添字で自由に読める大きなバッファ)**の行番号**を渡す唯一の無料チャネルとして使われています。頂点シェーダは `gl_BaseInstance` から model 行列([`default.vert` 内](../../src/core/resources/default.vert#L29))、material instance の行([`pelican_material_instance.glsl` 内](../../src/core/resources/shaders/include/pelican_material_instance.glsl#L63))、skin palette の基底(`gl_BaseInstance * PELICAN_MAX_SKIN_JOINTS`、[`pelican_skinning.glsl` 内](../../src/core/resources/shaders/include/pelican_skinning.glsl#L17))、morph weight の instance([`pelican_morph.glsl` 内](../../src/core/resources/shaders/include/pelican_morph.glsl#L64))を一斉に引きます。つまり `ModelInstanceId.index` は CPU 側のスロット番号であると同時に、これら複数バッファの行番号でもあります。この一致が不変条件で、instance を詰め直す(compaction する)なら全バッファを同じ順で並べ替えなければならず、片方だけ動かすとメッシュは正しいのに別インスタンスの姿勢で描かれます。
>
> **骨子**:
> ```mermaid
> flowchart TB
>     CPU["CPU: cmd.instanceCount = 1; cmd.firstInstance = id.index"]
>     GPU["GPU: gl_BaseInstance = i"]
>     Model["objects[i].model"]
>     Material["material instance[i]"]
>     Skin["skinPalette[i * 128 + joint]"]
>     Morph["morphInstances[i]"]
>     CPU --> GPU
>     GPU --> Model
>     GPU --> Material
>     GPU --> Skin
>     GPU --> Morph
> ```
>
> **手がかり**: push constant は engine 用の先頭 64 byte と material index で埋まっており(§6.9)、instance ごとの値を載せる余地がありません。material グループごとに 1 回の [`drawIndexedIndirect`](../../src/core/renderer/materialrender.cpp) で回す以上、「今どのインスタンスか」を伝える経路が firstInstance しか残っていない、というのがこの使い方の理由です。CPU 側の live inventory は [`DrawItemSnapshot`](../../src/core/renderer/drawqueuebuilder.hpp) の stable `ModelInstanceId` で instance を同定し、同じ index を `DrawIndexedArguments::first_instance` に保存します。バッファ書き込みの行番号も同じ index です。
>
> **不変条件**: `ModelInstanceId.index` = model / previous-model / skin palette / morph weight / material override 各バッファの行番号。スロットは retire して再利用しますが、生きている instance を詰め直しません([`modelinstance_slotmap_test.cpp`](../../test/modelinstance_slotmap_test.cpp))。indirect command の `firstInstance` を 0 以外にするため `drawIndirectFirstInstance`、shader で `gl_BaseInstance` を読むため `shaderDrawParameters` がそれぞれ必要で、device 選択時に両方を検査し logical device 生成時に明示的に有効化します。

> 🧩 **難所 — draw queue は provider に鍵だけ作らせる**([`DrawQueueBuilder::build()`](../../src/core/renderer/drawqueuebuilder.cpp#L461) / [`stateBatchedSort()`](../../src/core/renderer/renderpolicyregistry.cpp#L93) / [`CompiledDrawQueueSet::combine()`](../../src/core/renderer/drawqueuebuilder.cpp#L743))
>
> **何をする所か**: `PolygonInstanceContainer` は全 primitive の declaration-order inventory を `DrawItemSnapshot` として保持します。`triggerUpdate()` は pure CPU の `DrawQueueBuilder` を呼び、並べ替えた indirect record と view/フィルタごとの描画区間を immutable な `CompiledDrawQueue` として一度だけ発行します。ビルダは「モジュール・device・material container・render graph state を触らない」と明記されています([`drawqueuebuilder.hpp` 内](../../src/core/renderer/drawqueuebuilder.hpp#L249))。
>
> **素朴に読むと**: 並べ替えの主体が **ビルダの外**へ出ました。`state_batched_v1` はビルダ内の固定 policy ではなく **provider レジストリの名前**です([`renderpolicyregistry.hpp` 内](../../src/core/renderer/renderpolicyregistry.hpp#L16))。しかも provider(C ABI、[`draw_sort_abi_v1.hpp`](../../src/core/userpublic/render/draw_sort_abi_v1.hpp))が返すのは **`(primary, secondary)` の 64bit×2 の鍵だけ**で、実際の `std::sort` と tie-break はビルダが持ちます([`std::sort()`](../../src/core/renderer/drawqueuebuilder.cpp#L566))。したがって「provider が順序を決める」は半分間違いで、**同点時の最終順序を決めるのはビルダの [`stableOrderKey()`](../../src/core/renderer/drawqueuebuilder.cpp#L215)**(scene epoch / instance index / generation / mesh / primitive / node / declaration ordinal)です。ここを provider 側へ寄せると決定性が失われます。組み込み provider が持っている互換順は鍵の中に畳み込まれていて、`primary = (material << 32) | source_material_index`、`secondary = (skinned << 32) | visibility`、`visibility` は third(0) / both(1) / first(2) です([`renderpolicyregistry.cpp` 内](../../src/core/renderer/renderpolicyregistry.cpp#L121))。この順なら三人称ビューは末尾の first だけを、一人称ビューは先頭の third だけを外すので、material group ごとの可視 item が連続します。さらに旧「三人称 / 一人称」の 2 レンジは、いまや **phase(opaque / transparent)× sort view × visibility × material filter** の 4 軸に増え、`CompiledDrawQueueSet::combine()` が複数の queue を **1 本の indirect バッファへ平坦化してから offset を rebase** します(コメントが規範: 「The GPU still sees one indirect buffer; CPU consumers select a phase/view range whose offsets have already been rebased into that flattened storage.」)。
>
> **骨子**:
> ```text
> build(request, provider):
>   selected = items のうち target_phase に合うもの
>   provider.sort(input, keys)                          # 鍵だけ。順序ではない
>   sort(order, [primary, secondary, stableOrderKey])
>   indirect_records_ = materialize
>   build_ranges(view, filter):
>     不可視 / フィルタ不一致 / material・source_material_index・skinned の変化で range を閉じる
>     splitIndirectDrawRange(maxDrawIndirectCount) で device 上限へ分割
> combine(variants): view-major、opaque/transparent の順に平坦化して offset を rebase
>                    + SceneDrawSegmentV1 を発行(GPU 書き込み draw 用)
> ```
>
> - **sort の鍵**: 決定性は 3 段目の stableOrderKey。
>
> **手がかり**: 区間を切る state 条件は material / source_material_index / skinned の変化と、可視性・material filter です([range を閉じる走査](../../src/core/renderer/drawqueuebuilder.cpp#L613))。穴のある順序を許す policy を足すなら、不可視位置で range を閉じるぶん同じ material の draw call 数が増えることを受け入れることになります。secondary view family は `locally_sorted_families` に名前があれば per-view の queue を別に compile し、それ以外の family は main の canonical 順を保ったまま culling 落ちを **zero-instance draw** に変えます([`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L665))。CPU-only fixture は [`drawqueuebuilder_test.cpp`](../../test/drawqueuebuilder_test.cpp)、VRM 一人称の実例は [`morph_gltf_test.cpp`](../../test/morph_gltf_test.cpp) にあります。
>
> **不変条件**: 鍵が同点なら `stableOrderKey()` 順(provider を替えても再現性が変わらない)。組み込み `state_batched_v1` の visibility 順 third(0) → both(1) → first(2) を変えないこと — 変えると「三人称は末尾を落とすだけ / 一人称は先頭を落とすだけ」で範囲が連続する、という性質が壊れます。新しい view / sort 規則は別の versioned policy として追加します。

### material ごとの描き方の列(LP-2 / LP-3a)と view の選択(LP-Va)

**描き方の列。**LP-2 から、material pass は range(1 つの material の連続した draw)ごとに**描き方の列**を回せます。表は世代の持ち物で、[`RendererRuntimeGeneration`](../../src/core/renderingpass/framegraphruntime.hpp#L168) の `material_draw_loops`([`MaterialDrawLoopTable`](../../src/core/material/materialdrawlooptable.hpp#L72))が (pass, base material, 頂点の mode) → 列を持ちます(mode は ✅SK-1 から。[`MaterialDrawLoopTable::find()`](../../src/core/material/materialdrawlooptable.cpp#L147) は `(material, mode)` で引く)。

候補の世代を準備するときに、合成結果の `material_draw_loops`(graph の draw_loop から作った規則)を全 material について評価して作り、候補が拒否されたら表ごと捨てます。世代が生きている間に登録された material(glTF、spawn、variant)は、登録の hook が生きている全部の表で評価します。[`renderMaterialDraws()`](../../src/core/renderer/materialrender.cpp#L82) は列を順に回し、「最後に bind した実効の pipeline と descriptor set」を覚えて bind を省きます(base の material id ではない。同じ range を 2 回描くため)。

override の pipeline は base の pipeline の記述を複写して render state と define だけを替えたもので、表が所有し、material の pipeline cache とは共有しません。descriptor set と GPU の material index は base のものを使うので、値の編集が 2 回目の描画にも効きます。define の差し替えは shader の compile が要り、1 世代 16 組までです。

**規則に問う主語は material × 頂点の mode(✅SK-1)。**規則が見る [`MaterialDrawSubject`](../../src/project/materialdrawloop.hpp#L129) は、1 つの material につき mode ごとに 1 つ作られます。

- **列挙**: [`MaterialContainer::drawLoopSubjects()`](../../src/core/material/materialcontainer.cpp#L2626) は material ごとに、[`supportsVertexMode()`](../../src/core/material/materialcontainer.cpp#L2622)(その mode の pipeline があるか)が真の mode だけ [`drawLoopSubject(material, mode)`](../../src/core/material/materialcontainer.cpp#L2598) を作ります。表の [`MaterialDrawLoopTable::evaluateInto()`](../../src/core/material/materialdrawlooptable.cpp#L220)、世代の planner の受け皿の検査([`vertexModes` を回す所](../../src/core/material/materialgenerationplanner.cpp#L859))も同じく mode ごとに回ります。step の override の pipeline は、その mode の base の pipeline から派生させます([派生元を引く行](../../src/core/material/materialdrawlooptable.cpp#L267))。
- **2 つの鍵**: `subject.key` は mode 付きの内部の鍵で、[`materialDrawSubjectKey()`](../../src/project/materialdrawloop.cpp#L18) の末尾に `#vertex/standard` / `#vertex/skinned` が付きます。`subject.material_key` は mode の無い公開の id(project material は `<文書>#<名前>`、glTF の material は `<asset>#material[<index>]`)で、`materialKey()` が返します。`when.material` は公開の id で照合するので、1 つの id が standard / skinned の両方の主語に当たります([`graphLoopDrawConditionMatches()`](../../src/project/materialdrawloop.cpp#L106) は `subject.key` と `subject.material_key` のどちらかに一致すれば当てるので、mode 付きの内部の鍵を書いた `when.material` はその mode の主語にだけ当たる)。`when.skinned` は主語ごとの `skinned` を見ます。拒否の文の `material` にも内部の鍵は出しません。
- **公開の形**: `get_frame_plan` の `loops[].elements` は (公開の id, branch) ごとに 1 行で、その branch を取る mode を `vertex_modes` に並べます([`applyMaterialDrawLoopElementsToFramePlanJson()`](../../src/project/materialdrawloop.cpp#L194))。両 mode が同じ branch を取れば 1 行、`when.skinned` で分かれれば同じ id の 2 行です。`get_render_authoring_context.materials` も公開の id ごとに 1 行で `vertex_modes` を持ちます([集める所](../../src/core/communication/renderconfigeditor.cpp#L3746))。Studio の Frame Plan は Material の行の下に読み取り専用の「Vertex modes」を出します。
- **描くとき**: `renderMaterialDraws()` の range は material と `skinned` の両方が同じ間だけ伸び([range を切る行](../../src/core/renderer/materialrender.cpp#L426))、列は [`find(pass, material, mode)`](../../src/core/renderer/materialrender.cpp#L431) で引きます。

**variant は mode に依らない(SK-3、着地待ち。`agent/sk3`)。**`drawLoopSubject()` は material の variant の名前を、mode を見ずに主語の `variants` へ写します(上の `drawLoopSubject(material, mode)` の末尾)。それでも skin 付きの部品の主語で `material_variant` の step が `graph.loop_variant_missing@1` になっていたのは、project material の base を登録した時点で、variant を付ける前に生きている表へ通知していたからです(SK-3 の実装者の報告)。SK-3 は base と variant を 1 つの組として登録します。

- [`registerMaterialFamily()`](../../src/core/material/materialregistration.hpp#L29)(`materialregistration.hpp`、Vulkan の確保から切り離した登録の transaction)は、[`ScopedMaterialPublicationDeferral`](../../src/core/material/materialregistration.hpp#L9) で通知を止めたまま base を作って variant を付け、両 mode の pipeline・variant・pass の descriptor が揃ってから生きている表へ通知します。途中で失敗したら base と variant を解放し、既に受け付けた表からも消します。止める印は入れ子でも呼び手の状態に戻ります。
- 呼ぶのは base と variant を受ける [`MaterialContainer::registerMaterial(info, variants)`](../../src/core/material/materialcontainer.cpp#L1274) の overload で、[`ProjectMaterialAssetContainer` がここを呼びます](../../src/core/material/projectmaterialasset.cpp#L727)。単独の `registerMaterial(info)` と後付けの `registerMaterialVariants()` の契約は変わりません。
- test: `materialdrawlooptable_test` の「SK3 materialdrawlooptable resolves outline in both modes after family publication」ほか 2 本(CPU)と、CB-1 の組 02「CB1 02 skin outline variant and when skinned draw loop」(GPU)。

**pass ごとに描いた material の数え方(✅MP-1)。**`get_frame_plan` の material pass の node の `drawn_materials` は、描くときに数えたものです。

1. **数える所**: `renderMaterialDraws()` の中で、draw ごとに material を bind した直後に、実効の material(variant と draw_loop の step を解決した後の id)と `draw_call.draw_count` を足します([MP-1 で draw ごとに足す所](../../src/core/renderer/materialrender.cpp#L310))。足す先は [`MaterialPassDrawnMaterials`](../../src/core/renderer/materialrender.hpp#L39) で、最初に描いた順の vector と、id から位置を引く map を持ちます。pass の名前ごとの map が `MaterialPassDrawTally` で、[`MaterialRendererDependencies::drawn_materials`](../../src/core/renderer/materialrender.hpp#L74) が null なら何も数えません。
2. **数の意味**: `draw_count` は CPU の draw queue の range の indirect command の数です。GPU が数を書く draw(`gpu_draw_source`)でも、`drawIndexedIndirectCount` が実際に描いた数ではなく、この数を足します。同じ pass が 1 frame に何度呼ばれても(同じ tally に入るので)和になり、draw_loop の step で同じ range を 2 回描けば 2 回足します。
3. **frame の tally**: `LogicalFrameRecorder` が frame ごとに 1 つの tally を持ち、`recordOneExecution()` が `FrameGraphExecutionRequest::drawn_materials` に渡し、`executeRenderingPasses()` が material renderer の依存に写します。
4. **公開した frame に残す**: `renderLogicalFrame()` が `publishFrame()` の後に [`retainDrawnMaterials()`](../../src/core/vkcore/renderer.cpp#L6988) を呼びます。ここで `Renderer::last_drawn_materials`([宣言 MP-1](../../src/core/vkcore/renderer.hpp#L261))を作り直します。pass ごとに先頭の [`drawnMaterialsListLimit`](../../src/core/vkcore/renderer.hpp#L65)(256)件まで、`MaterialContainer::materialName()` で名前を引いて残し、それを超えたら `truncated` にします。program(`RenderingPassId`)・runtime の世代・frame(`EngineTime::evaluationIndex()`)も一緒に残します。
5. **plan に出す**: [`currentFramePlanJson()` の MP-1 の分](../../src/core/vkcore/renderer.cpp#L4728)は、残した記録の program と世代が今のものと同じときだけ、今の program の material pass ごとに、同じ名前の node に `drawn_materials`・`drawn_materials_frame`(と `drawn_materials_truncated`)を書きます。記録に無い pass は `[]` です。名前の無い material では `material` の鍵を出しません。

test は `headless_material_draw_loop_test` の「MP-1 get_frame_plan lists the materials each material pass drew in the last frame」(GPU)で、pass ごとの数を、同じ frame の draw の記録(`MaterialDrawRecord` の `effective_material` と `draw_count`)の和と比べます。

> 🧩 **難所 — 静的な登録簿の寿命**(`cf40ec93`): 生きている表の一覧(と mutex)は関数ローカルの static でしたが、プロセスの終了時に module の container が世代を壊すより先に破棄され、`~MaterialDrawLoopTable` が壊れた vector から erase して、Debug の CRT のダイアログで止まっていました(WP375b の GPU test が ctest の 1860 秒で timeout)。今は `renderpolicyregistry.cpp` と同じく、一度だけ確保して破棄しない形です([一覧](../../src/core/material/materialdrawlooptable.cpp#L25))。

**view の選択。**provider は view ごとに軸の座標(`light` / `cascade` / `face`)を返し、組み直し時の長さは [`renderViewAxisLength()`](../../src/core/renderer/viewfamilyproviderregistry.hpp#L125) が答えます。`view_select` は pass / task の定義から frame planner の node、physical scope の JSON と fingerprint まで運ばれ(✅RC-2 の前は execution plan にも載っていました)、scheduler は一致する view の分だけ invocation を出します(`sequential` のまま、`logical_view_count` は family の view 数のまま)。組み直し時の検査は [`validateRenderViewSelects()`](../../src/core/renderer/viewfamilyproviderregistry.hpp#L165) です。

## 6.6 Frame target: window と headless の共通インターフェース

**この節の語**: **epoch** は target / swapchain のある状態の世代識別子、**lease** はその世代の資源を使用中に保持する所有参照で、GPU の **submission**（投入した仕事）が完了するまでの寿命は[§6.11 の図](#generation-lifetime)を参照してください。

[`IFrameTarget`](../../src/core/vkcore/frametarget.hpp#L254) が描画先の抽象インターフェースです。

```cpp
// frametarget.hpp:290-302
virtual FrameBeginResult beginFrame(
    std::shared_ptr<const RendererRuntimeGeneration> runtime_generation,
    GpuSubmissionLease submission_lease, FrameBeginMode mode) = 0;
virtual void recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                       vk::Format source_format, vk::Extent2D source_extent) = 0;
virtual FrameSubmitResult submit(FrameTargetFrame frame) = 0;
virtual void abandon(FrameTargetFrame frame) noexcept = 0;
virtual FrameTargetCaps caps() const = 0;
virtual FrameTargetStatus status() const;
virtual std::vector<uint8_t> readbackLastFrameRGBA8() = 0;
```

かつての `render_begin()` / `try_render_begin()` / `render_end()` / `consumeExtentChanged()` は **もうありません**(`993b45e3` で置き換え。`render_end()` の後継は `submit()`、描かずに手放す側は `abandon()`)。取得は `beginFrame()` 1 本に畳まれ、blocking と zero-wait は引数 [`FrameBeginMode`](../../src/core/vkcore/frametarget.hpp#L138) で選びます。

戻り値は bool ではなく [`FrameBeginResult`](../../src/core/vkcore/frametarget.hpp#L203) で、`disposition`(`ready` / `unavailable` / `device_rebuild_required` / `fatal`)と `reason`([`FrameUnavailableReason`](../../src/core/vkcore/frametarget.hpp#L150))を分けて返します。「今フレームは描かない」は `unavailable` であって描画失敗ではありません。取得したフレームは `submit()` か `abandon()` のどちらかで必ず手放し、`beginFrame()` に渡す `GpuSubmissionLease` が GPU resource の寿命を握ります(§6.11)。`FrameTargetCaps` も [`OutputCompileFacts compile_facts` 1 個だけ](../../src/core/vkcore/frametarget.hpp#L46)になりました。

このうち `consumeExtentChanged()` だけは **1 対 1 の後継がありません**。「前回から extent が変わったか」を frame target に尋ねてフラグを消費する口そのものが消え、extent は毎フレーム `FrameRenderContext` の値として無条件に返るだけになりました([extent を返すフィールド](../../src/core/vkcore/frametarget.hpp#L40))。変化したかどうかを決めるのは Renderer 側で、[自分が覚えている前フレームの extent](../../src/core/vkcore/renderer.hpp#L221) と取得したフレームの extent を毎回比べます([view 0 での比較](../../src/core/vkcore/renderer.cpp#L6621))。frame target 側に「変わった」という状態は残らないので、判定は毎フレーム作り直されます。変わっていた場合の処置は §6.7 です。

[`RenderTarget`](../../src/core/vkcore/rendertarget.hpp#L22) がこの interface を所有し、[`createFrameTarget()`](../../src/core/vkcore/rendertarget.cpp#L15) で実装を選びます。

| 実装 | 用途 | 特徴 |
|---|---|---|
| [`SwapchainFrameTarget`](../../src/core/vkcore/swapchainframetarget.hpp#L9) | 通常 window | acquire、submit、present、resize/recreate。surface が TRANSFER_SRC を持てば CPU readback 可 |
| [`OffscreenFrameTarget`](../../src/core/vkcore/offscreenframetarget.hpp#L14) | `--headless` | offscreen color/depth、最後の RGBA8 を readback 可能 |

`FrameRenderContext` は command buffer、color/depth view、extent、待機 semaphore、最後に必要な image layout に加え、[`in_flight_frame_index`](../../src/core/vkcore/frametarget.hpp#L43) を返します(logical frame の view 間整合チェックと FrameUBO slot 選択に使用)。frames-in-flight は [`in_flight_frames_num = 2`](../../src/core/vkcore/rendertarget.hpp#L16) です。

headless capture は [`RenderTarget::captureLastFrameToPng()`](../../src/core/vkcore/rendertarget.cpp#L102) が BGRA/RGBA を補正して PNG を書きます。swapchain 実装の [`readbackLastFrameRGBA8()`](../../src/core/vkcore/swapchainframetarget.cpp#L3086) も、surface が TRANSFER_SRC を持てば windowed で readback を実装済みです。持たない場合のみ `capture unavailable_windowed` 例外になります。

> 🧩 **難所 — WSI の回復は「どこで返ったか」で意味が変わる**([`decideWindowWsiRecovery()`](../../src/core/vkcore/swapchainrecovery.hpp#L86) / [`classifyWsiResult()`](../../src/core/vkcore/swapchainrecovery.hpp#L51) / [`BasePresentRetirementTracker`](../../src/core/vkcore/swapchainrecovery.hpp#L18))
>
> **何をする所か**: `VkResult` を「今フレームを落とす / swapchain を作り直す / surface を作り直す / device ごと作り直す / 後で再試行する」のどれかへ翻訳します。かつて `SwapchainFrameTarget::beginFrame(bool nonblocking)` の中に `nonblocking` の分岐として散っていた判断が、**純粋な決定表**として [`swapchainrecovery.hpp`](../../src/core/vkcore/swapchainrecovery.hpp) へ外出しされました(`SwapchainFrameTarget` 自体は pimpl 化しています)。
>
> **素朴に読むと**: 同じ `VkResult` が **観測地点によって別の処置**になる、という一点を掴まないと読めません。コメントが規範です。
>
> > The same VkResult has different recovery meaning depending on where it was
> > observed. For example, NOT_READY is a routine frame drop at acquire, while a
> > failed preparation call must enter bounded retry. Keep this policy pure so
> > the real WSI path and protocol-fake tests share one decision table.
>
> だから引数が `(WindowWsiCallSite, vk::Result)` の 2 つあり、`acquire` での `eNotReady` は日常的なフレームドロップ、`swapchain_create` での同じ結果は bounded retry になります。もう 1 つの非自明が退役(retirement)側で、**base Vulkan には「提示が完了した」を知るプリミティブが無い**ため、「後継 epoch が提示したイメージを再取得できた」ことを代用の完了信号にしています。
>
> > Base Vulkan has no presentation-complete primitive. After an image that was
> > presented by a successor epoch is reacquired, every earlier swapchain can be
> > retired without guessing about presentation-engine progress.
>
> そのうえ [`surfaceLossRequiresDeviceRebuild(exact_present_retirement, has_unproven_present)`](../../src/core/vkcore/swapchainrecovery.hpp#L36) が示すとおり、surface 喪失時に device ごと作り直すかどうかまで「厳密な完了が取れるか」に依存します。「同じエラーなのに扱いが違う」の理由がここに全部あります。
>
> **骨子**:
>
> | 関数・項目 | 分類・値・条件 |
> |---|---|
> | `classifyWsiResult(result)` | `{ready, refresh_advisory, not_ready, swapchain_unavailable, surface_unavailable, device_lost, retryable_failure, fatal}` |
> | `decideWindowWsiRecovery(site, result)` | `action` を返す |
> | `site` | `{acquire, present, surface_create, surface_support_query, swapchain_create, dependent_resources}` |
> | `action` | `{proceed, proceed_and_refresh, drop_frame, replace_swapchain, replace_surface, rebuild_device, retry_later, fatal}` |
> | retire | 後継 epoch の image を再取得できたら、それ以前の epoch を退役してよい |
>
> **具体例**: 同じ `VkResult` が site でどう分かれるかを、実際の決定表([`decideWindowWsiRecovery()`](../../src/core/vkcore/swapchainrecovery.cpp#L87))から 4 行だけ抜くとこうなります。
>
>
> | VkResult | 分類 | site=acquire | site=swapchain_create |
> |---|---|---|---|
> | eNotReady / eTimeout | not_ready | drop_frame | retry_later |
> | eSuboptimalKHR | refresh_advisory | proceed_and_refresh | retry_later |
> | eErrorOutOfDateKHR | swapchain_unavailable | replace_swapchain | replace_swapchain |
> | (表にない未知のエラー) | fatal | fatal | retry_later |
>
> つまり window をドラッグでリサイズしている最中の典型列は「present が `eSuboptimalKHR` → **そのフレームは表示したうえで**次の作り直しを予約」→「次の acquire が `eErrorOutOfDateKHR` → `replace_swapchain`」です。同じ `eErrorOutOfDateKHR` でも `surface_create` / `surface_support_query` で観測すれば `replace_surface` になり、swapchain ではなく surface から作り直します。最後の行が site で割れるのは「準備呼び出しは途中まで公開された object を持たないので retry に落とせる、acquire / present は隠せない」という理由で、[`swapchainrecovery.cpp` 内](../../src/core/vkcore/swapchainrecovery.cpp#L139) のコメントが規範です。
>
> **手がかり**: zero-wait 経路(XR ミラー)は今も「落としてよい optional sink」で、`beginFrame(..., FrameBeginMode::nonblocking)` として渡されます([`openxrmirrorsink.cpp` 内](../../src/core/openxr/openxrmirrorsink.cpp#L303))。「今回は描かない」は例外ではなく `FrameBeginDisposition::unavailable` + `FrameUnavailableReason` です。[`FrameTargetStatus`](../../src/core/vkcore/frametarget.hpp#L176) の `surface_epoch` / `swapchain_epoch` / `recovery_attempt` / `exact_present_retirement` は**診断ではなく契約**で、テストはこの値を読みます([`swapchainrecovery_test.cpp`](../../test/swapchainrecovery_test.cpp)、プロトコル fake の窓は [`run_wsi_fault_window.cmake`](../../test/run_wsi_fault_window.cmake))。エポック回復プロトコルの全体像は [`docs/design_wsi_epoch_recovery.md`](../design_wsi_epoch_recovery.md) にあります。
>
> **不変条件**: 決定表は純粋関数のまま保つこと(実 WSI 経路とプロトコル fake テストが同じ表を共有するのが目的です)。call site を増やしたら決定表側も同時に増やし、呼び出し元で `vk::Result` を直接分岐しないこと。

### [`makeFrameDescriptorPoolPlan()`](../../src/core/renderer/frameresources.cpp#L28) — multiplier 3 / 6 の根拠 {#reading-note-018}

1 スロットあたり **uniform 3 個**(frame UBO / resolution UBO / light UBO)と **storage 3 個**(object / previous object / directional shadow)を消費するので係数 3 です。ray_query 時はスロットごとに通常セットと ray_query セットの 2 セットを確保する(`max_sets = slot_count * 2`)ので 3×2 = 6 になり、加えて acceleration structure を `slot_count` 分積みます。内訳は [frame/resolution UBO の 2 write](../../src/core/renderer/frameresources.cpp#L205) と [`updateSceneDescriptors()` の 4 write](../../src/core/renderer/frameresources.cpp#L312)(storage 3 + light UBO 1)を足すと一致します。

### [`replacement_anchor` の保存](../../src/core/vkcore/swapchainframetarget.cpp#L1150) — 成功時に退避する理由 {#reading-note-060}

[コメント](../../src/core/vkcore/swapchainframetarget.cpp#L1150)どおり **`vkCreateSwapchainKHR` が成功した時点で oldSwapchain は即退役する**ので、その後の依存オブジェクト生成が失敗すると「まだ退役していない有効な swapchain」が 1 つも残らなくなります。だから新ハンドルを依存リソース作成前に退避し、リトライ時の oldSwapchain アンカーとして使います。

### [`releaseAbandonedImage()`](../../src/core/vkcore/swapchainframetarget.cpp#L1297) — `vkReleaseSwapchainImagesEXT` をいつ呼ぶか {#reading-note-061}

maintenance1 が有効で、旧 epoch に「acquire 済みだが present せず放棄した image」(`request.release_image_index`、pending_abandonment 由来)がある場合**のみ**、その 1 枚に対して cutover 直前に呼びます。失敗は警告ログのみです。

### [present 完了追跡の 2 系統](../../src/core/vkcore/swapchainframetarget.cpp#L557) — 証明と推論 {#reading-note-062}

maintenance1 があるときは image ごとの present fence(+ 再利用時に退避した `superseded_present_fences`)で**厳密に証明**します。無い場合は `BasePresentRetirementTracker` が「後続 epoch が同 image を reacquire した」ことを watermark として記録し([`noteAcquired()`](../../src/core/vkcore/swapchainframetarget.cpp#L609) → `noteSuccessorImageReacquired`)、それ以前の epoch は present 完了と**推論**します。WSI の戻り値の読み方は[第6章「WSI の回復は『どこで返ったか』で意味が変わる」](06_rendering_vulkan_shader.md)。

### [`surfaceLossRequiresDeviceRebuild()`](../../src/core/vkcore/swapchainframetarget.cpp#L1544) — "unproven presentation" {#reading-note-063}

[実装](../../src/core/vkcore/swapchainrecovery.cpp#L36)は `!exact_present_retirement && has_unproven_present` の 1 行なので、true になるのは **maintenance1 が無く、かつ present 完了を証明できていない epoch が残っているとき**だけです。base Vulkan には surface lost 後の present 完了プリミティブが無く、退役できない swapchain がネイティブウィンドウを掴み続けて `NATIVE_WINDOW_IN_USE` の無限リトライになるため、device 再構築を要求します。"unproven presentation" とは `mayRetire()` が false のまま(= 後続 reacquire による証明が得られていない)present 済み epoch のことです。

## 6.7 Offscreen render target と layout tracker

frame target の color/depth と、frame graph 設定で宣言する offscreen target は別物です。後者は [`RenderTargetContainer`](../../src/core/renderingpass/rendertargetcontainer.hpp#L27) が、名前、format、固定/相対 extent、image、view を所有します。

<a id="image-vocabulary"></a>

### image の語彙: `ImageFormat` / `ImageUsage` / extent(✅RC-3a1、`5446196f0`)

render target の format・usage・extent は project の中立な型で持ち、Vulkan の値にするのは core の 1 つの翻訳単位だけです(設計 [design_render_graph_ir](../design_render_graph_ir.md) §4 と §5 の RC-3a)。

- **format**: [`ImageFormat`](../../src/project/imageformat.hpp#L15)(`pelican_project`、46 値)。綴りは作者の形(`R16G16B16A16_SFLOAT`)が名前です。
  - 性質表は [`kImageFormats`](../../src/project/imageformat.cpp#L17) の 1 つだけで、1 行が [`ImageFormatInfo`](../../src/project/imageformat.hpp#L99) です: 名前、`authorable`(render target の宣言が書けるか。45 行が true、`D32_SFLOAT_S8_UINT` だけ false)、aspect(`color` / `depth` / `stencil` / `depth_stencil`)、成分数、成分の格納の型 [`ImageComponentStorage`](../../src/project/imageformat.hpp#L76)(unorm / snorm / uint / sint / sfloat / ufloat / srgb。depth+stencil は depth の型と `stencil_storage` を別々に)、容量の見積もり `estimated_texel_bytes`(RC-1 の値。D32S8 = 8)。行は enum の順で、`static_assert` が順を固定します。
  - shader が見る型 [`imageShaderNumeric()`](../../src/project/imageformat.cpp#L147)(float / signed / unsigned)は表の列ではなく、格納の型から導く関数です。material pass の出力の型の照合(`materialPassFormatNumericClass()`)、MSAA の resolve の方式([`colorAttachmentResolveMode()`](../../src/core/renderingpass/renderingsamplecount.cpp#L2344)。整数は sample 0、ほかは平均)、history の clear が使います(以前の `ends_with("Sint"/"Uint")` の判定の置き換え)。
  - 綴り ⇔ enum: [`findImageFormat()`](../../src/project/imageformat.cpp#L182)(46 全部。engine が書いた文書を読む側)、[`parseAuthoredImageFormat()`](../../src/project/imageformat.cpp#L189)(作者の 45 だけ、`Unknown format: <名前>`)、`authoredImageFormatNames()`(Studio の候補)。render target の宣言は `parseRenderGraph()` が `format` と `format_candidates` をこの関数で読みます([`format`](../../src/project/rendergraphparse.cpp#L1686)、[`format_candidates`](../../src/project/rendergraphparse.cpp#L1700))。関数の文(`Unknown format: <名前>`)は外に出ず、宣言の拒否は `render_graph.image_invalid@1 {resource, field: format | format_candidates, reason: unknown}` です(✅RC-3c2a で core の render target の parser `parseRenderTargetDefinitionsFromJson` は消え、宣言を読むのはこの parser だけ。test「RC-3a1 a render target declaration accepts and refuses the formats and usages it did」がこの code を見ます)。
  - JSON: `to_json(ImageFormat)` は名前、`to_json(optional<ImageFormat>)` は無ければ `""`(物理計画の buffer)。`from_json` は delete で、読み手は必ず名前付きの parse を通ります([宣言](../../src/project/imageformat.hpp#L166))。
  - v0 の綴りの読み替え: [`imageFormatFromV0PhysicalPlanName()`](../../src/project/imageformat.cpp#L201) が `R16G16B16A16Sfloat` の形(最初の語はそのまま、後の語は先頭だけ大文字、`_` なし)を表の名前から作って比べます。使うのは [`parsePhysicalPlanImageFormat()`](../../src/project/imageformat.cpp#L224) だけで、その呼び手は ✅RC-6b2 から complete physical plan の読み手([`requireFormat()`](../../src/project/vulkancompletephysicalplan.cpp#L121))だけです(物理 fragment の読み手は fragment ごと消えた)。読めても `automatic_plan_fingerprint` は format の名前を含むので([足す所](../../src/project/vulkanphysicalfragment.cpp#L469))、v0 で eject した package は stale で断られます(manual [第6章](../manual/06_rendering.md#v0-physical-fragment-format))。
- **usage**: [`ImageUsageBit`](../../src/project/imageusage.hpp#L14) の 8 ビットと、`vk::Flags` と同じ使い心地の集合 [`ImageUsage`](../../src/project/imageusage.hpp#L27)。名前の表 [`kImageUsages`](../../src/project/imageusage.cpp#L11) は作者の 6 つを先に、engine が足す `INPUT_ATTACHMENT` / `TRANSIENT_ATTACHMENT` を `authorable = false` で持ちます。宣言は [`parseAuthoredImageUsage()`](../../src/project/imageusage.cpp#L51) で読みます。
  - engine が宣言に足す usage は project の純関数 [`engineImageUsage()`](../../src/project/generationresources.cpp#L16)([`ImageUsageFacts` と宣言](../../src/project/generationresources.hpp#L36))の 1 つです(✅RC-3c2a。RC-3a1 の時点の core の `registeredRenderTargetUsage` と、物理計画の前後に散っていた加算を置き換えた): 宣言のビットに local read なら input、外部 depth の export なら transfer_src を足し、transient なら transient を足し、tile-local なら attachment のビットだけ残して input と transient を足し、history なら transfer_dst を足します。適用は 2 時点で、物理計画の前の device への問い([`queriedImageUsage()`](../../src/core/renderingpass/renderingsamplecount.cpp#L243))と、variant の storage を統合した後の generation の資源の記述の最終 usage です(下の「中立な描画グラフ」の generation の資源の記述)。container の [`registerRenderTarget()`](../../src/core/renderingpass/rendertargetcontainer.cpp#L544) も宣言の検査の後に同じ関数を通します(最終 usage には何も足さない)。
- **extent**: 新しい型は作らず、[`ResolvedResourceExtent`](../../src/project/resourceextent.hpp#L32)(uint32 × 2)を使います。`ResourceExtentPlan` / `ResolvedResourceExtent` / `resolveResourceExtent()` の宣言は core の軽いヘッダから include できるよう `resourceextent.hpp`([`ResourceExtentPlan`](../../src/project/resourceextent.hpp#L18) から)に移しました(定義は `targetrenderplanning.cpp` のまま)。
- **中立にした欄**: [`RenderTargetDefinition`](../../src/core/renderingpass/rendertargetdefinition.hpp#L35) の `fixed_extent` / `format` / `format_candidates` / `usage`、`RenderTargetMetadata` の `usage` / `format` / extent([欄](../../src/core/renderingpass/rendertargetmetadata.hpp#L18))、project の物理計画の `VulkanPhysicalResourcePlan.format`(`optional<ImageFormat>`。buffer と、表に無い外部 swapchain の形式は nullopt で dump と fingerprint は `""`)と format 候補・format capability・外部 depth・sample count の format。
- **Vulkan への写像**: core の [`vulkanimagevocabulary.hpp`](../../src/core/renderingpass/vulkanimagevocabulary.hpp#L19) / [`vulkanimagevocabulary.cpp`](../../src/core/renderingpass/vulkanimagevocabulary.cpp#L15) の表 `kVulkanFormats`(enum の順、`static_assert`)と [`kVulkanUsages`](../../src/core/renderingpass/vulkanimagevocabulary.cpp#L74) だけです。関数は `toVulkanFormat` / `imageFormatFromVulkan` / `requireImageFormatFromVulkan`(swapchain や device が渡す vk の値を戻す)/ [`vulkanFormatName()`](../../src/core/renderingpass/vulkanimagevocabulary.cpp#L108)(表示用。表に無い vk 形式は `vk::to_string` の綴り)/ `toVulkanImageUsage` / `imageUsageFromVulkan` / `toVulkanExtent` / `extentFromVulkan`。image を作るのは [`createRenderTargetImage()`](../../src/core/renderingpass/rendertargetcontainer.cpp#L241) です。`src/project` に `vk::Format` / `vk::ImageUsageFlags` / `vk::Extent2D` は 0 件です(RC-3a1 の実装者の報告の `rg`)。
  - 範囲外で vk のまま残るもの(RC-3a1 の報告): material pass の vk の定数 `materialPassColorAttachmentFormats*` など(検査は `toVulkanFormat(metadata.format)` で比べる)、`compileRenderingTargetPlans` の swapchain の `vk::Format` の引数、`GraphicsPipelineDesc` の format、readback の構造体、頂点属性・shader reflection。
- **snapshot は別の契約**: bytes は性質表の列から取りますが、snapshot が写せる format の集合 12 種は [`snapshotCopiesFormatOf()`](../../src/project/rendergraphir.cpp#L1027)(RC-3c1b で core の計画用 parse の `kSnapshotCopyFormats` から parser の `assignSnapshotSizing` に移り、✅RC-3c2a でこの関数に移った)として別に持ち、それ以外は `render_graph.snapshot_size_unavailable@1 {graph, node, resource}` で断ります(解決済みの format は `parseRenderGraph()` の最後、class だけの image は finalize)。
- **compose の depth 判定**: [`composedTargetKind()`](../../src/project/rendergraphcomposition.cpp#L1486) と [`composedTargetKindOf()`](../../src/project/featurecompose.cpp#L1880) は「format の先頭が `D`」をやめ、表の aspect で見ます(表に無い名前は color)。Studio の [`GraphCanvasModel::depthFormat()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7161) の名前の判定は、engine の `format_aspects` が無いときの fallback として残っています。
- test([`imagevocabulary_test.cpp`](../../test/imagevocabulary_test.cpp#L55)、GROUP render_pipeline):
  - 「RC-3a1 every row of the image format table is what Vulkan's format traits say」: 46 行で aspect(`vk::componentName`)・成分数・成分ごとの格納の型(`vk::componentNumericFormat`)・写像の往復と一意性を照合します。core 側の test で、RC-3c2 で core の写像の test に吸収してよい移行中の橋です(仕様)。
  - [「RC-3a1 a render target declaration accepts and refuses the formats and usages it did」](../../test/imagevocabulary_test.cpp#L93): 45 format・6 usage の受理と、内部の値と vk の綴りの拒否(本番の parser でも)。
  - [「RC-3a1 the loader, the planner and registration consume the declared image vocabulary」](../../test/imagevocabulary_test.cpp#L222): 本番の parser → `compileRenderingTargetPlans` → 登録の値と core の写像を通し、format(vk)・数値型・usage(history の transfer_dst 込み)・extent を独立に書いた vk の期待値と比べます(✅RC-3c2a から、登録の値は test の helper [`plannedRenderTargets()`](../../test/registered_render_targets_test_support.hpp#L53) が `imagePlacementsOf()` → 1 variant の `resolveGenerationResources()` → `renderTargetDefinitionOf(GenerationImage)` で作ります。前は `applyRenderingTargetPlan` → `registeredRenderTargetUsage`)。候補の選択なし / complete physical plan で選択、の 2 section(✅RC-6b2 で physical fragment で選択する section(v0 の綴りに書き換えた package を含む)は消えた)。GPU は要りません。
  - 実装者の報告の対照: 表の `R8_UINT` の格納の型を sint にすると 1 本目が赤、写像の `R8_UINT → eR8Uint` を `eR16Uint` にすると 3 本目が赤。

<a id="render-vocabulary-rest"></a>

### 残りの語彙: attachment の load / store・写しの enum・resolution domain・buffer と gpu draw の宣言(✅RC-3a2、`4114e2607`)

RC-3a1 の image の語彙に続いて、RC-3a の残りを project の中立な型にしました(設計 [design_render_graph_ir](../design_render_graph_ir.md) §3 と §5 の RC-3a)。挙動は変えていません(wire の綴りと fingerprint も同じ)。`src/project` に `vk::` / `<vulkan` は 0 件です(RC-3a2 の実装者の報告の `rg`)。

- **load / store**: [`AttachmentLoadOp`](../../src/project/attachmentoperations.hpp#L21)(`load` / `clear` / `dont_care`)、[`AttachmentStoreOp`](../../src/project/attachmentoperations.hpp#L27)(`store` / `dont_care`)、[`AttachmentAspect`](../../src/project/attachmentoperations.hpp#L16)(`color` / `depth`)。以前の 4 つの表現(`PassDefinition` などの `vk::AttachmentLoadOp`、計画の `FrameGraphAttachmentLoadOp`、物理計画の `VulkanPhysicalAttachmentLoadOp`、JSON の綴り)と aspect の写し 3 つ(`PassAttachmentAspect` ほか)をこれにまとめ、別名は残していません。
  - 既定は [`defaultPassAttachmentOperations()`](../../src/project/attachmentoperations.cpp#L84)(pass の型と aspect から。color は `clear` / `store` で `ui` / `imgui` だけ `load` / `store`、depth は `clear` / `dont_care`)、優先順は [`resolveAttachmentOperations()`](../../src/project/attachmentoperations.hpp#L56)(attachment > pass > 既定、load と store は別々)。どちらも project の 1 か所で、計画([`rendergraphparse.cpp`](../../src/project/rendergraphparse.cpp#L1130)。✅RC-3c1b で core の計画用 parse から移った)と実行([`PassDefinition::colorAttachmentOperations()`](../../src/core/renderingpass/renderingpass.hpp#L364) / `depthAttachmentOperations()`。物理計画の値が無いときの fallback)が呼びます。core の `passattachmentoperations.cpp` は消えました。
  - **解決済みの計画は op を省略できない**: [`PassAttachmentOperations`](../../src/project/attachmentoperations.hpp#L32) は既定構築を delete してあり、計画の [`FrameGraphAttachmentDefinition`](../../src/project/frameplan.hpp#L36) と物理計画の [`VulkanPhysicalAttachmentPlan`](../../src/project/targetrenderplanning.hpp#L267) は `load_op` / `store_op` の 2 欄(aspect に関係なく `clear` / `store` が初期値)から `operations` 1 欄に変わりました。編集の側の `VulkanPhysicalAttachmentFragment`(「書いていない = 自動 plan のまま」なので `optional` の 2 欄)は ✅RC-6b2 で型ごと消えました。
  - 綴りの表は [`kLoadOps` / `kStoreOps`](../../src/project/attachmentoperations.cpp#L29) の 1 つ(op ごとに 正規形 / 物理計画 / 作者の別綴り)で、入口ごとの関数がこれを引きます: 作者の [`findAuthoredAttachmentLoadOp()`](../../src/project/attachmentoperations.cpp#L130) / `findAuthoredAttachmentStoreOp()`(7 / 5 綴り、知らない綴りは nullopt。例外を投げる wrapper の `parseAuthoredAttachmentLoadOp()` / `parseAuthoredAttachmentStoreOp()`(`Unknown attachment load op: <綴り>`)は、RC-3a2 の時点で呼んでいた 3 系統のうち計画用 parse が RC-3c1b で、実行用の pass と attachment の parser が ✅RC-3c2b2 で消えて test だけが呼んでいたので、✅RC-4 で消しました。`parseRenderGraph()` は正規形の綴りだけを受け、別綴りは `render.json_not_normalized@1`)、物理計画の `findPlanAttachmentLoadOp()`(`clear` / `load` / `discard`、`store` / `discard`。[complete plan の読み手](../../src/project/vulkancompletephysicalplan.cpp#L373)。fragment の読み手は ✅RC-6b2 で消えた)、書く側の `attachmentLoadOpPlanName()`(dump・eject・[fingerprint](../../src/project/vulkanphysicalfragment.cpp#L560))、Studio の schema(`authoredAttachmentLoadOpSpellings()` を[引く所](../../src/project/graphparameterschema.cpp#L250))。正規化の [`canonicalLoadOp()`](../../src/project/renderingjsonnormalize.cpp#L39) は RC-3a2 で触っておらず、同じ集合の別の表のままです(RC-3a2 の報告 §3 は正規化が `findAuthored*` を引くと書いていますが、4114e2607 の差分に `renderingjsonnormalize.cpp` は入っておらず、正規化は自前の表を使います。コードで確認)。入口ごとの受理集合の表は manual [第6章](../manual/06_rendering.md#attachment-load-store) §6.2「attachment の load / store」。
  - **Vulkan への写像**は [`toVulkanAttachmentLoadOp()`](../../src/core/renderingpass/vulkanimagevocabulary.cpp#L139) / [`toVulkanAttachmentStoreOp()`](../../src/core/renderingpass/vulkanimagevocabulary.cpp#L151) の 1 か所。記録の側は [`recordedattachmentops.hpp`](../../src/core/vkcore/recordedattachmentops.hpp#L14) の `recordedAttachmentOps` / `recordedColorAttachmentOps` / `recordedDepthAttachmentOps`([定義](../../src/core/vkcore/render_pass_frame_setup.cpp#L346))を通り、[`createColorAttachments()`](../../src/core/vkcore/render_pass_frame_setup.cpp#L256) / [`createDepthAttachment()`](../../src/core/vkcore/render_pass_frame_setup.cpp#L317)、tile-local scope の 2 関数、UI / ImGui の dispatch([呼ぶ所](../../src/core/vkcore/render_pass_dispatch.cpp#L141))が全部ここを使います。この header を新設したのは、`render_pass_frame_setup.hpp` が VMA を要する header を引き、render_pipeline group の test から include できないからです(報告)。UI / ImGui の要求構造体、OpenXR の mirror と sprite renderer の自前の attachment は vk の op のままです(報告)。
- **写しの enum**: sampling は core の `FullscreenInputSampling` / `Filter` / `AddressMode` を project の [`ShaderResourcePortSampling`](../../src/project/shaderresourceport.hpp#L90) / [`ShaderResourcePortFilter`](../../src/project/shaderresourceport.hpp#L34) / [`ShaderResourcePortAddressMode`](../../src/project/shaderresourceport.hpp#L39) に(fullscreen の [`input_sampling`](../../src/project/passinfo.hpp#L26) もこの型。✅RC-4 から `FullscreenPassInfo` は project の `passinfo.hpp`)。view の次元は core の `PassInputViewDimension` を project の [`VulkanResourceViewLayout`](../../src/project/targetrenderplanning.hpp#L181) に(名前に Vulkan とあるが project の中立な型。新しい名前は作っていない)。cull / depth compare は `SurfaceCullMode` / `SurfaceDepthCompare` を PSN-1 の [`RasterCullMode`](../../src/project/rasterpass.hpp#L36) / [`RasterDepthCompare`](../../src/project/rasterpass.hpp#L63) に。**統合は型だけ**で、既定は所有者ごとに違うままです: [`SurfaceRenderState`](../../src/project/surfaceformat.hpp#L169) は cull `back`・depth test / write on・`less`、[`PipelineState`](../../src/project/rasterpass.hpp#L254) は cull `none`。[`materialPipelineState()`](../../src/project/surfaceformat.cpp#L1431) は合成の中身を変えず、switch が代入になっただけです。
- **resolution domain**: [`RenderResolutionDomain`](../../src/project/renderresolutiondomain.hpp#L16) を vk を include する `renderingpass.hpp` から `src/project/renderresolutiondomain.{hpp,cpp}` へ。既定の [`defaultRenderResolutionDomain()`](../../src/project/renderresolutiondomain.cpp#L23) は type の**文字列**の比較から `RenderPassType` の switch になり、parse([`parseRenderResolutionDomain()`](../../src/project/renderresolutiondomain.cpp#L48))も同じ所です。Studio の schema の既定の表示(以前は `"independent"` / `"scene"` / `"unclassified"` の直書き)も [`resolutionDomain(RenderPassType)`](../../src/project/graphparameterschema.cpp#L260) がこの関数から取ります。
- **buffer / indirect / gpu draw の宣言**: core の `framegraphbufferdefinition.hpp`(`<vulkan/vulkan.hpp>` を include)は消え、project の [`framegraphbufferdefinition.hpp`](../../src/project/framegraphbufferdefinition.hpp#L61) が [`FrameGraphBufferExtentSizeDefinition`](../../src/project/framegraphbufferdefinition.hpp#L61) / [`FrameGraphBufferDefinition`](../../src/project/framegraphbufferdefinition.hpp#L82) / [`ComputeIndirectDispatchDefinition`](../../src/project/framegraphbufferdefinition.hpp#L125) と byte の定数、名前関数([`frameGraphHostBufferSourceName()`](../../src/project/framegraphbufferdefinition.cpp#L13) など)を持ちます。大きさ・offset は `vk::DeviceSize` から `std::uint64_t` に。indexed draw の command の大きさは中立な [`frameGraphIndexedDrawCommandBytes`](../../src/project/framegraphbufferdefinition.hpp#L42)(`sizeof(uint32_t) * 5` = 20)で、core の [`static_assert`](../../src/core/renderingpass/vulkanimagevocabulary.cpp#L162) が `sizeof(vk::DrawIndexedIndirectCommand)` と照合します。
  - gpu draw は作者の値を project の [`GpuDrawSourceDeclaration`](../../src/project/gpudrawsource.hpp#L32)(buffer の名前・`max_draw_count`・uint64 の offset・layout / fallback / execution と 3 つの enum)に分け、core の [`GpuDrawSourceDefinition`](../../src/core/renderingpass/renderingpass.hpp#L164) はそれを**継承**して登録済みの `FrameGraphBufferId` 3 つ(`commands_id` / `count_id` / `segments_id`)を足すだけです(継承にしたのは `draw_source->commands` などの読み手を変えずに済むから。報告)。ID を書くのは [`pinGpuDrawSourceBufferBindings()`](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L33)。宣言を読むのは ✅RC-3c2b2 から `parseRenderGraph()` の [`gpuDrawSource()`](../../src/project/rendergraphparse.cpp#L500)(IR の `MaterialNodeInfo::gpu_draw_source`)で、`passDefinitionOf()` がそれを継承して定義にします(前は core の `parseGpuDrawSourceFromJson()`)。
  - 「project の側から core / Vulkan の header なしで使える」は test が強制します: [`rendervocabulary_test.cpp`](../../test/rendervocabulary_test.cpp#L13) は project の header だけを include し、Vulkan の header の印があれば `#error`、link は `pelican_project` だけの group `project_formats` です。
- test(`rendervocabulary_test.cpp` は GROUP project_formats で link は `pelican_project` だけ、`attachmentoperations_test.cpp` は GROUP render_pipeline。[登録](../../test/CMakeLists.txt#L3487)):
  - [「RC-3a2 the project's render vocabulary needs neither core nor Vulkan」](../../test/rendervocabulary_test.cpp#L59): uint64 の型、20 bytes、4 GiB を超える offset、名前関数。
  - [「RC-3a2 a pass type's omitted attachment operations and resolution domain come from one place」](../../test/rendervocabulary_test.cpp#L97): 全 type × aspect の既定、優先順、domain の既定と `none`、拒否の文言、Studio の fallback = 既定の関数。
  - [「RC-3a2 each way in admits the attachment op spellings it did and refuses the rest by name」](../../test/rendervocabulary_test.cpp#L169) と [「RC-3a2 the pass parsers admit the authored op spellings and refuse the rest by name」](../../test/attachmentoperations_test.cpp#L293): 入口ごとの受理と拒否(作者の `color_load_op: "discard"` と wire の `dont_care` は断られる)。
  - [「RC-3a2 the loader, the planner and registration hand the recording the attachment ops they resolve」](../../test/attachmentoperations_test.cpp#L229): 本番の loader → `compileRenderingTargetPlans` → 登録の値(✅RC-3c2a から test の helper `plannedRenderTargets()` が generation の資源の記述から作る。前は `applyRenderingTargetPlan`)→ registration の parse → `compileRenderingPassRuntime` を通し、`createColorAttachments` / `createDepthAttachment` が `beginRendering` の attachment に入れる値(`recordedColor/DepthAttachmentOps`)を独立に書いた vk の期待値と比べます。指定なし / pass だけ / attachment が優先、transient の store、plan の無い実行側の fallback、complete physical plan の綴り(physical fragment の `clear → discard` の SECTION は ✅RC-6b2 で消えた)。GPU の `beginRendering` そのものは見ていません(その直前の関数)。
  - 実装者の報告の対照: `toVulkanAttachmentLoadOp` の `clear → eClear` を `eLoad` にすると最後の test が 3 section で赤。
- 残り(RC-3c1 への申し送り、報告。core の pass の parser と `parseGpuDrawSourceFromJson` は ✅RC-3c2b2 で消えた): core の pass の parser は作者の別綴りを受けたまま、`PassDefinition.color_load_op` などは `optional` でなく [`applyDefaultAttachmentOperations()`](../../src/core/renderingpass/renderingpass.hpp#L352) で既定を詰めている、`parseGpuDrawSourceFromJson` は core、fullscreen の filter / address の文字列の parse は resource port と別、cull / depth compare の名前関数の重複、`PassDefinition.rasterization_samples` などの vk の欄。

<a id="render-graph-ir"></a>

### 中立な描画グラフ `RenderGraphIR` と唯一の JSON parser(✅RC-3c1a・✅RC-3c1b・✅RC-3c2a・✅RC-3c2b1・✅RC-3c2b2)

設計 [design_render_graph_ir](../design_render_graph_ir.md) §2.2・§2.3 の IR と、それを JSON から作る parser は project にあります。RC-3c1a がこの道を足し、RC-3c1b が本番をこの道に切り替えて core の計画用 parse を消しました(設計 §5 の「RC-3c1b で決まった細部」)。完成した graph の JSON を計画のために typed にするのは [`parseRenderGraph()`](../../src/project/rendergraphparse.cpp#L2011) だけで、改訂につき 1 回です。core に JSON → `FrameGraphDefinition` の道は無く、`FrameGraphDefinition` を作るのは project の `frameGraphDefinitionOf()` だけです。RC-3c2a で image と buffer の宣言もこの parser だけが読むようになり、variant をまたぐ資源の統合は project の `resolveGenerationResources()` 1 つになりました(下の「generation の資源の記述」、設計 §2.4 と §5 の「RC-3c2a で決まった細部」)。RC-3c2b1 で compute / RT task の宣言もこの parser だけが読むようになり、registration が受け取る `ComputeTaskDefinition` は確定 IR から作られます(下の「compute / RT task の道」、設計 §5 の「RC-3c2b1 で決まった細部」)。✅RC-3c2b2 で pass の宣言もこの parser だけが読むようになり、registration が受け取る `RenderingPassDefinition` も確定 IR から作られます(下の「[pass の道](#pass-ir)」、設計 §5 の「RC-3c2b2 で決まった細部」)。これで「完成した graph の JSON を typed にするのは `parseRenderGraph()` だけ、改訂につき 1 回」が完成しました(設計 §2.8 の RC-3c2b2 の追補)。compiler program の出力に JSON の欄はもう無く(`normalized_config` は消えた)、registration は IR と資源の記述だけを受けます。

```text
compose(JSON)→ ImGui の追加 → parseRenderGraph(改訂 0)→ compileRenderPipeline(resolved, 改訂 0 の IR)(routing の schema / state は IR から)
   → graph transform / subgraph の provider(応答ごとに parseRenderGraph を 1 回。IR を次の要求へ渡す)
   → provenance を GraphIR に書く → finalizeRenderGraph(1 回。表の適用・改名・snapshot の大きさは output extent で)
   → synchronizeRenderGraphProvenance(IR から pass / resource の provenance)
   → planRenderGraph(FramePlan。hazard_stress なら seed)/ toLogicalGraph(論理 graph)
   → compileRenderingTargetPlans(IR と graph → FramePlan の表を受ける物理計画。順は FramePlan から)→ imagePlacementsOf
        (planner・shadow・物理計画の部品は中で frameGraphDefinitionOf の結果を読む)
全 runtime variant の後(runRenderCompilerProgram の最後に 1 回)
   → 出力の検証(available なら IR 必須・graph 名と frame plan の鍵の一致・buffer 名)
   → 物理の順 == FramePlan の順(physicalNodeOrder(graph)。✅RC-3d2)→ validateMaterialRoutingAgainstIR(routing と最終 IR)
   → computeTaskDefinitionsOf(variant ごとに確定 IR から ComputeTaskDefinition の列)
   → completeGenerationResources: resolveGenerationResources(資源の記述)→ buffer の検査 → validateRenderGraphResources → renderingPassDefinitionsOf(記述の index を仮の ID にして検査だけ)
   → RenderCompilerProgramResult(variants・generation_resources・compute_task_definitions)
   → registration: 記述から RTD と buffer の列 → GPU 登録 → renderingPassDefinitionsOf(registry の ID で)→ compute task の登録(受け取った列に loop_index を入れて)
      (registration の中の実行の登録 compileExecution: 物理の順の検査と scope ↔ node の対応 compileFrameGraphScopeMapping。✅RC-3d3)
```

- **型**([`rendergraphir.hpp`](../../src/project/rendergraphir.hpp#L44)、`pelican_project`): [`RenderGraphIR`](../../src/project/rendergraphir.hpp#L349)(config の `images` / `buffers`、config の compute task の名前と ✅RC-3c2b1 からその作者名の列 `authored_shared_compute_tasks`、`graphs`、`frame_node_order`、`ray_task_acceleration_structures` の表と ✅RC-3c2b1 の `ray_task_callable_records` の表)、[`GraphIR`](../../src/project/rendergraphir.hpp#L326)(`nodes` と provenance 3 種。✅RC-3c2a から graph ローカルの宣言は持たない)、[`NodeIR`](../../src/project/rendergraphir.hpp#L252)、[`ResourceUse`](../../src/project/rendergraphir.hpp#L128)、[`ImageResource`](../../src/project/rendergraphir.hpp#L286)、[`BufferResource`](../../src/project/rendergraphir.hpp#L319)(✅RC-3c2a から `FrameGraphBufferDefinition` の別名で、宣言の全部。`size_from_extent` の buffer の size は 0 で、資源の記述が決める)。
  - node の種類は [`RenderNodeShape`](../../src/project/rendergraphir.hpp#L55): 作者の pass の型 14 種と `compute`、engine だけが作る `acceleration_structure`。`scope` は `graph` / `shared`(config の compute task の写し)/ `synthesized`。
  - `info` は [`NodeInfo`](../../src/project/rendergraphir.hpp#L246) の variant で、実行用の値の全部を持ちます(pass は ✅RC-3c2b2、compute は ✅RC-3c2b1 から。下の「[pass の道](#pass-ir)」「compute / RT task の道」): fullscreen と output_transform([`FullscreenNodeInfo`](../../src/project/rendergraphir.hpp#L165))・raster([`RasterNodeInfo`](../../src/project/rendergraphir.hpp#L176))・補助 pass 6 種([`AuxiliaryNodeInfo`](../../src/project/rendergraphir.hpp#L185))の shader の宣言と実行用の値、material の filter / variant / gpu draw の宣言・`contract`(既定は `defaultMaterialPassContract`)・`output_schema`・✅RC-3c2b2 の `output_states` と range([`MaterialNodeInfo`](../../src/project/rendergraphir.hpp#L189))、compute の shader・dispatch・ray tracing の宣言・ray query の AS([`ComputeNodeInfo`](../../src/project/rendergraphir.hpp#L208))、snapshot の source / destination / `snapshot_after` / bytes([`SnapshotNodeInfo`](../../src/project/rendergraphir.hpp#L225)。bytes は finalize が決め、それまでは 0)、AS の名前。
  - **use が利用の正本**: 読み・書き・footprint・access intent・attachment はすべて `uses` の 1 件ずつで、[`ResourceUseRole`](../../src/project/rendergraphir.hpp#L82) は出所(`input` / `output` / `resource_port` / `screen_input` / `surface_resource` / `material_resource` / `color_attachment` / `depth_attachment` / `indirect` / `copy_source` / `copy_destination`)。attachment の use は解決済みの load / store(`operations`)と subresource と color の位置を持ち、LOAD なら access が `read_write` で footprint が `same_pixel`(parser が付ける)。history の use は入口を問わず footprint が temporal です。
  - **provenance は graph に**: `GraphIR` の `render_strategy` / `graph_transforms` / `subgraph_replacements`。parse は空で作るので、provider が受ける論理 graph には provenance が無く(fingerprint は純粋な graph)、compiler が provider の列の後に書きます。[`frameGraphDefinitionOf()`](../../src/project/rendergraphir.cpp#L360) が写し、論理 graph と物理計画の logical-graph fingerprint に入ります。実装者の報告では、切り替え前の binary で記録した物理計画の fingerprint・provenance・eject した pin / physical fragment と、切り替え後の値の差は 0 でした(設計 §5 の B3)。
  - **format 未解決の image**: `format_class` があって `format` が無い config image は [`format_unresolved`](../../src/project/rendergraphir.hpp#L310) で受けます(editor の compose と preview は class の解決を通らないため)。format が要る所(finalize の最初、device への問い、IR → `RenderTargetDefinition` の [`renderTargetDefinitionOf()`](../../src/core/renderingpass/rendertargetdefinition.cpp#L8))が `render_graph.image_format_unresolved@1 {resource}` で断ります(RC-3c1b の `snapshot_extent_pending` は ✅RC-3c2a で消えました)。
- **parser**: [`parseRenderGraph()`](../../src/project/rendergraphparse.cpp#L2011)([拒否のコードの一覧](../../src/project/rendergraphparse.hpp#L12))。正規形(RC-3b)だけを受け、`rendering_passes` の無い top-level の `passes` / `compute_tasks` は `render.json_not_normalized@1` です。拒否は名前付きのコード + 対象名の `GraphRegistryError` で、graph 名の空・重複(`render_graph.graph_name_invalid@1`)と render target 名の重複(`render_graph.image_invalid@1`、`reason: duplicate`)もここです。material resource の `same_pixel` と cube view / subresource の組み合わせ、compute の未知の schedule と壊れた ray query の AS など、実行用 parser が断っていた物は parse で前倒しに断ります。`ray_task_acceleration_structures` と ✅RC-3c2b1 の `ray_task_callable_records` は形と値を持つだけで、表が指す task の解決と適用は最終の改訂の `resolveRenderGraphReferences()` です(provider が後で task を足してよいため)。`PELICAN_WITH_IMGUI` の違いは引数の `PassFieldOwnershipCapabilities` で受けます。呼んだ回数は [`renderGraphParseCount()`](../../src/project/rendergraphparse.hpp#L88)(thread_local)で数えられます。✅RC-3c2b2 から pass の実行用の値もここで読み、形の誤りを `render_graph.node_field_invalid@1` で断ります(下の「[pass の道](#pass-ir)」)。省略可の引数で `PassShapePolicy`(既定 `defaultPassShapePolicy()`)と通知 `PassShapeDiagnosticSink` を受け、Studio の widget と test の補助も同じ parser で policy を当てます。graph の `name` の欠落(`render_graph.graph_name_invalid@1`、`reason: missing`。前は黙って `frame_graph` を補った)、`passes` の欠落(`render_graph.section_invalid@1 {graph, field: passes}`。共有の compute task があっても)、実行 pass の名前の空・重複(`render_graph.node_name_empty@1` / `render_graph.duplicate_node@1`)も parse が断ります。
  - graph の node は passes → config の compute task の写しの順に並びます([`append_shared` の定義](../../src/project/rendergraphparse.cpp#L2053))。graph 自身の `compute_tasks` は ✅RC-3c2b1 から `render_graph.graph_declaration_unsupported@1 {graph, field: compute_tasks}` で断ります(下の「compute / RT task の道」)。名前は config の宣言だけを [`findConfigImage()`](../../src/project/rendergraphir.hpp#L383) / `findConfigBuffer()` で引きます(✅RC-3c2a。graph ローカル → config の順に引いた `findImage()` は消えました)。
  - **資源の宣言(✅RC-3c2a)**: image は [`parseConfigImages()`](../../src/project/rendergraphparse.cpp#L1579)、buffer は [`parseBuffers()`](../../src/project/rendergraphparse.cpp#L1770) が読み、core の `parseRenderTargetDefinitionsFromJson` / `parseFrameGraphBufferDefinitionsFromJson` は消えました。buffer の拒否は `render_graph.buffer_invalid@1 {resource, field, reason}` で、旧 parser の拒否に、重複名(`name` / `duplicate`)、`size_from_extent.resource` が config image でない(`unknown_image`)、明示した空の `initial_f32`([`empty`](../../src/project/rendergraphparse.cpp#L1885)。省略は 0 で埋める)を足しました。負の size / tile は `requires_unsigned_integer` です(旧は uint64 へ折り返していた。実装者の報告 §13 の 6)。宣言した size の契約(index の uint32 境界・runtime_f32 の float 境界・`initial_f32` の個数・view family の uvec4・host source の非 0・command の最小・scene 系の record の倍数)は [`requireBufferSizeContract()`](../../src/project/framegraphbufferdefinition.cpp#L95) で parse が、`size_from_extent` の size の契約は資源の記述が検査します。graph の `render_targets` / `buffers` は [`render_graph.graph_declaration_unsupported@1 {graph, field}`](../../src/project/rendergraphparse.cpp#L2090) です(作者の手書きでしか作られず、登録もされていなかったため。実装者の報告 §8)。snapshot の copy 先の解決済みの format が copy の契約の外なら、parse の最後で `render_graph.snapshot_size_unavailable@1 {graph, node, resource}` です(preview も parse を通るので断られる)。
- **本番の呼び手**(`src` の検索で確認):
  - compiler の runtime の variant: [`compileDefaultLogicalVariant()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L212) が ImGui の追加の後に 1 回 parse し([改訂 0](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L296)。✅RC-3c2b2 からこの IR を `compileRenderPipeline()` にも渡す)、その IR を [`resolveLogicalGraphTransforms()`](../../src/core/renderingpass/graphtransformregistry.cpp#L1192) → [`resolveTaggedSubgraphReplacements()`](../../src/core/renderingpass/subgraphreplacementregistry.cpp#L1204) に渡します。両者は応答を正規化して 1 回 parse し([transform の応答](../../src/core/renderingpass/graphtransformregistry.cpp#L1268)、[splice 後の config](../../src/core/renderingpass/subgraphreplacementregistry.cpp#L1351))、その IR を次の要求の「前」にして、最終改訂の JSON と IR を返します。provider に渡す論理 graph は IR から `toLogicalGraph()` で作ります([`logicalGraphsOf()`](../../src/core/renderingpass/graphtransformregistry.cpp#L238))。回数は variant ごとに `1 + transform の応答の数 + splice の数` です。
  - compiler の preview(data_only): compose の直後に 1 回 parse して [`resolveRenderGraphReferences()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L317) を通します。preview の try の中なので、ここで断れば preview だけが unavailable です。registration は preview の variant の IR を [`makePreviewGraphProgram()`](../../src/core/renderingpass/previewgraph.cpp#L117) に渡し、`pass_names` は IR の node(`scope == graph` で compute でも anchor でもない物、宣言順)から取ります。generation は ✅RC-3c2b2 から IR の全欄の指紋 [`renderGraphFingerprint()`](../../src/project/rendergraphfingerprint.cpp#L167) の下位 53 bit(0 なら 1。前は合成済みの JSON の hash)で、graph 名・node 名の空・重複はここで `render_graph.preview_invalid@1 {graph, node?, reason}` です。
  - editor: render target の使用者 [`renderTargetUsers()`](../../src/core/communication/renderconfigeditor.cpp#L891)(全 node の `uses[].resource`)、restart の前の検査 [`restartGraphRejection()`](../../src/core/communication/renderconfigeditor.cpp#L1429)(graph ごとに `toLogicalGraph()`)、planning の名前 [`planningNamesOf()`](../../src/core/communication/renderconfigeditor.cpp#L3355)(IR の宣言・node・書く use・swapchain を読む use)。
  - variant ごとの IR は `RenderCompilerProgramVariantOutput::render_graph`([欄](../../src/core/renderingpass/rendercompilerprogram.hpp#L138)。runtime は finalize 後、preview は parse 後、unavailable は null)に載り(✅RC-3c2b2 から available な出力には必須で、出力の検証が IR の graph 名と runtime の `frame_plans` の鍵の一致を `render_graph.compiler_output_invalid@1 {variant, reason, graph?}` で見る。[検証の所](../../src/core/renderingpass/rendercompilerprogram.cpp#L130))、program を横断する `frame_node_order` の graph 名の検査がこれを読みます([横断の検査](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L654)。`render_graph.frame_node_order_invalid@1` / `render_graph.frame_node_order_unknown@1 {graph}`)。
- **finalize**: [`finalizeRenderGraph()`](../../src/project/rendergraphir.cpp#L1046) は provider の列の後に 1 回、次の順に呼びます。
  1. config の image の format が全部解決済み(`render_graph.image_format_unresolved@1`)。
  2. [`resolveRenderGraphReferences()`](../../src/project/rendergraphir.cpp#L905): ✅RC-3c2b1 から [`applyRayTaskTables()`](../../src/project/rendergraphir.cpp#L823) が 2 つの表(AS と callable record)を作者の task 名で全 graph の写しに当て(表を当てるのはここだけ。表の task の存在・ray work・callable の schema は `render_graph.compute_task_invalid@1 {node, field, reason}`)、config の task と raster 系の pass が選ぶ AS の存在([`requireSelectedAccelerationStructuresDeclared()`](../../src/project/rendergraphir.cpp#L881)、`render_graph.acceleration_structure_unknown@1`。作者の task 名で、接尾辞の前)。
  3. [`requireRenderGraphRayQueryCapability()`](../../src/project/rendergraphir.cpp#L775)(scene を問い合わせる物があれば各 graph の target planning に `pelican.vulkan.ray_query@1`)。
  4. [`namespaceSharedComputeTasks()`](../../src/project/rendergraphir.cpp#L737)(variant の接尾辞。重複・衝突は名前付き)。
  5. [`insertAccelerationStructureNodes()`](../../src/project/rendergraphir.cpp#L912)(`__scene_acceleration_structure` と名前付きの分を各 graph の先頭に入れ、問い合わせる node を待たせ、宣言順を振り直す。予約名は `render_graph.acceleration_structure_reserved@1`)。ここが断るのは予約名だけで、選ばれた AS の存在は見ません。存在の検査は 2. の `resolveRenderGraphReferences()` の 1 か所で、作者の task 名のまま(接尾辞の前に)済んでいます(✅RC-3c1b。ここに残っていた同じ検査の呼び出しは確認レビューで消えました。未知の AS の拒否を関数を直に呼んで見る test も、`insertAccelerationStructureNodes()` ではなく `resolveRenderGraphReferences()` を呼ぶ形に変わりました)。
  6. [`applyFrameNodeOrder()`](../../src/project/rendergraphir.cpp#L996)(並びの隣どうしに `after`。前に FO-1 の照合。variant ごとは `require_all_graphs = false`)。
- **compiler の C1 の順**([`compileDefaultVulkanVariant()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L368)): 最終 IR → [`renderTargetDefinitionsOf()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L410)(✅RC-3c2a。IR の宣言から `RenderTargetDefinition` を作る。前は render target の実行用 parser)→ LP-Va / VF-1 / VF-2(`frameGraphDefinitionOf()` の結果で)→ ray bindless の graph 名 → [finalize](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L449)(backend の output extent を渡し、snapshot の大きさを決める)→ [`synchronizeRenderGraphProvenance()`](../../src/project/rendergraphir.cpp#L531)(✅RC-3c2b1。IR の改名の対応で pass / resource の provenance を同期する。前はここで compute task の実行用 parser と `detail::namespaceComputeTaskDefinitions()` が JSON を読んでいた)→ graph ごとに [`planRenderGraph()`](../../src/project/rendergraphir.cpp#L376) と [`requireRenderGraphInputsProduced()`](../../src/project/rendergraphir.cpp#L380) → [`targetViewExecutionRequest()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L35)(IR の `renderGraphMultiviewCapableNodes()` と node の shape・`schedule`・`view_family` から)→ 物理計画 → [`imagePlacementsOf()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L546)(physical package の `image_placements`)。planner は物理計画より前にあり、不正な graph は planner の名前付きの code で先に断られます。RC-3c1b の時点でここにあった buffer の実行用 parser と `validateGpuDrawSourceBufferContracts` は消え、buffer の容量の検査は下の仕上げ(`completeGenerationResources()`)に移りました(`size_from_extent` の size が決まった後で検査するため)。
  - **入力の産出**: `requireRenderGraphInputsProduced()` は graph の pass(`scope == graph`、compute でない)の history でない `input` と snapshot の `source` だけを見て、plan でその前に書く node が無ければ `render_graph.input_not_produced@1 {graph, node, resource}` です。config の buffer と、`initial`(IA-1)/ file の `source`(EN-2)を持つ config の image は frame の前から中身があるので通します。compute の `reads` は対象外です(persistent image を読み書きする compute を断らないため)。
- **途中の改訂の検査と最終の検査**: planner の拒否 8 種のうち、順の決まらない 2 writer(`render_graph.ambiguous_writes@1`)だけは最終の `frame_node_order` で合法になりえます。[`toLogicalGraph()`](../../src/project/rendergraphir.cpp#L723) は論理 graph を作る前に [`requireFrameGraphStructure()`](../../src/project/frameplan.cpp#L477)(2 writer 以外の 7 種)だけを呼ぶので、provider の前後と editor の restart の検査はこの 7 種を planner の code で断り、2 writer は finalize の後の C1 の `planRenderGraph()`([`planFrameGraph()`](../../src/project/frameplan.cpp#L473)、8 種)だけが断ります。
- **論理 graph と物理計画**: 論理 graph の型は [`renderGraphShadowOptions()`](../../src/project/rendergraphir.cpp#L680) が IR の `ImageResource` から決めます。tile-local は、device が持てる (image, format) の集合 [`renderGraphDeviceTileLocalFormats()`](../../src/project/rendergraphir.cpp#L607) と、graph ごとの適格の判定 [`renderGraphTileLocalFormats()`](../../src/project/rendergraphir.cpp#L637) の 2 段です。device への問いは中立な [`RenderGraphDeviceFacts`](../../src/project/rendergraphir.hpp#L495)(format・usage・cube 互換を受けて対応するかを返す関数)で、core の [`renderGraphDeviceFactsOf()`](../../src/core/renderingpass/renderingsamplecount.cpp#L811) が `queryFormatCapability()` から作ります。
  - [`compileRenderingTargetPlans()`](../../src/core/renderingpass/renderingsamplecount.cpp#L1513) は IR と、IR から作った `RenderTargetDefinition`(`renderTargetDefinitionsOf()`)を受け、graph ごとに `frameGraphDefinitionOf()` を作って([作る所](../../src/core/renderingpass/renderingsamplecount.cpp#L1535))部品(attachment の資源・物理 attachment・geometry node・resolution plan・once の制約)に渡し、論理 graph は [`toLogicalGraph(IR, graph, {device})`](../../src/core/renderingpass/renderingsamplecount.cpp#L1650) です。部品を IR に直すのは物理計画を直す段です。core の `shadowOptions` / `graphTileLocalAttachmentFormats` は消えました。
  - **適格の判定は 1 つ**: [`ImageDeclarationFacts`](../../src/project/rendergraphir.hpp#L458) の上の project の定義(`isTransientAttachmentCandidate` / `isTileLocalAttachmentCandidate` / `isRuntimeAliasEligible` / `nodeHasSamePixelRead`)が正本です。core は `RenderTargetDefinition` → facts の写しを [`declarationFactsOf()`](../../src/core/renderingpass/renderingsamplecount.cpp#L53) の 1 か所に持ちます。物理計画の後の alias の使い手は [`isRuntimeAliasCandidate()`](../../src/core/renderingpass/renderingsamplecount.cpp#L83) で、宣言の適格に `storage_mode == materialized` を足した物です(project の側は物理計画の前の前提で、storage mode を持たない)。
  - **順は planner から必須の入力(✅RC-3d2)**: node の順を決めるのは planner だけで、物理計画は選び直しません(設計 §2.6 の RC-3d の追補)。
    - **入口**: [`compileRenderingTargetPlans()`](../../src/core/renderingpass/renderingsamplecount.hpp#L167) / `compileRenderingTargetPlansForVulkanDevice()` は graph 名 → `FramePlan` の表を必須の引数に取り、compiler の 2 分岐は planner が作った `frame_plans` をそのまま渡します([device の分岐](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L510))。表に graph が無ければ `target_plan.node_order_missing@1 {graph}` です([断る所](../../src/core/renderingpass/renderingsamplecount.cpp#L1665))。
    - **順の型**: 下の層の [`VulkanTargetPlanRequest`](../../src/project/targetrenderplanning.hpp#L337) は [`FramePlanNodeOrder`](../../src/project/frameplan.hpp#L208) を必須の欄で持ちます。この型は既定構築できず、`FramePlan` から作るか、名前の列を論理 graph に照らして作ります([照らす所](../../src/project/frameplan.cpp#L492)。名前の集合が違えば `target_plan.node_order_mismatch@1`、論理 graph の data / `after` / `before` に反すれば `target_plan.node_order_conflicts_dependency@1`)。空の graph の空の順は有効で、「空の列 = 欠落」とは扱いません。
    - **使う所**: [`makeTargetLoweringGraph()`](../../src/project/targetrenderplanning.cpp#L1625) が lowering の node をこの順で並べ(lifetime・alias・候補の評価より前)、analyzer の 2 回の呼び出しも同じ順を受けます。物理計画の中の名前順の topological sort は消えました。analyzer の report と dump の `planning_opportunities.node_order` は入力の順です。
    - **complete plan(と RC-6b2 の前の fragment)**: v3 の `dependency_safe` の fragment は順を保つ分割と隣り合う scope の融合だけを受け、展開順が違えば `applyScopePartition()` が `physical_fragment.reorders_frame_plan@1` で断りました(fragment ごと ✅RC-6b2 で消えた)。complete plan の verifier は期待順を必須の引数に取り、stale(`complete_physical_plan.stale_automatic_plan@1`)を backend 候補の比較より先に見てから順を照らします([その所](../../src/project/vulkancompletephysicalplan.cpp#L1586))。install の [`installVerifiedVulkanCompletePhysicalPlanPackage()`](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L911) は verification context に残した順を verifier に渡します。利用者向けの形は manual 第6章「[旧い pin と physical fragment](../manual/06_rendering.md#legacy-plan-packages-removed)」。
    - **共通の仕上げと直接の登録**: physical package を持つ variant の全 graph を、backend 中立の問い [`physicalNodeOrder(graph)`](../../src/core/renderingpass/rendercompilerprogram.hpp#L54)(展開した node の列と scope ごとの半開区間の `first` と `count`。Vulkan は [`physicalNodeOrderOf()`](../../src/project/targetrenderplanning.cpp#L3713))で FramePlan の順と照らします([その所](../../src/core/renderingpass/rendercompilerprogram.cpp#L297)。`validateVariantOutput()` の後、`completeGenerationResources()` の前)。差し替えた compiler program の道を守る最後の網で、共通の仕上げを通らない直接の登録は上の「計画と実行 ID の結合」の `compileFrameGraphScopeMapping()` が同じ関数で断ります。
    - **PF-1**: [`passfusion.cpp`](../../src/project/passfusion.cpp#L359) の独自の順は実行しない構造の解析なので RC-3d では触っていません。PF-2 で実行に入れるときは FramePlan の順と上の前計算を入力に取る、が設計の申し送りです。
<a id="generation-resources"></a>

- **generation の資源の記述(✅RC-3c2a)**: 世代が登録する image と buffer の値は、全 runtime variant を 1 つにまとめた [`GenerationResources`](../../src/project/generationresources.hpp#L123)(`src/project/generationresources.{hpp,cpp}`、`pelican_project`)が持ち、資源の検査と GPU 登録はこれだけを読みます(設計 §2.4)。
  - **入力**: variant ごとの [`GenerationResourceVariant`](../../src/project/generationresources.hpp#L93)(名前・確定 IR・[`ImagePlacement`](../../src/project/generationresources.hpp#L76) の列・runtime に層が変わる image の名前)と output extent。`ImagePlacement` は物理計画が config image ごとに決めた format・samples・layers・storage・alias group・local read・外部 depth の export で、core の `RenderingTargetPlanCompilation`(vk を引くので core に残る)から [`imagePlacementsOf()`](../../src/core/renderingpass/renderingsamplecount.cpp#L2073) が抜き出します。runtime に層が変わる image は shadow の契約の target([`runtimeLayeredImagesOf()`](../../src/project/generationresources.cpp#L501))。
  - **出力**: config image ごとの [`GenerationImage`](../../src/project/generationresources.hpp#L100)(宣言・format・最終 usage・解決済み extent・samples・layers・storage・alias group・persistent・`runtime_array_layers`)と、大きさの決まった buffer の列と、variant ごとの index の列(宣言順 = 登録の順)。
  - **統合**([`resolveGenerationResources()`](../../src/project/generationresources.cpp#L97)): usage は宣言の和、layers は最大、storage は食い違えば materialized、persistent はどれか 1 つが persistent なら。format・mip_levels・dimension・extent・samples・history・source の食い違いは `render_graph.image_variant_conflict@1 {resource, field, variants}`。最終 usage は storage を統合した**後**に [`engineImageUsage()`](../../src/project/generationresources.cpp#L16) で 1 回計算します(flat の transient depth と XR の materialized depth は materialized の usage で、transient の bit は残らない)。extent は [`resolveResourceExtent()`](../../src/project/targetrenderplanning.cpp#L1551)。placement は runtime variant の全 config image に 1 つずつ要り、欠け・重複・知らない名前は `render_graph.image_placement_invalid@1 {variant, resource, reason}` です(空の集合も断る)。
  - **alias の group**([判定](../../src/project/generationresources.cpp#L174)): 全 variant が全 member を group に置いた group は残り、一部の member だけを持つ(か member を group の外に置いた)variant がある group は全 member の共有を解除し、member を 1 つも持たない variant は数えません。runtime に層が変わる image を含む group も解除します。最後に残った group が 2 人以上で、format・usage・extent・samples・mips・layers・dimension・storage(materialized)が一致し history でないことを確かめ([検査](../../src/project/generationresources.cpp#L233))、違えば `render_graph.alias_group_invalid@1 {group, reason: member_count | contract, resource?}` です。前は target ごとに比べたので、片方の variant に無い member が 1 人の group として残りえました。
  - **runtime に層が変わる image**: 記述の層は物理計画の値(静的な下限)で、history・cube・file source なら `render_graph.image_runtime_layers_invalid@1 {resource, reason: history | not_2d | file_source}`。light の数で層が変わったときの検査は runtime に残ります。
  - **buffer**([統合](../../src/project/generationresources.cpp#L255)): 同名の buffer は全欄([`operator==`](../../src/project/framegraphbufferdefinition.cpp#L55)。`initial_f32` は `std::bit_cast` のビットで比べ、+0.0 と -0.0 は別)が一致すれば 1 つ、違えば `render_graph.buffer_variant_conflict@1 {resource, field, variants}`(`field` は size → size_from_extent → lifetime → host_source → command_layout → view_family → index_buffer → initial_f32 の順で最初に違う欄)。`size_from_extent` の buffer は `resource` が指す config image の**解決済み extent** から [`extentDerivedBufferBytes()`](../../src/project/framegraphbufferdefinition.cpp#L84)(tile の切り上げ除算、uint64 の溢れは `size_from_extent` / `overflow`)で大きさを決め、`requireBufferSizeContract()` を当てます。前は「width / height を持つ最初の target」を基準に double で計算していました(実装者の報告では、固定 64×64 の image を足した `hybrid_v1 + clustered_lighting` の構成で 2,176 → 1,060,896 byte に変わる。出荷の 7 project では変わらない)。
  - **仕上げと呼ぶ位置**: [`completeGenerationResources()`](../../src/core/renderingpass/rendercompilerprogram.cpp#L223) は [`runRenderCompilerProgram()`](../../src/core/renderingpass/rendercompilerprogram.cpp#L274) の最後に 1 回(その直前に同じ関数が variant ごとに `computeTaskDefinitionsOf()` を呼ぶ。下の「compute / RT task の道」)呼ばれ、physical package を持つ variant(runtime。preview は入らない)について `resolveGenerationResources()` → variant ごとに ✅RC-3c2b1 から IR 版の [`validateComputeTaskBufferContracts()`](../../src/project/rendergraphir.cpp#L491)・IR 版の `validateGpuDrawSourceBufferContracts()` → [`validateRenderGraphResources()`](../../src/project/generationresources.cpp#L485) → ✅RC-3c2b2 から `renderingPassDefinitionsOf()`(記述の index を仮の ID にする resolver で検査だけ)の順に通し、結果を `RenderCompilerProgramResult::generation_resources` に入れます(✅RC-3c2b2 から program の `compile()` が返す `RenderCompilerProgramOutput` とは別の型で、program は仕上げの値を書けない)。output extent は backend context の `outputExtent()`([`vulkanrendercompilerpackage.hpp`](../../src/core/renderingpass/vulkanrendercompilerpackage.hpp#L64))で、registration は登録する extent(`base_extent`)を渡します。registration はこの結果から variant ごとに [`renderTargetDefinitionOf(const GenerationImage &)`](../../src/core/renderingpass/rendertargetdefinition.cpp#L41) と buffer の列を作り([作る所](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1103))、`registerRenderTargetDefinitions` / `registerBuffers` に渡すだけです(`mergeVariantRenderTargetPhysicalRequirements` は消えた)。pass の定義は ✅RC-3c2b2 から確定 IR と記述から `renderingPassDefinitionsOf()` が registry の ID で作ります(前は pass の実行用 parser が ID を解決し、metadata を記述から [`renderTargetMetadataOf()`](../../src/core/renderingpass/rendertargetdefinition.cpp#L53) で受けていた。この関数の src の呼び手は今は無く、GPU の test だけが使う)。
  - **資源の検査**: [`validateRenderNodeResources()`](../../src/project/generationresources.cpp#L302) は pass の parse にあった検査(`renderingpassvalidation.cpp` の usage・extent・samples・material attachment と、`passsequencejsonparser.cpp` の snapshot の usage・format・extent)を、variant の IR の use と記述の上に移した物です。code は `render_graph.image_usage_missing@1` / `history_input_invalid@1` / `attachment_resource_invalid@1` / `snapshot_invalid@1` / `material_input_invalid@1` / `material_attachment_invalid@1`([一覧のコメント](../../src/project/generationresources.hpp#L168))。use の名前が image かどうかは、その variant の IR の config image で決めます(別 variant の同名の image を buffer の読みに使わない)。省略可の observer(`ResourceCheckObserver`)が、検査が引いた image を引いたとおりに渡します(C1 の test の口)。material の format の表は project の [`materialattachmentformats.hpp`](../../src/project/materialattachmentformats.hpp#L20) で、core の `materialpassattachments.hpp` は vk への写しだけです。
  - **editor 用の入口**: [`resolveDeclaredGenerationResources()`](../../src/project/generationresources.cpp#L491) は 1 variant の各 image を宣言どおり(single-sample・materialized・alias なし)に記述します。editor の G-1([`validateAuthoredMaterialPass()`](../../src/core/communication/renderconfigeditor.cpp#L690))は 1920×1080 でこれを作って `validateRenderNodeResources()` と ✅RC-3c2b2 から `passDefinitionOf()` を足した node だけにかけ、`edit_render_targets` は形の検査を `renderTargetDefinitionsOf(parseRenderGraph(...))`([その所](../../src/core/communication/renderconfigeditor.cpp#L6491))にしました。どちらの拒否も名前付きの code です(manual [第13章](../manual/13_editor.md) の「render target の宣言」と「material pass の追加と route の拒否」)。
  - **snapshot の大きさ**: [`finalizeRenderGraph()`](../../src/project/rendergraphir.cpp#L1046) が copy 先の image を `FinalizeRenderGraphRequest::output_extent` で解決して [`snapshotCopyBytes()`](../../src/project/rendergraphir.cpp#L1040) で決めます。前は parse の時点で「固定サイズの display」を基準に double で計算していたので、端数の scale で実物の image より小さくなりえました(1920×1080・scale 0.3333333333・4 byte で 917,604 → 921,600。実装者の報告)。
<a id="compute-task-ir"></a>

- **compute / RT task の道(✅RC-3c2b1)**: compute task の JSON を読むのは `parseRenderGraph()` の [`parseComputeTask()`](../../src/project/rendergraphparse.cpp#L1458) だけで、registration が登録する `ComputeTaskDefinition`(型と読む側は残る。設計 §5 の利用者の決定)は確定 IR から作ります。
  - **IR の欄**: [`ComputeNodeInfo`](../../src/project/rendergraphir.hpp#L208) は `shader`・`ray_tracing`([`RayTracingTaskDeclaration`](../../src/project/computetaskdeclaration.hpp#L120))・`ray_query_acceleration_structure`・`dispatch`([`ComputeDispatchDefinition`](../../src/project/computetaskdeclaration.hpp#L43)。indirect の `offset` を含む)を持ちます。RT の宣言は書かれたとおりの形(鍵の列、layout の無い形の miss / closesthit と layout の形の traced_ray_types / misses / hit_shaders の両方、型が違う値は `Form::other`)で持ち、どちらの形が正しいかは core が project の ray group layout に照らして決めます(`parseRenderGraph()` は layout を知らないため)。callable・callable record(schema と詰めた record)・max_recursion・acceleration_structure の形は parse が見ます。reads / writes / resource_ports は今までどおり `uses` です。
  - **作る関数と置き場**: core の [`computeTaskDefinitionsOf()`](../../src/core/renderingpass/computetaskdefinition.cpp#L451)(`src/core/renderingpass/computetaskdefinition.{hpp,cpp}`)が config の task ごとに 1 つ、宣言順に作ります。名前は IR のとおり(variant の接尾辞を含む)、RT の拒否は作者名です。layout による shader の解決は [`layoutRayTracingShaders()`](../../src/core/renderingpass/computetaskdefinition.cpp#L171)(layout の無い形は [`legacyRayTracingShaders()`](../../src/core/renderingpass/computetaskdefinition.cpp#L154))で、旧と同じ順・code・文です。RT の task の launch(rays / rays_from / indirect)と compute の task の `shader` の有無は、ここ(実行の側)で `pelican.ray_tracing.trace_extent_required@1` / `render_graph.compute_task_invalid@1 {field: shader, reason: requires_string}` として要求します(計画だけの fixture と test は shader の無い task を計画するため)。順は持ちません: RC-3c2b1 では `after` / `before` に全 graph の写しの最終の辺(改名後・AS の build 待ち・`frame_node_order`)を集めていましたが、読む所が src に無く、✅RC-4 で欄ごと消えました(正本は IR の辺と FramePlan)。`ComputeTaskDefinition` の型そのものは ✅RC-4 から project の [`computetaskdefinition.hpp`](../../src/project/computetaskdefinition.hpp#L62) にあり、core の `computetaskdefinition.{hpp,cpp}` に残るのは作る関数と RT の shader の解決です(§6.7 の「[実行用の leaf の型](#execution-leaf-types)」)。
  - **呼ぶ位置**: [`runRenderCompilerProgram()`](../../src/core/renderingpass/rendercompilerprogram.cpp#L274) が出力の検証の後、runtime の variant ごとに呼び、`RenderCompilerProgramResult::compute_task_definitions` に入れてから `completeGenerationResources()` を呼びます。✅RC-3c2b2 から、この欄は program の `compile()` が返す `RenderCompilerProgramOutput`(`variants` だけ)には無く、仕上げの型 [`RenderCompilerProgramResult`](../../src/core/renderingpass/rendercompilerprogram.hpp#L149) だけにあるので、program は compute の定義を返せません(前は `compile()` の型にもあり、差し替えの program が詰めると数が合わず名前の無い文で断られていた)。登録される値は必ず IR から作った物です。registration は今も記述と compute の列の数が runtime variant と同じことを確かめます(違えば `Render compiler program completed a different set of runtime variants`)。registration は列の数を確かめた後、`loopIndicesOf()` で `loop_index` を入れて `compileComputeTasks()` に渡します([渡す所](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L451))。
  - **入口の検査**: [`sharedComputeTasksOf()`](../../src/project/rendergraphir.cpp#L432) は `computeTaskDefinitionsOf()` と buffer の検査の共通の入口で、IR が task を作り切れる形か(作者名の列と索引の一致、各 graph に写しが 1 つずつ、索引に無い shared node が無い、compute node はすべて shared、写しの実行の値が辺と位置を除いて同じ)を `render_graph.compute_task_invalid@1 {node, field: shared_compute_tasks, reason, graph?}` で見ます(reason は `authored_names_mismatch` / `duplicate` / `missing_copy` / `unindexed` / `not_a_compute_task` / `graph_scope` / `not_shared` / `copies_differ`)。
  - **表**: AS の表と callable の表を当てるのは [`applyRayTaskTables()`](../../src/project/rendergraphir.cpp#L823) の 1 か所で、`resolveRenderGraphReferences()` から呼ばれます(runtime も preview も通る)。callable の inline の record は parse で検査して詰め、表の値は最終の改訂でその task の schema で詰めて置き換えます。inline が不正なら表で上書きされても断ります。callable の表の拒否は `render_graph.compute_task_invalid@1 {node, field: ray_task_callable_records, reason: not_an_object | unknown_task | task_declares_no_callable_record}` です。callable の schema・値・packing の部品は project の [`rayrecordabi.hpp`](../../src/project/rayrecordabi.hpp#L23) で、core の `raygrouplayout.cpp` は hit group の record の packing もこれで行います。適用の回数は [`renderGraphApplicationCounts()`](../../src/project/rendergraphir.cpp#L868)(thread_local)で数えられます。
  - **provenance**: compiler が finalize の後に [`synchronizeRenderGraphProvenance()`](../../src/project/rendergraphir.cpp#L531) を 1 回呼びます([呼ぶ所](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L471))。config の task は `authored_shared_compute_tasks[i]` → `shared_compute_tasks[i]` の対応で既存の entry を作者名で引くので、`authored_name`・feature・graph / loop origin が改名の後も残ります(`loop_index` はこの `loop_origin` から作られる)。
  - **dispatch の拒否**: [`computeDispatch()`](../../src/project/rendergraphparse.cpp#L1326) は形を読む前に鍵を見て、groups / groups_from / local_size / rays / rays_from / indirect 以外を RT の task でも `render_graph.compute_task_invalid@1 {node, field: dispatch.<key>, reason: unknown_field}` で断ります。groups の負の値・uint32 を超える値は `requires_positive_uint32`、object の未知の軸は `unknown_axis`、indirect の未知の鍵は `unknown_field`、負の offset は `requires_unsigned_integer`、4 byte 境界でない offset は `not_4_byte_aligned` です(前は無視・折り返し・切り詰めで受けていた物を含む)。RT の task の空の dispatch(`{}`)と groups だけの dispatch は parse を通り、launch が無ければ上の実行の側が断ります。indirect の buffer の検査(task が書く・未知・command layout・整列・容量)は資源の記述の後の [`validateComputeTaskBufferContracts()`](../../src/project/rendergraphir.cpp#L491) で、`render_graph.compute_task_invalid@1 {node, field: dispatch.indirect.buffer | dispatch.indirect.offset, reason, buffer}` です。不正な schedule の名前は parse の `reason: not_per_frame_per_view_or_once`、3 つ以外の `NodeSchedule` の値は [`nodeScheduleName()`](../../src/project/nodeschedule.cpp#L7) の `render_graph.node_schedule_unknown@1 {value}` です。
  - **既定値**: compute の欄の既定は project の [`computetaskdeclaration.hpp`](../../src/project/computetaskdeclaration.hpp#L24)(group 数 1 と `[1, 1, 1]`、max_recursion 1、AS `"scene"`)と [`nodeschedule.hpp`](../../src/project/nodeschedule.hpp#L20)(`per_frame`)の 1 か所で、parse・core の実行の型の既定・`graphparameterschema.cpp` の `render.compute` の fallback・Studio の新しい task の草稿がこれを読みます。core の `ComputeTaskSchedule` は `NodeSchedule` の別名です。
<a id="pass-ir"></a>

- **pass の道(✅RC-3c2b2)**: pass の JSON を読むのは `parseRenderGraph()` だけで(計画の値に加えて、実行用の値を [`passExecutionDeclaration()`](../../src/project/rendergraphparse.cpp#L747) と [`passShader()`](../../src/project/rendergraphparse.cpp#L711) が読む)、registration が登録する `RenderingPassDefinition` / `PassDefinition`(型と読む側は残る。設計 §5 の利用者の決定)は確定 IR から作ります。core の pass の実行用 parser(`parseRenderingPassDefinitionsFromConfigJson` から下の 13 組、`passinfojsonparser.cpp`・`materialpassinfojsonparser.cpp`・`renderingpasstargetjsonparser.cpp` など)、core の `passshaderdefines.{hpp,cpp}`、`RenderCompilerProgramVariantOutput::normalized_config` と `buffer_names` は消えました(src に 0 件)。
  - **IR の欄**: Fullscreen / Raster / 補助 pass の info は共通の基底 [`PassShaderDeclaration`](../../src/project/passdeclaration.hpp#L35)(`shader_declared` = `shader` を書いたか、vertex / skinned_vertex / fragment の `optional`)を持ちます。`shader` の省略と `"shader": {}` はこれで区別され、既定の shader は IR に入りません。[`FullscreenNodeInfo`](../../src/project/rendergraphir.hpp#L165) は `implementation_provider`・`raster_state`(`optional<PipelineState>`。書かなければ nullopt)・`push_constants`・`uses_light_data`・`input_sampling`・`shader_defines`、[`RasterNodeInfo`](../../src/project/rendergraphir.hpp#L176) は `draw_declared`・`contract`(`RasterPassContract`)・`shader_implementation`・`shader_defines`、[`AuxiliaryNodeInfo`](../../src/project/rendergraphir.hpp#L185) は基底だけ、[`MaterialNodeInfo`](../../src/project/rendergraphir.hpp#L189) は `output_states` と `material_start` / `material_count` を足しました。node の共通欄は [`NodeIR`](../../src/project/rendergraphir.hpp#L252) の `clear_color`・`color_clear_values`(location 順。`clear_colors` の省略は空、`{}` は各 color 出力に `clear_color` を展開)・`input_settings`・`shader_declaration_parsed` です。resource port と material の binding は今までどおり `uses` から導きます。
  - **形は parse、必須は実行の側**: 型・範囲・未知の鍵・組み合わせは parse が `render_graph.node_field_invalid@1 {graph, node, field, reason, detail?}` で断ります(共有の helper の文は [`guarded()`](../../src/project/rendergraphparse.cpp#L72) が `detail` に入れる。`PassShaderDefineError` と `GraphRegistryError` はそのまま通す)。実行に要る値の必須(fullscreen の vertex / fragment、raster の `draw` / vertex / color 出力があるときの fragment、補助 pass の stage)は下の `passDefinitionOf()` が `render_graph.pass_execution_invalid@1 {graph, node, field, reason}` で断ります。計画だけの fullscreen(shader の無い planner の fixture)と `draw` の無い raster は parse と計画を通ります。
  - **作る関数**: core の [`passDefinitionOf()`](../../src/core/renderingpass/passdefinitionfromir.cpp#L80)(`src/core/renderingpass/passdefinitionfromir.{hpp,cpp}`。[宣言と契約](../../src/core/renderingpass/passdefinitionfromir.hpp#L13))が node 1 つから `PassDefinition` を作り、[`renderingPassDefinitionOf()`](../../src/core/renderingpass/passdefinitionfromir.cpp#L279) が 1 graph(宣言順、anchor / snapshot / compute / AS の node は除く)、`renderingPassDefinitionsOf()` が全 graph を束ねます。JSON を読まず finalize もしません。target の ID は引数の `RenderTargetNameResolver` から、attachment の load / store は IR の use の解決済みの op を `RasterAttachmentView` に明示して入れ、pass の欄の op は pass の型の既定のままです(解決した値は `colorAttachmentOperations()` / `depthAttachmentOperations()` で旧と同じ。生の欄の違いは設計 §5 の「表現の差」)。screen / surface の契約は use の role / port と資源の記述の format から既存の project の関数で復元します。補助 pass の既定の shader([`passdeclaration.hpp`](../../src/project/passdeclaration.hpp#L21))は、shadow_depth / velocity / picking で `shader` を書かなかったときだけここで補います([その所](../../src/core/renderingpass/passdefinitionfromir.cpp#L232))。
  - **3 つの呼び手**(`src` の検索で確認): (1) registration([その所](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L423))が全 graph を registry の resolver で、(2) 共通の仕上げ `completeGenerationResources()`([呼ぶ所](../../src/core/renderingpass/rendercompilerprogram.cpp#L268))が全 runtime variant を [`generationRenderTargetNames()`](../../src/core/renderingpass/passdefinitionfromir.cpp#L66)(記述の image の index を仮の ID にする)で検査だけ、(3) editor の G-1 [`validateAuthoredMaterialPass()`](../../src/core/communication/renderconfigeditor.cpp#L690) が未 finalize の合成済み IR と `resolveDeclaredGenerationResources()` で足した node だけを([呼ぶ所](../../src/core/communication/renderconfigeditor.cpp#L707))。(2) があるので compile だけの道でも registration と同じ拒否が GPU 登録の前に出ます。(3) が node 1 つだけなのは、後の transform が補う別の pass の必須の stage で早く断らないためです。
  - **出力と仕上げの型**: program の `compile()` が返す [`RenderCompilerProgramOutput`](../../src/core/renderingpass/rendercompilerprogram.hpp#L141) は `variants` だけで、`runRenderCompilerProgram()` が返す [`RenderCompilerProgramResult`](../../src/core/renderingpass/rendercompilerprogram.hpp#L149) が `generation_resources` と `compute_task_definitions` を足します。buffer の名前の空・重複は IR の宣言を見て `render_graph.buffer_invalid@1 {variant, resource, field: name, reason: required | duplicate}` です(消えた `buffer_names` の index の検査の代わり)。
  - **routing**: compose は material routing の route を選ぶだけで、pass の `material_outputs` / `material_output_states` と fullscreen の `raster_state` を読みません。compiler は改訂 0 の IR を `compileRenderPipeline()` に渡し、[`deriveMaterialRoutingFromIR()`](../../src/project/renderpipeline.cpp#L1383) が route の schema / state を IR から取り、仕上げの前に [`validateMaterialRoutingAgainstIR()`](../../src/project/renderpipeline.cpp#L1387) が最終 IR と照合します(`render_graph.material_route_invalid@1 {graph, node, route, field, reason}`。[呼ぶ所](../../src/core/renderingpass/rendercompilerprogram.cpp#L304))。
  - **既定値**: pass の欄の既定は project の [`passdeclaration.hpp`](../../src/project/passdeclaration.hpp#L21)(clear `[0, 0, 0, 1]`、push constants `none`(Studio の省略は空文字)、`uses_light_data` false、補助 pass の既定の shader 5 つ、raster の shader implementation)・[`shaderresourceport.hpp`](../../src/project/shaderresourceport.hpp#L87)(sampling の linear / repeat)・[`rasterpass.hpp`](../../src/project/rasterpass.hpp#L137)(direct draw の vertex_count 3、clip_y down)・`computetaskdeclaration.hpp`(ray query の AS "scene")の 1 か所で、parse・core の実行の型・`graphparameterschema.cpp`・Studio の値の出所がこれを読みます。
  - **shape policy**: shape の検査は parse 済みの use から [`passShapeObservationOf()`](../../src/project/passshapepolicy.hpp#L153) が作る観測を `evaluatePassShape()` に渡します。Studio の widget([その所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2206))は `parseRenderGraph()` の引数で policy と通知を渡し、違反の一覧を表示します。
  - **provider の応答**: graph transform の応答は、正規化の直後に `parseRenderGraph()` を 1 回通してから ABI の保護構造を比べます([その所](../../src/core/renderingpass/graphtransformregistry.cpp#L1268))。壊れた宣言が名前の無い文に隠れず、名前付きで断られます(parse の回数は変わらない)。
  - **IR の欄の追加と指紋**: IR に欄を足すときは [`renderGraphFingerprint()`](../../src/project/rendergraphfingerprint.cpp#L167) の欄の列にも足します(preview の generation がこの指紋なので、足さないと値の変化が generation に出ない)。
- **editor の拒否の規則**: editor の preflight の拒否は 1 つの [`preflightRejection()`](../../src/core/communication/renderconfigeditor.cpp#L1386) を通ります。`GraphRegistryError` なら [`namedRefusal()`](../../src/core/communication/renderconfigeditor.cpp#L1370) で code・message・detail の欄(既にある欄は上書きしない。graph.* なら `document`)をそのまま返し、それ以外は `render_pipeline_preflight_failed` です。apply / commit の経路と変種の preflight も同じ `namedRefusal()` を使います。利用者から見た応答は manual [第13章](../manual/13_editor.md#変種と組み直しの拒否の形lp-0a--lp-0d) の「変種と組み直しの拒否の形」です。
  - Studio の Graph タブの「the graph pass that draws it runs after this pass」の句は、code が `render_graph.input_not_produced@1` で、detail の `resource` を公開中の FramePlan の graph の pass が書くときだけ足します([判定](../../src/devstudio/view/graphcanvaswidget.cpp#L8168)、[`GraphCanvasModel::graphWritesResource()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6108))。
- **core から project へ移った物**(RC-3c1a。置き場だけで中身は同じ):
  - FramePlan の型(`FramePlanNodeKind`・[`FrameGraphNodeDefinition`](../../src/project/frameplan.hpp#L76)・[`FrameGraphDefinition`](../../src/project/frameplan.hpp#L117)・[`FramePlan`](../../src/project/frameplan.hpp#L156) ほか)と planner の `planFrameGraph()`、[`validateFramePlanStructure()`](../../src/project/frameplan.cpp#L555)、[`findFramePlanUnreadOutputs()`](../../src/project/frameplan.cpp#L398)、FO-1 の [`requireFrameNodeOrderAgreesWithDependencies()`](../../src/project/frameplan.cpp#L591) → `src/project/frameplan.{hpp,cpp}`。core の [`frameplanner.hpp`](../../src/core/renderingpass/frameplanner.hpp#L5) はこれを include するだけです。planner の拒否は名前付きだけです(RC-3c1b で旧の文と `planFrameGraphWithNamedRefusals()` は 1 つになりました)。
  - 論理 graph の shadow [`compileLogicalFrameGraphShadow()`](../../src/project/logicalframegraphshadow.cpp#L39) → `src/project/logicalframegraphshadow.{hpp,cpp}`。core の [`logicalframegraphadapter.hpp`](../../src/core/renderingpass/logicalframegraphadapter.hpp#L4) は include だけです。
  - **core に残る物**: `framePlanToJson`(view family の軸の serializer。[宣言](../../src/core/renderingpass/frameplanner.hpp#L48))。registration の pass の実行用 parser(`normalized_config` を読んでいた)と gpu_draw の実行用 info の読みは ✅RC-3c2b2 で消えました(下の「[pass の道](#pass-ir)」)。compute task の実行用 parser(`parseComputeTaskDefinitionsFromConfigJson()`、`computetaskdefinitionjson.cpp` ごと)と `detail::namespaceComputeTaskDefinitions()`(`vulkanrendercompilerprogramdetail.hpp` ごと)は ✅RC-3c2b1 で消えました。render target と buffer の実行用 parser と `validateGpuDrawSourceBufferContracts` の JSON の読みは ✅RC-3c2a で消えました(`validateGpuDrawSourceBufferContracts` は IR の `MaterialNodeInfo::gpu_draw_source` を読む。✅RC-3c2b2 から project の [IR 版](../../src/project/gpudrawsource.cpp#L68))。format class の解決 `resolveRenderTargetFormatClassesV2()`(JSON → JSON。parse の前に compiler の runtime の variant と editor の G-1 が通す)は core に残ります。
- **既定の 1 か所**: 宣言の無い読みの footprint は [`undeclaredReadFootprint()`](../../src/project/logicalrendergraph.hpp#L137)(history は temporal、current は arbitrary)で、parser・core の `passDefinitionOf()`(material resource の binding。✅RC-3c2b2 の前は core の material resource の実行用 parser)・shadow が使います。render target の `source` は [`ImageSourceDeclaration`](../../src/project/imagesource.hpp#L15) と [`parseImageSourceDeclaration()`](../../src/project/imagesource.cpp#L10) で、core の `RenderTargetSource` はその別名です([`using`](../../src/core/renderingpass/rendertargetdefinition.hpp#L25))。
- test:
  - 新しい道だけの [`rendergraphir_test.cpp`](../../test/rendergraphir_test.cpp#L258)(GROUP render_planning_core、[rendergraphir_test の登録](../../test/CMakeLists.txt#L3584)): RC-3c1a の橋の test から、旧の道との比較を消して直接の検査を移した物です。出荷 project 7 本と feature の組が finalize まで通る(成功と既知の拒否の組の固定集合)、graph ローカルの宣言、LOAD の use、tile-local と device facts、multiview の候補、AS の挿入、namespace と `frame_node_order`、compute だけの ray query、history の temporal、resource port の向き、拒否(parse が断る物 / finalize が断る物 / planner が断る物を入力ごとに code と対象名で)。単一 graph の形の入力は [`wrapSingleGraph`](../../test/render_graph_ir_test_support.hpp#L35) で包みます。
  - 本番の compiler program を通す [`rendercompilerprogram_ir_test.cpp`](../../test/rendercompilerprogram_ir_test.cpp#L16)(GROUP render_pipeline、[rendercompilerprogram_ir_test の登録](../../test/CMakeLists.txt#L3648)): parse の回数が式どおり(flat / preview / xr と provider の数の組で、同じ test の中で provider の数を変える)、次の provider が直前の応答から作った改訂を受ける、`frame_node_order` が provider の足した node を指せる、拒否の code と対象名、provider が足す task を表が指す、format 未解決の image が preview の IR に届く、など。GPU の [`rendercompilerprogram_device_test.cpp`](../../test/rendercompilerprogram_device_test.cpp#L290)([rendercompilerprogram_device_test の登録](../../test/CMakeLists.txt#L3650)): registration の request 作り([`makeRenderGraphVariantRequests()`](../../src/core/renderingpass/renderingpassconfigregistration.hpp#L168))から compile までを通す物、表が AS を選ぶ物、preview が外した task を表が指す物の 3 本。
  - editor: [「RC-3c1b the editor answers a planner refusal with its code and subject」](../../test/renderconfigeditor_test.cpp#L7453)(planner の拒否と共通 enqueue の preflight が code と対象名で返る)。
  - 境界 [`render_graph_ir_boundary.cpp`](../../test/render_graph_ir_boundary.cpp#L24)(`pelican_project` だけを link、group に入れない。[境界の exe の登録](../../test/CMakeLists.txt#L3472)): JSON → IR → 3 つの IR → IR → FramePlan → 論理 graph。
  - PF-1 の融合も IR から同じに決まる: [「RC-3c1a pass fusion plans the same from RenderGraphIR's facts」](../../test/passfusion_test.cpp#L862)。
  - 資源の記述(✅RC-3c2a): 純関数の [`generationresources_test.cpp`](../../test/generationresources_test.cpp#L68)(GROUP project_planning、[generationresources_test の登録](../../test/CMakeLists.txt#L3598)。engine の usage の規則、統合と alias の group の 3 種、variant の食い違いの拒否、runtime の層、buffer の大きさ・全欄の一致を順方向と逆順で、明示した空の `initial_f32`、use が image か buffer かを variant の IR で決めること、snapshot の大きさ)。GPU の [`generationresources_device_test.cpp`](../../test/generationresources_device_test.cpp#L265)([generationresources_device_test の登録](../../test/CMakeLists.txt#L3652)。本番の registration を publish まで通す C1「the registration merges the variants' images once, for the checks and the GPU」と、light の数で shadow の層が変わる C2)。device なしの compile だけで仕上げの記述・gpu_draw の容量の拒否・`image_usage_missing`・preview の snapshot の format の拒否を見る [「RC-3c2a the compiler program completes and checks the generation's resources before any registration」](../../test/rendercompilerprogram_test.cpp#L868)。IR → `RenderTargetDefinition` と graph ローカルの宣言の拒否は `rendergraphir_test.cpp`(「RC-3c2a a render target definition is made from the IR, and needs a resolved format」「RC-3c2a IR: a graph declares no images or buffers of its own」)。
  - compute / RT task(✅RC-3c2b1): 純関数の [`computetaskdefinition_test.cpp`](../../test/computetaskdefinition_test.cpp#L99)(GROUP render_planning_core、[computetaskdefinition_test の登録](../../test/CMakeLists.txt#L3586)。dispatch の各形の受理と拒否、graph ローカルの task、callable の inline と表、XR の改名の後も RT の拒否が作者名、schedule の名前)。差し替えの program で IR を書き換えて境界の拒否と整列を見る [「RC-3c2b1 the completion refuses by name an IR whose compute tasks it cannot make whole」](../../test/rendercompilerprogram_ir_test.cpp#L406)(render_pipeline)。GPU の [「RC-3c2b1 D3 the registration receives each table and the variant's names applied once, from the IR」](../../test/rendercompilerprogram_device_test.cpp#L511)(表あり / なし × flat + xr を同じ test で流し、registration が受け取った値と適用の回数、preview の IR 自身の値を見る)と [「RC-3c2b1 the callable records' table names a task a provider adds; the preview without it is unavailable」](../../test/rendercompilerprogram_device_test.cpp#L608)。test の中で compute task を作るときは [`TestSupport::computeTaskDefinitionsOf()`](../../test/compute_task_test_support.hpp#L18)(`renderGraphOf` → `applyRayTaskTables` → `computeTaskDefinitionsOf`)を通します。
  - pass(✅RC-3c2b2): 本番の compiler program を通す [`rendercompilerprogram_ir_test.cpp`](../../test/rendercompilerprogram_ir_test.cpp#L500)(GROUP render_pipeline)に、同じ test の中で bloom の `raster_state` の有り / 無しを通して `passDefinitionOf()` の戻り値の blend と nullopt を見る [「RC-3c2b2 completion derives optional pass state and refuses missing stages」](../../test/rendercompilerprogram_ir_test.cpp#L500)、計画だけの宣言と実行の必須・補助 pass の `shader` の省略 / `{}` を見る [「RC-3c2b2 pass declarations separate shape from execution requirements」](../../test/rendercompilerprogram_ir_test.cpp#L554)、旧 parser が断っていた入力を compiler program の入口から入れて code と対象名を見る「RC-3c2b2 D2 …」の 10 本([fullscreen と補助 pass](../../test/rendercompilerprogram_ir_test.cpp#L677) から [routing](../../test/rendercompilerprogram_ir_test.cpp#L1489) まで)と named の material 入力の feedback の 1 本(受理の対照を同じ test に置く。実装者の報告の最後の記録は 11 TC / 4584 assertions)があります。出力の境界(IR の必須・graph 名と frame plan・buffer 名)は [「RC-3c2b2 compiler outputs require IR and exact runtime graph coverage」](../../test/rendercompilerprogram_test.cpp#L636)。GPU の [「RC-3c2b2 D4 preview generations identify the production IR」](../../test/rendercompilerprogram_device_test.cpp#L647)(同じ入力で同じ・0 でない・53 bit・11 通りの変更で違う)と [「RC-3c2b2 registration derives the registered blend from edited IR」](../../test/rendercompilerprogram_device_test.cpp#L722)(本番の registration まで通して登録された定義の blend を見る)。Studio の [「RC-3c2b2 Studio no push constants preserves omission through load apply and RPC」](../../test/devstudio_fullscreen_pass_test.cpp#L5654)。test の中で pass の定義を作るときは [`TestSupport::renderingPassDefinitionsOf()`](../../test/rendering_pass_test_support.hpp#L11)(normalize → `parseRenderGraph` → 宣言の資源の記述 → 本番の `renderingPassDefinitionsOf`)を通します(`renderingpass_helpers_test.cpp` の 45 本などが旧 parser の直接の呼び出しからこれに移った)。

<a id="execution-leaf-types"></a>

### 実行用の leaf の型の置き場(✅RC-4、`f191624f2`)

設計 [design_render_graph_ir](../design_render_graph_ir.md) §2.7 の RC-4 の追補。実行用の型のうち backend に依存しない leaf の型を project へ移しました。**値の持ち主は変えていません**: 新しい値の型・継承・写しは無く、core の `PassInfo` の variant・`PassDefinition`・`CompiledComputeTask`・container が移した型をそのまま持ちます。名前空間は同じ `Pelican` なので core の側に `using` はありません。挙動も変えていません。

| 型 | 置き場 | core の側 |
|---|---|---|
| `ShaderStage` / `shaderStages` / `ShaderReferenceKind` / [`ShaderReference`](../../src/project/shaderreference.hpp#L40)、[`DeclaredShaderStage`](../../src/project/shaderreference.hpp#L57) / [`DeclaredShaderReference`](../../src/project/shaderreference.hpp#L70)、[`makeShaderReference()`](../../src/project/shaderreference.cpp#L76) と stage 名・拡張子の関数 | `src/project/shaderreference.{hpp,cpp}` | [`src/core/shader/shaderreference.hpp`](../../src/core/shader/shaderreference.hpp#L4) は project の header を include するだけの互換 header(既存の include の利用者のため)。core の `shaderreference.cpp` は消えた。`DeclaredShader*` は前は core の `renderingpass.hpp` |
| [`FullscreenPassInfo`](../../src/project/passinfo.hpp#L15)・[`GenericRasterPassInfo`](../../src/project/passinfo.hpp#L39)・補助 pass の 6 種(`DebugDrawPassInfo` / `GizmoPassInfo` / `DebugTextPassInfo` / `ShadowDepthPassInfo` / `VelocityPassInfo` / `PickingPassInfo`)・`UiPassInfo` | [`src/project/passinfo.hpp`](../../src/project/passinfo.hpp#L15) | core の `renderingpass.hpp` が include して `PassInfo` の要素にする |
| [`PassLoopIndex`](../../src/project/passloopindex.hpp#L11) | `src/project/passloopindex.hpp` | `PassDefinition::loop_index` と `ComputeTaskDefinition::loop_index` の型 |
| [`RayTracingTaskShaderDefinition`](../../src/project/computetaskdefinition.hpp#L24)・`ComputeTaskSchedule`(`NodeSchedule` の別名)・[`ComputeTaskDefinition`](../../src/project/computetaskdefinition.hpp#L62)・[`computeTaskScheduleName()`](../../src/project/computetaskdefinition.cpp#L5) | `src/project/computetaskdefinition.{hpp,cpp}` | core の [`computetaskdefinition.hpp`](../../src/core/renderingpass/computetaskdefinition.hpp#L3) は中立な型と IR を include して [`computeTaskDefinitionsOf()`](../../src/core/renderingpass/computetaskdefinition.cpp#L451) を宣言する |

- **core に残る物**(理由は実装者の報告と設計 §2.7): [`MaterialPassInfo`](../../src/core/renderingpass/renderingpass.hpp#L176) と material の入力・資源の binding、`GpuDrawSourceDefinition`(generation の ID を持つ)。[`ImGuiPassInfo`](../../src/core/renderingpass/renderingpass.hpp#L207)(`PELICAN_WITH_IMGUI` は `pelican_core` の公開の compile 定義で `pelican_project` には無いので、移すと翻訳単位ごとに型が食い違う)。[`PassInfo`](../../src/core/renderingpass/renderingpass.hpp#L210) の variant と [`PassDefinition`](../../src/core/renderingpass/renderingpass.hpp#L265)(`rasterization_samples` は `vk::SampleCountFlagBits` のまま)。RT の shader の解決 [`legacyRayTracingShaders()`](../../src/core/renderingpass/computetaskdefinition.cpp#L154) / [`layoutRayTracingShaders()`](../../src/core/renderingpass/computetaskdefinition.cpp#L171) と Vulkan の ABI の `static_assert`(`RayGroupLayout`・core の名前検索・Vulkan の ABI に依存)。
- **見送った作り替え**: 中立な実行値の型を作って core の型がそれを継承する案・組み立てと必須値の検査を project の関数に移す案・共通の仕上げと editor の G-1 の仮の ID をやめる案は、利用者の決定(2026-10-27)で見送り、2 つ目の backend が具体化した時に設計し直します(理由は設計 §2.7 の RC-4 の追補)。`passDefinitionOf()` の 3 つの呼び手(上の「[pass の道](#pass-ir)」)は RC-3c2b2 のままです。
- **削った欄と helper**(src・test に 0 件。実装者の報告の分類した検索): `PassDefinition::region_tags` / `resolution_domain` / `once`、`ComputeTaskDefinition::after` / `before`(compute の辺を集めるループと `appendEdges`、そのためだけの `computeTaskDefinitionOf()` の IR の引数も)、`passOutputSamples()`(呼び手 0)、`parseAuthoredAttachmentLoadOp()` / `parseAuthoredAttachmentStoreOp()`(本番の呼び手 0)。同じ事実の正本は IR(`NodeIR.regions` / `resolution_domain` / `schedule` / `after`)と FramePlan です。同じ名前の欄は別の型(`FrameGraphNodeDefinition`、論理・物理計画、FramePlan / execution の node、Studio の `GraphCanvasPlanPass` など)に残ります。`renderTargetMetadataOf()` は残しました(GPU の test が記述から独立に期待値を作って GPU 登録の結果と比べる)。
- **依存の向きの test**: [`pelican_test_project_execution_leaf`](../../test/CMakeLists.txt#L13)(CTest 名 `project_execution_leaf`、group に入れない)は `pelican_define_test()` を使いません(それは全 test に Vulkan の include path を渡すため)。link は `pelican_project` だけで、[`project_execution_shaderreference_test.cpp`](../../test/project_execution_shaderreference_test.cpp#L10) ほか 4 つの翻訳単位が移した header を 1 つずつ単独で include して compile し、`main` が `makeShaderReference()` を呼びます(stem・stage・拡張子付きの参照の拒否)。実装者の対照: 移した `shaderreference.hpp` に core の `vulkanimagevocabulary.hpp` を一時に include させると、この target の compile が `vulkan/vulkan.hpp` の不在(C1083)で赤。
- **書き換えた test**: `renderingpass_helpers_test.cpp` の region・domain の検査は同じ fixture の `NodeIR.regions` / `NodeIR.resolution_domain` に、load `dont_care` / store `store` と不正値の拒否は本番の正規化 → `parseRenderGraph()` → pass の構築に。足した [「RC-4 raster schedule stays in the IR and FramePlan」](../../test/renderingpass_helpers_test.cpp#L208) は fullscreen の `per_frame` / `once` を IR の schedule と FramePlan の node の `once` の両方で見ます。[「RC-3c2b1 the definitions are the final IR's, the ray tracing refusals name the authored task」](../../test/computetaskdefinition_test.cpp#L304) の改名後の `after` は `sharedComputeTasksOf()` の `NodeIR.after`・各 graph の `FrameGraphNodeDefinition.after`・FramePlan の順に。[`rendervocabulary_test.cpp`](../../test/rendervocabulary_test.cpp#L169) の作者の全綴りは `findAuthoredAttachmentLoadOp()` / `findAuthoredAttachmentStoreOp()` の値か nullopt に(消した wrapper の例外の文は契約から外れた)。

### view accessor は 2 本ある — scalar と layered

同じ target から image view を取る口が 2 つあり、**どちらを使うかは消費側 shader の ABI が決めます**。WP239b で両者の契約が公開ヘッダのコメントとして明記されました。

- [`getImageView()`](../../src/core/renderingpass/rendertargetcontainer.hpp#L165) は **scalar 互換アクセサ**です。array target であっても **layer 0 の `e2D` view** を返します(実体は `getImageLayerView(id, 0, ...)`)。
- [`getLayeredImageView()`](../../src/core/renderingpass/rendertargetcontainer.hpp#L186) は **全 layer を張る `e2DArray` view を 1 本**返します。**layer 数が 1 でも同じ**です。

後者の「1 layer でも layered」が WP239b の変更点です。それまでは layer 数 1 の family array に対してだけ `layer_count = 1` の `e2DArray` subresource view を別途生成していました。descriptor の shape(`e2DArray`)は変わらないので、これは束縛の意味を変える修正ではなく、同じ範囲の view を二重に作るのをやめる整理です。実際に落ちていたのは期待値の側でした — `hybrid_v1` の GPU テストが producer view-family array 導入前の scalar 契約のまま `getImageView()`(array target でも layer 0 の `e2D`)と比較しており、shader ABI が要求する layered view と一致しませんでした。WP239b はこの期待値を `getLayeredImageView()` へ直しています。現在は 1 layer でも canonical layered view へ統一され、同じ範囲の subresource view を重複生成しません。

> 🧩 **難所 — `family_array` は物理 layout をまたいで 1 つの descriptor に正規化する**([`ensureScreenInputDescriptor()`](../../src/core/material/materialcontainer.cpp#L2060) / [`MaterialDescriptorBuilder::build()`](../../src/core/material/materialdescriptorbuilder.cpp#L442))
>
> **何をする所か**: `materialcontainer.cpp` はテクスチャ台帳・descriptor 組み立て・世代リロード計画の 3 つに分かれ、descriptor set を実際に作るのは [`MaterialDescriptorBuilder`](../../src/core/material/materialdescriptorbuilder.hpp#L116)、束縛する image view を選ぶ規則は material 側と fullscreen 側が共有する [`resolvePassInputImageView()`](../../src/core/renderingpass/passinputdescriptorvariant.cpp#L21) です。material が読む pass input(screen input)について、`.surface` の resource port が宣言した view 種別([`ShaderResourcePortView`](../../src/project/shaderresourceport.hpp#L24))と、pass 側の物理 view 種別([`VulkanResourceViewLayout`](../../src/project/targetrenderplanning.hpp#L181)。RC-3a2 で core の写し `PassInputViewDimension` をこの project の型に統合)を突き合わせ、実際に束縛する `vk::ImageView` と descriptor の次元を決めます。
>
> **素朴に読むと**: 名前の似た enum が 2 つあり、**同じ綴り(`shared_2d`)が両方にあって意味が違います**。宣言側 `ShaderResourcePortView`(`shared_2d` / `per_view` / `family_array` / `cube`)は **shader ABI**、つまり `sampler2D` か `sampler2DArray` かを決めます。pass 側 `VulkanResourceViewLayout`(`shared_2d` / `sequential_2d` / `layered_2d_array` / `family_2d_array`)は **producer が view family をどう物理化したか**です。ここで効くのは、**1 本の material pipeline が複数の compatible pass variant で共有される**ことです。同じ transparent material が main view family でも planar reflection の secondary family でも使われるので、shader ABI を物理 layout ごとに変えるわけにいきません。そこで `family_array` を宣言した sampled resource に限り、`shared_2d`(1 view の scalar 表現)と `sequential_2d`(view を順次実行するが保存先は array-backed)を descriptor 境界で `family_2d_array` へ**正規化**します。`sequential_2d` が抜けていたのが WP239c の回帰で、planar reflection の capture target が `sampler2D` と判定され ABI mismatch を報告していました。物理 scheduler 側の `sequential_2d` lowering は変えていません。
>
> **骨子**:
> ```text
> descriptor_view_dimension = view_dimension
> if 非 input_attachment && port.view == family_array &&
>    view_dimension ∈ {shared_2d, sequential_2d}:
>      descriptor_view_dimension = family_2d_array          # ABI を variant 間で固定
> descriptor_dimension = descriptor_view_dimension ∈ {layered_2d_array, family_2d_array}
>                        ? e2DArray : e2D
> 束縛時: array_view なら getLayeredImageView()      # 全 layer の canonical view
> ```
>
> **具体例**: port が `view: "family_array"` を宣言した sampled resource は、producer 側の物理化がどれであっても shader からは 1 種類に見えます。
>
>
> | 宣言(.surface の port) | producer の物理化 | descriptor に載る次元 | shader が書く型 |
> |---|---|---|---|
> | family_array | shared_2d | family_2d_array | sampler2DArray |
> | family_array | sequential_2d | family_2d_array | sampler2DArray |
> | family_array | layered_2d_array | そのまま | sampler2DArray |
>
> 2 行目が WP239c で足りていなかった行です。planar reflection の capture target は view を順次実行する `sequential_2d` として物理化されるので、正規化を通さないと descriptor が `e2D` になり、`sampler2DArray` を要求する shader と ABI mismatch で落ちていました。同じ transparent material が main family と planar reflection の secondary family の両方で使われる以上、shader ABI を物理化ごとに変えるわけにいかない、というのがこの正規化の理由です。
>
> **手がかり**: 正規化は `descriptor_dimension` だけでなく、descriptor に**保存する `view_dimension` そのもの**も `family_2d_array` へ書き換えます。これが束縛する view を選ぶ [`resolvePassInputImageView()`](../../src/core/renderingpass/passinputdescriptorvariant.cpp#L21) の分岐(`input.view == family_2d_array`)を array 経路へ振り向け、`getLayeredImageView()` が選ばれる理由です(片方だけ直すと descriptor 型と束縛 view が食い違います)。この分岐は以前 material 側の `buildScreenInputDescriptor()` にありましたが、`923ce648`(2026-09-19 の MaterialContainer の分割)で material と fullscreen の 2 本の view 選択がこの 1 関数にまとめられ、material 側は [`MaterialDescriptorBuilder::build()`](../../src/core/material/materialdescriptorbuilder.cpp#L442) から呼びます。緩めたのは sampled resource だけで、input attachment・通常の `shared_2d` / `per_view`・`cube`・layered multiview の検証規則はそのままです。ABI mismatch の例外文には shader 側・解決後 descriptor・宣言 port の 3 つの dimension が並ぶようになったので、次の不一致はメッセージだけで切り分けられます。directional shadow の入力契約が `family_array` であることは [`makeBuiltinMaterialPassInputContract()`](../../src/project/materialscreeninput.cpp#L64) にあり、対応する shader 側は `sampler2DArray` です([`pelican_lighting_v1.glsl` 内](../../src/core/resources/shaders/include/pelican_lighting_v1.glsl#L17))。テストは [`headless_render_test.cpp`](../../test/headless_render_test.cpp) の "hybrid_v1 preset registers and renders a headless frame"(shadow の束縛を `getLayeredImageView()` と比較)、[`golden_cases_test.cpp`](../../test/golden_cases_test.cpp) の "planar reflection executes a clipped secondary view family on the GPU"、CPU 側は [`targetplanning_test.cpp`](../../test/targetplanning_test.cpp)(`directional_shadow` の view policy が `family_array` であることを固定)です。
>
> **不変条件**: 1 layer の family array も canonical layered view を使うこと(同じ範囲の subresource view を重複生成しない)。`family_array` を宣言していない port view を array 化しないこと。descriptor の `view_dimension` と `descriptor_dimension` を同時に決めること。

extent が変わったフレームでは [`handleFrameTargetResize()`](../../src/core/vkcore/renderer.cpp#L2792) が呼ばれます。**ただし window 出力はここで resize されません。** この関数が最初にするのは [`windowOutputFactsChanged()`](../../src/core/vkcore/renderer.cpp#L2768) の判定で、window target の `OutputCompileFacts` が publish 済み世代のものとずれていれば `OutputRelowerRequired` を投げます([その throw](../../src/core/vkcore/renderer.cpp#L2806))。[`OutputCompileFacts`](../../src/core/vkcore/outputcompilefacts.hpp#L20) は `extent` を含むので、**window では「extent だけ変わった」も必ずこちらへ倒れます**。コメントが規範です。

> Window output resources, descriptors, pipelines, and compile facts are
> one renderer generation. Even an extent-only change is re-lowered
> through the normal all-or-nothing configuration transaction instead of
> mutating live targets beneath the frame's immutable generation.

例外を受けるのは [`Renderer::render()`](../../src/core/vkcore/renderer.cpp#L7191) のリトライループです([再 lower して retry する所](../../src/core/vkcore/renderer.cpp#L7175))。[`relowerRenderPipelineForCurrentOutput()`](../../src/core/vkcore/renderer.cpp#L3525) が rendering config から graph variant を lower し直して新しい世代を publish し、`internal_render_extent` を現在の出力サイズへ入れ直してからフレームを再試行します。試行は **2 回まで**(`attempt < 2`)で、2 周目でも facts がずれていれば `"window output compile facts changed during re-lowering"` で止まります — 黙って古い世代のまま描き続けません。

したがって `handleFrameTargetResize()` の本体が走るのは **window でない logical target(現状は OpenXR とテスト用 target)だけ**で、内容は次の 4 つです。

- [`prepareForExtent()`](../../src/core/renderingpass/rendertargetcontainer.cpp#L944) が新しい base extent 一式の**候補を丸ごと 1 個**作る(現物の image / view はまだ差し替えない)。
- [`publishPreparedExtent()`](../../src/core/renderingpass/rendertargetcontainer.cpp#L1178) がその候補を **1 回の publish** で反映する。候補を作った時点から registry が動いていれば `"render target extent candidate is stale"` で弾かれます。
- [`rebindFullscreenInputs()`](../../src/core/vkcore/renderer.cpp#L2710) が fullscreen pass の input descriptor を新しい image view へ rebind する。
- layout tracker を reset する。

戻り値 `true` は「in-place の resize を実際に行った」の意味で、受けた呼び出し元が temporal history を落として `internal_render_extent` を更新します。この 2 段構え(候補作成と publish を分ける / stale なら例外)は [`headless_render_test.cpp`](../../test/headless_render_test.cpp) の "typed image subresources execute a two-stage depth pyramid and rebind after resize" が固定しています。

画像 layout の現在値は [`RenderTargetLayoutTracker`](../../src/core/vkcore/render_target_layout_tracker.cpp#L131) が追います。キーは target ID 単体ではなく `(rt_id, surface_index)` の組で(#L72-L74)、**history 付き(double-buffered)target** の現/旧 surface を別々に追跡します。初見の surface は target の initial layout、同じ layout への遷移は何もしません。`-2` の swapchain target は特殊 ID なので tracker が無視し、`SwapchainFrameTarget` 側に管理を任せます。

layout transition は単なる状態ラベルではありません。[`makeTransitionInfo()`](../../src/core/vkcore/render_target_layout_tracker.cpp#L9) が old/new layout から pipeline stage と access mask を作ります。

| layout | 主な stage / access |
|---|---|
| shader-read | fragment shader / shader read |
| color attachment | color output / attachment read+write |
| depth attachment | early/late fragment tests / depth read+write |
| general | compute shader / shader read+write |
| transfer src/dst | transfer / transfer read・write(history copy、mirror copy 用。#L30-L37, #L52-L59) |

### persistent の target は tracker の reset をまたいで layout を保つ(PS-1)

✅PS-1 の `lifetime: "persistent"` は [`RenderTargetDefinition::persistent`](../../src/core/renderingpass/rendertargetdefinition.hpp#L70) になり(宣言を読むのは ✅RC-3c2a から `parseRenderGraph()` の [lifetime を読む所](../../src/project/rendergraphparse.cpp#L1670) で、`ImageResource::persistent` を経て。variant をまたぐ統合では、どれか 1 つが persistent なら記述の `GenerationImage::persistent` が立つ)、container の record にも写ります。同じ名前の登録が繰り返されたときは、どれか 1 つが persistent なら以後ずっと persistent です(lifetime は image の契約に入れず、印は立つだけ。候補の取り消しをまたいでも害が無いため。[その所](../../src/core/renderingpass/rendertargetcontainer.cpp#L762))。

- **alias**: [`isRuntimeAliasCandidate()`](../../src/core/renderingpass/renderingsamplecount.cpp#L83)、transient attachment と tile-local の候補の判定が persistent を外し、[`runtimeAliasExclusion()`](../../src/core/renderingpass/renderingsamplecount.cpp#L103) が原因に `persistent lifetime` を足します。
- **layout**: `RenderTargetLayoutTracker` は persistent の target の key ごとに、追っていた `VkImage` を `persistent_images` に覚えます(`transition()` / `memoryDependency()` / `assumeLayout()` が [`notePersistentImage()`](../../src/core/vkcore/render_target_layout_tracker.cpp#L386) を呼ぶ)。[`reset()`](../../src/core/vkcore/render_target_layout_tracker.cpp#L342) は、ほかの key の layout と shader の stage を捨てても、persistent の key の分は残します。PS-1 の前は reset で全部を捨てていたので、次の使用は [`initialLayout()`](../../src/core/renderingpass/rendertargetcontainer.cpp#L1562) からの遷移になっていました(UNDEFINED からの遷移は中身を保証しません)。
- **作り直し**: 使うたびに [`forgetRecreatedPersistentImage()`](../../src/core/vkcore/render_target_layout_tracker.cpp#L368) が今の `VkImage` と覚えた handle を比べ、違えば(extent・format の変更や組み直しで作り直された)その key の layout を捨てて initial layout から始めます。`currentLayout()` も同じ比較をします。ping-pong の 2 面は作りません。

### 現在の image barrier の注意点

compute の image resource は [`transitionResourcesForDispatch()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1854) で、sampled のみなら `eShaderReadOnlyOptimal`、storage を含むなら `eGeneral` を選びます。現在の layout が同じなら [`RenderTargetLayoutTracker::memoryDependency()`](../../src/core/vkcore/render_target_layout_tracker.cpp#L211)、異なれば `transition()` を呼びます。同じ `GENERAL` のまま使う image にも、layout を保ったまま stage / access の memory dependency を発行します。

`transition()` 自体が同じ layout で早期 return することと、compute dispatch 前の同期が省略されることは同義ではありません。buffer の RAW barrier は別経路の [`bufferReadAfterWriteBarrier()`](../../src/core/renderingpass/computetaskcontainer.cpp#L2023) が担当します。

> 🧩 **難所 — 色の経路は型で申告し、文字列は RPC の端だけ**([`OffscreenFrameTarget::OffscreenFrameTarget()`](../../src/core/vkcore/offscreenframetarget.cpp#L98) / [`caps()`](../../src/core/vkcore/offscreenframetarget.cpp#L376))
>
> **何をする所か**: frame target 側(headless)の color attachment のフォーマットを決め、選んだ結果を [`OutputCompileFacts::encoding_path`](../../src/core/vkcore/outputcompilefacts.hpp#L26)(`srgb_hardware` / `srgb_shader_unorm`)として外へ申告します。
>
> **素朴に読むと**: 第一候補は `R8G8B8A8Srgb` ですが、`COLOR_ATTACHMENT` と `TRANSFER_SRC` を optimalTiling で両方満たさない実装があるため `R8G8B8A8Unorm` へ落ちます(UNORM でも満たさなければ throw して黙って進みません、[format 選択の分岐](../../src/core/vkcore/offscreenframetarget.cpp#L113))。読みにくいのは、その判定式に**テスト専用フラグが `||` で混ざっている**ことです([`force_unorm_color_path_for_testing`](../../src/core/launchconfig.hpp#L108) / [その判定式](../../src/core/vkcore/offscreenframetarget.cpp#L113))。この分岐は大抵の開発機では絶対に通らないので、放っておくとテストが一度も踏まない到達不能経路になります。フラグはそれを CI で踏むための唯一の入口で、[`rpc_color_contract_test.cpp` 内](../../test/rpc_color_contract_test.cpp#L161) が `GENERATE(false, true)` で両方を回します。そして肝心なのは、**フォールバックしても出力バイトの意味は変わらない**ことです。選んだ format は終端 pass のフォーマットになり、UNORM なら [`renderingpassruntimecompiler.cpp` 内](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L2261) が `PELICAN_OUTPUT_UNORM_FALLBACK` を define して [`output_transform.frag`](../../src/core/resources/output_transform.frag) が `linearToSrgb()` を自分で掛けるからです(§6.1 の「HW が OETF」の代替)。違うのは**手段と丸め誤差**だけで、同じテストが許容差を `fallback ? 1 : 0` に切り替えているのがその現れです。
>
> **骨子**:
> ```mermaid
> flowchart TB
>     Srgb{"Srgb の条件を満たす?"}
>     Hardware["Srgb<br/>encoding_path = srgb_hardware"]
>     Unorm{"Unorm の条件を満たす?"}
>     Shader["Unorm<br/>encoding_path = srgb_shader_unorm"]
>     Define["終端 frag に PELICAN_OUTPUT_UNORM_FALLBACK"]
>     Throw["throw"]
>     Srgb -->|"はい"| Hardware
>     Srgb -->|"いいえ"| Unorm
>     Unorm -->|"はい"| Shader --> Define
>     Unorm -->|"いいえ"| Throw
> ```
>
> - **Srgb の条件**: `COLOR_ATTACHMENT` と `TRANSFER_SRC` を満たし、かつ `force_unorm_..._for_testing` でない。満たさない、または `force_unorm_..._for_testing` なら Unorm を調べる。
> - **Unorm の条件**: `COLOR_ATTACHMENT` と `TRANSFER_SRC` を満たす。
>
> - **RPC 表示アダプタ**: ここだけが `srgb_hardware` を `"srgb"`、`srgb_shader_unorm` を `"unorm_fallback"` と表示する。
>
> **手がかり**: つまり `encoding_path` は「絵が違う」の申告ではなく、**どちらの経路で sRGB になったか**の申告です。かつて `FrameTargetCaps::color_path` という文字列だったものは型になり、`"srgb"` / `"unorm_fallback"` という簡潔な綴りは [`outputEncodingPathRpcName()`](../../src/core/vkcore/outputcompilefacts.hpp#L65) という**表示アダプタ 1 箇所だけ**に閉じました。コメントが規範です(「The RPC color contract predates the typed compiler vocabulary. Keep its concise wire spelling in this display adapter only.」)。RPC の `get_status` は `color.path` として返し([`rpcserver.cpp` 内](../../src/core/communication/rpcserver.cpp#L1811))、`readback_encoding` は経路によらず常に `"srgb"` です。golden 比較はこの値を見て許容差(0 か ±1 LSB か)を選ぶ必要があります。同じ判定は windowed 側にもあり、[`outputFacts()`](../../src/core/vkcore/swapchainframetarget.cpp#L434) が surface format `R8G8B8A8_SRGB` / `B8G8R8A8_SRGB` のときだけ `srgb_hardware` を返します。
>
> **不変条件**: フォールバックしても readback の意味(sRGB エンコード済み 8 bit)を変えないこと。経路を増やしたら `OutputEncodingPath` の値を増やし、テスト用フラグで到達できるようにすること。文字列表現を RPC アダプタの外へ持ち出さないこと。候補が尽きたら例外にして黙って進まないこと。

## 6.8 Compute task

compute の JSON は ✅RC-3c2b1 から `parseRenderGraph()` の [`parseComputeTask()`](../../src/project/rendergraphparse.cpp#L1458) が IR の node に読み、registration が登録する `ComputeTaskDefinition` は確定 IR から [`computeTaskDefinitionsOf()`](../../src/core/renderingpass/computetaskdefinition.cpp#L451) が作ります(§6.7 の「[compute / RT task の道](#compute-task-ir)」。前は core の `parseComputeTaskDefinitionsFromConfigJson()` が JSON から直に作っていた)。読む欄は次のとおりです。

- `shader`: extensionless stem
- `reads`, `writes`: buffer または concrete render target の名前
- `after`, `before`: 明示順序
- `dispatch.groups`: x/y/z group 数
- `schedule`: `per_frame`(既定)/ `per_view` / ✅PS-1 の `once`([読む所](../../src/project/rendergraphparse.cpp#L1528))。`once` は走る frame では per_frame と同じに dispatch され、走るかどうかは §6.4「once の node を記録しない frame」

descriptor は shader reflection の set 1、すなわち `PELICAN_SET_PASS_INPUT` だけを読み、binding 順に構築します。[`resourceForBinding()`](../../src/core/renderingpass/computetaskcontainer.cpp#L486) の対応規則は次の優先順です。

1. reflection された binding 名と宣言 resource 名が一致すれば、その resource。
2. resource が1個だけなら、それを使用。
3. それ以外は binding の順番と resource 配列の順番を対応。

`resource_ports` を持つ task では、[`createDescriptorSet()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1028) は reflection の binding ごとに先に [`interfaceForBinding()`](../../src/core/renderingpass/computetaskcontainer.cpp#L511) で同じ binding 番号の port を探し、見つかればその port の resource を書きます。上の 3 規則は port の無い binding だけに使われます。

port の binding は [`makeComputeResourceInterface()`](../../src/core/renderingpass/computetaskcontainer.cpp#L143) が resource の並びの位置(同じ resource の 2 本目以降の port は resource の数より後ろ)で仮に振り、RS-5 からは shader を読み込んだ後に [`bindShaderResourceInterfaceByReflectedName()`](../../src/core/shader/shaderresourceinterface.cpp#L941) が、反映の set 1 で `pelican_resource_<port>` の名前を持つ binding に付け替えます(RT task は全 stage の反映を `merge()` してから。[呼ぶ所](../../src/core/renderingpass/computetaskcontainer.cpp#L1400))。pipeline の layout はその反映から作られるので、layout と書き込みが同じ「名前 → binding」で一致し、port を書く順は効きません。

生成 include で compile した shader では include が振った番号がそのまま反映に出るので、付け替えは何も変えません。名前が反映に無い port は番号を変えず、pipeline の作成の [`validateShaderResourceInterfaceReflection()`](../../src/core/shader/shaderresourceinterface.cpp#L849) が absent として断ります(RT では `validateRayTracingPipelineInterfaces()` の後)。1 つの名前が 2 つの binding にあれば付け替えの関数自身が断ります。付け替えるのは登録のときだけで、hot reload の組み直し([`rebuildDirty()`](../../src/core/shader/pipelinefactory.cpp#L2150) など)は保存した desc の binding のまま組みます。fullscreen / raster pass はこの関数を通りません(binding は pass の入力の位置)。

生成 include は ✅RS-11 から、port ごとに `#define PELICAN_RESOURCE_PORT_<port> 1` を書きます([`generateShaderResourcePortInclude()`](../../src/core/shader/shaderresourceinterface.cpp#L647) の [その所](../../src/core/shader/shaderresourceinterface.cpp#L667))。

port の無い pass の `#include "pelican_resource_ports.glsl"` は、engine resource として登録した空の同名の file([中身](../../src/core/resources/shaders/include/pelican_resource_ports.glsl#L4)。登録は `src/core/loader/engineresources.cpp` と `src/core/resources/CMakeLists.txt` の `b_embed`)で compile されます。port を任意にする shader は `#ifdef PELICAN_RESOURCE_PORT_<port>` で既定値に分けます(`bloom_composite.frag`)。条件無しにアクセサを呼ぶ shader は、port の無い pass では今までどおりアクセサの名前で compile に失敗します。

buffer は storage buffer、render target は storage image でなければ [`createDescriptorSet()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1028) が例外にします。task 実行は graphics frame の command buffer 上で pipeline と set 1 を bind し、[`dispatch()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1932) を記録します。

**descriptor pool は世代の task から決め、batch が持つ(✅CP-1、`9c08995a6`)。**CP-1 の前は `ComputeTaskContainer` の pool が固定(set 256 / 型ごとに descriptor 1024)で、task 数が増えると `ErrorOutOfPoolMemory` になっていました(72 task を flat と XR の両 variant に入れると 432 set。実装者の報告)。

- **数え方**: [`ComputeDescriptorBudget`](../../src/core/renderingpass/computedescriptorbudget.hpp#L18) は task の pipeline の反映から set 1(`PELICAN_SET_PASS_INPUT`)の binding だけを数え、`binding.count × view の数 × parity 2` を descriptor の型ごとに足し、binding のある task だけ set を `view の数 × 2` 足します。2 は frame in flight ではなく履歴の parity の数で、確保と見積もりの両方が [`computeDescriptorParities`](../../src/core/renderingpass/computedescriptorbudget.hpp#L16) を読みます。余裕は [`capacity()`](../../src/core/renderingpass/computedescriptorbudget.hpp#L45) 1 か所で `required + ceil(required / 4) + 16` です。`uint32_t` を越える数は `compute.descriptor_budget@1 {field, count, limit, task}` で断ります。
- **device の上限**: Vulkan の `maxDescriptorSet*` は pool の合計ではなく pipeline layout ごとの上限なので、[`validateComputeDescriptorLayout()`](../../src/core/renderingpass/computedescriptorbudget.hpp#L63) は task ごとの反映に storage buffer / storage image / sampled image / sampler の 4 つを当て、越えれば同じ `compute.descriptor_budget@1` です(pool の合計は比べません)。
- **所有**: [`ComputeTaskContainer::prepareDescriptorGeneration()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1607) は、まだ pool を持たない task をまとめて見積もり、[`createDescriptorPool()`](../../src/core/renderingpass/computetaskcontainer.cpp#L739) で batch の pool を 1 つ作り、全部の task の set を作り終えてから `TaskRecord::descriptor_pool`(`shared_ptr`。[set より先に宣言して、set が先に解放される](../../src/core/renderingpass/computetaskcontainer.hpp#L148))に配ります。同じ pool を共有するのは同じ世代の task だけです。render target の組み直し([`ComputeTaskContainer::rebindRenderTargets()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1656))も元の pool ごとに見積もり直して新しい pool を作るので、退役中の世代と今の世代が pool を混ぜません。古い pool は今までどおり arena の rollback / retire と DeletionQueue の lease で退役します。
- **枯渇**: set の確保の `vk::OutOfPoolMemoryError` は `compute.descriptor_pool_exhausted@1 {result: "ErrorOutOfPoolMemory", task, tasks, sets, max_sets}` に変わります(登録と組み直しの両方。[登録の側](../../src/core/renderingpass/computetaskcontainer.cpp#L1629))。候補は半端なまま公開されません。
- **Problems**: `compute.descriptor_` で始まる拒否は、[`recordRenderPipelineProblem()`](../../src/core/watch/problemlist.cpp#L365) が `render.config_apply_failed@1` に包み、`reason_code`・`reason`(元の構造化した拒否)・`document` を付けて同じ domain の 1 行を置き換えます([包む所](../../src/core/watch/problemlist.cpp#L386))。直して commit すると既存の編集 / reload の clear で消えます。`get_status.reload.last_error` は render の participant の今の `domain_error` を映します。
- **編集の RPC の失敗のうち、どれを載せるか(✅CP-1b、`507a57ed8`)**: CP-1 は `applyRenderPipelineAuthoringCandidate()` の catch で編集の失敗を全部記録したので、検証で断った編集(`frame_node_order.conflicts_dependency@1`、`pelican.ray_tracing.shader_record_abi_mismatch@1` など)の行が Problems に残り、`studio_scenario_gui_graph_order` と `studio_scenario_gui_rt_checks` が赤になりました。今は catch が [`recordRenderPipelineAuthoringFailure()`](../../src/core/vkcore/renderer.cpp#L3802) を呼び、[`vk::SystemError`・`std::bad_alloc`・`compute.descriptor_*` のときだけ](../../src/core/vkcore/renderer.cpp#L3811) source `edit_rpc` で記録します([catch](../../src/core/vkcore/renderer.cpp#L3966))。それ以外の拒否は答えの `error` に残るだけで、Problems の行を足しも置き換えもしません。「GPU の準備に入ったか」では分けません。shader record の ABI の検査のように、検証は GPU の準備の途中でも走るためです(コードの注記)。staged の後の rollback([記録する所](../../src/core/vkcore/renderer.cpp#L256))、file watch の読み直し([記録する所](../../src/core/vkcore/renderer.cpp#L3779))、`reload_render_documents` の拒否([`noteProblems()`](../../src/core/communication/renderconfigeditor.cpp#L7848))、起動時の失敗([記録する所](../../src/core/vkcore/renderer.cpp#L3158))はこの判定を通らず、拒否の種類に関係なく記録します。
- **起動**: 起動時の読み込みの失敗は今までどおり `PelicanCore::run()` が false を返し、player は非 0 で終わります(CP-1 は、拒否される文書で player の子 process を `--headless` で起動し、非 0 の終了と stderr の `compute.descriptor_budget@1` を確かめる test を足した。`test/run_cp1_startup.cmake`。headless の player が MessageBox を出さないのは CP-1 ではなく DS-1f の [`shouldShowPlayerFatalDialog()`](../../src/core/playerdiagnostics.hpp#L16) の条件)。
- test: `cp1_compute_descriptor_gpu` の「CP1 compute generations dispatch 80 stereo tasks and report apply failures」(GPU。80 task × 2 view の dispatch、確保の途中の失敗・組み直しの途中の失敗・device の上限の超過・修復、実 player の子 process の非 0)と、`reloadtransaction_test` の `CP1 …` 5 本(CPU。CP-1b の「CP1 RPC edit refusals do not add or replace Problems」は、node の順・RT の ABI・名前の無い拒否の 3 種で行も revision も変わらないこと、descriptor・device memory・host memory の失敗は編集でも 1 行になることを見る)。

### buffer 宣言の実装済み範囲

[`FrameGraphResourceContainer::registerBuffers()`](../../src/core/renderingpass/framegraphbuffercontainer.cpp#L33) は `size > 0` の buffer を一度だけ device local memory に確保します。同じ名前の 2 回目は飛ばします。✅RC-3c2a から registration が渡すのは generation の資源の記述の buffer で、variant をまたぐ同名の buffer は全欄が一致したものだけです(上の「generation の資源の記述」。前は先に登録した方が勝っていた)。

- 文字列だけ、または size 0 の宣言は graph の既知名にはなりますが、実 buffer を確保しません。
- 宣言を読むのは ✅RC-3c2a から `parseRenderGraph()` の [`parseBuffers()`](../../src/project/rendergraphparse.cpp#L1770) だけです(core の `parseFrameGraphBufferDefinitionsFromJson` は消えた)。`lifetime: persistent|transient` は [lifetime を読む所](../../src/project/rendergraphparse.cpp#L1909) で読みますが、登録側は `persistent` を参照していません。transient の frame 単位 recycle は未実装です。✅PS-1 の graph の lowering は、once の node が書く・読む buffer の `lifetime` を見ます(書かなければ persistent。`transient` を書いた buffer に once が書くと `graph.once_writes_transient@1`)。
- `host_source: "runtime_f32"` の buffer だけが `initial_f32` を持てます([`FrameGraphBufferDefinition::initial_f32`](../../src/project/framegraphbufferdefinition.hpp#L98))。parse は **空でない float 配列であること**と、**`size` の 4 byte ごとにちょうど 1 要素**であることを検査し、外れると `render_graph.buffer_invalid@1 {resource, field: initial_f32, reason}` で落とします([`initial_f32` の parse](../../src/project/rendergraphparse.cpp#L1882)。reason は `requires_number_array` / `empty`(✅RC-3c2a。明示した `[]` は省略と別に断る)/ `requires_runtime_f32` / `count_mismatch`。個数は [`requireBufferSizeContract()`](../../src/project/framegraphbufferdefinition.cpp#L95) が見るので、`size_from_extent` の buffer では大きさが決まった後に資源の記述が見ます)。登録時には確保した CPU 側の値列へ [`std::copy` で先頭から書き込み](../../src/core/renderingpass/framegraphbuffercontainer.cpp#L96)、dirty を立てて GPU へ運びます。空なら 0 — この機能が入る前の全 buffer の挙動です。要点は**最初のフレームが未初期化値で描かれない**ことで、`render.scalar` の書き込みが届くのはそれを publish したフレームからです。文書に直接書くこともできますが、feature の scalar から導出される場合は authored な `initial_f32` は拒否されます(§6.1 の難所「`binding: buffer` は 5 つの宣言が噛み合って初めて成立する」)。
- `dispatch.groups_from: {"port": "..."}` は typed image resource port の選択 mip extent と shader reflection の `local_size` から `ceil(extent / local_size)` を導出します。render target resize と compute shader reload の rebind 時にも再計算されます。
- `dispatch.local_size` は受理しません。workgroup size の authority は compute shader の `layout(local_size_*=...)` と SPIR-V reflection です。
- 実行後に group 数を変える API は [`setDispatchGroups()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1818) です。

設定 schema に項目があることと、runtime behavior が完成していることを区別して読む必要があります。

### Compute の parse と runtime の対応 {#compute-runtime-support}

調査時点の実装範囲です。

| 設定 | parse | runtime behavior |
|---|---:|---|
| `dispatch.groups` | 済 | `vkCmdDispatch(x,y,z)` に使用 |
| `dispatch.indirect` | 済 | typed command bufferから`vkCmdDispatchIndirect`。command resourceのread edgeも自動導出 |
| `dispatch.groups_from: {"port":"..."}` | 済 | typed image port の選択 mip extent と shader reflection の local size から direct group 数を導出。resize/rebindでも再計算 |
| `dispatch.local_size` | 検出 | 二重 authority を避けるため明示 error。shader の `layout(local_size_*=...)` が authority |
| `dispatch` の未知の鍵、範囲外の数(✅RC-3c2b1) | 検出 | `render_graph.compute_task_invalid@1` の `unknown_field` / `unknown_axis` / `requires_positive_uint32` / `requires_unsigned_integer`。前は無視・折り返し・切り詰めで受けていた |
| `schedule: per_frame` | 済 | 対応 |
| `schedule: per_view` / `once` | 済 | view ごとの実行 / once の実行として登録 |
| それ以外の schedule | 検出 | 明示 error |
| buffer `size > 0` | 済 | device-local storage buffer を一度確保 |
| buffer `size == 0` | 済 | graph 名だけ。実 buffer はなし |
| buffer `command_layout: compute_dispatch` | 済 | 12 byte以上を検証し`INDIRECT_BUFFER` usageを付与 |
| buffer `command_layout: indexed_draw` | 済 | packed 20 byte以上を検証し`INDIRECT_BUFFER` usageを付与 |
| buffer `command_layout: draw_count` | 済 | u32 1個以上を検証し`INDIRECT_BUFFER` usageを付与 |
| buffer `host_source: scene_draw_commands_v1` | 済 | CPU DrawQueue commandを20 byte strideへcompactし、容量超過はゼロ埋め+診断 |
| buffer `host_source: scene_draw_bounds_v1` | 済 | commandと同じflatten順でworld AABBを32 byte strideへpack。`minimum.w`がvalid flag |
| buffer `host_source: scene_draw_segments_v1` | 済 | CPU-bound state rangeをsource/output command範囲とcount slotへ対応付ける32 byte record |
| material pass `gpu_draw_source` | 済（fixed/segmented state） | fixed rangeまたは複数segmentを`drawIndexedIndirectCount`で置換。state bindはCPU、容量不足/未対応device/CPU強制はpass全体を既存DrawQueueへfallback |
| `lifetime: persistent` | 済 | 実質全 buffer が container lifetime |
| `lifetime: transient` | 済 | frame ごとの確保/recycle は未実装 |

根拠は [`computeDispatch()`](../../src/project/rendergraphparse.cpp#L1326)(✅RC-3c2b1 から dispatch を読むのは project の parse。前は core の `parseDispatch()`)、[`registerBuffers()`](../../src/core/renderingpass/framegraphbuffercontainer.cpp#L33)、[`registerComputeTask()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1308) です
(旧 `computetask.cpp` は宣言パース / container / buffer container の 3 ファイルに分割され、宣言パースの `computetaskdefinitionjson.cpp` は RC-3c2b1 で消えました)。

画像全体または特定 mip を処理する task は、`{"groups_from":{"port":"reduced_depth"}}` のように typed image port を指定できます。port の `subresource.mip`、render target の現在 extent、shader reflection の [`local_size`](../../src/core/shader/shaderreflection.hpp) を runtime が結合し、X/Y group 数を切り上げ除算で求めます。`local_size_z` は 2D image contract のため 1 が必須です。固定 `groups`、image-derived `groups_from`、GPU-produced `indirect` は相互排他的です。粒子数のような任意の scalar parameter 由来 dispatch はこの契約へ混ぜず、typed indirect command buffer または将来の別契約で扱います。

compute task は dedicated compute queue へ submit せず、graphics frame command buffer に記録します。一方 [`pickQueues()`](../../src/core/vkcore/core.cpp#L228) の fallback は graphics と compute を別 family として受理できます。現 frame graph compute は graphics queue に compute capability があることを実質仮定していますが、fallback path はそれを必須検証していません。async compute を実装する場合は command pool/submit だけでなく queue family ownership transfer も必要です。

### [`slot_index = group_index * ray_count + ray_index`](../../src/core/renderingpass/computetaskdefinition.cpp#L308) — 線形化の向き {#reading-note-036}

(hit group, ray type) の 2 次元表を **group major** で線形化したもので、各ヒットグループの ray type 分が連続ブロックになります。これは SBT のヒットレコード索引(`q × ray_type_count + ray type` — [§9.23.5](09_black_magic_and_gotchas.md#9235-rtacceleration-structuresbtoracle) の `rayTracingInstanceRecordOffset()`)と同じ major の取り方なので、そのまま SBT のスロット配置へ写せます(→ [§9.23.5](09_black_magic_and_gotchas.md#9235-rtacceleration-structuresbtoracle) の q・record offset)。入力表は [`hit_shaders[ray][group][...]` という 3 層ネストの入力](../../src/project/rendergraphparse.cpp#L1237)です(✅RC-3c2b1 から、`parseRenderGraph()` が書かれたとおりの形で IR に持ち、core の `layoutRayTracingShaders()` が layout に照らしてこの表に並べます)。

### [layer_count の自動展開](../../src/core/renderingpass/computetaskcontainer.cpp#L355) — 条件と目的 {#reading-note-037}

対象が 2D array 次元で port の view が `per_view` または `family_array` のときに限り、`layer_count == 1`(かつ `base_array_layer == 0`)を `physical_view_count` に展開します。目的は「1 レイヤだけ書けば family 全体を指す」省略記法を許しつつ、**明示指定は論理 view 数と完全一致することを強制する**ことです。descriptor 側の正規化は[第6章「`family_array` は物理 layout をまたいで 1 つの descriptor に正規化する」](06_rendering_vulkan_shader.md)が本体です。

### [`sequential_layer`](../../src/core/renderingpass/computetaskcontainer.cpp#L1129) と [`shares_storage_layout`](../../src/core/renderingpass/computetaskcontainer.cpp#L1166) {#reading-note-038}

`sequential_layer` は非 cube・非 layered で `sequential_2d` レイアウトの多 view リソース、つまり **view ごとに別 array layer を使う**構成なので、その view の層を選ぶ必要があります。`shares_storage_layout` は同一 render target が同じディスパッチ内で storage image としても束縛されている場合で、**1 つの image を 2 つのレイアウトに同時に置けない**ため sampled 側も `eGeneral` に揃えます。

## 6.9 Shader reference、compile、reflection

### extensionless stem

project rendering config では shader を `foo/bar` のような stem で指定します。[`makeShaderReference()`](../../src/project/shaderreference.cpp#L76)(✅RC-4 から project。core の `src/core/shader/shaderreference.hpp` は互換の include だけ)は `.vert`、`.frag`、`.comp`、`.spv` などが明記されていると例外にします。

stage に応じた候補は次です。

- vertex: `stem.vert` / `stem.vert.spv`
- fragment: `stem.frag` / `stem.frag.spv`
- compute: `stem.comp` / `stem.comp.spv`
- raygen: `stem.rgen` / `stem.rgen.spv`
- miss: `stem.rmiss` / `stem.rmiss.spv`
- closest-hit: `stem.rchit` / `stem.rchit.spv`

後ろ 3 stage は `compute_tasks[].ray_tracing` 専用です。`any_hit` / `intersection` / `callable` は現在未対応で、宣言すると名前付きエラーになります。実行と SBT の契約は[第11章](11_ray_query_acceleration_structures.md#rt_shadow_mask_pipeline-feature)を参照してください。

runtime shader compiler が有効なら source を先に、次に SPIR-V を試します。無効なら SPIR-V だけです。候補選択は [`ShaderLibrary::loadFromStemReference()`](../../src/core/shader/shaderlibrary.cpp#L589) で確認できます。

このほか `.surface` ファイルは [`surfacecompiler`](../../src/core/shader/surfacecompiler.hpp) で GLSL/SPIR-V 化されて pipeline へつながり(WP116/117)、`PELICAN_WITH_SPIRV_LINK=ON`時のオフライン SPIR-V linking は [`spvlink.hpp`](../../src/core/shader/spvlink.hpp) と `spvlink` CLI が担います。feature の scalar params は shader define へ変換され(WP114)、compile 結果は shader cache に保存されます(`shader_cache_test`)。

> 🧩 **難所 — 消さないための空呼び出し**([`makeTemplateHookStubs()`](../../src/core/shader/surfacecompiler.cpp#L991) / [`makeUserLibrarySource()`](../../src/core/shader/surfacecompiler.cpp#L1151))
>
> **何をする所か**: spvlink 経路で、**同じ仮想 include 名 `__pelican_user_surface.glsl` に中身の違う 2 つのソースを差し込んで 2 回コンパイル**する所です。template 側にはフックの空実装(stub)、user 側には本物の `.surface` コードを入れます。
>
> **素朴に読むと**: 両方の生成コードに現れる `keep_alive` — `pelican_param_foo(); pelican_sample_bar(vec2(0.0)); pelican_light_count(); …` という**戻り値を捨てるだけの呼び出しの羅列**の意図が分からないと読めません。正体は DCE(dead code elimination)よけです。template 側はフック本体が空だと `pelican_param_*` / `pelican_sample_*` の定義ごと消され、**それにぶら下がる descriptor 宣言(material UBO・texture binding)まで消えます**。user 側は、**Export 対象(= 実際に書かれたフック)と全アクセサ**を `main()` から呼んでおかないとその関数が消えます。呼ぶのが「全フック」でないのが要点で、生成される `main()` は `if (surface.hooks.surface_v1)` のようにフックごとガードされています([`surfacecompiler.cpp` 内](../../src/core/shader/surfacecompiler.cpp#L293))— 書かれていないフックは定義自体が無く、呼べば compile error になるからです(アクセサ側の呼び出しは無条件、#L249-L259)。その `main()` は link 前に捨てられるので、**生成された `main` は最初から捨てるために書かれています**。
>
> **骨子**:
> ```mermaid
> flowchart TB
>     Compile["compileExperimentalStage(stage)"]
>     Hooks{"hooks が空(depth pass 等)?"}
>     Only["template を 1 本コンパイルして終わり<br/>link しない"]
>     Template["A) template compile<br/>user include の中身だけ stub へ差し替え"]
>     User["B) user compile<br/>アクセサは 0 を返すダミー定義<br/>本物のユーザーコードを #include"]
>     Link["C) linkSpirvModules(A, B)"]
>     Compile --> Hooks
>     Hooks -->|"はい"| Only
>     Hooks -->|"いいえ"| Template --> User --> Link
>     Template -->|"A の成果物"| Link
> ```
>
> - **template compile**: param/texture は常に、resource port は **その stage の分だけ** 空呼び出し。
> - **user compile**: main() から authored フック(hooks でガード)と全アクセサを呼ぶ。main() は後で削除。
>
> **手がかり**: `template_options.virtual_includes` を走査して**同じ名前の中身だけを差し替える** 3 行が「逆 include」の実体です。[`makeUserInclude()`](../../src/core/shader/surfacecompiler.cpp#L220) は生成ソースの先頭に `#line <code_line> "<元ファイル名>"` を置き、glslang のエラー行番号を `.surface` の実際の行へ翻訳します([`diagnosticSourceName()`](../../src/core/shader/surfacecompiler.cpp#L213) が `\` → `/`、`"` → `'` に置換するのは `#line` のファイル名がダブルクォート文字列だから)。この経路はbuild時の`PELICAN_WITH_SPIRV_LINK=ON`と実行時の`PELICAN_SPV_LINK=experimental`が揃ったときだけで、既定は従来の source composition です。**読み始める前にどちらの経路かを確定させてください**。テストは [`surfacecompiler_test.cpp`](../../test/surfacecompiler_test.cpp)。
>
> **不変条件**: stub 側と user 側でフックの**シグネチャが完全一致**していること。`keep_alive` の呼び出しは 1 つでも削ると対応する descriptor が消えます(「使っていないから消す」は成立しません)。

> 🧩 **難所 — リンクの向きは二方向**([`prepareTemplate()`](../../src/core/shader/spvlink.cpp#L481) / [`prepareUser()`](../../src/core/shader/spvlink.cpp#L497))
>
> **何をする所か**: 上で割った 2 本の SPIR-V を SPIRV-Tools の linker で 1 本に繋ぐための下ごしらえです。`OpDecorate … LinkageAttributes` を貼り、import 側の関数本体を剥がします。
>
> **素朴に読むと**: 「ユーザー関数を template に注入する」片方向だと思って読むと、2 つの関数がほぼ鏡像になっている理由が分かりません。実際は **2 方向**で、`user_exports`(`pelican_surface_v1` などのフック)は user 側で Export・template 側で Import、`template_exports`(`pelican_light` / `pelican_shadow` / `pelican_param_*` などエンジンが提供するアクセサ)は template 側で Export・user 側で Import です。片方向だけにすると、ユーザーコードが `pelican_light()` を呼んだ瞬間に「未定義シンボル」ではなく **user 側の 0 を返す stub がそのまま残る**ので、compile も link も validation も通り、**絵だけが黒くなります**。
>
> **骨子**:
>
> | 処理 | 対象 | 変更 |
> |---|---|---|
> | prepareTemplate | user_exports | Import + 本体を宣言だけに剥がす |
> | prepareTemplate | template_exports | Export |
> | prepareUser | user_exports | Export |
> | prepareUser | template_exports | Import + 本体剥がし |
> | prepareUser | entry point `"main"` とその関数本体 | 削除(残すと entry point が 2 つ) |
>
> **export 集合は stage ごとに違う**: `template_exports` を組む [`engineExports()`](../../src/core/shader/surfacecompiler.cpp#L1266) は、resource port 由来のアクセサ(`pelican_sample_*` / `pelican_size_*` / `pelican_load_*` / `pelican_count_*`)を **port が宣言した stage で絞ります**([`generatedAccessorNames()`](../../src/core/shader/surfacecompiler.cpp#L1119) の `resourceStages(resource.stage) & stage`)。絞りが無いと、たとえば vertex 専用 port のアクセサ名が fragment の export 集合に残ります。テンプレート側の stub が同じ stage 条件で `keep_alive` の呼び出しを省く以上、その関数は生き残らないので、[`findNamedId()`](../../src/core/shader/spvlink.cpp#L156) が `"SPIR-V symbol '…' was not found"` を投げてリンクが落ちます。**resource port については、[`makeTemplateHookStubs()`](../../src/core/shader/surfacecompiler.cpp#L991) が `keep_alive` を出す条件と `engineExports()` が名前を出す条件が同じ式でなければなりません**(片方だけ触ると壊れます)。なお `params` / `textures` / `screen_inputs` は `generatedAccessorNames()` 側では絞られず、user 側の [`makeUserLibrarySource()`](../../src/core/shader/surfacecompiler.cpp#L1151) も resource port のダミー定義と `main()` からの呼び出しを stage で絞りません — 絞っているのは template 側の 2 か所だけです。
>
> **手がかり**: [`symbolMatches()`](../../src/core/shader/spvlink.cpp#L149) は、glslang が `pelican_surface_v1(struct-PelicanSurfaceInputV1…;` のようにマングルして吐く `OpName` を、前方一致 + 直後の 1 文字が `( @ $ .` のいずれか、で判定します(複数一致は "is ambiguous" で例外)。[`addLinkageDecoration()`](../../src/core/shader/spvlink.cpp#L196) の挿入位置が `opcode >= SpvOpTypeVoid && opcode <= SpvOpTypeForwardPointer` という **opcode の数値レンジ**なのは、SPIR-V の logical layout が「全 decoration → 型セクション」の順を要求し、型 op が連番だからです。[`normalizeAbiDecorations()`](../../src/core/shader/spvlink.cpp#L368) を外すと、同じ GLSL struct から出た型なのに「型が違う」と言われて link が落ちます。テストは [`spvlink_test.cpp`](../../test/spvlink_test.cpp)。
>
> **不変条件**: Import 側の関数は本体を持たず、Export 側は定義を 1 つだけ持つこと。ABI に出せる型は scalar / vec2-4 / 単純 struct / Function ポインタのみで、array・matrix・Block 装飾された struct・リソースハンドルは意図的に禁止です(2 モジュール間で layout の一致が保証できないため)。`cache_key` は toolchain revision まで含むので、SPIRV-Tools を上げると全再リンクになるのが正しい挙動です。

### `ShaderBundle`

[`ShaderBundle`](../../src/core/shader/shaderlibrary.hpp#L30) は次をひとまとめにします。

- `vk::UniqueShaderModule`
- `ShaderReflection`
- source path
- compile define 群
- version
- 最後の compile log

source compile は [`ShaderCompiler`](../../src/core/shader/shadercompiler.hpp#L75)、SPIR-V 解析は SPIRV-Reflect を使う [`reflect()`](../../src/core/shader/shaderreflection.cpp#L203) が担当します。reflection で取得するものは descriptor set/binding/type/count/name、push constant range、vertex input、compute local size です。

✅EL-4c から、storage image の binding は [`ReflectedBinding::storage_image_format`](../../src/core/shader/shaderreflection.hpp#L44) に GLSL の layout qualifier の format を持ちます。SPIR-V の image format を Vulkan の format へ写すのは [`storageImageFormat()`](../../src/core/shader/shaderreflection.cpp#L84) で(`r8` は `R8_UNORM`)、qualifier が無い image と storage image 以外の descriptor は `eUndefined` です。今これを読むのは `get_engine_shaders` の `resource_ports[].format` だけで、vocabulary の名前に直せた format だけを出します([付ける所](../../src/core/communication/engineauthoringlists.cpp#L305))。

✅EL-4c から、module に登録された `ShaderCompiler` とは別に、[`ShaderCompiler::detached()`](../../src/core/shader/shadercompiler.hpp#L91) が module を読まない compiler を作ります。通常の constructor は `PathResolver` から project の `.pelican/shader_cache` を disk cache に取りますが、[detached の constructor](../../src/core/shader/shadercompiler.cpp#L711) は何も取らず(disk cache 無し、memory cache はその instance のもの)、`records_startup_metrics` を false にして `StartupMetrics` に compile の時間と cache の数を足しません。

`get_engine_shaders` の反映の worker thread がこれを 1 つ持ち、frame の側の compiler と状態を共有しません([使う所](../../src/core/communication/engineauthoringlists.cpp#L380)。RPC の側は [第7章](07_tools_rpc_tests.md) の `get_engine_shaders` の行)。

**disk cache の置き場と鍵(✅TS-1、`88571042`)。**GPU の test は毎回新しい temp project を作るので、project ごとの cache が毎回空でした。TS-1 は置き場を移せるようにし、鍵から project の場所を外しました。

- **置き場**: [`ShaderCompiler::ShaderCompiler()`](../../src/core/shader/shadercompiler.cpp#L711) は PathResolver が setup 済みなら `<project>/.pelican/shader_cache` を取り、環境変数 [`PELICAN_SHADER_CACHE_DIR`](../../src/core/shader/shadercompiler.hpp#L44) が空でなければそれに置き換えます(Windows は `_wdupenv_s` で読む。[`sharedShaderCacheDirectory()`](../../src/core/shader/shadercompiler.cpp#L677))。変数は置き場を移すだけで、project を持たない compiler(detached と、PathResolver の無い compiler)には cache を与えません —— それを前提にした test がある、というのが仕様書から外れた理由です。`setCacheDirectory()` で明示した directory は変数に左右されません。変数を設定するのは GPU test の launcher `pelican_gpu_lease_run`([第7章 §7.8](07_tools_rpc_tests.md))で、子を起こす前に、未設定なら `%LOCALAPPDATA%/pelican/test_shader_cache` を作って設定します([`prepare_shader_cache_directory()`](../../test/gpu_lease_run.cpp#L136)、[呼ぶ所](../../test/gpu_lease_run.cpp#L387)。`PELICAN_GPU_LEASE=off` の判定より前。使い方は [manual 第10章 §10.9](../manual/10_tools.md))。project の cache directory を列挙して miss を数える GPU test「WP366 renderer A B recovers q r group miss and WP384 preserves frame publication」は、test の中で変数を外します([該当箇所](../../test/headless_render_test.cpp#L10900))。
- **鍵**: [`shaderCacheKey()`](../../src/core/shader/shadercompiler.cpp#L446) は source graph の SHA-256、define、shaderc の版と実行時の SPIR-V の版 / revision、target env、`compile-options`([`shaderCompileOptions`](../../src/core/shader/shadercompiler.cpp#L65)。target env の版・最適化なし・debug info なし。TS-1 で足した)、stage、entry point、contract salt を並べた SHA-256 です。[`sourceGraphSha256()`](../../src/core/shader/shadercompiler.cpp#L299) は include を辿ったファイルを、絶対 path ではなく [`keyFileName()`](../../src/core/shader/shadercompiler.cpp#L268) の「それを含む最初の include root の番号 + `/` + root からの相対 path」で名付け(どの root の下にも無いファイルは絶対 path のまま。`engine://` と virtual source は自分の名前)、include root の下の全ファイルも「番号/相対名」と中身で並べ、最後に engine に埋め込まれた全 shader source の SHA を足します。中身と相対位置が同じなら別の場所の project とも同じ鍵になります。名付け方が変わったので、TS-1 の前に書かれた project の cache は一度だけ全部外れます。
- **鍵を作る費用**: 鍵のためのファイルの読み込みは compiler ごとの `ShaderKeyMemory` に (path, size, 更新時刻) で覚え、変わっていなければ読み直しません([`readKeyFile()`](../../src/core/shader/shadercompiler.cpp#L241))。更新時刻が今から 2 秒以内のファイルは覚えません(同じ大きさの 2 回の書き込みが 1 刻みに入ると変化を見落とし、古い SPIR-V に当たるため)。観測点は [`ShaderCompiler::keyFileReadCount()`](../../src/core/shader/shadercompiler.hpp#L99)。include root の無い graph の SHA は root・virtual source・埋め込みの engine source だけの関数なので、process 全体の表([`processSourceGraphs()`](../../src/core/shader/shadercompiler.cpp#L233))で使い回します(以前は compiler ごと)。✅TS-2(`4be89f7e`)から、鍵・cache の中身の照合・bundle の `spirv_sha256` の SHA-256 は picosha2 ではなく [`sha256Hex()`](../../src/project/sha256stream.hpp#L35)(Windows は BCrypt)で計算します。include の走査は [`mayHoldIncludeDirective()`](../../src/core/shader/shadercompiler.cpp#L287) が、行頭の空白の後が `#` でない行と `include` の語を含まない行で regex を呼ばず(最初の文字が印字可能な ASCII 以外なら regex に回す)、include root 以下の列挙は root を 1 回だけ解決して普通のファイルを「解決した root + 相対名」にします([該当箇所](../../src/core/shader/shadercompiler.cpp#L408)。symlink などは 1 件ずつ解決)。計算の仕方が変わっただけで**鍵の値は変わりません**。これを見る [「TS-2 a project shader's cache key is the documented digest of its sources」](../../test/shader_cache_test.cpp#L353) は、期待値を固定の hex で書かず、鍵の構成を test の中で書き下して比べます(engine の shader を 1 byte 変えると鍵が変わるため。build に依る値は [`shaderCacheKeyBuildInputs()`](../../src/core/shader/shadercompiler.hpp#L60) から読む)。報告の数字は RT6 named TLAS で 176 s → 98.5 s(前後 1 回ずつ)です。ただし TS-1 が測った鍵の計算は 32 s で、減った分はそれより大きく、他の負荷の差が混ざっている可能性がある、と報告に書かれています。
- **読み書き**: entry は 1 行目の形式名、鍵(16 進)、SPIR-V の SHA-256、word 数の後に SPIR-V が続き、[`readCache()`](../../src/core/shader/shadercompiler.cpp#L485) は鍵と SPIR-V の SHA が両方合わなければ捨てて compile し直します。書き込みは同じ directory の一時ファイルから rename で、別の process が同じ鍵を先に書いていれば消して置き直します(これは TS-1 の前から)。engine は cache の directory を掃除しません。GPU test の launcher が自分で決めた共有 directory だけは、✅TS-2 から launcher が 1 日 1 回、14 日書かれていないファイルを消します([第7章 §7.8](07_tools_rpc_tests.md))。
- **`entry_digest`**: RT の entry ごとの module の SHA は、使うのが procedural の oracle の状態(intersection の entry)だけなので、intersection の entry だけで計算します([該当箇所](../../src/core/shader/rtspirvinspection.cpp#L845))。`rtspirvinspection.hpp` の `entry_digest` の説明はこれに合わせて直していません。
- test: `shader_cache_test` の [「TS-1 a shared shader cache directory serves an engine shader to a second project」](../../test/shader_cache_test.cpp#L196)・[「TS-1 without the shared cache variable a project keeps its own shader cache」](../../test/shader_cache_test.cpp#L230)・[「TS-1 a project shader shares its key across project locations and misses on an include byte」](../../test/shader_cache_test.cpp#L247)(当たった SPIR-V が cache 無しの compile と byte で一致すること、同じ bytes でも相対位置が違えば外れることも見る)・[「TS-1 a cache key reads an unchanged include file once per compiler」](../../test/shader_cache_test.cpp#L293)。

✅EL-5(`59e5d792`)の `get_shader_interface` は、project の shader の stem を同じ形で反映します。engine の shader と project の shader で違うのは「形がどこにあるか」だけなので、要素を作る関数は 1 つです。

- **形の場所**: `ShaderFormLocation {name, engine_id, file?}` が 1 つの形(GLSL の source か SPIR-V)の在りかで、`ShaderStageForms {reference, stage, source?, spirv?}` が stem の 1 stage です。`readForm()` は file があれば disk から、無ければ engine の資源から bytes を読みます。[`shaderStageJson()`](../../src/core/communication/engineauthoringlists.cpp#L251) が `get_engine_shaders` と `get_shader_interface` の両方の要素を作り、source の compile は [`compileForm()`](../../src/core/communication/engineauthoringlists.cpp#L190) が file なら `ShaderCompiler::compileFile()`(include は file の directory、`<directory>/shaders/include`、engine の資源の順。[`compileFile()`](../../src/core/shader/shadercompiler.cpp#L761))、engine の資源なら `compileSource()` で行います。file の compile の log は、`weakly_canonical` した絶対パスを形の名前(`project://shaders/<file>`)に置き換えます。
- **stage の集め方**: [`shaderInterfaceStatusJson()`](../../src/core/communication/engineauthoringlists.cpp#L624) が stage ごとに [`shaderStemCandidateReference()`](../../src/core/shader/shaderlibrary.cpp#L557) の候補(source と `.spv`)を `resolve`(`PathResolver::resolveProjectRef()`)で解き、[`existingShaderStemCandidate()`](../../src/core/shader/shaderlibrary.cpp#L561) で在るかを見ます。`existingShaderStemCandidate()` は `ShaderLibrary::loadFromStemReference()` の候補の検査を切り出したもので、library も同じものを呼びます。`shaderStemCandidateReference()` は library と同じ `shaderSourceCandidateReference()` を包むだけです。在った形の bytes の SHA-256 を `<候補>\n<sha256>\n` の列にして、その列の SHA-256 を答えの key にします。ここまでは RPC の thread で、compile はしません。
- **worker**: [`ShaderInterfaceReflection`](../../src/core/communication/engineauthoringlists.cpp#L476) が reference ごとの `Record {wanted, wanted_key, done, answer}` と待ち行列(`std::deque<ShaderInterfaceJob>`)を持ちます。[`status()`](../../src/core/communication/engineauthoringlists.cpp#L535) は、key が同じで `refresh` でなければ持っている答えか `building` を返し、そうでなければ世代を進めて、待っている同じ reference の job を消してから最後に積みます(worker が動いていなければここで始めます)。worker は自分の `ShaderCompiler::detached()` を 1 つ持ち、job を 1 つずつ取り出して stage ごとに `shaderStageJson()` を呼び、終わったときに record の `wanted` がその job の世代のままなら答えを置きます(その間に新しい依頼が来ていれば捨てる)。
- **止める**: `EngineRpcEndpoint::Impl` の destructor が `stopEngineShadersReflection()` の後に [`stopShaderInterfaceReflection()`](../../src/core/communication/rpcserver.cpp#L2860) を呼びます。worker は stage の区切りか待ちの中で抜け、join の後、待ち行列を捨て、出来ていない record の `wanted` を `done` に戻します(出来上がった答えは残り、次の依頼は作り直します)。
- **`engine://`**: `get_engine_shaders` の答え(`engineShadersStatusJson()`)からその reference の要素を抜くだけで、別の反映はしません。
- **穴**: compile に rendering 節の `shader_defines` を渡していません(`compileFile()` を define 無しで呼びます)。include の中身は key に入らないので、include の変更は `refresh` まで答えに出ません。

vertex と fragment の reflection は [`merge()`](../../src/core/shader/shaderreflection.cpp#L335) で統合します。同じ set/binding の型または配列数が違う、push constant の offset/size が違う場合は pipeline 作成前に例外になります。✅RS-15 から、2 つの stage が同じ set 1(`PELICAN_SET_PASS_INPUT`)の binding を宣言し、片方だけがその binding を可変の組(名前の接頭辞 `pelican_variadic_input_`)として宣言しているときは、組の側の名前を残します([該当箇所](../../src/core/shader/shaderreflection.cpp#L367))。

raster の vertex が入力 0 を固定の宣言で読み、fragment の `engine://passthrough` が同じ binding を組で宣言する pass で、先に合わせた vertex の名前が残って `readPassInputLayout()` が組に穴を見つけ、`variadic pass inputs are malformed` で断っていたためです(manual 第6章 §6.4「可変の入力」)。

### descriptor set と push constant の契約

エンジンと shader の予約規約は [`pelican_sets.hpp`](../../src/core/shader/pelican_sets.hpp#L15) です。

| set | 定数 | 用途 |
|---:|---|---|
| 0 | `PELICAN_SET_FRAME` | camera/light など frame 共通データ |
| 1 | `PELICAN_SET_PASS_INPUT` | fullscreen 入力、compute resource |
| 2 | `PELICAN_SET_MATERIAL` | material texture/data |
| 3 | `PELICAN_SET_FREE` | 自由枠 |

binding 定数は各 set の用途に応じて定義されています([`pelican_sets.hpp` 内](../../src/core/shader/pelican_sets.hpp#L26)): `FRAME_UBO=0` / `OBJECT_BUFFER=1` / `LIGHT_UBO=2` / `PREVIOUS_OBJECT_BUFFER=3`(TAA velocity 用)/ `MATERIAL_BUFFER=6` のほか、skin palette、morph 系、material instance override 系の binding があります。

push constant は engine 64 bytes + shader 64 bytes、合計128 bytesを契約値としています。実際の pipeline layout は reflection された range から [`createPipelineLayout()`](../../src/core/shader/pipelinefactory.cpp#L549) が作ります。

✅LC-1 から、shader 側の半分の `[112, 120)` を graph のループの回の番号に使います([`PELICAN_PUSH_LOOP_OFFSET = 112` / `PELICAN_PUSH_LOOP_BYTES = 8`](../../src/core/shader/pelican_sets.hpp#L78)、GLSL 側は `pelican_sets.glsl` と include の `pelican_loop.glsl`。書き方は manual 第6章 §6.18「回の番号を shader に渡す」)。流れは次のとおりです。

- **値を決める**: 合成の後、[`loopIndicesOf()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L359) が compiled pipeline の `pass_provenance` の `loop_origin.element` と `graph_loops` の回数から、展開された pass と compute task の名前ごとに [`PassLoopIndex`](../../src/project/passloopindex.hpp#L11) `{element, count}`(✅RC-4 から project) を作り、`PassDefinition::loop_index` / `ComputeTaskDefinition::loop_index` に入れます(JSON からは読まない runtime だけの field。ループの外は `{0, 1}`)。compute task は名前で既に登録されていれば再利用されるので、組み直しで回数が変わったときのために `registerComputeTask()` が `loop_index` だけを書き直します。
- **送る**: fullscreen / raster は `FullscreenPassRenderer::render()` が descriptor を bind した後に、compute task は `ComputeTaskContainer::dispatch()` が(ray tracing の task かどうかを分ける前に)、[`PipelineFactory::pushLoopIndex()`](../../src/core/shader/pipelinefactory.cpp#L1933) を呼びます。これは pipeline の merge 済み reflection から、range が `[112, 120)` を丸ごと含む stage を [`loopIndexPushStages()`](../../src/core/shader/shaderreflection.cpp#L549) で集め、その OR の stage flags で `pushConstants` を 1 回呼びます(どの stage も含まなければ何もしない)。layout の range は今までどおり `makePushConstantRanges()` が stage ごとに 1 本へ潰したものなので、include の block と自前の block(`PELICAN_LOOP_CUSTOM_PUSH`)のどちらでもこの判定に乗ります。
- **断る**: [`validateLoopIndexPushRange()`](../../src/core/shader/shaderreflection.cpp#L560) が、range が `[112, 120)` に一部だけ重なる stage を `shader.loop_push_overlap@1: {shader, stage, range}` の `std::runtime_error` で断ります。呼ぶのは `buildGraphicsPipeline()`(vertex と fragment を別々に)と `buildComputePipeline()` で([graphics](../../src/core/shader/pipelinefactory.cpp#L1044)、[compute](../../src/core/shader/pipelinefactory.cpp#L1088))、ray tracing の pipeline は検査しません。render config editor は code を carried refusal として扱います([`isCarriedRefusalCode()`](../../src/core/communication/renderconfigeditor.cpp#L89))。
- **見せる**: [`readsLoopIndex()`](../../src/core/shader/shaderreflection.cpp#L556) が `get_frame_plan` の node([fullscreen / raster](../../src/core/vkcore/renderer.cpp#L4641)、[compute](../../src/core/vkcore/renderer.cpp#L4657))と `get_engine_shaders` の項目([付ける所](../../src/core/communication/engineauthoringlists.cpp#L281))の `reads_loop_index: true` になります。
- material の renderer は自分の push(0〜72 byte)を送るだけで `pushLoopIndex()` を呼ばないので、material pass と draw_loop は対象外です。fullscreen pass の `push_constants`(`camera_position` / `projection_view`)は pass の契約の flag になるだけで([`PassImplementationRegistry` の分岐](../../src/core/renderingpass/passimplementationregistry.cpp#L246))、fullscreen の描画が `pushConstants` を呼ぶのは LC-1 の回の番号だけです。

> 🧩 **難所 — push の先頭 64 byte**([`makePushConstantRanges()`](../../src/core/shader/shaderreflection.cpp#L471))
>
> **何をする所か**: merge 済み reflection の push constant range 群を検証し、`VkPipelineLayoutCreateInfo::pPushConstantRanges` へ渡せる形 —「stage ごとにちょうど 1 本の区間」— へ畳み込みます。
>
> **素朴に読むと**: 非自明が 2 つ同居しています。第一に、[`merge()`](../../src/core/shader/shaderreflection.cpp#L335) は push constant を**検証も重複排除もせず単に連結するだけ**です(`merge()` 自身が投げるのは binding の type/count 不一致と compute local size 不一致のみで、push constant の契約検査はここではなく `makePushConstantRanges()` が行います)。したがって vert と frag が 1 本ずつ持ったまま到着し、そのまま Vulkan へ渡すと「同じ stage を 2 つの range に含めてはならない」に触れます。だから stage 単位で min(offset) と max(offset+size) を取り、**区間を 1 本に潰す**必要があります。第二に engine 領域の条件で、`offset < 64` なら `offset == 0 && end >= 64` でなければ弾く、という書き方です。意味は「先頭 64 byte の MVP に少しでも掛かるなら `[0,64)` を丸ごと覆え」。部分的な上書きを静かに通すと `engineMvp` の一部だけが shader の値で潰れます。
>
> **骨子**:
> ```mermaid
> flowchart TB
>     Validate["1. 各 range を単体検証"]
>     Scan["2. stage_bit = 1,2,4,… と 32 ビットすべてを走査<br/>stage_bit != 0 が終了条件"]
>     Bounds["3. その stage を含む range 全部から<br/>begin=min(offset), end=max(offset+size)"]
>     Group["4. grouped_ranges[{begin,end}] |= stage"]
>     Emit["map を走査して 1 本ずつ吐く"]
>     Validate --> Scan --> Bounds --> Group
>     Group -->|"次の stage"| Scan
>     Group -->|"走査後"| Emit
> ```
>
> - **各 range の検証**: size>0 / 4 byte 倍数 / offset+size <= 128。engine 領域に掛かるなら [0,64) を完全被覆。
>
> **手がかり**: `begin` の初期値 `numeric_limits<uint32_t>::max()` は「この stage を使う range が 1 本も無かった」の番兵で、`begin != max` が存在判定です。キーが `pair<begin,end>` なので、たまたま同じ区間になった vertex と fragment は 1 本の range に stage フラグ 2 つで出ます。定数は [`pelican_sets.hpp`](../../src/core/shader/pelican_sets.hpp#L72)(engine 64 + shader 64 = 128)。検証だけしたいとき用の薄いラッパが [`validatePushConstantContract()`](../../src/core/shader/shaderreflection.cpp#L516) です。
>
> **不変条件**: 出力の range 群は、**どの stage bit も高々 1 本にしか現れない**こと(崩すと pipeline layout 作成が validation error になります)。合計 128 byte・engine 先頭 64 byte は shader 側 GLSL と対の仕様で、片側だけ動かせません。

### define と SPIR-V

compile define は source compile 時にしか適用できません。engine resource が SPIR-V なのに define があれば [`loadResolvedReference()`](../../src/core/shader/shaderlibrary.cpp#L516) が拒否します。feature composition が shader define を足す構成では runtime compiler を有効にするか、define ごとの SPIR-V variant を別 resource として用意する必要があります。

## 6.10 PipelineFactory と hot reload

[`PipelineFactory`](../../src/core/shader/pipelinefactory.hpp#L310) は graphics/compute pipeline を作り、handle で保持します。

graphics pipeline 作成は次の順です。

1. shader bundle の reflection を merge。
2. set ごとの descriptor layout key を作る。
3. 同じ binding signature の descriptor set layout を cache から再利用。
4. reflection から pipeline layout を作る。
5. target format を `vk::PipelineRenderingCreateInfo` に入れ、Dynamic Rendering pipeline を作る。

実装は [`buildGraphicsPipeline()`](../../src/core/shader/pipelinefactory.cpp#L998) と [`createGraphicsPipeline()`](../../src/core/shader/pipelinefactory.cpp#L556) です。dynamic state は viewport と scissor が常にあり、`GraphicsPipelineDesc::dynamic_front_face` が立っている pipeline だけ front face が加わります(✅CC-1。立てるのは material・velocity・shadow depth。[足す所](../../src/core/shader/pipelinefactory.cpp#L661)。§6.5 の「面の表は view ごとに決まります」)。また driver pipeline cache を executable 隣の `pipeline_cache.bin` から読み書きします。保存は [`PipelineFactory::~PipelineFactory()`](../../src/core/shader/pipelinefactory.cpp#L485) から呼ばれます。

hot reload の流れは次です。

```mermaid
flowchart TB
    Watch["1. FileWatcher / ContentDigest<br/>2. ReloadService::applyRequests()"]
    Prepare["ShaderLibrary::prepareReload()<br/>3. 依存解決 → 4. compile/reflect"]
    subgraph Rebuild["PipelineFactory::rebuildPrepared()"]
        Candidate["5. pipeline candidate 構築<br/>6. material の prepare"]
        Success{"全成功?"}
        Publish["bundle/pipeline/material を一括 publish"]
        Discard["candidate だけ破棄し旧世代を維持"]
        Candidate --> Success
        Success -->|"はい"| Publish
        Success -->|"いいえ"| Discard
    end
    Render["Renderer::render()<br/>7. consume → 8. rebind"]
    Watch --> Prepare --> Candidate
    Publish --> Render
```

図中の番号に対応する処理:

1. `FileWatcher / ContentDigest`: frame start。
2. `ReloadService::applyRequests()`: 同フレームの変更を batch 化。
3. `ShaderLibrary::prepareReload()`: root/include/.surface reverse dependency を解決。
4. 影響する全 variant を compile/reflect。live bundle は未変更。
5. `PipelineFactory::rebuildPrepared()`: 依存 pipeline を全て candidate 構築。
6. 必要なら `.surface + .material.json` の layout/values も prepare。
7. 成功して一括 publish した後、`Renderer::render()` の render start で publish 通知を consume。
8. fullscreen input descriptor を rebind。

shader candidate は [`ShaderLibrary::prepareReload()`](../../src/core/shader/shaderlibrary.cpp#L1179)、group-wide な pipeline publish は [`PipelineFactory::rebuildPrepared()`](../../src/core/shader/pipelinefactory.cpp#L1958) が担当します。material peer の準備は [`MaterialContainer::prepareSurfaceMaterialReload()`](../../src/core/material/materialcontainer.hpp#L407) が担います。compile error、pipeline 作成失敗、material peer の検証失敗のいずれでも、最後に成功した世代を残します。cache hit/miss と追跡中の unit/bundle/dependency 数は `get_status.reload.runtime.pelican.shaders.details` から確認できます。

> 🧩 **難所 — reload の swap は 3 回**([`rebuildPrepared()`](../../src/core/shader/pipelinefactory.cpp#L1958) / [`ShaderLibrary::activatePrepared()`](../../src/core/shader/shaderlibrary.cpp#L1284))
>
> **何をする所か**: 上の疑似コードの「全成功時だけ一括 publish」を、shader bundle・pipeline・cross-domain な material candidate をまたいだ 1 トランザクションとして実現します。
>
> **素朴に読むと**: `activatePrepared()` が **idempotent な「有効化」ではなく `std::swap` の反復適用(= 対合)**(idempotent は「何回呼んでも 1 回呼んだのと同じ」、対合(involution)は「2 回呼ぶと元に戻る」で、ここでは対照的な性質です)だと気づかないと、この関数は読めません。実装は `swap(bundles.get(candidate.id), candidate.replacement)` の 1 行だけで、呼ぶたびに live 側と candidate 側が入れ替わります。したがって意味は「呼んだ回数の偶奇」で決まり、成功経路では **3 回**呼ばれます([1 回目は候補構築の直前](../../src/core/shader/pipelinefactory.cpp#L2007) / [2 回目はその復帰コメントの直後](../../src/core/shader/pipelinefactory.cpp#L2117) / [3 回目は finalize の直前](../../src/core/shader/pipelinefactory.cpp#L2140))。1 回足したり消したりすると、旧 SPIR-V を指したまま publish する / 新世代を捨てたつもりが live に残る、という**例外も log も出ない静かな**破壊になります。
>
> **骨子**:
> ```mermaid
> stateDiagram-v2
>     state "live: old" as Old
>     state "live: new<br/>swap#1 後: 候補構築" as Candidate
>     state "live: old<br/>swap#2 後: before_publish" as Before
>     state "live: new<br/>swap#3 後: publish(以降 throw しない)" as Published
>     state "live: old<br/>(a) 候補構築が throw" as FailedCandidate
>     state "live: old<br/>(b) before_publish が失敗" as FailedBefore
>     Old --> Candidate
>     Candidate --> Before
>     Before --> Published
>     Candidate --> FailedCandidate
>     Before --> FailedBefore
> ```
>
> - **(a) 候補構築の失敗**: catch 内で swap して旧世代へ戻し、discard_new_layouts()。
> - **(b) before_publish の失敗**: swap#2 済みなので live はすでに旧世代。swap を呼ばないで、discard_new_layouts() だけして return。
>
> **手がかり**: 「Restore the live shader table while the cross-domain material candidate commits.」というコメント([該当行](../../src/core/shader/pipelinefactory.cpp#L2117))が swap#2 の理由そのものです。読み飛ばさないでください。上の (a) と (b) の**非対称**も同じくらい重要で、(b) の経路([`before_publish` の失敗処理](../../src/core/shader/pipelinefactory.cpp#L2122))は `activatePrepared()` を**呼びません** — 呼ぶと偶奇が狂って新世代が live に残ります。対になる非トランザクション版が `rebuildDirty()` で、こちらは pipeline ごとに try/catch していて「一部だけ更新される」— 両者の差を意識して読みます。現在は `graphics_overrides`([`GraphicsPipelineReloadOverride`](../../src/core/shader/pipelinefactory.hpp#L269))と `before_publish` コールバックが引数に加わり、graph 再コンパイルと shader reload が同じトランザクションに束ねられています。公開後の旧 pipeline/layout は即破棄せず [`replacePipeline()`](../../src/core/shader/pipelinefactory.cpp#L1431) が DeletionQueue へ回します(§6.11)。境界の全体像は [第9章](09_black_magic_and_gotchas.md)。
>
> **不変条件**: 成功経路の `activatePrepared()` 呼び出しは奇数回で終わること。publish フェーズは **絶対に throw しない**(throw しうる処理はすべて `before_publish` までに済ませる)。失敗経路は必ず `discard_new_layouts()` を通すこと — 通らないと live でない descriptor set layout が cache に居座ります。

`engine://` の埋め込み source/SPIR-V は物理 `AssetKey` を持たないため自動 reload 対象外です。project/mounted-store 上の GLSL、SPIR-V、`.surface` は FileWatcher の対象です(project.json が `hot_reload` を宣言していれば、宣言に入る path のものだけ。✅HW-1、[第3章](03_project_and_loading.md) §3.10 の後の「走査を安くした所と、宣言で絞る所」)。

✅PB-1(`a55d1eaf`)から、shader の読み直しの失敗は log と `get_status.reload` だけでなく、engine の今の問題の一覧(`get_problems`)にも残ります。
`ReloadService` は、file 監視の shader の batch、render pipeline の companion の読み直し、`reload_shaders`(`forceShaderReload()`)の後に、`ShaderLibrary` が「失敗して、まだ公開し直していない」と持つ unit の
primary source の集合(`failedReloadSources()`。`units_failed_since_publish` から引く)を一覧に渡し、一覧の shader の問題をその集合に合わせます。

1 件は `shader_reload_failed@1` で、
compiler の log の最初の error の行の file が文書自身なら `line` が付きます。include が壊れると、それを読む各 shader が 1 件ずつです。形と消え方は manual 第6章 §6.16 の「engine が持つ今の問題の一覧」、
部品の位置は [第7章](07_tools_rpc_tests.md) の「今の問題の一覧(✅PB-1 `a55d1eaf`)」です。

### reflection と reload の resource contract {#shader-reload-boundaries}

reflection は descriptor layout と pipeline layout を source/SPIR-V から自動生成します。便利ですが、C++ 側の resource contract がなくなるわけではありません。

- set 0/1/2/3 の意味は [`pelican_sets.hpp`](../../src/core/shader/pelican_sets.hpp#L15) の convention。
- fullscreen/compute binder は特定 descriptor type を要求。
- material renderer は engine vertex layout/push constant の構造を前提にする。
- reflection は field の semantic を理解せず、set/binding/type/count/name だけを見る。

hot reload は shader compile と pipeline rebuild を transactional にします。しかし descriptor layout を変更する edit は、shader body だけの edit より危険です。

- shader reload の runtime 公開は **`RuntimeReloadBoundary::render_start`** の 1 点に集約されています。[`consumeShaderReloadPublication()`](../../src/core/vkcore/renderer.cpp#L2745) が `ReloadService::applyRuntimeBoundary(render_start)` を呼び、**その summary の `committed` が 0 でないときだけ** [`rebindFullscreenInputs()`](../../src/core/vkcore/renderer.cpp#L2710) が走ります。呼び出しは view の記録へ入る前([`renderer.cpp` の frame 前段](../../src/core/vkcore/renderer.cpp#L6326))で、shader 側の participant がこの boundary を宣言している箇所は [`reloadservice.cpp` の shader participant 登録](../../src/core/watch/reloadservice.cpp#L715) です。
- `rebindFullscreenInputs()` が貼り直すのは 3 系統です — 公開済み generation 内の fullscreen / generic raster pass の input resource、material の screen input、compute task の render target。したがって **reload 専用の処理ではありません**。logical target の extent が変わった直後にも同じ関数が呼ばれます([`renderer.cpp` の extent 変更後](../../src/core/vkcore/renderer.cpp#L2821))。逆に言うと、この 3 系統の外側で descriptor を自前 cache している pass は、reload でも resize でも取り残されます。
- compute descriptor set は [`registerComputeTask()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1308) 時に一度作り、hot reload path では作り直していません。resource port の binding も同じで、RS-5 から登録のときに shader の反映の名前で決まりますが、hot reload の pipeline の組み直しは保存した desc の binding のまま行い、名前で引き直しません。
- material は [`MaterialContainer::prepareSurfaceMaterialReload()`](../../src/core/material/materialcontainer.hpp#L407) により surface/material 連動 reload に対応しました。UI/debug の descriptor ownership は各 container に分散したままです。

したがって hot reload の安全な基本範囲は、既存 set/binding/type と push constant layout を保った shader body の変更です。layout-changing reload を正式対応するなら、pipeline 使用者ごとの descriptor rebuild notification が必要です。

### [`descriptor_abi_changed`](../../src/core/material/materialgenerationplanner.cpp#L1552) — 全 descriptor 破棄と revision {#reading-note-039}

`pass_inputs` / `resource_interface` が変わると既存の descriptor set はレイアウト・束縛内容と ABI 不一致になります。[commit 側](../../src/core/material/materialgenerationplanner.cpp#L1552)ではまず **`waitIdle()` して in-flight フレームが古い set を参照し終えるのを待ち**、その後 clear して `++descriptor_revision` します。revision は消費側キャッシュの世代無効化用です。同じ理由の待機が [live な `MaterialBuffer` への in-place 書き込み](../../src/core/material/materialcontainer.cpp#L1606)にもあります(こちらはバッファの差し替えではなく、コメントどおり「in-flight フレームが共有する live SSBO の読み手を守る」ための `waitIdle()`)。

### [variant 次元と parity 次元](../../src/core/material/materialdescriptorbuilder.cpp#L441) — 2 軸が直交する {#reading-note-040}

**variant は sequential なスクリーン入力の array layer**(view ごとの層。[全リソースの layer 数一致を要求](../../src/core/material/materialdescriptorbuilder.cpp#L599))、**parity はダブルバッファされた render target のどちらの実体を読むか**です。2 軸は直交するので (variant, parity) の全組み合わせ分の descriptor set を事前確保し、フレーム中は選ぶだけにします。

### [`rayMaterialTableSource()` の canonical key 検証](../../src/core/material/materialcontainer.cpp#L1811) — `document:` / `asset:` プレフィックス {#reading-note-041}

[§9.23.5](09_black_magic_and_gotchas.md#9235-rtacceleration-structuresbtoracle) の `source_by_handle` / `source_by_key` の照合先で、プレフィックスは [`MaterialRowIdentity`](../../src/core/shader/raygrouplayout.hpp#L305) の 3 択(`MaterialRowDocumentIdentity` / `MaterialRowAssetIdentity` / `MaterialRowIdentityUnavailable`)に対応します(`MaterialRowDocumentIdentity` 自体は document 側の 1 状態にすぎません)。ゲートと lookup が同じ値を見る理由は[第11章](11_ray_query_acceleration_structures.md)が本体です。**プレフィックス文字列そのものの版管理規約は未執筆。**

### [`ReloadService::registerParticipant()`](../../src/core/watch/reloadservice.cpp#L151) — 型が言えないことだけを検査する {#reading-note-058}

適用ルートの排他は検査ではなく型になりました。[`FileReloadRole::apply`](../../src/core/watch/reloadservice.hpp#L171) は [`PerRequestReloadApply`](../../src/core/watch/reloadservice.hpp#L135)(1 件ずつ)/ [`BatchReloadApply`](../../src/core/watch/reloadservice.hpp#L143)(まとめて)/ [`CompanionBatchReloadApply`](../../src/core/watch/reloadservice.hpp#L154)(他 participant の request を吸い上げて 1 世代で publish)の `std::variant` で、二つ持つこと自体が書けません。

ここに残っているのは**型が言えないことだけ**です — 名前が空でないこと、選ばれたルートの callback が null でないこと、companion apply なら companion 名が 1 つ以上あり空・自己参照・重複でないこと、そして名前が一意であること。batch owner 同士の順序も宣言から導かれます: ある owner が別の owner の companion に名指されていれば、名指した側の**後**に適用されるので、どちらが先に登録されたかで外側のトランザクションが companion を失うことがありません([宣言のコメント](../../src/core/watch/reloadservice.hpp#L151))。

### [`applyRenderPipelineCompanionReload()`](../../src/core/watch/reloadservice.cpp#L448) — 流れと早期 return {#reading-note-059}

companion リクエストを shader キーと material ドキュメントに仕分け → `materials->prepareRuntimeGenerationReload()` で material_plan を作り、その `shader_overrides` 込みで `library->prepareReload()` → material commit と generation commit を 1 つの `coordinated_commit` に合成 → `pipelines->rebuildPrepared()` で一括コミット、という流れです。早期 return は「prepared 空 かつ pipeline_overrides 空 かつ coordinated_commit なし」= **実質変更なし**のときです。swap の三重構造は[第6章「reload の swap は 3 回」](06_rendering_vulkan_shader.md)が本体です。

### 単調増加する generation / epoch を「変わったか」の検出に使うの使用箇所 {#background-usage-05}

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `frame_revision` による `duplicate_revision` 判定(985〜997 行)が代表例です。アニメフレームは単調増加する `frame.frame_revision` を持ち、`animation_revisions[index]` 以下なら本体(スキンパレット等)を突き合わせずに `duplicate_revision` を返して捨て、世代不一致なら `stale_generation` を返します。`resetSlot()`(503 行)はスロット再利用のたびに `animation_generations` を +1 し、`0`(無効値)へ折り返したらもう一度 +1 して飛ばします。シーン全体の破棄は `modelinstanceslots.cpp` の `clearPrepared()`(99 行)が `scene_epoch_` を +1 します。この 3 粒度(revision / slot generation / scene epoch)のおかげで、同じフレームの二重適用や古い更新の取りこぼし受理を等値・大小比較だけで拒否でき、同じ入力列なら同じ状態になる決定性を保てます。

### Kahn 法のトポロジカルソートと、未知の依存先を無視する実装の使用箇所 {#background-usage-12}

**このリポジトリでは**: `src/core/watch/reloadtransaction.cpp` の `topologicalOrder()`(105 行〜)。`ReloadCoordinator::execute()` は 1 トランザクション分のアクタを全部 `stage()` した後にこれを呼び、返ったインデックス順で staged 済みリソースを並べ替えてから `registry_.publish(ordered)`(337 行)へ渡します。各アクタの `after` に並ぶ依存先を `actor_for` から引いて「依存先 → 依存元」の辺を張って入次数を数え、`if (found == actor_for.end()) continue;`(118 行)で同一トランザクション外の依存(外側で既に満たされている前提)を読み飛ばします。この順序が要るのは `publish()` が渡された順に `content_revision` を進めて逆依存辺を張り直すためで、依存先を先に確定させないと依存元が古い版を掴みます。末尾の `result.size() != actors.size()`(137 行)が循環依存のトランザクションを例外にし、ホットリロードの差し替えを「全部通すか、全部やめるか」の原子的な単位に保ちます。

## 6.11 GPU resource の遅延破棄

Vulkan object は C++ の所有権上不要になっても、前の frame の command buffer が GPU 上で参照中かもしれません。即時 destructor は use-after-free になります。

<a id="generation-lifetime"></a>

```mermaid
sequenceDiagram
    participant R as Renderer / CPU
    participant P as 公開 root
    participant Q as DeletionQueue
    participant T as FrameTarget / GPU
    R->>P: 記録する世代 G を取得して保持
    R->>Q: 置換した旧 pipeline 等を defer（batch B）
    R->>Q: leaseForNextSubmission（G を含む資源）
    Q-->>R: lease = 資源 + B の強参照
    R->>T: 記録・submit（lease を渡す）
    T->>T: 完了 fence まで lease を保持
    R->>Q: 成功した submission を confirm（B を seal）
    Q->>Q: current_batch を次の batch に交換
    opt ray joint の staged 世代がある
        R->>P: publishFrameGeneration（G を公開）
    end
    Note over P,T: root が別世代に変わっても、参照中の G は存続
    T->>T: GPU 完了を観測し lease を解放
    Note over R,T: B の最後の強参照が落ちると deferred resource を破棄<br/>G と arena もそれぞれ最後の所有者が手放すまで存続
```
根拠: [`framegraphruntime.hpp:143`](../../src/core/renderingpass/framegraphruntime.hpp#L165)、[`renderer.cpp:5703`](../../src/core/vkcore/renderer.cpp#L5744) / [`renderer.cpp:6059`](../../src/core/vkcore/renderer.cpp#L6100) / [`renderer.cpp:6762`](../../src/core/vkcore/renderer.cpp#L6813)、[`deletionqueue.cpp:63`](../../src/core/vkcore/deletionqueue.cpp#L63)。

**この節の語**: **submission** は GPU に投入した仕事の単位、**lease** はその仕事が使う資源を完了まで保持する所有参照です（target の取得 token と GPU 完了待ちの lease は寿命が異なります）。

**注**: `DeferredResource<T>` は任意の movable resource を「解放できる」という共通の口へ型消去し、batch に保持します。`PipelineFactory::replacePipeline()` が旧 pipeline/layout を積む入口です。frame 数による猶予はなく、複数 submission が同じ batch を保持すれば最後の lease まで残ります。通常の世代公開はフレーム記録前にも起き、図の submit 後の公開は ray joint 経路です。view 終端で submit する target はそこで batch を seal し、後の seal は重複しません。

出力 image が得られず未送信なら staged 世代を次フレームへ持ち越す経路があり、その他の未公開終了は guard が巻き戻します。送信後は出力公開が失敗しても世代を戻しません（[`StagedRuntimeRollbackGuard`](../../src/core/vkcore/renderer.cpp#L3077)、[`publishFrameGeneration()`](../../src/core/vkcore/renderer.cpp#L6106)）。`flushAll()` は GPU を待たないため呼び手の wait-idle が必要で、終了時に pending が残った destructor は wait-idle 後に safety flush します。teardown の受け入れ停止と再入禁止は以下の注を参照してください。

### submission lease と teardown の変更時の注意 {#gpu-retirement-gotchas}

変更時の原則は次です。

- GPU が参照し得る旧 object を local temporary の destructor に任せない。
- new object を公開した後、old object を deletion queue へ移す。
- **新しい submit 経路を足したら、lease を取って submission 完了まで手放さない。** lease を持たずに `confirmSubmission()` を呼ぶと、その batch は下の難所ブロックの理由で**その場で**破棄されます。
- queue に入れる object が dependent object より先に破棄されても Vulkan 規約上安全か確認する。
- shutdown は wait-idle → pending flush → module destruction の順を維持する。[`flushAll()`](../../src/core/vkcore/deletionqueue.cpp#L88) は fence も wait-idle も見ずに全 batch を落とすので、呼ぶ側が先に idle にする責任を持ちます(実際 [`loop.cpp` 内](../../src/core/appflow/loop.cpp#L311) は `waitIdle()` の直後、teardown は [`runtime_teardown_order`](../../src/core/appflow/teardown.hpp#L23) の `wait_idle` を先頭・`deletion_queue` を末尾に置いています)。

#### teardown 開始後の `defer()` はエラー

`DeletionQueueCore` は [`accepting` / `draining`](../../src/core/vkcore/deletionqueue.hpp#L78) フラグと [`requireAccepting()`](../../src/core/vkcore/deletionqueue.hpp#L81) を持ちます。**`defer()` だけでなく `leaseForNextSubmission()` と `confirmSubmission()` も先頭でこれを呼ぶ**ので、受け入れ停止後はこの 3 本すべてが `std::logic_error` になります([`deletionqueue_test.cpp` 内](../../test/deletionqueue_test.cpp#L156) が 3 本とも検査しています)。

teardown の最終段は phase で分岐します。

| phase | 動作 |
|---|---|
| `ModuleRuntimePhase::shutting_down` | [`drainForTeardown()`](../../src/core/vkcore/deletionqueue.hpp#L103)。以後 `defer()` 不可 |
| それ以外(`runtime_reset`) | `flushAll()`。queue は再利用可能なまま |

したがって「module の destructor から GPU object を defer する」コードは、terminal shutdown では失敗します。destructor は既に所有している資源で完結させるか、明示 teardown 段階へ移してください。受け入れ状態の確認は `acceptingResources()`(core)/ `acceptingResourcesForTesting()`(module ラッパ)です。

> 🧩 **難所 — 破棄の時刻を決めているのは refcount 1 本**([`confirmSubmission()`](../../src/core/vkcore/deletionqueue.cpp#L78) / [`flushAll()`](../../src/core/vkcore/deletionqueue.cpp#L88))
>
> **何をする所か**: defer された資源を、それを参照している実 GPU submission が完了するまで生かし、完了したものだけ本物のデストラクタへ落とします。
>
> **素朴に読むと**: 鍵は [`std::vector<std::weak_ptr<RetirementBatch>> batches`](../../src/core/vkcore/deletionqueue.hpp#L77) が **`weak_ptr` である**ことで、この vector は寿命を一切持ちません。batch を生かしている強参照は 2 本だけ — 次に積む先である [`current_batch`](../../src/core/vkcore/deletionqueue.hpp#L75) と、`leaseForNextSubmission()` が配った `GpuSubmissionLease` です。`confirmSubmission()` は `current_batch` を新しい batch で**上書き**するので、**lease を誰も持っていなければその瞬間に旧 batch の refcount が 0 になり、GPU がまだ読んでいる資源がその場で破棄されます**。「queue に積んだのだから遅延している」と読むと、この 1 本の依存が見えません。`batches` が `weak_ptr` なのは `flushAll()` のためで、生きている batch だけ `releaseAll()` し、失効したものは [`batches.erase(it)`](../../src/core/vkcore/deletionqueue.cpp#L103) で落とします — つまり `batches` は所有者ではなく「まだ破棄されていない batch の台帳」です。
>
> もう 1 つが `draining` で、`draining` を立てて 4 行の `DrainScope` で必ず倒す形は「念のための再入禁止」に見えますが、塞いでいるのは具体的な事故です。`release()` の実体は [`resource.reset()`](../../src/core/vkcore/deletionqueue.hpp#L45)、つまり**走るのは寝かせた型のデストラクタで、その中身は各 container のコード**です([`VertBufContainer::releaseModelGeometry()`](../../src/core/model/vertbufcontainer.cpp#L600) が defer する `DeferredRelease` のデストラクタは、lease を取り直したうえで owner のメソッドを呼び戻します)。そこから `defer()` が来ると [`resources.push_back`](../../src/core/vkcore/deletionqueue.hpp#L58) が走査中の vector を再確保し、`confirmSubmission()` が来ると `batches.push_back` が `flushAll()` のイテレータを無効にします。だから 3 本とも先頭で [`DeletionQueueCore::requireAccepting()`](../../src/core/vkcore/deletionqueue.cpp#L54) を呼び、drain 中は `std::logic_error` にします。これは「壊れる」を「その場で分かる」へ変える置き換えで、`DrainScope` が try-catch でなく RAII なのは、`releaseAll()` が throw しても、後から早期 return を足しても、1 か所でフラグを倒せるからです。`flushAll()` 自身の再入も同じく `std::logic_error` です。
>
> **流れ**: [世代と lease のタイムライン](#generation-lifetime)を参照してください。`leaseForNextSubmission()` が次の batch を準備し、`confirmSubmission()` はその準備済み batch へ交換します。
>
> **手がかり**: `flushAll()` は fence も wait-idle も見ません(待つのはデストラクタの [`wait_idle_hook()`](../../src/core/vkcore/deletionqueue.cpp#L34) と、`wait_idle` step を先に通す [`runtime_teardown_order`](../../src/core/appflow/teardown.hpp#L23) と、それを走査する [`teardownRuntimeNoThrow()`](../../src/core/appflow/teardown.cpp#L122))。`pendingCount()` は [`PendingCounter`](../../src/core/vkcore/deletionqueue.hpp#L18) を `DeferredResource` の生成/解放で増減させた live 数で、デストラクタはこれが 0 でなければ WARNING を出して wait-idle → flush する安全網に入ります([`DeletionQueueCore::~DeletionQueueCore()`](../../src/core/vkcore/deletionqueue.cpp#L22))。テストは [`"DeletionQueueCore releases resources only after the bound GPU submission completes"`](../../test/deletionqueue_test.cpp#L37)(lease を手放すまで破棄されない)/ [`"WP384 DeletionQueue keeps the old generation until its submission fence"`](../../test/deletionqueue_test.cpp#L60)(batch ごとに独立して retire する)/ [`"DeletionQueueCore teardown drain closes the queue and rejects late resources"`](../../test/deletionqueue_test.cpp#L145)(drain 後は 3 本とも throw)です。
>
> **不変条件**: submission に紐付けたい資源は `leaseForNextSubmission()` の lease を submission 完了まで保持する(保持しないなら `confirmSubmission()` の時点で破棄されてよい資源だけを defer する)。`draining` を立てる区間には必ず RAII で対になる復帰を置く。`releaseAll()` の実行中に `resources` / `batches` を触らない。`batches` を `shared_ptr` に変えない — 所有しないことが「lease だけが寿命を決める」という契約そのものです。

### submission の集約点と in-flight 定数 {#gpu-submission-lifetime}

**破棄が走るのは lease の最後の参照が消えた瞬間**です。frame target は [`GpuSubmissionLeaseSlots`](../../src/core/vkcore/frametarget.hpp#L223) に in-flight slot ごとに lease を持ち、その slot の completion fence を待ってから [`complete(slot)`](../../src/core/vkcore/offscreenframetarget.cpp#L178) で手放します。

`Renderer` 側の 3 点は [`deletion_queue.leaseForNextSubmission()`](../../src/core/vkcore/renderer.cpp#L6140) → [`target.submitLogicalFrame(submission_lease)`](../../src/core/vkcore/renderer.cpp#L6821) → [submission 確定の呼び出し](../../src/core/vkcore/renderer.cpp#L6102) です(それぞれ `LogicalFrameRecorder` の `collectPassPlan()` / `submitFrame()` / `sealRetirementBatch()` に属し、出力の公開は別 step の [`publishFrame()`](../../src/core/vkcore/renderer.cpp#L6825) です)。

つまり「何 frame 後に消えるか」を数えるコードは deletion queue からは消えました([`DeletionQueueCore`](../../src/core/vkcore/deletionqueue.hpp#L17) は `in_flight_frames_num` を一度も参照しません)。

ただし定数そのものは健在で、[`in_flight_frames_num`](../../src/core/vkcore/rendertarget.hpp#L16) は frame target の command buffer 配列や [`GpuSubmissionLeaseSlots`](../../src/core/vkcore/offscreenframetarget.hpp#L17)、[`FrameResources::configureViewCount()`](../../src/core/renderer/frameresources.cpp#L110) の descriptor slot 数、swapchain / OpenXR / ImGui の image count など `src/` 全体で 27 か所に残っています — lease slot 専用の定数ではありません。hot reload の [`replacePipeline()`](../../src/core/shader/pipelinefactory.cpp#L1431) が代表的な defer 元です。

### [`GpuSubmissionLease`](../../src/core/vkcore/frametarget.hpp#L53) — 型消去リースが 2 種類ある {#reading-note-019}

型は「1 回のキュー submit が参照する全 GPU オブジェクトの型消去された所有ルート」で、任意型の資源束を型を知らずに保持するため `shared_ptr<const void>` です。`submission_lease_` は**その 1 回の submit が参照する資源をフェンス完了まで生かす**もの、[`target_epoch_lease_`](../../src/core/vkcore/frametarget.hpp#L97) は**出力ターゲット側の epoch(swapchain 世代など)を記録中に生かす**別軸で、submit 時に [`consumed.target_epoch_lease.get() != active.epoch.get()`](../../src/core/vkcore/swapchainframetarget.cpp#L2506) として「このフレームは今の epoch のものか」の照合にも使われます。破棄の時刻そのものは [§9.12](06_rendering_vulkan_shader.md#gpu-retirement-gotchas) の難所「破棄の時刻を決めているのは refcount 1 本」が本体です。

### [`RetirementBatch`](../../src/core/vkcore/deletionqueue.hpp#L50) — なぜ weak_ptr で持つのか {#reading-note-020}

実所有者は `current_batch` と [`SubmissionLeaseBundle`](../../src/core/vkcore/deletionqueue.hpp#L67) で、キュー自身は weak 参照しか持ちません。submit 失敗でリースが捨てられた場合や、フレームターゲットがフェンス完了後にリースを解放した場合、batch は自動で消えます。`flushAll` の `lock()` はその**既に解放済みバッチ**で失敗し、エントリを erase します。

### in-flight フレームと GPU 資源の寿命の使用箇所 {#background-usage-07}

**使用場所**: [Renderer::renderLogicalFrame()](../../src/core/vkcore/renderer.cpp#L5484) / [GpuSubmissionLease](../../src/core/vkcore/frametarget.hpp#L53) / [DeletionQueueCore::flushAll()](../../src/core/vkcore/deletionqueue.cpp#L88)

### DeletionQueue のカウンタと submission の紐付け {#deletion-queue-completion-counter}

**溜めた資源の寿命を決めるのは「defer した frame 番号」ではなく「実 submission への紐付け」です。** `DeletionQueueCore` には frame を数える口も、「もう解放してよいものを掃き出す」口もありません。frame を名乗る唯一の残骸は [`currentFrame()`](../../src/core/vkcore/deletionqueue.hpp#L105) ですが、返すのは完了 submission 数です。現在は次の 3 手です。

## 6.12 Vulkan 初期化と resource wrapper

[`VulkanManageCore`](../../src/core/vkcore/core.hpp#L36) が instance、physical device、logical device、queues、command pools、VMA allocator(VMA = Vulkan Memory Allocator — GPU メモリを大きくまとめて確保し、buffer/image へ小分けに配る定番ライブラリ。`vk::DeviceMemory` を自前で管理せずに済みます)を所有します。constructor は [`core.cpp` 内](../../src/core/vkcore/core.cpp#L1027) です。

- Vulkan API version は [`1.3.283`](../../cmake/pelican_target_environment.cmake)。
- [`selectVulkanValidation()`](../../src/core/vkcore/vulkanvalidation.hpp) が validation layer と synchronization validation を一体で選択します。未指定は従来どおり `_DEBUG` だけ ON、`--vulkan-validation=on|off` は構成を問わず上書きします。生成に成功した instance の結果は [`getVulkanValidationStatus()`](../../src/core/vkcore/core.hpp) から `get_status.vulkan_validation` へ公開されます。要求時に `VK_LAYER_KHRONOS_validation` が無ければ `pelican.vulkan.validation_layer_unavailable@1` で停止します。
- window mode のみ surface と swapchain extension を要求します。
- 起動時に [`selectDebugUtilsExtension(launch_config.gpu_labels, supportedInstanceExtensions())`](../../src/core/vkcore/core.cpp#L668) を評価し、有効なときだけ `VK_EXT_debug_utils` を instance extension へ足します(flat: [`core.cpp` 内](../../src/core/vkcore/core.cpp#L61)、XR: [もう 1 箇所](../../src/core/vkcore/core.cpp#L110))。詳細は §6.16。
- physical device は window mode の swapchain extension と、`multiDrawIndirect`、`drawIndirectFirstInstance`、`shaderDrawParameters`、`dynamicRendering` の全 feature を満たす候補だけを選びます。OpenXR runtime が device を選ぶ経路でも同じ検査を行います。
- logical device では上記 4 feature を明示的に有効化します。
- graphics、presentation、compute queue family を [`pickQueues()`](../../src/core/vkcore/core.cpp#L228) で選び、それぞれの queue を取得します。
- graphics と compute command pool を別に作ります。
- device 生成後に [`debug_utils = DebugUtilsDispatch::resolve(instance, device, selection)`](../../src/core/vkcore/core.cpp#L696)。取得は [`getDebugUtils()`](../../src/core/vkcore/core.hpp#L104) です。
- XR active 時は [`bootstrap.hpp`](../../src/core/vkcore/bootstrap.hpp) 経由で、OpenXR runtime の graphics requirements に従って instance/device を生成する経路が加わりました(core.cpp には flat/XR 用の 2 つの app_info があります)。

[`VulkanManageCore::setCurrentFrameIndex(logical_frame)`](../../src/core/vkcore/core.hpp#L135) は毎フレーム `engine_time.advance()` の直後に呼ばれ(第2章 §2.4)、**debug-utils ラベルと GPU timing に共通の論理フレーム軸**を与えます。

buffer/image memory は VMA を使います。[`BufferWrapper`](../../src/core/vkcore/buf.hpp#L7) と [`ImageWrapper`](../../src/core/vkcore/image.hpp#L8) が Vulkan resource と VMA allocation を同じ struct に持ち、RAII で一緒に解放します。

compute task は独立 compute submission ではなく、[`FrameRenderContext::cmd_buf`](../../src/core/vkcore/frametarget.hpp#L23) へ dispatch を記録します。この command buffer は [`allocCmdBufs()`](../../src/core/vkcore/core.cpp#L1423) が graphics pool/queue 用に作るものです。したがって frame graph compute は async compute pipeline ではなく、graphics queue 上の直列 compute と理解するのが正確です。

### multiDrawIndirect、drawIndirectFirstInstance、shaderDrawParameters(gl_BaseInstance)の使用箇所 {#background-usage-02}

**このリポジトリでは**: `core.cpp` の `queryDeviceFeatureSupport()`(219 行)が 3 feature を `getFeatures2` の chain で照会し、1 つでも欠ける device は `firstMissingRequiredVulkanFeature()` で選外にします(279〜280 行)。選ばれた device では `multiDrawIndirect` / `drawIndirectFirstInstance` / `vk11.shaderDrawParameters` をすべて true にして `createLogicalDevice()` の `vk::StructureChain` に載せます(350〜370 行)。

`PolygonInstanceContainer::triggerUpdate()` は `limits.maxDrawIndirectCount` を build request に渡し、builder が同じ material の連続範囲を上限以下へ分割します(657〜660 行)。`first_instance = instance.index` がシェーダで `gl_BaseInstance` になり、`default.vert` の `pelicanObjects.objects[gl_BaseInstance].model`(29 行)で per-instance のモデル行列を引きます。この 3 点が揃わないと「1 回の間接描画で drawCount>1」「各 draw が別々の firstInstance を持つ」「シェーダがそれを読む」が成立せずバッチが崩れるため、照会だけでは有効にならない feature を明示 enable し、非対応 GPU は起動時に弾いています。

## 6.13 描画を変更するときの実践的な追い方

### 新しい render pass 種別を増やす

1. [`PassInfo`](../../src/core/renderingpass/renderingpass.hpp#L210) に info struct を追加。struct の置き場は、generation の ID も条件付きの compile 定義も持たない値だけなら project の [`passinfo.hpp`](../../src/project/passinfo.hpp#L15)、ID を持つ binding を含むなら core の `renderingpass.hpp`(✅RC-4 の分け方。§6.2)。
2. IR の info([`rendergraphir.hpp`](../../src/project/rendergraphir.hpp#L246) の `NodeInfo`)に欄を足し、[`passExecutionDeclaration()`](../../src/project/rendergraphparse.cpp#L747) で形を読み(拒否は `render_graph.node_field_invalid@1`)、[`passDefinitionOf()`](../../src/core/renderingpass/passdefinitionfromir.cpp#L80) で info struct に写す(実行に要る値の必須はここ。`render_graph.pass_execution_invalid@1`)。IR の指紋 [`rendergraphfingerprint.cpp`](../../src/project/rendergraphfingerprint.cpp#L167) の欄の列にも足す(✅RC-3c2b2。前は core の `passinfojsonparser.cpp` 周辺)。
3. [`renderingpassruntimecompiler.cpp`](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L2105) で shader/pipeline/renderer resource を登録。
4. [`render_pass_dispatch.cpp`](../../src/core/vkcore/render_pass_dispatch.cpp#L155) で draw call を dispatch。
5. pure parser test、runtime registration test、headless render test を追加。

### 新しい compute resource を増やす

1. planner の resource declaration/known-resource 検証を拡張。
2. descriptor reflection type と resource object の対応を [`createDescriptorSet()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1028) へ追加。
3. resource ごとの正しい access/stage barrier を実装。
4. resize、hot reload、teardown 時の寿命を定義。

### 画面が真っ黒なときの読む順

1. [`currentFramePlanJson()`](../../src/core/vkcore/renderer.cpp#L4515) で node 順と reads/writes を確認。
2. [`RenderPassExecutor::execute()`](../../src/core/vkcore/render_pass_executor.cpp#L315) で target layout と attachment を確認。
3. [`renderDynamicPassDrawCalls()`](../../src/core/vkcore/render_pass_dispatch.cpp#L195) で意図した variant に入ったか確認。
4. shader bundle の `log` と reflection を確認。
5. `--vulkan-validation` で synchronization validation を有効にして barrier/layout error を確認(未指定でも `_DEBUG` は ON)。
6. `--gpu-labels` を付けて RenderDoc の event ツリーを見る、または `get_status.gpu_timing` の node 行を見る(§6.16、§6.17)。
7. headless capture と golden test で出力を固定して比較。

## 6.14 Projection jitter と TAA(WP112〜115)

temporal 系の中心型は [`projectionjitter.hpp`](../../src/core/renderer/projectionjitter.hpp) にあります。

- [`ProjectionJitterSettings`](../../src/core/renderer/projectionjitter.hpp#L12): projection 行列へ加えるサブピクセルジッタの設定。ユーザー定義のジッタ系列も宣言できます(WP115。fixture: [`fixtures/projection_jitter_consumers.json`](../../test/fixtures/projection_jitter_consumers.json))。
- [`RenderFrameSnapshot`](../../src/core/renderer/projectionjitter.hpp#L31) / [`TemporalFrameHistory`](../../src/core/renderer/projectionjitter.hpp#L54): 現フレームの view/projection/jitter と前フレーム値の組。view ごとに 1 history を持ち、`buildRenderFrameSnapshot()` → 描画 → `commitRenderFrameSnapshot()` の順で回します。
- velocity pass(`VelocityPassInfo` + [`velocitypasscontainer.hpp`](../../src/core/renderer/velocitypasscontainer.hpp))が screen-space velocity を書き、previous object buffer(`PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING`)と合わせて TAA resolve の入力になります。

> 🧩 **難所 — ジッタは w 倍で足す**([`applyProjectionJitter()`](../../src/core/renderer/projectionjitter.cpp#L76)、打ち消しは [`velocity.frag`](../../src/core/resources/velocity.frag))
>
> **何をする所か**: サブピクセルのジッタを projection 行列へ埋め込み、velocity 側で同じ量を引き戻します。
>
> **素朴に読むと**: `applyProjectionJitter()` は 4 行しかありませんが、なぜ w 行 `P[c][3]` を `P[c][0]` / `P[c][1]` に足すのかが分からないと読めません。これは `clip.x += dx * clip.w` と等価で、**透視除算のあとで定数 NDC シフトになる**唯一の書き方です(NDC は `clip.x / clip.w` なので、`(clip.x + dx*clip.w) / clip.w = ndc.x + dx`。奥行きに比例する `clip.w` が約分で消えるため、ずれ量が深度に依存しません)。素朴に「平行移動列 `P[3][0]` に足す」とやると view 空間での平行移動になり、**深度によってずれ量が変わって**遠景ほどジッタが効かなくなります(= TAA が遠景で解像しない)。glm が column-major(`P[column][row]`)なので全列に対するループになる点も読み間違えやすい所です。打ち消し側も同じくらい非自明で、`velocity.frag` は現在・前フレームの clip をそれぞれ NDC に落としてから **各フレームのジッタを引き**、差分に `0.5`(NDC 幅 2 → UV 幅 1 の換算)を掛けます。引かないと velocity にフレームごとのジッタ振動がそのまま乗り、TAA の再投影が毎フレーム半ピクセル暴れます。
>
> **骨子**:
> ```text
> CPU: offset_px  = halton23(sample_index) - 0.5        # [-0.5, 0.5)
>      jitter_ndc = (2*ox/width, 2*oy/height)
>      for c in 0..3: P[c][0] += dx*P[c][3];  P[c][1] += dy*P[c][3]
> GPU: cur = clip.xy/clip.w - jitter_ndc
>      prv = pclip.xy/pclip.w - previous_jitter_ndc
>      velocity_uv = (cur - prv) * 0.5
> ```
>
> **手がかり**: y の符号反転が**無い**のは意図的で、Pelican は正 height の Vulkan viewport を使うため NDC +y と framebuffer +y が同方向です(該当コメントが規範)。✅CC-1(`c8367b87`)で world から画面への y の反転を camera の射影([`clipspace.hpp`](../../src/core/renderer/clipspace.hpp#L35))に入れましたが、反転は jitter を足す前の射影の中なので、jitter・velocity・再投影の符号はそのままです。骨子の `halton23` は **Halton 列**(ハルトン列 — 添字を基数 2 や 3 で表記し、その桁を小数点以下へ折り返して作る準乱数列。ふつうの乱数と違って点どうしが固まらず一様に散るので、少ないサンプル数でもサブピクセル位置が偏りません)の基底 2 / 3 版で、`pattern` の既定値です(もう一方は `table`)。系列側は `sample_index = ((frame_index - 1) % phases) + 1` と **1 始まり**に正規化し、`frame_index == 0` は明示 error にします(unsigned のアンダーフローに任せない、という判断)。FrameUBO の `projection` に入るのは**ジッタ済み**行列で、`jitter_ndc` / `previous_jitter_ndc` のオフセットは [`frameresources.hpp`](../../src/core/renderer/frameresources.hpp#L72) の `static_assert` で固定されています。テストは [`projectionjitter_test.cpp`](../../test/projectionjitter_test.cpp)。
>
> **不変条件**: ジッタは projection にのみ入れ、view / world には入れないこと。CPU の `offset_px`・`jitter_ndc`・shader サンプリングの y 符号は同一。系列は `frame_index` の純関数(状態を持たない)で、ここを崩すと replay 不能になります。

temporal history のリセットは `renderLogicalFrame()` 内で次のトリガから起きます:
family/view membershipまたは実行順の変化、`set_time`等による`timeSetRevision`の不連続と
cameraの`discontinuityRevision`、extent変化、graph variant切替。matrix履歴自体は
`family_id + view_id`で引きますが、history imageは現状実行添字所有なので順序変更でも
resource historyをresetします。明示リセット用の公開APIは
[`Renderer::resetTemporalHistory()`](../../src/core/vkcore/renderer.cpp)です。

> 🧩 **難所 — epoch ペアが reset 信号**([`buildRenderFrameSnapshot()`](../../src/core/renderer/projectionjitter.cpp#L85) / [`RenderFrameSnapshot::historyValid()`](../../src/core/renderer/projectionjitter.hpp#L49))
>
> **何をする所か**: 上のトリガを GPU へどう伝えるかの答えです。history が使えるかどうかを bool ではなく **2 つの uint32 の一致**で表し、shader は `==` で判定します。
>
> **素朴に読むと**: `historyValid()` の実装が `temporal_reset_epoch == previous_temporal_reset_epoch` だけなので、「epoch は何かのカウンタで、値そのものに意味がある」と誤読しがちです。実際は**値に意味は無く、一致/不一致だけが意味を持ちます**。reset フレームでは `previous = 旧 epoch`、`current = 旧 epoch + 1` を書き込んで**わざと食い違わせ**、翌フレームは `commitRenderFrameSnapshot()` が新 epoch を history に載せるので両方が一致し、自然に valid へ戻ります。つまり**不一致はちょうど 1 フレームだけ続く**のが設計です。素朴に `bool history_valid` を FrameUBO へ置くと、reset の原因が複数箇所から立つためフラグを誰が下ろすか曖昧になり、「2 フレーム無効」や「下ろし忘れで永久無効」が起きます。epoch ペアなら下ろす処理が存在しません。
>
> **骨子**:
> ```text
> reset = reset_requested || !history.valid
> if reset: previous_* = current_*         # 差分ゼロ = velocity 0
>           previous_epoch = history.epoch;  epoch = history.epoch + 1
> else:     previous_* = history.*;         epoch = previous_epoch = history.epoch
> commit:   history.* = snapshot.*
> ```
>
> - **commit**: 次フレームは自動的に一致(valid)へ。
>
> **手がかり**: reset フレームでも `previous_*` 行列に**現在値を入れる**ので velocity は 0 になり、履歴を読まなければ絵は正しくなります(フラグ方式で `previous_*` を前フレームのゴミのまま渡すと、reset フレームだけ巨大な velocity が出て TAA が飛びます)。shader 側は [`taa_resolve.frag`](../../src/core/resources/taa_resolve.frag) の `main()` 冒頭でこの一致から `valid` を始め、再投影 UV の範囲外判定・深度不連続判定と `&&` で畳んでいきます。真理値表テストが [`projectionjitter_test.cpp`](../../test/projectionjitter_test.cpp) の "temporal reset epoch truth table is invalid for one frame for every reset cause" で、名前が仕様そのものです。
>
> **不変条件**: 不一致は 1 フレームだけ(2 フレーム以上続く実装にすると TAA が収束しません)。reset フレームの `previous_*` は現在値と等しいこと。epoch は単調増加し、ラップは error です(`nextEpoch()` が `UINT32_MAX` で throw するのは、ラップして偶然一致し「reset したのに valid」になるのを静かに起こさないため)。

テスト: [`temporal_test.cpp`](../../test/temporal_test.cpp)、[`taa_resolve_test.cpp`](../../test/taa_resolve_test.cpp)。

## 6.15 XR composition と mirror(WP125〜133)

OpenXR 統合(`src/core/openxr/`、独立 static lib `pelican_openxr`)は描画層へ次の形で接続します。

| 部品 | 役割 |
|---|---|
| [`OpenXr::DiscoveryRuntime`](../../src/core/openxr/openxrdiscovery.hpp#L43) | instance/system の discovery。Vulkan bootstrap(graphics requirements)と連携 |
| [`OpenXr::SessionRuntime`](../../src/core/openxr/openxrsession.hpp#L135) | session 状態機械。`waitFrame`/`beginFrame` と [`XrDisplayTiming`](../../src/core/openxr/openxrsession.hpp#L25)、[`XrLocatedViews`](../../src/core/openxr/openxrsession.hpp#L40) |
| [`OpenXr::XrCompositionTarget`](../../src/core/openxr/openxrcompositiontarget.hpp#L101) | XR swapchain を `ILogicalFrameTarget` として公開(`IXrCompositionTarget` 同 #L59) |
| [`OpenXr::XrMirrorSink`](../../src/core/openxr/openxrmirrorsink.hpp#L39) | window への mirror 表示。`beginFrame(..., FrameBeginMode::nonblocking)` の zero-wait で desktop swapchain を取り、[`classifyMirrorBeginResult()`](../../src/core/openxr/openxrmirrorsink.hpp#L26) が `FrameBeginResult` を present / drop / disable の 3 択へ翻訳する(§6.6)。`unavailable` は drop、`device_rebuild_required` / `fatal` は以後 mirror を無効化 |
| [`buildMainRenderViewFamily()`](../../src/core/openxr/openxrviewspace.hpp) | 両eyeのpose/fovをactive cameraへanchorし、stable eye ID付き`$main` familyへ変換(WP131/WP223) |
| [`CompiledGraphVariantPolicy`](../../src/project/graphvariantpolicy.hpp) | `#xr` graph variant の typed feature decision、exact 2-view sequential、history/jitter、mirror、suffix 契約。OpenXR session lifecycle は所有しない |

フレームの流れは第2章の「windowed + XR session running」経路のとおりで、
`renderLogicalFrame(target, $main stereo family)`に`XrCompositionTarget`を渡し、view 0の
描画後にmirror用の中間コピーを記録して、logical frameの外でmirror sinkが`tryPresent`します。

テスト: `xractivation_test` / `xrdiscovery_test` / `xrsession_test` / `xraction_test` / `xrviewspace_test` / `xrcompositiontarget_test` / `xrfeaturepolicy_test`(GPU なしで検証するための [`synthetic_stereo_target.hpp`](../../test/synthetic_stereo_target.hpp))、process integration は [`run_vrm_xr_demo_rpc.cmake`](../../test/run_vrm_xr_demo_rpc.cmake)。

### XR mirror の drop と disable の境界 {#xr-mirror-dispositions}

frame の開始は成否 bool ではなく [`FrameBeginResult`](../../src/core/vkcore/frametarget.hpp#L203) を返します([`IFrameTarget::beginFrame()`](../../src/core/vkcore/frametarget.hpp#L291))。判断の軸は [`FrameBeginDisposition`](../../src/core/vkcore/frametarget.hpp#L143) の 4 値で、frame が取れなかった内訳は [`FrameUnavailableReason`](../../src/core/vkcore/frametarget.hpp#L150) の `reason` に別途入ります。mirror はこれを [`classifyMirrorBeginResult()`](../../src/core/openxr/openxrmirrorsink.cpp#L58) で 3 つの行動へ落とします。

| disposition | 行動 | mirror の挙動 |
|---|---|---|
| `ready` | `present` | 描いて `submit()`。`presented` を数える |
| `unavailable` | `drop` | 何もせず return。`dropped` と `last_drop_reason` だけ進む。**エラーではありません** |
| `device_rebuild_required` / `fatal` | `disable` | WARNING を出して以後は永久に諦める。`failures` を数える |

**mirror 経路にエラー処理を足すときに、`unavailable` を error へ昇格させないでください。** 分類が見るのは `disposition` だけで `reason` は見ないので、`reason` が `surface_lost` であっても `unavailable` なら drop です([テストが 4 分類を明示的に固定しています](../../test/xrfeaturepolicy_test.cpp#L196))。surface の回復は frame target 側の仕事で、mirror から同期的に回復を呼ばないことが設計上の要求です([`docs/design_wsi_epoch_recovery.md`](../design_wsi_epoch_recovery.md) の「9.4 XR mirror」)。

mirror が best-effort でいられるのは、入力が engine 所有の中間 target `__xr_mirror_left` だけで、OpenXR swapchain image を受け取らず、`FrameBeginMode::nonblocking` で開始するため desktop swapchain を待ちもしないからです([`XrMirrorSink`](../../src/core/openxr/openxrmirrorsink.hpp#L39) 直前のコメント)。

[`XrMirrorSink::tryPresent()`](../../src/core/openxr/openxrmirrorsink.cpp#L274) は全体が `noexcept` で、例外が出れば frame を [`abandon()`](../../src/core/vkcore/frametarget.hpp#L313) して自分を disable するだけです — HMD 側の logical frame は止めません。集計は [`XrMirrorSinkStats`](../../src/core/openxr/openxrmirrorsink.hpp#L29) に載り、`OpenXR mirror diagnostic: milestone=...` の INFO ログに出ます(WP133 が入れた「mirror は drop してよい」規約を、現在はこの 3 分類が担っています)。

drop は `beginFrame()` が `ready` を返した後にも起こります。letterbox 矩形が空(destination が 0 サイズ = 最小化)なら、取得済みの frame をそのまま `submit()` してから drop として数えます。**`beginFrame()` で得た frame は必ず `submit()` か `abandon()` へ渡してください** — 渡さなければ `FrameTargetFrame` のデストラクタが abandon 扱いで回収するので壊れはしませんが、どの経路で落ちたのかが追えなくなります。

### [2 種類の image view](../../src/core/openxr/openxrcompositiongraphics.cpp#L115) — 2D Array と層ごとの 2D {#reading-note-064}

**multiview 一括描画([`array_layers == xr_stereo_view_count`](../../src/core/openxr/openxrcompositiongraphics.cpp#L247))では layered view を attachment に使い、片目ずつ順次描画するときは `layer_views[base_array_layer]` を使う**ためです。両経路を同じ swapchain image 上で切り替えられるようにしてあります。

### [depth だけ `eTransferDstOptimal` へ遷移させる barrier](../../src/core/openxr/openxrcompositiongraphics.cpp#L209) — color と違う遷移先 {#reading-note-066}

depth は色と違い**レンダーパスの出力ではなく、エンジン側の depth を外部からコピーして XR に渡す**運用だからです。`eUndefined` → `eTransferDstOptimal` へ遷移させ、dstAccess を `eTransferWrite` にしています。

### [depth extension の 4 値を書く行](../../src/core/openxr/openxrcompositiontarget.cpp#L915) — minDepth/maxDepth と nearZ/farZ {#reading-note-067}

`minDepth` / `maxDepth` は depth image に書かれている**深度値そのものの値域**(ここでは 0.0〜1.0 の Vulkan 標準)、`nearZ` / `farZ` はその正規化深度が対応する**実際のビュー空間距離**(`new_near_z > 0 && new_far_z > new_near_z` で検証済み)です。ランタイムはこの 4 値で深度を線形化して reprojection に使います。

## 6.16 Vulkan debug utils ラベル ✅実装済み(WP139 / D-P0a)

`VK_EXT_debug_utils` を使い、Vulkan object へ名前を、command buffer へスコープラベルを付けます。**既定では無効**で、起動オプション `--gpu-labels`([`EngineLaunchConfig::gpu_labels`](../../src/core/launchconfig.hpp#L100))で有効化します。

選択は [`selectDebugUtilsExtension(requested, supported)`](../../src/core/vkcore/debugutils.hpp#L21) が [`DebugUtilsExtensionSelection{requested, available, enabled, reason}`](../../src/core/vkcore/debugutils.hpp#L14) を返す形です。既定の reason は `"disabled_by_launch_option"` なので、`get_status` を見れば「拡張が無いのか、オプションを付け忘れたのか」が区別できます。

[`DebugUtilsDispatch`](../../src/core/vkcore/debugutils.hpp#L41) が実際の関数ポインタを持ちます。すべて `noexcept` で、無効時は何もしません。

| 種別 | メソッド |
|---|---|
| コマンドラベル | `beginCommandLabel` / `endCommandLabel` |
| queue ラベル | `beginQueueLabel` / `endQueueLabel` |
| object 名 | `nameImage` / `nameImageView` / `nameSwapchain` / `nameBuffer` / `nameDescriptorSet` |

RAII ラッパは [`ScopedCommandDebugLabel`](../../src/core/vkcore/debugutils.hpp#L73) です。

### ラベル文字列は仕様

[`makeFrameGraphDebugLabel()`](../../src/core/vkcore/debugutils.hpp#L95)(実装は [`makeFrameGraphDebugLabel()`](../../src/core/vkcore/debugutils.cpp#L162))が次の形を作ります。

```text
frame/<logical_frame>/graph/<variant>/view/<view_index>/node/<ordinal>:<kind>:<name>
```

各 node はさらに `barriers` と `body` の 2 つの子ラベルへ分割されます。これを行うのは [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1575) が node ごとに直接作る 3 つの [`ScopedCommandDebugLabel`](../../src/core/vkcore/debugutils.hpp#L73)(node / `barriers` / `body`。[barriers subrange の記録](../../src/core/vkcore/renderer.cpp#L2019))で、render node と compute node は同じ loop 内のこの 1 か所を共有します。

テンプレート [`recordDebugLabeledNode()`](../../src/core/vkcore/debugutils.hpp#L98) は同じ規約をテストから検証するためのもので、`renderer.cpp` からは呼ばれません([test/debugutils_test.cpp](../../test/debugutils_test.cpp#L94))。XR mirror の中間コピーだけは別地点でラベル付けされます([`recordXrMirrorIntermediate()`](../../src/core/vkcore/renderer.cpp#L2827))。

> **設計決定:** このラベル文字列は「見やすさのためのおまけ」ではなく **診断識別子の正規形** です。GPU timing のサンプル名も同じ関数から派生するため(§6.17)、RenderDoc の event ツリーと `get_status.gpu_timing` の行を文字列一致で突き合わせられます。ラベル形式を変えるときは両方の契約を同時に変えることになります。

状態は RPC の `get_status.debug_utils` に `{available, enabled, reason, capabilities{object_name, command_label, queue_label}}` として載ります([第7章](07_tools_rpc_tests.md))。テストは [`test/debugutils_test.cpp`](../../test/debugutils_test.cpp) です。

## 6.17 Stereo-safe GPU timing と VRAM 診断 ✅実装済み(WP143 / D-P2a、WP145 / D-P2b)

### GPU timing

[`rendertiming.hpp`](../../src/core/vkcore/rendertiming.hpp) の識別子が **view ごとに分離** されたのが WP143 の要点です。stereo(XR)では同じ node が 2 回記録されますが、`view_index` が識別子に入っているため左右の時間が混ざりません。

| 型 | 内容 |
|---|---|
| [`GpuTimingSubrange`](../../src/core/vkcore/rendertiming.hpp#L27) | `barriers` / `body` |
| [`GpuTimingRangeIdentity`](../../src/core/vkcore/rendertiming.hpp#L34) | `logical_frame` + `graph_variant` + `view_index` |
| [`GpuTimingSampleIdentity`](../../src/core/vkcore/rendertiming.hpp#L49) | 上記 + `node_ordinal` / `node_kind` / `node_name` / `subrange` |
| [`GpuTimingViewRow`](../../src/core/vkcore/rendertiming.hpp#L75) / [`GpuTimingNodeRow`](../../src/core/vkcore/rendertiming.hpp#L85) | 集計出力の行 |

履歴容量は [`gpu_timing_history_capacity = 120`](../../src/core/vkcore/rendertiming.hpp#L18) です。

✅PO-1 から、履歴は 1 つの runtime の世代の frame だけを持ちます。renderer は論理フレームの range を始める前に [`RenderTiming::beginRuntimeGeneration()`](../../src/core/vkcore/rendertiming.cpp#L512) を呼び([呼ぶ所](../../src/core/vkcore/renderer.cpp#L6283))、世代が [`window_generation`](../../src/core/vkcore/rendertiming.hpp#L200) と違えば `gpu_history`・系列の合計・最後に記録した range を空にして snapshot を出し直します。

range は記録した時の世代を持ち、回収した時に窓の世代と違えば履歴に足さずに捨てます([捨てる所](../../src/core/vkcore/rendertiming.cpp#L655))。`statusJson()` は `generation`(窓の世代。最初の frame の前は `null`)と `samples`(`gpu_history` の数)を出します([出す所](../../src/core/vkcore/rendertiming.cpp#L739))。`get_gpu_timing` の node の平均(`nodeAverageJson()`)も同じ `gpu_history` から作ります。

✅ST-1 から、`get_status` の投影 [`projectGpuTimingStatus()`](../../src/core/vkcore/rendertiming.cpp#L293) は `nodes` に**最後の 1 論理フレーム**(`history.back()`)の sample だけを並べ([該当箇所](../../src/core/vkcore/rendertiming.cpp#L304))、`node_averages` に node ごとの窓の平均を足します([足す所](../../src/core/vkcore/rendertiming.cpp#L374))。

平均は [`averageGpuTimingHistoryNodes()`](../../src/core/vkcore/rendertiming.cpp#L138)(履歴の最後の [`gpu_timing_node_average_window = 30`](../../src/core/vkcore/rendertiming.hpp#L19) frame)と行の組み立て [`gpuTimingNodeAverageRowsJson()`](../../src/core/vkcore/rendertiming.cpp#L152) で作り、`get_gpu_timing` の [`RenderTiming::nodeAverageJson()`](../../src/core/vkcore/rendertiming.cpp#L713) も同じ 2 つを通るので、2 つの RPC の行は同じ値です。履歴の各 frame から node の sample を作るのは [`makeGpuTimingNodeFrame()`](../../src/core/vkcore/rendertiming.cpp#L105) です。

[`makeGpuTimingSampleLabel()`](../../src/core/vkcore/rendertiming.hpp#L152) は `makeFrameGraphDebugLabel()` の結果へ `/barriers` または `/body` を足すだけです([`makeGpuTimingSampleLabel()`](../../src/core/vkcore/rendertiming.cpp#L182))。つまり **RenderDoc のラベルと GPU timing のサンプル名は同一命名規約** です。

帰属の規範は [`gpuTimingAttributionContractJson()`](../../src/core/vkcore/rendertiming.hpp#L153)(無効時は [`disabledGpuTimingStatusJson()`](../../src/core/vkcore/rendertiming.hpp#L154))が JSON として出します。`barriers` は「compile 済み incoming barrier」、`body` は「node 自身の image/resource transition と描画・dispatch」です。

fixture は [`test/fixtures/gpu_timing_attribution.json`](../../test/fixtures/gpu_timing_attribution.json)、テストは [`test/rendertiming_test.cpp`](../../test/rendertiming_test.cpp) と [`test/golden_timing_test.cpp`](../../test/golden_timing_test.cpp) です。

### メモリ診断

[`collectMemoryStatus(vulkan, geometry, materials)`](../../src/core/vkcore/memorydiagnostics.hpp#L46) と [`memoryStatusJson(status)`](../../src/core/vkcore/memorydiagnostics.hpp#L49) が2系統を返します。

- [`DriverMemoryStatus`](../../src/core/vkcore/memorydiagnostics.hpp#L24): driver heap ごとの `size` / `usage` / `budget`。取得できない環境では `available=false` と `reason` を返します。
- [`EngineMemoryCategoryStatus`](../../src/core/vkcore/memorydiagnostics.hpp#L30): engine 側カテゴリごとの `allocated` / `logical_used` / `free` / `high_water` / `object_count`。値が無い項目は `std::optional` が空のままで、代わりに `reason` が入ります。

> **設計決定:** 「取れなかった値を 0 で埋めない」のがこの API の要点です。0 と「測定不能」を JSON 上で区別できるので、`get_status.memory` を見る側が誤った回帰判定をしません。

テストは [`test/memorydiagnostics_test.cpp`](../../test/memorydiagnostics_test.cpp) です。

`get_frame_plan` の `memory_summary` と物理 image の `bytes`(✅RC-1 / RC-1b。§6.4「物理 image の bytes と `memory_summary`」)はこれとは別物です。driver にも allocator にも聞かず、render target の image を texel × extent(mip・layer・面込み)で数えた値で、alignment を含みません(`total_bytes` は RC-1b から、公開中の世代の GPU arena に登録された render target 全部の和)。

## 6.18 RenderDoc in-app capture 🚧受動統合のみ(WP140 / D-P1a)

[`DECLARE_MODULE(RenderDocCapture)`](../../src/core/renderdoc/renderdoccapture.hpp#L66) が F11 キーと RPC `capture_gpu` の両方から使われるキャプチャ状態機械です。

> **設計決定:** エンジンは **RenderDoc をロードしません**。すでにプロセスへ注入されている API テーブルを [`RenderDocApiTable`](../../src/core/renderdoc/renderdoccapture.hpp#L32) として観測するだけです。したがって「RenderDoc から起動したときにだけ使える」機能で、エンジン単体で capture を始めることはできません。

| 要素 | 内容 |
|---|---|
| 状態 | [`RenderDocCaptureState`](../../src/core/renderdoc/renderdoccapture.hpp#L15): `unavailable` / `idle` / `armed` / `capturing` / `completing` / `failed` |
| 発生源 | [`RenderDocCaptureSource`](../../src/core/renderdoc/renderdoccapture.hpp#L24): `f11` / `rpc` |
| API | `request(source, xr_active)` / `captureArmedFrame(frame_index, device, window, render_once)` / `status()` / `beginShutdown()` |
| エラー | [`RenderDocCaptureError`](../../src/core/renderdoc/renderdoccapture.hpp#L58)(`reason()` 付き) |

ビルドフラグ `PELICAN_WITH_RENDERDOC` が OFF のときは [`renderdoccapture_stub.cpp`](../../src/core/renderdoc/renderdoccapture_stub.cpp) が使われ、状態は常に `unavailable` です。

`renderDocDevicePointerFromVulkanInstance()`([`renderDocDevicePointerFromVulkanInstance()`](../../src/core/renderdoc/renderdoccapture.hpp#L98))のコメントが規範です。

> RenderDoc's Vulkan API expects the dispatch-table pointer stored in a
> VkInstance, not the VkInstance handle itself.

### フレームループ側の接続

- モジュール解決は `Renderer` が Vulkan instance を作る**前**です([`loop.cpp` 内](../../src/core/appflow/loop.cpp#L161))。
- F11 の arm は [`requestF11CaptureIfNeeded()`](../../src/core/appflow/loop.cpp#L321)、呼び出しは interactive state 更新の末尾([F11 arm の呼び出し](../../src/core/appflow/loop.cpp#L720))です。
- 実キャプチャは **flat 描画のみ** [`renderFlatFrameWithOptionalCapture()`](../../src/core/appflow/loop.cpp#L331)(呼び出しは [flat capture 呼び出し](../../src/core/appflow/loop.cpp#L761))で、`armed` のときだけ `StartFrameCapture` / `EndFrameCapture` を明示発行します。失敗しても **論理フレームは必ず1回だけ描画されます**(二重描画しないガード付き)。
- 終了時は `finishLoopResources()` の先頭で `beginShutdown()`([RenderDoc shutdown 呼び出し](../../src/core/appflow/loop.cpp#L309))。

テストは [`test/renderdoccapture_test.cpp`](../../test/renderdoccapture_test.cpp)(`PELICAN_WITH_RENDERDOC` 時のみ)です。

## 6.19 preview 実行系(`PreviewExecutor`) 🚧模式ラスタ(WP172)

§6.1 で触れた第3 variant の実行側が [`PreviewExecutor::execute(program, projection, request, engine_time)`](../../src/core/vkcore/previewexecutor.hpp#L63) です。編集 RPC の `render_preview` から呼ばれます([第7章](07_tools_rpc_tests.md))。

> **設計決定:** 現在の `PreviewExecutor` は **Vulkan を一切使わない CPU 模式ラスタライザ** です([`previewexecutor.cpp`](../../src/core/vkcore/previewexecutor.cpp))。背景のグラデーション+グリッドを描き、投影後の各オブジェクト位置へ小さな円と十字マーカーを置くだけで、色は `light` / `camera` / `collider` component の有無で選びます。material も mesh も shader も評価しません。WP172 が確立したのは「preview がライブ描画状態を汚さない」という**隔離契約**であって、見た目の忠実度ではありません。`render_preview` の出力を最終描画の代用と見なさないでください。

| 要素 | 内容 |
|---|---|
| リクエスト | [`PreviewCaptureRequest`](../../src/core/vkcore/previewexecutor.hpp#L25): `width` / `height` / `pixel_encoding` / `view` / `projection` / `graph_generation` / `max_bytes` / `preview_request_id` |
| 出力形式 | [`PreviewPixelEncoding`](../../src/core/vkcore/previewexecutor.hpp#L14): `rgba8_srgb` または `png`(PNG エンコーダも自前実装、zlib stored ブロック — deflate の「無圧縮」ブロック形式で、65535 byte 以下ずつ長さヘッダを付けて生データを並べるだけ。圧縮ライブラリなしで PNG が要求する zlib 形式を満たせます) |
| 結果 | [`PreviewCaptureResult`](../../src/core/vkcore/previewexecutor.hpp#L36): bytes と `timing` 診断 JSON |
| 上限 | [`preview_capture_max_dimension = 2048`](../../src/core/vkcore/previewexecutor.hpp#L54) / [`preview_capture_hard_max_bytes = 16 MiB`](../../src/core/vkcore/previewexecutor.hpp#L55)。超過は [`PreviewCaptureTooLarge`](../../src/core/vkcore/previewexecutor.hpp#L44) |
| 世代検証 | `request.graph_generation != program.generation` なら `std::invalid_argument("preview graph generation mismatch")` |
| 利用できない preview(✅PV-1) | `PreviewGraphProgram::available` が false なら、`EditorPreviewService::renderPreview()` が gate と XR の検査の後、params を読む前に `EditorPreviewErrorCode::preview_unavailable`(名前 `preview.unavailable@1`、payload `{method, reason}`)を投げ、executor を呼びません([PV-1 の `checkPreviewAvailable()`](../../src/core/communication/editorpreviewservice.cpp#L134)) |

隔離の証明が [`previewStateInventory()`](../../src/core/vkcore/previewexecutor.hpp#L59) です。コメントが規範です。

> Literal WP172 ownership inventory.  The order is part of the diagnostic
> contract and is deliberately shared by RPC, tests, and the design report.

12 行の各エントリが `request-local` / `explicitly suppressed` / `read-only` のいずれかに分類され、DeletionQueue、RenderTargetContainer history、`PolygonInstanceContainer` の前フレーム状態、`Renderer` の temporal history、swapchain / XR mirror などが **明示的に抑止されている** ことを列挙します。`Renderer` 側の対応は [`previewIsolationStateJson()`](../../src/core/vkcore/renderer.cpp#L3983) です。

> 🧩 **難所 — 圧縮しない zlib を書く**([`zlibStored()`](../../src/core/vkcore/previewexecutor.cpp#L42) / [`crc32()`](../../src/core/vkcore/previewexecutor.cpp#L22) / [`appendChunk()`](../../src/core/vkcore/previewexecutor.cpp#L33))
>
> **何をする所か**: preview の RGBA8 バッファを、zlib も libpng も使わずに PNG バイト列へ変換します([`encodePng()`](../../src/core/vkcore/previewexecutor.cpp#L69))。
>
> **素朴に読むと**: `0x78 0x01` の 2 バイト、`LEN` とその 1 の補数 `NLEN`、テーブル無しのビット単位 CRC32、手書きの adler32 が並んでいて、「圧縮ライブラリを避けた」だけではなぜこれが PNG として正当なのかが分かりません。理屈はこうです。PNG の `IDAT` は生の deflate ではなく **zlib ストリーム**でなければなりませんが、deflate には**無圧縮ブロック(type 00)**があり、「最終ブロックフラグ 1 bit + LEN + NLEN + 生バイト」という並びだけで完全に合法です。だから「圧縮しないが正しい zlib」が数十行で書けます。`0x78 0x01` は CM=8 / CINFO=7 で、FCHECK 条件(2 バイトを big-endian で見て 31 の倍数、`0x7801 = 31 × 991`)を満たす既定ヘッダです。65535 で切りながらループするのは `LEN` が uint16 だからで、`IHDR` の `{8, 6, 0, 0, 0}` は 8 bit / truecolor+alpha / 非インタレースを意味します。
>
> **骨子**:
>
> | 順 | zlib ストリームの要素 | 意味・繰り返し |
> |---|---|---|
> | 1 | `78 01` | ヘッダ |
> | 2 | `BFINAL`, `LEN`, `~LEN`, `raw ≤65535` | この順で無圧縮ブロックを構成し、`{ … }*` として繰り返す |
> | 3 | `adler32(圧縮前バイト列)` | チェックサム |
>
> chunk の並びは次のとおりです。
>
> | 順 | chunk の要素 | CRC の対象 |
> |---|---|---|
> | 1 | `len` | 対象外 |
> | 2 | `type` | 対象 |
> | 3 | `data` | 対象 |
> | 4 | `crc32(type + data)` | `type + data` の計算結果 |
>
> - **len**: CRC の対象外。
>
> **手がかり**: 2 つのチェックサムは**対象が違います**。adler32 は zlib ストリームの中身、つまり**圧縮前**のスキャンライン列に対して取り([adler32 の計算](../../src/core/vkcore/previewexecutor.cpp#L59))、CRC32 は各チャンクの `type + data` に対して取ります(長さフィールドは含めません、[CRC の対象範囲](../../src/core/vkcore/previewexecutor.cpp#L39))。取り違えると「多くのビューアは開けるのに一部が壊れていると言う」という追いにくい形で出ます。各スキャンラインの先頭に付く `0` は PNG の filter type(None)で、これも省けません([filter byte を積む行](../../src/core/vkcore/previewexecutor.cpp#L74))。`crc32()` の `0xedb88320u & (0u - (crc & 1u))` は分岐無しの条件付き XOR です。
>
> **不変条件**: adler32 は圧縮前、CRC32 は `type + data`、無圧縮ブロックは 65535 byte 以下。エンコーダが入力だけで決まる純粋関数であること — 外部の圧縮器に置き換えると同じ入力から同じバイト列が出る保証が消えます(テストは [`editorpreview_test.cpp`](../../test/editorpreview_test.cpp)、base64 が `iVBORw0KGgo` で始まる = PNG シグネチャ一致を固定)。

### preview / `render_preview` の隔離 {#gotcha-9-19}

- `render_preview` は **`Renderer::renderLogicalFrame()` を通りません**([`PreviewGraphProgram`](../../src/core/renderingpass/previewgraph.hpp#L22) 直前のコメント)。したがって temporal history、FrameResources slot、layout tracker などのライブ状態を汚しません。
- 汚していないことの証明が [`previewStateInventory()`](../../src/core/vkcore/previewexecutor.hpp#L59)(「並び順も診断契約の一部」)と [`Renderer::previewIsolationStateJson()`](../../src/core/vkcore/renderer.cpp#L3983) です。
- 上限は 2048px / 16 MiB([`previewexecutor.hpp` 内](../../src/core/vkcore/previewexecutor.hpp#L54))。超過は `PreviewCaptureTooLarge` です。
- `PreviewCaptureRequest::graph_generation` が `PreviewGraphProgram::generation` と食い違えば `std::invalid_argument("preview graph generation mismatch")` で拒否されます(stale preview の防止)。
- **現在の出力は CPU 模式ラスタです**([第6章 §6.19](06_rendering_vulkan_shader.md))。material も shader も評価しないので、`render_preview` の画像を最終描画の代用と見なさないでください。

## 6.20 RT3-1: 加速構造の宣言面(`ray_group_layout` version 2 で導入、現在は version 3)

WP374 系(RT3-1)より前は、BLAS/TLAS に載る値をプロジェクトが宣言できませんでした。mask は `0xff` 固定、`instanceCustomIndex` には canonical 行番号 `q` がそのまま入っていました。version 2 の layout がこの2つを利用者の宣言へ移しました。MG-2a からは読める layout が version 3 だけになり、version 1 / 2 は [`pelican.ray_group_layout.version_unsupported@1`](../../src/core/shader/raygrouplayout.cpp#L658) で拒否されます(旧版の意味で読む分岐は残っていません)。加速構造そのものの作り方は[第11章](11_ray_query_acceleration_structures.md)、ここでは**宣言がどう解決されるか**だけを扱います。

### 担体は粒度で分かれる

| 値 | 粒度 | 担体 |
|---|---|---|
| `mask`(U8) / `instance_flags` / `custom_index`(24 bit) | instance | scene の `pelican_ray_instance` component([`RayInstanceComponent`](../../src/core/userpublic/components/rayinstance.hpp#L67)) |
| `geometry_flags` | geometry | layout の `acceleration_structure.rules`(material 鍵規則) |

省略時の値は1箇所にまとまっています。[`rayAccelerationStructureCanonicalDefaults`](../../src/core/userpublic/components/rayinstance.hpp#L33) が `mask = 0xff` / `instance_flags = 0` / `custom_index = 0` / `geometry_flags = 0` で、layout parse・component 解決・TLAS lowering・テストがすべて**この値を参照し、リテラルを書き直しません**(コメントが規範)。

flag の語彙は Vulkan の operand bit そのものです([`rayInstanceTriangleFacingCullDisable` 以下](../../src/core/userpublic/components/rayinstance.hpp#L12))。authoring 層と scene 層が Vulkan ヘッダに依存せず解決済み宣言を運べて、AS builder はそのまま渡せる、という選択です。

### layout 側の `acceleration_structure`

```json
{ "schema": "pelican.ray_group_layout", "version": 3,
  "acceleration_structure": {
    "defaults": { "mask": 255, "instance_flags": [], "geometry_flags": [] },
    "rules": [
      { "material": {"document": "materials/multi.json", "material_name": "Oak"},
        "geometry_flags": ["opaque"] },
      { "material": {"asset": "models/tree.glb", "source_material_index": 7},
        "geometry_flags": [] } ] } }
```

型は [`RayAccelerationStructureDeclaration`](../../src/core/shader/raygrouplayout.hpp#L89) で、`defaults` と [`RayAsGeometryRule`](../../src/core/shader/raygrouplayout.hpp#L82) の列だけです。selector は [`RayAsMaterialSelector`](../../src/core/shader/raygrouplayout.hpp#L67) の typed union(`document_material` か `asset_material`)で、**同一 exact key の重複規則は first-match にせず拒否**します([`ray_as_duplicate_rule`](../../src/core/shader/raygrouplayout.cpp#L536))。照合の実体は [`rayAsSelectorMatches()`](../../src/core/shader/raygrouplayout.cpp#L547) で、document selector は `material_name` が空なら名前を問わず一致し、名前付きなら一致を要求します。

### 優先順位は `defaults < material rule < instance override`

解決は [`resolveRaySceneRows()`](../../src/core/shader/raygrouplayout.cpp#L1328) の1関数に閉じています。行ごとに defaults を置き、rule があれば `geometry_flags` だけを上書きし、`pelican_ray_instance` があれば **field 単位で** mask / instance_flags / custom_index を上書きします。`RayInstanceComponent` の3つが `std::optional` なのはこのためで、「authored の空配列 `instance_flags: []` は本物の override、component 全体が空なら codec が拒否」という規則がヘッダのコメントに書いてあります([`RayInstanceComponent` 直前](../../src/core/userpublic/components/rayinstance.hpp#L65))。

### 状態の表

layout の有無と宣言の有無で、解決値と名前付き拒否は次のように決まります。RT3-1 の時点では v1 layout の行(`custom_index = q`、`pelican.ray_as_declaration_requires_layout_v2@1`)を含む 10 状態でしたが、MG-2a で v1 / v2 の読み手が消えたので、それらは #2 の拒否 1 つにまとまりました。

| # | 入力 | 結果 |
|---:|---|---|
| 1 | layout 自体が無い(`LegacyNoLayout`) | row snapshot を作らず、TLAS は legacy: custom index は **登録順の連番**、mask/flags は canonical defaults([`lowerRayTracingTlasInstancePlan()` の legacy 分岐](../../src/core/renderer/accelerationstructure.cpp#L718)) |
| 2 | layout の `version` が 3 以外(1 / 2 を含む) | parse で [`pelican.ray_group_layout.version_unsupported@1`](../../src/core/shader/raygrouplayout.cpp#L658)。`acceleration_structure` の有無を見る前に落ちます。`ray_as_declaration_requires_layout_v2@1` はもう作られません |
| 3 | layout が無い project の scene に builtin の `pelican_ray_instance` がある | scene load で [`pelican.ray_instance_requires_layout_v2@1`](../../src/core/loader/scene.cpp#L116)。code 名は公開時のまま据え置きで、今の意味は「layout が無い」です。resolver 側にあった同じ code の検査は、layout が無ければ resolver が呼ばれないので消えました |
| 4 | layout、`acceleration_structure` 無し | 全 row が canonical defaults。**`custom_index` は 0 であって `q` ではありません**(出所は `engine_default`) |
| 5 | layout + `defaults` だけ | `defaults` の4値がそのまま全 row へ |
| 6 | layout + material rule が1つ当たる | その row の `geometry_flags` だけが rule 値へ([rule 値の代入](../../src/core/shader/raygrouplayout.cpp#L1394)) |
| 7 | layout + rules 有り、row の material identity が `Unavailable` | [`pelican.ray_as_row_material_identity_unavailable@1`](../../src/core/shader/raygrouplayout.cpp#L1382)。黙って defaults へ落としません |
| 8 | layout + 2つ以上の rule が一致 | [`pelican.ray_as_rule_ambiguous@1`](../../src/core/shader/raygrouplayout.cpp#L1389) |
| 9 | layout + `pelican_ray_instance` の override | field 単位で defaults / rule の上に載る([instance override の枝](../../src/core/shader/raygrouplayout.cpp#L1399)) |

override 側には3つの範囲・整合検査が続きます。`custom_index` が [`rayCustomIndexMaximum`](../../src/core/userpublic/components/rayinstance.hpp#L18)(`0x00ffffff`)を超えると [`pelican.ray_instance_field_range@1`](../../src/core/shader/raygrouplayout.cpp#L1411)、`force_opaque` と `force_no_opaque` を同時に立てると [`pelican.ray_as_conflicting_instance_flags@1`](../../src/core/shader/raygrouplayout.cpp#L1421)、portable alpha profile が有効な layout で `force_opaque` を使うと [`pelican.ray_profile_opaque_as@1`](../../src/core/shader/raygrouplayout.cpp#L1426) です。

> **設計決定:** 自動移行はしません。RT3-1 の時点では既存の v1 layout をそのまま読み、`custom_index == q` を固定した test を v1 の test として残していました。MG-2a(v0 は形式ごとに 1 版)で v1 / v2 の読み手を消したので、旧版の layout は version 3 の形へ書き直すしかありません。その test は「version 1 / 2 は名前付きで拒否され、version 3 の row は `q` ではなく既定値を取る」ことを確かめる test に変わっています。[`RayLayoutSemanticMode`](../../src/core/shader/raygrouplayout.hpp#L45) は `LegacyNoLayout` / `LayoutV3` の2値になり、row と snapshot が運んでいた `semantic_mode` のフィールドは消えました。

### `custom_index` の供給源は `q` ではない

layout がある project では `gl_InstanceCustomIndexEXT` に出るのは**宣言値**(宣言が無ければ既定の 0)です。`q`(canonical 行番号)は SBT の record base を決める値であって、shader へ公開する識別子ではありません。実装上この2つは同じ `RaySceneRow` に並んでいるので混ざりやすく、テスト側に専用の seam が置かれています。

- [`rayTracingInstanceRecordOffset(q, ray_type_count)`](../../src/core/shader/raygrouplayout.hpp#L556) が SBT 側の `q * R`。
- [`ScopedRayTracingTlasRecordOffsetOverrideForTesting`](../../src/core/shader/raygrouplayout.hpp#L622) が、TLAS lowering が消費するその値だけを差し替える test-only seam です。コメントが目的を書いています — **TLAS の base が誤って `q` になっていないこと**を、GPU の end-to-end sentinel で証明するためのものです。

したがって「row を追加して `q` がずれても宣言した `custom_index` は不変」と「SBT の sentinel は独立に正しい」を、同じ GPU テストで同時に見る形になっています。

### BLAS variant: 鍵はgeometry列

`geometry_flags` は BLAS の build 入力なので、同じ geometry でも flags が違えば別の BLAS が要ります。cache の鍵は [`State::BlasVariantKey`](../../src/core/renderer/accelerationstructure.cpp#L927) の `(geometry_kind, geometry_allocation_id, geometry_flags)` をgeometry順に並べた列です。1 geometryも同じ列形式で扱います。flagsだけの変更でも古いBLASを再利用しません。

purge は「geometry が消えたら落とす」ではなく、**そのフレームで実際に参照された variant 集合との差分**です。手順は次の形です。

```text
planned_instances = lowerRayTracingTlasInstancePlan(...)
groups = 互換な連続行をまとめ、geometry keyを列にする
for group in groups:
    同じkeyをcandidateへ登録済みなら共有する
    再利用可 かつ active_state_ に有れば candidate へ持ち越す
    無ければ build して candidate へ入れる
candidate->blas は参照されたgeometry列の集合と一致する
```

- **planned_instances / candidate->blas**: planned_instances は解決済み flags を含む。candidate->blas と参照された geometry 列の集合の一致が purge。

持ち越しは [`active_state_->blas.find(key)`](../../src/core/renderer/accelerationstructure.cpp#L1430) の一致で決まり、参照されないkeyはcandidateに入りません。commitでcandidateがactiveを置き換えるため、規則のreloadで不要になったvariantはactive集合から除かれます。送信済みの資源はsubmission leaseが保持します。

### 診断: `resident_blas_keys`

frame planの`ray_query_acceleration_structures`へ、BLASごとの先頭geometry情報と`geometry_count`が出ます([`rayQueryAccelerationStructureDiagnosticsToJson()`](../../src/core/renderer/accelerationstructure.cpp#L830))。現在、結合した全geometryの一覧はこの診断には出しません。

| キー | 意味 |
|---|---|
| `blas_variant_count` | candidate が持つ BLAS 数(= required variant 数) |
| [`resident_blas_keys`](../../src/core/renderer/accelerationstructure.cpp#L866) | 各 variant の `{geometry_allocation_id, geometry_flags, index_device_address, vertex_device_address}` |
| `active_blas_inputs` | 同じ配列(旧キー名。互換のため両方出ます) |
| `blas_build_count` / `tlas_build_count` | 実際に build した回数 |

配列は `(allocation_id, geometry_flags)` で [sort](../../src/core/renderer/accelerationstructure.cpp#L1555) されるので、診断の並びはフレーム間で決定的です。「同一 allocation を opaque と mask で共有したら BLAS build が 2 になる」「規則を戻したら variant が消える」を、この2キーで外から確認できます。

### WP377: SBT と TLAS の frame transaction、および既知の穴

RT の1フレームは、compile → pipeline → SBT → AS record → submit の5点をまたぐ1つの transaction です。各境界にはテスト用の注入点が置かれています([`RayTracingJointTransactionPoint`](../../src/core/shader/raygrouplayout.hpp#L639))。コメントが規範を書いています — **この5点でだけ呼ばれ、throw すれば実際の失敗と同じ rollback ガードを通る**。

| 点 | 呼び出し位置 |
|---|---|
| `after_compile` / `after_pipeline` | [registration 側](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1037) / [同](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1371) |
| `after_sbt` | [`createRayTracingSceneShaderBindingTables()` の直後](../../src/core/vkcore/renderer.cpp#L5889) |
| `after_as_record` | [`recordFrame()` の直後](../../src/core/vkcore/renderer.cpp#L5956) |
| `submit` | [logical frame の締め](../../src/core/vkcore/renderer.cpp#L6819) |

AS の公開は submission が成功した後です。[`FrameBuild`](../../src/core/vkcore/accelerationstructure.hpp#L260) は publication token で、コマンドと資源は即座に lease されるものの、新しい BLAS/TLAS の集合が active になるのは [`acceleration_structure_build->commit()`](../../src/core/vkcore/renderer.cpp#L6111) を通った後だけです。

> ✅ **WP384 で閉じた穴(`rev377a` #1)**: 以前は `recordFrame()` の前後で SBT と TLAS descriptor を `FrameResources` に先に書いていたため、`after_as_record` / `submit` の失敗で却下世代の SBT が active に残りました。いまは frame 中の候補を [`FrameResources::FramePublication`](../../src/core/renderer/frameresources.cpp#L403)(immutable な `RtDescriptorBundle` の候補)に束ね、target の三相 `submitLogicalFrame()` → publication callback(noexcept、[renderer.cpp の publication](../../src/core/vkcore/renderer.cpp#L6106) で RT state・staged runtime・bundle を 1 回で flip)→ `publishLogicalFrame()` の中央でだけ active を切り替えます。失敗時は候補の destructor が捨てるだけで、active は旧世代のまま(戻す処理が要らない設計)。却下 candidate は最初の実 submit 成功で seal された `DeletionQueue` の batch の fence で退役します。受け入れは各 fault 点直後の active SBT / TLAS 同一性と、再利用 frame で GPU が新世代を読むこと(`headless_render_test.cpp` の WP384 節)。

---

描画層は object 数が多いですが、境界は一貫しています。JSON は「定義」、planner は「順序」、runtime compiler は「GPU object」、executor は「コマンド記録」、frame target は「提出先と同期」を担当します。


### [3 つの identity の組み立て](../../src/core/renderer/accelerationstructure.cpp#L674) — scene / geometry / geometry_match {#reading-note-024}

[第11章「`RaySceneRowInput` の identity 3 種」](11_ray_query_acceleration_structures.md)が本体です。1 点だけ補うと、[コードコメント](../../src/core/renderer/accelerationstructure.cpp#L676)が「instance slot / generation / draw publication ordinal を使うと q が publish 順に依存してしまう」と明記していて、**scene_identity が authored/content 由来だけで作られているのはこの一文が理由**です。

### [`row.q != canonical_index` の拒否](../../src/core/renderer/accelerationstructure.cpp#L805) — q と record offset {#reading-note-025}

`q` は**正準ソート済み scene row 列の 0 起点連番**そのものです。この検査は TLAS 行が「静的シーンの正準置換」であることを保証し、q を配列添字として SBT / マテリアル表へ読み替えられるようにします。[`rayTracingInstanceRecordOffset(q, ray_type_count)`](../../src/core/shader/raygrouplayout.cpp#L2086) = `q * ray_type_count` は、1 インスタンスあたり ray_type_count 本の hit record を連続確保する前提での先頭 record 番号です。

### [`index >= (1u << 24u)` の上限](../../src/core/shader/raygrouplayout.cpp#L1548) — 24 ビットの出所 {#reading-note-026}

**Vulkan の `VkAccelerationStructureInstanceKHR::instanceShaderBindingTableRecordOffset` が 24 ビットのビットフィールド**だからです。GPU に載る値は `q * ray_type_count` なのでその積を 2^24−1 で制限し、行数側でも先に弾きます(`pelican.ray_row_count_limit@1`)。**別のフィールドですが**同じ 24 ビット幅の上限が [v2 の custom_index override の 24 ビット検査](../../src/core/shader/raygrouplayout.cpp#L1409) にも掛かります(こちらは `instanceCustomIndex` 側の 24 ビットで、超過は `pelican.ray_instance_field_range@1`)。

### [`resolveRaySceneRows()`](../../src/core/shader/raygrouplayout.cpp#L1328) — 同じ geometry_match_identity に別 binding を許さない {#reading-note-027}

geometry_match_identity は「同じ GPU ジオメトリ範囲が同じ transform で置かれている」ことしか意味しません。**物理的に区別できない同一ヒットに異なる group_index / record_data が割り当てられると、どちらのマテリアル束縛が当たるか決められない**ので、`pelican.ray_row_binding_ambiguous@1` で拒否します。

### [profile モードの material_index](../../src/core/shader/raygrouplayout.cpp#L1992) — なぜ `material_index == q` になるか {#reading-note-028}

profile では alpha override が **scene row ごとに異なり得る**(base_factor_a / uv_offset_scale / uv_rotation を行単位で上書きし、`uses_vertex_color` も行のジオメトリ属性依存)ため、1 行 1 エントリの per-row 表になります。キーが `"row:<q>:<material_reference>"` なので material_index は必然的に q と一致します。通常モードは行固有の上書きが無いので canonical key で参照し、同一マテリアルを 1 エントリに共有化してテーブルを縮めます([表の構築](../../src/core/shader/raygrouplayout.cpp#L1577))。

### [硬コードされた頂点オフセット](../../src/core/shader/raygrouplayout.cpp#L1000) — `10u` / `3u` / `12u` の出所 {#reading-note-029}

[`CommonVertStruct`](../../src/core/model/vertbufcontainer.hpp#L23)(`vec3 pos; vec3 normal; vec4 tangent; vec2 texcoord; vec4 color`、[サイズ 64・offset 0/12/24/40/48 を static_assert で固定](../../src/core/model/vertbufcontainer.hpp#L36))です。バイトオフセットを word 単位に割ると normal = 3、texcoord = 10、color0 = 12 になります。生成側は [`makeRayGroupLayoutVirtualInclude()`](../../src/core/shader/raygrouplayout.cpp#L858)。

### [`BlasVariantKey`](../../src/core/renderer/accelerationstructure.cpp#L927) — 一意性と再利用判定 {#reading-note-030}

BLAS の build 入力は geometry_allocation_id が決める頂点 / インデックス範囲と `VkGeometryFlagsKHR` の 2 つだけで完全に決まるからです(transform は TLAS 側)。[`can_reuse`](../../src/core/renderer/accelerationstructure.cpp#L1305) は `active_state_ != nullptr && !pool_invalidated && !model_invalidated` で、この 2 つの世代は[別カウンタ](../../src/core/renderer/accelerationstructure.cpp#L1263)として診断に出ます。

### [`descriptorSetLayoutKeysFor()` の set 数決定](../../src/core/shader/pipelinefactory.cpp#L496) — reflection と ray_layout の階層 {#reading-note-031}

set 0 は frame として常に固定、それ以外は reflection の最大 set 番号から決まり、ray_layout が ray table を使う場合**のみ** set 3 をエンジン定義の bindless material / texture / geometry レイアウトで上書きします。つまり **reflection が基底、ray layout が特定 set の権威**という階層です。set 0 の契約は[第11章](11_ray_query_acceleration_structures.md)にも書かれています。

### [shader group の stage index と SBT](../../src/core/shader/pipelinefactory.cpp#L771) — regions への対応 {#reading-note-032}

stage は raygen = 0、miss = 1..M、closest_hit = 1+M..、any_hit はその後ろ。group はその順で general(raygen)→ general(miss)×M → triangles hit group×N と並びます(**any_hit は独立した group にならず、対応する hit group の `anyHitShader` に畳み込まれる**ので、group 数は stage 数より any_hit の分だけ少ない)。[SBT 構築](../../src/core/shader/pipelinefactory.cpp#L880)はこの group 順に handle を取り、handle_alignment で stride を、base_alignment で各領域 offset とバッファ先頭を揃えるので、raygen / miss / hit の `VkStridedDeviceAddressRegionKHR{address+offset, stride, size}` がそのまま regions になります。

### [`RayTracingOracleContract`](../../src/core/shader/raytracingoracle.hpp#L14) — 定数群の根拠 {#reading-note-033}

**`docs/design_raytracing.md` の fixture subset 契約が出所**です。最小角 5°(`minimum_triangle_angle_degrees`)は退化三角形の排除、掠め入射の排除はそれとは別の `minimum_incidence_dot = 0.0871`(約 85°)、`ε_t = relative_t_epsilon · max(1,|t|)` は最近接 hit と次候補の分離(一意性)、barycentric マージンは辺・頂点近傍の回避、`endpoint_f32_ulp_padding = 4` は [GPU の f32 端点誤差の吸収](../../src/core/shader/raytracingoracle.cpp#L695)です。

### [`intersect()`](../../src/core/shader/raytracingoracle.cpp#L226) — double と区間演算を並走させる {#reading-note-034}

double 側は GPU と同じ ["Normative Moller-Trumbore sequence"](../../src/core/shader/raytracingoracle.cpp#L229) を**式順序込みで再現**して u/v/t の代表値と inside 判定を出し、有向丸めの区間側は同じ式で t の**保証付き区間**を作ります。前者だけでは真値包含の保証が無く、後者だけでは一意な代表値が得られません。

<a id="shader-catalog-internals"></a>

### shader catalog の構築

返却フィールドは[第13章の shader catalog](../manual/13_editor.md#engine-shader-catalog-fields)を参照してください。

反映は process に 1 つの worker thread が行います([worker](../../src/core/communication/engineauthoringlists.cpp#L361))。engine は RPC の endpoint を作った直後に worker を裏で始め(Studio が聞く前に一覧が出来ているように。[始める所](../../src/core/communication/rpcserver.cpp#L2852))、endpoint が閉じるときに止めて join します([止める所](../../src/core/communication/rpcserver.cpp#L2857))。止めるのは shader 1 本ごとの区切りで、作りかけの一覧は捨て、次に始めるときは最初から作ります([1 本ごとに見る所](../../src/core/communication/engineauthoringlists.cpp#L349))。

worker が動いておらず答えも無いときに `get_engine_shaders` が来れば、その呼び出しが worker を始めて `building` を返します。worker は自分専用の compiler([`ShaderCompiler::detached()`](../../src/core/shader/shadercompiler.hpp#L91))を使います。この compiler は module を読まず、project の disk の shader cache を持たず、`StartupMetrics` に記録しないので、反映は `get_status` の `startup`(shader の時間と cache の数)に入らず、frame の側の compiler の cache とも共有しません。読むのは engine に組み込まれた shader だけです。

`get_graph_catalog` は反映も compile もしません。Studio の Graph は、`building` を受けると 500 ms 後に poll から聞き直し、`failed` は `error` の文を notice に出して一覧を空として扱います(palette の「Reading...」はそこで止まります)。`status` の無い古い答えは今までどおり読みます(`src/devstudio/model/graphcanvasmodel.cpp`)。palette の render.compute で選んだ ray tracing の pipeline の task が書く image は、`resource_ports` の `format` があればその format で作ります(shadow mask なら `R8_UNORM`。無ければ `R16G16B16A16_SFLOAT`)。

`reflectShaderForms()` は source を compile して反映し、失敗時は同梱 SPIR-V を試します。resource port の include は pass ごとに生成するため、単体の一覧では空 include を使います。任意の port だけを読む shader は反映でき、無条件にアクセサを呼ぶ shader は compile に失敗する場合があります([反映入口](../../src/core/communication/engineauthoringlists.cpp#L251))。

storage image の format は reflection の layout qualifier から vocabulary 名へ変換します。`render.image` の lowering は宣言した usage を渡し、使用先の pass / task が必要な usage を検証します([lowering](../../src/project/rendergraphcomposition.cpp#L856)、[pass の attachment の usage(✅RC-3c2a から資源の検査の純関数)](../../src/project/generationresources.cpp#L373)、[compute port](../../src/core/renderingpass/computetaskcontainer.cpp#L310))。

Studio の Graph は material pass contracts、format aspects、engine shader 一覧を取得し、選択肢と接続検査に使います。実装確認の入口は `renderconfigeditor_test` と `engine_primitives_test` の EL-4 / MP-1 ケースです。
