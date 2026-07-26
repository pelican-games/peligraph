# バグハント生データ: Opus 監査 2 体の指摘(未検証を含む)

日付: 2026-07-26
性格: **エージェントの一次報告。裏取り済みの項目のみ「検証済み」と明記している。**
残りは**主張であって確定ではない**。本書は後続の検証・横展開の入力である。

## 0. 種バグ(修正済み・パターンの原型)

`src/core/vkcore/swapchainframetarget.cpp` の present 復帰分岐が到達不可能だった。
vulkan.hpp は `presentKHR` の許可結果を `{eSuccess, eSuboptimalKHR}` に限定し、
`eErrorOutOfDateKHR` は例外を投げる。よって「リサイズ時に swapchain を作り直す」
コードは一度も実行されず、ウィンドウリサイズでプロセスが死んでいた。
コンパイルは通り、コードは正しく読め、レビューも通っていた。
修正コミット: `20efe46`。

**教訓**: 書いてあることは実行の証拠にならない。ライブラリが実際に返す/投げる
ものをヘッダで確認する。

## 1. 検証済み(私が実コードで裏取り済み)

### V1. `VK_ERROR_DEVICE_LOST` が完全に未処理。検出分岐は全部死んでいる — HIGH

- `DeviceLost` は `src/` 全体で **0 ヒット**(検出も回復も存在しない)
- `Device::waitForFences` の許可結果はインストール済み 3 SDK すべてで
  `{eSuccess, eTimeout}`
  (`1.3.296.0/vulkan_funcs.hpp:1687`, `1.4.309.0:1847`, `1.4.350.0:1724`)
- よって `waitForFences(..., UINT64_MAX)` の後の `!= eSuccess` 判定は
  **timeout が起こりえず、それ以外は例外**なので到達不能

死にコードの所在(報告値):
`swapchainframetarget.cpp:249`, `:286`, `:452` /
`openxrcompositiontarget.cpp:232`, `:440`, `:453` /
`offscreenframetarget.cpp:145`, `:224`

症状: GPU ハング・TDR・ドライバリセットで `vk::DeviceLostError` が飛び、
`Loop::run` に try/catch が無いためプロセス終了。メッセージは
`vk::Queue::submit` のみで**エンジンのバグと区別できない**。

### V2. リサイズ後のキャプチャが範囲外読み込みしうる — HIGH

`releaseSurfaceDependants()`(`swapchainframetarget.cpp:159-165`)は
`swapchain_images` を空にするが **`current_image_index` をリセットしない**。
`recreateSurfaceDependants()`(`:198-209`)もしない。`has_rendered_frame` は
`:459` で true になったまま。

`readbackLastFrameRGBA8` は `:497` で `has_rendered_frame` を見て通過し、
`:511` で `swapchain_images[current_image_index]` を引き、`:515` で
`oldLayout = ePresentSrcKHR` と宣言する。再生成後はその画像は一度も描画・
present されていない(layout は UNDEFINED)ため不正遷移 + ゴミ画像。
新スワップチェーンの枚数が減っていれば `std::vector` の範囲外読み込み。

到達経路: エディタの `capture` rpc(`rpcserver.cpp:1105`)。

## 2. 未検証の主張(A: ウィンドウ/デバイス寿命 監査)

| ID | 主張 | 報告確信度 |
|---|---|---|
| A-F2 | `chooseSwapchainExtent`(`swapchainframetarget.cpp:26-28`)がゼロ検査なし。`waitFramebufferExtent`(`window.cpp:256-269`、`:168` で呼ぶ)と `getSurfaceCapabilitiesKHR`(`:46`)の間に最小化が挟まると extent {0,0} で swapchain/image 作成 → VUID 違反 | MEDIUM |
| A-F3 | `rendertargetcontainer.cpp:601-712` の `recreateForExtent` に rollback が無く、途中失敗で新旧 extent が混在。`renderer.cpp:1790-1793` の後処理(`rebindFullscreenInputs`/`layout_tracker.reset()`)も飛ぶ。**同ファイルの `registerRenderTarget`(`:587-597`)はロールバックしている**という非対称 | HIGH(形)/ MEDIUM(到達性) |
| A-F4 | 高さ 1px + `extent_scale` 0.5 で `rendertargetcontainer.cpp:64-75` がゼロ切り捨て → `:71-73` で throw。`render_begin()` 後なのでセマフォ signaled・コマンドバッファ記録中・フェンス未 signal のまま脱出。`window.cpp` に `glfwSetWindowSizeLimits` が無い | HIGH(経路)/ MEDIUM(既定構成) |
| A-F5 | `SurfaceLost` は 0 ヒット。サーフェスは `window.cpp:310-319` で一度だけ作られ再生成 API が無い(`swapchainframetarget.cpp:171-173` は同じものを再利用)。ドライバ再起動・RDP 遷移で回復不能 | HIGH |
| A-F6 | swapchain **フォーマット**変更は検出される(`:199-208`)が、応答が `recreateForExtent` だけで、RT は保存済み `rt.format` を再利用(`rendertargetcontainer.cpp:641`)、パイプラインは起動時コンパイルのまま(`renderingpassruntimecompiler.cpp:862-889`)。HDR/広色域モニタへの移動で不整合 | MEDIUM |
| A-F7 | 恒常的 `SUBOPTIMAL` で毎フレーム `recreateSurfaceDependants()` = `waitIdle` + swapchain 再構築。acquire 側は `eSuboptimalKHR` を受理して描画するので絵は正しいまま速度だけ崩壊。停止条件なし | MEDIUM |
| A-F9 | `surfaceDependantsSetup`(`:167-196`)が破棄先行。`extent`/`Camera::setScreenSize` を `:174-175` で確定した後に depth を `:179` で確保するため、OOM で「新 extent を広告しているのに depth が null」。`surface_stale = false` は成功時のみ(`:204`)。`submission_leases.completeAll()` は `:202` で先に走る | HIGH(形)/ MEDIUM |
| A-F10 | present 失敗時にセマフォ wait が実行された保証が無いのに、再生成はセマフォを作り直さない(`:213-214` で一度きり、`releaseSurfaceDependants` は触らない)。in-flight 2 なので 2 フレーム後に同じ binary semaphore を再 signal → VUID-vkQueueSubmit-pSignalSemaphores-00067 | MEDIUM |
| A-F11 | 最小化時 `glfwWaitEvents()`(`window.cpp:259-268`)が `recreateSurfaceDependants` 経由で呼ばれ、**ループ全体**が止まる。rpc・ホットリロード・ECS・音声も停止 | HIGH |
| A-F12 | `swapchainframetarget.cpp:444-447` のコメントは「mirror は device idle を待たない」と宣言しているが、`openxrmirrorsink.cpp:186`/`:291` の `recoverSurfaceIfStale` が `waitIdle` を呼ぶ。XR 中のミラーウィンドウ リサイズで HMD フレームが停止 | HIGH |
| A-F13 | `surface_stale` は `:204` でしか false にならず、mirror が `disabled`(`openxrmirrorsink.cpp:168-172`, `:297`)または `!xr_frame_renderable`(`loop.cpp:524`)だと**永久にラッチ**。また mirror の二重失敗で `submission_leases` スロットが解放されず世代スナップショットがプロセス寿命まで固定 | MEDIUM |

A 監査が「正しく処理されている」と判定した項目: 再生成時の caps/format/present
mode/preTransform/image count の再クエリ(キャッシュ無し)・in-flight 待ち
(`:201` の `waitIdle` が `:169` の破棄に先行)・リース捕捉順序(`:426-427`)・
ウィンドウクローズ中フレーム・ゼロ extent での CPU スピン無し。

## 3. 未検証の主張(B: 到達不可能経路 監査)

### Tier 1(死にコードが実際の穴を隠している)

| ID | 主張 | 報告確信度 |
|---|---|---|
| B-1 | **DLL reload の teardown 失敗経路が到達不能**。`gamelogicreload.cpp:318-323` の else は `teardown()` が throw した場合のみだが、本番は `runtimeTeardown`(`:468,469,476`)= `RuntimeTeardownGuard::run()` が **noexcept**(`teardown.hpp:88`, `teardown.cpp:109`)で、内部の `cleanupStep` が `catch(...)` で握り潰す(`teardown.cpp:18-30`)。**結果: teardown の一部が失敗しても素通りで `FreeLibrary`(`:171-172`)され、生きた entity が未マップ DLL の関数ポインタを保持したまま「reload 成功」と報告される** | HIGH |
| B-2 | `logicalrendertype.cpp:226-228` の `symbol` case が `:150-153` の早期 return で先取りされ死亡。**副作用として `:157` の `value_kind` 一致検査も飛ぶ → 記号引数がどの種類のパラメータにも通る** | HIGH |
| B-3 | `editorpreviewprojection.cpp:86` の `found->empty()` は nlohmann の string に対し常に false(`default:` 扱い)。空文字が通り、後段で `schema_violation` でなく `method_unavailable` になる。**他 14 箇所は正しく `get_ref<const std::string&>().empty()` を使っている唯一の例外** | HIGH |
| B-4 | `logicalrendergraph.cpp:234-238` の「import かつ produce」検査は、`imports` が version 0 強制(`:205-209`)・`producers` が version≠0 強制(`:219-223`)で集合が素なので常に false | HIGH |
| B-5 | `samplecountplanning.cpp:64-66` の「共通サンプル数なし」が到達不能(呼び出し元 `:321` が `:282-286` で 1 非対応を既に throw)。**非互換グループが error でなく 1× に静かに降格** | HIGH |
| B-6 | `materialformat.cpp:369-373` / `surfaceformat.cpp:325-327` の整数オーバーフロー catch が発火不能(`is_number_integer()` が通す型では nlohmann は単なる static_cast)。INT64_MAX 超のリテラルが負値に回り込む | HIGH |
| B-7 | `renderingpassvalidation.cpp:214-216` の swapchain ガードが、それが守るはずの `:209` の `rt_metadata.get()` より後。`at()` が先に `std::out_of_range` を投げる | HIGH |
| B-8 | ABI thunk 4 箇所(`graphtransformregistry.cpp:1636-1639,1662-1664` ほか `subgraphreplacementregistry` / `passimplementationregistry` / `renderstrategyregistry`)が `noexcept` 内で try/catch し、`out_of_memory`/`provider_error` を返すと広告しているが到達不能(escape すれば `std::terminate`) | HIGH |
| B-9 | `imageloader.cpp:186-194` の EXR 三重拒否が死亡。**副作用: version bit 9 が立っていない tile 付き EXR を tinyexr が scanline として復号し、その死んだ二層目こそがそれを捕まえるはずだった** | HIGH |

### Tier 2(種バグの直系 — vulkan.hpp が投げる)

- `pipelinefactory.cpp:428-430`, `:448-450` — `createGraphicsPipelineUnique` /
  `createComputePipelineUnique` の許可は `{eSuccess, ePipelineCompileRequiredEXT}`
  で、後者は専用 flag が要るが `GraphicsPipelineDesc` に flags が無い → throw 本体が死亡。
  影響は軽微(caller が `std::exception&` を捕まえる)
- `offscreenframetarget.cpp:145-150`, `:224-229` /
  `openxrcompositiontarget.cpp:230-233`, `:439-442`, `:451-456` — V1 と同型
- **`rendertiming.cpp:315` は欠陥ではない**(raw ポインタ版 `getQueryPoolResults` は
  noexcept overload で結果を直接返す)

### 順序欠陥(死にコードではない)

- `gamelogicreload.cpp:136-139` — `platformLoadError()`(`GetLastError()`/`dlerror()`)を
  `releaseGameLogicRegistrations(owner)` の**後**に読む。間に 11 パスの解放と
  ユーザ `onDestroy` とログが挟まる → 報告されるローダエラーが上書きされうる。
  一行の並べ替えで直る。MEDIUM

### Tier 3(死んでいるが無害・約 45 件)

一行の防御ガードが多数。詳細は元報告参照。注目すべき例外:

- `scene.cpp:266` — scene format の**警告レーン全体が死んでいる**
  (`sceneformat.hpp:13` の `warnings` は一度も書かれず、診断は全部 hard throw)。
  アクセサ・3 代入・swap がドキュメント公開のたびにコピーされている
- `basicconfig.cpp:733` — 死んだガードのために**保存のたびに JSON 全体を再パース**
- `reloadservice.cpp:274-276` — shader participant の per-request enqueue が
  常にスキップされる(`:573-575`/`:588-591` で consumed 済み)
- 同じ「非正準チェック」が 4 箇所に重複(`targetrenderplanning.cpp:102`,
  `vulkanphysicalfragment.cpp:63`, `targetplanning.cpp:30`,
  `logicalrendertype.cpp:273`)— `parseSemanticTypeId` の構成上、再組立は常にバイト一致

### B 監査の未カバー領域(重要)

`src/core/renderer/`, `src/core/fullscreenpass/`, `src/core/ecs/`, `imgui/`,
`ui/`, `phys/`, `audio/`, `communication/`, `userpublic/`, `devstudio/`,
`devcli/` は**未監査**。

## 4. 導出したバグ類型(横展開の軸)

| 型 | 定義 | 実例 |
|---|---|---|
| **T1 ラッパ契約の取り違え** | C++ ラッパが下位 API の誤差契約を例外へ変換しているのに、戻り値を比較している | 種バグ, V1, Tier 2 全部 |
| **T2 守る対象より後ろのガード** | 検査が、それが守るはずの操作の後に置かれている | B-7 |
| **T3 早期 return / フィルタによる先取り** | 前段の早期脱出で後段の case・検証が到達不能になり、**その検証自体が飛ぶ** | B-2 |
| **T4 ライブラリ意味論による恒真・恒偽** | 述語がライブラリの仕様上つねに同じ値 | B-3(nlohmann `empty()`) |
| **T5 noexcept 内の try/catch** | noexcept 関数内で例外を捕まえて返り値にする設計(escape すれば terminate) | B-8 |
| **T6 不変条件で素な集合の交差検査** | 構築時の不変条件により交差しえない集合を検査 | B-4 |
| **T7 呼び出し元が既に検証済みの fallback** | 上流が throw 済みで、下流の fallback が到達不能。**結果として静かな降格** | B-5 |
| **T8 エラー状態を遅れて読む** | `GetLastError`/`dlerror` を介在呼び出しの後に読む | 順序欠陥 |
| **T9 握り潰す cleanup が成功を報告** | `catch(...)` で握った失敗が上位に伝わらず、成功として処理が続く | **B-1(最も危険)** |
| **T10 状態のリセット漏れ** | 再生成・再初期化でインデックス/フラグが古いまま | V2, A-F13 |
| **T11 部分失敗のロールバック欠如** | 途中で throw すると新旧が混在した状態が残る | A-F3, A-F9 |
| **T12 未処理の環境条件** | デバイス喪失・サーフェス喪失など、そもそも検出コードが無い | V1, A-F5 |

## 5. 未検証の主張(C: 並行性 監査)

### C-1. DeletionQueue の epoch が 1 フレーム早く、GPU use-after-free — HIGH ★最重要

**構造は私が裏取り済み**: `renderer.cpp:2490-2491` で `deletion_queue.beginFrame()` が
`renderLogicalFrame` の**先頭**にあり、`deletionqueue.cpp:74-82` は
`++current_frame` 直後に `releaseEligible(current_frame - in_flight_frames)` を
呼んで実破棄する(`in_flight_frames_num = 2`、`rendertarget.hpp:40`)。

主張の核心: `current_frame` は `renderLogicalFrame` 内でしか進まないため、
**update フェーズで defer されたリソースは「前フレーム」の epoch を持つ**。
update フェーズの defer 実例 = テクスチャ ホットリロード
(`framephase.cpp:123` → `texturereloadhandler.cpp:227`, `:229`)。

| 時点 | 状態 |
|---|---|
| F2 update | 旧 image/descriptor を `current_frame == 1` で defer |
| F2 render | `beginFrame()` → 2 → `releaseEligible(0)` → まだ生存。submission S2 発行 |
| F3 render | `beginFrame()` → 3 → `releaseEligible(1)` → **ここで破棄** |
| F3 の数百行後 | `swapchainframetarget.cpp:244` で待つのは S1(F1)のフェンス |

つまり S1 実行中に破棄されうる。`GpuSubmissionLease` は
`RenderPipelineRuntimeGeneration` しか保持せず、マテリアル テクスチャ/
descriptor/パイプラインは**カバーしない**。

**headless では再現しない**(`offscreenframetarget.cpp:224-231` が submit 直後に
フェンス待ちで CPU/GPU を完全直列化)→ **golden/CI が構造的に検出できない**。

render フェーズ後の defer(`pipelinefactory.cpp:576,579`・
`rendertargetcontainer.cpp:685`・`computetask.cpp:741`)は境界上で安全。

副次(MEDIUM): submission を出さない論理フレーム
(`swapchainframetarget.cpp:246-248`, `:273-277`, `:279-282` が
`in_flight_frame_index` を進めずに nullopt 返却)で epoch と slot 周期が恒久的にズレる。

### C-2. WindowedRpcHost の detach がストリーム参照より長生き — MEDIUM(テスト)/ LOW(本番)

`windowedrpchost.cpp:96-103` の `reader.detach()`。キュー状態の扱いは正しい
(`:86` の decay-copy、`:89` の `stopping` 先行格納)。問題は
`WindowedRpcQueueState` が `std::istream&` / `std::ostream&`(`:16-17`)を
**所有しない参照**で持つこと。detach したスレッドは `:55` の
`std::getline(state->input, line)` に居座る。

- 本番は `std::cin`/`std::cout`(`loop.cpp:441-446`)なので実害は静的破棄順のみ = LOW
- **テストはスタック上の stringstream**(`test/windowedrpchost_test.cpp:57-61`, `:93-100`)。
  `REQUIRE(host.readerFinished())` 失敗時(`:21`)に Catch2 が throw → `host` 破棄 →
  detach → 一段外側で stringstream 破棄、その間スレッドは `getline` 中 = MEDIUM

### C-3. ReloadGate の生ポインタは構築順に依存した安全性 — LOW

`reloadservice.cpp:422-423` が `&GET_MODULE(ReloadGate)` を watcher スレッドの
lambda に捕獲。`FastModuleContainer` は LIFO 破棄(`container.hpp:262-272`)なので、
`ReloadGate` が `ReloadService` より後に初回構築されていたら先に壊れる。
現状 `pelican_core.cpp:64` が `:83` より先に構築するので**偶然安全**。

関連: `container.hpp:151-153` の lock-free fast path が `__ready()` 確認後に
`__get().value()` を deref する一方、`destroyModule<T>`(`:116-119`)は
false 格納 → reset の順。teardown 中に生きているスレッドが `GET_MODULE` すると危険。

### C 監査のクリーン判定(信頼できる)

publication root(`framegraphruntime.cpp:431-490` の CAS/acq_rel、ABA 無し、
スナップショット 1 回)/ WindowedRpcHost のキュー・停止プロトコル /
NativeWatch のキャンセル(`filewatcher.cpp:111-193`)/ FileWatcher の worker 分割 /
JobSystem / parallelPrepareOrdered / DeferredCallbackLifetime /
ResourceRegistry / DLL unload のスレッド面 / processrunner。

## 6. 未検証の主張(D: 非 Vulkan ラッパ契約 監査)

| ID | 主張 | 報告確信度 |
|---|---|---|
| **D-1** | **`jsonrpc.cpp:173` の `document.value("jsonrpc", std::string{})` が throw しうる**。nlohmann の `value()` はキーが**存在すれば** `get<T>()` を呼ぶ(`json.hpp:2253-2269`)ので、`{"jsonrpc": 2.0}` で `type_error.302`。`parseJsonRpcRequest` の try は `json::parse` しか覆っていない(`:150-154`)。`rpcserver.cpp:720` は try(`:737`)の**前**で呼ばれ、`windowedrpchost.cpp:40` は**素の std::thread 上**(`:86`)→ **`std::terminate`** | HIGH |
| **D-2** | tinygltf の `accessor.normalized`(`tiny_gltf.h:814`)を主頂点経路が**読んでいない**(`gltf.cpp:611-771`、`readComponentByType` は素の static_cast)。正規化 UNSIGNED_SHORT の TEXCOORD/COLOR/WEIGHTS が 65535 倍に。**同じリポジトリの `gltf.cpp:782-784` と `vrmadecoder.cpp:328-329` は normalized を明示的に拒否している**(主経路だけ抜けた) | HIGH |
| **D-3** | 16bit PNG を含む glTF: `gltfimage.cpp:27-41` が 16bpc で復号し `bits=16` を設定するが、`gltf.cpp:1366-1375` は `eR8G8B8A8Unorm` 決め打ちで `image.bits`/`pixel_type` を**どこも読まない**。`util.cpp:82-114` はサイズ照合しないので validation も出ず、上半分を 8bit として読む | HIGH |
| **D-4** | **imgui の `CheckVkResultFn` を install していない**(`imguisystem.cpp:313-326`、grep 0 件)。`imgui_impl_vulkan.cpp:396-404` の 46 箇所の VkResult チェックが全部 no-op。`:1228` は `VkDescriptorSet` を**未初期化**で宣言し、alloc 失敗後もそのまま `vkUpdateDescriptorSets` へ | HIGH |
| **D-5** | miniaudio の null backend fallback(`audio.cpp:409-415`)が到達不能。`ma_engine_init` は再生デバイスが無くても null backend へ降格して `MA_SUCCESS` を返す(`miniaudio.h:44043-44052`, `:6739`, `:43332-43337`)。**音が出ないのにログ上は正常** | HIGH |
| **D-6** | `ma_sound_start` の失敗分岐(`audio.cpp:366-369`)が死にコード。`miniaudio.h:78746-78772` の非成功経路は `pSound==NULL` と `at_end` のみで、直前の init が両方を排除。実際の再生失敗は成功として報告される | HIGH |
| **D-7** | tinygltf は外部画像の読み込み失敗を **`warn` + `return true`** で通す(`tiny_gltf.h:4404-4415`)。`gltf.cpp:214-222` は `!loaded` しか見ないので `width` が -1 のまま `gltf.cpp:1370` で `0xFFFFFFFF` に。エラーは「glTF texture candidate is empty」で**ファイル名も原因も出ない**。さらに `scene.cpp:628` は `inspect()` を呼ぶが `warn` を出すのは `commit()` のみ | HIGH |
| **D-8** | tinygltf は壊れた animation channel を**黙って捨てて** `true` を返す(`tiny_gltf.h:5552-5563`)。`vrmadecoder.cpp:571-577` は `!LoadBinaryFromMemory` の分岐でしか errors/warnings を読まない → 動かないボーンができる | HIGH(契約)/ MEDIUM(頻度) |
| **D-9** | tinygltf は `asset.version` 欠落でも `err` に積んで `true` を返す(`tiny_gltf.h:4280-4290`)。`err` が非空でロードを止める呼び出し元が**一つも無い** | HIGH(契約)/ MEDIUM |
| **D-10** | `gltf.cpp:1367` が `model.textures.at(...).source` を無検査で `images.at()` へ。`Texture::source` の既定は **-1**(`tiny_gltf.h:659`)で `source` は optional(`:4439-4459`)。**他の -1 index は全部ガードされている**(7 箇所)のにここだけ抜け。KHR_texture_basisu / EXT_texture_webp / avif で発火 | HIGH(欠落)/ MEDIUM(重大度) |
| **D-11** | quill の `FileSink` が throw しうる(`FileSink.h:370`)のに、`log.cpp:16-28` の `setupLogger` は `PelicanCore` の**コンストラクタ**(`pelican_core.cpp:33`, `:40`)から呼ばれ、`main.cpp:443` の構築も try の外 → `std::terminate`。`logFileName` は相対パス `"pelican.log"`(`config.hpp:9`) | HIGH |
| **D-12** | miniaudio の voice が**一度も解放されない**(`audio.cpp` に `erase` が 0 件)。`playSound` は毎回新規デコードし、node もキャッシュも解放されない。10/s で 1 時間 = 約 36000 ノード + 同数のデコード済みバッファ | HIGH |
| D-13 | `gamelogicreload.cpp:136-139` が `platformLoadError()` を `releaseGameLogicRegistrations` の**後**に読む(B 監査と同一指摘) | MEDIUM |
| D-14 | `gltf.cpp:929`, `:937-940` の `glm::normalize` が未検証クォータニオンに。**同リポジトリの `vrmaretarget.cpp:42-46` と `physworld.cpp:63-69` は同じケースを明示ガードしている** | MEDIUM |
| D-15 | GLFW の `glfwGetError` を**一度も読んでいない**(grep 0 件)。`glfwGetRequiredInstanceExtensions` の 4 つの失敗理由が単一 NULL に潰れる。さらに imgui が `ImGui_ImplGlfw_Init` で `(void)glfwGetError(nullptr)` してキューを捨てる | MEDIUM(診断性) |
| D-16 | `window.cpp:321-332` が GLFW の返す `const char*` をライブラリ終了後まで保持しうる契約違反(現状は文字列リテラルなので動作) | LOW |

### D 監査のクリーン判定

GLFW(`glfwCreateWindowSurface` は生 `VkResult` を正しく比較)/ VMA-Hpp(全部
throw・比較を試みていない)/ shaderc / SPIRV-Reflect / tinyexr / **自作 KTX2
パーサ**(全読み込みを境界検査、DFD 検証、mip 連鎖、オーバーフロー検査)/
stb_image / glm(`perspectiveRH_ZO` を明示使用)/ argparse / picosha2 /
battery-embed / RenderDoc / Win32 last-error(`filewatcher.cpp:173` は
`bytes == 0` を**オーバーフローとして正しく扱っている**)。

**未監査**: SPIRV-Tools(`PELICAN_WITH_SPIRV_LINK` 既定 OFF)、Catch2。

### D 監査の総括(パターンの確認)

> 欠陥は**ラッパが契約を隠した場所**にだけ集まっている。明示的な分類を強制する
> API(GLFW の生 VkResult・VMA-Hpp の throw・SPIRV-Reflect・tinyexr・自作
> KTX2・Win32 watcher)は全部クリーンだった。OpenXR の観察と完全に一致する。

類型の追加:

| 型 | 定義 | 実例 |
|---|---|---|
| **T13 hook 未設定でエラーが蒸発** | ライブラリがエラーをコールバックに流すが、誰も install していない | D-4 |
| **T14 `true` は「続行した」であって「成功した」ではない** | 部分失敗を成功として返すライブラリ契約 | D-7, D-8, D-9 |
| **T15 全域に見えるアクセサが throw する** | `value()` のように既定値付きに見えて型不一致で投げる | D-1 |
| **T16 兄弟経路が守っているのに一つだけ抜け** | 同じ検査が他所にあるのにその経路だけ欠落 | D-2, D-10, D-14 |
| **T17 epoch/世代の 1 ずれ** | 解放が実際の GPU 完了より 1 世代早い | C-1 |

## 7. 未検証の主張(E: renderer / fullscreenpass / watch 監査)

この 3 ディレクトリに `vk::Result` 比較は **0 件**(種バグ本体は再発していない)。
ただし親戚が 2 つ。

| ID | 主張 | 報告確信度 | 実害 |
|---|---|---|---|
| **E-1** | `atlasassetresource.cpp:74-79` の BC5/未対応形式の診断が死亡。呼び出し元 `:138-147` が同じ述語で先に throw する。**結果: BC5 atlas を指定すると「size が合わない」という誤った原因が表示される**(実際はサイズは正しく形式が違う) | HIGH | 診断の誤誘導 |
| **E-2** | `renderpolicyregistry.cpp:540` の catch が到達不能(`unregisterDrawSortProvider` が `noexcept`、`:70-71`/`:387-389`)。**しかも当該関数内の `:392` の `unique_lock` と `:407` の `push_back` は throw しうる → メモリ逼迫下の unregister がプロセス終了**。graceful path が書かれているのに使えない | HIGH | 終了 |
| E-3 | `debugdraw.cpp:84` / `debugtext.cpp:191` の `descriptor_sets.empty()` 検査が構造上つねに false(`vulkan_funcs.hpp:4336-4347` は必ず `descriptorSetCount` 個返す)。他 4 箇所は直接 index しており、この 2 つが例外 | HIGH | 診断のみ |
| E-4 | `fullscreenpasscontainer.cpp:457-460` が `:388-395` の重複 | HIGH | 無害 |
| E-5 | `frameresources.cpp:41-46`, `:63-68` のオーバーフロー ガードが `> 32` の後ろで無意味 | HIGH | 無害 |
| E-6 | `drawqueuebuilder.cpp:126, :136, :150` の enum fallback が `validateSnapshot`(`:421`, `:84-92`)の後で死亡 | HIGH | 無害 |
| E-7 | `reloadservice.cpp:169` の早期 return が、唯一の呼び出し元(`:219`)が常に `attempted = true`(`:191`)を渡すため死亡 | HIGH | 無害 |

E 監査のクリーン判定: filewatcher / contentdigest / reloadgate / reloadqueue /
assetkey / reloadtransaction / camera(28 throw すべて到達可能)/ spritescene /
spriterenderer / uirenderer / uicontainer / materialrender /
fullscreenpassrenderer / temporal / indirectdrawlimits / modelinstanceslots /
shadowdepthpasscontainer / velocitypasscontainer / projectionjitter
(`:64-68` は table pattern 経由で到達可能 = 生きている)/
polygoninstancecontainer の catch 群。
