# カメラシステム: glTF 同等以上(v1)

対象読者: エンジン担当。
ステータス: v1.1 ドラフト(2026-08-05。WP265 の runtime free camera を追記)。
前提: `design_scene_format.md`(camera コンポーネント)、R6(glTF 規約)、
`design_game_logic_native.md`(コントローラの置き場所)、
transform_seq v2 予約(カメラトラック)。

## 0. 「glTF 同等以上」の解釈(2026-07-07 決定の具体化)

1. **同等** = glTF のカメラ定義(perspective: yfov/znear/zfar/aspect、
   orthographic: xmag/ymag/znear/zfar)を**損失なく読み書き**できる。
   glb 内のカメラノードをシーンに取り込め、書き出せば round-trip する
2. **以上** = エンジン拡張(コントローラ・ブレンド・シェイク等)は
   **glTF を汚さない層**に置く — glb へ書くときは extras、シーンでは
   コンポーネント params。glTF 互換部分と拡張部分の境界を形式で明示する

## 1. 三層構造

**【改訂 2026-07-12 — ユーザー方針「feature 層 = ユーザー空間」の
カメラ適用】** 三層を「機構語彙(エンジン・遅い・版付き)/ ユーザー空間
(速い)」の速度分離で再整理する:

| 層 | 内容 | 帰属 |
|----|------|------|
| 定義 | perspective / orthographic(glTF 1:1)・camera コンポーネント形式 | **機構語彙**(形式は [PF] 規律で凍結) |
| アクティブカメラ機構 | 「常に 1 個・切替はフレーム境界で atomic」の保証・`set_camera`(GameContext + rpc)・transform_seq カメラトラック再生 | **機構語彙** |
| コントローラ | orbit / follow / fly / fixed・シェイク・**ブレンド/カット演出のロジック** | **ユーザー空間**(ゲームシステム。同梱 orbit/follow/fly は**特権なしの標準ライブラリ** — 公開 API(GameContext/Actions/EngineTime)のみで実装されていることを保証し、プロジェクトへコピーして改造したら自分のもの。G2 の DLL ホットリロードで保存 → 数秒反映) |

- **ブレンドの帰属(改訂)**: pose 補間は「2 つのカメラの world pose を
  読み、補間して自分の transform に書くユーザーシステム」で完全に
  書ける — エンジンに補間 primitive は持たない。エンジンが保証するのは
  atomic 切替とフレーム境界だけ。旧 C3 の「ブレンドは v2」はエンジン
  実装でなく**標準ライブラリのコントローラ例**として提供する
- **ジッタは完全に外**(2026-07-12 決定 — `design_postprocess_temporal.md`
  未決 2: レンダー側 projection modifier。カメラ系からは見えない)
- 同梱コントローラの dogfooding 検証(公開 API のみ使用か)は次の
  カメラ系 WP の受け入れ基準に含める(WP50 実装の追認)

- **アクティブカメラの切替**: シーン内に複数カメラを許可(現行は暗黙 1 個)。
  `set_camera {name}` を GameContext と rpc の両方に追加(stage 3 の続き —
  エディタのビュー切替にもそのまま使う)
- orthographic 対応はレンダラ側の射影行列生成に分岐を足す(現行 perspective のみ)

## 2. 既存との整合

- 現行 `camera` コンポーネント(fov 等)は glTF perspective のサブセット —
  v1 形式のまま**キー名を glTF に寄せて拡張**(scene format の version は
  上げず追加キーのみで済む想定。ずれる場合は v1.2 として追記)
- project.json の `basic_config.camera` は「カメラ未定義シーンの既定」として
  存続(意味論変更なし)
- 入力とカメラ: orbit/fly コントローラは Actions(`move` / `look`)を消費する。
  binding は入力 profile 側で与える

### 2.1 runtime free camera(WP265)

`pelican_player --free-camera` は、プロジェクトを変更しないデバッグ用の自由飛行
カメラを明示的に有効化する公開起動面である。Studio も同じ player 引数を使うだけで、
エディタ専用 API は持たない。

実装前に rendering config の `featurecompose` を調べた。そこには
`transform_resolved_config` があり、保存対象ではない解決済み rendering config を
起動時に変換できる。一方、scene/input には同種の汎用 hook がない。scene の
`AuthoringSceneDocument` は `save_scene` の保存正本なので、そこへ controller を注入すると
後の保存で利用者ファイルへ混入し得る。このため authored JSON の変換は採らない。

代わりに、次の二つをそれぞれの runtime 投影境界で合成する。

- camera: 解決済み scene camera 群とは別に synthetic な `fly` camera を加え、最初の
  scene camera があればその pose/projection を開始値にする。既存 camera の controller と
  authored transform は変更しない
- input: `engine://input/free_camera_actions.json` と
  `engine://input/profiles/free_camera.json` を入力 runtime が選ぶ。project の
  `input/actions.json`、`input/profiles/*.json`、`project.json` は読替えも書込みもしない

この overlay は `EngineLaunchConfig::free_camera` がある起動だけに存在し、省略時は camera と
input の従来経路だけを通る。操作は WASD=移動、矢印=視線、gamepad は左 stick=移動・右
stick=視線。既存の `Fly` とその `speed` / `sensitivity` を使い、新 controller 型は加えない。

## 3. 検証

- round-trip: カメラ入り glb → シーン取り込み → 書き出しでパラメータ一致
  (純ロジックテスト)
- golden: orthographic 1 ケース + orbit コントローラの決定的軌道 1 ケース
  (fixed_step で決定的)

## 4. 実装順(WP 候補)

C1: 定義層(glTF 1:1 化 + orthographic + 複数カメラ/set_camera)→
C2: コントローラ(orbit/follow/fly、ビルトインシステムとして)→
C3: glb round-trip + transform_seq v2 カメラトラック(v2 改訂と同時)。

## 5. 未決事項

1. aspect の扱い(glTF は省略可 = ビューポート追従。エンジンも同義に)
2. ブレンド(v2)の補間仕様
