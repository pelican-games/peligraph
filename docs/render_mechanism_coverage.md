# 描画機構カバレッジ分析: 何がユーザー空間で書けて、何が書けないか(v15)

対象読者: エンジン担当、および feature / material / shader をユーザー空間で書く人。

ステータス: **v15(2026-07-29)**。84 行の技法候補を現行コードとテストへ再照合し、
WP207bのmaterial typed resource consumer、WP208のscalable lighting/cluster selection、
WP209aのstatic texture dimension/material sampler authoring、WP209bの2D runtime RT
mip/layer/subresource view、WP218の任意長・型付きmaterial output ABI、
WP219のoutput別blend/write-mask、WP220のmaterial same-pixel local-read、
WP228のplanar reflection Forward transparent capture/family-local sort、
WP229のViewFamily-local clustered selectionまで反映した。
instance/draw-owned layer、opaque/transparent phaseを跨ぐvariant queue、runtime
3D/cube targetとraster attachment subresource、typed integer material image inputは
後続である。監査記録は
[`design_reviews/2026-07-26_render_capability_authoring_audit_codex.md`](design_reviews/2026-07-26_render_capability_authoring_audit_codex.md) と
[`design_reviews/2026-07-28_wp218_material_outputs_report.md`](design_reviews/2026-07-28_wp218_material_outputs_report.md)、
[`design_reviews/2026-07-28_wp219_material_output_states_report.md`](design_reviews/2026-07-28_wp219_material_output_states_report.md)、
[`design_reviews/2026-07-29_wp220_material_local_read_report.md`](design_reviews/2026-07-29_wp220_material_local_read_report.md)、
[`design_reviews/2026-07-29_wp226_planar_reflection_report.md`](design_reviews/2026-07-29_wp226_planar_reflection_report.md)、
[`design_reviews/2026-07-29_wp228_planar_transparent_capture_report.md`](design_reviews/2026-07-29_wp228_planar_transparent_capture_report.md)、
[`design_reviews/2026-07-29_wp229_view_family_clustered_selection_report.md`](design_reviews/2026-07-29_wp229_view_family_clustered_selection_report.md)。

## 0. 判定規則

エンジンの方針は「機構はエンジン、技法はユーザー空間」である。ここで問うのは
**技法名を engine enum に持っているか**ではなく、次の条件を満たすかである。

> `src/` を変更せず、project の feature / material / shader だけで動作する実装を
> 完結できるか。

| 判定 | 意味 |
|---|---|
| **○** | 実装済み、または既存の直接的な public hook/input だけで完結する |
| **△** | 回避策、固定上限、外部 bake、未 dogfood の組み合わせが必要 |
| **✕** | 下記 G1〜G18 のうち未解消の具体的な機構不足がある |

単に「GLSL なら計算式を書ける」は ○ の根拠にしない。複数技法を一行にまとめた項目は
個別判定を併記するため、○/△/✕の単純合計を優先順位に使わない。

## 1. 現在ユーザー空間へ露出している機構

### 1-1. 実装・テスト済み

| 機構 | 現在の契約 | 根拠 |
|---|---|---|
| render target | fixed/output-relative extent、format/class/role、usage、history、format candidates、fixed/full mip、array layers、fullscreen/compute subresource view | [rendertargetjsonparser.cpp](../src/core/renderingpass/rendertargetjsonparser.cpp) |
| MRT / MSAA | materialは版付きschemaで任意長のfloat/SINT/UINT color output、typed sample-count resolve、target別typed clear。上限はdeviceの`maxColorAttachments`だけ。fullscreenは現在1 color/pass | [materialoutput.hpp](../src/project/materialoutput.hpp) |
| fullscreen input | image **および storage buffer**、image は filter/address 指定 | [fullscreenpasscontainer.cpp:281](../src/core/fullscreenpass/fullscreenpasscontainer.cpp) |
| material surface | `displace` / `surface` / `brdf` / `lighting`、custom params/texture、generated accessor | [surfacecompiler.cpp:55](../src/core/shader/surfacecompiler.cpp) |
| material render state | opaque/blend/**additive**、none/**front**/back cull、depth test/write/compare | [surfaceformat.cpp:530](../src/project/surfaceformat.cpp) |
| material selection | material `tags` + pass `material_filter.include/exclude`。compile済みcompact range、flat/preview/XR共通 | [drawqueuebuilder.cpp](../src/core/renderer/drawqueuebuilder.cpp) |
| material screen input | `opaque_color` / depth / linearized depth、opaque snapshot、sampler/input-attachment自動lowering、XR layered binding | [surfacecompiler.cpp](../src/core/shader/surfacecompiler.cpp) |
| material graph resource | `.surface`のvertex/fragment typed readonly buffer・image port、generated sampler/local-read accessor、pass mapping、世代固定descriptor | [surfacecompiler.cpp](../src/core/shader/surfacecompiler.cpp) |
| compute | fixed dispatch、storage buffer/image、reads/writes/after/before | [computetask.cpp](../src/core/renderingpass/computetask.cpp) |
| temporal | history、velocity、projection jitter、reset epoch、解像度分離 | [taa.json](../src/core/resources/features/taa.json) |
| graph/compiler | logical type/value、target planning、physical fragment、transient/tile-local/alias runtime | [design_render_graph_compiler.md](design_render_graph_compiler.md) |
| XR | sequential/multiview、内部 2D-array image/view、array swapchain/depth submit | [design_openxr.md](design_openxr.md) |

### 1-2. 境界を正確に言う必要があるもの

| 項目 | 現在の境界 |
|---|---|
| graphics buffer input | fullscreen はraw storage buffer、material/geometryはtyped readonly storage bufferに対応。material write/atomicは未公開 |
| sampler | fullscreen/compute named image port は filter/address 指定可。material custom texture は filter/mip-filter/address/compare/anisotropy を typed 宣言できる。compare の実 binding は compatible depth image provider待ち |
| texture dimension | project authored KTX2 の 2D/cube/2D-array/3D、runtime 2D RTのmip/layerとsampled/storage部分viewは公開済み。runtime 3D/cube targetとraster attachment部分viewは未公開(G10b) |
| render state | surface単位とpass-local named variantに対応済み。同一opaque/transparent phase内で別state/surfaceを使える。phase跨ぎはvariant-aware draw queue待ち |
| pass kind | implementation provider は差し替え可。authoring kind と material contract は v1 閉集合(G15) |
| object selection | material単位は安定tag/filter対応済み。instance/draw-owned tag/layerは未公開(G14)。`material_range`はlegacy draw-call ordinal |
| material raster physical ABI | output枚数・順序・型・source、field名別blend/write-mask、screen/resource same-pixel inputのsampler/input-attachment loweringを公開済み。整数image inputはtyped accessor待ち |

## 2. 技法別判定(84 行)

### A. material / shading

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| A1 | custom BRDF(髪の異方性・布 sheen・clear coat) | **○** | `brdf` hook |
| A2 | cell/ramp shading(PBR base) | **○** | `lighting` hook + custom texture |
| A3 | rim light / Fresnel decoration | **○** | `lighting` hook |
| A4 | SDF face shadow | **○** | custom texture と light direction |
| A5 | matcap / fake environment | **○** | custom texture + `lighting` |
| A6 | parallax occlusion mapping | **○** | `surface` hook |
| A7 | detail map / triplanar | **○** | `surface` hook |
| A8 | vertex animation(wind/sway) | **○** | `displace` + Frame UBO time |
| A9 | thin-film interference | **○** | `brdf` hook |
| A10 | eye shading(cornea/parallax) | **○** | `surface` + `brdf` |
| A11 | skin SSS analytic approximation | **△** | BRDF近似とmaterial単位のtag/variant overlayは可。screen-space diffusionのtyped contractとinstance単位選別はG15/G14 |
| A12 | wetness / snow accumulation | **○** | `surface` mask composition |
| A13 | virtual texture / texture streaming | **✕** | feedback write、GPU draw/residency、descriptor table(G8/G9) |

### B. lighting

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| B1 | tiled/clustered lighting(raster) | **○** | scalable inventoryとproject-owned clustered selectionをhybrid consumerで実GPU dogfood済み。WP229でflat/XR eye/planar reflectionのfamily-local consumerへ拡張 |
| B2 | tiled/clustered lighting(compute) | **○** | compute selection、typed buffer、70灯overflow、XR左右眼とreflection固有bufferを実GPU実証済み(WP208/WP229) |
| B3 | area light(LTC) | **△** | LUT 評価は可。light schema に shape/orientation/size が無い(G5) |
| B4 | light cookie / IES | **△** | texture は可。lightごとのresource indexが無い(G5/G9) |
| B5 | custom non-shadow attenuation | **○** | `lighting` hook |
| B6 | auto exposure / histogram | **△** | downsample は可。storage image/buffer histogram は構成可能だが実 feature 未検証 |
| B7 | prebaked light probe(SH) | **△** | texture/params 持込み可。placement/bake は外部 |
| B8 | dynamic GI(DDGI / SSGI) | **SSGI △ / DDGI ✕** | 2D subresourceとstatic 3D/arrayは解消したが、probe inventory/runtime 3D targetが不足(G5/G10b) |
| B9 | lightmap | **△** | texture は可。標準 geometry の第2 UV 契約を dogfood する必要あり |

### C. shadow

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| C1 | B-layer material shadow reception | **○** | WP205でpublic shadow relation、project-copy同値、feature-off purge、実Vulkan goldenまで固定 |
| C2 | cascaded shadow map | **○** | WP225でstable `$cascade/N`、hybrid split、XR frustum union、D32 array target、最大8 cascade LightUBO、main-depth選択、cascade別conservative draw compactionを接続。3-layer実Vulkan goldenで全layerと遠方caster除外を検証 |
| C3 | point/spot shadow | **✕** | named multi-view familyのsequential実行とstatic cube/arrayは利用可能。point/spot view provider、runtime cube-face attachment、light shadow schemaが不足(G5/G6b/G10b) |
| C4 | PCSS / PCF | **△** | public shadow resourceとmanual depth compareは利用可能。標準filtered algorithm、comparison sampler利用、品質goldenは未作成 |
| C5 | screen-space contact shadow | **△** | fullscreen depth ray march で構成可能。実 feature/golden 未作成 |
| C6 | shadow cache / virtual shadow map | **✕** | mip/layer view、named view-family、cascade array targetは解消。page table、GPU-driven page execution、residencyが不足(G8/G9) |
| C7 | capsule shadow | **△** | 固定/baked data は可能。動的 capsule inventory の public data channel は G5 |

### D. reflection / environment

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| D1 | prebaked IBL | **○** | 2D octahedral/stripに加えてnative cubemapをmaterialから利用可能(WP209a) |
| D2 | runtime prefilter / dynamic environment | **✕** | 2D mip/subresourceは解消。capture viewとruntime cube targetが無い(G6b/G10b) |
| D3 | parallax-corrected reflection probe | **△** | baked texture + hook。probe selection/data は material単位または G5 |
| D4 | post SSR | **△** | scene color + depth fullscreen で構成可能。実 feature 未作成 |
| D5 | material SSR/refraction integration | **○** | typed screen input と実 Vulkan refraction test 済み |
| D6 | planar reflection | **△** | WP226〜232でreflection provider、per-view clip plane、Vulkan ZO oblique projection、独立解像度Deferred + Forward opaque/transparent capture、汎用family culling、view-local transparent sort、ViewFamily-local clustered selection、7-level low-pass mip生成とfamily-array material samplingを実Vulkan実装。BRDF-aware prefilter、複数plane/probe、secondary multiviewは未完(G6b) |
| D7 | hybrid RT reflection | **✕** | acceleration structure/RT pipeline と descriptor path が無い(G7/G9) |

### E. volumetric / atmosphere

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| E1 | analytic height fog | **○** | fullscreen + depth |
| E2 | radial god ray | **○** | fullscreen chain |
| E3 | froxel volumetric fog | **△** | static 3D textureは可能。runtime 3D target/subresource viewはG10b |
| E4 | ray-marched cloud | **△** | fullscreen 可。3D noise は 2D atlas/外部 bake |
| E5 | physical atmosphere(Bruneton) | **△** | static 3D LUT持込みとcompute image inputは可能。runtime生成3D LUTはG10b |

### F. post process

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| F1 | bloom | **○** | bundled/example 実装 |
| F2 | TAA | **○** | bundled feature 実装 |
| F3 | temporal upscale | **○** | history/velocity/jitter と render/output resolution 分離を実装済み。品質algorithmはuser-space |
| F4 | SSAO/HBAO | **○** | example 実装 |
| F5 | GTAO + bent normal | **△** | 追加G-buffer output自体はWP218で可能。AO producerとlighting consumerのproject dogfoodが未作成 |
| F6 | depth of field | **△** | fullscreen/downsample で構成可能。実 feature 未作成 |
| F7 | motion blur | **△** | velocity は実装済み。blur feature 未作成 |
| F8 | LUT color grading | **○** | 2D stripとstatic 3D LUTを利用可能(WP209a) |
| F9 | chromatic aberration/vignette/grain/flare | **○** | fullscreen |
| F10 | sharpen(CAS class) | **○** | fullscreen |
| F11 | depth/normal post outline | **△** | fullscreen で構成可能。標準 dogfood 未作成 |

### G. transparency / special draw

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| G1t | sorted transparency | **○** | `forward_transparent_v1` + draw sort provider |
| G2t | inverted-hull outline | **○** | WP206bのproject-owned surface/pass dogfoodで、entity/mesh/base material/draw複製なしにfront-cull forward overlayを実GPU描画 |
| G3t | order-independent transparency | **weighted △ / PPLL ✕** | weighted方式に必要な複数typed outputとattachment別blend/write-maskは実装済み。残りはfeature/compositeのdogfood。PPLLはfragment write/atomic、descriptor table、custom geometry contract不足(G9/G15) |
| G4t | stochastic transparency | **△** | dither/mask は可。TAA integration の品質調整が必要 |
| G5t | refraction/glass | **○** | material screen input 実配線・描画済み |
| G6t | additive effect | **○** | `.surface render_state.blend: additive` 実装・test済み |
| G7t | deferred decal | **△** | G-buffer schema拡張、選択write、ping-pong/fullscreen、material local-readは可。mesh decalにはinstance単位選別(G14)とproject-owned dogfoodが必要 |

### H. geometry / performance

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| H1 | GPU culling / depth pyramid | **culling △ / pyramid ○** | 2-layer/full-chain RTのmip0→mip1 computeとfullscreen表示を実GPU dogfood済み(WP209b)。固定状態1 rangeのdraw args/count接続はWP210bで解消。実culling policyとmulti-segmentは未完 |
| H2 | occlusion culling | **△** | GPU visibilityから固定状態1 rangeのrenderer draw count/argsへ接続可能。depth-pyramid判定のproject dogfoodとmulti-segmentは未完 |
| H3 | LOD switching | **△** | game側model swapは可。標準LOD policyなし |
| H4 | impostor | **△** | asset generation外部、runtime selectionはH3 |
| H5 | mesh shader / meshlet | **✕** | shader stage/pipeline と GPU-driven contract が無い(G8/G9/G15) |
| H6 | tessellation / displacement stage | **✕** | tessellation stage authoring が無い(G15) |
| H7 | virtual geometry | **✕** | streaming/residency/GPU-driven/bindless(G8/G9) |
| H8 | GPU particle | **△** | fixed-stateのGPU chosen indexed draw countまで接続可能。particle固有のinstance生成とproject dogfoodは未完 |
| H9 | vegetation wind / mass draw | **△** | windは○。large-scale GPU cullingはG8 |
| H10 | terrain clipmap | **△** | mesh policyはgame側。height displacementは可 |

### I. water / environment

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| I1 | Gerstner wave | **○** | `displace` hook |
| I2 | FFT ocean | **○** | compute buffer→material vertex displacementとfragment sampled imageを実GPU dogfood済み。FFT実装自体はproject shader |
| I3 | water refraction | **○** | material screen input |
| I4 | water reflection | **△** | SSRに加え、WP226〜232の標準planar reflection target/resource port、Deferred + Forward opaque/transparent capture、roughness用mip列とtyped family-array samplerを利用可能。水面のFresnel・法線歪み・BRDF-aware convolutionはproject/material側の置換algorithm |
| I5 | foam / shoreline | **○** | depth差分 + material/fullscreen |
| I6 | caustics | **○** | projected texture + lighting hook |
| I7 | underwater fog/distortion | **○** | fullscreen |

### J. XR

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| J1 | multiview | **○** | synthetic/headless/runtime実装済み。現実装のHMD/対象GPU gateは残る |
| J2 | foveation / VRS | **✕** | device/backend vocabulary 未公開(G11)。Questで重要だが全構成の正当性必須条件ではない |
| J3 | space warp / reprojection | **✕** | OpenXR runtime-specific integration が必要 |
| J4 | per-eye resolution / gaze driven | **✕** | VRS/eye tracking/runtime policy(G11) |
| J5 | arbitrary user feature in XR | **△** | layered/sequential pathは実装済み。各featureのview contractと実機gateが必要 |

### K. ray tracing / next generation

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| K1 | RT shadow/reflection/GI | **✕** | acceleration structure、RT pipeline、descriptor contract(G7/G9) |
| K2 | path-traced reference renderer | **✕** | G7/G9 + accumulation/inventory |
| K3 | neural rendering/NRC | **✕** | model runtime、training/inference resource contract未設計 |

## 3. 機構ギャップ一覧

G 番号は v3 で意味を修正した。v2 の G2/G13 をそのまま参照してはならない。

| ID | 正確なギャップ | 主な対象 |
|---|---|---|
| **G1（解消済み、WP207a）** | compute pipelineへFrame/Light setとnamed sampled/storage image portを実装。fullscreenも同じgenerated interfaceを使う | clustered compute、DDGI、runtime LUTの入力境界 |
| **G2（解消済み、WP207b）** | material vertex/fragmentへtyped readonly buffer/sampled image port、element/stage schema、generated accessor、世代固定descriptorを実装 | FFT ocean、GPU simulation consumer。PPLL write/storage imageは別拡張 |
| **G3（解消済み、WP210a）** | typed command buffer、GPU-written indirect dispatch、自動read edge/barrierを実装 | GPU culling、adaptive work |
| **G4（解消済み、WP209a）** | project-owned KTX2の2D/cube/2D-array/3D、generated accessor、reflection/runtime view照合を実装 | native IBL、3D noise/LUT |
| **G5** | light/custom scene data schemaがdir/point/spotと固定上限中心 | many lights、area/cookie/IES、capsule |
| **G6a（解消済み、WP205）** | public directional shadow resource/light relation、generated `pelican_shadow()`、project-copy同値とpurgeを実装 | filtered PCF/PCSSは品質algorithm側の残件 |
| **G6b（部分解消: WP223〜232）** | stable runtime ViewFamily、pass/task relation、secondary sequential scheduling、family固有extent、directional CSM、planar reflection、oblique near-plane、汎用secondary culling、family別transparent sort、family-local clustered selection、planar mip filter/family-array samplingを実装。secondary multiviewとcube provider/attachmentが未完 | point/spot shadow、reflection probe、advanced planar capture |
| **G7** | acceleration structure / RT shader/pipeline contractが無い | K1/K2 |
| **G8（部分解消、WP210b）** | 固定状態1 material rangeのGPU-written indexed draw/countは実装済み。複数material/pipeline segment、GPU-visible state key、実culling dogfoodが未完 | culling、particles、virtual geometry |
| **G9** | bindless/descriptor indexing contractが無い | large resource tables、RT/virtualized workload |
| **G10a（解消済み、WP209b）** | 2D runtime RTのfixed/full mip、array layer、fullscreen/compute sampled/storage subresource viewを実装 | depth pyramid、2D runtime LUT |
| **G10b** | runtime 3D/cube targetとraster attachmentの任意mip/layer出力が無い | runtime IBL、froxel、raster mip generation |
| **G11** | VRS/fragment density/foveation backend contractが無い | XR foveation |
| **G12（authoring解消済み、WP209a）** | material samplerのfilter/mip-filter/address/compare/anisotropyを実装。hardware compareの実利用はcompatible depth image provider待ち | hardware PCF、special filtering |
| **G13（解消済み、WP206b）** | pass-local named surface/render-state variantを実装。同一phase routeに対応し、opaque/transparent phase跨ぎだけをvariant-aware draw queueへ残す | duplicate-free inverted hull、special overlay |
| **G14** | material-owned stable tag/filterは実装済みだが、instance/draw-owned tag/layerが未公開 | 同じmaterialを共有するinstanceの個別SSS/outline/decal/reflection |
| **G15** | material/geometry pass contractがv1閉集合 | custom geometry stage/output |
| **G16** | stencil state/resource semanticsが未公開 | stencil mask/portal |
| **G17（解消済み、WP219）** | `material_output_states`でschema field名ごとのblend equation / write maskを公開。省略fieldはsurface render_stateを継承し、variant整合性・pipeline key・hot reload・`independentBlend`/format capabilityまで検証 | weighted OIT、選択的G-buffer更新 |
| **G18（解消済み、WP220）** | material screen/resource same-pixel inputを、同じpublic accessorのsamplerまたはinput attachmentへlowering。variant整合、reflection/index、descriptor、sequential/multiview、materialized fallbackを実GPU検証 | tile GPU上のdeferred lighting/decal、subpass相当の融合。UINT/SINT typed inputは別拡張 |

## 4. dogfood の扱い

v2 の raster tiled lighting例は当時 `R32G32B32A32_UINT` がformat parserに無く、
標準light inventoryも32灯なので動作証明になっていなかった。formatとUINT material outputは
WP218、light inventoryはWP208で解消したが、個別技法は引き続き次の順で判定する。

1. project-owned feature/surface/shaderだけで最小fixtureを書く。
2. compile/plan dumpだけでなく、headless Vulkanの画素またはbuffer結果を検証する。
3. flat、preview、sequential XR、multiview、hot reloadのうち影響範囲を回帰する。
4. 成功して初めて○へ上げる。止まった箇所はfile:lineとtyped contractを記録する。

既にこの条件を満たす代表例は、compute buffer → fullscreen、material refraction、
material same-pixel local-read、additive/front/depth surface state、TAA、MSAA、
upscale resolution contract、public directional shadow receptionである。

次の dogfood は以下を推奨する。

1. ~~public directional shadow reception / CSM~~（WP205で受光、WP224でnamed secondary ViewFamily、WP225で3-cascade実GPU dogfood済み）
2. ~~実装済みtag選択 + pass-local variantによる inverted-hull outline~~（WP206bで実GPU dogfood済み）
3. compute result → material vertex displacement
4. ~~scalable light inventoryを使う raster/compute clustered lighting~~（WP208で実GPU dogfood済み）
5. ~~static native cubemapを使うmaterial sampling~~（WP209aで実GPU dogfood済み）
6. ~~authored mip/layer viewを使う depth pyramid~~（WP209bでlayer 1上の実GPU dogfood済み）

## 5. 実装順

詳細な監査と受け入れ条件は
[`2026-07-26_render_capability_authoring_audit_codex.md`](design_reviews/2026-07-26_render_capability_authoring_audit_codex.md)
および [`implementation_plan.md`](implementation_plan.md) の WP205+ 候補を正とする。

推奨順は次である。

1. WP204 runtime sliceは完了
2. G6a public shadow contractはWP205、directional CSMはWP225、planar reflectionと汎用secondary cullingはWP226、Forward opaque captureはWP227、transparent capture/sortはWP228、family-local clustered selectionはWP229、oblique near-planeはWP230、extent-derived dispatch/material remaining-mip/family-array planar filterはWP231〜232で完了。G6bの次はsecondary multiviewまたはpoint/spot cube provider
3. material-owned G14はWP206a、G13の同一phase variantはWP206bで完了。必要なdogfoodでG15、instance/draw-owned G14とphase跨ぎqueueは実需要時に拡張
4. G1はWP207a、G2はWP207bで完了
5. G5 lighting data v2 + clustered dogfoodはWP208で完了
6. G4とG12 authoringはWP209a、G10aはWP209bのdepth pyramidで完了。G10bはruntime 3D/cubeまたはraster subresourceの実需要時
7. G8 fixed-state GPU-written draw arguments/countはWP210bで完了。次は実occlusion
   dogfoodと必要になったsegment metadata。G9 bindlessは実測需要時
8. delivery laneとして`dist-bake`
9. G11 VRSはQuest device gate後

surface render stateとfullscreen buffer inputは既に実装済みなので、再実装WPを作らない。
