# WP218 material outputs 実装レポート

日付: 2026-07-28

状態: **完了**

## 結論

material passのcolor outputを固定5枚ABIから、renderer strategyが定義する
版付き・順序付きschemaへ拡張した。明示schemaの枚数にengine独自の上限はない。
実行時の上限はVulkan deviceの`maxColorAttachments`と、各format/sample capabilityである。

従来presetはschemaを省略し、既存のdeferred 5-MRT / forward 1-color経路をそのまま使う。
したがって標準利用者に詳細設定を強制せず、G-bufferを増減・再配置・整数化したい
rendererだけが新しい契約を記述する。

## 公開契約

material passへ`pelican.material_outputs` v1を置く。

```json
{
  "material_contract": "deferred_geometry_v1",
  "material_outputs": {
    "schema": "pelican.material_outputs",
    "version": 1,
    "name": "project.extended_gbuffer",
    "outputs": [
      { "name": "base_color", "type": "vec4", "source": "surface.base_color" },
      { "name": "normal", "type": "vec4", "source": "surface.normal_encoded" },
      { "name": "object_id", "type": "uint", "source": "custom" }
    ]
  },
  "output": {
    "color": ["gbuffer_base_color", "gbuffer_normal", "gbuffer_object_id"],
    "depth": "offscreen_depth"
  },
  "clear_colors": {
    "gbuffer_object_id": [4294967295, 0, 0, 0]
  }
}
```

`outputs[]`の順序は次の3つに同時に使うためABI-significantである。

1. generated fragment shaderのlocation
2. material graphics pipelineのcolor format順
3. passの`output.color` target順

型は`float/vec2/vec3/vec4`、`int/ivec2/ivec3/ivec4`、
`uint/uvec2/uvec3/uvec4`。builtin sourceはsurface/input/lightingの既知値を生成コードで
供給する。`custom` fieldは`.surface`の次のhookから名前で書く。

```glsl
void pelican_material_outputs_v1(
    in PelicanSurfaceInputV1 input_data,
    in PelicanSurfaceV1 surface,
    inout PelicanMaterialOutputsV1 outputs) {
    outputs.object_id = 73u;
}
```

実装の正は[`materialoutput.hpp`](../../src/project/materialoutput.hpp)、
利用方法は[manual 6章](../manual/06_rendering.md)を参照する。

## compile / runtime 接続

| 段階 | WP218の責務 |
|---|---|
| project parse | closed schema、版、識別子、重複名、型/source組合せを検証 |
| route compile | material pass、flat/XR variant、compiled frame routeへ同じschemaを保持 |
| target planning | output topologyを既存physical plannerへ渡し、device attachment上限とformat/sample capabilityを検証 |
| surface compile | schemaからoutput struct、location宣言、builtin write、custom hookを生成 |
| reflection | SPIR-Vの全fragment output locationとnumeric typeを宣言へ照合 |
| pipeline create | routeの実color/depth format、MSAA、local-read mappingでpipelineを作成 |
| execution | target別typed clear、MSAA resolve、sampled consumer、history初期化を実formatで処理 |
| hot reload | live pipeline contractとcandidateをpublish前に比較し、不一致なら旧generationを維持 |

固定5枚の配列はschema省略時のcompatibility defaultにだけ残した。任意schema経路は
`std::vector`とcompiled route metadataを使い、枚数定数を参照しない。

## 同時に閉じた型安全性の穴

- pass-wide `clear_color`に加え、RT名をキーにした疎な`clear_colors`を追加した
- clear値は最終physical formatからfloat/SINT/UINTを選ぶ。integer値の小数・範囲外を拒否する
- history imageもalternate format決定後にtyped clearへ変換する
- integer sampled targetのimage作成時にlinear filtering capabilityを要求しない
- fullscreen consumerが要求したfilterを実format capabilityへ照合する
- nearest samplerのmipmap modeもnearestにし、Vulkan validationの不整合を除いた
- 一般的な8/16/32-bit color integer/float formatをrender target parser、
  byte-size計算、storage-image mappingへ追加した

## hot reload の境界

既存material pipelineはcolor/depth format、MSAA、local-read mapping、output schemaを
snapshotする。render graphだけを変更してこのcontractを変えたcandidateは公開しない。
テストではschema名を不整合に変更し、reload失敗後もactive generationのpointerが同一で、
次frameの描画結果も変わらないことを確認した。

graphと`.surface`を同時に変更して全live material pipelineをcandidate generationへ
再構築するcoordinated transactionは未実装である。現状の拒否は互換維持ではなく、
古いshaderと新しいattachment列を混ぜないためのfailure-atomic境界である。

## 検証

Debug / OpenXR OFF buildで次を実行した。

```text
pelican_test_renderingpass_helpers_test   461 assertions / 65 cases
pelican_test_surfacecompiler_test         282 assertions / 18 cases
pelican_test_renderpipeline_resolve_test  133 assertions / 14 cases
pelican_test_shader_library_test          104 assertions / 9 cases
pelican_test_targetrenderplanning_test    244 assertions / 28 cases
pelican_test_multiview_execution_test      63 assertions / 1 case
headless [wp196],[wp218]                   42 assertions / 2 cases
```

WP218 headless gateは6枚目の`R32_UINT` G-bufferを追加し、MSAAを
`lower_supported`で要求する。surface hookが中央へ`73u`を書き、未描画領域は
attachment別clearの`UINT_MAX`になる。resolve後の`usampler2D` consumerで中央と隅を
別色としてreadbackし、実shader writeとtyped clearの両方を検証する。
deviceが6 color attachmentsまたは対象format/sampleを持たない場合は、理由付きskipとなる。

## 意図的に残す不足

優先度順の次候補は次である。

1. **attachment別blend / color write mask（WP219で解消）**

   `material_output_states`をschema field名へ対応付け、route/pipeline/hot reloadと
   `independentBlend` capabilityまで接続した。詳細は
   [`2026-07-28_wp219_material_output_states_report.md`](2026-07-28_wp219_material_output_states_report.md)。
2. **material/custom raster local-read input ABI（WP220で解消）**

   同じsurface accessorをsampler/input attachmentへlowerする公開契約を追加した。
   詳細は
   [`2026-07-29_wp220_material_local_read_report.md`](2026-07-29_wp220_material_local_read_report.md)。
3. **coordinated graph + surface hot reload**

   schema変更時にlive material pipelineもcandidate世代へ再prepareするtransaction。
4. **shaderc OFF / dist-bake**

   独自schemaはfragment source生成を必要とする。配布・Quest向けにはWP211が生成物を
   bakeし、runtime compilerなしで同じreflection contractを再検証する必要がある。
5. **consumer semanticsのdogfood**

   bent normal、custom material model、weighted OIT等をproject-owned featureとして実装する。
   engineはfield名から意味を推測せず、renderer strategyとconsumer shaderが意味を所有する。
6. **material以外のgraphics MRT authoring**

   WP218はG-bufferを生成するmaterial passを対象とする。fullscreenは現在1 color/passで、
   custom geometry/pass contractもG15の境界に残る。必要になった時は同じordered output
   schemaを一般graphics interfaceへ昇格し、material専用の意味は持ち込まない。

この後続実装は`MaterialOutputSchema`、compiled route、physical rendering contractを
拡張点として使う。固定スロットやVulkan値の分岐を別系統へ再導入しない。
