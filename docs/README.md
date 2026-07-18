# pelican2 設計文書 索引

最終更新: 2026-07-18。文書が矛盾したら**凍結済み > ドラフト、設計文書 > 指示書**の順で優先。

> **使い方を知りたい人へ**: 設計文書とは別に、人間向けの利用マニュアル(チュートリアル+リファレンス、実装状況バッジ付き)が [`manual/`](manual/00_index.md) にある。「いま何がどう動くか」はそちらが正(コード準拠・2026-07-10 全域調査)。

> **実装を読みたい人へ**: 起動、ECS、Vulkan、RPC、テストをソースから追う技術資料は [`source-code-guide/`](source-code-guide/README.md) にまとめている。

## まずこれを読む(新規参加者・新エージェント)

1. 本索引
2. `implementation_plan.md` §0(共通規則)と §1(現行 WP 一覧)— active 実装指示書
3. `design_project_format.md`([PF])— プロジェクトという単位の定義
4. `adding_features.md` — 機能を足すときのレシピ集

## 凍結済み(変更には版数改訂が必要)

| 文書 | 版 | 内容 |
|------|----|------|
| `design_project_format.md` [PF] | v6.3 | プロジェクト形式・PathResolver・パス分類。v6.3(§3-A)= user:// + asset store + #フラグメント(2026-07-08 承認) |
| `design_project_format_web_profile.md` [PFW] | v1.3 | web 解釈規則・**サブセット原則**(web ⊆ pelican)・シェーダ stem 規約・fixture 共有 |
| `external_tools_requirements.md` | R1〜R10 | 外部ツール契約(正本)。glTF ハブ、transform_seq、pelican.vat、座標系、JSON-RPC |
| `design_shader_freedom_kit.md` [SF] | — | シェーダ基盤(コンパイラ/ライブラリ/ファクトリ/set 規約)。実装済み |
| `design_headless_rendering.md` [HL] | — | ヘッドレス描画。実装済み |

## ドラフト(実装済み or 実装中 — レビュー反映で更新される)

| 文書 | 状態 | 内容 |
|------|------|------|
| `design_scene_format.md` | v1.1・実装済み(WP25) | pelican.scene v1。エンベロープ・light コンポーネント・objects[].name |
| `design_render_feature_modules.md` | v1・実装済み(WP28〜30) | **パージ可能 GPU 機能**。fragment 合成・defines・RT overrides |
| `design_compute_task_graph.md` | v2・実装済み(WP33〜35) | **統一フレームグラフ**。依存宣言→機械最適化→手詰めの三層。CPU 拡張予約 |
| `design_input_actions.md` | v1・I1 実装済み(WP39) | 入力四層・アクション層・OpenXR 整合・収録→データ化(I2〜I4 未) |
| `design_project_interpretation_layer.md` | v1・部分実施 | 解釈(pelican_project)とバインダの分離。ターゲット分離は未 |
| `design_project_dcc_houdini.md` | v1・部分実施 | Houdini レーン・**pelican.import manifest**(WP21 済)。VAT レーン未 |
| `design_asset_format_policy.md` | v1.1・部分実施 | **二層モデル**(ランタイム層/ソース層)。EXR 済、KTX2/音声未 |
| `design_build_tiers.md` | v1・実装中(WP40) | **配布ビルド**。PELICAN_WITH_* ユニット + dist-config 導出 |
| `design_physics_queries.md` | v1・P2 実装済み(WP46/47) | 物理クエリ(raycast/overlap)。collider・PhysWorld・シミュは将来トラック(§6) |
| `design_camera_system.md` | v1・C2 実装済み(WP48/50) | カメラ glTF 同等以上。定義/コントローラ/演出の三層 |
| `design_material_shading.md` | v1 ドラフト・未実装 | **pelican.material v1**・シェーダ契約の安定 API 化・variant 規律(M1→M3) |
| `design_postprocess_temporal.md` | v1 ドラフト・未実装 | **history/velocity + ポストスタック規約**(フレームグラフ v2.5、T1→T3) |
| `design_scene_flow.md` | v1 ドラフト・S1 実装中(WP52) | シーン遷移(loadScene)+ 非同期ロード(S2 将来) |
| `design_audio.md` | v1 ドラフト・A1 実装中(WP51) | **PELICAN_WITH_AUDIO** + miniaudio + null バックエンド + バス |
| `design_persistence.md` | v1・承認済み・実装中(WP55/P1) | user:// スキーム・設定/セーブの三区分 |
| `design_event_layer.md` | v1・API 承認済み・実装中(WP56) | フレーム境界配送のイベントバス・物理トリガー |
| `design_determinism_services.md` | v1・実装済み(WP53) | PCG32 決定的乱数 + シード規約 |
| `design_text_hud.md` | v1・実装済み(WP54) | debug_text feature(ビットマップ HUD)。本格テキストは 2D/UI 設計で |
| `design_project_vcs.md` | v1・レビュー 3 巡承認済み・実装中(WP55/57) | asset store(マウント間接化)+ assets manifest + project init 雛形 + 外部 DAM 契約 |
| `design_asset_containers.md` | v1・レビュー 3 巡承認済み・実装中(WP55〜) | **#フラグメント参照**・glTF シーン抽出・PSD レーン(psd-tools)・import ルール表 |
| `dcc_integration_qa_2026-06-12.md` | — | DCC 連携 QA。**§6 = pelican.vat v1 仕様の正** |
| `design_roadmap_renderworld.md` | 古い | 全体ロードマップと ECS 境界。合意後回し方針(2026-07-02)で一部失効 |
| `design_cloth_simulation.md` | 古い | 布シミュ構想。VAT レーンに実質置換 |

## 指示書・レビュー記録

- `agent_operations.md` — **マルチエージェント運用ガイド**(体制・WP/ウェーブ・codex 起動標準・敵対レビューループ・マージゲート・鉄則。新規エージェントはまずこれ)
- `implementation_plan.md` — 未完了 WP の詳細・受け入れ基準・ウェーブ運用(active ledger)
- `implementation_archive.md` — 完了済み WP の一覧・詳細(逐語 archive)
- `adding_features.md` — 機能追加レシピ集(cookbook)
- `ci.md` — Windows/MSVC CPU gate、`gpu` ラベル、SKIP exact policy の運用
- `example_assets.md` — example の外部 binary asset 22件の取得・provenance・ライセンス台帳
- `rendering_phase1_review.md` — Validation Run の流儀

## 他リポジトリの文書

- `my_webpage/docs/`: `implementation_plan_web.md`(WW 指示書)、
  `pelican2-webgpu-compat.md`(rendering config 互換の正)、
  `pelican2-project-format.md`(web 実装者向け要約)、
  `design-engine-gui-system.md`(GUI 三層: tokens/primitives/surfaces)
- `pelican-houdini-adapter/docs/`: 契約文書のコピー(正本はこちら側)

## 方向決定済み・設計/実装待ち(2026-07-07 時点)

- ゲームロジック = ネイティブ C++(`design_game_logic_native.md`、G1a→G1b→G2)
- devstudio = Qt(`design_devstudio_direction.md`、D1→D3)
- コマンド層 stage 3 = GO(複数インスタンス要件込み — implementation_plan §3)
- OpenXR = 推進(view 次元・XrFrameTarget — implementation_plan §3)
- 2D ゲーム機能 + 2D⇔3D 変換 / アニメーショングラフ —
  implementation_plan §3 に方向記録。設計文書はこれから
- 物理シミュレーション = 将来トラック(`design_physics_queries.md` §6、Jolt 推奨)
