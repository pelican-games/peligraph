# WP238b Generic Raster Pass ABI vertical slice

## 目的

custom raster techniqueを追加するたびにengine-owned `PassInfo`、parser、runtime dispatchを
専用分岐で増やす構造を止める。通常authoringは簡潔な既定値を使いながら、必要な利用者は
draw algorithm、shader、fixed-function state、任意個のattachment/resource contractを
局所的に差し替えられる最小の共通ABIを作る。

## 実装

- `src/project/rasterpass.*`
  - backend非依存`RasterPassContract`
  - open/versioned geometry implementation ID
  - typed `RasterDirectDrawOperation`
  - portable topology/cull/front-face/depth/blend/write-mask
  - canonical JSONとstable fingerprint
- `GenericRasterPassInfo` / `type: "raster"`
  - versioned shader implementation provenance
  - vertex/optional fragment shader reference
  - typed image/buffer `resource_ports`
  - colorまたはdepthのみを含む任意attachment構成
- `rasterpassvulkanadapter.*`
  - portable stateから`GraphicsPipelineDesc`への一方向lowering
  - fused physical scopeのlogical-location / physical-slot写像
  - unused attachment slotのwrite mask 0
  - integer targetでのblend拒否
- runtime
  - 既存fullscreen containerのdescriptor/lifetime実装を内部再利用
  - generic direct drawのvertex/instance/first値を実行
  - sequential/multiview、history/subresource、typed buffer/image inputを再利用
  - typed same-pixel inputをinput attachment ABIへlowering
  - engine固定の8-input rejectionを削除し、device/pipeline layoutを実上限とした
- common execution
  - `pelican.logical.raster@1`
  - `pelican.execution.generic_raster_direct@1`
  - graphics capabilityを持つ既存device endpointへ合流
- dogfood
  - `sprite_demo`の`ssao_clear`を3-vertex generic raster passへ移行

## 受け入れ

- portable contractの正規化、19 attachment、invalid version/operation/depth/countをCPU test
- parserでopen implementation ID、shader、sampled image/storage buffer portをCPU test
- Vulkan adapterで2 logical MRTを3-slot fused scopeへ並べ替え、unused slotを無効化
- full pass parserで2 color + depthのgeneric raster passを構築
- frame plannerと`FrameExecutionPlan`でraster dialect/implementationを保持
- 既存rendering-pass helper、frame planner、execution-plan、runtime transaction回帰を実行

## 意図的な残件

- operation variantはprocedural direct drawだけ。indexed、indirect/count、mesh/task、
  custom vertex streamは具体workloadとbackend verifierを伴う別sliceで追加する。
- material、shadow、debug、UIの内部実装はまだ共通contractへ移していない。
- runtime class名`FullscreenPassContainer`はcompatibility detailとして残る。公開ABIではなく、
  indexed/indirect operation追加時にdescriptor-owning raster executorへ改名・分離する。
- complete raw physical planと`NativeScope`はWP238cで扱う。
