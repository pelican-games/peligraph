# 第10章 ツールリファレンス

最終確認: 2026-10-07(develop 8c37c92a9)

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
| `pelican_game_logic` | **DLL**(`PELICAN_PROJECT` 指定時) | player と同ディレクトリ | プロジェクトの `code/` のビルド産物(WP90 で静的リンクから移行) |
| `pelican_cli` | exe | `build/dist/<Config>/` | 開発 CLI。**エンジン(pelican_core)にリンクしない** |
| `pelican_studio` | exe(Qt6) | `build/dist/<Config>/` | Widgets エディタ shell + 別 process engine viewport。render-tools dock は **4 タブ**(Frame Plan / Features / Rendering settings / Graph)で、Inspector にグラフ装着欄がある(§10.7) |
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
| `--rpc` | off | stdio JSON-RPC モード。**`--headless` は不要です**: ヘッドレスでは stdin を読み切るまでブロッキング、**ウィンドウモードではフレーム境界処理 + 有界キュー(既定 64 件)**。溢れると `busy` エラーを即返します。※`--rpc` を付けると ImGui UI は無効([第13章](13_editor.md)) |
| `--embedded` | off | 別 process の窓に埋め込まれる前提で起動する(EW-1、[引数](../../src/player/main.cpp#L348))。render 窓を `GLFW_VISIBLE` / `GLFW_FOCUSED` / `GLFW_FOCUS_ON_SHOW` = `GLFW_FALSE` で作る(非表示・focus 無し。見せるのは埋め込む側 — [window.cpp](../../src/core/os/window.cpp#L199))。project の `basic_config.fullscreen` が true でも無視して `basic_config.window_size` の窓で作り、log に `--embedded: the project's fullscreen setting is ignored; the window is created windowed` を 1 行出す。この引数を読むのは窓の作成だけで、時刻や game の動きは通常の窓あり起動と同じ。`--headless` との併用は引数の誤り(stderr に `--embedded cannot be combined with --headless` と usage、終了コード -1 — [併用の拒否](../../src/player/main.cpp#L477))。Studio が viewport の engine に渡す。窓の目印は下の段落 |
| `--render-fallback` | off | `--rpc` と一緒に使う(BS-1、[`--render-fallback` の引数の定義](../../src/player/main.cpp#L355))。起動時に rendering の構成が組めなくても終わらず、engine 内蔵の一色の fallback の世代で動き、理由を `get_status.render_pipeline` で言い、`reload_render_documents` か render の編集で直した構成を受け取る。`--rpc` が無ければ引数の誤り(stderr に `--render-fallback requires --rpc` と usage、終了コード -1)。付けない起動では、構成が組めなければ今までどおり致命(終了コード 1)。Studio は 起こす engine にいつも付ける(§10.7 の Studio の所有する引数)。詳細は [第6章](06_rendering.md) §6.16 の「起動時に組めない構成と `--render-fallback`」と [第13章](13_editor.md) §13.5 |
| `--frames <n>` | 3 | ヘッドレスのフレーム数。**0 = 無制限** |
| `--size WxH` | 1280x720 | ヘッドレスの描画サイズ |
| `--render-out <path>` | なし | PNG 出力先(sRGB エンコード)。`%04d` 等で毎フレーム連番 |
| `--fps <f>` | 60.0 | ヘッドレス固定ステップのレート(dt = 1/fps) |
| `--start-time <秒>` | なし | **ヘッドレス専用**。この時刻の frame から `--frames` 枚を出す。`llround(秒 × fps)` で **parse 時に frame 番号へ量子化**され、engine には「frame の間の時刻」が渡りません([parseStartFrame()](../../src/player/main.cpp#L326))。`--headless` が無ければ `offline.headless_required@1`、負・非有限・数でない綴りは**すべて** `offline.time_invalid@1`、`--rpc` / `--replay` / `--record-input` / `--bake-camera-output` との併用は `offline.mode_conflict@1`([検査](../../src/player/main.cpp#L638))。`--frames` は併用です |
| `--start-state <edit\|playing>` | `playing` | 起動時の再生の状態(PM-1b、[引数](../../src/player/main.cpp#L373))。`edit` は behavior を眠らせ(instance を作らない)、game の時計を止め、editor camera を用意して、RPC の `set_play_state` を待つ(`--free-camera` が無ければ `blender` の free camera とその overlay を足す。`--editor-transform` は足さない)。`--replay` / `--record-input` / `--bake-camera-output` / `--start-time` / `--rpc` の無い `--headless` との併用は `offline.mode_conflict@1: --start-state edit cannot be combined with <引数>`([併用の拒否](../../src/player/main.cpp#L632))、他の値は `--start-state must be one of: edit, playing`(どちらも引数の誤りとして usage を出して終了コード -1)。edit 状態の中身は [第13章](13_editor.md) §13.5 の「edit 状態」。Studio は `edit` を渡す(第13章 §13.1「Studio で game を再生・停止する」) |
| `--dump-frame-plan` | off | 解決済みフレームプラン JSON を **stderr** へ出力([第6章](06_rendering.md)) |
| `--gpu-labels` | off | Vulkan debug object 名と command label を有効化。validation layer の選択とは独立([第6章](06_rendering.md) §6.14) |
| `--vulkan-validation <off\|on>` | `_DEBUG`: on / それ以外: off | Khronos validation layer と synchronization validation を一体で選択。要求時に `VK_LAYER_KHRONOS_validation` が無ければ名前付き hard error |
| `--allow-absolute-paths` | off | CLI 由来のコンテンツ参照に限り絶対パスを許可。JSON 内の絶対パスは常に拒否 |
| `--user-dir <dir>` | なし | `user://` ルートの差し替え |
| `--log-file <path>` | なし | この起動のログを `<path>` に書く。親 directory は作り、**起動ごとに切り詰めます**(1 ファイル = 1 回の実行)。省略時は従来どおり cwd の `pelican.log` に追記します([main.cpp](../../src/player/main.cpp#L384)、[log.cpp](../../src/core/log.cpp#L34))。Studio は project ごとの log をこれで渡します(§10.7) |
| `--ignore-engine-version` | off | `engine_min_version` 不適合を hard error → WARN に降格 |
| `--play-seq <path.jsonl>` | なし | `pelican.transform_seq` の再生(`PELICAN_WITH_SEQPLAYER` ビルドユニットが ON のビルドでのみ使えます — [第2章](02_getting_started.md) §2.2) |
| `--seq-mesh <builtin:sphere\|path.glb>` | builtin:sphere | transform_seq 全オブジェクトに使うメッシュ |
| `--seq-loop` | off | クリップをループ再生 |
| `--play-vat <path.glb>` | なし | `pelican.vat` 入り GLB の再生(VAT ユニット必須) |
| `--camera "px,py,pz,tx,ty,tz,fov_deg"` | なし | カメラの位置・注視点・垂直 FOV(度)の上書き |
| `--game-logic <path.dll>` | (exe 同階層) | ゲームロジック DLL の明示指定 |
| `--strict-assets` | off | assets manifest の差分を起動エラーに昇格(WP66。ホットリロードも無効化)。配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`)では、起動時の照合はこれを付けたときだけ走ります(AV-1、§10.4 の `assets`)。付けない起動では読めない material 文書があっても止まらず問題の一覧に載せ、付けた起動では今までどおり失敗します([第6章](06_rendering.md) §6.7 の「起動時に読めない material 文書」) |
| `--record-input <path.jsonl>` | なし | 順序付き入力を `pelican.input_seq` **v2** で収録(WP89。GP-4 から v2 で、ヘッダに窓座標 → framebuffer ピクセルの換算 `content_scale` / `framebuffer` を持つ — [第7章 §7.3](07_input_ui.md)) |
| `--replay <path.jsonl>` | なし | input_seq のリプレイ。v2 と v1 を読む(v1 は content scale 1 として読む)。`--record-input` と排他、ホットリロード自動無効 |
| `--input-profile <name>` | project.json の既定 | アクティブ入力プロファイルの上書き |
| `--input-action-overlay <uri>` | なし | `pelican.input_action_overlay` v1 の action/profile bundle を**この起動だけ** project 入力へ重ねる |
| `--free-camera [blender\|unity]` | off (`blender` が省略時 preset) | project 入力と並存する runtime-only の `orbit` camera と同梱入力 overlay を有効化 |
| `--editor-transform [blender\|grab]` | off (`blender` が省略時 preset) | runtime の transform 編集アクションを有効化。`grab` は handle のドラッグだけを残す |
| `--xr off\|auto\|on` | off | OpenXR(PCVR)起動(WP125。`auto` = 不在なら flat 続行 / `on` = 不在・headless・rpc・リプレイでは名指しエラー — [第2章](02_getting_started.md)) |
| `--bake-camera-output <path.jsonl>` | なし | リプレイ中のカメラ軌跡を transform_seq v1 で出力。`--headless` + `--replay` 必須 |
| `--audio-backend device\|offline\|null` | 環境変数 `PELICAN_AUDIO_BACKEND`、それも無ければ窓あり = `device` / ヘッドレス = `offline` | 音のバックエンドを名指しする(AU-3、[読み取り](../../src/player/main.cpp#L490))。**引数があると環境変数は読まない**。空の環境変数は未設定の扱い。未知の値は `audio.backend_unknown@1` で起動を止める。`null` はデバイスを開かない(退避ではないので `get_status` の `audio` に `fallback_reason` は付かない)。`device` / `offline` は開けなければ `null` へ退避する。`PELICAN_WITH_AUDIO=OFF` の build では `null` だけを受け付け(`offline` / `device` は `This binary was built with PELICAN_WITH_AUDIO=OFF: --audio-backend <値> is unavailable`)、環境変数は読まない。詳細は [第8章 §8.11](08_gameplay.md) |

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

**render 窓の目印(EW-1、Windows)**: 窓あり起動の render 窓には、`--embedded` の有無に関係なく常に Win32 の窓 property `PelicanEngineRenderWindow`(値 1)が付きます。窓を作った直後に `SetPropW` で付け([付ける所](../../src/core/os/window.cpp#L218))、窓を壊す前に `RemovePropW` で外します([外す所](../../src/core/os/window.cpp#L309)。埋め込む側が親ごと窓を壊した後なら外すものは無い)。名前の定数は [`Pelican::engineRenderWindowProperty`](../../src/core/os/enginewindowproperty.hpp#L15) で、engine の型に依らない header だけのファイルなので、engine を link しない埋め込み側も include できます。埋め込む側は「その process の、owner の無い top-level 窓で、この property を持つもの」を、見えているかどうかを問わず探します(`--embedded` の窓は非表示で作られるため。GLFW の helper 窓には property が付きません)。Studio がこの規則で窓を探す仕組みは [第13章](13_editor.md) の「engine の窓は Studio の外に出ません」の段落を見てください。なお `get_status` の `input.cursor.embedded`(§10.3)はこの引数を見ず、`--rpc` の窓が `WS_CHILD` にされているかで決まります([判定](../../src/core/os/window.cpp#L348))。

test は `ew1_engine_window_embedded`([engine_window_embedded.py](../../test/engine_window_embedded.py)。Windows かつ `PELICAN_WITH_RPC` の build の GPU test): `--embedded --headless` が断られること、fullscreen を指定した project を `--embedded` で開くと property 付きの窓がちょうど 1 つあり、player の top-level 窓はどれも見えず、client 領域が `window_size` の大きさで、非表示のまま frame が進み、`ShowWindow` の後も同じ窓で frame が進み、log に上の 1 行があること、`--embedded` 無しでは同じ property 付きの窓が見えていることを見ます。

※ `--play-seq` / `--seq-mesh` の相対パスは現状 cwd 基準で解決されます(既知の食い違い。[第11章](11_status.md))。

## 10.3 JSON-RPC(stdio)— 外部制御プロトコル(実装済み)

実装: [rpcserver.cpp](../../src/core/communication/rpcserver.cpp)。プロトコルの正本は [../external_tools_requirements.md](../external_tools_requirements.md) R8。

### 枠組み(不変)

- stdin/stdout の NDJSON(1 行 = 1 つの JSON-RPC 2.0 リクエスト)。**stdout はプロトコル専用**(ログは cwd の `pelican.log`。`--log-file` を付けたときはそのファイル)。
- batch・notification 非対応。エラーコードは標準 + アプリケーションエラー -32000、RenderDoc capture -32010。ハンドラ内例外は応答に変換されプロセスは落ちません。EOF で正常終了。
- 1 プロセス 1 クライアント。**ゲーム DLL リロード中は全メソッドがアプリケーションエラーで拒否**されます。

> **設計決定(決定性):** 同一の RPC スクリプトを 2 回実行したとき、応答列は(instance_id を除き)**バイト単位で完全一致**しなければならない。CI で検証済み。`instance_id` だけが除外されるのは、それが RPC エンドポイント生成時に振られる UUID v4 で、起動ごとに必ず変わる値だからです([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L2822))。

### 基盤メソッド一覧

| メソッド | params | 動作 |
|---|---|---|
| `get_status` | `{}` | `{instance_id, ray_tracing:{procedural_oracle, ray_layout_mode}, project_root, scene, scene_source:{...}, frame, time, play:{...}, seed, renderdoc, diagnostics:{renderdoc:{...}, scene_load:{...}}, debug_utils:{...}, vulkan_validation:{layer, available, enabled, synchronization, reason}, gpu_timing:{...}, memory:{...}, window_output:{...}, stores, input:{..., cursor:{requested, effective, embedded}}, reload:{...}, participants:{game_logic:{...}}, sprite:{...}, color:{...}, render:{published_frames}, startup:{...}, modules:{...}, xr:{active, reference_space, floor_semantics, ...}, animation:{available, sinks:[{owner, node, source, cursors:[{clip, time_seconds, normalized_phase}], frame_revision, pose_hash, state?, transition?, root_delta?}]}, audio:{available, backend, fallback_reason?, voices:[{node, state, position_seconds}]}, assets_verification:{state, ...}, preview:{available, reason?}, render_pipeline:{state, root, refusal?}, problems:{revision, count, errors, warnings}}`(`{...}` と略した鍵の形は下の「`get_status` の鍵」。`ray_tracing.ray_layout_mode` は RTL-1b で、このプロセスが起動した ray の layout の mode `"none"` / `"layout"`(まだ決まっていなければ `null`)。layout の有無を変えた編集を commit した後も、起こし直すまで前の値です。[第13章](13_editor.md) §13.5 の「ray の layout と bindings を読み書きする」。`preview` は PV-1 で、単発 preview(`render_preview`)が今の世代で使えるか。使えなければ `reason: {code: "preview.unsupported_authoring@1", feature, target?, graph?, pass?, cause?, message}`。[第6章](06_rendering.md) §6.13 の PV-1 の段落。`render_pipeline` は BS-1 で、project の構成で動いていれば `{state: "ready", root}`、`--render-fallback` の起動で構成が断られて fallback の世代で動いていれば `{state: "fallback", root, refusal: {code, ...fields, document?, message}}`([第6章](06_rendering.md) §6.16)。`problems` は PB-1 で、`get_problems` の一覧の件数。`revision` は一覧が変わったときだけ増える([第6章](06_rendering.md) §6.16 の「engine が持つ今の問題の一覧」)。`assets_verification` は AV-1 で、起動時の asset 検証の要約。形は下の「起動時の asset 検証の結果」。`animation` は AG-3a。animation module の無い build では `{available: false}` だけ。`audio` は AU-1 で、`backend` は `device` / `offline` / `null`、`voices` は CT-1 で graph の `audio.playback` の node ごとの `idle` / `playing` / `paused` と音源の秒、audio を外した build では `{available: false}` だけ。`gpu_timing` の `nodes` は 最後の 1 frame の sample だけで、node ごとの平均は `node_averages`([第6章](06_rendering.md) §6.14)。[第8章](08_gameplay.md) §8.11)※rpc は常に flat 駆動のため `xr.active=true` は rpc からは観測できない |
| `get_gpu_timing` | `{}` | node ごとの GPU 時間の平均 `{schema: "pelican.gpu_timing_node_averages", version: 1, enabled, supported, reason, window_size: 30, frame_count, nodes: [{logical_frame, graph_variant, view_index, node_ordinal, node_kind, node_name, barriers_ms, body_ms, body_supported, sample_count}]}`([`get_gpu_timing` の登録](../../src/core/communication/rpcserver.cpp#L1880))。`nodes` は窓の最後の 30 論理フレーム(履歴がそれより短ければその数 `frame_count`)の node ごとの平均で、`get_status.gpu_timing.node_averages` と同じ行・同じ値です([第6章](06_rendering.md#gpu-タイミングwp143) §6.14 の「GPU タイミング」)。`gpu_timing` feature が無ければ `{enabled: false, supported: false, reason: "feature_not_enabled", frame_count: 0, nodes: []}`。GPU が graphics queue の timestamp を持たなければ `supported: false`・`reason: "graphics_queue_timestamps_unsupported"`(持てば `"enabled"`) |
| `set_seed` | `{seed}`(符号なし整数) | 決定的乱数のシード設定。返り値 `{seed}` |
| `set_time` | `{t}` | 仮想時刻の直接設定(dt=0)。返り値 `{t, frame}`(`frame` は評価の番号) |
| `step_frame` | `{}` | headless 専用で、描画のフレームを 1 つ進める(評価の番号が 1 増える)。game が進むのは `playing` のときだけ。窓ありでは `step_frame_windowed` で断る(第 13 章 §13.5) |
| `set_play_state` | — | `edit` / `playing` / `paused` を切り替える。params・返り値・拒否は [第13章](13_editor.md#一時停止と-1-歩ずつ進めるpm-1a) §13.5 の「一時停止と 1 歩ずつ進める」「edit 状態」「再生のセッション」 |
| `advance_frames` | — | `paused` の game を指定の歩数だけ進める。同上 |
| `set_view_camera` | — | `edit` / `paused` で見る camera を editor と game から選ぶ。[第13章](13_editor.md#edit-状態pm-1b) §13.5 の「edit 状態」 |
| `render_frame` | `{}` | 時刻を進めず描画のみ。返り値 `{t, frame}` |
| `capture_gpu` | `{}` | `render_frame` と同じcurrent-time描画を1回だけ**RenderDoc**(GPU に送った描画コマンドを 1 フレーム分丸ごと記録して後から追える外部デバッガ)で captureし、実際に増えたindexのcanonical `.rdc` pathを返す |
| `capture` | `{path}` | 直近フレームを PNG 保存(sRGB)。応答は `{path, encoding: "srgb", contract: 2}`(`path` は書いたファイルの絶対 path) |
| `get_frame_plan` | `{}`、または `{loop_elements: {node, offset?, limit?}}`(LP-3b。draw_loop の要素の paging) | フレームプラン JSON(fullscreen / raster pass の node には shader の set 1 の宣言 `pass_inputs` が付く。VI-1 / VI-1b。LP 系からは最上位の `loops[]`、node と resource の `loop_origin`、node の `view_select` / `view_axes` / `no_selected_view` も。shader が `pelican_loop.glsl` の回の番号を読む pass / task の node に `reads_loop_index: true`。`schedule: "once"` の node に `schedule: "once"`・`last_run_generation`・`last_run_frame`(まだ走っていなければ `null`)。`physical_target_plan.resources[]` の `initial: {asset}` を持つ image の要素に `initial_asset: {asset, sha256}`(`sha256` は `assets.manifest` の値、載っていなければ `null`。asset の隣に `.import.json` があれば `derived_from` / `stale` / `stale_reasons` も)。最上位に `preview: {available, reason?}`(`get_status` の `preview` と同じ形)。fallback の世代の間だけ最上位に `render_pipeline`(`get_status` の `render_pipeline` と同じ形)。形は[第 13 章](13_editor.md) §13.5「ループと分解の RPC」) |
| `rerun_once` | `{node?}`(PS-1。`node` は空でない文字列。ほかの鍵は `-32602`) | `{scheduled: [<pass / task の名前>...]}`。`schedule: "once"` の node を次の frame で走らせる。`node` を省くと今の frame plan の once の全部、graph の node の id ならその node が作った once、`graph.loop` の id なら全回の once。per_frame の node は `[]`。frame plan に無い名前は `-32602` で `data` が `{code: "node_unknown", node}`([`rerun_once` の登録](../../src/core/communication/rpcserver.cpp#L2489)、[`Renderer::requestOnceRerun()`](../../src/core/vkcore/renderer.cpp#L4273)。走る条件は[第 6 章](06_rendering.md) §6.18「一度だけ走る pass と、frame をまたぐ image」) |
| `pick_object` | `{x, y}` | 最後に完了した **ID バッファ**(色ではなく「そのピクセルに写っているオブジェクトの ID」を書き出す描画出力。`picking` feature が宣言する `R32_UINT` のレンダーターゲット `picking_id` — [picking.json](../../src/core/resources/features/picking.json))の 1 ピクセルを同期読み出し。`engine://features/picking.json` が必要 |
| `set_gizmo` | `{selection:GizmoSelection\|null, mode}` | 選択 transform のギズモ表示を設定/解除。`engine://features/gizmo.json` が必要 |
| `query_gizmo_handle` | `{selection:GizmoSelection, mode, x, y}` | 指定した選択・mode・物理 pixel にある handle を状態なしで問い合わせ。同 feature が必要 |
| `query_gizmo_drag_basis` | `{selection:GizmoSelection, mode, axis}` | 選択・mode・軸(`null` は view の平面)のドラッグの 1 論理 pixel あたりの量を状態なしで問い合わせ。同 feature が必要。形は下の「ギズモのドラッグ基底と modal transform」 |
| `get_modal_transform` | `{}` | engine の窓で進む modal transform の状態を読む。同上 |
| `cancel_modal_transform` | `{reason?}` | 進行中の modal transform を取り消す。同上 |
| `ack_modal_transform` | `{operation_id, revision}` | 終わった modal transform を受け取ったと伝え、`idle` に戻す。同上 |
| `load_gltf` | `{path, name?}` | glb の一時ロード(プロジェクト相対のみ)。返り値 `{path, name}`(`path` は解決したファイル、`name` は省けば `null`) |
| `load_scene` | `{name}` | シーン切替。応答は `{name, committed, effect_errors[]}`(WP380b/380e。commit 後の副作用の失敗は error ではなく result に出ます — [第13章](13_editor.md) §13.13) |
| `get_runtime_objects` | — | 今の scene で game が spawn した生きている object の一覧。形は [第13章](13_editor.md#spawn-された-object-を読むro-1) §13.5 の「spawn された object を読む」 |
| `set_camera` | `{name}` | シーン内カメラへ切替。返り値 `{name}` |
| `raycast` | `{origin, direction, max_distance}`(3 鍵ちょうど) | 最近傍 1 件を `{hit: {name, collider_id, shape_ordinal, distance, position, normal, trigger, one_way}}`、当たらなければ `{hit: null}`([受け口](../../src/core/communication/rpcserver.cpp#L1243))。`PELICAN_WITH_PHYSICS=OFF` の build では `hit: null` ではなく JSON-RPC error(code `-32000`、`data` = `{code: "physics.module_unavailable@1", requirement: "physics", option: "PELICAN_WITH_PHYSICS"}`。PG-2b)。provider が 0 本の build も `-32000`(message は `physics.provider_unavailable@1` の例外文。[第8章](08_gameplay.md) §8.4「機能を外した build」) |
| `update_transforms` | `{objects, transforms}` | 次フレーム境界で適用。`rot` は名指し拒否(`rotation`)。返り値 `{queued}`(積んだ更新の数) |
| `inject_input` | `{events:[...]}` | 入力注入。`key_*` / `mouse_*` / `axis` + **`gamepadButtonDown` / `gamepadUp` / `gamepadAxis`**(リプレイ中は拒否)。`mouse_move` の `x` / `y` は**窓座標**で、OS からの移動と同じ event になる。`ctx.input().pointer()` と UI の入力ルータがフレームの換算で framebuffer ピクセルにする(headless は 1 倍。GP-4、[注入の受け口](../../src/core/communication/rpcserver.cpp#L866))。返り値 `{queued}`(積んだ入力 event の数) |
| `inject_event` | `{type, payload}` | 登録済みゲームイベントの emit。payload は EventPayloadSchema で事前検証。返り値 `{queued: 1}`。知らない `type` と payload の検査の失敗は `-32602` |
| `reload_game_logic` | `{}` | ゲームロジック DLL を即時リロード |
| `reload_shaders` | — | shader を読み直して公開する。params・返り値・拒否は [第13章](13_editor.md#shader-を読み直させるf-5) §13.5 の「shader を読み直させる」 |
| `get_assets_verification` | `{}` | 起動時(または最後の再検証)の asset 検証の全件(AV-1。下の「起動時の asset 検証の結果」) |
| `get_problems` | — | engine が持つ今の問題の全部。params・返り値と 1 件の形は [第6章](06_rendering.md#engine-が持つ今の問題の一覧--get_problemspb-1-a55d1eaf) §6.16 の「engine が持つ今の問題の一覧」 |
| `verify_assets` | `{}` | asset 検証を worker で走らせ直し、すぐ `{state: "running"}` を返す |
| `update_assets_manifest` | `{store}` | その store の manifest を今のディスクから書き直し、再検証を始める |
| `set_input_profile` | `{name}` | 入力プロファイル切替。返り値 `{name, gamepad_polling}`。選べない名前は `-32000` |
| `start_input_record` | `{path}` | input_seq 収録開始(WP89。v2 で書く) |
| `stop_input_record` | `{}` | 収録停止。`{path, frames, events}` |
| `start_input_replay` | `{path}` | リプレイ開始(fixed-step 化・リロードゲート閉鎖) |
| `stop_input_replay` | `{}` | リプレイ停止 |
| `export_scene_snapshot` | `{schema_version:2, allow_pending?:false}` | current scene を含む全 **authoring document**(= 編集の正本としてメモリ上に持つシーン文書。シーンファイル 1 つにつき 1 文書で、ディスクの実物とは別 — [第13章](13_editor.md) §13.2)の deterministic semantic bytes、SHA-256、revision、`prefab_closure` を返す。`schema_version` は `2` だけ(SnapshotV1 は MG-2b で撤去。`1` は `unsupported_snapshot_version`) |
| `import_scene_snapshot` | `{schema_version:2, semantic_scene_bytes, digest, current_scene_id, prefab_closure}` | disk を上書きせず、検証済み snapshot を in-memory scene source として reload する |
| `plan_project_migration` | `{}`(省略可) | 前の版の文書を移す計画(dry-run。何も書かない)。`{project_root, required, engine_version, saved_by_engine, documents:[{path, schema, from, to, steps, digest, manual_items}], expected:{<path>: <sha256>}}`(MG-1。[第3章 §3.13](03_project_format.md)) |
| `apply_project_migration` | `{expected:{<path>: <sha256>}}`(plan が返した `expected` をそのまま) | 移行を 1 つの transaction で適用。応答は plan の形 + `{applied, backup, saved_by_engine_after, restart_required}`。`expected` が今の文書と食い違えば何も書かずに `project.migration_plan_stale@1`。`expected` が無い / オブジェクトでない / 値が文字列でないものは `-32602` |

**`get_status` の鍵**([`get_status` の組み立て](../../src/core/communication/rpcserver.cpp#L1603))。上の表の形で `{...}` と略した鍵と、表の形に無かった鍵の形です。

| 鍵 | 形 |
|---|---|
| `scene_source` | `{hot_reload: false, save_method: "save_scene"}`。シーンはホットリロードの対象外で、保存は `save_scene` です |
| `play` | `{state, game_frame, game_time, pending_steps, view_camera, session, recorded_edits}`。`frame` / `time` との違いと各鍵は [第13章](13_editor.md#一時停止と-1-歩ずつ進めるpm-1a) §13.5 の「一時停止と 1 歩ずつ進める」 |
| `diagnostics.scene_load` | 読み込みの経路ごとの `{unspecified, startup, rpc, pending, import_versioned}`。各値は `{rejected_rows: [{address: {object, component, list_path, row_id}, error}], effect_errors}`(`effect_errors` は `load_scene` の result と同じ形。[第13章](13_editor.md#1313-シーン遷移とゲームロジックの結果wp378380b380e381381a) §13.13。[経路ごとに並べる所](../../src/core/loader/scene.cpp#L1890)) |
| `input` | `{recording, replaying, replay_frame, hot_reload, profile, profiles, gamepad_polling, cursor: {requested, effective, embedded}}`。`profile` は選んでいる入力プロファイル(無ければ `null`)、`profiles` は選べる名前の列。`cursor` は表の下 |
| `memory` | `{schema_version: 1, driver_available, driver_reason, heaps: [{heap_index, device_local, size, usage, budget, source}], engine_categories: [{category, allocated_bytes, logical_used_bytes, free_bytes, high_water_bytes, object_count, source, reason}]}`。`driver_reason` と `reason` は無ければ `null`([`memoryStatusJson()`](../../src/core/vkcore/memorydiagnostics.cpp#L72)) |
| `gpu_timing` | GPU の計測の状態、最後の 1 frame の sample、node ごとの平均 `node_averages`。鍵の表は [第6章](06_rendering.md#gpu-タイミングwp143) §6.14 の「GPU タイミング」 |
| `window_output` | 出力先の状態 `{state, reason, surface_epoch, swapchain_epoch, presentation_queue_family, surface_recoveries, retry_count, extent_revision, output_facts_hash, last_vk_result, retired_epochs, maintenance, retirement_mode, quarantined_resources, device_rebuild_reason}`。`state` は `ready` / `preparing` / `preparing_surface` / `suspended_zero_extent` / `unavailable_retry` / `surface_lost` / `device_rebuild_required` / `fatal`、`reason` は `none` / `zero_extent` / `acquire_not_ready` / `output_out_of_date` / `surface_stale` / `output_preparing` / `retry_pending` / `output_reconfigured` / `surface_lost`([名前の表](../../src/core/vkcore/frametarget.cpp#L152))。`presentation_queue_family` は無ければ `null`、`maintenance` は `worker` / `not_applicable`、`retirement_mode` は `present_fence` / `reacquire` / `not_applicable` |
| `startup` | `{config_ms, vulkan_ms, shaders_ms, shader_cache_hits, shader_cache_requests, models_ms, total_ms, complete}`。起動の段ごとの時間と shader cache の当たりの数 |
| `modules` | `{phase, creation_frozen, initialized, dependencies, initialized_after_runtime_start}`。`phase` は `booting` / `running` / `shutting_down`、`initialized` と `dependencies` は数、`initialized_after_runtime_start` は実行が始まった後に初期化した module の名前の列 |
| `color` | `{contract: 2, swapchain_format, path, readback_encoding: "srgb", capture}`。`path` は出力の sRGB 化が hardware の `srgb` か、shader で変換する `unorm_fallback` か。`capture` は `available` / `unavailable_windowed` |

⚠ `window_output.reason` の名前の表で、`device_lost` の行は `"srgb_shader_unorm"` という文字列を返します([その行](../../src/core/vkcore/frametarget.cpp#L200))。名前の取り違えに見えます(engine 側で確認する事項。この表には `device_lost` の名前を挙げていません)。

**`input.cursor {requested, effective, embedded}`**([組み立て](../../src/core/communication/rpcserver.cpp#L1770)): `requested` はゲームコードが `ctx.input().setCursorMode` で頼んだモード、`effective` は窓に適用したモード(`normal` / `hidden` / `locked`)、`embedded` は窓が別の窓に埋め込まれているか(`--rpc` かつ `WS_CHILD`。Windows だけで判定)。埋め込みでは `locked` を頼んでも `effective` は `normal` です。headless は窓が無いので `effective` は常に `normal`、`embedded` は `false`。

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

### ギズモのドラッグ基底と modal transform

**`query_gizmo_drag_basis {selection, mode, axis}`**([`query_gizmo_drag_basis` の登録](../../src/core/communication/rpcserver.cpp#L2586))は、ドラッグの 1 論理 pixel で値がどれだけ動くかを、状態を変えずに返します。`query_gizmo_handle` と同じく `engine://features/gizmo.json` が要り(無ければ `-32000`)、`selection` と `mode` は `set_gizmo` と同じ形です。鍵はこの 3 つちょうどで、ほかは `-32602` です。

- `axis` は `"x"` / `"y"` / `"z"` か `null` です。`null`(view の平面に沿った移動)は `mode: "translate"` だけで、ほかの mode では `-32602` です。
- 応答は `{contract: 1, selection, mode, axis, extent: {width, height}, content_scale, basis}` です。`axis` は送った値のままです。
- `basis` は、軸なら `{kind: "axis", axis, drag_direction: {x, y}, value_per_logical_pixel}`(意味は `query_gizmo_handle` の `handle` の同じ名前の鍵と同じ)、`axis: null` なら `{kind: "view_plane", world_per_logical_pixel_x: [x, y, z], world_per_logical_pixel_y: [x, y, z]}`(画面の右・下へ 1 論理 pixel 動かしたときの world の移動量)です。
- 求まらないときは `basis: null` です。軸では、その軸の handle がドラッグの向きを持たないとき(`query_gizmo_handle` が `handle: null` を返す潰れた軸と同じ判定)、view の平面では、出力の大きさが 0 か、camera の向きと射影から向きが決まらないときです([軸の基底](../../src/core/renderer/gizmo.cpp#L514)、[view の平面の基底](../../src/core/renderer/gizmo.cpp#L528))。
- selection の拒否(存在しない transform、`error.data.code` の 5 つの名前)は `query_gizmo_handle` と同じです。

**modal transform の 3 本**は、engine の窓でキー操作が始めた移動・回転・拡縮(modal transform)の状態を外から読み、止め、結果を受け取ったと伝えます([`get_modal_transform`](../../src/core/communication/rpcserver.cpp#L2672)、[`cancel_modal_transform`](../../src/core/communication/rpcserver.cpp#L2684)、[`ack_modal_transform`](../../src/core/communication/rpcserver.cpp#L2708))。gizmo の feature は要りません。

- **`get_modal_transform {}`**(鍵を書くと `-32602`)→ 状態 `{contract: 1, enabled, revision, phase, operation_id, selection, mode, axis, delta: {translation: [x, y, z], rotation: [x, y, z, w], scale_exponent: [x, y, z]}, reason, bindings: {translate, rotate, scale}}`([状態の形](../../src/core/communication/rpcserver.cpp#L665))。
  - `phase` は `idle` / `active` / `confirmed` / `cancelled` です。`operation_id` は操作の番号で、無ければ `null`。`selection` は gizmo と同じタグ付きの形、`mode` は `translate` / `rotate` / `scale`、`axis` は `x` / `y` / `z` で、どれも無ければ `null`。`reason` は取り消しの理由で、無ければ `null` です。
  - `revision` は状態が変わるたびに 1 増えます。`enabled` は modal transform の入力が効いているかで、効かなくなると進行中の操作は `reason: "actions_disabled"` で取り消されます([`ModalTransformController::update()`](../../src/core/renderer/modaltransform.cpp#L70))。
  - `bindings` は 3 つの action に割り当てたキーボードのキーのうち最初のものの表示名です(1 文字は大文字、`enter` は `Enter`、`escape` は `Esc`)。キーボードの割り当てが無ければ `null` です。
- **`cancel_modal_transform {reason?}`** → 同じ状態。`active` の操作を `cancelled` にします(`active` でなければ何もしません)。`reason` は空でない文字列で、省くと `studio_cancel` です。ほかの鍵は `-32602` です。
- **`ack_modal_transform {operation_id, revision}`**(2 つちょうど。どちらも符号なし整数)→ `{acknowledged, state}`。`phase` が `confirmed` か `cancelled` で、`operation_id` が今の操作と同じで、`revision` が今の値以上のときだけ `acknowledged: true` になり、状態を `idle` に戻します(`revision` が 1 増える)。そうでなければ `false` で、何も変えません([`ModalTransformController::acknowledge()`](../../src/core/renderer/modaltransform.cpp#L239))。

### エディタ拡張メソッド(詳細は[第13章](13_editor.md))

上の表の `export_scene_snapshot` / `import_scene_snapshot` はエディタトラックで増えた分です。**総数は挙げません** —— メソッドは増え続けるので、数字を書くと必ず腐ります。登録の実物は `src/core/communication/rpcserver.cpp`(基盤)と `src/core/communication/editorrpchandlers.cpp`(エディタ)です。

シーンの編集・履歴・プレビュー・保存のためのメソッド群です。params と戻り値、**エラーが `result.status` に出る**という重要な作法は [第13章](13_editor.md) §13.5〜§13.6 にまとめてあります。`edit` 系だけ作法が違うのは、これらが**チケットを返す受付のメソッド**で、呼び出し自体は成功しており、実際の適用がフレーム境界で起きるためです(受付で弾かれた = `rejected` / 適用時に弾かれた = `failed`)。基盤メソッドは呼んだその場で答えが決まるので、失敗は通常の JSON-RPC `error` に出ます。

| 分類 | メソッド | 正本 |
|---|---|---|
| 読み取り | `scene_tree` / `scene_outline`(component を含まない軽い一覧)/ `get_components` / `get_scene_revision` / `list_assets` / `query_journal` / `can_edit` / `can_preview` / `get_component_add_candidates`(object に足せる component の候補) | [第13章 読み取り](13_editor.md#読み取り) |
| RT の occurrence | `get_ray_occurrences`(object 1 件の RT の occurrence と、その行の hit group・binding・record)/ `resolve_ray_occurrence`(occurrence の id を 1 件に解く) | [第13章 読み取り](13_editor.md#読み取り) |
| セッション | `open_editor_session` / `resume_editor_session` | [第13章 セッションと編集](13_editor.md#セッションと編集) |
| 編集 | `edit` / `undo` / `redo` / `get_edit_result` | [第13章 セッションと編集](13_editor.md#セッションと編集) |
| prefab | `set_prefab_parameter` / `unset_prefab_parameter` / `unpack_prefab`(同名の op 1 つだけの `edit` を出す) | [第13章 セッションと編集](13_editor.md#セッションと編集) |
| チケットプレビュー | `open_preview` / `update_preview` / `commit_preview` / `abort_preview` / `get_preview_result` | [第13章 プレビュー](13_editor.md#プレビュー) |
| 隔離プレビュー | `eval_preview` / `render_preview` | [第13章 プレビュー](13_editor.md#プレビュー) |
| 保存 | `save_scene` | [第13章 スナップショットと保存](13_editor.md#スナップショットと保存) |
| procedural asset | `get_procedural_assets` / `edit_procedural_asset`(asset_data の procedural asset の AABB と record を読む・書く。`edit` の op ではない) | [第13章 §13.3](13_editor.md#133-編集の実行モデル--チケットとフレーム境界) |
| project の宣言 | `get_project_settings` / `edit_project_settings`(project.json の `hot_reload` と `asset_stores` を読む / 書いてその場で反映する) | [第13章 project.json の宣言を書き換える](13_editor.md#projectjson-の宣言を書き換えるpj-1) |
| material 文書 | `list_materials` / `get_material` / `get_surface_schema` / `list_surfaces` / `get_material_bindings`(material 文書・`.surface` の宣言・model の割り当てを読む)/ `edit_material` / `create_material` / `bind_material`(書く)/ `set_object_material` / `get_object_materials`(scene の object ごとの上書き) | [第13章 material 文書を読み書きする](13_editor.md#material-文書を読み書きするmt-1) |
| material の削除・戻し・改名 | `delete_material` / `register_material` / `rename_material` | [第13章 material を消す・戻す・改名する](13_editor.md#material-を消す戻す改名するeb-1) |
| model の登録 | `register_model` / `unregister_model`(project の中の model のファイルを asset_data に載せる・外す) | [第13章 project の中の model ファイルを登録する・外す](13_editor.md#project-の中の-model-ファイルを登録する外すmi-1) |
| レンダー設定 | `get_render_features` / `list_render_features` / `edit_render_features`(feature の一覧と有効化・`materialize`)/ `get_render_authoring_context`(編集に要る名前と候補)/ `fork_render_pipeline`(preset を project の文書へ写す)/ `edit_region_replacement`(preset の領域の差し替え)/ `edit_preset_pass`(preset・feature・root の pass を直す)/ `add_authored_pass` / `remove_authored_pass`(editor が作る pass の fragment を足す・外す)/ `edit_render_targets`(editor が作る render target)/ `set_material_route_default` / `edit_rendering_keys`(rendering 節のほかの鍵)/ `edit_target_planning` / `edit_vulkan_plan_pins` / `put_vulkan_physical_fragment`(計画の決定)/ `get_frame_node_order` / `edit_frame_node_order`(pass の順) | [第13章 レンダー設定を編集する](13_editor.md#レンダー設定を編集する) |
| compute / RT の task と AS | `get_compute_task_authoring` / `edit_compute_task` / `add_compute_task` / `remove_compute_task`(task の宣言)/ `get_ray_callable_records` / `edit_ray_callable_records`(callable の record の初期値)/ `get_scene_acceleration_structures` / `edit_scene_acceleration_structures`(名前付きの TLAS と task の接続、三角形と AABB の座標系) | [第13章 レンダー設定を編集する](13_editor.md#レンダー設定を編集する) |
| 断片の決定 | `get_fold_groups` / `fold_nodes`(group の宣言)/ `verify_fold`(開いた形と比べて印を付ける非同期の ticket) | [第13章 レンダー設定を編集する](13_editor.md#レンダー設定を編集する) |
| 音の解析 | `bake_audio_analysis`(音の解析を焼く非同期の ticket)/ `list_derived_assets`(取り込みで作った file と、古いか) | [第13章 レンダー設定を編集する](13_editor.md#レンダー設定を編集する) |
| まとめて 1 回 | `edit_batch`(文書を書き換える編集 method を 1 つの ticket で) | [第13章 レンダー設定を編集する](13_editor.md#レンダー設定を編集する) |
| render 文書の丸ごと | `get_render_documents` / `restore_render_documents`(丸ごと読む・CAS で戻す)/ `reload_render_documents`(ディスクから読み直して組み直す) | [第13章 ループと分解の RPC](13_editor.md#ループと分解の-rpc--engine-の契約lp-0alp-vb--rd-1--rd-1b) |
| ループと分解 | `decompose_material_pass`(一体版の material pass を分解版へ)/ `get_graph_templates` / `apply_graph_template`(同梱の分解版の雛形) | [第13章 ループと分解の RPC](13_editor.md#ループと分解の-rpc--engine-の契約lp-0alp-vb--rd-1--rd-1b) |
| 共通グラフ | `get_graph_instances` / `get_graph_catalog` / `get_engine_shaders`/ `get_shader_interface`/ `get_registered_providers`/ `get_graph_values` / `get_graph_node_values` / `edit_graph_document` / `edit_graph_instances` | [第13章 読み取り](13_editor.md#読み取り)・[グラフの装着を編集する](13_editor.md#グラフの装着を編集する) |
| 共有部品と package | `export_render_feature` / `import_render_feature` / `install_package` / `uninstall_package` | [第13章 レンダー設定を編集する](13_editor.md#レンダー設定を編集する) |
| ray の layout と bindings | `get_ray_layout` / `edit_ray_layout`(RTL-1a。`edit_ray_layout` は render の編集の ticket を返し、`edit_batch` に入る)/ `get_ray_table`(EB-3。SBT の全行を 1 回で) | [第13章 レンダー設定を編集する](13_editor.md#レンダー設定を編集する) の「ray の layout と bindings を読み書きする」「SBT の全行を 1 回で読む」 |

既存の基盤メソッドへの影響: `step_frame` の応答に `edit_results[]` が付き、`load_scene` / `reload_game_logic` / `start_input_replay` は進行中のプレビューを強制中止します。`get_status` の `participants.game_logic`、`load_scene` と `import_scene_snapshot` の result の `committed` / `effect_errors`、`get_components` の各 component の `resolution_error` は [第13章](13_editor.md#1313-シーン遷移とゲームロジックの結果wp378380b380e381381a) §13.13 です。

各 method に後から足された鍵と名前付きの拒否も、上の表の「正本」の節に書きます。足された順(作業単位・日付・commit)は [RPC の追加の記録(2026)](../history/rpc_additions_2026.md) にあります。

<a id="起動時の-asset-検証の結果av-1"></a>

### 起動時の asset 検証の結果

player が起動時に行う assets manifest の照合([第3章](03_project_format.md) §3.4 の「起動時検証」)の結果は、log に出るだけでなく store ごとに engine に残ります([`AssetsVerification`](../../src/core/loader/assetsverification.hpp#L86))。読み方と走らせ直し方は次の 4 つです(handler は [`configureAssetsVerificationRpcHandlers()`](../../src/core/communication/rpcserver.cpp#L1481))。Studio の **Assets のドック**がこれを見せます: 木の下の 1 行と該当するファイルの行の印(`not_run` では何も出さない)、Details の store ごとの一覧、**Verify again**(`verify_assets`)、store ごとの **Update manifest**(`update_assets_manifest`。書く前に `N files will change in <manifest>.` を見せる)。下の 4 つの拒否もドックでは 1 文になります([第13章](13_editor.md) §13.1 の「Studio の Assets のドック」)。

**`get_status.assets_verification`**(軽い要約。[`statusJson()`](../../src/core/loader/assetsverification.cpp#L204))

| `state` | 形 |
|---|---|
| `verified` | `{"state":"verified","stores":1,"matched":1,"info":1,"warnings":2,"errors":0}`。件数は store ごとの issue を severity で数えたもので、起動時の log の要約行 `assets manifest: N stores, ...` と同じ数 |
| `not_run` | `{"state":"not_run","reason":"<理由>"}`。理由は 3 つの決まった文言: `"no asset store declares a manifest"`(manifest を宣言した store が無い。manifest なし = 検証なしなので正常)/ `"PELICAN_WITH_HOT_RELOAD=OFF"`(配布 build で `--strict-assets` なしに起動した)/ `"this host does not verify assets"`(player の起動経路を通らない host) |
| `running` | `{"state":"running"}`。`verify_assets` の再検証の最中 |

**`get_assets_verification {}`** → 全件([`detailJson()`](../../src/core/loader/assetsverification.cpp#L238))。

```json
{"state":"verified","verified_at":"2000-01-01T00:00:00Z",
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

**`verify_assets {}`** → `{"state":"running"}` をすぐ返し、module 専用の worker thread で起動時と同じ検査(中身の照合込み、起動時と同じ strict、同じ hash cache)を走らせます([`verify()`](../../src/core/loader/assetsverification.cpp#L303))。終わると `get_status` / `get_assets_verification` の結果が置き換わります。完了の通知は無いので、`get_status.assets_verification.state` が `running` でなくなるのを待って取り直します。

- 走っている間にもう一度呼ぶと、新しく走らせず `{"state":"running"}` を返します(合流)。
- manifest を宣言した store が無い project では走らせず `{"state":"not_run","reason":"no asset store declares a manifest"}` を返します。
- 再検証は log を出しません。配布 build でも明示の要求として走り、終わると `verified` になります。
- 照合する store の構成は、起動時のものか、PJ-1 の `edit_project_settings` で足した後のものです。project.json を手で書き換えたときは engine を起こし直します([第13章](13_editor.md#projectjson-の宣言を書き換えるpj-1) §13.5 の「project.json の宣言を書き換える」)。

**`update_assets_manifest {"store": "<name>"}`** は「今のディスクを正とする」操作です。`pelican_cli assets manifest` と同じ書き方(hash cache は `<project>/.pelican/assets-hash-cache.json`、`--full` なし)でその store の manifest を書き、INFO を 1 行(`manifest main: 2 files (1 hashed, 1 cached) -> <path>`、CLI と同じ文言)出して、`verify_assets` と同じ再検証を始めます([`updateManifest()`](../../src/core/loader/assetsverification.cpp#L318))。返り値は `{"store":"main","manifest":"<path>","files":2,"hashed":1,"cached":1,"state":"running"}` です。書き込みは handler の中で同期に行います。AV-1 の時点では、再検証が走っている最中ならそれが終わるまで待ってから書いていました(その間は frame も止まりました)。再検証が走っている間は待たずに `assets_verification_running@1 {store}` で断ります(走っているかを見て、さらに store の disk の lock を待たずに取ろうとし、取れなければ同じ拒否。[`AssetsVerification::updateManifest()`](../../src/core/loader/assetsverification.cpp#L318))。`get_status.assets_verification.state` が `running` でなくなってから送り直します。worker に合流して結果を待たずに返す形にしなかったのは、書き込みの失敗を RPC の答えで返せなくなるためと、Studio の Assets のドックが走っている間は Update manifest を送らない作りだからです。

params は `get_assets_verification` / `verify_assets` が空の object だけ、`update_assets_manifest` が `store`(空でない文字列)だけで、それ以外は `-32602` です。断るときは `reload_shaders` と同じ形(`code: -32000`、`data: {error_code, error_fields}`)です。

| `error_code` | message の例 | `error_fields` |
|---|---|---|
| `assets_store_unknown@1` | `update_assets_manifest: no asset store is named 'nope'` | `{"store":"nope"}` |
| `assets_manifest_undeclared@1` | `update_assets_manifest: asset store 'plain' declares no manifest in project.json` | `{"store":"plain"}` |
| `assets_manifest_write_failed@1` | `update_assets_manifest: cannot write the manifest of asset store 'main': <理由>` | `{"store","manifest","reason"}`(宣言した manifest が解決できない store では `{"store","reason"}`) |
| `assets_verification_unavailable@1` | `verify_assets: this host does not verify assets` | `{}` |
| `assets_verification_running@1` | `update_assets_manifest: the asset stores are being verified; update the manifest of 'main' once get_status says the verification is done` | `{"store":"main"}` |

<a id="移行の-2-method-と移行専用-endpointmg-1"></a>

### 移行の 2 method と移行専用 endpoint

`plan_project_migration` / `apply_project_migration`([実装](../../src/core/communication/projectmigrationrpc.cpp#L50))は
`pelican_cli project migrate` の RPC 版です(流れと名前は [第3章 §3.13](03_project_format.md))。失敗は application error(-32000)で、
`error.data` が `{error_code, error_fields}`(例: `error_code: "project.migration_plan_stale@1"`、`error_fields: {expected, actual}`)です。

- **ふつうに動いている engine** も 2 つに答えます([登録](../../src/core/communication/rpcserver.cpp#L1893))。移行の要る project はそもそも入口で止まるので、ここに来るのは空の plan(`required: false`)です(動いている間に文書が前の版に戻された場合を除く)。
- **移行専用 endpoint**: `--rpc` の player が開くときの入口で `project.migration_required@1` に当たると、engine を起動せずに
  この 2 method **だけ**に答える JSON-RPC endpoint になります([`runProjectMigrationRpcServer()`](../../src/core/communication/projectmigrationrpc.cpp#L98)、
  [player 側](../../src/player/main.cpp#L752))。ほかの method は application error で、message は入口が出した
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

<a id="pelican_cli-project-init----template-standardblankwp57--f-2"></a>

### `pelican_cli project init <dir> [--template standard|blank]`

空(または未存在)ディレクトリに雛形一式を生成します。生成直後に `pelican_player --project <dir>` でそのまま起動できます。詳細は [第2章](02_getting_started.md)。
空でないディレクトリは `project init target directory is not empty: <dir>` で拒否し、何も書きません。

雛形の中身は [`src/project/projecttemplate.cpp`](../../src/project/projecttemplate.cpp) の 1 箇所にあり(`pelican_project`)、
CLI は `--template` を読んで [`initializeProjectTemplate()`](../../src/project/projecttemplate.cpp#L400) を呼ぶだけです
([CLI 側](../../src/devcli/projectinit.cpp#L30))。Studio の File > New Project... も同じ関数を呼びます([第13章](13_editor.md))。
`--template` は 2 つです。

- **`standard`**(既定): 以下で説明する 3D シーン向けの 16 エントリ。F-2 の前に `project init` が書いていたものと
  byte 単位で同じでした(`devcli_project_templates` が SHA-256 で固定)。MG-1 から `project.json` だけが 1 行違い、
  `engine_min_version` の次の行に `"saved_by_engine": "0.1.0"`(書いたエンジンの版。[第3章 §3.2](03_project_format.md))が入ります。
- **`blank`**: `standard` と同じ 16 エントリ。違うのは   `passes/main_rendering_config.json` の `rendering` 節だけで、`pipeline` キーが無い **direct config** です   (preset も `features` も `render_targets` もありません)。中身は fullscreen pass `present` 1 本   (`input: []`、`output.color: ["display"]`、vertex `engine://fullscreen`、fragment `engine://passthrough`)で、   雛形は shader を書きません(`shaders/` もありません)。`engine://passthrough` は入力がすべて可変の engine の shader で、   入力が 0 本なら UV の gradient を、入力があれば入力 0 をそのまま書きます。`present` に target を   読ませるだけなら shader を書かずに済みます   ([雛形](../../src/project/projecttemplate.cpp#L228))。engine は direct config にも canonical anchor と   `display` を読む `output_transform` を足すので、描かれる frame は「project が書いた pass 1 本 + engine の終端」です。
  scene / input / ui / code / asset は `standard` と同じです(3D の model は置きません)。

知らない名前は `unknown project template: <name> (expected standard|blank)` で拒否します。

生成される `passes/main_rendering_config.json` は **`pelican.graph` root** です(両方の雛形で同じ)。`standard` で著作した内容
(`pipeline.preset` = `engine://render_pipelines/hybrid_v1.json`、`features` = `shadow_directional` と
`sky_ambient` の 2 本)は `rendering` オブジェクトの中に入ります。雛形の文字列は graph root をそのまま書いたもので
([standard](../../src/project/projecttemplate.cpp#L199) / [blank](../../src/project/projecttemplate.cpp#L228)。
[選ぶ関数](../../src/project/projecttemplate.cpp#L261))、以前それを包んでいた `wrapLegacyRenderingConfig()` は MG-2b で撤去しました
(`standard` の生成物は撤去の前と同じバイト列で、`run_devcli_project_templates.cmake` の digest も変わっていません)。`render_targets` / `rendering_passes` は preset が持つので
project は書きません —— `run_devcli_project_init.cmake` は**まず root が `pelican.graph` で `rendering` が
object であることを確かめてから**、節の中に禁止メンバが無いことを見ます
([検査](../../test/run_devcli_project_init.cmake#L48)。順序が逆だと、節ごと消えたときに検査が素通りします)。

生成される `code/CMakeLists.txt` は 2 行で、`pelican_game_sources(game.cpp)` の**後に**
`pelican_packages()` を呼びます([template](../../src/project/projecttemplate.cpp#L274))。順序が意味を持ちます ——
package の object は、その前に作られたゲームロジック DLL へ合流するからです。

<a id="pelican_cli-project-migrate---project----dry-runmg-1"></a>

### `pelican_cli project migrate --project <dir|project.json> [--dry-run]`

前の版のエンジンで作った project の文書を、このエンジンの版へ移します。流れ(検出 → plan → apply → 退避先)と
名前付きの失敗は [第3章 §3.13](03_project_format.md) が正です([CLI 側](../../src/devcli/projectmigratecommand.cpp#L38))。

- `--project` は project のディレクトリか、その中の `project.json`。どちらでもないものは
  `--project must point to a directory holding project.json, or to project.json: <path>`、`project.json` 以外の名前のファイルは
  `--project file must be named project.json: <path>` で拒否します。
- `--dry-run` は plan だけを出して何も書きません。付けなければ plan と apply を 1 回で行います(CAS の `expected` は渡しません)。
- 結果は **stdout に JSON**(2 space の字下げ)で、終了コード 0。失敗は stderr に `<名前>: <fields JSON>` を 1 行出して終了コード -1。
  この章の冒頭の「成功は 1 行サマリ」の例外です。
- **apply(`--dry-run` 無し)は、その project を開いている engine と Studio をすべて閉じてから実行します。** apply は最初に   project の transaction のディレクトリ(`.pelican/render-authoring.transaction`)を自分で作って取り、既にあれば何も読まず・消さずに   `project.migration_transaction_busy@1: {"path": …, "message": …}` で止まります。message は「engine と Studio を閉じてから apply し直す。
  残骸なら project を開けば解消する」という内容です。使用中と落ちたプロセスの残骸は区別できないので、どちらもこの名前です   (RPC の `apply_project_migration` も同じ。詳細は [第3章 §3.13](03_project_format.md) の 4.)。`--dry-run` は plan だけなので取りません。

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

<a id="pelican_cli-import-wp217984"></a>

### `pelican_cli import ...`

DCC 納品物の取り込み。3 つの形があります:

- `import <delivery_dir> --project <dir>` — `pelican.import` manifest の sha256 照合 → asset_data.json へ glb 登録(冪等)
- `import gltf --extract-scene <glb> ...` — glTF のノード階層を scene v2(`parent` 付き)として抽出(WP79。MG-2b から v2 を書く。[第5章](05_assets.md))。camera の node の回転は変換せずに `transform` へ書き、camera はそのまま glTF と同じ向きです([第4章 §4.2](04_scene_ecs.md) の camera)
- `import --rules ...` — `imports.rules.json`(glob → レシピ表)に従った一括取り込み

manifest の形式・実例は [第5章](05_assets.md) を参照してください。

<a id="pelican_cli-assets-manifestverifystatus---project-wp66"></a>

### `pelican_cli assets manifest|verify|status --project <dir>`

asset store の sha256 台帳(assets manifest)を生成・照合します。

```sh
pelican_cli assets manifest --project mygame            # 生成(store 走査 → manifest 書き出し)
pelican_cli assets verify --project mygame [--full]     # 照合(差分は severity 付き列挙、あれば exit 1)
pelican_cli assets status --project mygame              # OK / MISSING / MISMATCH の一覧
```

> **設計決定(検証はロードを止めない):** 起動時検証の深刻度は「内容の変化 = INFO、構造の逸脱(欠落・参照不能)= WARNING」で、エラーに昇格するのは `--strict-assets`(または配布 strict)のときだけ。

CLI の生成・照合と起動時の検証は file を 1 MiB ずつ読んで SHA-256 を計算し、Windows では BCrypt(CNG)で計算します(digest は同じなので、今の manifest と `.pelican/assets-hash-cache.json` はそのまま使えます)。起動時の検証の時間と、その前に出る 1 行は [第 3 章](03_project_format.md) §3.4 の「起動時検証」です。

起動時の検証の結果は engine が store ごとに持ち、**RPC で読めます**(`get_status.assets_verification` の件数、`get_assets_verification` の全件。§10.3 の「起動時の asset 検証の結果」)。engine を止めずに照合し直す `verify_assets` と、今のディスクを正として manifest を書き直す `update_assets_manifest {store}`(この CLI の `assets manifest` と同じ書き方)もあります。**配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`。dist-config が常に出す)では起動時に照合しません**。照合するのは `--strict-assets` を付けたときだけで、そのときは開発の build と同じに照合し、食い違えば起動を止めます。

<a id="pelican_cli-bake-camera---replay----project-wp89"></a>

### `pelican_cli bake-camera --replay <path.jsonl> --project <dir>`

リプレイを headless 実行し、カメラ軌跡を `imports/pelican-camera/<name>/` に `camera.transform_seq.jsonl` + `pelican.import` manifest として着地させます(DCC への逆輸出)。

各 frame の `rot` は camera の transform の回転で、glTF の camera と同じ規約です(R·(0,0,−1) が視線、R·(0,1,0) が up。[回転を作る所](../../src/core/playback/camerabake.cpp#L104))。CC-1 より前に焼いた列は視線が R·(0,0,+1) の規約なので、`tools/convert_camera_convention.py` が `generator: pelican2-camera-bake` の列の `rot` に Y 軸まわり 180° を掛けて直します(§10.10、[第3章 §3.13](03_project_format.md))。

<a id="pelican_cli-dump-lowered-material---project---materialjson-material-namewp76"></a>

### `pelican_cli dump-lowered-material [--project <dir>] <surface> [material.json] [material-name]`

`.surface` の lowering 結果(std140 レイアウト・生成 GLSL)を stdout に出力します。「B は C の糖衣」の検証・デバッグ用です。
`material.json` を渡すとその material で lowering します(文書に material が複数あれば `material-name` も要ります)。

**`--project <dir>`** を先頭に付けると、`<dir>/project.json` の `basic_config.rendering_config_json` が指す rendering 文書を読み、その root の `material_route_default` で lowering します(engine が起動時に読むのと同じ値。
[project の既定を読む所](../../src/devcli/materialcommand.cpp#L33))。`"forward"` の project では automatic な material の `route:` 行が `reason=default_forward` になります([第6章 §6.7](06_rendering.md))。付けなければ `"auto"` です。この option にはまだ test がありません。

<a id="pelican_cli-dist-config----with-rpcseqplayer---out-wp41"></a>

### `pelican_cli dist-config <project> [--with rpc,seqplayer] [--out <file>]`

> **設計決定(配布は宣言から導出):** 配布ビルドの機能フラグを手で並べさせない。プロジェクトの内容を走査して必要な `PELICAN_WITH_*` を導出し、根拠コメント付きの CMake キャッシュプリセットとして書き出す。

- `PELICAN_WITH_VAT` — GLB に `pelican.vat` extras が実在すれば ON / `PELICAN_WITH_EXR` — `.exr` 参照があれば ON
- `PELICAN_WITH_RPC` / `PELICAN_WITH_SEQPLAYER` / `PELICAN_WITH_OPENXR` — 既定 OFF。`--with rpc,seqplayer,openxr` で明示 ON
- **`PELICAN_WITH_IMGUI` — 常に OFF を書き出す**(配布ビルドに開発 UI を含めない)
- **`PELICAN_WITH_RENDERDOC` — 常に OFF を書き出す**(配布ビルドにcapture integrationを含めない)
- **`PELICAN_WITH_HOT_RELOAD` — 常に OFF を書き出す**(AV-1。配布ビルドはファイルを監視せず、起動時の assets manifest の照合も `--strict-assets` のときだけ。コメントは `# PELICAN_WITH_HOT_RELOAD: OFF - distribution builds watch no files and check assets when they are baked`。[書き出す所](../../src/devcli/distconfig.cpp#L926))。OFF の player の挙動は §10.5 の「配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`)」

```sh
pelican_cli dist-config projects/mygame --out build-dist/preset.cmake
cmake . -B build-dist -C build-dist/preset.cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DSKIP_DEVSTUDIO=ON
cmake --build build-dist --config Release --target pelican_runtime
cmake --install build-dist --config Release --component Runtime --prefix build-dist/package
```

※ AUDIO / PHYSICS 系は導出対象外(常に既定 ON)。B3(embed サブセット)・B4(shaderc OFF 焼き込み配布)は 📐設計のみ。

**asset の照合と `--skip-assets-verify`**: 、dist-config は preset を書く前に、manifest を宣言した asset store ごとに `pelican_cli assets verify --full` と同じ照合(中身の照合込み、hash し直し、`.pelican/local.json` の root の上書きも同じ)を行います([照合する所](../../src/devcli/distconfig.cpp#L835))。
差分があれば、どのファイルかを stderr に出して **exit 1** で、preset は書きません。`--skip-assets-verify` を付けると照合しません。

```text
dist-config: assets_manifest_mismatch@1: asset store 'main' (<store の root>) differs from <manifest>
  content_mismatch: a.bin
  missing: b.bin
  extra: c.bin
dist-config: 3 asset file(s) differ from their manifest; no preset was written. If these files are what ships, write the manifest again (pelican_cli assets manifest --project <project>), or pass --skip-assets-verify
```

| 場面 | 出力 | 終了コード |
|---|---|---|
| 全 store が manifest と一致 | stdout `assets verify: main: 2 files match <manifest>`(store ごとに 1 行) | 0(preset を書く) |
| 差分がある | stderr に上の形。`missing` / `content_mismatch` / `extra` に限らず、`assets verify` が数える全種(`case_mismatch` / `unreadable` / `invalid_manifest` を含む)で失敗 | 1(preset を書かない) |
| `--skip-assets-verify` | stdout `assets verify: skipped (--skip-assets-verify)` | 0 |
| manifest を宣言した store が無い | stdout `assets verify: no asset store declares a manifest; nothing to verify` | 0(前と同じ) |
| 照合自体ができない(manifest が読めないなど) | stderr `dist-config: assets_verify_failed@1: the asset stores could not be verified: …` と `--skip-assets-verify` の案内 | 1 |

- 引数や project の誤りなど、ほかの失敗は前と同じ終了コード(-1)です。
- `projects/example` は manifest を宣言するので、dist-config も照合し、食い違えば exit 1 です。

<a id="pelican_cli-vrm-dump-wp111"></a>

### `pelican_cli vrm dump <model.vrm>`

VRM 1.0(`VRMC_vrm`)の humanoid / expressions / lookAt を検証して canonical dump を出します
(レンダラへの適用はしません)。引数は `dump` と 1 ファイルちょうどで、それ以外は usage を出して失敗します。
詳細は [第5章](05_assets.md)。

<a id="pelican_cli-package-installlistinspectx-1"></a>

### `pelican_cli package install|list|inspect`

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

<a id="pelican_cli-graph-catalogag-0"></a>

### `pelican_cli graph-catalog`

引数を取りません。**登録済みグラフモジュールのカタログ**を `pelican.graph_catalog` v1 の JSON で stdout に出します ([実装](../../src/devcli/graphcatalogcommand.cpp#L10))。モジュール作者が engine のソースを読まずに 型・操作・sink を知るための口で、`get_graph_catalog` RPC が返すのと同じ値です(RPC はこれに `material_pass_contracts` / `material_output_formats` を足して返します。EL-4 で同じく足した `engine_shaders` は 別の RPC `get_engine_shaders` が返します。CLI はモジュールのカタログだけを印字し、この 2 つも engine の shader の一覧も出しません。[第13章](13_editor.md) §13.5)。op ごとの `required_parameters`と `parameters` / `parameters_complete`、`placement` と parameter の `choices_from` などの CT-4 の鍵はモジュールのカタログの側の鍵なので、CLI にも出ます([第6章](06_rendering.md) §6.17「catalog の読み方」)。

```sh
pelican_cli graph-catalog
```

モジュールは既定 build で **5 本**(`pelican.value` / `pelican.audio` / `pelican.render` / `pelican.pose` / `pelican.ui`)、
`PELICAN_WITH_ANIMATION=OFF` では `pelican.pose` が、`PELICAN_WITH_AUDIO=OFF` では `pelican.audio` が消えて 1 本ずつ減ります。
`pelican.value` / `pelican.render` / `pelican.ui` の 3 本は常に登録されます(`pelican.ui` は GP-3 で加わった `ui.value` / `ui.visible` の sink。[第7章](07_input_ui.md) §7.4)。この個数と、各操作が
`domain` / `evaluation` / `change` / `sink` を持つことは
[`run_devcli_graph_catalog.cmake`](../../test/run_devcli_graph_catalog.cmake#L24)(ctest 名 `devcli_graph_catalog`)が
固定しています。出力はモジュール名・操作名・型・sink のどれも**名前順**です([catalog()](../../src/project/graphmodule.cpp#L713) が名前を鍵にした `std::map` を走り、型と sink は明示的に sort します)。登録順に依存しません。

<a id="pelican_cli-image-ktx2---out----format----raw----width-w---height-h----hdr-ia-1"></a>

### `pelican_cli image ktx2 --out <file.ktx2> --format <VK の名前> (--raw <file> --width W --height H | --hdr <file.hdr>)`

非圧縮で mip 1 段の 2D の KTX2 を書きます(。[`runImageCommand()`](../../src/devcli/imagecommand.cpp#L118))。graph の `render.image` の `initial: {asset}`([第6章](06_rendering.md) §6.18「persistent の image を画像資産から始める」)に渡す float の画像を、外の KTX2 の道具なしで作るための口です。拡大縮小と変換はしません。

```sh
pelican_cli image ktx2 --out lut.ktx2 --format R32_SFLOAT --raw lut.raw --width 64 --height 64
pelican_cli image ktx2 --out sky.ktx2 --hdr sky.hdr
```

- `--format` は VkFormat の名前から `VK_FORMAT_` を除いた綴りで、`R8G8B8A8_UNORM` / `R8G8B8A8_SRGB` / `R16_SFLOAT` / `R16G16_SFLOAT` / `R16G16B16A16_SFLOAT` / `R32_SFLOAT` / `R32G32_SFLOAT` / `R32G32B32A32_SFLOAT` のどれかです。BC の format と知らない名前は断ります。
- `--raw` は texel の bytes(little endian、行は詰める)で、`--format`・`--width`・`--height` が要ります。bytes の数が `width × height × texel の bytes` と違えば書きません。
- `--hdr` は Radiance の `.hdr` を stb_image で `R32G32B32A32_SFLOAT` に読み、大きさは file から採ります。`--format` は省けて(既定 `R32G32B32A32_SFLOAT`)、ほかの format と `--width` / `--height` は断ります。
- `--raw` と `--hdr` はどちらか 1 つだけです。
- 成功すると `ktx2 <format> <W>x<H> -> <out>` の 1 行で終了コード 0、失敗は stderr に理由で -1 です。
- **書き方**: `<out>.tmp` に書いて閉じてから `<out>` へ置き換えます([image ktx2 の AB-2 の書き出し](../../src/devcli/imagecommand.cpp#L110))。動いている engine が古い texture を見張り・読んでいても、書きかけの file は見えません(コードの注記)。Windows で読む側が開いていて置き換えが断られると、50 ms から倍々に合計 2 秒までやり直し、それでも開かれたままなら `cannot replace <out>: another handle held it for <N> ms (...)` の文で -1 です。一時 file は消え、元の file は残ります。やり直しは `bake_audio_analysis` と同じ関数です([第5章](05_assets.md) §5.8 の「読む側が開いている間の置き換え」)。
- 書いた file は engine の KTX2 の読み込みで読めます([第5章](05_assets.md) §5.2「テクスチャ・画像と atlas」の KTX2 の対応範囲)。test: `ia1_ktx2_cli_test` の [「IA-1 pelican_cli image ktx2 writes --raw and --hdr files the loader reads」](../../test/ia1_ktx2_cli_test.cpp#L97)。

<a id="pelican_cli-audio-analyze---asset----out--optionsab-1"></a>

### `pelican_cli audio analyze --asset <file> --out <file.ktx2> [options]`

音(WAV / Ogg Vorbis)を解析して、rms・peak・帯域の値の表(`R32_SFLOAT` の KTX2)と、隣の manifest `<out>.import.json` を書きます(。[`runAnalyze()`](../../src/devcli/audiocommand.cpp#L33))。RPC `bake_audio_analysis` と同じ関数を呼び、同じ settings を受けます。表の形・manifest・settings の既定と範囲は [第5章](05_assets.md) §5.8「音の解析を焼いた表」です。

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
- 表と manifest の置き換えは上の `image ktx2` と同じやり直しを通ります。表か manifest が開かれたまま 2 秒を越えると、stderr は `audio.analysis_failed@1` の fields に `"reason":"output_locked"` と `"path"`(開かれていた方の path)が付いた形です([第5章](05_assets.md) §5.8 の拒否の表)。
- `pelican_cli` は engine を link しないので、engine の decode(`src/core/audio/sounddecode.cpp`)と bake(`audioanalysis.cpp`)を自分の source に入れ、miniaudio の実装を自分の unit(`src/devcli/miniaudioimpl.cpp`)で持ちます([`src/devcli/CMakeLists.txt` の AB-1 の部分](../../src/devcli/CMakeLists.txt#L28))。

<a id="105-ホットリロードwp9096110147162178"></a>

## 10.5 ホットリロード

保存すれば動いたまま反映される、が現在の開発体験です。基盤は [src/core/watch/](../../src/core/watch)(FileWatcher + ReloadService。デバウンス 200ms、適用は**次フレーム先頭で一括**、mid-frame 差し替えなし)。

| 対象 | 状態 | 挙動 |
|---|---|---|
| シェーダ / `.surface`(+ `.material.json` の cross-file) | WP108 | 影響する全バリアント・パイプラインを準備してから atomic に公開。失敗時は旧世代維持 + WARNING |
| テクスチャ(PNG / EXR / KTX2) | WP100 | 同 shape は in-place、shape 変化は再バインド(実測 ~1ms) |
| マテリアル値(`.material.json`) | WP105 | **同レイアウトのみ** SSBO 値更新(レイアウト変化は WARN + 旧値継続 🚧) |
| モデルコンテナ(.glb / .gltf / .vrm) | WP110/147 | 同一コンテナの全 fragment + live instanceを1 transactionで再構築。animationは対象asset generationだけ更新し、別assetは継続 |
| VRMA source/profile | 部分WP178 | generation/stale/rebind entry pointは済。FileWatcher/loaderからの自動配線は未 |
| ゲームロジック DLL | WP90/162 | 下記。candidateを旧DLLと並存side-decodeしてから切替 |
| input actions / profile(HR2-I)、pelican.ui(U3) | 📐 | backlog Tier 1 |
| rendering config |  | ディスク経由は `pelican.render_pipeline` 参加者が拾う。加えて Studio から `features[]`・feature の scalar パラメータ・**共通グラフ文書**(`nodes` / `definitions`)を編集して再起動なしで反映できる(§13.5)。どれも engine がファイルを所有し、編集側は触りません |
| scene JSON / project.json | 対象外 | 設計上の決定(再起動)。ただし project.json の `hot_reload` / `asset_stores` は、RPC `edit_project_settings` で書かせればその場で反映される(PJ-1。§10.3) |

- **有効条件**: ウィンドウモードのみ(既定 ON・専用フラグなし)。リプレイ / `--strict-assets` / rpc 駆動中は中央ゲートで無効。headless では自動監視しません。project.json に `hot_reload`(監視する素材とフォルダの配列)を書くと、**列挙したものだけ**を監視し、rpc 駆動中(Studio の engine)でもそこは監視します。空の配列なら監視しません。リプレイ / `--strict-assets` / headless は宣言があっても無効です。書き方・拒否・`--rpc` の下で反映されないもの(render 文書)は [第3章](03_project_format.md) の「hot reload で監視するもの」。宣言は起動時に読みます。動いている engine には `edit_project_settings` で宣言を差し替えられ、watcher はその場で作り直されます([第13章](13_editor.md#projectjson-の宣言を書き換えるpj-1) §13.5 の「project.json の宣言を書き換える」)。
- **配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`)AV-1**: dist-config が常に OFF を出す build(§10.4)では、ファイルを監視せず(FileWatcher を作らない)、game logic の DLL の更新も見に行きません。`get_status.reload` は `state: "disabled"`、`error` と `runtime."pelican.shaders".gate_reason` は `"hot reload build disabled"` で、この理由は他のどの理由よりも先に出ます([gate の 1 か所](../../src/core/watch/reloadgate.cpp#L49))。project.json の `hot_reload` は読みます(形の拒否は同じ)が効かず、宣言があれば起動時に INFO を 1 行 `project.json hot_reload is ignored: this binary was built with PELICAN_WITH_HOT_RELOAD=OFF` 出します([出す所](../../src/core/watch/reloadservice.cpp#L971))。F5・`reload_game_logic`・`reload_shaders` の明示の reload は AV-1 で変えていません(`reload_shaders` を断るのは今も replay と `--strict-assets` だけ。OFF の build で実際に走らせての確認はしていません)。起動時の assets manifest の照合も `--strict-assets` のときだけになります(§10.4 の `assets`)。ソースは build から外していません(FileWatcher のコードは binary に残ります)。
- **fail-soft**: 壊れたファイルを保存しても旧リソースで動き続け、名前入り WARNING と `get_status.reload.last_reload_error` に出ます。修正して保存し直せば回復します。render 文書(`--rpc` の無い起動だけ。HW-1)と shader の読み直しの失敗は `get_problems` の一覧にも `source: "hot_reload"` で残り、通れば消えます([第6章](06_rendering.md) §6.16 の「engine が持つ今の問題の一覧」)。

<a id="ファイル監視の走査fw-1"></a>

### ファイル監視の走査

監視は「変わったファイルだけを読む」形です([`FileWatcher`](../../src/core/watch/filewatcher.hpp#L69))。

- **走査**は directory の一覧の size と更新時刻(mtime)を前回と比べるだけで、hash(SHA-256)するのは size か mtime が違うファイルだけです。変更の無い polling の tick は 1 ファイルも hash しません。
- **起動時はどのファイルも hash しません。**そのため、起動後に各ファイルを最初に保存したときだけは、中身が前と同じでも 1 回 reload されます(engine は起動時の中身を知らないため)。2 回目からは同じ中身の保存は無視されます。
- mtime が走査の時刻から **100 ms 以内**のファイルは size / mtime を信用せず、次の走査で読み直します(同じ時刻の刻みの中で同じ大きさに書き直された保存を取りこぼさないため。[`settled_after`](../../src/core/watch/filewatcher.cpp#L104))。FAT(刻み 2 s)や時計のずれた network share では、同じ刻みの中に同じ大きさで 2 回保存すると 2 回目を取りこぼすことがあります。
- project 直下の **`.pelican/`**(shader cache・hash cache・transaction・log)は走査せず、そこへの書き込みだけの通知では走査も起きません。
- 走査が終わるたびに、hash したファイルがあれば(起動後の最初は 0 件でも)INFO を 1 行出します: `file watcher: scanned <件数> files, hashed <件数> (<x.x> MB) in <ms> ms`([出す所](../../src/core/watch/filewatcher.cpp#L756))。

`get_status.reload` で監視の様子が見えます([`ReloadService::statusJson()`](../../src/core/watch/reloadservice.cpp#L1502))。

| field | 中身 |
|---|---|
| `state` | `disabled` / `reconciling` / `watching` / `polling` / `degraded` |
| `epoch` | gate の世代(開閉のたびに進む) |
| `error` | 監視の異常、または閉じている理由(`"rpc driver"` / `"hot_reload empty"` / `"replay"` / `"strict assets"` / `"shader_hot_reload disabled"`、AV-1 の配布 build では `"hot reload build disabled"`)。無ければ `null` |
| `applied` / `failed` / `last_reload_error` | 適用した数・失敗した数・最後の失敗 |
| `participants` / `runtime` / `model_catalog` | 登録された参加者、runtime の参加者の状態、model の目録 |
| `watch` | HW-1。正規化した `hot_reload` の宣言。書いていなければ `null`、空の配列なら `[]` |
| `scans` / `hashed_files` / `hashed_bytes` | HW-1(数えるのは FW-1)。起動からの累計: 終わった走査の回数・hash したファイルの数・hash した byte 数。watcher がまだ無ければ 0。PJ-1 の `edit_project_settings` が watcher を作り直しても、前の watcher の分を足して数え続けます([第13章](13_editor.md#projectjson-の宣言を書き換えるpj-1) §13.5 の「project.json の宣言を書き換える」) |

### ゲームロジック DLL のホットリロード(WP90/162 = G2/BEH1)

`-DPELICAN_PROJECT=<dir>` でプロジェクトの `code/` は **`pelican_game_logic.dll`** としてビルドされます(静的リンクからの移行 — [第8章](08_gameplay.md))。リロードのトリガーは 3 通り:

1. ウィンドウモードでの自動検出(DLL の更新を監視)
2. **F5** で強制リロード
3. rpc `reload_game_logic`

適用方式は **full-reset**: system state/ECS/behaviorを破棄して現在sceneを
再構築し、hot stateは維持しません。ただしcandidate DLLは旧DLLと並存する
registration-only phaseでABI、schema fingerprint、type継続性、全authored
behavior paramsをside-decodeします。不適合なら旧DLL/runtimeを一切変えず拒否します。

<a id="起動高速化wp82"></a>

### 起動高速化

- **シェーダディスクキャッシュ**: `<project>/.pelican/shader_cache/`(SHA-256 キー)。2 回目以降のシェーダコンパイルはほぼゼロに。破損しても WARN 1 行で通常コンパイルに戻ります。
  - 環境変数 **`PELICAN_SHADER_CACHE_DIR`**(空でないとき)を設定すると、cache はその directory に置かれます([読む所](../../src/core/shader/shadercompiler.cpp#L677))。移すだけで、project を持たない compiler に cache を与えることはしません。
  - 鍵は中身から作られ、project のファイルは絶対 path ではなく「include root の番号 + root からの相対 path」で名付けます。中身と相対位置が同じなら、別の場所にある project とも同じ鍵になり、1 つの directory を共有できます。engine に埋め込まれた shader が 1 byte でも違う build、shaderc の版や compile option の違う build とは鍵が分かれます。TS-1 で鍵の作り方が変わったので、**既存の project の cache は TS-1 の後の最初の起動で一度だけ全部外れます**。
  - TS-2 で鍵の計算の仕方(SHA-256 の実装、include の走査、include root の列挙)を軽くしましたが、**鍵の値は変わらない**ので、既存の cache はそのまま当たります。
  - engine 自身は cache の directory を掃除しません。自分で `PELICAN_SHADER_CACHE_DIR` に決めた directory と project の `.pelican/shader_cache` は増え続けるので、手で消します。GPU テストの launcher が自分で決めた共有 directory だけは、launcher が 1 日 1 回掃除します(§10.9)。
- **並列モデルロード**: CPU 側 prepare を並列化(GPU commit は宣言順直列 = 決定性維持)。起動時に 1 行レポート(`startup: ...`)が出て、`get_status.startup` でも見えます。

<a id="106-imgui-開発者-uiwp8586159164170"></a>

## 10.6 ImGui 開発者 UI

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

Studio の panel ごとの操作は [第15章 Studio の操作](15_studio.md) にあります。

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

`--tab` が取るのは `frame_plan` / `render_features` / `rendering_settings` / `graph` の 4 つ(`rendering_settings` は RSX から。[第13章](13_editor.md)「Studio の Rendering settings タブ」) ([一覧](../../src/devstudio/view/mainwindow.cpp#L2291))。**未知の名前は window を作る前に console へ 理由を出して終了コード 2** です([検査](../../src/devstudio/main.cpp#L36)) —— window を開いて「たまたま前にあった タブ」を見せると、綴り間違いが成功に見えるためです。`--tab` は `openProject` の**後**に適用されます ([順序](../../src/devstudio/view/uimain.cpp#L78)) —— project を開くとその project のツール配置が復元され、 どのタブが前かが変わるからです。

見た目は **View ▸ Theme ▸ Light / Dark** で切り替えます(N-5。CLI の引数ではありません)。選択は QSettings の `studio/theme` に残り、**再起動なしで**窓と 2 つのノードキャンバスに当たります([第13章 §13.5](13_editor.md))。

`--automation <名前>` は **GUI を外から駆動するための口**です。flag が無い起動では socket は作られません。
命令・名前の付け方・シナリオの書式・driver(`tools/studio_drive.py`)・report の読み方は [test/studio_scenarios/README.md](../../test/studio_scenarios/README.md) が正です(この節にあった段落のうち英語の節に無いものは、README の末尾の「第13章・第10章から移した記述」)。
シナリオの目録と画素を読む点の記録は [Studio シナリオの記録(2026-10)](../history/studio_scenarios_2026-10.md)、Studio の操作は [第15章](15_studio.md) です。

> **設計決定(D0: エディタ特権の禁止・2026-07-08):** devstudio は公開契約(rpc / pelican_project / データ形式)の上に建つ 1 クライアントであり、エンジン内部への裏口 API を持たない。編集操作はまず rpc メソッドとして定義し、devstudio はそれを呼ぶだけ。

project を開くと、中央 viewport は同じ project を `--rpc --project <root>` と editor feature overlay 付きの `pelican_player` 子 process として起動し、Windows の native HWND を Qt host へ再親付けします。player が自分の swapchain へ描いて OS が合成するため、pixel readback、 copy、process 間 frame 転送はありません。player が終了しても Studio は残り、`Restart Engine` から再起動できます。既定では Studio と同じ directory の `pelican_player.exe` を使います。project、`--rpc`、`--render-fallback`(BS-1s。いつも付け、`--headless` のときも付ける。[足す所](../../src/devstudio/viewport/studioplayerarguments.cpp#L91))、`--feature-overlay`、`--embedded`(EW-2。窓を非表示で作らせ、埋め込んだ後に Studio が見せる。追加引数に `--headless` があるときは付けない)は Studio が所有し、 `PELICAN_STUDIO_PLAYER_ARGUMENTS` 内の同名指定は除去します。開発時の executable と追加引数の override は次です:

```powershell
$env:PELICAN_STUDIO_PLAYER = "C:/path/to/pelican_player.exe"
$env:PELICAN_STUDIO_PLAYER_ARGUMENTS = "--gpu-labels --vulkan-validation=on --free-camera"
build/dist/Debug/pelican_studio.exe
```

`--project` はここに書きません。Studio が開いた project から自分で渡すため、同名指定は除去されます。

埋め込み player は Studio の環境変数を継承します(QProcess)。音を出させたくないときは、Studio を `PELICAN_AUDIO_BACKEND=null` で起動します(AU-3。`PELICAN_STUDIO_PLAYER_ARGUMENTS` に `--audio-backend null` を書いても同じです)。
シナリオの driver(`tools/studio_drive.py`)は、呼び出し側がこの変数を決めていない(未設定か空)ときに `null` を渡します 。

player のログは **project ごと**に `<project>/.pelican/logs/player.log` へ出ます。Studio が `--log-file` を付けて起動し、ファイルは起動のたびに切り詰められます。`PELICAN_STUDIO_PLAYER_ARGUMENTS` に自分で `--log-file` を書いた場合はそれが優先され、Studio は付け足しません。

Studio が viewport の窓を先に壊した場合、player の main loop は Win32 の窓が無いことを検知して終了します([Window::process()](../../src/core/os/window.cpp#L374))。

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
あります([置き場](../../src/devstudio/view/inspectorwidget.cpp#L308))。選択オブジェクトを owner とする
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
[構築](../../src/devstudio/view/mainwindow.cpp#L632))。**Graph は新しい dock ではなくこの dock の 3 枚目**です ——
dock を増やすことは「見失う場所」を増やすことなので、同じレンダー設定を語る 3 つの見方を 1 箇所に置いています。

**Frame Plan タブ**は実行中 player の公開 `get_frame_plan` を読む調査面で、**9 枚のタブ**を持ちます ——
logical graph / passes / resources / physical plan / decisions / backends / barriers / materials / raw JSON。
**タブは掴んで並べ替えでき、その順序は保存されます**(下記のツール配置)。

Passes の先頭行を上から読むと実行順が分かり、同じ行に入力／出力 target、 種別(render / compute / anchor / snapshot copy / output transform)が出ます。行を開くと履歴入力、 color/depth attachment の load/store、前後 barrier、material filter、execution resource use が見えます。
Resources、Barriers、Materials では writer/reader、hazard edge、公開された material route を逆引きでき、 上部 filter は折りたたみ配下も検索します。**Physical plan は alias group と scope をそのまま出します。**Backends のタブで backend の候補を pin し、Physical plan のタブで物理の計画を凍結(Freeze / Remove / Eject again)でき、Physical plan には常駐 BLAS の節と compile の書き換え(subgraph の置き換え・graph transform・render strategy)の節も出ます([第13章](13_editor.md)の「下の層の決定を Frame Plan から書く」)。

**Logical graph は左右 2 面**です。左のキャンバスでノードを選ぶと、右の詳細ペインにそのパスの行が出ます。
境目は掴んで動かせ、位置は保存されます。
**ノードの見た目は Graph タブと同じです** —— 見出し帯(名前と種別)+ GPU 計時の行 + 書く target の行 + 読む resource の行で、行の端のソケットは resource の種類ごとに形が違います(image = 丸 / buffer = 四角 / acceleration structure = 菱形 / swapchain = 六角 / history = 縁の太い丸)。辺はソケット中心をつなぎ、 ラベルは tooltip になっています。**ホイールでカーソル中心のズーム(0.25〜4.0)、中ボタンまたは Space + 左でパン、 `F` と右クリックの Fit view** は 2 つのキャンバス共通です(詳細は[第13章](13_editor.md) §13.5)。

**Features タブが編集面です。**著作 config の `features[]` を読み、engine が返したカタログから選んで
足す・外すができ、**再起動なしで反映されます**。カタログは engine が可用性を判定して返すので、
モジュールの動的生成が要る feature は理由付きで断られます(§6.5)。カタログの各項目には
`source`(`engine` / `package`)が付き、install した package の feature も同じ一覧に並びます。RPC の契約は §13.5。

ボタンは 3 段です。著作一覧の下の行が **Remove selected** / **Parameters...** / **Make editable...**、
カタログの下が **Add selected**、最下段が **Refresh** / **Procedural assets...** / **Tasks...** /
**Fork preset...** / **Region replacements...** / **Callable Records...** / **Scene AS...** / **Apply**
([構築](../../src/devstudio/view/renderfeatureswidget.cpp#L2491))。**Parameters...** は選択 feature の
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
([定義](../../src/devstudio/view/graphcanvasreadcapability.hpp#L36))は汎用の RPC メソッドを**表現できない**形で、
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

共通 editor 基盤は authoring document、typed query/edit、CAS/journal、undo/redo、atomic save、snapshot import、watch、isolated preview を備えます。Studio は process/window の接続、picking、選択同期、Frame Plan、gizmo のドラッグ、起動主体が宣言する editor feature overlay を利用します。複数 client WebSocket は未接続です。

**まだ無いもの(名前で書きます)。**

- **時間軸の view と、definition scope のノード値表示はありません**(AG-3b)。engine 側の RPC
  (`get_graph_node_values`)は利用できます。
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
- **GPU テストの共有 shader cache**: `pelican_gpu_lease_run` は子を起こす前に、`PELICAN_SHADER_CACHE_DIR` が未設定なら `%LOCALAPPDATA%/pelican/test_shader_cache`(Windows 以外は `$XDG_CACHE_HOME/pelican/test_shader_cache`、無ければ `~/.cache/pelican/test_shader_cache`)を作って設定します([`prepare_shader_cache_directory()`](../../test/gpu_lease_run.cpp#L136))。環境変数として子へ継承されるので、全 GPU テスト・全 worktree・Studio のシナリオが起こす player が 1 つの cache を共有します。既に設定されていれば触らず、directory を作れなければ設定しません(各 project の `.pelican/shader_cache` のまま)。`PELICAN_GPU_LEASE=off` の素通しでも設定されます。
  - project の cache directory を列挙して miss を数える GPU テスト(`WP366 renderer A B recovers q r group miss and WP384 preserves frame publication`)は、テストの中で変数を外しています。lease_run を通らない非 GPU のテストには変数が付きません。開発者が変数を全体に設定すると、「初回は miss」を前提にする非 GPU のテスト(`shader_library_test` の hot reload の 1 本)が赤になりうる、と TS-1 の報告にあります。
  - launcher は自分で決めた共有 directory を 1 日 1 回掃除します([`sweep_shader_cache_directory()`](../../test/gpu_lease_run.cpp#L101))。印のファイル `last_sweep.stamp` の更新時刻から 24 時間たっていれば、directory 直下の普通のファイルのうち 14 日書かれていないものを消します(下の directory には入りません)。失敗は全部無視し、テストの起動は止めません。呼び手が `PELICAN_SHADER_CACHE_DIR` を決めていれば何もしません。cache に当たってもファイルの更新時刻は変わらないので、使っている entry も 14 日ごとに 1 回 compile し直されます。
- 結合テスト: `test/run_*.cmake` が player / cli を子プロセス起動して検証。例: `run_devcli_assets`/ `run_event_schema_compile`/ `run_dump_lowered_material`/ `run_devcli_gltf_extract`/ `run_spvlink_golden`/ `run_devcli_rules_import`/ `run_devcli_bake_camera` + `run_input_record_replay_headless`/ `run_ui_u2_rpc_replay`など。
- 統合テスト: [`run_devcli_graph_catalog.cmake`](../../test/run_devcli_graph_catalog.cmake)(ctest 名 `devcli_graph_catalog`。`pelican_cli graph-catalog` の出力を検査し、**さらに `docs/v0_freeze.md` の操作表と突き合わせます** —— 表は `<!-- graph-op-table:begin -->` / `:end` のコメントで区切られていて、誰も比べない写しを 2 つ目の正にしないための仕掛けです)、`package_install_smoke`(X-1。install → カタログ → 描画 → uninstall を実 player で。`gpu` ラベル)、[`run_project_catalog_smoke.cmake`](../../test/run_project_catalog_smoke.cmake)(出荷 project の走査。`project.json` の `requires` が満たせない project は**名前を出して skip** し、player の中で落ちません)。
- **Studio のシナリオ(Q-1)**: `test/studio_scenarios/*.json` は 1 本が 1 つの ctest `studio_scenario_<名前>` になり
([登録](../../test/CMakeLists.txt#L3673))、`pelican_gpu_lease_run` 経由の **GPU lane** で走ります —— 実物の窓に
実物の player が埋まるので `QT_QPA_PLATFORM=offscreen` は掛けません。Studio target と Python 製テストを有効にし、
Python 3.11 以上を用意します。`uv` は不要です。完全な GPU gate はシナリオの未登録・skip を失敗にするため、
`SKIP_DEVSTUDIO=OFF` と `PELICAN_PYTHON_TESTS=ON` を指定します。[`run_studio_scenario.cmake`](../../test/run_studio_scenario.cmake#L30) が fixture
`test/fixtures/studio_automation_graph` を temp に 2 つ複製し、Qt を PATH に置いて driver を起動します。
対象の一覧は [`test/studio_scenarios/`](../../test/studio_scenarios/) が正本です。成果物(`report.json`、
`studio.log`、スクリーンショット)は `build/test_artifacts/studio_scenarios/<名前>/` に残ります。
- ゴールデンイメージテスト(**`inventory.json` の `cases[]` に登録されたものだけが対象。ディレクトリを置くだけでは走らない**。件数は [第6章](06_rendering.md) §6.10 が正)。
- ctest 非登録のスモーク: `run_build_units_smoke.cmake`(IMGUI / PHYSICS 系を含む単独 OFF ビルド検証。`PELICAN_WITH_ANIMATION=OFF` は [`run_animation_smoke()`](../../test/run_build_units_smoke.cmake#L1210) が回し、[`verify_animation_absent()`](../../test/run_build_units_smoke.cmake#L1143) が「消えているべきものが消えている」を確かめます)、`run_project_code_smoke.cmake`、`run_package_template_smoke.cmake`(X-1。package 入りの一時 project を configure から build して headless で走らせるので、**primary worktree の依存ソースを** `-DPELICAN_PACKAGE_TEMPLATE_SMOKE_DEPS=<primary>/build/_deps` で渡します)。
- **CI**: push/PRのWindows CPU gateに加え、手動/週次の   build-unit OFF/Jolt/project-code/clean-clone matrixがあります。GPU testは   `gpu` labelで除外し、`SKIP`はexact allowlist、retryなし。GPU CI2は未。

## 10.10 リポジトリ保守スクリプト(`tools/`)

`tools/` はエンジンの実行物ではなく、**この repo を触る人(と coding agent)のための道具**です。
どれも production にはリンクされません。

| スクリプト | 何をするか |
|---|---|
| [`pelican_rpc.py`](../../tools/pelican_rpc.py) | Python 3.12 標準ライブラリだけの JSON-RPC クライアント(WP160。§10.3) |
| [`worktree.sh`](../../tools/worktree.sh) | worktree ごとの configure / build / test ラッパ |
| [`codex_bg_build.sh`](../../tools/codex_bg_build.sh) / [`codex_bg_status.sh`](../../tools/codex_bg_status.sh) | 長い build を切り離して起動し、あとから状態を見る |
| [`public_api_inventory.py`](../../tools/public_api_inventory.py) | 公開 context API の exact inventory を生成・照合 |
| [`doclink.py`](../../tools/doclink.py) | docs 内の `#L<行番号>` リンクを追随させる |
| [`convert_camera_convention.py`](../../tools/convert_camera_convention.py) | CC-1 より前に書いた project の camera を今の向きの規約へ変換する(`uv run tools/convert_camera_convention.py [--dry-run] [--assume-up X,Y,Z] <project dir>...`。何を書き換えるかは [第3章 §3.13](03_project_format.md) の「camera の向きの規約の変換」) |

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

各行は `getter` / `mutator` / `host_entrance` に分類され、**mutator と host_entrance は [`sceneparticipantmanifest.cpp`](../../src/core/loader/sceneparticipantmanifest.cpp) の runtime manifest に 対応する行が無いと生成が失敗します**。逆に manifest にあって inventory に無い API も失敗です (双方向。WP380e)。ゲーム API に mutator を足したら、scene transaction 側の分類も同時に足す、 という結び付きをこの 1 本が強制しています。

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
