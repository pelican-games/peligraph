# 第4章 シーンと ECS

対象: pelican2(2026-07-16 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- シーン形式 `pelican.scene` v1 の正確なスキーマと実物例
- v1 で書けるコンポーネント全種類(必須/任意フィールド・既定値・単位)
- シーンがロードされてから ECS・ライト・物理に振り分けられるまでの流れ
- ランタイム ECS(世代付き ID・生成トランザクション)の、ゲーム開発者が知るべき要点
- シーン遷移(`loadScene`)の仕組み

正本の設計文書は [../design_scene_format.md](../design_scene_format.md)(v1.1)ですが、コンポーネントの受理仕様の多くはコードにのみ存在するため、この章の表はパーサ実装(`src/project/sceneformat.cpp`、`src/core/loader/scene.cpp` ほか)から採録しています。

## 4.1 シーンファイルの全体構造

シーンは JSON で書きます。**1 ファイルに複数シーンを持つのが標準形**です(2026-07-02 レビューで確定)。実物([../../projects/example/scenes/main.scene.json](../../projects/example/scenes/main.scene.json)、31 オブジェクト+13 ライトの抜粋):

```json
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "components": [
            { "name": "transform", "pos": [0, 0, -3], "rotation": [0, 0.1736, 0, 0.348], "scale": [1, 1, 1] },
            { "name": "camera" }
          ]
        },
        {
          "components": [
            { "name": "transform", "pos": [0, -1.3, 3], "rotation": [0, 0.707, -0.707, 0], "scale": [0.25, 0.25, 0.25] },
            { "name": "simplemodelview", "model": "sotai" }
          ]
        },
        {
          "name": "BlueSpot",
          "components": [
            { "name": "light", "type": "spot",
              "position": [2, 0.5, 2], "direction": [0, -0.5, -1],
              "intensity": 1.8, "color": [0.6, 0.7, 1],
              "innerConeAngle": 18, "outerConeAngle": 28 }
          ]
        }
      ]
    },
    "scene_flow_second": {
      "objects": [
        {
          "name": "SceneFlowCamera",
          "components": [
            { "name": "transform", "pos": [0, 1, -4], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
            { "name": "camera", "type": "perspective", "yfov": 0.78539816339, "znear": 0.1, "zfar": 1000 }
          ]
        }
      ]
    }
  }
}
```

### エンベロープの規則

| フィールド | 規則 |
|---|---|
| `schema` | **必須**。`"pelican.scene"` 以外は hard error |
| `version` | **必須**。整数で **`1` ちょうど**(0・負・2 以上は拒否) |
| `scenes` | **必須**。`{ "<scene_id>": { "objects": [...] }, ... }` |

- schema なしのレガシー形式、トップレベル `lights` セクションは **v1 では受理されません**(名指しのエラーで移行先を案内)。※設計文書には「WARN+自動解釈」と書かれていますが、WP63(strict v1 化)で意図的に廃止されており、実装が正です。
- 起動時にロードされるのは `project.json` の `basic_config.default_scene_id` のシーンです。

### オブジェクトと名前・親子

- 各オブジェクトは `components` 配列が**必須**です。
- `name` は**任意**。付ける場合は `[a-zA-Z0-9_]` のみ(識別子規約 R7)で、**シーン内一意**(重複は hard error)。
- 名前の用途: rpc `update_transforms` の宛先、カメラコントローラの `target`、**`parent` 参照**、ライト名、collider 名、エラーメッセージ。
- `parent`(✅WP79): オブジェクト直下の任意キーで、**親オブジェクトの `name`** を文字列で指定します(省略 = ルート)。未知の親・同名複数(曖昧)・循環(経路列挙付き)はすべて hard error。親子に参加するオブジェクトは `transform` 必須で、`localtransform` を書かなければローダーが transform から自動注入します。ワールド変換は `parent_world × local` で毎フレーム合成されます(scale は成分積)。

```json
{ "name": "Wheel", "parent": "CarBody",
  "components": [
    { "name": "transform", "pos": [0.8, -0.3, 0], "rotation": [0,0,0,1], "scale": [1,1,1] },
    { "name": "simplemodelview", "model": "wheel" }
  ] }
```

(例 — 実物のネガティブ検証は `test/fixtures/project_format/invalid/scene_{unknown_parent,ambiguous_parent,parent_cycle}.json`)

> **設計決定(コンポーネント名 = 登録名):** シーン形式が定義するのは「`name` + 任意パラメータのレコード列」まで。コンポーネントの語彙は ECS の登録名(+バインダの特別扱い)で決まり、新コンポーネント追加にシーン形式仕様の改訂は不要。未知の名前は `Unknown component 'X' on object 'Y'` で fail-fast。

> **設計決定(ファイルパス禁止):** コンポーネントのパラメータにファイルパスを直接書くことは禁止。モデルは必ず `asset_data.json` の登録名(`model: "sotai"`)経由で参照する。パス解決規則の適用面を asset_data.json に限定し、シーン検証をエンジン・web・外部ツールで安価にするため。

## 4.2 コンポーネント一覧(v1 で書けるものすべて)

`ref()` が読むフィールドは**すべて必須**(欠落は例外)です。既定値があるものは明記します。

### transform(✅)

| フィールド | 型 | 意味 |
|---|---|---|
| `pos` | [x,y,z] | 位置 |
| `rotation` | [x,y,z,w] | クォータニオン(**x,y,z,w 順**) |
| `scale` | [x,y,z] | スケール |

### simplemodelview(✅)— モデル表示

```json
{ "name": "simplemodelview", "model": "sotai" }
```

`model` は `asset_data.json` の `models[].name` を指します(`foo.glb#mesh/名前` のフラグメント登録も可 — [第5章](05_assets.md))。✅WP67 で**単一コンポーネントに統合**されました: `SimpleModelViewComponent` が `model_name` + `dirty` + インスタンスハンドルを 1 つで持ちます(旧 `simplemodelviewupdate` は消滅。JSON の書き味は不変)。実行時に `model_name` を書き換えれば(`init` が dirty を立てるため)モデル差し替えもできます。

### localtransform(✅)

`pos` / `rotation` / `scale`(すべて必須)。毎フレーム値が `transform` へ反映されます。親子階層はオブジェクト直下の `parent` キー(§4.1)で宣言します — localtransform コンポーネント自体に親を書くフィールドはありません。

### camera(✅)

CameraComponent 自体はマーカーで、投影・コントローラのパラメータは Camera モジュールがシーン JSON から直接読みます。

| フィールド | 規則 |
|---|---|
| `type` | `"perspective"` / `"orthographic"`。省略時は xmag/ymag の有無で推定 |
| `yfov` | **ラジアン**。省略時は `basic_config.camera` へフォールバック |
| `znear` / `zfar` | 省略時フォールバック |
| `aspect` | 任意(省略時はビューポートから計算) |
| `xmag` / `ymag` | orthographic 用 |
| `controller` | カメラコントローラ([第8章](08_gameplay.md)) |
| `sprite` | `{"pixel_perfect":"off"\|"strict", "sort":"z"\|"declaration"}`(✅WP106。スプライトの pixel policy — [第6章](06_rendering.md) §6.9) |

- 旧キー `fov_y` / `near` / `far` は移行先を案内する hard error です(WP63)。
- パラメータを何も書かなければ `basic_config.camera` の既定投影を使います(example の 1 個目のカメラがこの形)。
- **シーン中で最初に現れた camera の投影がアクティブ**になります。位置・向きは同じオブジェクトの `transform` から取られます(rotation の +Z が視線方向)。
- `controller` の種類: `orbit`(`target`・`distance` 必須)/ `follow`(`target`・`offset` 必須)/ `fly`(`speed`・`sensitivity` 必須)。controller 付きカメラのオブジェクトは **name 必須**です。

### light(✅)— ECS を経由しない

> **設計決定:** light はコンポーネント表記だが、v1 では ECS に light 型を作らず、ローダー(バインダ)が `LightContainer` へ直接振り分ける。将来内部を ECS に移管しても **JSON 形式は変わらない** — 解釈レイヤ分離([第3章](03_project_format.md) §3.5)の直接の配当。

| type | 必須フィールド | 任意(既定値) |
|---|---|---|
| `"directional"` | `direction` [x,y,z], `color` [r,g,b] | `intensity`(1.0) |
| `"point"` | `position`, `color` | `intensity`(1.0) |
| `"spot"` | `position`, `direction`, `color` | `intensity`(1.0), `innerConeAngle`(12.5), `outerConeAngle`(17.5) |

- コーン角は**度数**です(カメラの `yfov` はラジアン — 単位の非対称に注意)。
- ライトの表示名はオブジェクトの `name`(型別に一意。無名ライトは複数可)。
- シャドウは先頭の directional light の向きから作られます([第6章](06_rendering.md))。
- ⚠️ 既知の挙動: `KeyLight` / `FillLight` / `PointLight1` / `SpotLight1` という名前のライトには、デモ用の**名前決め打ちアニメーション**がまだ残っています(`lightcontainer.cpp`)。この名前を使うと勝手に動きます。

### collider(✅・WP46/47/107)— ECS を経由しない(PhysWorld へ)

| フィールド | 規則 |
|---|---|
| `shape` | **必須**。`"sphere"` / `"box"` / `"capsule"` |
| `pos` / `rotation` | 任意(既定 [0,0,0] / [0,0,0,1])。オブジェクト基準のローカルオフセット |
| `radius` | sphere/capsule 必須(> 0) |
| `half_extents` | box 必須([x,y,z] 全成分 > 0) |
| `half_height` | capsule 必須(>= 0) |
| `layer` / `mask` | 任意 uint32(既定 `1` / `0xffffffff`)。query と reciprocal に一致した collider だけ対象 |
| `trigger` / `one_way` | 任意 bool(既定 false)。query metadata。イベント/方向 policy 自体は別層 |

旧名 `size` / `height` は半分値の新名を案内する hard error です。オブジェクトに `transform` があれば移動に追従します。クエリ(raycast / overlap / shapeCast)は [第8章](08_gameplay.md) を参照してください。なお `PELICAN_WITH_PHYSICS=OFF` ビルドでは collider を含むシーンは名指しエラーになります(パージ可能境界)。

### animation(✅WP38)— スケルタルクリップ再生

skinned glTF モデルと組み合わせて、クリップを宣言的に再生します(ゲームコード不要):

```json
{ "name": "simplemodelview", "model": "character" },
{ "name": "animation", "clip": "character.glb#animation/Turn", "speed": 1.0, "loop": true, "start_time": 0.0 }
```

| フィールド | 必須? | 既定 | 規則 |
|---|---|---|---|
| `clip` | **必須** | — | **`<glb>#animation/<クリップ名>` フラグメント参照**。モデルと**同一 glb** でないと実行時エラー |
| `speed` | 任意 | 1.0 | 再生速度 |
| `loop` | 任意 | true | 非 bool は名指しエラー |
| `start_time` | 任意 | 0.0 | 開始オフセット秒 |
| `graph` | 禁止 | — | v1 予約キー(存在するだけでエラー)。グラフ再生は C++ API — [第8章](08_gameplay.md) |

制限: 補間は LINEAR / STEP のみ(CUBICSPLINE はロードエラー)、モーフターゲットは非対応(VRM-S1 送り)、joint 上限 128。時刻源は `EngineTime`(rpc `set_time` と同一経路 = 決定的)。

### sprite_view(✅WP103/104/106)— 2D スプライト

```json
{ "name": "OneWayPlatform",
  "components": [
    { "name": "transform", "pos": [-0.6, 1.2, 0.05], "rotation": [0,0,0,1], "scale": [1,1,1] },
    { "name": "sprite_view", "texture": "demo_atlas#sprite/full", "size": [1.8, 0.12],
      "pivot": [0.5, 0.5], "layer": 1, "color": [0.95, 0.75, 0.25, 1.0] },
    { "name": "collider", "shape": "box", "half_extents": [0.9, 0.06, 0.5], "one_way": true }
  ] }
```

(実物: [../../projects/sprite_demo/scene.json](../../projects/sprite_demo/scene.json))

| フィールド | 必須? | 既定 | 規則 |
|---|---|---|---|
| `texture` | **必須** | — | asset_data の `textures[].name`、または `<名前>#sprite/<スプライト名>`(atlas 参照) |
| `size` | 任意 | ソース px ÷ pixels_per_unit | `[w, h]`(有限・正) |
| `pivot` | 任意 | [0.5,0.5] | 0..1 |
| `color` | 任意 | [1,1,1,1] | 乗算 tint(リニア) |
| `flip` | 任意 | [false,false] | [flip_x, flip_y] |
| `layer` | 任意 | 0 | int16 のソートキー |
| `billboard` | 任意 | "none" | `"none"` / `"y_axis"` / `"full"` |

**closed schema**(未知キーはエラー)。スプライトは常に **XY 平面 + Z 法線**(2026-07-12 決定 — 床置きは Transform 回転で)。描画には rendering config の `sprite` feature 参照が必要です([第6章](06_rendering.md) §6.9)。

## 4.3 シーンロードの流れ

`SceneLoader::load(scene_id)`([scene.cpp](../../src/core/loader/scene.cpp))の実際の処理順:

1. **正規化・検証**(解釈レイヤ `sceneformat.cpp`): エンベロープ・識別子・name 一意性をチェック。不正は fail-fast。
2. 各オブジェクトの components を振り分け: `light` → LightContainer、`collider` → PhysWorld、**それ以外 → ECS**(未知名はエラー)。ライトだけのオブジェクトは ECS entity を作りません。
3. 旧シーンをクリア(name 束縛・物理・全 GameObject・モデルインスタンス)。
4. ECS オブジェクトを**トランザクション生成**(§4.4)。JSON 不正はロールバックして例外。
5. name があり transform を持つオブジェクトは「name → GameObjectId」を束縛(rpc の宛先になる)。
6. `SceneLoaded` イベントを emit([第8章](08_gameplay.md))。

### シーン遷移(✅・WP52)

- ゲームコード: `ctx.loadScene("scene_flow_second")`、rpc: `load_scene`。
- 呼び出しは**予約のみ**(シーンの存在チェックだけ即時)。実際の切替は**フレーム末尾**(ゲームシステム実行後、描画前)に適用されます。更新処理の最中に ECS を壊さないための遅延適用です。
- 切替時は全 GameObject が破棄され、起動時と同じ経路で再構築されます。旧シーンの GameObjectId は世代の仕組みにより必ず「無効」と判定されます(誤って別オブジェクトを触ることがない)。
- 非同期ロード(S2)は 📐設計のみです。

### フレームループの順序(ヘッドレスも同順)

```
入力スナップショット → 時刻 advance → イベント配送 → ECS システム群
→ ゲームシステム(order 順) → SeqPlayer → シーン遷移の適用 → 描画
```

## 4.4 ランタイム ECS の要点(ゲーム開発者向け)

ECS の実装(WP62・R5-core ✅)は archetype/chunk 方式ですが、ゲームコードから chunk を直接触る API は公開されていません。**`GameObjects` / `GameContext` ファサード経由**で操作します([第8章](08_gameplay.md) に実例)。知っておくべきことは次の 3 点に要約できます:

1. **GameObjectId はフレームを跨いで保持してよい。** ID は世代付き(index + generation の 8 バイト)で、削除済み ID は安全に「無効」と検出されます(no-op + bool 返却)。
2. **コンポーネントへのポインタ・参照の保持は禁止。** 内部の詰め替え(relocate)で無効になります。毎回 ID から引き直してください。
3. **`remove()` の戻り値(bool)を見る。** 既に消えていたら false が返ります(例外にはなりません)。

> **設計決定(生成トランザクション):** オブジェクト生成は construct → populate → init → publish の順で行われ、途中で失敗すると**逆順にロールバック**される。publish 前の ID は外から観測できない。正当性を性能予算で緩めない(全スロットの値構築を無条件で行う)ことが規約として明文化されている。

> **設計決定(stale ID の扱い):** 削除済み ID へのアクセスは「no-op + bool」または「名前入りの例外」(API による)で、debug ビルドだけ assert で挙動が変わるようなことはしない。generation が枯渇したスロットは再利用せず永久 retire(低確率の ABA を「許容」しない)。

エンジン内部の ECS システム(transform → 描画反映、カメラ反映など)は依存グラフをトポロジカルソートして JobSystem で並列実行されます。ゲームロジックはこれとは別の「ゲームシステム」(`PELICAN_REGISTER_SYSTEM`)として決定的な順序で実行されます([第8章](08_gameplay.md))。

### ユーザー定義コンポーネント

シーン JSON に書けるコンポーネントの語彙は現在エンジン組み込みのみで、**プロジェクトから新しい ECS コンポーネント型を登録する公開 API はまだありません**(📐構想段階)。ゲーム固有の状態はゲームシステムのメンバ変数として持つのが現行の形です。

## 4.5 よくあるエラー

エラーメッセージはすべて名前入りなので、メッセージで検索すれば原因に到達できます。

| メッセージ(抜粋) | 原因 |
|---|---|
| `scene data schema is not supported; use schema 'pelican.scene' with version 1 ...` | エンベロープ欠落・レガシー形式 |
| `scene field 'lights' is not supported in v1; use named objects with a 'light' component` | 旧 lights セクション |
| `scene not found: <id>` | `default_scene_id` / `loadScene` の指定ミス |
| `Unknown component 'X' on object 'Y'` | コンポーネント名のタイポ |
| `duplicate object name 'X' in scene 'Y'` | name 重複 |
| `Light 'X' requires vec3 field: Y` | ライトの必須フィールド欠落 |
| `'size' is not supported in v1; use 'half_extents'` | collider の旧キー名 |

## 4.6 既知の注意点

- `CameraSystem` は現状、camera コンポーネントを持つ**先頭 1 entity のみ**を処理します。複数カメラを置いた場合の意味論は未規定です(アクティブ投影は「シーン中最初の camera」)。
- example の `basic_config.camera.up` は `[0,-1,0]`(Y 下向き up)です。自作プロジェクトで座標系に迷ったらここを確認してください。
- スケルタルアニメーション(glTF スキン再生)は ✅WP38 で実装済みです(§4.2 の `animation` コンポーネント)。頂点キャッシュ系のアニメーションは従来どおり VAT([第5章](05_assets.md))が担当します。

## 関連文書

- [../design_scene_format.md](../design_scene_format.md) — pelican.scene v1 の設計(互換規則の記述は WP63 以前のもの)
- [../design_ecs_lifecycle.md](../design_ecs_lifecycle.md) — ECS ライフサイクル設計(WP62 で実装)
- [../design_camera_system.md](../design_camera_system.md) — カメラの三層設計
- [../design_physics_queries.md](../design_physics_queries.md) — collider と物理クエリ
- [第3章 プロジェクト形式](03_project_format.md) / [第8章 ゲームロジック](08_gameplay.md)
