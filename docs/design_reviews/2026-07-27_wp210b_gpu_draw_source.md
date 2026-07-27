# WP210b GPU-written indexed draw source 実装レビュー

日付: 2026-07-27
状態: fixed-state vertical slice 実装済み

## 結論

GPU computeが同一フレーム内でmaterial passのindexed draw commandとdraw countを生成し、
rendererが`vkCmdDrawIndexedIndirectCount`で消費する経路を追加した。最初の境界は
1つのCPU DrawQueue material rangeへpipeline、descriptor、vertex layoutを固定する。
これによりbindlessやGPU pipeline selectionを先に要求せず、culling/particleへ必要な
command transport、同期、feature fallbackを実GPUで検証できる。

## 公開契約

- `command_layout: "indexed_draw"`: packed `VkDrawIndexedIndirectCommand`、20 byte stride
- `command_layout: "draw_count"`: 1個の`uint32_t`
- `host_source: "scene_draw_commands_v1"`: 現フレームのCPU DrawQueue command候補
- material pass `gpu_draw_source`:
  - `commands` / `count`
  - `max_draw_count`
  - `command_offset` / `count_offset`
  - `fallback: "cpu_draw_queue"`
  - `execution: "automatic" | "cpu"`

CPU compile時にbuffer参照、command layout、4-byte alignment、範囲、
`material_range.count == 1`を検証する。GPU登録後はactive generationの
`FrameGraphBufferId`へ解決し、hot reload candidateと旧in-flight generationを
名前だけで混同しない。

## 実行と同期

`gpu_draw_source`のcommands/countはrender nodeのreadとなる。ordered producerが同resourceを
writeすると、実行側はcompute shader writeから`DrawIndirect` stageの
`IndirectCommandRead`へbuffer barrierを発行する。現plannerは宣言順より後ろのwriterを
自動producerにしないため、top-level adapterでpassより後に追加されるcompute taskは
consumer passへの`before`を明示する。

Vulkan 1.2 coreと`VkPhysicalDeviceVulkan12Features::drawIndirectCount`の両方を確認して
device作成時にfeatureを有効化する。実行上限はauthored `max_draw_count`と
`maxDrawIndirectCount`の小さい方である。feature未対応または`execution: "cpu"`では、
同じ固定状態に対する既存CPU DrawQueue commandへfallbackする。

## 検証

CPU contract test:

- host source / command layoutのparseと名前
- commands/countの自動read、明示順序後の2本のRAW barrier
- unknown/wrong layout、範囲超過、unaligned offset
- 複数material range、非material pass、host source/layout不一致の拒否

実Vulkan golden:

- count 0: geometryを描画しない
- count 1: candidateの一部だけを描画
- count max: CPU baselineとsemantic image一致
- count overflow: `maxDrawCount` clamp後にCPU baselineと一致
- `execution: "cpu"`: GPU countが0でもCPU baselineと一致
- producerがconsumer passより前に計画される

fixtureのcompute shaderは2 commandのidentity copyであり、culling algorithmそのものを
実装したという主張には使わない。

## 意図的に残した境界

1. 候補列にmaterial/pipeline/skinned-state segment metadataはない。
2. GPU commandだけでCPU DrawQueueに存在しないpipeline/material状態は作れない。
3. 複数material/pipelineを1 consumerで切り替えるGPU-visible state keyは未設計。
4. Vulkan 1.1 + `VK_KHR_draw_indirect_count`だけのextension経路は未実装。
5. project-owned culling/particle、hot reload、XR per-view、GPU timingの受け入れは未完。

## 次の縦切り

次は実用アルゴリズムを1つ選び、候補commandに必要なboundsとsegment metadataを最小限
追加する。推奨はdepth pyramidを再利用するocclusion cullingである。まず1 fixed-state
segmentでvisibility compactをdogfoodし、その計測後にmulti-segment tableを一般化する。
bindlessはその段階でdescriptor pressureが実測blockerになった場合だけ別WPにする。
