# ECS とゲームロジックを graph にどう結ぶか: component を共有メモリにする案(議論メモ)

対象読者: エンジン担当(ECS / userpublic / graph)、Studio 担当。
ステータス: **議論メモ(2026-10-23)。決定事項ではない。** あとで見直す。WP 化・レビュー前。
前提: マニュアル [第4章 §4.2 / §4.4](manual/04_scene_ecs.md)、[第8章 §8.4 / §8.5 / §8.6 / §8.13](manual/08_gameplay.md)、
[design_ecs_lifecycle.md](design_ecs_lifecycle.md)、[design_animation_graph.md](design_animation_graph.md)(AG-1b の撤回理由)、
[design_audio_timing_and_matching.md](design_audio_timing_and_matching.md)(ゲーム状態 → 音)、
[design_render_stage_as_canvas_filter.md](design_render_stage_as_canvas_filter.md)(`Show:` の領域フィルター)、
[v1 開発計画](v1_development_plan.md)「一つのグラフモデルは意味の共通化であり、一枚のキャンバスではない」。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- 決まっていないのは「ECS と graph のどちらが正で、どちらが相手を読み書きするか」。今は game code が `setValueGraphParameter` で
  graph に値を押し込む片方向だけで、graph が ECS の状態を読む node も、graph が component を書く sink(pose / ui / audio 以外)も無い。
- 案: **component を共有メモリにし、graph は型付きの port で component の field を読み書きする**(`pelican.ecs` module: `ecs.read` / `ecs.write` /
  `ecs.event` / `ecs.emit`)。読みは frame 先頭の snapshot、書きとイベントは次 frame の先頭に可視(イベント層の frame 境界配送と同じ規則)。
  (entity, component.field) ごとに書き手は 1 つ、という**書き手の登録簿**を `pose.sink_conflict@1` / `ui.sink_conflict@1` から一般化する。
- 同一 object の component 間に Unity の `SendMessage` 型の通信は無く、足さない。結合は静的な要求(`applies_to` / `required_for`)、
  system 経由のデータ共有、owner 付き graph、イベントの 4 種で、それを**関係の view**として canvas に出す。component の管理(属性の編集)は縦並びの Inspector のまま。

## 1. 今の結び方

| 向き | ある | 無い |
|---|---|---|
| game code → graph | `render().setValueGraphParameter(object, parameter, value)`(owner 付き `graph.instance` の `value.parameter` を上書き。edge が供給する port は `graph.override_shadowed@1`)、`emitValueGraphEvent`(`event.playback` へ)、`setRenderValues(buffer)`。animation は別口 `animation().setParameter / forceState` | 宣言だけで結ぶ手段 |
| graph の入力 | `value.input` は `time` / `delta_time` / `scene_time`([`graphmodule.cpp:930`](../src/project/graphmodule.cpp#L930))。`value.morph_weight` だけが owner の model を読む | component の field を読む node |
| graph → ECS | `pose.skeletal`、`ui.value` / `ui.visible`、`audio.*`、`render.scalar`。sink の競合は `pose.sink_conflict@1` / `ui.sink_conflict@1` | component の field を書く sink、イベントを出す sink、game code が graph の値を読む API |
| 姿勢 | 装着様式が 2 つ(`animation` component と `graph.instance` + `pose.*`)。AG-1b は「派生プログラムの置き場」と「評価器の状態を写す API」が無いとして一本化を v0 で撤回 | その 2 つ |
| 型 | value 領域は `pelican.value.scalar@1` のみ | vector / bool |
| 同一 object の component 間 | 静的な要求(`pelican_ray_instance` は `simplemodelview` が無いと `ray_instance_not_ray_traced@1`、field 表の `applies_to` / `required_for`)、エンジン内部 system のデータ共有(`localtransform` → `transform`、animation → 骨)、owner 付き graph、イベント | 隣の component を名指しで呼ぶ口(behavior は `self()` と自分の `params`、`GameObjects` の限られた読み書きだけ。ユーザー定義 component を隣から読む口は未確認) |

## 2. 案: component を共有メモリにする

選択肢は 3 つ。(B) 今のまま code が橋渡し、(C) graph をゲームロジックまで広げる(ビジュアルスクリプト)、(A) ECS の component を正にし、graph は読む node と書く sink を持つ。
(C) は決定性と C++ hot reload という強みに逆らう。(A) は `pelican_ray_instance`(component → 描画側の表)と `pose.skeletal`(graph → 骨)で片側ずつ既にやっている形。

```text
pelican.ecs module(module の登録契約に乗る。catalog に載る)
  ecs.read   {component, field, object?}   owner(省略時)か名前付き object の field を frame 先頭の snapshot で読む
  ecs.write  {component, field}            sink。graph の値を component の field に書く(可視になるのは次 frame の先頭)
  ecs.event  {event, field?}               配送された typed event を trigger(pulse)にする → value.trigger_clock へ
  ecs.emit   {event}                       sink。port の値を pelican_payload に詰めて emit(配送は次 frame の先頭)
```

- **順序と決定性**: 読みは frame 先頭の snapshot、書きとイベントは次 frame の先頭。system の実行順に依存しない(イベント層と同じ規則)。
- **書き手の登録簿**: (entity, component.field) ごとに書き手は 1 つ(宣言 component、system / behavior、graph sink のいずれか)。publish 時とシーンロード時に名前付きエラー。
  code からの `setValueGraphParameter` は「明示の上書き」の層として残す。
- **型**: value 領域に `vector2/3/4` と `bool` を足す(位置を読むのに要る)。feature GUI の catalog の kind と同じ語彙。
- **姿勢の一本化(v1)**: `animation` component を「`engine://graphs/animation_*.json` を owner 付きで instance 化する糖衣」にする。AG-1b が欠いた 2 つは、
  (1) 派生プログラムの置き場 = component が graph 文書を参照する形(`pelican_ray_instance` が AS 宣言を参照するのと同じ)、
  (2) 評価器の状態を写す API = graph instance の state port(保存と hot reload に要る)。これで `animation().setParameter` は `setValueGraphParameter` の別名。
- **隣の component の読み書き(code 側)**: `BehaviorContext` と system に `self()` の component を型で読む口と、登録簿を通した書き口。graph の `ecs.read` / `ecs.write` と同じ規則。
  `SendMessage` 型の呼び出しは足さない(実行順依存の非決定性の入口)。
- **静的な要求の明文化**: `applies_to` / `required_for` を全 component の schema の一部にし、Inspector に出す。

## 3. 関係の view(node で見せるもの)

component の**管理**(属性の編集)は縦並びの Inspector が速く、node にしても object の中に線は少ない。価値があるのは object をまたぐ**関係**で、
v1 計画の「制作対象 / 関係 / 時間 / 実行結果の view」のうち関係の view にあたる。

| 見たいもの | 縦並びで | node で |
|---|---|---|
| どの component.field を誰が書くか(登録簿)。`sink_conflict` の原因 | 各 object を開いて推測 | field が port、書き手から線。衝突は赤 |
| `ecs.read` / `ecs.write` の binding(HP → vignette、速度 → 音量) | Inspector の bindings 節に文字で | object の component port から graph instance へ線 |
| イベントの流れ(物理 trigger → event → system → graph → audio) | コードを読む | event が node、購読者へ線。frame 境界の遅れが段で見える |
| object 間の参照(camera の target、trigger の相手、親子) | field に名前 | 線 |
| object の中の要求と読み(`required_for` / `applies_to`、system の読み書き) | 無し | component 同士の線 |
| 再利用の単位(prefab 相当) | 別機能 | graph の `definitions` + `graph.instance` が既にそれ。component の束も definition にできる |

- Graph canvas に `scene`(または `ecs`)領域を足し、`Show:` の領域フィルターに乗せる。object が node、component.field が port。線は 書き手 / binding / イベント / 要求 の 4 種。
- 編集の分担: Inspector が field の値の正。canvas は線(binding と参照)だけを編集する(Frame Plan と Graph の分担と同じ)。`Values from:` で object を選ぶと component の値と graph の値が同じ canvas に重なる。
- 実装は canvas カーネル([design_studio_node_canvas_kernel.md](design_studio_node_canvas_kernel.md))の上に領域として足す。別の canvas を増やさない。
- 注意: Outliner(階層)/ Inspector(属性)/ canvas(関係)の 3 つで同じ正本を見るので、どれで何を編集するかを先に決める。関係が少ない scene では canvas は空に近く、価値が出るのは `ecs.*` と登録簿の後。

## 4. 順序(案)

1. 型の追加(vector / bool)と `ecs.read`。「code 無しで速度を音量に、HP を vignette に」が書ける。
2. 書き手の登録簿の一般化(publish 時とシーンロード時の名前付きエラー)。隣の component の型付き読み書きも同じ規則で。
3. `ecs.write` と `ecs.event` / `ecs.emit`(frame 境界の規則)。
4. `animation` component を graph の糖衣に(v1。姿勢の一本化)。
5. Inspector の bindings 節と `applies_to` / `required_for` の表示。
6. canvas の `scene` 領域(関係の view)。

## 5. 未決(全部)

- (A) を採るか。採るなら `ecs.write` の対象を「表示系の component だけ」に限るか、全 component に開くか(ゲームロジックの一部が graph に移る境界)。
- 書き手の登録簿の粒度(component 単位か field 単位か)と、system の書き手宣言の形(`PELICAN_REGISTER_SYSTEM` に writes の列挙を足すか)。
- `ecs.read` の snapshot を frame 先頭に取るコスト(読む field だけを写すか、component 全体か)。
- 値の型の追加が catalog の kind、feature GUI の schema、`get_graph_values` に及ぶ範囲。
- 姿勢の一本化で `animation` component の 7 キー(`clip` XOR `graph` …)をどう graph 文書に写すか。
- 関係の view の最初の対象(登録簿の衝突表示だけから始めるか)。
