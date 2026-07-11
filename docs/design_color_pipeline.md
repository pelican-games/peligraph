# 色パイプライン(v3)

対象読者: エンジン担当・シェーダ/UI を書く人。
ステータス: v3 ドラフト(2026-07-11。v2 は round 2 レビューで **Reject**
(`docs/design_reviews/2026-07-11_color_ui_v5_rereview_codex.md` §1)。
本版の主変更: ①dual-use fallback を「複製」に変更(誤色 fallback の廃止)
②resource role 別 capability 表 ③canonical frame-plan anchor の統一
(postprocess 文書・UI 文書と同一列)④terminal encode と paired storage
round-trip の分離計数 + 誤差 budget ⑤debug API の意味を黙って変えない
⑥capture point の一本化と contract version)。
前提: HDR/tonemap feature(WP30)、`design_postprocess_temporal.md` §標準パス名、
`design_ui_2d_foundation.md` v6、`docs/shader_contract.md`、
UI v2 レビュー R6(baseline 更新の例外手続き)。

## 0. 原則と作業空間

1. **作業空間 = linear-sRGB(Rec.709 primaries、D65 白色点)**。
   「linear」は transfer(伝達関数)の話であり、gamut(primaries)とは別概念。
   v1 世代のエンジンは gamut 変換を行わない(全入出力を Rec.709 とみなす)。
   wide gamut / working space 切替は将来トラック(§6)
2. **シェーダの入出力は常に linear**。sRGB decode/encode はハードウェア
   (`*_SRGB` view)か、専用の terminal パス(§2-2)だけが行う。
   OETF/EOTF をシーン・エフェクト・UI のシェーダに書くことは禁止
3. **transfer が適用されるのは RGB のみ**。alpha は常に linear の
   straight alpha(UI ABI と同一。`design_ui_2d_foundation.md` §1-1)。
   coverage/mask 値も同様に linear
4. tone curve(HDR→LDR)/ **terminal display encode** / **paired storage
   round-trip** の 3 つを別カウンタで数える:
   - tone curve: 各表示経路でちょうど 1 回
   - terminal display encode(表示・readback バイトを作る変換): ちょうど 1 回
   - paired storage round-trip(8bit SRGB 中間 RT への HW encode → 次 pass の
     HW decode の対): **回数自由だが必ず対で、必ず HW**。シェーダコード内の
     OETF/EOTF は 0 回(唯一の例外 = UNORM fallback の output_transform)
   frame-plan trace は 3 カウンタを個別に記録し(§4)、「不正なシェーダ内
   OETF」と「正当な paired storage encode」を機械的に区別する。paired
   round-trip は 8bit 量子化誤差を蓄積するため、経路ごとに hop 数と誤差
   budget を固定する(§2-6)

## 1. 現状(実測 v2 — レビュー C1 の指摘を含む)

- swapchain 選択は `R8G8B8A8_UNORM`/`B8G8R8A8_UNORM` を優先し view も同一
  (`src/core/vkcore/swapchainframetarget.cpp:52-84`)。headless は
  `R8G8B8A8_UNORM` 固定(`src/core/vkcore/offscreenframetarget.cpp:91-105`)。
  attachment はエンコードしない
- **tone curve と OETF の所有点が分裂している**:
  - 通常 lighting shader は ACES 近似カーブの後に `pow(1/2.2)` まで行う
    (`src/core/resources/fullscreen.frag:230-234`)
  - `hdr` feature の `tonemap.frag` は Reinhard のみで OETF なし
    (`src/core/resources/tonemap.frag:12-19`)
  - `fullscreen.frag` は `PELICAN_FEATURE_HDR` で分岐**しない**。従って hdr on
    が動く graph(現状は golden の合成 graph のみ)では
    「ACES 近似+pow → RGBA16F → Reinhard」の**二重 tone mapping +
    tone curve 前エンコード**になっている(実測 2026-07-11)
  - `hdr_tonemap` の挿入先 `before:present` は **example の main graph には
    存在しない**(末尾は `FinalBloomComposite` → `ui_pass`)。anchor 不在は
    `insertPassByAnchor` が throw するため
    (`src/project/featurecompose.cpp:389-392`)、example に hdr を有効化すると
    **compose 失敗**する(overlay 消失以前の問題 — v2 の記述を訂正)。
    仮に `present` pass を持つ graph に挿されても、`lit_color` だけを読んで
    全面上書きするので bloom 合成と overlay は消える構造
    (`src/core/resources/features/hdr.json:10-28`)
  - 既存 `hdr_on` golden の graph は `hdr_source → copy_to_swapchain → present`
    の合成専用 graph で、bloom chain も overlay も含まない
    (`test/golden_image_test.cpp:843-885`)— hdr×bloom×UI の複合経路は
    **現在どのテストも観測していない**(§4 の新 gate で埋める)
- frame graph の format enum に `*_SRGB` が存在しない
  (`src/core/renderingpass/renderingpassjsonhelpers.cpp:9-23`)
- material texture は一律 `R8G8B8A8_UNORM` 登録
  (`src/core/material/materialcontainer.cpp:123-139`)。glTF loader は
  image/texture 1 個 = GPU texture 1 個で全 slot 共有
  (`src/core/model/gltf.cpp:509-523`)。UI atlas・debug font atlas も UNORM
  (`src/core/renderer/uicontainer.cpp:54-74`, `src/core/renderer/debugtext.cpp:54-63`)
- glTF `COLOR_0` / `baseColorFactor` は float のまま linear multiplier として
  流れる(`src/core/model/gltf.cpp:460-476`)— **これは glTF 仕様どおり正しく、
  移行で壊してはならない**
- golden tolerance は全 case 0 ではない: `stem_fullscreen`(avg 1 / max 4)、
  `vat_playback`(avg 2 / max 32)
- capture/CLI/RPC は image バイトをそのまま PNG 化し色メタデータを付けない
  (`src/core/vkcore/rendertarget.cpp:38-60`)。既存 rpc テストは絶対色を
  一度も検証していない(非空・差分有無・byte 再現性のみ)

## 2. 目標アーキテクチャ

### 2-1. 論理 terminal resource `display` と output_transform(graph rewrite)

「swapchain に各パスが直接書く」構造をやめる。

- frame graph に論理リソース **`display`** を導入する。
  意味論 = **表示用 linear LDR**(格納は `R8G8B8A8_SRGB` view — バイトは
  エンコード済みだが、シェーダ I/O・blend はハードウェアにより linear)
- 既存の pass 設定・feature(`bloom composite` / `ui` / `debug_draw` /
  `debug_text` / 将来の imgui)で `"swapchain"` を出力にしているものは
  すべて **`display` へ retarget** する。プロジェクト JSON 内の `"swapchain"`
  という文字列はエンジンが `display` の別名として読み替える(後方互換)。
  scene pass が実 swapchain image に触ることは以後できない
- エンジンは feature 合成後の**絶対最後**に、purge 不可の
  **`output_transform`** パスを必ず 1 個追加する:
  `display`(sampled、SRGB view 経由で linear 復号)→ 実ターゲット
  - swapchain が `*_SRGB` の場合: シェーダは linear を書き、HW がエンコード
    (シェーダ内変換なし)
  - swapchain が `*_UNORM` の場合(フォールバック): シェーダ内で IEC
    61966-2-1 OETF を適用して書く。**変換式はこの 1 箇所にしか存在しない**
  - 両経路で `display` までの pass 列・blend・pipeline format は**同一**
    (差は output_transform の specialization だけ)。v1 実装では SRGB 経路でも
    output_transform を省略しない(パリティ検査可能性 > 1 パス分のコスト。
    省略最適化は等価性テスト常設後の将来案件)
- `output_transform` は将来の HDR 出力(PQ / scRGB)・カラーグレーディングの
  差し込み点を兼ねる(§5)
- 名前付きスクリーンスナップショット・capture は `display`(= linear LDR の
  encoded バイト)を読む。readback の意味論は §2-7

swapchain / headless の format 選択と **resource role 別 capability 要件**
(照会は `vkGetPhysicalDeviceFormatProperties` の format feature bit。
「SRGB attachment 対応」と「SRGB attachment **blend** 対応」は別 bit として
必ず個別に照会する):

| 対象 | format | 必要 feature bit | 非対応時 |
|------|--------|-----------------|---------|
| swapchain | `B8G8R8A8_SRGB` / `R8G8B8A8_SRGB` + `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` 最優先 | (surface format 列挙に従う) | `*_UNORM` フォールバック経路。選択結果と経路名はログ + `get_status` |
| headless/offscreen | `R8G8B8A8_SRGB` | `COLOR_ATTACHMENT` + `TRANSFER_SRC` | UNORM + output_transform 内エンコード。**どちらでも readback バイト = エンコード済み sRGB** |
| `display` 中間 | `R8G8B8A8_SRGB` | `COLOR_ATTACHMENT_BLEND`(overlay が blend する)+ `SAMPLED_IMAGE` + `SAMPLED_IMAGE_FILTER_LINEAR`(output_transform / snapshot が読む)+ `TRANSFER_SRC`(capture) | **起動エラー**(8bit sRGB のこの組は実質全デバイス対応 — 誤色で続行しない) |
| `lit_color`(hdr off)/ bloom RT 群 | `R8G8B8A8_SRGB` view | `COLOR_ATTACHMENT_BLEND` + `SAMPLED_IMAGE_FILTER_LINEAR`(bloom は linear filter でサンプル) | 同上(起動エラー) |
| material texture(color)| `R8G8B8A8_SRGB` view | `SAMPLED_IMAGE` + `SAMPLED_IMAGE_FILTER_LINEAR` | §2-5 の複製戦略に従う(誤色続行はしない) |

原則: **capability 不足の経路は「正しくない色で続行」ではなく、代替戦略
(複製/専用パス)か明示エラー**。正常終了した全経路で linear invariant
(§0-2)が成立する。

### 2-2. 規範 pass 列(4 経路)— tone curve / OETF の所有点表

記法: `[L]` = scene-linear HDR、`[dL]` = display-linear LDR(SRGB view 格納)。

| # | 経路 | pass 列 | tone curve 所有(回数) | display encode 所有(回数) |
|---|------|--------|----------------------|--------------------------|
| 1 | hdr off + SRGB swapchain | scene passes → `lit_color[dL]`(lighting shader 内 ACES 近似カーブ、**`pow(1/2.2)` は削除**)→ bloom(dL 域)→ composite → `display[dL]` → overlays(UI/debug)→ output_transform → swapchain(HW encode) | lighting shader(1) | swapchain SRGB view(1) |
| 2 | hdr on + SRGB swapchain | scene passes → `lit_color[L]`(RGBA16F。lighting shader は `PELICAN_FEATURE_HDR` で**カーブを skip し scene-linear を出力**)→ bloom(HDR 域)→ composite → `scene_ldr_in[L]` … `hdr_tonemap`(Reinhard)→ `display[dL]` → overlays → output_transform → swapchain | tonemap pass(1) | swapchain SRGB view(1) |
| 3 | headless(SRGB offscreen) | 経路 1/2 と同一の `display` まで → output_transform → offscreen SRGB → readback | 経路に同じ(1) | offscreen SRGB view(1) |
| 4 | UNORM フォールバック | 経路 1/2 と同一の `display` まで → output_transform(**シェーダ内 OETF**)→ UNORM target | 経路に同じ(1) | output_transform シェーダ(1) |

**canonical frame-plan anchor(正本の統一)**: これまで 3 文書がばらばらに
パス列を書いていた(postprocess: `post_main → tonemap → post_ldr →
swapchain` / UI v5 §6: `pelican_ui → debug_text → imgui → present` /
本書 v2: 「bloom 合成後・overlay 前」)。**本書のこの列を唯一の正本**とし、
postprocess/UI 文書はここへの参照に改める:

```
scene passes                     … 出力先 = scene_color 域(format class: scene)
→ [anchor: post_main]            … HDR 域の scene effect(hdr on の bloom はここ)
→ [anchor: tonemap]              … hdr on のみ tonemap pass(scene → display)
→ [anchor: post_ldr]             … display 域の effect(hdr off の bloom 合成到達点)
→ [anchor: pelican_ui]           … UI
→ [anchor: debug_draw / debug_text / imgui]
→ output_transform(エンジンが常設・purge 不可・常に最後)
→ present / readback
```

- anchor は feature 挿入語彙(`before:` / `after:`)の解決先として**graph に
  常に実在する**(空でも名前は存在 — 「anchor not found」の構造的排除)。
  プロジェクト JSON の既存パスは compose 時にこの列へ写像される
- `display` は `post_ldr` 以降の書き込み先。`tonemap` anchor 以降のパスが
  scene_color を出力先にすることは validation エラー
- **RT format の path 依存は名前列挙でなく format class で導出する**:
  RT 宣言に `format_class: scene | display | data | explicit(<format>)` を
  追加し、`scene` は hdr off → `R8G8B8A8_SRGB` / hdr on →
  `R16G16B16A16_SFLOAT` に解決する。hdr feature の
  `render_target_overrides`(名前列挙)は廃止。project 定義の bloom RT も
  `format_class: scene` を宣言すれば自動で追従する
- この rewrite は「tonemap の移設」ではなく **RT 宣言・pipeline
  compatibility・descriptor・barrier・trace fixture を横断する変更**である
  (C1 の規模見積りに含める — §6)

規範ポイント(現状バグの修正を含む):

- `fullscreen.frag` は `PELICAN_FEATURE_HDR` 分岐を持つ: off = カーブあり
  (OETF なし)、on = scene-linear のまま出力。二重 tone mapping を排除
- overlays(UI / debug_draw / debug_text / imgui)は常に `display[dL]` 上で
  linear blend。経路 1〜4 で overlay の順・blend・format は同一
- 8bit 中間(`lit_color` hdr off・bloom RT 群)を SRGB view にするのは
  暗部精度のため(linear 格納の 8bit UNORM は暗部で banding する。
  sRGB 格納は知覚均等に近く、従来の display-referred 格納と同等の精度を保つ)。
  代償の paired round-trip 量子化は §2-6 の誤差 budget で管理する

### 2-3. RT / texture 棚卸しと決定(C0 監査の対象表)

| 対象 | 現在 | 決定 |
|------|------|------|
| lighting(hdr off)| curve+pow 内蔵、UNORM `lit_color` | curve のみ内蔵(pow 削除)、`lit_color` = `R8G8B8A8_SRGB` view |
| lighting(hdr on)| 分岐なし=二重 tone map | `PELICAN_FEATURE_HDR` で curve skip、RGBA16F linear |
| `hdr_tonemap` | `before:present`(example では anchor 不在 = compose 失敗) | canonical `tonemap` anchor へ移設、出力 `display` |
| G-buffer albedo/emissive | 8bit UNORM(display-referred 値が事実上入る) | `R8G8B8A8_SRGB` view(格納バイト従来同等・シェーダ I/O linear)。normal/MR/AO/depth 系は UNORM/SFLOAT のまま(data) |
| bloom 全 RT | 8bit UNORM | `format_class: scene` を宣言(§2-2)— hdr off = `R8G8B8A8_SRGB` view / hdr on = `R16G16B16A16_SFLOAT` に自動解決。threshold の意味は §2-6 |
| UI atlas(color page)| UNORM | `R8G8B8A8_SRGB` view(UI 画像は sRGB authored)。白テクセルページも同様(1.0 は不変) |
| font atlas / debug font | UNORM | **data**(coverage)— `R8_UNORM` 系のまま。transfer 適用禁止 |
| debug_draw / debug_text の API 色引数 | 無印 `glm::vec4` を raw のまま shader へ | **無印 vec4 の意味は黙って変えない — linear として維持**(既存 caller、例: `physworld.cpp` の collider 色 {0.1,0.95,0.85} は linear 値になり見えが変わる → 再基準化対象)。authored 色向けに **明示ヘルパ `Pelican::srgb(r,g,b,a)`**(constexpr、IEC decode して linear vec4 を返す)を追加し、「見た目の色」で書きたい caller はこれを通す |
| glTF baseColor/emissive texture | UNORM view | SRGB view(§2-5 の view 戦略) |
| glTF normal/metallicRoughness/occlusion texture | UNORM | UNORM(data)— 変更なし |
| glTF `COLOR_0` / `baseColorFactor` / `emissiveFactor` | raw linear multiplier | **変更なし(decode しない)**。保護 fixture を置く(§4) |
| `KHR_materials_emissive_strength` 等の強度 | — | radiometric(§2-4)。clamp しない |
| VAT texture | RGBA16F data | 対象外(data)。移行 manifest に「不変」と明記 |
| frame graph format enum | `*_SRGB` なし | `R8G8B8A8_SRGB` / `B8G8R8A8_SRGB` を追加。あわせて resource 宣言に `role: color|data` メタデータ(省略時 data)を追加し、lint が「color role なのに UNORM 8bit」を警告 |
| capture/CLI/RPC | raw byte → PNG | §2-7 の versioned contract |
| clear color(pass JSON)| raw float | **linear 値として解釈**(display 色で書きたい場合は authored 色として §2-4 の decode を通す。既存 0/1 のみの clear は不変) |

### 2-4. authored 色の field schema(encoding × role)

JSON 上の色・数値 field は次の 2 属性で分類する。**「一括 sRGB 扱い」はしない**。

- `encoding: srgb | linear` — 数値の表記空間
- `role: color | data | radiometric` — 意味。transfer を適用してよいのは
  `color` の RGB 成分だけ

| field | encoding | role | 変換 |
|-------|----------|------|------|
| scene のライト色(`color`)| srgb | color | パース時に RGB のみ decode → linear、clamp [0,1] |
| ライト `intensity` | linear | radiometric | 変換・clamp なし |
| UI スキン色 / widget 色 | srgb | color | RGB decode → linear。**alpha は素通し**。u8 化は round-half-even(暗部の量子化損失は UI ABI(linear u8)の既知コスト — banding が問題化したら vertex color の幅を広げる別案件) |
| pelican.material `type: color` の factor | srgb(既定)| color | RGB decode。params 宣言に `"encoding": "linear"` override 可 |
| pelican.material `type: float/vecN` | linear | data/radiometric | 変換なし |
| glTF `baseColorFactor` / `emissiveFactor` / `COLOR_0` | linear | (linear multiplier) | **変換なし** — glTF 仕様準拠。pelican 側 schema の既定(color=srgb)は glTF 由来 field には適用しない(出所ごとの個別表が正) |
| glTF texture(baseColor/emissive)| srgb | color | SRGB view による HW decode |
| glTF texture(normal/MR/AO)/ VAT | linear | data | 変換なし |
| debug_draw/text の色引数(無印 vec4)| linear | color | 変換なし(§2-3 — sRGB 表記で書きたい場合は `Pelican::srgb()` ヘルパで呼び出し側が 1 回 decode) |
| clear_color | linear | — | 変換なし(§2-3) |

decode 式は IEC 61966-2-1 区分関数(c ≤ 0.04045 → c/12.92、それ以外 →
((c+0.055)/1.055)^2.4)。**RGB のみ**。radiometric は clamp 禁止
(HDR 輝度を失わない)。

### 2-5. texture view 戦略(同一 image の color/data 二用途)

「image 1 個 = GPU texture 1 個 = view 1 個」をやめる:

- loader は texture を **(image, encoding-class)** で管理する。
  image は `VK_IMAGE_CREATE_MUTABLE_FORMAT` + `VkImageFormatListCreateInfo`
  (`R8G8B8A8_UNORM` + `R8G8B8A8_SRGB`)で作成し、slot の用途に応じて
  SRGB view / UNORM view を払い出す(1 image に最大 2 view、VkImage 共有)
- 同一 glTF image が baseColor(color)と metallicRoughness(data)の両方から
  参照されるケースはこれで両立する。mutable format list が使えない場合
  (Vulkan 1.2 core なので実質ないが)は **image を encoding-class ごとに
  複製する**(SRGB 用と UNORM 用の 2 image、メモリ 2 倍・色は正しい)。
  「UNORM view のまま警告して続行」は**しない** — 誤色は §0-2 の invariant
  違反であり、§4 の dual-use fixture を自ら破る(v2 の記述を撤回)。
  複製すら不可能な失敗(メモリ枯渇等)は通常のロードエラー
- material default texture(white 等)も color/data 2 view を持つ
  (white 1.0 はどちらの view でも 1.0)

### 2-6. blend・bloom・additive の意味論

- **blend は linear で行われる**(SRGB view 上の blend は HW が
  decode→blend→encode)。半透明・UI の重なりの見えは現行(sRGB 空間 blend)
  から変わる。これは修正であり、§3 の再基準化対象
- **bloom**: threshold(現行 const 0.4)・knee・intensity は
  「その経路の `lit_color` の **linear 値**」に対して評価される、と再定義する
  - hdr off: linear LDR(tone curve 後)に対する threshold —
    従来は display-referred 値に対する 0.4 だったので抽出範囲が変わる
    (linear 0.4 ≈ sRGB 0.665)。**見えの変化は再基準化に含める**。
    定数は当面据え置き、pass param 化は M2b-2 系列(§6 未決 3)
  - hdr on: scene-linear HDR に対する threshold(>1 の輝度が素直に抽出される
    — 物理的に正しい bloom)。composite は tonemap **前**(§2-2 経路 2)
  - bloom RT の加算 composite は各経路の format 上で行う(hdr off の
    SRGB view 格納でも blend/加算演算自体は linear)
- **paired round-trip の誤差 budget(hdr off の 8bit SRGB multipass)**:
  bloom chain は downsample/upsample/composite で複数 hop の
  HW encode→decode→8bit 量子化を通る。規範:
  - 経路ごとに trace が hop 数を記録し、**canonical graph の hop 数を fixture
    に固定**(増えたら fail — 黙って pass が増えない)
  - 解析 fixture: 既知単色板を bloom chain に通した解析値との誤差を
    **1 hop あたり最大 ±1/255(linear 換算で sRGB 量子化 1 step)、経路合計
    max = hop 数**として budget 化。budget 超過 = fail
  - scene 本線(lit_color → display)は 1 hop なので蓄積しない。蓄積するのは
    bloom の様な multipass effect のみ — effect 追加時は hop 数と budget を
    宣言する(feature JSON の必須 metadata に将来昇格)
- **additive パーティクル / 加算合成**(将来の 2D ゲーム層を含む):
  texture RGB = sRGB decode(color)、tint = authored color(§2-4)、
  alpha/強度 = linear。additive の「白飛びの気持ちよさ」が従来の
  sRGB 空間加算に依存していた場合、linear 加算では飽和が遅くなる —
  intensity は radiometric として調整する(互換モードは設けない。
  現リポジトリに additive パーティクルの実アセットはまだ無い)

### 2-7. capture / CLI / RPC contract(versioned)

readback・PNG の意味論変更は公開 API の観測可能な変更なので、契約として固定する:

- **capture point は 1 箇所に固定**: `readbackLastFrameRGBA8` / CLI
  `--render-out` / RPC `capture` / 名前付きスナップショット(無印)は全て
  **output_transform の出力(= 実ターゲットの最終バイト)**を読む。
  SRGB 経路では display のバイトと一致するが、契約上の正はあくまで
  「present/readback されるものと同一のバイト」1 本(`display` を読むという
  v2 の記述は撤回 — 二通りに読める契約を残さない)。名前付きスナップショット
  で中間 RT を指名した場合のみその RT のバイト(encoding は RT の format に
  従う — レスポンスに明記)
- 返るバイト列・PNG = **エンコード済み sRGB(IEC 61966-2-1)、straight alpha
  (alpha は linear のまま)、RGBA order**
- PNG は stb_image_write 出力のため色メタデータ chunk を持たない。
  **「メタデータ無しでも sRGB と解釈する」を API 契約に明記**する
  (DCC/外部ツール利用者向け)。sRGB chunk 付与は将来の writer 差し替え時
- **contract version を数値で持つ**: `get_status` に
  `"color": {"contract": 2, "swapchain_format": "...",
  "path": "srgb" | "unorm_fallback", "readback_encoding": "srgb"}` を追加
  (contract 1 = 現行の「raw バイト・意味論未定義」、2 = 本設計)。
  `capture` のレスポンスにも `"encoding": "srgb", "contract": 2` を追加
  (既存 path フィールドは維持)。C++ 側は `readbackLastFrameRGBA8` の
  doc comment に contract 2 の意味論を記載し、旧 consumer(rpc テスト・
  DCC スクリプト)の移行は C0 manifest の consumer 欄に記録する
- RPC 経路の**絶対色テスト**を新設: 既知 emissive(linear 0.5)の板を
  capture し、中心 pixel が 188±0 であること(§4 known-value の RPC 版。
  SRGB 経路の HW encode は IEC 式で決定的 — ±0 が成立する。UNORM fallback
  との**経路間 parity は ±1**: HW conversion とシェーダ OETF の丸めの
  byte-exact 一致は Vulkan 仕様が保証しない(ULP 猶予)ため、±0 を要求した
  v2 を修正。parity fixture は per-channel ≤1 を gate、実測 0 なら記録)

## 3. 移行手順(golden 一斉再基準化 — 1 回きり・安全化版)

R6 レビューが認めた例外手続きを、v1 レビュー C7 の 5 条件で強化して使う:

1. **C0 監査 = machine-readable manifest**(コード変更なし):
   `docs/color_migration_manifest.json` に §2-3 表の全項目を
   `{path/field, old_encoding, new_encoding, expected_change:
   "bit_exact" | "analytic" | "visual_review", reason}` で列挙する。
   レビュー可能な唯一の変更対象リストであり、C1 の diff 承認の照合先
2. **三者比較**: C1 実装前 binary(基準 commit)と実装後 binary を
   **同一 device / driver / scene / input** で実行し、golden 全 case について
   old / new / analytic reference(手計算できる case のみ)を保存する。
   **case ごとに** diff 画像と manifest 上の理由を突き合わせてレビューし、
   説明の付かない差分が 1 case でもあれば baseline を更新しない。
   承認記録(case → 理由)は PR に表で残す
3. **tolerance の正直化**: 既存の非ゼロ tolerance
   (`stem_fullscreen` avg1/max4、`vat_playback` avg2/max32)は色移行と
   無関係の実装差/非決定性由来であり、**C1 では触らない**(維持 + 理由存置)。
   「全 case exact」とは主張しない。新規追加の色系 fixture は tolerance 0
4. **encode 正当性テストの常設**(§4)を C1 と同一 PR で導入
5. **frame-plan trace の比較**: pass 列(名前・順序・format・
   output_transform の位置と個数)を text golden として保存し、pixel が偶然
   近くても構造が違えば失敗させる
6. 以後の golden は「**エンコード済み sRGB バイト**」という意味論を持つ

C1 は WP70(set 0 再編)と衝突するため **WP70 の後**。C0 は read-only なので
先行してよい(レビュー承認済みの進め方)。

## 4. 常設テスト(analytic fixtures)

| fixture | 検査内容 |
|---------|---------|
| known-value | linear 0.5 を書いた readback が 188(SRGB 経路 ±0 / UNORM fallback ±1) |
| gradient ramp | 二重エンコード・欠落は ramp 形状で即発覚 |
| fallback parity | SRGB / UNORM 両経路で readback per-channel 差 ≤1(HW/シェーダ丸め差 — §2-7) |
| srgb texture decode | 既知バイト(188)の color texture をサンプル → linear 0.5 として振る舞う |
| data texture passthrough | 同バイトの data texture → 188/255 のまま |
| dual-use image | 同一 image を color/data 両 slot から参照して両方正しい |
| alpha straightness | 半透明重ね合わせで alpha に transfer がかかっていない |
| glTF COLOR_0 / factor 保護 | raw linear multiplier のまま(decode されたら fail) |
| transparent overlap / UI overlap | linear blend の合成結果(解析値と比較) |
| additive | 加算合成の飽和挙動 |
| bloom threshold | linear luminance 閾値の抽出範囲(解析可能な単色板) |
| hdr off / hdr on | 経路 1/2 の golden(tone curve 1 回・overlay 消失なし) |
| **hdr 複合 graph gate(新設)** | example 相当の graph(bloom chain + UI + debug overlay)を hdr off/on 両方で描画。trace で「hdr on: bloom は tonemap 前 / overlay は tonemap 後 / output_transform は最後に 1 個」を検査し、pixel golden も持つ(既存 hdr_on golden の合成 graph では観測できない複合経路を埋める) |
| bloom round-trip budget | 既知単色板の bloom chain 通過を解析値と比較(hop 数固定 + 1 hop ±1/255 budget — §2-6) |
| RPC capture 絶対色 | §2-7 の RPC known-value |
| frame-plan trace | §3-5 の pass 列 text golden(全経路)。**3 カウンタ(tone curve / terminal encode / paired round-trip)を個別記録**(§0-4)— paired は resource edge ごとに `linear→SRGB storage→linear sample` の対として記録し、terminal encode と混同しない |

## 5. HDR ディスプレイ出力(将来トラック — 席だけ予約)

- HDR10: `A2B10G10R10_UNORM` + `VK_COLOR_SPACE_HDR10_ST2084`(PQ)
- scRGB(Windows): `R16G16B16A16_SFLOAT` + extended sRGB linear
- 差し込み点は §2-1 の `output_transform`(SDR sRGB / HDR10 PQ / scRGB の
  切替点)。**本設計はエンコードの正しさのみを扱い、トーンカーブ
  (ACES/AgX 等)の選択は別トラック**(現行カーブ = hdr off: ACES 近似 /
  hdr on: Reinhard を維持)

## 6. 実装順(WP 候補)と未決事項

| 段階 | 内容 | 依存 |
|------|------|------|
| C0 | 色空間監査 → `color_migration_manifest.json`(コード変更なし)。§2-1 capability 表・§2-2 anchor 写像・§2-7 consumer 欄を manifest 項目に含める | なし(先行可) |
| C1a | **canonical anchor + format_class + output_transform の graph rewrite**(色意味論は変えない: display/中間はいったん UNORM のまま、output_transform は passthrough)— trace fixture で構造だけ先に固定し、golden は不変のまま通す | C0、**WP70 後** |
| C1b | **色意味論の一括移行**(SRGB view 化 + shader 分岐 + view/複製戦略 + authored decode + contract 2)+ §3 手続きの再基準化 + §4 テスト常設(単独ゲート) | C1a |

v2 の「C1 一括」を撤回し 2 段に分割: graph rewrite は RT 宣言・pipeline
compatibility・descriptor・barrier を横断する大変更であり(round 2 指摘)、
「構造の変更(pixel 不変)」と「色の変更(全再基準化)」を同一ゲートに
入れると diff の原因分離ができない。C1a は golden 不変が合格条件なので
安全に先行できる。

未決:

1. exposure 制御(カメラ/シーン単位)— tonemap トラックと同時
2. トーンカーブの選択肢(ACES / AgX)・グレーディング/OCIO — 別トラック
3. bloom threshold/intensity の pass param 化(M2b-2 の render_state 系列)
4. wide gamut / working space 切替(Rec.2020 等)— 需要が出たら
