# 色 v3 / UI v6 / EventPayloadSchema v2 再審査 (round 3)

対象: commit `d31d032`。

## 冒頭判定

| 対象 | 判定 | 要約 |
|---|---|---|
| `docs/design_color_pipeline.md` v3 | **Reject** | 誤色 fallback、現状認識、capture versioning は改善した。しかし paired round-trip budget の単位と上限が成立せず、canonical anchor の「唯一の正本」化も未完、C1a の byte/pixel 不変条件も閉じていない。 |
| `docs/design_ui_2d_foundation.md` v6 + schema/fixtures | **Reject** | binary64 を正本にした判断と 19/19 の構造分類は妥当。しかし semantic validator の一部 invariant は現交換形式から計算不能で、状態機械上の反例も通す。coverage manifest も「全 enum 値」を実際には覆わない。 |
| `docs/design_event_payload_schema.md` v2 | **条件付き Accept** | explicit descriptor、static-init 中の構築廃止、型付き range、4 状態 lookup への方式転換は round 2 blocker を解消する。下記 E-C1〜E-C6 を EventPayloadSchema WP の必須受け入れ条件として添付すること。 |

従って、現状の文書のまま色 C1a/C1b と UI U0 を開始してはならない。
EventPayloadSchema WP だけは、本レビューの条件を WP 本文・acceptance test に取り込むことを条件に開始可とする。

## 検証範囲

- round 2 の blocker 正本は `docs/design_reviews/2026-07-11_color_ui_v5_rereview_codex.md:45-141`、`:155-230`、`:236-319` とした。
- 対象 3 文書だけでなく、既存の postprocess 正本、現行 example graph、HDR feature、event registry/archive 実装も照合した。
- JSON Schema は draft 2020-12、strict/all-errors の Ajv 8 系で再検証した。`valid/` 4 件は全件 valid、`invalid/` 15 件は全件 invalid となり、提出された **19/19 の期待分類自体は再現**した。fixture の所在と主張は `docs/design_ui_2d_foundation.md:476-500`、実 schema は `docs/schemas/pelican.ui_semantic_fixture.schema.json:1-247`。
- 作業ツリーには本レビュー以前からの変更があったため、それらには触れていない。本ファイル以外は変更していない。

---

# 1. 色パイプライン v3

## 1.1 round 2 blocker との照合

| round 2 指摘 | round 3 判定 | 根拠 |
|---|---|---|
| 誤色になる dual-use fallback | **解消** | mutable view 不可時は encoding-class ごとに image を複製し、複製不能は load error とした。UNORM 誤色継続を明示撤回している (`docs/design_color_pipeline.md:229-245`)。 |
| role 別 capability | **部分解消** | 表を追加した (`docs/design_color_pipeline.md:112-127`)。ただし §1.4 の feature bit と swapchain readback 条件が不足する。 |
| `before:present` の現状誤認 | **解消** | example では anchor 不在で compose 失敗することへ訂正した (`docs/design_color_pipeline.md:54-65`)。実 graph も `FinalBloomComposite → ui_pass` である (`projects/example/passes/main_rendering_config.json:297-316`)。 |
| canonical anchor / HDR graph rewrite | **部分解消** | format class と anchor 列を提示した (`docs/design_color_pipeline.md:140-170`)。しかし既存 postprocess 文書は旧正本のままで、列自体にも overlay 間の全順序がない (§1.3)。 |
| terminal encode と中間 storage conversion の分離 | **部分解消** | 3 counter と edge trace は正しい (`docs/design_color_pipeline.md:27-37`, `:355-359`)。一方、数値 budget は成立しない (§1.2)。 |
| debug vec4 / capture versioning | **解消** | 無印 vec4 を linear のまま維持し明示 `Pelican::srgb()` を追加する方針 (`docs/design_color_pipeline.md:193-197`, `:222-223`)、capture point と contract 2 (`docs/design_color_pipeline.md:280-310`)は round 2 への妥当な回答。 |

## 1.2 [Blocker] paired round-trip の `1 hop ±1/255` は単位も伝播則も成立しない

設計は「linear 換算で sRGB 量子化 1 step」を `±1/255` とし、経路合計 max を hop 数とする
(`docs/design_color_pipeline.md:262-272`)。これは encoded code space と linear space を混同している。

本書自身が採用する IEC EOTF (`docs/design_color_pipeline.md:225-227`)で計算すると、上端の 1 code step は

```
decode(255/255) - decode(254/255) = 0.008897902886...
1/255                                 = 0.003921568627...
```

である。最寄り量子化の半 step だけでも約 `0.00444895` となり、linear 値に対する
`±1/255` を超える。従って「linear 誤差」なら 1 hop の上限として偽である。
逆に encoded byte/code 値を測るなら単位は `±1 code` 又は `±1/255 encoded` と書くべきで、
`max = hop 数` も code 値であることを明示しなければ検査器を一意に実装できない。

さらに bloom は単なる恒等 round-trip ではない。downsample/filter、upsample、加算 composite を挟む
(`projects/example/passes/main_rendering_config.json:270-305`)ため、edge j で生じた量子化誤差は後続演算の gain で
増幅又は減衰する。最終誤差を hop 数だけで足す規則には根拠がない。特に複数入力の加算 composite は、
同じ hop 数でも入力枝数で worst case が変わる。

再審査条件:

1. 測定単位を一つに固定する。推奨は各 SRGB storage edge の **8-bit code 値**と、最終出力の
   **linear 値**を別 metric にすること。
2. reference は各 edge で IEC decode → 高精度 pass 演算 → IEC encode/規定量子化を行い、実装出力と比較する。
   「解析値」が量子化前か量子化後か、HW が許す丸め集合をどう扱うかも固定する。
3. 最終 linear budget は `hop × 1/255` でなく、sRGB の局所 step と各 pass の filter/composite gain を含む
   feature 固有 bound にする。又は最終値の単純 bound を捨て、全 storage edge の code budget +
   supported device/driver ごとの final golden に分離する。
4. 0、暗部境界、0.5、254/255 近傍、1.0、複数枝が同符号にずれる加算の test vector を置く。

§4 の `bloom round-trip budget` は現在この誤った規則をそのまま参照している
(`docs/design_color_pipeline.md:340-359`)ため、fixture 名を置いただけでは blocker は解消しない。

## 1.3 [Blocker] canonical anchor はまだ一つの正本になっていない

色 v3 は自分の列を「唯一の正本」とし、postprocess/UI 文書を参照へ改めると宣言する
(`docs/design_color_pipeline.md:140-155`)。UI v6 は実際に独自列を撤回して色 v3 を参照した
(`docs/design_ui_2d_foundation.md:355-363`)。一方、`design_postprocess_temporal.md` は依然として
`scene_color → post_main → tonemap → post_ldr → swapchain` を自文書の標準列として規定し、
scene_color は常に linear HDR、tonemap 後に swapchain と記す
(`docs/design_postprocess_temporal.md:69-90`)。これは次と衝突する。

- 色 v3 では hdr off の `scene` format class は SRGB、tone curve は lighting shader 内である
  (`docs/design_color_pipeline.md:133-138`, `:162-167`)。
- `post_ldr` 以後は `display` へ書き、実 target の前には必ず `output_transform` がある
  (`docs/design_color_pipeline.md:96-108`, `:157-161`)。
- postprocess 文書の末尾 `swapchain` は、色 v3 の後方互換 alias と canonical terminal のどちらなのか決まらない。

また色 v3 の `[anchor: debug_draw / debug_text / imgui]` は 3 anchor の**集合**にしか見えず、相互の全順序を
定義していない (`docs/design_color_pipeline.md:146-154`)。UI v6 の要約列は `debug_text → imgui` で
`debug_draw` を落としている (`docs/design_ui_2d_foundation.md:357-361`)。frame plan の全順序を text golden に
する方針 (`docs/design_color_pipeline.md:332-334`)と両立させるには、3 anchor を別行にし、同じ文言・順序を
全参照文書で共有する必要がある。

再審査条件は、`design_postprocess_temporal.md` §3 を色 v3 への規範参照へ変更し、
canonical 列を少なくとも
`post_main → tonemap → post_ldr → pelican_ui → debug_draw → debug_text → imgui → output_transform → present/readback`
のような一意の全順序として固定すること。順序を別に選ぶのはよいが、slash で同順位にしてはならない。

## 1.4 [Major] capability 表は capture contract と全 resource usage をまだ閉じない

表の `display` と `lit_color/bloom` 行は attachment blend と linear filter を挙げる
(`docs/design_color_pipeline.md:117-123`)が、少なくとも次が欠ける。

- color attachment として使う format は `COLOR_ATTACHMENT`、blend するものはさらに
  `COLOR_ATTACHMENT_BLEND` の双方を要求する。blend bit だけを完全な集合として扱わないこと。
- `lit_color/bloom` は sample されるため `SAMPLED_IMAGE` と、linear filter を使う edge では
  `SAMPLED_IMAGE_FILTER_LINEAR` の双方が必要。現在の行は前者を落としている。
- default capture は output_transform の**実 target**を読む契約である
  (`docs/design_color_pipeline.md:284-293`)。windowed swapchain も対象なら surface の
  `supportedUsageFlags` に `TRANSFER_SRC` があることと swapchain image usage への追加、非対応時の正しい代替を
  表に入れる必要がある。現実装は swapchain usage が color attachment のみ
  (`src/core/vkcore/swapchainframetarget.cpp:71-84`)で、windowed readback は明示的に未対応
  (`src/core/vkcore/swapchainframetarget.cpp:316-317`)である。

windowed capture を contract 2 の対象外にする選択も可能だが、その場合は
`readbackLastFrameRGBA8 / CLI / RPC / snapshot は全て同じ最終 byte を読む` という現在の無条件文を狭め、
`get_status` で capability/error を規定すること。

## 1.5 [Blocker] C1a は「UNORM のまま」だけでは pixel/byte 不変にならない

C1a は canonical anchor、format_class、別 `display`、常設 output_transform を導入しながら、
中間は UNORM、output_transform は passthrough として golden 不変を要求する
(`docs/design_color_pipeline.md:370-382`)。しかし format が同じ UNORM であることは必要条件にすぎない。

現 graph は `FinalBloomComposite` が swapchain を load/write し、その後 `ui_pass` が同じ swapchain を使う
(`projects/example/passes/main_rendering_config.json:297-316`)。C1a 後は、この結果を別 image `display` に保存し、
追加 pass で sample/write する。次のいずれかが変われば byte 不変ではない。

- 既存 pass から canonical anchor への写像による pass 順、load/store/clear、blend state、MSAA resolve。
- passthrough の sample method、filter、texel center、format/channel swizzle、blend/write mask。
- display/target の R8/B8 format 対応、alpha、extent、barrier/layout。
- `format_class` の C1a 暫定解決規則。通常規則は hdr off の scene を SRGB にする
  (`docs/design_color_pipeline.md:162-167`)のに、C1a は UNORM 維持とだけ書かれている。

加えて「既存 golden を更新しない」だけでは byte 不変の証明にならない。既存 suite には非ゼロ tolerance があり、
それを維持するとしている (`docs/design_color_pipeline.md:327-330`)ため、小差分を隠せる。

C1a の開始条件:

1. 旧 pass/resource → canonical anchor/format_class の完全な写像表と、順序・load/store/clear/blend/MSAA が不変で
   あることを frame-plan diff で検査する。
2. C1a 専用の format resolver mode を明示し、全 color/display/scene を旧 UNORM と同じ channel order/sample count
   に解決する。C1b で初めて SRGB/16F 規則へ version を進める。
3. output_transform の C1a path は bit-preserving copy を規範化する。最も明瞭なのは同 format の transfer copy。
   fullscreen shader を使うなら texel 対応、filter/blend 無効、全 256 code × RGBA の往復が byte exact であることを
   supported device set で先に実証する。
4. tolerance 付き golden とは別に、旧/new binary の最終 RGBA8 hash/byte 比較を C1a gate にする。
   差が 1 byte でもあれば C1a では baseline 更新せず、写像又は copy path を修正する。

この条件が入れば「構造」と「色」を C1a/C1b に分ける判断自体は妥当である。

---

# 2. UI 基盤 v6 と semantic schema/fixtures

## 2.1 round 2 blocker との照合

| round 2 指摘 | round 3 判定 | 根拠 |
|---|---|---|
| half boundary の規範矛盾 | **原則解消** | `+0.5` を含む各演算の binary64/RN-even を正本にし、`nextafter(0.5,0) → 1` を明示した (`docs/design_ui_2d_foundation.md:256-277`)。 |
| layout 整数 overflow | **部分解消** | int64 と final int32 error を決めた (`docs/design_ui_2d_foundation.md:278-286`)。ただし px 変換後の範囲と parser/FP environment gate は §2.2 の条件が必要。 |
| structural schema を semantic と呼ぶ問題 | **方向は解消、閉包は未達** | schema と semantic invariant を分離し第三 gate にした (`docs/design_ui_2d_foundation.md:458-475`)。ただし一部 invariant は入力不足で実装不能、重要な反例を列挙していない (§2.3)。 |
| fixture/enum/invalid coverage | **部分解消** | valid 4 / invalid 15 の構造分類は再現した。manifest の「全 enum」主張は偽で、semantic-invalid fixture がない (§2.4)。 |

## 2.2 [Major] binary64 の式は閉じたが、decimal parse と FP environment の実装契約が閉じていない

`t1/t2/t3` の演算列、FMA/fast-math/x87 禁止まで書いた点は正しい
(`docs/design_ui_2d_foundation.md:261-277`)。`std::floor` 自体は、この範囲の有限 binary64 が入力なら
結果の整数値を一意にできる。残る可搬性問題は式より入口と実行環境である。

- 「JSON decimal を correctly-rounded binary64」と要求するだけで、どの parser/conversion algorithm を正本にするか
  がない (`docs/design_ui_2d_foundation.md:261-269`)。stdlib の `strtod`、JSON library、locale、現 rounding mode に
  依存させず、同じ token を同じ bits にする実装又は wrapper が必要。
- RN-even は process 起動時の一回設定だけでなく、UI layout を実行する thread の FP environment と compile flags の
  contract である。gate は結果 vector だけでなく、MSVC `/fp:strict` 又は同等設定、`FE_TONEAREST` の assert、
  FMA contraction 無効を対象 target ごとに検査する必要がある。
- final UI rect の int32 error はあるが、別段の `edge × ui_scale` は `ui_scale ≤ 16`
  (`docs/schemas/pelican.ui_semantic_fixture.schema.json:17-24`)なので int32 edge から int32 を超える px edge を作れる。
  px rect は schema 上 int32 (`docs/schemas/pelican.ui_semantic_fixture.schema.json:185-200`)で、描画 ABI は float2
  (`docs/design_ui_2d_foundation.md:33-39`)である。px 変換後の int64 中間、int32/float 表現範囲超過時の
  `limit_exceeded`、又は framebuffer clip 前後のどちらを記録するかを固定すること。

U0 条件は、decimal midpoint/隣接値を含む token→`uint64_t` bit pattern fixture、全 round vector、
locale/rounding mode を意図的に変えた負 test、px overflow test を Windows/MSVC と CI の他 compiler で通すこと。

## 2.3 [Blocker] 9 invariant は網羅的でなく、一つは現 fixture から計算不能

第三 gate の導入は正しいが (`docs/design_ui_2d_foundation.md:458-475`)、現在の 9 行だけでは次を拒否できない。

1. **clip equality は計算不能**: invariant は `clip_ui` が「祖先 clip チェーンの交差に等しい」とする
   (`docs/design_ui_2d_foundation.md:466-474`)。しかし widget record は id/type/rect/clip/layer/decl_seq だけで、
   parent handle、各祖先の `overflow`、clip source がない
   (`docs/schemas/pelican.ui_semantic_fixture.schema.json:36-50`)。slash 区切り id から親名を推測できても、
   祖先が fixture に存在する保証と overflow 状態がない。validator の入力を fixture 単体とするなら schema に
   `parent_id` と clip source/overflow を足すか、validator の入力を `(source document, layout snapshot, fixture)` と明記し
   revision 一致を検査する必要がある。
2. **最初の draw run が自由**: 「前 run の end と一致」は run 0 を拘束しない
   (`docs/design_ui_2d_foundation.md:471-472`)。`first_index=6,index_count=6` の 1 run fixture は schema-valid かつ
   現 invariant-valid になる。run 0 は 0、最終 end は生成 index buffer count と一致、全 run の合計 quad 上限を
   検査すること。
3. **scissor の framebuffer containment がない**: rect ordering だけでは負 offset や framebuffer 外 scissor を通す。
   `0 ≤ left ≤ right ≤ width`、`0 ≤ top ≤ bottom ≤ height`、必要なら content rect との関係を規定すること。
4. **入力/capture 状態機械を検査しない**: §2-3 は press が hit widget を capture すると定義する
   (`docs/design_ui_2d_foundation.md:163-177`)のに、提出 valid fixture は target のない pointer_down に
   `effects:["capture"]` を付ける (`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:80-93`)。
   現 9 invariant はこれを通す。同じく down 無し click/up、owner 無し cancel/release、pointer/button 不一致、
   `click` と `cancel` の同居も通る。pointer ごとの capture/press state と effect-kind 整合を invariant にするか、
   `effects` は網羅的 semantic output でないと明記して名称を下げる必要がある。
5. **参照整合の方針がない**: input `target`、lifecycle `widget` が widget list に存在すべき時点、remove/reload 後に
   dangling path を記録してよい条件がない。単純な存在検査で済まないため、lifecycle/input の時系列規則として定義する。

少なくとも上記を閉じ、各 invariant に「一つだけ違反する schema-valid / semantic-invalid fixture」を 1 件以上置くまで、
第三 gate は実在する受け入れ基準にならない。

## 2.4 [Blocker] coverage manifest の「全 enum 値」主張は実データと一致しない

manifest は全 enum 値・oneOf branch・invalid class を対応付け、schema enum の未記載を CI failure にすると宣言する
(`test/fixtures/ui_semantic/normative/coverage.json:1-5`)。しかし consumed は `mouse/key/touch/pad` の代表 1 件ずつしか
列挙しない (`test/fixtures/ui_semantic/normative/coverage.json:27-31`)。schema が正本と明記する `ui_key` 全値と
`ui_pad_control` 全値 (`docs/schemas/pelican.ui_semantic_fixture.schema.json:226-241`)は manifest にない。
実 fixture も `key:space` と `pad:dpad_up` の各一例だけである
(`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:80-87`)。

さらに invalid manifest は schema-invalid class だけ (`test/fixtures/ui_semantic/normative/coverage.json:81-96`)で、
§2.3 の semantic invariant 9 行の coverage section/fixture を持たない。従って 19/19 は「現在置かれた 19 file の
構造分類が合う」証拠であって、schema branch と semantic contract の閉包証明ではない。

再審査条件:

1. coverage manifest 自体の schema を置き、schema 内の全 `enum/const/oneOf` branch と semantic invariant ID を
   機械抽出した集合が manifest key 集合と完全一致することを CI で検査する。
2. `ui_key` と `ui_pad_control` は全値を列挙する。語彙 class 代表だけに緩和するなら、文書と manifest の
   「全 enum 値」を「各語彙 branch」に改め、enum 追加を検出する別 test を置く。
3. schema-invalid だけでなく semantic-invalid coverage を追加し、各 invariant と §2.3 の状態機械規則を反例で行使する。

---

# 3. EventPayloadSchema v2

## 3.1 round 2 blocker の解消判定

次の方式転換は受理する。

- schema の正本を default instance の `ref()` 実行結果から explicit static descriptor へ変更した
  (`docs/design_event_payload_schema.md:57-92`)。
- recording archive は init phase の sequence-level debug 一致検査へ降格した
  (`docs/design_event_payload_schema.md:121-137`)。
- static registration は constructor/ref を実行しない (`docs/design_event_payload_schema.md:86-89`)。
- exact scalar width、Quat、VecN、型付き integer range をモデル化し (`docs/design_event_payload_schema.md:26-54`)、
  archive API を変更しない (`docs/design_event_payload_schema.md:105-119`)。
- lookup は Unknown/Payloadless/Opaque/Typed を返す (`docs/design_event_payload_schema.md:139-153`)。

現 target は C++20 である (`src/core/CMakeLists.txt:3-5`)。member pointer から field type を導出し、
`std::array`/`std::string_view`/`std::variant` を constexpr 構築する方式は C++20/MSVC で実装可能であり、
方式自体を Reject する理由はない。ただし、例だけでは storage lifetime と不正型をどこで即時失敗させるかが閉じないため、
以下を WP 条件にする。

## 3.2 二重管理が実務で壊れるシナリオ

descriptor は type drift を member pointer で防げるが、`ref()` の名前列とは依然二重管理である
(`docs/design_event_payload_schema.md:63-79`, `:121-134`)。具体的には:

- member を `ref()` だけに追加: descriptor prevalidation はその field を required と認識せず、検証後の `.at()` で throw。
- member を descriptor だけに追加: caller に field を要求するが `ref()` は読まず、release では値を黙って破棄。
- 片側だけ rename/reorder: UI/エディタが見せる schema と実 load key/order がずれる。
- 条件分岐 `ref`: default debug input で通らない枝だけ release payload で追加 field を読む。
- Debug init を通さない Release-only binary、plugin/DLL の遅延登録、テスト catalog に入らない event では一致検査自体が走らない。

文書はこの限界を認める (`docs/design_event_payload_schema.md:130-137`)が、「レビュー対象」だけでは運用 gate にならない。
以下 E-C4 を必須とする。

## 3.3 Payloadless と Typed(fields 0)は区別を維持すべき

区別は必要である。`Typed(fields 0)` は作者が空 object schema を明示し、schema-driven consumer に参加する意思を示す。
`Payloadless` は `ref` 自体がなく、既存の単なる signal event を表す。将来 optional field/schema version を足す際にも移行意図が異なる。
v2 の 4 状態 API はこの区別を返せる (`docs/design_event_payload_schema.md:139-153`)ので維持してよい。

ただし payload の受理規則を閉じる必要がある。現実装では `ref` のない default-constructible event の loader は
渡された JSON を無視する (`src/core/userpublic/details/event/registerer.hpp:60-74`)。これは
`unknown_fields_reject` を全 by-name 経路へ適用する方針 (`docs/design_event_payload_schema.md:105-116`)と両立しない。
Payloadless と Typed(empty) はどちらも by-name 入力を厳密な `{}` のみにするのか、payload 自体の省略を許すのかを決め、
余分な key を黙って捨ててはならない。

## 3.4 条件付き Accept の必須条件

以下を EventPayloadSchema WP にそのまま添付すること。

### E-C1: descriptor の所有権と consteval surface

`pelican_payload` は event 型の static storage にある `std::array<PayloadFieldSchema,N>` 相当を**所有**し、
registry の `span` はその static object だけを指すこと。`payloadFields()` が一時 array への span を返す実装は禁止。
空名、重複名、range 種別、min/max、NaN/Inf、F32 bound の表現可能性を `consteval` 又は constant-evaluation を
強制する initializer で失敗させる。MSVC の compile-pass 1 件と各 compile-fail fixture を CI に置く。
根拠: schema は非 owning span (`docs/design_event_payload_schema.md:42-54`)だが、例の `payloadFields()` の戻り型・所有者は
未規定 (`docs/design_event_payload_schema.md:75-92`)。

### E-C2: Typed の registration 成立条件

`pelican_payload` を持つ型は `default_initializable` かつ `ISerializable<EventType, JsonArchiveLoader>` でなければ
registration compile error にする、又は Typed とは別に `TypedNotLoadable` 状態を設計する。現 loader はこの条件を
満たさないと null になり (`src/core/userpublic/details/event/registerer.hpp:59-74`)、descriptor があっても検証後に
構築できない。debug 一致検査を行う Typed には `nothrow_default_constructible` も static_assert し、
副作用なしは coding rule + counting-constructor test で担保する。

### E-C3: JSON shape と「archive 変更ゼロ」を具体化

prevalidator は scalar だけでなく Vec2/3/4 と Quat の array 長、各要素の number 型・有限性・範囲、String、
integer token (`1.0` を integer と認めるか)、2^53 境界を規範化する。全 invalid case で
`load_json_payload` 呼出回数、constructor 回数、pending event 数が 0 のままであることを検査する。
現 archive は vector/quat を添字で読むだけ (`src/core/userpublic/serialize/jsonarchive.cpp:39-53`)なので、
構築前 validation が形を完全に閉じなければならない。

### E-C4: descriptor/ref 全 catalog 一致 gate

全 registered Typed event を列挙する test binary を作り、名前・順序・重複込みの一致検査を CI の必須 test とする。
Debug 手動起動だけに依存しない。field 追加忘れ、descriptor だけ追加、rename、reorder、duplicate、条件分岐の既知限界を
個別 fixture にする。plugin/遅延 registration を許すなら、その module registration 完了時にも同じ検査を走らせる。
可能なら field list macro/宣言から descriptor と `ref` の両方を生成し、二重記述を局所化する。

### E-C5: Opaque と global unknown-field policy の矛盾を解消

文書は Opaque の by-name emit を従来どおり可とする (`docs/design_event_payload_schema.md:94-103`)一方、
全 by-name JSON を descriptor で事前検証し unknown field を拒否するとする
(`docs/design_event_payload_schema.md:105-116`)。Opaque には descriptor がないので両方は実装できない。
次のどちらかを明示選択すること。

1. Opaque の by-name emit を廃止し、typed C++ emit のみ許す。推奨。
2. Opaque は明示的な legacy-unvalidated 例外とし、RPC/UI/replay からは拒否、内部の限定 API だけに残す。

UI は Opaque を「fields を指定した時だけ」でなく event binding 自体で拒否すること。schema がない以上、fields 省略でも
required payload を構築できる保証がない。Unknown は `unknown_event`、Opaque は `no_payload_event`、Payloadless/Typed(empty)
は E-C6 の空 payload 規則に従う。

### E-C6: 4 状態ごとの空 payload 行列

lookup state × UI binding × RPC by-name × JSON payload shape の表を WP に置く。最低限:

| state | UI | RPC/by-name |
|---|---|---|
| Unknown | `unknown_event` | unknown event error |
| Opaque | `no_payload_event`、binding 不可 | E-C5 で廃止又は限定 legacy |
| Payloadless | fields 指定は `no_payload_event`、無指定 emit は可 | payload 省略又は `{}` の選択を固定、extra key 拒否 |
| Typed(empty) | schema 照合後に無指定/空 fields emit 可 | 厳密 `{}`、extra key 拒否 |
| Typed(nonempty) | required/type/range/unknown-field 検証 | 同じ validator と error category |

この行列と UI error enum は既に名前上は一致している
(`docs/design_ui_2d_foundation.md:437-448`, `docs/design_event_payload_schema.md:139-153`)ため、条件を満たせば
相互参照は閉じる。

---

# 4. 3 文書間の相互参照と開始ゲート

| 項目 | 判定 | 根拠 / 必要修正 |
|---|---|---|
| version 参照 | **概ね一致** | 色 v3 は UI v6 を参照 (`docs/design_color_pipeline.md:11-13`)、Event v2 は UI v6 を参照 (`docs/design_event_payload_schema.md:3-9`)、UI v6 は色 v3 と Event 正本を参照 (`docs/design_ui_2d_foundation.md:10-15`, `:200-206`)。ただし UI §9 見出しがまだ “v5” (`docs/design_ui_2d_foundation.md:520`)。 |
| anchor 正本 | **不一致** | UI は色 v3 へ統合済みだが、postprocess が旧列を規範化したまま。色の overlay anchor 間も全順序でない (§1.3)。 |
| 色依存 WP 名 | **曖昧** | UI は U1 を「色 C1 後」とする (`docs/design_ui_2d_foundation.md:62-68`, `:524-526`)が、色 v3 は C1a/C1b に分割した (`docs/design_color_pipeline.md:370-382`)。U1 は **C1b 完了後**と書き換えること。 |
| Event error code | **名前は一致、状態行列は未完** | `unknown_event` / `no_payload_event` は一致する (`docs/design_ui_2d_foundation.md:437-448`, `docs/design_event_payload_schema.md:151-153`)。Opaque、Payloadless、Typed(empty) の無指定 fields を E-C5/E-C6 で固定する。 |
| capture / display | **色文書内で未完** | capture point 一本化は改善したが、windowed swapchain capability/実装が未定 (§1.4)。 |

最終ゲート:

- **色 C0**: read-only 監査は続行可。capability manifest に §1.4 の不足 bit と windowed capture を追加する。
- **色 C1a/C1b**: 開始不可。§1.2〜§1.5 を設計へ反映して再審査する。
- **EventPayloadSchema WP**: E-C1〜E-C6 を WP acceptance criteria に添付する条件で開始可。
- **UI U0**: 開始不可。EventPayloadSchema 条件達成後、§2.2〜§2.4 を閉じ、schema-valid/semantic-invalid fixture を追加して再審査する。

結論として、round 3 は round 2 の「方式選択」blocker の多くを解消した。残る Reject の理由は実装量ではなく、
色の誤差 budget が異なる単位を混ぜていること、UI の第三 gate が現データから検査できない条件を含むこと、
そして canonical/coverage を「唯一・全件」と呼ぶ主張が実ファイルと一致しないことである。
