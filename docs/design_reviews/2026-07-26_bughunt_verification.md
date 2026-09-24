# バグハント指摘の敵対的検証

| ID | 判定 | 実害 | 修正規模 | 根拠(自分で確認した file:line) |
|---|---|---|---|---|
| A-F2 | CONFIRMED | ウィンドウ最小化との競合時に swapchain 再作成が失敗し、実行が終了しうる。Win32 で明記された TOCTOU | 小 | `src/core/os/window.cpp:256-268` の待機後、`src/core/vkcore/swapchainframetarget.cpp:46,78-83,167-173` で capabilities を取り直す。固定 `currentExtent` は `:24-33` で無検査のまま返す。公式 [Win32 WSI 仕様](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html) は最小化時 `(0,0)` を明記 |
| A-F3 | PARTIAL | 複数 RT の新旧混在状態は作れるが、例外は最上位まで伝播して通常は同じ実行を継続しない | 大 | `src/core/renderingpass/rendertargetcontainer.cpp:601-710` は RT ごとに即 commit、対して登録は `:587-596` で rollback。後処理は `src/core/vkcore/renderer.cpp:1785-1792`、最上位 catch は `src/core/userpublic/pelican_core.cpp:44-100` |
| A-F4 | PARTIAL | 小さいウィンドウと縮小 RT で確実に fatal error になりうる。ただし壊れた fence/semaphore を次フレームで再利用する前に実行が終了する | 小〜中 | `src/core/renderingpass/rendertargetcontainer.cpp:64-74`。target 開始は resize より先 (`src/core/vkcore/renderer.cpp:2903-2914`)、fence reset/record begin は `src/core/vkcore/swapchainframetarget.cpp:291-295`。最上位 catch は上記 |
| A-F5 | CONFIRMED | `VK_ERROR_SURFACE_LOST_KHR` は回復不能で実行終了。稀だがユーザ可視 | 大 | surface は `src/core/os/window.cpp:310-318` で一度だけ作成し、再作成も同じ surface を使用 (`src/core/vkcore/swapchainframetarget.cpp:171-173`)。処理分岐はリポジトリ内 0 件。SDK `vulkan.hpp:8650-8655,8842` は `SurfaceLostKHRError` を投げる |
| A-F6 | PARTIAL | format 変更時に RT/pipeline を再構築しない点は正しいが、現状は copy/format guard が明示的に止めるため「不正 pipeline で描画」より fatal error が主症状 | 大 | 変更検出 `src/core/vkcore/swapchainframetarget.cpp:198-208`、保存済み format の再利用 `src/core/renderingpass/rendertargetcontainer.cpp:638-647`、初期コンパイル `src/core/renderingpass/renderingpassruntimecompiler.cpp:862-889`、copy guard `src/core/vkcore/swapchainframetarget.cpp:355-365` |
| A-F7 | CONFIRMED | 恒常 `SUBOPTIMAL` なら毎フレーム `waitIdle` と再構築。絵は出ても大きく stutter | 小 | acquire は `eSuboptimalKHR` を許容 (`src/core/vkcore/swapchainframetarget.cpp:286-289`)、present は即再作成 (`:443-450`)、再作成は `device.waitIdle()` (`:198-203`) |
| A-F9 | PARTIAL | 破棄先行・部分 commit は事実。ただし OOM 後は最上位 catch で停止するため、null depth のまま通常描画を継続するわけではない | 中 | `src/core/vkcore/swapchainframetarget.cpp:159-183,198-204`、最上位 catch `src/core/userpublic/pelican_core.cpp:44-100` |
| A-F10 | PARTIAL | semaphore 再利用の危険は実在するが、原報告の「OUT_OF_DATE なら present wait が実行されない」は誤り。問題は fence が presentation 完了を保証せず、render-finished semaphore を frame index で再利用していること | 中 | semaphore は frame 単位 (`src/core/vkcore/swapchainframetarget.cpp:213-214,421-457`)。公式 [vkQueuePresentKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html) は reject 時も wait 操作を実行すると規定。一方、公式 [swapchain semaphore reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html) は present wait semaphore を swapchain image に紐づけるよう要求 |
| A-F11 | CONFIRMED | 最小化中、メインループ全体が停止し RPC・reload・ECS 更新も止まる | 小〜中 | `src/core/os/window.cpp:256-268` の `glfwWaitEvents()` は再作成 (`src/core/vkcore/swapchainframetarget.cpp:167-173`) から同期呼出し。更新/RPC/render は同一ループ (`src/core/appflow/loop.cpp:461-470,548-555`) |
| A-F12 | CONFIRMED | XR mirror resize/recovery が `device.waitIdle()` を呼び、次の HMD frame を遅延させる。現在の HMD frame は既に `xrEndFrame` 済みなので「その場で停止」ではなく次フレームの stutter | 中 | 非待機を宣言するコメント `src/core/vkcore/swapchainframetarget.cpp:443-447` に反して、mirror は `src/core/openxr/openxrmirrorsink.cpp:184-190,285-292` から recovery、実体は `swapchainframetarget.cpp:481-490,198-203` |
| A-F13 | PARTIAL | 通常の OUT_OF_DATE 二重失敗では lease は先に `completeAll()` される。「永久ラッチ」は disabled 中は無害。実際の保持穴は submit 後の予期しない present 例外で mirror を disable した場合 | 小〜中 | stale clear `src/core/vkcore/swapchainframetarget.cpp:198-204`、disabled return `src/core/openxr/openxrmirrorsink.cpp:167-172`、`frame_begun=false` 後の `render_end` `:285-303`、lease retain は `swapchainframetarget.cpp:421-439` |
| B-1 | CONFIRMED | teardown の一部失敗後も DLL を unload でき、旧コードを指す生存 object/callback を残しうる。高実害の隠れた穴 | 中〜大 | unload 前 teardown `src/core/gamelogic/gamelogicreload.cpp:246-290`、例外 rollback `:318-323`。本番 `runtimeTeardown` は `src/core/appflow/teardown.hpp:86` / `teardown.cpp:17-30,109-120` で各失敗を記録して握り潰す |
| B-2 | REFUTED | SymbolId は型未確定の遅延制約として全 parameter kind に通す設計。dead `case` は重複だが型検査 bypass は欠陥ではない | 任意整理 | 早期 return `src/project/logicalrendertype.cpp:150-163`、遅延照合 `:835-860,873-987`、期待動作のテスト `test/logicalrendergraph_test.cpp:230-240` |
| B-3 | CONFIRMED | 空 method が schema error でなく後段の method unavailable/not found へ進む。診断品質の穴 | 小(1 行) | `src/core/loader/editorpreviewprojection.cpp:83-91`。nlohmann `json.hpp:2933-2965` の `empty()` は string 長を見ず、string は非 empty 扱い |
| B-4 | CONFIRMED | guard は完全に死んでいるが、現行の import/producer 制約下では無害 | 小(削除) | import は version 0 (`src/project/logicalrendergraph.cpp:203-209`)、producer は version 0 を拒否 (`:215-223`)、交差は完全 ID 比較 (`:234-238`) |
| B-5 | REFUTED | prefer/automatic の低い共通 sample count への fallback は理由付きの仕様。exact mode は正しく失敗する | 任意整理 | 1x 能力を先に要求 `src/project/samplecountplanning.cpp:267-286`、fallback 理由 `:320-335`、exact の拒否 `:303-318`、期待テスト `test/samplecountplanning_test.cpp:77-79` |
| B-6 | CONFIRMED | `UINT64_MAX` 等が int64 範囲検査をすり抜ける。対象 ABI では負値化しうる | 小 | `src/project/materialformat.cpp:365-373`、`src/project/surfaceformat.cpp:319-327`。nlohmann `detail/conversions/from_json.hpp:68-100` は unsigned から単純 `static_cast` |
| B-7 | CONFIRMED | 意図した swapchain 禁止診断より先に generic `out_of_range` が出る | 小(並べ替え) | metadata lookup `src/core/renderingpass/renderingpassvalidation.cpp:209`、special ID guard `:212-216`、resolver `src/core/renderingpass/rendertargetmetadataresolver.cpp:15-20` |
| B-8 | PARTIAL | register 側の advertised status は内側の `noexcept` 関数自身が catch して返すため到達可能。unregister 側だけは内側 `noexcept` の lock/push_back 例外で terminate し、外側 catch は無効 | 中(4 系統) | graph transform `src/core/renderingpass/graphtransformregistry.cpp:1308-1385,1389-1422,1613-1663`。同型は subgraph `:1327-1441,1735-1785`、pass `:678-779,939-984`、strategy `:856-966,1175-1224` |
| B-9 | CONFIRMED | version bit と tile attribute が矛盾する malformed EXR を scanline として解釈しうる。通常資産には無害だが、入力堅牢性の穴 | 中 | 二段目 guard `src/core/loader/imageloader.cpp:173-194`。tinyexr `tinyexr.h:5957-5987,6236-6237,10732` は tiled bit が偽なら tile attribute を tiled として反映しない |
| B-T2-PIPE | CONFIRMED | result 比較は conformant driver では死亡。実害はなく、他の失敗は vulkan.hpp が投げる | 小(削除) | `src/core/shader/pipelinefactory.cpp:427-450`、desc に flags なし (`pipelinefactory.hpp:44-77`)。実使用 SDK 1.4.309 `vulkan_funcs.hpp:3444-3470,3672-3685` の許容 result は success/compile-required |
| B-T2-FENCE | CONFIRMED | V1 と同型の dead error branch。device lost の独自診断だけ失われる | 小 | `src/core/vkcore/offscreenframetarget.cpp:145-150,224-229`、`src/core/openxr/openxrcompositiontarget.cpp:230-234,439-455`。§1 V1 の既検証結果と同じ vulkan.hpp enhanced overload 契約 |
| B-T2-RT | CONFIRMED | 「`rendertiming.cpp:315` は欠陥でない」というクリーン判定が正しい | なし | `src/core/vkcore/rendertiming.cpp:303-317` は raw pointer overload。SDK 1.4.309 `vulkan_funcs.hpp:2293-2313` は `noexcept` で生の `Result` を返す |
| B-ORDER | CONFIRMED | 本来の loader error が registration 解放中の Win32/POSIX error で上書きされ、誤診断になりうる | 小(1 行移動) | `src/core/gamelogic/gamelogicreload.cpp:132-139` は `releaseGameLogicRegistrations` 後に `platformLoadError()`、error 読取は `:71-79` |
| B-T3-SCENE | CONFIRMED | warnings lane は死んでおり、コピーコストと API 複雑性だけが残る | 小〜中 | `src/project/sceneformat.hpp:11-14`、`src/project/sceneformat.cpp:201-216` は warnings を書かず、consumer は `src/core/loader/scene.cpp:264-269` |
| B-T3-SAVE | CONFIRMED | 保存ごとの JSON 再 parse は、正常な nlohmann dump/parse 契約では semantic guard として死亡。serializer 回帰検知用の意図的 assertion ではある | 小 | `src/core/loader/basicconfig.cpp:703-739`。`encodeSemantic()` は単純 `raw_document_.dump()` (`src/core/loader/authoringscenedocument.cpp:102-106`) |
| B-T3-SHADER | CONFIRMED | built-in shader participant の per-request `.enqueue` は全呼出しで batch 経路に consume される。現状は無害な重複 API | 小 | 登録 `src/core/watch/reloadservice.cpp:267-276`、単件も `applyRequests` へ委譲 (`:464-466`)、shader consume `:569-599`、一般 enqueue は `:624-626` |
| B-T3-CANON | CONFIRMED | 4 個の「非正準」再検査は parser が正準性を保証するため死亡。無害 | 小(4 箇所削除) | `src/project/targetrenderplanning.cpp:97-109`、`src/project/vulkanphysicalfragment.cpp:58-70`、`src/project/targetplanning.cpp:26-37`、`src/project/logicalrendertype.cpp:267-280,322-348` |
| C-1 | CONFIRMED | update-phase hot reload で旧 GPU 資源を fence 待機より先に解放し、GPU use-after-free/device lost を起こしうる。最重要 | 中 | defer は現在 epoch (`src/core/vkcore/deletionqueue.hpp:54-60`)、2 epoch 後を `<=` 解放 (`deletionqueue.cpp:74-82`)。Renderer は target fence 待機より先に begin (`src/core/vkcore/renderer.cpp:2478-2492,2903`)。reload はその前 (`src/core/appflow/framephase.cpp:119-124`)、texture 退役 `src/core/material/texturereloadhandler.cpp:213-230` |
| C-2 | CONFIRMED | テストの stack stream、将来の短命 stream で detached reader が UAF。production の `std::cin` では低頻度 | 中 | stream を参照保持 (`src/core/communication/windowedrpchost.cpp:15-28`)、未完了 reader を detach (`:88-103`)。stack stream のテスト `test/windowedrpchost_test.cpp:57-61,93-100` |
| C-3 | REFUTED | 現在の構築順/LIFO 破棄では ReloadService が watcher を join してから ReloadGate を破棄する。構築順変更に弱い設計上の注意に留まる | 任意強化 | 構築順 `src/core/userpublic/pelican_core.cpp:64,83`、LIFO `src/core/container.hpp:258-272`、service stop `src/core/watch/reloadservice.cpp:36-38`、watcher join `src/core/watch/filewatcher.cpp:656-675` |
| D-1 | PARTIAL | 型不一致で throw する点は正しい。通常 queue 受理時はメイン側最上位 catch で実行終了、`std::terminate` は queue overflow 時に reader thread の `busyResponse` が直接 parse した場合 | 小 | `src/project/jsonrpc.cpp:148-175`、nlohmann `json.hpp:2247-2268`。parse が handler try より前 (`src/core/communication/rpcserver.cpp:719-737`)。worker 直呼びは `src/core/communication/windowedrpchost.cpp:39-68` |
| D-2 | CONFIRMED | normalized integer TEXCOORD/COLOR/WEIGHTS が 0..1 でなく整数値のまま float 化され、表示・skinning が破損 | 中 | `src/core/model/gltf.cpp:594-771,1614-1632` は単純 cast。tinygltf `tiny_gltf.h:809-817` に `normalized`。兄弟経路は `gltf.cpp:773-784` / `src/core/loader/vrmadecoder.cpp:319-329` で拒否 |
| D-3 | CONFIRMED | 16-bit PNG の byte 列を RGBA8 image にコピーし、画素を誤解釈する。原報告の「上半分」ではなく、先頭から RGBA8 必要量を読む | 中 | 16-bit decode `src/core/model/gltfimage.cpp:22-41,99-106`、固定 RGBA8 upload `src/core/model/gltf.cpp:1366-1375`、copy は byte 数と format 必要量を照合しない `src/core/vkcore/util.cpp:82-114` |
| D-4 | CONFIRMED | ImGui backend 内の Vulkan 失敗 46 箇所が no-op。descriptor allocation 失敗時は未初期化 handle を update に渡しうる | 小 | init hook 未設定 `src/core/imgui/imguisystem.cpp:313-329`。vendored ImGui 1.91.9b `imgui_impl_vulkan.cpp:396-404,1221-1253` は callback がなければ無処理、`descriptor_set` は未初期化 |
| D-5 | PARTIAL | 物理音声 device 不在時に miniaudio の null backend へ成功降格して「miniaudio」とログする点は正しい。ただし app fallback 自体は OOM 等、null backend も初期化不能な場合には到達可能 | 小 | app fallback `src/core/audio/audio.cpp:323-329,397-415`。miniaudio `miniaudio.h:43332-43389,44043-44101` は既定 backend 列の最後に null を試す |
| D-6 | CONFIRMED | この新規・非空 audio buffer 経路では `ma_sound_start` 失敗分岐は実質死亡。デバイスが実際に鳴るかをこの戻り値では検出できない | 小(整理) | call `src/core/audio/audio.cpp:338-369`、空 data 拒否 `:184-233`。miniaudio `miniaudio.h:78746-78772` は null または既に at-end の seek 失敗以外 success |
| D-7 | CONFIRMED | 外部画像欠落の本来の filename warning が `inspect()` 失敗時に出ず、generic empty texture だけになる | 小〜中 | tinygltf `tiny_gltf.h:4385-4415` は warn + true。app は bool のみ確認 (`src/core/model/gltf.cpp:211-223`)、empty guard `:292-296`。scene は commit 前に inspect (`src/core/loader/scene.cpp:625-633`)、warning 出力は commit (`gltf.cpp:1958-1968`) |
| D-8 | CONFIRMED | VRMA の malformed animation channel が `err` を残して捨てられるが、ロード true のため app が無視し、track 欠落として現れる | 小 | tinygltf `tiny_gltf.h:5488-5513,5544-5563`。VRMA decoder は false 時しか strings を扱わない (`src/core/loader/vrmadecoder.cpp:571-577`) |
| D-9 | CONFIRMED | `asset.version` 欠落でも tinygltf は true。main glTF は error をログするだけで継続、VRMA は無視 | 小 | tinygltf `tiny_gltf.h:4280-4290`。main load は bool のみ (`src/core/model/gltf.cpp:214-223`) で commit 時ログ (`:1966-1968`)、VRMA は上記 `:571-577` |
| D-10 | CONFIRMED | base `source` がない extension texture で `images.at(-1)` 相当となり generic load failure。メモリ破壊ではなく未対応 extension の粗い fatal 診断 | 小〜中 | `src/core/model/gltf.cpp:1366-1374`。tinygltf `tiny_gltf.h:655-668,4439-4458` で `source` は既定 -1/optional |
| D-11 | CONFIRMED | 書込み不能 cwd 等で logger 構築例外が `main` の保護外へ出て、起動時 terminate | 小 | `src/core/log.cpp:16-28`、`src/core/userpublic/pelican_core.cpp:32-41`、`src/player/main.cpp:434-444`。quill `FileSink.h:359-381` は `fopen`/`setvbuf` 失敗で throw。path は `src/core/config.hpp:9` |
| D-12 | CONFIRMED | 再生済み/stop 済み voice と decoded buffer が終了まで蓄積する。長時間・高頻度 SE で無制限メモリ増加 | 中 | backend map `src/core/audio/audio.cpp:294-320,331-395`、public map `:435-450,465-467`。終了時 `clear` 以外の `erase` は 0 件 |
| D-13 | CONFIRMED | B-ORDER と同一。loader error の取得順が逆 | 小(1 行移動) | `src/core/gamelogic/gamelogicreload.cpp:132-139,71-79` |
| D-14 | PARTIAL | zero quaternion は vendored GLM が identity にするため原報告ほど広くない。巨大 JSON number の float overflow は未検査で NaN を通しうる | 小 | `src/core/model/gltf.cpp:874-940`。GLM `glm/ext/quaternion_geometric.inl:11-23` は長さ 0 以下だけ identity、NaN/Inf は除外しない。堅牢な兄弟 `src/core/animation/vrmaretarget.cpp:37-46` |
| D-15 | PARTIAL | GLFW error を捨てて診断を潰す点は正しいが、当該 API の header が列挙する error は原報告の 4 種でなく 2 種。ImGui の clear は cursor 作成時の意図的消去 | 小 | app `src/core/os/window.cpp:321-331`。GLFW 3.4 `glfw3.h:2428-2457,6320-6360`、ImGui `imgui_impl_glfw.cpp:620-641` |
| D-16 | REFUTED | API は非所有 pointer を返すが、現行 2 caller は GLFW 生存中に即 copy/instance 作成し、terminate 後まで保持しない | 任意強化 | pointer 返却 `src/core/os/window.cpp:321-331`、消費 `src/core/vkcore/core.cpp:32-72,75-91`、GLFW lifetime 契約 `glfw3.h:6347-6349` |
| E-1 | CONFIRMED | BC5/未知 format の正確な診断が先行する複合 guard に隠れ、「RGBA8/BC7 size」不一致と誤表示。描画破壊ではない | 小 | format 別診断 `src/core/renderer/atlasassetresource.cpp:67-80`、先行複合 guard `:127-149` |
| E-2 | CONFIRMED | OOM/lock 例外時、graceful provider_error でなく `noexcept` terminate。通常時は無害 | 小〜中 | `src/core/renderer/renderpolicyregistry.cpp:387-408` の `unique_lock`/`push_back` は内側 `noexcept`。外側 catch は `:532-542` |
| E-3 | CONFIRMED | `empty()` guards は死んでいるが、失敗はその前に vulkan.hpp が throw するため無害 | 小(削除) | `src/core/renderer/debugdraw.cpp:78-87`、`debugtext.cpp:185-194`。実使用 SDK 1.4.309 `vulkan_funcs.hpp:4321-4347` は count 個確保し、失敗時 `resultCheck` |
| E-4 | CONFIRMED | 同じ concrete RT guard の重複。無害 | 小(削除) | 先行 guard `src/core/fullscreenpass/fullscreenpasscontainer.cpp:388-395`、重複 `:457-460`。間で `input_rts` は変更されない |
| E-5 | CONFIRMED | 32 view 上限後の `size_t` overflow guard は現行 target で死亡。無害 | 小(削除) | `src/core/renderer/frameresources.cpp:31-48,53-70` |
| E-6 | CONFIRMED | enum fallback throws は全 item の事前 validation 後で死亡。無害な防御重複 | 小(整理) | validation `src/core/renderer/drawqueuebuilder.cpp:22-45,79-115,418-426`、fallback `:117-150` |
| E-7 | CONFIRMED | `mergeShaderRuntimeResult` の空 result return は唯一の caller が `attempted=true` を設定するため死亡。無害 | 小(削除) | `src/core/watch/reloadservice.cpp:168-177,180-219`。呼出しは `:219` のみ |

## 検証条件

対象はディスク上の commit `34a135626fea96e2c40fee18f8477d96c4d5e29c` である。54 主張の判定は CONFIRMED 39、PARTIAL 11、REFUTED 4 となった。§1 の V1/V2 は再検証対象から除外したが、§3 が明示的に再掲した fence 群は既検証 V1 の契約と突き合わせた。

依存契約はリポジトリ本文の推測ではなく、`C:/src/pelican2/build-off-openxr/_deps/` の nlohmann-json 3.12.0、tinygltf、tinyexr、miniaudio、Dear ImGui 1.91.9b、GLFW 3.4、Quill、GLM の実ヘッダを読んだ。Vulkan は同 build の `CMakeCache.txt:1208` が指す SDK 1.4.309 を主証拠とし、pipeline の複数-success 契約は 1.3.296/1.4.309/1.4.350 でも同じであることを確認した。指示に従いビルドとテスト実行はしていない。

## REFUTED の詳細

### B-2: SymbolId の kind 検査 bypass

`logicalrendertype.cpp:150-154` の早期 return により `:226-228` の `symbol` case が死ぬこと自体は正しい。しかし matcher は SymbolId を enum/range 等の値として直ちに確定せず、`symbolic_constraint_deferred` として後段へ渡す (`:835-860,873-987`)。テストも enum parameter に SymbolId を与えて遅延 bind することを要求している。したがって dead switch case は整理対象だが、「任意 kind に通ること」が欠陥という結論は逆である。

### B-5: sample count が静かに 1x へ降格

全 resource が 1x を持つことは `samplecountplanning.cpp:267-286` で前提化される。prefer/automatic は共通集合から最良値を選び、fallback の理由と制限 resource を返す (`:320-335`)。exact は不一致を throw する (`:303-318`)。テストも 4x 要求から 2x を選ぶ `preferred_common_fallback` を明示的に期待する。死んでいるのは「共通値なし」の防御 throw だけで、fallback は silent でも bug でもない。

### C-3: ReloadGate の生ポインタ UAF

現在は `PelicanCore::run` が ReloadGate を先、ReloadService を後に構築し、module container は LIFO 破棄する。ReloadService destructor は FileWatcher を stop/join するので、worker が参照する gate は join 完了後まで生存する。構築順を将来変えると危険という設計上の fragility はあるが、現行ディスク状態に UAF 経路はない。

### D-16: GLFW extension name の lifetime 違反

GLFW の pointer lifetime は terminate までであり、`Window::getRequiredVulkanInstanceExts` の型が非所有 pointer を外へ出すのは弱い API である。しかし flat path は `vk::createInstanceUnique` 呼出しまで同一 scope で即消費し、XR path は `vector<string>` へ即 copy する。いずれも Window/GLFW の破棄後まで保持しない。

## PARTIAL の詳細

### A-F3/A-F4/A-F9: 壊れた中間状態と実行継続の混同

3 件とも例外安全性の形は原報告どおりである。A-F3 は RT 単位 commit、A-F9 は swapchain 破棄先行、A-F4 は fence reset/command recording 後のゼロ extent throw である。一方、これらの例外を回復して次フレームへ進める catch はなく、`PelicanCore::run` が fatal として終了する。従って現在のユーザ実害は「その操作でアプリが落ちる」であり、「混在状態のまま描画を継続して後で壊れる」ではない。将来局所 recovery を導入するなら、3 件とも先に transaction 化が必要になる。

### A-F6: surface format 変更

swapchain format 変更は検出する一方、内部 RT と pipeline は旧 format のままという不整合は本物である。ただし output transform copy と ImGui には format equality guard があり、通常は Vulkan に不正 pipeline を渡す前に明示例外となる。正しい記述は「HDR/広色域移動時に format-aware graph 再コンパイルがなく fatal」。

### A-F10: present semaphore の本当の lifetime

`vkQueuePresentKHR` が OUT_OF_DATE/SURFACE_LOST を返しても、queue に投入された wait は実行されるため原報告の直接原因は成立しない。それでも render-finished semaphore を in-flight frame index で再利用する設計は不正である。submit fence の signal は presentation engine が wait を消費し終えたことを保証しない。swapchain image ごとの semaphore、または `VK_EXT_swapchain_maintenance1` の present fence に切り替える必要がある。

### A-F13: stale と lease

`surface_stale` が disabled mirror 中に残ること自体は、以後 mirror を呼ばないため有害ではない。通常の OUT_OF_DATE recovery は `completeAll()` を再構築より前に実行するので、再構築失敗を重ねても原報告の lease leak にはならない。実在する穴は `render_end` 内で submit/lease retain 後、予期しない present 例外が出た場合である。caller は呼出し直前に `frame_begun=false` としているため cleanup が再度 `render_end` せず、mirror も disable され、その slot の世代 snapshot が以後保持される。

### B-8: `noexcept` と try/catch の解釈

`noexcept` 関数内でも、関数自身の try/catch が例外を捕捉することはできる。4 registry の register 本体は allocation/lock/name copy を内側で捕捉して status を返すため、ABI thunk の advertised `out_of_memory/provider_error` は到達可能である。問題は unregister 本体で、`unique_lock` と `free_slots.push_back` を catch せず `noexcept` にしている。ここだけは外側 thunk に到達する前に terminate する。

### D-1: JSON-RPC malformed request

`{"jsonrpc":2.0,...}` で `value<string>` が `type_error.302` を投げるのは確認できる。通常の windowed request は reader が文字列を queue するだけで、main thread の frame boundary で parse され、最終的に `PelicanCore` の catch で fatal 終了する。queue が満杯の時だけ reader thread が busy response の ID を得るため直接 parse し、thread entry に catch がないため `std::terminate` になる。どちらも protocol error response を返さない点は修正対象である。

### D-5/D-14/D-15

- D-5: null backend は既定 backend 列の最後なので、物理 device 不在は app の catch に来ない。ただし app catch は OOM や null backend 自体の初期化失敗では生きている。「完全な dead code」ではなく、「無音成功を識別できない」が正しい。
- D-14: GLM は厳密な zero quaternion を identity にする。残る問題は巨大 JSON number の float 化による Inf、NaN、退化 matrix であり、finite/epsilon validation がないこと。
- D-15: `glfwGetRequiredInstanceExtensions` の header 記載 error は `GLFW_NOT_INITIALIZED` と `GLFW_API_UNAVAILABLE`。単一文言に潰す診断欠陥はあるが、「4 失敗理由」と ImGui が当該 error を後から捨てるという説明は正確でない。

## 実害が大きい CONFIRMED 上位 10 件

1. **C-1 — DeletionQueue の 1 epoch 早い解放。** frame 1 で使った texture を frame 2 の update で退役すると epoch 1 が付き、frame 3 の `DeletionQueue::beginFrame` が epoch 1 を解放してから frame 1 の slot fence を待つ。GPU が frame 1 を処理中なら use-after-free になる。なお frame 2 の Renderer 内、`beginFrame` 後に退役した旧 pipeline は epoch 2 となり、frame 3 で frame 1 fence を待った後の frame 4 まで残るため安全である。「全 defer が危険」ではなく update-before-render が穴である。
2. **B-1 — 不完全 teardown 後の DLL unload。** cleanup step の失敗をログだけで握り潰すため、旧 DLL の behavior/callback を保持したまま unload できる。次の callback は unmapped code を呼ぶ可能性がある。成功/失敗を構造化して unload 可否へ伝播させる必要がある。
3. **D-4 — ImGui Vulkan error hook 未設定。** allocation/device-lost を 46 箇所で無視し、少なくとも texture descriptor allocation は未初期化 handle の使用へ直結する。callback 設定と、backend が failure 後に続行しないことの双方を保証すべきである。
4. **D-12 — audio voice の無制限保持。** public map と backend map の二層が終了済み voice を保持し、decoded PCM も解放されない。短い SE を 10 回/秒なら 1 時間で約 36,000 voice が残る。stop/at-end の reap と backend destroy API が必要。
5. **D-11 — log file open 失敗で起動 terminate。** `pelican.log` が作れない cwd だけで core constructor が main の try 外から例外を出す。stderr fallback、または core 構築を main の error boundary 内へ移す小修正で防げる。
6. **D-2 — normalized glTF attribute の誤復号。** normalized UBYTE/USHORT の UV、色、weight が 0..1 へ変換されず、最大 255/65535 の値になる。一般的な glTF encoding なので到達性が高く、見た目や skinning が大きく壊れる。
7. **D-3 — 16-bit glTF image の format 不一致。** decoder は 16-bit RGBA byte 列を保持するのに upload は RGBA8 固定である。16→8 変換するか R16 format を選び、byte 数を format/extent から検算すべきである。
8. **A-F5 — surface lost の回復欠落。** surface object を作り直す所有 API がなく、該当 WSI error は vulkan.hpp 例外として最上位へ抜ける。頻度は低いが、RDP/表示系切替等からの復帰には Window surface、swapchain、関連 graph の世代交換が必要。
9. **A-F12 — XR mirror recovery の global idle。** optional desktop mirror の resize が全 device を idle にし、次の XR frame を止める。mirror の swapchain/present 資源だけを世代交換し、HMD composition queue の進行から切り離すべきである。
10. **A-F2 — 最小化競合の zero extent。** positive framebuffer を待った後に surface capabilities を再取得するまでの窓があり、固定 `currentExtent` は再検査しない。zero/invalid extent なら再待機または nonblocking skip する小さな修正で、ユーザ操作による fatal を除去できる。

## 修正の推奨順序

重大度を主、同程度なら修正コストの小さい順とした。

| 優先 | 対象 | 理由と最小方針 |
|---|---|---|
| P0 | C-1 | GPU UAF。DeletionQueue の release を最初の target fence wait 後へ移す、または update-phase defer を次 epoch に stamp する。複数 target/XR を含む fence 対応の設計確認が必要 |
| P0 | D-4 | 小修正で未初期化 Vulkan handle 使用を防げる。`CheckVkResultFn` を必須設定し、failure で backend 処理を中断 |
| P0 | B-1 | DLL unload 後の旧関数 pointer 呼出しを防ぐ。teardown step の structured result を reload transaction に返し、一件でも失敗したら unload/commit しない |
| P0 | D-12 | 長時間運転で確実に増える。backend に destroy/reap を追加し、終了 voice と stop voice を frame/update 境界で両 map から除去 |
| P1 | D-11, A-F2 | 小コストで起動不能・ユーザ操作 fatal を除去。logger fallback と extent 再検査 |
| P1 | D-2, D-3 | 一般資産の表示破壊。componentType/normalized ごとの変換と image bit depth/format の整合検査を中央化 |
| P1 | A-F10 | validation error/GPU 同期違反。render-finished semaphore を swapchain image に紐づけるか present fence を採用 |
| P1 | A-F12 | XR frame pacing。mirror recovery から global `waitIdle` を排除 |
| P1 | A-F5 | 稀だが回復不能。surface を含む WSI generation の再作成 API を設計 |
| P1 | B-9 | malformed asset 堅牢性。EXR version/header の tile 整合を decoder 前に検証 |
| P2 | D-1, D-7, D-8, D-9, D-10 | 外部入力の error boundary/診断を統一。tinygltf の bool だけでなく `err`/`warn` と required extension/index を検証 |
| P2 | A-F3, A-F6, A-F9 | 将来の局所 recovery を安全にする transaction 化。新世代を全作成後に一括 publish |
| P2 | D-5, D-6, D-14, D-15 | backend 選択・数値入力・診断の契約を明示。各々は小〜中 |
| P3 | B-3, B-6, B-7, B-ORDER/D-13, E-1, E-2 | 一行〜局所修正で schema/overflow/診断/noexcept 穴を解消 |
| P4 | B-4, B-T2-PIPE, B-T2-FENCE, B-T3-SCENE, B-T3-SAVE, B-T3-SHADER, B-T3-CANON, E-3〜E-7 | 現在は無害な dead/redundant code。機能修正と分けた cleanup change として処理 |
