# シェーダ契約(v1 / WP70 実装)

対象: プロジェクト/feature/マテリアル用シェーダを書く人。ここでは
`src/core/shader/pelican_sets.hpp`、`src/core/resources/shaders/include/pelican_sets.glsl`、
`PipelineFactory`、既存シェーダ、各 renderer/container の現行実装から読める契約だけを記録する。

## Descriptor sets

| set | 名前 | 現行の使い方 |
|-----|------|--------------|
| 0 | `PELICAN_SET_FRAME` | 全 pipeline 共通の固定 layout。binding 0 `FrameUBO`、1 `ObjectBuffer` SSBO、2 `LightUBO`、3 `PreviousObjectBuffer` SSBO、4 `FrameResolutionUBO`。エンジン管理・読み取り専用。 |
| 1 | `PELICAN_SET_PASS_INPUT` | fullscreen / UI / compute / material pass の入力。通常の fullscreen/compute/material resource は logical name から generated binding へ解決する。UI texture と raw escape hatch もこの set を使う。 |
| 2 | `PELICAN_SET_MATERIAL` | 標準 material texture。binding 0 `baseColorSampler`、1 `metallicRoughnessSampler`、2 `normalSampler`、3 `emissiveSampler`、7 `occlusionSampler`。VAT 有効時は 4 `vatPositionSampler`、5 `vatNormalSampler`。binding 6 は全マテリアルを並べた `MaterialBuffer` SSBO。 |
| 3 | `PELICAN_SET_FREE` | variant ごとの補助枠。debug draw/text は binding 0 の SSBO、debug text fragment は binding 1 の atlas texture、`PELICAN_SKINNED` は binding 2 の `SkinPalette` SSBO を使う。binding 2 はスキン variant だけエンジン所有。 |

`LightUBO` の shadow 行列列の後ろには `environmentAmbientRadiance` と
`environmentSkyRadiance` の `vec4` がある。`sky_ambient` feature の runtime parameter
`color * ambient_intensity` / `color * sky_intensity` を linear-sRGB radiance として供給する。
feature が無いときは両方 zero である。非ゼロ既定値は feature JSON だけが所有し、
LightUBO の初期化や shader に別の fallback は置かない。

機械可読な正本は [`material_resources_manifest.json`](material_resources_manifest.json) に置く。
`.surface` の custom texture は宣言順に set 2 binding 8 から割り当て、`role: color`
(または `color_space: srgb`) は SRGB view、`role: data` は UNORM view を使う。
未指定 resource は white / flat-normal / black の semantic dummy に解決され、いずれも
SRGB/UNORM の両 view を持つ。binding 6 の `MaterialBuffer` は既存 96 byte の標準 field に
続けて 256 byte の custom value payload を持つ。payload 内の member offset は `.surface`
params の宣言順で計算した std140 layout が正であり、JSON object の列挙順には依存しない。

### static texture dimension / material sampler（WP209a）

current `pelican.surface v1` の custom texture は dimension と sampler policy を宣言できる。
これは現行版への additive field であり、別版や旧構文の受理分岐はない。`sampler` は必ず
inline mapping で記述する。

```glsl
//! textures:
//!   - { name: environment, default: "project://textures/environment.ktx2", color_space: linear, dimension: cube, sampler: { filter: linear, mip_filter: linear, address: clamp_to_edge, compare: none, anisotropy: 4, anisotropy_fallback: disable } }
```

| `dimension` | generated object | accessor |
|---|---|---|
| `2d`（既定） | `sampler2D` | `vec4 pelican_sample_<name>(vec2 coordinates)` |
| `cube` | `samplerCube` | `vec4 pelican_sample_<name>(vec3 direction)` |
| `2d_array` | `sampler2DArray` | `vec4 pelican_sample_<name>(vec3 uv_layer)` |
| `3d` | `sampler3D` | `vec4 pelican_sample_<name>(vec3 coordinates)` |

`sampler` の field は `filter: nearest|linear`、`mip_filter: nearest|linear`、
`address: repeat|mirrored_repeat|clamp_to_edge`、
`compare: none|never|less|equal|less_equal|greater|not_equal|greater_equal|always`、
`anisotropy: 1以上の有限数`、`anisotropy_fallback: disable|reject`。
省略時は linear / linear / repeat / none / 1 / disable であり、既存2D materialの
descriptorや画素を変えない。

compareを有効にすると戻り値は`float`になり、accessor末尾へ`float reference`を足した
shadow samplerになる。3D comparisonはschemaで拒否する。anisotropyは利用可能ならdevice
limitへclampし、利用不能ならfallback policyに従ってdisableまたは名前付きrejectする。

KTX2 loader、generated GLSL、SPIR-V reflection、Vulkan image/view、material bindingは
同じdimensionを照合し、不一致をtexture名付きで拒否する。non-2D宣言には一致する実textureが
必要で、2D semantic dummyへ暗黙fallbackしない。現行のstatic KTX2公開集合は
2D/cube/2D-array/3Dであり、cube arrayと3D arrayは対象外。現行KTX2 formatはcolorなので、
hardware depth compareを実行するにはcompatible depth-format imageを供給する後続経路が要る。

set 0 layout は reflection の有無にかかわらず `PipelineFactory` が全 pipeline に挿入する。
reflection が set 0 を宣言する場合は上記 5 binding の型・個数と一致しなければ pipeline 作成を拒否する。
set 1 以降は従来どおり reflection から生成し、高い set だけを使う場合も途中に空 layout を置く。

### 名前付き fullscreen / compute resource port(WP207a)

fullscreen pass の `input`、compute task の `reads` / `writes` は frame graph の
依存関係を定義する。任意の `resource_ports` は、その既存 resource を shader でどう読むかを
名前付きで注釈する。port は edge、producer、lifetime、実行順を追加しない。

```json
"resource_ports": {
  "source": {
    "resource": "scene_color",
    "access": "sampled",
    "view": "shared_2d",
    "sampling": {"filter": "linear", "address": "clamp_to_edge"}
  },
  "destination": {
    "resource": "filtered_color",
    "access": "storage"
  }
}
```

port 名は GLSL identifier であり、shader は
`#include "pelican_resource_ports.glsl"` を宣言する。generated interface は
sampled port に `pelican_sample_<port>()` / `pelican_size_<port>()`、storage port に
`pelican_load_<port>()` / `pelican_store_<port>()` / `pelican_size_<port>()` を作る。
全image portはさらに`pelican_base_mip_<port>()` /
`pelican_base_layer_<port>()`を持ち、graphが選んだ絶対base subresourceを返す。
実 descriptor 変数は generated include の内部詳細であり、shader は set/binding を書かない。

`view: "shared_2d"` は全 view 共通の `sampler2D` / `image2D` である。
`view: "per_view"` は sequential graphics / `schedule: "per_view"` computeでは当該 eye の
2D view、multiview graphicsと`per_frame` computeでは2D arrayになる。per-view computeの
物理targetが1枚再利用なら同じ2D descriptorを各実行で使い、複数layerなら眼別descriptorを
bindする。2Dにもindexed accessor overloadと`pelican_view_count_<port>() == 1`を生成するため、
同じshader sourceをarray loweringと共有できる。logical viewとphysical target planが一致しない
構成はpipeline登録前に拒否する。

`pelican_frame.glsl`は`pelican_view_index()`と`pelican_view_count()`も公開する。
sequential / computeではFrameUBOの値、multiview graphicsでは`gl_ViewIndex`を返す。

reflection は port の set、binding、descriptor kind、count、生成変数名、2D/2D-array/cube次元を
照合する。hot reload も同じ interface を保存して再検証する。`resource_ports` を持たない
既存 shader の raw set 1 ABI は不変であり、buffer や特殊 descriptor の escape hatch として
利用できる。

### runtime RT subresource port(WP209b)

2D runtime render targetは`mip_levels: <positive integer>|"full"`と
`layers: <positive integer>`を持てる。fullscreen/compute/materialのsampled image portは
optional `subresource`でview rangeを選ぶ。

```json
"subresource": {
  "mip": 2,
  "mip_count": "remaining",
  "layer": 1,
  "layer_count": 1
}
```

省略値は`mip=0`、`mip_count=1`、`layer=0`、`layer_count=1`。`mip_count`は正整数または
選択base mipから現在のtarget最終mipまでを表す`"remaining"`を受理する。後者はtarget
resize/recreate時に新しいmip数へ再解決される。明示viewでは
shaderから見たLOD 0が`mip`で選んだbase mipに対応し、`pelican_size_<port>()`も
そのviewのサイズを返す。`pelican_base_mip_<port>()`と
`pelican_base_layer_<port>()`は正規化済みrangeの絶対baseを返すため、同じshaderが
task名やbackend viewを見ずにmip/layer別のalgorithmを選べる。base layerはrangeの起点であり、
現在のfamily memberは`pelican_view_index()`で別に取得する。`shared_2d`の2D viewはlayer count 1、storage imageは
mip count 1が必要である。`per_view`がphysical layered viewへloweringされた場合だけ
2D-array accessorになる。physical imageが論理view数より多いlayer容量を持っても、
layered `per_view` descriptorは論理view数ぶんだけを公開する。明示rangeは1 layerから
logical view数へ展開するか、logical view数と同じ連続rangeを指定する。1枚再利用の
`sequential_2d`は`layer_count: 1`のままでよい。

computeは同じlogical imageを複数portへ割り当てられるが、各portに明示range/accessが必要で、
storageを含むoverlapは拒否する。fullscreenはinput順がdescriptor bindingのauthorityなので、
1 input resourceにつき1 portである。material/geometryのsampled portも同じrangeをbindし、
generated `pelican_sample_lod_*` / `pelican_mip_count_*`で参照できる。materialのsubresource
portはsampler descriptor専用で、`same_pixel` local read/input attachmentへはlowerしない。

layout/hazard trackingは現時点ではresource単位である。互いに素なrangeでもwhole imageを
保守的に遷移し、subresource並列化は行わない。

### runtime cube render target port(WP236)

runtime render targetは資源形状を`dimension: "2d"|"cube"`で宣言する。cubeは固定の
正方形extentと6 layersを必要とし、`layers`省略時は6になる。これはViewFamilyの実行形状とは
別の型であり、六面を自動的に6-view scheduleへ変換しない。

```json
{
  "name": "environment_capture",
  "dimension": "cube",
  "width": 256,
  "height": 256,
  "extent_scale": 1.0,
  "format": "R16G16B16A16_SFLOAT",
  "mip_levels": "full",
  "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
}
```

raster出力は既存の`subresource.layer`でface 0〜5を2D attachmentとして選ぶ。
fullscreen/compute/materialのsampled portは`view: "cube"`で同じimageの全6 faceを選ぶ。

```json
"resource_ports": {
  "environment": {
    "resource": "environment_capture",
    "access": "sampled",
    "view": "cube",
    "subresource": {
      "mip": 0,
      "mip_count": "remaining",
      "layer": 0,
      "layer_count": 6
    }
  }
}
```

generated objectは`samplerCube`で、
`pelican_sample_<port>(vec3 direction)` /
`pelican_sample_lod_<port>(vec3 direction, float lod)`を公開する。resource shape、
descriptor reflection、runtime image viewを同じcube dimensionで照合する。cube storage、
same-pixel/input attachment、cube array、runtime 3Dは現契約外で、明示設定をrejectする。

feature compositionが生成する値付きdefineはgraphicsだけでなくcompute shaderの
compile/cache recipeにも同じ順序で入る。project-owned compute algorithmも
`PELICAN_FEATURE_<FEATURE>_<PARAM>=<value>`を直接利用でき、pipeline recordはdefine集合を
保持する。

### material/geometry resource port(WP207b / WP220)

`.surface` は shader-facing port を `resource_ports` で宣言する。current
`pelican.surface v1` への additive field であり、別版や旧版互換分岐はない。

```glsl
//! resource_ports:
//!   - { name: displacement, kind: buffer, element: vec4, stage: vertex }
//!   - { name: simulation_color, kind: image, stage: fragment }

void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {
    vertex.position += pelican_load_displacement(0u).xyz;
}
```

`kind: buffer` は `element` 必須の readonly std430 array で、
`pelican_load_<port>(uint)` / `pelican_count_<port>()` を生成する。element は
`float|vec2|vec3|vec4`、対応する signed/unsigned vector、`mat4`。
`kind: image` は同じ公開関数のsampled/input-attachment両variantを持ち、
`pelican_sample_<port>(vec2)` / `pelican_size_<port>()` を生成する。
`stage` は `vertex|fragment|vertex_fragment`、省略時 fragment。reflection は
descriptor kind/nameだけでなく実 stage visibilityも一致させる。

material pass は同名 port を graph resource へ割り当てる。

```json
"material_resources": {
  "displacement": {
    "resource": "simulation_positions",
    "access": "storage",
    "footprint": "arbitrary"
  },
  "simulation_color": {
    "resource": "simulation_color",
    "access": "sampled",
    "view": "shared_2d",
    "sampling": {"filter": "linear", "address": "clamp_to_edge"},
    "footprint": "same_pixel"
  }
}
```

この map が producer/read edge、footprint、current/history、physical view、sampler policy の
authorityであり、surface は特定の graph resource 名を持たない。buffer は history/per-view/
sampling不可。image history は `<name>@history` で指定する。

imageは通常set 1のcombined image samplerである。fragment-only portが
`footprint: "same_pixel"`で、physical plannerが同一rendering scopeへ融合した場合は、
同じbinding位置のdescriptorをinput attachmentへ変え、generated functionを
`subpassLoad()`へloweringする。descriptor bindingとinput attachment indexは独立しており、
reflectionは両方を照合する。local variantは意味論上現在画素だけを読むため、
関数のUV/LOD引数を使わない。history、material subresource、MSAAはlocal化せず、
条件不成立時はsamplerへfallbackする。

shared 2Dとsequential per-view 2Dは両variantで利用できる。pass mappingが
`view: "family_array"`を宣言したsampled portは`sampler2DArray`へloweringする。
surface側の`pelican_sample_<port>(vec2)`と`pelican_sample_lod_<port>(vec2, float)`は
`pelican_view_index()`で現在のfamily memberを選び、view index付きoverloadは任意layerを
明示できる。surface宣言へ物理view型は追加せず、semantic interfaceとsampling algorithmを
pass/backend loweringから分離する。1-view familyがscalar 2D imageへ物理化された場合は
runtimeが1-layer 2D-array viewを生成し、複数material passで一つのshader ABIを共有する。

consumer-owned layered multiviewの一般sampled inputは未対応で、input attachment variantだけ
2D-array attachment viewをbindできる。generated image accessorは現在floating-point
`vec4`契約であり、UINT/SINT input attachmentはtyped image portを追加するまで拒否する。

一つのmaterial shader/pipelineを共有するactive graph variantは、各portの
sampler/input-attachment種別、input attachment index、正規化後のshader image-view ABIが
一致する必要がある。
graph-only hot reloadでこの物理契約が変わる場合も、WP222のcoordinated transactionが
保持済みsurface compiler recipeからshaderを再生成し、依存pipelineとmaterial metadataを
候補generationへ合わせる。全candidateの構築完了後だけgraphと同時公開し、
compile failureまたはstale publicationでは全て旧世代を維持する。

runtime compile は buffer を active generation の `FrameGraphBufferId` へ固定し、
target recreate/pipeline reloadではdescriptorを新しいimage view/IDへ再生成する。
typed portを使わないmaterialでは追加のstorage descriptor pool/samplerを生成しない。
詳細と制限は
[`design_reviews/2026-07-26_wp207b_material_resource_ports.md`](design_reviews/2026-07-26_wp207b_material_resource_ports.md)
および
[`design_reviews/2026-07-29_wp220_material_local_read_report.md`](design_reviews/2026-07-29_wp220_material_local_read_report.md)
を参照する。

### 名前付きスクリーンスナップショット(M3.5 / WP83)

`.surface` の `screen_inputs` は rendering config の `snapshots` で先に定義した名前だけを
参照する。宣言順 `i` の入力はset 1 binding `i`を使い、生成accessor
`vec4 pelican_screen_<name>(vec2 uv)`で読む。通常はcombined image samplerだが、
same-pixel contractがphysical tile-localへ解決された場合は、同じpublic accessorを
input attachmentへloweringする。入力名はshader identifierでなければならず、
未定義名はmaterial名・snapshot名を含む起動時エラーになる。

v1 の snapshot は `display` を opaque 後の指定 copy point で一度だけ同 format・同 extent の
sampled image へコピーする固定内容である。許可する copy point は canonical `post_ldr` 領域の
1点だけで、2点目、`pelican_ui` 以後、透明描画後の snapshot はエラーとする。同じ snapshot を
読む透明 material 同士は互いの結果を見ず、逐次屈折は非対応である。frame plan は
`snapshot_copy` ノードの `byte_size` と、消費 pass の通常の read/barrier だけを公開する。
マテリアル横断の集約 dump は公開契約に含めない。

`FrameUBO` (`pelican_frame.glsl`) は `time` / `dt` / 64-bit `frame_index`
(low/high 32-bit) / `resolution` とその逆数 / `camera_position` / `view` / `projection`
を持つ。VAT を含む時間依存 shader は push constant でなくこの値を読む。

## WP88 temporal additions

Set 0 adds binding 3, `PreviousObjectBuffer` (readonly SSBO), alongside binding 1
`ObjectBuffer`. Both use the same object index. `FrameUBO` appends `previous_view` and
`previous_projection`, preserving all previous offsets. New objects start with previous
model equal to current, so their first velocity is zero.

The standard `velocity` feature writes `R16G16_SFLOAT` UV-space velocity. Fullscreen
pass inputs use set 1 in JSON declaration order; an `@history` target binds the previous
physical image while an unqualified target binds the current image.

## Push constants

`PELICAN_PUSH_ENGINE_BYTES = 64`、`PELICAN_PUSH_SHADER_BYTES = 64`、
`PELICAN_PUSH_TOTAL_BYTES = 128` が C++ と GLSL include に定義されている。

- offset 0..63 は engine 領域で、宣言する場合は 64B の `mat4` MVP 全体だけを宣言する。
- offset 64..127 は shader 領域。material index と UI draw data はここに置く。
- reflection は stage ごとの block を論理的に 64B 境界で検査し、4B alignment、128B 上限、
  engine 領域の部分使用を pipeline 作成前に拒否する。Vulkan の同一 stage range 重複禁止に従い、
  pipeline layout では stage ごとに宣言範囲の envelope を 1 つだけ作る。同じ envelope の stage は
  1 range にまとめる。
- fullscreen の camera/time/resolution と lighting data は set 0 へ移動済み。
  既存 pass JSON の `push_constants: camera_position|projection_view` と `uses_light_data` は
  形式互換のため受理するが、GPU 供給元は常に FrameUBO/LightUBO である。

### `engineMvp` v1 の規範意味論(WP175)

`pelican_surface_v1` を含む現行 v1 ABI では、push constant の offset 0..63 にある
`engineMvp` は歴史的な名前であり、model 行列を含む MVP ではない。CPU が供給する値は
`RenderFrameSnapshot::view_projection_jittered`、すなわち
`applyProjectionJitter(projection_non_jittered, jitter_ndc) * view` である。
入力空間と出力空間は **world → jittered clip** とする。

vertex shader は skin/morph/vertex hook の後に model 行列を適用して world position を作り、
その world position に `engineMvp` を一度だけ掛ける。jitter は一般式
`clip'.xy = clip.xy + jitter_ndc * clip.w` により projection へ適用済みであり、shader が
`engineMvp` の後へ追加してはならない。FrameUBO の `projection` と
`previous_projection` も同じ snapshot の current/previous jittered projection である。

この意味論、64 byte の範囲、symbol 名は v1 では凍結する。`engineMvp` を
`viewProjectionJittered` 等へ直す場合は、既存 symbol の再解釈や in-place rename ではなく、
次期 shader ABI の新 symbol として導入する。

## Vertex input

`GraphicsPipelineDesc::use_engine_vertex_layout = true` の pipeline は
`VertBufContainer::getDescription()` の固定 layout を使う。binding は 0、rate は vertex。

| location | GLSL 名の慣例 | format | C++ source field |
|----------|---------------|--------|------------------|
| 0 | `inPos` | `R32G32B32_SFLOAT` | `CommonVertStruct::pos` |
| 1 | `inNormal` | `R32G32B32_SFLOAT` | `CommonVertStruct::normal` |
| 2 | `inTexUV` | `R32G32_SFLOAT` | `CommonVertStruct::texcoord` |
| 3 | `inColor` | `R32G32B32A32_SFLOAT` | `CommonVertStruct::color` |
| 4 | `inTangent` | `R32G32B32A32_SFLOAT` | `CommonVertStruct::tangent` |

fullscreen / UI / debug draw / debug text のように `gl_VertexIndex` や SSBO から頂点を生成する
pipeline は、この vertex input layout を使わない。

`GraphicsPipelineDesc::use_skinned_vertex_layout = true` は別の専用 vertex buffer を使う。
location 0〜4 は上表と同じ意味・format のまま、location 5 に `JOINTS_0`
(`R16G16B16A16_SINT`)、location 6 に `WEIGHTS_0` (`R32G32B32A32_SFLOAT`) を追加する。
非スキン pipeline はこの buffer/layout を参照しない。スキン variant の頂点シェーダは
set 3 binding 2 の std430 `mat4` palette を `gl_BaseInstance * 128 + joint` で参照する。

## Defines

現行の feature define 合成は `src/project/featurecompose.cpp` が行う。

1. rendering config 直下の `shader_defines`
2. `features` 配列の順に読み込まれた render feature の `shader_defines`

この順で重複を除き、`RenderingPassConfigRuntimeDependencies::shader_defines` として runtime compile に渡す。
`ShaderLibrary::loadFromReference` は GLSL compile 時にこれを macro definition として渡す。
`.spv` 参照に defines が付いた場合は reject される。

material defines は WP58/M1 時点では parser の結果に保持されるだけで、renderer/pipeline にはまだ合流しない。
M2 以降で合流する場合は、feature 由来 defines の後ろに material 由来 defines を追加するのが
`design_material_shading.md` の契約である。

## SPIR-V / Vulkan ターゲット環境

ターゲット版の宣言箇所は
[`cmake/pelican_target_environment.cmake`](../cmake/pelican_target_environment.cmake) だけである。
Vulkan instance、VMA、ImGui、埋め込み用 glslang、runtime shaderc、SPIRV-Tools、Slang fixture、
外部 validator はすべてこの契約から値を受け取る。変更時はこのファイルだけを編集し、
未対応の組合せと、shader target が Vulkan API target を超える組合せは configure 時に拒否する。

現行契約は Vulkan API 1.3.283 と、shader target `vulkan1.2` / SPIR-V 1.5 である。
renderer は core dynamic rendering を使うため Vulkan API 1.3 を維持する一方、SPIR-V は
既知の 1.4 以上の要件を満たす既存 runtime target 1.5 に留め、理由なく 1.6 へ上げない。
この変更で runtime compiler の出力版と Vulkan API の宣言値は変わらない。
埋め込み SPIR-V は 1.0 から 1.5 へ上がるため、`PELICAN_RUNTIME_SHADER_COMPILER=OFF` は
SPIR-V 1.5 を直接消費する。これは engine の Vulkan 1.3 最低要件の範囲内である。
OFF の CTest はこの埋め込み版と OFF 対応済みの実行面を検証する。`dist-bake` 未実装のため
source stem / feature define の事前焼き出しを要するテストだけは ON 専用として登録する。

## ランタイム SPIR-V ディスクキャッシュ (WP82)

runtime shaderc の出力はプロジェクトローカルの
`<project>/.pelican/shader_cache/` に保存する。キャッシュキー v1 は次の次元を
長さ付きで直列化し、その全体を SHA-256 にした値である。

1. root source と、実際の include 解決規則で到達する全 file / virtual / engine include の
   名前および bytes から作る source graph SHA-256。macro 展開された include も取りこぼさないよう、
   include root 配下・virtual source・埋込み engine shader source の全候補も保守的に含める
2. shaderc に渡す全 define（値と順序を含む。source 内で未使用でも含む）
3. shaderc toolchain version と、runtime が報告する SPIR-V version / revision
4. 上記の一元化された target environment、shader stage、entry point
5. engine shader contract salt (`pelican-shader-contract-v1-wp82-20260712`)

この列挙の変更・shader ABI の変更・compile option の追加は cache key version または
contract salt の更新を伴う。entry は format version、key、payload SHA-256、word count を持ち、
完全検証後だけ hit とする。破損・未知形式・read/write 不可は shaderc 再コンパイルへ
フォールバックし、起動を失敗させない。書込みは同一 directory の一時 file から rename する。
キャッシュはコンパイル済み bytes を変換しないため、hit/miss は同じ SPIR-V bytes を返す。
非 literal（macro operand）の include は依存先を完全に証明できないため cache を使わず、
stale hit を許す代わりに通常 compile へフォールバックする。

## B 層 source ABI (M3a)

B 層の**既定経路**は GLSL ソース専用で、エンジン所有の
`engine://shaders/material/surface_v1.vert` / `surface_v1.frag` へ仮想 include として
逆 include される。ユーザー文書自体は変更せず、`#line` が code 本体の元のファイル名・行番号を
コンパイラへ渡す。この source include fallback は M3b 導入後も production baseline として
常設し、`PELICAN_SPV_LINK` 未指定時の挙動である。

版付き v1 hook とシグネチャは次のとおり。`PelicanVertexV1`、`PelicanSurfaceInputV1`、
`PelicanSurfaceV1`、`PelicanLightV1` の field・型・意味を含めて凍結する。field 追加も行わず、
拡張時は v2 型・v2 symbol と v1 adapter を新設する。

```glsl
void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex);
void pelican_surface_v1(in PelicanSurfaceInputV1 surface_input,
                        inout PelicanSurfaceV1 surface);
vec3 pelican_brdf_v1(in PelicanSurfaceV1 surface, in vec3 light_dir,
                     in vec3 view_dir, in vec3 radiance);
vec3 pelican_ambient_v1(in PelicanSurfaceV1 surface, in vec3 view_dir,
                        in vec3 radiance);
vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface,
                         in PelicanSurfaceInputV1 surface_input);
```

`pelican_vertex_displace_v1` と `pelican_surface_v1` は直交して共存できる。
`pelican_brdf_v1` と `pelican_lighting_v1` は terminal hook なので排他である。
未知の `pelican_` 定義、terminal hook 併存、空または既知 hook ゼロの snippet は、
surface 名を含むロードエラーになる。

material passが`pelican.material_outputs` v1を宣言した場合だけ、strategy-privateな
追加hookを生成する。

```glsl
void pelican_material_outputs_v1(
    in PelicanSurfaceInputV1 surface_input,
    in PelicanSurfaceV1 surface,
    inout PelicanMaterialOutputsV1 outputs);
```

`PelicanMaterialOutputsV1`のfield列はbase ABIの固定structではなく、pass schemaから
materialごとに生成される。field名・GLSL型・順序はschemaがauthorityであり、fragment
locationと`output.color`順へ同時にlowerする。hookを書かない場合もbuiltin sourceと
型付きzero defaultで全fieldを初期化する。hookを書けるのは対応schemaを持つpassへ
compileするときだけで、schemaなしのlegacy shaderでは未知hookとして拒否する。
reflectionは全output locationのnumeric typeをschemaへ照合する。

公開 source library は `engine://shaders/include/pelican_surface_v1.glsl` と
`engine://shaders/include/pelican_lighting_v1.glsl` である。後者は
`pelican_light_count()`、`pelican_light(i, world_position)`、
`pelican_shadow(i, world_position)`、`pelican_env_ambient(normal)` を公開する。
`pelican_env_ambient` は現行の単色段階では `environmentAmbientRadiance.rgb` を返し、
feature 無効時は zero を返す。`normal` は将来の方向依存 IBL と ABI を共有するために残す。
同梱 standard/toon lighting はこの関数群だけを使う。params は
`pelican_param_<name>()`、texture は `pelican_sample_<name>(uv)` という生成 accessor で読む。

同じ vertex 合成物を main / `PELICAN_PASS_DEPTH` / `PELICAN_PASS_VELOCITY` で再コンパイルし、
vertex displacement と custom0/custom1 の経路を全 pass で維持する。B の lowering はこの
3 variant、公開 template/library、set 2 resource、render state を C-material 記述として出力する。
`PELICAN_SKINNED` も同じ template の variant であり、テンプレートが
JOINTS_0/WEIGHTS_0 と palette を適用してからユーザー hook を呼ぶ。したがって B 層の
`.surface` 本文はスキニング対応のために変更してはならない。

## B 層 experimental SPIR-V link ABI (M3b / WP80)

build時に`PELICAN_WITH_SPIRV_LINK=ON`、実行時に
`PELICAN_SPV_LINK=experimental`を明示したプロセスだけが同じ `.surface` API の
SPIR-V link backend を選ぶ。build unitは既定OFFで、OFF binaryがruntime選択を受けた場合は
名指しの診断を返す。その他の値・未指定は上記 source 経路であり、既定のshader source、
define、golden、binding は変えない。

linker は固定 revision の SPIRV-Headers / SPIRV-Tools / SPIRV-Reflect を使い、binary parser、
linker、optimizer、validator、reflection/remap API だけで処理する。SPIR-V text assembly の
生成・置換・再 assemble は行わない。hook と公開 library 関数の ABI は scalar、vec2〜4、
単純 struct（および Function storage のそれらへの pointer）だけを許可する。matrix、array、
resource handle、Block/BufferBlock 型は hook 名を含むエラーで拒否する。

experimental 経路の custom texture は split sampler を標準形とする。宣言順 `i` に対して
sampled image は set 2 binding `8 + 2*i`、sampler は `9 + 2*i` であり、CPU binding 表は
logical name、descriptor type、remap 前後の set/binding を持つ。既定 source 経路の combined
image sampler は `8 + i` である。

キャッシュキーには固定3依存の revision/実行版、target env、template/user compiler generator、
両入力 SHA-256、export/import symbol、material set/binding 規約、全 define/pass/stage/ABI salt を含める。
GLSL library corpus は uncalled hook を保持する `--keep-uncalled`、Slang corpus は hook の
`[noinline]` と `-O0` を規約とする。

## WP70 で解消済みの差分

- set 0 は全 pipeline で固定 layout となり、`ObjectBuffer` と `LightUBO` の binding 衝突を解消した。
- material data は個別 UBO でなく set 2 binding 6 の SSBO 配列になった。glTF PBR factor と
  VAT の静的 bounds/playback 値も同じ配列から material index で参照する。
- push constant は engine 64B / shader 64B に reflection 段階で分割・検証される。
