# WP219 material output attachment state 実装報告

日付: 2026-07-28

## 結論

WP218の任意長・型付きmaterial outputへ、attachmentごとのblend equationと
color write maskを追加した。renderer strategyはVulkanのattachment番号ではなく
`material_outputs.outputs[].name`をキーに状態を指定する。未指定outputは従来どおり
surfaceの`render_state.blend`とRGBA writeを継承するため、既存preset/materialに
追記は不要である。

これはweighted blended OIT、選択的G-buffer更新、integer ID targetを併設する
transparent MRTの共通固定機能であり、技法名をengine enumへ追加する実装ではない。

## 公開記述

material passの`material_outputs`と同じ階層へ、疎な
`material_output_states` objectを書く。

```json
{
  "name": "weighted_accumulation",
  "type": "material",
  "material_contract": "forward_transparent_v1",
  "material_outputs": {
    "schema": "pelican.material_outputs",
    "version": 1,
    "name": "project.weighted_oit",
    "outputs": [
      {
        "name": "accum",
        "type": "vec4",
        "source": "surface.base_color"
      },
      {
        "name": "revealage",
        "type": "float",
        "source": "custom"
      },
      {
        "name": "object_id",
        "type": "uint",
        "source": "custom"
      }
    ]
  },
  "material_output_states": {
    "accum": {
      "blend": {
        "color": { "src": "one", "dst": "one", "op": "add" },
        "alpha": { "src": "one", "dst": "one", "op": "add" }
      },
      "write_mask": "rgba"
    },
    "revealage": {
      "blend": {
        "color": {
          "src": "zero",
          "dst": "one_minus_src_color",
          "op": "add"
        },
        "alpha": {
          "src": "zero",
          "dst": "one_minus_src_alpha",
          "op": "add"
        }
      },
      "write_mask": "r"
    },
    "object_id": {
      "blend": "opaque",
      "write_mask": "r"
    }
  }
}
```

`blend`は`opaque`、`blend`、`additive`のpreset、またはcolor/alpha別の
`src` / `dst` / `op` objectを受理する。factorは`zero`、`one`、
`src_color`、`one_minus_src_color`、`dst_color`、
`one_minus_dst_color`、`src_alpha`、`one_minus_src_alpha`、
`dst_alpha`、`one_minus_dst_alpha`、`src_alpha_saturate`である。
operationは`add`、`subtract`、`reverse_subtract`、`min`、`max`である。
write maskは`rgba`の任意部分集合または`none`を使う。

blend constantとdual-source blendはv1に含めない。前者はpipeline-wide constantの
公開契約、後者はfragment dual-source output ABIとdevice featureが別途必要であり、
factor名だけを先に公開すると動かない設定を作るためである。

## compileと所有権

1. project層がfield名をschemaへ照合し、schema順のtyped overrideへ正規化する。
2. material routing compilerがflat/XR/previewでschemaとstateの一致を要求し、
   resolved metadataへcanonical JSONとfingerprintを残す。
3. runtime pass bindingがoutput schema/stateをmaterial physical contractへ渡す。
4. material pipeline compilerがsurface render stateを既定値として全attachmentへ展開し、
   疎なoverrideだけを適用してVulkan stateへlowerする。
5. pipeline keyとlive material snapshotがstateを含む。graph reloadでstateが変わる場合は
   WP222のcoordinated rebuildでmaterial pipelineも同じcandidateへ再prepareする。

固定機能の意味はpass/renderer strategyが所有する。`.surface`は透明/不透明など
material自身の既定stateを引き続き所有し、個別G-buffer field名やOIT技法を知らない。

## capabilityと失敗条件

- output名がschemaに無い、空override、未知factor/op/channelはparse時に拒否する。
- integer outputでblendを有効化する記述はparse時に拒否する。
- output別stateが実際に異なる場合、logical deviceで`independentBlend`を有効化して
  いなければpipeline作成前に拒否する。
- blendを有効化したphysical formatには`COLOR_ATTACHMENT_BLEND` supportを要求する。
- `color_attachment_states`とphysical color formatの枚数不一致は
  `PipelineFactory`が拒否する。
- route variant間のstate差、resolved fingerprint改ざん、live graph reload差は
  publication前に拒否し、旧generationを維持する。

## 受け入れ

- `renderingpass_helpers_test [wp219]`: 16 assertions
- `renderpipeline_resolve_test [wp219]`: 7 assertions
- `headless_render_test [wp219]`: 42 assertions、6-MRT、MSAA、
  `independentBlend`を使う実GPU gate

headless gateは`B8G8R8A8_UNORM` albedoを0.25でclearし、shaderの0.25を
`one + one`で加算して0.5にする。同時にR-only write maskでG/Bのclear値を保持し、
6枚目の`R32_UINT` object IDはblendを無効化して73を書き込む。fullscreen consumerで
三つの結果を同じ画素から読み、stateを変えるgraph hot reloadとschemaを変えるreloadの
両方がrollbackされることを確認する。

## 残る境界

1. material/custom raster shaderからのsame-pixel local-read input ABI
   （WP220で解消）
2. graph + surface + material pipelineのcoordinated hot reload（WP222で解消）
3. shaderc OFF / dist-bakeでのcustom schema artifact
4. weighted OIT accumulation + compositeをproject-owned featureとしてdogfood
5. blend constants、dual-source blend、logic op、advanced blend extension

G18は後続のWP220で同じ`MaterialPipelineRenderingContract`へinput attachment ABIを
追加して解消した。WP219のoutput stateやWP218のoutput枚数を別の固定スロットへ戻さない。
詳細は
[`2026-07-29_wp220_material_local_read_report.md`](2026-07-29_wp220_material_local_read_report.md)
を参照する。
