# 物理クエリ: raycast / overlap / shapeCast(v2)

対象読者: エンジン担当。
ステータス: **v2 実装済み(P1/P2 + WP107、2026-07-15)**。
前提: `design_game_logic_native.md`(GameContext = 消費者)、
`design_devstudio_direction.md` D2(エディタピッキング = 消費者)、
`design_asset_format_policy.md` §4(コリジョン形状の glb 内規約・予約)。

## 0. スコープ(クエリ先行。シミュレーションは将来トラック)

(2026-07-07 ユーザー訂正を反映)**ランタイム物理シミュレーションは
「やらない」のではなく「後」**。使い分け:

- 映像・VFX レーン(破壊・布・流体)= Houdini ベイク + transform_seq / VAT(現行)
- **ゲームプレイの動的物理(剛体・キャラクター操作)= 将来のエンジン
  ランタイムシミュレーション**(§6 に展望)

クエリ先行にするのは順序の判断(エディタ D2 ピッキングと
ゲームロジックの必須要件が先に来る)。形状定義・コライダーコンポーネント・
クエリワールドは**将来のシミュレーションの土台**として設計する
(broadphase・形状表現をシミュ導入時に捨てない)。現在提供するのは問い合わせ:

- `raycast(origin, dir, max_dist)` → 最初のヒット(object 名・距離・位置・法線)
- `overlap(shape, transform)` → 重なっている object 名の列
- `shapeCast(shape, delta, filter)` → ordered all-hit / closest、TOI、
  collider 上 position、押し出し normal、initial-overlap MTD
- 共通 filter = reciprocal layer/mask、self/ignore、trigger/one-way inclusion。
  結果 identity = stable ColliderId + full EntityId + shape ordinal

## 1. コリジョン形状の宣言

1. **形状はコンポーネント**(scene v1 の文法どおり):
   `{"name": "collider", "shape": "box|sphere|capsule", ...寸法}` — 既存の
   `components/collider.{hpp,cpp}` を正本とする。query metadata の既定は
   `layer=1`、`mask=0xffffffff`、`trigger=false`、`one_way=false`
2. **メッシュコライダは glb 内規約**(フォーマット方針 §4 の予約を確定):
   ノード extras `pelican.collision`(`{"shape": "mesh"|"convex"}`)。
   v1 は AABB / 球 / カプセル + 静的メッシュ(BVH)まで
3. 休眠中の `collision` ブランチは**部品取り**として再評価(そのままマージしない。
   現行本線との乖離が大きいため、使える純ロジックだけ移植)

shapeCast の contact epsilon と TOI/MTD tie epsilon はともに `1e-5`。
同深度 MTD は移動逆向きを優先し、その後 world x/y/z で固定する。
all-hit は TOI bucket 後に ColliderId/full EntityId/shape ordinal/name の全順序。
方向付き one-way の通過/接地判断は、この metadata と ordered all-hit を使う
S2D-2 のユーザー空間 policy であり、provider 内へ固定しない。

## 2. 実装構造(既存の型に従う)

- **純ロジック**: `src/project/…` ではなく `core/phys/` に
  `physquery.{hpp,cpp}`(形状・レイ・BVH — モジュール/GPU 非依存、
  GPU 不要テスト。決定性: 同一シーン + 同一 query → 同一 ordered result)
- **バインダ**: シーンロード時に collider コンポーネント → クエリワールドへ登録。
  transform 変更(ゲームコード / update_transforms)に追従
- **消費者 API**: GameContext に `raycast` / `overlap` / `shapeCast`
  (ゲームコード)、
  rpc に `raycast` メソッド(エディタ D2 / エージェント用 — stage 3 の続き)
- **provider 境界**: 公開 C ABI は V1 を維持し、V2 で shapeCast capability
  だけを additive 追加。Builtin/Jolt/game DLL provider は同じ service へ
  capability 単位で合成され、Jolt 型を engine/game header へ漏らさない
- **パージ**: `PELICAN_WITH_PHYSICS=OFF` では service stub だけ、provider-only
  では Builtin/Jolt narrow phase をリンクしない。collider のない scene では
  query world も空のまま(素通り原則)

## 3. 検証

- 純ロジック: 形状×レイ、shapeCast 全 9 shape pair、thin collider、MTD、
  同時 hit/filter 継続 fixture(GPU 不要)
- 結合: rpc raycast をシナリオテストに(load_gltf → raycast → 期待ヒット)
- デバッグ描画: collider の可視化を debug_draw feature に追加(安い・効果大)

## 4. 実装順(WP 候補)

P1: 純ロジック(box/sphere/capsule + レイ、決定性テスト、済) →
P2: collider バインダ + GameContext API + debug 可視化(済) →
S2D-P/WP107: shapeCast/MTD/filter + Provider ABI V2(済) →
P3: rpc raycast + メッシュ(BVH)+ glb extras 規約。

## 5. 未決事項

1. 動的 BVH 更新の頻度(v2 は毎フレーム再収集で開始し、計測で判断)
2. rpc query・メッシュ/BVH・glb extras の P3
3. **ランタイムシミュレーションの実装方針: 自前 vs 物理ライブラリ採用** —
   シミュ着手時の最初の意思決定。**推奨 = Jolt(2026-07-07 比較検討済み)**:
   決定性モードを機能として持つ(本エンジンのリプレイ/rpc 決定性と整合)・
   現役保守・モダン C++・並列前提設計・キャラクターコントローラ同梱。
   Bullet は不採用方針(実質保守停止・決定性非保証・唯一の優位である
   ランタイムソフトボディは Houdini ベイク/VAT レーンが担当済み)。
   2D 物理は Box2D を別枠予約(2D 設計文書のスコープ判断)。
   Jolt query provider は WP107 で実装済みだが、剛体 world/step/constraint を
   持つシミュレーションは未実装。採用の場合もコライダーコンポーネント /
   glb extras 規約 /
   クエリ API は形式として維持し、バックエンド差し替えで受ける
   (PELICAN_WITH_PHYSICS ユニット必須 — 誕生時要件)

## 6. シミュレーションへの展望(方向のみ)

- 決定性との関係が最大の設計論点(固定タイムステップ・リプレイ・rpc 決定性
  テストを壊さないソルバ運用)。シミュ設計文書はここから書き始めること
- ベイク(Houdini)とランタイムの併存: 同じシーンで「ベイク再生オブジェクト」と
  「ランタイム剛体」が共存できる形式(コンポーネントの種別で区別)を保つ
