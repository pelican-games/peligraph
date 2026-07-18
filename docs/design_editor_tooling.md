# 編集系 rpc とエンジン内インスペクタ/アセットブラウザ(v2.4 — 付表の閉包・再々レビュー待ち)

**v2.4(2026-07-18)**: v2.3 再レビュー
`docs/design_reviews/2026-07-18_editor_v23_rereview_codex.md` は
**逐語転記 8 件を全件合格**とした上で付表を Reject(V23-R1〜R4)。
本版は §7 必須修正の 5 件を反映: ①逐語本文は不変 ②§1-6-1 を
acceptance/execution 二段 + ticket 全遷移(open/update/commit/abort/
forced-abort)+ preview lease overlap で閉じ、E-C4 matrix を別表で
新設 ③§1-6-2 に FrameResources/flat・xr temporal histories/resize/
RenderTiming allocator を追加し全行を三分類 literal に ④snapshot の
versioned JSON schema(export/import)を掲載し RPC import に一本化
⑤§3 に STRUCT-SCHEMA0 edge 復元 + E-C2/E-C4/E-C5/§1-6 の owner 添付。

**v2.3(2026-07-18)**: v2.2 の §1-4/§1-5 は敵対レビュー
`docs/design_reviews/2026-07-18_editor_v22_preview_review_codex.md` で
**Reject**(v2.1 受理部は再審なし)。本版はその **V22-C1〜V22-C7 +
snapshot 必須修正を逐語で本文に反映**し、§1-6(operation/write-set
matrix・renderer preview state inventory・snapshot schema)を新設、
§3 の依存グラフを受理済み再レビュー §6 へ再接続した。**§1-4〜§1-6 は
再レビュー未通過** — E-RPC0 以降の WP 登録前に再提出すること。
v2.2 で誤っていた主要点: ①undo の「同一 field」判定は structural op で
破綻(→ write-set/domain + 一 transaction preflight)②eval_preview の
live apply→revert は E-C1 と非両立(→ PreparedProjection の request-local
評価)③render_preview は既存 renderLogicalFrame 流用不可(→ 第三の
preview graph variant + 隔離 state)④scratch snapshot は query 再利用
では不足(→ export/ingestion 機構 = SNAPSHOT0 新設)。

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

### 1-4. 同時編集(人+エージェント — v2.3・V22-C1〜C3 逐語)

原則: **編集者は人もエージェントも対等な rpc クライアント**(D0)。
分岐する document は作らない(単一 journal・単一タイムライン)。

#### 1-4-1. actor identity と actor 別 revert(V22-C1 逐語)

> actor identity は各 edit request が自己申告する任意文字列にしない。
> session が collision-free な ActorId を発行し、GUI adapter/transport
> connection/再接続 token へ bind する。表示名は identity と分離する。
> journal record は各 command の stable target、read set、write set、
> structural domain、forward postcondition と
> `{revision, transaction_id, actor_id}` last-writer stamp を保持する。
> value field は AuthoringObjectId + stable component slot + schema
> field path、reparent は child/old-new parent edge + descendant
> transform closure、spawn/destroy は object/subtree existence、
> parent edge、declaration-order interval、name reservation を domain に
> 含める。
>
> actor revert は base SceneRevision を持つ一個の通常 transaction とし、
> 全 inverse command の postcondition/last-writer precondition を同じ
> snapshot で preflight する。一項でも original transaction 後の他 actor
> write または structural overlap があれば inverse を一項も適用せず
> `undo_conflict` と conflict domain/owner transaction/revision を返す。
> conflict no-op は revision と journal を進めない。成功した revert
> だけを redo 対象にし、redo にも同じ CAS/precondition を適用する。
> spawn-edit-undo、reparent-local-edit-undo、destroy-index-insert-undo、
> add-edit/remove-undo と descendant cross-edit を二 actor fixture に
> 含める。

(「conflict command だけ no-op、残り適用」は §1-2 の failure
atomicity に反するため不採用 — conflict は transaction 全体 no-op。)

#### 1-4-2. CAS 粒度(V22-C2 逐語 — global CAS は v1 の意図的制約)

> v1 の CAS unit は AuthoringSceneDocument 全体の SceneRevision とする。
> base と current が異なれば対象 object/field が不変でも
> `stale_revision` とし、自動 merge/自動 rebase を行わない。これは
> 偽競合を許容して correctness を優先する v1 の意図的制約である。
> response は current revision と command target の現値/存在状態を
> 返すが、structural command は closure を再 query せず current
> revision だけを差し替えて retry してはならない。actor undo/redo、
> ticket commit、通常 edit の全てに同じ CAS を適用する。watch/polling
> は refresh latency の最適化であって stale overwrite 防止の
> correctness boundary ではなく、CAS reject がその boundary である。
> 無関係 object の edit でも stale になる fixture を expected behavior
> として固定する。

object/field 粒度への緩和は write-set/phantom/closure protocol を
別版で設計してから(v1 ではやらない)。

#### 1-4-3. watch と transport(V22-C3 逐語)

> v1 の watch は polling とし、poll 間の競合安全性は global
> SceneRevision CAS が担保する。ただし stdio RPC を headless blocking
> loop に限定したまま「windowed GUI + agent の同一 session」を完了
> 条件にしない。windowed/embedded engine が外部 RPC request を bounded
> queue として受け、EditorCommandService へ渡し、frame-boundary commit
> 後に応答できる transport owner を先行 WP に置く。一つの外部接続 +
> 組み込み GUI までを v1 とし、複数外部接続は WebSocket 後続でもよい。
>
> watch token は `{scene_revision, preview_epoch}` とする。preview
> open/update/commit/abort/forced-abort は SceneRevision を変えなくても
> preview_epoch を単調増加させ、actor、ticket、affected_authoring_ids、
> 状態を返す。client はどちらかの epoch 変化で表示対象を再 query する。
> watch が遅延・欠落しても古い base revision の commit は必ず CAS
> reject される fixture を置く。

transport owner = **E-HOST0(新 WP)**: windowed loop に外部 rpc の
bounded queue service を足す(WP27 stdio server の frame-boundary
接続)。last-transaction 情報(actor/affected ids)の watch 応答は
JOURNAL0 の publish data なので **WATCH0(JOURNAL0 後の enrichment)**
に分離する。

### 1-5. 試行実行(preview — v2.3・V22-C4〜C7 逐語)

原則: **エンジンは世界を 1 つしか持たない。世界を増やしたければ
プロセスを増やし、操作者を増やしたければクライアントを増やす**。
document のブランチ機構は作らない(試行は「隔離 → 1 commit or
全破棄」でありマージが存在しない)。三段梯子。

#### 1-5-1. ticket preview(人に見える試行 — V22-C4 逐語)

> ticket preview の「痕跡ゼロ」は AuthoringSceneDocument、
> SceneRevision、journal、Save bytes に durable trace を残さない意味に
> 限定し、任意の live observer の副作用まで rollback できるとは
> 約束しない。v1 は一 session 一個の live preview lease とし、ticket に
> actor、base revision、preview_epoch、affected write set、adapter
> capability を持たせる。別 ticket、overlapping edit、scene transition、
> reload、replay/golden/strict 遷移は同時進行させず、stable busy/reason
> error にする。非 overlap edit を許す場合も、その commit で preview の
> base revision が stale になった時点で frame observer 再開前に preview
> を forced-abort する。
>
> live preview 可否は codec editable と同一視せず adapter ごとの
> `live_preview_capability` で明示する。game callback/behavior/physics
> identity/temporal history へ不可逆な副作用を持つ field は v1
> `method_unavailable` とする。preview apply/abort は各々 E-C1 の
> prepare→noexcept publish を通し、abort は current committed document を
> 再投影する。commit は ticket base revision を検査する通常の一
> transaction とし、成功時だけ journal と SceneRevision を一回進める。
> renderer 対象は preview open/abort 境界で temporal reset を一回
> 行うか、history 非参加 field に限定するかを capability ごとに
> 固定する。

#### 1-5-2. eval_preview(人に見せない数値判定 — V22-C5 逐語)

live apply→revert は**しない**(E-C1 の「observer が走らない publish
区間」と非両立)。prepared state の request-local 評価にする:

> eval_preview は live runtime へ publish して revert する API に
> しない。immutable base AuthoringSceneDocument と overrides を
> E-PROJTX0 と同じ codec/validation/adapter prepare に通し、publish
> 直前の `PreparedProjection` を request-local EvaluationContext として
> query adapter へ渡す。descendant world、Light、Phys query、camera/
> renderer-derived value はこの context の explicit state を読む。live
> ECS/Light/Phys/renderer/document/SceneRevision/journal を mutate
> せず、成功・失敗とも context を破棄する。prepared state を明示入力と
> して評価できない adapter/field は v1 `method_unavailable` とする。
> これは persistent document branch ではなく request-local staging で
> あり、merge/commit API を持たない。
>
> gate は evaluation 前後で authored semantic bytes、SceneRevision、
> journal、ECS values と component/chunk versions/global tick、
> EntityId/free-list、Light/Phys binding と identity counter、renderer
> handle/history/epoch、behavior/event trace、EngineTime、reload
> generation が一致することを検査する。複数 override、parent-child
> world closure、collider override + raycast/overlap、prepare fault、
> query fault を含める。

(返すのは「設定した値」でなく**評価済み状態** — 将来のパラメータ間
数式/制約系も評価後の値を返す契約で additive に吸収される。実例:
phys query は既に explicit collider span 入力で staged 評価可能。)

#### 1-5-3. render_preview(人に見せない描画判定 — V22-C6 逐語)

既存 `renderLogicalFrame` の流用は**不可**(deletion epoch/GPU
timing/temporal history を必ず進める)。第三の graph variant を持つ:

> PREVIEW0 は flat/xr と別の `preview` graph variant を module graph
> freeze 前に precompile する。preview feature policy は projection
> jitter、velocity、history read/write、TAA、UI、swapchain/XR mirror を
> 除外し、除外不能な authored pass は feature/pass 名入りで起動時に
> reject する。preview executor は request-local render target/layout
> tracker/frame resources/temporal snapshot を持ち、shared
> RenderTargetContainer history、PolygonInstanceContainer previous
> state、Renderer flat/xr history、DeletionQueue logical epoch、
> RenderTiming published history、EngineTime/frame index を
> advance/reset しない。GPU timing が必要なら `preview_request_id`
> namespace の非公開 timing とし、通常 logical-frame 集計へ混ぜない。
>
> capture request は width/height、pixel encoding、camera identity/
> explicit camera、graph generation、最大 byte 数を schema で固定し、
> swapchain/OpenXR へ present しない。v1 は `xr_active` 中の
> render_preview を method+reason 付き `xr_active_unsupported` で
> reject する。XR 中にも許可する版は HMD wait/begin/end、mirror、
> pacing、GPU timing と非干渉な auxiliary submission を別 fixture/実機
> gate で証明してから追加する。通常 frame の直前/直後に preview を
> 挟み、flat/XR history epoch、previous palette/model、deletion epoch、
> timing status、次の capture bytes が preview 無し control と一致する
> ことを gate とする。

(WP133 の feature filter/起動時 compile は再利用可・XR graph と
`selectGraphVariant()` 自体の再利用は禁止。)

#### 1-5-4. editor gate(V22-C7 逐語)

> `can_edit` と `can_preview` は ReloadGate から独立した editor gate
> snapshot とし、reason bit を replay、golden、strict、reload/
> scene-transition、preview-lease conflict に分ける。rpc_driver と
> headless は reject reason にしない。ticket preview の open/update/
> commit、eval_preview、render_preview は request acceptance と
> frame-boundary execution の双方で snapshot/epoch を検査し、途中で
> gate が閉じた request は live mutation なしで method 名、reason、
> gate epoch 付き reject/failed にする。open live preview がある状態
> では replay 開始、reload、scene transition を reject するか、
> observer 再開前に E-C1 abort を完了してから遷移する。replay/golden/
> strict では preview handler/evaluation/render graph invocation が
> 0 回であることを fixture にする。

#### 1-5-5. スクラッチインスタンス(SNAPSHOT0 — レビュー §8 必須修正逐語)

「E-RPC0 query の再利用で足りる」は**撤回**。export/ingestion を
機構として持つ(document branch/merge ではなく immutable
export/import。昇格専用 API は引き続き作らない — 採用値は通常 edit):

> E-RPC0 に `export_scene_snapshot` を明示し、同一 lock/snapshot から
> `{scene_revision, semantic_scene_bytes, digest, current_scene_id,
> pending_ticket_ids, preview_epoch}` を返す。bytes は
> AuthoringSceneDocument の deterministic semantic encode を一回だけ
> 使用し、全 envelope/全 scene/全 object/全 raw component/declaration
> order を含む。accepted 未 commit edit、ticket preview、runtime-only
> component は bytes に含めず、metadata で明示する。caller は pending/
> preview が 0 でない snapshot を許容するか stable busy error を
> 選べる。AuthoringObjectId は source session 内 identity であり
> scratch process へ永続移植しない。
>
> scratch 側には snapshot bytes/digest を startup 前に
> ProjectBasicConfig へ与える launch surface または RPC import surface
> を置き、通常 project の scene file を上書きしない。asset/rendering/
> shader は同じ project root を read-only 共有し、scene source だけ
> snapshot に差し替える。複数 scene、非 current scene、unknown/
> read-only component、pending edit、preview 中、raw numeric/array
> order の round-trip fixture を置く。採用 edit は source snapshot
> revision を base とする人 session の通常 edit 一件であり、その間に
> 人が edit していれば global CAS reject する。

### 1-6. matrix と inventory(再レビュー入口 §11 の提出物 — v2.4 で閉包)

#### 1-6-1a. transaction / preview state matrix(owner = E-RPC1/JOURNAL0)

precondition は **acceptance(受付時)/ execution(frame-boundary
実行時)の二段**。execution では必ず editor gate snapshot/epoch を
再検査し、閉じていれば live mutation なしで
`gate_closed{method, reason, gate_epoch}`(V22-C7)。通常 edit 行は
**open preview lease との write-set overlap** を acceptance/execution
双方で検査し、overlap は `preview_lease_conflict{ticket, owner}`。

| op | stable target / owner | acceptance precondition | execution precondition | adapter | rev / preview_epoch | stable conflict result |
|---|---|---|---|---|---|---|
| set_component_value | ObjectId+component slot+field path | base rev CAS・can_edit・editable・schema 検証・lease overlap なし | gate 再検査・lease 再検査・preflight | codec runtime apply(E-PROJTX0) | rev+1 | stale_revision / not_editable / preview_lease_conflict / gate_closed |
| add_component | ObjectId+component slot(新設) | 同上 + slot 不存在 | 同上 + ECS-MUT0 prepare | migration + special adapter | rev+1 | stale_revision / duplicate_component / preview_lease_conflict / gate_closed |
| remove_component | ObjectId+component slot | 同上 + slot 存在 | 同上 | migration + special adapter | rev+1 | stale_revision / missing_component / preview_lease_conflict / gate_closed |
| spawn | object/subtree existence + declaration interval + name reservation | CAS・can_edit・名前一意・parent 存在・lease overlap なし | gate/lease 再検査 + 全 codec preflight | 全 codec + migration | rev+1 | stale_revision / name_conflict / parent_not_found / preview_lease_conflict / gate_closed |
| destroy | 同上(closure) | CAS・can_edit・closure 解決・lease overlap なし | gate/lease 再検査 | 同上(逆) | rev+1 | stale_revision / preview_lease_conflict / gate_closed |
| reparent | child + old/new parent edge + descendant closure | CAS・can_edit・循環/zero-scale preflight・preserve 指定・lease overlap なし | gate/lease 再検査 + closure 再計算 preflight | transform closure 再計算 | rev+1 | stale_revision / cycle_detected / zero_scale / preview_lease_conflict / gate_closed |
| undo / redo | 元 transaction の write set + structural domain | CAS・can_edit・**postcondition/last-writer precondition 全項**・lease overlap なし | gate/lease 再検査 + precondition 再検査(同一 snapshot) | inverse 経由で各 op と同一 | rev+1(成功時のみ) | **undo_conflict{domain, owner_txn, revision}**(全 no-op・rev/journal 不変)/ stale_revision / preview_lease_conflict / gate_closed |
| ticket preview **open** | write set lease(ActorId 所有) | can_preview・lease 空き(一 session 一個)・全対象 field の live_preview_capability | gate 再検査 | E-C1 prepare→noexcept publish | epoch+1(rev 不変) | preview_lease_busy{owner} / method_unavailable{field} / gate_closed |
| ticket preview **update** | 同一 lease(所有 ActorId のみ) | lease 所有・can_preview・capability | gate 再検査 | 同上 | epoch+1(rev 不変) | not_lease_owner / method_unavailable / gate_closed |
| ticket **commit** | 同一 lease | lease 所有・ticket base rev CAS・can_edit | gate 再検査 + 通常 transaction preflight | 通常の一 transaction | rev+1・epoch+1・lease 解放 | stale_revision(→ forced-abort 遷移)/ not_lease_owner / gate_closed |
| ticket **abort**(所有者) | 同一 lease | lease 所有 | — (abort は拒否されない) | committed document を E-C1 prepare→noexcept publish で再投影 | epoch+1(rev 不変)・lease 解放 | not_lease_owner のみ |
| ticket **forced-abort**(system) | open lease(system 発火) | 発火条件: 非 overlap edit commit による base stale 化 / replay・reload・scene transition 遷移要求 | **frame observer 再開前**に abort と同一 adapter 経路 | epoch+1・lease 解放 | 結果通知 = ticket_forced_aborted{reason} |
| eval_preview | overrides(request-local・lease 不要) | can_preview・全 override が prepared 評価可能 | gate 再検査 | PreparedProjection + EvaluationContext のみ(live 不変) | **rev/epoch とも不変** | method_unavailable{adapter/field} / gate_closed |
| render_preview | overrides + camera(request-local) | can_preview・非 xr_active・capture schema 検証(size 上限) | gate 再検査 | preview graph executor(§1-6-2 準拠) | **rev/epoch とも不変** | xr_active_unsupported / capture_too_large / gate_closed |

#### 1-6-1b. E-C4 operation matrix(owner = E-RPC1/JOURNAL0 — 受理済み E-C4 の列)

| op | authoring forward | authoring inverse | runtime adapter | 必要先行 WP | preflight error | activation/deactivation boundary |
|---|---|---|---|---|---|---|
| set_component_value | staged document へ field write | 旧値 write(last-writer stamp 付き) | codec runtime apply(transform は descendant closure 再計算) | E-PROJTX0 | stale_revision / not_editable / schema violation | なし(observer 再開前に publish 完了) |
| add_component | component 挿入(slot 採番・raw canonical) | 同 slot remove | ECS-MUT0 migration + special adapter publish | ECS-MUT0(済 WP152)+ E-PROJTX0 | duplicate / unknown type / schema violation | behavior 付き component は commit 後の次 activation boundary で onInit(BEH2 規範) |
| remove_component | component 削除(raw JSON・array index 保存) | 保存 raw から復元 | migration + special adapter unpublish | 同上 | missing_component | pre-destroy で deinit(逆 attachment 順) |
| spawn | object 挿入(declaration interval + name reservation + parent edge) | closure destroy | 全 codec project + migration | E-PROJTX0 | name_conflict / parent_not_found | activation barrier で attachment_seq 順 onInit |
| destroy | closure 削除(全 raw・order・parent edge を journal に保存) | 保存 closure から spawn 復元 | 逆順 unpublish + pre-destroy barrier | E-PROJTX0 | closure 解決不能 | pre-destroy barrier(逆 attachment 順 onDestroy) |
| reparent | parent edge 差し替え + preserve policy 記録 | 旧 edge + 旧 local/world closure 復元 | descendant world 再計算(E-C2) | E-PROJTX0 | cycle / zero_scale | なし |
| undo / redo | inverse command 列の通常 transaction 再生 | (redo = revert の逆) | 各 op と同一 | JOURNAL0 | undo_conflict / stale_revision | 各 op に従う |

#### 1-6-2. renderer preview state inventory(V22-C6 の gate 対象 — 全行 literal 三分類)

分類 literal: `read-only`(shared を読むが変更しない)/
`request-local`(request 専用 instance を持ち shared に触れない)/
`explicitly suppressed`(読みも書きもしない — 実装/検査で禁止)。

| shared state | 分類 | 備考 |
|---|---|---|
| DeletionQueue(logical epoch) | explicitly suppressed | epoch advance 禁止。preview の一時 resource は request-local queue + fence 完了時即解放 |
| shared FrameResources(beginLogicalFrame / slot select / uniform update) | request-local | preview executor が専用 frame resources を所有 |
| RenderTiming query allocator / pending range / published status | request-local | `preview_request_id` namespace の専用 timing instance。shared query ring の configure/record は explicitly suppressed |
| RenderTargetContainer history | explicitly suppressed | preview は request-local RT のみ。shared history の read も write もしない(§1-5-3 の history read/write 除外と一致) |
| PolygonInstanceContainer previous state(instance history) | explicitly suppressed | preview graph は velocity/history pass を含まないため参照も advance もしない |
| Renderer flat/xr temporal histories + last_view_snapshots + reset/observed revision | explicitly suppressed | 第三 variant は flat/xr の temporal state 群に触れない |
| camera history / snapshot | explicitly suppressed | explicit camera は request-local。shared camera snapshot の commit なし |
| internal_render_extent / resize / RT rebind path | explicitly suppressed | request width/height は request-local RT にのみ適用。shared extent 更新・recreate・DeletionQueue defer を発火させない |
| layout tracker | request-local | |
| EngineTime / frame index | read-only | advance しない(評価は現在時刻を読むのみ) |
| flat/xr graph variant・selectGraphVariant() | explicitly suppressed | 第三 `preview` variant を使用 |
| swapchain / XR mirror / present | explicitly suppressed | present しない |

#### 1-6-3. snapshot export/import JSON schema と sequence(owner = SNAPSHOT0)

ingestion は **RPC import surface に一本化**する(scratch は headless
stdio rpc = WP27 既存面。launch option 案は撤回 — bytes を CLI に載せる
encoding/size 問題を避け、通常 project 起動 → import → reload の一本道
にする)。

export request(`export_scene_snapshot`):

```json
{ "allow_pending": false }
```

- `allow_pending`(bool・省略時 false): false で pending_ticket_ids が
  非空 or open preview lease あり → stable error `snapshot_busy`
  (何も返さない)。true なら snapshot は commit 済み状態のみを含み、
  pending/preview は metadata で明示。

export response:

```json
{
  "schema_version": 1,
  "scene_revision": 42,
  "current_scene_id": "main",
  "semantic_scene_bytes": "<AuthoringSceneDocument.encodeSemantic() の
                            UTF-8 JSON text 全文(required・non-null)>",
  "digest": { "algorithm": "sha256",
              "hex": "<semantic_scene_bytes の UTF-8 byte 列の
                      sha256、64 桁小文字 hex(required)>" },
  "pending_ticket_ids": [],
  "preview_epoch": 7
}
```

- 型: scene_revision/preview_epoch = uint64、current_scene_id =
  string(必須・document に存在する scene 名)、pending_ticket_ids =
  string 配列(必須・空可)。bytes は **UTF-8 JSON string として
  inline**(base64/別 file 不採用 — semantic encode は JSON text で
  あり二重 encode を避ける)。
- size 上限 64 MiB(超過 = stable error `snapshot_too_large`)。

import request(`import_scene_snapshot`・scratch 側):

```json
{
  "schema_version": 1,
  "semantic_scene_bytes": "<export と同一 text>",
  "digest": { "algorithm": "sha256", "hex": "<同上>" },
  "current_scene_id": "main"
}
```

- 検証順: ①schema_version 既知(`unsupported_snapshot_version`)
  ②size 上限 ③digest 再計算一致(`digest_mismatch`)④parse +
  semantic validate(`snapshot_invalid{detail}`)⑤current_scene_id
  存在(`scene_not_found`)。**どの失敗でも通常 project の scene
  source/ProjectBasicConfig cache/revision は不変**(公開は全検証
  成功後の一 transaction — E-C5 と同型)。
- 成功: scene source を snapshot に差し替えて reload。asset/
  rendering/shader は project root を read-only 共有。
  AuthoringObjectId は再採番(source session の id を移植しない)。

sequence(schema 名で記述):

```
人 session : export_scene_snapshot{allow_pending:false}
             → response(scene_revision R, bytes, digest, ...)
scratch    : pelican_player --headless --rpc(通常 project 起動)
             → import_scene_snapshot{bytes, digest, current_scene_id}
             → 検証 5 段 → 成功で snapshot scene を load
agent      : overrides スイープ + eval_preview / render_preview
             + capture(scratch 内・人の GUI 無風)
採用       : 人 session へ通常 edit 1 件(base = R)
             → R 以後に人が編集済みなら stale_revision(global CAS
               reject)→ 現在値を見て再判断
```

WP 化(§3 の表・グラフ参照): §1-4-1 → E-RPC1/JOURNAL0。§1-4-3 →
E-HOST0(transport)+ WATCH0(enrichment)。§1-5-1/1-5-4/§1-6-1a/
§1-6-1b → E-RPC1/JOURNAL0(editor gate/override validator を所有)。
§1-5-2/1-5-3/§1-6-2 → PREVIEW0(**E-RPC1 完了後**)。§1-5-5/§1-6-3 →
SNAPSHOT0。

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

## 3. WP 分割(v2.3 — 受理済み再レビュー §6 + v2.2 レビュー §9 の修正グラフ)

```
WP71(済) → STRUCT-SCHEMA0(済 WP150)      → ED-CODEC0
ED-AUTH0(済 WP149) + ED-CODEC0            → E-RPC0-base(query + export)
ED-AUTH0 + ED-CODEC0 + ECS-MUT0(済 WP152) → E-PROJTX0
E-RPC0-base + E-PROJTX0                    → E-RPC1/JOURNAL0
WP27(済)                                    → E-HOST0(windowed rpc host)
E-RPC1/JOURNAL0                             → WATCH0(transaction enrichment)
E-RPC1/JOURNAL0(gate/validator 所有) + WP133(済) → PREVIEW0
E-RPC0-base(export) + E-HOST0(人 session 側) → SNAPSHOT0
                     (scratch 側 import は WP27 stdio rpc 既存面)
E-RPC0-base                                 → UI-AB0
E-RPC0-base + E-RPC1/JOURNAL0 + E-HOST0    → UI-INS0
ED-AUTH0 + E-RPC1/JOURNAL0                  → SAVE0
```

| WP | 内容 | 条件の添付元(逐語 owner) |
|----|------|--------------|
| **ED-AUTH0**(済 WP149) | AuthoringSceneDocument/SceneRevision/AuthoringObjectId + cache 統合 | §0-1 |
| **STRUCT-SCHEMA0**(済 WP150) | 共通 field descriptor + 三 use-site policy | B-C1 |
| **ED-CODEC0** | codec 五つ組 + 七種 coverage(共通 schema API 新設禁止 — WP150 を使用) | §0-2 + **E-C2**(transform local/world 意味論)+ E-C3-3(七種 canonical 例) |
| **E-RPC0-base** | query 群 + EditorCommandService + fake adapter 等価 + `export_scene_snapshot` | §1-1 + §1-6-3(export schema) |
| **ECS-MUT0**(済 WP152) | archetype migration transaction | E-C3-1 |
| **E-PROJTX0** | 異種 projection の prepare→noexcept-publish 合成 | **E-C1** + **E-C2**(descendant closure/reparent preserve) |
| **E-RPC1/JOURNAL0** | edit ticket/commit + JOURNAL0(write-set/domain/stamp)+ actor/undo + ticket preview lease + editor gate + override validator | §1-2/1-3 + **E-C3-2**(競合 fixture)+ **E-C4**(operation matrix = §1-6-1b)+ §1-4-1/1-4-2/1-5-1/1-5-4 + §1-6-1a |
| **E-HOST0** | windowed loop の外部 rpc bounded queue service | §1-4-3(前半) |
| **WATCH0** | watch token {scene_revision, preview_epoch} + last-transaction 応答 | §1-4-3(後半) |
| **PREVIEW0** | eval_preview(PreparedProjection)+ render_preview(第三 graph variant) | §1-5-2/1-5-3 + **§1-6-2**(state inventory) |
| **SNAPSHOT0** | snapshot export/ingestion(RPC import) | §1-5-5 + **§1-6-3**(JSON schema/sequence) |
| **UI-INS0/AB0** | ImGui パネル(service 経由のみ) | §2-1 |
| **SAVE0** | atomic 全 document 保存 | §2-2 + **E-C5**(no-throw publication + fault injection) |

## 4. 未決事項

1. D2(ピッキング/ギズモ)・D3(undo 実行)・WebSocket は v1 のまま
   後続(journal/ticket の形はここで固定済み)
2. behavior の attach 編集は `design_object_behaviors.md` BEH2
   (E-RPC1/JOURNAL0 依存)
3. 非 current scene の編集(document は全 scene を持つ)— v1 は
   current のみ編集可・他は保存で保全のみ
