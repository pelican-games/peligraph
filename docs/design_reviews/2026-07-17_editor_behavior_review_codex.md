# 編集系 RPC / エンジン内ツール v1 + object behavior v1 敵対レビュー

日付: 2026-07-18  
対象 commit: `33e5eca73d50bb4c6052ed5b7d0bdd6532ef94c2`  
対象: `docs/design_editor_tooling.md` v1、`docs/design_object_behaviors.md` v1  
方法: 文書と現行 C++ の静的照合。依頼どおりビルド・実行は行っていない。

## 0. 結論

| 文書 | 判定 | 結論 |
|---|---|---|
| `design_editor_tooling.md` v1 | **Reject** | E-RPC を正本にして ImGui/Qt/agent を同じ意味論へ集約する方向は正しい。しかし、(1) WP62 後の登録に逆方向 serializer は存在しない、(2) scene object と runtime entity は 1:1 でなく、主要 component の多くは汎用 ComponentInfo 経路にない、(3) 同期 stdio RPC で「次フレーム適用結果をその edit 応答で返す」契約が閉じない、(4) `{op,target,before,after}` だけでは destroy/reparent/複合編集の undo 単位を定義できない、(5) SAVE0 が read-only component を警告付きで落として正本ファイルを上書きし得る、という blocker がある。現 WP 粒度では着手不可。根拠: `docs/design_editor_tooling.md:36-67`, `docs/design_editor_tooling.md:90-107`, `src/core/ecs/componentinfo.hpp:20-48`, `src/core/userpublic/serialize/jsonarchive.hpp:30-47`, `src/core/loader/scene.cpp:98-170`。 |
| `design_object_behaviors.md` v1 | **Reject** | object 単位 behavior と一個の BehaviorSystem という方向は妥当。しかし、(1) 同一 entity の同一 component 重複を ECS が拒否するため提示 JSON の「複数 behavior」を通常 component としてロードできない、(2) params 省略/default/bool/nested `params` を扱う既存反射機構がない、(3) `EventView` 購読は現行 E1 の typed dispatch と接続していない、(4) entity publish・親配線より前の `onInit`、callback 中の spawn/destroy、runtime spawn 順が未規範、(5) G2 の候補 DLL 検証ロード中に「登録が揃った時点で onInit」すると live runtime へ副作用を起こし得る、(6) WP90 は ECS/scene 全再構築なので「状態は params/component に置けば残る」が事実と反する。BEH0/1 の境界を引き直すまで着手不可。根拠: `docs/design_object_behaviors.md:49-93`, `src/core/userpublic/details/ecs/coretemplate.cpp:93-112`, `src/core/gamelogic/gamelogicreload.cpp:205-263`, `src/core/appflow/teardown.cpp:31-42`。 |

以下の Reject は、機能要求そのものの否定ではない。両文書とも「現行実装に接続する規範」と「lossless な正本」を欠いたまま WP を開始すると、後続の undo/save/reload で形式を壊すため、設計を差し戻すという意味である。

---

## 1. editor tooling の blocker

### E-1. 「逆方向 serialize を追加」で済むという前提が成立しない

設計は「全 component は JSON から構築でき、逆方向 serialize を登録へ追加」「WP62 で serializer は optional 化済み」とする (`docs/design_editor_tooling.md:52-57`)。現実には `ComponentInfo` が持つ JSON callback は `cb_load_by_json2` 一個だけであり、manager の公開操作も `loadByJson` までである (`src/core/ecs/componentinfo.hpp:20-48`)。typed registerer も `JsonArchiveLoader` に対する `ref` の有無だけを検出して loader を登録する (`src/core/userpublic/details/component/registerer.hpp:65-82`)。`JsonArchiveSaver` は `prop` の宣言だけで、出力先も定義もない (`src/core/userpublic/serialize/jsonarchive.hpp:30-47`, `src/core/userpublic/serialize/jsonarchive.cpp:1-56`)。

従って WP62 の「optional serializer」は、実装上は **optional JSON loader** である。逆方向は callback 一個の追加ではなく、canonical scene codec、schema、special binder、authored/runtime 値の区別を新設する仕事である。

主要 component を、仮に汎用 `JsonArchiveSaver` だけ足した場合まで敵対的に追うと次のとおりである。

| scene component | 現行経路 | 逆方向の実情 |
|---|---|---|
| `transform` | ComponentInfo | `ref` は未初期化の一時値を archive に渡した後、その値を component へ書き戻す loader 専用形である。現在値を保存する対称 codec ではない (`src/core/ecs/predefined/transform.hpp:9-26`)。階層 scene の authored transform は local TRS だが runtime `TransformComponent` は world 側であり、そのまま保存すると意味が変わる (`docs/design_scene_format.md:80-85`, `src/core/loader/scene.cpp:292-313`)。 |
| `simplemodelview` | ComponentInfo | `model` 一項は対称化しやすい (`src/core/userpublic/components/modelview.hpp:10-18`)。ただし model 変更時の lifecycle/再 bind まで codec の責務に含める必要がある。 |
| `camera` | ECS 上は空 marker | ECS `ref` は空である一方、projection/controller は Camera が source JSON を直接再 parse する (`src/core/ecs/predefined/camera.hpp:5-9`, `src/core/renderer/camera.cpp:505-539`)。ComponentInfo だけでは表示も編集もできない。 |
| `animation` | ComponentInfo + SceneLoader 正規化 | runtime は `loop` を `uint8_t` で持つが、scene loader は authored JSON の boolean を要求する (`src/core/userpublic/components/animation.hpp:8-19`, `src/core/loader/scene.cpp:133-152`)。汎用 saver は canonical scene JSON を生成しない。 |
| `sprite_view` | ComponentInfo + custom closed loader | saver 側 `ref` は `flip_x`/`flip_y`/`has_explicit_size` と数値 `billboard` を出すが、scene 側は `flip` 配列、文字列 `billboard`、closed key 集合を要求する (`src/core/userpublic/components/spriteview.hpp:27-41`, `src/core/userpublic/components/spriteview.cpp:16-24`, `src/core/userpublic/components/spriteview.cpp:56-92`)。往復不能である。 |
| `light` | SceneLoader → LightContainer の特別扱い | ComponentInfo を通らないことが scene v1 の明示規範であり、実装も special dispatch する (`docs/design_scene_format.md:70-79`, `docs/design_scene_format.md:107-110`, `src/core/loader/scene.cpp:122-130`)。 |
| `collider` | SceneLoader → PhysWorld の特別扱い | ComponentInfo に登録されず special dispatch される (`src/core/loader/scene.cpp:67-78`, `src/core/loader/scene.cpp:122-130`)。 |

現状のままなら全 component が serializer なしで read-only になる。素朴な Saver を足しても、安全に編集可能と言える主要 component は `simplemodelview` 程度で、transform/camera/light/collider/animation/sprite は別 adapter が要る。「read-only だらけにならないか」という問いへの回答は **なる** である。

### E-2. source object、runtime entity、編集対象の identity が一致しない

E-RPC は `scene_tree` の各 object に必ず `entity_id` を返し、`get_components(object)` や edit の `object` を使う (`docs/design_editor_tooling.md:27-30`, `docs/design_editor_tooling.md:38-43`)。しかし scene v1 の object 名は省略可能である (`docs/design_scene_format.md:61-65`)。さらに light は ECS entity を作らず、collider-only object は有効な ECS component list を持たない。SceneLoader は light/collider を取り除いた後、残った ECS component がある object だけを通常 entity として生成する (`src/core/loader/scene.cpp:122-170`, `src/core/loader/scene.cpp:270-289`, `src/core/loader/scene.cpp:331-355`)。名前→ID binding も named transform object に限られる (`src/core/loader/scene.cpp:280-289`, `src/core/loader/scene.cpp:341-347`)。

従って、source scene の全 object を `EntityId` で指すことはできない。逆に runtime spawn は source JSON にまだ存在しない。E-RPC v1 には、少なくとも次の三 identity が必要である。

1. `SceneRevision` — query/edit の optimistic concurrency と stale 応答用。
2. `AuthoringObjectId` — scene document 内で無名・light-only object も指せる session-stable handle。
3. optional `EntityId` — runtime entity を持つ場合だけ付く世代付き handle。

`scene_tree` は `entity_id` を nullable にし、query が **authored JSON** と **runtime projected value** のどちらを返すかを field ごとに明示しなければならない。これがないと「現在値→JSON」と「ファイルが正本」が衝突する。

### E-3. 同期 RPC では「次フレームの適用結果を edit 応答」は返せない

設計は edit を次フレーム先頭で一括適用し、その成功/エラーを応答するとする (`docs/design_editor_tooling.md:44-46`)。現行 stdio server は request handler が戻った時点で直ちに一行応答し、次の request を読む同期ループである (`src/core/communication/rpcserver.cpp:676-720`)。既存 `update_transforms` はこの制約に合わせ、edit request には `{queued:N}` を返し、実適用は後続 `step_frame`/`render_frame` で行う (`src/core/communication/rpcserver.cpp:953-960`, `src/core/communication/rpcserver.cpp:990-1005`)。適用時エラーは後続 frame method のエラーであって、元 edit request の応答ではない (`src/core/communication/rpcserver.cpp:359-368`)。

ここは protocol を二段に固定すべきである。edit request は parse/precondition 成功と `ticket` だけを返し、frame-boundary commit 後の結果は `step_frame.edit_results[]` または `get_edit_result(ticket)` で返す。元 request を次フレームまで block すると、単一 stdio client は frame を進める request を送れず deadlock する。

また適用位相も未定義である。現行 frame の先頭では runtime reload が phase 列より先に適用され、その後 `freeze_events → freeze_input → deliver_events → update_game` と進み、pending scene load は update の末尾で適用される (`src/core/appflow/framephase.cpp:90-104`, `src/core/appflow/framephase.cpp:123-143`)。E-RPC commit は `runtime reload 後・freeze_events 前` など一点に固定し、pending scene transition との競合は `SceneRevision` 不一致で reject しなければならない。

### E-4. D0 の共有実体は作れるが、現行 handler を ImGui から共有はできない

D0 は編集系 RPC 自体を本当の API とし、devstudio は RPC client としてのみリンクする決定である (`docs/design_devstudio_direction.md:14-31`)。editor 文書の「E-RPC handler と同じ内部関数」はこの決定と両立し得るが、現行 edit 実装は `runEngineRpcServer` 内の capture lambda で、module 解決も RPC translation unit の private aggregate/anonymous helper に閉じている (`src/core/communication/rpcserver.cpp:289-325`, `src/core/communication/rpcserver.cpp:723-729`, `src/core/communication/rpcserver.cpp:946-976`)。今の関数を ImGui から呼ぶ形ではない。

必要なのは、JSON を知らない typed `EditorCommandService` を engine 側に一個置き、RPC adapter と ImGui adapter の双方が **同じ enqueue/query 関数**へ到達する構造である。`UI-INS0` の「裏口 grep fixture」 (`docs/design_editor_tooling.md:105`) は保証として弱い。fake service で RPC/UI の同一 command・同一 validation・同一 ticket/result を比較し、UI target が ECS/SceneLoader の mutation API を直接呼ばないことを target/link 境界で検査すべきである。

### E-5. ReloadGate の `enabled()` を edit gate に流用すると RPC 自身が使えない

設計は replay/strict 中の edit reject を「HR と同じ中央 gate」で行う (`docs/design_editor_tooling.md:61-67`)。現行 `ReloadGate` は hot reload の enable gate であり、reason は replay/strict に加えて **rpc_driver** を持つ (`src/core/watch/reloadgate.hpp:13-29`)。`enabled = requested && !replay && !strict && !rpc` なので通常の stdio RPC driver でも false になる (`src/core/watch/reloadgate.cpp:26-34`)。しかも snapshot は個々の reason bit を公開せず、文字列しか返さない (`src/core/watch/reloadgate.hpp:15-18`, `src/core/watch/reloadgate.cpp:37-43`)。

従って `ReloadGate::enabled()` を購読して edit 可否を決めてはならない。中央 policy の所有は共有してよいが、`can_edit = !replay && !golden && !strict` を独立 query として追加し、RPC driver 自体は許可する必要がある。golden/replay では tool callback 自体を走らせない既存規範も維持する (`docs/design_ui_2d_foundation.md:599-606`)。

### E-6. journal の四項組は undo のデータモデルではなく見出しにすぎない

`{op,target,before,after}` を「逆適用可能」とするだけでは (`docs/design_editor_tooling.md:47-48`)、次が決まらない。

- `destroy_object`: 子孫を同時破棄するのか、子を root/reparent するのか。undo に必要な全 object/component raw JSON、元の object 配列 index、parent edge、component 配列 index、special component、runtime-only attachment のどこまでを `before` に含むか。
- `spawn_object`: undo/redo 後に EntityId が変わる。後続 command の target を名前や旧 EntityId にすると再適用不能である。
- `reparent`: old parent だけでなく、宣言順/sibling 表示順を変更する操作か否か、cycle 検査、world pose を保持するのか local TRS を保持するのかがない。
- `add/remove_component`: ECS は同一 component 重複を拒否し、現行 public API には既存 entity の archetype を migration して component を add/remove する面がない。公開面は create/remove/set-existing までである (`src/core/userpublic/gameobjects.hpp:119-127`, `src/core/userpublic/details/ecs/coretemplate.hpp:100-132`)。
- 複合編集: gizmo drag、multi-select、spawn+configure、behavior attach+params 設定を一 undo にする transaction/group/coalescing がない。
- failure atomicity: command 3 が失敗した場合に 1,2 を rollback するのか、部分 journal を残すのかがない。

journal の正本は command 単位でなく、少なくとも `{transaction_id, base_scene_revision, ordered_commands, ordered_inverse_commands, affected_authoring_ids, commit_status}` とする必要がある。全 command を preflight し、authoring document と runtime projection の両方が成功したときだけ一 transaction を journal へ載せること。destroy inverse は affected closure の lossless snapshot、add/remove inverse は component raw JSON と正確な配列位置を持つこと。ImGui drag は明示 `coalesce_key` または begin/end transaction で一単位にすること。

### E-7. SAVE0 は現在の記述だと正本ファイルを破壊する

「書き出せる component のみ警告付きで」scene file を上書きする案 (`docs/design_editor_tooling.md:90-97`) は受理できない。read-only component を省略した保存は warning ではなく data loss であり、そのファイルを次回ロードすると runtime も失われる。scene v1 は一ファイル複数 scene が標準なので、current scene だけを再生成しても他 scene を落とす (`docs/design_scene_format.md:61-69`)。

さらに現行 `ProjectBasicConfig::sceneDataJson()` は初回読込後の文字列を cache し、invalidate/update API を持たない (`src/core/loader/basicconfig.hpp:32-47`, `src/core/loader/basicconfig.hpp:60-71`, `src/core/loader/basicconfig.cpp:431-435`)。scene JSON は asset HR の自動 reload 対象外で、明示 `load_scene` だけが規範である (`docs/design_asset_hot_reload.md:214-225`)。従って「書けば HR が拾って次回 load と一致」は現実には成立せず、同一 process の `load_scene` や G2 rebuild は cache 済み旧 document を再利用し得る。G2 rebuild が `currentScene()` を再 load する実装もこの経路である (`src/core/gamelogic/gamelogicreload.cpp:353-363`)。

SAVE0 の正本は runtime ECS の全 serialize ではなく、load 時から raw JSON を lossless に保持する **authoring document** でなければならない。edit transaction が authoring document と runtime adapter を同時 commit し、Save は document 全体を deterministic に atomic replace する。codec のない既存 component は raw JSON をそのまま保持し、決して省略しない。runtime-only object/component を保存できない場合は Save 全体または当該 edit を hard error にする。保存前に baseline digest と disk digest を比較して外部変更の上書きを拒否し、成功時に cache と `SceneRevision` も同じ transaction で更新すること。

HR の self-write token は、runtime apply 成功と `live_digest` 前進が同一 transaction の場合だけ通知を抑制できる規範である (`docs/design_asset_hot_reload.md:110-133`)。scene は現状 HR 対象外なので、SAVE0 が token を使うと主張してはならない。将来 scene watch を追加するなら別 WP でこの三状態契約へ参加させる必要がある。

---

## 2. object behavior の blocker

### B-1. 提示 JSON は scene format には additive だが、通常 ECS component にはならない

scene v1 validator は component entry が object で string `name` を持つことまでしか検証しないため、`{"name":"behavior","type":...,"params":...}` 自体は additive に受理可能である (`src/project/sceneformat.cpp:114-127`)。`type`/`params` の型・unknown key は behavior binder が追加検証すべき事項である。

一方、一 object に複数 behavior を同名 component の反復で表す案 (`docs/design_object_behaviors.md:49-60`) をそのまま ComponentInfoManager へ流すと、同じ ComponentId が二回並ぶ。ECS は EntityId を加えた component list を sort し、duplicate を明示エラーにする (`src/core/userpublic/details/ecs/coretemplate.cpp:93-112`)。従って `behavior` は普通の一値 ECS component ではない。

v1 は次のどちらかを規範に固定すべきである。

1. SceneLoader が反復 behavior records を declaration order のまま集め、engine-owned `BehaviorAttachmentArena` と entity 一個につき一個の `BehaviorAttachmentListComponent` へ lower する special binder。
2. scene JSON を一個の `{"name":"behaviors","items":[...]}` component に変える。

既存 JSON を維持するなら 1 が自然である。light/collider と同様の special dispatch であること、behavior-only object にも有効な entity を生成すること、generic `add_component/remove_component` とは別に attachment index/handle を持つことを明記せよ。

また永続データに C++ 型名を使う `PELICAN_REGISTER_BEHAVIOR(PlayerControl)` (`docs/design_object_behaviors.md:23-38`) は namespace/refactor で scene を壊す。UI widget registry が explicit stable name と schema version を持つ先例 (`docs/design_ui_2d_foundation.md:353-376`) に合わせ、`PELICAN_REGISTER_BEHAVIOR(Type, "player_control", schema_version)` のように C++ spelling と persistent name を分けるべきである。

### B-2. params は WP62 の component loaderにも EventPayloadSchema にも、そのまま相乗りできない

`Params` の JSON 往復を「WP62 と同じ流儀、固定 field をマクロ宣言」とするが、具体的な正本がない (`docs/design_object_behaviors.md:62-70`)。

- ComponentInfo 登録に field schema/range/enum/default はなく、存在するのは whole component の loader callback だけである (`src/core/ecs/componentinfo.hpp:20-32`, `src/core/userpublic/details/component/registerer.hpp:20-31`)。
- `JsonArchiveLoader::prop` は scalar/vec/quat/string のみで、bool、nested object、optional field がない (`src/core/userpublic/serialize/jsonarchive.hpp:10-28`)。各 `prop` 実装は `.at(name)` なので、Params の field 省略時に C++ default を残す挙動にもならない (`src/core/userpublic/serialize/jsonarchive.cpp:6-53`)。
- EventPayloadSchema の member-pointer descriptor は再利用価値が高いが、現行型集合は bool を意図的に拒否し、全 field required である (`docs/design_event_payload_schema.md:46-58`, `docs/design_event_payload_schema.md:86-105`)。behavior 文書は bool と params 省略/default を要求するため、そのまま流用はできない。

新しい behavior 専用 field macro を重複発明するのではなく、EventPayloadSchema の `field<&T::member>` を generic `StructFieldSchema` へ抽出し、Bool/Enum、required/default、canonical JSON encode/decode を additive に拡張するのがよい。Params は default construct 後に **存在する key だけ** atomic に apply、unknown key/type/range/NaN/Inf を reject、encode は scene が許す canonical bool/number/string/vecN を出す。一個の descriptor が loader、saver、inspector schema を駆動し、`ref` と descriptor の二重宣言を正本にしてはならない。

pending 中は DLL schema がないため raw `params` JSON を engine-owned storage に保持する。active DLL が正常にロード済みなのに type が見つからない場合は typo として hard error、DLL 自体が unavailable の場合だけ pending WARN、と状態を分ける必要がある。

### B-3. `EventView` は現行 E1 の購読経路に存在せず、誰が何を購読するか未定義

behavior は `onEvent(BehaviorContext&, EventView const&)` を override して「E1 購読」とする (`docs/design_object_behaviors.md:32-36`)。現行 E1 は event を `type_index/name/shared payload` で queue し (`src/core/userpublic/details/event/registerer.hpp:28-45`)、system registration 時に `onEvent(const Event&, GameContext&)` overload を event type ごとの dispatch thunk として収集する (`src/core/userpublic/details/system/registerer.hpp:39-80`, `src/core/userpublic/details/system/registerer.hpp:134-151`)。frame 頭では frozen event を registered systems にだけ配送する (`src/core/userpublic/details/event/registerer.cpp:393-417`)。`EventView` も catch-all subscriber hook も現行にはない。

v1 は「全 behavior が全 event を受ける」暗黙 broadcast にしてはならない。推奨は system と同じ typed overload、例えば `onEvent(const DoorOpened&, BehaviorContext&)` を registration 時に thunk 化し、matching type の attachment だけへ宣言順配送する方式である。generic EventView を採るなら、登録時の subscribed type/name 一覧、payload の型安全な取得、unknown/reloaded event type、配送順、owner unload 時の queue 除去まで定義せよ。BehaviorSystem の event 配送位置も system order に含め、update だけ位置を決めて event は別順になる穴を残してはならない。

### B-4. `onInit` は component init callback でも「登録が揃った瞬間」でも実行できない

ECS の entity 作成 transaction は populate 後に component `init` を呼ぶが、EntityId を live publish するのは全 init 成功後である (`src/core/userpublic/details/ecs/coretemplate.cpp:212-246`)。scene hierarchy は全 entity 作成後に parent EntityId を配線する (`src/core/loader/scene.cpp:270-313`)。従って component `init` から behavior `onInit(ctx.self())` を呼ぶと、自 entity を resolve できず、兄弟や parent も未完成である。

scene load は二段にすべきである。第一段で全 entity/component/special attachment と parent edge を transactionally publishし、第二段の scene activation barrier で behavior instances を attachment order に生成して `onInit` する。onInit failure は scene activation 全体を rollback し、既に init 済みの instance を逆順 onDestroy/destroy すること。

同様に destroy は entity/component/parent がまだ resolve 可能な pre-destroy barrier で `onDestroy` を逆 attachment 順に一回だけ呼ぶ必要がある。現行 ECS clear は component storage を先に clear してから EntityId を release する (`src/core/userpublic/details/ecs/coretemplate.cpp:303-321`) ため、外部 arena の behavior を自動では通知しない。`onDestroy` は teardown を止めない `noexcept` 契約、または engine が例外を捕捉して必ず destroy thunk を実行する契約にすること。

さらに BehaviorContext は GameContext と同じ create/remove 面を持つので、callback 中の spawn/destroy/attach が iteration を変更できる (`src/core/userpublic/gamecontext.hpp:40-47`)。各 frame は開始時の attachment snapshot を一回だけ走査し、callback 中の structural mutation は次の edit/behavior commit boundary まで queue する、という規範が必要である。

### B-5. 「scene 宣言順 = EntityId 発行順」は runtime spawn と ID 再利用を閉じない

設計は entity stable order を scene declaration order = EntityId issuance order とする (`docs/design_object_behaviors.md:72-79`)。しかし EntityId は free-list LIFO で再利用される。remove は index を stack に積み、次の allocate は末尾 index を取る (`src/core/userpublic/details/ecs/coretemplate.cpp:76-85`, `src/core/userpublic/details/ecs/coretemplate.cpp:191-207`)。clear も index 昇順に release するため、次 scene load の numeric ID は declaration orderと逆向きになり得る (`src/core/userpublic/details/ecs/coretemplate.cpp:303-320`, `docs/design_reviews/2026-07-10_wp62_report.md:43-49`)。また chunk remove は末尾要素を穴へ relocate するため、storage iteration orderも安定 identity ではない (`src/core/userpublic/details/ecs/chunk.cpp:71-81`, `src/core/userpublic/details/ecs/chunk.cpp:190-202`)。

BehaviorSystem は EntityId 数値や ECS storage 順で sort せず、engine-owned monotonic `attachment_seq` を持つべきである。scene load は `(object declaration index, component array index)`、runtime transaction は `(commit_seq, command_index, attachment_index)` から列を確定する。frame 中 spawn は次 boundary から参加、destroy 済みは snapshot 上 skip、undo restore が元 seq を復元するか末尾へ入るかも journal 規範で一意にせよ。BehaviorSystem 自身の system `order` と同 order の name tie-break も固定値として文書化すること。

### B-6. G2 の owner token は再利用できるが、「登録時 init」は候補検証 protocol と衝突する

`PELICAN_REGISTER_SYSTEM` は登録時 TLS owner を record へ付け、owner 単位で解除する (`src/core/userpublic/details/system/registerer.cpp:9-20`, `src/core/userpublic/details/system/registerer.cpp:56-67`, `src/core/userpublic/details/reload/registrationowner.cpp:7-25`)。この owner token と static registration pattern は behavior registry に再利用できる。ただし自動で全 registry が解除される共通 manager ではない。GameLogicReloader は failure/unload の各 branch で system/event/physics/animation を個別に明示解除する (`src/core/gamelogic/gamelogicreload.cpp:93-147`)。behavior registry と live instances の purge を全 branch に追加する必要がある。

より重大なのは、G2 が新 DLL を **live runtime teardown 前に一度 LoadLibrary して登録・ABI 検証し、すぐ unload** する点である (`src/core/gamelogic/gamelogicreload.cpp:158-163`, `src/core/gamelogic/gamelogicreload.cpp:205-228`)。behavior 文書の「登録が揃った時点で instance 化 + onInit」 (`docs/design_object_behaviors.md:81-85`) を実装すると、この candidate validation load 中に旧 sceneへ新 code の instance/onInit が走り、直後の unload で関数 pointer が dangling になる。

behavior registration は factory/schema/thunk を記録する **宣言だけ** とし、static registration や candidate validation で instance 作成・onInitを一切してはならない。accepted owner の activation と scene rebuild 後にだけ instance 化する。reload 成功 sequence は `candidate declarative validate → old instances onDestroy/destroy → old owner unregister → old DLL unload → new owner load/activate → scene rebuild → onInit`、rollback は同じ規範で旧 DLL/scene を再構築すること。現行 reload が teardown→unload→new load→rebuild の順を持つことは利用できる (`src/core/gamelogic/gamelogicreload.cpp:223-267`)。

この owner safety は G2 の追加機能ではなく、WP90 済みの現在の通常 build で BEH0 を安全に動かす最低条件である。従って owner-aware registry/unregister/destroy thunk は BEH1 へ先送りせず BEH0 に含め、BEH1 は二世代 reload/pending/rollback の統合 fixture に絞るべきである。

### B-7. 「状態は params/component 側へ置けば reload 後も残る」は現行 WP90 と矛盾する

behavior 文書は内部状態は消えるが、残したい値は params/component 側へ置くとする (`docs/design_object_behaviors.md:87-93`)。現行 WP90 は runtime teardown で physics/ECS/model instances を全 clear し (`src/core/appflow/teardown.cpp:31-42`)、新 DLL load 後に `currentScene()` を scene file/cache から全再構築する (`src/core/gamelogic/gamelogicreload.cpp:223-263`, `src/core/gamelogic/gamelogicreload.cpp:353-363`)。実装 report も ECS/scene は DLL 世代ごとに破棄・再構築と明記する (`docs/design_reviews/2026-07-12_wp90_report.md:14-26`)。

従って v1 で reload 後に残るのは **再構築元の authored scene に既に入っている値だけ** であり、runtime で変化した component や未保存の behavior params ではない。しかも scene text は `ProjectBasicConfig` に cache される (`src/core/loader/basicconfig.cpp:431-435`)。BEH1 は次のどちらかを明示選択せよ。

1. WP90 full-reset を維持し、「unsaved/runtime state は params/component を含めすべて消える。Save+authoring cache commit 済みの値だけ復元」と訂正する。
2. engine-owned authoring document/params snapshot を rebuild source にする新 transaction を実装し、その snapshot の generation、失敗 rollback、disk との authority を規範化する。

「component に置けば残る」という現行文言だけは削除必須である。

---

## 3. 依頼で指定された六点への短答

1. **journal 四項組は undo に足りるか** — 足りない。spawn/destroy の affected closure と配列位置、reparent の local/world policy、stable authoring identity、transaction grouping、preflight/rollback、async result ticket が必要である。§1 E-3/E-6 を WP 条件にすること。
2. **serializer coverage** — 現在の逆方向 callback は 0。素朴に Saver を足しても主要 component の canonical round-trip はほぼ成立しない。camera/light/collider は別 binder、transform は local/world mapping、animation/sprite は scene 表現変換が必要である。§1 E-1 の表が coverage baseline である。
3. **behavior params 反射** — WP62 をそのまま再利用不可。EventPayloadSchema の member-pointer descriptor を generic schema に抽出し、Bool/Enum/default/optional/canonical saver を追加して component/behavior/editor の共通土台にする。behavior 専用 macro をもう一系統作らない。
4. **runtime spawn 順** — EntityId/free-list/ECS storage 順では閉じない。scene declaration index と monotonic runtime commit sequence による engine-owned `attachment_seq` を正本にする。
5. **G2 pending/params 再 init** — 起動は DLL→scene なので通常 startup に pending は不要 (`src/core/userpublic/pelican_core.cpp:73-75`)。DLL unavailable 時だけ raw pending を保持し、candidate registration では解決しない。G2 は accepted DLL 後の scene rebuild で解決する。params の復元元は現状 cached authored scene だけである。
6. **WP 粒度** — E-RPC0 と BEH0 が前提工事を抱えすぎ、BEH0/1 の owner 境界が逆である。次節の分割へ直すこと。

UI/HR との関係も明確にする。U3 は game UI の Controller factory/lifecycle/hot reload transaction であり (`docs/design_ui_2d_foundation.md:353-378`, `docs/design_ui_2d_foundation.md:613-619`)、editor journal/SAVE の実装依存ではない。factory は metadata のみ、live instance は arena、reload は side-build→frame-boundary swap→逆順 deinit という **規範パターン** は behavior に再利用できる。ImGui engine tool 自体は同文書では U4 である。scene file は HR 自動 reload 対象外、`pelican.ui` だけが U3/HR0 に合流する (`docs/design_asset_hot_reload.md:214-225`)。この境界を混ぜてはならない。

---

## 4. WP に逐語添付すべき条件

### 4.1 editor 系

#### ED-AUTH0（E-RPC0 の先行 WP、新設）

> scene v1 の全 envelope/全 scene/全 object/全 component raw JSON を保持する engine-owned AuthoringSceneDocument を導入する。各 load は単調増加 SceneRevision と session-stable AuthoringObjectId を割り当て、無名・light-only・collider-only object も列挙可能にする。runtime EntityId は nullable projection とし、source object identity に代用しない。ProjectBasicConfig の scene cache は AuthoringSceneDocument と一つの更新/invalidate APIへ統合する。parse→bind→query→deterministic encode で未編集 document が semantic equal、非 current scene と read-only/special component が一項も脱落しない fixture を gate とする。

#### ED-CODEC0（E-RPC0 の先行 WP、新設）

> component codec を `decode authored JSON / encode canonical authored JSON / schema / runtime apply / runtime project` の組として登録する。ComponentInfo の lifecycle/loader callback を逆方向 serializer が既にあるものとして扱わない。v1 coverage は transform(local/world mapping と hierarchy)、simplemodelview、camera、light、collider、animation(boolean loop)、sprite_view(closed schema)を必須とする。各 component について authored→runtime→canonical JSON→fresh load の semantic equality と invalid type/range/unknown keyを fixture 化する。codec のない component は raw authored JSON を保存時に必ず保持し、runtime edit 不可を query metadata で明示する。

#### E-RPC0 改訂条件

> query は `{scene_revision, authoring_object_id, name?, parent?, entity_id?, components[]}` を返す。component ごとに `authored_json`, optional `runtime_json`, `editable`, `codec/schema state`, `pending` を区別する。JSON key/order、object/component declaration order、EntityId 表示は二回実行で一致させる。RPC handler の外に typed EditorCommandService を置き、RPC/ImGui fake adapter が同じ query 結果/error codeを返すことを検査する。依存は WP62(済)だけでなく ED-AUTH0/ED-CODEC0 とする。

#### ECS-MUT0（E-RPC1 の先行 WP、新設）

> live entity の component add/remove に必要な archetype migration を ECS transaction として実装する。重複/EntityId remove/不存在、construct-populate-init fault、deinit/destroy、over-alignment、move-only、system chunk cache/version、free-list/ID、external binding の rollback を WP62 と同じ failure-atomicity で検査する。special attachment(light/collider/behavior)は generic ECS migration と同一視せず adapter を持つ。E-RPC1 はこの WP 完了前に add/remove_component を約束しない。

#### E-RPC1 改訂条件

> edit request は enqueue acceptance と ticket を同期応答し、frame-boundary commit の結果は `step_frame.edit_results[]` または `get_edit_result(ticket)` で返す。commit 点を runtime reload 後・freeze_events 前の一点に固定し、replay/golden/strict は method 名と reason 入りで reject、rpc_driver 自体は許可する。全 transaction は base SceneRevision を検査し、ordered command を全 preflight 後、AuthoringSceneDocument と runtime adapterへ failure-atomic に commitする。部分成功を禁止し、journal には committed transaction だけを記録する。RPC/windowed の同一 phase trace、stale revision、scene transition 競合、callback/reload 競合を fixture に含める。

#### JOURNAL0（E-RPC1 内の必須成果物）

> journal record は `{transaction_id, base_revision, committed_revision, ordered_forward, ordered_inverse, affected_authoring_ids, coalesce_key?, status}` とする。destroy inverse は affected object closure、全 raw component JSON、元 object/component array index、parent edgeを lossless に持つ。reparent は old/new parent と local/world preservation policy を持つ。spawn/redo は旧 EntityId でなく AuthoringObjectId を再 bind する。multi-select、spawn+configure、gizmo drag は明示 transaction/coalesce 単位とする。forward→inverse→forward の三段で authoring document semantic equality、runtime query equality、behavior lifecycle trace一致を gate とする。

#### UI-INS0 / UI-AB0 改訂条件

> ImGui は EditorCommandService の query/enqueue/poll-result interface だけを使用し、ECS/SceneLoader/LightContainer/PhysWorld を直接 mutateしない。RPC adapter と UI adapter に同一 command を与え、validation/error/ticket/commit trace が一致する fake-service test を置く。grep は補助に降格する。replay/golden/headless では panel callback・query・edit enqueue が 0 回であることを既存 U4 規範と同じ trace で検査する。

#### SAVE0 改訂条件

> Save は runtime ECS を列挙して「serialize 可能な component だけ」を出力してはならない。AuthoringSceneDocument の全 envelope/全 scene/全 raw component を deterministic encodeし、baseline disk digest 一致を確認後、同一 directory の temporary fileへ write+flush+parse/semantic validateし、atomic replaceする。不一致は external modification error、codec/pending/runtime-only data の loss は hard errorとする。file replace、ProjectBasicConfig cache、AuthoringSceneDocument revision の公開を一 transaction とし、失敗時は旧 file/cache/revisionを維持する。scene は HR 自動 reload 対象外であることを status/UI に明示する。保存→同一 process明示 reload→新規 process reloadの双方で全 scene tree/component semantic equalityを gate とする。

推奨依存順は次である。

`ED-AUTH0 + ED-CODEC0 → E-RPC0 → ECS-MUT0 + E-RPC1/JOURNAL0 → UI-INS0 / UI-AB0 / SAVE0`

### 4.2 behavior 系

#### BEH-P0（BEH0 の先行 WP、新設。ED-CODEC0 と共通化可）

> EventPayloadSchema の member-pointer field descriptor を generic StructFieldSchema へ抽出し、Bool/Enum、required/default、range、unit、canonical JSON encode/decodeを持たせる。Params は default construct 後に存在 keyだけを temporary へ適用し、全検証成功後に commitする。unknown key、型不一致、整数範囲、NaN/Inf、range、nested/unsupported type は stable error code+field pathで拒否する。params 省略、部分指定、全型往復、二回 encode byte一致、compile-fail unsupported fieldを gate とする。behavior/component/eventで別々の field macroを新設しない。

#### BEH0 改訂条件

> `PELICAN_REGISTER_BEHAVIOR(Type, stable_name, schema_version)` は factory/destroy/typed-event-dispatch/params-schema と RegistrationOwner を宣言登録するだけで、static init/candidate validation 中に live instanceを作らない。SceneLoader は反復 `name=behavior` recordを special binderで declaration orderの attachment listへ lowerし、behavior-only objectにも entityを作る。一 object複数 behavior、同 type複数 attachmentを許可し、各 attachmentは engine-owned handleと attachment_seqを持つ。全 entity/component/parent publish後の activation barrierで onInit、pre-destroy barrierで逆順 onDestroy+destroyを一回だけ実行する。callback中の structural mutationは次 boundaryへ deferし、frame-start snapshotを各 instance最大一回走査する。
>
> BehaviorSystem の固定 system order/name tie-break、scene `(object index, component index)`、runtime `(commit_seq, command index, attachment index)` の順を規範化し、EntityId数値/ECS storage順を使用しない。E1は typed `onEvent(const Event&, BehaviorContext&)` thunkまたは明示 subscription一覧で既存 frozen eventへ接続し、matching eventだけを BehaviorSystem位置で配送する。決定性 gateは実行/イベント/lifecycle traceと RNG結果を新規 process二回および replay二回で byte一致させる。
>
> G2が既に存在するため、owner別 registry purge、owner別 live instance destroy、全 GameLogicReloader failure/unload branchからの unregisterは BEH0の必須安全条件とする。BEH0を「reload未対応だが dangling callbackを残す」状態で完了させない。

#### BEH1 改訂条件

> 二世代 DLL fixtureで `candidate validation load/unload は registration count以外の runtime/lifecycle traceを一切変えない` ことを最初に検査する。成功 reloadは old onDestroy/destroy→old unregister/unload→new register/activate→scene rebuild→new onInit の完全列を、失敗 reloadは旧 DLL/runtime維持、new scene/params decode failureは旧 DLL reload+scene rebuild rollbackを検査する。新 DLLで type削除/schema非互換、DLL unavailable pending、後続復旧、queued event owner purge、onInit faultを独立 fixtureにする。active DLLが正常なのに type不在は hard error、DLL unavailable時だけ raw JSON pending+名前入りWARNとする。
>
> params/stateの復元元を明記する。WP90 full-resetを維持する場合は「runtime/unsaved component/params stateは消え、AuthoringSceneDocumentへcommit済みの値だけ再 init」を規範文にする。runtime snapshotを残す案を採る場合は別 versioned migration WPとし、BEH1へ暗黙に混ぜない。

#### BEH2 改訂条件

> behavior attach/remove/set-paramは generic ECS component nameだけでなく attachment handle/indexをtargetにし、E-RPC transaction/journalへ参加する。attachはcommit後の次 activation boundaryでonInit、removeはcommit前pre-destroyでonDestroy、params editは「live instanceへatomic apply」または「destroy/recreate」のどちらかを固定し、同一frameのonEvent/onUpdateから見える版を規範化する。undo/redo、replay/golden reject、G2同時発生、type comboのowner generation更新、pending params表示をfixtureにする。依存は BEH0/BEH-P0/E-RPC1/JOURNAL0/ED-AUTH0 とする。

推奨依存順は次である。

`BEH-P0 + ED-AUTH0 → BEH0 → BEH1`  
`BEH0 + E-RPC1/JOURNAL0 → BEH2`

---

## 5. 再レビューの最小提出物

再レビュー時はコード実装ではなく、まず次の設計差分があればよい。

1. `AuthoringSceneDocument / SceneRevision / AuthoringObjectId` と runtime projection の状態図。
2. component codec coverage 表（少なくとも E-1 の七種）と canonical JSON 例。
3. edit ticket/commit/result、journal transaction、Save/cache/file の失敗時状態遷移。
4. behavior attachment の scene lowering、activation/deactivation、typed E1 dispatch の擬似コード。
5. G2 candidate validate/success/rollback/pending の lifecycle trace。
6. 改訂 WP 表と、上記逐語条件をどの gateへ添付したかの対応表。

これらが入れば、方向性を保ったまま「Reject → 条件付き」へ上げられる。現状は、E-RPC の正本が runtime ECS なのか authored document なのか、behavior の正本が ECS component なのか attachment arena なのかが未決であり、実装から決めるには影響範囲が大きすぎる。
