# 第1章 エンジン全体像

対象: pelican2(HEAD=`82054c05`・2026-09-17。2026-07-21 に全域再確認、2026-09-04 に §1.5 の進捗を更新、2026-09-17 に v0 の段(共通グラフ / 姿勢 / package / 共有部品)を §1.0・§1.5・§1.6 に反映)/ このマニュアルはコードを正とする

## この章で学ぶこと

- Pelican2 とは何か — 構成要素(player / cli / Studio / web ビューア)と技術スタック
- リポジトリの地図(どこに何があるか)
- レイヤ構造と、コード全体を貫くレイヤ規則
- 横断的な設計原則(サブセット原則・fail-fast・strict v1・決定性・二層モデルなど)の厳密な定義
- 開発体制(設計文書 → WP 指示書 → コーディングエージェント)と文書の優先順位
- この文書全体で使う記号と基本語(WP / D0 / codec / transaction / JSON-RPC / `name@1` / 決定的 / named reject / パージ可能)

## 1.0 この文書で使う記号と基本語

マニュアル全体で断りなく出てくる語です。ここでは 1 語 1〜2 文の最小定義だけを置きます(語の索引は [第14章 用語集](14_glossary.md))。

| 語 | 意味 |
|---|---|
| **WP**(Work Package) | 開発の作業単位。台帳 [../implementation_plan.md](../implementation_plan.md) に 1 行として登録され、**1 WP = 1 ブランチ = 1 マージ**です(§1.6)。`WP380a` のような枝番が付きます。状態の一覧は [第11章 §11.1](11_status.md)。 |
| **D0** | 「**エディタ特権の禁止**」(2026-07-08 ユーザー決定 — [../design_devstudio_direction.md](../design_devstudio_direction.md) §0.5)。標準エディタも公開契約の上に建つ 1 クライアントで、編集操作はまず JSON-RPC メソッドとして定義する、という規則です([第13章](13_editor.md))。 |
| **codec**(component codec) | シーン JSON のコンポーネント 1 個を受理・検証し、正規形へ直す変換器。engine 同梱のコンポーネントごとに用意され、`decodeAuthored()`(JSON → 値)と `encodeCanonical()`(値 → 正規形 JSON)の対で成り立ちます([componentcodec.hpp](../../src/core/loader/componentcodec.hpp)・[第4章](04_scene_ecs.md) §4.2)。 |
| **transaction**(トランザクション) | 「**準備(prepare)→ 途中で失敗したら公開状態を変えずに戻す(rollback)→ 全部成功したときだけ一斉に公開(publish)**」という適用の単位。シーンロード・ECS のアーキタイプ移行・ホットリロード・編集の適用がこの形です([第12章](12_codemap.md) §12.6)。 |
| **JSON-RPC** | 外部プロセスから `pelican_player` を操る制御プロトコル(JSON-RPC 2.0)。stdin/stdout の NDJSON(1 行 = 1 リクエスト)で、stdout はプロトコル専用です(メソッド一覧は [第10章](10_tools.md) §10.3)。 |
| **`name@1`** | 機械可読な**版付き識別子**の書き方。`@` の前が名前、後ろがその名前の契約の版です。エラーコード(`component_unknown@1`)にも、パスの operation / implementation ID(`pelican.execution.compute@1`)にも使います。 |
| **決定的**(deterministic) | 同じ入力からは常に同じ結果が出ること。エンジン内部の判定に壁時計・`std::rand` を使わず、固定ループ順序・入力スナップショット・固定ステップ時刻で担保します(下記 設計決定 5、[第8章](08_gameplay.md) §8.10)。 |
| **named reject**(名指し拒否) | 受理できない入力を黙って無視も推測もせず、**何がどう悪いかを名前(エラーコード・キー名・登録済み id 一覧)付きで**拒否すること(下記 設計決定 1)。 |
| **purgeable**(パージ可能) | 参照を書いたときだけ機構が存在すること。書かなければ初期化も実行もされず、挙動もコストも変わりません(下記 設計決定 3)。 |
| **共通グラフ**(`pelican.graph` v1) | 値・描画・姿勢を **1 つの文書のノードと辺**で繋ぐ機構。`definitions`(再利用する部分グラフ)/ `nodes`(root のノード列)/ `outputs` を持ち、op は module が登録します(下記 設計決定 12)。ノード語彙は [第6章 §6.17](06_rendering.md)、編集操作は [第13章 §13.5](13_editor.md)、凍結された op 一覧は [../v0_freeze.md](../v0_freeze.md) §1.3。パーサは [graphcomposition.cpp](../../src/project/graphcomposition.cpp)。 |
| **姿勢(pose)グラフ** | 共通グラフのうち `pelican.pose` module の op(`pose.clip_sample` / `pose.blend` / `pose.state_machine` / `pose.retarget`)で姿勢を作り、sink `pose.skeletal` で個体へ書く部分。型 `pelican.pose@1` が port を流れます(bytes ではなく stage handle + layout 世代)。書き方は [第8章 §8.13](08_gameplay.md)、装着の排他は [第4章 §4.2](04_scene_ecs.md)。`PELICAN_WITH_ANIMATION=OFF` の build には module ごと存在しません。 |
| **`graph.instance`(装着)** | 共通グラフの定義を 1 つ実体化するノード。`owner: {scene, object}` を書くとその個体に結び付き、入力 port の値は **辺 > `parameters` > 定義の既定値**の優先で決まります([第4章 §4.1](04_scene_ecs.md))。 |
| **package**(`pelican.package` v1) | 本体ツリーの外で作った部品一式(feature / graph / component / code)を 1 ディレクトリにまとめたもの。使うときは project へ**複製(vendoring)**します([第3章 §3.11](03_project_format.md))。 |
| **共有部品(render component)** | materialize 済みの feature 1 本を `user://components/<name>/` へ書き出し、別 project へ取り込める形にしたもの(`pelican.render_component` v1)。[第3章 §3.12](03_project_format.md)。 |
| **`rendering` 節** | rendering config が `pelican.graph` root の下に入るときの置き場所。文書の最上位は共通グラフが所有し、rendering config のキーは全部 `rendering` の下に来ます([第3章 §3.2](03_project_format.md)・[第6章 §6.16](06_rendering.md))。 |
| **root motion** | clip に焼かれた「根の移動」を、姿勢ではなく**個体の transform** に渡すこと。`pose.clip_sample{root_motion: "extract", root_node}` で有効になり、owner の local 位置に加算されます([第4章 §4.2](04_scene_ecs.md)・[第8章 §8.13](08_gameplay.md))。 |

## 1.1 Pelican2 とは

Pelican2 は **C++20 / Vulkan(vulkan.hpp)/ Windows(MSVC)** のゲームエンジンです。ゲームは「**プロジェクト**」(`project.json` + JSON データ + アセット + 任意の C++ コード)として作り、エンジン本体のコードには手を入れません。

実行物は通常3つ、experimental build unitを有効にすると4つ + web ビューアです:

| 実行物 | 役割 |
|---|---|
| `pelican_player` | ゲームランタイム。ウィンドウ / ヘッドレス / JSON-RPC 駆動([第2章](02_getting_started.md)・[第10章](10_tools.md))。JSON-RPC はヘッドレス(blocking)とウィンドウ(フレーム境界処理)の両方で使えます ✅WP156 |
| `pelican_cli` | 開発 CLI。11 サブコマンド系統(`assets` / `audio` / `bake-camera` / `image` / `import` / `dist-config` / `package` / `project` / `dump-lowered-material` / `vrm` / `graph-catalog` — [main.cpp](../../src/devcli/main.cpp#L51))。エンジン本体にリンクしない |
| `pelican_studio` | Qt6 製エディタ 🚧。別 process の player を埋め込み、**その JSON-RPC を実際に呼びます** —— Outliner 選択、Inspector、gizmo、保存、Frame Plan(= フレームグラフを解決した実行計画。[用語集](14_glossary.md))、そして `features[]` の編集と再起動なしの反映([第2章](02_getting_started.md) §2.7・[第10章](10_tools.md) §10.7・[第13章](13_editor.md)) |
| `pelican-spv-link` | `PELICAN_WITH_SPIRV_LINK=ON`時だけ作るexperimental SPIR-V リンカ CLI([spvlink/main.cpp](../../src/spvlink/main.cpp)・[第10章](10_tools.md)) 🚧 |
| web ビューア | 別リポジトリ `my_webpage` の WebGPU「Shader Dock」。**同じプロジェクトファイルをブラウザで開く**([第9章](09_web.md)) |

特徴を一言でいうと: **「データ(JSON)で宣言し、C++ で振る舞いを書き、すべてを決定的・検証可能にする」** エンジンです。レンダリングパイプラインは JSON だけで定義でき([第6章](06_rendering.md))、ゲームロジックはネイティブ C++(ホットリロード可能な DLL — [第8章](08_gameplay.md))、実行はヘッドレス+JSON-RPC で完全に自動化できます。

## 1.2 データフロー(起動から画面まで)

```
project.json ──┬─ scene_data_json ──▶ pelican.scene v2 ─▶ ECS(オブジェクト)
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

図の読み方: `──▶` は「左のものが右を作る/右へ渡す」向き、`├▶` `└▶` は 1 つの入力から複数の行き先へ分かれる枝、`▲` と `┘` は下から上への合流(ゲームコード `code/*.cpp` が `GameContext` 経由でその上の実体を毎フレーム操作する)を表します。左端の `project.json` から入って、右端の ECS・描画に着地します。`GameContext` は**ゲームコードに公開される唯一の API 面**(facade)で、時刻・入力・オブジェクト操作はすべてここを通ります([gamecontext.hpp](../../src/core/userpublic/gamecontext.hpp)・[第8章](08_gameplay.md))。

毎フレームの流れ(固定順序 — 決定性の土台):

```
時刻 advance → イベント凍結 → 入力スナップショット → アクション凍結 → イベント配送
→ ECS システム → ゲームシステム(order 順。BehaviorSystem も order=50 として同じ総順序(= 全ゲームシステムが 1 列に並ぶ実行順。定義はこの節の末尾)に参加)
→ シーン遷移の適用 → 描画
```

`behavior` はシーンのオブジェクトに直接ぶら下がる毎フレーム処理です。シーンロード時に `behavior` コンポーネントだけが ECS の振り分けから外れて BehaviorAttachmentArena(**arena** = エンジン側が behavior の実体をまとめて所有する置き場)に渡り([scene.cpp](../../src/core/loader/scene.cpp) → [behaviorarena.cpp](../../src/core/gamelogic/behaviorarena.cpp))、実行はゲームシステムと同じ総順序に `order=50` / 名前 `"BehaviorSystem"` で参加します([behavior.hpp](../../src/core/userpublic/behavior.hpp))。ここでいう**総順序**は、登録された全ゲームシステムを `order`(登録時に書く整数)の昇順、同じ `order` なら登録名の辞書順に並べた、1 つに定まる実行順のことです([sortGameSystemRegistrations()](../../src/core/userpublic/details/system/registerer.cpp#L35))。書き方は [第8章](08_gameplay.md)、JSON 側の語彙は [第4章](04_scene_ecs.md) を参照してください。

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
│   │   ├── audio/        #   WAV / Ogg Vorbis 再生(miniaudio。se / bgm バス、長い音は streaming)
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
│   └── spvlink/          # pelican-spv-link CLI(experimental、build unitは既定OFF)
├── projects/             # example / sprite_demo / animgraph_demo / posegraph_demo /
│                         #   vrm_xr_demo / raytracing_demo / pathtrace_demo(JSON 例の出典)
├── docs/                 # 設計文書(索引: docs/README.md)+ 本マニュアル(manual/)+ コード解説(source-code-guide/)
├── tools/                # 開発者・エージェント用スクリプト(RPC クライアント / worktree ラッパ /
│                         #   切り離し build / 公開 API inventory / doclink)。第10章 §10.10
└── test/                 # Catch2 単体 + fixture + golden + 結合スクリプト(run_*)。件数は第12章
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

> **設計決定 3(パージ可能 — 参照 = 存在):** feature・compute・actions・collider などは、**参照を書いたときだけ存在**する。参照しなければその機構は**初期化も実行もされず**(モジュールごと生成されない)、挙動もコストも完全に不変(ゴールデンテストで機械的に証明)。エンジンに機能を「埋め込まない」。

> **設計決定 4(サブセット原則):** 「web で開けるプロジェクト ⊆ pelican で開けるプロジェクト」。形式拡張は常にエンジン先行。web 専用の形式要素は作らない([第9章](09_web.md))。

> **設計決定 5(決定性):** エンジン内部の判定に壁時計・`std::rand`・`random_device` を使わない。固定ループ順序・入力スナップショット・決定的システム実行順・フレーム境界のイベント配送・PCG32 乱数(既定シード 0 — シードから列が一意に決まる決定的な擬似乱数生成器。[deterministicrng.cpp](../../src/core/userpublic/deterministicrng.cpp))・固定ステップ時刻(ヘッドレス)により、**同じ入力からは常に同じ結果**が出る([第8章](08_gameplay.md) §8.10)。

> **設計決定 6(二層モデル — アセット):** エンジンが読む形式は閉じた小集合(ランタイム層 = glb/VRM・PNG/JPG・EXR・WAV / Ogg Vorbis・JSON/JSONL・SPIR-V/GLSL)。FBX/PSD 等(ソース層)はエンジンに 1 バイトも入れず、**外部ツール**(`pelican_cli import` や import-tools)で変換し、`imports/` にファイルとして着地させる(隠れインポートキャッシュは作らない。一覧と手順は [第5章](05_assets.md) §5.1)。

> **設計決定 7(プロジェクト読み取り専有):** エンジン実行時、プロジェクトディレクトリには何も書かない。**例外は利用者が明示的に行う編集の保存だけ**で、書き込みはすべて名前の付いた操作から始まります。書き込み先は `user://` に分離(複数インスタンス並行起動の安全根拠)。パス解決は cwd を使わず、常にプロジェクトルート基準+脱出禁止([第3章](03_project_format.md))。※**「編集の保存」の範囲は v0 で広がりました**: シーンの保存と `features[]` の編集([第13章](13_editor.md))に加えて、`install_package`(`project://packages/<ns>/` への複製 + `project.json` の `packages` への記録 — [第3章 §3.11](03_project_format.md))、`uninstall_package`(その取り消し)、`import_render_feature`(`project://` に feature ファイルを 1 本新規作成 — [第3章 §3.12](03_project_format.md))、`materialize`(engine 参照の feature を project 側の派生として保存 — [第6章](06_rendering.md))が project に書きます。どれも利用者が明示的に呼んだときだけで、エンジンが自発的に書くものはありません。`export_render_feature` の書き先は project ではなく `user://components/` です。

> **設計決定 8(schema + version ゲート):** 新しい交換形式の JSON には必ず `schema` と `version` を付け、不一致は hard error(規約 R10)。v0 では 1 つの形式に 1 つの版だけを読みます(旧い版は移行せず書き直す)。`pelican.scene` と `pelican.asset_data` は version 2 だけで、それ以外は `pelican.<schema>.version_unsupported@1`(✅MG-2b。[sceneformat.cpp](../../src/project/sceneformat.cpp#L123) / [assetdataformat.cpp](../../src/project/assetdataformat.cpp#L112))。凍結対象の schema 一覧は [../v0_freeze.md](../v0_freeze.md) §1.1(31 種)。rendering config は MG-2b から `pelican.graph` root だけで、文書の最上位が共通グラフ文書なので `schema: pelican.graph` / `version: 1` を厳密に検査します。最上位に rendering のキーを直接並べた旧い形(legacy root)は `render_config.legacy_root@1` で拒否されます([第3章 §3.2](03_project_format.md))。`ui_overlay.json` も `pelican.ui` / `version: 1` を検査します([document.cpp](../../src/core/ui/document.cpp#L372))。

> **設計決定 9(API ではなくデータ契約):** 外部ツール(DCC・DAM・エディタ)との統合はプラグイン API ではなくファイル契約(glTF ハブ、pelican.import、asset store)と JSON-RPC で行う。エディタ(devstudio)にも特権はない(D0: 編集操作はまず rpc メソッドとして定義される)。※WP149〜172 でこの原則が実装に落ちた: 編集 / undo / redo / 保存 / プレビューはすべて JSON-RPC メソッドとして定義され、player 内の ImGui Inspector も rpc と**同じ `EditorCommandService`** を呼ぶ([editorcommandservice.hpp](../../src/core/communication/editorcommandservice.hpp) / [inspector.cpp](../../src/core/imgui/inspector.cpp))。詳細は [第13章](13_editor.md)。

> **設計決定 10(ゲームロジック = ネイティブ C++・2026-07-07 決定):** スクリプト言語は不採用。エンジンは「ソース同居の SDK」。※ビルド形態は WP90(2026-07-12)で userpublic 境界の DLL に移行した(ホットリロードのため。境界面は従来どおり userpublic のみ — [第8章](08_gameplay.md))。※書き口は 2 つになった: シーン全体に効く**ゲームシステム**(`PELICAN_REGISTER_SYSTEM`)と、オブジェクト単位の状態と毎フレーム処理を持つ **behavior**(`PELICAN_REGISTER_BEHAVIOR`、✅WP155/162/167)。どちらも同じ DLL に載り、同じ総順序で走る。

> **設計決定 11(feature 層 = ユーザー空間・2026-07-12 決定):** エンジンが持つのは版付きの**機構語彙**(anchor / history / snapshot / format_class など)だけ。feature・カメラコントローラ・flipbook・moveAndSlide 等の「機能」はユーザー空間で、**同梱のものも特権なしの標準ライブラリ**にすぎない(コピーして改造したら自分のもの)。エンジンが名前入りエラーで守るのは invariant(anchor の全順序・色契約・決定性ゲート)だけ([第11章](11_status.md) §11.2)。

> **設計決定 12(module 登録契約 — 共通グラフの op も「同梱の標準ライブラリ」・✅AG-0/AG-1a):** 共通グラフのノードの語彙は、コードに埋め込まれた表ではなく **module の manifest**([GraphModuleManifest](../../src/project/graphmodule.hpp#L35): 名前空間・版・提供する型・op の契約・sink)で決まります。`registerModule` が登録し、`resolveOperation` が文書のノードを契約へ引き当てます([graphmodule.hpp:51](../../src/project/graphmodule.hpp#L51))。エンジン同梱の 5 module(`pelican.value` / `pelican.audio` / `pelican.render` / `pelican.pose` / `pelican.ui`)も**この入口を通るだけ**で、特権はありません([makeBuiltinGraphModuleRegistry()](../../src/project/graphmodule.cpp#L737))—— 設計決定 11 と同じ形が、feature 層だけでなくノード語彙にも適用されています。
>
> 帰結は 2 つです。**(a) 登録されていない op は名指しで拒否される** —— `graph.operation_unregistered@1{node, op}`([graphmodule.cpp:257](../../src/project/graphmodule.cpp#L494))。`PELICAN_WITH_ANIMATION=OFF` の build は `pelican.pose` module を登録しないので、`pose.*` を書いた文書はこの名前で止まります(黙って無視されません)。**(b) 契約はあるが実行 backend が無い op も別の名前で拒否される** —— `graph.operation_unexecutable@1{node, op, backend}`([valuegraph.cpp:289](../../src/core/userpublic/valuegraph.cpp#L554))。「語彙に載っている」と「動く」は別の事実として扱われます。manifest の二重登録も同様に名前付き(`graph.module_duplicate@1` / `graph.operation_duplicate@1` / `graph.type_duplicate@1` / `graph.sink_unregistered@1` — [graphmodule.hpp:51](../../src/project/graphmodule.hpp#L51))で、**拒否された manifest は何も登録しないので、登録順で意味が変わることはありません**。登録済みの語彙は `pelican_cli graph-catalog` で一覧でき([第10章](10_tools.md))、その出力は [../v0_freeze.md](../v0_freeze.md) §1.3 の表と test で照合されています。

## 1.6 開発体制と文書の読み方

このリポジトリは**マルチエージェント開発**で作られています([../agent_operations.md](../agent_operations.md)):

- **ユーザー(enjoyoriori)** = 意思決定。**Claude** = 設計文書・WP(Work Package)指示書・レビュー・マージ。**codex** = 実装・監査・敵対レビュー。
- 作業単位は WP。**1 WP = 1 ブランチ = 1 マージ**で、受け入れ基準 = マージ基準。番号は連番ではなく、`380a` `380a-fix` のような枝番が付きます。**2026-09-16〜17 の v0 の段は通し番号ではなく記号 ID**(`AG-0` / `AG-1a` / `AG-1b` / `P-1` / `P-2a-1a〜4b` / `AG-2` / `AG-3a` / `O-1` / `X-1` / `P-2b` / `R-1` / `R-2` / `R-3` / `R1-2` / `N-1` / `N-1b` / `N-2`)で登録されており、これも同じ「1 段 = 1 ブランチ = 1 マージ」です。**行単位の正は [../implementation_plan.md](../implementation_plan.md)**、v0 の段の SHA 一覧は [第11章 §11.1](11_status.md) の v0 表と [../v0_freeze.md](../v0_freeze.md) §5 にあります([第11章](11_status.md) の通し番号の表は WP238e + WP375c〜385 までの転記です)。今後の候補は [../roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md) と [../v1_development_plan.md](../v1_development_plan.md)。
- 統合ブランチは `codex/render-target-runtime-slice`(事実上の開発本線)。

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
