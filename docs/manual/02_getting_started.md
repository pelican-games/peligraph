# 第2章 ビルドと起動

対象: pelican2(2026-07-21 時点)/ このマニュアルはコードを正とする

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

quill / glm / nlohmann_json / argparse / tinyexr / miniaudio などは CMake の
FetchContent で自動取得されます。shaderc は例外で、Vulkan SDK 同梱版を使います。
`PELICAN_RUNTIME_SHADER_COMPILER=ON` の PC 開発ビルドには完全な Vulkan SDK が必要です。
ホスト PC で事前生成した SPIR-V を消費するモバイル等のターゲットは
`-DPELICAN_RUNTIME_SHADER_COMPILER=OFF` で shaderc をリンクしません。

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

テストとテスト専用依存をすべて外す場合は `-DBUILD_TESTING=OFF` を追加します。
旧`-DSKIP_TEST=ON`は互換aliasですが非推奨です。エンジン／ゲームの実行にPythonは
必要ありません。

テストを有効にした場合、`PELICAN_PYTHON_TESTS`は次の三値です。

- `OFF`（既定）: Pythonを探索せず、C++/CMakeテストだけを登録
- `AUTO`: Python 3があればPython製gateも登録し、なければC++/CMakeテストだけを登録
- `ON`: Python 3とPython製gateを必須化（CIの完全テスト構成）

> **注意:** リポジトリの [README.md](../../README.md) には値なしの `-DSKIP_DEVSTUDIO` と書かれていますが、リポジトリ内の実運用(`test/run_build_units_smoke.cmake`)は `=ON` 付きです。`=ON` を付ける書き方が確実です。

### ビルド成果物の場所

| 実行ファイル | 場所 |
|---|---|
| `pelican_player.exe` | **build ツリー内**: `build/src/player/<Config>/`(例: `build/src/player/Debug/pelican_player.exe`) |
| `pelican_cli.exe` | `dist/`(Debug 構成では `dist_debug/`) |
| `pelican_studio.exe` | `dist/`(同上) |

### 機能ユニット(PELICAN_WITH_*)

エンジンの一部機能は CMake オプションで切り離せる「ビルドユニット」になっています(✅実装済み・WP40/51)。production機能は**既定 ON** なので、開発中は意識する必要はありません。experimental SPIR-V linkerだけは既定OFFです。配布ビルドで OFF にする方法は [第10章](10_tools.md) の `dist-config` を参照してください。

| オプション | 内容 |
|---|---|
| `PELICAN_WITH_RPC` | stdio JSON-RPC 制御サーバ |
| `PELICAN_WITH_SEQPLAYER` | `pelican.transform_seq` 再生 |
| `PELICAN_WITH_VAT` | `pelican.vat`(GLB)再生 |
| `PELICAN_WITH_EXR` | EXR 読み込み |
| `PELICAN_WITH_AUDIO` | WAV SE 再生 |
| `PELICAN_WITH_IMGUI` | ImGui 開発者 UI(F1 トグル・Plan Viewer)。配布では dist-config が常時 OFF([第10章](10_tools.md)) |
| `PELICAN_WITH_OPENXR` | OpenXR(PCVR)ランタイム。開発既定 ON・**配布は `dist-config --with openxr` を指定した時だけ ON** |
| `PELICAN_WITH_RENDERDOC` | 注入済みRenderDocの受動検出とF11/RPC capture。binary/import libraryはリンクせず、配布では常にOFF |
| `PELICAN_WITH_PHYSICS` | 物理クエリ層。配下に `PELICAN_WITH_BUILTIN_PHYSICS`(既定 ON)/ `PELICAN_WITH_JOLT_PHYSICS`(既定 OFF)のプロバイダ選択 |
| `PELICAN_WITH_STANDARD_RENDER_ALGORITHMS` | 交換可能な標準描画algorithm asset集。既定ON。OFFでもgraph/compiler機構は残り、project shaderで置換できる |
| `PELICAN_RUNTIME_SHADER_COMPILER` | 実行時 GLSL コンパイル(Vulkan SDK版shaderc)。OFFでは事前生成SPIR-Vだけを消費 |
| `PELICAN_WITH_SPIRV_LINK` | experimental SPIR-V linker、pinned SPIRV-Tools、`pelican-spv-link` CLI（**既定 OFF**） |

OFF でビルドした機能を使おうとすると、黙って無視されるのではなく `This binary was built with PELICAN_WITH_X=OFF: ...` という**名指しのエラー**で止まります(fail-fast 方針。[第1章](01_overview.md) 参照)。
`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS`だけは機構ではなく既定実装のasset packageなので、
OFFでも対応feature自体は利用できます。その場合はfeatureのshader asset parameterへ
`project://`実装を明示し、存在しない既定`engine://render_algorithms/...`を選ばないでください。

ビルドユニットではありませんが、同じ [CMakeLists.txt](../../CMakeLists.txt) には次のオプションもあります:

| オプション | 既定 | 内容 |
|---|---|---|
| `PELICAN_ENABLE_ASAN` | OFF | AddressSanitizer 付きでビルドする(テストビルド想定) |
| `PELICAN_PROJECT` | 空(CACHE PATH) | `code/` をホットリロード可能なゲーム DLL としてビルドする対象プロジェクト(§2.4) |
| `BUILD_TESTING` | ON | OFFでCatch2を含む全テストとテスト専用toolingを構成対象から除外 |
| `PELICAN_PYTHON_TESTS` | OFF | Python製test gateを`OFF` / `AUTO` / `ON`で登録 |

`PELICAN_SPV_LINK=experimental`は実行時のbackend選択です。build時に
`PELICAN_WITH_SPIRV_LINK=ON`を指定していないbinaryでは利用できず、名指しの診断を返します。
ON時のpinned SPIRV-Tools生成処理にはPython 3が必要です。

## 2.3 example プロジェクトを起動する

リポジトリには実プロジェクトの例 [projects/example](../../projects/example) が入っています。

```sh
build/src/player/Debug/pelican_player.exe --project projects/example
```

`--project` は**任意の作業ディレクトリから**実行できます(パス解決に cwd を使わないため)。相対パスは cwd 基準で絶対化されます。

> **注意(重要):** example の **3D モデルやテクスチャなどのバイナリアセットは git 管理されていません**。「ファイル / サイズ / SHA-256 / 出所 / ライセンス」の一覧表は [../example_assets.md](../example_assets.md) にあるので、記載どおりのファイルを配置してからフルシーンを起動してください。

アセットストアの解決先と、まだ供給されていないファイルは、マニフェスト対応のコマンドで確認できます(手順の正本は [projects/example/README.md](../../projects/example/README.md)):

```sh
pelican_cli assets status --project projects/example
pelican_cli assets verify --project projects/example
```

手元にバイナリが無い状態で試したい場合は、次節の `project init` から始めるのが確実です。

example のほかに、機能別のデモプロジェクトが 3 つあります(いずれも同様に `--project` で起動):

- [projects/sprite_demo](../../projects/sprite_demo) — 2D 横スクロールの vertical slice(スプライト・pixel perfect・`moveAndSlide`・ゲームパッド)
- [projects/animgraph_demo](../../projects/animgraph_demo) — アニメーショングラフ(歩き↔走りブレンド + ジャンプ割込み)
- [projects/vrm_xr_demo](../../projects/vrm_xr_demo) — VRM キャラクター(表情・視線・一人称)+ OpenXR。**flat-first** — XR なしでも WASD で完全動作(下記)

### XR モードで起動する(PCVR / Quest Link)✅WP125〜138

```sh
pelican_player --xr on --project projects/vrm_xr_demo --input-profile touch
```

- `--xr off|auto|on`(既定 `off`)。`auto` = XR ランタイム不在なら INFO 1 行で flat 続行 / `on` = 不在なら名指しの hard error。
- **決定的な駆動モードとの併用時は XR が強制 off** になります。判定は最も具体的なドライバを名指しする順序で、**rpc → golden → replay → headless**([xractivation.hpp](../../src/core/xractivation.hpp) の `xrForcedOffDriver()`)。`--xr on` なら `--xr on is incompatible with <driver>` の hard error、`--xr auto` なら黙って flat に正規化されます(この経路で INFO が出るのは `PELICAN_WITH_OPENXR=OFF` ビルドのときだけです)。
- Quest 3 は Meta Quest Link(Air Link)を有効な OpenXR ランタイムにして接続してから実行します。XR 中もウィンドウには左眼のミラーが表示されます。
- project.json 側に XR のキーはありません(有効化は CLI のみ。入力はいつもの `pelican.input_actions` + Touch 用プロファイル)。
- Meta XR Simulator v201.0 の実 runtime では1000連続 frame、3分超、
  mirror 1000/1000、Vulkan validation 0を確認済みです(WP136 の検証で見つかった
  blocker 3 件 — XR session 中は ImGui frame を begin しない / `timelineSemaphore`
  の常時有効化 / XR 診断ログの規範化 — は WP138 で修正済み。レポートは
  [docs/design_reviews/](../design_reviews/))。⚠ Quest Linkを
  含む物理HMDでの表示・入力確認はまだ行われていません。詳細は
  [第6章](06_rendering.md) §6.12。

### 暗黙プロジェクト(`--project` 省略時)

`--project` を省略すると、player は次の順でプロジェクトを探します:

1. exe のあるディレクトリの `project.json`
2. exe のディレクトリから親をさかのぼって `projects/example/project.json`

見つかった場合も必ず WARN ログ `implicit project root = ... (pass --project to silence)` が出ます。スクリプトや CI では常に `--project` を明示するのが規約です。

## 2.4 最初の自分のプロジェクトを作る

`pelican_cli project init` が雛形一式を生成します(✅実装済み・WP57)。生成されるプロジェクトは**バイナリアセット不要で、生成直後にそのまま起動できます**(カメラ+ディレクショナルライトのみのシーン)。

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
├── assets/{models,textures,audio}/   # 置き場(assets/ 自身を含め .gitkeep 入り)
├── input/actions.json                # 入力アクション定義
├── input/profiles/keyboard.json      # バインディングプロファイル(第7章)
├── passes/main_rendering_config.json # レンダリングパイプライン定義(deferred 構成)
├── ui/ui_overlay.json                # 2D オーバーレイ(空)
├── code/CMakeLists.txt               # pelican_game_sources(game.cpp)
├── code/game.cpp                     # StarterSystem(空のゲームシステム)
├── .gitattributes / .gitignore / README.md
```

生成されるファイルは全 16 エントリで、一覧は [projectinit.cpp](../../src/devcli/projectinit.cpp) の `templateFiles()` が正本です。

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
    "input_actions_json": "input/actions.json",
    "input_profiles": { "keyboard": "input/profiles/keyboard.json" },
    "input_profile": "keyboard"
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

ゲームロジックは C++ で書きます。configure 時に `-DPELICAN_PROJECT` でプロジェクトを指定すると、`code/CMakeLists.txt` が取り込まれ、**`pelican_game_logic.dll` としてビルド**されます(✅WP90 で静的リンクから移行 — 実行中に **F5 でホットリロード**できます。[第10章](10_tools.md) §10.5):

```sh
cmake . -B build -DSKIP_DEVSTUDIO=ON -DPELICAN_PROJECT=mygame
cmake --build ./build
```

`code/game.cpp` の `PELICAN_REGISTER_SYSTEM(StarterSystem, 100)` が毎フレーム呼ばれる入口です。書き方は [第8章](08_gameplay.md) を参照してください。

同じ DLL には、**オブジェクト単位の毎フレーム処理**である behavior(`PELICAN_REGISTER_BEHAVIOR`)も載ります(✅WP155/162/167)。シーン側は対象オブジェクトに `{ "name": "behavior", "type": "<登録名>" }` を書くだけで、実体は DLL の登録名で解決されます。BehaviorSystem はゲームシステムと同じ総順序に `order=50` で参加します。書き方は [第8章](08_gameplay.md)、JSON 側の語彙は [第4章](04_scene_ecs.md) を参照してください。

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

- テストは Catch2 v3 の単体テストと、実際に `pelican_player` / `pelican_cli` を子プロセス起動する CMake / PowerShell スクリプト駆動の結合テスト(`test/run_*.cmake` / `test/run_*.ps1`)の 2 種類です。
- **GPU 必須のテストは、Vulkan デバイスの列挙に失敗した環境では `SKIP()`** されます(テスト規約)。
- 開発全体の完了条件は常に「ビルド成功 + 全テストグリーン + `git diff --check` クリーン」です([../implementation_plan.md](../implementation_plan.md) §0)。

## 2.7 Pelican Studio を起動する

```sh
cmake --build ./build --target run_studio
```

`run_studio` ターゲットは windeployqt(Qt DLL 配置)まで面倒を見ます。

> **注意:** Qt Studio 自体は現状 **QML のダミー画面のみ**です。
> 一方、engine側の typed editor service/RPC、Object Tree、Inspector、
> undo/save/preview は実装済みで、通常playerのImGuiから利用できます。
> Qtのプロジェクト読み込み・埋め込みviewport・gizmoは未実装です。
> エディタの設計方向(D0「エディタ特権の禁止」など)は [第10章](10_tools.md) §10.7 と
> [../design_devstudio_direction.md](../design_devstudio_direction.md) /
> [../design_editor_tooling.md](../design_editor_tooling.md) を参照してください。

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
| rpc モードでログが出ない | rpc モードでは **stdout はプロトコル専用**(ヘッドレスの blocking rpc / ウィンドウのフレーム境界 rpc のどちらも同じ)。ログは cwd の `pelican.log` に出ます(Release ビルドも同様) |
| `windowed rpc request queue is busy`(`data.reason = "busy"` / `queue_capacity`) | ウィンドウモードの `--rpc` はフレーム境界でのみ処理する**有界キュー**(既定 64)。詰め込みすぎると即エラーで返るので、応答を待ってから次のリクエストを送る([第10章](10_tools.md)) |
| `--xr on is incompatible with <driver>` | `--xr on` と決定的な駆動モード(rpc / golden / replay / headless)の併用。`--xr auto` にすれば(ログなしで)flat に正規化されます(§2.3) |
| `Unknown behavior type 'X' on object 'Y'` | シーンの `behavior.type` がゲームロジック DLL に登録されていない。`PELICAN_REGISTER_BEHAVIOR` の登録名と綴りを合わせる([第8章](08_gameplay.md)) |
| `behavior type 'X' on object 'Y' is pending because the game-logic DLL is unavailable`(WARNING) | DLL 未ロード時は hard error にせず pending として保持する仕様。`-DPELICAN_PROJECT` 付きでビルドするか、F5 でリロードすると解決します |
| `behavior on object 'X' requires a non-empty string type` | `behavior` コンポーネントに `type`(非空の文字列)が無い、または文字列以外 |

## 関連文書

- [../../README.md](../../README.md) — ビルド手順の原本
- [../implementation_plan.md](../implementation_plan.md) — §0 共通規則(ビルド・テスト・コード規約)
- [../design_headless_rendering.md](../design_headless_rendering.md) — ヘッドレス描画の設計
- [../design_build_tiers.md](../design_build_tiers.md) — ビルドユニットと配布ビルド
- [第3章 プロジェクト形式](03_project_format.md) / [第10章 ツールリファレンス](10_tools.md)
