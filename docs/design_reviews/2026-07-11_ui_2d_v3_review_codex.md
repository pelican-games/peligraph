# 2D/UI 基盤設計 v3 再々レビュー

## 判定: **Reject**

対象: `docs/design_ui_2d_foundation.md` v3 ドラフトおよび
`docs/design_input_actions.md` §4.2 (2026-07-11)

v3 は v2 の Reject 理由を大きく減らした。R1 の vertex/index/run/blend、R2 の
5 段フレーム位相、R3 の PSD supported subset、R4 の commit 境界・Controller 権限・
stable identity、R6 の byte-exact gate は本文へ入った。しかし、R1 の color/output
ABI、R3 の整数レイアウト、R4 の semantic payload schema、R5 の fixture 交換形式は
まだ複数の非同値実装を許す。R1〜R6 の「全解消」には至っておらず、現版を U0 の
実装正本にはできない。

Reject は全作業の停止を意味しない。後述のとおり、R2 に基づく FrameInput WP は
独立に着手できる。一方、U0 は schema・layout・semantic fixture 自体を実装する段階
なので、B1〜B4 を正本へ反映するまで開始しない。

## 1. Blocker findings

### B1 [R1/C1/C11] color transfer と出力 attachment の契約が現行経路と矛盾する

20B vertex、offset `0/8/16`、`uint16` index、`u32` の run 範囲、pipeline を含む
完全な run key、blend 値、容量の数え方と診断は固定された
(`docs/design_ui_2d_foundation.md:29-64`)。この部分は R1 を解消している。

残る問題は color/output ABI である。v3 は authored sRGB をパース時に linear u8 へ
変換し、texture を sRGB view で読み、既存の「sRGB format」へ linear を書けば
hardware が encode すると規定する (`docs/design_ui_2d_foundation.md:58-61`)。しかし現行の
swapchain 選択は `R8G8B8A8_UNORM` / `B8G8R8A8_UNORM` を優先し
(`src/core/vkcore/swapchainframetarget.cpp:52-62`)、選んだ UNORM format をそのまま
attachment にする (`src/core/vkcore/swapchainframetarget.cpp:71-84`)。headless の color
attachment も `R8G8B8A8_UNORM` 固定である
(`src/core/vkcore/offscreenframetarget.cpp:91-103`)。`*_UNORM` view は shader の linear
出力を sRGB OETF で encode しないため、「既存 sRGB format が hardware encode」は
現行経路では成立しない。

正本で、少なくとも次のどちらか一方に閉じる必要がある。

1. color attachment を列挙した `*_SRGB` view に統一し、linear shader output を
   hardware encode する。
2. `*_UNORM` を維持し、どの段で sRGB OETF を適用するかを shader/pass 契約として
   固定する。

併せて authored sRGB8 → linear UNORM8 の式、clamp、tie rounding と、swapchain・
headless・中間 attachment ごとの format を列挙すること。これがないと同じ JSON 色が
異なる vertex byte / golden になり、R1 と R6 の前提が閉じない。U0 の color parser と
draw command 生成、および U1 の pipeline 実装を止める条件である。

### B2 [R3/C4/C7/C11] fractional anchor を整数 `ui_units` に落とす規則がない

左上原点、Y 下向き、edge rect、draw 発行時の px 丸め、丸め後 rect による hit は
明記された (`docs/design_ui_2d_foundation.md:139-153`)。constraint の二段 pass、循環
エラー、panel intrinsic からの anchor 子除外、PSD 変換表も追加された
(`docs/design_ui_2d_foundation.md:215-256`)。これらは R3 の主要部分を解消している。

一方、layout は整数 `ui_units` で計算するとし
(`docs/design_ui_2d_foundation.md:142-149`)、anchor は `[0,1]²` の割合とする
(`docs/design_ui_2d_foundation.md:224-226`)が、`parent_edge + parent_size * anchor`
が非整数になる場合の表現・丸めがない。非 stack 子は anchor と offsets で「直接確定」
するだけで (`docs/design_ui_2d_foundation.md:228-242`)、pixel snap は px 変換の一箇所だけ
とされる (`docs/design_ui_2d_foundation.md:244-245`)。例えば幅 101 の親に anchor 0.5 を
置いた edge は 50.5 ui_units となり、整数 canonical space と両立しない。

anchor 値の数値表現、各 edge の式、符号付き right/bottom offset の向き、非整数
ui_units の一意な丸めを規範化すること。また fill の余り配分は「余り px」ではなく
canonical な ui_units として記述し、min/max clamp 後に残りが負の場合も min を破らない
規則へ閉じる必要がある (`docs/design_ui_2d_foundation.md:233-238`)。これは U0 の layout、
hit、semantic fixture expected 値を止める条件である。

### B3 [R4/C5/C6] semantic payload の例はあるが field schema が閉じていない

frame 一括 commit と凍結 tree の帰結、Controller の ECS read-only 権限、
`(document_key, stable_id)` の値コピー、reload 時の未 commit command 破棄と status は
明確になった (`docs/design_ui_2d_foundation.md:179-208`)。R4 のこの部分は解消している。

しかし v2 の修正条件は、event 名、payload field path、source、型、required/default、
単位、範囲を document schema に固定することだった
(`docs/design_reviews/2026-07-10_ui_2d_v2_review_codex.md:144-152`)。v3 の例が持つ field
属性は `name`、`from`、および static の `value` だけで
(`docs/design_ui_2d_foundation.md:192-201`)、型の参照元、required/default、単位、範囲、
nested field path の表現は定義されていない。「型不一致をロード時エラー」とだけ書いても、
何と照合するかが一意でない。

event registry/schema への安定した参照方法を含む完全な field 定義を置き、valid / invalid
fixture で unknown event、unknown source、型不一致、範囲外、required 欠落を固定すること。
これは U0 の document parser と U2 の E1 emit を止める条件である。

### B4 [R5/C11] `pelican.ui_semantic_fixture` はまだ再実装可能な schema v1 ではない

v2 が要求したのは header だけでなく、`event_seq` の JSON 表現、input/effect enum、
optional 規則、error trace 形式、安定名、配列順までを固定した交換形式だった
(`docs/design_reviews/2026-07-10_ui_2d_v2_review_codex.md:155-167`)。

v3 は JSON 例と、rect/identity/安定名/配列順/optional の規約を追加した
(`docs/design_ui_2d_foundation.md:318-345`)。ただし次が不足する。

- `event_seq` は C++ 側で `u64` とされた (`docs/design_ui_2d_foundation.md:126-131`)のに、
  JSON 例は number の `17` である (`docs/design_ui_2d_foundation.md:333-336`)。全 u64 を
  number として受けるのか decimal string とするのか、範囲・canonical 表現がない。
- `kind` と `effects` は「schema に列挙」と書くだけで、その schema/列挙値が本文にない。
  `button`、`consumed` control、`lifecycle`、error trace の形と enum も定義されていない
  (`docs/design_ui_2d_foundation.md:333-345`)。
- valid / invalid / expected 一式は U0 gate にすると宣言するだけで、必須ケース、期待する
  error code/path、fixture の canonical 比較規則がない
  (`docs/design_ui_2d_foundation.md:340-345`)。

実装言語ごとに同じ trace を読み書きできる closed schema（JSON Schema ファイルでも、
本文の型・enum 表でもよい）と、最低一組の normative valid / invalid / expected を定義する
こと。R5 は未解消であり、semantic fixture 全通過を gate とする U0
(`docs/design_ui_2d_foundation.md:365-371`)は現状では合否判定不能である。

## 2. R1〜R6 解消照合

| 要請 | 判定 | 照合結果 |
|---|---|---|
| R1 | **部分解消** | 20B/offset/stride、index、static assert、run key、blend、上限診断は解消 (`docs/design_ui_2d_foundation.md:29-64`)。color transfer と attachment format は B1。 |
| R2 | **解消** | 5 段 phase と E1 swap (`docs/design_ui_2d_foundation.md:107-120`)、frame 粗粒度 mask と制限 (`docs/design_ui_2d_foundation.md:121-124`)、WP (a)〜(g) (`docs/design_ui_2d_foundation.md:126-135`)を規定。入力正本も ordered InputEvent + frame marker へ改訂済み (`docs/design_input_actions.md:89-101`)。 |
| R3 | **部分解消** | 座標・edge・draw/hit の px 丸め、二段解法、循環、anchor 形式、panel intrinsic、PSD 表は入った (`docs/design_ui_2d_foundation.md:139-153`, `docs/design_ui_2d_foundation.md:215-256`)。fractional anchor の整数化が B2。 |
| R4 | **部分解消** | commit/権限/identity/reload status は解消 (`docs/design_ui_2d_foundation.md:179-208`)。payload field schema は B3。 |
| R5 | **未解消** | 例と基本規約は増えたが、u64 表現、closed enum、lifecycle/error trace、normative fixture が不足 (B4)。 |
| R6 | **解消** | 旧経路/共通経路を同一 fixture で byte-exact 比較し、tolerance 0 を維持、理由付き versioned baseline 更新後も 0 に戻す U2 gate が明記された (`docs/design_ui_2d_foundation.md:369-371`)。ImGui device loss の扱いも導入 WP の明示事項に戻った (`docs/design_ui_2d_foundation.md:347-363`)。 |

## 3. 実装開始を止める条件

### U0 を止める条件

1. B1 の color value / transfer / attachment format が一つの ABI に閉じるまで、color parser
   と draw command の実装を開始しない。
2. B2 の anchor edge 算出と整数化が決まるまで、layout/hit/expected fixture の実装を
   開始しない。
3. B3 の payload field schema と invalid ケースが決まるまで、document parser と semantic
   emit を実装しない。
4. B4 の closed schema と normative fixture ができるまで、「semantic fixture 全通過」を
   U0 gate として運用しない。

### FrameInput WP を止める条件

R2 自体に開始ブロッカーは残らない。5 段 phase、coarse mask、rpc/replay 同値条件を正として
着手してよい。ただし WP の受け入れ前に、`ordered_events` の所有者、span の有効区間、frame
終了後の保持禁止、`event_seq` の reset/単調増加 scope をテスト可能な文にすること。現状は
「frame 中 immutable」と「span の寿命規約を受け入れ範囲に含める」までである
(`docs/design_ui_2d_foundation.md:99-104`, `docs/design_ui_2d_foundation.md:126-131`)。

## 4. 実装と並行してよい条件

- FrameInput WP の queue swap、freeze、Actions 一回評価、通常/rpc/replay 位相同値 test。
- R6 の U2 compatibility fixture/harness の準備。共通 color ABI を仮定した baseline 更新は
  B1 解消後に限る。
- ImGui 導入 WP で device loss 時の backend resource 再生成を supported / unsupported の
  どちらにするか決定し、unsupported なら診断と復旧境界を定義する作業。
- PSD converter の unsupported-case fixture、reload/capture lifecycle fixture のケース設計。

## 5. 非 blocker の文書修正

- 文書 status は v3 だがタイトル、実装順、未決事項の見出しが v2 のままである
  (`docs/design_ui_2d_foundation.md:1-6`, `docs/design_ui_2d_foundation.md:365-379`)。版識別を
  v3 に統一する。
- 完全な run merge key には pipeline が入っている一方、run 分割節の再記述は
  texture/sampler/clip だけを列挙する (`docs/design_ui_2d_foundation.md:48-52`,
  `docs/design_ui_2d_foundation.md:67-74`)。前者が正であることが明確になるよう後者にも
  `pipeline_key` を再掲する。

## 6. 再審査条件

1. B1 の color/output ABI を現行 swapchain/headless 経路を含めて閉じる。
2. B2 の fractional anchor と min/max/negative remaining を整数規範にする。
3. B3 の semantic payload field schema と invalid fixture を定義する。
4. B4 の closed semantic fixture schema、enum/error trace、normative fixture を定義する。

この四点が正本へ入れば R1〜R6 は機械検証可能になり、v3 は Accept 判定の対象になる。
