# pelican2 設計文書 索引

最終更新: 2026-07-23(RPE6b1 hybrid screen input vertical slice / WP187 完了)。文書が矛盾したら
**凍結済み > ドラフト、設計文書 > 指示書**の順で優先し、実装状態は
コード・テスト・`design_reviews` の完了レポートを正とする。

> **使い方を知りたい人へ**: 設計文書とは別に、人間向けの利用マニュアル
> (チュートリアル+リファレンス、実装状況バッジ付き)が
> [`manual/`](manual/00_index.md) にある。2026-07-10 の全域調査を基礎に、
> WP179 までの状態差分を同期している。

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
| `design_render_feature_modules.md` | v1.2・config合成実装済み(WP28〜30)、typed接続未 | **パージ可能 GPU 機能**。fragment 合成・defines・RT overrides。typed GraphFragment / closed forest へ接続 |
| `design_compute_task_graph.md` | v2.2・現行render/compute実装済み(WP33〜35)、異種拡張未 | **統一フレームグラフ**。依存宣言→機械最適化→手詰めの三層。authoring kind と execution domain を分離 |
| `design_heterogeneous_execution_graph.md` | v1 方針・HEG1a/1b実装済み | **異種 execution の正本**。共通 typed dialect、endpoint/link topology、backend probe、CPU/Vulkan sibling lowering、fragment / closed forest、trust-first・optimize-by-default・advisory診断。動画は拡張可能性だけを予約 |
| `design_render_pipeline_extensibility.md` | v2.1 方針・RPE1〜RPE6b1実装済み | **renderer 拡張境界の正本**。preset から physical/native までの拡張 ladder、renderer compiler facade、draw sort/MSAA/XR/hot reload の分離 |
| `design_render_graph_compiler.md` | v1.1 方針・RPE6b1実装済み | **renderer compiler の詳細設計**。logical type / versioned value、material/light contract、target execution seam、tile GPU、Vulkan physical IR / NativeScope |
| `design_input_actions.md` | v1・I1〜I4 + XR action/pose 実装済み(WP39/49/89/91/130/132) | 入力四層・アクション層・プロファイル・収録/再生。HR2-I は未 |
| `design_project_interpretation_layer.md` | v1・主要分離実施済み | 解釈(`pelican_project`)と engine binder の分離 |
| `design_project_dcc_houdini.md` | v1・engine 側受け口実装済み | import manifest/VAT 再生は実装済み。Houdini adapter は外部リポジトリ |
| `design_asset_format_policy.md` | v1.1・主要形式実装済み | 二層モデル。EXR/KTX2/音声/import を実装済み(本文の古い状態列に注意) |
| `design_build_tiers.md` | v1・B1/B2 + CI matrix 実装済み(WP40/41/137/165) | `PELICAN_WITH_*` ユニット、dist-config、OFF/clean-clone gate |
| `design_physics_queries.md` | v2・query/E2 実装済み(WP46/47/107/179) | raycast/overlap/shapeCast、provider ABI、trigger Enter/Exit。剛体 simulation は未 |
| `design_camera_system.md` | v1・C2 実装済み(WP48/50) | カメラ glTF 同等以上。定義/コントローラ/演出の三層 |
| `design_material_shading.md` | M1〜M3.5 実装済み | `pelican.material`、`.surface`、FrameUBO/SSBO、stdlib/OpenPBR、screen snapshot。spv-link は experimental |
| `design_postprocess_temporal.md` | T1/T2 + TAA 実装済み(WP88/95/112〜115) | history/velocity、projection jitter、ユーザー空間 TAA stdlib |
| `design_scene_flow.md` | S1 実装済み(WP52) | 同期 scene transition。非同期 S2 は未 |
| `design_audio.md` | A1 実装済み(WP51) | `PELICAN_WITH_AUDIO` + miniaudio/null backend + bus。A2/A3 は未 |
| `design_persistence.md` | P1 実装済み(WP55/65) | `user://`、settings/saveData、atomic write |
| `design_event_layer.md` | v1.1・E1/E2 実装済み(WP56/71/179) | frame-boundary typed event bus + deterministic `OverlapEnter/Exit` |
| `design_determinism_services.md` | v1・実装済み(WP53) | PCG32 決定的乱数 + シード規約 |
| `design_text_hud.md` | v1・実装済み(WP54) | debug_text feature(ビットマップ HUD)。本格テキストは 2D/UI 設計で |
| `design_project_vcs.md` | v1・主要機能実装済み(WP55/57/66) | asset store、assets manifest、project init、外部 DAM 契約 |
| `design_asset_containers.md` | v1・K1〜K4 実装済み(WP77/79/81/84) | `#` fragment、glTF scene extract、PSD/atlas tools、import rules |
| `design_animation_graph.md` | v2.1・A0〜A2 + VRM/VRMA 実装済み(WP94〜102/111/121〜134/176〜178) | graph v1、typed VRMA decode/retarget/source。graph v2/live/SpringBone は未 |
| `design_openxr.md` | v2.1・XR0〜XR4実装済み(WP125〜138) | sequential stereo PCVR、action/pose、mirror、Simulator gate。XR2b/standalone は未 |
| `design_editor_tooling.md` | v2.5・共通 authoring/editor 基盤実装済み(WP149〜172) | typed RPC/service、transaction、undo/save/snapshot/watch/preview。Qt viewport/gizmo は未 |
| `design_asset_hot_reload.md` | v2.1・HR0〜HR2-G + targeted animation generation 実装済み | 残り HR2-I、U3、VRMA watcher 自動配線 |
| `design_debug_profiling.md` | D-P0〜D-P2 実装済み(WP139/140/143/145) | debug labels、RenderDoc、GPU/VRAM/XR timing。D-P3以降は未 |
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

## 次段の方向(2026-07-23 時点)

- hot reload 残件 = HR2-I(input/profile)、U3(UI)、VRMA watcher 配線
- editor = 共通 authoring/RPC/ImGui 基盤は済。Qt embedded viewport、
  picking/gizmo、WebSocket multiple-client は後続
- animation = VRMA-I0 まで済。graph v2、clip event sidecar、timeline/live
  source、SpringBone は設計・WP 登録待ち
- OpenXR = sequential PCVR は済。XR2b multiview/depth と Quest
  standalone SA0〜SA3 が後続
- physics = query/Jolt provider/E2 trigger は済。rigid-body simulation は
  将来トラック(`design_physics_queries.md` §6、Jolt 推奨)
- rendering = `hybrid_v1` の deferred + forward 合成、semantic material route、
  RPE1〜RPE6b1(resolve、typed manifest、draw queue/provider、transparent/XR sort、
  logical type kernel / versioned shadow graph、typed material screen input)は実装済み。次は
  RPE6c0 topology / backend probe / optimize-by-default policy、RPE6c1 desktop/tile target planner、MSAA、XR variant、
  pipeline transaction の順。RPE6c0/1では異種 execution 設計の canonical / disposable
  lowering seamだけを置き、CPU schedulerと動画backendは計測・具体需要まで実装しない。
  lighting/IBL、motion blur、bindless、compute particles はこの compiler 境界上で
  需要と計測を伴う設計から開始
