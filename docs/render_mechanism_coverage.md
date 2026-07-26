# 描画機構カバレッジ分析: 何がユーザー空間で書けて、何が書けないか(v7)

対象読者: エンジン担当、および feature / material / shader をユーザー空間で書く人。

ステータス: **v7(2026-07-26)**。84 行の技法候補を現行コードとテストへ再照合し、
v2 の件数不整合、G2/G13、material screen input、sampler/texture dimension、
raster tiled lighting の誤判定を訂正した。v4のWP206a stable tag selectionに続き、
v5ではWP206bのpass-local named variantとinverted-hull dogfood、v6ではWP207aの
compute Frame/Lightとfullscreen/compute named image port、v7ではWP207bのmaterial
vertex/fragment typed buffer/image consumerとcompute→vertex displacement dogfoodを反映した。
instance/draw-owned layerとopaque/transparent phaseを跨ぐvariant queueは後続である。監査記録は
[`design_reviews/2026-07-26_render_capability_authoring_audit_codex.md`](design_reviews/2026-07-26_render_capability_authoring_audit_codex.md)。

## 0. 判定規則

エンジンの方針は「機構はエンジン、技法はユーザー空間」である。ここで問うのは
**技法名を engine enum に持っているか**ではなく、次の条件を満たすかである。

> `src/` を変更せず、project の feature / material / shader だけで動作する実装を
> 完結できるか。

| 判定 | 意味 |
|---|---|
| **○** | 実装済み、または既存の直接的な public hook/input だけで完結する |
| **△** | 回避策、固定上限、外部 bake、未 dogfood の組み合わせが必要 |
| **✕** | 下記 G1〜G16 のうち未解消の具体的な機構不足がある |

単に「GLSL なら計算式を書ける」は ○ の根拠にしない。複数技法を一行にまとめた項目は
個別判定を併記するため、○/△/✕の単純合計を優先順位に使わない。

## 1. 現在ユーザー空間へ露出している機構

### 1-1. 実装・テスト済み

| 機構 | 現在の契約 | 根拠 |
|---|---|---|
| render target | fixed/output-relative extent、format/class/role、usage、history、format candidates | [rendertargetjsonparser.cpp](../src/core/renderingpass/rendertargetjsonparser.cpp) |
| MRT / MSAA | material/fullscreen の複数 color、typed sample-count resolve | [renderingsamplecount.cpp](../src/core/renderingpass/renderingsamplecount.cpp) |
| fullscreen input | image **および storage buffer**、image は filter/address 指定 | [fullscreenpasscontainer.cpp:281](../src/core/fullscreenpass/fullscreenpasscontainer.cpp) |
| material surface | `displace` / `surface` / `brdf` / `lighting`、custom params/texture、generated accessor | [surfacecompiler.cpp:55](../src/core/shader/surfacecompiler.cpp) |
| material render state | opaque/blend/**additive**、none/**front**/back cull、depth test/write/compare | [surfaceformat.cpp:530](../src/project/surfaceformat.cpp) |
| material selection | material `tags` + pass `material_filter.include/exclude`。compile済みcompact range、flat/preview/XR共通 | [drawqueuebuilder.cpp](../src/core/renderer/drawqueuebuilder.cpp) |
| material screen input | `opaque_color` / depth / linearized depth、opaque snapshot、XR layered binding | [headless_render_test.cpp:2444](../test/headless_render_test.cpp) |
| material graph resource | `.surface`のvertex/fragment typed readonly buffer・sampled image port、generated accessor、pass mapping、世代固定descriptor | [surfacecompiler.cpp](../src/core/shader/surfacecompiler.cpp) |
| compute | fixed dispatch、storage buffer/image、reads/writes/after/before | [computetask.cpp](../src/core/renderingpass/computetask.cpp) |
| temporal | history、velocity、projection jitter、reset epoch、解像度分離 | [taa.json](../src/core/resources/features/taa.json) |
| graph/compiler | logical type/value、target planning、physical fragment、transient/tile-local/alias runtime | [design_render_graph_compiler.md](design_render_graph_compiler.md) |
| XR | sequential/multiview、内部 2D-array image/view、array swapchain/depth submit | [design_openxr.md](design_openxr.md) |

### 1-2. 境界を正確に言う必要があるもの

| 項目 | 現在の境界 |
|---|---|
| graphics buffer input | fullscreen はraw storage buffer、material/geometryはtyped readonly storage bufferに対応。material write/atomicは未公開 |
| sampler | fullscreen/compute named image port は filter/address 指定可。material custom texture は固定 sampler、compare/anisotropy 未公開(G12) |
| texture dimension | XR 内部 array は存在。project authored static cube/array/3D と RT mip/layer/subresource view が未公開(G4/G10) |
| render state | surface単位とpass-local named variantに対応済み。同一opaque/transparent phase内で別state/surfaceを使える。phase跨ぎはvariant-aware draw queue待ち |
| pass kind | implementation provider は差し替え可。authoring kind と material contract は v1 閉集合(G15) |
| object selection | material単位は安定tag/filter対応済み。instance/draw-owned tag/layerは未公開(G14)。`material_range`はlegacy draw-call ordinal |

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
| B1 | tiled/clustered lighting(raster) | **△** | fullscreen raster 自体は可。標準 light 32灯、format/consumer の実 dogfood 未完(G5) |
| B2 | tiled/clustered lighting(compute) | **△** | compute→material typed consumerは実装済み。scalable light inventoryとclustered dogfoodが不足(G5) |
| B3 | area light(LTC) | **△** | LUT 評価は可。light schema に shape/orientation/size が無い(G5) |
| B4 | light cookie / IES | **△** | texture は可。lightごとのresource indexが無い(G5/G9) |
| B5 | custom non-shadow attenuation | **○** | `lighting` hook |
| B6 | auto exposure / histogram | **△** | downsample は可。storage image/buffer histogram は構成可能だが実 feature 未検証 |
| B7 | prebaked light probe(SH) | **△** | texture/params 持込み可。placement/bake は外部 |
| B8 | dynamic GI(DDGI / SSGI) | **SSGI △ / DDGI ✕** | SSGI は fullscreen 候補。DDGIのcompute image inputは解消したが、3D/static arrayとprobe inventoryが不足(G4/G5) |
| B9 | lightmap | **△** | texture は可。標準 geometry の第2 UV 契約を dogfood する必要あり |

### C. shadow

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| C1 | B-layer material shadow reception | **✕** | `pelican_shadow()` が 1.0(G6a) |
| C2 | cascaded shadow map | **✕** | public shadow contract と任意 view family が無い(G6a/G6b) |
| C3 | point/spot shadow | **✕** | shadow contract、view、cube/array、light schema(G4/G5/G6a/G6b) |
| C4 | PCSS / PCF | **✕** | shadow resource が未公開(G6a)。manual compare は可能なので comparison sampler 自体は必須条件ではない |
| C5 | screen-space contact shadow | **△** | fullscreen depth ray march で構成可能。実 feature/golden 未作成 |
| C6 | shadow cache / virtual shadow map | **✕** | view、mip/layer、residency(G6b/G8/G10) |
| C7 | capsule shadow | **△** | 固定/baked data は可能。動的 capsule inventory の public data channel は G5 |

### D. reflection / environment

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| D1 | prebaked IBL | **○** | 2D octahedral/strip texture で実装可能。native cubemap は G4 |
| D2 | runtime prefilter / dynamic environment | **✕** | capture view と authored mip/subresource view が無い(G6b/G10) |
| D3 | parallax-corrected reflection probe | **△** | baked texture + hook。probe selection/data は material単位または G5 |
| D4 | post SSR | **△** | scene color + depth fullscreen で構成可能。実 feature 未作成 |
| D5 | material SSR/refraction integration | **○** | typed screen input と実 Vulkan refraction test 済み |
| D6 | planar reflection | **✕** | reflection camera/view family を pass に供給できない(G6b) |
| D7 | hybrid RT reflection | **✕** | acceleration structure/RT pipeline と descriptor path が無い(G7/G9) |

### E. volumetric / atmosphere

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| E1 | analytic height fog | **○** | fullscreen + depth |
| E2 | radial god ray | **○** | fullscreen chain |
| E3 | froxel volumetric fog | **△** | 2D atlasなら可能。native 3D image path は G4/G10 |
| E4 | ray-marched cloud | **△** | fullscreen 可。3D noise は 2D atlas/外部 bake |
| E5 | physical atmosphere(Bruneton) | **△** | LUT 持込み可。compute image inputは解消したがruntime 3D LUTはG4 |

### F. post process

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| F1 | bloom | **○** | bundled/example 実装 |
| F2 | TAA | **○** | bundled feature 実装 |
| F3 | temporal upscale | **○** | history/velocity/jitter と render/output resolution 分離を実装済み。品質algorithmはuser-space |
| F4 | SSAO/HBAO | **○** | example 実装 |
| F5 | GTAO + bent normal | **△** | AO は可。G-buffer/lighting consumer の追加が必要 |
| F6 | depth of field | **△** | fullscreen/downsample で構成可能。実 feature 未作成 |
| F7 | motion blur | **△** | velocity は実装済み。blur feature 未作成 |
| F8 | LUT color grading | **○** | 2D strip LUT で可能。3D LUT は G4 |
| F9 | chromatic aberration/vignette/grain/flare | **○** | fullscreen |
| F10 | sharpen(CAS class) | **○** | fullscreen |
| F11 | depth/normal post outline | **△** | fullscreen で構成可能。標準 dogfood 未作成 |

### G. transparency / special draw

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| G1t | sorted transparency | **○** | `forward_transparent_v1` + draw sort provider |
| G2t | inverted-hull outline | **○** | WP206bのproject-owned surface/pass dogfoodで、entity/mesh/base material/draw複製なしにfront-cull forward overlayを実GPU描画 |
| G3t | order-independent transparency | **weighted △ / PPLL ✕** | weighted方式もmultipass/MRT routeのdogfoodが必要。PPLLはfragment write/atomic、descriptor table、custom geometry contract不足(G9/G15) |
| G4t | stochastic transparency | **△** | dither/mask は可。TAA integration の品質調整が必要 |
| G5t | refraction/glass | **○** | material screen input 実配線・描画済み |
| G6t | additive effect | **○** | `.surface render_state.blend: additive` 実装・test済み |
| G7t | deferred decal | **△** | ping-pong/fullscreenとmaterial tag選別は可。instance単位選別とG-buffer更新方式にG14/G15 |

### H. geometry / performance

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| H1 | GPU culling / depth pyramid | **culling ✕ / pyramid △** | pyramidは別RT連鎖で可。GPU draw argsはG8、mip view方式はG10 |
| H2 | occlusion culling | **✕** | GPU visibilityからdraw count/argsへ接続できない(G3/G8) |
| H3 | LOD switching | **△** | game側model swapは可。標準LOD policyなし |
| H4 | impostor | **△** | asset generation外部、runtime selectionはH3 |
| H5 | mesh shader / meshlet | **✕** | shader stage/pipeline と GPU-driven contract が無い(G8/G9/G15) |
| H6 | tessellation / displacement stage | **✕** | tessellation stage authoring が無い(G15) |
| H7 | virtual geometry | **✕** | streaming/residency/GPU-driven/bindless(G8/G9) |
| H8 | GPU particle | **△** | fixed maximum countならcompute+draw可能。GPU chosen draw countはG8 |
| H9 | vegetation wind / mass draw | **△** | windは○。large-scale GPU cullingはG8 |
| H10 | terrain clipmap | **△** | mesh policyはgame側。height displacementは可 |

### I. water / environment

| # | 技法 | 判定 | 根拠・制約 |
|---|---|---|---|
| I1 | Gerstner wave | **○** | `displace` hook |
| I2 | FFT ocean | **○** | compute buffer→material vertex displacementとfragment sampled imageを実GPU dogfood済み。FFT実装自体はproject shader |
| I3 | water refraction | **○** | material screen input |
| I4 | water reflection | **△** | SSRなら可能。planar reflectionはG6b |
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
| **G3** | authored dispatchが定数、indirect dispatchが無い | GPU culling、adaptive work |
| **G4** | static cube/array/3D texture dimensionをprojectから宣言できない | native IBL、3D noise/LUT |
| **G5** | light/custom scene data schemaがdir/point/spotと固定上限中心 | many lights、area/cookie/IES、capsule |
| **G6a** | public shadow resource/light relationが無く`pelican_shadow()`がstub | material shadow、PCF/PCSS |
| **G6b** | passへ任意のcamera/view familyを供給できない | CSM、point shadow、planar reflection |
| **G7** | acceleration structure / RT shader/pipeline contractが無い | K1/K2 |
| **G8** | GPUがrendererのdraw count/indirect argsを書けない | culling、particles、virtual geometry |
| **G9** | bindless/descriptor indexing contractが無い | large resource tables、RT/virtualized workload |
| **G10** | authored RT mip/layer/subresource viewが無い | depth pyramid、runtime IBL、froxel |
| **G11** | VRS/fragment density/foveation backend contractが無い | XR foveation |
| **G12** | material samplerのaddress/filter/compare/anisotropy authoringが無い | hardware PCF、special filtering |
| **G13（解消済み、WP206b）** | pass-local named surface/render-state variantを実装。同一phase routeに対応し、opaque/transparent phase跨ぎだけをvariant-aware draw queueへ残す | duplicate-free inverted hull、special overlay |
| **G14** | material-owned stable tag/filterは実装済みだが、instance/draw-owned tag/layerが未公開 | 同じmaterialを共有するinstanceの個別SSS/outline/decal/reflection |
| **G15** | material/geometry pass contractがv1閉集合 | custom geometry stage/output |
| **G16** | stencil state/resource semanticsが未公開 | stencil mask/portal |

## 4. dogfood の扱い

v2 の raster tiled lighting例は `R32G32B32A32_UINT` がformat parserに無く、
標準light inventoryも32灯なので、動作証明になっていなかった。今後は次の順で判定する。

1. project-owned feature/surface/shaderだけで最小fixtureを書く。
2. compile/plan dumpだけでなく、headless Vulkanの画素またはbuffer結果を検証する。
3. flat、preview、sequential XR、multiview、hot reloadのうち影響範囲を回帰する。
4. 成功して初めて○へ上げる。止まった箇所はfile:lineとtyped contractを記録する。

既にこの条件を満たす代表例は、compute buffer → fullscreen、material refraction、
additive/front/depth surface state、TAA、MSAA、upscale resolution contractである。

次の dogfood は以下を推奨する。

1. public directional shadow reception
2. ~~実装済みtag選択 + pass-local variantによる inverted-hull outline~~（WP206bで実GPU dogfood済み）
3. compute result → material vertex displacement
4. scalable light inventoryを使う raster/compute clustered lighting
5. authored mip/layer viewを使う depth pyramid

## 5. 実装順

詳細な監査と受け入れ条件は
[`2026-07-26_render_capability_authoring_audit_codex.md`](design_reviews/2026-07-26_render_capability_authoring_audit_codex.md)
および [`implementation_plan.md`](implementation_plan.md) の WP205+ 候補を正とする。

推奨順は次である。

1. 現在の WP204 runtime slice を閉じる
2. G6a public shadow contract
3. material-owned G14はWP206a、G13の同一phase variantはWP206bで完了。必要なdogfoodでG15、instance/draw-owned G14とphase跨ぎqueueは実需要時に拡張
4. G1はWP207a、G2はWP207bで完了
5. 次はG5 lighting data v2 + clustered dogfood
6. G4/G10/G12 texture dimension/subresource/sampler
7. G3/G8 GPU-driven execution。G9 bindlessは実測需要時
8. delivery laneとして`dist-bake`
9. G11 VRSはQuest device gate後

surface render stateとfullscreen buffer inputは既に実装済みなので、再実装WPを作らない。
