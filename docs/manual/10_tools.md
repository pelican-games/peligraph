# 第10章 ツールリファレンス

対象: pelican2(2026-07-17 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- `pelican_player` の全 CLI 引数(22 個)
- JSON-RPC(stdio)による外部制御 — 全 19 メソッドと実セッション例
- `pelican_cli` の 6 サブコマンド系統(`project init` / `import` / `dist-config` / `assets` / `bake-camera` / `dump-lowered-material`)
- ホットリロード(シェーダ・テクスチャ・マテリアル・モデル・ゲーム DLL)と起動高速化
- ImGui 開発者 UI(F1)と Frame Plan Viewer
- 配布ビルド、Pelican Studio の現状、テスト基盤

## 10.1 実行物の一覧

| ターゲット | 種類 | 出力先 | 役割 |
|---|---|---|---|
| `pelican_player` | exe | build ツリー内(`build/src/player/<Config>/`) | ゲームランタイム。ウィンドウ / ヘッドレス / RPC の 3 モード |
| `pelican_game_logic` | **DLL**(`PELICAN_PROJECT` 指定時) | player と同ディレクトリ | プロジェクトの `code/` のビルド産物(✅WP90 で静的リンクから移行) |
| `pelican_cli` | exe | `dist/`(Debug は `dist_debug/`) | 開発 CLI。**エンジン(pelican_core)にリンクしない** |
| `pelican_studio` | exe(Qt6) | `dist/`(同上) | エディタ。現状はダミー画面のみ(§10.8) |
| `pelican-spv-link` | exe | build 内 | experimental SPIR-V リンカ CLI([第6章](06_rendering.md) §6.7) |
| `pelican_project` | 静的 lib | build 内 | 解釈レイヤ(JSON パース/検証の純ロジック) |
| `pelican_core` | 静的 lib | build 内+`dist/lib` | エンジン本体。公開ヘッダは `dist/lib/include` へコピー |

## 10.2 pelican_player CLI リファレンス

実装: [src/player/main.cpp](../../src/player/main.cpp)。引数エラー時は usage を表示して終了コード -1、正常終了 0、実行時致命エラー 1。

| 引数 | 既定 | 意味 |
|---|---|---|
| `--project <dir\|project.json>` | (暗黙探索) | プロジェクトを開く。省略時は exe ディレクトリ → 祖先の `projects/example` を探索し、WARN を出す |
| `--headless` | off | ウィンドウなし実行(時刻は固定ステップになる) |
| `--rpc` | off | stdio JSON-RPC モード。**v1 では `--headless` 必須** |
| `--frames <n>` | 3 | ヘッドレスのフレーム数。**0 = 無制限** |
| `--size WxH` | 1280x720 | ヘッドレスの描画サイズ |
| `--render-out <path>` | なし | PNG 出力先(sRGB エンコード)。`%04d` 等で毎フレーム連番 |
| `--fps <f>` | 60.0 | ヘッドレス固定ステップのレート(dt = 1/fps) |
| `--dump-frame-plan` | off | 解決済みフレームプラン JSON を **stderr** へ出力([第6章](06_rendering.md)) |
| `--allow-absolute-paths` | off | CLI 由来のコンテンツ参照に限り絶対パスを許可。JSON 内の絶対パスは常に拒否 |
| `--user-dir <dir>` | なし | `user://` ルートの差し替え |
| `--ignore-engine-version` | off | `engine_min_version` 不適合を hard error → WARN に降格 |
| `--play-seq <path.jsonl>` | なし | `pelican.transform_seq` の再生(SEQPLAYER ユニット必須) |
| `--seq-mesh <builtin:sphere\|path.glb>` | builtin:sphere | transform_seq 全オブジェクトに使うメッシュ |
| `--seq-loop` | off | クリップをループ再生 |
| `--play-vat <path.glb>` | なし | `pelican.vat` 入り GLB の再生(VAT ユニット必須) |
| `--camera "px,py,pz,tx,ty,tz,fov_deg"` | なし | カメラの位置・注視点・垂直 FOV(度)の上書き |
| `--game-logic <path.dll>` | (exe 同階層) | ゲームロジック DLL の明示指定(✅WP90) |
| `--strict-assets` | off | assets manifest の差分を起動エラーに昇格(✅WP66。ホットリロードも無効化) |
| `--record-input <path.jsonl>` | なし | 順序付き入力を `pelican.input_seq` v1 で収録(✅WP89) |
| `--replay <path.jsonl>` | なし | input_seq のリプレイ。`--record-input` と排他、ホットリロード自動無効(✅WP89) |
| `--input-profile <name>` | project.json の既定 | アクティブ入力プロファイルの上書き(✅WP91) |
| `--xr off\|auto\|on` | off | OpenXR(PCVR)起動(✅WP125。`auto` = 不在なら flat 続行 / `on` = 不在・headless・rpc・リプレイでは名指しエラー — [第2章](02_getting_started.md)) |
| `--bake-camera-output <path.jsonl>` | なし | リプレイ中のカメラ軌跡を transform_seq v1 で出力。`--headless` + `--replay` 必須(✅WP89) |

よく使う組み合わせ:

```sh
pelican_player --project projects/example                                    # 通常起動
pelican_player --headless --project mygame --frames 3 --render-out out.png  # ヘッドレス→PNG
pelican_player --rpc --headless --project mygame                            # 外部制御
pelican_player --project mygame --record-input s.jsonl                      # プレイ収録
pelican_player --headless --project mygame --replay s.jsonl \
  --bake-camera-output cam.jsonl                                            # カメラ焼き出し
```

※ `--play-seq` / `--seq-mesh` の相対パスは現状 cwd 基準で解決されます(既知の食い違い。[第11章](11_status.md))。

## 10.3 JSON-RPC(stdio)— 外部制御プロトコル(✅実装済み)

実装: [rpcserver.cpp](../../src/core/communication/rpcserver.cpp)。プロトコルの正本は [../external_tools_requirements.md](../external_tools_requirements.md) R8。

### 枠組み(不変)

- stdin/stdout の NDJSON(1 行 = 1 つの JSON-RPC 2.0 リクエスト)。**stdout はプロトコル専用**(ログは `pelican.log`)。
- batch・notification 非対応。エラーコードは標準 + アプリケーションエラー -32000。ハンドラ内例外は応答に変換されプロセスは落ちません。EOF で正常終了。
- 1 プロセス 1 クライアント。**ゲーム DLL リロード中は全メソッドがアプリケーションエラーで拒否**されます。

> **設計決定(決定性):** 同一の RPC スクリプトを 2 回実行したとき、応答列は(instance_id を除き)完全一致しなければならない。CI で検証済み。

### メソッド一覧(19 個)

| メソッド | params | 動作 |
|---|---|---|
| `get_status` | `{}` | `{instance_id, project_root, scene, frame, time, seed, stores, input:{...}, reload:{...}, sprite:{...}, color:{...}, startup:{...}, xr:{active, reference_space, floor_semantics, ...}}` ※rpc は常に flat 駆動のため `xr.active=true` は rpc からは観測できない |
| `set_seed` | `{seed}` | 決定的乱数のシード設定 |
| `set_time` | `{t}` | 仮想時刻の直接設定(dt=0) |
| `step_frame` | `{}` | 1 フレーム進める |
| `render_frame` | `{}` | 時刻を進めず描画のみ |
| `capture` | `{path}` | 直近フレームを PNG 保存(sRGB。応答に `encoding:"srgb"`) |
| `get_frame_plan` | `{}` | フレームプラン JSON |
| `load_gltf` | `{path, name?}` | glb の一時ロード(プロジェクト相対のみ) |
| `load_scene` | `{name}` | シーン切替 |
| `set_camera` | `{name}` | シーン内カメラへ切替 |
| `update_transforms` | `{objects, transforms}` | 次フレーム境界で適用。`rot` は名指し拒否(`rotation`) |
| `inject_input` | `{events:[...]}` | 入力注入。`key_*` / `mouse_*` / `axis` + **`gamepadButtonDown` / `gamepadUp` / `gamepadAxis`**(リプレイ中は拒否) |
| `inject_event` | `{type, payload}` | 登録済みゲームイベントの emit。payload は EventPayloadSchema で事前検証(✅WP71) |
| `reload_game_logic` | `{}` | ゲームロジック DLL を即時リロード(✅WP90) |
| `set_input_profile` | `{name}` | 入力プロファイル切替(✅WP91) |
| `start_input_record` | `{path}` | input_seq 収録開始(✅WP89) |
| `stop_input_record` | `{}` | 収録停止。`{path, frames, events}` |
| `start_input_replay` | `{path}` | リプレイ開始(fixed-step 化・リロードゲート閉鎖) |
| `stop_input_replay` | `{}` | リプレイ停止 |

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

名前(オブジェクト名・`load_gltf` の name)は `[a-zA-Z0-9_]` のみ(R7)です。

📐設計のみ: WebSocket 展開(複数クライアント)、薄い Python クライアント `pelican_rpc.py`。

## 10.4 pelican_cli リファレンス

```
pelican_cli <assets|bake-camera|import|dist-config|project|dump-lowered-material> ...
```

エラーは stderr+終了コード -1、成功は 1 行サマリ+終了コード 0。

### `pelican_cli project init <dir>`(✅WP57)

空(または未存在)ディレクトリに雛形一式を生成します。生成直後に `pelican_player --project <dir>` でそのまま起動できます。詳細は [第2章](02_getting_started.md)。

### `pelican_cli import ...`(✅WP21/79/84)

DCC 納品物の取り込み。3 つの形があります:

- `import <delivery_dir> --project <dir>` — `pelican.import` manifest の sha256 照合 → asset_data.json へ glb 登録(冪等)
- `import gltf --extract-scene <glb> ...` — glTF のノード階層を scene v1(`parent` 付き)として抽出(✅WP79。[第5章](05_assets.md))
- `import --rules ...` — `imports.rules.json`(glob → レシピ表)に従った一括取り込み(✅WP84)

manifest の形式・実例は [第5章](05_assets.md) を参照してください。

### `pelican_cli assets manifest|verify|status --project <dir>`(✅WP66)

asset store の sha256 台帳(assets manifest)を生成・照合します。

```sh
pelican_cli assets manifest --project mygame            # 生成(store 走査 → manifest 書き出し)
pelican_cli assets verify --project mygame [--full]     # 照合(差分は severity 付き列挙、あれば exit 1)
pelican_cli assets status --project mygame              # OK / MISSING / MISMATCH の一覧
```

> **設計決定(検証はロードを止めない):** 起動時検証の深刻度は「内容の変化 = INFO、構造の逸脱(欠落・参照不能)= WARNING」で、エラーに昇格するのは `--strict-assets`(または配布 strict)のときだけ。

### `pelican_cli bake-camera --replay <path.jsonl> --project <dir>`(✅WP89)

リプレイを headless 実行し、カメラ軌跡を `imports/pelican-camera/<name>/` に `camera.transform_seq.jsonl` + `pelican.import` manifest として着地させます(DCC への逆輸出)。

### `pelican_cli dump-lowered-material <surface>`(✅WP76)

`.surface` の lowering 結果(std140 レイアウト・生成 GLSL)を stdout に出力します。「B は C の糖衣」の検証・デバッグ用です。

### `pelican_cli dist-config <project> [--with rpc,seqplayer] [--out <file>]`(✅WP41)

> **設計決定(配布は宣言から導出):** 配布ビルドの機能フラグを手で並べさせない。プロジェクトの内容を走査して必要な `PELICAN_WITH_*` を導出し、根拠コメント付きの CMake キャッシュプリセットとして書き出す。

- `PELICAN_WITH_VAT` — GLB に `pelican.vat` extras が実在すれば ON / `PELICAN_WITH_EXR` — `.exr` 参照があれば ON
- `PELICAN_WITH_RPC` / `PELICAN_WITH_SEQPLAYER` / `PELICAN_WITH_OPENXR` — 既定 OFF。`--with rpc,seqplayer,openxr` で明示 ON
- **`PELICAN_WITH_IMGUI` — 常に OFF を書き出す**(配布ビルドに開発 UI を含めない)

```sh
pelican_cli dist-config projects/mygame --out build-dist/preset.cmake
cmake . -B build-dist -C build-dist/preset.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-dist --config Release
```

※ AUDIO / PHYSICS 系は導出対象外(常に既定 ON)。B3(embed サブセット)・B4(shaderc OFF 焼き込み配布)は 📐設計のみ。

## 10.5 ホットリロード(✅WP90/96〜110)

保存すれば動いたまま反映される、が現在の開発体験です。基盤は [src/core/watch/](../../src/core/watch)(FileWatcher + ReloadService。デバウンス 200ms、適用は**次フレーム先頭で一括**、mid-frame 差し替えなし)。

| 対象 | 状態 | 挙動 |
|---|---|---|
| シェーダ / `.surface`(+ `.material.json` の cross-file) | ✅WP108 | 影響する全バリアント・パイプラインを準備してから atomic に公開。失敗時は旧世代維持 + WARNING |
| テクスチャ(PNG / EXR / KTX2) | ✅WP100 | 同 shape は in-place、shape 変化は再バインド(実測 ~1ms) |
| マテリアル値(`.material.json`) | ✅WP105 | **同レイアウトのみ** SSBO 値更新(レイアウト変化は WARN + 旧値継続 🚧) |
| モデルコンテナ(.glb / .gltf / .vrm) | ✅WP110 | 同一コンテナの全 fragment + 配置済み全インスタンスを 1 トランザクションで再構築(ID・Transform・物理は不変) |
| ゲームロジック DLL | ✅WP90 | 下記 |
| input actions / profile(HR2-I)、pelican.ui(U3) | 📐 | backlog Tier 1 |
| scene JSON / rendering config / project.json | 対象外 | 設計上の決定(再起動) |

- **有効条件**: ウィンドウモードのみ(既定 ON・専用フラグなし)。リプレイ / `--strict-assets` / rpc 駆動中は中央ゲートで無効。headless では自動監視しません。
- **fail-soft**: 壊れたファイルを保存しても旧リソースで動き続け、名前入り WARNING と `get_status.reload.last_reload_error` に出ます。修正して保存し直せば回復します。

### ゲームロジック DLL のホットリロード(✅WP90 = G2)

`-DPELICAN_PROJECT=<dir>` でプロジェクトの `code/` は **`pelican_game_logic.dll`** としてビルドされます(静的リンクからの移行 — [第8章](08_gameplay.md))。リロードのトリガーは 3 通り:

1. ウィンドウモードでの自動検出(DLL の更新を監視)
2. **F5** で強制リロード
3. rpc `reload_game_logic`

v1 は **full-reset 方式**: システムの状態と ECS を全破棄し、現在のシーンを再構築します(ホットステートの維持はしません)。ABI バージョン検証付きで、ロード失敗時は旧 DLL のまま続行します。

### 起動高速化(✅WP82)

- **シェーダディスクキャッシュ**: `<project>/.pelican/shader_cache/`(SHA-256 キー)。2 回目以降のシェーダコンパイルはほぼゼロに。破損しても WARN 1 行で通常コンパイルに戻ります。
- **並列モデルロード**: CPU 側 prepare を並列化(GPU commit は宣言順直列 = 決定性維持)。起動時に 1 行レポート(`startup: ...`)が出て、`get_status.startup` でも見えます。

## 10.6 ImGui 開発者 UI(✅WP85/86)

`PELICAN_WITH_IMGUI`(既定 ON・配布は dist-config が常時 OFF)でビルドすると、**通常ウィンドウ起動時のみ** ImGui のデバッグ UI が使えます(headless / rpc / リプレイ / golden では無効)。

- **F1** で表示トグル(専用 CLI フラグはありません)。メニューバー「Pelican」→ Frame Plan Viewer / Frame Stats(FPS・frame index・CPU 時間)/ ImGui Demo。
- **Frame Plan Viewer**: フレームプランのノードグラフ可視化(パス = ノード、RT = エッジ。ホイールでズーム、中/右ドラッグでパン、左クリックで選択詳細)。データ源は `get_frame_plan` と同一の公開 JSON のみ — **D0(エディタ特権の禁止)準拠の第 1 実例**です。
- すべて読み取り専用の可視化で、編集系のミューテーション経路はありません。

## 10.7 Pelican Studio(devstudio)

**現状 🚧: Qt + QML の骨組みのみ**(ダミー画面)。起動は `cmake --build ./build --target run_studio`。

> **設計決定(D0: エディタ特権の禁止・2026-07-08):** devstudio は公開契約(rpc / pelican_project / データ形式)の上に建つ 1 クライアントであり、エンジン内部への裏口 API を持たない。編集操作はまず rpc メソッドとして定義し、devstudio はそれを呼ぶだけ。

計画段階(すべて 📐未着手): D1(プロジェクトを開く+埋め込みビューポート)→ D2(ピッキング+ギズモ+編集)→ D3(保存 round-trip + undo/redo)。ツール自作の入口は 3 つ: データツール(`pelican_project`)/ ライブツール(JSON-RPC)/ 組み込みビュー(将来)。ImGui オーバーレイ(§10.6)が D0 準拠ツールの先行実例です。

## 10.8 テスト基盤

- 単体テスト: Catch2 v3(`pelican_define_test`)。**GPU 必須テストは Vulkan デバイス列挙失敗時に `SKIP()`**。
- 結合テスト: `test/run_*.cmake` が player / cli を子プロセス起動して検証。2026-07-10 以降の追加: `run_devcli_assets`(WP66)/ `run_event_schema_compile`(WP71)/ `run_dump_lowered_material`(WP76)/ `run_devcli_gltf_extract`(WP79)/ `run_spvlink_golden`(WP80)/ `run_devcli_rules_import`(WP84)/ `run_devcli_bake_camera` + `run_input_record_replay_headless`(WP89)/ `run_ui_u2_rpc_replay`(WP93)など。
- ゴールデンイメージテスト: **49 ケース**(ディレクトリ自動発見。[第6章](06_rendering.md) §6.10)。
- ctest 非登録のスモーク: `run_build_units_smoke.cmake`(IMGUI / PHYSICS 系を含む単独 OFF ビルド検証)、`run_project_code_smoke.cmake`。
- **CI(✅WP137)**: GitHub Actions の Windows **CPU ゲート**が push/PR で回ります(`.github/workflows/`)。GPU 必須テストは `gpu` ラベルで除外し、`SKIP` は完全一致 allowlist のみ許可(想定外の SKIP はゲート失敗)・リトライなし。

## 関連文書

- [../external_tools_requirements.md](../external_tools_requirements.md) — 外部ツール契約(R1〜R10。JSON-RPC は R8)
- [../design_asset_hot_reload.md](../design_asset_hot_reload.md) — アセットホットリロード(v2.1・HR0〜HR2-G 実装済み)
- [../design_game_logic_native.md](../design_game_logic_native.md) — ゲームロジック(G2 DLL リロード実装済み)
- [../design_devstudio_direction.md](../design_devstudio_direction.md) — エディタの方向性(D0〜D3)
- [../design_build_tiers.md](../design_build_tiers.md) — ビルドユニット・配布
- [第2章 ビルドと起動](02_getting_started.md) / [第6章 レンダリング](06_rendering.md) / [第7章 入力と UI](07_input_ui.md) / [第8章 ゲームロジック](08_gameplay.md)
