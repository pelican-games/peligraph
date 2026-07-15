# 第2章 ビルドと起動

対象: pelican2(2026-07-10 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- エンジンをビルドするための必要環境と CMake の使い方
- `pelican_player` で example プロジェクトを起動する方法
- `pelican_cli project init` で自分の最初のプロジェクトを作って動かす手順
- ヘッドレス実行(ウィンドウなしで PNG を出力)とテストの回し方
- つまずきやすいポイント(バイナリアセット、暗黙プロジェクト、Qt パスなど)

## 2.1 必要環境

| 要件 | 内容 |
|---|---|
| OS | Windows(開発の主環境)。ビルドは MSVC + C++20 |
| Vulkan SDK | **1.4.x**(1.4.350 で検証済み。vma-hpp v3.3.0 以降、1.3.x は非対応) |
| Qt6 | Pelican Studio(エディタ)用。6.10 以上。**player だけなら不要** |
| CMake | ルートの [CMakeLists.txt](../../CMakeLists.txt) が全体を構成 |

外部ライブラリ(quill / glm / nlohmann_json / argparse / shaderc / tinyexr / miniaudio など)はすべて CMake の FetchContent で自動取得されるので、個別インストールは不要です。

## 2.2 ビルド

```sh
# 初回構成(Qt のパスは自分の環境に合わせる)
cmake . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64

# ビルド(MSVC では既定で Debug 構成)
cmake --build ./build
```

Qt を入れていない、または player だけ欲しい場合は Studio をスキップします:

```sh
cmake . -B build -DSKIP_DEVSTUDIO=ON
```

テストをスキップする場合は `-DSKIP_TEST=ON` を追加します。

> **注意:** リポジトリの [README.md](../../README.md) には値なしの `-DSKIP_DEVSTUDIO` と書かれていますが、リポジトリ内の実運用(`test/run_build_units_smoke.cmake`)は `=ON` 付きです。`=ON` を付ける書き方が確実です。

### ビルド成果物の場所

| 実行ファイル | 場所 |
|---|---|
| `pelican_player.exe` | **build ツリー内**: `build/src/player/<Config>/`(例: `build/src/player/Debug/pelican_player.exe`) |
| `pelican_cli.exe` | `dist/`(Debug 構成では `dist_debug/`) |
| `pelican_studio.exe` | `dist/`(同上) |

### 機能ユニット(PELICAN_WITH_*)

エンジンの一部機能は CMake オプションで切り離せる「ビルドユニット」になっています(✅実装済み・WP40/51)。すべて**既定 ON** なので、開発中は意識する必要はありません。配布ビルドで OFF にする方法は [第10章](10_tools.md) の `dist-config` を参照してください。

| オプション | 内容 |
|---|---|
| `PELICAN_WITH_RPC` | stdio JSON-RPC 制御サーバ |
| `PELICAN_WITH_SEQPLAYER` | `pelican.transform_seq` 再生 |
| `PELICAN_WITH_VAT` | `pelican.vat`(GLB)再生 |
| `PELICAN_WITH_EXR` | EXR 読み込み |
| `PELICAN_WITH_AUDIO` | WAV SE 再生 |
| `PELICAN_RUNTIME_SHADER_COMPILER` | 実行時 GLSL コンパイル(shaderc) |

OFF でビルドした機能を使おうとすると、黙って無視されるのではなく `This binary was built with PELICAN_WITH_X=OFF: ...` という**名指しのエラー**で止まります(fail-fast 方針。[第1章](01_overview.md) 参照)。

## 2.3 example プロジェクトを起動する

リポジトリには実プロジェクトの例 [projects/example](../../projects/example) が入っています。

```sh
build/src/player/Debug/pelican_player.exe --project projects/example
```

`--project` は**任意の作業ディレクトリから**実行できます(パス解決に cwd を使わないため)。相対パスは cwd 基準で絶対化されます。

> **注意(重要):** example の **3D モデルやテクスチャなどのバイナリアセットは git 管理されていません**。[projects/example/README.md](../../projects/example/README.md) に「ファイルパス / 入手元 / sha256 / サイズ」の一覧表があるので、記載どおりのファイルを配置してからフルシーンを起動してください。手元にバイナリが無い状態で試したい場合は、次節の `project init` から始めるのが確実です。

### 暗黙プロジェクト(`--project` 省略時)

`--project` を省略すると、player は次の順でプロジェクトを探します:

1. exe のあるディレクトリの `project.json`
2. exe のディレクトリから親をさかのぼって `projects/example/project.json`

見つかった場合も必ず WARN ログ `implicit project root = ... (pass --project to silence)` が出ます。スクリプトや CI では常に `--project` を明示するのが規約です。

## 2.4 最初の自分のプロジェクトを作る

`pelican_cli project init` が 15 ファイルの雛形を生成します(✅実装済み・WP57)。生成されるプロジェクトは**バイナリアセット不要で、生成直後にそのまま起動できます**(カメラ+ディレクショナルライトのみのシーン)。

```sh
# 1. 雛形生成(dist_debug は Debug ビルドの場合)
dist_debug/pelican_cli.exe project init mygame

# 2. 起動
build/src/player/Debug/pelican_player.exe --project mygame
```

対象ディレクトリは「存在しない(自動作成される)」か「空」である必要があります。生成物は次のとおりです:

```
mygame/
├── project.json                      # マニフェスト(下記)
├── scenes/main.scene.json            # MainCamera + KeyLight の最小シーン
├── assets/asset_data.json            # モデル登録(空)
├── assets/{models,textures,audio}/   # 置き場(.gitkeep 入り)
├── input/actions.json                # 入力アクション定義
├── passes/main_rendering_config.json # レンダリングパイプライン定義(deferred 構成)
├── ui/ui_overlay.json                # 2D オーバーレイ(空)
├── code/CMakeLists.txt               # pelican_game_sources(game.cpp)
├── code/game.cpp                     # StarterSystem(空のゲームシステム)
├── .gitattributes / .gitignore / README.md
```

生成される `project.json`(実物・`src/devcli/projectinit.cpp` が出力):

```json
{
  "schema": "pelican.project",
  "version": 1,
  "name": "pelican-project",
  "generator": "pelican_cli project init",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Pelican Project",
    "window_size": { "width": 1280, "height": 720 },
    "fullscreen": false,
    "framerate": 60,
    "camera": { "yfov": 0.7853981633974483, "znear": 0.1, "zfar": 1000, "up": [0.0, -1.0, 0.0] },
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main_rendering_config.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/ui_overlay.json",
    "input_actions_json": "input/actions.json"
  }
}
```

各フィールドの意味は [第3章](03_project_format.md) で詳しく説明します。

### モデルを表示する

1. glTF バイナリ(`.glb`)を `mygame/assets/models/` に置く
2. `assets/asset_data.json` に登録する:

```json
{
  "models": [
    { "name": "mymodel", "path": "assets/models/mymodel.glb" }
  ]
}
```

3. `scenes/main.scene.json` の `objects` 配列にオブジェクトを追加する(例):

```json
{
  "components": [
    { "name": "transform", "pos": [0, 0, 3], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
    { "name": "simplemodelview", "model": "mymodel" }
  ]
}
```

パスは常に**プロジェクトルート基準**の相対パスで書きます(JSON ファイルの場所基準ではありません)。シーン形式の詳細は [第4章](04_scene_ecs.md) を参照してください。

### ゲームコード(C++)を組み込む

ゲームロジックは C++ で書き、**player に静的リンク**します。configure 時に `-DPELICAN_PROJECT` でプロジェクトを指定すると、`code/CMakeLists.txt` が取り込まれます:

```sh
cmake . -B build -DSKIP_DEVSTUDIO=ON -DPELICAN_PROJECT=mygame
cmake --build ./build
```

`code/game.cpp` の `PELICAN_REGISTER_SYSTEM(StarterSystem, 100)` が毎フレーム呼ばれる入口です。書き方は [第8章](08_gameplay.md) を参照してください。

## 2.5 ヘッドレス実行(ウィンドウなしで PNG を出す)

CI・ゴールデンイメージテスト・DCC 連携の基盤です(✅実装済み・WP1〜7)。

```sh
# 3 フレーム描画して最終フレームを out.png に保存
pelican_player --headless --project mygame --frames 3 --size 1280x720 --render-out out.png

# %d / %04d を含むパスなら毎フレーム連番で保存
pelican_player --headless --project mygame --frames 10 --render-out out/%04d.png
```

ヘッドレス時は時刻が**固定ステップ**(dt = 1/fps、`--fps` で変更、既定 60)になり、実行が決定的になります。`--camera "px,py,pz,tx,ty,tz,fov_deg"` でカメラを上書きできます。

さらに `--rpc --headless` で JSON-RPC による外部制御モードになります([第10章](10_tools.md))。

## 2.6 テストを実行する

```sh
ctest --test-dir ./build -C Debug --output-on-failure
```

- テストは Catch2 v3 の単体テストと、実際に `pelican_player` / `pelican_cli` を子プロセス起動する CMake スクリプト駆動の結合テスト(`test/run_*.cmake`)の 2 種類です。
- **GPU 必須のテストは、Vulkan デバイスの列挙に失敗した環境では `SKIP()`** されます(テスト規約)。
- 開発全体の完了条件は常に「ビルド成功 + 全テストグリーン + `git diff --check` クリーン」です([../implementation_plan.md](../implementation_plan.md) §0)。

## 2.7 Pelican Studio を起動する

```sh
cmake --build ./build --target run_studio
```

`run_studio` ターゲットは windeployqt(Qt DLL 配置)まで面倒を見ます。

> **注意:** Studio は現状 **QML のダミー画面のみの休眠状態**です(テキストフィールド 2 個。プロジェクト読み込み・ビューポートは未実装 📐)。エディタの設計方向(D0「エディタ特権の禁止」など)は [第10章](10_tools.md) と [../design_devstudio_direction.md](../design_devstudio_direction.md) を参照してください。

## 2.8 トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| 起動直後に schema / version のエラー | project.json のエンベロープ検証(`schema: "pelican.project"`、`version: 1` ちょうど)に失敗。エラーメッセージが正しいキー名を案内します(fail-fast 方針) |
| `'fov_y' is not supported in v1; use 'yfov' (radians)` | カメラの旧キー名。`yfov`(ラジアン)/`znear`/`zfar` に書き換える |
| モデルのファイルが見つからないエラー | example のバイナリアセット未配置(§2.3)。エラーには解決後の絶対パスが含まれるので、そのパスに置く |
| `implicit project root = ...` の WARN | `--project` 未指定。明示すれば消える |
| `engine_min_version` のエラー | プロジェクトが要求するエンジン版(現行 `0.1.0`)より新しい。`--ignore-engine-version` で WARN に降格可能 |
| Qt が見つからず configure 失敗 | `-DCMAKE_PREFIX_PATH` に Qt のインストールパスを渡すか、`-DSKIP_DEVSTUDIO=ON` |
| GPU テストが全部 SKIP | Vulkan デバイスが列挙できない環境(リモートデスクトップ等)。仕様どおりの挙動 |
| rpc モードでログが出ない | rpc モードでは **stdout はプロトコル専用**。ログは cwd の `pelican.log` に出ます(Release ビルドも同様) |

## 関連文書

- [../../README.md](../../README.md) — ビルド手順の原本
- [../implementation_plan.md](../implementation_plan.md) — §0 共通規則(ビルド・テスト・コード規約)
- [../design_headless_rendering.md](../design_headless_rendering.md) — ヘッドレス描画の設計
- [../design_build_tiers.md](../design_build_tiers.md) — ビルドユニットと配布ビルド
- [第3章 プロジェクト形式](03_project_format.md) / [第10章 ツールリファレンス](10_tools.md)
