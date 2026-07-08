# シェーダ契約(v1 / 現行実装)

対象: プロジェクト/feature/マテリアル用シェーダを書く人。ここでは
`src/core/shader/pelican_sets.hpp`、`src/core/resources/shaders/include/pelican_sets.glsl`、
`PipelineFactory`、既存シェーダ、各 renderer/container の現行実装から読める契約だけを記録する。

## Descriptor sets

| set | 名前 | 現行の使い方 |
|-----|------|--------------|
| 0 | `PELICAN_SET_FRAME` | エンジン管理のフレーム/パス単位データ。標準 material/shadow では binding 0 が `ObjectBuffer` SSBO、lighting fullscreen では binding 0 が `LightUBO`。同じ set/binding でも pipeline ごとに型が違う現状がある。 |
| 1 | `PELICAN_SET_PASS_INPUT` | fullscreen / UI / compute の入力。fullscreen は pass input の texture/buffer を binding 0 から順に割り当てる。UI texture もこの set を使う。 |
| 2 | `PELICAN_SET_MATERIAL` | 標準 material texture。binding 0 `baseColorSampler`、1 `metallicRoughnessSampler`、2 `normalSampler`、3 `emissiveSampler`。VAT 有効時は 4 `vatPositionSampler`、5 `vatNormalSampler`。material params UBO は設計上ここへ追加予定だが現行コードにはまだない。 |
| 3 | `PELICAN_SET_FREE` | debug/user/future 用の自由枠。現行 debug draw/text は binding 0 の SSBO、debug text fragment は binding 1 の atlas texture を使う。 |

Descriptor set layout は `ShaderReflection` の SPIR-V reflection から
`PipelineFactory` が pipeline ごとに生成する。ある shader が高い set 番号だけを使う場合でも、
0 から最大 set までの layout vector が作られ、使わない set は空 layout になる。

## Push constants

`PELICAN_PUSH_ENGINE_BYTES = 64`、`PELICAN_PUSH_SHADER_BYTES = 64`、
`PELICAN_PUSH_TOTAL_BYTES = 128` が C++ と GLSL include に定義されている。

現行実装の注意:

- `ShaderReflection` は shader の push constant block を 1 つの連続 range として扱う。
  engine 64B と shader 64B の 2 range 分割はまだ enforcement されていない。
- 標準 material/shadow の先頭 64B は `mat4 vpMatrix` として使われる。
- VAT material は同じ offset 0 から最大 128B まで使う。
- fullscreen の `camera_position` は fragment push constant 16B、
  `projection_view` は fragment push constant 128B を offset 0 に push する。
- UI は vertex push constant として 32B を offset 0 に push する。

マテリアル用カスタムシェーダは、M2 で別契約が追加されるまで offset 0 の engine 領域を
独自用途に使わないこと。現在はコード側で分割保護されていないため、衝突は shader 作者側で避ける。

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

## 現状として記録する差分

- set 0 の意味は設計文書では frame 共通データだが、現行実装では pipeline ごとに
  `ObjectBuffer` と `LightUBO` が binding 0 を共有している。
- material params UBO は設計上 set 2 に入るが、現行 binding は texture 0-5 だけである。
- push constant の engine 64B / shader 64B 分割は定数としてはあるが、pipeline layout は
  reflection 由来の単一 range である。
