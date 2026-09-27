# 第8章 ゲームロジック

対象: pelican2(2026-09-17 に §8.4 の API 表と §8.13(アニメーション)を全面更新。HEAD=`82054c05`)/ このマニュアルはコードを正とする

## この章で学ぶこと

- ゲームコード(C++)の書き方 — システム登録・`GameContext`・オブジェクト操作・DLL ホットリロード
- ゲーム DLL の ABI(登録入口・epoch anchor・古い DLL の拒否)と schema の宣言面
- **オブジェクト behavior**(`PELICAN_REGISTER_BEHAVIOR` — シーンに貼れる振る舞い。✅WP155/162/167)
- イベント層(emit と購読、フレーム境界配送)
- シーン遷移・カメラ(コントローラ含む)・物理クエリ(shapeCast / moveAndSlide)の使い方
- ポインタの位置(framebuffer ピクセル)・カーソルのモード・画面の点を通る ray(`ctx.input()` / `screenRay`、✅GP-4)
- 決定性(固定ループ順序・決定的乱数・シード)— 「なぜ毎回同じ結果になるのか」
- オーディオ・永続化(設定/セーブ)・2D スプライトの操作
- アニメーション — **姿勢の 2 つの装着様式**(`animation` コンポーネント / 共通グラフの `graph.instance` + `pose.*`)、root motion、観測、`PELICAN_WITH_ANIMATION=OFF` の見え方

(本文の `✅WP155` のような `WP<数字>` は、エンジン側の作業単位(work package)に付く通し番号です — [用語集](14_glossary.md)。「どの作業で入った機能か」の目印として添えています)

## 8.1 ゲームコードの位置づけ

> **設計決定(ネイティブ一本・2026-07-07 ユーザー決定):** ゲームロジックは C++ で書く。スクリプト言語は不採用。エンジンは「ソース同居の SDK」。

✅WP90(G2)で、ゲームコードのビルド形態は静的リンクから **`pelican_game_logic.dll`** に移行しました。公開面(userpublic)だけを import する DLL で、実行中のホットリロード(F5 / 自動検出 / rpc)に対応します。リロードは v1 では **full-reset 方式**です — 差分を当てるのではなく、物理 → ECS → モデルインスタンス → 遅延ミューテーション → 未配送イベントの順に破棄し([runtime_teardown_order](../../src/core/appflow/teardown.hpp#L23))、現在のシーンを起動時と同じロード経路で組み直します([rebuildCurrentScene](../../src/core/gamelogic/gamelogicreload.cpp#L897))。C++ のオブジェクトは DLL ごと入れ替わるので、システムや behavior のメンバ変数に持っていた値は残りません。詳細は [第10章](10_tools.md) §10.5。

> **設計決定(レイヤ規則):** ゲームコードが include してよいのは `src/core/userpublic/` のヘッダのみ。エンジン内部のモジュール取得(`GET_MODULE`)をゲームコードから直接呼ぶことは禁止で、入力・時刻・オブジェクト・物理・音・シーン・乱数はすべて **`GameContext`** ファサードを通す。

ビルドの組み込み方([第2章](02_getting_started.md) §2.4):`code/CMakeLists.txt` に `pelican_game_sources(<ソース>...)` と書き、CMake configure 時に `-DPELICAN_PROJECT=<プロジェクトパス>` を渡します。

### ゲーム DLL の ABI(✅WP381/381a/382a)

ふだん意識する必要はありませんが、**古い DLL が黙って動く事故を防ぐ**ための仕掛けがあるので、エラーが出たときのために書いておきます。

- ゲーム DLL がホスト実行ファイルから import する登録入口は `pelican_register_behavior_v2` / `pelican_register_event_v2` / `pelican_register_system_v2` / `pelican_emit_v2` と、コンポーネントの `__registerComponent` です。加えて **epoch anchor** [`pelican_game_logic_epoch_v2`](../../src/core/userpublic/gamelogic.hpp#L23) を必ず import します(SDK が自動で埋めます)。epoch anchor とは、ABI の世代番号を名前そのものに含んだ関数シンボルのことです([用語集](14_glossary.md))。世代が上がると名前ごと変わるため、古い SDK でビルドされた DLL は「host に存在しない名前を import している DLL」として機械的に見分けられます。
- ロードの**前**に、DLL の PE import テーブルだけを読む診断([diagnoseGameLogicImports](../../src/core/gamelogic/gamelogicimports.cpp#L559))が走ります。期待する epoch anchor が無い / 旧名の入口(`__registerBehavior` など)が残っている DLL は [`game_logic_abi_stale@1`](../../src/core/gamelogic/gamelogicimports.hpp#L40) で拒否され、**一度も実行されません**。ordinal import と delay-load import も `game_logic_abi_unsupported_import@1` で拒否です。`game_logic_abi_stale@1` のような `名前@数字` は**エラーコード**で、人間向けのメッセージ本文とは別に、rpc の応答へそのまま出る機械可読な識別子です([用語集](14_glossary.md))。
  - registration を持たない provider 専用の古い DLL には、リンカが強制する要素が無いため、**この診断が唯一の門番**です。
- **再帰 schema**(フィールドの中にさらにフィールドを持つ、木になった schema)を宣言した型があると、SDK は `_v3` 入口([registerer.hpp](../../src/core/userpublic/details/behavior/registerer.hpp#L322))を**自動で選びます**。v2 の入口は平らなフィールド列しか運べないためです。判定は「`List` か `Flags` のフィールドが 1 つでもあるか」だけで([schemaRequiresV3](../../src/core/userpublic/details/schema/structfieldschema.hpp#L823))、§8.5.3 の `List<Row>` は行の型 `Row` が自分の `schema` を持つのでこれに当たります。書き手のコードは変わりません。v3 入口を使う DLL と v2 だけの DLL は同じホストに**同居できます**。
- 逆に、ホストが古くて v3 入口を持たないのに DLL が v3 を使っていると、`game_logic_abi_stale@1{found: "v3", host: "v2"}` で拒否されます。

実行中の DLL は原本ではなく **shadow copy**(別ディレクトリへ取った複製)を読みます。リロードのたびに連番付きの複製を作ってそちらをロードするので([makeShadowCopy](../../src/core/gamelogic/gamelogicreload.cpp#L373)。`.pdb` も一緒に複製するのでデバッガも繋がります)、エンジンが原本のファイルを掴んだままにならず、走らせたままビルドし直せます。置き場所は原本の隣の `.pelican-hot-reload/<プロセス ID>/` で([gamelogicreload.cpp](../../src/core/gamelogic/gamelogicreload.cpp#L501))、**プロセスごとに別ディレクトリ**なので同じプロジェクトを 2 つ起動しても取り合いになりません(死んだプロセスの残骸は起動時に掃除されます)。ビルドが原本を掴んだままだと、リロードは失敗ではなく保留になり、何度ポーリングしても掴まれたままなら [`game_logic_source_busy@1`](../../src/core/gamelogic/gamelogicreload.cpp#L787) が status に出ます。

エラーコードは rpc の `get_status` の reload 節と `reload_game_logic` のエラーに出ます([第10章](10_tools.md) §10.5)。

## 8.2 最小の実例 — WASD でキャラクターを動かす

[projects/example/code/playercontrol.cpp](../../projects/example/code/playercontrol.cpp)(実物・全文)がそのまま最小デモです:

```cpp
#include <components/modelview.hpp>
#include <components/predefined.hpp>
#include <gameobjects.hpp>
#include <gamesystem.hpp>

namespace {

class ExamplePlayerControl {
    Pelican::GameObjectId object = Pelican::invalidGameObjectId;
    bool object_created = false;

    Pelican::GameObjectId createControlledObject(Pelican::GameContext &ctx) {
        Pelican::LocalTransformComponent transform{
            .scale = Pelican::vec3{0.35f, 0.35f, 0.35f},
            .rotation = Pelican::quat{0.0f, 0.7071f, -0.7071f, 0.0f},
            .pos = Pelican::vec3{0.0f, -1.2f, 1.0f},
            .parent = Pelican::invalidGameObjectId,
        };
        Pelican::SimpleModelViewComponent model_view;
        model_view.model_name = "character";

        const auto id = Pelican::GameObjects::add()
                            .addComponent<Pelican::TransformComponent>()
                            .addComponent<Pelican::LocalTransformComponent>(transform)
                            .addComponent<Pelican::SimpleModelViewComponent>(model_view)
                            .finish();
        (void)ctx.setLocalTransform(id, transform);
        return id;
    }

  public:
    void update(Pelican::GameContext &ctx) {
        if (!object_created) {
            object = createControlledObject(ctx);
            object_created = true;
        }

        if (!ctx.actionsConfigured()) {
            return;
        }

        const auto move = ctx.actionAxis2("move");
        if (move.x == 0.0f && move.y == 0.0f) {
            return;
        }

        auto transform = ctx.localTransform(object);
        const auto dt = static_cast<float>(ctx.deltaTime() > 0.0 ? ctx.deltaTime() : 1.0 / 60.0);
        constexpr float speed = 2.5f;
        transform.pos.x += move.x * speed * dt;
        transform.pos.z += move.y * speed * dt;
        (void)ctx.setLocalTransform(object, transform);
    }
};

} // namespace

PELICAN_REGISTER_SYSTEM(ExamplePlayerControl, 100);
```

ポイント:

- `dt` は delta time(前フレームからの経過秒)。`ctx.deltaTime()` は最初のフレームや rpc `set_time` の直後に 0 になり得るので([EngineTime::setTime](../../src/core/appflow/enginetime.cpp#L63) が 0 に戻します)、その場合は 1/60 秒で代用しています。
- `ctx.actionsConfigured()` ガード — `actions.json` を持たないプロジェクトでも壊れない作法です(ビルトインのカメラコントローラも同じ作法)。
- `GameObjects::add()...finish()` ビルダーでオブジェクトを生成し、以後は **`GameObjectId` だけを保持**します(ポインタ保持は禁止 — [第4章](04_scene_ecs.md) §4.4)。
- `PELICAN_REGISTER_SYSTEM` は**型の完全定義より後**(ファイル末尾)に置きます。※設計文書のコード例は定義前に書いていますが、マクロが型を実体化するためコンパイルできません。実物が正です。

## 8.3 ゲームシステムの登録と実行順序(✅WP43)

```cpp
PELICAN_REGISTER_SYSTEM(Type, order);
```

- システム型は `void update(GameContext&)` と `void onEvent(const E&, GameContext&)` の**どちらか(または両方)**を持つ必要があります(どちらも無いと登録時エラー)。
- 実行点は毎フレーム「ECS システム群の後・描画の前」。
- **順序は order 昇順 → 同値は型名の辞書順**。静的初期化順に依存しない決定的順序です。
- behavior 全体は **`"BehaviorSystem"`(order = 50)という 1 個のシステム**としてこの順序に参加します(§8.5)。`order < 50` のシステムは全 behavior より前、`> 50` は後に走ります。
- インスタンスは関数ローカル static のシングルトンで、**シーン切替をまたいで生存**します(§8.7 の落とし穴参照)。
- エンジン組み込みのカメラコントローラは order **10000** で登録されています。ゲームが order < 10000 を使えば、カメラ追従はそのフレームのゲーム更新後の transform を見ます。

### フレームループの固定順序(決定性の土台)

```
入力スナップショット確定 → 時刻 advance → イベント配送(前フレーム分)
→ ECS システム → ゲームシステム(order 順) → SeqPlayer
→ シーン遷移の適用 → 描画
```

rpc の `step_frame` も**同一順序**を再現します。「rpc 駆動のテスト = 実行時と同じ意味論」がこれで保証されます(`step_frame` は headless 専用で、窓ありでは `step_frame_windowed` で断られます。✅PM-1a)。一時停止中のフレームでは、この順序のうちイベント配送・ゲームシステム・シーン遷移の適用が飛ばされます(§8.4 の「時刻の 3 つの値と一時停止」)。

## 8.4 GameContext API 一覧

`GameContext` は状態を持たない薄いファサードです([gamecontext.hpp](../../src/core/userpublic/gamecontext.hpp))。面は「根 + 8 つのドメインファサード」に分かれています — 根には入力(アクション)・時刻・ログ・オブジェクト・乱数・シーン・イベント・設定が残り、物理 / 音 / アニメーション / セーブ / 描画 / ポインタとカーソル / シーンオブジェクトの名前解決 / UI は [`physics()`](../../src/core/userpublic/gamecontext.hpp#L357) / [`audio()`](../../src/core/userpublic/gamecontext.hpp#L358) / [`animation()`](../../src/core/userpublic/gamecontext.hpp#L359) / [`save()`](../../src/core/userpublic/gamecontext.hpp#L360) / [`render()`](../../src/core/userpublic/gamecontext.hpp#L361) / [`input()`](../../src/core/userpublic/gamecontext.hpp#L362) / [`objects()`](../../src/core/userpublic/gamecontext.hpp#L363) / [`ui()`](../../src/core/userpublic/gamecontext.hpp#L364) 経由です。ファサードはどれも**状態を持たない空の値**(`ui(document_key)` の返す `UiContext` だけは文書の key を view で持つので、key の文字列を使い終わるまで生かしておきます)なので、`ctx.audio().playSound(...)` は 1 回の呼び出しであって検索ではありません。

### 根 `GameContext`([宣言](../../src/core/userpublic/gamecontext.hpp#L284))

| カテゴリ | API | 備考 |
|---|---|---|
| 入力 | `actionsConfigured()` / `actionPressed/Released/Held(name)` / `actionAxis1(name)` / `actionAxis2(name)` / `actionPose(name)` | 未知アクション名は名前入り例外。`actionPose` は ✅WP132 で実動作(下記) |
| 時刻 | `time()` / `deltaTime()` / `frameIndex()` | |
| ログ | `logInfo/Warning/Error(msg)` | rpc モードでは stdout が使えないため、ログは必ずこれで |
| オブジェクト | `createObject(LocalTransformComponent)` / `removeObject(id)`→bool / `localTransform(id)` / `setLocalTransform(id, t)`→bool | `setLocalTransform` は描画側へも即時伝播。ただし親の位置を掛けたワールドは次の transform 更新で揃う(下の「objects()」)。`transform` を持つシーンオブジェクトには localtransform が必ず付くので、親子の無いシーンでも `true` を返す(✅GP-1) |
| 実行時生成 | [`spawn(prefab, SpawnParams)`](../../src/core/userpublic/gamecontext.hpp#L326)→`TransientObjectRef` / [`destroy(ref)`](../../src/core/userpublic/gamecontext.hpp#L334)→`Resolved<TransientObjectRef>` | ✅GP-2。単一 object の prefab から collider・behavior 付きの object を作り、消す。実体化と破棄は次のフレーム境界。`spawn` の拒否は `SpawnError` を投げる(`code()` で名前)。下の「実行時生成 — `ctx.spawn`」 |
| スプライト | `createSpriteObject(transform, sprite)` / `spriteView(id)`→optional / `setSpriteView(id, s)`→bool / `setSpriteTexture(id, tex)`→bool | §8.14(✅WP103) |
| 乱数 | `random()` / `randomInt(min,max)` / `randomFloat(min,max)` / `setSeed(u64)` / `seed()` | §8.10 |
| シーン | `loadScene(name)` / `currentScene()` | §8.7 |
| イベント | `emit(const Event&)` | §8.6 |
| 設定 | `gameSettings()` / `setGameSettings(json)` / `saveSettings()` | §8.12(✅WP65)。**ゲーム設定はセーブデータではない**ので根に残っています |

### 時刻の 3 つの値と一時停止(✅PM-1a)

engine は RPC の `set_play_state` で一時停止でき、`advance_frames` で 1 歩ずつ進められます(契約は [第13章](13_editor.md) §13.5 の「一時停止と 1 歩ずつ進める」)。ゲームコードから見た 3 つの値の意味は次のとおりです。

| API | 意味 | 一時停止の前後 |
|---|---|---|
| [`ctx.time()`](../../src/core/userpublic/gamecontext.cpp#L70) | game の時計(秒) | 一時停止の間は止まり、再開すると止まった所から続く。**飛ばない** |
| [`ctx.deltaTime()`](../../src/core/userpublic/gamecontext.cpp#L78) | game の時計がこのフレームで進んだ秒 | 再開の最初のフレームに一時停止していた時間は入らない。`advance_frames` の 1 歩では `1/fps`(既定 60 fps) |
| [`ctx.frameIndex()`](../../src/core/userpublic/gamecontext.cpp#L89) | **評価の番号**。描画のフレームごとに 1 増え、一時停止中も増える | 再開の後、止まっていたフレームの数だけ**飛ぶ** |

- **一時停止の間、ゲームコードは走りません。** game の system(`PELICAN_REGISTER_SYSTEM`)・behavior・イベントの配送・game DLL が登録した animation の phase は呼ばれず、`ctx.spawn` / `destroy` と `ctx.loadScene` の要求は再開(か次の 1 歩)まで待ちます。だから一時停止中に `deltaTime()` が 0 になるのを見ることはありません
- `ctx.frameIndex()` が game の歩数ではなく評価の番号なのは、animgraph の [`prepareTick()`](../../src/core/userpublic/animation/animgraph.cpp#L1150) に渡す revision を、engine が phase を走らせる revision と一致させるためです(一致しないと phase が `invalid_argument` で断ります)。**game の歩数を数えたいときは `frameIndex()` の差を使わず、自分で数えるか `time()` を使ってください**。0 から数える game の歩数は RPC の `get_status` の `play.game_frame` でだけ読めます
- 一時停止しない実行(今までの起動のまま)では、評価の番号と game の歩数は同じ値で進みます
- 組み込みの camera controller と modal transform は engine 内部で editor の system に印が付いていて、一時停止中も動きます。このとき `deltaTime()` は game の時計ではなく、そのフレーム自身の `dt` を返します。ゲームの system にこの印を付ける公開の口はありません

### ドメインファサード

| ファサード | API | 備考 |
|---|---|---|
| [`physics()`](../../src/core/userpublic/gamecontext.hpp#L357) → [`PhysicsContext`](../../src/core/userpublic/gamecontext.hpp#L49) | `raycastAll(ray, filter)` / `raycastClosest(ray, filter)` / `overlapAll(shape, filter)` / `shapeCastAll(shape, delta, filter)` / `shapeCastClosest(shape, delta, filter)` | §8.8。`filter` は省略可。`PELICAN_WITH_PHYSICS` 無しビルドでは全問い合わせが `BuildFeatureCallDisabledError`(`build_feature_disabled@1`)、provider が 0 本のビルドでは `phys::ProviderUnavailableError`(`physics.provider_unavailable@1`)。どちらも「何にも当たらなかった」とは答えません(下の「機能を外した build」) |
| [`audio()`](../../src/core/userpublic/gamecontext.hpp#L358) → [`AudioContext`](../../src/core/userpublic/gamecontext.hpp#L84) | `playSound(path)` / `playSound(path, bus)`→SoundHandle / `stopSound(h)` / `setBusVolume(bus,v)` / `busVolume(bus)` / `isPlaying(h)` / `setSoundVolume(h,v)` / `setSoundPitch(h,p)` / `setSoundPaused(h,b)` / `soundState(h)`→optional | §8.11。`PELICAN_WITH_AUDIO` 無しビルドは `BuildFeatureCallDisabledError`(`build_feature_disabled@1`。下の「機能を外した build」) |
| [`animation()`](../../src/core/userpublic/gamecontext.hpp#L359) → [`AnimationContext`](../../src/core/userpublic/gamecontext.hpp#L114) | `setParameter(object, name, value)`→`Animation::Status` / `forceState(object, state)`→`Animation::Status` | **`animation` コンポーネントの `graph`**(`pelican.anim_graph`)に装着された評価器を動かす。初回装着の前と取り外し後は `not_found`、シーンのロード/破棄トランザクション中は `phase_order_error`([宣言](../../src/core/userpublic/gamecontext.hpp#L116)) |
| [`save()`](../../src/core/userpublic/gamecontext.hpp#L360) → [`SaveContext`](../../src/core/userpublic/gamecontext.hpp#L123) | `saveData(slot, json)` / `loadData(slot)`→optional / `listSaves()`→`SlotInfo` 列 | §8.12(✅WP65) |
| [`render()`](../../src/core/userpublic/gamecontext.hpp#L361) → [`RenderContext`](../../src/core/userpublic/gamecontext.hpp#L138) ライト | `setDirectionalLightDirection(name, dir)` / `setDirectionalLightIntensity(name, f)` / `setPointLightPosition(name, pos)` / `setSpotLightDirection(name, dir)` — いずれも→bool | ✅WP142。[第4章](04_scene_ecs.md) §4.2 の light をオブジェクト `name` で指す。未知名は例外ではなく **false** が返る |
| `render()` カメラ | `setCamera(name)` | §8.9 |
| `render()` 値グラフ | `setValueGraphParameter(object, parameter, value)`→bool / `emitValueGraphEvent(object, input, command)`→bool / `setRenderValues(buffer, values)`→bool | **共通グラフ(pass 文書)の `graph.instance`** の入力を実行時に上書きする。`std::nullopt` で上書きを取り消す。edge が供給しているポートを狙うと `graph.override_shadowed@1`([宣言](../../src/core/userpublic/gamecontext.hpp#L155)。[第6章](06_rendering.md) §6.17) |
| `render()` HUD | `debugText(x, y, text, color, scale)` | colorはRGBA、scaleは整数倍率。省略時は白・等倍。debug_text feature 未参照時は no-op([第7章](07_input_ui.md)) |
| `render()` レイトレーシング | `setRayTracingCallableRecord(task, index, values)` / `setRayTracingDispatchExtent(task, w, h, d)` | ゲーム/更新スレッドから呼ぶ。値は task の `callable_record` schema に合う必要があり、不正な更新は前の値を残す |
| `render()` 画面の点から ray | [`screenRay(position_px)`](../../src/core/userpublic/gamecontext.hpp#L174)→`Resolved<phys::Ray, RenderError>` | ✅GP-4。framebuffer ピクセルの位置を通る world の ray。失敗は例外ではなく `render.screen_ray_unavailable@1`(XR が有効)/ `render.screen_position_invalid@1`(有限でない値)。下の「input() と screenRay」 |
| [`input()`](../../src/core/userpublic/gamecontext.hpp#L362) → [`InputContext`](../../src/core/userpublic/gamecontext.hpp#L244) | `pointer()`→[`PointerState`](../../src/core/userpublic/userinput.hpp#L143) / `setCursorMode(mode)`([`CursorMode`](../../src/core/userpublic/userinput.hpp#L151) = `normal` / `hidden` / `locked`) | ✅GP-4。ポインタの位置を framebuffer ピクセルで読み、カーソルの表示とロックを頼む。ボタンと軸はこれまでどおりアクション(根の「入力」)で読む。下の「input() と screenRay」 |
| [`objects()`](../../src/core/userpublic/gamecontext.hpp#L363) → [`ObjectsContext`](../../src/core/userpublic/gamecontext.hpp#L214) | `resolve(name)`→`Resolved<ObjectRef>` / `materialize(ref)`→`Resolved<GameObjectId>` / `worldTransform(ref)`→`Resolved<WorldTransform>` | ✅GP-1。シーンオブジェクトを名前で引き、ワールド変換を読む。失敗は例外ではなく名前付きエラー(下の「objects()」)。`materialize` / `worldTransform` は `spawn` が返した `TransientObjectRef` も受ける([多重定義](../../src/core/userpublic/gamecontext.hpp#L219)。✅GP-2) |
| [`ui()`](../../src/core/userpublic/gamecontext.hpp#L364) / `ui(document_key)` → [`UiContext`](../../src/core/userpublic/gamecontext.hpp#L271) | `setText(path, text)` / `setVisible(path, bool)` / `setValue(path, float)` → いずれも [`UiWriteResult`](../../src/core/userpublic/ui_write.hpp#L39) | ✅GP-3。UI 文書の node を path(root からの `id` を `/` でつないだもの)で指して書く。text は label / button、value は gauge、visible は全 node。書き込みはフレーム境界で適用。失敗は例外ではなく `ui.feature_unreferenced@1` / `ui.node_unknown@1` / `ui.property_mismatch@1` / `ui.sink_conflict@1`(この順で先のもの)。`ui.*` sink が書いている (path, 性質) はグラフのもので、ここからは書けない([第7章](07_input_ui.md) §7.4「ゲームコードと共通グラフから UI を書く」) |

> **この 3 本は「アニメーションの API」ですが、対象が 2 つに分かれていて互いに委譲しません(✅AG-1b 決定 5)。ファサード分割もその線を引き直しただけで、行き先は変わっていません。** `render().setValueGraphParameter` が触るのは **pass 文書の `graph.instance`** の入力ポート、`animation().setParameter` / `animation().forceState` が触るのは **シーンの `animation` コンポーネント**が持つ評価器です(前者は [gamecontext.cpp:406](../../src/core/userpublic/gamecontext.cpp#L630) で `SceneLoader` へ、後者は [:319](../../src/core/userpublic/gamecontext.cpp#L535) / [:329](../../src/core/userpublic/gamecontext.cpp#L545) で `AnimationSystem` へ行きます)。**姿勢の装着様式が 2 つある**(§8.13)ことがそのまま API に出ています。同じ owner に両方は付かないので、「どちらを呼ぶか」は装着の仕方で決まります。
>
> 同じ `graph.instance` 側には `render().emitValueGraphEvent(object, input, command)` もあります。`input` は **`event.playback` ノードの(展開後の)id** で、そこから `value.event_clock` / `audio.playback` の `events` ポートへ再生制御イベントが流れます([valuegraph.cpp:302](../../src/core/userpublic/valuegraph.cpp#L469))。
>
> 一方、**姿勢の評価器そのものは `GameContext` に載っていません** — 評価器を載せると標準の評価器だけが特権を持ってしまい、「評価器ごと自作できる」という設計が空文になるためです([../design_animation_graph.md](../design_animation_graph.md))。自作評価器は凍結 C-ABI 経由です(§8.13)。

### 機能を外した build でゲームコードの呼び出しが返すもの(✅PG-2 / PG-2b)

ビルドユニット([第2章](02_getting_started.md) の「機能ユニット」)を外した build でも、`GameContext` の関数はすべて残ります。呼んだときの答えは、関数が値を返すか `Status` を返すかで 2 通りに分かれます。値を返す facade(audio / physics)は例外を 1 種類だけ投げ、`Status` を返す facade(animation)は外した build でも `Status` で答えます([規則のコメント](../../src/core/userpublic/gamecontext.hpp#L109))。ただし `noexcept` の `onDestroy` の中だけは、audio / physics も投げずに空で答えます(✅PG-2b。表の「`onDestroy` の中」の 2 行)。

| 呼び出し | build | 返るもの |
|---|---|---|
| `ctx.audio()` の 9 関数(`playSound` の 2 つの overload を含む) | `PELICAN_WITH_AUDIO=OFF` | [`BuildFeatureCallDisabledError`](../../src/core/build_features.hpp#L45) を投げる。`code()` = `build_feature_disabled@1`、`feature()` = `"PELICAN_WITH_AUDIO"`、`what()` = `This binary was built with PELICAN_WITH_AUDIO=OFF: playSound is unavailable`(呼んだ関数名が入る) |
| 同上を `onInit` の中で呼ぶ | `PELICAN_WITH_AUDIO=OFF` | `playSound` / `stopSound` / `setBusVolume` / `busVolume` / `isPlaying` は同じ例外を投げる。`setSoundVolume` / `setSoundPitch` / `setSoundPaused` は build を見る前に `false` を返す(ロード中は ON の build でも `false`) |
| 同上を `onDestroy` の中で呼ぶ | `PELICAN_WITH_AUDIO=OFF` | 9 本とも例外にならず、空で答える: 無効な `SoundHandle` / `false` / `0.0` / `soundState` は `nullopt`(✅PG-2b。[`soundState` の包み](../../src/core/userpublic/gamecontext.cpp#L673))。拒否は記録に残る: scene のロード/破棄トランザクションの中ではロード結果の `effect_errors` に `build_feature_disabled@1`(`phase` は `destroy`)、トランザクションの無い `onDestroy`(プレイ中の object 削除、ゲーム DLL の解放、`deactivateAll`)では engine log に `build_feature_disabled@1 in a destruction callback: …`。トランザクションの中の `setSoundVolume` / `setSoundPitch` / `setSoundPaused` は、ON の build と同じく build を見る前に `false` を返すので何も記録されない |
| `ctx.physics()` の 5 本を `onDestroy` の中で呼ぶ | `PELICAN_WITH_PHYSICS=OFF`、または provider が 0 本 | 例外にならず、空で答える: `raycastAll` / `overlapAll` / `shapeCastAll` は空の配列、`raycastClosest` / `shapeCastClosest` は `nullopt`(✅PG-2b)。これは「何にも当たらなかった」と同じ答えで、見分けられるのは記録だけです。記録は audio と同じ場所(トランザクションの中なら `effect_errors`、無ければ engine log)に、`build_feature_disabled@1` か `physics.provider_unavailable@1` で残る |
| `ctx.physics()` の 5 本 | `PELICAN_WITH_PHYSICS=OFF` | `BuildFeatureCallDisabledError`。`code()` = `build_feature_disabled@1`、`feature()` = `"PELICAN_WITH_PHYSICS"`。rpc の `raycast` は JSON-RPC の error(code `-32000`、`data` = `{code: "physics.module_unavailable@1", requirement: "physics", option: "PELICAN_WITH_PHYSICS"}`。✅PG-2b。[受け口](../../src/core/communication/rpcserver.cpp#L1153)) |
| `ctx.physics()` の 5 本 | `PELICAN_WITH_PHYSICS=ON` で provider が 0 本(`PELICAN_WITH_BUILTIN_PHYSICS` と `PELICAN_WITH_JOLT_PHYSICS` が両方 OFF) | ゲーム DLL がそのクエリの provider を登録していなければ [`phys::ProviderUnavailableError`](../../src/core/phys/physquery.hpp#L102) を投げる。`code()` = `physics.provider_unavailable@1`。空の結果(「何にも当たらなかった」)は返さない。rpc の `raycast` も同じ場合は JSON-RPC の error(code `-32000`、message はこの例外の `what()`)を返す |
| `ctx.animation().setParameter` / `forceState` | `PELICAN_WITH_ANIMATION=OFF` | 例外ではなく `Animation::Status::not_found`。シーンのロード/破棄トランザクションの中では、ON の build と同じく `phase_order_error` |

- `BuildFeatureCallDisabledError` は `BuildFeatureDisabledError` の派生です。PG-2 より前から `BuildFeatureDisabledError` や `std::runtime_error` で受けていたコードは、そのまま受け取れます。名前で分岐したいときは `code()` を見てください。型の定義は [build_features.hpp](../../src/core/build_features.hpp#L45)、`phys::ProviderUnavailableError` は `gamecontext.hpp` が include している `phys/physquery.hpp` にあります。
- provider 0 本の build では、トリガー更新は例外を投げません。`OverlapEnter` / `OverlapExit` を出さず、それまでの overlap の組をそのまま持ち続けます([physworld.cpp のコメント](../../src/core/phys/physworld.cpp#L564))。
- `PELICAN_WITH_PHYSICS=OFF` の build の rpc `raycast` は、PG-2b から stub の [`PhysWorld::raycastClosest()`](../../src/core/phys/physworld_stub.cpp#L108) に問い合わせず、params を見る前に上の JSON-RPC error で断ります。PG-2b より前は stub の空の答えがそのまま `{"hit": null}` になり、「外れ」と区別できませんでした。
- activate 相(`onInit`)と、ロードの外の通常の呼び出しは、PG-2b の後も上の表のとおり投げます。空で答えるのは `onDestroy` の中だけです([規則のコメント](../../src/core/userpublic/gamecontext.hpp#L43))。
- 残した穴: `render()` の `setRayTracingDispatchExtent` / `setRayTracingCallableRecord` は、scene のトランザクション中の呼び出しを `SceneTransactionMutationRejected` で投げます([トランザクション中の拒否](../../src/core/userpublic/gamecontext.cpp#L298))。PG-2b の包みの外なので、ロード中の `onDestroy` から呼ぶと `std::terminate` になりえます(コードからの推論。実行では未確認)。
- `animation` の OFF build の挙動は凍結 ABI の契約です(§8.13「`PELICAN_WITH_ANIMATION=OFF` の見え方」)。

### objects() — シーンオブジェクトを名前で引いてワールドを読む(✅GP-1)

公開型は [object_ref.hpp](../../src/core/userpublic/object_ref.hpp) にあり、`gamecontext.hpp` が取り込んでいます。手順は「名前で解決 → 実行時オブジェクトにする → ワールドを読む」の 3 段です。

```cpp
void update(Pelican::GameContext &ctx) {
    // 1. 名前を ObjectRef にする。resolve はシーンの宣言を先頭から探すので、
    //    ref を保持すれば検索は 1 回で済む(ただしシーンが変わったら引き直す)
    const auto hero = ctx.objects().resolve("Hero");
    if (!hero) {
        ctx.logWarning(hero.errorName());   // 例: object.not_materialized@1
        return;
    }

    // 2. ワールド変換を読む(pos / rotation / scale)
    if (const auto world = ctx.objects().worldTransform(hero.value())) {
        const Pelican::vec3 pos = world.value().pos;
        // ...
    }

    // 3. 書き込みには GameObjectId が要る
    if (const auto object = ctx.objects().materialize(hero.value())) {
        auto local = ctx.localTransform(object.value());
        local.pos.y += 1.0f;
        (void)ctx.setLocalTransform(object.value(), local);
    }
}
```

`Resolved<T, E>`([宣言](../../src/core/userpublic/object_ref.hpp#L156))は「値、または値が無い理由の名前」です。`ok()` / `if (r)` で判定し、`get()` は値が無ければ null、`value()` は値が無ければエラー名で始まる `std::logic_error` を投げます。`errorName()` は値があるとき空です。`E` はそのファサードのエラーの enum で、既定は `ObjectError` です。`objects()` の 3 本は `Resolved<T>`(= `Resolved<T, ObjectError>`)を、`render().screenRay` は `Resolved<phys::Ray, RenderError>` を返します(✅GP-4 で `E` を足しました。GP-1 の `Resolved<T>` はそのまま書けます)。エラー名は enum ごとの [`errorCodeName()`](../../src/core/userpublic/object_ref.hpp#L149) から引きます。

| エラー名(`ObjectError`) | いつ出るか |
|---|---|
| `object.stale_authority@1`(`stale_authority`) | ref を解決した後にシーンがロードされた。**同じシーンの再ロードでも出ます**(宣言 ID が同じでも、別の実行とみなして読みません)。既定構築の `ObjectRef{}` もこれ |
| `object.not_materialized@1`(`not_materialized`) | シーンに宣言されていない名前 / 宣言はあるが実行時のオブジェクトが無い(削除された、ライトだけのオブジェクトなど) |
| `object.missing_capability@1`(`missing_capability`) | オブジェクトは生きているが、`worldTransform` が読む `transform` を持たない |

複数当てはまるときは表の上のものが返ります。名前は [`objectErrorName()`](../../src/core/userpublic/object_ref.hpp#L138) でも引けます。

注意点:

- **ref はシーンをまたいで使えません。** シーンを切り替えた(または読み直した)ら、`SceneLoaded` の `onEvent` などで `resolve` し直してください(§8.7 の落とし穴と同じ理由です)。
- **シーンのロード中(behavior の `onInit` を含む)に `resolve` すると、ロード中の新しいシーンで解決します。** `onInit` で解決した ref は、ロードが終わった後もそのまま使えます。
- 同じ名前が 2 つあれば宣言順で先のものが選ばれますが、シーン文書が同名を拒否する([第4章 §4.1](04_scene_ecs.md))ので、ロードできたシーンでは起きません。
- **`worldTransform` は「今の」ワールド変換を読みます。** ワールドは毎フレームの transform 更新で local から作り直されます。`setLocalTransform` はそのオブジェクトのワールドを即座に書きますが、親が無いものとして書くので、**親を持つオブジェクトや、動かしたオブジェクトの子を直後に読むと、次の transform 更新まで 1 フレーム前の値**になります。親の無いオブジェクト自身はすぐ正しく読めます。
- **ワールド変換(`TransformComponent`)を直接書いても、次のフレームで local の値に戻ります。** 位置は `setLocalTransform` で変えてください([第4章 §4.4](04_scene_ecs.md))。
- `load_gltf` で置いた一時オブジェクト(transient)を指す ref はまだ出しません。ゲームコードが `spawn` で作った object は、`ObjectRef` ではなく `TransientObjectRef` で指します(次の節)。

### 実行時生成 — `ctx.spawn`(✅GP-2)

弾や破片のように、シーンに書いていない object をゲームの途中で作るときは [`ctx.spawn(prefab, params)`](../../src/core/userpublic/gamecontext.hpp#L326) を使います。作れるのは **object を 1 つだけ持つ prefab** から、collider と behavior を含めて 1 つです。`createObject` は transform だけの entity を作る口で、collider も behavior も付きません。

prefab は project.json の `prefabs` に `{"name", "path"}` で登録した `pelican.prefab` 文書です。transform は prefab に書かず、`spawn` の引数で渡します。

```json
{
  "schema": "pelican.prefab", "version": 1, "name": "bullet",
  "parameters": [ { "name": "tag", "type": "string", "default": "none" } ],
  "components": [
    { "name": "collider", "shape": "sphere", "radius": 0.1 },
    { "name": "behavior", "type": "bullet_motion", "params": { "tag": { "$param": "tag" } } }
  ]
}
```

弾を撃って、次のフレームで位置を読み、当たったら消す例です。

```cpp
#include <gamesystem.hpp>
#include <optional>

namespace {

class Gun {
  public:
    void update(Pelican::GameContext &ctx) {
        if (ctx.actionPressed("fire") && !bullet) {
            Pelican::SpawnParams params;
            params.transform.pos = Pelican::vec3{0.0f, 1.0f, 0.0f};
            params.parameters = {{"tag", "player"}};   // prefab の parameter を名前で。省いたものは既定値
            try {
                bullet = ctx.spawn("bullet", params);  // ここではまだ object は無い
            } catch (const Pelican::SpawnError &error) {
                ctx.logWarning(error.code());          // 例: physics.module_unavailable@1
            }
            return;
        }
        if (!bullet) return;
        // 次のフレームからは読める。同じフレームのうちは object.not_materialized@1
        const auto world = ctx.objects().worldTransform(*bullet);
        if (!world) {
            if (world.error() == Pelican::ObjectError::stale_authority) bullet.reset();  // シーンが替わった
            return;
        }
        if (world.value().pos.z > 50.0f) {
            (void)ctx.destroy(*bullet);                // 消えるのは次のフレーム境界
            bullet.reset();
        }
    }

  private:
    std::optional<Pelican::TransientObjectRef> bullet;
};

} // namespace

PELICAN_REGISTER_SYSTEM(Gun, 100);
```

**フレーム境界の規則**

- 要求は**呼んだ時点で検査して展開**します。prefab はシーンのロードと同じ ResolvedScene の経路で展開するので、prefab・parameter・build の機能の検査もロードと同じ答えになります。例えば `PELICAN_WITH_PHYSICS=OFF` の build で collider を持つ prefab を spawn すると、ロードと同じ `physics.module_unavailable@1` で拒否されます([検査の呼び出し](../../src/core/loader/runtimespawn.cpp#L238))。拒否されたときは何も積まれません。
- **object ができるのは次のフレーム境界**です。境界はフレームの先頭、ゲームコードより前にあります([`updateFrameState()` の中の境界の呼び出し](../../src/core/appflow/framephase.cpp#L173)。イベント配送・ECS・ゲームシステムより前)。behavior のコールバックの中から呼んでも外から呼んでも同じ境界です。一時停止中のフレームは境界を通らないので、着地は再開か `advance_frames` の 1 歩のフレームです(✅PM-1a)。
- 境界より前に `materialize` / `worldTransform` を呼ぶと `object.not_materialized@1` です。境界の後のフレームでは、そのフレームのゲームコードの全部から見えます。
- 1 つの境界の中では、要求を**呼んだ順**に実行します。spawn と destroy も混ぜて呼んだ順です。まだ実体化していない spawn を destroy すると、同じ境界で作ってから消します(`onInit` の後に `onDestroy`)。
- 境界で起きること: ECS の entity と描画のインスタンスを作り、collider(と model の衝突 mesh)を物理に `runtime_only` で登録し、最後に behavior を付けて `onInit` を呼びます。`onInit` が例外を投げたら、**その spawn だけ**を巻き戻します(init 済みの behavior に `onDestroy`、entity・collider・描画を戻す)。ほかの spawn とシーンには影響しません。失敗は engine log に `runtime spawn of boundary … failed: …` と出て、ref はずっと `object.not_materialized@1` のままです。
- `destroy` は、behavior の `onDestroy`(後に付いたものから)→ 物理から collider を外す → ECS の entity を消す、の順です([`retire()`](../../src/core/loader/runtimespawn.cpp#L449))。
- `name` を渡すと、その object の collider に raycast が当たったときの名前(`hit->id`)になります。渡さなければ engine が `spawn:<境界>.<番号>` を付けます。
- `name` はシーン形式の object 名と同じ規則(`[a-zA-Z0-9_]`)で検査されます(prefab の展開がシーン形式を通るため。合わなければ `object.spawn_invalid@1`)。
- **engine が名前の無い object に付ける名前の接頭辞 `spawn:` と `pelican://` は予約です**(✅GP-2b)。名前の無い spawn は `spawn:<境界>.<番号>`、名前の無い authored object は `pelican://scene/<scene>/authoring-object/<n>` になり、どちらも物理の collider 名と同じ名前の空間に入ります。渡した `name` がこのどちらかで始まると、呼んだ時点で `object.name_reserved_by_engine@1` で拒否します([検査](../../src/core/loader/runtimespawn.cpp#L187)、接頭辞は [`runtime_spawn_name_prefix`](../../src/core/loader/runtimespawn.hpp#L26) と [`runtime_object_identity_prefix`](../../src/project/sceneformat.hpp#L32))。いまは上の形式の規則が `:` を含む名前を先に弾くので、予約が無くても衝突は起きませんが、予約は形式の規則とは独立した防御です(検査の順は予約が先なので、`spawn:2.1` は `object.name_reserved_by_engine@1` になります)。予約は接頭辞だけで、`spawn_3_0` のような名前は使えます。

**エラー名**

`spawn` の拒否は [`SpawnError`](../../src/core/userpublic/object_ref.hpp#L113)(`std::runtime_error` の派生)で、`code()` が名前、`what()` は `<名前>: <説明>` です。

| 名前 | いつ出るか |
|---|---|
| `<語>.module_unavailable@1` | prefab に、この build が外した機能の component がある(collider で physics が無いなど)。シーンのロードと同じ名前 |
| `prefab_*` / `behavior_type_unknown@1` | prefab や parameter が解決できない、behavior の型が登録されていない。シーンのロードと同じ名前 |
| `object.spawn_outside_scene@1` | シーンが読み込まれていない |
| `object.spawn_component_unsupported@1` | prefab が light か camera を持つ |
| `object.name_reserved_by_authored@1` | 渡した `name` がシーンに書かれた object の名前 |
| `object.name_reserved_by_engine@1` | 渡した `name` が `spawn:` か `pelican://`(engine が名前の無い object に付ける名前の接頭辞)で始まる(✅GP-2b) |
| `object.name_duplicate@1` | 渡した `name` を、このシーンの実行で spawn した別の object が使っている(待ち行列の中のものを含む) |
| `object.spawn_sequence_exhausted@1` | 境界の番号・1 境界の spawn 数・prefab の behavior 数が、behavior の識別子に入る数(2^28 / 2^16 / 2^8)を超えた |
| `object.spawn_invalid@1` | 展開した prefab の component が読めない、`name` が空文字列、または `name` が object 名の規則 `[a-zA-Z0-9_]` に合わない |

`destroy` と、`TransientObjectRef` を受ける `materialize` / `worldTransform` は例外ではなく `Resolved` で答えます。`object.stale_authority@1` は前のシーン実行の ref(既定構築の ref も)、`object.not_materialized@1` は境界の前・もう消した・もう destroy を頼んだ ref です。

**寿命**

- spawn した object は**保存されません**。シーン文書には書かれず、保存しても出ません。
- object は**シーンの実行と一緒に終わります**。シーンのロード(同じシーンの読み直しを含む)とエンジンの再起動で消え、その behavior には新しいシーンのロードが commit したときに `onDestroy` が呼ばれます([ロード側の指定](../../src/core/loader/scene.cpp#L1537))。ロードの前に積んだまま実体化していない要求は、捨てられます。
- エンジンにはまだ play モードがないので、設計の「preview が終わるまで」を「シーンの実行が終わるまで」と読んでいます。play モードができたら見直します。
- エディタの編集(同じシーンの実行の中)では、spawn した object の behavior と collider はそのまま残ります。
- `TransientObjectRef` は `ObjectRef` とは別の型です。シーンの実行(`ObjectAuthority`)は共有しますが、authored の ref を渡すべきところに transient の ref を渡すとコンパイルエラーになります。中身は「シーンの実行・実体化した境界の番号(実行の始めから 1, 2, …)・その境界の中の番号」で、名前も authoring id も持ちません。

**behavior の `attachment_seq`**: spawn した object の behavior の `attachment_seq` は、`(シーンの実行の中の境界の番号, その境界の中の spawn の番号, prefab の中の behavior の番号)` から決まります([`runtimeSpawnAttachmentSeq()`](../../src/core/gamelogic/behaviorarena.cpp#L82))。同じ入力なら 2 回の実行で同じ値になり、シーンに書いた behavior の後に並びます(シーンの object が 2^20 個未満のとき。§8.5.5)。

**GP-2 が残した穴**

- light / camera を持つ prefab は spawn できません(`object.spawn_component_unsupported@1`)。
- prefab の object 型の parameter は、シーンにある同名 object の代役(名前と、parameter が要求する component だけの写し)を置いて解決します([`objectParameterStandIns()`](../../src/core/loader/runtimespawn.cpp#L80))。
- シーンのロード中に旧シーンの behavior の `onDestroy` から spawn すると、要求が新しいシーンの実行に積まれ、シーンの切り替えを越えて object ができます。
- 大量に spawn したときの負荷は測っていません。
- object を複数持つ prefab は v1 です。

### input() と screenRay — クリックした先を調べる(✅GP-4)

ポインタの位置は [`ctx.input().pointer()`](../../src/core/userpublic/gamecontext.cpp#L489) で、その位置を通る world の ray は [`ctx.render().screenRay()`](../../src/core/userpublic/gamecontext.cpp#L475) で得られます。ray は §8.8 の物理クエリにそのまま渡せます。

```cpp
void update(Pelican::GameContext &ctx) {
    // "click" は actions.json の button アクション(例: mouse:left に bind)
    if (!ctx.actionPressed("click")) return;
    const auto pointer = ctx.input().pointer();
    if (!pointer.inside) return;                      // 窓の外、またはまだ動いていない

    const auto ray = ctx.render().screenRay(pointer.position_px);
    if (!ray) {
        ctx.logWarning(ray.errorName());              // XR 中は render.screen_ray_unavailable@1
        return;
    }
    if (const auto hit = ctx.physics().raycastClosest(ray.value())) {
        ctx.logInfo(hit->id);                         // 当たった collider のオブジェクト名
    }
}
```

**`pointer()`**([`PointerState`](../../src/core/userpublic/userinput.hpp#L143) = `{position_px, inside}`)

- 位置は **framebuffer ピクセル**です。原点は framebuffer の左上、+x が右、+y が下で、ピクセル (i, j) は [i, i+1) × [j, j+1) を覆います。
- OS はカーソルを窓座標で報告します。それを軸ごとに `px = 窓座標 × framebuffer の大きさ ÷ 窓の大きさ` で換算します([`pointerTransformFor()`](../../src/core/os/inputstate.cpp#L56))。表示倍率 200% の画面なら、窓座標 (10, 20) はピクセル (20, 40) です。窓の大きさが 0 の軸(最小化など)は 1 倍です。headless では窓が無いので 1 倍で、framebuffer は出力先の大きさです([`livePointerTransform()`](../../src/core/appflow/framephase.cpp#L67))。
- `inside` は、位置が分かっていて(入力を最後に clear してからカーソルの event が来ている)、かつ 0 ≤ px < framebuffer の大きさのとき true です([`pointerStateFor()`](../../src/core/os/inputstate.cpp#L74))。最初のカーソル event までは `position_px` は (0, 0) で `inside` は false です。
- 位置は clamp せず、どのアクションとも合成しません。ロック中のカーソルや、窓の外で捕まえたカーソルは framebuffer の外の位置を返しえます。
- **ポインタはアクションではありません。** アクションの axis2 は [-1, 1] に clamp され、同じアクションの binding どうしで足し合わされる(第7章 §7.2)ので、ピクセルの位置を運べないためです。
- 値はフレーム先頭の入力凍結で決まり、そのフレームの中では変わりません。

**`setCursorMode(mode)`**

- `normal` はカーソルを見せ、`hidden` は窓の上にあるときだけ隠し、`locked` は隠して窓の中に閉じ込め、移動量を制限なしで報告します(GLFW の disabled cursor)。
- 反映は**次のフレーム**です。通常のフレームループが窓の event を処理した直後に、前のフレームで頼まれたモードを窓へ適用します([`applyCursorMode()` の呼び出し](../../src/core/appflow/loop.cpp#L739))。
- 別の窓に埋め込まれた窓(`--rpc` で起動し、Studio の viewport として `WS_CHILD` にされた窓)では、`locked` を `normal` として扱います。ロックすると Studio 側のカーソルまで奪うためです([`Window::applyCursorMode()`](../../src/core/os/window.cpp#L343))。`hidden` はそのまま効きます。埋め込みの判定は Windows でだけ行います。
- 頼んだモードと効いているモードは rpc `get_status` の `input.cursor {requested, effective, embedded}` で見えます([第10章](10_tools.md))。headless には窓が無いので、頼んでも `effective` は `normal` のままです。
- scene load の途中に頼んでも load の transaction には入りません。分類は `nonparticipant.window-cursor` で([manifest の行](../../src/core/loader/sceneparticipantmanifest.cpp#L234))、load の成否に関係なく次のフレームで適用されます。

**`screenRay(position_px)`**

- 今の主 camera の projection と view の逆行列で、framebuffer ピクセルの位置を world の ray にします([`Camera::rayThroughPixel()`](../../src/core/renderer/camera.cpp#L391))。逆行列は double で計算します。
- NDC は `2 × px ÷ 幅 − 1`(y も同じ式で、**反転しません**)。平面描画のフレームが NDC を framebuffer へ写す向きに合わせてあります。幅と高さは camera の viewport(出力先の大きさ)です。
- 起点は near 面上、方向は正規化済み、`max_distance` は `phys::Ray` の既定値です。framebuffer の外の位置にも ray を返します。
- camera は呼んだ時点のものを読みます。同じ update の中で先に camera を動かしていれば、動かした後の camera の ray です。
- 失敗は [`RenderError`](../../src/core/userpublic/render_error.hpp#L11)(新しいヘッダ `render_error.hpp`。`gamecontext.hpp` が取り込みます)で返ります。名前は [`renderErrorName()`](../../src/core/userpublic/render_error.hpp#L20) でも引けます。

| エラー名(`RenderError`) | いつ出るか |
|---|---|
| `render.screen_position_invalid@1`(`screen_position_invalid`) | 位置が有限の数でない(NaN / 無限大) |
| `render.screen_ray_unavailable@1`(`screen_ray_unavailable`) | XR が有効。左右の目がそれぞれ別の view なので、画面の 1 点に 1 本の ray が決まらない |

位置の検査が先なので、XR 中に NaN を渡すと `render.screen_position_invalid@1` です。

**GP-4 が残した穴**

- 埋め込みの窓で `locked` を無視する挙動は、判定の純関数の test でしか確かめていません(Studio の中での実機確認は未確認)。
- カーソルモードの反映は 1 フレーム遅れます。
- `inside` の出入り(enter / leave)は記録していません。
- gizmo と modal transform は独自の `gizmoContentScale` で換算しており、窓座標 → ピクセルの換算が 2 系統あります。
- 「画面の 1 点に 1 本の ray が決まらない(複数の view で描いている)」の判定は `xr_active` だけです。

### actionPose(✅WP132 — XR 姿勢入力)

```cpp
const auto head = ctx.actionPose("head");
if (head.valid && head.source == Pelican::ActionPoseSource::synthetic_head) {
    // XR 起動中のみ届く。flat 起動では valid = false の既定値が返る(throw しない)
}
```

- `ActionPose` は position/orientation に加え **valid/tracked の 4 フラグ**、`source`(`action_space` / `synthetic_head`)、`reference_space`(stage / local_floor / local)、`hand`(left/right)を持ちます。
- 公開名の規約: `aim_left` / `aim_right` / `grip_left` / `grip_right` はコントローラの action space、**`head` だけは synthetic(合成)**です — コントローラのように OpenXR の action space から届く姿勢ではなく、`xrLocateViews` が返す両眼 view の位置を平均してエンジンが作る頭部姿勢で([syntheticHeadPose](../../src/core/openxr/openxrsession.cpp#L127))、binding を書くと名指しエラーになります。
- 実例は [projects/vrm_xr_demo](../../projects/vrm_xr_demo) の視線追従(head pose を優先し、flat では authored camera 基準にフォールバック)。

未知の名前(アクション・シーン・カメラ・オブジェクト・イベント)は**すべて名前入りの例外**になります(「名前入り」とは `unknown event type: DoorOpened` のように、どの名前で失敗したのかが例外メッセージに入るという意味です)。「静かに無視」はこのエンジンには存在しません(fail-fast の一貫した文化)。

> **シーンロードの最中だけ、この表の意味が変わります(✅WP380b〜e)。** `onInit` から呼んだ `emit` / `setSeed` / `loadScene` / `audio().playSound` / `audio().stopSound` / `audio().setBusVolume` / `setGameSettings` / `save().saveData` / `saveSettings` は**その場では起きず**、シーンロードのトランザクションに溜まります。ロードが失敗すれば全部消え、成功したときだけ実行されます。読み取り(`audio().isPlaying` / `save().loadData` / `save().listSaves` / `gameSettings`)は自分が溜めた書き込みを見ます。詳細は [第4章 §4.3](04_scene_ecs.md)。

## 8.5 オブジェクト behavior — もう一つのゲームコードの書き方(✅WP155/162/167)

ゲームシステム(§8.3)が「プロセスに 1 個・シーン全体を見る処理」なのに対し、**behavior は「オブジェクトに貼りつける振る舞い」**です。シーン JSON にパラメータごと書けて、エディタから付け外しもできます。

StudioではInspectorの **Add component...** から **Behavior: 登録名** を選び、宣言JSONの`params`を設定して追加できます。
同じ型を複数装着でき、追加・削除・Undo/Redo・保存は通常のシーン編集を使います。詳細は[第13章](13_editor.md)を参照してください。

> **設計決定(engine-owned arena):** behavior は ECS コンポーネントではなく、light / collider と同じ「特別扱いのコンポーネント」としてエンジン所有の attachment arena に入る(arena = 同種のものをまとめて置くエンジン側の置き場。ここでは attachment を連番で並べる [BehaviorAttachmentArena](../../src/core/gamelogic/behaviorarena.hpp#L213))。理由は、ゲーム DLL の型をエンジンの ECS チャンクに載せずに、ホットリロード時の世代交代(§8.5.6)とエディタからの付け外しを安全に行うため。

### 8.5.1 最小の実例

`test/fixtures/behavior_game_dll/behavior.cpp`(実物・全文):

```cpp
#include <behavior.hpp>

#include <string>

namespace {

struct ProjectBehaviorParams {
    std::string label;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&ProjectBehaviorParams::label>("label"),
                           "default"));
};

class ProjectBehavior final : public Pelican::Behavior {
  public:
    using Params = ProjectBehaviorParams;

    void onInit(Pelican::BehaviorContext &ctx) override {
        ctx.logInfo("WP155 behavior init label=" + ctx.params<Params>().label);
    }

    void onEvent(const Pelican::SceneLoaded &, Pelican::BehaviorContext &ctx) {
        ctx.logInfo("WP155 behavior event label=" + ctx.params<Params>().label);
    }

    void onUpdate(Pelican::BehaviorContext &ctx) override {
        ctx.logInfo("WP155 behavior update label=" + ctx.params<Params>().label);
    }

    void onDestroy(Pelican::BehaviorContext &ctx) noexcept override {
        ctx.logInfo("WP155 behavior destroy label=" + ctx.params<Params>().label);
    }
};

} // namespace

PELICAN_REGISTER_BEHAVIOR(ProjectBehavior, "wp155_project_behavior", 1);
```

シーン側([第4章](04_scene_ecs.md) §4.2):

```json
{ "name": "BehaviorOnly", "components": [
    { "name": "behavior", "type": "wp155_project_behavior", "params": { "label": "second" } }
] }
```

- include は **`<behavior.hpp>` 1 本**だけで足ります。
- マクロの引数は `(C++ 型, 永続名, schema バージョン)`。**永続名と C++ 型名は分離**されているので、クラス名を変えてもシーン JSON は壊れません。
- ⚠ 現時点で `projects/` 配下に behavior を使うサンプルはありません。実例はすべて `test/fixtures/behavior_game_dll/` ・ `behavior_reload_dll/` ・ `physics_trigger_behavior/` にあります。

### 8.5.2 クラスの決まりごと

| 項目 | 規則 |
|---|---|
| 基底 | `Pelican::Behavior` の継承が必須(コンパイル時 static_assert) |
| ライフサイクル | `onInit` / `onUpdate` / `onDestroy` は **virtual**(`override` を付ける)。`onDestroy` は **`noexcept` 必須** |
| イベント購読 | `onEvent(const E&, BehaviorContext&)` は **virtual ではない**(concept で検出され型消去される)。`override` は付けない |
| `Params` | `Type::Params` と `Params::schema` が必須。default 構築可能かつ **nothrow swappable**(`std::swap` が例外を投げない型であること。[registerer.hpp](../../src/core/userpublic/details/behavior/registerer.hpp#L265) が `std::is_nothrow_swappable_v` で検査。差し替えの途中で例外が出ると params が半分だけ新しい状態になるため) |
| 登録名 | 空文字は登録時エラー。同名の重複、エンジン側との衝突も名指しエラー |
| schema バージョン | 1 以上(0 はエラー) |

`onEvent` を持たせるときは、**同一翻訳単位で `PELICAN_REGISTER_EVENT(E)` → behavior 型定義 → `PELICAN_REGISTER_BEHAVIOR`** の順に書いてください(§8.6 のシステムと同じ制約です)。`onDestroy` から例外が漏れた場合は握りつぶされ、ERROR ログが出ます。

### 8.5.3 params(型付きパラメータ)

```cpp
static constexpr auto schema = Pelican::structFields(
    Pelican::behaviorParamsPolicy,
    Pelican::defaulted(Pelican::field<&P::speed>("speed", Pelican::frange(0.0, 20.0), "m/s"), 4.0f),
    Pelican::defaulted(Pelican::field<&P::can_jump>("can_jump"), true));
```

(例 — 書式は `test/behaviorarena_test.cpp` の実物に準拠)

- **すべてのフィールドが `defaulted(...)` 必須**です(`required(...)` はコンパイルエラー)。シーン JSON 側で `params` を省略しても全部既定値で成立します。イベントペイロード([第7章](07_input_ui.md)の EventPayloadSchema)とは逆の規約なので注意してください。
- 使える型: 整数各種 / `F32` / `F64` / `Vec2〜4` / `Quat` / `String` / **`Bool`** / **`Enum`**(`enumValues(...)` 必須・最大 8 個)。任意 JSON は使えません。
- ✅WP382a で **`Flags<Enum>` と `List<Row>`** が加わりました(下記)。素の配列・任意ネスト構造体は今も使えません。
- 範囲は `irange` / `urange` / `frange`、単位文字列は `field<&P::x>("x", range, "m/s")` の第 3 引数。これらは**インスペクタのウィジェットにそのまま反映**されます([第13章](13_editor.md))。
- 未知キー・型不一致・範囲外はすべて `field 'params.speed' is out of range` の形式で**フィールドパス付きの名指しエラー**になります。
- 適用は「既定値で構築 → 存在するキーだけ適用 → 全検証成功後に swap」。途中で失敗しても既存の値は一切変わりません。
- `ctx.params<Params>()` は非 const 参照を返すので実行中に書き換えられますが、**その変更は永続化されません**(リロードやシーン再構築で authoring 値に戻ります)。

#### 行と旗(✅WP382a)

「同じ形のレコードを何行か持たせたい」「複数の旗を立てたい」ときは、素の `std::vector` ではなく専用の型を使います。JSON では `List<Row>` は行オブジェクトを並べた入れ物、`Flags<Enum>` は語彙名を並べた文字列配列になります(書式は [第4章](04_scene_ecs.md) §4.4)。

```cpp
enum class InstanceFlag : std::uint32_t { CullDisable = 1u, FlipFacing = 2u };

struct OverrideRow {                       // 行の型は自分の schema を持つ
    std::string occurrence_id;
    std::optional<std::uint8_t> mask;      // OptionalOverride は optional で受ける
    std::optional<Pelican::Flags<InstanceFlag>> instance_flags;

    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("my_override_row"),
        Pelican::required(Pelican::field<&OverrideRow::occurrence_id>("occurrence_id")),
        Pelican::optionalOverride(
            Pelican::field<&OverrideRow::mask>("mask", Pelican::urange(0, 255))),
        Pelican::optionalOverride(
            Pelican::field<&OverrideRow::instance_flags>("instance_flags"),
            Pelican::enumValues(
                Pelican::enumValue("cull_disable", InstanceFlag::CullDisable),
                Pelican::enumValue("flip_facing", InstanceFlag::FlipFacing))));
};

struct CarrierParams {
    Pelican::List<OverrideRow> overrides;
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&CarrierParams::overrides>("overrides")));
};
```

(実物: `test/fixtures/wp382a_schema/v3_fixture.cpp`)

- `Flags<Enum>`([structfieldschema.hpp](../../src/core/userpublic/details/schema/structfieldschema.hpp#L184))は `enumValues(...)` が**必須**(無いとコンパイルエラー)。語彙は最大 8 個。
- [`List<Row>`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L222) の `Row` は自分の `schema` を持つ型で、**その中にさらに `List` は置けません**(1 段まで。違反はコンパイルエラー)。
- [`optionalOverride(...)`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L638) のフィールドは `std::optional<T>` で受けます。behavior params の**トップレベルには置けません**(そこは `defaulted` 必須)ので、実際の置き場所は行の schema の中です。
- これらを 1 つでも宣言すると、SDK は登録入口を自動で `_v3` に切り替えます(§8.1)。書き手が指定するものはありません。

### 8.5.4 BehaviorContext

`BehaviorContext` は **`GameContext` を継承**しているので、§8.4 の API(入力・時刻・ログ・物理クエリ・乱数・音・シーン遷移・永続化・`emit`)はそのまま使えます。追加されるのは次の 4 つです:

| API | 意味 |
|---|---|
| `self()` | 自分が貼られているオブジェクトの `GameObjectId` |
| `attachment()` / `attachmentSeq()` | attachment のハンドルと決定的な連番 |
| `params<Params>()` | 型付きパラメータへの参照(型が違えば `std::logic_error`) |

- `createObject` / `createSpriteObject` / `removeObject` も呼べますが、**構造変更は次の behavior 境界まで遅延**されます。生成系は常に `invalidGameObjectId` を返す(その場では ID が取れない)点に注意してください。
- `spawn` / `destroy`(✅GP-2)は、コールバックの中でも外でも同じく次のフレーム境界で実行されます。`spawn` はその場で `TransientObjectRef` を返します(§8.4「実行時生成 — `ctx.spawn`」)。
- `GameObjects::removeAll()` を behavior のコールバック中に呼ぶと `std::logic_error` になります。

### 8.5.5 実行順とライフサイクル(決定性)

- behavior 全体は **`"BehaviorSystem"`(order = 50)という 1 個のゲームシステム**として、他のゲームシステムと同じ `(order, 型名)` の全順序に並びます。つまり `order < 50` のシステムは全 behavior より前、`order > 50` は後に走ります(ビルトインのカメラコントローラは 10000)。
- attachment どうしの順序は `attachment_seq` の昇順。シーンロード時の seq は**宣言順**(オブジェクト順 → コンポーネント順)から決まります。`ctx.spawn` で作った object の behavior の seq は `(境界の番号, その境界の中の spawn の番号, prefab の中の behavior の番号)` から決まり、シーンに書いた behavior より後に並びます(シーンの object が 2^20 個未満のとき。✅GP-2。§8.4「実行時生成 — `ctx.spawn`」)。
- **生成**: シーンの entity/collider/light を publish した後、`attachment_seq` 昇順に `onInit`。途中で例外が出たら、それまでに init 済みのものを逆順に `onDestroy` して全部巻き戻し、例外を投げ直します(このとき失敗した本人の `onDestroy` は呼ばれません)。巻き戻るのは behavior だけではなく、scene load 全体が旧シーンに戻ります(entity・collider・light・カメラ・描画・`onInit` 中の `emit` / `loadScene()` も。✅WP380a、[第4章 §4.3](04_scene_ecs.md))。`SceneLoaded` の emit は commit の後です。
- **破棄**: `attachment_seq` の**逆順**に `onDestroy`([destroyDetached](../../src/core/gamelogic/behaviorarena.cpp#L376))。`ctx.self()` から自分の transform / sprite を読めます — シーン切替の retire 段では entity は既に unpublish 済みなので、読めるのは**破棄トークンが持つ最終スナップショット**です。破棄トークンとは、unpublish のときにコンポーネントの値をそのまま退避しておく控えのことで、値が実際に壊れるのは retire の完了時です([finish](../../src/core/ecs/archetypemigration.cpp#L917))。
- 更新・イベント配送はどちらも開始時に有効な attachment のスナップショットを取ってから走るため、途中で消えたオブジェクトは安全にスキップされます。

#### `onInit` / `onDestroy` から呼べること(✅WP380b〜e)

シーンロードの最中は、`GameContext` の作用がトランザクションに溜まります(§8.4 の囲み・[第4章 §4.3](04_scene_ecs.md))。behavior から見た要点:

- `onInit` で `ctx.audio().playSound()` すると**音はまだ鳴りません**。ロードが commit したあとの flush で鳴ります。直後に `ctx.audio().isPlaying(handle)` を呼べば true が返り、`ctx.audio().stopSound(handle)` すればその予約ごと消えます(鳴らずに終わる)。
- `onInit` で `ctx.save().saveData()` した内容は、同じ `onInit` の中の `ctx.save().loadData()` で読めます。`ctx.save().listSaves()` にも並びますが、**まだ書いていないので `timestamp` は 0** です。
- `onInit` が例外を投げると、これらの作用は**すべて捨てられます**(予約した voice は解放され、`user://` の中身も設定も変わりません)。シーンも旧シーンに戻ります。
- **`onDestroy` は破棄されるオブジェクトの最終状態を読めます。** `ctx.localTransform(ctx.self())` / `ctx.spriteView(ctx.self())` は破棄トークンのスナップショットを返します。書き込み側は throw せず `false` を返します。
- `onDestroy` は `noexcept` なので、この段で `playSound` や `saveData` が失敗しても**例外にはならず**、ロード結果の `effect_errors` に名前付きで積まれます。
- `onDestroy` の中の `loadScene(name)` は、**もう commit された新しいシーン集合**に対して解決されます。旧シーンにしか無い名前は拒否され、`scene_retire_load_rejected@1` として記録されます。
- ⚠️ ロード中に `GameObjects::` の静的 API を直接呼ぶと `SceneTransactionMutationRejected{api: ...}` です。オブジェクト操作は `BehaviorContext`(= `GameContext`)経由で書いてください。

### 8.5.6 DLL ホットリロードとの関係(✅WP162)

ゲームロジック DLL を差し替える([第10章](10_tools.md) §10.5)と、behavior は次の規律で世代交代します:

| 新 DLL の状態 | 結果(エラーはリロード拒否 = 旧 DLL 継続) |
|---|---|
| 旧 DLL にあった登録名が消えた | `behavior_type_removed: stable_name='X' version=1` |
| schema を変えたのにバージョン据え置き | `schema_changed_without_version_bump: ...` |
| バージョンを上げたが既存 params が新 schema で読めない | `schema_incompatible: ... scene='...' object_index=0 field='params.count': ...` |

- バージョンを上げたときは、**現在ロードしていないシーンも含めて**全 behavior の params が新 schema で読めることが条件です。
- リロード後、behavior のインスタンス状態(メンバ変数)は失われ、params は authoring 値から再構築されます(full-reset 方式)。
- `schema_changed_without_version_bump` の判定は **schema の指紋**(schema の宣言内容を宣言順のまま 1 本の JSON 文字列に書き出したもの — [schemaNodeFingerprint](../../src/core/userpublic/details/schema/structfieldjson.hpp#L1040))で、この文字列が 1 文字でも変われば別の schema と見なします。フィールドの型だけでなく**既定値・宣言順・enum 値の割り当て・presence(`defaulted` ↔ `required` ↔ `optionalOverride`)の変更も検出します**([validateBehaviorReloadSources](../../src/core/gamelogic/behaviorreloadvalidation.cpp#L85))。「型は同じだから大丈夫」は通りません。
- ⚠️ **この検証は behavior params だけが対象です。** イベントペイロードと ECS コンポーネントの schema を version bump なしで変えても、現状は検出されません(WP382a-fix で追加予定 = 📐**未対応**)。

### 8.5.7 ゲームシステムとの使い分け

| | ゲームシステム `PELICAN_REGISTER_SYSTEM` | behavior `PELICAN_REGISTER_BEHAVIOR` |
|---|---|---|
| 実体の数 | プロセスに 1 個 | 貼った attachment ごとに 1 個 |
| 対象 | 自分で `GameObjectId` を管理 | `ctx.self()` が自オブジェクト |
| データの与え方 | ハードコード / 自前で JSON を読む | **シーン JSON の `params`**(型付き・インスペクタ編集可) |
| シーン切替 | インスタンスは**生き残る**(§8.7 の落とし穴) | **破棄され、新シーンで作り直される** |
| エディタ | 触れない | attach / remove / params 編集 + undo([第13章](13_editor.md)) |

指針としては、**「シーン全体を見る 1 つの処理」(入力ディスパッチ・グローバル状態・演出)はゲームシステム、「このオブジェクトはこう振る舞う」+「値をシーンデータとして持たせたい」は behavior** です。ユーザー定義の ECS コンポーネント登録 API は今も存在しないため([第4章](04_scene_ecs.md) §4.4)、**オブジェクト固有の authored データを持たせる唯一の公開手段が behavior params** でもあります。

## 8.6 イベント層(✅WP56/71/179)

> **設計決定(フレーム境界配送):** `emit` はフレーム中いつでもできるが、**配送は次フレームの頭**(全システム update 前)。同一フレーム内の即時配送はしない — システム実行順に依存する非決定性を絶つため。配送順は emit 順。「今すぐ知りたい」ものはイベントではなくクエリ API(raycast 等)を使う(イベントは通知、クエリは質問)。

```cpp
// 1. イベント型の定義と登録(値だけを持つ struct)
struct DoorOpened {
    std::string door_name;
    template <class T> void ref(T &ar) { ar.prop("door_name", door_name); }
};
PELICAN_REGISTER_EVENT(DoorOpened);

// 2. 発行(システムの update 内などで)
ctx.emit(DoorOpened{"gate_a"});

// 3. 購読(システムのメンバとして)
void onEvent(const DoorOpened &e, Pelican::GameContext &ctx);
```

- イベント名は型名の最後の `::` 以降(`Pelican::SceneLoaded` → `"SceneLoaded"`)。名前空間違いの同名型は起動時に衝突エラーになります。
- ペイロードは値コピーで保持されます(1 フレーム跨ぐため、参照・ポインタは入れない)。型に課される要件は「コピー構築できる object 型」だけです([registerer.hpp](../../src/core/userpublic/details/event/registerer.hpp#L148))。
- `ref()` は「どのフィールドがどの名前か」を 1 か所で宣言するメンバ関数です。エンジンはこれを使って JSON からペイロードを組み立てたり、エディタ向けにフィールド名の一覧を取ったりします。
- **同一翻訳単位内で `PELICAN_REGISTER_EVENT` → 型定義 → `PELICAN_REGISTER_SYSTEM` の順**に書いてください。システム登録マクロは、その時点で見えているイベントカタログだけを購読対象にします。
- エンジンが発行するイベントは `SceneLoaded { scene_name }` と、
  物理トリガーの `OverlapEnter { self, other }` /
  `OverlapExit { self, other }` です。`self` / `other` は generation を含む
  full `EntityId`(`GameObjectId` の別名 — [entity.hpp](../../src/core/userpublic/details/ecs/entity.hpp#L19))で、同じ pair を両オブジェクト視点へ配送します。
- rpc の `inject_event` で外部からもイベントを注入できます。ただし**ペイロードを渡せるのは `pelican_payload`([第7章](07_input_ui.md) の EventPayloadSchema)を宣言した型だけ**で、`ref()` しか持たない型は `event type does not expose a payload schema` で拒否されます([validateByName](../../src/core/userpublic/details/event/registerer.cpp#L387))。
- behavior も同じ `onEvent` でイベントを受けられます(§8.5)。書く順序の制約(`PELICAN_REGISTER_EVENT` → 型定義 → 登録マクロ)も同じです。
- **シーンロード中の `emit` はロードのトランザクションに溜まります**(✅WP380a)。ロードが失敗すれば配送されず、成功すれば commit 後の flush で pending queue に入ります(配送は従来どおり次フレーム頭)。`SceneLoaded` 自身も同じ flush で出ます — 送信箱に入る順は「`onInit` の emit → 旧 behavior の `onDestroy` の emit → `SceneLoaded`」で、配送順は emit 順なので **`SceneLoaded` は最後**に届きます。

### 使い分けの作法(フレーム境界配送を前提にした設計)

配送が次フレームである以上、**イベントで因果を積み上げると 1 つ進むごとに 1 フレーム遅れます**。次の 2 つを守ってください:

- **同じフレームのうちに結果が要るものは、イベントではなく直接呼び出しかクエリで書く。** 「今ぶつかっているか」は `raycast` / `overlap` で問い合わせ、「ぶつかった**という出来事**を他所に知らせる」ときだけイベントにします。
- **多段の連鎖(A→B→C→D)はイベントで繋がず、1 つのイベントに畳む。** 3 段連鎖は 3 フレームの遅れになります。中間状態を持つ側が一度にまとめて処理し、外に出す通知だけをイベントにします。

## 8.7 シーン遷移(✅WP52)

```cpp
ctx.loadScene("scene_flow_second");   // 予約(名前はその場で検証。未知名は即例外)
```

- 適用は**フレーム末尾**(ゲームシステム実行後・描画前)。同フレームに複数回呼ぶと最後の 1 件が勝ちます。
- 切替では全 GameObject・物理・描画インスタンス・名前束縛が破棄され、**起動時と同じロード経路**で新シーンが構築されます(第 2 経路を作らない方針)。
- 切替は **1 つのトランザクション**です(✅WP380a)。途中で失敗したら document・ライト・カメラ・ECS・物理・描画・behavior が**まるごと旧シーンのまま**で、中途半端な状態にはなりません(読み込み済みのモデル資産だけはキャッシュなので残ります)。commit 後に音やセーブの書き出しだけが失敗した場合は「シーンは新しいまま + `effect_errors`」になります([第4章 §4.3](04_scene_ecs.md))。
- **`EngineTime` は継続**します(時刻・フレーム番号はリセットされない)。時計はシーンではなくプロセスに属するモジュールで([enginetime.hpp](../../src/core/appflow/enginetime.hpp#L31))、シーンロードはこれに触らないためです。シーンローカル時間が欲しければ `SceneLoaded` 受信時の時刻を覚えてください。
- 非同期ロード(`loadSceneAsync`)は 📐S2 未実装です。

### 落とし穴: システム状態はシーン切替で消えない

システムのインスタンスは static シングルトンなので、メンバに保持した `GameObjectId` は切替後「死んだ ID」になります(`setLocalTransform` / `removeObject` は false を返し、`localTransform` は例外になり得ます)。保持した `ObjectRef`(§8.4 の objects())も、切替後は `object.stale_authority@1` で拒否されます。**`spawn` した `TransientObjectRef` も同じで、シーンの切替(同じシーンの読み直しを含む)で stale になります。** spawn した object 自体も切替で消え、作り直されることはありません。同じフレームで `spawn` の後に `loadScene` を呼ぶと、その spawn は実体化しないまま捨てられます(切替がフレームの末尾、spawn の実体化が次のフレームの先頭のため)。**`SceneLoaded` の `onEvent` で状態をリセットする**(`ObjectRef` なら `resolve` し直す、`TransientObjectRef` は捨てて必要なら spawn し直す)のが正道です。「シーンをまたいで残るオブジェクト」(persistent)の仕組みは v1 にはありません。

## 8.8 物理クエリとトリガー(✅P1/P2/WP107/179)

> **設計決定:** ランタイム物理シミュレーションは「やらない」ではなく「**後**」(2026-07-07 決定)。現在あるのは raycast / overlap / shapeCast の **query world**(collider の配置だけを持ち、問い合わせに答える世界。速度や力を積分して物を動かす step は持たない — [physworld.hpp](../../src/core/phys/physworld.hpp#L42) の公開面はバインドとクエリだけ)。Jolt は query provider として実装済みですが、剛体 step/constraint を持つシミュレーションは未実装です。映像用の破壊・布は Houdini ベイク(transform_seq / VAT)レーンが担当([第5章](05_assets.md))。

- collider はシーン JSON のコンポーネントとして定義します([第4章](04_scene_ecs.md) §4.2)。**collider を持つオブジェクトには `name` が必須**です。
- ゲームコードから:

```cpp
// レイキャスト(最近傍 1 件)
Pelican::phys::Ray ray{ {0,0,0}, {0,0,1}, 100.0f };   // origin, direction, max_distance
if (auto hit = ctx.physics().raycastClosest(ray)) {
    // hit->id(オブジェクト名), hit->distance, hit->position, hit->normal
}

// 形状オーバーラップ(該当 collider の hit 一覧、collider id 昇順。名前は hit.id)
auto hits = ctx.physics().overlapAll(Pelican::phys::Shape{ Pelican::phys::Sphere{ {0,1,0}, 0.5f } });

// capsule を delta だけ平行移動し、最初の衝突を取得
Pelican::phys::Shape player = Pelican::phys::Capsule{
    {0, 2, 0}, {}, 0.5f, 0.25f
};
if (auto hit = ctx.physics().shapeCastClosest(player, {0, -4, 0})) {
    // hit->time_of_impact は delta のどこで当たったかの割合(0 = 出発点、1 = 動かしきった先)。
    // normal は player を押し出す向き。最初から食い込んでいた(initial_overlap)ときは
    // normal * penetration_depth が MTD(minimum translation distance = 食い込みを解く最小の移動)。
}
```

- collider metadata は scene component に `layer`(既定 1)、`mask`(既定 `0xffffffff`)、`trigger`、`one_way`(既定 false)で指定します。詳細 query overload の `QueryFilter` で reciprocal layer/mask、self/ignore、trigger/one-way inclusion を制御できます。
- **決定性の規約**: shapeCast の TOI tie は ε=`1e-5` bucket 後に stable `ColliderId`、full entity generation、shape ordinal の順です。MTD tie は移動の逆向きを優先し、その後 world x/y/z で固定します。closest は同じ ordered all-hit の先頭です。方向付き one-way policy は `shapeCastAll` を順に評価するため、無視した後の「次の床」を再 query せず続行できます。
- 初期 penetration が ε より深ければ TOI 0 + MTD。接触だけなら接近中に限り TOI 0、静止/離反では hit になりません。zero delta は depenetration query です。
- `one_way` は metadata です。前位置や接近方向から「通す/乗る」を決める policy は、WP109 の標準ユーザー空間ライブラリ `platformer::moveAndSlide` が担当します(その実装では「法線が床向き」「その面へ向かって動いている」「動く前に面より上にいた」の 3 つが揃ったときだけ衝突として扱います — [acceptsOneWay](../../src/core/userpublic/platformer/charactercontroller2d.cpp#L187))。
- `trigger=true` は物理応答を行わない検知専用 collider です。query では
  既定で通常の hit として見え、`QueryFilter::include_triggers=false` で
  除外できます。overlap 開始時に `OverlapEnter`、終了時に
  `OverlapExit` を各 pair の両視点へ一度だけ emit し、stay は送りません。
  entity destroy / collider remove は次の trigger update で Exit、scene
  全 reset は旧 EntityId が無効になる domain boundary として Exit なしです。
- Provider ABI V2 により Builtin/Jolt/game DLL provider を capability 単位で差し替えます。Jolt header は engine/game の公開型へ出ません。physics-off と provider-only build では不要な backend をリンクしません。
- **プロバイダの選択はビルド時**です: `PELICAN_WITH_BUILTIN_PHYSICS`(既定 ON)/ `PELICAN_WITH_JOLT_PHYSICS`(既定 OFF。両方 ON なら Jolt が勝つ)。project.json や実行時の切替はありません。クエリ契約(順序・タイブレーク)はプロバイダによらずエンジン側が所有するため、通常は意識不要です。ゲーム DLL から capability 単位で provider をオーバーレイする上級拡張点(`physics/abi_v2.hpp` — raycast だけ独自実装し残りはフォールバック、等)もあります。
- transform 追従は現行では毎フレーム再収集(BVH = bounding volume hierarchy、形状を包む箱を木に組んで判定候補を絞る加速構造。それを持たないという意味 — 計測してから見直す方針)。capsule の軸はローカル Y、box は OBB(回転対応)です。
- `debug_draw` feature を rendering config で参照していると collider のワイヤフレームが描画されます([第6章](06_rendering.md))。
- rpc の query は `raycast`([受け口](../../src/core/communication/rpcserver.cpp#L1153)。`origin` / `direction` / `max_distance` を受けて最近傍 1 件を返す)だけです。provider が 0 本のビルドと `PELICAN_WITH_PHYSICS=OFF` のビルド(✅PG-2b)では、この `raycast` も「当たらなかった」ではなく JSON-RPC の error を返します(§8.4「機能を外した build」)。
- 📐未実装: rpc の overlap / shapeCast、メッシュコライダ/BVH、剛体シミュレーション。

### 8.7.1 side-scroller controller(WP109)

```cpp
#include <platformer/charactercontroller2d.hpp>

Pelican::platformer::MoveAndSlide2DSettings settings;
settings.max_slope_degrees = 50.0f;
settings.collide_with_one_way = !drop_through;

const auto moved = Pelican::platformer::moveAndSlide(
    ctx, player_capsule,
    {velocity.x * fixed_dt, velocity.y * fixed_dt, 0.0f}, {}, settings);
player_capsule = moved.shape;
if (moved.grounded && velocity.y < 0.0f) velocity.y = 0.0f;
if (moved.hit_ceiling && velocity.y > 0.0f) velocity.y = 0.0f;
```

この helper は XY side-scroller 用で、連続 sweep による tunneling 防止、初期
overlap 回復、接地/壁 slide/天井、最大斜面角、上からだけ乗る one-way を扱います。
剛体 simulation や Jolt 固有 API ではなく、公開 `shapeCastAll` の結果だけを使う
非特権ライブラリです。callback overload へ独自 ordered-all-hit query を渡せるため、
Builtin/Jolt/game DLL provider の交換、ゲーム独自 controller への置換、機能の完全な
不使用が可能です。step-up(段差の乗り越え)、coyote time(地面から離れた直後の数フレームだけジャンプ入力を受け付ける猶予)、moving platform はゲーム固有 policy として
この最小 helper の外に残しています。実例は `projects/sprite_demo` を参照してください。

## 8.9 カメラ(✅C1/C2 = WP48/50)

カメラは「**定義**(glTF 1:1 の投影)/ **コントローラ**(orbit・follow・fly)/ **演出**(カット切替など)」の三層設計です。定義とコントローラの書き方(シーン JSON)は [第4章](04_scene_ecs.md) §4.2 を参照してください。

- **カット切替**: `ctx.render().setCamera("SceneFlowCamera")` または rpc `set_camera`。名前付きカメラだけが宛先になれます。切替後はフリーカメラ操作がロックされ、そのカメラの定義が即時適用されます。
- **コントローラ**はビルトインのゲームシステム(order 10000)が毎フレーム駆動します。orbit / fly は Actions の `move` / `look`(いずれも axis2)を消費しますが、アクションが未定義でも「動かないだけ」でエラーにはなりません。
- damping はブレンド重み `1 - exp(-damping * dt)` で、0 なら即時追従。パラメータ変更やフレーム巻き戻し(rpc `set_time`)を検出すると内部状態を自動リセットします(決定性のため)。
- 📐未実装(C3 以降): glb カメラノードの取り込み/書き出し round-trip、transform_seq のカメラトラック、ブレンド・シェイク。

## 8.10 決定性 — なぜ毎回同じ結果になるのか

> **設計決定:** エンジン内部の判定に wall clock・`std::rand`・`random_device` を使わない。乱数は **PCG32**(64 ビットの内部状態を線形合同で進め、ビットを混ぜて 32 ビットを取り出す生成器 — [nextUInt](../../src/core/userpublic/deterministicrng.cpp#L25))**の自前実装**(distribution も自前)で、プラットフォーム・コンパイラに依存しない列を生成する(`std::mt19937` + `std::uniform_*` は処理系差があるため不採用)。シードの既定は **0(固定)** — 「毎回違う」が欲しいゲームは自分で `ctx.setSeed(エントロピー)` を呼ぶ。

決定性を構成する要素(✅すべて実装済み):

1. フレームループの固定順序(§8.3)と rpc `step_frame` の同一順序
2. 入力はフレーム頭のスナップショットで凍結(WP37)—— フレーム開始時に、前フレーム以降に OS から届いた入力を 1 回だけ切り出し([beginFrame](../../src/core/os/inputstate.cpp#L417))、そのフレーム中は誰が何度読んでも同じ値です(途中で届いた入力は次フレームの分として待ちます)
3. システム実行順 = order → 名前の辞書順
4. イベント配送 = 次フレーム頭・emit 順
5. 物理のタイブレーク(同じ距離・同じ TOI に並んだヒットの順序決め)= stable `ColliderId` → entity 世代 → shape ordinal(名前は最終手段)。無フィルタ専用だった単引数版 `raycastClosest(ray)` は廃止され、[`raycastClosest(ray, filter = {})`](../../src/core/userpublic/gamecontext.hpp#L56) 1 本になりました(filter を省いても規則は同じ。「フィルタ無しで全 collider」が要るときは `phys::every_collider_filter` を渡します)。最近傍 1 件を選ぶときの同距離クラスタの決着だけは、どの filter でも文字列 id の昇順です([physworld.cpp](../../src/core/phys/physworld.cpp#L591))
6. 乱数 = PCG32 + 固定シード(`basic_config.seed` / `ctx.setSeed` / rpc `set_seed`)
7. シーン切替 = フレーム境界適用
8. ヘッドレス実行 = 固定ステップ時刻(dt = 1/fps)

リプレイの三点セット「固定ステップ+入力記録+シード」は ✅**完成**しています(入力記録 = `pelican.input_seq`、WP89 — [第7章](07_input_ui.md) §7.3)。同一ビルド + プロジェクト + シードで PNG byte 一致まで CI 検証されています。rpc シナリオ(`inject_input` + `step_frame`)も従来どおり使えます。

ウィンドウモードの時刻は実測 dt(上限 0.1 秒でクランプ)なので、厳密なリプレイ互換が必要な検証はヘッドレスで行ってください。

## 8.11 オーディオ(✅A1 = WP51、AU-1 `feca9dce`、AU-2 `e68573a2`、AU-3 `a8b03406`)

音を鳴らす道は 2 本あります。ゲームコードから 1 回鳴らす `ctx.audio().playSound`(下の「ゲームコードから鳴らす」)と、共通グラフの `audio.playback` ノード(下の「グラフで鳴らす」)です。グラフの voice は「今どこを鳴らすか」をグラフの時計が決めるので、ループ・一時停止・シーンをまたぐ BGM・フェードをグラフの中で組めます。

### ゲームコードから鳴らす

```cpp
auto h = ctx.audio().playSound("assets/audio/shot.wav");     // se バスで鳴る
auto m = ctx.audio().playSound("assets/audio/theme.ogg", "bgm"); // バスを名指し(AU-1)。Ogg Vorbis(AU-2)
ctx.audio().setBusVolume("se", 0.5f);                        // バスは master / bgm / se の 3 固定
bool playing = ctx.audio().isPlaying(h);
ctx.audio().stopSound(h);
```

- 対応フォーマットは **WAV と Ogg Vorbis** です(✅AU-2。MP3 / FLAC は需要が出てから)。**拡張子(大文字小文字は区別しない)と先頭 bytes が同じ形式を指すときだけ**受け付けます([`detectSoundFormat`](../../src/core/audio/audio.cpp#L318))。
  - `.wav` は先頭が `RIFF`、8 byte 目から `WAVE`。中身は PCM か IEEE float で、規格から外れたもの(chunk の食い違い、block align の不一致など)は `Invalid WAV: ...` で拒否します。
  - `.ogg` は先頭が `OggS` で、最初の packet が Vorbis の識別 header(`\x01vorbis`)であること。**Opus や FLAC を入れた Ogg は通りません。**
  - それ以外(`.mp3`、拡張子なし、中身が WAV の `.ogg` のような食い違い)は、decode の前に **`audio.format_unsupported@1`** `{asset, extension, signature, supported}` で拒否します。`signature` は先頭 bytes の判定(`wav` / `ogg` / `unknown`)です。`playSound` からもこの名前の例外が出ます。
  - 形式の方針は [資産形式の方針](../design_asset_format_policy.md) §1.1、一覧は [第5章](05_assets.md) §5.8。
- 再生バックエンドは miniaudio(0.11.25)で、Ogg Vorbis の decoder は miniaudio に同梱の stb_vorbis です(`PELICAN_WITH_AUDIO` の中だけで有効)。
- **10 秒を超える音は streaming で鳴ります**(✅AU-2。長さは header で判断)。全体を decode せず、符号化されたままの bytes を 1 つだけ持ち、voice ごとに自分の decoder で再生しながら読みます。下の「長い音の streaming」を見てください。
- `playSound(path)` は se バスで鳴ります。**`playSound(path, bus)`**([宣言](../../src/core/userpublic/gamecontext.hpp#L89))は `"se"` か `"bgm"` を受け、それ以外(`"master"` を含む)は `Unknown audio bus: <名前> (a voice plays on se or bgm)` で投げます。master は se と bgm の両方に掛かる音量で、voice を載せる先ではありません。実効音量は master × 各バスです。
- **同じアセットの decode は 1 回です**(✅AU-1)。decode した結果はアセットの正規化した参照を鍵に保持し、ディスク上のファイルなら更新時刻とサイズも鍵に含めます(置き換えたファイルは decode し直す)。合計の上限は 256 MB で、超えたときは **どの voice も持っていないもの**から捨てます([`decodedCacheBudget`](../../src/core/audio/audio.cpp#L408))。`playSound`、グラフの voice、rendering config 登録時のアセット検査が同じ decode を共有します。streaming の音(10 秒超)も同じ cache に入りますが、持つのは PCM ではなく符号化された bytes で、上限にはその大きさで数えます。
- `PELICAN_WITH_AUDIO=OFF` ビルドで音 API を呼ぶと、どのフラグで無効化されたかを言うエラー(`BuildFeatureCallDisabledError`、`code()` = `build_feature_disabled@1`)になります。`onDestroy` の中だけは例外にならず `effect_errors` に積まれます(§8.4「機能を外した build」)。
- **シーンロード中(behavior の `onInit` / `onDestroy`)に呼ぶと、再生・停止・バス音量はロードの成功後にまとめて適用されます**(§8.4 の囲み・[第4章 §4.3](04_scene_ecs.md))。`isPlaying` / `busVolume` はその予約を反映した値を返します。

### どのバックエンドで鳴るか(✅AU-1、起動時の指定は ✅AU-3)

起動で名指ししなければ、起動の形でバックエンドが決まります([`preferredAudioBackend`](../../src/core/audio/audio.cpp#L437)、[`Audio::Audio()`](../../src/core/audio/audio.cpp#L799))。

| 起動の形 | バックエンド | 音 |
|---|---|---|
| 窓あり(Studio に埋め込んだ player を含む) | `device` | オーディオデバイスへ出る |
| ヘッドレス(`--headless`) | `offline` | デバイスを開かないエンジンへ混ぜる。何も読み出さないので聞こえない |
| 起動で `null` を名指ししたとき(✅AU-3) | `null` | 無音。デバイスもエンジンも開かない。退避ではないので `fallback_reason` は付かない |
| `device` / `offline` が開けなかったとき | `null` | 無音。API は全部動く。開けなかった理由が残る |

AU-1 より前は「ヘッドレス / rpc なら null」でしたが、いまは **rpc で動いていても窓があれば `device`** です(Studio の埋め込み player で音が出るのはこのため)。

**起動で名指しする(✅AU-3)。** player は次の順でバックエンドを決めます([引数と環境変数の読み取り](../../src/player/main.cpp#L462)、[`EngineLaunchConfig::audio_backend`](../../src/core/launchconfig.hpp#L57))。

| 優先 | 指定 | 値 |
|---|---|---|
| 1 | 引数 `--audio-backend <値>` | `device` / `offline` / `null` |
| 2 | 環境変数 `PELICAN_AUDIO_BACKEND` | 同上 |
| 3 | どちらも無い | 上の表(窓あり = `device`、ヘッドレス = `offline`) |

- **引数があると環境変数は読みません。** 環境変数が空文字列なら未設定と同じ扱いです([`audioBackendEnvironment`](../../src/player/main.cpp#L122))。
- 値は完全一致です(`Null` や空文字列の引数は通らない。[`audioBackendKindFromName`](../../src/core/audio/audiobackendkind.hpp#L31))。それ以外の値は起動を止め、`audio.backend_unknown@1: --audio-backend must be one of: device, offline, null: <値>` で失敗します(環境変数から来たときは `--audio-backend` の所が `PELICAN_AUDIO_BACKEND` になります。[`parseAudioBackend`](../../src/player/main.cpp#L110))。
- `device` / `offline` を名指ししても、開けなければ名指ししないときと同じく `null` へ退避し、`fallback_reason` が付きます。
- **Studio の埋め込み player は Studio の環境を継承します**(QProcess)。Studio を `PELICAN_AUDIO_BACKEND=null` で起動すれば、埋め込み player は音を出しません。Studio の scenario driver(`tools/studio_drive.py`)は、呼び出し側が値を決めていない(未設定か空)ときに `null` を渡します([該当箇所](../../tools/studio_drive.py#L1278)、`d616e35a`)。
- **`PELICAN_WITH_AUDIO=OFF` の build** では `--audio-backend null` だけを受け付けます。`offline` / `device` は `This binary was built with PELICAN_WITH_AUDIO=OFF: --audio-backend <値> is unavailable` で起動を止めます(未知の値は、その前に `audio.backend_unknown@1`)。**環境変数は読みません**(継承した値で OFF の player が止まらないように)。

どれになったかは rpc の **`get_status` の `audio`** で読めます([`audioStatusJson()`](../../src/core/communication/rpcserver.cpp#L186))。

```json
"audio": {"available": true, "backend": "device"}
"audio": {"available": true, "backend": "null", "fallback_reason": "device: <開けなかった理由>"}
"audio": {"available": false}
```

- `backend` は `device` / `offline` / `null` のどれかで、**実際に使っているもの**です(起動で名指ししたかどうかを示す欄はありません。AU-3 でも形は同じ)。`fallback_reason` は null へ退避したときだけ出て、「目指したバックエンド名: 理由」の形です。名指しの `null` には付きません。
- `available: false` は `PELICAN_WITH_AUDIO=OFF` の build です。Audio module を作っていない host では `backend` が JSON の `null` になります。

### グラフで鳴らす — `audio.playback`(✅AU-1)

`audio.playback` は 1 ノードで 1 つの voice です。catalog の入力と parameter は次のとおりです([登録](../../src/project/graphmodule.cpp#L587))。

| 種類 | 名前 | 意味 |
|---|---|---|
| 入力(必須) | `time` | このノードの時計。`events` を繋ぐなら `scene_time` だけから来ること(違えば拒否。下記) |
| 入力(任意) | `events` | 再生制御イベント(`event.playback` の出力) |
| 入力(任意) | `position` | 分解版の「今鳴らす音源の秒」 |
| 入力(任意) | `source_begin` / `source_end` / `speed` / `delay` | 一体版の写像を edge で駆動するとき |
| parameter | `asset` | 鳴らす WAV / Ogg Vorbis(必須) |
| parameter | `bus` | `se`(既定)/ `bgm` |
| parameter | `wrap` | `once`(既定)/ `repeat` / `ping_pong` |
| parameter | `source_begin` / `source_end` / `speed` / `delay` | 一体版の写像を固定値で書くとき |

`asset` / `bus` / `wrap` を変えると **rebuild** です(voice を作るときに決まる値なので。catalog の `change.parameters`)。出力は `voice`(`pelican.audio.voice@1`)で、`audio.volume` / `audio.pitch` の `voice` 入力へ繋ぎます。

組み方は **一体版** と **分解版** の 2 通りで、同じ写像を書けば **同じ音源時刻を指します**。違うのはループ点で途切れるかどうかだけです。

#### 一体版 — 写像を parameter で渡す

`value.playback` と同じ写像(`wrap` / `source_begin` / `source_end` / `speed` / `delay`)を `audio.playback` 自身に書きます。`source_end` を省くと音源の長さ、範囲の長さがそのまま 1 周の長さです(speed 1 で実時間)。

**一体版でも `events` は任意です**(✅AU-3。AU-2 までは必須で、無いと `unconnected graph input: <node>.events` で拒否されていました)。

- `events` も `position` も無いノードは、**owner の最初のフレームで鳴り始め**、そのフレームから `time` 入力の時計で経過時間(写像の elapsed)を数えます([開始](../../src/core/userpublic/valuegraph.cpp#L875))。分解版の「`events` が無ければ owner の最初のフレーム」と同じ規則です。
- `events` が無いときの `time` は、どの時計から来てもかまいません(`time` でも `scene_time` でも、時計の無い定数でも拒否されない)。
- `time` は必須のままです。`events` を繋ぐなら `time` は `scene_time` だけから、の検査(`audio.clock_mismatch@1`、下の「状態と拒否」)も変わりません。
- Studio の palette で置いた `audio.playback` は、`time` に scope の `scene_time`(`value.input`。無ければ `scene_time` / `scene_time_2` … の id で 1 つ足す)が繋がった形で置かれ、そのまま鳴ります(ノードと時計は 1 回の編集なので、Undo 1 回で両方消えます。[palette の雛形](../../src/devstudio/model/graphcanvasmodel.cpp#L2252)、`d616e35a`)。手で組むときも `scene_time → time` を 1 本繋げば鳴ります。

下の例は `events` で鳴らす形です。

```json
{"schema": "pelican.graph", "version": 1,
 "definitions": {
   "engine_hum": {
     "nodes": [
       {"id": "clock", "op": "value.input", "parameters": {"name": "scene_time"},
        "outputs": ["value"]},
       {"id": "cue", "op": "event.playback", "outputs": ["events"]},
       {"id": "voice", "op": "audio.playback",
        "parameters": {"asset": "project://assets/audio/hum.wav",
                       "wrap": "repeat", "source_begin": 0.5, "source_end": 1.5},
        "inputs": {"time": {"node": "clock", "port": "value"},
                   "events": {"node": "cue", "port": "events"}},
        "outputs": ["voice"]}
     ],
     "outputs": {}}},
 "nodes": [
   {"id": "car_hum", "op": "graph.instance", "definition": "engine_hum",
    "owner": {"scene": "default_scene", "object": 1}}],
 "outputs": {}}
```

(`rendering` など pass 文書の他のキーは省略しています。)ゲームコードからは `ctx.render().emitValueGraphEvent(object, "car_hum/cue", Animation::CurvePlaybackCommand::start)` で鳴らします(§8.4。`pause` / `resume` / `reset` も同じ口)。

- `wrap` が `once` / `repeat` で、`speed` / 範囲 / `delay` が**固定値**かつ `speed > 0` のとき、voice は miniaudio のループ点で **sample 精度のネイティブループ**になります([判定](../../src/core/userpublic/valuegraph.cpp#L391))。voice は自分の時計で進み、フレームと mixer の普通のずれでは seek しません。**グラフの時刻と 0.25 s 以上離れたとき**(`set_time` で時刻が飛んだ、engine の時計がフレームを丸めた、など)だけ seek してグラフの時刻へ戻します([`nativeResyncSeconds`](../../src/core/loader/scenegraphruntime.cpp#L19))。
- それ以外(`ping_pong`、負の `speed`、写像のどれかを edge で駆動)は、下の分解版と同じ追従で鳴ります。
- `delay` の間と、`once` で範囲の終わりを過ぎた後は止まっています。

#### 分解版 — `value.playback → position`

写像は `value.playback`(あるいはどんなグラフでも)で計算し、その値を `position` に繋ぎます。`position` は「今鳴らす音源の秒」です。

```json
{"id": "map", "op": "value.playback",
 "parameters": {"wrap": "repeat", "source_begin": 0.5, "source_end": 1.5, "duration": 1.0},
 "inputs": {"elapsed": {"node": "clock", "port": "value"}},
 "outputs": ["value"]},
{"id": "voice", "op": "audio.playback",
 "parameters": {"asset": "project://assets/audio/hum.wav"},
 "inputs": {"time": {"node": "clock", "port": "value"},
            "events": {"node": "cue", "port": "events"},
            "position": {"node": "map", "port": "value"}},
 "outputs": ["voice"]}
```

- voice は毎フレーム `position` に追従します。音源の長さを法として自分のカーソルと比べ、**`dt + 10 ms` を超えてずれたら seek** します([許容幅](../../src/core/loader/scenegraphruntime.cpp#L191))。`set_time` で時刻が飛べば 1 回 seek して付いていきます。
- **`position` が前のフレームから動かないフレームでは voice は止まります**(時計を pause すれば音も止まる)。
- `position` を繋いだノードでは **`events` は任意**です。`events` が無ければ、owner の最初のフレームで鳴り始めます。
- `events` もあるときは、**voice 自身の pause イベントが `position` より優先**します(`position` が進み続けていても止まる)。resume すると、その時点の `position` へ 1 回 seek します。
- 分解版は `value.playback` を通すので、ループ点では seek で戻ります(一体版のネイティブループと違い、そこで途切れうる)。

#### 状態と拒否

- 各ノードは **idle / playing / paused** の状態機械を持ちます。`start` で(もう一度 `start` すれば voice を作り直して)playing、`pause` で paused、`resume` で playing に戻り、`reset` で idle(voice を止める)。
- `position` と写像(`wrap` / `source_begin` / `source_end` / `speed` / `delay` のどれか。parameter でも edge でも)を 1 ノードに同時に書くと **`audio.playback_mapping_conflict@1`** `{node, parameters}`([valuegraph.cpp](../../src/core/userpublic/valuegraph.cpp#L97))。
- `events` を繋いだノードで、`time` が読む時計(上流を辿って届く `value.input` のうち `time` / `scene_time` の名前の集合)が **`scene_time` だけ**でないと **`audio.clock_mismatch@1`** `{node, events_clock: ["scene_time"], time_clock}`(✅AU-2、[events の時計の検査](../../src/core/userpublic/valuegraph.cpp#L110))。`time` に `time` を繋いだもの、`time` と `scene_time` の両方が届くもの、**時計の無い定数**(`time_clock` が空)も拒否されます。一体版にも分解版にも掛かります。
- さらに `position` も繋いだノードで、`position` が読む時計が `time` の時計と違うと **`audio.clock_mismatch@1`** `{node, events_clock, position_clock}`([position の時計の検査](../../src/core/userpublic/valuegraph.cpp#L122))。`position` の上流に時計が無いとき(定数など)はこの検査をしません。
- `bus` が `se` / `bgm` 以外なら `unknown audio bus: <名前> (a voice plays on se or bgm)`。

#### events を使うなら `time` は `scene_time`(✅AU-2 で強制)

イベントは **シーン時刻**で刻印されます(`emitValueGraphEvent` はシーン開始からの経過時間を付けて積む)。ノードはそのイベントを `time` 入力の時刻と比べて取り出すので、`time`(アプリ時刻)で読むと `start` から数えた経過時間がシーン開始時刻の分だけ多くなり、再生位置がその分ずれます。

そのため **`events` を繋いだ `audio.playback` の `time` は `scene_time` だけから来なければならず**、違えばグラフを lowering する時点で上の `audio.clock_mismatch@1` で拒否されます。AU-1 の時点ではこれは規約で拒否されませんでしたが、AU-2 から強制です。AU-1 のころに `events` と `time` を組んだ文書は、`time` を `scene_time` に繋ぎ替えてください。アプリ時刻で鳴らしたい BGM は、次の「`time` に繋いで events 無し」の形で組みます。

⚠️ この検査は `audio.playback` だけです。**`value.event_clock` で `events` と `time` の時計を組み合わせた場合は検査していません**(同じずれが起きても拒否されない)。

#### シーンをまたいで鳴り続ける BGM — `time` に繋いで events 無し

**シーンの commit はそのシーンでグラフが鳴らした voice を全部止めます**([scene.cpp](../../src/core/loader/scene.cpp#L738))。シーンをまたいで同じ位置から鳴らしたい BGM は、位置を **アプリ時刻 `time`** から計算する分解版で組み、各シーンの owner に同じ定義を 1 つずつ付けます。

```json
{"schema": "pelican.graph", "version": 1,
 "definitions": {
   "bgm": {
     "nodes": [
       {"id": "now", "op": "value.input", "parameters": {"name": "time"}, "outputs": ["value"]},
       {"id": "map", "op": "value.playback",
        "parameters": {"wrap": "repeat", "source_begin": 0, "source_end": 120, "duration": 120},
        "inputs": {"elapsed": {"node": "now", "port": "value"}},
        "outputs": ["value"]},
       {"id": "voice", "op": "audio.playback",
        "parameters": {"asset": "project://assets/audio/theme.wav", "bus": "bgm"},
        "inputs": {"time": {"node": "now", "port": "value"},
                   "position": {"node": "map", "port": "value"}},
        "outputs": ["voice"]}
     ],
     "outputs": {}}},
 "nodes": [
   {"id": "title_bgm", "op": "graph.instance", "definition": "bgm",
    "owner": {"scene": "title", "object": 1}},
   {"id": "stage_bgm", "op": "graph.instance", "definition": "bgm",
    "owner": {"scene": "stage", "object": 3}}],
 "outputs": {}}
```

(`source_end` / `duration` は音源の長さに合わせてください。`object` は各シーンでの authoring object id です。§6.17「`definitions` と `graph.instance`」。)

- `events` が無いので、owner の最初のフレームで鳴り始めます。
- シーンを切り替えると前のシーンの voice は止まり、次のシーンの owner の最初のフレームで新しい voice が **その時点の `time` から計算した位置**で鳴り始めます。アプリ時刻はシーンをまたいで連続しているので、曲は同じ位置から続きます(voice は作り直すので、切り替えの瞬間は途切れます)。
- `scene_time` に繋ぐと、シーンごとに頭から鳴り直します。

#### フェード — `value.curve → audio.volume`

`audio.volume` の `voice` 入力に `audio.playback` の `voice` を繋ぎ、`value` に曲線を繋ぎます。

```json
{"id": "fade", "op": "value.curve",
 "parameters": {"keys": [{"time": 0.0, "value": 0.0}, {"time": 2.0, "value": 1.0}]},
 "inputs": {"time": {"node": "clock", "port": "value"}},
 "outputs": ["value"]},
{"id": "level", "op": "audio.volume",
 "inputs": {"value": {"node": "fade", "port": "value"},
            "voice": {"node": "voice", "port": "voice"}},
 "outputs": []}
```

`audio.volume` の値は **そのフレームの長さ(dt)をかけて直線で寄せます**([`rampSoundVolume`](../../src/core/loader/scenegraphruntime.cpp#L262))。フレーム境界で段が付かず、曲線を折れ線でなぞる音量になります。最初のフレームはその値から始まります(上の例なら無音から)。`audio.pitch` は寄せずにその値を設定します。

### 長い音の streaming(✅AU-2)

- header が言う長さが **10 秒を超える**音は、WAV でも Ogg Vorbis でも streaming で鳴ります(判定は [`Audio::prepareSound()`](../../src/core/audio/audio.cpp#L856)、10 秒は [`defaultStreamingThresholdSeconds`](../../src/core/audio/audio.cpp#L401))。
- 全体を decode しません。**rendering config 登録時のアセット検査も header だけを読みます**(WAV は短い WAV と同じ chunk の規則、Ogg Vorbis は decoder で header と長さを読む)。
- decode の cache が持つのは PCM ではなく **符号化された bytes** で、同じアセットを鳴らす voice の間で 1 つを共有します。voice はそれぞれ自分の decoder(`ma_decoder`)を持ち、再生しながら読みます。
- 一体版のネイティブループも、分解版の seek も、decode 済みの音と同じ意味で動きます(範囲・ループ点・開始位置は同じ frame 番号で指定します)。
- bytes は **ディスクから少しずつ読むのではなく、メモリに読み込んだものから** decode します(miniaudio の stb_vorbis に、ワイド文字のパスでファイルを開く入口が無いため)。streaming で減るのは PCM に展開した分のメモリで、ファイルの大きさ分はメモリに載ります。
- 10 秒は固定です。変えられるのはテスト用の口(`Audio::setStreamingThresholdForTesting`)だけで、project や rendering config からは変えられません。

### 残した穴(AU-2 の時点)

- **`ping_pong` と負の `speed`** はネイティブループにならず追従で鳴るので、**逆向きに進む区間では毎フレーム seek** になります。
- **seek の直後に、seek 前の音が少し鳴ります。** miniaudio の `ma_sound` が processing cache に読み込んでいた seek 前の frame(最大 1024 frame、48 kHz で約 21 ms)が先に混ざり、その後に seek 先の音になります(voice の位置は seek 先をすぐ返します)。分解版のループ点(seek で戻る所)にはその分の余分な音が混ざります。AU-1 から続く miniaudio の挙動です。
- **streaming の音は、本体が壊れていても登録時には分かりません**(登録時の検査は header だけ)。再生すると名前付きのエラーにならず、無音で終わります。
- `value.event_clock` の `time` に `time` を繋いで `events` と組み合わせた文書は検査していません(上の「events を使うなら `time` は `scene_time`」)。
- **ヘッドレスの本番実行では voice が進みません。** `offline` バックエンドのエンジンを誰も回さない(PCM を読み出す呼び出し元がいない)ためです。音の書き出しは v1 です。
- **窓付き player の実機で音を聞いての確認はまだです**(自動テストはデバイスを開けることの確認と、`offline` エンジンの PCM で行っています)。
- MP3 / FLAC は需要が出てから、3D 音響は 📐未実装です。

### 残した穴(AU-3 の時点)

- **`events` の無い voice は、owner がいったん不活性になって戻ると頭から鳴り直します。** 不活性の間はノードの状態を捨て([該当箇所](../../src/core/userpublic/valuegraph.cpp#L837))、戻ったフレームで改めて開始する(一体版は経過時間をそのフレームから数え直す)ためです。
- **`wrap: once`(既定)で `events` が無いと、owner の最初のフレームから 1 回だけ鳴って止まります。** 好きな時に鳴らしたいなら `events` を繋ぎます(ループさせたいなら `wrap: repeat`)。

## 8.12 永続化(✅WP65)

保存先は `user://`([第3章](03_project_format.md) §3.3。Windows 実体は `%APPDATA%/pelican/<プロジェクト name>/`)。プロジェクトディレクトリへの書き込みは設計上禁止です(読み取り専有原則)。

### 設定(pelican.settings v1)

```cpp
auto game = ctx.gameSettings();          // "game" 区画(任意 JSON)を取得
game["volume_preset"] = "quiet";
ctx.setGameSettings(game);
ctx.saveSettings();                      // user://settings.json へ atomic 書き込み
```

ファイル形式(`user://settings.json`):

```json
{ "schema": "pelican.settings", "version": 1,
  "engine": { "audio": { "bus_volumes": { "master": 1.0, "bgm": 1.0, "se": 1.0 } } },
  "game": { "volume_preset": "quiet" } }
```

- `engine` 区画はエンジン所有(`saveSettings()` が現在のバス音量を自動キャプチャ)、`game` 区画がゲームの自由領域です。トップレベルは 4 キー固定・未知キー拒否。
- 起動時に自動ロードされます。ファイル破損時はクラッシュせず WARN + 既定値で続行します。

### セーブデータ

```cpp
ctx.save().saveData("slot-a", nlohmann::json{{"level", 3}});  // user://saves/slot-a.json
auto loaded = ctx.save().loadData("slot-a");                  // optional<Json>(欠落/破損は nullopt)
auto slots  = ctx.save().listSaves();                         // SaveContext::SlotInfo{slot, timestamp} の昇順一覧
```

- スロット名はポータブルなファイル名検証付き(`.`/`..`・末尾ドット/空白・`/ \ # : " < > | ? *`・制御文字は例外)。中身はエンベロープなしの任意 JSON です。
- **例外挙動の非対称に注意**: 書き込み系(`ctx.saveSettings()` / `ctx.save().saveData()`)は失敗で例外、読み込み系(`ctx.save().loadData()`)は throw せず `nullopt` を返します(起動フローを壊さないため)。
- 書き込みはすべて atomic(tmp + rename)。削除 API(`deleteSave`)や rpc 経由の永続化メソッドはまだありません。
- **シーンロード中に書くと、ディスクへの反映はロードの成功後**です(§8.4 の囲み)。同じロードの中では `loadData` が書いた値を返し、`listSaves()` にも並びますが、その `timestamp` は **0**(まだファイルが無い)です。`onDestroy` から書いた場合、失敗しても例外にはならず `effect_errors` に記録されます。

## 8.13 アニメーション再生(✅WP38/94/97/101/102/176〜178、AG-1a/1b/2)

### 姿勢の 2 つの装着様式

**個体(owner)に骨格の姿勢を付ける書き方は 2 つ**あり、v0 ではどちらも契約です。

| | (1) animation コンポーネント | (2) `graph.instance` + `pose.*` |
|---|---|---|
| 書く場所 | シーン JSON の `animation`([第4章](04_scene_ecs.md) §4.2) | rendering config(`pelican.graph` root)の `nodes`([第6章](06_rendering.md) §6.17) |
| 中身 | **`clip` XOR `graph`**(クリップ 1 本か、`pelican.anim_graph` v1 文書か。どちらか片方**ちょうど 1 つ**が必須) | `pose.clip_sample` / `pose.blend` / `pose.state_machine` を `pose.skeletal` sink へ繋ぐ |
| ゲームコードからの操作 | `setAnimationGraphParameter` / `forceAnimationGraphState`(§8.4) | `setGraphParameter` / `emitGraphEvent`(§8.4) |
| 出荷の実物 | **宣言は無い**(`movement.anim_graph.json` は animgraph_demo のゲームコードが自分で読み込みます) | `projects/posegraph_demo` |

**この 2 つは同じ owner に共存しません。** 同じ個体に 2 本目の writer が来ると **`pose.sink_conflict@1`** で名前付きに落ちます —— 黙って上書きすることはありません。検査は二段で、文書レベルの proxy(`component_writer`)と、publish 時の **owner ごとの writer token**(フレーム改訂付き)の両方が見ています([posegraph.cpp:40](../../src/core/animation/posegraph.cpp#L40))。

> **「姿勢の正本を 1 本にまとめる」は v0 では撤回されました(✅AG-1b)。** 一本化には「シーンが持つ派生プログラムの置き場」と「評価器の状態を写す API」が先に要り、v0 にはどちらもありません。**v0 が約束するのは 2 つの経路の parity**(`loop` / `speed` / `start_time` の意味、位相、パラメータ編集をまたいだ状態の保持)**と owner 単位の排他**の 2 つだけです([docs/v0_freeze.md](../v0_freeze.md) §1.6)。

`animation` コンポーネントの受理キーは `name` / `clip` / `graph` / `parameters` / `speed` / `loop` / `start_time` の 7 つですが、**`graph` を書いた場合 `speed` / `loop` / `start_time` は拒否されます**(`field 'animation.<鍵>' is clip-only; graph states define their playback clocks`)。逆に `parameters`(グラフの初期パラメータ)は `graph` のときだけ書けます([componentcodec.cpp:990](../../src/core/loader/componentcodec.cpp#L1460))。

### ゲームコードから見た層

1. **宣言的クリップ再生(WP38)** — シーン JSON の `animation` コンポーネントの `clip`([第4章](04_scene_ecs.md) §4.2)。ゲームコード不要。時刻源は EngineTime(決定的)。
2. **アニメーショングラフ(WP101/102)** — `pelican.anim_graph` v1 のデータアセット + **非特権の公開評価器**。**装着の仕方が 2 通り**あります: シーンの `animation.graph` に書けば**エンジンが評価器を束縛してくれ**(`setAnimationGraphParameter` / `forceAnimationGraphState` が効く相手はこれです)、ゲームコードが `#include <animation/animgraph.hpp>` で `EvaluatorV1` を自分で持つこともできます。後者はエンジン特権なしで動きます。

グラフ JSON(実物: [../../projects/animgraph_demo/movement.anim_graph.json](../../projects/animgraph_demo/movement.anim_graph.json) 全文):

```json
{
  "schema": "pelican.anim_graph",
  "version": 1,
  "parameters": { "speed": 0.0, "jump": 0.0 },
  "initial_state": "Move",
  "states": [
    { "name": "Move", "type": "blend1d", "parameter": "speed", "clips": [
      { "threshold": 0.0, "clip": "Walk", "speed": 1.0, "start_offset": 0.0, "loop": true },
      { "threshold": 1.0, "clip": "Run",  "speed": 1.6, "start_offset": 0.0, "loop": true }
    ] },
    { "name": "Jump", "type": "clip", "clip": "Jump", "speed": 1.0, "start_offset": 0.0, "loop": false }
  ],
  "transitions": [
    { "from": "Move", "to": "Jump", "priority": 100, "interrupt": "always", "duration": 0.12,
      "conditions": [ { "parameter": "jump", "op": ">",  "value": 0.0 } ] },
    { "from": "Jump", "to": "Move", "priority": 100, "interrupt": "always", "duration": 0.18,
      "conditions": [ { "parameter": "jump", "op": "<=", "value": 0.0 } ] }
  ]
}
```

(`clip` はモデル glb 内のクリップ名。`parameters` は名前 → 初期値、遷移は `priority` / `interrupt`("never" / "higher_priority" / "always")/ `duration`(クロスフェード秒)/ `conditions` を持ちます)

使い方(実物: [../../projects/animgraph_demo/code/animgraphdemo.cpp](../../projects/animgraph_demo/code/animgraphdemo.cpp)):

```cpp
#include <animation/animgraph.hpp>

auto doc = Pelican::AnimationGraph::parseDocumentV1(json_text);
Pelican::AnimationGraph::EvaluatorV1 evaluator{doc, "Player"};  // シーンの named object

void update(Pelican::GameContext &ctx) {
    if (!evaluator.bound()) { (void)evaluator.bind(); return; }  // モデル未生成時はリトライ
    (void)evaluator.setParameter("speed", speed);
    (void)evaluator.setParameter("jump", jumping ? 1.0 : 0.0);
    (void)evaluator.prepareTick(ctx.time(), ctx.deltaTime(), ctx.frameIndex());
}
```

**この「ゲームコードが `EvaluatorV1` を自分で持つ」書き方をするオブジェクトには、`animation` コンポーネントを付けません** —— 評価器が自分で pose sink を claim するので、コンポーネントを付けると 2 本目の writer になります(上の排他と同じ話です)。一方、**シーンの `animation.graph` に同じ文書を書く**書き方なら、束縛も再束縛もエンジンがやります。`forceState` / `getStatus()`(現在ステート・遷移・semantic pose hash)も公開されています。v1 の語彙は clip + blend1d + crossfade + interrupt("never" / "higher_priority" / "always")のみで、layer・2D blend は v2 予約(キーを書くとエラー)。

3. **自作評価器(上級・WP94/102)** — 凍結 C-ABI([../animation_abi_v1.md](../animation_abi_v1.md) が正)+ versioned service table。ゲーム DLL が公開ヘッダのみで pose sampling / blend / commit を駆動できます(標準評価器自体がこの面の dogfood)。

4. **VRM 表情・視線・一人称(✅WP123b/134 = S1b/S1c)** — VRM 1.0 モデルの表情を per-instance で動かす公開サービスです:

```cpp
#include <animation/vrm_application_v1.hpp>

auto *svc = Pelican::Vrm::getApplicationServiceV1();   // versioned negotiation
Pelican::Vrm::SetExpressionInputDescV1 input{};
// instance handle は animation service の resolve 系で取得
input.expressions = {{ "happy", 1.0 }};                 // 表情名 + weight [0,1]
input.look_at_yaw_degrees = yaw; input.look_at_pitch_degrees = pitch;
svc->set_expression_input(input);
```

- 標準の evaluator が公開 phase(snapshot → lookAt → resolve → commit)で morph weight / マテリアル色 / textureTransform を **atomic に publish** します(解決式は `Base + Σ((Target−Base)×weight)`、binary 表情は ≥0.5 で 1)。MToon 系の色 bind(shadeColor 等)は WARN + skip(公開 diagnostics で照会可)。
- **bone lookAt / firstPerson(S1c)**: `animation/pose_staging_v1.hpp` の pose staging で eye bone の回転を段階適用。firstPerson `auto` はロード時に頭部メッシュを決定的に分割し、**XR の両眼 = 頭部非表示 / flat・ミラー = 全身**が自動で切り替わります。
- 完成例は [projects/vrm_xr_demo](../../projects/vrm_xr_demo)(表情サイクル + 視線追従 + XR。エンジン変更ゼロで userpublic ヘッダのみ)。

5. **VRMA typed AnimationSource(✅WP176〜178)** — `.vrma` を
   `VRMC_vrm_animation` 1.0 の GLB alias として decode し、body / expression /
   gaze を別 channel のまま保持します。版付き retarget profile が humanoid
   mapping、rest/T-pose 差、optional bone、hips scale を解決し、named source
   として既存 graph の `clip` / `blend1d` から利用できます。expression / gaze
   は joint Pose に混ぜず、body と同じ frame revision で VRM application sink
   へ送られます。

VRMA source/profile の差し替えは generation を進め、旧 metadata/cursor/graph
pose を `stale_generation` にします。`EvaluatorV1::rebind()` は新 rig/profile
へ再束縛し、古い cursor time や transition snapshot を暗黙再利用しません。
ただし現在は decoder/retarget/source registry の接続面までで、project asset の
自動 FileWatcher 配線は未実装です。

制限: native clip path は joint 128 / 補間 LINEAR・STEP、morph の weight 駆動は
VRM expression service 経由のみ(通常クリップの weights channel は非対応)。
**IK はありません。** **root motion(クリップの根の移動を個体の transform へ渡すこと)は ✅AG-2 で入りました** —— 下の小節です。ただし VRMA の hips を自動で root motion として解釈することはしません。決定性は
「同一ビルドで byte 一致」です。

### 姿勢グラフ(`pelican.pose`)

装着様式 (2) の中身です。ノード語彙そのもの(module / 型 / 契約の読み方)は [第6章](06_rendering.md) §6.17 にあり、ここでは**アニメーション側の規則**だけを書きます。

| op | パラメータ / ポート |
|---|---|
| `pose.clip_sample` | `parameters`: `rig`(必須)/ `clip`(必須)/ `speed`(既定 1)/ `loop`(既定 true)/ `start_time`(既定 0)/ `root_motion`(`"none"`(既定)か `"extract"`)/ `root_node`(`extract` のとき必須)。入力 `time`(スカラー)→ 出力 `pose` |
| `pose.blend` | 入力 `a` / `b`(pose)と `weight`(スカラー)→ 出力 `pose` |
| `pose.state_machine` | `parameters`: `rig`(必須)/ `graph`(`pelican.anim_graph` v1 文書を**そのまま埋め込む**)。**文書の `parameters` の名前がそのまま入力ポートになります**(`time` は予約済みで、同名のパラメータは拒否)→ 出力 `pose` |
| `pose.retarget` | `parameters`: `profile`(必須)。入力 `pose` → 出力 `pose`(同じ姿勢をそのまま渡すので root motion の経路に挟めます) |
| `pose.skeletal` | **sink**。入力 `pose`。owner 個体の姿勢としてコミットする |
| `pose.transform` | **sink**。catalog には載りますが **v0 では装着時に必ず拒否されます**(`pose.transform has no root writer yet`。[posegraphruntime.cpp:374](../../src/core/animation/posegraphruntime.cpp#L433)) |

規則:

- **pose の op は `owner` 付きの `graph.instance` の中にしか置けません。** owner の無い場所の pose ノードは書き込む先が無いので、`pose operation requires an owned graph instance: <id>` で断られます([posegraph.cpp:100](../../src/core/animation/posegraph.cpp#L100))。
- **pose ノードが読めるスカラー以外の入力はありません。** pose 以外のノードから来る edge は `value` ポートだけが許され、それ以外は `pose operations read scalar inputs only` です。評価は **animation phase** で、同じフレームの値評価より 1 段早く走ります —— つまり **スカラー → 姿勢の一方向**で、逆向きはありません([第6章](06_rendering.md) §6.17)。
- **ポートを流れる型は `pelican.pose@1`(opaque)だけ**です —— **staged pose のハンドル + レイアウト世代 + フレーム改訂**であって骨の bytes ではありません。レイアウトの食い違いは評価時に `pose.layout_mismatch@1` で名前が付きます。pose のコントラクトはこの `pose` とスカラーの `time` / `weight` だけで組まれています([posegraphmodule.cpp:114](../../src/project/posegraphmodule.cpp#L114))。`pelican.rig@1` も opaque な型として登録されていますが、**どのポートにも現れません** —— リグは各ノードの `parameters.rig`(必須の文字列)で指す**同一性**であって、運ばれる値ではないからです([posegraphmodule.cpp:142](../../src/project/posegraphmodule.cpp#L142)、[posegraphmodule.hpp:15](../../src/project/posegraphmodule.hpp#L15))。
- **`pose.state_machine` の出力は、`pose.skeletal` sink ちょうど 1 本にしか繋げません。** 包んだ評価器が自分の個体の writer を持っているので、2 本目に渡すのも `pose.skeletal` 以外の op に渡すのも `pose.sink_conflict@1` です。繋ぎ先が 1 本も無い文書も拒否されます([posegraph.cpp:207](../../src/core/animation/posegraph.cpp#L207))。

**parity(装着様式 (1) と同じ挙動になること)の中身**は次の 4 つです(✅AG-1b):

1. **`loop: false` は clamp** です。クリップの外へ出た時刻は先頭/末尾に貼り付き、wrap しません([posegraphruntime.cpp:38](../../src/core/animation/posegraphruntime.cpp#L40))。
2. **キーが 0 秒から始まらないクリップの位相ずれ**は animation service 側で揃えており、`time_seconds` は **絶対クリップ時刻**として報告されます(この 1 本は実装を読んだ範囲での理解で、装着様式をまたいだ数値比較までは未確認です)。
3. **instance のパラメータを変えても、ステートマシンの現在ステートとクリップのカーソルは保たれます。** 退役するプログラムの評価器を donor として取っておき、**owner + ノード id + 埋め込んだ `anim_graph` 文書が一致すれば `rebind()` で引き継ぐ**からです([posegraphruntime.cpp:404](../../src/core/animation/posegraphruntime.cpp#L463))。
4. 逆に **`graph` の文書そのものを変えて再公開したときは引き継ぎません** —— 一致条件に文書が入っているので、新しい評価器が `initial_state` から始まります。シーンの読み直しでも初期化されます。

### root motion(✅AG-2)

`pose.clip_sample` に `root_motion: "extract"` と `root_node`(glTF のノード index)を書くと、**クリップが根のノードに書いている移動が、姿勢の中ではなく owner 個体の transform へ出て行きます**。

- **差分は「折り返す前の時刻」で取ります。** ノードの時刻写像を u = (`time` − `start_time`)·`speed` + クリップ開始 とし、R(u) = loops(u)·(root(末尾) − root(先頭)) + root(canonical(u))、移動量 = R(今フレーム) − R(前フレーム)。この 1 本の式で **`speed < 0` も、1 フレームに 1 周以上進む場合も、境界ちょうども**同じように扱えます([posegraphruntime.cpp:312](../../src/core/animation/posegraphruntime.cpp#L369))。
- **`set_time`(シーク)は移動ではありません。** 時刻が外から差し替えられたフレームは前回値を捨て、その 1 フレームの移動量は 0 になります。一方、**`value.curve` のキーで `time` が跳んだ場合は「移動」として扱われます** —— この経路には「不連続だ」と知らせる信号が無いためです。カットを作りたいなら `set_time` を使ってください。
- **適用先は owner の `localtransform`** です。`local.pos += local.rotation * delta` で足し込みます(**スケールは掛けません** —— 掛けると拡大した個体が同じクリップで違う距離を進んでしまうため)。姿勢の側では根ノードの translation が **クリップ先頭の値に固定(pin)**されるので、メッシュが個体の中で滑ることはありません。**回転量の抽出は v0 の射程外**です。
- **名前付きの拒否は 4 種類**です。
  - `pose.root_motion_unsupported@1` — 抽出ノードの下流に `pose.blend` / `pose.state_machine` がある(`pose.retarget` は挟めます)/ クリップの長さが 0 / 根ノードに translation チャンネルが無い
  - `pose.root_node_unknown@1` — `root_node` が無い・負・範囲外・リグのレイアウトに無い
  - `pose.root_motion_no_transform@1` — sink が書く個体に `localtransform` が無い(**黙って無効化しません**)
  - `pose.root_motion_shared@1` — 1 つの抽出ノードの姿勢を 2 つの消費者が受け取っている(1 本のクリップで 2 個体が動くことになるため)
- **末尾キーを先頭キーと同じにした「継ぎ目のない」クリップは、1 周の正味変位が 0 なので動きません。** 移動させたいなら**正味の変位を末尾キーに残す**のがクリップ側の規約です。
- `localtransform` は **`transform` を持つシーンオブジェクトには必ず付く**ので(✅GP-1、[第4章 §4.2](04_scene_ecs.md))、root motion のために宣言する必要はありません(以前は親子関係に参加する個体にしか付かず、単独の個体には手書きの宣言が要りました)。**親を持つ個体は 1 フレーム遅れます**(同じフレームでは local をそのまま world として描き、次のフレームに `LocalTransformSystem` が world を組み直すため)。
- 出荷プロジェクトに `root_motion` の宣言はありません。実物は `test/root_motion_fixture.hpp` と `skeletal_rootmotion_test` です。

### 走っている姿勢を観測する(✅AG-3a)

`get_status` の `animation` キーが、そのフレームに姿勢を書いた sink を並べます([第10章](10_tools.md))。`PELICAN_WITH_ANIMATION=OFF` の build では `{"available": false}` だけです。

```json
{"available": true, "sinks": [
  {"owner": {"scene": "default_scene", "object": 1}, "node": "hero_walk_run/out",
   "source": "pose_graph_clip",
   "cursors": [{"clip": "Walk", "time_seconds": 0.5, "normalized_phase": 0.5}],
   "frame_revision": 42, "pose_hash": 12345}]}
```

- **`source` は 4 値**です: `pose_graph_clip` / `pose_graph_state_machine`(装着様式 (2))と `component_graph` / `component_clip`(装着様式 (1))。**両方の装着様式が同じ 1 つの表に出ます** —— コンポーネントで動いているシーンが「アニメーションの無いシーン」に見えないためです(pose graph 側は [animationstatus.cpp:54](../../src/core/communication/animationstatus.cpp#L54)、コンポーネント側の 2 値は [:87](../../src/core/communication/animationstatus.cpp#L87))。
- `cursors[].normalized_phase` は **[0, 1] に正規化した位置**で、キーが 0 秒から始まらないクリップでも最初のキーで 0、最後のキーで 1 になります。ただし**これはクリップごとの値ではなく、ステートの位相**です —— ステートの中に複数クリップがあると、位相はリーダークリップの尺で進み([animgraph.cpp:591](../../src/core/userpublic/animation/animgraph.cpp#L592))、**同じステートのすべてのクリップが同じ値を報告します**([:894](../../src/core/userpublic/animation/animgraph.cpp#L894))。クリップ個別の位置が要るときは `time_seconds` を見てください。
- `state` と `transition`(`{active, target, progress}`)は **ステートマシンを含む chain にだけ**出ます。キーごと無いので、「遷移していない」と「そもそもステートマシンが無い」を取り違えません。
- `root_delta` は **root motion を抽出している chain にだけ**出ます。値は**クリップ自身の空間での移動量**で、個体の回転を掛ける前の数字です(実際に進んだ距離ではありません)。
- 観測は**読むだけ**です。`get_status` を同じフレームで何度呼んでも時計は進みません。

### `PELICAN_WITH_ANIMATION=OFF` の見え方

アニメーションはビルド機能ユニットなので、外した build が存在します([第2章](02_getting_started.md))。そのとき何が起きるかは**全部名前が付いています**。

- プロジェクトが `project.json` に `requires: ["animation"]` を書いていれば、**プロジェクトを読む時点で** `animation.module_unavailable@1` になり、player は名前を出して exit 1 します([第3章](03_project_format.md))。
- `animation` コンポーネントを含むシーンは **prepare で** `animation.module_unavailable@1`。prepare の失敗なので rollback 契約に乗り、**古いシーンがそのまま残ります**([第4章](04_scene_ecs.md) §4.3)。
- グラフ文書に `pose.*` を書いていれば `graph.operation_unregistered@1` です —— `pelican.pose` module がそもそも登録されないためです([第6章](06_rendering.md) §6.17)。
- **ゲーム DLL はリンクできて、ロードもできます。** 凍結 ABI の交渉入口(`getApiV1` / `getPoseStagingServiceV1` / VRM の `getApplicationServiceV1`)は stub として残り、**`unsupported_version` を返します** —— SDK に対してビルドした DLL が「このバイナリにはアニメーションランタイムが無い」ことを**名前で知る**ためです([animation_disabled.cpp:13](../../src/core/animation/animation_disabled.cpp#L13))。
- `GameContext` 側では `ctx.animation().setParameter()` / `ctx.animation().forceState()` が `not_found` を返します(§8.4)。

## 8.14 2D スプライトの操作(✅WP103/106)

シーン JSON の `sprite_view`([第4章](04_scene_ecs.md) §4.2)に加えて、C++ から生成・操作できます(実物: [../../projects/sprite_demo/code/flipbook_demo.cpp](../../projects/sprite_demo/code/flipbook_demo.cpp)):

```cpp
auto id = ctx.createSpriteObject(transform, sprite);   // SpriteViewComponent を直接渡す

auto view = ctx.spriteView(id);                        // 読み → 書き戻しで反転
if (view) { view->flip_x = !view->flip_x; (void)ctx.setSpriteView(id, *view); }
```

パラパラアニメは**エンジン機構ではなく非特権の標準ライブラリ** `FlipbookClip` で行います(呼び手が決定的な local time を渡す — ECS もクロックも持たない):

```cpp
#include <sprite/flipbook.hpp>

const Pelican::sprite::FlipbookClip walk{
    {{"demo_atlas#sprite/left", 0.20}, {"demo_atlas#sprite/right", 0.20}},
    Pelican::sprite::FlipbookPlayback::loop};

walk.apply(ctx, id, local_time);   // local_time はゲームが管理(例: 接地中のみ加算)
```

2D の移動・接地は §8.8.1 の `platformer::moveAndSlide`、pixel perfect は [第6章](06_rendering.md) §6.9 を参照してください。`projects/sprite_demo` が「入力 → moveAndSlide → transform 反映 → flip/flipbook → 着地イベント emit」を 1 ファイルで通した完成例です。

## 関連文書

- [../design_game_logic_native.md](../design_game_logic_native.md) — ネイティブゲームロジックの設計
- [../design_object_behaviors.md](../design_object_behaviors.md) — オブジェクト behavior(v2.1。§8.5 の設計側。実装と食い違う箇所はコードが正)
- [../design_event_layer.md](../design_event_layer.md) — イベント層 v1.1(E1/E2 実装済み、Stay は未採用)
- [../design_scene_flow.md](../design_scene_flow.md) — シーン遷移(S2 は未実装)
- [../design_physics_queries.md](../design_physics_queries.md) — 物理クエリと将来のシミュレーション方針
- [../design_camera_system.md](../design_camera_system.md) — カメラ三層
- [../design_determinism_services.md](../design_determinism_services.md) — 決定的乱数とシード規約
- [../design_audio.md](../design_audio.md) — オーディオ
- [../design_persistence.md](../design_persistence.md) — user:// と設定/セーブ三区分
- [../design_animation_graph.md](../design_animation_graph.md) — アニメーショングラフと公開評価器の設計(§8.13)
- [../animation_abi_v1.md](../animation_abi_v1.md) — 凍結 C-ABI(自作評価器の面)の正
- [../v0_freeze.md](../v0_freeze.md) — v0 の凍結一覧。§1.6 が「rendering config の root と 2 つの装着様式」、§1.3 が共通グラフの op / 型 / sink
- [第4章 シーンと ECS](04_scene_ecs.md) / [第7章 入力と UI](07_input_ui.md) / [第10章 ツールリファレンス](10_tools.md) / [第13章 エディタ](13_editor.md)
