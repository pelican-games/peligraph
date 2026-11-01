# 第2章 ビルドと起動

最終確認: 2026-10-10

## この章で学ぶこと

- エンジンをビルドするための必要環境と CMake の使い方
- `pelican_player` で example プロジェクトを起動する方法
- `pelican_cli project init` で自分の最初のプロジェクトを作って動かす手順
- ヘッドレス実行(ウィンドウなしで PNG を出力)とテストの回し方
- つまずきやすいポイント(バイナリアセット、暗黙プロジェクト、Qt パスなど)

## 2.1 必要環境

| 要件 | 内容 |
|---|---|
| OS | Windows(開発の主環境)。日本語を含む path の project も開けます(下の「日本語の path」)。player は Android(arm64-v8a のスマホ)でも動きます(✅SA1、build は Windows の上で NDK を使う。§2.9) |
| コンパイラ | MSVC、C++20。CI と下の例は Qt の kit `msvc2022_64`(Visual Studio 2022 向け)を使います。CMake は compiler の版を検査せず、動く最低の版は未確認です |
| CMake | **3.20 以上**([`cmake_minimum_required(VERSION 3.20)`](../../CMakeLists.txt#L1))。ルートの [CMakeLists.txt](../../CMakeLists.txt) が全体を構成 |
| Vulkan SDK | **1.4.309 以上**。configure が SDK の `vulkan_core.h` の版を読み、古ければ `Vulkan SDK 1.4.309 or newer is required (found <版>)` で止まります([版の検査](../../CMakeLists.txt#L312))。1.4.350 で検証済み。vma-hpp v3.3.0 以降、1.3.x は非対応 |
| GPU | **Vulkan 1.3** と、機能 `multiDrawIndirect` / `drawIndirectFirstInstance` / `shaderDrawParameters` / `dynamicRendering`、窓を出すときは拡張 `VK_KHR_swapchain`(下の「GPU の選び方」)。レイトレーシングの機能は任意です |
| Qt6 | Pelican Studio(エディタ)用。6.10 以上。**player だけなら不要** |
| Python | **build にも実行にも要りません**(下の「Git と Python」)。要るのは `PELICAN_WITH_SPIRV_LINK=ON`(既定 OFF)と Python 製テスト(`PELICAN_PYTHON_TESTS`、既定 OFF)だけです |
| Git | source tree が Git の checkout である必要はありません(ZIP で落としても configure できます)。ただし依存の取得に `git` コマンドとネットワークが要ります(下の「Git と Python」) |

この表とこの下に出る外部名を 1 行ずつ: **GLSL**(OpenGL Shading Language)は本リポジトリでシェーダを書くソース言語、**SPIR-V** は Vulkan が実際に読むシェーダの中間バイナリ形式([用語集](14_glossary.md#spir-v--spir-v-reflection))、**shaderc** はその GLSL を SPIR-V へ変換するコンパイラライブラリ([shadercompiler.cpp](../../src/core/shader/shadercompiler.cpp#L25))、**vma-hpp** は GPU メモリ確保ライブラリ VulkanMemoryAllocator の C++ バインディング([VulkanMemoryAllocator-Hpp](../../src/core/CMakeLists.txt#L75))です。

vma-hpp が v3.3.0 以降でなければならないのは、SDK 1.4 系で vulkan.hpp の例外クラスが
改名され(`ValidationFailedEXTError` → `ValidationFailedError`)、その版から vma-hpp が例外送出を
vulkan.hpp 側へ委譲して改名の影響を受けなくなったためです([src/core/CMakeLists.txt](../../src/core/CMakeLists.txt#L69) のコメントが理由の正)。

quill / glm / nlohmann_json / argparse / tinyexr / miniaudio などは CMake の
FetchContent で自動取得されます(各依存は `GIT_REPOSITORY` と `GIT_TAG` で宣言され、ほとんどは commit の hash で固定しています。例 [quill](../../CMakeLists.txt#L88)。JoltPhysics だけは tag `v5.5.0` の指定です([Jolt の宣言](../../CMakeLists.txt#L455)))。shaderc は例外で、Vulkan SDK 同梱版を使います(Android の build だけは shaderc・glslang・SPIRV-Tools を pin して source から build します。§2.9)。
`PELICAN_RUNTIME_SHADER_COMPILER` は「**実行時に GLSL を SPIR-V へコンパイルする shaderc を組み込むか**」の
CMake フラグです。`ON` の PC 開発ビルドには完全な Vulkan SDK が必要です(無いと configure が名指しで止まります)。
ホスト PC で事前生成した SPIR-V を消費するモバイル等のターゲットは
`-DPELICAN_RUNTIME_SHADER_COMPILER=OFF` で shaderc をリンクしません。
✅SA1 の Android の player はこの形ではなく、端末の上で実行時に compile します(事前生成の SPIR-V を配る B4 は SA1 に入れていない)。

**GPU の選び方(✅DS-1)。**player は Vulkan の物理デバイスを列挙し、API が 1.3 未満のものと、上の表の機能・拡張・queue が揃わないものを外し、残りから discrete GPU(+1000)と graphics / present が同じ queue のもの(+100)を優先して選びます([`selectVulkanPhysicalDevice()`](../../src/core/vkcore/devicefeaturepolicy.cpp#L11)、必須の機能を集める所は [core.cpp](../../src/core/vkcore/core.cpp#L457))。`pelican_player --gpu <index|name>` で列挙の 0 始まりの番号か、デバイス名の完全一致を指定できます([引数](../../src/player/main.cpp#L405))。指定しても必須の検査は飛ばしません。どれも選べなければ `No suitable Vulkan 1.3 GPU. Devices: [0] <名前> (Vulkan 1.x) ...`(`--gpu` を付けたときは `No suitable Vulkan 1.3 GPU for --gpu '<指定>'. Devices: ...`)で止まり、候補の名前と API の版が並びます。player と Studio の exe は NVIDIA / AMD の切り替え用の export(`NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance`)を持ちます([gpupreference.cpp](../../src/platform/gpupreference.cpp))。OpenXR を使うときは runtime が選んだ GPU に従います。

**Git と Python(✅DS-1)。**
- Python は build に要りません。OpenXR は `KhronosGroup/OpenXR-SDK` の release-1.1.61 を commit で pin しており、この SDK は生成済みの loader と header を含みます([OpenXR の宣言](../../CMakeLists.txt#L284))。Python が要るのは experimental SPIR-V linker(`PELICAN_WITH_SPIRV_LINK=ON`。pinned SPIRV-Tools のコード生成)と Python 製テストだけです。
- source tree に `.git` が無くても configure は通ります。Studio の crash symbol の保存が source の commit を読めないときは `Cannot identify source commit for crash symbols: Git or .git is unavailable; using unknown` の WARNING を出し、commit を `unknown` として続けます([判定](../../cmake/pelican_crash_symbols.cmake#L14))。
- 依存は FetchContent が Git の repository から取得するので、初回の configure には `git` コマンドとネットワークが要ります。依存を source tree に同梱して Git もネットワークも要らない configure にする作業(DS-1c、`PELICAN_FETCH_DEPENDENCIES` で取得に切り替える形)は公開後に入る予定で、この版にはまだありません。

**日本語の path(✅JP-1)。**Windows / MSVC で build する Pelican の全 exe(`src/` と `test/` の target)に、`activeCodePage` を UTF-8、`longPathAware` を true にする manifest が付きます([manifest](../../cmake/pelican_windows.manifest#L5)、[付ける関数](../../cmake/pelican_windows_manifest.cmake#L3))。これで `C:/作業/日本語 プロジェクト` のような path の project を開け、日本語名の model を登録できます。UTF-8 の code page を受けるのは Windows 10 version 1903 以降で、process の code page が UTF-8 でないときは起動時に 1 回 `Process code page is not UTF-8; 日本語の path で失敗することがある (Windows 10 1903 or later is required).` が出ます([文](../../src/project/windowsutf8.hpp#L23))。

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

この設定は依存ライブラリの生成処理とは独立です。✅DS-1 から OpenXR は生成済みの source を含む
`KhronosGroup/OpenXR-SDK` を使うので、Python の無い環境でも既定のまま(`PELICAN_WITH_OPENXR` は ON のまま)configure できます。
Python が要る依存は experimental SPIR-V linker(`PELICAN_WITH_SPIRV_LINK=ON`)だけで、既定は OFF です(§2.1 の「Git と Python」)。

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

**ゲームを出荷する(✅DS-1 / DS-1f)。**上の手順に `pelican_cli dist-config` の preset と `PELICAN_PROJECT` を足します。

```sh
pelican_cli dist-config projects/mygame --out build-dist/preset.cmake
cmake -S . -B build-dist -C build-dist/preset.cmake -DPELICAN_PROJECT=projects/mygame
cmake --build build-dist --config Release --target pelican_runtime
cmake --install build-dist --config Release --component Runtime --prefix "$PWD/build-dist/package"
```

| 段 | 何が決まるか |
|---|---|
| `dist-config` | project を走査して `PELICAN_WITH_*` を決めた preset を書きます。既定で `SKIP_DEVSTUDIO=ON`(Studio を build しない)・`PELICAN_WITH_RPC=OFF`、`--with-studio` を付けると Studio と RPC を残します。player は常に `PELICAN_PLAYER_GUI_SUBSYSTEM=ON`(console の窓を出さない Windows GUI の exe)になります([preset を書く所](../../src/devcli/distconfig.cpp#L933))。preset の全項目と asset の照合は [第10章](10_tools.md) の `dist-config` |
| `PELICAN_PROJECT` | project の `code/` を `pelican_game_logic.dll` として build し、`Runtime` component として player と同じ `bin/` に install します |
| install の後 | `package/bin/` に player・game DLL・MSVC の再頒布 runtime が入ります。project 本体(`project.json` と、それが指すファイル)は自分でコピーします |

- **player は自分の場所を基準にします。**game DLL は exe の隣を探し([DLL の場所](../../src/player/main.cpp#L714))、`--project` を省くと exe の隣の `project.json` を開きます(§2.3 の「暗黙プロジェクト」。WARN の 1 行が出ます)。exe の場所は `GetModuleFileNameW` から取るので、PATH から名前だけで起動しても、別の作業ディレクトリから起動しても同じです([`executableDirectory()`](../../src/core/executabledirectory.hpp#L13))。相対の `--project` は作業ディレクトリ基準です(§2.3)。
- **起動に失敗したときの見え方。**エラーの文は常に stderr に出ます。console も標準出力の接続も無い手起動(エクスプローラからの double-click など)のときだけ、同じ文を `Pelican` の題の message box でも出します。`--headless` / `--rpc` / `--embedded`、環境変数 `PELICAN_NO_MESSAGEBOX=1`、console や pipe・ファイルへの出力があるときは出しません([判定](../../src/core/playerdiagnostics.hpp#L16))。GUI の exe でも `--console` で親の console に繋げます。
- `PELICAN_PLAYER_GUI_SUBSYSTEM` の既定は OFF(開発と test の build は console の exe)です([option](../../src/player/CMakeLists.txt#L1))。
- 出荷物を install した場所・別の作業ディレクトリ・日本語の path で通しで起動する確認は、v0 の全数 gate の配布の項目で行う予定で、この文書の時点では未確認です。

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

- **消えるもの**: `src/core/animation/*` のソース一式(clip サンプラ・state machine 評価器 `EvaluatorV1`・`AnimationSystem`・pose graph の runtime。[src/core/animation/CMakeLists.txt:1](../../src/core/animation/CMakeLists.txt#L1))と、共通グラフの `pelican.pose` module(登録呼び出し自体が `#if` の中 — [makeBuiltinGraphModuleRegistry()](../../src/project/graphmodule.cpp#L797))。`pelican_cli graph-catalog` は 4 module 27 op ではなく **3 module 21 op** を返します(`PELICAN_WITH_AUDIO=ON` のとき。`PELICAN_WITH_AUDIO=OFF` では `pelican.audio` module も登録されません。[../v0_freeze.md](../v0_freeze.md) §1.3)。
- **残るもの**: 凍結ヘッダ(`abi_v1.hpp` / `pose_staging_v1.hpp`)と、その**交渉入口**(`getApiV1` / `getPoseStagingServiceV1` / `Vrm::getApplicationServiceV1`)。ゲーム DLL はこれらをリンクするので、入口は残したまま **`unsupported_version` を返す stub** に差し替わります([animation_disabled.cpp:13](../../src/core/animation/animation_disabled.cpp#L13))。「関数が無くてロードできない」ではなく「名前で『この build には無い』と答える」形です。**skinning の compute 側(palette / `skinned.vert` / `ray_deform.comp`)も残ります** —— 姿勢を受け取る側であって、姿勢を作る側ではないからです。
- **使おうとしたときの見え方**: `animation` コンポーネントを持つシーンは scene load の prepare で `animation.module_unavailable@1`(→ トランザクションなので旧シーンが保たれます。検査は [`resolvedObjectFeatureRefusal()`](../../src/core/loader/resolvedscene.cpp#L78)・[第4章 §4.3](04_scene_ecs.md))。エディタでこのコンポーネントを足す(`add_component` / `spawn`)ときも同じ関数・同じ名前で拒否されます(✅PG-1)。`pose.*` を書いた共通グラフ文書は `graph.operation_unregistered@1`。`project.json` に `requires: ["animation"]` を書いてあるプロジェクトは、**そこまで行く前に** project の parse で止まります(下記)。

**project 側からの宣言(`requires`)。** `project.json` に `"requires": ["animation"]` と書いておくと、OFF build でそのプロジェクトを開いた時点で `animation.module_unavailable@1: this build has PELICAN_WITH_ANIMATION=OFF, so project.json's requires: ["animation"] cannot be satisfied` を出して `pelican_player` が exit 1 します(ノードを 1 つずつ拒否させて原因を探させない、という判断です)。出荷では `animgraph_demo` / `vrm_xr_demo` / `posegraph_demo` の 3 つが宣言しています(`requires` の語は `animation` のほかに `audio` / `physics` / `xr` / `vat` / `seqplayer` があり、`sprite_demo` は `physics` を宣言しています。✅PG-1)。同じ値を**コーパスを順に調べる側**も読むので、OFF build のスモークはそれらを止まらずに名前付きで飛ばします:

```
skip posegraph_demo: requires animation (PELICAN_WITH_ANIMATION=OFF)
```

この 1 行は宣言したプロジェクトごとに出るので、OFF build では `animgraph_demo` / `posegraph_demo` /
`vrm_xr_demo` の **3 行**が並びます([プロジェクトの skip 出力](../../test/run_project_catalog_smoke.cmake#L123))。
飛ばされたプロジェクトがエディタ経路のレーンを持っていた場合は `skip editor lane: <project> requires …` が続き
([editor lane の skip 出力](../../test/run_project_catalog_smoke.cmake#L371))、最後に「何件中何件を飛ばしたか」の要約が 1 行出ます。

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
- [projects/raytracing_demo](../../projects/raytracing_demo) — 手続き幾何のレイトレーシング(intersection shader で球を解く。Vulkan ray tracing 対応 GPU が要る)。自分の project でレイトレーシングを始めるなら、JSON を手で書くより先に Studio の道があります: Blank の project の Graph タブで palette の `render.compute` の New… から RT の task を置き、Rendering settings タブの Ray layout の節の **Enable hit groups** で物ごとの hit group に広げます([第13章](13_editor.md) §13.5 の「RT を Graph で組む(✅CT-6s)」と「Studio で RT を組む(✅RTL-2s)」)
- [projects/pathtrace_demo](../../projects/pathtrace_demo) — frame をまたいで蓄積する path tracer。Principled BSDF(ガラス・金属・coat)、scene の camera(透視 / 正射影)、glTF の mesh。engine を変えずに project 側の shader だけで組んである。中身は [projects/pathtrace_demo/README.md](../../projects/pathtrace_demo/README.md)、ray layout の書き方は [第6章 §6.15](06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v3-と-pelican_ray_instancert3-1--wp374374a377379383)(Vulkan ray tracing 対応 GPU が要る)

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

- **`passes/main_rendering_config.json` は `pelican.graph` root で生成されます** —— 最上位が `{"schema": "pelican.graph", "version": 1, "definitions": {}, "nodes": [], "outputs": {}, "rendering": { … }}` で、pipeline preset と features は `rendering` 節の中です([renderingConfigJson()](../../src/project/projecttemplate.cpp#L263) は graph root をそのまま書いた文字列を返します。包み関数 `wrapLegacyRenderingConfig` は MG-2b で撤去されました)。rendering config の root はこの形だけで、rendering のキーを最上位に直接並べた旧い形は `render_config.legacy_root@1` で拒否されます([第3章 §3.2](03_project_format.md))。
- **`code/CMakeLists.txt` は 2 行**です —— `pelican_game_sources(game.cpp)` の**後に** `pelican_packages()`([projecttemplate.cpp:276](../../src/project/projecttemplate.cpp#L277))。`pelican_packages()` は `packages/*/` に複製済みで code を持つ package を build してゲームロジック DLL に合流させるので、**順序が意味を持ちます**(合流先の DLL が先に定義されている必要があります)。package を使わないうちは何もしません。

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
    "camera": { "yfov": 0.7853981633974483, "znear": 0.1, "zfar": 1000 },
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

`camera` に `up` はありません。✅CC-1(`c8367b87`)で camera の up は transform の回転から取るようになり、`basic_config.camera.up` は撤去されました(書いてあると `project.camera_up_removed@1` で開けません。前に作った project の直し方は [第3章 §3.13](03_project_format.md) の「camera の向きの規約の変換」)。

雛形の `scenes/main.scene.json` の `MainCamera` は `pos` が `(0, 0, 3)`・回転なしで、回転なしの camera は −Z を向くので原点を見ます([雛形の scene](../../src/project/projecttemplate.cpp#L133))。同じ scene の `KeyLight`(directional)の `direction` は `(-0.5, -1, -0.25)` で、光は −Z の側へ進み、camera から見える面を照らします([雛形の KeyLight の向き](../../src/project/projecttemplate.cpp#L148))。

✅CC-1c(`b0b2fc77`)の前は `(-0.5, -1, 0.25)` のままで、CC-1 で camera が z = +3 に移った後は光が +Z へ進み、camera の見る面が光の裏になって黒く写っていました(commit の記録は ambient が無いことを理由に挙げています。Studio の scenario 7 本(`add_primitive_object`・`forward_loop_from_scratch`・`gui_build_A` / `B` / `C` / `full` / `optimize`)で見つかりました)。この scene は Standard と Blank の両方の雛形が書きます([`templateFiles()`](../../src/project/projecttemplate.cpp#L352))。座標の規約は [第6章 §6.1](06_rendering.md) の「座標の規約」です。

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

**Studio から(✅MI-1s)**: Studio で project を開き(File > New Project... の Blank でも)、engine が動いている間に、Outliner の右クリックか
Scene の toolbar の **Add Object > Model from file…** を押して `.glb`(か `.vrm`)を選びます。project の外のファイルは `assets/models/` に
コピーされ、`asset_data.json` への登録と、その model を原点に描く object の追加を Studio がします(object の名前はファイル名の stem から)。
File > Save Scene で `scenes/main.scene.json` に残ります。画面にはその model を描く pass が要ります(Blank の雛形は gradient を描く
`present` だけです)。詳しくは [第13章](13_editor.md) §13.1 の「Studio で外の model を持ち込む(✅MI-1s)」、登録の RPC は [第5章](05_assets.md) §5.2 です。

**手で書くとき**:

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

- 秒はコマンドラインで **1 回だけ frame 番号に量子化**されます(`llround(秒 × fps)`。エンジンには「フレームの間の時刻」が渡りません — [parseStartFrame()](../../src/player/main.cpp#L330))。秒は **3600(1 時間)まで**で、この上限は `--fps` によりません([上限の定数](../../src/player/main.cpp#L328))。
- **飛ばすのではなく、そこまで回します。** `start_frame − 1` 枚を capture 無しで実行(warm-up)し、**最初に capture される frame が t の frame** です([loop.cpp:441](../../src/core/appflow/loop.cpp#L680))。グラフの時計・ゲームシステムの積分・アニメーションの評価器は時刻から逆算できないので、「その時刻に居た」と言える唯一の方法が全ステップを通ることだからです。`--start-time 0` は通常の run と同一です。
- 実行時に stderr へ 1 行出ます: `offline: start_time=2 start_frame=120 replay=119 frames=2`。
- **`--frames` とは併用できます**(枚数の指定)。**併用できないのは** `--rpc` / `--replay` / `--record-input` / `--bake-camera-output` の 4 つで、これらは「どの frame を出すか」を自分で決める経路だからです。
- 名前付きの拒否は 3 つ: `offline.time_invalid@1`(負・非有限・数でない・3600 秒を超える・丸めた frame 番号やその時刻が有限でない)/ `offline.headless_required@1`(`--headless` 無し)/ `offline.mode_conflict@1`(上の 4 フラグとの併用)。いずれも**コマンドライン解析の時点**で決まり、GPU まで届きません。

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
| `Vulkan SDK 1.4.309 or newer is required (found ...)` | Vulkan SDK が古い。1.4.309 以上を入れて `VULKAN_SDK` を設定し、build ディレクトリを作り直す(§2.1) |
| `No suitable Vulkan 1.3 GPU. Devices: ...` / `No suitable Vulkan 1.3 GPU for --gpu '...'. Devices: ...` | Vulkan 1.3 と必須の機能を持つ GPU が無い、または `--gpu` の指定がそれ以外を指した。並んだ候補の番号か名前を `--gpu` に渡す(§2.1 の「GPU の選び方」) |
| `Process code page is not UTF-8; ...` | Windows 10 1903 より前で、UTF-8 の manifest が効いていない。日本語の path で失敗することがある(§2.1 の「日本語の path」) |
| 初回の configure が FetchContent の取得で失敗 | 依存の取得に `git` とネットワークが要る(§2.1 の「Git と Python」) |
| GPU テストが全部 SKIP | Vulkan デバイスが列挙できない環境(リモートデスクトップ等)。仕様どおりの挙動 |
| rpc モードでログが出ない | rpc モードでは **stdout はプロトコル専用** —— JSON-RPC の応答行以外は何も流れません(ヘッドレスの blocking rpc / ウィンドウのフレーム境界 rpc のどちらも同じ)。ログは cwd の `pelican.log` に出ます(Release ビルドも同様)。`--log-file <path>` で起動ごとのファイルに分けられ、Studio が起動した player は `<project>/.pelican/logs/player.log` に出します |
| `windowed rpc request queue is busy`(`data.reason = "busy"` / `queue_capacity`) | ウィンドウモードの `--rpc` はフレーム境界でのみ処理する**有界キュー**(既定 64)。詰め込みすぎると即エラーで返るので、応答を待ってから次のリクエストを送る([第10章](10_tools.md)) |
| `--xr on is incompatible with <driver>` | `--xr on` と決定的な駆動モード(rpc / golden / replay / headless)の併用。`--xr auto` にすれば(ログなしで)flat に正規化されます(§2.3) |
| `Unknown behavior type 'X' on object 'Y'` | シーンの `behavior.type` がゲームロジック DLL に登録されていない。`PELICAN_REGISTER_BEHAVIOR` の登録名と綴りを合わせる([第8章](08_gameplay.md)) |
| `behavior type 'X' on object 'Y' is pending because the game-logic DLL is unavailable`(WARNING) | DLL 未ロード時は hard error にせず pending として保持する仕様。`-DPELICAN_PROJECT` 付きでビルドするか、F5 でリロードすると解決します |
| `behavior on object 'X' requires a non-empty string type` | `behavior` コンポーネントに `type`(非空の文字列)が無い、または文字列以外 |

## 2.9 Android で player を動かす

✅SA1(`fc0c09bf6`、2026-11-01)。`pelican_player` を arm64-v8a のスマホの NativeActivity として動かします。
APK の中の `libpelican_player.so` が `android_main` から入り、project を画面に描きます(Studio は持っていきません)。
APK は Gradle を使わずに作る **debug 用の 1 種類だけ**です(package `org.pelicanengine.sa1b`、`debuggable="true"`、横向き固定。
[manifest](../../tools/android/AndroidManifest.xml))。確かめた端末は NX789J(Snapdragon 8 Elite / Adreno 830、Android API 36)の 1 台です。

**手順の正は [`tools/android/README.sa1b.md`](../../tools/android/README.sa1b.md)(build・APK・headless の実行)と
[`tools/android/README.sa1-lifecycle.md`](../../tools/android/README.sa1-lifecycle.md)(home・消灯と点灯などの lifecycle の確認)です。**
ここでは要る物と、手順の読み方だけを書きます。どの script も Git Bash で repo の root から実行します。

### 要る物

| 物 | 内容 |
|---|---|
| Android NDK | **r27c**。script の既定は `<作業ディレクトリ>/android/android-ndk-r27c`(環境変数 `NDK` で変える)。r27c の libc++ では `std::jthread` に `-fexperimental-library` が要り、[`cmake/pelican_android.cmake`](../../cmake/pelican_android.cmake#L34) が付けます |
| Android SDK | **platform `android-36`** と **build-tools `36.0.0`**(`aapt2`・`zipalign`・`apksigner`)。既定は `<作業ディレクトリ>/android/sdk`(環境変数 `SDK`)。**SDK の利用規約(license)への同意は利用者が自分で行います**。script は SDK を入れず、規約にも同意しません |
| JDK | `keytool` と `apksigner.jar` を動かす java。既定は Eclipse Adoptium の jdk-21(環境変数 `JDK`) |
| Vulkan SDK | host 側の 1.4.309(`VULKAN_SDK`)。engine に埋め込む shader を host の glslang で compile します(desktop と同じ) |
| Python | shaderc・glslang・SPIRV-Tools の生成器と、APK の asset をまとめる [`sa1_apk.py`](../../tools/android/sa1_apk.py) が使います(環境変数 `PYTHON`) |
| 依存の source | configure はネットから取りません(`FETCHCONTENT_FULLY_DISCONNECTED=ON`)。quill・glm・vma-hpp などは **desktop の build tree の `_deps`**(環境変数 `DEPS`)を使い、Android だけが要る shaderc(v2025.1)・glslang・SPIRV-Tools(v2025.1)・Vulkan-Headers(vulkan-sdk-1.4.309.0)は、[`cmake/pelican_android.cmake`](../../cmake/pelican_android.cmake#L133) が pin した tag / commit の checkout を `ANDROID_DEPS` に置きます([`sa1_configure.sh`](../../tools/android/sa1_configure.sh#L6) の冒頭のコメントに一覧) |
| validation layer | Khronos の Android 版 `libVkLayer_khronos_validation.so`(vulkan-sdk-1.4.309.0)。[`sa1_fetch_validation.sh`](../../tools/android/sa1_fetch_validation.sh#L6) が公式の `android-binaries-1.4.309.0.zip` を `ANDROID_DEPS` に取り、zip と `.so` の SHA-256 を `provenance.json` に残します。layer が無いと [`sa1_build_apk.sh`](../../tools/android/sa1_build_apk.sh#L33) は止まります(`ALLOW_MISSING_VALIDATION=1` は APK の構造を見るためだけで、engine は layer が無ければ起動で止まる) |
| debug keystore | **repo の外**に置きます。既定は `<作業ディレクトリ>/android/debug.keystore`(環境変数 `KEYSTORE`)。無ければ `sa1_build_apk.sh` が `keytool` で作ります([その所](../../tools/android/sa1_build_apk.sh#L43)) |
| 端末 | arm64-v8a、**Android 13(API 33)以上**([minSdk の検査](../../cmake/pelican_android.cmake#L25))、Vulkan 1.3。`adb` で USB デバッグがつながっていること |

script の既定の path は開発機の物(`<作業ディレクトリ>/...`)なので、自分の環境では上の環境変数で渡してください。

### 流れ

1. **configure**: [`sa1_configure.sh`](../../tools/android/sa1_configure.sh#L27) が `build-android/` を NDK の toolchain、`ANDROID_PLATFORM=android-33`、`ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`(16 KB page)で作ります。root の `CMakeLists.txt` は `if(ANDROID)` の道で [`cmake/pelican_android.cmake`](../../cmake/pelican_android.cmake#L38) を読み、IMGUI・OpenXR・RPC・hot reload・RenderDoc・audio・SPIR-V link・intro link・Studio を既定で OFF にします(configure の行の `-D` が勝つ)。Windows の configure は変わりません。
2. **build**: `ninja -C build-android pelican_player` で `libpelican_player.so` を作ります。
3. **APK**: [`sa1_build_apk.sh`](../../tools/android/sa1_build_apk.sh#L25) が `PROJECT`(既定 `projects/posegraph_demo`)を assets に入れ(**`.pelican/` は除く**)、`aapt2 link`(min 33 / target 36)、strip した `.so` と validation layer の無圧縮の追加、`zipalign -P 16`、`apksigner` の署名と検証、`llvm-readelf` で自分の `.so` の LOAD が 16 KB(`0x4000`)揃いであることの確認までを行います(validation layer は Khronos の配布が 4 KB 揃いで、debug の APK にだけ入るので記録だけ)。strip する前の `.so` は `ndk-stack` 用に残ります。出力は `OUT`(既定 `build-android/sa1b-apk/pelican-sa1b-debug.apk`)。
4. **install と起動**: `adb -s <serial> install -r <apk>` → `adb -s <serial> shell am start -n org.pelicanengine.sa1b/android.app.NativeActivity`。

### 起動の引数

player の引数は app が自分で組みます([その所](../../src/platform/android/android_main.cpp#L289)): `--project`(internal storage に展開した project)、`--user-dir`、`--log-file`、そして **`--vulkan-validation on` は固定**です(validation を外した run はこの APK ではできません)。
足したい引数は intent の string extra `pelican_args` で渡します(`--es pelican_args '"--frames 0 --timing-frames 30"'`)。空白で区切るだけで、引用符は解釈しません([`intentArguments()`](../../src/platform/android/android_main.cpp#L65))。

- 何も渡さなければ窓ありで 600 frame 描いて終わります([既定の `--frames`](../../src/platform/android/android_main.cpp#L309))。`--frames 0` は終わりなし(back で終える。✅SA1f から BACK は app が受けて、離した時に activity を終えます)。✅SA1 から、Android の `--frames` と frame の時間は**実際に描けた frame だけ**を数えます(window が無い間・acquire が timeout した周回は数えない)。
- `--headless` を渡すと INIT_WINDOW を待たずに起動し、960x540 で 3 frame 描いて `files/sa1_posegraph.png` に書きます([その所](../../src/platform/android/android_main.cpp#L294))。
- `--timing-frames N`(1〜100000、既定 120)は N frame ごとの CPU の時間(p50 / p95 / p99、pacing を含む)を log に出す間隔です。

project の展開は版(全 file の path と中身の hash)が変わった時だけで、`project.stage` に書き終えてから置き換えます。user directory(`files/user`)と pipeline cache(`files/cache/pipeline_cache.bin`、tmp に書いてから rename)は project の外なので残ります。shader cache の版の文字列は Android では `shaderc-v2025.1-android` です(desktop の cache と混ざらない)。

### 端末の serial と log

- **serial を必ず指定します。** `adb` を手で打つときは毎回 `-s <serial>` を付けてください(複数の端末や emulator がつながっていると取り違えます)。runner の script(`sa1_run_device.py`・`sa1_run_windowed.py`・`sa1_run_lifecycle.py`・✅SA1f の `sa1f_run_device.py`)は serial と `adb` の path を中に固定している(`adb = [..., '-s', '<serial>']`、[例](../../tools/android/sa1_run_lifecycle.py#L28))ので、別の端末ではその行を書き換えます。
- **`log.tag=S` の端末では file の log を見ます。** 確かめた端末は `getprop log.tag` が `S` で、app が自分の process の閾値を INFO にしても([その所](../../src/platform/android/android_main.cpp#L252))`logcat` の dump は空でした(原因は未確認)。同じ log は app の internal storage にもあります。`adb -s <serial> exec-out run-as org.pelicanengine.sa1b cat files/engine.log`(engine の quill の log。validation の warning / error の累計も最後に出る)、`files/pump.log`(lifecycle の command と surface の解放の ack。quill を通さず毎行 fsync)、`files/stderr.log`、`files/engine-result.txt`(engine の終了コード)。端末の property は変えません。
- PNG を `exec-out` で取るときは Git Bash の `>` か Python の bytes で受けます(Windows PowerShell 5 の文字列の redirect は壊します)。

### 入力を端末で確かめる(✅SA1f)

✅SA1f(`bbedafc88`、2026-11-02)から、キーボード・ゲームパッド・タッチ・ペンが action map と UI に入り、BACK で app が終わります(何がどう入るかは [第7章「Android の入力」](07_input_ui.md#android-input))。端末での確かめは次の手順で行います。どれも Git Bash で repo の root から実行します(`python` は手元の Python 3)。

1. **fixture を作る**: `python tools/android/sa1f_fixture.py`。[`sa1f_fixture.py`](../../tools/android/sa1f_fixture.py#L6) が `projects/posegraph_demo` を `build-android/sa1f-project` に写し、確かめ用の action 4 つ(`sa1f_kbd` = `kbd:space`、`sa1f_pad` = `pad:a`、`sa1f_region` = 画面の左半分の下 3/4 の `touch:region`、`sa1f_tap` = 右半分の下 3/4 の `touch:tap`)と、button 1 つの UI 文書を足します。元の project は変えません。
2. **APK を作る**: `PROJECT=build-android/sa1f-project OUT=build-android/sa1f-apk bash tools/android/sa1_build_apk.sh`(`ninja -C build-android pelican_player` の後)。
3. **端末で走らせる**: `python tools/android/sa1f_run_device.py --apk build-android/sa1f-apk/pelican-sa1b-debug.apk --label <名前>`。

[`sa1f_run_device.py`](../../tools/android/sa1f_run_device.py#L57) は APK を入れ直し、`--frames 0 --sa1-test-input --timing-frames 120` で起動して最初の present を待ち、`adb shell input` で次の順に入れて `files/engine.log` を確かめます。

| 段 | 入れる物 | 確かめる事 |
|---|---|---|
| `keyboard-space` | `input keyboard keyevent KEYCODE_SPACE` | `sa1f_kbd` が pressed |
| `gamepad-a` | `input gamepad keyevent --longpress KEYCODE_BUTTON_A` | `sa1f_pad` が pressed、最後に released |
| `touch-tap` | 画面の横 75%・縦 65%(左上から)の点の `input tap` | `sa1f_tap` の pressed と released が同じフレーム |
| `touch-region` | 横 25%・縦 65% の点で 800 ms の定点 `input swipe` | `sa1f_region` の pressed と held、最後に released |
| `ui-tap` / `ui-hold` | UI の button の上 (200, 210) の tap と 800 ms の定点 swipe | UI のポインタ 1 番の down と up が同じ座標で揃い、掴みと放しがある |
| `stylus-tap` | `input stylus tap` | `sa1f_tap` がペンでも立つ |
| 終わり | `input keyevent KEYCODE_BACK` | `engine-result.txt` が `0`、`pump.log` に `DESTROY engine done`、validation が有効で error 0 |

タッチの段ではどれも、`pointer()` が入れた座標を指し、最後に離した後で `inside=false` になることも確かめます。瞬間の tap は 1 フレームの中で触れて離れて途中の `pointer()` が見えないので、tap の後に同じ点で 120 ms の定点 swipe を足しています。

- **`--sa1-test-input`** は app が引数から取り除く確かめ用の switch で、ある時だけ action の値・`pointer()`・UI のポインタの event を `engine.log` に書きます([その所](../../src/platform/android/android_main.cpp#L261))。無い起動では log は増えません。
- 結果は `build-android/sa1f-results/<名前>/` に、段ごとの `engine.log` / `pump.log`・全 `adb` の引数と終了コード(`commands.json`)・判定(`checks.json`)が残ります。同じ名前のフォルダがあると始めません。端末が lock されていると解除を試みずに止まり、失敗した時は `failure-*` の log を残して app を動かしたままにします。
- 判定の関数は端末なしで `python tools/android/test_sa1f_run_device.py` が確かめます(adb は呼びません)。
- serial と `adb` の path は script の中に固定です(上の「端末の serial と log」)。2 本以上の指は `adb shell input` では入れられないので、CPU のテストだけで確かめています。

### 今できない事

音、RPC と Studio からの操作、ImGui、hot reload、game logic の code を持つ project(試していない。未確認)。入力では、文字の入力とソフトキーボード、ゲームパッドの抜き差しの検出、仮想スティック・仮想ボタン・ジェスチャ・振動(feature が action に値を出す口 SA1g の後)。性能は validation を有効にしたままの値しかありません。窓の向きの扱い(preTransform IDENTITY)と lifecycle の設計は [../design_wsi_epoch_recovery.md](../design_wsi_epoch_recovery.md) §7.1 の SA1 追補、段の計画と残りは [../roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md) の「Quest standalone(SA トラック)」です。

## 関連文書

- [../../README.md](../../README.md) — ビルド手順の原本
- [../implementation_plan.md](../implementation_plan.md) — §0 共通規則(ビルド・テスト・コード規約)
- [../design_headless_rendering.md](../design_headless_rendering.md) — ヘッドレス描画の設計
- [../design_build_tiers.md](../design_build_tiers.md) — ビルドユニットと配布ビルド
- [第3章 プロジェクト形式](03_project_format.md) / [第10章 ツールリファレンス](10_tools.md)
