# アニメーショングラフ(v1)

対象読者: エンジン担当・キャラクターを動かす人。
ステータス: v1 ドラフト(2026-07-12。codex 敵対レビュー前)。
前提: WP38(クリップ再生 v1 — `graph` キーは予約済み)、
「feature 層 = ユーザー空間」方針(2026-07-12 — 本設計はそのアニメ適用)、
G1a/G2(ユーザーシステム + DLL ホットリロード)、EngineTime 決定性。

## 0. 帰属の切り分け(本設計の中核判断)

カメラ(コントローラ = ユーザー空間)と同じ速度分離を適用する:

| 層 | 内容 | 帰属 |
|----|------|------|
| **ポーズ機構語彙** | クリップサンプリング(clip, t → Pose)・ポーズブレンド(重み付き N 本、per-joint pos/scale = lerp・rotation = shortest-path slerp)・palette 提出 | **エンジン**(公開 API・版付き・決定的) |
| **グラフアセット** | `pelican.anim_graph` v1(states / transitions / blend1d)| **データ形式**(エンジンが凍結規律で所有) |
| **グラフ評価器** | アセットを解釈して 機構語彙を呼ぶ AnimGraphSystem | **特権なしの標準ライブラリ**(公開ポーズ API のみで実装 — dogfooding。ユーザーは評価器ごと自作/改造してよい) |

帰結: 変わったブレンド(物理ベース・プロシージャル・IK 前段)が必要に
なったら、アセットを捨てて**ユーザーシステムでポーズ API を直接呼ぶ**
自由が常にある。G2 により保存 → F5 → 数秒で手触りが変わる。

## 1. 公開ポーズ API(機構語彙 — userpublic)

```cpp
// すべて決定的・EngineTime 駆動。SkeletalModelData(WP38)の上に立つ
Pose      samplePose(ClipHandle clip, double time, bool loop);
void      blendPoses(std::span<const Pose*>, std::span<const float> weights,
                     Pose& out);           // 重みは正規化を要求(検証)
void      setPalette(GameObjectId, const Pose&);  // フレーム境界で GPU へ
```

- `Pose` = joint ローカル TRS 配列(skin の joint 順)。palette 化
  (world 積・inverseBind 積)はエンジン内部(WP38 の既存経路)
- 骨数不一致の blend は名前入りエラー(推測リターゲットは v1 でしない)

## 2. `pelican.anim_graph` v1(データ形式)

```json
{ "schema": "pelican.anim_graph", "version": 1,
  "parameters": [
    { "name": "speed", "type": "float", "default": 0.0 },
    { "name": "grounded", "type": "bool", "default": true } ],
  "states": [
    { "name": "idle", "clip": "character.glb#animation/Idle", "loop": true },
    { "name": "move", "loop": true,
      "blend1d": { "param": "speed",
        "points": [ { "value": 0.0, "clip": "...#animation/Walk" },
                    { "value": 6.0, "clip": "...#animation/Run" } ] } } ],
  "initial": "idle",
  "transitions": [
    { "from": "idle", "to": "move",
      "when": { "param": "speed", "op": ">", "value": 0.1 },
      "duration": 0.15 },
    { "from": "move", "to": "idle",
      "when": { "param": "speed", "op": "<=", "value": 0.1 },
      "duration": 0.2 } ] }
```

- **states**: `clip`(単クリップ)XOR `blend1d`(1 パラメータの線形
  ブレンド空間 — 点は value 昇順・重複エラー・範囲外は端で clamp)
- **transitions**: 条件は「1 パラメータ + 比較演算子」の単純形のみ
  (`> >= < <= == !=`、bool は `==`)。**複合条件・式言語は v1 で持たない**
  (複雑な遷移ロジックはゲームコードで param を組み立てる — 表現力は
  ユーザー空間へ逃がす)。`duration` = クロスフェード秒(0 = カット)
- クロスフェード中の再遷移: v1 は**現在の遷移完了まで新遷移を評価しない**
  (割込みフェードは v2 予約 — 状態爆発を避ける)
- **v2 予約キー**(存在 = エラー): `layers`(部分骨マスク)、`events`
  (フットステップ等のクリップマーカー → E1)、`graphs`(ネスト)、
  遷移の `interrupt`
- 検証: 未知 state 参照 / 到達不能 state / 未宣言 param / blend1d の
  骨集合不一致 — すべて名前入りロードエラー

## 3. 接続

- **コンポーネント**(WP38 の予約を実体化):
  `{"name": "animation", "graph": "anim/character.graph.json"}` —
  `clip` キーとは XOR(併存エラー)
- **パラメータ設定**: `GameContext::setAnimParam(object, name, value)`。
  未宣言 param・型不一致は名前入りエラー。適用はそのフレームの
  グラフ評価前(system 実行順で決定的 — AnimGraphSystem は
  ユーザーシステムの後に走る規約)
- rpc: `set_anim_param`(inject 系と同じ流儀)— デバッグ・リプレイ用。
  グラフ状態は `get_status.animation`(object → state / 遷移進行度)
- ホットリロード: グラフアセットは H2 の対象(§design_asset_hot_reload —
  reload = 初期 state から再開。状態保持は欲張らない)

## 4. 決定性

- 遷移条件の評価はフレーム境界・EngineTime 固定 step で決定的
- クロスフェード重み = 遷移経過時間 / duration(binary64 → float、
  丸め規範は palette 計算と同じ)
- 収録/リプレイ(WP89)下で byte 一致(param 設定が InputEvent 由来なら
  自動、ゲームロジック由来なら同一 tick 評価で一致)

## 5. テスト戦略

- 解析 fixture: 2 骨モデルで idle→move 遷移、フェード中点(t = duration/2)
  のポーズ = 手計算の 50% blend と一致
- blend1d: param = 中間値での重み・端 clamp・点重複エラー
- 決定性: 同一 param 列 → 2 回実行で palette byte 一致
- golden: クロスフェード中点の固定ポーズ 1 ケース
- negative: v2 予約キー・未知 state・骨不一致・XOR 違反

## 6. WP 分割

| 段階 | 内容 | 依存 |
|------|------|------|
| A1 | 公開ポーズ API(sample/blend/setPalette)+ 解析 fixture | WP38(済) |
| A2 | anim_graph v1 パーサ + AnimGraphSystem(標準ライブラリ)+ component/GameContext/rpc + golden | A1 |

## 7. 未決事項

1. `Pose` の所有(毎フレーム alloc を避ける pool — A1 実装判断)
2. ルートモーション(clip の root 移動を transform に反映)— v2 で
   別トラック(2D/3D 移動系と絡む)
3. リターゲット(骨集合の異なるクリップ適用)— 明確に将来
