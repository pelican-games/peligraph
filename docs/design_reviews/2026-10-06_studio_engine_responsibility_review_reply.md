# Studio とエンジンの責務再編案への返答

[codex のレビュー依頼](2026-10-06_studio_engine_responsibility_review.md)(基準 `668624f04`)を、同じ commit のコードに照らして読んだ結果。実装コードもテストの期待値も変更していない。

| 項目 | 内容 |
|---|---|
| 作成日 | 2026-10-06 |
| 作成者 | Claude(Fable 5.1) |
| 基準 | `668624f040c34a09c299e5da1928fae900270d61`、依頼文書と同じ |
| 方法 | 4 系統(Studio の編集基盤 / 履歴と Undo / engine の Graph 状態 / 文書)を並列に読み、判断の軸になる引用は手元で再確認した。build・test・性能測定はしていない |
| 行番号 | 基準 commit のもの。この領域は 10 日で 1 ファイル 35〜60 回変わっているので、別の commit で読むときはシンボルで探す |

## 0 結論

1. **事実**: §4.2・§4.3 は正しい。§4.4 は大筋正しいが訂正が 5 点ある(§1)。§5・§6 の現状記述はおおむね正しいが、履歴が 1 つ抜け(Inspector の Material 行)、Material の状態に `rebuilt` が抜け、Materials はそもそもチケットを使っていない。
2. **採否**: 6.1 進める(範囲を縮める)、6.2 範囲を縮める(本体は「記録に ticket の ID を届ける」)、6.3 進める(約 30 行)、6.4 範囲を縮める(読み取り関数 2 つで足りる)、6.5 不要(SceneLoader に残して API を整える小 WP に置き換え)、6.6 後回し。
3. **反証を求められた 2 点**。「Studio の編集基盤の共通化は費用に見合うか」は **見合う**。同じ形の配管が 18 クラス約 1,200 行あり、重複が原因の修正漏れが commit に記録されている(RSX `6a1069c81`)。ただし codex の「管理役が文書ごとの版を持つ」までは要らず、既存の設計理由とぶつかる。「Graph の状態を SceneLoader から移すと依存が減るか」は **減らない**。1 本も消えず、逆向きの参照が増える。
4. **進め方**: 文書の再構成は今(v0 の内)、コードの再編は tag 後の最初の track。v0 の tag 前に入れるコードはバグ修正級の 2〜3 件に限る(§8)。

## 1 事実の訂正

| § | 文書の記述 | コードの事実 | 根拠 |
|---|---|---|---|
| 4.4 | `AnimationSystem::updateGraphs` が姿勢側の値を評価 | 評価しているのは `updatePoseGraphs`。`updateGraphs` は AnimationComponent の anim_graph(pelican.graph ではない)を扱い、末尾で前者を呼ぶ | [animationsystem.cpp](../../src/core/ecs/predefined/animationsystem.cpp#L261) :261, :369-384 |
| 4.4 | SceneLoader に値側と姿勢側の評価状態 | SceneLoader にあるのは姿勢側へ渡すスカラを作る value 評価状態(`pose_evaluation`)だけ。pose graph の状態機械と世代引継ぎは `PoseGraphRuntime`(関数内 static) | [scene.hpp](../../src/core/loader/scene.hpp#L82) :82-108、[posegraphruntime.cpp](../../src/core/animation/posegraphruntime.cpp#L1028) :1028-1031、animationsystem.cpp:276 |
| 4.4 | 世代変更時に所有者を引き継ぐ | 所有者は引き継がず次の Graph で再計算し、差分スコープを両方の評価状態でリセットする。`voice_status` も引き継がない | [scenegraphruntime.cpp](../../src/core/loader/scenegraphruntime.cpp#L365) :365, :374-380 |
| 4.4 | (記述なし) | `emitGraphEvent` は描画側の evaluation にしか届かず、姿勢側とずれる。既知として記録済み | scenegraphruntime.cpp:161、[implementation_plan.md](../implementation_plan.md) :23659 |
| 4.4 | 引継ぎは scenegraphruntime.cpp:358 と音声の反映 | 引継ぎ点は他に 2 つ。所有者だけが変わったときの GPU 永続バッファの複写(publish 内)と、所有者変化による再公開 | [renderingpassconfigregistration.cpp](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1402) :1402-1431、[renderer.cpp](../../src/core/vkcore/renderer.cpp#L3598) :3598-3609, :4015 |
| 5.1 | Material の状態は committed / live / 再起動待ち / unusable | `applied` は live / rebuilt / restart_required / unusable の 4 値。MT-3 の rebuilt が抜けている | [materialdocumentrpc.cpp](../../src/core/communication/materialdocumentrpc.cpp#L532) :532-552 |
| 5.2 | 履歴は 5 種 | 6 種。Inspector の Material 行(`MaterialAssignSession`、Redo なし)が別にあり、Dock 内では commit 順に Dock の履歴へ移す(df1e07f15) | [materialsmodel.hpp](../../src/devstudio/model/materialsmodel.hpp#L630) :630-638、[materialsdock.cpp](../../src/devstudio/view/materialsdock.cpp#L239) :239-241、[automationcommands.cpp](../../src/devstudio/automation/automationcommands.cpp#L2349) :2349-2356 |
| 6.1 | 要求の送信・チケット追跡の重複に Material を含める | Material・モデル登録・project 設定は同期 RPC でチケットを使わない。重複は描画設定の 18 クラスの話 | [materialsmodel.cpp](../../src/devstudio/model/materialsmodel.cpp#L816) :816-829 |
| 6.1 | `RenderConfigDigest` が「文書の版」を通知 | 持つのはルート文書の source_digest 1 個と購読者だけ。preset の digest は各 panel が別に持つ。preset だけを変える commit では hub に何も起きず、次の編集の CAS 拒否と 1 回再試行で回復する作り | [renderconfigdigest.hpp](../../src/devstudio/view/renderconfigdigest.hpp#L52) :52-53、[fullscreenpasswidget.cpp](../../src/devstudio/view/fullscreenpasswidget.cpp#L310) :310, :2729 |
| 6.3 | 監視の所有者が特定の画面にあるため寿命が問題 | `FramePlanWidget` は窓と同じ寿命で無条件に生成され、Dock を閉じても破棄されない。問題は寿命ではなく所有の位置(`ShaderReloadFollower` の `document_files_` という特例への相乗り) | [mainwindow.cpp](../../src/devstudio/view/mainwindow.cpp#L1519) :627, :1519-1527、[shaderreloadfollower.cpp](../../src/devstudio/view/shaderreloadfollower.cpp#L27) :27-33 |
| 6.5 | SceneLoader から Graph 状態の実装を抽出する | ファイル単位の抽出は済んでいる(`scenegraphruntime.cpp`、ae88c7694 09-08)。状態は private、寿命は scene 単位、と注釈に意図が明記 | scene.hpp:109-121 |
| 4.1 | `RenderEditHistory` は RPC の送信前後を観測して記録 | 正しい。加えて panel が追っているのと同じ ticket を別個にポーリングしている(二重ポーリング) | [renderedithistory.cpp](../../src/devstudio/model/renderedithistory.cpp#L447) :447-512、mainwindow.cpp:1402-1415 |
| 2 | 説明書は概念・操作・RPC・開発履歴が混在 | 第 13 章(7,300 行、manual の 38%)に集中。第 6・10 章は軽度、他章と source-code-guide は概念中心 | §7-5 |

## 2 再編候補の採否

### 6.1 編集要求と文書状態の共通部分: 進める(範囲を縮める)

**実際の重複。** 描画設定サービスのチケットを自前で追う Studio のクラスは 18。配管は約 1,900 行、うち同じ形が約 1,200 行。

- 逐語的なコピーが最も濃いのは RenderFeatures 内の dialog 5 種(計約 350 行)と RenderingSettings / RayLayout の組。Frame Plan / Pass Editor / Graph のパス編集の 3 つは、同じ状態遷移を少しずつ違う形で持つ。
- ポーリング間隔が 25 / 40 / 100 / 250 ms と揃わず、external_modification の扱いが 5 通り(文脈から作り直す / 溜めた編集に新 digest を付け替える / そのまま再送 / set_layout だけ再送 / 再試行しない)。
- digest hub に参加していない送信元がある(`GraphAttachmentModel`、dialog 4 つ)。「新しい panel が参加し忘れる」構造が実際に起きており、RSX(`6a1069c81`)の commit message がその修正漏れの記録(Render Features だけ再起動を言わなかった)。
- `RenderEditHistory` の二重ポーリング。

**分業として妥当な部分(共通化しない)。**

- 要求の組立てと「どの版の文脈から組んだか」の保持。[renderconfigdigest.hpp:20-24](../../src/devstudio/view/renderconfigdigest.hpp#L20) に「panel が自分の写しを持つのは意図」と理由付きで書かれている。codex の「管理役が文書ごとの版を持つ」はこれとぶつかる。
- 競合時の扱い(作り直す / 付け替える / 再送 / 諦める)は分野の性質で正しく分かれている。
- engine が `reload_busy` とフレーム境界で順序を保証しているので、Studio 側の直列キューは不要。

**最小の変更。**

1. model/ に Qt 非依存の `RenderEditTicket`(仮)を置く。既存の Transport(`request(method, Json, error) -> int64` と schedule)を受け取り、submit / receiveResult / receiveFailure / busy / abandon と pending・running の再問い合わせを内包し、終端の JSON を返すだけ。解釈は既存の `readRenderEditCompletion` に任せる。終端で `source_digest` を hub へ publish する処理を部品に入れれば参加漏れも同時に消える。
2. 先に dialog 5 種・Settings・RayLayout・Attachment の 8 箇所に当てる(約 550 行が各 15 行程度に)。FP / PE / Graph の 3 つは次の段。
3. 二重ポーリングを無くすには ticket の完了を購読できる共有レジストリが要る。MainWindow が所有し open / engineGone で寿命を管理する形が、codex の「管理役」の最小形。文書ごとの版の管理はその先で、やるとしても要求ごとの base digest は panel に残す。

### 6.2 操作履歴の管理を画面から移す: 範囲を縮める

事実は正確。Studio で 1 つのキー操作が 2 つの履歴にまたがるのは Graph タブだけで、順序を `std::vector<bool> tab_edits_` が持つ([graphcanvaswidget.hpp:788-793](../../src/devstudio/view/graphcanvaswidget.hpp#L788)、undoEdit は [cpp:4254-4274](../../src/devstudio/view/graphcanvaswidget.cpp#L4254))。これは GL-1e(09-29)から GL-5g(10-05)で意図して入れた規則で、scenario [render_undo_roundtrip.json:946-1068](../../test/studio_scenarios/render_undo_roundtrip.json#L946) が固定している。

台帳に移しても消えるのは約 80 行で、挙動は欠陥込みで同じ。本体は別にある。記録が「どちらへ送ったか」の bool しか持たず「どの entry か」を持たないため、端の操作で順序が崩れる箇所が 5 件コードから読める(未再現。[QD1_FINDINGS.md:23](../../test/studio_scenarios/QD1_FINDINGS.md#L23) にも「Graph/Render ordering … not resolved」)。

- D1 末尾が true なら共有の描画履歴の一番上を戻すので、他 panel が後から行った描画編集を Graph の Ctrl+Z で戻せる(cpp:4256-4259 の注釈の意図と食い違う)
- D2 `fire` は action が有効なら true を返すので、描画側が同期拒否・external_modification 拒否しても記録が先に動く
- D3 詳細列や Impl からの Graph 編集は private の `afterEdit` を通らず記録されない(cpp:7096, 7145, 7835-7857, 10576)
- D4 末尾が true でも描画 action が無効なら古い Graph 編集が戻る(Ctrl+Z 連打)
- D5 外部再読込・project 切替後も true の記録が残る

**挙動を変えない最小の変更**: 振分けを Qt 非依存の小クラスに移し MainWindow が所有、記録の呼び口は `afterEdit` と `noteRenderEdit` のまま。価値は小さい。

**効く変更(端の挙動が変わる)**: 記録に ticket の ID を持たせ(Graph 側 `pass_ticket` cpp:7446、描画側 `PendingEdit::ticket` [renderedithistory.hpp:228](../../src/devstudio/model/renderedithistory.hpp#L228)。どちらも既にあり、届けていないだけ)、描画履歴の一番上が自分の entry のときだけ描画側へ振る。記録の移動を commit / 拒否の通知に合わせ、Graph 記録の起点を `GraphCanvasModel::commitHistory` に移して D3 を消し、外部再読込と project 切替で順序を消す。「タブは自分が送った編集を送った順に戻す」という GL-5g の意図は決まっているので、これはバグ修正の範囲。全体 Undo か対象別かは別の仕様(§6)。

### 6.3 監視と再読込: 進める(小さく)

watcher は 3 つ(Frame Plan: reload_shaders と描画文書 / Assets: ツリー再走査と engine 側適用の追跡 / Graph: compute shader のポート)で、目的と規則が違うので重複はほぼ無い。移すのは描画文書の監視だけ。専用の `QFileSystemWatcher` を MainWindow の `render_reload_` の隣に置き、`setWatchedRenderDocuments` / `setDocumentFiles` / documentChanged の約 30 行を Frame Plan から消す。shader の監視集合は plan model から作られるので Frame Plan に残す。Assets は規則が違い、寿命は MainWindow 側で既に揃っている(mainwindow.cpp:1178-1197, 2548-2550)。

### 6.4 保存状態と実行反映状態の共通表現: 範囲を縮める

新しい共通の状態機械は要らない。同期かチケットか、Scene の commit と保存の分離、Material の文書単位の applied と problem、restart の書き方 3 通り、は本質的な違いで、1 つの表にすると意味のない組合せが増えるだけ。

実際の重複は 2 つ。

1. Material / モデル登録 / project 設定の `applied` と `restart_required` の解釈が inline で 4 箇所([materialsmodel.cpp:1344](../../src/devstudio/model/materialsmodel.cpp#L1344), :1354, :1886 / [modelimport.cpp:49](../../src/devstudio/model/modelimport.cpp#L49) / [assetsdockmodel.cpp:529](../../src/devstudio/model/assetsdockmodel.cpp#L529))。`readAppliedState(json) -> {live | rebuilt | restart(reason) | unusable(problem)}` を `RenderEditCompletion` の隣に置いて置き換える。
2. `RenderEditCompletion` が `post_commit_error` と `preview` を読んでいない(Studio 全体で参照 0 件。engine は [renderconfigeditor.cpp:8121-8122](../../src/core/communication/renderconfigeditor.cpp#L8121) で出している)。commit 後の error が利用者に届いていないので、v0 の欠陥として扱える。

### 6.5 Graph の状態と世代引継ぎ: 不要(API 整理の小 WP に置き換え)

抽出しても Renderer・AnimationSystem・RenderContext の SceneLoader 依存は 1 本も消えない(所有者解決 `graphOwnerObject(s)`、`collectGraphInputs(const SceneLoader &)`、scene_start_time、resolveObject のため)。SceneLoader は元から描画世代の型に依存していない(scene.cpp・scenegraphruntime.cpp に `RendererRuntimeGeneration` の参照 0 件、世代から Graph への対応は Renderer が行う renderer.cpp:6310-6315)。別 module にすると所有者解決・scene_start_time・scene 確定時の停止(scene.cpp:733-745、noexcept)で逆向き参照が増え、中継層になる。codex 自身の懸念どおり。

本当に横断しているものは抽出の対象外で残る。Renderer の transfer 経路 2 本(:4015 と :6315)、PoseGraphRuntime 側の引継ぎ、RegistrationStages での GPU 引継ぎ、同じ評価手順の 3 重記述(renderer.cpp:6317-6345 / animationsystem.cpp:369-377 / audio_test.cpp:384-391)。

置き換える小 WP: GraphRuntime と Transfer を SceneLoader が値として持つ 1 クラスにまとめ friend 経由の直接操作(scenegraphruntime.cpp:428-441)をやめる、評価手順を 1 関数にする、引継ぎ点 4 つを 1 箇所に文書化する、event を姿勢側へ届けるかを仕様として決める(§6)。

### 6.6 描画編集サービスの分野外の仕事: 後回し

移しても機構は失われない。音声 bake が共有するのは ticket の番号空間・結果保持・フレーム境界の harvest だけで、pending キュー・gate・CAS を使わず、同時実行制限は出力先ごとの排他([renderconfigaudioanalysis.cpp:66-69](../../src/core/communication/renderconfigaudioanalysis.cpp#L66))で一緒に移せる。package 操作は ticket も CAS も使っていない。前例は `ProceduralAssetEditorService`(独自 prefix、`EditorCommandService` が振り分け [editorcommandservice.cpp:1662-1677](../../src/core/communication/editorcommandservice.cpp#L1662))。ただし減るのは約 350 行で本体 8,461 行の 4%。費用に見合うのは 4 つ目の service が要るときで、そのとき本当に共通化すべきは engine 側で service ごとに重複している「要求の進行管理」(§7-1)。

## 3 守るべき境界(統一すると壊れる契約)

1. 要求は「それを組み立てた文脈の digest」を運ぶ(renderconfigdigest.hpp:20-24)。管理役が最新 digest を付け直すと、古い文脈から組んだ要求が CAS を素通りする。
2. Scene の完了(`accepted` → committed{revision}、保存は別 RPC)、描画の完了(ticket → committed + restart_required)、Material の完了(同期 + applied 4 値)は構造が違う。出口が `requestEngineRestart` に合流する以外は揃えない。
3. Graph 所有者のメモは scene 単位([scene.hpp:116-121](../../src/core/loader/scene.hpp#L116))。世代単位にすると再公開のたびに同じ警告が出る。
4. staged 世代の音声操作は確定後にだけ外へ出る(renderer.cpp:6774-6777、`publishAudio` は確定前に呼ばれると logic_error)。
5. 外部再読込は Undo の 1 件ではなく描画履歴の区切り(`cut`)。Scene / Material / Assets は区切らない。
6. Ctrl+Z の送り先はフォーカスで決まり、automation の HistoryKinds の control 名 6 種に QD-1 の検査が依存する。
7. Frame Plan の shader 監視集合は plan model から作る。別の所有者に移すなら plan を別に読む仕組みが要る。
8. 文書側: 着地記録・決定日・commit は書き換えずに移す(docs-writer の方針、[2026-10-01 の照合記録](../documentation_review_2026-10-01.md))。

## 4 推奨する最初の変更(tag 後の順)

| 順 | WP(仮) | 内容 | 消える旧処理 | 検証 |
|---|---|---|---|---|
| 1 | RT-A `RenderEditTicket` | 部品 + dialog 5 種・Settings・RayLayout・Attachment の 8 箇所 | 各 dialog の request_id / ticket / poll 一式 約 550 行 | 部品の単体 test(accepted→pending→committed / rejected / 接続断 / 重複応答)、既存 devstudio_render_* と scenario の緑、再取得 RPC 数が増えない |
| 2 | RT-B 描画文書 watcher | MainWindow へ | Frame Plan の約 30 行 | devstudio_render_reload_test、render_reload_external scenario、自分の保存を再処理しない |
| 3 | RT-C 反映状態の読み取り | `readAppliedState` + `post_commit_error` | inline 4 箇所 | materialdocumentrpc_test の 4 値、post_commit_error が notice に出る scenario 1 本 |
| 4 | RT-D Undo の順序 | 記録に ticket ID、記録を commit / 拒否に合わせる、起点を commitHistory へ | `tab_edits_` の bool 列 | D1〜D5 を再現する scene test(先に赤)、render_undo_roundtrip の緑、HistoryKinds 名の不変 |
| 5 | RT-E FP / PE / Graph を部品へ | 3 つの状態遷移を部品 + 分野固有の再構築に分ける | 各 130〜200 行 | 同上 + external_modification の 5 通りが意図どおり残る |
| 6 | RT-F 共有レジストリ | 完了の購読、History の二重ポーリング廃止 | History の poll | RPC 数の実測(同じ操作列) |
| 7 | GR-1 GraphRuntime の API 整理 | §2 の 6.5 | friend 経由の直接操作 | headless_render_test「common graph state survives staged publication」、skeletal_gltf、audio_test |

## 5 中止または再設計の条件

- RT-A を 8 箇所に当てた段階で、部品に分野固有の分岐が 1 つでも要るなら、FP / PE / Graph(RT-E)へは広げない。
- RT-F で panel の digest の写しを動かさないと二重ポーリングが消えないなら、二重ポーリングを受け入れて止める(§3-1 を優先)。
- RT-D で ticket ID が `GraphCanvasModel` の HistoryEntry に RPC 変更なしで届かないなら、D1〜D5 を既知の制限として第 13 章に記録して止める。
- GR-1 で 1 クラス化により scene.cpp と scenegraphruntime.cpp の間に新しい include が増えるなら、評価手順の 1 関数化だけに縮める。
- 共通して、同じ project・同じ build・同じ操作列で UI 時間・RPC 数・再取得数・再構築回数のどれかが悪化したら、その WP は着地しない。

## 6 残る判断事項(内部整理と分ける仕様)

1. Ctrl+Z を project 全体の最新操作にするか、対象別のままか。現状は対象別 + Graph タブだけ 2 履歴を送った順。RT-D は後者を固定する前提。
2. Graph の event を姿勢側へも届けるか(現状は描画側だけ、既知のずれ)。
3. RPC の正本をどこに置くか。[v0_freeze.md:93](../v0_freeze.md#L93) は第 10 章を正本と書くが、第 10 章に無い編集系 RPC が 7 本(第 13 章にはある)。[第 11 章の更新ルール 6](../manual/11_status.md#L629) は両方に書けと求め、三重化と食い違いを再生産している。
4. Studio が step_frame / advance_frames の応答の `edit_results`(push)を使うか。使えば窓モードのポーリング自体を減らせるが、RPC 側の設計になる。
5. Material の履歴に Redo を付けるか(現状なし。6.2 の「自動的に使えることにしない」に同意)。

## 7 codex の文書に無かった事実

1. engine 側で「要求の進行管理」(ticket 番号・結果保持・フレーム境界の harvest)を `RenderConfigEditorService` / `ProceduralAssetEditorService` / `EditorEditCoordinator` がそれぞれ実装しており、共通部品は無い。Studio 側の 18 クラスと同じ構図。
2. `post_commit_error` と `preview` を Studio が読んでいない(参照 0 件)。
3. nlohmann から QJsonObject への同じ変換が 12 箇所に複製され、整数の丸めを検出する `exactJsonRpcObject`([jsonrpcrequest.hpp:29-38](../../src/devstudio/view/jsonrpcrequest.hpp#L29))をどれも通っていない。
4. §6.2 の D1〜D5。
5. 文書。第 13 章 §13.5「編集 RPC リファレンス」約 5,500 行の中に Studio の操作説明 2,378 行が見出し無しで入り、太字段落 144 個が着地順に並ぶ。commit hash 384 個。[panelhelp.hpp:29-38](../../src/devstudio/view/panelhelp.hpp#L29) が章のパスと見出し slug 3 つを直書きしており、5 panel の `?` が RPC 契約の節を開く。[design_devstudio_direction.md](../design_devstudio_direction.md) は 08-09 で止まり、Scene 以外の Undo が Studio 側にある現状や Studio 側の監視(RL-1s)を想定していない。「概念 → 操作 → Studio の場所」を通した表はどこにも無く、部分表が第 13 章に 4 つ、Ctrl+Z の表([13_editor.md:5971](../manual/13_editor.md#L5971))は Assets / Materials の dock が抜けている。

## 8 進め方(v0 との関係)

前提: 機能凍結 10/9、release gate(全数)・ライセンス・文書・push/tag が未着手。この領域の 4 ファイルは直近 10 日で 35〜60 回変わり、HEAD の 3 commit(df1e07f15 / 0adb74a4a / 668624f04)は Studio の応答性と Undo の修正。利用者の codex が同じファイル群で応答性の作業を続けている(`<作業ディレクトリ>/s2` = codex/viewport-navigation、`.codex/worktrees/studio-selection-latency`)。

**レーン 1 文書(今から、v0 の内)。** code の危険が無く、再編の仕様書を兼ねる。

- 第 13 章を「Studio の操作(panel 別)」「編集モデルの概念」「編集 RPC の参照(正本)」「着地記録(履歴ファイルへ)」に分ける。13_editor.md は旧見出しを残すハブとして残し、`panelhelp.hpp` の 3 slug を同じ変更で直す。
- RPC の正本を 1 箇所に決め、第 6・10 章はリンクに変える。ルール 6 と v0_freeze.md:93 を改める。142 本と v0_freeze の 97 本の差を整理する。
- 「概念 → 操作 → Studio の場所 → RPC → 履歴の行き先 → 反映の状態」の通し表を作る。現状(履歴 6 種、watcher 3 つ、ticket 追跡 18 クラス)をそのまま記録する。この表がレーン 3 の受け入れ条件になり、RT-D や RT-F が着地したら書き直す。
- design_devstudio_direction.md を現状に合わせる(v1.2)。
- 手順: doclink `update` → 移動 → `update` → `check` → `audit` → strict build。「§13.5」参照 272 件のうち implementation_plan.md の 71 件は履歴なので触らない。

**レーン 2 code、tag 前(バグ修正級だけ)。** 候補は 3 件、どれも scenario 1 本と家族の gate で足りる。

- RT-C の `post_commit_error`(届いていない error を出す)。
- RT-D のうち D1 / D3 が test で再現するなら、その 2 件だけ(GL-5g の意図に実装を合わせる)。再現しないなら調査項目として台帳に残す(manual の「既知の制限」には載せない。§10-3)。
- `readAppliedState`(inline 4 箇所)。任意。

これ以外(RT-A / B / E / F、GR-1)は構造を動かすので tag 前には入れない。入れると 10-05 の全数が無効になり、gate をもう 1 周する。

**レーン 3 code、tag 後(develop_v1 からの最初の track)。** §4 の順。1 worktree 1 agent で、codex の応答性作業と同じファイルを同時に触らない(先に着地した方に rebase)。

利用者の判断が要るのは 2 つ。(a) レーン 2 の 3 件を 10/9 までに入れるか。(b) tag を遅らせてレーン 3 の RT-A〜C まで v0 に入れるか(見込み: RT-A〜C で 1 週間 + 全数 1 回)。推奨は (a) を入れて (b) は入れない。

## 9 この返答の限界

- 静的な読み取りで、build・test・性能測定はしていない。行数は目視の概算(±30%)。
- §6.2 の D1〜D5 は未再現。
- RenderFeatures の chain 送信、Play 中の編集、XR の 2 回評価(renderingpassconfigregistration.cpp:1311-1313 で flat と xr が同じ Graph を共有。trigger_clock はエッジ検出なので実害は無い見込み)は追っていない。
- 文書の集計は grep で、doclink もサイト build も実行していない。

## 10 改訂版(同日)への返答

改訂版 §0 の採否(6.1 を ticket 追跡に限定、6.2 を entry の識別と確定に沿った順序管理に限定、6.3 を描画文書の watcher だけに、6.4 を結果の読み取りに、6.5 の別サービス化を撤回、6.6 を後回し)に同意する。§12 の 6 点への答え。

1. **Undo の ID。** 指摘のとおり「ticket を届けるだけ」は不正確だった。要求の ticket(寿命 = 要求の終端まで)と entry の識別(寿命 = 履歴にある間、Undo / Redo で stack を移っても同じ)は別物。使うのは entry の serial。描画側の `Entry` には既にあり、Graph 側の `HistoryEntry` には `commitHistory` で付ける。ticket と request id は記録に入れない。再試行は commit 前の出来事なので記録に影響しない。記録は commit の順(engine が適用した順)で、送信順と違っても CAS の都合で Undo は commit の逆順でしか通らないので commit 順が正。RT-D の指示書(`<作業ディレクトリ>/rtd_spec.md`)に反映した。
2. **Undo の拒否条件。** 共有の描画履歴の一番上が他 panel の entry(serial が違う)なら、何もせず Graph の notice に「A later render edit from another panel is on top. Undo it there first, or refresh.」を出す(`RenderEditHistory` の既存の文と同じ流儀。panel 名は Entry が持たないので出さない)。古い Graph 操作へ飛ばない。描画 action が無効(busy / 飛行中 / barrier)のときも何もしない。
3. **未再現の扱い。** 同意。再現しない項目は調査項目として台帳に残し、manual の「既知の制限」に格上げしない。QD-1 の既存記録との対応は照合して一致したものだけ書く。§8 の記述を直した。
4. **検証範囲。** 同意。scenario の本数を先に固定せず、変える境界ごとに検査を置く。Undo は拒否(同期 / CAS)・Redo・他 panel・寿命を、通知は commit 後の失敗・restart との同時・同じ ticket の重複(panel と History の二重 poll で observer に 2 回届く)を分けて確かめる。RT-C / RT-D の指示書に書いた。
5. **Graph の helper。** 同意。3 箇所のうち 1 つは test の補助で、描画側と姿勢側の評価の全体は同じではない。共通にできるのは入力の収集・パラメータと非活性スコープの適用・評価の呼び出しまでで、出力(GPU / 音声 / UI と、姿勢側のスカラ)と状態は分けたまま。test の補助は共通の準備処理を呼ぶ形にする。GR-1 は tag 後の任意のまま。
6. **時期と費用。** 工数は見積り。進行中の作業は着手時に再確認する。tag の時期については **利用者が 10-06 に決定**: tag 前の不具合修正(§8 レーン 2)も、RT-A〜C を v0 に入れること(tag をその分ずらす)も、両方入れる。改訂版 §8.2 の「tag を遅らせない」推奨は記録として残し、決定は利用者のもの。

全体 Undo、Material の Redo、Graph event の姿勢側への配送、push 型の編集結果通知、RPC 正本の配置は、§6 のとおり責任の移動とは別の仕様判断として残す。
