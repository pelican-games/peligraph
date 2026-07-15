# 第11章 実装状況と文書マップ

対象: pelican2(2026-07-10 時点、branch `codex/rendering-phase1-refactor`)/ このマニュアルはコードを正とする

## この章で学ぶこと

- WP(Work Package)全台帳 — 何が実装済みで、何が登録のみ・予約なのか
- 設計文書マップ — docs/ の各文書と本マニュアルの章の対応、状態(凍結/ドラフト/古い)
- 既知の「設計文書とコードの食い違い」一覧
- 本マニュアル自体の更新ルール

## 11.1 WP 全台帳

判定は「implementation_plan.md の記載 + コードの実在 + git log」によります(2026-07-10 調査)。

凡例: **済** = マージ済みで機能する / **未** = 登録済み・未実装 / **予約** = 番号予約のみ。

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
| 23 | KTX2 | 予約 |
| 24 | 音声 | 予約(実質 WP51 = A1 として実装) |
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
| 38 | スケルタルアニメーション | 予約 |
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
| 58 | マテリアル M1(pelican.material パーサ + shader_contract.md) | 済(**レンダラ未接続**) |
| 59 | スパイク pelican-spv-link | 済(スパイクのみ。判定: B 層に SPIR-V ABI リンク採用) |
| 60 | リファクタ R1(死荷重削除。`--project-settings` 廃止等) | 済 |
| 61 | リファクタ R2(カメラキー正規形移行・データのみ) | 済 |
| 62 | リファクタ R5-core(ECS ライフサイクル + 世代 ID) | 済 |
| 63 | リファクタ R3(strict v1 化 — 旧キー・レガシー受理の全廃) | 済 |
| 64 | リファクタ R4(legacy 描画経路撤去 + A/B 差分ゲート) | **未** |
| 65 | 永続化 P1(user:// への設定/セーブ書き込み API) | **未** |
| 66 | assets manifest V2(生成・照合・起動時検証) | **未** |

**要するに: WP1〜63 はほぼすべて実装済みで、未実装は WP64/65/66 の 3 件(いずれも登録済み)+ 予約 5 件(WP22/23/32/36/38)です。**

WP 化待ちの方向決定済みトラック: コマンド層の WebSocket 展開、devstudio D1〜D3、OpenXR、2D UI 基盤(pelican.ui — レビュー差し戻し中)、アニメーショングラフ、bindless、マテリアル M2 以降、ポストプロセス/テンポラル T1〜T3、コンテナアセット K1〜K4、物理シミュレーション(Jolt 方針のみ)、web ビルド(WASM・可能性のみ)。

web 側(my_webpage)の WW 台帳は [第9章](09_web.md) §9.6 を参照してください。

## 11.2 設計文書マップ(docs/ ⇔ 本マニュアル)

矛盾時の優先順位: **凍結済み > ドラフト、設計文書 > 指示書**。ここでの「状態」は 2026-07-10 のコード実態基準です(docs/README.md の表記は一部古い — §11.3)。

### 凍結済み(変更には版数改訂が必要)

| 文書 | 版 | 内容 | 対応する章 |
|---|---|---|---|
| [design_project_format.md](../design_project_format.md) [PF] | v6.3 | プロジェクト形式・PathResolver | [第3章](03_project_format.md) |
| [design_project_format_web_profile.md](../design_project_format_web_profile.md) [PFW] | v1.3 | web 解釈規則・サブセット原則 | [第9章](09_web.md) |
| [external_tools_requirements.md](../external_tools_requirements.md) | R1〜R10 | 外部ツール契約(glTF ハブ、transform_seq、JSON-RPC) | [第5章](05_assets.md)・[第10章](10_tools.md) |
| [design_shader_freedom_kit.md](../design_shader_freedom_kit.md) [SF] | — | シェーダ基盤(実装済み) | [第6章](06_rendering.md) |
| [design_headless_rendering.md](../design_headless_rendering.md) [HL] | — | ヘッドレス描画(実装済み) | [第2章](02_getting_started.md)・[第6章](06_rendering.md) |

### ドラフト(実装済み or 実装中)

| 文書 | 実装状態 | 対応する章 |
|---|---|---|
| [design_scene_format.md](../design_scene_format.md) | ✅ WP25/63(互換規則の記述は古い) | [第4章](04_scene_ecs.md) |
| [design_ecs_lifecycle.md](../design_ecs_lifecycle.md) | ✅ WP62 | [第4章](04_scene_ecs.md) |
| [design_render_feature_modules.md](../design_render_feature_modules.md) | ✅ WP28〜31 | [第6章](06_rendering.md) |
| [design_compute_task_graph.md](../design_compute_task_graph.md) | ✅ WP33〜35(実行一本化は WP64 待ち) | [第6章](06_rendering.md) |
| [design_material_shading.md](../design_material_shading.md) | 🚧 M1 のみ(パーサ止まり) | [第6章](06_rendering.md) |
| [design_postprocess_temporal.md](../design_postprocess_temporal.md) | 📐 未実装 | [第6章](06_rendering.md) |
| [shader_contract.md](../shader_contract.md) | ✅(実装から採録した契約) | [第6章](06_rendering.md) |
| [design_input_actions.md](../design_input_actions.md) | ✅ I1/I2(I3/I4 は未) | [第7章](07_input_ui.md) |
| [design_ui_2d_foundation.md](../design_ui_2d_foundation.md) | 📐 レビュー Reject 中 | [第7章](07_input_ui.md) |
| [design_text_hud.md](../design_text_hud.md) | ✅ WP54 | [第7章](07_input_ui.md) |
| [design_game_logic_native.md](../design_game_logic_native.md) | ✅ G1(G2 は未) | [第8章](08_gameplay.md) |
| [design_event_layer.md](../design_event_layer.md) | ✅ E1(E2 は未) | [第8章](08_gameplay.md) |
| [design_scene_flow.md](../design_scene_flow.md) | ✅ S1(S2 は未) | [第8章](08_gameplay.md) |
| [design_physics_queries.md](../design_physics_queries.md) | ✅ P1/P2(P3・シミュは未) | [第8章](08_gameplay.md) |
| [design_camera_system.md](../design_camera_system.md) | ✅ C1/C2(C3 は未) | [第8章](08_gameplay.md) |
| [design_determinism_services.md](../design_determinism_services.md) | ✅ WP53 | [第8章](08_gameplay.md) |
| [design_audio.md](../design_audio.md) | ✅ A1(A2/A3 は未) | [第8章](08_gameplay.md) |
| [design_persistence.md](../design_persistence.md) | 🚧 リゾルバのみ(P1 = WP65 未) | [第3章](03_project_format.md)・[第8章](08_gameplay.md) |
| [design_asset_format_policy.md](../design_asset_format_policy.md) | ✅(状態列は古い) | [第5章](05_assets.md) |
| [design_asset_containers.md](../design_asset_containers.md) | 🚧 構文のみ(K1〜K4 未) | [第5章](05_assets.md) |
| [design_project_dcc_houdini.md](../design_project_dcc_houdini.md) | 🚧 import 済み(アダプタ未) | [第5章](05_assets.md) |
| [dcc_integration_qa_2026-06-12.md](../dcc_integration_qa_2026-06-12.md) | §6 = pelican.vat の正 ✅ | [第5章](05_assets.md) |
| [design_project_vcs.md](../design_project_vcs.md) | ✅ V1(V2 = WP66 未) | [第3章](03_project_format.md) |
| [design_project_interpretation_layer.md](../design_project_interpretation_layer.md) | ✅ WP44(現状記述は一部古い) | [第3章](03_project_format.md) |
| [design_build_tiers.md](../design_build_tiers.md) | ✅ B1/B2(B3/B4 未) | [第2章](02_getting_started.md)・[第10章](10_tools.md) |
| [design_devstudio_direction.md](../design_devstudio_direction.md) | 📐 D0 決定のみ(D1〜D3 未) | [第10章](10_tools.md) |
| [design_roadmap_renderworld.md](../design_roadmap_renderworld.md) | 古い(一部失効) | — |
| [design_cloth_simulation.md](../design_cloth_simulation.md) | 古い(VAT レーンに実質置換) | [第5章](05_assets.md) |

### 指示書・運用

| 文書 | 内容 |
|---|---|
| [implementation_plan.md](../implementation_plan.md) | WP 詳細・受け入れ基準・共通規則(§0) |
| [adding_features.md](../adding_features.md) | 機能追加レシピ集 |
| [agent_operations.md](../agent_operations.md) | マルチエージェント運用 |
| [rendering_phase1_review.md](../rendering_phase1_review.md) | Validation Run の流儀 |
| `design_reviews/` | codex の監査・敵対レビュー記録(一次資料) |

## 11.3 既知の食い違い(設計文書 vs コード)

2026-07-10 の全域調査で確認した主要な食い違いです。**常にコードが正**。詳細な一覧は各章の「既知の〜」節にも分散して記載しています。

### 挙動に関わるもの(重要)

1. **フレームグラフの実行は二経路**: compute を含まない構成は今もレガシー経路(JSON 宣言順)で実行される。docs/README.md の「実装済み(WP33〜35)」はプラン**作成**までが正確で、実行一本化は WP64(未)。純 render 構成の `before`/`after` 明示エッジはレガシー経路では効かない。
2. **strict v1 化(WP63)が各設計文書に未反映**: scene のレガシー受理(「WARN+自動解釈」)、カメラ旧キーのエイリアス受理、`.spv` 明示参照の後方互換 — 文書にはあるが、実装はすべて名指しの hard error。
3. **hello-project(web サンプル)の camera が旧形式**: web で開けて pelican で開けない = サブセット原則違反の実例([第9章](09_web.md) §9.7)。native ブリッジも非互換。
4. **`--play-seq` / `--seq-mesh` の相対パスが cwd 基準**([PF] はプロジェクトルート基準と規定)。`--play-vat` は設計どおり。
5. **project.json の `name` が必須になっていない**([PF] は必須)。欠落は黙って `user://` 無効化に落ちる。
6. **audio の `playMusic` が存在しない**(設計の API 表にはある)。全 voice は se バス固定。
7. **rpc `raycast` は未実装**(設計の消費者 API 表にはある。P3 スコープ)。
8. **エンジン発行イベントは `SceneLoaded` のみ**(設計表の OverlapEnter/Exit は E2 未実装)。
9. **同梱 hdr feature のアンカー `before:present` が example と不整合**(example に present パスがない → 足すと起動エラー)。
10. **`GameContext::debugText` は白・等倍固定**(設計の色・スケールは内部 API のみ)。

### キー名・形式の非対称(混同注意)

- **transform_seq は `rot`、scene / rpc は `rotation`**(意図的な別スキーマ判定)。
- **カメラの `yfov` はラジアン、ライトのコーン角は度、`--camera` の fov も度**。
- **schema/version エンベロープの有無が形式ごとに異なる**: scene / input_actions / import / material / render_feature / frame_plan にはあり、asset_data / ui_overlay / rendering config にはない。

### 文書の鮮度の問題

- docs/README.md(2026-07-08 更新)の状態列は遅れている: audio / scene_flow / event_layer / project_vcs / material M1 は「実装中・未実装」表記だがマージ済み。`design_ui_2d_foundation.md` は索引に未掲載。
- `implementation_plan.md` 冒頭の「設計の正は 3 文書」は実際には 5 文書。WP25/21 の実装場所の記載は WP44 の移動(`src/project/` へ)に未追随。
- `design_asset_format_policy.md` の状態列(VAT/EXR/音声/import「未実装」)はすべて実装済み。

## 11.4 このマニュアルの更新ルール

本マニュアル(`docs/manual/`)は 2026-07-10 に全域調査(9 領域並列コードリーディング)から書き起こされました。維持のためのルール:

1. **コードが正。** 設計文書と食い違ったら、実装を確認してマニュアルを実装に合わせる(意図の説明として設計文書を引用するのは可)。
2. **実装状況バッジを守る。** ✅実装済み / 🚧実装中 / 📐設計のみ(未実装)。WP がマージされたら該当章のバッジと §11.1 の台帳を更新する。設計文書にしかない機能を「使える」と書かない。
3. **JSON 例は実物から。** `projects/example/`・`test/fixtures/`・`src/core/resources/` の実ファイル、またはパーサ実装で検証した形だけを載せる。創作例には「(例)」と明記。
4. **章の追加**: `NN_slug.md` で採番し、[00_index.md](00_index.md) の目次と、関係する章の相互リンクを更新する。章の冒頭形式(タイトル → 対象行 → 「この章で学ぶこと」→ 末尾「関連文書」)を踏襲する。
5. **バッジ更新のタイミング**: 形式(スキーマ)に触れる WP・strict 化 WP・機能追加 WP のマージ時。特に WP64(レンダリング実行一本化)・WP65(永続化)・WP66(assets manifest)のマージ時は [第6章](06_rendering.md)・[第8章](08_gameplay.md)・[第3章](03_project_format.md) の該当節の書き換えが必要。
6. **§11.3 の食い違い一覧は「解消したら消す」**。設計文書側が改訂されたか、実装が設計に追いついたら該当行を削除する。

## 関連文書

- [../implementation_plan.md](../implementation_plan.md) — WP 台帳の一次資料
- [../README.md](../README.md) — 設計文書の索引
- [../agent_operations.md](../agent_operations.md) — 開発体制
- [第1章 エンジン全体像](01_overview.md) / [目次](00_index.md)
