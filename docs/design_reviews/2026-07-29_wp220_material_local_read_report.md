# WP220 material same-pixel local-read ABI 実装報告

日付: 2026-07-29

状態: **完了**

## 結論

`.surface` の screen input と image resource port を、公開アクセサを変えずに
`combined image sampler` または `input attachment` へ物理解決できるようにした。
material pass が `same_pixel` read を宣言し、target planner が
`pelican.vulkan.tile_local_plan@1` を選べる場合、material raster consumerも
producerと同じdynamic rendering scopeへ融合される。

shader authorはVulkan descriptor type、binding、input attachment indexを記述しない。
同じ `pelican_screen_<name>()` / `pelican_sample_<name>()` がdesktopの
materialized経路ではtexture sampling、tile-local経路では`subpassLoad()`になる。
これはdeferred lighting/decal等の機構を可能にする固定境界であり、
特定の技法名をengine enumへ追加する実装ではない。

## 公開記述

再利用可能なsurfaceはshader-facing image portだけを宣言する。

```glsl
//! pelican.surface v1
//! language: glsl
//! resource_ports:
//!   - { name: local_color, kind: image, stage: fragment }

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    surface.base_color =
        pelican_sample_local_color(input_data.uv);
}
```

material passがlogical resourceとread footprintを割り当てる。

```json
{
  "name": "local_material",
  "type": "material",
  "material_contract": "forward_opaque_v1",
  "material_resources": {
    "local_color": {
      "resource": "material_local_source",
      "access": "sampled",
      "view": "shared_2d",
      "footprint": "same_pixel"
    }
  },
  "output": {
    "color": "lit_color",
    "depth": "offscreen_depth"
  }
}
```

`screen_inputs`も同じ仕組みを使う。組み込みcontractのうちdepth系の
same-pixel inputはlocal-read候補になり、offsetを許す`opaque_color`は
`neighborhood`のため通常のsampler経路を維持する。

## logical / physical の所有権

1. `.surface` はsemantic input名とstageだけを所有する。
2. material passはgraph resource、history、view、sampling、read footprintを所有する。
3. logical/target compilerはresource lifetime、extent、sample、view、device/format capability
   からmaterializedまたはtile-localを選ぶ。
4. compiled rendering passはlocal-read対象へ物理input attachment indexを割り当てる。
5. material shader compilerは全active graph variantが同じ物理ABIに解決されることを確認し、
   compiler-owned defineでsamplerまたはinput attachment宣言を生成する。
6. SPIR-V reflectionはdescriptor type、stage、名前、input attachment indexを検証する。
7. material pipeline/descriptorは同じcompiled contractから
   `VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT`、null sampler、
   `VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR`を設定する。

descriptor bindingとinput attachment indexは別の番号空間である。bindingはsurfaceの
interface順、input attachment indexはcompiled passの物理input tableから決まり、
両者を同じ値と仮定しない。

## 選択条件とfallback

tile-local化には次が必要である。

- non-history、single-sample attachment
- fragment-only material image port、またはsame-pixel screen input
- read footprintが`same_pixel`
- producer/consumerのextent、view、attachment scopeが互換
- device/formatがdynamic rendering local readと必要usageを受理
- 同じmaterial routeを使う全active graph variantで
  sampler/local-read種別とinput attachment indexが一致

いずれかを満たさなければ、plannerはmaterialized sampled imageへ戻す。
local variantではpublic関数のUV/LOD引数はABI互換のため残るが、意味論上は現在画素だけを読み、
引数値を使わない。`neighborhood`、`arbitrary`、`temporal`を
`same_pixel`と偽ってlocal化してはならない。

sequential viewはlayer別2D descriptorを使う。layered multiviewはlocal-read時だけ
2D-array attachment viewを同じinput attachmentへbindできる。通常samplingでの
material `sampler2DArray` accessorは別拡張である。

## failure-atomic境界

- input attachmentはhistoryを読めない。
- image subresource指定はmaterial portでは引き続き未対応である。
- feature-owned trailing material inputsは現状sampler-onlyである。
- 生成image accessorは現在floating-point `vec4`契約である。
  UINT/SINT color targetのlocal-readは型付き
  `usubpassInput` / `isubpassInput`が必要なため、名前付きエラーで拒否する。
- graph変更でlive materialのsampled/local種別、input attachment index、
  format、MSAA等が変わる場合、coordinated material rebuildがまだ無いため
  candidate generationを公開せず旧generationを維持する。
- custom schemaと同様、shaderc OFF配布物はWP211 `dist-bake`の対象である。

## 検証

Debug / OpenXR OFF buildで次を確認した。

- `renderingpass_helpers_test`: 480 assertions / 67 cases
- `renderingsamplecount_test`: 181 assertions / 18 cases
- `surfacecompiler_test`: 301 assertions / 19 cases
- `targetplanning_test`: 164 assertions / 10 cases
- `shader_compiler_reflection_test`: 95 assertions / 8 cases
- `materialbinding_test`: 16 assertions / 3 cases
- `shader_library_test`: 104 assertions / 9 cases
- headless `[wp220]`: 27 assertions / 1 case
- headless `[tile-local]`: 40 assertions / 2 cases
- headless `[material-resource]`: 36 assertions / 1 case

WP220 headless gateはfullscreen producerが`material_local_source`へ書き、
forward material passが同じ画素をgenerated
`pelican_sample_local_color()`から読み、最後にsampled fullscreen passが結果を表示する。
実deviceでtargetの`tile_local_attachment`選択、scope fusion、SPIR-Vの
`eInputAttachment` reflection、descriptor image view、最終pixelを一度に検証した。

## 次の境界

1. graph + surface + material pipelineのcoordinated hot reload
2. image portのfloating / SINT / UINT typed accessor
3. shaderc OFF / dist-bake artifact
4. Quest/Meta XR Simulator・物理HMDでの帯域、multiview、表示gate
5. deferred decalまたはproject-owned deferred lightingでの技法dogfood
