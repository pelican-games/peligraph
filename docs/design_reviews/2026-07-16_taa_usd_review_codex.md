# TAA / projection jitter v1 + USD / OpenPBR v1 敵対レビュー

レビュー日: 2026-07-16  
対象: `docs/design_taa_jitter.md` v1、`docs/design_usd_openpbr.md` v1

## 冒頭判定

- `docs/design_taa_jitter.md` v1: **Reject**
- `docs/design_usd_openpbr.md` v1: **Reject**

TAA 案は、確定済みの「カメラ API から分離したレンダー側 modifier」という方向自体には従っている
(`docs/design_postprocess_temporal.md:99-115`)。しかし、規範式が現行の透視投影では符号逆、直交投影では
要素自体が誤りである。さらに scene の主描画は FrameUBO の projection を使っておらず、標準 feature として
提示された一パス二出力 RMW は現行 frame graph が明示的に禁止する。設計どおりに実装しても TAA にならない。

USD 案も「USD は source lane、runtime は glTF 等の閉じた集合だけを読む」という二層方針には適合する
(`docs/design_asset_format_policy.md:15-30`)。しかし、OpenPBR の出力形式が現行 `pelican.material` では表現不能で、
出力した外部 material を GLB primitive に結び付ける経路もない。加えて候補の `guc` は USD→glTF ツールではなく
逆方向の glTF→USD ツールである。これは比較軸の不足ではなく候補集合の事実誤認である。

---

# 1. `docs/design_taa_jitter.md` v1

## 1.1 判定: **Reject**

### TAA-1 [Blocker] §1-3 の投影式は透視・直交の両方で誤っている

現行 camera は透視に `glm::perspectiveRH_ZO`、直交に `glm::orthoRH_ZO` を使う
(`src/core/renderer/camera.cpp:471-482`)。GLM の `P[column][row]` 規約で、任意の入力頂点に対して
透視除算後へ定数 `d=(dx,dy)` を加えるには、clip 座標で

```text
clip'.x = clip.x + dx * clip.w
clip'.y = clip.y + dy * clip.w
```

としなければならない。従って一般式は全列 `c` について

```text
P'[c][0] = P[c][0] + dx * P[c][3]
P'[c][1] = P[c][1] + dy * P[c][3]
```

である。現行の RH perspective では `P[2][3] = -1` なので、変更対象を一要素へ簡約するなら
`P'[2][0] -= dx`、`P'[2][1] -= dy` である。設計の `+=` は NDC を指定方向と逆へ動かす
(`docs/design_taa_jitter.md:57-60`)。

直交では `clip.w=1` なので変更すべきなのは translation column の
`P[3][0] += dx`、`P[3][1] += dy` である。設計どおり `P[2][0/1]` を変えると、定数移動ではなく
view-space z に依存する shearing になる。Vulkan の ZO depth range はこの xy の導出を変えない。

Y の規範も閉じていない。現行 viewport は正の height を使う
(`src/core/vkcore/render_pass_frame_setup.cpp:73-76`)ため、`jy` の正方向を「画像上向き」「framebuffer
下向き」のどちらにするかを明記し、CPU の offset、`jitter_ndc`、shader sampling の三者で同じ符号にしなければ
ならない。「Vulkan 規約に合わせて実装で確定」は設計の空欄であって規範ではない。

### TAA-2 [Blocker] 「scene raster の FrameUBO projection だけ」という適用点は現行描画経路に存在しない

FrameUBO は CPU/GLSL とも一個の固定 layout で、projection/current previous を一組だけ持つ
(`src/core/renderer/frameresources.hpp:11-30`,
`src/core/resources/shaders/include/pelican_frame.glsl:6-15`)。renderer は frame 冒頭で camera の行列を一度取得し、
一度だけ FrameResources を更新する (`src/core/vkcore/renderer.cpp:913-920`)。その descriptor は material、shadow、
velocity に共通で bind される (`src/core/renderer/materialrender.cpp:52-57`,
`src/core/renderer/materialrender.cpp:84-94`, `src/core/renderer/materialrender.cpp:100-115`)。

より致命的なのは、scene の主 material vertex shader が FrameUBO projection を使っていないことである。
material renderer は `camera.getVPMatrix()` を push constant で渡し
(`src/core/renderer/materialrender.cpp:57-60`)、標準 shader は `pelicanPush.engineMvp` を使う
(`src/core/resources/default.vert:22-28`)。従って FrameUBO の projection だけを jitter 済みにしても、主描画は
非 jitter のままである。「現行シェーダは変更なしで動く」
(`docs/design_taa_jitter.md:80-81`)は成立しない。

一方 shadow は light VP の push constant を使うので、FrameUBO を jitter 済みにしても直接は変わらない
(`src/core/renderer/materialrender.cpp:91-94`)。この偶然を pass 境界の契約とみなしてはならない。
`output_transform` より前という anchor の位置は、pass が camera projection を何の意味で消費するかを表さない。
主 material、velocity、SSAO、world debug、sprite、shadow、UI、fullscreen の各 consumer を列挙し、
render-frame snapshot と各 draw のどちらで jittered/non-jittered を選ぶかを決める必要がある。少なくとも
「一個の共有 UBO の同じ field を pass ごとに違う値にする」ことはできない。

### TAA-3 [Blocker] velocity の変更箇所と単位が実コードに一致しない

`velocity.vert` と `velocity_skinned.vert` は current/previous clip 座標を出すだけで、NDC 除算をしない
(`src/core/resources/velocity.vert:10-16`, `src/core/resources/velocity_skinned.vert:13-21`)。NDC 除算と
velocity 出力は共通の fragment shader にある
(`src/core/resources/velocity.frag:7-10`)。従って「両 `.vert` の NDC 変換後へ二行追加」
(`docs/design_taa_jitter.md:82-87`)という変更記述は、存在しないコード位置を指している。

現行 `outVelocity` は NDC 差へ `0.5` を掛けた UV 差である。jitter を NDC で公開するなら、共通 frag で current /
previous NDC から引いてから `*0.5` するか、vertex 側で clip.xy へ符号込みの `jitter_ndc * clip.w` を適用するかを
一つに決める必要がある。加えて FrameUBO の CPU/GLSL layout を同時更新し、`offsetof` と `sizeof` の ABI gate を
更新しなければならない (`src/core/renderer/frameresources.hpp:22-30`)。

zero subtraction は数学的には同値だが、「byte 不変」は shader binary の同一性ではなく capture の同一性として
定義すべきである。jitter off の rendered golden を byte 比較することは妥当だが、shader を変更しながらバイナリまで
同一とは言えない。

### TAA-4 [Blocker] §2 の一パス TAA は現行 feature/frame-graph 契約で表現できない

設計は同じ pass で `scene_color` を read/write し、さらに `scene_color` と `taa_accum` の二つへ出力する
(`docs/design_taa_jitter.md:100-120`)。現行 validator は非 history target の同一 pass read/write を明示的に拒否する
(`src/core/renderingpass/renderingpassvalidation.cpp:18-29`)。fullscreen pass の color output はちょうど一個でなければ
ならない (`src/core/renderingpass/renderingpassvalidation.cpp:151-163`)。history の「前面を読み、現面へ書く」自体は
二面で実装済み (`src/core/renderingpass/rendertargetcontainer.cpp:133-166`)だが、それは scene color RMW と二出力を
合法化しない。

「bloom 連鎖の前例」も RMW の前例ではない。example の bloom は `lit_color` を読み別 RT へ出し
(`projects/example/passes/main_rendering_config.json:181-190`)、最後も `lit_color` と bloom RT を読んで swapchain へ出す
(`projects/example/passes/main_rendering_config.json:317-326`)。入力と出力は別物である。

depth read にも契約が足りない。fullscreen input は `SAMPLED` usage 必須
(`src/core/renderingpass/renderingpassvalidation.cpp:54-63`)だが、example の `offscreen_depth` は
`DEPTH_STENCIL_ATTACHMENT` しか持たない
(`projects/example/passes/main_rendering_config.json:39-45`)。feature override で usage は追加できる
(`src/project/featurecompose.cpp:686-739`)ものの、設計の `scene_color` / `depth` は実 target 名でも canonical alias でもない。
stdlib feature がプロジェクト固有名をどう解決するかが未定義である。

結論として、既存機構だけで実装するなら最低でも resolve と copy/composite の二パスが必要である。resolve は
scene source + velocity + depth + `taa_accum@history` を読み `taa_accum` 一個へ書き、次 pass が current
`taa_accum` を読み downstream の別 target へ書く。あるいは engine 側へ MRT/RMW を新設する必要があり、後者なら
「特権なし stdlib」という前提を撤回することになる。

### TAA-5 [Blocker] `frame_index` では resize / set_time 後の履歴無効を判定できない

history 規約は resize と set_time で履歴をリセットするとしている
(`docs/design_postprocess_temporal.md:46-53`)。一方 EngineTime の `setTime()` は時刻と delta だけを変え、
`frame_index` は変えない (`src/core/appflow/enginetime.cpp:29-53`)。resize 時も camera history は invalidated されるが、
frame index はそのままである (`src/core/vkcore/renderer.cpp:908-920`)。従って
「履歴無効は既存 frame_index で知る」 (`docs/design_taa_jitter.md:119-121`)では、初回以外の reset を識別できない。
clear color を履歴サンプルとして blend すれば、reset 直後に黒が混入する。

FrameUBO 又は pass parameter に明示的な `history_valid` / `temporal_reset_epoch` を公開し、resize、set_time、
camera discontinuity、feature 初回有効化で同じ invalidation 経路を通す必要がある。previous projection と
previous jitter も、その経路で current と一致させなければならない。

### TAA-6 [Blocker] J1 gate は本文中で自己矛盾している

J1 表は「jitter 有効・TAA なしで golden 不変」を要求しながら、同じ cell の括弧内で「off が既定なので off 時の
byte 不変」と言い換えている (`docs/design_taa_jitter.md:140-145`)。jitter on は raster sample 位置を変える機能なので、
TAA がなくても edge coverage、texture sampling、depth が変わる。既存画像の不変を要求してはならない。

正しい gate は (a) provider 無し/default off で既存 golden byte 一致、(b) provider on/TAA off は新設した deterministic
jitter-only fixture が規範値と一致、(c) provider on/TAA on は N-frame TAA golden と一致、の三分割である。

## 1.2 再提出時に J1 / T-TAA へ逐語添付すべき条件

以下を満たすまでは再判定も **Reject** とする。

### J1（機構）

1. **J1-MATH**: 「jitter 適用は `clip'.xy = clip.xy + jitter_ndc * clip.w` を規範とする。GLM
   `P[column][row]` では全列 `c` について row 0/1 へ row 3 の jitter 倍を加える。現行
   `perspectiveRH_ZO` と `orthoRH_ZO` の双方について CPU 単体テストを置き、複数 z の点が同じ NDC delta になることを
   検査する。`offset_px.y` の正方向と正 height viewport からの符号を fixture に固定する。」
2. **J1-SERIES**: 「Halton の index origin（0 又は 1）、8 規範値、phase wrap、width/height が 0 の場合の error、
   resize 後の最初の sample を schema contract と fixture に固定する。」
3. **J1-SCHEMA**: 「`projection_jitter` を feature compose が型・range・未知 key まで検証し、二提供者を feature 名入りで
   reject する。compose result と `pelican.frame_plan` v1 のどこへ `{provider,pattern,phases}` を保持するかを定義し、
   `get_frame_plan` fixture を追加する。」現行 compose result は config/defines/feature_names しか保持せず
   (`src/project/featurecompose.hpp:16-20`)、frame plan 出力にも該当 field はない
   (`src/core/renderingpass/frameplanner.cpp:798-805`)。
4. **J1-DELIVERY**: 「camera API と culling matrix は非 jitter のままにする。render-frame snapshot に non-jittered と
   jittered の current/previous projection/VP を保持する。main material の `engineMvp`、velocity、SSAO、world debug、
   sprite、shadow、UI、fullscreen の各 consumer についてどちらを使うかを表と integration fixture で固定する。
   pass anchor を projection 意味論の判定に使わない。」
5. **J1-ABI/VELOCITY**: 「FrameUBO の CPU/GLSL std140 offset/size を同時に更新し ABI test を置く。velocity は共通
   `velocity.frag` で current/previous NDC から jitter を引いた後、UV delta (`*0.5`) を出す実装を規範とし、static /
   skinned、camera motion、object motion、jitter-only、jitter-off の数値 fixture を置く。」別実装を選ぶ場合も、変更対象
   file と同じ数値規範を明記する。
6. **J1-RESET**: 「`history_valid` 又は同値の reset epoch を FrameUBO/public pass input に追加する。初回、resize、
   set_time、camera cut、feature enable の invalidation を統一し、reset frame は current を無加工で出す。reset 時の
   previous projection/jitter は current と一致させる。」
7. **J1-GATE**: 「既存 golden の gate は provider 無し/default off の byte 一致とする。provider on/TAA off は画像不変を
   要求せず、規範 Halton sample ごとの jitter-only golden と二回実行一致を新設する。」

### T-TAA（stdlib feature）

1. **TAA-GRAPH**: 「現行 validator を変えない v1 は二 fullscreen pass とする。resolve は四入力から current
   `taa_accum` 一出力、copy/composite は current `taa_accum` 一入力から downstream color 一出力とする。同一 target の
   非 history read/write と fullscreen MRT を使用しない。完全な `taa.json` fixture が example config へ compose/validate
   できることを gate にする。」
2. **TAA-RESOURCE**: 「scene linear HDR color、velocity、depth、downstream color の canonical binding 又は feature parameter
   を schema で定義する。depth target へ `SAMPLED` usage を feature override で加え、format/extent/sample count と
   depth linearization 式（ZO、perspective/ortho）を固定する。プロジェクト固有名を shader/stdlib JSON に直書きしない。」
3. **TAA-RESOLVE**: 「velocity の単位（UV delta）、UV origin、jitter の単位（NDC）、reprojection 式、out-of-bounds reject、
   depth disocclusion threshold、history invalid、alpha semantics、3x3 clamp の edge sampling、blend 係数の向きを数式と
   fixture に固定する。」現案は depth を読むとだけ書き、disocclusion 判定を定義していない。
4. **TAA-GOLDEN**: 「default off の既存全 golden byte 一致、jitter-only、静止 N-frame、camera/object motion、disocclusion、
   resize、set_time、orthographic、二回実行一致を別々の gate とする。」

---

# 2. `docs/design_usd_openpbr.md` v1

## 2.1 判定: **Reject**

### USD-1 [Blocker] v1 OpenPBR subset の写像先が現行 material/surface ABI に存在しない

`PelicanSurfaceV1` は frozen ABI で、base color、normal、metallic、roughness、occlusion、emissive しか持たない
(`src/core/resources/shaders/include/pelican_surface_v1.glsl:4-30`)。現行 template が出す material target もその固定値だけで
ある (`src/core/resources/shaders/material/surface_v1.frag:48-91`)。OpenPBR v1 として挙げた specular color/IOR、coat
color/weight/roughness はこの struct 又は出力へ保持できない
(`docs/design_usd_openpbr.md:67-72`)。

B-layer の custom `pelican_lighting_v1` hook で OpenPBR lighting をその場計算する経路は考えられる。しかしそれは
「`.surface` params を追加すれば表現できる」という話ではなく、forward routing、light loop、shadow、IBL、alpha と
custom texture binding を含む lighting ABI の選択である。設計自身も任意 BRDF/lighting は既定で forward 行きとしている
(`docs/design_material_shading.md:317-330`)。`PelicanSurfaceV2/G-buffer 拡張` と
`専用 lighting hook/forward` のどちらを採るかを決めず、「別 snippet なので既存無変更」だけで golden 維持を主張する
(`docs/design_usd_openpbr.md:79-85`)ことはできない。

さらに具体的な schema blocker がある。`pelican.material` は custom parameter の `values` は持つが custom texture
override を持たない (`src/project/materialformat.hpp:20-39`)。top-level `textures` は parser が明示的に reject する
(`src/project/materialformat.cpp:458-469`)。`.surface` 側の texture 宣言は一個の `default_reference` を持つだけ
(`src/project/surfaceformat.hpp:47-57`)で、lowering も全 material についてその default を bind する
(`src/project/materiallowering.cpp:245-265`)。従って import が出すという
`pelican.material (values + textures 辞書)` (`docs/design_usd_openpbr.md:63-65`)は現行 parser へ入力できない。

alpha mode も一個の共通 OpenPBR surface では表現できない。blend/cull/depth は material value ではなく surface 固定の
render state である (`src/project/surfaceformat.cpp:530-600`)。OPAQUE/MASK/BLEND を material ごとに変えるなら、少なくとも
surface variant 三種、又は material-level pipeline state と alpha-cutoff ABI が必要である。

### USD-2 [Blocker] 外部 `pelican.material` と GLB primitive の binding が閉じていない

現行 glTF loader は glTF 内の material index から `MaterialInfo` を生成して model template へ登録する
(`src/core/model/gltf.cpp:1161-1219`, `src/core/model/gltf.cpp:1410-1419`)。scene loader は GLB template を読み置くだけで、
外部 `pelican.material` や primitive ごとの override map を受け取らない
(`src/core/loader/scene.cpp:456-482`)。従って U0 が GLB、scene fragment、別 material を生成しても、mesh が OpenPBR
material を使う経路がない。

既存 shading 設計には将来の scene material component / glb extras の案がある
(`docs/design_material_shading.md:141-147`)が、現実装の契約ではない。U0 は
`USD material binding/subset → GLB primitive → pelican material name` の安定した対応表、name collision、複数 material
subset、fragment load、missing material error を一つの runtime/import ABI として先に実装・検証しなければならない。

### USD-3 [Blocker] `guc (USD→glTF)` は変換方向が逆で、既に archived である

設計は `guc` を USD→glTF の候補にする (`docs/design_usd_openpbr.md:26-30`,
`docs/design_usd_openpbr.md:111-114`)。Google の公式 repository は名称も機能も **USD from glTF** で、CLI は
`usd_from_gltf <source.gltf> <destination.usdz>`、用途は glTF→USDZ である。また 2024-06-06 に archived されている
([google/usd_from_gltf](https://github.com/google/usd_from_gltf))。逆変換候補として試作する余地はない。

`usd-core` 自前抽出は Windows を含む wheel が現在提供され、一次候補としては妥当
([usd-core on PyPI](https://pypi.org/project/usd-core/))。ただし MaterialX は OpenUSD core の単なる shader 値読取ではなく、
UsdMtlx の file-format / discovery / parsing plugin と検索 path を伴う
([UsdMtlx official documentation](https://openusd.org/release/api/usd_mtlx_page_front.html))。wheel に必要 plugin/data が
同梱されるかを Windows CI で probe しなければならない。

Blender も同等候補ではない。公式 manual は import が layers/references を扱わず、UsdPreviewSurface の変換は lossy と
明記する。import 項目は UsdPreviewSurface を列挙する一方、MaterialX network の説明は export 側である
([Blender USD manual](https://docs.blender.org/manual/en/dev/files/import_export/usd.html))。従って
MaterialX/OpenPBR import が fixture で証明されない限り、Blender は「lossy PreviewSurface fallback」にしか置けない。

比較表には最低でも変換方向、maintenance/security、license と再配布形態、Windows CI install、exact version/hash、
USD resolver/plugin availability、variant/payload/layer、UsdGeomSubset/material binding、MaterialX graph fidelity、UDIM、
color space、texture transform/channel packing、primvar interpolation、triangulation/normals/tangents、negative determinant、
deterministic naming/order/float/GLB metadata、診断、性能を含める必要がある。現案の四軸では採用判断にならない。

### USD-4 [Blocker] import manifest を「共有」できず、決定性の記述も矛盾している

現行 engine parser が受ける output schema は `gltf`、`png` と四つの `pelican.*` だけである
(`src/project/importmanifest.cpp:134-148`)。`pelican.material` は受理されない。外部 import-tools の KTX2 recipe は既に
`khronos.ktx2` version 2 を出す
(`C:/pelican-import-tools/src/pelican_import_tools/ktx2.py:79-90`)ため、これも現行 parser と不一致で
ある。U0 が material/KTX2 を outputs に列挙すれば「houdini-adapter と manifest 形式を共有」
(`docs/design_usd_openpbr.md:49-53`)という gate を通らない。

import-tools の writer は output を path 順に sort し stable JSON を書く点は使える
(`C:/pelican-import-tools/src/pelican_import_tools/common.py:38-67`)。しかし manifest の `tool` は
一個の name/version だけ (`src/project/importmanifest.hpp:11-33`)で、usd-core/Blender、converter、texture encoder から成る
toolchain を記録できない。`source` metadata は自由 JSON なので、少なくとも規範的 `source.toolchain[]` と各 executable /
Python package の version/hash、recipe arguments、resolver/plugin environment を定義すべきである。

設計は「byte determinism は強制しない」と書いた直後
(`docs/design_usd_openpbr.md:31-32`)、U0 gate で同一 SHA-256 を要求する
(`docs/design_usd_openpbr.md:49-53`)。どちらかに統一しなければならない。受け入れ gate を SHA-256 とするなら、converter
だけでなく依存ツール、出力順序、float serialization、GLB JSON key/order、image encoder、環境を pin する必要がある。

### USD-5 [Blocker] axis/unit/flatten/USDZ は「manifest に記録」だけでは再現契約にならない

既存 manifest は `source` に追加 metadata を保持できる
(`src/project/importmanifest.hpp:17-20`)ため、格納場所そのものは流用できる。しかし設計が定めるのは「m・+Y にする」
ことと「パラメータを記録する」ことだけである (`docs/design_usd_openpbr.md:43-47`)。必要なのは少なくとも次である。

- authored `upAxis` / `metersPerUnit`、default 欠損時の値、適用した 4x4 basis/scale、precision/rounding。
- xform を node に残すか vertex へ bake するか、negative determinant 時の winding/normal/tangent、normal transform。
- variant selection、session layer、purpose、payload load policy、population mask/defaultPrim、resolver/search path。
- dependency closure の resolved URI と hash、texture/UDIM/localization、name/path canonicalization。

OpenUSD の flatten は composition arc を単一 layer へ bake するが、重複・メモリ/計算量のコストがあり、外部 asset 群を
移送可能にするには別途 localization と参照の retarget が必要である
([OpenUSD glossary: Flatten](https://openusd.org/release/glossary.html#flatten))。単に variant 引数を受けて flatten するだけでは、
同じ root file から同じ依存解決になる保証がない。

`usdz は unzip` も不十分である。root layer の選択、archive 内 path traversal/duplicate/case collision、asset resolver
semantics を定めず一般 ZIP として展開してはならない。OpenUSD の package/resolver API を使うか、安全な展開規約と
fixture を設ける必要がある。

### USD-6 [Major] OpenPBR version、mapping、WARN の境界が規範になっていない

「1.1 に pin」 (`docs/design_usd_openpbr.md:79-80`)は exact pin ではない。2026-07-16 時点の公式最新 release は
1.1.1 である ([OpenPBR official repository](https://github.com/AcademySoftwareFoundation/OpenPBR))。manifest には最低でも
`openpbr_version: "1.1.1"` と specification/reference implementation の tag 又は commit hash を記録し、patch update を
自動 additive とみなさないこと。

また MaterialX は任意 graph であり、単純な values/textures 辞書ではない。v1 が受ける node/category、constant、image、
channel、colorspace、texcoord、transform、connection の allowlist を定め、bake するものと reject するものを分ける必要が
ある。未対応 WARN は「node が存在したら」ではなく「非 default 値が authored、又は connection があり、結果へ寄与する
場合」に限定しないと、全 default input を持つ OpenPBR node が WARN だらけになる。WARN code、USD prim path、input 名、
fallback 値を machine-testable に固定すべきである。

### USD-7 [Major] 現行 glTF loader の KHR 対応範囲を基準化していない

現行 loader が明示的に読む material extension は `KHR_materials_emissive_strength` だけである
(`src/core/model/gltf.cpp:1185-1191`)。core の base color、metallic/roughness、normal scale、emission、occlusion strength は読むが
(`src/core/model/gltf.cpp:1161-1216`)、texture collection も base/MR/normal/emissive の四系統だけである
(`src/core/model/gltf.cpp:1238-1251`)。specular/ior/clearcoat/alphaMode/doubleSided を「additive」にするには、
既存 material が extension を持たない場合の byte-identical gate と、extension を持つ場合の新 routing/texture/pipeline state
gateを分けなければならない。OpenPBR material binding 問題を解く前に loader だけ拡張すると、同じ material を二経路で
解釈する競合が生じる。

## 2.2 再提出時に WP へ逐語添付すべき条件

以下を満たすまでは再判定も **Reject** とする。

### WP の再分割

1. **M-PBR0a（representation/routing ABI）**: 「OpenPBR v1.1.1 の各対応 input を
   `surface param/custom texture/render state/lighting` のどこへ保持するか表にする。`PelicanSurfaceV2 + G-buffer` 又は
   `pelican_lighting_v1 + forward` を決定する。per-material custom texture override、OPAQUE/MASK/BLEND、alpha cutoff、
   double-sided、material-to-GLB-primitive override ABI を実装し、dump-lowered と schema error fixture を gate にする。」
2. **M-PBR0b（shading/mapping）**: 「M-PBR0a の ABI 上に OpenPBR snippet と UsdPreviewSurface/OpenPBR/glTF mapping table を
   実装する。OpenPBR 1.1.1 exact pin、parameter unit/range/default/colorspace/channel、unsupported WARN code を規範化し、
   base/specular/IOR/coat/emission/normal/alpha の数値・画像 golden と既存 material 全 golden を gate にする。」
3. **U-USD0a（tool spike/selection）**: 「production recipe と分離した probe とする。`guc` を候補から削除する。
   exact usd-core wheel と Blender fallback を、固定した USD corpus で §USD-3 の比較軸により評価する。MaterialX/OpenPBR を
   読めなければその候補を PreviewSurface-only と明記する。採用 tool/version/hash/license/Windows install を成果物にする。」
4. **U-USD0b（composition/static geometry）**: 「採用した usd-core lane で variant/payload/resolver/localization、axis/unit、
   xform、mesh/subset/primvar、deterministic naming/serialization を実装する。GLB + scene の runtime load fixture、dependency
   closure hash、二回 SHA-256 一致を gate にする。これは material を default standard へ落とせるため M-PBR0 に依存させない。」
5. **U-USD0c（materials/textures/binding）**: 「UsdPreviewSurface と allowlist 済み MaterialX OpenPBR を M-PBR0b へ写像し、
   per-primitive/subset binding、PNG/KTX2、UDIM policy、color space、texture transform、WARN/reject を実装する。生成した
   GLB+scene+pelican.material を engine が実際に OpenPBR で描く golden を gate にする。依存は U-USD0b + M-PBR0b とする。」
6. **U-USD1a / U-USD1b**: UsdSkel と camera を別 WP にする。前者は joint order、bind/rest transform、weights、clip/time
   sampling、後者は perspective/ortho、aperture/focal、clip、axis/unit、animated camera を別 fixture で gate する。
7. **U-USD2a / U-USD2b**: rigid xform animation と PointInstancer を別 WP にする。前者は interpolation/timeCodesPerSecond、
   後者は prototype mapping、protoIndices、ids/inactiveIds、per-instance primvars と scale/orientation を別 fixture で gate する。

### 全 USD WP 共通 contract

1. **MANIFEST**: 「engine/import-tools 双方が受理する output schema/version 表を一つの fixture にし、少なくとも `gltf`、
   `png`、`pelican.scene`、`pelican.material`、採用する場合 `khronos.ktx2` を同期する。`source.toolchain[]` に全 tool/package の
   name/version/hash と recipe args、resolver/plugin environment、OpenPBR exact version を記録する。」
2. **DETERMINISM**: 「同一 source dependency closure + 同一 recipe/toolchain から全 output SHA-256 が二回一致することを gate
   とする。byte determinism を採用しない場合は SHA-256 gate を削除し、何を semantic compare するかを field 単位で定義する。」
3. **NORMALIZATION**: 「axis/unit/flatten/localization の全 metadata key と型を manifest schema と fixture に固定する。
   authored 値、default、applied transform、selection/load/resolver、dependency hash を記録し、inverse/negative determinant と
   missing dependency を検査する。」
4. **BINDING**: 「USD prim/subset path → output GLB mesh/primitive index → pelican material name の mapping を安定出力し、scene
   loader/runtime がその mapping を消費する。collision、missing、duplicate、fragment load は名前入り hard error とする。」
5. **SAFETY**: 「USDZ は package API 又は安全展開器を使い、root layer、path traversal、absolute path、symlink、duplicate/
   case collision、archive bomb limit を fixture で検査する。」

---

# 3. 最終結論

両案とも方向性の再考は不要だが、現 v1 は実装可能性を証明する中間契約が欠けている。TAA は投影式・行列配送・graph
shape を修正しなければ画面に jitter/TAA が正しく載らない。USD/OpenPBR は material representation/binding と converter
選定を先に分離しなければ、変換に成功しても engine 上では OpenPBR material が mesh に適用されない。

従って「文言修正を条件に着手可」ではなく、上記 blocker を反映した v2 と WP 再分割を先にレビューへ戻すべきである。
