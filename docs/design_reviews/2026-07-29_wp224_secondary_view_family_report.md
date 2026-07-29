# WP224 named secondary ViewFamily 実装レポート

日付: 2026-07-29

## 1. 結果

render graphのpass/taskが`$main`以外のview providerを選び、そのfamilyの行列と
FrameUBOで実行できるproduction runtime縦切りを追加した。標準directional shadowは
`$shadow/directional` familyのstable view `$cascade/0`を使う。flatのmono cameraでも
XRのstereo cameraでもshadow scopeは一回だけ実行され、main viewの行列を誤って使わない。

この変更はCameraやLightContainerをRendererへ個別直結する分岐を増やさない。
logical relation、physical scope、runtime providerを同じfamily IDで結ぶ。

## 2. compiler relation

`view_family`を次のartifactへ同じ値で伝播する。

```text
PassDefinition / ComputeTaskDefinition
  -> FrameGraphNodeDefinition
  -> LogicalGraphNode
  -> FramePlanNode
  -> FrameGraphExecutionNode
  -> LogicalFrameNodeInvocation
```

既定値は`$main`で、JSON dumpでは既定値を省略する。標準
`directional_shadow` passだけ`$shadow/directional`を明示する。異なるfamilyのnodeは
同じphysical rendering scopeへfusionできない。scheduleはfamily IDごとのcardinalityを
受け取り、scopeに対応するfamily/view indexをinvocationへ固定する。

## 3. runtime providerとGPU state

`RenderViewFamilies`は必須の`$main`と0個以上のnamed familyを保持する。
Rendererはcompiled graphが要求するIDを解決し、callerが
`$shadow/directional`を渡していなければLightContainerの標準providerを補う。
callerが同じIDを明示した場合はそのproviderを優先する。

各familyは独立した次のstateを持つ。

- stable `view_id`に対応するtemporal matrix history
- current / previous non-jitteredおよびjittered snapshot
- FrameUniformData / FrameResolutionUniformData
- in-flight frameごとのsequential descriptor slot

main familyだけgraph variantのmono/stereo cardinalityとprojection jitter policyを使う。
secondary familyへmain familyのjitterを暗黙適用しない。main multiview UBOは従来どおり
main familyだけをarray化し、secondary familyは独立したscalar slotを使う。

node実行直前にinvocationの`view_family + view_index`でsnapshot、descriptor slot、
first-person flag、view-projectionを選ぶ。scheduleとcompiled nodeのfamilyが違う場合は
runtime errorにする。

## 4. directional shadow移行

LightContainerは従来の合成済みmatrixだけでなく、view、projection、
camera positionを分けた`DirectionalShadowView`を供給する。shadow material drawは
LightContainerを直接参照せず、選択中familyの`view_projection`をpush constantへ渡す。

standard lightingがshadow textureを読むときのLightUBOにも同じfamily-selected matrixを
書く。このためcallerが標準shadow familyを置換しても、shadow生成とshadow samplingの
座標系が分離しない。従来の`shadowViewProjection()`は互換用helperとして残した。

## 5. 現在の制約

production runtimeでsecondary familyに許しているのは1-view `single_view` scopeである。
これはdirectional shadow一枚の縦切りを閉じるための意図的な制約で、次は次の順で外す。

1. stable `$cascade/N`を持つ複数view family
2. family固有extentとshadow array-layer target
3. secondary sequential execution
4. cascade matrix/split light ABI
5. family view別draw culling
6. CSMのflat/XR実Vulkan golden

secondary multiview、point/spot cube face、planar reflection providerはこの後である。

## 6. 検証

以下をDebug構成で通した。

- `pelican_core` build
- view family: 30 assertions / 5 cases
- projection consumer inventory: 387 / 7
- XR view space: 83 / 6
- rendering-pass helpers: 497 / 68
- frame planner: 197 / 24
- target render planning: 244 / 28
- multiview executionとfamily別FrameUBO/LightUBO: 69 / 1
- temporal golden: 343 / 8
- renderer/framegraph trace: 166 / 2
- directional shadow image golden: 6 / 1

renderer trace fixtureにはnon-main nodeだけ`view_family`を追加した。同時に、既に
compute fixtureへ追加済みだった`build_dispatch` nodeが期待traceから漏れていたため、
canonical traceを実行結果から再生成して同期した。
