# 実装指示書(コーディングエージェント向け)

> 完了済み WP の逐語記録は [implementation_archive.md](implementation_archive.md) を参照。本書は未完了 WP と共通・運用規則だけを扱う。

対象読者: 実装を担当するコーディングエージェント。各 Work Package (WP) は独立に依頼できる単位として書かれている。

設計の正は以下の文書。本書と矛盾したら設計文書を優先し、矛盾を発見したら作業を止めて報告すること。

- `docs/design_roadmap_renderworld.md` — 全体順序と ECS 境界
- `docs/design_headless_rendering.md` — ヘッドレス描画の設計(以下 [HL])
- `docs/design_shader_freedom_kit.md` — シェーダ基盤の設計(以下 [SF])
- `docs/design_project_format.md` — プロジェクト形式の設計(以下 [PF]。v6 凍結)
- `docs/design_project_format_web_profile.md` — 共通形式 Web プロファイル(以下 [PFW])
- `docs/design_render_pipeline_extensibility.md` — renderer の拡張境界と
  段階実装(以下 [RPE])
- `docs/design_render_graph_compiler.md` — logical type / target planning /
  Vulkan physical plan の詳細(以下 [RGC])
- `docs/design_heterogeneous_execution_graph.md` — typed dialect / domain partition /
  CPU・Vulkan sibling lowering / fragment・closed forest の詳細(以下 [HEG])
- `docs/design_wsi_epoch_recovery.md` — window surface / swapchain epoch、
  output facts、回復publicationとpresent lifetime(以下 [WSI])

改訂履歴: v2 で実装者レビューを反映し WP を再分割・採番し直した。旧番号との対応: 旧WP1→WP1、旧WP2→WP3、旧WP3→WP4+5+6、旧WP4→WP7、旧WP5→WP8、旧WP6→WP9、旧WP7→WP10、旧WP8→WP11、旧WP9→WP12、旧WP10→WP13、旧WP11→WP14、旧WP12→WP15、旧WP13→WP16。WP2(EngineTime)は新設。
v3(2026-07-02): WP1〜17 完了を受けて WP18(プロジェクト形式)・WP19(シェーダ stem)を追加。設計の正に [PF] / [PFW] を追加。web 側の対応作業(WW1〜3)は my_webpage リポジトリの `docs/implementation_plan_web.md` にある(本書の管轄外)。

## 0. 全 WP 共通規則

### ビルド・テスト

```sh
cmake . -B build -DCMAKE_PREFIX_PATH=<Qt install path>   # 初回のみ。Qt 不要の作業は -DSKIP_DEVSTUDIO
cmake --build ./build --config Debug
ctest --test-dir ./build -C Debug --output-on-failure
```

- 完了条件は常に「ビルド成功 + 全テストグリーン + `git diff --check` クリーン + **文書参照が緑**」
- 描画挙動に触れる WP は `pelican_player.exe` の短時間起動確認も行う(`docs/rendering_phase1_review.md` の Validation Run と同じ流儀)

#### 文書参照の完了条件(2026-08-01 追加)

コードを動かすと `docs/source-code-guide/` と `docs/manual/` の `#L<行>` リンクがずれます。
**ソースだけを触る WP では pre-commit フックが走らない**ため、誰も気づかないまま腐ります。
実例として WP242a のマージで 85 本がずれていました。**WP を閉じる前に次の 2 つを緑にすること。**

```sh
uv run tools/doclink.py check    # 行のずれ。update で自動修正できる
uv run tools/doclink.py audit    # 消えた関数の説明・飛び先の取り違え
```

- `check` が赤いのは**ただのずれ**なので、`uv run tools/doclink.py update` を実行して
  差分ごとコミットすればよい。判断は要らない。
- `audit` が赤いのは**文書が嘘をついている**という意味である。関数を改名・削除したなら、
  それを説明している本文も直す。**行番号を動かしても直らない**。
  改名した本人が一番安く直せるので、後続 WP へ送らないこと。
- 「かつて存在した」ことを意図的に書く場合だけ
  [`docs/doc_audit_allowlist.txt`](doc_audit_allowlist.txt) へ理由付きで登録する。
  **問題を黙らせるために登録しない。**

### コード規約(既存コードから踏襲)

- C++20、vulkan.hpp(C API 直接使用禁止)、リソースは `vk::UniqueXxx` / `ImageWrapper` / `BufferWrapper`
- 型は CamelCase、関数は lowerCamelCase、ファイルは小文字連結(例 `rendertarget.cpp`)、メンバは snake_case
- モジュールは `DECLARE_MODULE(Name)` + `GET_MODULE(Name)`(container.hpp)。
  `GET_MODULE` は削除せず composition root / 薄い runtime adapter で使う。
  **純粋 compiler、policy、provider callback ではモジュール間依存を関数引数の
  依存構造体で明示する**(render_pass_dispatch.hpp の `XxxDependencies` が手本、
  [RPE] §10)
- ID 型は `PELICAN_DEFINE_HANDLE`(handle.hpp)、ログは quill の `LOG_INFO(logger, ...)` 系
- エラーは fail-fast(`throw std::runtime_error`)。ただしシェーダコンパイル失敗のみ result 返却([SF] §4.1)
- 通常の外部ライブラリ追加はルート CMakeLists.txt の FetchContent 節に追記する。
  ただしVulkan SDK等のplatform toolchain dependencyは明示providerとし、
  cross targetへ暗黙source-build fallbackを持ち込まない

### レイヤ規則

- `src/core/vkcore` から `src/core/renderingpass` 以上のレイヤへ新たな include を**追加しない**(通知はフラグ/イベントで上に渡し、編成は上位層が行う。WP8 が実例)

### テスト規約

- Catch2 v3。`test/CMakeLists.txt` の `pelican_define_test(<name> [libs...])` で登録
- GPU 必須テストは Vulkan デバイス列挙失敗時に `SKIP()` すること

### 版の扱い(2026-07-26 改訂・ユーザー決定)

全ての versioned 形式は版フィールドを持ち、**現行版ちょうど 1 つだけを受理する**。
版が省略可能な形式は必須化する。**旧版を受理する分岐・旧版を新版へ昇格する処理を
足さない。** 版を上げるときは旧版の受理を同時に削除する。

理由: 現時点で外部利用者が存在しないため、旧版互換は誰のためでもない負債である。
**版システム自体は将来のために維持する** — 実際に互換が必要になった時点で、その
利用者に合わせて移行方針を定める。版フィールドを今削ると、そのとき全形式へ足し
直しになる。

**例外(消してはならないもの)**: ABI の版一致チェック
(`client_abi_version != current → unsupported_version`)は互換残しではなく
**食い違い検出**である。ゲーム DLL がエンジンと食い違ったときに silent crash では
なく明確なエラーにするためのものなので、単一版受理のまま保持する。

これは 2026-07-08 の「ランタイムは v1 だけを読む」を、対象と例外を明確にして
置き換えたものである(旧文は §3 の RenderWorld / ECS 項に残る)。

**runtime epochはversionではない**: `SurfaceEpoch` / `SwapchainEpoch` /
temporal reset epoch等はprocess-localな寿命・不連続markerであり、保存形式の
解釈や旧版受理には使わない。古いepochは互換用に残さず、対応submit /
present完了までのresource lifetimeとしてだけ保持する([WSI] §3)。

### 禁止事項

- **旧版受理の追加**(上記「版の扱い」)。版を増やす WP は旧版の受理を同時に削除する
- `src/core/ecs/` の無関係変更。変更禁止そのものは 2026-07-08 の
  ユーザー決定で解除済みだが、ECS 変更は WP の明示範囲に限定し、
  lifecycle / generation / failure-atomic 規約を回帰テストで固定する
- 無関係箇所のリフォーマット・リネーム(diff を見る人間のため)
- 挙動変更とリファクタの同一コミット混在
- 1 WP = 1 ブランチ = 1 PR。ブランチ名は `agent/wp<番号>-<短い説明>`

## 1. 現行 WP 一覧

| WP | 内容 | 状態 |
|----|------|------|
| WP203c | XR2b-c — OpenXR array swapchain / depth submit / GPU gate | 実装済み・Simulator/実機 gate待ち |
| WP204 | physical plan eject / direct authoring | verified format/attachment + transient/tile-local/alias + dependency-safe reorder/fusion runtime実装済み・general scope/queueと実機GPU gate待ち |
| WP213 | 版の単一化 A — import manifest の version 必須化 | ✅ 完了（2026-07-26、archive） |
| WP214 | 版の単一化 B — physics service V1 の削除 | ✅ 完了（2026-07-26、archive） |
| WP215 | transactional window output root / frame token | 実装済み |
| WP216 | nonblocking SwapchainEpoch / XR mirror retirement | 実装済み・XR実機 gate待ち |
| WP217 | SurfaceEpoch recreation / present support revalidation | 実装済み・live fault gate済み・manual platform gate待ち |
| WP218 | strategy-private arbitrary material outputs / typed MRT | ✅ 完了（2026-07-28）。任意長schema、UINT実GPU、MSAA、typed clear、reflection、reload rollback |
| WP219 | material output attachment state | ✅ 完了（2026-07-28）。output別blend/write-mask、device capability、実GPU、reload rollback |
| WP220 | material same-pixel local-read ABI | ✅ 完了（2026-07-29）。screen/resource input、sampler/input-attachment自動lowering、実GPU |
| WP221 | top-level render compiler program / open backend package | ✅ 内部slice完了（2026-07-29）。flat/preview/XR一括compile、runtime/data-only artifact、mixed/native Vulkan package、provenance、実GPU |
| WP222 | coordinated render graph / surface / material pipeline reload | ✅ 完了（2026-07-29）。候補ABI再解決、surface/pipeline再構築、material values併合、単一publication、実GPU rollback |
| WP223 | runtime ViewFamily foundation | ✅ 完了（2026-07-29）。stable family/view ID、Camera/OpenXR provider、jitter modifier、family別temporal identity |
| WP224 | secondary ViewFamily relation/runtime | ✅ 完了（2026-07-29）。logical〜Vulkan relation、family別FrameUBO、directional shadow provider、mixed-family実行 |
| WP225 | cascaded secondary ViewFamily | ✅ 完了（2026-07-29）。CSM provider、array target、sequential schedule、LightUBO、cascade別culling、実GPU |
| WP226 | planar reflection ViewFamily / generic secondary culling | ✅ 完了（2026-07-29）。reflection provider、clip plane ABI、独立解像度feature、汎用secondary culling、実GPU |
| WP227 | planar reflection Forward opaque capture / pass binding inheritance | ✅ 完了（2026-07-29）。Forward再描画、late binding継承、clustered compiler ABI、実GPU |
| WP228 | secondary ViewFamily transparent capture / family-local sort | ✅ 完了（2026-07-29）。同一sort providerのview別再評価、reflection-local snapshot、Forward transparent再描画、実GPU |
| WP229 | ViewFamily-local clustered light selection | ✅ 完了（2026-07-29）。selection ABI v2、family token、XR左右眼領域、cross-feature integration、reflection多灯実GPU |
| WP230 | planar reflection oblique near-plane projection | ✅ 完了（2026-07-29）。Vulkan ZO一般式、透視/非対称/正射影/X反転、fallback、実GPU |
| WP231 / 231b | image extent dispatch / remaining-mip material view | ✅ 完了（2026-07-29）。typed output extent、local-size ceil divide、remaining mip descriptor、resize/実GPU |
| WP232 | planar reflection mip filter / family-array material ABI | ✅ 完了（2026-07-29）。7-level low-pass、pass-owned view lowering、scalar-family adapter、roughness sampling実GPU |
| WP233 | replaceable render algorithm asset/package | ✅ 完了（2026-07-29）。typed shader asset parameter、compute define/subresource ABI、標準packageのproject差替え・build purge、ON/OFF実GPU |
| WP234 | runtime ViewFamily provider package | ✅ 完了（2026-07-29）。汎用family registry、caller優先解決、planar C++ policyのbuild purge、shadow/reflection ON/OFF実GPU |
| WP235 | raster attachment mip/layer view | ✅ 完了（2026-07-29）。typed attachment view、論理〜Vulkan物理計画、sequential/multiview image view、mip extent、実GPU |
| WP236 | runtime cube render target | ✅ 完了（2026-07-30）。resource/view形状分離、cube-compatible allocation、face attachment、fullscreen/compute/material samplerCube、実GPU |
| WP237 | replaceable cube capture algorithm | ✅ 完了（2026-07-30）。stable 6-face provider、Deferred + Forward capture、family-local clustered selection、secondary runtime layer/descriptor一般化、package purge、実GPU |
| WP238a | common FrameExecutionPlan vertical slice | ✅ 完了（2026-07-30）。open endpoint/capability IR、render/compute/copy収束、closure/fingerprint、compiler/runtime/dump、atomic reload |
| WP238b | Generic Raster Pass ABI vertical slice | ✅ 完了（2026-07-30）。backend非依存draw/state contract、open実装ID、任意MRT/resource port、Vulkan adapter、execution-plan dialect、sprite dogfood |
| WP238c | complete physical plan / NativeScope data boundary | ✅ CPU slice完了（2026-07-30）。完全physical package、canonical round-trip/fingerprint、strict verifier、typed NativeScope effect/ownership/sync。runtime executorは後続 |
| WP238d | NativeScope executor provider / runtime publication | ✅ source-level runtime slice完了（2026-07-31）。owner/generation lease、prepare/rollback、typed resource view、automatic outer sync、scope単位dispatch、generation retirement |
| WP238e | NativeScope command-producing Vulkan fixture | ✅ 完了（2026-07-31）。exact logical/device verification context、実command記録、validation error 0、readback capture、generation rebuild/retirement。公開game-DLL ABIとdevice-loss注入は後続 |
| WP239a | complete-plan verification contextのreload回帰修正 | ✅ 完了（2026-07-31）。automatic planとfragment-linked planを分離保持し、reload CPU回帰を追加 |
| WP239b | hybrid_v1 screen input view-family binding回帰修正 | ✅ 完了（2026-07-31）。family arrayのcanonical layered view契約へテストと生成を統一 |
| WP239c | planar reflection resource port image-view ABI回帰修正 | ✅ 完了（2026-07-31）。sequential captureをmaterial境界でfamily-array descriptorへ適応 |
| WP240a | 既定 rendering config の preset 化 | ✅ 完了（2026-07-31）。新規projectと`animgraph_demo`を`hybrid_v1` + directional shadowへ移行し、project-space headless GPU回帰を常設 |
| WP240b | 背景と環境光 — 既定レンダラの最小見栄え | ✅ 完了（2026-07-31）。単色sky/ambientをfeature化し、既定値をfragmentへ一元化。ライト0のdeferred/forward金属を実GPU画素で検証 |
| WP240c | project空間 material の宣言と実行時ロード | ✅ 完了（2026-07-31）。glTF producerをloweringへ収束し、strict asset index、常設texture resolver、runtime material登録・binding・hot reloadを接続 |
| WP241 | skip を名乗る 4 件の GPU テスト失敗 | ✅ 完了（2026-08-01）。broad catch→SKIP を横断除去し、2系統のfixture契約違反を修正。GPU 125/125、SKIP 0 |
| WP242a | 影を落とせるライトを複数にする | ✅ 完了（2026-08-01）。inventory index 0 固定を解除し、forward/deferred を `pelican_lighting_v1.glsl` へ一本化。GPU gate 126/126 |
| WP242b | point / spot の shadow view provider | **未着手**。provider は directional 1 種のみ。cube shadow は src/ に存在しない |
| WP242c | 影のフィルタと bias の project 空間化 | **未着手**。単一タップ、bias と遮蔽値がハードコード |

WP231〜237の受け入れ詳細:
[`WP231`](design_reviews/2026-07-29_wp231_image_extent_compute_dispatch.md)、
[`WP231b`](design_reviews/2026-07-29_wp231b_remaining_mip_material_ports.md)、
[`WP232`](design_reviews/2026-07-29_wp232_planar_reflection_mip_filter_report.md)、
[`WP233`](design_reviews/2026-07-29_wp233_replaceable_render_algorithm_package.md)、
[`WP234`](design_reviews/2026-07-29_wp234_runtime_view_family_provider_package.md)、
[`WP235`](design_reviews/2026-07-29_wp235_raster_attachment_subresource.md)、
[`WP236`](design_reviews/2026-07-30_wp236_runtime_cube_render_target.md)、
[`WP237`](design_reviews/2026-07-30_wp237_replaceable_cube_capture.md)、
[`WP238a`](design_reviews/2026-07-30_wp238a_frame_execution_plan.md)、
[`WP238b`](design_reviews/2026-07-30_wp238b_generic_raster_pass.md)、
[`WP238c`](design_reviews/2026-07-30_wp238c_complete_physical_native_scope.md)、
[`WP238d`](design_reviews/2026-07-31_wp238d_native_scope_executor_runtime.md)、
[`WP238e`](design_reviews/2026-07-31_wp238e_native_scope_vulkan_fixture.md)。

WP206b の pass-local material variant slice を閉じた後の描画候補は次。番号は実装順を固定するための
予約であり、各候補は着手前に下記の設計/受け入れ条件をレビューして active へ昇格する。

| 候補 | 内容 | 状態 |
|---|---|---|
| WP207a | compute Frame/Light + sampled resource port | ✅ 完了（2026-07-26、archive） |
| WP207b | material/geometry typed frame-graph resource port | ✅ 完了（2026-07-26、archive） |
| WP208 | lighting data contract v2 + clustered dogfood | ✅ 完了（2026-07-26、archive） |
| WP209a | static texture dimension + material sampler authoring | ✅ 完了（2026-07-26、archive） |
| WP209b | RT mip/layer/subresource view | ✅ 完了（2026-07-26、archive） |
| WP210 | indirect dispatch + GPU-written draw arguments | ✅ WP210a〜g 完了（2026-07-28）。transport、culling、segments、hot reload、XR、timing 判断入力まで実装 |
| WP211 | `dist-bake` + shaderc OFF feature delivery | 並行候補・配布/Quest前必須 |
| WP212 | VRS/foveation backend contract | Quest SA2 device/計測待ち |

完了済み WP の一覧・依存関係・本文は
[`implementation_archive.md`](implementation_archive.md) に逐語保存する。
(最新の内部slice受け入れ完了: WP222、2026-07-29。WP203c はローカル実装・自動テスト済みだが、
Simulator/物理 HMD と対象 GPU の実測を残すため active のまま。)

## 2. WP 詳細

### WP213 / WP214: 版の単一化

両 WP は2026-07-26に完了した。実装内容・移行判断・全受け入れ結果は
[`implementation_archive.md`](implementation_archive.md) と次の完了レポートを参照:

- [`design_reviews/2026-07-26_wp213_report.md`](design_reviews/2026-07-26_wp213_report.md)
- [`design_reviews/2026-07-26_wp214_report.md`](design_reviews/2026-07-26_wp214_report.md)

### WP218: strategy-private arbitrary material outputs

固定5-MRTの既定動作を残しつつ、renderer strategyが順序・枚数・型を定義できる
`pelican.material_outputs` v1を実装した。logical schemaにengine上限はなく、
physical compile時にdeviceの`maxColorAttachments`とformat/sample capabilityを照合する。
generated surface shader、SPIR-V reflection、material route、frame graph、MSAA resolve、
SINT/UINT clear/history、flat/XR整合性、hot reload rollbackを同じcontractへ接続した。

受け入れ結果と意図的に残した境界は
[`design_reviews/2026-07-28_wp218_material_outputs_report.md`](design_reviews/2026-07-28_wp218_material_outputs_report.md)
を正とする。attachment別blend/write-maskはWP219、material local-read inputは
WP220、graph+surface coordinated reloadはWP222で解消した。残る候補は
typed integer image accessorとWP211 dist-bakeである。

### WP219: material output attachment state

WP218のordered output schemaへ、field名をキーにした疎な
`material_output_states`を追加した。未指定fieldはsurfaceの`render_state.blend`と
RGBA writeを継承するため標準materialの記述量は増えない。指定fieldは
`opaque` / `blend` / `additive` preset、またはcolor/alpha別のsrc/dst/op、
およびchannel write maskを持てる。

logical enumからVulkan stateへのlowering、flat/XR route整合性、compiled metadataの
fingerprint、pipeline cache key、live material hot reload rollbackを一つのcontractへ
接続した。device compilerは異なるattachment stateに`independentBlend`、blend対象formatに
`COLOR_ATTACHMENT_BLEND`を要求し、integer outputのblendはpipeline作成前に拒否する。
実GPU gateは6-MRT + MSAAで、albedoのadditive blendとR-only write、および
`R32_UINT` object IDのblend無効化を同時に検証する。

受け入れ結果と公開構文は
[`design_reviews/2026-07-28_wp219_material_output_states_report.md`](design_reviews/2026-07-28_wp219_material_output_states_report.md)
を正とする。material/custom raster local-read input ABIはWP220で同じ
physical rendering contractへ追加した。

### WP220: material same-pixel local-read ABI

`.surface`のscreen inputとimage resource portを、公開アクセサを変えずに
combined image samplerまたはinput attachmentへ物理解決する。
material passが`same_pixel`を宣言し、target plannerがtile-local候補を選べる場合、
material raster consumerもproducerと同じdynamic rendering scopeへ融合する。

active graph variant間でsampled/local種別とinput attachment indexが一致することを
shader compile前に要求する。surface compiler、SPIR-V reflection、pipeline local-read
mapping、descriptor type/layout、sequential/multiview viewを一つのcontractへ接続した。
不成立時はmaterialized samplerへfallbackし、整数input attachmentは現行`vec4` image
accessorと型が合わないため明示的に拒否する。

受け入れ結果と公開境界は
[`design_reviews/2026-07-29_wp220_material_local_read_report.md`](design_reviews/2026-07-29_wp220_material_local_read_report.md)
を正とする。graph+surface+material pipelineのcoordinated reloadはWP222で解消した。
次の描画基盤候補はtyped integer image accessor、WP211 dist-bake、Quest/物理HMD gateである。

### WP221: top-level render compiler program / open backend package

flat/XRのrender-config登録に固定されていたresolve、strategy、graph transform、
subgraph replacement、frame/target planningと、別経路だったpreview resolveを、一回の
`RenderCompilerProgram` invocationへ抽出した。runtime variantでは従来の共通plannerと
Vulkan loweringを使う`mixed` modeであり、描画挙動を変えない。previewは同じinvocationと
provider snapshotを使う`data_only` artifactで、request-local executorだけが消費する。

共通interfaceはopenなbackend context / physical packageだけを所有し、Vulkan format、
physical device、render-target definition、target planは
`VulkanRenderCompiler...` packageへ分離した。source-level custom programは標準plannerへ
委譲しても、plannerを呼ばずにbackend-native packageを直接作ってもよい。全経路は
package verifier、GPU arena、prepared generation、単一publication、rollback/retireを通る。
program selectionはengineが`CompiledRenderPipeline`へstampし、dump metadataから観測できる。

受け入れ結果と残した境界は
[`design_reviews/2026-07-29_wp221_render_compiler_program_report.md`](design_reviews/2026-07-29_wp221_render_compiler_program_report.md)
を正とする。previewへのruntime transform / tagged subgraph / physical-plan表示、
`NativeScope` runtime executor、Metal package、CPU/external linker、
公開game-DLL ABIは後続である。

### WP222: coordinated render graph / surface / material pipeline reload

同じwatcher batchに含まれるrender config、`.surface`、material valuesを一つの
render-pipeline transactionへ昇格する。private GPU generationに対して全live materialの
output schema、format、sample count、local-read mapping、attachment stateを再解決し、
file-backedだけでなくengine-generated surfaceも保持したcompiler recipeから再生成する。
影響するshader bundleとgraphics pipelineを全て候補化し、material metadata/value commitを
検証後のpre-publication callbackへまとめる。

公開はpublication mutex内でbase generationのstale検査、pre-publication commit、
runtime rootのCASを連続して行う。どこか一つでも失敗した場合はgraph、shader、
pipeline、material schema/valueの全てを旧世代に保ち、candidate GPU arenaだけを破棄する。
graph-onlyのwrite mask変更、graph+surfaceのoutput schema変更を実GPUで受理し、
同時shader compile失敗時の完全rollbackを検証した。

受け入れ結果、対応範囲、意図的に残した構造変更境界は
[`design_reviews/2026-07-29_wp222_coordinated_render_reload_report.md`](design_reviews/2026-07-29_wp222_coordinated_render_reload_report.md)
を正とする。

### WP238a: common FrameExecutionPlan vertical slice

現行`FramePlan`の`render` / `compute` / `snapshot_copy`等をexecution domainと誤認せず、
authoring互換・診断表現として残したまま、backend非依存の`FrameExecutionPlan`へ写す。
共通IRはcoarseなhost/device/external endpoint classとopenなbackend identity、
versioned capability / implementation / dependency / effect ID、resource epoch/access/intent/
footprintだけを持つ。Vulkan handle、queue family enum、native command payloadは持たない。

標準compilerはVulkan physical target planが実際に選択したendpointへ各nodeを割り当て、
render/anchor/output、compute、copyをそれぞれgraphics、compute、transfer capabilityで
表す。endpoint capability不足、未知参照、逆向きdependency、cross-endpoint bridge欠落を
final planのhard errorとする一方、非連結componentは許可する。

runtime packageはlegacy frame planとexecution planのgraph/node/resource/barrier対応を検証し、
同じimmutable renderer generationで一括publishする。compatibility facadeはlegacy
`FramePlan`から保守的なplanを生成できるが、production compilerは
`FrameGraphDefinition`由来のintent/footprintとphysical endpointを必ず運ぶ。
`currentFramePlanJson()`はstable fingerprint付きexecution planを併記する。

本sliceは既存Vulkan executorの分岐や描画結果を変更しない。CPU scheduler、queue-family選択、
Generic Raster Pass ABI、complete raw physical plan、`NativeScope`はこの共通語彙を消費する
後続sliceで扱う。詳細と受け入れ結果は
[`design_reviews/2026-07-30_wp238a_frame_execution_plan.md`](design_reviews/2026-07-30_wp238a_frame_execution_plan.md)
を正とする。

### WP238b: Generic Raster Pass ABI vertical slice

Techniqueごとに`PassInfo` variantとVulkan dispatch分岐を増やす代わりに、一つの
`type: "raster"`を追加する。project層の`RasterPassContract`はversionedなopen
implementation IDとtyped draw operation、topology/cull/front-face/depth、
attachmentごとのblend/write-maskだけを持ち、Vulkan型、shader module、descriptor
binding番号を持たない。標準authoringは`direct` operationを使うが、implementation IDは
backendが解釈するenumではなく、そのtyped operationを生成した交換可能algorithmのprovenanceである。

shader assetと`resource_ports`は`GenericRasterPassInfo`で契約へ結び、既存graph edge、
history/view/subresource、multiview、local-read loweringを再利用する。color attachment数と
resource port数にengine固定上限を置かず、実device compilerがformat/sample/descriptor/
`maxColorAttachments`を判定する。Vulkan側は独立したadapterでportable stateを
`GraphicsPipelineDesc`へ変換し、融合scopeではlogical fragment locationからphysical
attachment slotへの写像を使う。未使用slotはwrite mask 0にする。

runtime storage/descriptor lifetimeは当面既存`FullscreenPassContainer`を内部実装として再利用するが、
公開ABIとdraw callはfullscreen固定ではない。legacy fullscreenは6-vertex互換を維持し、
generic rasterはcontractのvertex/instance/first値を実行する。typed tile-local portは同じ
resource ABIからinput attachmentへloweringされる。`FrameExecutionPlan`では
`pelican.logical.raster@1` / `pelican.execution.generic_raster_direct@1`として通常renderと
同じgraphics endpointへ合流する。

本sliceはprocedural direct drawを成立させる最小operationであり、vertex/indexed/indirect/
mesh draw、custom vertex layout、material/debug passのgeneric contract移行は後続で
operation variantとadapterを追加して行う。詳細と受け入れ結果は
[`design_reviews/2026-07-30_wp238b_generic_raster_pass.md`](design_reviews/2026-07-30_wp238b_generic_raster_pass.md)
を正とする。

### WP238c: complete physical plan / NativeScope data boundary

`VulkanCompletePhysicalPlanPackage`はgraph、logical/automatic fingerprint、backend candidateと
全engine-visible resource/scope/attachment/alias/feature closureをcanonical JSONで
eject/parseできる。strict verifierはnode coverage、data/after/before順序、lifetime/read
footprint、attachment subresource、alias互換性、alternate format evidence、endpoint capability
を再検証し、verified markerとstable package fingerprintを返す。sparse physical fragmentとは
同じschemaへ統合せず、差分指定と完全同層指定の意図を観測可能に保つ。

`VulkanNativeScopeDeclaration`は既存physical scopeに対するdata-only implementation boundaryで、
typed resource effect、ownership、queue、feature/extension、sync mode、tooling/device-loss/
hot-reload能力、opaque implementation configを保持する。WP238c自体はruntime callbackや
Vulkan handleを受け取らないdata gateとして維持する。詳細は
[`design_reviews/2026-07-30_wp238c_complete_physical_native_scope.md`](design_reviews/2026-07-30_wp238c_complete_physical_native_scope.md)
を正とする。

### WP238d: NativeScope executor provider / runtime publication

custom compilerのverified complete packageを対応するruntime target planへ束縛し、GPU mutation前に
両者の完全一致を検証する。source-level executor registryはimplementationをactive ownerから
engine fallbackの順で解決し、provider owner/registration generation/API/capabilityと、codeを
生存させるgeneration leaseをprepared executorへ残す。provider `prepare`は既存GPU登録transaction
内でdevice/core/pipeline serviceを使え、失敗時はactive generationを変えずcandidate objectを
破棄する。

rendererは各viewのphysical scope先頭でcallbackを一度recordし、scope内の通常node bodyを抑止する。
外部graph barrierはengine、内部command/barrierはproviderが所有する。`automatic`はtyped effectと
attachment aspectから外側image layoutを導出し、`manual`/`unchecked`は作者がbarrierを所有した上で
宣言outer layoutへ戻す。record contextは宣言済みrender-target/frame-target/buffer、MSAA resolve/
attachment、layer/mip viewとFrameResourcesだけを渡す。runtime dumpにはprovider世代とresource
loweringを出す。詳細は
[`design_reviews/2026-07-31_wp238d_native_scope_executor_runtime.md`](design_reviews/2026-07-31_wp238d_native_scope_executor_runtime.md)
を正とする。公開game-DLL raw command ABIは、このsource runtime seamを使う実GPU具体例と
device-loss境界を確認するまで凍結しない。

### WP238e: NativeScope command-producing Vulkan fixture

delegating compilerがdefault Vulkan packageに保持されたcanonical logical graph、target topology、
automatic plan、format capability、実際に`vkCreateDevice`へ渡したextension closureをそのまま使い、
complete packageを再検証して一括installするhelperを追加した。callerがprivate device factsを
再構成したり、未有効extensionを検証入力として申告する必要はない。

test providerは隔離したphysical scopeでdynamic renderingを開始し、宣言済みcolor attachmentを
clearして終了する。後続の通常fullscreen passとheadless readbackでmagenta/greenの実画素を確認し、
debug-utils validation error 0、二世代のhot replacement、in-flight GPU lease完了後の旧executor
retirementを同時にgateした。詳細は
[`design_reviews/2026-07-31_wp238e_native_scope_vulkan_fixture.md`](design_reviews/2026-07-31_wp238e_native_scope_vulkan_fixture.md)
を正とする。公開game-DLL ABIと意図的な`VK_ERROR_DEVICE_LOST`注入は未実装である。

### WP239: `gpu` ラベル回帰の修復（239a / 239b / 239c）

2026-07-31 に `ctest -C Debug -L gpu` を全数実行した結果、**116 件中 4 件が失敗**していた
（108 passed / 4 failed / 4 skipped、実時間 396 秒）。4 件は独立した 3 つの原因に分かれ、
それぞれを WP239a / 239b / 239c が所有する。**1 WP = 1 ブランチ = 1 PR**（§0）は維持する。

原因は `git bisect` で確定し、**各原因コミットの直前を実際にビルドして緑を確認**した。

| 失敗テスト | 原因コミット | 直前の緑を確認したコミット | 所有 WP |
|---|---|---|---|
| fixed spatial upscale carries render/output extents and per-input sampling | `d1c8081` | `5a4f95e` | WP239a |
| dependency-safe physical scopes reorder and fuse real Vulkan rendering | `d1c8081` | `5a4f95e` | WP239a |
| hybrid_v1 preset registers and renders a headless frame | `79dca25` | `5ba40c7` | WP239b |
| planar reflection executes a clipped secondary view family on the GPU | `272ea14` | `369ae47` | WP239c |

**この 4 件が長期間放置された理由を WP の一部として扱う**。CI0 の CPU gate は
`ctest -LE gpu`（[`ci.md`](ci.md)）なので `gpu` ラベルは常設の回帰網に入っていない。
WP239b / 239c の 2 件は 2 日以上、誰にも気づかれずに赤いままだった。修正だけでは
同じことが再発するため、各 WP の受け入れ条件に**全 `gpu` ラベルの緑**を含める。

再現手順は 3 WP 共通:

```sh
cmake -S . -B build -DSKIP_DEVSTUDIO=ON -DPELICAN_WITH_SPIRV_LINK=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug -L gpu --output-on-failure
```

Python を PATH に置いていない環境では SPIRV-Tools の configure が失敗する。
`-DPython3_EXECUTABLE=<path>` で明示する（uv 管理の interpreter でよい）。

**完了結果（2026-07-31）**:

- WP239a は `4c1c88c`、WP239b は `9a8b7de`、WP239c は `70d0e51` で修正した。
- 4 件の直接再現テストは全て緑。
- 最新 configure/build 後の `ctest -C Debug -LE gpu` は **942 / 942 passed**
  （環境依存 1 skipped）、`ctest -C Debug -L gpu` は **121 / 121 passed**
  （既知 4 skipped、実時間 402 秒）。
- 全 CPU gate で、SPIR-V link の stage 別 export 漏れと directional shadow の古い
  `shared_2d` 期待値も検出した。前者は `be67a6f` で実装修正し、後者は
  `1a938ba` で `family_array` 契約へ更新した。
- WP239b の判定は
  [`2026-07-31_wp239b_family_array_binding_report.md`](design_reviews/2026-07-31_wp239b_family_array_binding_report.md)、
  WP239c の判定は
  [`2026-07-31_wp239c_planar_reflection_image_view_report.md`](design_reviews/2026-07-31_wp239c_planar_reflection_image_view_report.md)
  を正とする。

### WP239a: complete-plan verification context の reload 回帰

**目的**: `d1c8081`（WP238e）が追加した verification context 検証が、通常の
rendering pipeline reload を全面的に拒否している。これを解消し、WP238e が守ろうとした
「complete physical plan の検証入力は実 device facts と一致する」という保証は維持する。

**症状**: reload 時に次が出て `ReloadService::applyRequestForTesting()` が false を返す。

```
Failed to load main rendering configuration:
  Render compiler Vulkan package has an invalid complete-plan verification context
render pipeline reload failed: ...（同文）
```

**確定している事実**:

1. **初回ロードは通る**。失敗するのは reload 経路だけで、`gpu` ラベル 108 件は緑のまま。
   両テストとも `pipeline.json` の `ReloadKind::modified` を適用した行で落ちる
   （`headless_render_test.cpp:2723` と `:3641`）。
2. throw 元は `vulkanrendercompilerprogram.cpp` の verification context 検証ループ
   （`physical.target_plan_compilation.verification_contexts` を回す箇所）。
3. `d1c8081` は `renderingsamplecount.cpp` の `compileRenderingTargetPlans()` で
   `topology` と `format_capabilities` を非 const 化して verification context へ
   `std::move` するよう変えている。
4. `targetrenderplanning_test` の CPU verifier 群は緑。**CPU テストは reload 経路を
   通っていない**。

**実装範囲**:

1. **どの条件が発火しているかを最初に特定する。** 現在の検証は 8 個の述語を 1 つの `if`
   に OR で並べ、失敗時に同じ 1 文を投げる。§0 の fail-fast は「名指し hard error」を
   要求しているので、**この診断不能な単一メッセージ自体を欠陥として扱い、条件ごとに
   別メッセージへ分割する**。分割は修正の前提作業であり、修正後も残す。
2. 特定した原因を修正する。reload で verification context と target plan の対応が崩れる
   なら、崩れない側を正とする。**検証を緩めて通す修正は採らない** —
   WP238e が閉じた「private device facts を再構成させない」保証を失うため。
3. reload 経路を CPU テストで固定する。今回の欠陥が `gpu` ラベルでしか出なかったこと
   自体が網の穴であり、同じ形の回帰を CPU gate で捕まえられるようにする。

**受け入れ条件**:

- 上記 2 テストが緑
- `ctest -C Debug -L gpu` が全数緑（4 skipped は Vulkan 非依存の既存 SKIP のみ）
- `ctest -C Debug -LE gpu` が緑
- verification context 検証の失敗が、8 条件それぞれ別のメッセージで名指しされる
- reload 経路を通る CPU テストが追加され、`d1c8081` を revert すると赤になる
- `git diff --check` クリーン

依存: なし。見積: 中。**HEAD が壊れている状態なので最優先**。

### WP239b: hybrid_v1 の screen input view-family binding 回帰

**目的**: `79dca25`（producer view-family arrays）以降、hybrid_v1 preset の material
screen input が期待と別の image view に束縛されている。正しい束縛先を決定し、実装か
テストのどちらが陳腐化しているかを判定して閉じる。

**症状**: `headless_render_test.cpp:5200`

```
REQUIRE( materials.boundScreenInputImageViewsForTesting(material_id, opaque->definition)
         == std::vector<vk::ImageView>{ ...getImageView(shadow_map) } )
with expansion:
  { 1C252400000000B5 }  ==  { 612F93000000004E }
```

**確定している事実**:

1. **要素数は両辺とも 1 で、中身の image view が別物**。配列長の問題ではない。
2. `79dca25` は shadow map を producer view-family array としてモデル化し、
   `materialcontainer.cpp` / `materialscreeninput.{cpp,hpp}` / `shaderresourceinterface.cpp`
   / `render_pass_executor.cpp` を同時に変えている。
3. テストは family 非依存の `RenderTargetContainer::getImageView(shadow_map)` を期待値に
   使っている。array 化後にこの accessor が何を返すべきかが論点。

**実装範囲**:

1. array 化後の `getImageView()` の契約を確定する。**material が束縛すべきは特定
   family / layer の view か、array 全体の view か**を決め、決めた側に合わせて実装
   またはテストを直す。
2. テスト側の陳腐化だった場合も、**単に期待値を実測へ書き換えない**。何を保証したい
   テストだったのかを保ったまま、array 契約で表現し直す。
3. 判定理由を `docs/design_reviews/` のレポートに残す。

**受け入れ条件**:

- hybrid_v1 の headless GPU テストが緑
- `ctest -C Debug -L gpu` が全数緑
- `getImageView()` の array 化後の契約が、コメントまたは設計文書に明記されている
- `git diff --check` クリーン

依存: なし（WP239a と独立、並行可）。見積: 中。

### WP239c: planar reflection resource port の image-view ABI 回帰

**目的**: `272ea14`（WP232、mip 越し planar reflection filter）が
`materialcontainer.cpp` に追加した image-view ABI 一致検査が、planar reflection の
golden GPU ケースを拒否している。ABI 検査と feature 側の宣言のどちらが正かを決めて閉じる。

**症状**: `golden_cases_test.cpp:18` が例外で落ちる。

```
material resource port 'planar_reflection' shader image-view ABI does not match
pass 'planar_reflection_forward_transparent'
```

**確定している事実**:

1. `272ea14` の直前（`369ae47`）では同テストが緑。
2. このエラー文言を導入したコミットは `272ea14` ただ 1 つ。
3. 対象は transparent 側の pass（`planar_reflection_forward_transparent`）。
   WP228 が入れた planar transparent capture との組み合わせで出ている。

**実装範囲**:

1. `planar_reflection` resource port の宣言側 image-view 形状と、mip filter 導入後に
   pass が要求する形状の食い違いを特定する。
2. **ABI 検査を緩める方向は採らない**。検査は WP232 が意図して入れたもので、
   不一致を silent に通すと descriptor 不整合が実行時まで遅延する。
3. `render_evidence_ledger.md` が planar reflection を **E3 +E5** としている根拠は
   この golden ケースなので、修正後に等級の再判定が必要になる（台帳側の更新は
   本 WP の範囲外だが、レポートに影響を明記する）。

**受け入れ条件**:

- planar reflection の golden GPU ケースが緑
- `ctest -C Debug -L gpu` が全数緑
- ABI 検査自体は維持されている（検査の削除・条件緩和による通過は不可）
- `git diff --check` クリーン

依存: なし（WP239a / 239b と独立、並行可）。見積: 中。

### WP240: エンジン既定レンダリングシステム(240a / 240b / 240c)

**背景**: [`render_evidence_ledger.md`](render_evidence_ledger.md) §4.1 の通り、`projects/` 配下 4 project が
参照する feature は `ui` と `sprite` だけで、ViewFamily 系・clustered lighting・TAA・draw sort・
graph transform・render strategy・physical fragment・plan pin は **project 空間での使用が 0 件**である。
WP181〜238e で積み上げた拡張機構には**消費者が一人もいない**。

エンジン既定のレンダリングシステムはその最初の消費者になる。同時にこれは
「何が足りないか」を**需要順で確定させる作業**でもある。
[`render_mechanism_coverage.md`](render_mechanism_coverage.md) §3 の G1〜G16 は監査由来の列挙であって
需要順ではない。実際に既定レンダラを組んだときに詰まった順が正である。

**先行検証(2026-07-31 実施・実測)**: 第 1 段階が engine 改修ゼロで成立することは確認済みである。
一時 project の rendering config を次の 4 行にし、`DamagedHelmet.glb` を置いた scene を
`pelican_player.exe --headless --frames 3 --size 512x512 --render-out <png>` で描いたところ、
hybrid_v1 の deferred 経路で正しく描画された。

```json
{
  "pipeline": { "preset": "engine://render_pipelines/hybrid_v1.json" },
  "features": ["engine://features/shadow_directional.json"]
}
```

同時に判明した実測事実を各 WP の前提とする。

1. 同じ scene から directional light を 1 つ抜くと、**emissive 以外がほぼ完全に黒**になる。
   環境光は実効的に効いていない(WP240b の根拠)。
2. `pelican_cli project init` が生成する `passes/main_rendering_config.json` は **111 行**で、
   pass は `gbuffer_pass` / `ssao_pass` / `ssao_blur_pass` / `present` の 4 つ。
   `present` が `uses_light_data: true` の fullscreen pass としてライティングと present を兼ねる。
   **forward 経路も半透明も snapshot も持たない**、hybrid_v1 より単純で凍結された設計である。
3. 空 scene では 111 行版と 4 行版の出力が **byte 一致**する。モデルを置くと差が出る
   (hybrid_v1 側が forward / snapshot を持つため)。

依存順序は 240a → 240b → 240c。240a は engine 改修を伴わないので単独で先行できる。
なお前提だった WP239(`gpu` ラベル回帰)は 2026-07-31 に完了済みである。

### WP240a: 既定 rendering config の preset 化

**目的**: エンジンが「既定のレンダリングシステム」を持つことを、まず配布物の既定値として成立させる。
新規 project が hybrid_v1 を継承し、以後 hybrid_v1 の改善に自動追随するようにする。

**実装範囲**:

1. `src/devcli/projectinit.cpp` の `rendering_config_json`(111 行の定数文字列、
   現在 191〜302 行付近)を上記 4 行の preset 形式へ置き換える。
   `default_rendering_pass` は `main_render` のままでよい
   (hybrid_v1 の rendering pass 名が `main_render` であることを確認済み)。
2. **これは等価な短縮ではなく機能の格上げである**。現テンプレートには
   forward_opaque / forward_transparent / snapshot が無いので、置き換えによって
   新規 project は半透明と refraction を初めて持つ。この差を
   `docs/design_reviews/` のレポートに明記すること。
3. `projects/` のいずれかを preset 形式へ移行する。**`example` は変換しない** —
   bloom / refract / toon の手書きチェーンを持ち、preset と `render_targets` /
   `rendering_passes` は排他なので機能を失う。`example` は「verbose config の実例」として残し、
   移行対象は新規 project か `animgraph_demo` のように描画要求の薄いものを選ぶ。
4. 移行した project を headless で描く CTest を追加し、**preset 経路が project 空間から
   成立することを常設の証拠にする**。これが台帳の E5 を preset 経路について埋める。

**受け入れ条件**:

- `pelican_cli project init` が生成した project が、追加編集なしで headless 描画できる
- 生成される rendering config が preset 参照であり、`render_targets` / `rendering_passes` を持たない
- 移行した project の headless 描画が CTest に登録され、`gpu` ラベル全数が緑
- `example` の描画結果が変わっていないこと(verbose config 経路の非回帰)
- `ctest -C Debug -LE gpu` が緑、`git diff --check` クリーン

**完了結果(2026-07-31)**:

- `pelican_cli project init` の生成 config を上記 4 行へ置換し、生成直後の project を描く
  `devcli_project_init_command` で preset / feature / ownership 境界も検査するようにした。
- `projects/animgraph_demo` を移行し、`animgraph_demo_preset_headless_player` が project の宣言から
  directional shadow、deferred、forward opaque / transparent、opaque snapshot、present まで
  runtime frame plan に展開されることと PNG 出力を検査する。
- `projects/example` は未変更で、同 config を使う既存 golden / player 回帰を含む GPU gate は
  **122 / 122 passed**（既知の実行環境依存 4 case skipped）。CPU gate は
  **942 / 942 passed**（環境依存 1 case skipped）。
- 機能差と project 選定は
  [`2026-07-31_wp240a_preset_default_report.md`](design_reviews/2026-07-31_wp240a_preset_default_report.md)
  に記録した。

依存: なし。見積: 小。

### WP240b: 背景と環境光 — 既定レンダラが「それらしく見える」ための最小

**目的**: ライトを消すと emissive 以外が黒になる現状を解消する。背景を持ち、
金属や陰の側が真っ黒にならない状態を、**preset を保ったまま feature で**達成する。

**確定している事実**:

1. ライト 1 灯を抜くと emissive 以外がほぼ黒(実測)。
2. 環境光は `src/core/resources/fullscreen.frag` の `standardAmbient` / `openPbrAmbient`
   と `shaders/include/pelican_lighting_v1.glsl` の `pelican_env_ambient(vec3 normal)` で
   法線由来の定数として計算されており、**project から与えるパラメータも hook も無い**。
3. `skybox` / `env_map` / `irradiance` / `brdf_lut` は `src/` に存在しない(調査報告。要再確認)。
4. feature は `render_targets` と `passes` を両方持て、`insertPassByAnchor` は canonical anchor に
   限らず任意の pass 名を anchor に取れる(調査報告。**着手前に自分で確認すること**)。
   これが成り立つなら、preset を保ったまま `deferred_lighting` や `scene_present` を
   anchor にして skybox pass を挿せる。

**実装範囲**:

1. **skybox を engine feature として追加する。** `scene_depth` を input に取り、
   遠クリップでない画素を discard する fullscreen pass で足りる。
   まず単色 / グラデーションで成立させ、cubemap は次段でよい。
2. **環境光を project 空間から与えられるようにする。** 最小は feature parameter による
   色と強度の注入。ambient hook を開けるか、`pelican_ambient_v1` の既存 hook で足りるかは
   実装者が判断する。**engine 側の定数ベタ書きを残したまま feature で上書きする形は採らない** —
   既定値がどこにあるかが二重化する。
3. IBL(prefiltered env + irradiance + BRDF LUT)は**この WP に含めない**。
   単色 ambient で「黒くない」を達成してから、需要と計測を伴って別 WP で設計する。
4. 追加した feature を WP240a で移行した project へ適用し、
   **ライトを消しても形状が見える**ことを headless 描画で確認する。

**受け入れ条件**:

- skybox と ambient が **project 空間の feature 宣言だけ**で有効化でき、preset を書き換えない
- directional light を 0 灯にした scene で、emissive 以外の形状が視認できる
- 既定値の所在が一箇所であること(engine 定数と feature parameter の二重管理をしない)
- 追加 feature を含む headless 描画が CTest に登録され、`gpu` ラベル全数が緑
- `git diff --check` クリーン

**完了結果(2026-07-31)**:

- `engine://features/sky_ambient.json` が `deferred_lighting` 後へ
  `sky_background` を挿入し、`scene_depth` の遠クリップ画素だけへ単色背景を描く。
  色、ambient 強度、sky 強度は runtime-only scalar で、非ゼロ既定値は同 JSON だけが所有する。
- 合成済み feature instance を薄い runtime adapter が毎フレーム解決し、LightUBO 末尾の
  ambient / sky radiance へ運ぶ。feature が無ければ pass / define / radiance は全て zero になる。
  deferred と forward OpenPBR の完全金属をライト 0 で描く GPU テストが両 route の非黒画素を検査する。
- 新規 project template と `projects/animgraph_demo` が preset を変更せず feature を参照する。
  `animgraph_demo` は scene light 0 の committed project として player integration を通した。
- 旧 shader ambient の除去で意図して変わった golden 24 case を目視確認後に再基準化した。
  VAT playback は黒一色へ退化させず、一時 project が同 feature を明示する可視な基準へ移行した。
- Debug build 成功。GPU gate は **123 / 123 passed**（既知 4 case skipped、419 秒）、
  CPU gate は **940 / 940 passed**（環境依存 1 case skipped、27 秒）。
  詳細は
  [`2026-07-31_wp240b_sky_ambient_report.md`](design_reviews/2026-07-31_wp240b_sky_ambient_report.md)
  に記録した。

依存: WP240a。見積: 中。

### WP240c: project 空間 material の宣言と実行時ロード

**目的**: 既定レンダラの中身は結局「標準マテリアル」である。それを project が名乗り、
実行時にロードする経路を作る。**本 WP が既定レンダリングシステムの最大の山**である。

**確定している事実(いずれも自分で `grep` して確認済み)**:

1. `src/` で `registerMaterial` を呼ぶのは `gltf.cpp` / `standardmaterialresource.cpp` /
   `seqplayer.cpp` の 3 箇所だけ。
2. `applyLoweredMaterialForRoute` の**呼び出し元は `test/` のみ**
   (`golden_harness.cpp` / `headless_render_test.cpp` / `materialbinding_test.cpp`)。
   定義は `src/core/material/material.cpp` にあるが、runtime から呼ばれていない。
3. `engine://textures` は `src/core/resources/surfaces/openpbr/*.surface` の**参照側にしか現れず**、
   これを `GlobalTextureId` へ解決するコードが `src/` に無い。
4. `alphaMode` / `alphaCutoff` / `doubleSided` は **`src/` 全体で 0 件**。
   `gltf.cpp` は `MaterialInfo.render_state` を設定しないので、半透明ガラスも
   カットアウトの葉も**全て opaque・背面カリング有りで登録される**。

つまり同梱の OpenPBR 6 variant も toon も refract も、**エンジンの中に居るだけで誰も使えない**。
hybrid_v1 の `forward_transparent` が空のままなのはこれが直接原因である。

**実装範囲**:

**宣言形は決定済み**である。[`design_material_shading.md`](design_material_shading.md) §3-12
(2026-07-31)を正とし、本 WP で新たに設計判断を起こさないこと。同節は §3-4 の「適用」項を
置き換えている。要点は 3 つで、**既存形式の版を上げるものは 1 つも無い**。

- 索引は `assets/asset_data.json` の `materials[]`(要素は `path` のみ)。同じ WP で
  同ファイルへ `schema: "pelican.asset_data"` / `version: 1` を導入する
- 割当は `pelican.material_bindings` の `binding.material` 解決先に project material
  レジストリを加える。両方に同名があれば名指し hard error。scene の `material`
  コンポーネントは**新設しない**
- glTF 再現は producer の収束で行う。patch 構文(`base_from` 等)は**作らない**

**実装範囲**:

1. **glTF 経路を lowering へ収束させる。** 現在 `MaterialInfo` の producer は
   `gltf.cpp`(直接組む・不完全)と `lowerMaterial` → `applyLoweredMaterialForRoute`
   (完全だが `test/` からのみ)の 2 つに割れている。glTF 由来のマテリアルを後者へ通す。
   `alphaMode` / `alphaCutoff` / `doubleSided` の解釈は**この収束の帰結として**得る —
   `gltf.cpp` に個別解釈を書き足す形は採らない。`routing` の 6 状態が
   `{opaque,mask,blend}_{single,double}` の 6 wrapper と 1:1 であることを利用する。
   **これは単独 WP へ切り出してよい**(受け入れが明確になる)。
2. **`asset_data.json` を versioned 化し `materials[]` を足す。** strict v1。
   既存 4 project を書き換える。material 名は project 全体で一意とし、文書横断の
   レジストリを新設して衝突を名指し hard error にする。
3. `engine://textures/*` と `project://*` を `GlobalTextureId` へ解決する**常設 resolver** を
   `src/` へ置く。現在テストのラムダが代行している。
4. `parseSurfaceFormat` → `lowerMaterial` → `loadFromSurfaceForMaterial` →
   `applyLoweredMaterialForRoute` → `registerMaterial` の glue を engine 側へ移す。
   現在この鎖を繋いでいるのは `test/headless_render_test.cpp` だけである。
5. `MaterialValuesReloadHandler` / `TextureReloadHandler` をこの経路へ配線する。
6. **`binding.material` の解決先へ project material レジストリを加える。**
   現在は `ModelTemplate::named_materials`(GLB 内の glTF material 名)のみ。
   両方に同名があれば名指し hard error。`materials[]` を宣言していない project では
   解決結果が変わらないことを回帰テストで固定すること。
7. **`binding.routing` を実行時に消費する。** 現在 `applyPrimitiveMaterialBindings` は
   `routing` 識別子を一度も読んでいない(パース時検証と test の dump にしか効かない)。
   形式の問題ではなく欠落なので本 WP で埋める。

**受け入れ条件**:

- project 空間に置いた `.material` / `.surface` が scene のメッシュへ割り当たり、
  OpenPBR surface で描かれることを headless 描画で確認できる
- `alphaMode: BLEND` を含む `.glb` を置くと `forward_transparent` に流れ、
  `MASK` が cutout として描かれる。**この分岐が `gltf.cpp` の個別解釈ではなく
  lowering の `routing` 経路から来ていること**
- `materials[]` を持たない既存 project の描画結果が変わらない(解決域拡張の非回帰)
- material 名の project 内衝突と、glTF material 名との衝突が、いずれも名指しで失敗する
- テクスチャ参照の解決が `src/` のコードで行われ、テストのラムダに依存しない
- `asset_data.json` が `pelican.asset_data` v1 として strict v1 で受理される(§0「版の扱い」)
- material の hot reload がこの経路でも成立する
- `gpu` ラベル全数と `ctest -LE gpu` が緑、`git diff --check` クリーン

**完了結果(2026-07-31)**:

- glTF material producer を `lowerGltfMaterial` → `applyLoweredMaterialForRoute` へ収束した。
  `alphaMode` / `alphaCutoff` / `doubleSided` は `gltf.cpp` の個別分岐ではなく、
  `{opaque,mask,blend}_{single,double}` の routing から得る。これは
  `4fa1586` として実装範囲 1 だけを先行コミットした。
- `asset_data.json` を `pelican.asset_data` strict v1 とし、path-only の `materials[]` を追加した。
  runtime owner が surface parse、material lowering、shader compile、GPU material/variant 登録を行い、
  engine/project texture resolver と values/texture reload を既存の監視経路へ接続する。
- `pelican.material_bindings` の解決域へ project material を加え、project 内重複、
  glTF/project 同名衝突、routing 不一致をいずれも名指し hard error にした。
  `materials[]` が無い場合は project 解決域を加えない。
- U-USD0c の既存 golden をテスト専用 material 登録から実際の
  `ProjectMaterialAssetContainer` 経路へ移し、project material + OpenPBR + primitive binding の
  実 GPU 描画を固定した。glTF の MASK/BLEND 6 状態、texture/value reload、衝突、strict v1 も
  focused 回帰で固定した。
- Debug 全 target build、CPU gate **944 / 944 passed**（既存の環境依存 1 case skipped）、
  GPU gate **125 / 125 passed**（既存 4 case skipped）を確認した。WSI live fault test は、
  alpha routing を表現できない旧 deferred-only `example` への偶発依存を外し、
  self-contained な `animgraph_demo` で同じ 6 fault 契約を検査する。
- 実装・回帰・使い方は
  [`2026-07-31_wp240c_project_material_report.md`](design_reviews/2026-07-31_wp240c_project_material_report.md)
  に記録した。

依存: WP240a。WP240b とは独立で並行可。見積: 大。**分割を検討してよい**
(1 を独立 WP にすると受け入れが明確になる)。

### WP241: skip を名乗っている 4 件の GPU テスト失敗

**完了（2026-08-01）**。実装境界、原因、検証結果は
[`design_reviews/2026-08-01_wp241_skip_failures_report.md`](design_reviews/2026-08-01_wp241_skip_failures_report.md)
を正とする。実装範囲1は独立コミット `b623fe3` として先行し、隠れていた2系統の
例外が4件すべてで実際の失敗になることを確認してからfixtureを修正した。

**目的**: `gpu` ラベルの 4 件が、engine の fail-fast エラーを握り潰して `SKIP` として
報告している。エラーを表に出し、原因を直す。

**発見の経緯**: `test/ci/run_gpu_gate.py`(2026-07-31 追加)の初回実行で検出した。
**同じ実行で `ctest` 自身は「122 件中 0 失敗、100% passed」と報告している。**
skip されたテストは赤くならないので、落ちたテストより見つけにくい。

**確定している事実**:

1. 4 件はいずれも**テスト本体全体を `try { ... } catch (const std::exception &error) { SKIP(...) }`
   で囲んでいる**。このエンジンは fail-fast で `throw std::runtime_error` する設計なので、
   **検出すべき回帰がそのまま silent skip になる**。
2. 実際に出ているメッセージは 2 系統。

   | テスト | 握り潰されているエラー |
   |---|---|
   | RPC load_gltf publishes once and preserves inventory on preflight and GPU failure | `render-pipeline candidate has no compatible pass for live material 0 (route 'deferred_geometry', shader contract 'gbuffer_v1')` |
   | HR1-M updates one same-layout material and rolls back invalid candidates | `material texture 'albedo_detail' is absent from shader reflection at binding 7` |
   | HR1-M watcher gate and 1000 reloads keep resources bounded | 同上 |
   | WP206b named variant owns its GPU record and reloads atomically with its base | 同上 |

3. これらは GPU 不在による skip では**ない**。同じ実行で他の 118 件は実 device 上で通っている。
4. 最後の 1 件は WP206b の reload atomicity を見る唯一のテストである。
   ただし [`render_evidence_ledger.md`](render_evidence_ledger.md) が WP206b を **E3 +E5** と
   する根拠は `headless_render_test` の "project-owned material variant renders a second
   opaque pass" という**別のテスト**であり、そちらは通っている。**等級そのものは揺らがない**が、
   reload 側は誰も検証していない状態が続いている。

**実装範囲**:

1. **`catch (const std::exception &) → SKIP` の idiom を 4 件から除去する。**
   capability 不足による skip と、実行して失敗したことは別物である。前者が必要なら
   **device / extension の有無を明示的に問い合わせて skip** し、テスト本体は囲まない。
   同じ idiom が他のテストにも無いか `grep` して、あれば同様に扱うこと。
2. 露出した 2 系統の失敗を直す。`albedo_detail` の 3 件は同一原因の可能性が高い。
3. 直したうえで `test/ci/gpu_skip_allowlist.txt` を更新する。**空のまま通るのが正常**で、
   allowlist へ足すのは「device capability が無いと本当に実行不能」と示せる場合だけ。

**受け入れ条件**:

- `python -B test/ci/run_gpu_gate.py --build-dir build --config Debug --artifacts-dir <dir>` が
  **exit 0**(`SKIP exact policy: PASS` かつ ctest 自体も緑)
- 4 件が skip ではなく実行され、通過する
- テスト本体を包む `catch (const std::exception &) → SKIP` が残っていない
- WP206b の reload atomicity が実際に検証された状態になる(現在このテストだけが見ており、
  それが skip している)
- `ctest -C Debug -LE gpu` が緑、`git diff --check` クリーン

依存: なし。見積: 中。**WP240b / 240c と並行可**だが、`albedo_detail` の原因が
material 経路にあるなら WP240c と衝突しうるので、着手前に担当を確認すること。

### WP242: 影 — 既定レンダラの第 5 段階(242a / 242b / 242c)

**位置づけ**: [`design_material_shading.md`](design_material_shading.md) §3-12 で閉じた
WP240 系列(preset 既定化 → 背景と環境光 → project 空間 material)の続きで、
需要順の不足リスト 4 位・5 位にあたる。どちらも「**詰む**」判定である。
「部屋に電球を何個か置いて影を落とす」という最も普通の要求で、この 2 つに同時にぶつかる。

**確定している事実(2026-07-31、`93cf13b` で確認)**:

1. **影を落とせるのは inventory index 0 のライト 1 灯だけ。** 判定は 2 箇所に
   独立して書かれている。forward は `pelican_lighting_v1.glsl` の `pelican_shadow()`
   冒頭で `pelican_light_inventory_index(light_index) != 0u` なら `1.0`(影なし)を返し、
   deferred は `fullscreen.frag` が同じ判定を持つ。
2. **LightUBO は 1 灯分しか場所がない。** `shadowViewProjections` は **cascade で添字**
   されており、ライトごとの行列を置く配列ではない(`src/core/light/light.hpp`)。
3. **provider は directional 1 種のみ。** `viewfamilyproviderregistry.cpp` が登録するのは
   `registerDirectionalShadowViewFamilyProvider` だけで、point / spot / cube shadow は
   `src/` に 1 件も存在しない。
4. ライト数の固定上限は `MAX_DIRECTIONAL_LIGHTS = 8` / `MAX_POINT_LIGHTS = 16` /
   `MAX_SPOT_LIGHTS = 8`。ただし clustered lighting 経由ならこの上限は効かないので、
   **本 WP の詰みどころではない**(coverage.md G5 と混同しないこと)。

### WP242a: 影を落とせるライトを複数にする

**目的**: 影を落とせるライトが 1 灯という制限を外す。ライトごとの shadow 行列を
GPU へ運ぶ経路を作り、消費側の index 0 固定を解除する。

**実装範囲**:

1. **2 箇所の `inventory_index != 0` 判定を、ライトごとの shadow 有無の問い合わせへ置き換える。**
   forward(`pelican_lighting_v1.glsl`)と deferred(`fullscreen.frag`)で**同じ意味論**に
   なることを保証すること。現在は独立に書かれており、片方だけ直すと経路によって
   見た目が変わる。
2. **ライトごとの shadow 行列と shadow map slice を運ぶ形を決める。** 現在の
   `shadowViewProjections[cascade]` は 1 灯前提なので、(ライト, cascade) の二次元へ
   拡張するか、shadow を落とすライトだけの compact な表にするかを選ぶ。
   **固定長配列を増やすだけの拡張は採らない** — 上限が別の場所へ移るだけである。
3. cascade 側の既存契約(WP225 の cascaded secondary view family)を壊さないこと。
   directional 1 灯 + 3 cascade の現行 golden が byte 一致で通り続けること。

**受け入れ条件**:

- directional 2 灯以上がそれぞれ影を落とし、headless 描画の画素で確認できる
- forward と deferred で同じシーンの影が一致する(片方だけ直っていない)
- 既存の `shadow_on` / `morph_skinned_shadow` golden が byte 一致
- `gpu` ラベル全数と `ctest -LE gpu` が緑、`git diff --check` クリーン

依存: なし。見積: 大。**WP241 とは独立**。

### WP242b: point / spot の shadow view provider

**目的**: directional にしか無い shadow view provider を point / spot へ広げる。

**実装範囲**:

1. spot は 1 view、point は cube 6 面。**cube capture の既存機構**
   (WP237 の `$face/` view provider)が流用できるか、新設が要るかを最初に判断すること。
   [`render_evidence_ledger.md`](render_evidence_ledger.md) は cube capture feature を
   **E1**(feature 経路を通す GPU テストが無い)としているので、流用するなら
   その経路に初めて実証が付く。
2. provider registry へ登録し、`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS` の
   OFF 側で消えることを既存 build-unit smoke で確認できる形にすること。
3. WP242a のライトごと shadow 経路の上に載せる。

**受け入れ条件**:

- spot と point がそれぞれ影を落とし、headless 描画の画素で確認できる
- point の 6 面が face ごとに正しく引かれている(1 面だけ描いて通る形にしない)
- `PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF` で provider ごと外れる
- `gpu` ラベル全数と `ctest -LE gpu` が緑

依存: **WP242a**。見積: 大。

### WP242c: 影のフィルタと bias の project 空間化

**目的**: 影の縁が 1 タップでジャギーであること、bias と遮蔽値が engine 内にハードコード
されていることを解消する。**品質の話なので 242a / 242b より後**。

**実装範囲**:

1. PCF(複数タップ)を入れる。タップ数は project 空間の feature parameter で選べること。
2. bias と遮蔽の強さを feature parameter へ出す。**engine 側の定数を残したまま
   feature で上書きする形は採らない**(既定値の所在が二重化する。WP240b と同じ規律)。
3. forward と deferred で同じフィルタが効くこと。

**受け入れ条件**:

- PCF のタップ数・bias・遮蔽強度が project 空間の宣言だけで変えられる
- 既定値の所在が一箇所
- `gpu` ラベル全数が緑

依存: WP242a。見積: 中。

### WP243: glTF マテリアルの遮蔽経路の是正(243a / 243b)

**発見の経緯**: 既定構成(hybrid_v1 + sky_ambient + shadow_directional)で
`DamagedHelmet.glb` を描いたところ、ベースカラーが白基調のアセットが**ほぼ真っ黒**に
描かれた。エンジンを一切変更せず、アセット側の metallicRoughness テクスチャの
**R チャンネルだけ**を 0 から 255 に差し替えたところ正常な絵になり、原因が確定した。

### WP243a: occlusion を metallicRoughness の R から読むのをやめる

**目的**: エンジンが glTF の `metallicRoughnessTexture` を必ず ORM パック済み
(R = occlusion)と決め打ちしている。glTF 2.0 ではこのテクスチャの **R チャンネルは
未定義**であり、occlusion は独立した `occlusionTexture` である。仕様準拠のアセットが
軒並み壊れる。

**現状の経路**:

- `default.frag` と `shaders/material/surface_v1.frag` がともに
  `mix(1.0, mr.r, material.surfaceFactors.w)` を occlusion として書き出している。
  `default.frag` にはその前提が
  `// glTF ORM texture: R=Occlusion, G=Roughness, B=Metallic` とコメントで明記されている。
  **この 2 箇所が同じ誤りを共有している**。`surfacecompiler.cpp` は
  `vec4(surface.roughness, surface.metallic, surface.occlusion, shading_model)` を吐く
  消費側なので、`surface_v1.frag` を直せば自動的に追従する。
- `fullscreen.frag` は G-buffer の B を `materialAO` として読み、
  `min(materialAO, pow(ssao, 3.0))` で SSAO と合成する。`min` なので、ほぼ 0 の
  materialAO が SSAO を無条件に押し切る。これが `albedo * ao * ambientRadiance` を
  厳密に 0 にする。
- `occlusion_strength` は glTF 既定の 1.0 なので `mix(1.0, mr.r, 1.0)` は `mr.r` そのもの。
  **強度を仕様どおり尊重するほど悪化する**。
- `gltf.cpp` で `occlusionTexture` に触れる行は `.strength` を読む 1 箇所だけで、
  画像インデックスは一度も読まれない。`materialcontainer.hpp` の
  テクスチャスロットは base_color / metallic_roughness / normal / emissive の 4 枠で、
  **occlusion 枠が存在しない**。
- `materialformat.cpp` は project 空間 material の occlusion テクスチャを構文としては
  受理するが、消費先はホットリロード用の署名文字列だけで**死んでいる**。

**実測**(この WP を書いた根拠。再現手順として使えること):

| 対象 | R 平均 | R が厳密に 0 の割合 |
| --- | --- | --- |
| DamagedHelmet `metallicRoughnessTexture` | 0.91 / 255 | 62.5% |
| DamagedHelmet `occlusionTexture`(未読込) | 224.86 / 255 | 0.0% |
| sponza の metallicRoughness 20 枚 平均 | 0.62 / 255 | 56〜100% |

**実装範囲**:

1. **occlusion を `occlusionTexture` から読む。** ORM パックはこの修正で自動的に
   成立する — glTF では `occlusionTexture` が `metallicRoughnessTexture` と
   同じ画像を指してよく、その場合に R を読むのが正しいからである。
   **ORM 用の分岐を別に設けないこと。**
2. **テクスチャスロットに occlusion を追加する。** 4 枠固定の前提が
   `materialcontainer.hpp` から descriptor 構築まで通っているので、
   増やす場所を一箇所に閉じること。
3. **occlusion テクスチャが無いときの既定は 1.0(遮蔽なし)。** `gltf.cpp` が
   metallicRoughness に対して行っている白 (255,255,255) の捏造と同じ方式でよいが、
   **既定値の所在を一箇所にすること**(WP240b と同じ規律)。
4. 上の 2 箇所(`default.frag` / `surface_v1.frag`)が**同じ意味論**になること。
   片方だけ直すと経路によって見た目が変わる。
5. `materialformat.cpp` の死んでいる occlusion 記述を、この経路へ接続するか
   削除するかを決めること。**受理するが効かない状態を残さない。**

**受け入れ条件**:

- `DamagedHelmet.glb` が既定構成でベースカラーどおりに描かれ、headless 描画の画素で
  確認できる(非発光画素の中央値が 8bit で 2 以下、という現状から脱していること)
- `occlusionTexture` と `metallicRoughnessTexture` が同一画像を指す ORM アセットでも
  正しく遮蔽が効く
- occlusion テクスチャを持たないアセットの見た目が変わらない
- forward と deferred で同じシーンの遮蔽が一致する
- `gpu` ラベル全数と `ctest -LE gpu` が緑、`git diff --check` クリーン

**既存 golden への影響**: 描画結果が大きく変わるため、遮蔽を含む byte 固定
baseline は更新が要る。**更新した golden の新旧を並べ、変化が意図どおりであることを
PR 本文で示すこと。** 黙って焼き直さない。

依存: なし。見積: 中。**最優先** — 仕様準拠の glTF アセットが全滅する欠陥である。

### WP243b: 直接光の拡散項に AO を掛けるのをやめる

**目的**: glTF は `occlusionTexture` を**間接光限定**と定めている。`fullscreen.frag` は
`directDiffuseOcclusion` として直接光の拡散項にも AO を掛けており、仕様違反である。

**実装範囲**:

1. `fullscreen.frag` の 4 箇所(clustered / directional / point / spot)で
   直接光の拡散項から AO を外す。
2. 現状は `openPbrBase ? 1.0 : ao` になっており、**OpenPBR 経路は既に正しく、
   glTF core 経路だけが間違っている**。分岐を消して両方が正しい側に揃うこと。
3. forward 経路が同じ規律になっているかを確認し、ずれていれば揃えること。

**受け入れ条件**:

- 直接光の拡散項に occlusion が掛からない
- `openPbrBase` による分岐が残っていない
- `gpu` ラベル全数が緑

依存: **WP243a**(先に直さないと AO が 0 に潰れていて差が見えない)。見積: 小。

### WP244: 無名オブジェクトがランタイムに束縛されない fail-silent

**目的**: シーン JSON でオブジェクトに最上位の `"name"` が無いと、エディタの編集が
ランタイムへ一切届かない。しかも**編集は「成功」を返す**。fail-fast の原則に反する。

**現状の経路**:

- `editorruntimefactory.cpp` の `if (!object.name) continue;` が authoring-id → EntityId の
  対応表を作る**唯一の場所**で、無名オブジェクトを黙って飛ばす。
  同ファイルにログを伴わない `continue` が他に 4 箇所ぶら下がっている。
- `scene.cpp` の `if (!object.name.empty() && has_transform)` により、無名オブジェクトは
  そもそもランタイムの名前索引に載らない。`SceneLoader::objectId` は
  `std::string` キーの map 引きである。
- 書き込み先が空でも `editorprojectiontransaction.cpp` の commit は新しい文書を発行して
  `committed` を返すため、インスペクタは「revision N でコミットしました」と表示する。
- `runtime_bindings` は `EditorRuntimeState` のコンストラクタで一度スナップショットされ、
  再収集は `import_scene_snapshot` ハンドラだけである。**`load_scene` では再収集されない**ため、
  シーンロード後はプロセスが終わるまで名前付きオブジェクトの編集も文書限りになる。

**影響範囲**: `projects/example/scenes/main.scene.json` は transform を持つ 32 個の
オブジェクトが**全て無名**である(名前が付いているのは transform を持たない light 14 個だけ)。
手書きシーンはこれを雛形にするため、実質的に既定で踏む。

**実装範囲**:

1. **束縛キーを名前から authoring object id へ移すことを第一候補とする。**
   `authoring_object_id` は既に存在し安定している。名前必須は名前引き索引に
   由来する偶発的な制約であって、設計上の要求ではない。
2. それを採らない場合でも、**束縛できなかった編集が `committed` を返してはならない**。
   名前付きハードエラーにすること。「成功と表示されるが何も起きない」を残さない。
3. `continue` で握り潰している 5 箇所を、ログか例外のどちらかへ寄せること。
4. `load_scene` 後に `runtime_bindings` を再収集すること。
5. `projects/example` の扱いを決めること — 1 を採れば修正不要になる。

**受け入れ条件**:

- 無名オブジェクトの transform 編集がランタイムへ届く(1 を採る場合)か、
  名前付きハードエラーになる(2 を採る場合)。**黙って成功を返す経路が無いこと**
- `load_scene` の後でも編集がランタイムへ届く
- インスペクタ経由の編集で描画結果が変わることを、画素で確認するテストがあること
  (現状はライブ ECS のコンポーネントまでしか検証されていない)
- `ctest` 全数が緑、`git diff --check` クリーン

依存: なし。見積: 中。**WP243 とは独立**、並行可。

### WP245: インスペクタのドラッグ中に値が巻き戻る

**目的**: 値をドラッグすると直前の値へ戻ることがある。自分自身のコミットを
「外部からの変更」と誤認して、編集途中のスナップショットを上書きしている。

**現状の経路**:

- `inspector.cpp` の `draw()` が `pollWatch()` を**毎フレーム無条件に**呼ぶ。
  ドラッグ中・プレビュー中のガードが無い。
- `inspector.hpp` の `inspectorWatchPollFrameInterval = 30` により、30 フレームごと
  (60fps で約 0.5 秒ごと)にシーンリビジョンを問い合わせ、前回観測したトークンと
  違えば `refresh()` を呼ぶ。
- `refresh()` は `selectObject()` を通じて選択オブジェクトのスナップショットを
  **丸ごと差し替える**。ところがドラッグ中の値はまさにそこ
  (`component.authored_json[...] = interaction.value`)に入っている。
- 決め手は、コミット後の `refresh()` が `watch.observe()` を呼ばないことである。
  ドラッグを離すとプレビューがコミットされリビジョンが上がるが、監視状態は古いまま
  なので、**次のポーリングで必ず「変わった」と判定される**。
  結果として 1 回いじるたびに、その直後の約 0.5 秒間だけ次の編集が消える窓が開く。

**実装範囲**:

1. **自分のコミット後に新リビジョンを取り込む。** 自分の変更を外部変更と
   誤認しないこと。これが根本原因である。
2. `pollWatch()` を、プレビュー保持中および編集中のウィジェットがある間はスキップする。
3. `refresh()` が編集途中のフィールド値を破壊しないこと(保存して復元するか、
   編集中は差し替えない)。
4. 本当に外部から変更されたときは今までどおり反映され、
   `stale_revision` の案内も出ること。**外部変更の検知そのものを殺さない。**

**受け入れ条件**:

- 連続してドラッグしても値が巻き戻らない
- 外部からシーンが変更された場合は従来どおり反映される
- 上の 2 つを分けて検証するテストがあること
- `ctest` 全数が緑

依存: なし。**WP244 とは別の層**(244 はランタイム束縛、245 は UI の状態管理)。見積: 小。

### WP246: glTF テクスチャの mipmap 生成

**目的**: 埋め込みテクスチャにミップマップが無く、縮小時に強いモアレが出る。
sponza の布と床で顕著。

**現状**: `vkCmdBlitImage` は `src/` 全体に存在せず、ミップ鎖は生成されない。
`imageloader.cpp` の PNG / JPEG / その他のデコーダは**常に 1 レベルだけ**を積み、
複数レベルを持つのは KTX 経路のみである。サンプラは
`mipmapMode = eLinear` / `maxLod = VK_LOD_CLAMP_NONE` を要求しているのに level 0 しか
存在しない。加えて `materialcontainer.cpp` の標準サンプラは
`anisotropyEnable = false` である。

**実装範囲**:

1. 単一レベルで読み込まれた画像に対してミップ鎖を生成する。GPU の blit 鎖と
   CPU 側生成のどちらを採るかを**最初に決めて理由を書くこと**。
2. sRGB フォーマットの扱いを明示すること(blit 鎖はフォーマットの転送関数の
   影響を受ける)。
3. 異方性フィルタを有効にするかを決める。有効にする場合はデバイス機能の確認を伴うこと。
   **機能が無い環境で黙って落ちないこと。**
4. KTX のように既にミップを持つ画像を二重に生成しないこと。

**受け入れ条件**:

- sponza の縮小領域でモアレが解消し、headless 描画の画素で確認できる
- 既にミップを持つ画像の経路が変わらない
- 異方性が使えないデバイスでも起動する
- `gpu` ラベル全数と `ctest -LE gpu` が緑

**既存 golden への影響**: 縮小を含む baseline は更新が要る。WP243a と同じく
新旧を並べて示すこと。

依存: なし。見積: 中。

### WP247 候補(未 WP 化): IBL / 環境スペキュラ

`src/` 内に `samplerCube` / `textureCube` の宣言が**ゼロ**であり、
`pelican_env_ambient()` は法線引数を受け取りながら一度も使わず定数を返す。
金属は直接光の鋭いハイライト以外を受け取れないため、glTF PBR アセットは
遮蔽を直した後も平坦に見える。**WP243a を直してから見た目を再評価し、
そのうえで WP 化すること** — 遮蔽が 0 に潰れている間は必要量の判断ができない。

### XR2b 分割 WP の逐語条件と所有権

初回レビューの逐語条件:

### XR4 — demo、XR2b — multiview

XR4 は XR2a.0〜3 と XR3a/b 完了後に限る。XR2b は XR2a.0 の logical-frame/view contract を保ったまま
graph/pass の view dimension を追加し、XR2a と同じ semantic image、flat view_count=1 byte 一致、GPU 計測改善を
gate にする。XR2a の壊れた二回 `Renderer::render()` を互換契約として残してはならない。

分割後の所有権は次で固定する。WP203aはtarget-planningの前提だけを完了し、
下記の最終gateを満たしたとは扱わない。

| 条件 | 一意の所有 WP |
|------|---------------|
| logical-frame/view contractを維持したgraph/pass view dimension、flat view_count=1 byte一致、二回`Renderer::render()`を残さない | WP203b |
| XR2aと同じsemantic image、OpenXR array/depth submit、GPU計測改善 | WP203c |

### WP203c: XR2b-c — OpenXR array swapchain / depth submit / GPU gate

**目的**: WP203bのVulkan multiview実行をOpenXR compositionへ接続し、見た目と性能の
XR2b最終gateを満たす。

**実装範囲**:

1. stereo color swapchainを2D arrayとして作成し、各projection viewの
   `imageArrayIndex`とacquire/wait/releaseを一logical frameの契約へ接続する。
2. runtime対応時は`XR_KHR_composition_layer_depth`用depth swapchainと
   `XrCompositionLayerDepthInfoKHR` chainを追加し、非対応runtimeは理由付きでcolor-onlyへfallbackする。
3. mirror、submission fence、runtime-generation lease、history onceの既存保証を維持する。
4. sequentialとmultiviewで同じsemantic imageになることを検証し、GPU timestampで
   改善を計測する。改善しないdeviceでは`auto`選択をdevice profileへ反映できる証跡を残す。

**受け入れ条件**:

- OpenXR protocol fakeでarray index、call order、failure rollback、depth chainを固定する
- Meta XR Simulatorと実機チェックリストで左右、depth、mirror、session再作成が成功する
- XR2aと同じsemantic image
- 対象GPUでsequentialよりGPU計測が改善する
- 全CTest、OpenXR validation、`git diff --check`が成功する

依存: WP203b。見積: 大。

### WP204: physical plan eject / direct authoring

**目的**: 通常は自動 target compiler を使いながら、再現試験や特殊 backend 実験では
同じ physical 層の決定を固定し、最終的には typed boundary を持つ physical fragment を
標準 plan へ link できるようにする。physical から logical への逆変換は行わない。

**Phase A(実装済み 2026-07-26)**:

1. rendering config の `target_planning` を `PlanningProfile`、graph-scoped
   `serial` / `isolate` / `no_alias`、strict warning policy へ型変換し、
   `VulkanTargetPlanRequest` まで接続した。
2. `pelican.vulkan_target_plan_pins` v1 を追加した。自動 plan の
   `ejectable_pin_package` は compiled logical graph の FNV fingerprint と選択済み
   backend candidate を持ち、`vulkan_plan_pins.flat/preview/xr[]` へ同じ package を
   貼り戻せる。
3. package は同一 graph/fingerprintだけに適用し、未知・stale・infeasible candidateを
   fallbackせず拒否する。適用結果は `applied_pin_package` に残す。
4. `RenderStrategy` の ABI seed / config fingerprint から lower-layer control
   (`target_planning` / `vulkan_plan_pins`)を除外して、pin貼り付けによる自己参照を防いだ。
   control data 自体は strategy 出力へ failure-atomic に復元する。

**Phase B v1(実装済み 2026-07-26)**:

1. `pelican.vulkan_physical_fragment` v1を追加した。graph、logical fingerprint、
   automatic plan fingerprint、backend candidate、sparse resource override、任意の
   complete scope partition / alias groupをsame-layerでcanonical round-tripする。
2. resource overrideはautomatic representationの維持、またはautomatic
   transient/tile-local imageの`materialized_image`化だけを許可する。format fieldは
   automatic formatをround-tripするが、別formatはsample/usage capability照合が未実装のため
   fail-closedで拒否する。
3. scopeは全nodeのexact ordered partitionとし、automatic scopeのsplitだけを許可する。
   異なるautomatic scopeのfusion、node重複/欠落、tile/transient resourceのscope越境、
   不正なsampled dependencyを拒否する。
4. alias groupはaliasable resourceだけを受け、representation / format / sample /
   view layout / extentの一致、重複所属なし、lifetime非重複を照合する。
5. canonical target topology、device facts、provider selection/generation、lowering graph、
   resource/scope/alias/sample/view/external-depth contractをautomatic plan fingerprintへ含め、
   stale artifactを拒否する。link後にendpoint capabilityとfeature closureを再検証する。
6. `vulkan_physical_fragments.flat|preview|xr[]`をpipeline/runtime target compilerへ接続し、
   dumpへ`ejectable_physical_fragment` / `applied_physical_fragment`を残す。v1導入時点で
   実行できなかったtile-local/alias結果はcapability gateで名指し拒否し、後続sliceで
   検証可能な部分集合だけを段階的に開放する。
7. `target_planning` / `vulkan_plan_pins`と同様にfragmentをstrategy fingerprintから除外し、
   lower-layer control貼り戻しによる自己参照を防いだ。

**Phase B v1受け入れ結果**:

- parse → canonical dump → parse、full eject、sparse editを同じlinkerで検証済み
- logical fingerprint、target facts、provider generation変化のstale rejectを検証済み
- malformed schema、scope boundary、overlap lifetime、physical compatibility、
  capability growthのrejectを検証済み
- fragment無しのautomatic planは従来経路のままで、追加runtime workを持たない
- OpenXR OFF / ONの両構成でvariant選択と関連runtime regressionを検証済み

**Phase B follow-up — verified alternate format(実装済み 2026-07-26)**:

1. render targetに`format_candidates`を追加した。`format`はautomatic/defaultのまま、
   候補宣言だけでは自動選択を変えない。feature overrideからも候補を重複なしで追加できる。
2. fragmentが別formatを選ぶ場合は、`ResourcePattern`での宣言、
   `materialized_image` representation、対象logical resourceのdevice evidenceを必須にした。
   required image usage、sample count、array layer上限、external depthのtransfer-source契約を
   target固有に照合し、証拠なし・非対応・未宣言をfail-closedで拒否する。
3. linkerは選択formatをresource plan、sample-count plan、lowering required feature、
   external-depth contractへ反映し、同じbackend candidateのfeature closureを再検証する。
4. runtime target bridgeは同じdevice capability snapshotをsample planningとfragment linkへ渡し、
   format assignmentを各`RenderTargetDefinition`へ適用する。同名targetを共有するgraphおよび
   flat/XR variant間でformatが競合する場合はGPU登録前に拒否する。
5. headless Vulkan hot reloadで`R8G8B8A8_UNORM`から`R16G16B16A16_SFLOAT`へ実imageと
   pipelineを世代交換し、reload前後の出力画素一致を検証した。OpenXR OFF / ONの両構成と
   XR protocol/runtime fixtureも回帰済みである。

**Phase B v2 follow-up — verified attachment operations(実装済み 2026-07-26)**:

1. physical fragment version 2へ任意の`attachments`を追加した。各entryは
   `(node, logical_resource)`をidentityとし、`load_op: load|clear|discard`と
   `store_op: store|discard`だけを疎に上書きする。v1 parser互換と、attachmentを持たない
   pure planの既存fingerprintを維持した。
2. pass-wideの簡潔なauthoring設定をFrameGraphのcolor/depth attachmentごとの型付き契約へ
   展開し、automatic physical planへ運ぶ。未知のload/store値は黙って既定値へ落とさず
   config parse時に拒否する。
3. verifierはnode/resourceの存在、raster write、logical readとLoadの一致を照合する。
   編集はmaterialized/external resourceに限定し、Store→Discardは別MSAA resolveが値を保存し、
   後続attachmentがmultisample surfaceをLoadしない場合だけ許可する。
4. runtime compilerはlink済み操作をpassのcolor attachmentごと・depth attachmentごとに
   適用する。通常dynamic rendering、output transform、UI、ImGui、layout transition判定、
   execution traceが同じ物理契約を参照する。authoringのpass-wide値はfallbackとして保持する。
5. headless Vulkan hot reloadでalternate formatと`Clear`→`Discard`を同じgenerationへ
   prepare/publishし、実行passの`eDontCare`適用とreload前後の画素一致を確認した。
   OpenXR OFF / ONの両構成、multiview、XR composition/runtime fixtureを回帰済みである。

**Phase B follow-up — automatic transient attachment runtime(実装済み 2026-07-26)**:

1. materialized/tile-localとは独立した`pelican.vulkan.transient_plan@1`候補を追加した。
   `optimized` profileで、attachment-only、non-history、single-sample、write-onlyの
   virtual resourceだけを対象にし、`conservative_debug`ではmaterializedを維持する。
2. 選択したtransient resourceのautomatic StoreだけをDiscardへloweringし、decision provenanceを
   planへ残す。materialized resource一般のStoreはliveness証明なしに省略しない。
3. runtime target adapterは対象deviceへformat/usageごとの
   `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT`対応を問い合わせ、physical representation
   assignmentを`RenderTargetDefinition`へ適用する。複数graphでrepresentationが競合する場合は
   GPU登録前に拒否する。
4. allocatorはtransient imageへusage bitを付け、lazily allocated memoryをpreferredにする。
   lazy memoryが必須ではないためdesktop deviceでもdevice-local fallbackを保つ。
5. pure runtime adapter testで未対応formatと`conservative_debug` fallback、alternate-format
   fragmentのfail-closedを検証した。headless Vulkanでは実image、Discard、描画結果、
   physical fragmentを伴うhot reload後の契約維持をOpenXR OFF / ONで確認した。

**Phase B follow-up — tile-local attachment runtime(実装済み 2026-07-26)**:

1. logical `same_pixel` read、extent、sample/view契約とdevice/format capabilityから
   `pelican.vulkan.tile_local_plan@1`を選び、producerとfullscreen consumerを一つの
   physical rendering scopeへ融合する。
2. input-attachment shader variant、location/index mapping、BY_REGION dependency、
   dynamic rendering local read、single-view/sequential/multiview executorへ接続した。
3. `tile_local_attachment`はinput/transient attachment usageとlazy-memory preferenceを持つ。
   material/custom consumer、neighborhood read、MSAA、非対応device/formatではmaterializedへ戻る。
4. synthetic Vulkanのsingle-view/multiviewとOpenXR境界を回帰した。Meta XR Simulator /
   HMD compositionと対象tile GPUでの帯域・GPU時間は外部gateとして残す。

**Phase B follow-up — physical image alias runtime(実装済み 2026-07-26)**:

1. non-history、single-sample、materialized、同一format/usage/extent/view契約で、
   compiled lifetimeが重ならないcolor attachment + sampled imageだけを自動alias候補にした。
2. 同じtargetを使う全graph variantが完全に同じgroupへ合意した場合だけruntime assignmentを
   適用する。不一致はallocation共有を無効化し、正しさを変えない。
3. VMA `CAN_ALIAS`とVulkan `IMAGE_CREATE_ALIAS_BIT`で一つのallocationへ別々のVkImageをbindする。
   registration generationごとに固有tokenを発行し、hot reload candidateを旧in-flight
   generationと共有しない。rollback、retire、同extent再生成でも所有権を維持する。
4. alias member切替時は保守的なmemory dependencyを発行し、新しいlogical imageを
   `Undefined`から必要layoutへ遷移する。headless Vulkanでallocation共有、barrier、画素、
   candidate publish/rollback、再生成を検証した。

**Phase B v3 follow-up — dependency-safe physical scope execution(実装済み 2026-07-26)**:

1. physical fragment version 3へ`scope_edit_mode: split_only|dependency_safe`を追加した。
   version 1/2は従来のsplit-only意味を維持し、ejectはversion 3を生成する。
2. logical data edgeと明示`after` / `before`を保つnode/scope reorderだけを受理し、
   編集後のphysical orderからresource lifetimeを再計算してalias groupを再検証する。
3. internal、single-sample、materialized、同一attachment/view契約のrender scopeを、
   Load/Store条件を守る一つのdynamic rendering instanceへ限定的に融合できる。
   tile-local local-readは同じ広いfusion契約のsubsetとして維持する。
4. scheduler/runtimeは元node indexの連続性を仮定せず、scope membershipと実行済みnodeで
   barrier、timing、sprite直前出力を解決する。sequential XRのview別recordingでも同じである。
5. logical adapterはLoad attachmentを同一resourceの`read_write` version edgeとして表現し、
   実config hot reloadとCPU fixtureの意味を統一した。
6. CPU contract、headless Vulkan reorder/fusion、synthetic multiview、tile-local回帰を通過した。
   詳細は
   [`design_reviews/2026-07-26_wp204_scope_execution_report.md`](design_reviews/2026-07-26_wp204_scope_execution_report.md)。

**残る WP204 後続**:

1. MSAA/external/異なるattachment集合を含むscope union、arbitrary sampled/storage/transfer
   dependencyのscope内同期、一般のmaterialized store elision
2. queue family/queue assignment、手動barrier/event/semaphoreを扱うaggressive fragment
3. MSAA/history/depth/storage/transfer/bufferまで含むalias範囲の拡張と、対象tile GPU /
   XR実機での性能・validation gate
4. complete physical/data-only `NativeScope` boundaryはWP238c、source-level executorの
   ownership/publicationはWP238d、command-producing実GPU/validation/readback/reload fixtureは
   WP238eで実装済み。open external runtime、公開game-DLL ABI、device-loss注入は未実装

依存: RPE6c1/WP191、WP202b。見積: 後続は大。

### WP206〜212候補: 描画 mechanism / authoring / delivery

正本:

- [`render_mechanism_coverage.md`](render_mechanism_coverage.md) v8
- [`render_authoring_ergonomics.md`](render_authoring_ergonomics.md) v6
- [`design_reviews/2026-07-26_render_capability_authoring_audit_codex.md`](design_reviews/2026-07-26_render_capability_authoring_audit_codex.md)

共通方針:

1. technique名をengine enumへ足さず、typed resource/view/execution/selection語彙を足す。
2. parser/APIだけで完了にせず、project-owned feature/material/shaderのdogfoodを1本通す。
3. logical authoringはtrust-first / optimize-by-default。矛盾はhard error、単なる曖昧tieは
   deterministic + advisoryとする。
4. 通常経路はlogical nameからgenerated accessorを作り、raw bindingはC-layer escape
   hatchとして残す。
5. physical指定はWP204 fragmentへlinkし、logical configへVulkan fieldを漏らさない。
6. feature未参照時に追加pass/resource/variantを持たない。

WP207a/WP207bは2026-07-26に完了した。実装内容と受け入れ結果は
[`implementation_archive.md`](implementation_archive.md)および
[`design_reviews/2026-07-26_wp207a_resource_ports.md`](design_reviews/2026-07-26_wp207a_resource_ports.md)、
[`design_reviews/2026-07-26_wp207b_material_resource_ports.md`](design_reviews/2026-07-26_wp207b_material_resource_ports.md)
を参照する。

WP208/WP209a/WP209b は2026-07-26に完了した。実装内容と受け入れ結果は
[`implementation_archive.md`](implementation_archive.md)および
[`design_reviews/2026-07-26_wp209a_static_texture_sampler.md`](design_reviews/2026-07-26_wp209a_static_texture_sampler.md)、
[`design_reviews/2026-07-26_wp209b_rt_subresource.md`](design_reviews/2026-07-26_wp209b_rt_subresource.md)
を参照する。

#### WP209b: RT mip/layer/subresource view

2026-07-26 完了。2D runtime RT の fixed/full mip chain、array layer、
fullscreen/compute の sampled/storage subresource viewをtyped化し、physical plan、
Vulkan image/view、resize/hot reloadまで接続した。depth pyramidを第2 array layer上で
実GPU dogfood済み。material portのsubresourceはWP231b、raster attachmentの
任意mip/layer出力はWP235、runtime cube targetはWP236で解消した。runtime 3D targetは
後続の明示拡張であり、未対応設定は黙って無視せず拒否する。

完了内容・検証・意図的制限は
[`implementation_archive.md`](implementation_archive.md)と
[`design_reviews/2026-07-26_wp209b_rt_subresource.md`](design_reviews/2026-07-26_wp209b_rt_subresource.md)
を参照する。

#### WP210: indirect dispatch + GPU-written draw arguments

**目的**: GPU workloadが次frameの固定最大数だけでなく、同frameのdispatch/draw countと
indirect argumentsを生成できるようにする。

**実装範囲**:

1. ✅ buffer usage/typed argument layout、`dispatchIndirect`、barrierを縦切りした（WP210a）。
2. ✅ DrawQueue/rendererへfixed-state GPU-written indexed indirect/count pathを追加した
   （WP210b）。
3. ✅ device feature、count clamp、zero count、CPU fallback、validationをcompiled planへ
   残した（WP210b）。
4. ✅ fixed descriptorで成立する実Vulkan dogfoodを先に行い、bindlessを暗黙依存にしなかった。

**WP210a 完了境界（2026-07-27）**:

- `command_layout: "compute_dispatch"`を論理型として追加し、Vulkan
  `INDIRECT_BUFFER` usageへlowerする
- `dispatch.indirect.{buffer,offset}`を追加。command resourceは自動read edgeとなる
- compute shader write → indirect command readのstage/access barrierを自動発行
- CPU compile時に参照、layout、4-byte alignment、12-byte範囲、direct/indirect混在、
  自身のcommand bufferへの暗黙frame間feedbackを拒否
- project-owned compute headless fixtureをGPU生成argumentへ移行し、同じ画像結果を確認

詳細は
[`design_reviews/2026-07-27_wp210a_indirect_dispatch.md`](design_reviews/2026-07-27_wp210a_indirect_dispatch.md)。

**WP210b 完了境界（2026-07-27）**:

- `indexed_draw`（packed 20 byte）/ `draw_count`（u32）のlogical command layoutを追加し、
  `INDIRECT_BUFFER` usageへlowerする
- `scene_draw_commands_v1`でCPU DrawQueueからcompactな候補command列を公開する
- material passの`gpu_draw_source`を1つの`material_range` entryへ固定し、
  generation-owned buffer IDをruntime compile時にpinする
- command/count read edgeとcompute shader write → indirect command read barrierを導出する
- Vulkan 1.2 `drawIndirectCount` featureを照会・有効化し、
  `maxDrawIndirectCount`との小さい方を上限にする。未対応または`execution: cpu`は既存
  CPU DrawQueueへfallbackする
- 実Vulkan fixtureでcount 0/1/max/overflow、CPU強制fallback、producer順序、
  baselineとのsemantic image一致を検証する

詳細は
[`design_reviews/2026-07-27_wp210b_gpu_draw_source.md`](design_reviews/2026-07-27_wp210b_gpu_draw_source.md)。

**WP210c 完了境界（2026-07-27）**:

- `scene_draw_bounds_v1`（32 byte/AABB）を追加し、`scene_draw_commands_v1`と同じ
  flattened DrawQueue indexでdeformation適用後のworld boundsを公開する
- boundsを持たないcustom geometryはvalid flag 0として公開し、project shaderが
  conservative keepできる契約にする
- sampled image portへ明示LOD、LOD別size、mip count accessorを追加する
- project-owned compute shaderで、実geometry depthから5 mipのmax-depth pyramidを生成し、
  projected AABBを使って1 fixed-state segmentのcommand列をcompactする
- 実Vulkan fixtureでhidden=1、visible=2をGPU buffer readbackで確認し、
  CPU強制fallbackとのsemantic image一致とhost command/bounds件数一致を検証する

詳細は
[`design_reviews/2026-07-27_wp210c_occlusion_dogfood.md`](design_reviews/2026-07-27_wp210c_occlusion_dogfood.md)。

**WP210d 完了境界（2026-07-27）**:

- `scene_draw_segments_v1`（32 byte）でCPU DrawQueueのfixed-state rangeと、
  GPU compact後のcommand範囲/count slotを対応付ける
- view、phase、visibility、material filterごとにsegmentを発行し、sourceが重なる場合も
  output範囲を分離する
- `gpu_draw_source.layout: "draw_queue_segments_v1"`で非空の複数`material_range` entryを
  選択可能にし、各entryのpipeline/material/vertex layoutはCPU bindingのまま維持する
- segment publication切り詰め、commands/count容量不足、device feature不足では
  pass全体をCPU DrawQueueへfallbackする
- 実Vulkan fixtureで異なる2 material stateを同じdepth-pyramid culling taskからcompactし、
  segment count `{1, 1}`、CPU fallback画像一致、plan順序を検証する

詳細は
[`design_reviews/2026-07-27_wp210d_gpu_draw_segments.md`](design_reviews/2026-07-27_wp210d_gpu_draw_segments.md)。

**WP210e 完了境界（2026-07-28）**:

- segmented graph config reloadでcommands/count/segmentsを同じ新runtime generationへ
  一括差し替えし、旧snapshotから旧buffer IDを保持できることを実Vulkanで検証する
- `scene_draw_segments_v1`の32 byte ABI違反をpublish前にrejectし、root generation、
  public buffer ID、segment count、最終画像を直前状態へ保つ
- cull shaderの成功reloadはbundle/pipeline transactionだけを更新し、graph generationと
  buffer IDを維持する。compile失敗ではshader versionと描画結果もrollbackする
- fixture終了時に旧snapshotを解放し、遅延GPU資源をpending 0まで明示flushする

詳細は
[`design_reviews/2026-07-28_wp210e_segmented_draw_hot_reload.md`](design_reviews/2026-07-28_wp210e_segmented_draw_hot_reload.md)。

**WP210f 完了境界（2026-07-28）**:

- compute scheduleを`per_frame|per_view`のtyped contractにし、`per_view`をlogical viewごとに
  sequential実行するmixed view-family scheduleへ接続した
- FrameUBOへ`view_index` / `view_count`を追加し、GLSL helperでsequentialとmultiviewを統一した
- per-view imageをscalar reuse、layer-per-eye descriptor、layered 2D-arrayの3物理形へlowerする
- flatのtransient targetとXRのexternal transfer targetがvariant間で同名の場合、
  storage mode / alias contractを安全なmaterialized supersetへ統合する
- 異なる両眼でCPU fallback、GPU sequential、mixed multiviewを実Vulkan実行し、
  GPU 2経路のRGBA8、全count slot、segment metadata、実行回数が一致することを確認した

詳細は
[`design_reviews/2026-07-28_wp210f_xr_per_view_gpu_draw.md`](design_reviews/2026-07-28_wp210f_xr_per_view_gpu_draw.md)。

**WP210g 完了境界（2026-07-28）**:

- device、candidate/visible/segment/view/output capacity、GPU/CPU 両経路の timing を
  `GpuDrawTimingObservation` と strict JSON v1 にした
- `frame_gpu_ms`、minimum gain、minimum sample 数だけを使う offline break-even evaluatorを
  追加し、live timingからruntime policyへfeedbackしない境界を固定した
- 10 / 130 / 1024 candidate recordsを各24 sample測る実Vulkan sweepを追加した
- GPU pathのcount reset/cull/material node identity、CPU pathでのcull不在、query全回収、
  両経路のRGBA8一致をCI gateにし、絶対msと勝者はgateから除外した
- 最新結果をbuild treeの単一JSONへ上書きし、captureごとの一時projectは削除する

詳細は
[`design_reviews/2026-07-28_wp210g_gpu_draw_timing.md`](design_reviews/2026-07-28_wp210g_gpu_draw_timing.md)。

**受け入れ条件**:

- ✅ occlusion cullingをproject-owned fixed-state dogfoodにする
- ✅ GPU生成count 0/1/max/overflow、graph/shader rollback、hot reload
- ✅ XR view count test
- ✅ CPU fallbackを同じper-view graphで実行し、GPU sequential / mixed multiviewの
  semantic image一致を確認
- ✅ GPU timingでCPU/GPU pathのworkload範囲と「crossoverなし」を含む判断結果を記録
- ✅ descriptor pressureが実測blockerになるまでbindless WPを作らない

依存: WP207b。見積: 大。

#### WP211: `dist-bake` + shaderc OFF feature delivery

**目的**: 開発buildで動くfeature/material/compute構成を、runtime shader compiler無しで
同じsemantic imageの配布物として起動する。

**実装範囲**:

1. composed feature instancesと実使用define集合からdeterministic variant manifestを生成する。
2. surface/fullscreen/compute stemをhost PCのVulkan SDK toolchainでcompileし、
   reflection/binding manifestとtarget profileを同梱する。
3. runtimeはprecompiled artifactを通常cache/provider境界から読み、featureがあることだけを
   理由に起動拒否しない。
4. 通常build/testへPython依存を戻さない。

**受け入れ条件**:

- TAA + shadow + custom project feature + custom surfaceを含むshaderc OFF headless golden
- ON/OFFでsemantic image一致
- clean directoryで同じmanifest/SPIR-V hash
- stale/missing/wrong-target artifactの名前入りreject
- PC packageと将来cross-target packageを同じmanifest schemaで表現

依存: `dist-config` / WP198。mechanism WPと並行可能。Quest SA開始前の必須gate。見積: 大。

#### WP212: VRS/foveation backend contract

**開始条件**: Quest SA2で実機device facts、利用可能なOpenXR/Vulkan foveation extension、
baseline GPU timingが得られていること。

**方針**:

- foveation policyはversioned user feature vocabulary、device extension bindingはbackend
- unsupported deviceは理由付き通常resolution fallback
- jitter/upscale/multiview/render resolutionと同じtyped graph/target contractで解決
- 「Questなら常に必須」と仮定せず、72/90Hzの計測でauto policyを決める

見積: device facts取得後に再見積。

### WP215〜217: Window presentation / WSI epoch recovery

正本: [`design_wsi_epoch_recovery.md`](design_wsi_epoch_recovery.md) [WSI]。

この三WPはbug huntのA-F2/A-F3/A-F4/A-F5/A-F6/A-F7/A-F9/A-F11/
A-F12/A-F13を、局所catchや追加boolでなく一つのwindow output lifecycleとして
閉じる。`SurfaceEpoch`等のepochは旧版互換ではなくprocess-localなresource
lifetimeである。各WPで置換対象の旧APIを直接削除し、compatibility shimを残さない。

#### WP215: transactional window output root / frame token

**目的**: surface factsに依存するtarget / pipelineとframe begin-endを、
partial update不能な一つのrenderer generationへ移す。

**実装範囲**:

1. typed `OutputCompileFacts`、`OutputEncodingPath`、canonical fingerprintを追加し、
   extent / format / color space / usage / queue bindingとWSI-only present configを分ける。
2. 既存`RenderPipelineRuntimeGeneration` publicationをwindow output childまで含む
   top-level rootへ広げる。別のactive WSI pointerを作らず、hot reloadと同じbase-id
   transaction / single CASを使う。
3. `RenderTargetContainer::recreateForExtent()`のtargetごとのin-place commitを、
   全target / descriptor / output pipelineを完成後にpublishするcandidate arenaへ置換する。
4. `IFrameTarget`をmove-only `FrameTargetFrame`のbegin / submit / abandonへ変更し、
   current image / begun / submittedをtarget外側のbool組で管理しない。
5. extentだけならcompiled logicalを再利用してtarget loweringを再実行し、
   format / encoding変更ならattachment/output pipelineまで再prepareする。
   present mode / image countだけではgraphを再compileしない。
6. fullscreen rebind、layout state、temporal resetをpublish contractへ含め、
   publish後にthrowし得る後処理列として残さない。

**受け入れ条件**:

- output factsのfield inclusion / exclusion、canonical hash、rebuild matrixをpure CPU testで固定
- target N個目、descriptor、pipeline、publish直前のfault injectionで
  active rootが完全なoldのまま、candidate membershipがexact rollback
- irreversible WSI cutover後を模したfixtureでは完全な`unavailable`だけが見え、
  partial new / retired oldをReadyとして公開しない
- SRGB↔UNORM test overrideとextent変更でtarget plan / pipeline fingerprintが追従
- resizeとhot reload同時発生でsingle CASまたはstale candidate retry
- begun frame途中の例外でfence / image / generation leaseが次frameに安全
- 旧`consumeExtentChanged()` /引数なしbegin-end /外側`abort_render()`を削除し、
  alias / shimを残さない
- 全CTest、window Vulkan smoke、validation error 0、`git diff --check`

閉じるfinding: A-F3 / A-F4 / A-F6 / A-F9。見積: 特大。

#### WP216: nonblocking SwapchainEpoch / XR mirror retirement

**目的**: swapchain依存objectと状態遷移を一epochへ束ね、window recoveryから
global waitとmain-loop停止を除く。

**実装範囲**:

1. swapchain / images / views / depth / acquire sync / per-image present semaphore /
   frame slot leaseをimmutable `SwapchainEpoch`所有へ移す。
2. `ready / refresh_pending / suspended_zero_extent / preparing /
   unavailable_retry`をdiscriminated stateとして実装し、VkResult分類を一か所へ集約する。
3. framebuffer callbackのrevision付きsnapshotを使い、zero extentはframe skip、
   `SUBOPTIMAL`はfacts keyでcoalesce、`OUT_OF_DATE`は同surfaceのchild epoch交換とする。
4. maintenance1対応時はper-present fenceをpollしてexact retireする。非対応時は
   reacquireで証明済みのsemaphoreだけ破棄し、未証明分だけdevice-lifetime
   quarantineへ移す。
5. swapchain recoveryの`device.waitIdle()`、renderer内`glfwWaitEvents()`、
   caller起動の`recoverSurfaceIfStale()`を削除する。
6. XR mirrorはunavailableならdropし、blockし得るprepareをpresentation
   maintenance workerへ送る。registry commit / publicationだけowner frame boundaryへ戻す。

**受け入れ条件**:

- acquire / present各位置のSuccess/Suboptimal/OutOfDate/OOMとzero extentの
  state/call-order protocol fake
- 同じSUBOPTIMAL keyで再構築1回以下、新revisionでだけ再評価
- maintenance1 fixtureはpresent fence前に旧semaphoreを破棄せず、
  signal後にexact retire
- base Vulkan fixtureはreacquire proofとquarantineを区別し、
  `waitIdle`を完了証明に使わない
- minimize中もRPC / reload / ECS / audio tickが進むwindow smoke
- mirror resize / minimize / failed prepare中もHMD protocol fakeの
  wait-begin-end countと順序が継続
- recovery traceにdevice/queue global idleとblocking GLFW event waitが無い
- validation error 0、全CTest、`git diff --check`

閉じるfinding: A-F2 / A-F7 / A-F11 / A-F12 / A-F13。依存: WP215。
見積: 特大。

#### WP217: SurfaceEpoch recreation / support revalidation

**目的**:永久所有surfaceをfresh factoryへ置換し、
`VK_ERROR_SURFACE_LOST_KHR`から同じlogical deviceで可能な範囲を回復する。

**実装範囲**:

1. Vulkan bootstrapをinstance→initial surface probe→device→initial output
   generationへ分け、initial surfaceをcomposition rootから最初の
   `SurfaceEpoch`へmoveする。
2. `VulkanManageCore`からactive `vk::UniqueSurfaceKHR`と`getSurface()`を削除し、
   instance + native windowからcandidateを作る`WindowSurfaceFactory`を追加する。
3. acquire / present / query / createの`SurfaceLostKHRError`をsurface-lost stateへ写像し、
   old swapchain / surface detach後にfresh surface + child swapchainをprepareする。
4. fresh surfaceに対して現在physical deviceと**作成済み**queue familyの
   present support / sharing planを再検証する。
5. 同じdeviceで成立しない場合、flatは`device_rebuild_required`、
   optional mirrorはreason付きdisableとする。別deviceへ暗黙移行しない。

**受け入れ条件**:

- acquire / present / support query / swapchain create各位置のsurface-lost injectionで
  fresh surface factoryがexact一回ずつ呼ばれ、old handleを再利用しない
- surface作成、support query、target prepare、swapchain prepare各failureで
  complete unavailableを維持し、bounded retryする
- same queue / logical device作成済みalternate queue / uncreated queue /
  unsupported physical deviceのdecision table test
- `VulkanManageCore::getSurface()`とpermanent surface memberが0件
- optional mirrorのsurface lostでHMD loopが継続
- Windows window smoke + validation error 0。RDP接続/切断、display移動は
  manual platform gateとしてreportへ結果を残す
- 全CTest、`git diff --check`

閉じるfinding: A-F5。依存: WP216。見積: 大。

**実装状況(2026-07-27)**:

- `WindowSurfaceFactory`、one-shot bootstrap handoff、`SurfaceEpoch`、
  worker 上の fresh surface + swapchain transaction を実装済み
- 現 presentation familyを優先し、作成済みalternate queueへだけ切替可能。
  未作成queueまたはphysical device非対応は理由付き
  `device_rebuild_required`
- base Vulkanのsuccessor reacquire証明をsurfaceごとに分離し、
  lost surfaceの未証明present資源はGPU完了後にquarantine。
  fresh swapchainを同じnative windowへ重ねず
  `presentation_completion_unavailable`のdevice rebuild要求にする
- state / retry / zero extentのsurface domain保持、RPC診断、
  pure queue decision table testを実装済み
- WSI準備中にpresentation frameをskipした場合も、開始済みのImGui frameを
  `EndFrame`で閉じ、次frameの`NewFrame`へ持ち越さない
- acquire / present / surface作成 / support query / swapchain作成 /
  dependent resource作成をtyped call siteとして追跡し、`VkResult`からの回復指示を
  productionとprotocol fakeで共有する純粋decision tableへ統一
- acquire / present / support query / swapchain create各位置の
  `VK_ERROR_SURFACE_LOST_KHR` protocol injectionで、factory call数、
  surface identity非再利用、呼び出し順、surface-domain retryを固定
- `vkCreateSwapchainKHR`成功前はprevious、成功後のdependent failureでは
  replacementだけを合法なretry anchorとするcutover tableをproductionへ接続
- Debug専用の順序付きWSI fault scriptを実acquire / present / preparation workerへ接続。
  presentは実API呼び出し後に結果を注入し、maintenance1 fenceの完了まで旧surfaceを
  nonblocking pollしてからfresh surfaceを作る
- 通常の`pelican_player`を使うWindows `wsi_fault_window_player` CTestで、
  acquire→surface作成失敗→support lost→swapchain lost→dependent OOM→present lostを
  一processで通し、6注入完全消費、factory 5回、prepare failure 4回、
  fresh-surface publish 2回、validation / native-window-in-use / stderr 0件を確認
- explicit `--frames`をwindowed logical loopの有限gateにも使い、専用の大型test executableを
  追加せずlive smokeを自動終了可能にした
- XR mirrorは理由付き`unavailable(surface_lost)`をそのlogical frameだけdropし、
  terminal resultだけをdisableするresult policyを使用
- 実機のRDP接続・切断、display移動、DPI変更による
  `VK_ERROR_SURFACE_LOST_KHR` 再現はmanual platform gateとして未実施

### 完了地点

WP203aでlogical XR policyとdevice-dependentなVulkan view execution planningを分離した。
`auto` / `sequential` / required `multiview`、scopeの実行回数とview mask、resourceの
shared/sequential/layered layout、device capability/fact、理由付きfallbackまで実装済みである。
WP203bでarray image/view、per-view UBO、`gl_ViewIndex` reflection、typed pipeline/pass
view contract、view-masked dynamic renderingに加え、production fullscreen shader variant、
layered input descriptor、mixed-scope scheduler、Renderer view-family境界まで実装した。

WP203c のローカル実装では、OpenXR color target を 2-layer 2D-array swapchain へ移し、
左右 projection view の `imageArrayIndex` と一回の acquire/wait/release を接続した。
`XR_KHR_composition_layer_depth` が使え、compiled external depth export と format/extent が
一致するときは 2-layer depth swapchain へ各 layer を copy して
`XrCompositionLayerDepthInfoKHR` を提出する。非対応・不一致時は理由を保持したまま
color-only へ戻る。

`xr.multiview_auto` の device/driver/graph 別実測 profile と
`get_status.gpu_timing.logical_frame_averages` を追加し、`auto` は profile が無ければ
optimize-by-default、実測 profile があれば `minimum_gain_percent` を満たすときだけ
multiview を選ぶ。physical target plan の `view_execution_plan.auto_gate` に device identity、
選択、測定値、理由を残し、hot reload 時にも再解決する。synthetic Vulkan fixture の
sequential/multiview semantic byte 一致と OpenXR protocol fake は通過済みである。

残る受け入れ gate は、**この実装を使った** Meta XR Simulator と物理 HMD での
左右/depth/mirror/session lifecycle 確認、および対象 GPU で別 run の
sequential/multiview 計測を profile に記録すること。session loss 後の再生成は従来どおり
別の未配線事項である。実装証跡は
[`design_reviews/2026-07-26_wp203c_implementation_report.md`](design_reviews/2026-07-26_wp203c_implementation_report.md)。

WP202bでrenderer-wide `RenderStrategy`を
preset展開後・feature composition前の独立ABIとして実装した。typed renderer facade、
builtin identity、game-DLL V1/V2 provider、failure-atomicな全config生成を
flat/preview/XR、logical graph、Vulkan target plan、transaction publicationへ接続した。
WP202aでは順序付きglobal `GraphTransform` chainを実装し、protected boundaryと
canonical nodeを維持したfull-config candidateだけを再compileして採用する。
WP201 でtagged region / subgraph replacementを
public game-DLL providerまで縦切りし、元graphを破壊しないcandidate再compile、
typed boundary不変検証、logical/target provenance、transaction snapshotへ接続した。
WP200 ではfullscreen passのshader pair差し替えを
`PassImplementationProviderV1`として公開し、builtin identityとgame DLL実装を
同じtyped logical contract / owner-aware registry / transaction snapshot経路へ接続した。
WP199 でPython製gateを既定OFFの明示opt-inへ変更し、
OpenXRの不要な任意Python探索を通常構成から隔離した。
WP198 でruntime shader compilerのproviderを
Vulkan SDKへ限定し、PC host compile / target precompiled-SPIR-V境界を固定した。
WP197 でPython/test-tool dependencyを通常buildから分離し、
Python testをAUTO/ON/OFF化、experimental SPIR-V linkerを既定OFFのbuild unit化した。
WP196 では project-backed rendering config / feature /
preset の同一 watcher-frame 変更を一つのtransactionへcoalesceし、preview + flat
(+ 起動中のXR)を全prepare後に一回のruntime generation CASで公開する経路を実装済みである。
window swapchain、offscreen、OpenXR各eye、独立desktop mirrorの実submission fenceが
使用generationを保持し、旧GPU resourceは最後の対応fence完了より前にretireされない。
失敗時はactive generation、registry、config cache、watch dependencyを維持する。
physical plan eject / direct authoring は WP204 Phase B v2とverified alternate-format /
per-attachment operation / transient / tile-local / physical image alias runtime sliceまで
実装済みである。
自動planへconservative resource materialization、split-only scope partition、verified alias
group、宣言済みかつdevice検証済みのmaterialized-image format変更、logical dependencyと
MSAA resolveを壊さないattachment load/store変更をlinkしてproduction runtimeで実行できる。
加えて、device/formatが対応するwrite-only single-sample attachmentの自動Store elision、
same-pixel fullscreen/material readのtile-local scope fusion、
lifetime非重複imageのallocation共有まで
実行できる。次はscope fusion/reorder、一般のmaterialized store elision、queue/barrier等の
aggressive controlと、現runtime subsetの範囲拡張を具体的なGPU gate付きで進める。
`NativeScope`はWP238eでVulkan-only command fixture、validation、readback capture、
generation replacementまで実証済みである。次に進める場合も、直ちに公開ABIを固定せず、
device-loss注入または実用的な二つ目のnative workloadが示す不足だけを追加する。

WP205でfeature-owned `directional_shadow` contractをstandard surfaceのpass-input ABIへ
接続した。shadow image、shared 2D view policy、manual depth compare、light indexと
light-space transform relationをtyped metadataとして保持し、同梱featureとprojectへ
コピーしたfeatureを同じ契約で実行する。feature未参照時は追加pass/resource/descriptorを
持たず、`pelican_shadow()`は従来どおり`1.0`へ解決する。B-layer Vulkan golden、
feature-off shader byte、flat/preview/XR view contract、resize/hot reload/rollback、
全894 CTestとvalidation error 0を確認済みである。

WP223でCamera/OpenXRのruntime入力をstable family/view ID付き`RenderViewFamily`へ統一し、
projection modifierとtemporal matrix履歴をprovider identityへ移した。WP224で
pass/compute/frame graph/logical IR/FramePlan/Vulkan executionを貫く`view_family`
relation、family cardinality scheduler、`RenderViewFamilies`、family別FrameUBO slotを
実行経路へ接続した。標準directional shadowは
`$shadow/directional/$cascade/0`として`$main`から独立して一回だけ実行し、shadow drawと
LightUBOが同じfamily-selected行列を使う。callerは同名familyを渡して標準providerを
置換できる。flat/XR temporal、mixed multiview、shadow画像、全renderer traceを回帰した。

WP225でsecondary familyを複数viewへ一般化し、CSMを最初のdogfoodとして閉じた。
`cascade_count` / `resolution` / `max_distance` / `split_lambda` / `stabilize`からstable
`$cascade/N` familyを生成し、family固有extent、D32 array-layer target、secondary
sequential schedule、最大8 cascadeのLightUBO ABI、main-view depthによる受光cascade選択を
接続した。XRでは左右frustumのunionから一つのshadow familyを作り、main viewごとにshadowを
重複生成しない。さらにworld AABBを各cascadeのzero-to-one clip volumeへ保守的に判定し、
material/skinned rangeを保った密なindirect bufferへ再配置する。3-layer実Vulkan goldenで
全layer書き込み、split、invocation、遠方casterのsubmission除外を検証済みである。
point/spot cube shadow、planar reflection、secondary multiviewはこの一般化を再利用する
後続項目であり、WP225には含めない。実装境界と検証結果は
[`2026-07-29_wp225_cascaded_secondary_view_family_report.md`](design_reviews/2026-07-29_wp225_cascaded_secondary_view_family_report.md)
を参照する。

WP226でplanar reflectionを二つ目のsecondary-family consumerとして縦切りした。
標準`planar_reflection` featureはplaneと独立解像度からstable
`$reflection/planar/$mirror/<source-view>` familyを自動生成し、反射view、world clip plane、
2-layer deferred G-buffer、SSAO、lighting、forward transparent向けresource portを接続する。
callerが同名familyを渡した場合はbuiltin providerを置換できる。
FrameUBOのper-view clip planeは標準surfaceから反射面の反対側を除外し、reflectionとCSMは
同じgeneric secondary-family indirect preparationを使う。canonical draw rangeを保ったまま
frustum/clip-plane外の既知AABBだけを`instanceCount=0`にするため、material/skinned phaseを
壊さず未知boundsも保守的に残す。実Vulkan goldenで64×64のG-buffer/depth/color、
reflection invocation、draw preparationを検証した。
WP226時点の標準captureはdeferred-route opaqueを対象とする。forward opaqueのcapture、
secondary multiview、point/spot cube family、family別transparent sortは後続とした。
実装境界と検証結果は
[`2026-07-29_wp226_planar_reflection_report.md`](design_reviews/2026-07-29_wp226_planar_reflection_report.md)
を参照する。

WP227で標準planar reflectionへ`forward_opaque_v1`の再描画passを追加した。
canonical `forward_opaque`と同じmaterial/surface resource bindingをcomposerの最終段で継承し、
reflection固有のtarget、view family、load/storeだけを局所上書きする。この
`inherit_bindings_from`はfeature合成とsurface resource consumer解決後に展開されるため、
shadowやclustered lighting featureをplanar reflectionの前後どちらに書いても、canonical
passへ最終的に集約されたbindingがreflection passへ一致して伝播する。helper fieldは
展開後のpass definitionから除去し、material contract不一致、未知source、cycleはcompile
errorにする。

clustered lightingを同時に有効化した実Vulkan fixtureではDeferredとForwardを左右に分け、
Forward objectがG-bufferへ入らずreflection color/depthだけを変えることを検証した。
このdogfoodで、非Forward surfaceへclustered defineが漏れる問題と、surface compilerが
合成したimplicit resource portがmaterial runtimeへ失われる問題も修正した。WP227時点の
cluster selectionはmain-view空間だったため、clip planeを持つsecondary viewではLightUBOへ
fallbackしていた。この残件はWP229でfamily-local selectionとして解消した。詳細は
[`2026-07-29_wp227_planar_forward_capture_report.md`](design_reviews/2026-07-29_wp227_planar_forward_capture_report.md)
を参照する。

WP228でsecondary ViewFamilyのtransparent drawをcanonical main-view順序の使い回しから分離した。
compiled planに`forward_transparent_v1`のsecondary material passがあるfamilyだけを対象に、
同じ公開draw-sort provider、material filter、opaque/transparent phase規則を各family viewの
camera origin/forwardで再評価する。生成したper-view queueはindirect commandだけでなく
material draw rangeも一緒に選択されるため、固定state segmentを壊さずmain viewと反射viewで
異なるback-to-front順序を持てる。shadowや透明passを持たないsecondary familyはcanonical
queueを共有し、通常のCSMへ追加sortコストを課さない。

標準planar reflectionはForward opaque後のcolor/depthを同解像度・同layerのsampled snapshotへ
コピーし、そのsnapshotをreflection-local screen inputとして
`planar_reflection_forward_transparent`へ渡す。transparent passはreflection color/depthを
loadして再描画するが、reflection texture port自体はopaque color snapshotへ局所overrideするため、
描画中attachmentへの自己samplingを作らない。実Vulkan fixtureではmain cameraと鏡映cameraで
透明二物体のdepth順が反転する配置を使い、Deferred-only、Forward opaque、Forward transparentの
三段差分、depth非更新、G-buffer不変、8 node実行を検証した。詳細は
[`2026-07-29_wp228_planar_transparent_capture_report.md`](design_reviews/2026-07-29_wp228_planar_transparent_capture_report.md)
を参照する。

WP229でclustered selectionをmain-view固定からViewFamily-localへ一般化した。
selection ABI v2は各view領域のheaderへ`view_index` / `view_count`とstable 64-bit
ViewFamily tokenを記録し、consumerは現在のFrameUBOと一致しないbufferを使用しない。
`size_from_extent.copies`で最大2 view分のheader + tile payloadを確保し、selector taskを
`per_view` scheduleへ変更した。flatでは先頭領域、XR sequentialでは左右眼の独立領域を
同じshader ABIで生成する。

feature composerには全base featureの合成後に評価する`integrations`を追加した。
clustered featureはplanar reflectionが存在するときだけreflection-local selection bufferと
`$reflection/planar` selectorを追加し、Deferred / Forward各consumerのselection bindingだけを
局所上書きする。`requires_passes`により、Deferred-onlyやopaque-onlyへ縮小したproject版
reflectionにも存在するpassだけを統合できる。41灯のreflection Vulkan fixture、70灯のflat +
XR左右眼GPU readbackで、LightUBO上限を超えたinventory、family token、overflowを検証した。
詳細は
[`2026-07-29_wp229_view_family_clustered_selection_report.md`](design_reviews/2026-07-29_wp229_view_family_clustered_selection_report.md)
を参照する。

WP230で標準planar reflection providerへoblique near-plane projectionを追加した。
world clip planeを反射view spaceへinverse-transposeし、Vulkan forward-Zのnear/far面8隅を
逆投影して向きを検証し、far面でretained half-space内最大のcornerを選ぶ。projectionの
near rowだけを`plane / dot(plane, far_corner)`へ置換するため、`0 <= z <= w`のnear境界が
鏡面と一致する。
固定のperspective要素を仮定せず、非対称XR、正射影、raster winding用X反転も同じ関数で扱う。
planeがcamera前方に無い場合やfar面と交差しない場合はprojectionを変更せず、従来の
clip-plane culling/fragment discardへfallbackする。標準featureは既定ONだがparameterで
無効化でき、caller-authored familyの置換境界も維持した。詳細は
[`2026-07-29_wp230_planar_oblique_projection_report.md`](design_reviews/2026-07-29_wp230_planar_oblique_projection_report.md)
を参照する。

WP234でsecondary ViewFamily生成を`Renderer`のfamily ID別分岐から汎用provider registryへ
移した。callerが渡したfamilyを最優先し、graphが要求する未解決familyだけをstable IDで
providerへ問い合わせる。directional shadowも同じresolverを通るため、後続のcube/probe
providerをrenderer本体へ分岐追加せず接続できる。

planar reflectionのcamera reflection、winding補正、oblique projection、feature parameter
解決は`src/core/render_algorithms`へ移し、標準shaderと同じ
`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS`でsource/objectごと除外する。OFF buildでも
project shaderとcaller-authored `$reflection/planar` familyを渡せば、同じgraph compiler、
scheduler、Vulkan backendで実描画できる。現registryはsource-level seamであり、
game DLL hot reload向けowner/generation/lease ABIは後続とする。詳細は
[`2026-07-29_wp234_runtime_view_family_provider_package.md`](design_reviews/2026-07-29_wp234_runtime_view_family_provider_package.md)
を参照する。

WP235でraster attachmentを単なるtarget IDから
`(target, optional subresource)`のtyped viewへ拡張した。従来の文字列はmip 0と
既存view-index規則を使う短縮形のまま、object形式では1 mipと連続array layerを明示できる。
選択mipのextentがrender area、viewport、scissorへ伝播し、sequential familyは
`base layer + view index`の2D view、multiview familyは同じ範囲の2D-array viewを使う。
論理graph、Vulkan physical attachment plan、fingerprint、runtime image-view cacheまで
同じrangeを保持する。

hazard/layoutは引き続きimage全体を保守的に扱う。またsame-pixel logical input portが
raster rangeをまだ指名できないため、明示subresource attachmentはtile-local fusionせず
materialized imageとして実行する。MSAA attachment imageはmip 0だけなので
non-zero mipとの組合せをhard errorにした。詳細は
[`2026-07-29_wp235_raster_attachment_subresource.md`](design_reviews/2026-07-29_wp235_raster_attachment_subresource.md)
を参照する。

WP236でruntime imageの**資源形状**をschedulerの**view-family layout**から分離した。
`dimension: "cube"`はsquareな2D image、6 layers、`eCubeCompatible`としてallocationされる。
raster出力はWP235のtyped subresourceで各faceを2D attachmentとして選び、
fullscreen/compute/materialの`view: "cube"`は同じimageの6 faceを1つのcube viewとして
`samplerCube`へbindする。logical target plan、Vulkan physical plan、alias/fingerprint、
runtime registration、resize/hot reloadまでdimensionを保持し、2Dとcubeをalias互換と
みなさない。

cubeはsampled portだけを公開し、storage cube、cube array、runtime 3Dは未対応として
明示rejectする。point/spot shadowやdynamic environmentのcapture family/providerは
機構の利用者でありWP236には含めない。詳細は
[`2026-07-30_wp236_runtime_cube_render_target.md`](design_reviews/2026-07-30_wp236_runtime_cube_render_target.md)
を参照する。

WP237で最初のruntime cube consumerを、交換可能な標準`cube_capture` featureとして縦切りした。
同featureは`$capture/cube`上にVulkan cube layer順のstable view
`$face/+x`、`$face/-x`、`$face/+y`、`$face/-y`、`$face/+z`、`$face/-z`を供給し、
通常のDeferred geometry/SSAO/lightingとForward opaque/transparent passで
`cube_capture_color`の6 faceへsequential描画する。六方向cameraは
`render_algorithms/cube_capture`だけが知り、graph compiler、scheduler、Vulkan backendへ
cube固有passや分岐を追加しない。callerが同名familyを渡せば標準providerより優先され、
`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF`ではproviderのsource/objectをbuildから
除外できる。

同時に、compile時1-view templateとなるsecondary familyについて、family内部targetを
runtime cardinalityまで選択可能なsequential array layoutへ一般化した。fullscreen descriptorも
compile時view数ではなく選択targetのlayer容量からvariantを確保するため、cubeだけでなく
cascade、複数mirror、将来のsecondary providerも同じ経路を使える。clustered lighting併用時は
6-view専用selection task/bufferを順序非依存integrationで追加する。公開cubeは意図的に
1 mipで、BRDF-aware prefilter、複数probeの更新/選択、main materialへの自動bindingは
別の交換可能algorithmとして残した。詳細は
[`2026-07-30_wp237_replaceable_cube_capture.md`](design_reviews/2026-07-30_wp237_replaceable_cube_capture.md)
を参照する。

## 3. トラック現況(WP 化待ちを含む)

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

- **レンダーパイプライン拡張境界(RPE、2026-07-23 v2.1 / HEG v1)**: versioned
  `hybrid_v1`、semantic material route、deferred + forward の scene-linear 合成は
  実装済み。以後は [RPE] / [RGC] の preset から physical/native までの拡張 ladder と
  Request / Resolved / Compiled / Prepared / Runtime 語彙へ揃える。現在の単一
  material-batched draw queue、sample count 1 固定、XR/preview ad-hoc callback を
  一度に直さず、RPE1(WP180)→ RPE2 typed plan(WP181) →
  RPE3 DrawQueueBuilder(WP182) → RPE4 provider registry(WP183) →
  RPE5 bounds / phase queue / transparent sort / XR view policy(WP184) →
  RPE6a logical type / shadow graph(WP185) → RPE6b0 versioned logical value /
  producer edge(WP186) → RPE6b1 typed screen input / opaque snapshot(WP187) →
  RPE6c0 topology / backend probe / optimize-by-default / advisory diagnostics(WP188) →
  RPE6c1 desktop/tile target plan(WP189) → typed sample-count resolve + 実 MSAA
  runtime vertical slice(WP190) → physical target planner runtime統合(WP191) →
  builtin graph variant policy(WP192)、runtime publication root(WP193)、
  append-only GPU registration transaction(WP194)、scope-aware replacement と
  generation-owned registry lease(WP195)、submission-fence lifetime と
  pipeline watcher publication(RPE10b3 / WP196)、fullscreen
  PassImplementation provider(RPE11a / WP200)、tagged region / subgraph replacement
  (RPE11b / WP201)、global GraphTransform(RPE11c / WP202a)、renderer-wide
  RenderStrategy(RPE11d / WP202b)まで完了。builtin identityと
  game DLL差し替えproviderは同じtyped
  boundary contractを使い、replacement candidateは全logical graphを再compileしてから
  target loweringへ進む。global transformは順序付きfull-config candidateとして実装し、
  renderer strategyはpreset後のseed config全体を生成する別ABIとして実装した。
  strategy V1のfacadeは現compilerが実際に消費できる機構だけを公開し、live
  material/light/geometry inventoryは後続versionへ残す。
  RPE6c0/1 では [HEG] の immutable canonical / disposable lowering seam、
  dialect legality、pairwise endpoint relationを置くが、汎用 CPU scheduler、execution linker、
  動画 backend は計測・具体需要まで実装しない。logical effectは作者を信頼する任意宣言、
  lower-level warningのstrict化はproject / CI opt-inとする。通常policyは宣言contractを信頼して
  parallel / fusion / aliasを許すoptimize-by-defaultとし、serial / isolation / conservative
  debugは明示時だけ有効にする

- **コマンド／エディタ層**: stdio JSON-RPC、`load_gltf` /
  `update_transforms`、typed editor query/edit、actor/CAS、undo/redo、
  atomic save、snapshot import、watch token、`eval_preview` /
  `render_preview`、薄い `pelican_rpc.py` まで実装済み(WP27/42/
  149〜172)。windowed host は bounded queue で engine thread の
  frame boundary に dispatch する。外部接続は引き続き 1 本で、
  複数クライアントは WebSocket 展開時の課題
- **ゲームロジック(ネイティブ C++)**: スクリプトを特権化せず、C++ の
  G1a/G1b/G2、behavior attachment/edit、二世代 DLL reload まで実装済み
  (WP43/45/90/155/162/167)。ゲーム固有ロジックは公開 service と
  `PELICAN_PROJECT` 経路で差分実装する
- **devstudio(Qt) / エディタ**: D0「エディタ特権の禁止」に従う
  typed service/RPC、ImGui Object Tree・schema-driven Inspector・
  Asset Browser、保存/undo/preview の共通実装は WP149〜172 で完成。
  Qt DevStudio の埋め込みビューポート、ピッキング、ギズモは未実装。
  Qt/ImGui/外部クライアントは同じ `EditorCommandService` を消費する
- **カメラシステム**: glTF 1:1 の perspective/orthographic、複数
  camera / atomic `set_camera` と公開 API だけで動く orbit/follow/fly は
  実装済み(WP48/50)。残りは glb round-trip と transform_seq v2 の
  camera track。カット/ブレンド/シェイクはユーザー空間の controller で
  実装する
- **2D ゲーム機能 + 2D⇔3D 相互変換**: sprite contract/GPU world quad、
  pixel-perfect/flipbook、shapeCast、side-scroller vertical slice まで実装済み
  (WP103/104/106/107/109)。UI と描画基盤を共有し上物を分離する。
  2D scene の 3D 空間配置・3D scene の 2D 投影編集は未設計
- **アニメーショングラフ**: A0〜A2、VRM-S0/S1、VRMA-C0/R0/I0
  まで実装済み(WP94〜102/111/121〜134/176〜178)。VRMA は
  body/expression/gaze を typed source として graph へ接続し、
  generation mismatch は明示 `rebind()` する。未実装は root-motion
  policy、automatic `.vrma` watcher 配線、graph v2、timeline/live source、
  SpringBone
- **物理クエリ／イベント**: raycast / overlap / shapeCast、Builtin/Jolt/
  game-DLL provider、purgeable OFF stub、決定的 `OverlapEnter/Exit` まで
  実装済み(WP46/47/107/165/179)。Jolt は query provider であり、
  rigid-body world/step/constraint は未実装。rpc query・mesh/BVH も P3
- **OpenXR トラック**: XR0〜XR4 と Simulator blocker 修正まで実装済み
  (WP125〜138)。Vulkan bootstrap、session loop、左右眼 sequential
  composition、action/pose、reference space、feature policy、left-eye
  mirror、VRM demo を Meta XR Simulator で検証済み。WP203a〜c で
  multiview planning/runtime、2-layer OpenXR color/depth composition、
  実測 device profile gate まで実装済み。残りは現実装の Simulator 再確認、
  物理 HMD/対象 GPU 実測 gate、Quest standalone SA0〜SA3
- **Window presentation / WSI lifecycle**: 2026-07-27に[WSI]を確定。
  SurfaceEpoch / SwapchainEpochはversion互換でなくruntime lifetimeとし、
  output factsとrenderer rootのsingle publication、nonblocking resize /
  minimize、present fence / quarantine、fresh surface recoveryを
  WP215→WP216→WP217で実装する。完了まではA-F5/A-F12等を対応済みと扱わない
- **bindless バックエンド**: 2026-07-08 方向決定 — classic(set 2)と併用(`design_material_shading.md` §3-5)。生成アクセサが差を吸収、M2 のデータ形(SSBO + 参照)が前提工事。実装は M2 の後・GPU 駆動系(WP36 パーティクル・大規模シーン)の需要と同時に WP 化。**web/モバイルの床に PC を縛らせない**(web は将来やるとしてもシンプルな 3D/2D — ユーザー確認)
- **web ビルド(WASM)**: 将来の可能性としてのみ保持(2026-07-08)。守るべき不変条件は全部現行規律(純ロジック規律・データ契約が抽象・classic 床・dist-bake/WGSL レーン)— 特別な保全作業なし。進めるときは案 B(WASM ゲームコア + TS レンダラ接合)→ 案 A(C++ WebGPU 実行系、データ契約の兄弟執行器)。**RHI の後付けは禁止**(本体の C++ インターフェースへの制約源にしない)
- **アセットホットリロード**: HR0〜HR2-G 完了。WP147 で model
  reload 時の animation 全体 reset を対象 asset generation + evaluator
  rebind へ置換し、WP162 で game DLL の二世代 side-decode を固定、
  WP178 で VRMA reload generation entry point を追加した。確定規約
  (リプレイ/strict/rpc 中無効・自己書き込み `(AssetKey, hash, epoch)`
  token)と単一 FileWatcher 経路を維持する。WP196で rendering config /
  render feature / pipeline preset の一括publicationを接続済み。残りは
  HR2-I(input/profile)、U3(UI)、`.vrma` watcher の自動配線
- **イベント層**: E1(WP56)と typed payload schema(WP71)に加え、
  ユーザーレビュー済み v1.1 の E2 `OverlapEnter/Exit` を WP179 で
  実装済み。Stay は需要が出た場合だけ additive に追加する
- **永続化(user:// + 設定/セーブ)**: [PF] v6.3 の `user://` と P1
  settings/saveData/loadData/listSaves を実装済み(WP55/65)。save delete、
  cloud sync、非同期 I/O は需要時に additive WP 化する
- **[PF] v6.3**: `user://`、asset store + `.pelican/local.json`、
  `#fragment` を承認・凍結済み。V1/WP55、V2/WP66、V3/WP57、
  K1〜K4 は WP77/79/81/84 で実装済み
- **コンテナアセット**: fragment load、glTF scene extraction、
  `pelican-import-tools` の PSD/atlas、`imports.rules.json` まで実装済み
  (WP77/79/81/84)。PSD 系は引き続きエンジン非リンクの外部ツール契約
- **RenderWorld / ECS**: (2026-07-02 方針変更)統合ブランチ系列を開発本線として独自に進める。**(2026-07-08 改訂・ユーザー決定)`src/core/ecs/` の変更禁止を解除** — 凍結の代償(PhysWorld 生ポインタ・カメラ二重所有・dummy コンポーネント・cb_deinit 不呼出で非自明型が memcpy 移動される)が 2026-07-08 リファクタ監査で顕在化したため。以後 ECS コアに世代付き EntityId・construct/move/destroy 規約等を入れてよい。main との合流は「随時追従 merge」から「将来の逆提案(こちらの ECS 改良を main へ提案)」へ位置づけ変更。**互換受理の追加禁止(同日決定)**: ランタイムは v1 だけを読む。旧形式の変換が要る場合は外部ツールで行う — 以後の WP が新旧両対応の受理コードを足すことを §0 違反とする
- **コマンド層の WebSocket 展開**: stage 2(stdio JSON-RPC)実装後、同じメソッド群を WebSocket に載せると devstudio と web viewer(my_webpage)が同一プロトコルでエンジンを叩ける([PFW] §7)。stage 2 の後に設計文書を書いてから WP 化
- **asset manifest(sha256)**: `assets.manifest.json` の生成・照合・起動時検証と
  asset store mount を WP55/66 で実装済み。通常起動では診断し、
  `--strict-assets` 指定時は不一致を起動前 error にする
- **naga 変換のビルドスクリプト化**: [PFW] §4-3(a) の WGSL→SPIR-V 一括変換を node CLI 化(web repo 側作業。実績コードは `apps/site/src/lib/shader/nagaSpirvCompiler.ts`)。WW3 の後
- **プロジェクト解釈レイヤ(`pelican_project`)**: パース・検証・パス解決・
  正規化をエンジン非依存 target へ分離済み(WP44)。engine は薄い binder、
  `pelican_cli import` は engine 外 consumer としてこの target を使う
- **compute / GPU 計測 / bindless / RT**: compute task graph/実行一本化は
  WP33〜35/64、stereo-safe GPU timing・VRAM/XR timing は WP143/145 で
  実装済み。bindless と RT は需要・設計合意後に WP 化する

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
