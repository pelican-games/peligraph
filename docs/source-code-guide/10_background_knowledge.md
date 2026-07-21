# 第10章 前提知識の補足(このコードが当然としていること)

調査時点: 2026-07-21 / 基準コミット: `bbc6c9d`

第2〜9章の「🧩 難所」ブロックが**このコードベース固有の難しさ**を扱うのに対して、この章は **pelican2 のコードが「知っている前提」で書かれている一般知識**をまとめたものです。仕様の細部や定番イディオムの名前を知らないと、コード自体は素直なのに読めない — そういう箇所を拾ってあります。

> **この章の作り方:** エンジンのソースを経験の浅い読み手に読ませ、「分からなかった」と報告された 88 箇所を、実装と突き合わせて仕分けしました。**このコードベース固有の難所だった 25 件**は第2〜9章の難所ブロックへ、**「一般知識を知らないから読めなかった」残り**をこの章に集めています。つまりここに並ぶのは、エンジンの設計ではなく **Vulkan / glTF / C++ / OS / アルゴリズムの側の知識**です。

各項目は「一般知識の説明」→「**このリポジトリでは**どこに出てくるか」の順です。上から読む必要はありません。コードで見慣れない書き方に出会ったときに引いてください。

## 10.1 Vulkan の仕様と作法

### swapchain の maxImageCount が 0 のとき

`vk::SurfaceCapabilitiesKHR` の `minImageCount` / `maxImageCount` は、そのサーフェスに対して作れる swapchain イメージ枚数の範囲です。Vulkan 仕様では `maxImageCount` の 0 は「特別値で、上限がない(メモリなど他の要因が許す限り何枚でもよい)」と定義されています。0 を素直に上限として扱うと `std::min` で枚数が 0 になり、swapchain 作成が即失敗します。そのため「`maxImageCount > 0` のときだけクランプする」という書き方が定型になります。`minImageCount + 1` を狙うのも定型で、最低枚数ちょうどだと presentation engine が 1 枚握っている間 CPU が次のイメージを取得できず待たされるためです。なお `minImageCount` は「これ以上を要求せよ」という下限であり、ドライバはそれより多く返してくることがあるので、実際の枚数は `getSwapchainImagesKHR` の戻り値で数え直す必要があります。

**このリポジトリでは**: `src/core/vkcore/swapchainframetarget.cpp` の `chooseSwapchainImageCount()`(`maxImageCount > 0` のときだけ `std::min` を掛ける)。実枚数は同ファイル `getImageFromSwapchain()` で取得しています。

### VkSurfaceCapabilitiesKHR::currentExtent の 0xFFFFFFFF

`currentExtent` はサーフェスの現在のサイズですが、仕様では「`width` と `height` がともに `0xFFFFFFFF`(= `UINT32_MAX`)のとき、サーフェスサイズは swapchain 側の `imageExtent` によって決まる」という特別値になっています。これは Wayland のように「ウィンドウのサイズをアプリが決める」プラットフォーム向けの規定です。この場合はアプリが自前でフレームバッファサイズを調べ、`minImageExtent` / `maxImageExtent` の範囲にクランプして渡す必要があります。逆に特別値でなければ `currentExtent` をそのまま使うのが正解で、独自に計算した値を渡すと不一致になります。ウィンドウ最小化時に extent が 0 になる環境もあるため、0 のときはフレームを作らず抜ける、という分岐も定型です。

**このリポジトリでは**: `src/core/vkcore/swapchainframetarget.cpp` の `chooseSwapchainExtent()` が `std::numeric_limits<uint32_t>::max()` と比較して分岐します。extent 0 の回避は同ファイル `SwapchainFrameTarget::beginFrame()` 冒頭の `framebufferExtent()` チェックです。

### sRGB フォーマットをスコアで選ぶ理由

Vulkan では、`*_SRGB` 形式のイメージに書き込むと線形値→sRGB 伝達関数のエンコードがハードウェアで自動的に掛かり、シェーダからサンプリングすると逆にデコードされて線形値が返ります(変換対象は RGB のみで、アルファには掛かりません)。つまり sRGB フォーマットを選べば、シェーダ側は最後までリニア空間で計算したままでよく、`pow(c, 1/2.2)` 相当を自前で書かなくて済みます。UNORM 形式を選ぶとこの自動変換がないので、同じシェーダのままだと画面が暗く沈みます。一方 `colorSpace` の `eSrgbNonlinear` は「presentation engine はこのイメージのデータを sRGB エンコード済みとして扱う」という別の宣言で、フォーマットとセットで意味を持ちます。サーフェスが返すフォーマット一覧に希望のものがあるとは限らないため、「必須」ではなくスコア付けして降順に並べ、先頭を採る書き方にしておくと、非対応環境でも起動だけはできます。

**このリポジトリでは**: `src/core/vkcore/swapchainframetarget.cpp` の `createSwapchain()` 内 `pred_fmt` ラムダ(sRGB+`eSrgbNonlinear` に 20 点、UNORM+`eSrgbNonlinear` に 10 点)。オフスクリーン側は `src/core/vkcore/offscreenframetarget.cpp` の初期化子で `color_format{vk::Format::eR8G8B8A8Srgb}` を第一候補にしています。

### present mode で必ず存在するのは FIFO だけ

`VkPresentModeKHR` のうち、Vulkan 仕様が「必ずサポートされる」と定めているのは `VK_PRESENT_MODE_FIFO_KHR` のみです。FIFO はキューに積んで垂直同期ごとに 1 枚出す、いわゆる VSync 有効の挙動で、ティアリングは起きませんが最大 1 フレーム分の待ちが入ります。`MAILBOX` は「キューが埋まっているとき、未提示のイメージを新しいもので置き換える」モードで、ティアリングなしのまま遅延を減らせますが、対応していない環境も珍しくありません。したがって present mode も「必須」ではなくスコア付けで優先度を表現するのが定型です。`std::stable_sort` を使うのは、同点(スコア 0)同士のときにドライバが列挙した順序を壊さないためで、MAILBOX がなければドライバが最初に挙げたモードが選ばれます(FIFO は一覧に必ず含まれますが、先頭とは限りません)。

**このリポジトリでは**: `src/core/vkcore/swapchainframetarget.cpp` の `createSwapchain()` 内 `pred_mode` ラムダ(`eMailbox` に 10 点)と、その直後の `std::stable_sort` / `surface_presentmodes[0]`。

### sharing mode: eExclusive と eConcurrent

Vulkan のバッファ/イメージは「どのキューファミリからアクセスされるか」を作成時に宣言します。`VK_SHARING_MODE_EXCLUSIVE` は「同時に 1 つのキューファミリだけが所有する」モードで、別ファミリから触るには queue family ownership transfer(release 側と acquire 側の 2 本のバリア)を明示的に記録しなければなりません。`VK_SHARING_MODE_CONCURRENT` は `pQueueFamilyIndices` に列挙したファミリから所有権移動なしにアクセスできますが、実装によっては圧縮などの最適化が無効になり性能が落ちます。そのため「graphics と present が同一ファミリなら EXCLUSIVE、別なら CONCURRENT」という分岐が定番です。仕様上も CONCURRENT のときは `queueFamilyIndexCount` が 2 以上かつ各インデックスが相異なる必要があるので、同一ファミリで CONCURRENT を指定するのは単に違反です。

**このリポジトリでは**: `src/core/vkcore/swapchainframetarget.cpp` の `createSwapchain()`、`graphics_queue_family == presentation_queue_family` で `eExclusive` / `eConcurrent` を切り替え、後者のみ `setQueueFamilyIndices(queue_families)` を呼びます。

### usage flags と format feature flags は別物

`VkImageUsageFlags`(`eColorAttachment`、`eTransferSrc` など)は「このイメージをどう使うつもりか」というアプリ側の宣言です。対して `VkFormatProperties` の `optimalTilingFeatures` / `linearTilingFeatures` / `bufferFeatures` は「その物理デバイスでその形式が実際に何をできるか」という能力の報告で、両者は別の軸です。usage に書いただけでは足りず、そのフォーマットが対応する feature を持っていなければ `createImage` は失敗します。feature は tiling ごとに別なので、`eOptimal` で作るなら `optimalTilingFeatures` を見る必要があります(linear の方を見て通した気になるのがよくある間違いです)。swapchain の場合はさらに軸が 1 本増え、サーフェス側の `VkSurfaceCapabilitiesKHR::supportedUsageFlags` にも許可がないと、そのイメージ用途は使えません。

**このリポジトリでは**: `src/core/vkcore/offscreenframetarget.cpp` のコンストラクタが `getFormatProperties(color_format).optimalTilingFeatures` と `required_features`(`eColorAttachment | eTransferSrc`)を突き合わせ、足りなければ UNORM にフォールバックします。swapchain 側は `src/core/vkcore/swapchainframetarget.cpp` の `capture_available` が `supportedUsageFlags` と `optimalTilingFeatures` の両方に `eTransferSrc` があることを確認しています。

### timeout に 0 を渡す waitForFences / acquireNextImageKHR

`vkWaitForFences` の `timeout` はナノ秒で、仕様では「0 を渡した場合は待たずにフェンスの現在状態をそのまま返す。条件が満たされていなければ `VK_TIMEOUT` を返す」と定められています。つまり timeout 0 は「ポーリング」であり、フレームが GPU 上でまだ終わっていないことを非ブロックで検出する手段になります。`vkAcquireNextImageKHR` にも同じ引数がありますが、こちらは 0 のとき「取得できなければ `VK_NOT_READY` を返す」という規定で、`VK_TIMEOUT` が返るのは timeout が 0 でも `UINT64_MAX` でもない場合です。両方を見て「まだ」と判断するコードは、この境界を跨いでも安全側に倒すための書き方です。逆に `UINT64_MAX` は「無期限に待つ」で、通常のブロッキング経路はこちらを渡します。

**このリポジトリでは**: `src/core/vkcore/swapchainframetarget.cpp` の `SwapchainFrameTarget::beginFrame(bool nonblocking)` が `nonblocking ? 0 : UINT64_MAX` を `waitForFences` と `acquireNextImageKHR` の両方に渡し、`eTimeout` / `eNotReady` を `std::nullopt` に変換します。

### vulkan-hpp の戻り値規約(いつ Result が返り、いつ例外が飛ぶか)

vulkan-hpp は `VULKAN_HPP_NO_EXCEPTIONS` を定義していなければ既定で例外を使いますが、すべての関数が例外だけで済むわけではありません。成功コードが 1 つ(`VK_SUCCESS` のみ)のコマンドは値だけを返し、エラー時に `vk::SystemError` を投げます。一方、仕様上の成功コードが複数あるコマンドは `vk::Result` あるいは `vk::ResultValue<T>` を返し、どの成功コードだったかは呼び出し側が `.result` を見て判断しなければなりません。`vkWaitForFences` は `VK_SUCCESS` と `VK_TIMEOUT`、`vkAcquireNextImageKHR` は `VK_SUCCESS` / `VK_TIMEOUT` / `VK_NOT_READY` / `VK_SUBOPTIMAL_KHR` が成功コードなので、いずれも後者に該当します。同じ理由で `presentKHR` も `eSuboptimalKHR` を戻り値として返します。

**このリポジトリでは**: `src/core/vkcore/swapchainframetarget.cpp` の `beginFrame()` が `waitForFences` の戻り値を `fence_result` に受け、`acquireNextImageKHR` の戻り値を `.result` / `.value` で分解しています。`src/core/vkcore/offscreenframetarget.cpp` の `render_begin()` も同じ形で `vk::to_string(result)` をログに出します。

### vk::DrawIndexedIndirectCommand の各フィールド

`VkDrawIndexedIndirectCommand` は `vkCmdDrawIndexed` の引数をそのまま GPU 可読なメモリ上の構造体にしたもので、フィールドの順序と意味は仕様で固定されています。`indexCount` は描画するインデックスの個数、`instanceCount` はインスタンス数、`firstIndex` はインデックスバッファ内の開始位置(バイトではなくインデックス要素単位)、`vertexOffset` は `int32_t` でインデックス値に加算される頂点オフセット、`firstInstance` は最初のインスタンス ID です。`firstIndex` と `vertexOffset` はどちらも「オフセット」ですが軸が違い、前者はインデックスバッファ側、後者は頂点バッファ側を指します。集約初期化で 5 つの数値を並べて書くとこの順序を取り違えても型が通ってしまうため、読む側は仕様順を覚えておく必要があります。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `makeDrawItemSnapshot()` が同じ 5 項目を backend-neutral な `DrawIndexedArguments` へ名前付きで保存し、`src/core/renderer/drawqueuebuilder.cpp` の `materialize()` がその順で `vk::DrawIndexedIndirectCommand` へ変換します。model staging と hot-reload rebuild は同じ snapshot helper を通ります。

### drawIndexed の vertexOffset はインデックス値に加算される

`vkCmdDrawIndexed` の `vertexOffset` は、仕様上「頂点バッファを引く前にインデックス値へ加算される値」です。インデックスバッファから読んだ値が `i` なら、実際に参照される頂点は `i + vertexOffset` になります。`int32_t` なので負値も取れます。これがあるおかげで、複数のメッシュを 1 本の頂点バッファと 1 本のインデックスバッファに連結して詰めても、各メッシュのインデックス値を 0 起点のまま書き換えずに済みます(連結時のずれは `firstIndex` と `vertexOffset` の 2 つで吸収する)。`firstIndex` はインデックスバッファ側の読み出し開始位置なので、両方を正しく設定しないと、別メッシュの頂点を引いた壊れた形状になります。

**このリポジトリでは**: `src/core/renderer/spriterenderer.cpp` の `buildSpriteDrawData()` が、チャンクごとに `chunk_first_index` と `chunk_vertex_offset` を記録して `SpriteDrawRun` に持たせ、`SpriteRenderer::render()` が `cmd_buf.drawIndexed(run.index_count, 1, run.first_index, run.vertex_offset, 0)` を発行します。各チャンクの uint16 index は 0 から再開するため、同じ batch がチャンクをまたいでも `vertex_offset` が変わる位置で必ず run を分けます。

### drawIndirect の stride は構造体より大きくてよい

`vkCmdDrawIndexedIndirect` の `stride` は「連続する draw パラメータ間のバイト間隔」で、仕様の要件は `drawCount > 1` のとき「4 の倍数、かつ `sizeof(VkDrawIndexedIndirectCommand)` 以上」であることだけです。ちょうど `sizeof(VkDrawIndexedIndirectCommand)` である必要はありません。そのため、コマンド構造体を先頭メンバに持つ自前の構造体を配列にして、`stride` にその `sizeof` を渡す、というイディオムが使えます。GPU は各要素の先頭 20 バイトだけを読み、残りの CPU 側メタデータ(マテリアル ID など)は無視されます。`offset` の方も 4 の倍数である必要があり、またバッファは `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` 付きで作られていなければなりません。

**このリポジトリでは**: `src/core/renderer/drawqueuebuilder.hpp` の `struct RenderCommand` が `vk::DrawIndexedIndirectCommand command;` を先頭メンバに置き、`DrawQueueBuilder` が `DrawIndirectInfo{... .offset = segment.first_command * sizeof(RenderCommand), .stride = sizeof(RenderCommand)}` を組み立てます。消費側は `src/core/renderer/materialrender.cpp` の `cmd_buf.drawIndexedIndirect(indirect_buf.buffer.get(), draw_call.offset, draw_call.draw_count, draw_call.stride)`。バッファ生成は `polygoninstancecontainer.cpp` の `createIndirectBuf()` で `eIndirectBuffer` を付けています。

### multiDrawIndirect、drawIndirectFirstInstance、shaderDrawParameters(gl_BaseInstance)

`vkCmdDrawIndexedIndirect` の `drawCount` に 2 以上を渡すには、`VkPhysicalDeviceFeatures::multiDrawIndirect` が有効化されている必要があります(無効なら `drawCount` は 0 か 1 に限られます)。さらに `VkPhysicalDeviceLimits::maxDrawIndirectCount` が上限です。indirect command の `firstInstance` を 0 以外にするには `VkPhysicalDeviceFeatures::drawIndirectFirstInstance` が必要です。GLSL 側で `gl_BaseInstance` / `gl_BaseVertex` / `gl_DrawID` を読むのはさらに別の機能で、Vulkan 1.1 の `shaderDrawParameters`(元は `VK_KHR_shader_draw_parameters`)が要ります。feature は「物理デバイスが対応しているか」を照会したうえで、`vkCreateDevice` の `pEnabledFeatures` / `VkPhysicalDeviceFeatures2` で明示的に有効化しないと使えません(照会しただけでは有効になりません)。

**このリポジトリでは**: `src/core/vkcore/core.cpp` が上記 3 feature をまとめて照会し、device 選択時に不足を拒否したうえで `createLogicalDevice()` の `vk::StructureChain` にすべて載せます。`PolygonInstanceContainer::triggerUpdate()` は device の `maxDrawIndirectCount` だけを `DrawQueueBuilder` へ渡し、builder が同じ material の連続範囲を上限以下へ分割します。読み出し側は `src/core/resources/default.vert` の `pelicanObjects.objects[gl_BaseInstance].model` です。

### bufferRowLength / bufferImageHeight の 0 は「詰めて配置」

`VkBufferImageCopy` の `bufferRowLength` と `bufferImageHeight` は、バッファ側のメモリを「もっと大きな 2D/3D イメージの部分領域」として扱うためのテクセル単位の指定です。仕様では「どちらかが 0 のとき、その軸については `imageExtent` に従って隙間なく詰まっている(tightly packed)とみなす」と定められています。つまり単純に「画像全体を幅×高さ×バイト数ぶんのバッファへそのまま吸い出す」場合は、両方 0 にしておくのが正しく、幅を入れる必要はありません。ここを実際の幅で埋めても同じ結果になりますが、パディング付きの行ピッチを扱うとき以外は 0 が定型です。`imageSubresource` の `aspectMask` / `mipLevel` / `layerCount` は別途明示が必要で、省略できません。

**このリポジトリでは**: `src/core/vkcore/offscreenframetarget.cpp` の `OffscreenFrameTarget::readbackLastFrameRGBA8()` が `copy_region.bufferRowLength = 0; copy_region.bufferImageHeight = 0;` としたうえで、`extent.width * extent.height * 4` バイトのステージングバッファへコピーします。

## 10.2 glTF / 3D アセット形式の仕様

### GLB のコンテナ構造(ヘッダ + JSON チャンク + BIN チャンク)

`.glb` は glTF 2.0 仕様が定めるバイナリコンテナで、先頭 12 バイトのヘッダ(magic / version / 全体長、いずれも little-endian の uint32)に続いて、可変個のチャンクが並びます。チャンクは 8 バイトのチャンクヘッダ(chunkLength / chunkType)+ チャンク本体という形で、magic は `0x46546C67`(ASCII の `glTF`)、JSON チャンクの型は `0x4E4F534A`(`JSON`)、BIN チャンクの型は `0x004E4942`(`BIN\0`)です。仕様上、**最初のチャンクは必ず JSON チャンク**でなければならず、BIN チャンクは 0 個または 1 個の任意です。したがって JSON 本体は常にファイル先頭から 20 バイト目(= 12 + 8)から始まり、この定数を直接書いてしまう実装がよくあります。各チャンクは 4 バイト境界にパディングされ、JSON チャンクは末尾を空白 `0x20`、BIN チャンクは `0x00` で埋める決まりで、chunkLength はそのパディングを含みます。BIN チャンクがある場合、JSON 側の buffer[0] は `uri` を持たず、この BIN チャンクの中身を指します。

**このリポジトリでは**: `src/devcli/distconfig.cpp` の `glb_magic` / `glb_json_chunk` 定数(35〜37 行付近)と `readGlbJsonChunk()` が、tinygltf を通さず生バイトから JSON チャンクだけを取り出しています(オフセット 20 の直書き)。

### buffer / bufferView / accessor の三層と byteStride

glTF の頂点データは 3 層に分かれています。`buffer` が生バイト列(GLB なら BIN チャンク)、`bufferView` がその中の連続した一区画(`byteOffset` / `byteLength` / 任意の `byteStride`)、`accessor` が bufferView に型を与えた読み取り口(`componentType` / `type` / `count` / bufferView 先頭からの相対 `byteOffset`)です。要素 i の先頭バイト位置は `bufferView.byteOffset + accessor.byteOffset + i * stride` で、**stride は `bufferView.byteStride` が定義されていればその値、未定義なら要素サイズ(= componentType のサイズ × type の成分数)**という規則になります。`byteStride` は頂点属性用の bufferView にのみ許され、インデックス用には置けないため、インターリーブ配置か否かは bufferView 側だけを見れば分かります。また accessor の offset は componentType のサイズの倍数でなければならない、という整列要件があるので、任意オフセットからの `reinterpret_cast` が仕様上は安全になっています。tinygltf の `Accessor::ByteStride(bufferView)` はこの「未定義なら密詰め」の場合分けをまとめた関数です。

**このリポジトリでは**: `src/core/model/gltf.cpp` の `getDataFromAccessor()`(600 行付近)が accessor → bufferView → buffer をこの順に辿り、`accessor.ByteStride(buffer_view)` を stride として `readComponentByType()` に渡しています。`src/devcli/distconfig.cpp` の `parseVatBufferViews()` は同じ構造を生 JSON から読み直しています。

### sparse accessor

`accessor.sparse` は「大半の要素が同じ値で、一部だけ違う」データを圧縮して持つための仕組みです。`sparse.count` 個の差分があり、`sparse.indices`(bufferView + byteOffset + componentType)が上書きする要素番号を、`sparse.values`(bufferView + byteOffset)がその値を持ちます。ベースになる値は accessor 本体の `bufferView` から読み、**accessor に `bufferView` が無い場合はすべてゼロ**という扱いになるのがポイントで、「bufferView が無い accessor」は壊れているのではなく sparse 専用の正当な形です。indices の componentType は UNSIGNED_BYTE / UNSIGNED_SHORT / UNSIGNED_INT のいずれかで、値は狭義単調増加でなければなりません。用途としてはモーフターゲット(数万頂点のうち動くのは顔の一部だけ)が典型です。読み込み側から見ると通常の密なパスとは別のコードパスが要るので、対応せずエラーにする実装は珍しくありません。

**このリポジトリでは**: `src/core/model/gltf.cpp` の `readMorphDeltaAccessor()`(714 行付近)が `accessor.sparse.isSparse` を見て morph target v1 では非対応として throw し、`src/core/loader/vrmadecoder.cpp` の `readFloatAccessor()`(330 行付近)も VRMA-C0 で同様に弾いています。

### normalized 整数属性を float へ戻す規則(UNORM / SNORM)

`accessor.normalized` が true のとき、格納された整数は 0..1 または -1..1 に写像された固定小数点値として解釈されます。glTF 2.0 仕様が定める復号式は、unsigned byte が `f = c / 255.0`、unsigned short が `f = c / 65535.0`、signed byte が `f = max(c / 127.0, -1.0)`、signed short が `f = max(c / 32767.0, -1.0)` です。signed 側で `max(..., -1.0)` が付くのは、-128/127 や -32768/32767 が -1 をわずかに下回るためで、これは OpenGL / Vulkan の SNORM 変換と同じ扱いです。除数が 128 や 32768 ではなく 127 / 32767 なのは、0 と ±1 の両方を正確に表せるようにするためです。`normalized` は整数 componentType にのみ指定でき、FLOAT や 32bit INT、インデックス用 accessor には指定できません。したがって「整数のまま読んで static_cast する」実装は、normalized 属性が来た瞬間に 255 倍ずれた値を出すことになります。

**このリポジトリでは**: `src/core/model/gltf.cpp` の `getDataFromAccessor()` は `accessor.normalized` を参照せず素の整数値を静的キャストしているため、正規化属性は想定外です。代わりに `readMorphDeltaAccessor()`(706〜710 行)と `src/core/loader/vrmadecoder.cpp` の `readFloatAccessor()`(327〜329 行)が「非 normalized の FLOAT であること」を明示的に要求して弾いています。

### node.weights と mesh.weights の優先順位

モーフターゲットの初期ウェイトは 2 か所に書けます。`mesh.weights` はそのメッシュ自身の既定値、`node.weights` はそのノードがメッシュを実体化するときの上書き値です。glTF 2.0 仕様では **node.weights があればそちらが mesh.weights に優先し、どちらも無ければ全ターゲットのウェイトは 0** と決まっています。同じメッシュを複数ノードから参照して、ノードごとに違う表情を初期値として与えられるのがこの二段構えの理由です。配列長はどちらもプリミティブの `targets` の個数と一致していなければならず、一致しない場合は不正なアセットです。実装側では「node 優先 → 無ければ mesh → 無ければ 0」という 3 段のフォールバックを書くことになります。

**このリポジトリでは**: `src/core/model/gltf.cpp` の `appendMorphDefaults()`(772〜786 行)が `source = &mesh.weights` を既定にして、`node.weights` が空でなければそちらへ差し替え、最終的に空なら `0.0` を積む、という順で実装しています。長さ検証は `meshMorphTargetCount()`(753 行付近)と `appendMorphDefaults()` の両方にあります。

### primitive.mode(トポロジ)と TRIANGLES 以外を扱わない実装が多い理由

`primitive.mode` は OpenGL の描画モード定数をそのまま使っていて、0=POINTS, 1=LINES, 2=LINE_LOOP, 3=LINE_STRIP, 4=TRIANGLES, 5=TRIANGLE_STRIP, 6=TRIANGLE_FAN です。**省略時の既定値は 4(TRIANGLES)**なので、多くのアセットには `mode` フィールドがそもそも現れません(tinygltf は未指定を -1 で表し、利用側で 4 に読み替える必要があります)。TRIANGLES 以外を切り捨てる実装が多いのは、スキニング・法線/接線の生成・三角形分割を前提にしたカリングや当たり判定など、パイプラインの下流がほぼ全部「3 頂点で 1 面」を前提にしているからです。加えて LINE_LOOP には対応するグラフィックス API のトポロジが無く、TRIANGLE_FAN は Vulkan のポータビリティサブセット(`VkPhysicalDevicePortabilitySubsetFeaturesKHR::triangleFans`)ではオプション機能で、Metal 系の環境では使えません。実務上もエクスポータはほぼ TRIANGLES しか吐かないため、対応コストに見合わないという判断になります。

**このリポジトリでは**: `src/core/model/gltf.cpp` の 1574〜1575 行で `primitive.mode < 0 ? TINYGLTF_MODE_TRIANGLES : primitive.mode` と既定値を補い、`pelican.vat`(1579 行)と VRM firstPerson auto(1594 行)だけが TRIANGLES を明示的に要求します。パイプライン側の既定トポロジは `src/core/shader/pipelinefactory.hpp` の `topology = vk::PrimitiveTopology::eTriangleList` です。

### extensions / extensionsUsed / extensionsRequired と extras

glTF は「コア仕様 + 拡張」で成り立っていて、拡張データは各 JSON オブジェクトの `extensions` メンバの下に、拡張名をキーとして入ります。ルートの `extensionsUsed` には使用している拡張名をすべて列挙する義務があり、`extensionsRequired` はそのうち「これを理解できないならロードを諦めてほしい」ものだけを列挙します。裏を返すと、**`extensionsRequired` に入っていない拡張はローダが黙って無視してよい**(無視しても表示は成立する)という設計です。`KHR_materials_emissive_strength` はその典型で、コアの `emissiveFactor` が [0,1] に制限されていて HDR な発光を表現できない問題に対し、スカラー `emissiveStrength` を掛ける形で拡張します。無視すれば強度 1 倍として妥当に描け、理解すればより正しく描ける、という後方互換の作りになっています。なお `extensions` とは別に `extras` があり、こちらは登録も命名規約も無いアプリ固有の自由領域です。

**このリポジトリでは**: `src/core/model/gltf.cpp` が `material.extensions.find("KHR_materials_emissive_strength")`(1303 行と 1372 行)と `KHR_texture_transform`(1352 行)を見つかったときだけ読み、見つからなければ既定値を使います。アプリ固有データ側は `primitive.extras` 経由の `pelican.vat` で、`tinyGltfValueToVatMeta()`(125 行)が扱います。

### 行列から TRS を分解できない場合(skew / perspective 成分)

glTF のノード変換は `matrix`(列優先の 16 要素)か `translation` / `rotation` / `scale` のどちらか一方で書きます。仕様は matrix について「TRS に分解可能でなければならない」と定めていて、せん断(skew)や射影(perspective)成分を含む行列は不正です。またアニメーションの対象になるノードでは `matrix` を使ってはならず、TRS で書く必要があります。実装側で分解が必要になるのは、内部表現を TRS に統一する場合や、アニメーションと合成する場合です。定番の実装は `glm::decompose()` で、これは translation / rotation / scale のほかに **skew と perspective も出力引数で返す**ので、それらがゼロ(perspective は (0,0,0,1))かどうかを見れば「分解できたか」ではなく「TRS として表現しきれているか」を判定できます。関数の戻り値 false は特異行列などの失敗で、これとは別のチェックが要ります。併せて、glTF の rotation は `(x, y, z, w)` 順の配列なのに対し `glm::quat` のコンストラクタは `(w, x, y, z)` 順である点も定番の罠です。

**このリポジトリでは**: `src/devcli/gltfsceneextract.cpp` の `nodeTrs()`(102〜122 行)が `glm::decompose()` の戻り値と `skew` / `perspective` の両方を検査し、`epsilon = 1.0e-9` を超えたら「scene v1 の TRS では表現できない」として throw します。同ファイル 135 行と `src/core/model/gltf.cpp` 822〜827 行に (x,y,z,w) → (w,x,y,z) の詰め替えがあります。

### VRM / VRMA が glTF 拡張として乗っていること

`.vrm` と `.vrma` はどちらも独自フォーマットではなく、**中身は普通の GLB** です。VRM 1.0 はルートの `extensions` に `VRMC_vrm` を置き、humanoid のボーン割り当て・表情(expressions)・視線(lookAt)・一人称表示(firstPerson)といった「人型としての意味付け」を持ちます。関連拡張として `VRMC_springBone`、`VRMC_node_constraint`、`VRMC_materials_mtoon` などが別々の拡張名で並びます。VRM 0.x は互換性のないスキーマを `VRM` という別キーで持つため、両者は別物として分岐する必要があります。VRMA(VRM Animation)は `VRMC_vrm_animation` 拡張で、glTF の `animations` 自体は普通のノードアニメーションのまま、拡張側が「どのノードがどの humanoid ボーン/表情/視線に対応するか」の対応表だけを与えます。つまりリターゲットに必要な意味情報が拡張側、数値データはコア側、という分担です。

**このリポジトリでは**: `src/core/model/vrmsemantic.cpp` の `decodeVrmSemantic()`(481 行〜)が `VRMC_vrm` を探し、無ければ `VRM`(0.x)の存在を info 診断として報告して素の glTF 扱いにします。`VRMC_node_constraint` はノードごとの `extensions` から読みます(352〜362 行)。VRMA 側は `src/core/loader/vrmadecoder.cpp` の 579〜592 行で `extensionsUsed` への `VRMC_vrm_animation` 記載とルート拡張・`specVersion == "1.0"` を要求しています。

## 10.3 C++ とビルドのイディオム

### RAII ハンドルラッパ(コピー禁止・ムーブのみ)

OS のハンドル(Win32 の `HANDLE`、POSIX の fd)を確実に閉じるための定番は、所有権を持つ小さなクラスで包み、デストラクタで解放する形です。コピーすると同じハンドルを二度閉じてしまうため、コピーコンストラクタとコピー代入を `= delete` し、ムーブのみを許す「ムーブ専用型」にします。ムーブは「相手から `release()` で所有権を奪い、自分の古い値を `reset()` で閉じる」の 2 操作で表現するのが標準的で、`release()` は閉じずに手放す、`reset(x)` は今持っている物を閉じてから `x` を引き取る、という役割分担になっています。`operator bool` を `explicit` にするのは、`if (h)` は書けるが `int n = h;` のような意図しない暗黙変換は起きないようにするためです(C++11 の文脈依存変換)。`std::unique_ptr` にカスタム deleter を付ける手もありますが、Win32 は失敗値が `NULL` の API と `INVALID_HANDLE_VALUE` の API の 2 系統あるため、両方を無効と扱う手書きラッパが好まれます。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の `UniqueHandle`(`_WIN32` ブロック内)。`reset()` が `nullptr` と `INVALID_HANDLE_VALUE` の両方を「閉じない」と判定しています。

### pimpl と「デストラクタは .cpp で定義する」規則

`std::unique_ptr<Impl>` を持ち、`Impl` の定義をヘッダから隠す pimpl では、デストラクタをヘッダで暗黙生成させてはいけません。`std::default_delete<Impl>::operator()` は `sizeof(Impl) > 0` を静的表明しており、不完全型のまま実体化するとコンパイルエラーか未定義動作になります。そのためヘッダでは `~T();` と宣言だけし、`Impl` が完全型になった .cpp 側で `T::~T() = default;` と定義するのが定石です。さらに、デストラクタを自前で宣言するとムーブコンストラクタ・ムーブ代入の暗黙生成が抑止される(rule of five)ので、それらもヘッダで宣言し .cpp で `= default` する必要があります。ムーブ代入は古い pimpl を破棄するため、これも完全型が必要です。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.hpp` の `StagedModelInstance`(`struct Impl;` 前方宣言と 4 つの特殊メンバ宣言)と、`src/core/renderer/polygoninstancecontainer.cpp` の `StagedModelInstance::Impl` 定義直後に並ぶ `= default` 群。

### CRTP(奇妙に再帰したテンプレートパターン)による強い型付け

`struct Derived : Base<Derived>` のように、自分自身をテンプレート引数として基底に渡す書き方を CRTP(Curiously Recurring Template Pattern)と呼びます。基底が実体化される時点で `Derived` はまだ不完全型ですが、メンバ関数の本体とシグネチャは「使われたときに」実体化されるため、基底の中で `Derived` を型として書けます。これに空の派生クラスを組み合わせると、内部表現は同じ整数でも型としては別物になる「強い typedef(phantom type)」が作れ、別種のハンドル同士の取り違えをコンパイルエラーにできます。ハッシュ関数オブジェクトを基底の入れ子 `struct Hash` として持たせておくと、`std::unordered_map<H, V, H::Hash>` のように派生型ごとに使い回せます。

**このリポジトリでは**: `src/core/handle.hpp` の `BasicHandle<T, Base>` と、それを空派生させるマクロ `PELICAN_DEFINE_HANDLE`(`SoundHandle` がその適用例)。

### 世代付きハンドル(index + generation)と SlotMap

配列の添字をそのままハンドルとして外に配ると、スロットが解放されて別のオブジェクトに再利用されたとき、古いハンドルが新しい住人を指してしまいます(ABA 問題)。定番の解法が「添字 + 世代番号」の組をハンドルにする方式で、スロットが死ぬたびに世代を +1 し、参照時に `generations[index] == handle.generation` を確認します。これで有効性判定が O(1) の整数比較 2 回で済み、解放済みハンドルは必ず弾かれます。空きスロットはフリーリスト(空き添字のスタック)で管理し、再利用時も添字は据え置き・世代だけ進めます。この構造はコミュニティでは slot map / generational index / generational arena と呼ばれ、EnTT のエンティティ ID(index + version)や Rust の `slotmap` クレートが同じ考え方です。世代 0 を「無効」に予約しておくと、ゼロ初期化されたハンドルが自動的に無効になります。

**このリポジトリでは**: `src/core/renderer/modelinstance.hpp` の `ModelInstanceId{index, generation, scene_epoch}` と、`src/core/renderer/modelinstanceslots.hpp/.cpp` の `ModelInstanceSlots`(`generations_` / `alive_` / `free_indices_`、`isLive()`・`retire()`)。

### 単調増加する generation / epoch を「変わったか」の検出に使う

データ本体を突き合わせずに「前回見たときから変わったか」を判定したいとき、変更のたびに +1 される単調増加カウンタ(revision / generation / epoch、論理時計)を横に持たせるのが定番です。等値比較だけで陳腐化を検出でき、大小比較を使えば「古い更新の取りこぼし受理」や「同じ更新の二重適用」も拒否できます(受け取った revision が保持中の値以下なら重複とみなす)。粒度を変えた複数のカウンタを併用するのも普通で、「スロット単位」「レイアウト単位」「シーン全体」で別々に持てば、無効化の影響範囲を必要最小限にできます。有限幅なので折り返しへの備えが要り、0 を無効値に予約しているなら折り返し時に 0 を飛ばす、あるいは上限に達したスロットは再利用をやめる、といった扱いをします。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `animation_generations`(`resetSlot()` の `if (++animation_generations[index] == 0) ++animation_generations[index];`)と `frame_revision` による `duplicate_revision` 判定、`src/core/renderer/modelinstanceslots.cpp` の `scene_epoch_`(`clearPrepared()`)。

### std::erase_if(erase-remove イディオムの置き換え)

C++20 で入った非メンバ関数 `std::erase_if(container, pred)` は、条件に合う要素を全部消して消した個数を返します。従来のシーケンスコンテナでは `v.erase(std::remove_if(v.begin(), v.end(), pred), v.end())` と 2 段階で書く必要があり(erase-remove イディオム)、`erase` を忘れると要素が残ったままサイズだけ変わらないという典型バグを踏みました。連想コンテナでは remove_if がそもそも使えない(`std::pair<const Key, T>` は代入できない)ため、以前はイテレータを回しながら `it = m.erase(it)` と書く必要があり、こちらも `erase_if` で 1 行になります。オーバーロードはコンテナごとに各ヘッダ(`<vector>` `<map>` `<unordered_map>` など)で定義されるので、使う側は該当ヘッダを include しておく必要があります。連想コンテナに渡す述語は `value_type`、つまりキーと値のペアを受け取るため、`entry.first` でキーを見る形になります。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `removeModelInstance()`(`std::vector<RenderCommand>` から削除)と `resetSlot()`(`unordered_map` から `entry.first.instance_identity` で削除)、`src/project/assetsmanifest.cpp` の `generateAssetsManifest()`(`std::map` のキャッシュ掃除)。

### std::tie による多段キーの辞書式比較

複数のメンバを優先順位付きで比較する比較関数は、素直に書くと `if (a.x != b.x) return a.x < b.x; ...` の連鎖になり、条件を 1 つ書き間違えるとソート順が壊れます。`std::tie(a, b, c)` は左辺値参照のタプルを作るだけの関数で、`std::tuple` の比較演算子が先頭から辞書式に比べてくれるため、`std::tie(p.a, p.b) < std::tie(q.a, q.b)` と書けば多段キー比較が 1 行で正しく書けます(コピーは発生しません)。`std::sort` に渡す比較子は strict weak ordering である必要があり、辞書式比較はこれを自動的に満たす点も利点です。ヘッダは `<tuple>` です。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `triggerUpdate()` 内のソート述語(material / source_material_index / skinned / view_visibility の 4 段)と、`uploadMaterialAbsoluteOverrides()` の 2 段比較。

### C++20 の指示付き初期化子(designated initializers)

`T{.a = 1, .c = 3}` のようにメンバ名を書いて集成体を初期化する構文で、C++20 で採り入れられました(C99 から来ていますが制約が強められています)。C++ では **宣言順に書かなければならず**、順序を入れ替えるとコンパイルエラーになります。位置指定との混在、`.a.b =` のような入れ子指定、配列添字の指定も許されません。省略したメンバは、既定メンバ初期化子があればそれ、なければ値初期化になるので、「書かなかったフィールドはゼロ相当」と読んで構いません。適用対象は集成体(ユーザ定義コンストラクタや private 非静的メンバを持たない型)だけです。読む側にとっての利点は、フィールドが 10 個ある構造体の初期化で「何番目の引数が何か」を数えなくて済むことです。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `MorphWeightFrame{...}` 構築、`RenderCommand{.command = ..., .material = ...}`、`DrawIndirectInfo{...}` など多数。

### デフォルト化された operator<=>(三方比較)

C++20 の `auto operator<=>(const T &) const = default;` は、宣言順にメンバを辞書式比較する三方比較演算子を生成します。戻り値を `auto` にすると全メンバの比較カテゴリの共通型が推論され、整数メンバだけなら `std::strong_ordering` になります。重要なのは、`<=>` をデフォルト定義すると `operator==` も暗黙にデフォルト宣言されることと、`<` `>` `<=` `>=` は `(a <=> b) < 0` 等への書き換えで、`!=` は `==` の否定として自動的に使えるようになることです。つまりこの 1 行で 6 種類の比較演算子が揃います。`<compare>` のインクルードが必要です(忘れると比較カテゴリ型が未定義になります)。

**このリポジトリでは**: `src/core/renderer/modelinstance.hpp` の `ModelInstanceId`(`auto operator<=>(const ModelInstanceId &) const = default;` と `#include <compare>`)。

### std::vector<bool> の特殊化

`std::vector<bool>` は標準が規定する部分特殊化で、実装は 1 要素 1 ビットに詰めてよいことになっています。その代償として `operator[]` が返すのは `bool&` ではなくプロキシ型 `std::vector<bool>::reference` で、`bool *` を返す `data()` は存在せず、範囲 for で `for (auto &b : v)` と書くとコンパイルエラーになります(`auto` か `auto &&` なら通ります)。個々の要素にアドレスが無いため、他のコンテナと同じ「要素への参照・ポインタが取れる」前提が崩れる、というのが有名な落とし穴です。一方で、添字代入・`push_back`・`std::fill` はプロキシ経由で普通に動くので、ビット列として使う分には素直に使えます。要素のアドレスが必要なときは `std::vector<char>` や `std::deque<bool>`、固定長なら `std::bitset` に替えるのが定石です。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.hpp` の `model_history_valid` / `morph_history_valid`(`std::fill` と添字代入のみで使用)、`src/core/renderer/modelinstanceslots.hpp` の `alive_`。

### std::filesystem の error_code オーバーロード(投げない版)

`<filesystem>` のほとんどの関数には 2 つのオーバーロードがあり、片方は失敗時に `std::filesystem_error` を投げ、もう片方は末尾の `std::error_code &` に結果を書いて **投げません**(こちらは `noexcept`)。`error_code` 版は成功時に `ec.clear()` するので、同じ `ec` を使い回すときは前回の値が残らない一方、自分で `clear()` して再利用しているコードもあります。`is_directory` / `is_regular_file` のような述語は、エラー時に `ec` を設定した上で `false` を返すので、失敗と「存在しない」を区別したければ `ec` を見る必要があります。ディレクトリ走査も同様で、`recursive_directory_iterator` はコンストラクタと `increment(ec)` に error_code 版があり、権限の無い 1 ディレクトリで走査全体を落とさずに続行できます。`last_write_time` が返す `file_time_type` のエポックは実装定義なので、`time_since_epoch().count()` は「変化検出用の不透明なトークン」としてのみ使うのが安全です。

**このリポジトリでは**: `src/project/assetsmanifest.cpp` の `scanStore()`(`is_directory(store_root, ec)`、`it.increment(ec)`、`entry.file_size(entry_ec)`)と `fileMtime()`(mtime を文字列化してキャッシュキーに使用)。

### プラットフォーム分岐の #ifdef で、同名関数を 2 回定義する

`#ifdef _WIN32 ... #else ... #endif` で挟んで同じ名前・同じシグネチャの関数を 2 回書くのは、プラットフォーム抽象の最も素朴な形です。プリプロセスの時点で片方しか残らないので、これは多重定義でもオーバーロードでもありません。呼び出し側は 1 種類の宣言だけを見ればよく、分岐はこのファイルの中で閉じます。読むときの注意は、`#else` と `#endif` が数百行離れることがあり、いま読んでいる行がどちらの枝かを見失いやすいこと、そして clangd や IDE は通常「有効な枝」しか索引化しないため、非有効側の定義には定義ジャンプで飛べないことです。判定マクロについては、MSVC / MinGW とも 32bit・64bit の両方の Windows ターゲットで `_WIN32` が定義され、64bit ではこれに加えて `_WIN64` も定義される、という関係を覚えておくと迷いません。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の `readPipe()` と `runPlatformProcess()` が `#ifdef _WIN32` / `#else` で 2 回ずつ定義され、`runProcess()` は分岐なしで `runPlatformProcess()` を呼ぶだけになっています。

### windows.h を include する前の NOMINMAX

`<windows.h>`(実体は `minwindef.h`)は `min` と `max` を関数形式マクロとして定義します。これが `std::min` / `std::max` はもちろん `std::numeric_limits<T>::max()` まで壊すため(`max()` がマクロ展開されて引数の数が合わなくなる)、include より前に `NOMINMAX` を定義してマクロ定義自体を抑止するのが定番の対処です。順序が意味を持つので、`#define NOMINMAX` は必ず `#include <windows.h>` より上に書きます。同じ目的で `WIN32_LEAN_AND_MEAN` を定義して取り込むヘッダを減らすこともよく併用されます。ビルドシステム側で `-DNOMINMAX` を渡している環境と衝突しないよう、`#ifndef NOMINMAX` で囲む書き方も一般的です(同一内容の再定義は本来無害ですが、警告を避ける意図です)。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の `#define NOMINMAX` → `#include <windows.h>`。`#ifndef` で囲む版は `src/core/loader/imageloader.cpp` や `src/core/os/window.cpp` にあります。

### 機能フラグをマクロ「値」で渡し、#ifdef ではなく #if で見る

ビルドオプションでサブシステムを丸ごと外す構成では、マクロを「定義する/しない」ではなく「0 か 1 の値として常に定義する」流儀があります。CMake の `target_compile_definitions(tgt PUBLIC FOO=$<BOOL:${FOO}>)` はまさにこれで、OFF のときも `FOO=0` が渡ります。したがってソース側は `#ifdef FOO` では **常に真になってしまい**、必ず `#if FOO` と書かなければなりません。この流儀の利点は、名前の綴り間違いを `-Wundef` で検出できることと、定義漏れと OFF を区別できることです。もう一点の勘所は `PUBLIC` と `PRIVATE` の使い分けで、フラグがヘッダの中身(宣言されるメンバや関数)を変える場合、利用側にも同じ値が伝わらないと ODR 違反や ABI 不一致になるため `PUBLIC` にする必要があります。逆に .cpp の中だけで効くフラグは `PRIVATE` で十分です。

**このリポジトリでは**: `src/core/CMakeLists.txt` の `target_compile_definitions(pelican_core PUBLIC PELICAN_WITH_AUDIO=$<BOOL:...> ...)` と、`PELICAN_ENGINE_BUILD=1` などの `PRIVATE` 側。参照側は `src/core/appflow/loop.cpp` や `src/core/persistence/persistence.hpp` の `#if PELICAN_WITH_AUDIO` / `#if PELICAN_WITH_PHYSICS`。

## 10.4 OS・プロセス・ファイルの作法

### 実行中の DLL は Windows では上書きできない(シャドウコピー)

Windows は、イメージセクションとしてマップされた実行ファイル/DLL のファイルを書き込み・削除から保護します。`LoadLibrary` が成功している間、そのファイルへの書き込みオープンは `ERROR_SHARING_VIOLATION`、削除は `ERROR_ACCESS_DENIED` で拒否されます。したがってエンジンが `game_logic.dll` を直接ロードしたままだと、ビルドツール側のリンクが「出力ファイルを開けない」で失敗します。POSIX にはこの制約がなく、`dlopen` 中のファイルを普通に置き換えられます(既存のマッピングが古い inode を保持し続けるだけ)ので、この回避策は Windows 由来です。定番の対処が **シャドウコピー**、つまりビルド成果物をロード用の別ディレクトリへコピーし、コピーの方をロードして原本には触らないやり方です。なお名前の変更(rename)だけは実行中でも許されるため、「リネームで退避してから上書き」という変種もあります。MSVC の `.pdb` も、デバッガがアタッチしていると同様にロックされるため、シンボルを見せたいならコピーと一緒に運ぶ必要があります。

**このリポジトリでは**: `src/core/gamelogic/gamelogicreload.cpp` の `GameLogicReloader::makeShadowCopy()` が `source_path.parent_path() / ".pelican-hot-reload"` へコピーし、同関数の後半で `.pdb` も同名でコピーします。ロードは常にコピー側(`initialize()` / `reloadTransaction()` が渡す `candidate_path`)に対して行われます。

### 同じパスの LoadLibrary / dlopen は参照カウントされる

`LoadLibraryW` は「まだロードされていなければマップし、すでにロード済みなら参照カウントを 1 増やして同じ `HMODULE` を返す」という仕様です。`dlopen` も POSIX 上で同様で、同一のオブジェクトファイルを指す呼び出しには同じハンドルが返り、`dlclose` の回数が釣り合うまでアンロードされません。つまり「新旧 2 世代の DLL を同時にロードして中身を比べる」ことは、**同じパスのままでは原理的にできません**。別々のマッピングが欲しければファイルの実体を別パスに分ける必要があり、リロード機構がコピー名に連番や世代番号を含めるのはこのためです。逆に、アンロードしたつもりでも参照カウントが残っていればコードは生きたままで、古い関数ポインタがまだ呼べてしまう点にも注意が要ります。

**このリポジトリでは**: `src/core/gamelogic/gamelogicreload.cpp` の `makeShadowCopy(const char *purpose)` がコピー名に `purpose`(`"initial"` / `"candidate"`)と `++copy_sequence` を埋め込みます。`validateCandidate()` は現行 DLL がロードされたまま候補をロードして `internal::validateBehaviorReload()` に掛けるので、この別名が前提です。

### POSIX のプロセスグループと setpgid、kill(-pid) で子孫ごと止める

POSIX のプロセスは必ずいずれかの **プロセスグループ** に属し、`setpgid(pid, pgid)` でグループを移せます。`setpgid(0, 0)` は「自分自身を、自分の PID を ID とする新しいグループのリーダーにする」という意味です。`kill(2)` は `pid` が負(かつ `-1` でない)のとき、`|pid|` をグループ ID とするグループの全プロセスへシグナルを送るので、`kill(-child_pid, SIGTERM)` は子とその子孫(グループを抜けていない限り)をまとめて止められます。`-1` だけは「送信可能な全プロセス」を意味する特別値なので、PID を符号反転して渡すコードでは PID 1 を踏まないことが暗黙の前提です。`setpgid` を親子の**両方**で呼ぶのは定番のイディオムで、これは競合の回避策です — 子が `exec` を終えた後に親が呼ぶと `EACCES` で失敗し、親が先に kill しようとするとグループがまだ無い、という順序依存を、どちらが先でも成立するようにして消します。子の側の呼び出しは `fork` 直後・`exec` 前に置く必要があります。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の `runPlatformProcess()`(非 Windows 版)が子側で `::setpgid(0, 0)`、親側で `::setpgid(pid, pid)` を呼び、タイムアウト/キャンセル時に `::kill(-pid, SIGTERM)` → 猶予 100ms → `::kill(-pid, SIGKILL)` と段階的に落とします。

### Windows の Job オブジェクト(プロセスグループの対応物)

Windows には「プロセスツリーをまとめて殺す」API が素直な形では無く、`CREATE_NEW_PROCESS_GROUP` は名前に反して kill の単位ではありません — これは `GenerateConsoleCtrlEvent` による Ctrl+C / Ctrl+Break の配送単位を分けるだけのフラグです。ツリー単位の生殺与奪を担うのは **Job オブジェクト**で、`CreateJobObject` で作り `AssignProcessToJobObject` でプロセスを入れると、以後そのプロセスが作る子孫も自動的に同じ Job に入ります。`TerminateJobObject` で全員を、`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` を設定しておけば最後の Job ハンドルを閉じた時点でも全員を終了させられます。ここで `CREATE_SUSPENDED` で起動し、Job へ入れてから `ResumeThread` するのは必須のイディオムです。そうしないと、割り当て前の一瞬に子がさらに孫を起動して Job の外に取り逃がす競合が残ります。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の Windows 版 `runPlatformProcess()` が `CreateJobObjectW` → `SetInformationJobObject(JobObjectExtendedLimitInformation)` で `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` を立て、`CreateProcessW(..., CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, ...)` → `AssignProcessToJobObject` → `ResumeThread` の順に進めます。

### 子プロセスのパイプ読みとデッドロック回避

子プロセスの stdout/stderr をパイプで受け取るとき、古典的なデッドロックが 2 種類あります。ひとつは、親が「まず子の終了を待ってから読む」と書いた場合で、子の出力がパイプのカーネルバッファ(数十 KB 程度、`CreatePipe` に 0 を渡せばシステム既定)を超えると子は `write` でブロックし、親は終了を待ち続けて双方が止まります。回避策は、待機と読み出しを並行させること — スレッドを分けるか、POSIX なら `select`/`poll`、Windows なら OVERLAPPED I/O を使います。もうひとつは EOF の取りこぼしで、`read` が 0 を返す(`ReadFile` が失敗する)のは**書き込み端のハンドルが 1 つ残らず閉じられたとき**なので、親が自分の持つ書き込み端を閉じ忘れると読み出しスレッドは永久に返ってきません。さらに厄介なのは、子が終了しても**継承した書き込み端を握ったままの孫**が生きていると同じことが起きる点で、これがプロセスグループ/Job ごと殺す動機のひとつになります。Windows では `CreateProcess` を `bInheritHandles = TRUE` で呼ぶと継承可能なハンドルが一括で渡るため、読み出し端は `SetHandleInformation(..., HANDLE_FLAG_INHERIT, 0)` で明示的に継承対象から外すのが作法です。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の `readPipe()` を stdout/stderr それぞれ `std::thread` で回し、親側は起動直後に書き込み端を手放します(POSIX 版は `::close(stdout_pipe[1])`、Windows 版は `stdout_write.reset()`)。終了処理で `job.reset()` / `::kill(-pid, SIGKILL)` してから `join()` する順序も、孫が握るパイプを EOF に到達させるためです(同ファイル内のコメントに明記あり)。

### ファイル監視 API は取りこぼす前提で使う

`ReadDirectoryChangesW` は、変更イベントをカーネル側のバッファに溜めて 1 回の呼び出しに詰めて返します。バッファが溢れると **溜まっていた内容は全部捨てられ**、`lpBytesReturned` に 0 が入るか `ERROR_NOTIFY_ENUM_DIR` が返ります(MSDN に明記されています)。つまり「どのファイルが変わったか」は復元不能で、アプリ側にできるのはディレクトリを丸ごと列挙し直すことだけです。さらに、1 回の完了を受け取ってから次を再武装するまでの隙間で起きた変更も原理的に落ちますし、ネットワークドライブ越しでは通知自体が信用できない(バッファ長も 64KB 以下が要求される)ため、実装は通常ポーリングへ切り替えます。この「イベントはヒントであり真実ではない。真実は再スキャンで取り直す」という設計が、inotify(`IN_Q_OVERFLOW`)や FSEvents(`kFSEventStreamEventFlagMustScanSubDirs`)を含むファイル監視全般の作法です。なお `CreateFileW` でディレクトリのハンドルを開くには `FILE_FLAG_BACKUP_SEMANTICS` が要ります。

**このリポジトリでは**: `src/core/watch/filewatcher.cpp` の `NativeWatch::threadMain()` が `const bool overflow = (ok && bytes == 0) || error == ERROR_NOTIFY_ENUM_DIR;` で溢れを判定し、先に `arm()` で再武装してから `notify(overflow)` します。`Impl::armAll()` は `isNetworkStore()`(`GetDriveTypeW == DRIVE_REMOTE`)や武装失敗を検出して `WatcherState::polling` / `degraded` に落とし、いずれの経路も最終的に全件走査の `Impl::reconcile()` へ合流します。

### 一時ファイル + rename による原子的置換

保存先を直接開いて上書きすると、書き込みの途中でクラッシュ/電断が起きたときに「半分だけ新しい」ファイルが残ります。これを避ける定番が **write-to-temp + rename** です。同じディレクトリに一時ファイルを作って全部書き、flush/close で内容を確定させてから、`rename` で本来の名前に被せます。POSIX の `rename(2)` は、置換先が既に存在する場合でもリンクは他プロセスから常に可視で、古いファイルか新しいファイルのどちらかを指す(中間状態は観測されない)と規定しており、この不可分性がこのイディオムの根拠です。一時ファイルを**同じディレクトリに作る**のは、`rename` が別ファイルシステムをまたぐと `EXDEV` で失敗するからで、`/tmp` に置くと破綻します。厳密な耐障害性まで求めるなら、`rename` の原子性は「見える内容」の話であって「ディスクに届いたか」は別問題なので、ファイルと親ディレクトリの `fsync` が要ります。Windows での対応物は `MoveFileExW` に `MOVEFILE_REPLACE_EXISTING` を渡す形で、`MOVEFILE_WRITE_THROUGH` を併せると移動がディスクに反映されるまで関数が戻りません。

**このリポジトリでは**: `src/core/loader/basicconfig.cpp` の `temporaryScenePath()` が保存先の `parent_path()` に `.pelican-save-<nonce>-<seq>.tmp` を作り(同一ディレクトリなので `EXDEV` を踏みません)、`TemporarySceneFile` の RAII が失敗時に消します。`writeAndFlushSceneFile()` の後、`replaceSceneFileAtomically()` が Windows では `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`、それ以外では `std::filesystem::rename()` を呼びます。

## 10.5 アルゴリズムとデータ処理の定番

### Kahn 法のトポロジカルソートと、未知の依存先を無視する実装

Kahn 法は、入次数(その頂点へ入ってくる辺の数)が 0 の頂点を取り出しては、その頂点から出る辺を消して隣接頂点の入次数を減らす、を繰り返す線形時間のトポロジカルソートです。DFS ベースの実装と違って再帰スタックを使わず、また「取り出せた頂点の数が全頂点数に満たなければ閉路がある」という判定が副産物として得られるのが特徴です。閉路の中の頂点は入次数が永久に 0 にならないため、キューが空になった時点で結果列に載らずに残ります。よって `result.size() != vertices` という一行が、そのまま閉路検出になります。取り出し順が FIFO か LIFO かは結果の妥当性には影響せず、同じ入力に対する順序を安定させたいかどうかの選択です。

依存先を頂点表から引けなかったときに `continue` で読み飛ばす実装は、「この依存はソート対象グラフの外にある」= すでに満たされている前提として扱う、という意味になります。辺を張らないので入次数も増えず、その頂点は最初から ready 側に入ります。部分グラフだけを並べ替えるときの定石ですが、代償として「依存先の名前を書き間違えた」ケースが黙って通ることになります。

**このリポジトリでは**: `src/core/watch/reloadtransaction.cpp` の `topologicalOrder()`。`actors[i].after` の各要素を `actor_for` から引き、`if (found == actor_for.end()) continue;` で同一トランザクション外の依存を読み飛ばし、末尾の `result.size() != actors.size()` で閉路を例外にしています。

### CRC-32 の初期値 0xFFFFFFFF と最終反転(なぜ 0 から始めないか)

CRC は本質的にメッセージを多項式とみなした剰余計算なので、レジスタを 0 で初期化すると先頭のゼロバイトが値をまったく動かしません。つまり `00 00 M` と `M` が同じ CRC になり、先頭に 0 が何個あるかを検出できなくなります。初期値を 0xFFFFFFFF にするのはこの盲点をつぶすためで、レジスタに 1 が立った状態から始めることで先頭ゼロもレジスタを撹拌します。最後の反転(0xFFFFFFFF との XOR、C++ では `~crc`)は末尾側に対する同じ対策で、これを入れておくと「メッセージ+CRC」全体を再計算した残差が 0 ではなく固定の非ゼロ値になり、末尾にゼロバイトが付いた破損も検出できます。この 2 つを合わせて CRC-32/ISO-HDLC(zlib・PNG・Ethernet と同じ変種)と呼びます。

ビット反転版(LSB ファースト)では生成多項式 0x04C11DB7 をビット反転した 0xEDB88320 を使い、右シフトで回します。`0u - (crc & 1u)` は最下位ビットが 1 なら全ビット 1、0 なら 0 になるマスクで、分岐なしで「LSB が立っていたら多項式を XOR」を書くイディオムです。

**このリポジトリでは**: `src/core/vkcore/previewexecutor.cpp` の `crc32()`。`appendChunk()` から呼ばれ、PNG 仕様どおり長さフィールドを除いた「チャンクタイプ 4 バイト + データ」に対して計算しています(`subspan(start, 4 + data.size())`)。

### zlib / deflate の BFINAL ビットと「非圧縮(stored)ブロック」

deflate(RFC 1951)のストリームはブロックの連なりで、各ブロックの先頭に BFINAL(1 ビット、最終ブロックなら 1)と BTYPE(2 ビット)が来ます。BTYPE=00 は非圧縮ブロックで、この場合は残りのビットを捨ててバイト境界まで進め、続けて LEN(2 バイト、リトルエンディアン)と NLEN(LEN の 1 の補数)を置き、そのあとに生データを LEN バイト並べます。LEN が 16 ビットなので 1 ブロックあたり最大 65535 バイト、それを超える入力は複数ブロックに分割し、最後のブロックだけ BFINAL=1 にします。つまり BFINAL/BTYPE=00 の 3 ビットは 1 バイト目の下位 3 ビットに収まるので、非圧縮ブロックの先頭バイトは常に `0x01` か `0x00` になります。ハフマン符号器を書かずに「形式として正しい deflate ストリーム」を出す最短経路がこれです。

zlib ラッパ(RFC 1950)は deflate データの前に CMF/FLG の 2 バイト、後ろに Adler-32 を 4 バイト(ビッグエンディアン)付けます。`0x78 0x01` は CM=8(deflate)・CINFO=7(32KB 窓)で、FLG は `(CMF<<8|FLG) % 31 == 0` を満たすよう選ばれた値です。Adler-32 は s1 を 1、s2 を 0 から始めて 65521(2^16 未満の最大の素数)で剰余を取り、`(s2 << 16) | s1` を出力します。

**このリポジトリでは**: `src/core/vkcore/previewexecutor.cpp` の `zlibStored()`。`out.push_back(final ? 1 : 0)` が BFINAL+BTYPE のバイト、`nlen = ~len` が NLEN、末尾の `s1`/`s2` ループが Adler-32 です。`encodePng()` から IDAT の中身として使われます。

### デッドゾーンの取り方(成分ごと vs 半径)と、方向を保つ再スケール

アナログスティックは中立位置でも微小な値を返すので、しきい値以下を 0 に潰す処理(デッドゾーン)が要ります。1 軸(トリガーや単軸)なら絶対値でしきい値判定すれば十分ですが、2 軸のスティックに軸ごとの判定を掛けると、無効領域が円ではなく十字型になり、斜め入力のときに片方の成分だけが 0 に落ちて方向が軸に吸い寄せられます。そのため 2 軸では合成ベクトルの長さ(半径)で判定するのが定番で、これを radial deadzone と呼びます。

潰すだけだと今度はしきい値の外側で値が不連続に跳ねるので、残った範囲 `[deadzone, 1]` を `[0, 1]` に線形に引き伸ばします。`(magnitude - deadzone) / (1 - deadzone)` がその式です。再スケール後は元のベクトルを正規化してから新しい長さを掛け直すことで、方向を変えずに大きさだけを補正できます。スティックの生値は矩形にクリップされて返ることがあり斜めで長さが 1 を超えうるので、最後に 1 でクランプします。しきい値を `[0,1)` に制限しておくと `1 - deadzone` のゼロ除算を構造的に防げます。

**このリポジトリでは**: `src/core/os/actionmap.cpp`。1 軸版が `applyDeadzone()`(`std::copysign` で符号を戻す)、2 軸版が `readGamepadAxisBinding()` の `axis2` 分岐(`magnitude` で判定し `x / magnitude * scaled` で方向を保つ)。`[0,1)` の検証は `parseInputProfileJson()` にあります。

### 走査順(DFS)と表示順(ソートキー)を分けて考えること

シーングラフや UI ツリーを扱うコードでは、「木をどうたどるか」と「描画結果をどの順で並べるか」は別の関心事です。木の走査は親子関係(変換の継承、クリップ矩形の継承)を解くために必要なので素直に DFS の再帰で書き、その過程では描画コマンドを平坦な配列に push するだけにします。そのあとで配列全体を独立したソートキー(レイヤ、深度、パイプライン状態など)で並べ替えると、ステート変更をまとめてドローコールを減らせます。両者を混ぜて「走査しながら正しい順で出す」ようにすると、レイヤ指定が親子関係をまたぐたびに破綻します。

ここで効くのが安定ソートです。同じソートキーを持つ要素同士の相対順序が入力順(= 宣言順・走査順)のまま保たれるので、「レイヤが同じなら後に書いたものが上」という直感的な規則が、追加のタイブレーク実装なしで得られます。逆に `std::sort` は等価要素の順序を保証しないため、同じ結果を再現したければ宣言順のシーケンス番号を明示的にキーへ入れる必要があります。

**このリポジトリでは**: `src/core/ui/module.cpp` の `visitNodes()` が DFS で `QuadCommand` を積み、`src/core/ui/drawcommands.cpp` の `buildDrawBatch()` が `std::stable_sort` で `(layer, decl_seq)` 順に並べ直してから `runs` にまとめています。ヒットテスト用の走査順は別途 `createRuntimeWidgets()` が `traversal` に前順で溜めます。

### 正規表現でパスやページ名を検証するときの、区切り文字とドットの扱い

C++ の `std::regex` は既定で ECMAScript 文法です。この文法では `.` は改行以外の任意の 1 文字を意味するので、拡張子やドット区切りの名前を照合したいときは `\.` とエスケープするか文字クラス `[.]` に入れる必要があります。C++ の通常の文字列リテラルに書く場合はバックスラッシュ自体をエスケープするので `"\\."` と二重になります(生文字列リテラル `R"(\.)"` ならそのまま 1 個)。一方 `/` は ECMAScript の**文法上は**特別な文字ではなく、JavaScript のリテラル `/.../` の区切りとして特別なだけなので、C++ 側では `\/` とせずそのまま書けます。

`std::regex_match` は文字列全体との一致を要求し、部分一致を探す `std::regex_search` とは別物です。したがって `regex_match` に渡すパターンの `^` と `$` は冗長ですが、意図を読み手に示すために残されることがよくあります。「セグメント/セグメント/…」の形は `SEG(/SEG)*` と書くのが定番で、これなら先頭・末尾の空セグメントや連続スラッシュを構造的に弾けます。

**このリポジトリでは**: `src/core/ui/semanticfixture.cpp` の名前空間スコープの `std::regex` 群。`texture_re` が `(\\.[A-Za-z0-9_-]+)*` でドット区切りをエスケープしつつ `/page:[0-9]+` を要求し、`widget_re` が `^[a-z0-9_]+(/[a-z0-9_]+)*$` でスラッシュ区切りのウィジェットパスを、`pointer_re` が RFC 6901 の JSON Pointer(`~` の後は `0` か `1` のみ)を検証しています。

## 10.6 座標・数値の約束事

### NDC(正規化デバイス座標)と Vulkan の座標系

頂点シェーダが `gl_Position` に書く値はクリップ空間の同次座標で、これを w で割ったものが NDC です。Vulkan では NDC の x, y が [-1, 1]、**z が [0, 1]** と定められています(Vulkan 仕様 "Coordinate Systems")。z の範囲が [-1, 1] なのは OpenGL の規約で、Vulkan では違います。さらにフレームバッファ座標の原点は**左上**で、y は下方向に増えます。したがってビューポートの height を正の値にしたまま描くと、NDC の +Y はフレームバッファの下方向へ向かいます。OpenGL 育ちの読者が「上下が逆」と感じる差はここに集約されます。

**このリポジトリでは**: 深度が [0, 1] 前提であることは `src/core/vkcore/previewexecutor.cpp` の `raster()` にある `ndc.z < -0.1f || ndc.z > 1.1f` の間引き判定、`test/camera_test.cpp` の「znear → 0.0 / zfar → 1.0」を検証する `REQUIRE`、そして `src/core/resources/taa_resolve.frag` の `linearizeDepth()` が深度バッファの値を再マップせずそのまま `vec4(uv * 2.0 - 1.0, depth, 1.0)` の z に入れている点に現れます。

### GLM の `_RH_ZO` / `_NO` サフィックスの意味

GLM の射影行列生成関数は名前に規約が埋め込まれています。`RH` は右手系のビュー空間(カメラは -Z を向く)、`LH` は左手系。`ZO` は深度出力が **Z**ero-to-**O**ne、`NO` は **N**egative-one-to-**O**ne です。サフィックス無しの `glm::perspective` は `GLM_FORCE_DEPTH_ZERO_TO_ONE` というコンパイル時マクロの有無で `_ZO` / `_NO` に切り替わるため、マクロの定義漏れで深度が壊れる事故が起きやすい箇所です。サフィックス付きの関数を直接呼べばマクロに依存しません。なお GLM の `perspectiveRH_ZO` は `Result[1][1]` が正であり、Y 軸の反転は射影行列側では行われません。

**このリポジトリでは**: `src/core/renderer/camera.cpp` の `Camera::rebuildProjectionMatrix()` と `Camera::prepareSceneCameras()` が、マクロに頼らず `glm::orthoRH_ZO` / `glm::perspectiveRH_ZO` を明示的に呼んでいます。

### ビューポート変換と「負の height」による Y 反転

NDC からフレームバッファ座標への変換は Vulkan 仕様 "Controlling the Viewport" に式として書かれています。`x_f = (p_x/2)·x_d + o_x`、`y_f = (p_y/2)·y_d + o_y`、`z_f = p_z·z_d + o_z` で、`p_x` = viewport.width、`o_x` = viewport.x + width/2、`p_y` = viewport.height、`o_y` = viewport.y + height/2、`p_z` = maxDepth - minDepth、`o_z` = minDepth です。ここで **height に負の値を入れると y の符号が反転する**というのが VK_KHR_maintenance1(Vulkan 1.1 でコアに昇格)で入った仕様で、「OpenGL 流の Y 上向き NDC をそのまま使うためのトリック」として広く使われています。逆に height を正のままにするなら、NDC の +Y は画面下方向という Vulkan 本来の向きになります。

**このリポジトリでは**: `src/core/vkcore/offscreenframetarget.cpp` の `setViewportAndScissor()` と `src/core/vkcore/swapchainframetarget.cpp` のフレーム開始処理が、どちらも `viewport.height = static_cast<float>(extent.height)`(正の値)を設定しています。その前提は `src/core/renderer/projectionjitter.cpp` の `projectionJitterSample()` 内のコメントに明記されています。

### w 除算(パースペクティブ除算)と w ≤ 0 のガード

クリップ空間から NDC を得るには xyz を w で割ります。射影行列を通すと透視射影では w がビュー空間の -z(=カメラからの奥行き)になるため、カメラ**後方**の点では w が負、近平面上の縮退では w が 0 付近になります。このまま割ると符号が反転して、後ろにあるはずの点が画面内の反対側に現れます。GPU はラスタライズ前に同次クリッピングでこれを処理してくれますが、**CPU 側で自分で投影するコードにはクリッピングが無い**ので、w の符号と大きさを自前で確認して弾く必要があります。これが投影ユーティリティに必ず `w <= eps` の早期 return が付いている理由です。

**このリポジトリでは**: `src/core/phys/physworld.cpp` の `projectPoint()` が `std::abs(clip.w) <= kProjectEpsilon || clip.w < 0.0f` で false を返し、`src/core/vkcore/previewexecutor.cpp` の `raster()` は `!std::isfinite(clip.w) || clip.w <= 0.00001f` で `continue` します。

### `ndc * 0.5 + 0.5` と、その逆変換の `2 / size`

[-1, 1] を [0, 1] に写すアフィン変換が `v * 0.5 + 0.5`、そこにサイズを掛ければピクセル座標になる、というのがグラフィクスで最も頻出するイディオムです。同じ式は UV 生成(フルスクリーン三角形)でもシャドウマップの参照でも使われます。重要なのは**逆向き**で、「ピクセル単位のオフセット」を NDC のオフセットに直すときは `2 * px / size`、つまり 0.5 の逆数である 2 が掛かります。この 2 が抜けると、ジッタやスナップの効きが常にちょうど半分になります。

**このリポジトリでは**: 順方向は `src/core/resources/sprite.vert` の `anchorFramebuffer = (anchorNdc * 0.5 + 0.5) * pelicanFrame.resolution.xy`、逆方向は同ファイルの `deltaNdc = (snappedFramebuffer - anchorFramebuffer) * 2.0 * pelicanFrame.resolution.zw` と `src/core/renderer/projectionjitter.cpp` の `jitter_ndc = 2.0f * offset_px / size` です(`resolution` の zw に 1/width, 1/height が入るのは `src/core/vkcore/renderer.cpp` の `data.resolution = glm::vec4{width, height, inverse_width, inverse_height}`)。

### NDC 空間のオフセットは、w を掛けてクリップ空間で足す

「最終的な NDC を Δ だけずらしたい」とき、`gl_Position` に直接 Δ を足すのは間違いです。出力は後段で w で割られるので、実際のずれは Δ/w になってしまいます。正しくは **Δ に w を掛けてから足す**(`clip.xy += delta_ndc * clip.w`)と、除算後にちょうど Δ だけずれます。同じことを行列側で一度に行うのが「射影行列の 1・2 行目に、w を作る行(第 4 行)の Δ 倍を加える」という書き換えで、TAA のジッタ行列はほぼ例外なくこの形で実装されています。

**このリポジトリでは**: 頂点単位の版が `src/core/resources/sprite.vert` の `clip.xy += deltaNdc * clip.w`、行列版が `src/core/renderer/projectionjitter.cpp` の `applyProjectionJitter()` にある `result[column][0] += jitter_ndc.x * projection[column][3]`(`projection[column][3]` が w を作る成分)です。

### 画像の行 0 は上端 — CPU ラスタで Y 反転が要る理由

PNG をはじめ多くのラスタ画像フォーマットは、非インタレース時のスキャンラインを**上から下へ**格納します(PNG 仕様 ISO/IEC 15948)。したがって `pixels[y * width + x]` のような線形バッファでは、y = 0 が画像の上端です。GPU 経由で描く場合はこの対応をビューポート変換が引き受けるので、シェーダ側に反転は現れません。一方、CPU で NDC から直接ピクセルを書き込むコードは、自分の座標系の Y の向きと画像の行方向の対応を明示的に書く必要があります。GPU 経路と CPU 経路で反転の有無が食い違って見えるのは、この責務の置き場所が違うためです。

**このリポジトリでは**: `src/core/vkcore/previewexecutor.cpp` の `raster()` が `y = (1.0f - (ndc.y * 0.5f + 0.5f)) * request.height` と明示的に反転しています(同ファイルの `encodePng()` が行ごとに先頭フィルタバイト 0 を積むループが、行が上から下である前提そのものです)。GPU 側の `src/core/resources/sprite.vert` には反転がありません。

### ピクセル中心は n + 0.5、テクセル中心は (i + 0.5) / size

Vulkan のラスタライズ規則では、整数のフレームバッファ座標は**ピクセルの角**を指し、整数座標 (x, y) を持つピクセルの**中心**は (x + 0.5, y + 0.5) です(Vulkan 仕様 "Rasterization")。だからフラグメントシェーダの `gl_FragCoord.xy` は既定で半整数になります(Vulkan では原点も常に左上)。テクスチャ側も同じ約束で、テクセル (i, j) の中心は正規化 UV の ((i + 0.5)/width, (j + 0.5)/height) です。ピクセルアートを nearest で等倍表示するときに「境界を整数に合わせる」のは、この結果としてサンプル点が各テクセルの中心に落ちるようにするためで、0.5 ずれるとどのテクセルを取るかが場所によって変わり、行や列が飛んだり重複したりします。

**このリポジトリでは**: `src/core/userpublic/sprite/pixelpolicy.hpp` の `quantizePixelBoundary` 宣言のコメントが「境界にスナップした結果テクセルサンプルが n + 0.5 に落ちる」と述べており、実装は `src/core/userpublic/sprite/pixelpolicy.cpp` の `std::floor(framebuffer_coordinate + 0.5)`、GPU 側の対応物が `src/core/resources/sprite.vert` の `floor(anchorFramebuffer + 0.5)` です。テクセル中心の側は `src/core/resources/taa_resolve.frag` の `currentNeighborhood()` にある `firstCenter = texelSize * 0.5` / `lastCenter = 1.0 - firstCenter` という UV のクランプ範囲がそのまま表しています。

### `floor(x + 0.5)` と `round()` は同じ丸めではない

`std::round` は C++ 標準(C の `round` に委譲)で「半端は 0 から遠い方へ」丸めると規定されています。つまり -2.5 は -3 になります。対して `floor(x + 0.5)` は常に +∞ 方向へ半端を送るので、-2.5 は -2 です。正の値しか来ない場面では一致しますが、負の値を含むと結果が変わります。さらに厄介なのは GLSL 側で、GLSL 仕様の `round()` は **0.5 の丸め方向が実装定義**(実装が速い方を選んでよい)と明記されており、半端が決定的に決まるのは `roundEven()` だけです。CPU と GPU で同じ結果を出したいコードが `round` ではなく `floor(x + 0.5)` を選ぶのは、この実装定義を踏まないためです。

**このリポジトリでは**: `src/core/userpublic/sprite/pixelpolicy.cpp` の `quantizePixelBoundary` が `std::floor(v + 0.5)`、対応する GLSL が `src/core/resources/sprite.vert` の `floor(anchorFramebuffer + 0.5)` で一致させてあります。境界値の期待は `test/sprite_foundation_test.cpp` の `quantizePixelBoundary(3.49) == 3.0` / `(3.50) == 4.0` に固定されています。

### ε 比較:絶対誤差・相対誤差・その混合

`std::abs(a - b) <= eps` は**絶対誤差**の比較で、値が大きくなると破綻します。float の仮数は 24 ビットなので、1.0 付近の刻み幅(ULP)は 2^-23 ≒ 1.19e-7 ですが、10^4 付近では ≒ 9.8e-4 になり、eps = 1e-5 のような閾値は「隣り合う表現可能な値ですら通らない」領域に入ります。逆に `<= eps * std::abs(a)` の**相対誤差**は 0 近傍で閾値が潰れて使えません。そこで定番なのが両者の混合で、`<= eps * std::max(1.0, std::abs(a))` と書き、1 未満のスケールでは絶対誤差、それ以上では相対誤差として振る舞わせます。閾値そのものは「意味のある差」から決めるべきで、機械イプシロンから決めるものではありません。

**このリポジトリでは**: `src/core/userpublic/sprite/pixelpolicy.cpp` の `closeToInteger()` と `nearlyZero()` がまさに `pixelContractTolerance * std::max(1.0, ...)` の混合形で、閾値定数 `pixelContractTolerance = 1.0e-4` は `src/core/userpublic/sprite/pixelpolicy.hpp` にあります。

### 長さは二乗のまま比較し、ε も二乗する

ベクトルの長さを閾値と比べるだけなら `std::sqrt` は不要で、`lengthSquared(v) <= eps * eps` と書けます。平方根の除去は速度のためだけでなく、0 近傍で sqrt の傾きが発散するため微小量に対する精度が落ちる、という理由もあります。ここで閾値を `eps` ではなく **`eps * eps`** にするのが要点で、比較する量が二乗スケールなのに閾値だけ一乗のままだと、実質の閾値が sqrt(eps) にすり替わります(eps = 1e-5 なら約 3.2e-3、300 倍以上緩くなる)。正規化前のゼロ長判定でこの形が頻出します。

**このリポジトリでは**: `src/core/phys/physquery.cpp` の `normalizedQuat()` と `closestPointOnSegment()` が `len_sq <= kEpsilon * kEpsilon`、`src/core/phys/physworld.cpp` の `normalized()` と `src/core/phys/joltphysicsprovider.cpp` の `normalizeOr()` も同じ形です。

### NaN はあらゆる比較で false — `isfinite` を先に置く

IEEE 754 では NaN を含む順序比較(`<`, `<=`, `>`, `>=`, `==`)はすべて false を返し、`!=` だけが true を返します。この非対称性のせいで、`if (d <= eps) return fallback;` と `if (d > eps) { ... }` は NaN の扱いが逆になり、条件の書き方ひとつで挙動が変わります。しかも inf - inf や 0 * inf は NaN を生むので、正規化やレイキャストのように除算と減算が混ざる経路では簡単に混入します。ε 比較そのもので NaN を弾こうとせず、`std::isfinite` による有限性チェックを比較の**前**に明示的に置く、というのが定石です。

**このリポジトリでは**: `src/core/userpublic/sprite/pixelpolicy.cpp` の `finiteBasis()` と `evaluatePixelContract()` 冒頭の `!std::isfinite(pixels_per_unit)` 系のガード、`src/core/phys/joltphysicsprovider.cpp` の `toJoltNormalized()` / `normalizeOr()` にある `!std::isfinite(...) || ... <= kEpsilon` がこの形です。

### 「ε 以内なら等しい」は同値関係ではない

`std::abs(a - b) <= eps` は反射律と対称律を満たしますが、**推移律を満たしません**。a と b が近く、b と c が近くても、a と c は 2·eps 離れうるからです。これは単なる理屈の話ではなく、C++ 標準が `std::unique` の二項述語に「同値関係であること」を事前条件として要求している(`[alg.unique]`)ため、ε 比較を渡した時点で規格上は未定義の領域に入ります。実際に何が起きるかは「直前に残した要素と比べるか、隣接要素同士で比べるか」という実装の選択に依存し、値が数珠つなぎに並んだ入力で結果が分かれます。ソート済みの入力で近接値をまとめる、という用途では意図どおり動くことが多いものの、境界の切れ方が処理系依存であることは意識しておく必要があります。

**このリポジトリでは**: `src/core/phys/physquery.cpp` で、`std::sort` した `breaks` に対して `std::unique(..., [](float lhs, float rhs) { return std::abs(lhs - rhs) <= kEpsilon; })` を掛けている箇所がこの形です。近接値のクラスタリング規則自体は `src/core/phys/physquery.hpp` の `shapeCastTieEpsilon` 周辺のコメントで契約として文章化されています。


## 関連文書

- [README](README.md) — 読む順番と難所インデックス
- [第6章 描画・Vulkan・Shader](06_rendering_vulkan_shader.md) — §10.1 / §10.6 の知識が最も濃く要る章
- [第3章 プロジェクトデータとロード](03_project_and_loading.md) — §10.2 の glTF まわり
- [第9章 黒魔術・制約](09_black_magic_and_gotchas.md) — §10.3 の C++ イディオムが「なぜこの形か」の答え
