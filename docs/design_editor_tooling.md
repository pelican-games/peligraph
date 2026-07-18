# 編集系 rpc とエンジン内インスペクタ/アセットブラウザ(v2.3 — 同時編集/試行実行の改稿・再レビュー待ち)

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

### 1-6. matrix と inventory(再レビュー入口 §11 の提出物)

#### 1-6-1. operation / write-set matrix

| op | stable target | precondition(preflight) | adapter | revision/epoch | conflict 時 |
|---|---|---|---|---|---|
| set_component_value | ObjectId+component slot+field path | base rev CAS・editable・schema 検証 | codec runtime apply | rev+1 | stale_revision |
| add_component | ObjectId+component slot(新設) | CAS・slot 不存在・ECS-MUT0 prepare | migration+special adapter | rev+1 | stale_revision / duplicate |
| remove_component | ObjectId+component slot | CAS・slot 存在 | migration+special adapter | rev+1 | stale_revision / missing |
| spawn | subtree existence+declaration interval+name reservation | CAS・名前一意 | 全 codec+migration | rev+1 | stale_revision / name_conflict |
| destroy | 同上(closure) | CAS・closure 解決 | 同上(逆) | rev+1 | stale_revision |
| reparent | child+old/new parent edge+descendant closure | CAS・循環/zero-scale preflight・preserve 指定 | transform closure 再計算 | rev+1 | stale_revision / cycle |
| undo/redo | 元 transaction の write set+domain | CAS・**postcondition/last-writer precondition 全項** | inverse 経由で同上 | rev+1(成功時のみ) | **undo_conflict**(全 no-op) |
| ticket preview open/update | write set(lease) | can_preview・lease 空き・capability | E-C1 prepare→publish | **preview_epoch+1**(rev 不変) | busy / method_unavailable |
| ticket commit | 同上 | ticket base rev CAS | 通常 transaction | rev+1・epoch+1 | stale_revision(forced-abort) |
| ticket abort | — | — | committed document 再投影 | epoch+1 | — |
| eval_preview | overrides(request-local) | can_preview・prepared 評価可能 | PreparedProjection のみ | **不変** | method_unavailable |
| render_preview | overrides+camera | can_preview・非 xr_active | preview graph executor | **不変** | xr_active_unsupported |

#### 1-6-2. renderer preview state inventory(V22-C6 の gate 対象)

| shared state | preview での扱い |
|---|---|
| DeletionQueue(logical epoch) | **advance しない**(request-local frame resources) |
| RenderTiming(published history) | **混ぜない**(preview_request_id namespace の非公開 timing) |
| RenderTargetContainer history | read-only(preview は request-local RT) |
| PolygonInstanceContainer previous state | read-only(advance しない) |
| camera history/snapshot | read-only(explicit camera は request-local) |
| layout tracker | request-local |
| EngineTime / frame index | read-only(advance しない) |
| flat/xr graph variant・selectGraphVariant | **不使用**(第三 preview variant) |
| swapchain / XR mirror | **不使用**(present しない) |

#### 1-6-3. snapshot export/import sequence

```
人 session: export_scene_snapshot
  → {scene_revision R, semantic_scene_bytes, digest,
     current_scene_id, pending_ticket_ids, preview_epoch}
scratch: pelican_player --headless --rpc
         --scene-snapshot <bytes/digest>(launch surface)
  → 同一 project root read-only 共有・scene source だけ差し替え
agent: overrides スイープ + eval/render_preview + capture
採用: 人 session へ通常 edit 1 件(base = R)
  → R の間に人が編集していれば stale_revision(CAS reject)→ 再判断
```

WP 化(§3 の表・グラフ参照): §1-4-1 → E-RPC1/JOURNAL0。§1-4-3 →
E-HOST0(transport)+ WATCH0(enrichment)。§1-5-1/1-5-4 → E-RPC1
(editor gate/override validator を所有)。§1-5-2/1-5-3 → PREVIEW0
(**E-RPC1 完了後** — gate/validator の共通部を所有者から使う)。
§1-5-5 → SNAPSHOT0。

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
ED-AUTH0(済) + ED-CODEC0                  → E-RPC0-base(query + export)
ED-AUTH0 + ED-CODEC0 + ECS-MUT0(済)       → E-PROJTX0
E-RPC0-base + E-PROJTX0                    → E-RPC1/JOURNAL0
WP27(済)                                    → E-HOST0(windowed rpc host)
E-RPC1/JOURNAL0                             → WATCH0(transaction enrichment)
E-RPC1/JOURNAL0(gate/validator 所有)       → PREVIEW0(+WP133 済)
E-RPC0-base(export) + E-HOST0              → SNAPSHOT0
E-RPC0-base                                 → UI-AB0
E-RPC0-base + E-RPC1/JOURNAL0 + E-HOST0    → UI-INS0
ED-AUTH0 + E-RPC1/JOURNAL0                  → SAVE0
```

| WP | 内容 | 条件の添付元 |
|----|------|--------------|
| **ED-AUTH0**(済 WP149) | AuthoringSceneDocument/SceneRevision/AuthoringObjectId + cache 統合 | §0-1 逐語 |
| **ED-CODEC0** | codec 五つ組 + 七種 coverage + StructFieldSchema 相乗り | §0-2 逐語(BEH-P0 と共通機構) |
| **E-RPC0-base** | query 群 + EditorCommandService + fake adapter 等価 + `export_scene_snapshot` | §1-1/§1-5-5 逐語 |
| **ECS-MUT0**(済 WP152) | archetype migration transaction | レビュー ECS-MUT0 逐語 |
| **E-PROJTX0** | 異種 projection の prepare→noexcept-publish 合成 | E-C1 逐語 |
| **E-RPC1/JOURNAL0** | edit ticket/commit + JOURNAL0(write-set/domain/stamp 拡張)+ actor/undo(§1-4-1)+ ticket preview lease(§1-5-1)+ editor gate(§1-5-4)+ override validator | §1-2/1-3/1-4-1/1-4-2/1-5-1/1-5-4 逐語 |
| **E-HOST0** | windowed loop の外部 rpc bounded queue service | §1-4-3 逐語(前半) |
| **WATCH0** | watch token {scene_revision, preview_epoch} + last-transaction 応答 | §1-4-3 逐語(後半) |
| **PREVIEW0** | eval_preview(PreparedProjection)+ render_preview(第三 graph variant) | §1-5-2/1-5-3 逐語 + §1-6 表 |
| **SNAPSHOT0** | snapshot export/ingestion(launch surface) | §1-5-5 逐語 |
| **UI-INS0/AB0** | ImGui パネル(service 経由のみ) | §2-1 逐語 |
| **SAVE0** | atomic 全 document 保存 | §2-2 逐語 |

## 4. 未決事項

1. D2(ピッキング/ギズモ)・D3(undo 実行)・WebSocket は v1 のまま
   後続(journal/ticket の形はここで固定済み)
2. behavior の attach 編集は `design_object_behaviors.md` BEH2
   (E-RPC1/JOURNAL0 依存)
3. 非 current scene の編集(document は全 scene を持つ)— v1 は
   current のみ編集可・他は保存で保全のみ
