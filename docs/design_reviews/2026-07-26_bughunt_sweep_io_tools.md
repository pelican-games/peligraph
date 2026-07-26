# Bughunt sweep: I/O / tools 領域

- 実施日: 2026-07-26
- 対象:
  - `src/core/communication/`
  - `src/core/ui/`
  - `src/core/imgui/`
  - `src/core/phys/`
  - `src/core/audio/`
  - `src/core/persistence/`
  - `src/core/os/`
  - `src/devcli/`
  - `src/devstudio/`
- 方法: 読み取り専用の静的スイープ。対象 125 ファイルを読み、T1–T17 の各類型を明示的に照合した。候補については呼び出し元・呼び出し先、固定された依存ライブラリの契約、成功／失敗の伝播先まで追跡した。
- 非実施: ソース変更、ビルド、テスト、実行による故障注入。目標文書の指示どおり実施していない。
- 対象外を維持: `src/core/renderingpass/`, `src/core/vkcore/`, `src/project/`
- 結果: 高 4 件、中 9 件、低 3 件。T1–T17 外の新規類型を 3 個（T18–T20）抽出した。

## T1–T17 網羅表

件数は「独立した finding family」の数である。同一 finding が複数類型に該当する場合は各類型に計上した。

| 類型 | 件数 | 判定 |
|---|---:|---|
| T1 wrapper contract mismatch | 0 | この領域には T1 の該当なし |
| T2 guard placed after operation | 0 | この領域には T2 の該当なし |
| T3 early return/filter preempts later case | 0 | この領域には T3 の該当なし |
| T4 library semantics makes branch constant | 1 | M-5。新規 `ma_sound` に対する開始エラー分岐は実質的に死んでいる |
| T5 try/catch around `noexcept` ABI | 0 | この領域には T5 の該当なし |
| T6 disjoint-set intersection by invariant | 0 | この領域には T6 の該当なし |
| T7 downstream fallback already precluded | 0 | この領域には T7 の該当なし |
| T8 stale `GetLastError` / `dlerror` / `errno` | 1 | M-7。失敗時の `errno` を複数の cleanup 後に読む |
| T9 swallowed cleanup failure reports success | 1 | M-8。終了時の入力記録保存失敗を空 catch で消す |
| T10 recreation reset omission | 0 | この領域には T10 の該当なし |
| T11 partial-failure rollback missing | 3 | H-2, M-6, L-1 |
| T12 environmental condition unhandled | 1 | M-7。POSIX `waitpid` の `EINTR` を通常の待機継続として扱えない |
| T13 ImGui `CheckVkResultFn` missing | 1 | H-1 |
| T14 transport success confused with semantic success | 2 | H-3, M-5 |
| T15 nlohmann `value()` wrong-type throw | 4 | M-1, M-2, M-3, M-4 |
| T16 sibling route missing guard | 1 | M-9。監視対象と実際の QML ロード経路が別 |
| T17 epoch off-by-one | 0 | この領域には T17 の該当なし |

## Findings

### 高

#### H-1 [T13] ImGui Vulkan backend の結果フックが未設定で、Vulkan 失敗後も無効ハンドルを使用し得る

- 場所: `src/core/imgui/imguisystem.cpp:313-330`
- 依存版: `src/core/imgui/CMakeLists.txt:3-6` が Dear ImGui `f5befd2d29e66809cd1110a152e375a7f1981f06`（v1.91.9b）を固定
- 確信度: HIGH
- 種別判定: 実害のある穴

`ImGui_ImplVulkan_InitInfo init_info{}` はゼロ初期化され、instance/device/queue 等は設定されるが `CheckVkResultFn` は一度も設定されない。固定版 backend の [`check_vk_result`](https://github.com/ocornut/imgui/blob/f5befd2d29e66809cd1110a152e375a7f1981f06/backends/imgui_impl_vulkan.cpp#L397-L404) は callback が null の場合に何もしない。これは単なる診断欠落ではない。同版の [`ImGui_ImplVulkan_AddTexture`](https://github.com/ocornut/imgui/blob/f5befd2d29e66809cd1110a152e375a7f1981f06/backends/imgui_impl_vulkan.cpp#L1228-L1253) は未初期化の `VkDescriptorSet` に対して `vkAllocateDescriptorSets` を呼び、結果をこの no-op 経路へ渡した後、その値で `vkUpdateDescriptorSets` を実行して返す。descriptor pool 枯渇や device loss では、失敗を止めず未定義値を Vulkan API に渡し得る。

ローカルの `ImGui_ImplVulkan_Init` の bool 検査（`imguisystem.cpp:327-329`）は初期化一回だけを覆い、frame 中や texture 追加時に backend 内部で発生する多数の `VkResult` を覆わない。

#### H-2 [T11] preview の実体を確定した後の台帳更新失敗に rollback がなく、RPC 結果と権威状態が分岐する

- 場所: `src/core/communication/editorjournal.cpp:2318-2341`, `2427-2453`, `2457-2492`, `2500-2514`
- 実体側の副作用: `src/core/communication/editorruntimefactory.cpp:1334-1341`
- 確信度: HIGH
- 種別判定: 実害のある穴（後段例外時）

open 経路は `executeLivePreview(...).committed()` を確認した後に preview boundary、epoch、lease、結果 JSON を更新する。commit 経路は `dependencies.execute(execution)` で document projection を確定した後に `Ticket` の構築、transaction ID 割り当て、`finalizeJournal`、tombstone、lease 解放、結果 JSON を行う。abort 経路も復元 projection を確定した後に boundary、epoch、tombstone、lease、結果を更新する。`preview_boundary` の実装は renderer の temporal history を実際に reset する。

これらの後段 callback・コンテナ挿入・JSON 構築・割り当てが例外を出すと、`editorjournal.cpp:2474-2492` は要求だけを `"failed"` に変換する。既に確定した document/runtime projection を逆向きに戻す経路はない。さらに frame 境界の `commitPending()` は最外層で全例外を消す。したがって、例えば commit 済み revision が journal/transaction に記録されない、open 済み preview が失敗応答なのに残る、abort で実体だけ復元され lease が残る、という分岐が継続プロセス内に残り得る。

#### H-3 [T14] CLI の text writer が open 成功だけで書き込み成功とし、破損した権威ファイルにも成功終了する

- 主場所: `src/devcli/importcommand.cpp:147-153`, `338-339`, `372-384`
- 同型箇所:
  - `src/devcli/projectinit.cpp:77-85`, `406-424`
  - `src/devcli/distconfig.cpp:190-199`, `929-941`
  - `src/devcli/bakecameracommand.cpp:99-104`, `156-160`
- 確信度: HIGH
- 種別判定: 実害のある穴

各 writer は `std::ofstream` を `trunc` で開き、`is_open()` だけを確認して `operator<<` 後の stream state を確認しない。iostream は既定では late write error を throw せず fail/bad bit に保持する。このため容量不足、quota、途中 I/O failure では helper が正常 return する。

最も重大な経路では import が既存の `asset_data_file` を先に truncate し、不完全な JSON を残して `registerImportedModels` を成功 return する。その後 `runImportCommand` は “verified / registered” を表示して exit code 0 を返す。project init は実際に書けていない file も `files_written` に加算し、dist-config と bake-camera も出力成功を表示して 0 を返す。ここで観測されるのは「stream へ投入を試みた」という transport 上の継続であり、ファイルが永続化されたという semantic success ではない。

#### H-4 [T18] 終了した音声を回収する経路がなく、再生回数に比例して decoded sample と backend object を保持し続ける

- 場所: `src/core/audio/audio.cpp:317-320`, `338-395`, `435-467`
- 確信度: HIGH
- 種別判定: 実害のある穴

`MiniaudioBackend::voices` は各 voice の `DecodedSound`、`ma_audio_buffer`、`ma_sound` を owning `unique_ptr` で保持する。`play()` は毎回 entry を追加する一方、`stop()` は `ma_sound_stop` だけで erase しない。自然終端の検出は `isPlaying()` が状態を読むだけで回収しない。上位 `Audio::voices` も同様に `playSound()` で追加し、`stopSound()` と `isPlaying()` のどちらも erase しない。

したがって停止済み・自然終了済みの one-shot sound を含め、二つの map と decoded PCM は `Audio` destruction まで残る。これは「handle を後で照会できる」だけの無害な tombstone ではなく、サンプル本体と miniaudio resource を再生回数に比例して保持する決定的な無制限増加である。

### 中

#### M-1 [T15] UI document parser が wrong-type を `UiError` にせず例外送出し、返却契約を破る

- 場所: `src/core/ui/document.cpp:56-69`, `251-254`, `297-303`, `332-346`, `360-380`
- 呼び出し側: `src/core/ui/module.cpp:136-140`
- 依存版: `CMakeLists.txt:143-149` が nlohmann/json v3.12.0 を固定
- 確信度: HIGH
- 種別判定: 実害のある穴

`parseUiDocument` は `DocumentParseResult` に `UiError` を集約して返す API であり、周囲の field は `is_*` 検査後にエラーを追加している。一方 `schema`, `version`, `revision`, `direction`, node の `sampler`, `enabled`, `hit_testable`, `visibility`, `overflow`、layout の各 discriminator は型検査なしで `json.value()` を使う。

nlohmann/json の [`value()`](https://json.nlohmann.me/api/basic_json/value/) は key が欠けた時だけ default を返し、key が存在して要求型へ変換できなければ `type_error` を送出する。例えば `"enabled":"yes"` や `"schema":1` は `TypeMismatch` を持つ `DocumentParseResult` にならず、例外で parser を離脱する。`UiModule` は parse result を `requireDocument` へ渡す契約なので、構造化された複数エラーの経路を迂回する。

#### M-2 [T15] semantic fixture validator が missing/wrong-type を記録した直後に throw し、validator と coverage gate が total function になっていない

- 場所: `src/core/ui/semanticfixture.cpp:23-33`, `70-107`, `165-220`, `347-390`
- 確信度: HIGH
- 種別判定: 実害のある穴

`validateTrace` と `validateLifecycle` は `kind` を `value("kind", "")` で読むため、存在する wrong-type 値で `type_error` になる。さらに `objectClosed` は required field 欠落を issue として記録しても `true` を返す。よって `{"kind":"capture_cancel","widget":"x"}` は `/reason` 欠落を記録した後、`validateLifecycle` が無条件に `j["reason"]` を参照して `out_of_range` を送出する。

公開 API は `std::vector<FixtureIssue>` または `CoverageGateResult.failures` に検証結果を集める形だが、coverage header も `registry.value(...)` / `manifest.value(...)` を型検査なしで呼ぶ。このため malformed fixture/manifest を「invalid と分類して返す」経路が例外に置き換わる。

#### M-3 [T15] editor preview の optional array が wrong-type の時だけ invalidParams ではなく applicationError になる

- 場所: `src/core/communication/editorpreviewservice.cpp:365-395`, `398-446`
- エラー変換: `src/core/communication/editorrpchandlers.cpp:13-35`, `src/core/communication/rpcserver.cpp:737-751`
- 確信度: HIGH
- 種別判定: 実害のある穴

`evalPreview` は object/許可 field を確認した後、`params.value("overrides", Json::array())` と `params.value("queries", Json::array())` を呼ぶ。`renderPreview` も同じ方法で `overrides` を読む。field が欠けていれば default array になるが、存在して object/string 等なら nlohmann `type_error` が出る。これらの呼び出しは `EditorPreviewProjectionError` の catch より前にある。

RPC adapter が invalid params に変換するのは `EditorPreviewError` の schema 系 code、`EditorCommandError::InvalidParams`、`std::invalid_argument` だけである。残った `type_error` は `RpcServer` の一般 `std::exception` catch に入り applicationError になる。同じ field の「欠落」と「wrong-type」で、client schema error が server application failure に化ける。

#### M-4 [T15] dist-config の manifest 選別 discriminator が wrong-type だと skip でなく全走査を中断する

- 場所: `src/devcli/distconfig.cpp:270-283`, `420-456`
- 確信度: HIGH
- 種別判定: 実害のある穴

`collectImportManifestGlbs` は project tree 内の全 `manifest.json` を読み、object でないもの、または `schema` が import schema でないものを `continue` する設計である。しかし判定は `manifest_json.value("schema", std::string{})` なので、`schema` が存在して string 以外なら比較に到達せず `type_error` になる。無関係な manifest を skip する sibling route にだけ型 guard がなく、単一の非 import manifest が dist-config 全体を失敗させる。project JSON の schema 検査も同じ `.value()` だが、こちらは入力自体を拒否する文脈なので、主たる実害は tree-wide manifest 選別経路にある。

#### M-5 [T4, T14] miniaudio の開始 result 検査は fresh voice では死んでおり、null backend の無音成功を app fallback と誤認する

- 場所: `src/core/audio/audio.cpp:323-328`, `338-373`, `397-415`
- 依存版: `CMakeLists.txt:119-140` が miniaudio 0.11.25 を固定
- 確信度: HIGH
- 種別判定:
  - T4 部分: 無害な死に分岐
  - T14 部分: 実害のある穴

固定版の [`ma_sound_start`](https://github.com/mackron/miniaudio/blob/0.11.25/miniaudio.h#L78746-L78771) は null pointer を除き、既に再生中なら success、終端なら seek のみを失敗候補とし、最後は node state を設定して `MA_SUCCESS` を返す。ローカルは直前に新規 `ma_sound` を正常初期化しているため `audio.cpp:366-369` の error branch は通常到達せず、実際に音が出るかの確認にはならない。この死に分岐自体は無害である。

一方 `ma_engine_init(nullptr)` は default backend 列を使う。固定版の [default backend 列生成](https://github.com/mackron/miniaudio/blob/0.11.25/miniaudio.h#L43173-L43235) は `ma_backend_null` までを列挙し、[null backend case](https://github.com/mackron/miniaudio/blob/0.11.25/miniaudio.h#L43332-L43336) も有効である。実 audio backend が一つも初期化できない環境でも null backend が `MA_SUCCESS` を返し得る。ローカルの app-level `NullAudioBackend` fallback は `ma_engine_init` が throw した場合だけなので、この時は到達しない。ログは `"audio backend: miniaudio"`、`playSound()` は有効 handle、`ma_sound_start` は success を返すが音は出ない。library transport 上の成功を「実デバイスで再生可能」という semantic success として扱っている。

#### M-6 [T11] project init の逐次生成に rollback がなく、途中失敗後は同じコマンドで再試行できない

- 場所: `src/devcli/projectinit.cpp:55-85`, `402-424`
- 確信度: HIGH
- 種別判定: 実害のある穴

初回は target directory を作成または空であることを確認し、template files を最終位置へ一つずつ書く。途中の directory creation/open/write が失敗しても、それ以前に作成した file/directory を戻す処理はない。次の試行では `ensureWritableProjectRoot` が残骸を non-empty として拒否する。したがって部分生成は単に「何件か不足した」状態ではなく、同じ init 操作自身が回復できない terminal state を作る。

H-3 の late write error が起きた場合は helper 自体が失敗を観測しないため、部分生成に加えて成功件数も過大になる。

#### M-7 [T8, T12] POSIX process wait が `EINTR` を fatal とし、元の `errno` を cleanup 後に読む

- 場所: `src/devcli/processrunner.cpp:321-345`
- signal 経路: `src/devcli/rulesimport.cpp:56-70`, `499-511`
- 確信度: T8 は HIGH、T12 の実環境発火条件は MEDIUM
- 種別判定: 実害のある穴

POSIX `waitpid(..., WNOHANG)` は signal interruption で `-1` / `EINTR` を返し得るが、loop は `waited < 0` をすべて fatal として process group を kill する。rules import は実際に SIGINT handler を設けて cancellation flag を渡しており、libc の restart 設定に依存して「cancel として後段で扱う」前に fatal wait error となり得る。少なくとも一般 `ProcessRunner` 契約では `EINTR` が未処理である。

さらに最初の `waitpid` 失敗直後の `errno` を保存せず、`kill`、二度目の `waitpid`、二つの thread join を済ませた後に `std::strerror(errno)` を呼ぶ。cleanup syscall が別の失敗をすると、報告されるのは元の wait 原因ではない。T12 の環境条件と T8 の診断汚染は同一 failure path 上の別の穴である。

#### M-8 [T9] active input recording の destructor 保存失敗が完全に不可視になり、正常終了に見える

- 場所: `src/core/os/inputsequence.cpp:335-355`, `365-396`
- 確信度: HIGH
- 種別判定: 実害のある穴

明示的な `stopRecording()` は `writeFile()` の failure を呼び出し側へ伝え、成功後だけ idle に遷移する。対して runtime destruction 時に recording 中なら同じ `writeFile()` を呼んだ後、全例外を空 catch で捨てる。`writeFile()` は directory 作成、open、late write の各 failure を検出できるが、この経路では上位 app/CLI に伝わらない。記録データが保存されていないまま、囲んでいる処理は正常な shutdown／exit を継続できる。

#### M-9 [T16] QML 起動失敗 handler が未使用 engine にだけ接続され、実ロード経路の失敗を検知しない

- 場所: `src/devstudio/view/uimain.cpp:10-27`, `src/devstudio/view/mainwindow.cpp:10-18`
- 確信度: HIGH
- 種別判定: 実害のある穴

`uimain` は local `QQmlApplicationEngine engine` の `objectCreationFailed` を exit(-1) に接続するが、この engine には一度も QML を load しない。実際の画面は `MainWindow` が別の `QQmlEngine` と `QQuickWidget` を作り、`QQuickWidget::setSource` でロードする。この sibling route には status/error の判定がない。

したがって resource URL、import、root object creation が失敗しても、接続済みの `objectCreationFailed` は発火せず、空／error widget のまま window を show して event loop に入る。終了時にも QML 起動失敗を反映した exit code にならない。

### 低

#### L-1 [T11] ImGui 初期化 constructor の途中 throw で既作成 global state が cleanup されない

- 場所: `src/core/imgui/imguisystem.cpp:284-300`, `313-345`
- 確信度: HIGH
- 種別判定: 実害はあるが process-exit に限定されやすい穴

`Impl` constructor は先に `ImGui::CreateContext()` を呼び、次に GLFW backend、最後に Vulkan backend を初期化する。cleanup は `~Impl()` にだけある。GLFW init failure、依存 module access の例外、Vulkan init failure で constructor が throw すると、C++ は未完成 object の `~Impl()` を呼ばないため、作成済み ImGui context と、段階によっては GLFW backend が残る。

現状の主経路では startup failure が process 終了へ波及しやすく、通常運転中の累積 corruption ではないため低とした。分岐自体は死んでおらず、同一 process 内で初期化を捕捉・再試行する呼び出しでは global state が残る。

#### L-2 [T19] detached RPC reader が borrowed stream より長生きできる

- 場所: `src/core/communication/windowedrpchost.cpp:15-28`, `53-71`, `76-103`
- 現行 production 呼び出し: `src/core/appflow/loop.cpp:436-446`
- 確信度: HIGH
- 種別判定: public API 上の実害のある穴。現行 production 呼び出しは安全

shared queue state は `std::istream&` と `std::ostream&` を借用する。destructor は blocking reader を停止できない場合に thread を detach し、shared state だけを thread に残して return する。stream 自体の lifetime は延長しないため、有限 lifetime の stream を渡した caller は host destruction 後に stream を破棄でき、reader が dangling reference へ `getline`／応答出力して undefined behavior になり得る。

現行 production trace は process-lifetime の `std::cin` / `std::cout` を渡すのでこの呼び出しに限れば無害である。しかし `WindowedRpcHost` の公開 constructor は任意の stream reference を受けるため、実装契約全体では lifetime hole が残る。

#### L-3 [T20] devstudio の project name property は QML→backend と backend→QML の両方向が途切れている

- 場所: `src/devstudio/view/ProjectEdit.qml:11-14`, `src/devstudio/view/viewmodel/testbackend.hpp:8-18`, `src/devstudio/view/viewmodel/testbackend.cpp:5-12`
- 確信度: HIGH
- 種別判定: 現行 prototype UI に限定された実害のある穴

QML は `TextField.text: backend.name` という一方向 binding だけで、編集時に `backend.name` を代入する handler がない。ユーザーが TextField を編集しても backend setter へ値は保存されない。逆方向では `Q_PROPERTY(... NOTIFY nameChanged)` を宣言しているが、`setName` は `m_name` を更新するだけで `nameChanged` を emit しないため、backend を直接更新しても既存 binding に通知されない。

class が `TestBackend` で画面も test scaffold であることから影響範囲を低としたが、現在表示される Project Name field のデータ経路としては実際に未接続である。

## 新規類型 T18+

| 類型 | 定義 | 件数 | Finding |
|---|---|---:|---|
| T18 terminal-state ownership retention | resource が terminal state に達しても owning registry から回収されず、操作回数に比例して重い payload を保持する | 1 | H-4 |
| T19 detached worker outlives borrowed dependency | detached worker の state だけ延命し、参照借用した I/O／dependency の lifetime を延長しない | 1 | L-2 |
| T20 declared observable route is unwired | property/observer 契約を宣言しているが、write-back または notify の実経路が接続されていない | 1 | L-3 |

## 読んだファイル

指定対象は以下の 125 ファイルを全件読んだ。

- `src/core/communication/`（17）:
  `CMakeLists.txt`, `editorassetqueryruntime.cpp`, `editorassetqueryruntime.hpp`, `editorcommandservice.cpp`, `editorcommandservice.hpp`, `editorjournal.cpp`, `editorjournal.hpp`, `editorpreviewservice.cpp`, `editorpreviewservice.hpp`, `editorrpchandlers.cpp`, `editorrpchandlers.hpp`, `editorruntimefactory.cpp`, `editorruntimefactory.hpp`, `rpcserver.cpp`, `rpcserver.hpp`, `rpcserver_stub.cpp`, `windowedrpchost.cpp`
- `src/core/ui/`（24）:
  `atlas.cpp`, `atlas.hpp`, `bitmapfont.cpp`, `bitmapfont.hpp`, `CMakeLists.txt`, `commandbuffer.cpp`, `commandbuffer.hpp`, `document.cpp`, `document.hpp`, `drawcommands.cpp`, `drawcommands.hpp`, `gpuabi.hpp`, `inputrouter.cpp`, `inputrouter.hpp`, `layout.cpp`, `layout.hpp`, `module.cpp`, `module.hpp`, `semanticfixture.cpp`, `semanticfixture.hpp`, `types.cpp`, `types.hpp`, `widgetarena.cpp`, `widgetarena.hpp`
- `src/core/imgui/`（13）:
  `assetbrowser.cpp`, `assetbrowser.hpp`, `CMakeLists.txt`, `compiledplanviewer.cpp`, `compiledplanviewer.hpp`, `imguiruntime.cpp`, `imguiruntime.hpp`, `imguisystem.cpp`, `imguisystem.hpp`, `inspector.cpp`, `inspector.hpp`, `planviewer.cpp`, `planviewer.hpp`
- `src/core/phys/`（18）:
  `builtinphysicsprovider.cpp`, `builtinphysicsprovider.hpp`, `CMakeLists.txt`, `joltphysicsprovider.cpp`, `joltphysicsprovider.hpp`, `physicsruntime.cpp`, `physicsruntime.hpp`, `physicsservice.cpp`, `physicsservice_stub.cpp`, `physquery.cpp`, `physquery.hpp`, `physqueryaggregate.cpp`, `physquerycontract.cpp`, `physqueryinternal.hpp`, `physquerysweep.cpp`, `physworld.cpp`, `physworld.hpp`, `physworld_stub.cpp`
- `src/core/audio/`（3）:
  `audio.cpp`, `audio.hpp`, `CMakeLists.txt`
- `src/core/persistence/`（3）:
  `CMakeLists.txt`, `persistence.cpp`, `persistence.hpp`
- `src/core/os/`（9）:
  `actionmap.cpp`, `actionmap.hpp`, `CMakeLists.txt`, `inputsequence.cpp`, `inputsequence.hpp`, `inputstate.cpp`, `inputstate.hpp`, `window.cpp`, `window.hpp`
- `src/devcli/`（22）:
  `assetscommand.cpp`, `assetscommand.hpp`, `bakecameracommand.cpp`, `bakecameracommand.hpp`, `CMakeLists.txt`, `distconfig.cpp`, `distconfig.hpp`, `gltfsceneextract.cpp`, `gltfsceneextract.hpp`, `importcommand.cpp`, `importcommand.hpp`, `main.cpp`, `materialcommand.cpp`, `materialcommand.hpp`, `processrunner.cpp`, `processrunner.hpp`, `projectinit.cpp`, `projectinit.hpp`, `rulesimport.cpp`, `rulesimport.hpp`, `vrmcommand.cpp`, `vrmcommand.hpp`
- `src/devstudio/`（16）:
  `CMakeLists.txt`, `main.cpp`, `model/CMakeLists.txt`, `model/project.cpp`, `model/project.hpp`, `resources/CMakeLists.txt`, `view/CMakeLists.txt`, `view/mainwindow.cpp`, `view/mainwindow.hpp`, `view/ProjectEdit.qml`, `view/ui_depended_setup.cmake`, `view/uimain.cpp`, `view/uimain.hpp`, `view/viewmodel/CMakeLists.txt`, `view/viewmodel/testbackend.cpp`, `view/viewmodel/testbackend.hpp`

実行経路・依存契約の裏取りとして、指定範囲外では次だけを参照した。

- 目標文書: `%LOCALAPPDATA%\Temp\claude\<scratch>\2b01b5c1-9872-4511-acc7-5eb40780aa54\scratchpad\goals\bughunt_sweep_goal.md`
- 先行スイープの類型根拠: `docs/design_reviews/2026-07-26_bughunt_findings_raw.md`
- 依存版の固定: `CMakeLists.txt`
- `WindowedRpcHost` の production lifetime trace: `src/core/appflow/loop.cpp`
- 固定版 upstream:
  - Dear ImGui `f5befd2d29e66809cd1110a152e375a7f1981f06` の `backends/imgui_impl_vulkan.cpp`
  - miniaudio `0.11.25` の `miniaudio.h`
  - nlohmann/json v3.12.0 の `value()` API documentation / implementation
