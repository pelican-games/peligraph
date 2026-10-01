# 第2章 ビルドと起動

対象: pelican2(HEAD=`82054c05`・2026-09-17。2026-07-21 の全域調査に、v0 の段(`PELICAN_WITH_ANIMATION` / `requires` / `--start-time` / package)を 2026-09-17 に反映)/ このマニュアルはコードを正とする

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

この表とこの下に出る外部名を 1 行ずつ: **GLSL**(OpenGL Shading Language)は本リポジトリでシェーダを書くソース言語、**SPIR-V** は Vulkan が実際に読むシェーダの中間バイナリ形式([用語集](14_glossary.md))、**shaderc** はその GLSL を SPIR-V へ変換するコンパイラライブラリ([shadercompiler.cpp](../../src/core/shader/shadercompiler.cpp#L22))、**vma-hpp** は GPU メモリ確保ライブラリ VulkanMemoryAllocator の C++ バインディング([VulkanMemoryAllocator-Hpp](../../src/core/CMakeLists.txt#L74))です。

vma-hpp が v3.3.0 以降でなければならないのは、SDK 1.4 系で vulkan.hpp の例外クラスが
改名され(`ValidationFailedEXTError` → `ValidationFailedError`)、その版から vma-hpp が例外送出を
vulkan.hpp 側へ委譲して改名の影響を受けなくなったためです([src/core/CMakeLists.txt](../../src/core/CMakeLists.txt#L68) のコメントが理由の正)。

quill / glm / nlohmann_json / argparse / tinyexr / miniaudio などは CMake の
FetchContent で自動取得されます。shaderc は例外で、Vulkan SDK 同梱版を使います。
`PELICAN_RUNTIME_SHADER_COMPILER` は「**実行時に GLSL を SPIR-V へコンパイルする shaderc を組み込むか**」の
CMake フラグです。`ON` の PC 開発ビルドには完全な Vulkan SDK が必要です(無いと configure が名指しで止まります)。
ホスト PC で事前生成した SPIR-V を消費するモバイル等のターゲットは
`-DPELICAN_RUNTIME_SHADER_COMPILER=OFF` で shaderc をリンクしません。

## 2.2 ビルド

```sh
# 初回構成(Qt のパスは自分の環境に合わせる)
cmake . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64

# ビルド
cmake --build ./build --config Debug
```

Qt を入れていない、または player だけ欲しい場合は Studio をスキップします:

```sh
cmake . -B build -DSKIP_DEVSTUDIO=ON
```

テストとテスト専用依存をすべて外す場合は `-DBUILD_TESTING=OFF` を追加します。
旧`-DSKIP_TEST=ON`は互換aliasですが非推奨です。エンジン／ゲームの実行にPythonは
必要ありません。

テストを有効にした場合、`PELICAN_PYTHON_TESTS`は次の三値です。

- `OFF`（既定）: Python製テスト用の探索をせず、C++/CMakeテストだけを登録
- `AUTO`: Python 3.11以上があればPython製gateも登録し、なければC++/CMakeテストだけを登録
- `ON`: Python 3.11以上とPython製gateを必須化（CIの完全テスト構成）

Studioの実窓シナリオもこの設定を使います。完全なGPU/release gateではStudioを有効にし、
`PELICAN_PYTHON_TESTS=ON`を指定します。

この設定は依存ライブラリの生成処理とは独立です。OpenXR-SDK-Sourceのdispatch header生成と
experimental SPIR-V linkerはPythonを必要とする場合があります。Pythonなしでconfigureする場合は
`-DPELICAN_WITH_OPENXR=OFF -DPELICAN_WITH_SPIRV_LINK=OFF`も指定してください。

### ビルド成果物の場所

| 実行ファイル | 場所 |
|---|---|
| `pelican_player.exe` | `build/dist/<Config>/`(例: `build/dist/Debug/pelican_player.exe`) |
| `pelican_cli.exe` | `build/dist/<Config>/` |
| `pelican_studio.exe` | `build/dist/<Config>/` |

各 build ツリーの中に実行物を集めるため、別の構成をビルドしても同じ source tree の
実行ファイルを上書きしません。`PELICAN_OUTPUT_ROOT` で `build/dist` 部分を変更できます。
`PELICAN_PROJECT` で作るゲーム DLL も player と同じディレクトリに置きます。

Windows 用の配布ディレクトリは次の手順で作れます。`Runtime` component は実行物と
Qt が選んだ runtime/plugin を配置し、engine/Studio のリソースはバイナリに埋め込まれています。

```sh
cmake -S . -B build-release -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64
cmake --build build-release --config Release --target pelican_runtime
cmake --install build-release --config Release --component Runtime --prefix "$PWD/build-release/package"
```

Qt の配布処理は絶対パスを要求するため、上のように `$PWD` を含めて指定します。
結果は `build-release/package/bin/` です。プロジェクトとそのアセットは別途用意し、
`--project` で指定します。ソース shader を使う v0 プロジェクトは runtime shader compiler を
ON のまま配布します。OFF は必要な shader variant を全て事前生成したプロジェクト用です。

### 機能ユニット(PELICAN_WITH_*)

エンジンの一部機能は CMake オプションで切り離せる「ビルドユニット」になっています(✅実装済み・WP40/51)。**ビルドユニット** = パージ可能なサブシステムを CMake オプション 1 個の単位にしたもので、OFF にするとそのコードはリンクされず、使おうとすると名指しのエラーになります([../design_build_tiers.md](../design_build_tiers.md))。主要機能は既定 ON ですが、Jolt プロバイダや experimental SPIR-V linker などは既定 OFF です。正確な既定値と連動条件は [ビルド機能の台帳](../build_features.md) を参照してください。配布ビルドで OFF にする方法は [第10章](10_tools.md) の `dist-config` を参照してください。

| オプション | 内容 |
|---|---|
| `PELICAN_WITH_RPC` | stdio JSON-RPC 制御サーバ |
| `PELICAN_WITH_SEQPLAYER` | `pelican.transform_seq` 再生 |
| `PELICAN_WITH_VAT` | `pelican.vat`(GLB)再生 |
| `PELICAN_WITH_EXR` | EXR 読み込み |
| `PELICAN_WITH_AUDIO` | 音の再生(WAV / Ogg Vorbis、se / bgm バス。miniaudio と同梱の stb_vorbis はこの中だけで有効)。⚠️ feature registry の DESCRIPTION は「WAV sound-effect」のまま([pelican_feature_registry.cmake](../../cmake/pelican_feature_registry.cmake#L122)) |
| `PELICAN_WITH_ANIMATION` | スケルタルアニメーションの評価器と `pelican.pose` グラフ module(✅AG-1a。[pelican_feature_registry.cmake:146](../../cmake/pelican_feature_registry.cmake#L149)、`SMOKE_ID animation`) |
| `PELICAN_WITH_IMGUI` | ImGui 開発者 UI(F1 トグル・Plan Viewer)。配布では dist-config が常時 OFF([第10章](10_tools.md)) |
| `PELICAN_WITH_OPENXR` | OpenXR(PCVR)ランタイム。開発既定 ON・**配布は `dist-config --with openxr` を指定した時だけ ON** |
| `PELICAN_WITH_RENDERDOC` | 注入済みRenderDocの受動検出とF11/RPC capture。binary/import libraryはリンクせず、配布では常にOFF |
| `PELICAN_WITH_PHYSICS` | 物理クエリ層。配下に `PELICAN_WITH_BUILTIN_PHYSICS`(既定 ON)/ `PELICAN_WITH_JOLT_PHYSICS`(既定 OFF)のプロバイダ選択 |
| `PELICAN_WITH_STANDARD_RENDER_ALGORITHMS` | 交換可能な標準描画algorithm package。既定ON。OFFでshader assetと所属C++ policy/providerを除外し、graph/compiler機構は残す |
| `PELICAN_RUNTIME_SHADER_COMPILER` | 実行時 GLSL コンパイル(Vulkan SDK版shaderc)。OFFでは事前生成SPIR-Vだけを消費 |
| `PELICAN_WITH_SPIRV_LINK` | experimental SPIR-V linker、pinned SPIRV-Tools、`pelican-spv-link` CLI（**既定 OFF**） |

OFF でビルドした機能を使おうとすると、黙って無視されるのではなく `This binary was built with PELICAN_WITH_X=OFF: ...` という**名指しのエラー**で止まります(fail-fast 方針。[第1章](01_overview.md) 参照)。
ゲームコードの呼び出し(`ctx.audio()` / `ctx.physics()` / `ctx.animation()`)が外した build で何を返すか(例外の型と `code()`、animation の `Status`、物理 provider が 0 本のとき)は [第8章 §8.4「機能を外した build」](08_gameplay.md) の表にまとめてあります。
`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS`だけは機構ではなく既定実装packageなので、
OFFでも対応feature自体は利用できます。その場合はfeatureのshader asset parameterへ
`project://`実装を明示し、存在しない既定`engine://render_algorithms/...`を選ばないでください。
planar reflectionでは標準camera providerも除外されるため、caller-authored
`$reflection/planar` ViewFamilyまたはsource buildしたhost providerも必要です。汎用ViewFamily
resolver、graph compiler、scheduler、backendは残ります。

#### `PELICAN_WITH_ANIMATION=OFF` のときに何が消えるか(✅AG-1a/AG-1b/P-2b)

ビルドユニットの「消え方」を 1 つ具体的に見ておきます。`-DPELICAN_WITH_ANIMATION=OFF` にすると:

- **消えるもの**: `src/core/animation/*` のソース一式(clip サンプラ・state machine 評価器 `EvaluatorV1`・`AnimationSystem`・pose graph の runtime。[src/core/animation/CMakeLists.txt:1](../../src/core/animation/CMakeLists.txt#L1))と、共通グラフの `pelican.pose` module(登録呼び出し自体が `#if` の中 — [makeBuiltinGraphModuleRegistry()](../../src/project/graphmodule.cpp#L563))。`pelican_cli graph-catalog` は 4 module 27 op ではなく **3 module 21 op** を返します(`PELICAN_WITH_AUDIO=ON` のとき。`PELICAN_WITH_AUDIO=OFF` では `pelican.audio` module も登録されません。[../v0_freeze.md](../v0_freeze.md) §1.3)。
- **残るもの**: 凍結ヘッダ(`abi_v1.hpp` / `pose_staging_v1.hpp`)と、その**交渉入口**(`getApiV1` / `getPoseStagingServiceV1` / `Vrm::getApplicationServiceV1`)。ゲーム DLL はこれらをリンクするので、入口は残したまま **`unsupported_version` を返す stub** に差し替わります([animation_disabled.cpp:13](../../src/core/animation/animation_disabled.cpp#L13))。「関数が無くてロードできない」ではなく「名前で『この build には無い』と答える」形です。**skinning の compute 側(palette / `skinned.vert` / `ray_deform.comp`)も残ります** —— 姿勢を受け取る側であって、姿勢を作る側ではないからです。
- **使おうとしたときの見え方**: `animation` コンポーネントを持つシーンは scene load の prepare で `animation.module_unavailable@1`(→ トランザクションなので旧シーンが保たれます。検査は [`resolvedObjectFeatureRefusal()`](../../src/core/loader/resolvedscene.cpp#L78)・[第4章 §4.3](04_scene_ecs.md))。エディタでこのコンポーネントを足す(`add_component` / `spawn`)ときも同じ関数・同じ名前で拒否されます(✅PG-1)。`pose.*` を書いた共通グラフ文書は `graph.operation_unregistered@1`。`project.json` に `requires: ["animation"]` を書いてあるプロジェクトは、**そこまで行く前に** project の parse で止まります(下記)。

**project 側からの宣言(`requires`)。** `project.json` に `"requires": ["animation"]` と書いておくと、OFF build でそのプロジェクトを開いた時点で `animation.module_unavailable@1: this build has PELICAN_WITH_ANIMATION=OFF, so project.json's requires: ["animation"] cannot be satisfied` を出して `pelican_player` が exit 1 します(ノードを 1 つずつ拒否させて原因を探させない、という判断です)。出荷では `animgraph_demo` / `vrm_xr_demo` / `posegraph_demo` の 3 つが宣言しています(`requires` の語は `animation` のほかに `audio` / `physics` / `xr` / `vat` / `seqplayer` があり、`sprite_demo` は `physics` を宣言しています。✅PG-1)。同じ値を**コーパスを順に調べる側**も読むので、OFF build のスモークはそれらを止まらずに名前付きで飛ばします:

```
skip posegraph_demo: requires animation (PELICAN_WITH_ANIMATION=OFF)
```

この 1 行は宣言したプロジェクトごとに出るので、OFF build では `animgraph_demo` / `posegraph_demo` /
`vrm_xr_demo` の **3 行**が並びます([プロジェクトの skip 出力](../../test/run_project_catalog_smoke.cmake#L108))。
飛ばされたプロジェクトがエディタ経路のレーンを持っていた場合は `skip editor lane: <project> requires …` が続き
([editor lane の skip 出力](../../test/run_project_catalog_smoke.cmake#L296))、最後に「何件中何件を飛ばしたか」の要約が 1 行出ます。

語彙・検証規則は [第3章 §3.2](03_project_format.md) の「機能ユニットの要求(`requires`)」にあります。

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
build/dist/Debug/pelican_player.exe --project projects/example
```

`--project` は**任意の作業ディレクトリから**実行できます(パス解決に cwd を使わないため)。相対パスは cwd 基準で絶対化されます。

> **注意(重要):** example の **3D モデルやテクスチャなどのバイナリアセットは git 管理されていません**。理由は、これらが showcase 用であって clean clone・CPU CI・ビルドユニットのスモークテストの入力ではないこと、そして権利と出所を台帳で管理するためです(リポジトリにも Git LFS にも実バイナリを足さない規約 — [../example_assets.md](../example_assets.md) 冒頭)。「ファイル / サイズ / SHA-256 / 出所 / ライセンス」の一覧表は [../example_assets.md](../example_assets.md) にあるので、記載どおりのファイルを配置してからフルシーンを起動してください。

アセットストアの解決先と、まだ供給されていないファイルは、マニフェスト対応のコマンドで確認できます(手順の正本は [projects/example/README.md](../../projects/example/README.md)):

```sh
pelican_cli assets status --project projects/example
pelican_cli assets verify --project projects/example
```

手元にバイナリが無い状態で試したい場合は、次節の `project init` から始めるのが確実です。

example のほかに、機能別のデモプロジェクトが 6 つあります(いずれも同様に `--project` で起動):

- [projects/sprite_demo](../../projects/sprite_demo) — 2D 横スクロールの vertical slice(スプライト・pixel perfect・`moveAndSlide`・ゲームパッド)
- [projects/animgraph_demo](../../projects/animgraph_demo) — アニメーショングラフ(歩き↔走りブレンド + ジャンプ割込み)
- [projects/posegraph_demo](../../projects/posegraph_demo) — 共通グラフの姿勢(`pose.clip_sample` 2 本を曲線で blend。code 無しのデータだけの project。`requires: ["animation"]`)
- [projects/vrm_xr_demo](../../projects/vrm_xr_demo) — VRM キャラクター(表情・視線・一人称)+ OpenXR。**flat-first** — XR なしでも WASD で完全動作(下記)
- [projects/raytracing_demo](../../projects/raytracing_demo) — 手続き幾何のレイトレーシング(intersection shader で球を解く。Vulkan ray tracing 対応 GPU が要る)
- [projects/pathtrace_demo](../../projects/pathtrace_demo) — frame をまたいで蓄積する path tracer。Principled BSDF(ガラス・金属・coat)、scene の camera(透視 / 正射影)、glTF の mesh。engine を変えずに project 側の shader だけで組んである。設計と検証は [design_pathtracing.md](../design_pathtracing.md)(Vulkan ray tracing 対応 GPU が要る)

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
# 1. 雛形生成(Debug ビルドの場合)
build/dist/Debug/pelican_cli.exe project init mygame

# 2. 起動
build/dist/Debug/pelican_player.exe --project mygame
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
├── passes/main_rendering_config.json # レンダリングパイプライン定義(`pelican.graph` root)
├── ui/ui_overlay.json                # 2D オーバーレイ(空)
├── code/CMakeLists.txt               # pelican_game_sources(game.cpp) + pelican_packages()
├── code/game.cpp                     # StarterSystem(空のゲームシステム)
├── .gitattributes / .gitignore / README.md
```

生成されるファイルは全 16 エントリで、一覧は [projecttemplate.cpp](../../src/project/projecttemplate.cpp) の `templateFiles()` が正本です(F-2 で `pelican_cli` から `pelican_project` へ移り、Studio の File > New Project... も同じ関数で書きます)。2 点だけ、生成物の形に約束があります(✅P-2a-4b / X-1):

- **`passes/main_rendering_config.json` は `pelican.graph` root で生成されます** —— 最上位が `{"schema": "pelican.graph", "version": 1, "definitions": {}, "nodes": [], "outputs": {}, "rendering": { … }}` で、pipeline preset と features は `rendering` 節の中です([renderingConfigJson()](../../src/project/projecttemplate.cpp#L262) は graph root をそのまま書いた文字列を返します。包み関数 `wrapLegacyRenderingConfig` は MG-2b で撤去されました)。rendering config の root はこの形だけで、rendering のキーを最上位に直接並べた旧い形は `render_config.legacy_root@1` で拒否されます([第3章 §3.2](03_project_format.md))。
- **`code/CMakeLists.txt` は 2 行**です —— `pelican_game_sources(game.cpp)` の**後に** `pelican_packages()`([projecttemplate.cpp:276](../../src/project/projecttemplate.cpp#L276))。`pelican_packages()` は `packages/*/` に複製済みで code を持つ package を build してゲームロジック DLL に合流させるので、**順序が意味を持ちます**(合流先の DLL が先に定義されている必要があります)。package を使わないうちは何もしません。

生成される `project.json`(実物・`src/project/projecttemplate.cpp` が出力):

```json
{
  "schema": "pelican.project",
  "version": 1,
  "name": "pelican-project",
  "generator": "pelican_cli project init",
  "engine_min_version": "0.1.0",
  "saved_by_engine": "0.1.0",
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

**描画パイプラインを 0 から組みたいときは `--template blank`** です(✅F-2。既定は上の `standard`)。
`pelican_cli project init mygame --template blank` は上と同じ 16 エントリを書きますが、
`passes/main_rendering_config.json` の `rendering` 節は preset も features も `render_targets` も持たず、
fullscreen pass `present` 1 本が `display` に UV の gradient を描くだけです。
`present` の fragment は engine 同梱の `engine://passthrough` なので、雛形は shader を 1 本も書きません(`shaders/` もありません。✅VI-2)。
この shader は入力が無ければ gradient を描き、`present` に入力を足すとその 0 番目をそのまま出します。
起動すると画面全体に gradient が出ます。
Studio からは File > New Project... で同じ雛形を選べます([第13章](13_editor.md)「Studio で新しい project を作る」)。
オプションの詳細は [第10章 §10.4](10_tools.md) にあります。

### モデルを表示する

1. glTF バイナリ(`.glb`)を `mygame/assets/models/` に置く
2. `assets/asset_data.json` に登録する:

```json
{
  "schema": "pelican.asset_data",
  "version": 2,
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

ヘッドレス時は時刻が**固定ステップ**(dt = 1/fps、`--fps` で変更、既定 60)になり、実行が決定的になります。固定ステップとは「壁時計を読まず、1 フレームあたりの進み幅を定数にする」ことで、実行速度やマシン性能が結果に混ざらないため、同じ入力からは同じフレーム列が出ます([第1章 §1.0](01_overview.md) の「決定的」)。`--camera "px,py,pz,tx,ty,tz,fov_deg"` でカメラを上書きできます。

### 時刻 t から出力する(`--start-time`)✅O-1

「2 秒時点の絵が欲しい」ときは `--start-time <秒>` を使います(**ヘッドレス専用**):

```sh
# t=2.0 秒の frame から 2 枚出す(--fps 60 なら 0001 が通常 run の 0120、0002 が 0121 と同じ絵)
pelican_player --headless --project mygame --start-time 2.0 --frames 2 --fps 60 --render-out out/%04d.png
```

- 秒はコマンドラインで **1 回だけ frame 番号に量子化**されます(`llround(秒 × fps)`。エンジンには「フレームの間の時刻」が渡りません — [parseStartFrame()](../../src/player/main.cpp#L326))。
- **飛ばすのではなく、そこまで回します。** `start_frame − 1` 枚を capture 無しで実行(warm-up)し、**最初に capture される frame が t の frame** です([loop.cpp:441](../../src/core/appflow/loop.cpp#L672))。グラフの時計・ゲームシステムの積分・アニメーションの評価器は時刻から逆算できないので、「その時刻に居た」と言える唯一の方法が全ステップを通ることだからです。`--start-time 0` は通常の run と同一です。
- 実行時に stderr へ 1 行出ます: `offline: start_time=2 start_frame=120 replay=119 frames=2`。
- **`--frames` とは併用できます**(枚数の指定)。**併用できないのは** `--rpc` / `--replay` / `--record-input` / `--bake-camera-output` の 4 つで、これらは「どの frame を出すか」を自分で決める経路だからです。
- 名前付きの拒否は 3 つ: `offline.time_invalid@1`(負・非有限・数でない)/ `offline.headless_required@1`(`--headless` 無し)/ `offline.mode_conflict@1`(上の 4 フラグとの併用)。いずれも**コマンドライン解析の時点**で決まり、GPU まで届きません。

さらに `--rpc --headless` で JSON-RPC による外部制御モードになります([第10章](10_tools.md))。

## 2.5b package を 1 つ入れて試す(✅X-1)

外から持ち込んだ部品(feature / グラフ / code)を使う最短手順です。package は **project に複製してから**使います(engine は project の外を探しません。形式と規則は [第3章 §3.11](03_project_format.md)):

```sh
# 1. 雛形を本体ツリーの外へ複製する(package は repo の外にあってよい)
cp -r test/fixtures/package_template /tmp/glowkit

# 2. project へ install(= packages/<namespace>/ への複製 + project.json への記録)
build/dist/Debug/pelican_cli.exe package install /tmp/glowkit --project mygame

# 3. 入ったか確認する(sha256 を再計算して照合します)
build/dist/Debug/pelican_cli.exe package list --project mygame
```

install が終わると `mygame/packages/acme.glowkit/` ができ、`project.json` に `{"namespace": "acme.glowkit", "package_version": …, "sha256": …}` が 1 行増えます。package が提供する feature は **`<namespace>.<名前>`**(例 `acme.glowkit.glow`)という名前で RPC `list_render_features` のカタログに `source: "package"` として現れ、`project://packages/acme.glowkit/features/glow.json` で参照できます。使うには rendering config の `features[]` に足します([第6章](06_rendering.md))。

複製元(`/tmp/glowkit`)を消してもプロジェクトは動きます。取り消しは `uninstall_package`(RPC)で、どこかがまだ参照していれば `package_in_use` で**何も消さずに**止まります。

## 2.6 テストを実行する

```sh
ctest --test-dir ./build -C Debug --output-on-failure
```

- テストは Catch2 v3 の単体テストと、実際に `pelican_player` / `pelican_cli` を子プロセス起動する CMake / PowerShell スクリプト駆動の結合テスト(`test/run_*.cmake` / `test/run_*.ps1`)の 2 種類です。
- **GPU 必須のテストは、Vulkan デバイスの列挙に失敗した環境では `SKIP()`** されます(テスト規約)。
- 変更時は必要なbuildと影響範囲のテスト、`git diff --check`、文書参照を確認します。全数を毎回繰り返す運用ではありません([../implementation_plan.md](../implementation_plan.md) §0)。releaseでは[CI運用](../ci.md)のGPU/Studio gateと[v0凍結一覧](../v0_freeze.md)の出荷確認を使います。

## 2.7 Pelican Studio を起動する

```sh
cmake --build ./build --config Debug --target run_studio
```

`run_studio` ターゲットは windeployqt(Qt DLL 配置)まで面倒を見ます。

> **注意:** Qt Studio は Widgets shell から別 process の player を native viewport に埋め込み、
> Outliner 選択、schema-driven Inspector、undo/redo、保存、Frame Plan、Move / Rotate / Scale
> gizmo のドラッグまで利用できます。**gizmo と picking は Studio が player 起動時に
> `engine://features/editor.json` overlay を必ず重ねるので、project 側の記述は要りません。**
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
| `project.engine_too_old@1`(`engine_min_version` のエラー) | プロジェクトが要求するエンジン版(現行 `0.1.0`)より新しい。`--ignore-engine-version` で WARN に降格可能 |
| `project.migration_required@1` | 前の版のエンジンで作った文書がある。`pelican_cli project migrate --project <dir>` で移してから開き直す([第3章 §3.13](03_project_format.md))。v0 のエンジンは移行の手順を持たないので、v0 では出ません |
| Qt が見つからず configure 失敗 | `-DCMAKE_PREFIX_PATH` に Qt のインストールパスを渡すか、`-DSKIP_DEVSTUDIO=ON` |
| GPU テストが全部 SKIP | Vulkan デバイスが列挙できない環境(リモートデスクトップ等)。仕様どおりの挙動 |
| rpc モードでログが出ない | rpc モードでは **stdout はプロトコル専用** —— JSON-RPC の応答行以外は何も流れません(ヘッドレスの blocking rpc / ウィンドウのフレーム境界 rpc のどちらも同じ)。ログは cwd の `pelican.log` に出ます(Release ビルドも同様)。`--log-file <path>` で起動ごとのファイルに分けられ、Studio が起動した player は `<project>/.pelican/logs/player.log` に出します |
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
