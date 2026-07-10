# 2D 描画基盤と UI システム(v1)

対象読者: エンジン担当・UI/2D を作る人。
ステータス: v1 ドラフト(2026-07-10。**codex 敵対レビュー前**)。
前提: 2026-07-07 決定「UI と 2D ゲーム機能は**描画基盤(2D パス)を共有し
上物を分離**」。`design_text_hud.md`(debug_text — 先行する類似実装)、
`design_asset_containers.md`(PSD → pelican.layout)、イベント層 E1(WP56)、
入力アクション層、フレームグラフ。

## 0. 三層構造(決定済み方向の具体化)

| 層 | 内容 | 本書 |
|----|------|------|
| 2D パス(基盤) | クアッドバッチング・スクリーン座標・ソート・アトラス | §1 |
| UI 層 | ウィジェットツリー(pelican.ui)・レイアウト・入力・イベント | §2〜5 |
| 2D ゲーム層 | スプライト・2D カメラ・2D 物理・3D 相互変換 | **別文書**(スコープ予約のみ §7) |

## 1. 2D パス(基盤)

- **クアッドバッチャ 1 本**: sprite / 9patch / テキストグリフ / 単色矩形を
  すべて「テクスチャ付きクアッド」に正規化して 1 つの頂点ストリームに積む。
  テクスチャアトラス前提(#フラグメント `atlas.png#sprite/name` がそのまま
  参照語彙になる — asset containers 設計との接続)
- 座標系: **ピクセル・左上原点**(debug_text と同一規約)。DPI/スケーリングは
  「仮想解像度 + 整数/自由スケール」を config で宣言(未決 1)
- ソート: **レイヤー番号 + 同レイヤーは宣言順**(決定的 — golden 可能)。
  Z バッファ不使用(半透明前提のペインターズ)
- フレームグラフ上は post_ldr 以後の **ui アンカー**の 1 パス(既存 UI パスの
  置換として実装)。reads/writes 宣言に乗るので順序は機械保証
- debug_text(WP54)は v1 では独立のまま。基盤安定後にグリフ描画を
  この バッチャへ移設して統合(v1.1 — 二重実装の期限を切る)

## 2. pelican.ui v1(ウィジェットツリー形式)

```json
{
  "schema": "pelican.ui",
  "version": 1,
  "root": {
    "type": "panel", "id": "hud",
    "anchor": "bottom_left", "offset": [16, -16], "size": [320, 96],
    "style": { "skin": "atlas.png#sprite/panel_dark" },
    "children": [
      { "type": "gauge", "id": "hp", "props": { "value_param": "/game/player/hp", "max": 100 } },
      { "type": "label", "id": "name", "props": { "text": "PLAYER" } },
      { "type": "button", "id": "menu", "props": { "text": "MENU" },
        "emit": { "on_click": "MenuOpened" } }
    ]
  }
}
```

- ノード = `type` + 共通属性(id / anchor / offset / size / layer / visible)+
  型別 `props` + `children`
- **レイアウトはアンカー + オフセットのみ(v1)** — 9 方位アンカー(親基準)と
  ピクセルオフセット。フレックス/グリッドは v2(需要が出てから。
  ゲーム HUD の大半はアンカーで足りる)
- スタイル: 値の直書きに加えて **トークン参照**(`"@color/accent"` 等)を許す。
  トークン表は project://ui/tokens.json — **web デザインシステム(my_webpage の
  --ds-*)と語彙を揃える**(共有はしない・対応表を維持、devstudio 決定と同じ方針)
- 状態スタイル: `style.hover` / `style.pressed` / `style.disabled` の差分オーバーレイ
- **標準スキン = ツール調(2026-07-10 ユーザー決定)**: エンジン同梱のスキンは
  1 個だけで、**フラット・直角・1px 境界・高密度**(VSCode/ImGui の世界)。
  デバッグ画面やモックがそのまま開発ツールの顔になる。ゲームらしい装飾
  (グラデーション・光る角・大きめの余白)は**プロジェクト側のスキン**
  (`project://ui/skins/*.json` + アトラス)として同じウィジェットツリーに
  当て替える — マテリアルの「同梱は最小・表現はコンテンツ」原則の UI 適用

## 3. ウィジェット語彙 v1(最小 6 種)

| type | 内容 | 描画要素 |
|------|------|---------|
| panel | 9patch コンテナ | 9 クアッド |
| image | sprite/画像 | 1 クアッド |
| label | テキスト(v1 は debug_text と同じビットマップ、v2 で SDF) | グリフ列 |
| button | panel + label + 状態 + click | 合成 |
| gauge | バー(value 0..1、fill 方向) | 2 クアッド |
| stack | 子を縦/横に等間隔配置(レイアウト専用・非描画) | なし |

追加(list / slider / input 等)は v2 — **語彙追加は形式のマイナー改訂**
(スキーマの流儀どおり)。未知 type は名前入りエラー(web は skip + WARN —
サブセット原則の scene と同じ扱い)。

## 4. 入力とイベント(既存資産への接続)

- ポインタ入力は入力スナップショット層から UI が**最初に**消費(UI がヒットしたら
  ゲームへ流さない — capture 規則)。ヒット判定は矩形(v1)
- **UI はコールバックを持たない — イベントを emit する**(E1/WP56 の実戦投入):
  `"emit": { "on_click": "MenuOpened" }` → ゲームシステムが
  `onEvent(const MenuOpened&)` で受ける。UI とロジックの結合が
  イベント層の規約(次フレーム配送・決定的順序)にそのまま乗る
- gauge の `value_param` のような**データ束縛は v1 では GameContext API**
  (`ctx.setUiValue("hp", 0.7f)`)で明示更新(宣言的束縛は将来)
- フォーカス/キーナビゲーションは v2(ゲームパッド UI 操作と同時に設計)

## 5. データ経路と開発体験

- pelican.ui は project://ui/ に置く(既存 ui_config_json の後継 —
  現行 `{"images":[]}` 形式は strict v1 の流儀で移行後に廃止)
- **PSD レーン接続**: pelican.layout(PSD 展開の出力)→ pelican.ui への変換を
  import レシピで提供(タイトル画面を PSD で組む → そのまま出る、の実現)
- ホットリロード対象(アセット HR 基盤に乗る): ui JSON 保存 → 即反映が
  UI 制作の反復速度の生命線
- golden: ウィジェット全種を並べた 1 ケース + 状態(pressed 等)1 ケース。
  rpc inject_input でクリック → イベント発火の結合テスト

## 6. web プロファイル

形式(pelican.ui / tokens)は web でも読める(サブセット原則)。web 側の
描画は DOM で実装可能(primitives 資産の再利用)だが、実装時期は需要が
出てから。**トークン語彙の対応表だけ先に維持**する。

## 7. 2D ゲーム層(別文書への予約)

スプライトコンポーネント・2D カメラ(orthographic は C1 で対応済み)・
2D 物理(Box2D 予約)・**2D⇔3D 相互変換**は `design_2d_game.md`(未起草)の
スコープ。本書の 2D パスはその描画基盤を兼ねる(スプライトはワールド座標系
バッチとして同じバッチャの別インスタンスを使う想定 — 未決 3)。

## 8. 実装順(WP 候補)

| 段階 | 内容 | 依存 |
|------|------|------|
| U1 | 2D バッチャ + ui パス置換 + panel/image + pelican.ui パーサ(純ロジック)+ golden | なし |
| U2 | label(debug_text のグリフ流用)+ button + 入力 capture + イベント emit + inject_input 結合テスト | U1, E1 |
| U3 | gauge + stack + トークン + 状態スタイル + PSD→ui 変換レシピ | U2 |

## 9. 未決事項

1. 仮想解像度と DPI スケール(整数スケール固定か自由か)
2. 9patch のマージン宣言の置き場所(アトラス側メタデータ vs style)
3. 2D ゲーム層とのバッチャ共有形態(同一インスタンス or 座標系別インスタンス)
4. label の日本語(SDF/動的アトラス)は text_hud v2 と同時(本書 v1 は ASCII)
