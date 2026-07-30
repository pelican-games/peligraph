# WP238a FrameExecutionPlan vertical slice

日付: 2026-07-30

## 結論

既存`FramePlan`を削除・再解釈せず、その隣にbackend非依存の
`FrameExecutionPlan`を追加した。これは将来のCPU/GPU domain partition、
Generic Raster Pass、Vulkan/Metal sibling lowering、NativeScopeを同じfinal
execution publicationへlinkするための最小共通IRである。現行Vulkan command
recordingの分岐と描画順は変更していない。

## 固定した境界

- execution domainを閉じた`CPU|Graphics|Compute|Transfer` enumにしない。
  nodeはversioned capabilityを要求し、open IDのendpointを選ぶ。
- 閉じたenumはhost/device/externalというcoarse endpoint classだけに限定する。
- 共通planへVulkan handle、queue family、pipeline、native command payloadを入れない。
- resource使用はcurrent/previous epoch、read/write、intent、footprintから導出する。
  通常resource edgeのために作者へeffect記述を要求しない。
- dependencyのないcomponentを許可する。未知endpoint/node、capability不足、
  逆向きdependency、cross-endpoint bridge欠落はfinal closure errorにする。
- planはcanonical orderingとstable FNV-1a fingerprintを持つ。

## 接続

標準Vulkan compilerは各graphについてlegacy `FramePlan`を一度だけ作り、
physical target planのselected probeからendpointを取得してexecution planを生成する。
render/anchor/output、compute、snapshot copyはgraphics、compute、transferのopen
capabilityへ写る。compiler program verifierはframe/execution/physical graph coverageと
resource/barrier対応を検証する。

runtime preparationは両planを同じ`RendererRuntimeGeneration`へ格納し、一回のCASで
publishする。hot reloadテストではpass、legacy plan、execution fingerprint、sample plan、
material route、providerが同じrevisionであることを確認した。isolated caller向けには
legacy planから保守的なexecution planを作るcompatibility adapterだけを残した。

`currentFramePlanJson()`は`execution_plan`を併記し、semantic dialect、selected
implementation/endpoint、required/provided capabilities、resource use、effect、
dependency、bridge、fingerprintを観測できる。

## 検証

- `pelican_test_frameexecutionplan_test`: render/compute/copy mapping、resource intent/
  footprint、deterministic canonicalization、非連結graph、capability/dependency/
  fingerprint負例
- `pelican_test_rendercompilerprogram_test`: backend-native programのexecution coverage
- `pelican_test_renderpipelinetransaction_test`: generation内のexecution fingerprint整合と
  rollback/stale publication
- `pelican_test_frameplanner_test`
- `pelican_test_renderingsamplecount_test`
- `git diff --check`

## 後続

1. Generic Raster Pass ABIを同じresource/effect/capability語彙へloweringする。
2. queue-family/engine選択とbridgeをVulkan physical planで具体化する。
3. complete physical planとNativeScopeをdata-only boundary + verifierとして追加する。
4. 実用CPU/GPU二候補taskを得た時点でCPU sibling lowererとexecution linkerを実証する。
