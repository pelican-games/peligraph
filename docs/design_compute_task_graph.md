# コンピュートタスクグラフ: 宣言的依存とスケジュール最適化

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-04。レビュー前)。
前提: [SF](実装済み)、`design_render_feature_modules.md`(v1 ドラフト)、
ロードマップ §3 の compute パス予約枠。GPU 計測(WP29 候補)と強く連携。

## 0. 要求(2026-07-04 ユーザー方針)

レンダーパスグラフと「同様ではないが同じ思想」で、コンピュートタスクを扱いたい:

1. タスクは自作シェーダーと同様にアセットとして書ける(パージ可能・差し替え可能)
2. **実行順序・バリア・スケジュールはユーザーが手書きせず、エンジンが最適化する**

レンダーパスとの違い(「同様ではない」の中身):

| | レンダーパス | コンピュートタスク |
|---|---|---|
| 実行頻度 | 毎フレーム・presentation に直結 | 毎フレーム(パーティクル)/ 要求時(ベイク)/ 複数フレーム非同期(将来) |
| 順序の源 | 配列順(作者が書いた順が正) | **reads/writes 宣言からエンジンが導出**(作者は順序を書かない) |
| リソース | render_targets(画像) | 画像 + **バッファ**(SSBO) |

## 1. 宣言形式(v1)

rendering config と同じファイルに新セクション。リソース名前空間は
render_targets と**共有**(compute の出力をパスが `input` で読める・逆も可)。

```json
{
  "buffers": [
    {"name": "particle_state", "size_bytes": 1048576, "usage": ["STORAGE", "VERTEX"], "lifetime": "persistent"}
  ],
  "compute_tasks": [
    {
      "name": "particle_sim",
      "shader": "shaders/particle_sim",
      "reads":  ["particle_state"],
      "writes": ["particle_state"],
      "dispatch": {"groups_from": "particle_count", "local_size": 64},
      "schedule": "per_frame"
    },
    {
      "name": "particle_sort",
      "shader": "engine://particle_sort",
      "reads":  ["particle_state"],
      "writes": ["particle_order"],
      "schedule": "per_frame"
    }
  ]
}
```

規則:

1. **順序は書かない**。`reads` / `writes` の宣言が唯一の依存情報で、
   エンジンが DAG を構築する。配列順に意味を持たせない(将来の最適化の自由度を
   作者との契約で確保する — ここがレンダーパスとの最大の違い)
2. 同一リソースへの writes-writes で依存が曖昧な場合(2 タスクが同じものを書き、
   read で繋がっていない)は **hard error**(順序が定義できない構成を許さない。
   意図的な累積書き込みは v1 では単一タスクに書くか、中間バッファで直列化する)
3. shader は stem 規約(`.comp` / web は `cs_main`)。engine:// / プロジェクトの
   両方から参照でき、feature fragment も compute_tasks を追加できる
   (`design_render_feature_modules.md` の合成に `compute_tasks` を足すだけ)
4. `lifetime`: `persistent`(フレームを跨いで保持 — シミュレーション状態)/
   `transient`(フレーム内のみ — 将来エイリアシング最適化の対象)
5. `dispatch.groups_from`: 固定値 or CPU から注入される名前付きパラメータ
   (パーティクル数など実行時可変の値。コマンド層 / ゲームロジックから設定)

## 2. スケジューラ(v1 の最適化と v2 の拡張)

### v1 — 単一キュー内の自動化(まずここまで)

1. **トポロジカルソート**: reads/writes から DAG → 実行レベル(互いに依存しない
   タスクの集合)に層別
2. **バリア自動挿入 + 融合**: レベル境界で必要な `vkCmdPipelineBarrier` を
   エンジンが計算。同一レベル内のタスク間はバリアなし、複数リソースの遷移は
   1 回のバリアに束ねる(手書きバリアの禁止 = 正しさの根拠を一元化)
3. **graphics との接続**: フレーム内の実行位置は
   `schedule: "per_frame"` → 「パスグラフの前」を既定とし、パスの `input` に
   compute の writes が現れたら compute→fragment のバリアも自動。
   パス出力を compute が読む(ポストプロセス解析等)は「パスグラフの後」に配置
4. **GPU 計測との統合**: タスク単位の timestamp を WP29 の機構にそのまま乗せる
   (スケジュール最適化は計測とセットでないと評価できない)

### v2 — 予告のみ(v1 で設計を壊さない範囲で言及)

- **非同期 compute キュー**: pickQueues は現状 graphics+present のみ。
  専用 compute キューの発見・タイムラインセマフォ同期・所有権移譲を導入し、
  graphics と依存のないタスク群をオーバーラップさせる。
  v1 の「順序を書かせない」契約がここで効く(作者の書き順に意味がないので、
  エンジンはキュー分割を自由にできる)
- **フレーム跨ぎ最適化**: persistent バッファの ping-pong 自動化、
  1 フレーム遅延を許すタスクの宣言(`latency: 1`)
- **on_demand スケジュール**: IBL ベイクのような単発実行(コマンド層 /
  ゲームロジックからトリガ、fence で完了追跡)

## 3. パージ可能性・web との関係

- `buffers` / `compute_tasks` を書かなければ何も起きない(feature modules と
  同じ素通り原則)。compute 対応 feature(GPU パーティクル等)は fragment 経由
- web(WebGPU は compute 対応): サブセット原則どおり pelican 先行。
  未知の compute_tasks を含む config は web で **hard error**
  (features と同じ意味論 — 絵や挙動が黙って変わるのを防ぐ)

## 4. 実装順(WP 候補・番号は登録時に確定)

| 段階 | 内容 | 依存 |
|------|------|------|
| C1 | 形式パーサ + DAG 構築 + writes-writes 検証(**純ロジック**、GPU 不要テスト。トポロジカル順とバリア計画を値として返す = スケジューラの単体テスト可能性) | 18 |
| C2 | 実行系: バッファ確保・compute パイプライン(PipelineFactory 拡張)・バリア発行・パスグラフ接続。golden で「compute なし構成の挙動不変」 | C1, 13 |
| C3 | 実証 feature: GPU パーティクル最小(sim + 描画接続)。WP22(pointcache)の再生側と合流させると一石二鳥 | C2, WP28 |

C1 を純ロジックで切るのが要点: 「エンジンが導出した実行計画」がデータとして
取り出せるので、スケジュール最適化(バリア数・レベル数)を golden ならぬ
**プラン比較テスト**で回帰保護できる。

## 5. 未決事項

1. `dispatch.groups_from` の名前付きパラメータ注入の正確な API
   (コマンド層 stage 3 / ゲームロジック設計と同時に確定)
2. indirect dispatch(GPU 駆動のディスパッチ数)— v1 は CPU 指定のみ
3. readback(compute 結果の CPU 回収)— WP6 の readback 流用で `capture` 類似の
   rpc メソッドにするか。需要が出てから
4. 非同期キューの導入時期(v2)— GPU 計測で graphics が飽和と分かってから
