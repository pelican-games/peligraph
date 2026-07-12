# 2D ゲーム層と 2D⇔3D 相互変換(v1)

対象読者: エンジン担当・2D ゲームを作る人・2D/3D 混在演出を作る人。
ステータス: v1 ドラフト(2026-07-12。ユーザーレビュー・codex 敵対レビュー前)。
前提: 2026-07-07 決定「UI と 2D ゲームは描画基盤共有・上物分離」、
`design_ui_2d_foundation.md` v8(quad ABI・K3 atlas)、
`design_camera_system.md`(orthographic は C1/WP48 で実装済み)、
scene v1(コンポーネント additive)、「feature 層 = ユーザー空間」方針、
サブセット原則(web で開ける ⊆ pelican で開ける)。

## 0. スコープ定義 — 「2D⇔3D 相互変換」とは何か

最初に相互変換の意味を確定する。要求は 2 方向:

- **2D → 3D(2D シーンの 3D 空間配置)**: スプライトで作った 2D コンテンツ
  (キャラ・背景・エフェクト)を 3D 空間の任意の位置・向きに置ける
  (ペーパーマリオ型・3D 空間内の看板/画面)
- **3D → 2D(3D シーンの 2D 投影編集)**: 3D コンテンツを直交投影で
  2D ゲームとして見せる/編集する(3D モデルのサイドスクローラー・
  devstudio の 2D ビュー編集)

### 0-1. 中核決定: ワールドは 1 つ(2D 専用シーングラフを作らない)

**2D ゲーム = 直交カメラ + スプライトコンポーネントの 3D シーン**とする
(Unity/Unreal 型)。Godot 型の 2D/3D 分離ワールドは不採用。理由:

1. 相互変換が要件である以上、ワールドを分けると「変換」が独立機構
   (同期・座標写像・別 ID 空間)になり、両方向とも壊れやすい。
   同一ワールドなら **2D→3D = ただの transform、3D→2D = ただのカメラ**で、
   変換機構そのものが消える
2. 既存資産が全部そのまま効く: ECS/scene v1/物理クエリ/イベント/
   golden/収録リプレイ/devstudio 計画に「2D 版」を複製しなくてよい
3. サブセット原則と整合: 2D プロジェクトは 3D プロジェクトの
  (スプライトと直交カメラしか使わない)部分集合になる

代償(ピクセルパーフェクト・ソート・物理の平面拘束)は §2〜4 で
個別に払う。「2D の使い勝手」は上物(コンポーネント・既定値・
ユーザー空間システム)で作り、エンジン機構は増やさない。

### 0-2. UI 層との関係(描画基盤共有・上物分離の具体化)

| | UI 層(pelican.ui) | 2D ゲーム層 |
|--|--|--|
| 座標 | framebuffer px(screen-space) | **world 単位(world-space)** |
| カメラ | なし(ViewportTransform のみ) | シーンカメラ(主に orthographic) |
| ソート | (layer, decl_seq) 全順序 | §3 のソートポリシー |
| 消費者 | レイアウト・イベント・Controller | ECS コンポーネント・ゲームロジック |

**共有するもの**: K3 atlas(`pelican.atlas` + `#sprite/` 参照)・テクスチャ
ページ・sampler key(nearest/linear)・straight alpha と blend 値・
色 ABI(linear 出力 — `design_color_pipeline.md` が正)・quad 展開の
CPU コード(QuadCommand 相当の POD とインデックス展開)。
**共有しないもの**: 描画パス(UI は canonical anchor `pelican_ui`、
スプライトは post_main より前のシーン内)・ソートキー・座標変換。
= 「バッチャのコードとアセット形式を共有し、パスと順序規則は別」。

## 1. スプライト描画(機構)

### 1-1. SpriteView コンポーネント(scene v1 に additive)

```jsonc
{ "type": "sprite_view",
  "texture": "assets/atlas.json#sprite/hero_idle_0",  // atlas 参照 or 画像直
  "size": [1.0, 1.5],          // world 単位。省略時 = px / ppu(§2)
  "pivot": [0.5, 0.0],         // 0..1、既定 [0.5, 0.5]
  "color": [1,1,1,1],          // 乗算 tint(linear で乗算)
  "flip": [false, false],
  "layer": 0,                  // §3 のソート層(int16)
  "billboard": "none"          // "none" | "y_axis" | "full"(§4-1)
}
```

- スプライト = **object transform で置かれる world-space quad**
  (XY 平面・+Z 法線)。回転・スケール・親子は Transform がそのまま効く
- 実装は専用の sprite パス 1 本(シーン深度と合成 — §3-3)。
  マテリアル梯子とは独立の固定シェーダから始める(B 層フックは
  スプライトには当てない — 必要になったら surface 化を別途設計)
- flipbook(コマ送り)は**エンジン機構にしない**: frame index の差し替えは
  ユーザー空間システム(SpriteView.texture の更新)で書ける。同梱の
  flipbook システム = 特権なし標準ライブラリ(カメラコントローラと同格)

### 1-2. アトラスとアセット

- atlas は K3 の `pelican.atlas` をそのまま使う(UI と同一形式・同一
  ローダ)。`#sprite/Name` フラグメント参照も同一
- 単独画像(png/KTX2)も texture 指定可(内部で 1 sprite の暗黙 atlas 扱い)
- 9-slice・タイルマップは v1 対象外(§6 の将来枠。タイルマップは
  形式から設計する価値があるため別途)

## 2. ピクセルと単位

- **ppu(pixels per unit)**: プロジェクト設定(rendering config)に 1 個。
  `size` 省略時のスプライト寸法 = テクスチャ px / ppu。
  「1 world 単位 = 1m」の 3D 慣習と接続する係数はこれ 1 つだけにする
- **pixel snap はカメラ側のオプション**(orthographic カメラの
  extras): 描画前にカメラ位置を ppu 格子へ丸める。スプライト個別の
  snap は持たない(ソート順や物理と絵がずれる事故のもと)
- ドット絵は sampler nearest(atlas/texture 側の宣言)+ pixel snap の
  組み合わせで成立。フィルタリングは UI と同じ sampler key の語彙

## 3. ソートと透明合成(2D の本丸)

### 3-1. ソートポリシー

alpha blend 前提のスプライトはペインターズ順が正しさそのもの。順序は:

1. **layer(int16)**: 大分類。layer が異なれば必ず layer 順
2. **layer 内 = ソートポリシー**(カメラ or プロジェクト設定で宣言):
   - `"z"`(既定): view 空間 depth の遠→近。3D 混在で自然
   - `"y_down"`: world Y が大きい方が奥(見下ろし 2D の定番)
   - `"declaration"`: 生成順(UI と同じ全順序 — 演出制御用)
3. **決定的タイブレーク**: 同値は entity 生成順(EntityId index)。
   map 走査順・浮動小数の同値に順序を依存させない(UI §1-2 と同じ規律)

### 3-2. バッチとの関係

ソート順を壊す併合はしない(UI §1-2 と同じ「隣接 + 完全キー一致のみ」)。
atlas に寄せてあれば実用上まとまる、で良しとする。

### 3-3. 3D シーンとの合成

- スプライトパスはシーンの depth buffer に対して **depth test ON /
  depth write OFF** で合成(3D ジオメトリには正しく遮蔽され、
  スプライト同士は §3-1 の順序で塗る)
- 完全 2D プロジェクトでは 3D ジオメトリが無いだけで、同じパスが走る
  (2D 専用モードを作らない)
- 半透明 3D との相互ソートは v1 で解かない(既知の一般問題。
  スプライトは「3D 不透明の後・post_main の前」の固定位置)

## 4. 2D⇔3D 相互変換の各論

### 4-1. 2D → 3D(配置)

- スプライトは最初から world オブジェクトなので、3D 空間配置は
  親 Transform を与えるだけ(追加機構なし)
- **billboard オプション**(§1-1)だけ機構で持つ: "y_axis"(ビルボード
  ツリー型)と "full"(パーティクル型)。view 依存のため描画側でしか
  できない — これが 2D→3D で唯一エンジンに足すもの
- 「2D シーンまるごと配置」= scene v1 のサブツリー(スプライト群)を
  親ノード下に置く、という運用(K2 のシーン抽出・フラグメント参照と
  同じ語彙で足りる)

### 4-2. 3D → 2D(投影)

- 直交カメラは実装済み(WP48)。「3D シーンを 2D として遊ぶ/見せる」は
  カメラ定義 + ゲームロジックの移動拘束(ユーザー空間)で成立 —
  エンジン追加なし
- **devstudio の 2D 投影編集**は editor の仕事(D2 の ortho ビュー +
  ピッキング)。エンジン側の前提(ortho カメラ・ID バッファピッキング
  計画・rpc 編集)は既に計画済みで、本書からの追加要求はなし
- **3D を 2D 素材化(render-to-texture スプライト)**: 「3D モデルを
  スプライトとして使う」(奥行きのある看板・ミニビュー)は M3.5 の
  named snapshot(スナップショット RT を screen_inputs で参照)を
  スプライトの texture 源に許すことで成立させる — v1 対象外だが
  形式上の穴だけ確保(texture に `snapshot:` スキームを予約)

### 4-3. 2D 物理

新しい 2D 物理エンジンは作らない。既存 physquery(P1/P2)の上に:

- **平面拘束はユーザー空間**: 2D ゲームの移動・接地はゲームロジックが
  XY(または XZ)平面で書く。collider は既存 box/capsule をそのまま
  使う(厚みのある 3D collider を平面運用)
- raycast/overlap は既存 API で 2D 用途に足りる(平面内レイも 3D レイ)
- 本格 2D 物理(Box2D 級の接触解決)は需要が出たら別トラック
  (シミュレーション自体が未導入 — Jolt 判断と同時に再評価)

## 5. 決定性と検証

- スプライトソートの決定性: §3-1 のタイブレークを fixture 化
  (同 layer・同 z の複数スプライトが 2 回実行で byte 一致)
- golden: ①ortho カメラ + atlas スプライト数枚(layer 跨ぎ・flip・tint)
  ②3D ジオメトリとの遮蔽(depth test)③billboard ④pixel snap on/off
- ppu・ソートポリシーは get_frame_plan / get_status で観測可能に

## 6. 実装順

| 段階 | 内容 | 依存 |
|------|------|------|
| S2D-0 | sprite_view コンポーネント + world quad パス + atlas 接続 + z ソート + golden | K3(済)・WP48 ortho(済)・U1 quad 基盤(済) |
| S2D-1 | ソートポリシー(y_down/declaration)+ ppu/pixel snap + billboard + 同梱 flipbook システム(ユーザー空間 dogfood) | S2D-0 |
| S2D-2 | 2D ゲーム example(見下ろし or サイドスクローラー 1 面 — 入力/物理クエリ/イベントの実証) | S2D-1 |
| 将来 | タイルマップ(形式設計から)・9-slice・snapshot テクスチャ・半透明相互ソート | — |

## 7. 未決事項

1. スプライトの世界平面の既定(XY+Z 法線 vs XZ+Y 法線)— 見下ろし 2D を
   XZ で作るか「XY + カメラを上から」で作るか。**v1 は XY 固定 +
   カメラで解く**を仮置き(要ユーザー確認)
2. layer と 3D レンダリングパス(マテリアル pass キー)の関係 —
   スプライトは単一パス内ソートで完結させる仮置き
3. タイルマップ形式(チャンク・衝突・オートタイル)— 別文書
4. スプライトへのライティング(normal 付きスプライト)— 需要が出たら
   surface 化(B 層)を検討
