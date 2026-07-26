# WP209b RT mip/layer/subresource view 完了レビュー

日付: 2026-07-26

## 結論

2D runtime render targetについて、authored mip/layer contractとshader portの
subresource rangeをlogical configからVulkan image viewまで一貫して運べる。
最初の実利用として、array layer 1上のmulti-mip depth pyramidをcomputeで生成し、
fullscreenで表示するGPU経路を通した。

XRの2D-array実装はschemaへ露出していない。`layers`はresourceの論理的な容量であり、
physical loweringがXR view familyを必要とする場合は
`max(authored layers, required view count)`へ拡張する。ユーザー宣言を縮めるloweringは
runtime verifierが拒否する。容量が論理view数より大きい場合も、`per_view` descriptorは
先頭の論理view数だけを切り出す。明示rangeなら`layer_count`を論理view数と一致させ、
`layer`で別の連続view bankを選べる。

## 公開authoring

render target:

```json
{
  "name": "depth_pyramid",
  "extent_scale": 1.0,
  "format": "R16G16B16A16_SFLOAT",
  "usage": ["COLOR_ATTACHMENT", "SAMPLED", "STORAGE"],
  "mip_levels": "full",
  "layers": 2
}
```

`mip_levels`は正整数または`"full"`、`layers`は正整数である。省略時はどちらも1。
full chainの実数はextentごとに`floor(log2(max(width,height))) + 1`で解決する。

fullscreen/compute resource port:

```json
"source_depth": {
  "resource": "depth_pyramid",
  "access": "sampled",
  "subresource": {
    "mip": 0,
    "mip_count": 1,
    "layer": 1,
    "layer_count": 1
  }
}
```

各fieldの省略値は`mip=0`、`mip_count=1`、`layer=0`、`layer_count=1`。
明示rangeを持つcompute portは、同じlogical imageを複数bindingへ割り当てられる。
storageを含むrange同士が重なる構成は拒否する。fullscreenはdescriptor bindingが
input順に固定されるため、1 input resourceにつき1 portに限定する。

## loweringとownership

1. parserが`RenderTargetDefinition`と`ShaderResourcePortDefinition`へtyped値を作る。
2. `ResourcePatternBinding`からdisposable `TargetLoweringGraph`へmip/layer contractを写す。
3. Vulkan physical planがrepresentation、extent、format、sample count、view layoutと同列に
   mip/layerを決定する。
4. runtime verifierがdevice limit、alias member、authored target contract、fragment
   fingerprintを照合する。
5. `RenderTargetContainer`がimageとsubresource view cacheをgeneration単位で所有し、
   compute/fullscreen descriptorはrecreate/reload時にexact viewを再取得する。

MSAA targetではraster attachmentをsingle-mipの別imageとし、sample/storage対象の
resolve imageだけがauthored mip chainを持つ。history clearとwhole-image layout transitionは
全mip/layerを対象にする。

## GPU dogfood

headless testは32x32、full chain、2 layersのRTを作る。

1. raster初期化はbase layer 0へ出力する。
2. compute seedがlayer 1 / mip 0をstorage writeする。
3. compute reduceが同じimageのlayer 1 / mip 0をsampleし、layer 1 / mip 1へstorage writeする。
4. fullscreenがlayer 1 / mip 1をsampleし、緑画素をswapchainへ出力する。
5. 64x32へrecreateして6 mipから7 mipへ増えること、全descriptor revisionが更新されることを
   確認する。
6. reduce shaderをhot reloadし、新generationのexact viewで同じGPU結果になることを確認する。

## 検証結果

- render target/sample count: 179 assertions / 18 cases
- frame planner: 189 assertions / 23 cases
- physical target planning: 238 assertions / 27 cases
- rendering pass helper/parser: 372 assertions / 56 cases
- sequential/multiview runtime: 60 assertions / 1 case
- headless Vulkan depth-pyramid/resize/hot-reload: 46 assertions / 1 case
- `git diff --check`: clean

## 明示的に残した境界

- material/geometry resource portはまだbase target viewだけをbindするため、
  `subresource`を受理せず明示エラーにする。
- raster attachmentを任意mip/layerへ向けるpass authoringは未実装。compute storage writeが
  今回のpyramid生成経路である。
- runtime targetは2Dのみ。3D/cube target、sparse image/residency、video imageは別contract。
- hazard/layout trackerはresource単位で、subresource単位の並列実行最適化は行わない。
  correctnessを保つためwhole imageを保守的に遷移する。

この境界により、未実装のdescriptor/attachment意味を設定だけ受理して黙ってbase viewへ
落とすことを避けつつ、次のWPがmaterial、raster attachment、3D/cubeのどれを必要とするかに
応じて独立に拡張できる。
