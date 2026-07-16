# TAA / projection jitter v2 + USD / OpenPBR v2 再レビュー

レビュー日: 2026-07-16  
対象: `docs/design_taa_jitter.md` v2、`docs/design_usd_openpbr.md` v2  
照合元: `docs/design_reviews/2026-07-16_taa_usd_review_codex.md` §1.2 / §2.2  

## 冒頭判定

- `docs/design_taa_jitter.md` v2: **条件付き**
- `docs/design_usd_openpbr.md` v2: **条件付き**

両案とも v1 のままでは実装不能だった中核（投影式、行列配送、二パス TAA、
OpenPBR representation/binding、USD tool 選定）は正しい方向へ直されており、
Reject を継続するほどの構造的な差し戻しは要らない。一方、前回の逐語条件を
「実装 WP の受け入れ条件」として読むと、TAA は Halton wrap、reset の shader
可視性、consumer 配送、named binding と resolve 数式がまだ閉じていない。
USD は `doubleSided` が脱落し、現行 `.surface` で surface variant を何ファイルに
落とすかが未定義である。従って以下の条件を各 WP に添付してから着手可とする。

---

# 1. `docs/design_taa_jitter.md` v2

## 1.1 前回 blocker / 逐語条件との照合

| 前回項目 | v2 判定 | 根拠 |
|---|---|---|
| TAA-1 / J1-MATH | **反映済み** | 一般式、perspective の `-=`, ortho の translation 列、複数 z の CPU test が明記された (`docs/design_taa_jitter.md:50-74`)。 |
| TAA-2 / J1-DELIVERY | **一部不足** | main material が push constant であることを認識し consumer 表を置いた (`docs/design_taa_jitter.md:75-101`)。ただし SSAO が表から漏れ、sprite / world debug の現行供給経路の記述が実コードと違う。 |
| TAA-3 / J1-ABI/VELOCITY | **反映済み** | 変更点を共通 `velocity.frag` とし、NDC で jitter を引いてから UV 差へ変換する規範、ABI gate、数値 fixture を明記した (`docs/design_taa_jitter.md:103-123`)。現行出力も `(current_ndc - previous_ndc) * 0.5` である (`src/core/resources/velocity.frag:7-10`)。 |
| TAA-4 / TAA-GRAPH | **反映済み** | 一パス RMW/MRT を撤回し、一入力面一出力の resolve + composite に分割した (`docs/design_taa_jitter.md:148-161`)。 |
| TAA-5 / J1-RESET | **一部不足** | `frame_index` 案を撤回し epoch と統一 invalidation を導入した (`docs/design_taa_jitter.md:125-138`)。ただし FrameUBO に current epoch 一個しかなく、stdlib shader が「前フレーム epoch と違う」を判定できない。 |
| TAA-6 / J1-GATE | **反映済み** | default off / jitter-only / TAA-on の三 gate へ分離した (`docs/design_taa_jitter.md:140-146`)。 |
| J1-SERIES | **一部不足** | 1 起点、規範値、wrap、zero extent、resize 後の規約は列挙したが、式が 0 番 sample を再導入している (`docs/design_taa_jitter.md:37-48`)。 |
| J1-SCHEMA | **反映済み** | 型/range/未知 key/二提供者、compose result、frame plan、fixture を明記した (`docs/design_taa_jitter.md:22-35`)。 |
| TAA-RESOURCE / RESOLVE | **一部不足** | 二パス resource と depth usage は反映したが、named binding の instance schema と、再投影・depth reject の実式/数値 fixture がない (`docs/design_taa_jitter.md:163-181`)。 |
| TAA-GOLDEN | **反映済み** | 前回列挙したケースを別 gate として保持した (`docs/design_taa_jitter.md:183-188`)。 |

## 1.2 条件 TAA-C1: Halton の 1 起点と phase wrap を同じ式にする

v2 は `halton23(frame_index % phases)` と書く一方で index は 1 起点とする
(`docs/design_taa_jitter.md:39-44`)。`frame_index` は 0 から始まり `advance()` ごとに
1 増える (`src/core/appflow/enginetime.hpp:20-25`,
`src/core/appflow/enginetime.cpp:29-40`)ため、現式では wrap 時に index 0 が入る。
「0 の (0,0) を避ける」という本文自身の目的と矛盾する。

### J1-SERIES 逐語添付条件

> **J1-SERIES-WRAP**: 「最初の描画 frame の `frame_index == 1` を規範とし、
> `sample_index = ((frame_index - 1) % phases) + 1`、
> `offset_px = halton23(sample_index) - 0.5` とする。frame 0 を描画し得る呼出経路は
> 名前入り error 又は同じ sample 1 への明示 mapping とし、unsigned underflow に
> 任せない。phase 8 の規範値は
> `(0,-1/6), (-1/4,1/6), (1/4,-7/18), (-3/8,-1/18),
> (1/8,5/18), (-1/8,-5/18), (3/8,1/18), (-7/16,7/18)` とし、
> 8 の次が 1 個目へ戻ることを CPU fixture で検査する。」

## 1.3 条件 TAA-C2: consumer 表の結論は概ね正しいが、配送経路を訂正する

リポジトリ内で camera projection を読む GPU consumer は次である。

- velocity は current/previous の FrameUBO projection を読む
  (`src/core/resources/velocity.vert:10-16`,
  `src/core/resources/velocity_skinned.vert:13-21`)。v2 どおり jittered を渡し frag で
  差し引く。
- sprite は既に FrameUBO の `projection * view` を読む
  (`src/core/resources/sprite.vert:39-53`)。v2 表の「専用 UBO/push(実装調査の上)」
  (`docs/design_taa_jitter.md:90`)は不要かつ現行経路と不一致である。
- SSAO は FrameUBO projection で view-space sample を G-buffer UV へ投影する
  (`src/core/resources/ssao.frag:48-72`)。jittered G-buffer と同じ projection が必要で、
  **non-jittered consumer ではない**が、前回が明示要求した consumer 表から漏れている。
- main material は camera VP push constant (`src/core/renderer/materialrender.cpp:52-60`)、
  shadow は light VP push constant (`src/core/renderer/materialrender.cpp:84-95`)であり、
  v2 の意味論は正しい。
- world debug は GPU で VP を掛けない。CPU が `camera.getVPMatrix()` で NDC 化
  (`src/core/phys/physworld.cpp:449-454`)し、vertex shader はその値をそのまま
  `gl_Position` にする (`src/core/resources/debug_draw.vert:18-21`)。従って
  「専用 UBO/push」では jitter を載せられない。

以上から、**現存する non-jittered camera-projection GPU consumer は見つからない**。
将来 SSAO/SSR 等が non-jittered projection を要求した場合に FrameUBO 末尾へ
`non_jittered_projection` 等を追加する構えは、現行 offset を保つ限り additive で足りる。
ただし現在の consumer 表と fixture は次の条件で閉じる必要がある。

### J1-DELIVERY 逐語添付条件

> **J1-DELIVERY-CONSUMERS**: 「consumer 表に SSAO を追加し、jittered projection
> で current G-buffer と同じ raster 座標を使うことを fixture で固定する。sprite の
> 供給経路は既存 FrameUBO とする。world debug は camera API を変更せず、
> render-frame snapshot の jittered VP を renderer→debug enqueue の引数として渡して
> CPU NDC 変換へ使い、debug vertex shader は clip/NDC 直送のままとする。
> `rg` 等で FrameUBO projection / camera VP の全 consumer を列挙する fixture 又は
> reviewable manifest を J1 gate に置く。provider off では既存値、provider on では
> SSAO/sprite/debug の輪郭が main material と同じ sample offset になることを検査する。」

## 1.4 条件 TAA-C3: epoch を stateless shader が判定できる形にする

v2 の FrameUBO 追加は `temporal_reset_epoch` 一個だけである
(`docs/design_taa_jitter.md:103-113`)が、consumer の規約は「前フレームの epoch と
違う」とする (`docs/design_taa_jitter.md:130-135`)。現行 FrameUBO にも current / previous
epoch の保存場所はない (`src/core/renderer/frameresources.hpp:11-30`,
`src/core/resources/shaders/include/pelican_frame.glsl:6-15`)。CPU state を持たない stdlib
fullscreen shader は current 一値から reset を判定できない。

### J1-RESET 逐語添付条件

> **J1-RESET-VISIBILITY**: 「FrameUBO/public pass input に
> `history_valid`（0/1）を置くか、`temporal_reset_epoch` と
> `previous_temporal_reset_epoch` の両方を置く。stdlib shader の private state や
> scene alpha への epoch 埋込みには依存しない。初回、resize、`set_time`、camera cut、
> feature enable の各 reset frame で shader が必ず invalid を観測し current color を
> 無加工で出し、次 frame で valid に戻る truth table を fixture にする。reset frame の
> previous projection/view/jitter は current と一致させるが、reset 判定用 flag/epoch まで
> 同値化して判定を消してはならない。」

## 1.5 条件 TAA-C4: named binding を独立 sub-gate にし、resolve の式を書く

現行 `features` は文字列参照の配列だけを受ける
(`src/project/featurecompose.cpp:109-126`)。render feature v1 の envelope は
schema/version/name だけを検査し (`src/project/featurecompose.cpp:85-99`)、既存
`render_target_overrides` は concrete target 名を key に取る
(`src/project/featurecompose.cpp:703-739`)。従って v2 の「named binding parameter を
1 点追加」 (`docs/design_taa_jitter.md:163-168`)は既存 schema の小さな利用ではなく、
feature instance syntax、parameter 宣言、置換範囲、target 検証を含む新機構である。

同じ J1 に置くこと自体は妥当である。projection jitter と T-TAA の間に必要な engine
compose 機構だからである。ただし「1 点」のまま隠さず **J1-BINDING** という独立
sub-gate にするべきで、収まらない場合は J1b に分離する。

また v2 は「全て数式で固定」とするが、実際に書かれた式は最後の blend だけである
(`docs/design_taa_jitter.md:173-179`)。velocity の符号からは
`previous_uv = current_uv - velocity_uv` となるが、本文にはこの式も depth 相対差の
両辺もない。

### J1-BINDING / TAA-RESOLVE 逐語添付条件

> **J1-BINDING**: 「既存の文字列 feature ref を互換維持した上で、feature instance の
> `{ref, parameters}` syntax と feature 側の parameter 宣言 syntax を schema/version
> 付きで定義する。named render-target parameter は required/default、置換可能 field
> （pass input/output、history suffix、render_target_overrides key）を固定し、未知/missing
> parameter、未知 target、role/format class/extent/sample count/usage 不適合を feature 名・
> parameter 名・解決 target 名入りで reject する。二つの異なる project RT 名へ bind した
> 同一 TAA feature の compose result と frame plan fixture を J1 gate にする。」
>
> **TAA-RESOLVE-EQUATIONS**: 「velocity は
> `v_uv = (current_ndc - previous_ndc) * 0.5`、履歴参照は
> `history_uv = current_uv - v_uv` とする。`history_uv` が `[0,1]^2` 外なら履歴を棄却する。
> depth sample `d` は Vulkan ZO の `[0,1]` とし、UV 左上・正 height viewport では
> `q = inverse(P_jittered) * vec4(2*uv-1, d, 1)`、
> `linearize(d,uv) = -q.z/q.w` を perspective/ortho 共通の規範とする。
> 四入力構成を維持する場合は
> `d0 = linearize(depth(current_uv))`, `d1 = linearize(depth(history_uv))`,
> `abs(d1-d0)/max(abs(d0),epsilon) > tau` を depth 不連続として棄却する、と比較する
> sample と epsilon を含めて固定する（previous depth history を採る場合は graph/input を
> 明示的に増やす）。3x3 clamp は current scene color の current pixel 周辺を
> clamp-to-edge で読む。valid 時のみ `out.rgb = mix(clamped_history, current.rgb, alpha)`、
> `out.a = current.a`、invalid 時は `out = current` とする。符号、OOB、閾値ちょうど、
> 四辺/corner、reset の数値 fixture を置く。」

## 1.6 TAA 結論

TAA-1/3/4/6 は骨抜きなく解消され、TAA-2/5 も方針は正しい。TAA-C1〜C4 は
局所的な契約補完であり、二層構成や二パス graph の再設計を要しない。上記逐語条件を
J1 / T-TAA に添付することを条件に **Accept 相当**とする。

---

# 2. `docs/design_usd_openpbr.md` v2

## 2.1 前回 blocker / 逐語条件との照合

| 前回項目 | v2 判定 | 根拠 |
|---|---|---|
| USD-1 / M-PBR0a | **一部不足** | representation/routing、custom texture、alpha variant、primitive binding を ABI-first WP にした (`docs/design_usd_openpbr.md:23-56`)。ただし前回逐語条件の `double-sided` が落ち、variant の artifact 形も未定義。 |
| USD-2 / BINDING | **反映済み** | USD prim/subset→GLB mesh/primitive→material の安定 mapping と runtime consumer、hard error を明記した (`docs/design_usd_openpbr.md:49-55`, `docs/design_usd_openpbr.md:172-174`)。 |
| USD-3 / U-USD0a | **反映済み** | `guc` を削除し、usd-core / Blender fallback / UsdMtlx Windows probe と完全な比較軸を置いた (`docs/design_usd_openpbr.md:87-107`)。 |
| USD-4 / MANIFEST・DETERMINISM | **概ね反映済み** | output schema table、toolchain、SHA-256 二回一致へ統一した (`docs/design_usd_openpbr.md:153-170`)。KTX2 parser 修正には下記の小さな残件がある。 |
| USD-5 / NORMALIZATION・SAFETY | **反映済み** | axis/unit/xform/negative determinant、dependency closure/localization、安全な USDZ 展開を contract と gate にした (`docs/design_usd_openpbr.md:109-129`, `docs/design_usd_openpbr.md:171-175`)。 |
| USD-6 / M-PBR0b | **反映済み** | OpenPBR 1.1.1 exact pin、数表、MaterialX allowlist、authored non-default/contributing connection に限定した WARN を明記した (`docs/design_usd_openpbr.md:58-76`, `docs/design_usd_openpbr.md:131-141`)。 |
| USD-7 / glTF KHR | **反映済み** | M-PBR0a 後の別レールとし、extension 無し既存 gate と extension 有り routing gate を分離した (`docs/design_usd_openpbr.md:78-83`)。 |
| U-USD0b 逐語条件 | **一部不足** | composition/normalization/determinism はあるが、前回が明記した mesh/subset/primvar の抽出規範が WP 本文から落ちた (`docs/design_usd_openpbr.md:109-129`)。 |
| U-USD0c / U1 / U2 / 共通 contract | **反映済み** | material golden、Skel/camera、xform/instancer の分割、共通五 contract を維持した (`docs/design_usd_openpbr.md:131-185`)。 |

## 2.2 新規判断: `lighting hook + forward` は deferred 展望と衝突しない

この推奨は妥当である。material 設計は arbitrary BRDF / lighting を既定で forward、
blend も forward とし、deferred は限定された opt-in としている
(`docs/design_material_shading.md:317-343`)。現行 lowering も `brdf_v1` / `lighting_v1`
hook を `forward_opaque`、blend/screen input を `forward_transparent` へ振り分ける
(`src/project/materiallowering.cpp:149-156`)。従って OpenPBR を forward に置くことは
deferred 化の妨害ではなく、既存ハイブリッド方針の正規利用である。

`PelicanSurfaceV1` は frozen で coat/IOR を持たない
(`src/core/resources/shaders/include/pelican_surface_v1.glsl:4-30`)が、同一 `.surface`
内の lighting hook は generated param/texture accessor を直接読める。現在の template も
custom lighting hook を呼ぶ分岐を持つ
(`src/core/resources/shaders/material/surface_v1.frag:62-85`)。将来 OpenPBR を deferred
へ載せる需要が出た時だけ PelicanSurfaceV2/G-buffer を別 WP にする v2 の境界
(`docs/design_usd_openpbr.md:33-39`)でよい。

## 2.3 条件 USD-C1: alpha 三種だけでは `doubleSided` を表せない

前回の M-PBR0a 逐語条件は OPAQUE/MASK/BLEND、alpha cutoff に加えて
`double-sided` を要求した
(`docs/design_reviews/2026-07-16_taa_usd_review_codex.md:328-334`)。v2 の M-PBR0a は
alpha 三種と cutoff までは書いたが `doubleSided` がない
(`docs/design_usd_openpbr.md:40-48`)。

現行 render state は surface document に一個だけあり、blend/cull/depth を固定する
(`src/project/surfaceformat.hpp:59-69`, `src/project/surfaceformat.hpp:90-106`)。lowering も
surface の render state を material へそのまま写す
(`src/project/materiallowering.cpp:240-253`)。material-level pipeline state を予約のままに
するなら、alpha mode 三種 × cull(back/none) の **最大六 state variant** が必要である。

### M-PBR0a 逐語添付条件

> **M-PBR0a-STATE**: 「OpenPBR/glTF の `doubleSided` を M-PBR0a の input 置き場表と
> binding ABI に戻す。material-level cull override を入れない v1 は
> `{opaque,mask,blend} × {single_sided(cull=back),double_sided(cull=none)}` の surface
> variant へ決定的に route する。MASK は opaque blend/depth-write と alpha cutoff
> discard、BLEND は blend/depth-read-only とし、各組合せの pipeline state dump、
> front/back view、cutoff 境界、primitive binding golden を gate にする。」

## 2.4 条件 USD-C2: 現行 `.surface` は一ソースから三 variant を生成しない

`.surface` v1 の header が認識する top-level field は language/params/textures/
screen_inputs/render_state だけで、variant/inheritance はない
(`src/project/surfaceformat.cpp:790-883`)。一 document は code 一個と render_state 一個を
持つ (`src/project/surfaceformat.hpp:98-108`)。従って v2 の「variant 三種」
(`docs/design_usd_openpbr.md:58-64`)は、現行形式のままなら三ファイル、
`doubleSided` を含めれば最大六ファイルになる。

GLSL 実装本体は engine include として共有できる。shader include resolver は登録済み
engine resource を解決できる (`src/core/shader/shadercompiler.cpp:84-124`)。しかし
params/textures の宣言 header は各 `.surface` に必要で、その同期を保証する既存機構は
ない。従って「三/六ファイル全部へ BRDF を複製」は不要だが、「一ソースから自動で
三 variant」は現状では成立しない。

### M-PBR0b 逐語添付条件

> **M-PBR0b-VARIANTS**: 「artifact 形を次のどちらかに固定する。(A) 一つの canonical
> OpenPBR parameter/texture 定義から build 時に三×cull の `.surface` wrapper を生成し、
> 生成物を engine resource として登録する、又は (B) 三×cull の薄い `.surface` wrapper
> を置き、lighting 実装は一つの登録済み engine GLSL include に集約する。(B) では全
> wrapper の params/textures 名・順序・型・default・color space が同一で、差分が
> render_state/cutoff mode/cull だけであることを parser fixture で比較する。variant 名と
> import routing は決定的にし、六 variant の shader/pipeline cache 列挙を gate にする。」

## 2.5 条件 USD-C3: U-USD0b に静的 geometry 抽出規範を戻す

U-USD0a の比較軸には subset、primvar interpolation、triangulation、normal/tangent が
ある (`docs/design_usd_openpbr.md:98-105`)が、採用 lane を実装する U-USD0b 本文は
composition/normalization/localization/safety の記述だけである
(`docs/design_usd_openpbr.md:109-129`)。前回逐語条件の「mesh/subset/primvar」を
比較 spike だけに置き、production WP から落としてはならない。

### U-USD0b 逐語添付条件

> **U-USD0b-GEOMETRY**: 「UsdGeomMesh の points/faceVertexCounts/faceVertexIndices、
> orientation、holes、subdivision policy、UsdGeomSubset family/type、primvar の
> constant/uniform/varying/vertex/faceVarying と indexed values を規範化する。
> triangulation、generated/preserved normal・tangent、UV set、negative determinant
> 反転後の順序を固定し、USD prim/subset path から出力 GLB mesh/primitive index への
> mapping が geometry の決定的 sort 後にも一致する fixture を U-USD0b gate に置く。」

## 2.6 KTX2 manifest 修正 `a8cc24b` の評価

修正の方向は正しい。parser は `khronos.ktx2` を明示 allowlist に足し、明示された
version が 2 以外なら reject する (`src/project/importmanifest.cpp:134-156`)。test も v2 の
accept と v3 の reject を置いた (`test/importmanifest_test.cpp:97-123`)。

ただし **「version 2 限定」はまだ厳密ではない**。実装は
`if (version && *version != 2)` なので version 欠落を accept する
(`src/project/importmanifest.cpp:148-154`)。error も
`version is not supported` だけで、期待値 2、実値、output file を示さない。設計の
schema/version 表を hard contract とするなら小修正を要する。

### 即時小修正 WP 逐語添付条件

> **MANIFEST-KTX2-VERSION**: 「`khronos.ktx2` output は `version` 必須かつ 2 のみを
> accept する。欠落は `import manifest output khronos.ktx2 requires version 2`、
> 不一致は schema、expected=2、actual、`outputs[].file` を含む名前入り error とする。
> version 欠落、1、2、3 の四 fixture を置く。`pelican.material` の schema/version
> allowlist 追加は M-PBR0a の format version 決定後に同じ表/fixtureへ追加する。」

## 2.7 USD / OpenPBR 結論

USD-2〜7 は骨抜きなく反映され、USD-1 も ABI-first / binding-first の主要修正は成立した。
`lighting hook + forward` は deferred 展望と整合する。USD-C1〜C3 と KTX2 の小条件は
既存 WP の境界内で閉じられ、tool lane や M-PBR0a/0b 分割の再設計は不要である。
上記逐語条件を添付することを条件に **Accept 相当**とする。

---

# 3. 最終結論

v2 は前回の Reject 理由を言い換えただけではなく、主要な誤りを実際に撤回・訂正している。
残件は実装時に推測を発生させる contract hole であり、いずれも本文の大方向を変えずに
WP gate へ追加できる。従って二文書とも判定は **条件付き**。条件は
TAA-C1〜C4、USD-C1〜C3、MANIFEST-KTX2-VERSION の逐語添付とする。
