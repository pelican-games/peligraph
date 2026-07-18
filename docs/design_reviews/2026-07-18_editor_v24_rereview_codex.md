# editor tooling v2.4 §1-6 / §3 再々レビュー

日付: 2026-07-18

対象: `docs/design_editor_tooling.md` v2.4 (`6b23a63`、根拠ツリー
`agent/review-v24` のレビュー開始時 HEAD `e73ebe6`)

前回: `docs/design_reviews/2026-07-18_editor_v23_rereview_codex.md`

## 0. 結論

**Reject**。

前回 §7 の 5 件のうち、逐語本文と renderer inventory は合格し、§3 の
dependency edge と大半の owner 添付も復元された。一方、実装 WP の acceptance
boundary にするための二つの中心提出物がまだ閉じていない。

1. §1-6-1a は ticket の acceptance/execution 間で lease state を再検査せず、
   forced-abort 行は列が一つ足りない。acceptance/preflight に書かれた失敗の
   stable result も全件対応していない。
2. §1-6-3 は例示 object に version field を足したが、export request を含む
   **名前付き versioned schema** と全 field の requiredness を固定していない。
   「schema 名で記述」とする sequence にも schema 名が存在せず、必須の
   `schema_version` を省略している。

従って E-RPC1/JOURNAL0 と SNAPSHOT0 を本書の表だけで登録・着手してはならない。
renderer inventory の PREVIEW0 gate と、§3 の復元済み dependency/owner は再修正時に
維持すること。

| 前回 §7 の必須修正 | 判定 | 要旨 |
|---|---|---|
| 1. V22-C1〜C7 + snapshot 逐語本文を不変にする | **Accept** | v2.3 (`da60b99`) と空白・改行を除いて 8 件全一致 |
| 2. §1-6-1a ticket 全遷移 + §1-6-1b E-C4 | **Reject** | op/列は増えたが、execution-time lease state と stable result が閉じず、forced-abort は表の列ずれ。E-C4 error 名も 1a と不一致 |
| 3. §1-6-2 renderer inventory | **Accept** | 指定 4 state 群を含む 12 行が literal 三分類。history は shared read/write とも suppressed で、現行実体とも整合 |
| 4. §1-6-3 snapshot schema/sequence | **Reject** | encoding/digest/size/validation/失敗時非公開は改善。一方、schema の version/name/requiredness と named-schema sequence が未完 |
| 5. §3 graph/owner | **条件付き Accept** | edge と主要 owner は復元。§1-6-3 の E-RPC0-base/SNAPSHOT0 二重 owner 表記だけを下記条件で固定すれば WP gate で吸収可能 |

## 1. 逐語本文の監査 — Accept

v2.3 の正本 blob (`git show da60b99:docs/design_editor_tooling.md`) と v2.4 の各
blockquote を、Markdown の `>` と空白・改行を除いて比較した。全 8 件が一致した。

| 条件 | v2.4 の所在 | 結果 |
|---|---|---|
| V22-C1 | `docs/design_editor_tooling.md:193-216` | 完全一致 |
| V22-C2 | `docs/design_editor_tooling.md:221-234` | 完全一致 |
| V22-C3 | `docs/design_editor_tooling.md:239-254` | 完全一致 |
| V22-C4 | `docs/design_editor_tooling.md:269-291` | 完全一致 |
| V22-C5 | `docs/design_editor_tooling.md:293-316` | 完全一致 |
| V22-C6 | `docs/design_editor_tooling.md:322-348` | 完全一致 |
| V22-C7 | `docs/design_editor_tooling.md:353-366` | 完全一致 |
| snapshot 必須修正 | `docs/design_editor_tooling.md:368-393` | 完全一致 |

前回レビュー自身も v2.3 の 8 件を合格済みである
(`docs/design_reviews/2026-07-18_editor_v23_rereview_codex.md:30-49`)。v2.4 は
ここを変更していない。

## 2. V24-R1: §1-6-1a は ticket の二段遷移と stable result を閉じない

前回の要求は、ticket open/update/commit/abort/forced-abort の各行に stable
target/owner、**acceptance と execution の precondition**、adapter、revision/
preview/gate epoch、全 stable conflict result を置くことだった
(`docs/design_reviews/2026-07-18_editor_v23_rereview_codex.md:220-226`)。
通常 edit の preview lease overlap を acceptance/execution の双方へ追加した本文と
基本 edit 行は改善されている (`docs/design_editor_tooling.md:399-414`)。しかし次は
未充足である。

### 2.1 acceptance 後の lease state が execution で再検査されない

- ticket open は acceptance で lease 空きを検査するが、execution は gate 再検査だけ
  である (`docs/design_editor_tooling.md:415`)。同じ frame boundary より前に二要求が
  空きを見て acceptance された場合、先に実行した open が作った lease を後続 open が
  再検査しない。一 session 一 lease と `preview_lease_busy` の境界にならない。
- update も acceptance で lease 所有を検査するだけで、execution は gate だけである
  (`docs/design_editor_tooling.md:416`)。先行 commit/abort/forced-abort が lease を解放した
  後の queued update の結果が未定義である。
- abort は execution precondition を `— (abort は拒否されない)` とする一方、acceptance
  後に forced-abort 等で lease が消えた場合の idempotence/result を定義しない
  (`docs/design_editor_tooling.md:418`)。V22-C7 が acceptance/execution の二回検査を要求
  する理由はまさにこの時間差である (`docs/design_editor_tooling.md:355-366`)。

open/update/abort は execution でも ticket existence、ActorId ownership、lease state と
必要 capability を再検査し、消滅済みを idempotent success とするのか stable
`ticket_not_found` 等にするのかを固定する必要がある。

### 2.2 forced-abort 行は Markdown 上も意味上も一列不足する

表 header は 7 列だが (`docs/design_editor_tooling.md:406-407`)、forced-abort は 6 列しか
ない (`docs/design_editor_tooling.md:419`)。そのため現状は次のように解釈される。

- execution precondition = 「frame observer 再開前に abort と同一 adapter 経路」
- adapter = 「epoch+1・lease 解放」
- rev / preview_epoch = 「結果通知 = ticket_forced_aborted{reason}」
- stable conflict result = 欠落

adapter、epoch/result、非該当なら `none` とする conflict 列を分離しなければ、前回が
要求した ticket 全遷移表ではない。

### 2.3 precondition と stable result が対応せず、1a/1b で code 名も異なる

例だけでも次が落ちている。

- set/add/remove の schema 検証に対応する stable result、add の unknown type、destroy の
  closure 解決失敗、reparent の preserve 未指定が 1a の result にない
  (`docs/design_editor_tooling.md:408-413`)。
- render_preview は capture schema 検証を行うが、size 超過以外の schema failure result が
  ない (`docs/design_editor_tooling.md:421`)。
- 1a は `duplicate_component` / `cycle_detected`、1b は `duplicate` / `cycle` とする
  (`docs/design_editor_tooling.md:409,413,428,432`)。同じ preflight の stable code が表間で
  一意でない。
- E-C2 逐語は zero scale だけでなく非有限値・canonical TRS 表現不能も stable error code
  と object path 付きで reject するよう要求するが
  (`docs/design_reviews/2026-07-18_editor_behavior_v2_rereview_codex.md:77-81`)、1a/1b の
  reparent 行はその二つと path payload を落とす (`docs/design_editor_tooling.md:413,432`)。

§1-6-1b 自体は full v1 edit 群、forward/inverse、runtime adapter、必要先行 WP、preflight
error、activation/deactivation boundary の列を持つ (`docs/design_editor_tooling.md:423-433`)。
従って方式の作り直しは不要だが、1a の全 failure point と同じ canonical error catalog を
参照させるまでは E-C4 の preflight error 列も合格にできない。

## 3. §1-6-2 renderer preview state inventory — Accept

指定された四つの欠落群は全て追加された。

1. shared FrameResources は `request-local`
   (`docs/design_editor_tooling.md:444`)。現行の shared module は view count/slot/last uniform
   data を持ち (`src/core/renderer/frameresources.hpp:43-70`)、通常 frame が
   begin/select/update する (`src/core/vkcore/renderer.cpp:1196,1274-1275`)ため、専用 instance
   に分ける分類は正しい。
2. Renderer flat/xr histories、last snapshot、reset/observed revision は
   `explicitly suppressed` (`docs/design_editor_tooling.md:448`)。実体は
   `src/core/vkcore/renderer.hpp:55-62`、通常 frame の reset/update/commit は
   `src/core/vkcore/renderer.cpp:1188-1217,1321-1327` にあり、列挙と一致する。
3. extent/resize/rebind は `explicitly suppressed`
   (`docs/design_editor_tooling.md:450`)。通常 path は extent 差で history reset/rebind を
   行う (`src/core/vkcore/renderer.cpp:1234-1241`)。RT recreate が旧 image/view を shared
   DeletionQueue へ defer する実体もある
   (`src/core/renderingpass/rendertargetcontainer.cpp:189-213`)ため、request-local RT のみに
   width/height を適用する境界は正しい。
4. RenderTiming query allocator/pending/published status は `request-local`
   (`docs/design_editor_tooling.md:445`)。shared state は query dimensions/ranges/status を持ち
   (`src/core/vkcore/rendertiming.hpp:114-131`)、configure と record がそれらを更新する
   (`src/core/vkcore/rendertiming.cpp:161-188,196-270`)ため、専用 namespace/instance が必要
   という記述と整合する。

全 12 行の分類 cell は literal に `read-only` / `request-local` /
`explicitly suppressed` のいずれかである (`docs/design_editor_tooling.md:441-454`)。
RenderTarget、instance、Renderer/camera histories はすべて `explicitly suppressed` なので
§1-5-3 の history read/write 除外 (`docs/design_editor_tooling.md:327-337`) とも無矛盾である。
現行 PolygonInstanceContainer が previous model/skin/morph/material state を実際に advance/
reset すること (`src/core/renderer/polygoninstancecontainer.cpp:666-720`)にも対応している。

## 4. V24-R2: §1-6-3 は versioned JSON schema と named-schema sequence になっていない

改善点は明確である。semantic bytes は inline UTF-8 JSON text、digest はその UTF-8 byte
列の SHA-256/小文字 hex、上限は 64 MiB と決めた
(`docs/design_editor_tooling.md:481-496`)。pending policy、import の検証順、stable error、
失敗時に scene source/cache/revision を変えない境界も書いた
(`docs/design_editor_tooling.md:463-472,509-517`)。現行 primitive が
`encodeSemantic()` の `std::string` しか返さない
(`src/core/loader/authoringscenedocument.hpp:68-78`,
`src/core/loader/authoringscenedocument.cpp:101-104`)ことに対し、transport encoding を明示した
点は合格である。

しかし前回要求は export request/response と import input の **versioned JSON schema**、
全 field の型・requiredness、そしてその schema 名を使う sequence である
(`docs/design_reviews/2026-07-18_editor_v23_rereview_codex.md:231-234`)。現状には次が残る。

1. export request は `{ "allow_pending": false }` だけで `schema_version` がなく、schema 名も
   ない (`docs/design_editor_tooling.md:463-470`)。response/import に version field があっても、
   export request を含む三 schema 全体を versioned にしたことにはならない。
2. response の `schema_version`、`scene_revision`、`preview_epoch` と digest object、import の
   各 field は、型が例から推測できる箇所はあっても required/non-null が全件明記されていない
   (`docs/design_editor_tooling.md:474-507`)。特に JSON number で `uint64` のどの範囲を許すかも
   固定されない。
3. response の JSON code block は `semantic_scene_bytes` と `digest.hex` の quoted string 内に
   raw 改行を含み (`docs/design_editor_tooling.md:481-485`)、厳密な JSON として parse できない。
   schema/example を実装 fixture にそのまま使えない。
4. `sequence(schema 名で記述)` と宣言するが、`ExportSceneSnapshotRequestV1` 等の schema 名を
   一つも定義していない。sequence は method 名と省略 field `bytes, digest, ...` だけで、import
   request の必須 `schema_version` さえ落とす
   (`docs/design_editor_tooling.md:519-532`)。

また、E-RPC0-base が export schema を持ち、SNAPSHOT0 が JSON schema/sequence 全体を持つ
§3 の二重表記 (`docs/design_editor_tooling.md:594,601`)は、どちらが schema evolution と
compatibility fixture の最終 owner かを曖昧にする。RPC import 一本化そのものは、scratch
側 transport を既存 WP27 とし、SNAPSHOT0 が ingestion handler を所有する graph
(`docs/design_editor_tooling.md:458-461,582-583`)と整合している。

## 5. §3 dependency / owner — 条件付き Accept

`WP71(済) → STRUCT-SCHEMA0(済 WP150) → ED-CODEC0` は復元された
(`docs/design_editor_tooling.md:574-577`)。前回欠落していた主要 owner も次の通り明示された。

| 条件 | owner |
|---|---|
| E-C1 | E-PROJTX0 (`docs/design_editor_tooling.md:596`) |
| E-C2 | ED-CODEC0 + E-PROJTX0 (`:593,596`) |
| E-C3-1 / E-C3-2 / E-C3-3 | ECS-MUT0 / E-RPC1・JOURNAL0 / ED-CODEC0 (`:593,595,597`) |
| E-C4 | E-RPC1/JOURNAL0 (`:597`) |
| E-C5 | SAVE0 (`:603`) |
| V22-C1/C2/C4/C7 | E-RPC1/JOURNAL0 (`:597`。対応 section は `:193,221,269,353`) |
| V22-C3 | E-HOST0(transport) + WATCH0(enrichment) (`:598-599`) |
| V22-C5/C6 | PREVIEW0 (`:600`) |
| snapshot 必須修正 | SNAPSHOT0 (`:601`) |
| §1-6-1a / 1b / 2 / 3 | E-RPC1/JOURNAL0 / E-RPC1/JOURNAL0 / PREVIEW0 / SNAPSHOT0 (`:597,600-601`) |

E-C2 と V22-C3 は条件内部の責務を明示分割した複数 owner であり、前回が要求した mapping と
一致する。残る §1-6-3 の重複は設計方式を変えず WP gate の一文で吸収可能なので、次を
**条件逐語**とする。

> §1-6-3 の schema evolution、compatibility fixture、export/import 一組の acceptance owner は SNAPSHOT0 とする。E-RPC0-base は SNAPSHOT0 が固定した export schema を実装する prerequisite surface であって、独立した schema owner ではない。export/import の片側だけを互換性変更してはならない。

この一文を E-RPC0-base と SNAPSHOT0 の双方の WP gate に添付すれば、§3 単体は再提出
blocker にしない。

## 6. 再提出の最小修正

無限往復を避けるため、方式変更ではなく次だけを blocker とする。

1. §1-6-1a の全行を 7 列にし、open/update/commit/abort/forced-abort の acceptance と
   execution の双方で必要な ticket/lease/owner/capability state を固定する。各 failure point
   を canonical stable error/result 一覧へ一対一に接続し、§1-6-1b と同じ code 名・payload を
   使う。E-C2 の非有限値/表現不能/path payload も復元する。
2. export request/response/import request を名前付き V1 schema として掲載し、全 field の
   JSON 型・required/nullability・数値範囲を明示する。例は厳密 JSON として parse gate に通す。
   sequence はその schema 名と必須 `schema_version` を省略せず、現在の source CAS までの流れを
   維持する。
3. §5 の条件逐語を E-RPC0-base/SNAPSHOT0 の WP gate に添付する。これは本書再改稿を要する
   blocker ではなく、WP 登録時の owner 注意でよい。

上記 1 と 2 が同じ版で満たされるまでは **Reject** を維持する。
