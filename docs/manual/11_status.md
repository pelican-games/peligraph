# 第11章 実装状況と文書マップ

対象: pelican2(2026-07-16 時点、branch `codex/rendering-phase1-refactor`)/ このマニュアルはコードを正とする

## この章で学ぶこと

- WP(Work Package)全台帳 — 何が実装済みで、何が登録のみ・予約なのか
- 設計文書マップ — docs/ の各文書と本マニュアルの章の対応、状態(凍結/ドラフト/古い)
- 既知の「設計文書とコードの食い違い」一覧
- 本マニュアル自体の更新ルール

## 11.1 WP 全台帳

判定は「implementation_plan.md の記載 + 実装コミット + design_reviews のレポート実在」によります(2026-07-16 調査)。

凡例: **済** = マージ済みで機能する / **未** = 登録済み・未実装 / **予約** = 番号予約のみ。

### WP1〜63(基盤期。詳細は各章)

| WP | 内容 | 状態 |
|---|---|---|
| 1 | EngineLaunchConfig + player CLI | 済 |
| 2 | EngineTime(仮想時刻の注入) | 済 |
| 3 | RenderTarget の facade 化 | 済 |
| 4 | surface なし Vulkan 起動 | 済 |
| 5 | OffscreenFrameTarget | 済 |
| 6 | readback + PNG 出力 + CLI 完成 | 済 |
| 7 | headless 描画テスト | 済 |
| 8 | リサイズ時の RT 追従 | 済 |
| 9 | 遅延破棄機構(DeletionQueue) | 済 |
| 10 | shaderc / spirv-reflect / stb 導入 | 済 |
| 11 | ShaderCompiler + ShaderReflection | 済 |
| 12 | ShaderLibrary | 済 |
| 13 | PipelineFactory | 済 |
| 14 | シェーダホットリロード | 済 |
| 15 | Material/UiRenderer 移行 + descriptor set 規約 | 済 |
| 16 | ゴールデンイメージテスト基盤 | 済 |
| 17 | SeqPlayer(transform_seq 再生) | 済 |
| 18 | プロジェクト形式(project.json + PathResolver + example 切り出し) | 済 |
| 19 | シェーダ stem 解決 + rendering config の struct 化 | 済 |
| 20 | VAT 再生(pelican.vat v1) | 済 |
| 21 | devcli `import`(pelican.import manifest) | 済 |
| 22 | pointcache | 予約 |
| 23 | KTX2 | (WP92 として実装済み) |
| 24 | 音声 | (WP51 = A1 として実装済み) |
| 25 | pelican.scene v1 | 済 |
| 26 | EXR リーダ(tinyexr・限定スコープ) | 済 |
| 27 | コマンド層 stage 2(stdio JSON-RPC 2.0) | 済 |
| 28 | feature 合成基盤 | 済 |
| 29 | debug_draw feature + GPU/CPU 計測 | 済 |
| 30 | HDR / トーンマップ feature | 済 |
| 31 | shadow directional feature | 済 |
| 32 | IBL | 予約 |
| 33 | フレームグラフ F0(プランナ) | 済 |
| 34 | フレームグラフ F1(compute 実行系) | 済 |
| 35 | フレームグラフ F2(プランダンプ + rpc) | 済 |
| 36 | GPU パーティクル | 予約 |
| 37 | 入力システム(スナップショット) | 済 |
| 38 | スケルタルアニメーション(クリップ v1) | 済(2026-07-12。`#animation/` 参照 + matrix palette) |
| 39 | 入力アクション層 I1 | 済 |
| 40 | ビルドユニット化 B1(PELICAN_WITH_* + OFF スモーク) | 済 |
| 41 | `pelican_cli dist-config` B2 | 済 |
| 42 | コマンド層 stage 3(get_status / update_transforms / load_gltf) | 済 |
| 43 | ゲームシステム登録 API G1a | 済 |
| 44 | pelican_project ターゲット分離(解釈レイヤ) | 済 |
| 45 | プロジェクトコード取り込み G1b(PELICAN_PROJECT) | 済 |
| 46 | 物理クエリ P1(純ロジック) | 済 |
| 47 | 物理クエリ P2(collider シーン接続) | 済 |
| 48 | カメラ C1(glTF 1:1 投影 + set_camera) | 済 |
| 49 | 入力 I2(rpc inject_input) | 済 |
| 50 | カメラ C2(orbit / follow / fly) | 済 |
| 51 | オーディオ A1(WAV + バス + null バックエンド) | 済 |
| 52 | シーン遷移 S1(loadScene) | 済 |
| 53 | 決定性乱数(PCG32) | 済 |
| 54 | debug_text feature | 済 |
| 55 | [PF] v6.3 リゾルバ拡張(user:// / asset store / #フラグメント構文) | 済 |
| 56 | イベント層 E1 | 済 |
| 57 | project init 雛形 V3 | 済 |
| 58 | マテリアル M1(pelican.material パーサ + shader_contract.md) | 済(レンダラ接続は WP68/70/76/78 で完了) |
| 59 | スパイク pelican-spv-link | 済(スパイク。本実装は WP80 = experimental) |
| 60 | リファクタ R1(死荷重削除。`--project-settings` 廃止等) | 済 |
| 61 | リファクタ R2(カメラキー正規形移行・データのみ) | 済 |
| 62 | リファクタ R5-core(ECS ライフサイクル + 世代 ID) | 済 |
| 63 | リファクタ R3(strict v1 化 — 旧キー・レガシー受理の全廃) | 済 |

### WP64〜111(機能拡張期 2026-07-10〜16)

| WP | タイトル | 状態 | 1行説明 | ユーザーから見える面 |
|---|---|---|---|---|
| 64 | リファクタ R4 — legacy 実行系の撤去 | 済 | 全 rendering config が planned frame graph 実行に一本化(A/B trace 差分ゲート付き) | 純 render 構成でも `before`/`after` 明示エッジが効く(挙動修正) |
| 65 | 永続化 P1(user:// 設定 + セーブ) | 済 | pelican.settings v1(engine/game 区画・atomic 書き込み)+ user://saves/ | API: `GameContext::saveSettings()` / `saveData(slot,json)` / `loadData(slot)` / `listSaves()` |
| 66 | assets manifest V2 | 済 | store 走査 → sha256 manifest 生成・照合・起動時検証(INFO/WARN/ERROR、ロードは止めない) | CLI: `pelican_cli assets manifest/verify/status`、起動フラグ `--strict-assets` |
| 67 | modelview 統合(R5 後続) | 済 | SimpleModelView 2 コンポーネントを統合、隠し自動生成を撤去 | scene JSON の `simplemodelview.model` の書き味は不変 |
| 68 | マテリアル M2a — .surface パーサ | 済 | `//! pelican.surface v1` ヘッダ(ordered params / textures / screen_inputs)+ material JSON の values 方式 | JSON: `.surface` ヘッダ形式、`.material.json` の `values` |
| 69 | FrameInput(入力の順序付きイベント化) | 済 | ordered InputEvent + event_seq + 5 段フレーム位相 + 消費マスク API | API: `FrameInput{ordered_events, snapshot}`(通常/rpc loop 位相同一) |
| 70 | マテリアル M2b-1 — FrameUBO | 済 | set0 b0 FrameUBO 新設(time が全マテリアルへ)、material データの SSBO 化、push constant 64B/64B 分割 | シェーダ契約: FrameUBO(time/dt/frame_index/resolution/camera/view/proj)。shader_contract.md の差分 3 件解消 |
| 71 | EventPayloadSchema | 済 | イベント payload の explicit descriptor + 構築前 JSON 検証 | Opaque の by-name emit は廃止(typed C++ emit のみ)。rpc emit の payload 検証規則が確定 |
| 72 | 色 C0 — 色空間監査 | 済 | コード変更なしの全域監査 | 成果物: `docs/color_migration_manifest.json` |
| 73 | 色 C1a — canonical anchor(構造のみ) | 済 | canonical anchor 列 / format_class / output_transform 常設ノード(golden byte 不変) | frame plan に anchor / output_transform が現れる |
| 74 | 色 C1b — 色意味論の一括移行 | 済 | SRGB swapchain + linear workflow へ全面移行、golden 再基準化(resolver_version: 2) | 画の色が正しくなる(一度きりの再基準化)。SRGB/UNORM の role 別 view |
| 75 | UI U0 — 純 CPU の UI 基盤 | 済 | pelican.ui schema / layout / 入力状態機械 / semantic validator | JSON: `pelican.ui` v1(schema 正本 = `docs/schemas/pelican.ui.schema.json` ほか) |
| 76 | マテリアル M2b-2 — values バインダ | 済 | .surface params → std140 SSBO、textures 辞書 binding(SRGB/UNORM view)、render_state | CLI: `pelican_cli dump-lowered-material <surface>` |
| 77 | コンテナ K1 — #フラグメント実ロード | 済 | glb の `#mesh/#material/#node/#animation` 部分ロード | asset_data / scene から `foo.glb#mesh/名前` 参照が使える |
| 78 | マテリアル M3a — B 層ソース経路 | 済 | .surface コードスニペット → テンプレート合成(`pelican_*_v1` フック反射、engine:// GLSL ライブラリ) | .surface に `pelican_surface_v1` 等を書くだけでカスタムシェーダ。standard/toon は同経路の dogfood |
| 79 | コンテナ K2 — glTF シーン抽出 + 親子 | 済 | scene v1 に `objects[].parent` 追加 + glTF ノード階層の抽出 | CLI: `pelican_cli import gltf --extract-scene <glb>`。JSON: scene v1 `parent` キー |
| 80 | マテリアル M3b — spv-link | 済 | SPIR-V リンクの experimental バックエンド(既定は WP78 ソース経路) | opt-in: `PELICAN_SPV_LINK=experimental`。昇格判定は保留 |
| 81 | コンテナ K3 — pelican-import-tools | 済(別リポジトリ) | Python 製外部ツール: psd_extract + atlas_pack(決定的パッキング) | 出力: `pelican.atlas` v1 JSON(`#sprite/<名前>` の解決先) |
| 82 | 起動高速化 | 済 | シェーダディスクキャッシュ(`.pelican/shader_cache/`)+ モデルロード並列化(決定的) | 起動 1 行レポート + `get_status.startup`。2 回目以降の起動が大幅短縮 |
| 83 | マテリアル M3.5 — スクリーンスナップショット | 済 | rendering config `"snapshots"` + `screen_inputs` 宣言で forward 自動振り分け | JSON: `"snapshots":[{"name","after"}]`、.surface `//! screen_inputs: [...]`、シェーダ `pelican_screen_<name>(uv)` |
| 84 | コンテナ K4 — import ルール表 | 済 | `imports.rules.json`(glob → レシピ)。外部ツール契約で起動 | CLI: `pelican_cli import --rules` |
| 85 | ImGui 導入 | 済 | PELICAN_WITH_IMGUI ユニット(dist は OFF)。golden/replay/headless では非実行 | F1(actions 経由)でデバッグ UI トグル |
| 86 | Plan viewer(ImGui) | 済 | frame plan のノードグラフ可視化(パス = ノード、RT = エッジ) | ImGui 内ツール。データ源は get_frame_plan と同一意味論 |
| 87 | UI U1 — GPU 描画 | 済 | quad buffer(20B 頂点)+ pelican.atlas 接続 + nested clip + ui の purgeable feature 化 | `engine://features/ui.json`。UI の `#sprite/` 参照。**旧 ui_overlay images 形式は廃止** |
| 88 | temporal T1+T2 — history + velocity | 済 | RT 宣言 `"history": true`(2 面自動管理・`@history` 読み)+ velocity feature(RG16F) | JSON: RT の `history` キー、`@history` 参照。TAA 本体は未実装(ユーザー領分) |
| 89 | 入力 I3 — 収録とリプレイ | 済 | ordered InputEvent 列を `pelican.input_seq` v1(JSONL)へ収録・リプレイ(2 回 byte 一致) | rpc: `start_input_record`/`stop_input_record`、CLI: `--replay <file>`、`pelican_cli bake-camera --replay` |
| 90 | G2 — ゲームロジック DLL ホットリロード | 済 | 実行中の再ロード(v1 = 全リセット方式)。ABI 版数ゲート・失敗時は旧 DLL 継続 | 保存 → 数秒で反映。リロード中 rpc/replay 拒否 |
| 91 | 入力 I4 — ゲームパッド + プロファイル | 済 | GLFW gamepad API。actions を「アクション定義」と「バインディングプロファイル」に分離 | JSON: `profiles/*.json`(起動引数/設定/rpc で切替)。pad 入力は収録リプレイ対応 |
| 92 | KTX2 テクスチャレーン | 済 | KTX2 制限サブセット自前パース(RGBA8 / BC7 / BC5、supercompression なし・全ミップ必須) | KTX2 が material textures 辞書・UI atlas で使える |
| 93 | UI U2 — 対話ウィジェット | 済 | bitmap label / button + E1 emit(WP71 照合・次フレーム配送)+ rpc click/replay の決定性 | UI JSON の emit 文法 |
| 94 | アニメ A0 — 公開契約の凍結 | 済 | animation ABI v1 の凍結(handle/struct_size/negotiation) | 正本: `src/core/userpublic/animation/abi_v1.hpp` + [animation_abi_v1.md](../animation_abi_v1.md) |
| 95 | skinned velocity の previous palette | 済 | 骨だけ動く場合の deformation velocity = 0 を解消 | velocity buffer が骨変形を正しく反映 |
| 96 | アセット HR0 — watcher/reconcile 基盤 | 済 | Win32 FileWatcher 状態機械 + ContentDigest + gate epoch(リプレイ/strict/rpc 中無効) | `get_status.reload`(watcher 状態) |
| 97 | アニメ A1 — 機構 jobs | 済 | 凍結 ABI の実体化(samplePoseAt / advanceCursor / N-way blend / commit) | ABI v1 の全関数が実動作。既存 clip component の挙動不変 |
| 98 | アセット HR1 — identity/transaction 基盤 | 済 | LogicalAssetId / generation / content_revision の三分離 + staged commit + rollback | `get_status.reload` に resource counters/errors |
| 99 | アニメ A1.5 — 敵対 fixture | 済 | 別 rig 誤ブレンド検出・loop 跨ぎ・stale handle 等の敵対検証 | (テスト資産のみ) |
| 100 | アセット HR1-T — テクスチャ差し替え | 済 | png/EXR/KTX2 の watch → in-place / descriptor 再バインド | 画像保存 → 実行中の絵が更新。失敗時は旧絵継続 |
| 101 | アニメ A2 — anim_graph v1 + 標準評価器 | 済 | `pelican.anim_graph` v1(state/blend1d/crossfade/interrupt)+ ユーザー空間標準評価器。※公開語彙不足で一度実装前停止 → WP102 着地後に完了 | JSON: `pelican.anim_graph` v1。example に歩き↔走り + ジャンプ割込みデモ |
| 102 | アニメ A1.1 — 公開 service surface | 済 | versioned service table を additive 追加(sink claim/release・phase callback 自動失効) | 第三者 game DLL が公開面だけで評価器を書ける |
| 103 | 2D S2D-0a — スプライト契約 + CPU 基盤 | 済 | `sprite_view` コンポーネント + SpriteCommand ABI + sort total key(GPU なし) | JSON: scene の `sprite_view`(asset 宣言 + `#sprite/` 参照) |
| 104 | 2D S2D-0b — GPU world quad パス | 済 | world 空間スプライト描画(3D 不透明の後、depth test ON / write OFF)+ billboard | スプライトが 3D と遮蔽し合って表示される |
| 105 | アセット HR1-M — material values 差し替え | 済 | `.material.json` の watch → 同 layout なら SSBO 値のみ update | material JSON 保存 → 実行中に値反映 |
| 106 | 2D S2D-1 — strict pixel policy + flipbook | 済 | render_only_quantization 方式の pixel perfect + 公開 FlipbookClip | JSON: `basic_config.sprite.pixels_per_unit`、camera の `sprite.pixel_perfect/sort` |
| 107 | 2D S2D-P — shapeCast + Provider ABI V2 | 済 | 固定姿勢 sweep(sphere/box/capsule 全 9 組・TOI/MTD)+ 物理 Provider ABI V2 | API: GameContext に `shapeCast` all/closest。collider に `layer/mask/trigger/one_way` |
| 108 | アセット HR2-S — shader/surface transaction | 済 | 単一 FileWatcher 経路化 + 全 variant/pipeline の atomic 差し替え + cross-file transaction | シェーダ/.surface 保存 → 実行中に atomic 反映 |
| 109 | 2D S2D-2 — side-scroller vertical slice | 済 | 非特権 `moveAndSlide`(`src/core/userpublic/platformer/`)+ 接地/斜面/one-way、replay 2 回 bit 一致 | `projects/sprite_demo` が操作可能なプラットフォーマーに |
| 110 | アセット HR2-G — model/fragment hot reload | 済 | 同一 container の全 fragment 一括 transaction + live instance の bulk rebuild | glb 保存 → 配置済みインスタンスが実行中に差し替わる |
| 111 | VRM-S0 — .vrm semantic decoder | 済(2026-07-16) | VRM 1.0(`VRMC_vrm`)の humanoid/expressions/lookAt を保持・検証・canonical dump(renderer 適用なし)。VRM 0.x は素の GLB + INFO | CLI: `pelican_cli vrm dump`。実装 `src/core/model/vrmsemantic.*` |

**要するに: WP1〜111 はすべて実装済み。残りは予約 3 件(WP22 pointcache / WP32 IBL / WP36 GPU パーティクル)のみです。**

WP 番号外の 2026-07-15 リファクタ群(design_reviews/ にレポートあり): module access hardening、PhysQuery 決定的クエリ基盤、purgeable physics provider boundary + provider DLL lifecycle。

今後の候補は [../roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md)(47 候補・6 ティア)が正です。Tier 1 の残りは HR2-I(input/profile hot reload)と U3(UI hot reload)、直近の推奨は TAA/USD 設計の敵対レビュー → J1/T-TAA・M-PBR0 の実装です。

web 側(my_webpage)の WW 台帳は [第9章](09_web.md) §9.6 を参照してください。

## 11.2 設計文書マップ(docs/ ⇔ 本マニュアル)

矛盾時の優先順位: **凍結済み > ドラフト、設計文書 > 指示書**。ここでの「状態」は 2026-07-16 のコード実態基準です(docs/README.md の表記は大幅に古い — §11.3)。

### 凍結・規範文書

| 文書 | 版 | 内容 | 対応する章 |
|---|---|---|---|
| [design_project_format.md](../design_project_format.md) [PF] | v6.3 | プロジェクト形式・PathResolver | [第3章](03_project_format.md) |
| [design_project_format_web_profile.md](../design_project_format_web_profile.md) [PFW] | v1.3 | web 解釈規則・サブセット原則 | [第9章](09_web.md) |
| [external_tools_requirements.md](../external_tools_requirements.md) | R1〜R10 | 外部ツール契約(glTF ハブ、transform_seq、JSON-RPC) | [第5章](05_assets.md)・[第10章](10_tools.md) |
| [design_shader_freedom_kit.md](../design_shader_freedom_kit.md) [SF] | — | シェーダ基盤(実装済み) | [第6章](06_rendering.md) |
| [design_headless_rendering.md](../design_headless_rendering.md) [HL] | — | ヘッドレス描画(実装済み) | [第2章](02_getting_started.md)・[第6章](06_rendering.md) |
| [animation_abi_v1.md](../animation_abi_v1.md) | **Frozen / normative** | アニメーション ABI v1(正本 header = `abi_v1.hpp`) | [第8章](08_gameplay.md) |

### ドラフト(実装済み or 実装中)

| 文書 | 実装状態 | 対応する章 |
|---|---|---|
| [design_scene_format.md](../design_scene_format.md) | ✅ WP25/63/79(`parent` は WP79 で追加) | [第4章](04_scene_ecs.md) |
| [design_ecs_lifecycle.md](../design_ecs_lifecycle.md) | ✅ WP62 | [第4章](04_scene_ecs.md) |
| [design_render_feature_modules.md](../design_render_feature_modules.md) | ✅ WP28〜31(feature = ユーザー空間ポリシーは §11.1 方針参照) | [第6章](06_rendering.md) |
| [design_compute_task_graph.md](../design_compute_task_graph.md) | ✅ WP33〜35 + WP64(実行一本化済み) | [第6章](06_rendering.md) |
| [design_material_shading.md](../design_material_shading.md) | ✅ M1〜M3.5(WP58/68/70/76/78/80/83)。spv-link は experimental | [第6章](06_rendering.md) |
| [design_color_pipeline.md](../design_color_pipeline.md) | ✅ v4 Accept・WP72〜74 で実装完了 | [第6章](06_rendering.md) |
| [design_postprocess_temporal.md](../design_postprocess_temporal.md) | ✅ T1/T2(WP88/95)。TAA は [design_taa_jitter.md](../design_taa_jitter.md) へ | [第6章](06_rendering.md) |
| [design_taa_jitter.md](../design_taa_jitter.md) | 📐 v1 ドラフト(敵対レビュー前) | [第6章](06_rendering.md) |
| [design_usd_openpbr.md](../design_usd_openpbr.md) | 📐 v1 ドラフト(敵対レビュー前) | [第5章](05_assets.md)・[第6章](06_rendering.md) |
| [shader_contract.md](../shader_contract.md) | ✅(FrameUBO/SSBO/resources manifest まで反映) | [第6章](06_rendering.md) |
| [design_input_actions.md](../design_input_actions.md) | ✅ I1〜I4 すべて(WP39/49/89/91) | [第7章](07_input_ui.md) |
| [design_ui_2d_foundation.md](../design_ui_2d_foundation.md) | ✅ v8 Accept・U0/U1/U2 実装済み(WP75/87/93) | [第7章](07_input_ui.md) |
| [design_2d_game_layer.md](../design_2d_game_layer.md) | ✅ v2.4・S2D-0a〜2 実装済み(WP103/104/106/107/109) | [第4章](04_scene_ecs.md)・[第6章](06_rendering.md)・[第8章](08_gameplay.md) |
| [design_event_payload_schema.md](../design_event_payload_schema.md) | ✅ v2 Accept・WP71 で実装 | [第8章](08_gameplay.md) |
| [design_animation_graph.md](../design_animation_graph.md) | ✅ v2.1・A0〜A2 実装済み(WP94/97/101/102)。VRM 系は未 | [第8章](08_gameplay.md) |
| [design_asset_hot_reload.md](../design_asset_hot_reload.md) | ✅ v2.1・HR0〜HR2-G 実装済み(WP96/98/100/105/108/110)。HR2-I 未 | [第10章](10_tools.md) |
| [design_text_hud.md](../design_text_hud.md) | ✅ WP54 | [第7章](07_input_ui.md) |
| [design_game_logic_native.md](../design_game_logic_native.md) | ✅ G1/G2(DLL ホットリロード = WP90) | [第8章](08_gameplay.md) |
| [design_event_layer.md](../design_event_layer.md) | ✅ E1 + WP71(E2 は未) | [第8章](08_gameplay.md) |
| [design_scene_flow.md](../design_scene_flow.md) | ✅ S1(S2 は未) | [第8章](08_gameplay.md) |
| [design_physics_queries.md](../design_physics_queries.md) | ✅ P1/P2 + shapeCast/Provider ABI V2(WP107)。シミュは未 | [第8章](08_gameplay.md) |
| [design_camera_system.md](../design_camera_system.md) | ✅ C1/C2 + user-space 再編の追記 | [第8章](08_gameplay.md) |
| [design_determinism_services.md](../design_determinism_services.md) | ✅ WP53 | [第8章](08_gameplay.md) |
| [design_audio.md](../design_audio.md) | ✅ A1(A2/A3 は未) | [第8章](08_gameplay.md) |
| [design_persistence.md](../design_persistence.md) | ✅ WP55 + WP65(P1 実装済み) | [第3章](03_project_format.md)・[第8章](08_gameplay.md) |
| [design_asset_format_policy.md](../design_asset_format_policy.md) | ✅(KTX2 = WP92 済。状態列は古い) | [第5章](05_assets.md) |
| [design_asset_containers.md](../design_asset_containers.md) | ✅ K1〜K4(WP77/79/81/84) | [第5章](05_assets.md) |
| [design_project_dcc_houdini.md](../design_project_dcc_houdini.md) | ✅ import + import-tools(K3) | [第5章](05_assets.md) |
| [dcc_integration_qa_2026-06-12.md](../dcc_integration_qa_2026-06-12.md) | §6 = pelican.vat の正 ✅ | [第5章](05_assets.md) |
| [design_project_vcs.md](../design_project_vcs.md) | ✅ V1〜V3 + V2 manifest(WP66) | [第3章](03_project_format.md) |
| [design_project_interpretation_layer.md](../design_project_interpretation_layer.md) | ✅ WP44(現状記述は一部古い) | [第3章](03_project_format.md) |
| [design_build_tiers.md](../design_build_tiers.md) | ✅ B1/B2(B3/B4 未) | [第2章](02_getting_started.md)・[第10章](10_tools.md) |
| [design_devstudio_direction.md](../design_devstudio_direction.md) | 📐 D0 決定のみ(D1〜D3 未) | [第10章](10_tools.md) |
| [design_roadmap_renderworld.md](../design_roadmap_renderworld.md) | 古い(一部失効) | — |
| [design_cloth_simulation.md](../design_cloth_simulation.md) | 古い(VAT レーンに実質置換) | [第5章](05_assets.md) |

### 指示書・運用・生成物

| 文書 | 内容 |
|---|---|
| [implementation_plan.md](../implementation_plan.md) | WP 詳細・受け入れ基準・共通規則(§0)。110KB 超 — 分割は backlog 登録済み |
| [roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md) | 今後の候補 47 件・6 ティア(セッション引き継ぎ用) |
| [adding_features.md](../adding_features.md) | 機能追加レシピ集 |
| [agent_operations.md](../agent_operations.md) | マルチエージェント運用 |
| [rendering_phase1_review.md](../rendering_phase1_review.md) | Validation Run の流儀 |
| `design_reviews/` | codex の監査・敵対レビュー・WP レポート(一次資料) |
| `schemas/` | pelican.ui 系 JSON Schema 4 本(U0 成果物) |
| `color_migration_manifest.json` / `material_resources_manifest.json` | WP72 / WP76 系の生成物 |
| `source-code-guide/` | ソースコード読解ガイド(全 10 ファイル。[第12章](12_codemap.md) は入口・概観、詳細はこちら) |

### 2026-07-12 の方針決定(横断・重要)

> **設計決定(feature 層 = ユーザー空間):** エンジンは**機構語彙**(anchor / history / snapshot / format_class / projection_jitter 枠など — 版付きで additive にのみ増える)だけを持ち、feature(JSON + シェーダ)は**ユーザー空間**。同梱 feature(hdr / shadow / bloom / debug 系)は**特権なしの標準ライブラリ**で、`engine://features/*.json` をプロジェクトへコピーして改造するのが公式ワークフロー。いじれない境界(canonical anchor の全順序・output_transform 等の常設ノード・色 invariant・決定性ゲート)は名前入りエラーで防衛される。カメラのコントローラ/ブレンド/シェイクも同様にユーザー空間へ再編済み。

## 11.3 既知の食い違い(設計文書 vs コード)

2026-07-16 の再調査で更新。**常にコードが正**。

### 挙動に関わるもの

1. **docs/README.md の索引が大幅に古い**(2026-07-15 更新だが表は未改訂): material_shading「未実装」/ postprocess_temporal「未実装」/ input_actions「I2〜I4 未」/ audio・scene_flow・event_layer・persistence・project_vcs「実装中」— すべてマージ済み。さらに ui_2d_foundation / 2d_game_layer / animation_graph / asset_hot_reload / color_pipeline / event_payload_schema / taa_jitter / usd_openpbr / animation_abi_v1 / roadmap_backlog が索引未掲載。**状態の正は本章 §11.1〜11.2**。
2. **strict v1 化(WP63)が一部の設計文書に未反映**: scene のレガシー受理(「WARN+自動解釈」)、カメラ旧キーのエイリアス受理 — 文書にはあるが、実装はすべて名指しの hard error。
3. **hello-project(web サンプル)の camera が旧形式**: web で開けて pelican で開けない = サブセット原則違反の実例([第9章](09_web.md) §9.7)。
4. **`--play-seq` / `--seq-mesh` の相対パスが cwd 基準**([PF] はプロジェクトルート基準と規定)。`--play-vat` は設計どおり。
5. **project.json の `name` が必須になっていない**([PF] は必須)。欠落は黙って `user://` 無効化に落ちる。
6. **audio の `playMusic` が存在しない**(設計の API 表にはある)。全 voice は se バス固定。
7. **rpc `raycast` は未実装**(P3 スコープ)。※ただし WP107 で GameContext/PhysWorld には `shapeCast` all/closest が追加済み(rpc 面ではない)。
8. **エンジン発行イベントは `SceneLoaded` のみ**(設計表の OverlapEnter/Exit は E2 未実装 — backlog 登録済み)。
9. **`GameContext::debugText` は白・等倍固定**(設計の色・スケールは内部 API のみ)。

### キー名・形式の非対称(混同注意)

- **transform_seq は `rot`、scene / rpc は `rotation`**(意図的な別スキーマ判定)。
- **カメラの `yfov` はラジアン、ライトのコーン角は度、`--camera` の fov も度**。
- **schema/version エンベロープの有無が形式ごとに異なる**: scene / input_actions / import / material / render_feature / frame_plan / ui / anim_graph / atlas / input_seq / settings にはあり、asset_data / rendering config にはない。

### 文書の鮮度の問題

- `implementation_plan.md` 冒頭の「設計の正は 3 文書」は実際には 5 文書。§1 の WP 表と §2 の詳細は概ね追随しているが、ファイルが 110KB 超で分割が backlog 登録済み。
- `design_asset_format_policy.md` の状態列(VAT/EXR/音声/import/KTX2「未実装」)はすべて実装済み。

## 11.4 このマニュアルの更新ルール

本マニュアル(`docs/manual/`)は 2026-07-10 に全域調査(9 領域並列コードリーディング)から書き起こされ、**2026-07-16 に WP64〜110 反映の全域更新**を行いました。維持のためのルール:

1. **コードが正。** 設計文書と食い違ったら、実装を確認してマニュアルを実装に合わせる(意図の説明として設計文書を引用するのは可)。
2. **実装状況バッジを守る。** ✅実装済み / 🚧実装中 / 📐設計のみ(未実装)。WP がマージされたら該当章のバッジと §11.1 の台帳を更新する。設計文書にしかない機能を「使える」と書かない。
3. **JSON 例は実物から。** `projects/example/`・`projects/sprite_demo/`・`projects/animgraph_demo/`・`test/fixtures/`・`src/core/resources/` の実ファイル、またはパーサ実装で検証した形だけを載せる。創作例には「(例)」と明記。
4. **章の追加**: `NN_slug.md` で採番し、[00_index.md](00_index.md) の目次と、関係する章の相互リンクを更新する。章の冒頭形式(タイトル → 対象行 → 「この章で学ぶこと」→ 末尾「関連文書」)を踏襲する。
5. **バッジ更新のタイミング**: 形式(スキーマ)に触れる WP・strict 化 WP・機能追加 WP のマージ時。直近では HR2-I・U3(ホットリロード残件)・J1/T-TAA(TAA)・VRM-S1 以降のマージ時に [第7章](07_input_ui.md)・[第6章](06_rendering.md)・[第5章](05_assets.md)・[第10章](10_tools.md) の該当節の書き換えが必要。
6. **§11.3 の食い違い一覧は「解消したら消す」**。設計文書側が改訂されたか、実装が設計に追いついたら該当行を削除する。

## 関連文書

- [../implementation_plan.md](../implementation_plan.md) — WP 台帳の一次資料
- [../roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md) — 今後の候補(47 件・6 ティア)
- [../README.md](../README.md) — 設計文書の索引
- [../agent_operations.md](../agent_operations.md) — 開発体制
- [第1章 エンジン全体像](01_overview.md) / [目次](00_index.md)
