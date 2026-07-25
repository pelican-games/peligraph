# WP203b phase 2 完了レポート — production multiview runtime

日付: 2026-07-26

## 結果

WP203aのphysical view planとWP203b phase 1のVulkan primitiveを、production
pass compilation、layered input、mixed-scope scheduling、Rendererのlogical-frame
境界まで接続した。OpenXR swapchain自体はまだeye別2Dであり、通常XRは意図的に
sequentialを維持する。array swapchain、composition depth、実runtimeでの
semantic/performance gateはWP203cが所有する。

## Runtime compilation

- XRかつview-family targetを明示的に有効化した構成だけがmultiview capabilityを
  target plannerへ渡す。
- builtin fullscreen implementationと対応済みengine shaderの組だけを事前候補にする。
  custom implementationとmaterial passは逐次scopeに残る。
- runtime compilerはpassに対応するimmutable `VulkanPhysicalScopePlan`を取得し、
  `GraphicsPipelineViewContract`と各inputの`PassInputViewDimension`をcompiled passへ残す。
- multiview variantには`PELICAN_MULTIVIEW`、view count、layered input bindingをdefineする。
  PipelineFactoryのSPIR-V reflectionが`gl_ViewIndex`を最終gateにする。
- engine shaderは`pelican_view.glsl`を介し、同じsourceで`sampler2D`と
  `sampler2DArray`、scalar viewと`gl_ViewIndex`を切り替える。

## Resource binding

- fullscreen descriptorはshared 2Dならlayer 0、sequentialならviewごとの2D view、
  multiviewなら全layerの2D-array viewをbindする。
- material screen inputもsequential viewごとのdescriptor variantを持つ。
  material passそのもののmultiview実行はまだadvertiseしない。
- history parity、resize後のrebind、MSAA attachment/resolve viewの既存契約を維持する。
- invocationのview count/indexとphysical input dimensionが一致しない場合は
  command recording前にfail-fastする。

## Logical-frame scheduling

`buildLogicalFrameViewFamilySchedule`はphysical scopeをframe-graph node順に展開する。

- `single_view`: 一回
- `sequential`: logical view数回
- `multiview`: view mask付きで一回

Rendererは一つのview-family command context内でこのscheduleを消費する。barrierと
GPU timingのnode境界は最初/最後のinvocationへ対応し、FrameUBOのscalar/multiview
slotをscopeごとに選ぶ。compute、snapshot copy、sprite attachment、swapchain barrierも
view/layer契約を使う。history、instance temporal state、runtime-generation lease、
logical-frame publicationは従来どおりframeに一回だけである。

## 検証

ローカルの`PELICAN_WITH_OPENXR=OFF` Debug構成で次を確認した。

- runtime target planからmultiview fullscreen shader/pipelineとlayered descriptorを
  実際にcompileし、sequential-only inputを拒否する。
- synthetic Vulkan描画は左右で異なるFrameUBOを一回のdynamic renderingで出力し、
  二回のsequential referenceとlayerごとにbyte一致する。
- mixed schedule、欠落scope、矛盾したexecution count/view maskのCPU fixture。
- flat headless 185 assertions / 5 cases。
- logical stereoを含むtemporal suite 127 assertions成功、7 cases成功・OpenXR専用1 case skip。
- target planning 74/11、sample count 49/7、graph variant 38/6、
  shader reflection 76/6、frame planner 141/17、rendering-pass helper 206/39。
- multiview GPU/runtime fixture 46 assertions / 1 case。
- `git diff --check`成功。

全テスト実行ファイルを再生成する一括buildは、低write artifact方針に従い行っていない。
変更に直接関係するGPU/renderer/compiler/planner suiteを再buildして実行した。

## WP203cへ渡す境界

1. OpenXR color swapchainを2-layer arrayへ変更し、full-array viewとper-layer viewを
   `beginViewFamily`から返す。
2. depth swapchainと`XrCompositionLayerDepthInfoKHR`を同じacquire/release lifecycleへ入れる。
3. targetがview-family contractを満たした時だけ`enable_multiview_runtime`を有効にする。
4. sequential referenceとのsemantic image比較、mirror、session再作成、GPU timestamp改善を
   Simulatorと実機でgateする。
