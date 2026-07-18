# editor tooling v2.3 §1-4〜§1-6 再レビュー

日付: 2026-07-18

対象: `docs/design_editor_tooling.md` v2.3 の §1-4 / §1-5 / §1-6 / §3

根拠ツリー: `agent/review-v23` (`da60b99`)

## 0. 結論

**Reject**。

V22-C1〜V22-C7 と snapshot 必須修正の本文転記は、空白・改行を除けば前回の
逐語と全件一致する。ここは受理する。しかし、前回 §11 が再レビュー入口にした
4 つの構造化提出物のうち、operation matrix、renderer state inventory、snapshot
JSON schema、WP owner 表が閉じていない。逐語ブロックを正しく置いても、その直後の
表が条件を省略・緩和しており、実装 WP の acceptance boundary にはまだできない。

| 対象 | 判定 | 要旨 |
|---|---|---|
| V22-C1〜V22-C7 + snapshot 必須修正の逐語転記 | **Accept** | 空白・改行を除いて 8 件すべて一致 |
| §1-6-1 operation / write-set matrix | **Reject** | ticket abort/forced-abort、preview lease、execution-time gate が閉じず、受理済み E-C4 の必須列もない |
| §1-6-2 renderer preview state inventory | **Reject** | 最低指定 7 行の名前はあるが、shared `FrameResources`、Renderer flat/xr history、resize state、RenderTiming query state が漏れ、history の分類も本文と矛盾する |
| §1-5-5 / §1-6-3 snapshot | **Reject** | source revision CAS の流れはあるが、要求された export/import **JSON schema** がない |
| §3 WP graph / owner 表 | **Reject** | 受理済み `STRUCT-SCHEMA0 → ED-CODEC0` を図から落とし、E-C2/E-C4/E-C5 と §1-6-3 の owner 添付もない |

従って E-RPC0-base/E-RPC1/JOURNAL0/PREVIEW0/SNAPSHOT0 の新規登録・着手を、この
文書を根拠に進めてはならない。既に完了した WP149/WP150/WP152 の再審ではない。

## 1. 逐語条件の監査

前回レビューの各 blockquote と v2.3 の対応 blockquote を、Markdown の `>` と空白・
改行だけを除いて比較した。

| 条件 | 前回 | v2.3 | 結果 |
|---|---|---|---|
| V22-C1 | 前回 `:69-85` | 本文 `:184-205` | 完全一致 |
| V22-C2 | 前回 `:106-114` | 本文 `:212-223` | 完全一致 |
| V22-C3 | 前回 `:140-151` | 本文 `:230-243` | 完全一致 |
| V22-C4 | 前回 `:178-192` | 本文 `:260-280` | 完全一致 |
| V22-C5 | 前回 `:218-231` | 本文 `:287-305` | 完全一致 |
| V22-C6 | 前回 `:265-280` | 本文 `:316-337` | 完全一致 |
| V22-C7 | 前回 `:301-309` | 本文 `:344-355` | 完全一致 |
| snapshot 必須修正 | 前回 `:331-345` | 本文 `:363-382` | 完全一致 |

この合格は「条件文を本文へ載せた」ことに対するものだけである。前回 §11 はさらに
matrix/inventory/schema/WP 表を要求しており
(`docs/design_reviews/2026-07-18_editor_v22_preview_review_codex.md:403-414`)、以下の
不足は逐語転記だけでは相殺されない。

## 2. V23-R1: snapshot は sequence だけで JSON schema がない

前回 §11-4 の要求は「full snapshot export/import の **JSON schema** と、source
session CAS までを示す sequence」である
(`docs/design_reviews/2026-07-18_editor_v22_preview_review_codex.md:413`)。

v2.3 §1-5-5 は export response の field 名を列挙し
(`docs/design_editor_tooling.md:363-372`)、§1-6-3 は CLI 風の sequence を示す
(`docs/design_editor_tooling.md:417-429`)。後半の `base = R → stale_revision` は source
session の global CAS まで到達しており、この部分は合格である。しかし、次が schema として
未定義である。

- export request の pending/preview policy の field 名、型、既定値と stable error。
- response の object/version、各 field の JSON 型・required/nullability・範囲。特に raw
  bytes は JSON value ではないため、UTF-8 JSON string、base64、別 file のどれで運ぶか。
- digest の algorithm、表現、何の byte 列を hash するか、import 時の照合順。
- import request または launch option の一意な schema。`--scene-snapshot <bytes/digest>`
  (`docs/design_editor_tooling.md:423-425`) は bytes と digest の二者択一にも一組にも読め、
  path/inline data/encoding/size limit/current scene/error が決まらない。
- digest mismatch、parse/semantic validation failure、unknown schema version、current scene
  不在、oversize の stable result と、失敗時に通常 scene source/cache/revision を変えない境界。

現行の正本 primitive は `encodeSemantic()` が UTF-8 の `std::string` を返すだけで
(`src/core/loader/authoringscenedocument.hpp:64-75`,
`src/core/loader/authoringscenedocument.cpp:99-102`)、`ProjectBasicConfig` の公開面も disk-backed
load/update/invalidate までである (`src/core/loader/basicconfig.hpp:53-75`)。従って encoding と
ingestion contract は既存 API から暗黙には補えない。これは前回 Reject の中心だった
「client 側に第二 serializer を作らせず、scratch ingestion を機構化する」条件そのものである。

## 3. V23-R2: operation matrix は preview conflict と E-C4 を閉じない

§1-6-1 は列名だけ見れば target/precondition/adapter/revision/epoch/conflict result を持つ
(`docs/design_editor_tooling.md:386-401`)。set/add/remove/spawn/destroy/reparent/undo/redo/
eval-preview の行もある。しかし ticket-preview の state machine と、既に優先規範である
E-C4 の双方で不足する。

### 3.1 ticket-preview の空欄と本文との矛盾

- `ticket abort` は target、precondition、conflict result がすべて `—` で、adapter も
  `committed document 再投影` としか書かない (`docs/design_editor_tooling.md:399`)。
  ticket id/ActorId ownership/open lease、E-C1 prepare→noexcept-publish、forced-abort の結果を
  表現していない。
- forced-abort は V22-C4 本文で独立した必須遷移なのに
  (`docs/design_editor_tooling.md:267-269`)、matrix に行がない。
- 通常 set/add/remove 等の precondition に preview lease overlap がなく、conflict result に
  stable busy/reason がない (`docs/design_editor_tooling.md:390-395`)。これは overlapping edit を
  同時進行させない V22-C4 (`docs/design_editor_tooling.md:263-268`)を表へ落としていない。
- `ticket commit` の precondition は ticket base revision CAS だけ
  (`docs/design_editor_tooling.md:398`)で、`ticket preview open/update/commit` を acceptance と
  frame-boundary execution の双方で gate snapshot/epoch 検査する V22-C7
  (`docs/design_editor_tooling.md:344-355`)を欠く。eval/render-preview 行も `can_preview` の一語
  だけで、execution-time gate close 時の method/reason/gate epoch result がない。

従って前回 §11-2 の「ticket-preview の target/precondition/adapter/revision/preview epoch/
conflict result」を過不足なく提示したとはいえない。

### 3.2 受理済み E-C4 の operation matrix ではない

受理済み E-C4 は各 edit operation について `authoring forward/inverse`、`runtime adapter`、
`必要先行 WP`、`preflight error`、`activation/deactivation boundary` を列挙するよう要求する
(`docs/design_reviews/2026-07-18_editor_behavior_v2_rereview_codex.md:97-101`)。v2.3 自身も冒頭で
この条件を維持すると宣言する (`docs/design_editor_tooling.md:37-39`)。

§1-6-1 には forward/inverse、必要先行 WP、activation/deactivation boundary の列がない。
precondition と conflict result も preflight error 全体の代替にはならない。さらに §3 の
E-RPC1/JOURNAL0 行は添付元に §1-6 または E-C4 を挙げていない
(`docs/design_editor_tooling.md:491`)。したがって、表を二つに分ける場合でも、少なくとも
E-C4 matrix の owner と提出物が別途必要である。

## 4. V23-R3: renderer inventory は最低名を満たすが state isolation を満たさない

§1-6-2 には DeletionQueue、RenderTiming、RT history、instance history、camera history、
layout tracker、EngineTime の最低指定名は存在する
(`docs/design_editor_tooling.md:403-415`)。ただし前回要求は単なる名前チェックでなく、shared
state を一項ずつ `read-only / request-local / explicitly suppressed` に分類することだった
(`docs/design_reviews/2026-07-18_editor_v22_preview_review_codex.md:410-412`)。現表には次の穴がある。

1. **shared FrameResources がない。** V22-C6 は request-local frame resources を逐語要求する
   (`docs/design_editor_tooling.md:320-324`)。現行 renderer は shared
   `FrameResources::beginLogicalFrame`、slot select、uniform update を毎 logical frame で実行する
   (`src/core/vkcore/renderer.cpp:1196`, `src/core/vkcore/renderer.cpp:1274-1275`)。preview で
   request-local にする対象を表から落としてはならない。
2. **Renderer 自身の flat/xr temporal state を列挙していない。** 実体は
   `flat_temporal_histories`、`xr_temporal_histories`、`last_view_snapshots`、reset/observed revision
   である (`src/core/vkcore/renderer.hpp:47-62`)。render はこれらを reset/更新/commit する
   (`src/core/vkcore/renderer.cpp:1188-1217`, `src/core/vkcore/renderer.cpp:1321-1327`)。
   `camera history/snapshot` 一行と graph variant の「不使用」は、この state 群の分類にならない。
3. **history の分類が本文と不一致である。** §1-5-3 は preview feature policy から history
   read/write を除く (`docs/design_editor_tooling.md:316-320`)のに、inventory は RT history と
   instance previous state を `read-only` とする (`docs/design_editor_tooling.md:409-410`)。
   `read-only` は shared history を入力に使うことを許す分類であり、`explicitly suppressed` とは
   異なる。request-local RT/temporal snapshot を使うなら、そのように分類を統一する必要がある。
4. **request width/height に対する shared resize state がない。** 現行 render は extent 差で
   `internal_render_extent` を更新し、shared RT/history/instance state を recreate/reset する
   (`src/core/vkcore/renderer.hpp:61-62`, `src/core/vkcore/renderer.cpp:1234-1241`)。RT recreate は
   旧 resource を shared DeletionQueue へ defer する
   (`src/core/renderingpass/rendertargetcontainer.cpp:189-213`)。preview request は width/height を
   任意指定するため、shared resize/rebind path を explicitly suppressed とする行が必要である。
5. **RenderTiming の published history 以外が未分類である。** 現行 logical frame は entry で
   shared query ring を configure する (`src/core/vkcore/renderer.cpp:1173-1186`)。record 時も
   last-node metadata、pending range、published status を変更する
   (`src/core/vkcore/rendertiming.cpp:196-229`, `src/core/vkcore/rendertiming.cpp:262-296`)。
   「published history に混ぜない」だけでなく、shared query allocator/status 全体を使わないか、
   request-local timing instance にするかを表で固定しなければならない。

DeletionQueue を advance しない、layout tracker を request-local、EngineTime を read-only とした
方向は正しい。上の漏れを追加し、各行を三分類の literal value にすれば V22-C6 と実コードを
接続できる。

## 5. V23-R4: §3 は受理済み graph と逐語 owner を復元していない

### 5.1 `STRUCT-SCHEMA0 → ED-CODEC0` が図から消えている

受理済み §6 は `WP71 → STRUCT-SCHEMA0 → ED-CODEC0` を固定し、ED-CODEC0 は共通 schema API を
新設しないとした
(`docs/design_reviews/2026-07-18_editor_behavior_v2_rereview_codex.md:188-213`)。v2.3 冒頭も
この graph が正しいと明記する (`docs/design_editor_tooling.md:43-45`)。

しかし §3 の graph は `ED-AUTH0 + ED-CODEC0 → E-RPC0-base` から始まり、
STRUCT-SCHEMA0 の node/edge を落とす (`docs/design_editor_tooling.md:469-482`)。WP 表も
ED-CODEC0 を「StructFieldSchema 相乗り」とし、先行 owner を示さない
(`docs/design_editor_tooling.md:487`)。実際の WP151 は WP150 の schema をそのまま使い、依存を
WP149+WP150 と固定済みである (`docs/implementation_plan.md:3893-3898`)。完了済み node を省略する
場合でも、`STRUCT-SCHEMA0(済 WP150) → ED-CODEC0` として graph に残す必要がある。

### 5.2 逐語条件の owner 添付が不足する

受理済み再レビューは、E-C2 を ED-CODEC0 **と** E-PROJTX0、E-C4 を E-RPC1、E-C5 を SAVE0 に
逐語添付するよう固定した
(`docs/design_reviews/2026-07-18_editor_behavior_v2_rereview_codex.md:77-81`,
`:97-107`, `:225-232`)。v2.3 §3 の条件欄では次が脱落している。

- ED-CODEC0 は §0-2 のみで E-C2 を持たず、E-PROJTX0 も E-C1 しか持たない
  (`docs/design_editor_tooling.md:487`, `docs/design_editor_tooling.md:490`)。
- E-RPC1/JOURNAL0 は §1-2/1-3/1-4/1-5 の一部だけで、E-C4 と §1-6-1 を持たない
  (`docs/design_editor_tooling.md:491`)。
- SAVE0 は §2-2 だけで、全 prepare→file replace→noexcept swap と fault gate の E-C5 を持たない
  (`docs/design_editor_tooling.md:497`; E-C5 原文は前記再レビュー `:103-107`)。
- SNAPSHOT0 は §1-5-5 だけで、JSON schema/sequence の owner である §1-6-3 を持たない
  (`docs/design_editor_tooling.md:495`)。

V22 条件について、E-HOST0/WATCH0 への V22-C3 分割、E-RPC1 後の PREVIEW0、WP133 依存は前回
§9 の修正方針と整合する (`docs/design_editor_tooling.md:475-477`,
`docs/design_reviews/2026-07-18_editor_v22_preview_review_codex.md:374-388`)。この正しい部分は維持
すべきである。

なお §1-6-3 は launch surface を具体例にする一方、§3 は E-HOST0 を SNAPSHOT0 の必須先行にし、
SNAPSHOT0 行も launch surface とする (`docs/design_editor_tooling.md:423-425`,
`:478`, `:495`)。RPC import を採るなら E-HOST0 依存、startup launch を採るなら別の launch/config
owner という選択を schema と graph で一致させる必要がある。

## 6. 依頼 5 項目への回答

| 指定項目 | 回答 |
|---|---|
| 1. V22-C1〜C7 + snapshot 必須修正の逐語 | **本文転記は合格**。ただし §1-6-1 が V22-C4/C7 を operation state へ落とさず、表レベルでは暗黙緩和が残る |
| 2. operation/write-set matrix | **不合格**。基本 edit 行はあるが ticket abort/forced-abort、preview lease conflict、execution gate result が閉じず、E-C4 の別必須列もない |
| 3. renderer preview state inventory | **不合格**。最低名 7 行はあるが shared state の列挙・三分類・本文との整合が不足 |
| 4. snapshot schema/sequence | **不合格**。source CAS sequence は合格、export/import JSON schema は欠落 |
| 5. WP graph/owner | **不合格**。PREVIEW/WATCH の再接続は改善したが、受理済み schema edge と E-C2/E-C4/E-C5/§1-6-3 owner が欠落 |

新たに v2.1 受理部の方式そのものを覆す矛盾は見つからなかった。今回の E-C1〜E-C5 との衝突は、
方式の変更ではなく §3 の dependency/owner と §1-6-1 の acceptance data から条件が脱落したことに
よる。

## 7. 再提出の必須修正

次の全件を同じ版で満たすこと。

1. V22-C1〜V22-C7 と snapshot 必須修正の現在の逐語本文は変更しない。
2. §1-6-1 を、通常 edit と preview state transition の双方で閉じる。ticket open/update/commit/
   abort/forced-abort を必要なら別行にし、各行の stable target/owner、acceptance と execution の
   precondition、adapter、revision/preview/gate epoch、全 stable conflict result を記す。通常 edit
   行にも preview lease overlap を入れる。さらに E-C4 の forward/inverse、必要先行 WP、
   preflight error、activation/deactivation boundary を同じ表または明示的に owner された別表で
   提出する。
3. §1-6-2 に少なくとも shared FrameResources、Renderer flat/xr temporal histories +
   last snapshot/reset state、render extent/resize/rebind、RenderTiming query allocator/status を追加
   する。全行の分類値を literal に `read-only` / `request-local` / `explicitly suppressed` の
   いずれかとし、§1-5-3 の history read/write 除外と一致させる。
4. export request/response と import/launch input の versioned JSON schema を掲載し、field 型・
   requiredness・bytes encoding・digest・size/pending policy・validation/error・失敗時非公開を固定
   する。その schema 名を使って export→scratch ingestion→source normal edit(base R)→CAS reject/
   commit の sequence を書く。
5. §3 に `STRUCT-SCHEMA0(済 WP150) → ED-CODEC0` を復元し、E-C1〜E-C5、V22-C1〜C7、snapshot
   条件、§1-6 各表について owner WP を一意に列挙する。特に E-C2=ED-CODEC0+E-PROJTX0、
   E-C4=E-RPC1/JOURNAL0、E-C5=SAVE0、§1-6-3=SNAPSHOT0 を落とさない。

以上を満たした版を再レビューするまでは **Reject** を維持する。
