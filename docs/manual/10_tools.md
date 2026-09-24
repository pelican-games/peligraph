# 第10章 ツールリファレンス

対象: pelican2(2026-09-17 時点、HEAD=`82054c05`)/ このマニュアルはコードを正とする

## この章で学ぶこと

- `pelican_player` の全 CLI 引数(**30 個**。正は [main.cpp](../../src/player/main.cpp) の `add_argument` 群)
- JSON-RPC(stdio)による外部制御 — 基盤メソッド、エディタ拡張、実セッション例
  - **JSON-RPC** は外部プロセスからエンジンを操る制御プロトコル、**stdio** はその通り道です: 起動した `pelican_player --rpc` の標準入力へリクエスト行を書き、標準出力から応答行を読みます(1 行 = 1 リクエスト / 1 応答の NDJSON。詳細は §10.3)
- `pelican_cli` の **9 サブコマンド系統**(`assets` / `bake-camera` / `import` / `dist-config` / `package` / `project` / `dump-lowered-material` / `vrm` / `graph-catalog`。正は [main.cpp の usage](../../src/devcli/main.cpp#L43))
- ホットリロード(シェーダ・テクスチャ・マテリアル・モデル・ゲーム DLL)と起動高速化
- ImGui 開発者 UI(F1)と Frame Plan Viewer
- 配布ビルド、Pelican Studio の現状、テスト基盤
- リポジトリ保守スクリプト(`tools/` — worktree ラッパ・切り離し build・API inventory・doclink)

## 10.1 実行物の一覧

| ターゲット | 種類 | 出力先 | 役割 |
|---|---|---|---|
| `pelican_player` | exe | `build/dist/<Config>/` | ゲームランタイム。ウィンドウ / ヘッドレス / RPC の 3 モード |
| `pelican_game_logic` | **DLL**(`PELICAN_PROJECT` 指定時) | player と同ディレクトリ | プロジェクトの `code/` のビルド産物(✅WP90 で静的リンクから移行) |
| `pelican_cli` | exe | `build/dist/<Config>/` | 開発 CLI。**エンジン(pelican_core)にリンクしない** |
| `pelican_studio` | exe(Qt6) | `build/dist/<Config>/` | Widgets エディタ shell + 別 process engine viewport。render-tools dock は **3 タブ**(Frame Plan / Features / Graph)で、Inspector にグラフ装着欄がある(§10.7) |
| `pelican-spv-link` | exe | `build/dist/<Config>/` | `PELICAN_WITH_SPIRV_LINK=ON`時だけ作るexperimental SPIR-V リンカ CLI([第6章](06_rendering.md) §6.7) |
| `pelican_project` | 静的 lib | build 内 | 解釈レイヤ(JSON パース/検証の純ロジック) |
| `pelican_core` | 静的 lib | build 内 | エンジン本体。`PELICAN_AUTO_STAGE_SDK=ON` のときだけ archive と公開ヘッダを `build/dist/<Config>/lib/` へコピー |

`pelican_runtime` target は有効な実行物と通常の project DLL をまとめてビルドします。
`cmake --install build-release --config Release --component Runtime --prefix build-release/package`
は `package/bin` に実行物と Windows の Qt runtime/plugin を組み立てます。先に
`cmake --build build-release --config Release --target pelican_runtime` を実行してください。
プロジェクトのコンテンツはこの component に含みません。全手順は [第2章 §2.2](02_getting_started.md#22-ビルド)。

## 10.2 pelican_player CLI リファレンス

実装: [src/player/main.cpp](../../src/player/main.cpp)。引数エラー時は usage を表示して終了コード -1、正常終了 0、実行時致命エラー 1。

| 引数 | 既定 | 意味 |
|---|---|---|
| `--project <dir\|project.json>` | (暗黙探索) | プロジェクトを開く。省略時は exe ディレクトリ → 祖先の `projects/example` を探索し、WARN を出す |
| `--feature-overlay <uri>` | なし | `pelican.render_feature_overlay` v1 の feature bundle(= `schema` / `version` / `name` / `features` だけを持つ小さな JSON。`features` の各要素は feature 参照文字列か `{ref, parameters}` — [renderfeatureoverlay.cpp](../../src/project/renderfeatureoverlay.cpp#L16))を**この起動だけ**重ねる。公開 CLI なので Studio 以外の profiler 等も利用可能 |
| `--headless` | off | ウィンドウなし実行(時刻は固定ステップになる) |
| `--rpc` | off | stdio JSON-RPC モード。**`--headless` は不要になりました**(✅WP156): ヘッドレスでは stdin を読み切るまでブロッキング、**ウィンドウモードではフレーム境界処理 + 有界キュー(既定 64 件)**。溢れると `busy` エラーを即返します。※`--rpc` を付けると ImGui UI は無効([第13章](13_editor.md)) |
| `--frames <n>` | 3 | ヘッドレスのフレーム数。**0 = 無制限** |
| `--size WxH` | 1280x720 | ヘッドレスの描画サイズ |
| `--render-out <path>` | なし | PNG 出力先(sRGB エンコード)。`%04d` 等で毎フレーム連番 |
| `--fps <f>` | 60.0 | ヘッドレス固定ステップのレート(dt = 1/fps) |
| `--start-time <秒>` | なし | **ヘッドレス専用**。この時刻の frame から `--frames` 枚を出す(✅O-1)。`llround(秒 × fps)` で **parse 時に frame 番号へ量子化**され、engine には「frame の間の時刻」が渡りません([parseStartFrame()](../../src/player/main.cpp#L284))。`--headless` が無ければ `offline.headless_required@1`、負・非有限・数でない綴りは**すべて** `offline.time_invalid@1`、`--rpc` / `--replay` / `--record-input` / `--bake-camera-output` との併用は `offline.mode_conflict@1`([検査](../../src/player/main.cpp#L523))。`--frames` は併用です |
| `--dump-frame-plan` | off | 解決済みフレームプラン JSON を **stderr** へ出力([第6章](06_rendering.md)) |
| `--gpu-labels` | off | Vulkan debug object 名と command label を有効化。validation layer の選択とは独立([第6章](06_rendering.md) §6.14) |
| `--vulkan-validation <off\|on>` | `_DEBUG`: on / それ以外: off | Khronos validation layer と synchronization validation を一体で選択。要求時に `VK_LAYER_KHRONOS_validation` が無ければ名前付き hard error |
| `--allow-absolute-paths` | off | CLI 由来のコンテンツ参照に限り絶対パスを許可。JSON 内の絶対パスは常に拒否 |
| `--user-dir <dir>` | なし | `user://` ルートの差し替え |
| `--log-file <path>` | なし | この起動のログを `<path>` に書く。親 directory は作り、**起動ごとに切り詰めます**(1 ファイル = 1 回の実行)。省略時は従来どおり cwd の `pelican.log` に追記します([main.cpp](../../src/player/main.cpp#L323)、[log.cpp](../../src/core/log.cpp#L32))。Studio は project ごとの log をこれで渡します(§10.7) |
| `--ignore-engine-version` | off | `engine_min_version` 不適合を hard error → WARN に降格 |
| `--play-seq <path.jsonl>` | なし | `pelican.transform_seq` の再生(`PELICAN_WITH_SEQPLAYER` ビルドユニットが ON のビルドでのみ使えます — [第2章](02_getting_started.md) §2.2) |
| `--seq-mesh <builtin:sphere\|path.glb>` | builtin:sphere | transform_seq 全オブジェクトに使うメッシュ |
| `--seq-loop` | off | クリップをループ再生 |
| `--play-vat <path.glb>` | なし | `pelican.vat` 入り GLB の再生(VAT ユニット必須) |
| `--camera "px,py,pz,tx,ty,tz,fov_deg"` | なし | カメラの位置・注視点・垂直 FOV(度)の上書き |
| `--game-logic <path.dll>` | (exe 同階層) | ゲームロジック DLL の明示指定(✅WP90) |
| `--strict-assets` | off | assets manifest の差分を起動エラーに昇格(✅WP66。ホットリロードも無効化) |
| `--record-input <path.jsonl>` | なし | 順序付き入力を `pelican.input_seq` v1 で収録(✅WP89) |
| `--replay <path.jsonl>` | なし | input_seq のリプレイ。`--record-input` と排他、ホットリロード自動無効(✅WP89) |
| `--input-profile <name>` | project.json の既定 | アクティブ入力プロファイルの上書き(✅WP91) |
| `--input-action-overlay <uri>` | なし | `pelican.input_action_overlay` v1 の action/profile bundle を**この起動だけ** project 入力へ重ねる |
| `--free-camera [blender\|unity]` | off (`blender` が省略時 preset) | project 入力と並存する runtime-only の `orbit` camera と同梱入力 overlay を有効化 |
| `--editor-transform [blender\|grab]` | off (`blender` が省略時 preset) | runtime の transform 編集アクションを有効化。`grab` は handle のドラッグだけを残す |
| `--xr off\|auto\|on` | off | OpenXR(PCVR)起動(✅WP125。`auto` = 不在なら flat 続行 / `on` = 不在・headless・rpc・リプレイでは名指しエラー — [第2章](02_getting_started.md)) |
| `--bake-camera-output <path.jsonl>` | なし | リプレイ中のカメラ軌跡を transform_seq v1 で出力。`--headless` + `--replay` 必須(✅WP89) |

よく使う組み合わせ:

```sh
pelican_player --project projects/example                                    # 通常起動
pelican_player --project projects/example --feature-overlay engine://features/editor.json # editor用featureを起動時だけ重ねる
pelican_player --headless --project mygame --frames 3 --render-out out.png  # ヘッドレス→PNG
pelican_player --rpc --headless --project mygame                            # 外部制御
pelican_player --project mygame --record-input s.jsonl                      # プレイ収録
pelican_player --headless --project mygame --replay s.jsonl \
  --bake-camera-output cam.jsonl                                            # カメラ焼き出し
pelican_player --project mygame --free-camera                               # Blender 操作(既定)
pelican_player --project mygame --free-camera unity                         # Unity 操作
pelican_player --project mygame --input-profile gamepad --free-camera       # gamepad profile と Blender 視点操作を併用
pelican_player --project mygame --vulkan-validation=on                      # 最適化ビルドでも validation + sync validation
```

`--feature-overlay` が読む文書は、厳密な `pelican.render_feature_overlay` v1 envelope
(`schema` / `version` / `name` / `features`)です。`features` の順序を保ったまま、project が
`rendering_config_json` で宣言した `features` の後ろへメモリ上で加え、同じ entry は重複させません。
引数が無ければ loader 自体を呼ばず、従来の rendering config をそのまま登録します。overlay URI は
`project.json` / scene / user 層には保存されず、overlay から追加された target と pass もその process の
寿命にだけ存在します。

**`pelican.graph` root では overlay も 1 段下に着きます。**root が `schema: "pelican.graph"` のとき、
overlay は `rendering.features` に積まれます —— root 直下に積んでも lowering が読まないからです。
`rendering` オブジェクトを持たない graph root は名前付きで断られます
(`render_config.section_unlocated@1`。[分岐](../../src/project/renderfeatureoverlay.cpp#L138))。

`--input-action-overlay` が読む文書は、厳密な `pelican.input_action_overlay` v1 envelope
(`schema` / `version` / `name` / `actions` / 任意の `profile`)です。`actions` は
`pelican.input_actions` v1、`profile` はその actions だけを参照する `pelican.input_profile` v1
への URI です。project の最初の action set の後ろへ overlay の全 set を積み、後ろほど高優先で
評価します。action 名・action set 名・overlay 名の衝突は衝突名を含む hard error です。
project profile と固定 overlay profile は同時適用され、RPC の profile 切替でも overlay binding は
残ります。引数が無ければ overlay loader を呼ばず、従来の project 定義をそのまま使います。
URI は `ProjectBasicConfig` に入らず、保存・再読み込みの対象にもなりません。

視点移動 preset の操作は次のとおりです。

| preset | orbit | pan | zoom |
|---|---|---|---|
| `blender`(既定) | MMB ドラッグ | Shift+MMB ドラッグ | Ctrl+MMB の上下ドラッグ / ホイール |
| `unity` | Alt+LMB ドラッグ | MMB ドラッグ | Alt+RMB の上下ドラッグ / ホイール |

既定を Blender にしたのは、要望が Blender の操作を起点としており、通常の視点移動も orbit
だからです。初期注視点は開始 camera の視線上 5 world units 先です。scene object を追加せず、
選択物へのフォーカスも行いません。未知の preset 名はエラーです。`--input-profile` は project 側、
free-camera preset は overlay 側を選ぶため、両者は併用できます。

`--start-time` は**飛びません。**指定した frame までの `start_frames − 1` frame を、capture だけ抜いて
**普通に回します**([replay prologue](../../src/core/appflow/loop.cpp#L672))。graph の時計は `render()` の中でしか
進まず、game system は自分で dt を積むので、「その時刻にいる」唯一の正直な方法が「そこまでの全 step を通ること」
だからです。開始時に stderr へ 1 行 `offline: start_time=… start_frame=… replay=… frames=…` が出ます。
最初に capture される frame が指定時刻の frame で、`--start-time 2.0 --frames 2 --fps 60` の `0001` は
通常起動の 120 枚目と同じ画です(`--start-time 0` は通常起動と同一)。

※ `--play-seq` / `--seq-mesh` の相対パスは現状 cwd 基準で解決されます(既知の食い違い。[第11章](11_status.md))。

## 10.3 JSON-RPC(stdio)— 外部制御プロトコル(✅実装済み)

実装: [rpcserver.cpp](../../src/core/communication/rpcserver.cpp)。プロトコルの正本は [../external_tools_requirements.md](../external_tools_requirements.md) R8。

### 枠組み(不変)

- stdin/stdout の NDJSON(1 行 = 1 つの JSON-RPC 2.0 リクエスト)。**stdout はプロトコル専用**(ログは cwd の `pelican.log`。`--log-file` を付けたときはそのファイル)。
- batch・notification 非対応。エラーコードは標準 + アプリケーションエラー -32000、RenderDoc capture -32010。ハンドラ内例外は応答に変換されプロセスは落ちません。EOF で正常終了。
- 1 プロセス 1 クライアント。**ゲーム DLL リロード中は全メソッドがアプリケーションエラーで拒否**されます。

> **設計決定(決定性):** 同一の RPC スクリプトを 2 回実行したとき、応答列は(instance_id を除き)**バイト単位で完全一致**しなければならない。CI で検証済み。`instance_id` だけが除外されるのは、それが RPC エンドポイント生成時に振られる UUID v4 で、起動ごとに必ず変わる値だからです([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L2062))。

### 基盤メソッド一覧

| メソッド | params | 動作 |
|---|---|---|
| `get_status` | `{}` | `{instance_id, project_root, scene, frame, time, seed, renderdoc, diagnostics:{renderdoc:{...}}, debug_utils:{...}, vulkan_validation:{layer, available, enabled, synchronization, reason}, stores, input:{...}, reload:{...}, participants:{game_logic:{...}}, sprite:{...}, color:{...}, render:{published_frames}, startup:{...}, xr:{active, reference_space, floor_semantics, ...}, animation:{available, sinks:[{owner, node, source, cursors:[{clip, time_seconds, normalized_phase}], frame_revision, pose_hash, state?, transition?, root_delta?}]}}`(`animation` は AG-3a。animation module の無い build では `{available: false}` だけ)※rpc は常に flat 駆動のため `xr.active=true` は rpc からは観測できない |
| `set_seed` | `{seed}` | 決定的乱数のシード設定 |
| `set_time` | `{t}` | 仮想時刻の直接設定(dt=0) |
| `step_frame` | `{}` | 1 フレーム進める |
| `render_frame` | `{}` | 時刻を進めず描画のみ |
| `capture_gpu` | `{}` | `render_frame` と同じcurrent-time描画を1回だけ**RenderDoc**(GPU に送った描画コマンドを 1 フレーム分丸ごと記録して後から追える外部デバッガ)で captureし、実際に増えたindexのcanonical `.rdc` pathを返す |
| `capture` | `{path}` | 直近フレームを PNG 保存(sRGB。応答に `encoding:"srgb"`) |
| `get_frame_plan` | `{}` | フレームプラン JSON |
| `pick_object` | `{x, y}` | 最後に完了した **ID バッファ**(色ではなく「そのピクセルに写っているオブジェクトの ID」を書き出す描画出力。`picking` feature が宣言する `R32_UINT` のレンダーターゲット `picking_id` — [picking.json](../../src/core/resources/features/picking.json))の 1 ピクセルを同期読み出し。`engine://features/picking.json` が必要 |
| `set_gizmo` | `{selection:GizmoSelection\|null, mode}` | 選択 transform のギズモ表示を設定/解除。`engine://features/gizmo.json` が必要 |
| `query_gizmo_handle` | `{selection:GizmoSelection, mode, x, y}` | 指定した選択・mode・物理 pixel にある handle を状態なしで問い合わせ。同 feature が必要 |
| `load_gltf` | `{path, name?}` | glb の一時ロード(プロジェクト相対のみ) |
| `load_scene` | `{name}` | シーン切替。応答は `{name, committed, effect_errors[]}`(✅WP380b/380e。commit 後の副作用の失敗は error ではなく result に出ます — [第13章](13_editor.md) §13.13) |
| `set_camera` | `{name}` | シーン内カメラへ切替 |
| `update_transforms` | `{objects, transforms}` | 次フレーム境界で適用。`rot` は名指し拒否(`rotation`) |
| `inject_input` | `{events:[...]}` | 入力注入。`key_*` / `mouse_*` / `axis` + **`gamepadButtonDown` / `gamepadUp` / `gamepadAxis`**(リプレイ中は拒否) |
| `inject_event` | `{type, payload}` | 登録済みゲームイベントの emit。payload は EventPayloadSchema で事前検証(✅WP71) |
| `reload_game_logic` | `{}` | ゲームロジック DLL を即時リロード(✅WP90) |
| `set_input_profile` | `{name}` | 入力プロファイル切替(✅WP91) |
| `start_input_record` | `{path}` | input_seq 収録開始(✅WP89) |
| `stop_input_record` | `{}` | 収録停止。`{path, frames, events}` |
| `start_input_replay` | `{path}` | リプレイ開始(fixed-step 化・リロードゲート閉鎖) |
| `stop_input_replay` | `{}` | リプレイ停止 |
| `export_scene_snapshot` | `{schema_version:1, allow_pending?:false}` | current scene を含む全 **authoring document**(= 編集の正本としてメモリ上に持つシーン文書。シーンファイル 1 つにつき 1 文書で、ディスクの実物とは別 — [第13章](13_editor.md) §13.2)の deterministic semantic bytes、SHA-256、revision を返す |
| `import_scene_snapshot` | `{schema_version:1, semantic_scene_bytes, digest, current_scene_id}` | disk を上書きせず、検証済み snapshot を in-memory scene source として reload する |

`pick_object` の座標は ID バッファ `picking_id` の左上原点で、`x` は右、`y` は下へ増えます。
両方とも符号なし整数だけを受理し、範囲外は `-32602`、feature 無効または完了済みの
picking frame が無い場合は `-32000` です。背景は `hit: null`、モデル面は次の形です。

```json
{
  "contract": 1,
  "coordinate": {"x": 320, "y": 180},
  "extent": {"width": 1280, "height": 720},
  "frame_index": 42,
  "hit": {
    "scene_id": "default_scene",
    "declaration_index": 3,
    "authoring_object_id": "...",
    "model_instance": {"index": 7, "generation": 2, "scene_epoch": 4}
  }
}
```

`scene_id` と 0 始まりの `declaration_index` は `scene_tree` / `get_components` と同じ
WP258 契約です。authoring 宣言を持たない一時モデルでは三つの authoring field が
`null` でも、世代付き `model_instance` は残ります。読み出しを同期にした理由と ID の
GPU 表現は[第6章](06_rendering.md#id-バッファ-pickingwp262)を参照してください。

`set_gizmo` と `query_gizmo_handle` の mode は `translate` / `rotate` / `scale` のいずれかです。
`GizmoSelection` は次のどちらか**ちょうど一つ**を表すタグ付き object です(**タグ付き** = `kind` の値でどちらの形かが決まり、その形の鍵しか書けない。両方の鍵を混ぜた object は拒否)。記載した鍵以外は
受理しません。

| `kind` | 厳密な形 | 対象 |
|---|---|---|
| `declaration` | `{"kind":"declaration","scene_id":string,"declaration_index":uint}` | `pick_object` / `scene_tree` と同じ authoring 宣言 |
| `runtime` | `{"kind":"runtime","object_id":{"index":uint32,"generation":uint32}}` | 現在の実行に存在する世代付き `GameObjectId` |

Studio の Outliner 選択は前者を送ります。後者は authoring 宣言を持たず、実行中に生成した
オブジェクトも直接指せます。
表示の設定例:

```json
{
  "selection": {
    "kind": "declaration",
    "scene_id": "default_scene",
    "declaration_index": 3
  },
  "mode": "rotate"
}
```

応答は `{"contract":1,"visible":true,"selection":...,"mode":"rotate"}` で、同じタグ付き
selection を返します。
`selection: null` と mode を送ると表示だけを解除します。engine に hover、押下、active handle、
ドラッグ状態は作られません。表示要求は process memory にしかなく、scene/project へ保存する経路も
ありません。`GameObjectId` は実行をまたいで安定しないため、runtime selection は対象を生成した
現在の実行でだけ使い、次の実行では新しい ID で要求し直してください。

当たり判定は左上原点、右向き X、下向き Y の物理 pixel 座標で、選択と mode を問い合わせごとに
明示します。たとえば:

```json
{
  "selection": {
    "kind": "runtime",
    "object_id": {"index": 17, "generation": 4}
  },
  "mode": "translate",
  "x": 420,
  "y": 240
}
```

```json
{
  "contract": 2,
  "selection": {
    "kind": "runtime",
    "object_id": {"index": 17, "generation": 4}
  },
  "mode": "translate",
  "coordinate": {"x": 420, "y": 240},
  "extent": {"width": 1280, "height": 720},
  "content_scale": 2.0,
  "grab_radius_pixels": 20.0,
  "handle": {
    "id": "translate_x",
    "axis": "x",
    "drag_direction": {"x": -1.0, "y": 0.0},
    "value_per_logical_pixel": 0.0125
  }
}
```

外れは `handle: null` です。`drag_direction` は問い合わせ座標と同じ Y 下向き空間の単位ベクトル、
`value_per_logical_pixel` は移動なら world 単位、回転なら radian、拡縮なら指数の無次元量です。
`content_scale` は物理 pixel 差分を論理 pixel 差分へ直す係数です。client は物理 pixel 差分を
この明示値で割った論理 pixel 差分 `d` に対して
`scalar = dot(d, drag_direction) * value_per_logical_pixel` だけを計算し、カメラ・射影を複製しません。
問い合わせは `set_gizmo` の値や描画済み frame を読み書きせず、掴み代は描画の 1 px 線幅とは
独立した 10 論理 px(DPI 2x の例では応答どおり 20 物理 px)です。射影軸が 8 論理 px 未満へ
潰れた移動・拡縮マーカーは、方向が未定義なので `handle: null` になります。
範囲外座標、不正 mode、存在しない transform は `-32602`、feature 無効は `-32000` です。
selection の硬いエラーは `error.data.code` でも判別できます。宣言鍵と runtime 鍵の混在は
`gizmo_selection_mixed_identity`、どちらの厳密形でもないもの(旧 untagged 形を含む)は
`gizmo_selection_invalid_shape`、未知の `kind` は `gizmo_selection_unknown_kind`、存在しない／世代が
古い runtime ID は `gizmo_runtime_object_not_found`、対象に transform が無い場合は
`gizmo_target_transform_not_found` です。混在時に一方を優先する互換経路はありません。
詳細は[第6章](06_rendering.md#ギズモ-render-feature-と状態なし当たり判定wp274)を参照してください。

### エディタ拡張メソッド(詳細は[第13章](13_editor.md))

上の表の `export_scene_snapshot` / `import_scene_snapshot` はエディタトラックで増えた分です。**総数は挙げません** —— メソッドは増え続けるので、数字を書くと必ず腐ります。登録の実物は `src/core/communication/rpcserver.cpp`(基盤)と `src/core/communication/editorrpchandlers.cpp`(エディタ)です。

シーンの編集・履歴・プレビュー・保存のためのメソッド群です(✅WP154/156/157/158/161/166/168/170/172)。params と戻り値、**エラーが `result.status` に出る**という重要な作法は [第13章](13_editor.md) §13.5〜§13.6 にまとめてあります。`edit` 系だけ作法が違うのは、これらが**チケットを返す受付のメソッド**で、呼び出し自体は成功しており、実際の適用がフレーム境界で起きるためです(受付で弾かれた = `rejected` / 適用時に弾かれた = `failed`)。基盤メソッドは呼んだその場で答えが決まるので、失敗は通常の JSON-RPC `error` に出ます。

| 分類 | メソッド |
|---|---|
| 読み取り | `scene_tree` / `get_components` / `get_scene_revision` / `list_assets` / `query_journal` / `can_edit` / `can_preview` |
| セッション | `open_editor_session` / `resume_editor_session` |
| 編集 | `edit` / `undo` / `redo` / `get_edit_result` |
| チケットプレビュー | `open_preview` / `update_preview` / `commit_preview` / `abort_preview` / `get_preview_result` |
| 隔離プレビュー | `eval_preview` / `render_preview` |
| 保存 | `save_scene` |
| 共通グラフ | `get_graph_instances` / `get_graph_catalog` / `get_graph_values` / `get_graph_node_values` / `edit_graph_document` / `edit_graph_instances` |
| 共有部品と package | `export_render_feature` / `import_render_feature` / `install_package` / `uninstall_package` |

既存メソッドの変化: `get_status` に `scene_source`(シーンは**ホットリロード対象外**・保存は `save_scene`)が追加、`step_frame` の応答に `edit_results[]` が追加、`load_scene` / `reload_game_logic` / `start_input_replay` は進行中のプレビューを強制中止します。
2026-09 の追加(詳細は [第13章](13_editor.md) §13.13): `get_status` に `participants.game_logic`
(`error_code` / `error_fields`。**起動時の初期化失敗も未試行の状態で出ます**)、`load_scene` と
`import_scene_snapshot` の result に `committed` / `effect_errors`、`get_components` の各
component に `resolution_error`。

2026-09 の追加(AG-3a、`32d9a20b`): `get_status` に `animation`(上の表)。新メソッド `get_graph_node_values{instance?}` → `{instance, generation, frame, published, source:"rendering", nodes:[{node, expanded, outputs:{port: value}}]}`(publish 時の snapshot、再評価しない。`instance` 省略で root 側の展開木全体、rendering domain の node は `outputs: {}`、publish 前は `published: false`)。**animation domain の node は空ではありません** —— 自分の値は出しませんが、そのフレームに**食べたスカラー**が values 側に `$pose/<node>/<port>` として publish されるので、**受けた入力ポートの名前で**並びます([renderconfigeditor.cpp:2010](../../src/core/communication/renderconfigeditor.cpp#L2363)、[graphAnimationScalarOutput()](../../src/project/graphpartition.cpp#L168))。読み取り専用で `edit_*` の CAS には乗らない。

**`get_graph_values` と `get_graph_node_values` は問いが違います。**前者は `instance` 必須で、その
**instance の入力 port だけ**を `{port, value, source, connected|parameter}` として返します(`source` は
`connected` / `parameter` / `default` / `override`)。後者は**展開木のノードを全部**並べ、各ノードの出力値を
`outputs` に入れます([実装](../../src/core/communication/renderconfigeditor.cpp#L2282))。両方とも
**同じ publish snapshot を読むだけ**で再評価しません —— 再評価は graph の時計を進め event を二重に消費するためです。
`source: "rendering"` は「これは描画側の評価が使った値だ」という明示で、animation phase は同じ文書を
自分の state で評価するため、event や trigger で駆動される文書では両者が恒久的に違いうることを表します。

2026-09 の追加(グラフ・共有部品・package): `get_graph_instances` / `get_graph_catalog` / `get_graph_values` /
`edit_graph_document` / `edit_graph_instances`([登録](../../src/core/communication/editorrpchandlers.cpp#L222))、
`export_render_feature` / `import_render_feature`(`user://components/<name>/` 経由の共有部品)、
`install_package` / `uninstall_package`(`project://packages/<ns>/` への vendoring)。params と返り値、
`ops` の 5 種と `scope`、名前付きの拒否は [第13章](13_editor.md) §13.5 が正です。

### 実セッション例

結合テスト `test/run_rpc_headless.cmake` が実際に流している入力(抜粋):

```jsonl
{"jsonrpc":"2.0","id":1,"method":"get_status","params":{}}
{"jsonrpc":"2.0","id":2,"method":"set_time","params":{"t":1.25}}
{"jsonrpc":"2.0","id":3,"method":"load_gltf","params":{"path":"assets/ground.glb","name":"movable"}}
{"jsonrpc":"2.0","id":4,"method":"update_transforms","params":{"objects":["movable"],"transforms":[{"pos":[3.0,0.0,-0.75],"rotation":[0.0,0.0,0.0,1.0],"scale":[0.6,0.6,0.6]}]}}
{"jsonrpc":"2.0","id":5,"method":"step_frame","params":{}}
{"jsonrpc":"2.0","id":6,"method":"capture","params":{"path":"out/rpc_capture_left.png"}}
```

名前(オブジェクト名・`load_gltf` の name)は `[a-zA-Z0-9_]` のみ(R7)です。

`capture_gpu` は外部からRenderDocを注入して起動した場合だけ有効です。通常起動では
`-32010` と `data.reason:"renderdoc_not_injected"` を返します。XR active中はv1境界として
`capture_xr_unsupported` です。F11とRPCはいずれも同じcapture状態機械を使います。

### Python 薄型クライアント

`tools/pelican_rpc.py` は Python 3.12 の標準ライブラリだけで player の起動と
JSON-RPC の送受信を行います。リポジトリルートをカレントディレクトリにして、
次のように使用します。

```python
from tools.pelican_rpc import PelicanRpc, PelicanRpcError

try:
    with PelicanRpc("projects/mygame") as rpc:
        status = rpc.get_status()
        rpc.set_time({"t": 1.25})
        frame = rpc.step_frame()
        tree = rpc.scene_tree()
        print(status["scene"], frame["frame"], tree["objects"])
except PelicanRpcError as error:
    print(error.code, error.message, error.data)
```

`exe_path` を省略すると `build/dist/Debug/pelican_player.exe` を探索します。
別の build ツリーを使う場合は `PelicanRpc(project, exe_path=player_path)` と明示して
ください。`call(method, params)` と各 helper は params の辞書を変換・検証せず
エンジンへ渡します。形式の正は常に `rpcserver.cpp` です。常駐プロセスを残さない
ため、通常は上例のように `with` を使用してください。

`export_scene_snapshot(params)` と `import_scene_snapshot(params)` も同じ素通し helper
です。import の検証は client ではなく engine が、必ず version → 64 MiB → SHA-256 →
parse/semantic → current scene の順で行います。失敗時は `PelicanRpcError.data` の
`code` が `unsupported_snapshot_version` / `snapshot_too_large` /
`digest_mismatch` / `snapshot_invalid` / `scene_not_found` のいずれかになり、
`snapshot_invalid` には `detail` も入ります。

### snapshot を使った安全なスイープ

人が操作している session の document を scratch process へ immutable に渡し、候補値だけを
人 session へ戻す公式 sequence は次のとおりです。snapshot 自体を merge/save する API は
ありません。

```python
from tools.pelican_rpc import PelicanRpc, PelicanRpcError

with PelicanRpc("projects/mygame") as human:
    exported = human.export_scene_snapshot(
        {"schema_version": 1, "allow_pending": False}
    )
    revision = exported["scene_revision"]  # R
    payload = {
        "schema_version": 1,
        "semantic_scene_bytes": exported["semantic_scene_bytes"],
        "digest": exported["digest"],
        "current_scene_id": exported["current_scene_id"],
    }

    with PelicanRpc("projects/mygame") as scratch:
        scratch.import_scene_snapshot(payload)
        # 候補ごとに PREVIEW0 の eval_preview / render_preview を call() し、
        # 必要なら capture() する。scratch の scene file は変更されない。
        chosen_operation = sweep_and_choose(scratch)

    session = human.call(
        "open_editor_session", {"display_name": "snapshot sweep adoption"}
    )
    accepted = human.call(
        "edit",
        {
            "actor_id": session["actor_id"],
            "base_revision": revision,
            "operations": [chosen_operation],
        },
    )
    # frame boundary 後に get_edit_result を確認する。stale_revision なら
    # 自動再送せず、現在値を query → 再 export → 候補を再判断する。
```

`semantic_scene_bytes` は UTF-8 JSON text の inline string で、base64 ではありません。
上限は 64 MiB です。import 成功時も通常 project の scene file は byte 不変で、
AuthoringObjectId は scratch session 内で新しく採番されます。採用は必ず `base_revision=R`
の通常 edit 一件に絞り、`stale_revision` を「人が R 以後に編集した」合図として扱います。

📐設計のみ: WebSocket 展開(複数クライアント)。

## 10.4 pelican_cli リファレンス

```
pelican_cli <assets|bake-camera|import|dist-config|package|project|dump-lowered-material|vrm|graph-catalog> ...
```

エラーは stderr+終了コード -1、成功は 1 行サマリ+終了コード 0。

### `pelican_cli project init <dir> [--template standard|blank]`(✅WP57 / ✅F-2)

空(または未存在)ディレクトリに雛形一式を生成します。生成直後に `pelican_player --project <dir>` でそのまま起動できます。詳細は [第2章](02_getting_started.md)。
空でないディレクトリは `project init target directory is not empty: <dir>` で拒否し、何も書きません。

雛形の中身は [`src/project/projecttemplate.cpp`](../../src/project/projecttemplate.cpp) の 1 箇所にあり(`pelican_project`)、
CLI は `--template` を読んで [`initializeProjectTemplate()`](../../src/project/projecttemplate.cpp#L382) を呼ぶだけです
([CLI 側](../../src/devcli/projectinit.cpp#L27))。Studio の File > New Project... も同じ関数を呼びます([第13章](13_editor.md))。
`--template` は 2 つです。

- **`standard`**(既定): 以下で説明する 3D シーン向けの 16 エントリ。F-2 の前に `project init` が書いていたものと
  byte 単位で同じです(`devcli_project_templates` が SHA-256 で固定)。
- **`blank`**: `standard` と同じ 16 エントリに `shaders/present.frag` を足した 17 エントリ。違うのは
  `passes/main_rendering_config.json` の `rendering` 節だけで、`pipeline` キーが無い **direct config** です
  (preset も `features` も `render_targets` もありません)。中身は fullscreen pass `present` 1 本
  (`input: []`、`output.color: ["display"]`、vertex `engine://fullscreen`、fragment `project://shaders/present`)で、
  `shaders/present.frag` は入力を読まずに UV の gradient を書きます
  ([雛形](../../src/project/projecttemplate.cpp#L211))。engine は direct config にも canonical anchor と
  `display` を読む `output_transform` を足すので、描かれる frame は「project が書いた pass 1 本 + engine の終端」です。
  scene / input / ui / code / asset は `standard` と同じです(3D の model は置きません)。

知らない名前は `unknown project template: <name> (expected standard|blank)` で拒否します。

生成される `passes/main_rendering_config.json` は **`pelican.graph` root** です(両方の雛形で同じ)。`standard` で著作した内容
(`pipeline.preset` = `engine://render_pipelines/hybrid_v1.json`、`features` = `shadow_directional` と
`sky_ambient` の 2 本)は `rendering` オブジェクトの中に入り、包む処理は
[`wrapLegacyRenderingConfig()`](../../src/project/renderconfigsection.hpp#L62) の 1 箇所だけです
([呼び出し側](../../src/project/projecttemplate.cpp#L249)。著作した文字列そのものは [:198](../../src/project/projecttemplate.cpp#L198))。`render_targets` / `rendering_passes` は preset が持つので
project は書きません —— `run_devcli_project_init.cmake` は**まず root が `pelican.graph` で `rendering` が
object であることを確かめてから**、節の中に禁止メンバが無いことを見ます
([検査](../../test/run_devcli_project_init.cmake#L48)。順序が逆だと、節ごと消えたときに検査が素通りします)。

生成される `code/CMakeLists.txt` は 2 行で、`pelican_game_sources(game.cpp)` の**後に**
`pelican_packages()` を呼びます([template](../../src/project/projecttemplate.cpp#L265))。順序が意味を持ちます ——
package の object は、その前に作られたゲームロジック DLL へ合流するからです。

### `pelican_cli import ...`(✅WP21/79/84)

DCC 納品物の取り込み。3 つの形があります:

- `import <delivery_dir> --project <dir>` — `pelican.import` manifest の sha256 照合 → asset_data.json へ glb 登録(冪等)
- `import gltf --extract-scene <glb> ...` — glTF のノード階層を scene v1(`parent` 付き)として抽出(✅WP79。[第5章](05_assets.md))
- `import --rules ...` — `imports.rules.json`(glob → レシピ表)に従った一括取り込み(✅WP84)

manifest の形式・実例は [第5章](05_assets.md) を参照してください。

### `pelican_cli assets manifest|verify|status --project <dir>`(✅WP66)

asset store の sha256 台帳(assets manifest)を生成・照合します。

```sh
pelican_cli assets manifest --project mygame            # 生成(store 走査 → manifest 書き出し)
pelican_cli assets verify --project mygame [--full]     # 照合(差分は severity 付き列挙、あれば exit 1)
pelican_cli assets status --project mygame              # OK / MISSING / MISMATCH の一覧
```

> **設計決定(検証はロードを止めない):** 起動時検証の深刻度は「内容の変化 = INFO、構造の逸脱(欠落・参照不能)= WARNING」で、エラーに昇格するのは `--strict-assets`(または配布 strict)のときだけ。

### `pelican_cli bake-camera --replay <path.jsonl> --project <dir>`(✅WP89)

リプレイを headless 実行し、カメラ軌跡を `imports/pelican-camera/<name>/` に `camera.transform_seq.jsonl` + `pelican.import` manifest として着地させます(DCC への逆輸出)。

### `pelican_cli dump-lowered-material <surface>`(✅WP76)

`.surface` の lowering 結果(std140 レイアウト・生成 GLSL)を stdout に出力します。「B は C の糖衣」の検証・デバッグ用です。

### `pelican_cli dist-config <project> [--with rpc,seqplayer] [--out <file>]`(✅WP41)

> **設計決定(配布は宣言から導出):** 配布ビルドの機能フラグを手で並べさせない。プロジェクトの内容を走査して必要な `PELICAN_WITH_*` を導出し、根拠コメント付きの CMake キャッシュプリセットとして書き出す。

- `PELICAN_WITH_VAT` — GLB に `pelican.vat` extras が実在すれば ON / `PELICAN_WITH_EXR` — `.exr` 参照があれば ON
- `PELICAN_WITH_RPC` / `PELICAN_WITH_SEQPLAYER` / `PELICAN_WITH_OPENXR` — 既定 OFF。`--with rpc,seqplayer,openxr` で明示 ON
- **`PELICAN_WITH_IMGUI` — 常に OFF を書き出す**(配布ビルドに開発 UI を含めない)
- **`PELICAN_WITH_RENDERDOC` — 常に OFF を書き出す**(配布ビルドにcapture integrationを含めない)

```sh
pelican_cli dist-config projects/mygame --out build-dist/preset.cmake
cmake . -B build-dist -C build-dist/preset.cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DSKIP_DEVSTUDIO=ON
cmake --build build-dist --config Release --target pelican_runtime
cmake --install build-dist --config Release --component Runtime --prefix build-dist/package
```

※ AUDIO / PHYSICS 系は導出対象外(常に既定 ON)。B3(embed サブセット)・B4(shaderc OFF 焼き込み配布)は 📐設計のみ。

### `pelican_cli vrm dump <model.vrm>`(✅WP111)

VRM 1.0(`VRMC_vrm`)の humanoid / expressions / lookAt を検証して canonical dump を出します
(レンダラへの適用はしません)。引数は `dump` と 1 ファイルちょうどで、それ以外は usage を出して失敗します。
詳細は [第5章](05_assets.md)。

### `pelican_cli package install|list|inspect`(✅X-1)

package は `pelican.package` v1 の `package.json` を持つ**ディレクトリ 1 つ**です。作る場所はどこでもよく、
install がその中身ごと `<project>/packages/<namespace>/` へ複製します。**複製した時点で「どこから来たか」は
意味を失います** —— engine が project の外の package を解決する経路はありません
([契約](../../src/project/packagemanifest.hpp#L13))。

```sh
pelican_cli package install <dir> --project <project>   # 複製 + project.json への記録
pelican_cli package list --project <project>            # 記録した digest を実バイトと照合
pelican_cli package inspect <dir>                       # install せずに manifest を読む
```

- **`install`** —— 検査は全部**最初の 1 byte を書く前**に終わります(manifest 不在 / 不正、engine が古い、
  依存 package が無い、feature 文書が不正、同名が install 済み、SDK ABI 不一致)。複製は staging ディレクトリを
  作ってから rename します。**namespace は文書に入ります** —— vendoring のとき feature の `name` が
  `<namespace>.<元の名前>` に書き換えられるので、名前を比べるすべての場所が同じ名前を見ます。
  成功すると `{status, namespace, package_version, sha256, destination, feature_references, feature_names}` を
  JSON で出します([実装](../../src/devcli/packagecommand.cpp#L88))。
- **`list`** —— `project.json` の `packages[]` を読み、**実ファイルの sha256 を計算し直して**比べます。
  ディレクトリごと無ければ `status: "missing"`、中身が書き換わっていれば `status: "modified"` で、
  どちらかがあれば stderr に `package_digest_mismatch` を出して非 0 終了です
  ([照合](../../src/devcli/packagecommand.cpp#L140))。digest を「書くだけ」にしないための本番経路です。
- **`inspect`** —— manifest を検証して **10 鍵**をそのまま出します: `schema` / `version` / `namespace` /
  `package_version` / `description` / `requires` / `dependencies` / `provides`(`features` / `graphs` /
  `components` / `code`)に、読んだ場所の `directory` と、この engine が期待する `sdk_abi_expected` が足されます
  ([組み立て](../../src/devcli/packagecommand.cpp#L64)、[足す 2 鍵](../../src/devcli/packagecommand.cpp#L179))。
  agent が「この package が何を持ち込むか」を install 前に読むための口です。

拒否は `PackageError` が code を運ぶので、CLI も RPC も理由を組み立て直しません
(`package_manifest_missing` / `package_manifest_invalid` / `package_namespace_invalid` /
`package_path_invalid` / `package_in_use` など)。Studio 側に package の UI はありません ——
install した feature は `list_render_features` の catalog に `source: "package"` として現れるだけです
([第13章](13_editor.md) §13.5)。

### `pelican_cli graph-catalog`(✅AG-0)

引数を取りません。**登録済みグラフモジュールのカタログ**を `pelican.graph_catalog` v1 の JSON で stdout に出します
([実装](../../src/devcli/graphcatalogcommand.cpp#L10))。モジュール作者が engine のソースを読まずに
型・操作・sink を知るための口で、`get_graph_catalog` RPC が返すのと同じ値です。

```sh
pelican_cli graph-catalog
```

モジュールは既定 build で **4 本**(`pelican.value` / `pelican.audio` / `pelican.render` / `pelican.pose`)、
`PELICAN_WITH_ANIMATION=OFF` では `pelican.pose` が消えて **3 本**です。この個数と、各操作が
`domain` / `evaluation` / `change` / `sink` を持つことは
[`run_devcli_graph_catalog.cmake`](../../test/run_devcli_graph_catalog.cmake#L23)(ctest 名 `devcli_graph_catalog`)が
固定しています。出力はモジュール名・操作名・型・sink のどれも**名前順**です([catalog()](../../src/project/graphmodule.cpp#L227) が名前を鍵にした `std::map` を走り、型と sink は明示的に sort します)。登録順に依存しません。

## 10.5 ホットリロード(✅WP90/96〜110/147/162/178)

保存すれば動いたまま反映される、が現在の開発体験です。基盤は [src/core/watch/](../../src/core/watch)(FileWatcher + ReloadService。デバウンス 200ms、適用は**次フレーム先頭で一括**、mid-frame 差し替えなし)。

| 対象 | 状態 | 挙動 |
|---|---|---|
| シェーダ / `.surface`(+ `.material.json` の cross-file) | ✅WP108 | 影響する全バリアント・パイプラインを準備してから atomic に公開。失敗時は旧世代維持 + WARNING |
| テクスチャ(PNG / EXR / KTX2) | ✅WP100 | 同 shape は in-place、shape 変化は再バインド(実測 ~1ms) |
| マテリアル値(`.material.json`) | ✅WP105 | **同レイアウトのみ** SSBO 値更新(レイアウト変化は WARN + 旧値継続 🚧) |
| モデルコンテナ(.glb / .gltf / .vrm) | ✅WP110/147 | 同一コンテナの全 fragment + live instanceを1 transactionで再構築。animationは対象asset generationだけ更新し、別assetは継続 |
| VRMA source/profile | 部分✅WP178 | generation/stale/rebind entry pointは済。FileWatcher/loaderからの自動配線は未 |
| ゲームロジック DLL | ✅WP90/162 | 下記。candidateを旧DLLと並存side-decodeしてから切替 |
| input actions / profile(HR2-I)、pelican.ui(U3) | 📐 | backlog Tier 1 |
| rendering config | ✅ | ディスク経由は `pelican.render_pipeline` 参加者が拾う。加えて Studio から `features[]`・feature の scalar パラメータ・**共通グラフ文書**(`nodes` / `definitions`)を編集して再起動なしで反映できる(§13.5)。どれも engine がファイルを所有し、編集側は触りません |
| scene JSON / project.json | 対象外 | 設計上の決定(再起動) |

- **有効条件**: ウィンドウモードのみ(既定 ON・専用フラグなし)。リプレイ / `--strict-assets` / rpc 駆動中は中央ゲートで無効。headless では自動監視しません。
- **fail-soft**: 壊れたファイルを保存しても旧リソースで動き続け、名前入り WARNING と `get_status.reload.last_reload_error` に出ます。修正して保存し直せば回復します。

### ゲームロジック DLL のホットリロード(✅WP90/162 = G2/BEH1)

`-DPELICAN_PROJECT=<dir>` でプロジェクトの `code/` は **`pelican_game_logic.dll`** としてビルドされます(静的リンクからの移行 — [第8章](08_gameplay.md))。リロードのトリガーは 3 通り:

1. ウィンドウモードでの自動検出(DLL の更新を監視)
2. **F5** で強制リロード
3. rpc `reload_game_logic`

適用方式は **full-reset**: system state/ECS/behaviorを破棄して現在sceneを
再構築し、hot stateは維持しません。ただしcandidate DLLは旧DLLと並存する
registration-only phaseでABI、schema fingerprint、type継続性、全authored
behavior paramsをside-decodeします。不適合なら旧DLL/runtimeを一切変えず拒否します。

### 起動高速化(✅WP82)

- **シェーダディスクキャッシュ**: `<project>/.pelican/shader_cache/`(SHA-256 キー)。2 回目以降のシェーダコンパイルはほぼゼロに。破損しても WARN 1 行で通常コンパイルに戻ります。
- **並列モデルロード**: CPU 側 prepare を並列化(GPU commit は宣言順直列 = 決定性維持)。起動時に 1 行レポート(`startup: ...`)が出て、`get_status.startup` でも見えます。

## 10.6 ImGui 開発者 UI(✅WP85/86/159/164/170)

`PELICAN_WITH_IMGUI`(既定 ON・配布は dist-config が常時 OFF)でビルドすると、**通常ウィンドウ起動時のみ** ImGui のデバッグ UI が使えます。**`--rpc` / headless / リプレイ / golden / XR セッション中は無効**です(そのため「GUI で触りながら外部エージェントも繋ぐ」ことは現状できません — [第13章](13_editor.md) §13.1)。

- **開発者 UI は既定で表示されています。`F1` は開くキーではなくトグル**で、
  起動直後に押すと消えます(専用 CLI フラグはありません)。
  メニューバー「Pelican」の項目は **7 つ** —— Asset Browser / Object Tree / Inspector /
  Frame Plan Viewer / **Compiled Passes** / Frame Stats / Dear ImGui Demo。
- **Frame Plan Viewer**: フレームプランのノードグラフ可視化(パス = ノード、RT = エッジ。ホイールでズーム、中/右ドラッグでパン、左クリックで選択詳細)。データ源は `get_frame_plan` と同一の公開 JSON のみ — **D0(エディタ特権の禁止)準拠の第 1 実例**です。
- Asset Browserは読み取り専用です。Object Tree / Inspectorの編集は
  ECS/renderer/PhysWorldへ直接触れず、RPCと同じtyped
  `EditorCommandService`を通ります。schema-driven field、live/ticket
  preview、undo/redo、atomic save、watch tokenによる再queryを備えます。

## 10.7 Pelican Studio(devstudio)

**現状 🚧: Qt Widgets の editor shell、別 process engine viewport、Outliner と viewport の
選択同期、render-tools dock(Frame Plan 9 タブ・Features・Graph の 3 タブ)、
ワークスペース配置の永続化、pass の著作フォーム(fullscreen / material / raster と preset pass の保存)、
`features[]` とその scalar パラメータの編集・materialize、共通グラフの閲覧と編集、
Inspector からのグラフ装着まで実装済み。**
起動は `cmake --build ./build --config Debug --target run_studio`。

### `pelican_studio` の CLI

引数は **3 つだけ**です([main.cpp](../../src/devstudio/main.cpp#L7))。

```sh
pelican_studio --project <dir>                 # 起動時にこの project を開く
pelican_studio --project <dir> --tab graph     # render-tools dock のタブを前に出す
pelican_studio --automation <名前>             # test 用の自動化チャネルを開く(Q-1)
```

`--tab` が取るのは `frame_plan` / `render_features` / `graph` の 3 つ
([一覧](../../src/devstudio/view/mainwindow.cpp#L690))。**未知の名前は window を作る前に console へ
理由を出して終了コード 2** です([検査](../../src/devstudio/main.cpp#L36)) —— window を開いて「たまたま前にあった
タブ」を見せると、綴り間違いが成功に見えるためです。`--tab` は `openProject` の**後**に適用されます
([順序](../../src/devstudio/view/uimain.cpp#L71)) —— project を開くとその project のツール配置が復元され、
どのタブが前かが変わるからです。

見た目は **View ▸ Theme ▸ Light / Dark** で切り替えます(✅N-5。CLI の引数ではありません)。選択は QSettings の
`studio/theme` に残り、**再起動なしで**窓と 2 つのノードキャンバスに当たります([第13章 §13.5](13_editor.md))。

`--automation <名前>` は **GUI を外から駆動するための口**です(Q-1)。指定すると `QLocalServer`(Windows では
`\\.\pipe\<名前>`)が開き、**改行区切りの JSON 1 行 = 1 命令**で `list` / `click` / `double_click` / `drag` /
`wheel` / `move` / `press` / `release` / `key` / `type` / `text` / `action` / `menu_list` / `view_state` /
`wait` / `wait_engine` / `rpc` / `open_project` / `screenshot` / `quit` の **20 命令**を受けます
([一覧](../../src/devstudio/automation/automationcommands.cpp#L1802))。**flag が無い起動では server object が
作られないので socket は存在しません**([構築](../../src/devstudio/view/uimain.cpp#L37)) —— 出荷時の挙動は
変わりません。listen できたときだけ `automation listening: <endpoint>` を標準出力に出します
([その 1 行](../../src/devstudio/view/uimain.cpp#L64))。開けなかったときは理由を出して **終了コード 3** です。

`wait_engine` は engine が editor RPC に答えた時点で返ります。engine は**最初の frame を出す前に** RPC に
答えるので、描画済みの frame が要る操作(`capture`、公開済みの frame plan)の前には `frames` を付けます。
`{"cmd": "wait_engine", "frames": 1}` は `get_status` の `render.published_frames` がその数に届くまで待ち、
応答に `published_frames` を足します([commandWaitEngine()](../../src/devstudio/automation/automationcommands.cpp#L1558))。
`published_frames` は renderer がその起動で公開した logical frame の数で、飛ばした frame は数えません。

回す側は [`tools/studio_drive.py`](../../tools/studio_drive.py#L982)(`uv run`、標準ライブラリだけ)です。
Studio を `--automation --project --tab` で起動し、シナリオ JSON(`test/studio_scenarios/<名前>.json`)の step を
順に送り、期待を検査して `report.json` を書きます。

```sh
uv run tools/studio_drive.py --studio build/dist/Debug/pelican_studio.exe \
  --player build/dist/Debug/pelican_player.exe \
  --scenario test/studio_scenarios/graph_read.json \
  --project <project の複製> --var other_project=<もう 1 つの複製> \
  --artifacts out/graph_read --report out/graph_read/report.json
```

report は `verdict`(`green` / `red`)・`summary`(step ごとに 1 行)・`failures`(送受信・理由・失敗時の
スクリーンショット)・`studio_log` を持ちます([組み立て](../../tools/studio_drive.py#L1136))。
最後の `shutdown` step は終了コード 0 での正常終了を検査します。異常終了・終了待ちのタイムアウト・watchdog による
強制終了はシナリオの操作が成功していても失敗です。`studio_exit_code` と `studio_shutdown_forced` に結果を記録します。
書式・命令表・名前の付け方は [README](../../test/studio_scenarios/README.md#L1) と
[第13章](13_editor.md) §13.5「Studio の自動化面」が正です。

✅Q-1b で命令が 7 個増えました(`wheel` / `move` / `press` / `release` / `action` / `menu_list` / `view_state`。
`drag` は `button` を取ります)。`press` / `move` / `release` は押している button を面で共有するので掴んだままの
pan が書け、`key` は `route`(`widget` 既定 / `window`)を取って **`window` のときだけ shortcut map を通ります**。
`list` の各項目は `shortcut` を返し、メニューバー・dock・gizmo の `QAction` は `pelican.file.*` /
`pelican.view.panels.*` / `pelican.gizmo.*` という objectName を持ちます —— `QAction` は widget ではないので
一覧は `menu_list`、発火は `action` です。現在のシナリオと実行条件は §10.9 を参照してください。

> **設計決定(D0: エディタ特権の禁止・2026-07-08):** devstudio は公開契約(rpc / pelican_project / データ形式)の上に建つ 1 クライアントであり、エンジン内部への裏口 API を持たない。編集操作はまず rpc メソッドとして定義し、devstudio はそれを呼ぶだけ。

project を開くと、中央 viewport は同じ project を `--rpc --project <root>` と editor feature
overlay 付きの
`pelican_player` 子 process として起動し、Windows の native HWND を
Qt host へ再親付けします。player が自分の swapchain へ描いて OS が合成するため、pixel readback、
copy、process 間 frame 転送はありません。player が終了しても Studio は残り、`Restart Engine`
から再起動できます。既定では Studio と同じ directory の
`pelican_player.exe` を使います。project、`--rpc`、`--feature-overlay` は Studio が所有し、
`PELICAN_STUDIO_PLAYER_ARGUMENTS` 内の同名指定は除去します。開発時の executable と追加引数の
override は次です:

```powershell
$env:PELICAN_STUDIO_PLAYER = "C:/path/to/pelican_player.exe"
$env:PELICAN_STUDIO_PLAYER_ARGUMENTS = "--gpu-labels --vulkan-validation=on --free-camera"
build/dist/Debug/pelican_studio.exe
```

`--project` はここに書きません。Studio が開いた project から自分で渡すため、同名指定は除去されます。

player のログは **project ごと**に `<project>/.pelican/logs/player.log` へ出ます。Studio が `--log-file` を
付けて起動し([引数の組み立て](../../src/devstudio/viewport/studioplayerarguments.cpp#L67))、ファイルは起動のたびに
切り詰められます。以前は全 player が Studio の cwd の `pelican.log` に追記していたため、別の project や
前回の起動の行が混ざっていました。`PELICAN_STUDIO_PLAYER_ARGUMENTS` に自分で `--log-file` を書いた場合は
それが優先され、Studio は付け足しません。

Studio が viewport の窓を先に壊した場合も player は自分で終わります。埋め込み窓は親と一緒に消え、GLFW の
close flag は立たないので、以前は surface を毎 frame 作り直そうとして(`ErrorSurfaceLostKHR` が数十行)
Studio に kill されるまで回っていました。main loop は Win32 の窓がもう無いことを見て止まります
([Window::process()](../../src/core/os/window.cpp#L318))。

`--free-camera [blender|unity]`、`--feature-overlay`、`--input-action-overlay` は Studio 固有機能ではなく、いずれも同じ
`pelican_player` の公開引数です。したがって素の player や profiler からも同じ起動時 overlay を
使えます。Studio が必要とする bundle の内容(gizmo + picking + GPU timing)は
`engine://features/editor.json` だけが所有し、Studio source は個別 feature URI の一覧を持ちません。
overlay はメモリ上だけにあり、project / scene JSON や入力 profile を保存しません。

viewport の左クリックは child client の物理 pixel 座標を `pick_object` へ送り、応答の
`(scene_id, declaration_index)` を Outliner と Inspector に反映します。Outliner を選んだ方向も
同じ `SelectionModel` を更新し、viewport はその 1 個の状態を非所有参照します。名前は表示にしか
使わないため、無名 object も別々に選択できます。WP264 の時点では枠線や gizmo を描きませんでしたが、
現在は選択と toolbar の Move / Rotate / Scale モードを `set_gizmo` で player へ送ります。

Inspector の `Add component...` / `Ray geometry...` の下、properties の一覧より**上**には **Attached graphs** グループが
あります([置き場](../../src/devstudio/view/inspectorwidget.cpp#L207))。選択オブジェクトを owner とする
`graph.instance` の一覧と **Attach...** / **Detach** / **Refresh**、選択した instance の **Instance inputs**
(port ごとに mode / 著作された値 / 実際に使われた値の 3 列)、そして port ごとの 4 導線 ——
**Control with a curve** / **Connect to another output** / **Open the source** / **Disconnect** です。
**Open the source** は Graph タブを前に出して該当ノードを選択します。手順と RPC は
[第13章](13_editor.md) §13.5「Inspector のグラフ装着」。

Inspectorの各componentにある「Remove component」で、選択objectからcomponentを削除できます。
削除は既存の編集journalを通り、Undo／Redoで取り消し・再実行できます。シーンファイルへの
書き出しにはSaveを使います。生成されたcomponentは対象外で、編集不可・処理待ち・他の編集中は
削除ボタンを無効にします。behaviorが複数ある場合も、押した行のattachmentだけを指定します。

選択は engine/game の共有状態ではなく、各ツールが持つ client-session の注視対象です。engine に
Studio 専用の selection state/RPC を足さず、公開 RPC と `pelican_project` の identity だけから
任意の client が同じ状態を作れる形にしたのが D0 上の理由です。背景への成功 pick は選択を解除します。
Studio 起動時の editor overlay が `picking` feature を用意します。overlay のロード失敗、RPC の失敗、
または応答を Outliner と対応付けられない場合は現在選択を維持し、viewport の警告行、status bar、
Engine Log の三つへ理由を出します。Studio を介さない player は overlay を受け取らないため、project の
purgeability と通常起動の出力は変わりません。

gizmo の押下時は `query_gizmo_handle` を 1 回だけ呼び、掴んだ handle と engine が射影した drag 契約を
Studio 側で保持します。以後の pointer move では hit test をやり直さず、上の単一内積式で選んだ軸へ
拘束した移動・回転・拡縮を行います。新しい押下で問い合わせを置き換えた場合、古い成功・失敗応答は
同じ世代判定で破棄します。
値の反映には Inspector と同じ `open_preview` / `update_preview` / `commit_preview` /
`abort_preview` lease を使います。ドラッグ中は Inspector の対応する数値も追従し、release で undo
可能な authoring 編集として commit されます。選択・モード変更や player 停止では commit せず
abort します。handle 以外を押した場合は通常の object pick へ移ります。

`File > Save Scene` または `Ctrl+S` は commit 済みの authoring document を `save_scene` で保存します。
その後にシーンを開き直しても gizmo 編集は残ります。gizmo の描画機能自体は Studio 起動時の editor
overlay が用意しますが、overlay の feature 一覧を scene や project へ書き戻す経路はありません。

`View > Panels > Frame Plan` の dock(`pelican.framePlanDock`)には **Frame Plan・Features・Graph の
3 つのタブ**が同居します(タブ widget の object name は `pelican.renderTools`。
[構築](../../src/devstudio/view/mainwindow.cpp#L270))。**Graph は新しい dock ではなくこの dock の 3 枚目**です ——
dock を増やすことは「見失う場所」を増やすことなので、同じレンダー設定を語る 3 つの見方を 1 箇所に置いています。

**Frame Plan タブ**は実行中 player の公開 `get_frame_plan` を読む調査面で、**9 枚のタブ**を持ちます ——
logical graph / passes / resources / physical plan / decisions / backends / barriers / materials / raw JSON。
**タブは掴んで並べ替えでき、その順序は保存されます**(下記のツール配置)。

Passes の先頭行を上から読むと実行順が分かり、同じ行に入力／出力 target、
種別(render / compute / anchor / snapshot copy / output transform)が出ます。行を開くと履歴入力、
color/depth attachment の load/store、前後 barrier、material filter、execution resource use が見えます。
Resources、Barriers、Materials では writer/reader、hazard edge、公開された material route を逆引きでき、
上部 filter は折りたたみ配下も検索します。**Physical plan は alias group と scope をそのまま出します。**

**Logical graph は左右 2 面**です。左のキャンバスでノードを選ぶと、右の詳細ペインにそのパスの行が出ます。
境目は掴んで動かせ、位置は保存されます。
**ノードの見た目は Graph タブと同じです(N-3)** —— 見出し帯(名前と種別)+ GPU 計時の行 + 書く target の行 +
読む resource の行で、行の端のソケットは resource の種類ごとに形が違います(image = 丸 / buffer = 四角 /
acceleration structure = 菱形 / swapchain = 六角 / history = 縁の太い丸)。辺はソケット中心をつなぎ、
ラベルは tooltip になりました。**ホイールでカーソル中心のズーム(0.25〜4.0)、中ボタンまたは Space + 左でパン、
`F` と右クリックの Fit view** は 2 つのキャンバス共通です(詳細は[第13章](13_editor.md) §13.5)。

**Features タブが編集面です。**著作 config の `features[]` を読み、engine が返したカタログから選んで
足す・外すができ、**再起動なしで反映されます**。カタログは engine が可用性を判定して返すので、
モジュールの動的生成が要る feature は理由付きで断られます(§6.5)。カタログの各項目には
`source`(`engine` / `package`)が付き、install した package の feature も同じ一覧に並びます。RPC の契約は §13.5。

ボタンは 3 段です。著作一覧の下の行が **Remove selected** / **Parameters...** / **Make editable...**、
カタログの下が **Add selected**、最下段が **Refresh** / **Procedural assets...** / **Tasks...** /
**Fork preset...** / **Region replacements...** / **Callable Records...** / **Scene AS...** / **Apply**
([構築](../../src/devstudio/view/renderfeatureswidget.cpp#L1847))。**Parameters...** は選択 feature の
scalar パラメータを編集し、**Make editable...** は materialize(具体値へ展開した派生を新しい
`project://` ファイルに作り、元の参照をそれに差し替える)です。どちらも詳細は
[第13章](13_editor.md) §13.5。**この dock に export / import や package の UI はありません** ——
`export_render_feature` / `import_render_feature` / `install_package` / `uninstall_package` は
RPC と CLI にだけあります。

**Graph タブ**は共通グラフ(`pelican.graph` root の `nodes` と `definitions`)のノードキャンバスです。
上段に `Scope:`(root と definition)とパンくず、右に `Values from:` と `Refresh`。左がキャンバス、右が
選択ノードの詳細表で、**この 2 面構成は上の Logical graph と同じ形**です。`Values from:` で選ぶのは
「どの instance の値を重ねるか」で、入力 port には `get_graph_values`、**出力 port には `get_graph_node_values`**(AG-3b。definition の中でも効きます)を重ねます。`Live` を入れると 250 ms ごとに読み直し、`Timeline` を開くと選択ノードの出力と owner の animation 状態が時間軸に貯まり、左ドラッグで scrub(`set_time` + 読むだけの描画)できます ——
definition の中のノードの値は `get_graph_node_values`(AG-3a)で engine からは取れますが、
**Studio 側の表示は AG-3b 待ちです**。ソケットの形が port の型を表し、形と色の既定表は
[`graph_socket_styles.json`](../../src/devstudio/resources/graph_socket_styles.json#L1) —— 新しい型を足すのに
C++ を触りません。読むだけでなく**編集もここで完結します**(ソケット間ドラッグで接続、`Delete` で切断・削除、
値の帯のその場編集、見出しのダブルクリックで rename、右クリックでノード追加)。操作の一覧と注意は
[第13章](13_editor.md) §13.5。

このタブが engine へ出せる RPC は型で絞られています。`GraphCanvasReadCapability`
([定義](../../src/devstudio/view/graphcanvasreadcapability.hpp#L31))は汎用の RPC メソッドを**表現できない**形で、
`get_graph_instances`(N-6 からは scope ごとの `layout`、N-8 からは `root_schema` も返す)/ `get_graph_catalog` / `get_graph_values` の 3 つの読みと、編集の
`edit_graph_document`、結果引き取りの `get_edit_result` だけを名前で持ちます。それ以外を要求すると
`The graph canvas cannot issue <method>.` になります。Frame Plan 側も同じ作りで、
`FramePlanReadCapability` と `RenderPassAuthoringCapability`
([pass 著作側](../../src/devstudio/view/renderpassauthoringcapability.hpp#L19))に分かれています。
**D0(§10.7 冒頭)を「気をつける」ではなく型で守るのがこの 3 つの struct です。**

plan は player の RPC 接続直後に1回取得し、それ以後は `Refresh` を押した時だけ更新します。実測で
6,000 行を超える応答を毎 frame 転送・parse・tree 再構築すると editor 自身が診断対象を重くするためです。
render graph の hot reload 後など、変化を確認したい時に明示更新してください。player が未起動または
停止中なら空の tree にはせず、パネル上部に理由と start/restart の案内を表示します。

共通 editor 基盤は WP149〜172 で実装済みです: authoring document、typed query/edit、
CAS/journal、undo/redo、atomic save、snapshot import、watch、isolated preview。Studio の
process/window 結線は WP251、汎用 ID バッファ picking + RPC は WP262、Studio の選択同期は
WP264、Frame Plan パネルは WP269 で入りました。engine の汎用 gizmo feature/RPC は WP274 で入り、
Studio の preview lease を使うドラッグ接続は WP275 で入りました。複数 client WebSocket は
未接続です。起動主体が宣言する editor feature overlay は WP289 で入りました。2026-09 の作業単位は
記号 ID です: Inspector のグラフ装着と 4 導線は P-1(`3611e390`)、feature パラメータと materialize は
P-2a-1c(`e3a2d3d0`)、graph root の節を読む authoring 面と共有部品の export / import は
P-2a-4a + P-2a-3(`7e983ed9`)、package は X-1(`64eae566`)、`get_status.animation` と
`get_graph_node_values` は AG-3a(`32d9a20b`)、Graph タブは N-1(`edafc41d`)/ N-1b(`6c47a8a4`)/
N-2(`f2eccbfd`)、自動化面は Q-1(`c026002d`)、Frame Plan のノードの見た目と 2 つのキャンバス共通の
ズーム・パン・fit は N-3(`d8d7387d`)で入りました。ツール自作の入口は
`pelican_project`、JSON-RPC/`pelican_rpc.py`、ImGui の三つです。

**まだ無いもの(名前で書きます)。**

- **時間軸の view と、definition scope のノード値表示はありません**(AG-3b)。engine 側の RPC
  (`get_graph_node_values`)は先に着地しています。
- **Studio は `get_status` を呼びません。**したがって `get_status.animation` を Studio の画面で見る経路は
  まだありません。
- **画素の一致では見ていません。**シナリオが保存するスクリーンショットは AI と人が見るためのもので、
  hash 比較も録画もありません。`widget` モードの絵に**埋め込まれた engine の窓は写りません**(別 HWND)。
- **`--automation` の口に認証はありません。**flag を付けた起動でだけ開きます。

**自動操作面はもうあります(Q-1)。**`pelican_studio --automation <名前>` + `tools/studio_drive.py` +
`test/studio_scenarios/*.json` で、**実物の窓と実物の engine を使った GUI の e2e** が回ります(上記)。
したがって「装着 → 編集 → 観測 → 保存 → 再読込を実 engine で 1 経路通すテストが無い」も**過去の記述**です ——
`studio_scenario_graph_edit_roundtrip` が値の帯の編集からディスク経由の再読込までを 1 本で通します。
canned reply のハーネスと mock を使う Studio テスト群はそのまま残っています(§10.9)。

**既知の穴: Studio は `projects/raytracing_demo` を開けません。**子 process の player が
`Renderer logical frame requires a compiled render pipeline` で終了します(N-1 の完了記録が記録した穴)。
他の出荷 project と、`pelican.graph` root に変換した project は開けます。

## 10.8 クラッシュシンボルの退避

**ダンプは Windows が勝手に取ってくれるが、Debug build を作り直すと PDB が上書きされ、
取ったダンプが読めなくなる。**実際に **9 件**の studio クラッシュが未解明で、**うち 8 件は対応する PDB が既に無く、おそらく永久に読めない。**

**MSVC の Debug ビルドで `pelican_studio` を作るたび**、実行ファイルと PDB が
ビルド出力の外へ退避される。`pelican_player` は対象外。

```
.pelican/crash-symbols/pelican_studio/generation-<n>-<commit>[-dirty]/
    pelican_studio.exe
    pelican_studio-<commit>.pdb
    manifest.txt
```

`manifest.txt` は `source_commit` / `source_dirty` / `binary_sha256` / `pdb_sha256` を持つ。

**PDB のファイル名にコミットが入り、リンカが `/PDBALTPATH` でその名前を実行ファイルへ焼き込む。**
だからダンプ側から「どのコミットのビルドか」が一意に辿れる ——
**タイムスタンプでは足りない**(同じコミットを作り直すと変わる)。

保持は **8 世代**。変えるなら configure で:

| 変数 | 既定 |
|---|---|
| `PELICAN_CRASH_SYMBOL_ARCHIVE_DIR` | `${CMAKE_SOURCE_DIR}/.pelican/crash-symbols`(**pelican の source tree であって、ゲームプロジェクトではない**) |
| `PELICAN_CRASH_SYMBOL_GENERATIONS` | `8` |

**退避先をビルド出力(`build/` / `dist` / `dist_debug`)の中へ向けると configure が FATAL_ERROR で止まる。**
プロセスがハンドルを掴むとディレクトリごと消せなくなるためである。

HEAD が動くと configure が再実行される。

## 10.9 テスト基盤

- 単体テスト: Catch2 v3(`pelican_define_test`)。**GPU 必須テストは Vulkan デバイス列挙失敗時に `SKIP()`**。
- **GPU テストの直列化(WP385)**: `gpu` label の付く CTest は launcher `pelican_gpu_lease_run`(`test/gpu_lease_run.cpp`)経由で起動し、Windows では machine-wide(`Global\` 名前空間)の lease を 1 本だけ取ります。CTest の `RESOURCE_LOCK pelican_gpu` は 1 つの `ctest` 実行の中しか直列化しないので、**別 worktree の `ctest` と同時に走っても GPU が重ならない**のはこの lease の側です。lease 待ちが締め切りを越えたら skip ではなく失敗(`gpu_lease_timeout@1{holder_pid, waited_seconds}`、exit 124)で、worktree ごとの configure / build / test は [`tools/worktree.sh`](../../tools/worktree.sh) が担当します(§10.10)。既定の lease 名は `pelican_gpu_lease_v1`、既定の待ち時間は **1200 秒**で、`PELICAN_GPU_LEASE_NAME` / `PELICAN_GPU_LEASE_TIMEOUT_SECONDS` で変えられます。**`PELICAN_GPU_LEASE=off` を置くと lease を取らずに素通し**します(対照実験用。off だと 2 process の GPU 区間が実際に重なります)。launcher は子プロセスを Job Object(kill-on-close)に入れてから resume するので、**孫プロセスが取り残されません**。
- 結合テスト: `test/run_*.cmake` が player / cli を子プロセス起動して検証。2026-07-10 以降の追加: `run_devcli_assets`(WP66)/ `run_event_schema_compile`(WP71)/ `run_dump_lowered_material`(WP76)/ `run_devcli_gltf_extract`(WP79)/ `run_spvlink_golden`(WP80)/ `run_devcli_rules_import`(WP84)/ `run_devcli_bake_camera` + `run_input_record_replay_headless`(WP89)/ `run_ui_u2_rpc_replay`(WP93)など。
- 2026-09 の追加: [`run_devcli_graph_catalog.cmake`](../../test/run_devcli_graph_catalog.cmake)(ctest 名 `devcli_graph_catalog`。`pelican_cli graph-catalog` の出力を検査し、**さらに `docs/v0_freeze.md` の操作表と突き合わせます** —— 表は `<!-- graph-op-table:begin -->` / `:end` のコメントで区切られていて、誰も比べない写しを 2 つ目の正にしないための仕掛けです)、`package_install_smoke`(X-1。install → カタログ → 描画 → uninstall を実 player で。`gpu` ラベル)、[`run_project_catalog_smoke.cmake`](../../test/run_project_catalog_smoke.cmake)(出荷 project の走査。`project.json` の `requires` が満たせない project は**名前を出して skip** し、player の中で落ちません)。
- **Studio のシナリオ(Q-1)**: `test/studio_scenarios/*.json` は 1 本が 1 つの ctest `studio_scenario_<名前>` になり
([登録](../../test/CMakeLists.txt#L2908))、`pelican_gpu_lease_run` 経由の **GPU lane** で走ります —— 実物の窓に
実物の player が埋まるので `QT_QPA_PLATFORM=offscreen` は掛けません。Studio target と Python 製テストを有効にし、
Python 3.11 以上を用意します。`uv` は不要です。完全な GPU gate はシナリオの未登録・skip を失敗にするため、
`SKIP_DEVSTUDIO=OFF` と `PELICAN_PYTHON_TESTS=ON` を指定します。[`run_studio_scenario.cmake`](../../test/run_studio_scenario.cmake#L30) が fixture
`test/fixtures/studio_automation_graph` を temp に 2 つ複製し、Qt を PATH に置いて driver を起動します。
対象の一覧は [`test/studio_scenarios/`](../../test/studio_scenarios/) が正本です。成果物(`report.json`、
`studio.log`、スクリーンショット)は `build/test_artifacts/studio_scenarios/<名前>/` に残ります。
- ゴールデンイメージテスト(**`inventory.json` の `cases[]` に登録されたものだけが対象。ディレクトリを置くだけでは走らない**。件数は [第6章](06_rendering.md) §6.10 が正)。
- ctest 非登録のスモーク: `run_build_units_smoke.cmake`(IMGUI / PHYSICS 系を含む単独 OFF ビルド検証。`PELICAN_WITH_ANIMATION=OFF` は [`run_animation_smoke()`](../../test/run_build_units_smoke.cmake#L950) が回し、[`verify_animation_absent()`](../../test/run_build_units_smoke.cmake#L883) が「消えているべきものが消えている」を確かめます)、`run_project_code_smoke.cmake`、`run_package_template_smoke.cmake`(X-1。package 入りの一時 project を configure から build して headless で走らせるので、**primary worktree の依存ソースを** `-DPELICAN_PACKAGE_TEMPLATE_SMOKE_DEPS=<primary>/build/_deps` で渡します)。
- **CI(✅WP137/165)**: push/PRのWindows CPU gateに加え、手動/週次の
  build-unit OFF/Jolt/project-code/clean-clone matrixがあります。GPU testは
  `gpu` labelで除外し、`SKIP`はexact allowlist、retryなし。GPU CI2は未。

## 10.10 リポジトリ保守スクリプト(`tools/`)

`tools/` はエンジンの実行物ではなく、**この repo を触る人(と coding agent)のための道具**です。
どれも production にはリンクされません。

| スクリプト | 何をするか |
|---|---|
| [`pelican_rpc.py`](../../tools/pelican_rpc.py) | Python 3.12 標準ライブラリだけの JSON-RPC クライアント(✅WP160。§10.3) |
| [`worktree.sh`](../../tools/worktree.sh) | worktree ごとの configure / build / test ラッパ(✅WP385) |
| [`codex_bg_build.sh`](../../tools/codex_bg_build.sh) / [`codex_bg_status.sh`](../../tools/codex_bg_status.sh) | 長い build を切り離して起動し、あとから状態を見る |
| [`public_api_inventory.py`](../../tools/public_api_inventory.py) | 公開 context API の exact inventory を生成・照合(✅WP380b/380e) |
| [`doclink.py`](../../tools/doclink.py) | docs 内の `#L<行番号>` リンクを追随させる |

### `tools/worktree.sh` — worktree ごとの configure / build / test

```sh
tools/worktree.sh configure <worktree> [追加の cmake 引数...]
tools/worktree.sh build     <worktree> [--target ...]
tools/worktree.sh test      <worktree> [-R ...]
```

- **依存ソースを primary worktree から借ります。** `git worktree list` の先頭を primary とみなし、
  その `build/_deps/<dep>-src` を `FETCHCONTENT_SOURCE_DIR_*` で指します。**別 worktree の
  `_deps` を掴んだまま消されると configure が壊れ、古いバイナリで偽緑になる**ので、
  この向き(primary から借りる)を守ってください。
- `MSBUILDDISABLENODEREUSE=1` を必ず立てます。並行 build で MSBuild の node が再利用されると、
  **別 worktree の出力が自分のログに混ざります**(exit 0 だけでは何を測ったか分かりません)。
- `configure` は `build/pelican_worktree_manifest.json` に generator / 依存ソース根 /
  node reuse / 引数の fingerprint を書きます。`build` と `test` は**その半分を計算し直して
  食い違ったら名前付きで止まります**(別 generator・別依存根・別 feature 集合の manifest を拒否)。
  manifest が無ければ「先に configure しろ」と言って exit 2 です。
- `PELICAN_WORKTREE_GENERATOR=Ninja` のときだけ、Git Bash が保持できない `vcvars64` を
  `cmd.exe` 側で立ててから CMake を呼びます。

### `tools/codex_bg_build.sh` / `codex_bg_status.sh` — 切り離した build

coding agent のシェルは約 120 秒で切られますが、全体 build はそれより長くかかります
(実際に WP383 / WP385 では 120 秒上限で build が中断し、**変異を当てたはずのバイナリで
測っていない**報告が出ました)。

```sh
tools/codex_bg_build.sh <tag> <command...>   # 起動して即戻る
tools/codex_bg_status.sh <tag> [tail 行数]   # RUNNING か DONE:<exit> と log の末尾
```

ログと終了状態は `build/.codex_bg/<tag>.log` / `.status`(`CODEX_BG_DIR` で変更可)に置かれます。

### `tools/public_api_inventory.py` — 公開 API の inventory

`GameContext` / `GameObjects` / `BehaviorContext` の **public メンバ関数を exact signature で
列挙**し、`test/fixtures/public_api_inventory.json` に書きます。

```sh
uv run tools/public_api_inventory.py           # 照合(古ければ非 0)
uv run tools/public_api_inventory.py --write   # 生成
```

各行は `getter` / `mutator` / `host_entrance` に分類され、**mutator と host_entrance は
[`sceneparticipantmanifest.cpp`](../../src/core/loader/sceneparticipantmanifest.cpp) の runtime manifest に
対応する行が無いと生成が失敗します**。逆に manifest にあって inventory に無い API も失敗です
(双方向。✅WP380e)。ゲーム API に mutator を足したら、scene transaction 側の分類も同時に足す、
という結び付きをこの 1 本が強制しています。

### `tools/doclink.py` — 行番号リンクの追随

docs 内の `../../src/...#L<行番号>` は refactor のたびに腐り、**腐ったリンクは無いリンクより悪い**
(黙って無関係なコードを指すため)ので、行番号は「アンカーが今どこにいるか」のキャッシュとして扱います。

```sh
uv run tools/doclink.py check     # 検証(腐っていたら非 0)
uv run tools/doclink.py update    # 行番号を書き直し、台帳を更新
uv run tools/doclink.py audit
uv run tools/doclink.py update --staged   # git index にある docs だけ(pre-commit フック)
```

アンカーは 2 段です。リンクテキストが C++ 識別子(`Foo::bar()`)ならその**定義行**を探し直し、
そうでなければ [docs/link_anchors.json](../link_anchors.json) が覚えている**行のテキスト**を探し直します。
どちらでも決められないときは**書き換えずにそのリンクを名指しで報告**します。uv が interpreter を
用意するので、システムの Python は要りません。

## 関連文書

- [../external_tools_requirements.md](../external_tools_requirements.md) — 外部ツール契約(R1〜R10。JSON-RPC は R8)
- [../design_asset_hot_reload.md](../design_asset_hot_reload.md) — アセットホットリロード(v2.1・HR0〜HR2-G + targeted animation generation 実装済み)
- [../design_game_logic_native.md](../design_game_logic_native.md) — ゲームロジック(G2 DLL リロード実装済み)
- [../design_devstudio_direction.md](../design_devstudio_direction.md) — エディタの方向性(D0〜D3)
- [../design_build_tiers.md](../design_build_tiers.md) — ビルドユニット・配布
- [第2章 ビルドと起動](02_getting_started.md) / [第6章 レンダリング](06_rendering.md) / [第7章 入力と UI](07_input_ui.md) / [第8章 ゲームロジック](08_gameplay.md)
