# editor tooling v2.5 最終確認レビュー

日付: 2026-07-18

対象: `docs/design_editor_tooling.md` v2.5 (`7debf1c`)

前回: `docs/design_reviews/2026-07-18_editor_v24_rereview_codex.md`

## 0. 結論

**条件付き Accept**。

v2.4 再々レビュー §6 が blocker とした二点は、同じ v2.5 でいずれも
充足された。

1. V24-R1 は **Accept**。§1-6-1a は全行 7 列となり、ticket の
   acceptance/execution 二段検査、消滅済み abort の idempotent success、
   canonical error/result、§1-6-1b との code 統一、E-C2 の
   非有限値/表現不能/object path payload が閉じた。
2. V24-R2 は **Accept**。export request/response/import request は三本の
   named V1 schema となり、field 制約、厳密 JSON 例、schema 名を使う
   sequence、source revision `R` に対する採用時 CAS が揃った。

従って **§1-4 / §1-5 / §1-6 を条件付き Accept** とする。条件は新しい
設計修正ではなく、v2.4 レビュー §5 が既に定めた schema owner 条件を
E-RPC0-base と SNAPSHOT0 の WP gate に逐語添付することだけである。

E-RPC1/JOURNAL0、E-HOST0、WATCH0、PREVIEW0 は登録可。SNAPSHOT0 も
下記 owner 条件を SNAPSHOT0 と E-RPC0-base の双方へ同時に添付すれば
登録可である。各 WP の着手順は §3 の dependency graph を維持する。

## 1. 監査境界と既受理部の不変確認

v2.4 レビューは、V22-C1〜C7 と snapshot 必須修正の逐語 8 block を
Accept 済みである
(`docs/design_reviews/2026-07-18_editor_v24_rereview_codex.md:38-56`)。
v2.4 (`6b23a63`) から v2.5 (`7debf1c`) の差分を確認すると、当該 block は
変更されていない。現行の所在は次の通りである。

| 既受理条件 | v2.5 の所在 | 判定 |
|---|---|---|
| V22-C1 | `docs/design_editor_tooling.md:199-226` | 不変 |
| V22-C2 | `docs/design_editor_tooling.md:227-244` | 不変 |
| V22-C3 | `docs/design_editor_tooling.md:245-267` | 不変 |
| V22-C4 | `docs/design_editor_tooling.md:275-298` | 不変 |
| V22-C5 | `docs/design_editor_tooling.md:299-327` | 不変 |
| V22-C6 | `docs/design_editor_tooling.md:328-358` | 不変 |
| V22-C7 | `docs/design_editor_tooling.md:359-373` | 不変 |
| snapshot 必須修正 | `docs/design_editor_tooling.md:374-400` | 不変 |

§1-6-2 も v2.4 から不変である
(`docs/design_editor_tooling.md:472-492`)。前回の Accept 根拠である全 12 行の
literal 三分類と renderer 実体との対応は再審しない
(`docs/design_reviews/2026-07-18_editor_v24_rereview_codex.md:121-151`)。

§3 の dependency graph と WP owner 表も v2.4 から不変である
(`docs/design_editor_tooling.md:620-651`)。前回の条件付き Accept
(`docs/design_reviews/2026-07-18_editor_v24_rereview_codex.md:190-215`)を
維持し、本レビューでは owner 条件の添付状態だけを §4 で確認する。

## 2. V24-R1 — Accept

### 2.1 ticket の二段遷移と 7 列

§1-6-1a は header、separator、全 operation 行を機械的に数え、すべて
7 列であることを確認した (`docs/design_editor_tooling.md:443-458`)。
forced-abort も adapter、epoch/lease、result notification が別 cell に
分離されており、v2.4 の列ずれは解消した
(`docs/design_editor_tooling.md:456`)。

共通規範は、execution で editor gate に加えて ticket existence、ActorId
ownership、lease state、capability を再検査すると明記する
(`docs/design_editor_tooling.md:435-441`)。各 ticket 行も次を固定した。

- open は lease 空きと capability を execution で再検査する
  (`docs/design_editor_tooling.md:452`)。
- update は ticket existence/ownership を再検査し、先行 commit/abort/
  forced-abort 済みを `ticket_not_found{final_status}` にする
  (`docs/design_editor_tooling.md:453`)。
- commit は existence/ownership、CAS、通常 transaction preflight を
  再検査する (`docs/design_editor_tooling.md:454`)。
- abort は消滅済みなら state/epoch を変えず
  `success{final_status}` を返す。存在時の非 owner は
  `not_lease_owner` である (`docs/design_editor_tooling.md:455`)。
- system forced-abort は frame observer 再開前に committed document を
  再投影し、epoch を一回進めて lease を解放し、owner へ
  `ticket_forced_aborted{reason}` を通知する
  (`docs/design_editor_tooling.md:456`)。

これは前回の要求「open/update/abort は execution でも ticket existence、
ActorId ownership、lease state と必要 capability を再検査し、消滅済みの
result を固定する」
(`docs/design_reviews/2026-07-18_editor_v24_rereview_codex.md:68-84`)を満たす。

### 2.2 canonical result と E-C4/E-C2 の一致

§1-6-1c は stable code と payload の正本を置いた
(`docs/design_editor_tooling.md:403-431`)。通常 edit の各 failure point は
§1-6-1a の result に接続され、v2.4 で具体的に欠けていた schema violation、
unknown component、destroy closure、reparent preserve、capture schema の
code も復元された (`docs/design_editor_tooling.md:445-458`)。

§1-6-1b は `schema_violation`、`duplicate_component`、
`unknown_component_type`、`closure_unresolvable`、`cycle_detected` 等、
catalog と同じ code 名を使う
(`docs/design_editor_tooling.md:464-470`)。E-C2 が要求する `zero_scale`、
`non_finite_transform`、`trs_unrepresentable` はすべて object path payload
を持ち (`docs/design_editor_tooling.md:423-425`)、reparent の両 matrix に
同じ名前で現れる (`docs/design_editor_tooling.md:450,469`)。

従って、v2.4 §6-1 の canonical error/result と ticket state machine の
blocker は閉じた。

## 3. V24-R2 — Accept

三つの transport schema はそれぞれ
`ExportSceneSnapshotRequestV1`、`ExportSceneSnapshotResponseV1`、
`ImportSceneSnapshotRequestV1` と命名された
(`docs/design_editor_tooling.md:503-548`)。各表は JSON 型、requiredness、
許容値または内容を列挙し、数値 field は共通で非負整数かつ
2^53−1 以下、`schema_version` は 1 のみに固定する
(`docs/design_editor_tooling.md:495-524,535-543`)。列挙された JSON 型に
`null` は含まれず、string field 等の non-null も明記されている。

掲載された request 一行例と二つの fenced JSON 例を JSON parser に通し、
三件とも parse 成功を確認した。二つの digest は 64 桁小文字 hex であり、
semantic bytes は JSON string 内で正しく escape されている
(`docs/design_editor_tooling.md:510,526-530,545-549`)。この確認は前回が要求した
strict parse gate であり、digest 再計算や semantic validation の fixture は
SNAPSHOT0 の compatibility gate で別途実施する。

非 blocker の注意として、二例に掲載された digest
`0f9c2a4b...012345` は例中 `semantic_scene_bytes` の実 SHA-256
`e2e3e938fc8a194571721e8752bf0c1ad8d5b99235aa4b217ecb2f7b232fe26d`
とは一致しない (`docs/design_editor_tooling.md:529,548`)。前回の要求は
strict JSON の **parse gate** なので V24-R2 の Accept は変えないが、
この値を digest validation/compatibility fixture へ流用してはならない。
SNAPSHOT0 では schema 規範どおり実 bytes から digest を生成する。

sequence は三つの schema 名を使用し、export/import の双方で必須
`schema_version:1` を省略していない。export response の revision `R` を
採用 edit の base とし、source が `R` 以後に変化していれば
`stale_revision` とする流れも維持する
(`docs/design_editor_tooling.md:563-580`)。import の size/digest/parse/
semantic/current-scene 検証順と、失敗時に source/cache/revision を変えない
境界も維持された (`docs/design_editor_tooling.md:551-559`)。

従って、v2.4 §6-2 の named versioned schema と named-schema sequence の
blocker は閉じた。

## 4. WP gate に残す条件逐語

残条件は次の一件だけである。v2.4 レビュー §5 の条件をそのまま引用する。

> §1-6-3 の schema evolution、compatibility fixture、export/import 一組の acceptance owner は SNAPSHOT0 とする。E-RPC0-base は SNAPSHOT0 が固定した export schema を実装する prerequisite surface であって、独立した schema owner ではない。export/import の片側だけを互換性変更してはならない。

出典:
`docs/design_reviews/2026-07-18_editor_v24_rereview_codex.md:208-215`。

v2.5 §1-6-3 冒頭は owner の要旨を記すが
(`docs/design_editor_tooling.md:498-501`)、上の三文を逐語では収録していない。
また §3 の E-RPC0-base/SNAPSHOT0 行は §1-6-3 を参照するだけである
(`docs/design_editor_tooling.md:642,649`)。従って、上の引用全文を
**E-RPC0-base と SNAPSHOT0 の双方の WP gate** に添付する。前回レビューは
これを「本書再改稿を要する blocker ではなく、WP 登録時の owner 注意」と
明記しているため
(`docs/design_reviews/2026-07-18_editor_v24_rereview_codex.md:217-230`)、
本件によって V24-R1/R2 の Accept は覆らない。

このほかに WP gate へ追加する残条件はない。

## 5. 実装 WP の登録可否

| WP | 登録可否 | gate / 着手順 |
|---|---|---|
| E-RPC1/JOURNAL0 | **登録可** | V24-R1 は閉包。E-RPC0-base + E-PROJTX0 完了後に着手する (`docs/design_editor_tooling.md:626,645`) |
| E-HOST0 | **登録可** | 受理済み §1-4-3 前半を添付。先行 WP27 は graph 上「済」 (`docs/design_editor_tooling.md:627,646`) |
| WATCH0 | **登録可** | 受理済み §1-4-3 後半を添付。E-RPC1/JOURNAL0 完了後に着手する (`docs/design_editor_tooling.md:628,647`) |
| PREVIEW0 | **登録可** | 受理済み §1-6-2 inventory を gate に添付。E-RPC1/JOURNAL0 完了後に着手する (`docs/design_editor_tooling.md:629,648`) |
| SNAPSHOT0 | **条件付きで登録可** | §4 の条件逐語を SNAPSHOT0 と E-RPC0-base の双方へ添付。E-RPC0-base + E-HOST0 完了後に着手する (`docs/design_editor_tooling.md:630-631,649`) |

WP153(E-PROJTX0)が依存する受理済み E-C1/E-C2 は再審していない。v2.5 の
reparent error/payload 復元は E-C2 の規範と整合し、WP153 の独立進行を
止める矛盾は本レビュー範囲では認めない。E-RPC1/JOURNAL0 の着手だけは
§3 の graph どおり E-PROJTX0 完了を待つ。
