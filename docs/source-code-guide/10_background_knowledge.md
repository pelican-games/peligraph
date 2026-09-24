# 第10章 前提知識の補足(このコードが当然としていること)

調査時点: 2026-07-21 / 基準コミット: `85882ec`
(§10.1 末尾の 5 項目 — cube イメージ / subresource range / mip 数の上限 / タイル常駐 / dynamic rendering local read — のみ 2026-07-31 / 基準コミット `ef76292`)
(§10.4 の 2 項目 — PE の import directory / 名前付き mutex と `Global\` 名前空間 — のみ 2026-09-04 / 基準コミット `8864050`)
(§10.1 の swapchain 4 項目 — maxImageCount の 0 / currentExtent の特別値 / sharing mode / vulkan-hpp の戻り値規約 — のみ 2026-07-31 / 基準コミット `ab731bb`。frame target が surface/swapchain の世代管理へ作り替わったのに合わせて書き直したもの)
(**§10.8 の 6 項目 — 2026-09-17 / 基準コミット `82054c05`。** v0 で入った共通グラフ・package・オフライン時刻の周りで、「エンジン固有の設計」ではなく**一般的な作法**にあたる話をまとめたものです。本文は他の 2026-09-04 項目と同じ「使用場所」書式で、行番号はリンクに寄せてあります)
(§10.1 の「色空間と伝達関数」と §10.5 の 3 項目 — Floyd–Warshall 法 / GJK と EPA / JSON-RPC 2.0 の予約エラーコード — のみ 2026-09-04 / 基準コミット `76f61371`。他章から「第10章へ委譲」と書かれていた一般知識を後から補ったもの)
(**2026-09-04 に足した 70 項目と、既存項目への「追記」段落 — 基準コミット `349a2d38`。** src を初読した読み手が「一般知識が足りなくて読めない」と挙げた 109 件を既存項目と突き合わせ、重複を除いた残りです。**この 70 項目と追記段落だけは markdown リンクで書かれており、[`tools/doclink.py`](../../tools/doclink.py) が行番号を追従させます** — 下の「行番号は追従されません」は、それ以前に書かれた 73 項目の本文にかかります)

第2〜9章の「🧩 難所」ブロックが**このコードベース固有の難しさ**を扱うのに対して、この章は **pelican2 のコードが「知っている前提」で書かれている一般知識**をまとめたものです。仕様の細部や定番イディオムの名前を知らないと、コード自体は素直なのに読めない — そういう箇所を拾ってあります。

> **この章の作り方:** エンジンのソースを経験の浅い読み手に読ませ、「分からなかった」と報告された 88 箇所を、実装と突き合わせて仕分けしました。**このコードベース固有の難所だった 25 件**は第2〜9章の難所ブロックへ、**「一般知識を知らないから読めなかった」残り**をこの章に集めています。つまりここに並ぶのは、エンジンの設計ではなく **Vulkan / glTF / C++ / OS / アルゴリズムの側の知識**です。

> **先発の 73 項目は行番号が追従されません(意図的です)。** 他章は markdown リンクで書かれ
> [`tools/doclink.py`](../../tools/doclink.py) がコードの移動に追従させますが、その 73 項目は
> `` `chooseSwapchainImageCount()`(175 行) `` の形で 227 箇所を引用しており、その 9 割は
> 「直前で挙げたファイルの N 行目」です。全部をリンクにすると 1 項目が 3 件超ずつリンクを
> 抱えて解説として読めなくなるため、**追従の代わりに上の基準コミットを宣言する**方を採って
> います。行番号がずれていたら、その項目の基準コミットで読んでください。
> **2026-09-04 に足した 70 項目は「**使用場所**:」の行にリンクをまとめる書式**にしてあり、
> 本文には行番号を書きません。こちらは `doclink.py` の追従対象です。
>
> ただし**関数名の方は追従されています**。`doclink.py audit` はソース全体の識別子と
> 突き合わせるので、この章が消えた関数や改名された関数を説明していれば検出されます
> (書式には依存しません)。守られていないのは行番号だけです。

各項目は「一般知識の説明」→「どこに出てくるか」の順です。後者の見出しは、先発の 73 項目が「**このリポジトリでは**」、2026-09-04 に足した 70 項目が「**使用場所**」で、後者にはリンクの後ろに「**このコードベースでの注意**」が付くことがあります(一般論と違う使われ方をしている場合だけ)。上から読む必要はありません。コードで見慣れない書き方に出会ったときに引いてください。

## この章の索引

全 149 項目。分類ごとに知識名だけを並べます(節そのものへの見出しリンクは張っていません — ブラウザの検索で名前を引いてください)。

**10.1 Vulkan の仕様と作法**(34 項目)

- swapchain の maxImageCount が 0 のとき
- VkSurfaceCapabilitiesKHR::currentExtent の 0xFFFFFFFF
- 色空間と伝達関数(OETF / EOTF)、linear と sRGB
- sRGB フォーマットをスコアで選ぶ理由
- present mode で必ず存在するのは FIFO だけ
- sharing mode: eExclusive と eConcurrent
- usage flags と format feature flags は別物
- timeout に 0 を渡す waitForFences / acquireNextImageKHR
- vulkan-hpp の戻り値規約(いつ Result が返り、いつ例外が飛ぶか)
- vk::DrawIndexedIndirectCommand の各フィールド
- drawIndexed の vertexOffset はインデックス値に加算される
- drawIndirect の stride は構造体より大きくてよい
- multiDrawIndirect、drawIndirectFirstInstance、shaderDrawParameters(gl_BaseInstance)
- bufferRowLength / bufferImageHeight の 0 は「詰めて配置」
- cube イメージは「6 レイヤ + CUBE_COMPATIBLE フラグ」でしかない
- image view の subresource range(baseMipLevel / levelCount / baseArrayLayer / layerCount)
- mip 数の上限は `floor(log2(max(w,h))) + 1`
- LAZILY_ALLOCATED メモリと TRANSIENT_ATTACHMENT(タイル常駐)
- dynamic rendering local read(input attachment を dynamic rendering で使う)
- descriptor set と WriteDescriptorSet(binding 番号がシェーダとの契約)
- descriptor binding flags と ePartiallyBound(descriptor indexing / bindless)
- descriptor type の使い分け(combined image sampler / storage image / sampled image + sampler)
- pNext チェーンと vulkan-hpp の StructureChain / unlink
- 機能照会(getFeatures2 の chain)と拡張関数の動的解決
- VMA(Vulkan Memory Allocator)の usage と allocation flags
- buffer device address と usage flags
- std140 / std430 レイアウト規則
- 頂点入力の binding / attribute / stride
- multiview と view mask
- sampler の filter と address mode
- レイトレーシング: shader group と SBT(handle size / stride / base alignment)
- レイトレーシング: AS ビルドの入力とメモリ要件(maxVertex / instance 配列 / scratch)
- レイトレーシング: instance flags と any-hit シェーダ
- in-flight フレームと GPU 資源の寿命

**10.2 glTF / 3D アセット形式の仕様**(12 項目)

- GLB のコンテナ構造(ヘッダ + JSON チャンク + BIN チャンク)
- buffer / bufferView / accessor の三層と byteStride
- sparse accessor
- normalized 整数属性を float へ戻す規則(UNORM / SNORM)
- node.weights と mesh.weights の優先順位
- primitive.mode(トポロジ)と TRIANGLES 以外を扱わない実装が多い理由
- extensions / extensionsUsed / extensionsRequired と extras
- 行列から TRS を分解できない場合(skew / perspective 成分)
- VRM / VRMA が glTF 拡張として乗っていること
- glTF の node.matrix は列優先の 16 要素
- animation channel の出力 accessor の型は target_path で決まる
- JOINTS_0 / WEIGHTS_0(skin ローカルの joint index と重みの正規化)

**10.3 C++ とビルドのイディオム**(40 項目)

- RAII ハンドルラッパ(コピー禁止・ムーブのみ)
- pimpl と「デストラクタは .cpp で定義する」規則
- CRTP(奇妙に再帰したテンプレートパターン)による強い型付け
- 世代付きハンドル(index + generation)と SlotMap
- 単調増加する generation / epoch を「変わったか」の検出に使う
- std::erase_if(erase-remove イディオムの置き換え)
- std::tie による多段キーの辞書式比較
- C++20 の指示付き初期化子(designated initializers)
- デフォルト化された operator<=>(三方比較)
- std::vector<bool> の特殊化
- std::filesystem の error_code オーバーロード(投げない版)
- プラットフォーム分岐の #ifdef で、同名関数を 2 回定義する
- windows.h を include する前の NOMINMAX
- 機能フラグをマクロ「値」で渡し、#ifdef ではなく #if で見る
- thread_local + RAII による「今このスレッドが実行中のもの」
- スコープガード(巻き戻しをデストラクタに置き、成功時に無効化する)
- double-checked locking と acquire / release
- compare_exchange による所有権の一度きりの確定
- ビットパッキングとビットマスクのイディオム
- shared_ptr / weak_ptr による寿命の「リース」
- 参照カウンタ付きの遅延解放と、受付/排出を分ける状態機械
- moved-from 状態の契約
- 型消去: std::any と any_cast
- Result 型で例外型を境界の外に出さない
- std::variant + std::visit + if constexpr のディスパッチ
- fold 式と std::apply(タプルライク型の全要素処理)
- consteval とコンパイル時の throw
- __COUNTER__ と ADL タグディスパッチによる「登録済みの型の列挙」
- メンバーポインタの非型テンプレート引数
- concept / requires 節と、特殊化による trait チェーン
- std::span によるイテレータ型・要素数の消去
- 関数スコープ static の一度きり初期化
- static_assert + sizeof による ABI レイアウトの凍結
- std::bit_cast と浮動小数点のビット表現(-0.0 の正規化)
- 整数オーバーフローの事前検査
- swap-and-pop(末尾要素で穴を埋める削除)
- C++20 の jthread / stop_token / condition_variable_any による協調停止
- 固定長バッファ(例外も動的確保もしない経路のため)
- 自己参照ラムダによる再帰
- コールバック連鎖(既存のフックを move して包む)

**10.4 OS・プロセス・ファイルの作法**(13 項目)

- 実行中の DLL は Windows では上書きできない(シャドウコピー)
- 同じパスの LoadLibrary / dlopen は参照カウントされる
- POSIX のプロセスグループと setpgid、kill(-pid) で子孫ごと止める
- Windows の Job オブジェクト(プロセスグループの対応物)
- PE の import directory / delay-import directory を、イメージをマップせずに読む
- Windows の名前付き mutex と `Global\` 名前空間(machine-wide の相互排他)
- 子プロセスのパイプ読みとデッドロック回避
- ファイル監視 API は取りこぼす前提で使う
- 一時ファイル + rename による原子的置換
- Windows のファイル共有モードとロック検出(ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION)
- プロセス生存判定と PID の型
- 動的ライブラリのエラー取得(GetLastError と dlerror)
- std::filesystem の canonical / weakly_canonical とルート名

**10.5 アルゴリズムとデータ処理の定番**(20 項目)

- Kahn 法のトポロジカルソートと、未知の依存先を無視する実装
- Floyd–Warshall 法と推移閉包(到達可能性の全点対計算)
- GJK と EPA(凸形状の距離計算と貫通量計算)
- CRC-32 の初期値 0xFFFFFFFF と最終反転(なぜ 0 から始めないか)
- zlib / deflate の BFINAL ビットと「非圧縮(stored)ブロック」
- デッドゾーンの取り方(成分ごと vs 半径)と、方向を保つ再スケール
- 走査順(DFS)と表示順(ソートキー)を分けて考えること
- JSON-RPC 2.0 の予約エラーコード(出典は RFC ではない)
- 正規表現でパスやページ名を検証するときの、区切り文字とドットの扱い
- Möller–Trumbore 光線-三角形交差
- 軸への投影による分離判定(AABB の slab 法と OBB の分離軸定理)
- Time of Impact(TOI)と conservative advancement
- FNV-1a とハッシュ値の合成(0x9e3779b9)
- ソート済み列に対する隣接検査(adjacent_find)
- 再帰的な型の循環検知(訪問スタック)
- SPIR-V のバイナリ形式
- JSON Pointer(RFC 6901)
- nlohmann::json の型判定・キー順・JSON null
- IEEE 754 の倍精度と JSON の「安全な整数」2^53−1
- JSON-RPC 2.0 の request / response の形と行区切り framing

**10.6 座標・数値の約束事**(21 項目)

- NDC(正規化デバイス座標)と Vulkan の座標系
- GLM の `_RH_ZO` / `_NO` サフィックスの意味
- ビューポート変換と「負の height」による Y 反転
- w 除算(パースペクティブ除算)と w ≤ 0 のガード
- `ndc * 0.5 + 0.5` と、その逆変換の `2 / size`
- NDC 空間のオフセットは、w を掛けてクリップ空間で足す
- 画像の行 0 は上端 — CPU ラスタで Y 反転が要る理由
- ピクセル中心は n + 0.5、テクセル中心は (i + 0.5) / size
- `floor(x + 0.5)` と `round()` は同じ丸めではない
- ε 比較:絶対誤差・相対誤差・その混合
- 長さは二乗のまま比較し、ε も二乗する
- NaN はあらゆる比較で false — `isfinite` を先に置く
- 「ε 以内なら等しい」は同値関係ではない
- 区間演算(interval arithmetic)と有向丸め
- ULP と nextafter によるパディング
- 四元数: 単位性の判定とベクトル回転の展開形
- GLM の quat コンストラクタ引数順は (w, x, y, z)
- TRS の合成順序と world → local の逆算
- 法線の変換は transpose(inverse(M))
- 切り上げ除算と mip 寸法のシフト
- view 行列の逆行列とカメラの前方軸

**10.7 OpenXR ランタイムの作法**(3 項目)

- swapchain image の acquire → wait → release 契約
- XR_TIMEOUT_EXPIRED は正常系(失敗ではない)
- XR_KHR_composition_layer_depth(深度をランタイムへ渡す)

**10.8 宣言的なグラフと拡張の作法**(6 項目)

- レジストリと登録順(登録順が文書の意味を変えない、という条件)
- semantic type と logical type(不透明な型がハンドルを運ぶ理由)
- 評価の分類(direct / replay / baked / recorded)と変更の費用(value / recompile / rebuild)
- 排他 create を「決定」の primitive にする(O_EXCL / hard link)
- 内容アドレスによる vendoring(sha256 が同一性、ABI の本当の gate は別)
- 連続時刻を固定ステップのタイムラインへ量子化する

## 10.1 Vulkan の仕様と作法

### swapchain の maxImageCount が 0 のとき

`vk::SurfaceCapabilitiesKHR` の `minImageCount` / `maxImageCount` は、そのサーフェスに対して作れる swapchain イメージ枚数の範囲です。Vulkan 仕様では `maxImageCount` の 0 は「特別値で、上限がない(メモリなど他の要因が許す限り何枚でもよい)」と定義されています。**仕様上の位置**は "Window System Integration (WSI)" 章 → "Surface Queries" 節 → 構造体 `VkSurfaceCapabilitiesKHR` の項です(章・節の**番号**は仕様の版ごとに動くので、ここでは番号を書きません。HTML 版はアンカー `#VkSurfaceCapabilitiesKHR` で直接引けます。次項の `currentExtent` も同じ構造体の同じ項です)。0 を素直に上限として扱うと `std::min` で枚数が 0 になり、swapchain 作成が即失敗します。そのため「`maxImageCount > 0` のときだけクランプする」という書き方が定型になります。`minImageCount + 1` を狙うのも定型で、最低枚数ちょうどだと presentation engine が 1 枚握っている間 CPU が次のイメージを取得できず待たされるためです。なお `minImageCount` は「これ以上を要求せよ」という下限であり、ドライバはそれより多く返してくることがあるので、実際の枚数は `getSwapchainImagesKHR` の戻り値で数え直す必要があります。

**このリポジトリでは**: `src/core/vkcore/swapchainframetarget.cpp` の `chooseSwapchainImageCount()`(175 行)が `minImageCount + 1` を要求し(177 行)、`maxImageCount > 0` のときだけ `std::min` でクランプします(178〜181 行)。この値は `createSwapchain()` の `create_info.minImageCount`(313 行)へ渡す「下限の要求」でしかなく、実枚数はドライバが返します。数え直しをするのは swapchain 世代を 1 つ組み立てる `buildEpoch()`(963 行)で、`device.getSwapchainImagesKHR()`(1059〜1061 行)の戻りをそのまま `epoch->images` にし、**その個数ぶん**だけ color image view(`createColorImageViews()`、1062 行)・present semaphore(1079〜1081 行)・present 待ちフラグ(1082〜1083 行)・`VK_EXT_swapchain_maintenance1` の present fence(1086〜1095 行)・debug 名(1160〜1171 行)を回します。同じ epoch の中でも acquire semaphore とコマンドバッファは**イメージ枚数ではなく in-flight フレーム数**で回る(1076〜1078 行、1113〜1127 行)ので、「枚数で回るもの」と「フレームスロット数で回るもの」を混同すると present 待ちが 1 本足りない、という壊れ方をします。`+1` を狙うのは presentation engine が 1 枚握っている間も CPU が次のイメージを acquire できるようにするためで、`maxImageCount == 0` を上限として扱うと枚数 0 で swapchain 作成が即失敗し、そもそも表示に入れません。

### VkSurfaceCapabilitiesKHR::currentExtent の 0xFFFFFFFF

`currentExtent` はサーフェスの現在のサイズですが、仕様では「`width` と `height` がともに `0xFFFFFFFF`(= `UINT32_MAX`)のとき、サーフェスサイズは swapchain 側の `imageExtent` によって決まる」という特別値になっています。前項と同じ `VkSurfaceCapabilitiesKHR`("Window System Integration (WSI)" 章 → "Surface Queries" 節、アンカー `#VkSurfaceCapabilitiesKHR`)の記述です。これは Wayland のように「ウィンドウのサイズをアプリが決める」プラットフォーム向けの規定です。この場合はアプリが自前でフレームバッファサイズを調べ、`minImageExtent` / `maxImageExtent` の範囲にクランプして渡す必要があります。逆に特別値でなければ `currentExtent` をそのまま使うのが正解で、独自に計算した値を渡すと不一致になります。ウィンドウ最小化時に extent が 0 になる環境もあるため、0 のときはフレームを作らず抜ける、という分岐も定型です。

**このリポジトリでは**: `chooseSwapchainExtent()`(swapchainframetarget.cpp:185)が `currentExtent.width != std::numeric_limits<uint32_t>::max()` なら presentation engine が決めた `currentExtent` をそのまま採り(188〜191 行)、特別値のときだけ引数のフレームバッファ実サイズを `minImageExtent` / `maxImageExtent` へ `std::clamp` します(192〜201 行)。その実サイズは `Window::framebufferSnapshot()`(`src/core/os/window.cpp`:291)が返す `FramebufferExtentSnapshot`(extent と `revision` の組)で、GLFW の framebuffer size コールバックが mutex 下で書き込み、サイズが実際に変わったときだけ `revision` を進めます(window.cpp 216〜236 行)。決まった extent は epoch の `OutputCompileFacts` になり、publish 時に `Camera::setScreenSize()` へ渡って投影の縦横比になります(初期化 866 行 / epoch 差し替え 1660 行)。

**「非ゼロになるまで待つ」入口はもうありません。**最小化などで extent が 0 のとき、`beginFrame()` は冒頭のスナップショット検査で `FrameBeginDisposition::unavailable` + `FrameUnavailableReason::zero_extent` を返して即座に抜け(2006〜2014 行)、`pumpMaintenance()`(1604 行)が復旧状態機械を `suspended_zero_extent` へ落とします(1853〜1865 行)。非ゼロに戻ったフレームで `resumeFromPositiveExtent()`(1887 行)が復帰し、そこから初めて swapchain の作り直しを予約します。`createSwapchain()` 自身もゼロ extent を `std::invalid_argument` で弾き(231〜235 行)、起動時にゼロだった場合は「bootstrap がエンジンループをブロックできない」として throw します(811〜818 行)。ブロックせず毎フレーム「今は出せない」と答える形にしてあるので、サイズ 0 の swapchain を作らないまま OpenXR のミラー窓もエンジンループも止まりません。

### 色空間と伝達関数(OETF / EOTF)、linear と sRGB

ライティングの加減乗除は「光の量に比例する値」= **linear(リニア)空間**で行わないと合いません。一方で 8 bit の画像や表示装置は、人の明るさ感度に合わせて非線形に符号化した値を扱います。この linear → 符号値の変換を **OETF**(opto-electronic transfer function、光電気伝達関数)、符号値 → linear の逆変換を **EOTF**(electro-optical transfer function、電気光伝達関数)と呼びます。sRGB は「この伝達関数 + 原色(色域) + 白色点」の組で定義された色空間で、伝達関数はよく指数 2.2 のべき乗で近似されますが、規格上は暗部だけを直線にした区分関数です — 順方向は `c <= 0.0031308 ? 12.92*c : 1.055*pow(c, 1/2.4) - 0.055`。この区分点 `0.0031308` と係数 `12.92` / `1.055` / 指数 `2.4` の出典は sRGB の規格そのもの、**IEC 61966-2-1:1999**(および Amendment A1:2003)の伝達関数定義です(検索するならこの規格番号で引けます)。したがって `pow(c, 1/2.2)` は近似であって sRGB そのものではありません。8 bit のまま linear で持つと暗部の階調が足りず縞(バンディング)が出るため、「計算は linear の float、保存と表示は符号化した 8 bit」という配分が定番になります。なお色空間は伝達関数だけでなく原色と白色点も含む語なので、「linear か sRGB か」という言い方は厳密には伝達関数の有無の対比です。

**このリポジトリでは**: この区分関数がそのまま `src/core/resources/output_transform.frag` の `linearToSrgb()`(11〜16 行)にあり、swapchain が `*_SRGB` を採れずハードウェアの OETF が使えないときだけ `PELICAN_OUTPUT_UNORM_FALLBACK` 付きで呼ばれます(21 行)。どちらの経路で sRGB になったかは [第6章](06_rendering_vulkan_shader.md) §6.7 の `OutputCompileFacts::encoding_path`(`srgb_hardware` / `srgb_shader_unorm`)が申告し、中間の `display` target を linear 8 bit にしない理由(§6.1 の難所「format_class が format を上書き」)もこの「8 bit は符号化して持つ」に帰着します。

### sRGB フォーマットをスコアで選ぶ理由

Vulkan では、`*_SRGB` 形式のイメージに書き込むと線形値→sRGB 伝達関数のエンコードがハードウェアで自動的に掛かり、シェーダからサンプリングすると逆にデコードされて線形値が返ります(変換対象は RGB のみで、アルファには掛かりません)。つまり sRGB フォーマットを選べば、シェーダ側は最後までリニア空間で計算したままでよく、`pow(c, 1/2.2)` 相当を自前で書かなくて済みます。UNORM 形式を選ぶとこの自動変換がないので、同じシェーダのままだと画面が暗く沈みます。一方 `colorSpace` の `eSrgbNonlinear` は「presentation engine はこのイメージのデータを sRGB エンコード済みとして扱う」という別の宣言で、フォーマットとセットで意味を持ちます。サーフェスが返すフォーマット一覧に希望のものがあるとは限らないため、「必須」ではなくスコア付けして降順に並べ、先頭を採る書き方にしておくと、非対応環境でも起動だけはできます。

**このリポジトリでは**: `createSwapchain()` の `pred_fmt` / `score_func`(swapchainframetarget.cpp:56〜68)が sRGB+`eSrgbNonlinear` に 20、UNORM+`eSrgbNonlinear` に 10、他に 0 を付け、`std::stable_sort` の降順で `surface_fmts[0]` を採ります(69・81 行)。採れた経路は `caps().color_path` が `"srgb"` / `"unorm_fallback"` として報告します(434〜444 行)。オフスクリーン側は `OffscreenFrameTarget` コンストラクタが `eR8G8B8A8Srgb` を第一候補にし、feature が無ければ `eR8G8B8A8Unorm` に落とします(offscreenframetarget.cpp:102・108〜116)。sRGB を採れればシェーダはリニアのまま書き込み、HW が sRGB エンコードするので `pow(c, 1/2.2)` を持たずに済み、ヘッドレスとオンスクリーンで同じシェーダ・同じ絵になります。UNORM しか無い環境でも起動は通し、`color_path` でどちらの経路だったかをテスト側が判別できます。同じ「加点して `stable_sort` で降順、先頭を採る」形は物理デバイスの選択にも使われていて(2026-09-04 追記)、[pickPhysicalDevice()](../../src/core/vkcore/core.cpp#L523) が「graphics と present が同一 queue family」に +100 のような重みを付けます。こちらは `(score, index)` のペアを逆順イテレータで `stable_sort` する形なので、同点でも添字が並び順を決め、同じマシンでは毎回同じ GPU が選ばれます。

### present mode で必ず存在するのは FIFO だけ

`VkPresentModeKHR` のうち、Vulkan 仕様が「必ずサポートされる」と定めているのは `VK_PRESENT_MODE_FIFO_KHR` のみです。FIFO はキューに積んで垂直同期ごとに 1 枚出す、いわゆる VSync 有効の挙動で、ティアリングは起きませんが最大 1 フレーム分の待ちが入ります。`MAILBOX` は「キューが埋まっているとき、未提示のイメージを新しいもので置き換える」モードで、ティアリングなしのまま遅延を減らせますが、対応していない環境も珍しくありません。したがって present mode も「必須」ではなくスコア付けで優先度を表現するのが定型です。`std::stable_sort` を使うのは、同点(スコア 0)同士のときにドライバが列挙した順序を壊さないためで、MAILBOX がなければドライバが最初に挙げたモードが選ばれます(FIFO は一覧に必ず含まれますが、先頭とは限りません)。

**このリポジトリでは**: `createSwapchain()` の `pred_mode` / `score_func`(swapchainframetarget.cpp:71〜76)が `eMailbox` に 10、他に 0 を付け、`std::stable_sort` の降順で `surface_presentmodes[0]` を `create_info.presentMode` にします(76・102 行)。MAILBOX が無ければ、FIFO を必ず含むドライバ列挙順の先頭がそのまま残ります。これで「MAILBOX があれば VSync のティアリング無しのまま遅延を減らす」程度の弱い優先を表現しつつ、`stable_sort` なので同点(スコア 0)のドライバ順を壊さず、環境ごとの present mode 差で挙動が大きく揺れません。非対応環境でも FIFO で確実に表示できるので、配布先の GPU を選びません。

### sharing mode: eExclusive と eConcurrent

Vulkan のバッファ/イメージは「どのキューファミリからアクセスされるか」を作成時に宣言します。`VK_SHARING_MODE_EXCLUSIVE` は「同時に 1 つのキューファミリだけが所有する」モードで、別ファミリから触るには queue family ownership transfer(release 側と acquire 側の 2 本のバリア)を明示的に記録しなければなりません。`VK_SHARING_MODE_CONCURRENT` は `pQueueFamilyIndices` に列挙したファミリから所有権移動なしにアクセスできますが、実装によっては圧縮などの最適化が無効になり性能が落ちます。そのため「graphics と present が同一ファミリなら EXCLUSIVE、別なら CONCURRENT」という分岐が定番です。仕様上も CONCURRENT のときは `queueFamilyIndexCount` が 2 以上かつ各インデックスが相異なる必要があるので、同一ファミリで CONCURRENT を指定するのは単に違反です。

**このリポジトリでは**: `createSwapchain()`(swapchainframetarget.cpp:219)が、引数で受け取った graphics / present の family index を比べ(329〜337 行)、同一なら `eExclusive`、異なれば `eConcurrent` にして 2 ファミリの配列を `setQueueFamilyIndices()` で渡します。graphics 側は device 作成時に決まった 1 つ(784〜785 行)ですが、present 側は **surface epoch が持つ値**で、`buildEpoch()` が swapchain を作るたびに `request.surface_epoch->presentation_queue_family` を渡します(997〜999 行 / 1019〜1021 行)。surface を作り直す復旧経路では `querySurfacePresentationQueue()`(`src/core/vkcore/windowsurface.cpp`:108)が新しい surface に対して present 可能なファミリを選び直すので(1277〜1282 行)、同じプロセスの中でも epoch ごとに present ファミリが変わりえます。つまりこの分岐は起動時 1 回の判定ではなく、swapchain を作るたびに評価されます。同一ファミリで CONCURRENT を指定するのは仕様違反(index が相異なる必要がある)、別ファミリで EXCLUSIVE のまま present するには所有権移動バリアを自前で書かねばならず、どちらも欠けると特定のハードでだけ swapchain が壊れるか検証エラーになります。GPU ごとにキューファミリ構成が違う中で、この一分岐が配布先の多様な環境で同じコードを通す移植性を担保しています。

### usage flags と format feature flags は別物

`VkImageUsageFlags`(`eColorAttachment`、`eTransferSrc` など)は「このイメージをどう使うつもりか」というアプリ側の宣言です。対して `VkFormatProperties` の `optimalTilingFeatures` / `linearTilingFeatures` / `bufferFeatures` は「その物理デバイスでその形式が実際に何をできるか」という能力の報告で、両者は別の軸です。usage に書いただけでは足りず、そのフォーマットが対応する feature を持っていなければ `createImage` は失敗します。feature は tiling ごとに別なので、`eOptimal` で作るなら `optimalTilingFeatures` を見る必要があります(linear の方を見て通した気になるのがよくある間違いです)。swapchain の場合はさらに軸が 1 本増え、サーフェス側の `VkSurfaceCapabilitiesKHR::supportedUsageFlags` にも許可がないと、そのイメージ用途は使えません。

**このリポジトリでは**: `OffscreenFrameTarget` コンストラクタ(offscreenframetarget.cpp:105〜116)が `optimalTilingFeatures` に `eColorAttachment | eTransferSrc` が両方あるか確かめ、欠ければ `eR8G8B8A8Unorm` に切り替えて再確認し、なお欠ければ throw します。swapchain 側は同じ `eTransferSrc` を「サーフェスの `supportedUsageFlags`」と「フォーマットの `optimalTilingFeatures`」の両軸で確認し、両方あるときだけ `imageUsage |= eTransferSrc` を立てます(swapchainframetarget.cpp:85〜93)。usage に書くだけでは足りず feature が無いと作成が落ちるため、`eTransferSrc` の有無がそのまま「このフレームを画像へ読み出せるか」(`capture_available`)になります。ヘッドレスは常に読み出せる前提でゴールデン画像を出し、ウィンドウ表示は環境が許すときだけ capture を有効にします。

### timeout に 0 を渡す waitForFences / acquireNextImageKHR

`vkWaitForFences` の `timeout` はナノ秒で、仕様では「0 を渡した場合は待たずにフェンスの現在状態をそのまま返す。条件が満たされていなければ `VK_TIMEOUT` を返す」と定められています。つまり timeout 0 は「ポーリング」であり、フレームが GPU 上でまだ終わっていないことを非ブロックで検出する手段になります。`vkAcquireNextImageKHR` にも同じ引数がありますが、こちらは 0 のとき「取得できなければ `VK_NOT_READY` を返す」という規定で、`VK_TIMEOUT` が返るのは timeout が 0 でも `UINT64_MAX` でもない場合です。両方を見て「まだ」と判断するコードは、この境界を跨いでも安全側に倒すための書き方です。逆に `UINT64_MAX` は「無期限に待つ」で、通常のブロッキング経路はこちらを渡します。

**このリポジトリでは**: `SwapchainFrameTarget::beginFrame(bool nonblocking)`(swapchainframetarget.cpp:232)が `nonblocking ? 0 : UINT64_MAX` を `waitForFences`(244 行)と `acquireNextImageKHR`(255 行)の両方に渡し、前者の `eTimeout`・後者の `eTimeout` / `eNotReady` を `std::nullopt` に畳みます(245〜247・257〜261 行)。非ブロック経路は OpenXR のミラー窓用で、フレームがまだ GPU 上で終わっていなくてもコンポジタを待たせないよう 0 でポーリングします(present 失敗時も `device.waitIdle()` を避け `surface_stale` を立てるだけ、414〜420 行)。ブロッキング経路(オフスクリーン / 通常表示)は `UINT64_MAX` で前フレームの完了を確実に待ち、フレーム順序の決定性を保ちます。

### vulkan-hpp の戻り値規約(いつ Result が返り、いつ例外が飛ぶか)

vulkan-hpp は `VULKAN_HPP_NO_EXCEPTIONS` を定義していなければ既定で例外を使いますが、すべての関数が例外だけで済むわけではありません。成功コードが 1 つ(`VK_SUCCESS` のみ)のコマンドは値だけを返し、エラー時に `vk::SystemError` を投げます。一方、仕様上の成功コードが複数あるコマンドは `vk::Result` あるいは `vk::ResultValue<T>` を返し、どの成功コードだったかは呼び出し側が `.result` を見て判断しなければなりません。`vkWaitForFences` は `VK_SUCCESS` と `VK_TIMEOUT`、`vkAcquireNextImageKHR` は `VK_SUCCESS` / `VK_TIMEOUT` / `VK_NOT_READY` / `VK_SUBOPTIMAL_KHR` が成功コードなので、いずれも後者に該当します。同じ理由で `presentKHR` も `eSuboptimalKHR` を戻り値として返します。

**このリポジトリでは**: `SwapchainFrameTarget::beginFrame()`(swapchainframetarget.cpp:1981)が、成功コードを 2 つ持つ `waitForFences` の戻り値を `vk::Result` として受けて分岐し(2036〜2054 行 — 非ブロック時の `eTimeout` だけ「まだ」に畳み、それ以外の非 `eSuccess` は throw)、`acquireNextImageKHR` は `vk::ResultValue<std::uint32_t>` を `.result` / `.value` に分解します(2062〜2099 行)。ここで効いてくるのが「**エラーコードは戻り値には現れない**」という規約の裏側で、`eErrorOutOfDateKHR` や `eErrorSurfaceLostKHR` は例外として飛んできます。そのため呼び出しは `try` / `catch (const vk::SystemError &)` で囲まれ、`resultFromSystemError()`(461 行、`error.code().value()` を `vk::Result` に戻すだけの関数)で戻り値経路と同じ `vk::Result` に揃えてから、ひとつの判定表へ流します。

判定表は 2 段です。`classifyWsiResult()`(`src/core/vkcore/swapchainrecovery.cpp`:40)が `eSuccess` → `ready`、`eSuboptimalKHR` → `refresh_advisory`、`eTimeout` / `eNotReady` → `not_ready`、`eErrorOutOfDateKHR` → `swapchain_unavailable`、`eErrorSurfaceLostKHR` → `surface_unavailable`、と結果コードを意味へ翻訳し、`decideWindowWsiRecovery()`(87 行)が**どの呼び出し地点で観測したか**(`acquire` / `present` / `swapchain_create` など)と組にして行動を決めます。同じ `eNotReady` でも acquire なら 1 フレーム落とすだけ、準備呼び出しなら bounded retry — という違いを 1 か所に閉じ込めるための分け方で、`swapchainrecovery.hpp` 55〜58 行のコメントがその意図を書いています。`beginFrame()` 側はその行動を受けて、`drop_frame` なら `unavailable`(2106〜2115 行)、`replace_swapchain` なら次の swapchain 準備を予約して `output_out_of_date` を返します(2116〜2132 行)。`presentKHR` も同じ形で、成功扱いの `eSuboptimalKHR` は戻り値・エラーは例外として catch し(2462〜2506 行)、`proceed_and_refresh` なら**そのフレームは表示しつつ**次の作り直しを予約します(2185〜2188 行)。オフスクリーン側の `OffscreenFrameTarget::beginFrame()`(offscreenframetarget.cpp:149)は WSI を持たないぶん単純で、`waitForFences` の戻り値だけを同じ形で見て、非 `eSuccess` を `vk::to_string(fence_result)` 付きで throw します(162〜177 行)。これらを「成功=例外なし」と決めつけて値だけ使うと TIMEOUT / NOT_READY / SUBOPTIMAL を成功と誤読し、逆に「エラーは全部例外だから catch すれば十分」と決めつけると `eSuboptimalKHR` の作り直し要求を取りこぼします。

### vk::DrawIndexedIndirectCommand の各フィールド

`VkDrawIndexedIndirectCommand` は `vkCmdDrawIndexed` の引数をそのまま GPU 可読なメモリ上の構造体にしたもので、フィールドの順序と意味は仕様で固定されています。`indexCount` は描画するインデックスの個数、`instanceCount` はインスタンス数、`firstIndex` はインデックスバッファ内の開始位置(バイトではなくインデックス要素単位)、`vertexOffset` は `int32_t` でインデックス値に加算される頂点オフセット、`firstInstance` は最初のインスタンス ID です。`firstIndex` と `vertexOffset` はどちらも「オフセット」ですが軸が違い、前者はインデックスバッファ側、後者は頂点バッファ側を指します。集約初期化で 5 つの数値を並べて書くとこの順序を取り違えても型が通ってしまうため、読む側は仕様順を覚えておく必要があります。

**このリポジトリでは**: `makeDrawItemSnapshot()`(polygoninstancecontainer.cpp:256)が 5 項目を名前付きの `DrawIndexedArguments`(`index_count` / `instance_count = 1` / `first_index = index_offset` / `vertex_offset = vert_offset` / `first_instance = instance.index`)に保存し(274〜281 行)、`drawqueuebuilder.cpp` の `materialize()`(215 行)が同じ順で集約初期化して `vk::DrawIndexedIndirectCommand{...}` に変換します(217〜223 行)。model 登録(391 行)も hot-reload の rebuild(873 行)も同じ helper を通ります。名前付きの中間表を挟むことで 5 つの数値の並びを GPU 構造体の直書きまで持ち込まず、取り違えを防いでいます。とくに `first_instance = instance.index`(スロット番号がシェーダの `gl_BaseInstance` になる約束)を 1 か所に固定しているので、staging とリロードで別々に書いてずれることがありません。

### drawIndexed の vertexOffset はインデックス値に加算される

`vkCmdDrawIndexed` の `vertexOffset` は、仕様上「頂点バッファを引く前にインデックス値へ加算される値」です。インデックスバッファから読んだ値が `i` なら、実際に参照される頂点は `i + vertexOffset` になります。`int32_t` なので負値も取れます。これがあるおかげで、複数のメッシュを 1 本の頂点バッファと 1 本のインデックスバッファに連結して詰めても、各メッシュのインデックス値を 0 起点のまま書き換えずに済みます(連結時のずれは `firstIndex` と `vertexOffset` の 2 つで吸収する)。`firstIndex` はインデックスバッファ側の読み出し開始位置なので、両方を正しく設定しないと、別メッシュの頂点を引いた壊れた形状になります。

**このリポジトリでは**: `buildSpriteDrawData()`(spriterenderer.cpp:53)がチャンクごとに現在の索引数・頂点数を `chunk_first_index` / `chunk_vertex_offset` として記録し(59〜60 行)、同じ batch でも `vertex_offset` が変わる位置で run を分けて `SpriteDrawRun` に持たせます(66〜69 行)。`SpriteRenderer::render()` は `bindIndexBuffer(..., eUint16)`(170 行)の上で run ごとに `cmd_buf.drawIndexed(run.index_count, 1, run.first_index, run.vertex_offset, 0)` を発行します(178 行)。各チャンクの uint16 index を 0 起点のまま連結頂点バッファへ載せ、そのズレを `vertex_offset` で吸収しているので、全スプライトを 1 本の頂点 / 索引バッファにまとめても index を書き換えずに正しい頂点を引けます。

### drawIndirect の stride は構造体より大きくてよい

`vkCmdDrawIndexedIndirect` の `stride` は「連続する draw パラメータ間のバイト間隔」で、仕様の要件は `drawCount > 1` のとき「4 の倍数、かつ `sizeof(VkDrawIndexedIndirectCommand)` 以上」であることだけです。ちょうど `sizeof(VkDrawIndexedIndirectCommand)` である必要はありません。そのため、コマンド構造体を先頭メンバに持つ自前の構造体を配列にして、`stride` にその `sizeof` を渡す、というイディオムが使えます。GPU は各要素の先頭 20 バイトだけを読み、残りの CPU 側メタデータ(マテリアル ID など)は無視されます。`offset` の方も 4 の倍数である必要があり、またバッファは `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` 付きで作られていなければなりません。

**このリポジトリでは**: `drawqueuebuilder.hpp` の `struct RenderCommand` が先頭に `vk::DrawIndexedIndirectCommand command;` を置き、後ろに material / node_index などの CPU 側メタを足します(21〜28 行、`static_assert(offsetof(RenderCommand, command) == 0)` が「先頭 20 バイト=draw 引数」の前提を守ります)。`DrawQueueBuilder` は `.offset = segment.first_command * sizeof(RenderCommand)`、`.stride = sizeof(RenderCommand)` を組み(drawqueuebuilder.cpp:503〜511)、消費側の `materialrender.cpp` が `cmd_buf.drawIndexedIndirect(indirect_buf.buffer.get(), draw_call.offset, draw_call.draw_count, draw_call.stride)` を発行します(76 行)。GPU は各要素の先頭 20 バイトだけを読み残りのメタは無視するので、GPU が読む draw 引数と CPU 側の material 情報を同じ配列に同居させ、パス切替を跨いだ 1 本の indirect バッファで回せます。バッファは `createIndirectBuf()` が `eIndirectBuffer` 付きで確保します(polygoninstancecontainer.cpp:30)。

### multiDrawIndirect、drawIndirectFirstInstance、shaderDrawParameters(gl_BaseInstance)

`vkCmdDrawIndexedIndirect` の `drawCount` に 2 以上を渡すには、`VkPhysicalDeviceFeatures::multiDrawIndirect` が有効化されている必要があります(無効なら `drawCount` は 0 か 1 に限られます)。さらに `VkPhysicalDeviceLimits::maxDrawIndirectCount` が上限です。indirect command の `firstInstance` を 0 以外にするには `VkPhysicalDeviceFeatures::drawIndirectFirstInstance` が必要です。GLSL 側で `gl_BaseInstance` / `gl_BaseVertex` / `gl_DrawID` を読むのはさらに別の機能で、Vulkan 1.1 の `shaderDrawParameters`(元は `VK_KHR_shader_draw_parameters`)が要ります。feature は「物理デバイスが対応しているか」を照会したうえで、`vkCreateDevice` の `pEnabledFeatures` / `VkPhysicalDeviceFeatures2` で明示的に有効化しないと使えません(照会しただけでは有効になりません)。

**このリポジトリでは**: `core.cpp` の `queryDeviceFeatureSupport()`(219 行)が 3 feature を `getFeatures2` の chain で照会し、1 つでも欠ける device は `firstMissingRequiredVulkanFeature()` で選外にします(279〜280 行)。選ばれた device では `multiDrawIndirect` / `drawIndirectFirstInstance` / `vk11.shaderDrawParameters` をすべて true にして `createLogicalDevice()` の `vk::StructureChain` に載せます(350〜370 行)。`PolygonInstanceContainer::triggerUpdate()` は `limits.maxDrawIndirectCount` を build request に渡し、builder が同じ material の連続範囲を上限以下へ分割します(657〜660 行)。`first_instance = instance.index` がシェーダで `gl_BaseInstance` になり、`default.vert` の `pelicanObjects.objects[gl_BaseInstance].model`(29 行)で per-instance のモデル行列を引きます。この 3 点が揃わないと「1 回の間接描画で drawCount>1」「各 draw が別々の firstInstance を持つ」「シェーダがそれを読む」が成立せずバッチが崩れるため、照会だけでは有効にならない feature を明示 enable し、非対応 GPU は起動時に弾いています。

### bufferRowLength / bufferImageHeight の 0 は「詰めて配置」

`VkBufferImageCopy` の `bufferRowLength` と `bufferImageHeight` は、バッファ側のメモリを「もっと大きな 2D/3D イメージの部分領域」として扱うためのテクセル単位の指定です。仕様では「どちらかが 0 のとき、その軸については `imageExtent` に従って隙間なく詰まっている(tightly packed)とみなす」と定められています。つまり単純に「画像全体を幅×高さ×バイト数ぶんのバッファへそのまま吸い出す」場合は、両方 0 にしておくのが正しく、幅を入れる必要はありません。ここを実際の幅で埋めても同じ結果になりますが、パディング付きの行ピッチを扱うとき以外は 0 が定型です。`imageSubresource` の `aspectMask` / `mipLevel` / `layerCount` は別途明示が必要で、省略できません。

**このリポジトリでは**: `OffscreenFrameTarget::readbackLastFrameRGBA8()`(offscreenframetarget.cpp:240)が `bufferOffset` / `bufferRowLength` / `bufferImageHeight` をすべて 0 にし、`imageSubresource`(color / mip 0 / layer 1)と `imageExtent = {width, height, 1}` を明示します(255〜263 行)。`copyImageToBuffer` で `eTransferSrcOptimal` の color image を `extent.width * extent.height * 4` バイトのホスト可視ステージングへ吸い出し(268 行)、`readBuf` で `std::vector<uint8_t>` に返します。行パディング無しの密詰めで吸うので、返るバッファはそのまま連続した RGBA8 画素になり、ゴールデン画像テストやスクショが行ピッチ補正なしで比較・保存できます。ここが決定性ヘッドレス出力の最終段で、`bufferRowLength` を実幅で埋めても結果は同じですが、0 が最短の「詰めて配置」宣言です。

### cube イメージは「6 レイヤ + CUBE_COMPATIBLE フラグ」でしかない

Vulkan にキューブ専用のイメージ型はありません。`VkImageCreateInfo::arrayLayers = 6` の 2D イメージに `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` を立てると、そのイメージから `VK_IMAGE_VIEW_TYPE_CUBE` のビューを作れるようになる、という関係です。cube ビューは `layerCount == 6` かつ `baseArrayLayer` が 6 の倍数でなければならず、イメージ側も幅と高さが等しい必要があります。同じイメージから **面ごとの 2D ビュー**も作れるので、「ラスタ出力は面ごとの 2D ビュー、サンプリングは方向ベクトルの cube ビュー」という使い分けが 1 枚のイメージ上で成立します。逆に言えば、cube かどうかはイメージの**作成フラグとビューの型**の話であって、レイヤ配置は 2D 配列と同じです。

**このリポジトリでは**: 物理形状は `ImageResourceDimension`(`src/project/imageresourcedimension.hpp`、9〜15 行)の `two_d` / `cube` で表し、その直前のコメントが規範です。

> Physical image shape is independent from the view-family layout selected
> by the scheduler. A cube image may still expose 2D face views for raster
> output and a cube view for direction-space sampling.

生成側は `dimension == cube` で `array_layers != 6` または幅≠高さを例外にし(`src/core/renderingpass/rendertargetcontainer.cpp` 246〜254 行)、`eCubeCompatible` を立てます(280〜285 行)。ビュー側は `createImageView()`(72 行)が `eCube` に対して「cube-compatible イメージであること / `layer_count == 6` / `base_array_layer % 6 == 0`」の 3 条件を検査します(85〜94 行)。面ごとの 2D ビューは `createSequentialImageViews()`(128 行)がレイヤ数ぶん作り、レイヤ全体を 1 本で見る 2D array ビューは `createLayeredImageView()`(143 行)です。cube capture(環境マップ)がラスタ時は面ごと、サンプリング時は方向ベクトル、と同じ画像を 2 通りに見られるのはこの作り分けのおかげです。

### image view の subresource range(baseMipLevel / levelCount / baseArrayLayer / layerCount)

`VkImageViewCreateInfo::subresourceRange` は「イメージのどの部分を見るか」を切り取る窓です。`levelCount` に `VK_REMAINING_MIP_LEVELS`、`layerCount` に `VK_REMAINING_ARRAY_LAYERS` を渡すと「base から残り全部」を意味します。**アタッチメントとして使うビューは `levelCount == 1` でなければならず**、そのときのレンダーエリアは元 extent を `>> baseMipLevel` した実サイズ(最小 1)になります。2 つのビューが同じイメージの**重ならない**範囲を指していれば、同時に別用途で使えます。

**このリポジトリでは**: `ImageSubresourceRange`(`src/project/imagesubresource.hpp`:79)が `base_mip_level` / `level_count` / `base_array_layer` / `layer_count` に加えて `mip_count_mode`(`fixed` / `remaining`)を持ち、`resolveImageSubresourceRange()`(169 行)が **Vulkan のビューを作る前に必ず明示的な数へ解決**します(コメント: `remaining is an authored/runtime-relative count, resolved to an explicit count before a Vulkan image view is cached or created`)。範囲の妥当性は `validImageSubresourceRange()`(149 行)、重なり判定は `imageSubresourceRangesOverlap()`(189 行)です。生成したビューは `(range, dimension)` を鍵にキャッシュされます(`rendertargetcontainer.hpp` の `subresource_image_views` 36 行 / `attachment_subresource_image_views` 46 行)。レンダーエリアの縮小は `attachmentExtent()` が担当します。

```cpp
// render_pass_frame_setup.cpp:118-128
const auto mip = attachment.subresource->base_mip_level;
return { std::max(1u, base.width >> mip), std::max(1u, base.height >> mip) };
```

`remaining` をそのまま Vulkan へ渡さず先に解決するのは、キャッシュ鍵として比較するときに「同じ範囲なのに別の鍵」になるのを避けるためで、宣言側の相対表現と物理側の絶対値をこの 1 か所で切り離しています。

同じ「1 枚のイメージを複数の窓から見る」使い分けは render target の外にもあります(2026-09-04 追記): [VulkanXrCompositionGraphics](../../src/core/openxr/openxrcompositiongraphics.cpp#L48) は OpenXR が返す 1 枚の配列イメージから、`layerCount = 2` の 2D array ビューと、レイヤごとの `layerCount = 1` の 2D ビューを両方作ります。[ComputeTaskContainer::createDescriptorSet()](../../src/core/renderingpass/computetaskcontainer.cpp#L1022) と [MaterialDescriptorBuilder::build()](../../src/core/material/materialdescriptorbuilder.cpp#L442) も、`baseArrayLayer` / `layerCount` を変えたビューで同じイメージの別レイヤを descriptor へ載せます。

### mip 数の上限は `floor(log2(max(w,h))) + 1`

2D イメージが持てる mip の最大数は `floor(log2(max(width, height))) + 1` です。C++20 の `std::bit_width(n)` が `n > 0` のときちょうどこの値を返すので、`std::log2` を浮動小数で計算して丸め誤差を踏む定番のバグを避けられます。extent が変われば最大数も変わるため、「フルチェーンが欲しい」という宣言はイメージを作り直すたびに解決し直す必要があります。

**このリポジトリでは**: `maximumImageMipLevels()`(`src/project/imagesubresource.hpp`:44、中身は `std::bit_width(largest)`)と `resolveImageMipLevels()`(50 行)。宣言側は `ImageMipLevelCount{mode: fixed|full_chain, count}`(33 行)で、モード enum `ImageMipLevelMode`(16 行)に付く 14〜15 行のコメントが規範です。

> The authored count remains independent from a concrete output extent.
> full_chain is resolved again whenever the runtime recreates the image.

`fixed` で extent を超える数を書けば例外、`full_chain` は毎回 extent から解き直します。宣言→物理の伝播は `VulkanPhysicalResourcePlan::mip_levels`(`src/project/targetrenderplanning.hpp`:205)を経て `rendertargetcontainer.cpp` の `resolveImageMipLevels()` 呼び出し(255〜258 行)に着地します。ウィンドウを 1 px 広げただけで mip 段数が変わりうるので、この解決を「一度きりの定数」にしないことがリサイズ耐性の条件です。

### LAZILY_ALLOCATED メモリと TRANSIENT_ATTACHMENT(タイル常駐)

タイルベース GPU では、1 つのレンダリングパスの中でしか使わないアタッチメントを DRAM へ書き戻さず、タイルメモリに留めたままにできます。Vulkan ではこれを `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` と `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` の組で表現します。lazily-allocated メモリは実際には物理メモリが割り当てられないことがあり、アタッチメント以外の用途(サンプリング・コピー・storage)には使えません。デスクトップ GPU にはこのメモリタイプ自体が無いことも多く、その場合は普通のイメージへフォールバックする必要があります。

**このリポジトリでは**: `RenderTargetStorageMode`(`src/core/renderingpass/rendertargetstoragemode.hpp`:10〜14)の `transient_attachment` / `tile_local_attachment` が、確保時に `vk::MemoryPropertyFlagBits::eLazilyAllocated` を **preferred** として渡します(`rendertargetcontainer.cpp` 259〜269 行)。どの resource がそこへ落ちるかを決めるのは物理ターゲット計画で、条件は `tile_local_eligible`(`src/project/targetrenderplanning.cpp`:2173〜2191)にまとまっています — 読みが `same_pixel` であること、書きも読みもあること、attachment アクセスだけであること(storage / transfer / host が混ざらない)、single sample、`require_store` でないこと、など。1 つでも外れれば `materialized` に戻ります。この enum のコメントが「render-target allocator が実際に実装しているモードだけを渡す」ことを明言しており、計画側の表現(`VulkanResourceRepresentation`)とは意図的に別の型になっています。

### dynamic rendering local read(input attachment を dynamic rendering で使う)

従来 `VkRenderPass` の subpass でしかできなかった「同じピクセルの前段出力を input attachment として読む」を dynamic rendering でも可能にするのが `VK_KHR_dynamic_rendering_local_read` です。有効にすると (a) イメージレイアウト `VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR`、(b) `vkCmdSetRenderingInputAttachmentIndicesKHR` によるカラー/深度 → input attachment index の対応付け、(c) パイプライン側の `VkRenderingInputAttachmentIndexInfoKHR` が使えるようになります。同一レンダリングインスタンス内の前段→後段の依存は、イメージバリアではなく `VK_DEPENDENCY_BY_REGION_BIT` 付きのメモリバリア(= フレームバッファローカル依存)で表します。これは「タイルを跨がない読み方だけが許される」ことの表明で、`same_pixel` 以外の読みには使えません。

**このリポジトリでは**: 拡張の照会は `src/core/vkcore/core.cpp` 310〜322 行(拡張が列挙にあるときだけ `PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR` を chain して問い合わせる)、有効化は 427〜430 行(拡張名の追加)と 491〜510 行(feature を立て、非対応なら `unlink()` で chain から外す)です。**必須ではない optional feature** なので、無い環境でも起動します。コマンド側の設定は `VulkanManageCore::setRenderingInputAttachmentIndices()`(`core.hpp`:103、実装 `core.cpp`:774)、パイプライン側は `GraphicsPipelineRenderingLocalReadContract`(`src/core/shader/pipelinefactory.hpp`:27〜44)を `vk::RenderingInputAttachmentIndexInfoKHR` へ積みます(`pipelinefactory.cpp` 417〜421 行、非対応時は 455〜461 行で `unlink()`)。レイアウト遷移の stage/access は `render_target_layout_tracker.cpp` の `eRenderingLocalReadKHR` の枝(50 行・97 行)、融合 scope 内の依存が by-region なのは `RenderPassExecutor::renderingScopeDependency()`(`render_pass_executor.cpp`:459〜488)です。[第6章](06_rendering_vulkan_shader.md) §6.1 の「難所 — 論理グラフは値に版を打つ」で `same_pixel` が物理表現の決定入力だと書いたのは、最終的にこの拡張の制約に接続するためです。

### descriptor set と WriteDescriptorSet(binding 番号がシェーダとの契約)

Vulkan のシェーダは資源を `layout(set = S, binding = B)` で受け取ります。CPU 側はその (set, binding) に対して `VkWriteDescriptorSet`(`dstSet` / `dstBinding` / `descriptorType` / バッファまたはイメージの情報)を組んで `vkUpdateDescriptorSets` を呼び、set の形そのものは `VkDescriptorSetLayout` が、確保元は `VkDescriptorPool` が持ちます。**仕様上の位置**は Vulkan 仕様 "Resource Descriptors" 章(HTML 版のアンカー `#VkWriteDescriptorSet` / `#VkDescriptorSetLayoutBinding`)です。GLSL 側の数値と C++ 側の数値が一致しているかをコンパイラは検査しないので、両方から読まれる定数表を 1 つ置いてそれを唯一の出所にする、というのが定型になります。

**使用場所**: [pelican_sets.glsl の binding 定数](../../src/core/resources/shaders/include/pelican_sets.glsl#L9) / [FrameResources::configureViewCount()](../../src/core/renderer/frameresources.cpp#L110)(`writes[0].dstBinding = PELICAN_FRAME_UBO_BINDING` として同じ定数を使う) / [PolygonInstanceContainer::PolygonInstanceContainer()](../../src/core/renderer/polygoninstancecontainer.cpp#L240) / [ComputeTaskContainer::createDescriptorSet()](../../src/core/renderingpass/computetaskcontainer.cpp#L1022)

**このコードベースでの注意**: 定数表は GLSL のヘッダ側にあり、C++ からも同じマクロ名で読まれます。番号を動かすときは、シェーダと C++ の両方が同じヘッダを見ていることが前提です。

### descriptor binding flags と ePartiallyBound(descriptor indexing / bindless)

`VK_EXT_descriptor_indexing`(Vulkan 1.2 でコア入り)は、descriptor 配列を「実行時に決まる添字で引く」ための機能群です。`VkDescriptorSetLayoutBindingFlagsCreateInfo` に立てる `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` は「配列の一部の要素が未書き込みでも、シェーダがその要素を読まない限り有効」という緩和で、テクスチャ枠を N 個ぶん確保しておいて実際には数個しか埋めない bindless 構成には必須です。関連する緩和に `VARIABLE_DESCRIPTOR_COUNT` と `UPDATE_AFTER_BIND` があり、シェーダ側で非一様な添字を使うには `nonuniformEXT()` が要ります。**外部で引くときの名前**は Vulkan 仕様の "Descriptor Set Layout" 節と拡張名 `VK_EXT_descriptor_indexing` です。

**使用場所**: [rayMaterialDescriptorBindingFlags()](../../src/core/shader/pipelinefactory.cpp#L205)(3 binding のうちテクスチャ配列にだけ `ePartiallyBound` を立てる) / [PipelineFactory::descriptorSetLayoutKeysFor()](../../src/core/shader/pipelinefactory.cpp#L486)(ray レイアウトのとき set 3 の `binding_flags` にそれを積み、`vk::DescriptorSetLayoutBindingFlagsCreateInfo` として layout 作成へ渡す) / [makeMaterialAlphaAnyHitSource()](../../src/core/shader/raygrouplayout.cpp#L1232)(生成される GLSL 側の `nonuniformEXT(row.texture_index)`)

### descriptor type の使い分け(combined image sampler / storage image / sampled image + sampler)

同じイメージでも、シェーダからの触り方によって descriptor type が変わります。読み取りサンプリングは `COMBINED_IMAGE_SAMPLER`(イメージとサンプラを 1 つの descriptor に束ねる)、計算シェーダからの書き込みは `STORAGE_IMAGE`、そしてイメージとサンプラを別々の descriptor(`SAMPLED_IMAGE` + `SAMPLER`)に分ける形の 3 通りが基本です。分離型は「同じ画像を複数のサンプラで引く」「サンプラだけ差し替える」ときに効き、bindless なテクスチャ配列とサンプラ配列を独立に組める代わりに binding が 1 本増えます。**仕様上の位置**は "Descriptor Types" 節(`VK_DESCRIPTOR_TYPE_*` の列挙)です。

**使用場所**: [MaterialContainer::registerMaterial()](../../src/core/material/materialcontainer.cpp#L794)(`split_custom_samplers` のとき `eSampledImage` + `eSampler` の 2 binding へ割る) / [ComputeTaskContainer::createDescriptorSet()](../../src/core/renderingpass/computetaskcontainer.cpp#L1022)

### pNext チェーンと vulkan-hpp の StructureChain / unlink

Vulkan の構造体は `sType` と `pNext` を持ち、拡張機能ぶんの構造体を `pNext` で数珠つなぎにして 1 回の呼び出しに載せます。vulkan-hpp の `vk::StructureChain<A, B, C>` はこの連結を型で表現したもので、`get<B>()` で要素を取り出し、`unlink<B>()` で**実行時に**その要素を鎖から外せます(`relink<B>()` で戻せます)。外すことは「その機能を要求しない」という意味になるので、拡張の無い環境で同じコードを通す常套手段です。逆に鎖へ残したまま非対応のデバイスへ渡すと、検証レイヤに叱られるか、そのまま失敗します。

**使用場所**: [createLogicalDevice()](../../src/core/vkcore/core.cpp#L562)(feature 構造体を chain に積み、非対応なら `unlink()`) / [PipelineFactory::createGraphicsPipeline()](../../src/core/shader/pipelinefactory.cpp#L554) / [FrameResources::FramePublication](../../src/core/renderer/frameresources.cpp#L403)(descriptor write の `pNext` に acceleration structure 情報を足す)

### 機能照会(getFeatures2 の chain)と拡張関数の動的解決

「この GPU がその機能を持つか」は `vkGetPhysicalDeviceFeatures2` に feature 構造体を `pNext` で繋いで問い合わせます。chain 全体が一度に埋まるので、複数の拡張を 1 回で照会できます。重要なのは **照会しただけでは有効にならない**ことで、`vkCreateDevice` の `pNext` に同じ構造体を(true にして)渡して初めて使えます。拡張のエントリポイントは Vulkan のローダに静的リンクされていないことがあるため、`vkGetDeviceProcAddr` で取得し、null なら「その拡張は無い」と判断します(vulkan-hpp では dispatcher に取り込みます)。

**使用場所**: [queryDeviceFeatureSupport()](../../src/core/vkcore/core.cpp#L316)(照会側) / [createLogicalDevice()](../../src/core/vkcore/core.cpp#L562)(有効化側) / [VulkanManageCore::VulkanManageCore()](../../src/core/vkcore/core.cpp#L925)

**このコードベースでの注意**: 必須の feature が 1 つでも欠ける物理デバイスは選外にし、optional なもの(dynamic rendering local read など)だけ `unlink()` で落とします。「必須」と「あれば使う」がこの 2 つの経路で分かれています。

### VMA(Vulkan Memory Allocator)の usage と allocation flags

VMA は Vulkan のメモリタイプ選択とサブアロケーションを引き受ける定番ライブラリ(AMD 製、構造体名 `VmaAllocationCreateInfo`)です。`VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE` は「デバイスローカルを優先しつつ、必要なら他のヒープへ落ちてよい」という意図の宣言で、`VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT` は「CPU からは前へ前へと書くだけで読み返さない」という宣言です。この 2 つを組むと write-combined なホスト可視メモリが選ばれやすくなり、ステージングバッファを別に持たずに直接書けます(読み返すと極端に遅いのが write-combined の性質です)。budget 追跡(`VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT`)や buffer device address(`VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`)はアロケータ作成時に宣言が要ります。

**使用場所**: [createAllocator()](../../src/core/vkcore/core.cpp#L821) / [createIndexBuf()](../../src/core/model/vertbufcontainer.cpp#L29) / [createVertBuf()](../../src/core/model/vertbufcontainer.cpp#L38)

### buffer device address と usage flags

`VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress`(Vulkan 1.2 コア)を有効にすると、`vkGetBufferDeviceAddress` でバッファの GPU 仮想アドレスを 64bit 整数として取得でき、シェーダから `buffer_reference` でポインタのように辿れます。ただし**アドレスを取る予定のバッファは、作成時に `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` を立てておかなければなりません**(後から取ることはできません)。レイトレーシングの BLAS 入力に使うバッファはさらに `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR` が要ります。得たアドレスに整列要件があるときは `address % alignment` の余りから前方へ寄せるのが定型です。

**使用場所**: [geometryBuildInputUsage()](../../src/core/model/vertbufcontainer.cpp#L17)(capability があるときだけ 2 つの usage を足す) / [recordBuild()](../../src/core/renderer/accelerationstructure.cpp#L175) / [PipelineFactory::createRayTracingShaderBindingTable()](../../src/core/shader/pipelinefactory.cpp#L819)

### std140 / std430 レイアウト規則

GLSL の uniform ブロックは既定で **std140**、storage ブロックは **std430** のレイアウト規則に従います(GLSL 仕様の "Standard Uniform Block Layout" / "Standard Shader Storage Block Layout")。std140 では、スカラーは自身のサイズ、`vec2` は 8、`vec3` と `vec4` は 16 バイト境界に置かれ(`vec3` は**サイズ 12・アラインメント 16**)、**配列の要素と構造体は 16 バイトの倍数へ切り上げられます**。std430 はこの「配列・構造体を 16 へ切り上げる」規則だけが緩み、要素の素のアラインメントがそのまま使われます。C++ 側で同じバイト列を作るには `alignas` を明示し、`offsetof` の `static_assert` で固定するのが定型で、これを怠るとシェーダ側が 1 メンバぶんずれた値を読みます。

**使用場所**: [FrameUniformData](../../src/core/renderer/frameresources.hpp#L48)(型自体が `alignas(16)`。vec4 / mat4 のメンバに `alignas(16)`、`vec2` の 2 本には `alignas(8)`、後続の `uint32_t` 4 本には指定なしで自然に 4 バイト刻みに詰まり、直後の offset / `sizeof` の `static_assert` 群がその並びを凍結します) / [RayMaterialTableRow](../../src/core/shader/raygrouplayout.hpp#L405) / [RayProfileMaterialTableRow](../../src/core/shader/raygrouplayout.hpp#L417)

**このコードベースでの注意**: レイトレーシング側は「profile 有り/無しで別の行構造体」を用意し、profile が無い構成では以前の ABI をバイト単位で保つようにしてあります。同じ意味の行なのに型が 2 つあるのはそのためです。

### 頂点入力の binding / attribute / stride

`VkPipelineVertexInputStateCreateInfo` は 2 つの配列で頂点レイアウトを表します。`VkVertexInputBindingDescription` が「何番のバッファを 1 頂点あたり何バイト進めて読むか」(binding と stride)、`VkVertexInputAttributeDescription` が「その binding の何バイト目から、どの形式で、シェーダの `location` いくつへ渡すか」です。stride は頂点構造体の `sizeof`、offset は `offsetof` に一致していなければならず、ここがずれるとコンパイルも検証も通ったまま形状だけが壊れます。

**使用場所**: [CommonVertStruct](../../src/core/model/vertbufcontainer.hpp#L23)(`pos` / `normal` / `tangent` / `texcoord` / `color` が offset 0 / 12 / 24 / 40 / 48、`sizeof` 64 を `static_assert` で固定) / [VertBufContainer::getDescription()](../../src/core/model/vertbufcontainer.cpp#L645)(その offset から attribute description を組む) / [VertBufContainer::addPrimitiveAllocation()](../../src/core/model/vertbufcontainer.cpp#L390)

**このコードベースでの注意**: 順序は vec3 pos / vec3 normal / **vec4 tangent** / vec2 texcoord / vec4 color です(tangent が texcoord より前)。この 64 バイトの並びは、パイプラインの attribute description・シェーダの `layout(location=)`・レイトレーシング側が頂点バッファを生バイトとして引くときの word オフセット、という 3 か所の共通の出所になっています。

### multiview と view mask

`VK_KHR_multiview`(Vulkan 1.1 コア)は、1 回の描画で複数のビュー(VR の左右目など)へ同時に書くための機能です。`viewMask` の**立っているビットの位置**が、シェーダで読める `gl_ViewIndex` の取りうる値になります。連続する N ビューなら `(1u << N) - 1u` が定型のマスクですが、N が 32 のときは左シフトが未定義になるため `UINT32_MAX` を直接使う分岐が要ります。dynamic rendering では `VkRenderingInfo::viewMask`、パイプライン側では `VkPipelineRenderingCreateInfo::viewMask` に同じ値を置き、両者が一致していなければなりません。

**使用場所**: [validateGraphicsReflection()](../../src/core/shader/pipelinefactory.cpp#L250)(view_count が [2,32] であること、マスクが連続であること、シェーダが `gl_ViewIndex` を消費することを検査) / [PipelineFactory::createGraphicsPipeline()](../../src/core/shader/pipelinefactory.cpp#L554) / [FrameResources::selectMultiview()](../../src/core/renderer/frameresources.cpp#L651)

### sampler の filter と address mode

`VkSamplerCreateInfo` の `magFilter` / `minFilter` は拡大・縮小時の補間で、`VK_FILTER_NEAREST` はテクセルをそのまま読み、`VK_FILTER_LINEAR` は近傍を線形補間します。`addressModeU/V/W` は [0,1] の外を引いたときの折り返し方(repeat / clamp to edge / mirror / clamp to border)で、3 軸ぶん独立に指定できますが、3D テクスチャや cube では 3 軸を揃えないと軸ごとに違う挙動になります。`mipmapMode` と `minLod` / `maxLod` は別軸で、mip を持たないイメージでも指定自体は必要です。

**使用場所**: [createSampler()](../../src/core/renderingpass/computetaskcontainer.cpp#L716) / [ComputeTaskContainer::createDescriptorSet()](../../src/core/renderingpass/computetaskcontainer.cpp#L1022)

### レイトレーシング: shader group と SBT(handle size / stride / base alignment)

`VK_KHR_ray_tracing_pipeline` のパイプラインは、ステージ配列(`pStages`)とグループ配列(`pGroups`)の 2 段でできています。グループは `GENERAL`(raygen / miss / callable)と `TRIANGLES_HIT_GROUP`(closest hit + 任意の any hit)などの種類を持ち、各グループは pStages 内の添字を指します。**グループの並び順がそのまま SBT(Shader Binding Table)の並び順**になります。`vkGetRayTracingShaderGroupHandlesKHR` で取れる不透明ハンドルは 1 個あたり `shaderGroupHandleSize` バイト、SBT の 1 レコードは `shaderGroupHandleAlignment` に、raygen / miss / hit / callable の**各領域の先頭アドレスは `shaderGroupBaseAlignment`** に揃える必要があります(いずれも `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`)。`vkCmdTraceRaysKHR` に渡す 4 つの `VkStridedDeviceAddressRegionKHR` はこの stride で読まれます。

**使用場所**: [PipelineFactory::createRayTracingPipeline()](../../src/core/shader/pipelinefactory.cpp#L705)(stage 配列と group 配列を組む) / [PipelineFactory::createRayTracingShaderBindingTable()](../../src/core/shader/pipelinefactory.cpp#L819)(ハンドルを領域ごとに詰め、確保したアドレスの余りから base alignment へ寄せる) / [packRayTracingHitTable()](../../src/core/shader/raygrouplayout.cpp#L2294)

**このコードベースでの注意**: base alignment を満たすために `total_size + (base_alignment - 1)` を確保し、実アドレスの余りから内部オフセットを決めています。確保したバッファの先頭が揃っている保証は無い、という前提での書き方です。

### レイトレーシング: AS ビルドの入力とメモリ要件(maxVertex / instance 配列 / scratch)

BLAS の三角形ジオメトリ(`VkAccelerationStructureGeometryTrianglesDataKHR`)で間違えやすいのが `maxVertex` で、これは頂点数ではなく「**使用しうる最大の頂点インデックス**」、つまり密な配列なら `vertex_count - 1` です。プリミティブ数は三角形リスト前提で `index_count / 3` を `VkAccelerationStructureBuildRangeInfoKHR::primitiveCount` に渡します。TLAS 側では `VkAccelerationStructureInstanceKHR` の配列を**16 バイト境界**に置く必要があり、instance が 0 件でも「有効なアドレス」を渡さなければならない(null は不可)ため、1 件ぶんの空領域を確保しておくのが定型です。ビルド用の scratch バッファは `VkPhysicalDeviceAccelerationStructurePropertiesKHR::minAccelerationStructureScratchOffsetAlignment` に揃えたアドレスを渡します。

**使用場所**: [RayQueryAccelerationStructureScope::recordFrame()](../../src/core/renderer/accelerationstructure.cpp#L1139)(`vertex_count - 1` を maxVertex に、`index_count / 3` を primitiveCount に、instance 0 件でも 1 件ぶん確保) / [recordBuild()](../../src/core/renderer/accelerationstructure.cpp#L175)(scratch の alignment) / [allocateAddressedBuffer()](../../src/core/renderer/accelerationstructure.cpp#L116)

**このコードベースでの注意**: triangles data は名前付きでなく**位置**で集約初期化されているので、format / vertexData / vertexStride / maxVertex / indexType / indexData の順序を知らないと `vertex_count - 1` がどのフィールドか読めません。

### レイトレーシング: instance flags と any-hit シェーダ

TLAS の各 instance は `VkGeometryInstanceFlagsKHR` を持ち、`FORCE_OPAQUE` は「このインスタンスは不透明として扱う」= **any-hit シェーダを呼ばない**、`FORCE_NO_OPAQUE` はその逆です。両方を同時に立てるのは矛盾なので、宣言側で弾く必要があります。any-hit シェーダは交差**候補**ごとに(順不同で、同じ交差に対して複数回)呼ばれ、`ignoreIntersectionEXT` でその候補を棄却します。三角形の hit attribute は組み込みで `vec2` の重心座標(残り 1 成分は `1 - u - v`)、プリミティブ番号は `gl_PrimitiveID` です。したがってアルファ抜きを any-hit で実装している構成で `FORCE_OPAQUE` を立てると、抜きが丸ごと無効になります。

**使用場所**: [decodeRayInstanceFlags()](../../src/core/loader/componentcodec.cpp#L1649)(`force_opaque` と `force_no_opaque` の同時指定を `conflicting_flags` として拒否) / [validateRayProfileAccelerationStructureFlags()](../../src/core/renderer/accelerationstructure.cpp#L531) / [makeMaterialAlphaAnyHitSource()](../../src/core/shader/raygrouplayout.cpp#L1232)(`hitAttributeEXT vec2` を受け、cutoff 未満で `ignoreIntersectionEXT`)

### in-flight フレームと GPU 資源の寿命

GPU に投入したコマンドバッファは、対応する fence が signal されるまで「まだ実行中かもしれない」ものです。そのコマンドバッファが参照している資源(バッファ・イメージ・acceleration structure・SBT・descriptor set)は、その間は破棄も上書きもできません。CPU が先へ進むために同じ資源を N セット用意して添字で回すのが in-flight frame で、この N はスワップチェーンのイメージ枚数とは**別の数**です。破棄を「fence 完了後」まで遅らせる仕組み(遅延解放キュー)と、フレームが掴んだ資源を型を消して握っておく仕組み(lease)を組むのが定型になります。

**使用場所**: [Renderer::renderLogicalFrame()](../../src/core/vkcore/renderer.cpp#L4773) / [GpuSubmissionLease](../../src/core/vkcore/frametarget.hpp#L53) / [DeletionQueueCore::flushAll()](../../src/core/vkcore/deletionqueue.cpp#L88)

## 10.2 glTF / 3D アセット形式の仕様

### GLB のコンテナ構造(ヘッダ + JSON チャンク + BIN チャンク)

`.glb` は glTF 2.0 仕様が定めるバイナリコンテナで、先頭 12 バイトのヘッダ(magic / version / 全体長、いずれも little-endian の uint32)に続いて、可変個のチャンクが並びます。チャンクは 8 バイトのチャンクヘッダ(chunkLength / chunkType)+ チャンク本体という形で、magic は `0x46546C67`(ASCII の `glTF`)、JSON チャンクの型は `0x4E4F534A`(`JSON`)、BIN チャンクの型は `0x004E4942`(`BIN\0`)です。仕様上、**最初のチャンクは必ず JSON チャンク**でなければならず、BIN チャンクは 0 個または 1 個の任意です。したがって JSON 本体は常にファイル先頭から 20 バイト目(= 12 + 8)から始まり、この定数を直接書いてしまう実装がよくあります。各チャンクは 4 バイト境界にパディングされ、JSON チャンクは末尾を空白 `0x20`、BIN チャンクは `0x00` で埋める決まりで、chunkLength はそのパディングを含みます。BIN チャンクがある場合、JSON 側の buffer[0] は `uri` を持たず、この BIN チャンクの中身を指します。

**このリポジトリでは**: `src/devcli/distconfig.cpp` の `readGlbJsonChunk()`(469 行)が、`glb_magic` / `glb_json_chunk` 定数(35・37 行)で magic(オフセット 0)・version(4)・全体長(8)・JSON 長(12)・先頭チャンク型(16)を検証したうえで、オフセット 20 の生バイトだけを `nlohmann::json::parse` へ渡します(tinygltf は起動しません)。これは `glbContainsVat()` → `deriveVatEnabled()` から呼ばれ、参照先 GLB に `pelican.vat` extras があるかだけを見て配布ビルドの `PELICAN_WITH_VAT` を ON/OFF する判定に使われます。BIN チャンクが数 MB あっても JSON 先頭を舐めるだけで済むので、メタデータ確認のためにモデル全体をデコードしない割り切りです。

### buffer / bufferView / accessor の三層と byteStride

glTF の頂点データは 3 層に分かれています。`buffer` が生バイト列(GLB なら BIN チャンク)、`bufferView` がその中の連続した一区画(`byteOffset` / `byteLength` / 任意の `byteStride`)、`accessor` が bufferView に型を与えた読み取り口(`componentType` / `type` / `count` / bufferView 先頭からの相対 `byteOffset`)です。要素 i の先頭バイト位置は `bufferView.byteOffset + accessor.byteOffset + i * stride` で、**stride は `bufferView.byteStride` が定義されていればその値、未定義なら要素サイズ(= componentType のサイズ × type の成分数)**という規則になります。`byteStride` は頂点属性用の bufferView にのみ許され、インデックス用には置けないため、インターリーブ配置か否かは bufferView 側だけを見れば分かります。また accessor の offset は componentType のサイズの倍数でなければならない、という整列要件があるので、任意オフセットからの `reinterpret_cast` が仕様上は安全になっています。tinygltf の `Accessor::ByteStride(bufferView)` はこの「未定義なら密詰め」の場合分けをまとめた関数です。

**このリポジトリでは**: `src/core/model/gltf.cpp` の `getDataFromAccessor()`(611 行)が `p_data = buffer.data + accessor.byteOffset + bufferView.byteOffset`(685 行)を求め、`stride = accessor.ByteStride(buffer_view)`(651 行)を `readComponentByType()` に渡して `std::memcpy(&value, p_data + stride*i, element_size)` → `static_cast<OutType>(value)`(606〜607 行)と 1 要素ずつ読み進めます。POSITION / NORMAL / TANGENT からインデックスまで、頂点データはすべてこの 1 つのテンプレートを通ります。stride の決定を `ByteStride`(未定義なら要素サイズ)に委ねているので、インターリーブ配置でも密詰めでも同じコードで正しく読めます。stride を要素サイズ決め打ちにすると、複数属性を 1 本の bufferView に詰めたアセットで隣の属性を踏んで壊れます。`src/devcli/distconfig.cpp` の `parseVatBufferViews()`(627 行)は同じ buffer/byteOffset/byteLength の三層を生 JSON から読み直します。

### sparse accessor

`accessor.sparse` は「大半の要素が同じ値で、一部だけ違う」データを圧縮して持つための仕組みです。**仕様上の位置**は glTF 2.0 仕様(Khronos glTF 2.0 Specification)の Concepts 章 → Accessors 節 → "Sparse Accessors" の項で、`accessor.sparse` / `sparse.indices` / `sparse.values` というプロパティ名でそのまま引けます。`sparse.count` 個の差分があり、`sparse.indices`(bufferView + byteOffset + componentType)が上書きする要素番号を、`sparse.values`(bufferView + byteOffset)がその値を持ちます。ベースになる値は accessor 本体の `bufferView` から読み、**accessor に `bufferView` が無い場合はすべてゼロ**という扱いになるのがポイントで、「bufferView が無い accessor」は壊れているのではなく sparse 専用の正当な形です。indices の componentType は UNSIGNED_BYTE / UNSIGNED_SHORT / UNSIGNED_INT のいずれかで、値は狭義単調増加でなければなりません。用途としてはモーフターゲット(数万頂点のうち動くのは顔の一部だけ)が典型です。読み込み側から見ると通常の密なパスとは別のコードパスが要るので、対応せずエラーにする実装は珍しくありません。

**このリポジトリでは**: `src/core/model/gltf.cpp` の `readMorphDeltaAccessor()`(773 行)と `src/core/loader/vrmadecoder.cpp` の `readFloatAccessor()`(330 行)がどちらも `accessor.sparse.isSparse` を検査し、前者は morph target v1、後者は VRMA-C0 で非対応として throw します。両関数はモーフ位置差分と VRMA のアニメーションサンプラを、いずれも bufferView に連続配置された範囲として直接読む(`p_data + stride*i`)ため、sparse を通すには別の散布パスが要ります。対応を表明せず即エラーにすることで、sparse を含むアセット(bufferView 無し = 全ゼロを正当とする形)を静かに読んで無音で崩れるのを防いでいます。

### normalized 整数属性を float へ戻す規則(UNORM / SNORM)

`accessor.normalized` が true のとき、格納された整数は 0..1 または -1..1 に写像された固定小数点値として解釈されます。glTF 2.0 仕様が定める復号式は、unsigned byte が `f = c / 255.0`、unsigned short が `f = c / 65535.0`、signed byte が `f = max(c / 127.0, -1.0)`、signed short が `f = max(c / 32767.0, -1.0)` です。signed 側で `max(..., -1.0)` が付くのは、-128/127 や -32768/32767 が -1 をわずかに下回るためで、これは OpenGL / Vulkan の SNORM 変換と同じ扱いです(外部を当たるときの正式名は、Vulkan 仕様 Fundamentals 章の "Fixed-Point Data Conversions" 節、OpenGL 仕様なら同名の節です。glTF 2.0 側は `accessor.normalized` の項)。除数が 128 や 32768 ではなく 127 / 32767 なのは、0 と ±1 の両方を正確に表せるようにするためです。`normalized` は整数 componentType にのみ指定でき、FLOAT や 32bit INT、インデックス用 accessor には指定できません。したがって「整数のまま読んで static_cast する」実装は、normalized 属性が来た瞬間に 255 倍ずれた値を出すことになります。

**このリポジトリでは**: `getDataFromAccessor()` の `readComponentByType()`(595〜607 行)は `std::memcpy(&value, ...)` で読んだ生の整数を `static_cast<OutType>(value)` するだけで `/255` を掛けないため、UNORM 属性を渡すと 255 倍(short なら 65535 倍)ずれた値が出ます。エンジンはこの復号を丸ごと避けるため、モーフ差分やアニメーションを運ぶ経路では FLOAT しか読まず、`readMorphDeltaAccessor()`(780〜784 行)と `src/core/loader/vrmadecoder.cpp` の `readFloatAccessor()`(327〜329 行)が「非 normalized の FLOAT + 期待する type」を明示的に要求して弾きます。正規化整数属性は v1 の契約外なので、255 倍ずれた頂点を素通しするより load 時点でエラーにする方針です。

**2026-09-04 追記 — 復号そのものは実装されています。**上の「復号を丸ごと避ける」は現在のコードには当てはまりません。[normalizedIntegerComponent()](../../src/core/model/gltfaccessorreader.hpp#L70) が上記の復号式(unsigned は `value / max`、signed は `max(value / max, -1)`)をそのまま持ち、[getDataFromAccessor()](../../src/core/model/gltf.cpp#L1418) が `accessor.normalized` のとき [readNormalizedScalar()](../../src/core/model/gltfaccessorreader.hpp#L87) / [readNormalizedVector()](../../src/core/model/gltfaccessorreader.hpp#L110) へ分岐します。除数に `std::numeric_limits<InType>::max()`(127 / 255 / 32767 / 65535)を使うのが仕様どおりで、128 や 32768 ではありません。変わっていないのは `readComponentByType()` の側で、こちらは今も `static_cast` するだけなので、normalized な accessor を非正規化の読み口へ回すと 255 倍ずれます。分岐の入口で「normalized なのに行き先が整数」を例外にしているのはそのためです。モーフ差分と VRMA サンプラが FLOAT のみを要求する点は変わりません。

### node.weights と mesh.weights の優先順位

モーフターゲットの初期ウェイトは 2 か所に書けます。`mesh.weights` はそのメッシュ自身の既定値、`node.weights` はそのノードがメッシュを実体化するときの上書き値です。glTF 2.0 仕様では **node.weights があればそちらが mesh.weights に優先し、どちらも無ければ全ターゲットのウェイトは 0** と決まっています。同じメッシュを複数ノードから参照して、ノードごとに違う表情を初期値として与えられるのがこの二段構えの理由です。配列長はどちらもプリミティブの `targets` の個数と一致していなければならず、一致しない場合は不正なアセットです。実装側では「node 優先 → 無ければ mesh → 無ければ 0」という 3 段のフォールバックを書くことになります。

**このリポジトリでは**: `src/core/model/gltf.cpp` の `appendMorphDefaults()`(835 行)が `source = &mesh.weights` を既定にし(846 行)、ノードが非空の `weights` を持てば `source = &node.weights` へ差し替え(848 行、長さ != ターゲット数なら throw)、各ターゲットで `source->empty() ? 0.0 : source->at(target)` を `morph_target_layout->default_weights` へ積みます(860 行)。これがレンダラへアップロードする初期ウェイト行になります。node→mesh→0 の 3 段フォールバックのおかげで、同じメッシュを複数ノードから実体化してもノードごとに違う初期表情を与えられます。長さ検証は `meshMorphTargetCount()`(807 行、mesh 側)と `appendMorphDefaults()`(849 行、node 側)の両方にあり、不一致は不正アセットとして throw します。

### primitive.mode(トポロジ)と TRIANGLES 以外を扱わない実装が多い理由

`primitive.mode` は OpenGL の描画モード定数をそのまま使っていて、0=POINTS, 1=LINES, 2=LINE_LOOP, 3=LINE_STRIP, 4=TRIANGLES, 5=TRIANGLE_STRIP, 6=TRIANGLE_FAN です。**省略時の既定値は 4(TRIANGLES)**なので、多くのアセットには `mode` フィールドがそもそも現れません(tinygltf は未指定を -1 で表し、利用側で 4 に読み替える必要があります)。TRIANGLES 以外を切り捨てる実装が多いのは、スキニング・法線/接線の生成・三角形分割を前提にしたカリングや当たり判定など、パイプラインの下流がほぼ全部「3 頂点で 1 面」を前提にしているからです。加えて LINE_LOOP には対応するグラフィックス API のトポロジが無く、TRIANGLE_FAN は Vulkan のポータビリティサブセット(`VkPhysicalDevicePortabilitySubsetFeaturesKHR::triangleFans`)ではオプション機能で、Metal 系の環境では使えません。実務上もエクスポータはほぼ TRIANGLES しか吐かないため、対応コストに見合わないという判断になります。

**このリポジトリでは**: `src/core/model/gltf.cpp` 1673〜1674 行が `primitive.mode < 0`(tinygltf の未指定)を `TINYGLTF_MODE_TRIANGLES` に読み替えます。ローダは mode で描画トポロジを分岐させず、どのプリミティブも `src/core/shader/pipelinefactory.hpp`(40 行)の `topology = vk::PrimitiveTopology::eTriangleList` のパイプラインへ流します。三角形単位の処理をする 2 経路 — `pelican.vat`(1678 行)と VRM firstPerson auto の三角形分割(1693 行)だけが TRIANGLES を明示的に要求して throw します。スキニング・firstPerson 分割・VAT はどれも「3 頂点で 1 面」前提で、非 TRIANGLES が来ても triangle list として黙って誤描画されるため、間違えてはいけないその経路だけを弾く作りです。

### extensions / extensionsUsed / extensionsRequired と extras

glTF は「コア仕様 + 拡張」で成り立っていて、拡張データは各 JSON オブジェクトの `extensions` メンバの下に、拡張名をキーとして入ります。ルートの `extensionsUsed` には使用している拡張名をすべて列挙する義務があり、`extensionsRequired` はそのうち「これを理解できないならロードを諦めてほしい」ものだけを列挙します。裏を返すと、**`extensionsRequired` に入っていない拡張はローダが黙って無視してよい**(無視しても表示は成立する)という設計です。`KHR_materials_emissive_strength` はその典型で、コアの `emissiveFactor` が [0,1] に制限されていて HDR な発光を表現できない問題に対し、スカラー `emissiveStrength` を掛ける形で拡張します。無視すれば強度 1 倍として妥当に描け、理解すればより正しく描ける、という後方互換の作りになっています。なお `extensions` とは別に `extras` があり、こちらは登録も命名規約も無いアプリ固有の自由領域です。

**このリポジトリでは**: `src/core/model/gltf.cpp` は `emissive_strength` を `1.0f` 既定に置き、`material.extensions.find("KHR_materials_emissive_strength")`(1402・1472 行)が拡張オブジェクトを見つけたときだけ `emissiveStrength` を読んで `emissiveFactor` に掛けます(乗算は 1424〜1426 行など)。`KHR_texture_transform`(1452 行)も offset=0 / scale=1 / rotation=0 を既定にし、在れば上書きします。これが「無視すれば強度 1 倍・変換なしで妥当に描け、理解すればより正しく描ける」という後方互換そのものです。アプリ固有データ側は `tinyGltfValueToVatMeta()`(126 行)が `primitive.extras["pelican.vat"]` を読み、登録も命名規約も要らない `extras` にエンジン独自のメタ(schema / fps / frame_count / bufferView 番号など)を載せています。

### 行列から TRS を分解できない場合(skew / perspective 成分)

glTF のノード変換は `matrix`(列優先の 16 要素)か `translation` / `rotation` / `scale` のどちらか一方で書きます。仕様は matrix について「TRS に分解可能でなければならない」と定めていて、せん断(skew)や射影(perspective)成分を含む行列は不正です。またアニメーションの対象になるノードでは `matrix` を使ってはならず、TRS で書く必要があります。実装側で分解が必要になるのは、内部表現を TRS に統一する場合や、アニメーションと合成する場合です。定番の実装は `glm::decompose()` で、これは translation / rotation / scale のほかに **skew と perspective も出力引数で返す**ので、それらがゼロ(perspective は (0,0,0,1))かどうかを見れば「分解できたか」ではなく「TRS として表現しきれているか」を判定できます。関数の戻り値 false は特異行列などの失敗で、これとは別のチェックが要ります。併せて、glTF の rotation は `(x, y, z, w)` 順の配列なのに対し `glm::quat` のコンストラクタは `(w, x, y, z)` 順である点も定番の罠です。

**このリポジトリでは**: `src/devcli/gltfsceneextract.cpp` の `nodeTrs()`(102〜122 行)は、GLB のノード階層を決定的な `pelican.scene` JSON(MG-2b から version 2)へ書き出す経路の一部で、各ノードを `transformComponent(trs)` と `simplemodelview` 参照へ変換します(281 行〜)。`matrix` ノードは `glm::decompose()` に掛け、戻り値 false(特異行列)と `skew` / `perspective` の `epsilon = 1.0e-9` 超え(115〜118 行)の両方を検査し、超えたら「scene v1 の TRS では表現できない」として throw します。scene v1 は TRS だけの宣言的コンポーネントなので、表現できない行列を黙って落とさず拒否することで書き出しの決定性(round-trip)を守ります。135 行と `src/core/model/gltf.cpp` 896〜901 行の (x,y,z,w) → (w,x,y,z) 詰め替えは、`glm::quat` のコンストラクタが `(w,x,y,z)` 順である罠の回避です。

### VRM / VRMA が glTF 拡張として乗っていること

`.vrm` と `.vrma` はどちらも独自フォーマットではなく、**中身は普通の GLB** です。VRM 1.0 はルートの `extensions` に `VRMC_vrm` を置き、humanoid のボーン割り当て・表情(expressions)・視線(lookAt)・一人称表示(firstPerson)といった「人型としての意味付け」を持ちます。関連拡張として `VRMC_springBone`、`VRMC_node_constraint`、`VRMC_materials_mtoon` などが別々の拡張名で並びます。VRM 0.x は互換性のないスキーマを `VRM` という別キーで持つため、両者は別物として分岐する必要があります。VRMA(VRM Animation)は `VRMC_vrm_animation` 拡張で、glTF の `animations` 自体は普通のノードアニメーションのまま、拡張側が「どのノードがどの humanoid ボーン/表情/視線に対応するか」の対応表だけを与えます。つまりリターゲットに必要な意味情報が拡張側、数値データはコア側、という分担です。

**このリポジトリでは**: `src/core/model/vrmsemantic.cpp` の `decodeVrmSemantic()`(481 行)は `VRMC_vrm` を引き、無ければ `VRM`(0.x)の存在を info 診断として報告して素の glTF 扱いで返します(=.vrm でもメッシュとしては開けます)。見つかれば `specVersion == "1.0"` を要求し、`humanoid.humanBones` を辿ってノードと人型ボーンを対応付けます。`VRMC_node_constraint` はノード単位の `extensions` から読みます(352〜362 行)。VRMA は `src/core/loader/vrmadecoder.cpp`(579〜592 行)が `extensionsUsed` のエイリアス記載・ルート拡張・`specVersion == "1.0"` を要求したうえで、数値は `model.animations.front()`(コア側)を読み、`decodeSemanticMappings(extension, ...)` が「どのノードがどのボーン/表情か」の対応表(拡張側)を与えます。意味は拡張側・数値はコア側という分担なので、コアの glTF ロードを変えずに VRM/VRMA の意味層を加算的に載せられます。

### glTF の node.matrix は列優先の 16 要素

glTF 2.0 の `node.matrix` は 16 個の数値の配列ですが、**並びは column-major(列優先)**と仕様で決まっています(Concepts 章 → Nodes and Hierarchy 節)。つまり先頭 4 要素が第 1 列で、平行移動成分は添字 12・13・14 に来ます。GLM の `glm::mat4` も列優先なので、16 要素をそのまま流し込めば正しく入ります。row-major 前提の行列ライブラリへ同じ順で入れると転置されたものになり、回転と平行移動が入れ替わった見た目になります。`node.matrix` と TRS(`translation` / `rotation` / `scale`)は排他で、両方を持つノードは不正です。

**使用場所**: [InternalGltfLoader::nodeTransform()](../../src/core/model/gltf.cpp#L1489) / [InternalGltfLoader::nodeRestPose()](../../src/core/model/gltf.cpp#L1532)

### animation channel の出力 accessor の型は target_path で決まる

glTF のアニメーションは channel(何を動かすか)と sampler(いつ・どんな値か)の組でできています。channel の `target.path` が `"translation"` / `"scale"` なら sampler の output accessor は VEC3、`"rotation"` なら VEC4(四元数、`[x, y, z, w]` 順)、`"weights"` ならスカラーの列(モーフターゲット数 × キーフレーム数)です。つまり**出力の型は宣言されているのではなく target_path から決まる**ので、読み込み側は path ごとに期待する型を切り替えて accessor を検証します。input accessor は常に秒を表すスカラーの単調増加列で、補間は `STEP` / `LINEAR` / `CUBICSPLINE` のいずれかです(CUBICSPLINE は 1 キーあたり接線込みで 3 倍の要素を持ちます)。

**使用場所**: [InternalGltfLoader::loadAnimationClip()](../../src/core/model/gltf.cpp#L1564) / [readFloatAccessor()](../../src/core/loader/vrmadecoder.cpp#L320)

### JOINTS_0 / WEIGHTS_0(skin ローカルの joint index と重みの正規化)

スキンメッシュの頂点属性 `JOINTS_0` は 4 本ぶんの joint 番号、`WEIGHTS_0` はそれに対応する 4 つの重みです。ここで押さえるべきは 2 点あります。1 つ目、**JOINTS_0 の番号は `skin.joints` 配列の中の添字**であって、glTF のノード番号でも、複数の skin を 1 本のパレットへ詰めたときの絶対番号でもありません。複数 skin を同じ行列パレットに載せるなら、skin ごとの先頭オフセットを足して絶対番号へ直す必要があります。2 つ目、**重みは合計 1** でなければ、パレット合成が伸び縮みします(仕様も正規化を要求しています)。合計が 0 の頂点は「スキンされていない」ので、割り算をしてはいけません。

**使用場所**: [InternalGltfLoader::loadMesh()](../../src/core/model/gltf.cpp#L2258)(joint 番号の範囲検査と `joint_offset` の加算) / [VertBufContainer::addSkinnedPrimitiveAllocation()](../../src/core/model/vertbufcontainer.cpp#L451)(`sum > 0` のときだけ 4 成分を合計で割る)

## 10.3 C++ とビルドのイディオム

### RAII ハンドルラッパ(コピー禁止・ムーブのみ)

OS のハンドル(Win32 の `HANDLE`、POSIX の fd)を確実に閉じるための定番は、所有権を持つ小さなクラスで包み、デストラクタで解放する形です。コピーすると同じハンドルを二度閉じてしまうため、コピーコンストラクタとコピー代入を `= delete` し、ムーブのみを許す「ムーブ専用型」にします。ムーブは「相手から `release()` で所有権を奪い、自分の古い値を `reset()` で閉じる」の 2 操作で表現するのが標準的で、`release()` は閉じずに手放す、`reset(x)` は今持っている物を閉じてから `x` を引き取る、という役割分担になっています。`operator bool` を `explicit` にするのは、`if (h)` は書けるが `int n = h;` のような意図しない暗黙変換は起きないようにするためです(C++11 の文脈依存変換)。`std::unique_ptr` にカスタム deleter を付ける手もありますが、Win32 は失敗値が `NULL` の API と `INVALID_HANDLE_VALUE` の API の 2 系統あるため、両方を無効と扱う手書きラッパが好まれます。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の `UniqueHandle`(81〜109 行)。`runPlatformProcess()`(Windows 版、146 行)が `CreatePipe` / `CreateJobObjectW` / `CreateProcessW` の返す生 `HANDLE` をすべてこのラッパで包み、ムーブだけで所有権を渡します。子プロセス起動後に `stdout_write.reset()`(223 行)で書き込み端を明示的に閉じ、これで読み取りスレッドが EOF に到達します。`reset()` が `nullptr` と `INVALID_HANDLE_VALUE` の両方を「閉じない」と判定するため、`CreatePipe` 失敗系と `CreateProcess` 失敗系のどちらの無効表現でも二重クローズや不正 `CloseHandle` を避けられます。ここで一本でも漏らすと、ビルドやアセット変換のたびに子プロセスを起こす devcli でハンドルリークが積み上がります。

### pimpl と「デストラクタは .cpp で定義する」規則

`std::unique_ptr<Impl>` を持ち、`Impl` の定義をヘッダから隠す pimpl では、デストラクタをヘッダで暗黙生成させてはいけません。`std::default_delete<Impl>::operator()` は `sizeof(Impl) > 0` を静的表明しており、不完全型のまま実体化するとコンパイルエラーか未定義動作になります。そのためヘッダでは `~T();` と宣言だけし、`Impl` が完全型になった .cpp 側で `T::~T() = default;` と定義するのが定石です。さらに、デストラクタを自前で宣言するとムーブコンストラクタ・ムーブ代入の暗黙生成が抑止される(rule of five)ので、それらもヘッダで宣言し .cpp で `= default` する必要があります。ムーブ代入は古い pimpl を破棄するため、これも完全型が必要です。

**このリポジトリでは**: 宣言は `src/core/renderer/polygoninstancecontainer.hpp` の `StagedModelInstance`(206〜222 行、`struct Impl;` 前方宣言と 4 つの特殊メンバ宣言)、定義は `src/core/renderer/polygoninstancecontainer.cpp` の `Impl` 実体(297 行)の直後に並ぶ `= default` 群(309〜314 行)です。`Impl` は「1 インスタンス分の完全に確保済み候補」(`skin_palette` / `draw_items` / `morph_weight_frame` など重い状態)をまとめ、公開ヘッダには `unique_ptr<Impl>` 越しの不透明ハンドルだけを見せます。`stageModelInstance()` が値で返し、`publishModelInstance(StagedModelInstance staged)` が値で受けてムーブ消費する(コピーは `delete`)ため、これは「失敗しない公開点まで運ぶ一意所有トークン」です。`Impl` が完全型になる .cpp でデストラクタとムーブを `= default` するので、ヘッダを include する呼び出し側は `Impl` のレイアウトを知らずに済み、コンパイル依存が切れます。

### CRTP(奇妙に再帰したテンプレートパターン)による強い型付け

`struct Derived : Base<Derived>` のように、自分自身をテンプレート引数として基底に渡す書き方を CRTP(Curiously Recurring Template Pattern)と呼びます。基底が実体化される時点で `Derived` はまだ不完全型ですが、メンバ関数の本体とシグネチャは「使われたときに」実体化されるため、基底の中で `Derived` を型として書けます。これに空の派生クラスを組み合わせると、内部表現は同じ整数でも型としては別物になる「強い typedef(phantom type)」が作れ、別種のハンドル同士の取り違えをコンパイルエラーにできます。ハッシュ関数オブジェクトを基底の入れ子 `struct Hash` として持たせておくと、`std::unordered_map<H, V, H::Hash>` のように派生型ごとに使い回せます。

**このリポジトリでは**: `src/core/handle.hpp` の `BasicHandle<T, Base>`(9〜19 行、入れ子 `struct Hash` 付き)と、それを空派生させるマクロ `PELICAN_DEFINE_HANDLE`、適用例の `SoundHandle`(24 行)。`SoundHandle` は `std::uint64_t` を包む phantom type で、`src/core/audio/audio.cpp` の `playSound()` が `SoundHandle{next_handle++}`(439 行)で採番し、`src/core/audio/audio.hpp` の `std::unordered_map<SoundHandle, Voice, SoundHandle::Hash> voices`(33 行)のキーにします。`stopSound()` / `isPlaying()` はこのハンドルを `voices.find()` で引き、`Hash` は CRTP 基底の入れ子なので派生型ごとにそのまま使い回せます。内部表現が同じ `uint64` でも、`GameContext` 経由でユーザーのゲームコードが握る `SoundHandle` に別種の id(エンティティ id など)を取り違えて渡すと、型が別物なのでコンパイルエラーになります。

### 世代付きハンドル(index + generation)と SlotMap

配列の添字をそのままハンドルとして外に配ると、スロットが解放されて別のオブジェクトに再利用されたとき、古いハンドルが新しい住人を指してしまいます(ABA 問題)。定番の解法が「添字 + 世代番号」の組をハンドルにする方式で、スロットが死ぬたびに世代を +1 し、参照時に `generations[index] == handle.generation` を確認します。これで有効性判定が O(1) の整数比較 2 回で済み、解放済みハンドルは必ず弾かれます。空きスロットはフリーリスト(空き添字のスタック)で管理し、再利用時も添字は据え置き・世代だけ進めます。この構造はコミュニティでは slot map / generational index / generational arena と呼ばれ、EnTT のエンティティ ID(index + version)や Rust の `slotmap` クレートが同じ考え方です。世代 0 を「無効」に予約しておくと、ゼロ初期化されたハンドルが自動的に無効になります。

**このリポジトリでは**: `src/core/renderer/modelinstance.hpp` の `ModelInstanceId{index, generation, scene_epoch}` と、`src/core/renderer/modelinstanceslots.hpp/.cpp` の `ModelInstanceSlots`(`generations_` / `alive_` / `free_indices_`)。`isLive()`(modelinstanceslots.cpp:10)が「scene_epoch 一致 ∧ `alive_[index]` ∧ `generations_[index] == id.generation`」の 3 条件を O(1) で確認し、外れれば `requireLive()`(15 行)が `"stale ModelInstanceId"` を throw します。`retire()`(62 行)はスロットを殺すとき `generations_` を +1 して `free_indices_` に積み、`nextId()` / `publish()` が空き添字を据え置きのまま世代だけ進めて再利用します。`ModelInstanceId` は公開ハンドルとしてユーザーのゲームコードがフレームや(ゲームロジックの)ホットリロードをまたいで保持するので、スロットが別モデルに再利用された後に古いハンドルを渡しても世代不一致で必ず弾かれ、他人のインスタンスを黙って書き換える ABA 事故を防げます。

同じ形はモデル以外にもあります(2026-09-04 追記): [makeLogicalAssetId()](../../src/core/watch/reloadtransaction.cpp#L19) は index と generation を 64bit の上下 32bit へ詰めて 1 つの id にし、[liveRecord()](../../src/core/watch/reloadtransaction.cpp#L77) が世代不一致を stale として弾きます。フレーム側は [IFrameTarget::consumeFrame()](../../src/core/vkcore/frametarget.hpp#L280) が `expected_serial` を突き合わせて、前フレームのトークンで今フレームを終わらせることを防ぎます。index 空間が `uint32` で有限である以上、再利用のたびに世代を進める側にも枯渇検査が要る、というのは共通の注意点です。

### 単調増加する generation / epoch を「変わったか」の検出に使う

データ本体を突き合わせずに「前回見たときから変わったか」を判定したいとき、変更のたびに +1 される単調増加カウンタ(revision / generation / epoch、論理時計)を横に持たせるのが定番です。等値比較だけで陳腐化を検出でき、大小比較を使えば「古い更新の取りこぼし受理」や「同じ更新の二重適用」も拒否できます(受け取った revision が保持中の値以下なら重複とみなす)。粒度を変えた複数のカウンタを併用するのも普通で、「スロット単位」「レイアウト単位」「シーン全体」で別々に持てば、無効化の影響範囲を必要最小限にできます。有限幅なので折り返しへの備えが要り、0 を無効値に予約しているなら折り返し時に 0 を飛ばす、あるいは上限に達したスロットは再利用をやめる、といった扱いをします。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `frame_revision` による `duplicate_revision` 判定(985〜997 行)が代表例です。アニメフレームは単調増加する `frame.frame_revision` を持ち、`animation_revisions[index]` 以下なら本体(スキンパレット等)を突き合わせずに `duplicate_revision` を返して捨て、世代不一致なら `stale_generation` を返します。`resetSlot()`(503 行)はスロット再利用のたびに `animation_generations` を +1 し、`0`(無効値)へ折り返したらもう一度 +1 して飛ばします。シーン全体の破棄は `modelinstanceslots.cpp` の `clearPrepared()`(99 行)が `scene_epoch_` を +1 します。この 3 粒度(revision / slot generation / scene epoch)のおかげで、同じフレームの二重適用や古い更新の取りこぼし受理を等値・大小比較だけで拒否でき、同じ入力列なら同じ状態になる決定性を保てます。

### std::erase_if(erase-remove イディオムの置き換え)

C++20 で入った非メンバ関数 `std::erase_if(container, pred)` は、条件に合う要素を全部消して消した個数を返します。従来のシーケンスコンテナでは `v.erase(std::remove_if(v.begin(), v.end(), pred), v.end())` と 2 段階で書く必要があり(erase-remove イディオム)、`erase` を忘れると要素が残ったままサイズだけ変わらないという典型バグを踏みました。連想コンテナでは remove_if がそもそも使えない(`std::pair<const Key, T>` は代入できない)ため、以前はイテレータを回しながら `it = m.erase(it)` と書く必要があり、こちらも `erase_if` で 1 行になります。オーバーロードはコンテナごとに各ヘッダ(`<vector>` `<map>` `<unordered_map>` など)で定義されるので、使う側は該当ヘッダを include しておく必要があります。連想コンテナに渡す述語は `value_type`、つまりキーと値のペアを受け取るため、`entry.first` でキーを見る形になります。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `removeModelInstance()`(522 行、`std::vector<DrawItemSnapshot> draw_inventory` から消えたインスタンス由来の draw item を全消し)と `resetSlot()`(512 行、`unordered_map material_absolute_override_frames` から `entry.first.instance_identity == identity` の上書きフレームを削除)、`src/project/assetsmanifest.cpp` の `generateAssetsManifest()`(523 行、`std::map` の `cache`)。マニフェスト側は「今回のスキャンで生存が確認された鍵集合」に無い古い行を `entry.first.starts_with(prefix)` で namespace ごとに掃除します。シーケンスコンテナ(vector)と連想コンテナ(map / unordered_map)で書き方が違う削除を 1 行に統一でき、erase-remove の `erase` 忘れや、連想コンテナでのイテレータ無効化バグを避けられます。

### std::tie による多段キーの辞書式比較

複数のメンバを優先順位付きで比較する比較関数は、素直に書くと `if (a.x != b.x) return a.x < b.x; ...` の連鎖になり、条件を 1 つ書き間違えるとソート順が壊れます。`std::tie(a, b, c)` は左辺値参照のタプルを作るだけの関数で、`std::tuple` の比較演算子が先頭から辞書式に比べてくれるため、`std::tie(p.a, p.b) < std::tie(q.a, q.b)` と書けば多段キー比較が 1 行で正しく書けます(コピーは発生しません)。`std::sort` に渡す比較子は strict weak ordering である必要があり、辞書式比較はこれを自動的に満たす点も利点です。ヘッダは `<tuple>` です。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `uploadMaterialAbsoluteOverrides()`(1142〜1144 行)。マテリアル絶対上書きフレームへのポインタ配列を `std::tie(instance_identity, source_material_index)` の辞書式比較で 2 段ソートします。ソート後は同一インスタンスのレコードが連続するので、直後のループが `headers[instance-1]` に連続範囲(`record_offset` + `record_count`)を割り当てて GPU バッファへ詰められます(1157〜1162 行)。`tie` は左辺値参照タプルを作るだけでコピーがなく、tuple 比較が strict weak ordering を自動的に満たします。手書きの `if` 連鎖で条件を 1 つ書き間違えるとソート順が壊れ、レコードのグループ化とヘッダのオフセットが食い違って、GPU 側が別インスタンスの上書きを引いてしまいます。

### C++20 の指示付き初期化子(designated initializers)

`T{.a = 1, .c = 3}` のようにメンバ名を書いて集成体を初期化する構文で、C++20 で採り入れられました(C99 から来ていますが制約が強められています)。C++ では **宣言順に書かなければならず**、順序を入れ替えるとコンパイルエラーになります。位置指定との混在、`.a.b =` のような入れ子指定、配列添字の指定も許されません。省略したメンバは、既定メンバ初期化子があればそれ、なければ値初期化になるので、「書かなかったフィールドはゼロ相当」と読んで構いません。適用対象は集成体(ユーザ定義コンストラクタや private 非静的メンバを持たない型)だけです。読む側にとっての利点は、フィールドが 10 個ある構造体の初期化で「何番目の引数が何か」を数えなくて済むことです。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.cpp` の `makeDrawItemSnapshot()`(265〜293 行)が代表例で、`DrawItemSnapshot` / `DrawStableIdentity` / `DrawIndexedArguments` / `DrawPipelineMaterialKey` を一気に指示付きで組み立てます。とくに `DrawIndexedArguments{.index_count = ..., .first_index = primitive.index_offset, .vertex_offset = primitive.vert_offset, ...}` は、10.1 で触れた 5 フィールド(意味の違う 2 つの「オフセット」を含む)をすべて名前付きで埋めます。`MorphWeightFrame{...}`(373・505 行)なども同様です。位置引数だと順序を取り違えても型が通ってしまう構造体で、名前指定にすれば取り違えを防げ、宣言順必須なので並べ替えればコンパイルエラーとして順序ズレも検出できます。

### デフォルト化された operator<=>(三方比較)

C++20 の `auto operator<=>(const T &) const = default;` は、宣言順にメンバを辞書式比較する三方比較演算子を生成します。戻り値を `auto` にすると全メンバの比較カテゴリの共通型が推論され、整数メンバだけなら `std::strong_ordering` になります。重要なのは、`<=>` をデフォルト定義すると `operator==` も暗黙にデフォルト宣言されることと、`<` `>` `<=` `>=` は `(a <=> b) < 0` 等への書き換えで、`!=` は `==` の否定として自動的に使えるようになることです。つまりこの 1 行で 6 種類の比較演算子が揃います。`<compare>` のインクルードが必要です(忘れると比較カテゴリ型が未定義になります)。

**このリポジトリでは**: `src/core/renderer/modelinstance.hpp` の `ModelInstanceId`(16 行の `auto operator<=>(const ModelInstanceId &) const = default;` と 3 行の `#include <compare>`)。既定 `<=>` は `index` / `generation` / `scene_epoch` を宣言順に比較し、`operator==` も暗黙宣言します。この `==` を、`src/core/renderer/drawqueuebuilder.hpp` の `DrawStableIdentity`(88 行の `bool operator==(...) const = default;`)が入れ子メンバ `instance` の比較として使い、2 つの draw item が同じインスタンス/メッシュ/プリミティブ/ノードかをメンバ単位で判定します。この 1 行が無いと `DrawStableIdentity` の既定 `==` はコンパイルできず、手書きすれば 3 フィールドのうち `scene_epoch` を書き落として別 epoch の id を同一視する — stale 検出を壊す — バグを踏みやすくなります。

### std::vector<bool> の特殊化

`std::vector<bool>` は標準が規定する部分特殊化で、実装は 1 要素 1 ビットに詰めてよいことになっています。その代償として `operator[]` が返すのは `bool&` ではなくプロキシ型 `std::vector<bool>::reference` で、`bool *` を返す `data()` は存在せず、範囲 for で `for (auto &b : v)` と書くとコンパイルエラーになります(`auto` か `auto &&` なら通ります)。個々の要素にアドレスが無いため、他のコンテナと同じ「要素への参照・ポインタが取れる」前提が崩れる、というのが有名な落とし穴です。一方で、添字代入・`push_back`・`std::fill` はプロキシ経由で普通に動くので、ビット列として使う分には素直に使えます。要素のアドレスが必要なときは `std::vector<char>` や `std::deque<bool>`、固定長なら `std::bitset` に替えるのが定石です。

**このリポジトリでは**: `src/core/renderer/polygoninstancecontainer.hpp` の `model_history_valid` / `morph_history_valid`(232・248 行)、`src/core/renderer/modelinstanceslots.hpp` の `alive_`。どちらもインスタンスごとの 1 ビットのフラグ列として、`std::fill(..., true/false)`(polygoninstancecontainer.cpp:723・745・767 行)と添字代入(495・509 行)だけで扱い、要素のアドレスもポインタも取りません。`history_valid` が false のスロットは `triggerUpdate(const DrawQueueFramePlan &)` で `previous = current` にして(571〜578 行)テンポラル履歴(モーションベクタ)の初期状態を表し、`alive_` はスロット生存の可否そのものです。プロキシ参照の落とし穴(`bool&` や `data()` が取れない)を踏まない使い方に限定したうえで、インスタンス数ぶんのビット列をコンパクトに持てます。逆に「参照が要る」側の例が [InternalGltfLoader::headRelatedNodes()](../../src/core/model/gltf.cpp#L681) で(2026-09-04 追記)、ノードごとの判定結果を一度だけ計算してキャッシュするメモ化テーブルを `std::vector<std::uint8_t>` で持ち、その `const &` を返します。`std::vector<bool>` だと返せるのがプロキシになるため、真偽値の列でも `uint8_t` を選ぶのはこの理由です。

### std::filesystem の error_code オーバーロード(投げない版)

`<filesystem>` のほとんどの関数には 2 つのオーバーロードがあり、片方は失敗時に `std::filesystem_error` を投げ、もう片方は末尾の `std::error_code &` に結果を書いて **投げません**(こちらは `noexcept`)。`error_code` 版は成功時に `ec.clear()` するので、同じ `ec` を使い回すときは前回の値が残らない一方、自分で `clear()` して再利用しているコードもあります。`is_directory` / `is_regular_file` のような述語は、エラー時に `ec` を設定した上で `false` を返すので、失敗と「存在しない」を区別したければ `ec` を見る必要があります。ディレクトリ走査も同様で、`recursive_directory_iterator` はコンストラクタと `increment(ec)` に error_code 版があり、権限の無い 1 ディレクトリで走査全体を落とさずに続行できます。`last_write_time` が返す `file_time_type` のエポックは実装定義なので、`time_since_epoch().count()` は「変化検出用の不透明なトークン」としてのみ使うのが安全です。

**このリポジトリでは**: `src/project/assetsmanifest.cpp` の `scanStore()`(198〜303 行)が `is_directory(store_root, ec)`(204 行)、`recursive_directory_iterator` の `ec` 版コンストラクタ(216 行)、`it.increment(ec)`(233 行)、`entry.is_regular_file(entry_ec)` / `entry.file_size(entry_ec)` と、投げない版を通して `ec` を見ます。`tolerate_errors` が真のとき(寛容スキャン経路)、権限の無い 1 エントリで走査全体を throw で落とさず、warning を積んで続行します。`fileMtime()`(149 行)は `last_write_time(path, ec)` の `time_since_epoch().count()` を文字列化してハッシュキャッシュのキーに使います。エポックが実装定義の `file_time_type` を「変化検出用の不透明トークン」としてのみ扱うので、OS/ファイルシステムをまたいでも移植性が保てます。

### プラットフォーム分岐の #ifdef で、同名関数を 2 回定義する

`#ifdef _WIN32 ... #else ... #endif` で挟んで同じ名前・同じシグネチャの関数を 2 回書くのは、プラットフォーム抽象の最も素朴な形です。プリプロセスの時点で片方しか残らないので、これは多重定義でもオーバーロードでもありません。呼び出し側は 1 種類の宣言だけを見ればよく、分岐はこのファイルの中で閉じます。読むときの注意は、`#else` と `#endif` が数百行離れることがあり、いま読んでいる行がどちらの枝かを見失いやすいこと、そして clangd や IDE は通常「有効な枝」しか索引化しないため、非有効側の定義には定義ジャンプで飛べないことです。判定マクロについては、MSVC / MinGW とも 32bit・64bit の両方の Windows ターゲットで `_WIN32` が定義され、64bit ではこれに加えて `_WIN64` も定義される、という関係を覚えておくと迷いません。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の `readPipe()`(Windows 138 行 / POSIX 268 行)と `runPlatformProcess()`(Windows 146 行 / POSIX 277 行)が `#ifdef _WIN32` / `#else` で 2 回ずつ定義され、`runProcess()`(390 行)は分岐なしで `runPlatformProcess()` を呼ぶだけです。中身は Windows 版が `CreateProcessW` + Job オブジェクト、POSIX 版が `fork`/`execv` + プロセスグループと全く別実装ですが、呼び出し側は 1 種類のシグネチャしか見ません。プリプロセスで片方しか残らないので多重定義にはならず、OS 差はこの 1 ファイルに閉じ込められて devcli の残りは OS を意識せずに済みます(移植時もこのファイルだけを見ればよい)。

### windows.h を include する前の NOMINMAX

`<windows.h>`(実体は `minwindef.h`)は `min` と `max` を関数形式マクロとして定義します。これが `std::min` / `std::max` はもちろん `std::numeric_limits<T>::max()` まで壊すため(`max()` がマクロ展開されて引数の数が合わなくなる)、include より前に `NOMINMAX` を定義してマクロ定義自体を抑止するのが定番の対処です。順序が意味を持つので、`#define NOMINMAX` は必ず `#include <windows.h>` より上に書きます。同じ目的で `WIN32_LEAN_AND_MEAN` を定義して取り込むヘッダを減らすこともよく併用されます。ビルドシステム側で `-DNOMINMAX` を渡している環境と衝突しないよう、`#ifndef NOMINMAX` で囲む書き方も一般的です(同一内容の再定義は本来無害ですが、警告を避ける意図です)。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の `#define NOMINMAX`(12 行)→ `#include <windows.h>`(13 行)。この後 `runPlatformProcess()` などが `std::chrono` や `std::numeric_limits` を使うため、`max()` マクロを潰しておかないと Windows ビルドだけがコンパイルエラーになります。ビルドシステムが `-DNOMINMAX` を渡す環境と衝突しないよう `#ifndef NOMINMAX` で囲む版は `src/core/loader/imageloader.cpp`(17〜18 行)や `src/core/os/window.cpp`(10〜11 行)にあり、いずれも `windows.h` の include より必ず上に置かれます(順序が意味を持つため)。

### 機能フラグをマクロ「値」で渡し、#ifdef ではなく #if で見る

ビルドオプションでサブシステムを丸ごと外す構成では、マクロを「定義する/しない」ではなく「0 か 1 の値として常に定義する」流儀があります。CMake の `target_compile_definitions(tgt PUBLIC FOO=$<BOOL:${FOO}>)` はまさにこれで、OFF のときも `FOO=0` が渡ります。したがってソース側は `#ifdef FOO` では **常に真になってしまい**、必ず `#if FOO` と書かなければなりません。この流儀の利点は、名前の綴り間違いを `-Wundef` で検出できることと、定義漏れと OFF を区別できることです。もう一点の勘所は `PUBLIC` と `PRIVATE` の使い分けで、フラグがヘッダの中身(宣言されるメンバや関数)を変える場合、利用側にも同じ値が伝わらないと ODR 違反や ABI 不一致になるため `PUBLIC` にする必要があります。逆に .cpp の中だけで効くフラグは `PRIVATE` で十分です。

**このリポジトリでは**: `src/core/CMakeLists.txt` の `target_compile_definitions(pelican_core PUBLIC PELICAN_WITH_AUDIO=$<BOOL:${PELICAN_WITH_AUDIO}> ...)`(6〜15 行)と、`PELICAN_ENGINE_BUILD=1` などの `PRIVATE` 側(17〜20 行)。OFF でも `PELICAN_WITH_AUDIO=0` が常に届くので、参照側は `#ifdef` ではなく `#if` で見ます。`src/core/appflow/loop.cpp` の `#if PELICAN_WITH_AUDIO`(260 行)/ `#if PELICAN_WITH_PHYSICS`(254 行)は Audio・PhysWorld モジュールの生成そのものを丸ごと囲み、`src/core/persistence/persistence.hpp` の `#if PELICAN_WITH_AUDIO`(42 行)は `applyAudioSettings` 等の**メンバ宣言**を囲みます。後者のようにヘッダの中身が変わるフラグは、利用側にも同じ値が届かないと ODR 違反や ABI 不一致になるため `PUBLIC` にする必要があり、.cpp 内だけで効く `PELICAN_ENGINE_BUILD` は `PRIVATE` で足ります。

### thread_local + RAII による「今このスレッドが実行中のもの」

「いま実行中のトランザクション」「いま登録中のオブザーバ」のような、呼び出しの深いところまで届けたい文脈を、引数で引き回さずに参照するためのイディオムです。名前空間スコープの `thread_local` なポインタに現在値を置き、その差し替えを RAII クラス(コンストラクタで `std::exchange` して旧値を保存、デストラクタで書き戻す)に閉じ込めます。RAII にしておけば例外で抜けても必ず前の値に戻り、入れ子も正しく動きます。`thread_local` なのでスレッドごとに独立し、別スレッドの実行中トランザクションを踏むことがありません。素の大域変数 + 手書きの復元にすると、途中の `return` や例外で復元を飛ばした瞬間に「終わったはずのトランザクションが実行中に見える」壊れ方をします。

**使用場所**: [PendingSceneTransactionScope](../../src/core/loader/scene.cpp#L225) / [thread_local な active_effect_transaction の宣言](../../src/core/loader/effecttransaction.cpp#L20) / [ScopedRenderPipelineCompanionReloadObserver のコンストラクタ](../../src/core/watch/reloadservice.cpp#L24)

### スコープガード(巻き戻しをデストラクタに置き、成功時に無効化する)

「途中で失敗したら、それまでの副作用を取り消す」を例外安全に書く定型です。ローカルなガード型のデストラクタに巻き戻し処理を書き、正常系の最後で「もう巻き戻さない」印(commit / dismiss と呼ばれることが多い)を立ててガードを無効化します。こうすると、途中のどの `throw` からでも巻き戻しが走り、成功経路でだけ走りません(`std::scope_exit` / `scope_fail` は C++ 標準ライブラリ拡張の同種の道具です)。ガードに「そもそも何もしていない」状態を表す番兵(世代番号が 0、ハンドルが無効など)を持たせて、staging 前に抜けたときに巻き戻しを飛ばす形もよく使われます。

**使用場所**: [LogicalFrameAbortGuard](../../src/core/vkcore/renderer.cpp#L3019) / [StagedRuntimeRollbackGuard](../../src/core/vkcore/renderer.cpp#L3046) / [ConstructionScope](../../src/core/container.hpp#L174)(構築中スタックの pop を必ず走らせる) / [VertBufContainer::addPrimitiveAllocation()](../../src/core/model/vertbufcontainer.cpp#L390)(`catch (...)` で確保済みの範囲を返す)

### double-checked locking と acquire / release

「初回だけ高コストな初期化をし、以降はロックなしで読みたい」ときの定型です。まずロックを取らずにフラグを読み、未初期化ならロックを取ってから**もう一度**読み、まだなら初期化する、という二重検査になります。ここで肝心なのがメモリ順序で、フラグを `memory_order_release` で書き、`memory_order_acquire` で読むことで「フラグが true に見えた時点で、初期化中に書いた内容もすべて見える」ことが保証されます。`relaxed` にすると、フラグだけ先に見えて中身が未完成のオブジェクトを掴む、という再現しにくいバグになります。C++11 以降の関数内 static 変数はこれを言語側が保証するので、自前で書くのは「初期化の場所を自分で決めたい」場合に限られます。

**使用場所**: [FastModuleContainer::get()](../../src/core/container.hpp#L149)(acquire で読み、`scoped_lock` の内側で再検査し、最後に release で `__ready()` を立てる)

### compare_exchange による所有権の一度きりの確定

「後始末をする権利は 1 回しか行使されてはならない」を、ロックを取らずに表す方法です。`std::atomic<T*>` に所有者を入れておき、`compare_exchange_strong(expected, nullptr)` が成功した側だけが後始末をします。CAS(compare-and-swap)は成功したスレッドが 1 つだけであることを保証するので、二重解放も取りこぼしも起きません。メモリ順序に `acq_rel` を使うのは、勝った側が相手の書き込みを見るためです(成功側は release + acquire。失敗時の順序は**別の引数**で指定します — 4 引数版の第 4 引数、つまり 2 つ並ぶメモリ順序引数の後ろの方で、store を伴わないので `acquire` 以下しか渡せません)。

**使用場所**: [FrameTargetFrameCleanup](../../src/core/vkcore/frametarget.hpp#L55)(`detach()` の `compare_exchange_strong(expected, nullptr, acq_rel, acquire)`)

### ビットパッキングとビットマスクのイディオム

2 つの 32bit 値を 1 つの 64bit に詰める `(hi << 32) | lo` は、単なる節約ではなく「2 つのキーの辞書順比較を、整数 1 回の比較で行う」ための表現です(上位に来た値が優先される)。復元はシフトとマスクで、詰める前に両方が 32bit に収まることを確かめておかないと静かに桁が落ちます。関連する定型が `(1u << n) - 1u`(下位 n ビットを立てる)で、n がビット幅と同じときは左シフトが未定義になるため別扱いが要ります。フラグ集合では「同時に立ってはいけない組み合わせ」を `(x & A) != 0 && (x & B) != 0` の形で検査するのが定石です。

**使用場所**: [sceneBehaviorAttachmentSeq()](../../src/core/gamelogic/behaviorarena.cpp#L72)(32bit を超える入力を `length_error` で弾いてから詰める) / [refreshSceneAttachmentSequence()](../../src/core/gamelogic/behaviorarena.cpp#L442) / [validateGraphicsReflection()](../../src/core/shader/pipelinefactory.cpp#L250)(view_count 32 のときだけ `UINT32_MAX` へ分岐) / [decodeRayInstanceFlags()](../../src/core/loader/componentcodec.cpp#L1649)(相反フラグの検査)

### shared_ptr / weak_ptr による寿命の「リース」

`std::shared_ptr<const void>` は、指している先の型を隠したまま「まだ誰かが使用中である」ことだけを表せる券です(型消去した参照カウント)。破棄したい側は、券が全部返るまで実際の解放が起きないことに頼れます。一方 `std::weak_ptr` は「所有はしないが、まだ生きていれば触りたい」という意図の表明で、`lock()` が空を返せば相手はもう居ない、と分かります。所有グラフに循環があるとき、片方を weak にして断ち切るのが古典的な用法ですが、ここでの主眼は循環の回避よりも「観測はするが寿命は延ばさない」という意思表示です。

**使用場所**: [GpuSubmissionLease](../../src/core/vkcore/frametarget.hpp#L53)(型消去した提出リース) / [FrameTargetFrame](../../src/core/vkcore/frametarget.hpp#L90) / [RetirementBatch](../../src/core/vkcore/deletionqueue.hpp#L50)(`weak_ptr` の配列で束を観測する)

### 参照カウンタ付きの遅延解放と、受付/排出を分ける状態機械

GPU 資源のように「今すぐには壊せない」ものを溜める破棄キューでは、2 つの考え方が要ります。1 つは**参照カウンタ**で、型消去した要素に仮想 `release()` を持たせたうえで、各要素が共有のカウンタを +1 して生まれ、解放時に -1 する。カウンタが 0 になった瞬間が「この束は全部片付いた」です(仮想関数だけだと「まだ何件残っているか」を数えるのに全要素を舐める必要があり、共有カウンタはそれを O(1) の整数読みにします)。もう 1 つは**受付と排出の分離**で、「新規の受付を止める(accepting = false)」と「溜まったものを流し切る(draining)」は別の遷移です。終了処理では必ず受付を止めてから流し切る順にしないと、排出中に積まれた要素が取り残されます。

**使用場所**: [PendingCounter](../../src/core/vkcore/deletionqueue.hpp#L18) / [RetirementBatch](../../src/core/vkcore/deletionqueue.hpp#L50) / [accepting / draining のフラグ](../../src/core/vkcore/deletionqueue.hpp#L78) / [DeletionQueueCore::flushAll()](../../src/core/vkcore/deletionqueue.cpp#L88)

### moved-from 状態の契約

C++ のムーブは、移動元を「有効だが未規定の状態」にします。標準ライブラリの型なら破棄と再代入は必ず安全ですが、それ以外に何が保証されるかは型ごとの契約です。自前のトークン型では、この未規定さを**能動的に使う**ことがあります — ムーブされた側を「もう commit してはいけない」印にして、二重コミットを型の側で潰す、という書き方です。読む側から見ると「ムーブしたあとに何もしていないのに状態が変わる」ように見えるので、契約がコメントか名前に書かれていないと読めません。

**使用場所**: [ProjectBasicConfig::importSceneDocument()](../../src/core/loader/basicconfig.cpp#L804)(トークンを move して以降を無効化する)

### 型消去: std::any と any_cast

`std::any` は「任意の 1 個のコピー可能な値」を型を隠して保持する箱で、取り出しは `std::any_cast<T>` です。型が違えば `std::bad_any_cast` を投げます(ポインタ版は null を返します)。異なるコーデックが返す異種の値を 1 つのコンテナで運ぶような場面で、共通の基底クラスを作らずに済ませる手段になります。取り出し側は必ず失敗しうるので、`bad_any_cast` をその境界の例外契約(たとえば `std::invalid_argument`)へ翻訳しておくと、呼び出し側が 1 種類の失敗だけを見ればよくなります。

**使用場所**: [codecValue()](../../src/core/loader/componentcodec.cpp#L422)(`any_cast` の失敗を `invalid_argument` に翻訳) / [prepareSceneBindings()](../../src/core/loader/scene.cpp#L305)

### Result 型で例外型を境界の外に出さない

モジュールの内側では例外で失敗を運びつつ、外向きの API では成功/失敗を値(`Result` 型)で返す、という二層構造です。境界の関数が内部例外だけを `catch` して `Result::failure()` に畳むことで、呼び出し側が実装都合の例外型を知らずに済みます。重要なのは「どの例外を捕まえるか」で、内部の型だけを捕まえて `std::exception` や `...` を素通しにするか、あるいは全部畳んで種類を失うか、を意識的に決める必要があります。前者なら想定外のバグは伝播し、後者ならすべてが「失敗」に見えます。

**使用場所**: [componentcodec.cpp の decode ラッパテンプレート](../../src/core/loader/componentcodec.cpp#L56)

### std::variant + std::visit + if constexpr のディスパッチ

`std::variant` の中身で分岐する定型は、`std::visit` に総称ラムダを渡し、その中で `using T = std::decay_t<decltype(value)>;` として `if constexpr (std::same_as<T, X>)` で枝を書く形です。`std::visit` は全ての候補型を扱えることをコンパイル時に要求するので、型を足したときに「その枝を書き忘れた」がコンパイルエラーになります(`switch` の default と違って取りこぼせません)。`std::monostate` は「値が入っていない」を表すための空の型で、必ず別扱いの枝が要ります。総称ラムダの中で `if constexpr` を使うと、偽の枝は実体化されないので、その型に無いメンバを参照する枝も書けます。これは同じ理由でテンプレート引数ごとのコード生成一般に使われ、依存名の中でメンバーテンプレートを呼ぶときは `obj.template f<...>()` の `.template` 修飾が要ります(パーサに「これはテンプレート」と教えるため)。

**使用場所**: [numericRange()](../../src/core/imgui/inspector.cpp#L38) / [freezeJournalOperation()](../../src/core/communication/editorjournal.cpp#L1958) / [PipelineFactory::rebuildPrepared()](../../src/core/shader/pipelinefactory.cpp#L1878) / [appendBehaviorEventHandlerAt()](../../src/core/userpublic/details/behavior/registerer.hpp#L137)(`if constexpr` の枝と `lookup.template operator()<Index>()`)

### fold 式と std::apply(タプルライク型の全要素処理)

C++17 の fold 式は、パラメータパックを 1 つの式に畳み込みます。`(f(args), ...)` はカンマ fold で、**左から右へ順に評価されることが規格で保証されます**(通常の関数引数の評価順は未規定なので、順序が意味を持つ処理はこちらで書く必要があります)。`(pred(args) && ...)` は全称判定で、短絡も効きます。要素が `std::tuple` に入っているときは `std::apply(f, t)` で全要素を一度に受け取り、その中で fold にかけるのが定型です。スキーマ宣言のように「宣言順が結果の順序になる」処理は、この評価順の保証に依存しています。

**使用場所**: [collectBehaviorEventHandlers()](../../src/core/userpublic/details/behavior/registerer.hpp#L148)(カンマ fold で全 Index を順に処理) / [materializeSchemaNode()](../../src/core/userpublic/details/schema/structfieldjson.hpp#L972)(`std::apply` + fold) / [decodeStructObject()](../../src/core/userpublic/details/schema/structfieldjson.hpp#L754) / [materializeBehaviorParamsSchemaFingerprint()](../../src/core/userpublic/details/behavior/registerer.hpp#L184)

### consteval とコンパイル時の throw

C++20 の `consteval` 関数は必ず定数評価されます。定数評価の文脈では `throw` に到達できない(到達すると定数式でなくなる)ため、`if (bad) throw "message";` と書いておくと、条件が成立するコードは**コンパイルエラー**になり、しかも文字列がエラーメッセージに現れます。`static_assert` では書きにくい「実行時の値のように見える引数に対する検査」を、静的検査として書くためのイディオムです。投げる型は何でもよく(文字列リテラルがよく使われます)、実行時には決して到達しません。

**使用場所**: [validateFloatingRange()](../../src/core/userpublic/details/schema/structfieldschema.hpp#L414)(範囲が非有限、または min > max なら文字列を throw) / [field()](../../src/core/userpublic/details/schema/structfieldschema.hpp#L486)(フィールド名が空、または予約接頭辞 `$` で始まるなら throw)

**このコードベースでの注意**: 同じ `consteval` でも、[structFieldType()](../../src/core/userpublic/details/schema/structfieldschema.hpp#L369) と [legacyBranchDescriptorsAreStrictlyOrdered()](../../src/core/loader/componentcodec.cpp#L1967) は `throw` ではなく `static_assert`(前者は `always_false_v` のフォールバック、後者は戻り値を呼び出し側の `static_assert` が見る形)で落とします。「引数の値に対する検査」は throw、「型やテーブル全体に対する検査」は `static_assert`、という使い分けです。

### __COUNTER__ と ADL タグディスパッチによる「登録済みの型の列挙」

翻訳単位の中で単調増加するプリプロセッサ整数 `__COUNTER__` を登録マクロに埋めると、登録ごとに違う整数が振られます。その整数 N をタグ型 `Tag<N>` にして、`entry(Tag<N>)` という関数をユーザー側のコードで定義させておけば、あとから `Tag<0>`, `Tag<1>`, ... を順に渡して **ADL(実引数依存探索)で見つかった定義から型を逆引き**できます。テンプレートの明示的特殊化と違い、定義を各所へ分散させたままにできるのが利点です。列挙側は `std::make_integer_sequence<int, N>` で 0..N-1 を生成し、fold 式で各 Index を順に試して、定義が無い Index は `requires` で静かに飛ばします。

**使用場所**: [PELICAN_REGISTER_EVENT](../../src/core/userpublic/details/event/registerer.hpp#L408)(`__COUNTER__` を渡すマクロ) / [EventCatalogTag のタグ型](../../src/core/userpublic/details/event/registerer.hpp#L32) / [collectBehaviorEventHandlers()](../../src/core/userpublic/details/behavior/registerer.hpp#L148) / [collectGameSystemEventHandlers()](../../src/core/userpublic/details/system/registerer.hpp#L85)

### メンバーポインタの非型テンプレート引数

C++17 以降、`template <auto Member>` でメンバーへのポインタを非型テンプレート引数として受け取れます。`decltype(Member)` は `T Owner::*` の形なので、traits で所有者型 `Owner` とメンバー型 `T` を取り出せ、実際の代入は `object.*Member = value;` と書けます。スキーマ定義が「この JSON のキーはこの構造体のこのフィールド」という対応を、文字列でもオフセットでもなく**型として**持てるのがこのイディオムの狙いで、フィールドを消したり型を変えたりするとスキーマ側がコンパイルエラーになります。

**使用場所**: [StructFieldDeclaration](../../src/core/userpublic/details/schema/structfieldschema.hpp#L473) / [field()](../../src/core/userpublic/details/schema/structfieldschema.hpp#L486) / [decodeStructPolicyField()](../../src/core/userpublic/details/schema/structfieldjson.hpp#L565)

### concept / requires 節と、特殊化による trait チェーン

C++20 の `concept` と `requires` 式は「この型がこう使えるか」をコンパイル時に問い合わせる構文です。`requires(T t) { { t.f() } -> std::same_as<void>; }` は「`t.f()` が有効で、その型が `void`」の検査、`requires { typename T::U; }` は「ネストした型別名 `U` が存在するか」の検査です。これに、テンプレート特殊化でラッパ型から中身を取り出す trait を組み合わせると、「必須フィールド」「既定値つき」「任意上書き」といった宣言の違いを、実行時の分岐ではなく型の違いとして表現できます。読む側は、分岐が `if` ではなく特殊化の並びとして書かれていることに気づく必要があります。

**使用場所**: [HasBehaviorEvent](../../src/core/userpublic/details/behavior/registerer.hpp#L116) / [PolicyField](../../src/core/userpublic/details/schema/structfieldschema.hpp#L768) / [required()](../../src/core/userpublic/details/schema/structfieldschema.hpp#L610) / [defaulted()](../../src/core/userpublic/details/schema/structfieldschema.hpp#L678)

### std::span によるイテレータ型・要素数の消去

`std::array<T, N>` は N ごとに別の型なので、そのまま非テンプレートの関数に渡せません。`std::span<const T>`(C++20)はポインタと個数の組を持つ非所有のビューで、`array` でも `vector` でも生配列でも同じ引数型で受けられるようにします。所有しないので**元の配列より長生きしてはいけない**のが唯一にして最大の注意点で、テンポラリから作った span を返すと即座にダングリングになります。

**使用場所**: [fieldSpan()](../../src/core/loader/componentcodec.cpp#L1876) / [ReleaseReport](../../src/core/model/modelresourcerelease.hpp#L36)(`view()` が固定長配列の先頭 N 件を span で返す)

### 関数スコープ static の一度きり初期化

関数内の `static` 変数は、その関数が最初に呼ばれたときに一度だけ初期化され、以降はアドレスが安定します(C++11 以降、初期化はスレッドセーフです)。これが効くのは「参照を返したい」場面で、ラムダの中や自動変数として作ると呼び出しごとに別物になってしまうところを、関数スコープの static に置けば同じオブジェクトへの参照を安全に返せます。代償として、プログラム終了時の破棄順序が翻訳単位をまたいで未規定になるため、他の static に依存する後始末は書けません。

**使用場所**: [UserComponentRegistererTemplatePublic::registerComponent()](../../src/core/userpublic/details/component/registerer.hpp#L185)(`static const auto schema = materializeSchemaNode(...)` の参照を登録レコードに載せる)

### static_assert + sizeof による ABI レイアウトの凍結

DLL 境界を越えて渡す構造体は、両側が同じレイアウトでビルドされていなければなりません。メンバーを 1 つ足しただけで壊れますが、実行時まで気づけないのが厄介です。そこで「凍結した版のレイアウトを表すだけのダミー構造体」を別に定義し、`static_assert(sizeof(Live) == sizeof(FrozenV2Layout))` で突き合わせると、レイアウトを変えた瞬間にビルドが落ちます。`sizeof` だけでは順序の入れ替えを検出できないので、`offsetof` の assert を併せるとより強くなります。

**使用場所**: [ComponentLoaderInfo](../../src/core/userpublic/details/component/registerer.hpp#L81)(直後に `sizeof` の `static_assert`) / [CommonVertStruct](../../src/core/model/vertbufcontainer.hpp#L23)(`offsetof` と `sizeof` の両方)

### std::bit_cast と浮動小数点のビット表現(-0.0 の正規化)

C++20 の `std::bit_cast<To>(from)` は、同じサイズの型の間でビット列をそのまま読み替えます(`reinterpret_cast` や union 経由と違い未定義動作になりません)。浮動小数点をハッシュやダイジェストに入れるとき必ず問題になるのが `-0.0` で、`-0.0 == +0.0` は true なのにビット列は違うため、素直にバイト化すると「等しい値なのに違うダイジェスト」が出ます。バイト化の直前に `if (v == 0.0f) v = 0.0f;` と書いて符号を落とすのが定型です(NaN も同様にビット表現が一意でないので、そもそも弾くのが安全です)。

**使用場所**: [appendResolverFloat()](../../src/core/loader/resolvedscene.cpp#L162)(NaN を例外にし、`0.0F` を代入し直してから `bit_cast`) / [canonicalTransformIdentity()](../../src/core/renderer/accelerationstructure.cpp#L552) / [encodeRecord()](../../src/core/shader/raygrouplayout.cpp#L385)

### 整数オーバーフローの事前検査

符号なし整数の演算は折り返し、符号付きはオーバーフロー自体が未定義動作なので、**起こしてから検出することはできません**。掛け算は `lhs > max / rhs`(rhs が 0 でないとき)、足し算は `lhs > max - rhs` の形で、演算する**前**に判定します。除算に置き換えるのがポイントで、`lhs * rhs > max` と書くと検査そのものが折り返します。サイズ計算(幅 × 高さ × バイト数)のように外から来た値を掛ける経路では、この検査を通した専用のヘルパを 1 つ用意して全部そこを通すのが定型です。

**使用場所**: [checkedMultiply()](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L197) / [parseExtentDerivedBufferSize()](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L223)

### swap-and-pop(末尾要素で穴を埋める削除)

順序を保つ必要のない配列から途中の要素を消すとき、後続を全部前へ詰める(O(n))代わりに、**末尾の要素を穴へ移してから末尾を 1 つ縮める**と O(1) で済みます。順序が壊れることと、消した要素の添字を外に配っていた場合はそれが別の要素を指すようになること(だから世代付きハンドルが要る)が代償です。実装上は「消す位置が末尾自身のときは移動しない」分岐を忘れると自己代入になります。関連して、コンテナを逆順に走査するのは「見つけた時点で末尾側から使う」「途中で消しても未走査部分が動かない」ようにするためです。

**使用場所**: [ECSArchetypeMigrationToken::State::publish()](../../src/core/ecs/archetypemigration.cpp#L93)(`relocate_one(atUnchecked(source_row), atUnchecked(source_last_row))` してから `--count`) / [ECSArchetypeMigration::prepare()](../../src/core/ecs/archetypemigration.cpp#L452)(archetype の chunk 列を `rbegin()` から走査して空きのある chunk を選ぶ)

### C++20 の jthread / stop_token / condition_variable_any による協調停止

`std::jthread` は、デストラクタで自動的に `request_stop()` と `join()` を行うスレッドです。停止要求は `std::stop_token` としてスレッド関数の第 1 引数に渡され、ワーカー側は自分の都合のよい場所で `stop_requested()` を見て抜けます(強制終了ではなく協調的な停止です)。待機中のスレッドを起こすには `std::condition_variable_any` の `wait(lock, stop_token, pred)` を使い、停止要求が来た時点で待機が解けるようにします。素の `std::condition_variable` には stop_token を取るオーバーロードが無いため、この組み合わせでは `_any` の方が要ります。

**使用場所**: [SwapchainBuildWorker::run()](../../src/core/vkcore/swapchainframetarget.cpp#L759)

### 固定長バッファ(例外も動的確保もしない経路のため)

デストラクタや遅延解放のように「例外を投げられない・投げたくない」経路では、`std::string` や `std::vector` の確保が失敗しうること自体が問題になります。そこで、固定容量の配列 + 現在の件数、というだけの型を用意し、溢れたぶんは中身を捨てて**件数だけ数える**、という設計にします。診断情報の完全性を捨てて、失敗しないことと `noexcept` を取るわけです。読む側は「なぜここだけ `std::string` でないのか」の答えがこれだと分かっている必要があります。

**使用場所**: [FixedEffectError](../../src/core/loader/effecttransaction.hpp#L124)(固定長の文字列 2 本を持つ) / [ReleaseReport](../../src/core/model/modelresourcerelease.hpp#L36)(容量 8、溢れは `overflow_count` に数える) / [VertBufContainer::releaseGeometryNow()](../../src/core/model/vertbufcontainer.cpp#L526)

### 自己参照ラムダによる再帰

ラムダは自分自身を名前で呼べません(`auto` で宣言した変数の型が、その初期化子の中ではまだ決まっていないため)。回避策として、自分自身を第 1 引数で受け取り `self(self, ...)` の形で呼ぶイディオムがあります(Y コンビネータ風、C++23 の "deducing this" があれば不要になります)。JSON のように任意の深さを持つ木を、その場限りの述語で走査したいときによく使われます。

**使用場所**: [UI document の木を下る `has_text`](../../src/core/ui/module.cpp#L157)(`self(self, child)` で子を再帰) / [親名の循環検出](../../src/project/sceneformat.cpp#L233)。`RpcServer::handleLine()` にあった `contains_resolution_error(contains_resolution_error, result)`(応答 JSON から `resolution_error` を再帰探索)は、名前付きの自己再帰関数 [`embedsResolutionError()`](../../src/core/communication/editorrpchandlers.cpp#L50) として editor 側へ移り、この形では残っていません。

### コールバック連鎖(既存のフックを move して包む)

すでにフックが刺さっているかもしれない場所へ自分の処理を足すとき、上書きすると先客の処理が消えます。定型は `auto previous = std::move(hook); hook = [previous = std::move(previous), ...] { if (previous) previous(); /* 自分の処理 */ };` で、旧フックをラムダに閉じ込めてから呼びます。先に旧フックを呼ぶか後に呼ぶかは意味が変わる(前処理か後処理か)ため、順序が契約になります。ムーブしてから包むのは、コピーによる二重実行を避けるためです。

**使用場所**: [EditorCommandService::EditorCommandService()](../../src/core/communication/editorcommandservice.cpp#L560)(`frame_boundary_extension` の先客を呼んでから `commitPending()` を足す)

## 10.4 OS・プロセス・ファイルの作法

### 実行中の DLL は Windows では上書きできない(シャドウコピー)

Windows は、イメージセクションとしてマップされた実行ファイル/DLL のファイルを書き込み・削除から保護します。`LoadLibrary` が成功している間、そのファイルへの書き込みオープンは `ERROR_SHARING_VIOLATION`、削除は `ERROR_ACCESS_DENIED` で拒否されます。したがってエンジンが `game_logic.dll` を直接ロードしたままだと、ビルドツール側のリンクが「出力ファイルを開けない」で失敗します。POSIX にはこの制約がなく、`dlopen` 中のファイルを普通に置き換えられます(既存のマッピングが古い inode を保持し続けるだけ)ので、この回避策は Windows 由来です。定番の対処が **シャドウコピー**、つまりビルド成果物をロード用の別ディレクトリへコピーし、コピーの方をロードして原本には触らないやり方です。なお名前の変更(rename)だけは実行中でも許されるため、「リネームで退避してから上書き」という変種もあります。MSVC の `.pdb` も、デバッガがアタッチしていると同様にロックされるため、シンボルを見せたいならコピーと一緒に運ぶ必要があります。

**このリポジトリでは**: `src/core/gamelogic/gamelogicreload.cpp` の `makeShadowCopy()` が、`initialize()` で決めた process 固有の `shadow_directory`(= `source_path.parent_path() / ".pelican-hot-reload" / <pid>`)へ `copy_file(..., overwrite_existing)` で成果物を写し、`.pdb` も追随させます。`loadCopy()` の `LoadLibraryW` は常にこのコピー側にだけ効かせます。自動 poll は原本 `source_path` の `(last_write_time, file_size)` が 2 回続けて一致するまで待ち、さらに write share を許さない read lease を取ってから shadow copy を作るため、リンカが truncate/refill 中の部分 DLL を raw import 診断へ渡しません。writer がまだ開いていれば次フレームで再試行しますが、3 回連続で busy なら `game_logic_source_busy@1` を pending status として公開し、writer が閉じれば次の poll で通常どおり再試行します。起動時には死んだ pid の shadow directory を sweep し、正常 shutdown では自分の pid directory を削除します。狙いは「ビルドツールが書く原本」と「エンジンがマップするファイル」の分離で、process 間でも shadow copy 名が衝突しません。これをしないとゲームを動かしたままリビルドしたときにロック済みの原本へリンカが書けず失敗します。

### 同じパスの LoadLibrary / dlopen は参照カウントされる

`LoadLibraryW` は「まだロードされていなければマップし、すでにロード済みなら参照カウントを 1 増やして同じ `HMODULE` を返す」という仕様です。`dlopen` も POSIX 上で同様で、同一のオブジェクトファイルを指す呼び出しには同じハンドルが返り、`dlclose` の回数が釣り合うまでアンロードされません。つまり「新旧 2 世代の DLL を同時にロードして中身を比べる」ことは、**同じパスのままでは原理的にできません**。別々のマッピングが欲しければファイルの実体を別パスに分ける必要があり、リロード機構がコピー名に連番や世代番号を含めるのはこのためです。逆に、アンロードしたつもりでも参照カウントが残っていればコードは生きたままで、古い関数ポインタがまだ呼べてしまう点にも注意が要ります。

**このリポジトリでは**: `src/core/gamelogic/gamelogicreload.cpp` の `makeShadowCopy(const char *purpose)` が、コピー名を `stem + "_" + purpose + "_" + (++copy_sequence) + 拡張子`(95〜96 行)で組み、`purpose` に `"initial"` / `"candidate"` を、`copy_sequence` に単調増加の連番を埋めます。`validateCandidate()`(171 行)は `active`(現行 DLL)をロードしたまま候補を `loadCopy()`(173 行)し、新旧 2 世代を `internal::validateBehaviorReload()`(182 行)へ突き合わせます。もし候補が現行と同じパスなら参照カウントが増えて同じ `HMODULE` が返るだけで 2 世代を同時に持てないため、連番で必ず別パスにして「切り替え前に互換性を検証する」経路を成立させています。

### POSIX のプロセスグループと setpgid、kill(-pid) で子孫ごと止める

POSIX のプロセスは必ずいずれかの **プロセスグループ** に属し、`setpgid(pid, pgid)` でグループを移せます。`setpgid(0, 0)` は「自分自身を、自分の PID を ID とする新しいグループのリーダーにする」という意味です。`kill(2)` は `pid` が負(かつ `-1` でない)のとき、`|pid|` をグループ ID とするグループの全プロセスへシグナルを送るので、`kill(-child_pid, SIGTERM)` は子とその子孫(グループを抜けていない限り)をまとめて止められます。`-1` だけは「送信可能な全プロセス」を意味する特別値なので、PID を符号反転して渡すコードでは PID 1 を踏まないことが暗黙の前提です。`setpgid` を親子の**両方**で呼ぶのは定番のイディオムで、これは競合の回避策です — 子が `exec` を終えた後に親が呼ぶと `EACCES` で失敗し、親が先に kill しようとするとグループがまだ無い、という順序依存を、どちらが先でも成立するようにして消します。子の側の呼び出しは `fork` 直後・`exec` 前に置く必要があります。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の非 Windows 版 `runPlatformProcess()` が、`fork` 直後・`execv` 前の子で `::setpgid(0, 0)`(299 行)、親で `::setpgid(pid, pid)`(321 行)を両側から呼び、順序依存を消して import 系の外部ツールを専用グループのリーダーにします。タイムアウト/キャンセル時は `::kill(-pid, SIGTERM)`(358 行)→ 猶予 100ms → `::kill(-pid, SIGKILL)`(368 行)と段階的に落とし、子が SIGTERM で先に抜けた場合でも `::kill(-pid, SIGKILL)`(373 行)でグループごと掃除します。PID を符号反転して渡すのは子孫まで一括で止めるためで、これがないと import ツールが孫プロセスを残し、その孫が握るパイプが EOF に達せず読み取りスレッドが返らなくなります。

### Windows の Job オブジェクト(プロセスグループの対応物)

Windows には「プロセスツリーをまとめて殺す」API が素直な形では無く、`CREATE_NEW_PROCESS_GROUP` は名前に反して kill の単位ではありません — これは `GenerateConsoleCtrlEvent` による Ctrl+C / Ctrl+Break の配送単位を分けるだけのフラグです。ツリー単位の生殺与奪を担うのは **Job オブジェクト**で、`CreateJobObject` で作り `AssignProcessToJobObject` でプロセスを入れると、以後そのプロセスが作る子孫も自動的に同じ Job に入ります。`TerminateJobObject` で全員を、`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` を設定しておけば最後の Job ハンドルを閉じた時点でも全員を終了させられます。ここで `CREATE_SUSPENDED` で起動し、Job へ入れてから `ResumeThread` するのは必須のイディオムです。そうしないと、割り当て前の一瞬に子がさらに孫を起動して Job の外に取り逃がす競合が残ります。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の Windows 版 `runPlatformProcess()` が `CreateJobObjectW`(171 行)→ `SetInformationJobObject`(177 行)で `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` を立て、`CreateProcessW(..., CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, ...)`(197〜198 行)→ `AssignProcessToJobObject`(208 行)→ `ResumeThread`(215 行)の順に進めます。`CREATE_SUSPENDED` で止めてから Job へ入れて再開するので、割り当て前に子が孫を起動して取り逃がす競合が消えます。タイムアウト時は `TerminateJobObject`(241 行)で全員を、正常終了時も `UniqueHandle job` の破棄(`job.reset()`、260 行)が KILL_ON_JOB_CLOSE を発火させて生き残った子孫ごと片づけ、両方のキャプチャパイプを EOF へ到達させます(258〜259 行のコメント)。

### PE の import directory / delay-import directory を、イメージをマップせずに読む

Windows の実行イメージ(PE)は先頭に DOS スタブ、続いて `PE\0\0` シグネチャ、COFF ヘッダ、optional ヘッダを持ち、optional ヘッダの末尾に **data directory** の配列が並びます。この配列の 1 番が import directory、13 番が delay-import directory です。どちらのエントリも `(RVA, size)` の組で、RVA(relative virtual address)は「イメージがメモリへマップされたときの、イメージ先頭からの相対アドレス」を指します。**ファイルをそのまま読む場合、RVA はファイルオフセットではありません** — section テーブルを走査し、RVA を含む section を見つけて `raw_offset + (rva - virtual_address)` へ翻訳する必要があります。import directory の各記述子(20 byte)は「元 thunk 配列の RVA」「モジュール名の RVA」「first thunk の RVA」を持ち、thunk 配列の各要素は最上位ビットが立っていれば **ordinal import**(下位 16 bit が序数)、そうでなければ「hint(2 byte) + NUL 終端名」を指す RVA、つまり **named import** です。要素の幅は PE32 で 4 byte、PE32+(64bit)で 8 byte です。delay-import 記述子は歴史的経緯で、フィールドが RVA の版と**絶対アドレス**の版があり、後者は `image_base` を引いて RVA へ直す必要があります。

**このリポジトリでは**: `src/core/gamelogic/gamelogicimports.cpp` が `LoadLibraryW` の前に走る診断用の最小 reader を持ちます。`importDirectoryIndex = 1` / `delayImportDirectoryIndex = 13`(19〜20 行)で 2 つの directory だけを取り、`fileOffset()`(212 行)が section テーブル経由で RVA をファイルオフセットへ翻訳します。`appendThunkImports()`(306 行)が PE32 / PE32+ の幅と ordinal フラグ(`0x80000000` / `0x8000000000000000`)を分け、`GameLogicPeImportKind` の 4 種へ分類します。delay 側の `delayFieldRva()`(293 行)が「絶対アドレス版」を `image_base` の減算で RVA へ直します。ファイルは 256 MiB、記述子は 4096 件、thunk は 65536 件で打ち切りますが、コメントが明記しているとおり **これは PE 形式の制限ではなく parser の作業量制限**で、壊れた入力が無制限の走査や確保にならないようにするためのものです。`directoryDescriptorOffset()`(248 行)が「directory と記述子が同じ section にあること」まで見るのも同じ理由(section 境界を越えた記述子を正当に見せないため)です。この reader が読むのは top-level の import だけで、`GetProcAddress` や依存 DLL 経由の解決は範囲外です。

### Windows の名前付き mutex と `Global\` 名前空間(machine-wide の相互排他)

`CreateMutexA/W` の第 3 引数に名前を渡すと、その名前がプロセスを跨ぐ待ち合わせ点になります。名前は kernel object namespace に属し、接頭辞で可視範囲が変わります — `Local\` は呼び出し元のログオンセッション内、`Global\` はマシン全体(全セッション)です。接頭辞を省くと Terminal Services の設定次第で `Local\` 相当になるため、「同じマシンで走る別セッションのプロセスと排他したい」なら `Global\` を明示します。所有スレッドが `ReleaseMutex` せずに終了すると、次に待っていたスレッドは `WAIT_ABANDONED` を受け取ります。これは失敗ではなく **「取れたが、前の所有者が異常終了した」** の意味で、多くの用途では `WAIT_OBJECT_0` と同じに扱います。注意が要るのは *section*(file mapping)オブジェクトで、`Global\` 名前空間に**新規作成**するには `SeCreateGlobalPrivilege` が要ります。mutex と違って、昇格していないプロセスは `Global\` の名前付き共有メモリを作れません。POSIX 側に直接の対応物は無く、`flock` / `fcntl` のファイルロックや `$XDG_RUNTIME_DIR` 下のロックファイルで、UID とセッションに閉じた範囲を作るのが定番です。

**このリポジトリでは**: `test/gpu_lease_run.cpp` が GPU テストの直列化に使います。`CreateMutexA(nullptr, FALSE, ("Global\\" + name).c_str())`(157 行)が **lease の本体**で、待ちは `WaitForSingleObject`(172 行)、`WAIT_OBJECT_0` と `WAIT_ABANDONED` を**どちらも取得成功**として扱います(前の holder が落ちても GPU が永久にロックされないため)。診断用の holder レコード(保持者 pid とトークン)は別の file mapping で、コメントが明記しているとおり `Global\` の section 作成には `SeCreateGlobalPrivilege` が要り、昇格していないテスト実行では作れません。そこでまず `Global\` を試し、失敗したらセッション名前空間へ落として(164〜169 行)、少なくとも timeout メッセージが pid 0 ではなく実際の保持者を名指しできるようにしています。**lease 自体は常に `Global\` の mutex なので、この fallback で排他が弱まることはありません。** POSIX 側は同じファイルの別実装で、UID と `$XDG_RUNTIME_DIR` に閉じた範囲であることを冒頭のコントラクト(6〜7 行)が明記しています。待ち時間が締め切りを超えたら skip ではなく `gpu_lease_timeout@1{holder_pid=...}` / exit 124 の失敗にする、というのがこの lease の設計判断です。

### 子プロセスのパイプ読みとデッドロック回避

子プロセスの stdout/stderr をパイプで受け取るとき、古典的なデッドロックが 2 種類あります。ひとつは、親が「まず子の終了を待ってから読む」と書いた場合で、子の出力がパイプのカーネルバッファ(数十 KB 程度、`CreatePipe` に 0 を渡せばシステム既定)を超えると子は `write` でブロックし、親は終了を待ち続けて双方が止まります。回避策は、待機と読み出しを並行させること — スレッドを分けるか、POSIX なら `select`/`poll`、Windows なら OVERLAPPED I/O を使います。もうひとつは EOF の取りこぼしで、`read` が 0 を返す(`ReadFile` が失敗する)のは**書き込み端のハンドルが 1 つ残らず閉じられたとき**なので、親が自分の持つ書き込み端を閉じ忘れると読み出しスレッドは永久に返ってきません。さらに厄介なのは、子が終了しても**継承した書き込み端を握ったままの孫**が生きていると同じことが起きる点で、これがプロセスグループ/Job ごと殺す動機のひとつになります。Windows では `CreateProcess` を `bInheritHandles = TRUE` で呼ぶと継承可能なハンドルが一括で渡るため、読み出し端は `SetHandleInformation(..., HANDLE_FLAG_INHERIT, 0)` で明示的に継承対象から外すのが作法です。

**このリポジトリでは**: `src/devcli/processrunner.cpp` の `readPipe()`(Windows 138 行 / POSIX 268 行)を stdout・stderr それぞれ専用の `std::thread` で回し(226〜227 行 / 326〜327 行)、待機ループと並行に排出するので、出力がパイプバッファを超えても「終了待ち→読み出し」型のデッドロックに陥りません。親は起動直後に自分の書き込み端を手放し(Windows `stdout_write.reset()` 223〜224 行 / POSIX `::close(stdout_pipe[1])` 322〜323 行)、Windows では読み出し端を `SetHandleInformation(..., HANDLE_FLAG_INHERIT, 0)`(165〜166 行)で継承対象から外します。終了処理で `job.reset()`(260 行)/ `::kill(-pid, SIGKILL)`(373 行)を先に済ませてから `join()` するのは、子が抜けても孫が握ったままの書き込み端を閉じ、読み出しスレッドを EOF で確実に返すためです(258〜259 行 / 371〜372 行のコメント)。

### ファイル監視 API は取りこぼす前提で使う

`ReadDirectoryChangesW` は、変更イベントをカーネル側のバッファに溜めて 1 回の呼び出しに詰めて返します。バッファが溢れると **溜まっていた内容は全部捨てられ**、`lpBytesReturned` に 0 が入るか `ERROR_NOTIFY_ENUM_DIR` が返ります(MSDN に明記されています)。つまり「どのファイルが変わったか」は復元不能で、アプリ側にできるのはディレクトリを丸ごと列挙し直すことだけです。さらに、1 回の完了を受け取ってから次を再武装するまでの隙間で起きた変更も原理的に落ちますし、ネットワークドライブ越しでは通知自体が信用できない(バッファ長も 64KB 以下が要求される)ため、実装は通常ポーリングへ切り替えます。この「イベントはヒントであり真実ではない。真実は再スキャンで取り直す」という設計が、inotify(`IN_Q_OVERFLOW`)や FSEvents(`kFSEventStreamEventFlagMustScanSubDirs`)を含むファイル監視全般の作法です。なお `CreateFileW` でディレクトリのハンドルを開くには `FILE_FLAG_BACKUP_SEMANTICS` が要ります。

**このリポジトリでは**: `src/core/watch/filewatcher.cpp` の `NativeWatch::threadMainImpl()` が `const bool overflow = (ok && bytes == 0) || error == ERROR_NOTIFY_ENUM_DIR;`(173 行)で溢れを判定し、先に `arm()`(180 行)で再武装してから `notify_(overflow || !ok)`(191 行)へ渡します。溢れ時は「どのファイルが変わったか」が失われるので、通知はヒント扱いにとどめ、真実は `Impl::reconcile()`(472 行)→ `scan()` の全件再ハッシュ(内容ダイジェスト比較)で取り直し、変わったアセットだけを `queue` へ積みます。`Impl::armAll()`(332 行)は `isNetworkStore()`(`GetDriveTypeW == DRIVE_REMOTE`、65 行)や武装失敗を見て `WatcherState::polling` / `degraded` へ落としますが、どの経路も最後は同じ `reconcile()` に合流するので、通知を取りこぼしても・ネットワークドライブでもホットリロードの判定が壊れません。

### 一時ファイル + rename による原子的置換

保存先を直接開いて上書きすると、書き込みの途中でクラッシュ/電断が起きたときに「半分だけ新しい」ファイルが残ります。これを避ける定番が **write-to-temp + rename** です。同じディレクトリに一時ファイルを作って全部書き、flush/close で内容を確定させてから、`rename` で本来の名前に被せます。POSIX の `rename(2)` は、置換先が既に存在する場合でもリンクは他プロセスから常に可視で、古いファイルか新しいファイルのどちらかを指す(中間状態は観測されない)と規定しており、この不可分性がこのイディオムの根拠です。一時ファイルを**同じディレクトリに作る**のは、`rename` が別ファイルシステムをまたぐと `EXDEV` で失敗するからで、`/tmp` に置くと破綻します。厳密な耐障害性まで求めるなら、`rename` の原子性は「見える内容」の話であって「ディスクに届いたか」は別問題なので、ファイルと親ディレクトリの `fsync` が要ります。Windows での対応物は `MoveFileExW` に `MOVEFILE_REPLACE_EXISTING` を渡す形で、`MOVEFILE_WRITE_THROUGH` を併せると移動がディスクに反映されるまで関数が戻りません。

**このリポジトリでは**: `src/core/loader/basicconfig.cpp` の `saveSceneDocument()`(693 行)が、`temporaryScenePath()`(453 行)で保存先の `parent_path()` に `<filename>.pelican-save-<nonce>-<seq>.tmp` を作り(同一ディレクトリなので `EXDEV` を踏まない)、`writeAndFlushSceneFile()`(488 行、write→flush→close)で全部書き切ってから `replaceSceneFileAtomically()`(508 行)で被せます。置換直前にディスクのダイジェストを取り直して TOCTOU 窓を閉じ(754 行)、Windows では `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`(511〜512 行)、それ以外は `std::filesystem::rename()`(520 行)を呼び、失敗時は `TemporarySceneFile` の RAII(479 行)が一時ファイルを消します。シーンファイルはローダとファイル監視が読む唯一の真実なので、直書きして保存中に落ちると「半分だけ新しい」壊れたプロジェクトが残る — 原子的置換はどの瞬間に落ちても旧か新の完全な方だけを観測させます。

### Windows のファイル共有モードとロック検出(ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION)

POSIX と違い、Windows ではファイルを開くときに「他のプロセスに何を許すか」を `CreateFileW` の `dwShareMode` で宣言します。`FILE_SHARE_READ` だけを渡すと「他プロセスの読みは許すが、書きと削除は許さない」という意味になり、逆に**自分が開いている間に他プロセスが書こうとすると失敗する**ようにもなります。ビルド中の DLL のように書き手がいるファイルを開こうとすると `ERROR_SHARING_VIOLATION`(32)か `ERROR_LOCK_VIOLATION`(33)が返るので、この 2 つを「まだ書き込み中だから少し待って再試行」の合図として使えます。ハンドルを開けたこと自体が短時間のロックになるため、判定用に開いたハンドルは RAII で確実に閉じます。

**使用場所**: [SourceReadLease](../../src/core/gamelogic/gamelogicreload.cpp#L202)(`FILE_SHARE_READ` のみで開き、デストラクタで `CloseHandle`) / [sourceWriterIsActive()](../../src/core/gamelogic/gamelogicreload.cpp#L230)(2 つのエラーコードを「書き手が生きている」と解釈)

**このコードベースでの注意**: 定数名は `ERROR_SHARING_VIOLATION` / `ERROR_LOCK_VIOLATION` です(`FILE_LOCK_VIOLATION` という名前の定数はありません)。再試行の回数は `sourceBusyRetryLimit` で 3 回に制限されています。

### プロセス生存判定と PID の型

「その PID のプロセスがまだ生きているか」は、OS ごとに別の API になり、しかも**エラーが必ずしも「居ない」を意味しません**。POSIX の `kill(pid, 0)` はシグナルを送らずに存在確認だけを行い、`0` なら存在、`errno == EPERM`(権限が無い)も**存在する**の意味です。Windows の `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, ...)` は他ユーザーのプロセスに対して `ERROR_ACCESS_DENIED` を返しますが、これも存在の証拠です。したがって両方とも「アクセス拒否は生存扱い、見つからないときだけ死亡扱い」に倒すのが安全側です。PID の型も `DWORD` と `pid_t` で違うので、跨いで持ち回るときは `std::uint64_t` のような共通表現に正規化します。

**使用場所**: [processIsAlive()](../../src/core/gamelogic/gamelogicreload.cpp#L238) / [currentProcessId()](../../src/core/gamelogic/gamelogicreload.cpp#L182)

### 動的ライブラリのエラー取得(GetLastError と dlerror)

`LoadLibraryW` / `GetProcAddress` が失敗したときの理由は Windows では `GetLastError()` の数値コードで、POSIX の `dlopen` / `dlsym` では `dlerror()` の文字列で得ます。両者の性質はかなり違い、`GetLastError()` はスレッドごとの「直前の失敗コード」で成功時にクリアされるとは限らない一方、`dlerror()` は**読み出すとクリアされる**ので 2 回目は null が返ります(そのため戻り値を必ず一度だけ保存します)。また `GetLastError()` から得たメッセージ文字列はマシンのロケールに従うため、そのまま JSON や UTF-8 のログへ載せるとエンコーディングが混ざります。数値コードだけを載せて文字列は自前で組む、という割り切りがよく使われます。

**使用場所**: [platformLoadError()](../../src/core/gamelogic/gamelogicreload.cpp#L165)(Windows は数値コードから英文を組み、POSIX は `dlerror()` の文字列を 1 度だけ読む)

### std::filesystem の canonical / weakly_canonical とルート名

`std::filesystem::canonical(p)` は実在するパスしか正規化できず、存在しなければ例外(または `error_code`)になります。対して `weakly_canonical(p)` は「存在する先頭部分だけを解決し、残りは字句的に正規化する」ので、これから作るファイルのパスにも使えます。Windows 固有の落とし穴が**ルート名**で、`path::has_root_name()` は `C:` や `\\server` の有無を返します。`C:\dir`(ルート名 + ルートディレクトリ)は絶対パスですが、`C:dir` は「ドライブ C の**カレントディレクトリ**からの相対」であり、しかも `current_path()` がドライブごとに存在するため、プロセスのカレントだけでは決まりません。この曖昧な形は受け付けずに拒否するのが安全です。

**使用場所**: [weaklyCanonicalOrAbsolute()](../../src/core/communication/rpcserver.cpp#L956) / [absoluteCapturePath()](../../src/core/communication/rpcserver.cpp#L948) / [projectRootString()](../../src/core/communication/rpcserver.cpp#L900) / [resolveDeclaration()](../../src/core/asset/model.cpp#L248)

## 10.5 アルゴリズムとデータ処理の定番

### Kahn 法のトポロジカルソートと、未知の依存先を無視する実装

Kahn 法は、入次数(その頂点へ入ってくる辺の数)が 0 の頂点を取り出しては、その頂点から出る辺を消して隣接頂点の入次数を減らす、を繰り返す線形時間のトポロジカルソートです。DFS ベースの実装と違って再帰スタックを使わず、また「取り出せた頂点の数が全頂点数に満たなければ閉路がある」という判定が副産物として得られるのが特徴です。閉路の中の頂点は入次数が永久に 0 にならないため、キューが空になった時点で結果列に載らずに残ります。よって `result.size() != vertices` という一行が、そのまま閉路検出になります。取り出し順が FIFO か LIFO かは結果の妥当性には影響せず、同じ入力に対する順序を安定させたいかどうかの選択です。

依存先を頂点表から引けなかったときに `continue` で読み飛ばす実装は、「この依存はソート対象グラフの外にある」= すでに満たされている前提として扱う、という意味になります。辺を張らないので入次数も増えず、その頂点は最初から ready 側に入ります。部分グラフだけを並べ替えるときの定石ですが、代償として「依存先の名前を書き間違えた」ケースが黙って通ることになります。

**このリポジトリでは**: `src/core/watch/reloadtransaction.cpp` の `topologicalOrder()`(105 行〜)。`ReloadCoordinator::execute()` は 1 トランザクション分のアクタを全部 `stage()` した後にこれを呼び、返ったインデックス順で staged 済みリソースを並べ替えてから `registry_.publish(ordered)`(337 行)へ渡します。各アクタの `after` に並ぶ依存先を `actor_for` から引いて「依存先 → 依存元」の辺を張って入次数を数え、`if (found == actor_for.end()) continue;`(118 行)で同一トランザクション外の依存(外側で既に満たされている前提)を読み飛ばします。この順序が要るのは `publish()` が渡された順に `content_revision` を進めて逆依存辺を張り直すためで、依存先を先に確定させないと依存元が古い版を掴みます。末尾の `result.size() != actors.size()`(137 行)が循環依存のトランザクションを例外にし、ホットリロードの差し替えを「全部通すか、全部やめるか」の原子的な単位に保ちます。

### Floyd–Warshall 法と推移閉包(到達可能性の全点対計算)

Floyd–Warshall 法(フロイド・ウォーシャル法)は、全頂点対の最短距離を求める古典的な動的計画法です。「中継してよい頂点を `0..k` に限ったときの答え」を `k` について 1 つずつ広げていく形をしていて、3 重ループの**最外が中継点 `k`** であることが正しさの条件になります(`k` の反復を 1 つ終えた時点で「中継点として `0..k` だけを使う答え」が完成している、というのが保たれる不変条件です)。距離ではなく「到達できるか否か」だけを bool で持てば同じ骨格が**推移閉包**(a→b と b→c があるなら a→c を張る、を閉じるまで繰り返した関係)の計算になり、更新式は `reach[i][j] |= reach[i][k] && reach[k][j]` になります。計算量は頂点数 V に対して O(V³)、記憶量は O(V²) で、疎なグラフでも密行列を持つぶん重いかわりに、実装が 3 行で済み辺の追加順に依存しません。ループ順を入れ替える(`k` を内側へ動かす)と、まだ更新されていない `reach[i][k]` を読むため閉包が不完全になりますが、**グラフの形によっては正しい答えが出てしまう**ので、テストで気づきにくい壊れ方をします。

**このリポジトリでは**: `src/core/renderingpass/frameplanner.cpp` の `transitiveClosure()` が到達可否版で、`validateWritesAreOrdered()` が「同じ resource に書く 2 node のどちらが先か決まっているか」を `reach(a,b) || reach(b,a)` で検査します。frame graph は node 数が数十なので O(V³) が問題にならず、密行列のほうが読みやすい、という選択です。設計上の意味と壊し方は [第6章](06_rendering_vulkan_shader.md) §6.3 の難所「writes-writes の曖昧検出」にあります。

### GJK と EPA(凸形状の距離計算と貫通量計算)

**GJK**(Gilbert–Johnson–Keerthi 法)は、2 つの**凸**形状が交差しているかを、両者の **Minkowski 差**(片方の全点からもう片方の全点を引いた点集合。2 形状が交差することと、この差集合が原点を含むことは同値です)が原点を含むかどうかの問題に置き換えて解く反復法です。差集合を実際に構築せず、方向ベクトル `d` に対する **support 関数**(その方向で最も遠い点)だけを使って、原点を囲もうとする単体(simplex — 2D なら三角形、3D なら四面体)を少しずつ作り替えます。原点を囲めたら交差、囲めないまま「これ以上原点へ近づけない」に至ったら非交差で、そのとき単体から原点までの距離がそのまま 2 形状間の最短距離になります。凸であることが前提なのは、support 関数が形状全体を代表できるのが凸のときだけだからで、凹形状は凸分解するか凸包で近似する必要があります。

**EPA**(Expanding Polytope Algorithm)は、GJK が「交差している」で終わったときに続けて走らせるアルゴリズムです。GJK が残した単体を種にして、Minkowski 差の境界へ向けて多面体を膨らませ続け、原点に最も近い面を見つけます。その面の法線と距離が、貫通を解消するのに必要な最小の移動 = **MTD**(minimum translation distance、最小移動距離)になります。GJK 単体では「どれだけめり込んでいるか」が出ない(交差の判定で止まる)ので、距離は GJK、貫通量は EPA、という役割分担になるのが定番です。どちらも反復法なので、最大反復数と収束の ε をどう置くかが実装ごとの差になります。

**このリポジトリでは**: `src/core/phys/physquerysweep.cpp` が両方を持ちます。反復上限は定数で `kGjkIterations = 40` / `kEpaIterations = 256`、EPA の収束判定は `kEpaTolerance = 1.0e-6F`(19〜21 行)。GJK 側は 442 行のループで最近接点と分離判定を出し、交差した組だけ `EpaFace` を張り替える `penetration()`(657 行)へ進んで貫通量を求めます。呼び出し面と、ε クラスタや collider identity による順序付けの契約は [第5章](05_gameplay_and_services.md) の physics query 節にあります。

内訳の関数は次のとおりです(2026-09-04 追記)。形状ごとの support 関数が [support()](../../src/core/phys/physquerysweep.cpp#L155)、Minkowski 差の支持点(`support(A,d) - support(B,-d)`)が [supportDifference()](../../src/core/phys/physquerysweep.cpp#L256)、GJK 本体が [convexDistance()](../../src/core/phys/physquerysweep.cpp#L433) です。単体から原点への最近点は、部分集合をビットマスクで列挙して各々の重心座標を連立方程式で解き、負の重みが出ない解のうち最短を選ぶ **Johnson のアルゴリズム**([closestForSubset()](../../src/core/phys/physquerysweep.cpp#L310) と [closestToOrigin()](../../src/core/phys/physquerysweep.cpp#L373))で、四面体が原点を含むかは符号付き体積の比 = 重心座標が全て非負かで判定します([tetraContainsOrigin()](../../src/core/phys/physquerysweep.cpp#L519)、面上のケースを内側へ倒す数値マージン付き)。掃引判定の入口は [shapeCast()](../../src/core/phys/physquerysweep.cpp#L757) です。

### CRC-32 の初期値 0xFFFFFFFF と最終反転(なぜ 0 から始めないか)

CRC は本質的にメッセージを多項式とみなした剰余計算なので、レジスタを 0 で初期化すると先頭のゼロバイトが値をまったく動かしません。つまり `00 00 M` と `M` が同じ CRC になり、先頭に 0 が何個あるかを検出できなくなります。初期値を 0xFFFFFFFF にするのはこの盲点をつぶすためで、レジスタに 1 が立った状態から始めることで先頭ゼロもレジスタを撹拌します。最後の反転(0xFFFFFFFF との XOR、C++ では `~crc`)は末尾側に対する同じ対策で、これを入れておくと「メッセージ+CRC」全体を再計算した残差が 0 ではなく固定の非ゼロ値になり、末尾にゼロバイトが付いた破損も検出できます。この 2 つを合わせて CRC-32/ISO-HDLC(zlib・PNG・Ethernet と同じ変種)と呼びます。

ビット反転版(LSB ファースト)では生成多項式 0x04C11DB7 をビット反転した 0xEDB88320 を使い、右シフトで回します。`0u - (crc & 1u)` は最下位ビットが 1 なら全ビット 1、0 なら 0 になるマスクで、分岐なしで「LSB が立っていたら多項式を XOR」を書くイディオムです。

**このリポジトリでは**: `src/core/vkcore/previewexecutor.cpp` の `crc32()`(22 行)。`appendChunk()` が長さフィールドを書いた直後の位置 `start` から `subspan(start, 4 + data.size())` を渡し(39 行)、PNG 仕様どおり長さフィールドを除いた「チャンクタイプ 4 バイト + データ」に対して計算して末尾へ付けます。`encodePng()` が IHDR / IDAT / IEND の各チャンクをこれで綴じます。狙いは preview capture(`PreviewExecutor::execute()` が返すヘッドレスなサムネイル)を libpng や zlib に一切リンクせず自前バイト列として吐くことで、初期値 0xFFFFFFFF と `~crc` の最終反転を省いたり長さ込みで計算したりすると PNG デコーダがチャンクを拒否し、生成した preview が開けなくなります。

### zlib / deflate の BFINAL ビットと「非圧縮(stored)ブロック」

deflate(RFC 1951)のストリームはブロックの連なりで、各ブロックの先頭に BFINAL(1 ビット、最終ブロックなら 1)と BTYPE(2 ビット)が来ます。BTYPE=00 は非圧縮ブロックで、この場合は残りのビットを捨ててバイト境界まで進め、続けて LEN(2 バイト、リトルエンディアン)と NLEN(LEN の 1 の補数)を置き、そのあとに生データを LEN バイト並べます。LEN が 16 ビットなので 1 ブロックあたり最大 65535 バイト、それを超える入力は複数ブロックに分割し、最後のブロックだけ BFINAL=1 にします。つまり BFINAL/BTYPE=00 の 3 ビットは 1 バイト目の下位 3 ビットに収まるので、非圧縮ブロックの先頭バイトは常に `0x01` か `0x00` になります。ハフマン符号器を書かずに「形式として正しい deflate ストリーム」を出す最短経路がこれです。

zlib ラッパ(RFC 1950)は deflate データの前に CMF/FLG の 2 バイト、後ろに Adler-32 を 4 バイト(ビッグエンディアン)付けます。`0x78 0x01` は CM=8(deflate)・CINFO=7(32KB 窓)で、FLG は `(CMF<<8|FLG) % 31 == 0` を満たすよう選ばれた値です。Adler-32 は s1 を 1、s2 を 0 から始めて 65521(2^16 未満の最大の素数)で剰余を取り、`(s2 << 16) | s1` を出力します。

**このリポジトリでは**: `src/core/vkcore/previewexecutor.cpp` の `zlibStored()`(42 行)。`encodePng()` が「行ごとに先頭フィルタバイト 0 を積んだスキャンライン列」を渡し(85 行)、返った zlib ストリームをそのまま IDAT チャンクの中身にします。中では入力を 65535 バイトずつに切り、`out.push_back(final ? 1 : 0)` が BFINAL+BTYPE=00 のバイト、`nlen = ~len` が NLEN、末尾の `s1`/`s2` ループが Adler-32 です。ハフマン符号器を書かずに妥当な zlib ストリームを出す最短経路で、これにより CRC-32 と合わせて preview PNG を zlib 依存ゼロ・決定的に生成できます(サイズは膨らみますが、配布物にエンコーダを抱えずに済みます)。

### デッドゾーンの取り方(成分ごと vs 半径)と、方向を保つ再スケール

アナログスティックは中立位置でも微小な値を返すので、しきい値以下を 0 に潰す処理(デッドゾーン)が要ります。1 軸(トリガーや単軸)なら絶対値でしきい値判定すれば十分ですが、2 軸のスティックに軸ごとの判定を掛けると、無効領域が円ではなく十字型になり、斜め入力のときに片方の成分だけが 0 に落ちて方向が軸に吸い寄せられます。そのため 2 軸では合成ベクトルの長さ(半径)で判定するのが定番で、これを radial deadzone と呼びます。

潰すだけだと今度はしきい値の外側で値が不連続に跳ねるので、残った範囲 `[deadzone, 1]` を `[0, 1]` に線形に引き伸ばします。`(magnitude - deadzone) / (1 - deadzone)` がその式です。再スケール後は元のベクトルを正規化してから新しい長さを掛け直すことで、方向を変えずに大きさだけを補正できます。スティックの生値は矩形にクリップされて返ることがあり斜めで長さが 1 を超えうるので、最後に 1 でクランプします。しきい値を `[0,1)` に制限しておくと `1 - deadzone` のゼロ除算を構造的に防げます。

**このリポジトリでは**: `src/core/os/actionmap.cpp`。1 軸版が `applyDeadzone()`(477 行、`std::copysign` で符号を戻す)、2 軸版が `readGamepadAxisBinding()` の `axis2` 分岐(525-534 行)です。後者は `strongestGamepadAxis()` が全パッドから拾った生値 x, y の合成長 `magnitude` で判定し、`scaled = min(1, (magnitude - deadzone)/(1 - deadzone))` を `x / magnitude * scaled`・`y / magnitude * scaled` に掛けて、方向を変えずに大きさだけ補正して `sample.state.axis2` に書きます。移動や照準にスティックを使うとき、軸ごとに潰すと斜め入力が軸へ吸い寄せられて狙った向きがずれるので、半径判定で入力の方向を保存しているのが要点です。deadzone は入力プロファイル JSON 由来で、`parseInputProfileJson()`(780 行)が `[0,1)` を検証して `1 - deadzone` のゼロ除算を構造的に封じています。

### 走査順(DFS)と表示順(ソートキー)を分けて考えること

シーングラフや UI ツリーを扱うコードでは、「木をどうたどるか」と「描画結果をどの順で並べるか」は別の関心事です。木の走査は親子関係(変換の継承、クリップ矩形の継承)を解くために必要なので素直に DFS の再帰で書き、その過程では描画コマンドを平坦な配列に push するだけにします。そのあとで配列全体を独立したソートキー(レイヤ、深度、パイプライン状態など)で並べ替えると、ステート変更をまとめてドローコールを減らせます。両者を混ぜて「走査しながら正しい順で出す」ようにすると、レイヤ指定が親子関係をまたぐたびに破綻します。

ここで効くのが安定ソートです。同じソートキーを持つ要素同士の相対順序が入力順(= 宣言順・走査順)のまま保たれるので、「レイヤが同じなら後に書いたものが上」という直感的な規則が、追加のタイブレーク実装なしで得られます。逆に `std::sort` は等価要素の順序を保証しないため、同じ結果を再現したければ宣言順のシーケンス番号を明示的にキーへ入れる必要があります。

**このリポジトリでは**: `src/core/ui/module.cpp` の `visitNodes()`(84 行)が node.children を DFS でたどり、各ノードの `QuadCommand` に `layer` と `decl_seq` を写して平坦な配列へ push するだけに徹します。`src/core/ui/drawcommands.cpp` の `buildDrawBatch()`(10 行)がそれを `std::stable_sort` で `(layer, decl_seq)` 昇順に並べ直し、隣り合う同一 `key`(テクスチャページ・サンプラ・シザー)を 1 本の `run` にまとめてステート変更を減らします。走査中に正しい順を作ろうとすると layer 指定が親子をまたぐたびに破綻するため両者を分けており、加えて 1 ノードが複数 quad を出す場面(nine-patch の 9 セルや `visitNodes()` のグリフ列は layer も decl_seq も同値)では `std::sort` だと並びが崩れるので、描画順を決定的にするには stable でなければなりません。ヒットテスト用の前順走査は別関心事として `createRuntimeWidgets()`(193 行)が `traversal` に溜めます。

### JSON-RPC 2.0 の予約エラーコード(出典は RFC ではない)

`-32700` のような負の大きな数がエラーコードとして出てくるのは JSON-RPC 2.0 の規約です。まず出典について — **JSON-RPC 2.0 は IETF の RFC ではありません**。JSON-RPC Working Group が jsonrpc.org で公開している仕様(2010-03-26、2013-01-04 修正)で、RFC 番号は割り当てられていません(RFC があるのは JSON のデータ形式のほう = RFC 8259 です)。予約コードは仕様の **§5.1 "Error object"** の表に載っていて、次の 5 つが事前定義です。

```text
-32700  Parse error       受け取ったテキストが JSON として不正
-32600  Invalid Request   JSON ではあるが Request object として不正
-32601  Method not found  そのメソッドが無い
-32602  Invalid params    パラメータが不正
-32603  Internal error    サーバ内部エラー
```

さらに `-32000` から `-32099` が「実装が定義してよいサーバエラー」として予約され、`-32768` から `-32000` までの全体が「仕様側の予約域」です。したがって**アプリ固有のエラーは、予約域の外(`-31999` 以上、あるいは正の値)を使うか、`-32000..-32099` の実装定義枠を使うか**のどちらかになります。この数値がなぜ飛び飛びなのかに意味はなく、既存実装との衝突を避けるために選ばれた区画です。

**このリポジトリでは**: `src/project/jsonrpc.hpp` の `namespace JsonRpcErrorCodes`(12〜19 行)が `parseError = -32700` / `invalidRequest = -32600` / `methodNotFound = -32601` / `invalidParams = -32602` と、実装定義枠の `applicationError = -32000` / `renderDocCaptureError = -32010` を持ちます。事前定義の 4 つは仕様どおりで、後ろ 2 つは `-32000..-32099` の枠内に収めた pelican 固有コードです([第7章](07_tools_rpc_tests.md) の protocol 層)。`-32603` internal error は現在この enum に無く、内部失敗も `applicationError` に寄せています。

### 正規表現でパスやページ名を検証するときの、区切り文字とドットの扱い

C++ の `std::regex` は既定で ECMAScript 文法です。この文法では `.` は改行以外の任意の 1 文字を意味するので、拡張子やドット区切りの名前を照合したいときは `\.` とエスケープするか文字クラス `[.]` に入れる必要があります。C++ の通常の文字列リテラルに書く場合はバックスラッシュ自体をエスケープするので `"\\."` と二重になります(生文字列リテラル `R"(\.)"` ならそのまま 1 個)。一方 `/` は ECMAScript の**文法上は**特別な文字ではなく、JavaScript のリテラル `/.../` の区切りとして特別なだけなので、C++ 側では `\/` とせずそのまま書けます。

`std::regex_match` は文字列全体との一致を要求し、部分一致を探す `std::regex_search` とは別物です。したがって `regex_match` に渡すパターンの `^` と `$` は冗長ですが、意図を読み手に示すために残されることがよくあります。「セグメント/セグメント/…」の形は `SEG(/SEG)*` と書くのが定番で、これなら先頭・末尾の空セグメントや連続スラッシュを構造的に弾けます。

**このリポジトリでは**: `src/core/ui/semanticfixture.cpp` の名前空間スコープの `std::regex` 群(54-59 行)を、`validateSemanticFixtureSchema()` が UI セマンティクスの正準フィクスチャ JSON の各トークンに `std::regex_match` で当てます。`widget_re`(55 行)は widget id や trace target を(187/84 行)、`texture_re`(57 行)は draw_run の texture を(201 行)、`pointer_re`(58 行)は error path の JSON Pointer(`~` の後は 0/1 のみ)を(217 行)検査します。`texture_re` の `(\\.[A-Za-z0-9_-]+)*` は `module.cpp` が吐く stable_name `"atlas:" + path + "/page:" + n`(module.cpp 178 行)のドット付きセグメントを表しており、`.` をエスケープしないと `.` が `/` や任意文字にも当たって崩れたテクスチャ ID が通ってしまいます。ここを厳密に照合することで、カバレッジゲート(`runSemanticCoverageGate()`)の契約であるフィクスチャがセマンティクス検査より前にすり抜けるのを防いでいます。

### Möller–Trumbore 光線-三角形交差

三角形の 3 頂点 V0, V1, V2 と光線(origin O, direction D)に対して、交差点を `O + tD = (1-u-v)V0 + uV1 + vV2` と置き、辺ベクトル `e1 = V1-V0`, `e2 = V2-V0` を使ってクラメルの公式で t・u・v(u, v は重心座標)を一度に解く古典的アルゴリズムです(Möller & Trumbore, 1997, "Fast, Minimum Storage Ray/Triangle Intersection")。行列式にあたる `det = dot(e1, cross(D, e2))` が 0 に近いとき、三角形が退化しているか光線が三角形の平面と平行で、そこがこの手法の数値的な弱点になります。三角形の平面方程式を事前計算しないので記憶量が小さく、GPU のレイトレーシング実装でも標準的な選択です。

**使用場所**: [intersect()](../../src/core/shader/raytracingoracle.cpp#L226)(倍精度の素の計算と、区間演算版を並べて持つ)

**このコードベースでの注意**: ここは GPU の結果を照合するための CPU 側オラクルなので、「同じ式を並べて書く」ことが目的です。コメントが normative sequence と断っているとおり、式をまとめて最適化してはいけない箇所です。

### 軸への投影による分離判定(AABB の slab 法と OBB の分離軸定理)

軸平行境界ボックス(AABB)を扱うアルゴリズムが軒並み単純なのは、**各軸を独立に扱える**からです。光線対 AABB の slab 法は、3 軸それぞれについて「光線がその軸のスラブ(2 平面の間)に入る時刻と出る時刻」を求め、3 軸の入る時刻の最大と出る時刻の最小を比べます(前者が後者を超えたら交差なし)。点対 AABB の距離も軸ごとのはみ出し量の二乗和で、線分対 AABB は同じ量を媒介変数 t の関数と見て区間を狭めていく形になります。

回転した箱(OBB)同士では軸独立性が失われるので、**分離軸定理**(SAT, separating axis theorem)を使います。凸形状同士が交差しないことと、両者の投影が重ならない軸が 1 本存在することは同値で、箱同士では調べるべき軸が「A の 3 面法線 + B の 3 面法線 + 両者の辺の外積 9 本 = 15 本」に限られます。各軸への投影半径は、回転行列の成分の絶対値と半径ベクトルの内積で求まります。1 本でも分離軸が見つかれば即座に「交差なし」で打ち切れます。

**使用場所**: [raycast(Ray, Box)](../../src/core/phys/physquery.cpp#L388)(slab 法) / [pointAabbDistanceSquared()](../../src/core/phys/physquery.cpp#L234) / [segmentAabbDistanceSquared()](../../src/core/phys/physquery.cpp#L253) / [overlaps(Box, Box)](../../src/core/phys/physquery.cpp#L577)(15 軸の SAT、各軸に `kEpsilon` の余裕)

### Time of Impact(TOI)と conservative advancement

高速に動く物体の衝突を「動かしてから重なりを調べる」で扱うと、1 フレームで相手を貫通してすり抜けます(tunneling)。これを避けるのが掃引(sweep)判定で、最初に接触する時刻 = **TOI**(time of impact)を求めます。実装で広く使われるのが **conservative advancement** で、現在時刻での 2 物体間の最短距離 d と、その方向への接近速度(closing speed)から「少なくともこの時間は接触しない」安全な刻み `d / closing_speed` を求めて時刻を進める、を繰り返します。距離が許容差を下回ったところで停止し、分離している側の時刻と重なっている側の時刻を bracket として保持しておくと、法線を「分離側」から取れるので数値的に安定します(重なった状態の単体からは接触法線が定まりにくいため)。接近速度が 0 以下なら永久に当たらないので即座に打ち切ります。

**使用場所**: [shapeCast()](../../src/core/phys/physquerysweep.cpp#L757)(GJK の距離から `step = (distance - tolerance) / closing_speed` を取って進める) / [refineOverlapTime()](../../src/core/phys/physquerysweep.cpp#L136)(分離側と重なり側の bracket を縮める)

### FNV-1a とハッシュ値の合成(0x9e3779b9)

**FNV-1a**(Fowler–Noll–Vo, variant 1a)は「バイトを XOR してから素数を掛ける」を繰り返すだけの非暗号学的ハッシュです。64bit 版の素数は 1099511628211(= 2^40 + 2^8 + 0xb3)、初期値(offset basis)は 14695981039346656037 で、実装が短く分布も実用的なので、内部的な指紋や辞書の鍵に広く使われます。暗号強度は無いので、外部から与えられた値の衝突を防ぐ用途には使えません。

複数のハッシュ値を 1 つに混ぜるときの定型が `h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2)` で、Boost の `hash_combine` がこの形です。`0x9e3779b9` は黄金比の逆数を 32bit 固定小数で表した値(2^32 / φ)で、ビットが偏りなく散っている定数として選ばれています(64bit 版は `0x9e3779b97f4a7c15`)。単に XOR や加算で混ぜると、同じ値の組み合わせが打ち消し合って衝突します。

**使用場所**: [hashWord()](../../src/core/vkcore/swapchainframetarget.cpp#L120)(64bit FNV-1a を 8 バイトぶん回す) / [surfaceSupportFingerprint()](../../src/core/vkcore/swapchainframetarget.cpp#L130)(offset basis 14695981039346656037 から始める) / [hashCombine()](../../src/core/shader/pipelinefactory.cpp#L69)

### ソート済み列に対する隣接検査(adjacent_find)

「この列に重複が無いこと」を確かめたいとき、総当たりは O(n²) ですが、**ソートしてしまえば重複は必ず隣り合う**ので `std::adjacent_find` の 1 パス O(n) で済みます(ソート自体の O(n log n) が支配的)。同じ理屈で「狭義単調増加か」の検査も隣接比較 1 回で書けます。前提はもちろん「ソート済みであること」で、ソートを忘れた列に adjacent_find をかけると、離れた位置にある重複を見逃します。なお要素数が数個しかない集合では、`std::set` を組むより線形探索の方が速くキャッシュにも乗るため、あえて素朴なループを選ぶことがあります。

**使用場所**: [ECSEntityMutation::prepareCreate()](../../src/core/ecs/archetypemigration.cpp#L997)(archetype key をソートしてから `adjacent_find` で重複コンポーネントを弾く)

### 再帰的な型の循環検知(訪問スタック)

型が型を含む構造(構造体のメンバがまた構造体、など)を再帰的に辿るとき、循環があると無限再帰になります。定石は「いま辿っている経路上の型」をスタックに積んでおき、これから降りようとする型がすでにスタックに載っていれば閉路として失敗させることです。**訪問済み集合ではなく訪問中スタック**である点が肝で、同じ型が兄弟として 2 回現れるのは循環ではありません(訪問済み集合で弾くと、正当な共有を誤検出します)。降りる前に積み、戻るときに降ろす、という対称性が崩れると誤検出になります。

**使用場所**: [Inspector::canonicalType()](../../src/core/shader/rtspirvinspection.cpp#L337)(SPIR-V の型 id を辿りながら訪問中スタックで閉路を止める)

### SPIR-V のバイナリ形式

SPIR-V モジュールは 32bit ワードの列です。先頭 5 ワードがヘッダ(magic `0x07230203`、バージョン、生成器 ID、id の上限、予約語)で、以降は命令が並びます。各命令の**先頭ワードは `word_count << 16 | opcode`** で、`word_count` はその命令自身を含む総ワード数なので、次の命令へはこの数だけ進めます(命令の中身を解釈しなくても走査できるのがこの形式の利点です)。文字列リテラルは 1 ワード 4 バイトのリトルエンディアンに詰めた NUL 終端で、末尾はワード境界まで 0 で埋められます。**外部で引くときの名前**は "SPIR-V Specification" の Physical Layout 節です。

**使用場所**: [Inspector::parse()](../../src/core/shader/rtspirvinspection.cpp#L615)(word_count で命令列を走査) / [decodeLiteralString()](../../src/core/shader/rtspirvinspection.cpp#L223)

### JSON Pointer(RFC 6901)

`/a/b/0` の形でJSON 文書内の 1 か所を指す標準記法が **JSON Pointer(RFC 6901)** です。`/` 区切りの各トークンはオブジェクトのキーか配列の添字で、空文字列のポインタは文書全体を指します。キーに `/` や `~` が含まれる場合のエスケープがあり、**`~0` が `~`、`~1` が `/`** です。復号は RFC 6901 が順序まで規定していて、**先に `~1` → `/`、後から `~0` → `~`** の順でなければなりません。逆順にすると `~01` が `~1`(正解)ではなく `/` になります。nlohmann::json では `json::json_pointer` 型で表現し、`at(pointer)` は不在で例外、`operator[]` は不在の位置を作ってしまう、という違いがあります。エラー報告の「どのフィールドが悪いか」を機械可読に返す用途で広く使われます。

**使用場所**: [prepareSet()](../../src/core/communication/editorjournalops.cpp#L648)(`Json::json_pointer{field_path}` でフィールド位置を解決) / [parseEditorMutationRequest()](../../src/core/communication/editorjournal.cpp#L2952)(構築できるかだけを検査する) / [appendResolverFloat()](../../src/core/loader/resolvedscene.cpp#L162)(エラーに `/resolver_inputs/<field>` を載せる)

### nlohmann::json の型判定・キー順・JSON null

nlohmann::json は数値を「符号なし整数 / 符号付き整数 / 浮動小数」の 3 つに区別して保持し、`is_number_unsigned()` / `is_number_integer()` / `is_number_float()` で見分けられます(`is_number_integer()` は符号なしにも true を返すので、負値を拒否したいときは順序に注意が要ります)。`nlohmann::ordered_json` は `std::map` ではなく挿入順を保つ連想コンテナを使う別名で、出力の安定性が要るとき(差分を取る、ダイジェストを取る)に選びます。もう 1 つの落とし穴が `std::optional` の出力で、**値が無いときにキーへ代入しないとキー自体が消えます**。「キーはあるが null」を出したいなら明示的に `nullptr` を代入する必要があり、この 2 つは受け手にとって別物です。

**使用場所**: [exactUnsigned()](../../src/core/communication/editorjournal.cpp#L69)(`is_number_unsigned()` を先に見て、次に `is_number_integer()` — この順でないと符号なしが符号付き枝へ落ちる) / [validateStructSigned()](../../src/core/userpublic/details/schema/structfieldjson.hpp#L201)(同じ 2 段の順序) / [editorComponentQueryJson()](../../src/core/communication/editorcommandservice.cpp#L1587)(値が無ければ `OrderedJson(nullptr)` を明示的に置く)

### IEEE 754 の倍精度と JSON の「安全な整数」2^53−1

JSON の数値には型の区別が無く、多くの実装(JavaScript の `JSON.parse` を含む)は倍精度浮動小数点で受けます。IEEE 754 の倍精度は仮数部が 53 ビットなので、**絶対値が 2^53 = 9007199254740992 を超える整数は 1 刻みで表せません**。したがって「往復しても壊れない整数」の上限は 2^53−1 = 9007199254740991(JavaScript の `Number.MAX_SAFE_INTEGER`)です。64bit の ID をそのまま JSON に載せる設計は、この値域で切るか、文字列として運ぶかのどちらかを選ぶ必要があります。

**使用場所**: [exactUnsigned()](../../src/core/communication/editorjournal.cpp#L69)(`maxExactJsonInteger` を超える値を `invalid_argument`) / [BehaviorAttachmentArena::reserveRuntimeAttachmentIdentity()](../../src/core/gamelogic/behaviorarena.cpp#L1057)(ハンドル空間の枯渇として `overflow_error`) / [max_exact_struct_json_integer の定義](../../src/core/userpublic/details/schema/structfieldjson.hpp#L199)

**このコードベースでの注意**: 境界の取り方が 2 つあります。journal と behavior arena は 2^53−1(9007199254740991)を上限に、スキーマ検証側は 2^53(9007199254740992)を上限にしています。前者が「安全な整数」の定義どおり、後者は 2^53 ちょうどを許す(表現自体は可能な)側です。

### JSON-RPC 2.0 の request / response の形と行区切り framing

JSON-RPC 2.0 の request は `{"jsonrpc": "2.0", "method": ..., "params": ..., "id": ...}` で、`params` は配列かオブジェクトのみ(スカラーは不可)、`id` を持たないものは **notification** と呼ばれ応答を返してはいけません。response は `result` と `error` の**どちらか一方だけ**を持たなければならず、両方あるものも両方無いものも不正です。この仕様自体には転送方式が定められていないため、行区切り(1 行 1 メッセージ)や `Content-Length` ヘッダ(LSP 方式)といった framing は実装側の取り決めになります。行区切りを採る場合、Windows で生まれたテキストは行末に `\r` が残るので、JSON へ渡す前に落とす必要があります(`\r` は JSON の空白文字なので実害が出ないこともありますが、id の照合や digest には効きます)。予約エラーコードは前掲の項を参照してください。

**使用場所**: [RpcServer::run()](../../src/core/communication/rpcserver.cpp#L1078)(`getline` の後に末尾 `\r` を落として 1 行 1 応答) / [RpcServer::handleLine()](../../src/core/communication/rpcserver.cpp#L1013) / [parseJsonRpcRequest()](../../src/project/jsonrpc.cpp#L148)

## 10.6 座標・数値の約束事

### NDC(正規化デバイス座標)と Vulkan の座標系

頂点シェーダが `gl_Position` に書く値はクリップ空間の同次座標で、これを w で割ったものが NDC です。Vulkan では NDC の x, y が [-1, 1]、**z が [0, 1]** と定められています(Vulkan 仕様 "Coordinate Systems")。z の範囲が [-1, 1] なのは OpenGL の規約で、Vulkan では違います。さらにフレームバッファ座標の原点は**左上**で、y は下方向に増えます。したがってビューポートの height を正の値にしたまま描くと、NDC の +Y はフレームバッファの下方向へ向かいます。OpenGL 育ちの読者が「上下が逆」と感じる差はここに集約されます。

**このリポジトリでは**: 代表は `src/core/resources/taa_resolve.frag` の `linearizeDepth()`(28 行)で、深度バッファから読んだ値を `[-1,1]` へ戻さず `vec4(uv * 2.0 - 1.0, depth, 1.0)` の z にそのまま入れ、逆射影を掛けてビュー空間 z を復元します。Vulkan の深度が最初から `[0,1]` だからこの再マップ無しが正しく、OpenGL 流に `depth*2-1` を挟むと現フレームと再投影先の深度が食い違い、TAA の disocclusion 判定(62〜65 行)が毎フレーム履歴を捨てて残像除去が効かなくなります。CPU 経路の `src/core/vkcore/previewexecutor.cpp` `raster()`(149 行)も同じ前提で `ndc.z < -0.1f || ndc.z > 1.1f` を視錐台外として間引き、`test/camera_test.cpp`(222〜223 行)が `perspectiveRH_ZO` の出力を znear→0.0 / zfar→1.0 に固定して規約を守らせています。

### GLM の `_RH_ZO` / `_NO` サフィックスの意味

GLM の射影行列生成関数は名前に規約が埋め込まれています。`RH` は右手系のビュー空間(カメラは -Z を向く)、`LH` は左手系。`ZO` は深度出力が **Z**ero-to-**O**ne、`NO` は **N**egative-one-to-**O**ne です。サフィックス無しの `glm::perspective` は `GLM_FORCE_DEPTH_ZERO_TO_ONE` というコンパイル時マクロの有無で `_ZO` / `_NO` に切り替わるため、マクロの定義漏れで深度が壊れる事故が起きやすい箇所です。サフィックス付きの関数を直接呼べばマクロに依存しません。なお GLM の `perspectiveRH_ZO` は `Result[1][1]` が正であり、Y 軸の反転は射影行列側では行われません。

**このリポジトリでは**: `src/core/renderer/camera.cpp` の `rebuildProjectionMatrix()`(473・481 行)と `prepareSceneCameras()` 内の `rebuild` ラムダ(541・548 行ほか)が、透視は `glm::perspectiveRH_ZO(yfov, aspect, znear, zfar)`、平行投影は `glm::orthoRH_ZO(-xmag, xmag, -ymag, ymag, znear, zfar)` とサフィックス付きで呼び分けます。深度 `[0,1]`・右手系という前提は深度バッファ・TAA・preview まで全経路で共有されるため、もしどこか 1 つの翻訳単位で `GLM_FORCE_DEPTH_ZERO_TO_ONE` を定義し忘れると、その TU だけ `glm::perspective` が `[-1,1]` 深度を吐き、include 順に依存して深度クリア/比較が壊れます。サフィックスを明示することでこの規約をマクロから切り離し、`getVPMatrix()`(673 行)の `glm::lookAt`(-Z を向く右手系)とも符号を合わせています。

### ビューポート変換と「負の height」による Y 反転

NDC からフレームバッファ座標への変換は Vulkan 仕様 "Controlling the Viewport" に式として書かれています。`x_f = (p_x/2)·x_d + o_x`、`y_f = (p_y/2)·y_d + o_y`、`z_f = p_z·z_d + o_z` で、`p_x` = viewport.width、`o_x` = viewport.x + width/2、`p_y` = viewport.height、`o_y` = viewport.y + height/2、`p_z` = maxDepth - minDepth、`o_z` = minDepth です。ここで **height に負の値を入れると y の符号が反転する**というのが VK_KHR_maintenance1(Vulkan 1.1 でコアに昇格)で入った仕様で、「OpenGL 流の Y 上向き NDC をそのまま使うためのトリック」として広く使われています。逆に height を正のままにするなら、NDC の +Y は画面下方向という Vulkan 本来の向きになります。

**このリポジトリでは**: `src/core/vkcore/offscreenframetarget.cpp` の `setViewportAndScissor()`(84 行)と `src/core/vkcore/swapchainframetarget.cpp` のフレーム開始処理(287 行)が、どちらも `viewport.height` に **正の** `extent.height` を、`minDepth=0 / maxDepth=1` を入れます。つまり負 height の Y 反転トリックは使わず、NDC の +Y はフレームバッファ下方向に対応します。この符号は engine 全体の約束事で、`src/core/renderer/projectionjitter.cpp` の `projectionJitterSample()`(69〜70 行コメント)が「positive-height だから CPU/シェーダ/画像の符号が一致する」と明記します。片方のターゲットだけ反転させると、ジッタやピクセルスナップが逆向きに効いて preview と実画面がずれるため、GPU 各ターゲットと CPU ラスタで Y の向きを 1 つに固定しています。

### w 除算(パースペクティブ除算)と w ≤ 0 のガード

クリップ空間から NDC を得るには xyz を w で割ります。射影行列を通すと透視射影では w がビュー空間の -z(=カメラからの奥行き)になるため、カメラ**後方**の点では w が負、近平面上の縮退では w が 0 付近になります。このまま割ると符号が反転して、後ろにあるはずの点が画面内の反対側に現れます。GPU はラスタライズ前に同次クリッピングでこれを処理してくれますが、**CPU 側で自分で投影するコードにはクリッピングが無い**ので、w の符号と大きさを自前で確認して弾く必要があります。これが投影ユーティリティに必ず `w <= eps` の早期 return が付いている理由です。

**このリポジトリでは**: 代表は `src/core/phys/physworld.cpp` の `projectPoint()`(146 行)で、コライダのワイヤフレーム頂点を CPU 側で NDC に投影する際、`std::abs(clip.w) <= kProjectEpsilon || clip.w < 0.0f` なら false を返し、呼び出し元の `lineWorld()` がその線分ごと描画をやめます。デバッグ描画は `DebugDraw` に NDC の端点を直接渡す CPU 経路なので GPU の同次クリッピングが効かず、カメラ後方(w<0)や近平面上(|w|≈0)の点をそのまま割ると符号が反転して線が画面を横断してしまいます。このガードがその代役です。preview の `src/core/vkcore/previewexecutor.cpp` `raster()`(146 行)も同じ理由で `!std::isfinite(clip.w) || clip.w <= 0.00001f` を弾いてから割ります。

### `ndc * 0.5 + 0.5` と、その逆変換の `2 / size`

[-1, 1] を [0, 1] に写すアフィン変換が `v * 0.5 + 0.5`、そこにサイズを掛ければピクセル座標になる、というのがグラフィクスで最も頻出するイディオムです。同じ式は UV 生成(フルスクリーン三角形)でもシャドウマップの参照でも使われます。重要なのは**逆向き**で、「ピクセル単位のオフセット」を NDC のオフセットに直すときは `2 * px / size`、つまり 0.5 の逆数である 2 が掛かります。この 2 が抜けると、ジッタやスナップの効きが常にちょうど半分になります。

**このリポジトリでは**: 順方向は `src/core/resources/sprite.vert` の `anchorFramebuffer = (anchorNdc * 0.5 + 0.5) * pelicanFrame.resolution.xy`(49 行)で、アンカーの NDC をフレームバッファのピクセル座標に直します。逆方向は同じ shader の `deltaNdc = (snappedFramebuffer - anchorFramebuffer) * 2.0 * pelicanFrame.resolution.zw`(51〜52 行)で、`floor(+0.5)` でピクセル境界にスナップした差分を NDC に戻します。この `2.0` が `*0.5+0.5` の 0.5 の逆数で、抜けるとスナップもジッタ(`src/core/renderer/projectionjitter.cpp` の `jitter_ndc = 2.0f * offset_px / size`)も常に半分しか効かず、pixel perfect が半ピクセルずれて滲み、TAA のサンプル網目も半分に縮んで残像除去が弱まります。`resolution.zw` に 1/width・1/height が入るのは `src/core/vkcore/renderer.cpp` の `data.resolution = glm::vec4{width, height, inverse_width, inverse_height}`(206〜207 行)で、除算を乗算にしてあります。

### NDC 空間のオフセットは、w を掛けてクリップ空間で足す

「最終的な NDC を Δ だけずらしたい」とき、`gl_Position` に直接 Δ を足すのは間違いです。出力は後段で w で割られるので、実際のずれは Δ/w になってしまいます。正しくは **Δ に w を掛けてから足す**(`clip.xy += delta_ndc * clip.w`)と、除算後にちょうど Δ だけずれます。同じことを行列側で一度に行うのが「射影行列の 1・2 行目に、w を作る行(第 4 行)の Δ 倍を加える」という書き換えで、TAA のジッタ行列はほぼ例外なくこの形で実装されています。

**このリポジトリでは**: 頂点単位版が `src/core/resources/sprite.vert` の `clip.xy += deltaNdc * clip.w`(53 行)です。最終 NDC を `deltaNdc` だけずらしたいが `gl_Position` は後段で w 除算されるため、先に `clip.w` を掛けておき、除算後にちょうど `deltaNdc` だけ動くようにしています。行列版は `src/core/renderer/projectionjitter.cpp` の `applyProjectionJitter()`(79〜80 行)で、各列の第 4 行(w を作る成分)`projection[column][3]` の `jitter_ndc` 倍を第 0/1 行へ加え、その行列を通る全頂点の `clip.xy` を一括で `jitter*w` ずらします。w を掛けずに直接足すと実ずれが `deltaNdc/w` になり、奥のスプライトほどスナップが弱く近いほど強い深度依存のブレになって pixel perfect も TAA のジッタも崩れます。

### 画像の行 0 は上端 — CPU ラスタで Y 反転が要る理由

PNG をはじめ多くのラスタ画像フォーマットは、非インタレース時のスキャンラインを**上から下へ**格納します(PNG 仕様 ISO/IEC 15948)。したがって `pixels[y * width + x]` のような線形バッファでは、y = 0 が画像の上端です。GPU 経由で描く場合はこの対応をビューポート変換が引き受けるので、シェーダ側に反転は現れません。一方、CPU で NDC から直接ピクセルを書き込むコードは、自分の座標系の Y の向きと画像の行方向の対応を明示的に書く必要があります。GPU 経路と CPU 経路で反転の有無が食い違って見えるのは、この責務の置き場所が違うためです。

**このリポジトリでは**: `src/core/vkcore/previewexecutor.cpp` の `raster()`(153 行)が `y = (1.0f - (ndc.y * 0.5f + 0.5f)) * request.height` と明示的に Y を反転して、+Y の NDC を小さい行番号(= PNG の上端)に落とします。続く `encodePng()`(73〜78 行)は行 0 から順にスキャンラインを並べる(各行頭にフィルタバイト 0 を積む)ので、PNG の「上から下」格納と一致します。GPU 経路では正 height のビューポート変換が NDC→ピクセルの Y を引き受けるため `src/core/resources/sprite.vert` に反転は現れません。CPU ラスタにはその変換が無いので自前で反転する必要があり、外すと preview の PNG だけが実画面に対して上下逆さまに出ます。

### ピクセル中心は n + 0.5、テクセル中心は (i + 0.5) / size

Vulkan のラスタライズ規則では、整数のフレームバッファ座標は**ピクセルの角**を指し、整数座標 (x, y) を持つピクセルの**中心**は (x + 0.5, y + 0.5) です(Vulkan 仕様 "Rasterization")。だからフラグメントシェーダの `gl_FragCoord.xy` は既定で半整数になります(Vulkan では原点も常に左上)。テクスチャ側も同じ約束で、テクセル (i, j) の中心は正規化 UV の ((i + 0.5)/width, (j + 0.5)/height) です。ピクセルアートを nearest で等倍表示するときに「境界を整数に合わせる」のは、この結果としてサンプル点が各テクセルの中心に落ちるようにするためで、0.5 ずれるとどのテクセルを取るかが場所によって変わり、行や列が飛んだり重複したりします。

**このリポジトリでは**: ピクセル中心側は pixel snap で、`src/core/userpublic/sprite/pixelpolicy.cpp` の `quantizePixelBoundary`(154 行 `std::floor(v + 0.5)`)と GPU 側 `src/core/resources/sprite.vert` の `floor(anchorFramebuffer + 0.5)`(50 行)が、投影したアトラス角を整数フレームバッファ座標(ピクセルの角)へ丸めます。ピクセル中心が n+0.5 にあるので、角を整数に合わせると nearest サンプル点が各テクセル中心の真下に揃い、`pixelpolicy.hpp` の宣言コメント(72〜74 行)が言う「サンプルが n+0.5 に落ちる」状態になります。0.5 ずれるとスプライトの移動中に場所ごとにテクセル境界を跨ぎ、行や列が重複/欠落して滲みます。テクセル中心側は `src/core/resources/taa_resolve.frag` の `currentNeighborhood()`(35〜36 行)が UV を `[texelSize*0.5, 1-texelSize*0.5]` にクランプし、3×3 近傍が端テクセルの外を読んで色クランプ箱を歪めるのを防いでいます。

### `floor(x + 0.5)` と `round()` は同じ丸めではない

`std::round` は C++ 標準(C の `round` に委譲)で「半端は 0 から遠い方へ」丸めると規定されています。つまり -2.5 は -3 になります。対して `floor(x + 0.5)` は常に +∞ 方向へ半端を送るので、-2.5 は -2 です。正の値しか来ない場面では一致しますが、負の値を含むと結果が変わります。さらに厄介なのは GLSL 側で、GLSL 仕様の `round()` は **0.5 の丸め方向が実装定義**(実装が速い方を選んでよい)と明記されており、半端が決定的に決まるのは `roundEven()` だけです。CPU と GPU で同じ結果を出したいコードが `round` ではなく `floor(x + 0.5)` を選ぶのは、この実装定義を踏まないためです。

**このリポジトリでは**: 同じピクセルスナップを CPU と GPU の両方で計算します。CPU 側 `src/core/userpublic/sprite/pixelpolicy.cpp` の `quantizePixelBoundary`(154 行 `std::floor(v + 0.5)`)が量子化後の位置を出し、GPU 側 `src/core/resources/sprite.vert` の `floor(anchorFramebuffer + 0.5)`(50 行)が実際に頂点を置きます。どちらも `round()` ではなく `floor(x+0.5)` なので、0.5 の丸め方向が CPU/GPU で必ず一致します(GLSL の `round()` は 0.5 が実装定義、`std::round` は 0 から遠い側で、負値で食い違う)。もし食い違うと、CPU が「整数スケールにスナップする=pixel perfect 可」と判定したスプライトが GPU では別の位置に落ち、対策したはずのスプライトが滲みます。境界の期待は `test/sprite_foundation_test.cpp`(434〜435 行)が `3.49→3.0` / `3.50→4.0` に固定しています。

### ε 比較:絶対誤差・相対誤差・その混合

`std::abs(a - b) <= eps` は**絶対誤差**の比較で、値が大きくなると破綻します。float の仮数は 24 ビットなので、1.0 付近の刻み幅(ULP)は 2^-23 ≒ 1.19e-7 ですが、10^4 付近では ≒ 9.8e-4 になり、eps = 1e-5 のような閾値は「隣り合う表現可能な値ですら通らない」領域に入ります。逆に `<= eps * std::abs(a)` の**相対誤差**は 0 近傍で閾値が潰れて使えません。そこで定番なのが両者の混合で、`<= eps * std::max(1.0, std::abs(a))` と書き、1 未満のスケールでは絶対誤差、それ以上では相対誤差として振る舞わせます。閾値そのものは「意味のある差」から決めるべきで、機械イプシロンから決めるものではありません。

**このリポジトリでは**: `src/core/userpublic/sprite/pixelpolicy.cpp` の `closeToInteger()`(9〜12 行)と `nearlyZero()`(18〜20 行)がともに `pixelContractTolerance * std::max(1.0, …)` の混合形です(閾値 `pixelContractTolerance = 1.0e-4` は同名 `.hpp` 11 行)。`closeToInteger()` は `evaluatePixelContract()`(58〜67 行)がカメラのズーム比が整数かを判定するのに使い、非整数なら `non_integer_zoom_x/y` で pixel contract を却下します。`nearlyZero()` は `classifyStrictSprite()`(115〜121 行)でビュー基底が軸整列か(非対角≈0・対角非0)を見ます。これらは「スプライトが pixel perfect の対象になるか」の関門なので、純粋な絶対誤差だと高倍率で 1 ULP が 1e-4 を超えて正しい整数ズームを弾き、純粋な相対誤差だと 0 近傍の「≈0」判定が潰れます。混合形にすることで「整数か」「ゼロか」の双方を安定させ、契約が本来通すべき構成だけを通します。

### 長さは二乗のまま比較し、ε も二乗する

ベクトルの長さを閾値と比べるだけなら `std::sqrt` は不要で、`lengthSquared(v) <= eps * eps` と書けます。平方根の除去は速度のためだけでなく、0 近傍で sqrt の傾きが発散するため微小量に対する精度が落ちる、という理由もあります。ここで閾値を `eps` ではなく **`eps * eps`** にするのが要点で、比較する量が二乗スケールなのに閾値だけ一乗のままだと、実質の閾値が sqrt(eps) にすり替わります(eps = 1e-5 なら約 3.2e-3、300 倍以上緩くなる)。正規化前のゼロ長判定でこの形が頻出します。

**このリポジトリでは**: `src/core/phys/physquery.cpp` の `normalizedQuat()`(99 行)と `closestPointOnSegment()`(226 行)、`src/core/phys/physworld.cpp` の `normalized()`(65 行)、`src/core/phys/joltphysicsprovider.cpp` の `normalizeOr()`(143 行)が、正規化の割り算前に `len_sq = dot(v,v)` を `kEpsilon * kEpsilon`(kEpsilon=1e-5)と比べ、下回れば単位クォータニオンや端点・フォールバックベクトルを返して割り算を回避します。`normalizedQuat()` はコライダ変換の `rotateVector()` などが通る要所です。ε を二乗するのが要点で、`len_sq <= kEpsilon` のままだと実効の長さ閾値が `sqrt(1e-5)≈3.2e-3` と約 300 倍緩くなり、ほぼ縮退した回転/線分がガードをすり抜けて極小長で割られ、NaN/inf が raycast や overlap の結果に伝播して決定的物理が壊れます。

**長さそのものが要るときは `std::hypot`**(2026-09-04 追記)。比較ではなく実際の長さが必要な場面では、`sqrt(x*x + y*y + z*z)` は途中の二乗でオーバーフロー / アンダーフローしますが、`std::hypot` は内部で桁を揃えるためその範囲で耐えます(C++17 で 3 引数版が入りました)。[parseBasicCameraUp()](../../src/core/loader/basicconfig.cpp#L244) が `basic_config.camera.up` の長さを `std::hypot` で測り、0 なら基底が作れないので設定エラーにします。

### NaN はあらゆる比較で false — `isfinite` を先に置く

IEEE 754 では NaN を含む順序比較(`<`, `<=`, `>`, `>=`, `==`)はすべて false を返し、`!=` だけが true を返します。この非対称性のせいで、`if (d <= eps) return fallback;` と `if (d > eps) { ... }` は NaN の扱いが逆になり、条件の書き方ひとつで挙動が変わります。しかも inf - inf や 0 * inf は NaN を生むので、正規化やレイキャストのように除算と減算が混ざる経路では簡単に混入します。ε 比較そのもので NaN を弾こうとせず、`std::isfinite` による有限性チェックを比較の**前**に明示的に置く、というのが定石です。

**このリポジトリでは**: `src/core/userpublic/sprite/pixelpolicy.cpp` の `evaluatePixelContract()` が冒頭で `!std::isfinite(pixels_per_unit) || pixels_per_unit <= 0.0`(41 行)を先に弾いて比率計算に入る前に `invalid_pixels_per_unit` を返し、`classifyStrictSprite()` も軸整列判定の前に `finiteBasis()`(14〜16 行)を通します。Jolt 側は `normalizeOr()`(143 行)が `!std::isfinite(length_squared) ||` を `<= kEpsilon*kEpsilon` 比較の前に置きます。NaN は `<,<=,>,>=` をすべて false にするので、素の `d <= eps` は d が NaN のとき静かに逆の枝へ行きます — 正規化や投影は減算と除算が混ざり NaN/inf を生みやすい経路です。`isfinite` を先頭に置くことで、NaN の `pixels_per_unit` は「eligible」に化けず却下され、NaN 長のベクトルは NaN 法線ではなくフォールバックに落ち、壊れたアセット/設定が「描けてしまう壊れフレーム」ではなくきれいな拒否になります。

### 「ε 以内なら等しい」は同値関係ではない

`std::abs(a - b) <= eps` は反射律と対称律を満たしますが、**推移律を満たしません**。a と b が近く、b と c が近くても、a と c は 2·eps 離れうるからです。これは単なる理屈の話ではなく、C++ 標準が `std::unique` の二項述語に「同値関係であること」を事前条件として要求している(`[alg.unique]`)ため、ε 比較を渡した時点で規格上は未定義の領域に入ります。実際に何が起きるかは「直前に残した要素と比べるか、隣接要素同士で比べるか」という実装の選択に依存し、値が数珠つなぎに並んだ入力で結果が分かれます。ソート済みの入力で近接値をまとめる、という用途では意図どおり動くことが多いものの、境界の切れ方が処理系依存であることは意識しておく必要があります。

**このリポジトリでは**: `src/core/phys/physquery.cpp` の `segmentAabbDistanceSquared()`(276〜280 行)が、線分×AABB の距離を評価する候補パラメータ `breaks` を `std::sort` してから `std::unique(..., [](float lhs, float rhs){ return std::abs(lhs - rhs) <= kEpsilon; })` で近接値を畳みます。ソート済みなので比較は隣接要素同士に限られ、推移律を満たさない述語を `std::unique` に渡す規格違反も影響が隣接に閉じます。ここは重複した評価点を間引くだけで距離の最小値は変わらないため無害ですが、決定性が本当に要る近接値クラスタリング(shape-cast の TOI/MTD タイブレーク)は別で、その規則は `src/core/phys/physquery.hpp` の `shapeCastTieEpsilon`(65 行)周辺のコメント(129〜137 行)が「どのクラスタを採り、どの鍵で順序付けるか」まで契約として明文化しています。


### 区間演算(interval arithmetic)と有向丸め

浮動小数点の計算を「1 個の近似値」ではなく「真値を必ず含む区間 [lower, upper]」で行う手法です。各演算の結果について、下端は下向き(`nextDown` / `FE_DOWNWARD`)、上端は上向き(`nextUp` / `FE_UPWARD`)へ丸めれば、真値が区間の外へ出ないこと(**包含性**)が保証されます。加算は端点同士を素直に足せますが、減算は `[a-d, b-c]` と交差し、乗算は端点の符号の組み合わせで最大・最小の位置が変わるため **4 通りの積の min / max** を取ります。除数の区間が 0 をまたぐと結果が発散するので、そこは呼び出し側で弾く必要があります。C++ では `std::nextafter` で 1 ULP ずらすのが、丸めモードを触らずに有向丸めを近似する簡便な方法です(厳密な最良区間より少し広くなります)。

**使用場所**: [multiply()](../../src/core/shader/raytracingoracle.cpp#L62)(4 積の min/max) / [divide()](../../src/core/shader/raytracingoracle.cpp#L71)(0 を含む除数を例外にする) / [nextDown()](../../src/core/shader/raytracingoracle.cpp#L40) / [RayTracingOracleInterval](../../src/core/shader/raytracingoracle.hpp#L128)

### ULP と nextafter によるパディング

**ULP**(unit in the last place)は、ある浮動小数点値のすぐ隣の値との差、つまりその大きさにおける刻み幅です。値が大きくなるほど ULP も大きくなるので、「誤差 ε 以内」を絶対値で書くと値域によって厳しすぎたり緩すぎたりします。`std::nextafter(x, ±inf)` は x の隣の表現可能な値を返すので、これを n 回適用すれば「n ULP ぶん広げる」がそのまま書けます。CPU の倍精度計算と GPU の単精度結果を突き合わせるとき、丸めの方向や FMA(積和の中間丸めが省かれる)の有無で結果が数 ULP ずれるため、比較の許容差をこの単位で置くのが自然です。

**使用場所**: [padEndpoints()](../../src/core/shader/raytracingoracle.cpp#L212) / [expandGpuRayT()](../../src/core/shader/raytracingoracle.cpp#L695) / [endpoint_f32_ulp_padding の定義](../../src/core/shader/raytracingoracle.hpp#L26)(nextafter を何回適用するかの定数)

### 四元数: 単位性の判定とベクトル回転の展開形

回転を表せる四元数は**単位四元数**(ノルム 1)だけです。読み込み時にできるのは「正規化できるか」の判定までで、`norm_squared` が 0 近傍なら回転として無意味なので拒否します — ここで勝手に正規化してしまうと、著作データの値を実装が書き換えたことになります(往復で一致しなくなります)。

四元数 q でベクトル v を回すとき、`q * v * conj(q)` を素直に計算する代わりに `t = 2 * cross(q.xyz, v); v' = v + q.w * t + cross(q.xyz, t)` と展開するのが標準的です。行列を作らずに済み、乗算回数も少なくなります。この式は q が単位四元数であることを前提にしているので、上の判定を通っていない値を渡すと大きさまで変わります。

**使用場所**: [requireNonZeroQuat()](../../src/core/loader/componentcodec.cpp#L407)(`norm_squared <= epsilon` を拒否するだけで正規化はしない) / [rotateVector()](../../src/core/phys/physquery.cpp#L107)(上の展開形) / [normalizedQuat()](../../src/core/phys/physquery.cpp#L97)

### GLM の quat コンストラクタ引数順は (w, x, y, z)

`glm::quat` のコンストラクタは **`{w, x, y, z}` の順**で成分を取ります(メンバ変数としての `.x .y .z .w` の並びとは別の話です)。一方 glTF の `rotation`、多くのワイヤ形式、そして人が書く JSON は `[x, y, z, w]` の順で格納します。したがって配列から読むときは必ず並べ替えが要り、忘れると「軸と角度が入れ替わった」ような、しかし一見それらしく見える回転になります。型は同じなのでコンパイラは何も言いません。

**使用場所**: [InternalGltfLoader::nodeTransform()](../../src/core/model/gltf.cpp#L1489) / [requireQuatField()](../../src/core/communication/rpcserver.cpp#L290)(配列の 0,1,2,3 を x,y,z,w に読んでから `glm::quat{w, x, y, z}` を組む)

### TRS の合成順序と world → local の逆算

`translate * toMat4(rotation) * scale` という積は、列ベクトル規約(GLM の既定、`M * v`)では **右から順に適用される**ので「スケール → 回転 → 並進」の意味になります。行ベクトル規約の資料(DirectX 系の解説など)では同じ意味の式が逆順に書かれるため、外部の説明を持ち込むときは規約の確認が要ります。

親の world 変換から子の local を復元するときは、この逆を逆順に当てます — 並進を引き、親回転の逆を掛け、親スケールで割る。スケールで割る以上、**成分が 0 に近い親スケールでは逆算が発散します**ので、閾値を置いて拒否するのが定石です。非一様スケールと回転が混ざると local の scale が一意に決まらない(せん断が生じる)ことにも注意が要ります。

**使用場所**: [PolygonInstanceContainer::prepareTrs()](../../src/core/renderer/polygoninstancecontainer.cpp#L2213)(合成側) / [projectTransform()](../../src/core/loader/componentcodec.cpp#L650)(逆算側、`invertible_epsilon` で 0 スケールを拒否)

### 法線の変換は transpose(inverse(M))

頂点位置を行列 M で変換するとき、法線に同じ M を掛けると、**非一様スケールやせん断があった場合に面と垂直でなくなります**。正しくは M の左上 3×3 の逆転置行列 `transpose(inverse(mat3(M)))` を掛けます(接平面上のベクトルとの内積が 0 に保たれる、という条件から導かれるので「normal matrix」と呼ばれます)。回転と一様スケールだけなら逆転置は元の行列の定数倍になるので、正規化してしまえば同じ結果になり、この違いが表に出ません。接線(tangent)は面に**平行**なベクトルなので本来は M をそのまま掛ける方が理屈に合いますが、同じ normal matrix を使う実装も多くあります。

**使用場所**: [InternalGltfLoader::transformVertexData()](../../src/core/model/gltf.cpp#L711)(`glm::transpose(glm::inverse(glm::mat3{transform}))` を法線と接線の両方に掛け、長さが 0 でないときだけ正規化する)

### 切り上げ除算と mip 寸法のシフト

整数の切り上げ除算は `(n + d - 1) / d` です(n, d は非負)。計算シェーダのディスパッチ数のように「1 グループが d 要素を処理する。端数のためにもう 1 グループ要る」を表すのに必須の形で、`n / d + 1` と書くと n が d の倍数のときに 1 グループ余分になります。オーバーフローを避けたければ `n / d + (n % d != 0)` と書けます。

mip レベル m の寸法は `max(1, n >> m)` です(2 で割ることを右シフトで書いたもの)。`max(1, ...)` が要るのは、一方の辺が先に 1 に達しても mip チェーンは続くためで、これを忘れると 0 サイズのディスパッチや 0 除算になります。

**使用場所**: [parseExtentDerivedBufferSize()](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L223)(タイル数の切り上げ) / [imageExtentDispatchGroups()](../../src/core/renderingpass/computetaskcontainer.cpp#L630)(`max(1u, dimension >> mip)` の後に切り上げ除算)

### view 行列の逆行列とカメラの前方軸

ビュー行列 V は world → view の変換なので、その逆 `world_from_view = inverse(V)` は view → world、つまり**カメラの姿勢そのもの**です。列優先の 4×4 では、第 0・1・2 列が**ビュー空間の +X / +Y / +Z 軸**を world 座標で表し、第 3 列がカメラ位置になります。右手座標系のカメラは −Z を向くので、第 2 列は「前」ではなく**後ろ向き**であり、視線ベクトルは `-world_from_view[2]` です(この符号の取り違えが、ソートや背面判定が丸ごと反転するよくある原因です)。ビュー行列が直交かつ一様スケールなら、逆行列は回転部の転置 + 平行移動の付け替えで安く求まりますが、素直に `inverse()` を呼んでも意味は同じです。

**使用場所**: [drawQueueSortView()](../../src/core/renderer/polygoninstancecontainer.cpp#L49)(`inverse(view)` の第 2 列を符号反転して正規化し、描画順のソート軸にする)

## 10.7 OpenXR ランタイムの作法

### swapchain image の acquire → wait → release 契約

OpenXR の swapchain は Vulkan の WSI とは別の API で、1 枚のイメージを使うのに **3 つの呼び出しを必ずこの順で**通します。`xrAcquireSwapchainImage` が「次に書くイメージの添字」を返し、`xrWaitSwapchainImage` が「ランタイムがそのイメージを書き込み可能にし終えた」まで待ち、描画後に `xrReleaseSwapchainImage` でランタイムへ返します。仕様上、**wait が成功する前に release を呼ぶのは違反**で、また acquire していないのに release するのも違反です(`XR_ERROR_CALL_ORDER_INVALID`)。フレームの途中で失敗して抜けるときも、acquire 済みのイメージは必ず release して返さなければならないため、成功経路と失敗経路の両方から呼ばれる後始末を状態で守るのが定型になります。**外部で引くときの名前**は OpenXR 仕様の "Swapchain Image Management" 節です。

**使用場所**: [XrCompositionTarget::Impl::acquireAndWait()](../../src/core/openxr/openxrcompositiontarget.cpp#L499) / [XrCompositionTarget::Impl::releaseSwapchain()](../../src/core/openxr/openxrcompositiontarget.cpp#L399) / [XrCompositionTarget::Impl::abortFrame()](../../src/core/openxr/openxrcompositiontarget.cpp#L427)

**このコードベースでの注意**: 各 swapchain が `XrSwapchainState`(acquired / waited / released)を持ち、`release_attempted` フラグで二重 release を防いでいます。順序の契約を「呼び出し規約」ではなく状態機械として保持しているので、中断経路から呼んでも安全です。

### XR_TIMEOUT_EXPIRED は正常系(失敗ではない)

`xrWaitSwapchainImage` は指定した timeout の間にイメージが使えるようにならなかったとき `XR_TIMEOUT_EXPIRED` を返します。これは**成功系の戻り値**(`XR_SUCCEEDED()` が真)であって失敗ではないので、エラーとして扱って抜けるのは誤りです。正しくは、使えるようになるまで再試行します(`XR_INFINITE_DURATION` を渡す選択肢もあります)。OpenXR は成功コードが複数ある API なので、`XR_FAILED()` / `XR_SUCCEEDED()` のマクロで大雑把に判定したあと、`XR_SESSION_LOSS_PENDING` のような「成功だが以後は縮退する」コードを個別に見る、という二段の判定になります。

**使用場所**: [XrCompositionTarget::Impl::acquireAndWait()](../../src/core/openxr/openxrcompositiontarget.cpp#L499)(`if (wait_result == XR_TIMEOUT_EXPIRED) continue;` の無限ループ)

### XR_KHR_composition_layer_depth(深度をランタイムへ渡す)

`XR_KHR_composition_layer_depth` は、projection layer の各ビューに深度イメージを添える拡張です。`XrCompositionLayerProjectionView::next` に `XrCompositionLayerDepthInfoKHR` を繋いで渡し、ランタイムはこれを再投影(late-stage reprojection / space warp)に使います。ここで 2 種類の値を混同しないことが重要で、`minDepth` / `maxDepth` は**深度イメージに書かれている値の範囲**(通常 0.0 と 1.0)、`nearZ` / `farZ` はその 2 つの値が対応する**メートル単位の距離**です。逆投影(reverse-Z)や無限遠平面を使っている場合、`nearZ` > `farZ` や `farZ = INFINITY` を渡すことになるので、投影行列の作り方と一致させる必要があります。

**使用場所**: [XrCompositionTarget::Impl::endLogicalFrame()](../../src/core/openxr/openxrcompositiontarget.cpp#L1043)(ビューごとの `XrCompositionLayerDepthInfoKHR` を組んで `projection_views[view].next` に繋ぐ)

## 10.8 宣言的なグラフと拡張の作法

この節は、v0 で入った共通グラフ・package・オフライン出力の周りで**一般的な作法**にあたる 6 つを集めたものです。どれも pelican 固有の発明ではなく、プラグイン機構・ファイルシステム・タイムラインを扱うソフトウェアが繰り返し採ってきた形です。エンジン固有の難しさの側は [第9章 §9.24](09_black_magic_and_gotchas.md) にあります。

### レジストリと登録順(登録順が文書の意味を変えない、という条件)

プラグイン的な機構は「モジュールが自分の型と操作を登録し、文書はその名前を使う」という形をとります。ここで設計上の分かれ目になるのが **登録順が意味に影響するかどうか**です。影響する設計(後から登録したものが前を上書きする、名前が衝突したら後勝ち)は書きやすい代わりに、モジュールを 1 つ足すだけで既存文書の意味が変わりえます。影響しない設計にするには、条件が 3 つ要ります — (a) 名前の衝突を**上書きではなく拒否**にする、(b) 登録の失敗が**部分的なモジュール**を残さない(原子的な登録)、(c) 列挙する API が登録順ではなく**名前順**で返す。

**小さな例**: モジュール A が `value.add` を、モジュール B も `value.add` を登録したとします。後勝ちの設計なら、`A → B` の順で登録したビルドと `B → A` のビルドで同じ文書が別の計算をします。「重複は拒否」にすると、どちらの順でも同じエラーになり、**壊れ方が順序に依存しません**。同じく、カタログを登録順で出力すると、モジュールの追加が既存の出力の差分を汚します。名前順で出せば追加行だけが増えます。

**使用場所**: [`GraphModuleRegistry::registerModule()`](../../src/project/graphmodule.hpp#L55)(`graph.module_duplicate@1` / `graph.operation_duplicate@1` などで拒否し、拒否されたモジュールは何も変えない)/ [`GraphModuleManifest` のコメント](../../src/project/graphmodule.hpp#L33)(「登録順は文書の意味を変えない」が宣言されている場所)/ [`catalog()`](../../src/project/graphmodule.hpp#L64)(名前順で出力)

### semantic type と logical type(不透明な型がハンドルを運ぶ理由)

型付きのデータフローでは、しばしば型が 2 段になります。**semantic type** は「何であるか」の名前(`pelican.value.scalar@1` のような名前空間 + 名前 + メジャー版)、**logical type** はそこへ構成子(image / buffer / value …)と引数を載せて**同値判定できる形**にしたものです。名前だけで比較すると「同じ名前だが解像度が違う画像」を同一視し、実体で比較すると「同じものを別の経路で作った」だけで別物になります。2 段にすると、前者を semantic、後者を引数付きの logical が受け持てます。

もう 1 つの定石が、**ポートを流れる値を不透明にする**ことです。バイト列ではなく「ハンドル + その世代」を流すと、受け手は中身を知らないまま「これは私が期待した世代のものか」だけを検査でき、世代違いを `memcpy` してしまう事故が型の側で消えます。代償として、その型は汎用のコピーやシリアライズができません — それは欠点ではなく、**意図した制限**です。

**小さな例**: 姿勢(スケルトンのポーズ)をバイト列で流すと、別のスケルトン向けに作られたポーズをブレンドしても「配列長が同じなら」通ってしまいます。ハンドル + layout 世代にしておけば、ブレンド演算の入口で世代不一致を名前付きで拒否できます。

**使用場所**: [`SemanticTypeId`](../../src/project/logicalrendertype.hpp#L24) / [`SemanticTypeSchema`](../../src/project/logicalrendertype.hpp#L108) / [`LogicalType`](../../src/project/logicalrendertype.hpp#L128)(構成子 + semantic + 引数)/ [`LogicalTypeRegistry::canonicalize()`](../../src/project/logicalrendertype.hpp#L146) / 不透明型の実例は [`pelican.pose@1` の説明](../../src/project/posegraphmodule.hpp#L11)

**このコードベースでの注意**: 登録されている semantic は 11 種です。値・音・イベントの 3 つ(`pelican.value.scalar@1` / `pelican.audio.voice@1` / `pelican.event.playback_control@1`)に加えて、`pelican.render` module が 6 つ(`storage_buffer@1` / `attribute@1` / `color_signal@1` / `depth@1` / `legacy_opaque_image@1` / `legacy_opaque_resource@1`、[`logicalrendertype.cpp`](../../src/project/logicalrendertype.cpp#L583) を [`graphmodule.cpp`](../../src/project/graphmodule.cpp#L337) が登録し、[`renderPorts()`](../../src/project/graphmodule.cpp#L55) 経由でポートに現れます)、pose module が 2 つ(`pelican.pose@1` / `pelican.rig@1`、[`posegraphmodule.cpp`](../../src/project/posegraphmodule.cpp#L74))です。`opaque` を持つのは姿勢とリグの 2 つで、ただし `pelican.rig@1` はポートには現れません(ノードの parameter として名前で指定します)。Studio 側は**型名にソケットの形を割り当てる表**を JSON で持っており、新しい型は C++ を書かずに [`graph_socket_styles.json`](../../src/devstudio/resources/graph_socket_styles.json) へ名前を足すだけで絵が付きます。

### 評価の分類(direct / replay / baked / recorded)と変更の費用(value / recompile / rebuild)

同じ「1 つの値を計算するノード」でも、**過去に何が起きたかに依存するか**で扱いが変わります。よく使われる 4 分類が、入力だけで決まる `direct`、前回までの評価に依存する `replay`(クロック、イベント消費、積分器)、事前に焼いた結果を読む `baked`、外部から記録された列を再生する `recorded` です。この分類が要るのは、**「もう一度評価してよいか」が分類ごとに違う**からです。`direct` は何度評価しても同じ、`replay` は 2 回評価すると時計が 2 回進みます。

対になるのが**パラメータを変えたときの費用**の分類で、値だけ差し替えれば済む `value`、シェーダなどの再コンパイルが要る `recompile`、資源ごと作り直す `rebuild` の 3 つです。エディタはこれを見て「触った瞬間に反映してよいか、確定してから作り直すか」を決められます。

**小さな例**: UI のスライダを 1 フレームに 60 回動かすとします。`value` のパラメータなら毎回そのまま適用してよく、`rebuild` のパラメータなら操作が終わるまで適用を遅らせるのが正解です。分類が無いと、どちらも同じ扱いになり、前者は不必要に重く、後者は 1 秒間に 60 回の再構築になります。

**使用場所**: [`GraphOperationEvaluation`](../../src/project/graphcomposition.hpp#L132) / [`GraphParameterChange`](../../src/project/graphcomposition.hpp#L136) / 状態無しで `replay` を評価しようとしたときの拒否 [`graph.evaluation_requires_state@1` の送出](../../src/core/userpublic/valuegraph.cpp#L49)

**このコードベースでの注意**: 分類はノードごとに**強い方へ引き上げられます**(§9.24「per-node の評価分類は `max()` でしか動かない」)。また費用側の既定は op ごとの 1 値ですが、パラメータ単位の表(`parameter_changes`)も持てます([宣言](../../src/project/graphcomposition.hpp#L144))。

### 排他 create を「決定」の primitive にする(O_EXCL / hard link)

「一時ファイルに書いて rename で置き換える」は原子的置換の定石です(§10.4 の該当項目)。しかしそれは **既にあるファイルを置き換える**操作で、「この決定は初めて下された」ことは表現できません。もう 1 つの primitive が **排他 create** — POSIX の `open(O_CREAT|O_EXCL)`、`link()`、Windows の `CreateHardLink` のように、**名前を公開し、既に取られていれば失敗する**操作です。これは 1 操作で「作成」と「重複検出」を兼ね、しかも生きたファイルのハンドルを壊さないので、他プロセスがそのファイルを開いていても負けません。

**小さな例**: 「作業が完了した」ことをディレクトリに残したいとします。`state.json` の `status` を `"prepared"` → `"committed"` に書き換える設計は、(a) 書き換えの途中でクラッシュすると状態が読めない、(b) ウイルススキャナがそのファイルを開いていると置き換えに失敗しうる、という 2 つの弱点があります。**別名 `committed` を排他 create する**設計なら、書き換えは一度も起きず、「存在するかどうか」だけが決定になります。掃除の順序だけは考える必要があって、「決定の印」より先に「やることリスト」を消すのが安全側です — 逆順に消して途中で止まると、完了済みの作業が未完了に見えます。

**使用場所**: [`linkExclusive()`](../../src/core/communication/renderconfigtransaction.cpp#L302)(hard link で名前を公開し、失敗は外部改変として報告)/ [`publishCommittedMarker()`](../../src/core/communication/renderconfigtransaction.cpp#L459) / [掃除の順序](../../src/core/communication/renderconfigtransaction.cpp#L676)

### 内容アドレスによる vendoring(sha256 が同一性、ABI の本当の gate は別)

依存を「どこから取ってきたか」ではなく「中身そのもの」で同定する方式を content-addressed(内容アドレス)と呼びます。配布物をプロジェクトの中へコピーして固定することを vendoring と言い、この 2 つを組み合わせると「**外の世界が変わっても、このプロジェクトが読むものは変わらない**」が成立します。記録すべきは名前・版・**インストールしたバイト列全体のダイジェスト**で、ダイジェストは書きっぱなしにせず、**普通の操作の中で再計算して照合する**のが要点です(照合しないダイジェストは、壊れているかどうかを誰も知りません)。

もう 1 つの定石が、**宣言された ABI 版と、実際に強制される gate を混同しない**ことです。マニフェストに「SDK 版 2 が要る」と書いても、それは install 時の検査にすぎません。実際にプロセスを守るのは、ライブラリを読み込むときにローダが解決する印(シンボルの epoch など)です。前者は親切な早期拒否、後者が本物の強制です。

**小さな例**: パッケージ `acme.glowkit` を入れ、`project.json` に `{namespace, package_version, sha256}` を記録します。誰かが `packages/acme.glowkit/` の中のファイルを手で書き換えると、一覧表示のたびにダイジェストが再計算されて食い違いが出ます。逆に、記録を `package_version` だけにしていたら、同じ版番号のまま中身が違う 2 つの状態を区別できません。

**使用場所**: [`packageTreeDigest()`](../../src/project/packagemanifest.hpp#L100)(ソートした相対パス順に全ファイルを digest。`package list` が再計算する)/ [`InstalledPackage`](../../src/project/packagemanifest.hpp#L70) / [`packageSdkAbiVersion()` のコメント](../../src/project/packagemanifest.hpp#L76)(「本当の gate はローダが解決する epoch anchor」)

### 連続時刻を固定ステップのタイムラインへ量子化する

「t 秒から出力したい」という要求を、固定ステップで進むシステムへ渡すときは、**どこで frame 番号へ落とすか**を 1 箇所に決めるのが定石です。境界で `round(t·fps)` して以降は整数だけを運ぶと、システムの内部に「ステップの間の時刻」が入り込みません。丸め方は選択で、`round` は「最も近いフレーム」、`floor` は「その時刻を過ぎていないフレーム」を意味します。**どちらでも動くが結果は 1 フレームずれる**ので、連番出力の byte 比較のような検証を持つなら先に決めて固定します。

もう 1 つ、**時刻を指定することと seek できることは別**です。フレームの見た目が「その時刻までに何が起きたか」に依存する(クロックが評価のたびに進む、システムが自分で dt を積分する)なら、任意の時刻へ飛ぶことは原理的にできません。この場合の正直な実装は、**そこまでを実際に回してから出力を始める**ことです。

**小さな例**: `fps = 60` で `t = 2.0` を指定すると frame 120 になります。`--frames 2` なら出力される 2 枚は、通常実行の 120 枚目・121 枚目と**バイト単位で一致**していなければおかしい。ここで丸めを `floor(t·fps − ε)` に変えると 119 が選ばれ、この一致が壊れます(壊れ方がテストで見えるので、丸めの選択はテストで固定できます)。

**使用場所**: [`parseStartFrame()`](../../src/player/main.cpp#L284)(`llround(seconds * fps)`。テキストとして parse し、綴り違い・非有限・負を 1 つの名前へ集約)/ [replay prologue](../../src/core/appflow/loop.cpp#L672)(`start_frames − 1` フレームを capture 無しで回してから出力を始める)

## 関連文書

- [README](README.md) — 読む順番と難所インデックス
- [第6章 描画・Vulkan・Shader](06_rendering_vulkan_shader.md) — §10.1 / §10.6 の知識が最も濃く要る章
- [第3章 プロジェクトデータとロード](03_project_and_loading.md) — §10.2 の glTF まわり
- [第9章 黒魔術・制約](09_black_magic_and_gotchas.md) — §10.3 の C++ イディオムが「なぜこの形か」の答え
