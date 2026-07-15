# 第7章 入力と UI

対象: pelican2(2026-07-10 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- 入力システムの四層構造と、`actions.json` の正確なスキーマ
- アクションセットのスタックと「上のセットが入力を消費する」規則
- rpc からの入力注入(自動テストの書き方)
- 現行 UI(`ui_overlay.json` 画像オーバーレイ)の仕様と、設計中の `pelican.ui` の現状
- テキスト HUD(`debug_text`)の使い方

## 7.1 入力システムの全体像 — 四層構造

> **設計決定(四層):** 入力は L0(デバイスバックエンド)→ L1(フレーム同期スナップショット)→ L2(アクション層)→ L3(消費者)の四層。L0 を差し替えても L2 から上は変わらない。アクション層を最初から入れる決め手は **OpenXR がネイティブに action ベース**であること(将来の XR 対応で二重構造を作らないため)。

| 層 | 内容 | 状態 |
|---|---|---|
| L0 | GLFW(キーボード+マウス)、rpc 注入 | ✅(ゲームパッド・OpenXR・リプレイは 📐) |
| L1 | フレーム同期スナップショット(WP37) | ✅ |
| L2 | アクション層 = `actions.json`(WP39) | ✅ |
| L3 | `Actions` / `GameContext`(WP43/45) | ✅(収録・リプレイ I3 は 📐) |

L1 の要点: 入力はフレーム頭のスナップショットで**凍結**され、フレーム内で不変です(決定性の柱)。同一フレーム内の「押して離す」は pressed と released の両方が立ちます(フレーム内の順序・途中座標は保持されません)。

## 7.2 actions.json のスキーマ

有効化は `project.json` の `basic_config.input_actions_json`(**任意キー**)。未指定ならアクション層は無効で、`ctx.actionsConfigured()` が false を返します。

実物その 1([../../projects/example/input/actions.json](../../projects/example/input/actions.json) 全文):

```json
{
  "schema": "pelican.input_actions",
  "version": 1,
  "action_sets": [
    {
      "name": "gameplay",
      "actions": [
        { "name": "move", "type": "axis2", "bindings": ["kbd:wasd"] }
      ]
    }
  ]
}
```

実物その 2(テストフィクスチャ `test/fixtures/input_actions/valid/gameplay_menu.json` — 複数セットの例):

```json
{
  "schema": "pelican.input_actions",
  "version": 1,
  "action_sets": [
    {
      "name": "gameplay",
      "actions": [
        {"name": "move", "type": "axis2", "bindings": ["kbd:wasd", "pad:left_stick"]},
        {"name": "jump", "type": "button", "bindings": ["kbd:space", "pad:a"]},
        {"name": "look_x", "type": "axis1", "bindings": ["mouse:delta_x"]},
        {"name": "fire", "type": "button", "bindings": ["mouse:left"]}
      ]
    },
    {
      "name": "menu",
      "actions": [
        {"name": "confirm", "type": "button", "bindings": ["kbd:space"]},
        {"name": "navigate", "type": "axis2", "bindings": ["kbd:arrows"]}
      ]
    }
  ]
}
```

### 規則(パーサ実装 `src/core/os/actionmap.cpp` が正)

| フィールド | 規則 |
|---|---|
| `schema` / `version` | `"pelican.input_actions"` / 整数 `1` 必須 |
| `action_sets[].name` | `[a-zA-Z0-9_]`、セット名の重複はエラー |
| `actions[].name` | `[a-zA-Z0-9_]`。**全セット横断でグローバル一意**(同名アクションを 2 つのセットに置けない。※設計文書のセット別名前空間の示唆とは異なり、実装が正) |
| `actions[].type` | `button` / `axis1` / `axis2` / `pose` の 4 種 |
| `actions[].bindings` | 文字列配列(空配列は可) |

v1 に任意フィールドはありません(デッドゾーン等の processing・ユーザーリバインドは v2 予約 📐)。

### binding 記法 `device:control`

| 書き方 | 使える type | 意味 |
|---|---|---|
| `kbd:a` 〜 `kbd:z`, `kbd:0-9`, `kbd:space`, `kbd:enter`, `kbd:escape`, `kbd:tab`, `kbd:backspace`, `kbd:left_shift` 等, `kbd:up` 等(矢印), `kbd:f1`-`f12`, `kbd:num0`-`num9` | button / axis1 | 単キー。axis1 では held ? 1.0 : 0.0 |
| `kbd:wasd`, `kbd:arrows` | **axis2 専用** | 組み込み合成。x = 右−左、y = 上−下、[-1,1] クランプ |
| `mouse:left` / `right` / `middle` / `button4`-`button8` | button / axis1 | マウスボタン |
| `mouse:delta_x` / `delta_y` | **axis1 専用** | フレーム内のマウス移動量。※delta≠0 の間は pressed/held も true になる(未文書化の実装挙動) |
| `pad:*` / `xr:*` | 全 type | **受理のみ・常に無反応**(ゲームパッドは I4、OpenXR は将来 📐)。今書いておいても害はない |

`pose` type は型のみ予約で、`ctx.actionPose()` は常に例外を投げます。記号キー・独立テンキーコードは存在しません。

### アクションセットのスタックと消費

- 起動時、**最初の action_set 1 個**が自動でスタックに積まれます。
- スタックの上のセットが「読んだ」キー・軸は、下のセットから**見えなくなります**(コンテキスト優先。メニューを開いたら gameplay に space が届かない、を作る仕組み)。同一セット内のアクション同士は消費し合いません。
- スタック操作は静的 API で行います(`GameContext` には未公開):

```cpp
Pelican::Actions::pushActionSet("menu");   // menu が最上位に
Pelican::Actions::popActionSet();
Pelican::Actions::setActionSetStack({"gameplay", "menu"});
```

### ゲームコードからの読み方

```cpp
void update(Pelican::GameContext &ctx) {
    if (!ctx.actionsConfigured()) return;          // actions.json 無しでも壊れない作法
    const auto move = ctx.actionAxis2("move");     // {x, y}
    if (ctx.actionPressed("jump")) { /* ... */ }
    const float look = ctx.actionAxis1("look_x");
}
```

未知のアクション名・type 不一致(button に `actionAxis2` 等)は名前入りの例外です。低レベル API(`UserInput::getKey` などの KeyCode 直読み)もアクション層と並存して公開されています。

## 7.3 rpc からの入力注入 — ゲームの自動テスト(✅WP49)

`--headless --rpc` で起動し、`inject_input` でイベントを注入 → `step_frame` で 1 フレーム進める、が自動テストの基本形です([第10章](10_tools.md) §10.3)。

```json
{"jsonrpc":"2.0","id":3,"method":"inject_input","params":{"events":[
  {"type":"key_down","key":"w"},
  {"type":"mouse_move","x":100.0,"y":50.0},
  {"type":"axis","axis":"mouse_delta_x","value":5.0}
]}}
```

- `key` / `button` の語彙は binding と同じ(大文字小文字不問)。未知の名前は名前入りの -32602 エラー。
- 注入したイベントは**次の `step_frame`** のスナップショットに反映され、Actions → GameContext まで通常経路で届きます。
- 完成した実例が `test/run_rpc_inject_input_headless.cmake` にあります(W 押下 → step_frame×12 → capture で PNG が変わり、2 回実行でビット一致することまで検証)。

📐未実装: 収録・リプレイ(`pelican.input_seq`、I3)。なお 2026-07-10 のレビューで「スナップショット列では UI のリプレイに情報が足りない(順序付きイベント記録への改訂が必要)」と指摘されており、設計自体が改訂待ちです。

## 7.4 UI 2D の現状

### いま動くもの: ui_overlay.json(✅・ただし廃止予定)

現行 UI は「スクリーンに画像を貼る」だけの旧式システムです。ウィジェット・入力処理・イベントはありません。

実物([../../projects/example/ui/ui_overlay.json](../../projects/example/ui/ui_overlay.json) 全文):

```json
{
  "images": [
    {
      "name": "ui_test",
      "file": "assets/textures/Frame84.png",
      "position": [0.5, 0.5, 0.1],
      "center": [0.5, 0.5],
      "scale": 0.6
    },
    {
      "name": "explosion",
      "file": "assets/textures/explosion06.png",
      "position": [0.8, 0.5, 0.2],
      "center": [0.5, 0.5],
      "scale": 1.0
    }
  ]
}
```

| フィールド | 既定 | 意味 |
|---|---|---|
| `file` | — | 画像パス(プロジェクト相対)。無い要素は黙ってスキップされる |
| `name` | file と同じ | 識別名 |
| `position` | [0,0,0] | **0〜1 正規化・左上原点**。第 3 成分(z)は現状**重なり順を制御しない** |
| `center` | [0.5,0.5] | 画像内アンカー |
| `scale` | 1.0 | 表示倍率 |

描画には rendering config に `type: "ui"` のパスが必要です([第6章](06_rendering.md))。既知の制限として、**複数画像の描画順は非決定的**(内部が unordered_map)で、この形式は新 UI 移行後に廃止される予定です。最小限の利用に留めてください。また、実行時に画像を動かす API は `GameContext` に公開されていません。

### 設計中: pelican.ui ウィジェットツリー(📐・レビュー Reject 中)

本格的な UI 基盤([../design_ui_2d_foundation.md](../design_ui_2d_foundation.md) v1 ドラフト)が設計されていますが、**2026-07-10 の敵対レビューで Reject** となり、スキーマは再版まで凍結扱いです([../design_reviews/2026-07-10_ui_2d_review_codex.md](../design_reviews/2026-07-10_ui_2d_review_codex.md))。実装はゼロです。方向性としての決定事項のみ紹介します:

> **設計決定(方向性・ユーザー決定済み):**
> - ウィジェット語彙 v1 は 6 種(panel / image / label / button / gauge / stack)。拡張は 3 段のラダー: ①既存ウィジェットの合成 → ②`PELICAN_REGISTER_WIDGET`(プロジェクト C++ 登録)→ ③ツールウィジェットパック(engine:// 同梱だが公開機構だけで実装 = 標準に特権なし)。
> - **UI はコールバックを持たず、イベントを emit する**(`on_click` → ゲームシステムの `onEvent` へ。フレーム境界配送・決定的順序に乗せる)。
> - **エンジン標準スキンはツール調フラット 1 種**(直角・1px 境界・高密度)。ゲーム的な装飾はプロジェクト側のスキンデータで行う(2026-07-10 決定)。
> - 座標系はピクセル・左上原点(debug_text と同一規約)。ソートはレイヤー+宣言順のペインターズ(Z バッファ不使用)。

レビューの主要な差し戻し理由(実装前に解決が必要): クアッドバッチャの GPU 契約が未定義 / 「UI が入力を最初に消費する」は現行 L1 スナップショットでは実装不能(順序付きイベントへの入力層改訂が必要)/ DPI 未決 / `PELICAN_REGISTER_WIDGET` の live オブジェクト所有モデル未定義 / アンカーだけではレイアウト計算が閉じない、など。**この節のスキーマ例をプロジェクトで書いても動きません。**

## 7.5 テキスト HUD — debug_text(✅WP54)

デバッグ数値・状態表示のためのビットマップ文字 HUD です。使い方は 2 ステップ:

1. rendering config に feature を 1 行追加:

```json
{ "features": ["engine://features/debug_text.json"], ... }
```

2. ゲームコードから毎フレーム呼ぶ(immediate 型 — 積んだ文字はそのフレームで消費):

```cpp
ctx.debugText(8, 8, "score: 1200\nhp: 35");
```

仕様:

- 座標は**ピクセル・左上原点**。`\n` = 改行、`\t` = 4 セル、ASCII 32〜126 のみ(範囲外は `?`)。
- フォントはエンジン埋め込みの等幅ビットマップ(既定セル 8×16)。
- **feature を参照していなければ完全に no-op**(呼んでも落ちない)。パージ可能原則の実例です。
- ゴールデンテストは tolerance 0(ビット完全一致)で保護されています。
- 制限: `GameContext::debugText` は白・等倍固定です(色・整数倍スケールは内部 API にのみ存在し、未公開)。SDF・日本語対応は v2 📐(新 UI 設計と同時に起草予定)。

用途の設計意図は「gpu_timing の数値表示・物理可視化のラベル・シナリオテストの状態表示」であり、ゲームの本番 UI ではありません(それは pelican.ui の領分)。

## 7.6 実装状況ダッシュボード

| 機能 | 状態 |
|---|---|
| 入力スナップショット L1 / アクション層 I1 / GameContext 統合 | ✅ WP37/39/43/45 |
| rpc inject_input(I2) | ✅ WP49 |
| 収録・リプレイ(I3) | 📐 設計のみ(改訂待ち) |
| ゲームパッド・カーソルロック(I4)/ OpenXR | 📐 未実装(binding は受理のみ) |
| 画像オーバーレイ ui_overlay.json | ✅(廃止予定) |
| pelican.ui ウィジェットツリー | 📐 設計のみ・レビュー Reject 中 |
| debug_text HUD | ✅ WP54 |
| SDF / 日本語テキスト | 📐 v2 予約 |

オーディオ(`ctx.playSound` 等)は [第8章](08_gameplay.md) §8.10 を参照してください。

## 関連文書

- [../design_input_actions.md](../design_input_actions.md) — 入力四層・アクション層(I1 実装済み、I2〜I4 の記述は将来形)
- [../design_ui_2d_foundation.md](../design_ui_2d_foundation.md) — UI 2D 基盤(v1 ドラフト・Reject 中)
- [../design_reviews/2026-07-10_ui_2d_review_codex.md](../design_reviews/2026-07-10_ui_2d_review_codex.md) — UI 設計の敵対レビュー(差し戻し条件 C1〜C12)
- [../design_text_hud.md](../design_text_hud.md) — debug_text(実装済み)
- [第6章 レンダリング](06_rendering.md) / [第8章 ゲームロジック](08_gameplay.md) / [第10章 ツールリファレンス](10_tools.md)
