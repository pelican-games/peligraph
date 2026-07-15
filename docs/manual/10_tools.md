# 第10章 ツールリファレンス

対象: pelican2(2026-07-10 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- `pelican_player` の全 CLI 引数
- JSON-RPC(stdio)による外部制御 — 全 13 メソッドと実セッション例
- `pelican_cli` の 3 サブコマンド(`project init` / `import` / `dist-config`)
- 配布ビルド(ビルドユニットと dist-config)
- Pelican Studio の現状と設計方向、テスト基盤

## 10.1 実行物の一覧

| ターゲット | 種類 | 出力先 | 役割 |
|---|---|---|---|
| `pelican_player` | exe | build ツリー内(`build/src/player/<Config>/`) | ゲームランタイム。ウィンドウ / ヘッドレス / RPC の 3 モード |
| `pelican_cli` | exe | `dist/`(Debug は `dist_debug/`) | 開発 CLI。**エンジン(pelican_core)にリンクしない** |
| `pelican_studio` | exe(Qt6) | `dist/`(同上) | エディタ。現状はダミー画面のみ(§10.6) |
| `pelican_project` | 静的 lib | build 内 | 解釈レイヤ(JSON パース/検証の純ロジック) |
| `pelican_core` | 静的 lib | build 内+`dist/lib` | エンジン本体。公開ヘッダは `dist/lib/include` へコピー |

## 10.2 pelican_player CLI リファレンス

実装: [src/player/main.cpp](../../src/player/main.cpp)。引数エラー時は usage を表示して終了コード -1、正常終了 0、実行時致命エラー 1。

| 引数 | 既定 | 意味 |
|---|---|---|
| `--project <dir\|project.json>` | (暗黙探索) | プロジェクトを開く。省略時は exe ディレクトリ → 祖先の `projects/example` を探索し、WARN を出す |
| `--headless` | off | ウィンドウなし実行(時刻は固定ステップになる) |
| `--rpc` | off | stdio JSON-RPC モード。**v1 では `--headless` 必須**。`PELICAN_WITH_RPC=OFF` ビルドではエラー |
| `--frames <n>` | 3 | ヘッドレスのフレーム数。**0 = 無制限** |
| `--size WxH` | 1280x720 | ヘッドレスの描画サイズ |
| `--render-out <path>` | なし | PNG 出力先。パスに `%d` / `%04d` を含むと**毎フレーム連番**、含まなければ最終フレームのみ |
| `--fps <f>` | 60.0 | ヘッドレス固定ステップのレート(dt = 1/fps) |
| `--dump-frame-plan` | off | 解決済みフレームプラン JSON を **stderr** へ出力([第6章](06_rendering.md)) |
| `--allow-absolute-paths` | off | CLI 由来のコンテンツ参照に限り絶対パスを許可(使用のたび WARN)。JSON 内の絶対パスは常に拒否 |
| `--user-dir <dir>` | なし | `user://` ルートの差し替え。project.json に `name` が無いとエラー |
| `--ignore-engine-version` | off | `engine_min_version` 不適合を hard error → WARN に降格 |
| `--play-seq <path.jsonl>` | なし | `pelican.transform_seq` の再生(SEQPLAYER ユニット必須) |
| `--seq-mesh <builtin:sphere\|path.glb>` | builtin:sphere | transform_seq 全オブジェクトに使うメッシュ |
| `--seq-loop` | off | クリップをループ再生 |
| `--play-vat <path.glb>` | なし | `pelican.vat` 入り GLB の再生(VAT ユニット必須)。プロジェクト相対で解決 |
| `--camera "px,py,pz,tx,ty,tz,fov_deg"` | なし | カメラの位置・注視点・垂直 FOV(度)の上書き |

よく使う組み合わせ:

```sh
# 通常起動
pelican_player --project projects/example

# ヘッドレスで 3 フレーム描いて PNG を保存(golden テストの型)
pelican_player --headless --project projects/example --frames 3 --render-out out.png

# 毎フレーム連番
pelican_player --headless --project mygame --frames 60 --render-out out/%04d.png

# 外部制御(エージェント・DCC ブリッジ・エディタの型)
pelican_player --rpc --headless --project mygame --size 1280x720
```

※ `--play-seq` / `--seq-mesh` の相対パスは現状 cwd 基準で解決されます(設計はプロジェクトルート基準 — 既知の食い違い。[第11章](11_status.md))。

## 10.3 JSON-RPC(stdio)— 外部制御プロトコル(✅実装済み)

実装: [rpcserver.cpp](../../src/core/communication/rpcserver.cpp)。プロトコルの正本は [../external_tools_requirements.md](../external_tools_requirements.md) R8。

### 枠組み

- トランスポート: **stdin/stdout の NDJSON**(1 行 = 1 つの JSON-RPC 2.0 リクエスト)。
- **stdout はプロトコル専用**。ログはファイル(cwd の `pelican.log`)へ行きます。「応答以外の行が stdout に混ざらない」ことはテストで機械検証されています。
- batch 配列・notification(id なし)は非対応(-32600)。エラーコードは標準(-32700/-32600/-32601/-32602)+アプリケーションエラー -32000。
- ハンドラ内の例外は -32000 応答に変換され、**プロセスは落ちません**。stdin の EOF で正常終了します。
- 1 プロセス 1 クライアント。起動ごとに UUID の `instance_id` が発行されます。

> **設計決定(決定性):** 同一の RPC スクリプトを 2 回実行したとき、応答列は(instance_id を除き)完全一致しなければならない。これは CI で検証されており、エージェント駆動・DCC 連携・エディタの基盤になっている。

### メソッド一覧(13 個)

| メソッド | params | 動作 |
|---|---|---|
| `get_status` | `{}` | `{instance_id, project_root, scene, frame, time, seed, stores}` を返す |
| `set_seed` | `{seed}` | 決定的乱数のシード設定 |
| `set_time` | `{t}` | 仮想時刻の直接設定(dt=0) |
| `step_frame` | `{}` | 1 フレーム進める(pending transform 適用 → 入力 → イベント → ECS → ゲームシステム → 描画) |
| `render_frame` | `{}` | **時刻を進めず**描画のみ |
| `capture` | `{path}` | 直近フレームを PNG 保存(path は出力先 locator なので cwd 相対可) |
| `get_frame_plan` | `{}` | フレームプラン JSON(`--dump-frame-plan` と同内容) |
| `load_gltf` | `{path, name?}` | glb の一時ロード(scene.json には書き戻さない)。path はプロジェクト相対のみ |
| `load_scene` | `{name}` | シーン切替([第8章](08_gameplay.md)) |
| `set_camera` | `{name}` | シーン内カメラオブジェクトへ切替 |
| `update_transforms` | `{objects:[名前...], transforms:[{pos,rotation,scale}...]}` | 名前付きオブジェクトの変換を**次のフレーム境界で**適用。`rot` キーは名指しで拒否(`rotation` を使う) |
| `inject_input` | `{events:[...]}` | 入力注入。`key_down/key_up/mouse_move/mouse_down/mouse_up/axis`([第7章](07_input_ui.md)) |
| `inject_event` | `{type, payload}` | `PELICAN_REGISTER_EVENT` で登録済みのゲームイベントを emit([第8章](08_gameplay.md)) |

### 実セッション例

結合テスト `test/run_rpc_headless.cmake` が実際に流している入力(抜粋):

```jsonl
{"jsonrpc":"2.0","id":1,"method":"get_status","params":{}}
{"jsonrpc":"2.0","id":2,"method":"set_time","params":{"t":1.25}}
{"jsonrpc":"2.0","id":3,"method":"load_gltf","params":{"path":"assets/ground.glb","name":"movable"}}
{"jsonrpc":"2.0","id":4,"method":"update_transforms","params":{"objects":["movable"],"transforms":[{"pos":[3.0,0.0,-0.75],"rotation":[0.0,0.0,0.0,1.0],"scale":[0.6,0.6,0.6]}]}}
{"jsonrpc":"2.0","id":5,"method":"step_frame","params":{}}
{"jsonrpc":"2.0","id":6,"method":"capture","params":{"path":"out/rpc_capture_left.png"}}
```

名前(オブジェクト名・`load_gltf` の name)は `[a-zA-Z0-9_]` のみ(識別子規約 R7)です。

📐設計のみ: WebSocket 展開(複数クライアント同時接続)、薄い Python クライアント `pelican_rpc.py`。

## 10.4 pelican_cli リファレンス

```
pelican_cli <import|dist-config|project> ...
```

エラーは stderr+終了コード -1、成功は 1 行サマリ+終了コード 0。

### `pelican_cli project init <dir>`(✅WP57)

空(または未存在)ディレクトリに 15 ファイルの雛形を生成します。生成直後に `pelican_player --project <dir>` でそのまま起動できます。詳細は [第2章](02_getting_started.md) §2.4。

### `pelican_cli import <delivery_dir> --project <dir|project.json>`(✅WP21)

DCC(Houdini 等)からの納品ディレクトリをプロジェクトに登録します。

1. `<delivery_dir>/manifest.json`(`pelican.import` v1)を読む。delivery_dir は**プロジェクトルート内**必須。
2. 全 outputs の **sha256 を実ファイルと照合**(不一致は即エラー)。
3. `schema: "gltf"` かつ `.glb` の出力を `asset_data.json` の `models` に `{name: <ファイル名 stem>, path: <プロジェクト相対>}` で追記。既登録の name / path はスキップ(**冪等** — 2 回実行しても重複しない)。
4. 出力: `verified N outputs, registered M models, skipped K models`

manifest の実物例(テストフィクスチャより):

```json
{
  "schema": "pelican.import",
  "version": 1,
  "tool": { "name": "houdini-adapter", "version": "0.3.0", "dcc": "houdini 21.0.512" },
  "source": { "file": "C:/show/shots/destruction_a.hip", "node": "/out/pelican_rbd", "seed": 42 },
  "created": "2026-07-02T12:00:00Z",
  "outputs": [
    { "file": "debris.glb", "schema": "gltf",
      "sha256": "50247eeaca40ca0bb1a5fb171799f32aba3cae2c8e33030bb4ba21f8d2c146c6" },
    { "file": "debris_sim.jsonl", "schema": "pelican.transform_seq", "version": 1,
      "sha256": "71167e92620243049421920148cb99b9a6088987be63e7118807e51eca237dd6" }
  ]
}
```

スキーマの詳細(outputs の file はスラッシュ相対・`..` 禁止、schema は `gltf` / `pelican.transform_seq` のみ等)は [第5章](05_assets.md) を参照してください。

### `pelican_cli dist-config <project> [--with rpc,seqplayer] [--out <file>]`(✅WP41)

> **設計決定(配布は宣言から導出):** 配布ビルドの機能フラグを手で並べさせない。プロジェクトの**内容**を走査して必要な `PELICAN_WITH_*` を導出し、根拠コメント付きの CMake キャッシュプリセットとして書き出す。

判定規則:

- `PELICAN_WITH_VAT` — プロジェクト内の GLB(asset_data + import manifest の gltf 出力)に `pelican.vat` extras が実在すれば ON
- `PELICAN_WITH_EXR` — asset_data / ui config に `.exr` 参照があれば ON
- `PELICAN_WITH_RPC` / `PELICAN_WITH_SEQPLAYER` — **既定 OFF**(配布ゲームに不要)。`--with rpc,seqplayer` で明示 ON

配布ビルドの全 3 コマンド:

```sh
pelican_cli dist-config projects/mygame --out build-dist/preset.cmake
cmake . -B build-dist -C build-dist/preset.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-dist --config Release
```

※ `PELICAN_WITH_AUDIO` は dist-config の導出対象外で、配布でも常に ON のままです(v1 仕様)。embed サブセットの絞り込み(B3)・shaderc OFF の焼き込み配布(B4)は 📐設計のみです。

## 10.5 ホットリロードと開発ワークフロー

- **シェーダホットリロード** ✅ — ウィンドウモードで実行中、シェーダソースを保存すると約 1 秒のポーリングで自動再コンパイルされます(ヘッドレスでは無効)。
- **フレームプランの確認** ✅ — `--dump-frame-plan` または rpc `get_frame_plan`([第6章](06_rendering.md))。
- シェーダ以外のアセットホットリロードは 📐設計のみです。

## 10.6 Pelican Studio(devstudio)

**現状 🚧: Qt + QML の骨組みのみ**(テキストフィールド 2 個のダミー画面)。プロジェクト読み込み・ビューポート・編集機能は未実装です。起動は `cmake --build ./build --target run_studio`。

> **設計決定(D0: エディタ特権の禁止・2026-07-08 ユーザー決定):** devstudio は公開契約(rpc / pelican_project / データ形式)の上に建つ 1 クライアントであり、エンジン内部への裏口 API を持たない。編集操作はまず rpc メソッドとして定義し、devstudio はそれを呼ぶだけ。効果: エージェント(自動化ツール)が人間と同一の操作面を持ち、エディタのテストが rpc 結合テストに還元される。

計画されている段階(すべて 📐未着手): D1(プロジェクトを開く+埋め込みビューポート+読み取り専用アウトライナ)→ D2(ピッキング+ギズモ+プロパティ編集)→ D3(シーン保存 round-trip + undo/redo)。※現行コードは `pelican_core` を直接リンクしており、D0 規約とはまだ不整合です(D1 着手時に整理予定)。

ツール自作の入口は 3 つ: **データツール**(形式+`pelican_project` ライブラリ)/ **ライブツール**(JSON-RPC)/ **組み込みビュー**(埋め込みビューポート+rpc、将来)。

## 10.7 テスト基盤

- 単体テスト: Catch2 v3。`test/CMakeLists.txt` の `pelican_define_test(<name> [libs...])` で登録します。
- **GPU 必須テストは Vulkan デバイス列挙失敗時に `SKIP()`** が規約です。
- 結合テスト: `test/run_*.cmake` が player / cli を子プロセス起動し、stdout・生成物・PNG を検証します(rpc 決定性、devcli 3 コマンド、seq/vat 再生、フレームプランダンプ等)。
- ゴールデンイメージテスト(✅WP16): ヘッドレス描画の出力 PNG を基準画像と比較します。流儀は [../rendering_phase1_review.md](../rendering_phase1_review.md)(Validation Run)を参照。
- ctest 非登録のスモーク: `test/run_build_units_smoke.cmake`(4 ユニットを単独 OFF にしてビルド+エラー文言検証)、`test/run_project_code_smoke.cmake`(`PELICAN_PROJECT` ビルド)。

## 関連文書

- [../external_tools_requirements.md](../external_tools_requirements.md) — 外部ツール契約(R1〜R10。JSON-RPC は R8)
- [../design_devstudio_direction.md](../design_devstudio_direction.md) — エディタの方向性(D0〜D3)
- [../design_build_tiers.md](../design_build_tiers.md) — ビルドユニット・配布
- [../design_headless_rendering.md](../design_headless_rendering.md) — ヘッドレス描画
- [第2章 ビルドと起動](02_getting_started.md) / [第6章 レンダリング](06_rendering.md) / [第8章 ゲームロジック](08_gameplay.md)
