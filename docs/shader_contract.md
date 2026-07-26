# シェーダ契約(v1 / WP70 実装)

対象: プロジェクト/feature/マテリアル用シェーダを書く人。ここでは
`src/core/shader/pelican_sets.hpp`、`src/core/resources/shaders/include/pelican_sets.glsl`、
`PipelineFactory`、既存シェーダ、各 renderer/container の現行実装から読める契約だけを記録する。

## Descriptor sets

| set | 名前 | 現行の使い方 |
|-----|------|--------------|
| 0 | `PELICAN_SET_FRAME` | 全 pipeline 共通の固定 layout。binding 0 `FrameUBO`、1 `ObjectBuffer` SSBO、2 `LightUBO`、3 `PreviousObjectBuffer` SSBO、4 `FrameResolutionUBO`。エンジン管理・読み取り専用。 |
| 1 | `PELICAN_SET_PASS_INPUT` | fullscreen / UI / compute の入力。fullscreen は pass input の texture/buffer を binding 0 から順に割り当てる。UI texture もこの set を使う。 |
| 2 | `PELICAN_SET_MATERIAL` | 標準 material texture。binding 0 `baseColorSampler`、1 `metallicRoughnessSampler`、2 `normalSampler`、3 `emissiveSampler`。VAT 有効時は 4 `vatPositionSampler`、5 `vatNormalSampler`。binding 6 は全マテリアルを並べた `MaterialBuffer` SSBO。 |
| 3 | `PELICAN_SET_FREE` | variant ごとの補助枠。debug draw/text は binding 0 の SSBO、debug text fragment は binding 1 の atlas texture、`PELICAN_SKINNED` は binding 2 の `SkinPalette` SSBO を使う。binding 2 はスキン variant だけエンジン所有。 |

機械可読な正本は [`material_resources_manifest.json`](material_resources_manifest.json) に置く。
`.surface` の custom texture は宣言順に set 2 binding 7 から割り当て、`role: color`
(または `color_space: srgb`) は SRGB view、`role: data` は UNORM view を使う。
未指定 resource は white / flat-normal / black の semantic dummy に解決され、いずれも
SRGB/UNORM の両 view を持つ。binding 6 の `MaterialBuffer` は既存 96 byte の標準 field に
続けて 256 byte の custom value payload を持つ。payload 内の member offset は `.surface`
params の宣言順で計算した std140 layout が正であり、JSON object の列挙順には依存しない。

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
実 descriptor 変数は generated include の内部詳細であり、shader は set/binding を書かない。

`view: "shared_2d"` は全 view 共通の `sampler2D` / `image2D` である。
`view: "per_view"` は sequential graphics では当該 eye の 2D view、multiview graphics と
1回 dispatch の compute では 2D array になる。後者の accessor は `view_index` を受け、
`pelican_view_count_<port>()` も生成する。logical view と physical target plan が一致しない
構成は pipeline 登録前に拒否する。

reflection は port の set、binding、descriptor kind、count、生成変数名、2D/2D-array 次元を
照合する。hot reload も同じ interface を保存して再検証する。`resource_ports` を持たない
既存 shader の raw set 1 ABI は不変であり、buffer や特殊 descriptor の escape hatch として
利用できる。typed buffer port と material vertex/fragment consumer は WP207b の範囲である。

### 名前付きスクリーンスナップショット(M3.5 / WP83)

`.surface` の `screen_inputs` は rendering config の `snapshots` で先に定義した名前だけを
参照する。宣言順 `i` の入力は set 1 binding `i` の combined image sampler となり、
生成 accessor `vec4 pelican_screen_<name>(vec2 uv)` で読む。入力名は shader identifier
でなければならず、未定義名は material 名・snapshot 名を含む起動時エラーになる。

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

## ランタイム SPIR-V ディスクキャッシュ (WP82)

runtime shaderc の出力はプロジェクトローカルの
`<project>/.pelican/shader_cache/` に保存する。キャッシュキー v1 は次の次元を
長さ付きで直列化し、その全体を SHA-256 にした値である。

1. root source と、実際の include 解決規則で到達する全 file / virtual / engine include の
   名前および bytes から作る source graph SHA-256。macro 展開された include も取りこぼさないよう、
   include root 配下・virtual source・埋込み engine shader source の全候補も保守的に含める
2. shaderc に渡す全 define（値と順序を含む。source 内で未使用でも含む）
3. shaderc toolchain version と、runtime が報告する SPIR-V version / revision
4. target environment (`vulkan-1.2`)、shader stage、entry point
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

公開 source library は `engine://shaders/include/pelican_surface_v1.glsl` と
`engine://shaders/include/pelican_lighting_v1.glsl` である。後者は
`pelican_light_count()`、`pelican_light(i, world_position)`、
`pelican_shadow(i, world_position)`、`pelican_env_ambient(normal)` を公開する。
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
sampled image は set 2 binding `7 + 2*i`、sampler は `8 + 2*i` であり、CPU binding 表は
logical name、descriptor type、remap 前後の set/binding を持つ。既定 source 経路の combined
image sampler (`7 + i`) は fallback の互換契約として不変である。

キャッシュキーには固定3依存の revision/実行版、target env、template/user compiler generator、
両入力 SHA-256、export/import symbol、material set/binding 規約、全 define/pass/stage/ABI salt を含める。
GLSL library corpus は uncalled hook を保持する `--keep-uncalled`、Slang corpus は hook の
`[noinline]` と `-O0` を規約とする。

## WP70 で解消済みの差分

- set 0 は全 pipeline で固定 layout となり、`ObjectBuffer` と `LightUBO` の binding 衝突を解消した。
- material data は個別 UBO でなく set 2 binding 6 の SSBO 配列になった。glTF PBR factor と
  VAT の静的 bounds/playback 値も同じ配列から material index で参照する。
- push constant は engine 64B / shader 64B に reflection 段階で分割・検証される。
