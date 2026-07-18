# editor tooling v2.2 同時編集・試行実行 敵対レビュー

日付: 2026-07-18

対象: `docs/design_editor_tooling.md` v2.2 の §1-4 / §1-5 のみ

根拠ツリー: `agent/review-v22` (`8a4551b`)

## 0. 結論

**Reject**。

v2.1 までの §0〜§1-3・§2 以降と E-C1〜E-C5 は再審していない。今回の
Reject は新設節だけに対するものである。

| 対象 | 判定 | 理由 |
|---|---|---|
| §1-4 actor 別 undo | **Reject** | journal に field/write-set identity がなく、spawn/destroy/reparent と他 actor の後続編集を安全に判定できない。request 内の任意文字列を actor identity とする規範も衝突・なりすましを防がない |
| §1-4 document 全体 CAS | **Accept (v1 の保守的選択として)** | 無関係 object も stale にする偽競合は生じるが、hierarchy closure と異種 adapter を一 transaction にする v1 では安全側である。watch は CAS の代替ではない、と固定する必要がある |
| §1-4 watch | **Reject (現行 WP 化のままでは)** | polling 自体は妥当だが、現行 stdio RPC は headless 専用の同期 1:1 loop で、人の windowed GUI と agent が同一 process を同時操作する transport がない。また runtime-only preview の変化は SceneRevision に現れない |
| §1-5-1 ticket preview | **Reject** | live runtime を複数 frame 公開した後の abort は、game callback、ECS version、physics binding、renderer temporal state 等を「document 値の再投影」だけでは消せない。複数 actor/ticket、通常 edit、reload/replay/scene transition との排他も未定義 |
| §1-5-2 eval_preview | **Reject** | E-C1 の prepare/publish と「一時 live apply→評価→revert」を同一視している。評価のため observer を走らせた時点で、値以外の副作用を lossless に巻き戻す契約がない |
| §1-5-2 render_preview | **Reject** | 現行 `renderLogicalFrame` は deletion epoch、GPU timing、RT/object/camera temporal history を必ず進める。temporal feature を除いただけの呼出しでは非汚染にならず、RPC/headless では WP133 の XR graph 自体も compile されない |
| §1-5-3 scratch instance | **Reject** | E-RPC0 の object query は full scene envelope の export/import protocol ではない。現行には snapshot bytes を scratch process へ注入する RPC/launch surface もない |
| WP 分割 | **Reject** | PREVIEW0 の依存が `E-PROJTX0 後`だけでは不足し、E-RPC0 に入れた watch の last-transaction 部分は後続 JOURNAL0 に逆依存する。本文の依存図も受理済み §6 と矛盾したまま |

以下の V22-C1〜V22-C7 を本文と各 WP に逐語添付し、設計を再提出するまで、
E-RPC0/E-RPC1/JOURNAL0 への §1-4/§1-5-1 統合と PREVIEW0 登録を開始しては
ならない。

## 1. actor identity と actor 別 revert は structural operation で閉じていない

§1-4 は「他 actor が同一 field を変更していたら no-op」とする
(`docs/design_editor_tooling.md:188-193`)。しかし既存 JOURNAL0 record は
`affected_authoring_ids` と ordered forward/inverse までで、field identity、write set、
postcondition、last-writer stamp を持たない (`docs/design_editor_tooling.md:147-154`)。

現行 authoring 表現でも、「同一 field」を JSON path だけで定義できない。

- object は `declaration_index` と raw JSON pointer、`parent` は object 名文字列である
  (`src/core/loader/authoringscenedocument.hpp:28-35`,
  `src/core/loader/authoringscenedocument.cpp:71-82`)。
- component identity は `declaration_index` と raw JSON pointer だけで、stable component
  slot id はない (`src/core/loader/authoringscenedocument.hpp:21-25`,
  `src/core/loader/authoringscenedocument.cpp:84-90`)。
- scene v1 の hierarchy edge 自体が `objects[].parent` の名前参照であり、parent の
  rename/destroy/restore と array position が相互作用する
  (`docs/design_scene_format.md:80-85`, `src/project/sceneformat.cpp:103-112`)。
- runtime の add/remove は一 component の「field」変更ではなく archetype migration
  であり、移動先・移動元 chunk の全 component version と external adapter publication
  を更新する (`src/core/ecs/archetypemigration.cpp:171-221`)。

具体的に次の履歴で §1-4 の no-op 判定は一意に決まらない。

| 履歴 | 単純な inverse の危険 |
|---|---|
| A: spawn X → B: X の component 値を edit → A: undo spawn | inverse destroy は B の成果ごと X を消す。「同一 field」比較だけでは spawn の所有 domain が定まらない |
| A: reparent C(P→Q, preserve world) → B: C の local TRS を edit → A: undo | parent edge だけ戻しても B の world を変え、local TRS も戻せば B の edit を消す。descendant closure も同時に影響する |
| A: destroy subtree → B: 元 array index へ別 object を挿入、または旧名を再利用 → A: undo | 保存済み index/parent edge の復元が B の declaration order や名前一意性と衝突する |
| A: add component → B: その component を edit/remove → A: undo | remove inverse の対象 slot と B の transaction の関係を component 名/array index だけでは識別できない |

さらに `actor_id` は request が自由に指定する session 内文字列としか書かれていない
(`docs/design_editor_tooling.md:176-178`)。同名 actor の二接続、再接続、agent が
`human:gui` を送る場合を区別できず、「自分の entry」という authorization/ownership の
前提が成立しない。

### V22-C1 (逐語)

> actor identity は各 edit request が自己申告する任意文字列にしない。session が
> collision-free な ActorId を発行し、GUI adapter/transport connection/再接続 token へ
> bind する。表示名は identity と分離する。journal record は各 command の stable target、
> read set、write set、structural domain、forward postcondition と
> `{revision, transaction_id, actor_id}` last-writer stamp を保持する。value field は
> AuthoringObjectId + stable component slot + schema field path、reparent は child/old-new
> parent edge + descendant transform closure、spawn/destroy は object/subtree existence、
> parent edge、declaration-order interval、name reservation を domain に含める。
>
> actor revert は base SceneRevision を持つ一個の通常 transaction とし、全 inverse command
> の postcondition/last-writer precondition を同じ snapshot で preflight する。一項でも
> original transaction 後の他 actor write または structural overlap があれば inverse を
> 一項も適用せず `undo_conflict` と conflict domain/owner transaction/revision を返す。
> conflict no-op は revision と journal を進めない。成功した revert だけを redo 対象にし、
> redo にも同じ CAS/precondition を適用する。spawn-edit-undo、reparent-local-edit-undo、
> destroy-index-insert-undo、add-edit/remove-undo と descendant cross-edit を二 actor fixture に
> 含める。

「conflict command だけ no-op、残りの inverse は適用」は §1-2 の transaction 全体の
failure atomicity に反するため採用できない。v1 は transaction 全体 conflict とするのが
最小である。

## 2. global SceneRevision CAS は v1 では維持してよい

現行の SceneRevision は document publication ごとに一つ採番され、candidate 全体を swap
してから counter を進める document-wide identity である
(`src/core/loader/basicconfig.cpp:433-442`)。§1-2 も「全 transaction は base
SceneRevision を検査」としている (`docs/design_editor_tooling.md:130-139`)。

従って object A の edit 後は、古い revision で object B を edit しても reject される。
これは偽競合だが lost update ではない。reparent の descendant closure、camera/light/phys/
renderer への異種投影、spawn/destroy の declaration order まで object-local version へ分解
すると、今度は複数 version の atomic snapshot と structural phantom 検出が必要になる。
v1 でその機構を追加する方が危険である。

### V22-C2 (逐語)

> v1 の CAS unit は AuthoringSceneDocument 全体の SceneRevision とする。base と current が
> 異なれば対象 object/field が不変でも `stale_revision` とし、自動 merge/自動 rebase を
> 行わない。これは偽競合を許容して correctness を優先する v1 の意図的制約である。
> response は current revision と command target の現値/存在状態を返すが、structural command
> は closure を再 query せず current revision だけを差し替えて retry してはならない。
> actor undo/redo、ticket commit、通常 edit の全てに同じ CAS を適用する。watch/polling は
> refresh latency の最適化であって stale overwrite 防止の correctness boundary ではなく、
> CAS reject がその boundary である。無関係 object の edit でも stale になる fixture を
> expected behavior として固定する。

object/field version への緩和は、write-set/phantom/closure の protocol を別版で設計してからで
よい。

## 3. watch の polling は妥当だが、現行 transport では同時 session が成立しない

`RpcServer::run()` は一つの `istream` から一行読み、一 handler を同期完了し、一応答を
flush してから次行へ進む (`src/core/communication/rpcserver.cpp:712-720`)。さらに main loop は
`headless && rpc` のときこの blocking server へ入り、windowed loop へは進まない
(`src/core/appflow/loop.cpp:381-387`)。公開 manual も「stdio JSON-RPC、v1 は
`--headless` 必須」と明記する (`docs/manual/10_tools.md:30-35`)。

一方、D0 は devstudio と agent を RPC client とし、複数 client は WebSocket 展開とする
(`docs/design_devstudio_direction.md:14-35`)。従って「v1 は polling」は push 不在への回答には
なるが、人の windowed GUI と外部 agent を同じ engine session へ接続する回答にはなって
いない。WebSocket を v1 に必須化する必要はないが、少なくとも windowed engine が一つの
外部 RPC queue を frame boundary で service する transport/host WP が必要である。

また `get_scene_revision` は committed transaction しか見ない
(`docs/design_editor_tooling.md:183-187`)。ticket preview は document/revision を変えないのに
query metadata の `preview 中` を変える (`docs/design_editor_tooling.md:204-209`)ため、revision
だけを poll する inspector は preview 開始/更新/abort を検知できない。

### V22-C3 (逐語)

> v1 の watch は polling とし、poll 間の競合安全性は global SceneRevision CAS が担保する。
> ただし stdio RPC を headless blocking loop に限定したまま「windowed GUI + agent の同一
> session」を完了条件にしない。windowed/embedded engine が外部 RPC request を bounded queue
> として受け、EditorCommandService へ渡し、frame-boundary commit 後に応答できる transport
> owner を先行 WP に置く。一つの外部接続 + 組み込み GUI までを v1 とし、複数外部接続は
> WebSocket 後続でもよい。
>
> watch token は `{scene_revision, preview_epoch}` とする。preview open/update/commit/abort/
> forced-abort は SceneRevision を変えなくても preview_epoch を単調増加させ、actor、ticket、
> affected_authoring_ids、状態を返す。client はどちらかの epoch 変化で表示対象を再 query
> する。watch が遅延・欠落しても古い base revision の commit は必ず CAS reject される
> fixture を置く。

## 4. ticket preview の「痕跡ゼロ」は live world では成立しない

§1-5-1 は runtime にだけ apply した値を複数 request/frame にわたって見せ、abort で
document 値へ再投影して「痕跡ゼロ」とする (`docs/design_editor_tooling.md:204-209`)。
しかし live runtime は値の倉庫ではない。

- ECS `setComponent` は live value を先に copy-assign し、その component version を更新する
  (`src/core/userpublic/details/ecs/coretemplate.hpp:163-172`,
  `src/core/userpublic/details/ecs/coretemplate.cpp:568-579`)。
- 各 frame は event callback、ECS/game system、animation、SeqPlayer、pending scene load を実行する
  (`src/core/appflow/framephase.cpp:90-150`)。preview 値を callback が見て発火した event や別の
  runtime state mutation は、元 component 値の再投影では戻らない。
- 通常 render は object/model/skin/morph/material の previous state を current へ進める
  (`src/core/renderer/polygoninstancecontainer.cpp:666-699`)。abort 後も preview frame が temporal
  history に入った事実は残る。
- collider の bind は monotonic ColliderId を消費する
  (`src/core/phys/physworld.cpp:245-250`, `src/core/phys/physworld.cpp:256-312`)ため、adapter が
  rebind で preview/revert を実装すれば値が戻っても identity counter は戻らない。

加えて、同時に二 actor が ticket preview を開く場合、通常 edit が preview 中の field または
別 field を commit する場合、scene transition/reload/replay が始まる場合の owner と abort
順が未定義である。

### V22-C4 (逐語)

> ticket preview の「痕跡ゼロ」は AuthoringSceneDocument、SceneRevision、journal、Save bytes
> に durable trace を残さない意味に限定し、任意の live observer の副作用まで rollback
> できるとは約束しない。v1 は一 session 一個の live preview lease とし、ticket に actor、
> base revision、preview_epoch、affected write set、adapter capability を持たせる。別 ticket、
> overlapping edit、scene transition、reload、replay/golden/strict 遷移は同時進行させず、
> stable busy/reason error にする。非 overlap edit を許す場合も、その commit で preview の
> base revision が stale になった時点で frame observer 再開前に preview を forced-abort する。
>
> live preview 可否は codec editable と同一視せず adapter ごとの
> `live_preview_capability` で明示する。game callback/behavior/physics identity/temporal history へ
> 不可逆な副作用を持つ field は v1 `method_unavailable` とする。preview apply/abort は各々
> E-C1 の prepare→noexcept publish を通し、abort は current committed document を再投影する。
> commit は ticket base revision を検査する通常の一 transaction とし、成功時だけ journal と
> SceneRevision を一回進める。renderer 対象は preview open/abort 境界で temporal reset を一回
> 行うか、history 非参加 field に限定するかを capability ごとに固定する。

literal な「全 runtime 痕跡ゼロ」が要件なら、frame をまたぐ ticket preview は削除し、次節の
isolated eval だけにする必要がある。

## 5. eval_preview は live apply/revert でなく prepared state の評価にする

E-C1 の受理済み逐語は、全 adapter の `prepare` は live state を変えず、全成功後だけ
frame-boundary で `noexcept publish` すると定める
(`docs/design_reviews/2026-07-18_editor_behavior_v2_rereview_codex.md:58-64`)。これに対し
§1-5-2 は「一時 apply→同一投影 pipeline で評価→即 revert」とする
(`docs/design_editor_tooling.md:210-215`)。

prepare 済み state を一度 live publish しなければ既存 observer は評価できず、publish して
observer を走らせれば E-C1 の「observer が走らない publish 区間」は終わる。その後に document
値を再 publish しても、ECS version、cache/version counter、allocator identity、callback trace は
元に戻らない。Light setter も live object への直接代入である
(`src/core/light/lightcontainer.cpp:243-280`)。

一方、physics query は explicit collider value の vector を構築して provider に渡す形であり
(`src/core/phys/physworld.cpp:315-345`)、provider 境界も collider span を引数にする
(`src/core/phys/physicsruntime.hpp:30-41`)。これは live `PhysWorld` を差し替えず staged projection
を評価できる実例である。

### V22-C5 (逐語)

> eval_preview は live runtime へ publish して revert する API にしない。immutable base
> AuthoringSceneDocument と overrides を E-PROJTX0 と同じ codec/validation/adapter prepare に
> 通し、publish 直前の `PreparedProjection` を request-local EvaluationContext として query
> adapter へ渡す。descendant world、Light、Phys query、camera/renderer-derived value はこの
> context の explicit state を読む。live ECS/Light/Phys/renderer/document/SceneRevision/journal を
> mutate せず、成功・失敗とも context を破棄する。prepared state を明示入力として評価できない
> adapter/field は v1 `method_unavailable` とする。これは persistent document branch ではなく
> request-local staging であり、merge/commit API を持たない。
>
> gate は evaluation 前後で authored semantic bytes、SceneRevision、journal、ECS values と
> component/chunk versions/global tick、EntityId/free-list、Light/Phys binding と identity counter、
> renderer handle/history/epoch、behavior/event trace、EngineTime、reload generation が一致する
> ことを検査する。複数 override、parent-child world closure、collider override + raycast/overlap、
> prepare fault、query fault を含める。

この方式なら「人の GUI に試行過程を映さない」を scheduler timing に依存せず構造的に保証
できる。

## 6. render_preview には第三の precompiled graph と隔離 state が必要

WP133 は flat と、TAA/jitter/velocity/history/UI を除いた XR graph を起動時に compile する
(`docs/implementation_plan.md:3426-3456`)。現行実装も Renderer が持つ variant は flat/xr の二つ
だけで (`src/core/vkcore/renderer_config.hpp:13-19`)、XR graph は `xr_active` の起動時だけ登録する
(`src/core/vkcore/renderer_config.cpp:128-151`)。RPC/headless は XR forced-off driver である
(`src/core/xractivation.hpp:55-70`)ため、「XR graph を一眼 offscreen に流用」は RPC preview の
実装にならない。

より重大なのは、既存 `Renderer::renderLogicalFrame` が単なる graph execute ではない点である。

- 冒頭で DeletionQueue の frame を進める (`src/core/vkcore/renderer.cpp:1169-1172`)。
  DeletionQueue は call ごとに epoch を増やし、in-flight 数を越えた resource を release する
  (`src/core/vkcore/deletionqueue.cpp:45-63`)。通常 logical frame の間に preview call を挟むと
  GPU frame 数と deletion epoch がずれる。
- shared RenderTiming に logical frame/view の timestamp range を登録する
  (`src/core/vkcore/renderer.cpp:574-590`, `src/core/vkcore/rendertiming.cpp:262-296`)。
- 成功後に render-target history、instance history、camera snapshot を必ず advance/commit する
  (`src/core/vkcore/renderer.cpp:1321-1327`)。
- flat/xr の graph 切替自体が全 temporal history を reset する
  (`src/core/vkcore/renderer.cpp:1115-1140`)。

したがって `temporal` feature を無効にしただけで既存 API をもう一回呼ぶ案は、§1-5-2 の
「history を一切変更しない」と両立しない。既存 OffscreenFrameTarget も launch 時の fixed
extent で画像を確保し、constructor が live Camera の screen size を変更する
(`src/core/vkcore/offscreenframetarget.cpp:97-137`)ため、request ごとの一時生成には使えない。

### V22-C6 (逐語)

> PREVIEW0 は flat/xr と別の `preview` graph variant を module graph freeze 前に precompile
> する。preview feature policy は projection jitter、velocity、history read/write、TAA、UI、
> swapchain/XR mirror を除外し、除外不能な authored pass は feature/pass 名入りで起動時に
> reject する。preview executor は request-local render target/layout tracker/frame resources/
> temporal snapshot を持ち、shared RenderTargetContainer history、PolygonInstanceContainer
> previous state、Renderer flat/xr history、DeletionQueue logical epoch、RenderTiming published
> history、EngineTime/frame index を advance/reset しない。GPU timing が必要なら
> `preview_request_id` namespace の非公開 timing とし、通常 logical-frame 集計へ混ぜない。
>
> capture request は width/height、pixel encoding、camera identity/explicit camera、graph
> generation、最大 byte 数を schema で固定し、swapchain/OpenXR へ present しない。v1 は
> `xr_active` 中の render_preview を method+reason 付き `xr_active_unsupported` で reject する。
> XR 中にも許可する版は HMD wait/begin/end、mirror、pacing、GPU timing と非干渉な auxiliary
> submission を別 fixture/実機 gate で証明してから追加する。通常 frame の直前/直後に preview
> を挟み、flat/XR history epoch、previous palette/model、deletion epoch、timing status、次の
> capture bytes が preview 無し control と一致することを gate とする。

WP133 の feature filter/起動時 compile は再利用できるが、XR graph/`selectGraphVariant()` 自体を
再利用してはならない。

## 7. replay/golden/strict gate は request と execution の両方で必要

§1-2 は ReloadGate の `enabled()` を流用せず `can_edit` を独立させ、rpc_driver は許可すると
既に定める (`docs/design_editor_tooling.md:130-135`)。これは正しい。現行 ReloadGate は
`replay/strict/rpc` のどれでも false になるため (`src/core/watch/reloadgate.hpp:13-30`,
`src/core/watch/reloadgate.cpp:26-34`)、preview にも流用できない。

ただし preview は長寿命 ticket と遅延 execution を持つ。現行 RPC では同じ session から
`start_input_replay` が動的に replay reason を立て、`stop_input_replay` が解除できる
(`src/core/communication/rpcserver.cpp:896-929`)。open preview 中に replay を開始した場合や、
enqueue 後 execution 前に gate が閉じた場合を acceptance 時だけの検査では防げない。DLL reload
中は全 RPC を一律 reject する既存境界もある
(`src/core/communication/rpcserver.cpp:682-687`)。

### V22-C7 (逐語)

> `can_edit` と `can_preview` は ReloadGate から独立した editor gate snapshot とし、reason bit
> を replay、golden、strict、reload/scene-transition、preview-lease conflict に分ける。
> rpc_driver と headless は reject reason にしない。ticket preview の open/update/commit、
> eval_preview、render_preview は request acceptance と frame-boundary execution の双方で
> snapshot/epoch を検査し、途中で gate が閉じた request は live mutation なしで method 名、
> reason、gate epoch 付き reject/failed にする。open live preview がある状態では replay開始、
> reload、scene transitionを rejectするか、observer 再開前に E-C1 abort を完了してから遷移
> する。replay/golden/strict では preview handler/evaluation/render graph invocation が 0 回で
> あることを fixture にする。

## 8. scratch snapshot は E-RPC0 query の再利用だけでは足りない

AuthoringSceneDocument は raw envelope を正本として保持し
(`src/core/loader/authoringscenedocument.cpp:20-32`)、`query()` は全 scene/object/component の view
を返す (`src/core/loader/authoringscenedocument.cpp:59-96`)。しかし lossless な full document
export に対応する既存 primitive は per-object query ではなく `encodeSemantic()` である
(`src/core/loader/authoringscenedocument.cpp:99-102`,
`src/core/loader/basicconfig.cpp:460-462`)。

scene envelope (`schema/version/scenes`)、unknown/raw component、非 current scene、declaration order を
query response から再合成させると、同じ正本の第二 serializer を client 側に作ることになる。
また現行 RPC の `load_scene` は既に document にある scene 名を選ぶだけで、snapshot bytes を
process へ取り込まない (`src/core/communication/rpcserver.cpp:973-979`)。

「pending」の意味も固定が必要である。accepted 未 commit ticket、live ticket preview、runtime-only
state を snapshot に混ぜると正本 snapshot でなくなる一方、無言で落とすと agent は見た状態と
異なる scratch を評価する。

### 必須修正

> E-RPC0 に `export_scene_snapshot` を明示し、同一 lock/snapshot から
> `{scene_revision, semantic_scene_bytes, digest, current_scene_id, pending_ticket_ids,
> preview_epoch}` を返す。bytes は AuthoringSceneDocument の deterministic semantic encode を
> 一回だけ使用し、全 envelope/全 scene/全 object/全 raw component/declaration order を含む。
> accepted 未 commit edit、ticket preview、runtime-only component は bytes に含めず、metadata で
> 明示する。caller は pending/preview が 0 でない snapshot を許容するか stable busy error を
> 選べる。AuthoringObjectId は source session 内 identity であり scratch process へ永続移植
> しない。
>
> scratch 側には snapshot bytes/digest を startup 前に ProjectBasicConfig へ与える launch surface
> または RPC import surface を置き、通常 project の scene file を上書きしない。asset/rendering/
> shader は同じ project root を read-only 共有し、scene source だけ snapshot に差し替える。
> 複数 scene、非 current scene、unknown/read-only component、pending edit、preview 中、raw numeric/
> array order の round-trip fixture を置く。採用 edit は source snapshot revision を base とする
> 人 session の通常 edit 一件であり、その間に人が edit していれば global CAS reject する。

これは document branch/merge ではなく、immutable export/import である。専用「昇格 API」は不要
だが、export と scratch ingestion は機構として必要であり、§1-5-3 の「機構新設なし」は撤回する。

## 9. WP 依存グラフは受理済み §6 と再接続できていない

受理済み再レビューは
`ED-AUTH0 + ED-CODEC0 + ECS-MUT0 → E-PROJTX0`、
`E-RPC0 + E-PROJTX0 → E-RPC1/JOURNAL0` と固定した
(`docs/design_reviews/2026-07-18_editor_behavior_v2_rereview_codex.md:192-213`)。
しかし現行 editor 文書の図は依然
`E-RPC0 → ECS-MUT0 + E-RPC1/JOURNAL0` で E-PROJTX0 を欠く
(`docs/design_editor_tooling.md:269-274`)。

新節にも次の hidden dependency がある。

1. `get_scene_revision` の「last transaction の actor/affected ids」は JOURNAL0 が publish する
   data であるのに、watch 全体を先行 E-RPC0 へ統合している
   (`docs/design_editor_tooling.md:183-195`, `docs/design_editor_tooling.md:232-235`)。
2. E-RPC1 行は内容に §1-5-1 ticket preview を含める一方、条件の添付元が
   `§1-2/1-3/1-4` だけで §1-5-1 を落としている
   (`docs/design_editor_tooling.md:276-285`)。
3. PREVIEW0 は prepared projection だけでなく E-RPC0 の typed service/export、E-RPC1 と共通の
   override validator/editor gate、WP133 を基礎にした第三 graph を要する。
   `E-PROJTX0 後`だけでは owner が決まらない。
4. scratch ingestion を「E-RPC0 query 再利用」として WP なしにしたため、実際の process input
   surface と fixture の owner がない。

### 修正後の最小 dependency

```text
ED-AUTH0 + ED-CODEC0                         → E-RPC0-base
ED-AUTH0 + ED-CODEC0 + ECS-MUT0              → E-PROJTX0
E-RPC0-base + E-PROJTX0                       → E-RPC1/JOURNAL0
E-RPC1/JOURNAL0                               → WATCH0(transaction enrichment)
E-RPC0-base + E-PROJTX0 + EDITOR-GATE0 +
OVERRIDE-VALIDATOR0 + WP133(済)                → PREVIEW0
E-RPC0 export + scratch snapshot ingestion    → SNAPSHOT0
```

EDITOR-GATE0/OVERRIDE-VALIDATOR0 を E-RPC1 が所有するなら PREVIEW0 は E-RPC1 完了後にする。PREVIEW0
を E-RPC1 と並行させたいならその共通部を先行小 WP に抽出する。E-RPC0 は base revision/export
までを先に着地させ、last-transaction watch の acceptance は JOURNAL0 後の enrichment に分ける。

## 10. 最低観点への回答

| 指定観点 | 回答 |
|---|---|
| 1. actor 別 revert undo | **穴あり**。field identity では structural overlap を表現できない。V22-C1 が必要 |
| 2. CAS 粒度 | **global CAS を v1 で維持**。偽競合を明示的制約として受け入れ、field/object CAS は後続 |
| 3. eval_preview revert | **両立しない**。live publish/revert をやめ、prepared state を explicit input とする V22-C5 が必要 |
| 4. render_preview | **現行 renderer 呼出しでは不可**。第三 precompiled graph + state/timing/deletion 隔離、XR active v1 reject が必要 |
| 5. replay/golden/strict | 独立 editor gate の方向は正しいが、preview の acceptance/execution 二重検査と open ticket の遷移規範が不足 |
| 6. watch polling | polling は妥当、correctness は CAS。現行 headless-only transport と preview_epoch 不在が blocker |
| 7. scratch snapshot | query 再利用では不足。full semantic export + scratch ingestion + pending policy が必要 |
| 8. WP 分割 | PREVIEW0/E-RPC0 watch/scratch の owner と依存が不足し、本文図も受理済み graph と矛盾 |

## 11. 再レビュー入口

再レビュー時は少なくとも次を一つの差分で提示すること。

1. §1-4/§1-5 本文へ V22-C1〜V22-C7 と snapshot 必須修正を反映した版。
2. operation/write-set matrix: set/add/remove/spawn/destroy/reparent/undo/redo/ticket-preview/
   eval-preview の target、precondition、adapter、revision/preview epoch、conflict result。
3. renderer preview state inventory: shared stateを一項ずつ列挙し「read-only / request-local /
   explicitly suppressed」を示す表。DeletionQueue、RenderTiming、RT history、instance history、
   camera history、layout tracker、EngineTime を必須行にする。
4. full snapshot export/import の JSON schema と、source session CAS までを示す sequence。
5. 受理済み §6 に接続した WP graph と、各逐語条件を落とさない WP 表。

これらが揃うまでは、実装で局所的な rollback や renderer flag を足しても設計上の Reject は
解消しない。
