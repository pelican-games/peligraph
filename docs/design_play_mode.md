# 再生モード(Play / Pause / Step / Stop)の設計

状態: v2(2026-09-27)。v1 を Claude subagent が実物に当てて検証し、致命 5 件・重要 11 件を反映した。engine の節(§1〜§11)は engine 側が書き、Studio の節(§12)は GUI session が書く。

## 0. 利用者の決定(2026-09-27、GUI session 経由)

- Studio から起動した engine は、止まった状態(編集モード)で起動する。Play を押すと game が動く
- ボタンは Play / Pause / Step の 3 つ
- 再生中の scene の編集は、停止すると元に戻すのが基本。ただし、再生中の変更を「残す」ボタンも欲しい
- 単体の player(Studio を通さない起動)は今どおり、起動した瞬間から動く

## 1. 今の作り(2026-09-27 時点の事実)

- **時間を止める仕組みが無い。** engine にも RPC にも無い。窓ありでは、`EngineTime` が毎フレーム realtime で進む(`appflow/loop.cpp` の `FrameStep::advance` → `enginetime.cpp` の `advance`)。同じ所で `frameIndex` を VMA の allocator の frame index に渡している(`vkcore/core.cpp` の `setCurrentFrameIndex`)
- **`EngineTime::frameIndex` は「評価の番号」を兼ねていて、単調に増える前提。** 次のものが、同じ値や 0 を拒否する。
  - animation の姿勢の書き込み(`renderer/polygoninstancecontainer.cpp`)
  - animation graph(`userpublic/animation/animgraph.cpp`)
  - animation の phase(`animation/animationservice.cpp`)
- ほかに frameIndex を読むもの:
  - GPU timing(logical frame ごとに 1 回だけ測る)
  - TAA の jitter
  - shader の `frame_index` uniform
  - graph の publish と graph node の logical_frame
  - picking、RenderDoc、XR の mirror
- **game が読む時間は `EngineTime` の `now` / `dt`。** 読むのは次のもの。
  - game のコード(`ctx.time()` など)
  - animation の sampling
  - seq と VAT
  - graph の入力(`time`、`delta_time`、`scene_time = now - scene_start_time`)
  - graph の時計は `render()` の中で進む(`loop.cpp:667`)
- **system の登録表に「editor 用 / game 用」の区別が無い。** engine 内蔵の system は次の 4 つ。
  - behavior(order 50)
  - camera controller(10000、free camera を含む)
  - modal transform(10010)
  - physics trigger(INT_MAX)

  DLL の system は、公開 ABI の `GameSystemRegistration`(`pelican_register_system_v2`、値渡し)で登録される。system の instance は static で、DLL を reload しない限り初期化されない
- **scene の読み込みでは、game のコードが次の順に走る。**
  - game の component の JSON loader と `init()`
  - behavior の params の正規化
  - behavior の constructor と `onInit`(activate 段。失敗すると読み込みごと戻る)
- **behavior の編集は runtime の attachment に依存している。**
  - `set_params` は、生きた attachment が無いと拒否する
  - undo / redo は、runtime の attachment と照合してから実行する
  - editor で behavior を付ける(attach)と、prepare 段で constructor が走る
- **ID を保ったまま丸ごと戻す口が無い。**
  - authoring は 1 つの文書で、全 scene を含む。`ctx.loadScene` も、この文書を借りるだけ
  - `import_scene_snapshot` は ID を必ず振り直す
  - 文書の `SceneRevision` は単調増加を要求する
  - `restoreObject` は、`next_authoring_object_id` 以上の ID を拒否する
- **editor の履歴(`EditorEditCoordinator`)は、書き戻しの口を持たない。** 持っているのは journal・last_writers・undo/redo の stack・preview の epoch。undo / redo の鍵は transaction_id と actor で、undo も redo も新しい ticket と txn を発行する
- **audio に全体の一時停止と停止が無い。**
  - backend は device / offline / null の 3 種
  - native の graph voice は、時計が止まっても鳴り、drive が位置を送り続ける
- **`step_frame` は窓ありでも登録されている。** ただし窓ありでは、描画の前に 1 フレームを入れ子で走らせる。Studio も窓ありの test も使っていない
- **Studio が engine に渡す引数には `--free-camera` が無い。** `--free-camera` の camera は active に固定される

## 2. 実行の状態

| 状態 | game の時計 | game のコード | 描画・RPC・編集・editor の system |
|---|---|---|---|
| `edit` | 止まる(Timeline の `set_time` で動かせる) | behavior・game system・game の event・DLL の animation phase は走らない(§4.3) | 走る |
| `playing` | 進む | 走る | 走る |
| `paused` | 止まる(`advance_frames` で 1 歩ずつ進む) | 止まる。behavior と game の状態は残る | 走る |

- 起動時の状態は `--start-state edit|playing` で決める(既定は `playing`)。Studio は `--start-state edit` を渡す
- `--start-state edit` は次の引数と一緒に使えず、既存の `offline.mode_conflict@1` と同じ形で断る。
  - `--replay`、`--record-input`、`--bake-camera-output`、`--start-time`
  - `--rpc` の無い `--headless`
- 移り方:
  - `edit → playing` / `edit → paused`: セッションを始める(§6)。`paused` に移ると、`onInit` まで走り、game のフレーム 0 で止まる。Studio で edit 中に Step を押したときは、これに続けて `advance_frames {count: 1}` を送る
  - `playing ↔ paused`: game の時計・game のコード・audio を止める、または再開する。game の状態はそのまま。再開のときは realtime の `last_tick` を今に戻す(再開の最初のフレームで dt が跳ねないように)
  - `playing / paused → edit`: セッションを終える(§6、§7)
- 同じ状態へ移る指示は、何もせずに成功を返す

## 3. 番号と時計を分ける

| 名前 | 増え方 | 誰が読むか |
|---|---|---|
| **評価の番号**(revision) | 描画のフレームごとに必ず 1 増える。戻らない | VMA、animation の書き込み・graph・phase の revision、GPU timing、TAA の jitter、shader の `frame_index`、graph の publish と logical_frame、picking、RenderDoc、XR の mirror、`ctx.frameIndex()`(editor の system も game のコードも。下の注)、get_status の `frame` |
| **game のフレーム** | game の 1 歩ごとに 1 増える。セッションの始めに 0 に戻る | get_status の `play.game_frame` と、`advance_frames` の返り値の `until_game_frame` だけ |
| **game の時計**(`now` / `dt` / `time_set_revision`) | `playing` と `advance_frames` の 1 歩でだけ進む。止まっている間は `dt = 0`。`set_time` はどの状態でも動かす | game のコード、animation の sampling、seq、VAT、graph の入力と時計、audio の graph voice |
| **editor の時計** | そのフレーム自身の `dt`(realtime では壁時計の差分を今と同じく 0.1 秒で丸めたもの、fixed_step では固定の `dt` = `1/fps`)と評価の番号 | editor の system(§4.2) |

- **`GameContext::frameIndex()` は game のコードにも評価の番号を返す**(PM-1a で決めた。`gamecontext.cpp`)。game のコードが animgraph の `prepareTick` に渡す revision は `ctx.frameIndex()` から取るので、engine が phase を走らせる revision(評価の番号)と一致していないと断られるため。そのため pause の後、game のコードから見える `ctx.frameIndex()` は止まっていたフレームの数だけ飛ぶ。`ctx.time()` は game の時計なので飛ばない。0 から数え直す game のフレームを外から読めるのは、get_status の `play.game_frame` と `advance_frames` の `until_game_frame` だけ

- `paused` での `advance_frames` の 1 歩の `dt` は `1/fps`(既定 60)に固める
- `edit` / `paused` でも animation・seq・graph・VAT は、評価の番号が進むので、止まった game の時計の時刻で毎フレーム評価し直される。Timeline の scrub で見た目が変わるのはこのため
- `scene_start_time` は、セッションの始めと edit の runtime を作るときに 0 にする。そのため edit では `scene_time` と Timeline の時刻が一致する
- セッションを始めるときは、game の時計を 0、game のフレームを 0 に戻す。終えるときは、始める前の時刻(Timeline の時刻)に戻す
  - PM-1b の実装(`EngineTime::endGameSession`): 終えるときに戻すのは時刻だけで、**game のフレームは 0 に戻さない**。`edit` では game のフレームは進まないので、`get_status` の `play.game_frame` は前のセッションの最後の値のまま残り、次のセッションの始めに 0 になる。セッションを経ずに `playing` で起動した engine を `edit` に移すと、時刻は 0 に戻る
- fixed_step(headless、input replay)の決定性は変えない。`--start-state` を省いた headless は、今と同じ番号の進み方になる(評価の番号と game のフレームは同じ値で進む)

## 4. 1 フレームの中で止めるもの・止めないもの

### 4.1 止めるもの(`edit` / `paused`。`paused` の `advance_frames` の 1 歩では走る)

- game の event の凍結と配送(`freeze_events` と `deliver_events` は一緒に止める。片方だけ止めると event の registerer の assert に当たる)
- `runtime_spawner.applyBoundary`
- `update_game` の中の次のもの
  - game の system(DLL の system、behavior、physics trigger)
  - DLL が登録した animation の phase callback(owner が engine 以外のもの)
  - `applyPendingLoad`
- game の UI への入力の振り分け(`edit` では game の UI に入力を渡さない。`paused` では渡しても event を積まない)
- 入力の記録・replay の消費と camera bake。これらは `--start-state edit` と一緒に使えないので、実際には `playing` でだけ走る

### 4.2 止めないもの

- 窓と入力の取り込み、RPC(`processFrameBoundary`)
- 編集の反映(`invokeEditorCommitQueueHook`)
- shader の reload と `audio.update`
- `ecs.update` の engine 側の system(transform の伝播、camera、model view など)
- engine 側の animation の sampling・graph・phase(game の時計の時刻、評価の番号で)
- seq の更新
- 描画(picking と gizmo を含む)
- **editor の system**
  - editor の system は engine の内部の登録にし、公開 SDK の `GameSystemRegistration` は変えない(ABI を守るため)
  - 対象は modal transform と、camera controller から分けた editor camera の制御の 2 つ
  - editor の system を呼ぶ間だけ、`GameContext` の時間の出どころを editor の時計に切り替える(`GameContext` の layout は変えない)

### 4.3 edit で走らせる game のコードと走らせないもの

- **走らせる**(scene のデータを作るため): game の component の JSON loader と `init()`、behavior の params の正規化
- **走らせない**: behavior の constructor・`onInit`・`onUpdate`・event、game の system、DLL の animation phase、spawn

### 4.4 見る camera

- `--start-state edit` のとき、engine は editor camera を用意する(今の `--free-camera` と同じ仕組み)
- `edit` では editor camera で見る
- `playing` では game の active camera で見る。editor camera の active 固定を外す
  - `--start-state edit` で起動したときに限り、editor の action set の overlay(`free_camera`、`editor_transform`)を、`playing` の間は action の stack から外す。これで同じ key が game に届く
  - `edit` / `paused` に戻ると、overlay を stack に積み直す
  - 同じ理由で、editor の system は `playing` の間は何もしない
  - `--start-state` を省いた起動(今の Studio と単体の player)では、今どおり overlay を積んだまま動く。PM-1a / PM-3a の間の Studio の挙動は変えない
- `paused` では game の camera のまま。`set_view_camera {camera: "editor" | "game"}` で切り替えられる(`edit` でも使える。`playing` では `play_state_mismatch`)
- PM-1b の実装(`src/core/renderer/camera.hpp` の `Camera::ViewCamera`、`src/core/appflow/playmode.cpp`)
  - editor camera は `--free-camera` の実行時 camera そのもの。`--start-state edit` で `--free-camera` が無ければ、既定の preset(`blender`)の free camera とその input overlay を足す。`--editor-transform` は足さない(Studio が別に渡す)
  - 見る camera は `editor` / `game` の 2 つの view で、見せていない方の view は位置・向き・投影・active camera ごと取っておき(parked)、戻すとそのまま出てくる。view は engine のもので scene のものではないので、scene の読み込みで変わらない。editor camera が無い起動では、view は常に `game`
  - 状態が移るたびに view を切り替えるのは `--start-state edit` で起動したときだけ(`edit` → editor、`playing` / `paused` → game)。既定の起動では `set_play_state` は view を変えない
  - **editor の view の間に game の camera を選んだとき**(game のコードの `ctx.render().setCamera(name)` と RPC の `set_camera`。どちらも `Camera::setActiveCamera`): scene の camera の名前なら、先に `game` の view へ切り替え(editor camera の姿勢は取っておく)、それから選ぶ。つまり選択は game の view に効く。editor camera の名前を指したときは `editor` の view に切り替える
  - camera controller は 2 つの system に分けた。engine 内部の editor の system `BuiltinCameraControllerSystem`(order 10000)は、`--start-state edit` の起動では editor camera だけを、それが見えている(active の)ときに動かす。game の system `BuiltinGameCameraControllerSystem`(order 10000、印なし)は、`--start-state edit` の起動でだけ scene の制御付き camera を動かす。既定の起動では、PM-1a と同じく editor の system が全部の制御付き camera を動かし、game の system は何もしない。controller の状態は scene の読み込みと view の切り替えをまたいで残る
  - `--start-state edit` の起動で `playing` の間は、`updateRegisteredGameSystems` を `GameSystemScope::game_only` で呼び、editor の system を走らせない

## 5. edit の behavior

- `edit` の読み込みでは、behavior の attachment を **眠った形**(handle と seq は振るが、user のコードの instance は作らない)で作る。authoring の behavior の宣言と params は持つ
- PM-1b の実装: 眠りは「DLL が無いときの pending」を使わず、**arena 全体の flag**(`BehaviorAttachmentArena::setDormant` / `isDormant`、`src/core/gamelogic/behaviorarena.hpp`)にした。眠った attachment は pending ではない(`pending` は「DLL が無い」の意味のまま)ので、editor は編集の相手としてそのまま扱う
  - 眠っている間に作る attachment(scene の読み込み、editor の attach、scene の reconciliation、spawn)は、handle・attachment_seq・type・owner・正規化した params を持ち、instance を作らない。constructor・`onInit`・`onUpdate`・event・spawn はどれも走らない
  - 眠りに入るとき・出るときは、game のコードが積んだ object の遅延ミューテーションを捨てる(積んだ run のものなので)
  - 眠りを解いただけでは何も起きない。次の scene の読み込み(セッションの始めの作り直し)が instance を作り、`onInit` を走らせる
  - `edit` 中の game logic の reload(F5 / `reload_game_logic`)と `load_scene` は、同じ読み込みの経路を通るので、眠ったまま作り直す
  - `edit` で作る runtime の `scene_start_time` は 0(`src/core/loader/scene.cpp` の `LoaderStateParticipant::prepare`)
- 眠った attachment を起こす入口は 2 つあり(`update` の activation と `dispatchEvent` の入口)、どちらも `edit` では塞ぐ(PM-1b: どちらも眠っている間は入口で何もせずに戻る)
- `edit` での behavior の編集(`set_params`、attach、detach、undo / redo)は、authoring と眠った attachment にだけ効く。params の変更は、保存した params を替えるだけにする。`set_params` の「生きた attachment が無ければ拒否」と、journal の behavior op の handle の照合(`rebindJournalBehaviorTargets`)は、眠った attachment を正しい相手として受け入れる(PM-1b: `set_params` は params の差し替えの準備を飛ばして保存した params だけを替え、attach は constructor を呼ばない。detach と undo / redo の経路には手を入れていない。test は set_params・attach・その undo / redo を見ている)
- セッションを始めると、instance の無い attachment から instance を作り、`onInit` を走らせる

## 6. 再生のセッション

### 6.1 始める(`edit → playing / paused`)

1. scene の編集の ticket か preview が未完了なら、`play_busy` で断る(`export_scene_snapshot` と同じ条件。render config と procedural asset の ticket は見ない。これらは戻さないので)
2. 次のものを保存する
   - authoring の文書(ID を含む)と `current_scene_id`
   - editor の履歴(journal、last_writers、undo / redo の stack、preview の epoch)
   - game の時計の時刻
3. game の event の pending を捨てる。game の時計とフレームを 0 に戻し、`scene_start_time = 0` にする
4. game logic の DLL が読み込まれていれば、reload する。こうして game の system の static な状態を毎回まっさらにする。そのあと、今の authoring から runtime を作り直し、behavior を作って `onInit` を走らせる(`load_scene` と同じ、ID を保つ経路)

### 6.2 再生中

- 編集は今どおり、authoring と runtime の両方に効き、editor の履歴にも積む
- セッションは、再生中に commit された編集の transaction_id を順に記録する。undo / redo は `source_transaction_id` をたどって打ち消し合わせる。undo した編集は記録から消え、redo で戻る
- 次の RPC は `play_session_active` で断る
  - `save_scene`
  - `load_scene`
  - `import_scene_snapshot`
- game のコードが `ctx.loadScene` で scene を替えるのは許す。そのとき editor の今の scene も替わる。その後の編集は、今と同じく gate の epoch が進むので `gate_closed` で断られる(仕様として manual に書く)

### 6.3 終える(`→ edit`)

1. scene の編集の ticket か preview が未完了なら、`play_busy` で断る
2. runtime を片付ける(behavior の `onDestroy`。spawn されたものは消える)。audio を全部止め、game の event の pending を捨てる
3. authoring の **中身** を保存時の形に戻す
   - `SceneRevision` は今の値 +1 で出し直す(巻き戻さない)
   - `next_authoring_object_id` は、保存時と今の値の大きい方にする
   - `current_scene_id` と `active_authority` は保存時の値に戻し、gate の epoch は進める
   - この形の入口を authoring の文書に足す。今ある私有の `publishPreparedSceneState` は、番号を巻き戻すので使わない
4. editor の履歴を保存時の形に戻す。対象は journal・last_writers・undo / redo の stack。ticket・txn・preview の epoch の番号は戻さない
   - last_writers まで戻すので、再生前の編集の undo は Stop の後もそのまま効く
   - `EditorEditCoordinator` に、engine 内部の snapshot / restore を足す
5. `keep` に挙がった編集を当て直す(§7)
6. game の時計を、始める前の時刻に戻し、`scene_start_time = 0` にする。`edit` の runtime を作り直す(behavior は instance を持たない形)

### 6.4 戻す範囲

- 戻すのは **scene の authoring だけ**
- ファイルへ直接書く編集は、再生中もそのまま書かれ、Stop で戻らない。manual に仕様として書く
  - render の構成: Frame Plan / Pass Editor の編集、rendering 節、preset、graph 文書、authored pass
  - material・shader・code・asset
  - game が書いた Persistence(`saveData`)
- 再生中の `runtime_only_changes` は、runtime を作り直すので消える

### 6.5 game logic の reload(F5 / `reload_game_logic`)

- どの状態でも許す
- `edit` では、DLL を差し替えたあと、`edit` の読み込み(§5)で作り直す
- `playing` / `paused` では今どおり、今の authoring(再生中の編集を含む)から作り直して `onInit` を走らせる。状態は保ち、セッションの保存もそのまま

## 7. 残す(keep)

- 形: `set_play_state {state: "edit", keep: [<transaction_id>...] | "all"}`。`keep` を省くと何も残さない
- 当て直すのは、セッションが記録した編集(§6.2)のうち、挙がったものだけ。記録の順に、戻した authoring へ当て直す
- 当て直すと ID が同じになる。理由は次の 2 つ
  - 再生中に作った object の記録は、採番済みの ID を含む closure で当て直す
  - `next_authoring_object_id` は §6.3 の 3 で大きい方になっている
- 当て直しは、今の revision を base にし、behavior の handle の rebind を含む形で行う。今この形の実行は test 用の `executeJournalForVerification` にしか無いので、production の入口を作る
- 当て直せない編集は飛ばし、結果の `not_kept: [{transaction_id, code}]` で返す(例: 作る編集を残さずに、その object を触る編集だけを残した)。Stop 自体は成功する
- 当て直した編集は、editor の履歴の 1 項目(「再生から残した編集」)になり、undo でまとめて外せる
- 挙がった transaction_id がセッションの記録に無ければ、`play_keep_unknown_transaction` で Stop ごと断る(何も変えない)
- game が runtime で変えた値(behavior が動かした位置など)を残したいとき
  - Studio が再生中に runtime の値を読み、その値を書く編集を送る。それを keep に挙げる
  - engine に専用の口は足さない

## 8. RPC

- `set_play_state {state: "edit" | "playing" | "paused", keep?}`
  - 返り値: `{state, previous, not_kept?: [...]}`
- `advance_frames {count}`(count は 1〜600)
  - `paused` でだけ許す。`edit` / `playing` では `play_state_mismatch`
  - 窓あり: 次のフレームの境目から、1 フレームに 1 歩ずつ、game を `count` 回進める(入れ子にしない)。すぐに `{scheduled: count, until_game_frame}` を返す
  - headless: その場で同期して `count` 歩進め、終わってから返す。返り値は `{scheduled, until_game_frame, frame, t, edit_results}`。`until_game_frame` は歩き終えた game のフレーム、`frame` は評価の番号、`t` は game の時計、`edit_results` は `step_frame` と同じ(PM-1a の `rpcserver.cpp`)
  - 進み終わっても `paused` のまま
- PM-1b の `set_play_state`(`src/core/communication/rpcserver.cpp`、engine 側は `src/core/appflow/playmode.cpp` の `changePlayState`)
  - `keep` を書くと `-32602`(PM-2 まで)。それ以外の鍵も `-32602`
  - `edit` との間を移るときは、今の scene を `load_scene` と同じ経路(`SceneLoadRoot::rpc`、preview は `scene_transition` で中止)で作り直す。作り直しが投げると、状態・game の時計・behavior の眠りを元に戻し、error を返す
  - 既定の起動(`--start-state` を省いた)でも `edit` へ移れる。そのとき view と editor の overlay は変えない(§4.4)
- `set_view_camera {camera: "editor" | "game"}`: `edit` / `paused` で使える
  - 返り値(PM-1b): `{camera, previous}`(どちらも `"editor"` / `"game"`)。同じ view への指示は何も変えずに成功する
  - `playing` では `play_state_mismatch`(`data` に `state` と `required: ["edit", "paused"]`)。editor camera の無い起動で `editor` を頼むと `view_camera_unavailable`(`data` に `camera`)。鍵が `camera` だけでない・値が 2 つ以外は `-32602`
- `get_status` に `play` を足す
  - 形: `{state, game_frame, game_time, pending_steps, view_camera, session: bool, recorded_edits: [{transaction_id, actor}]}`
  - 今ある `frame` と `engine_time` は評価の番号と game の時計のままにし、意味を manual に書く
  - PM-1a の時点の `play` は `{state, game_frame, game_time, pending_steps}` の 4 つ(`view_camera` / `session` / `recorded_edits` は PM-1b / PM-2)。PM-1b で `view_camera`(`"editor"` / `"game"`)を足して 5 つ
  - 一時停止しているかは `play.state` だけで示す。`get_status` の `audio` の形は変えない(全体の一時停止は `audio` に出ない)
- `step_frame`
  - headless 専用にする。窓ありでは `step_frame_windowed` で断り、message で `advance_frames` を示す
  - headless では、描画のフレームを 1 つ進める。game が進むのは `playing` のときだけ
- `set_time` / `render_frame`: 状態を問わず今の意味のまま。`edit` / `paused` では時計が進まないので、その時刻にとどまる

## 9. audio

- `Audio` に次の 2 つを足し、backend の仮想関数も足す
  - `setAllPaused(bool)`: 全体の一時停止
  - `stopAll()`: 全体の停止
- backend ごとの動き
  - device(miniaudio): engine の stop / start
  - offline: pause の間、`mixOffline` は無音を返し、再生位置を進めない
  - null: 何もしない
- `edit` / `paused` の間は全体を一時停止し、`playing` で再開する。Stop で全部止める
- native の graph voice は、全体の一時停止で止まる。再開したときの位置合わせは、今の drive の許容幅に任せる
- `edit` で Timeline を scrub しても音は鳴らさない(v0)

## 10. 名前付きの拒否

| code | いつ |
|---|---|
| `play_busy` | セッションを始める・終えるときに、scene の編集の ticket か preview が未完了 |
| `play_session_active` | 再生中(`playing` / `paused`)の `save_scene` / `load_scene` / `import_scene_snapshot` |
| `play_state_mismatch` | `advance_frames` を `paused` 以外で使った。`set_view_camera` を `playing` で使った |
| `view_camera_unavailable` | editor camera の無い起動(`--start-state edit` も `--free-camera` も無い)で `set_view_camera {camera: "editor"}` を使った(PM-1b で追加。`data` に `camera`) |
| `play_keep_unknown_transaction` | `keep` の transaction_id がセッションの記録に無い |
| `step_frame_windowed` | 窓ありで `step_frame` を使った |
| `offline.mode_conflict@1`(既存) | `--start-state edit` と両立しない引数 |
| (引数の誤り) | `--start-state` の値の誤り、`advance_frames` の `count` が範囲外 |

## 11. WP の切り方

- **PM-1a**(engine): 今の Studio で、そのまま Pause / Step が効く
  - 番号と時計の分離(§3)
  - `playing ↔ paused`
  - `advance_frames`
  - get_status の `play`
  - audio の全体の一時停止
  - `step_frame` を窓ありで断る
  - 再開のときの dt
  - 既定の起動(`playing`)は、今と同じ見た目と決定性のまま
  - **着地 `c654e65c`**(2026-09-27)。この節の計画との差: editor の system の印は PM-1a で入れた(engine 内部の `markEditorGameSystem`。camera controller は分けずに丸ごと、modal transform と合わせて 2 つ)/ audio は `setAllPaused` だけで、`stopAll` は PM-2 のまま / `ctx.frameIndex()` は game のコードにも評価の番号(§3 の注)
- **PM-1b**(engine): `edit` 状態
  - `--start-state` と、両立しない引数の拒否
  - edit の読み込み(instance を持たない behavior、§4.3)
  - editor の system の内部登録と editor の時計
  - camera controller の分割と、見る camera(§4.4)
  - event の止め方、F5 を edit で
  - Stop は「今の authoring から edit の runtime を作り直す」だけ(戻さない)
  - **着地 `17938ee3`**(2026-09-27)。この節の計画との差
    - 眠りは pending を土台にせず、arena の flag にした(§5)
    - editor の system の内部登録と editor の時計は PM-1a で入っていた。PM-1b で入れたのは camera controller の分割(game の system `BuiltinGameCameraControllerSystem` を足した)と、`--start-state edit` の起動で `playing` の間は editor の system を走らせない `GameSystemScope::game_only`(§4.4)
    - event の止め方: `edit` の間は PM-1a の「1 歩でないフレーム」と同じく凍結と配送を止め、`edit` との間を移るたびに pending の event を捨てる。game の UI には `edit` の間は入力を渡さない
    - F5 を edit で: 専用の分岐は足していない。reload が通る読み込みの経路が眠りの flag を見る(§5)
    - Stop で `game_frame` を 0 に戻さない(§3)
    - `set_play_state` の `keep` は `-32602`(§8)。`play_busy` は無く、未完了の preview は `load_scene` と同じく中止する(PM-2 で `play_busy` にする)
    - 残した穴: `edit` 中の `start_input_replay` / `start_input_record` を断っていない(PM-2 で断る)
- **PM-2**(engine): セッション
  - 保存と戻し(中身は戻し、番号は進める)
  - 履歴の保存と戻し
  - keep(transaction_id)
  - Play の始めに DLL を reload
  - `play_busy` / `play_session_active` / `play_keep_unknown_transaction`
  - audio の全体の停止
- **PM-3**(Studio、GUI session): ボタン、表示、keep、`--start-state edit` を渡す
  - Pause / Step のボタンは PM-1a の後に出してよい
  - `--start-state edit` と Stop は、PM-2 の着地を待つ。PM-1b だけで出すと、「停止で戻す」という利用者の決定と逆になるため
- `load_scene` と `step_frame` の拒否には、今まで無かった `data.code` を付けることになる(rpcserver の他の拒否と同じ形)
- 受け入れは、主に headless の `--start-state edit --rpc` で見る。窓の test は PM-3 の scenario に任せる

## 12. Studio(GUI session が書く)

### 12.1 ボタンとキー

- viewport の上の帯(今の Move / Rotate / Scale の並び)に 3 つのボタンを置く
  - `▶ Play`: `edit` では「セッションを始めて `playing`」。`paused` では「`playing` に戻す」。`playing` の間は `■ Stop` に変わる
  - `❚❚ Pause`: `playing` ↔ `paused` の切り替え(押した状態で `paused`)。`edit` では押せない
  - `▶| Step`: `paused` では `advance_frames {count: 1}`。`edit` では `set_play_state {state: "paused"}` を送ってから `advance_frames {count: 1}`(§2。onInit まで走り、game のフレーム 1 で止まる)。`playing` では押せない
- `■ Stop`(`playing` / `paused`): `set_play_state {state: "edit", keep}`。`keep` は §12.3 の一覧で印を付けたもの
- キー(Unity と同じ): Ctrl+P = Play / Stop、Ctrl+Shift+P = Pause、Ctrl+Alt+P = Step。テキスト欄に focus があるときは効かない
- 送信中(返事待ち)の間は 3 つとも押せない。`play_busy` で断られたら、status に「編集の送信か preview が終わってから、もう一度押してください」相当の 1 行と、終わるのを待って自動でもう一度送るかを選ぶボタン

### 12.2 今の状態を見せる

- 状態は `get_status` の `play`(`state`、`game_frame`、`game_time`、`pending_steps`、`view_camera`、`session`、`recorded_edits`)から読む。Studio が今 `get_status` を読んでいる周期に乗せる(新しい poll を増やさない)
- viewport の枠と上の帯の色で分ける: `edit` は今の色、`playing` は青系、`paused` は黄系(色は theme の token で。明暗どちらでも読める)
- status bar: `Edit` / `Playing 00:12.4 · frame 734` / `Paused · frame 734`(`pending_steps` があれば `Stepping (3 left)`)
- 起動: Studio は engine に `--start-state edit` を渡す(EW-1 の `--embedded` と一緒に)。起動の直後は `Edit`

### 12.3 再生中の編集と「残す」

- 再生中も Inspector・Outliner・Graph tab・Frame Plan などの編集はできる(engine は断らない)
- **scene の編集**(Inspector・Outliner・behavior・graph の装着など、scene の authoring に効くもの)は Stop で戻る。編集の行(notice)の後ろに `— reverts on Stop` を添え、`Keep` のリンクを出す(押すと §12.3 の一覧でその行に印)
- **Play changes**(再生中の編集の一覧。viewport の上の帯のボタンで開く小さな panel)
  - 行 = `play.recorded_edits` の 1 件。行の文は Studio が送った編集ならその履歴の項目名(`Move Cube`、`Set Light.intensity` など。transaction_id から引く)、他の client の編集なら `actor` と transaction_id
  - 行ごとの check(既定は外れている = 戻す)、`Keep all` / `Keep none`
  - undo で消えた編集は一覧から消え、redo で戻る(engine の `recorded_edits` に従う)
- **game が動かした値を残す**: Inspector で object を選んでいるときに `Take from game` ボタン(再生中だけ)。その object の Transform の今の runtime の値を読み、同じ値を書く編集として送り、その行に印を付ける(engine の §7 の「Studio が値を読んで書く編集を送る」形)。v0 は Transform だけ
- **Stop のあと**: status に `Stopped: 5 changes reverted, 2 kept` 。`not_kept` があれば、その件数と行ごとの理由(code を読める文に)を Play changes の panel に残す
- Stop のあとは scene の revision が進むので、Inspector・Outliner・Graph tab の装着は今の外部変更の経路(Q-16 の watch)で読み直す。Studio の scene の Undo の履歴は engine の履歴(戻した形)に合わせて読み直す

### 12.4 戻らない編集の見せ方

- §6.4 のとおり、再生中でも次の編集はファイルに書かれ、Stop で戻らない: render の構成(Frame Plan・Pass Editor・Render Features・Graph tab の graph 文書・Decompose・Replace with loop version)、material・shader・code・asset
- 再生中にこれらの panel で編集を送るときは、その panel の上に 1 行の帯「Changes here are written to files and stay after Stop.」を出す(再生中の間だけ。閉じても次の再生で出る)。render の編集の Undo(RU-1)は今どおり効く
- Save(scene)・Load scene・Import snapshot は再生中は押せない(tooltip に `play_session_active` の理由)。File > Save Scene も同じ

### 12.5 見る camera

- `edit`: editor camera(今の viewport の操作のまま)
- `playing`: game の active camera。viewport での editor camera の操作(WASD など)は game に渡る入力と重ならないよう、`playing` の間は Studio からは送らない
- `paused`: 帯に `View: Game | Editor` の切り替え(`set_view_camera`)。既定は Game。Editor に切り替えると、止まった game の世界を editor camera で見て回れる
- Stop で editor camera に戻る

### 12.6 他の機能との関係

- **Graph tab の Timeline**: `edit` / `paused` では scrub した時刻にとどまる(§8)。`playing` では scrub を押せない(tooltip: Pause してから)
- **game logic の reload**: 帯のメニューに `Reload game logic`(`reload_game_logic`)を足す。どの状態でも押せる(§6.5)。`playing` / `paused` では「再生中の状態は保ち、今の authoring から作り直す」を tooltip に
- **Restart Engine / project の切替 / Studio を閉じる**: 再生中なら確認(「再生を終えます。再生中の scene の変更は失われます(Keep の印は使われません)」)
- **Outliner**: 再生中に spawn された runtime の object は、`(runtime)` の印を付けて灰色で出す(編集できない)。Stop で消える

### 12.7 PM-3 の分け方

- **PM-3a**(PM-1a の後): 今の既定の起動(`playing`)のまま、`Pause` / `Step` / Resume のボタンと状態の表示(§12.2 の枠の色と status)。Timeline の scrub を `playing` で押せなくする
  - ✅着地(1eaf28a6)。穴: Studio には周期の `get_status` の読みが無い(Q-2)ので、既存の読み・状態を変えた直後・Step の追従に乗せた。`playing` の間の `frame N` は最後に読んだ値で止まって見える(`paused` と `Stepping` は正確)
- **PM-3b**(PM-2 の後): `--start-state edit`、Play / Stop、Play changes の panel と Keep、Take from game、戻らない編集の帯、Save などを押せなくする、見る camera の切り替え、Reload game logic、再起動などの確認、Outliner の runtime の印

### 12.8 受け入れ(PM-3 の GPU シナリオ)

- PM-3a: behavior で動く object を持つ fixture で、Pause で位置が止まり(2 回の読みで同じ)、Step で 1 歩だけ変わり、Play で動き出す。状態の表示が `Paused · frame N` になる
- PM-3b:
  - 起動が `Edit`(object が動かない)。Play で動き、Stop で起動時の位置に戻る
  - 再生中の Inspector の編集(Light の強さなど)が Stop で戻る。Keep の印を付けた編集は残る
  - `Take from game` で、game が動かした位置が Stop の後も残る
  - 再生中に Save が押せない
  - 再生中の Pass Editor の編集が Stop の後も残り、帯が出ていた
  - `paused` で `View: Editor` にすると editor camera で見える(screenshot の違い)

## 13. v1 に残すもの

- 再生中の Persistence を捨てる(sandbox)
- edit での音の preview
- 再生中の render 構成の編集を Stop で戻す(RD-1 の restore で作れるが、v0 では戻さないと決める)
