# 第7章 入力と UI

対象: pelican2(2026-07-21 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- 入力システムの四層構造と、**順序付き FrameInput**(WP69)
- `actions.json`(アクション定義)と `pelican.input_profile`(バインディング)の**分離**(WP91・破壊的変更)
- ゲームパッド対応と、収録・リプレイ(`pelican.input_seq` v2)
- ポインタの位置(framebuffer ピクセル)とカーソルのモード(`ctx.input()`、GP-4)
- 実装済みの UI 基盤 `pelican.ui` v1(ウィジェット・レイアウト・イベント emit)と、ゲームコード(`ctx.ui()`)・共通グラフ(`ui.*` sink)から UI を書く方法(GP-3)
- テキスト HUD(`debug_text`)の使い方

(`WP69` のような **WP** 番号は開発の作業単位です。読み方は [第1章 §1.0](01_overview.md)、状態の一覧は [第11章 §11.1](11_status.md)。)

## 7.1 入力システムの全体像 — 四層構造

> **設計決定(四層):** 入力は L0(デバイスバックエンド)→ L1(フレーム同期)→ L2(アクション層)→ L3(消費者)の四層。L0 を差し替えても L2 から上は変わらない —— 例えば L0 を GLFW から OpenXR へ替えても、`actions.json` の `move` / `jump` を読むゲームコード(L3)は 1 行も変わりません(下表が各層の実物)。アクション層を最初から入れる決め手は **OpenXR がネイティブに action ベース**であること。

| 層 | 内容 | 状態 |
|---|---|---|
| L0 | GLFW(キーボード+マウス+**ゲームパッド 16 スロット**)、**OpenXR(Touch コントローラ + pose)**、rpc 注入、リプレイ注入 | ✅(WP130/132 で XR も実装) |
| L1 | **順序付きイベント + スナップショット**(`FrameInput`、WP37→WP69) | ✅ |
| L2 | アクション層 = `actions.json` + `input_profile`(WP39/91) | ✅ |
| L3 | `Actions` / `GameContext` / **収録・リプレイ**(WP43/45/89) | ✅ |

L1 の要点(✅WP69 で改訂):

- フレーム先頭で入力が**凍結**され、フレーム内で不変(決定性の柱)。
- `FrameInput` は「順序付きイベント列(`ordered_events` — 全イベントに単調増加の `event_seq` が付き、途中座標・発生順が保持される。`event_seq` は時刻ではなく**通し番号**(`std::uint64_t`。0 から採番し、フレームをまたいでも巻き戻らない — [inputstate.hpp:99](../../src/core/os/inputstate.hpp#L99)))」と「集約スナップショット」の両方を持ちます。**この 2 つは同じフレームの同じ入力を 2 通りに見せたもの**で、重複でも競合でもありません(順番が要るならイベント列、いまの状態が要るならスナップショット)。スナップショット側では従来どおり同一フレームの「押して離す」は pressed / released が両方立ちます。
- **入力消費マスク**: ここでいう**消費**は「先に受け取った層がその入力を握り、下の層へ渡さない」ことです。消費の優先順は ImGui → `pelican.ui` → アクション層。UI がポインタを hit / capture したフレームは、そのポインタがアクション層から見えなくなります。
- ImGui が消費者に入るのは**通常ウィンドウ起動のときだけ**です。headless / rpc / リプレイ / golden に加え、✅WP138 以降は **XR session が active な間もエンジンは ImGui frame を begin しません**(XR グラフに `imgui` パスが無いため。論理フレーム境界でゲートが閉じたときは開始済みフレームを閉じてから外します)。この場合の消費順は `pelican.ui` → アクション層になります([第6章](06_rendering.md) §6.12)。
- 入力はフレーム内で不変になり、通常ループと RPC の状態更新は[第1章のフレーム順の図](01_overview.md#frame-order)に従います。

## 7.2 アクション定義とバインディングプロファイル

> ⚠ **破壊的変更(WP91):** かつて `actions.json` の各アクションに書いていた `bindings` 配列は**書くとエラー**になりました(「must define bindings in a pelican.input_profile」)。アクション定義(何ができるか)とバインディング(どのキーで)は別ファイルに分離されています。

### actions.json — アクション定義のみ

有効化は `project.json` の `basic_config.input_actions_json`(任意キー)。実物([../../projects/sprite_demo/input/actions.json](../../projects/sprite_demo/input/actions.json) 全文):

```json
{
  "schema": "pelican.input_actions",
  "version": 1,
  "action_sets": [
    {
      "name": "gameplay",
      "actions": [
        { "name": "move", "type": "axis2" },
        { "name": "jump", "type": "button" }
      ]
    }
  ]
}
```

| フィールド | 規則 |
|---|---|
| `schema` / `version` | `"pelican.input_actions"` / 整数 `1` 必須 |
| `action_sets[].name` | `[a-zA-Z0-9_]`、重複エラー |
| `actions[].name` | `[a-zA-Z0-9_]`。全セット横断でグローバル一意 |
| `actions[].type` | `button` / `axis1` / `axis2` / `pose` |
| `bindings` | **書いたらエラー**(profile へ) |

### pelican.input_profile — バインディング(✅WP91)

実物([../../projects/sprite_demo/input/keyboard.json](../../projects/sprite_demo/input/keyboard.json) 全文):

```json
{
  "schema": "pelican.input_profile",
  "version": 1,
  "name": "keyboard",
  "bindings": [
    { "action": "move", "binding": "kbd:wasd" },
    { "action": "jump", "binding": "kbd:space" }
  ]
}
```

ゲームパッドの実例([../../projects/example/input/profiles/gamepad.json](../../projects/example/input/profiles/gamepad.json) 全文):

```json
{
  "schema": "pelican.input_profile",
  "version": 1,
  "name": "gamepad",
  "bindings": [
    { "action": "move", "binding": "pad:left_stick", "deadzone": 0.2, "invert_y": true },
    { "action": "jump", "binding": "pad:a" }
  ]
}
```

- **deadzone**(不感帯)は、スティックの中立付近のふらつきを 0 に落とすしきい値です。軸の大きさ(0〜1 に正規化した値。`pad:*_stick` では 2 軸の長さ)がこの値以下なら 0 とし、超えた分は再び 0〜1 に伸ばし直します([applyDeadzone()](../../src/core/os/actionmap.cpp#L666))。`0.2` は「20% 以下は倒していない扱い」の意味です。
- binding 要素のフィールド: `action` / `binding` + 任意 `deadzone`(数値 [0,1)、pad 軸限定)、`invert_x`(bool、pad 軸なら axis1 / axis2 どちらでも可)/ `invert_y`(bool、axis2 限定)。反転はいずれも deadzone 適用後です。
- プロファイルの登録と既定は `project.json`: `basic_config.input_profiles`(名前 → ファイルの辞書)と `basic_config.input_profile`(既定名)。
- 切替は 3 通り: 起動引数 `--input-profile <name>` / rpc `set_input_profile` / ゲームコード `selectInputProfile`。筐体・環境差(アーケード筐体等)は**プロファイルを増やして**表現します。
- `--input-action-overlay <uri>` は `pelican.input_action_overlay` v1 bundle の action 集合と固定 profile を**この起動だけ** project 集合へ重ねます。action / action set の同名衝突は名前付きの hard error で、暗黙の上書きはありません。未知 action の問い合わせも hard error のままです。
- `--free-camera [blender|unity]` はこの公開 overlay の同梱 bundle を使います。project の actions/profile と同時に有効で、`--input-profile` は project profile だけを選ぶため併用できます。プロジェクトファイルは変更されません。省略時 preset は `blender` です。各操作は[ツール章 §10.2](10_tools.md#102-pelican_player-cli-リファレンス)にあります。

### binding 記法 `device:control`

| 書き方 | 使える type | 意味 |
|---|---|---|
| `kbd:a`〜`z`, `0-9`, `space`, `enter`, `escape`, `tab`, `backspace`, `left_shift` 等, 矢印, `f1`-`f12`, `num0`-`num9` | button / axis1 | 単キー |
| `kbd:wasd`, `kbd:arrows` | axis2 専用 | 組み込み合成。4 キーの押下を 1 つの 2 次元ベクトル(x・y とも [-1,1])にまとめます |
| `mouse:left` / `right` / `middle` / `button4`-`button8` | button / axis1 | マウスボタン |
| `mouse:delta_x` / `delta_y` | axis1 専用 | フレーム内マウス移動量 |
| `mouse:wheel_x` / `wheel_y` | axis1 専用 | 符号と小数の量を保ち、[-1,1] に clamp するフレーム内ホイール移動量。event のない次フレームは 0 |
| `mouse:shift+middle`, `kbd:ctrl+shift+k` | button 専用 | 修飾 chord。`shift` / `ctrl`(`control`) / `alt` / `super` を `+` で並べ、最後にボタンを書く。左右どちらの修飾キーでも成立 |
| `pad:a, b, x, y, left_bumper, right_bumper, back, start, guide, left_thumb, right_thumb, dpad_up/right/down/left` | button / axis1 | ✅ ゲームパッドボタン 15 種 |
| `pad:left_x, left_y, right_x, right_y, left_trigger, right_trigger` | axis1 | ✅ パッド軸 6 種 |
| `pad:left_stick` / `pad:right_stick` | axis2 専用 | ✅ スティック合成(radial deadzone) |
| `xr:/user/hand/<left\|right>/input/...` | 全 type | ✅WP130。**OpenXR**(VR / AR ランタイムの業界標準 API)が定める**実パス形式** —— `/user/hand/left/input/thumbstick` のような、入力を指すスラッシュ区切りの文字列をそのまま書きます(v1 は Touch コントローラのみ。実物は [touch.json](../../projects/example/input/profiles/touch.json))。`xr:/user/hand/` 以外は名指しエラー。flat 起動では未解決として無視されるので、**同じ actions.json / profile が flat でも XR でも通る** |

- アクティブな profile に `pad:` binding が 1 つも無ければゲームパッドは**一切ポーリングされません**(パージ可能原則)。切断時は保持ボタンの release + 軸ゼロが発行されます。マッピングは GLFW 内蔵の SDL_GameControllerDB。
- 修飾 chord は列挙した修飾キーがすべて押されている間だけ成立し、列挙していない修飾キーは無視します。修飾なし binding の従来の意味は変わらないため、同じ set に `mouse:middle` と `mouse:shift+middle` を別 action として置けば Shift+MMB では両方が成立します。pan / orbit 等の意味上の優先順位は利用側で決めます。
- profile の版は v1 のままです。field や既存 control の意味を変えず、従来は不正だった control 語彙を加えたためです。parser は引き続き version 1 だけを受理します。詳しい判断理由は[入力アーキテクチャ §2](../design_input_actions.md#wheel--修飾-chord-と-profile-の版wp271)を参照してください。
- `pose` type は ✅WP132 で実動作になりました(XR 起動時のみ供給 — [第8章](08_gameplay.md) の `actionPose`)。**pose を含む actions.json は `--record-input` / リプレイが名指しで拒否されます**(input_seq の仕様。v2 でも同じ)。カーソルのロックは ✅GP-4 でゲームコードから頼めるようになりました(下の「ポインタとカーソル」)。

### アクションセットのスタックと消費

(従来どおり)起動時は最初の action_set 1 個が積まれ、上のセットが読んだ入力は下から見えません。操作は `Pelican::Actions::pushActionSet / popActionSet / setActionSetStack`。

### ゲームコードからの読み方

```cpp
void update(Pelican::GameContext &ctx) {
    if (!ctx.actionsConfigured()) return;
    const auto move = ctx.actionAxis2("move");
    if (ctx.actionPressed("jump")) { /* ... */ }
}
```

### ポインタとカーソル(✅GP-4)

ポインタの位置とカーソルのモードは、アクションではなく [`ctx.input()`](../../src/core/userpublic/gamecontext.hpp#L362) が返す [`InputContext`](../../src/core/userpublic/gamecontext.hpp#L244) で扱います。ポインタをアクションにしないのは、axis2 のアクションが [-1, 1] に clamp され、同じアクションの binding どうしで足し合わされる(§7.2)ので、ピクセルの位置を運べないためです。

```cpp
const auto pointer = ctx.input().pointer();       // PointerState{position_px, inside}
if (pointer.inside) { /* pointer.position_px は framebuffer ピクセル */ }
ctx.input().setCursorMode(Pelican::CursorMode::locked);   // normal / hidden / locked
```

- **`pointer()`** は [`PointerState`](../../src/core/userpublic/userinput.hpp#L143) を返します。位置は framebuffer ピクセル(左上原点、+y が下)で、OS が報告する窓座標を軸ごとに `px = 窓座標 × framebuffer の大きさ ÷ 窓の大きさ` で換算したものです([`pointerTransformFor()`](../../src/core/os/inputstate.cpp#L56))。大きさが 0 の軸は 1 倍、headless は窓が無いので 1 倍で、framebuffer は出力先の大きさです。
- `inside` は位置が分かっていて(カーソルの event が来ている)、0 ≤ px < framebuffer の大きさのとき true です([`pointerStateFor()`](../../src/core/os/inputstate.cpp#L74))。位置は clamp しません。
- **`setCursorMode(mode)`** は `normal` / `hidden` / `locked`([`CursorMode`](../../src/core/userpublic/userinput.hpp#L151))。次のフレームで GLFW の窓に反映されます。埋め込みの窓(`--rpc` で起動し、Studio の viewport として `WS_CHILD` にされた窓)では `locked` を `normal` として扱います([`effectiveCursorMode()`](../../src/core/os/inputstate.cpp#L85))。状態は rpc `get_status` の `input.cursor {requested, effective, embedded}` で見えます。
- 換算するのは**ポインタと UI の入力ルータの 2 か所だけ**です。[`InputSnapshot`](../../src/core/os/inputstate.hpp#L61) と入力 event は窓座標のままで、ImGui も窓座標をそのまま読みます(GP-4 で変えていません)。
- 画面の点を通る ray(`ctx.render().screenRay`)と使用例、残した穴は [第8章 §8.4「input() と screenRay」](08_gameplay.md) にあります。

### 旧形式からの移行(2026-07-10 以前のプロジェクト)

1. `actions.json` の各アクションから `bindings` を削除する
2. `input/keyboard.json` 等の profile ファイルを作り、`{action, binding}` を移す
3. `project.json` に `"input_profiles": {"keyboard": "input/keyboard.json"}` と `"input_profile": "keyboard"` を追加する

## 7.3 入力注入・収録・リプレイ

### rpc からの入力注入(✅WP49/91)

`--headless --rpc` で起動し、`inject_input` → `step_frame` が自動テストの基本形です。ゲームパッドイベントも注入できます:

```json
{"jsonrpc":"2.0","id":3,"method":"inject_input","params":{"events":[
  {"type":"key_down","key":"w"},
  {"type":"mouse_move","x":100.0,"y":50.0},
  {"type":"pad_button_down","pad":0,"button":"a"},
  {"type":"pad_axis","pad":0,"axis":"left_x","value":0.75}
]}}
```

`mouse_move` の `x` / `y` は**窓座標**です(framebuffer ピクセルではありません)。注入した移動は OS からの移動と同じ event になり、`ctx.input().pointer()` と UI の入力ルータがフレームの換算で framebuffer ピクセルにします([注入の受け口](../../src/core/communication/rpcserver.cpp#L866))。headless は 1 倍なので、窓座標とピクセルは同じ値です。

### 収録とリプレイ(✅WP89 = I3)

順序付き入力イベント列を **`pelican.input_seq`(JSONL)** として収録・再生できます。収録は v2 で書き、再生は v2 と v1 を読みます(✅GP-4 で v2)。同一ビルド + プロジェクト + シードで**完全再現**(PNG byte 一致で結合テスト済み)。

```sh
pelican_player --project mygame --record-input session.jsonl   # 収録
pelican_player --project mygame --replay session.jsonl         # リプレイ(ホットリロード自動無効)
```

- ファイルはヘッダ行 `{schema, version, fps, generator, content_scale, framebuffer}` + `{"frame":N}` 境界マーカー + イベント行(`button` / `cursor_move` / `axis` / `scroll` / `character` / `pad_button` / `pad_axis`)。
- **v2 の換算の記録**: ヘッダの `content_scale: [x, y]` と `framebuffer: [幅, 高さ]` が、最初のフレームで窓座標を framebuffer ピクセルへ換算した値です。途中のフレームで換算が変わったら(窓の大きさを変えた、別の倍率の画面へ移したなど)、そのフレームのマーカーにも同じ 2 つを書きます([`toJsonLines()`](../../src/core/os/inputsequence.cpp#L373))。再生はこの記録の換算を使うので、倍率の違う機械で再生しても、`pointer()` と UI が見るピクセルは収録時と同じです([`prepareFrame()`](../../src/core/os/inputsequence.cpp#L487))。
- **v1 の読み方**: v1 には換算の記録が無いので、全フレームを content scale 1 として読みます(v1 の窓座標は書かれた当時その意味でした)。framebuffer は記録に無いので、再生している機械のものを使います。v1 のマーカーに `content_scale` / `framebuffer` があれば「version 2 が要る」として拒否し、version が 1 と 2 以外なら拒否します([版の検査](../../src/core/os/inputsequence.cpp#L308))。v2 のヘッダでは 2 つとも必須です。
- 収録ファイルは `--record-input` / `start_input_record` で指定した場所に書く JSONL で、project の文書ではありません。そのため MG-1 の移行(第3章 §3.13)の対象ではなく、v1 → v2 の移行手順は登録していません。v1 のファイルはそのまま再生できます。
- rpc からは `start_input_record` / `stop_input_record` / `start_input_replay` / `stop_input_replay`。リプレイ中の `inject_input` は拒否されます。
- **カメラ焼き出し**: `pelican_cli bake-camera --replay session.jsonl --project mygame` で、リプレイ中のカメラ軌跡を `pelican.transform_seq` v1 として `imports/pelican-camera/<name>/` に着地させられます(pelican.import manifest 付き — DCC への逆輸出)。

## 7.4 UI — pelican.ui v1(✅WP75/87/93)

> ⚠ **破壊的変更(WP87):** 旧 `ui_overlay.json` の `images` 配列形式は**削除されました**(パースエラーになります)。現行の UI は `pelican.ui` v1 ドキュメントです。ファイル名は従来どおり `basic_config.ui_config_json` で指します。

2026-07-10 時点で「レビュー Reject 中・実装ゼロ」(= 設計レビューで差し戻され、コードが 1 行も無い状態。レビュー記録は [../design_reviews/](../design_reviews/))だった `pelican.ui` は、レビュー条件を折り込んだ v8 設計で承認され、U0(CPU 基盤)→ U1(GPU 描画)→ U2(対話ウィジェット)まで実装済みです。

### 有効化(2 段)

1. rendering config の features に `"engine://features/ui.json"` を追加(参照しなければ UI モジュールごと生成されない — パージ可能)
2. `project.json` の `basic_config.ui_config_json` に UI ドキュメント JSON を指定

### ドキュメント形式

実物([../../projects/example/ui/ui_overlay.json](../../projects/example/ui/ui_overlay.json) — 中身は新形式、抜粋):

```json
{
  "schema": "pelican.ui",
  "version": 1,
  "key": "example_overlay",
  "revision": "wp87-u1",
  "direction": "ltr",
  "root": {
    "id": "root",
    "type": "panel",
    "children": [
      {
        "id": "ui_test",
        "type": "image",
        "sprite": "assets/textures/ui.atlas.json#sprite/ui_test",
        "layer": 1,
        "layout": {
          "x": { "mode": "fixed", "value": 1925 },
          "y": { "mode": "fixed", "value": 1085 },
          "anchor_min": [0.5, 0.5],
          "anchor_max": [0.5, 0.5],
          "offsets": [-962, -542, 963, 543]
        }
      }
    ]
  }
}
```

- **機械可読スキーマが正**: [../schemas/pelican.ui.schema.json](../schemas/pelican.ui.schema.json)(`additionalProperties: false` の closed schema — 未知フィールドは拒否)。
- トップレベル: `schema` / `version: 1` / `key`(1〜256 文字)/ `root` 必須、`revision` / `direction`(`"ltr"` のみ)任意。
- **ウィジェット語彙 6 種**(語彙 = 書ける `type` の値の全集合。これ以外は拒否): `panel` / `image` / `label` / `button` / `gauge` / `stack`。
- **`gauge` の描画(✅GP-3)**: 左端から伸びる横棒を 1 本描きます。棒の幅は **floor(ゲージの幅 × clamp(`value`, 0, 1))**(UI 単位)で、色は `color`。`color` を省くと既定色 `{96, 200, 120, 255}` になります([visitNodes() の gauge 分岐](../../src/core/ui/module.cpp#L106) / [既定色](../../src/core/ui/document.cpp#L243))。`value` は文書に書けるほか、実行時に `ctx.ui().setValue` と `ui.value` sink で書き換えられます(下の「ゲームコードと共通グラフから UI を書く」)。背景の枠は描かないので、地が要るなら親の `panel` に色を付けます。
- 主なプロパティ: 共通 `id`(`^[a-z0-9_]+$`)/ `layout` / `visibility` / `enabled` / `hit_testable` / `layer` / `emit` / `children`。描画系 `sprite`(atlas 参照は `<name>.atlas.json#sprite/<名前>` 形式)/ `color` / `hover_color` / `pressed_color` / `text` / `text_color` / `nine_patch`(panel)。
- レイアウト: 軸ごとの `mode: fixed|content|fill`(+ min/max/weight)、`anchor_min/max`、`offsets`、`stack`(horizontal/vertical)+ `gap` / `padding` / `align` / `justify`。計算は決定的(binary64 固定)。
  - `mode` は x 軸・y 軸それぞれに書き、その軸の長さの決め方を選びます: **`fixed`** = 書いた `value` をそのまま使う / **`content`** = 中身(子とテキスト・画像)から算出した固有サイズを使う / **`fill`** = 親 `stack` の余りを `weight` の比で分け合う。いずれも `min` / `max` で挟まれます([axisSize()](../../src/core/ui/layout.cpp#L76) と [distribute()](../../src/core/ui/layout.cpp#L93))。`content` の親の下に `fill` の子を置くと循環なので名指しエラーです。
- 描画: レイヤー + 宣言順の**ペインターズソート**(画家が絵を塗り重ねるように、後に描いたものが上に来る方式。深さの比較をしないので Z バッファ不使用 —— 前後関係は `layer` と JSON の宣言順だけで決まります)、20B 頂点の quad バッチ、nine-patch は 3×3 展開、テキストは debug_text と同じ埋め込みビットマップフォント。atlas は `pelican.atlas` v1([第5章](05_assets.md))。

### インタラクション(✅U2 = WP93)

> **設計決定(UI はコールバックを持たない):** button 等の `emit` は**イベントを発行する**だけで、配送は次フレーム頭・決定的順序(E1 イベント層 — [第8章](08_gameplay.md))。payload はロード時に EventPayloadSchema(WP71)で検証される。

```json
{ "id": "start_button", "type": "button", "text": "START",
  "emit": { "on_click": { "event": "MenuStart" } } }
```

emit のソースには static 値のほか `stable_id` / `drag_delta_ui` / `widget_value` が使えます。それぞれ、イベント payload のそのフィールドに **`stable_id`** = 発火したウィジェットの安定 ID(親からの `親id/子id` パス)、**`drag_delta_ui`** = 直前の位置からのドラッグ移動量 `[x, y]`(UI 座標)、**`widget_value(パス)`** = ウィジェット自身の現在値、を入れます([document.cpp](../../src/core/ui/document.cpp#L196) / [module.cpp](../../src/core/ui/module.cpp#L241))。rpc 経由のクリックとリプレイの決定性は結合テストで担保されています。

### ゲームコードと共通グラフから UI を書く(✅GP-3)

UI の中身を実行時に変える口は 2 つあります。ゲームコードの **`ctx.ui()`** と、共通グラフの **`ui.*` sink** です。どちらも「UI 文書のどの node の、どの性質か」を名指しして書きます。

#### `ctx.ui()` — ゲームコードから書く

[`GameContext::ui()`](../../src/core/userpublic/gamecontext.hpp#L364) はプロジェクトの UI 文書を、`ui(document_key)` は文書の `key` を名指しして [`UiContext`](../../src/core/userpublic/gamecontext.hpp#L271) を返します。v0 の UI 文書は 1 本なので、`key` が違えば `ui.node_unknown@1` です。

```cpp
auto ui = ctx.ui();
if (auto r = ui.setText("root/hud/score", "SCORE 120"); !r)
    ctx.logWarning(r.errorName());                // 例: "ui.node_unknown@1"
(void)ui.setValue("root/hud/hp", 0.35f);          // gauge の棒が幅の 35% になる
(void)ui.setVisible("root/hud/pause_hint", false);
```

- **path** は root から自分までの `id` を `/` でつないだ文字列で、root の `id` も含みます(root `root` の子 `hud` の子 `score` なら `root/hud/score`)。`id` は兄弟の間でしか一意ではないので、node を区別するのは path です。emit の `stable_id` に入る文字列と同じです([path の組み立て](../../src/core/ui/document.cpp#L234))。
- 書ける性質は node の型で決まります。`setText` は `label` / `button`、`setValue` は `gauge`、`setVisible` はすべての node です([uiNodeHasProperty()](../../src/core/ui/module.cpp#L301))。
- 戻り値は [`UiWriteResult`](../../src/core/userpublic/ui_write.hpp#L39)(`[[nodiscard]]`)です。`ok()` / `operator bool` / `error()` / `errorName()` を持ち、失敗は例外ではなく戻り値で返ります。理由は [`UiWriteError`](../../src/core/userpublic/ui_write.hpp#L10) の 4 つで、複数当てはまるときは**この順で先のもの**を返します。

| 順 | 名前 | いつ |
|---|---|---|
| 1 | `ui.feature_unreferenced@1` | rendering config が `engine://features/ui.json` を参照していない(UI が無いプロジェクト) |
| 2 | `ui.node_unknown@1` | その path の node が無い、または `document_key` が文書の `key` と違う |
| 3 | `ui.property_mismatch@1` | node の型がその性質を持たない(例: gauge への `setText`)、または `setValue` の値が有限でない |
| 4 | `ui.sink_conflict@1` | 公開中の rendering config の `ui.*` sink が同じ (path, 性質) を書いている(下記) |

- **書き込みは呼んだ時点で検査され、フレーム境界で適用されます。** 受け付けた書き込みは列に積まれ、そのフレームのゲームコードがすべて走ったあと、描画の前に一度にまとめて入ります([commitFrameWrites() の呼び出し](../../src/core/appflow/framephase.cpp#L269))。断られた書き込みは何も積みません。
- `setText` で文字列を変えると、`label` / `button` の大きさ(`content` 軸の固有サイズ)を測り直します。測り直すのは固有サイズが文字列から測られた node だけで、`intrinsic_size` を文書に書いた node はそのままです([syncDocumentFromArena()](../../src/core/ui/module.cpp#L351))。
- **scene load 中に書いても、load の transaction には入りません。** 公開 API の分類は `nonparticipant.ui-document` で([manifest の行](../../src/core/loader/sceneparticipantmanifest.cpp#L228))、UI 文書は scene の参加者ではないので、load が失敗しても書き込みは次のフレーム境界で入ります。

#### `ui.*` sink — 共通グラフから書く

graph module **`pelican.ui`**(version 1)は常に登録されます(UI にはビルドユニットの ON / OFF が無いため。[登録](../../src/project/graphmodule.cpp#L772))。持ち込む op は sink 2 本です。

| sink | 入力 | 書く先 |
|---|---|---|
| `ui.value` | `value`(スカラー) | `parameters.node` の path にある gauge の値 |
| `ui.visible` | `value`(スカラー) | `parameters.node` の path にある node の表示。**0 以外で表示**、0 で非表示 |

時刻で gauge を満たす例です(rendering config の `nodes` に置きます。形は GP-3 の結合テスト [run_gp3_ui_headless.cmake](../../test/run_gp3_ui_headless.cmake#L32) と同じです):

```json
"nodes": [
  {"id": "clock", "op": "value.input", "parameters": {"name": "time"}, "inputs": {}, "outputs": ["value"]},
  {"id": "fill", "op": "value.curve",
   "parameters": {"keys": [{"time": 0, "value": 0}, {"time": 1, "value": 1}]},
   "inputs": {"time": {"node": "clock", "port": "value"}}, "outputs": ["value"]},
  {"id": "meter_sink", "op": "ui.value", "parameters": {"node": "root/hud/meter"},
   "inputs": {"value": {"node": "fill", "port": "value"}}, "outputs": []}
]
```

- sink の値は毎フレーム、グラフの publish の時点で UI へ直接入り、そのフレームの UI パスが描きます([renderer.cpp の ui_writer](../../src/core/vkcore/renderer.cpp#L6336))。
- **単一 writer の単位は `(文書, path, 性質)`** です。`ui.*` の sink は owner に属さないので、`pose.*` の「owner 単位」とは単位が違います。同じ node でも `value` と `visible` は別の単位です。
- rendering config を準備する時点で、次の理由で名前付きに断ります(準備で断られた config はどのフレームにも届きません。[validateUiSinks()](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L112)、衝突は [Graph の binding 検査](../../src/core/userpublic/valuegraph.cpp#L643)):
  - `ui.feature_unreferenced@1` — `ui.*` sink を持つ config が `engine://features/ui.json` を参照していない
  - `ui.node_unknown@1` / `ui.property_mismatch@1` — `parameters.node` の path が無い / その node が性質を持たない(例: `ui.value` の先が label)
  - `ui.sink_conflict@1` — 2 本の sink が同じ (path, 性質) を書く
- **sink とゲームコードがぶつかったら sink が勝ちます。** sink が書いている (path, 性質) へ `ctx.ui()` で書くと、その書き込みが `ui.sink_conflict@1` で断られます([uiSinkClaims()](../../src/core/userpublic/gamecontext.cpp#L193))。同じ node の別の性質(sink が `value` を書いている gauge への `setVisible` など)は受け付けます。
- UI feature を参照していないプロジェクトは、ゲームコードの経路でも sink の経路でも `ui.feature_unreferenced@1` になります。

#### GP-3 が残した穴

- `setVisible(false)`(と `ui.visible` の 0)で消えるのは**その node 自身の quad だけ**で、子は描かれ続けます(描画の走査が子へ無条件に降りるため。[visitNodes() の子への再帰](../../src/core/ui/module.cpp#L136))。子ごと隠したいときは子にもそれぞれ書きます。
- 文書で `visibility: "collapsed"` にした node は、`setVisible(true)` でも描かれません。collapsed の node はレイアウトに載らず([place()](../../src/core/ui/layout.cpp#L142))、描画はレイアウトの無い node を飛ばします。
- 衝突の検出は**公開済み**の rendering config の sink を見ます。最初の publish より前の `ctx.ui()` の書き込みでは、sink との衝突を検出しません(公開中の世代が無ければ「sink は無い」と答えるため)。

### ポインタの座標(✅GP-4)

UI の入力ルータは、カーソル event の窓座標を、ポインタ(§7.2「ポインタとカーソル」)と同じフレームの換算で framebuffer ピクセルにしてから hit 判定します([`routeFrameInput()` の換算](../../src/core/ui/inputrouter.cpp#L122))。GP-4 より前は窓座標をそのままピクセルとして扱っていた(表示倍率 1 を仮定していた)ので、倍率が 1 でない画面ではクリックの位置と widget の位置が食い違っていたはずです(差分からの推論。旧挙動の実行では未確認)。リプレイでは記録の換算を使うので、倍率の違う機械で収録したものも同じピクセルで判定されます。ImGui は窓座標のまま読むので変わっていません。

### 残っている未実装(明示)

- 🚧 UI のスケール(UI 単位 → ピクセルの `ui_scale`。配管はあるが常に 1.0。上のポインタの換算とは別のもの)
- 📐 エンジン標準スキン(現状は色をノードのプロパティで直指定)/ `PELICAN_REGISTER_WIDGET` とツールウィジェットパック / UI のホットリロード(U3)

## 7.5 テキスト HUD — debug_text(✅WP54 / WP169)

使い方は不変です — rendering config に `engine://features/debug_text.json` を 1 行 + ゲームコードから `ctx.render().debugText(x, y, text)`。座標はピクセル・左上原点、feature を参照しなければ完全 no-op、golden は tolerance 0 です。API は C-8 の facade 分割で `GameContext` から [`RenderContext::debugText`](../../src/core/userpublic/gamecontext.hpp#L160) へ移りました。`color`(既定 白 `{1, 1, 1, 1}`)と `scale`(既定 1)は引数で渡せ、省略したときだけ白・等倍です。

✅WP169 で、`debug_text` と `pelican.ui` の**ビットマップレイアウトが単一実装に統一**されました。以前は「フォントテーブルだけ共有・配置計算は別実装」でしたが、現在はどちらも [src/core/ui/bitmapfont.cpp](../../src/core/ui/bitmapfont.cpp) の `BitmapFont::layout()` を通ります。**見た目・座標・API は不変**で(既存 golden と凍結ハッシュはすべて維持、golden 更新 0)、production の `DebugText` と `UiRenderer` を同一オフスクリーン RT に描いた 96×80 RGBA8 の全 30,720 byte が一致することを `test/debugtext_ui_compat_test.cpp`(GPU ラベル付きの Catch2 テスト)がゲートしています。

統一された文字レイアウトの規則(`pelican.ui` の label / button も同じ):

| 入力 | 挙動 |
|---|---|
| ASCII 32〜126 | そのまま描画 |
| 範囲外のコード | `'?'` に置換 |
| `\r` | 無視 |
| `\n` | 改行(行送り = セル高 × scale) |
| `\t` | 水平送り = セル幅 × scale × 4 |
| scale | 1〜64 に clamp(`DebugText` 側も `BitmapFont` 側も上限 64) |

> ⚠ **新しい失敗(WP169):** 座標 × scale の結果が int32 を超えると `std::overflow_error("bitmap text layout exceeds int32")` になります。旧実装は黙って回り込んでいました。極端な座標を渡す可能性があるなら呼び出し側で丸めてください。

GPU 経路自体は意図的に別のままです(`DebugText` は専用 atlas + SSBO・フレームバッファ四辺へのクリップ、`pelican.ui` は `AtlasAssetResource` + quad バッチ・ウィジェット clip の scissor)。SDF・日本語は 📐(backlog)。

## 7.6 実装状況ダッシュボード

| 機能 | 状態 |
|---|---|
| 入力 L1(順序付き FrameInput + 消費マスク) | ✅ WP37/69 |
| アクション層 + プロファイル分離 | ✅ WP39/91 |
| rpc inject_input(ゲームパッド含む) | ✅ WP49/91 |
| 収録・リプレイ(I3、input_seq + bake-camera) | ✅ WP89(GP-4 で v2 = 換算の記録。v1 も読む) |
| ゲームパッド(I4) | ✅ WP91 |
| OpenXR 入力(Touch action set + pose) | ✅ WP130/132(v1 は Touch のみ・pose の収録は非対応) |
| ポインタの位置(framebuffer ピクセル)/ カーソルのモード(I4 残件のロックを含む)/ `screenRay` | ✅ GP-4(埋め込みの窓ではロックしない。残した穴は第8章 §8.4) |
| UI の入力ルータの表示倍率対応 | ✅ GP-4 |
| pelican.ui v1(U0/U1/U2) | ✅ WP75/87/93(スキン・`ui_scale` は 🚧/📐) |
| gauge 描画 / ゲーム→UI 更新 API(`ctx.ui()`)/ `ui.value`・`ui.visible` sink | ✅ GP-3(子を連れて隠せない等の穴は §7.4「GP-3 が残した穴」) |
| ui_overlay.json(旧 images 形式) | ❌ 削除済み(WP87) |
| イベント payload スキーマ | ✅ WP71 |
| UI / 入力設定のホットリロード(U3 / HR2-I) | 📐(backlog Tier 1) |
| debug_text HUD | ✅ WP54 / WP169(`pelican.ui` とレイアウト計算を統一・互換ゲート付き) |
| SDF / 日本語テキスト | 📐 |

オーディオ(`ctx.audio().playSound` 等)は [第8章](08_gameplay.md) を参照してください。

### 機能を外した build での呼び出し(✅PG-2)

この章のゲームコード向け API(アクション・pose・`debugText`・`ui()`)は、ビルドユニット([第2章](02_getting_started.md) の「機能ユニット」)を外しても答え方が変わりません。これらの実装(`userinput.cpp` と `gamecontext.cpp` の該当関数)には `PELICAN_WITH_*` による分岐がありません。外した build で答え方が変わるのは音と物理とアニメーションで、その一覧は [第8章 §8.4「機能を外した build」](08_gameplay.md) にあります。

| 呼び出し | 外した build での答え |
|---|---|
| `ctx.actionPressed` / `actionReleased` / `actionHeld` / `actionAxis1` / `actionAxis2` | 変わらない(入力のアクション層はビルドユニットではない) |
| `ctx.input().pointer` / `setCursorMode`、`ctx.render().screenRay` | 変わらない(GP-4。どれもビルドユニットではない) |
| `ctx.actionPose` | 分岐は無い。`PELICAN_WITH_OPENXR=OFF` の build では XR が起動しないので、flat 起動と同じく `valid = false` の既定値が返る(throw しない。§8.4 の actionPose) |
| `ctx.render().debugText` | 変わらない。`debug_text` はビルドユニットではなく rendering config の feature で、参照していなければ no-op(§7.5) |
| `ctx.ui().setText` / `setVisible` / `setValue` | 変わらない。UI はビルドユニットではなく rendering config の feature で、参照していなければ例外ではなく `ui.feature_unreferenced@1` が返る(§7.4) |
| `ctx.audio().playSound` など | `PELICAN_WITH_AUDIO=OFF` で `BuildFeatureCallDisabledError`(`code()` = `build_feature_disabled@1`)。詳細は第8章 |
| `--rpc`(`inject_input` / 収録・リプレイの rpc) | ゲームコードの呼び出しではないが、`PELICAN_WITH_RPC=OFF` の build では起動時に [`runEngineRpcServer()` の stub](../../src/core/communication/rpcserver_stub.cpp#L8) が `BuildFeatureDisabledError`(`This binary was built with PELICAN_WITH_RPC=OFF: --rpc is unavailable`)を投げる。こちらは PG-2 の派生型ではなく基底型で、`code()` は無い |

## 関連文書

- [../design_input_actions.md](../design_input_actions.md) — 入力四層・アクション層(I1〜I4 実装済み)
- [../design_ui_2d_foundation.md](../design_ui_2d_foundation.md) — UI 2D 基盤(v8 承認・U0〜U2 実装済み)
- [../schemas/pelican.ui.schema.json](../schemas/pelican.ui.schema.json) — pelican.ui の機械可読スキーマ(正)
- [../design_event_payload_schema.md](../design_event_payload_schema.md) — イベント payload スキーマ(WP71)
- [../design_text_hud.md](../design_text_hud.md) — debug_text(実装済み)
- [第6章 レンダリング](06_rendering.md) / [第8章 ゲームロジック](08_gameplay.md) / [第10章 ツールリファレンス](10_tools.md)
