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
| **評価の番号**(revision) | 描画のフレームごとに必ず 1 増える。戻らない | VMA、animation の書き込み・graph・phase の revision、GPU timing、TAA の jitter、shader の `frame_index`、graph の publish と logical_frame、picking、RenderDoc、XR の mirror、editor の system の `ctx.frameIndex()` |
| **game のフレーム** | game の 1 歩ごとに 1 増える。セッションの始めに 0 に戻る | game の system と behavior の `ctx.frameIndex()`、get_status の `play.game_frame` |
| **game の時計**(`now` / `dt` / `time_set_revision`) | `playing` と `advance_frames` の 1 歩でだけ進む。止まっている間は `dt = 0`。`set_time` はどの状態でも動かす | game のコード、animation の sampling、seq、VAT、graph の入力と時計、audio の graph voice |
| **editor の時計** | 壁時計の `dt`(今の realtime と同じ丸め)と評価の番号 | editor の system(§4.2) |

- `paused` での `advance_frames` の 1 歩の `dt` は `1/fps`(既定 60)に固める
- `edit` / `paused` でも animation・seq・graph・VAT は、評価の番号が進むので、止まった game の時計の時刻で毎フレーム評価し直される。Timeline の scrub で見た目が変わるのはこのため
- `scene_start_time` は、セッションの始めと edit の runtime を作るときに 0 にする。そのため edit では `scene_time` と Timeline の時刻が一致する
- セッションを始めるときは、game の時計を 0、game のフレームを 0 に戻す。終えるときは、始める前の時刻(Timeline の時刻)に戻す
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
- `paused` では game の camera のまま。`set_view_camera {camera: "editor" | "game"}` で切り替えられる(`edit` でも使える。`playing` では `play_state_mismatch`)

## 5. edit の behavior

- `edit` の読み込みでは、behavior の attachment を **眠った形**(handle と seq は振るが、user のコードの instance は作らない)で作る。authoring の behavior の宣言と params は持つ。今ある「DLL が無いときの pending」を土台にする
- 眠った attachment を起こす入口は 2 つあり(`update` の activation と `dispatchEvent` の入口)、どちらも `edit` では塞ぐ
- `edit` での behavior の編集(`set_params`、attach、detach、undo / redo)は、authoring と眠った attachment にだけ効く。params の変更は、保存した params を替えるだけにする。`set_params` の「生きた attachment が無ければ拒否」と、journal の behavior op の handle の照合(`rebindJournalBehaviorTargets`)は、眠った attachment を正しい相手として受け入れる
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
  - headless: その場で同期して `count` 歩進め、終わってから返す
  - 進み終わっても `paused` のまま
- `set_view_camera {camera: "editor" | "game"}`: `edit` / `paused` で使える
- `get_status` に `play` を足す
  - 形: `{state, game_frame, game_time, pending_steps, view_camera, session: bool, recorded_edits: [{transaction_id, actor}]}`
  - 今ある `frame` と `engine_time` は評価の番号と game の時計のままにし、意味を manual に書く
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
- **PM-1b**(engine): `edit` 状態
  - `--start-state` と、両立しない引数の拒否
  - edit の読み込み(instance を持たない behavior、§4.3)
  - editor の system の内部登録と editor の時計
  - camera controller の分割と、見る camera(§4.4)
  - event の止め方、F5 を edit で
  - Stop は「今の authoring から edit の runtime を作り直す」だけ(戻さない)
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

(見出しだけ。Play / Pause / Step のボタン、再生中の表示、Inspector の「残す」、Timeline との関係、戻らない編集の見せ方、見る camera の切り替え)

## 13. v1 に残すもの

- 再生中の Persistence を捨てる(sandbox)
- edit での音の preview
- 再生中の render 構成の編集を Stop で戻す(RD-1 の restore で作れるが、v0 では戻さないと決める)
