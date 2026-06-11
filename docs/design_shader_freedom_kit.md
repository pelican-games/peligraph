# シェーダ自由化キット 詳細設計

対象読者: レンダラー担当(実装ガイド)

ステータス: ドラフト

前提: ロードマップ上の位置づけは `design_roadmap_renderworld.md` §2 の本線 3。本線 1(ヘッドレス描画)と 2(遅延破棄)が完了していること。

## 1. 目的

「任意のシェーダを、C++ 側の配管工事なしで書いて・即実行して・自動検証できる」状態を作る。compute・RT を含む今後のすべてのシェーダ実装の土台。

## 2. 現状の問題点(コード根拠つき)

| # | 問題 | 現状コード |
|---|------|-----------|
| P1 | シェーダ追加にビルドが必要 | `src/core/resources/CMakeLists.txt` の `embed_shader()` が glslang CLI → .spv → battery-embed。シェーダ 1 本ごとに CMake 編集 + リビルド |
| P2 | descriptor layout / pipeline layout が手書き | `MaterialContainer` と `FullscreenPassContainer` がそれぞれ `descset_layouts` / `pipeline_layout` をコンストラクタでベタ書き構築 |
| P3 | パイプライン生成がコンテナごとに重複 | `MaterialContainer::pipelines`、`FullscreenPassContainer::registerFullscreenPass()` が各自 `vk::UniquePipeline` を生成・保持。`VkPipelineCache` 未使用 |
| P4 | push constant が固定構造体 | `PushConstantStruct { glm::mat4 mvp; }`(materialcontainer.hpp)がレイアウト唯一の形 |
| P5 | ShaderContainer に依存情報がない | `ShaderContainer` は `vk::UniqueShaderModule` の置き場のみ。どのパイプラインがどのシェーダを使うか追跡できず、リロード不可能 |
| P6 | リロード時の破棄安全性がない | in-flight フレームが参照中のパイプライン/モジュールを置換する手段がない(本線 2 の遅延破棄が前提になる理由) |

## 3. 全体構成

```
GLSL ファイル (+ #include)
   │  ShaderCompiler (shaderc)            … P1 解決
   ▼
SPIR-V + ShaderReflection (spirv-reflect) … P2 解決
   │  まとめて ShaderBundle
   ▼
ShaderLibrary (パス→Bundle、依存追跡、リロード通知) … P5 解決
   │
   ▼
PipelineFactory (Bundle + PipelineStateDesc → vk::Pipeline、VkPipelineCache) … P3 解決
   │  破棄は DeletionQueue 経由                  … P6 解決
   ▼
各パス種別 (material / fullscreen / ui / compute / rt)
```

新規モジュールはすべて `src/core/shader/` 配下に置く(`shadercompiler.*`、`shaderreflection.*`、`shaderlibrary.*`、`pipelinefactory.*`)。

## 4. API スケッチ

### 4.1 ShaderCompiler

```cpp
// shaderc をライブラリとしてリンク(FetchContent または Vulkan SDK 同梱)
struct ShaderCompileResult {
    std::vector<uint32_t> spirv;
    std::string log;                 // 警告含む
    bool ok;
};

struct ShaderCompileOptions {
    // GLSL: 省略可(ステージは拡張子 .vert/.frag/.comp/.rgen/.rmiss/.rchit から推定、エントリは main)
    // HLSL: stage と entry_point の指定が必須(1 ファイル複数エントリがありうるため)
    std::optional<vk::ShaderStageFlagBits> stage;
    std::string entry_point = "main";
};

DECLARE_MODULE(ShaderCompiler) {
  public:
    ShaderCompileResult compileFile(const std::filesystem::path &path,
                                    const ShaderCompileOptions &opts = {});
    ShaderCompileResult compileSource(std::string_view source, vk::ShaderStageFlagBits stage,
                                      std::string_view name);
    void addIncludeDir(const std::filesystem::path &dir);   // #include 解決
};
```

- `#include` は shaderc の includer で対応。共有定義は `shaders/include/` に置く(§6)
- 失敗時は throw せず result を返す(ホットリロードで編集途中の保存に耐えるため)

言語対応(GLSL + HLSL + Slang):

- 拡張子でバックエンドを振り分ける。GLSL 系 → shaderc、`.hlsl` → DXC(Vulkan SDK 同梱、SM6 対応)、`.slang` → slangc / Slang ライブラリ API。境界が SPIR-V なので ShaderBundle 以降は言語非依存
- HLSL の register space ↔ descriptor set 写像は「`spaceN` = set N、register shift なし」で固定(DXC デフォルト挙動に揃える)。これにより §5 の set 規約が両言語で同一になる
- Slang は `ParameterBlock<T>` が descriptor set に対応する。宣言順依存にせず明示 binding で §5 の規約に合わせる
- push constant は HLSL/Slang 側では `[[vk::push_constant]]` を使用
- エントリポイントは HLSL/Slang では複数・名前付きが標準のため `ShaderCompileOptions` で指定する
- リフレクションの正はあくまで SPIR-V(spirv-reflect)。Slang 固有のリフレクション API は使わない(全言語で同一経路を保つため)

### 4.2 ShaderReflection

```cpp
struct ReflectedBinding {
    uint32_t set, binding;
    vk::DescriptorType type;
    uint32_t count;                  // 配列。bindless は VARIABLE_COUNT 扱い
    vk::ShaderStageFlags stages;
    std::string name;
};

struct ShaderReflection {
    std::vector<ReflectedBinding> bindings;
    std::optional<vk::PushConstantRange> push_constant;
    // vertex stage のみ:
    std::vector<vk::VertexInputAttributeDescription> vertex_inputs;
    // compute のみ:
    glm::uvec3 local_size;
};

ShaderReflection reflect(std::span<const uint32_t> spirv);          // spirv-reflect 使用
ShaderReflection merge(std::span<const ShaderReflection> stages);   // vert+frag 等の統合
```

- `merge` は同一 (set, binding) の stage flags を OR、型不一致は throw
- ここから `vk::DescriptorSetLayoutCreateInfo` / `vk::PipelineLayoutCreateInfo` を機械生成するヘルパを併設

### 4.3 ShaderBundle / ShaderLibrary

```cpp
PELICAN_DEFINE_HANDLE(ShaderBundleId, int);

struct ShaderBundle {
    vk::UniqueShaderModule module;
    ShaderReflection reflection;
    std::filesystem::path source_path;   // 埋め込み由来なら空
    uint64_t version;                    // リロードごとに増加
};

DECLARE_MODULE(ShaderLibrary) {
  public:
    ShaderBundleId loadFromFile(const std::filesystem::path &path);
    ShaderBundleId loadFromSpirv(std::span<const uint32_t> spirv, std::string_view name); // 埋め込み標準シェーダ用
    const ShaderBundle &get(ShaderBundleId id) const;

    // ホットリロード: 再コンパイル成功時のみ差し替え、失敗時は旧版維持 + ログ
    bool reload(ShaderBundleId id);
    // パイプライン側が購読する。リロードで version が変わった Bundle を列挙
    std::vector<ShaderBundleId> takeDirtyBundles();
};
```

- 既存 `ShaderContainer` は当面残し、移行完了後に削除(§7)

### 4.4 PipelineFactory

```cpp
struct GraphicsPipelineDesc {
    ShaderBundleId vert, frag;
    std::vector<vk::Format> color_formats;       // dynamic rendering 前提
    vk::Format depth_format = vk::Format::eUndefined;
    // 必要最小限のステート。未指定はエンジン既定値
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    vk::CullModeFlags cull = vk::CullModeFlagBits::eBack;
    bool depth_test = true, depth_write = true;
    bool use_engine_vertex_layout = true;        // false ならリフレクション由来
};

struct ComputePipelineDesc { ShaderBundleId comp; };

PELICAN_DEFINE_HANDLE(PipelineHandle, int);

DECLARE_MODULE(PipelineFactory) {
  public:
    PipelineHandle create(const GraphicsPipelineDesc &desc);
    PipelineHandle create(const ComputePipelineDesc &desc);

    vk::Pipeline pipeline(PipelineHandle h) const;
    vk::PipelineLayout layout(PipelineHandle h) const;
    const ShaderReflection &reflection(PipelineHandle h) const;

    // ShaderLibrary の dirty 通知を受けて再生成。旧パイプラインは DeletionQueue へ
    void rebuildDirty();
};
```

- descriptor set layout は reflection から自動生成。同一レイアウトはハッシュでキャッシュ共有
- `VkPipelineCache` をディスク永続化(`build/` または実行ディレクトリ)
- パイプライン再生成はフレーム先頭の安全点で `rebuildDirty()` を呼ぶ 1 か所に限定

## 5. descriptor set スロット規約(全シェーダ共通・最重要の合意事項)

| set | 用途 | 更新頻度 | 管理者 |
|-----|------|----------|--------|
| 0 | フレーム共通(カメラ行列、時間、画面サイズ) | 毎フレーム 1 回 | レンダラー共通 |
| 1 | パス入力(前段 RT、storage image 等) | パス切替時 | パスシステム |
| 2 | マテリアル(テクスチャ、マテリアル定数) | ドロー単位 | MaterialContainer |
| 3 | 自由枠(実験用 / 将来の bindless テーブル) | 任意 | シェーダ作者 |

- 現行コードとの差分: 現在 `lightDescriptorSetNumber = 2`(materialrender.cpp)等、番号がアドホック。移行時にこの表へ揃える
- push constant は「先頭 64B = mvp 等のエンジン予約、以降 64B = シェーダ自由」を上限 128B で規約化(P4 の解消)
- この規約を `shaders/include/pelican_sets.glsl` に定数として書き、C++ 側は同名ヘッダで共有する(§6)

## 6. GLSL 共有ヘッダ規約

- `src/core/resources/shaders/include/` を標準 include dir とする
  - `pelican_sets.glsl` / `pelican_sets.hlsli`: set/binding(space)番号の define
  - `pelican_frame.glsl` / `pelican_frame.hlsli`: set0 の UBO 定義(カメラ、時間)
  - `pelican_vertex.glsl`: エンジン標準頂点レイアウト
- 共有ヘッダは言語ごとに用意する(GLSL と HLSL で構文互換がないため)。番号や構造体レイアウトの両言語間の一致は、リフレクション結果を比較する単体テストで担保する
- C++ とシェーダの構造体共有は「シェーダ側を正とし、C++ 側に同レイアウトの struct を置いて static_assert でサイズ検証」から始める(コード生成は将来課題)

## 7. 移行手順(各段で動作を保ったまま)

1. **shaderc + spirv-reflect の導入**(FetchContent 追加のみ。既存経路は無変更)
2. **ShaderCompiler / ShaderReflection 実装 + 単体テスト**: 既存の `default.vert` 等を実行時コンパイルし、生成 SPIR-V が embed 済み .spv と同等に動くこと、リフレクション結果が手書き layout と一致することを Catch2 で検証
3. **ShaderLibrary 導入**: `renderingpassruntimecompiler.cpp` の `registerShaderFromFile()` を ShaderLibrary 経由に置換(.spv 直読みに加えて .vert/.frag 直読みを許可)
4. **PipelineFactory 導入 + FullscreenPassContainer 移行**: `registerFullscreenPass()` 内のパイプライン生成を PipelineFactory 呼び出しに置換。fullscreen 系は構造が単純なので最初の移行対象に最適
5. **ホットリロード**: ファイルウォッチャ(`std::filesystem::last_write_time` ポーリングで開始、後で OS API 化)→ `ShaderLibrary::reload` → `PipelineFactory::rebuildDirty`。フレームループの安全点に組み込む
6. **MaterialContainer / UiRenderer 移行**: 手書き descset_layouts/pipeline を PipelineFactory + reflection 由来に置換。set 規約(§5)への番号揃えはここで実施
7. **embed_shader の縮小**: 標準シェーダ(default/fullscreen/ui)のみ埋め込みフォールバックとして残し、`loadFromSpirv` で ShaderLibrary に登録。`temp_effects` 系はファイルロードに移行
8. **ShaderContainer 削除**

各段の検証: ヘッドレス描画(本線 1)による PNG 比較。4 以降はゴールデンイメージテストを段ごとに追加。

## 8. テスト戦略

- リフレクション単体テスト: 代表シェーダ(UBO / sampler 配列 / push constant / storage image)ごとに期待 binding 列を検証
- コンパイラ単体テスト: include 解決、エラー時の log 内容、不正 GLSL で ok=false
- ゴールデンイメージ: `test/shaders/<name>/`(GLSL + 期待 PNG + 許容誤差)をディレクトリ走査して自動登録
- ホットリロード結合テスト: コンパイル失敗 → 旧パイプライン維持、成功 → version 増加を確認

## 9. 後続項目との接続

- **compute パス(本線 5)**: `ComputePipelineDesc` と reflection の `local_size` をそのまま使用。パス JSON の `"type": "compute"` 追加時、シェーダ側の追加工事は不要になっている
- **bindless(本線 7)**: set3 を VARIABLE_DESCRIPTOR_COUNT 化。reflection が配列 count を返すので layout 生成の変更は局所
- **RT(本線 8)**: ShaderCompiler のステージ推定に .rgen/.rmiss/.rchit を追加済みの想定。SBT 構築は PipelineFactory に `RayTracingPipelineDesc` を追加して対応
