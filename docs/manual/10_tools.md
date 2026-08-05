# 第10章 ツールリファレンス

対象: pelican2(2026-07-17 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- `pelican_player` の全 CLI 引数(22 個)
- JSON-RPC(stdio)による外部制御 — 基盤メソッド、エディタ拡張、実セッション例
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
| `pelican_studio` | exe(Qt6) | `dist/`(同上) | Widgets エディタ shell + 別 process engine viewport(§10.7) |
| `pelican-spv-link` | exe | build 内 | `PELICAN_WITH_SPIRV_LINK=ON`時だけ作るexperimental SPIR-V リンカ CLI([第6章](06_rendering.md) §6.7) |
| `pelican_project` | 静的 lib | build 内 | 解釈レイヤ(JSON パース/検証の純ロジック) |
| `pelican_core` | 静的 lib | build 内+`dist/lib` | エンジン本体。公開ヘッダは `dist/lib/include` へコピー |

## 10.2 pelican_player CLI リファレンス

実装: [src/player/main.cpp](../../src/player/main.cpp)。引数エラー時は usage を表示して終了コード -1、正常終了 0、実行時致命エラー 1。

| 引数 | 既定 | 意味 |
|---|---|---|
| `--project <dir\|project.json>` | (暗黙探索) | プロジェクトを開く。省略時は exe ディレクトリ → 祖先の `projects/example` を探索し、WARN を出す |
| `--headless` | off | ウィンドウなし実行(時刻は固定ステップになる) |
| `--rpc` | off | stdio JSON-RPC モード。**`--headless` は不要になりました**(✅WP156): ヘッドレスでは stdin を読み切るまでブロッキング、**ウィンドウモードではフレーム境界処理 + 有界キュー(既定 64 件)**。溢れると `busy` エラーを即返します。※`--rpc` を付けると ImGui UI は無効([第13章](13_editor.md)) |
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
| `--free-camera` | off | project を変更せず runtime-only の `fly` camera と埋め込み入力 profile を有効化。WASD=移動、矢印=視線、左右 stick にも対応(✅WP265) |
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
- batch・notification 非対応。エラーコードは標準 + アプリケーションエラー -32000、RenderDoc capture -32010。ハンドラ内例外は応答に変換されプロセスは落ちません。EOF で正常終了。
- 1 プロセス 1 クライアント。**ゲーム DLL リロード中は全メソッドがアプリケーションエラーで拒否**されます。

> **設計決定(決定性):** 同一の RPC スクリプトを 2 回実行したとき、応答列は(instance_id を除き)完全一致しなければならない。CI で検証済み。

### 基盤メソッド一覧

| メソッド | params | 動作 |
|---|---|---|
| `get_status` | `{}` | `{instance_id, project_root, scene, frame, time, seed, renderdoc, diagnostics:{renderdoc:{...}}, stores, input:{...}, reload:{...}, sprite:{...}, color:{...}, startup:{...}, xr:{active, reference_space, floor_semantics, ...}}` ※rpc は常に flat 駆動のため `xr.active=true` は rpc からは観測できない |
| `set_seed` | `{seed}` | 決定的乱数のシード設定 |
| `set_time` | `{t}` | 仮想時刻の直接設定(dt=0) |
| `step_frame` | `{}` | 1 フレーム進める |
| `render_frame` | `{}` | 時刻を進めず描画のみ |
| `capture_gpu` | `{}` | `render_frame` と同じcurrent-time描画を1回だけRenderDoc captureし、実際に増えたindexのcanonical `.rdc` pathを返す |
| `capture` | `{path}` | 直近フレームを PNG 保存(sRGB。応答に `encoding:"srgb"`) |
| `get_frame_plan` | `{}` | フレームプラン JSON |
| `pick_object` | `{x, y}` | 最後に完了した ID バッファの 1 ピクセルを同期読み出し。`engine://features/picking.json` が必要 |
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
| `export_scene_snapshot` | `{schema_version:1, allow_pending?:false}` | current scene を含む全 authoring document の deterministic semantic bytes、SHA-256、revision を返す |
| `import_scene_snapshot` | `{schema_version:1, semantic_scene_bytes, digest, current_scene_id}` | disk を上書きせず、検証済み snapshot を in-memory scene source として reload する |

`pick_object` の座標は `picking_id` の左上原点で、`x` は右、`y` は下へ増えます。
両方とも符号なし整数だけを受理し、範囲外は `-32602`、feature 無効または完了済みの
picking frame が無い場合は `-32000` です。背景は `hit: null`、モデル面は次の形です。

```json
{
  "contract": 1,
  "coordinate": {"x": 320, "y": 180},
  "extent": {"width": 1280, "height": 720},
  "frame_index": 42,
  "hit": {
    "scene_id": "default_scene",
    "declaration_index": 3,
    "authoring_object_id": "...",
    "model_instance": {"index": 7, "generation": 2, "scene_epoch": 4}
  }
}
```

`scene_id` と 0 始まりの `declaration_index` は `scene_tree` / `get_components` と同じ
WP258 契約です。authoring 宣言を持たない一時モデルでは三つの authoring field が
`null` でも、世代付き `model_instance` は残ります。読み出しを同期にした理由と ID の
GPU 表現は[第6章](06_rendering.md#id-バッファ-pickingwp262)を参照してください。

### エディタ拡張メソッド(詳細は[第13章](13_editor.md))

上の表の `export_scene_snapshot` / `import_scene_snapshot` と合わせて、エディタトラックで増えたのは **23 メソッド**です(RPC は全部で 44 メソッド)。

シーンの編集・履歴・プレビュー・保存のためのメソッド群です(✅WP154/156/157/158/161/166/168/170/172)。params と戻り値、**エラーが `result.status` に出る**という重要な作法は [第13章](13_editor.md) §13.5〜§13.6 にまとめてあります。

| 分類 | メソッド |
|---|---|
| 読み取り | `scene_tree` / `get_components` / `get_scene_revision` / `list_assets` / `query_journal` / `can_edit` / `can_preview` |
| セッション | `open_editor_session` / `resume_editor_session` |
| 編集 | `edit` / `undo` / `redo` / `get_edit_result` |
| チケットプレビュー | `open_preview` / `update_preview` / `commit_preview` / `abort_preview` / `get_preview_result` |
| 隔離プレビュー | `eval_preview` / `render_preview` |
| 保存 | `save_scene` |

既存メソッドの変化: `get_status` に `scene_source`(シーンは**ホットリロード対象外**・保存は `save_scene`)が追加、`step_frame` の応答に `edit_results[]` が追加、`load_scene` / `reload_game_logic` / `start_input_replay` は進行中のプレビューを強制中止します。

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

`capture_gpu` は外部からRenderDocを注入して起動した場合だけ有効です。通常起動では
`-32010` と `data.reason:"renderdoc_not_injected"` を返します。XR active中はv1境界として
`capture_xr_unsupported` です。F11とRPCはいずれも同じcapture状態機械を使います。

### Python 薄型クライアント

`tools/pelican_rpc.py` は Python 3.12 の標準ライブラリだけで player の起動と
JSON-RPC の送受信を行います。リポジトリルートをカレントディレクトリにして、
次のように使用します。

```python
from tools.pelican_rpc import PelicanRpc, PelicanRpcError

try:
    with PelicanRpc("projects/mygame") as rpc:
        status = rpc.get_status()
        rpc.set_time({"t": 1.25})
        frame = rpc.step_frame()
        tree = rpc.scene_tree()
        print(status["scene"], frame["frame"], tree["objects"])
except PelicanRpcError as error:
    print(error.code, error.message, error.data)
```

`exe_path` を省略すると `build/src/player/Debug/pelican_player.exe` を探索します。
別の build ツリーを使う場合は `PelicanRpc(project, exe_path=player_path)` と明示して
ください。`call(method, params)` と各 helper は params の辞書を変換・検証せず
エンジンへ渡します。形式の正は常に `rpcserver.cpp` です。常駐プロセスを残さない
ため、通常は上例のように `with` を使用してください。

`export_scene_snapshot(params)` と `import_scene_snapshot(params)` も同じ素通し helper
です。import の検証は client ではなく engine が、必ず version → 64 MiB → SHA-256 →
parse/semantic → current scene の順で行います。失敗時は `PelicanRpcError.data` の
`code` が `unsupported_snapshot_version` / `snapshot_too_large` /
`digest_mismatch` / `snapshot_invalid` / `scene_not_found` のいずれかになり、
`snapshot_invalid` には `detail` も入ります。

### snapshot を使った安全なスイープ

人が操作している session の document を scratch process へ immutable に渡し、候補値だけを
人 session へ戻す公式 sequence は次のとおりです。snapshot 自体を merge/save する API は
ありません。

```python
from tools.pelican_rpc import PelicanRpc, PelicanRpcError

with PelicanRpc("projects/mygame") as human:
    exported = human.export_scene_snapshot(
        {"schema_version": 1, "allow_pending": False}
    )
    revision = exported["scene_revision"]  # R
    payload = {
        "schema_version": 1,
        "semantic_scene_bytes": exported["semantic_scene_bytes"],
        "digest": exported["digest"],
        "current_scene_id": exported["current_scene_id"],
    }

    with PelicanRpc("projects/mygame") as scratch:
        scratch.import_scene_snapshot(payload)
        # 候補ごとに PREVIEW0 の eval_preview / render_preview を call() し、
        # 必要なら capture() する。scratch の scene file は変更されない。
        chosen_operation = sweep_and_choose(scratch)

    session = human.call(
        "open_editor_session", {"display_name": "snapshot sweep adoption"}
    )
    accepted = human.call(
        "edit",
        {
            "actor_id": session["actor_id"],
            "base_revision": revision,
            "operations": [chosen_operation],
        },
    )
    # frame boundary 後に get_edit_result を確認する。stale_revision なら
    # 自動再送せず、現在値を query → 再 export → 候補を再判断する。
```

`semantic_scene_bytes` は UTF-8 JSON text の inline string で、base64 ではありません。
上限は 64 MiB です。import 成功時も通常 project の scene file は byte 不変で、
AuthoringObjectId は scratch session 内で新しく採番されます。採用は必ず `base_revision=R`
の通常 edit 一件に絞り、`stale_revision` を「人が R 以後に編集した」合図として扱います。

📐設計のみ: WebSocket 展開(複数クライアント)。

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
- **`PELICAN_WITH_RENDERDOC` — 常に OFF を書き出す**(配布ビルドにcapture integrationを含めない)

```sh
pelican_cli dist-config projects/mygame --out build-dist/preset.cmake
cmake . -B build-dist -C build-dist/preset.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-dist --config Release
```

※ AUDIO / PHYSICS 系は導出対象外(常に既定 ON)。B3(embed サブセット)・B4(shaderc OFF 焼き込み配布)は 📐設計のみ。

## 10.5 ホットリロード(✅WP90/96〜110/147/162/178)

保存すれば動いたまま反映される、が現在の開発体験です。基盤は [src/core/watch/](../../src/core/watch)(FileWatcher + ReloadService。デバウンス 200ms、適用は**次フレーム先頭で一括**、mid-frame 差し替えなし)。

| 対象 | 状態 | 挙動 |
|---|---|---|
| シェーダ / `.surface`(+ `.material.json` の cross-file) | ✅WP108 | 影響する全バリアント・パイプラインを準備してから atomic に公開。失敗時は旧世代維持 + WARNING |
| テクスチャ(PNG / EXR / KTX2) | ✅WP100 | 同 shape は in-place、shape 変化は再バインド(実測 ~1ms) |
| マテリアル値(`.material.json`) | ✅WP105 | **同レイアウトのみ** SSBO 値更新(レイアウト変化は WARN + 旧値継続 🚧) |
| モデルコンテナ(.glb / .gltf / .vrm) | ✅WP110/147 | 同一コンテナの全 fragment + live instanceを1 transactionで再構築。animationは対象asset generationだけ更新し、別assetは継続 |
| VRMA source/profile | 部分✅WP178 | generation/stale/rebind entry pointは済。FileWatcher/loaderからの自動配線は未 |
| ゲームロジック DLL | ✅WP90/162 | 下記。candidateを旧DLLと並存side-decodeしてから切替 |
| input actions / profile(HR2-I)、pelican.ui(U3) | 📐 | backlog Tier 1 |
| scene JSON / rendering config / project.json | 対象外 | 設計上の決定(再起動) |

- **有効条件**: ウィンドウモードのみ(既定 ON・専用フラグなし)。リプレイ / `--strict-assets` / rpc 駆動中は中央ゲートで無効。headless では自動監視しません。
- **fail-soft**: 壊れたファイルを保存しても旧リソースで動き続け、名前入り WARNING と `get_status.reload.last_reload_error` に出ます。修正して保存し直せば回復します。

### ゲームロジック DLL のホットリロード(✅WP90/162 = G2/BEH1)

`-DPELICAN_PROJECT=<dir>` でプロジェクトの `code/` は **`pelican_game_logic.dll`** としてビルドされます(静的リンクからの移行 — [第8章](08_gameplay.md))。リロードのトリガーは 3 通り:

1. ウィンドウモードでの自動検出(DLL の更新を監視)
2. **F5** で強制リロード
3. rpc `reload_game_logic`

適用方式は **full-reset**: system state/ECS/behaviorを破棄して現在sceneを
再構築し、hot stateは維持しません。ただしcandidate DLLは旧DLLと並存する
registration-only phaseでABI、schema fingerprint、type継続性、全authored
behavior paramsをside-decodeします。不適合なら旧DLL/runtimeを一切変えず拒否します。

### 起動高速化(✅WP82)

- **シェーダディスクキャッシュ**: `<project>/.pelican/shader_cache/`(SHA-256 キー)。2 回目以降のシェーダコンパイルはほぼゼロに。破損しても WARN 1 行で通常コンパイルに戻ります。
- **並列モデルロード**: CPU 側 prepare を並列化(GPU commit は宣言順直列 = 決定性維持)。起動時に 1 行レポート(`startup: ...`)が出て、`get_status.startup` でも見えます。

## 10.6 ImGui 開発者 UI(✅WP85/86/159/164/170)

`PELICAN_WITH_IMGUI`(既定 ON・配布は dist-config が常時 OFF)でビルドすると、**通常ウィンドウ起動時のみ** ImGui のデバッグ UI が使えます。**`--rpc` / headless / リプレイ / golden / XR セッション中は無効**です(そのため「GUI で触りながら外部エージェントも繋ぐ」ことは現状できません — [第13章](13_editor.md) §13.1)。

- **F1** で表示トグル(専用 CLI フラグはありません)。メニューバー
  「Pelican」から Frame Plan Viewer、Frame Stats、Asset Browser、
  Object Tree / Inspector、ImGui Demoを開けます。
- **Frame Plan Viewer**: フレームプランのノードグラフ可視化(パス = ノード、RT = エッジ。ホイールでズーム、中/右ドラッグでパン、左クリックで選択詳細)。データ源は `get_frame_plan` と同一の公開 JSON のみ — **D0(エディタ特権の禁止)準拠の第 1 実例**です。
- Asset Browserは読み取り専用です。Object Tree / Inspectorの編集は
  ECS/renderer/PhysWorldへ直接触れず、RPCと同じtyped
  `EditorCommandService`を通ります。schema-driven field、live/ticket
  preview、undo/redo、atomic save、watch tokenによる再queryを備えます。

## 10.7 Pelican Studio(devstudio)

**現状 🚧: Qt Widgets の editor shell、別 process engine viewport、Outliner と viewport の
選択同期まで実装済み。**
起動は `cmake --build ./build --target run_studio`。

> **設計決定(D0: エディタ特権の禁止・2026-07-08):** devstudio は公開契約(rpc / pelican_project / データ形式)の上に建つ 1 クライアントであり、エンジン内部への裏口 API を持たない。編集操作はまず rpc メソッドとして定義し、devstudio はそれを呼ぶだけ。

project を開くと、中央 viewport は同じ project を `--rpc --project <root>` 付きの
`pelican_player` 子 process として起動し、Windows の native HWND を
Qt host へ再親付けします。player が自分の swapchain へ描いて OS が合成するため、pixel readback、
copy、process 間 frame 転送はありません。player が終了しても Studio は残り、`Restart Engine`
から再起動できます。既定では Studio と同じ directory、次に
`../build/src/player/Debug/pelican_player.exe` を探します。project と `--rpc` は Studio が所有し、
`PELICAN_STUDIO_PLAYER_ARGUMENTS` 内の同名指定は除去します。開発時の executable と追加引数の
override は次です:

```powershell
$env:PELICAN_STUDIO_PLAYER = "C:/path/to/pelican_player.exe"
$env:PELICAN_STUDIO_PLAYER_ARGUMENTS = "--gpu-labels --free-camera"
dist_debug/pelican_studio.exe
```

`--project` はここに書きません。Studio が開いた project から自分で渡すため、同名指定は除去されます。

`--free-camera` は Studio 固有機能ではなく同じ `pelican_player` の公開引数です。したがって
上の環境変数を使わず、素の player へ直接指定しても同じ自由飛行カメラになります。overlay は
メモリ上だけにあり、scene JSON や入力 profile を保存しません。

viewport の左クリックは child client の物理 pixel 座標を `pick_object` へ送り、応答の
`(scene_id, declaration_index)` を Outliner と Inspector に反映します。Outliner を選んだ方向も
同じ `SelectionModel` を更新し、viewport はその 1 個の状態を非所有参照します。名前は表示にしか
使わないため、無名 object も別々に選択できます。この WP では枠線や gizmo は描きません。

選択は engine/game の共有状態ではなく、各ツールが持つ client-session の注視対象です。engine に
Studio 専用の selection state/RPC を足さず、公開 RPC と `pelican_project` の identity だけから
任意の client が同じ状態を作れる形にしたのが D0 上の理由です。背景への成功 pick は選択を解除します。
`picking` feature が無い、RPC が失敗した、または応答を Outliner と対応付けられない場合は現在選択を
維持し、viewport の警告行、status bar、Engine Log の三つへ理由を出します。feature は自動で有効化せず、
project の purgeability を保ちます。

共通 editor 基盤は WP149〜172 で実装済みです: authoring document、typed query/edit、
CAS/journal、undo/redo、atomic save、snapshot import、watch、isolated preview。Studio の
process/window 結線は WP251、汎用 ID バッファ picking + RPC は WP262、Studio の選択同期は
WP264 で入りました。プロパティ編集、gizmo、複数 client WebSocket は未接続です。ツール自作の入口は
`pelican_project`、JSON-RPC/`pelican_rpc.py`、ImGui の三つです。

## 10.8 テスト基盤

- 単体テスト: Catch2 v3(`pelican_define_test`)。**GPU 必須テストは Vulkan デバイス列挙失敗時に `SKIP()`**。
- 結合テスト: `test/run_*.cmake` が player / cli を子プロセス起動して検証。2026-07-10 以降の追加: `run_devcli_assets`(WP66)/ `run_event_schema_compile`(WP71)/ `run_dump_lowered_material`(WP76)/ `run_devcli_gltf_extract`(WP79)/ `run_spvlink_golden`(WP80)/ `run_devcli_rules_import`(WP84)/ `run_devcli_bake_camera` + `run_input_record_replay_headless`(WP89)/ `run_ui_u2_rpc_replay`(WP93)など。
- ゴールデンイメージテスト: **49 ケース**(ディレクトリ自動発見。[第6章](06_rendering.md) §6.10)。
- ctest 非登録のスモーク: `run_build_units_smoke.cmake`(IMGUI / PHYSICS 系を含む単独 OFF ビルド検証)、`run_project_code_smoke.cmake`。
- **CI(✅WP137/165)**: push/PRのWindows CPU gateに加え、手動/週次の
  build-unit OFF/Jolt/project-code/clean-clone matrixがあります。GPU testは
  `gpu` labelで除外し、`SKIP`はexact allowlist、retryなし。GPU CI2は未。

## 関連文書

- [../external_tools_requirements.md](../external_tools_requirements.md) — 外部ツール契約(R1〜R10。JSON-RPC は R8)
- [../design_asset_hot_reload.md](../design_asset_hot_reload.md) — アセットホットリロード(v2.1・HR0〜HR2-G + targeted animation generation 実装済み)
- [../design_game_logic_native.md](../design_game_logic_native.md) — ゲームロジック(G2 DLL リロード実装済み)
- [../design_devstudio_direction.md](../design_devstudio_direction.md) — エディタの方向性(D0〜D3)
- [../design_build_tiers.md](../design_build_tiers.md) — ビルドユニット・配布
- [第2章 ビルドと起動](02_getting_started.md) / [第6章 レンダリング](06_rendering.md) / [第7章 入力と UI](07_input_ui.md) / [第8章 ゲームロジック](08_gameplay.md)
