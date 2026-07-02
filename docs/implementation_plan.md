# 実装指示書(コーディングエージェント向け)

対象読者: 実装を担当するコーディングエージェント。各 Work Package (WP) は独立に依頼できる単位として書かれている。

設計の正は以下の 3 文書。本書と矛盾したら設計文書を優先し、矛盾を発見したら作業を止めて報告すること。

- `docs/design_roadmap_renderworld.md` — 全体順序と ECS 境界
- `docs/design_headless_rendering.md` — ヘッドレス描画の設計(以下 [HL])
- `docs/design_shader_freedom_kit.md` — シェーダ基盤の設計(以下 [SF])
- `docs/design_project_format.md` — プロジェクト形式の設計(以下 [PF]。v6 凍結)
- `docs/design_project_format_web_profile.md` — 共通形式 Web プロファイル(以下 [PFW])

改訂履歴: v2 で実装者レビューを反映し WP を再分割・採番し直した。旧番号との対応: 旧WP1→WP1、旧WP2→WP3、旧WP3→WP4+5+6、旧WP4→WP7、旧WP5→WP8、旧WP6→WP9、旧WP7→WP10、旧WP8→WP11、旧WP9→WP12、旧WP10→WP13、旧WP11→WP14、旧WP12→WP15、旧WP13→WP16。WP2(EngineTime)は新設。
v3(2026-07-02): WP1〜17 完了を受けて WP18(プロジェクト形式)・WP19(シェーダ stem)を追加。設計の正に [PF] / [PFW] を追加。web 側の対応作業(WW1〜3)は my_webpage リポジトリの `docs/implementation_plan_web.md` にある(本書の管轄外)。

## 0. 全 WP 共通規則

### ビルド・テスト

```sh
cmake . -B build -DCMAKE_PREFIX_PATH=<Qt install path>   # 初回のみ。Qt 不要の作業は -DSKIP_DEVSTUDIO
cmake --build ./build --config Debug
ctest --test-dir ./build -C Debug --output-on-failure
```

- 完了条件は常に「ビルド成功 + 全テストグリーン + `git diff --check` クリーン」
- 描画挙動に触れる WP は `pelican_player.exe` の短時間起動確認も行う(`docs/rendering_phase1_review.md` の Validation Run と同じ流儀)

### コード規約(既存コードから踏襲)

- C++20、vulkan.hpp(C API 直接使用禁止)、リソースは `vk::UniqueXxx` / `ImageWrapper` / `BufferWrapper`
- 型は CamelCase、関数は lowerCamelCase、ファイルは小文字連結(例 `rendertarget.cpp`)、メンバは snake_case
- モジュールは `DECLARE_MODULE(Name)` + `GET_MODULE(Name)`(container.hpp)。**新規コードではモジュール間依存を関数引数の依存構造体で明示する**(render_pass_dispatch.hpp の `XxxDependencies` 構造体が手本)
- ID 型は `PELICAN_DEFINE_HANDLE`(handle.hpp)、ログは quill の `LOG_INFO(logger, ...)` 系
- エラーは fail-fast(`throw std::runtime_error`)。ただしシェーダコンパイル失敗のみ result 返却([SF] §4.1)
- 外部ライブラリ追加はルート CMakeLists.txt の FetchContent 節に追記

### レイヤ規則

- `src/core/vkcore` から `src/core/renderingpass` 以上のレイヤへ新たな include を**追加しない**(通知はフラグ/イベントで上に渡し、編成は上位層が行う。WP8 が実例)

### テスト規約

- Catch2 v3。`test/CMakeLists.txt` の `pelican_define_test(<name> [libs...])` で登録
- GPU 必須テストは Vulkan デバイス列挙失敗時に `SKIP()` すること

### 禁止事項

- `src/core/ecs/` 以下の変更(担当者が別。必要が生じたら止めて報告)
- 無関係箇所のリフォーマット・リネーム(diff を見る人間のため)
- 挙動変更とリファクタの同一コミット混在
- 1 WP = 1 ブランチ = 1 PR。ブランチ名は `agent/wp<番号>-<短い説明>`

## 1. WP 一覧と依存関係

| WP | 内容 | 依存 | 規模 | リスク |
|----|------|------|------|--------|
| 1 | EngineLaunchConfig + player CLI 引数 | なし | 小 | 低 |
| 2 | EngineTime(仮想時刻の注入) | 1 | 小 | 低 |
| 3 | RenderTarget の facade 化(移動のみ) | なし | 中 | 中 |
| 4 | surface なし Vulkan 起動 | 1 | 小 | 中 |
| 5 | OffscreenFrameTarget | 3, 4 | 中 | 中 |
| 6 | readback + PNG 出力 + CLI 完成 | 2, 5 | 中 | 低 |
| 7 | headless 描画テストを ctest に追加 | 6 | 小 | 低 |
| 8 | リサイズ時の RT 追従修正 | 3 | 中 | 中 |
| 9 | 遅延破棄機構(DeletionQueue) | なし | 中 | 低 |
| 10 | shaderc / spirv-reflect / stb 導入 | なし | 中 | **高(環境差分)** |
| 11 | ShaderCompiler + ShaderReflection + 単体テスト | 10 | 大 | 中 |
| 12 | ShaderLibrary | 11 | 中 | 低 |
| 13 | PipelineFactory(fullscreen 限定)+ fullscreen 移行 | 9, 12 | 中 | 中 |
| 14 | シェーダホットリロード | 13 | 中 | 中 |
| 15 | MaterialContainer / UiRenderer 移行 + desc 一般化 + set 規約 | 13 | 大 | 高 |
| 16 | ゴールデンイメージテスト基盤 | 7, 11 | 中 | 低 |
| 17 | SeqPlayer(transform_seq 再生) | 1, 2 | 中 | 低 |
| 18 | プロジェクト形式(project.json + PathResolver + example 切り出し) | 1 | 大 | 中 |
| 19 | シェーダ stem 解決 + rendering config の struct 化 | 12, 18a/b | 中 | 中 |
| 25 | pelican.scene v1(エンベロープ + light コンポーネント化) | 18a/b | 中 | 中 |
| 26 | EXR リーダ(tinyexr・限定スコープ) | 18a/b | 小〜中 | 低 |

(WP20〜24 は設計文書側で候補予約のみ: WP20 VAT 再生 / WP21 pelican_cli import /
WP22 pointcache / WP23 KTX2 / WP24 音声。着手前に本書へ正式記載する)

- **並列依頼可能**: WP1, 3, 9, 10(互いに独立)。WP2 は WP1 直後に
- **クリティカルパス**: 1 → 4 → 5 → 6 → 7(headless 検証基盤)
- **DCC bridge M1(Blender からのプレビュー)の成立条件 = WP6 + WP17**(WP17 単体は WP2 後に実装・通常起動で確認可能)
- **共通プロジェクト形式(2026-07 開始)のクリティカルパス = 18 → 19**。web 側 WW1〜3([PFW] §8)は形式凍結済みのため WP18 のエンジン実装完了を待たずに並行できる
- compute、GPU 計測、bindless、RT、コマンド層、RenderWorld は §3 参照(設計合意・前提 WP 完了待ち)

## 2. WP 詳細

### WP1: EngineLaunchConfig + player CLI 引数

参照: [HL] §3.3, §3.4

1. `src/core/launchconfig.hpp` を新規作成。[HL] §3.3 の `EngineLaunchConfig` を**純データのモジュール**として定義(`DECLARE_MODULE`、メンバ public でよい)
2. `src/player/main.cpp` に argparse を導入(devcli/main.cpp に使用例あり)。受けるオプション: `--headless`(flag)、`--frames N`(既定 3)、`--size WxH`(既定 1280x720)、`--render-out <path>`、`--fps F`(既定 60、WP2 の fixed_step 用)
3. parse 結果を `GET_MODULE(EngineLaunchConfig)` に書き込んでから `PelicanCore::run()` を呼ぶ。**この WP では値は誰も読まない**(配線のみ)

受け入れ基準: 引数なしで従来通り起動。`--headless` 指定時はログに「headless requested but not implemented yet」。`--help` が動く。

### WP2: EngineTime(仮想時刻の注入)

参照: ロードマップ §2.1。設計文書がないため本節が仕様。

目的: 壁時計依存を排し、headless・ゴールデンテスト・将来の `set_time`/`step_frame` コマンド・動画レンダリングの決定性を確保する。

1. `src/core/appflow/enginetime.{hpp,cpp}` を新規作成:

```cpp
DECLARE_MODULE(EngineTime) {
  public:
    enum class Mode { realtime, fixed_step };
    void setup(Mode mode, double fixed_dt);  // Loop 起動時に LaunchConfig から決定
    void advance();      // Loop のみが毎フレーム呼ぶ。realtime: 壁時計差分(上限 0.1s クランプ)、fixed_step: fixed_dt
    void setTime(double t);                  // 将来のコマンド層用
    double now() const;                      // 起動からの秒
    double dt() const;
    uint64_t frameIndex() const;
};
```

2. **進めるのは `Loop::run()` のみ**(`ecs.update()` の前で `advance()`)。他モジュールは読み取り専用
3. `Renderer::render()` の `static const auto start_time = FrameClock::now()` と壁時計参照(vkcore/renderer.cpp)を削除し、`GET_MODULE(EngineTime).now()` に置換。`light_container.updateAnimation(time)` の引数も同じ(**呼び出し位置は変えない**。ECS 側への移管は RenderWorld トラックの仕事)
4. モード決定: headless 時は `fixed_step`(dt = 1/fps)、通常起動は `realtime`
5. 単体テスト(GPU 不要): fixed_step で `advance()` N 回 → `now()` が N×dt と一致、frameIndex 単調増加、setTime 反映

受け入れ基準: 通常起動の見た目が従来と変わらない(ライトアニメ速度含む)。テストグリーン。`src/core` から `high_resolution_clock` の描画系参照が EngineTime 内部だけになる。

### WP3: RenderTarget の facade 化(移動のみ)

参照: [HL] §3.1, §3.2

**このWPは「コードの移動 + 薄い委譲」以外を一切含まない。** resize 修正(WP8)や headless 分岐(WP5)を混ぜないこと。

1. `src/core/vkcore/frametarget.hpp` を新規作成し、`IFrameTarget` と `FrameTargetCaps` を定義([HL] §3.2。`readbackLastFrameRGBA8` は宣言のみで `SwapchainFrameTarget` では `throw`)
2. 現 `RenderTarget` の実装(swapchain 作成、acquire/present、`recreateSurfaceDependants` 等)を `src/core/vkcore/swapchainframetarget.{hpp,cpp}` の `SwapchainFrameTarget : IFrameTarget` へ**移動**(ロジック変更禁止。`recreateSurfaceDependants` の中身も現状のまま)
3. `RenderTarget` モジュールは `std::unique_ptr<IFrameTarget> impl` を持つ facade にし、既存公開 API(`render_begin` / `render_end` / `getSwapchainFormat`)を委譲で維持。コンストラクタでは常に `SwapchainFrameTarget` を生成
4. **固定事項**: `FrameRenderContext` と `in_flight_frames_num` の定義は `rendertarget.hpp` に残す(他ファイルの include を壊さない)。`RenderTargetLayoutTracker` には触れない

受け入れ基準: 全呼び出し側が無変更でビルドが通る。player 起動・リサイズ・終了が従来通り。diff が「移動 + 委譲」だけであることを PR 説明で明示。

### WP4: surface なし Vulkan 起動

参照: [HL] §3.3

1. `VulkanManageCore` に headless 分岐を追加: `EngineLaunchConfig.headless` 参照。GLFW 拡張を要求しない、surface 非作成、`pickPhysicalDevice` / `pickQueues` の present サポート条件を外し `presentation_queue = graphic_queue` に縮退
2. **`GET_MODULE(Window)` が headless 経路で一度も呼ばれないこと**(呼ぶと GLFW が初期化される)。`getSurface()` は headless 時 throw
3. 単体テスト: headless 設定で `VulkanManageCore` を初期化し device 取得成功を確認(デバイスなし環境は SKIP)
4. この WP では描画はまだ動かなくてよい(`RenderTarget` 初期化が swapchain 前提のため)。テストは VulkanManageCore 単体まで

受け入れ基準: テストグリーン。通常起動無変更。

### WP5: OffscreenFrameTarget

参照: [HL] §3.2, §3.5

1. `src/core/vkcore/offscreenframetarget.{hpp,cpp}` に実装。要点([HL] §3.2): in-flight 1、セマフォなし(`image_prepared_semaphore = VK_NULL_HANDLE`)、`required_layout = eTransferSrcOptimal`、color は RGBA8 + usage COLOR_ATTACHMENT | TRANSFER_SRC、depth D32
2. `RenderTarget` facade のコンストラクタで headless なら `OffscreenFrameTarget` を選択
3. `Loop::run` の headless 分岐: `window.process()` / `framerate_adjuster.wait()` を呼ばず `frames` 消化で終了([HL] §3.4)
4. `cmdbuf.recordEndSubmit` は既定引数で空セマフォ許容済み(変更不要)。fence 同期のみで回す
5. 終了経路: `Loop::run()` 終了時に `VulkanManageCore::waitIdle()` を明示(WP9 の flushAll 挿入点になる)

受け入れ基準: `pelican_player --headless --frames 3` が GUI なし・validation エラーなしで終了コード 0。通常起動無変更。readback はまだなくてよい。

### WP6: readback + PNG 出力 + CLI 完成

参照: [HL] §3.2, §6

1. `OffscreenFrameTarget::readbackLastFrameRGBA8()`: fence 待ち → `copyImageToBuffer` で host visible staging へ → memcpy(`VulkanUtils` の staging 実装を参考に)
2. `RenderTarget::captureLastFrameToPng(path)`: stb_image_write 使用(WP10 と独立に、この WP で stb のみ先行導入してよい)。BGRA の場合は R/B スワップ
3. `--render-out` 完成: パスに `%d` 系書式を含む場合は毎フレーム連番保存、含まない場合は最終フレームのみ
4. EngineTime(WP2)の fixed_step と組み合わせ、同一引数 2 回実行で出力 PNG が一致することを確認(完全一致が GPU 的に安定しない場合はその旨を PR に記録。判定は WP16 の tolerance 比較に委ねる)

受け入れ基準: `pelican_player --headless --frames 3 --render-out out.png` が妥当な PNG(example シーンで非単色)を出力。`--render-out out/%04d.png` で 3 枚出る。

### WP7: headless 描画テスト

参照: [HL] §4

1. `test/headless_render_test.cpp` を新規作成、`pelican_define_test(headless_render_test pelican_core)` で登録
2. テスト内容: `EngineLaunchConfig` を headless + fixed_step に設定 → 最小構成で 3 フレーム描画 → `readbackLastFrameRGBA8` → (a) サイズが extent と一致 (b) 全ピクセル同一でない(クリアのみの場合はクリア色一致)、を assert
3. Vulkan デバイス取得失敗(インスタンス作成 throw 含む)は `SKIP("no vulkan device")`
4. シーン構築は player の resources を流用せず、テスト内で最小限に

受け入れ基準: GPU ありで PASS、GPU なしで SKIP。ctest 一覧に表示される。

### WP8: リサイズ時の RT 追従修正

参照: [HL] §2.3, §3.6

**編成方針(固定)**: `SwapchainFrameTarget` は extent 変更を**フラグで通知するだけ**にし、RT 再生成・再バインド・tracker リセットの編成は `Renderer` が行う(vkcore → renderingpass の依存を作らないため。§0 レイヤ規則)。

1. `IFrameTarget` に `bool consumeExtentChanged()` を追加。`SwapchainFrameTarget::recreateSurfaceDependants()` がフラグを立てる
2. `rendertargetjsonparser` が読んだ `extent_scale` を `RenderTargetContainer::InternalRenderTarget` まで保存(現在は `rendertargetconfigregistration.cpp` で計算後に捨てている)
3. `RenderTargetContainer::recreateForExtent(vk::Extent2D base_extent)`: scale 付き RT を再確保。ID と `name_to_id` は維持(image/view のみ差し替え)
4. `Renderer::render()` で `render_begin()` 後にフラグを確認し、立っていれば (a) `recreateForExtent` (b) compiled pass の `input_targets` 再バインド(`FullscreenPassContainer::setInputTextures` 再実行。`RenderingPassContainer` に compiled pass 列挙の getter がなければ最小の getter を追加) (c) `RenderTargetLayoutTracker` リセット。`recreateSurfaceDependants` 内の `device.waitIdle()` が先行しているため安全
5. 旧 image/view の破棄は WP9 完了済みなら `DeletionQueue` 経由、未完了なら waitIdle 直後の即時破棄でよい(PR に明記)

受け入れ基準: ウィンドウをリサイズしても描画が壊れない(引き伸ばしボケなし、validation エラーなし)。手動確認手順を PR 説明に記載。

### WP9: 遅延破棄機構(DeletionQueue)

参照: ロードマップ本線 2。設計文書がないため本節が仕様。

1. `src/core/vkcore/deletionqueue.{hpp,cpp}`:

```cpp
// 純ロジック。モジュール機構・Vulkan 非依存 → GPU 不要で単体テスト可能
class DeletionQueueCore {
    uint64_t current_frame = 0;
    uint32_t in_flight_frames;
    std::function<void()> wait_idle_hook;
    std::vector<std::pair<uint64_t, std::function<void()>>> pending;
  public:
    DeletionQueueCore(uint32_t in_flight_frames, std::function<void()> wait_idle_hook);
    template <class T> void defer(T &&resource);   // move-only リソースを抱えて遅延破棄
    void beginFrame();   // current_frame++ し、(current_frame - in_flight_frames) 以前を実行
    void flushAll();     // 全破棄。呼び出し前に wait idle 済みであることが契約
    ~DeletionQueueCore();// pending 非空なら wait_idle_hook() + flushAll + LOG_WARNING(安全網)
};

DECLARE_MODULE(DeletionQueue) {    // 薄いラッパ。寿命ピン留めはこちらだけが担う
    DeletionQueueCore core;
  public:
    DeletionQueue();     // ctor で GET_MODULE(VulkanManageCore) を呼び(下記・生成順ピン留め)、
                         // 実 waitIdle を hook として core に注入。in_flight_frames_num も渡す
    // defer / beginFrame / flushAll を core へ委譲
};
```

2. **寿命保証(本 WP の核心)**: `FastModuleContainer` は生成の逆順で破棄するため、ラッパの ctor で `GET_MODULE(VulkanManageCore)` を呼んで「core より後に生成される」ことをピン留めする。これにより DeletionQueue は core より**先に**破棄され、deleter は常に生きた device 上で走る。ロジックを `DeletionQueueCore` に分離するのは、この ctor 依存が単体テストに Vulkan 初期化を要求しないようにするため(`dcc_integration_qa_2026-06-12.md` §9)
3. **明示 flush 点**: `Loop::run()` の終了直前に `GET_MODULE(VulkanManageCore).waitIdle(); GET_MODULE(DeletionQueue).flushAll();` を置く(WP5 で入れた waitIdle の直後)。headless 0 フレーム終了でもこの経路を必ず通す。`beginFrame()` は `Renderer::render()` 先頭で呼ぶ
4. 単体テスト(GPU 不要): `DeletionQueueCore` を直接生成(wait_idle_hook はカウンタ付きフェイク)し、カウンタを抱えた fake リソースで、in_flight 数経過後に deleter 実行・flushAll で全実行・dtor 安全網の hook 呼び出し+警告、を検証

受け入れ基準: テストグリーン。既存コードへの組み込みは beginFrame/flushAll の挿入のみ(利用開始は WP8/13 以降)。

### WP10: 外部ライブラリ導入(shaderc / spirv-reflect / stb)

参照: [SF] §3。**規模表記は中だが環境差分リスクが高い。ハマったら戦況報告を優先すること。**

方針(固定):

- **Vulkan SDK 同梱の `shaderc_combined` を第一候補**(`find_library`)。見つからない場合のみ FetchContent(`google/shaderc`、SHADERC_SKIP_TESTS/EXAMPLES/INSTALL を ON)にフォールバック。**フォールバックは最初から実装する**
- CMake オプション `PELICAN_RUNTIME_SHADER_COMPILER`(既定 ON)を新設。OFF 時は shaderc 依存ごとコンパイルアウトし、ShaderLibrary は .spv 直読みのみになる(ロードマップ §6 のオプション化基準に合致)
- **MSVC の既知の罠**: SDK の `shaderc_combined.lib` は Release CRT でビルドされており、Debug ビルド(/MDd)とリンクすると CRT 不一致になる。Debug 構成では `shaderc_combinedd.lib`(SDK に同梱されていれば)を使い、なければ Debug のみ FetchContent 側に倒す。この分岐を CMake に書き、PR で挙動を説明すること

1. 上記方針で shaderc を導入
2. spirv-reflect(`KhronosGroup/SPIRV-Reflect`、静的ライブラリ設定)、stb(ヘッダオンリー、`stb_image_write.h` のみ。WP6 で導入済みならスキップ)を FetchContent 追加
3. リンク確認の最小テスト(`shaderc::Compiler` 生成、`spvReflectCreateShaderModule` 空呼び出し)を `test/shader_compile_test.cpp` の雛形として追加

受け入れ基準: Windows/MSVC で Debug/Release 両構成のクリーンビルドが通る。`PELICAN_RUNTIME_SHADER_COMPILER=OFF` でもビルドが通る。ビルド時間の増分を PR 説明に記載。

### WP11: ShaderCompiler + ShaderReflection

参照: [SF] §4.1, §4.2(API はそちらが正)

1. `src/core/shader/shadercompiler.{hpp,cpp}`: [SF] §4.1 の API。GLSL のみ実装(HLSL/DXC、Slang は将来 WP。ただし `ShaderCompileOptions` の形は最初から [SF] 通り)。ステージ推定は拡張子テーブル
2. `src/core/shader/shaderreflection.{hpp,cpp}`: [SF] §4.2 の API + descriptor set layout / pipeline layout 生成ヘルパ
3. 単体テスト(GPU 不要): 実在シェーダ(default.vert 等)のコンパイル成功と SPIR-V マジック、不正 GLSL で ok=false + log 非空、include 解決、リフレクション結果が現行手書き layout(materialcontainer.cpp)と一致
4. **既存の描画経路には手を入れない**

受け入れ基準: テストグリーン。既存挙動無変更。

### WP12: ShaderLibrary

参照: [SF] §4.3

1. `src/core/shader/shaderlibrary.{hpp,cpp}`: [SF] §4.3 の API。`loadFromFile` は「.spv = 直読み」「それ以外 = ShaderCompiler 経由」(`PELICAN_RUNTIME_SHADER_COMPILER=OFF` 時は .spv のみ受理し他は throw)
2. **リロードのトランザクション規約([SF] §7.5。本 WP で実装)**: `reload()` は新 module + 新 reflection が**完成するまで現 Bundle を一切変更しない**。失敗時は旧版がそのまま生き、戻り値 false + ログのみ
3. `renderingpassruntimecompiler.cpp` の `registerShaderFromFile()` を ShaderLibrary 経由に置換
4. 既存 `ShaderContainer` は削除しない(将来 WP で整理)
5. 単体テスト: version 増加、失敗時の旧版維持、takeDirtyBundles の消費動作

受け入れ基準: 既存 JSON 設定(.spv 指定)が無変更で動く。.frag 直指定の動作確認を 1 ケース追加。

### WP13: PipelineFactory(fullscreen 限定)+ fullscreen 移行

参照: [SF] §4.4, §5, §7

**スコープ(固定)**: この WP の PipelineFactory は**fullscreen パスを動かすのに必要な最小限**とする。`GraphicsPipelineDesc` は `vert/frag/color_formats` + 固定ステート(vertex input なし、depth test/write なし、blend なし、cull なし、topology = TriangleList)のみ。vertex layout・blend・depth・cull の一般化は **WP15 で行う**。「汎用ファクトリを最初から作らない」こと。

1. `src/core/shader/pipelinefactory.{hpp,cpp}`: 上記最小 desc。descriptor set layout はリフレクションから自動生成、同一レイアウトのハッシュ共有、`VkPipelineCache` のディスク永続化(実行ファイル隣 `pipeline_cache.bin`)
2. 旧パイプライン破棄は `DeletionQueue::defer`(WP9)経由
3. `FullscreenPassContainer::registerFullscreenPass` の内部を PipelineFactory 呼び出しに置換。手書き descset_layouts と齟齬があれば**リフレクション由来を正**とし、シェーダ側 binding が [SF] §5 規約とずれる箇所はシェーダを直す
4. headless(WP6)で全 fullscreen パス(bloom 系、ssao、debug_texture)の移行前後 PNG を比較

受け入れ基準: 描画結果が移行前と一致(PNG 比較)。`rebuildDirty()` は空実装でよい(WP14 で完成)。

### WP14: シェーダホットリロード

参照: [SF] §7、トランザクション規約は [SF] §7.5

1. `std::filesystem::last_write_time` のポーリング(1 秒間隔、ShaderLibrary のソースパス一覧を走査)。専用スレッドなし、フレームループから呼ぶ
2. 安全点(`Renderer::render()` 冒頭、`DeletionQueue::beginFrame()` の後)で: dirty 検出 → `ShaderLibrary::reload`(失敗時は旧版続行)→ `PipelineFactory::rebuildDirty`
3. **`rebuildDirty()` のトランザクション規約**: 新パイプライン生成に**成功してから** handle の指す先を swap する。生成失敗時は旧パイプライン続行 + ログ。旧パイプラインは必ず `DeletionQueue::defer` へ
4. `EngineLaunchConfig.shader_hot_reload`(既定: 通常起動 true、headless false)
5. 結合テスト: シェーダ書き換え → reload → version 増加と新パイプライン生成。壊れた GLSL で旧版維持

受け入れ基準: player 起動中に .frag を編集保存すると次フレーム以降反映。壊れた GLSL を保存しても落ちず、修正保存で復帰する。

### WP15: MaterialContainer / UiRenderer 移行 + desc 一般化 + set 規約適用

参照: [SF] §5, §7

1. `GraphicsPipelineDesc` を一般化: `use_engine_vertex_layout`、depth test/write、cull、blend、topology([SF] §4.4 の完全形)。WP13 の fullscreen 利用箇所が壊れないことをテストで担保
2. `default.vert/frag`、`ui.vert/frag`、ライト UBO の set/binding を [SF] §5 規約に揃える(`lightDescriptorSetNumber = 2` の付け替え等)。`shaders/include/pelican_sets.glsl` を新設し全シェーダが include
3. `MaterialContainer` / `UiRenderer` の手書き descset_layouts・pipeline 生成を PipelineFactory + リフレクション由来に置換
4. push constant を [SF] §5 規約(先頭 64B エンジン予約 / 後半 64B 自由)に整理
5. PNG 比較で移行前後の一致確認

受け入れ基準: 描画結果一致。material/fullscreen/ui 経路から `vk::DescriptorSetLayoutCreateInfo` の手書き構築が消えている。

### WP16: ゴールデンイメージテスト基盤

参照: [SF] §8、[HL] §6

**比較方針(固定)**: ピクセル厳密一致は採用しない。per-pixel 差の**平均と最大**を case ごとの tolerance ファイルと比較する。GPU/driver 名(`VkPhysicalDeviceProperties::deviceName`)を実行時に取得し、失敗レポートと golden メタデータに記録する。

1. `test/golden/<case>/`(シェーダ + scene.json + expected.png + tolerance.json)をディレクトリ走査して Catch2 動的テストケース化
2. 失敗時は actual.png / diff.png を `build/test_artifacts/<case>/` に出力
3. 初期ケース 3 つ: クリア色のみ / default マテリアルの三角形 1 枚 / fullscreen パス 1 段
4. 期待画像の再生成手段(`--update-golden` 相当)を用意
5. GPU なし環境は SKIP(lavapipe/SwiftShader の導入は将来検討。本 WP ではやらない)

受け入れ基準: 3 ケースが GPU あり環境で PASS、GPU なしで SKIP。意図的にシェーダを壊すと FAIL し diff.png が出る。

### WP17: SeqPlayer(transform_seq 再生)

参照: `docs/dcc_integration_qa_2026-06-12.md` §4(設計の経緯)、要求書 R4(`hidden` 追補含む)。

**位置づけ**: SeqPlayer 単体は WP2 完了後に実装・通常ウィンドウ起動で確認可能。**DCC bridge M1(Blender からのプレビュー)として成立するのは WP6 + 本 WP の両方完了後**。droplet/cloth ツールの最初の納品経路の前提でもある。

1. `src/core/playback/seqplayer.{hpp,cpp}`(core モジュール。`SceneLoader` が core/loader に居る前例に合わせる)。**JSONL のパースと時刻サンプリングはモジュール非依存の plain クラスに分離**し、GPU 不要で単体テスト可能にする(WP9 の Core 分離と同じ方針)
2. CLI 追加(WP1 の argparse): `--play-seq <path.jsonl>`、`--seq-mesh <builtin:sphere|path.glb>`、`--seq-loop`(flag)
3. 起動時: ヘッダ行を検証(schema/version 不一致は fail-fast)、objects ごとに `PolygonInstanceContainer::placeModelInstance`。v1 は**全オブジェクト共有メッシュ 1 つ**(`builtin:sphere` は単位球を生成、glb 指定時は最初のメッシュ)。オブジェクト名→glb ノード名の個別対応は v2
4. 毎フレーム(`ecs.update()` 後・`renderer.render()` 前に Loop から呼ぶ。core→core なのでレイヤ規則に抵触しない): `EngineTime.now()` で該当サンプル行を選択(v1 は floor サンプル・補間なし、末端 clamp、`--seq-loop` で周回)、全インスタンスに `setTrs`
5. `hidden`(R4 追補)の扱い: 契約は「描かない」。現行の indirect draw 構成でインスタンス単位スキップが重い場合、**v1 の内部実装は scale 1e-6 への縮退で代用してよい**(0 でなく ε なのは法線行列の特異化回避。RenderWorld の draw command seam 実装後に真の draw skip へ置換)
6. `--camera "px,py,pz,tx,ty,tz,fov_deg"`(glTF 座標、position / 注視点 / 垂直 FOV 度)。省略時は既存シーンカメラ。プレビューのフレーミングに必須(`dcc_integration_qa_2026-06-12.md` §12)
7. テスト: (a) パース+サンプリング単体(GPU 不要、固定 fixture)、(b) headless 結合: 2 オブジェクト×3 フレームの golden jsonl → `--render-out` 連番で位置が動くこと(WP7 の流儀)

受け入れ基準: `pelican_player --headless --frames 90 --play-seq droplets.jsonl --seq-mesh builtin:sphere --camera "0,1,3,0,0.5,0,40" --render-out out/%04d.png` で球が動く連番が出る。通常起動無変更。

### WP18: プロジェクト形式(project.json + PathResolver + example 切り出し)

参照: **[PF] §5 が仕様の正**(API・解決手順・レビュー済み事故ポイントまで確定済み)。本節は作業分割と追加成果物のみ。[PF] §3(パス分類)・§4(project.json v1 と設定優先順位)を実装前に必ず読むこと。

**規模が大きいので 3 PR に分割してよい**(ブランチは `agent/wp18a-...` 等)。分割する場合も受け入れ基準は最終 PR で全項目を満たす。

1. **WP18a — PathResolver 基盤**(他と独立に完結):
   - `core/loader/pathresolver.{hpp,cpp}`: [PF] §5-2 の API・解決手順どおり。**Windows の case-fold 比較・path component 単位判定・`weakly_canonical` の error_code 版**は [PF] に明記された事故ポイントなので省略しないこと
   - `core/loader/engineresources.{hpp,cpp}`: EngineResourceRegistry(v1 は `default_config.json` 1 エントリ手書き。未知 id の throw に登録済み一覧を含める)
   - 単体テスト: [PF] 受け入れ基準 d, g, j, k に対応するケース(GPU 不要)
2. **WP18b — 配線と読み込み置換**:
   - **PathResolver の参照規則追従(WW1 レビュー由来・[PFW] §2-6)**: `resolveRef` で
     (a) `\` を含む参照 (b) 空文字列の参照 を reject(web 実装と同一挙動)。
     fixture に `invalid/backslash_separator.json`・`invalid/empty_ref.json` を追加し
     expectations.json を更新(error_kind は `separator` / `empty`)
   - `--project <dir|project.json>` を argparse に追加(WP1)。省略時は exe ディレクトリを暗黙ルートとし WARN([PF] §5-3)
   - project.json ロード: `schema`/`version`/`engine_min_version` の hard error ゲート([PF] §4)。設定合成を CLI > project.json > embedded default の 3 段に
   - ProjectBasicConfig / ModelAssetContainer / シーン・UI・パス読み込みのファイルアクセスを PathResolver 経由に置換(**cwd 参照の根絶**。`GET_MODULE(PathResolver)` 直呼びは core/loader と起動配線のみ、他層は依存構造体経由 — [PF] §5-2)
   - 単体テスト: [PF] 受け入れ基準 e, h, i
3. **WP18c — example プロジェクト切り出し**(ウェーブ 9: WP19・WP25 マージ後。
   2026-07-02 実装・レビュー済み。実装時の設計追記: 暗黙プロジェクトの
   開発ビルド向け fallback = [PF] v6.1):
   - `src/player/resources/` → `projects/example/` へ [PF] §2 レイアウトで移設。POST_BUILD コピー削除。`projects/example/README.md`(バイナリアセット一覧表: ファイル名/入手元/sha256/サイズ)を作成([PF] §5-5, 5-6)
   - **rendering config は stem 形式(WP19)、scene は pelican.scene v1(WP25)の
     最終形で書く**。`../../../../src/core/resources/*.spv` 参照はここで完全に消える
   - ctest・golden テストの起動を `--project` 明示に変更
4. **追加成果物(本 WP で新規): 適合 fixture の整備**([PFW] §6-1)。
   `test/fixtures/project_format/` に valid/invalid の JSON 一式と機械可読な期待値 `expectations.json`(`{file, expect: "ok"|"error", error_kind}`)を置く。上記単体テストはこの fixture を読んで走らせる形にする(web 側 WW2 が同じ fixture を取り込むため、**テストコードに JSON をインライン埋め込みしない**)

受け入れ基準: [PF] §5 の a〜k 全項目 + fixture ディレクトリが expectations.json 込みで存在し、C++ テストが fixture 駆動であること。

### WP19: シェーダ stem 解決(共通形式対応)

参照: [PFW] §4(規約の正)、`design_project_dcc_houdini.md` ではなく
**`design_project_interpretation_layer.md` §3-2 を必読**。依存: WP12, WP18。

**WP18b 実装との整合(2026-07-02 決定)**: WP18b の `resolveShaderPaths`
(basicconfig.cpp)は shader 参照を解決済み絶対パスに書き換える方式だが、
stem 参照(拡張子なし)は実在ファイル名が確定しないため両立しない。
**解釈レイヤ設計 §3-2 の (b) を採用**: 本 WP で rendering config の解釈を
struct ベース(パース + 解決済み参照を持つ plain struct)へ寄せ、
`resolveShaderPaths` の JSON 書き換えはこの WP で廃止する。
struct は将来 `pelican_project` ターゲットへ移動できるよう
純ロジック(モジュール非依存)で書くこと(DeletionQueueCore 方式)。

1. ShaderLibrary に stem+ステージ → モジュール解決を追加: `<stem>.vert` / `<stem>.frag`(ソース、実行時コンパイル)を先に試し、無ければ `<stem>.vert.spv` / `<stem>.frag.spv`。`PELICAN_RUNTIME_SHADER_COMPILER=OFF` 時は .spv のみ。パス解決は PathResolver 経由(`engine://` stem も同規則で Registry を引く)
2. rendering pass parser: `shader.vertex` / `shader.fragment` の値に既知拡張子(`.spv` `.vert` `.frag` `.wgsl`)が**ない**場合を stem 参照として受理。拡張子付きは従来どおり動作させつつ、project.json 経由で開いたプロジェクト内では WARN(「バックエンド固有参照・非可搬」)
3. (2026-07-02 訂正: `projects/example` は WP18c で作られるため本 WP の対象外)
   **`src/player/resources/` の既存 config と既存 golden case は書き換えない** —
   レガシー後方互換(受け入れ基準 b)の回帰アンカーとして維持する。
   stem 参照の描画検証は、**fixture ベースの一時プロジェクト**
   (projectconfig_test の ProjectSandbox 方式で stem 参照 config + シェーダを
   生成)を使った golden または headless 比較で行う。
   `../../../../*.spv` 参照の根絶と example の stem 化は WP18c(ウェーブ 9)が行う
4. fixture: stem の valid / invalid(未解決 stem)ケースを `test/fixtures/project_format/` に追加。
   あわせて `engine_resources.json`(engine:// id 一覧の fixture)が
   `registeredEngineResourceIds()` と一致することを検証する単体テストを追加
   (web 側 WW2 の鏡像チェックと同じ fixture を共有する。registry に id を
   増やしたらこの fixture も更新する運用)
5. golden テスト(WP16)で移行前後の描画一致を確認

受け入れ基準: (a) stem 参照で従来と同一の描画(golden 比較) (b) `.spv` 明示参照の後方互換が保たれる (c) 未解決 stem のエラーメッセージに試行したパス一覧(`.vert` → `.vert.spv` の順)が含まれる (d) fixture が expectations.json 込みで追加されている (e) `resolveShaderPaths` による JSON 書き換えが削除され、rendering config の解釈が純ロジックの struct 経由になっている。

### WP25: pelican.scene v1(エンベロープ + light コンポーネント化)

参照: **`design_scene_format.md` §1〜3 が仕様の正**(手順は同 §3 の 1〜6)、
`design_project_interpretation_layer.md` §2(純ロジック規律)。依存: WP18a/b。

補足(ファイル配置と競合回避):

1. エンベロープ検証・レガシー検出(scenes 包み・lights → light コンポーネント変換)・
   name 一意性検証は `core/loader/sceneformat.{hpp,cpp}` に**純ロジック**
   (モジュール機構・GPU 非依存)として実装。GPU 不要の単体テストを付ける
2. SceneLoader(バインダ)と `LightContainer::load` の入力調整は同 §3-2 のとおり
3. **`basicconfig.{hpp,cpp}` には触れない**(WP19 と並走させるための競合回避。
   `sceneDataJson()` は従来どおりテキストを返し、解釈は sceneformat 側で行う)
4. `src/core/ecs/` 以下も触れない(light は ComponentInfoManager を経由しない
   特別扱い — 設計文書 §2-3)

受け入れ基準: (a) v1 形式・レガシー形式の両方でシーンが読める(レガシーは WARN)
(b) golden テストで移行前後の描画一致(ライト経路を触るため必須)
(c) name 重複・schema 不一致・version 超過が hard error(fixture 駆動テスト)
(d) 未知コンポーネント名のエラーに object name が含まれる
(e) fixture が expectations.json 込みで追加されている。

### WP26: EXR リーダ(tinyexr・限定スコープ)

参照: **`design_asset_format_policy.md` §1 が仕様の正**。依存: WP18a/b。

1. tinyexr を FetchContent 追加(ヘッダオンリー。ルート CMakeLists の
   FetchContent 節にアルファベット順で追記)
2. 画像読み込み経路(stb_image を使っている箇所)に `.exr` 分岐を追加:
   single-part・scanline・half/float のみ対応、RGBA16F/32F として GPU へ。
   deep / multi-part / タイルは明確なエラーメッセージで reject
3. テクスチャとしての用途登録(asset_data_json 経由)は既存の画像と同じ扱い
4. 単体テスト: 小さな .exr fixture(数ピクセル、テスト内生成でもよい)の
   読み込み成功 + 非対応 EXR の reject。既存 PNG 経路の無変更をテストで担保

受け入れ基準: (a) .exr テクスチャがプロジェクトから参照・表示できる
(b) 非対応 EXR が用途の分かるエラーで落ちる (c) PNG/JPG 経路が無変更
(d) `PELICAN_RUNTIME_SHADER_COMPILER=OFF` 構成でもビルドが通る。

## 3. 保留中のトラック(WP 化待ち)

- **最小コマンド層**: 3 段階で進める。(1) ファイル連携を正とする(WP6 の `--render-out` で成立済み) → (2) **stdio NDJSON 上の JSON-RPC 2.0** で `set_time` / `step_frame` / `render_frame` / `capture` の 4 メソッドのみ(WP6 完了後に WP 化可能。独自行プロトコルは作らない。TCP 常駐サーバはまだ作らない。要求書 R8 追補参照) → (3) `load_gltf` / `update_transforms` は **RenderWorld 合意後**
- **RenderWorld**: (2026-07-02 方針変更)**ECS 側との合意形成は後回しにし、統合ブランチ系列(`codex/rendering-phase1-refactor` 由来)を当面の開発本線として独自に進める**。JSON 規約(シーン形式等)は現行形式から離れすぎない範囲で本線側が自由に定義してよい(`design_scene_format.md` 参照)。ただし (a) `src/core/ecs/` コア本体の変更禁止は維持(コンポーネント追加は `userpublic/components` で完結するため通常は不要)、(b) main との将来の合流可能性を壊さないため、main に随時追従 merge する運用は続ける。ライトアニメーション更新の ECS 側移管は本線で実施してよい(単独 PR)
- **コマンド層の WebSocket 展開**: stage 2(stdio JSON-RPC)実装後、同じメソッド群を WebSocket に載せると devstudio と web viewer(my_webpage)が同一プロトコルでエンジンを叩ける([PFW] §7)。stage 2 の後に設計文書を書いてから WP 化
- **asset manifest(sha256)**: `assets.manifest.json` + 起動前検証([PF] §5-6 の予告)。WP18c の README 一覧表で当面代替し、需要(=黒背景事故の再発 or web 側キャッシュ検証の要求)が出たら WP 化
- **naga 変換のビルドスクリプト化**: [PFW] §4-3(a) の WGSL→SPIR-V 一括変換を node CLI 化(web repo 側作業。実績コードは `apps/site/src/lib/shader/nagaSpirvCompiler.ts`)。WW3 の後
- **プロジェクト解釈レイヤ(pelican_project 分離)**: `design_project_interpretation_layer.md`(v1 ドラフト)。解釈(パース・検証・パス解決・正規化)をエンジン非依存の静的ライブラリに分離し、エンジン側は薄いバインダにする。WP19 で rendering config の解釈から着手し、CMake ターゲット分離は後続の小 WP(移動+委譲のみ、WP3 の流儀)。WP21(pelican_cli import)がライブラリの最初のエンジン外利用者になる予定
- compute パス / GPU 計測 / bindless / RT: それぞれ設計文書を書いてから WP 化(ロードマップ §2 の順)

## 4. マルチエージェント運用(ブランチとマージ)

### 規則

1. **main を常にグリーンに保つ**: 受け入れ基準を満たした PR だけが main に入る。マージ後に `ctest` が割れたら最優先で revert
2. **1 WP = 1 ブランチ(`agent/wpN-...`)= 1 PR = squash マージ**: main の履歴が「1 コミット ≒ 1 WP」になり、bisect と revert が WP 単位でできる
3. **WP ブランチは統合ブランチ(下記 5)から切る。WP 同士のスタック禁止**: 依存 WP が未マージなら着手しない(待ち時間は別の独立 WP を取る)。マージはレビュー通過後ただちに(長生きブランチを作らない)
4. **契約文書(`external_tools_requirements.md` / schemas)は WP の PR に混ぜない**: 専用 PR+人間レビュー+ツール側コピー同期(`dcc_integration_qa_2026-06-12.md` §3 の規約)
5. **(2026-06-12 改訂)`codex/rendering-phase1-refactor` を当面の統合ブランチとする**: main へのマージを待たずに WP を開始できるようにするため。WP ブランチはここから切り、PR のベースもここへ向ける。根拠: このブランチは origin/main(ECS 最新)を取り込み済み(6bc26c2)で、フルビルド+全テスト+player 起動を検証済み=「main の上位互換かつグリーン」。運用条件:
   - **main への取り込みは merge commit で行う(squash / rebase 禁止)**。履歴を書き換えると、ここから切った WP ブランチ全部のベースが無効になるため
   - main(ECS 側)に新コミットが入ったら統合ブランチへ随時 merge して追従する(取り込み手順は 6bc26c2 と同じ: 重複ファイル確認 → merge → ビルド+ctest)
   - main へのマージ後は統合ブランチを廃止し、以降の WP ブランチは main から切る(規則 3 に戻る)

### 競合が予想されるファイルと作法

| ファイル | 触る WP | 作法 |
|---|---|---|
| `appflow/loop.cpp` | 2, 5, 17 | 直列にスケジュール(下のウェーブ)。同時に走らせない |
| `vkcore/renderer.cpp` | 2, 8, 13, 14 | 同上 |
| ルート CMakeLists.txt(FetchContent 節) | 6, 10 | 追記のみ・アルファベット順。競合しても自明に解決できる形を保つ |
| `test/CMakeLists.txt` | ほぼ全 WP | `pelican_define_test` の追記のみ |
| `core/loader/*`・`player/main.cpp` | 18, 19 | WP18 の分割 PR(a→b→c)は直列。WP19 は WP18 マージ後 |

### ウェーブ(依存を満たしつつ並列度を上げる依頼順)

| ウェーブ | 並列依頼 | 備考 |
|---|---|---|
| 1 | WP1, WP3, WP9, WP10 | 互いに独立。最大 4 エージェント |
| 2 | WP2, WP4, WP11(+WP8) | WP8 は renderer.cpp が WP2 と重なるため WP2 マージ後に開始 |
| 3 | WP5, WP12, WP17 | WP17 は loop.cpp が WP5 と重なるため WP5 マージ後に開始 |
| 4 | WP6, WP13 | |
| 5 | WP7, WP14, WP16 | WP7 完了 = headless 検証基盤(統合チェックポイント) |
| 6 | WP15 | 大物・高リスク。単独で走らせ、他 WP と並走させない |
| 7 | WP18a → WP18b | 完了済み(2026-07-02 レビュー合格)。web 側 WW1 も完了 |
| 8 | WP19, WP25, WP26(+ WW2 別リポジトリ) | WP18b マージ後に並列 3 本。競合回避: basicconfig.{hpp,cpp} は WP19 専有(WP25 は触らない規約)、WP26 は画像経路と CMake FetchContent 節のみ |
| 9 | WP18c | **WP19・WP25 マージ後**(example の shader 参照を stem 形式・scene を v1 形式という最終形で一度に書くため)。WW3(web stem)もこのウェーブから開始可 |

統合チェックポイント: ウェーブ 1 完了後と WP7 完了後に、人間が pelican_player の手動起動確認
(`rendering_phase1_review.md` の Validation Run と同じ流儀)を行う。WP16 以降は
golden テストが回帰を機械的に守る。

## 5. 依頼時のテンプレート

```
リポジトリ: pelican2 / ブランチ: main から agent/wpN-xxx を作成
タスク: docs/implementation_plan.md の WPN を実装してください。
設計文書 docs/design_*.md の該当節を必ず読むこと。
完了条件: WPN の受け入れ基準 + §0 の共通規則。
逸脱・不明点があれば実装せずに質問すること。
```
