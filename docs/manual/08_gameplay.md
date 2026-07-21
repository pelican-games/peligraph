# 第8章 ゲームロジック

対象: pelican2(2026-07-21 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- ゲームコード(C++)の書き方 — システム登録・`GameContext`・オブジェクト操作・DLL ホットリロード
- **オブジェクト behavior**(`PELICAN_REGISTER_BEHAVIOR` — シーンに貼れる振る舞い。✅WP155/162/167)
- イベント層(emit と購読、フレーム境界配送)
- シーン遷移・カメラ(コントローラ含む)・物理クエリ(shapeCast / moveAndSlide)の使い方
- 決定性(固定ループ順序・決定的乱数・シード)— 「なぜ毎回同じ結果になるのか」
- オーディオ・永続化(設定/セーブ)・アニメーション・2D スプライトの操作

## 8.1 ゲームコードの位置づけ

> **設計決定(ネイティブ一本・2026-07-07 ユーザー決定):** ゲームロジックは C++ で書く。スクリプト言語は不採用。エンジンは「ソース同居の SDK」。

✅WP90(G2)で、ゲームコードのビルド形態は静的リンクから **`pelican_game_logic.dll`** に移行しました。公開面(userpublic)だけを import する DLL で、実行中のホットリロード(F5 / 自動検出 / rpc)に対応します。リロードは v1 では **full-reset 方式**(システム状態と ECS を破棄して現シーンを再構築)です。詳細は [第10章](10_tools.md) §10.5。

> **設計決定(レイヤ規則):** ゲームコードが include してよいのは `src/core/userpublic/` のヘッダのみ。エンジン内部のモジュール取得(`GET_MODULE`)をゲームコードから直接呼ぶことは禁止で、入力・時刻・オブジェクト・物理・音・シーン・乱数はすべて **`GameContext`** ファサードを通す。

ビルドの組み込み方([第2章](02_getting_started.md) §2.4):`code/CMakeLists.txt` に `pelican_game_sources(<ソース>...)` と書き、CMake configure 時に `-DPELICAN_PROJECT=<プロジェクトパス>` を渡します。

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
| 音 | `playSound(path)`→SoundHandle / `stopSound(h)` / `setBusVolume(bus,v)` / `isPlaying(h)` | §8.11 |
| シーン | `loadScene(name)` / `currentScene()` | §8.7 |
| 永続化 | `gameSettings()` / `setGameSettings(json)` / `saveSettings()` / `saveData(slot, json)` / `loadData(slot)`→optional / `listSaves()` | §8.12(✅WP65) |
| ライト | `setDirectionalLightDirection(name, dir)` / `setDirectionalLightIntensity(name, f)` / `setPointLightPosition(name, pos)` / `setSpotLightDirection(name, dir)` — いずれも→bool | ✅WP142。[第4章](04_scene_ecs.md) §4.2 の light をオブジェクト `name` で指す。未知名は例外ではなく **false** が返る |
| HUD | `debugText(x, y, text)` | debug_text feature 未参照時は no-op([第7章](07_input_ui.md)) |
| イベント | `emit(const Event&)` | §8.6 |

アニメーション操作の API は GameContext に**ありません** — クリップは宣言的コンポーネント([第4章](04_scene_ecs.md))、グラフは独立した公開評価器(§8.13)です。

### actionPose(✅WP132 — XR 姿勢入力)

```cpp
const auto head = ctx.actionPose("head");
if (head.valid && head.source == Pelican::ActionPoseSource::synthetic_head) {
    // XR 起動中のみ届く。flat 起動では valid = false の既定値が返る(throw しない)
}
```

- `ActionPose` は position/orientation に加え **valid/tracked の 4 フラグ**、`source`(`action_space` / `synthetic_head`)、`reference_space`(stage / local_floor / local)、`hand`(left/right)を持ちます。
- 公開名の規約: `aim_left` / `aim_right` / `grip_left` / `grip_right` はコントローラの action space、**`head` は `xrLocateViews` 由来の synthetic**(binding を書くと名指しエラー)。
- 実例は [projects/vrm_xr_demo](../../projects/vrm_xr_demo) の視線追従(head pose を優先し、flat では authored camera 基準にフォールバック)。

未知の名前(アクション・シーン・カメラ・オブジェクト・イベント)は**すべて名前入りの例外**になります。「静かに無視」はこのエンジンには存在しません(fail-fast の一貫した文化)。

## 8.5 オブジェクト behavior — もう一つのゲームコードの書き方(✅WP155/162/167)

ゲームシステム(§8.3)が「プロセスに 1 個・シーン全体を見る処理」なのに対し、**behavior は「オブジェクトに貼りつける振る舞い」**です。シーン JSON にパラメータごと書けて、エディタから付け外しもできます。

> **設計決定(engine-owned arena):** behavior は ECS コンポーネントではなく、light / collider と同じ「特別扱いのコンポーネント」としてエンジン所有の attachment arena に入る。理由は、ゲーム DLL の型をエンジンの ECS チャンクに載せずに、ホットリロード時の世代交代(§8.5.6)とエディタからの付け外しを安全に行うため。

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
| `Params` | `Type::Params` と `Params::schema` が必須。default 構築可能かつ **nothrow swappable**(値の差し替えを atomic に行うため) |
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
- 使える型: 整数各種 / `F32` / `F64` / `Vec2〜4` / `Quat` / `String` / **`Bool`** / **`Enum`**(`enumValues(...)` 必須・最大 8 個)。ネストした構造体・配列・任意 JSON は使えません。
- 範囲は `irange` / `urange` / `frange`、単位文字列は `field<&P::x>("x", range, "m/s")` の第 3 引数。これらは**インスペクタのウィジェットにそのまま反映**されます([第13章](13_editor.md))。
- 未知キー・型不一致・範囲外はすべて `field 'params.speed' is out of range` の形式で**フィールドパス付きの名指しエラー**になります。
- 適用は「既定値で構築 → 存在するキーだけ適用 → 全検証成功後に swap」。途中で失敗しても既存の値は一切変わりません。
- `ctx.params<Params>()` は非 const 参照を返すので実行中に書き換えられますが、**その変更は永続化されません**(リロードやシーン再構築で authoring 値に戻ります)。

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
- **生成**: シーンの entity/collider/light を publish した後、`attachment_seq` 昇順に `onInit`。途中で例外が出たら、それまでに init 済みのものを逆順に `onDestroy` して全部巻き戻し、例外を投げ直します(このとき失敗した本人の `onDestroy` は呼ばれません)。`SceneLoaded` の emit はその後です。
- **破棄**: `attachment_seq` の**逆順**に `onDestroy`。**entity がまだ生きているうち**に呼ばれるので、`ctx.self()` から transform や物理を触れます。
- 更新・イベント配送はどちらも開始時に有効な attachment のスナップショットを取ってから走るため、途中で消えたオブジェクトは安全にスキップされます。

### 8.5.6 DLL ホットリロードとの関係(✅WP162)

ゲームロジック DLL を差し替える([第10章](10_tools.md) §10.5)と、behavior は次の規律で世代交代します:

| 新 DLL の状態 | 結果(エラーはリロード拒否 = 旧 DLL 継続) |
|---|---|
| 旧 DLL にあった登録名が消えた | `behavior_type_removed: stable_name='X' version=1` |
| schema を変えたのにバージョン据え置き | `schema_changed_without_version_bump: ...` |
| バージョンを上げたが既存 params が新 schema で読めない | `schema_incompatible: ... scene='...' object_index=0 field='params.count': ...` |

- バージョンを上げたときは、**現在ロードしていないシーンも含めて**全 behavior の params が新 schema で読めることが条件です。
- リロード後、behavior のインスタンス状態(メンバ変数)は失われ、params は authoring 値から再構築されます(full-reset 方式)。

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
// 1. イベント型の定義と登録(POD struct)
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
- ペイロードは値コピーで保持されます(1 フレーム跨ぐため、参照・ポインタは入れない)。
- **同一翻訳単位内で `PELICAN_REGISTER_EVENT` → 型定義 → `PELICAN_REGISTER_SYSTEM` の順**に書いてください。システム登録マクロは、その時点で見えているイベントカタログだけを購読対象にします。
- エンジンが発行するイベントは `SceneLoaded { scene_name }` と、
  物理トリガーの `OverlapEnter { self, other }` /
  `OverlapExit { self, other }` です。`self` / `other` は generation を含む
  full `EntityId` で、同じ pair を両オブジェクト視点へ配送します。
- rpc の `inject_event` で外部からもイベントを注入できます(`ref()` を持つ型は JSON からペイロード構築)。
- behavior も同じ `onEvent` でイベントを受けられます(§8.5)。書く順序の制約(`PELICAN_REGISTER_EVENT` → 型定義 → 登録マクロ)も同じです。

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
- **`EngineTime` は継続**します(時刻・フレーム番号はリセットされない)。シーンローカル時間が欲しければ `SceneLoaded` 受信時の時刻を覚えてください。
- 非同期ロード(`loadSceneAsync`)は 📐S2 未実装です。

### 落とし穴: システム状態はシーン切替で消えない

システムのインスタンスは static シングルトンなので、メンバに保持した `GameObjectId` は切替後「死んだ ID」になります(`setLocalTransform` / `removeObject` は false を返し、`localTransform` は例外になり得ます)。**`SceneLoaded` の `onEvent` で状態をリセットする**のが正道です。「シーンをまたいで残るオブジェクト」(persistent)の仕組みは v1 にはありません。

## 8.8 物理クエリとトリガー(✅P1/P2/WP107/179)

> **設計決定:** ランタイム物理シミュレーションは「やらない」ではなく「**後**」(2026-07-07 決定)。現在は raycast / overlap / shapeCast の query world。Jolt は query provider として実装済みですが、剛体 step/constraint を持つシミュレーションは未実装です。映像用の破壊・布は Houdini ベイク(transform_seq / VAT)レーンが担当([第5章](05_assets.md))。

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
    // hit->time_of_impact は 0..1。normal は player を押し出す向き。
    // initial_overlap 時は normal * penetration_depth が MTD。
}
```

- collider metadata は scene component に `layer`(既定 1)、`mask`(既定 `0xffffffff`)、`trigger`、`one_way`(既定 false)で指定します。詳細 query overload の `QueryFilter` で reciprocal layer/mask、self/ignore、trigger/one-way inclusion を制御できます。
- **決定性の規約**: shapeCast の TOI tie は ε=`1e-5` bucket 後に stable `ColliderId`、full entity generation、shape ordinal の順です。MTD tie は移動の逆向きを優先し、その後 world x/y/z で固定します。closest は同じ ordered all-hit の先頭です。方向付き one-way policy は `shapeCastAll` を順に評価するため、無視した後の「次の床」を再 query せず続行できます。
- 初期 penetration が ε より深ければ TOI 0 + MTD。接触だけなら接近中に限り TOI 0、静止/離反では hit になりません。zero delta は depenetration query です。
- `one_way` は metadata です。前位置や接近方向から「通す/乗る」を決める policy は、WP109 の標準ユーザー空間ライブラリ `platformer::moveAndSlide` が担当します。
- `trigger=true` は物理応答を行わない検知専用 collider です。query では
  既定で通常の hit として見え、`QueryFilter::include_triggers=false` で
  除外できます。overlap 開始時に `OverlapEnter`、終了時に
  `OverlapExit` を各 pair の両視点へ一度だけ emit し、stay は送りません。
  entity destroy / collider remove は次の trigger update で Exit、scene
  全 reset は旧 EntityId が無効になる domain boundary として Exit なしです。
- Provider ABI V2 により Builtin/Jolt/game DLL provider を capability 単位で差し替えます。Jolt header は engine/game の公開型へ出ません。physics-off と provider-only build では不要な backend をリンクしません。
- **プロバイダの選択はビルド時**です: `PELICAN_WITH_BUILTIN_PHYSICS`(既定 ON)/ `PELICAN_WITH_JOLT_PHYSICS`(既定 OFF。両方 ON なら Jolt が勝つ)。project.json や実行時の切替はありません。クエリ契約(順序・タイブレーク)はプロバイダによらずエンジン側が所有するため、通常は意識不要です。ゲーム DLL から capability 単位で provider をオーバーレイする上級拡張点(`physics/abi_v1.hpp` / `abi_v2.hpp` — raycast だけ独自実装し残りはフォールバック、等)もあります。
- transform 追従は現行では毎フレーム再収集(BVH 等の加速構造なし — 計測してから見直す方針)。capsule の軸はローカル Y、box は OBB(回転対応)です。
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
不使用が可能です。step-up、coyote time、moving platform はゲーム固有 policy として
この最小 helper の外に残しています。実例は `projects/sprite_demo` を参照してください。

## 8.9 カメラ(✅C1/C2 = WP48/50)

カメラは「**定義**(glTF 1:1 の投影)/ **コントローラ**(orbit・follow・fly)/ **演出**(カット切替など)」の三層設計です。定義とコントローラの書き方(シーン JSON)は [第4章](04_scene_ecs.md) §4.2 を参照してください。

- **カット切替**: `ctx.setCamera("SceneFlowCamera")` または rpc `set_camera`。名前付きカメラだけが宛先になれます。切替後はフリーカメラ操作がロックされ、そのカメラの定義が即時適用されます。
- **コントローラ**はビルトインのゲームシステム(order 10000)が毎フレーム駆動します。orbit / fly は Actions の `move` / `look`(いずれも axis2)を消費しますが、アクションが未定義でも「動かないだけ」でエラーにはなりません。
- damping はブレンド重み `1 - exp(-damping * dt)` で、0 なら即時追従。パラメータ変更やフレーム巻き戻し(rpc `set_time`)を検出すると内部状態を自動リセットします(決定性のため)。
- 📐未実装(C3 以降): glb カメラノードの取り込み/書き出し round-trip、transform_seq のカメラトラック、ブレンド・シェイク。

## 8.10 決定性 — なぜ毎回同じ結果になるのか

> **設計決定:** エンジン内部の判定に wall clock・`std::rand`・`random_device` を使わない。乱数は **PCG32 の自前実装**(distribution も自前)で、プラットフォーム・コンパイラに依存しない列を生成する(`std::mt19937` + `std::uniform_*` は処理系差があるため不採用)。シードの既定は **0(固定)** — 「毎回違う」が欲しいゲームは自分で `ctx.setSeed(エントロピー)` を呼ぶ。

決定性を構成する要素(✅すべて実装済み):

1. フレームループの固定順序(§8.3)と rpc `step_frame` の同一順序
2. 入力はフレーム頭のスナップショットで凍結(WP37)
3. システム実行順 = order → 名前の辞書順
4. イベント配送 = 次フレーム頭・emit 順
5. 物理のタイブレーク = stable `ColliderId` → entity 世代 → shape ordinal(名前は最終手段。※レガシー API `raycastClosest(ray)` 単引数と `overlapAll` 名前列のみ旧来の名前辞書順)
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
