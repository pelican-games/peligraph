# WP209a 完了記録 — static texture dimension / material sampler

日付: 2026-07-26

## 結論

`pelican.surface v1` から project-owned KTX2 の 2D/cube/2D-array/3D と material
sampler policy を宣言し、generated accessor、SPIR-V reflection、Vulkan image/view、
descriptor bindingまで一つのtyped contractとして運べる。

この追加はcurrent v1のadditive fieldである。versionを増やしておらず、旧構文を受理する
互換分岐もない。省略時だけ従来の2D/linear/repeat/no-compare/anisotropy 1になる。

## 公開面

```glsl
//! textures:
//!   - { name: environment, default: "project://textures/environment.ktx2", color_space: linear, dimension: cube, sampler: { filter: linear, mip_filter: nearest, address: clamp_to_edge, anisotropy: 4, anisotropy_fallback: disable } }
```

- dimension: `2d | cube | 2d_array | 3d`
- filter / mip_filter: `nearest | linear`
- address: `repeat | mirrored_repeat | clamp_to_edge`
- compare: `none | never | less | equal | less_equal | greater | not_equal |
  greater_equal | always`
- anisotropy: 1以上の有限数
- anisotropy_fallback: `disable | reject`

shaderはbinding番号やVulkan view typeを記述せず、
`pelican_sample_<name>(coordinates)`を使う。coordinateは2Dだけ`vec2`、それ以外は
`vec3`。comparison samplerは戻り値が`float`になり、`reference`引数を追加する。

## loweringとruntime

1. surface parserがdimension/samplerをtyped valueへ変換する。
2. material loweringが宣言をruntime bindingへ保持し、anisotropy capabilityを
   exact/clamp/disable/rejectへ決定的に解決する。
3. KTX2 loaderがdimension、array layer/face、mipごとのwidth/height/depthと全payloadを保持する。
4. texture registrationが2D/cube/2D-array/3D imageと対応viewを作り、cube-compatible flag、
   3D image type、mip/layer/depth copy regionを選ぶ。
5. generated GLSLとreflectionがsampler objectのdimensionを照合し、material登録時に
   declarationとloaded image dimensionを照合する。
6. texture/material reloadもdimension、depth、layer、samplerをtransaction contractへ含める。

sampler objectは解決済みpolicyをkeyにcacheする。feature未使用materialにはcustom samplerを
追加せず、既存fullscreen/compute resource-port samplerには変更を加えない。

## rejection / fallback

- surface宣言とloaded texture dimensionの不一致はtexture名付きhard error。
- non-2D宣言は2D semantic dummyへ暗黙fallbackしない。
- KTX2 cubemapは6面かつ正方形必須。
- cube arrayと3D arrayは現公開集合外としてhard error。
- 3D comparisonはschemaでhard error。
- comparisonをcolor formatへbindするとhard error。
- anisotropy feature不在時は`disable`なら理由を残して無効化し、`reject`ならhard error。
- device上限未満へ下げる場合はclamp理由を残す。

## dogfood / 検証

- KTX2 2D/cube/2D-array/3D positive fixtureとshape negative fixture。
- dimension別generated GLSLとSPIR-V reflection。
- comparison shadow accessorのcompile。
- sampler exact/clamp/disable/rejectのpure CPU test。
- headless Vulkanでcubemap六面をuploadし、`samplerCube`から+X面の赤をreadback。
- 同scenarioで2D textureをcube宣言へbindする失敗とanisotropy resolutionを検証。
- material/texture reload、material binding、shader reflection、Vulkan headless、
  atlas/debug textの隣接テストを回帰。

## 意図的な後続

現行KTX2公開formatはcolor textureである。comparison authoringとshadow sampler生成は完成したが、
hardware depth compareのpositive runtime dogfoodにはcompatible depth-format image providerが
必要であり、public shadow resourceまたはWP209bのRT subresource viewへ接続する。

runtime targetのmip/layer/subresource view、cube array、3D array、runtime IBL/depth pyramidは
WP209aへ混ぜず後続へ分離した。
