# 第8章 ゲームロジック

対象: pelican2(2026-07-10 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- ゲームコード(C++)の書き方 — システム登録・`GameContext`・オブジェクト操作
- イベント層(emit と購読、フレーム境界配送)
- シーン遷移・カメラ(コントローラ含む)・物理クエリの使い方
- 決定性(固定ループ順序・決定的乱数・シード)— 「なぜ毎回同じ結果になるのか」
- オーディオと永続化の現状

## 8.1 ゲームコードの位置づけ

> **設計決定(ネイティブ一本・2026-07-07 ユーザー決定):** ゲームロジックは C++ で書き、エンジンごとビルドして 1 実行体に静的リンクする。スクリプト言語は不採用。エンジンは「ソース同居の SDK」であり、DLL 境界を作らない(ABI 問題の回避)。DLL ホットリロード(G2)は将来課題 📐。

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
        Pelican::SimpleModelViewUpdateComponent model_update;
        model_update.model_name = "character";
        model_update.dirty = true;

        const auto id = Pelican::GameObjects::add()
                            .addComponent<Pelican::TransformComponent>()
                            .addComponent<Pelican::LocalTransformComponent>(transform)
                            .addComponent<Pelican::SimpleModelViewComponent>()
                            .addComponent<Pelican::SimpleModelViewUpdateComponent>(model_update)
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
- インスタンスは関数ローカル static のシングルトンで、**シーン切替をまたいで生存**します(§8.6 の落とし穴参照)。
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
| 入力 | `actionsConfigured()` / `actionPressed/Released/Held(name)` / `actionAxis1(name)` / `actionAxis2(name)` | 未知アクション名は名前入り例外。`actionPose` は OpenXR 未実装のため常に throw 📐 |
| 時刻 | `time()` / `deltaTime()` / `frameIndex()` | |
| ログ | `logInfo/Warning/Error(msg)` | rpc モードでは stdout が使えないため、ログは必ずこれで |
| オブジェクト | `createObject(LocalTransformComponent)` / `removeObject(id)`→bool / `localTransform(id)` / `setLocalTransform(id, t)`→bool | `setLocalTransform` は描画側へも即時伝播 |
| 物理 | `raycastClosest` / `overlapAll` / `shapeCastAll` / `shapeCastClosest` | §8.7。詳細 overload は stable identity/filter を返す |
| カメラ | `setCamera(name)` | §8.8 |
| 乱数 | `random()` / `randomInt(min,max)` / `randomFloat(min,max)` / `setSeed(u64)` / `seed()` | §8.9 |
| 音 | `playSound(path)`→SoundHandle / `stopSound(h)` / `setBusVolume(bus,v)` / `isPlaying(h)` | §8.10 |
| シーン | `loadScene(name)` / `currentScene()` | §8.6 |
| HUD | `debugText(x, y, text)` | debug_text feature 未参照時は no-op([第7章](07_input_ui.md)) |
| イベント | `emit(const Event&)` | §8.5 |

未知の名前(アクション・シーン・カメラ・オブジェクト・イベント)は**すべて名前入りの例外**になります。「静かに無視」はこのエンジンには存在しません(fail-fast の一貫した文化)。

## 8.5 イベント層(✅WP56)

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
- エンジンが発行するイベントは現在 **`SceneLoaded { scene_name }` のみ**です(起動時の初回ロードでも emit されます)。設計文書にある `OverlapEnter` / `OverlapExit`(物理トリガー)は 📐E2 未実装です。
- rpc の `inject_event` で外部からもイベントを注入できます(`ref()` を持つ型は JSON からペイロード構築)。

## 8.6 シーン遷移(✅WP52)

```cpp
ctx.loadScene("scene_flow_second");   // 予約(名前はその場で検証。未知名は即例外)
```

- 適用は**フレーム末尾**(ゲームシステム実行後・描画前)。同フレームに複数回呼ぶと最後の 1 件が勝ちます。
- 切替では全 GameObject・物理・描画インスタンス・名前束縛が破棄され、**起動時と同じロード経路**で新シーンが構築されます(第 2 経路を作らない方針)。
- **`EngineTime` は継続**します(時刻・フレーム番号はリセットされない)。シーンローカル時間が欲しければ `SceneLoaded` 受信時の時刻を覚えてください。
- 非同期ロード(`loadSceneAsync`)は 📐S2 未実装です。

### 落とし穴: システム状態はシーン切替で消えない

システムのインスタンスは static シングルトンなので、メンバに保持した `GameObjectId` は切替後「死んだ ID」になります(`setLocalTransform` / `removeObject` は false を返し、`localTransform` は例外になり得ます)。**`SceneLoaded` の `onEvent` で状態をリセットする**のが正道です。「シーンをまたいで残るオブジェクト」(persistent)の仕組みは v1 にはありません。

## 8.7 物理クエリ(✅P1/P2/WP107)

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
- `one_way` は metadata です。前位置や接近方向から「通す/乗る」を決める policy と `moveAndSlide` は S2D-2 の標準ユーザー空間ライブラリが担当します。`trigger=true` も query filter には使えますが、enter/exit イベントは E2 未実装です。
- Provider ABI V2 により Builtin/Jolt/game DLL provider を capability 単位で差し替えます。Jolt header は engine/game の公開型へ出ません。physics-off と provider-only build では不要な backend をリンクしません。
- transform 追従は現行では毎フレーム再収集(BVH 等の加速構造なし — 計測してから見直す方針)。capsule の軸はローカル Y、box は OBB(回転対応)です。
- `debug_draw` feature を rendering config で参照していると collider のワイヤフレームが描画されます([第6章](06_rendering.md))。
- 📐未実装: rpc の query メソッド、メッシュコライダ/BVH、物理トリガーイベント、剛体シミュレーション。

## 8.8 カメラ(✅C1/C2 = WP48/50)

カメラは「**定義**(glTF 1:1 の投影)/ **コントローラ**(orbit・follow・fly)/ **演出**(カット切替など)」の三層設計です。定義とコントローラの書き方(シーン JSON)は [第4章](04_scene_ecs.md) §4.2 を参照してください。

- **カット切替**: `ctx.setCamera("SceneFlowCamera")` または rpc `set_camera`。名前付きカメラだけが宛先になれます。切替後はフリーカメラ操作がロックされ、そのカメラの定義が即時適用されます。
- **コントローラ**はビルトインのゲームシステム(order 10000)が毎フレーム駆動します。orbit / fly は Actions の `move` / `look`(いずれも axis2)を消費しますが、アクションが未定義でも「動かないだけ」でエラーにはなりません。
- damping はブレンド重み `1 - exp(-damping * dt)` で、0 なら即時追従。パラメータ変更やフレーム巻き戻し(rpc `set_time`)を検出すると内部状態を自動リセットします(決定性のため)。
- 📐未実装(C3 以降): glb カメラノードの取り込み/書き出し round-trip、transform_seq のカメラトラック、ブレンド・シェイク。

## 8.9 決定性 — なぜ毎回同じ結果になるのか

> **設計決定:** エンジン内部の判定に wall clock・`std::rand`・`random_device` を使わない。乱数は **PCG32 の自前実装**(distribution も自前)で、プラットフォーム・コンパイラに依存しない列を生成する(`std::mt19937` + `std::uniform_*` は処理系差があるため不採用)。シードの既定は **0(固定)** — 「毎回違う」が欲しいゲームは自分で `ctx.setSeed(エントロピー)` を呼ぶ。

決定性を構成する要素(✅すべて実装済み):

1. フレームループの固定順序(§8.3)と rpc `step_frame` の同一順序
2. 入力はフレーム頭のスナップショットで凍結(WP37)
3. システム実行順 = order → 名前の辞書順
4. イベント配送 = 次フレーム頭・emit 順
5. 物理のタイブレーク = 名前の辞書順
6. 乱数 = PCG32 + 固定シード(`basic_config.seed` / `ctx.setSeed` / rpc `set_seed`)
7. シーン切替 = フレーム境界適用
8. ヘッドレス実行 = 固定ステップ時刻(dt = 1/fps)

リプレイの三点セット「固定ステップ+入力記録+シード」のうち、**入力記録(`pelican.input_seq`、I3)だけが 📐未実装**です。現状の代替は rpc シナリオ(`inject_input` + `step_frame` の NDJSON スクリプト)で、CI では「同一スクリプト 2 回実行 → 応答列一致」を検証しています。

ウィンドウモードの時刻は実測 dt(上限 0.1 秒でクランプ)なので、厳密なリプレイ互換が必要な検証はヘッドレスで行ってください。

## 8.10 オーディオ(✅A1 = WP51)

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

## 8.11 永続化(user://)の現状

セーブデータ・設定の保存先として `user://` スキーム([第3章](03_project_format.md) §3.3)が用意されていますが、**現状はパス解決までが実装済み**で、設定(`pelican.settings`)・セーブ(`saveData` / `loadData` / `listSaves`)・atomic 書き込みの API は 🚧WP65(登録済み・未実装)です。それまでゲーム側で永続化 API を待つ必要があります。プロジェクトディレクトリへの書き込みは設計上禁止です(読み取り専有原則)。

## 関連文書

- [../design_game_logic_native.md](../design_game_logic_native.md) — ネイティブゲームロジックの設計
- [../design_event_layer.md](../design_event_layer.md) — イベント層(E2 は未実装)
- [../design_scene_flow.md](../design_scene_flow.md) — シーン遷移(S2 は未実装)
- [../design_physics_queries.md](../design_physics_queries.md) — 物理クエリと将来のシミュレーション方針
- [../design_camera_system.md](../design_camera_system.md) — カメラ三層
- [../design_determinism_services.md](../design_determinism_services.md) — 決定的乱数とシード規約
- [../design_audio.md](../design_audio.md) — オーディオ
- [../design_persistence.md](../design_persistence.md) — user:// と設定/セーブ三区分
- [第4章 シーンと ECS](04_scene_ecs.md) / [第7章 入力と UI](07_input_ui.md) / [第10章 ツールリファレンス](10_tools.md)
