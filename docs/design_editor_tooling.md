# 編集系 rpc とエンジン内インスペクタ/アセットブラウザ(v2.2 — 同時編集/試行実行を追加・レビュー待ち)

**v2.2(2026-07-18)**: ユーザー方針「人+エージェントの同時作業」を受け
§1-4(同時編集 = actor_id/CAS/watch/actor 別 undo)と §1-5(試行実行の
三段梯子 = ticket preview / eval・render_preview / スクラッチ
インスタンス)を新設。**§1-4/§1-5 は codex 敵対レビュー未通過** —
E-RPC0/E-RPC1/JOURNAL0 の WP 登録前にレビューを通すこと。v2.1 までの
受理済み本文は変更していない。

**v2.1(2026-07-18)**: 再レビュー
`docs/design_reviews/2026-07-18_editor_behavior_v2_rereview_codex.md` で
**条件付き受理**。同レビュー **E-C1〜E-C5 が本書に優先する規範**であり、
各 WP 登録時に逐語添付する。要点:

- **E-PROJTX0(新 WP・E-RPC1 の先行)**: document+runtime の
  failure-atomic 合成は「全 adapter の prepare(live 無変更)→ 全成功後
  frame-boundary で noexcept publish」protocol(E-C1 逐語)。
  ECS-MUT0 は generic migration participant であり aggregate
  transaction の代用ではない
- **transform 意味論(E-C2 逐語)**: authored=local が正本・
  set は staged document + LocalTransformComponent → commit 区間内に
  descendant closure の world 再計算 → observer 再開。runtime_json は
  {local_trs, world_trs} 区別・world の自動 write-back 禁止。
  reparent は `preserve: "local"|"world"` 必須・zero scale/循環は
  preflight reject
- **E-C3**: ECS-MUT0 の前回逐語(fault matrix)を WP に完全転記・
  E-RPC1 の競合 fixture(phase trace/stale revision/scene transition/
  callback・reload 競合)復元・ED-CODEC0 は七種の canonical JSON 例を
  最小提出物に含む
- **E-C4**: E-RPC1 は operation matrix(各 op の forward/inverse/
  adapter/先行 WP/error/boundary)を WP 表に置く。未提供 method は
  stable `method_unavailable`
- **E-C5**: Save の公開は「全構築 → file replace → noexcept swap」。
  中間状態を観測させない・各 prepare 点 + replace 直前の fault
  injection・新規 process reload gate
- **依存グラフはレビュー §6 が正**(STRUCT-SCHEMA0 →
  ED-CODEC0/BEH-P0、ED-AUTH0+ED-CODEC0+ECS-MUT0 → E-PROJTX0 →
  E-RPC1)。**ED-AUTH0 は §7 の境界逐語を添付すれば単独着地可**

対象読者: エンジン担当・エディタ/ツールを使う人。
ステータス: v2 ドラフト(2026-07-18)。v1 は敵対レビュー
`docs/design_reviews/2026-07-17_editor_behavior_review_codex.md` §1(以下
「レビュー」)で **Reject** — ①逆方向 serializer は 1 個も存在しない
(E-1: 「optional serializer」の実体は optional **loader**)②scene
object と runtime entity は 1:1 でない(E-2)③同期 stdio で「次フレーム
結果を edit 応答で返す」は deadlock(E-3)④journal 四項組は undo の
データモデルでない(E-6)⑤SAVE0 が正本ファイルを破壊し得る(E-7)。
v2 = **正本を AuthoringSceneDocument に確定**し、レビュー §4.1 の
逐語条件と WP 再分割を採用した全面改稿。
前提: v1 と同じ(D0・WP27/42・WP62・scene v1・file-is-truth)。

## 0. 正本の確定(v2 の中核決定)

**編集の正本 = AuthoringSceneDocument(authored JSON の lossless な
engine 内表現)。runtime ECS は projection(投影)である。**

- edit は「document への編集 + runtime への同時投影」の transaction
- Save は「document 全体の決定的 encode + atomic replace」— runtime の
  serialize では**ない**(codec がない component も raw JSON のまま
  必ず保存される = data loss が構造的に不可能)
- これにより v1 の矛盾(「現在値→JSON」vs「ファイルが正本」)を解消

### 0-1. AuthoringSceneDocument(ED-AUTH0 — レビュー逐語)

> scene v1 の全 envelope/全 scene/全 object/全 component raw JSON を
> 保持する engine-owned AuthoringSceneDocument を導入する。各 load は
> 単調増加 SceneRevision と session-stable AuthoringObjectId を
> 割り当て、無名・light-only・collider-only object も列挙可能にする。
> runtime EntityId は nullable projection とし、source object identity に
> 代用しない。ProjectBasicConfig の scene cache は
> AuthoringSceneDocument と一つの更新/invalidate API へ統合する。
> parse→bind→query→deterministic encode で未編集 document が semantic
> equal、非 current scene と read-only/special component が一項も
> 脱落しない fixture を gate とする。

状態図(正本と投影):

```
disk file ──load──▶ AuthoringSceneDocument ──project──▶ runtime(ECS/Light/Phys)
                     │ SceneRevision N                    │ EntityId(nullable)
   ▲                 │ AuthoringObjectId(全 object)       │
   └──Save(atomic replace・digest 検査)──┘  edit = document+projection 同時 commit
```

### 0-2. component codec(ED-CODEC0 — レビュー逐語)

> component codec を `decode authored JSON / encode canonical authored
> JSON / schema / runtime apply / runtime project` の組として登録する。
> ComponentInfo の lifecycle/loader callback を逆方向 serializer が
> 既にあるものとして扱わない。v1 coverage は transform(local/world
> mapping と hierarchy)、simplemodelview、camera、light、collider、
> animation(boolean loop)、sprite_view(closed schema)を必須とする。
> 各 component について authored→runtime→canonical JSON→fresh load の
> semantic equality と invalid type/range/unknown key を fixture 化する。
> codec のない component は raw authored JSON を保存時に必ず保持し、
> runtime edit 不可を query metadata で明示する。

coverage 表(レビュー E-1 の実情を出発点に):

| component | 現状 | codec の仕事 |
|---|---|---|
| transform | loader のみ・authored=local / runtime=world | **local/world mapping**(authored local TRS が正本・runtime project は階層合成の逆算) |
| simplemodelview | loader のみ | 対称化容易。model 変更の再 bind を apply に含む |
| camera | ECS は空 marker・実体は Camera が source JSON 再 parse | **専用 adapter**(projection/controller を document から) |
| light | ComponentInfo 非経由(special dispatch) | **special adapter**(LightContainer への apply/project) |
| collider | 同上(PhysWorld) | special adapter |
| animation | loader のみ・bool→uint8 正規化 | canonical encode は authored bool を出す |
| sprite_view | closed loader・saver ref と authored 形が不一致 | closed schema 準拠の専用 encode(flip 配列・文字列 billboard) |

- schema(型・range・enum・default)は codec 登録の一部 —
  **BEH-P0 の StructFieldSchema と同一機構**(`design_object_behaviors.md`
  §2 — 別マクロ系を作らない)

## 1. E-RPC v1

### 1-1. query(E-RPC0 改訂条件 — レビュー逐語)

> query は `{scene_revision, authoring_object_id, name?, parent?,
> entity_id?, components[]}` を返す。component ごとに `authored_json`,
> optional `runtime_json`, `editable`, `codec/schema state`, `pending` を
> 区別する。JSON key/order、object/component declaration order、
> EntityId 表示は二回実行で一致させる。RPC handler の外に typed
> EditorCommandService を置き、RPC/ImGui fake adapter が同じ query
> 結果/error code を返すことを検査する。依存は ED-AUTH0/ED-CODEC0。

- `list_assets` は v1 のまま(store/kind/status — HR status 連動)

### 1-2. edit(E-RPC1 改訂条件 — レビュー逐語)

> edit request は enqueue acceptance と ticket を同期応答し、
> frame-boundary commit の結果は `step_frame.edit_results[]` または
> `get_edit_result(ticket)` で返す。commit 点を runtime reload 後・
> freeze_events 前の一点に固定し、replay/golden/strict は method 名と
> reason 入りで reject、**rpc_driver 自体は許可する**(ReloadGate の
> `enabled()` 流用禁止 — `can_edit` を独立 query として追加)。
> 全 transaction は base SceneRevision を検査し、ordered command を全
> preflight 後、AuthoringSceneDocument と runtime adapter へ
> failure-atomic に commit する。部分成功を禁止し、journal には
> committed transaction だけを記録する。

- `add/remove_component` は **ECS-MUT0(archetype migration
  transaction)完了まで約束しない**(レビュー E-6 — 現行公開面に
  migration は存在しない)

### 1-3. journal(JOURNAL0 — レビュー逐語)

> journal record は `{transaction_id, base_revision, committed_revision,
> ordered_forward, ordered_inverse, affected_authoring_ids,
> coalesce_key?, status}` とする。destroy inverse は affected object
> closure、全 raw component JSON、元 object/component array index、
> parent edge を lossless に持つ。reparent は old/new parent と
> local/world preservation policy を持つ。spawn/redo は旧 EntityId で
> なく AuthoringObjectId を再 bind する。multi-select、spawn+configure、
> gizmo drag は明示 transaction/coalesce 単位とする。
> forward→inverse→forward の三段で authoring document semantic
> equality、runtime query equality、behavior lifecycle trace 一致を
> gate とする。

失敗時状態遷移(edit/Save 共通の原則 — 「途中状態を残さない」):

```
edit:  preflight NG → 何も変更せず ticket=rejected(理由入り)
       commit 中に runtime apply NG → document/投影とも rollback・
       journal 追記なし・ticket=failed
save:  digest 不一致 → external_modification error(何も書かない)
       encode/validate NG → temporary 破棄(元 file/cache/revision 不変)
       replace 成功 → file+cache+revision を同一 transaction で公開
```

### 1-4. 同時編集(人+エージェント — v2.2 新設・E-RPC1/JOURNAL0 追加条件)

原則: **編集者は人もエージェントも対等な rpc クライアント**(D0 の帰結
— エージェント専用の裏口も特権も作らない)。分岐する document は
作らない(単一 journal・単一タイムライン)。

1. **actor_id**: 全編集 rpc は `actor_id`(session 内安定な文字列 —
   例 `human:gui` / `agent:<名前>`)を必須で持ち、journal record と
   query 応答(最終更新者)に記録する。省略は stable error。
2. **revision CAS(§1-2 の base SceneRevision 検査の帰結を明文化)**:
   stale revision の edit は**黙って上書きせず** reject し、応答に
   現在 revision と affected field の現在値を含める(再 query
   不要で再判断可能に)。
3. **watch**: `get_scene_revision`(revision + 最終 transaction の
   {actor_id, affected_authoring_ids} のみの軽量 query)を追加。
   v1 はポーリング・WebSocket 展開時に push へ。インスペクタは
   revision 変化で表示中 object を再 query する(stale 表示からの
   上書き事故防止)。
4. **undo = actor 別 revert 方式**: undo は履歴の巻き戻しではなく
   「自分の journal entry の ordered_inverse を**新規 transaction
   として append**」(§1-3 の inverse をそのまま使う)。自分の最終
   entry 以後に**他 actor が同一 field を変更していたら no-op +
   理由応答**(黙って他人の作業を消さない)。redo は対称。
   全 actor 混合のタイムライン undo は v1 非目標。
5. fixture: 二 actor の交互編集で journal の actor 帰属・CAS reject・
   undo no-op 通知・watch の revision 単調性を検査する。

### 1-5. 試行実行(preview — v2.2 新設)

原則: **エンジンは世界を 1 つしか持たない。世界を増やしたければ
プロセスを増やし、操作者を増やしたければクライアントを増やす**。
document のブランチ機構は作らない(試行は「隔離 → 1 commit or
全破棄」であり、マージが存在しないため)。三段梯子:

1. **ticket preview(人に見える試行)**: open 済み ticket 配下で
   試行値を runtime 投影にのみ apply(document 未 commit・journal
   記録なし)。commit で通常の 1 transaction に確定、abort で
   document の値へ再投影(痕跡ゼロ)。WP90 凍結原則「commit して
   いない runtime 値は消える」の意味論そのまま。query/can_edit は
   「preview 中」を metadata で明示する。
2. **eval_preview / render_preview(人に見せない判定)**:
   `{overrides[], queries[] | render{camera?, capture}}` を受け、
   一時 apply → **同一の投影パイプラインで評価**(descendant world
   再計算・派生値・phys raycast/overlap 等を含む)→ 結果 data /
   オフスクリーン capture を返して即 revert。swapchain へ present
   しない・journal/revision/history を一切変更しない。
   - 返すのは「設定した値」ではなく**評価済み状態**。将来
     パラメータ間の数式/制約系(ユーザー空間で発展する側)が
     入っても、評価後の値を返す契約なので additive に吸収される
   - render_preview v1 は **temporal 系無効の単発決定的描画**
     (history 汚染禁止 — TAA/velocity/prev-palette と非干渉)
   - overrides は codec editable 値に限定。構造変更(spawn/destroy/
     reparent)の試行は v1 対象外(→ 3 段目へ)
   - replay/golden/strict では edit 系と同じ規範で reject
3. **スクラッチインスタンス(重い尻尾)**: 構造ごと変える実験・
   時間発展が要る検証(物理を数秒回す等)・クラッシュし得る試行は
   別プロセス。公式パターン = 「query で現 document snapshot を
   export → headless インスタンスに食わせる → rpc でスイープ +
   capture → 採用値を人のセッションへ**通常の編集 rpc 1 件**として
   送る」。昇格専用 API は作らない(D0)。プロジェクト読み取り
   専有・複数インスタンス識別(get_status)は既存要件を流用。

WP 化: §1-4 は E-RPC0(watch)/E-RPC1・JOURNAL0(actor_id/undo)の
条件に統合。§1-5-1 は E-RPC1(ticket 意味論の拡張)、§1-5-2 は
**PREVIEW0(新 WP — E-PROJTX0 後)**、§1-5-3 は機構新設なし
(snapshot export は E-RPC0 query の再利用 + 運用レシピ文書化)。

## 2. エンジン内 UI(ImGui — F2)

### 2-1. UI-INS0 / UI-AB0(改訂条件 — レビュー逐語)

> ImGui は EditorCommandService の query/enqueue/poll-result interface
> だけを使用し、ECS/SceneLoader/LightContainer/PhysWorld を直接
> mutate しない。RPC adapter と UI adapter に同一 command を与え、
> validation/error/ticket/commit trace が一致する fake-service test を
> 置く。grep は補助に降格する。replay/golden/headless では panel
> callback・query・edit enqueue が 0 回であることを既存規範と同じ
> trace で検査する。

- インスペクタの表示: `editable=false`(codec なし)は authored_json の
  read-only 表示 + 理由バッジ。runtime_json がある場合は authored との
  差分表示(「実行時に変化した値」の可視化)
- アセットブラウザは v1 案のまま(閲覧 + 参照コピー + provenance)

### 2-2. Save(SAVE0 改訂条件 — レビュー逐語)

> Save は runtime ECS を列挙して「serialize 可能な component だけ」を
> 出力してはならない。AuthoringSceneDocument の全 envelope/全 scene/
> 全 raw component を deterministic encode し、baseline disk digest
> 一致を確認後、同一 directory の temporary file へ
> write+flush+parse/semantic validate し、atomic replace する。不一致は
> external modification error、codec/pending/runtime-only data の loss は
> hard error とする。file replace、ProjectBasicConfig cache、
> AuthoringSceneDocument revision の公開を一 transaction とし、失敗時は
> 旧 file/cache/revision を維持する。scene は HR 自動 reload 対象外で
> あることを status/UI に明示する。保存→同一 process 明示 reload→
> 新規 process reload の双方で全 scene tree/component semantic
> equality を gate とする。

## 3. WP 分割(レビュー §4.1 の依存順を採用)

```
ED-AUTH0 + ED-CODEC0 → E-RPC0 → ECS-MUT0 + E-RPC1/JOURNAL0
                                   → UI-INS0 / UI-AB0 / SAVE0
```

| WP | 内容 | 条件の添付元 |
|----|------|--------------|
| **ED-AUTH0** | AuthoringSceneDocument/SceneRevision/AuthoringObjectId + cache 統合 | §0-1 逐語 |
| **ED-CODEC0** | codec 五つ組 + 七種 coverage + StructFieldSchema 相乗り | §0-2 逐語(BEH-P0 と共通機構) |
| **E-RPC0** | query 群 + EditorCommandService + fake adapter 等価 | §1-1 逐語 |
| **ECS-MUT0** | archetype migration transaction(WP62 流儀の failure atomicity) | レビュー ECS-MUT0 逐語 |
| **E-RPC1** | edit ticket/commit + can_edit + JOURNAL0 + §1-4(actor_id/CAS/undo)+ §1-5-1(ticket preview) | §1-2/1-3/1-4 逐語 |
| **UI-INS0/AB0** | ImGui パネル(service 経由のみ) | §2-1 逐語 |
| **SAVE0** | atomic 全 document 保存 | §2-2 逐語 |
| **PREVIEW0** | eval_preview / render_preview(E-PROJTX0 後) | §1-5-2 逐語 |

## 4. 未決事項

1. D2(ピッキング/ギズモ)・D3(undo 実行)・WebSocket は v1 のまま
   後続(journal/ticket の形はここで固定済み)
2. behavior の attach 編集は `design_object_behaviors.md` BEH2
   (E-RPC1/JOURNAL0 依存)
3. 非 current scene の編集(document は全 scene を持つ)— v1 は
   current のみ編集可・他は保存で保全のみ
