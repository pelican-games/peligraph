# 実装指示書(コーディングエージェント向け)

対象読者: 実装を担当するコーディングエージェント。各 Work Package (WP) は独立に依頼できる単位として書かれている。

設計の正は以下の 3 文書。本書と矛盾したら設計文書を優先し、矛盾を発見したら作業を止めて報告すること。

- `docs/design_roadmap_renderworld.md` — 全体順序と ECS 境界
- `docs/design_headless_rendering.md` — 本線 1 の設計(以下 [HL])
- `docs/design_shader_freedom_kit.md` — 本線 3 の設計(以下 [SF])

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

### テスト規約

- Catch2 v3。`test/CMakeLists.txt` の `pelican_define_test(<name> [libs...])` で登録
- GPU 必須テストは Vulkan デバイス列挙失敗時に `SKIP()` すること

### 禁止事項

- `src/core/ecs/` 以下の変更(担当者が別。必要が生じたら止めて報告)
- 無関係箇所のリフォーマット・リネーム(diff を見る人間のため)
- 挙動変更とリファクタの同一コミット混在
- 1 WP = 1 ブランチ = 1 PR。ブランチ名は `agent/wp<番号>-<短い説明>`

## 1. WP 一覧と依存関係

| WP | 内容 | 依存 | 規模目安 |
|----|------|------|---------|
| 1 | EngineLaunchConfig + player CLI 引数 | なし | 小 |
| 2 | RenderTarget の facade 化(挙動無変更) | なし | 中 |
| 3 | OffscreenFrameTarget + headless 起動 + PNG 出力 | 1, 2 | 大 |
| 4 | headless 描画テストを ctest に追加 | 3 | 小 |
| 5 | リサイズ時の RT 追従修正 | 2 | 中 |
| 6 | 遅延破棄機構(DeletionQueue) | なし | 中 |
| 7 | shaderc / spirv-reflect / stb の FetchContent 導入 | なし | 小 |
| 8 | ShaderCompiler + ShaderReflection + 単体テスト | 7 | 大 |
| 9 | ShaderLibrary | 8 | 中 |
| 10 | PipelineFactory + fullscreen パス移行 | 6, 9 | 大 |
| 11 | シェーダホットリロード | 10 | 中 |
| 12 | MaterialContainer / UiRenderer 移行 + set 規約適用 | 10 | 大 |
| 13 | ゴールデンイメージテスト基盤 | 4, 8 | 中 |

WP1, 2, 6, 7 は並列依頼可能。以降の項目(compute、GPU 計測、RenderWorld、コマンド層、bindless、RT)は設計文書を先に書いてから WP 化する。

## 2. WP 詳細

### WP1: EngineLaunchConfig + player CLI 引数

参照: [HL] §3.3, §3.4

1. `src/core/config.hpp` の隣に `src/core/launchconfig.hpp` を新規作成。[HL] §3.3 の `EngineLaunchConfig` を**純データのモジュール**として定義(`DECLARE_MODULE`、メンバ public でよい)
2. `src/player/main.cpp` に argparse を導入(devcli/main.cpp に使用例あり)。受けるオプション: `--headless`(flag)、`--frames N`(既定 3)、`--size WxH`(既定 1280x720)、`--render-out <path>`
3. parse 結果を `GET_MODULE(EngineLaunchConfig)` に書き込んでから `PelicanCore::run()` を呼ぶ。**この WP では値は誰も読まない**(配線のみ)
4. `--help` が動くこと、引数なし起動が従来と同一挙動であることを確認

受け入れ基準: 引数なしで従来通り起動。`--headless` 指定時も(まだ未実装なので)従来挙動でよいが、`LOG_INFO` で「headless requested but not implemented yet」を出す。

### WP2: RenderTarget の facade 化(挙動無変更リファクタ)

参照: [HL] §3.1, §3.2

1. `src/core/vkcore/frametarget.hpp` を新規作成し、`IFrameTarget` と `FrameTargetCaps` を定義([HL] §3.2 のシグネチャに従う。`readbackLastFrameRGBA8` は宣言のみで `SwapchainFrameTarget` では `throw`)
2. 現 `RenderTarget` の実装(swapchain 作成、acquire/present、`recreateSurfaceDependants` 等)を `src/core/vkcore/swapchainframetarget.{hpp,cpp}` の `SwapchainFrameTarget : IFrameTarget` へ**移動**(コピーでなく移動。ロジック変更禁止)
3. `RenderTarget` モジュールは `std::unique_ptr<IFrameTarget> impl` を持つ facade にし、既存公開 API(`render_begin` / `render_end` / `getSwapchainFormat`)を委譲で維持。コンストラクタでは常に `SwapchainFrameTarget` を生成(headless 分岐は WP3)
4. `FrameRenderContext` と `in_flight_frames_num` の定義は rendertarget.hpp に残す(include している他ファイルを壊さないため)

受け入れ基準: 全呼び出し側(renderer.cpp 等)が無変更でビルドが通る。player 起動・リサイズ・終了が従来通り。diff が「移動 + 薄い委譲」だけであること。

### WP3: OffscreenFrameTarget + headless 起動 + PNG 出力

参照: [HL] §3.2〜§3.5(設計の詳細はすべてそちら)

1. `src/core/vkcore/offscreenframetarget.{hpp,cpp}` に `OffscreenFrameTarget` を実装。要点は [HL] §3.2(in-flight 1、セマフォなし、`required_layout = eTransferSrcOptimal`、color usage に TRANSFER_SRC)
2. `readbackLastFrameRGBA8()`: fence 待ち → `copyImageToBuffer` で host visible staging へ → memcpy。`VulkanUtils`(vkcore/util.cpp)の staging 実装を参考に
3. `VulkanManageCore` の headless 分岐([HL] §3.3): `EngineLaunchConfig.headless` 参照、GLFW 拡張・surface・present 要件を外す。`getSurface()` は headless 時 throw。**`GET_MODULE(Window)` が headless 経路で一度も呼ばれないこと**(呼ぶと GLFW が初期化されてしまう)
4. `RenderTarget` facade のコンストラクタで headless なら `OffscreenFrameTarget` を選択
5. `Loop::run` の分岐([HL] §3.4): headless 時は `window.process()` と `framerate_adjuster.wait()` を呼ばず、`frames` 消化で終了
6. PNG 出力: stb_image_write(WP7 で導入済み)で `captureLastFrameToPng` を実装。format が BGRA の場合は R/B スワップ
7. 終了経路: `PelicanCore::run` 終了時に `render_out` 指定があれば capture

受け入れ基準: `pelican_player --headless --frames 3 --render-out out.png` が GUI なしで終了コード 0、妥当な PNG(例: example シーンで非単色)を出力。通常起動は無変更挙動。validation layer エラーなし。

### WP4: headless 描画テスト

参照: [HL] §4

1. `test/headless_render_test.cpp` を新規作成、`pelican_define_test(headless_render_test pelican_core)` で登録
2. テスト内容: `EngineLaunchConfig` を headless に設定 → 最小構成で 3 フレーム描画 → `readbackLastFrameRGBA8` → (a) サイズが extent と一致 (b) 全ピクセルが同一値でない、を assert
3. Vulkan デバイス取得失敗(インスタンス作成 throw 含む)は `SKIP("no vulkan device")`
4. シーン構築はプレイヤーの resources を流用せず、テスト内で最小限に(空シーン + クリア色でも可。その場合 (b) は「クリア色と一致」に置き換え)

受け入れ基準: GPU ありで PASS、GPU なし環境で SKIP。ctest 一覧に表示される。

### WP5: リサイズ時の RT 追従修正

参照: [HL] §2.3, §3.6

1. `rendertargetjsonparser` が読んだ `extent_scale` を `RenderTargetContainer::InternalRenderTarget` まで保存する(現在は `rendertargetconfigregistration.cpp` で extent 計算後に捨てている)
2. `RenderTargetContainer::recreateForExtent(vk::Extent2D base_extent)` を追加: scale 付き RT を再確保、`name_to_id` と ID は維持(image/view のみ差し替え)
3. `SwapchainFrameTarget::recreateSurfaceDependants()` から、(a) `recreateForExtent` (b) compiled pass の `input_targets` 再バインド(`FullscreenPassContainer::setInputTextures` 再実行) (c) `RenderTargetLayoutTracker` リセット、を呼ぶ。すべて `device.waitIdle()` 後
4. (b) のために `RenderingPassContainer` から compiled pass の列挙手段が必要。なければ最小の getter を追加

受け入れ基準: ウィンドウをリサイズしても描画が壊れない(拡大して引き伸ばしボケがないこと、validation エラーなし)。手動確認手順を PR 説明に記載。

### WP6: 遅延破棄機構(DeletionQueue)

参照: ロードマップ本線 2。設計文書がないため本節が仕様。

1. `src/core/vkcore/deletionqueue.{hpp,cpp}` を新規作成:

```cpp
DECLARE_MODULE(DeletionQueue) {
    uint64_t current_frame = 0;
    // (破棄予定フレーム, 任意リソースを抱えた deleter)
    std::vector<std::pair<uint64_t, std::function<void()>>> pending;
  public:
    // 任意の move-only リソースを抱えて遅延破棄。例: defer(std::move(old_pipeline));
    template <class T> void defer(T &&resource);
    void beginFrame();   // current_frame++ し、(current_frame - in_flight_frames_num) 以前を実行
    void flushAll();     // waitIdle 後の全破棄(終了時・swapchain 再生成時)
};
```

2. `Renderer::render()` の先頭で `beginFrame()`、`FastModuleContainer` 破棄順の関係で `VulkanManageCore` より後に初期化されることを確認(GET_MODULE の初出順で決まる)
3. 終了時とswapchain 再生成時は `flushAll()`
4. 単体テスト: フレームを進めて in_flight 数経過後に deleter が呼ばれること、`flushAll` で全部呼ばれることを、カウンタを抱えた fake リソースで検証(GPU 不要)

受け入れ基準: テストグリーン。既存コードへの組み込みはこの WP ではしない(利用開始は WP10 以降)。

### WP7: 外部ライブラリ導入(shaderc / spirv-reflect / stb)

参照: [SF] §3、[HL] §3.2

1. ルート CMakeLists.txt に FetchContent 追加: shaderc(`google/shaderc`、SKIP_TESTS/EXAMPLES を ON)、spirv-reflect(`KhronosGroup/SPIRV-Reflect`、static library 設定)、stb(ヘッダオンリー、`stb_image_write.h` のみ使用)
2. shaderc は依存(glslang, SPIRV-Tools)ごと取得されるためビルド時間が大きく増える。Vulkan SDK 同梱の `shaderc_combined` をまず探し(`find_library`)、見つかればそれを使い、なければ FetchContent にフォールバックする構成を推奨
3. リンク確認用の最小コード(`shaderc::Compiler` の生成、`spvReflectCreateShaderModule` の空呼び出し)を一時テストとして追加し、ビルドが通ることを確認してから削除(または WP8 のテスト雛形として残す)

受け入れ基準: クリーンビルドが Windows/MSVC で通る。ビルド時間の増分を PR 説明に記載。

### WP8: ShaderCompiler + ShaderReflection

参照: [SF] §4.1, §4.2(API はそちらが正)

1. `src/core/shader/shadercompiler.{hpp,cpp}`: [SF] §4.1 の API。GLSL のみ実装(HLSL/DXC、Slang は後続 WP。ただし `ShaderCompileOptions` の形は [SF] 通りに最初から入れる)。ステージ推定は拡張子テーブルで
2. `src/core/shader/shaderreflection.{hpp,cpp}`: [SF] §4.2 の API + descriptor set layout / pipeline layout 生成ヘルパ
3. 単体テスト `test/shader_compile_test.cpp`(GPU 不要):
   - `src/core/resources/default.vert` 等の実在シェーダをコンパイルして ok=true、SPIR-V マジックナンバー確認
   - 不正 GLSL で ok=false かつ log 非空
   - include 解決(テスト用 include ファイルを test/data/ に置く)
   - リフレクション: default.vert/frag の binding 列が現行手書き layout(materialcontainer.cpp 参照)と一致することを期待値で固定
4. この WP では**既存の描画経路に手を入れない**

受け入れ基準: テストグリーン。既存挙動無変更。

### WP9: ShaderLibrary

参照: [SF] §4.3

1. `src/core/shader/shaderlibrary.{hpp,cpp}`: [SF] §4.3 の API。`loadFromFile` は拡張子で「.spv = 直読み」「それ以外 = ShaderCompiler 経由」
2. `renderingpassruntimecompiler.cpp` の `registerShaderFromFile()` を ShaderLibrary 経由に置換(これにより パス JSON の `*_shader_path` に .vert/.frag を直接書けるようになる)
3. `reload()` は実装するが呼び出し元はまだない(WP11)。単体テストで version 増加と失敗時の旧版維持を検証
4. 既存 `ShaderContainer` は削除しない(WP13 まで併存)

受け入れ基準: 既存 JSON 設定(.spv 指定)が無変更で動く。.frag 直指定の動作確認を 1 ケース追加。

### WP10: PipelineFactory + fullscreen パス移行

参照: [SF] §4.4, §5, §7-4

1. `src/core/shader/pipelinefactory.{hpp,cpp}`: [SF] §4.4 の API。descriptor set layout はリフレクションから自動生成、同一レイアウトのハッシュ共有、`VkPipelineCache` のディスク永続化(パスは実行ファイル隣 `pipeline_cache.bin`)
2. 旧パイプラインの破棄は `DeletionQueue::defer`(WP6)経由
3. `FullscreenPassContainer::registerFullscreenPass` の内部を PipelineFactory 呼び出しに置換。既存の手書き descset_layouts と整合しない場合は**リフレクション由来を正**とし、シェーダ側の binding が [SF] §5 の set 規約とずれている箇所はシェーダを修正
4. ヘッドレス描画(WP3)+既存 example で全 fullscreen パス(bloom 系、ssao 等)の出力が移行前と一致することを目視 + PNG 比較で確認

受け入れ基準: 描画結果が移行前と一致(PNG 比較)。`rebuildDirty()` は空実装でよい(WP11 で完成)。

### WP11: シェーダホットリロード

参照: [SF] §7-5

1. `std::filesystem::last_write_time` のポーリング(1 秒間隔、`ShaderLibrary` 内のソースパス一覧を走査)で変更検出。専用スレッドは立てず、フレームループから呼ぶ
2. フレーム先頭の安全点(`Renderer::render()` 冒頭、`DeletionQueue::beginFrame()` の後)で: dirty 検出 → `ShaderLibrary::reload`(失敗時はログのみで旧版続行)→ `PipelineFactory::rebuildDirty`
3. `EngineLaunchConfig` に `bool shader_hot_reload`(既定: devstudio/通常起動 true、headless false)を追加
4. 結合テスト: テスト内でシェーダファイルを書き換え → reload → version 増加と新パイプライン生成を確認

受け入れ基準: player 起動中に .frag を編集保存すると次フレーム以降に反映される。壊れた GLSL を保存しても落ちない。

### WP12: MaterialContainer / UiRenderer 移行 + set 規約適用

参照: [SF] §5, §7-6

1. `default.vert/frag`、`ui.vert/frag`、ライト UBO の set/binding を [SF] §5 の規約表に揃える(`lightDescriptorSetNumber = 2` の付け替え等)。`shaders/include/pelican_sets.glsl` を新設し、全シェーダがそれを include する形に
2. `MaterialContainer` / `UiRenderer` の手書き descset_layouts・pipeline 生成を PipelineFactory + リフレクション由来に置換
3. push constant を [SF] §5 の規約(先頭 64B エンジン予約 / 後半 64B 自由)に整理
4. PNG 比較で移行前後の一致確認

受け入れ基準: 描画結果一致。`src/core` から `vk::DescriptorSetLayoutCreateInfo` の手書き構築が material/fullscreen/ui 経路から消えている。

### WP13: ゴールデンイメージテスト基盤

参照: [SF] §8、[HL] §6

1. `test/golden/`: `test/golden/<case>/`(シェーダ + scene.json + expected.png + tolerance)をディレクトリ走査して Catch2 の動的テストケースとして登録
2. 比較は per-pixel 差の平均と最大で判定(しきい値は case ごとの tolerance ファイル)。失敗時は actual.png と diff.png を `build/test_artifacts/<case>/` に出力
3. 最初のケースとして「クリア色のみ」「default マテリアルの三角形 1 枚」「fullscreen パス 1 段」の 3 つを追加
4. 期待画像の再生成スクリプト(`--update-golden` 相当)を用意

受け入れ基準: 3 ケースが GPU あり環境で PASS、GPU なしで SKIP。意図的にシェーダを壊すと FAIL し diff.png が出る。

## 3. 依頼時のテンプレート

エージェントへの依頼文は以下を含めること:

```
リポジトリ: pelican2 / ブランチ: main から agent/wpN-xxx を作成
タスク: docs/implementation_plan.md の WPN を実装してください。
設計文書 docs/design_*.md の該当節を必ず読むこと。
完了条件: WPN の受け入れ基準 + §0 の共通規則。
逸脱・不明点があれば実装せずに質問すること。
```
