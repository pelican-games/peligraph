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

`trigger=true` は検知専用であり、transform・速度その他への物理応答を
一切行わない。raycast / overlap / shapeCast は既定で trigger も通常の hit
として返し、hit の `metadata.trigger` で識別できる。除外したい問い合わせは
共通 `QueryFilter::include_triggers=false` を明示する。この可視性規則は
trigger event の発生有無とは独立である。

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

2026-09-07現在: 静的meshのscene/import/reloadとraycast、Sphere/Box/Capsule overlapを実装済み。
Box/Capsule overlapは回転した形状と変換後の三角形表面を判定し、接触も含む。閉じたmeshを充填体とは扱わない。
既存のoverlap公開serviceを使用し、ProviderV3には`query_mesh_box_overlap_all`と
`mesh_box_overlap_all`を追加する。入力geometryはcallback中のみ有効な借用データで、
選択中providerがBox mesh判定に未対応なら`unavailable`を返す。
CapsuleはY軸に沿った半長`half_height`の線分と半径`radius`で表し、rotationで回転する。
軸線と三角形表面の距離で判定し、非一様scale等はmesh側の三角形へ適用するため半径はworld単位を保つ。
ProviderV3の`query_mesh_capsule_overlap_all`と`mesh_capsule_overlap_all`を使用する。
Sphere sweepは既存Shape Cast公開serviceへ接続。面・有限辺・頂点との接触時刻を解析的に求め、
world単位の半径、両面、BVH絞り込み、非一様scale・反転・shearを扱う。
ProviderV3の`query_mesh_sphere_shape_cast_all`と`mesh_sphere_shape_cast_all`を使用する。
TOIはdeltaに対する割合、positionはmesh表面、normalは球を表面から離す向き。
開始時の接触は既存epsilon内で接近中だけTOIゼロとなり、静止・離反中はhitを返さない。
epsilonを超える初期貫通はbuiltinでは`unavailable`。単一三角形の押し戻しを非凸mesh全体のMTDとして返さない。
未対応時はprimitiveだけの部分結果も返さない。C++の従来shapeCastAll wrapperはunavailableを空結果へ変換するため、
状態の判別にはqueryShapeCastAllまたは公開serviceを使う。ProviderV3利用DLLは現行headerで再ビルドする。
Box sweepも同じ公開serviceへ接続。回転は移動中固定し、箱3軸・三角形法線・辺の外積9軸の
投影区間が重なる時間帯から最初の接触を求める。衝突時の箱で三角形をclipし、mesh表面上の接触位置を返す。
ProviderV3の`query_mesh_box_shape_cast_all`と`mesh_box_shape_cast_all`を使用する。
Sphereと同じ初期接触・初期貫通の制約を持ち、Boxの半寸法はworld単位を保つ。
Capsule sweepも公開serviceへ接続。回転固定の軸線を三角形側へ移したprismと球のSweepへ変換し、
面・辺・頂点の解析式をSphereと共有する。半長ゼロはSphereと同じ経路を使う。
prism面の接触点は重心座標で元の三角形上へ戻す。軸中央での接触も扱い、時間の離散サンプリングは使わない。
ProviderV3の`query_mesh_capsule_shape_cast_all`と`mesh_capsule_shape_cast_all`を使用する。
prism内部にある初期軸交差は、境界面から離れていてもunavailableにする。半径ゼロの境界接触は外向き法線で
接近・離反を区別し、接線方向の初期移動が後続面のhitとして返ることも防ぐ。
mesh全体の初期貫通MTD・mesh triggerは未実装。

2026-09-06: RPC `raycast`を既存PhysWorldへ接続。paramsは`origin`・`direction`（各3数値）と`max_distance`（有限・非負）。方向は正規化する。結果は`hit: null`またはname/distance/position/normal/collider_id/shape_ordinal/trigger/one_wayを持つhit。現在のtransformを問い合わせ、raycast既定のlayer/mask・trigger/one-way包含規則を使用する。P3のメッシュ/BVH/glb extras規約の完了を意味しない。

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
