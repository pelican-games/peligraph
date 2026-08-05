# 第7章 入力と UI

対象: pelican2(2026-07-21 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- 入力システムの四層構造と、**順序付き FrameInput**(WP69)
- `actions.json`(アクション定義)と `pelican.input_profile`(バインディング)の**分離**(WP91・破壊的変更)
- ゲームパッド対応と、収録・リプレイ(`pelican.input_seq`)
- 実装済みの UI 基盤 `pelican.ui` v1(ウィジェット・レイアウト・イベント emit)
- テキスト HUD(`debug_text`)の使い方

## 7.1 入力システムの全体像 — 四層構造

> **設計決定(四層):** 入力は L0(デバイスバックエンド)→ L1(フレーム同期)→ L2(アクション層)→ L3(消費者)の四層。L0 を差し替えても L2 から上は変わらない。アクション層を最初から入れる決め手は **OpenXR がネイティブに action ベース**であること。

| 層 | 内容 | 状態 |
|---|---|---|
| L0 | GLFW(キーボード+マウス+**ゲームパッド 16 スロット**)、**OpenXR(Touch コントローラ + pose)**、rpc 注入、リプレイ注入 | ✅(WP130/132 で XR も実装) |
| L1 | **順序付きイベント + スナップショット**(`FrameInput`、WP37→WP69) | ✅ |
| L2 | アクション層 = `actions.json` + `input_profile`(WP39/91) | ✅ |
| L3 | `Actions` / `GameContext` / **収録・リプレイ**(WP43/45/89) | ✅ |

L1 の要点(✅WP69 で改訂):

- フレーム先頭で入力が**凍結**され、フレーム内で不変(決定性の柱)。
- `FrameInput` は「順序付きイベント列(`ordered_events` — 全イベントに単調増加の `event_seq` が付き、途中座標・発生順が保持される)」と「集約スナップショット」の両方を持ちます。スナップショット側では従来どおり同一フレームの「押して離す」は pressed / released が両方立ちます。
- **入力消費マスク**: 消費の優先順は ImGui → `pelican.ui` → アクション層。UI がポインタを hit / capture したフレームは、そのポインタがアクション層から見えなくなります。
- ImGui が消費者に入るのは**通常ウィンドウ起動のときだけ**です。headless / rpc / リプレイ / golden に加え、✅WP138 以降は **XR session が active な間もエンジンは ImGui frame を begin しません**(XR グラフに `imgui` パスが無いため。論理フレーム境界でゲートが閉じたときは開始済みフレームを閉じてから外します)。この場合の消費順は `pelican.ui` → アクション層になります([第6章](06_rendering.md) §6.12)。
- フレーム位相は「イベント凍結 → 入力凍結(リプレイ注入→収録)→ アクション確定(ImGui → UI → Actions)→ イベント配送 → ゲーム更新」の固定順で、通常ループと rpc ループで同一です。

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

- binding 要素のフィールド: `action` / `binding` + 任意 `deadzone`(数値 [0,1)、pad 軸限定)、`invert_x`(bool、pad 軸なら axis1 / axis2 どちらでも可)/ `invert_y`(bool、axis2 限定)。反転はいずれも deadzone 適用後です。
- プロファイルの登録と既定は `project.json`: `basic_config.input_profiles`(名前 → ファイルの辞書)と `basic_config.input_profile`(既定名)。
- 切替は 3 通り: 起動引数 `--input-profile <name>` / rpc `set_input_profile` / ゲームコード `selectInputProfile`。筐体・環境差(アーケード筐体等)は**プロファイルを増やして**表現します。
- `--free-camera` はデバッグ用の例外ではなく、公開された runtime overlay です。明示した起動だけで埋め込み profile `pelican_free_camera`(WASD/矢印、左右 stick)を選び、プロジェクトの actions/profile ファイルは変更しません。`--input-profile` との同時指定は、どちらを選ぶか曖昧になるためエラーです。

### binding 記法 `device:control`

| 書き方 | 使える type | 意味 |
|---|---|---|
| `kbd:a`〜`z`, `0-9`, `space`, `enter`, `escape`, `tab`, `backspace`, `left_shift` 等, 矢印, `f1`-`f12`, `num0`-`num9` | button / axis1 | 単キー |
| `kbd:wasd`, `kbd:arrows` | axis2 専用 | 組み込み合成([-1,1]) |
| `mouse:left` / `right` / `middle` / `button4`-`button8` | button / axis1 | マウスボタン |
| `mouse:delta_x` / `delta_y` | axis1 専用 | フレーム内マウス移動量 |
| `pad:a, b, x, y, left_bumper, right_bumper, back, start, guide, left_thumb, right_thumb, dpad_up/right/down/left` | button / axis1 | ✅ ゲームパッドボタン 15 種 |
| `pad:left_x, left_y, right_x, right_y, left_trigger, right_trigger` | axis1 | ✅ パッド軸 6 種 |
| `pad:left_stick` / `pad:right_stick` | axis2 専用 | ✅ スティック合成(radial deadzone) |
| `xr:/user/hand/<left\|right>/input/...` | 全 type | ✅WP130。OpenXR 実パス形式(v1 は Touch コントローラのみ)。`xr:/user/hand/` 以外は名指しエラー。flat 起動では未解決として無視されるので、**同じ actions.json / profile が flat でも XR でも通る** |

- アクティブな profile に `pad:` binding が 1 つも無ければゲームパッドは**一切ポーリングされません**(パージ可能原則)。切断時は保持ボタンの release + 軸ゼロが発行されます。マッピングは GLFW 内蔵の SDL_GameControllerDB。
- `pose` type は ✅WP132 で実動作になりました(XR 起動時のみ供給 — [第8章](08_gameplay.md) の `actionPose`)。**pose を含む actions.json は `--record-input` / リプレイが名指しで拒否されます**(input_seq v1 の仕様)。カーソルロックは 📐未実装です。

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

### 収録とリプレイ(✅WP89 = I3)

順序付き入力イベント列を **`pelican.input_seq` v1(JSONL)** として収録・再生できます。同一ビルド + プロジェクト + シードで**完全再現**(PNG byte 一致で結合テスト済み)。

```sh
pelican_player --project mygame --record-input session.jsonl   # 収録
pelican_player --project mygame --replay session.jsonl         # リプレイ(ホットリロード自動無効)
```

- ファイルはヘッダ行 `{schema, version, fps, generator}` + `{"frame":N}` 境界マーカー + イベント行(`button` / `cursor_move` / `axis` / `scroll` / `character` / `pad_button` / `pad_axis`)。
- rpc からは `start_input_record` / `stop_input_record` / `start_input_replay` / `stop_input_replay`。リプレイ中の `inject_input` は拒否されます。
- **カメラ焼き出し**: `pelican_cli bake-camera --replay session.jsonl --project mygame` で、リプレイ中のカメラ軌跡を `pelican.transform_seq` v1 として `imports/pelican-camera/<name>/` に着地させられます(pelican.import manifest 付き — DCC への逆輸出)。

## 7.4 UI — pelican.ui v1(✅WP75/87/93)

> ⚠ **破壊的変更(WP87):** 旧 `ui_overlay.json` の `images` 配列形式は**削除されました**(パースエラーになります)。現行の UI は `pelican.ui` v1 ドキュメントです。ファイル名は従来どおり `basic_config.ui_config_json` で指します。

2026-07-10 時点で「レビュー Reject 中・実装ゼロ」だった `pelican.ui` は、レビュー条件を折り込んだ v8 設計で承認され、U0(CPU 基盤)→ U1(GPU 描画)→ U2(対話ウィジェット)まで実装済みです。

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
- **ウィジェット語彙 6 種**: `panel` / `image` / `label` / `button` / `gauge` / `stack`。ただし `gauge` は型・レイアウトのみで**描画は 🚧 未実装**。
- 主なプロパティ: 共通 `id`(`^[a-z0-9_]+$`)/ `layout` / `visibility` / `enabled` / `hit_testable` / `layer` / `emit` / `children`。描画系 `sprite`(atlas 参照は `<name>.atlas.json#sprite/<名前>` 形式)/ `color` / `hover_color` / `pressed_color` / `text` / `text_color` / `nine_patch`(panel)。
- レイアウト: 軸ごとの `mode: fixed|content|fill`(+ min/max/weight)、`anchor_min/max`、`offsets`、`stack`(horizontal/vertical)+ `gap` / `padding` / `align` / `justify`。計算は決定的(binary64 固定)。
- 描画: レイヤー + 宣言順のペインターズソート(Z バッファ不使用)、20B 頂点の quad バッチ、nine-patch は 3×3 展開、テキストは debug_text と同じ埋め込みビットマップフォント。atlas は `pelican.atlas` v1([第5章](05_assets.md))。

### インタラクション(✅U2 = WP93)

> **設計決定(UI はコールバックを持たない):** button 等の `emit` は**イベントを発行する**だけで、配送は次フレーム頭・決定的順序(E1 イベント層 — [第8章](08_gameplay.md))。payload はロード時に EventPayloadSchema(WP71)で検証される。

```json
{ "id": "start_button", "type": "button", "text": "START",
  "emit": { "on_click": { "event": "MenuStart" } } }
```

emit のソースには static 値のほか `stable_id` / `drag_delta_ui` / `widget_value` が使えます。rpc 経由のクリックとリプレイの決定性は結合テストで担保されています。

### 残っている未実装(明示)

- 🚧 `gauge` の描画 / ゲームコードから UI を書き換える公式 API(ラベル文字列・ゲージ値の更新など — UiCommandBuffer は内部のみ)/ DPI スケール(配管はあるが常に 1.0)
- 📐 エンジン標準スキン(現状は色をノードのプロパティで直指定)/ `PELICAN_REGISTER_WIDGET` とツールウィジェットパック / UI のホットリロード(U3)

## 7.5 テキスト HUD — debug_text(✅WP54 / WP169)

使い方は不変です — rendering config に `engine://features/debug_text.json` を 1 行 + ゲームコードから `ctx.debugText(x, y, text)`。座標はピクセル・左上原点、feature を参照しなければ完全 no-op、golden は tolerance 0 です。`GameContext::debugText` は**白・等倍固定**のままで、色や scale を渡す公開 API はありません。

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
| 収録・リプレイ(I3、input_seq v1 + bake-camera) | ✅ WP89 |
| ゲームパッド(I4) | ✅ WP91 |
| OpenXR 入力(Touch action set + pose) | ✅ WP130/132(v1 は Touch のみ・pose の収録は非対応) |
| カーソルロック(I4 残件) | 📐 未実装 |
| pelican.ui v1(U0/U1/U2) | ✅ WP75/87/93(gauge 描画・ゲーム→UI 更新 API・スキン・DPI は 🚧/📐) |
| ui_overlay.json(旧 images 形式) | ❌ 削除済み(WP87) |
| イベント payload スキーマ | ✅ WP71 |
| UI / 入力設定のホットリロード(U3 / HR2-I) | 📐(backlog Tier 1) |
| debug_text HUD | ✅ WP54 / WP169(`pelican.ui` とレイアウト計算を統一・互換ゲート付き) |
| SDF / 日本語テキスト | 📐 |

オーディオ(`ctx.playSound` 等)は [第8章](08_gameplay.md) を参照してください。

## 関連文書

- [../design_input_actions.md](../design_input_actions.md) — 入力四層・アクション層(I1〜I4 実装済み)
- [../design_ui_2d_foundation.md](../design_ui_2d_foundation.md) — UI 2D 基盤(v8 承認・U0〜U2 実装済み)
- [../schemas/pelican.ui.schema.json](../schemas/pelican.ui.schema.json) — pelican.ui の機械可読スキーマ(正)
- [../design_event_payload_schema.md](../design_event_payload_schema.md) — イベント payload スキーマ(WP71)
- [../design_text_hud.md](../design_text_hud.md) — debug_text(実装済み)
- [第6章 レンダリング](06_rendering.md) / [第8章 ゲームロジック](08_gameplay.md) / [第10章 ツールリファレンス](10_tools.md)
