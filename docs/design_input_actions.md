# 入力アーキテクチャ: アクション層・OpenXR 整合・収録からのデータ化

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-04。レビュー前)。
前提: WP37(フレーム同期スナップショット・実装済み)、WP27(コマンド層)、
R4(transform_seq)、WP21(import manifest)、`design_compute_task_graph.md`(将来の VR 描画)。

## 0. 要求(2026-07-04 ユーザー方針)

1. VR / OpenXR 対応を見据える
2. ゲーム制作時のテスト・プレビューに使える
3. **プレビューでの操作からゲーム用データを作れる**(収録 → アセット化)

## 1. 四層構造

```
L0 デバイスバックエンド   GLFW(kbd/mouse・済)/ ゲームパッド / OpenXR / rpc 注入 / リプレイファイル
        ↓ すべて同じイベントキューへ
L1 スナップショット       フレーム同期・直列化可能(WP37 実装済み)
        ↓
L2 アクション層           プロジェクトアセット(pelican.input_actions)。本設計の中核
        ↓
L3 消費者                 ゲームロジック / devstudio / 収録
```

要点: **L0 を差し替えても L2 から上は変わらない**。リプレイ・rpc 注入・OpenXR は
全部「もう一つのバックエンド」であり、テスト容易性と VR 対応が同じ仕組みから出る。

## 2. アクション層(L2)— 中核

生のキーではなく**意味(アクション)**をゲームに見せる。決め手は
**OpenXR がネイティブに action ベース**であること(XrAction / action set /
suggested bindings)。今アクション層を入れておくと、OpenXR 対応時に
概念の翻訳が不要になる。Steam Input も同構造。

### pelican.input_actions v1(プロジェクトアセット: `input/actions.json`)

```json
{
  "schema": "pelican.input_actions",
  "version": 1,
  "action_sets": [
    {
      "name": "gameplay",
      "actions": [
        {"name": "move",  "type": "axis2"},
        {"name": "jump",  "type": "button"},
        {"name": "aim",   "type": "pose"}
      ]
    },
    {"name": "menu", "actions": [ ... ]}
  ]
}
```

WP91 以降、アクション定義はバインディングを持たない。バインディングは交換可能な
`pelican.input_profile` v1(`input/profiles/*.json`)へ分離する。

```json
{
  "schema": "pelican.input_profile",
  "version": 1,
  "name": "gamepad",
  "bindings": [
    {"action": "move", "binding": "pad:left_stick", "deadzone": 0.2, "invert_y": true},
    {"action": "jump", "binding": "pad:a"}
  ]
}
```

`basic_config.input_profiles` は名前から profile 参照への辞書、
`basic_config.input_profile` は project 既定値。`--input-profile <name>` が起動時に
上書きし、RPC `set_input_profile {name}` が実行中に切り替える。筐体固有レイアウトは
コード変更ではなく profile の追加で表す。未知の pad button/axis は profile 名・
control 名を含むエラーとし、deadzone / `invert_x` / `invert_y` は pad axis binding の
属性とする。

- **action type**: `button` / `axis1` / `axis2` / `pose`(pose は OpenXR 用に
  v1 から型だけ予約 — 6DoF 位置姿勢。native 実装は OpenXR トラックで)
- **action set** = コンテキスト(gameplay / menu / vehicle)。スタックで
  優先順位を持ち、上のセットが消費した入力は下に流れない
- **binding 記法** `device:control` は profile v1 で kbd/mouse/pad を定義、`xr:` は予約
- processing(デッドゾーン・応答カーブ・tap/hold/連打/チョード)は
  binding 側の修飾として v2 で拡張(器だけ設計)
- **ユーザーリバインドはプロジェクトの外**: actions.json はアクション定義、
  profile は配布既定。ユーザー上書きも同じ profile 形式を user 設定へ置く
  (プロジェクト配布物を汚さない)
- ゲームロジック API は `actions.get("jump").pressed` 系のみ。
  **ロジック実行方式の設計(別文書)はこの API を前提にする**

## 3. OpenXR との整合(設計だけ先に)

- **入力**: アクション層がそのまま XrAction に写像される(§2 の決め手)。
  pose type・haptic 出力(将来 `outputs` として)を予約
- **描画は別トラック**(本書のスコープ外): ステレオ(multiview)、
  xrWaitFrame によるループ駆動(EngineTime と統合)、ランタイム提供
  スワップチェーン。フレームグラフに「view 数」概念を足す設計が必要 —
  compute/フレームグラフ設計の v3 課題として予約のみ
- 判断: **OpenXR は「入力の形を合わせておき、描画トラックは需要が確定してから」**。
  アクション層が入っていれば後付けのコストが最小になる

## 4. テスト・プレビュー・収録 → データ化

### 4.1 rpc 注入(テストの要)

コマンド層に `inject_input {events}` メソッドを追加(stage 2.5)。
ウィンドウなしで入力を流せる = **シナリオテスト**が成立:
NDJSON スクリプトで「入力 → step_frame → capture」を回し golden 比較。
決定性(スナップショット + EngineTime)により再現は完全。

### 4.2 収録・リプレイ

- **(2026-07-11 改訂 — UI 設計 v3 の要請)** 収録の記録単位は
  「L1 スナップショット列」ではなく **ordered InputEvent 列(event_seq +
  フレーム境界マーカー)**を `pelican.input_seq` v1(JSONL、ヘッダ
  schema/version/fps)として書き出す。スナップショットはリプレイ時に
  再構成する。理由: L1 はフレーム内のイベント順序・各イベント時点の座標を
  捨てるため、UI(press→move→release の区別、capture、ドラッグ)の再現には
  情報不足(`design_ui_2d_foundation.md` §2-1・v2 レビュー R2)。
  I3 未実装のため互換負債なし
- リプレイ = L0 のリプレイバックエンドが input_seq を流す。
  同一ビルド + 同一プロジェクト + 同一シード + 同一 UI document revision なら
  **完全再現**
- 用途: バグ再現・回帰テスト・デモ(アトラクトモード)・リプレイゴースト

### 4.3 プレビューからゲーム用データを作る(ユーザー要求 3)

収録セッションから**既存形式へ焼き出す**(新形式を発明しない):

| 収録対象 | 焼き出し先 | 用途 |
|---------|-----------|------|
| カメラの動き | **transform_seq**(R4・再生系実装済み) | カットシーン・映像用カメラパス |
| オブジェクト操作の軌跡 | transform_seq | リプレイゴースト・群衆の粗い動き |
| 入力そのもの | pelican.input_seq | デモ入力・チュートリアル・テスト |
| イベントマーカー(キー押下で打点) | JSONL(スキーマは需要時) | レベルスクリプトのタイミング出典 |

焼き出し結果は **`imports/` に pelican.import manifest 付きで着地**
(WP21 の import 経路をそのまま通す — エンジン内収録を「もう一つの DCC」として
扱う。source.file はプロジェクト自身 + セッション情報)。

## 5. 見落としがちな入力機能(チェックリスト)

| 機能 | 位置づけ |
|------|---------|
| マウスカーソルロック / 相対モード / raw input | FPS カメラに必須。L0(GLFW)小改修。**早めに要る** |
| フォーカス喪失処理(キー押しっぱなし解除) | 事故防止。L1 で全解放イベント。**早めに要る** |
| テキスト入力・IME | devstudio・チャット・名前入力。GLFW char/preedit。日本語は別格の重さ — devstudio 本格化まで保留 |
| ゲームパッド(複数・ホットプラグ・振動) | L0 バックエンド。振動 = 初の「出力」で haptics の器を兼ねる |
| デバイス → プレイヤー割当 | ローカルマルチ。アクション層にプレイヤー次元 |
| 入力バッファ・先行入力・tap/hold・コマンド入力 | アクション processing(v2)。アクションゲームの手触りの核 |
| リバインド UI + ユーザー設定保存 | §2 で分離設計済み。UI は devstudio/ゲーム側 |
| ドラッグ & ドロップ(glb をウィンドウへ) | DCC 的 UX。GLFW drop callback。プレビュー強化として安い |
| クリップボード | devstudio 用 |
| 入力可視化 HUD(debug_draw 流用) | 配信・デバッグ・リプレイ検証 |
| タッチ / ペン | web 側デモには関係(サブセット原則: アクション層の形式は web でも読める) |
| ネットコード向け決定性(rollback) | 遠い将来。決定性重視の現設計が既に有利、としてだけ記録 |

## 6. 実装順(WP 候補・登録時に採番)

| 段階 | 内容 | 依存 |
|------|------|------|
| I1 | アクション層 v1(actions.json パーサ純ロジック + kbd/mouse binding + set スタック + ゲーム API) | WP37 |
| I2 | rpc `inject_input` + シナリオテスト 1 本 | WP27, WP37 |
| I3 | 収録・リプレイ(input_seq)+ カメラ収録 → transform_seq 焼き出し + manifest 着地 | I1, I2, WP21 |
| I4 | ゲームパッド + カーソルロック + フォーカス処理 | I1 |
| XR | OpenXR トラック(入力 = アクション層写像、描画 = フレームグラフ v3 と同時設計) | I1 ほか・需要確定後 |

I1 が要: **ゲームロジック実行方式の設計は I1 の API を前提にする**ため、
ロジック設計文書より先(または同時)に固める。

## 7. 未決事項

1. binding 記法の詳細(`kbd:wasd` のような合成 binding をどこまで組み込みにするか)
2. pose アクションの座標系規約(glTF 規約 R6 と OpenXR reference space の写像)
3. input_seq とスナップショットのバージョン互換(KeyCode enum が増えたとき)
4. イベントマーカー焼き出しのスキーマ(需要が出てから)
