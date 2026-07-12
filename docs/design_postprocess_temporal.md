# ポストプロセススタックと時間軸リソース(フレームグラフ v2.5)

対象読者: エンジン担当・ポストエフェクトを feature として書く人。
ステータス: v1 ドラフト(2026-07-08。レビュー前)。
前提: `design_compute_task_graph.md` v2(統一フレームグラフ)、
`design_render_feature_modules.md`(feature fragment・アンカー)、
WP31(shadow — feature 合成の実証済み)。

## 0. 目的

SSAO・DoF・モーションブラー・TAA・temporal filter 系は
**前フレームの情報(history)と motion vector(velocity)**を要求する。
現フレームグラフは単一フレーム内の DAG で、この 2 つは feature fragment の
書き方では追加できない**グラフ本体の拡張**。ここを先に設計し、以後の
ポストエフェクトが「feature JSON + シェーダだけ」で書ける状態にする。

3 部構成: §1 history リソース(グラフ拡張)、§2 velocity(標準 feature)、
§3 標準ポストスタック規約(アンカーと中間 RT の慣習)。

## 1. history リソース(フレームをまたぐ RT)

### 1-1. 宣言

RT 定義に `"history": true` を追加(rendertarget 定義の拡張キー):

```json
{ "id": "taa_accum", "format": "...", "history": true }
```

- グラフは **2 面(N / N-1)を自動管理**し、フレーム境界でフリップする。
  利用側は面を意識しない
- 読み: パスの reads に `"taa_accum@history"` — **前フレーム面**を読む。
  書き: writes に `"taa_accum"` — 現フレーム面に書く。
  同一パスが history を読み現面を書くのが典型形(accumulation)

### 1-2. プランナ上の意味論

- `@history` read は**フレーム内依存を作らない**(前フレームの内容は
  フレーム開始時点で確定済み)。よって既存 DAG の順序自由度を壊さない
- 同一フレーム内で `X@history` read と `X` write が両方あっても
  hazard ではない(別イメージ)。`X` への writes-writes 曖昧は従来どおり
  hard error
- プラン JSON(pelican.frame_plan)には `"reads_history": [...]` として
  現れる(スキーマ v1 に追加キー — 後方互換、比較 fixture は更新)

### 1-3. ライフサイクル規約

| 事象 | 挙動 |
|------|------|
| 初回フレーム | history 面は**定義のクリア値でクリア済み**を保証(読んだら黒、ではなく宣言的)。「有効データか」をシェーダが知りたい場合のために frame UBO に `frame_index` が既にある |
| リサイズ | 両面とも再生成 + クリア(= 履歴リセット)。ちらつきより正しさ |
| set_time による時間ジャンプ | 履歴リセット(rpc 駆動の決定性のため — 未決 1) |
| 決定性 | 「同一初期状態から同一フレーム数」で完全一致。golden は `step_frame ×N → capture` の既存流儀で扱える |

## 2. velocity buffer(標準 feature)

`engine://features/velocity.json` として提供(パージ可能 — 参照 = 存在):

- motion vector パス: 各オブジェクトの **前フレーム model 行列**との差分 +
  カメラの view-proj 差分から screen-space velocity(RG16F)を出す
- 前フレーム行列の保持はレンダラ側(model matrix バッファの 2 面化 —
  history 機構の buffer 版だが、v1 は velocity feature の内部実装として
  閉じる。汎用 buffer history への昇格は需要が出てから)
- 他 feature は reads に `velocity` を書くだけで依存がつながる
  (motion blur / TAA の入力)
- カメラジッタ(TAA)は**本設計に含めない**(projection への介入はカメラ系。
  未決 2)

## 3. 標準ポストスタック規約(正本は色パイプライン文書)

**アンカー列と色の正本は `design_color_pipeline.md` v4 §2-2 の canonical
frame-plan anchor である**(2026-07-11 改訂 — 本節が旧規定していた
`scene_color → post_main → tonemap → post_ldr → swapchain` の列と
「scene_color は常に linear HDR」「golden = swapchain 相当」の色方針は
**撤回**し、色文書への規範参照に置き換える。hdr off の scene 域 format・
`display`/`output_transform`・capture 契約はすべて色文書が定める)。

本書に残るのは機構でなく慣習の部分のみ:

- 同一アンカー内の順序は宣言順タイブレーク(既存)+ 必要なら明示エッジ
- **解像度クラス**: 中間 RT の命名慣習 `<name>_half` / `<name>_quarter`。
  ダウンサンプル済み scene 色(`scene_color_half`)は最初に要求した
  feature が生成し、以後は reads で共有(bloom と dof が半解像度を
  二重に作らないための慣習)。format は色文書の format_class に従う

## 4. 移行(WP 候補)

| 段階 | 内容 | 依存 |
|------|------|------|
| T1 | history 機構(RT 2 面化 + `@history` reads + プランナ/ランタイム + plan スキーマ追記 + fixture)。実証 = 最小 accumulation feature(前フレームと 50% ブレンド)の golden(step_frame ×N で決定的) | なし |
| T2 | velocity feature(前フレーム行列保持 + motion vector パス + golden) | T1 不要(独立)だが同時期推奨 |
| T3 | 実証エフェクト 1 本 = **モーションブラー**(velocity 消費、履歴不要で単純)。TAA はジッタ設計(未決 2)解決後 | T2 |
| — | アンカー慣習・カラー方針は T1 の PR で adding_features.md に追記(コード無し) | |

## 5. 未決事項

1. `set_time` 時の履歴リセットの粒度(全 history か、feature が opt-out
   できるか)。推奨: v1 は全リセット(決定性最優先)
2. TAA のカメラジッタ — **方向確定(2026-07-12 ユーザー相談)**:
   「カメラ設計側に注入点を予約」を**撤回**し、**ジッタはカメラ API から
   完全分離したレンダー側の projection modifier 枠**とする。根拠:
   ①消費者はテンポラル一族のみ(TAA / DLSS・FSR2・XeSS 系アップスケーラ
   (ジッタ + velocity + depth を要求する)/ チェッカーボード / 静止時
   アキュムレーション)②カリング・レイキャスト・UI 投影・ゲームプレイは
   **非ジッタ行列を使わねばならない**(見せてはいけない)。骨子:
   - feature が `projection_jitter` を宣言(pattern: halton23 等・
     phases)。**同時に 1 提供者**(併存は名前入りエラー — ターミナル
     フック排他と同じ流儀)
   - 系列は **frame_index の純関数**(決定的 — golden/replay と整合)
   - 適用は scene ラスタパスの投影のみ。FrameUBO に「適用済み proj +
     今/前フレームのジッタオフセット」を追加(velocity のジッタ差引と
     TAA resolve 用)。カリングは非ジッタ
   - アップスケーラ対応の将来要件(フェーズ数が解像度比に依存・
     mip bias)はこの枠の属性として拡張 — ユーザーの TAA 設計着手時に
     API 詳細を確定して WP 化
3. history の面数: 2 固定か N 指定可か。推奨: v1 は 2 固定
   (N が要るのは高度な temporal 系のみ、需要が出てから)
4. buffer(非 image)の history 昇格(GPU パーティクルの状態バッファは
   WP34 で既に 2 面化の前例あり — 統合するか別機構のままか)
