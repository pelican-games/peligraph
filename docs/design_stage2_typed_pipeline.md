# 段階 2 の強化: typed な論理 graph を core の入力にし、JSON の二重 parse を解く

対象読者: エンジン担当(描画コンパイラ / renderer)。
ステータス: 方針決定(2026-10-17。「段階 2 の強化と二重経路のリファクタは行う」)。WP 化・レビュー前。
前提: [design_plan_layer_inventory.md](design_plan_layer_inventory.md) 2-6、
[design_render_preset_feature_recipe.md](design_render_preset_feature_recipe.md)(compose の出力を触る WP と同時期)、
[design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md) §2(`CompiledRenderPipeline::logical_graph` が render dialect fragment)。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- 段階 2(`compileRenderPipeline`、[`renderpipeline.cpp:2340`](../src/project/renderpipeline.cpp#L2340))の出力
  [`CompiledRenderPipeline`](../src/project/renderpipeline.hpp#L600) には、policy のほかに **typed な論理 graph `logical_graph`**
  ([`logicalrendergraph.hpp`](../src/project/logicalrendergraph.hpp) 280 行 + `.cpp` 929 行)が既にある。
- ところが core は **同じ resolved JSON をもう一度読んで** `PassDefinition`、`FrameGraphNodeDefinition`、`RenderTargetDefinition` を作る。
  node の構造が project と core の 2 系統の型で存在し、一致は「同じ JSON を読んだから」で担保されている。
- 方針: core の `FrameGraphDefinition` を `logical_graph` から作り、pass 定義の backend 非依存部分を project の型にし、
  core には「論理 → Vulkan 列挙の写像」と物理計画だけを残す。**実行時の経路は触らない**(registration / hot reload の CPU 段だけ)。
- 効果: 写しの排除(棚卸しメモ 規則 2)、GPU 無しの test が `logical_graph` → `planFrameGraph` まで型で届く、
  段階 2 の出力の canonical hash で hot reload の incremental prepare。性能低下は無い(毎フレームの経路は prepare 済みの handle を読むだけ)。

## 1. 現状の二重経路

```text
resolved JSON(段階 1 の出力)
   ├─ 段階 2  compileRenderPipeline → CompiledRenderPipeline { policy…, logical_graph }   … project(純ロジック)
   │                                        └─ 消費: targetplanning(project)、vulkanrendercompilerprogram.cpp:1213、renderingsamplecount.cpp:644
   └─ core   parseRenderingPassDefinitionsFromConfigJson → PassDefinition[]             … renderingpassconfigregistration.cpp:478-620
             parsePassDefinitionFromJson(passdefinitionjsonparser.cpp 289 行)
             parseRenderTargetFromJson   (rendertargetjsonparser.cpp 338 行、vk::Format / usage)
             parseRenderNodeFromJson / parseComputeNodeFromJson(frameplanner.cpp:669 / :858)
             makeRenderNodeDefinition(PassDefinition)(frameplanner.cpp:1196)→ FrameGraphDefinition → planFrameGraph(:1858)
```

- registration の注記([`renderingpassconfigregistration.cpp`](../src/core/renderingpass/renderingpassconfigregistration.cpp#L478) 付近):
  「normalized JSON は CPU 段の後は要らない。compiled definitions から再構築する」。JSON を持ち続ける理由が二重 parse そのもの。
- `frameplanner.cpp` 2,473 行のうち 669〜1,343 行が parse。planning(`planFrameGraph`)は後半で、本件では触らない。

## 2. 型の対応

| core が JSON から作るもの | 段階 2 に既にあるもの | 足りないもの / やること |
|---|---|---|
| [`FrameGraphNodeDefinition`](../src/core/renderingpass/frameplanner.hpp#L83): name / kind / declaration_index / reads / reads_history / read_footprints / local_read_shader_inputs / resource_accesses / writes / after / before / region_tags / attachments / snapshot_after / byte_size / raster_geometry / ray_query / resolution_domain / material_filter / material_variant / view_family / view_select / once | [`LogicalGraphNode`](../src/project/logicalrendergraph.hpp#L163): name / kind / declaration_index / ports / uses / after / before / region_tags / view_family / view_select | `LogicalGraphNode` に attachment の論理 op、snapshot_after、byte_size、material_filter / variant、once、ray_query、resolution_domain を足す(`uses` と `ports` で reads / writes / footprints は表せる)。`makeFrameGraphDefinition(const CompiledLogicalRenderGraph &)` を書き、`parseRenderNodeFromJson` / `parseComputeNodeFromJson` / `makeRenderNodeDefinition(PassDefinition)` を消す |
| [`PassDefinition`](../src/core/renderingpass/renderingpass.hpp#L474): shader 参照、`pass_info`(material / fullscreen / generic raster / debug 系 / shadow / velocity の variant)、入力 target と設定、implementation selection、`vk::AttachmentLoadOp` / `StoreOp`、`vk::SampleCountFlagBits`、clear、once | 散在: `PassAttachmentOperations`、`SampleCountPolicy`、`DeclaredShaderReference`、`PassImplementationSelection` | project に `CompiledPassDefinition`(論理 op、論理 sample count、shader 参照、pass_info、入力)を置く。core は `toVk*` の写像関数だけ。renderer の使用箇所は `using PassDefinition = ...` + 変換で名前を保つ |
| [`RenderTargetDefinition`](../src/core/renderingpass/rendertargetdefinition.hpp): `vk::Format`、`format_candidates`、`vk::ImageUsageFlags`、extent、history … | `LogicalResourceDesc`、format_class / format 文字列 | 論理側(format 文字列、extent 規則、history、usage の論理フラグ)は project。`vk::Format` の解決は device facts が要るので core に残す |
| 残す | `planFrameGraph`、`VulkanTargetPlan`、pipeline state、shader module、descriptor | 触らない |

## 3. 段階

| 段 | 内容 | 振り分け | 検証 |
|---|---|---|---|
| 0 | **橋の test**: 既存の JSON から `logical_graph` と `FrameGraphDefinition` を両方作り、field ごとの同値を assert する(移行中の安全網)。fixture は `test/fixtures/devstudio/example_frame_plan.json` の元 config と各 feature の golden config | v1 | 純ロジック test(GPU 不要) |
| 1 | `LogicalGraphNode` の拡張と `makeFrameGraphDefinition(logical_graph)`。core の `parseRenderNodeFromJson` / `parseComputeNodeFromJson` を削除。JSON → graph の parse は project の 1 箇所だけに(`frameplanner_test` が JSON fixture を使うので、project 側に JSON → `logical_graph` の入口は残す) | v1 | 段 0 の test、`example_frame_plan.json` の plan が byte 一致、golden 不変 |
| 2 | `CompiledPassDefinition` と render target の論理定義を project へ。`passdefinitionjsonparser.cpp` / `rendertargetjsonparser.cpp` を project へ移し、core は vk 写像だけ。registration から normalized JSON の保持を外す | v1 | 同上 + renderer の全 pass 種類(material / fullscreen / generic raster / debug / shadow / velocity)の headless |
| 3 | 段階 2 の出力に canonical hash(`logical_graph_fingerprint` を policy と pass 定義まで広げる)。registration が変わっていない rendering pass の prepare を省く | v1 | hot reload の時間計測(前後)、決定性 gate |

- 順番の前提: `develop` → `develop_v1` のマージ後。`vulkanrendercompilerprogram.cpp` / `framegraphruntime.cpp` に触るので、
  `execution_plan` の削除(棚卸しメモ 2-1)と同じ人が続けてやる。preset レシピ化で compose の出力を触る WP と同時期に置くと、段階 1 → 2 → core の受け渡しを 1 回で直せる。
- 段 0〜1 は Linux / mac のクラウド環境で進められる(pelican_project と frameplanner は GPU 不要)。段 2 の renderer 側確認は Windows。

## 4. 性能

- 触るのは registration / hot reload の CPU 段だけ。毎フレームの経路(`renderLogicalFrame` → `executePlannedFrameGraph`)は prepare 済みの
  `CompiledFrameGraphExecution` と handle を読むだけで、`PassDefinition` の parse にも JSON にも触らない。
- 減る側: JSON の 2 回読みが 1 回に、normalized JSON の保持が消える、段 3 で hot reload の再 prepare が減る。
- 規律: project の型は名前を `std::string` で持つ。フレーム内でそれを引く経路を新しく作らない(今の renderer は prepare 時に handle に解決している)。

## 5. やらないこと

- `planFrameGraph` のアルゴリズム変更、`get_frame_plan` の鍵の変更(本件では変えない)。
- Metal 向けの抽象(第 2 backend は書かない)。`CompiledPassDefinition` は Vulkan 1 本の前提で、論理 op の語彙は今の JSON と同じ。
- Studio の変更(Studio は `get_frame_plan` の JSON を読むので本件の影響を受けない)。

## 6. 未決

- `LogicalGraphNode` を拡張するか、別の `CompiledPassDefinition` に node の付随情報(attachment op、snapshot、once)を置いて `logical_graph` は依存関係だけに保つか。
  target planning が `logical_graph` を読むので、依存関係だけに保つ方が planner の入力が汚れない。別型を推す。
- `frameplanner_test` など JSON fixture を使う test の入口を project の JSON → `logical_graph` に付け替える範囲(全部か、代表だけか)。
- 段 3 の hash の粒度(rendering pass 単位か、pass 単位か)。
