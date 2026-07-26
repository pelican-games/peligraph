# WP204 tile-local attachment runtime 実装レポート

日付: 2026-07-26

対象: RPE12b / WP204 tile-local runtime vertical slice

判定: **same-pixel logical footprintからの自動選択、physical scope fusion、
shader ABI、Vulkan dynamic rendering local read、runtime allocation、
single-view/multiview実Vulkan検証まで完了**

## 1. 実装した範囲

通常のrendering configへVulkan layoutやstorage modeを追加せず、次をすべて満たすresourceだけを
`pelican.vulkan.tile_local_plan@1`の候補にする。

- non-history、single-sampleのcolor/depth attachment
- producerがattachmentとしてwriteする
- 全consumerが非raster render passで、read footprintが`same_pixel`
- consumerの全attachmentとlocal-read resourceのphysical extentが一致する
- fused scope内にswapchain attachmentを含まない
- deviceで`VK_KHR_dynamic_rendering_local_read`のextension/featureが有効
- default formatがattachment、input attachment、transient attachment usageを受理する

material/custom/raster consumer、`neighborhood` / `arbitrary` / `temporal` read、extent不一致、
MSAA、feature/format非対応ではmaterialized candidateへ戻る。tile-local候補を持たないgraphへ
device capabilityを広告しないため、無関係なgraphがtile backendを選ぶこともない。

## 2. logicalからphysical scopeへのlowering

logical graphのread footprintとdependencyは正本のまま保持し、target compilerが
producer/consumerを一つの`VulkanPhysicalScopePlan`へ融合する。scopeは次を型付きで保持する。

- ordered node列とscope-local resource
- color attachment locationとinput attachment index
- depth input index
- attachmentごとのload/store
- rasterization sample count
- single-view / sequential / multiview、view count、execution count、view mask

link時には全nodeが連続し、同じextent/sample/view executionを持ち、local値のproducerと
consumerがscope内で閉じることを再検証する。scope全体のcolor attachment unionもdeviceの
attachment上限以下でなければならない。tile-local resourceのStoreはDiscardへloweringする。

per-view schedulerはphysical scopeをnodeへ平坦化せず、現在viewで実行するscopeをそのまま
抽出する。これによりsequentialでもproducer/consumer間のrendering instanceが切れず、
multiviewでは2-layer、`viewMask=0b11`、execution count 1を維持する。

## 3. shader / pipeline / command execution

engine fullscreen shaderは`PELICAN_DECLARE_INPUT_N` / `PELICAN_SAMPLE_INPUT`を使う。
通常planでは2Dまたは2D-array sampler、local-read planでは同じsourceから`subpassInput`
variantを生成する。SPIR-V reflectionとcompiled scope mappingを照合し、対応しない
material/custom implementationを自動候補から除外する。

graphics pipelineとcommand executionは
`VkRenderingAttachmentLocationInfoKHR` / `VkRenderingInputAttachmentIndexInfoKHR`を設定する。
一つのdynamic rendering instance内でnodeごとのpipeline/descriptor/drawを順に実行し、
node境界ではBY_REGIONのlocal-read dependencyを発行する。local-read対象attachmentだけを
`VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR`へ置き、output-only attachmentは通常layoutを保つ。

## 4. runtime resourceと複数graph

`RenderTargetStorageMode::tile_local_attachment`はattachment-onlyの実usageへ正規化し、
`INPUT_ATTACHMENT | TRANSIENT_ATTACHMENT`を付ける。VMA allocationはlazily allocated memoryを
優先し、存在しないdesktop deviceではdevice-local memoryへfallbackする。VkImageはgeneration
resourceであり、frameごとに作り直さない。

同名targetを複数graphが共有する場合は、materializedがtile-localより強く、tile-localが
write-only transientより強い。どれかのgraphがscope外保持を必要とすればimage自体は
materializedになるが、別graphのlocal-read scopeに必要な`INPUT_ATTACHMENT` usageは維持する。

physical fragmentは自動tile-local resourceをmaterializedへ戻し、scopeを合法な境界で分割する
保守的編集だけを受理する。materializedからtile-localへのpromotionや別automatic scopeの融合は
まだ受理しない。

## 5. 検証

pure planning fixtureはfeature/format、consumer kind、extent、MSAA、複数graph fallbackに加え、
tile-local融合scopeが2-view multiview contractを維持することを検証する。

実Vulkan fixtureは二種類ある。

1. headless single-view fixture: producerが`(0.2, 0.4, 0.8)`を書き、local-read consumerが
   channelを反転する。最終sRGB readbackは`(231, 170, 124, 255)`で一致する。
2. synthetic multiview fixture: 2-layer sourceを一回のmultiview renderingでlocal readし、
   左右layerの結果を検証する。

これらはOpenXR runtimeなしでVulkan image、pipeline、descriptor、layout、dependency、
view maskをvalidation付きで通す第2層gateである。Meta XR Simulator / Quest Linkの
composition target統合と、対象tile GPUでの帯域・GPU時間の実測は完了を主張しない。

最終cacheは`PELICAN_WITH_OPENXR=ON`で、次の対象を再実行した。

| 対象 | assertions | cases |
|---|---:|---:|
| target render planning | 187 | 22 |
| rendering-pass helpers/runtime compiler | 261 | 45 |
| runtime sample/target adapter | 168 | 16 |
| render-pipeline resolve | 124 | 13 |
| graph variant policy | 80 | 8 |
| XR composition protocol | 1041 | 13 |
| synthetic Vulkan multiview | 59 | 1 |
| headless Vulkan / hot reload / local read | 231 | 6 |
| **合計** | **2151** | **124** |

すべて成功した。実Vulkanの二対象は同時実行せず、device/driver stateを共有しないよう
直列で実行した。

## 6. コミット範囲

細かな作業コミットは、最終的に次の三つのレビュー可能な垂直単位へ整理した。

| commit message | 内容 |
|---|---|
| `feat(render): compile fused local-read scopes` | device capability、typed read footprint、physical scope/view契約、shader ABI、pipeline mapping、scope-local binding |
| `feat(render): execute fused local-read scopes` | Vulkan command dispatch、fused dynamic rendering executor、locality verifier、per-view scope保持 |
| `feat(render): select runtime tile-local attachments` | graph/device/formatによる自動選択、runtime allocation、fallback、E2E、文書 |

## 7. 残る境界

- Meta XR Simulator / Quest Link / 物理HMDでの現composition path検証
- Quest standalone等のtile GPUでの帯域・GPU時間・lazy memory behavior計測
- MSAA local read / resolve、一般material/custom/raster consumer
- depth local readの専用実Vulkan acceptance fixture
- alias groupの実memory binding
- physical fragmentによるaggressive fusion/reorder、raw barrier/queue、`NativeScope`
