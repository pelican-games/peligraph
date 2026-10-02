# 第10章 ツールリファレンス

対象: pelican2(2026-09-17 時点、HEAD=`82054c05`。2026-09-25 に §10.3 の移行の 2 method と移行専用 endpoint、§10.4 の `project migrate` を MG-1(`28bf3b40`)に合わせて追記)/ このマニュアルはコードを正とする

## この章で学ぶこと

- `pelican_player` の全 CLI 引数(**30 個**。正は [main.cpp](../../src/player/main.cpp) の `add_argument` 群)
- JSON-RPC(stdio)による外部制御 — 基盤メソッド、エディタ拡張、実セッション例
  - **JSON-RPC** は外部プロセスからエンジンを操る制御プロトコル、**stdio** はその通り道です: 起動した `pelican_player --rpc` の標準入力へリクエスト行を書き、標準出力から応答行を読みます(1 行 = 1 リクエスト / 1 応答の NDJSON。詳細は §10.3)
- `pelican_cli` の **11 サブコマンド系統**(`assets` / `audio` / `bake-camera` / `image` / `import` / `dist-config` / `package` / `project` / `dump-lowered-material` / `vrm` / `graph-catalog`。正は [main.cpp の usage](../../src/devcli/main.cpp#L51))
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
| `--feature-overlay <uri>` | なし | `pelican.render_feature_overlay` v1 の feature bundle(= `schema` / `version` / `name` / `features` だけを持つ小さな JSON。`features` の各要素は feature 参照文字列か `{ref, parameters}` — [renderfeatureoverlay.cpp](../../src/project/renderfeatureoverlay.cpp#L17))を**この起動だけ**重ねる。公開 CLI なので Studio 以外の profiler 等も利用可能 |
| `--headless` | off | ウィンドウなし実行(時刻は固定ステップになる) |
| `--rpc` | off | stdio JSON-RPC モード。**`--headless` は不要になりました**(✅WP156): ヘッドレスでは stdin を読み切るまでブロッキング、**ウィンドウモードではフレーム境界処理 + 有界キュー(既定 64 件)**。溢れると `busy` エラーを即返します。※`--rpc` を付けると ImGui UI は無効([第13章](13_editor.md)) |
| `--embedded` | off | 別 process の窓に埋め込まれる前提で起動する(✅EW-1、[引数](../../src/player/main.cpp#L348))。render 窓を `GLFW_VISIBLE` / `GLFW_FOCUSED` / `GLFW_FOCUS_ON_SHOW` = `GLFW_FALSE` で作る(非表示・focus 無し。見せるのは埋め込む側 — [window.cpp](../../src/core/os/window.cpp#L199))。project の `basic_config.fullscreen` が true でも無視して `basic_config.window_size` の窓で作り、log に `--embedded: the project's fullscreen setting is ignored; the window is created windowed` を 1 行出す。この引数を読むのは窓の作成だけで、時刻や game の動きは通常の窓あり起動と同じ。`--headless` との併用は引数の誤り(stderr に `--embedded cannot be combined with --headless` と usage、終了コード -1 — [併用の拒否](../../src/player/main.cpp#L472))。Studio が viewport の engine に渡す(✅EW-2)。窓の目印は下の段落 |
| `--frames <n>` | 3 | ヘッドレスのフレーム数。**0 = 無制限** |
| `--size WxH` | 1280x720 | ヘッドレスの描画サイズ |
| `--render-out <path>` | なし | PNG 出力先(sRGB エンコード)。`%04d` 等で毎フレーム連番 |
| `--fps <f>` | 60.0 | ヘッドレス固定ステップのレート(dt = 1/fps) |
| `--start-time <秒>` | なし | **ヘッドレス専用**。この時刻の frame から `--frames` 枚を出す(✅O-1)。`llround(秒 × fps)` で **parse 時に frame 番号へ量子化**され、engine には「frame の間の時刻」が渡りません([parseStartFrame()](../../src/player/main.cpp#L326))。`--headless` が無ければ `offline.headless_required@1`、負・非有限・数でない綴りは**すべて** `offline.time_invalid@1`、`--rpc` / `--replay` / `--record-input` / `--bake-camera-output` との併用は `offline.mode_conflict@1`([検査](../../src/player/main.cpp#L627))。`--frames` は併用です |
| `--start-state <edit\|playing>` | `playing` | 起動時の再生の状態(✅PM-1b、[引数](../../src/player/main.cpp#L368))。`edit` は behavior を眠らせ(instance を作らない)、game の時計を止め、editor camera を用意して、RPC の `set_play_state` を待つ(`--free-camera` が無ければ `blender` の free camera とその overlay を足す。`--editor-transform` は足さない)。`--replay` / `--record-input` / `--bake-camera-output` / `--start-time` / `--rpc` の無い `--headless` との併用は `offline.mode_conflict@1: --start-state edit cannot be combined with <引数>`([併用の拒否](../../src/player/main.cpp#L621))、他の値は `--start-state must be one of: edit, playing`(どちらも引数の誤りとして usage を出して終了コード -1)。edit 状態の中身は [第13章](13_editor.md) §13.5 の「edit 状態」。Studio は ✅PM-3b から `edit` を渡す(第13章 §13.1「Studio で game を再生・停止する」) |
| `--dump-frame-plan` | off | 解決済みフレームプラン JSON を **stderr** へ出力([第6章](06_rendering.md)) |
| `--gpu-labels` | off | Vulkan debug object 名と command label を有効化。validation layer の選択とは独立([第6章](06_rendering.md) §6.14) |
| `--vulkan-validation <off\|on>` | `_DEBUG`: on / それ以外: off | Khronos validation layer と synchronization validation を一体で選択。要求時に `VK_LAYER_KHRONOS_validation` が無ければ名前付き hard error |
| `--allow-absolute-paths` | off | CLI 由来のコンテンツ参照に限り絶対パスを許可。JSON 内の絶対パスは常に拒否 |
| `--user-dir <dir>` | なし | `user://` ルートの差し替え |
| `--log-file <path>` | なし | この起動のログを `<path>` に書く。親 directory は作り、**起動ごとに切り詰めます**(1 ファイル = 1 回の実行)。省略時は従来どおり cwd の `pelican.log` に追記します([main.cpp](../../src/player/main.cpp#L379)、[log.cpp](../../src/core/log.cpp#L34))。Studio は project ごとの log をこれで渡します(§10.7) |
| `--ignore-engine-version` | off | `engine_min_version` 不適合を hard error → WARN に降格 |
| `--play-seq <path.jsonl>` | なし | `pelican.transform_seq` の再生(`PELICAN_WITH_SEQPLAYER` ビルドユニットが ON のビルドでのみ使えます — [第2章](02_getting_started.md) §2.2) |
| `--seq-mesh <builtin:sphere\|path.glb>` | builtin:sphere | transform_seq 全オブジェクトに使うメッシュ |
| `--seq-loop` | off | クリップをループ再生 |
| `--play-vat <path.glb>` | なし | `pelican.vat` 入り GLB の再生(VAT ユニット必須) |
| `--camera "px,py,pz,tx,ty,tz,fov_deg"` | なし | カメラの位置・注視点・垂直 FOV(度)の上書き |
| `--game-logic <path.dll>` | (exe 同階層) | ゲームロジック DLL の明示指定(✅WP90) |
| `--strict-assets` | off | assets manifest の差分を起動エラーに昇格(✅WP66。ホットリロードも無効化)。配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`)では、起動時の照合はこれを付けたときだけ走ります(✅AV-1、§10.4 の `assets`) |
| `--record-input <path.jsonl>` | なし | 順序付き入力を `pelican.input_seq` **v2** で収録(✅WP89。GP-4 から v2 で、ヘッダに窓座標 → framebuffer ピクセルの換算 `content_scale` / `framebuffer` を持つ — [第7章 §7.3](07_input_ui.md)) |
| `--replay <path.jsonl>` | なし | input_seq のリプレイ。v2 と v1 を読む(v1 は content scale 1 として読む)。`--record-input` と排他、ホットリロード自動無効(✅WP89) |
| `--input-profile <name>` | project.json の既定 | アクティブ入力プロファイルの上書き(✅WP91) |
| `--input-action-overlay <uri>` | なし | `pelican.input_action_overlay` v1 の action/profile bundle を**この起動だけ** project 入力へ重ねる |
| `--free-camera [blender\|unity]` | off (`blender` が省略時 preset) | project 入力と並存する runtime-only の `orbit` camera と同梱入力 overlay を有効化 |
| `--editor-transform [blender\|grab]` | off (`blender` が省略時 preset) | runtime の transform 編集アクションを有効化。`grab` は handle のドラッグだけを残す |
| `--xr off\|auto\|on` | off | OpenXR(PCVR)起動(✅WP125。`auto` = 不在なら flat 続行 / `on` = 不在・headless・rpc・リプレイでは名指しエラー — [第2章](02_getting_started.md)) |
| `--bake-camera-output <path.jsonl>` | なし | リプレイ中のカメラ軌跡を transform_seq v1 で出力。`--headless` + `--replay` 必須(✅WP89) |
| `--audio-backend device\|offline\|null` | 環境変数 `PELICAN_AUDIO_BACKEND`、それも無ければ窓あり = `device` / ヘッドレス = `offline` | 音のバックエンドを名指しする(✅AU-3、[読み取り](../../src/player/main.cpp#L479))。**引数があると環境変数は読まない**。空の環境変数は未設定の扱い。未知の値は `audio.backend_unknown@1` で起動を止める。`null` はデバイスを開かない(退避ではないので `get_status` の `audio` に `fallback_reason` は付かない)。`device` / `offline` は開けなければ `null` へ退避する。`PELICAN_WITH_AUDIO=OFF` の build では `null` だけを受け付け(`offline` / `device` は `This binary was built with PELICAN_WITH_AUDIO=OFF: --audio-backend <値> is unavailable`)、環境変数は読まない。詳細は [第8章 §8.11](08_gameplay.md) |

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

**overlay は `pelican.graph` root の 1 段下に着きます。**overlay は `rendering.features` に積まれます ——
root 直下に積んでも lowering が読まないからです。`rendering` オブジェクトを持たない graph root は名前付きで断られます
(`render_config.section_unlocated@1`)。`pelican.graph` root でない rendering config(旧い legacy root)は、
MG-2b から overlay の前に `render_config.legacy_root@1` で断られます([分岐](../../src/project/renderfeatureoverlay.cpp#L140))。

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

**render 窓の目印(✅EW-1、Windows)**: 窓あり起動の render 窓には、`--embedded` の有無に関係なく常に Win32 の窓 property `PelicanEngineRenderWindow`(値 1)が付きます。窓を作った直後に `SetPropW` で付け([付ける所](../../src/core/os/window.cpp#L218))、窓を壊す前に `RemovePropW` で外します([外す所](../../src/core/os/window.cpp#L309)。埋め込む側が親ごと窓を壊した後なら外すものは無い)。名前の定数は [`Pelican::engineRenderWindowProperty`](../../src/core/os/enginewindowproperty.hpp#L15) で、engine の型に依らない header だけのファイルなので、engine を link しない埋め込み側も include できます。埋め込む側は「その process の、owner の無い top-level 窓で、この property を持つもの」を、見えているかどうかを問わず探します(`--embedded` の窓は非表示で作られるため。GLFW の helper 窓には property が付きません)。Studio がこの規則で窓を探す仕組みは [第13章](13_editor.md) の「engine の窓は Studio の外に出ません(✅EW-2)」の段落を見てください。なお `get_status` の `input.cursor.embedded`(§10.3)はこの引数を見ず、`--rpc` の窓が `WS_CHILD` にされているかで決まります([判定](../../src/core/os/window.cpp#L348))。

test は `ew1_engine_window_embedded`([engine_window_embedded.py](../../test/engine_window_embedded.py)。Windows かつ `PELICAN_WITH_RPC` の build の GPU test): `--embedded --headless` が断られること、fullscreen を指定した project を `--embedded` で開くと property 付きの窓がちょうど 1 つあり、player の top-level 窓はどれも見えず、client 領域が `window_size` の大きさで、非表示のまま frame が進み、`ShowWindow` の後も同じ窓で frame が進み、log に上の 1 行があること、`--embedded` 無しでは同じ property 付きの窓が見えていることを見ます。

※ `--play-seq` / `--seq-mesh` の相対パスは現状 cwd 基準で解決されます(既知の食い違い。[第11章](11_status.md))。

## 10.3 JSON-RPC(stdio)— 外部制御プロトコル(✅実装済み)

実装: [rpcserver.cpp](../../src/core/communication/rpcserver.cpp)。プロトコルの正本は [../external_tools_requirements.md](../external_tools_requirements.md) R8。

### 枠組み(不変)

- stdin/stdout の NDJSON(1 行 = 1 つの JSON-RPC 2.0 リクエスト)。**stdout はプロトコル専用**(ログは cwd の `pelican.log`。`--log-file` を付けたときはそのファイル)。
- batch・notification 非対応。エラーコードは標準 + アプリケーションエラー -32000、RenderDoc capture -32010。ハンドラ内例外は応答に変換されプロセスは落ちません。EOF で正常終了。
- 1 プロセス 1 クライアント。**ゲーム DLL リロード中は全メソッドがアプリケーションエラーで拒否**されます。

> **設計決定(決定性):** 同一の RPC スクリプトを 2 回実行したとき、応答列は(instance_id を除き)**バイト単位で完全一致**しなければならない。CI で検証済み。`instance_id` だけが除外されるのは、それが RPC エンドポイント生成時に振られる UUID v4 で、起動ごとに必ず変わる値だからです([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L2782))。

### 基盤メソッド一覧

| メソッド | params | 動作 |
|---|---|---|
| `get_status` | `{}` | `{instance_id, project_root, scene, frame, time, seed, renderdoc, diagnostics:{renderdoc:{...}}, debug_utils:{...}, vulkan_validation:{layer, available, enabled, synchronization, reason}, stores, input:{..., cursor:{requested, effective, embedded}}, reload:{...}, participants:{game_logic:{...}}, sprite:{...}, color:{...}, render:{published_frames}, startup:{...}, xr:{active, reference_space, floor_semantics, ...}, animation:{available, sinks:[{owner, node, source, cursors:[{clip, time_seconds, normalized_phase}], frame_revision, pose_hash, state?, transition?, root_delta?}]}, audio:{available, backend, fallback_reason?, voices:[{node, state, position_seconds}]}, assets_verification:{state, ...}, preview:{available, reason?}}`(`preview` は ✅PV-1 で、単発 preview(`render_preview`)が今の世代で使えるか。使えなければ `reason: {code: "preview.unsupported_authoring@1", feature, target?, graph?, pass?, cause?, message}`。[第6章](06_rendering.md) §6.13 の PV-1 の段落。`assets_verification` は ✅AV-1 で、起動時の asset 検証の要約。形は下の「起動時の asset 検証の結果」。`animation` は AG-3a。animation module の無い build では `{available: false}` だけ。`audio` は AU-1 で、`backend` は `device` / `offline` / `null`、`voices` は ✅CT-1 で graph の `audio.playback` の node ごとの `idle` / `playing` / `paused` と音源の秒、audio を外した build では `{available: false}` だけ。`gpu_timing` の `nodes` は ✅ST-1 から最後の 1 frame の sample だけで、node ごとの平均は `node_averages`([第6章](06_rendering.md) §6.14)。[第8章](08_gameplay.md) §8.11)※rpc は常に flat 駆動のため `xr.active=true` は rpc からは観測できない |
| `set_seed` | `{seed}` | 決定的乱数のシード設定 |
| `set_time` | `{t}` | 仮想時刻の直接設定(dt=0) |
| `step_frame` | `{}` | headless 専用で、描画のフレームを 1 つ進める(評価の番号が 1 増える)。game が進むのは `playing` のときだけ。窓ありでは `step_frame_windowed` で断る(第 13 章 §13.5) |
| `render_frame` | `{}` | 時刻を進めず描画のみ |
| `capture_gpu` | `{}` | `render_frame` と同じcurrent-time描画を1回だけ**RenderDoc**(GPU に送った描画コマンドを 1 フレーム分丸ごと記録して後から追える外部デバッガ)で captureし、実際に増えたindexのcanonical `.rdc` pathを返す |
| `capture` | `{path}` | 直近フレームを PNG 保存(sRGB。応答に `encoding:"srgb"`) |
| `get_frame_plan` | `{}`、または `{loop_elements: {node, offset?, limit?}}`(✅LP-3b。draw_loop の要素の paging) | フレームプラン JSON(fullscreen / raster pass の node には shader の set 1 の宣言 `pass_inputs` が付く。✅VI-1 / VI-1b。LP 系からは最上位の `loops[]`、node と resource の `loop_origin`、node の `view_select` / `view_axes` / `no_selected_view` も。✅LC-1 から、shader が `pelican_loop.glsl` の回の番号を読む pass / task の node に `reads_loop_index: true`。✅PS-1 から、`schedule: "once"` の node に `schedule: "once"`・`last_run_generation`・`last_run_frame`(まだ走っていなければ `null`)。✅IA-1 から、`physical_target_plan.resources[]` の `initial: {asset}` を持つ image の要素に `initial_asset: {asset, sha256}`(`sha256` は `assets.manifest` の値、載っていなければ `null`。✅AB-1 から、asset の隣に `.import.json` があれば `derived_from` / `stale` / `stale_reasons` も)。✅PV-1 から最上位に `preview: {available, reason?}`(`get_status` の `preview` と同じ形)。形は[第 13 章](13_editor.md) §13.5「ループと分解の RPC」) |
| `rerun_once` | `{node?}`(✅PS-1。`node` は空でない文字列。ほかの鍵は `-32602`) | `{scheduled: [<pass / task の名前>...]}`。`schedule: "once"` の node を次の frame で走らせる。`node` を省くと今の frame plan の once の全部、graph の node の id ならその node が作った once、`graph.loop` の id なら全回の once。per_frame の node は `[]`。frame plan に無い名前は `-32602` で `data` が `{code: "node_unknown", node}`([`rerun_once` の登録](../../src/core/communication/rpcserver.cpp#L2449)、[`Renderer::requestOnceRerun()`](../../src/core/vkcore/renderer.cpp#L4094)。走る条件は[第 6 章](06_rendering.md) §6.18「一度だけ走る pass と、frame をまたぐ image」) |
| `pick_object` | `{x, y}` | 最後に完了した **ID バッファ**(色ではなく「そのピクセルに写っているオブジェクトの ID」を書き出す描画出力。`picking` feature が宣言する `R32_UINT` のレンダーターゲット `picking_id` — [picking.json](../../src/core/resources/features/picking.json))の 1 ピクセルを同期読み出し。`engine://features/picking.json` が必要 |
| `set_gizmo` | `{selection:GizmoSelection\|null, mode}` | 選択 transform のギズモ表示を設定/解除。`engine://features/gizmo.json` が必要 |
| `query_gizmo_handle` | `{selection:GizmoSelection, mode, x, y}` | 指定した選択・mode・物理 pixel にある handle を状態なしで問い合わせ。同 feature が必要 |
| `load_gltf` | `{path, name?}` | glb の一時ロード(プロジェクト相対のみ) |
| `load_scene` | `{name}` | シーン切替。応答は `{name, committed, effect_errors[]}`(✅WP380b/380e。commit 後の副作用の失敗は error ではなく result に出ます — [第13章](13_editor.md) §13.13) |
| `set_camera` | `{name}` | シーン内カメラへ切替 |
| `raycast` | `{origin, direction, max_distance}`(3 鍵ちょうど) | 最近傍 1 件を `{hit: {name, collider_id, shape_ordinal, distance, position, normal, trigger, one_way}}`、当たらなければ `{hit: null}`([受け口](../../src/core/communication/rpcserver.cpp#L1239))。`PELICAN_WITH_PHYSICS=OFF` の build では `hit: null` ではなく JSON-RPC error(code `-32000`、`data` = `{code: "physics.module_unavailable@1", requirement: "physics", option: "PELICAN_WITH_PHYSICS"}`。✅PG-2b)。provider が 0 本の build も `-32000`(message は `physics.provider_unavailable@1` の例外文。[第8章](08_gameplay.md) §8.4「機能を外した build」) |
| `update_transforms` | `{objects, transforms}` | 次フレーム境界で適用。`rot` は名指し拒否(`rotation`) |
| `inject_input` | `{events:[...]}` | 入力注入。`key_*` / `mouse_*` / `axis` + **`gamepadButtonDown` / `gamepadUp` / `gamepadAxis`**(リプレイ中は拒否)。`mouse_move` の `x` / `y` は**窓座標**で、OS からの移動と同じ event になる。`ctx.input().pointer()` と UI の入力ルータがフレームの換算で framebuffer ピクセルにする(headless は 1 倍。✅GP-4、[注入の受け口](../../src/core/communication/rpcserver.cpp#L862)) |
| `inject_event` | `{type, payload}` | 登録済みゲームイベントの emit。payload は EventPayloadSchema で事前検証(✅WP71) |
| `reload_game_logic` | `{}` | ゲームロジック DLL を即時リロード(✅WP90) |
| `reload_shaders` | `{}` | shader を読み直して公開する(✅F-5。file watch が閉じている `--rpc` の下でも、明示の要求は通る。入力と宣言が合わなくなった pass はその pass だけ前の shader で描き続け、`refused_passes` に名が出る。✅VI-1b。返り値とエラーは[第 13 章](13_editor.md)の表) |
| `get_assets_verification` | `{}` | 起動時(または最後の再検証)の asset 検証の全件(✅AV-1。下の「起動時の asset 検証の結果」) |
| `verify_assets` | `{}` | asset 検証を worker で走らせ直し、すぐ `{state: "running"}` を返す(✅AV-1) |
| `update_assets_manifest` | `{store}` | その store の manifest を今のディスクから書き直し、再検証を始める(✅AV-1) |
| `set_input_profile` | `{name}` | 入力プロファイル切替(✅WP91) |
| `start_input_record` | `{path}` | input_seq 収録開始(✅WP89。v2 で書く) |
| `stop_input_record` | `{}` | 収録停止。`{path, frames, events}` |
| `start_input_replay` | `{path}` | リプレイ開始(fixed-step 化・リロードゲート閉鎖) |
| `stop_input_replay` | `{}` | リプレイ停止 |
| `export_scene_snapshot` | `{schema_version:2, allow_pending?:false}` | current scene を含む全 **authoring document**(= 編集の正本としてメモリ上に持つシーン文書。シーンファイル 1 つにつき 1 文書で、ディスクの実物とは別 — [第13章](13_editor.md) §13.2)の deterministic semantic bytes、SHA-256、revision、`prefab_closure` を返す。`schema_version` は `2` だけ(SnapshotV1 は MG-2b で撤去。`1` は `unsupported_snapshot_version`) |
| `import_scene_snapshot` | `{schema_version:2, semantic_scene_bytes, digest, current_scene_id, prefab_closure}` | disk を上書きせず、検証済み snapshot を in-memory scene source として reload する |
| `plan_project_migration` | `{}`(省略可) | 前の版の文書を移す計画(dry-run。何も書かない)。`{project_root, required, engine_version, saved_by_engine, documents:[{path, schema, from, to, steps, digest, manual_items}], expected:{<path>: <sha256>}}`(✅MG-1。[第3章 §3.13](03_project_format.md)) |
| `apply_project_migration` | `{expected:{<path>: <sha256>}}`(plan が返した `expected` をそのまま) | 移行を 1 つの transaction で適用。応答は plan の形 + `{applied, backup, saved_by_engine_after, restart_required}`。`expected` が今の文書と食い違えば何も書かずに `project.migration_plan_stale@1`。`expected` が無い / オブジェクトでない / 値が文字列でないものは `-32602` |

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
| 読み取り | `scene_tree` / `scene_outline`(component を含まない軽い一覧)/ `get_components` / `get_scene_revision` / `list_assets` / `query_journal` / `can_edit` / `can_preview` |
| セッション | `open_editor_session` / `resume_editor_session` |
| 編集 | `edit` / `undo` / `redo` / `get_edit_result` |
| チケットプレビュー | `open_preview` / `update_preview` / `commit_preview` / `abort_preview` / `get_preview_result` |
| 隔離プレビュー | `eval_preview` / `render_preview` |
| 保存 | `save_scene` |
| 共通グラフ | `get_graph_instances` / `get_graph_catalog` / `get_engine_shaders`(✅EL-4b)/ `get_shader_interface`(✅EL-5)/ `get_registered_providers`(✅EL-6)/ `get_graph_values` / `get_graph_node_values` / `edit_graph_document` / `edit_graph_instances` |
| 共有部品と package | `export_render_feature` / `import_render_feature` / `install_package` / `uninstall_package` |

既存メソッドの変化: `get_status` に `scene_source`(シーンは**ホットリロード対象外**・保存は `save_scene`)が追加、`step_frame` の応答に `edit_results[]` が追加、`load_scene` / `reload_game_logic` / `start_input_replay` は進行中のプレビューを強制中止します。
2026-09 の追加(詳細は [第13章](13_editor.md) §13.13): `get_status` に `participants.game_logic`
(`error_code` / `error_fields`。**起動時の初期化失敗も未試行の状態で出ます**)、`load_scene` と
`import_scene_snapshot` の result に `committed` / `effect_errors`、`get_components` の各
component に `resolution_error`。

2026-09 の追加(GP-4、`c095493c`): `get_status` の `input` に `cursor {requested, effective, embedded}`([組み立て](../../src/core/communication/rpcserver.cpp#L1735))。`requested` はゲームコードが `ctx.input().setCursorMode` で頼んだモード、`effective` は窓に適用したモード(`normal` / `hidden` / `locked`)、`embedded` は窓が別の窓に埋め込まれているか(`--rpc` かつ `WS_CHILD`。Windows だけで判定)。埋め込みでは `locked` を頼んでも `effective` は `normal` です。headless は窓が無いので `effective` は常に `normal`、`embedded` は `false`。

2026-09 の追加(AG-3a、`32d9a20b`): `get_status` に `animation`(上の表)。新メソッド `get_graph_node_values{instance?}` → `{instance, generation, frame, published, source:"rendering", nodes:[{node, expanded, outputs:{port: value}}]}`(publish 時の snapshot、再評価しない。`instance` 省略で root 側の展開木全体、rendering domain の node は `outputs: {}`、publish 前は `published: false`)。**animation domain の node は空ではありません** —— 自分の値は出しませんが、そのフレームに**食べたスカラー**が values 側に `$pose/<node>/<port>` として publish されるので、**受けた入力ポートの名前で**並びます([renderconfigeditor.cpp:2010](../../src/core/communication/renderconfigeditor.cpp#L3723)、[graphAnimationScalarOutput()](../../src/project/graphpartition.cpp#L461))。読み取り専用で `edit_*` の CAS には乗らない。

**`get_graph_values` と `get_graph_node_values` は問いが違います。**前者は `instance` 必須で、その
**instance の入力 port だけ**を `{port, value, source, connected|parameter}` として返します(`source` は
`connected` / `parameter` / `default` / `override`)。後者は**展開木のノードを全部**並べ、各ノードの出力値を
`outputs` に入れます([実装](../../src/core/communication/renderconfigeditor.cpp#L3642))。両方とも
**同じ publish snapshot を読むだけ**で再評価しません —— 再評価は graph の時計を進め event を二重に消費するためです。
`source: "rendering"` は「これは描画側の評価が使った値だ」という明示で、animation phase は同じ文書を
自分の state で評価するため、event や trigger で駆動される文書では両者が恒久的に違いうることを表します。

2026-09 の追加(グラフ・共有部品・package): `get_graph_instances` / `get_graph_catalog` / `get_graph_values` /
`edit_graph_document` / `edit_graph_instances`([登録](../../src/core/communication/editorrpchandlers.cpp#L267))、
`export_render_feature` / `import_render_feature`(`user://components/<name>/` 経由の共有部品)、
`install_package` / `uninstall_package`(`project://packages/<ns>/` への vendoring)。params と返り値、
`ops` の 5 種と `scope`、名前付きの拒否は [第13章](13_editor.md) §13.5 が正です。

2026-10 の追加(IA-2 / SC-1 / EL-6。形の正は [第13章](13_editor.md) §13.5):

- 新メソッド `get_registered_providers {}`(✅EL-6、`5582bb93`)→ `{draw_sort, pass_implementation, region_replacements, graph_transforms, render_strategy}`。各値は `[{name, source: "engine" | "game", provider_version}]` を名前順で、呼ぶたびに registry から読みます([登録 `get_registered_providers`](../../src/core/communication/editorrpchandlers.cpp#L302))。
- `reload_render_documents` の committed の結果に `changed_assets`(✅IA-2、`cdcdebc1`)。image の `initial` の asset の中身だけが変わったときも組み直して公開し、その asset を辞書順で並べます。何も変わらなければ `no_change: true`・`changed_assets: []`。
- `get_components` / `scene_tree` の `schema.fields[]` と `get_component_add_candidates` の `schema_fields[]` に、camera の `controller.*` の `discriminant` / `applies_to` / `required_for` / `default`(✅SC-1、`14e10b19`)。`controller.type` は `enum` `["orbit", "follow", "fly"]` になりました。
- `get_render_features` の `feature_instances[].parameter_schema` の宣言に `engine_default`(✅SC-1)。materialize 済みの feature で、元の `default` を持つ宣言だけです。

2026-10 の追加(CT-3 / MP-1。形の正は [第13章](13_editor.md) §13.5 と [第6章](06_rendering.md) §6.17・§6.18):

- `get_graph_catalog` の各 op に `parameters: [{name, kind, required?, alternatives?, default?, enum?, min?, max?, items?, fields?, expression?, description?}]` と `parameters_complete`(✅CT-3、`0d215a68`)。catalog 本体の鍵なので `pelican_cli graph-catalog` にも出ます。`parameters_complete` が `false` の op は `render.image` / `render.fullscreen` / `render.raster` / `render.compute` / `render.material_pass` です。
- 名前付きの拒否 `graph.render_node_parameter_invalid@1 {node, op, parameter, expected, reason: type | enum | range, allowed? | min? / max?}`(✅CT-3)。編集の rejected には CT-2 の `document` も付きます。
- `get_graph_catalog` の `material_pass_contracts` の各 contract に `screen_inputs` / `surface_resources`(`[{name, source_type}]`。✅MP-1、`afd923e1`)。CLI には出ません。
- `get_frame_plan` の material pass の node に `drawn_materials: [{material?, id, draws}]`・`drawn_materials_frame`、256 件を超えたら `drawn_materials_truncated: true`(✅MP-1)。最後に公開した logical frame で、その pass が描いた material です。

2026-10 の追加(FR-1 / FR-2 / AB-1。形の正は [第13章](13_editor.md) §13.5、[第6章](06_rendering.md) §6.5・§6.8・§6.18、[第5章](05_assets.md) §5.8):

- `list_render_features` の各 feature に `required_targets: [{name, type, usage?}]`(✅FR-1、`45ca769b`)と `insertion: [{pass, anchor, position}]`(✅FR-2、`0b15fb62`)。feature を足す前に、pipeline に要る target と挿入先が分かります。
- `get_render_authoring_context` に `snapshot_after_candidates`(✅FR-1)。`rendering.snapshots` の `after` に書ける名前です。
- 名前付きの拒否 `feature.required_target_missing@1 {feature, target, type, document}`・`snapshot.after_unknown@1 {snapshot, after, resolved?, candidates}`(✅FR-1)、`feature.insertion_anchor_missing@1 {feature, anchor, candidates, document}`(✅FR-2)。`graph.target_ref_type_mismatch@1` に `expected: {type, layers?}`(✅FR-1)。
- 新メソッド `bake_audio_analysis {asset, out, settings?}`(✅AB-1、`892a1455`)→ `{ticket, status: "accepted", asset, out}`。worker で焼き、`get_edit_result` の committed に `{operation: "bake_audio_analysis", asset, out, manifest, layout}`([登録 `bake_audio_analysis`](../../src/core/communication/editorrpchandlers.cpp#L261))。
- 新メソッド `list_derived_assets {}`(✅AB-1)→ `[{asset, manifest, tool, source, derived_from, stale, stale_reasons}]`(配列)。asset store の `*.import.json` を全部読みます([登録 `list_derived_assets`](../../src/core/communication/editorrpchandlers.cpp#L264))。

2026-10 の追加(CT-4 / FR-3。形の正は [第13章](13_editor.md) §13.5、[第6章](06_rendering.md) §6.5・§6.17・§6.18):

- `get_graph_catalog` の各 op の `parameters` の要素に `choices_from?`(`{rpc, key, field?}` か `{graph_nodes: [<op>...]}`)・`asset_kind?` / `extensions?`・`soft_min?` / `soft_max?`、kind `union` と `variants`、各 op に `placement: "anywhere" | "owned_instance"`(✅CT-4、`4d682338`)。catalog 本体の鍵なので `pelican_cli graph-catalog` にも出ます。
- `get_render_authoring_context` に `view_families` と `canonical_anchors`(✅CT-4)。`choices_from` が指す一覧です。
- 名前付きの拒否 `graph.op_requires_owned_instance@1 {node, op}`(✅CT-4)。`pose.*` と `value.morph_weight` を `owner` 付きの `graph.instance` の外に置いた。編集の rejected には CT-2 の `document` も付きます。
- `list_render_features` の `required_targets` の各要素に `needs_writer`(✅FR-3、`55390f4b`)。名前付きの拒否 `feature.required_target_unwritten@1 {feature, target, type, writer_candidates: [{node?, pass, port, resource}], document}`(✅FR-3)。
- 新メソッド `edit_batch {base_source_digest, edits: [{method, params}]}`(✅FR-3)→ `{ticket, status: "accepted", base_source_digest, methods}`。文書を書き換える 20 の編集 method を順に 1 つの候補へ当て、組み立てと preflight を 1 回だけ行う 1 つの ticket です。当てられない edit は `batch_index` 付きで断ります([登録 `edit_batch`](../../src/core/communication/editorrpchandlers.cpp#L343))。

2026-10 の追加(IA-3 / CT-5 / PV-1 / scene_outline。形の正は [第13章](13_editor.md) §13.5・§13.7、[第6章](06_rendering.md) §6.5・§6.13・§6.17・§6.18):

- `reload_render_documents` の比較に、`value.sample_table` が読む表(KTX2)と隣の `.import.json` の中身(✅IA-3、`26473e75`)。`changed_assets` には表の参照で出ます(manifest だけが変わったときも表の名前)。
- `get_graph_catalog` の `choices_from` に条件 `where` と 3 つ目の形 `{asset_manifest: {parameter, manifest, key}}`、`render.fullscreen` の `push_constants` が enum(✅CT-5、`65f3a8a3`)。`get_render_authoring_context` に `acceleration_structures` と `render_target_vocabulary.format_class_patterns`。`get_graph_instances` に `definition_instances {<definition>: {owned_instances_only, instances: [{id, op, owned}]}}`。
- 名前付きの拒否 `graph.render_scalar_target_incompatible@1 {node?, target, expected: {host_source, size?}, actual}`(✅CT-5。graph の中の buffer は partition で、graph の外の buffer は登録の段で。後者は `node` 無し)。`edit_graph_instances` は graph の名前付きの拒否を、その code と CT-2 の `document` のまま返します(前は `invalid_graph_attachment`)。
- `get_status` と `get_frame_plan` に `preview: {available, reason?}`、render の編集の committed の結果にも `preview`(✅PV-1、`a173e70c`)。`render_preview` は preview が使えない世代で `preview.unavailable@1 {method, reason}`。preview だけが組めない編集は断られず committed になります。
- 新メソッド `scene_outline {scene_id?, if_scene_revision?}`(`b29ccfea`。利用者の codex の変更)→ `{scene_id, scene_revision, not_modified, objects?: [{authoring_object_id, declaration_index, name?, parent_authoring_object_id}]}`。component を含まない authoring の一覧で、`if_scene_revision` が今の revision と同じなら `objects` を省きます([登録 `scene_outline`](../../src/core/communication/editorrpchandlers.cpp#L77))。

### 起動時の asset 検証の結果(✅AV-1)

player が起動時に行う assets manifest の照合([第3章](03_project_format.md) §3.4 の「起動時検証」)の結果は、log に出るだけでなく store ごとに engine に残ります([`AssetsVerification`](../../src/core/loader/assetsverification.hpp#L83))。読み方と走らせ直し方は次の 4 つです(handler は [`configureAssetsVerificationRpcHandlers()`](../../src/core/communication/rpcserver.cpp#L1477))。Studio にこれを見せる画面はまだありません(AV-1s)。

**`get_status.assets_verification`**(軽い要約。[`statusJson()`](../../src/core/loader/assetsverification.cpp#L204))

| `state` | 形 |
|---|---|
| `verified` | `{"state":"verified","stores":1,"matched":1,"info":1,"warnings":2,"errors":0}`。件数は store ごとの issue を severity で数えたもので、起動時の log の要約行 `assets manifest: N stores, ...` と同じ数 |
| `not_run` | `{"state":"not_run","reason":"<理由>"}`。理由は 3 つの決まった文言: `"no asset store declares a manifest"`(manifest を宣言した store が無い。manifest なし = 検証なしなので正常)/ `"PELICAN_WITH_HOT_RELOAD=OFF"`(配布 build で `--strict-assets` なしに起動した)/ `"this host does not verify assets"`(player の起動経路を通らない host) |
| `running` | `{"state":"running"}`。`verify_assets` の再検証の最中 |

**`get_assets_verification {}`** → 全件([`detailJson()`](../../src/core/loader/assetsverification.cpp#L238))。

```json
{"state":"verified","verified_at":"2026-10-02T10:19:15Z",
 "stores":[{"name":"main","root":"<store の root>","manifest":"<manifest の path>",
            "matched":1,"hashed":1,"cached":0,
            "issues":[
              {"kind":"content_mismatch","severity":"info","file":"a.bin","message":"asset content differs: a.bin (...) - run `pelican_cli assets manifest` to accept the update"},
              {"kind":"missing","severity":"warning","file":"b.bin","message":"asset is missing: b.bin"},
              {"kind":"extra","severity":"warning","file":"c.bin","message":"asset is not listed in the manifest: c.bin"}]}]}
```

- `kind` は `content_mismatch` / `missing` / `extra` / `case_mismatch` / `unreadable` / `invalid_manifest`。`severity` は小文字の `info` / `warning` / `error` です(CLI の `INFO` / `WARNING` とは表記が違います)。`--strict-assets` で起動した engine では全部 `error` になります。
- `file` は store の root からの相対 path です。宣言した manifest が解決できない store は `manifest: null` で、issue は `invalid_manifest` 1 件(`file` は空)です。
- `hashed` / `cached` は、中身の照合で hash したファイルの数と hash cache が答えたファイルの数です。
- `verified_at` は UTC の ISO 8601(秒まで)。`not_run` では `reason` が付き、`verified_at: null`、`stores: []` です。
- `running` の間は `state: "running"` と、**直前の結果**(`verified_at` と `stores`)を返します。件数が多くても全部返します。

**`verify_assets {}`** → `{"state":"running"}` をすぐ返し、module 専用の worker thread で起動時と同じ検査(中身の照合込み、起動時と同じ strict、同じ hash cache)を走らせます([`verify()`](../../src/core/loader/assetsverification.cpp#L298))。終わると `get_status` / `get_assets_verification` の結果が置き換わります。完了の通知は無いので、`get_status.assets_verification.state` が `running` でなくなるのを待って取り直します。

- 走っている間にもう一度呼ぶと、新しく走らせず `{"state":"running"}` を返します(合流)。
- manifest を宣言した store が無い project では走らせず `{"state":"not_run","reason":"no asset store declares a manifest"}` を返します。
- 再検証は log を出しません。配布 build でも明示の要求として走り、終わると `verified` になります。
- 照合する store の構成は起動時のままです(`asset_stores` を書き換えたら engine を起こし直します)。

**`update_assets_manifest {"store": "<name>"}`** は「今のディスクを正とする」操作です。`pelican_cli assets manifest` と同じ書き方(hash cache は `<project>/.pelican/assets-hash-cache.json`、`--full` なし)でその store の manifest を書き、INFO を 1 行(`manifest main: 2 files (1 hashed, 1 cached) -> <path>`、CLI と同じ文言)出して、`verify_assets` と同じ再検証を始めます([`updateManifest()`](../../src/core/loader/assetsverification.cpp#L313))。返り値は `{"store":"main","manifest":"<path>","files":2,"hashed":1,"cached":1,"state":"running"}` です。書き込みは handler の中で同期に行い、再検証が走っている最中ならそれが終わるまで待ってから書きます(その間は frame も止まります)。

params は `get_assets_verification` / `verify_assets` が空の object だけ、`update_assets_manifest` が `store`(空でない文字列)だけで、それ以外は `-32602` です。断るときは `reload_shaders` と同じ形(`code: -32000`、`data: {error_code, error_fields}`)です。

| `error_code` | message の例 | `error_fields` |
|---|---|---|
| `assets_store_unknown@1` | `update_assets_manifest: no asset store is named 'nope'` | `{"store":"nope"}` |
| `assets_manifest_undeclared@1` | `update_assets_manifest: asset store 'plain' declares no manifest in project.json` | `{"store":"plain"}` |
| `assets_manifest_write_failed@1` | `update_assets_manifest: cannot write the manifest of asset store 'main': <理由>` | `{"store","manifest","reason"}`(宣言した manifest が解決できない store では `{"store","reason"}`) |
| `assets_verification_unavailable@1` | `verify_assets: this host does not verify assets` | `{}` |

### 移行の 2 method と移行専用 endpoint(✅MG-1)

`plan_project_migration` / `apply_project_migration`([実装](../../src/core/communication/projectmigrationrpc.cpp#L50))は
`pelican_cli project migrate` の RPC 版です(流れと名前は [第3章 §3.13](03_project_format.md))。失敗は application error(-32000)で、
`error.data` が `{error_code, error_fields}`(例: `error_code: "project.migration_plan_stale@1"`、`error_fields: {expected, actual}`)です。

- **ふつうに動いている engine** も 2 つに答えます([登録](../../src/core/communication/rpcserver.cpp#L1858))。移行の要る project はそもそも入口で止まるので、ここに来るのは空の plan(`required: false`)です(動いている間に文書が前の版に戻された場合を除く)。
- **移行専用 endpoint**: `--rpc` の player が開くときの入口で `project.migration_required@1` に当たると、engine を起動せずに
  この 2 method **だけ**に答える JSON-RPC endpoint になります([`runProjectMigrationRpcServer()`](../../src/core/communication/projectmigrationrpc.cpp#L98)、
  [player 側](../../src/player/main.cpp#L741))。ほかの method は application error で、message は入口が出した
  `project.migration_required@1: {"documents":[…]}` です。apply が済んだ後は `project.migration_restart_required@1: {"message":"the project was migrated; start a new player to open it"}` に変わります。
  入力が終われば終了し、終了コードは apply が行われたら 0、行われなかったら 1 です。移した project は新しい player で開き直します。
  stderr には入口の 1 行(`project.migration_required@1: …`)が先に出ます。
- Studio はまだこの endpoint を扱いません(移行の画面は GUI session の別 WP)。

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
parse/semantic → `prefab_closure` の照合 → current scene の順で行います。`schema_version` は `2` だけで、
`1`(MG-2b で撤去した SnapshotV1)は他の鍵を見る前に `unsupported_snapshot_version` です。失敗時は `PelicanRpcError.data` の
`code` が `unsupported_snapshot_version` / `snapshot_too_large` /
`digest_mismatch` / `snapshot_invalid` / `prefab_dependency_mismatch` / `scene_not_found` のいずれかになり、
`snapshot_invalid` には `detail` も入ります。

### snapshot を使った安全なスイープ

人が操作している session の document を scratch process へ immutable に渡し、候補値だけを
人 session へ戻す公式 sequence は次のとおりです。snapshot 自体を merge/save する API は
ありません。

```python
from tools.pelican_rpc import PelicanRpc, PelicanRpcError

with PelicanRpc("projects/mygame") as human:
    exported = human.export_scene_snapshot(
        {"schema_version": 2, "allow_pending": False}
    )
    revision = exported["scene_revision"]  # R
    payload = {
        "schema_version": 2,
        "semantic_scene_bytes": exported["semantic_scene_bytes"],
        "digest": exported["digest"],
        "current_scene_id": exported["current_scene_id"],
        "prefab_closure": exported["prefab_closure"],
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
pelican_cli <assets|audio|bake-camera|image|import|dist-config|package|project|dump-lowered-material|vrm|graph-catalog> ...
```

エラーは stderr+終了コード -1、成功は 1 行サマリ+終了コード 0。

### `pelican_cli project init <dir> [--template standard|blank]`(✅WP57 / ✅F-2)

空(または未存在)ディレクトリに雛形一式を生成します。生成直後に `pelican_player --project <dir>` でそのまま起動できます。詳細は [第2章](02_getting_started.md)。
空でないディレクトリは `project init target directory is not empty: <dir>` で拒否し、何も書きません。

雛形の中身は [`src/project/projecttemplate.cpp`](../../src/project/projecttemplate.cpp) の 1 箇所にあり(`pelican_project`)、
CLI は `--template` を読んで [`initializeProjectTemplate()`](../../src/project/projecttemplate.cpp#L401) を呼ぶだけです
([CLI 側](../../src/devcli/projectinit.cpp#L30))。Studio の File > New Project... も同じ関数を呼びます([第13章](13_editor.md))。
`--template` は 2 つです。

- **`standard`**(既定): 以下で説明する 3D シーン向けの 16 エントリ。F-2 の前に `project init` が書いていたものと
  byte 単位で同じでした(`devcli_project_templates` が SHA-256 で固定)。MG-1 から `project.json` だけが 1 行違い、
  `engine_min_version` の次の行に `"saved_by_engine": "0.1.0"`(書いたエンジンの版。[第3章 §3.2](03_project_format.md))が入ります。
- **`blank`**: `standard` と同じ 16 エントリ(✅VI-2 までは `shaders/present.frag` を足した 17 エントリでした)。違うのは
  `passes/main_rendering_config.json` の `rendering` 節だけで、`pipeline` キーが無い **direct config** です
  (preset も `features` も `render_targets` もありません)。中身は fullscreen pass `present` 1 本
  (`input: []`、`output.color: ["display"]`、vertex `engine://fullscreen`、fragment `engine://passthrough`)で、
  雛形は shader を書きません(`shaders/` もありません)。`engine://passthrough` は入力がすべて可変の engine の shader で、
  入力が 0 本なら UV の gradient を、入力があれば入力 0 をそのまま書きます(✅VI-1 / VI-2)。`present` に target を
  読ませるだけなら shader を書かずに済みます
  ([雛形](../../src/project/projecttemplate.cpp#L229))。engine は direct config にも canonical anchor と
  `display` を読む `output_transform` を足すので、描かれる frame は「project が書いた pass 1 本 + engine の終端」です。
  scene / input / ui / code / asset は `standard` と同じです(3D の model は置きません)。

知らない名前は `unknown project template: <name> (expected standard|blank)` で拒否します。

生成される `passes/main_rendering_config.json` は **`pelican.graph` root** です(両方の雛形で同じ)。`standard` で著作した内容
(`pipeline.preset` = `engine://render_pipelines/hybrid_v1.json`、`features` = `shadow_directional` と
`sky_ambient` の 2 本)は `rendering` オブジェクトの中に入ります。雛形の文字列は graph root をそのまま書いたもので
([standard](../../src/project/projecttemplate.cpp#L200) / [blank](../../src/project/projecttemplate.cpp#L229)。
[選ぶ関数](../../src/project/projecttemplate.cpp#L262))、以前それを包んでいた `wrapLegacyRenderingConfig()` は MG-2b で撤去しました
(`standard` の生成物は撤去の前と同じバイト列で、`run_devcli_project_templates.cmake` の digest も変わっていません)。`render_targets` / `rendering_passes` は preset が持つので
project は書きません —— `run_devcli_project_init.cmake` は**まず root が `pelican.graph` で `rendering` が
object であることを確かめてから**、節の中に禁止メンバが無いことを見ます
([検査](../../test/run_devcli_project_init.cmake#L48)。順序が逆だと、節ごと消えたときに検査が素通りします)。

生成される `code/CMakeLists.txt` は 2 行で、`pelican_game_sources(game.cpp)` の**後に**
`pelican_packages()` を呼びます([template](../../src/project/projecttemplate.cpp#L275))。順序が意味を持ちます ——
package の object は、その前に作られたゲームロジック DLL へ合流するからです。

### `pelican_cli project migrate --project <dir|project.json> [--dry-run]`(✅MG-1)

前の版のエンジンで作った project の文書を、このエンジンの版へ移します。流れ(検出 → plan → apply → 退避先)と
名前付きの失敗は [第3章 §3.13](03_project_format.md) が正です([CLI 側](../../src/devcli/projectmigratecommand.cpp#L38))。

- `--project` は project のディレクトリか、その中の `project.json`。どちらでもないものは
  `--project must point to a directory holding project.json, or to project.json: <path>`、`project.json` 以外の名前のファイルは
  `--project file must be named project.json: <path>` で拒否します。
- `--dry-run` は plan だけを出して何も書きません。付けなければ plan と apply を 1 回で行います(CAS の `expected` は渡しません)。
- 結果は **stdout に JSON**(2 space の字下げ)で、終了コード 0。失敗は stderr に `<名前>: <fields JSON>` を 1 行出して終了コード -1。
  この章の冒頭の「成功は 1 行サマリ」の例外です。
- **apply(`--dry-run` 無し)は、その project を開いている engine と Studio をすべて閉じてから実行します(✅MG-1b)。** apply は最初に
  project の transaction のディレクトリ(`.pelican/render-authoring.transaction`)を自分で作って取り、既にあれば何も読まず・消さずに
  `project.migration_transaction_busy@1: {"path": …, "message": …}` で止まります。message は「engine と Studio を閉じてから apply し直す。
  残骸なら project を開けば解消する」という内容です。使用中と落ちたプロセスの残骸は区別できないので、どちらもこの名前です
  (RPC の `apply_project_migration` も同じ。詳細は [第3章 §3.13](03_project_format.md) の 4.)。`--dry-run` は plan だけなので取りません。

v0 の engine は手順を 1 本も登録していないので、今日はどの project でも次の形になり、何も書きません
(`project init` 直後の project での例。コードから組み立てたもので、実行して写した出力ではありません):

```json
{
  "project_root": "C:/work/my_game",
  "required": false,
  "engine_version": "0.1.0",
  "saved_by_engine": "0.1.0",
  "documents": [],
  "expected": {},
  "dry_run": true
}
```

`--dry-run` を付けないときは、この後ろに `"applied": false, "backup": null, "saved_by_engine_after": "0.1.0"` が入り、
最後が `"dry_run": false` になります。移すものがあるときは、`documents[]` の各要素が
`{path, schema, from, to, steps:[手順の id…], digest, manual_items:[{code, pointer, message}…]}`、`expected` が
書き換える全文書と `project.json` の `{path: sha256}`、apply の後は `applied: true`・`backup: ".pelican/migration/<時刻>"`・
`saved_by_engine_after` がこのエンジンの版になります。`manual_items` が空でない文書は、自動で直せなかった箇所を
`pointer`(RFC 6901)の場所で手で直します。`saved_by_engine` は project.json に無ければ `null` です。

### `pelican_cli import ...`(✅WP21/79/84)

DCC 納品物の取り込み。3 つの形があります:

- `import <delivery_dir> --project <dir>` — `pelican.import` manifest の sha256 照合 → asset_data.json へ glb 登録(冪等)
- `import gltf --extract-scene <glb> ...` — glTF のノード階層を scene v2(`parent` 付き)として抽出(✅WP79。MG-2b から v2 を書く。[第5章](05_assets.md))
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

✅AH-1(`0f93b5b8`)から、CLI の生成・照合と起動時の検証は file を 1 MiB ずつ読んで SHA-256 を計算し、Windows では BCrypt(CNG)で計算します(digest は同じなので、今の manifest と `.pelican/assets-hash-cache.json` はそのまま使えます)。起動時の検証の時間と、その前に出る 1 行は [第 3 章](03_project_format.md) §3.4 の「起動時検証」です。

✅AV-1(`5f7c5cdb`)から、起動時の検証の結果は engine が store ごとに持ち、**RPC で読めます**(`get_status.assets_verification` の件数、`get_assets_verification` の全件。§10.3 の「起動時の asset 検証の結果」)。engine を止めずに照合し直す `verify_assets` と、今のディスクを正として manifest を書き直す `update_assets_manifest {store}`(この CLI の `assets manifest` と同じ書き方)もあります。**配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`。dist-config が常に出す)では起動時に照合しません**。照合するのは `--strict-assets` を付けたときだけで、そのときは開発の build と同じに照合し、食い違えば起動を止めます(2026-10-02 利用者決定)。

### `pelican_cli bake-camera --replay <path.jsonl> --project <dir>`(✅WP89)

リプレイを headless 実行し、カメラ軌跡を `imports/pelican-camera/<name>/` に `camera.transform_seq.jsonl` + `pelican.import` manifest として着地させます(DCC への逆輸出)。

### `pelican_cli dump-lowered-material [--project <dir>] <surface> [material.json] [material-name]`(✅WP76)

`.surface` の lowering 結果(std140 レイアウト・生成 GLSL)を stdout に出力します。「B は C の糖衣」の検証・デバッグ用です。
`material.json` を渡すとその material で lowering します(文書に material が複数あれば `material-name` も要ります)。

**`--project <dir>`(✅G-4)** を先頭に付けると、`<dir>/project.json` の `basic_config.rendering_config_json` が指す rendering
文書を読み、その root の `material_route_default` で lowering します(engine が起動時に読むのと同じ値。
[project の既定を読む所](../../src/devcli/materialcommand.cpp#L33))。`"forward"` の project では automatic な material の `route:` 行が
`reason=default_forward` になります([第6章 §6.7](06_rendering.md))。付けなければ `"auto"` です。この option にはまだ test がありません。

### `pelican_cli dist-config <project> [--with rpc,seqplayer] [--out <file>]`(✅WP41)

> **設計決定(配布は宣言から導出):** 配布ビルドの機能フラグを手で並べさせない。プロジェクトの内容を走査して必要な `PELICAN_WITH_*` を導出し、根拠コメント付きの CMake キャッシュプリセットとして書き出す。

- `PELICAN_WITH_VAT` — GLB に `pelican.vat` extras が実在すれば ON / `PELICAN_WITH_EXR` — `.exr` 参照があれば ON
- `PELICAN_WITH_RPC` / `PELICAN_WITH_SEQPLAYER` / `PELICAN_WITH_OPENXR` — 既定 OFF。`--with rpc,seqplayer,openxr` で明示 ON
- **`PELICAN_WITH_IMGUI` — 常に OFF を書き出す**(配布ビルドに開発 UI を含めない)
- **`PELICAN_WITH_RENDERDOC` — 常に OFF を書き出す**(配布ビルドにcapture integrationを含めない)
- **`PELICAN_WITH_HOT_RELOAD` — 常に OFF を書き出す**(✅AV-1。配布ビルドはファイルを監視せず、起動時の assets manifest の照合も `--strict-assets` のときだけ。コメントは `# PELICAN_WITH_HOT_RELOAD: OFF - distribution builds watch no files and check assets when they are baked`。[書き出す所](../../src/devcli/distconfig.cpp#L884))。OFF の player の挙動は §10.5 の「配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`)」

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
型・操作・sink を知るための口で、`get_graph_catalog` RPC が返すのと同じ値です(✅EL-4 から RPC はこれに `material_pass_contracts` / `material_output_formats` を足して返します。EL-4 で同じく足した `engine_shaders` は ✅EL-4b から別の RPC `get_engine_shaders` が返します。CLI はモジュールのカタログだけを印字し、この 2 つも engine の shader の一覧も出しません。[第13章](13_editor.md) §13.5)。op ごとの `required_parameters`(✅CT-1)と `parameters` / `parameters_complete`(✅CT-3)、`placement` と parameter の `choices_from` などの CT-4 の鍵(✅CT-4)はモジュールのカタログの側の鍵なので、CLI にも出ます([第6章](06_rendering.md) §6.17「catalog の読み方」)。

```sh
pelican_cli graph-catalog
```

モジュールは既定 build で **5 本**(`pelican.value` / `pelican.audio` / `pelican.render` / `pelican.pose` / `pelican.ui`)、
`PELICAN_WITH_ANIMATION=OFF` では `pelican.pose` が、`PELICAN_WITH_AUDIO=OFF` では `pelican.audio` が消えて 1 本ずつ減ります。
`pelican.value` / `pelican.render` / `pelican.ui` の 3 本は常に登録されます(`pelican.ui` は GP-3 で加わった `ui.value` / `ui.visible` の sink。[第7章](07_input_ui.md) §7.4)。この個数と、各操作が
`domain` / `evaluation` / `change` / `sink` を持つことは
[`run_devcli_graph_catalog.cmake`](../../test/run_devcli_graph_catalog.cmake#L24)(ctest 名 `devcli_graph_catalog`)が
固定しています。出力はモジュール名・操作名・型・sink のどれも**名前順**です([catalog()](../../src/project/graphmodule.cpp#L713) が名前を鍵にした `std::map` を走り、型と sink は明示的に sort します)。登録順に依存しません。

### `pelican_cli image ktx2 --out <file.ktx2> --format <VK の名前> (--raw <file> --width W --height H | --hdr <file.hdr>)`(✅IA-1)

非圧縮で mip 1 段の 2D の KTX2 を書きます(`c6690eba`。[`runImageCommand()`](../../src/devcli/imagecommand.cpp#L118))。graph の `render.image` の `initial: {asset}`([第6章](06_rendering.md) §6.18「persistent の image を画像資産から始める」)に渡す float の画像を、外の KTX2 の道具なしで作るための口です。拡大縮小と変換はしません。

```sh
pelican_cli image ktx2 --out lut.ktx2 --format R32_SFLOAT --raw lut.raw --width 64 --height 64
pelican_cli image ktx2 --out sky.ktx2 --hdr sky.hdr
```

- `--format` は VkFormat の名前から `VK_FORMAT_` を除いた綴りで、`R8G8B8A8_UNORM` / `R8G8B8A8_SRGB` / `R16_SFLOAT` / `R16G16_SFLOAT` / `R16G16B16A16_SFLOAT` / `R32_SFLOAT` / `R32G32_SFLOAT` / `R32G32B32A32_SFLOAT` のどれかです。BC の format と知らない名前は断ります。
- `--raw` は texel の bytes(little endian、行は詰める)で、`--format`・`--width`・`--height` が要ります。bytes の数が `width × height × texel の bytes` と違えば書きません。
- `--hdr` は Radiance の `.hdr` を stb_image で `R32G32B32A32_SFLOAT` に読み、大きさは file から採ります。`--format` は省けて(既定 `R32G32B32A32_SFLOAT`)、ほかの format と `--width` / `--height` は断ります。
- `--raw` と `--hdr` はどちらか 1 つだけです。
- 成功すると `ktx2 <format> <W>x<H> -> <out>` の 1 行で終了コード 0、失敗は stderr に理由で -1 です。
- 書いた file は engine の KTX2 の読み込みで読めます([第5章](05_assets.md) §5.2「テクスチャ・画像と atlas」の KTX2 の対応範囲)。test: `ia1_ktx2_cli_test` の [「IA-1 pelican_cli image ktx2 writes --raw and --hdr files the loader reads」](../../test/ia1_ktx2_cli_test.cpp#L97)。

### `pelican_cli audio analyze --asset <file> --out <file.ktx2> [options]`(✅AB-1)

音(WAV / Ogg Vorbis)を解析して、rms・peak・帯域の値の表(`R32_SFLOAT` の KTX2)と、隣の manifest `<out>.import.json` を書きます(`892a1455`。[`runAnalyze()`](../../src/devcli/audiocommand.cpp#L33))。RPC `bake_audio_analysis` と同じ関数を呼び、同じ settings を受けます。表の形・manifest・settings の既定と範囲は [第5章](05_assets.md) §5.8「音の解析を焼いた表」です。

```sh
pelican_cli audio analyze --asset sounds/tone.wav --out tables/tone.ktx2
pelican_cli audio analyze --asset sounds/tone.wav --out tables/few.ktx2 --features rms,bands --bands 4 --hop-ms 20 --channels left
```

| option | settings の鍵 |
|---|---|
| `--features rms,peak,bands` | `features`(カンマ区切り) |
| `--bands N` | `bands` |
| `--hop-ms MS` / `--window-ms MS` | `hop_ms` / `window_ms` |
| `--attack-ms MS` / `--release-ms MS` | `smoothing.attack_ms` / `smoothing.release_ms` |
| `--channels mix\|left\|right` | `channels` |

- 数の option は JSON の数として読むので、整数は整数のまま RPC と同じ検査を通ります。数でない文字は `<option> must be a number: <文字>` です。
- `--out` の拡張子が `.ktx2` でなければ断ります。`--asset` と `--out` は project の参照ではなく file の path です。
- 成功すると stdout に `{"out", "manifest", "layout"}` の JSON(字下げ 2)を出して終了コード 0。名前付きの拒否は stderr に `<code> <fields の JSON>`(`audio.analysis_settings_invalid@1` など)、ほかの失敗は理由の文で、どちらも -1 です。settings で断ったときは何も書きません。
- `PELICAN_WITH_AUDIO=OFF` の build の `pelican_cli` は `audio.analysis_unavailable@1 {"reason":"built_without_audio"}` で断ります。
- `pelican_cli` は engine を link しないので、engine の decode(`src/core/audio/sounddecode.cpp`)と bake(`audioanalysis.cpp`)を自分の source に入れ、miniaudio の実装を自分の unit(`src/devcli/miniaudioimpl.cpp`)で持ちます([`src/devcli/CMakeLists.txt` の AB-1 の部分](../../src/devcli/CMakeLists.txt#L28))。
- test: `ab1_audio_analysis_test` の [「AB-1 a bake of 440 Hz then silence: rms and peak follow the tone and the band holding 440 Hz leads」](../../test/ab1_audio_analysis_test.cpp#L151)(`runAudioCommand()` を直接呼ぶ)。

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

- **有効条件**: ウィンドウモードのみ(既定 ON・専用フラグなし)。リプレイ / `--strict-assets` / rpc 駆動中は中央ゲートで無効。headless では自動監視しません。✅HW-1 から、project.json に `hot_reload`(監視する素材とフォルダの配列)を書くと、**列挙したものだけ**を監視し、rpc 駆動中(Studio の engine)でもそこは監視します。空の配列なら監視しません。リプレイ / `--strict-assets` / headless は宣言があっても無効です。書き方・拒否・`--rpc` の下で反映されないもの(render 文書)は [第3章](03_project_format.md) の「hot reload で監視するもの」。
- **配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`)✅AV-1**: dist-config が常に OFF を出す build(§10.4)では、ファイルを監視せず(FileWatcher を作らない)、game logic の DLL の更新も見に行きません。`get_status.reload` は `state: "disabled"`、`error` と `runtime."pelican.shaders".gate_reason` は `"hot reload build disabled"` で、この理由は他のどの理由よりも先に出ます([gate の 1 か所](../../src/core/watch/reloadgate.cpp#L49))。project.json の `hot_reload` は読みます(形の拒否は同じ)が効かず、宣言があれば起動時に INFO を 1 行 `project.json hot_reload is ignored: this binary was built with PELICAN_WITH_HOT_RELOAD=OFF` 出します([出す所](../../src/core/watch/reloadservice.cpp#L894))。F5・`reload_game_logic`・`reload_shaders` の明示の reload は AV-1 で変えていません(`reload_shaders` を断るのは今も replay と `--strict-assets` だけ。OFF の build で実際に走らせての確認はしていません)。起動時の assets manifest の照合も `--strict-assets` のときだけになります(§10.4 の `assets`)。ソースは build から外していません(FileWatcher のコードは binary に残ります)。
- **fail-soft**: 壊れたファイルを保存しても旧リソースで動き続け、名前入り WARNING と `get_status.reload.last_reload_error` に出ます。修正して保存し直せば回復します。

### ファイル監視の走査(✅FW-1)

監視は「変わったファイルだけを読む」形です([`FileWatcher`](../../src/core/watch/filewatcher.hpp#L69))。

- **走査**は directory の一覧の size と更新時刻(mtime)を前回と比べるだけで、hash(SHA-256)するのは size か mtime が違うファイルだけです。変更の無い polling の tick は 1 ファイルも hash しません。
- **起動時はどのファイルも hash しません。**そのため、起動後に各ファイルを最初に保存したときだけは、中身が前と同じでも 1 回 reload されます(engine は起動時の中身を知らないため)。2 回目からは同じ中身の保存は無視されます。
- mtime が走査の時刻から **100 ms 以内**のファイルは size / mtime を信用せず、次の走査で読み直します(同じ時刻の刻みの中で同じ大きさに書き直された保存を取りこぼさないため。[`settled_after`](../../src/core/watch/filewatcher.cpp#L104))。FAT(刻み 2 s)や時計のずれた network share では、同じ刻みの中に同じ大きさで 2 回保存すると 2 回目を取りこぼすことがあります。
- project 直下の **`.pelican/`**(shader cache・hash cache・transaction・log)は走査せず、そこへの書き込みだけの通知では走査も起きません。
- 走査が終わるたびに、hash したファイルがあれば(起動後の最初は 0 件でも)INFO を 1 行出します: `file watcher: scanned <件数> files, hashed <件数> (<x.x> MB) in <ms> ms`([出す所](../../src/core/watch/filewatcher.cpp#L756))。
- 報告の測定(Debug、`projects/example` の複写 416 ファイル・85.4 MB)では、起動時の最初の走査が 19,176 ms から 5.5 ms に、52.6 MB の `.glb` を 1 byte 書き換えてから reload の待ち行列に入るまでが 17,010 ms から 91 ms になりました(FW-1 の commit の記述と台帳)。

`get_status.reload` で監視の様子が見えます([`ReloadService::statusJson()`](../../src/core/watch/reloadservice.cpp#L1413))。

| field | 中身 |
|---|---|
| `state` | `disabled` / `reconciling` / `watching` / `polling` / `degraded` |
| `epoch` | gate の世代(開閉のたびに進む) |
| `error` | 監視の異常、または閉じている理由(`"rpc driver"` / `"hot_reload empty"` / `"replay"` / `"strict assets"` / `"shader_hot_reload disabled"`、✅AV-1 の配布 build では `"hot reload build disabled"`)。無ければ `null` |
| `applied` / `failed` / `last_reload_error` | 適用した数・失敗した数・最後の失敗 |
| `participants` / `runtime` / `model_catalog` | 登録された参加者、runtime の参加者の状態、model の目録 |
| `watch` | ✅HW-1。正規化した `hot_reload` の宣言。書いていなければ `null`、空の配列なら `[]` |
| `scans` / `hashed_files` / `hashed_bytes` | ✅HW-1(数えるのは FW-1)。起動からの累計: 終わった走査の回数・hash したファイルの数・hash した byte 数。watcher がまだ無ければ 0 |

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
  - ✅TS-1 から、環境変数 **`PELICAN_SHADER_CACHE_DIR`**(空でないとき)を設定すると、cache はその directory に置かれます([読む所](../../src/core/shader/shadercompiler.cpp#L677))。移すだけで、project を持たない compiler に cache を与えることはしません。
  - 鍵は中身から作られ、project のファイルは絶対 path ではなく「include root の番号 + root からの相対 path」で名付けます。中身と相対位置が同じなら、別の場所にある project とも同じ鍵になり、1 つの directory を共有できます。engine に埋め込まれた shader が 1 byte でも違う build、shaderc の版や compile option の違う build とは鍵が分かれます。TS-1 で鍵の作り方が変わったので、**既存の project の cache は TS-1 の後の最初の起動で一度だけ全部外れます**。
  - ✅TS-2 で鍵の計算の仕方(SHA-256 の実装、include の走査、include root の列挙)を軽くしましたが、**鍵の値は変わらない**ので、既存の cache はそのまま当たります。
  - engine 自身は cache の directory を掃除しません。自分で `PELICAN_SHADER_CACHE_DIR` に決めた directory と project の `.pelican/shader_cache` は増え続けるので、手で消します。GPU テストの launcher が自分で決めた共有 directory だけは、✅TS-2 から launcher が 1 日 1 回掃除します(§10.9)。
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

`--tab` が取るのは `frame_plan` / `render_features` / `rendering_settings` / `graph` の 4 つ(`rendering_settings` は ✅RSX から。[第13章](13_editor.md)「Studio の Rendering settings タブ」)
([一覧](../../src/devstudio/view/mainwindow.cpp#L1608))。**未知の名前は window を作る前に console へ
理由を出して終了コード 2** です([検査](../../src/devstudio/main.cpp#L36)) —— window を開いて「たまたま前にあった
タブ」を見せると、綴り間違いが成功に見えるためです。`--tab` は `openProject` の**後**に適用されます
([順序](../../src/devstudio/view/uimain.cpp#L78)) —— project を開くとその project のツール配置が復元され、
どのタブが前かが変わるからです。

見た目は **View ▸ Theme ▸ Light / Dark** で切り替えます(✅N-5。CLI の引数ではありません)。選択は QSettings の
`studio/theme` に残り、**再起動なしで**窓と 2 つのノードキャンバスに当たります([第13章 §13.5](13_editor.md))。

`--automation <名前>` は **GUI を外から駆動するための口**です(Q-1)。指定すると `QLocalServer`(Windows では
`\\.\pipe\<名前>`)が開き、**改行区切りの JSON 1 行 = 1 命令**で `list` / `click` / `double_click` / `drag` /
`wheel` / `move` / `press` / `release` / `key` / `type` / `text` / `action` / `menu_list` / `view_state` /
`wait` / `wait_engine` / `rpc` / `open_project` / `screenshot` / `quit` の **20 命令**を受けます
([一覧](../../src/devstudio/automation/automationcommands.cpp#L2139))。**flag が無い起動では server object が
作られないので socket は存在しません**([構築](../../src/devstudio/view/uimain.cpp#L41)) —— 出荷時の挙動は
変わりません。listen できたときだけ `automation listening: <endpoint>` を標準出力に出します
([その 1 行](../../src/devstudio/view/uimain.cpp#L71))。開けなかったときは理由を出して **終了コード 3** です。

`wait_engine` は engine が editor RPC に答えた時点で返ります。engine は**最初の frame を出す前に** RPC に
答えるので、描画済みの frame が要る操作(`capture`、公開済みの frame plan)の前には `frames` を付けます。
`{"cmd": "wait_engine", "frames": 1}` は `get_status` の `render.published_frames` がその数に届くまで待ち、
応答に `published_frames` を足します([commandWaitEngine()](../../src/devstudio/automation/automationcommands.cpp#L1877))。
`published_frames` は renderer がその起動で公開した logical frame の数で、飛ばした frame は数えません。

回す側は [`tools/studio_drive.py`](../../tools/studio_drive.py#L1276)(`uv run`、標準ライブラリだけ)です。
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
スクリーンショット)・`studio_log` を持ちます([組み立て](../../tools/studio_drive.py#L1445))。
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

✅IA-1s で、シナリオが Studio の外の作業を代わりにする口が 2 つ増えました。

- **`file` step の `copy`**: `{"file": "copy", "path": "{project}/x", "from": "{artifacts}/y"}` は `from` の file を `path` へ byte のまま写します(あれば上書きします。[file step の copy](../../tools/studio_drive.py#L809))。`write` は UTF-8 の文字しか書けないので、人が外で焼き直した binary の file を project の file に上書きする、の代わりです。`path` と `from` のどちらも run の持つ 2 つの directory(`{project}` と `{artifacts}`)の中でなければ step の失敗です(ほかの `file` step と同じ範囲。[第13章](13_editor.md) §13.5「Studio の自動化面」)。
- **`<名前>.prepare.py`**: `test/studio_scenarios/<名前>.json` の隣に `<名前>.prepare.py` があると、ctest の `run_studio_scenario.cmake`([prepare の script を走らせる cmake の口](../../test/run_studio_scenario.cmake#L68))は driver を起こす前に `<python> -B <名前>.prepare.py --out <run の directory>/incoming --cli <pelican_cli>` を走らせます(120 秒で打ち切り)。人が Studio を開く前に外で作る file(自分の script で書いて CLI で焼いた資産など)の代わりで、シナリオは `{artifacts}/incoming/` の file を Studio の Import で取り込みます(`{artifacts}` がその run の directory です)。`pelican_cli` は ctest の登録が target から `-DCLI=` で渡し([prepare に CLI を渡す登録](../../test/CMakeLists.txt#L3563))、target が無いか file が無ければ、prepare を持つシナリオは失敗です。prepare が 0 以外で終わっても、その stderr を付けて失敗です。`tools/studio_drive.py` を直接回すときは prepare は走りません(ctest の cmake の口のため)。
  - 今 prepare を持つのは `gui_build_ltc` と、✅AB-1s の `gui_build_audio_reactive`(音量が時間で変わる 1 秒の小さな WAV 2 つを書く。`--cli` は受けるが使わない)の 2 本です。[`gui_build_ltc.prepare.py`](../../test/studio_scenarios/gui_build_ltc.prepare.py) は LTC の表 2 枚と焼き直した 1 枚を raw の float で書き、`pelican_cli image ktx2 --format R32G32B32A32_SFLOAT --raw … --width 64 --height 64`(§10.4)で `.ktx2` にします。
  - 書式の正の [README](../../test/studio_scenarios/README.md) には、`copy` と prepare はまだ書かれていません(IA-1s の commit は README を変えていません)。

> **設計決定(D0: エディタ特権の禁止・2026-07-08):** devstudio は公開契約(rpc / pelican_project / データ形式)の上に建つ 1 クライアントであり、エンジン内部への裏口 API を持たない。編集操作はまず rpc メソッドとして定義し、devstudio はそれを呼ぶだけ。

project を開くと、中央 viewport は同じ project を `--rpc --project <root>` と editor feature
overlay 付きの
`pelican_player` 子 process として起動し、Windows の native HWND を
Qt host へ再親付けします。player が自分の swapchain へ描いて OS が合成するため、pixel readback、
copy、process 間 frame 転送はありません。player が終了しても Studio は残り、`Restart Engine`
から再起動できます。既定では Studio と同じ directory の
`pelican_player.exe` を使います。project、`--rpc`、`--feature-overlay`、`--embedded`(✅EW-2。窓を非表示で作らせ、埋め込んだ後に Studio が見せる。追加引数に `--headless` があるときは付けない)は Studio が所有し、
`PELICAN_STUDIO_PLAYER_ARGUMENTS` 内の同名指定は除去します。開発時の executable と追加引数の
override は次です:

```powershell
$env:PELICAN_STUDIO_PLAYER = "C:/path/to/pelican_player.exe"
$env:PELICAN_STUDIO_PLAYER_ARGUMENTS = "--gpu-labels --vulkan-validation=on --free-camera"
build/dist/Debug/pelican_studio.exe
```

`--project` はここに書きません。Studio が開いた project から自分で渡すため、同名指定は除去されます。

埋め込み player は Studio の環境変数を継承します(QProcess)。音を出させたくないときは、Studio を
`PELICAN_AUDIO_BACKEND=null` で起動します(✅AU-3。`PELICAN_STUDIO_PLAYER_ARGUMENTS` に `--audio-backend null` を書いても同じです)。
シナリオの driver(`tools/studio_drive.py`)は、呼び出し側がこの変数を決めていない(未設定か空)ときに `null` を渡します
(`d616e35a`)。

player のログは **project ごと**に `<project>/.pelican/logs/player.log` へ出ます。Studio が `--log-file` を
付けて起動し([引数の組み立て](../../src/devstudio/viewport/studioplayerarguments.cpp#L100))、ファイルは起動のたびに
切り詰められます。以前は全 player が Studio の cwd の `pelican.log` に追記していたため、別の project や
前回の起動の行が混ざっていました。`PELICAN_STUDIO_PLAYER_ARGUMENTS` に自分で `--log-file` を書いた場合は
それが優先され、Studio は付け足しません。

Studio が viewport の窓を先に壊した場合も player は自分で終わります。埋め込み窓は親と一緒に消え、GLFW の
close flag は立たないので、以前は surface を毎 frame 作り直そうとして(`ErrorSurfaceLostKHR` が数十行)
Studio に kill されるまで回っていました。main loop は Win32 の窓がもう無いことを見て止まります
([Window::process()](../../src/core/os/window.cpp#L374))。

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
あります([置き場](../../src/devstudio/view/inspectorwidget.cpp#L288))。選択オブジェクトを owner とする
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
[構築](../../src/devstudio/view/mainwindow.cpp#L372))。**Graph は新しい dock ではなくこの dock の 3 枚目**です ——
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
([構築](../../src/devstudio/view/renderfeatureswidget.cpp#L2391))。**Parameters...** は選択 feature の
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
([定義](../../src/devstudio/view/graphcanvasreadcapability.hpp#L34))は汎用の RPC メソッドを**表現できない**形で、
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
- **GPU テストの共有 shader cache(✅TS-1)**: `pelican_gpu_lease_run` は子を起こす前に、`PELICAN_SHADER_CACHE_DIR` が未設定なら `%LOCALAPPDATA%/pelican/test_shader_cache`(Windows 以外は `$XDG_CACHE_HOME/pelican/test_shader_cache`、無ければ `~/.cache/pelican/test_shader_cache`)を作って設定します([`prepare_shader_cache_directory()`](../../test/gpu_lease_run.cpp#L136))。GPU テストは毎回新しい temp project を作るので、それまでは shader cache が毎回空でした。環境変数として子へ継承されるので、全 GPU テスト・全 worktree・Studio のシナリオが起こす player が 1 つの cache を共有します。既に設定されていれば触らず、directory を作れなければ設定しません(各 project の `.pelican/shader_cache` のまま)。`PELICAN_GPU_LEASE=off` の素通しでも設定されます。TS-1 の報告では warm の状態で `golden image cases match expected output` が 131.3 s → 49.2 s、`LP-2 hybrid golden cases draw identically…` が 28.5 s → 7.7 s でした。
  - project の cache directory を列挙して miss を数える GPU テスト(`WP366 renderer A B recovers q r group miss and WP384 preserves frame publication`)は、テストの中で変数を外しています。lease_run を通らない非 GPU のテストには変数が付きません。開発者が変数を全体に設定すると、「初回は miss」を前提にする非 GPU のテスト(`shader_library_test` の hot reload の 1 本)が赤になりうる、と TS-1 の報告にあります。
  - ✅TS-2 から、launcher は自分で決めた共有 directory を 1 日 1 回掃除します([`sweep_shader_cache_directory()`](../../test/gpu_lease_run.cpp#L101))。印のファイル `last_sweep.stamp` の更新時刻から 24 時間たっていれば、directory 直下の普通のファイルのうち 14 日書かれていないものを消します(下の directory には入りません)。失敗は全部無視し、テストの起動は止めません。呼び手が `PELICAN_SHADER_CACHE_DIR` を決めていれば何もしません。cache に当たってもファイルの更新時刻は変わらないので、使っている entry も 14 日ごとに 1 回 compile し直されます。
- 結合テスト: `test/run_*.cmake` が player / cli を子プロセス起動して検証。2026-07-10 以降の追加: `run_devcli_assets`(WP66)/ `run_event_schema_compile`(WP71)/ `run_dump_lowered_material`(WP76)/ `run_devcli_gltf_extract`(WP79)/ `run_spvlink_golden`(WP80)/ `run_devcli_rules_import`(WP84)/ `run_devcli_bake_camera` + `run_input_record_replay_headless`(WP89)/ `run_ui_u2_rpc_replay`(WP93)など。
- 2026-09 の追加: [`run_devcli_graph_catalog.cmake`](../../test/run_devcli_graph_catalog.cmake)(ctest 名 `devcli_graph_catalog`。`pelican_cli graph-catalog` の出力を検査し、**さらに `docs/v0_freeze.md` の操作表と突き合わせます** —— 表は `<!-- graph-op-table:begin -->` / `:end` のコメントで区切られていて、誰も比べない写しを 2 つ目の正にしないための仕掛けです)、`package_install_smoke`(X-1。install → カタログ → 描画 → uninstall を実 player で。`gpu` ラベル)、[`run_project_catalog_smoke.cmake`](../../test/run_project_catalog_smoke.cmake)(出荷 project の走査。`project.json` の `requires` が満たせない project は**名前を出して skip** し、player の中で落ちません)。
- **Studio のシナリオ(Q-1)**: `test/studio_scenarios/*.json` は 1 本が 1 つの ctest `studio_scenario_<名前>` になり
([登録](../../test/CMakeLists.txt#L3530))、`pelican_gpu_lease_run` 経由の **GPU lane** で走ります —— 実物の窓に
実物の player が埋まるので `QT_QPA_PLATFORM=offscreen` は掛けません。Studio target と Python 製テストを有効にし、
Python 3.11 以上を用意します。`uv` は不要です。完全な GPU gate はシナリオの未登録・skip を失敗にするため、
`SKIP_DEVSTUDIO=OFF` と `PELICAN_PYTHON_TESTS=ON` を指定します。[`run_studio_scenario.cmake`](../../test/run_studio_scenario.cmake#L30) が fixture
`test/fixtures/studio_automation_graph` を temp に 2 つ複製し、Qt を PATH に置いて driver を起動します。
対象の一覧は [`test/studio_scenarios/`](../../test/studio_scenarios/) が正本です。成果物(`report.json`、
`studio.log`、スクリーンショット)は `build/test_artifacts/studio_scenarios/<名前>/` に残ります。
- ゴールデンイメージテスト(**`inventory.json` の `cases[]` に登録されたものだけが対象。ディレクトリを置くだけでは走らない**。件数は [第6章](06_rendering.md) §6.10 が正)。
- ctest 非登録のスモーク: `run_build_units_smoke.cmake`(IMGUI / PHYSICS 系を含む単独 OFF ビルド検証。`PELICAN_WITH_ANIMATION=OFF` は [`run_animation_smoke()`](../../test/run_build_units_smoke.cmake#L1210) が回し、[`verify_animation_absent()`](../../test/run_build_units_smoke.cmake#L1143) が「消えているべきものが消えている」を確かめます)、`run_project_code_smoke.cmake`、`run_package_template_smoke.cmake`(X-1。package 入りの一時 project を configure から build して headless で走らせるので、**primary worktree の依存ソースを** `-DPELICAN_PACKAGE_TEMPLATE_SMOKE_DEPS=<primary>/build/_deps` で渡します)。
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
