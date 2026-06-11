# ヘッドレス描画 設計

対象読者: レンダラー担当(実装ガイド)

ステータス: ドラフト

前提: ロードマップ(`design_roadmap_renderworld.md` §2)の本線 1。すべての後続項目(シェーダテスト、compute、計測 CLI、RT 検証)の検証基盤になる。

## 1. 目的

- ウィンドウ/スワップチェーンなしでエンジンを起動し、1〜N フレーム描画して PNG に出力できるようにする
- `ctest` に実描画テストを追加可能にする
- 副目標: リサイズ時に extent_scale 依存の RT 群が追従しない問題(§2.3)の修正

## 2. 現状分析

### 2.1 スワップチェーン結合点

| # | 結合点 | 内容 |
|---|--------|------|
| C1 | `VulkanManageCore` ctor (core.cpp) | instance 拡張を `Window.getRequiredVulkanInstanceExts()` から取得、surface を Window から作成、`pickPhysicalDevice` / `pickQueues` が surface サポートを必須条件にしている |
| C2 | `RenderTarget` モジュール (rendertarget.cpp) | swapchain 作成、acquire/present、セマフォ同期、depth 画像の所有。`render_begin()/render_end()` が swapchain 前提 |
| C3 | `Loop::run` (loop.cpp) | `Window.process()` をループ継続条件にしている |
| C4 | `getSwapchainFormat()` 参照 | `renderingpassruntimecompiler.cpp`(fullscreen の color format 解決)と `renderer.cpp`(`RenderPassDispatchDependencies.swapchain_color_format`) |
| C5 | 特別 RT id `swapchainRenderTargetId()` | パス JSON の出力先 "swapchain" が swapchain image view に解決される(rendertargetimageviewresolver 経由) |

### 2.2 既にある良い構造(維持する)

- `FrameRenderContext`(cmd_buf、color/depth attachment view、extent、semaphore、required_layout)が描画実行側とプレゼン側の seam として既に機能している。**この型を境界として維持**し、パス実行側(`RenderPassExecutor` 以下)は一切変更しない
- swapchain 再生成(`recreateSurfaceDependants`)は acquire/present の OutOfDate/Suboptimal に対して実装済み

### 2.3 既知の問題(本作業で修正)

- `recreateSurfaceDependants()` は swapchain と depth を再生成し `Camera.setScreenSize` を更新するが、`RenderTargetContainer` の RT 群(gbuffer 等、ロード時に extent_scale × swapchain extent で確定)と `FullscreenPassContainer` の入力 descriptor は**再生成されない**。リサイズ後は旧サイズの RT に描き続ける

## 3. 設計

### 3.1 方針

`RenderTarget` を「フレームターゲット戦略」の facade にし、内部実装を 2 つに分ける:

```
RenderTarget (モジュール名・公開APIは現状維持 + capture API 追加)
 ├─ SwapchainFrameTarget   … 現行実装の移設(acquire/present/再生成)
 └─ OffscreenFrameTarget   … 新規(オフスクリーン image、present なし)
```

- モジュール名と `render_begin()/render_end()` のシグネチャを維持することで、`renderer.cpp` 等の呼び出し側変更を最小化
- どちらを使うかは起動設定(§3.4)で決定。実行中の切替はしない

### 3.2 API スケッチ

```cpp
// vkcore/frametarget.hpp (新規)
struct FrameTargetCaps {
    vk::Format color_format;
    vk::Extent2D extent;
    bool presents;                    // false = headless
};

class IFrameTarget {
  public:
    virtual ~IFrameTarget() = default;
    virtual FrameRenderContext render_begin() = 0;
    virtual void render_end() = 0;
    virtual FrameTargetCaps caps() const = 0;
    // 直近に render_end したフレームの色画像を読み戻す(内部で fence 待ち)
    virtual std::vector<uint8_t> readbackLastFrameRGBA8() = 0;
};

// rendertarget.hpp (既存モジュールを facade 化)
DECLARE_MODULE(RenderTarget) {
    std::unique_ptr<IFrameTarget> impl;   // 起動設定から選択
  public:
    FrameRenderContext render_begin() { return impl->render_begin(); }
    void render_end() { impl->render_end(); }
    vk::Format getSwapchainFormat() const;   // 当面維持(caps().color_format を返す)
    bool presents() const;
    bool captureLastFrameToPng(const std::filesystem::path &path);  // stb_image_write 使用
};
```

`OffscreenFrameTarget` の要点:

- 色: RGBA8(`eB8G8R8A8Unorm` 互換挙動のため swizzle に注意)、usage = COLOR_ATTACHMENT | TRANSFER_SRC。深度は現行同様 D32
- in-flight 多重化は当初 1(同期単純化優先。性能は目的でないため)。`render_begin` は fence 待ちのみ、セマフォは `FrameRenderContext.image_prepared_semaphore = VK_NULL_HANDLE`
- `required_layout = eTransferSrcOptimal`(present 用 `ePresentSrcKHR` の代わり)。`render_end` のバリアもこれに揃える
- `cmdbuf.recordEndSubmit` は既定引数で空セマフォを許容済み(cmdbuf.hpp)のため変更不要

### 3.3 VulkanManageCore のヘッドレス対応(C1)

```cpp
struct EngineLaunchConfig {     // 新規・先頭で確定する純データモジュール
    bool headless = false;
    vk::Extent2D headless_extent{1280, 720};
    uint32_t headless_frames = 1;            // 0 = 無制限(将来のサーバモード用)
    std::optional<std::filesystem::path> render_out;  // PNG 出力先
};
```

- headless 時: instance 拡張は空(GLFW 拡張を要求しない)、surface 非作成、`pickQueues` は presentation サポート条件を外し `presentation_queue = graphic_queue` に縮退
- `GET_MODULE(Window)` を headless 経路で一切呼ばないこと(モジュールは遅延初期化なので、呼ばなければ GLFW は初期化されない)
- `getSurface()` は headless 時に呼ばれたら throw(誤用の早期検出)

### 3.4 Loop と CLI(C3)

```cpp
// Loop::run() を分岐
while (running) {
    if (launch.headless) { /* window.process() を呼ばない。frames 消化で終了 */ }
    else if (!window.process()) break;
    ecs.update();
    renderer.render();
    if (!launch.headless) framerate_adjuster.wait();   // headless は全速
}
// 終了時: render_out が指定されていれば captureLastFrameToPng
```

player の `main.cpp` に argparse(devcli で使用実績あり)を追加:

```
pelican_player --headless --frames 3 --size 1280x720 --render-out out.png
```

frames 既定 3 の理由: in-flight とリソース初期化の遅延を流してから最終フレームを capture するため。

### 3.5 swapchain format 参照の置換(C4, C5)

- `getSwapchainFormat()` の実体を `caps().color_format` にする。headless では offscreen 画像の format を返すため、呼び出し側(runtimecompiler、dispatch)は無変更で動く
- `swapchainRenderTargetId()` への出力は `render_pass_frame_setup.cpp` が `FrameRenderContext.color_attachment` を使って解決している(`RenderTargetImageViewResolver` は通常 RT 専用)。つまり OffscreenFrameTarget が `color_attachment` に offscreen view を入れるだけで解決され、**追加変更は不要**

### 3.6 リサイズ追従の修正(§2.3)

- `RenderTargetContainer` に `recreateForExtent(vk::Extent2D)` を追加(extent_scale 付きで登録された RT を再確保。ロード時に scale を `InternalRenderTarget` に保存しておく)
- `FullscreenPassContainer::setInputTextures` の再実行(入力 RT の view が変わるため)。compiled pass が input_targets を保持しているので再バインド可能
- `RenderTargetLayoutTracker` のリセット
- 呼び出し順: `recreateSurfaceDependants()` → 上記 3 つ。`device.waitIdle()` 中なので遅延破棄(本線 2)導入前でも安全
- 注: 本修正はウィンドウありモードの品質修正であり、headless とコードを共有する(`recreateForExtent` は将来 capture サイズ変更にも使う)

## 4. テスト計画

- `test/headless_render_test.cpp`: example 相当の最小シーンを `--headless` 経路で 3 フレーム描画 → readback → 「全ピクセル同一でない」「アルファ/クリア色が期待通り」程度の弱い検証から開始(ゴールデンイメージは本線 4 で導入)
- Vulkan デバイスがない CI 環境対策: デバイス列挙に失敗したらテストを SKIP 扱いにする(Catch2 の `SKIP()`)。将来 lavapipe/SwiftShader の導入を検討
- リサイズ追従(§3.6)は自動化困難のため、当面は手動確認手順を test/README に記載

## 5. 移行手順(各段で動作維持)

1. `EngineLaunchConfig` 導入 + player に argparse(まだ headless 未実装、フラグだけ受ける)
2. `IFrameTarget` 抽出: 現行 `RenderTarget` の実装を `SwapchainFrameTarget` に移設し facade 化(挙動無変更のリファクタ。ここで PR 1 つ)
3. `OffscreenFrameTarget` + `VulkanManageCore` の headless 分岐 + `Loop` 分岐 + readback/PNG(stb_image_write を FetchContent 追加)
4. `--render-out` 完成、headless テストを ctest に追加
5. リサイズ追従修正(§3.6)

## 6. 後続項目との接続

- ゴールデンイメージテスト(本線 4): `readbackLastFrameRGBA8()` と許容誤差比較を組むだけ
- 計測 CLI(本線 6): `--headless --profile out.json` の組み合わせで完成形
- コマンド層(並行 B): headless 無制限モード(`headless_frames = 0`)+ コマンド受信ループがサーバモードになる
- パス単位ダンプ(将来): `readback` を任意 RT に一般化すれば `--capture-pass gbuffer_albedo` が作れる
- 連番出力(外部ツール連携): `--render-out out/%04d.png` のようにパスに `%d` 系を含む場合は毎フレーム capture する。仮想時刻(`set_time`/`step_frame`)と組み合わせて動画用オフラインレンダラーになる(`docs/external_tools_requirements.md` 参照)
