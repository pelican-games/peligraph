# 第4章 シーンと ECS

対象: pelican2(2026-09-04 時点、HEAD=`d146b8bd`)/ このマニュアルはコードを正とする

## この章で学ぶこと

- シーン形式 `pelican.scene` v1 の正確なスキーマと実物例
- v1 で書けるコンポーネント全種類(必須/任意フィールド・既定値・単位)— **`behavior` を含む**
- シーンがロードされてから ECS・ライト・物理に振り分けられるまでの流れ — **1 つのトランザクション**としての scene load
- ランタイム ECS(世代付き ID・生成トランザクション)の、ゲーム開発者が知るべき要点
- ゲーム DLL から ECS コンポーネントを登録する方法と、名前空間の規則
- シーン遷移(`loadScene`)の仕組み

正本の設計文書は [../design_scene_format.md](../design_scene_format.md)(v1.1)ですが、コンポーネントの受理仕様の多くはコードにのみ存在するため、この章の表は実装から採録しています。正本の所在は、エンベロープと識別子が [sceneformat.cpp](../../src/project/sceneformat.cpp)、**コンポーネントの受理仕様が [componentcodec.cpp](../../src/core/loader/componentcodec.cpp)**、振り分けが [scene.cpp](../../src/core/loader/scene.cpp)、behavior が [behaviorarena.cpp](../../src/core/gamelogic/behaviorarena.cpp) です。

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
| `version` | **必須**。整数で **`1` か `2`**(0・負・3 以上は拒否。`2` は prefab(`pelican.prefab` 文書の展開。台帳 WP361)を使う scene 用。v1 に `prefab` を書くと `prefab_requires_scene_v2` で拒否) |
| `scenes` | **必須**。`{ "<scene_id>": { "objects": [...] }, ... }` |

- **hard error** とは、警告を出して続行するのではなく、その場で例外を投げてロードを打ち切る失敗のことです(エンベロープの検査なら [validateEnvelope](../../src/project/sceneformat.cpp#L52) が `std::runtime_error` を投げます)。シーンロードは 1 つのトランザクションなので(§4.3)、hard error になったシーンが半分だけ構築されて残ることはありません。
- schema なしのレガシー形式、トップレベル `lights` セクションは **v1 では受理されません**(名指しのエラーで移行先を案内)。※設計文書には「WARN+自動解釈」と書かれていますが、WP63(strict v1 化)で意図的に廃止されており、実装が正です。
- 本文の `WP63` / `✅WP79` のような `WP<数字>` は、エンジン側の作業単位(work package)に付く通し番号です([用語集](14_glossary.md))。「どの作業で入った仕様か」の目印として本文に添えています。
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

> **設計決定(コンポーネント名 = 登録名):** シーン形式が定義するのは「`name` + 任意パラメータのレコード列」まで。コンポーネントの語彙は ECS の登録名(+バインダの特別扱い)で決まり、新コンポーネント追加にシーン形式仕様の改訂は不要。未知の名前は `component_unknown@1{"name":"X"}` で fail-fast。

> **設計決定(ファイルパス禁止):** コンポーネントのパラメータにファイルパスを直接書くことは禁止。モデルは必ず `asset_data.json` の登録名(`model: "sotai"`)経由で参照する。パス解決規則の適用面を asset_data.json に限定し、シーン検証をエンジン・web・外部ツールで安価にするため。

## 4.2 コンポーネント一覧(v1 で書けるものすべて)

> **設計決定(受理仕様は component codec が正・✅WP151):** `transform` / `simplemodelview` / `camera` / `light` / `collider` / `animation` / `sprite_view` の 7 種は、**[componentcodec.cpp](../../src/core/loader/componentcodec.cpp) の codec が受理仕様の正**です(以前は各コンポーネントの `ref()` が正でした)。7 種はいずれも **closed schema** — 表に無いキーを書くと名指しのエラーになります。エディタの読み書き・インスペクタのウィジェット・保存の往復もすべてこの codec を通ります([第13章](13_editor.md))。`localtransform` だけは従来どおり `ref()` 経路です。

codec の表には 8 個目として `pelican_ray_instance` が並んでいますが、これは **ビルトイン ray instance が有効なときだけ**カタログに現れます(無効なら `componentCodecs()` が末尾 1 個を落とすため、名前自体が未知になります)。詳細は下の `pelican_ray_instance` の項を見てください。

### transform(✅)

| フィールド | 必須? | 既定 | 意味 |
|---|---|---|---|
| `pos` | 任意 | `[0,0,0]` | 位置 |
| `rotation` | 任意 | `[0,0,0,1]` | クォータニオン(**x,y,z,w 順**) |
| `scale` | 任意 | `[1,1,1]` | スケール |

3 つとも省略でき、`{"name":"transform","pos":[0,0,0]}` のように必要なキーだけ書けます。

### simplemodelview(✅)— モデル表示

```json
{ "name": "simplemodelview", "model": "sotai" }
```

`model` は `asset_data.json` の `models[].name` を指します(`foo.glb#mesh/名前` のフラグメント登録も可 — [第5章](05_assets.md))。✅WP67 で**単一コンポーネントに統合**されました: `SimpleModelViewComponent` が `model_name` + `dirty` + インスタンスハンドルを 1 つで持ちます(旧 `simplemodelviewupdate` は消滅。JSON の書き味は不変)。実行時に `model_name` を書き換えれば(`init` が dirty を立てるため)モデル差し替えもできます。

### localtransform(✅)

`pos` / `rotation` / `scale`(すべて必須)。毎フレーム値が `transform` へ反映されます。親子階層はオブジェクト直下の `parent` キー(§4.1)で宣言します — localtransform コンポーネント自体に親を書くフィールドはありません。

### camera(✅)

CameraComponent 自体は**マーカー**です。マーカーコンポーネントとは、値を 1 つも持たず「このオブジェクトはカメラである」という印だけを付けるコンポーネントのことで、[CameraComponent](../../src/core/ecs/predefined/camera.hpp#L5) の中身はダミーのフィールド 1 個だけです。投影・コントローラのパラメータは ECS ではなく Camera モジュールがシーン JSON から直接読みます。

| フィールド | 規則 |
|---|---|
| `type` | `"perspective"` / `"orthographic"`。省略時は xmag/ymag の有無で推定 |
| `yfov` | **ラジアン**。省略時は `basic_config.camera` へフォールバック |
| `znear` / `zfar` | 省略時フォールバック |
| `aspect` | 任意(省略時はビューポートから計算) |
| `xmag` / `ymag` | orthographic 用 |
| `perspective` | `{yfov, znear, zfar, aspect}` の**ネスト形式**(glTF 流。closed schema) |
| `orthographic` | `{xmag, ymag, znear, zfar}` のネスト形式。`perspective` との併記はエラー |
| `params` | 任意 object(authoring の往復保存用) |
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
| `"directional"` | `direction` [x,y,z], `color` [r,g,b] | `intensity`(1.0), `range` |
| `"point"` | `position`, `color` | `intensity`(1.0), `range` |
| `"spot"` | `position`, `direction`, `color` | `intensity`(1.0), `range`, `innerConeAngle`(12.5), `outerConeAngle`(17.5) |

- `color` は**必須**で、各成分は `[0,1]` の範囲です。
- `range` は任意の正の値(0 は「未指定」扱い)。
- 型に合わないキーは名指しで拒否されます: directional に `position`、point に `direction`、spot 以外に `innerConeAngle` / `outerConeAngle` はいずれもエラーです。
- コーン角は**度数**(`[0,180]`。`innerConeAngle` が `outerConeAngle` を超えるとエラー)です。カメラの `yfov` はラジアンなので、単位の非対称に注意してください。
- ライトの表示名はオブジェクトの `name`(型別に一意。無名ライトは複数可)。
- シャドウは先頭の directional light の向きから作られます([第6章](06_rendering.md))。

> **設計決定(オーサリング境界・✅WP142):** かつてエンジンのコアには「`KeyLight` / `FillLight` / `PointLight1` / `SpotLight1` という**名前のライトを勝手にアニメーションさせる**」デモ用コードが残っていました。これは撤去され、ライトを動かす処理は example のゲームコード(ユーザー空間)へ移りました。ライトを実行時に動かすには [第8章](08_gameplay.md) §8.4 のライト API(`setDirectionalLightDirection` など)を使います。**特定の名前を付けても、もう何も起きません。**

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

制限: 補間は LINEAR / STEP のみ(CUBICSPLINE はロードエラー)、joint 上限 128。モーフターゲットは描画対応 ✅WP121(初期 weight で表示)ですが、**クリップの `weights` チャネルは v1 非対応**(表情などの weight 駆動は [第8章](08_gameplay.md) §8.13 の VRM service 経由)。時刻源は `EngineTime`(rpc `set_time` と同一経路 = 決定的)。

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

### behavior(✅WP155/162/167)— オブジェクトに振る舞いを貼る

ゲームコードで書いた振る舞いを、パラメータごとオブジェクトに貼りつけます。C++ 側の書き方は [第8章](08_gameplay.md) §8.5 が本編で、ここでは**シーン JSON 側の書式**だけを説明します。

```json
{ "name": "TriggerZone", "components": [
    { "name": "transform", "pos": [0, 0, 0] },
    { "name": "collider", "shape": "sphere", "radius": 1.0, "trigger": true },
    { "name": "behavior", "type": "wp179_trigger_behavior" }
] }
```

(実物: `test/run_physics_trigger_behavior.ps1`)

| フィールド | 必須? | 規則 |
|---|---|---|
| `type` | **必須** | `PELICAN_REGISTER_BEHAVIOR`(ゲームコードが `<behavior.hpp>` を include して使うマクロ。定義は [registerer.hpp](../../src/core/userpublic/details/behavior/registerer.hpp#L385))の第 2 引数(永続名)。空文字・非文字列はエラー |
| `params` | 任意 | オブジェクト。省略すると全フィールドが既定値。型・範囲は behavior 側の schema で検証され、違反は `field 'params.speed' is out of range` のようなフィールドパス付きエラー |

- **同じオブジェクトに何個でも書けます**(コンポーネントの中で唯一)。実行順は宣言順です。
- light / collider と同じく **ECS を経由しません**(エンジン所有の attachment arena に入ります)。ただし light と違い、**behavior だけを持つオブジェクトにも ECS entity が作られます**。
- ゲームロジック DLL に無い `type` を書くと `Unknown behavior type 'X' on object 'Y'` で起動が止まります。DLL がまだロードされていない場合だけは WARNING を出して保留され、ロード後に有効化されます。

### pelican_ray_instance(条件付き)— ray tracing のインスタンス属性

**プロジェクトの ray tracing 設定が ray layout の LayoutV2 セマンティクスで構成されているときだけ**登録される ECS コンポーネントです([predefined.cpp](../../src/core/ecs/predefined.cpp#L39) が `builtinRayInstanceEnabled()` を見ています。ray layout 自体は [第6章](06_rendering.md))。構成されていなければ登録も codec も無いので、書くと `component_unknown@1` です。

```json
{ "name": "pelican_ray_instance", "mask": 255,
  "instance_flags": ["force_opaque"], "custom_index": 12 }
```

| フィールド | 規則 |
|---|---|
| `mask` | 任意 uint(0..255) |
| `instance_flags` | 任意。**文字列配列**。語彙は `triangle_facing_cull_disable` / `triangle_flip_facing` / `force_opaque` / `force_no_opaque` |
| `custom_index` | 任意 uint(0..0x00ffffff) |
| `overrides` | **現状は必ず拒否**(下記) |

- 3 つとも省略可ですが、**1 つも書かないとエラー**(`pelican.ray_instance_empty@1`)です。closed schema で、上記以外のキーは `pelican.ray_instance_unknown_field@1`、範囲外の値は `pelican.ray_instance_field_range@1` になります。
- `instance_flags` は配列でないと `pelican.ray_instance_field_type@1`、重複も同じコード、未知の名前は `pelican.ray_as_unknown_flag@1`、`force_opaque` と `force_no_opaque` の併記は `pelican.ray_as_conflicting_instance_flags@1` です([componentcodec.cpp](../../src/core/loader/componentcodec.cpp#L1144))。この codec の名前付き拒否は `pelican.` 接頭辞 + `@1` が付きますが、**`overrides` の拒否だけは接頭辞が付きません**(下記)。
- `overrides`はWP382dで有効化されました。occurrence IDを指定した行でmask/flags/custom indexを上書きできます。対象が失われても行を消さず、`OccurrenceAbsent`として診断へ残します。previewでは`ray_instance_overrides_unsupported@1`を返します。

### コンポーネント名の名前空間と所有者(✅WP379/379a)

コンポーネント名は「誰が登録したか」で受理規則が変わります([enforceComponentRegistrationPolicy](../../src/core/userpublic/details/component/registerer.cpp#L21)):

| 登録元 | `pelican_` 接頭辞 | 接頭辞なし |
|---|---|---|
| エンジン(host) | OK | `builtin_name_unprefixed@1`(既存 7 名だけ grandfathered) |
| 外部(ゲーム DLL など) | `reserved_component_namespace@1` で拒否 | OK |

- grandfathered の 7 名は `eid` / `transform` / `localtransform` / `simplemodelview` / `camera` / `animation` / `sprite_view` です。**これ以降エンジンが足すビルトインは必ず `pelican_` で始まります**(`pelican_ray_instance` が最初の適用例)。
- エンジン所有として登録する registrar([EngineComponentRegisterer](../../src/core/ecs/enginecomponentregistrar.hpp#L15))は **host 専用**で、ゲーム DLL からは include しても**リンク以前にコンパイルが通りません**(`registerEngineComponent` を呼ぶ game DLL は専用のコンパイル失敗テストで守られています)。
- シーン JSON に**未登録の名前**を書くと `component_unknown@1{"name":"X"}`([componentinfo.hpp](../../src/core/ecs/componentinfo.hpp#L27))。**登録済みだが codec が無い**名前は、ロードはできてもエディタからは編集できず `not_editable`([editorjournal.cpp](../../src/core/communication/editorjournal.cpp#L225))になります。ゲーム DLL が登録した独自コンポーネントは(codec を持たないため)この状態です。

## 4.3 シーンロードの流れ

[`SceneLoader::load(SceneLoadRequest)`](../../src/core/loader/scene.cpp#L546) は **1 つのトランザクション**です(✅WP380a〜e)。**トランザクション**とは、途中の状態を外から見せず「全部成功して切り替わる」か「1 つも変わらない」かのどちらかにする実行単位のことです([用語集](14_glossary.md))。「旧シーンを消してから新シーンを作る」のではなく、新シーンの候補を全部作って一斉に切り替え、失敗したら全部を旧シーンに戻します。

**この経路は 1 本だけ**です。起動時のロード / rpc `load_scene` / `ctx.loadScene()` の遅延適用(pending)/ エディタからのスナップショット import は、入口([SceneLoadRoot](../../src/core/loader/sceneload.hpp#L82))が違うだけで同じ executor を通ります。実際の処理順:

1. **正規化・検証**(解釈レイヤ `sceneformat.cpp`): エンベロープ・識別子・name 一意性をチェック。不正は fail-fast。
2. **prepare**: 各オブジェクトの components を振り分け: `light` → LightContainer、`collider` → PhysWorld、**`behavior` → BehaviorAttachmentArena**、**それ以外 → ECS**(未知名・未登録の behavior 型名はここでエラー)。ライトだけのオブジェクトは ECS entity を作りませんが、**behavior だけのオブジェクトには作られます**。描画・カメラ・ライト・SeqPlayer/VAT・乱数列・イベント送信箱の候補も、稼働中の状態に触らずここで作ります。モデル資産(`ModelAssetContainer`)だけは候補ではなくキャッシュで、失敗しても読み込んだモデルは残ります(どのシーンにも属さないので世代混在にはなりません)。
3. **early_publish**: ECS オブジェクトを**トランザクション生成**(§4.4)し、name があり transform を持つオブジェクトの「name → GameObjectId」束縛(rpc の宛先になる)と物理・behavior の候補を作り、旧 entity を **unpublish** します。unpublish とは、ID から entity を引けなくすること(id テーブルの live を落とし、generation を 1 つ進める)です。コンポーネントの値はその場では壊さず破棄トークンへ退避され、実際に壊れるのは retire の完了時です([publishDestroy](../../src/core/ecs/archetypemigration.cpp#L760))。JSON 不正はここまでで全部戻して例外。
4. **publish**(失敗しない): 候補を稼働中の状態と入れ替えます(swap だけ)。
5. **activate**: **behavior を宣言順に有効化**して `onInit` を呼びます。ここが**失敗しうる最後の段**で、途中で例外が出たら init 済みの分を逆順に `onDestroy` した上で、**描画・ECS・カメラ・ライト・物理・behavior・イベント・乱数・シーン遷移要求の全部**を旧シーンに戻します。document・現在シーン名・authority も旧シーンのままです(`onInit` の `emit` / `loadScene()` も一緒に捨てられます)。
6. **commit_metadata**: シーン文書・現在シーン名・epoch などの metadata を 1 回の swap で切り替えます。**ここが commit 点**で、以降は `PreCommitFailure` になりません。
7. **retire**(旧シーンの後片付け): 旧シーンの entity・モデルインスタンス・一時モデルは即座には消さず、次フレーム(次の pending load 適用時と描画更新の先頭)に消費される retirement queue に積みます。旧 behavior の `onDestroy` はこの段で、`attachment_seq` の逆順に走ります。
8. **flush**: 溜めておいた作用を実際に外へ出します — `SceneLoaded` イベントの emit([第8章](08_gameplay.md))、乱数列の公開、音の再生/停止/バス音量、`user://` への書き込み。
9. **finish**: 生成済み entity のトークンを確定します。

2〜9 の 8 段が [SceneLoadPhase](../../src/core/loader/sceneload.hpp#L27) の 8 値(`prepare` / `early_publish` / `publish` / `activate` / `commit_metadata` / `retire` / `flush` / `finish`)にそのまま対応します(1 は executor に入る前の解釈レイヤなので phase ではありません)。ロード結果に付く phase trace は内部の順序検証(と fault injection)のためのもので、rpc には出ていません。

### ロード中の「作用」はトランザクションに溜まる(✅WP380b〜e)

`onInit`(activate)や、ゲーム DLL が登録したコンポーネントの JSON ローダ(early_publish)からエンジン API を呼ぶと、それは**その場では起きません**。[PreparedEffectTransaction](../../src/core/loader/effecttransaction.hpp#L27) に **staging** されます。staging とは、API の呼び出しをその場で実行せず、トランザクションの控え(送信箱 = outbox)に積んでおくことです([用語集](14_glossary.md))。積んだ作用は**失敗すれば消え、commit したときだけ flush で実行**されます。

| 呼んだもの | ロード中の扱い |
|---|---|
| `emit(...)` | 送信箱に staging。flush で pending queue へ(配送は従来どおり次フレーム頭) |
| `setSeed` / `random()` 系 | 候補の乱数列に対して進む。commit 後に公開 |
| `loadScene(name)` | 候補の pending 要求として保持(名前の存在チェックは即時) |
| `playSound` / `stopSound` / `setBusVolume` | 音は鳴らず、voice slot だけ予約。flush で実際に再生 |
| `setGameSettings` / `saveSettings` / `saveData` | shadow に書き、ディスクへは flush で atomic 書き込み |

**読み取りは自分の書き込みを見ます。** ロード中の `isPlaying(h)` は「さっき staging した `playSound` のハンドル」に true を返し、その後 `stopSound` すれば false。`loadData(slot)` は staging 済みの `saveData` の中身をそのまま返し、`listSaves()` にも staging 済みスロットが並びます — ただし**まだ書いていないので `timestamp` は 0**([listSaves](../../src/core/loader/effecttransaction.cpp#L407))。`gameSettings()` も同じく shadow を返します。

**構造変更(mutation = entity の生成・削除・localtransform の書き換え)の直接呼び出しは名前付きで拒否されます。** トランザクション中に behavior のコールバック**外**から `GameObjects::create` / `remove` / `setLocalTransform` を呼ぶと、その呼び出し自体が [`SceneTransactionMutationRejected{api: ...}`](../../src/core/userpublic/gameobjects.hpp#L20)(`std::logic_error` の派生。`api` にはねられた API 名が入ります)を投げます([gameobjects.cpp](../../src/core/userpublic/gameobjects.cpp#L14) が入口ごとに判定。`removeAll` は behavior のコールバック中でも拒否)。踏むのは主に、ゲーム DLL のコンポーネント JSON ローダから ECS を触ろうとしたときです。

### 旧シーンの onDestroy が見えるもの

retire 中の `onDestroy` は「自分の最終状態」を読めます。entity はもう unpublish されていますが、`ctx.localTransform(self)` / `ctx.spriteView(self)` は**破棄トークンが持つスナップショット**を返します([retiredComponent](../../src/core/loader/effecttransaction.cpp#L462))。書き込み側(`setLocalTransform` / `setSpriteView` / `setSpriteTexture`)は throw せず **false** を返し、`SceneRetiredObjectMutationRejected` として `effect_errors` に積まれます([rejectRetiredMutation](../../src/core/loader/effecttransaction.cpp#L473))。

- `onDestroy` は `noexcept` なので、この段の fallible API(`playSound` / `saveData` など)は**例外を投げずに `effect_errors` へ**回されます。呼び出し側のゲームコードは戻り値だけ見ればよく、握りつぶしではなく記録されます。
- retire 中の `loadScene(name)` は、**commit 済みのシーン集合**に対して解決されます([requestLoadDuringRetire](../../src/core/loader/scene.cpp#L1236))。旧シーンにしか無い名前を書くと `scene not found in committed projection: X` になり、`scene_retire_load_rejected@1` として記録されます。

### commit したのに作用が失敗したとき

flush で音やディスク書き込みが失敗しても、**シーンは新しいまま**です。この結果は [`CommittedWithEffectFailure`](../../src/core/loader/sceneload.hpp#L175) として返り、rpc の `load_scene` は例外ではなく `{"name": ..., "committed": true, "effect_errors": [...]}` を返します([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1660))。`effect_errors` の各要素は `{api, error_code, error_fields, phase}` で、`api` は `playSound` / `saveData` などの公開 API 名、`phase` は `activate` / `destroy` / `flush` のどれかです。

- 「シーンは切り替わらなかった」= commit 前の失敗は従来どおり**例外**です。両者を混同しないでください。
- staging できる作用は 256 件までです。257 件目からは**控えに積まれず、実行もされません**(`playSound` は無効なハンドルを返します)。捨てた件数だけが `effect_outbox_full@1{dropped: N}` という 1 件のエラーにまとめて記録されます([reserveEntry](../../src/core/loader/effecttransaction.cpp#L114))。

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
2. **コンポーネントへのポインタ・参照の保持は禁止。** ECS は同じ構成の entity を連続した配列(chunk)に詰めて持つため、entity を 1 つ消すと配列の末尾の entity がその穴へ移されます。この移動が**詰め替え(relocate)**で([publishDestroy](../../src/core/ecs/archetypemigration.cpp#L760))、自分とは無関係な entity の生成・削除でもコンポーネントのアドレスは変わります。毎回 ID から引き直してください。
3. **`remove()` の戻り値(bool)を見る。** 既に消えていたら false が返ります(例外にはなりません)。

> **設計決定(生成トランザクション):** オブジェクト生成は construct → populate → init → publish の順で行われ、途中で失敗すると**逆順にロールバック**される。publish 前の ID は外から観測できない。正当性を性能予算で緩めない(全スロットの値構築を無条件で行う)ことが規約として明文化されている。

> **設計決定(stale ID の扱い):** 削除済み ID へのアクセスは「no-op + bool」または「名前入りの例外」(API による)で、debug ビルドだけ assert で挙動が変わるようなことはしない。generation が枯渇したスロットは再利用せず永久 retire(低確率の ABA を「許容」しない)。

エンジン内部の ECS システム(transform → 描画反映、カメラ反映など)は依存グラフをトポロジカルソートして JobSystem で並列実行されます。ゲームロジックはこれとは別の「ゲームシステム」(`PELICAN_REGISTER_SYSTEM`)として決定的な順序で実行されます([第8章](08_gameplay.md))。

### ユーザー定義コンポーネント(✅WP379/382a)

ゲーム DLL から **ECS コンポーネント型を登録できます**。`DECLARE_COMPONENT` で ID を宣言し、[`registerComponent<T>(name)`](../../src/core/userpublic/details/component/registerer.hpp#L185) を静的初期化で呼びます:

```cpp
#include <details/component/registerer.hpp>
#include <details/ecs/componentdeclare.hpp>

struct MyComponent {
    std::string label;
    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("my_component"),
        Pelican::defaulted(Pelican::field<&MyComponent::label>("label"), ""));
};
DECLARE_COMPONENT(MyComponent, 61);   // 0..63 の空き番号(エンジンは 0..19 を使用中)
// 静的初期化で: Pelican::internal::getComponentRegisterer()
//                   .registerComponent<MyComponent>("my_component");
```

(実物: `test/fixtures/wp382a_schema/v3_fixture.cpp` / `test/fixtures/ray_namespace_component/component.cpp`)

- 名前は `pelican_` で始められません(§4.2 の名前空間表)。ID はエンジンと共有の 64 スロット(`MAX_COMPONENTS = 64`)から選びます。
- 登録すればシーン JSON に `{"name": "my_component", ...}` と書けます。**エンジン側の codec は無い**ので、受理仕様は自分の `schema`(または `ref()`)が正で、エディタからは `not_editable` です。
- ⚠️ ここは `PELICAN_REGISTER_SYSTEM` / `PELICAN_REGISTER_BEHAVIOR` のような専用マクロがまだ無く、`details/` のヘッダを直接叩く形です。**ユーザー空間ではあるが、まだ整った SDK 面ではありません。**
- 「オブジェクト固有の authored データと毎フレーム処理」だけが欲しいなら、今も `behavior`(§4.2・[第8章](08_gameplay.md) §8.5)の方が短く書けます。シーン全体にまたがる状態はゲームシステムのメンバ変数です。

なお、稼働中の entity に対するコンポーネントの追加・削除は、内部的には失敗時に完全巻き戻しできるトランザクション機構(✅WP152)として実装済みですが、**公開 API はまだ無く、エディタ経路だけが使っています**([第13章](13_editor.md))。

### schema が使える値の形(JSON 側・✅WP382a)

コンポーネントや behavior params の schema には、スカラのほかに次の 3 つが宣言できます。C++ の書き方は [第8章](08_gameplay.md) §8.5.3、ここでは **JSON にどう書くか**だけを示します。

**`List<Row>` — 行の集まり。** 素の配列ではなく、**オブジェクトのコンテナ**です。各行が書き換わらない番号 `$row.id` を持つ必要があるためで、添字を行の同一性にすると、先頭に 1 行挿入しただけでエディタの参照や undo が別の行を指してしまいます([../design_declaration_carrier.md](../design_declaration_carrier.md) 決定 2「添字を identity にしない」):

```json
"overrides": {
  "$list": { "v": 1, "next_id": 3 },
  "rows": [
    { "$row": { "id": 1, "v": 1 }, "occurrence_id": "mesh/chair", "mask": 0 },
    { "$row": { "id": 2, "v": 1 }, "occurrence_id": "mesh/table" }
  ]
}
```

| 書き間違い | 結果 |
|---|---|
| 素の配列で書く | `ListContainerMissing` |
| `$list` が無い / `v`・`next_id` が欠ける | `RowEnvelopeMissing` |
| `$list.v` や `$row.v` が 1 以外 | `RowEnvelopeUnsupported` |
| `$list` / `rows` 以外のキー、`$row` に `id`/`v`/`hint` 以外のキー | 名指しの unknown field |
| 行の `id` が重複 | `DuplicateRowId` |
| `id` が 0 | 範囲エラー |
| `next_id` がどの行 `id` より大きくない | `$list.next_id must exceed every row id` |

**行 id は挿入するときだけ書きます。** 既存行の値を編集するときに id を振り直してはいけません(id は行の同一性そのもので、`next_id` はまだ使っていない番号の目印です)。ネストは 1 段まで — 行の schema がさらに `List` を持つ宣言はコンパイルエラーです。

**`Flags<Vocab>` — 文字列の配列。** 宣言した語彙名だけを並べます。配列でない・要素が文字列でない・重複・未知の名前はすべて名指しエラー。**合成則は置換**です — 書いた配列がそのフィールドの値そのものになり、既定値と OR で足し合わされることはありません(`[]` は「何も立っていない」)。語彙は最大 8 個です。

```json
"instance_flags": ["cull_disable", "flip_facing"]
```

**`OptionalOverride` — 省略が「継承」を意味するフィールド。** キーを書かなければ「指定なし」のまま残り、書けばその値で確定します(`null` を書くのは型エラーで、「指定なし」の書き方ではありません)。上書き行(row)の中で「この行だけ mask を差し替える」といった使い方をします。behavior params の**トップレベル**には置けません(そこは全フィールド `defaulted` 必須 — §8.5.3)。`List` を `OptionalOverride` にすることもできません。

## 4.5 よくあるエラー

エラーメッセージはすべて名前入りなので、メッセージで検索すれば原因に到達できます。

| メッセージ(抜粋) | 原因 |
|---|---|
| `scene data schema is not supported; use schema 'pelican.scene' with version 1 ...` | エンベロープ欠落・レガシー形式 |
| `scene field 'lights' is not supported in v1; use named objects with a 'light' component` | 旧 lights セクション |
| `scene not found: <id>` | `default_scene_id` / `loadScene` の指定ミス |
| `component_unknown@1{"name":"X"}` | コンポーネント名のタイポ |
| `duplicate object name 'X' in scene 'Y'` | name 重複 |
| `Light 'X' requires vec3 field: Y` | ライトの必須フィールド欠落 |
| `'size' is not supported in v1; use 'half_extents'` | collider の旧キー名 |
| `reserved_component_namespace@1` | ゲーム DLL が `pelican_` で始まる名前で ECS コンポーネントを登録した(§4.2) |
| `ray_instance_overrides_unsupported@1` | overrides未対応のsurfaceで読み込んだ。native previewは`surface=preview` |
| `ListContainerMissing` / `RowEnvelopeMissing` / `DuplicateRowId` | `List` フィールドを素の配列で書いた・`$list`/`$row` 欠落・行 id 重複(§4.4) |
| `SceneTransactionMutationRejected{api: create}` | シーンロード中に `GameObjects::*` を直接呼んだ(§4.3) |
| `scene not found in committed projection: <id>` | 旧シーンの `onDestroy` から、新シーンに無い名前で `loadScene` した(§4.3) |

## 4.6 既知の注意点

- `CameraSystem` は現状、camera コンポーネントを持つ**先頭 1 entity のみ**を処理します。複数カメラを置いた場合の意味論は未規定です(アクティブ投影は「シーン中最初の camera」)。
- example の `basic_config.camera.up` は `[0,-1,0]`(Y 下向き up)です。自作プロジェクトで座標系に迷ったらここを確認してください。
- スケルタルアニメーション(glTF スキン再生)は ✅WP38 で実装済みです(§4.2 の `animation` コンポーネント)。頂点キャッシュ系のアニメーションは従来どおり VAT([第5章](05_assets.md))が担当します。

## 関連文書

- [../design_scene_format.md](../design_scene_format.md) — pelican.scene v1 の設計(互換規則の記述は WP63 以前のもの)
- [../design_ecs_lifecycle.md](../design_ecs_lifecycle.md) — ECS ライフサイクル設計(WP62 で実装)
- [../design_camera_system.md](../design_camera_system.md) — カメラの三層設計
- [../design_physics_queries.md](../design_physics_queries.md) — collider と物理クエリ
- [../design_object_behaviors.md](../design_object_behaviors.md) — behavior コンポーネント(v2.1)
- [第3章 プロジェクト形式](03_project_format.md) / [第8章 ゲームロジック](08_gameplay.md) / [第13章 エディタ](13_editor.md)
