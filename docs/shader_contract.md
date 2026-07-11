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

## WP70 で解消済みの差分

- set 0 は全 pipeline で固定 layout となり、`ObjectBuffer` と `LightUBO` の binding 衝突を解消した。
- material data は個別 UBO でなく set 2 binding 6 の SSBO 配列になった。glTF PBR factor と
  VAT の静的 bounds/playback 値も同じ配列から material index で参照する。
- push constant は engine 64B / shader 64B に reflection 段階で分割・検証される。
