# デバッグ・プロファイリング・最適化トラック(v1.1 — 条件付き受理)

対象読者: エンジン担当・パフォーマンスを見る人・絵の不具合を追う人。
ステータス: **v1.1(2026-07-17)— 敵対レビュー
`docs/design_reviews/2026-07-17_debug_profiling_review_codex.md` で
条件付き Accept(v1 文面のままの着手は Reject)。同レビューの
C1〜C9 が本書の各節に優先する規範であり、各 WP 登録時に該当条件を
逐語添付すること。**

v1.1 の要点(C1〜C9 の骨子 — 原文が正):

- **実装順は C9 の再分割が正**: `D-P0a(debug-utils 基盤 + command
  label — pass/RT の既存正本のみ)→ D-P1a(flat/headless capture)→
  D-P2a(stereo-safe GPU timing)` を第一波。resource 全面命名
  (D-P0b)・memory/XR timing(D-P2b)・XR capture・CI 分割・
  crash/device-fault は後続 WP(§9 の旧 順序表は破棄)
- D-P0 の「名前の正本は既に全部ある」は**誤り**(C1) — pass/RT のみ。
  buffer/pipeline 等は命名を運ぶ API から(D-P0b の inventory)
- D-P1 = **注入済み RenderDoc module の受動取得のみ(LoadLibrary
  禁止)**。RPC capture は render_frame と同型の一回 capture・
  実際に増えた capture index から .rdc path 取得・F11/RPC/重複要求/
  失敗 code/flat・headless・XR の境界を状態機械で固定(C2)
- D-P2 のキー = `graph variant + logical frame + view index + node
  ordinal/kind/name + subrange`(**左右眼を混ぜない**)。query pool は
  in-flight ring(C3)。VRAM = driver budget と engine 論理 allocation の
  分離・`shouldRender=false` は dropped と数えない・XrTime↔QPC は
  KHR 拡張がある場合のみ(C4)
- **計測値は diagnostics として決定性比較から正規化除外**。simulation
  state と final RGBA8 は厳密一致維持。.rdc の byte 一致を gate に
  しない(C5)
- Tracy = `PELICAN_ENABLE_TRACY`(build-tier 規約の明示例外)。OFF 時
  symbol 不在・dist 強制 OFF・ON-disconnected cost・Tracy Vulkan の
  query pool 追加を gate に(C6)
- D-P5 は CI0/CI2/toolchain/dist-bake の境界で分割(GPU validation は
  CI2 依存・dist-bake validation は B4 依存)(C7)
- D-P6 は user-mode crash と device loss に分割。**crash handler 内で
  quill/JSON graph を走らせない**(別 process dump helper +
  pre-serialized breadcrumb)。PDB/build-id・privacy/retention・
  RenderDoc crash handler との所有関係を契約に(C8)
前提: gpu_timing feature(WP29)・startup 計測(WP82)・ImGui/Plan
viewer(WP85/86)・golden/replay 決定性・PELICAN_ENABLE_ASAN(WP62)・
ecs_benchmark・「feature 層 = ユーザー空間」方針・ビルドユニット規律
(PELICAN_WITH_* 既定 ON・dist OFF)。

## 0. 三層(このトラックも同じ切り分け)

| 層 | 内容 | 帰属 |
|----|------|------|
| **機構(計測語彙)** | debug marker 命名・GPU/CPU カウンタ・capture フック・crash 診断 | エンジン(版付き additive) |
| **表示** | ImGui オーバーレイ・get_status 拡張 | エンジンツール(D0 — 特権なし・rpc と同じ情報) |
| **外部統合** | RenderDoc・Tracy | **opt-in ビルドユニット**(dist OFF・不在でも全機能動作) |

原則: **計測は常設・軽量(リリースでも数える)、表示と外部統合は
purgeable**。最適化 WP は必ず「計測 → 数字を根拠に変更 → 同じ計測で
before/after をレポート」の形(WP82 起動高速化の流儀を標準化)。

## 1. D-P0: debug 命名の全面付与(RenderDoc の土台・最優先)

`VK_EXT_debug_utils` の object 名 + コマンドラベルを**既存の論理名から
機械的に**付ける:

- パス = frame plan のパス名(`cmdBeginDebugUtilsLabelEXT` で
  anchor/feature 由来名も)・RT = rendering config の id・
  buffer = 用途名(`vertices_mem_pool` 等)・pipeline = material/
  variant 名・texture = 宣言名・descriptor set = set 番号 + 所有者
- **名前の正本は既に全部ある**(frame plan・rendering config・
  material 名)— 新しい命名体系を発明しない
- validation layer のメッセージにも同じ名前が出る副次効果
- 有効条件: debug utils 拡張が存在する時のみ(不在で無害)。
  dist では既定 off(1 行で on 可 — 実機プロファイル用)
- gate: RenderDoc でキャプチャした際にパス階層・全リソースが
  論理名で読めるスクリーンショットをレポートに。golden byte 不変

## 2. D-P1: RenderDoc in-app capture(小)

- `renderdoc_app.h` の in-app API(injected 時のみ dll を掴む —
  リンク依存なし・不在で無害)
- トリガ 3 系統: **F11**(ImGui 常駐と同じ流儀)/ **rpc
  `capture_gpu_frame`**(戻り = .rdc パス — replay と組んで
  「決定的に N フレーム目をキャプチャ」が可能になる)/ ImGui ボタン
- ドキュメント: adding_features.md に「絵の不具合を追う手順」
  (replay で再現 → 該当フレームで capture → D-P0 の名前で読む)
- gate: rpc capture の結合テスト(RenderDoc 不在環境では名前入り
  「not injected」エラー)・golden 不変

## 3. D-P2: GPU 計測 v2(gpu_timing の深化)

- **階層タイミング**: パス内の feature 由来サブ区間もラベル単位で計測。
  get_status.gpu_timing を階層 JSON に(additive)
- **フレーム履歴オーバーレイ**: ImGui に直近 120 フレームの
  パス別積み上げグラフ(Plan viewer の隣)
- **VRAM 予算**: `VK_EXT_memory_budget` + 自前 allocator 統計
  (mega-buffer free-range の使用率/断片化・texture/RT 合計)を
  get_status.memory に
- **XR フレーム計測**(実機に必須): xrWaitFrame margin・
  shouldRender 率・dropped frame 数を get_status.xr.timing に
- gate: 計測 on/off で golden byte 不変(query pool は既存規約どおり
  不参照時不生成)

## 4. D-P3: CPU フェーズ計測(軽量スコープタイマ)

- フレームを既存の論理フェーズ(input → ECS systems(system 別)→
  animation phases(WP102 の phase 名)→ record → submit → present
  wait)で常設計測。リングバッファ 120 フレーム
- get_status.cpu_timing + ImGui 積み上げ(D-P2 と同じオーバーレイに
  CPU/GPU 並記 — 「どちらがボトルネックか」を 1 画面で)
- 決定性: 計測値は get_status/表示のみに使い、**ロジックに
  フィードバックしない**(タイマ値が replay に影響しない規範)
- gate: オーバーヘッド実測(計測常設で フレーム時間 +1% 未満)・
  golden/replay byte 不変

## 5. D-P4: Tracy 統合(opt-in ユニット・中)

- `PELICAN_WITH_TRACY`(既定 **OFF** — 唯一の既定 OFF ユニット。
  理由: ネットワーク接続を開くプロファイラを既定に入れない)
- D-P3 のスコープと D-P0 の GPU ラベルを Tracy zone へブリッジ
  (計測点は共有 — Tracy 専用の計測を書かない)
- 用途: フレームスパイク・ロック競合・メモリ churn の深掘り
  (自前オーバーレイでは足りない調査の受け皿)
- gate: ON ビルドの smoke + OFF で完全不在(シンボル 0)

## 6. D-P5: 検証の常設化(CI 品質)

- **validation layer on の golden 実行**を ctest に常設(現在は
  手動)— 全パスが validation クリーンであることを回帰で保証。
  実行時間対策に代表 golden のサブセットで可
- **UBSan 構成**(WP62 の宿題)+ ASan 構成の定期実行手順
- shader 検証: spirv-val を dist-bake/compile 経路に常設(未実施なら)
- gate: validation エラー 0 が REQUIRE(新規 WP が violation を
  入れたら即赤)

## 7. D-P6: クラッシュ診断(中)

- Windows minidump writer(未処理例外 → .dmp + ログ flush +
  直近の get_status スナップショット併記)
- `VK_EXT_device_fault`(存在時)で device lost の詳細を回収
- ユーザー向け: クラッシュ時に「この 3 ファイルを報告」が揃う形
- gate: 意図的クラッシュの結合テスト(dump 生成 + ログ末尾一致)

## 8. OPT 系(計測が入ってから — 数字駆動)

順序は「計測 → 上位から」。現時点の既知候補(着手前に D-P2/P3 の
数字で裏取りすること):

1. **OPT-S: 起動第 2 弾** — 実測で models 1.9s が支配的。
   example の KTX2 一括化(WP92 レシピ適用)+ モデルロード並列化。
   目標: warm 2.2s → 1.2s 級
2. **OPT-XR: Release 実機ベースライン** — Release フルゲート常設化 +
   Quest 3 実機で D-P2 の XR timing 計測。72Hz 予算(13.9ms)に対する
   パス別内訳を最初のレポートに
3. **OPT-MV: multiview 評価** — 現行は view 逐次描画(WP129)。
   `VK_KHR_multiview` で geometry 系パスの CPU/GPU 削減を**計測付きで
   評価**(効果が薄ければ入れない — 判断材料をレポートに)
4. **OPT-CULL/BATCH**: 描画件数系(頻度順ソート・instance 統合)は
   実プロジェクトの数字が出てから(現 example では過剰最適化)

## 9. 実装順と依存

| WP | 内容 | 依存 | 見積 |
|----|------|------|------|
| D-P0 | debug 命名全面付与 | なし | 中 |
| D-P1 | RenderDoc capture | D-P0(価値の前提) | 小 |
| D-P2 | GPU 計測 v2 + VRAM + XR timing | WP29/133 | 中〜大 |
| D-P3 | CPU フェーズ計測 | なし(D-P2 と表示統合) | 中 |
| D-P5 | validation 常設 + UBSan | なし | 中 |
| D-P4 | Tracy ユニット | D-P0/P3 | 中 |
| D-P6 | crash 診断 | なし | 中 |
| OPT-S/XR/MV | 数字駆動最適化 | D-P2/P3 | 各中 |

推奨の最初の 3 連: **D-P0 → D-P1 → D-P2**(RenderDoc が実用になり、
実機 XR の数字が取れる状態を最短で作る)。D-P5 は隙間の独立 WP。

## 10. 未決事項

1. Tracy の GPU zone(Vulkan timestamp との統合)を v1 に含めるか —
   D-P4 設計時に工数見て判断
2. フレームキャプチャの自動比較(golden の RenderDoc 版)— 需要待ち
3. Release ゲートを標準ゲートに昇格するか(現在 Debug のみ)—
   OPT-XR で Release 常設化した後に判断
