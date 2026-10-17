# macOS 対応の実証: MoltenVK で Pelican を動かす

対象読者: エンジン担当。Mac を持っている人。
ステータス: 実証計画(2026-10-17。決定: 第 2 backend は書かない。mac は MoltenVK で動かす)。
前提: [design_plan_layer_inventory.md](design_plan_layer_inventory.md) 2-1(`execution_plan` の削除)、
[design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md)(兄弟 backend の設計。当面作らない)、
LX-1(Linux ビルドの修正。`claude/gallant-bohr-1mm9f3`)。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- エンジンが device に**必須**としているのは Vulkan 1.3 と 4 つの feature(`multiDrawIndirect` / `drawIndirectFirstInstance` /
  `shaderDrawParameters` / `dynamicRendering`)だけで、MoltenVK 1.3.0(2025-04)以降はすべて満たす。
  ray query は任意で、無ければ RT 系 feature が落ちるだけ。
- コードには mac の準備が**既に一部ある**: instance の portability enumeration(`core.cpp:125-128, 152-155, 208-210` の `__APPLE__`)、
  player の `-force_load`(`src/player/CMakeLists.txt:26`)。無いのは `CMakeLists.txt:27` の platform 判定と、Linux で踏んだ
  MSVC 依存の修正(LX-1 で済み)。
- 実証の目標は「Apple silicon の Mac で `pelican_player --headless --render-out` が example を描き、次に窓、次に Studio」。
  決めるのは **mac を対応対象にするか**であって、Metal backend を書くかではない(書かない)。

## 1. 要求と MoltenVK の対応

| エンジンの要求 | 種別 | 場所 | MoltenVK |
|---|---|---|---|
| device の `apiVersion >= 1.3`(満たさないと throw) | 必須 | [`core.cpp:569`](../src/core/vkcore/core.cpp#L569) | 1.3.0 で Vulkan 1.3、1.4.0 で 1.4。最新 1.4.2 |
| `multiDrawIndirect` / `drawIndirectFirstInstance` / `shaderDrawParameters` / `dynamicRendering` | 必須 | [`core.cpp:405-420`](../src/core/vkcore/core.cpp#L405) `required` | 対応 |
| instance の `eEnumeratePortabilityKHR` + `VK_KHR_portability_enumeration` | 必須(mac) | `core.cpp:125-128` ほか、`__APPLE__` | 既にある |
| `timelineSemaphore` / `multiview` / descriptor indexing(`runtimeDescriptorArray`、`descriptorBindingPartiallyBound`)| 任意 | `core.cpp:417-454` | 対応(descriptor indexing は Apple silicon の Tier 2 argument buffer。Intel Mac は Tier 1 で上限が低い)|
| `VK_KHR_dynamic_rendering_local_read` | 任意 | `core.cpp:482` | 1.4.0 で追加 |
| `VK_EXT_swapchain_maintenance1` / `VK_EXT_memory_budget` | 任意 | `core.cpp:495, 588` | 1.2.3 で追加 / 対応 |
| ray query / acceleration structure / RT pipeline | 任意(`ray_query_selection`) | `core.cpp:351-440, 589` | 未対応と思われる(要確認)。無ければ RT 系 feature と `ray_query` を使う graph node が名前付きで落ちる |
| timestamp query(GPU 時間) | 任意 | render_timing | 対応。精度は Metal の counter sample に依る |
| シェーダの拡張 | | `GL_EXT_multiview`、ray tracing 系、include だけ。geometry / tessellation 無し | SPIRV-Cross → MSL で問題になる機能を使っていない |
| 禁止事項(portability subset) | | VkEvent、triangle fan、logicOp、wide line、3 ch の RT、D24S8 の RT は使っていない。depth は D32 | |
| `eMutableFormat` + format list | 要確認 | [`core.cpp:1422`](../src/core/vkcore/core.cpp#L1422) | sRGB ↔ UNORM の再解釈なら対応。それ以外の組は `imageViewFormatReinterpretation` の制約に当たる |
| blend の constant alpha | 要確認 | pipelinefactory | portability subset で `constantAlphaColorBlendFactors` が無いことがある |
| RT の alias(`eAlias` + VMA の aliasing image) | | [`rendertargetcontainer.cpp:307`](../src/core/renderingpass/rendertargetcontainer.cpp#L307) | 1.3.0 から MTLHeap が既定で有効(AMD 除く)なので実メモリも共有。Intel Mac の AMD GPU では正しく動くが節約されない |
| `eLazilyAllocated` / `eTransientAttachment`(transient 候補)、tile_local 候補 | | [`rendertargetcontainer.cpp:297`](../src/core/renderingpass/rendertargetcontainer.cpp#L297)、`targetrenderplanning` | Apple GPU は tile ベース。memoryless texture に落ち、**desktop で選ばれなかった候補がここで初めて選ばれる**。tile_local は local_read(1.4.0)前提 |

## 2. ビルドの移植

| 項目 | 状態 | やること |
|---|---|---|
| `CMakeLists.txt:27` `if(WIN32)` → `PLATFORM_DESKTOP` | LX-1 で Linux を追加済み | `OR APPLE` を足す(`claude/gallant-bohr-1mm9f3` に積んだ) |
| MSVC 依存(`template` キーワード、`OrderedJson` の暗黙変換、inline namespace、`std::array` の CTAD、`<csignal>` / `<cstddef>` など 18 ファイル) | LX-1 で修正済み(GCC)。clang も同じ系統 | `develop` 先頭に cherry-pick 済み。mac では clang で再確認 |
| Windows 専用コード(`_WIN32` 47 ファイル) | Linux でリンクできたので非 Windows 経路はある | file watcher(polling?)、`processrunner`、OpenXR、RenderDoc は mac で動作確認 |
| Vulkan SDK | `find_package(Vulkan)`、1.4.309 以上 | Vulkan SDK for macOS(MoltenVK 同梱)。`VULKAN_SDK` を通す |
| GLFW | FetchContent | mac では `VK_EXT_metal_surface` を要求してくれる。追加作業なし |
| shaderc(`PELICAN_RUNTIME_SHADER_COMPILER`) | SDK 依存 | SDK の shaderc を使う。無ければ OFF で事前コンパイル |
| Studio(Qt 6.10) | | Qt の mac 版。`studio_cpu` の scenario をそのまま走らせる |
| OpenXR | `PELICAN_WITH_OPENXR` 任意 | OFF |

## 3. 実証の段階と合否

| 段 | 内容 | 合否 |
|---|---|---|
| 0 | Apple silicon の Mac で configure + `pelican_core` / `pelican_player` のビルド(clang) | リンクまで通る。GCC で直した 18 ファイルに clang 固有の追加修正がどれだけ要るかを記録 |
| 1 | `pelican_player --headless --render-out` で example | 画像が出る。`get_status` の device facts(`core.cpp` の `required` / 任意の一覧)と選ばれた feature を保存。ray query 無しで落ちる feature の一覧を記録 |
| 2 | 窓あり(GLFW + `VK_EXT_metal_surface`)で example | present が回る。present mode(mailbox が無ければ FIFO)、resize、`VK_EXT_swapchain_maintenance1` の有無 |
| 3 | golden 画像の比較 | Windows の golden と diff。精度・blend・sRGB の差は mac 用 baseline を別に持つ。構造的な差(描かれない pass)はバグとして追う |
| 4 | `get_frame_plan` の物理計画 | transient / tile_local 候補が選ばれるかを確認(棚卸しメモ 3-2 / 3-3 の検証)。選ばれたら validation layer と `hazard_stress` を回す |
| 5 | Studio(`studio_cpu`) | 起動と scenario の通過数。Qt の差分 |

## 4. 既知の制約と対処

- **ray tracing**: MoltenVK に acceleration structure が無い(要確認)。RT 系 feature と `ray_query` の graph node は mac では使えない、
  と明記する。Metal の RT を使うには MoltenVK 側の対応待ち。
- **Intel Mac**: descriptor indexing が Tier 1、MTLHeap が AMD で無効。Apple silicon だけを対象にする(Blender も 4.5 で Intel Mac を落とした)。
- **GPU 時間**: timestamp の意味が Metal の counter sample になる。表示はそのまま、数値の比較は同じ機種内で。
- **validation layer**: MoltenVK は layer を自分では読まない。SDK の loader 経由で入れる。
- **性能**: SPIRV-Cross の MSL 変換は pipeline 作成時。shader の数が多いと初回が重い。pipeline cache を効かせる。

## 5. 未決

- mac を**対応対象**(CI に gate を持つ)にするか、**動くことがある**止まりにするか。CI に mac runner を持つなら GPU gate は Apple silicon の self-hosted になる。
- RT 無しの mac で example をどう見せるか(RT 系 feature を preset から外した mac 用 preset を持つか、落ちた feature を Studio の Problems に出すだけにするか)。
- `eMutableFormat` の format list が sRGB ↔ UNORM 以外の組を持つか(確認してから)。
