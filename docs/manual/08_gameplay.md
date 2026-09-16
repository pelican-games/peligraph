# 第8章 ゲームロジック

対象: pelican2(2026-09-04 時点、HEAD=`d146b8bd`)/ このマニュアルはコードを正とする

## この章で学ぶこと

- ゲームコード(C++)の書き方 — システム登録・`GameContext`・オブジェクト操作・DLL ホットリロード
- ゲーム DLL の ABI(登録入口・epoch anchor・古い DLL の拒否)と schema の宣言面
- **オブジェクト behavior**(`PELICAN_REGISTER_BEHAVIOR` — シーンに貼れる振る舞い。✅WP155/162/167)
- イベント層(emit と購読、フレーム境界配送)
- シーン遷移・カメラ(コントローラ含む)・物理クエリ(shapeCast / moveAndSlide)の使い方
- 決定性(固定ループ順序・決定的乱数・シード)— 「なぜ毎回同じ結果になるのか」
- オーディオ・永続化(設定/セーブ)・アニメーション・2D スプライトの操作

(本文の `✅WP155` のような `WP<数字>` は、エンジン側の作業単位(work package)に付く通し番号です — [用語集](14_glossary.md)。「どの作業で入った機能か」の目印として添えています)

## 8.1 ゲームコードの位置づけ

> **設計決定(ネイティブ一本・2026-07-07 ユーザー決定):** ゲームロジックは C++ で書く。スクリプト言語は不採用。エンジンは「ソース同居の SDK」。

✅WP90(G2)で、ゲームコードのビルド形態は静的リンクから **`pelican_game_logic.dll`** に移行しました。公開面(userpublic)だけを import する DLL で、実行中のホットリロード(F5 / 自動検出 / rpc)に対応します。リロードは v1 では **full-reset 方式**です — 差分を当てるのではなく、物理 → ECS → モデルインスタンス → 遅延ミューテーション → 未配送イベントの順に破棄し([runtime_teardown_order](../../src/core/appflow/teardown.hpp#L23))、現在のシーンを起動時と同じロード経路で組み直します([rebuildCurrentScene](../../src/core/gamelogic/gamelogicreload.cpp#L853))。C++ のオブジェクトは DLL ごと入れ替わるので、システムや behavior のメンバ変数に持っていた値は残りません。詳細は [第10章](10_tools.md) §10.5。

> **設計決定(レイヤ規則):** ゲームコードが include してよいのは `src/core/userpublic/` のヘッダのみ。エンジン内部のモジュール取得(`GET_MODULE`)をゲームコードから直接呼ぶことは禁止で、入力・時刻・オブジェクト・物理・音・シーン・乱数はすべて **`GameContext`** ファサードを通す。

ビルドの組み込み方([第2章](02_getting_started.md) §2.4):`code/CMakeLists.txt` に `pelican_game_sources(<ソース>...)` と書き、CMake configure 時に `-DPELICAN_PROJECT=<プロジェクトパス>` を渡します。

### ゲーム DLL の ABI(✅WP381/381a/382a)

ふだん意識する必要はありませんが、**古い DLL が黙って動く事故を防ぐ**ための仕掛けがあるので、エラーが出たときのために書いておきます。

- ゲーム DLL がホスト実行ファイルから import する登録入口は `pelican_register_behavior_v2` / `pelican_register_event_v2` / `pelican_register_system_v2` / `pelican_emit_v2` と、コンポーネントの `__registerComponent` です。加えて **epoch anchor** [`pelican_game_logic_epoch_v2`](../../src/core/userpublic/gamelogic.hpp#L23) を必ず import します(SDK が自動で埋めます)。epoch anchor とは、ABI の世代番号を名前そのものに含んだ関数シンボルのことです([用語集](14_glossary.md))。世代が上がると名前ごと変わるため、古い SDK でビルドされた DLL は「host に存在しない名前を import している DLL」として機械的に見分けられます。
- ロードの**前**に、DLL の PE import テーブルだけを読む診断([diagnoseGameLogicImports](../../src/core/gamelogic/gamelogicimports.cpp#L559))が走ります。期待する epoch anchor が無い / 旧名の入口(`__registerBehavior` など)が残っている DLL は [`game_logic_abi_stale@1`](../../src/core/gamelogic/gamelogicimports.hpp#L40) で拒否され、**一度も実行されません**。ordinal import と delay-load import も `game_logic_abi_unsupported_import@1` で拒否です。`game_logic_abi_stale@1` のような `名前@数字` は**エラーコード**で、人間向けのメッセージ本文とは別に、rpc の応答へそのまま出る機械可読な識別子です([用語集](14_glossary.md))。
  - registration を持たない provider 専用の古い DLL には、リンカが強制する要素が無いため、**この診断が唯一の門番**です。
- **再帰 schema**(フィールドの中にさらにフィールドを持つ、木になった schema)を宣言した型があると、SDK は `_v3` 入口([registerer.hpp](../../src/core/userpublic/details/behavior/registerer.hpp#L322))を**自動で選びます**。v2 の入口は平らなフィールド列しか運べないためです。判定は「`List` か `Flags` のフィールドが 1 つでもあるか」だけで([schemaRequiresV3](../../src/core/userpublic/details/schema/structfieldschema.hpp#L823))、§8.5.3 の `List<Row>` は行の型 `Row` が自分の `schema` を持つのでこれに当たります。書き手のコードは変わりません。v3 入口を使う DLL と v2 だけの DLL は同じホストに**同居できます**。
- 逆に、ホストが古くて v3 入口を持たないのに DLL が v3 を使っていると、`game_logic_abi_stale@1{found: "v3", host: "v2"}` で拒否されます。

実行中の DLL は原本ではなく **shadow copy**(別ディレクトリへ取った複製)を読みます。リロードのたびに連番付きの複製を作ってそちらをロードするので([makeShadowCopy](../../src/core/gamelogic/gamelogicreload.cpp#L299)。`.pdb` も一緒に複製するのでデバッガも繋がります)、エンジンが原本のファイルを掴んだままにならず、走らせたままビルドし直せます。置き場所は原本の隣の `.pelican-hot-reload/<プロセス ID>/` で([gamelogicreload.cpp](../../src/core/gamelogic/gamelogicreload.cpp#L427))、**プロセスごとに別ディレクトリ**なので同じプロジェクトを 2 つ起動しても取り合いになりません(死んだプロセスの残骸は起動時に掃除されます)。ビルドが原本を掴んだままだと、リロードは失敗ではなく保留になり、何度ポーリングしても掴まれたままなら [`game_logic_source_busy@1`](../../src/core/gamelogic/gamelogicreload.cpp#L743) が status に出ます。

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

- `dt` は delta time(前フレームからの経過秒)。`ctx.deltaTime()` は最初のフレームや rpc `set_time` の直後に 0 になり得るので([EngineTime::setTime](../../src/core/appflow/enginetime.cpp#L44) が 0 に戻します)、その場合は 1/60 秒で代用しています。
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

rpc の `step_frame` も**同一順序**を再現します。「rpc 駆動のテスト = 実行時と同じ意味論」がこれで保証されます。

## 8.4 GameContext API 一覧

`GameContext` は状態を持たない薄いファサードです([gamecontext.hpp](../../src/core/userpublic/gamecontext.hpp))。

| カテゴリ | API | 備考 |
|---|---|---|
| 入力 | `actionsConfigured()` / `actionPressed/Released/Held(name)` / `actionAxis1(name)` / `actionAxis2(name)` / `actionPose(name)` | 未知アクション名は名前入り例外。`actionPose` は ✅WP132 で実動作(下記) |
| 時刻 | `time()` / `deltaTime()` / `frameIndex()` | |
| ログ | `logInfo/Warning/Error(msg)` | rpc モードでは stdout が使えないため、ログは必ずこれで |
| オブジェクト | `createObject(LocalTransformComponent)` / `removeObject(id)`→bool / `localTransform(id)` / `setLocalTransform(id, t)`→bool | `setLocalTransform` は描画側へも即時伝播 |
| スプライト | `createSpriteObject(transform, sprite)` / `spriteView(id)`→optional / `setSpriteView(id, s)`→bool / `setSpriteTexture(id, tex)`→bool | §8.14(✅WP103) |
| 物理 | `raycastClosest(ray)` / `raycastClosest(ray, filter)` / `raycastAll(ray, filter)` / `overlapAll(shape)` / `overlapAllHits(shape, filter)` / `shapeCastAll` / `shapeCastClosest` | §8.8。filter 付き overload は stable identity 付きヒットを返す |
| カメラ | `setCamera(name)` | §8.9 |
| 乱数 | `random()` / `randomInt(min,max)` / `randomFloat(min,max)` / `setSeed(u64)` / `seed()` | §8.10 |
| 音 | `playSound(path)`→SoundHandle / `stopSound(h)` / `setBusVolume(bus,v)` / `busVolume(bus)` / `isPlaying(h)` | §8.11 |
| シーン | `loadScene(name)` / `currentScene()` | §8.7 |
| 永続化 | `gameSettings()` / `setGameSettings(json)` / `saveSettings()` / `saveData(slot, json)` / `loadData(slot)`→optional / `listSaves()` | §8.12(✅WP65) |
| ライト | `setDirectionalLightDirection(name, dir)` / `setDirectionalLightIntensity(name, f)` / `setPointLightPosition(name, pos)` / `setSpotLightDirection(name, dir)` — いずれも→bool | ✅WP142。[第4章](04_scene_ecs.md) §4.2 の light をオブジェクト `name` で指す。未知名は例外ではなく **false** が返る |
| HUD | `debugText(x, y, text, color, scale)` | colorはRGBA、scaleは整数倍率。省略時は白・等倍。debug_text feature 未参照時は no-op([第7章](07_input_ui.md)) |
| イベント | `emit(const Event&)` | §8.6 |

アニメーション操作の API は GameContext に**ありません** — クリップは宣言的コンポーネント([第4章](04_scene_ecs.md))、グラフは独立した公開評価器(§8.13)です。評価器を `GameContext` に載せると標準の評価器だけが特権を持ってしまい、「評価器ごと自作できる」という設計が空文になるためです([../design_animation_graph.md](../design_animation_graph.md))。

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

> **シーンロードの最中だけ、この表の意味が変わります(✅WP380b〜e)。** `onInit` から呼んだ `emit` / `setSeed` / `loadScene` / `playSound` / `stopSound` / `setBusVolume` / `setGameSettings` / `saveData` / `saveSettings` は**その場では起きず**、シーンロードのトランザクションに溜まります。ロードが失敗すれば全部消え、成功したときだけ実行されます。読み取り(`isPlaying` / `loadData` / `listSaves` / `gameSettings`)は自分が溜めた書き込みを見ます。詳細は [第4章 §4.3](04_scene_ecs.md)。

## 8.5 オブジェクト behavior — もう一つのゲームコードの書き方(✅WP155/162/167)

ゲームシステム(§8.3)が「プロセスに 1 個・シーン全体を見る処理」なのに対し、**behavior は「オブジェクトに貼りつける振る舞い」**です。シーン JSON にパラメータごと書けて、エディタから付け外しもできます。

StudioではInspectorの **Add component...** から **Behavior: 登録名** を選び、宣言JSONの`params`を設定して追加できます。
同じ型を複数装着でき、追加・削除・Undo/Redo・保存は通常のシーン編集を使います。詳細は[第13章](13_editor.md)を参照してください。

> **設計決定(engine-owned arena):** behavior は ECS コンポーネントではなく、light / collider と同じ「特別扱いのコンポーネント」としてエンジン所有の attachment arena に入る(arena = 同種のものをまとめて置くエンジン側の置き場。ここでは attachment を連番で並べる [BehaviorAttachmentArena](../../src/core/gamelogic/behaviorarena.hpp#L174))。理由は、ゲーム DLL の型をエンジンの ECS チャンクに載せずに、ホットリロード時の世代交代(§8.5.6)とエディタからの付け外しを安全に行うため。

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
- `GameObjects::removeAll()` を behavior のコールバック中に呼ぶと `std::logic_error` になります。

### 8.5.5 実行順とライフサイクル(決定性)

- behavior 全体は **`"BehaviorSystem"`(order = 50)という 1 個のゲームシステム**として、他のゲームシステムと同じ `(order, 型名)` の全順序に並びます。つまり `order < 50` のシステムは全 behavior より前、`order > 50` は後に走ります(ビルトインのカメラコントローラは 10000)。
- attachment どうしの順序は `attachment_seq` の昇順。シーンロード時の seq は**宣言順**(オブジェクト順 → コンポーネント順)から決まります。
- **生成**: シーンの entity/collider/light を publish した後、`attachment_seq` 昇順に `onInit`。途中で例外が出たら、それまでに init 済みのものを逆順に `onDestroy` して全部巻き戻し、例外を投げ直します(このとき失敗した本人の `onDestroy` は呼ばれません)。巻き戻るのは behavior だけではなく、scene load 全体が旧シーンに戻ります(entity・collider・light・カメラ・描画・`onInit` 中の `emit` / `loadScene()` も。✅WP380a、[第4章 §4.3](04_scene_ecs.md))。`SceneLoaded` の emit は commit の後です。
- **破棄**: `attachment_seq` の**逆順**に `onDestroy`([destroyDetached](../../src/core/gamelogic/behaviorarena.cpp#L337))。`ctx.self()` から自分の transform / sprite を読めます — シーン切替の retire 段では entity は既に unpublish 済みなので、読めるのは**破棄トークンが持つ最終スナップショット**です。破棄トークンとは、unpublish のときにコンポーネントの値をそのまま退避しておく控えのことで、値が実際に壊れるのは retire の完了時です([finish](../../src/core/ecs/archetypemigration.cpp#L917))。
- 更新・イベント配送はどちらも開始時に有効な attachment のスナップショットを取ってから走るため、途中で消えたオブジェクトは安全にスキップされます。

#### `onInit` / `onDestroy` から呼べること(✅WP380b〜e)

シーンロードの最中は、`GameContext` の作用がトランザクションに溜まります(§8.4 の囲み・[第4章 §4.3](04_scene_ecs.md))。behavior から見た要点:

- `onInit` で `playSound` すると**音はまだ鳴りません**。ロードが commit したあとの flush で鳴ります。直後に `ctx.isPlaying(handle)` を呼べば true が返り、`stopSound` すればその予約ごと消えます(鳴らずに終わる)。
- `onInit` で `saveData` した内容は、同じ `onInit` の中の `loadData` で読めます。`listSaves()` にも並びますが、**まだ書いていないので `timestamp` は 0** です。
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
- `schema_changed_without_version_bump` の判定は **schema の指紋**(schema の宣言内容を宣言順のまま 1 本の JSON 文字列に書き出したもの — [schemaNodeFingerprint](../../src/core/userpublic/details/schema/structfieldjson.hpp#L1012))で、この文字列が 1 文字でも変われば別の schema と見なします。フィールドの型だけでなく**既定値・宣言順・enum 値の割り当て・presence(`defaulted` ↔ `required` ↔ `optionalOverride`)の変更も検出します**([validateBehaviorReloadSources](../../src/core/gamelogic/behaviorreloadvalidation.cpp#L85))。「型は同じだから大丈夫」は通りません。
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
- **`EngineTime` は継続**します(時刻・フレーム番号はリセットされない)。時計はシーンではなくプロセスに属するモジュールで([enginetime.hpp](../../src/core/appflow/enginetime.hpp#L10))、シーンロードはこれに触らないためです。シーンローカル時間が欲しければ `SceneLoaded` 受信時の時刻を覚えてください。
- 非同期ロード(`loadSceneAsync`)は 📐S2 未実装です。

### 落とし穴: システム状態はシーン切替で消えない

システムのインスタンスは static シングルトンなので、メンバに保持した `GameObjectId` は切替後「死んだ ID」になります(`setLocalTransform` / `removeObject` は false を返し、`localTransform` は例外になり得ます)。**`SceneLoaded` の `onEvent` で状態をリセットする**のが正道です。「シーンをまたいで残るオブジェクト」(persistent)の仕組みは v1 にはありません。

## 8.8 物理クエリとトリガー(✅P1/P2/WP107/179)

> **設計決定:** ランタイム物理シミュレーションは「やらない」ではなく「**後**」(2026-07-07 決定)。現在あるのは raycast / overlap / shapeCast の **query world**(collider の配置だけを持ち、問い合わせに答える世界。速度や力を積分して物を動かす step は持たない — [physworld.hpp](../../src/core/phys/physworld.hpp#L42) の公開面はバインドとクエリだけ)。Jolt は query provider として実装済みですが、剛体 step/constraint を持つシミュレーションは未実装です。映像用の破壊・布は Houdini ベイク(transform_seq / VAT)レーンが担当([第5章](05_assets.md))。

- collider はシーン JSON のコンポーネントとして定義します([第4章](04_scene_ecs.md) §4.2)。**collider を持つオブジェクトには `name` が必須**です。
- ゲームコードから:

```cpp
// レイキャスト(最近傍 1 件)
Pelican::phys::Ray ray{ {0,0,0}, {0,0,1}, 100.0f };   // origin, direction, max_distance
if (auto hit = ctx.raycastClosest(ray)) {
    // hit->id(オブジェクト名), hit->distance, hit->position, hit->normal
}

// 形状オーバーラップ(該当オブジェクト名の一覧、名前昇順)
auto names = ctx.overlapAll(Pelican::phys::Shape{ Pelican::phys::Sphere{ {0,1,0}, 0.5f } });

// capsule を delta だけ平行移動し、最初の衝突を取得
Pelican::phys::Shape player = Pelican::phys::Capsule{
    {0, 2, 0}, {}, 0.5f, 0.25f
};
if (auto hit = ctx.shapeCastClosest(player, {0, -4, 0})) {
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
- 📐未実装: rpc の query メソッド、メッシュコライダ/BVH、剛体シミュレーション。

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

- **カット切替**: `ctx.setCamera("SceneFlowCamera")` または rpc `set_camera`。名前付きカメラだけが宛先になれます。切替後はフリーカメラ操作がロックされ、そのカメラの定義が即時適用されます。
- **コントローラ**はビルトインのゲームシステム(order 10000)が毎フレーム駆動します。orbit / fly は Actions の `move` / `look`(いずれも axis2)を消費しますが、アクションが未定義でも「動かないだけ」でエラーにはなりません。
- damping はブレンド重み `1 - exp(-damping * dt)` で、0 なら即時追従。パラメータ変更やフレーム巻き戻し(rpc `set_time`)を検出すると内部状態を自動リセットします(決定性のため)。
- 📐未実装(C3 以降): glb カメラノードの取り込み/書き出し round-trip、transform_seq のカメラトラック、ブレンド・シェイク。

## 8.10 決定性 — なぜ毎回同じ結果になるのか

> **設計決定:** エンジン内部の判定に wall clock・`std::rand`・`random_device` を使わない。乱数は **PCG32**(64 ビットの内部状態を線形合同で進め、ビットを混ぜて 32 ビットを取り出す生成器 — [nextUInt](../../src/core/userpublic/deterministicrng.cpp#L25))**の自前実装**(distribution も自前)で、プラットフォーム・コンパイラに依存しない列を生成する(`std::mt19937` + `std::uniform_*` は処理系差があるため不採用)。シードの既定は **0(固定)** — 「毎回違う」が欲しいゲームは自分で `ctx.setSeed(エントロピー)` を呼ぶ。

決定性を構成する要素(✅すべて実装済み):

1. フレームループの固定順序(§8.3)と rpc `step_frame` の同一順序
2. 入力はフレーム頭のスナップショットで凍結(WP37)—— フレーム開始時に、前フレーム以降に OS から届いた入力を 1 回だけ切り出し([beginFrame](../../src/core/os/inputstate.cpp#L373))、そのフレーム中は誰が何度読んでも同じ値です(途中で届いた入力は次フレームの分として待ちます)
3. システム実行順 = order → 名前の辞書順
4. イベント配送 = 次フレーム頭・emit 順
5. 物理のタイブレーク(同じ距離・同じ TOI に並んだヒットの順序決め)= stable `ColliderId` → entity 世代 → shape ordinal(名前は最終手段。※レガシー API `raycastClosest(ray)` 単引数と `overlapAll` 名前列のみ旧来の名前辞書順)
6. 乱数 = PCG32 + 固定シード(`basic_config.seed` / `ctx.setSeed` / rpc `set_seed`)
7. シーン切替 = フレーム境界適用
8. ヘッドレス実行 = 固定ステップ時刻(dt = 1/fps)

リプレイの三点セット「固定ステップ+入力記録+シード」は ✅**完成**しています(入力記録 = `pelican.input_seq`、WP89 — [第7章](07_input_ui.md) §7.3)。同一ビルド + プロジェクト + シードで PNG byte 一致まで CI 検証されています。rpc シナリオ(`inject_input` + `step_frame`)も従来どおり使えます。

ウィンドウモードの時刻は実測 dt(上限 0.1 秒でクランプ)なので、厳密なリプレイ互換が必要な検証はヘッドレスで行ってください。

## 8.11 オーディオ(✅A1 = WP51)

```cpp
auto h = ctx.playSound("assets/audio/shot.wav");  // WAV のみ。戻り値はハンドル
ctx.setBusVolume("se", 0.5f);                     // バスは master / bgm / se の 3 固定
bool playing = ctx.isPlaying(h);
ctx.stopSound(h);
```

- 対応フォーマットは **WAV のみ**(厳格なパーサ。規格逸脱ファイルは名指しで拒否)。再生バックエンドは miniaudio です。
- ヘッドレス / rpc 実行では自動的に **null バックエンド**(無音・API は全部動く)になります。テストの決定性のためです。
- `PELICAN_WITH_AUDIO=OFF` ビルドで音 API を呼ぶと、どのフラグで無効化されたかを言うエラーになります。
- ⚠️ 現状の制限: `playMusic`(ループ BGM)は未実装で、**すべての再生は se バス固定**です。`setBusVolume("bgm")` は受理されますが、bgm バスに音を載せる手段がまだありません。Ogg/ストリーミング/3D 音響も 📐未実装です。
- **シーンロード中(behavior の `onInit` / `onDestroy`)に呼ぶと、再生・停止・バス音量はロードの成功後にまとめて適用されます**(§8.4 の囲み・[第4章 §4.3](04_scene_ecs.md))。`isPlaying` / `busVolume` はその予約を反映した値を返します。

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
ctx.saveData("slot-a", nlohmann::json{{"level", 3}});   // user://saves/slot-a.json
auto loaded = ctx.loadData("slot-a");                    // optional<Json>(欠落/破損は nullopt)
auto slots  = ctx.listSaves();                           // {slot, timestamp} の昇順一覧
```

- スロット名はポータブルなファイル名検証付き(`.`/`..`・末尾ドット/空白・`/ \ # : " < > | ? *`・制御文字は例外)。中身はエンベロープなしの任意 JSON です。
- **例外挙動の非対称に注意**: 書き込み系(`saveSettings` / `saveData`)は失敗で例外、読み込み系(`loadData`)は throw せず `nullopt` を返します(起動フローを壊さないため)。
- 書き込みはすべて atomic(tmp + rename)。削除 API(`deleteSave`)や rpc 経由の永続化メソッドはまだありません。
- **シーンロード中に書くと、ディスクへの反映はロードの成功後**です(§8.4 の囲み)。同じロードの中では `loadData` が書いた値を返し、`listSaves()` にも並びますが、その `timestamp` は **0**(まだファイルが無い)です。`onDestroy` から書いた場合、失敗しても例外にはならず `effect_errors` に記録されます。

## 8.13 アニメーション再生(✅WP38/94/97/101/102/176〜178)

3 つのレベルがあります。

1. **宣言的クリップ再生(WP38)** — シーン JSON の `animation` コンポーネント([第4章](04_scene_ecs.md) §4.2)。ゲームコード不要。時刻源は EngineTime(決定的)。
2. **アニメーショングラフ(WP101/102)** — `pelican.anim_graph` v1 のデータアセット + **非特権の公開評価器**。エンジン特権なしで `#include <animation/animgraph.hpp>` だけで動きます。

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

グラフを使うオブジェクトには `animation` コンポーネントを**付けません**(評価器が pose sink を claim します)。`forceState` / `getStatus()`(現在ステート・遷移・semantic pose hash)も公開されています。v1 の語彙は clip + blend1d + crossfade + interrupt("never" / "higher_priority" / "always")のみで、layer・2D blend は v2 予約(キーを書くとエラー)。

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
VRMA を含め IK・root motion の object-transform 適用はありません。決定性は
「同一ビルドで byte 一致」です。

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
- [第4章 シーンと ECS](04_scene_ecs.md) / [第7章 入力と UI](07_input_ui.md) / [第10章 ツールリファレンス](10_tools.md) / [第13章 エディタ](13_editor.md)
