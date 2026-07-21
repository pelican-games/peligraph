# 第1章 エンジン全体像

対象: pelican2(2026-07-21 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- Pelican2 とは何か — 構成要素(player / cli / Studio / web ビューア)と技術スタック
- リポジトリの地図(どこに何があるか)
- レイヤ構造と、コード全体を貫くレイヤ規則
- 横断的な設計原則(サブセット原則・fail-fast・strict v1・決定性・二層モデルなど)の厳密な定義
- 開発体制(設計文書 → WP 指示書 → コーディングエージェント)と文書の優先順位

## 1.1 Pelican2 とは

Pelican2 は **C++20 / Vulkan(vulkan.hpp)/ Windows(MSVC)** のゲームエンジンです。ゲームは「**プロジェクト**」(`project.json` + JSON データ + アセット + 任意の C++ コード)として作り、エンジン本体のコードには手を入れません。

実行物は 4 つ(うち `pelican-spv-link` は experimental)+web ビューアです:

| 実行物 | 役割 |
|---|---|
| `pelican_player` | ゲームランタイム。ウィンドウ / ヘッドレス / JSON-RPC 駆動([第2章](02_getting_started.md)・[第10章](10_tools.md))。JSON-RPC はヘッドレス(blocking)とウィンドウ(フレーム境界処理)の両方で使えます ✅WP156 |
| `pelican_cli` | 開発 CLI。7 サブコマンド系統(`assets` / `bake-camera` / `import` / `dist-config` / `project` / `dump-lowered-material` / `vrm` — [main.cpp](../../src/devcli/main.cpp))。エンジン本体にリンクしない |
| `pelican_studio` | Qt6 製エディタ(現状は骨組みのみ 🚧)。**エディタ機能の実体は player 側の JSON-RPC + ImGui パネルに入っており(WP149〜172)、Qt Studio はまだそれらを呼んでいません**([第2章](02_getting_started.md) §2.7・[第10章](10_tools.md)・[第13章](13_editor.md)) |
| `pelican-spv-link` | experimental な SPIR-V リンカ CLI([spvlink/main.cpp](../../src/spvlink/main.cpp)・[第10章](10_tools.md)) 🚧 |
| web ビューア | 別リポジトリ `my_webpage` の WebGPU「Shader Dock」。**同じプロジェクトファイルをブラウザで開く**([第9章](09_web.md)) |

特徴を一言でいうと: **「データ(JSON)で宣言し、C++ で振る舞いを書き、すべてを決定的・検証可能にする」** エンジンです。レンダリングパイプラインは JSON だけで定義でき([第6章](06_rendering.md))、ゲームロジックはネイティブ C++(ホットリロード可能な DLL — [第8章](08_gameplay.md))、実行はヘッドレス+JSON-RPC で完全に自動化できます。

## 1.2 データフロー(起動から画面まで)

```
project.json ──┬─ scene_data_json ──▶ pelican.scene v1 ─▶ ECS(オブジェクト)
(マニフェスト) │                                        ├▶ LightContainer(light)
               │                                        ├▶ PhysWorld(collider)
               │                                        └▶ BehaviorAttachmentArena(behavior)
               ├─ asset_data_json ──▶ モデル一括ロード(glb/VRM)
               ├─ rendering_config_json ─▶ feature 合成 ─▶ フレームグラフ ─▶ 描画
               ├─ input_actions_json ──▶ アクション層(入力)
               └─ ui_config_json ──▶ UI オーバーレイ
                                     ▲
        code/*.cpp(ゲームシステム)──┘ GameContext 経由で毎フレーム操作
```

毎フレームの流れ(固定順序 — 決定性の土台):

```
時刻 advance → イベント凍結 → 入力スナップショット → アクション凍結 → イベント配送
→ ECS システム → ゲームシステム(order 順。BehaviorSystem も order=50 として同じ総順序に参加)
→ シーン遷移の適用 → 描画
```

`behavior` はシーンのオブジェクトに直接ぶら下がる毎フレーム処理です。シーンロード時に `behavior` コンポーネントだけが ECS の振り分けから外れて BehaviorAttachmentArena に渡り([scene.cpp](../../src/core/loader/scene.cpp) → [behaviorarena.cpp](../../src/core/gamelogic/behaviorarena.cpp))、実行はゲームシステムと同じ総順序に `order=50` / 名前 `"BehaviorSystem"` で参加します([behavior.hpp](../../src/core/userpublic/behavior.hpp))。書き方は [第8章](08_gameplay.md)、JSON 側の語彙は [第4章](04_scene_ecs.md) を参照してください。

## 1.3 リポジトリの地図

```
pelican2/
├── CMakeLists.txt        # 全体構成 + FetchContent(外部ライブラリ)
├── src/
│   ├── core/             # エンジン本体(静的 lib pelican_core)
│   │   ├── vkcore/       #   Vulkan 低層(device、swapchain、RT、DeletionQueue)
│   │   ├── renderer/     #   レンダラ編成、カメラ、ライト、UI 描画
│   │   ├── renderingpass/#   rendering config パーサ、フレームグラフ、compute
│   │   ├── shader/       #   シェーダ基盤(コンパイラ/リフレクション/ライブラリ/ファクトリ)
│   │   ├── material/     #   マテリアル実体(MaterialContainer、標準マテリアルリソース)
│   │   ├── light/        #   ライト実体(LightContainer)
│   │   ├── ecs/          #   ECS コア(コンポーネント登録・predefined)
│   │   ├── loader/       #   バインダ(PathResolver、basicconfig、scene、画像)
│   │   ├── model/        #   glTF ロード、VAT
│   │   ├── asset/        #   asset_data.json → モデル登録
│   │   ├── os/           #   ウィンドウ(GLFW)、入力スナップショット、アクション層
│   │   ├── phys/         #   物理クエリ(raycast/overlap/shapeCast、provider 境界)
│   │   ├── playback/     #   SeqPlayer(transform_seq)、VatPlayer、カメラ焼き出し
│   │   ├── audio/        #   WAV SE 再生(miniaudio)
│   │   ├── animation/    #   スケルタル/グラフ評価の機構(ABI v1 jobs)
│   │   ├── ui/           #   pelican.ui ランタイム(レイアウト・atlas)
│   │   ├── imgui/        #   ImGui 開発 UI + Plan Viewer / Object Tree / Inspector(編集可)/ Asset Browser
│   │   ├── renderdoc/    #   RenderDoc の受動検出と F11/RPC capture(PELICAN_WITH_RENDERDOC)
│   │   ├── watch/        #   アセットホットリロード基盤(FileWatcher/ReloadService)
│   │   ├── gamelogic/    #   ゲーム DLL のロード・ホットリロード
│   │   ├── persistence/  #   user:// 設定・セーブ(pelican.settings)
│   │   ├── openxr/       #   OpenXR(PCVR。session/composition/action/mirror)
│   │   ├── communication/#   JSON-RPC サーバ + エディタ編集サービス + windowed RPC ホスト
│   │   ├── appflow/      #   メインループ、EngineTime、teardown
│   │   ├── userpublic/   #   ★ゲームコードに公開する API の境界(GameContext 等)
│   │   ├── resources/    #   埋め込みリソース(engine:// の実体)
│   │   └── container.hpp / handle.hpp / launchconfig.hpp / job_system / log / profiler
│   ├── project/          # ★解釈レイヤ(静的 lib pelican_project。エンジン非依存の純ロジック)
│   ├── player/           # pelican_player(main.cpp)
│   ├── devcli/           # pelican_cli
│   ├── devstudio/        # Pelican Studio(Qt)
│   └── spvlink/          # pelican-spv-link CLI(experimental)
├── projects/             # example / sprite_demo / animgraph_demo / vrm_xr_demo(JSON 例の出典)
├── docs/                 # 設計文書(索引: docs/README.md)+ 本マニュアル(manual/)+ コード解説(source-code-guide/)
└── test/                 # Catch2 単体(116 実行ファイル)+ fixture + golden(49)+ 結合スクリプト(run_*、33 本)
```

※ `src/core/` は主要ディレクトリのみを載せています(このほかに `fullscreenpass/` / `geomhelper/` があります)。

## 1.4 レイヤ構造とレイヤ規則

> **設計決定(レイヤ規則):** `src/core/vkcore`(Vulkan 低層)から `src/core/renderingpass` 以上のレイヤへ **include を追加しない**。下位層は通知(フラグ/イベント)を上げるだけで、編成(リソース再生成・再バインド)は上位層が行う。実例: リサイズ処理 — スワップチェーンはフラグを立てるだけで、レンダーターゲットの再生成は Renderer が編成する。

> **設計決定(userpublic 境界):** ゲームコードが include してよいのは `src/core/userpublic/` のヘッダのみ。エンジン内部はモジュール機構(`DECLARE_MODULE` / `GET_MODULE` — 遅延構築のサービスロケータ)で構成されるが、**ゲームコードからの `GET_MODULE` 直呼びは禁止**で、すべて `GameContext` ファサードを通す。エンジン内部でも新規コードはモジュール間依存を「依存構造体」を関数引数で渡す形で明示する。

> **設計決定(解釈レイヤの分離):** JSON のパース・検証・計画(sceneformat / featurecompose / importmanifest / jsonrpc / materialformat など)は、GPU・Vulkan・モジュール機構に依存しない**純ロジック**として書く。その中核は `src/project/`(静的ライブラリ `pelican_project`、依存は nlohmann_json + ハッシュ用 picosha2 のみ)に分離済みで、`pelican_cli` や将来のエディタはエンジンをリンクせずにプロジェクトを読める。※`frameplanner` も同じ純ロジック方針ですが、実体は分離されておらず `src/core/renderingpass/`(pelican_core 側)にあります。

```
ゲームコード(プロジェクトの code/)
    ↓ userpublic(GameContext / GameObjects / PELICAN_REGISTER_SYSTEM)
エンジン上位層(renderer / renderingpass / loader = バインダ)
    ↓                     ↖ 解釈レイヤ pelican_project(純ロジック)
vkcore(Vulkan 低層)
```

## 1.5 横断的な設計原則

このエンジンを理解する鍵は、個々の機能よりも**全体を貫く少数の原則**です。各章で繰り返し登場します。

> **設計決定 1(fail-fast — 読めるふりをしない):** スキーマ不一致・バージョン不一致・未知の名前・必須フィールド欠落は、警告ではなく**名指しの hard error** で起動を止める。エラーメッセージには必ず手がかり(解決後の絶対パス・登録済み id 一覧・移行先のキー名・OFF にしたビルドフラグ名)を含める。理由: 警告止まりだと「動いたように見えて一部だけ壊れる」事故になる。唯一の例外はシェーダコンパイル失敗(ホットリロード中の編集途中保存に耐えるため result 返却)。

> **設計決定 2(strict v1 — 互換受理の追加禁止・2026-07-08 決定):** ランタイムは v1 形式**だけ**を読む。旧キー名・レガシー形式の受理コードを足すことは規約違反で、旧形式の変換は外部ツールの仕事。WP63 で既存の互換受理(カメラの `fov_y` 等)も撤去された。

> **設計決定 3(パージ可能 — 参照 = 存在):** feature・compute・actions・collider などは、**参照を書いたときだけ存在**する。参照しなければ機構ごと素通りし、挙動もコストも完全に不変(ゴールデンテストで機械的に証明)。エンジンに機能を「埋め込まない」。

> **設計決定 4(サブセット原則):** 「web で開けるプロジェクト ⊆ pelican で開けるプロジェクト」。形式拡張は常にエンジン先行。web 専用の形式要素は作らない([第9章](09_web.md))。

> **設計決定 5(決定性):** エンジン内部の判定に壁時計・`std::rand`・`random_device` を使わない。固定ループ順序・入力スナップショット・決定的システム実行順・フレーム境界のイベント配送・PCG32 乱数(既定シード 0)・固定ステップ時刻(ヘッドレス)により、**同じ入力からは常に同じ結果**が出る([第8章](08_gameplay.md) §8.10)。

> **設計決定 6(二層モデル — アセット):** エンジンが読む形式は閉じた小集合(ランタイム層)。FBX/PSD 等(ソース層)はエンジンに 1 バイトも入れず、外部ツールで変換して着地させる([第5章](05_assets.md))。

> **設計決定 7(プロジェクト読み取り専有):** エンジン実行時、プロジェクトディレクトリには何も書かない。書き込み先は `user://` に分離(複数インスタンス並行起動の安全根拠)。パス解決は cwd を使わず、常にプロジェクトルート基準+脱出禁止([第3章](03_project_format.md))。

> **設計決定 8(schema + version ゲート):** 新しい交換形式の JSON には必ず `schema` と `version` を付け、不一致は hard error(規約 R10)。※歴史的経緯で asset_data.json / ui_overlay.json / rendering config には未適用。

> **設計決定 9(API ではなくデータ契約):** 外部ツール(DCC・DAM・エディタ)との統合はプラグイン API ではなくファイル契約(glTF ハブ、pelican.import、asset store)と JSON-RPC で行う。エディタ(devstudio)にも特権はない(D0: 編集操作はまず rpc メソッドとして定義される)。※WP149〜172 でこの原則が実装に落ちた: 編集 / undo / redo / 保存 / プレビューはすべて JSON-RPC メソッドとして定義され、player 内の ImGui Inspector も rpc と**同じ `EditorCommandService`** を呼ぶ([editorcommandservice.hpp](../../src/core/communication/editorcommandservice.hpp) / [inspector.cpp](../../src/core/imgui/inspector.cpp))。詳細は [第13章](13_editor.md)。

> **設計決定 10(ゲームロジック = ネイティブ C++・2026-07-07 決定):** スクリプト言語は不採用。エンジンは「ソース同居の SDK」。※ビルド形態は WP90(2026-07-12)で userpublic 境界の DLL に移行した(ホットリロードのため。境界面は従来どおり userpublic のみ — [第8章](08_gameplay.md))。※書き口は 2 つになった: シーン全体に効く**ゲームシステム**(`PELICAN_REGISTER_SYSTEM`)と、オブジェクト単位の状態と毎フレーム処理を持つ **behavior**(`PELICAN_REGISTER_BEHAVIOR`、✅WP155/162/167)。どちらも同じ DLL に載り、同じ総順序で走る。

> **設計決定 11(feature 層 = ユーザー空間・2026-07-12 決定):** エンジンが持つのは版付きの**機構語彙**(anchor / history / snapshot / format_class など)だけ。feature・カメラコントローラ・flipbook・moveAndSlide 等の「機能」はユーザー空間で、**同梱のものも特権なしの標準ライブラリ**にすぎない(コピーして改造したら自分のもの)。エンジンが名前入りエラーで守るのは invariant(anchor の全順序・色契約・決定性ゲート)だけ([第11章](11_status.md) §11.2)。

## 1.6 開発体制と文書の読み方

このリポジトリは**マルチエージェント開発**で作られています([../agent_operations.md](../agent_operations.md)):

- **ユーザー(enjoyoriori)** = 意思決定。**Claude** = 設計文書・WP(Work Package)指示書・レビュー・マージ。**codex** = 実装・監査・敵対レビュー。
- 作業単位は WP。**1 WP = 1 ブランチ(`agent/wpNN-slug`)= 1 マージ**で、受け入れ基準 = マージ基準。現在 **WP179** まで実装が着地しています([第11章](11_status.md) に全台帳。今後の候補は [../roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md))。
- 統合ブランチは `codex/rendering-phase1-refactor`(事実上の開発本線)。

文書の優先順位(矛盾したときの規則):

> **凍結済み > ドラフト、設計文書 > 指示書。** そして本マニュアルの原則として、**文書とコードが食い違ったらコードが正**(食い違い一覧は [第11章](11_status.md))。

- 設計文書の索引は [../README.md](../README.md)。凍結文書([PF] プロジェクト形式、[PFW] web プロファイル、外部ツール契約 R1〜R10、[SF] シェーダ基盤、[HL] ヘッドレス描画)の変更には版数改訂が必要です。
- WP の詳細と共通規則(ビルド・テスト・コード規約・レイヤ規則)は [../implementation_plan.md](../implementation_plan.md) §0。
- 機能を足すときのレシピ集は [../adding_features.md](../adding_features.md)。

### コード規約の要点(コードを読むときの手がかり)

- 型は CamelCase、関数は lowerCamelCase、ファイル名は小文字連結、メンバは snake_case。
- リソースは `vk::UniqueXxx` / `ImageWrapper` / `BufferWrapper`、ID 型は `PELICAN_DEFINE_HANDLE`、ログは quill(`LOG_INFO(logger, ...)`)。
- エラーは fail-fast(`throw std::runtime_error`)。
- テストは Catch2 v3。GPU 必須テストは Vulkan デバイスがなければ `SKIP()`。

## 1.7 このマニュアルの読み進め方

- **とにかく動かしたい** → [第2章 ビルドと起動](02_getting_started.md)
- **プロジェクトのファイル形式を知りたい** → [第3章](03_project_format.md) → [第4章](04_scene_ecs.md) → [第5章](05_assets.md)
- **絵作り** → [第6章 レンダリング](06_rendering.md)
- **ゲームを書く** → [第7章 入力と UI](07_input_ui.md) → [第8章 ゲームロジック](08_gameplay.md)
- **ブラウザで動かす** → [第9章 Web プロファイル](09_web.md)
- **CLI・自動化・配布** → [第10章 ツールリファレンス](10_tools.md)
- **何が実装済みで何が設計だけか** → [第11章 実装状況と文書マップ](11_status.md)
- **エンジンのソースを読む** → [第12章 コード読解ガイド](12_codemap.md)(さらに深くは [../source-code-guide/](../source-code-guide/README.md))
- **エディタ / 編集 RPC** → [第13章 エディタとエディタ RPC](13_editor.md)

## 関連文書

- [../README.md](../README.md) — 設計文書の索引(凍結/ドラフトの区分)
- [../implementation_plan.md](../implementation_plan.md) — WP 指示書と共通規則
- [../adding_features.md](../adding_features.md) — 機能追加レシピ集
- [../agent_operations.md](../agent_operations.md) — マルチエージェント運用
- [../design_project_format.md](../design_project_format.md) / [../design_project_format_web_profile.md](../design_project_format_web_profile.md) — 二大凍結文書
