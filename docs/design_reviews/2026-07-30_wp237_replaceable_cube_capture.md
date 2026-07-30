# WP237: replaceable cube capture algorithm

日付: 2026-07-30
状態: 完了

## 結論

runtime cubemapを描くための六方向camera policyを、削除・差替え可能な標準
`render_algorithms` packageとして実装した。エンジン本体はcube画像のallocation/view、
generic ViewFamily、resource relation、sequential schedulingだけを提供する。
「+Xから-Zまで六回sceneを描く」という技法は標準`cube_capture` feature/providerが所有し、
graph compilerやVulkan backendへcube capture固有のpass enumや分岐を追加していない。

cube画像とsix-view familyは同一概念ではない。cube画像をsamplingするだけのprojectに
六つのcameraは不要である。一方、capture algorithmは六つのcameraを実際に必要とするため、
同algorithmが明示的にstable `$capture/cube` familyを供給し、各viewを対応layerへ実行する。

## 実装した契約

- 標準feature: `engine://features/cube_capture.json`
- 標準provider: `standard.cube_capture_v1`
- family ID: `$capture/cube`
- view ID / cube layer順:
  `$face/+x`、`$face/-x`、`$face/+y`、`$face/-y`、`$face/+z`、`$face/-z`
- camera: 共通position、90度・aspect 1のVulkan ZO perspective、
  `0 < near_distance < far_distance`
- output: square、6-layer、`R16G16B16A16_SFLOAT`の`cube_capture_color`
- capture path: Deferred geometry、SSAO、Deferred lighting、
  Forward opaque、opaque color/depth snapshot、Forward transparent
- clustered lighting併用時: `$capture/cube`専用の6-view selection buffer/taskを
  feature順序に依存せず追加

各passは既存の`material`、`fullscreen`、`snapshot_copy`だけで構成する。
Deferred/Forwardのbindingもcanonical passから継承するため、標準surfaceだけに閉じない。
caller-authored `$capture/cube` familyはproviderより常に優先される。

## 汎用機構側で修正した境界

secondary familyはphysical compile時には1-view templateだが、runtime providerは複数viewを
返せる。従来はfamily内部resourceがcompile時の1 layer扱いになり、fullscreen descriptorも
compile時view数だけ作られていた。このままでは2 view目以降がlayer 0を参照する。

WP237では次をcube固有条件なしで一般化した。

1. secondary familyが書く内部targetは`sequential_2d` layoutとしてruntime view layerを保持する。
2. secondary familyから別familyへ渡すtargetは従来どおり`family_2d_array`にする。
3. fullscreenのsequential descriptor variant数はcompile時view数でなく、
   選択input/output targetのlayer容量から導出する。
4. 一層しかないinputは全viewで共有できるが、複数layer inputは必要容量を満たさなければ拒否する。

これによりcube captureだけでなく、cascade、複数mirror、将来のproject providerも
同じruntime-cardinality経路を利用できる。

## 差替え・purge

`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF`では
`render_algorithms/cube_capture`のC++ source/objectとprovider登録がbuildから消える。
汎用feature schema、compiler、ViewFamily registry/resolver、scheduler、Vulkan backend、
cube resource/view機構は残る。OFF構成で標準featureを使うprojectは、callerから同じfamilyを
渡すか、host側でstable family ID providerを登録できる。

feature JSONは標準libraryの例兼schema consumerとしてengine resourceに残す。
したがって「標準camera policyをpurgeする」と「cube captureを記述不能にする」は別である。

## 意図的に含めないもの

- `cube_capture_color`は1 mip。未生成mipを公開しない。
- GGX等のBRDF-aware prefilter
- 複数probeの配置、更新頻度、優先度、parallax補正、materialへの自動割当
- point/spot shadow固有のcamera/light schema
- secondary multiview
- cube array、cube storage image、runtime 3D image

これらはgeneric cube resourceを膨らませず、別の交換可能algorithm/typed contractとして
実需要ごとに追加する。特にmain materialへの暗黙bindingは行わず、利用者が
`cube_capture_color`を`view: "cube"`のtyped resource portへ明示bindingする。

## 受け入れ

- feature compositionで6-layer cube output、全capture pass、stable family、
  parameter/defineを検証
- clustered lightingとのfeature順序を両方向で検証し、6-view selectionと
  Deferred/Forward bindingを確認
- standard package ONでprovider登録、OFFでprovider source/object/registry不在を確認
- pure view testで六方向、stable ID、projection、入力validationを確認
- target planner testでsecondary内部targetとcross-family targetのlayoutを確認
- fullscreen descriptor testでruntime layer容量の最終layerまでvariant生成を確認
- Vulkan実行testでcube-compatible 6-layer imageの全faceへsequential描画し、
  六回の実行とface別readbackを確認

## 次の候補

reflection probeとして次に閉じるなら、最初はfeature-ownedなBRDF-aware mip prefilterと
明示的な単一probe bindingを追加する。その後に複数probeのupdate budget/selectionを
scene/light typed dataへ一般化する。shadow用途を先に進める場合は、同じsix-face機構を
使いつつpoint lightのfar rangeとshadow sampling relationを別provider/featureとして実装する。
