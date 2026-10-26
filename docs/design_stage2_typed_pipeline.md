# 段階 2 の強化: 計画用 graph を project で 1 回だけ作り、core はそれを受ける

対象読者: エンジン担当(描画コンパイラ / renderer)。
> **置き換え済み(2026-10-24)**: このメモは前提を 2 回誤った(「`CompiledRenderPipeline` に `logical_graph` が既にある」「計画用 graph を移すだけで挙動不変」)。現在の方針は [design_render_graph_ir.md](design_render_graph_ir.md)。以下は履歴。

ステータス: 方針決定(2026-10-17。「段階 2 の強化と二重経路のリファクタは行う」)。**2026-10-24 に前提の誤りを直して書き直し**(利用者了承)。
RC-3a / RC-3b / RC-4 として WP 化。
前提: [design_plan_layer_inventory.md](design_plan_layer_inventory.md) 2-6、
[design_render_preset_feature_recipe.md](design_render_preset_feature_recipe.md)(compose の出力を触る WP と同時期)、
[design_pass_fusion.md](design_pass_fusion.md)(PF-3 が node の pass の形を要る)、
[design_pipeline_state_nodes.md](design_pipeline_state_nodes.md)(PSN-1 の `PipelineState` を RC-4 が運ぶ)。

## 0. 要約

- 2026-10-17 版は「段階 2 の出力 `CompiledRenderPipeline` に typed な論理 graph `logical_graph` が既にあり、core が同じ JSON を読み直している」と書いたが、**誤りだった**
  (2026-10-23、RC-3a の橋の test を書いている途中で codex が発見、統合側が確認)。`CompiledRenderPipeline`([renderpipeline.hpp](../src/project/renderpipeline.hpp))は policy しか持たない。
- 実際は **graph の構造を読むのは core だけ**で、しかも同じ composed config を何度も読み直している(§1)。論理 graph(`CompiledLogicalRenderGraph`、型は project の [logicalrendergraph.hpp](../src/project/logicalrendergraph.hpp))を作るのも core の `compileLogicalFrameGraphShadow` だけ。
- 目指す姿は変わらない: **pass の JSON を 1 回だけ読み、typed な graph を project で作り、core は受け取って Vulkan に写すだけ**。変わるのは道筋で、「既にある論理 graph を広げる」ではなく「core にある計画用 graph の層(`FrameGraphDefinition` 一式)を project へ移す」。
  `FrameGraphDefinition` は既に backend 非依存(attachment の load / store は自前の enum、material filter・view select・subresource は project の型)なので、型を新しく発明しない。

## 1. 今の経路(develop_v1 = 60ea40dd8 で確認)

```text
resolved / composed config(JSON)
  ├─ core: 実行用に parse → PassDefinition / RenderTargetDefinition(vk の型。registration)
  ├─ core: 計画用に parse → parseFrameGraphDefinitionsFromConfigJson → FrameGraphDefinition
  │        読み直しが 6 箇所: renderconfigeditor.cpp ×3、graphtransformregistry.cpp、subgraphreplacementregistry.cpp、
  │        vulkanrendercompilerprogram.cpp ×2
  │        ├→ planFrameGraph → FramePlan(順序・barrier。compiler が variant ごとに作る)
  │        └→ compileLogicalFrameGraphShadow → CompiledLogicalRenderGraph → project の物理計画(compileVulkanTargetPlan)
  └─ project: 段階 2 compileRenderPipeline → CompiledRenderPipeline(policy だけ。graph なし)
```

- `makeFrameGraphDefinition(RenderingPassDefinition)`(core の PassDefinition から作る道)は production から呼ばれていない(test だけ)。
- 計画用の node の parse(`parseRenderNodeFromJson` / `parseComputeNodeFromJson`、frameplanner.cpp)は、実行用の parse と helper を共有している
  (`renderingpassjsonhelpers`、`passattachmentoptionsjsonparser`、`materialpassinfojsonparser`、`passfieldownershipcapabilities`。どれも core の `renderingpass.hpp` を include する)。これが移すときの主な絡まり。
- 結果: FramePlan と論理 graph は「同じ JSON をそれぞれ読んだ」ことで一致しているだけ。RC-2 の codex レビューは、差し替えた compiler が古い FramePlan と新しい計画を混ぜて返しても通ることを示した(RC-2b では閉じず、ここで閉じる)。

## 2. 段階

| 段 | WP | 内容 | 受け入れ |
|---|---|---|---|
| 1 | **RC-3a** | 計画用 graph の層を project へ移す: `FrameGraphDefinition` 一式の型、JSON → graph の parse、`planFrameGraph` と `FramePlan` の型、`validateFramePlanStructure`、`compileLogicalFrameGraphShadow`。共有 helper は「計画用に要る backend 非依存の部分」を project に切り出し、core の実行用 parse はそれを使う。core の呼び出し元は project の関数を呼ぶ | **挙動不変**: plan の fixture(`example_frame_plan.json` ほか)が byte 一致、golden 不変。`src/core` に計画用 graph の JSON parse が残らない。project の test だけで JSON → FramePlan まで届く |
| 2 | **RC-3b** | 段階 2 が計画用 graph(と論理 graph)を **1 回だけ** 作って `CompiledRenderPipeline` に載せる。runtime の経路(compiler の `planFrameGraph`、物理計画の shadow)は載ったものを受ける。FramePlan と論理 graph を同じ定義から作り、compiler が返した FramePlan が定義と食い違えば名前付きで断る。node に pass の形(fullscreen / raster / material / compute …)を持たせる(PF-3)。編集中の候補を検証する経路(renderconfigeditor、graph transform、subgraph replacement)は候補の JSON を読む必要があるので、project の parse を呼ぶ形のまま | plan と golden 不変。compiler の出口で FramePlan と定義の混在が断られる test。PF-3 が pass の形を読める |
| 3 | **RC-4** | 実行用 `PassDefinition` の backend 非依存部分(論理 op、sample count、shader 参照、pass_info、PSN-1 の `PipelineState`)を project の型へ。core は vk への写像だけ。pass の JSON を読むのが 1 回になる。**注記(2026-10-27、RC-4 の着地 f191624f2。上の文は当時の計画のまま残す)**: 実際にやった範囲は、中立な leaf の型(shader 参照と `makeShaderReference()`、fullscreen / raster / 補助 pass / UI の info、`PassLoopIndex`、`ComputeTaskDefinition`)を project へ移すことと、読み手の無い欄(`PassDefinition::region_tags` / `resolution_domain` / `once`、`ComputeTaskDefinition::after` / `before`)と helper(`passOutputSamples`、`parseAuthoredAttachmentLoadOp` / `StoreOp`)を削ることだけ。値の持ち主は変えず、material の info と binding・`ImGuiPassInfo`・`PassDefinition`・`rasterization_samples`(vk の型のまま)・RT の layout の解決は core に残る。論理 op(RC-3a2 の `AttachmentLoadOp` / `AttachmentStoreOp`)と `PipelineState`(`src/project/rasterpass.hpp`)はすでに project の型。**値の持ち主を分ける作り替え(中立な実行値の型と継承、組み立ての関数を project に)は利用者の決定(2026-10-27)で見送り**、2 つ目の backend(WebGPU など)が具体化した時に設計し直す。「pass の JSON を読むのが 1 回」は RC-3c2b2 で済んでいた。今の記録は [design_render_graph_ir.md](design_render_graph_ir.md) §2.7 の RC-4 の追補と §5 の RC-4 の行 | 全 pass 種類の headless、golden 不変 |
| 4 | (後) | 段階 2 の出力の canonical hash で、変わっていない rendering pass の prepare を省く(hot reload の incremental) | hot reload の時間の前後、決定性 gate |

- RC-3a の前に v1feature(PF / PSN)へ一声: PF-1 の test は `parseFrameGraphDefinitionsFromConfigJson` → `compileLogicalFrameGraphShadow` を使うので include が変わる。RC-4 の前にも一声(PSN-1 の `PipelineState` と重なる)。

## 3. 性能

- 触るのは registration / hot reload / 編集の検証の CPU 段だけ。毎フレームの経路(`renderLogicalFrame` → `executePlannedFrameGraph`)は prepare 済みの handle を読むだけで変わらない。
- 減る側: 実行時の経路での graph の読み直し(RC-3b)、pass JSON の 2 回読み(RC-4)。

## 4. やらないこと

- `planFrameGraph` のアルゴリズム変更、`get_frame_plan` の鍵の変更(RC-3a / 3b では変えない)。
- Metal 向けの抽象(第 2 backend は書かない。[design_macos_moltenvk_proof.md](design_macos_moltenvk_proof.md))。
- Studio の変更(Studio は `get_frame_plan` の JSON を読む)。

## 5. 未決

- RC-3a で共有 helper をどこまで project に移すか(計画用に要る部分だけを切るか、実行用 parse ごと RC-4 で移すまで二重のままにするか)。RC-3a の実装者が依存を地図にして決め、報告する。
- RC-3b で FramePlan を compiler に作らせ続けて照合するか、runtime が定義から作り直すか(後者なら照合は要らないが、差し替え compiler の自由度が減る)。
- 段 4 の hash の粒度(rendering pass 単位か、pass 単位か)。
