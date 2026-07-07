# テキスト描画: デバッグ HUD 層(v1)

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-08。レビュー前)。
前提: `design_render_feature_modules.md`(パージ可能 feature)、
debug_draw(WP29 — 同型の前例)、将来の 2D/UI 設計(本格テキストの受け皿)。

## 0. 二段階方針

| 段階 | 内容 | 位置づけ |
|------|------|---------|
| v1(本書) | 組み込みビットマップフォント(ASCII)の **debug_text feature** | デバッグ・計測表示・入力可視化 HUD。パージ可能 |
| v2(別設計) | SDF/MSDF フォントアセットレーン(ttf → アトラスを import で焼く)+ 日本語(動的アトラス)+ UI/2D 統合 | ゲーム UI 本番用。2D 設計と同時に起草 |

v1 を先行させる理由: gpu_timing の数値・物理可視化のラベル・シナリオテストの
状態表示など**開発体験の需要が今ある**。v2 の設計判断(SDF か MSDF か、
日本語アトラス戦略、IME)は 2D/UI と不可分なので切り離す。

## 1. v1 仕様

- `engine://features/debug_text.json` — パージ可能(参照 = 存在)。
  debug_draw と同型: スクリーン空間パス、post_ldr 相当の最後段
- フォント: **エンジン埋め込みの等幅ビットマップフォント**(ASCII 95 字、
  8x16 か同等。PNG アトラス + 座標表を engine:// リソースに。ライセンスは
  public domain のもの)
- 描画: 文字列 → quad 列(インスタンス or 頂点生成)。色・スケール
  (整数倍のみ — にじみ防止)。座標はピクセル(左上原点)
- API: `DebugText` モジュール(エンジン内部・feature 系から使う)+
  `GameContext::debugText(x, y, text)`(毎フレーム呼ぶ immediate 型 —
  debug_draw と同じ寿命規約)。feature 不参照時は no-op(存在しない機能を
  呼んでも落ちない — debug_draw と同じ)
- 用途の内蔵例: gpu_timing の結果表示(feature 参照時のみ)は
  接続だけ予約(v1 に含めない — 未決 2)

## 2. 検証

golden 1 ケース(固定文字列 — ビットマップなので完全決定的)+
feature 不参照時の golden 全維持 + no-op 動作のテスト。

## 3. 未決事項

1. 埋め込みフォントの選定(候補: Cozette / Spleen / IBM VGA 系の
   public domain ビットマップ。8x16 推奨)
2. gpu_timing → debug_text の自動 HUD(features が features に依存する
   初例になる — 依存宣言の仕組みは合成側にある。v1.1 で)
3. web プロファイル: debug_text は web でも同じ feature 形式で書けるが
   実装は需要が出てから
