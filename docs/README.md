# pelican2 設計文書 索引

最終更新: 2026-07-07。文書が矛盾したら**凍結済み > ドラフト、設計文書 > 指示書**の順で優先。

## まずこれを読む(新規参加者・新エージェント)

1. 本索引
2. `implementation_plan.md` §0(共通規則)と §1(WP 一覧)— 実装指示書
3. `design_project_format.md`([PF])— プロジェクトという単位の定義
4. `adding_features.md` — 機能を足すときのレシピ集

## 凍結済み(変更には版数改訂が必要)

| 文書 | 版 | 内容 |
|------|----|------|
| `design_project_format.md` [PF] | v6.2 | プロジェクト形式・PathResolver・パス分類。v6.1 暗黙 fallback、v6.2 input_actions_json |
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
| `dcc_integration_qa_2026-06-12.md` | — | DCC 連携 QA。**§6 = pelican.vat v1 仕様の正** |
| `design_roadmap_renderworld.md` | 古い | 全体ロードマップと ECS 境界。合意後回し方針(2026-07-02)で一部失効 |
| `design_cloth_simulation.md` | 古い | 布シミュ構想。VAT レーンに実質置換 |

## 指示書・レビュー記録

- `implementation_plan.md` — WP 詳細・受け入れ基準・ウェーブ運用(v3)
- `adding_features.md` — 機能追加レシピ集(cookbook)
- `rendering_phase1_review.md` — Validation Run の流儀

## 他リポジトリの文書

- `my_webpage/docs/`: `implementation_plan_web.md`(WW 指示書)、
  `pelican2-webgpu-compat.md`(rendering config 互換の正)、
  `pelican2-project-format.md`(web 実装者向け要約)、
  `design-engine-gui-system.md`(GUI 三層: tokens/primitives/surfaces)
- `pelican-houdini-adapter/docs/`: 契約文書のコピー(正本はこちら側)

## 未着手の戦略設計(次に書くべき文書)

1. ゲームロジック実行方式(スクリプト/DLL/データ駆動の選択。アクション層 API は前提済み)
2. コマンド層 stage 3(load_gltf / update_transforms)
3. OpenXR 描画トラック(フレームグラフへの view 概念)
