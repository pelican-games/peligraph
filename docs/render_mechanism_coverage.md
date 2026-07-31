# 描画機構カバレッジ分析: 何がユーザー空間で書けて、何が書けないか(v22)

対象読者: エンジン担当、および feature / material / shader をユーザー空間で書く人。

ステータス: **v22(2026-07-31)**。84 行の技法候補を現行コードとテストへ再照合し、
WP207bのmaterial typed resource consumer、WP208のscalable lighting/cluster selection、
WP209aのstatic texture dimension/material sampler authoring、WP209bの2D runtime RT
mip/layer/subresource view、WP218の任意長・型付きmaterial output ABI、
WP219のoutput別blend/write-mask、WP220のmaterial same-pixel local-read、
WP228のplanar reflection Forward transparent capture/family-local sort、
WP229のViewFamily-local clustered selection、WP233のtyped shader asset差替え、
WP234のruntime ViewFamily provider registryと標準C++ algorithm package purge、
WP235のraster attachment mip/layer view、WP236のruntime cube target/view、
WP237の交換可能なsix-face capture/provider、WP238bの汎用raster pass ABI
(`"type": "raster"`)、WP240cのproject material索引・runtime lowering・
primitive bindingまで反映した。
instance/draw-owned layer、opaque/transparent phaseを跨ぐvariant queue、runtime
3D target、typed integer material image inputは
後続である。

**v22 での訂正(v21 からの差分)**:

1. WP240c で `pelican.asset_data` v1 の `materials[]` から project material を起動時に
   lowering・GPU登録し、`pelican.material_bindings` の既存 ABI で glTF primitive へ
   割り当てる経路を §1-1 へ反映した。新しい技法 hook ではないため A1〜A12 の判定は不変。

監査記録は
[`design_reviews/2026-07-26_render_capability_authoring_audit_codex.md`](design_reviews/2026-07-26_render_capability_authoring_audit_codex.md) と
[`design_reviews/2026-07-28_wp218_material_outputs_report.md`](design_reviews/2026-07-28_wp218_material_outputs_report.md)、
[`design_reviews/2026-07-28_wp219_material_output_states_report.md`](design_reviews/2026-07-28_wp219_material_output_states_report.md)、
[`design_reviews/2026-07-29_wp220_material_local_read_report.md`](design_reviews/2026-07-29_wp220_material_local_read_report.md)、
[`design_reviews/2026-07-29_wp226_planar_reflection_report.md`](design_reviews/2026-07-29_wp226_planar_reflection_report.md)、
[`design_reviews/2026-07-29_wp228_planar_transparent_capture_report.md`](design_reviews/2026-07-29_wp228_planar_transparent_capture_report.md)、
[`design_reviews/2026-07-29_wp229_view_family_clustered_selection_report.md`](design_reviews/2026-07-29_wp229_view_family_clustered_selection_report.md)、
[`design_reviews/2026-07-29_wp233_replaceable_render_algorithm_package.md`](design_reviews/2026-07-29_wp233_replaceable_render_algorithm_package.md)、
[`design_reviews/2026-07-29_wp234_runtime_view_family_provider_package.md`](design_reviews/2026-07-29_wp234_runtime_view_family_provider_package.md)、
[`design_reviews/2026-07-29_wp235_raster_attachment_subresource.md`](design_reviews/2026-07-29_wp235_raster_attachment_subresource.md)、
[`design_reviews/2026-07-30_wp236_runtime_cube_render_target.md`](design_reviews/2026-07-30_wp236_runtime_cube_render_target.md)、
[`design_reviews/2026-07-30_wp237_replaceable_cube_capture.md`](design_reviews/2026-07-30_wp237_replaceable_cube_capture.md)、
[`design_reviews/2026-07-30_wp238b_generic_raster_pass.md`](design_reviews/2026-07-30_wp238b_generic_raster_pass.md)、
[`design_reviews/2026-07-31_wp240c_project_material_report.md`](design_reviews/2026-07-31_wp240c_project_material_report.md)。

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
| render target | fixed/output-relative extent、format/class/role、usage、history、format candidates、fixed/full mip、array layers、2D/runtime cube resource shape、fullscreen/compute/material/raster attachment subresource view | [rendertargetjsonparser.cpp](../src/core/renderingpass/rendertargetjsonparser.cpp) の`parseDimension`/`parseMipLevels`/`parseArrayLayers` |
| MRT / MSAA | materialは版付きschemaで任意長のfloat/SINT/UINT color output、typed sample-count resolve、target別typed clear。上限はdeviceの`maxColorAttachments`だけ。fullscreenは現在1 color/pass。**raster passはmaterialを経由せず任意枚数のcolor attachment + 任意depth attachmentを直接書ける**(engine側の枚数上限なし) | [materialoutput.hpp](../src/project/materialoutput.hpp) の`MaterialOutputSchema`、[renderingpassvalidation.cpp](../src/core/renderingpass/renderingpassvalidation.cpp) の`validatePassOutputs`(fullscreen 1 colorの強制)、[rasterpass.hpp](../src/project/rasterpass.hpp) の`RasterFixedFunctionState::color_attachments` |
| fullscreen / raster input | image **および storage buffer**、image は filter/address 指定。同じ`input`語彙をraster passも使う(image=sampled、buffer=storageのみ) | [renderingpasstargetjsonparser.cpp](../src/core/renderingpass/renderingpasstargetjsonparser.cpp) の`parseInputResourcesFromJson`、[fullscreenpasscontainer.cpp](../src/core/fullscreenpass/fullscreenpasscontainer.cpp) のinput descriptor構築 |
| raster pass(WP238b) | pass type `"raster"`。`draw`(open/versionedな`implementation` + typed direct operation)、`raster_state`(topology/cull/front_face/depth test-write-compare/attachment別blend・write mask)、`shader`(versioned implementation + vertex/optional fragment)、`resource_ports`。**draw operationは`direct`一種類のみ**(G15)。CPU testと`projects/sprite_demo`の`ssao_clear`置き換えまでで、画素goldenは未作成(§4) | [rasterpass.hpp](../src/project/rasterpass.hpp) の`RasterPassContract`、[genericrasterpassinfojsonparser.cpp](../src/core/renderingpass/genericrasterpassinfojsonparser.cpp)、[renderingpassjsonhelpers.cpp](../src/core/renderingpass/renderingpassjsonhelpers.cpp) の`makePassInfo` |
| material surface | `pelican_vertex_displace_v1` / `pelican_surface_v1` / `pelican_material_outputs_v1` / `pelican_brdf_v1` / `pelican_ambient_v1` / `pelican_lighting_v1` の6 hook(`brdf`と`lighting`は排他)、custom params/texture、generated accessor | [surfaceformat.cpp](../src/project/surfaceformat.cpp) の`validateSurfaceHooks`、[surfacecompiler.cpp](../src/core/shader/surfacecompiler.cpp) の生成側 |
| project material asset | `pelican.asset_data` v1 の `materials[]`(path-only索引)から `pelican.material` v1 を起動時に lowering・shader compile・GPU登録。engine/project texture resolver、values/texture hot reload、glTF/project二つのnamed-material解決域と衝突拒否、既存`pelican.material_bindings`のrouting照合 | [projectmaterialasset.cpp](../src/core/material/projectmaterialasset.cpp) の`ProjectMaterialAssetContainer`、[projectmaterialtextureresolver.cpp](../src/core/material/projectmaterialtextureresolver.cpp)、[modeltemplate.cpp](../src/core/model/modeltemplate.cpp) の`applyPrimitiveMaterialBindings` |
| material render state | opaque/blend/**additive**、none/**front**/back cull、depth test/write/compare | [surfaceformat.cpp](../src/project/surfaceformat.cpp) の`parseRenderState` |
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
| graphics buffer input | fullscreen / raster は`input`のみのraw storage bufferに加え、`resource_ports`(buffer portは`element`必須)でtyped readonly storage bufferも宣言できる。material/geometryはtyped readonly storage bufferのみ。material write/atomicは未公開 |
| sampler | fullscreen/compute named image port は filter/address 指定可。material custom texture は filter/mip-filter/address/compare/anisotropy を typed 宣言できる。compare の実 binding は compatible depth image provider待ち |
| texture dimension | project authored KTX2 の 2D/cube/2D-array/3D、runtime 2D RTのmip/layerとsampled/storage/raster attachment部分view、runtime cube RTのface attachmentとsampled cube viewは公開済み。runtime 3D targetは未公開(G10b) |
| render state | surface単位とpass-local named variantに対応済み。同一opaque/transparent phase内で別state/surfaceを使える。phase跨ぎはvariant-aware draw queue待ち |
| pass kind | implementation provider による runtime 差し替えは fullscreen pass 限定(`passimplementationregistry.cpp` の `resolveRenderingPassImplementations` が非 fullscreen の `implementation.provider` を拒否する)。WP238b で汎用 authoring kind `"raster"` が入り、**draw operation の値と shader asset は pass JSON だけで差し替えられる**ようになった(engine 側に技法ごとの pass kind を足さずに済む)。`draw.implementation` / `shader.implementation` は backend が解決しない provenance ID で、dispatch には使われない。公開されている draw operation は `direct`(vertex/instance/first のみ)一種類で、indexed / indirect / mesh / tessellation / custom vertex stream と material・geometry contract は依然 v1 閉集合(G15 部分解消) |
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
| C3 | point/spot shadow | **✕** | named multi-view familyのsequential実行、2D-array/cube face attachment、runtime sampled cube viewは利用可能。point/spot view providerとlight shadow schemaが不足(G5/G6b) |
| C4 | PCSS / PCF | **△** | public shadow resourceとmanual depth compareは利用可能。標準filtered algorithm、comparison sampler利用、品質goldenは未作成 |
| C5 | screen-space contact shadow | **△** | fullscreen depth ray march で構成可能。実 feature/golden 未作成 |
| C6 | shadow cache / virtual shadow map | **✕** | mip/layer view、named view-family、cascade array targetは解消。page table、GPU-driven page execution、residencyが不足(G8/G9) |
| C7 | capsule shadow | **△** | 固定/baked data は可能。動的 capsule inventory の public data channel は G5 |

### D. reflection / environment

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| D1 | prebaked IBL | **○** | 2D octahedral/stripに加えてnative cubemapをmaterialから利用可能(WP209a) |
| D2 | runtime prefilter / dynamic environment | **△** | WP237でstable six-face provider、Deferred + Forward capture、family-local clustered selectionを交換可能packageとして実装し、6 face実GPU描画まで検証。公開結果は現在1 mipで、BRDF-aware prefilter、複数probeの更新/選択、main materialへのbinding policyが未完(G6b) |
| D3 | parallax-corrected reflection probe | **△** | baked texture + hook。probe selection/data は material単位または G5 |
| D4 | post SSR | **△** | scene color + depth fullscreen で構成可能。実 feature 未作成 |
| D5 | material SSR/refraction integration | **○** | typed screen input と実 Vulkan refraction test 済み |
| D6 | planar reflection | **△** | WP226〜234でper-view clip plane、Vulkan ZO oblique projection、独立解像度Deferred + Forward opaque/transparent capture、汎用family culling、view-local transparent sort、ViewFamily-local clustered selection、7-level low-pass mip生成、family-array material sampling、provider/shader差替えと標準package purgeを実Vulkan実装。BRDF-aware prefilter、複数plane/probe、secondary multiviewは未完(G6b) |
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
| H1 | GPU culling / depth pyramid | **culling △ / pyramid ○** | 2-layer/full-chain RTのmip0→mip1 computeとfullscreen表示を実GPU dogfood済み(WP209b)。固定状態1 rangeのdraw args/count接続はWP210b、project-owned shaderによるdepth-pyramid occlusion compactionはWP210c、複数`material_range`を選べる`draw_queue_segments_v1`はWP210d、XR per-view実行はWP210fで解消。残るのはsegmentごとのpipeline/material/vertex layoutがCPU binding固定であること(GPU側でstateを選べない、G8) |
| H2 | occlusion culling | **△** | GPU visibilityからrenderer draw count/argsへ接続でき、depth-pyramid判定のproject dogfood(WP210c)とmulti-segment(WP210d)は実GPU fixtureで解消済み。segmentはCPUが発行したfixed-state rangeの範囲内に限られ、GPU-visible state keyは未公開(G8) |
| H3 | LOD switching | **△** | game側model swapは可。標準LOD policyなし |
| H4 | impostor | **△** | asset generation外部、runtime selectionはH3 |
| H5 | mesh shader / meshlet | **✕** | shader stage/pipeline と GPU-driven contract が無い(G8/G9/G15)。WP238b の `"raster"` kind でも公開 operation は direct draw のみで、task/mesh stage は入っていない |
| H6 | tessellation / displacement stage | **✕** | tessellation stage authoring が無い(G15)。`"raster"` kind の `raster_state` は topology/cull/front_face/depth までで、tessellation stage も vertex input も持たない |
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
| I4 | water reflection | **△** | SSRに加え、WP226〜234の標準planar reflection target/resource port、Deferred + Forward opaque/transparent capture、roughness用mip列とtyped family-array samplerを利用可能。prefilter shaderとcamera providerをproject実装へ差し替え、標準asset/C++ packageをpurge可能。水面のFresnel・法線歪み・BRDF-aware convolutionはproject/material側の置換algorithm |
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

v21 で全 18 項目を `5a4f95e` 時点の実装へ再照合した。**判定が動いたのは G8 と G15 の 2 項目だけ**で、
新設が必要なギャップは見つからなかった。WP238a(共通`FrameExecutionPlan`)、WP238c(complete
physical plan)、WP238d(`NativeScope` executor)、WP238e(`NativeScope` Vulkan fixture)は
engine の source を書く人向けの拡張であり、
§0 の「`src/` を変更せず project の feature/material/shader だけで完結できるか」という判定規則では
ユーザー空間の機構に当たらないため、G としては立てない。解消済み項目は §2 の 84 行からは ID
参照されていない(§2 が挙げるのは G5 / G6b / G7 / G8 / G9 / G10b / G11 / G14 / G15 だけ)が、
判定履歴として削除せず残す。

| ID | 正確なギャップ | 主な対象 |
|---|---|---|
| **G1（解消済み、WP207a）** | compute pipelineへFrame/Light setとnamed sampled/storage image portを実装。fullscreenも同じgenerated interfaceを使う | clustered compute、DDGI、runtime LUTの入力境界 |
| **G2（解消済み、WP207b）** | material vertex/fragmentへtyped readonly buffer/sampled image port、element/stage schema、generated accessor、世代固定descriptorを実装 | FFT ocean、GPU simulation consumer。PPLL write/storage imageは別拡張 |
| **G3（解消済み、WP210a）** | typed command buffer、GPU-written indirect dispatch、自動read edge/barrierを実装 | GPU culling、adaptive work |
| **G4（解消済み、WP209a）** | project-owned KTX2の2D/cube/2D-array/3D、generated accessor、reflection/runtime view照合を実装 | native IBL、3D noise/LUT |
| **G5** | light/custom scene data schemaがdir/point/spotと固定上限中心。[light.hpp](../src/core/light/light.hpp) の`LightUBO`が`MAX_DIRECTIONAL_LIGHTS`/`MAX_POINT_LIGHTS`/`MAX_SPOT_LIGHTS`の固定長配列で、shapeやorientation/size、lightごとのresource index(cookie/IES)のfieldが無い(WP208のscalable inventoryはclustered selection側の別contract) | many lights、area/cookie/IES、capsule |
| **G6a（解消済み、WP205）** | public directional shadow resource/light relation、generated `pelican_shadow()`、project-copy同値とpurgeを実装 | filtered PCF/PCSSは品質algorithm側の残件 |
| **G6b（部分解消: WP223〜237）** | stable runtime ViewFamily、pass/task relation、secondary sequential scheduling、family固有extent、directional CSM、planar reflection、oblique near-plane、汎用secondary culling、family別transparent sort、family-local clustered selection、planar mip filter/family-array sampling、stable-ID provider registry、標準shader/C++ package差替え・purge、cube-compatible target/face attachment/sampled cube view、交換可能なsix-face reflection captureを実装。secondary multiview、point/spot shadow provider、複数probe policyが未完 | point/spot shadow、reflection probe、advanced planar/cube capture |
| **G7** | acceleration structure / RT shader/pipeline contractが無い。`src/`でacceleration structureに触れるのは[spvlink.cpp](../src/core/shader/spvlink.cpp) の`SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`を文字列化する1箇所だけで、AS構築もRT pipelineも存在しない | K1/K2 |
| **G8（部分解消、WP210b〜210g）** | 固定状態1 material rangeのGPU-written indexed draw/count(WP210b)、project-owned depth-pyramid occlusion dogfood(WP210c)、`draw_queue_segments_v1`による複数`material_range` segment(WP210d)、segmented graphのhot reload(WP210e)、XR per-view実行(WP210f)、break-even計測(WP210g)まで実装・実Vulkan検証済み。**未完はGPU-visible state key**で、segmentごとのpipeline/material/vertex layoutはCPU bindingのまま固定される。GPUがstateまで選ぶ経路、およびresidency/streamingは無い | culling、particles、virtual geometry |
| **G9** | bindless/descriptor indexing contractが無い。`descriptorIndexing` / `bindless` / `VARIABLE_DESCRIPTOR_COUNT` / `PartiallyBound` は`src/`に1件も無い | large resource tables、RT/virtualized workload |
| **G10a（解消済み、WP209b）** | 2D runtime RTのfixed/full mip、array layer、fullscreen/compute sampled/storage subresource viewを実装 | depth pyramid、2D runtime LUT |
| **G10b（部分解消、WP235〜236）** | raster attachmentの任意1 mip/連続layer出力とruntime cube image、face attachment、sampled cube viewを実装。runtime 3D image/viewは未公開で、[rendertargetjsonparser.cpp](../src/core/renderingpass/rendertargetjsonparser.cpp) の`parseDimension`が`Render target dimension must be '2d' or 'cube'`として`3d`を拒否する | froxel、runtime 3D LUT |
| **G11** | VRS/fragment density/foveation backend contractが無い。`fragmentDensity` / `shadingRate` / `foveat*` は`src/`に1件も無い | XR foveation |
| **G12（authoring解消済み、WP209a）** | material samplerのfilter/mip-filter/address/compare/anisotropyを実装。hardware compareの実利用はcompatible depth image provider待ち | hardware PCF、special filtering |
| **G13（解消済み、WP206b）** | pass-local named surface/render-state variantを実装。同一phase routeに対応し、opaque/transparent phase跨ぎだけをvariant-aware draw queueへ残す | duplicate-free inverted hull、special overlay |
| **G14** | material-owned stable tag/filterは実装済みだが、instance/draw-owned tag/layerが未公開。[drawqueuebuilder.hpp](../src/core/renderer/drawqueuebuilder.hpp) が持つのは`material_tags`だけで、instance側にtag fieldが無い | 同じmaterialを共有するinstanceの個別SSS/outline/decal/reflection |
| **G15（部分解消、WP238b）** | 汎用authoring kind `"raster"`が入り、draw operationの値とshader assetをpass JSONだけで差し替えられるようになった。`draw.implementation` / `shader.implementation`は[rasterpass.hpp](../src/project/rasterpass.hpp) が`The backend consumes operation, not this identifier.`と書くprovenance IDで、backendは解決しない。`implementation.provider`によるruntime差し替えも[passimplementationregistry.cpp](../src/core/renderingpass/passimplementationregistry.cpp) の`resolveRenderingPassImplementations`がfullscreen passのみ受理する。attachment枚数にengine側上限は無く、portableなtopology/cull/front_face/depth/blend/write-maskを宣言できる。**未完はdraw operationの語彙**で、公開されているのは`direct`(vertex/instance/first)一種類だけである(`RasterDrawOperation = std::variant<RasterDirectDrawOperation>`)。indexed / indirect+count / mesh・task stage / tessellation stage / custom vertex stream、およびmaterial・geometry pass contract自体は依然v1閉集合 | custom geometry stage/output |
| **G16** | stencil state/resource semanticsが未公開。[pipelinefactory.cpp](../src/core/shader/pipelinefactory.cpp) が`depth.stencilTestEnable = false`を固定しており、`.surface`にもpassにもstencil経路が無い。WP238bの`raster_state`もdepthまでで、stencilは含まない | stencil mask/portal |
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
3. ~~compute result → material vertex displacement~~（WP207bで実GPU dogfood済み。
   `test/headless_render_test.cpp` の`[headless][render][material-resource][wp207b]`ケースが
   compute出力をvertex stageのtyped buffer portから読んで頂点を動かしている）
4. ~~scalable light inventoryを使う raster/compute clustered lighting~~（WP208で実GPU dogfood済み）
5. ~~static native cubemapを使うmaterial sampling~~（WP209aで実GPU dogfood済み）
6. ~~authored mip/layer viewを使う depth pyramid~~（WP209bでlayer 1上の実GPU dogfood済み）
7. `"type": "raster"` pass の画素 golden。WP238b は portable contract / parser / Vulkan adapter /
   frame plan の CPU test と `projects/sprite_demo` の`ssao_clear`置き換えまでで、
   **headless Vulkan の画素・buffer 検証はまだ無い**。上の判定手順 2 を満たしていないので、
   §1-1 では「露出している機構」として扱い、技法判定を ○ へ上げる根拠には使わない

## 5. 実装順

詳細な監査と受け入れ条件は
[`2026-07-26_render_capability_authoring_audit_codex.md`](design_reviews/2026-07-26_render_capability_authoring_audit_codex.md)
および [`implementation_plan.md`](implementation_plan.md) の WP205+ 候補を正とする。

推奨順は次である。

1. WP204 runtime sliceは完了
2. G6a public shadow contractはWP205、directional CSMはWP225、planar reflectionと汎用secondary cullingはWP226、Forward opaque captureはWP227、transparent capture/sortはWP228、family-local clustered selectionはWP229、oblique near-planeはWP230、extent-derived dispatch/material remaining-mip/family-array planar filterはWP231〜232、filter asset差替えはWP233、runtime family provider registryと標準C++ package purgeはWP234、raster attachment mip/layer viewはWP235、runtime cube target/viewはWP236、交換可能なsix-face captureはWP237で完了。G6bの次はcube prefilter/probe policy、point/spot shadow provider、またはsecondary multiview
3. material-owned G14はWP206a、G13の同一phase variantはWP206bで完了。G15はWP238bの汎用`raster` kindで部分解消し、次はdraw operation語彙の拡張(indexed / indirect+count)を具体workloadとbackend verifierが揃った時点で行う。instance/draw-owned G14とphase跨ぎqueueは実需要時に拡張
4. G1はWP207a、G2はWP207bで完了
5. G5 lighting data v2 + clustered dogfoodはWP208で完了
6. G4とG12 authoringはWP209a、G10aはWP209bのdepth pyramidで完了。G10bのraster subresourceはWP235、runtime cubeはWP236で部分解消し、残るruntime 3Dは実需要時
7. G8 fixed-state GPU-written draw arguments/countはWP210b、実occlusion dogfoodはWP210c、
   segment metadataはWP210d、hot reloadはWP210e、XR per-viewはWP210f、break-even計測は
   WP210gで完了。次はGPU-visible state key(GPU側でpipeline/materialを選ぶ経路)で、
   G9 bindlessと同じく実測需要時
8. delivery laneとして`dist-bake`
9. G11 VRSはQuest device gate後

surface render stateとfullscreen buffer inputは既に実装済みなので、再実装WPを作らない。
