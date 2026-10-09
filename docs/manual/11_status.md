# 第11章 実装状況と文書マップ

最終確認: 2026-10-08(WP 台帳・履歴への索引と参照先を確認。§11.2・§11.3 の確認時点は各節に記載)

> **この章の参照先:** §11.1 は WP 台帳と履歴への索引です。現在の利用方法は各章、作業の仕様・完了記録は実装指示書、過去の転記は history を参照してください。§11.2・§11.3 は記載された確認範囲のまま保持しています。

- **WP 番号の読み方**: WP(Work Package)は開発の作業単位で、**1 WP = 1 ブランチ = 1 マージ**です。番号は連番とは限らず、`238e` `380a-fix` のような枝番が付きます([第1章 §1.0](01_overview.md))。
- **記号 ID(v0 の段)**: 2026-09-16〜17 の v0 では、通し番号ではなく **`AG-0` / `AG-1a` / `P-2a-1c` / `X-1` / `R1-2` / `N-2` のような記号 ID** が作業単位の名前として使われました。読み方は「系統 + 段」で、`AG` = アニメーション / 共通グラフ、`P` = パラメータと部品(`P-2a` はその Bloom パイロット、`P-2b` はサンプル project)、`X` = 持ち出し(package)、`O` = オフライン出力、`R` = 整理と凍結、`N` = ノードキャンバス(Studio)です。枝番の規則は WP と同じで、`AG-1a` → `AG-1b` は同じ段の前後を表します。本文のバッジも `✅AG-1b` / `✅P-2a-3` のように書きます。記録の入口は §11.1 の v0 行、面の凍結は [../v0_freeze.md](../v0_freeze.md)。
- **索引の見方**: §11.1 の「正本の入口」は作業の仕様・完了記録へ、「history の該当見出し」は当時の転記へ進みます。現在の利用可否は各機能の章で確認してください。
- **タイトルの `U1` `C1a` `F0` `RPE2` `VRM-S1` などは WP 番号ではなくトラック記号**(設計文書側で決めた「どの系統の第何段か」)です。よく出るものの凡例:
  - `U1`〜`U4` — プレファブの出荷単位([../design_prefabs.md](../design_prefabs.md) §7)。`U1` = leaf library + ProjectEnvelope の prefabs 保持 + ResolvedScene seam + version 2 ゲート、以降 `U2` unpack、`U3` authoring transaction、`U4` `ctx.spawn`。→ [第13章](13_editor.md)
  - `C0` / `C1a` / `C1b` — 色パイプラインの実装順([../design_color_pipeline.md](../design_color_pipeline.md) §6)。`C1a` = canonical anchor / format_class / output_transform の構造だけを入れて出力 byte を変えない段、`C1b` = 色意味論の一括移行。→ [第6章 §6.3](06_rendering.md)
  - `F0`〜`F2` — フレームグラフ(compute task graph)の移行計画([../design_compute_task_graph.md](../design_compute_task_graph.md) §5)。`F0` = プランナを実装して「導出順 = 現行配列順」を assert するだけのシャドー検証、`F1` = compute 実行系、`F2` = 実行順をプランナ由来へ切替。→ [第6章 §6.6](06_rendering.md)
  - `RPE1`〜`RPE12` — 描画パイプライン拡張性の段階実装([../design_render_pipeline_extensibility.md](../design_render_pipeline_extensibility.md) §12)。`RPE2` = typed `CompiledRenderPipeline` の導入と `composition_metadata` の runtime 読み撤去(WP181)。→ [第6章](06_rendering.md)
  - `VRM-S0` / `VRM-S1` / `VRMA-*` — VRM 一級化の WP 群([../design_animation_graph.md](../design_animation_graph.md) §4)。`VRM-S1` = per-instance application sink(expression / gaze)。→ [第5章](05_assets.md)
  - `RPE` は設計文書名 `design_render_pipeline_extensibility.md` の頭字と一致し、`VRM-S` の `S` は設計側の行が `semantic decode/storage` と書いています。`U` / `C` / `F` が何の略かは設計文書に書かれておらず**未確認**です。ここに無い記号は §11.2 の文書マップから該当設計文書と章を辿ってください。
- **`WW` は web 側(my_webpage リポジトリ)の作業単位** = Web Work Package で、pelican2 の WP とは別系統の番号です([第9章 §9.6](09_web.md#96-実装状況ww-台帳))。

## この章で学ぶこと

- WP(Work Package)の台帳・履歴への索引 — 各期間の正本と当時の記録の参照先
- 設計文書マップ — docs/ の各文書と本マニュアルの章の対応、状態(凍結/ドラフト/古い)
- 既知の「設計文書とコードの食い違い」一覧
- 本マニュアル自体の更新ルール

## 11.1 WP の台帳はどこにあるか

WP の仕様・完了記録の正本は [実装指示書](../implementation_plan.md) です。旧 §11.1 の着地台帳は [history の転記](../history/wp_ledger_manual_copy.md#111-wp-全台帳) にあります。履歴の状態欄を現在の利用可否と読み替えず、使い方と有効な制限は各章で確認してください。

初期の完了 WP は正本の [現行 WP 一覧](../implementation_plan.md#1-現行-wp-一覧) から案内する archive に移っています。各期間の入口を以下にまとめます。

| 期間 | 概要 | 正本の入口 | history の該当見出し |
|---|---|---|---|
| WP1〜63 | 起動・プロジェクト形式・描画・入力・永続化などの基盤。 | [実装指示書](../implementation_plan.md#1-現行-wp-一覧) / [移設済み WP](../implementation_archive.md#wp1-enginelaunchconfig--player-cli-引数) | [転記](../history/wp_ledger_manual_copy.md#wp163基盤期詳細は各章) |
| WP64〜111 | 実行系一本化、material・UI・hot reload・VRM の拡張。 | [実装指示書](../implementation_plan.md#1-現行-wp-一覧) / [移設済み WP](../implementation_archive.md#wp64-リファクタ-r4--legacy-実行系の撤去f2-の完遂) | [転記](../history/wp_ledger_manual_copy.md#wp64111機能拡張期-2026-07-1016) |
| WP112〜179 | TAA・OpenPBR・XR・editor・VRMA・trigger の拡張。 | [実装指示書](../implementation_plan.md#1-現行-wp-一覧) / [移設済み WP](../implementation_archive.md#wp112-j1--projection-jitter-機構--feature-named-binding) | [転記](../history/wp_ledger_manual_copy.md#wp112179taa--openpbr--xr--editor--vrma--trigger2026-07-1619) |
| WP180〜202b | 描画パイプラインの型付け・公開拡張境界・世代管理。 | [実装指示書](../implementation_plan.md#1-現行-wp-一覧) / [移設済み WP](../implementation_archive.md#wp180済-2026-07-21-rpe1--typed-render-pipeline-resolve-boundary) | [転記](../history/wp_ledger_manual_copy.md#wp180202brpe--レンダーパイプライン拡張境界2026-07-2225) |
| WP203a〜214 | XR multiview、physical plan、描画機構と版の整理。 | [実装指示書](../implementation_plan.md#wp203c-xr2b-c--openxr-array-swapchain--depth-submit--gpu-gate) / [移設済み WP](../implementation_archive.md#wp203a済-2026-07-25-xr2b-a--view-execution-target-planning) | [転記](../history/wp_ledger_manual_copy.md#wp203a214xr-multiview--physical-plan--描画機構--版の単一化2026-07-2527) |
| WP215〜238e | WSI epoch、material ABI、ViewFamily と共通実行計画。 | [実装指示書](../implementation_plan.md#wp215217-window-presentation--wsi-epoch-recovery) | [転記](../history/wp_ledger_manual_copy.md#wp215238ewsi-epoch--material-abi--viewfamily--共通実行計画2026-07-2731) |
| WP239〜374 | editor・prefab・RT・宣言担体。転記元には行単位の表がない期間。 | [実装指示書](../implementation_plan.md#wp239-gpu-ラベル回帰の修復239a--239b--239c) | [転記](../history/wp_ledger_manual_copy.md#wp2393742026-08本章には行を転記していない) |
| WP375c〜386 | scene load transaction、game DLL ABI、宣言 schema と GPU lease。 | [実装指示書](../implementation_plan.md#wp375c-wp375b-が作った後退-2-件rpc-off-の-build-破壊--gpu-label-欠落--inactive-lifecycle-preview-が-abort-不能と位置依存の-attachment_seq) | [転記](../history/wp_ledger_manual_copy.md#wp375c386scene-load-transaction--game-dll-abi--宣言-schema-v3--gpu-lease2026-09-0304) |
| v0 | 共通グラフの装着・編集・観測・保存・再読込、持ち出しと出力。 | [実装指示書](../implementation_plan.md#ag-0-共通グラフの-module-登録契約と-purge-ゲートv0往復の段-1) | [転記](../history/wp_ledger_manual_copy.md#v02026-09-1617-共通グラフ姿勢パラメータ持ち出しオフライン出力整理) |
| 2026-10-06〜08(Studio の責務再編 RT-A〜D、文書の再編 DOC-1〜12、node ドラッグの追従 DL-1、FX-1) | 描画編集の ticket 追跡の部品化、同期 RPC の反映状態の読み取り、描画文書の watcher の移設、Graph タブの Undo の順序、manual と読解ガイドの再編、ドラッグ中の edge 探索と同期の削減。ここでの RT は ray tracing ではなく「再編(reorganization)」の WP 名。 | [責務再編の返答](../design_reviews/2026-10-06_studio_engine_responsibility_review_reply.md)(採否と順序)/ [文書の再編計画](../documentation_plan_2026-10-06.md) | 各 commit の本文(`git log --grep='RT-[ABCD]\|DOC-\|DL-1\|FX-1' develop`)。実装指示書には節が無い |

Studio と編集 RPC の機能ごとの記録は [Studio の履歴](../history/studio_status_2026-10.md)、web 側の WW 台帳の入口は [第9章 §9.6](09_web.md#96-実装状況ww-台帳) です。

## 11.2 設計文書マップ(docs/ ⇔ 本マニュアル)

矛盾時の優先順位: **凍結済み > ドラフト、設計文書 > 指示書**。
実装状態はレンダリング系が 2026-07-31、それ以外が 2026-07-19 の
コード・test・完了レポート基準です。**WP239 以降(2026-08)の着地は下の表に
反映していません** —— 例えば `design_object_identity.md` / `design_declaration_carrier.md` /
`design_raytracing.md` は表に行がありませんが、WP374〜386 の規範として実際に使われています。
**2026-09-17 に、v0 の規範となった 3 文書([v0_freeze.md](../v0_freeze.md) / [design_graph_composition.md](../design_graph_composition.md) / [v1_development_plan.md](../v1_development_plan.md))だけを表に足しました。**

### 凍結・規範文書

| 文書 | 版 | 内容 | 対応する章 |
|---|---|---|---|
| [design_project_format.md](../design_project_format.md) [PF] | v6.3 | プロジェクト形式・PathResolver | [第3章](03_project_format.md) |
| [design_project_format_web_profile.md](../design_project_format_web_profile.md) [PFW] | v1.3 | web 解釈規則・サブセット原則 | [第9章](09_web.md) |
| [external_tools_requirements.md](../external_tools_requirements.md) | R1〜R10 | 外部ツール契約(glTF ハブ、transform_seq、JSON-RPC) | [第5章](05_assets.md)・[第10章](10_tools.md) |
| [design_shader_freedom_kit.md](../design_shader_freedom_kit.md) [SF] | — | シェーダ基盤(実装済み) | [第6章](06_rendering.md) |
| [design_headless_rendering.md](../design_headless_rendering.md) [HL] | — | ヘッドレス描画(実装済み) | [第2章](02_getting_started.md)・[第6章](06_rendering.md) |
| [animation_abi_v1.md](../animation_abi_v1.md) | **Frozen / normative** | アニメーション ABI v1(正本 header = `abi_v1.hpp`) | [第8章](08_gameplay.md) |
| [v0_freeze.md](../v0_freeze.md) | **v0 凍結一覧(正本)・2026-09-17** | 「v0 で動かさないと約束する面」の実名一覧: 文書 schema 31 種 / RPC 89 method(基盤 29 + エディタ 60) / 共通グラフの module・op・型・sink / ABI と版 / package manifest の最小形 / rendering config の root と 2 つの装着様式。加えて撤去・維持の裁定、約束しないもの、既知の穴、各段の SHA | 全章(面ごとの詳細は各章。RPC の params は [第10章](10_tools.md) が正本) |

### ドラフト(実装済み or 実装中)

| 文書 | 実装状態 | 対応する章 |
|---|---|---|
| [design_scene_format.md](../design_scene_format.md) | ✅ WP25/63/79(`parent` は WP79 で追加) | [第4章](04_scene_ecs.md) |
| [design_ecs_lifecycle.md](../design_ecs_lifecycle.md) | ✅ WP62 | [第4章](04_scene_ecs.md) |
| [design_render_feature_modules.md](../design_render_feature_modules.md) | ✅ WP28〜31(feature = ユーザー空間ポリシーは §11.1 方針参照) | [第6章](06_rendering.md) |
| [design_compute_task_graph.md](../design_compute_task_graph.md) | ✅ WP33〜35 + WP64(実行一本化済み) | [第6章](06_rendering.md) |
| [design_material_shading.md](../design_material_shading.md) | ✅ M1〜M3.5(WP58/68/70/76/78/80/83) + stable material tag selection(WP206a) + pass-local named variant(WP206b) + material local-read(WP220)。spv-link は experimental | [第6章](06_rendering.md) |
| [design_color_pipeline.md](../design_color_pipeline.md) | ✅ v4 Accept・WP72〜74 で実装完了 | [第6章](06_rendering.md) |
| [design_postprocess_temporal.md](../design_postprocess_temporal.md) | ✅ T1/T2(WP88/95)。TAA は [design_taa_jitter.md](../design_taa_jitter.md) へ | [第6章](06_rendering.md) |
| [design_taa_jitter.md](../design_taa_jitter.md) | ✅ v2.1(条件付き受理)・J1/J1b/J1c/T-TAA すべて実装済み(WP112〜115) | [第6章](06_rendering.md) |
| [design_usd_openpbr.md](../design_usd_openpbr.md) | ✅ v2.1(条件付き受理)・M-PBR0a/0b + U-USD0a/0b/0c 実装済み(WP116〜119/124)。U-USD1 系は未 | [第5章](05_assets.md)・[第6章](06_rendering.md) |
| [design_openxr.md](../design_openxr.md) | ✅ v2.2・XR0〜XR4 + Meta XR Simulator gate実装済み(WP125〜138)。WP203a〜cでXR2b array/multiview/depth/profile gate、WP204でtile-local local readとのplanner/synthetic Vulkan接続をローカル実装済み。desktop mirror WSI lifecycleは**WP215〜217で実装済み**(XR実機・RDP/ディスプレイ切替のmanual platform gateは未消化)。現実装のSimulator/物理HMD・対象GPU実測、standaloneは未 | [第6章](06_rendering.md) §6.12・[第2章](02_getting_started.md) |
| [design_render_pipeline_extensibility.md](../design_render_pipeline_extensibility.md) [RPE] | ✅ RPE1〜RPE11d を実装済み(WP180〜202b)。draw sort / pass implementation / subgraph replacement / graph transform / render strategy の設計の正 | [第6章](06_rendering.md) §6.2 |
| [design_render_graph_compiler.md](../design_render_graph_compiler.md) [RGC] | ✅ 論理型カーネル・target planning・Vulkan physical plan・generic raster pass(WP185〜191/204/235〜238c)。**公開 ABI は未凍結** | [第6章](06_rendering.md) §6.2・§6.6 |
| [design_heterogeneous_execution_graph.md](../design_heterogeneous_execution_graph.md) [HEG] | 🚧 共通実行計画(WP238a)と generic raster dialect(WP238b)は実装済み。NativeScope(WP238c/d/e)は **source-level のみ**で公開 game-DLL ABI は未 | [第6章](06_rendering.md) §6.2・§6.6 |
| [design_wsi_epoch_recovery.md](../design_wsi_epoch_recovery.md) [WSI] | ✅ WP215/216/217 実装済み(ユーザー決定 2026-07-27)。RDP / ディスプレイ切替の manual platform gate は未消化 | [第6章](06_rendering.md)・[第2章](02_getting_started.md) |
| [render_mechanism_coverage.md](../render_mechanism_coverage.md) | ✅ 描画技法をコードへ再照合した棚卸し。残ギャップ(area/IES ライト、acceleration structure、bindless、VRS/foveation、instance 所有 tag、stencil 等)は同文書のギャップ表が正 | [第6章](06_rendering.md) |
| [render_authoring_ergonomics.md](../render_authoring_ergonomics.md) | 🚧 U2/U3/U5 の解消まで反映。**WP218 以降(material output ABI / ViewFamily / WP238 系)は未反映** | [第6章](06_rendering.md) |
| [design_debug_profiling.md](../design_debug_profiling.md) | ✅ D-P0〜D-P2 実装済み(WP139/140/143/145)。D-P3/D-P5/D-P4/D-P6/OPT は未 | [第10章](10_tools.md) |
| [shader_contract.md](../shader_contract.md) | ✅(FrameUBO/SSBO/resources manifest + material sampler/input-attachment ABIまで反映) | [第6章](06_rendering.md) |
| [design_input_actions.md](../design_input_actions.md) | ✅ I1〜I4 すべて(WP39/49/89/91) | [第7章](07_input_ui.md) |
| [design_ui_2d_foundation.md](../design_ui_2d_foundation.md) | ✅ v8 Accept・U0/U1/U2 実装済み(WP75/87/93) | [第7章](07_input_ui.md) |
| [design_2d_game_layer.md](../design_2d_game_layer.md) | ✅ v2.4・S2D-0a〜2 実装済み(WP103/104/106/107/109) | [第4章](04_scene_ecs.md)・[第6章](06_rendering.md)・[第8章](08_gameplay.md) |
| [design_event_payload_schema.md](../design_event_payload_schema.md) | ✅ v2 Accept・WP71 で実装 | [第8章](08_gameplay.md) |
| [design_animation_graph.md](../design_animation_graph.md) | ✅ v2.1・A0〜A2 + VRM application + VRMA-C0/R0/I0 実装済み(WP94〜102/111/121〜134/176〜178)+ §1 機構語彙 / §3 単一 writer は AG-1a/1b の規範。graph v2/live/SpringBone は未 | [第8章](08_gameplay.md)・[第4章 §4.2](04_scene_ecs.md) |
| [design_graph_composition.md](../design_graph_composition.md) | ✅ 共通グラフ `pelican.graph` の設計(定義と instance、入力 parameter、装着編集)。AG-0 / AG-1a / AG-1b / P-1 / N-2 の規範。**P-1 で「入力の二重指定は拒否」が「辺 > `parameters` > 既定値」に訂正されました**(文書側も同じ commit で訂正済み)。event(ノード出力からの発行)は v0 の後 | [第6章 §6.17](06_rendering.md)・[第13章](13_editor.md)・[第4章 §4.1](04_scene_ecs.md) |
| [design_asset_hot_reload.md](../design_asset_hot_reload.md) | ✅ v2.1・HR0〜HR2-G、targeted animation generation(WP147)、VRMA reload entry(WP178)実装済み。HR2-I/U3/VRMA watcher配線は未 | [第10章](10_tools.md) |
| [design_text_hud.md](../design_text_hud.md) | ✅ WP54 | [第7章](07_input_ui.md) |
| [design_game_logic_native.md](../design_game_logic_native.md) | ✅ G1/G2 + behavior attachment/edit + two-generation reload(WP90/155/162/167) | [第8章](08_gameplay.md) |
| [design_editor_tooling.md](../design_editor_tooling.md) | ✅ v2.5(条件付き受理)。authoring/service/RPC/transaction/undo/save/snapshot/watch/preview、汎用 gizmo feature/RPC と Studio 操作を実装済み(WP149〜172/WP274/275)。WebSocket は未 | [第13章](13_editor.md)・[第10章](10_tools.md) |
| [design_object_behaviors.md](../design_object_behaviors.md) | ✅ v2.1・BEH0/BEH1/BEH2 実装済み(WP155/162/167)。`onFixedUpdate` や behavior 間の直接参照は未 | [第4章](04_scene_ecs.md)・[第8章](08_gameplay.md) |
| [design_event_layer.md](../design_event_layer.md) | ✅ v1.1 E1 + typed schema + E2 Enter/Exit(WP56/71/179)。Stay は未採用 | [第8章](08_gameplay.md) |
| [design_scene_flow.md](../design_scene_flow.md) | ✅ S1(S2 は未) | [第8章](08_gameplay.md) |
| [design_physics_queries.md](../design_physics_queries.md) | ✅ P1/P2 + shapeCast/Provider ABI V2 + trigger events(WP107/179)。rpc P3/mesh/BVH/rigid simulation は未 | [第8章](08_gameplay.md) |
| [design_camera_system.md](../design_camera_system.md) | ✅ C1/C2 + user-space 再編の追記 | [第8章](08_gameplay.md) |
| [design_determinism_services.md](../design_determinism_services.md) | ✅ WP53 | [第8章](08_gameplay.md) |
| [design_audio.md](../design_audio.md) | ✅ A1。A2 は AU-1 で共通グラフの `audio.playback` と `playSound(path, "bgm")` に置き換え(Ogg Vorbis / streaming は AU-2 `e68573a2` で着地)。A3 は未 | [第8章](08_gameplay.md) |
| [design_persistence.md](../design_persistence.md) | ✅ WP55 + WP65(P1 実装済み) | [第3章](03_project_format.md)・[第8章](08_gameplay.md) |
| [design_asset_format_policy.md](../design_asset_format_policy.md) | ✅(KTX2 = WP92 済。状態列は古い) | [第5章](05_assets.md) |
| [design_asset_containers.md](../design_asset_containers.md) | ✅ K1〜K4(WP77/79/81/84) | [第5章](05_assets.md) |
| [design_project_dcc_houdini.md](../design_project_dcc_houdini.md) | ✅ import + import-tools(K3) | [第5章](05_assets.md) |
| [dcc_integration_qa_2026-06-12.md](../dcc_integration_qa_2026-06-12.md) | §6 = pelican.vat の正 ✅ | [第5章](05_assets.md) |
| [design_project_vcs.md](../design_project_vcs.md) | ✅ V1〜V3 + V2 manifest(WP66) | [第3章](03_project_format.md) |
| [design_project_interpretation_layer.md](../design_project_interpretation_layer.md) | ✅ WP44(現状記述は一部古い) | [第3章](03_project_format.md) |
| [design_build_tiers.md](../design_build_tiers.md) | ✅ B1/B2(B3/B4 未) | [第2章](02_getting_started.md)・[第10章](10_tools.md) |
| [design_devstudio_direction.md](../design_devstudio_direction.md) | 🚧 D1(shell / viewport / project / outliner)済。D2(選択・Inspector・gizmo)済。D3 は保存と undo/redo まで済み、`features[]` の編集と反映も入った | [第10章](10_tools.md) §10.7 |
| [design_roadmap_renderworld.md](../design_roadmap_renderworld.md) | 古い(一部失効) | — |
| [design_cloth_simulation.md](../design_cloth_simulation.md) | 古い(VAT レーンに実質置換) | [第5章](05_assets.md) |

### 指示書・運用・生成物

| 文書 | 内容 |
|---|---|
| [implementation_plan.md](../implementation_plan.md) | WP 詳細・受け入れ基準・共通規則(§0)と約2.2MBの履歴。対象WPだけを読む。現行の残件はv0/v1計画で確認する。**v0 の段(記号 ID)の完了記録もここが正**(§11.1 の v0 表は転記) |
| [build_features.md](../build_features.md) | CMake registryから生成する機能フラグの基準・対照・連動表。configureと契約テストの参照先 |
| [history/v0_delivery_2026-09.md](../history/v0_delivery_2026-09.md) | v1計画から分離した着地・途中判断・旧残件の履歴。現在の作業指示ではない |
| [v1_development_plan.md](../v1_development_plan.md) | v1 の現況・残件・開発運用。v0 の段(AG / P / X / O / R / N)の**出所**であり、「v0 で凍結するもの / 広く約束しないもの」の 5 行表を持つ(その表を実名に展開したのが [v0_freeze.md](../v0_freeze.md)) |
| [roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md) | 今後の候補 47 件・6 ティア + D-P / SA トラック(セッション引き継ぎ用。ウェーブ表は 2026-07-16 夜時点) |
| [adding_features.md](../adding_features.md) | 機能追加レシピ集 |
| [rendering_phase1_review.md](../rendering_phase1_review.md) | Validation Run の流儀 |
| `design_reviews/` | codex の監査・敵対レビュー・WP レポート(一次資料) |
| `schemas/` | pelican.ui 系 JSON Schema 4 本(U0 成果物) |
| `color_migration_manifest.json` / `material_resources_manifest.json` | WP72 / WP76 系の生成物 |
| `source-code-guide/` | ソースコード読解ガイド(全 12 ファイル。[第12章](12_codemap.md) は入口・概観、詳細はこちら) |

### 方針決定(横断・重要)

> **設計決定(feature 層 = ユーザー空間・2026-07-12):** エンジンは**機構語彙**(anchor / history / snapshot / format_class / projection_jitter 枠など — 版付きで additive にのみ増える)だけを持ち、feature(JSON + シェーダ)は**ユーザー空間**。同梱 feature(hdr / shadow / bloom / debug 系)は**特権なしの標準ライブラリ**で、`engine://features/*.json` をプロジェクトへコピーして改造するのが公式ワークフロー。いじれない境界(canonical anchor の全順序・output_transform 等の常設ノード・色 invariant・決定性ゲート)は名前入りエラーで防衛される。カメラのコントローラ/ブレンド/シェイクも同様にユーザー空間へ再編済み。

> **設計決定(temporal のユーザー管理境界・2026-07-16):** [design_taa_jitter.md](../design_taa_jitter.md) §0-1 の境界表が規範。判定基準は**「5 年後に発展しているのはどちら側か」** — 品質・アルゴリズム(taa.json 構成・resolve/composite シェーダ・パラメータ・ジッタ系列)は全部ユーザー管理、データ供給と安全境界(ジッタ適用点・単一提供者排他・FrameUBO 供給 field)だけがエンジン語彙。語彙が足りなければ**追加の語彙 WP でエスカレーション**する(こっそり特権化しない)。`halton23` 等の名前付きジッタ系列も非特権の便宜にすぎない(同じ数表の直書きと全 byte 一致することが fixture で証明されている — WP115)。

> **設計決定(版の扱い・2026-07-26 ユーザー決定):** 全ての versioned 形式は版フィールドを持ち、**現行版ちょうど 1 つだけを受理する**。版が省略可能な形式は必須化する(WP213/214)。旧版を受理する分岐も、旧版を新版へ昇格する処理も足さない — 版を上げるときは旧版の受理を同時に削除する。ただし**版システム自体は将来のために維持する**(いま版フィールドを削ると、互換が実際に必要になった時点で全形式へ足し直しになる)。**例外**: ABI の版一致チェックは互換残しではなく**食い違い検出**(古いゲーム DLL を silent crash でなく明確なエラーにする)なので保持する。**runtime epoch は version ではない** — `SurfaceEpoch` / `SwapchainEpoch` / temporal reset epoch は process-local な寿命マーカーであり、保存形式の解釈には使わない。正本 = [implementation_plan.md](../implementation_plan.md) §0「版の扱い」。

> **設計決定(build 依存の境界・2026-07-24):** Python はエンジン/ゲームの通常 build 依存から外れた(`PELICAN_PYTHON_TESTS` 既定 OFF)。shaderc は Vulkan SDK の明示 provider であり、**cross target へ暗黙の source-build fallback を持ち込まない**。CI gate や fixture 生成ツールを無理に C++ へ移植しない、という開発境界も同時に固定された(WP197〜199)。

> **設計決定(描画 mechanism WP の共通方針・2026-07-26):** technique 名をエンジンの enum へ足さず typed な resource / view / execution / selection 語彙を足す / パーサと API だけで完了にせず project-owned な feature・material・shader の dogfood を 1 本通す / logical authoring は trust-first・optimize-by-default(矛盾は hard error、単なる曖昧 tie は deterministic + advisory)/ raw binding は C 層の escape hatch として残す / physical 指定は WP204 の fragment へ link し logical config へ Vulkan の field を漏らさない / feature 未参照時に追加 pass・resource・variant を持たない(WP206〜212)。

> **方向決定(Quest standalone = SA トラック・2026-07-17):** MVP は PCVR(Quest Link)のまま、standalone は段階計画として記録: SA0(NDK/clang spike)→ SA1(GLFW 非依存の platform 層)→ SA2(standalone session — XR1〜3 を再利用)→ SA3(性能: multiview + dist-bake 必須級)。今から守る door-keeper 4 件(clang CI smoke / Windows 専用コードのユニット境界 / B3/B4 前提化 / タイル GPU 設計規律)。

## 11.3 既知の食い違い(設計文書 vs コード)

2026-07-19 の再調査で更新(レンダリング系は 2026-07-31 に再確認)。**常にコードが正**。

### バグ探索の消化状況

- **2026-07-21 の読み取り専用バグ探索(正本: [`design_reviews/2026-07-21_readonly_bug_hunt.md`](../design_reviews/2026-07-21_readonly_bug_hunt.md))で confirmed とされた 25 件は、25 件すべて修正済み**です。先行既知の 2 件(sprite-1 / vk-1)も修正済みで、**未処理は判断保留だった bug-18(`gamelogicreload.cpp` の `pollAttempt`)1 件だけ**です。個票は同レポートが正本なので、本章には転記しません。
- **2026-07-26 のバグ探索第 2 波は別調査**です(id 体系が違う。正本: [`design_reviews/2026-07-26_bughunt_priority.md`](../design_reviews/2026-07-26_bughunt_priority.md)。54 主張 → CONFIRMED 39 / PARTIAL 11 / REFUTED 4)。この波の window 出力群は WP215〜217 の lifecycle として設計・実装され、ほかも 07-26〜27 の `fix(` コミット群で消化されています。
- ⚠ 第 2 波の **V1(`DEVICE_LOST` の検出分岐が死にコード)は部分解消にとどまります**。現在 typed な device-loss 分類が入っているのは WSI 経路(`swapchainrecovery` / `frametarget`)と NativeScope の capability bit だけで、**エンジン全体の device-loss 回復ポリシー(ドライバ TDR とエンジンのバグを区別する)はまだありません**。

### 挙動に関わるもの

1. **strict v1 化(WP63)が一部の設計文書に未反映**: scene のレガシー受理(「WARN+自動解釈」)、カメラ旧キーのエイリアス受理 — 文書にはあるが、実装はすべて名指しの hard error。
2. **hello-project(web サンプル)の camera が旧形式**: web で開けて pelican で開けない = サブセット原則違反の実例([第9章](09_web.md) §9.7)。
3. **`--play-seq` / `--seq-mesh` の相対パスが cwd 基準**([PF] はプロジェクトルート基準と規定)。`--play-vat` は設計どおり。
4. **project.json の `name` が必須になっていない**([PF] は必須)。欠落は黙って `user://` 無効化に落ちる。
5. **audio の `playMusic` は作らず、AU-1(2026-09-25、`feca9dce`)で置き換えた**。BGM とループは共通グラフの `audio.playback`(`bus: "bgm"`)、ゲームコードからは `playSound(path, "bgm")`([第8章](08_gameplay.md) §8.11)。Ogg Vorbis と 10 秒を超える音の streaming は AU-2(2026-09-25、`e68573a2`)で着地した。
6. **rpc `raycast` を2026-09-06に接続**。origin/direction/max_distanceから既存PhysWorldのclosest hitを返す。PhysicsContext(旧 GameContext 直下)/PhysWorldの`shapeCast` all/closestは既存WP107の範囲であり、今回RPCには追加しない。
7. **`debugText` の色・倍率指定を2026-09-06に接続**。`debugText(x,y,text,color,scale)`。省略時は白・等倍(C-8 の facade 分割後は [`RenderContext::debugText`](../../src/core/userpublic/gamecontext.hpp#L160) = `ctx.render().debugText`)。
8. **OpenXR のセッション再生成はループ未配線**: [design_openxr.md](../design_openxr.md) §9 は swapchain/session 喪失時の再生成を規定するが、現行ループは teardown 要求で終了する(unwind・判定までは実装済み)。
9. **旧 sequential XR は Meta XR Simulator で検証済みだが、WP203c/WP204 の現実装と物理 HMD は未確認**: Simulator v201.0 で1000 frame/3分超/validation 0 は WP136/138 時点の旧 composition 経路。2-layer color/depth array、multiview、tile-local local read、Quest Link実機表示・入力、対象GPUの性能profile/帯域効果、standaloneは外部gate待ち。rpc は常に flat 駆動のため `get_status.xr.active=true` を rpc から観測する手段がない。

### キー名・形式の非対称(混同注意)

- **transform_seq は `rot`、scene / rpc は `rotation`**(意図的な別スキーマ判定)。
- **カメラの `yfov` はラジアン、ライトのコーン角は度、`--camera` の fov も度**。
- **rendering config のエンベロープは共通グラフのもの**: scene / asset_data / input_actions / import / material / render_feature / frame_plan / ui / anim_graph / atlas / input_seq / settings は自分の `schema` / `version` を持ちます。rendering config は MG-2b から `pelican.graph` root だけなので、最上位の `schema` は `pelican.graph`(`version: 1`)で、`pelican.render_config` のような独自の schema 名はありません。エンベロープを持たない旧い形(legacy root)は `render_config.legacy_root@1` で拒否されます([第3章 §3.2](03_project_format.md))。
- **3 つの `requires` は別の語彙**: project.json の `requires`(ビルド機能ユニットの名前の配列)/ package manifest の `requires`(`{engine_min_version, sdk_abi, capabilities}`)/ feature の `integrations[].requires`(他の feature 名)。[第3章 §3.2](03_project_format.md) に対照表があります。
- **3 つの `component` も別の語彙**: ECS のコンポーネント([第4章](04_scene_ecs.md))/ `pelican.render_component` = 共有部品([第3章 §3.12](03_project_format.md))/ package manifest の `provides.components`(= その共有部品のディレクトリを指す配列。[第3章 §3.11](03_project_format.md))。

### v0 の既知の穴(2026-09-17)

**約束の内側にあるが、今日は塞がっていないもの**です。正本は [../v0_freeze.md](../v0_freeze.md) §4(そこから台帳の該当節を引けます)。マニュアルの読者に影響が出るものだけをここに挙げます:

1. **(解消)`graph.instance.owner` の authoring object id は保存と再読込を跨いで保たれます** —— scene の保存が `authoring_object_id` と `next_authoring_object_id` を書き、読み込みがそれを使います(`efb30dad`、[sceneformat.cpp](../../src/project/sceneformat.cpp#L45)。[../v0_freeze.md](../v0_freeze.md) §4.1)。OL-1 で、owner より前のオブジェクトを `duplicate` し、先頭にもう 1 つ挿入してから保存・再読込しても owner が同じオブジェクトを指すことを `test/sceneformat_test.cpp` の `[ol1]` が確かめています。解決できない owner が**警告を出して非アクティブになる**(警告ログが 1 回出るだけ)になる挙動は残ります([第4章 §4.1](04_scene_ecs.md))。
2. **姿勢側の値スナップショットが無い** —— `get_graph_values` / `get_graph_node_values` が返すのは**描画側で publish した値**で、`source: "rendering"` を必ず返します。姿勢の評価は別の評価状態で走るので、event / trigger で駆動するノードがある文書では姿勢側が実際に食った値と**恒久的にずれ得ます**(event の無い文書ではずれません)。
3. **values パーティションは 1 フレームに 2 回評価されます** —— 描画側と姿勢側がそれぞれ評価するためです。入力が同じなので値は同じですが、**状態を持つノード(`value.trigger_clock` / `value.event_clock` / `audio.playback`)は 2 つの状態を並走させます**。1 回に束ねるのは v0 の後。
4. **`get_graph_values` は入れ子の instance を返しません**(root の instance の入力 port だけ)。展開木の全ノードが要るときは `get_graph_node_values` を使います。
5. **(解消)frame graph の buffer エントリは未知キーを名前付きで断ります** —— ✅EB-1(`aaa7b2da`)から、合成した構成の `buffers[]`(rendering 節・preset・feature の `buffers[]` と、graph の `render.buffer` / `render.attribute` の node を lowering したもの)の entry が frame graph の読まない鍵を持つと、`graph.buffer_key_unknown@1 {buffer, key, allowed}` で断ります(`allowed` は `name` / `size` / `size_from_extent` / `index_buffer` / `host_source` / `command_layout` / `initial_f32` / `view_family` / `lifetime`。[鍵を見る所](../../src/project/renderpipeline.cpp#L67))。EB-1 の前は、`initial_f32` を綴り間違えると名指しのエラーではなく**無音で 0 始まり**でした(P-2a-2 の完了記録)。出荷 7 project・test の fixture・engine の資源の JSON に未知の鍵を持つ buffer は無いことを実装者が確かめています。image(render target)の entry は EB-1 の時点では未知キーを拒否しませんでした(実装者の報告。指示書は「image と同じ流儀」でしたが、image の側にその検査がありませんでした)。→ ✅FO-1(`a4d7760a`)から、`render_targets[]` の entry(graph の `render.image` の node を lowering したものを含む)も、同じ合成の走査で `graph.image_key_unknown@1 {image, key, allowed}` で断ります(`allowed` は 17 個。[image の鍵を見る所](../../src/project/renderpipeline.cpp#L104)、[第6章](06_rendering.md) §6.2 の `render_targets`)。出荷 7 project・fixture・engine の資源の JSON に未知の鍵を持つ image の entry は無いことを実装者が確かめています。
6. **pass 名とレンダーターゲット名に名前空間がありません** —— 同じ pass 名を持つ package が 2 つ入ると合成で衝突します([第3章 §3.11](03_project_format.md))。
7. **`.spv` しか無いエンジンシェーダは、define 付き feature からは使えません**(per-pass 生成のヘッダを include するシェーダは事前コンパイルできないため source-only。`bloom_composite.frag` / `ssao.frag` がこの形)。package が自前のシェーダを持てないのも同じ理由です。
8. **(解消)configure の glob が `requires` を読みます** —— 前は、`PELICAN_WITH_ANIMATION=OFF` のビルドで `PELICAN_TEST_PROJECTS_DIR` を出荷コーパスに向けると、configure 時の glob が `requires` を読まないため、skip されるはずのプロジェクトのゲームロジック DLL まで作ろうとしてリンクに失敗していました(ビルドが赤になるのはテストが skip を決めるより前なので、`requires` の実行時の宣言では防げなかった)。`be35b7de` から、configure と test の走査が同じ `src/project/projectrequirements.def` を参照し(`cmake/pelican_project_requirements.cmake` が読む)、OFF のビルドでは対応しない sample の DLL の target を作りません([../v0_freeze.md](../v0_freeze.md) §4.1 の「configure の `requires` 無視」の行)。
9. **(一部解消)読めない material 文書で起動が止まらなくなりました** —— ✅FO-1(`a4d7760a`)から、`--strict-assets` を付けない起動では、asset_data の `materials[]` が指す文書が無い・読めないとき、その文書の material を登録せずに起動し、`get_problems` に `document_kind: "material"` の error(`material.document_unknown@1` / `material.document_invalid@1`)を、その material を指していた model に warning(`material.binding_unresolved@1`。その primitive は model の元の material で描く)を載せます。`list_materials` の `status` もその文書は engine の状態です([第6章](06_rendering.md) §6.7 の「起動時に読めない material 文書」)。FO-1 の前は起動が致命で終わりました(`--render-fallback` の対象外)。残るもの:
   - MT-2 の解けない object の上書き(scene の `simplemodelview.materials` の行が解けない名前を指す)は、問題の一覧に載りません。今も log の warning だけで、その行が無いものとして描きます([第6章](06_rendering.md) §6.7 の「object ごとの上書き」)。
   - 断られた文書は、ファイルを直しても engine を起こし直すまで登録されず、問題の一覧からも消えません。
     → ✅EB-2(`e1ba9058`)で解消しました。直した文書は `register_material {path}`(答えに `recovered: true`)か、`hot_reload` に宣言した文書なら保存で、起こし直さずに取り込まれ、`list_materials` が `loaded` になり、問題の一覧からその文書の error と由来する warning が消えます(Studio 側は MR-1s)。
   - (✅EB-2 から)watch が見ている登録済みの文書が起動後に読めなくなると、engine は前の値で描き続け、問題の一覧に source `hot_reload` の error が 1 件入ります。このとき `list_materials` のその行は、engine が持つ値ではなくファイルを読んだ結果(`invalid`、`materials: []`、`line` / `column` は無し)です。登録済みの文書を直したファイルから取り込み直す RPC は無く、`hot_reload` の宣言の無い engine は外の書き換えに気づきません([第6章](06_rendering.md) §6.7 の「起こし直さずに取り込む」)。
   - ✅FO-2(`31c390c7`)から、文書は読めるのにそれを描く pass が material の使うもの(ray query)を宣言していない場合は、「文書が読めない」ではなく `material.pass_contract_unmet@1`(`document` は pass を宣言している文書)で、`list_materials` は `unusable` です。pass が宣言すると次の frame で engine が取り込み直します([第6章](06_rendering.md) §6.7 の「pass の宣言漏れは別の code(✅FO-2)」)。FO-1 の時点ではこの場合も `material.document_invalid@1` で起動を続けていました(RT6 の test の赤で後から見つかった。[../ci.md](../ci.md) の「全数の記録」)。Studio の Materials のドックで直した文書を Save で取り込む道は ✅MR-1s(`5c26d492`)、pass へ飛ぶ道は ✅RTL-3s(`c80bc6a1`)です([第13章](13_editor.md) §13.1)。
10. **graph root で Frame Plan にしか残らない編集が 3 件あります**(✅GL-5g `cfa9859f` の実装者の報告): status 行の Reload(断られた edit の送り直し。対象外としている)、loop の copy の pass への Serial / Isolate(Graph で body の node を選んでも Decisions は出ない)、feature のまとまり(複数の pass の fold)そのものの Decisions(fold に潜れば pass ごとに出る)。どれも Frame Plan からは届きます([第13章](13_editor.md) §13.5 の GL-5d の段落)。
11. **配布の構成(`PELICAN_WITH_HOT_RELOAD=OFF`)の test を CI が回していません** —— ✅EB-4(`5df8b9d2`)で、OFF の構成で test が通るようにしました(名前付き skip 8 件、OFF でだけ登録する test 3 本)。ただし CI1 の構成 matrix の hot reload の行は OFF の tree を build して player の 2 つの確認をするだけで、OFF の構成で ctest は回していません。EB-4 の確かめは手元の 1 回です。また、`src/core/ecs/predefined/animationsystem.cpp` の `AnimationSystem::updateGraphs()` は OFF の構成でも毎 frame、animation graph の file の更新時刻を見て読み直します(配布でも毎 frame の stat。EB-4 の実装者の報告で、直していない)。詳細は [../ci.md](../ci.md) の「配布の構成(hot reload OFF)の test」。
12. **Studio のシナリオに間欠の赤があります** —— combo の選択肢が engine の非同期の答え(`get_material_bindings`・`list_surfaces` など)で埋まる前に、シナリオがその選択肢を選ぶ競合と読んでいます(`gui_rt_full_from_blank` の step 55 `has no entry Every Sphere`、`gui_material_ray_query_from_blank` の step 56 `has no entry shaders/query`。どちらも単独で流し直すと緑)。QA-1 で対応中です(統合側の記録。この文書の時点で未着地)。
13. **出荷 sample の絵の閾値の余裕が薄いものがあります** —— `project_catalog_headless_smoke` の 160x90 の frame 10 で、animgraph_demo(24 色 / 最小 20)と posegraph_demo(25 / 20)の色数、example の non-modal の割合(0.9612 / 0.90)は、絵が少し変わる変更で先に赤になりえます(EB-5b の実装者の表。[../ci.md](../ci.md) の「project catalog smoke の検査の中身」)。vrm_xr_demo の閾値は CC-1 の前の逆さの絵に合わせた値だったので、EB-5b(`85d0b06b`)で記録し直しました。

### v0 で約束しないもの(2026-09-17)

「まだ無い」ではなく、**v0 の契約として約束しない**と決めたものです(正本は [../v0_freeze.md](../v0_freeze.md) §3):

- **複合 undo** —— journal / undo の対象は**シーン編集の 13 op だけ**です(OL-1 で `duplicate` が加わった)(それ以外は `method_unavailable`)。rendering config の編集が約束するのは **CAS + トランザクション**、つまり「失敗したら何も変わらない」まで。文書単位の巻き戻しは内部にありますが、**シーンの undo スタックと rendering config の編集を 1 本に束ねる複合 undo は v0 の外**です(`### R-2:` の裁定)。グラフの編集(`edit_graph_document`)は scene の journal には載りませんが、Graph Canvas は graph 専用の基本 Undo/Redo を持ちます(CAS で適用し、外部で変更されたら履歴を捨てる。`0d41aeba`、[v0 凍結一覧 §4.1](../v0_freeze.md))。scene と graph を跨ぐ 1 本の履歴は v0 の外です。
- **代表ゲームの射程** —— 代表は `projects/example` で、触るのは入力・ECS 生成と `setLocalTransform`・VRM 表情・ライトアニメーションまでです。**物理クエリ・音・save/load・UI 操作・behavior のサンプルは v1**(example のシーンに collider は 0、リポジトリに音声ファイルは 0、[../example_assets.md](../example_assets.md) が新規バイナリを禁じ、`playSound` は `PELICAN_WITH_AUDIO=OFF` で throw するため「外せる」と両立しません)。
- そのほか v1 計画から: 全ドメインの高度な最適化と完全な seek / 任意の式言語と双方向 bind / 全 RT 機能の方式間互換 / native ABI の永久互換 / 稼働中の任意 module の unload。

### 文書の鮮度の問題

- `design_asset_format_policy.md` の状態列(VAT/EXR/音声/import/KTX2
  「未実装」)はすべて実装済み。
- **§11.4 ルール 6 が v0 の段で守られなかった(2026-09-17 に一括で解消)。** ルール 6 は「RPC メソッドを足したら [第10章](10_tools.md) の表に(編集系なら [第13章](13_editor.md) にも)、CLI 引数を足したら第10章 §10.2 の表に足す」ですが、v0 の各段(AG-0 〜 R-3 と Studio 側の N-1 〜 N-3 / N-5 / Q-1 / Q-1b)は**着地の時点ではマニュアルへ転記されませんでした**。対象は RPC 10 本(`get_graph_catalog` / `get_graph_instances` / `get_graph_values` / `get_graph_node_values` / `edit_graph_document` / `edit_graph_instances` / `export_render_feature` / `import_render_feature` / `install_package` / `uninstall_package`)、CLI 2 系統(`pelican_cli package` / `graph-catalog`)、player の `--start-time`、および v0 で増えた名前付きコード(`graph.*` / `pose.*` / `offline.*` / `project.requires_unknown@1` / `render_config.section_unlocated@1`)です。**2026-09-17 の更新でこれらを各章へ転記しました**。再発防止として、段の完了記録に「マニュアルのどの表に足したか」を書く運用が要ります(台帳側の課題)。

## 11.4 このマニュアルの更新ルール

本マニュアル(`docs/manual/`)は 2026-07-10 に全域調査から書き起こされ、
**2026-07-16 に WP64〜110**、**2026-07-17 に WP111〜137**、
**2026-07-19 に WP138〜179 の状態・event/physics/VRMA 差分**、
**2026-07-31 に WP180〜238e(レンダラ再構築。前回の宣言基準 `5bbaa5d` 以降 230 コミット
= merge を除く)**を反映しました。**2026-09-04 に WP375c〜385(scene load transaction /
game DLL ABI / 宣言 schema v3 / GPU lease)を §11.1 の追補表として足しました** ——
このとき **WP239〜374 は行として転記していません**(§11.1 冒頭の注記が正)。
**2026-09-17 に v0 の段(AG-0 〜 R-3 と Studio 側の N-1 〜 N-3 / N-5 / Q-1 / Q-1b。共通グラフ・姿勢・パラメータ・package・共有部品・
オフライン出力)を §11.1 の v0 表として足し、同時に第1〜4章・第6章・第8章・第10章・第13章・
第14章へ本文を転記しました**(上の §11.3「文書の鮮度の問題」の 2 つ目も参照)。
維持のためのルール:

1. **コードが正。** 設計文書と食い違ったら、実装を確認してマニュアルを実装に合わせる(意図の説明として設計文書を引用するのは可)。
2. **実装状況バッジを守る。** ✅実装済み / 🚧実装中 / 📐設計のみ(未実装)。WP がマージされたら該当章のバッジと §11.1 の台帳を更新する。設計文書にしかない機能を「使える」と書かない。
3. **JSON 例は実物から。** `projects/example/`・`projects/sprite_demo/`・`projects/animgraph_demo/`・`projects/posegraph_demo/`・`test/fixtures/`・`src/core/resources/` の実ファイル、またはパーサ実装で検証した形だけを載せる。創作例には「(例)」と明記。
4. **章の追加**: `NN_slug.md` で採番し、[00_index.md](00_index.md) の目次と、関係する章の相互リンクを更新する。章の冒頭形式(タイトル → 対象行 → 「この章で学ぶこと」→ 末尾「関連文書」)を踏襲する。
5. **バッジ更新のタイミング**: 形式(スキーマ)に触れる WP・strict 化 WP・機能追加 WP のマージ時。直近では HR2-I・U3・VRMA watcher・CI2 の着地時、**XR2b(WP203c)/ WP204 / WP216 / WP217 は外部 gate を通過した時点**で関係章を書き換える。加えて WP211 `dist-bake` の着地時に第2章・第10章を、**WP238 系の公開 game-DLL ABI が凍結された時点**で第6章を書き換える(それまで NativeScope を「使える」と書かない)。
6. **一緒に直すもの(再発防止)**: golden ケースを足したら `test/golden/inventory.json` にも登録する(件数の自動発見はもう無い)。RPC メソッドを足したら **正本に 1 回だけ書く**(基盤・読取の method は [第10章](10_tools.md)、編集系の method は [第13章](13_editor.md) の編集 RPC リファレンス)。もう片方には method 表の 1 行とリンクだけを足し、params・戻り値・拒否コードを 2 箇所に書かない(2026-10-06 改定。それまでの「両方に書く」が第 6・10・13 章の三重記載と食い違いを生んでいた。[文書の再編計画](../documentation_plan_2026-10-06.md) §1)。CLI 引数を足したら第10章 §10.2 の表に足す。**名前付きコード(`name@1`)を足したら、それを踏む利用者が読む章のエラー表**([第4章 §4.5](04_scene_ecs.md) / [第13章 §13.6](13_editor.md) / 該当章)**にも足す**。ビルド機能ユニット(`PELICAN_WITH_*`)を足したら [第2章 §2.2](02_getting_started.md) の表と、project の `requires` の語彙([第3章 §3.2](03_project_format.md))を確認する。
7. **§11.3 の食い違い一覧は「解消したら消す」**。設計文書側が改訂されたか、実装が設計に追いついたら該当行を削除する。
8. **着地の記録は manual に書かない**(2026-10-06)。着地日・commit hash・検証件数・「実装者の報告では」・着地時の「穴」は `docs/history/` か [実装指示書](../implementation_plan.md) に書き、manual には「今どうなっているか」と、今も有効な制限だけを現在形で書く。判定は 3 問: 「commit が変わっても真か」が No なら history、「利用者が書く値・読む値か」が Yes なら manual、「コードを開いた人が迷う所か」が Yes なら読解ガイド。

## 関連文書

- [../implementation_plan.md](../implementation_plan.md) — WP 台帳の一次資料
- [../roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md) — 今後の候補(47 件・6 ティア)
- [../README.md](../README.md) — 設計文書の索引
- [第1章 エンジン全体像](01_overview.md) / [目次](00_index.md)
