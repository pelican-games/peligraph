# pipeline の全状態を語彙にし、state node として graph に置く

対象読者: エンジン担当(描画コンパイラ / shader / renderer)、Studio 担当。
ステータス: 改善案メモ(2026-10-22。議論の記録。あとで見直す。WP 化・レビュー前)。
前提: [design_stage2_typed_pipeline.md](design_stage2_typed_pipeline.md)(`CompiledPassDefinition` に `PipelineState` を置く)、
feature GUI の議論(catalog の schema で欄を作る)、講習会の穴(depth bias / wireframe / stencil / line width、汎用 raster の頂点バッファ描画)。
マニュアル [第6章 type: raster](manual/06_rendering.md)(`raster_state` の今の語彙)。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- 今の `raster_state` は topology / cull / front_face / depth test・write・compare / blend だけで、残りの Vulkan graphics pipeline の状態は
  [`pipelinefactory.cpp:577-598`](../src/core/shader/pipelinefactory.cpp#L577) の**固定値**(depthClamp、polygonMode、depthBias、lineWidth、
  sampleShading、stencil、logicOp)。dynamic state は viewport / scissor と front_face だけ。
- 方針: **(1) 語彙を Vulkan の全状態群まで広げて固定値を無くす。(2) 群ごとの state node にして pass の state ポートへ繋ぐ。inline の object も残す。**
  JSON はどちらも正。device 要件は compile 時に名前付きエラー。値を value の edge で動かせる(dynamic state か pipeline の作り直し)。

## 1. 今

- parser は project 側([`rasterpass.cpp`](../src/project/rasterpass.cpp)、材質側は [`surfaceformat.cpp`](../src/project/surfaceformat.cpp)。blend / write mask は同一パーサ)。
- factory は `desc` から topology / cull_mode / front_face / rasterization_samples / depth_test / depth_write / depth_compare / blend を受け、ほかは固定。
- 汎用 raster pass の `draw` は `direct` のみ(頂点バッファ無し)。shader 段は vertex / fragment / compute と RT の 6 段。tessellation / geometry / mesh は無し。

## 2. 語彙(Vulkan の状態群をそのまま)

| 群 | 項目 | device 要件(無ければ名前付きエラー) | 備考 |
|---|---|---|---|
| vertex input | `vertex_layout`: binding(stride、rate)、attribute(location、format、offset) | なし | 汎用 raster の `draw.operation` に `vertex_buffer` / `indexed` / `indirect` を足すのと同時 |
| input assembly | topology(ある)、primitive restart | なし | |
| tessellation | patch control points、`shader.tesc` / `shader.tese` | `tessellationShader` | MoltenVK は対応(制限あり) |
| rasterization | polygon mode(fill / line / point)、cull、front face、depth clamp、depth bias(constant / slope / clamp)、line width、rasterizer discard、conservative | `fillModeNonSolid`、`depthClamp`、`wideLines`、`VK_EXT_conservative_rasterization` | MoltenVK: wide line と conservative は無い |
| multisample | samples(ある)、sample shading + min、sample mask、alpha to coverage / one | `sampleRateShading`、`alphaToOne` | |
| depth / stencil | test / write / compare(ある)、depth bounds、**stencil**(front / back: fail / pass / depthFail / compare、compare mask / write mask / reference) | `depthBounds` | stencil 付き depth format の選択も語彙に |
| color blend | attachment ごとの enable / factors / ops / write mask(ある)、dual source、**logic op**、blend constants | `dualSrcBlend`、`logicOp` | MoltenVK: logicOp 無し |
| shader | 段の追加(tesc / tese / geometry)、**specialization constants**(段ごと) | `geometryShader` | geometry は Metal に無い。任意 |
| compute / RT | subgroup size、max recursion depth、SBT(ある) | | |

- 置き場: 段階 2 強化メモの `CompiledPassDefinition` に `PipelineState`(project、純ロジック)。core は vk 列挙への写像だけ。固定値は全部ここから来る。
- 要件は compile 時に device facts で検査し、feature の `required_capabilities` と同じ流儀で名前付きエラー(`pipeline.state_unsupported@1 {pass, field, feature}` のような形)。

## 3. node にする形

```text
render.rasterization  ─┐
render.depth_stencil  ─┼─▶ render.raster / render.fullscreen / render.material_pass の state ポート
render.blend          ─┤      未接続のポートは既定値。inline の raster_state も従来どおり書ける
render.vertex_layout  ─┘
render.specialization ─┘
```

- state は**群ごと**に node にする(1 つの巨大 node にしない)。複数の pass で共有できる、群ごとに差し替えられる、catalog の schema で Studio の欄が自動で出る、の 3 つが理由。
- JSON は inline の object と node 参照のどちらも正。Studio に **Extract to node**(inline → node)と **Inline**(node → inline)を付ける(Expand to edit… と同じ流儀)。
- material pass の `material_output_states` と surface の depth compare は同じ語彙に畳む(パーサは既に同一)。
- **値を edge で動かす**: stencil reference、depth bias、blend constants、line width は value 領域の edge を受けられる。
  device に extended dynamic state があれば dynamic state に落とし、無ければ値ごとに pipeline を作り直す(hash で cache)。どちらも決定的。
- Frame Plan は pass ごとの**実効 state**(inline + node + 既定 + dynamic の印)を出す。GPU 時間の node は変わらない。
- 既定値は Vulkan の既定ではなく今のエンジンの既定(cull none、depth test false など)を保ち、golden を変えない。

## 4. 順序

| 段 | 内容 | 直るもの |
|---|---|---|
| 1 | 語彙の完成と固定値の排除(`PipelineState`、parser、factory の写像、device 要件) | 講習会の穴: depth bias / wireframe / stencil / line width |
| 2 | 汎用 raster の頂点バッファ描画(`vertex_buffer` / `indexed` / `indirect`)と `vertex_layout` | 自分の頂点配列で描ける |
| 3 | state node と pass の state ポート、Extract / Inline、Frame Plan の実効 state | 共有と Studio の欄 |
| 4 | value edge による dynamic state(extended dynamic state の有無で分岐) | 実行時に動く state |
| 5 | tessellation / geometry の段(任意) | |

1〜2 は段階 2 強化と同じ場所。3 は catalog の schema に乗るだけなので Studio 側は小さい。

## 5. 未決

- state node の粒度(上の 5 群でよいか、blend を attachment ごとの node に割るか)。
- inline と node の両方があるときの優先(node 優先で inline は無視か、衝突を名前付きエラーか)。
- extended dynamic state の版(1 / 2 / 3)のどこまでを前提にするか。MoltenVK の対応範囲を mac 実証で確認。
- stencil 付き depth format(D24S8 / D32S8)の RT 宣言と、MoltenVK(D24S8 無し)の扱い。
