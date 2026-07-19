# イベント層(v1.1 — ユーザーレビュー承認済み)

対象読者: エンジン担当・ゲームロジックを書く人。
ステータス: **v1.1(2026-07-19)— ユーザーレビュー完了・承認**:

1. **フレーム境界配送(1 フレーム遅延)を恒久仕様として承認**
   (条件 = 遅延最小限: 配送は次フレーム先頭 = 全 update 前で、
   反応は常にちょうど 1 フレーム後に描画される — 境界方式の理論下限
   であることを確認済み。同一フレーム因果は直接呼び出し/クエリ、
   多段連鎖は 1 イベントへ畳む作法をマニュアルに明記する)
2. **API 書き味を恒久仕様として承認**(struct 宣言 +
   `PELICAN_REGISTER_EVENT`・購読 = typed onEvent overload・payload =
   値コピー小型、参照/ポインタ禁止・対象は EntityId で運ぶ)
3. **E2(物理トリガー)GO — Enter/Exit のみ**(OverlapStay は需要が
   出たら additive)

v1 の未決事項の解決: ①購読解除 → WP155/162/163 の owner purge/token
台帳で解決済み ③inject_event → WP56 で実装済み ②イベント収録
(JSONL)→ 需要が出るまで保留のまま。
前提: `design_game_logic_native.md`(システム様式)、
`design_physics_queries.md`(トリガーの発生源)、決定性方針。

## 0. 目的と非目的

システム間・エンジン→ゲームの疎結合通信。物理トリガー(触れた/離れた)、
シーンイベント(loaded)、アニメーションイベント(将来)、ゲーム独自
イベントがすべてこの上に載る。

非目的: 入力(Actions が既にイベント相当を持つ — 重ねない)、
rpc(別プロトコル)、レンダラ内部の通知。

## 1. 決定性配送(設計の核)

- **emit はフレーム中いつでも、配送は次フレーム頭**(全システムの update 前)。
  同一フレーム内の即時配送はしない — システムの実行順に結果が依存する
  非決定性の芽を最初から絶つ
- 配送順 = emit 順(安定)。リプレイ(input_seq + seed + fixed step)で
  イベント列も完全再現される
- 1 フレーム遅延は仕様(60fps で 16ms)。「今すぐ知りたい」ものは
  クエリ API(raycast 等)を使う — イベントは通知、クエリは質問

## 2. API(v1 案)

```cpp
// 定義: ゲーム/エンジンが POD struct を宣言し名前で登録
struct DoorOpened { std::string door_name; };
PELICAN_REGISTER_EVENT(DoorOpened);

// 発行(GameContext)
ctx.emit(DoorOpened{"gate_a"});

// 購読(システムのメンバ — update と同じ様式で受ける)
void onEvent(const DoorOpened &e, GameContext &ctx);
```

- 型安全(struct)を基本にし、名前は登録マクロが自動化
  (registerComponent の黒魔術と同じ系譜 — 実装様式を揃える)
- ペイロードは値コピーで小さく(参照・ポインタ禁止 — 1 フレーム跨ぐため)

## 3. エンジン発行イベント(v1 で入れる分)

| イベント | 発生源 |
|---------|--------|
| SceneLoaded { scene_name } | シーン切替(`design_scene_flow.md` S1/S2) |
| OverlapEnter / OverlapExit { self, other } | PhysWorld の overlap 差分(トリガー collider — E2) |

## 4. 実装順(WP 候補)

| 段階 | 内容 |
|------|------|
| E1 | バス(登録・emit・フレーム境界配送)+ SceneLoaded + ゲーム emit/購読 + 純ロジックテスト(配送順・遅延・リエントラント emit) |
| E2 | 物理トリガー(collider に `trigger: true` + PhysWorld 差分検出 + Enter/Exit) |

## 5. 未決事項

1. 購読の解除(システム死亡時)— v1 はシステム = 静的登録なので不要か
2. イベントの record(input_seq と同様の JSONL 収録 — デバッグ用)は
   需要が出てから
3. rpc からの emit(テスト用 `inject_event`)を E1 に含めるか
   (推奨: 含める — inject_input と同じ価値)
