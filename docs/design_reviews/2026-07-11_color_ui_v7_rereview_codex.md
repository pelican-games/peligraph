# 色 v4 / UI v7 再審査 (round 4)

対象: commit `974b924`。

## 冒頭判定

| 対象 | 判定 | 要約 |
|---|---|---|
| `docs/design_color_pipeline.md` v4 | **条件付き Accept** | round 3 の誤差 budget、canonical anchor、基本 capability bit、C1a byte 不変 gate は解消した。ただし C1a の正とした transfer copy は、実 target 側の `TRANSFER_DST` usage/capability と転送前後 layout を契約しておらず、現状の windowed swapchain では成立しない。C-C1〜C-C2 を C1a の実装 WP に添付すること。 |
| `docs/design_ui_2d_foundation.md` v7 + schema/fixtures | **Reject** | decimal/FP/overflow は改善し、Ajv の期待分類 29/29 と valid trace の I11 整合は再現した。しかし I3 は `_ui` と `_px` を直接交差させ、I6 は fixture に独立な総 index 数がなく、I11 も effect-kind と button 状態を閉じていない。round 3 が要求した coverage manifest schema/実在・集合一致 gate も未提出である。 |
| `implementation_plan.md` WP71 の E-C1〜E-C6 添付 | **未完（要修正）** | round 3 §3.4 を正本参照した点と E-C5 の選択は妥当。ただし要約は E-C1/E-C3/E-C4 の条件を落とし、E-C6 では `Typed(empty)` に payload 省略を許すようにも読めて正本の「厳密 `{}`」と衝突する。WP71 単体で実装者が誤読しない粒度へ直す必要がある。 |

従って色 C0 は続行可、C1a は本レビューの条件を WP に添付後に開始可とする。
UI U0 は開始不可。WP71 も下記 §3 の添付修正が済むまで「条件添付済み」と扱ってはならない。

## 検証範囲

- round 3 の再審査条件は `docs/design_reviews/2026-07-11_color_ui_v6_rereview_codex.md:72-145`、`:170-239`、`:290-349` を正本とした。
- Ajv CLI 5 / Ajv 8、draft 2020-12、all-errors で `normative/valid` 4 件、`semantic_invalid` 10 件、`invalid` 15 件を再検証した。前二者 14 件は schema-valid、後者 15 件は schema-invalid となり、**29/29 の期待分類は再現**した。
- Vulkan の transfer copy 条件は公式 `vkCmdCopyImage` reference の Valid Usage（source/destination usage、format feature、layout）と `VkSurfaceCapabilitiesKHR::supportedUsageFlags` を照合した。
- 作業ツリーには本レビュー以前からの変更・未追跡ファイルがある。本レビュー文書以外には触れていない。

---

# 1. 色パイプライン v4

## 1.1 round 3 指摘との照合

| round 3 指摘 | round 4 判定 | 根拠 |
|---|---|---|
| storage edge と最終出力の誤差単位を分離し、hop 加算則を撤回 | **解消（表記条件 1 件）** | v3 の `±1/255(linear) × hop` を明示撤回し、各 storage edge を 8-bit code ±1、最終値を golden、hop 数を構造 gate に分けた (`docs/design_color_pipeline.md:268-286`)。常設 fixture も同じ規則を参照する (`docs/design_color_pipeline.md:379-382`)。 |
| anchor の一意全順序 + postprocess の規範参照化 | **解消** | `pelican_ui → debug_draw → debug_text → imgui → output_transform` を別行の全順序にした (`docs/design_color_pipeline.md:147-160`)。postprocess 側も旧列・旧色方針を撤回し v4 §2-2 への規範参照に置換した (`docs/design_postprocess_temporal.md:69-84`)。 |
| capability 基本 bit + windowed capture の条件付き契約 | **capture について解消** | `COLOR_ATTACHMENT` / `SAMPLED_IMAGE` を基本 bit として追加した (`docs/design_color_pipeline.md:118-124`)。windowed capture は surface `TRANSFER_SRC` 非対応時に `unavailable_windowed` と RPC error を返す (`docs/design_color_pipeline.md:308-314`)。ただし C1a output copy の destination 条件は §1.2 のとおり別に不足する。 |
| C1a の byte 不変 4 条件 | **方式は解消、Vulkan 前提は未完** | 完全写像 + frame-plan diff、resolver version、bit-preserving copy、旧/new byte/hash gate が揃った (`docs/design_color_pipeline.md:401-420`)。transfer copy の成立条件だけが閉じていない。 |

誤差規則の内容は受理する。ただし見出しが「最終出力検査（単位 = linear / 実質 golden）」なのに、実際の比較対象は「最終 RGBA8 golden」である (`docs/design_color_pipeline.md:278-280`)。round 3 が求めた選択は「feature 固有の linear bound」**又は**「edge code budget + final golden」だったため、ここは後者である。C-C2 で単位名を正すこと。

## 1.2 [条件 C-C1] C1a transfer copy の swapchain layout/usage 前提を閉じる

C1a は `output_transform` の正を同 format の `vkCmdCopyImage` 系 transfer copy とした (`docs/design_color_pipeline.md:412-416`)。しかし capability 表が swapchain に要求する transfer usage は windowed capture 用の `TRANSFER_SRC` だけである (`docs/design_color_pipeline.md:118-122`)。現実装も swapchain image usage は `COLOR_ATTACHMENT` のみ (`src/core/vkcore/swapchainframetarget.cpp:71-84`)。

Vulkan の `vkCmdCopyImage` では、source image は `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`、destination image は `VK_IMAGE_USAGE_TRANSFER_DST_BIT` で作成され、source/destination format feature にそれぞれ transfer bit が必要である。layout も source は `TRANSFER_SRC_OPTIMAL` 又は `GENERAL`、destination は `TRANSFER_DST_OPTIMAL` 又は `GENERAL` 等でなければならない。presentable image に追加できる usage は surface の `supportedUsageFlags` に制約される（[Vulkan `vkCmdCopyImage`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImage.html)、[Vulkan `VkSurfaceCapabilitiesKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/VkSurfaceCapabilitiesKHR.html)）。

従って C1a WP に次をそのまま添付すること。

1. `display` を `TRANSFER_SRC`、windowed swapchain と headless/offscreen の実 target を `TRANSFER_DST` で作る。windowed は `supportedUsageFlags & TRANSFER_DST` を swapchain 作成前に照会し、対応時だけ swapchain `imageUsage` に追加する。format feature の transfer source/destination bit も role 表で照会する。
2. copy の直前に `display` を color-attachment write から `TRANSFER_SRC_OPTIMAL` へ、acquire 済み target を `TRANSFER_DST_OPTIMAL` へ同期し、copy 後は target を `PRESENT_SRC_KHR` 又は readback 契約が要求する layout へ遷移する。stage/access mask、queue ownership、初回旧 layout も frame-plan trace に含める。
3. C1a の source/destination は extent、sample count、texel block size、channel order を一致させる。resolver v1 の「旧 format と同一」規則 (`docs/design_color_pipeline.md:407-411`)を実 target との組にも適用し、copy/resolve の暗黙変換を許さない。
4. `TRANSFER_DST` 非対応 surface では、(a) `docs/design_color_pipeline.md:413-416` の全 code byte-exact 実証済み shader copy を使う、又は (b) C1a windowed path を明示的に unsupported として開始しない。capture の `unavailable_windowed` は `TRANSFER_SRC` 不足だけを表すため、この destination 不足と混同しない。

これで「bit-preserving copy = transfer copy 正」は成立する。現記述のままでは windowed swapchain への copy は valid usage を満たさない。

## 1.3 [条件 C-C2] 最終 metric の単位表記を一意にする

`docs/design_color_pipeline.md:278-280` を次のどちらか一方に固定すること。

- 現方式を維持する場合: 「最終出力検査（単位 = encoded 8-bit RGBA code / golden）」とし、linear metric という語を削除する。
- linear 比較も行う場合: golden byte を IEC decode した値の比較なのか、量子化前 reference の比較なのか、channel ごとの bound を別途規定する。

本レビューは前者を想定する。これは round 3 の「最終値は golden へ分離」を満たす最小修正であり、再々審査は不要である。

---

# 2. UI 基盤 v7 + schema/fixtures

## 2.1 解消した点

- decimal token は UI loader 専用の `std::from_chars` wrapper と token→binary64 bit pattern fixture を正本化し、locale/JSON parser 既定経路を排除した (`docs/design_ui_2d_foundation.md:266-275`)。
- `/fp:strict` 相当、FMA contraction 無効、layout thread の `FE_TONEAREST` assert、rounding/locale 負 test を要求した (`docs/design_ui_2d_foundation.md:279-284`)。
- px 変換も binary64→floor→int64 とし、int32/float32 正確表現域を超えた場合を `limit_exceeded`、`rect_ui` を clip 前、`scissor_px` を framebuffer clip 後とした (`docs/design_ui_2d_foundation.md:293-307`)。
- widget schema に optional `overflow`（省略 = visible）を追加した (`docs/schemas/pelican.ui_semantic_fixture.schema.json:36-51`; `docs/design_ui_2d_foundation.md:486-489`)。
- semantic-invalid 10 件は全件 schema-valid で、それぞれ少なくとも表題の I1/I2/I3/I4/I5/I6/I8/I10/I11/I12 違反を持つ。schema-invalid 15 件を含め Ajv の期待分類は正しい。
- coverage の「全 enum」主張を小 enum 全値と key/pad 語彙 branch 代表へ狭めた方針自体は妥当 (`docs/design_ui_2d_foundation.md:514-521`; `test/fixtures/ui_semantic/normative/coverage.json:4-31`)。

## 2.2 [Blocker] I3 は coordinate space と unit が閉じていない

I3 は `clip_ui` を「`overflow: clip` な祖先の `rect_ui` と viewport `content_rect` の交差」とする (`docs/design_ui_2d_foundation.md:493-496`)。しかし schema にある viewport field は **`content_rect_px`** (`docs/schemas/pelican.ui_semantic_fixture.schema.json:12-24`)であり、`rect_ui` / `clip_ui` は ui_units である。異なる単位の数値配列をそのまま交差できない。

この不整合は valid fixture で実際に観測できる。`input_trace_variants.json` は framebuffer 1920×1080、`ui_scale = 1.5`、`content_rect_px = [0,0,1280,720]` (`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:4-7`)なのに、root の `rect_ui` / `clip_ui` も `[0,0,1280,720]` (`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:13-21`)である。`content_rect_px` を名称どおり px と解釈して scale 変換すれば同じ数値にはならない。

また layout 式は `parent_edge + parent_size × anchor` なので anchor 子の最終 edge は root-space absolute と推測できる (`docs/design_ui_2d_foundation.md:258-278`)が、semantic fixture の `rect_ui` が親相対か root-content absolute かは交換形式の規約 (`docs/design_ui_2d_foundation.md:430-434`)に明記されていない。stack 子についても同じ座標規則を明文化する必要がある。

再審査条件:

1. `rect_ui` / `clip_ui` は **root-content 原点の absolute ui_units** と明記する（親相対を選ぶなら validator が全祖先 offset を合成する規則へ変える）。
2. I3 の初期 clip を ui_units で持つ。推奨は fixture に `content_rect_ui` を追加して px field と分離すること。`content_rect_px` から導出するなら letterbox offset を含む `ViewportTransform` の逆変換・rounding を規範化する。
3. `input_trace_variants.json` を選択した単位規則に合わせ、I3 semantic-valid であることを専用 positive test にする。

## 2.3 [Blocker] I6 の「最終 end = 総 index 数」は fixture 単体では検査不能

I6 は run 0、run 間連続性、最終 end と総 index 数の一致、総 quad 上限を要求する (`docs/design_ui_2d_foundation.md:498`)。ところが交換形式の draw run は `first_index` と `index_count` しか持たず、生成 index buffer の独立な総 index 数を持たない (`docs/schemas/pelican.ui_semantic_fixture.schema.json:54-69`)。

「総 index 数」を `Σ run.index_count` と定義すると、run 0 = 0 かつ連続である限り最終 end と必ず等しくなり、生成 buffer 末尾の未参照 index や run が buffer を越える反例を検出できない。round 3 が求めた「最終 end = 生成 index buffer count」への回答になっていない。

fixture root に `total_index_count`（又は buffer summary）を required で追加し、最終 end との一致と各 run の範囲を照合すること。`i06_run_index_gap.json` は run 0 gap しか行使しない (`test/fixtures/ui_semantic/normative/semantic_invalid/i06_run_index_gap.json:15-23`)ため、末尾不一致と quad 上限の反例も追加する。

## 2.4 [Blocker] I11 は valid fixture と矛盾しないが、状態機械を閉じていない

指定された seq 14 には `target: "root"` が入り (`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:90-98`)、seq 15 の cancel/release はその capture 後である (`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:99-104`)。seq 10〜18 は、現 I11 の「capture 前提」「click/cancel 非同居」「hover 交互」と矛盾しない。従って特に問われた fixture 自身の不整合は解消した。

一方、I11 の規則 (`docs/design_ui_2d_foundation.md:503`)は次をまだ通す。

- target 付き `pointer_move` / `pointer_up` の `effects:["capture"]`（capture を `pointer_down` に限定していない）。
- capture 後の `pointer_move` に `release_capture` / `click`、又は `pointer_up` に `drag_start`（先行 down だけを要求し event kind と effect を結び付けていない）。
- pointer id が同じで button が異なる 2 件の down/capture。状態を `pointer_id(+button)` ごととする一方、move/cancel schema は button を持たない (`docs/schemas/pelican.ui_semantic_fixture.schema.json:91-116`)ため、どの capture の drag/cancel か一意に決まらない。
- `effects` が網羅的とする (`docs/design_ui_2d_foundation.md:506-508`)一方、pointer_up/cancel が capture を終了する際に `release_capture` / `cancel` を必須とする規則がない。

`i11_capture_without_target.json` は最初の一条件しか行使しない (`test/fixtures/ui_semantic/normative/semantic_invalid/i11_capture_without_target.json:24-33`)。状態を「pointer ごとに active button は最大 1」とするか、trace に button を保持するかを選び、event kind × state × allowed/required effects の遷移表と各不正遷移 fixture を置くこと。

## 2.5 [Blocker] coverage gate は宣言だけで実体がない

round 3 は coverage manifest 自体の schema と、schema の enum/const/oneOf branch・semantic invariant ID の**完全一致** gate を要求した (`docs/design_reviews/2026-07-11_color_ui_v6_rereview_codex.md:231-239`)。v7 も manifest schema 検証と機械抽出を行うと宣言する (`docs/design_ui_2d_foundation.md:510-525`)が、対象 commit の `docs/schemas/` には fixture schema 1 ファイルしかなく、coverage schema も CI validator も存在しない。

さらに:

- v7 は完全一致でなく `schema 集合 ⊆ manifest key 集合` とした (`docs/design_ui_2d_foundation.md:524-525`)ため、stale/誤記 key を拒否しない。
- coverage の I7/I9 の値は実在 path に説明文を連結した文字列である (`test/fixtures/ui_semantic/normative/coverage.json:98-110`)。記述どおり「全 referenced fixture の実在」を文字列で検査すれば、この二つは存在しない path になる。
- 文書は各 I に schema-valid/semantic-invalid fixture を置くと言う (`docs/design_ui_2d_foundation.md:522-523`)が、I7/I9 は schema-invalid fixture を参照する。schema 二重化ならそれ自体は妥当なので、主張を「schema で強制されない I は semantic_invalid、I7/I9 は schema-invalid」と正すべきである。
- invariant ID 一つへの file 一つの対応だけでは、I5/I6/I10/I11 の複数 clause を被覆したことにならない。少なくとも I11 は §2.4 の反例が未被覆である。

coverage manifest schema を追加し、値を `{ "fixture": "...", "gate": "schema|semantic", "clause": "..." }` 等の構造にすること。抽出した required key/clause 集合と manifest 集合を完全一致させ、全 path 実在・期待 schema 分類・期待 semantic invariant を実行する CI test を U0 開始前 gate とする。

## 2.6 その他の整合修正

- schema description がまだ UI v6 を参照する (`docs/schemas/pelican.ui_semantic_fixture.schema.json:5`)ため v7 に更新する。
- I10 は missing を許す capture_cancel reason を `remove | reload | scene_unload` とした (`docs/design_ui_2d_foundation.md:502`)。valid lifecycle fixture は全 reason で widget を残している (`test/fixtures/ui_semantic/normative/valid/lifecycle_variants.json:33-73`)ため、missing を許す三分岐と許さない四分岐の両方を fixture 化する。

---

# 3. WP71 の E-C1〜E-C6 添付確認

## 3.1 添付として評価できる点

- WP71 は EventPayloadSchema v2 と round 3 §3 を正本として明示し、E-C1〜E-C6 を acceptance criteria に含めるとした (`docs/implementation_plan.md:1474-1485`)。
- E-C2 は許容された二案から registration compile error を選び、loadable/default/nothrow と constructor side-effect test を明示した (`docs/implementation_plan.md:1490-1493`)。
- E-C5 は推奨案 1（Opaque by-name 廃止）を明示選択し、UI binding 自体を拒否した (`docs/implementation_plan.md:1500-1502`)。Event 設計側の状態表も同じ選択へ更新済み (`docs/design_event_payload_schema.md:101-106`)。
- E-C6 の行列は round 3 §3.4 を正と明記した (`docs/implementation_plan.md:1503-1505`)。参照自体は追跡可能である。

## 3.2 添付未完とする差分

### E-C1

round 3 は empty/duplicate name だけでなく、range kind、min/max、NaN/Inf、F32 bound の表現可能性を constant evaluation で失敗させ、それぞれ compile-fail fixture にする条件だった (`docs/design_reviews/2026-07-11_color_ui_v6_rereview_codex.md:290-296`)。WP71 の要約は「空名/重複名/不正 range」だけで、NaN/Inf と F32 representability が落ちている (`docs/implementation_plan.md:1487-1489`)。

### E-C3

round 3 は scalar、String、Vec/Quat、integer token、2^53 境界を含む全 JSON shape を要求した (`docs/design_reviews/2026-07-11_color_ui_v6_rereview_codex.md:306-312`)。WP71 は Vec/Quat と integer だけを列挙し、scalar/String を落としている (`docs/implementation_plan.md:1494-1496`)。

### E-C4

catalog CI と主要 drift fixture は入った (`docs/implementation_plan.md:1497-1499`)が、plugin/遅延 registration を許す場合に module registration 完了時にも一致検査を走らせる条件が落ちている。原条件は `docs/design_reviews/2026-07-11_color_ui_v6_rereview_codex.md:314-319`。plugin/遅延登録を禁止するなら、その禁止を WP71 に書けば条件を閉じられる。

### E-C6

正本の最低行列は Payloadless だけを「payload 省略又は `{}` の選択」、Typed(empty) を RPC で「厳密 `{}`」とした (`docs/design_reviews/2026-07-11_color_ui_v6_rereview_codex.md:335-345`)。WP71 は両者をまとめて「payload 省略 or 厳密 `{}` を固定」と書く (`docs/implementation_plan.md:1503-1505`)ため、Typed(empty) の payload 省略を選べるように読める。直前に「レビュー §3.4 の表が正」とあっても同じ acceptance paragraph 内で矛盾している。

## 3.3 WP71 の修正条件

1. E-C1 と E-C3 は上記の脱落項目を列挙へ戻す。
2. E-C4 は「plugin/late registration を禁止」又は「module registration 完了時にも catalog 一致検査」のどちらかを明示する。
3. E-C6 は最低 5 行の state 行列を WP71 本文に複製し、RPC shape を `Payloadless = 省略か {}` の選択、`Typed(empty) = 厳密 {}` と分離する。選択した Payloadless 規則もこの WP で確定する。
4. `受け入れ = E-C1〜E-C6 の各 fixture` (`docs/implementation_plan.md:1507`)を、compile-fail / runtime invalid / catalog drift / state-matrix case の具体的 test 群に展開する。E-C6 は単一 fixture ではなく行列全セルを行使する。

以上を反映すれば、EventPayloadSchema v2 の「条件付き Accept」自体を再審査する必要はない。今回の判定は設計方式への異議ではなく、WP71 への条件転記が正本と同値になっていない点に限る。

---

# 4. 最終ゲート

- **色 C0**: 続行可。
- **色 C1a**: C-C1〜C-C2 を実装 WP の acceptance criteria に添付後、開始可。C1a の byte/hash 完全一致 gate は維持する。
- **色 C1b**: C1a 完了後。v4 の storage edge ±1 code + final golden 規則で実装可。
- **WP71**: §3.3 の転記修正後に開始可。EventPayloadSchema v2 の設計判定は条件付き Accept のまま。
- **UI U0**: 開始不可。§2.2〜§2.5 を設計/schema/fixtures/coverage gate に反映して再審査する。

結論として、色 v4 は実装 WP に閉じた Vulkan 前提を足せば進められる段階に達した。UI v7 は round 3 の改善方向を実装可能な形へ近づけたが、semantic validator の「fixture 単体完結」と coverage の「機械的閉包」はまだ成立していない。
