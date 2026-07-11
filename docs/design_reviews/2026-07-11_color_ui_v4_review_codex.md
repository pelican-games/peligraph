# 色パイプライン v1 / 2D・UI 基盤 v4 敵対レビュー

## 冒頭判定

- `docs/design_color_pipeline.md` v1: **Reject**
- `docs/design_ui_2d_foundation.md` v4: **Reject**

色設計の方向、すなわち「scene/合成は linear、表示 transfer は最後に一度だけ」は正しい。
しかし現案は、実装上すでに二箇所に分裂している tone curve の所有点、feature 合成後の
terminal output transform、同一 glTF image の color/data 二用途、alpha と scene-linear 値の
扱いを閉じていない。さらに一斉再基準化は、現存する非ゼロ tolerance と、RPC capture が
絶対色を一度も検証していない事実を見落としている。C1 を「単独ゲート」にすると、別の
回帰を新 baseline として合法化できる。

UI v4 は B2 の中心式を追加し、B4 の enum/error 形を前進させた。一方、B3 が照合先とする
event payload schema は現行 registry に存在せず、B4 は型・必須性・kind 別 variant・error
code・実 fixture を閉じていない。従って U0 の機械ゲートはまだ実装者ごとに別物になる。

---

# 1. 色パイプライン v1 レビュー

## 1.1 判定: **Reject**

### C1 [Blocker] 現状認識は半分正しいが、tone curve と OETF の実際の所有点を取り違えている

swapchain が `R8G8B8A8_UNORM` / `B8G8R8A8_UNORM` を優先し、選択 format をそのまま
attachment view に使うという認識は正しい
(`src/core/vkcore/swapchainframetarget.cpp:52-84`)。headless が
`R8G8B8A8_UNORM` 固定なのも正しい
(`src/core/vkcore/offscreenframetarget.cpp:91-105`)。従って §1 の
「現状は attachment が encode しない」は実装と一致する
(`docs/design_color_pipeline.md:18-24`)。

しかし「既存 tonemap curve が encode を含む場合」という仮定形では足りない
(`docs/design_color_pipeline.md:58-59`)。実装では次のように責務が分裂している。

- 通常 lighting shader は独自の tone curve の後で明示的に `pow(1/2.2)` まで行う
  (`src/core/resources/fullscreen.frag:226-236`)。
- `hdr` feature の `tonemap.frag` は Reinhard だけで、OETF は含まない
  (`src/core/resources/tonemap.frag:12-20`)。
- `hdr` feature は `lit_color` を RGBA16F に override し、その後の pass から直接
  `swapchain` へ書く (`src/core/resources/features/hdr.json:5-24`)。

つまり C0 が答えるべきなのは単なる「encode の有無」ではない。`hdr` off/on ごとに、
lighting tone curve、bloom、display tone curve、OETF のどれを残すかを一つの規範的な
pass 列にしなければならない。現案の「現行カーブ維持」
(`docs/design_color_pipeline.md:81-83`)では、通常 shader の curve と HDR feature の
Reinhard のどちらが「現行」か決まらず、二重 tone mapping 又は tone mapping 欠落を許す。

**修正条件**: `hdr off` / `hdr on` / headless / UNORM fallback の四経路について、
`scene-linear HDR → bloom 等 scene effect → tone curve → linear LDR overlay → OETF → present/readback`
の所有 pass と format を表にすること。各経路で tone curve 回数と OETF 回数を別々に検査すること。

### C2 [Blocker] 「authored 色」を一括 sRGB 扱いする規則は glTF、alpha、HDR 値を壊す

§2-2 は color texture と data texture を分ける点では正しいが、全 authored 色を JSON 上
sRGB とし、`[0,1]` clamp 後に linear u8 化する
(`docs/design_color_pipeline.md:38-48`)。この規則には三つの非同値な対象が混ざっている。

1. **RGB と alpha**: sRGB transfer を受けるのは RGB だけで、straight alpha/coverage は
   linear のままである。現文は RGBA のどの component に式をかけるかを限定していない。
   UI ABI は straight alpha を正本にしている
   (`docs/design_ui_2d_foundation.md:85-94`)ため、alpha まで decode する実装を許してはならない。
2. **display-authored RGB と scene-linear multiplier**: glTF の `baseColorFactor`、
   `emissiveFactor`、`COLOR_0` は texture の sRGB texelと同じ扱いではない。現 loader は
   `COLOR_0` と `baseColorFactor` を float のまま vertex color に入れる
   (`src/core/model/gltf.cpp:460-476`)。glTF 仕様も baseColor texture は sRGB decode、
   factor と `COLOR_0` は linear multiplier と定めている
   ([glTF 2.0 Specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html))。
   blanket な authored-sRGB 規則は、現在ここだけは正しい処理を逆に壊す。
3. **LDR color と radiometric/HDR 値**: light/emissive の値を常に `[0,1]` clamp すると、
   scene-linear の高輝度表現を失う。`intensity` だけを例外にしても、どの field が
   display color、linear multiplier、radiometric value かを schema で識別できない。

また「linear」だけでは primaries/white point がない。少なくとも v1 の working space を
`linear-sRGB / Rec.709 primaries, D65` と固定し、transfer と gamut を別概念にする必要がある。
UE が working color space と texture source encoding を別設定として扱うのもこの理由である
([UE Working Color Space](https://dev.epicgames.com/documentation/en-us/unreal-engine/working-color-space-in-unreal-engine))。

**修正条件**: schema の各 color field/resource に
`encoding = srgb | linear`、`role = color | data | radiometric`、RGB/alpha の別規則、
range/precision を持たせること。glTF field は glTF 仕様を正として個別表にし、既存 raw
`COLOR_0` を sRGB decode しない fixture を置くこと。

### C3 [Blocker] texture の用途別 format 規則は、現 loader の「image 一個 = GPU texture 一個」では実装できない

現在の material texture 登録は既定で常に `R8G8B8A8_UNORM`
(`src/core/material/materialcontainer.cpp:123-139`)であり、glTF loader は `model.textures` ごとに
一度だけ登録した `texture_map` を全 material slot で共有する
(`src/core/model/gltf.cpp:509-523`)。同じ image/texture が baseColor と data slot の両方から
参照された場合、§2-2 の「用途により SRGB/UNORM view を選ぶ」だけでは一つの view を
両立できない。

UI と debug text も現在は 8-bit image を用途にかかわらず UNORM に写像する
(`src/core/renderer/uicontainer.cpp:54-74`, `src/core/renderer/debugtext.cpp:54-63`)。
さらに frame-graph format parser 自体が `*_SRGB` を受け付けない
(`src/core/renderingpass/renderingpassjsonhelpers.cpp:9-23`)。

Unity が color texture の sRGB sampling と lookup/mask の linear sampling を asset 用途で
分け、Godot が shader uniform に `source_color` を要求するのと同様に、用途は loader の
暗黙推測ではなく resource/view 契約でなければならない
([Unity: Working with linear Textures](https://docs.unity3d.com/kr/2018.3/Manual/LinearRendering-LinearTextures.html),
[Godot: Shading language](https://docs.godotengine.org/en/4.3/tutorials/shaders/shader_reference/shading_language.html))。

**修正条件**: 同一 image に対する SRGB/UNORM の compatible view を許す mutable image 戦略、
又は用途ごとの複製/asset reject のいずれかを決めること。frame graph format enum、image format
feature 検査、UI atlas/font atlas、material default texture を変更対象表へ加えること。

### C4 [Blocker] UNORM fallback は「pass を一個挿す」だけでは frame graph 上成立しない

現案は UNORM swapchain 時に present 直前へ encode fullscreen pass を一個挿すとする
(`docs/design_color_pipeline.md:30-36`, `docs/design_color_pipeline.md:50-54`)。しかし現在の
main graph は bloom composite、UI が順に `swapchain` へ直接書き
(`projects/example/passes/main_rendering_config.json:297-316`)、debug draw/text feature も
`insert: end` で `swapchain` を load/write する
(`src/core/resources/features/debug_draw.json:5-16`,
`src/core/resources/features/debug_text.json:5-16`)。

encode pass には sampled source と別の destination が必要である。同じ swapchain image を
読みながら同じ image に書けないし、encode 後に `end` feature が走ればその出力だけ
未 encode になる。従って必要なのは pass の追加ではなく、全 terminal overlay を
`display_linear` 中間へ retarget し、feature 合成後の絶対最後に非 purgeable な
`output_transform` を置く graph rewrite である。SRGB swapchain 経路と fallback 経路で
overlay 順・blend・pipeline format が同一になることも必要である。

headless を `R8G8B8A8_SRGB` 固定とする規則
(`docs/design_color_pipeline.md:34`)にも format feature 検査と fallback がない。Vulkan では
color attachment 可否は format feature で決まるため、offscreen も
`COLOR_ATTACHMENT | TRANSFER_SRC` 対応を照会してから選ぶ必要がある。swapchain の
SRGB format 優先自体は妥当である
([Vulkan swapchain guide](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/01_Presentation/01_Swap_chain.html))。

**修正条件**: `swapchain` という名前を scene pass が直接触る構造をやめ、論理 terminal
resource と output transform を frame-plan schema に入れること。`UI → debug_text → imgui`
を含む合成後 trace、SRGB/UNORM の pixel parity、headless capability fallback を gate にすること。

### C5 [Blocker] linear blend の影響は半透明だけではなく、現 bloom の意味・精度・順序を変える

§2-3 は半透明の見えが変わることだけを明記する
(`docs/design_color_pipeline.md:55-59`)。現実には現在の lighting shader が tone curve と
`pow(1/2.2)` を行った後 (`src/core/resources/fullscreen.frag:226-236`)、既定では 8-bit UNORM の
`lit_color` と bloom RT 群へ流れる
(`projects/example/passes/main_rendering_config.json:39-43`,
`projects/example/passes/main_rendering_config.json:58-109`)。bloom threshold `0.4` はその値域に
対して評価される (`src/core/resources/bloom_highpass.frag:11-27`)。

linear 化後に同じ `0.4` を使うか、sRGB 0.4 相当へ変換するか、bloom を tone map 前の HDR
へ移すかで絵は全く異なる。加算 composite も固定 8-bit RT では pass ごとに clamp/quantize
される (`src/core/resources/bloom_composite.frag:12-21`)。これは「golden を更新」で済ませる
選択ではなく、effect の意味論を先に決める設計判断である。

particle/additive も同じである。texture RGB、vertex/tint、alpha、additive intensity のどれを
sRGB decode するか、straight/premultiplied/additive の pipeline key、従来 gamma-authoring の
互換方針が §2 にない。glTF vertex color を linear multiplier とする規則と同様、vertex color
を一律 authored sRGB にしてはならない。

**修正条件**: bloom を scene-linear HDR の tone map 前に置くか linear LDR の後に置くか、
各 RT format と threshold/intensity の単位を固定すること。transparent UI、重なり UI、
straight alpha edge、additive particle、bloom threshold の analytic fixture を追加すること。

### C6 [Blocker] capture/RPC の PNG 意味論は変わるが、既存 RPC test はその誤りを検出しない

readback は image byte をそのまま返し
(`src/core/vkcore/offscreenframetarget.cpp:172-205`)、PNG writer は channel swap 以外の
色変換や color metadata 付与をしない
(`src/core/vkcore/rendertarget.cpp:38-60`)。RPC `capture` も path しか返さない
(`src/core/communication/rpcserver.cpp:645-650`)。従って §3 の「readback/PNG は encoded sRGB
byte」という新契約 (`docs/design_color_pipeline.md:70-75`)は、公開 API と RPC の
observable semantics の変更である。

既存 RPC test は、PNG が非空で二枚が異なること
(`test/run_rpc_headless.cmake:237-255`)又は二回の実行で file byte が同じこと
(`test/run_rpc_inject_input_headless.cmake:190-221`)しか見ない。OETF 欠落、二重 OETF、
誤った alpha decode のいずれでも、この test は通り得る。「rpc test に波及しない」のではなく、
**既存 test が意味論変更を観測していない**。

**修正条件**: `readbackLastFrameRGBA8`、CLI `--render-out`、RPC `capture` の contract を
versioned にし、`encoding: "srgb"`、alpha、channel order を status/response 又は正本文書で
固定すること。RPC 経路にも known RGB/alpha の絶対 byte test を置き、DCC 利用者向け変更を
記録すること。PNG に sRGB chunk を付けるか、付けないなら「chunk 無しでも sRGB と解釈する」
API 契約を明記すること。

### C7 [Blocker] golden 一斉再基準化は現状のままでは本物の回帰を不可視化する

§3 は C1 で全 golden を再生成し、以後 tolerance 0/exact に戻すとする
(`docs/design_color_pipeline.md:61-75`)。しかし現リポジトリには既に
`stem_fullscreen` が average 1 / max 4
(`test/golden/stem_fullscreen/tolerance.json:1-4`)、`vat_playback` が average 2 / max 32
(`test/golden/vat_playback/tolerance.json:1-4`)で存在する。「更新後は全て 0」は現状の
非決定性又は実装差まで同じ C1 で解決しない限り虚偽になる。

さらに golden harness は環境変数一つで expected を描画結果へ上書きする
(`test/golden_image_test.cpp:1526-1544`)。設計書と PR を全 case 共通の理由にするだけでは、
unrelated shader/feature regression も一緒に承認できる。linear 0.5 と ramp は output transfer
だけを検査し、texture decode、alpha、blend、bloom、feature order、RPC capture の正しさを
証明しない。

**安全な一回移行の最低条件**:

1. C0 の成果物を path/field 単位の machine-readable manifest とし、各 shader、texture slot、
   RT、clear color、vertex color、capture consumer の旧 encoding・新 encoding・期待差を固定する。
2. 変更前 binary と変更後 binary を同一 device/driver/scene/input で実行し、old/new/analytic
   reference の三者を保存する。新 baseline を生成する前に case ごとの diff と理由を review する。
3. 既存 tolerance 非ゼロ case は色移行から分離し、0 にできないならその tolerance と理由を
   維持する。「色移行だから 0」に書き換えない。
4. encode parity だけでなく、SRGB texture decode、UNORM data texture、同一 image 二用途、
   RGB/alpha、linear vertex color、半透明重なり、additive、bloom、HDR off/on、UI/debug、
   raw readback/CLI/RPC を analytic fixture にする。
5. frame-plan/execution trace も同時に比較し、pixel が偶然近くても output transform の回数・
   位置が違えば失敗させる。

## 1.2 §2-1 棚卸し表の不足一覧

現表 (`docs/design_color_pipeline.md:28-36`) は attachment format 表としても C1 の実装対象表としても
不足している。最低限、次を追加すべきである。

| 対象 | 現在の実体 | 必須決定 |
|---|---|---|
| 通常 lighting / HDR lighting | inline curve + `pow` と HDR Reinhard が分裂 | hdr off/on の tone curve/OETF 所有点 |
| G-buffer albedo/emissive | 8-bit UNORM (`projects/example/passes/main_rendering_config.json:3-25`) | stored value が linear か encoded か、精度 |
| bloom 全 RT | 8-bit UNORM (`projects/example/passes/main_rendering_config.json:58-109`) | HDR/linear LDR、clamp、threshold |
| UI atlas | UNORM (`src/core/renderer/uicontainer.cpp:54-74`) | color は SRGB、mask/data は UNORM、alpha |
| debug font atlas | UNORM (`src/core/renderer/debugtext.cpp:54-63`) | alpha-only data と RGB の別扱い |
| debug_draw/text vertex color | shaderへ raw float (`src/core/resources/debug_draw.frag:3-8`, `src/core/resources/debug_text.frag:8-14`) | API argument が linear か sRGB か |
| glTF textures | 全 slot が単一 UNORM texture (`src/core/model/gltf.cpp:509-523`) | slot 別 viewと共有 image |
| glTF `COLOR_0` / factor | raw linear multiplier (`src/core/model/gltf.cpp:460-476`) | **sRGB decode しない** |
| VAT texture | RGBA16F data (`src/core/model/gltf.cpp:227-231`) | color migration 対象外であること |
| arbitrary frame-graph/compute output | format enum は用途を持たない (`src/core/renderingpass/renderingpassjsonhelpers.cpp:9-23`) | resource role/encoding metadata |
| capture/CLI/RPC | raw byte→PNG (`src/core/vkcore/rendertarget.cpp:38-60`) | encoded byte、alpha、metadata、version |

## 1.3 色設計の再審査条件

1. hdr off/on を含む tone curve と OETF の規範 pass 列を定義する。
2. working primaries、RGB/alpha、display-authored/linear multiplier/radiometric の field schema を分ける。
3. 同一 image の color/data 二用途と全 texture/RT の view/format/capability 方針を閉じる。
4. fallback を feature 合成後の terminal graph rewrite として定義する。
5. bloom・transparent・UI overlap・additive/particle の意味と fixture を定義する。
6. capture/RPC contract と絶対値 test を更新する。
7. 事前 manifest、三者比較、case ごとの承認を持つ一回 migration 手順へ改める。
8. 非ゼロ tolerance case を正直に扱い、「全て exact」という記述を現実と一致させる。

この八点が入るまで C1 を単独ゲートとして開始してはならない。C0 の read-only 棚卸し自体は
先行してよい。

---

# 2. 2D・UI 基盤 v4 レビュー

## 2.1 判定: **Reject**

## 2.2 v3 B1〜B4 の解消照合

| v3 blocker | 判定 | 再審査結果 |
|---|---|---|
| B1 color/output ABI | **未解消** | UI 文書から色正本を分離し、U1 を色 C1 後にした構造は正しい (`docs/design_ui_2d_foundation.md:58-64`, `docs/design_ui_2d_foundation.md:399-405`)。ただし参照先の色 v1 が本レビュー C1〜C7 を閉じていないため、実装可能な ABI にはまだなっていない。 |
| B2 fractional anchor | **部分解消** | anchor edge の式、offset の符号、ui_units への一回整数化は追加された (`docs/design_ui_2d_foundation.md:234-243`)。ただし round の数値規範と負の残りの algorithm が閉じていない。U0 layout blocker は残る。 |
| B3 semantic payload schema | **未解消** | field 属性は増えたが (`docs/design_ui_2d_foundation.md:196-215`)、照合先だとする event registry に schema reflection が存在しない。文書の事実認識が誤り。 |
| B4 semantic fixture closed schema | **未解消** | enum、event_seq 上限、error の外形は追加された (`docs/design_ui_2d_foundation.md:357-379`)。しかし型/required/variant/additional property/error code/canonical number と実 normative fixture がない。 |

### U-B2 [Blocker] 二重丸めそのものではないが、round の定義と overflow algorithm がまだ非決定的

anchor 計算時の `round_half_up` と §2-2 の px 変換時の `round` は、同じ境界を二度丸める
事故ではない。前者は real anchor を canonical integer `ui_units` に入れる量子化、後者は
`ui_units` を framebuffer px に写す量子化であり、設計が整数 layout を選ぶ限り二段あるのは
整合している (`docs/design_ui_2d_foundation.md:146-154`,
`docs/design_ui_2d_foundation.md:234-243`)。例えば width 101、anchor 0.5 は先に 51 ui_units
となり、以後はその canonical edge を scale する。この挙動を direct float→px と同値だと
期待してはならない。

残る blocker は次である。

- `round_half_up` の負値での定義がない。`floor(x+0.5)` と「絶対値を half-up 後に符号復元」は
  `-0.5` で異なる。親 edge/offset が負になり得る以上、式又は test vector が必要である。
- anchor の JSON number を binary32/binary64/fixed decimal のどれで評価するか、積和を
  どの精度・順序で行うかがない。half 境界付近で CPU/言語差を許す。
- 本文は fill 余りを ui_units と正しく書く (`docs/design_ui_2d_foundation.md:237-243`)一方、
  擬似 algorithm は「余り px」と書く (`docs/design_ui_2d_foundation.md:247-255`)。
- 「min を破らない」と「残りが負なら fill 子は 0」が同居する
  (`docs/design_ui_2d_foundation.md:241-243`, `docs/design_ui_2d_foundation.md:251-255`)。
  `min_size > 0` の fill 子で両方を満たせない。min 合計を配置して overflow させるのか、
  fill を 0 にして min を破るのかが未決である。

**修正条件**: anchor 数値型、演算順、全実数に対する round 式を固定し、`-1.5/-0.5/0.5/1.5`、
101×0.5、scale 1.5 の test vector を本文へ置くこと。negative remaining は min 合計を保持した
overflow rect と overflow clip の算出まで擬似 algorithm を一つにすること。

### U-B3 [Blocker] `PELICAN_REGISTER_EVENT` が payload schema を持つという主張は現実装では偽

v4 は UI load 時の型照合先を「`PELICAN_REGISTER_EVENT` が登録する payload 構築 schema」とし、
by-name emit が既にその schema を持つとする
(`docs/design_ui_2d_foundation.md:207-212`)。現行 `EventTypeRegistration` が保持するのは
`name`、C++ `type_index`、`load_json_payload` function pointer だけである
(`src/core/userpublic/details/event/registerer.hpp:32-38`)。macro も型名を渡して
`registerEvent<Type>` を呼ぶだけで schema を生成しない
(`src/core/userpublic/details/event/registerer.hpp:118-132`)。

serializable event の loader は emit 時に `event.ref(archive)` を実行するだけ
(`src/core/userpublic/details/event/registerer.hpp:54-66`)であり、field 一覧、required/default、
range、単位、nested path を事前列挙できない。`JsonArchiveLoader::prop` は JSON の `.at(name)`
をその場で型変換するだけである
(`src/core/userpublic/serialize/jsonarchive.cpp:6-49`)。さらに default-constructible だが
`ref` を持たない event は JSON payload を丸ごと無視して構築される
(`src/core/userpublic/details/event/registerer.hpp:67-70`)。現状の by-name emit
(`src/core/userpublic/details/event/registerer.cpp:57-70`)が提供するのは runtime construction
であって schema introspection ではない。

UI 側の field grammar 自体も、`min/max` を「宣言時」と述べるだけで JSON 形を示さず、
`widget_value(<プロパティパス>)` の path syntax、単位、event field への nested destination、
unknown payload field の reject 規則がない
(`docs/design_ui_2d_foundation.md:196-212`)。

**修正条件**: event 層に explicit `EventPayloadSchema` を追加し、registration が stable event name、
field path、scalar/vector 型、required/default、range、unit、unknown-field policy を公開すること。
`ref` から暗黙 reflection できるという前提は捨てること。UI U0 より前の依存 WP とし、
unknown event/source、type/range/required、non-serializable event を load-time fixture にすること。

### U-B4 [Blocker] enum 表を加えても `pelican.ui_semantic_fixture` は closed schema ではない

v4 の進歩は認める。`event_seq` を safe JSON number に制限し、kind/button/effects/lifecycle の
語彙と error object の外形を置いた
(`docs/design_ui_2d_foundation.md:363-379`)。しかし次が未定義である。

- root、viewport、document、widgets、draw_runs、input_trace、lifecycle の各 property の型、
  required/optional、配列長、数値範囲、`additionalProperties` の可否。
- `input_trace.kind` ごとの discriminated variant。例えば `pointer_move` に button は必要か、
  `pointer_cancel` に position/target は必要かがない。
- lifecycle 各 kind の必須 payload。`controller_init`、`document_swap`、`command_dropped` が
  同じ object shape では必要情報を表せない。
- `consumed` は例示形式だけで open vocabulary のまま
  (`docs/design_ui_2d_foundation.md:365-371`)。keyboard/touch/gamepad control 名を閉じていない。
- error の `code` は「安定コード」という placeholder だけで enum がなく、`path` の
  JSON Pointer 等の syntax、複数 error の順、invalid fixture の期待 error がない
  (`docs/design_ui_2d_foundation.md:373-379`)。
- canonical 化は key 順・空白しか述べず、integer/float、`-0`、exponent、Unicode escape を
  固定しない。semantic JSON comparison なのか serialized byte comparison なのかも曖昧。
- normative fixture は「schema と同時に commit」と未来形であり、リポジトリ内にはまだない。
  現在の U0 gate は実物なしでは判定不能である (`docs/design_ui_2d_foundation.md:377-379`)。

**修正条件**: JSON Schema 等の機械可読な closed schema を正本として置き、kind ごとの
`oneOf`、`additionalProperties:false`、error code/path、number 表現を閉じること。最低一組では
coverage が足りないため、各 enum variant と各 invalid class を normative fixture にすること。
比較は parse 後の semantic equality を正とするか、RFC 8785 等の canonicalization を名前で
固定すること。

## 2.3 非 blocker だが正本で直す点

- status は v4 なのにタイトルが「v2」のままである
  (`docs/design_ui_2d_foundation.md:1-7`)。fixture/schema version と文書 revision を混同しないよう
  タイトルを v4 に直す。
- anchor 節の注記「B4 v3→B2」は blocker ID の履歴として読みにくい
  (`docs/design_ui_2d_foundation.md:234-240`)。現在の ID に統一する。
- `event_seq` を 2^53−1 で生成エラーにするなら、長時間 session での reset scope と
  replay 境界も FrameInput 正本へ書く (`docs/design_ui_2d_foundation.md:129-139`,
  `docs/design_ui_2d_foundation.md:373-374`)。

## 2.4 UI v4 の再審査条件

1. 色 v1 の C1 が Accept され、UI color/alpha/attachment ABI の参照先が実装可能になる。
2. anchor の数値型・負値 round と negative remaining/min/overflow algorithm を閉じる。
3. event registry に実在する introspectable payload schema を設計し、UI U0 の依存に置く。
4. `pelican.ui_semantic_fixture` を機械可読 closed schema と実 normative fixtures で完成させる。

FrameInput の独立 WP は v3 レビューどおり先行してよい。U0 は layout expected、document emit
validation、semantic fixture gate 自体が B2〜B4 に依存するため、上記四点の反映前に開始しては
ならない。
