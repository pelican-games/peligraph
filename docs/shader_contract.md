# シェーダ契約(v1 / WP70 実装)

対象: プロジェクト/feature/マテリアル用シェーダを書く人。ここでは
`src/core/shader/pelican_sets.hpp`、`src/core/resources/shaders/include/pelican_sets.glsl`、
`PipelineFactory`、既存シェーダ、各 renderer/container の現行実装から読める契約だけを記録する。

## Descriptor sets

| set | 名前 | 現行の使い方 |
|-----|------|--------------|
| 0 | `PELICAN_SET_FRAME` | 全 pipeline 共通の固定 layout。binding 0 `FrameUBO`、1 `ObjectBuffer` SSBO、2 `LightUBO`。エンジン管理・読み取り専用。 |
| 1 | `PELICAN_SET_PASS_INPUT` | fullscreen / UI / compute の入力。fullscreen は pass input の texture/buffer を binding 0 から順に割り当てる。UI texture もこの set を使う。 |
| 2 | `PELICAN_SET_MATERIAL` | 標準 material texture。binding 0 `baseColorSampler`、1 `metallicRoughnessSampler`、2 `normalSampler`、3 `emissiveSampler`。VAT 有効時は 4 `vatPositionSampler`、5 `vatNormalSampler`。binding 6 は全マテリアルを並べた `MaterialBuffer` SSBO。 |
| 3 | `PELICAN_SET_FREE` | debug/user/future 用の自由枠。現行 debug draw/text は binding 0 の SSBO、debug text fragment は binding 1 の atlas texture を使う。 |

機械可読な正本は [`material_resources_manifest.json`](material_resources_manifest.json) に置く。
`.surface` の custom texture は宣言順に set 2 binding 7 から割り当て、`role: color`
(または `color_space: srgb`) は SRGB view、`role: data` は UNORM view を使う。
未指定 resource は white / flat-normal / black の semantic dummy に解決され、いずれも
SRGB/UNORM の両 view を持つ。binding 6 の `MaterialBuffer` は既存 96 byte の標準 field に
続けて 256 byte の custom value payload を持つ。payload 内の member offset は `.surface`
params の宣言順で計算した std140 layout が正であり、JSON object の列挙順には依存しない。

set 0 layout は reflection の有無にかかわらず `PipelineFactory` が全 pipeline に挿入する。
reflection が set 0 を宣言する場合は上記 3 binding の型・個数と一致しなければ pipeline 作成を拒否する。
set 1 以降は従来どおり reflection から生成し、高い set だけを使う場合も途中に空 layout を置く。

`FrameUBO` (`pelican_frame.glsl`) は `time` / `dt` / 64-bit `frame_index`
(low/high 32-bit) / `resolution` とその逆数 / `camera_position` / `view` / `projection`
を持つ。VAT を含む時間依存 shader は push constant でなくこの値を読む。

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

## B 層 source ABI (M3a)

B 層 `.surface` は GLSL ソース専用で、エンジン所有の
`engine://shaders/material/surface_v1.vert` / `surface_v1.frag` へ仮想 include として
逆 include される。ユーザー文書自体は変更せず、`#line` が code 本体の元のファイル名・行番号を
コンパイラへ渡す。HLSL/Slang と `.spv` link は M3b 以降であり、この経路では受理しない。

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

公開 source library は `engine://shaders/include/pelican_surface_v1.glsl` と
`engine://shaders/include/pelican_lighting_v1.glsl` である。後者は
`pelican_light_count()`、`pelican_light(i, world_position)`、
`pelican_shadow(i, world_position)`、`pelican_env_ambient(normal)` を公開する。
同梱 standard/toon lighting はこの関数群だけを使う。params は
`pelican_param_<name>()`、texture は `pelican_sample_<name>(uv)` という生成 accessor で読む。

同じ vertex 合成物を main / `PELICAN_PASS_DEPTH` / `PELICAN_PASS_VELOCITY` で再コンパイルし、
vertex displacement と custom0/custom1 の経路を全 pass で維持する。B の lowering はこの
3 variant、公開 template/library、set 2 resource、render state を C-material 記述として出力する。

## WP70 で解消済みの差分

- set 0 は全 pipeline で固定 layout となり、`ObjectBuffer` と `LightUBO` の binding 衝突を解消した。
- material data は個別 UBO でなく set 2 binding 6 の SSBO 配列になった。glTF PBR factor と
  VAT の静的 bounds/playback 値も同じ配列から material index で参照する。
- push constant は engine 64B / shader 64B に reflection 段階で分割・検証される。
