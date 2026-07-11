# UI v8 再審査 (round 5) + WP71/72/73/74 添付確認

対象: commit `9190605a739c74a929ccd90315ea4038c64246b4`。

## 冒頭判定

| 対象 | 判定 | 要約 |
|---|---|---|
| `docs/design_ui_2d_foundation.md` v8 + schema/fixtures | **条件付き Accept** | I3 の単位分離、I6 の独立総数、I10 の三分岐、coverage の実ファイル化は round 4 blocker の方式を解消した。36 fixture の Ajv 分類と現行 coverage gate も再現した。ただし I11 は hover 初期状態・button/owner 一致・空 effects・multi-pointer 独立性を閉じず、coverage の「本文 clause との完全一致」は現 checker では成立しない。§6 の U-C1〜U-C4 を U0 WP に逐語添付する条件で開始可。 |
| WP71 (E-C1〜E-C6) | **添付確認済み** | E-C1 の 6 compile-fail 分類、E-C3 の全 JSON shape、E-C4 の plugin/遅延 registration 禁止、E-C6 の 5 行行列と具体的 test 群が復元された (`docs/implementation_plan.md:1487-1524`)。 |
| WP72 (C0) | **設計正本と整合** | manifest の項目、capability/anchor/consumer 監査は色 v4 の C0 と一致する (`docs/implementation_plan.md:1526-1537`; `docs/design_color_pipeline.md:344-348,402-402`)。 |
| WP73 (C1a + C-C1) | **内容同値、逐語条件は未達** | C-C1 の usage/layout/同一 format/非対応 surface の 4 条件は実質すべて入った (`docs/implementation_plan.md:1545-1564`)。しかし round 4 の文を「そのまま添付」という条件 (`docs/design_reviews/2026-07-11_color_ui_v7_rereview_codex.md:44-49`)に対し、語句を短縮・再構成しており逐語コピーではない。§6 WP-C1 の置換後に開始可。 |
| WP74 (C1b) | **設計正本と整合** | resolver v2、意味論一括移行、§3 の 6 手順、§4 常設 test を単独 gate とする内容は色 v4 C1b と一致する (`docs/implementation_plan.md:1566-1574`; `docs/design_color_pipeline.md:340-364,368-387,402-404`)。 |

色 v4 の C-C2 本文修正は成立している。§2-6 は storage edge を 8-bit code、最終値を encoded RGBA8 golden と分離し、linear 比較を行わないと明記した (`docs/design_color_pipeline.md:272-285`)。ただし冒頭の変更要約だけは依然「最終出力 = linear」と書く (`docs/design_color_pipeline.md:10-12`)ため、§6 DOC-C2 で本文と揃える。

最終ゲートは、**WP72 は続行可、WP71 は開始可、WP73 は WP-C1 後に開始可、WP74 は WP73 完了後、UI U0 は U-C1〜U-C4 を WP acceptance criteria に添付後に開始可**とする。条件は方式変更を要求せず U0 の test/validator 成果物で閉じるため、UI v8 自体を Reject にはしない。

## 1. 実行検証

### 1.1 36 fixture の schema 分類

Ajv CLI 5.0.0 (Ajv 8、draft 2020-12、`--all-errors`)で次を実行した。

```text
npx --yes ajv-cli@5.0.0 validate \
  -s docs/schemas/pelican.ui_semantic_fixture.schema.json \
  -d <group>/*.json --spec=draft2020 --all-errors
```

結果は次のとおりで、**36/36 の期待分類を再現**した。

| group | 件数 | 結果 |
|---|---:|---|
| `valid/` | 5 | 全件 schema-valid |
| `invalid/` | 15 | 全件 schema-invalid |
| `semantic_invalid/` | 16 | 全件 schema-valid |

fixture schema は viewport の `content_rect_ui` と root の `total_index_count` を required にし (`docs/schemas/pelican.ui_semantic_fixture.schema.json:8-25`)、`total_index_count` は 0〜98304・6 の倍数に閉じている (`docs/schemas/pelican.ui_semantic_fixture.schema.json:72-77`)。semantic-invalid が schema-valid であることも「構造と意味を分離する」規約と一致する (`docs/design_ui_2d_foundation.md:486-490`)。

coverage manifest 自体も coverage schema に対して Ajv valid だった。直接実行した
`node test/fixtures/ui_semantic/normative/check_coverage.mjs .` は
`coverage gate: all checks passed`、CTest 登録 #187 も
`ctest --test-dir build -C Debug -R '^ui_semantic_coverage_gate$' --output-on-failure`
で pass した。CTest の Node 登録と repo root 引数は正しい (`test/CMakeLists.txt:341-350`)。

ただし、この green は現在 checker が実装する「manifest の限定的構造検査、path 実在、enum key 集合、key/pad pattern 同期」の green であり、§4 で述べる未実装の semantic coverage を証明しない。

## 2. round 4 blocker との照合

### 2.1 I3: 主 blocker は解消、I12 の edge 丸めだけ条件

`rect_ui` / `clip_ui` / `position_ui` を root-content 原点の absolute ui_units とし、`content_rect_px` と別に `content_rect_ui` を必須化したため、I3 は同じ座標系・同じ単位の交差になった (`docs/design_ui_2d_foundation.md:430-440,499-501`)。schema も両 field を必須にする (`docs/schemas/pelican.ui_semantic_fixture.schema.json:15-25`)。`input_trace_variants.json` は 1920×1080 px、scale 1.5、1280×720 ui の対応へ直っている (`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:4-21`)。round 4 §2.2 の異単位交差は解消した。

I12 は二つの独立 field を照合するだけなので、I3 と検査上の循環はない。しかし §2-2 は「各 edge を個別に half-up」すると規定する (`docs/design_ui_2d_foundation.md:150-164`)のに、I12 は `round((right-left) × ui_scale)` と読める幅高式である (`docs/design_ui_2d_foundation.md:510`)。非ゼロ ui origin では両者は同値でない。例えば ui の x edge が `[1,2]`、scale 1.5 のとき、edge 個別変換の幅は `round(3)-round(1.5)=3-2=1`、幅を先に取る式は `round(1×1.5)=2` になる。

root-content 原点を採るなら `content_rect_ui.left = top = 0` を invariant にし、letterbox offset は `content_rect_px.left/top` だけに持たせるのが最小である。又は I12 を edge 個別の式へ直す。現在の I12 fixture は framebuffer containment だけを違反させ (`test/fixtures/ui_semantic/normative/semantic_invalid/i12_content_rect_outside.json:4-21`)、scale/half-up 境界を行使しない。

### 2.2 I6: 独立総数は解消、clause coverage は未完

I6 は run0、run 間連続、最終 end と独立 `total_index_count` の一致、上限の 4 clause に分かれた (`docs/design_ui_2d_foundation.md:503-504`)。`i06b_total_mismatch.json` は end=6、total=12 の独立不一致を作れている (`test/fixtures/ui_semantic/normative/semantic_invalid/i06b_total_mismatch.json:15-25`)ので、round 4 の「Σとの恒等でなく突き合わせる」blocker は解消した。

一方、coverage schema/manifest は `I6a_run0_zero` と `I6b_total_match` の 2 key しか持たない (`docs/schemas/pelican.ui_semantic_coverage.schema.json:55-64`; `test/fixtures/ui_semantic/normative/coverage.json:103-107`)。複数 run 間の gap/overlap と上限一次強制を clause として追跡していない。`i06_run_index_gap.json` も run が 1 本だけなので run0≠0 だけを行使し、run 間連続性は行使しない (`test/fixtures/ui_semantic/normative/semantic_invalid/i06_run_index_gap.json:27-43`)。

### 2.3 I10: 解消

I10 は通常の dangling 禁止、許可外 reason (`hide`) での missing 禁止、許可 reason (`remove|reload|scene_unload`) での missing positive の 3 key に分かれた (`docs/schemas/pelican.ui_semantic_coverage.schema.json:68-70`; `test/fixtures/ui_semantic/normative/coverage.json:110-112`)。これは「許す」を「不在必須」と誤ることも避けており、round 4 §2.6 を満たす。

## 3. I11 遷移表の残る穴

状態を pointer_id ごとの `idle | captured{button,drag_started}` とし、capture は down、up は release 必須、cancel は cancel+release 必須、drag 後 click 禁止にした点は正しい (`docs/design_ui_2d_foundation.md:509-527`)。提出された 4 negative fixture も各表題の違反を直接作る (`test/fixtures/ui_semantic/normative/coverage.json:113-116`)。

しかし、次の trace は実装者によって accept/reject が分かれる。

1. **hover の初期状態と owner がない**: 初回 move が `effects:["hover_exit"]` の trace を「交互」の違反とする規則がない。`hover_enter(root)` の次に別 target 上で `hover_exit` だけを出す場合も、単なる文字列交互なら通る。状態定義に hover owner がなく (`docs/design_ui_2d_foundation.md:509`)、表は「交互」としか書かない (`docs/design_ui_2d_foundation.md:519`)。
2. **idle cancel の zero-effect 表現が二つある**: schema では `effects` が optional (`docs/schemas/pelican.ui_semantic_fixture.schema.json:113-123`)で、表は idle cancel を「effects なし」とする (`docs/design_ui_2d_foundation.md:523`)。省略と `effects: []` のどちらを writer 正本とするか、semantic equality で同値扱いするかが未確定である。
3. **captured button/owner と up が照合されない**: `down(pointer=0, button=left, target=A, capture)` の後に `up(pointer=0, button=right, target=B, release_capture+click)` を置いても、現表の `pointer_up | captured` 行だけなら通る。§2-3 は同一 owner の press/release を click 条件にする (`docs/design_ui_2d_foundation.md:170-179`)が、I11 の状態は owner を保持せず、up の button/target 一致を要求しない。
4. **captured 中の別 button down が曖昧**: 表は captured 中の `pointer_down` 自体を許し、capture 系 effect だけを禁じる (`docs/design_ui_2d_foundation.md:518`)。その後、別 button の up が既存 capture を解放できるなら「active button は pointer あたり最大 1」という規範と衝突する。
5. **multi-pointer 独立性を行使していない**: valid trace は pointer 0 を release してから pointer 1 を capture/cancel し、その後 pointer 0 を再利用する直列例である (`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:146-238`)。pointer 0 と 1 を同時 capture し、一方の cancel/release が他方の state を変えない interleave はない。
6. **同一 event 内の effect 順序がない**: valid は `drag_start, drag` と `cancel, release_capture` の順を選ぶ (`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:129-161,183-190`)が、schema は unique な配列とするだけ (`docs/schemas/pelican.ui_semantic_fixture.schema.json:223-227`)で、逆順を許すか、effects を集合として比較するかが未規定である。

従って I11 は「主要違反 4 種の fixture がある」段階には達したが、kind × **完全な state** × effects の閉じた遷移系にはまだなっていない。

## 4. coverage schema/checker の評価

### 4.1 現在成立していること

- manifest entry は path/prose を分離し、`fixture/gate/clause` の構造になった (`docs/schemas/pelican.ui_semantic_coverage.schema.json:79-94`)。
- schema は group/key を required + `additionalProperties:false` で閉じる (`docs/schemas/pelican.ui_semantic_coverage.schema.json:13-77`)。
- checker は referenced path の実在を確認する (`test/fixtures/ui_semantic/normative/check_coverage.mjs:62-69`)。
- fixture schema から enum を抽出し manifest key と集合一致を取る (`test/fixtures/ui_semantic/normative/check_coverage.mjs:71-100`)。
- key/pad enum と pattern alternation の同期を検査する (`test/fixtures/ui_semantic/normative/check_coverage.mjs:102-111`)。

### 4.2 「invariant 追加時の workflow」はまだ成立しない

coverage schema の required key を閉じる方式は、**coverage schema 自身も同時に正しく更新された場合**には manifest の追加忘れを検出する。しかし checker が比較する semantic clause の正は coverage schema の手書き `required` 配列だけである (`test/fixtures/ui_semantic/normative/check_coverage.mjs:52-55`)。設計本文の invariant 表を読まないため、本文に I13 や I6/I11 の新 clause を追加し、coverage schema の更新も忘れた場合、gate は green のままである。schema description の「design doc に追加すれば fail」という主張 (`docs/schemas/pelican.ui_semantic_coverage.schema.json:5`)は現実装と一致しない。

現に required 配列は I6 の連続/上限、I12 の scale、I11 の hover/button/owner/idle cancel/multi-pointer を持たない (`docs/schemas/pelican.ui_semantic_coverage.schema.json:55-75`)。それでも checker は pass する。従って「本文 clause 集合と manifest 集合の完全一致」という v8 本文の主張 (`docs/design_ui_2d_foundation.md:541-547`)は未達である。

また、現 checker は full coverage schema validator と同値ではない。例えば coverage schema が required とする top-level `description` の存在・型 (`docs/schemas/pelican.ui_semantic_coverage.schema.json:8-12`)や `clause` の型/最大長 (`docs/schemas/pelican.ui_semantic_coverage.schema.json:80-88`)を native check は検査しない (`test/fixtures/ui_semantic/normative/check_coverage.mjs:35-60`)。現 manifest 自体は別途 Ajv valid だが、CTest は Ajv を呼ばず native check だけである。

さらに checker は entry が指す fixture に対象 enum 値が実在するか、`gate` と directory/期待分類が一致するか、指定 semantic clause だけを実際に fail/pass するかを検査しない。path existence の後は enum の **key** 集合だけを比較し (`test/fixtures/ui_semantic/normative/check_coverage.mjs:62-100`)、schema classification と semantic invariant 実行は将来の U0 C++ validator へ委ねると明記する (`test/fixtures/ui_semantic/normative/check_coverage.mjs:12-13`)。round 4 が求めた coverage witness の機械検証は U0 acceptance まで残っている。

## 5. WP71〜WP74 と色条件の詳細

### 5.1 WP71

round 4 §3.3 の修正はすべて反映された。

- E-C1: static storage 所有と、空名/重複名/range kind/min>max/NaN・Inf/F32 representability の 6 compile-fail (`docs/implementation_plan.md:1489-1493`)。
- E-C3: scalar 幅・符号・有限性、String、Vec/Quat、integer token、2^53 境界 (`docs/implementation_plan.md:1498-1501`)。
- E-C4: plugin/遅延 registration を本 WP では禁止し、将来緩和時の module 完了後再検査も記録 (`docs/implementation_plan.md:1502-1507`)。
- E-C6: 5 状態を複製し、Payloadless は省略又は `{}`、Typed(empty) は厳密 `{}` と分離 (`docs/implementation_plan.md:1511-1519`)。acceptance も compile/runtime/catalog/state matrix の具体群へ展開した (`docs/implementation_plan.md:1521-1524`)。

WP71 は条件添付済みとして開始してよい。

### 5.2 WP72 / WP74

WP72 は C0 manifest の全 field と追加監査項目を持ち (`docs/implementation_plan.md:1531-1537`)、色 v4 の C0 定義 (`docs/design_color_pipeline.md:344-348,402-402`)と齟齬がない。参照行に色 v4 §6 C0 も加えると追跡しやすいが、内容 blocker ではない。

WP74 は色 v4 の C1b 定義どおり、resolver v2、authored decode、view/複製、contract 2、analytic fixture、誤差検査、6 手順以外の再基準化禁止を含む (`docs/implementation_plan.md:1566-1574`; `docs/design_color_pipeline.md:340-364,368-387,402-404`)。齟齬なし。

### 5.3 WP73 / C-C2

WP73 の C-C1 は意味上は round 4 の 4 項を保存している。ただし依頼条件は「そのまま添付」であり (`docs/design_reviews/2026-07-11_color_ui_v7_rereview_codex.md:44-49`)、WP73 は例えば「acquire 済み target」「同期し」等を省き、括弧・語順も再構成した (`docs/implementation_plan.md:1550-1564`)。規範内容の欠落とは判定しないが、**逐語確認の答えは No** である。原文 4 項をそのまま置換すれば閉じる。

C-C2 は §2-6 で解消済み (`docs/design_color_pipeline.md:272-285`)。冒頭要約の stale な「最終出力 = linear」 (`docs/design_color_pipeline.md:10-12`)だけを encoded code/golden に直す。

## 6. 条件付き Accept の必須添付条件

以下は **U0 WP に添付できる受け入れ条件の粒度**で記す。このまま acceptance criteria と test list に複製すること。

### U-C1: I11 を完全な per-pointer state machine にする

1. 状態を最低でも `pointer_id → {capture: idle | {button, owner, drag_started}, hover: none | owner}` とし、各表行に pre-state、event 条件、許可/必須 effects、next-state を書く。
2. captured 中は `pointer_up.button == capture.button` を必須にし、captured move/up/click の owner/target 規則を §2-3 の「同一 owner」と一致させる。別 button down は reject するか、capture を変えない pressed-state として完全に定義する。
3. hover は初回 `hover_exit` を拒否し、owner 切替時の `hover_exit → hover_enter` の順序と target 対応を定義する。`drag_start+drag`、`cancel+release_capture` を配列順規範にするか effects を集合比較にするかも一方へ固定する。
4. idle `pointer_cancel` の zero effect は「`effects` 省略のみ」「空配列のみ」「semantic equality では同値」のいずれかを選び、writer と validator の規則を一致させる。
5. negative fixture を hover 初期/owner 不一致、up button 不一致、up/click owner 不一致、captured 中の別 button、idle cancel 非空、effect order に各 1 本追加する。positive fixture に pointer 0/1 の同時 capture を interleave し、一方の cancel/release 後も他方が保持される case を追加する。

### U-C2: I12 の変換方向と丸めを一意にする

1. `content_rect_ui` は `[0,0,width,height]` とするか、非ゼロ edge を許すなら §2-2 と同じ edge 個別 half-up 式で `content_rect_px` の extent を照合する。letterbox offset の所有 field と ui→px の式を明記する。
2. I12 を少なくとも containment と scale/edge-rounding の 2 clause に分け、scale 1.5、half 境界、1 px letterbox、非整除 framebuffer の positive/negative fixture を置く。

### U-C3: clause registry を単一の機械可読正本にする

1. invariant/clause ID の単一 registry を機械可読ファイルに置き、設計表と coverage schema の双方をそこから生成するか、checker が registry と manifest を直接完全一致比較する。設計本文だけに clause を追加しても CI が落ちる構成にする。
2. registry へ I6 の run0/連続/total/上限、I12 の containment/scale、U-C1 の I11 各違反クラスを全て登録する。schema 一次強制の clause も `gate:"schema"` で残す。

### U-C4: coverage entry を witness として実行する

1. CTest gate で coverage schema を完全に検証する。依存ゼロ native validator を維持するなら `description`、`clause`、全 required/type/length/minProperties まで JSON Schema と同値に検査する。
2. 各 enum entry は参照 fixture の正しい field にその値が実在すること、各 entry の `gate` と directory/期待 schema 分類が一致することを検査する。
3. U0 C++ semantic validator の最初の成果物として、全 semantic entry を clause ID 指定で実行し、negative は指定 clause を fail、positive は pass することを CTest に組み込む。36 fixture の Ajv 相当 schema 分類も同 gate に含め、`check_coverage.mjs` の green だけを「3 段 gate 完了」と扱わない。

### WP-C1: WP73 の C-C1 を逐語化する

`docs/implementation_plan.md` WP73 の 4 項を、round 4
`docs/design_reviews/2026-07-11_color_ui_v7_rereview_codex.md:46-49`
の 4 項で文字どおり置換する。意味の追加変更は不要。

### DOC-C2: 色 v4 冒頭の単位表記を同期する

`docs/design_color_pipeline.md:10-12` の「最終出力 = linear」を
「最終出力 = encoded 8-bit RGBA code / golden（linear 比較なし）」へ直し、
同文書 §2-6 (`docs/design_color_pipeline.md:272-285`) と一致させる。

以上を添付すれば UI v8 の設計方式を再審査する必要はない。U0 の implementation PR では U-C1〜U-C4 の fixture と CTest 実行結果を acceptance evidence とする。
