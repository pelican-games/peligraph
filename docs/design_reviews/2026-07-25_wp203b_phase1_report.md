# WP203b phase 1 中間レポート — Vulkan multiview runtime primitives

日付: 2026-07-25

## 結果

WP203aのtyped view execution planを、OpenXRに依存しないVulkan runtime primitiveへ
縦切りした。このphaseはWP203bの完了ではない。production pass variantとscope schedulerを
有効化する前に、resource、shader、pipeline、command recordingの契約を実GPUで固定する。

## 実装済み

- `ImageWrapper`とimage allocationがarray layer数を保持し、layout transitionも
  image全layerを対象にする。
- target planのresource layer数を`RenderTargetDefinition`へ適用する。
  flat/XR variantが同じlogical target名を共有する場合は、mutable GPU registration前に
  最大layer数へ統合する。
- internal render targetは各layerの2D viewと全layerの2D-array viewを持つ。
  MSAA attachment / resolve image、history image、resize再作成も同じlayer契約を維持する。
- `FrameResources`は従来のper-view scalar slotを残したまま、in-flight frameごとの
  multiview UBO slotへ`FrameUniformData[view_count]`を連続配置できる。
- `pelican_frame.glsl`は通常buildのscalar blockを変えず、
  `PELICAN_MULTIVIEW`時だけ`pelicanFrame`を`views[gl_ViewIndex]`へ写像する。
- SPIR-V reflectionは`gl_ViewIndex`消費を検出する。graphics pipeline creationは
  contiguous view mask、view count、shader reflectionを検証し、dynamic-rendering pipelineの
  `viewMask`へ同じtyped contractを設定する。
- compiled passとexecutorはpipelineと同じview contractを共有する。
  executorはmultiview時に2D-array attachmentと`viewMask`を、sequential時に該当layerの
  2D attachmentを選ぶ。

## GPU証跡

synthetic headless Vulkan testは次を一つのfixtureで検証する。

1. 左右で異なるcamera/projection recordをmultiview UBOへ格納する。
2. `viewMask = 0b11`のdynamic renderingを一回だけ実行する。
3. 同じshader sourceのscalar variantを各layerへ二回描画する。
4. multiview結果とsequential結果をlayerごとにbyte比較する。
5. 左右layerが互いに異なること、非対応shaderをmultiview pipelineへ使うと
   Vulkan object作成前に拒否されることを確認する。

flat record一件のpack結果は既存`FrameUniformData` 352 bytesとbyte一致する。

## phase 1 検証

- multiview、logical stereo、OpenXR graph transition、headless readback、
  hybrid preset、target planning、sample planningのfocused 9 testsが成功した。
- `PELICAN_WITH_OPENXR=OFF`構成で`pelican_project`と`pelican_core`が成功した。
- 配列画像を単層向け簡易upload APIへ渡す経路は、暗黙に全layer分のsource bytesを
  読まず、明示的なlayer region APIを要求する。
- `git diff --check`が成功した。

## 残作業（WP203b phase 2）

1. pass implementationの事前capabilityとmultiview shader/pipeline variantを
   runtime compilationへ接続し、reflection結果を最終gateにする。
2. fullscreen/material screen inputをlayered descriptorとarray samplingへ接続する。
3. `VulkanPhysicalScopePlan`のsingle/sequential/multiviewを依存順に実行する
   logical-frame schedulerを導入する。view-independent workをeyeごとに重複実行しない。
4. synthetic logical-frame targetをRenderer境界へ接続し、command traceとflat byte fixtureを
   固定する。
5. 全CTestとproduction command pathのVulkan validationを完走する。

OpenXR 2D-array swapchain、composition depth、実機semantic/performance gateはWP203cの所有である。
