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
| 20 | VAT 再生(pelican.vat v1) | 15, 16, 18, 19 | 大 | 高 |
| 21 | devcli `import`(pelican.import manifest) | 18 | 中 | 低 |
| 27 | コマンド層 stage 2(stdio JSON-RPC 2.0) | 2, 6, 18 | 中 | 中 |
| 28 | feature 合成基盤(fragment / defines / overrides) | 12, 13, 18 | 中 | 中 |
| 33 | フレームグラフ F0(プランナ + シャドー検証) | 18 | 中 | 低 |
| 37 | 入力システム(フレーム同期 + エッジ + 記録可能形) | 1 | 中 | 低 |
| 29 | feature 実証(debug_draw)+ GPU/CPU 計測 | 28 | 中 | 中 |
| 30 | HDR / トーンマップ feature | 26, 28 | 中 | 中 |
| 34 | フレームグラフ F1(compute 実行系) | 13, 33 | 大 | 高 |
| 35 | フレームグラフ F2(render 切替 + プランダンプ + rpc) | 27, 33, 34 | 中 | 中 |
| 40 | ビルドユニット化 B1(PELICAN_WITH_* ×4 + OFF スモーク) | 20, 26, 27 | 中 | 中 |
| 41 | `pelican_cli dist-config` B2(配布プリセット導出) | 21, 40 | 中 | 低 |

(WP31 shadow / WP32 IBL / WP36 GPU パーティクル / WP38 スケルタルアニメーションは
設計文書側で予約済み。着手ウェーブが近づいたら本書へ詳細登録する)

(WP22〜24 は設計文書側で候補予約のみ: WP22 pointcache / WP23 KTX2 / WP24 音声。
着手前に本書へ正式記載する)

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

### WP20: VAT 再生(pelican.vat v1)

参照: **`docs/dcc_integration_qa_2026-06-12.md` §6 が pelican.vat v1 仕様の正**、
`external_tools_requirements.md` R5、`design_project_dcc_houdini.md` §5。
依存: WP15, 16, 18, 19。**規模: 大・リスク高。分割 PR(a: パーサ / b: GPU 再生)可**。

1. **WP20a — 形式パーサ(純ロジック)**: `core/model/vatformat.{hpp,cpp}` に
   mesh primitive extras `pelican.vat` の検証(schema/version/fps/frame_count/
   vertex_count/bounds_min/bounds_max/loop/position_view/normal_view?)と
   bufferView 参照の解決を実装。**モジュール・GPU 非依存**
   (sceneformat.{hpp,cpp} が手本)。仕様の制約を fail-fast で検証:
   頂点数上限 8192・1 primitive 1 クリップ・トポロジ/頂点順序はベース glb と固定
2. **WP20b — GPU 再生**: 位置テクスチャ(幅=頂点数、高さ=フレーム数、RGBA16F)を
   glb バッファから生成し、頂点シェーダで **NEAREST + 隣接 2 フレーム行の手動 lerp**
   (ハードウェア線形フィルタ禁止 — 仕様)+ bounds 正規化の復元。
   パイプラインは PipelineFactory 経由の vertex バリアント。
   再生時刻は `EngineTime.now()` × fps、`loop` 対応。
   再生制御の置き場所は SeqPlayer(WP17)の前例に合わせ `core/playback`
3. CLI: `--play-vat <path.glb>`(WP17 の `--play-seq` の流儀。プロジェクト相対)
4. テスト: (a) パーサ単体 — fixture glb はテスト内で生成(数頂点×数フレーム、
   GPU 不要) (b) golden — 小 VAT fixture で複数フレームの絵が動くこと
   (`--render-out` 連番、WP17 テストの流儀)

受け入れ基準: (a) 仕様の制約違反(上限超過・複数クリップ・view 欠落)が
明確なエラーで reject される(単体テスト) (b) golden で VAT 再生の絵が固定される
(c) VAT なし glb の描画経路が無変更(golden 全ケース維持) (d) 通常起動無変更。

### WP21: devcli `import`(pelican.import manifest)

参照: **`design_project_dcc_houdini.md` §3 が manifest 仕様の正**。依存: WP18。規模: 中。

1. `core/loader/importmanifest.{hpp,cpp}`(純ロジック、モジュール非依存):
   pelican.import v1 のパース + 検証 — schema/version ゲート、
   `outputs[].file` は manifest からの相対のみ(絶対・`..` 脱出は reject)、
   sha256 必須。`source.file` は検証しない(ツール側パスのまま保持 — 仕様どおり)
2. sha256 計算はヘッダオンリー実装(PicoSHA2 等)を FetchContent 追加
   (ルート CMakeLists、アルファベット順)
3. devcli にサブコマンド `import <delivery_dir> --project <dir|project.json>`:
   manifest 検証 → outputs の実在 + sha256 照合 → プロジェクトの
   asset_data_json へ登録(v1 は glb のみ `models` へ追記。name は stem、
   path はプロジェクト相対。transform_seq 等は検証のみで登録対象外)。
   **冪等**: 同じ納品を再 import しても重複登録しない(name 一致は上書き確認 or skip)
4. テスト: fixture manifest の valid / invalid(sha256 不一致・絶対パス outputs・
   未知 schema・脱出参照)を `test/fixtures/import_manifest/` に置き fixture 駆動で
   (GPU 不要)

受け入れ基準: (a) 正しい納品 dir の import が成功し asset_data_json が更新される
(b) 再実行で重複登録されない (c) sha256 不一致・絶対パス・未知 schema が
用途の分かるエラーで落ちる(fixture 駆動テスト) (d) エンジン本体(player)の
挙動無変更(manifest はエンジンが読まない — 設計どおり)。

### WP27: コマンド層 stage 2(stdio JSON-RPC 2.0)

参照: **`external_tools_requirements.md` §4 R8 がプロトコルの正**(エンベロープ・
メソッド表・エラーコード)。依存: WP2(EngineTime)、WP6(readback/PNG)、WP18。規模: 中。

1. **エンベロープ純ロジック**: `core/communication/jsonrpc.{hpp,cpp}` に
   JSON-RPC 2.0 の request パースと response / error 直列化を
   **モジュール・GPU 非依存**で実装(sceneformat.{hpp,cpp} の流儀)。R8 の規則:
   notification 不可(id なしは invalid request)、バッチ配列は v1 非対応(-32600)、
   パースエラー -32700、未知メソッド -32601、params 不正 -32602、
   アプリ固有エラーは -32000〜-32099
2. **ディスパッチャ**: `core/communication/rpcserver.{hpp,cpp}` — stdin から
   1 行 = 1 リクエスト(NDJSON、UTF-8・改行を含まない)を読み、メソッド表で
   dispatch し stdout へ 1 行応答。**stdout はプロトコル専用** — quill ログや
   他の出力が stdout に混ざらないことを確認し、混ざる場合は rpc モード起動時に
   ログ出力先を stderr/ファイルへ限定する
3. **4 メソッド**(R8 の表どおり):
   - `set_time {t}` → `EngineTime::setTime`
   - `step_frame {}` → advance + ecs.update + render を 1 回
   - `render_frame {}` → 現時刻で render を 1 回(advance しない)
   - `capture {path}` → 直近フレームを PNG 保存(WP6 の readback 流用)。
     path は出力先 locator([PF] §3 の絶対パス規則の対象外)。result に解決後 path
4. **CLI**: `--rpc`(flag)。**v1 は `--headless` 必須**(非 headless との併用は
   起動エラー。ウィンドウ付き rpc は将来)。rpc モードの Loop は framerate 待ちを
   せず stdin をブロッキング読みし、EOF で正常終了(waitIdle → flushAll 経路を通す)
5. テスト: (a) jsonrpc 単体(GPU 不要)— パース・直列化・全エラーコード
   (b) 結合 — cmake スクリプト(run_seqplayer_headless の流儀)で
   `pelican_player --rpc --headless --project <example>` を起動し、NDJSON
   スクリプトを stdin 供給: set_time → step_frame → render_frame → capture の
   応答列検証 + PNG 実在 + **同一スクリプト 2 回実行で応答列が一致**(決定性)

受け入れ基準: (a) 結合テストグリーン (b) 不正 JSON・未知メソッド・id なしが
仕様どおりのエラーコードで応答され、プロセスは落ちない (c) stdout に応答以外が
混ざらない (d) 通常起動・既存 headless 経路が無変更 (e) 単体テストは GPU 不要。

### WP28: feature 合成基盤(fragment / defines / overrides)

参照: **`design_render_feature_modules.md` §1 が仕様の正**(fragment 形式・合成規則)。
依存: WP12, 13, 18。**feature を 1 つも使わない構成での挙動不変が絶対条件**。

1. 合成純ロジック: `core/renderingpass/featurecompose.{hpp,cpp}` —
   `pelican.render_feature` v1 のパース + 合成(features 配列順 / RT・パス名衝突は
   hard error / `insert` アンカー `before:<pass>`・`after:<pass>`・`end` /
   `render_target_overrides` は format・usage 追加のみ)。モジュール・GPU 非依存
   (sceneformat の流儀)。fragment 参照の解決は PathResolver 経由(engine:// 可)。
   合成結果は既存 parser / validation に流す(合成は前段の変換に徹する)
2. `ShaderCompileOptions` に `std::vector<std::string> defines` を追加し、
   shaderc の macro definition に接続。合成後 config の `shader_defines` を
   その config 由来の全シェーダコンパイルに注入(runtimecompiler / ShaderLibrary 経由)
3. `PELICAN_RUNTIME_SHADER_COMPILER=OFF` ビルドで features 使用 config は
   「feature には実行時コンパイラが必要」の明確なエラーで拒否
4. PipelineFactory のパイプラインキャッシュキーに define 集合を含める
5. `src/core/resources/shaders/include/pelican_features.glsl` を新設
   (v1 は器。既存シェーダへの include 追加はしない — 各 feature WP の仕事)
6. fixture: `test/fixtures/render_features/` に valid(パス 1 本挿入のダミー
   feature)/ invalid(名前衝突・不明アンカー・override 不正)+ expectations.json
   (project_format と同じ流儀・fixture 駆動テスト)

受け入れ基準(= マージ基準): (a) **features 未使用の全既存 config で挙動不変**
(golden 全ケース維持) (b) ダミー feature を使う fixture プロジェクトが headless で
描画され、挿入パスの効果が golden で固定される (c) 衝突・不明アンカー・不正
override が hard error(fixture 駆動テスト) (d) defines がコンパイル結果に
到達することの単体テスト(GPU 不要: コンパイル成功/失敗の分岐で検証)
(e) OFF ビルドでのビルド成功 + 明確な実行時エラー (f) 合成単体テストは GPU 不要。

### WP33: フレームグラフ F0(プランナ + シャドー検証)

参照: **`design_compute_task_graph.md` §1〜3・§5 F0・§6-5 が仕様の正**。依存: WP18。
**実行系(renderer / vkcore / renderingpass の実行コード)への変更は禁止** —
本 WP はプランナの追加とテストのみ。

1. `core/renderingpass/frameplanner.{hpp,cpp}`(純ロジック・モジュール非依存):
   - render pass からの依存導出: `input` → reads、`output` → writes、
     `color_load_op` / depth load が `load` のものは reads + writes
   - `buffers` / `compute_tasks` / `after` / `before` のパース(形式受理と
     プラン算出まで。実行はしない)
   - DAG 構築 → 順序導出不能な writes-writes は hard error → トポロジカルソート
     (**宣言順を安定タイブレーク**とする)→ 依存のないノードの層別 → バリア計画
   - プランの JSON 直列化: ノードに `"kind": "render" | "compute"`
     (将来 `"cpu"` を追加できる形 — §6-5 の語彙予約)
2. **シャドー検証テスト**: 既存の全 config(projects/example の
   main_rendering_config、golden 各ケースの config、fixture 群)について
   「導出順 = 現行の配列順」を assert
3. エラー系 fixture: writes-writes 曖昧・循環(after/before 起因含む)・
   未知リソース参照
4. プラン比較テストの初期形: example config のプラン JSON を fixture として保存し
   一致を assert(スキーマは本 WP で確定し、設計文書 §7-4 の未決を解消して追記する)

受け入れ基準(= マージ基準): (a) **実行系の diff がゼロ**(src/core/vkcore・
renderer・既存 renderingpass 実行コードに変更なし。golden 全維持は当然)
(b) シャドー検証が全既存 config でグリーン (c) エラー系 fixture 駆動テスト
(d) プラン JSON に kind フィールド (e) 全テスト GPU 不要。

### WP37: 入力システム

設計文書がないため本節が仕様(WP2 の流儀)。現状 `userpublic/userinput.hpp` は
空スタブ(KeyCode enum が空・実装なし)。依存: WP1。`src/core/ecs/` 変更禁止は継続。

目的: フレーム同期の入力状態。**決定性を最初から設計に入れる** —
フレームごとの入力スナップショットが直列化可能な値であること(将来の記録再生の器)。

1. `core/os/inputstate.{hpp,cpp}`(純ロジック): v1 スコープはキーボード + マウス
   (ゲームパッドは v2)。`KeyCode` enum(英数字・矢印・修飾キー・Space/Enter/Esc・
   マウスボタン)。イベント列(press/release/移動)を受けてフレームスナップショット
   (down / pushed / released のエッジ + マウス位置・デルタ)を確定する
   plain クラス。スナップショットは POD 的な直列化可能構造体
2. Window(GLFW)のコールバックでイベントをキューに積み、`Loop::run()` が
   **フレーム先頭で 1 回だけ**スナップショットを確定(フレーム内で入力状態は不変)。
   headless / rpc モードでは空スナップショット(挙動不変)
3. `userpublic/userinput.{hpp,cpp}` の静的 API(`getKey` / `isKeyPushed` /
   `isKeyReleased`)を実装し、スナップショットへ委譲。KeyCode enum を埋める
4. 動作確認用の最小配線を **1 つだけ**入れる(例: F1 で現在フレームの入力状態を
   LOG_INFO)。ゲーム的な機能は入れない

受け入れ基準(= マージ基準): (a) エッジ検出の純ロジック単体テスト(イベント列 →
複数フレームの pushed/released 遷移。GLFW・GPU 不要) (b) 通常起動でキー入力が
ログで確認できる(手動確認を PR に記録) (c) headless・golden・rpc 全経路が無変更
(d) スナップショット構造体が直列化可能(static_assert or 単体テストで示す)。

### WP29: feature 実証(debug_draw)+ GPU/CPU 計測

参照: `design_render_feature_modules.md` §2(debug_draw / gpu_timing 行)、
`design_compute_task_graph.md` §6(CPU 計測の位置づけ)。依存: WP28。

1. `engine://features/gpu_timing.json`(パスなし fragment)+ パス境界の
   timestamp query。CPU 側もフレーム内訳(update / render / present 待ち)を計測。
   出力は quill(既定 1 秒ごとに集計 1 行)。恒常オーバーヘッドは feature
   不参照時ゼロ(query pool 自体を作らない)
2. `engine://features/debug_draw.json` + `DebugDraw` モジュール(line 頂点を
   CPU バッファに積む API。feature 不参照時は no-op)+ line 描画パス + シェーダ
   (engine:// stem)
3. 検証: debug_draw で描いた既知の線分の golden ケース / feature 不参照時の
   golden 全維持 / 計測ログのスモークテスト(値が出ること)

受け入れ基準: (a) 両 feature とも「1 行有効化・不参照で挙動とコスト不変」
(b) debug_draw の golden (c) 計測ログの結合テスト(headless で数値行が出る)。

### WP30: HDR / トーンマップ feature

参照: `design_render_feature_modules.md` §2(HDR 行)。依存: WP26, 28。

1. `engine://features/hdr.json`: `render_target_overrides` で中間 RT を
   RGBA16F 化 + tonemap fullscreen pass(`engine://tonemap` stem)を
   `before:present` 相当のアンカーで挿入 + `PELICAN_FEATURE_HDR` define
2. トーンマップは v1 で 1 種(ACES 近似 or Reinhard — 実装時にシンプルな方)
3. 検証: HDR on/off の golden 両ケース、EXR テクスチャ(WP26)を光源値 >1 で
   使う fixture シーンで飽和が消えることを golden 固定

受け入れ基準: (a) off で挙動不変 (b) on の golden (c) RT override の
効果(フォーマット変更)が検証される。

### WP34: フレームグラフ F1(compute 実行系)

参照: `design_compute_task_graph.md` §2〜3・§5 F1。依存: WP13, 33。
**規模大・リスク高: 単独ウェーブで走らせる**。

1. `buffers` の確保(VMA、persistent/transient は v1 とも単純確保)
2. compute パイプライン(PipelineFactory 拡張 + `.comp` stem 対応)
3. WP33 のプランどおりにバリア発行・dispatch(`groups_from` は v1 固定値 +
   名前付きパラメータの CPU 設定 API 最小)。per_frame の配置はプラン準拠
4. パスグラフ接続: compute の書いた RT/バッファをパスが読むケースを 1 つ実証
5. 検証: (a) compute なし構成の挙動不変(golden 全維持) (b) 「バッファに
   書き込む → 結果を色として描く」最小 fixture の golden (c) validation エラーなし
   (結合テストで検出 — run_rpc_headless の流儀)

受け入れ基準: 上記 (a)〜(c) + プラン(WP33)との整合が実行時 assert で守られる。

### WP35: フレームグラフ F2 残作業(プランダンプ + rpc + 明示エッジ検証)

(2026-07-04 縮小: **render 切替の中核は WP34 が実装済み** — framegraphruntime が
render/compute 両ノードをプラン駆動で実行し、golden 全維持で意味論保存を実証済み。
本 WP は手詰め層の可視化と契約検証のみ。規模: 中 → 小)

参照: `design_compute_task_graph.md` §1(手詰め層)・§7-4(プラン JSON スキーマ)。
依存: WP27, 33, 34。

1. `--dump-frame-plan`: 起動時、合成・プラン確定後の `pelican.frame_plan` JSON を
   **stderr またはファイル**へ出力(stdout 禁止 — rpc プロトコル純度)
2. rpc `get_frame_plan`: 現在の実行プランを result で返す
   (スキーマは WP33 の fixture `plans/example_main_render.json` と同一)
3. `after` / `before` 明示エッジが実行順に反映されることの検証
   (パーサは WP33 で受理済み・実行はプラン駆動なので、テストが主작업。
   プラン比較 fixture で「エッジ追加 → レベル構成が変わる」を固定)
4. プラン比較テストの CI 常設化(feature 使用 config のプラン fixture を追加)

受け入れ基準: (a) `get_frame_plan` の結合テスト(rpc 経由でスキーマ準拠 JSON、
stdout 純度維持 — run_rpc_headless の流儀) (b) `--dump-frame-plan` の出力検証
(c) 明示エッジのプラン比較テスト (d) golden 全維持・通常起動無変更。

### WP39: 入力アクション層(I1)

参照: **`design_input_actions.md` §2 が仕様の正**(§1 四層・§6 I1)。依存: WP37。規模: 中。

1. `core/os/actionmap.{hpp,cpp}`(純ロジック・モジュール非依存):
   `pelican.input_actions` v1 のパース — schema/version ゲート、action_sets、
   action type `button` / `axis1` / `axis2` / `pose`(pose は**型のみ受理**、
   binding 解決は「OpenXR 未対応」の明確なエラー)。binding 記法
   `kbd:` / `mouse:` は解決、`pad:` / `xr:` は受理のみ(解決は後続 WP)。
   合成 binding は v1 組み込み 2 つだけ(`kbd:wasd`、`kbd:arrows` → axis2)
2. アクション評価: InputSnapshot(WP37)→ action set スタック(上のセットが
   消費した入力は下に流れない)→ ActionState(pressed / released / held / axis 値)
3. 読み込み: `basic_config.input_actions_json`(**任意キー**。未指定なら
   アクション層は完全素通り = 既存挙動不変)。PathResolver 経由。
   [PF] v6.2 の追記に対応(形式拡張はエンジン先行 — サブセット原則)
4. ゲーム API: `userpublic` に Actions 静的 API(`isPressed("jump")` /
   `axis2("move")` 等)。KeyCode 直読みの UserInput は残す(低レベル API として)
5. fixture: `test/fixtures/input_actions/` に valid / invalid(未知 type・
   action 名重複・不正 binding・pose の binding 解決要求)+ expectations.json
6. テスト: パース・セットスタック消費・エッジ評価の純ロジックテスト
   (GPU / GLFW 不要。イベント列 → スナップショット → アクション状態)

受け入れ基準: (a) `input_actions_json` 未指定で全既存テスト・golden 維持
(b) fixture 駆動テスト (c) サンプル actions.json + キーイベント列で
pressed / axis2 が期待どおり(複数フレーム・セット切替含む)
(d) `src/core/ecs/` 変更禁止の維持。

### WP40: ビルドユニット化 B1(PELICAN_WITH_* ×4 + OFF スモーク)

参照: **`docs/design_build_tiers.md` §2 が仕様の正**。依存: WP20, 26, 27(対象機能)。

1. CMake オプション 4 つを新設(**既定すべて ON**):
   `PELICAN_WITH_VAT`(vatformat / vatplayer / vat.vert / VAT golden)、
   `PELICAN_WITH_EXR`(imageloader の EXR 分岐 + tinyexr FetchContent ごと)、
   `PELICAN_WITH_RPC`(jsonrpc / rpcserver / --rpc)、
   `PELICAN_WITH_SEQPLAYER`(seqplayer / --play-seq)
2. OFF 時の規約(§2-2): 該当機能を参照する入力(--rpc 指定、VAT extras 付き glb、
   .exr 参照、--play-seq)には **silent skip ではなく明確なエラー**
   「この バイナリは PELICAN_WITH_X=OFF でビルドされています」。
   スタブは最小(登録関数の空実装 or 起動時 throw)
3. テスト側: 各ユニットの単体・golden・結合テストを対応オプションで
   条件登録(OFF ビルドでテストターゲット自体を除外)
4. **OFF スモーク CI**: 単一 OFF × 4 通りの「configure + build が通る +
   OFF エラーが出る」検証スクリプト(組合せはテストしない — §2-4)。
   ctest ではなく `test/run_build_units_smoke.cmake` 等の独立スクリプトでよい
   (フルリビルド 4 回は重いので、通常 ctest には含めず手動/リリース手順用。
   実行方法を README or スクリプト冒頭に記載)

受け入れ基準: (a) フル構成(全 ON)の全テスト・golden が無変更でグリーン
(b) 各単一 OFF で configure + build 成功 (c) OFF バイナリへの該当入力が
明確なエラー(4 ユニット分の検証がスモークスクリプトに含まれる)
(d) 既定 ON なので通常の開発手順・CI に変化がない。

### WP41: `pelican_cli dist-config` B2(配布プリセット導出)

参照: **`docs/design_build_tiers.md` §3 が仕様の正**。依存: WP21, 40。

1. devcli サブコマンド `dist-config <project> [--with rpc,seqplayer] [--out <file>]`
2. 判定規則(v1・ユニットは WP40 の 4 つのみ):
   - `PELICAN_WITH_VAT`: プロジェクトの asset_data_json / import manifest に
     登録された glb を vatformat(純ロジック)でスキャンし、pelican.vat extras を
     持つものがあれば ON
   - `PELICAN_WITH_EXR`: プロジェクト内の参照ファイル(assets / ui)に
     `.exr` があれば ON
   - `PELICAN_WITH_RPC` / `PELICAN_WITH_SEQPLAYER`: **既定 OFF**(配布ゲームに
     不要)。`--with` で明示 ON
3. 出力: `-C` 用 CMake キャッシュファイル
   (`set(PELICAN_WITH_X ON|OFF CACHE BOOL "" FORCE)` 列 + ヘッダコメントに
   生成元プロジェクト・日時・**各判定の根拠 1 行**)
4. embed サブセット注入(B3)はスコープ外
5. テスト: fixture プロジェクト(vat あり/なし・exr あり/なし)で出力を検証
   (GPU 不要)。結合: projects/example に対して生成した preset で
   configure + build が通ることをスモーク確認(コミットメッセージに記録)

受け入れ基準: (a) example で dist-config → 生成 preset の configure+build 成功
(b) 判定根拠がコメント出力される (c) GPU 不要の fixture テスト
(d) 既存挙動・全テスト無変更。

### WP42: コマンド層 stage 3(get_status / update_transforms / load_gltf)

参照: R8 §4(メソッド表)、本書 §3 の GO 記録(複数インスタンス要件)、
`design_scene_format.md`(objects[].name = 宛先)。依存: WP25, 27。

1. `get_status {}` → `{instance_id, project_root, frame, time}`
   (instance_id は起動時生成の UUID — 複数インスタンス識別)
2. `update_transforms {objects: [name...], transforms: [{pos,rot,scale}...]}` —
   R4 と同形。宛先は scene の `objects[].name`。未知名は
   アプリエラー(-32000 台)で名前を含めて返す。適用は**フレーム境界**
   (次の step_frame / render_frame の前に反映)
3. `load_gltf {path, name?}` — **プロジェクト相対のみ**(PathResolver の
   脱出防止が防壁 — R8 どおり)。エンティティを生成し、`name` を付ければ
   以後 update_transforms の宛先になる。**一時的ロード**(scene.json へは
   書き戻さない — 永続化はエディタ D3 の仕事と明記)
4. 結合テスト(run_rpc_headless の流儀で拡張 or 新スクリプト):
   load_gltf(小 fixture glb)→ update_transforms → step_frame → capture で
   絵が動くこと + **同一スクリプト 2 回で応答列一致** + エラー系
   (未知名・脱出パス・不正 params)
5. **並走制約: `appflow/loop.cpp` に触れない**(WP43 専有。適用点は
   rpcserver / 既存 step_frame 経路内で完結させる)

受け入れ基準: (a) 結合テストグリーン(絵の変化を PNG で検証) (b) エラー系が
仕様どおりのコードで返る (c) 決定性 (d) 既存 rpc テスト・stdout 純度維持
(e) get_status の応答検証。

### WP43: ゲームシステム登録 API(G1a)

参照: **`design_game_logic_native.md` §1 が仕様の正**。依存: WP39。

1. `userpublic` に `PELICAN_REGISTER_SYSTEM(Type, order)` 静的登録
   (registerComponent と同じ黒魔術様式)と `GameContext` facade を新設。
   GameContext v1 の API 面: Actions(WP39)読み取り・時刻(EngineTime 読み)・
   LOG・オブジェクト transform 操作(既存 container 経由の最小 facade)。
   **GET_MODULE をゲームコードに露出しない**
2. 実行点: `Loop::run()` の固定位置(`ecs.update()` 後・render 前)で
   全システムの `update(ctx)` を **order 昇順 → 同値は登録名の辞書順**で呼ぶ
   (静的初期化順に依存しない決定的順序)
3. 検証用ビルトインデモシステム 1 個(テスト内登録に留め、example の
   既定挙動は変えない)
4. テスト: (a) 順序決定性(order/名前のソート、純ロジック) (b) システムが
   Actions・時刻を読める(イベント注入 → 状態確認) (c) システム未登録時の
   挙動不変(golden 全維持)
5. **並走制約: `core/communication/` に触れない**(WP42 専有)。
   `appflow/loop.cpp` は本 WP 専有。`src/core/ecs/` 変更禁止は継続

受け入れ基準: 上記テスト a〜c + §0 の共通規則。

### WP45: プロジェクトコード取り込み(G1b)

参照: **`design_game_logic_native.md` §1 が仕様の正**。依存: WP37, 39, 40, 43。

1. CMake オプション `PELICAN_PROJECT`(プロジェクトパス)。指定時、
   `<project>/code/CMakeLists.txt` を include してソース列を player ターゲットに
   取り込む(規約: プロジェクト側は `pelican_game_sources(<src>...)` を呼ぶだけ。
   規約は adding_features.md に**レシピ 5** として追記すること)。
   **未指定時は完全に従来どおり**(既存テスト・golden が無変更 — 受け入れ基準 a)
2. projects/example に `code/` を追加: 最小の操作デモ
   - `input/actions.json`(move = kbd:wasd の axis2、[PF] v6.2 の
     `input_actions_json` を example の project.json に追加)
   - システム 1 つ(WP43 の PELICAN_REGISTER_SYSTEM + GameContext):
     move アクションでシーン内オブジェクト 1 つを平行移動
3. スモークスクリプト(`test/run_project_code_smoke.cmake` — build units の流儀):
   `-DPELICAN_PROJECT=projects/example` で configure + build →
   headless 3 フレームが exit 0 + PNG 出力。通常 ctest には含めない
   (実行方法をスクリプト冒頭に記載し、必ず 1 回実行して結果をコミットに記録)
4. 操作の手動確認: 通常起動で WASD によりオブジェクトが動くことを確認し、
   コミットメッセージに記録

受け入れ基準: (a) `PELICAN_PROJECT` 未指定の全テスト・golden 無変更
(b) スモーク(example コード込みビルド + headless)成功 (c) WASD 手動確認の記録
(d) adding_features.md レシピ 5 追記(docs は別コミット) (e) ecs コア変更禁止。

### WP44: 解釈レイヤのターゲット分離(pelican_project)

参照: **`design_project_interpretation_layer.md` §2・§3-3 が仕様の正**。
**このWPは「ファイル移動 + CMake ターゲット新設 + include 修正」以外を含まない**
(WP3 の流儀。ロジック変更・リファクタ禁止。挙動不変 = 全テスト・golden 維持)。

1. 静的ライブラリターゲット `pelican_project` を `src/project/` に新設。
   **許可依存: std / nlohmann_json / tinygltf(vatformat が要する場合のみ)**。
   vulkan・quill・container.hpp(モジュール機構)・b::embed への依存禁止
2. 移動候補(**各ファイルの依存純度を確認し、条件を満たすものだけ移動**。
   除外したものと理由をコミットメッセージに列挙):
   sceneformat / frameplanner / featurecompose / importmanifest(loader)、
   vatformat(model)、jsonrpc(communication)、actionmap・inputstate(os)
3. pelican_core が pelican_project をリンク。既存 include パスを修正。
   PathResolver(モジュール)は**移動しない**(純ロジック分離は将来の別 WP)
4. devcli も pelican_project を直接リンクできることを確認(dist-config /
   import が使う解釈コードの独立性の実証)
5. GPU 不要テスト群のリンク先を pelican_project に切替可能なものは切替
   (任意。無理はしない)

受け入れ基準: (a) 全テスト・golden 無変更でグリーン (b) `pelican_project` が
禁止依存を含まない(ターゲットのリンク一覧で機械確認し、確認方法をコミットに記録)
(c) diff が移動 + CMake + include 修正のみであること(PR 説明で明示)。

### WP46: 物理クエリ P1(純ロジック)

参照: **`design_physics_queries.md` §1〜3・P1 が仕様の正**。依存: なし(WP44 と並列可)。

1. `core/phys/physquery.{hpp,cpp}`(純ロジック・モジュール/GPU/ECS 非依存):
   - 形状: Sphere / Box(OBB — 位置 + 回転 + half extents)/ Capsule
   - レイ交差(解析解): レイ × 3 形状。ヒットは距離・位置・法線
   - overlap: sphere-sphere / sphere-box / sphere-capsule / box-box(SAT)/
     capsule-capsule / box-capsule の 6 ペア
   - 複数形状クエリ: id 付き形状列に対する `raycastClosest` / `overlapAll`。
     **決定性: 同距離タイは id の辞書順**(設計 §2 の規約)
2. `src/core/phys/CMakeLists.txt`(現在 0 バイト)にソース登録、pelican_core へ
3. テスト(GPU 不要): 解析解の期待値テスト(軸整列・回転あり・接触境界・
   非ヒットの各ケース)+ タイ決定性 + 法線の向き検証
4. シーン接続(collider コンポーネント → クエリワールド)・rpc・BVH は
   P2/P3 のスコープ外。**既存コードへの影響ゼロ**(新規ファイルのみ +
   test/CMakeLists 追記)

受け入れ基準: (a) 上記テスト全グリーン(GPU 不要) (b) レイ・overlap の
解析解ケースが境界条件(接する・かすめる)を含む (c) 純ロジック規律
(GET_MODULE / vulkan / quill 依存なし) (d) 既存テスト・golden 無変更。

### WP47: 物理クエリ P2(シーン接続 + GameContext + 可視化)

参照: **`design_physics_queries.md` §1〜3・P2 が仕様の正**。依存: WP43, 46。

1. collider コンポーネント拡充: 既存 `userpublic/components/collider.{hpp,cpp}`
   (SphereColliderComponent の骨)を土台に box / capsule を追加。
   scene v1 の文法(`{"name": "collider", "shape": "...", ...寸法}`)、
   serialize(`ref`)連携で loadByJson 自動化
2. クエリワールド: `core/phys/physworld.{hpp,cpp}` — collider を持つオブジェクトの
   形状 + transform + 名前を保持し、WP46 の physquery に流す薄い集約。
   transform 追従は v1 = 毎フレーム再収集(設計 未決 2 — 計測で見直す前提)。
   **ワールド組み立ては純ロジック関数**(コンポーネントデータ列 → 形状列)に
   分離して GPU 不要テスト可能に
3. バインダ: SceneLoader が collider コンポーネントを physworld へ振り分け
   (light の前例と同じ様式)
4. `GameContext` に `raycastClosest` / `overlapAll` を追加
5. debug_draw feature 参照時、collider のワイヤ表示(sphere = 3 円、box = 12 辺、
   capsule = 近似)を DebugDraw へ積む
6. テスト: (a) 組み立て純ロジック + raycast 期待ヒット(GPU 不要)
   (b) collider パースの fixture(valid/invalid: 未知 shape・負の寸法)
   (c) 可視化 golden 1 ケース (d) collider なしシーンの挙動不変(golden 全維持)

受け入れ基準: 上記 a〜d + `src/core/ecs/` 変更禁止 + §0 共通規則。

### WP48: カメラ C1(glTF 1:1 + orthographic + set_camera)

参照: **`design_camera_system.md` §1〜2・C1 が仕様の正**。依存: WP25, 42。

1. camera コンポーネントのパラメータを glTF 1:1 に(perspective:
   yfov/znear/zfar/aspect(省略可 = ビューポート追従)、orthographic:
   xmag/ymag/znear/zfar)。**既存キー(fov_y/near/far)はエイリアスとして受理**
   (既存シーン無変更 — golden 全維持)
2. orthographic の射影行列分岐(renderer/camera 系)
3. 複数カメラ: シーン内に camera コンポーネント複数可。アクティブカメラの切替を
   `GameContext::setCamera(name)` と rpc `set_camera {name}` の両方に追加
   (宛先は objects[].name。未知名はエラー)
4. テスト: (a) 両カメラ型 + エイリアスのパース fixture (b) orthographic golden
   1 ケース (c) rpc set_camera の結合テスト(切替前後で PNG 相違 + 決定性)
   (d) 既存シーンの golden 全維持
5. **注意**: カメラ既定値は現在 `basic_config.camera`(ProjectBasicConfig)→
   renderer の経路。`src/core/ecs/` 以下(predefined/camera.hpp 含む)は
   **変更禁止** — 拡張パラメータはコンポーネント params とレンダラ側で受け、
   どうしても ecs 側変更が必要と判断したら実装せず BLOCKED.md で報告

受け入れ基準: 上記 a〜d + ecs 変更禁止 + §0 共通規則。

### WP31: shadow directional(feature 合成のシェーダ合流実証)

参照: **`design_render_feature_modules.md`(feature fragment・shader_defines が仕様の正)**
と `docs/adding_features.md` レシピ 1。依存: WP28, 30, 35。見積: 大。

1. feature fragment `src/core/resources/features/shadow_directional.json`
   (engine://features/shadow_directional.json として登録 — 3+1 チェックリスト遵守):
   - shadow map RT(depth-only、D32、既定 2048^2、render_target_overrides で寸法変更可)
   - depth-only パス(ライト視点、シーンジオメトリを再描画)を main パスの前に
     挿入(アンカー = after/before 明示エッジとして実体化される既存様式)
   - `shader_defines: ["PELICAN_FEATURE_SHADOW"]`
2. ライト行列: directional light(WP25 のライトコンポーネント)から view-proj を
   導出しライト UBO を拡張。**バインダ/レンダラ側で実装(`src/core/ecs/` 変更禁止)**
3. lighting シェーダに `#ifdef PELICAN_FEATURE_SHADOW` でシャドウマップ
   サンプリングを合流(v1 = 単純比較 + 固定バイアス。PCF/cascade は v2)
4. depth-only パイプライン対応(pipelinefactory — fragment 省略 or 空)が
   未対応なら最小拡張
5. フレームグラフ: depth パス writes(shadow map)→ main パス reads の依存が
   プラン(get_frame_plan / fixture)に正しく現れること
6. テスト: (a) golden `shadow_on` / `shadow_off`(directional light + 遮蔽物の
   シーン fixture 追加)(b) frame_plan fixture に shadow 依存鎖 (c) feature
   不参照の既存 config で golden 全維持 (d) 実行時コンパイル variant の
   キャッシュキーに defines が入っていること(WP28 の機構を使用 — 新設不要)

受け入れ基準: 上記 a〜d + §0 共通規則。プランナが並べ替えても正しい絵が出る
(依存宣言のみで順序保証 — 手詰めエッジに頼らないこと。必要なら設計と相談 = BLOCKED.md)。

### WP49: 入力 I2(rpc inject_input + シナリオテスト)

参照: **`design_input_actions.md` §4.1 が仕様の正**。依存: WP27, 37, 39, 42, 45。見積: 中。

1. rpc メソッド `inject_input {events: [...]}`: イベント語彙は WP37 の入力
   スナップショット層に合わせる(key down/up・mouse move/button・axis)。
   スキーマのパース・検証は pelican_project 側(jsonrpc の既存様式)、
   適用はバインダ側
2. 注入バックエンド: headless/rpc 駆動時に GLFW ポーリングの代わりに注入
   イベントでスナップショットを構成(既存バックエンドとの合成規則は
   「rpc 駆動時は注入が正」で単純化してよい)
3. 注入 → アクション層(WP39)→ GameContext まで届くことを保証
   (projects/example の WASD デモがそのまま消費者)
4. シナリオテスト 1 本: NDJSON スクリプトで
   `inject_input(W 押下) → step_frame ×N → capture` を回し、
   (a) 開始時と PNG 相違(オブジェクトが動いた)(b) 2 回実行で完全一致(決定性)
5. 未知イベント種・不正パラメータは名前入り JSON-RPC エラー

受け入れ基準: 上記 4(a)(b) + 5 + 既存テスト・golden 全維持 + §0 共通規則。

### WP50: カメラ C2(コントローラ orbit / follow / fly)

参照: **`design_camera_system.md`(コントローラ = ビルトインシステム様式が仕様の正)**。
依存: WP39, 43, 48。見積: 中。

1. カメラコンポーネントの params に `controller` キー:
   `{"controller": {"type": "orbit" | "follow" | "fly", ...}}`。
   orbit = target(objects[].name)+ distance + 角度 + damping、
   follow = target + オフセット + damping、fly = 速度 + 感度
2. 実装はエンジン組み込みのビルトインシステム(WP43 の
   PELICAN_REGISTER_SYSTEM 様式をエンジン内から使う初例)。
   **`src/core/ecs/` 変更禁止** — カメラ transform の更新は既存の
   setLocalTransform / renderer カメラ経路で
3. orbit / fly は Actions(move / look)を消費。アクション未構成時は不動
   (エラーにしない — actions.json なしのプロジェクトでも壊れない)
4. 決定性: コントローラ更新は EngineTime の dt のみに依存(実時間参照禁止)
5. テスト: (a) controller パース fixture(3 種 + 未知 type は名前入りエラー)
   (b) orbit の決定的軌道 golden 1 ケース(set_time で角度が定まる — 入力不要)
   (c) 既存シーン(controller なし)の golden 全維持

受け入れ基準: 上記 a〜c + ecs 変更禁止 + §0 共通規則。エディタカメラ(D1)で
fly を共用する将来を壊さない(コントローラ状態はコンポーネント params +
システムローカルに閉じる)。

### WP51: オーディオ A1(ユニット + WAV SE + バス音量)

参照: **`design_audio.md` が仕様の正**。依存: WP40, 43, 45。見積: 中。

1. `PELICAN_WITH_AUDIO` ユニット新設(WP40 の様式: 既定 ON・OFF は明確エラー・
   OFF スモークを run_build_units_smoke に追加)
2. miniaudio を FetchContent 追加(ルート CMakeLists — アルファベット順追記)。
   `src/core/audio/` 新設: デバイスバックエンド + **null バックエンド**
   (headless / `--rpc` 時は自動で null。デバイス列挙不能時も null に
   フォールバック — CI で落ちない)
3. バス 3 本固定(master/bgm/se)+ バス毎音量
4. WAV の SE 再生(全読み・多重発音)。パスは PathResolver 経由
   (project:// / engine://、絶対パス拒否)
5. `GameContext`: playSound / stopSound / setBusVolume / isPlaying
   (SoundHandle は handle.hpp の既存流儀)。**gamecontext.{hpp,cpp} への追記は
   ファイル末尾に**(並走 WP と交差するため)
6. テスト: (a) null バックエンドで play → isPlaying → stop の状態遷移
   (GPU・音声デバイス不要)(b) 不正 WAV / 未知パスのエラー (c) OFF スモーク
   (d) 既存テスト・golden 全維持

受け入れ基準: 上記 a〜d + §0 共通規則。Ogg / フェード / 3D 減衰は A2/A3
(スコープ外 — 手を出さない)。

### WP52: シーン遷移 S1(loadScene + rpc load_scene)

参照: **`design_scene_flow.md` §1 が仕様の正**。依存: WP25, 42, 43。見積: 中。

1. `GameContext::loadScene(name)` / `currentScene()`。update 中の呼び出しは
   **予約 → フレーム末尾で適用**(イテレーション中の全破棄禁止)
2. 切替の意味論: 全 GameObject 破棄 → 新シーンを**起動時と同じバインダ経路**で
   構築(light/collider/camera/material。第 2 経路を作らない)。
   EngineTime は継続
3. rpc `load_scene {name}` + `get_status` にシーン名追加。未知名は名前入り
   エラー。load_gltf の一時オブジェクトは切替で破棄
4. projects/example に 2 シーン目を追加(検証用の最小シーン)
5. テスト: (a) rpc 結合 — load_scene 前後で capture の PNG 相違 + 決定性
   (2 回実行一致)(b) 未知名エラー (c) 切替 ×10 でオブジェクト数が安定
   (リーク検出)(d) 既存テスト・golden 全維持
6. **gamecontext.{hpp,cpp} への追記はファイル末尾に**(並走交差対策)

受け入れ基準: 上記 a〜d + `src/core/ecs/` 変更禁止 + §0 共通規則。
非同期(S2)はスコープ外。

### WP53: 決定性乱数(PCG32 + GameContext + rpc)

参照: **`design_determinism_services.md` が仕様の正**。依存: WP42, 43。見積: 小。

1. PCG32 自前実装(`src/core/userpublic/` 配下 or core — 純ロジック、
   distribution も自前: randomInt は棄却法、random は 53bit → double)
2. シード: project.json `basic_config.seed`(省略時 0)。
   `GameContext`: random / randomInt / randomFloat / setSeed / seed
   (**追記はファイル末尾に**)
3. rpc: `set_seed {seed}` 追加、`get_status` に seed 追加
4. adding_features.md の新サブシステム要件に「wall clock / std::rand /
   random_device を判定に使わない」を 1 行追記
5. テスト: (a) 同一シード → 同一列(固定期待値 — 処理系非依存を fixture で
   証明)(b) setSeed 再現 (c) randomInt の境界(min=max、負範囲)
   (d) rpc set_seed/get_status 結合 (e) 既存テスト全維持

受け入れ基準: 上記 a〜e + §0 共通規則。ストリーム分離は v2(スコープ外)。

### WP54: debug_text feature(ビットマップ HUD)

参照: **`design_text_hud.md` §1 が仕様の正**。依存: WP28, 29。見積: 中。

1. `engine://features/debug_text.json`(パージ可能 — debug_draw と同型)。
   スクリーン空間の最後段パス(swapchain 直前)
2. エンジン埋め込み等幅ビットマップフォント(ASCII 95 字、8x16 目安、
   **public domain のもの — 出典とライセンスをファイル横に記録**)。
   PNG アトラス + 座標表を engine:// リソース登録(3+1 チェックリスト)
3. 文字列 → quad 列。色・整数倍スケール・ピクセル座標(左上原点)
4. `DebugText` モジュール + `GameContext::debugText(x, y, text)`
   (immediate 型 — 毎フレーム積み直し、debug_draw と同じ寿命規約。
   feature 不参照時は no-op で落ちない。**gamecontext 追記はファイル末尾**)
5. テスト: (a) golden 1 ケース(固定文字列)(b) feature 不参照時の
   golden 全維持 + no-op (c) クリッピング(画面外座標)で落ちない

受け入れ基準: 上記 a〜c + §0 共通規則。SDF / 日本語 / UI 統合は v2
(スコープ外 — 手を出さない)。

### WP55: [PF] v6.3 リゾルバ拡張(user:// + asset store + #フラグメント構文)

参照: **[PF] §3-A(2026-07-08 承認済み)が仕様の正**。詳細は
`design_persistence.md` §0-1 / `design_project_vcs.md` §1 /
`design_asset_containers.md` §1。依存: なし。見積: 中。

1. `user://` スキーム: `%APPDATA%/pelican/<project_id>/` に解決
   (project_id = project.json の name。ディレクトリは初回書き込み時に作成)。
   **テスト用に root を起動引数 `--user-dir <path>` で差し替え可能に**
2. asset store: `project.json` の `asset_stores`(mount = 相対のみ、
   プロジェクト外相対は許可・絶対は拒否)+ `.pelican/local.json`
   (**パス辞書限定** — 未知キー・未宣言 store 名・その他の内容はエラー)。
   マウント入れ子/重なり = 宣言時 hard error。**脱出禁止は mount root 単位**
   (正規化後に mount 外 = 拒否、symlink 含む)。宣言なし = 従来挙動
   (既存テスト・golden 全維持で証明)
3. `#` フラグメント: パス解析で `<パス>#<種別>/<残り>` を分離する型と
   パーサ(正準形 = フルパス、短名は糖衣)。**本 WP は構文と検証のみ** —
   実際のサブアセットロードは後続 WP(K2 系)。フラグメント付き参照が
   ローダに到達したら「未対応の種別」の明確エラー
4. 起動時に store 解決結果(store 名 → 実パス)を 1 行ログ + `get_status` に
   `stores` を追加
5. テスト: (a) user:// 解決 + --user-dir 差し替え (b) store 宣言なし従来一致 /
   兄弟相対 mount / local.json 上書き / パス辞書違反エラー / 入れ子 hard error /
   mount root 脱出拒否 (c) フラグメント構文(正常・曖昧・不正)
   (d) 既存テスト・golden 全維持

受け入れ基準: 上記 a〜d + §0 共通規則。

### WP56: イベント層 E1(バス + SceneLoaded + inject_event)

参照: **`design_event_layer.md`(§2 API は 2026-07-08 承認済み)が仕様の正**。
依存: WP43, 52。見積: 中。

1. イベントバス: `PELICAN_REGISTER_EVENT(Type)` + `ctx.emit(struct)` +
   システムの `onEvent(const E &, GameContext &)`(registerComponent の
   黒魔術と同じ様式で登録を自動化)
2. **配送は次フレーム頭**(全システム update 前)、配送順 = emit 順。
   ペイロードは値コピー(参照・ポインタ禁止を静的に強制できる範囲で)
3. エンジン発行: `SceneLoaded { scene_name }`(WP52 の切替適用点から emit)
4. rpc `inject_event {type, payload}`(テスト用 — inject_input と同じ価値)
5. テスト: (a) 純ロジック — 配送順・1 フレーム遅延・配送中の emit
   (リエントラント)は次々フレーム (b) SceneLoaded の結合(load_scene →
   次フレームでゲームシステムが受信)(c) inject_event 結合
   (d) 既存テスト・golden 全維持

受け入れ基準: 上記 a〜d + §0 共通規則。E2(物理トリガー)はスコープ外。

### WP57: project init 雛形(V3)

参照: **`design_project_vcs.md` §3 が仕様の正**。依存: なし。見積: 小。

1. `pelican_cli project init <dir>`: project.json / scenes / assets / input /
   code の最小雛形(projects/example の縮約 — 動く最小シーン 1 個)
2. `.gitattributes`(glb/vrm/png/wav/spv = `-text`、LFS track はコメントアウト
   同梱)+ `.gitignore`(`.pelican/`、build 等)+ README 雛形
   (アセット配置の「最初にやること」)
3. 生成したプロジェクトが `pelican_player --project <dir>` でそのまま起動する
   こと(結合テスト)
4. 既存ディレクトリへの上書きは拒否(空でない場合エラー)

受け入れ基準: 3 の結合テスト + 4 + 既存テスト全維持 + §0 共通規則。

### WP58: マテリアル M1(pelican.material パーサ + シェーダ契約文書)

参照: **`design_material_shading.md` §3〜5 が仕様の正**。依存: WP44。見積: 中。

1. `src/project/materialformat.{hpp,cpp}`: pelican.material v1 のパース + 検証
   (純ロジック、nlohmann のみ。base = glTF pbrMetallicRoughness 1:1 キー、
   shader stem、defines は bool のみ、params は数値/vecN のみ)
2. エラーは名前入り(未知キーは [PFW] 流儀に従い本体は無視+WARN 相当の
   結果報告、型違い・不正参照は reject)
3. `docs/shader_contract.md` 新設: 現行の set 0〜3(pelican_sets.hpp)・
   push constant 分割・頂点入力 location・defines 合成順を**実装から読み取って
   文書化**(コード変更はしない — 文書が実装に従う)
4. テスト: fixture(valid 最小 / full / 各エラー)+ ラウンドトリップ
   (パース → 構造体 → 期待値)。**バインダ・レンダラには触れない**(M2 の領分)

受け入れ基準: パーサテスト全緑 + 契約文書が実装と一致(レビューで照合)+
既存テスト全維持 + §0 共通規則。

### WP59: スパイク — pelican-spv-link(B 層の SPIR-V ABI リンク実証)

参照: **`design_material_shading.md` §3-6 が仕様の正**。依存: なし。見積: 中。
**探索 WP** — 成果物はプロトタイプ + 報告書(golden 追加なし・src/ 変更禁止)。

1. `experiments/spvlink/` に自己完結の実験場を作る(メインビルドに組み込まない。
   独立 CMake ターゲットか、experiments 内で完結するスクリプト)
2. 最小テンプレートシェーダ(ライティング直書きで可)を「`pelican_surface` を
   Import 宣言して呼ぶ」形で SPIR-V 化(glslang の Linkage 対応を調査 —
   不可なら stub 関数を後で置換する等の代替手段を試してよい。**やり方の発見も
   スパイクの成果**)
3. 同一の `pelican_surface`(単純な albedo 加工 + ユーザーテクスチャ 1 枚)を
   **GLSL 版と Slang 版**で書く。シムヘッダ(PelicanSurface 定義)は手書きで可
   (生成は本実装の領分)。Slang は prebuilt リリースの取得でよい
4. リンクパイプライン: spirv-link → spirv-opt(inline + DCE)→
   binding remap(ユーザー descriptor を空きスロットへ)→ spirv-val。
   SPIRV-Tools は shaderc 同梱のものを流用できるか調査、だめなら FetchContent
5. 検証: (a) 両言語版とも spirv-val を通る (b) リンク済みモジュールから
   VkShaderModule + パイプライン生成が成功(既存 headless 基盤の流用可。
   絵を出すのはボーナス)(c) defines を変えた再リンク
6. `experiments/spvlink/REPORT.md`: 型正規化で実際に何を潰したか /
   glslang・Slang それぞれの落とし穴 / binding remap の実際 /
   **本採用の可否と推奨**(だめなら何が壁か)

受け入れ基準: 上記 6 の報告書 + (a)(b) の再現手順。既存ビルド・テストに
影響ゼロ(src/ 不変)。**結論が「不成立」でも、壁の特定ができていれば合格**。

### WP60: リファクタ R1 — 死荷重の削除(挙動無変化の証明付き)

参照: 2026-07-08 リファクタ監査(ユーザー承認済み)。依存: なし
(ECS 凍結解除済み)。見積: 中。

1. 旧衝突系の撤去: `src/core/ecs/predefined/collision.{cpp,hpp}`
   (SimpleCollisionSystem — 毎フレーム 100 万件 reserve・結果未使用)、
   `predefined.cpp` の強制登録、`SphereColliderComponent` 別名。
   現行 collider は PhysWorld 経路(scene.cpp)のみが正
2. player のデモ残骸: `src/player/main.cpp` の MyCharComponent/MyCharSystem と、
   それだけのために存在する coredist ブリッジ
3. 旧 pelican_cli 生成器: devcli/main.cpp の旧デフォルト分岐(myproject/hoge)、
   Mustache 依存(FetchContent 含む)、旧テンプレート 4 つ。正規経路は
   `project init`(WP57)。**削除前に部品取り(ユーザー指示)**:
   旧生成器・テンプレートを読み、project init に取り込む価値がある要素
   (テンプレ内容・生成構成・エラー処理等)を `docs/design_reviews/`
   に SALVAGE メモとして残すこと(取り込む価値なしならその旨を書く)
4. 空 API 群: `ECS::compaction()`、`removePrimitiveEntry`、`Hoge` handle と
   `hoge_test`、空の `sound/CMakeLists.txt`、未使用 adapter API
5. テスト・fixture の追随削除(旧系専用のもののみ。共用 fixture は触らない)

受け入れ基準: (a) ビルド + 全テスト + **golden 16 ケース + player 起動が
完全無変化**(= 削除しても何も変わらないことの機械的証明)
(b) SALVAGE メモ (c) §0 共通規則。**削除対象以外のリファクタに手を出さない**
(strict v1 化・構造統合は後続 R 系の領分)。

### WP61: リファクタ R2 — カメラキーの正規形移行(データのみ・受理は両対応のまま)

参照: **監査 Q4 の対応表(`docs/design_reviews/2026-07-08_refactor_qa_codex.md`)が
作業リストの正**。依存: WP60。見積: 小。

1. スコープ = **データファイルの camera キーを正規形(yfov[ラジアン]/znear/zfar)へ
   一括変換**。パーサ・受理コードは一切変えない(strict 化は R3 の領分)。
   fov_y は度 → ラジアン変換(45°→0.7853981633974483 等 — Q4 の換算値)
2. 対象(Q4 のリストどおり): projects/example/project.json /
   src/core/resources/default_config.json / devcli projectinit テンプレート /
   test/camera_test.cpp のデータ部(**別名受理を明示テストする箇所は残す** —
   それは R3 で rejection テストに変わる)/ golden の project generator 6 箇所 /
   CMake テスト生成器 11 本
3. rpc の rot→rotation・shader stem 限定・collider は**やらない**
   (パーサ変更を伴うため R3)
4. 受け入れ: (a) 全テスト・golden 完全無変化(変換の正しさの証明 —
   ラジアン値が正確なら絵は 1px も変わらない)(b) データファイルに
   fov_y/near/far が残っていないこと(別名受理テストの埋込データを除く)を
   grep で確認 (c) §0 共通規則

### WP62: リファクタ R5-core — ECS ライフサイクル + 世代付き EntityId(単一ゲート)

参照: **`docs/design_ecs_lifecycle.md` v2.1(確定)が仕様の正**
(v1/v2 レビュー 2 本が `docs/design_reviews/` にあり、条件 C1〜C7 は
本文反映済み)。依存: WP60, 61。見積: **特大**(本リファクタトラックの本丸)。

実装内容は設計 §1〜§6 の全部(要約):

1. ストレージ: alignment メタデータ + aligned allocation(`vector<uint8_t>` 廃止)
   + CHUNK_CAPACITY 契約 + batch API 3 段分離(§1-2 C3)
2. ライフサイクル: 4 特性 + 全 slot 値構築(construct_at、省略なし)+
   noexcept 契約(move/destroy/deinit)+ copy fallback 廃止 +
   `SimpleModelViewUpdateComponent` special members 修正 + 生成 transaction +
   mutation guard(再入拒否)+ 逆順 rollback + typed 登録一本化
   (serializer optional 化 C5)
3. teardown フェーズ(scope guard・no-throw・例外経路含む全終了経路で 1 回)
4. 世代付き EntityId: canonical 定義(entity.hpp、デフォルト invalid)+
   id_table + free-list LIFO + clear で世代保持 + MAX retire +
   全経路 resolve + stale ポリシー表(bool 三層伝播)
5. 外部参照 ID 化: PhysWorld(variant transform_source・bind 時 prune・
   即時可視維持)/ SceneLoader(ID 1 本)/ rpc(parse + flush 両 resolve)
6. benchmark の typed/bounded 書き直し + 実装前 baseline 計測
7. テスト: 設計 §7 の全部(カナリア イベント列・fault injection + 資源カウンタ・
   再入拒否・batch 境界・ABA・stale 全分類・teardown 例外経路・
   ASan + 別 UBSan ジョブ)

受け入れ基準: 設計 §7 全項目 + 既存テスト・golden 全維持(POD 経路不変)+
WP47 の即時可視テスト不変 + §0 共通規則。**内部コミットは分割可・ゲートは
1 つ**(部分マージ不可)。判断に迷ったら設計とレビュー 2 本を読み、それでも
不明なら BLOCKED.md。

### WP63: リファクタ R3 — strict v1 化(受理の厳格化・アトミック)

参照: **監査 Q4(`docs/design_reviews/2026-07-08_refactor_qa_codex.md`)の
爆風リストと対応表が作業リストの正**。依存: WP61, 62。見積: 中。
**パーサ変更 + データ + テストを同一コミットでアトミックに**(途中状態は
連鎖破壊する — Q4 の指摘)。

1. **version == 1 強制**: project(runtime + devcli distconfig の 2 箇所)と
   scene を `!= 1` reject に。schema なし top-level scene(legacy)受理と
   旧 `lights` 変換を削除。fixture `scene_legacy.json` は削除し、
   **version 0 / -1 / schema なしの rejection fixture を新設**
   (expectations.json の該当行も error へ)
2. **camera 別名削除**: fov_y / near / far の受理を止め、
   **名前入りエラー**(「fov_y は廃止 — yfov(ラジアン)を使え」の形で
   新キー名を案内)。camera_test の別名受理テストは rejection テストへ反転
3. **collider 別名削除**: size / height の受理を止め同様の名前入りエラー。
   physworld_test の互換テスト 2 箇所を rejection へ反転
4. **rpc の rot → rotation**: パーサとテストデータ(run_rpc_headless /
   set_camera / scene_flow の該当行 — Q4 に行番号)を同時変更。
   transform_seq(別スキーマ)は**対象外**(Q4 の判定どおり)
5. **shader 参照は stem のみ**: makeShaderReference の明示拡張子
   (.spv/.vert/.frag/.comp/.wgsl)受理を削除(名前入りエラーで stem を案内)。
   **内部の stem → .spv fallback 解決は維持**。互換を明示テストしていた
   埋込データ(projectconfig_test / renderingpass_helpers_test /
   shader_library_test の Q4 記載箇所)は rejection テストへ反転 or 削除
6. **golden の SKIP 握り修正**: 実行開始後の例外を SKIP にせず fail に
   (SKIP は「Vulkan デバイスなし」のみ — 過去に実エラーが SKIP に化けた
   教訓の恒久対策)
7. 受け入れ: (a) 全テスト・golden 全維持 (b) 全別名・legacy 受理の
   rejection fixture がエラーメッセージ(新キー名の案内込み)まで検証
   (c) grep で受理コードの残骸ゼロ (d) §0 共通規則

### WP64: リファクタ R4 — legacy 実行系の撤去(F2 の完遂)

参照: **監査 Q1(`docs/design_reviews/2026-07-08_refactor_qa_codex.md` の
「legacy 実行系の撤去リスク」)が仕様の正** — 意味論差分表と追加受け入れ基準
8 項目をそのまま採用する。依存: WP35, 62。見積: 大。

1. **撤去前に A/B 差分テストを作る**(同一 WP 内・先行コミット):
   テスト限定の実行経路セレクタ + **実行 node trace の計装**(実行された
   node 名/種別/順序、load/store/clear、最終 layout)を追加し、
   全 Renderer 経由 golden で legacy/planned の trace と画像の一致を検証
2. `renderer.cpp` の `has_compute` 分岐を撤去し全 config を planned 実行へ。
   `executeLegacyRenderingPasses` を削除。A/B セレクタはテスト資産として
   fixture 比較(採取済み trace)に置換
3. **planned のみが持つ意味論を正式化**: pure-render の before/after 明示
   エッジが実行順に反映されるケースをテスト追加(legacy は無視していた —
   これは修正であり回帰ではない、と文書化)
4. Q1 の残基準: (a) `framePlanOrder(plan)` と実行列の一致テスト
   (b) **Vulkan validation + synchronization validation を有効にした
   複数フレーム実行**(waitIdle なし・in-flight 数超、compute↔render 混在)で
   エラー 0 件 (c) resize / shader hot reload 後の fullscreen 入力 rebind 検証
   (d) GPU timing の node 名・順序がプランと一致
   (e) 代表 config の CPU frame time / バリア走査コストを撤去前後で計測し
   REPORT に記録(バリア走査の per-node 事前コンパイル化は数値が悪ければ実施)
5. swapchain スモーク(ウィンドウ経路)は既存 player 起動テストで代替可 —
   resize は (c) に含める。実施不能な項目は理由を REPORT に明記(黙って
   省略しない)

受け入れ基準: 1〜4 のテスト全緑 + 既存テスト・golden 全維持 + REPORT
(`docs/design_reviews/2026-07-10_wp64_report.md` — trace 差分の有無・計測値)
+ §0 共通規則。

### WP65: 永続化 P1(user:// 設定 + セーブデータ)

参照: **`design_persistence.md` §1〜2 が仕様の正**([PF] v6.3 承認済み、
user:// リゾルバは WP55 実装済み)。依存: WP55。見積: 中。

1. `core/persistence/` 新設: pelican.settings v1(エンベロープ +
   `engine`/`game` 区画)の読み書き。読みは起動時自動、書きは明示
   (`GameContext::saveSettings()`)。**atomic 書き込み**(temp + rename)、
   破損時は読み失敗を返す(クラッシュしない・名前入り WARN)
2. セーブデータ: `GameContext::saveData(slot, json)` / `loadData(slot)` /
   `listSaves()`(slot 名 + タイムスタンプ)。user://saves/ 配下
3. engine 既知キー v1: オーディオのバス音量(WP51 連携 — 起動時に適用)。
   ゲーム自由区画は素の JSON パススルー
4. テスト: (a) --user-dir 差し替えでの読み書き round-trip (b) atomic
   (書き込み中断を模した temp 残骸があっても壊れない)(c) 破損 settings で
   起動継続 + WARN (d) バス音量の適用 (e) 既存テスト・golden 全維持
5. **gamecontext への追記はファイル末尾に**(並走交差対策)

受け入れ基準: a〜e + §0 共通規則。リバインド差分保存(P2)はスコープ外。

### WP66: asset store V2(assets.manifest 生成・照合・起動時検証)

参照: **`design_project_vcs.md` §2 が仕様の正**(深刻度モデル: 内容不一致 =
INFO / 構造逸脱 = WARNING / strict・dist = ERROR)。依存: WP55, 57。見積: 中。

1. `pelican_cli assets manifest`: store 走査 → sha256 + サイズの
   assets.manifest.json(**冪等 — 相対パスソート済み出力**)。`.pelican/` の
   サイズ+mtime キャッシュで差分ハッシュ、`--full` で全再計算
2. `pelican_cli assets verify`: 照合(欠落・不一致・manifest 外を名前入り
   列挙)。`--full` = INFO 級込みの完全照合
3. 起動時検証: store 宣言に manifest があれば検証。**深刻度モデルどおり**
   (INFO = 内容不一致 + 「assets manifest で追認」案内 / WARNING = 欠落・
   参照不能・大文字小文字不一致 / ERROR は `--strict-assets` のみ)。
   **ロードは止めない**。起動サマリ 1 行(件数 + store 解決先)
4. `pelican_cli assets status`: 欠落と期待パスの一覧(clone 直後の案内)
5. projects/example の README 手書き表を manifest 生成に置換(表は削除し
   「assets manifest で検証」の説明へ)
6. テスト: (a) 生成の冪等性(2 回実行で byte 一致)(b) 差分ハッシュの正しさ
   (c) verify の 3 深刻度の分類 (d) 起動時検証がロードを止めないこと
   (e) 既存テスト・golden 全維持

受け入れ基準: a〜e + §0 共通規則。fetch・lint はスコープ外。

### WP67: リファクタ R5 後続 — modelview 統合(絆創膏と隠しコンポーネントの撤去)

参照: **`design_ecs_lifecycle.md` §3(ModelViewUpdate 統合)+ 監査 Q3 が背景**。
依存: WP62(ECS v2.1)。見積: 中。

1. `SimpleModelViewComponent` と `SimpleModelViewUpdateComponent` を統合し
   1 コンポーネントに(監査判定「JSON を変えるより統合が自然」— **scene JSON の
   `simplemodelview.model` の書き味は不変**。example の 31 参照が既存利用者)
2. SceneLoader の隠し `simplemodelviewupdate` 自動生成を撤去
3. 手動文字列コピーの絆創膏(「データぶっ壊れる」コメント一帯)を撤去 —
   WP62 のライフサイクル(construct/move/destroy + deinit)が正となったため
   不要になったことをカナリアで証明
4. deinit(GPU インスタンス解放)が remove/clear/teardown の全経路で
   正しく走ることを lifecycle テストに追加(リークカウンタ)
5. 受け入れ: (a) 既存テスト・golden 全維持(scene JSON 不変の証明)
   (b) 統合後の登録型が ECS v2.1 の static_assert を素で通る(絆創膏なし)
   (c) カナリア/リークテスト (d) §0 共通規則

### WP68: マテリアル M2a — .surface パーサと values 方式(形式 drift 解消)

参照: **`design_material_shading.md` v1.2 §3・§3-10 が仕様の正**。
依存: WP58。見積: 中。**純ロジックのみ — バインダ・レンダラは M2b の領分**。

1. `src/project/surfaceformat.{hpp,cpp}` 新設: `.surface` コンテナのヘッダ
   パース — `//! pelican.surface v1` ブロック(language / **順序付き params
   配列**(name・明示型 float|vecN|int・default 必須・min/max/hint 任意)/
   textures(name・default パス・color_space)/ screen_inputs)+ コード本体の
   分離。エラーは名前入り(型なし・default なし・重複名・未知 language)
2. `materialformat` を v1.2 形式へ改訂: `values`(宣言済み param の上書き)方式。
   旧 v1.1 の JSON 側 params/textures **宣言**は削除(「宣言順 std140」は
   仕様バグとして廃止済み — レビュー指摘)。未知キーの扱いを確定
   (warning 報告 — [PFW] 流儀)
3. fixture: valid(最小/full/多言語ヘッダ)/ invalid(各エラー系)/
   values の型不一致・未宣言 param 上書きエラー
4. 受け入れ: (a) パーサテスト全緑 (b) 既存テスト・golden 全維持
   (c) shader_contract.md との整合(矛盾を見つけたら報告 — 修正は M2b)
   (d) §0 共通規則

### WP69: FrameInput — 入力層の順序付きイベント化とフレーム位相

参照: **`design_ui_2d_foundation.md` v3 §2-1(位相契約・受け入れ (a)〜(g))+
v3 レビュー `docs/design_reviews/2026-07-11_ui_2d_v3_review_codex.md` §3
(開始ブロッカーなしと判定・追加条件)が仕様の正**。依存: WP37, 39, 56。見積: 中〜大。

1. `FrameInput { span<const InputEvent> ordered_events; InputSnapshot snapshot; }`
   を公開。event_seq = u64、**採番 = キュー投入時、プロセス起動から単調増加**
   (リプレイ時は記録値を使用する前提の設計 — 記録自体は I3)。
   span はフレーム内 immutable・フレーム跨ぎ保持禁止(debug assert で検査)
2. **5 段フレーム位相**を通常 loop と rpc loop の両方に実装:
   ①E1 pending → deliver_now swap(swap と配送を分離)②input freeze
   ③Actions フレームを**一度だけ**確定(現行の query 毎評価を廃止 —
   挙動は不変であること)+ **外部消費マスク API**(frame 粗粒度。UI 実装前は
   呼び手なしだが API とテストを整備)④deliver_now 配送(以後の emit は
   pending_next)⑤ECS/game 更新
3. GLFW / rpc / (将来 replay) が同一 InputEvent 列を通ることの確認と、
   **通常 loop と rpc loop の位相同一性テスト**
4. `pelican.input_seq` の記録単位改訂は設計反映済み(I3 未実装のため
   コード作業なし — 形式定義のずれがないかだけ確認)
5. テスト: (a) event_seq の型・採番・単調性 (b) span の寿命(跨ぎ保持の検出)
   (c) マスク単体(マスクした control が Actions から見えない)
   (d) Actions 一回評価の等価性(既存 inject_input シナリオ・WASD 挙動不変)
   (e) 位相同一性 (f) **既存テスト・golden・rpc スイート全維持**

受け入れ基準: a〜f + §0 共通規則。UI 本体には触れない。

### WP70: マテリアル M2b-1 — FrameUBO と契約差分の解消(最重量)

参照: **`design_material_shading.md` v1.2 §3-5・§4・§6 M2b 行 +
`docs/shader_contract.md` の「現状として記録する差分」3 件が作業リストの正**。
依存: WP62, 64, 68。見積: **特大**(全シェーダに触る)。

1. **FrameUBO 新設(set 0 binding 0・全パイプライン共通の唯一の意味)**:
   time / dt / frame_index / resolution / camera 位置 / view / proj。
   ObjectBuffer SSBO は set 0 binding 1 へ、LightUBO は set 0 binding 2 へ移動
   (「同じ set/binding で pipeline ごとに型が違う」の解消)
2. **time の引越し**: VAT push constant への同乗をやめ FrameUBO から供給
   (**非 VAT マテリアルにも time が届くようになる** — 本 WP の目玉)。
   VAT の静的パラメータ(bounds 等)は 3 の material SSBO へ
3. **push constant の 64B/64B 分割 enforcement**: reflection 検証で
   shader 宣言の範囲を検査(engine 領域 = mvp のみに縮小)
4. **マテリアルデータの SSBO 化**: 全マテリアル struct 配列 + インデックス
   (個別 UBO 禁止 — bindless 前工事の設計要件)。glb の PBR factor も SSBO へ
5. 全標準シェーダ(material/shadow/fullscreen/UI/debug 系)を新契約へ更新。
   `docs/shader_contract.md` を実装に合わせて改訂(差分 3 件を「解消済み」へ)
6. **受け入れの砦 = golden 全維持(全 17 ケース・SKIP ゼロ)+ 全テスト +
   player**。時間の供給元変更は値が同じなら絵が変わらないことの証明を兼ねる

スコープ外(M2b-2 へ): .surface values のバインダ・textures 辞書 binding・
ダミーテクスチャ・resources manifest・render state 拡張・dump-lowered-material。

### WP71: EventPayloadSchema — イベント payload の explicit descriptor

参照: **`docs/design_event_payload_schema.md` v2 が正**(2026-07-11 codex
round 3 レビューで**条件付き Accept** —
`docs/design_reviews/2026-07-11_color_ui_v6_rereview_codex.md` §3)。
依存: なし(E1 済)。**UI U0 の前提**。見積: 小〜中。
排他: `src/core/userpublic/details/event/`(serialize 層は変更禁止)。

作業内容は設計文書 v2 §1〜§6 のとおり(explicit constexpr descriptor 正本・
static init での構築ゼロ・構築前 JSON 検証・debug init phase 一致検査・
4 状態 lookup)。**レビュー添付条件 E-C1〜E-C6 を受け入れ基準に含める**
(レビュー文書 §3.4 が正 — 要約):

(round 4 レビュー §3.3 で転記精度を修正済み — 以下が正)

- **E-C1**: descriptor は event 型の static storage が所有(一時 array への
  span 禁止)。**空名 / 重複名 / range 種別の型不適合 / min>max /
  NaN・Inf / F32 field への binary64 で表現した bound の F32 表現可能性** —
  すべて constant evaluation で失敗させる。MSVC compile-pass 1 件 +
  **上記 6 分類それぞれの compile-fail fixture** を CI に
- **E-C2**: `pelican_payload` を持つ型は `default_initializable` +
  `ISerializable<T, JsonArchiveLoader>` を registration 時に強制
  (satisfy しなければ compile error)。`nothrow_default_constructible` も
  static_assert。副作用なしは counting-constructor テストで担保
- **E-C3**: prevalidator は**全 JSON shape を規範化**: scalar 各型
  (幅・符号・有限性)、String、Vec2/3/4・Quat の配列長/要素型/有限性/範囲、
  integer token 規則(`1.0` 不受理)、2^53 境界。全 invalid case で
  loader 呼出 0 回・constructor 0 回・pending event 0 を検査
- **E-C4**: 全 registered Typed イベントの descriptor↔ref 一致検査を
  **CI 必須テスト**に(debug 手動起動に依存しない)。追加忘れ/rename/
  reorder/duplicate/条件分岐の既知限界を個別 fixture に。
  **plugin/遅延 registration は本 WP では禁止**(全イベント registration は
  起動時 static init で完結する、を静的検査 — 将来 DLL ホットリロード
  (G2)で緩める場合は module registration 完了時の catalog 再検査を条件に)
- **E-C5**: **Opaque の by-name emit は廃止**(typed C++ emit のみ —
  レビュー推奨案 1 を採用)。UI は Opaque を fields の有無に関係なく
  binding 自体で拒否(`no_payload_event`)
- **E-C6**: state ごとの行列(round 3 §3.4 の表と同値 — 転記):

  | state | UI | RPC/by-name |
  |---|---|---|
  | Unknown | `unknown_event` | unknown event エラー |
  | Opaque | `no_payload_event`(binding 自体不可) | 不可(E-C5 で廃止) |
  | Payloadless | fields 指定 = `no_payload_event`、無指定 emit 可 | **payload は「省略 or `{}`」の両方を受理**(本 WP で確定)。extra key 拒否 |
  | Typed(empty) | schema 照合後、無指定/空 fields emit 可 | **厳密 `{}` のみ**(省略不可 — Payloadless と規則を分ける)。extra key 拒否 |
  | Typed(non-empty) | required/type/range/unknown-field 検証 | 同一 validator・同一 error 分類 |

受け入れ = 設計 v2 §6 の 8 テスト + **E-C1 の 6 compile-fail / E-C3 の
全 shape 別 runtime invalid / E-C4 の catalog drift 5 分類 / E-C6 の行列
全セル(5 state × UI・RPC × 省略・`{}`・extra key)を行使するテスト群** +
既存全テスト。

### WP72: 色 C0 — 色空間監査(コード変更なし)

参照: **`docs/design_color_pipeline.md` v4(条件付き Accept)§2-3・§3-1 が正**。
依存: なし(先行可)。見積: 小(read-only)。排他: なし(docs のみ)。

成果物 = `docs/color_migration_manifest.json`:
§2-3 の全項目(shader / RT / texture slot / clear color / vertex color /
capture consumer)について `{path/field, old_encoding, new_encoding,
expected_change: "bit_exact"|"analytic"|"visual_review", reason}` を列挙。
§2-1 capability 表の実デバイス照会結果(windowed surface の
TRANSFER_SRC/DST を含む)、§2-2 anchor への旧 pass 写像案、§2-7 の
既存 consumer(rpc テスト・DCC スクリプト)一覧を含める。

### WP73: 色 C1a — canonical anchor / format_class / output_transform(構造のみ・golden 不変)

参照: **色 v4 §2-2・§6 C1a の 4 条件が正** +
**WP72 C0 レポートの「C1 への引き渡し条件」1〜2
(`docs/design_reviews/2026-07-11_wp72_c0_report.md` — 特に
resolver v1 は現行の B8G8R8A8 channel order を維持すること)**。
依存: WP72(済)、WP70 merge(済)。
見積: 大(frame graph 横断)。排他: `src/core/renderingpass/` /
`src/project/featurecompose.cpp` / pass JSON。

**受け入れ = 色 v4 §6 C1a の 4 条件**(完全写像表 + frame-plan diff /
`resolver_version: 1` = 旧 UNORM 同一解決 / bit-preserving copy /
旧新 binary の最終 RGBA8 hash 完全一致 — 1 byte の差も不合格)+
**レビュー条件 C-C1(round 4 §1.2 の 4 項の逐語転記 — round 5 WP-C1)**:

1. `display` を `TRANSFER_SRC`、windowed swapchain と headless/offscreen の
   実 target を `TRANSFER_DST` で作る。windowed は
   `supportedUsageFlags & TRANSFER_DST` を swapchain 作成前に照会し、
   対応時だけ swapchain `imageUsage` に追加する。format feature の
   transfer source/destination bit も role 表で照会する。
2. copy の直前に `display` を color-attachment write から
   `TRANSFER_SRC_OPTIMAL` へ、acquire 済み target を
   `TRANSFER_DST_OPTIMAL` へ同期し、copy 後は target を `PRESENT_SRC_KHR`
   又は readback 契約が要求する layout へ遷移する。stage/access mask、
   queue ownership、初回旧 layout も frame-plan trace に含める。
3. C1a の source/destination は extent、sample count、texel block size、
   channel order を一致させる。resolver v1 の「旧 format と同一」規則を
   実 target との組にも適用し、copy/resolve の暗黙変換を許さない。
4. `TRANSFER_DST` 非対応 surface では、(a) 全 code byte-exact 実証済みの
   shader copy を使う、又は (b) C1a windowed path を明示的に unsupported
   として開始しない。capture の `unavailable_windowed` は `TRANSFER_SRC`
   不足だけを表すため、この destination 不足と混同しない。

### WP74: 色 C1b — 色意味論の一括移行 + golden 再基準化(単独ゲート)

参照: **色 v4 §2 全体・§3(manifest / 三者比較 / per-case 承認 /
tolerance 正直化)・§4 テスト表・§1 の C0 追補 6 件 +
WP72 C0 レポートの「C1 への引き渡し条件」3〜5(shader_lab pow(0.92) の
所有決定・emissive factor の radiometric 分離・UI atlas 混同禁止・
visual_review 項目の三者比較転記・capture contract 2 の consumer 同時更新)
が正**。依存: WP73。見積: 特大。
排他: シェーダ全域 / loader / swapchain / capture / golden。
`resolver_version: 2`(SRGB/16F 規則)への切替、authored 色 decode、
view/複製戦略、contract 2、§4 の全 analytic fixture 常設、
storage edge ±1 code + final golden の誤差検査を含む。
**再基準化は §3 の 6 手順以外の方法で行ってはならない。**

### WP75: UI U0 — 純 CPU の UI 基盤(schema/layout/入力/semantic validator)

参照: **`docs/design_ui_2d_foundation.md` v8(2026-07-11 round 5 で
**条件付き Accept** — `docs/design_reviews/2026-07-11_ui_v8_rereview_codex.md`)
§4・§7・§9 U0 行が正**。依存: **WP71(EventPayloadSchema)完了後**。
見積: 大。排他: `src/core/ui/`(新設)/ `test/fixtures/ui_semantic/` /
`docs/schemas/`(UI 系のみ)。GPU 描画(U1)は含まない。

**レビュー添付条件 U-C1〜U-C4(round 5 §6 が正本 — 逐語参照)を
acceptance criteria に含める。要点**:

- **U-C1(I11 の完全 state machine 化)**: 状態を
  `pointer_id → {capture: idle | {button, owner, drag_started},
  hover: none | owner}` に拡張し、全表行に pre-state / event 条件 /
  許可・必須 effects / next-state。captured 中は `pointer_up.button ==
  capture.button` 必須、click の owner/target は §2-3 の同一 owner 規則と
  一致。hover は初回 exit 拒否 + owner 切替の exit→enter 順序。effects の
  配列順規範 or 集合比較の一方に固定。idle cancel の zero-effect 表現
  (省略 / 空配列 / 同値扱い)を一意化。negative 6 本 + multi-pointer
  interleave positive を fixture 追加
- **U-C2(I12 の変換規則)**: `content_rect_ui = [0,0,w,h]` 固定
  (letterbox offset は px 側のみ)か edge 個別 half-up 照合かを一意化。
  I12 を containment / scale・edge-rounding の 2 clause に分割し、
  scale 1.5・half 境界・1px letterbox・非整除 framebuffer の fixture
- **U-C3(clause registry の単一正本)**: invariant/clause ID の機械可読
  registry を置き、設計表・coverage schema をそこから生成 or checker が
  registry と manifest を完全一致比較(本文にだけ clause を足しても CI が
  落ちる構成)。I6 の run0/連続/total/上限、I12 の 2 clause、U-C1 の
  違反クラス全登録
- **U-C4(coverage witness の実行)**: CTest gate で coverage schema を
  完全検証(native validator を JSON Schema と同値まで拡張)。各 enum
  entry の値実在・gate と分類の一致を検査。U0 C++ semantic validator が
  全 semantic entry を clause 指定で実行(negative = 指定 clause で fail /
  positive = pass)+ 36 fixture の schema 分類も同 gate に。
  `check_coverage.mjs` 単体の green を「3 段 gate 完了」と扱わない

受け入れ = §9 U0 の 3 段ゲート + U-C1〜U-C4 の fixture/CTest 実行結果 +
既存全テスト。

### WP76: マテリアル M2b-2 — values バインダと C 基盤の完成

参照: **`design_material_shading.md` v1.2 §3-4(属性/欠損既定/render_state)・
§3-8(.surface 自己記述コンテナ)・§4 M2b 行の残り + WP70 の
「スコープ外(M2b-2 へ)」列挙が作業リストの正**。依存: WP70(済)。
見積: 大。排他: `src/core/material/` / `src/core/renderer/materialrender.*` /
`src/project/surfaceformat.*`。

1. **.surface values バインダ**: WP68 パーサの ordered params 宣言から
   std140 レイアウトを確定し、material SSBO(WP70 の set2 b6)へ値を
   詰める。宣言順・明示型・default 必須は WP68 契約どおり。
   **type color の factor は sRGB→linear decode(色 v4 §2-4 の
   encoding×role 表が正 — `"encoding": "linear"` override 対応)**
2. **textures 辞書 binding**: .surface の textures 宣言 → set2 の
   カスタムスロット割付(既存 0-5 の後ろ)。**color/data の role 宣言 →
   SRGB/UNORM view 選択(WP74 の view 機構に乗る)**
3. **ダミーテクスチャ**: 未指定スロットの white/normal/black 既定
   (color/data 両 view — WP74 の white 前例に従う)
4. **公開 resources manifest**: エンジンが .surface シェーダに供給する
   buffer/image/sampler の機械可読一覧(C 層ユーザーの参照文書を
   生成できる形)。shader_contract.md へ節追加
5. **render_state 拡張**: .surface ヘッダの render_state(blend/cull/
   depth)を pipeline へ反映。capability 検証付き(不正組合せは
   名前入りロードエラー)
6. **dump-lowered-material の器**: `pelican_cli dump-lowered-material
   <surface>` が lowering 結果(defines・binding 表・render_state)を
   テキスト出力(B→C 同値 CI の土台 — B 実装前は器のみ)
7. 既定 PBR は現行挙動維持 — **golden 全 17 維持(SKIP 0)+ 全テスト +
   player が受け入れの砦**。新規 fixture: values binder の std140
   オフセット検証・textures 辞書・render_state 反映・dump 出力

### WP77: コンテナ K1 — #フラグメント参照の実ロード

参照: **`design_asset_containers.md` §1 + [PF] v6.3(§3-A 凍結済み)が正**。
構文解析・正準形解決は WP55 で実装済み — 本 WP は**ローダの部分ロード**。
依存: WP55(済)。見積: 中。排他: `src/core/loader/` /
`src/core/model/gltf.*`(view 機構は WP74 の現状を壊さない)。

1. glb の `#mesh/<名前|連結パス>` / `#material/...` / `#node/...` /
   `#animation/...` の**部分ロード**(丸ごとロードして捨てるのではなく、
   対象と依存(メッシュ→マテリアル→テクスチャ)だけを GPU 化)
2. 短い一意名 = 糖衣(フルパスへ解決・曖昧は**複数一致を列挙する
   名前入りエラー**)。同一フルパス重複もロードエラー
3. フラグメントなし = 従来経路そのまま(**既存 golden/テスト無変化**)
4. asset_data.json の models[].path と scene の simplemodelview から
   フラグメント付き参照を受理(例: sotai の 1 メッシュだけ参照する
   fixture を projects/example ではなく test fixture に追加)
5. エラー系 fixture: 未知種別 / 未知名 / 曖昧名 / glb 以外への #mesh
6. 受け入れ = 新 fixture 群 + 既存全テスト + golden 全維持 + player

### WP78: マテリアル M3a — B 層 production baseline(ソース経路)

参照: **`design_material_shading.md` v1.2 §3-1(A/B/C 梯子)・§3-2
(SPIR-V ABI、B はソース専用)・§3-3(パス variant)・§3-6〜3-7
(フック深さ梯子・エンジンシェーダライブラリ ABI・dogfooding)・
§4 M3a 行が正**。依存: WP76(済 — dump-lowered-material の器・values/
textures 機構を使う)。見積: 特大。
排他: `src/core/shader/` の合成系 / `src/core/resources/`(テンプレート・
ライブラリ追加)/ `src/project/materiallowering.*` / `src/project/surfaceformat.*`。

1. **テンプレート合成(shaderc includer)**: .surface の code スニペットを
   エンジン所有テンプレートへ逆 include。ユーザーは
   `pelican_surface_v1` 系関数を書くだけ。**WP68 の identity 契約
   (source.substr(code_offset) == document.code)は維持**
2. **フック検出 = 関数名の反射**(深さ指定キー不要): 定義された
   `pelican_*_v1` 関数名から displace / surface / brdf / lighting を判定。
   **事故防止 3 規則**: ①未知の `pelican_` 名 = 名前入りエラー
   ②brdf と lighting の併存 = エラー(ターミナルフック排他)
   ③空スニペット = エラー
3. **版付きシンボル**: `pelican_surface_v1` 等の v1 シグネチャを凍結
   (shader_contract.md に契約節を追加)
4. **エンジンシェーダライブラリ(ソース形)**: `pelican_light` /
   `pelican_shadow` / `pelican_env_ambient` を engine:// の GLSL include と
   して公開(lib.spv / spv-link は M3b — 本 WP はソース経路のみ)
5. **dogfooding**: 同梱 standard / toon ライティングスニペットを
   **公開ライブラリ関数のみで**実装(エンジン特権 API 不使用の証明 =
   テンプレートと同一経路でコンパイルが通ること)
6. **B→C lowering 同値 CI**: `dump-lowered-material` の出力(WP76)を
   B マテリアルにも適用し、「B は C の糖衣」を fixture で固定
   (lowering 結果のテキスト golden)
7. **エラー翻訳**: 合成後シェーダのコンパイルエラー行番号を
   ユーザースニペット行番号へ写像(#line ディレクティブ)。fixture で
   「スニペット N 行目」表示を検証
8. example に B マテリアル 1 個(toon の見本)+ golden 1 ケース追加。
   **パス variant(PELICAN_PASS_DEPTH / VELOCITY)は shadow との組で
   最低 1 fixture**(影の剥離防止 — §3-3)
9. 受け入れ = 上記 fixture 群 + **既存 golden 全維持(SKIP 0)** +
   全テスト + player。既定 PBR(A 層)の挙動不変

### WP79: コンテナ K2 — glTF シーン抽出 + scene v1 親子

参照: **`design_asset_containers.md` §2 + `design_scene_format.md`
(v1.1)が正**。依存: WP77(済 — メッシュはフラグメント参照で元 glb を
指す)。見積: 中。排他: `src/devcli/` / `src/core/loader/scene.cpp` /
`src/project/sceneformat.*`。

**scene v1 親子の設計判断(本 WP で確定 — 設計者決定済み)**:

- `objects[].parent` = 親オブジェクトの `name`(文字列・省略可)。
  **省略 = ルート(現行と完全同一経路)** — additive な v1 小改訂であり
  version は 1 のまま(lights コンポーネント化と同格)
- parent を使う場合: 親側に `name` 必須 / 未知親 = 名前入りロードエラー /
  **循環 = 参加ノード列挙付きロードエラー**(ロード時検出必須)/
  同名 object が複数あるシーンで parent 参照 = 曖昧エラー
- transform 合成 = `child_world = parent_world × child_local`
  (TRS はローカル値)。**ランタイム実体は WP62 の ECS 親子(EntityId)
  への配線のみ** — 新規機構を作らない
- 抽出ツール(devcli)はエンジンランタイムに一切入らない(配布ビルド
  対象外 — パージ整理は 2026-07-11 ユーザー確認済み)

1. **scene v1 親子**: sceneformat パーサ + SceneLoader の ECS 親子配線 +
   negative fixture(未知親・循環・曖昧)+ 親子 transform の
   golden 1 ケース(回転親の子が正しく公転する解析可能な配置)
2. **`pelican_cli import gltf --extract-scene <glb>`**: ノード階層 →
   pelican.scene(name・TRS・parent)。KHR_lights_punctual → light
   コンポーネント / カメラノード → camera コンポーネント(C1 で 1:1)/
   extras → コンポーネント params
3. メッシュノード → `simplemodelview` + **`#node/<フルパス>` フラグメント
   参照で元 glb を指す**(glb は分解しない — K1 の実ロードに乗る)
4. 出力の決定性(同一 glb → byte 同一の scene JSON)+ round-trip
   fixture(抽出 → ロード → 全ノードの world transform が glTF 直ロードと
   一致)
5. 受け入れ = 新 fixture 群 + 既存全テスト + golden 全維持 + player

### WP80: マテリアル M3b — pelican-spv-link(experimental バックエンド)

参照: **`design_material_shading.md` §3-6(spv-link・WP59 スパイク要件
1〜5・昇格ゲート 7 項目)・§4 M3b 行が正**。一次資料 =
`experiments/spvlink/REPORT.md`。依存: WP78(済 — 同じ B API の裏)。
見積: 大。排他: `src/core/shader/` のリンカ系(新設)/
`src/project/materiallowering.*` は読み取りのみ。

1. **同じ B API の裏の experimental バックエンド**: opt-in フラグ
   (config or PELICAN_SPV_LINK=experimental)でのみ有効。既定は
   WP78 のソース経路 — **既定経路の挙動・golden は完全不変**
2. スパイク要件の本実装(§3-6 の 1〜5 をそのまま受け入れ基準に):
   ①テキストアセンブリ書換禁止 — **SPIRV-Tools / SPIRV-Reflect の API**
   ②ABI 表面 = scalar/vecN/単純 struct 限定を検証で強制(行列・配列・
   リソースハンドル・レイアウト依存型は名前入り拒否)
   ③**CPU バインド表に descriptor type を追加**(combined vs split
   sampler — split を標準形に統一)
   ④Slang `[noinline]` / glslang `--keep-uncalled` の規約化
   ⑤GLSL include fallback(= WP78 経路)の CI 常設維持
3. SPIRV-Headers/Tools/Reflect は **pin/vendor**(SDK 追従禁止)
4. ローカルで実行可能な昇格ゲートの前倒し: コーパス(GLSL 産 +
   Slang 産の各数本 — struct/制御フロー/discard/複数フック)+
   spirv-val ゲート + リンク済み golden + **toolchain 版数・入力 hash 込みの
   再現可能キャッシュキー**(ゲート⑦)
5. スコープ外(昇格判断に残す): 多ベンダ実機マトリクス・fuzzer・
   RenderDoc debug プロファイル・naga web bake — 実行できないゲートを
   「通過した」と主張しない(レポートに未実施として明記)
6. 受け入れ = 上記 fixture 群 + 既定経路の golden 全維持(SKIP 0)+
   全テスト + player

### WP38: スケルタルアニメーション再生(クリップ v1)

参照: 本節が仕様の正(設計判断確定済み)。依存: WP77(#animation 名前
解決)・WP78(スキニングはテンプレート所有 — §3-1 の契約)。見積: 特大。
排他: `src/core/model/gltf.*`(スキン読取)/ `src/core/renderer/` の
スキニング系(新設)/ アニメコンポーネント(新設)。

**設計判断(確定)**:

- glTF skins(joints・inverseBindMatrices)+ animations を読む。
  sampler 補間は **LINEAR / STEP のみ**(CUBICSPLINE は v1 では
  名前入りエラー — 黙って LINEAR に落とさない)。morph target は対象外
- スキニングは **matrix palette SSBO + 頂点シェーダ**(JOINTS_0 /
  WEIGHTS_0)。**テンプレートが所有** — B マテリアルのスニペットは
  1 文字も変わらない(§3-1 の約束の実証)。depth/velocity variant にも
  同じ変位が乗る(影の剥離防止 — WP78 の variant 機構に乗る)
- コンポーネント形式(**グラフ拡張可能な器** — アニメグラフは後続):
  `{"name": "animation", "clip": "<glb>#animation/<名前>",
  "speed": 1.0, "loop": true, "start_time": 0.0}`。
  `"graph"` キーは**予約**(v1 では存在 = エラー)
- 時間は EngineTime 駆動(決定的)。rpc set_time でポーズが決まる —
  golden は固定時刻のポーズで撮る

1. glTF skin/animation の読取(K1 の #animation 解決を実表現に接続)
2. matrix palette 計算(ジョイント階層は WP62 ECS 親子でなく glTF skin
   内部の階層をそのまま評価 — シーングラフに骨を撒かない)
3. テンプレートへのスキニング合流(PELICAN_SKINNED variant)+
   depth/velocity variant
4. fixture: 解析可能な 2 骨の回転クリップ(固定時刻のジョイント行列を
   手計算と一致)+ LINEAR/STEP 補間 + ループ境界 + CUBICSPLINE エラー +
   固定時刻ポーズの golden 1 ケース(件数 REQUIRE 更新を忘れない)
5. example: character.glb か AliciaSolid.vrm にクリップがあれば
   デモ配線(なければ fixture のみで可 — 無理に例を作らない)
6. 受け入れ = 上記 fixture + 既存 golden 全維持 + 全テスト + player

### WP81: コンテナ K3 — pelican-import-tools 創設(PSD 展開 + アトラスパック)

参照: **`design_asset_containers.md` §3(PSD レーン)+ §4 imports 規約が
正**。**新リポジトリ `pelican-import-tools`**(2026-07-12 ユーザー承認 —
ローカル作成、リモートは後日)。エンジン非リンク(外部ツール契約)。
依存: なし(#sprite の消費側 = U1 は後続)。見積: 中。

1. リポジトリ骨格: Python + uv(houdini-adapter の流儀)、pytest、
   README(データ契約 = 入力 → imports/ 出力 + pelican.import manifest)
2. **psd_extract**: PSD → レイヤーごとのトリミング済み PNG + オフセット、
   レイヤーツリー JSON(名前・位置・サイズ・不透明度・表示・グループ)、
   pelican.import manifest(sha256)。**psd-tools に乗る(自前パーサ禁止)**
3. **atlas_pack**: PNG 群 → アトラスページ PNG + `pelican.atlas` v1 JSON
   (`{schema, version, pages: [{image, size}], sprites: {名前: {page,
   rect[l,t,r,b] 右下排他}}}` — `#sprite/<名前>` の解決先)。
   **bleed padding**(UI 文書の import 規約)+ **決定的パッキング**
   (同一入力 → byte 同一出力。名前→サイズの安定順)
4. manifest / atlas JSON の形式は pelican2 の `design_asset_containers.md`
   §3 と往復整合(形式の追加はエンジン先行のサブセット原則 —
   `#sprite/` 語彙は [PF] v6.3 で定義済み)
5. 受け入れ = pytest green(最小 PSD fixture + 合成 PNG fixture・
   決定性 = 2 回実行 byte 一致・トリム/オフセット/透明の解析検証)

### WP82: 起動高速化 — シェーダキャッシュ + モデルロード並列化

参照: 本節が仕様の正(2026-07-12 実測に基づく — rpc headless で
起動 8.4s の内訳: ランタイムシェーダコンパイル **2.9s** +
ModelAssetContainer 初期化(モデルロード)**4.9s** が支配的。
ユーザー要望「起動からの描画が遅い」への回答)。依存: WP78(variant
キャッシュキー)。見積: 大。排他: `src/core/shader/` のコンパイル入口 /
`src/core/asset/` / `src/core/model/gltf.*` のロード駆動部。

1. **シェーダディスクキャッシュ**: コンパイル済み SPIR-V を
   `<project>/.pelican/shader_cache/`(gitignore 済み区画)にキャッシュ。
   キーは「ソース SHA-256 + 全 defines + shaderc バージョン + target env +
   エンジンのシェーダ契約 salt」(WP80 の再現可能キー設計と同じ流儀 —
   **キーに入れ忘れた次元が stale hit を生む**ので列挙を仕様化)。
   ヒット時は shaderc を呼ばない。破損キャッシュは黙って再コンパイル
   (エラーにしない)。**キャッシュの有無で golden が変わらないことを
   同一プロセス連続 2 回コンパイルの byte 比較で固定**
2. **モデルロードの並列化**: モデル間並列(thread pool)+ glb 内
   テクスチャの stb デコード並列。**GPU アップロード(queue submit)は
   直列のまま**(転送 queue の並行化はスコープ外)。ロード完了順の
   非決定性が **ECS 登録順・描画順に漏れない**こと(登録は宣言順で
   バリア — 決定性の砦)。rpc/golden/replay の決定性テスト全維持
3. **起動フェーズレポート常設**: 既存 profiler Timer を集約し、起動完了時に
   `startup: config Xms / vulkan Xms / shaders Xms (cache hit N/M) /
   models Xms / total Xms` を 1 行 INFO ログ + get_status.startup に出す
   (退行検知の観測点)
4. fixture: キャッシュ hit/miss の挙動(2 回目起動でコンパイル 0 件)・
   破損キャッシュ回復・並列ロード後の ECS 登録順決定性(2 回実行一致)
5. 受け入れ = **起動実測の before/after をレポートに記録**(同一マシン)+
   既存全テスト + golden 全維持(SKIP 0)+ player。目標値: 2 回目以降の
   起動でシェーダ 2.9s → 0.1s 未満、モデル 4.9s → 2.5s 未満(sponza
   単体が支配的なら実測根拠つきで目標を修正してよい)

### WP83: マテリアル M3.5 — 名前付きスクリーンスナップショット(訂正版 v2)

参照: **`design_material_shading.md` v1.2 §3-7-3(名前付きスナップショット —
Godot SCREEN_TEXTURE の教訓・v1 は opaque 後 1 点のみ・逐次屈折非対応明記・
コピーコストの可視化)+ §3-9(screen_inputs → forward 自動振り分け)が正**。
**追加要件(2026-07-12 ユーザー決定・同日 2 回訂正)**: 定義ファイルは
分割のまま、**パスをノードとして見られる可視化ツールで一か所にまとめて
見たい**。①「単一の合流 JSON をエンジンに課す」解釈は撤回 ②「web ツール
(my_webpage WW8)」も撤回 — **ビューアはエンジン内ツール(ImGui)**
(2026-07-12 ユーザー決定。web 版の実装着手分は my_webpage から全撤去
済み)。エンジン側 dump への追加はスナップショットノードと
screen_inputs 消費が自然に現れる最小 2 点のみ。ビューア本体 = WP86。
依存: WP78(B 層)・WP73(canonical anchor)。見積: 大。
排他: `src/core/renderingpass/` / `src/project/materiallowering.*` /
`src/core/communication/rpcserver.cpp`(get_frame_plan)。

1. **スナップショット定義**: rendering config に
   `"snapshots": [{"name": "opaque_color", "after": "<anchor|pass>"}]`。
   v1 は **opaque 後の 1 点のみ許可**(2 点目以降・透明後は名前入り
   起動時エラー — 制限は明示)。コピーは frame graph の実ノード
   (プラン比較テストに現れる)
2. **.surface の `//! screen_inputs: ["opaque_color"]`**: WP68/76 ヘッダの
   拡張。宣言したマテリアルは自動で forward 透明系パスへ振り分け
   (§3-9)。未定義スナップショット名 = 名前入りエラー。シェーダには
   生成 accessor(`pelican_screen_<name>(uv)`)で届く(binding は
   resources manifest に追記)
3. **透明同士の逐次屈折は非対応と明記**: 同一スナップショットを読む透明
   マテリアル同士は互いの結果を見ない(スナップショットは 1 回コピーの
   固定内容)。この意味論を文書 + fixture で固定
4. **plan dump は最小拡張**: `get_frame_plan` / `--dump-frame-plan` の
   `pelican.frame_plan` version 1 を維持し、追加はコピー量を持つ
   `snapshot_copy` 実ノードと、`screen_inputs` 消費側ノードの通常の
   `reads` / barrier 依存だけとする。RT 一覧・anchor 対応表・マテリアル横断
   消費表などの合流ビューは作らない(可視化はエンジン内ツール WP86 の責務)
5. fixture: 屈折デモ 1 個(example に .surface + golden 1 ケース —
   件数 REQUIRE 更新)・スナップショット未定義/2 点目/透明後の
   エラー 3 本・プラン fixture にスナップショットノードと read 依存
6. 受け入れ = 上記 fixture + 既存 golden 全維持(SKIP 0)+ 全テスト +
   player

### WP84: コンテナ K4 — imports.rules.json(import ルール表)

参照: **`design_asset_containers.md` §4 が正**(glob → レシピ中央表・
per-file .meta 不採用・ルール外ファイルは何もしない・設定 3 層すべて git・
ローカル層に import 意味論を置かない)。依存: WP79(extract_scene レシピ)・
WP81(psd_layers / atlas_pack レシピ = pelican-import-tools)。見積: 中。
排他: `src/devcli/` / `src/project/`(ルール表パーサ)。

1. `imports.rules.json` パーサ(schema/version/rules/defaults 3 層、
   未知キー・不正 glob は名前入りエラー)。glob は `**` 対応・
   `\` 区切り拒否([PFW] §2-6 既存規則)
2. `pelican_cli import --rules`(または引数なし規定動作)がルール表を
   評価し、マッチしたファイルへレシピを起動:
   `extract_scene` = devcli 内蔵(WP79)/ `psd_layers`・`atlas_pack` =
   **pelican-import-tools を外部ツール契約で起動**(パスは設定 or PATH、
   見つからなければ導入手順つきエラー — エンジンに Python をリンクしない)
3. 出力は imports/ + pelican.import manifest(既存規約)。再実行は冪等
   (入力不変なら生成物 byte 不変)。生成物上書きは --force 規約(§5)
4. ルールにないファイルは無反応(fixture で確認 — ゼロ手数の維持)
5. fixture: ルール評価(match/defaults/組み込み既定の優先順)・
   外部ツール不在エラー・冪等性・`extract_scene` の end-to-end 1 本
6. 受け入れ = 新 fixture + 既存全テスト + golden 全維持 + player

### WP85: ImGui 導入(エンジン開発者 UI の基盤)

参照: **`design_ui_2d_foundation.md` v8 §8(隔離規約)が仕様の正**
(2026-07-10 に方向承認済み・本日 WP 化)。依存: なし。見積: 大。
排他: `src/core/` の ImGui ユニット(新設)/ `src/core/window/`(入力
配線)/ CMake ユニット定義。

1. **PELICAN_WITH_IMGUI ビルドユニット**(既定 ON・dist-config は OFF を
   導出)。OFF 時はソース・シンボル・フォント資産を一切含めない
   (WP40 の OFF スモークの流儀で fixture)
2. **backend**: GLFW + Vulkan(公式 backend を vendor)。
   **Window が GLFW callback の唯一の owner** — ordered event を
   ImGui adapter と InputState へ一度ずつ渡す(backend の callback
   install 禁止 = 二重配信根絶)
3. **入力優先順** `raw → ImGui(WantCapture)→ pelican.ui → gameplay` を
   実装 + テスト
4. **非実行の隔離**: golden / replay / headless / rpc 駆動では ImGui
   コールバック自体を実行しない。「frame plan に imgui pass が無い・
   入力消費が不変・公開 API 呼出 0 回」をテストで固定(§8)
5. imgui pass は canonical anchor 列の `imgui` 位置(色 v4 §2-2)に入る。
   multi-viewport / docking は v1 OFF 固定。device loss 時のバックエンド
   資源再生成の対応可否を明示(レポートに記録)
6. 最初の中身はデモウィンドウ + FPS/フレーム統計 1 枚(ツール本体は
   WP86 以降)。F1 等でトグル(キーは actions 経由 — 生 GLFW 読み禁止)
7. 受け入れ = OFF スモーク + 隔離テスト + 既存 golden 全維持(SKIP 0)+
   全テスト + player(ImGui 表示ありのスクリーンショットをレポートに)

### WP86: Plan viewer — パスのノードグラフ可視化(ImGui ツール)

参照: **本節が仕様の正(2026-07-12 ユーザー要望: 分割された描画定義
(rendering config / feature / .surface)を「パスをノードとして見られる
ツール」で一か所にまとめて見たい — エンジン内ツールとして)**。
依存: WP85(ImGui)・WP83(スナップショットが plan に出る)。見積: 大。
排他: ImGui ツール層(新設 — engine core への変更は原則なし)。

1. データ源は**実行中エンジンの frame plan そのもの**(get_frame_plan と
   同一の公開意味論 — D0。エンジン内部構造への裏口アクセス禁止)+
   material lowering 情報(dump-lowered-material と同一意味論)
2. **ノードグラフ表示**(ImDrawList 自前描画 — 外部ノードグラフ
   ライブラリは v1 では入れない):
   - パス = ノード(種別色分け: raster / compute / feature 由来 /
     エンジン常設(output_transform 等)/ スナップショットコピー)
   - RT/リソース = エッジ(read/write の向き。RMW 連鎖が視覚的に追える)
   - レイアウトは実行順の左→右層状(topological)で**決定的**
   - ズーム / パン / ノード選択
3. 選択パネル: パスの I/O・format(format_class 解決結果)・
   load/store・由来 feature・anchor 位置。リソース選択で読み書きパスを
   ハイライト。スナップショット選択でコピー点とバイト数
4. **マテリアル重ね表示**: マテリアル一覧(surface stem・screen_inputs・
   振り分け先パス・render_state)からパスへの対応をハイライト
5. 隔離: WP85 の規約に完全準拠(golden/replay/headless で非実行・
   PELICAN_WITH_IMGUI=OFF で消える)
6. 受け入れ = example プロジェクト(hdr on/off・shadow・bloom・
   スナップショット入り)での表示スクリーンショットをレポートに +
   既存 golden 全維持 + 全テスト + player

### WP87: UI U1 — GPU 描画(quad buffer / アトラス / clip / feature 化)

参照: **`design_ui_2d_foundation.md` v8 §1(quad ABI)・§2-2(Viewport
Transform)・§6(purgeable feature 化・canonical anchor)・§9 U1 行が正**。
依存: WP75(U0 済)・WP74(C1b 済 — U1 は C1b 後の規定)・WP81
(K3 済 — pelican.atlas v1)。見積: 特大。
排他: `src/core/ui/` / `src/core/renderer/uicontainer.*` /
`src/core/resources/features/`(ui feature 新設)。

1. **quad buffer**: U0 の draw command 列 → QuadVertex(20B stride:
   pos float2@0 / uv float2@8 / color u8x4 RGBA8_UNORM linear@16)+
   uint16 index。上限 16384 quad / 256 unique clip(超過 =
   `limit_exceeded`)
2. **DrawRun**: merge key (pipeline_key, texture_page, sampler_key,
   clip_id) の隣接マージのみ。white テクセル page 0
3. **アトラス接続**: `pelican.atlas` v1 JSON(WP81 の出力)を読み、
   `#sprite/<名前>` 参照を page/rect に解決。**アトラス画像は color role =
   SRGB view**(WP74 機構)。bleed padding 前提の UV
4. **clip**: nested clip の交差 → scissor(§4 の px 変換規則)
5. **ui feature 化**: `engine://features/ui.json` の purgeable feature に
   移行(現行の特殊 pass 型を置換)。**不参照時に UiModule・GPU 資源・
   パーサが立ち上がらない**ことをテスト保証。canonical anchor
   `pelican_ui` に入る。旧 ui_overlay.json は atomic 移行(strict v1 流儀)
6. panel(9patch)/ image widget の実描画
7. golden: 2 アトラス交互重なり・nested clip・旧 UI 移行(explosion/
   ui_test の見た目維持)— 件数 REQUIRE 更新を忘れない(現在 21)
8. 受け入れ = §9 U1 ゲート + 既存 golden 全維持(SKIP 0)+ 全テスト +
   player(example の UI が従来どおり出るスクリーンショット)

### WP88: temporal T1+T2 — RT history 機構 + velocity(機構のみ)

参照: **`design_postprocess_temporal.md` §1〜2・§4 T1/T2 行が正**。
**スコープ確定(2026-07-12 ユーザー決定): エンジンは機構(T1 history +
T2 velocity)まで。TAA・アキュムレーション等の効果シェーダは
ユーザーが書く領分** — 機構の実証は最小 accumulation feature(前フレーム
50% ブレンド)の golden 1 個に留める。依存: WP73(canonical anchor)。
見積: 大。排他: `src/core/renderingpass/`(history)/
`src/core/renderer/`(前フレーム行列)。

1. **T1 history**: RT 宣言 `"history": true` で 2 面自動管理。
   `@history` 付き read は**フレーム内依存を作らない**(前フレーム面を
   読む)。プランナ/ランタイム/plan スキーマ追記 + fixture。
   初回フレームの history 面は clear 済み(未定義読み禁止)
2. **T2 velocity**: model matrix バッファの 2 面化(前フレーム行列)+
   screen-space velocity(RG16F)を出す velocity feature。
   カメラジッタ(TAA 用 projection 介入)は**スコープ外**(設計未決 2 —
   ユーザーの TAA 設計と同時に決める)
3. 実証 = accumulation feature(50% ブレンド)の golden(step_frame ×N で
   決定的)+ velocity の解析 fixture(等速移動物体の velocity 値)
4. **ユーザーが TAA を書くための出口を確認**: feature JSON +
   .surface/生シェーダから `@history` と velocity を読めること
   (ドキュメント: adding_features.md にレシピ追記)
5. 受け入れ = fixture 群 + 既存 golden 全維持(SKIP 0)+ 全テスト + player

### WP89: 入力 I3 — 収録とリプレイ(+ カメラ収録の焼き出し)

参照: **`design_input_actions.md` §4(改訂済み: 収録単位 = ordered
InputEvent 列 + フレーム境界マーカー、`pelican.input_seq`)+
`design_ui_2d_foundation.md` v8 §2-1(FrameInput/event_seq — WP69 実装済み)
が正**。依存: WP69(済)。見積: 大。
排他: `src/core/input/` / `src/core/communication/`(record/replay rpc)。

1. **収録**: ordered InputEvent 列(event_seq + フレーム境界)を
   `pelican.input_seq` v1(JSONL)へ記録。開始/停止は rpc
   (`start_input_record` / `stop_input_record`)と CLI フラグ
2. **リプレイ**: `--replay <file>`(または rpc)で同列を注入 —
   スナップショットは再生時に再構成(WP69 の同一位相経路)。
   リプレイ中はホットリロード無効(確定規約)・実入力は遮断
3. **決定性の証明**: 収録 → リプレイ 2 回で rpc capture が byte 一致する
   fixture(WASD シナリオ — WP49 の inject_input 資産を流用)
4. **カメラ収録 → transform_seq 焼き出し**: リプレイ実行中のカメラ
   world transform を transform_seq v1 として `imports/` +
   pelican.import manifest に出力(`pelican_cli bake-camera --replay ...`
   — devcli 側。DCC 往復の入口)
5. 受け入れ = 上記 fixture + 既存全テスト + golden 全維持 + player

### WP90: G2 — ゲームロジック DLL ホットリロード

参照: **`design_game_logic_native.md` G2 節が正**。依存: G1a/G1b(済)。
見積: 特大。排他: `src/core/userpublic/` のロード機構 / PELICAN_PROJECT
ビルド系。

1. プロジェクトゲームコードを DLL としてビルド(PELICAN_PROJECT の
   出力形態追加)し、実行中に再ロード
2. **状態の扱いは v1 = 全リセット方式**(シーン再ロード同等)から:
   リロード検知 → teardown(WP62 の例外安全 teardown に乗る)→
   新 DLL ロード → シーン再構築。オブジェクト単位の状態移送は v2
   (欲張らない — まず「保存 → 数秒で反映」の体験を成立させる)
3. 安全策: リロード中の rpc/replay は拒否(名前入り)・DLL の
   ABI 版数チェック(エンジン版と不一致は拒否)・ロード失敗時は
   旧 DLL 継続 + エラー表示(クラッシュさせない)
4. Windows の DLL ロック回避(コピーしてロードする pdb 対応込みの定石)
5. fixture: example の playercontrol を書き換え → リロード → 挙動変化を
   rpc で検証。失敗 DLL(リンクエラー相当)で旧動作継続
6. 受け入れ = fixture + 既存全テスト + golden 全維持 + player で
   実際に F5 相当の再ロードを 1 回行うスクリーンショット/ログ

### WP91: 入力 I4 — ゲームパッド + バインディングプロファイル

参照: **`design_input_actions.md`(アクション層)+ 本節の設計判断が正**。
依存: WP89(収録形式に pad イベントが乗るため後)。見積: 中。
排他: `src/core/input/`。

**設計判断(確定)**:

- GLFW の gamepad API(SDL_GameControllerDB 互換マッピング内蔵)を使う。
  新規依存なし。**ビルドユニットは作らない** — パージ境界はアクション層の
  データ(pad をバインドしなければポーリング含め完全不活性)
- **バインディングプロファイル**: `pelican.input_actions` を
  「アクション定義」と「バインディング(プロファイル)」に分離。
  プロファイルは複数持て(`profiles/gamepad.json` / `profiles/arcade.json`
  等)、起動引数 or project 設定 or rpc で切替。**ゲーム筐体などの独自
  レイアウトはプロファイル追加だけで対応**(2026-07-12 ユーザー要望)
- 未知ボタン/軸はプロファイル側の named エラー(黙って無視しない)。
  デッドゾーン・軸反転はプロファイルの属性

1. GLFW joystick/gamepad の InputEvent 化(event_seq に乗る =
   I3 の収録/リプレイが自動で pad 対応)
2. プロファイル分離 + 切替 + example に gamepad プロファイル 1 個
3. fixture: プロファイル切替・デッドゾーン・収録リプレイ(pad 入力の
   決定性)・未知ボタンエラー
4. 受け入れ = fixture + 既存全テスト + golden 全維持 + player

### WP92: KTX2 テクスチャレーン(エンジン読取 + import レシピ)

参照: **本節が仕様の正(2026-07-12 ユーザー承認)**。狙い = デコード消滅
(起動)+ VRAM 削減(BCn は GPU 上も圧縮のまま)+ ミップ焼き込み。
依存: WP74(SRGB/UNORM view)・WP84(ルール表)。見積: 大。
排他: `src/core/loader/` の画像読取 / import-tools(レシピ追加のみ —
**別リポジトリへの書込は import-tools のレシピ 1 件に限定**)。

**サブセット(エンジン先行の原則)**:

- エンジンは **KTX2 の制限サブセットを自前パース**(新規ライブラリ
  リンクなし): supercompression なし / 形式 = `R8G8B8A8_{UNORM,SRGB}` +
  `BC7_{UNORM,SRGB}` + `BC5_UNORM`(normal 用)/ 2D・全ミップ必須。
  範囲外は「何が非対応か」を含む名前入りエラー(Basis/zstd は将来拡張)
- color/data role → SRGB/UNORM は WP74 の view 機構どおり(BC7 にも
  SRGB variant がある)。ミップは KTX2 の levels をそのまま
  vkCmdCopyBufferToImage(ランタイム生成なし)
- 適用先 = **スタンドアロンテクスチャ**(material textures 辞書・
  UI atlas 画像)。glb 内蔵(KHR_texture_basisu)は対象外と明記

1. エンジン: KTX2 サブセットパーサ + ローダ配線 + capability 照会
   (BC7/BC5 の format feature — 非対応 GPU は名前入りエラー)
2. fixture: RGBA8 KTX2 はテスト内ライタで生成(決定的)。BC7/BC5 は
   コミット済み小 fixture(生成手順を README 記録)。known-value
   (SRGB decode 188 系)を KTX2 経由でも検証
3. import-tools: `ktx2` レシピ = **toktx(KTX-Software)を外部ツール
   契約で起動**(不在は導入手順つきエラー — psd-tools と同じ流儀)。
   ルール表の組み込み既定に `.png → ktx2` は**入れない**(opt-in)
4. 効果測定: sponza のテクスチャを KTX2 化した場合の起動 models 時間
   before/after をレポートに記録(可能なら)
5. 受け入れ = fixture + 既存全テスト + golden 全維持 + player

### WP93: UI U2 — 対話ウィジェットと E1 emit・rpc 再生

参照: **`design_ui_2d_foundation.md` v8 §3(2 レーン・emit 文法)・
§9 U2 行(ゲート)が正**。依存: WP87(U1 済)・WP71
(EventPayloadSchema 済 — emit の照合先)。見積: 特大。
排他: `src/core/ui/` / `src/core/renderer/debugtext.*`(互換ゲートのみ)。

1. **bitmap label / button**: 同梱グリフ表(整数 advance — §7 の決定性
   規約)によるテキスト描画。button は U0 の capture 状態機械に接続
   (hover/pressed の見た目 = UI-local 同フレーム)
2. **UI-local 状態と commit 境界**: §3 のフレーム一括 commit
   (UiCommandBuffer)を実描画に接続
3. **E1 emit**: emit 文法(§3 — fields/from)を EventPayloadSchema
   (WP71)と照合して**次フレーム配送**。invalid ①〜⑦の実 fixture
   (U0 の error code 表と 1:1)
4. **rpc の ordered click/drag/replay**: inject_input のポインタ事象で
   button click → イベント発火までが決定的に再現(2 回実行 byte 一致)。
   WP89 の収録/リプレイにも自然に乗ることを fixture で確認
5. **debug_text 互換ゲート(R6 — tolerance 0)**: 旧 debug_text 経路と
   UI 共通経路で同一 fixture を描き **byte-exact 比較**(意図的な色
   意味論変更時のみ理由記録付き versioned 更新 → 以後再び 0)
6. 受け入れ = §9 U2 ゲート + semantic fixture 全維持 + 既存 golden
   全維持(SKIP 0・新規は件数 REQUIRE 更新)+ 全テスト + player
   (ボタンを rpc click して反応するデモのスクリーンショット)

### WP94: アニメ A0 — 公開契約の凍結(CA0-Spec + CA0-Probe)

参照: **`design_animation_graph.md` v2.1 §1・§7 が正**。受入条件 =
敵対レビュー `docs/design_reviews/2026-07-12_anim_v2_hotreload_review_codex.md`
**§4.1(CA0-ABI)・§4.2(CA0-Interval)を逐語で満たすこと**。
依存: WP38(済)・WP90(G2 ABI version gate — DLL 境界の先行例)。
見積: 特大。排他: `src/core/animation/`(新設)+ public header
(`src/core/userpublic/animation/` 等の新設)。既存 WP38 経路の
挙動変更禁止(A1 の仕事)。

A0 の性格 = 「fixture で契約を決める」のではなく「**公開契約を fixture で
検証する**」。完了条件は fixture green だけでなく、**凍結 public header +
normative semantics 文書が存在し、その header を二世代 fixture が検査する**
こと(レビュー §4.1 末尾)。

1. **CA0-Spec(実装なしで凍結可能な成果物)**:
   - public header: 固定幅 opaque handle(invalid 値・generation 込み)、
     `struct_size` / `version` / reserved-zero 先頭固定の in/out 構造体、
     status/error enum
   - Rig/PoseLayout/SkinBinding/Clip/Cursor の identity と exact
     compatibility・stale・destroy/unload 規則の normative 文書
     (SkinBinding 自身の identity/generation と mesh/skin identity を
     含む — Pose generation 検査での代用禁止。レビュー §1 #2)
   - frame lifetime・read/write alias・thread ownership・failure
     atomicity・phase 登録/tie-break・commit revision の規範
   - **Pose バッファの実装形(scratch arena vs opaque handle)をここで
     決定**(§8 未決 1 の解消)
   - **commit と temporal history の契約 = 設計書 §1-5 の 6 項を
     normative に含める**(N/N-1 palette・publishAnimationFrame /
     advanceTemporalHistoryAfterRender の分離・reset 規則)
   - blend 記述に additive space(local vs model/mesh)区別と
     event/marker 抑制 policy を含める(レビュー §1 #6)
   - Clip metadata の列挙(source rig・time range・wrap mode・channel
     inventory・annotation identity・sampling context/cursor generation)
   - old client/new engine・new client/old engine の descriptor
     negotiation 規則
2. **CA0-Probe(最小実装 + fixture)**:
   - arena/opaque buffer: acquire → writable view → frame reset、
     64-byte alignment、二体 parallel、DLL reload/stale generation
   - N-way quaternion reference algorithm(入力順・sign 正準化・
     正規化・zero weight・許容誤差)を実装し golden 化
   - wrap/reverse/multi-loop interval advance(§4.2 の cursor 原子性・
     `capacity/count` 二段式・`dt` vs seek 区別・同時刻 `(source,
     ordinal)` 順を含む)、local-to-model、commit 一往復
   - **外部 game DLL を旧/新 header でビルドし、`sizeof/offsetof/
     struct_size` と unknown tail 無視を検査する二世代 fixture**
3. 受け入れ = CA0-Spec 文書 + 凍結 header + CA0-Probe fixture 全 green +
   既存全テスト + golden 全維持(SKIP 0)+ player

### WP95: skinned velocity の previous palette(WP88 欠陥修正)

参照: 敵対レビュー同上 **§5 が正**(v2 レビューで発見された現行欠陥 —
アニメ基盤と独立に修正可能)。依存: WP88(済)。見積: 小〜中。
排他: `src/core/renderer/polygoninstancecontainer.*` /
`src/core/resources/velocity_skinned.vert` / renderer の velocity 配線。

現象: `velocity_skinned.vert` が current skin matrix で作った同一
`local_position` を current/previous 両方に使うため、**actor/camera 静止で
骨だけが動く場合 deformation velocity = 0**。

1. `PolygonInstanceContainer` に previous skin palette バッファを追加
   (current と同 layout・同 instance identity)
2. velocity pass の skinned 経路は current palette / previous palette で
   それぞれ `local_position` を計算(`pelican_skin_matrix` の previous
   variant)
3. history 前進は render 終端の既存 advance 位置で一度だけ(commit 側は
   current を publish するだけ — 設計書 §1-5 の 2・3)
4. reset 規則: 初回・resize・`set_time`・instance place/remove 時は
   previous=current(zero velocity)
5. fixture: 骨のみ運動(モデル行列不変)で velocity が非ゼロ、
   `set_time` 直後は zero velocity。既存 velocity golden 維持
6. 受け入れ = 新 fixture + 既存全テスト + golden 全維持(SKIP 0・
   velocity golden が意味的に変わる場合は理由記録付き再基準化)+ player

### WP96: アセット HR0 — FileWatcher/Reconcile 基盤(GPU なし)

参照: **`design_asset_hot_reload.md` v2.1 §0〜2・§6・§7 が正**。受入条件 =
再レビュー `docs/design_reviews/2026-07-12_hr_v2_2d_v1_review_codex.md` の
**HR-C1(digest 三状態)・HR-C3(polling reconcile)・HR-C4-1/2(status
所有と gate adapter)を逐語で満たすこと**。依存: なし(resource handler は
fake のみ — HR1 以降が実差し替え)。見積: 大。
排他: `src/core/watch/`(新設)+ `ContentDigest` utility 新設。
既存 `ShaderLibrary::reloadModifiedSources` の**動作は変更しない**
(gate adapter 化のみ — 時刻 poll の削除は HR2-S)。

1. **Win32 watcher state machine**(設計 §1): FILE_FLAG_OVERLAPPED・
   64KiB 以下 buffer・即 re-arm・overflow(0 bytes /
   ERROR_NOTIFY_ENUM_DIR)→ subtree inventory rescan・rename OLD/NEW
   非 pairing・停止手順(CancelIoEx → completion 回収 → 解放 → close →
   join)・store 重複 dedupe・reparse escape 拒否
2. **ContentDigest utility**(§2-1): streaming SHA-256(picosha2)・
   canonical AssetKey・安定 read(size/mtime/identity 前後比較 + retry)。
   **§2-1a の三状態(observed/live/pending + self-write token)を
   HR-C1 逐語で**
3. **gate epoch + reconcile**(§2-3): 中央 gate(リプレイ/strict/rpc)
   購読・disable/resume の arm 先行 barrier・
   `disabled|reconciling|watching|polling|degraded` 状態。
   **polling fallback も HR-C3 逐語の同状態機械**(fake clock fixture)
4. **ReloadQueue**: AssetKey 分類・同一リソース合流・fake handler での
   frame 境界 apply(実 handler なし)
5. **get_status.reload の watcher 部分**(state/epoch/watcher error)を
   HR0 が所有(HR-C4-1)。`EngineLaunchConfig::shader_hot_reload` を
   中央 gate の adapter 化(HR-C4-2 前半 — 二重適用なしを fixture で)
6. **§6 の非 GPU integration suite 全部**: 実 FS(modify/create/delete/
   atomic-save rename/Unicode/長パス)・synthetic overflow 注入→rescan・
   停止中 CancelIoEx(UAF なし)・resume race(最終 hash が一度だけ)・
   polling 降格/復帰・dedupe/escape・self-write 4 ケース(HR-C1-4)
7. 受け入れ = 上記 suite 全 green + 既存全テスト + golden 全維持
   (SKIP 0)+ player。CI 不安定を理由に実 FS テストを削らない
   (タイムアウト余裕と リトライは可・削除は不可)

### WP97: アニメ A1 — 機構 jobs(凍結 ABI の実体化)

参照: **`design_animation_graph.md` v2.1 §1(全部)・§7 A1 行が正**。
ABI の正本 = WP94 で凍結した `src/core/userpublic/animation/abi_v1.hpp` +
`docs/animation_abi_v1.md`(**ABI の変更は additive のみ・既存
struct/挙動の変更禁止** — 変更が必要と判断した場合は実装せず質問)。
依存: WP94(済)・WP95(済 — previous palette の描画側)。見積: 特大。
排他: `src/core/animation/` + `src/core/ecs/predefined/animationsystem.*` +
`src/core/renderer/polygoninstancecontainer.*`(palette 接続のみ)。

1. **実装 jobs**(CA0-Probe の probe 実装を本実装へ昇格):
   Rig/PoseLayout/SkinBinding の実 asset 由来構築(WP38
   `SkeletalModelData` から。node 名保存の追加を含む — 設計 §1-1 注記)、
   `samplePoseAt`(caller-owned PoseView)、`advanceCursor`(regex 禁止の
   ABI 規範どおり)、normal blend(per-joint weights 含む N-way 規範 =
   probe の reference algorithm と golden を共有)、local-to-model、
   instance animation-frame commit
2. **commit → renderer 接続**: `publishAnimationFrame` が
   PolygonInstanceContainer の current palette を publish(WP95 の
   previous palette / `advanceTemporalHistoryAfterRender` 位置と §1-5
   契約どおりに接続 — commit は previous を上書きしない)
3. **既存 clip component 互換**: 現行 AnimationSystem(WP38 経路)を
   新 jobs の上に移行し、**既存 skeletal golden・fixture を全維持**
   (byte 一致 — 数学実装を変えた場合は §6 の versioned 再基準化手続き +
   理由記録。安易な再基準化は不可)
4. A1.5 の敵対 fixture の先取りは任意(non-joint 祖先・同骨数別 rig は
   A1.5 の WP で正式化)
5. 受け入れ = 新 fixture + 既存全テスト + golden 全維持(SKIP 0)+
   player(AliciaSolid の joint 上限 WARN 挙動も不変)

### WP98: アセット HR1 — identity / transaction 基盤

参照: **`design_asset_hot_reload.md` v2.1 §3-1・§3-1a・§4・§5・§7 が正**。
受入条件 = 再レビュー
`docs/design_reviews/2026-07-12_hr_v2_2d_v1_review_codex.md` の
**HR-C2(identity 三概念分離)を逐語 + HR-C4-1(status の additive
拡張)+ HR-C4-4(frame-boundary barrier fixture)**。
依存: WP96(HR0 — 済)。見積: 大。
排他: `src/core/watch/`(HR0 への追記)+ identity/transaction の新設
ユニット。**実リソース(texture/shader/model)の差し替えは実装しない**
(HR1-T/M・HR2 の仕事 — 本 WP は fake resource で framework を検証)。

1. **canonical AssetKey の実装**(§3-1): project logical reference +
   fragment 正本。物理 file identity は watcher dedupe 限定
2. **identity 三概念**(§3-1a = HR-C2 逐語): `LogicalAssetId`(宣言
   寿命中 stable)/ slot `generation`(unbind/destroy 後の再利用時のみ
   増加)/ `content_revision`(commit 成功ごと単調増加)。互換性破断
   検出 revision は別 field
3. **reverse dependency index**: AssetKey → 参照 resource の辞書 +
   edge の除去/公開を handle table swap と同一 commit barrier で可視化
4. **CPU candidate / staged commit framework**(§4): parse all →
   validate all → stage all → topological commit。途中失敗 = candidate
   のみ破棄・global 状態不変。DeletionQueue 接続点の定義
5. **frame-boundary barrier**(HR-C4-4): 複数 logical table の commit を
   一つの barrier で公開し、observer が group の半端な revision を
   読めないことを fixture 化
6. **status**(HR-C4-1): `get_status.reload` に resource
   counters/errors を additive 拡張(HR0 所有の watcher 部分は不変)
7. fixture(fake resource): 同一 asset 1000 reload で LogicalAssetId
   不変 + content_revision 単調増加 / 削除→再宣言で旧 handle stale /
   group rollback で ID・revision・edge・counts 全不変(HR-C2-4)
8. 受け入れ = 上記 fixture 全 green + 既存全テスト + golden 全維持
   (SKIP 0)+ player

### WP99: アニメ A1.5 — 敵対 fixture

参照: **`design_animation_graph.md` v2.1 §7 A1.5 行が正**。対象 = WP97 の
jobs 実装(`src/core/animation/animationjobs.*`)と凍結 ABI v1。
**実装コードの挙動変更は原則禁止 — fixture が実バグを見つけた場合のみ
最小修正 + レポートに根拠を記録**(仕様変更と思われる場合は実装せず質問)。
依存: WP97(済)。見積: 中〜大。排他: test/ 配下 + 発見バグの最小修正。

fixture リスト(§7 A1.5 + レビュー由来の追補。各項は「期待値が仕様の
どの行から来るか」をコメントで引用すること):

1. **non-joint 祖先**: skin joint でない中間 node(回転持ち)を挟んだ
   rig で palette が正しい(joint-only 縮約では再現不能な形)
2. **同骨数別 rig の誤ブレンド検出**: 骨数が同じ・名前が違う 2 rig 間で
   PoseLayout identity 不一致が正しくエラーになる(exact compatibility)
3. **loop 跨ぎ event/root**: multi-loop advance(dt が 2 周以上)で
   crossing の (source, ordinal) 順・loop index・root delta の合算が規範
   どおり。逆再生・seek 直後も
4. **N-way 順序不変**: 3+ clip の blend で入力順を入れ替えても
   規範順(sign 正準化・zero weight)で byte 一致
5. **hot reload stale handle**: registry 世代を進めた後の旧
   Rig/Clip/Cursor/Pose handle が全 API で stale エラー(部分成功なし)
6. **2 体並列**: 同一 rig の 2 instance を別 thread で評価し、arena
   非干渉・commit revision の instance 独立・palette 非混線
7. **クランプ端の再 emit 禁止**(WP97 が規範化した clamp 挙動の固定)
8. **capacity 境界**: crossing_capacity ちょうど / 1 少ない / 0 の三点で
   query→fill と cursor 原子性

受け入れ = 新 fixture 全 green + 既存全テスト + golden 全維持(SKIP 0)+
player(自己完結 project 可)。

### WP100: アセット HR1-T — テクスチャ差し替え(初の実 handler)

参照: **`design_asset_hot_reload.md` v2.1 §3-2 テクスチャ行・§7 HR1-T 行が
正**。基盤 = WP96(HR0 watcher)+ WP98(HR1 transaction — レポート
`docs/design_reviews/2026-07-12_wp98_report.md` §「HR1-T / HR1-M / HR2 の
接続点」の group API に**そのまま乗ること**。identity allocator/rollback/
barrier/status を個別再実装しない)。依存: WP96・WP98(済)。見積: 大。
排他: texture handler 新設 + `MaterialContainer` の replace 面追加
(`registerTexture` 系の既存挙動不変)。

1. **texture asset の watcher 接続**: 独立画像(png/EXR/KTX2)の
   AssetKey → ReloadRequest → transaction group(WP98 の reverse
   dependency で参照 material を巻き込む)
2. **同 shape 差し替え**: extent/format/mip 同一なら in-place
   re-upload(logical `GlobalTextureId` 不変・descriptor 無変更)
3. **shape/format/mip 変化**: image 再生成 + **reverse index で参照中の
   全 material descriptor set を再バインド**(descriptor は登録時
   一回書きのため逆引き必須 — MaterialContainer に rebind 面を新設)。
   旧 image は DeletionQueue(WP98 RetireSink 経由)
4. **失敗系**: 壊れた画像・未対応 format は candidate 破棄 + 旧絵継続 +
   `last_reload_error`。WP98 の rollback 不変条件(counts/IDs/bytes)を
   実 GPU resource でも fixture 化
5. **決定性 gate**: リプレイ/strict/rpc 中は HR0 gate で発火しない
   (書き換えても capture 不変の fixture)
6. fixture: 実 FS end-to-end(png 書き換え → capture 差分)1 本 +
   **1000 reload で texture/descriptor/GPU bytes が単調 leak しない** +
   KTX2 の mip/format 変化(WP92 fixture 流用)+ 同 GlobalTextureId
   維持 + in-flight フレーム跨ぎで crash なし
7. 受け入れ = 上記全 green + 既存全テスト + golden 全維持(SKIP 0)+
   player 8 秒(自己完結 project 可・可能なら実プロジェクトで F5 相当の
   手動確認手順もレポートに記載)

### WP101: アニメ A2 — `pelican.anim_graph` v1 + 標準評価器

参照: **`design_animation_graph.md` v2.1 §2(全部 — 特に §2-5 の A2 条件
逐語)・§3・§7 A2 行が正**。受入条件 = 敵対レビュー
`docs/design_reviews/2026-07-12_anim_v2_hotreload_review_codex.md` の
**§4.3(CA2-Interrupt)・§4.4(CA2-Clock)を逐語で満たすこと**。
依存: WP97(A1 — 済)・WP99(A1.5 — 済)。見積: 特大。
排他: anim_graph loader/schema(新設)+ 標準評価器
(`src/core/userpublic/` 側のユーザー空間システムとして — **エンジン
特権 API を使わないこと自体が受け入れ条件**。D0 と同じ dogfooding)。

1. **`pelican.anim_graph` v1 形式**: envelope schema/version・clip state・
   blend1d・crossfade・transition(priority / interrupt: "never|
   higher_priority|always" / duration 0 の緊急カット)・parameter
   (NaN/Inf 拒否・**単一 flat namespace と明記**)・v2 予約キー
   (`layers`/`events`/`graphs`/`sync`/`trigger` — 存在 = エラー)
2. **意味論 = §2 の確定事項 + §4.3/§4.4 逐語**:
   - 一 tick の transition decision 最大一回。
     `forceState(sequence) → priority → 宣言順` の一意順
   - interrupt snapshot: 旧 transition の当該 tick pose を一度評価し
     PoseLayout generation + frame revision 付きで snapshot 化。
     新 transition の alpha=0 出力は snapshot と byte 一致。
     snapshot は pose のみ(root delta/event 再 emit なし)。
     再 interrupt は chain せず一枚に materialize
   - `set_time`/replay seek/graph・model reload/layout mismatch の
     reset/reconstruct/error を明記・fixture 化
   - clock: shared normalized phase・length sync・clip speed(負含む)・
     start offset・loop endpoint・zero-duration clip・blend1d leader
     選択と同値 weight tie-break・同 tick exit→enter の re-enter reset
   - deterministic trace: state・cursor・transition progress・snapshot
     revision/layout・semantic pose hash(pointer/offset を含めない)を
     get_status 系で観測可能に
3. **標準評価器 = ユーザー空間**: A1 の公開機構語彙(凍結 ABI +
   phase 登録)だけで実装。`forceState(object, state)` をユーザー
   システム向けに公開
4. **AnimationSource/slot 調停(§3)**: 既存 clip component と graph の
   共存 — sink 単位の単一 writer + handoff。既存 clip component の
   挙動は不変(graph 未使用プロジェクトに影響ゼロ)
5. fixture: 遷移順序(priority/宣言順/force)・interrupt snapshot の
   byte 一致・0 秒カット・re-enter reset・NaN 拒否・v2 予約キー
   エラー・replay 2 回 byte 一致(WP89 経路)・2 体で別 graph 独立
6. example: 歩き↔走り blend1d + ジャンプ割込みの動くデモ(WASD 接続)
7. 受け入れ = §2-5 逐語条件 + 新 fixture 全 green + 既存全テスト +
   golden 全維持(SKIP 0)+ player(デモの手動確認手順をレポートに)

**【2026-07-12 追記】WP101 は実装前停止(正しい停止)** —
`docs/design_reviews/2026-07-12_wp101_report.md` のとおり、凍結 ApiV1 の
関数表 3 個ではユーザー空間評価器が成立しない(pose acquire・sample・
blend・phase 登録・sink handoff の公開入口なし)。**WP102(A1.1)を先行**
させ、WP101 は WP102 着地後に再派遣する(依存: WP102)。

### WP102: アニメ A1.1 — 公開 service surface(WP101 の前提)

参照: **`docs/design_reviews/2026-07-12_wp101_report.md`「不足している
公開入口」の列挙が要件の正**。設計判断 3 件は確定済み
(`design_animation_graph.md` v2.1 §7 A1.1 行):
①ApiV1 の **additive tail / versioned service table**(凍結 prefix
無変更・negotiation 規律どおり)②AnimationSource/sink authority =
**engine-owned registry**・ユーザー評価器は versioned handle で
claim/release(sink 単位 single-writer + handoff — 設計 §3)
③phase callback は **DLL unload 時に owner generation とともに自動失効**
(WP90 の owner 契約と同型)。
依存: WP94/97/99(済)。見積: 大。
排他: `src/core/userpublic/animation/` + `src/core/animation/` +
二世代 DLL fixture 拡張。**凍結済み prefix・既存 3 関数の挙動変更禁止**。

1. 公開関数(WP101 レポートの列挙を全部): sink/instance/rig/layout/
   clip 解決、pose arena frame 開始 + PoseViewV1 acquire、clip metadata
   取得・cursor 生成/破棄・point sample、normal N-way blend、
   local-to-model、skin palette 構築、phase callback 登録/解除
   (owner generation 付き)、source slot の claim/release/handoff、
   set_time/seek/reload/layout mismatch の通知面
2. ABI 規律: 全部 additive(struct_size/version/capability bits)。
   old client/new engine・new client/old engine の negotiation を
   二世代 DLL fixture に**拡張**(既存 fixture は不変で PASS のまま)
3. **gate = 第三者 game DLL から公開面だけで最小評価器
   (単一 clip 再生 + 2 clip blend + commit)を駆動する敵対 fixture**
   (内部 header include なしをビルドレベルで保証)
4. 受け入れ = 新 fixture 全 green + 既存全テスト(WP99 敵対 fixture
   含む)+ golden 全維持(SKIP 0)+ player

### WP103: 2D S2D-0a — スプライト契約と CPU 基盤(GPU なし)

参照: **`design_2d_game_layer.md` v2.1 §0-2・§1-1・§3・§5・§9 S2D-0a 行が
正**。受入条件 = 再レビュー
`docs/design_reviews/2026-07-12_2d_v2_rereview_codex.md` の
**C1(sampler)・C2(source ordinal)・C3(chunk 境界)を逐語で**。
確定判断: スプライト平面 = **XY + Z 法線固定**(2026-07-12 ユーザー
決定・§10-1)、sampler 正本 = asset 宣言側(§1-1)。
依存: K3 atlas(済)・U1(済 — 抽出元)。見積: 大。
排他: 2D 契約/CPU の新設ユニット + **AtlasAsset の consumer-neutral
抽出**(UiModule/UIContainer からのリファクタ — U1/U2 の全 fixture・
golden byte 不変が絶対条件)。GPU パスは実装しない(S2D-0b)。

1. **sprite_view コンポーネント**(§1-1): scene v1 `name` dispatch 準拠・
   asset 宣言 ID + `#sprite/` 参照・public struct + ref/validation・
   未知 field/範囲/finite/fragment missing の fixture。sampler は
   asset 宣言側(nearest|linear、既定 linear)— C1 のとおり
   per-sprite override なし・batch key に含む
2. **AtlasAsset 抽出**: atlas parser・GPU page upload・page lifetime を
   UiModule/UIContainer から consumer-neutral resource へ。UI と
   sprite の一方だけ有効でも他方を初期化しない(purge 規約)。
   **U1/U2 の既存 fixture・golden が byte 不変であること**
3. **SpriteCommand / world ABI**(§0-2): world transform・pivot・UV・
   color・page・sampler・layer・sort key・billboard。UI の
   QuadCommand とは別型(共有は index topology/chunk helper のみ)
4. **sort total key**(§3): layer → policy(z/y_down/declaration_seq)→
   EntityId full → **source-local stable ordinal(C2)**。finite 必須・
   canonical float key・`-0/+0`・EntityId 再利用 fixture
5. **visibility/caching/chunking**(§5): cull → sort → 16384 以下
   chunk 分割・static chunk cache。**C3 の境界 fixture(可視 16384/
   16385/2 chunk 超)** + 50k 論理/2k 可視 fixture(WP29 計測形式)
6. 受け入れ = C1/C2/C3 逐語 + 上記 fixture 全 green(全部 CPU —
   GPU 不要)+ 既存全テスト + golden 全維持(SKIP 0)+ player

### WP104: 2D S2D-0b — GPU world quad パス

参照: **`design_2d_game_layer.md` v2.1 §1-2・§4・§9 S2D-0b 行が正**。
基盤 = WP103(S2D-0a — レポート
`docs/design_reviews/2026-07-13_wp103_report.md`: SpriteCommand ABI・
sort/chunk・AtlasAssetResource が実装済み。**その出力をそのまま
GPU に流すこと** — CPU 側の再設計禁止)。依存: WP103(済)・
WP48 ortho(済)。見積: 大。
排他: sprite パス/シェーダ新設 + renderer への配線 +
sprite_view の ECS 反映 system。UI 経路の挙動変更禁止。

1. **world-space 頂点/インスタンス buffer**: SpriteCommand(world
   transform・pivot・UV・color・flip)→ GPU。camera VP は FrameUBO。
   billboard("y_axis"/"full")は view 依存のため描画側で展開
2. **sprite パス**: 3D 不透明の後・post_main の前の固定位置。
   depth test ON / depth write OFF。blend は UI と同じ straight alpha
   値。色 ABI = linear 出力(色パイプライン v4 準拠)
3. **atlas page bind**: WP103 の AtlasAssetResource の shared page を
   sprite consumer から参照(UI 無効でも sprite 単独で初期化できる —
   purge 双方向を fixture 化)
4. **sprite_view → 描画の ECS 接続**: Transform/parent 追従・
   place/remove・visibility(§5 の cull は CPU 済み)
5. golden: ①ortho + atlas スプライト複数(layer 跨ぎ・flip・tint)
   ②3D ジオメトリとの遮蔽(depth test)③複数 atlas page ④回転/親子
   ⑤UI あり/なしの resource ownership。**件数 REQUIRE 24/23 を
   追加分だけ更新**
6. 受け入れ = 新 golden + 既存全テスト + 既存 golden 全維持(SKIP 0)+
   player 8 秒(スプライトが表示される自己完結 demo project 付き)

### WP105: アセット HR1-M — material values 差し替え

参照: **`design_asset_hot_reload.md` v2.1 §3-2 values 行・§7 HR1-M 行が
正**。受入条件 = 再レビュー同 §HR-C4-3: **単体 gate は same-layout
update + fake dependency actor まで。実 `.surface + .material.json` の
cross-file atomic fixture は HR2-S の exit gate(本 WP に含めない)**。
基盤 = WP98 group API + WP100 texture handler(実装参考)。
依存: WP96/98/100(済)。見積: 中。
排他: values handler 新設 + MaterialContainer の SSBO update 面
(現行は登録時一回書き — update API を新設、既存 register 挙動不変)。

1. `.material.json` の AssetKey → watcher 接続 → 再 parse/lower
   (WP76 バインダ再実行)
2. **同 layout なら SSBO 値の update のみ**(descriptor/pipeline 不変・
   GlobalMaterialId 不変)。surface layout が変わる場合は本 WP では
   **旧 values 継続 + 名前入り WARN**(transaction group 化は HR2-S)
3. 失敗系: 壊れた JSON・型不一致・範囲外 = candidate 破棄・旧値継続・
   last_reload_error
4. fixture: WP76 layout fixture 再利用・実 FS end-to-end 1 本
   (values 書き換え → capture 差分)・二体/material 単位更新
   (1 material の変更が他 material に波及しない)・1000 reload leak
   なし・リプレイ中不発火
5. 受け入れ = 上記全 green + 既存全テスト + golden 全維持
   (SKIP 0・件数 29/28)+ player 8 秒

### WP106: 2D S2D-1 — strict pixel policy + flipbook dogfood

参照: **`design_2d_game_layer.md` §1-1・§2・§3・§8・§9 S2D-1 行が正**。
受入条件 = 再レビュー C4 の **成立式・strict 対象集合表・適用順・一項
negative fixture** をすべて閉じること。基盤 = WP103/104(済)。見積: 大。
排他: camera sprite policy・SpriteScene/SpriteRenderer の render-only
quantization・公開 flipbook helper。物理 Transform と UI quad ABI は変更禁止。

1. **方式選定**: `render_only_quantization` に固定。project の
   `basic_config.sprite.pixels_per_unit` と camera 単位の
   `sprite.pixel_perfect = off|strict` / `sprite.sort = z|y_down|declaration`
   を closed schema で追加する
2. **C4 成立式**: content viewport = letterbox を持たない framebuffer 全域。
   `wupp=(2*xmag/width,2*ymag/height)`、
   `zoom=(1/ppu)/wupp`。x/y が同じ 1 以上の整数(相対許容差 `1e-4`)のとき
   だけ strict active。pixel center = `n+0.5`、境界 = 整数。xmag / ymag /
   viewport / ppu を一項だけ壊す CPU negative fixture を置く
3. **strict 対象集合**: nearest + orthographic + view 軸に平行 + source texel
   ごとの最終 framebuffer scale が各軸で整数、を必要十分条件とする。
   size 省略/明示、整数・非一様整数 scale は対象。fractional effective
   scale、回転/傾き、billboard、linear sampler は理由付き downgrade。
   camera 契約不成立も silent fallback せず名前入り WARN + status
4. **適用順**: Transform は不変のまま world→view→projection 後、local atlas
   corner 1 点だけを framebuffer 整数境界へ丸め、同じ clip-space delta を
   quad 全頂点へ一度だけ加える。camera snap と二重丸めは行わない
5. **観測と policy**: ppu / zoom / viewport / sort / chunk / eligible / downgrade
   理由を `get_status.sprite`、`get_frame_plan.sprite`、sprite draw trace へ additive
   公開。y_down/declaration と billboard の GPU/CPU fixture を閉じる
6. **Flipbook dogfood**: 特権のない公開 `FlipbookClip` を追加。明示 local time
   から deterministic に frame を選び、公開 `GameContext` の sprite 作成・
   texture 差し替えだけで動く project code demo を付ける
7. **golden**: zoom 1/2/3、odd/even viewport/texture、fractional camera/sprite、
   atlas edge、非一様整数/fractional scale、rotation、billboard を4件で覆う。
   件数 REQUIRE は VAT 有効 33 / 無効 32。既存 `sprite_ortho_atlas` が露呈した
   Vulkan depth 変換も RH/ZO に修正し、near=0 / far=1 を CPU fixture 化
8. 受け入れ = C4 fixture + 新規 golden/status assertions + 公開 project code
   build + 既存全テスト + golden SKIP 0 + player 8 秒

### WP107: 2D S2D-P — shapeCast query minimum + Provider ABI V2

参照: **`design_2d_game_layer.md` v2.3 §7・§9 S2D-P 行が正**。
受入条件 = 再レビュー C5 の **public 入出力・initial overlap/zero delta・
all-hit 全順序・MTD 非一意軸・collider/filter schema** を同じ WP で閉じる。
基盤 = P1/P2 と PhysQuery foundation refactor(済)。物理シミュレーション、
`moveAndSlide`、方向付き one-way policy は S2D-2 へ残す。

1. **pure shapeCast**: 固定姿勢の平行 sweep。sphere/box/capsule 全 9 組、
   thin collider、TOI/position/normal、zero-delta MTD、touching/non-finite を
   CPU fixture 化
2. **決定性**: TOI `1e-5` bucket + stable collider/full entity/shape ordinal。
   MTD tie は移動逆向き優先後に world x/y/z で全順序化し、closest と filter
   継続は ordered all-hit の先頭から導出
3. **schema**: collider に uint32 `layer/mask`、bool `trigger/one_way` と
   安定既定値を追加。self/ignore、stale full identity、未知 bit、ignore 後の
   次 hit を positive/negative fixture 化
4. **ABI V2**: V1 ABI を不変のまま `ApiV2/ServiceV2/ProviderV2` と
   `query_shape_cast_all` を additive 追加。descriptor/result の境界検証、
   malformed provider containment、V1 provider の capability 単位 fallback
5. **backend/purge**: Builtin と Jolt を同じ Provider V2 へ実装。
   `PELICAN_WITH_PHYSICS=OFF`、provider-only、Jolt の 3 構成と公開 header だけを
   include する V1/V2 provider DLL の load/rollback/unload を通す
6. **公開接続**: PhysWorld/GameContext に all/closest を追加し、physics-off は
   空結果。game DLL link fixture で新旧 API を同時参照
7. 受け入れ = pure/PhysWorld/service fixture + 3 構成 build/CTest + provider DLL
   E2E + public API link + `git diff --check`

### WP108: アセット HR2-S — shader/surface dependency transaction

参照: **`design_asset_hot_reload.md` v2.1 §3-2・§4・§7 HR2-S 行が正**。
受入条件 = 再レビュー HR-C4-2/3 の **ShaderLibrary 時刻 poll 撤去・単一
FileWatcher 経路・実 `.surface + .material.json` cross-file atomic fixture**。
基盤 = WP82/96/98/105(済)。状態: **完了(2026-07-15)**。

1. `ShaderCompileResult` に source graph の物理 dependency を保持し、
   `ShaderLibrary` で project/mounted-store の `AssetKey` へ変換する。
   root/include/.surface から reload unit と全 define/pass variant を逆引きする
2. 1 秒 mtime poll を撤去し、`ReloadService` の `pelican.shaders` を file-backed
   participant 化。同一 watcher frame の shader/material request を一つの batch
   result にまとめ、成功した digest だけを commit する
3. 全 affected bundle を prepare した後、`PipelineFactory` が依存 pipeline を
   全 candidate 構築する。途中失敗は candidate のみ破棄し、旧 bundle/pipeline
   を維持。成功時の旧 pipeline/layout は `DeletionQueue` へ送る
4. `.surface` parameter layout 変更時は依存 `.material.json` を再 parse/lowerし、
   shader/pipeline/material values と registry compatibility revision を同じ
   publication で更新する。peer のどれかが不正なら全体を rollbackする
5. status に watcher source、追跡 unit/bundle/dependency、cache hit/miss を公開。
   renderer は render-start で commit 通知だけを consumeして fullscreen descriptor
   を rebindする
6. fixture: root/include/.surface 全 variant、cache hit/miss、compile/material failure
   rollback、cross-file layout update、in-flight old pipeline、batch digest retry、
   runtime architecture。詳細は `docs/design_reviews/2026-07-15_wp108_report.md`

### WP109: 2D S2D-2 — side-scroller vertical slice

参照: **`design_2d_game_layer.md` v2.4 §7-2・§9 S2D-2 行が正**。
受入条件 = **入力/event/fixed-step/接地/斜面/one-way を同じ side-scroller
1 面で接続し、replay 2 回一致・tunneling を fixture で閉じる**。
基盤 = WP106(S2D-1) / WP107(S2D-P)。状態: **完了(2026-07-16)**。

1. **非特権 helper**: `src/core/userpublic/platformer/` に
   `moveAndSlide` を追加。公開 `Shape` / `ShapeCastQueryHit` / `QueryFilter`
   だけを入出力とし、`GameContext` adapter と ordered all-hit callback 注入の
   2 経路を持つ。Builtin/Jolt/game DLL provider や独自 query source を交換しても
   slope/one-way policy は同じで、ゲームから helper 自体を不使用・差分実装できる
2. **controller policy**: XY 平面、連続 sweep、初期 overlap の決定的回復、
   wall slide、ceiling、最大斜面角、急斜面の壁扱い、trigger 除外を実装。
   one-way は metadata + 接近方向 + 開始 support point で上からだけ接地し、
   `collide_with_one_way=false` で drop-through する
3. **決定性**: helper は provider の canonical all-hit 順を保ち、固定 iteration・
   固定 epsilon で処理。240 fixed-step の同一入力を 2 回再生し、player pose の
   float bit trace と着地 event 列が完全一致することを fixture 化
4. **縦切り**: `projects/sprite_demo` を player capsule、床、斜面、one-way、壁、
   A/D・Space・S 入力、flipbook、`PlayerLanded` event を備えた実行可能デモへ更新。
   player dynamics は project code に置き、剛体シミュレーションへ依存しない
5. **公開 DLL gate**: game DLL から custom event を `emit` できるよう event
   registerer の必要 symbol を exportし、public API link fixture で回帰を防ぐ
6. 将来へ残すもの = step-up/coyote time/moving platform/TileMap/物理 trigger event/
   剛体・GPU simulation。これらを controller 最小契約へ先取りしない
7. 詳細と検証記録は `docs/design_reviews/2026-07-16_wp109_report.md`

### WP110: アセット HR2-G — model/fragment hot reload

参照: **`design_asset_hot_reload.md` v2.1 §3-2・§4・§7 HR2-G 行が正**。
受入条件 = **同一 container の全 fragment を一括 prepare/stage/publishし、
複数 live instance の identity/Transform/physics を維持したまま描画と skinning を
差し替える。失敗時は全世代を維持し、rig 変化時は animation/temporal state を
reset、1000 回 reload 後も資源量が増加しないこと**。
基盤 = WP77/98/105(済)。状態: **完了(2026-07-16)**。

1. `ModelAssetId`、content revision、rig compatibility revision を分離し、物理
   container から全 fragment template への reverse index を `ResourceRegistry` に登録
2. glTF を CPU prepare + side-effect-free inspect した後、texture/material/geometry を
   ownership 付き candidate として stage。同一 container の全 template が成功した時だけ
   一括 publishし、途中の parse/validate/GPU capacity failure は全 candidate を破棄
3. `PolygonInstanceContainer` は asset identity で全 live instance を bulk rebuild。
   `ModelInstanceId` と current Transform は維持し、previous=current、history invalid、
   animation revision reset + generation advance とする。ECS/physics は変更しない
4. index/static/skinned mega-buffer を free-range suballocator 化し、material/texture ID と
   旧 Vulkan object を `DeletionQueue` で retire。material SSBO slot と geometry range は
   in-flight 安全期間後まで再利用しない
5. `ReloadService` の `pelican.models` participant を単一 FileWatcher 経路へ接続。
   project/store の canonical container key を依存として公開する
6. fixture: 同一 GLB の複数 fragment + 3 instance、rig 変更、invalid CPU rollback、
   2 actor 目の GPU stage failure rollback、in-flight material slot、1000 reload。
   詳細は `docs/design_reviews/2026-07-16_wp110_report.md`

### WP111: VRM-S0 — `.vrm` semantic decoder(保持と検証のみ)

参照: **`design_animation_graph.md` v2.1 §4 が正**。受入条件 = 敵対レビュー
`docs/design_reviews/2026-07-12_anim_v2_hotreload_review_codex.md` **§4.5 の
VRM-S0 行を逐語で**(gate: optional/unknown/duplicate bone・node index・
round-trip dump fixture)。依存: WP38(済)。見積: 中〜大。
排他: VRM semantic の decode/storage 新設(`src/core/model/` 配下)+
devcli dump コマンド。**renderer・material・per-instance 適用は一切
実装しない(VRM-S1 の仕事)**。

**確定判断(2026-07-16)**: ランタイムがデコードするのは **VRM 1.0
(`VRMC_vrm` 拡張)のみ** — 「ランタイムは v1 だけを読む」規則の適用。
VRM 0.x(example の AliciaSolid.vrm 含む)は従来どおり素の GLB として
ロードし、「VRM 0.x semantic は未対応(変換は import-tools 予定)」の
INFO 1 行のみ。0.x の互換受理コードを足すことは §0 違反とする。

1. **decode/storage**: `VRMC_vrm` の specVersion 検証(未対応 version は
   名前入り WARN + semantic なしで続行 — モデル自体は描ける)、
   humanoid bone map(必須骨の欠落検出・optional 骨・**未知骨名は保持
   したまま WARN**・重複 bone→node は エラー)、expressions
   (preset/custom・morphTargetBinds・materialColorBinds/
   textureTransformBinds は**参照の保持のみ**)、lookAt(type・
   rangeMap)、firstPerson・constraint(`VRMC_node_constraint`)は
   **metadata として保持のみ**
2. **storage の形**: `SkeletalModelData` と並ぶ `VrmSemanticData`
   (immutable・shared_ptr)。node index は glTF node 空間で保持し、
   rig 側(AnimationRig)への写像 API を用意(A1 の node 名保存を活用)
3. **devcli `vrm dump`**: semantic を canonical JSON へ dump(键順固定・
   決定的)。round-trip fixture の正本
4. fixture: 決定的な最小 VRM 1.0 バイナリをテスト内ライタで生成
   (KTX2 の流儀)— 正常系 / optional 骨欠落 / 未知骨名 / 重複 bone /
   specVersion 未対応 / VRMC_vrm なし(素の GLB)/ VRM 0.x(AliciaSolid
   実物 — semantic なし + INFO を確認)
5. 受け入れ = round-trip dump fixture + 上記全 fixture + 既存全テスト +
   golden 全維持(SKIP 0・件数 29/28)+ player 8 秒(example —
   AliciaSolid の挙動不変)

### WP112: J1 — projection jitter 機構 + feature named binding

参照: **`design_taa_jitter.md` v2.1 §1 全部が正**(再レビュー条件
TAA-C1〜C4 反映済み — 各節の「逐語添付条件」ブロックが受入条件)。
TAA feature 本体(§2)は対象外(T-TAA = 次 WP)。
依存: WP88/95(済)。見積: 大。
排他: featurecompose(jitter schema + J1-BINDING)・FrameUBO(ABI
gate 更新込み)・materialrender(engineMvp 1 行)・velocity.frag・
renderer の snapshot/epoch 配線・debug enqueue 引数。

1. **系列**(§1-2): `sample_index = ((frame_index-1) % phases) + 1`・
   frame 0 は名前入り error・phases=8 の規範数表 fixture・
   width/height=0 error
2. **投影式**(§1-3): 一般式(全列 row0/1 += jitter·row3)。
   perspective/ortho の複数 z で NDC delta 一定の CPU 単体テスト・
   jy 正方向 = framebuffer 下向きの三者同符号 fixture
3. **配送**(§1-4 consumer 表どおり): 主 material = jittered VP push・
   velocity/sprite/SSAO = FrameUBO(jittered)・world debug = snapshot
   の jittered VP を enqueue 引数で・shadow/culling/rpc = 不変。
   consumer 列挙 manifest + provider on で SSAO/sprite/debug の輪郭が
   主 material と同 offset の fixture
4. **FrameUBO ABI**(§1-5): jitter_ndc ペア + epoch ペアを additive
   追加・CPU/GLSL std140 同時更新・offsetof/sizeof gate。
   velocity.frag で NDC 減算 → *0.5(数値 fixture 6 種)
5. **reset**(§1-6): epoch ペア・統一 invalidation 経路・truth table
   fixture(初回/resize/set_time/camera cut/feature enable)
6. **J1-BINDING**(§2 の独立 sub-gate): feature instance
   `{ref, parameters}` 構文 + parameter 宣言 + 検証(feature 名・
   parameter 名・target 名入り reject)+ 二重 bind fixture +
   compose result / frame_plan への追加
7. 受け入れ = §1-7 の三分割 gate(off = 既存 golden byte 一致 /
   jitter-only = 数表 golden + 2 回一致 / TAA on は T-TAA へ)+
   既存全テスト + golden 全維持(SKIP 0・件数 33/32)+ player 8 秒

### WP113: T-TAA — 標準 TAA feature(ユーザー空間 stdlib)

参照: **`design_taa_jitter.md` v2.1 §2(全部 — 二パス構成・
TAA-RESOLVE-EQUATIONS の全式)・§2-1・§3 が正**。
基盤 = WP112(J1 — レポート
`docs/design_reviews/2026-07-16_wp112_report.md`: jitter 機構・epoch
ペア・named binding が実装済み。**この公開機構だけで書くこと** —
エンジン本体への変更は原則禁止・不足があれば実装せず質問 = A2 と
同じ dogfooding の掟)。依存: WP112(済)。見積: 中〜大。
排他: `engine://features/taa.json` + resolve/composite シェーダ +
golden + adding_features.md レシピ。

1. **二パス構成**(§2): taa_resolve(4 入力 → taa_accum 1 出力)+
   taa_composite(taa_accum → downstream 1 出力)。named binding で
   RT 名を解決(プロジェクト固有名の直書き禁止)
2. **resolve の全式 = §2 の TAA-RESOLVE-EQUATIONS 逐語**(履歴参照・
   深度線形化・disocclusion・clamp・blend・invalid 素通し)。
   α と τ・ε は feature params 公開
3. depth は feature override で SAMPLED usage 追加(既存機構)
4. **golden(§2-1 — 各別 gate)**: 既定 off 全維持 / jitter-only(J1
   済)/ 静止 N フレーム収束 / camera motion / object motion /
   disocclusion / resize 後 1 フレーム / set_time 後 1 フレーム /
   orthographic / 2 回実行 byte 一致
5. example config への compose/validate が通る完全な taa.json fixture +
   adding_features.md に「コピーして改造する」ユーザーレシピ
6. 受け入れ = 上記全部 + 既存全テスト + golden 全維持(SKIP 0・
   件数 33/32 は新規追加分だけ更新)+ player 8 秒(TAA on の example
   相当プロジェクトの見た目確認手順をレポートに)

**【2026-07-16 追記】WP113 は実装前停止(正しい停止)** — J1 の
parameter は RT 名のみでスカラー(α/τ/ε)を配送できない
(`docs/design_reviews/2026-07-16_wp113_report.md`)。**WP114(J1b)を
先行**させ、WP113 は WP114 着地後に再派遣(依存: WP114)。

### WP114: J1b — feature スカラー parameter → shader defines

参照: **`design_taa_jitter.md` v2.1 §2 の J1b ブロックが正**。
設計判断確定: instance `parameters` の数値(float/int/bool)を
**per-feature shader defines へ lower**(既存 defines 合成レーン =
WP28、cache キー = WP82 に自然に乗る。UBO/ABI 変更なし)。
依存: WP112(済)。見積: 小〜中。
排他: featurecompose(parameter 宣言 + lowering)+ fixture。

1. feature 側 parameter 宣言に `type`(float/int/bool)・`range`・
   `default` を追加(RT parameter と同じ宣言表に同居)
2. instance `parameters` の数値を検証(型・range・未知名 — feature 名・
   parameter 名入り reject)し、その feature のシェーダ compile への
   defines として lower(命名規範: `PELICAN_FEATURE_<NAME>_<PARAM>`)
3. compose result / get_frame_plan に解決済み値を出す(観測点)
4. fixture: 宣言/検証/lower の正負・同 feature 二重 bind で別値・
   defines が cache キーに効く(値変更で再コンパイル)・既存 feature
   無変更(golden 全維持)
5. 受け入れ = fixture 全 green + 既存全テスト + golden 全維持
   (SKIP 0・33/32)+ player 8 秒

### WP115: J1c — ジッタ系列のユーザー定義(pattern: "table")

参照: **`design_taa_jitter.md` v2.1 §1-1 の J1c ブロックが正**
(2026-07-16 ユーザー指摘 — 系列こそ発展する側でありユーザー空間に
開く。「feature 層 = ユーザー空間」方針の適用)。
依存: WP112(済)。見積: 小。
排他: featurecompose の projection_jitter 検証 + 系列評価 + fixture。

1. `pattern: "table"` + `offsets_px` 配列(宣言 JSON 直書き)。
   phases = 配列長(明示併記は一致必須)
2. 検証: 各値 [-0.5,0.5)・長さ 1..64・非数値/NaN は feature 名入り
   エラー。frame_plan / compose result への出力は既存形式に table 分を
   additive 追加
3. **halton23 と同値の table を書いた場合の byte 一致 fixture**
   (名前付きパターン = 特権なしの証明)
4. sample_index 式・frame 0 エラー・wrap は WP112 の規範をそのまま
   共有(重複実装禁止 — 系列取得だけ差し替え)
5. **adding_features.md に「temporal 系のユーザー管理枠」節を追加**
   (`design_taa_jitter.md` §0-1 の境界表をユーザー向けの言葉で転載 —
   何をコピーして改造してよいか・エンジン語彙 6 項に当たったら
   語彙追加を依頼する、の手順込み)
6. 受け入れ = fixture 全 green + 既存全テスト + golden 全維持
   (SKIP 0・33/32 — T-TAA が先に着地していた場合はその件数)+
   player 8 秒

### WP116: M-PBR0a — OpenPBR 表現/routing ABI

参照: **`design_usd_openpbr.md` v2.1 §1-1 が正**(再レビュー条件
USD-C1 = M-PBR0a-STATE 反映済み)。openpbr surface 本体・写像表は
M-PBR0b(次 WP)— 本 WP は **ABI の新設のみ**。
依存: M3a(済)・WP105(済)。見積: 特大。
排他: materialformat/surfaceformat/materiallowering(additive)・
scene loader / model template の binding 消費経路・dump-lowered。

1. **OpenPBR 対応 input の置き場表**(§1-1-1): 各 input →
   surface param / custom texture / render state / lighting の 1 表
   (`doubleSided` を含む — USD-C1)
2. **shading 経路 = lighting hook + forward**(§1-1-2 確定済み。
   PelicanSurfaceV1 は不変・deferred は将来の PelicanSurfaceV2 で)
3. **per-material custom texture override**(§1-1-3): pelican.material
   の additive key(現行 parser は top-level textures を reject —
   これを宣言済み surface texture の差し替えに限り解禁)+ lowering +
   binder。未宣言 texture 名・型不一致は名前入りエラー
4. **alpha/doubleSided routing**(§1-1-4 = M-PBR0a-STATE 逐語):
   `{opaque, mask, blend} × {single_sided, double_sided}` の最大六
   surface variant へ決定的 route。MASK = opaque 系 + cutoff discard・
   BLEND = blend/depth-read-only。gate: 各組合せの pipeline state
   dump・front/back view・cutoff 境界・binding golden
5. **material → GLB primitive binding ABI**(§1-1-5): import が出す
   `USD prim/subset path → GLB mesh/primitive index → material 名`
   mapping を scene loader / model template が消費し primitive 単位で
   material を差し替え。collision/missing/duplicate/fragment は
   名前入り hard error
6. 受け入れ = dump-lowered-material に新 ABI 追加 + schema error
   fixture + **既存 material の golden 全 byte 維持**(新 ABI は
   additive)+ 既存全テスト + golden SKIP 0(件数 40/39)+
   player 8 秒

### WP117: M-PBR0b — openpbr surface + 写像表

参照: **`design_usd_openpbr.md` v2.1 §1-2 が正**(再レビュー条件
USD-C2 = M-PBR0b-VARIANTS の wrapper-B 形式を含む)。
基盤 = WP116(M-PBR0a — レポート
`docs/design_reviews/2026-07-17_wp116_report.md` の ABI にそのまま
乗ること。ABI の変更・再実装は禁止 — 不足があれば実装せず質問)。
依存: WP116(済)。見積: 大。
排他: openpbr surface wrapper 群 + engine GLSL include + 写像表
文書/fixture + example マテリアル + golden。

1. **openpbr surface(wrapper-B 形式 — USD-C2 逐語)**: 薄い
   `.surface` wrapper ×6(`{opaque,mask,blend} × {single,double}`)+
   lighting 実装は**単一の登録済み engine GLSL include** に集約。
   全 wrapper の params/textures の名前・順序・型・default・
   colorspace が同一で差分が render_state/cutoff/cull だけであることを
   **parser fixture で比較**。六 variant の shader/pipeline cache 列挙
2. **公開 lighting ライブラリのみで実装**(standard/toon と同じ制約 =
   特権なし)。forward 経路(WP116 の routing どおり)
3. **v1 サブセットの数表規範**: base(weight/color/metalness/
   diffuse_roughness)・specular(weight/color/roughness/IOR)・
   emission・coat(1 層)・opacity/normal/alpha の unit・range・
   default・colorspace・channel を表で固定。**OpenPBR 1.1.1 exact pin**
   (仕様 tag/hash を文書に記録)
4. **写像表**: UsdPreviewSurface / MaterialX OpenPBR(allowlist)/
   glTF KHR_materials_* → 上記パラメータの対応表(文書 + 数値
   fixture — import-tools 側の実装は U-USD0c)
5. **未対応 WARN 規範**: authored 非 default または寄与する connection
   のみ WARN(code・prim path・input 名・fallback 値を machine-testable
   に)— 本 WP では表と runtime 側の WARN 面だけ(USD 入力は 0c)
6. **dogfooding**: example に OpenPBR サンプル球(coat の効きが golden
   で見える)
7. 受け入れ = base/specular/IOR/coat/emission/normal/alpha の数値・
   画像 golden + **既存 material golden 全 byte 維持** + 既存全テスト +
   golden SKIP 0(件数 40/39 から追加分更新)+ player 8 秒

### WP118: U-USD0a — USD ツール選定 spike(production レシピと分離)

参照: **`design_usd_openpbr.md` v2.1 §2-1 が正**(guc は削除済み —
候補は usd-core 自前 / Blender headless lossy fallback の 2 系)。
依存: なし(エンジンコード変更なし)。見積: 中。
排他: **エンジン(pelican2)への変更は docs のレポート 1 本のみ**。
probe スクリプトと USD corpus は `%USERPROFILE%\Documents\
pelican-import-tools` の `spike/usd/` 配下(main 直コミット運用)。

1. **固定 USD corpus の作成**(usd-core で決定的に生成・コミット):
   静的メッシュ+階層 / UsdGeomSubset / primvar 各補間 /
   UsdPreviewSurface / MaterialX OpenPBR ノード / variant 付き /
   usdz / Y-up・Z-up・単位違い
2. **usd-core wheel の probe**(Windows): exact version pin・
   **UsdMtlx plugin/data が wheel に同梱されるか**・resolver/検索 path・
   ライセンスと再配布形態
3. **Blender headless の probe**: USD import の lossy 範囲の実測
   (PreviewSurface のみ? MaterialX は?)
4. **比較レポート**(§2-1 の完全比較軸: 方向・保守・license・
   Windows CI 導入・version/hash・resolver/plugin・variant/payload・
   subset/binding・MaterialX 忠実度・UDIM・colorspace・texture
   transform・primvar・triangulation・normals/tangents・negative
   determinant・決定的直列化・診断・性能)→ **採用案の結論**
5. 受け入れ = レポート
   `docs/design_reviews/2026-07-17_wp118_usd_spike.md`(採用
   tool/version/hash/license/導入手順・corpus の場所と再生成手順)+
   import-tools 側 spike の pytest(probe が再実行可能)。
   エンジンのテスト・golden には触れない

### WP119: U-USD0b — コンポジションと静的ジオメトリ変換

参照: **`design_usd_openpbr.md` v2.1 §2-2(U-USD0b-GEOMETRY 込み)+
§3 共通 contract(MANIFEST/DETERMINISM/NORMALIZATION/SAFETY)が正**。
採用レーン = **WP118 の結論(usd-core==26.5・レポート
`docs/design_reviews/2026-07-17_wp118_usd_spike.md` が toolchain の正 —
version/hash/警告方針を引き継ぐこと)**。
material は **default standard へ落とす**(OpenPBR/binding は U-USD0c)。
依存: WP118(済)・K3。見積: 大。
排他: import-tools の `usd` レシピ本体(spike とは別の production
コード)+ pelican2 側は「生成物のロード fixture + 小 fixture 資産」のみ。

1. **レシピ**: USD/USDZ → glb(+`#fragment`)+ scene v1 断片。
   variant/payload/resolver/localization をレシピ引数 + manifest 記録。
   USDZ は安全展開(SAFETY 逐語: package API or 安全展開器 +
   traversal/duplicate/bomb fixture)
2. **正規化契約(NORMALIZATION 逐語)**: upAxis/metersPerUnit(欠損
   既定込み)・適用 basis/scale・xform 方針・negative determinant の
   winding/normal 反転・全 metadata key/型を manifest schema に固定
3. **ジオメトリ規範(U-USD0b-GEOMETRY 逐語)**: points/faceVertex*・
   orientation・holes・subdivision policy・UsdGeomSubset family/type・
   primvar 全補間 + indexed・triangulation・normal/tangent 生成/保持・
   UV set。**prim/subset path → GLB mesh/primitive index の mapping が
   決定的 sort 後にも一致する fixture**
4. **決定性(DETERMINISM 逐語)**: 同一 closure + toolchain で全出力
   SHA-256 二回一致。`source.toolchain[]` に全 tool/package の
   name/version/hash・レシピ引数を記録(MANIFEST)
5. エンジン側 gate: 生成した glb + scene 断片(corpus 数点分を
   fixture としてコミット)を実ロードして描く fixture + golden 1 枚
6. 受け入れ = import-tools pytest(レシピ・安全展開・決定性)+
   pelican2 側ロード fixture + 既存全テスト + golden SKIP 0
   (件数 41/40 から追加分更新)+ player 8 秒

### WP120: VRM-S1 — per-instance application sink(expression/gaze)

参照: **`design_animation_graph.md` v2.1 §4 + 敵対レビュー
`docs/design_reviews/2026-07-12_anim_v2_hotreload_review_codex.md`
§4.5 の VRM-S1 行が正**(gate: 二体で別 expression・同 frame
revision・golden。renderer/material の per-instance 化を含む大物)。
基盤 = WP111(VrmSemanticData)・WP102(AnimationServiceV1 phase)・
WP116(material ABI)。依存: 全て済。見積: 特大。
排他: renderer の per-instance morph/expression 適用 + phase 接続。

**【重要・最初に確認】現行レンダラに glTF morph target(blend shape)
の描画機構があるか監査すること。なければ実装せず、VRM-S1a(morph
target 描画機構)/ VRM-S1b(expression sink)の分割案を質問として
レポートに書いて停止**(A1.1/J1b と同じ手順)。

1. morph target weights の per-instance 適用(expression の
   morphTargetBinds 消費)
2. material color / texture transform binds の per-instance 適用
   (WP116 の override ABI を per-instance に拡張 — 必要なら停止質問)
3. lookAt/gaze の bone/expression 両 type・firstPerson mesh annotation
4. §1-4 phase への写像(humanoid→lookAt→expression→constraint の
   実行順)— AnimationServiceV1 の公開 phase に登録
5. gate: 二体で別 expression・同 frame revision(§1-5 の N/N-1 と
   整合)・golden(expression on/off)・既存全テスト + golden 維持
6. 受け入れ = 上記 + player 8 秒(VRM 1.0 fixture で expression が
   動く手動確認手順)

**【2026-07-17 追記】WP120 は監査停止(正しい停止)** — morph target
描画機構が全経路で不在(`docs/design_reviews/2026-07-17_wp120_report.md`
の監査表が正)。停止質問への確定判断: ①S1a→S1b 分割承認
②公開面は **versioned application service の additive 追加**(標準
評価器だけの engine 内部例外は dogfooding 違反 — 不採用。S1a の
commit 面はレポート案どおり renderer 内部で閉じ、公開は S1b で)
③**MaterialInstanceOverrideBlock は独立 WP(WP122)に分離**。
実行順: WP121(S1a)→ WP122(M-INST0)→ WP123(S1b)。

### WP121: VRM-S1a — glTF morph target 描画機構

参照: **`docs/design_reviews/2026-07-17_wp120_report.md` の
「VRM-S1a」節(6 項)と「必要な機構語彙」の morph 系 5 語彙
(MorphTargetLayout / DeltaRange / WeightFrame /
publishMorphWeightFrame / advanceMorphHistoryAfterRender)が仕様の正**。
VRM semantic は含まない汎用 glTF 機構。
依存: WP110(モデル reload の identity)・WP95(N/N-1 流儀)。
見積: 特大。排他: gltf decode(targets/weights)・vertbufcontainer
(delta storage)・polygoninstancecontainer(weight frame)・
vertex shader 群(material/shadow/velocity)・fixture/golden。

1. decode: `primitive.targets`(POSITION/NORMAL/TANGENT)+
   `mesh.weights`/`node.weights` 既定。count/type/有限値/sparse 方針を
   名前入り error で固定
2. storage: immutable delta の共有 GPU range + 決定的 layout
   (model generation 付き — WP110 の reload と整合)
3. instance weight frame: 二体独立・current N / previous N-1・
   初回/seek/reload/teleport で previous=current(§1-5 と同型)。
   commit 面は renderer 内部の versioned descriptor(公開は S1b)
4. 変形: `base + Σ(weight × delta)` を **skinning より前**に、
   material/shadow/velocity の全 vertex path で同一に。velocity は
   weight N と N-1 を読む(骨のみ/morph のみ運動の velocity fixture)
5. **target なしモデルの vertex ABI・batching・golden を byte/image
   不変に**。target 数上限・weight buffer capacity・overflow policy を
   明示
6. 受け入れ = morph off/on・二 instance 別 weight 同 revision・
   skinned morph・shadow・deformation velocity・reload/reset の
   fixture + image golden + 既存全テスト + golden SKIP 0
   (件数 41/40 から追加分更新)+ player 8 秒

### WP124: U-USD0c — USD マテリアル/テクスチャ/binding 変換

参照: **`design_usd_openpbr.md` v2.1 §2-3 + §3 共通 contract が正**。
基盤 = WP117(openpbr surface + 写像表)・WP119(usd レシピ +
prim→primitive mapping)・WP116(binding ABI)— 全て済。
**WP118 の縮退確定を引き継ぐ: UsdMtlx 非同梱 → MaterialX は
authored 値のみ**(external .mtlx graph は reject + WARN)。
依存: WP117/119(済)。見積: 大。
排他: import-tools の usd レシピ拡張 + pelican2 は「manifest への
`pelican.material` schema 受理(additive・fixture 付き)+ ロード
fixture/golden」のみ。

1. UsdPreviewSurface → WP117 写像表 → `pelican.material`(values +
   WP116 texture override)。MaterialX OpenPBR は authored 値のみ
   (allowlist — 未対応 node/connection は WP117 の WARN 規範)
2. per-primitive/subset binding を WP119 の mapping に接続し、
   WP116 の binding document として出力
3. texture: PNG 抽出(KTX2 連鎖は opt-in)・colorspace 記録・
   texture transform(supported subset)・UDIM は v1 reject + WARN
4. **engine 側小変更**: importmanifest の output schema 表に
   `pelican.material`(version 1)を additive 追加 + 正負 fixture
   (再レビュー §2.6 の予告どおり)
5. gate: **生成した glb + scene + pelican.material をエンジンが実際に
   OpenPBR で描く golden**(coat 付きサンプルを corpus に追加)+
   決定性 SHA-256 二回一致 + WARN の machine-testable fixture
6. 受け入れ = import-tools pytest + pelican2 全テスト + golden SKIP 0
   (件数 42/41 から追加分更新)+ player 8 秒

### WP122: M-INST0 — per-instance material override ABI

参照: **`docs/design_reviews/2026-07-17_wp120_report.md` の
`MaterialInstanceOverrideBlock` 語彙が仕様の正** + 停止質問③への
確定判断(独立 WP・material template 不変・WP116 の per-material ABI と
混ぜない)。VRM 非依存の汎用機構(per-instance tint 等にも使う)。
依存: WP116(済)・WP121(済 — instance frame の N/N-1 流儀)。
見積: 大。排他: materialcontainer/polygoninstancecontainer の
instance override 面 + シェーダの override 読み + fixture/golden。

1. **`MaterialInstanceOverrideBlock`**: instance ごとの
   color factor / UV transform 値 + generation。material template・
   共有 SSBO は**不変**(instance 側の別 storage — palette と同じ
   instance identity 流儀)
2. 対象パラメータ(v1): base color factor・emissive factor・
   UV offset/scale/rotation(WP117 openpbr と WP58 standard の両
   surface が読めるよう、FrameUBO/pelican_sets の規約に沿った
   instance block として供給 — 実装形はシェーダ側 accessor 経由で
   surface 非依存に)
3. **N/N-1**: expression 由来の毎フレーム更新に耐える current/previous
   (§1-5 と同型 — velocity には関与しないが reload/reset 規則は共有)
4. override なし instance のコスト = ゼロ(既存 golden byte 不変)
5. fixture: 二体別 override・material template 共有の不変証明・
   reload reset・1000 回更新 leak なし・override golden(tint 二体)
6. 受け入れ = 上記全 green + 既存全テスト + golden SKIP 0
   (件数 43/42 から追加分更新)+ player 8 秒

### WP123: VRM-S1b — per-instance expression/application sink

参照: **`docs/design_reviews/2026-07-17_wp120_report.md` の
「VRM-S1b」節(6 項)+「§1-4 phase 写像案」+ 残りの機構語彙
(ExpressionInputSnapshot / ResolvedExpressionFrame /
FirstPersonVisibilityState / VrmApplicationFrame)が仕様の正**。
確定判断(WP120 停止質問②): morph/material/UV/gaze の公開出力は
**versioned application service を additive に追加**(凍結 abi_v1.hpp
無変更・AnimationServiceV1 の成長規律 — engine 内部特権例外は不採用)。
依存: WP111(semantic)・WP121(morph)・WP122(override)— 全て済。
見積: 特大。排他: VRM application 層新設 + application service 公開 +
phase 登録 + fixture/golden。

1. expression 入力の frame snapshot → VRM 合成規則(binary/override/
   bind weight)→ instance ごとの ResolvedExpressionFrame
2. morphTargetBinds → **WP121 の weight frame へ publish**(layout
   解決済み application state — clip/VRMA source と責務分離)
3. materialColorBinds / textureTransformBinds → **WP122 の
   MaterialInstanceOverrideBlock へ**(template 不変)
4. lookAt(bone/expression 両 type)+ firstPerson mesh annotation の
   view 依存 visibility
5. phase 写像 = WP120 レポートの表(parameter_snapshot →
   world_post_process priority 100/200/300 → commit)。**公開
   register_phase のみ使用**(priority 定数と collision policy を
   公開契約に固定)
6. **公開 application service**(additive): resolve/publish 面を
   第三者 DLL fixture で検証(A1.1 の二世代 fixture 流儀)
7. gate = §4.5 逐語(二体別 expression・同 frame revision・
   expression on/off golden)+ bone/expression lookAt + firstPerson +
   N/N-1 + reload/reset + 既存全テスト + golden SKIP 0(45/44 から
   追加分更新)+ player 8 秒(VRM 1.0 fixture の表情確認手順)

**【2026-07-17 追記】WP123 は事前監査停止(4 回目の正しい停止)** —
WP122 override が material index と絶対 target を表現できない
(`docs/design_reviews/2026-07-17_wp123_report.md`)。停止質問の確定:
①**WP122b 先行承認**(下記)②MToon 系 color type は v1 対象外 =
名前入り WARN + bind 単位 skip ③firstPerson(auto split 含む)は
S1c へ分離(近似不採用)④bone lookAt(pose staging)も S1c へ —
**WP123 v1 = morph + material/UV + expression lookAt に再スコープ**。

### WP122b: M-INST1 — material 別 absolute override

参照: **WP123 停止レポートの blocking 監査 1・2 と停止質問 1 が仕様の
正**。依存: WP122(済)。見積: 中〜大。
排他: polygoninstancecontainer / pelican_material_instance.glsl /
ModelTemplate の初期値 metadata。

1. `(instance identity, source glTF material index)` ごとの **版付き
   absolute override storage**(WP122 の instance 1 レコード
   multiplier は互換経路として不変)
2. `ModelTemplate` に **source material index → 初期 color/UV 値**の
   immutable metadata を保持(`Base + Σ((Target-Base)×w)` の Base —
   初期値 0 でも絶対値を表現できる根拠)
3. shader accessor は draw 中の material identity で照合(material
   index なし record は従来どおり全 material 適用)
4. fixture: 同 instance で material 0 のみ着色(他 material 不変)・
   絶対 target(初期値 0 含む)・二体独立・N/N-1・reload reset・
   1000 回 leak なし・override golden。**既存 golden byte 不変**
5. 受け入れ = 上記 + 既存全テスト + golden SKIP 0(45/44 + 追加分・
   **dir 実数照合**)+ player 8 秒

### WP-S1c(予約): bone lookAt + firstPerson

pose staging(公開 application service の版付き transaction)と
firstPerson auto triangle split(import-time)。WP123 着地後に詳細登録。

### WP125: XR0 — OpenXR build unit / activation

参照: **`design_openxr.md` v2.1 §1 が正**。受け入れ条件 = 初回レビュー
`docs/design_reviews/2026-07-17_openxr_review_codex.md` §12 の
**XR0-BUILD-ACTIVATION** と再レビュー
`docs/design_reviews/2026-07-17_openxr_v2_rereview_codex.md` の
**XR0-ACTIVATION-EDGE** の**逐語全文**(タグ参照でなく両ブロックを
そのまま受け入れ条件とする — 逐語添付の規律)。
依存: なし。見積: 中。
排他: ルート/CMake(FetchContent + unit)・launchconfig(--xr 三値)・
dist-config(--with openxr)・OFF smoke・表駆動 fixture。

**XR0-BUILD-ACTIVATION(初回レビュー §12 逐語)**: 「`PELICAN_WITH_
OPENXR` は default ON の private unit とし、OFF では OpenXR source/
object/library/symbol を binary から除去する。runtime activation は
off/auto/on の三値で、explicit on の unavailable は hard error、auto の
unavailable は名前入り INFO + flat とする。headless/RPC/golden/replay は
XR off を強制する。OpenXR-SDK は exact commit 固定・不要 target OFF と
し、single-OFF smoke と full-build flat golden byte 一致を gate にする。」

**XR0-ACTIVATION-EDGE(再レビュー逐語)**: 「通常 window 起動では
`--xr off` は probe なしの flat、`--xr auto` は runtime/system/
graphics-binding 不在時に名前入り INFO を一回出して flat、`--xr on` は
同じ不在・不適合を名前入り hard error とする。headless、RPC、golden、
replay では `off` は flat、`auto` は OpenXR discovery を行わず off に
正規化、explicit `on` は `--xr on is incompatible with <mode>` の
名前入り hard error とする。`PELICAN_WITH_OPENXR=OFF` binary では
`on` の error に同 build flag 名を含め、`auto` は INFO + flat とする。
配布 v1 は `pelican_cli dist-config <project> --with openxr` だけが
`PELICAN_WITH_OPENXR=ON` を preset へ入れる明示入力で、省略時は配布
preset で OFF とする。これは runtime 既定 `xr=off` を変更しない。
compile ON/OFF × mode off/auto/on × window/強制-off driver の表駆動
fixture を XR0 gate にする。」

XR0 では session/instance を**作らない**(discovery 実施は XR1a)。
本 WP の activation 判定は「flag/mode の解決と error/INFO の文言」まで。

### WP126: XR1a — OpenXR discovery + Vulkan bootstrap

参照: **`design_openxr.md` v2.1 §2 が正**。受け入れ条件 = 初回レビュー
§12 の **XR1A-VULKAN-BOOTSTRAP 逐語**: 「XrInstance/XrSystem discovery を
GPU resource 作成前に行い、`XR_KHR_vulkan_enable2` の requirements、
runtime-selected physical device、window/engine 必須 extension を満たす
VkInstance/VkDevice を作る。runtime 不在時だけ resource 作成前に flat
bootstrap へ降格する。fake が engine heuristic と異なる GPU を返す
fixture、required extension merge/missing fixture、multi-GPU 実機記録を
gate にする。」
依存: WP125(済 — activation 解決と discovery hook)。見積: 大。
排他: vkcore/core(bootstrap 分岐)・openxr discovery 実装・
protocol fake の discovery 部分。**session は作らない**(XR1b)。

1. WP125 の discovery hook の実体化: XrInstance → XrSystem →
   `xrGetVulkanGraphicsRequirements2KHR`(loader 直呼びでなく
   `xrGetInstanceProcAddr` 解決の注入表経由 — XR1C-TEST-SEAM の前半)
2. `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` 経由の
   VkInstance/VkDevice 生成(window/engine extension を merge)。
   physical device = `xrGetVulkanGraphicsDevice2KHR`
3. 降格は **GPU resource 作成前に一度だけ**(session 後の hot switch
   なし)。`--xr auto` 不在 = INFO + flat / `--xr on` 不在 = hard error
   (WP125 の行列がそのまま効く)
4. fixture(protocol fake): fake が heuristic と異なる GPU を返す /
   required extension の merge / missing extension で on = error・
   auto = flat / discovery 各段の失敗点。**実 runtime での multi-GPU
   記録はレポートに**(手動 — HMD 不要・Link runtime インストール済み
   環境なら instance/system まで確認可)
5. 受け入れ = fixture 全 green + 既存全テスト + golden SKIP 0
   (46/45・xr=off で byte 不変)+ player 8 秒 + OFF smoke 維持

### WP127: XR1b — session 状態機械 + ループ/display timing

参照: **`design_openxr.md` v2.1 §3 が正**。受け入れ条件 = 初回レビュー
§12 の **XR1B-SESSION-LOOP 逐語**: 「session_running は beginSession
成功から endSession 呼出しまでとし、state の数値比較で代用しない。
running 中は毎回 wait/begin/end を一組実行し、shouldRender=false は
update 一回・zero layer、FOCUSED は input eligibility のみに使う。
predictedDisplayTime は immutable display timing として simulation
EngineTime から分離し、FramerateAdjust は running 中だけ bypass する。
READY/STOPPING/LOSS_PENDING/EXITING と wait/begin/end error の
call trace fixture を gate にする。」+ **XR1C-TEST-SEAM 逐語**(protocol
fake は fake VkImage を Vulkan へ渡さない — WP126 の注入表を拡張)。
依存: WP126(済)。見積: 大。
排他: openxr session 実装 + loop.cpp の XR 分岐 + XrDisplayTiming +
protocol fake 拡張。**swapchain・描画は作らない**(XR2a.1)。

1. session 生成/破棄・xrPollEvent・状態機械(READY で begin・
   STOPPING で end・LOSS_PENDING/EXITING terminal)
2. running 中のループ分岐(設計 §3 の擬似コードどおり):
   xrWaitFrame → EngineTime.advance 一回 → xrBeginFrame →
   (shouldRender なら locate views — 値は保持のみ・描画なし)→
   xrEndFrame(zero layer)→ FramerateAdjust bypass
3. `XrDisplayTiming`(frame-local immutable)— EngineTime.setTime に
   書かない(毎フレーム temporal reset の罠の封じ込め fixture)
4. headless/rpc/golden/replay は running に入らない(WP125 行列)
5. fixture: 全状態遷移 call trace・shouldRender=false・wait/begin/end
   error・zero-layer・「session 中も既存 flat golden 経路不変」
6. 受け入れ = fixture 全 green + 既存全テスト + golden SKIP 0
   (48/47)+ player 8 秒 + OFF smoke 維持。**実機(Link)で
   session_running 到達の記録があればレポートに**(HMD なければ
   system unavailable 降格の再確認でよい)

### WP128: XR2a.0 — renderer 論理フレーム/view 分離(OpenXR 非依存)

参照: **`design_openxr.md` v2.1 §4 が正**。受け入れ条件 = 初回レビュー
§12 の **XR2A0-LOGICAL-FRAME 逐語**: 「`Renderer::render()` を logical
frame と per-view execution に分離する。update/reload/deletion/
animation、target acquire/submit、history flip、object/morph advance、
snapshot commit は logical frame ごとに一回だけ行う。Frame UBO は
in-flight×view の上書き不能 slot とする。real offscreen 二眼 fixture で
異なる view/projection/camera_position が各出力へ届き、frame_index/
history/epoch/object previous が一回だけ進むことを validation layer
付きで検査する。view_count=1 の既存 golden は byte 一致させる。」
依存: WP112(snapshot)・WP95/121(history 流儀)。**OpenXR 非依存**。
見積: 特大。排他: renderer.cpp の分離・frameresources(UBO slot 化)・
snapshot の view 次元 + camera_position(初回レビュー §5 —
view/proj だけでなく position も view 別)。

1. renderLogicalFrame(設計 §4 の擬似コードどおり)への分割
2. FrameUBO = in-flight × view の不変 slot(dynamic offset / 複数 set)
3. snapshot に camera_position を追加し view 別化
4. **Vulkan-backed synthetic stereo target**(実 offscreen 二眼 —
   XR1C-TEST-SEAM の第 2 層)で検証
5. 受け入れ = 逐語条件 + **view_count=1 で既存 golden 全 byte 一致**
   (最重要)+ 既存全テスト + golden SKIP 0(48/47)+ player 8 秒

### WP129: XR2a.1 — IXrCompositionTarget / swapchain

参照: **`design_openxr.md` v2.1 §5 が正**。受け入れ条件 = 初回レビュー
§12 の **XR2A1-SWAPCHAIN 逐語** + 再レビューの **XR2A1-TARGET-BOUNDARY
逐語**(分離確定・per-swapchain 状態機械・failure unwind — 両ブロックの
原文は各レビュー参照、登録時に全文転記済みの本節が受け入れ条件)。
依存: WP127(session)・WP128(logical-frame core)— 済。見積: 大。
排他: IXrCompositionTarget 新設 + XR swapchain + renderer の target
接続(IFrameTarget は不変)。

**XR2A1-SWAPCHAIN(逐語)**: 「二個の 2D swapchain 又は一個の 2-layer
swapchain のどちらかを規範として選ぶ。一 logical frame で各 swapchain を
acquire→wait→GPU submit→release し、左右両 view を一枚の projection
layer として一回の xrEndFrame に渡す。release layout/queue ownership、
format、sample count、per-view rect、shouldRender=false、partial failure
unwind を call trace と Vulkan fixture で固定する。`IFrameTarget` の
flat acquire/present 一回契約を無理に偽装しない。」
(v2.1 で「view ごと arraySize=1 ×2」に確定済み)

**XR2A1-TARGET-BOUNDARY(逐語)**: 「既存 `IFrameTarget` とその
1 acquire/1 submit/present 契約は flat 専用として変更しない。XR は別の
`IXrCompositionTarget` が `beginLogicalFrame / beginView(i) /
endView(i) / endLogicalFrame`、二個の `arraySize=1` color swapchain、
projection layer、単一 `xrEndFrame` を所有する。renderer は target
非依存の logical-frame core から per-view context を受け取り、XR target
を `IFrameTarget` の一実装として偽装しない。各 swapchain の状態を独立に
`idle → acquired → waited → submitted → released` と追跡する。
`XR_TIMEOUT_EXPIRED` は同じ acquired image への wait を再試行し、wait
成功前に release しない。一方の acquire/wait/submit/release が失敗した
場合は、成功済みの他方を合法な順序で unwind し、未 submit の image を
参照する projection layer は渡さない。session が frame call を継続可能な
場合だけ zero-layer `xrEndFrame` で閉じ、session/instance loss は
generation teardown へ送る。左右それぞれの acquire、wait timeout、GPU
submit、release 失敗位置を protocol trace の表駆動 gate にする。」

検証 = protocol fake(状態機械/unwind)+ WP128 の synthetic stereo
(実描画は既検証)。実機表示は XR2a.2(座標)後の統合確認で。

### WP130: XR3a — action set / suggested bindings

参照: **`design_openxr.md` v2.1 §10 が正**。受け入れ条件 = 初回レビュー
§12 の **XR3A-ACTIONS 逐語**: 「project action/action set を attach 前に
XrAction へ作成し、active set stack を xrSyncActions へ写す。Touch
suggested binding、FOCUSED loss、boolean/float/vector2、左右 subaction を
protocol fake と実機で検査する。」
依存: WP127(session)・I1(済)。見積: 中〜大。
排他: openxr action 層 + actionmap の XR backend 接続。
**pose provider は含まない**(XR3b — L1 改訂が要るため別)。

1. `pelican.input_actions` の button/axis1/axis2 を XrAction 化
   (attach 前一括生成・active set stack → xrSyncActions 毎フレーム)
2. Touch controller の suggested bindings 同梱(profile JSON)
3. aim/grip は **left/right を別公開名**(subaction path)— pose 自体は
   XR3b だが命名規約はここで確定
4. 非 FOCUSED = inactive snapshot(error にしない)
5. fixture: protocol fake で sync/action 状態・FOCUSED loss・型 3 種・
   subaction。既存 kbd/mouse/pad 経路 byte 不変
6. 受け入れ = fixture 全 green + 既存全テスト + golden SKIP 0
   (48/47)+ player 8 秒 + OFF smoke 維持

### WP131: XR2a.2 — view snapshot / reference space

参照: **`design_openxr.md` v2.1 §6 が正**。受け入れ条件 = 初回レビュー
§12 の **XR2A2-VIEW-SPACE 逐語**: 「`world_from_stage * stage_from_eye`
と inverse view の行列式、RH/+Y/-Z/m/xyzw、asymmetric RH_ZO projection と
viewport Y、per-view camera_position を規範化する。STAGE/LOCAL_FLOOR/
LOCAL の選択と LOCAL floor offset を status に出す。identity、既知 IPD、
90 度 yaw、非対称 FOV、LOCAL fallback の CPU fixture と Quest 実機の
左右/IPD/head tracking gate を持つ。」(実機 gate は XR2a.3 の統合
確認へ委譲可 — CPU fixture は本 WP)
依存: WP127(locateViews)・WP128(view 別 snapshot)・WP129(target)
— 済。見積: 中〜大。
排他: XR adapter の行列合成 + reference space 選択(WP127 の仮 LOCAL を
STAGE→LOCAL_FLOOR→LOCAL 優先順に昇格)+ get_status。

1. 設計 §6 の行列式どおりの合成(active camera = world_from_stage・
   camera API 不変)。XrFovf → 非対称 RH_ZO・viewport Y 符号
2. reference space 優先順 + LOCAL fallback の floor 非保証明示
   (get_status に space/floor semantics)
3. CPU fixture: identity・既知 IPD・90° yaw・非対称 FOV・LOCAL
   fallback(数値で固定)
4. 受け入れ = fixture 全 green + 既存全テスト + golden SKIP 0
   (48/47)+ player 8 秒 + OFF smoke 維持

### WP132: XR3b — pose provider

参照: **`design_openxr.md` v2.1 §10 が正**。受け入れ条件 = 初回レビュー
§12 の **XR3B-POSE 逐語**: 「L1 を ordered event + typed pose sample に
拡張し、aim/grip action space と synthetic head/view source を区別する。
position/orientation の valid/tracked flag、reference space、左右
identity、freeze 時点を固定する。pose record/replay v1 非対応は名前入り
error とする。」
依存: WP130(action set・aim_left/right 命名)・WP127(session)— 済。
見積: 大。排他: inputstate の L1 拡張(typed pose sample)・
actionmap の pose evaluator(現行の明示 skip を実装に置換)・
ActionPose の flag 拡張・xrSyncActions/locate の freeze 前実行。

1. L1 = ordered event + typed pose sample の frame provider へ改訂
   (kbd/mouse/pad の既存直列化 byte 不変)
2. aim/grip = action space(WP130 の命名)・head = synthetic source
   (xrLocateViews 由来と明記)
3. `ActionPose` に orientation/position の valid/tracked 4 flag +
   source/reference-space identity(公開 struct は additive)
4. pose の record/replay = v1 非対応(query/record 開始時の名前入り
   error fixture)
5. `Actions::pose()` の未実装 error を実装に置換(既存 fixture 更新)
6. 受け入れ = fixture 全 green(protocol fake の pose 供給・
   tracking loss の flag 別挙動)+ 既存全テスト + golden SKIP 0
   (48/47)+ player 8 秒 + OFF smoke 維持

### WP133: XR2a.3 — feature policy + mirror(実機初表示)

参照: **`design_openxr.md` v2.1 §7 が正**。受け入れ条件 = 初回レビュー
§12 の **XR2A3-FEATURE-MIRROR 逐語**: 「flat graph と history/TAA/
velocity/UI を除いた XR graph を precompile し、logical frame boundary で
切り替える。XR entry/flat return で各 history を一回 reset する。mirror
は engine-owned left-eye intermediate を window extent へ scale/letterbox
後、screen-space UI を overlay する optional sink とし、window stall で
HMD を止めない。legacy capture/golden は flat path の意味を変えない。
TAA-on transition trace、window resize/minimize、XR-off golden byte 一致
を gate にする。」
依存: WP128(logical frame)・WP129(composition)・WP131(view/space)
— 全て済。見積: 特大。
排他: graph variant 選択・XR graph compose・mirror sink・
legacy capture の XR 中 reject・loop への XR 描画統合。

1. flat/XR 両 graph の起動時 precompile(XR graph = TAA/jitter/velocity/
   history/UI 除外。除外不能な未知 history feature は XR activation を
   名前入り拒否)
2. logical frame 境界での graph 選択 + XR 出入りの temporal reset
   (epoch 一回)
3. **ここで初めて XR 描画を配線**: session running + shouldRender で
   WP128 core → WP129 target → WP131 の view 行列、の全統合
4. mirror = left-eye intermediate の scale/letterbox + UI overlay の
   optional sink(frame drop 可・HMD ループ非依存)
5. XR 中の legacy capture = 名前入り reject。headless/golden は
   フラット経路のみ(byte 一致)
6. gate: TAA-on project の session transition trace(XR frame plan に
   taa/velocity/history/jitter が無い・flat 復帰で戻る・reset epoch
   一回)・window resize/minimize で HMD 継続(fake で)・XR-off
   golden byte 一致 + 既存全テスト + player 8 秒。
   **実機チェックリスト(§8-3 + 再レビュー §13 の追加項目)は
   レポートに雛形を添付し、ユーザー実機確認は別途**(HMD 接続が
   必要なため — エージェントは fake/synthetic までで可)

### WP134: S1c — bone lookAt + firstPerson(詳細)

参照: **WP123 停止レポートの「追加で見つかった application sink の
不足」節と停止質問 3・4 の確定判断が仕様の正**。
依存: WP123(済)・WP121(morph)・WP131(XR の camera/view —
firstPerson の観点)。見積: 特大。
排他: application service の pose staging(versioned・別 header
negotiation — 凍結 abi_v1.hpp 無変更)・firstPerson の node identity
保存 + auto split(import-time)・lookAt bone type。

1. **pose staging**: application service に版付き pose staging /
   application transaction を追加(phase 100 で leftEye/rightEye の
   local rotation を変更し、300 の constraint と同じ pose を commit へ
   渡せる公開面 — WP123 の質問 4 の確定どおり別 entry point で
   negotiation)
2. **bone lookAt**: VRM 1.0 lookAt bone type(rangeMap 適用)を
   staging 経由で実装
3. **firstPerson**: 明示 annotation の node identity を
   ModelPrimitiveRefInfo/RenderCommand 系へ保存 + **auto の
   triangle split は import-time**(head bone weight による分割 —
   import-tools でなくエンジン load 時の前処理として実装し、
   分割結果の決定性 fixture)+ view 依存 visibility(XR の
   first-person view で head 非表示)
4. gate: bone lookAt の角度 fixture(rangeMap 数値)・staging の
   phase 間一貫性・auto split の決定性・visibility の per-view 切替 +
   既存全テスト + golden SKIP 0(48/47 + 追加分・dir 実数照合)+
   player 8 秒

### WP135: XR4 — VRM キャラデモ project(flat 先行)

参照: **`design_openxr.md` v2.1 §11 XR4 行 + 本節が仕様の正**。
狙い = Quest 3 で VRM キャラが動く体験の中身を**実機なしで完成**させる
(XR は起動フラグのみ — flat で全部検証可能)。
依存: WP101(anim_graph)・WP121〜123b/134(VRM 表情・lookAt・
firstPerson)・WP130/132(Touch/pose)— 全て済。見積: 大。
排他: `projects/vrm_xr_demo/` 新設 + fixture。**エンジンコード変更
禁止**(不足があれば実装せず質問 — デモはユーザー空間の集大成)。

1. **project**: 決定的な合成 VRM 1.0 キャラ(test の vrm fixture
   writer を昇格 — humanoid 全必須骨 + 表情 + lookAt + firstPerson
   込み)+ 地面 + ライト。README に「自分の .vrm(1.0)への
   差し替え手順」
2. **アニメ**: anim_graph(Idle/Walk/Run + Jump 割込み — WP101 の
   デモ流儀)を VRM キャラへ。WASD/pad(flat)と Touch stick(XR)を
   同じ input_actions で
3. **VRM 機能の実演**: 視線 = カメラ(flat)/ HMD(XR の synthetic
   head pose)追従・表情サイクル(ボタンで preset 切替)・
   firstPerson(XR で頭部非表示 — flat では third person)
4. **XR 対応の配線**: `--xr on` で動く構成(XR graph 除外に抵触する
   feature を使わない)。Touch bindings は WP130 の命名
5. gate: flat での rpc 駆動 fixture(anim 遷移・表情切替・lookAt 角度の
   決定的検証)+ golden 1〜2 枚 + player 8 秒(flat)+ **XR は
   起動フラグの activation 検証まで**(実機/Simulator は別枠)
6. 受け入れ = 上記 + 既存全テスト + golden SKIP 0(件数追加分更新)

### WP136: XR Simulator smoke(実機前の実 runtime 検証)

参照: 本節が仕様の正。狙い = **Meta XR Simulator(または入手可能な
デスクトップ OpenXR runtime)に対して `--xr on` の実経路を通す**。
依存: WP133(済)。見積: 中(調査含む)。
排他: **エンジンコード変更禁止**(発見した問題は再現手順付きで
レポートへ — 修正は別 WP)。成果物 = レポート + 導入手順。

1. **導入調査**: Meta XR Simulator の入手経路(Meta 開発者
   ダウンロード・npm/nuget・Unity/Unreal パッケージ同梱)を調査し、
   ログイン不要で自動導入できるならインストール。不可なら
   **ユーザー向け導入手順書**を成果物にする(URL・手順・
   XR_RUNTIME_JSON の設定)
2. 代替 runtime(Monado Windows 等)も 1 案評価(導入性のみ)
3. **導入できた場合**: `--xr on` で player を起動し、チェックリスト
   §8.2(READY→FOCUSED 遷移・shouldRender・focus loss)と §8.4
   (graph transition・mirror)の自動化可能項目を実測。ログ・
   get_status.xr・mirror スクリーンショットをレポートに
4. 受け入れ = レポート
   `docs/design_reviews/2026-07-17_wp136_report.md`(導入可否・
   実測結果 or 手順書・発見問題の再現手順)。エンジンの
   テスト/golden には触れない

### WP138: XRSIM 修正 — Simulator で発見した実機 blocker 3 件

参照: **`docs/design_reviews/2026-07-17_wp136_report.md` §5 が現象の正**。
設計判断(2026-07-17 確定):
依存: WP133/136(済)。見積: 中。
排他: imgui gate・vkcore device features・XR 診断ログ。

1. **XRSIM-1(ImGui frame 不整合 — blocker)**: v1 の解 =
   **XR session active 中は ImGui frame を begin しない**
   (`imguiruntime` の enable gate に XR を追加 — headless/replay と
   同列)。framephase 側で begin 済みの frame が XR 遷移を跨ぐ場合は
   遷移前に必ず閉じる。mirror への ImGui 表示は将来需要が出たら別 WP
   (「XR 中はエンジン UI なし」を v1 仕様として明記)
2. **XRSIM-2(timelineSemaphore validation error — high)**:
   device 作成時に `timelineSemaphore` feature を**対応 GPU なら常時
   有効化**(VkPhysicalDeviceVulkan12Features chain)。非対応 GPU で
   `--xr on` の場合は名前入りエラー(Meta runtime の Vulkan layer が
   要求するため)。flat 挙動は不変(feature 有効化のみ・golden 維持)
3. **XRSIM-3(観測不能 — medium)**: RPC/windowed 排他は維持
   (WebSocket トラックで再訪)。代わりに **XR 診断ログ**を規範化:
   session state 遷移・view configuration(初回)・reference space・
   `shouldRender` 変化を 1 行ログで出す + `get_status.xr` schema へ
   同項目を additive 追加(headless fake 経由で fixture 可能)
4. **gate = Simulator 再検証**(WP136 の手順で MSI admin-image を
   再展開): FOCUSED 到達 → **連続 1000 frame 描画**(assert なし)→
   mirror 非白画面のスクリーンショット → focus loss/regain →
   `shouldRender=false` の zero-layer 経路 → 3 分連続稼働。
   validation error 0(XRSIM-2 の消滅確認)
5. 受け入れ = 上記 + 既存全テスト + golden SKIP 0(件数不変)+
   player 8 秒(flat・ImGui 従来どおり)+ OFF smoke

### WP139: D-P0a — debug-utils 基盤 + command label

参照: **`design_debug_profiling.md` v1.1 §1 + レビュー C1(前半)が
正**(逐語: 「D-P0 を debug-utils 基盤/command label と resource
coverage の二つに分割する。D-P1 が依存するのは前者だけとする」)。
依存: なし。見積: 小〜中。
排他: vkcore の debug-utils 配線 + renderer/executor の label 挿入。

1. `VK_EXT_debug_utils` の有効化(instance/device — 存在しない環境では
   全 API が no-op・dist では既定 OFF の起動フラグ `--gpu-labels`)
2. **command label**: 既存正本(pass 名・anchor・compute task 名・
   graph variant suffix)から `vkCmdBeginDebugUtilsLabelEXT` を
   pass/compute/mirror 境界に機械挿入。XR は view index を label に含む
3. object 命名は**既存正本があるものだけ**(RT image/view・swapchain・
   frame UBO 等)。buffer/pipeline の全面命名は D-P0b(inventory 込み)
4. gate: label on/off で golden byte 不変・validation error 0・
   RenderDoc(手動)で pass 木が読めるスクリーンショット 1 枚 +
   既存全テスト + player 8 秒
5. 受け入れ = 上記 + OFF smoke(ユニット非依存 — 拡張なし環境 no-op)

### WP140: D-P1a — RenderDoc in-app capture(flat/headless)

参照: **`design_debug_profiling.md` v1.1 §2 + レビュー C2 逐語が正**:
「RenderDoc は注入済み module の受動取得だけに限定し、capture の状態
機械と境界を規範化する。`LoadLibrary` しない。RPC は既存 `render_frame`
と同じ current-time render を一回だけ capture し、実際に増えた capture
index から `.rdc` path を得る。F11、RPC、overlapping request、失敗
code、flat/headless/XR の境界を固定する。」
依存: WP139(済 — label がある前提で capture が読める)。見積: 中。
排他: renderdoc_app.h 同梱(単一 header・MIT)+ capture 配線 + rpc。

1. `renderdoc_app.h` を third-party として同梱(pin + ライセンス表記)。
   **GetModuleHandle での受動取得のみ**(RenderDoc 起動でアプリを
   立ち上げた場合だけ有効 — 未注入時は全 API no-op + get_status で
   `renderdoc: absent`)
2. **F11** = 次フレームを 1 枚 capture。**rpc `capture_gpu`** =
   render_frame と同型の一回 render を capture し、増えた capture
   index から `.rdc` path を応答に返す
3. 状態機械: 要求中の再要求は名前入り reject・失敗 code 伝搬・
   headless では rpc のみ・**XR 中は v1 reject**(XR capture は
   後続 WP — C2 の境界どおり)
4. gate: RenderDoc を導入(winget/公式インストーラ — version 記録)し、
   **注入起動で .rdc が生成され pass label 木が読める**ことを実測
   (スクリーンショット — WP139 の未実施分もここで回収)。
   未注入起動で全 no-op・golden byte 不変・既存全テスト + player 8 秒
5. 受け入れ = 上記 + OFF smoke

### WP141: 負債 GOLDEN0 — golden inventory の GPU 不要 gate 化

参照: **負債議論 `docs/design_reviews/2026-07-17_debt_discussion_codex.md`
の WP-GOLDEN0 定義が正**(逐語: 「case/file/hash/trace の exact set、
VAT ON/OFF 条件、update mode membership lock。完了後に count REQUIRE を
削除」)。狙い = golden の「件数 REQUIRE 手動更新」文化(交差マージ罠が
二連続した)を、**機械照合の inventory manifest** に置換して CI(CPU
gate)にも乗せる。
依存: WP137(済)。見積: 中。排他: golden inventory fixture +
golden_image_test の count REQUIRE 削除 + CI への組み込み。

1. **inventory manifest**(コミット物): 全 golden case の
   {name, files, expected.png の sha256, tolerance 有無, VAT 条件} の
   exact set。生成は決定的スクリプト(更新 mode 付き — 追加/削除が
   明示的 diff になる)
2. **GPU 不要の照合テスト**: ディレクトリ実態と manifest の完全一致
   (過不足・hash 不一致・未知 file を名前入りで列挙)。`gpu` ラベル
   なし = CI で毎 push 照合
3. update mode(golden 再基準化)でも membership が manifest 経由で
   ロックされること(勝手な case 追加が CI で赤になる)
4. **count REQUIRE(現 49/48)を削除**(inventory が正になるため)。
   ローカル gate の「golden dir 数照合」手順も docs 更新
5. 受け入れ = inventory fixture green(CPU)+ 既存 golden 全維持
   (GPU ローカル)+ 既存全テスト + player 8 秒 + CI green

### WP142: 負債 LIGHT0 — magic-name light animation の authoring 境界化

参照: **負債議論 `docs/design_reviews/2026-07-17_debt_discussion_codex.md`
の WP-LIGHT0 定義 + N1 節が正**(逐語: 「magic-name light animation を
core から example/component へ移し、light cap 超過を fail/warn。
shadow fitting は別 acceptance criteria に切り出す」)。
依存: なし。見積: 小〜中。
排他: 該当 core コードの撤去 + example の component/ゲームコード化 +
light cap 検証。

1. core 内の名前依存ライトアニメーション(N1 の指摘箇所)を特定し
   **core から削除**。同じ見た目を example 側のユーザー空間
   (component + ゲームコード or transform_seq)で再現
2. **light cap 超過 = 名前入り WARN**(何個目からが落ちたか)。
   silent drop を廃止
3. shadow fitting が絡む場合は挙動を変えず acceptance criteria の
   分離だけ記録(本 WP で fitting を触らない)
4. gate: **example の golden byte 不変**(移設が見た目を変えない
   証明)+ cap 超過 fixture + 既存全テスト + player 8 秒 + CI green

### WP143: D-P2a — stereo-safe GPU timing

参照: **`design_debug_profiling.md` v1.1 §3 + レビュー C3 逐語が正**:
「pass 名だけをキーにしない。`graph variant + logical frame + view
index + node ordinal/kind/name + subrange` を identity とし、左右眼を
混ぜない。barrier、render、compute、anchor、output transform、mirror の
どこを含むかを宣言し、query pool は in-flight ring で再利用する。」
依存: WP29(gpu_timing)・WP133(XR graph)・WP139(label と同じ
node identity)— 済。見積: 中〜大。
排他: rendertiming の v2 化 + query pool ring + get_status/ImGui 表示。
**VRAM・XR timing 変換(C4)は D-P2b — 本 WP ではやらない**。

1. 計測 identity を C3 のキーに改訂(WP139 の label 命名と同一規範 —
   RenderDoc の木と 1:1 で突き合う)
2. 含む区間の宣言表(render/compute/anchor/output_transform/mirror・
   barrier の帰属)を規範化し fixture 化
3. query pool を in-flight ring 化(現行の不参照時不生成は維持)
4. 表示: get_status.gpu_timing(diagnostics — C5 の決定性除外扱い)+
   ImGui 表(flat)。**計測 on/off で golden byte 不変**
5. gate: flat + XR(Simulator)で view 0/1 が別 row になる実測 +
   計測 on/off golden 不変 + 既存全テスト + player 8 秒 + OFF smoke

### WP144: 負債 TRANSIENT0 — load_gltf の強 transaction

参照: **負債議論 `docs/design_reviews/2026-07-17_debt_discussion_codex.md`
の WP-TRANSIENT0 定義 + A2 節が正**(逐語: 「`load_gltf` name
preflight/rollback、model swap の stage-commit、失敗時に entity/slot/
draw command が不変である gate」)。HR2-G(WP110)の staged commit
基盤を rpc `load_gltf` / 一時ロード経路にも適用する形。
依存: WP110(済)。見積: 中〜大。
排他: rpc load_gltf 経路 + scene loader の一時ロード + rollback fixture。

1. **name preflight**: 衝突する宣言名・不正 fragment を GPU 資源確保の
   **前に**検証(現行の途中失敗を根絶)
2. **stage-commit**: WP110 の candidate/publish 面に載せ替え。途中失敗
   (parse/validate/GPU capacity)で entity/slot/draw command/texture/
   material の**全カウントと ID が不変**である gate(WP110 fixture の
   rpc 経路版)
3. 失敗応答は rpc error に原因(名前入り)を返す
4. 受け入れ = rollback fixture(CPU 失敗/GPU 失敗の両方)+ 既存全
   テスト + golden SKIP 0 + player 8 秒 + CI green

### WP145: D-P2b — VRAM 計測 + XR timing 変換

参照: **`design_debug_profiling.md` v1.1 §3 + レビュー C4 逐語が正**:
「memory budget は heap ごとの driver budget と engine logical
allocation/free-range を分離する。`shouldRender=false` を dropped frame
と数えず、portable な `XR_FRAME_DISCARDED`、mirror drop、vendor metric を
別 counter にする。XrTime と QPC の差は
`XR_KHR_win32_convert_performance_counter_time` が使える場合だけ計算
する。」依存: WP143(済)。見積: 中。
排他: VRAM 計測(VK_EXT_memory_budget)+ XR frame counter 群 +
get_status/ImGui 追記。**WP144 と並走 — gltf/model/rpc load 経路に
触らない**(VertBuf の論理 allocation は読み取りのみ)。

1. **VRAM**: heap 別の driver budget/usage(VK_EXT_memory_budget —
   非対応 GPU は named absent)と、engine 論理値(VertBuf free-range・
   texture/material カウント等の既存 ForTesting 面の集約)を**別枠**で
   get_status.memory へ
2. **XR frame counters**: `XR_FRAME_DISCARDED` 系・mirror drop・
   shouldRender=false 区間を**別 counter** に(dropped と混同しない)。
   XrTime↔QPC は KHR 拡張がある場合のみ変換(なければ absent)
3. 表示: ImGui に memory 表(diagnostics — C5 の正規化除外)
4. 受け入れ = fixture(fake budget/counter)+ Simulator で counter が
   動く実測 + 計測 on/off golden byte 不変 + 既存全テスト + player
   8 秒 + OFF smoke

### WP146: 負債 INSTANCE0 — ModelInstanceId の SlotMap 化

参照: **負債議論 `docs/design_reviews/2026-07-17_debt_discussion_codex.md`
の WP-INSTANCE0 定義が正**(逐語: 「free-list + generation + alive +
scene epoch、1 万回 churn、stale/double-remove/clear/capacity gate。
scene/playback/transient ownership も明文化」)。
依存: WP110/144(済 — rebuild/transaction は既存面を維持)。
見積: 中〜大。排他: polygoninstancecontainer の instance id 管理 +
全 caller(scene loader/playback/rpc/transient)の handle 検証。

1. `ModelInstanceId` を **generation 付き SlotMap** に(free-list 再利用 +
   alive check + scene epoch)。stale handle は全 API で名前入り
   エラー/false(黙って identity 行列にしない)
2. remove の slot 回収(現行の「identity 化して放置」を廃止 —
   1024 枯渇の恒久解消)。WP110 の in-place rebuild は同 slot 維持で
   不変
3. **ownership 明文化**: scene loader / SeqPlayer / rpc 一時ロードの
   どれが remove 責務を持つかを表にして fixture 化
4. gate: **1 万回 place/remove churn で枯渇なし** + stale/
   double-remove/clear 後 use/capacity 超過の各 fixture + 既存全テスト +
   golden SKIP 0 + player 8 秒 + CI green

### WP147: 負債 ANIM0 — animation registry の reload generation

参照: **同議論の WP-ANIM0 定義が正**(逐語: 「legacy registry
invalidation、Evaluator rebind、asset 単位 generation、複数回 reload
継続 gate」)。WP99 が入れた全体 reset(保守的境界)を **asset 単位の
invalidation** に精密化し、WP110 の「skeletal 置換 = ABI registry 全体
reset」制限を解消する。
依存: WP97/99/102/110(済)。見積: 中〜大。
排他: animation 側 registry/service の generation 管理 + 評価器の
rebind 経路。**WP146 と並走 — polygoninstancecontainer の id 管理に
触らない**(publish の instance generation 検査は既存面を使う)。

1. `AnimationAssetRegistry` を asset(model)単位 generation に
   (全体 clear の廃止)。他 asset の Rig/Clip/Cursor は reload 後も
   有効のまま
2. **Evaluator rebind**: anim_graph 評価器(WP101)が stale 検出後に
   同じ graph/parameter 状態で新 generation へ再 bind できる公開経路
   (状態を失わない — deterministic trace で前後一致 fixture)
3. model hot reload(HR2-G)・rpc load_gltf(WP144)と接続し、
   **複数回 reload しても他モデルのアニメが継続**する gate
4. 受け入れ = 上記 fixture + WP99 敵対 fixture 全維持 + 既存全テスト +
   golden SKIP 0 + player 8 秒 + CI green

### WP148: 負債 ECS0 — scheduler の hazard 照合(最大の構造リスク)

参照: **負債議論 `docs/design_reviews/2026-07-17_debt_discussion_codex.md`
の WP-ECS0 定義 + N3/Q5 節が正**(逐語: 「read/write hazard と
declared DAG の照合、missing dependency/cycle/unexecuted-node
fail-fast、必要な automatic serialization」。実コード指摘: 登録時に
収集済みの read/write index を execution level 構築が無視し、同 level を
並列 schedule — edge 1 本の忘れで write/write race)。
依存: WP62(済)。見積: 大。**単独派遣(他 WP と並走禁止 — ECS コアは
全系の土台)**。
排他: `src/core/userpublic/details/ecs/` の scheduler + predefined の
edge 検証。**登録 API の意味論・既存 system の実行結果は不変**。

1. **hazard 照合(fail-fast)**: graph 構築時に、収集済み read/write
   metadata に対し「全 conflict が依存辺で順序付けられているか」を
   検査。未順序 conflict は **system 名ペア + component 名入りの
   起動時エラー**(黙って並列にしない)
2. cycle・missing dependency・unexecuted node も同時に fail-fast
   (現行の静かな脱落を根絶)
3. **automatic serialization(opt-in の既定 ON)**: 未順序 conflict を
   エラーにする代わりに「登録順で直列化 + WARN」するモードを
   起動フラグで(移行期の逃げ道 — strict では常にエラー)
4. read/read 並列は維持。既存 predefined/example の全 system が
   エラーなしで通ることを確認(必要な明示 edge の追加は本 WP で —
   実行順が変わる場合は golden で検証)
5. 受け入れ = hazard 正負 fixture(edge 忘れ検出・cycle・
   unexecuted)+ 既存全テスト + **golden 全 byte 不変**(順序修正が
   絵を変えないこと — 変わる場合は停止して質問)+ player 8 秒 +
   CI green

### WP149: ED-AUTH0 — AuthoringSceneDocument(エディタトラック第 1 弾)

参照: **`design_editor_tooling.md` v2.1 §0-1 + 再レビュー
`docs/design_reviews/2026-07-18_editor_behavior_v2_rereview_codex.md`
§7 の単独着地境界(逐語)が受け入れ条件**:

> ED-AUTH0 は scene v1 bytes の parse、全 envelope/scene/object/
> component raw JSON 保持、SceneRevision/AuthoringObjectId 割当、
> ProjectBasicConfig cache との単一 read/update/invalidate 面、
> deterministic semantic encode fixture までを所有する。`query` は
> テスト用の raw authoring traversal を意味し、EditorCommandService/
> RPC schema、component codec、runtime edit、journal、Save file
> replace を含めない。runtime bind は既存 SceneLoader へ同じ semantic
> JSON を供給するだけとし、未編集 load、scene 切替、G2 currentScene
> rebuild の観測挙動を変えない。既存全 scene fixture に加え、複数
> scene、無名、light-only、collider-only、unknown/read-only component
> を含む未編集 document の load→encode→fresh load semantic equality を
> gate とする。

依存: なし(WP62 済)。見積: 中。
排他: AuthoringSceneDocument 新設 + ProjectBasicConfig cache 統合。
**codec・RPC・edit・Save を一切含めない**(単独着地性の条件)。

受け入れ = 上記逐語 gate + 既存全テスト + golden SKIP 0・byte 不変 +
player 8 秒 + CI green

### WP150: STRUCT-SCHEMA0 — 共通 field descriptor + use-site policy

参照: **再レビュー
`docs/design_reviews/2026-07-18_editor_behavior_v2_rereview_codex.md`
§3.1(B-C1)の逐語が受け入れ条件**:

> StructFieldSchema は field descriptor の型情報を共通化するが、
> decode/validation は `EventPayload`、`BehaviorParams`、`Component` の
> 明示 use-site policy を必須とする。EventPayload policy は既存どおり
> Bool/Enum/default/optional を許可せず全 field required、
> unknown-field reject、現在の validation/error precedence、
> `load_json_payload` 呼出回数を不変にする。BehaviorParams policy
> だけが Bool/Enum/default と存在 key のみの atomic apply を許可する。
> Component policy は codec ごとに required/default を明示する。同じ
> field 宣言 primitive を使っても、policy の省略または暗黙 default は
> compile error とする。
>
> WP71 の全既存 runtime/compile-fail fixture を無変更で通し、bool
> event compile-fail、missing field、unknown+missing 同時入力の error
> code 優先順位、typed/payloadless/opaque 分類、descriptor/ref 一致
> 検査、payload load call count が refactor 前と一致する regression
> gate を追加する。既存 event JSON の受理/拒否集合を一件も広げず
> 狭めない。

依存: WP71(済)。見積: 中〜大。
排他: EventPayloadSchema からの descriptor 抽出 + policy 機構 +
regression gate。**behavior/codec の実装は含めない**(BEH-P0/
ED-CODEC0 が消費者)。

受け入れ = 上記逐語 gate(WP71 全 fixture 無変更が最重要)+ 既存全
テスト + golden SKIP 0 + player 8 秒 + CI green

### WP137: 負債 CI0 — CPU gate の常設(GitHub Actions)

参照: **負債議論 `docs/design_reviews/2026-07-17_debt_discussion_codex.md`
の「WP-CI0」定義が正**(A3 の三分割の第 1 — GPU runner を待たずに
CPU gate を立てる。**自動 retry は入れない**)。
依存: なし。見積: 中。
排他: `.github/workflows/` 新設 + ctest のラベル/除外整備 +
docs(CI 運用 1 ページ)。**エンジンコードの挙動変更禁止**
(テストのラベル付与・CMake の option 追加は可)。

1. workflow(windows-latest): configure(Python は runner の
   setup-python で供給 — ローカル固有パスに依存しない形へ)→
   Debug build → **GPU 不要テストのみ**の ctest 実行
2. **GPU 不要サブセットの機械的定義**: golden/player/GPU 依存
   テストへ ctest LABEL(例 `gpu`)を付与し `-LE gpu` で除外。
   「どのテストが CI で走るか」が CMake から一意に決まること
3. **SKIP の exact allowlist**: CI では「予期しない Skipped =
   失敗」(symlink 権限等の許可済みリストのみ通す — SKIP 偽緑の
   再発防止を CPU 側から)
4. 失敗時 artifact(ログ・失敗テスト出力)保存。**retry なし**
   (flaky は赤のまま可視化 — 議論の C5 処方どおり)
5. ローカルゲートとの関係を docs に 1 ページ(CI = CPU 回帰の砦・
   golden/player はローカル(将来 CI2)のまま、の分担)
6. 受け入れ = workflow 定義がリポジトリに乗り、ローカルで
   `ctest -LE gpu` 相当が green + SKIP allowlist が機能 + ローカル
   全 ctest/golden/player 無影響(byte 不変)。GitHub 上の実 run は
   push 後に私(Claude)が確認

## 3. 保留中のトラック(WP 化待ち)

- **【方針決定 2026-07-12】feature 層 = ユーザー空間**(ジッタ相談からの
  一般化 — ユーザー決定): テンポラル系のように**手法が発展し続ける領域は
  purgeable feature 層で吸収**し、ユーザーが「まぁまぁいじる」前提で
  境界を固定する。
  1. **三層の速度分離**: エンジン = 機構語彙(anchor / history /
     projection_jitter 枠 / snapshot / format_class 等 — **版付きで
     ゆっくり additive にだけ増える**)/ feature(JSON + シェーダ)=
     ユーザー空間(速い・自由)/ project config = 1 行有効化
  2. **同梱 feature(hdr / shadow / bloom / debug 系)= 特権なしの標準
     ライブラリ**(standard/toon lighting の dogfooding と同格)。
     `engine://features/*.json` をプロジェクトへコピーして改造したら
     自分のもの、が公式ワークフロー(ロードは ref ベースで既に
     プロジェクト相対を通せる — 保証テストを次の feature 系 WP に同梱)
  3. `pelican.render_feature` schema は実質の公開 API — [PF] と同じ
     凍結規律(additive・版付き)で扱う
  4. **いじらせない境界**(検証が名前入りエラーで防衛): canonical
     anchor の全順序(挿すのは自由・エンジン terminal の並べ替え不可)、
     output_transform 等の常設ノード、色 invariant、決定性ゲート
  5. 新手法対応の型: エンジンは機構語彙を 1 個足すだけ(例: 将来の
     アップスケーラ向けフェーズ数属性)→ ユーザー feature が組み合わせる

- **最小コマンド層**: (1) ファイル連携済み → (2) WP27 実装済み → (3) `load_gltf` / `update_transforms` は **2026-07-07 に実装 GO 決定**。前提はすべて充足(宛先 = scene v1 の objects[].name / アセット意味論 = WP21)。設計時要件: **複数インスタンス運用**(エージェントが複数エンジンを並行駆動する使い方) — stdio rpc は 1 プロセス 1 クライアントの現行構造を維持しつつ、`get_status`(instance id・project・フレーム番号)を追加してインスタンス識別可能に。プロジェクトは読み取り専有なので並行起動は安全(書き込み系操作を入れる際に排他を設計)。複数クライアント同時接続は TCP/WebSocket 展開時の課題として分離
- **ゲームロジック(ネイティブ C++)**: `design_game_logic_native.md`(2026-07-07 方針決定 — スクリプト不採用、C++ 複数ファイル。G1a システム登録 API → G1b PELICAN_PROJECT ビルド取り込み → G2 DLL ホットリロード)
- **devstudio(Qt)**: 2026-07-07 決定 — `design_devstudio_direction.md`。D1(埋め込みビューポート + アウトライナ)→ D2(ピッキング/ギズモ/編集)→ D3(保存 round-trip/undo)。前提 = 解釈レイヤのターゲット分離(WP44 済)。**D0 追加(2026-07-08 ユーザー決定): エディタ特権の禁止 — 標準 UI もツール。編集系 rpc を D2 の前提として先に定義。WebSocket 展開の優先度引き上げ。`pelican_rpc.py`(薄い rpc クライアント)を小 WP 候補に**
- **カメラシステム**: 2026-07-07 方向決定 — **glTF カメラと同等以上**。glTF の perspective/orthographic を損失なく読み書き(拡張は extras に載せ round-trip 可能に)し、その上にコントローラ(orbit/follow/fly)・カットとブレンド・transform_seq v2 カメラトラックとの統合を積む。設計文書はコントローラ API(GameContext との接続)込みで起草する
- **2D ゲーム機能 + 2D⇔3D 相互変換**: 2026-07-07 方向決定 — 2D ゲーム向け機能(スプライト・直交投影・2D 物理の要否)は必要。UI システムとは**描画基盤(2D パス)を共有し上物を分離**する方針を提案(UI = レイアウト/イベント、2D ゲーム = スプライト/カメラ/物理で要件が異なるため)。**2D⇔3D 相互変換**(2D シーンの 3D 空間配置・3D シーンの 2D 投影編集)は設計文書のスコープ課題として最初に定義すること
- **アニメーショングラフ**: `design_animation_graph.md` **v2.1**(2026-07-12 — ultra リサーチ + 敵対レビューで条件付き承認)。実装順 A0(=WP94 登録済み)→ A1 → A1.5 → A2(受入条件 = レビュー §4.3/§4.4 逐語)→ VRM 5 WP(VRM-S0/S1・VRMA-C0/R0/I0)。A2 以降は A0 の着地を見てから WP 化
- **物理クエリ**: 2026-07-07 **必須決定** — raycast / overlap を GameContext(G1a)とエディタピッキング(D2)の両方に供給する。休眠中の phys モジュール・collision ブランチの再評価から着手。コリジョン形状は glb 内規約(フォーマット方針 §4 予約)と同時に設計
- **OpenXR トラック**: **2026-07-07 に推進決定**。入力(アクション層・pose 型)は準備済み。残り = ①ランタイム統合(xrWaitFrame とループ主導権・EngineTime 統合)②描画(フレームグラフに view 次元 = multiview、XrFrameTarget を IFrameTarget の第 3 実装として追加)③PELICAN_WITH_OPENXR ユニット必須・ヘッドセットなし環境のテスト戦略。設計文書を書いてから WP 化
- **bindless バックエンド**: 2026-07-08 方向決定 — classic(set 2)と併用(`design_material_shading.md` §3-5)。生成アクセサが差を吸収、M2 のデータ形(SSBO + 参照)が前提工事。実装は M2 の後・GPU 駆動系(WP36 パーティクル・大規模シーン)の需要と同時に WP 化。**web/モバイルの床に PC を縛らせない**(web は将来やるとしてもシンプルな 3D/2D — ユーザー確認)
- **web ビルド(WASM)**: 将来の可能性としてのみ保持(2026-07-08)。守るべき不変条件は全部現行規律(純ロジック規律・データ契約が抽象・classic 床・dist-bake/WGSL レーン)— 特別な保全作業なし。進めるときは案 B(WASM ゲームコア + TS レンダラ接合)→ 案 A(C++ WebGPU 実行系、データ契約の兄弟執行器)。**RHI の後付けは禁止**(本体の C++ インターフェースへの制約源にしない)
- **アセットホットリロード**: v2 の HR0/HR1/HR1-T/HR1-M/HR2-S(WP108)/HR2-G(WP110)まで完了。確定規約(リプレイ/strict/rpc 中無効・自己書き込み `(AssetKey, hash, epoch)` token)と単一 FileWatcher 経路を維持する。残りは HR2-I(input/profile)
- **イベント層**: `design_event_layer.md` v1 ドラフト(2026-07-08)。**API 意味論(emit/購読の書き味・フレーム境界配送)のユーザーレビューを経てから E1 を WP 化**。E2(物理トリガー)は E1 後
- **永続化(user:// + 設定/セーブ)**: `design_persistence.md` v1 ドラフト(2026-07-08)。**user:// スキーム追加 = [PF] v6.3 の凍結改訂が必要 — ユーザー承認待ち**。承認後 P1 を WP 化
- **[PF] v6.3 改訂案(一括)**: ①user://(persistence)②asset store マウント + .pelican/local.json(`design_project_vcs.md`)③#フラグメント参照(`design_asset_containers.md`)。**3 点まとめてユーザー承認を取り、1 回の版数改訂で凍結文書へ反映**。承認後の WP: P1 / V1〜V3 / K1〜K4
- **コンテナアセット**: `design_asset_containers.md` v1 ドラフト。K1 フラグメント参照 → K2 glTF シーン抽出(scene v1 親子改訂と同時)→ K3 pelican-import-tools 創設(PSD = psd-tools、アトラスパック)→ K4 import ルール表。PSD 系はエンジン非リンク(外部ツール契約)
- **RenderWorld / ECS**: (2026-07-02 方針変更)統合ブランチ系列を開発本線として独自に進める。**(2026-07-08 改訂・ユーザー決定)`src/core/ecs/` の変更禁止を解除** — 凍結の代償(PhysWorld 生ポインタ・カメラ二重所有・dummy コンポーネント・cb_deinit 不呼出で非自明型が memcpy 移動される)が 2026-07-08 リファクタ監査で顕在化したため。以後 ECS コアに世代付き EntityId・construct/move/destroy 規約等を入れてよい。main との合流は「随時追従 merge」から「将来の逆提案(こちらの ECS 改良を main へ提案)」へ位置づけ変更。**互換受理の追加禁止(同日決定)**: ランタイムは v1 だけを読む。旧形式の変換が要る場合は外部ツールで行う — 以後の WP が新旧両対応の受理コードを足すことを §0 違反とする
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
| 10 | WP20(a→b), WP21(+ WW5・houdini-adapter は別リポジトリ) | WP20 と WP21 は並列可。競合: test/CMakeLists.txt(追記のみ)とルート CMakeLists FetchContent 節(WP21 のみ追記)。WP20 = model/playback/shader 系、WP21 = devcli/loader 系で分離 |
| 11 | WP28, WP33, WP37 | 並列 3 本。WP28 = featurecompose + shader/pipelinefactory 系(renderingpassconfigloader は WP28 専有)、WP33 = frameplanner 新設 + テストのみ(**実行系変更禁止**)、WP37 = os/入力系。共有追記は test/CMakeLists.txt のみ |
| 12 | WP29, WP30 | WP28 マージ後。WP29 = renderer 計測系、WP30 = feature アセット中心で接触面小 |
| 13 | WP34 | 実行系の大物。単独で走らせる(完了 — render 切替も先取り実装) |
| 14 | WP35, WP39(+ WW6 別リポジトリ) | 並列 3 本。WP35 = rpc/framegraphruntime 周辺、WP39 = os/入力 + loader 小、WW6 = web。共有は test/CMakeLists.txt 追記のみ |
| 15 | WP40 | ビルドユニット化(完了) |
| 16 | WP41, WP42, WP43 | 並列 3 本。専有: WP41 = devcli、WP42 = communication + loader/scene バインダ(loop.cpp 禁止)、WP43 = appflow/loop + userpublic(communication 禁止)。共有は test/CMakeLists.txt 追記のみ |
| 17 | WP44(解釈レイヤのターゲット分離 — 移動+委譲のみ、単独)、WP31/32/36/38、入力 I2〜I4 | 着手前に詳細登録 |
| 18 | WP44, WP46 | 完了(2026-07-07) |
| 19 | WP47, WP48 | 完了(2026-07-07)。専有: WP47 = phys/scene バインダ/debugdraw、WP48 = renderer/camera + rpcserver |
| 20 | WP31, WP49(I2), WP50(C2) | 並列 3 本。専有: WP31 = resources/features + シェーダ + renderingpass/pipelinefactory、WP49 = communication/jsonrpc + 入力注入(os/input)、WP50 = userpublic システム + renderer/camera。**3 本とも golden ケースを追加するため件数 REQUIRE は統合時に調整(各自は自分の追加分のみ数える)**。共有は test/CMakeLists.txt 追記のみ |
| 21 | WP51(音), WP52(シーン遷移), WP53(乱数), WP54(debug_text) | 並列 4 本。専有: WP51 = core/audio 新設 + ルート CMakeLists FetchContent、WP52 = loader/scene + GameObjects 破棄経路、WP53 = 乱数(新規ファイル)、WP54 = features/レンダラ。**gamecontext.{hpp,cpp} は 4 本全部が追記する — 各自ファイル末尾に足し、統合時に調整**。rpcserver は WP52/53 が両方 get_status を触る(小競合予定)。golden 追加は WP54 のみ |
| 22 | WP55([PF] v6.3 リゾルバ), WP56(イベント E1), WP57(init 雛形), WP58(マテリアル M1) | 並列 4 本。専有: WP55 = loader/pathresolver、WP56 = userpublic イベント + gamecontext(単独)、WP57 = devcli、WP58 = src/project 新規 + docs/shader_contract.md。**rpcserver は WP55(get_status stores)と WP56(inject_event)が交差 — 追記形で書き統合時に調整**。golden 追加なし |

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
