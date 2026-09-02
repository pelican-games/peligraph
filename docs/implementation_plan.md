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
cmake . -B build -DCMAKE_PREFIX_PATH=<Qt install path> -DPELICAN_WITH_SPIRV_LINK=ON
cmake --build ./build --config Debug
ctest --test-dir ./build -C Debug -j4 --output-on-failure
```

- **`-j4` を省かないこと。**直列 674 秒に対し `-j4` で 346 秒(実測、失敗 0)。
  それ以上上げても伸びないのは `pelican_golden_gpu` の resource lock が
  27 件の GPU テストを直列化して臨界経路になっているため。
  **`-j64` では 12 件が失敗し GPU が 10 回 SEGFAULT した**ので、全数は `-j4`〜`-j8` に留めること。
  内周で速く回したいときは `-LE gpu -j16`(1016 件が約 9 秒)。
- Qt 不要の作業は `-DSKIP_DEVSTUDIO=ON`。ただし `src/devstudio/` を触る WP では OFF にして両方確かめること
- **`-DPELICAN_WITH_SPIRV_LINK=ON` を省かないこと。** この option の既定は OFF で
  ([`ci.md`](ci.md) §「experimental SPIR-V linker」)、省くと SPIR-V リンカのテスト 5 件が
  **そもそも登録されない**。CI は ON で回すので、省いたまま「全数緑」と報告すると
  CI で初めて割れる。実例として WP249 はこの指定が無い手順で検証され、
  ctest 総数が統合ブランチより 5 件少ない構成のまま緑と報告された(実害は無かったが、
  それは偶然である)
- **機能フラグの検証セット(§4 規則 9)**: WP で触れた gate に対応する
  **全てのフラグを、それぞれ対照構成で実際にビルド・テストすること。**
  一つの対照構成を別のフラグの対照として代用しない。基準値・対照値・対照で
  連動するフラグの正本は
  [`cmake/pelican_feature_registry.cmake`](../cmake/pelican_feature_registry.cmake) とする。
  以下の表は同 registry から生成され、通常の configure と
  `cmake -P cmake/verify_feature_registry.cmake` が完全一致を検査する。
  更新は `cmake -P cmake/update_feature_ledger.cmake` で行う。

<!-- PELICAN_FEATURE_REGISTRY_BEGIN -->
| フラグ | 基準値 | 対照値 | 対照で連動するフラグ |
|---|---:|---:|---|
| `PELICAN_RUNTIME_SHADER_COMPILER` | `ON` | `OFF` | — |
| `PELICAN_WITH_SPIRV_LINK` | `ON` | `OFF` | — |
| `PELICAN_WITH_AUDIO` | `ON` | `OFF` | — |
| `PELICAN_WITH_VAT` | `ON` | `OFF` | — |
| `PELICAN_WITH_EXR` | `ON` | `OFF` | — |
| `PELICAN_WITH_RPC` | `ON` | `OFF` | — |
| `PELICAN_WITH_SEQPLAYER` | `ON` | `OFF` | — |
| `PELICAN_WITH_IMGUI` | `ON` | `OFF` | — |
| `PELICAN_WITH_PHYSICS` | `ON` | `OFF` | `PELICAN_WITH_JOLT_PHYSICS=OFF`<br>`PELICAN_WITH_BUILTIN_PHYSICS=OFF` |
| `PELICAN_WITH_OPENXR` | `ON` | `OFF` | — |
| `PELICAN_WITH_RENDERDOC` | `ON` | `OFF` | — |
| `PELICAN_WITH_STANDARD_RENDER_ALGORITHMS` | `ON` | `OFF` | — |
| `PELICAN_WITH_JOLT_PHYSICS` | `OFF` | `ON` | `PELICAN_WITH_BUILTIN_PHYSICS=OFF` |
| `PELICAN_WITH_BUILTIN_PHYSICS` | `ON` | `OFF` | — |
| `SKIP_DEVSTUDIO` | `OFF` | `ON` | — |
<!-- PELICAN_FEATURE_REGISTRY_END -->

registry が宣言した連動以外のフラグを対照行で動かしてはならない。

- 完了条件は「ビルド成功 + 全テストグリーン + `git diff --check` クリーン + **文書参照が緑**」

  **(2026-08-19 改訂)全数 GPU 込みを回す頻度を分けること。**
  内周(実装中・変異確認・受け入れ条件の検査)は **`-LE gpu -j16`** で回す(約 9 秒)。
  **`-j4` の全数(GPU 込み、約 346 秒)は統合ブランチへマージする直前に 1 回**でよい。
  GPU・レンダリング・シェーダに触れる WP はこの限りではなく、内周でも GPU を回すこと。
  `doclink audit` は**文書本文を触った WP**でだけ回す。`check` は常に回す。
- **新しく書く主張には §4 規約 10(否定対照)を適用すること。**
  機能が効いていることの主張には、効いていない状態を同じテストの中で実行し、
  結果が異なることを主張する。受け入れ条件には
  「機能が無いときに観測されるはずのもの」を書く。

  ~~描画挙動に触れる WP は `pelican_player.exe` の短時間起動確認も行う~~
  **(2026-08-13 削除)** 名指しのプロジェクトも合格基準も成果物も無く、
  コードを書いたエージェントの自己申告だった。`projects/example` が起動しなくなっていた
  9 日間ずっと有効で、一度も鳴らなかった。
  代わりに `project_catalog_headless_smoke`(WP287 / WP294)が
  指定されたプロジェクト置き場を実際に起動し、単色でないことまで検査する。

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
| WP243a | occlusion を metallicRoughness の R から読むのをやめる | ✅ 完了（2026-08-01）。set 2 binding 7 を独立 occlusion、custom texture を 8 以降へ移動。glTF 部分選択・project material・SPIR-V link ABI・画素閾値を固定。GPU 126/126、非GPU 945/945 |
| WP248 | project 封筒 / path 解決の `pelican_project` 化 | ✅ 完了（2026-08-01）。封筒検証と path / asset-store 純ロジックを移し、engine は adapter 化。`pelican_project` 単独リンク境界を常設 |

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
2. **テクスチャスロットに occlusion を追加する。使う binding 番号を最初に決めること。**
   MATERIAL set は 0=baseColor / 1=metallicRoughness / 2=normal / 3=emissive /
   4=vatPosition / 5=vatNormal / 6=materialBuffer / 7 以降=カスタムテクスチャ
   (`materiallowering.hpp` の `materialCustomTextureFirstBinding = 7`)で
   **空き枠が無い**。カスタムテクスチャを 8 以降へ押すか、別の配置にするかを選ぶ。
   同時に直す必要があるもの: `pelican_sets.glsl` と `pelican_sets.hpp`、
   `materiallowering.hpp`、両方の frag シェーダ、
   `shader_compiler_reflection_test.cpp` の `set2_bindings.size()` 期待値、
   `test/fixtures/material_lowering/` の lowering dump。
   **「一箇所に閉じる」では済まない。触る場所を漏らさず数えること。**
3. **`texturesForMaterials()` も直すこと。** `gltf.cpp` にはテクスチャを 4 枠で
   列挙する箇所がもう一つあり、どの画像を実際に読み込むかをここが決めている。
   occlusion を足し忘れると、node / mesh / material の部分選択ロードで
   `texture_map` が未設定のまま `.value()` に入り `std::bad_optional_access` で落ちる。
4. **occlusion テクスチャが無いときの既定は 1.0(遮蔽なし)。** `gltf.cpp` が
   metallicRoughness に対して行っている白 (255,255,255) の捏造と同じ方式でよいが、
   **既定値の所在を一箇所にすること**(WP240b と同じ規律)。
5. 上の 2 箇所(`default.frag` / `surface_v1.frag`)が**同じ意味論**になること。
   片方だけ直すと経路によって見た目が変わる。
6. `materialformat.cpp` の死んでいる occlusion 記述を、この経路へ接続するか
   削除するかを決めること。**受理するが効かない状態を残さない。**

**forward は範囲外**: forward の lighting は occlusion を読んでいない
(`standard_lighting.glsl` は言及ゼロ、`openpbr_lighting.glsl` は
`surface.occlusion = 1.0;` と書くだけ)。この WP は deferred の G-buffer 経路を直す。
forward へ遮蔽を導入するかは別途決めること。**「forward と deferred で一致」を
この WP の条件にしない** — 現状 forward には一致させる相手が無い。

**受け入れ条件**:

- `DamagedHelmet.glb` が既定構成でベースカラーどおりに描かれる。
  **判定は自動化された閾値で行うこと**: headless 描画の非発光・非背景画素の中央値が
  現状の 8bit 2 以下から明確に外れること(目視の「明るくなった」で済ませない)。
  この判定をテストとして残すか、残さないなら理由を PR 本文に書くこと
- `occlusionTexture` と `metallicRoughnessTexture` が同一画像を指す ORM アセットでも
  正しく遮蔽が効く
- occlusion テクスチャを持たないアセットの見た目が変わらない
- node / mesh / material の部分選択ロードが `occlusionTexture` を持つマテリアルで落ちない
- `gpu` ラベル全数と `ctest -LE gpu` が緑、`git diff --check` クリーン

**既存 golden への影響**: **golden は動かないのが正しい。** golden の
マテリアルは `standardmaterialresource.cpp` の独立した既定 occlusion テクスチャ(白)を
使うため、ao = 1.0 である。既定 metallicRoughness の R も 255 のままだが、WP243a 後は
occlusion として参照しない。
**golden が赤くなったらそれは焼き直す対象ではなく回帰である。** 原因を潰すこと。
これは上の受け入れ条件「occlusion テクスチャを持たないアセットの見た目が変わらない」
と同じことを別の側から言っている。

依存: なし。見積: 中。**最優先** — 仕様準拠の glTF アセットが全滅する欠陥である。

### WP243b: 直接光の拡散項に AO を掛けるのをやめる

**目的**: glTF は `occlusionTexture` を**間接光限定**と定めている。`fullscreen.frag` は
`directDiffuseOcclusion` として直接光の拡散項にも AO を掛けており、仕様違反である。

**実装範囲**:

1. `fullscreen.frag` の 4 箇所(clustered / directional / point / spot)で
   直接光の拡散項から AO を外す。
2. 現状は `directDiffuseOcclusion = openPbrBase ? 1.0 : ao` になっており、
   **OpenPBR 経路は既に正しく、glTF core 経路だけが間違っている**。
   この三項を消して両方が正しい側に揃うこと。
3. **`openPbrBase` そのものは消さないこと。** `fullscreen.frag` の
   `openPbrBase` 出現は 23 箇所あり、その大半は NDF / G / `brdfDenominator` /
   距離減衰といった**シェーディングモデルの選択**である。
   触ってよいのは `directDiffuseOcclusion` の三項だけである。
4. forward は occlusion を読んでいないので範囲外(WP243a と同じ理由)。

**受け入れ条件**:

- 直接光の拡散項に occlusion が掛からない
- `directDiffuseOcclusion` の `openPbrBase` 三項が残っていない。
  **他の `openPbrBase` 分岐は残っていること**
- `gpu` ラベル全数が緑

**既存 golden への影響**: **WP243a と違い、これは画素を動かす。** hybrid_v1 preset を
使う golden は実際に SSAO パスを通るため、ao < 1 の領域が変わる。
更新した golden の新旧を並べ、変化が意図どおりであることを PR 本文で示すこと。
黙って焼き直さない。

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

**名前必須は 1 箇所ではない。範囲を見誤らないこと**:
実装範囲 1 を採っても、それだけで名前の要求が消えるのは**親を持たない
standalone transform の経路だけ**である。少なくとも次の 2 つが独立に名前を要求する。

- `editorruntimefactory.cpp` の `makeTransformBindings` は
  `if (object == scene.objects.end() || !object->name) continue;` で無名を落とし、
  結果に `*object->name` を詰めている。**階層(親付き)オブジェクトの経路**である。
- `editorprojectionadapters.cpp` は
  `"collider authoring object requires a name"` を投げる。**collider の経路**である。

この 2 つを直すのか、明示的に範囲外とするのかを決め、**決めた方を PR 本文に書くこと**。

**検証の落とし穴**: `projects/example` には親付きオブジェクトも collider も無いため、
そこだけで確認すると上の 2 経路が壊れたままでも緑に見える。
**親を持つオブジェクトを含むフィクスチャで検証すること。**

**受け入れ条件**:

- 無名オブジェクトの transform 編集がランタイムへ届く(1 を採る場合)か、
  名前付きハードエラーになる(2 を採る場合)。**黙って成功を返す経路が無いこと**
- 親を持つ無名オブジェクトでも上と同じ結論になる(範囲外とするなら、
  そこで名前付きハードエラーになること)
- `load_scene` の後でも編集がランタイムへ届く
- インスペクタ経由の編集で描画結果が変わることを、画素で確認するテストがあること
  (現状はライブ ECS のコンポーネントまでしか検証されていない)

**画素テストの書き方(実測済み。ここを外すと理由を取り違えて落ちる)**:
編集の commit は **`step_frame` の中でしか起きない**。`invokeEditorCommitQueueHook()` は
`framephase.cpp` の `updateFrameState()` にあり、`rpcserver.cpp` でこれを呼ぶのは
`step_frame` だけで `render_frame` は呼ばない。実測でも、`edit` が accepted を返した後に
`render_frame` を 3 回回して撮った画は編集前と**バイト一致**(authored pos も revision も
据え置き)で、`step_frame` を 1 回入れた瞬間に反映された。
したがってテストは `edit` → **`step_frame`** → `capture` の順で書くこと。
`--size` を固定し、実値で大きな差分が出ることと、値 0.0 の no-op で**バイト一致**になることの
両方を assert すること。`save_scene` はテストに入れないこと(下記のとおり実行時反映には不要)。
- `ctest` 全数が緑、`git diff --check` クリーン

依存: なし。見積: 中。**WP243 とは独立**、並行可。

### WP245: インスペクタのドラッグ中に値が巻き戻る

**目的**: 値をドラッグすると直前の値へ戻ることがある。自分自身のコミットを
「外部からの変更」と誤認して、編集途中のスナップショットを上書きしている。

**現状の経路**:

- `inspector.cpp` の `draw()` が `pollWatch()` を**毎フレーム無条件に**呼ぶ。
  ドラッグ中・プレビュー中のガードが無い。
- `inspector.hpp` の `inspectorWatchPollFrameInterval = 30` により、30 フレームごと
  (60fps で約 0.5 秒ごと)に **watch token** を問い合わせ、前回観測したものと
  違えば `refresh()` を呼ぶ。
- `refresh()` は `selectObject()` を通じて選択オブジェクトのスナップショットを
  **丸ごと差し替える**。ところがドラッグ中の値はまさにそこ
  (`component.authored_json[...] = interaction.value`)に入っている。
- **決め手は watch token の中身である。** `EditorWatchToken` は
  `{scene_revision, preview_epoch}` の複合であり、`preview_epoch` は
  `advancePreviewEpoch()` によって open / **update** / commit / abort の
  すべてで進む。つまりドラッグ中の `update_preview` ごとにトークンが変わるので、
  **ドラッグしている最中のポーリングが毎回「変わった」と判定して `refresh()` を呼ぶ**。
  コミット後の窓の話ではなく、ドラッグ中ずっと起きている。

**実装範囲**:

1. **`pollWatch()` を、プレビュー保持中および編集中のウィジェットがある間はスキップする。
   これが効く修正である。** ドラッグ中に `preview_epoch` が進み続ける以上、
   ここを塞がないと巻き戻りは止まらない。
2. `refresh()` が編集途中のフィールド値を破壊しないこと(保存して復元するか、
   編集中は差し替えない)。
3. 自分のコミット後に新しい watch token を取り込み、自分の変更を外部変更と
   誤認しないこと。**1 の補強であって、単独では症状が止まらない**
   — トークンには `preview_epoch` が含まれており、ドラッグ中に既に食い違っているためである。
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

**現状**: `vkCmdBlitImage` は `src/` 全体に存在せず、ミップ鎖はどこでも生成されない。
サンプラは `mipmapMode = eLinear` / `maxLod = VK_LOD_CLAMP_NONE` を要求しているのに
level 0 しか存在しない。加えて `materialcontainer.cpp` の標準サンプラは
`anisotropyEnable = false` である。

**直す場所を間違えないこと**: glTF の埋め込み画像は `imageloader.cpp` を**通らない**。
`src/core/model/` は `imageloader.hpp` を一切 include しておらず、
`gltfimage.cpp` が `stbi_load_from_memory()` を直接呼び、`gltf.cpp` が
生ポインタを取る `MaterialContainer::registerTexture(extent, data, format, bytes)`
オーバーロードへ渡している。`LoadedImage` / `createTextureResource` には到達しない。
**この WP の目的にとって `imageloader.cpp` を直しても効果はゼロである。**
ミップ生成は生ポインタ側の `registerTexture` 経路に置くこと。
`imageloader.cpp` 経由で読まれる画像(KTX 以外)にも同じ問題があるなら、
そちらも直すのか範囲外とするのかを決めて PR 本文に書くこと。

**実装範囲**:

1. 単一レベルで読み込まれた画像に対してミップ鎖を生成する。GPU の blit 鎖と
   CPU 側生成のどちらを採るかを**最初に決めて理由を書くこと**。
   **画像生成側も直す必要がある** — 現状の `mipLevels()` は
   デコード結果のレベル数をそのまま返すので、鎖を足すには
   image create info の `mipLevels` を先に増やさなければならない。
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

### WP248〜251: devstudio D1(Qt エディタの立ち上げ)

**方針**: [`design_devstudio_direction.md`](design_devstudio_direction.md) の D1
「プロジェクトを開く + 埋め込みビューポートで実エンジン表示 + アウトライナ(読み取り専用)」。
プロセス境界は同文書 §0.6 の決定(**別プロセス + ウィンドウ再親付け**)に従う。

**依存の実状(2026-08-01 調査)**: 「D1 の依存 = 解釈レイヤのターゲット分離(WP44)」は
完了しているが、**それだけでは D1 の看板機能が成立しない**。`project.json` を読むローダは
`src/project/` に 1 件も無く `basicconfig.cpp` にあり、PathResolver も engine 側モジュールである。
「エンジンを起動せずにシーンを表示する」には WP248 が要る。

### WP248: プロジェクト封筒とパス解決を pelican_project へ

**目的**: devstudio が**エンジンを起動せずに**プロジェクトを開けるようにする。D1 の前提。

**現状**:

- `basicconfig.cpp` が `pelican.project` の schema 定数と検証を持つ。`src/project/` 側には無い。
- PathResolver は `pathresolver.hpp` の `DECLARE_MODULE(PathResolver)` で engine モジュール。
  ただし同ヘッダの `parsePathReference()` は既にモジュール外の自由関数で、**純ロジックの
  継ぎ目は既にある**。
- [`implementation_archive.md`](implementation_archive.md) は WP18 の時点で
  「PathResolver(モジュール)は**移動しない**(純ロジック分離は将来の別 WP)」と記録している。
  **本 WP がその「将来の別 WP」である。**

**実装範囲**:

1. `pelican.project` 封筒の解析と検証を `pelican_project` へ移す。**engine 側は移した実装を
   呼ぶだけにする** — 既定値と検証規則の所在を二重化しない(WP240b と同じ規律)。
2. パス参照の解決のうち**純ロジック部分**を `pelican_project` へ分離する。
   モジュールとしての PathResolver は engine 側に残してよいが、その中身は分離した
   純ロジックを呼ぶ形にすること。
3. **`pelican_project` のリンク面を増やさないこと。** 現在は
   `PUBLIC nlohmann_json PRIVATE picosha2` だけで、Vulkan も quill も engine も引いていない。
   ここに何かを足したらこの WP は失敗である。ログが要るなら戻り値で返す。

**受け入れ条件**:

- `pelican_project` だけをリンクした実行ファイルから、project.json を開いて
  シーン一覧とアセット一覧が取れる(既存の `devcli` が先例。engine を引いていない)
- `pelican_project` の `target_link_libraries` が増えていない
- 封筒の検証規則が 1 箇所にしか無い(engine 側に写しが残っていない)
- `ctest` 全数が緑、`git diff --check` クリーン

依存: なし。見積: 中。**WP250 の前提**。

### WP249: devstudio シェル — Widgets 化・D0 リンク境界・ドッキング・レイアウトプリセット

**目的**: 休眠中の Qt 骨組みを、ドッキング可能なシェルとして起こす。
**利用者の要望「GUI のドッキングとかレイアウトプリセットとかほしい」に直接応えるのはこの WP**で、
**ビューポート(WP251)より先に入れられる**。

**現状(調査済み)**:

- 骨組みは 168 行。`uimain.cpp` が `QApplication`(`QGuiApplication` ではない)、
  `mainwindow.hpp` が `QMainWindow` を継承しており、**既に Widgets 主体**である。
  `mainwindow.cpp` の本体は `setCentralWidget(centralQml)` 一行に等しい。
- Qt は `find_package(Qt6 REQUIRED COMPONENTS Core Widgets Quick QuickWidgets QuickControls2)` /
  `qt_standard_project_setup(REQUIRES 6.10)`。**Widgets は既に要求済み**。
- `src/devstudio/CMakeLists.txt` が `pelican_core` をリンクしており、**D0 に違反している**。
  ただし devstudio のどの翻訳単位も Pelican のヘッダを include していない(Qt と argparse と
  自前ヘッダのみ)ので、**ソース変更ゼロで外せる**。
- CI は `docs/ci.md` の 2 箇所で `-DSKIP_DEVSTUDIO=ON` を指定している。

**実装範囲**:

1. **シェルは Qt Widgets、QML は葉のパネル内部に限る**、という規則を決めて文書に書くこと。
   ドッキング(`QDockWidget`)とレイアウト保存(`QMainWindow::saveState`/`restoreState`)は
   Widgets の機能で、QML には標準の対応物が無い。現在のファイル配置は逆を示唆しているので、
   **明示的に書かないと後任が迷う**。
2. `pelican_core` へのリンクを外し、`pelican_project` を明示的にリンクする。
   **core の `pelican_project` は PRIVATE リンクなので `$<LINK_ONLY:>` として伝播し、
   include ディレクトリは devstudio へ届かない。** 明示的に足すこと。
3. **D0 のビルドレベル検査を成立させる。** devstudio が engine のシンボルに触れていないことを
   機械的に確かめる形にすること。「触らないよう気をつける」では検査にならない。
4. ドッキングとレイアウトプリセット。**プリセットの置き場は `user://` にしないこと** —
   `user://` はプロジェクト単位(`%APPDATA%/pelican/<project_name>`)で、project.json に
   `name` が無いと名指しのエラーになり、しかもその解決器は engine モジュールで D0 に反する。
   全体プリセットは devstudio 自身のディレクトリ、プロジェクトごとの最後の配置は
   `<project>/.pelican/`(既に `.gitignore` 済みのツール出力先)を推奨する。
   `saveState`/`restoreState` の版管理を必ず入れること(版無しで復元すると壊れる)。
5. `view/CMakeLists.txt` の `set(CMAKE_AUTOMOC ON)` は**死んでいる** — ターゲットは
   `devstudio/CMakeLists.txt` で先に作られており、この変数は届かない。moc が動いているのは
   `qt_standard_project_setup` の副作用である。ターゲット属性として設定し直すこと。
6. CI の扱いを決めること。既定ビルドを Qt 無しに保つのか、devstudio のコンパイル確認を
   別ジョブで入れるのか。**決めた方を `docs/ci.md` に書くこと。**

**受け入れ条件**:

- devstudio が `pelican_core` をリンクしていない。かつそれが**機械的に検査される**
- パネルがドッキング・タブ化でき、レイアウトを名前を付けて保存・復元できる
- レイアウトファイルに版が入っており、版違いを読んだときに壊れず既定へ落ちる
- `-DSKIP_DEVSTUDIO=ON` の既定ビルドが従来どおり通る
- `ctest` 全数が緑、`git diff --check` クリーン

**テストできることの限界**: Qt の GUI そのものは自動検証しない。**リンク境界の検査と、
レイアウトの保存・復元・版違いの取り扱いは view から切り離せばテストできる**。
受け入れ条件に「ビューポートに絵が出る」のような検証不能な項目を書かないこと。

依存: なし。見積: 中。**利用者の要望に最短で応える WP**。

### WP250: 読み取り専用アウトライナ(pelican_project 直リンク)

**目的**: プロジェクトを開いてシーンとオブジェクトの木を表示する。エンジン起動を要さない。

**特に外しやすい点 — オブジェクトの同一性を名前で引かないこと**:

`sceneformat.cpp` のとおり `name` は**任意**であり、一意性は名前を持つものの間でしか
強制されない。実測では `projects/example/scenes/main.scene.json` の `default_scene` は
**46 オブジェクト中 14 個しか名前を持たない**(親を持つものは 0)。名前をキーにすると
残り 32 個が空キーに潰れる。engine 側には既に代替規則があり、
`authoringscenedocument.cpp` の `runtimeObjectIdentityName` が
`pelican://scene/<id>/authoring-object/<n>` を組む。**アウトライナも
(scene_id, 宣言順の索引) を同一性の基礎にすること。**

**rpc 側との突き合わせは今日できない(D2 への申し送り)**: `editorcommandservice.cpp` の
オブジェクト出力は `name` が無ければ省略し、宣言順の索引を一度も出さない。
`authoring_object_id` はセッション内で単調増加する採番で、文書から再現できない。
直リンクで見た木と rpc で見た木を対応付けるには、**rpc 側に宣言順の索引を出す変更**が要る。
D0 の 2「編集系 rpc を先に定義する」に従い、**その rpc 変更をこの WP で行うか、
D2 の先頭で行うかを決めて書くこと**。

**実装範囲**:

1. WP248 で移した封筒解析を使ってプロジェクトを開き、シーン一覧を出す。
2. シーンごとのオブジェクト木を出す。同一性は上記のとおり。表示名が無いものには
   engine と**同じ規則**の代替名を使うこと(規則を二重に実装しない)。
3. 読み取り専用。この WP では編集しない。

**受け入れ条件**:

- 名前を持たないオブジェクトが 1 つずつ別行として並ぶ(潰れない)
- `projects/example` を開いて 2 シーン・46 と 2 オブジェクトが出る
- モデル層が view から分離されており、上記が headless にテストされている
- devstudio が `pelican_core` をリンクしていない(WP249 の検査が生きている)
- `ctest` 全数が緑

依存: **WP248**(封筒とパス解決)、**WP249**(シェル)。見積: 中。

### WP251: 埋め込みビューポート(別プロセス + ウィンドウ再親付け)

**目的**: エディタ内に本物のエンジンのピクセルを出す。D1 の最後の一片。

**方式**: [`design_devstudio_direction.md`](design_devstudio_direction.md) §0.6 の決定に従い、
devstudio が `pelican_player` を**子プロセス**として起動し、そのウィンドウを Qt のコンテナへ
再親付けする。子プロセスが自分のスワップチェーンへ描き OS が合成するので、
**読み戻しもコピーもプロセス間転送も無い**。

**実装範囲**:

1. 子プロセスの起動・監視・終了。**エンジンが落ちてもエディタが生き残ること**
   (この方式を選んだ利点の 1 つであり、実際にそう振る舞うことを確かめる)。
2. ウィンドウの再親付けと寸法追従。
3. **既存の窓まわりの契約を尊重すること。** WP215(トランザクショナルな window output root /
   frame token)、WP216(nonblocking SwapchainEpoch)、WP217(SurfaceEpoch の再生成と
   support 再検証)が確立した規約の**中で**生きること。親が寸法を変える経路は、
   これらにとって新しい入力である。**独自の再生成経路を作らない。**
4. §4-1 の未決事項に決着をつける。**実測すること** — 入力フォーカス、キーボードの経路、
   DPI、z 順が実用に耐えるか。耐えなければそう報告し、同一プロセス案の再検討材料にすること
   (§0.6 のとおり、その場合に作り直しになるのはビューポート結線だけである)。

**受け入れ条件**:

- Qt のパネル内にエンジンの描画が出て、パネルの寸法変更に追従する
- 子プロセスを強制終了してもエディタが落ちない
- headless / rpc 経路が従来どおり動く(§1-3「headless/rpc 経路には影響を与えない」)
- 既存の window / swapchain のテストが緑のまま
- `ctest` 全数が緑

**テストできることの限界**: 「絵が出る」ことは自動検証しない。**子プロセスの生死、
寸法追従の計算、headless 経路の非回帰**は検証できる。実測結果は PR 本文に数値で書くこと。

依存: **WP249**(シェル)。WP250 とは独立で並行可。見積: 大。

### WP252: 埋め込みビューポートのリサイズを間引く

**目的**: devstudio の分割バーをドラッグすると重い。**マウス移動ごとに swapchain が
作り直されている**ためである。

**発見の経緯**: WP251 完了後、利用者が実際に操作して「ウィンドウの変形が重い」と報告した。
WP251 の実測は「リサイズ**後**に何 ms で寸法が一致するか」を測って 45ms / 最悪 52ms と
報告しており、数値としては嘘ではない。**測っていなかったのは、ドラッグ**中**に何回
再生成が走るか**である。受け入れ条件が「パネルの寸法変更に追従する」止まりで、
連続操作のコストを問うていなかった。**数値が緑でも体感が重い、という乖離はここから来た。**

**現状の経路**:

- `embeddedviewport.cpp` の `resizeEvent()` が Qt のリサイズイベントごとに
  `extent_changed()` を**即座に無条件で**呼ぶ。間引きも遅延も無い。
- `extent_changed` は `resizeEmbeddedWindow()` に繋がっており、
  `nativewindowhost.cpp` の `SetWindowPos` を `SWP_FRAMECHANGED` 付きで呼ぶ。
  これが GLFW の framebuffer callback を発火させ、player 側で swapchain が再生成される。
- 既存の `QTimer` は window discovery / diagnostics / pointer focus 用で、
  リサイズの間引きには使われていない。
- `DevicePixelRatioChange` も同じ `extent_changed()` を呼ぶ。

**実装範囲**:

1. **連続リサイズ中の適用回数に上限を設ける。** 一定間隔に絞る形でよい。
2. **ドラッグ停止後に必ず最終寸法を適用すること。** 間引きの副作用で最後の寸法が
   落ちるのは論外である。静止状態での正しさは今までどおり厳密であること。
3. **`DevicePixelRatioChange` は間引きの対象外としてよい**(頻度が低く、
   取りこぼすと表示が壊れる)。判断して理由を書くこと。
4. **swapchain 再生成を devstudio が持たないこと**(WP251 の契約、WP215〜217)。
   間引くのは「いつ `SetWindowPos` を呼ぶか」だけである。**再生成そのものに手を入れない。**
5. 間引きの判定を**純ロジックとして分離すること**。`viewportgeometry.{hpp,cpp}` が
   既に純ロジックの置き場になっているので、そこが自然である。

**受け入れ条件**:

- **連続リサイズ中の適用回数に上限があり、それがテストで検証されている。**
  一連の寸法変化を与えて、適用回数が入力イベント数より十分少ないことを assert すること
- **最後に与えた寸法が必ず適用される**(静止時の寸法が厳密に一致する)
- `DevicePixelRatioChange` の扱いが決まっていて、理由が書かれている
- devstudio 側に swapchain / surface の再生成が無いまま
- WP251 が追加した既存テストが緑のまま
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` の構成でも通ること)、`git diff --check` クリーン

**範囲外**: player の Debug/Release 選択には触れない。`PELICAN_STUDIO_PLAYER` と
`PELICAN_STUDIO_PLAYER_ARGUMENTS` で既に上書きでき、`devstudio` は開発者向けツールなので
Debug の player を起動するのはむしろ正しい。**Debug がこの重さにどれだけ寄与しているかは
未実測であり、この WP では原因として扱わない。**

依存: なし(WP251 はマージ済み)。見積: 小。

### WP253: ミップ生成を GPU の blit 鎖へ移す

**目的**: WP246 が入れた **CPU box filter によるミップ生成が、起動時間を倍にしている**。
GPU の blit 鎖へ移して戻す。

**実測(2026-08-02。同一 Debug ビルド、同一プロジェクト、シェーダキャッシュ温、各 3 回)**:

| | 起動時間 |
|---|---|
| WP246 **前**(`agent/wp243b` の player) | 3.93 / 3.85 / 3.94 秒 |
| WP246 **後**(統合ブランチ) | 8.04 / 7.79 / 7.66 秒 |

区間の内訳もログのタイムスタンプ差から取れている。

- **エンジン自体の起動は 1.7 秒**(モデルを持たない空シーン)。Vulkan の instance / device
  生成は合計 0.2 秒程度で、ここは問題ではない
- 残りは **glTF モデル 1 個の読み込み**。WP246 前で 2.2 秒、後で 6.1 秒
- `DamagedHelmet.glb` は **2048×2048 の JPEG を 5 枚**持つ。約 2100 万画素を
  最適化無効の Debug ビルドで畳み込んでいる

**なぜこうなったか**: WP246 の受け入れ条件に **起動時間・読み込み時間への影響が入っていなかった**。
実装は「GPU の blit 鎖と CPU 側生成のどちらを採るか最初に決めて理由を書くこと」という条件に
従って CPU を選び、sRGB の扱いを理由として書いている。**判断の筋は通っていたが、
その選択の代償を測らせる条件が無かった。** WP252 と同じ抜け方である。

**実装範囲**:

1. ミップ生成を `vkCmdBlitImage` の鎖へ移す。転送コマンドを積むだけにし、
   **CPU で画素を畳み込まないこと**。
2. **sRGB の扱いを仕様で確認して結論を書くこと。** WP246 が CPU を選んだ理由がここである。
   現行実装は UNORM 格納値を縮小し sRGB 変換をサンプリング時に任せている
   (= 符号化された値を平均している)。blit が sRGB フォーマットに対して
   どの空間でフィルタするかを **Vulkan 仕様で確認**し、現行と同じか・より正しいか・
   より悪いかを判定して書くこと。**推測で進めないこと。**
3. **フォーマットが線形フィルタ blit を支援しない場合の経路を持つこと。**
   `vkGetPhysicalDeviceFormatProperties` で確認し、支援しないフォーマットで
   黙って壊れた絵を出さないこと。落とすなら名前付きハードエラー、
   CPU へ落とすならそう書くこと。
4. KTX のように既にミップを持つ画像を二重に生成しないこと(WP246 の性質を維持)。
5. WP246 が追加したテストの意図(ミップが生成されていること、既存経路が変わらないこと)を
   維持すること。box filter の画素値を直接期待している assert があるなら、
   **GPU 生成でも成立する形に書き換える**こと。期待値を消して通すのではない。

**受け入れ条件**:

- **モデル読み込み時間が WP246 前の水準に戻ること。** 上の実測と同じ条件
  (同一 Debug ビルド、同一プロジェクト、キャッシュ温、3 回)で計測し、
  **数値を PR 本文に書くこと**。目安は WP246 後の 7.8 秒から 4 秒台へ戻ること
- sponza のモアレ低減(WP246 の成果)が維持されていること。
  WP246 は壁面 ROI の平均エッジ差 5.67 → 2.98 を報告している。**同等であること**
- 線形フィルタ blit を支援しないフォーマットで黙って壊れないこと
- 既にミップを持つ画像の経路が変わらないこと
- `gpu` ラベル全数と `ctest -LE gpu` が緑、`git diff --check` クリーン

**golden への影響**: WP246 は golden を 1 件も動かさなかった。フィルタの実装が変われば
画素が動く可能性がある。**動いた場合は新旧を並べて理由を書くこと。黙って焼き直さない。**

依存: なし(WP246 はマージ済み)。見積: 中。**今日入れたばかりの回帰なので優先**。

### WP254: 長いパスでシェーダキャッシュが黙って死ぬ

**目的**: プロジェクトが深い場所にあると、シェーダキャッシュの書き込みが**毎回失敗**し、
**毎回全シェーダを再コンパイル**する。しかもそれが分かる形で報告されない。

**実測(2026-08-02)**:

| プロジェクトの置き場所 | `recompiling` 警告 | 生成されたキャッシュ |
|---|---|---|
| 229 文字のパス | **15 回**(毎回) | **0 件** |
| `C:/pt/` | 0 回 | 15 件 |

ディレクトリは作られるのに中身が 0 件のまま、という状態になる。

**現状の経路**:

- `shadercompiler.cpp` が一時ファイル名を `path + ".tmp-" + nonce` で作る。
  キャッシュパス自体が 229 文字あるところに接尾辞と nonce が乗り、
  Windows の `MAX_PATH`(260)を超える。**拡張長パス(`\\?\`)は使っていない。**
- `std::ofstream` が開けず `temporary entry cannot be opened` を返し、
  `warnCacheOnce()` が警告して**再コンパイルに落ちる**。
- その `warnCacheOnce` の `warned` フラグは**コンパイル呼び出しごとのローカル変数**である。
  つまり「once」はプロセス単位ではなく**シェーダ単位**で、N 個のシェーダに N 回警告が出る。
  一方で「**キャッシュが全く効いていない**」という一言はどこにも出ない。

**再コンパイルへ落ちること自体は正しい。** キャッシュは最適化であって、
書けないことを致命エラーにするのは誤りである(§0 の「シェーダコンパイル失敗のみ
result 返却」と同じ精神)。**問題は、致命的でない失敗が診断可能な形で表に出ないこと**である。

**実装範囲**:

1. **長いパスで書けるようにすること。** 拡張長パスの前置、一時ファイル接尾辞の短縮、
   キャッシュファイル名の短縮のいずれか、または組み合わせ。**選んだ理由を書くこと。**
2. **キャッシュが機能していないことを 1 回だけ、明確に報告すること。**
   シェーダごとの警告を N 回出すのではなく、プロセスで 1 回「キャッシュは無効である/理由」と
   分かる形にすること。現状の `warned` がローカル変数である点を直すこと。
3. **致命エラーにしないこと。** 再コンパイルへの縮退は維持する。
4. キャッシュが効いている通常経路の挙動を変えないこと。

**受け入れ条件**:

- 長いパス(260 文字を超える一時ファイル名になる配置)でキャッシュが**生成される**
- 上の実測と同じ方法で確認できること。すなわち、深いパスに置いたプロジェクトを
  2 回起動し、2 回目に `recompiling` が出ず、キャッシュディレクトリにファイルがあること
- 何らかの理由でキャッシュが使えないとき、**プロセスで 1 回だけ**、
  理由の分かる報告が出ること(シェーダ数だけ繰り返さない)
- キャッシュ失敗が致命エラーになっていないこと
- 長いパスの経路がテストで検証されていること
- `ctest` 全数が緑、`git diff --check` クリーン

**なぜ気付かれなかったか**: この症状は検証用プロジェクトを深いスクラッチパッドへ置いた
ときに初めて表面化した。通常の `projects/` 配下では起きない。**Windows で長いパスは
珍しくないので、利用者の環境で静かに起きうる。**

依存: なし。見積: 小。

### WP255: ドラッグ中はビューポートを凍結する

**方針(2026-08-03 ユーザー決定)**: 「**変形中に描画する必要はない。変形が終わってから描画すれば
変形中は快適**」。この判断に従う。

**なぜ WP252 では足りなかったか**: WP252 は適用を 20Hz に絞り、200 イベント → 5 適用を達成した。
**回数は減ったが体感は改善せず、新たに見た目の問題が出た。** 利用者の報告は
「移動した範囲に UI がいっぱいされるし、なんかラグい感じがする」。

ドラッグ中に 5 回リサイズするというのは**両方の悪いところ取り**である。再生成のコストは払うのに、
追従は間に合わない。**片側に振り切る** — ドラッグ中は一切触らず、終わってから 1 回だけ適用する。

**受け入れ条件に「適用回数の上限」を書いたのが誤りだった。** 上限は満たされたが、
利用者が求めていたのは滑らかさである。**代理の数値が目的を置き換えてしまった。**

**現状の問題は 3 つある**:

1. **coalescer が throttle であって debounce ではない。** `viewportgeometry.hpp` の
   `EmbeddedViewportResizeIntervalMs = 50` は「最小間隔」であり、ドラッグ中も 50ms ごとに
   適用が走る。必要なのは「**静止するまで適用しない**」である。
2. **露出領域を塗る担当が誰もいない。** `embeddedviewport.cpp` の `NativeViewportSurface` は
   `setAttribute(Qt::WA_OpaquePaintEvent)`(= 背景消去は不要、全画素を自分で塗る)と
   `setAutoFillBackground(true)`(= 背景を塗ってほしい)を**同時に設定**しており、
   さらに `paintEvent` を実装していない。子ウィンドウが覆っていない領域には**前の画素が残る**。
   これが「移動した範囲に UI がいっぱいされる」の正体である。引き伸ばしではなく消し忘れ。
3. **子プロセスが残る。** studio 終了後も `pelican_player` が生き残っている例を観測した
   (親の studio が存在しない player プロセス)。WP251 は「エンジンが落ちてもエディタが
   生き残ること」を条件にしたが、**逆方向(エディタ終了時に子を確実に終わらせる)を
   条件にしていなかった**。起動のたびに player が積み上がる。

**実装範囲**:

1. **リサイズ適用を debounce にする。** 寸法変更が続いている間は `SetWindowPos` を
   **一度も呼ばない**。静止してから 1 回だけ適用する。
   `ViewportResizeCoalescer` は最小間隔方式なので、論理の置き換えが要る。
   純ロジックのまま(テスト可能なまま)にすること。
2. **露出領域が塗られるようにする。** `WA_OpaquePaintEvent` と `setAutoFillBackground` の
   矛盾を解消し、子が覆っていない領域にビューポート背景(黒)が出るようにすること。
   **前の内容が残らないこと**が要件であって、手段は問わない。
3. **studio 終了時に子 player を確実に終わらせること。** 正常終了・異常終了の両方で。
4. `DevicePixelRatioChange` は従来どおり即時適用でよい(WP252 の判断を維持)。
5. swapchain 再生成そのものには触れないこと(WP251 / WP215〜217 の契約)。

**受け入れ条件**:

- **連続したリサイズ中の適用回数が 0 であること。** 上限ではなく 0。
  **これは現在のビルドで確実に落ちる条件である**(いまは 5 回)
- 静止後に**ちょうど 1 回**適用され、最終寸法が厳密に一致すること
- 子が覆っていない領域に**前の内容が残らないこと**。テストで検証できる形にすること
  (描画そのものを assert できないなら、属性の組み合わせが矛盾していないことでもよい。
  **何を検証したのか PR 本文に書くこと**)
- studio 終了後に `pelican_player` プロセスが残らないこと
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` の構成でも通ること)、`git diff --check` クリーン

**この WP で測ってはいけないもの**: 「適用回数の上限」。WP252 がそれで通ってしまった。
0 か 1 かを数えること。

依存: なし(WP252 はマージ済み)。見積: 小。

### WP257: ビューポートの最小寸法で操作が破綻する

**症状(利用者報告)**: 「横方向の幅に最小値があるのか、そのときだけ縮めようとすると
**両辺動く**のが変な気がする」。

**原因**: `embeddedviewport.cpp` の `NativeViewportSurface` が `setMinimumSize(160, 90)` を
持つ。ビューポートがこれ以上縮められなくなると、Qt のドックレイアウトは行き場を失った分を
**反対側へ配分**する。それが「両辺が動く」の正体である。Qt としては通常の挙動だが、
分割バーを掴んでいる利用者にとっては予期しない動きになる。

**160×90 という値に根拠が見当たらない。** swapchain の下限は
`swapchainframetarget.cpp` が読む `capabilities.minImageExtent` であって、
通常これは 1×1 である。この定数は「なんとなく安全そうな値」以上の裏付けを持たない。

**実装範囲**:

1. **最小寸法の根拠を決めること。** 下げるなら、どこまで下げてよいかを
   `minImageExtent` と実際の挙動から決め、**理由を書くこと**。単に小さい値へ置き換えて
   終わりにしないこと。
2. **限界に達したときの挙動を、驚かない形にすること。** 反対側が動くのが避けられないなら、
   そもそも限界に達しにくくする。避けられるなら、ドラッグがそこで止まるようにする。
   **どちらを採ったか PR 本文に書くこと。**
3. **極小・ゼロ寸法で壊れないこと。** 既存テストは
   `embeddedViewportPixelExtent(QSize{0, 451}, 1.25)` のようにゼロ幅を通しており、
   `SetWindowPos` にもゼロが渡りうる。player が落ちたり swapchain が壊れたりしないことを
   確かめること。ウィンドウ最小化と同じ経路のはずなので、**既存の扱いを調べてから決めること**。

**受け入れ条件**:

- ビューポートを縮める操作で、通常の範囲では反対側の辺が動かないこと
- 最小寸法の値に理由が書かれていること
- 極小・ゼロ寸法にしても player が生き続けること
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` の構成でも通ること)、`git diff --check` クリーン

依存: なし。見積: 小。専有: `src/devstudio/`。

### WP258: オブジェクトの宣言順索引を rpc に出す

**目的**: 直リンクで読んだシーンの木と、rpc が返す木を**対応付けられるようにする**。
D2(選択・ギズモ・プロパティ編集)全体の前提であり、これが無いと着手できない。

**経緯**: WP250 のアウトライナは `pelican_project` を直リンクして
`(scene_id, 宣言順の索引)` でオブジェクトを同定する。名前は任意で、
`projects/example` の `default_scene` は 46 個中 14 個しか名前を持たないためである。
一方 rpc のオブジェクト出力は `authoring_object_id` と、**名前があるときだけ** `name` を返す。
**両者を突き合わせる手段が無い。** WP250 はこれを「D2 の先頭で行う契約変更」として申し送った。

D0 の 2「**編集系 rpc が本当の API**。エディタ内部関数ではなく rpc メソッドとして先に定義し、
devstudio はそれを呼ぶだけにする」に従い、**先に rpc を定義する**。

**これは情報の追加であって、新しい概念の発明ではない**:

- `authoringscenedocument.hpp` は `declaration_index` を**既に第一級の概念として持つ**
  (`previous_declaration_index` まである)。
- `authoringscenedocument.cpp` の挿入は
  `objects.insert(objects.begin() + declaration_index, ...)` であり、
  `authoring_object_id` はそれとは**独立に**単調増加で払い出される。
  初回ロードでは一致するが、途中挿入が起きた瞬間にずれる。
  **つまり id を宣言順の代わりに使うことはできない。**
- `editorcommandservice.cpp` の出力に `declaration_index` は**一度も現れない**。
  内部にある情報を公開面に出していないだけである。

**実装範囲**:

1. オブジェクトを返す rpc 出力に**宣言順の索引を含める**。
   対象は少なくともシーン木の問い合わせとコンポーネント問い合わせ。
   **どのメソッドに足したかを列挙すること。**
2. **形式の版の扱いを決めること。** rpc の応答形式に版があるなら、
   フィールド追加が版の変更に当たるかを判断し、理由を書くこと(strict v1 の規律)。
3. 直リンク側(`pelican_project`)と rpc 側で、**同じオブジェクトが同じ索引になること**。
   規則を二重に実装しないこと — WP250 が代替名の規則を `pelican_project` に置いて
   engine 側を呼び出しだけにしたのと同じ形を採ること。
4. **`authoring_object_id` は残すこと。** セッション内で安定した参照として使われている。
   置き換えではなく追加である。

**受け入れ条件**:

- 名前を持たないオブジェクトを、直リンクの木と rpc の木で**突き合わせられる**こと。
  `projects/example` の `default_scene`(46 個中 14 個だけ名前あり)で検証すること
- 途中挿入の後でも索引が宣言順を表していること(`authoring_object_id` とずれても正しいこと)
- 既存の rpc 利用者(`tools/pelican_rpc.py`、既存テスト)が壊れないこと
- `ctest` 全数が緑、`git diff --check` クリーン

依存: なし(WP250 はマージ済み)。見積: 小。**D2 全体の前提**。

### WP259: 実行中のフレームが参照しうる descriptor set を解放している

**目的**: リサイズ時に、**まだ GPU が読んでいるかもしれない descriptor set を解放**している。
正しさの問題であり、性能の話より優先する。

**現状**:

- `fullscreenpasscontainer.cpp` の descriptor pool は
  `ci.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;` で作られる。
  つまりセットは破棄時に実際に解放される。
- 同ファイルの `input_textures.insert_or_assign(pass_id.value, std::move(info));` が
  **直前のエントリを即座に破棄**する。`rendertarget.hpp` の
  `constexpr size_t in_flight_frames_num = 2;` により、**最大 2 フレームが実行中**でありうる。
- `materialcontainer.cpp` の `screen_input_descriptors` にも同型の差し替えがある。

**正しい形はこのリポジトリの中にある**: `computetask.cpp` は新しいプールを確保し、
古いプールとセットを `RetiredComputeDescriptorResources` へ移して
`GET_MODULE(DeletionQueue).defer(std::move(retired));` する。この engine の DeletionQueue は
**リース方式**(`leaseForNextSubmission` / `confirmSubmission`、最後のリースが落ちたら解放)であり、
まさにこのためにある。`fullscreenpasscontainer.cpp` にはこの退避が無い。

**実装範囲**:

1. fullscreen と material の両方で、差し替え時に古い descriptor 資源を**即座に破棄せず退避**すること。
2. **`computetask.cpp` の既存の形を踏襲すること。** 新しい退避機構を発明しないこと。
   可能なら共通化すること(規則を三重に持たない)。
3. `eFreeDescriptorSet` を残すか、プールごと退避する方式に寄せるかを決め、**理由を書くこと**。

**受け入れ条件**:

- 実行中のフレームが参照しうる descriptor set が解放されないこと
- 退避が `DeletionQueue` のリースに載っていること(フレーム数の決め打ちで待たないこと)
- リサイズを繰り返しても descriptor 資源が際限なく積み上がらないこと
- `gpu` ラベル全数と `ctest -LE gpu` が緑、`git diff --check` クリーン

**検証の限界**: use-after-free は再現しないことが多い。**Vulkan validation layer を有効にして
リサイズを繰り返す**確認を行い、結果を PR 本文に書くこと。「落ちなかった」だけでは不十分である。

依存: なし。見積: 中。**最優先** — 正しさの問題である。

### WP260: 発火しえない指紋の自己再検証を外す

**目的**: 再 lower の **24%** が、**構造上絶対に発火しない検査**に使われている。

**現状(実測)**: `vulkanrendercompilerprogram.cpp` が

```
if (context.automatic_plan->logical_graph_fingerprint !=
    vulkanTargetPlanLogicalGraphFingerprint(*context.logical_graph)) {
```

を実行する。この `verification_contexts` は `renderingsamplecount.cpp` により、
**指紋を埋めたのと同じ `compileRenderingTargetPlansForVulkanDevice` 呼び出しの中で**積まれる。
つまり**数マイクロ秒前に自分が作った値を再計算して比較**している。
デバッグ用のガードもフラグも無い。**指紋関数が非決定的でない限り発火しえない。**

**実装範囲**:

1. この自己再検証を外すか、デバッグ構成限定にすること。**どちらを採ったか理由を書くこと。**
2. 指紋関数の決定性そのものを確かめたいなら、**それは単体テストの仕事**である。
   毎フレーム経路で払うコストではない。テストが無いなら足すこと。

**受け入れ条件**:

- 再 lower の実測時間が短くなること。**同一条件で計測して数値を PR 本文に書くこと**
  (単体 `pelican_player` のウィンドウリサイズ、3 回以上)
- 指紋の決定性がテストで担保されていること
- `ctest` 全数が緑

依存: なし。見積: 小。

### WP261: 指紋を JSON dump 経由で作るのをやめる

**目的**: 再 lower の **23.7%** が、nlohmann の文書を構築して `.dump()` し、
その文字列をハッシュする処理に使われている。

**現状**: `targetrenderplanning.cpp` が
`fingerprint.appendString(compiledLogicalRenderGraphToJson(graph).dump());`、
`vulkanphysicalfragment.cpp` にも同型がある。文書構築と文字列化が本体コストで、
ハッシュそのものではない。

**実装範囲**:

1. 構造から直接ハッシュを取る形にすること。JSON の中間表現を作らないこと。
2. **指紋の意味論を変えないこと。** 同じ入力が同じ値になり、異なる入力が異なる値になること。
   既存の指紋値が変わるのは構わないが、**変わったことを明示すること**(永続化されている場合は
   互換性の判断が要る)。
3. WP260 が自己再検証を外した後なら、指紋は「異なるものを異なると判定する」ためだけに使われる。
   その用途に足りる形にすること。

**受け入れ条件**:

- 再 lower の実測時間が短くなること。**数値を PR 本文に書くこと**
- 同一入力で同一値、異なる入力で異なる値になることがテストで担保されていること
- 指紋が永続化される経路があるなら、その互換性の扱いが決まっていること
- `ctest` 全数が緑

依存: **WP260**(先に自己再検証を外さないと、指紋を変えた影響が二重に出る)。見積: 中。

**WP256(`extent` を compile-facts の指紋から外す)は取り下げ**: 実測により、再 lower の
**約 97% は extent に依存しない処理**であることが分かった。決定的な証拠は
**640×360 と 3840×2160 で処理時間が 9ms しか変わらない**(画素数 36 倍)ことである。
レンダーターゲット画像の再作成は全体の **1.3%** にすぎない。
extent を指紋から外しても、外した先の処理がそのまま走る。WP259〜261 を先に済ませ、
そのうえで残りが問題なら改めて検討する。

### WP262: ID バッファによるピッキング(render feature)

**目的**: 視覚的なピッキングを engine 側に用意する。**D2(選択・ギズモ・プロパティ編集)の
最後の前提**であり、これが無いとビューポート上でオブジェクトを選べない。

**設計上の位置付け**: [`design_devstudio_direction.md`](design_devstudio_direction.md) §2 が
「ピッキング(視覚) = **ID バッファパス = render feature**
(`engine://features/picking.json`)+ 読み出し。**パージ可能**・エディタ以外
(デバッグ)にも使える」と定めている。**エディタ専用の裏口にしないこと**(D0)。

**現状**: `src/core/resources/features/` に `picking.json` は**存在しない**。
既存の feature(`debug_draw.json` など)が形の手本になる。

**実装範囲**:

1. `engine://features/picking.json` を render feature として新設する。
   **既存 feature と同じ形**(`pelican.render_feature` v1、`passes` に `insert` で差し込む)。
2. オブジェクト識別子を書き出す ID バッファのパスを持つこと。
   **何を ID として書くかを決めて書くこと** — WP258 で rpc に出した
   `(scene_id, declaration_index)` と対応が取れる形が望ましい。
   対応が取れないなら、その理由と代替の対応付け手段を書くこと。
3. **読み出し経路**を用意すること。座標を指定して ID を得る形。
   同期・非同期のどちらにするかを決め、理由を書くこと。
4. **パージ可能であること。** feature を外せば ID バッファのコストが完全に消えること。
   常時有効にしないこと。
5. エディタ以外(デバッグ)からも使えること。devstudio 専用の API を作らないこと。

**受け入れ条件**:

- feature を有効にしたとき、指定座標のオブジェクト識別子が取れること
- feature を外したとき、ID バッファのターゲットもパスも**存在しない**こと
  (`--dump-frame-plan` などで確認できる形が望ましい)
- 取得した識別子が、WP258 で rpc が返す識別子と対応付けられること。
  対応付けの手順をテストで示すこと
- 既存の golden が動かないこと(feature 無効が既定であるため)
- `gpu` ラベル全数と `ctest -LE gpu` が緑、`git diff --check` クリーン

**範囲外**: 物理クエリ(raycast)による論理ピッキング。設計文書 §2 は視覚と論理の
両方を持つと定めているが、これは視覚側のみである。

依存: なし(WP258 はマージ済み)。見積: 中。**D2 の前提**。

### WP263: 子 player のログをパネルに出す

**目的**: studio を起動しても**エンジンの出力がどこにも見えない**。何か起きても追えない。

**現状**: 配管は既に通っている。`engineprocess.cpp` の `drainOutput()` が
`process_.readAllStandardOutput()` で捕捉し `outputReceived` シグナルを出している。
ところが受け側は `status_->setToolTip(recent_output_);` — **ステータスラベルの
ツールチップに入れているだけ**で、しかも `clippedOutput()` が末尾 4000 文字へ切り詰める。
**出口が無いだけである。**

**なぜ要るか**: 本日 studio を複数回起動して確認したが、
「クラッシュせずに終了した」以上のことが分からなかった。
D2 で選択・ギズモ・編集を積む前に、**詰まったときに追える土台**が要る。

**実装範囲**:

1. ログをドックパネルとして出す。既存の 4 パネルと同じ作法
   (`makeDock`、閉じられる・移動できる・フロートできる)。
2. **切り詰めの方針を決めること。** 4000 文字で捨てるのは表示用の都合であって、
   パネルには履歴が要る。上限行数か上限バイト数を決め、理由を書くこと。
   **無制限にしないこと**(長時間起動でメモリを食う)。
3. stderr も捕捉すること。現在は `readAllStandardOutput()` のみで、
   **エラー出力が捨てられている可能性がある**。確かめて、捨てていれば拾うこと。
4. レイアウトプリセット(WP249)と整合すること。新パネルが既存の保存済みレイアウトを
   壊さないこと(版の扱いは WP249 が既に持っている)。

**受け入れ条件**:

- エンジンの出力がパネルに表示され、スクロールできること
- stderr が失われないこと
- 長時間起動でログが無制限に増えないこと
- 既存のレイアウトプリセットを読んでも壊れないこと
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` の構成でも通ること)、`git diff --check` クリーン

**テストできることの限界**: GUI 表示そのものは自動検証しない。
**切り詰めの規則と、stderr を含む出力の取り回しは view から切り離せばテストできる。**

依存: なし。見積: 小。**D2 の前に入れる価値がある** — 追えない状態で複雑な機能を積まないため。

### WP264: D2 の選択 — ビューポートとアウトライナの双方向同期

**目的**: ビューポートをクリックしてオブジェクトを選び、アウトライナと Inspector に反映する。
逆にアウトライナで選んでもビューポートに反映する。**D2 の入口**であり、
プロパティ編集とギズモはこの上に載る。

**前提はすべて揃っている**:

- **WP262**: `pick_object` rpc(座標 → 識別子)。汎用 rpc で devstudio 専用ではない
- **WP258**: `scene_tree` / `get_components` が `declaration_index` を返す
- **WP262 の識別子は WP258 のものと一致する** — 第二の同一性スキームは不要
- **WP250**: アウトライナが `(scene_id, declaration_index)` で同定している
- **WP263**: 詰まったときに追えるログパネル

**実装範囲**:

1. ビューポートのクリック → `pick_object` → 選択状態の更新 → アウトライナと Inspector に反映。
2. アウトライナの選択 → ビューポートへ反映。**何をもって「反映」とするかを決めること** —
   この WP では枠線などの視覚表現は必須にしない(ギズモの WP で扱う)。
   最低限、選択状態が一元管理され両者が同じものを指すこと。
3. **選択状態の置き場を決めること。** devstudio 側に持つのか、engine 側に持って rpc で問うのか。
   D0 に照らして判断し、理由を書くこと。**devstudio だけが持てる状態にしないこと。**
4. 何も無い場所をクリックしたときの扱い(選択解除か、維持か)を決めること。
5. `picking` feature が無効なプロジェクトでの挙動を決めること。
   **黙って何も起きないのは不可**(fail-silent は今日 WP244 で潰した類の欠陥)。

**受け入れ条件**:

- ビューポートのクリックで選ばれたオブジェクトと、アウトライナの行が一致すること
- アウトライナで選んだものがビューポート側の選択状態にも反映されること
- **名前を持たないオブジェクトでも正しく選べること**
  (`projects/example` は 46 個中 14 個しか名前を持たない)
- `picking` feature が無い場合の挙動が定義され、黙殺でないこと
- モデル層が view から分離され、選択の同期が headless にテストされていること
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` の構成でも通ること)、`git diff --check` クリーン

依存: WP258 / WP262(いずれもマージ済み)。見積: 中。**D2 の入口**。

### WP265: ビューポートの視点移動

**目的**: エディタのビューポートで視点を動かせるようにする。

**エンジン側の機能は既に全部ある**(2026-08-05 調査):

- `camera.hpp` の `SceneCameraControllerType` に **`Orbit` / `Follow` / `Fly`**
- `cameracontrollersystem.cpp` の `updateFly` が **`look` / `move` の入力アクションを読んで**
  yaw / pitch と位置を更新する。`sensitivity` と `speed` もコントローラの宣言値
- 入力プロファイルは project 空間の宣言(`input/profiles/*.json` +
  `basic_config.input_profiles`)で、`--input-profile` で切り替えられる

**したがってエンジンに新機能は要らない。** 必要なのは
「**利用者のプロジェクトを書き換えずに**、エディタ用のカメラと入力を与える」ことである。

**これが判断の要る点**: エディタが開くのは利用者のプロジェクトであり、
そのシーンのカメラに `controller: {type: "fly"}` を勝手に書き込むことはできない。
**プロジェクトは編集対象であって、エディタの都合で汚してよいものではない。**

**実装範囲**:

1. **プロジェクトを変更せずにエディタ用カメラを有効にする道を決めること。** 候補:
   起動時のオーバーレイ(`featurecompose` の `transform_resolved_config` に相当する仕組みが
   レンダリング設定側にはある)、engine 側の既定として持つ、rpc で実行時に注入する、など。
   **選んだ理由を書くこと。** 利用者のシーン JSON をディスク上で書き換える案は採らないこと。
2. 入力プロファイルも同様に、プロジェクトへ書き込まずに与えること。
3. **エディタ以外からも使えること**(D0 と同じ規律)。デバッグ用のフリーカメラは
   エディタ専用にする理由がない。`picking` feature が
   「パージ可能・エディタ以外にも使える」形で入ったのと同じ考え方。
4. 操作方式(軌道か自由飛行か、両方か)を決めること。**既存の 3 種から選ぶこと。**
   新しいコントローラ型を足す必要があるなら、その理由を書くこと。

**受け入れ条件**:

- ビューポートで視点を動かせること
- **利用者のプロジェクトのファイルが 1 バイトも変わらないこと**。これを検証すること
- エディタ以外(素の player)からも同じ手段で使えること
- 既存のシーンカメラの挙動が変わらないこと
- `ctest` 全数が緑、`git diff --check` クリーン

**範囲外**: 選択したオブジェクトへのフォーカス(F キー相当)。選択が入ってからでよい。

依存: なし。WP264 とは独立で並行可。見積: 中。

### WP266: D2 のプロパティ編集 — devstudio の Inspector

**目的**: 選択したオブジェクトのコンポーネント値を devstudio で見て編集できるようにする。
**D2 の残り半分**(もう半分はギズモ)。

**現状**: Inspector パネルには `Selection` ラベルと `No editable properties` の固定文字しかない。
WP264 で選択は同期するようになったが、**値が出ない**。

**公開面は既に揃っている**(2026-08-05 調査): `editorrpchandlers.cpp` が編集系 rpc を
**22 メソッド**登録済みである。`scene_tree` / `get_components` / `edit` /
`open_preview` / `update_preview` / `commit_preview` / `abort_preview` /
`get_edit_result` / `get_preview_result` / `undo` / `redo` / `save_scene` ほか。
**D0 の「編集系 rpc が本当の API」は満たされている。足りないのは devstudio 側だけである。**

**手本が engine 内にある**: ImGui 側のインスペクタ(`inspector.hpp` / `inspector.cpp`)は
**スキーマ駆動**でウィジェットを組み立てる。`InspectorWidgetKind`
(整数 / 浮動小数 / 真偽 / 列挙 / 文字列 / ベクトル / クォータニオン)と
`json_pointer` を持つ `InspectorWidgetDescriptor` の列を、コンポーネントのスキーマから作る。
**同じ考え方を Qt 側で実装すること。ウィジェットの種類を手で列挙しない。**

**実装範囲**:

1. 選択されたオブジェクトのコンポーネントを `get_components` で取り、
   **スキーマからウィジェットを組み立てて**値を表示する。
2. 編集を `edit` へ送る。ドラッグ操作はライブプレビュー
   (`open_preview` / `update_preview` / `commit_preview` / `abort_preview`)を使うこと。
3. **本日直した 2 つの欠陥を再発させないこと**:
   - **WP245**: プレビュー保持中・ウィジェット編集中は再取得で値を上書きしないこと。
     ImGui 側は `inspectorRefreshBlocked()` で解決している。**同じ罠が Qt 側にもある。**
   - **WP244**: 届かなかった編集が成功を返さないこと。
4. `undo` / `redo` を配線するかを判断すること。しないなら理由を書くこと。
5. **編集の適用は `step_frame` の中でしか起きない**(`invokeEditorCommitQueueHook` が
   `updateFrameState()` にあり、`render_frame` は呼ばない)。ビューポートの player を
   どう進めるかを確かめること。**ここを外すと「コミット済みなのに絵が変わらない」になる。**

**受け入れ条件**:

- 選択したオブジェクトの transform の値が表示され、編集すると**ビューポートに反映される**こと
- ドラッグ中に値が巻き戻らないこと。**外部変更の反映は殺さないこと**(WP245 と同じ二本立て)
- 届かなかった編集が成功として表示されないこと
- ウィジェットの種類がスキーマから決まること(手書きの分岐で型を判定しないこと)
- モデル層が view から分離され、headless にテストされていること
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` の構成でも通ること)、`git diff --check` クリーン

**範囲外**: ギズモ。回転をクォータニオンのまま出すか操作しやすい表現を足すかも、
**この WP では扱わない**(利用者の要望は記録済み。ギズモと併せて判断する)。

依存: WP264(マージ済み)。見積: 中。**D2 の残り半分**。

### WP267: フリーカメラが project の up 規約を無視している

**症状(利用者報告)**: 「なんか DamagedHelmet がこれまでと上下反転してる」。

**原因**: `cameracontrollersystem.cpp` の `worldUpFor()` が
`constexpr glm::vec3 world_up{0.0f, 1.0f, 0.0f}` を**ハードコード**している。
ところがプロジェクトは `basic_config.camera.up` を宣言でき、
`C:/pt/gltfcheck` は `[0.0, -1.0, 0.0]`(Y 下向き)である。
フリーカメラが自前の Y 上向きを使うため上下が反転する。

**この前提は `Fly` コントローラに元からあったが、WP265 がそれをエディタの
ビューポートへ露出させた**ので実質的に回帰である。WP265 の受け入れ条件に
「既存のシーンカメラの挙動が変わらないこと」は入れたが、
**「フリーカメラがプロジェクトの up 規約に従うこと」を入れていなかった。**

**実装範囲**:

1. `worldUpFor()` がプロジェクトの `basic_config.camera.up` を参照すること。
   値は `basicconfig.cpp` が既に読んでいる。**新しい読み取り経路を作らないこと。**
2. **`Fly` 以外のコントローラ(`Orbit` / `Follow`)も同じ前提を持っていないか確認すること。**
   持っていれば同時に直すこと。片方だけ直すと規約が二重になる。
3. up と視線が平行に近いときの退避(現在は `0.98` で Z 軸へ逃げている)を、
   宣言された up に対して正しく行うこと。

**受け入れ条件**:

- `up` が `[0,-1,0]` のプロジェクトで、フリーカメラの上下が反転しないこと。
  **画素で確認すること**(同じシーンをフリーカメラ有無で描いて上下の一致を見る)
- `up` が `[0,1,0]` のプロジェクトの挙動が変わらないこと
- `Orbit` / `Follow` も同じ規約に従うこと(または従っていることを確認したと書くこと)
- `ctest` 全数が緑、`git diff --check` クリーン

依存: なし。見積: 小。**回帰なので優先**。

### WP268: 視点移動のプリセット(Blender / Unity 風)

**目的**: 視点移動の操作方式を**複数のプリセットから選べる**ようにする。
利用者の要望: 「blender の視点移動仕様に寄せたいかも」「複数プリセットから選択できるといいね。unity/blender 的な」。

**既存の仕組みで表現できる**: 入力プロファイルは
`{"action": ..., "binding": ..., "invert_y": ...}` の宣言(`pelican.input_profile` v1)で、
`--input-profile` で切り替えられる。`--free-camera` は現在
`free_camera.json` 1 枚を埋め込みで束ねている。**プリセットはこの宣言を増やすだけで足りる。**

**実装範囲**:

1. **プリセットを複数持てるようにする。** `--free-camera` が 1 枚固定なのをやめ、
   名前で選べる形にすること。既定を決めて理由を書くこと。
2. **Blender 風と Unity 風を用意すること。** 実際の操作系を調べて宣言に落とすこと。
   推測で決めないこと。少なくとも次を明示すること:
   - 軌道(orbit)の中心をどう決めるか
   - パン / ズーム / 回転それぞれのボタンと修飾キー
   - マウスホイールの役割
3. **`Orbit` コントローラが要るなら使うこと。** Blender の既定は軌道であり、
   `Fly` では表現できない。既存の `SceneCameraControllerType::Orbit` が使えるか確認し、
   足りなければ何が足りないかを書くこと。**新しいコントローラ型を安易に足さないこと。**
4. プリセットの置き場を決めること。engine の埋め込みリソースか、project 空間か、両方か。
   **project 空間に置く場合、利用者のプロジェクトを書き換えない**(WP265 の規律)。
5. devstudio から選べるようにするかは**この WP では扱わない**(まず player の公開面で選べること)。

**受け入れ条件**:

- 少なくとも 2 つのプリセットが名前で選べること
- 各プリセットの操作系が文書に書かれていること(何を押すと何が起きるか)
- 既定のプリセットが決まっており、理由が書かれていること
- 利用者のプロジェクトが変更されないこと
- `ctest` 全数が緑、`git diff --check` クリーン

**範囲外**: devstudio の UI からの選択。エディタ設定として持つかは触ってから決める。

依存: **WP267**(up 規約が直っていないと、どのプリセットも上下が狂う)。見積: 中。

**(2026-08-06 改訂)前提が誤っていた。この WP は 3 本に分割する。**

WP268 の着手時調査で、**「宣言を増やすだけで足りる」という前提が崩れた**。実測した事実:

- **入力 v1 にホイールの binding が無い。** `actionmap.cpp` は `kbd:` / `pad:` の接頭辞で
  解析しており、ホイールを表す語彙が無い
- **修飾キーの chord 記法が無い。** `Shift+MMB` のような組み合わせを 1 つの binding として
  書けない
- **`Orbit` は `target` を必須とする。** `camera.cpp` の
  `requireControllerNonEmptyString(controller, "target", camera_name)` により
  **名前付きシーンオブジェクトが要る**。ランタイムだけの注視点を持てない。`distance` も必須
- **`Orbit` にパンが無い。** `look` と `move` を読むだけで、注視点を平行移動する経路が無い

公式の操作系(調査済み): Blender は MMB orbit / Shift+MMB pan / Ctrl+MMB とホイールで zoom。
Unity は Alt+LMB orbit / MMB pan / Alt+RMB とホイールで zoom。
**いまの語彙ではどちらも書けない。**

分割は次のとおり。**WP268 は WP273 に読み替える。**

### WP271: 入力 binding にホイールと修飾キーの組み合わせを足す

**目的**: `pelican.input_profile` がホイールと `修飾+ボタン` を表現できるようにする。
Blender / Unity 風の視点移動はこれ無しには宣言できない。

**実装範囲**:

1. ホイールを binding として表現できるようにする。軸として扱うか離散イベントとして扱うかを
   決めて理由を書くこと。
2. 修飾キーとボタンの組み合わせを 1 つの binding として書けるようにする。記法を決めること。
3. **形式の版を上げるかを判断すること。** `pelican.input_profile` は v1 である。
   語彙の追加が版の変更に当たるかを strict v1 の規律に照らして決め、理由を書くこと。
   **既存プロファイルが読めなくなってはいけない。**
4. 既存の `kbd:` / `pad:` の解析を壊さないこと。

**受け入れ条件**:

- ホイールと `修飾+ボタン` を宣言でき、テストで検証されていること
- 既存の入力プロファイル(`projects/` 配下と engine 埋め込み)が従来どおり読めること
- 版の扱いが決まっており理由が書かれていること
- `ctest` 全数が緑、`git diff --check` クリーン

依存: なし。見積: 中。

### WP272: `Orbit` にランタイム注視点・パン・ズームを足す

**目的**: `Orbit` を、名前付きシーンオブジェクト無しで使えるようにし、パンとズームを持たせる。

**現状**: `target` と `distance` が必須で、注視点はシーン内のオブジェクトに縛られる。
パンが無い。エディタのビューポートは**利用者のシーンに依存しない注視点**を要する
(WP265 の規律 — プロジェクトを書き換えない)。

**実装範囲**:

1. ランタイムだけの注視点を持てるようにすること。**既存の「名前付きオブジェクトを中心にする」
   用法を壊さないこと。**
2. パンを足すこと。注視点を視線に垂直な平面で平行移動する形。
3. ズームを足すこと。距離を変える形。
4. **新しいコントローラ型を足さないこと。** `Orbit` の拡張として収めること。
   収まらないなら理由を書くこと。
5. up は WP267 で入れた project の宣言に従うこと。

**受け入れ条件**:

- `target` 無しで `Orbit` が使えること
- パンとズームが効くこと
- **既存の `target` 付き `Orbit` の挙動が変わらないこと。画素で確認すること**
- `ctest` 全数が緑

依存: **WP267**(マージ済み)。見積: 中。

### WP273: 視点移動のプリセット(旧 WP268)

**目的**: 上の 2 本の上に、Blender 風と Unity 風のプリセットを宣言として載せる。

**操作系(調査済みの公式仕様。これに合わせること)**:

| | orbit | pan | zoom |
|---|---|---|---|
| Blender | MMB | Shift+MMB | Ctrl+MMB / ホイール |
| Unity | Alt+LMB | MMB | Alt+RMB / ホイール |

**実装範囲**:

1. プリセットを名前で選べるようにする。`--free-camera` が `Fly` 固定なのをやめること。
2. 上の表どおりの binding を宣言として書くこと。
3. 既定を決めて理由を書くこと(利用者の要望と Blender の既定が orbit であることを考慮)。
4. **利用者のプロジェクトを書き換えないこと**(WP265 の規律)。
5. 初期の注視点をどう決めるかを書くこと。選択物へのフォーカスは範囲外。

**受け入れ条件**:

- Blender 風と Unity 風が名前で選べ、上の表どおりに動くこと
- 各プリセットの操作系が文書に書かれていること
- 利用者のプロジェクトが変更されないこと
- `ctest` 全数が緑

依存: **WP271** と **WP272**。見積: 小。

### WP269: レンダーパスの可視化パネル(devstudio)

**目的**: いま何がどの順で描かれているのかを devstudio で見られるようにする。
レンダリング周りの不具合を追う土台。

**土台は両側に揃っている**(2026-08-05 調査):

- **公開面**: `get_frame_plan` rpc が `renderer.currentFramePlanJson()` を返す。
  devstudio から取れる。新しい rpc は要らない
- **手本**: ImGui 側に `planviewer`(756 行)と `compiledplanviewer`(515 行)が既にある。
  パス、リソース、バリア、attachment ops、lowered materials、compute tasks、
  `color_load_op` / `depth_store_op` などを見せている。
  **何を見せるべきかは前例で決まっている**

**規模に注意**: `--dump-frame-plan` の出力は実測で **6481 行**ある。
**JSON をそのまま貼ったら読めない。** 構造を持った表示にすること。

**実装範囲**:

1. `get_frame_plan` を呼んでパネルに表示する。**ImGui 側が見せている項目を出発点にすること。**
   ゼロから項目を考え直さないこと。
2. **6481 行を読める形にすること。** 木構造、折りたたみ、絞り込みなど手段は問わないが、
   「全部出して終わり」にしないこと。何を採ったか書くこと。
3. 更新の契機を決めること。手動更新か、フレームごとか、変化時か。
   **フレームごとに 6481 行を取り直すのは論外**である。理由を書くこと。
4. player が動いていないときの表示を決めること(WP264 と同じ規律 — 黙って空にしない)。
5. 既存の 5 パネルと同じ作法(`makeDock`)。レイアウトプリセット(WP249)を壊さないこと。

**受け入れ条件**:

- パスの一覧と順序が読めること
- 各パスの入出力ターゲットが分かること
- 6481 行相当の情報が畳まれた形で提示され、全部展開しなくても概要が掴めること
- player 未起動時の表示が定義されていること
- モデル層が view から分離され、headless にテストされていること
  (`get_frame_plan` の応答を食わせて構造を組み立てる部分)
- 既存のレイアウトプリセットを読んでも壊れないこと
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` の構成でも通ること)、`git diff --check` クリーン

**テストできることの限界**: GUI 表示そのものは自動検証しない。
**応答から表示用の構造を組み立てる部分は view から切り離せばテストできる。**

**範囲外**: 編集(パスの有効化・無効化など)。この WP は読み取り専用。
`compiledplanviewer` 相当の低レベル情報も範囲外とし、必要なら別 WP にする。

依存: なし。`src/devstudio/` に閉じる。見積: 中。

### WP270: ビューポートの横幅下限が別のウィジェットで復活した

**症状(利用者報告)**: 「エンジンのパネルサイズの変更だけどビューポートが小さく仕切れない問題は
のこってるね」。WP257 で直したはずの症状が残っている。

**原因**: WP257 は `footer` の水平サイズポリシーを `Ignored` にして下限を外した。
その後 **WP264 が `picking_notice_`(`QLabel`)を同じレイアウトへ直接追加**しており
(`layout->addWidget(picking_notice_);`)、**このラベルにはサイズポリシーが設定されていない**。
ラベルは文字列に応じた最小幅を持つため、**WP257 が外した制約が別のウィジェットで復活した**。

**なぜテストが捕まえなかったか**: WP257 のテストは
`REQUIRE(viewport.minimumSizeHint().width() == 1);` を確かめている。条件としては正しい。
ところが `picking_notice_` は `hide()` された状態で構築されるため、
**テスト時点では最小幅に寄与せず通ってしまう**。実際に表示された瞬間に効く。
**「隠れているウィジェットを見逃す」形の抜けである。**

**実装範囲**:

1. `picking_notice_` が横幅の下限を作らないようにすること。
2. **同じ問題が再発しない形にすること。** 個別に対処するのではなく、
   ビューポートのレイアウトに載るウィジェットが横幅の下限を作らない、という規律を
   構造で担保すること。手段は問わないが、**次に誰かがウィジェットを足したときに
   黙って壊れない形**にすること。
3. **テストを、隠れているウィジェットにも効く形にすること。** 現在の
   `minimumSizeHint()` を見る検査は、`hide()` されたウィジェットを見逃す。
   表示状態にしてから測る、あるいは載っている全ウィジェットのポリシーを検査するなど、
   **今回の欠陥を実際に捕まえられる**形にすること。

**受け入れ条件**:

- `picking_notice_` が表示されている状態でも、ビューポートを横に小さく畳めること
- **上記のテストが、修正前のコードでは落ちること**を確認して報告すること
  (落ちない検査を足しても意味がない)
- 将来ウィジェットを追加しても下限が復活しにくい構造になっていること。
  どう担保したかを書くこと
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` の構成でも通ること)、`git diff --check` クリーン

依存: なし。`src/devstudio/` に閉じる。見積: 小。

### WP274: ギズモの render feature(engine 側)

**目的**: 変換ギズモのハンドルを描き、**状態を持たない当たり判定**に答える feature を engine に置く。
D2 最後の一片であり、利用者の最初の要望「操作しやすい回転」の本命。

**方針の根拠**: [`design_devstudio_direction.md`](design_devstudio_direction.md) §2.1 を読むこと。
旧案(`debug_draw` + `update_transforms`)は**両端とも成立しない**ことが確定し、差し替えた。

**実装範囲**:

1. `engine://features/gizmo.json` を **パージ可能な render feature** として新設する。
   `picking.json` が手本(既定で無効、外せばターゲットもパスも消える)。
2. **選択とモード(移動 / 回転 / 拡縮)を rpc で受け取り**、対応するハンドルを描くこと。
   選択の同定は WP258 の `(scene_id, declaration_index)`。**第二の同一性スキームを作らないこと。**
3. **状態を持たない当たり判定クエリ**を rpc で公開すること。
   「選択 S・モード M のとき座標 (x,y) にあるハンドルはどれか」に答える形。
   **掴んでいる最中の状態を engine に持たせないこと。**
4. **掴み代を持たせること。** `pipelinefactory.cpp` の `rasterization.lineWidth = 1.0f` が
   リポジトリ唯一の指定で `wideLines` の要求も無い。線を 1 ピクセルで描くなら、
   判定は**線の太さと無関係な許容半径**で行うこと。値の決め方(DPI 追従など)を書くこと。
5. **エディタ以外からも使えること**(D0)。devstudio 専用の裏口を作らないこと。

**受け入れ条件**:

- feature を有効にすると、指定した選択とモードのハンドルが描かれること
- 当たり判定クエリが、ハンドル上の座標で正しいハンドルを返し、外れた座標で「なし」を返すこと
- **掴み代が 1 ピクセルより広いこと。**具体的な許容半径をテストで検証すること
- feature を外すとターゲットもパスも**存在しない**こと
- 既存 golden が動かないこと(既定で無効であるため)
- `gpu` ラベル全数と `ctest -LE gpu` が緑、`git diff --check` クリーン

依存: WP258 / WP262(いずれもマージ済み)。見積: 大。

### WP275: devstudio のギズモ操作

**目的**: ビューポートでハンドルを掴んでドラッグし、選択オブジェクトを動かす。

**実装範囲**:

1. 選択とモードを WP274 の rpc へ送ること。
2. 押下時に当たり判定クエリを 1 回行い、掴んだハンドルをエディタ側の状態として保持すること。
   **ドラッグ中に判定し直さないこと**(掴んだ後は既知の軸に沿った拘束運動)。
3. **ドラッグの適用は編集 rpc の preview lease を使うこと。**
   `open_preview` / `update_preview` / `commit_preview` / `abort_preview`。
   **`update_transforms` を使わないこと** — §2.1 のとおり studio からは flush されず、
   `load_scene` で黙って捨てられる。
4. **undo に載ること。** WP266 が `undo` / `redo` を配線済みである。
   ギズモの操作が取り消せないのは欠陥であって機能の不足ではない。
5. Inspector(WP266)と食い違わないこと。同じオーサリング値を編集するため、
   ギズモで動かしたら Inspector の表示も追従すること。
6. モードの切り替え手段を決めること(キー、ツールバー、その両方)。

**受け入れ条件**:

- ハンドルを掴んで動かすと、選択オブジェクトが動くこと
- **その操作が undo で取り消せること**
- **保存して開き直しても残ること**(オーサリング編集であることの確認)
- ギズモで動かした後、Inspector の表示が一致すること
- モデル層が view から分離され、headless にテストされていること
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` の構成でも通ること)、`git diff --check` クリーン

**範囲外**: 回転の数値入力表現(Inspector にオイラー角を出すか)。
ギズモで回せるようになってから、まだ必要かを判断する。

依存: **WP274**。見積: 中。

### WP276: ギズモが掴んだ矢印どおりに動くようにする(WP275 の欠陥修正)

**完了（2026-08-09）**。公開 drag 契約、3 欠陥の回帰、修正前 red gate、全受け入れ結果は
[`design_reviews/2026-08-09_wp276_projected_gizmo_drag_report.md`](design_reviews/2026-08-09_wp276_projected_gizmo_drag_report.md)
を正とする。

**目的**: WP275 でマージ済みのギズモは、**カメラを回すと掴んだ矢印と逆または直交する方向に動く**。
実用にならない。加えて確定した欠陥が 2 件ある。3 件まとめて直す。

**根拠**: WP275 マージ後の敵対的レビューで確定(反証 3 観点 × 各指摘、10 件は棄却され 3 件が残った)。

#### 欠陥 1(高): ドラッグ方向がカメラに依存していない

`src/devstudio/model/gizmomodel.cpp:91` の `pointerScalar(GizmoAxis, dx, dy)` は
**カメラを引数に取らない**。X は `dx`、Y は `-dy`、Z は固定の対角線という画面基底が焼き込まれている。
一方 engine は `src/core/renderer/gizmo.cpp:189` でワールド軸を `Camera::getVPMatrix()` で射影して
ハンドルを描き、当たり判定も同じ射影で行う。**描かれる矢印の画面上の向きはカメラで変わるのに、
studio の符号は変わらない。** カメラを yaw 180° 回して +X 矢印が画面左を向いた状態で
ポインタを右へ引くと、オブジェクトは +X すなわち画面左へ動く。掴んだ矢印とも、カーソルとも逆。

同じ根で `TranslationUnitsPerLogicalPixel` は**固定のワールド/ピクセル比**であり、
カメラからの距離に依らない。100 m 先の物と 1 m 先の物が 1px あたり同じワールド距離動く。
ハンドルはカーソルに追従しない。

**studio 側にカメラ計算を複製して直してはならない。** `gizmomodel.cpp:95-97` のコメントが
述べている動機(studio がカメラ/射影計算の複製を持たないこと)は正しい。誤りは
**engine が答えを返していない**ことにある。`query_gizmo_handle` の応答
(`src/core/communication/rpcserver.cpp:1435-1443`)は `handle`/`axis`/`grab_radius_pixels` しか
返さず、射影された向きを含まない。engine は `GizmoGeometry.segments` に必要な値をすでに持っている。

**実装範囲**:

1. `query_gizmo_handle` の `handle` オブジェクトに、掴んだハンドルについて次を追加すること。
   - **ドラッグしたときに値が増える向きの単位ベクトル**(2 次元、`coordinate` と同じ y 下向きピクセル空間)。
     モードごとに意味が違う。移動と拡縮は射影された軸方向、**回転は掴んだ点における輪の接線方向**。
     「どちらへ引けば増えるか」を知っているのは幾何を持つ engine であり、そこに置くこと。
   - **1 論理ピクセルあたりの値の変化量**。移動はワールド単位、回転はラジアン、拡縮は無次元。
2. studio は `scalar = dot(ピクセル差分, 向き) * ピクセルあたりの変化量` の**単一の式**にすること。
   3 モードで場合分けしないこと。`pointerScalar` の固定基底、Z の対角線、
   `TranslationUnitsPerLogicalPixel` / `RotationRadiansPerLogicalPixel` /
   `ScaleExponentPerLogicalPixel` を**削除**すること(既定値の所在は engine 一箇所になる)。
3. **退化を定義してテストすること。** カメラが軸の真正面を向くと射影された軸長が 0 に潰れ、
   向きが定まらない。engine 側で明示的に扱うこと(その場合ヒットを返さない、
   あるいは名前付きの表明を返す)。無言で NaN や巨大な跳躍を出さないこと。

**受け入れ条件**:

- **少なくとも 3 つのカメラ姿勢**で、そのうち 1 つは射影された +X が画面左を向く姿勢、
  1 つは射影された +Y が画面上を向く姿勢を含み、**描かれたハンドルの向きへポインタを引いたとき
  値が増える**ことを headless テストで検証すること。すなわち
  `sign(scalar) == sign(dot(ドラッグ, 描かれた向き))` が全姿勢で成り立つこと
- 同じ大きさのピクセル移動で、**カメラから遠いオブジェクトほど大きくワールド移動する**こと
  (射影に追従していることの確認)。少なくとも 2 つの距離で検証すること
- **`src/devstudio/` にカメラ・射影の計算が存在しないこと。**
  `lookAt` / `perspective` / `VPMatrix` / `glm::mat4` を grep して 0 件であることを示すこと
- 退化姿勢でドラッグを開始しても NaN・無限大・1 フレームでの巨大な跳躍が起きないこと

#### 欠陥 2(中): 古い問い合わせの失敗が「feature が無い」という嘘になる

`gizmomodel.cpp:283` の `cancelInteraction()` は `pending_hit` を捨てるが **`generation` を上げない**
(上げるのは 470 / 479 / 489 / 497 の 4 箇所のみ)。`pointerPressed`(:505)がこれを呼ぶため、
1 回目の問い合わせが飛んでいる最中に 2 回目の押下が来ると、1 回目の失敗が :607 の世代ゲートを
通過し、:609 の request_id 照合に落ちて :614 の `"Gizmo display failed: "` に流れる。
`set_gizmo` は成功しているのに表示の失敗として報告される。さらにその文字列が feature URI を
含むと `src/devstudio/view/mainwindow.cpp:561` が
「render graph に engine://features/gizmo.json が無い」に書き換える。**入っているのに無いと言う。**
成功側は :361 で照合外の応答を黙って捨てており、非対称になっている。

**受け入れ条件**:

- 押下 → 押下 → 1 回目の問い合わせが失敗、の順で、通知が
  「feature が無い」でも「表示の失敗」でもないことを headless テストで検証すること
- 失敗の経路が成功の経路と同じ判定を使っていること(非対称を残さないこと)

#### 欠陥 3(高): 保存テストが保存を検証していない

`test/devstudio_inspector_test.cpp:527` の
"Devstudio save scene persists committed authoring edits only while idle" は
`save_scene` が送られたことと busy 遷移しか見ていない。**開き直していない。**
WP275 の受け入れ条件「保存して開き直しても残ること」は立証されていない。

**受け入れ条件**:

- ギズモで編集 → コミット → 保存 → **シーンを読み直し**、
  読み直した**オーサリング値が編集後の値と一致する**ことを検証すること。
  rpc が送られたことの確認では条件を満たさない
- **この修正前のコードでは落ちること**を確認し、どう確認したかを書くこと

依存: なし(WP274 / WP275 はマージ済み)。engine と devstudio の両方に触れる。見積: 中。

### WP277: ギズモの選択識別子を実行時オブジェクトへ広げる

**目的**: ギズモが**実行時に生成されたオブジェクト**を指せるようにする。
契約が固まる前に済ませる。

**なぜ今か**: `GizmoSelection` は `src/core/renderer/gizmo.hpp:50` で
`{scene_id, declaration_index}` に固定されており、これは**宣言されたオブジェクトの identity** である。
実行時生成オブジェクトには declaration index が無いため、`requireGizmoTarget` が必ず弾く。
ゲーム内の配置モードや、実行中に生成した対象を動かす用途では、
**動かしたい当のオブジェクトがギズモの対象になれない**。

依存しているコミットは **WP274 / WP275 / WP276 の 3 つだけで、クライアントは devstudio 1 つだけ**である。
今なら形の差し替えで済む。三つ目のクライアントが出た後は契約のバージョン上げになる。
`GizmoSelection` はリポジトリ全体で 3 ファイル・11 箇所にしか現れない。

**範囲外(重要)**: **編集経路には触れないこと。** この WP は
「どれを指すか」と「当たり判定」だけを広げる。実行時オブジェクトをドラッグで動かす経路
(ゲーム側の編集、`LocalTransformComponent` と親付き変換の扱い)は別の WP に属する。
`GameContext` への公開 API 追加もこの WP ではやらない。

**実装範囲**:

1. `GizmoSelection` を、宣言オブジェクトと実行時オブジェクトのどちらも指せる形にすること。
   実行時側は `GameObjectId` で指す(`src/core/loader/scene.hpp:27` の
   `SceneRuntimeObjectBinding` が既に authoring と runtime を対応付けている)。
2. **二つの形を並存させたまま残さないこと。** 判別のある単一の形に決め、
   `set_gizmo` / `query_gizmo_handle` の応答・devstudio・
   `docs/manual/10_tools.md` の契約記述を**同じコミットで**移行すること。
   今それをやるためにこの WP がある。
3. **strict v1**: どちらの形でもないもの、両方の鍵が混在したものは
   **名前付きの硬いエラー**にすること。黙って一方を優先しないこと。
4. `resolveGizmoTargetTransform`(`gizmo.cpp`)の解決を分岐させること。
   実行時形は authoring 文書と `SceneLoader` を経由せず ECS へ直接向かう。
   宣言形の既存経路は変えないこと。
5. **実行時 id は実行をまたいで安定しない。** ギズモの表示要求は
   永続化されないこと(現状そうである)を確認し、その理由を書くこと。
   存在しない id は名前付きの硬いエラーにすること。

**受け入れ条件**:

- **実行時に生成したオブジェクト**(宣言に無いもの)をギズモの対象に設定でき、
  その位置にハンドルが描かれ、当たり判定が正しいハンドルを返すことを headless テストで検証すること
- 宣言オブジェクトの既存の挙動が変わらないこと(WP274 / WP276 のテストが無改変で通ること。
  契約の形を変えるため呼び出し側の記述は変わってよいが、**期待値は変えないこと**)
- 存在しない実行時 id、混在した形、どちらでもない形が、それぞれ**名前付きの硬いエラー**になること
- `GizmoSelection` の古い形がリポジトリに残っていないこと。
  `docs/manual/10_tools.md` の契約記述が新しい形と一致していること
- 編集経路(preview lease、`update_transforms`、`GameContext`)に**変更が無いこと**。
  差分を示して確認すること
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` でも通ること)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

依存: WP276(マージ済み)。見積: 小。

### WP278: SPIR-V / Vulkan ターゲットの所在を一箇所にする

**目的**: シェーダのターゲット環境が **4 箇所に 3 つの異なる値**で宣言されている。一箇所に寄せる。

**確認済みの現状**:

| 場所 | 値 |
|---|---|
| `src/core/resources/CMakeLists.txt:6` の `glslang -V`(`--target-env` 指定なし) | Vulkan 1.0 / SPIR-V **1.0** |
| `src/core/shader/shadercompiler.cpp:56` / `:451` | `vulkan-1.2` / SPIR-V **1.5** |
| `src/core/shader/spvlink.cpp:25` | `SPV_ENV_VULKAN_1_2` |
| `src/core/vkcore/core.cpp:24` | Vulkan **1.3.283** |

実測で裏が取れている。`src/core/resources/*.spv` のバージョン語は `0x00010000`(SPIR-V 1.0)であり、
実行時コンパイラの出力は 1.5 である。

**なぜ欠陥か**: エンジンシェーダは GLSL と SPIR-V の**両方**が埋め込まれ、
`ShaderLibrary::loadFromStemReference` は `PELICAN_RUNTIME_SHADER_COMPILER` が ON のときだけ
GLSL を優先する。つまり**同じシェーダがビルド構成によって異なる能力を持つ**。
SPIR-V 1.0 でしか表現できない機能しか使っていない今は表に出ないが、
1.4 以上を要求する拡張(ray query など)を入れた瞬間に、
ON のビルドでは通り OFF のビルドでは通らない、という形で噛む。
`CMakeLists.txt:41` が OFF 構成を支持し `:325-332` が SDK 無し環境向けに明記している以上、
これは仮定ではなく支持された構成である。

**実装範囲**:

1. ターゲットの宣言を**一箇所**に置くこと。他の 3 箇所はそこから導出すること。
   どこを正とするかを決め、理由を書くこと。
2. `embed_shader` に明示的な `--target-env` を渡すこと。
   ビルド時経路と実行時経路が**同じ SPIR-V バージョンを出す**ようにすること。
3. 値を上げること自体が目的ではない。**食い違いを無くすことが目的**である。
   ただし将来 1.4 以上が必要になることは分かっているので、
   上げる場合はどの構成に影響するかを述べること。

**受け入れ条件**:

- `src/core/resources/*.spv` のバージョン語と、実行時コンパイラの出力のバージョンが**一致**すること。
  両方を実際に読んで比較するテストまたは検証手順を示すこと
- ターゲットを変えたいとき、**編集する箇所が 1 つ**であること。どこかを示すこと
- `PELICAN_RUNTIME_SHADER_COMPILER` が ON の構成と OFF の構成の**両方**でビルドが通り、
  `ctest` 全数が緑であること。OFF 構成を実際に走らせること
- 既存 golden が動かないこと。動いた場合は 1 枚ずつ理由を述べること
- `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: なし。レイトレの前提だが、単独で価値がある。見積: 小〜中。

### WP279: ギズモの掴み代の既定値を一箇所に戻す

**目的**: `gizmoGrabRadiusLogicalPixels = 10.0f`(`src/core/renderer/gizmo.hpp:90`)が
`GrabRadiusLogicalPixels = 10.0`(`src/devstudio/model/gizmomodel.cpp:18`)として複製されている。
既定値の所在は一箇所の違反。

さらに悪いことに、devstudio は `query_gizmo_handle` が返す `grab_radius_pixels` を
**自分の複製で割って content scale を逆算している**(`gizmomodel.cpp:440-441`)。
クライアントが当たり判定の半径からスケール係数を逆算しなければならないのは、
**応答が過小仕様である**という徴候である。

**実装範囲**:

1. `query_gizmo_handle` の応答に content scale を明示的に含めること。
   クライアントが逆算しなくてよくすること。
2. devstudio 側の掴み代の複製を**削除**すること。
3. WP276 で追加した応答の形と整合させること。**第二の版を作らないこと。**

**受け入れ条件**:

- `src/devstudio/` に掴み代の数値定数が存在しないこと。grep して 0 件であることを示すこと
- devstudio が content scale を割り算で逆算していないこと
- DPI 追従の既存テスト(WP274 の「掴み代が 1 ピクセルより広い」)が期待値を変えずに通ること
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` でも通ること)、`git diff --check` クリーン

依存: **WP277**(同じファイル群に触れるため、WP277 のマージ後に着手すること)。見積: 小。

### WP280: レイトレの能力配線(feature はまだ作らない)

**目的**: ハードウェアレイトレの**デバイス能力だけ**を配線する。
acceleration structure も pass も shader も作らない。**この WP で絵は 1 ピクセルも変わらない。**

**位置づけ**: レイトレ対応の第一歩。以降の順は
(この WP) → BLAS/TLAS を静的ジオメトリ限定でリース管理下に → `rt_shadow_mask` feature。
最初の feature を ray query にするのは、`rayQueryEXT` が通常の fragment / compute シェーダの中で走り、
shader binding table も新しいパイプライン種別も新しいシェーダステージも要らないためである。

**前提**: WP278(マージ済み)でシェーダターゲットの所在が一箇所になった。
ray query は SPIR-V 1.4 以上を要求するため、これが先に必要だった。

**手本にすべき前例**: `multiview`。`src/core/vkcore/core.cpp:475-478` のコメントが意図を述べている
——「機能があれば日和見的に有効化する。target planning は選択の前に実装/シェーダの宣言を要求する」。
**ただし multiview をそのまま真似てはいけない箇所が 2 つある**。下記 3 と 6 を読むこと。

**実装範囲**:

1. `queryDeviceFeatureSupport`(`core.cpp:276`)の `StructureChain` に
   `PhysicalDeviceAccelerationStructureFeaturesKHR` と `PhysicalDeviceRayQueryFeaturesKHR` を加え、
   `vk12.bufferDeviceAddress` を読むこと。`DeviceFeatureSupport`(`core.cpp:262`)に
   **日和見的な**項目として追加すること。**required に入れないこと** —
   レイトレの無いデバイスでも従来どおり起動しなければならない。
2. `createLogicalDevice`(`core.cpp:464-529`)で、対応がある場合に限り
   `VK_KHR_acceleration_structure` / `VK_KHR_ray_query` / `VK_KHR_deferred_host_operations` を有効化し、
   対応する feature 構造体を chain に繋ぎ、`vk12features.bufferDeviceAddress` を立てること。
   **3 つは揃って初めて意味を持つ**。片方だけ有効になる状態を作らないこと。
3. **VMA に `eBufferDeviceAddress` フラグを立てること**(`createAllocator`、`core.cpp:555`)。
   **ただし `bufferDeviceAddress` を実際に有効化したときに限る。**
   全バッファが `VulkanManageCore::allocBuf` を通る以上、
   フラグ無しのアロケータで device address 付きバッファを確保するのは VMA の誤用である。
4. 拡張関数のエントリポイントを読み込むこと。動的ディスパッチャは存在せず、
   `core.hpp:62-67` のように PFN メンバを手で持つ規約である。必要なのは
   `vkCreateAccelerationStructureKHR` / `vkDestroyAccelerationStructureKHR` /
   `vkGetAccelerationStructureBuildSizesKHR` / `vkGetAccelerationStructureDeviceAddressKHR` /
   `vkCmdBuildAccelerationStructuresKHR`。
5. `VkPhysicalDeviceAccelerationStructurePropertiesKHR` を照会し
   `minAccelerationStructureScratchOffsetAlignment` を保持すること。
   `core.cpp:287` は素の `getProperties()` しか呼んでいない。
6. **能力の事実は「有効化した結果」から作ること。**
   `renderingsamplecount.cpp:2146-2157` は物理デバイスを**再照会**して multiview の
   *supported* を読んでいるが、これは `core.hpp:27-29` が禁じている書き方である
   (multiview では supported == enabled なので無害なだけ)。
   レイトレでは 3 つの拡張が揃わなければ有効化しないため **supported と enabled が食い違いうる**。
   `VulkanRuntimeCapabilities` を経由すること。**この誤りを複製しないこと。**
7. target planning に能力文字列として公開すること(`pelican.vulkan.ray_query@1` など。
   命名は `src/project/vulkanviewplanning.hpp:15-18` の既存規約に合わせること)。
   **プロジェクトが要求したのに実機に無い場合は名前付きの硬いエラー**にすること。
   前例は `pelican.plan.multiview_required_unavailable@1`
   (`src/project/vulkanviewplanning.cpp:186-192`)。
   **multiview の `automatic` のように黙って落とす経路を作らないこと** ——
   multiview は縮退しても同じ絵が出るから正当なのであって、レイトレは絵が変わる。

**範囲外**: acceleration structure そのもの、新しい pass 種別、新しい resource kind、
シェーダの変更、`rt_shadow_mask`。すべて後続 WP に属する。

**受け入れ条件**:

- **絵が変わらないこと。** 既存 golden が 1 枚も動かないこと。動いた場合はこの WP の失敗である
- `ctest` 全数が緑、GPU ラベル全数を含むこと
- 開発機(RTX 5080)で 3 拡張と `bufferDeviceAddress` が**有効化された**ことがログまたはテストで確認できること
- **レイトレの無いデバイスでも起動できること。**実機が用意できないため、
  能力判定の入力を合成した単体テストで、
  (a) 拡張が 1 つも要求されないこと、(b) VMA の BDA フラグが立たないこと、
  (c) 起動が失敗しないこと、を検証すること。**実機が無いことを条件を省く理由にしないこと**
- **VMA の BDA フラグが `bufferDeviceAddress` の有効化と厳密に一致**すること。
  片方だけ立つ入力を与えて検証すること
- 能力の事実が `VulkanRuntimeCapabilities` 由来であること。
  物理デバイスの再照会を新たに書いていないことを grep で示すこと
- プロジェクトが ray query を要求して実機に無い場合、名前付きの硬いエラーになること
- `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: WP278(マージ済み)。見積: 中。

### WP281: 加速構造を静的ジオメトリ限定で置く

**目的**: BLAS / TLAS を GPU 資源として engine に置く。**まだ誰も参照しない。**
レイトレの二歩目で、`rt_shadow_mask`(三歩目)の土台。

**前提**: WP280(マージ済み)が 3 拡張と `bufferDeviceAddress` を有効化し、
5 本の PFN と `minAccelerationStructureScratchOffsetAlignment` を取得し、
能力を `pelican.vulkan.ray_query@1` として公開し、
不在時の硬いエラー `pelican.plan.ray_query_required_unavailable@1` を用意した。

#### 静的ジオメトリ限定であることを、見える形で宣言すること

**この engine には変形後のジオメトリがメモリ上に存在しない。**
スキニングは `skinned.vert` が `pelican_skin_matrix()` を頂点シェーダ内で適用し、
morph は `pelican_morph_vertex()`、VAT は `vat.vert` が位置を `texelFetch` して補間する。
compute skinning も transform feedback も変形後頂点のアリーナも無い。
`VertBufContainer` が持つのは**元の姿勢の頂点だけ**である。

BLAS はバッファに実在する頂点からしか作れない。したがって**この WP の時点では、
スキン付き・morph・VAT のメッシュはレイトレから見えない**。
そのまま入れると全キャラクタがバインドポーズの影を落とすことになる。

**要求**: 対象外であることを**黙って落とさず、宣言として扱うこと。**

- `ModelPrimitiveRefInfo::skinned`(`src/core/model/modeltemplate.hpp:75`)が既に判別子として在る。
  これを使い、対象外のプリミティブを**数え、名前を出して記録すること**。
  「静かに 0 件」と「静かに除外した 40 件」が区別できない状態にしないこと
- 対象外が存在すること自体は**エラーにしないこと**(まだ誰も参照しないため)。
  ただしログまたは診断で件数が見えること
- 変形後ジオメトリを扱う compute deform pass は**この WP の範囲外**であり、
  後続 WP に属することを文書に書くこと

#### 実装範囲

1. 三つのジオメトリプール(`vertbufcontainer.cpp:17-38` の
   `createIndexBuf` / `createVertBuf` / `createSkinVertBuf`)の usage に
   `eShaderDeviceAddress | eAccelerationStructureBuildInputReadOnlyKHR | eStorageBuffer` を足すこと。
   **device address が要るのは BLAS の構築入力としてであり、シェーダからの読み出しには不要**である
   (全ジオメトリは 3 本の大域プールとオフセットで届く)。
2. 静的プリミティブごとの BLAS と、フレームごとの TLAS を作ること。
   TLAS の再構築は `renderer.cpp:4553` の
   「Object, skin, morph, and material-override GPU state is frozen after target acquisition
   and before the first view records」の地点に入れること。
3. **寿命は既存のリース式 `DeletionQueue` にそのまま乗せること。**
   `leaseForNextSubmission` / `confirmSubmission` は既に正しい形であり、
   `VertBufContainer` が退役プールに対して `deferOldBuffer`(`vertbufcontainer.cpp:120`)で
   やっていることと同じである。**フレーム数による寿命管理を新設しないこと。**
4. `RenderPipelineGpuResourceKind`(`renderpipelinegpuarena.hpp:25`)は閉じた enum である。
   加速構造の居場所を決め、既存のパージ可能な scope に乗せること。
5. **構築は宣言が要求したときだけ行うこと。** 常時構築にすると、
   レイトレを使わないプロジェクトも BLAS の構築費用を払う。パージ可能の原則に反する。
   WP280 の能力文字列と要求の仕組みに接続すること。

#### 無効化

**無効化の契機は transform 編集ではない。** transform は
`polygoninstancecontainer.cpp:782-786` が毎フレーム全行列を無条件に再アップロードしており、
dirty 追跡は存在しない。TLAS の毎フレーム再構築はその地点にそのまま入る。

**本当の危険は BLAS で、契機はジオメトリプールの再確保である。**
`ensureIndexCapacity`(`vertbufcontainer.cpp:304`)と
`ensureVertexCapacity`(`:323`)はプールを倍化して内容を複写し、
古いバッファを `deferOldBuffer` に渡す。BLAS は構築時にジオメトリを**デバイスアドレスで捕まえる**ため、
**容量を跨ぐモデル読み込み 1 回で、シーン中の全 BLAS が一斉に無効になる。**
現状これを知らせるものは `:320` のログ 1 行しかない。

**要求**: プール再確保と `rebuildModelInstances` の両方に無効化を接続すること。
黙って古いアドレスを掴んだままにしないこと。

#### 受け入れ条件

- **絵が変わらないこと。**既存 golden が 1 枚も動かないこと
- 能力を要求しないプロジェクトで、**加速構造が構築されないこと**。
  構築回数が 0 であることをテストで示すこと(「作っていないつもり」では条件を満たさない)
- 静的プリミティブから BLAS が作られ、TLAS のインスタンス数が静的インスタンス数と一致すること
- **プールの再確保を実際に起こし**(容量を跨ぐモデル読み込み)、
  その後に古いデバイスアドレスを参照した加速構造が残っていないことを検証すること。
  容量を跨がせる方法をテストに書くこと
- スキン付き / morph / VAT のプリミティブが**除外され、その件数が観測できる**こと
- `ctest` 全数が緑(GPU ラベル全数を含む)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

依存: WP280(マージ済み)。見積: 大。

### WP282: 加速構造の対象外を、落とさずに数える

**目的**: WP281 が入れた加速構造の分類に残った 2 つの穴を塞ぐ。

**根拠**: WP281 マージ後の敵対的レビューで確定。

#### 欠陥 1: 不正ジオメトリがフレームごと落とす

`src/core/renderer/accelerationstructure.cpp:417-427` は静的プリミティブの
`index_count % 3 != 0` を `std::runtime_error` にする。ところが公開 API の
`VertBufContainer::addPrimitiveEntry` は索引列が無いとき
`indices = iota(vertex_count)` を合成する(`vertbufcontainer.cpp:391-394`)ため、
**4 頂点の非索引プリミティブは合法な入力でありながら `index_count = 4` になる**。
`validateVertexStreams`(`:141-174`)は長さと範囲しか見ておらず 3 の倍数性を検査しない。
ラスタライザは末尾の不完全な三角形を捨てて普通に描く。

つまり **ray query を要求した瞬間にだけ、それまで動いていたプロジェクトが毎フレーム死ぬ。**
`Renderer::render` が捕まえるのは `OutputTemporarilyUnavailable` と
`OutputRelowerRequired` だけ(`renderer.cpp:4838-4871`)なので、例外は外まで抜ける。

**これは WP281 自身の方針と非対称である。**変形ジオメトリは*除外して数える*のに、
不正ジオメトリは*フレームごと落とす*。同じ扱いにすること。

**実装範囲**: 3 の倍数でない索引数、頂点数 0、索引数 0、負の頂点オフセットを、
変形ジオメトリと**同じ診断経路の除外区分**として扱うこと。区分は変形とは別の名前にすること
(「BLAS を作れない」理由が違うため)。

#### 欠陥 2: 除外フラグを実ジオメトリから導出するテストが無い

`test/accelerationstructurepolicy_test.cpp` の除外テストは
`morph_deformed` / `vat_deformed` を**手書きの入力として与えている**。
分類器は検証されているが、**フラグを立てる側は検証されていない**。

この穴は既に一度刺さっている。WP281 の `gltf.cpp:2099` は
`vat_deformed` を `#if PELICAN_WITH_VAT` の外で代入しており、
`PELICAN_WITH_VAT=OFF` の構成をコンパイル不能にしていた
(別途修正済み)。手書き入力のテストでは検出できない場所である。

**実装範囲**: **実際の glTF を読み込んで**、
skinned / morph / VAT のプリミティブに対応するフラグが立ち、
静的プリミティブでは立たないことを検証すること。
既存のテスト資産で足りない形式があれば、足りないことを述べること。

#### 受け入れ条件

- 索引数が 3 の倍数でない静的プリミティブを含むシーンで、ray query を要求しても
  **フレームが落ちず**、そのプリミティブが除外区分として**数えられる**こと
- その件数が変形による除外と**区別できる**こと
- 実 glTF から `skinned` / `morph_deformed` / `vat_deformed` が導出されることを、
  手書きフラグではなく読み込み経由で検証すること
- **ビルド階層の全構成でビルドが通ること。**
  `PELICAN_WITH_VAT` / `PELICAN_RUNTIME_SHADER_COMPILER` の OFF 構成を**実際に構成してビルドすること**。
  ON だけで済ませないこと
- 既存 golden が 1 枚も動かないこと
- `ctest` 全数が緑、`git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: WP281(マージ済み)。見積: 小〜中。

### WP283: rt_shadow_mask — レイトレで最初に絵が出る feature

**目的**: ray query による遮蔽マスクを描く feature を置く。**レイトレで初めて絵が変わる段。**

**位置づけ**: WP278(ターゲット一元化)/ WP280(能力配線)/ WP281・WP282(加速構造と分類)
の上に載る四歩目にして、最初の可視な成果。

#### なぜ影マスクで、なぜ誰も消費しないのか

**SSAO の置き換えではない。** SSAO は feature 化されておらず、engine 内蔵の
`render_pipelines/hybrid_v1.json` の中身である。置き換えるには既定パイプラインを編集することになり、
WP274 のギズモが示したパージ可能な feature の作法と正反対になる。
加えて AO は品質項であり、TLAS が古くても間違っていても**わずかに暗いだけで誰も気づかない**。
fail-fast のプロジェクトにとって、最初のレイトレ機能が持つ性質として最悪である。
**大きな声で失敗しないものは、継ぎ目を通したことにならない。**

**照明にも繋がない。** この WP は**誰も消費しない自前の `R8_UNORM` ターゲット**に書く。
繋ぐのは次の WP であり、そのときには TLAS ではなく普通のサンプルドターゲットになっているので、
`shadow_directional.json` が使っている `surface_resources` 契約がそのまま使える
(あの機構はレンダーターゲットを contract に対応付けるものであり、TLAS を運べない)。
この分解により、加速構造・デバイス能力・ディスクリプタ配線・フレームグラフの資源種別を
**単独で、目に見える形で**検証できる。

#### TLAS の置き場所

**set 0(frame set)に置くこと。** TLAS はシーンデータであってパス入力ではない。
`pipelinefactory.cpp:104-122` の `frameDescriptorSetLayoutBindings()` は
6 個の固定表で、set 0 だけはリフレクション由来ではない(`:320` で無条件に注入される)。
`validateFrameBindings`(`:124-153`)が許可リスト外を例外にする。
必要な編集は `pelican_sets.hpp` / `shaders/include/pelican_sets.glsl` /
`pipelinefactory.cpp` の表と検証 / `frameresources.cpp` のプールと書き込み。

**set 1 に置いてはいけない。** `fullscreenpasscontainer.cpp` の入力契約は
ポート種別が `{automatic, image, buffer}` に閉じ、バインディングが位置決めで、
ディスクリプタプールが 3 種類しか持たず、書き込み経路に AS の分岐が無い。
set 0 に置けば **fullscreen パス機構に一切触らずに済む**。これが ray query を選んだ意味である。

**ただし無条件に足さないこと。** ray query を要求しないプロジェクトの set 0 レイアウトと
ディスクリプタプールが**今日と同一**であること。パージ可能の原則であり、
拡張が有効でないデバイスで AS ディスクリプタ型のプールを作れない問題も同時に避けられる。

#### 実装範囲

1. `engine://features/rt_shadow_mask.json` を**既定で無効・パージ可能**な feature として新設すること。
   手本は `src/core/resources/features/shadow_directional.json`(構造的な兄弟)。
   自前の `R8_UNORM` ターゲットを持ち、**誰も消費しない**。
2. パスは**通常の `fullscreen` パス**であること。**新しいパス種別を作らないこと。**
   `makePassInfo` にも `FramePlanNodeKind` にも手を入れない。
   シェーダが `rayQueryEXT` を使うだけである。SBT も RT パイプラインも要らない。
3. シェーダは既存の埋め込み経路に乗せること。WP278 でターゲットが SPIR-V 1.5 に揃ったので
   `GL_EXT_ray_query` が通る。**`PELICAN_RUNTIME_SHADER_COMPILER` が OFF の階層は
   埋め込み成果物を消費する**ので、両経路で通ることを確認すること(§4 規約 9)。
4. feature が有効なのに実機に ray query が無い場合は、WP280 の
   `pelican.plan.ray_query_required_unavailable@1` で落ちること。新しい経路を作らないこと。

#### 静的限定の帰結を隠さないこと

WP281 / WP282 により、スキン付き・morph・VAT のジオメトリは TLAS に**存在しない**。
したがって**それらは影を落とさない**。これは既知の制約であって不具合ではないが、
**絵の上で見えるものを golden が黙って正当化しないこと。**

キャラクタが影を落とさないことを、**そうと分かる形で**記録すること。
「静的ジオメトリ限定」という但し書きが実際に何を意味するかは、
文章より 1 枚の絵のほうが正確に伝わる。

#### 受け入れ条件

- feature を有効にすると、**静的な遮蔽物が実際にマスクに影を落とす**こと。
  「パスが走った」では条件を満たさない。画素で検証すること
- **同じ光源と同じシーンでラスタの影と比較し、その結果を報告すること。**
  一致を要求はしない(手法が違う)。**差がどこに出るかを述べること**
- feature を外すと**ターゲットもパスも存在しない**こと
- **ray query を要求しないプロジェクトの set 0 レイアウトとディスクリプタプールが
  今日と同一**であること。既存 golden が 1 枚も動かないこと
- スキン付き / morph / VAT のジオメトリが影を落とさないことが、
  golden または画素検証で**見える**こと
- `PELICAN_RUNTIME_SHADER_COMPILER` の ON / OFF 両階層でビルドが通ること(§4 規約 9)
- `ctest` 全数が緑(GPU ラベル全数を含む)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

依存: WP282(マージ済み)。見積: 大。

### WP284: rt_shadow_mask の欠陥修正

**根拠**: WP283 マージ後の敵対的レビューで確定。以下 6 件のうち ① ② ⑤ は直接確認済み。

#### ① [高] 背景の判定が成立しない — 空がレイを飛ばす

`src/core/resources/rt_shadow_mask.frag:22` は `world.a < 0.5` で被覆を判定するが、
**この分岐は絶対に成立しない**。材質シェーダは `outWorldPos = vec4(..., 1.0)` を書き
(`default.frag:65`、`shaders/material/surface_v1.frag:120`)、
gbuffer パスは `clear_color` を宣言しないので `renderingpass.hpp:427` の既定
`{0.0, 0.0, 0.0, 1.0}` を継ぐ。**alpha は常に 1.0** である。

結果、ジオメトリの無い画素が `world.xyz == (0,0,0)` からレイを飛ばす。
**原点付近に静的ジオメトリがあるシーンでは空が真っ黒になる**
(原点中心の地面、原点に立つキャラクタ、原点を含む部屋 — いずれもありふれている)。
現在のテストが通っているのは、固定ジオメトリがたまたま原点からの射線を外しているだけである。

**要求**: 被覆を表す信号を**実際に存在させる**こと。
`default.frag:29` は alpha を `A: reserved` と注記しており読む者はいないので、
gbuffer の clear の alpha を 0 にする案が最小だが、決めて理由を書くこと。
深度から導く案でもよい。**現状のまま「たまたま当たらない」に依存しないこと。**

#### ② [高] 静的ジオメトリが 0 のプロジェクトが毎フレーム落ちる

`src/core/renderer/frameresources.cpp:435` は
`"ray-query frame descriptor requires a TLAS"` を投げる。
一方 TLAS の構築は `accelerationstructure.cpp:551` の
`if (!classification.static_instance_indices.empty())` で丸ごと飛ばされる。
したがって **feature を有効にした静的ジオメトリの無いシーンは毎フレーム落ちる。**

**WP282 でちょうど取り除いた中断の形が、別の場所で再発している。**
適格なジオメトリが無いことは**数える状態**であってエラーではない、が WP282 で定めた方針である。

**要求**: 遮蔽が無い(マスク全面 1.0)として成立させること。診断で件数が見えること。

#### ③ [中] レイのバイアスが法線方向でなく、しかも量子化より小さい

`rt_shadow_mask.frag:17` の `RAY_ORIGIN_BIAS = 0.01` は
`origin = world.xyz + toLight * BIAS` として**射線方向**に適用されており、
これは tmin を 0.01 にするのと等価で、面から離れる距離は `0.01 * dot(toLight, n)`、
斜入射で 0 に潰れる。

加えて原点となる `gbuffer_worldpos` は `R16G16B16A16_SFLOAT`
(`render_pipelines/hybrid_v1.json:45`)であり、
座標の絶対値が 32 を超えると ULP が 0.03125、往復誤差が最大 0.0156 と
**バイアスより粗くなる**。受け面自身も TLAS に入っているため、自分の三角形に再ヒットしうる。

**要求**: 原点から離れたシーン(数十〜数百単位)で自己遮蔽が出ないこと。
法線方向のオフセット、距離に応じたスケール、あるいは両方。決めて理由を書くこと。

#### ④ [中] OFF 階層の唯一の主張がクリア値と同じ

`test/headless_render_test.cpp:8412` の画素テストは本体全体が
`#if PELICAN_RUNTIME_SHADER_COMPILER` に包まれており、OFF では空の TEST_CASE になる。
唯一残る `"rt shadow mask renders from the embedded shader path"` の主張は
`std::all_of(mask.pixels, value > 192)` — すなわち**全面白**であり、
これは `features/rt_shadow_mask.json` の `clear_color: [1,1,1,1]` が
R8_UNORM に書く 255 と**ビット単位で同じ**である。
さらに固定シーンはレイが必ず外れる配置になっている。

**つまり OFF 階層では、シェーダが 1 本もレイを飛ばさなくても緑になる。**

**要求**: 埋め込み経路でも**影が実際に出る**ことを主張すること。

#### ⑤ [中] 描画ごとに全パイプラインを線形走査する

`src/core/shader/pipelinefactory.cpp:691` の `pipelineLayoutUsesRayQueryFrameSet` は
登録済み全パイプラインの `std::any_of` であり、
これを `frameresources.cpp:585` の `bindGraphics` が**描画呼び出しごと**に呼ぶ。
**レイトレを使わないプロジェクトも払う。**

**要求**: 定数時間にすること。

#### ⑥ [中] TLAS バインディングの反射に能力要求が伴わない

シェーダが TLAS バインディングを反射しただけで
加速構造のディスクリプタレイアウトが作られ、
`pelican.vulkan.ray_query@1` の要求が伴わない経路がある。

**要求**: 反射が能力要求を含意すること。二つの真実の源を作らないこと。

#### 受け入れ条件

- 原点に静的ジオメトリを置いたシーンで、**背景の画素が遮蔽されない**こと
- 静的ジオメトリの無いシーンで feature を有効にしても**落ちず**、マスクが全面 1.0 で、
  除外件数が診断に出ること
- 原点から 100 単位以上離した同じシーンで、**自己遮蔽の斑が出ない**こと
- **影の領域と影でない領域の両方**を固定すること。
  「暗い画素が N 個以上」という下限だけの主張を残さないこと(WP283 の条件の穴)
- OFF 階層で、埋め込みシェーダが**実際に影を出す**ことを主張すること
- `bindGraphics` がパイプライン数に比例した走査をしないこと
- `ctest` 全数が緑(GPU ラベル全数を含む)、両ビルド階層でビルドが通ること(§4 規約 9)

依存: WP283(マージ済み)。見積: 中。

### WP285: レイトレーシングパイプラインと SBT

**目的**: `VK_KHR_ray_tracing_pipeline` によるディスパッチ経路を engine に置く。
ray query と**同じ影マスクをもう一度**、今度は raygen / miss / closest-hit と
shader binding table で実装する。

**なぜ二度実装するのか**: これが受け入れ条件の本体である。
同じシーン・同じ光源で **2 つの独立実装が同じ絵を出せば、どちらも正しい**と言える。
片方だけでは言えない。実際 WP283 は、クリア値と区別できない主張で緑になっていた。

**なぜマテリアル表を待たないのか**: 遮蔽判定は**ヒット地点で何も読まない**。
miss なら 1.0、hit なら 0.0 を書くだけである。
GPU 常駐のジオメトリ→マテリアル対応表と descriptor indexing が要るのは
反射・GI・アルファテスト形状であって、可視性クエリではない。

#### 設計の要:fullscreen パスの真似をしないこと

**`vkCmdTraceRaysKHR` は dynamic rendering のスコープ内に記録できない。**
したがって WP283 が使った fullscreen パスの形は取れない。

**手本は `compute_tasks` である。** `FramePlanNodeKind`
(`src/core/renderingpass/frameplanner.hpp:14-20`)は
`{render, compute, anchor, snapshot_copy, output_transform}` の閉じた enum で、
`compute` は既に「レンダーパスの外で走る独立した最上位ノード」として存在し、
`featurecompose.cpp:460-471` が `compute_tasks` を最上位キーとして合成している。
**同じ制約を持つ既存の前例がある。**そこに寄せること。

出力も違う。raygen は色アタッチメントではなく**ストレージ画像へ `imageStore`** する。
ターゲットの usage に STORAGE が要る。

#### 実装範囲

1. `VK_KHR_ray_tracing_pipeline` を、WP280 の
   `src/core/vkcore/devicefeaturepolicy.cpp` の方針関数に**同じ原子性で**追加すること。
   `VkPhysicalDeviceRayTracingPipelinePropertiesKHR` から
   `shaderGroupHandleSize` / `shaderGroupBaseAlignment` / `shaderGroupHandleAlignment` を取得すること。
   能力文字列 `pelican.vulkan.ray_tracing_pipeline@1` と名前付きの硬いエラーを、
   WP280 の `pelican.plan.ray_query_required_unavailable@1` と同じ形で用意すること。
2. `PipelineFactory` に RT パイプラインの生成経路を足すこと
   (今は `create` と `createCompute` しかない)。
   PFN は `vkCreateRayTracingPipelinesKHR` /
   `vkGetRayTracingShaderGroupHandlesKHR` / `vkCmdTraceRaysKHR`。
   動的ディスパッチャは無く、`core.hpp:62-67` の手動 PFN が規約である。
3. **SBT の詰め方を純粋関数として切り出すこと。**
   3 つのアライメント値とグループ数を入力に、各領域の
   stride / size / オフセットを返す形にすること。
   WP280 の `selectVmaAllocatorCreateFlags` が同じ作法で、
   デバイス無しの単体テストで検証できたのが効いた。**同じ形にすること。**
4. `ShaderStage`(`src/core/shader/shaderreference.hpp:8`)に
   `raygen` / `miss` / `closesthit` を足すこと。
   シェーダコンパイラは既にこの 3 つを写せる(`shadercompiler.cpp:67-72`)。
   **`any_hit` / `intersection` / `callable` は範囲外**とし、
   宣言されたら名前付きの硬いエラーにすること。黙って無視しないこと。
   (これらはアルファテスト形状に要るもので、マテリアル表が前提になる。)
5. `engine://features/rt_shadow_mask_pipeline.json` を
   **既定で無効・パージ可能**な feature として置くこと。自前の R8_UNORM ターゲットを持つ。

#### 範囲外

反射・GI・アルファテスト形状。ジオメトリ→マテリアル対応表。descriptor indexing。
`any_hit` / `intersection` / `callable`。既存 ray query 経路の置き換え
(**両方が残ること**が受け入れ条件である)。

#### 受け入れ条件

- **同じシーン・同じ光源で、ray query 版のマスクと RT パイプライン版のマスクが一致すること。**
  レイの起点バイアスに起因する差が残る場合は、許容幅とその根拠を述べること。
  「どちらも影がある」では条件を満たさない。**画素単位で比較すること**
- WP284 が固定した性質を両方の実装が満たすこと。すなわち
  **背景が遮蔽されない**、**静的ジオメトリ 0 で落ちない**、
  **原点から 100 単位以上離しても自己遮蔽しない**、
  **影の領域と影でない領域の両方が固定されている**
- **SBT のアライメント計算が、デバイス無しの単体テストで検証されていること。**
  `shaderGroupHandleSize != shaderGroupHandleAlignment` の場合と、
  `shaderGroupBaseAlignment > shaderGroupHandleAlignment` の場合を含めること
  (実機で最も踏まれる形である)
- feature を外すとターゲットもパスも存在しないこと
- RT パイプラインを要求しないプロジェクトの挙動が変わらないこと。既存 golden が動かないこと
- `any_hit` / `intersection` / `callable` を宣言すると名前付きの硬いエラーになること
- `ctest` 全数が緑(GPU ラベル全数を含む)、両ビルド階層でビルドが通ること(§4 規約 9)

依存: WP284(マージ済み)。見積: 大。

### WP286: モーダル変形操作(G / R / S と軸指定)

**目的**: Blender 式の変形操作を devstudio に足す。
`G` / `R` / `S` で移動・回転・拡縮に入り、続く `X` / `Y` / `Z` で軸を拘束する。

#### 今のギズモとは操作モデルが違う

**現行は「掴む」**:押下座標から `query_gizmo_handle` で軸を決める。
**モーダルは「掴まない」**:軸はキーで決まり、ポインタはどこにあってもよい。
`G` を押した瞬間に変形が始まり、確定するまでマウスに追従する。

したがって engine 側に足りないものが 1 つある。
**当たり判定を伴わずに、指定した軸のドラッグ基底を得る手段**である。
WP276 が `query_gizmo_handle` の応答に
「引くと値が増える向きの単位ベクトル」と「1 論理ピクセルあたりの変化量」を入れた。
モーダルでも**まったく同じ値**が要るが、入力が座標ではなく軸になる。

**基底の計算を二重に持たないこと。** 既存の当たり判定経路と同じ計算から出すこと。
studio 側にカメラ計算を複製しないこと(WP276 で消したものを戻さないこと)。

#### キー割り当てを直書きしないこと

視点移動プリセットが手本である。
`src/core/resources/input/free_camera_actions.json` が名前付き動作を宣言し、
`input/profiles/free_camera_blender.json` と `free_camera_unity.json` が
`{"action": ..., "binding": "mouse:middle"}` の形で束ねている。
**同じ作法に乗せること。**状態機械に Qt のキーコードを直書きしないこと。

プリセットは最低 2 つ(`blender` と、既存の掴む操作だけの既定)を用意し、
**切り替えられること**。既定値の所在は一箇所。

#### 状態機械は engine に置くこと(studio ではない)

**キーは Qt に届かない。**`src/devstudio/viewport/nativewindowhost.cpp:198` が
`AttachThreadInput` + `SetFocus(child)` で、ビューポート上でボタンが押された時点で
**キーボード焦点をプレイヤーの子ウィンドウへ渡している**
(`embeddedviewport.cpp:605-617` の `focusEmbeddedWindow` と `pollPointerFocus`)。
`G` を押したい瞬間、キーはエンジン側にある。
studio 側に状態機械を置くと、焦点設計と戦うことになる。

したがって分担はこうなる。

| | 持ち主 |
|---|---|
| キー割り当て(`kbd:g` → `gizmo.translate`) | **engine**。既存の入力プロファイルにエディタ用アクションセットを足す |
| モーダル状態(モード / 軸 / 累積量) | **engine**。`cameracontrollersystem.cpp` と同じ型のシステム |
| 状態と割り当ての公開 | **rpc** |
| オーサリング確定(preview lease) | **studio**。ここだけがエディタ専用(WP277) |

この分担の帰結:

- 消費モデルがそのまま効く。「移動モード中は `X` が軸指定、その下で中ボタン軌道回転は生きたまま」が
  コードを書かずに成立する
- エディタ操作が `--record-input` に乗り、決定的に再現できる
- ツールバーの「Move (G)」の `G` を studio 側の文字列定数にしないで済む
- **D0 が主張ではなく構造になる。**ゲームが配置モードを作るとき同じ仕組みを使える
- **入力宣言層を `src/project/` へ移す作業(大)が前提ではなくなる。**
  studio が割り当てを*表示*するだけなら rpc で足りる。
  移設が要るのはエディタで割り当てを*編集する* UI を作るときであり、それは後で決めてよい

#### 実装範囲

1. engine に、**軸を指定してドラッグ基底を得る問い合わせ**を足すこと。
   既存の当たり判定と同じ幾何計算から出すこと。
   退化(カメラが軸の真正面)の扱いは WP276 と揃えること。
2. `G` / `R` / `S` でモードに入り、`X` / `Y` / `Z` で軸を拘束する状態機械を **engine** に置くこと。
   `src/core/userpublic/cameracontrollersystem.cpp` が形の手本である。
   **軸を指定しない `G` は視点平面上の移動**とすること
   (Blender で最も使う操作であり、これが無いと「Blender 風」にならない)。
   視点平面の基底も engine から得ること。
3. 確定と取り消しを決めること。Blender は左クリック / Enter で確定、
   右クリック / Esc で取り消し。**取り消しは preview lease の abort に落ちること。**
4. モード中に軸キーを押し直したとき、別のモードキーを押したとき、
   選択が変わったとき、プレイヤーが停止したときの挙動を決めること。
   WP275 / WP276 が既に abort 経路を持っている。**第二の abort を作らないこと。**
5. **Inspector の数値欄を編集中にモードへ入らないこと。**
   ビューポート上でボタンが押されたときにのみ焦点が engine 側へ渡る設計なので
   (`embeddedviewport.cpp:611` の `pollPointerFocus`)、
   通常はキーが engine に届かない。**それに暗黙に依存しないこと** ——
   焦点が engine 側にあるまま Inspector を編集できる経路が無いことを確認し、
   確認方法を書くこと。無ければその事実を根拠として記録すること。
6. 既存の「ハンドルを掴む」操作を**残すこと**。Blender も両方持っている。
7. モードと軸を **rpc で studio に見せること。**
   ツールバーの表示と、確定時の preview lease 駆動に使う。
   `G` という文字を studio 側の定数にしないこと(割り当ては engine の真実である)。

#### 範囲外

変形中の数値入力(`G` `X` `2` `Enter` で 2 単位移動)。
ローカル軸(`X` を二度押しでローカル)。スナップ。複数選択。
いずれも本 WP の状態機械が入ってから、必要かを判断する。

#### 受け入れ条件

- `G` → `X` → マウス移動 → 確定、で選択オブジェクトが X 軸に沿って動くこと。
  **その量が、同じ軸のハンドルを同じ画素数ドラッグした場合と一致すること**
  (基底が二重化していないことの確認。これが本 WP の中心的な条件である)
- 軸を指定しない `G` が視点平面上で動き、カメラを回すと平面も追従すること
- `Esc` / 右クリックで取り消され、**元の値に戻ること**。lease が開いたまま残らないこと
- モーダルでの変形が **undo で戻り、保存して開き直しても残る**こと(WP275 と同じ条件)
- Inspector の数値欄を編集中に変形が始まらないこと(上記 5 の確認を含む)
- キー割り当てが宣言データであり、プリセットを切り替えられること。
  **`src/devstudio/` にキーコードも `"G"` のような割り当て文字列も存在しないこと**を
  grep で示すこと。ツールバーの表示は rpc 由来であること
- `src/devstudio/` にカメラ・射影の計算が無いままであること(WP276 の条件を維持)
- **`--record-input` で記録し `--replay` で再生したとき、変形が再現すること。**
  engine 側に置いた利点が実際に効いていることの確認である
- モデル層が view から分離され、headless にテストされていること
- `ctest` 全数が緑(`-DSKIP_DEVSTUDIO=ON` でも通ること)、`git diff --check` クリーン

依存: WP276 / WP277 / WP279(いずれもマージ済み)。
**加えて「アクション宣言の合成」が前提** —— engine がエディタ用アクションセットを配って
プロジェクトが継承できないと、`cameracontrollersystem.cpp:183` の
`isUnknownActionError`(例外メッセージの前方一致で握り潰す)と同じ形を再生産する。
`basicconfig.hpp:81` の `input_actions_json_ref` が `std::optional` 一つであることが原因で、
`input_profile_json_refs` が map であるのと非対称になっている。

**入力宣言層を `src/project/` へ移す作業(大)は前提ではない。**
状態機械が engine 側にあり、studio は rpc で状態と割り当てを受け取るだけなので、
studio が binding 文法を持つ必要がない。移設が要るのは
エディタで割り当てを*編集する* UI を作るときであり、そのときに判断すればよい。

見積: 中(engine 側に寄せたことで studio 側の状態機械が消えた)。

### WP287: projects/example の修復と、プロジェクト起動の smoke テスト

**目的**: 看板のデモプロジェクトが起動しない。直す。そして**なぜ誰も気づかなかったか**を塞ぐ。

#### 現状

```
pelican_player --project projects/example --headless --frames 2
Pelican fatal error : material route 'forward_transparent' with shader contract
'forward_scene_color_v1' has no compatible registered material pass
```

**最初の不良コミットは `4fa1586`(2026-07-31)**「feat(material): converge glTF through material lowering」。
親 `dd18ae6` は正常で、両方を実際にビルドして確認済み。約 100 コミット前であり、
レイトレ系および devstudio 系のコミットは無関係。

**引き金は `assets/models/AliciaSolid.vrm`** — このプロジェクトで
`alphaMode: BLEND` を含む唯一のアセット。4fa1586 が glTF/VRM のマテリアルを
semantic material lowering に通すようにしたため、BLEND が
`engine://surfaces/openpbr/blend_double.surface` に写り、
`render_state.blend != opaque` から `MaterialRouteClass::forward_transparent` に分類され、
`MaterialPassContract::forward_transparent_v1` を要求する。
`projects/example/passes/main_rendering_config.json` は透過フォワードパスを宣言していない
(このファイルは 2026-07-12 から未変更。**設定は前から不完全で、4fa1586 が初めて問うた**)。

**注意**: `materials/depth_fade.material.json` と `refract.material.json` は引き金では**ない**。
両方削除しても同じエラーになることを確認済み。

#### なぜテストが鳴らないか

`projects/` 以下を起動するテストは `animgraph_demo_preset_headless_player` の **1 本だけ**。
`projects/example` は `-ProjectPass` としてパス設定を読ませる用途では現れるが、
**プロジェクトとして起動されることが一度もない。**

さらに悪いことに、**終了コードだけでは足りない**。
`projects/animgraph_demo` は `exit 0` で描画結果が**一様な単色**である
(実測。`--render-out` の PNG を目視で確認した)。

#### 実装範囲

1. `projects/example` を起動できるようにすること。
   設定に透過フォワードパスを足すのが本筋だが、決めて理由を書くこと。
2. **エラーメッセージに手がかりを入れること。**現状は route と contract しか言わない。
   どのマテリアル / どのアセット / どの設定ファイルが要求したのかを名指しすること。
   fail-fast は正しいが、名前を出さない硬いエラーは仕事の半分しかしていない。
3. **`projects/` 以下の全プロジェクトをヘッドレス起動する smoke テストを足すこと。**
   - 終了コード 0 だけでなく、**描画結果が一様な単色でないこと**を検査すること
   - プロジェクトが増えたら自動的に対象になること(一覧を手で書かないこと)
   - `animgraph_demo` が一様色である件を調べ、**直すか、一様色が正しい理由を書くか**を決めること。
     どちらでもよいが、黙って例外にしないこと

#### 受け入れ条件

- `pelican_player --project projects/example --headless --frames 2` が exit 0 で、
  **DamagedHelmet が描かれている**こと(画素で確認し、その方法を書くこと)
- 修復前のコードで smoke テストが**落ちること**を確認し、確認方法を書くこと
- smoke テストが `projects/` 以下の**全プロジェクト**を対象にし、
  一様色を不合格とすること
- エラーメッセージが、要求元のマテリアルと設定ファイルを名指しすること
- 既存 golden が動く場合は 1 枚ずつ理由を述べること
- `ctest` 全数が緑、`git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 中。

### WP288: `project init` の雛形を、同梱される既定プロジェクトにする

**目的**: 既定プロジェクトの中身が C++ の文字列リテラルとして
`src/devcli/projectinit.cpp` に直書きされている(`constexpr std::string_view` が 13 個、334 行)。
データをデータの場所へ移す。

**なぜ問題か**: `project_json` / `input_actions_json` / `rendering_config_json` などが
コードの中にある。単体で開けず、編集に再ビルドが要り、
`rendering_config_json` リテラルの中で `engine://features/shadow_directional.json` と
`sky_ambient.json` を選んでいる ——
**エンジンの既定の選択が CLI のソースに埋まっている。**既定値の所在として最悪の場所である。

**方針**: エンジンが**実在するプロジェクトとして既定を同梱**し、
`pelican_cli project init` はそれを複製すること。

**継承ではなく複製であること。** 読み込み時に既定とマージする設計にはしないこと。
プロジェクトの実効設定が複数文書のマージになると、
「このプロジェクトが何か」を 1 つのファイルから読めなくなり **strict v1 と衝突**する。
また compiled graph を fingerprint に取っている以上、
継承された既定は**そのハッシュへのバージョン管理されない入力**になり、
エンジン更新で既存プロジェクトの描画グラフが黙って変わる。**決定性と衝突する。**

伝播が要るもの(エンジンが所有する語彙)は、
feature が既に持っている **opt-in の合成**(プロジェクトファイルに 1 行書く、見える継承)で扱うこと。

**実装範囲**:

1. 既定プロジェクトをリポジトリ内の実プロジェクトとして置くこと。置き場所を決めて理由を書くこと。
2. `projectinit.cpp` の 13 個のリテラルを削除し、複製に置き換えること。
3. **既定プロジェクトが WP287 の smoke テストの対象に入ること。**
   既定が壊れたら鳴る状態にすること。

**受け入れ条件**:

- `pelican_cli project init` の出力が、移設前と**同じ内容**であること(差分で示すこと)
- `src/devcli/projectinit.cpp` に JSON の文字列リテラルが残っていないこと
- 既定プロジェクトが smoke テストで起動され、一様色でないこと
- 既存の `devcli_project_init_command` が通ること
- `ctest` 全数が緑、`git diff --check` クリーン

依存: **WP287**(smoke テストを先に用意するため)。**および WP289**
(既定プロジェクトを作る前に開発時と出荷時の境界を決めておくため)。見積: 中。

### WP289: 開発時オーバーレイ — エディタが必要とする feature を起動時に重ねる

**目的**: エディタが動くために必要な feature(ギズモ、ピッキング)を、
**プロジェクトファイルを一切変更せずに**有効化できるようにする。

#### なぜ要るか

**現状、方法が存在しない。**プレイヤーの起動時上書きは `--game-logic` だけで、
`studioPlayerArguments`(`src/devstudio/viewport/embeddedviewport.cpp:168-176`)は
`--rpc --project <root>` しか渡していない。
`mainwindow.cpp:561,613` は「プロジェクトに feature を足してください」と告げるだけである。

その結果、**リポジトリのどのプロジェクトも `gizmo.json` / `picking.json` を宣言していない**。
素の `projects/example` をスタジオで開いても、クリック選択もギズモも出ない。
足せば動くが、**その編集は出荷物に混ざる**。

パージ可能の原則が「重ねなければ跡形もない」を既に保証している。**足りないのは適用点だけである。**

#### 分けるべきは機能ではなく宣言する主体

| | 誰が宣言するか |
|---|---|
| **ゲーム設定** | プロジェクト。バージョン管理され、出荷され、golden で固定される |
| **開発時設定** | それを必要とする道具(エディタ、プロファイラ)。**プロジェクトファイルに書かれない** |

**機能そのものに「開発用」の印を付けないこと。**
ギズモは、開発時はエディタが重ね、ゲーム内レベルエディタを出荷するなら
そのプロジェクトが宣言する —— 同じ機能、違う宣言者。
印を付けると必ず例外が要る(WP277 が確認した「ゲームもギズモを使いたい」がその例外)。

#### 最重要の構造条件

> **出荷される描画グラフは `rendering_config_json` だけから計算されること。
> 重ね合わせは、別の・名前の付いた経路で適用されること。**

「慣習として開発時設定は出荷しない」では不十分である。
重ね合わせが `project.json` の `basic_config` 内の別キーであれば、
**いつか誰かが間違った場所で読む**。
プロジェクト読み込み経路から見えない場所に置けば、
エディタ抜きで起動したプレイヤーは**そもそも見ることができない**。

golden を重ね合わせ無しで取ることは、この条件の系として自動的に成立する。

#### 実装範囲

1. プレイヤーに**feature を起動時に重ねる公開手段**を足すこと。
   最低限 CLI(`--project` と同じ公開面)。devstudio 専用の裏口にしないこと(D0)。
   プロファイラや将来の道具も同じ手段を使えること。
2. `engine://features/editor.json` を新設し、gizmo と picking を束ねること。
   **studio のソースに feature URI の一覧を直書きしないこと。**既定値の所在は一箇所。
3. `studioPlayerArguments` がそれを渡すこと。
4. 重ね合わせた feature が**プロジェクトに書き戻される経路を作らないこと。**
   保存しても、開き直しても、プロジェクトは重ね合わせを知らないこと。

#### 範囲外

user 層(マシンごとの好み。「自分は `gpu_timing` も出したい」)。
プロジェクト単位の開発時設定(チーム共有)。
いずれも実需が出てから判断する。**この WP はエンジン既定の 1 層だけを作る。**

#### 受け入れ条件

- **素の `projects/example` をスタジオで開くと、クリック選択とギズモのハンドルが出ること。**
  そのセッションの後で `git status projects/example` が**変更なし**であること。
  この 2 つを合わせて確認すること
- 重ね合わせ無しで起動したプレイヤーの出力が**今日と同一**であること。
  既存 golden が 1 枚も動かないこと
- **`src/devstudio/` に feature の URI 文字列が存在しないこと。**grep して 0 件であることを示すこと
- 重ね合わせを外すとターゲットもパスも**存在しない**こと(パージ可能)
- **重ね合わせがプロジェクト読み込み経路から到達不能であること。**
  どう構造的に保証したかを書くこと。「そう書かないから安全」では条件を満たさない
- WP287 の catalog smoke テストが**重ね合わせ無しで**回り続けること
- `ctest` 全数が緑、両ビルド階層でビルドが通ること(§4 規約 9)、
  `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: WP287(マージ済み)。見積: 中。

### WP290: 入力アクションのオーバーレイ

**目的**: 道具が宣言する入力アクション集合を、**プロジェクトの集合と並べて**有効化できるようにする。
WP289 の写しであり、同じ原則の入力側への適用である。

#### 現状は二者択一で、その代償を握り潰しで払っている

`src/core/userpublic/userinput.cpp:114-134` は

```cpp
if (launch.free_camera) { ...engine のアクション集合... }
else                    { ...project のアクション集合... }
```

**厳密な either/or** である。`--free-camera` を使うとプロジェクトのアクションが丸ごと消える。
帰結が 2 つある。

- `--free-camera` と `--input-profile` の併用が硬いエラーになっている(`userinput.cpp:115-118`)
- `src/core/userpublic/cameracontrollersystem.cpp:183` が
  **例外メッセージの前方一致で「不明なアクション」を握り潰している**

```cpp
bool isUnknownActionError(const std::runtime_error &error) {
    const std::string message = error.what();
    return message.rfind("unknown input action:", 0) == 0;
}
```

`CameraControllerSystem` は 2 系統の名前(`look`/`pan`/`zoom`/`move` と `pelican_view_*`)を問い合わせ、
どちらか一方は必ず存在しないため毎回投げ、それを文字列比較で飲み込んでいる。
`projects/example` では毎フレーム 9 件が投げられて捨てられている。
**fail-fast が文字列比較で沈黙に変換されている。**

#### 方針: WP289 と同じ形

エディタの変形キー(G/R/S)も `--free-camera` も、**道具が宣言する開発時の入力**である。
プロジェクトが宣言するゲームの入力とは別物であり、**プロジェクト形式を変える必要はない。**

`EngineLaunchConfig`(`src/core/launchconfig.hpp:57-60`)には既に
`render_feature_overlays` があり、コメントが原則を述べている ——
「Tool-declared startup inputs. These are deliberately absent from ProjectBasicConfig,
so project loading cannot opt into or persist them.」**その隣に置くこと。**

#### 実装範囲

1. `EngineLaunchConfig` に入力アクションのオーバーレイを足すこと。
   公開 CLI 面から与えられること(D0。devstudio 専用にしないこと)。
   `ProjectBasicConfig` に対応するフィールドを**作らないこと** —— WP289 と同じ構造条件。
2. アクション集合を**合成**すること。オーバーレイとプロジェクトの集合が並存すること。
   名前の衝突をどう扱うか決めて、名前付きの硬いエラーにすること。黙って一方を優先しないこと。
3. `--free-camera` を置換ではなくオーバーレイとして表現し直すこと。
   `--input-profile` との併用禁止を**消せるなら消すこと**。消せない理由があれば書くこと。
4. **`isUnknownActionError` と 3 つの `optional*` ラッパを削除すること。**
   不明なアクションを硬いエラーに戻すこと。これが本 WP の主目的である。

#### 範囲外

G/R/S そのもの(WP286)。実行時の再割り当てと user 層への永続化。
`src/project/` への宣言層の移設。

#### 受け入れ条件

- オーバーレイとプロジェクトのアクション集合が**同時に**有効であること。
  `projects/example` を `--free-camera` 付きで起動して、
  プロジェクトの `move` / `jump` と `pelican_view_*` の**両方**が引けることを検証すること
- **`isUnknownActionError` がリポジトリに存在しないこと。**grep して 0 件を示すこと
- 不明なアクション名の問い合わせが**硬いエラーになる**こと(握り潰されないこと)
- 名前の衝突が名前付きの硬いエラーになること
- **`ProjectBasicConfig` にオーバーレイのフィールドが無いこと。**
  プロジェクトが保存・再読み込みでオーバーレイを知らないこと
- オーバーレイ無しの挙動が今日と同一であること。既存 golden が 1 枚も動かないこと
- `--record-input` / `--replay` がオーバーレイ有りでも成立すること
- `ctest` 全数が緑、両ビルド階層でビルドが通ること(§4 規約 9)、
  `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: WP289(マージ済み)。**WP286 の前提。**見積: 中。

### WP291: schema コンパイルテストの直列化を解く

**目的**: CPU 層の走行時間の大半が、18 件のテストが**同じビルドディレクトリを奪い合って
直列化している**ことに費やされている。専用ビルドディレクトリを与えて解く。

**実測**:

- CPU 層(`-LE gpu -j16`)は 1034 件で 43.9 秒
- そのうち **18 件が `RESOURCE_LOCK event_schema_compile_build`**
  (`test/CMakeLists.txt:570`, `:598`)で完全直列。合計 **44.2 秒**
- 同じ層から 18 件を除くと **8.94 秒**
- 1 件あたりの内訳は明瞭で、`event_schema_compile_compile_pass` は **0.94 秒**
  (ターゲットが最新で MSBuild が空振り)、`fail_*` は 1.36〜1.41 秒。
  **つまり 18 件それぞれが 0.94 秒のプロセス起動費用を払っている**

**方針**: `test/run_event_schema_compile.cmake:6` は `cmake --build --target` を
毎回叩いており、ビルドディレクトリが共有なので lock が要る。
**専用ビルドディレクトリを与えれば lock が不要になる。**
手本は `test/run_build_units_smoke.cmake`(既に専用ツリーで動いている)。

**実装範囲**:

1. `event_schema_compile_*` と `struct_schema_compile_*` の各テストに
   専用のビルドディレクトリを与えること。
2. `RESOURCE_LOCK event_schema_compile_build` を外すこと。
3. **各ターゲットの成功/失敗の期待値を、今と同じ粒度で個別に検証し続けること。**
   まとめて 1 件にして「どれかが失敗した」にしないこと。

**受け入れ条件**:

- **CPU 層(`-LE gpu -j16`)が 43.9 秒から有意に短縮されること。**
  変更前後の実測を報告に書くこと
- 18 件の検証内容が変わらないこと。失敗時にどのターゲットが落ちたか特定できること
- `RESOURCE_LOCK event_schema_compile_build` がリポジトリに残っていないこと
- 同時に走る MSBuild が増えるため、`-j16` で不安定にならないこと。
  **3 回連続で緑になることを確認**し、回数を報告に書くこと
- `ctest` 全数が緑、`git diff --check` クリーン

依存: なし。見積: 小〜中。

### WP292: golden を 1 回だけ描く

**目的**: 同じ 52 件の golden を **203 回描いている**。104 回で足りる。
GPU 臨界経路から約 110 秒を落とす。**主張は 1 つも減らさない。**

**実測**:

3 つのテストがそれぞれ独立に `loadGoldenInventoryCases()` を走査し、
**同じ引数で** `renderCase()` を呼んでいる。

| テスト | 呼び出し位置 | 描画数 | 実測 |
|---|---|---|---|
| RGBA8 バイト列 | `test/golden_harness.cpp:10192-10193` | 104 | 106.0 s |
| golden 画像 | `test/golden_harness.cpp:9519` | 52 | 63.6 s |
| 実行トレース | `test/golden_harness.cpp:10736` | 47 | 52.8 s |

**3 エントリで 228.4 秒 = 全体の 33.9%。** 1 描画あたり 1.083 秒で、
**99 描画が既に計算済みの値の再計算**である。

`RenderedCase`(`golden_harness.cpp:141-179`)は
`image` / `execution_trace` / `plan_order` を **1 回の描画から**すべて保持している。
3 つの関数はその**別々のフィールドを読んでいるだけ**である。

**方針**: 1 パスに畳むこと。ケースごとに labels off / on を 1 回ずつ描き、
そこから画像許容差・ハッシュ・トレースの主張を**すべて**適用する。

**実装範囲**:

1. 3 つの走査を 1 つに統合すること。**REQUIRE を 1 つも削らないこと。**
2. `runGoldenImages` のケースごとの `DYNAMIC_SECTION` 報告は
   `CAPTURE` / `INFO` に置き換える必要がある(他 2 つが accumulate-then-compare のため)。
   **失敗時にどのケースが落ちたか分かり続けること。**
3. 独立した決定性の固定(`labels_on == labels_off`、TAA の 2 回描画のバイト一致、
   `runGpuTimingIdentity` の 4 描画)には**触れないこと**。あれらは別の性質を見ている。

**範囲外**: labels on/off の不変条件を 52 件から 3 件に間引くこと。
さらに 55 秒縮むが**これは被覆を捨てる**。本 WP では行わない。

**受け入れ条件**:

- **描画回数が 203 から 104 に減ること。**数え方を報告に書くこと
- **既存 golden が 1 枚も動かないこと**
- 統合前に存在した `REQUIRE` / `CHECK` が**すべて残っていること**。
  統合前後で数えて報告すること
- 失敗時に**どの golden ケースが落ちたか**が出力から分かること。
  意図的に 1 件壊して確認し、その出力を報告に貼ること
- **GPU 全数(`-L gpu`)の実測が短縮されること。**変更前後の秒数を報告に書くこと
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン

依存: なし。**WP291 と並行可**(触るファイルが異なる)。見積: 中。

### WP293: テスト実行ファイルをリンク閉包でまとめる

**目的**: `build/test/Debug` が **3.28 GB** ある。テストの数ではなく
**同じヘッダを 181 回実体化していること**が原因である。リンク閉包でまとめて削る。

#### 測定された事実

- 181 個の実行ファイル、合計 **3,125 MiB**。中央値 **27.7 MB**
- **20 MiB 超の 91 個が 2,658 MiB(全体の 95%)を占め、そこに入るテストは 722 件**
- 費用はケース数と相関しない(**r = 0.166**)。
  `pelican_test_deterministicrng_test.exe` は **TEST_CASE 3 個で 29,024,768 B**、
  `pelican_test_outputcompilefacts_test.exe` は **4 個で 1,319,936 B**。
  差は**引いたエンジンヘッダの数**である(前者 4 本、後者 1 本)
- Catch2 + CRT の下限は約 1.3 MB
- 前回の実ビルドで 177 個のリンクに **121 秒(64 コア)** かかっている
- 予備実験:**159 個のテストオブジェクトが 1 個の 59.6 MB バイナリに約 6.8 秒でリンクでき、
  `/FORCE` 無し、エラー 0、1052 ケースが列挙できた**

#### 統合してもプロセス分離は失われない

`pelican_define_test` は `catch_discover_tests`(`test/CMakeLists.txt:58,62`)を使っており、
**TEST_CASE ごとに ctest エントリを作りバイナリを再起動する**。
バイナリをまとめてもプロセスはまとまらない。
golden / replay / 決定性のテストが要求するプロセス分離は**そのまま維持される**。
これが本 WP を成立させている前提である。

#### 1 個にまとめないこと

**リンク閉包でグループ分けすること。**全部を 1 個にすると

- コンパイルエラー 1 つで全テストがビルド不能になる
- テスト 1 本を直す開発者が全体を再ビルドすることになる

95% の容量が「レンダラ側を引く 91 個」に集中している以上、
**そこをまとめるだけでほぼ全ての効果が得られる。**
グループの切り方は実測(どのターゲットが何を引くか)で決めること。理由を書くこと。

独自の `main` を持つ 10 本(`audio_disabled_probe` / `behavior_determinism_probe` /
`devstudio_engine_owner_fixture` / `gltf_scene_fixture_writer` / `physics_feature_probe` /
`png_nonuniform_check` / `process_fixture_child` / `project_library_boundary` /
`vat_fixture_writer` / `vrm_expression_preview_writer`)は**統合対象外**である。

#### 名指しされた危険

**すべてのテストプロセスが、統合したグループ全 TU の静的初期化子を走らせるようになる。**
このリポジトリはモジュールコンテナと大域登録を使っているため、
**テストの結果が静かに変わりうる。**推測で済ませず、確かめること。

#### 実装範囲

1. リンク閉包でグループを決め、`pelican_define_test` をグループに対応させること。
2. **グループごとにビルド可能な named target を残すこと。**
   テスト 1 本を直すときに全部を再ビルドしない道を確保すること。
3. `PELICAN_LEAN_TEST_ARTIFACTS`(`test/CMakeLists.txt:10`)との関係を確認すること。
   同じ問題への部分的な対処なので、重複するなら整理すること。

#### 受け入れ条件

- **`ctest -N` の出力が変更前と完全に一致すること。**
  テスト名も件数も順序も変わらないこと。差分を取って報告すること
  (テストが失われていないことの、これが一番強い証拠である)
- **`build/test/Debug` の合計サイズを変更前後で報告すること。**3.28 GB からどれだけ減ったか
- **リンク時間を変更前後で報告すること**(前回実測 121 秒 / 64 コア)
- **静的初期化子の影響が無いことを確かめること。**
  どう確かめたかを書くこと。「たぶん大丈夫」では条件を満たさない
- **グループ 1 つだけをビルドする時間を報告すること。**開発者の内周が悪化していないこと
- 独自 `main` の 10 本が単独のままであること
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン

依存: なし。**WP291 / WP292 とはファイルが重なる**(`test/CMakeLists.txt`)ため、
**両者のマージ後に着手すること。**見積: 大。

### WP294: smoke テストの対象を、指定できるプロジェクト置き場にする

**目的**: `project_catalog_headless_smoke` が**新規クローンでも worktree でも必ず落ちる**。
リポジトリの `projects/` を対象にしているが、そこのモデル資産は追跡されていない。

#### 現状

`test/CMakeLists.txt:1530` が `-DPROJECTS_DIR=${CMAKE_SOURCE_DIR}/projects` を渡し、
`run_project_catalog_smoke.cmake:16` がそこを glob する。
ところが `.gitignore:12` が `projects/example/assets/models/*.glb` を除外しているため、
**`git worktree add` した先にはモデルが 1 つも存在しない**(本体には 6 個、worktree には 0 個)。

結果として、このテストは**資産がたまたま手元にあるマシンでしか通らない**。
実害は既に出ており、WP290 と WP291 のエージェントがそれぞれ 1 回踏んで作業を無駄にした。
§4 規約 6 が WP ごとの worktree を義務づけている以上、
**このテストは、計画自身が定めた作業手順の中で通らない。**

#### 方針(利用者の判断)

単独開発である間は**テスト用のプロジェクト置き場を指定して、そこを対象にする**。
将来この形式を他者に使ってもらうなら、プロジェクト側は別リポジトリに分ける。

したがって「資産を追跡対象にする(+80MB)」も「取得の仕組みを作る(大)」も採らない。
**対象を設定可能にすることが本 WP である。**

#### 実装範囲

1. 対象ディレクトリを CMake のキャッシュ変数にすること。
   `PROJECTS_DIR` は既に `run_project_catalog_smoke.cmake` の引数になっているので、
   登録側(`test/CMakeLists.txt:1530`)の固定を外すだけで足りるはずである。
2. **未指定のとき、テストが失敗しないこと。**ただし黙って成功にもしないこと。
   **名前付きの理由を持つ skip** にすること。
3. **その skip を既存の許可リスト機構に登録すること**
   (`test/ci/gpu_skip_allowlist.txt` と `skip_policy.py`)。
   登録された skip が現れなくなればゲートが落ちるため、**この skip は腐らない。**
   黙って skip し続ける状態を作らないこと。
4. 指定されたときは、今と同じ検査(起動・終了コード・単色でないこと)を行うこと。

#### 受け入れ条件

- **`git worktree add` した先で `ctest` 全数が緑になること。**
  資産を手でコピーせずに確認し、確認手順を報告に書くこと。
  **これが本 WP の中心条件である**(現状はここで落ちる)
- 対象を指定した実行で、今と同じ検査が行われること。
  `projects/` を指定して従来どおり通ることを示すこと
- 未指定時の skip が `gpu_skip_allowlist.txt` に登録され、
  CPU/GPU ゲートがそれを許容し、かつ**登録が消えたら落ちる**こと
- `ctest` 全数が緑、`git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: WP287 / WP293(いずれもマージ済み)。見積: 小。

### WP295: 製品が動くことを見る層

**目的**: 1181 件のテストが緑でありながら、**製品が動くことを誰も見ていない**。
実プロジェクトすべてで起動に失敗するプレイヤーに差し替えても **1169 件が通る**。
今日 1 日で利用者に届いた 9 件の欠陥は、**全部この境界の向こう側**にあった。

- `projects/example` が 9 日間起動しなかった(テストは緑)
- `animgraph_demo` が exit 0 で一様な単色を描いていた
- ギズモと picking はどのプロジェクトも有効化しておらず、エディタ経路が一度も通っていなかった
- `G` / `R` / `S` が既定プリセットの取り違えで死んでいた(**規約 10 適用後の WP で**)

**足すのは量ではなく種類である。**測定で約 30 秒、全体の 5%。

#### 設計

**土台は `test/run_project_catalog_smoke.cmake`(WP287 / WP294)。**
既に全プロジェクトを glob し、`VUID-` を走査し、対象置き場を指定できる。**作り直さないこと。**

**① 最終フレームを見ること。**現在は 1 枚しか見ていない。
`--render-out` を連番にし、`--frames` を明示して**最後のフレーム**に対して主張する。

各プロジェクトについて:

- 終了コード 0
- 出力に `Validation Error` / `VUID-` が無いこと
- PNG が復号できること
- **異なる色の数**が、そのプロジェクトについて記録した下限以上
- **最頻色以外の画素の割合**が記録した下限以上
- アニメーションを宣言するプロジェクトでは**最終フレーム ≠ 1 フレーム目**

`pelican_test_png_nonuniform_check` は現在「2 色以上あること」しか見ておらず、
**内容の 96% を失っても通る**。上の 2 つの下限がその代わりである。

**② エディタ経路を 1 件。Qt は要らない。**

`pelican_player --rpc --headless --project X --feature-overlay engine://features/editor.json`
は既に exit 0 で、フレームプランに `gizmo_pass` と `picking_pass` が出る(実測 +0.11 秒)。
rpc で `scene_tree` → `pick_object` → `set_gizmo` → `render_frame` → `capture` を駆動し、

- `gizmo_pass` が存在すること
- **何も選択していない同じセッションの capture と異なること**(規約 10 の対比)

**これが今日の欠陥 4 件目を直接捕まえる。**

#### 規約 10 の入口条件を満たすこと

**studio が実際に渡す argv の形で起動すること。**
`studioPlayerArguments()` が生成する形(値なしの旗を含む)を使うこと。
明示指定だけを試すと、既定が壊れていても緑になる ——
これは 2026-08-13 に実際に起きた。

主張は**走っている側が解決した値**に対して行うこと。
引数が存在することを主張しても意味がない。

#### 範囲外

シナリオ golden。**52 枚を既に抱えており、うち 40 枚は許容差 0。**
12 枚足すと、正当な描画変更のたびの更新費用が約 25% 増える。
上の主張(終了コード・VUID・パスの存在・基準との差・最頻色以外の割合)は
**シェーディングを変えても再基準化が要らない。**だからこの一覧なのである。

記録入力によるシナリオも本 WP では作らない。作るなら 10 フレーム以内、
画像で固定せず**変化の方向**を主張すること。

#### 既知の落とし穴(先に潰すこと)

- **`projects/example` は入力が無いと 6 フレームがバイト単位で同一**である。
  フレーム間の主張をそのまま置くと**空虚になる**
- **`--replay` で `--frames` を明示しないと、単一パスの `--render-out` は
  最後ではなく 1 フレーム目を書く**

#### 受け入れ条件

- **修正前の `projects/example`(4fa1586〜d3c378a の状態)でこの層が落ちること**を確認すること。
  確認方法を報告に書くこと。落ちないなら 9 日間の欠陥を捕まえられない
- **`animgraph_demo` の UI マーカーを外した状態で落ちること**を確認すること。
  今の「一様でない」判定より強いことの確認である
- エディタ経路の主張が、**選択なしの実行と異なる**ことを示すこと
- 追加分の実測時間を報告すること。**30 秒を大きく超えるなら削ること**
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

依存: WP287 / WP294(マージ済み)。見積: 中。

### WP296: 起動時間 — ビルド種別に依らない 3 点

**目的**: `projects/example` の起動は Debug 4568ms / RelWithDebInfo 1235ms。
**差の 3.3 秒は Debug ビルドであってエンジンではない**(出力フレームはバイト単位で同一)。
本 WP は**ビルド種別に依らず効く 3 点**だけを扱う。Debug/Release の選択は別途。

**回帰ではないことの確認済み**: 3.50 秒という過去の測定値は、
`projects/example` が起動できなかった時期のものであり比較にならない。
当時の状態を復元して今日のバイナリで走らせると material route エラーで exit 1 になる。
候補コミット 5 点の実測も ±3% に収まっている。

#### ① 参照されていない資産を積んでいる

`projects/example/assets/asset_data.json:6` が `alicia`
(`assets/models/AliciaSolid.vrm`、7.5MB)を宣言しているが、
**シーンからもコードからも参照されていない**(grep で 0 件、確認済み)。
`model.cpp:366-394` は資産表の全項目を無条件に読む。

実測: **Debug 1243ms / RelWithDebInfo 85ms**。

**要求**: 資産表が「何も使わないもの」を並べない状態にすること。
外すか、シーンに置いて実際に見せるかを決め、**理由を書くこと**。
`docs/example_assets.md` がこの資産を記載しているので、そちらも整合させること。
なお `alicia` は VRM であり、example が VRM を見せる意図なら
外すのではなく置くほうが正しい可能性がある。**判断して述べること。**

**範囲外**: エンジン側で「参照されていないモデルを読まない」ようにすること。
`model.cpp:401-412` に遅延経路が既にあるが、
初回参照でストールする挙動とエディタ/ホットリロードの期待を変える。別 WP に属する。

#### ② 434MB のコピーが 1 回

`src/core/model/gltfimage.cpp:96` の `const auto decoded = parallelPrepareOrdered<DecodedImage>(...)`
が `const` であるため、`:100-104` の取り出しが**コピーになる**。

**要求**: `const` を外して `std::move` で取り出すこと。実測 **RelWithDebInfo で約 62ms**。

#### ③ ray query 拡張を、デバイス対応ではなくプロジェクト要求で有効化する

`src/core/vkcore/devicefeaturepolicy.cpp` は
`VK_KHR_acceleration_structure` / `ray_query` / `deferred_host_operations` を
**デバイスが対応していれば日和見的に**有効化する。
実測 **95ms**。RelWithDebInfo の 1.03 秒に対して **9%** である。

**これは WP280 で私(Claude)が選んだ設計である。**`multiview` の前例に倣って
「機能があれば有効化し、選択は宣言に委ねる」とした。費用を測っていなかった。

**先に答えるべき問い**: **論理デバイス生成の時点で、
プロジェクトが ray query を要求するかどうかを知れるか。**
知れないなら本項は 95ms では済まない。
**知れないと判断した場合は、実装せずにその根拠を報告すること。**
無理に押し込まないこと。

知れる場合でも、失うものを述べること —
日和見的な有効化は「このデバイスは対応しているか」の探索を可能にしていた。

#### 受け入れ条件

- **3 項目それぞれについて、変更前後の起動時間を個別に実測して報告すること。**
  まとめての測定では、どれが効いたか分からない
- 測定は同一プロジェクト・同一マシンで、**最低 3 回の中央値**を使うこと
- 既存 golden が 1 枚も動かないこと
- WP295 の catalog smoke が緑のままであること
  (① で資産構成を変えるなら、色数の下限に影響しないことを確認すること)
- ③ を見送る場合、その根拠が報告に書かれていること
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

依存: WP295(マージ済み)。見積: 小〜中。

### WP297: Vulkan 検証レイヤを実行時の選択にする

**目的**: 検証レイヤが `#ifdef _DEBUG` に溶接されている。
**「速いビルド」と「検証あり」が排他になっている**のが問題の本体である。外す。

#### 現状

`src/core/vkcore/core.cpp:74` と `:154` の 2 箇所が
`#ifdef _DEBUG` で `VK_LAYER_KHRONOS_validation` を積み、
`:96-100` が同期検証(`eSynchronizationValidation`)も併せて有効にする。
**実行時に切り替える手段は存在しない。**

実測(`projects/example`、同一マシン、フレーム出力はバイト単位で同一):

| | Debug | RelWithDebInfo |
|---|---|---|
| vulkan | 324ms | 325ms(**1.00 倍** = ドライバの実仕事) |
| shaders | 977ms | 47ms(20.8 倍) |
| models | 3562ms | 611ms(5.8 倍) |
| **total** | **4568ms** | **1235ms** |

差の大半は最適化そのものより MSVC の Debug STL(`_ITERATOR_DEBUG_LEVEL=2`)である。

#### なぜ切り替えるべきなのが「ビルド種別」ではないか

いまは 2 通りしか選べない。外せば 3 通りになる。

| 状況 | 構成 | 起動 |
|---|---|---|
| ゲーム開発、パイプラインが安定 | 非 Debug + 検証オフ | 1.0 秒 |
| **エンジンを触っている** | **非 Debug + 検証オン** | **速いまま安全** |
| コードをステップ実行 | Debug | 3.4 秒 |

**真ん中がいま存在しない。**これが一番惜しい。
Vulkan を書いている最中に速いビルドで検証を効かせられる価値は、
「安定したから検証を切る」より大きい。

`assert` 64 個は `NDEBUG` に紐づくため本 WP では切り離せない。差は残る。

#### 実装範囲

1. 検証レイヤの有効化を**実行時の選択**にすること。同期検証も含めること。
2. **既定は今日の挙動を厳密に保つこと。**`_DEBUG` ビルドではオン、それ以外ではオフ。
   既定のまま起動したときの挙動が変わってはならない。
3. **公開 CLI 面に出すこと**(D0)。devstudio 専用にしないこと。studio も渡せること。
4. **要求したのに層が導入されていない場合は名前付きの硬いエラー**にすること。
   黙って無効で起動しないこと。要求していない場合は当然失敗しないこと。
5. 同期検証を独立に選べるかは決めてよい。決めた理由を書くこと
   (同期検証は最も高価で最も価値がある)。

#### 受け入れ条件(§4 規約 10)

- **既定のまま**の `_DEBUG` ビルドで、検証レイヤが**実際に有効になっている**こと。
  **走っている側が報告する状態**に対して主張すること。旗の有無ではない
- **既定のまま**の非 `_DEBUG` ビルドで、検証レイヤが有効になっていないこと
- **非 `_DEBUG` ビルドで明示的に要求すると有効になること。**
  これが本 WP の目的である。要求した実行と要求しない実行が**観測可能に異なる**こと
- 層が導入されていない状態で要求したとき、名前付きの硬いエラーになること。
  どう確認したかを書くこと
- **検証の有無による起動時間と 1 フレームあたりの費用を実測して報告すること。**
  利用者が「何を買っているか」を知れるようにすること
- 既存 golden が 1 枚も動かないこと
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

#### 範囲外

スタジオがどのプレイヤーバイナリを起動するかの設定。
それは**エンジンが存在する前**の判断であり、スタジオ自身の設定に属する。
本 WP が入ってから実需に合わせて決める ——
**選ぶ機構を先に作ると、選ぶ理由のほうが変わる。**

依存: なし。見積: 小。

### WP298: 落ちた理由を studio に届ける

**目的**: エンジンが落ちたとき、**エディタは何が起きたか一言も伝えない**。
利用者は「編集 → 起動 → 死ぬ → 何も言われない → 勘で直す」を繰り返すことになる。

#### 現状:メッセージは十分で、経路が繋がっていない

エンジンの致命エラーは**必要な情報を全部持っている**。WP287 で名前を出すようにした:

```
material 'Alicia_face_mastuge' from asset '...AliciaSolid.vrm'
requires route 'forward_transparent' with shader contract 'forward_scene_color_v1',
but rendering config 'passes/main_rendering_config.json' has no compatible registered material pass
```

これが `src/core/userpublic/pelican_core.cpp:98` で `LOG_ERROR` される。

ところが studio は `--rpc` でプレイヤーを起動するため、
`src/core/log.cpp:50` が「stdout はプロトコル用に予約」と判断して**ログをファイルに送る**。
そして **studio は `pelican.log` を一度も開かない**(`grep -rn "pelican.log" src/devstudio/` が 0 件、確認済み)。

**一方 stderr は既に studio に届いている。**
`src/devstudio/viewport/engineprocess.cpp:145` の `drainStandardError` が
`outputReceived`(`:311`)経由で Engine Log ドックに流している。
`--dump-frame-plan` も `std::cerr` を使っており(`src/core/appflow/loop.cpp:132`)、
**stderr がデータ経路として既に使われている**ことも確認できる。

**配管はある。繋がっていないだけである。**

そして利用者が見るのは `src/devstudio/view/frameplanwidget.cpp:254` の

> Frame plan unavailable: ... Start or restart pelican_player, then press Refresh.

**「まだ再生していない」と「設定が壊れていて起動できない」が区別できない。**

#### 実装範囲

1. `src/core/log.cpp:46-58` で、`reserve_stdout_for_protocol` が真のとき
   **ファイルシンクの代わりにではなく、並べて** stderr シンクを作ること。
   **stdout には何も足さないこと** —— JSON-RPC の枠組みを壊す。
2. **既定の挙動を減らさないこと。**ファイルシンクは残す。追加であって変更ではない。
   非 rpc 実行と Release の経路が変わらないこと。
3. studio 側で stderr の直近 N 行を保持し、**プレイヤーが非ゼロ終了したとき**に
   Engine Log ドックを前面に出し、Frame Plan の案内文を
   **最後の `Pelican fatal error` 行**で置き換えること。
4. モデル層を view から分離し、headless にテストできること
   (既存の devstudio テストと同じ作法)。

#### 受け入れ条件(§4 規約 10 とその追記)

- **`projects/example` を意図的に壊し**(マテリアルのルートに対応するパスを外すなど)、
  **エンジンが出した文言そのものが studio 側に届くこと**を検証すること。
  「シンクが構築された」ことの確認では条件を満たさない。
  **走っている側が出した文字列**に対して主張すること
- **正常起動時に、その表示が出ないこと。**対照として同じ確認を行うこと
- 「まだ再生していない」と「起動に失敗した」が**表示上区別できる**こと
- **stdout に 1 バイトも足していないこと。**rpc の往復が壊れていないことを確認すること
- 既定のまま(旗なし)の挙動が変わっていないこと。
  非 rpc 実行でファイルシンクが従来どおり働くこと
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

#### 範囲外

frame plan の表示内容そのもの(WP299)。feature の来歴(WP300)。
**この WP は経路だけを繋ぐ。**

依存: なし。**新しい rpc は不要。**見積: 小。

### WP299: すでに線に乗っている「理由」を studio が読む

**目的**: 「なぜこうコンパイルされたか」を**エンジンは既に書いて送っている**。
studio が読んでいないだけである。**エンジンの変更はゼロ。**

#### 現状:データは届いていて、モデル層が捨てている

`get_frame_plan` の応答(`projects/example` で実測 **165 KB**)には既に入っている:

| 内容 | 出所 |
|---|---|
| `physical_target_plan.decisions[]{id,subject,selected,detail}` | `src/project/targetrenderplanning.cpp:4123` |
| `planning_opportunities.decisions[]` | `src/project/targetplanning.cpp:1036` |
| `physical_target_plan.resources[].reason` | `targetrenderplanning.cpp:4019` |
| `backend_selection` の候補別 `failures[]` / `diagnostics[]` | `targetplanning.cpp:704`, `:112` |
| `material_filter.unmatched_include` / `unmatched_exclude` | `src/core/renderingpass/frameplanner.cpp:1836` |

語彙は `PlanningDecision` / `PlanningDiagnostic` / `BackendConstraintFailure`
(`src/project/targetplanning.hpp:230`, `:151`, `:204`)。
**45 個の版付き ID が約 20 箇所で書かれている。**

実際に叩いて確認した例:

```
pelican.plan.multiview_auto_gate@1
  → "no measured device profile matched; optimize-by-default selects multiview"
pelican.plan.backend_candidate_selected@1
  → "lowest deterministic cost tuple"
```

**`grep -rc "decisions" src/devstudio/` は 0 件。**
`buildFramePlanModel`(`src/devstudio/model/frameplanmodel.cpp:200-567`)は
**厳格な許可リスト**であり、`physical_target_plan` に触れるのは `:380` と `:459` の 2 箇所だけ。
**残りは黙って捨てられ、生 JSON の退避先も無い。**

#### 実装範囲

1. 決定を `subject` で束ねて見せること。`id` と `detail` の両方を出すこと
   —— `id` は版付きで検索でき、`detail` は人が読む。
2. 資源ごとの `reason` を詳細行に出すこと。
3. マテリアルフィルタの `unmatched_include` / `unmatched_exclude` をパスの詳細に出すこと。
   **「宣言したフィルタが何にも一致しなかった」は手で書いている人が最も踏む間違いである。**
4. 却下されたバックエンド候補を、その `failures` とともに見せること。
5. **既存の絞り込み欄の裏に、生 JSON の退避表示を置くこと。**
   モデル化していない鍵が**黙って消えない**ようにすること。

#### fixture は実際の応答を取り込むこと

**手書きしないこと。**先例がある。

`test/compiledplanviewer_test.cpp:36-44` はエンジンが**一度も出したことのない形**を手書きしている
—— `view_count` を根に置き、`planning_opportunities` を配列にしている。
実際の応答では `planning_opportunities` は**オブジェクト**であり、
`view_count` は入れ子で 61 回現れる(両方とも実測で確認済み)。
その結果 `buildCompiledPlanFacts` は 13 個の鍵のうち 8 個を誤った階層で読み、
`buildCompiledPlanOpportunities` は**到達不能**である。**そしてテストは緑である。**

**要求**: fixture は `--dump-frame-plan` あるいは `get_frame_plan` の
**実出力を取り込んだもの**をリポジトリに置いて使うこと。
大きい(165 KB)ので、必要なら区間を切り出してよいが、
**形は実物と一致していること。**切り出したなら、その方法を書くこと。

#### 受け入れ条件(§4 規約 10)

- **応答に含まれる決定が、モデルにすべて現れること。**
  取り込んだ実 fixture の `decisions` を数え、モデル側の件数と一致することを主張すること
- **モデル化していない鍵が黙って消えないこと。**
  fixture に未知の鍵を足したとき、それが生 JSON の退避表示に現れることを検証すること。
  これが本 WP の中心条件である —— 許可リストが黙って捨てるのが元の欠陥である
- 決定が 1 件も無い応答で、決定の表示が**空になること**(捏造も異常終了もしないこと)。
  規約 10 の対照である
- モデル層が view から分離され、headless にテストされていること
- **エンジン側の差分が無いこと。**`git diff --stat src/core/ src/project/` が空であることを示すこと
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

#### 範囲外

ノード図・辺・レイアウト。**まず読めることが先である。**
順序の根拠(`after`/`before` は線に乗っていない —— `frameplanner.cpp:1719`)。
feature の来歴(WP300)。

依存: なし。**新しい rpc は不要。エンジン変更もゼロ。**見積: 中。

### WP300: feature の来歴を compose 時に刻む

**目的**: 「このパスは誰が足したのか」に答える。
**いまは推測しており、しかも間違いうる。**

#### 現状:2 回目のコンパイルで推測している

`buildRuntimeAnnotations`(`src/core/imgui/planviewer.cpp:161-287`)は
`PathResolver` で生の設定を読み直し、`resolveRenderPipeline` を**もう一度**走らせて
来歴を導出している。しかもその再実行は
`RenderEnvironmentCapabilities{true, RenderPipelineGraphVariant::flat}`
(`planviewer.cpp:179-180`)を**直書き**している。

つまり `runtime_shader_compiler_enabled` を常に真とし、variant を常に `flat` とする。
**XR/multiview の variant や `PELICAN_RUNTIME_SHADER_COMPILER=OFF` の構成では、
実際にコンパイルされたものと食い違う。**
来歴の表示が嘘をつきうる状態である。

**本当の直し方は、実際の compose の最中に刻むことである。**
`addFeaturePasses`(`src/project/featurecompose.cpp:1789-1817`)は
`entry.at("pass")` をそのまま挿入し、ターゲットは `targets.push_back(target)`(`:1505-1523`)。
**どちらも印を残さない。**

**前例は既にあり、1 箇所だけ機能している。**
surface contract には `provider_feature` / `provider_reference` が刻まれ
(`featurecompose.cpp:2193`)、`:2525` と `:2536` で使われている。
**同じ作法を pass と target に広げるだけである。**

#### 実装範囲

1. compose の最中に、パスとレンダーターゲット(およびバッファ)へ来歴を刻むこと。
   `source` は少なくとも `project` / `feature:<name>` / `engine` を区別できること。
   命名と形は `provider_feature` の前例に合わせること。**第二の流儀を作らないこと。**
2. 刻んだ来歴を既存の `get_frame_plan` の応答に載せること。
   **新しい rpc を作らないこと。**
3. studio がそれを表示すること(WP299 の表示に足す)。
4. **`buildRuntimeAnnotations` の再コンパイルを削除すること。**
   刻んだ値があるなら、推測は冗長であり、かつ間違いうる。**残さないこと。**

#### 受け入れ条件(§4 規約 10)

- パスとターゲットの来歴が、**compose が実際に行ったこと**と一致すること。
  feature を 1 つ足した前後で、増えたパスの `source` がその feature を指すこと
- **推測が嘘をつく構成で、刻んだ値が正しいこと。**
  `PELICAN_RUNTIME_SHADER_COMPILER=OFF` の構成で確認すること
  —— 旧経路は `true` を直書きしているので、ここが両者の分かれ目である。
  **これが本 WP の中心的な対照である**
- `buildRuntimeAnnotations` の再コンパイル経路がリポジトリに残っていないこと
- feature を使わないプロジェクトで、来歴が `project` / `engine` として正しく出ること
- 既存 golden が 1 枚も動かないこと。
  **来歴は診断であって描画に影響してはならない** —— 影響したらこの WP の失敗である
- 新しい rpc を追加していないこと
- `ctest` 全数が緑(`-j4`)、両ビルド階層でビルドが通ること(§4 規約 9)、
  `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

#### 範囲外

ノード図・辺・レイアウト。順序の根拠(`after`/`before` は線に乗っていない)。

依存: **WP299**(studio の表示に足すため、そのマージ後に着手すること)。見積: 中〜大。

## 論理→物理の差分を見せる(WP301〜WP307)

**目的**: レンダリングパスを組む者が、**自分が書いた論理構造に対して
エンジンが何を統合し何を分けたままにしたか**を見られるようにする。

**調査で判明した前提**: 差分の材料はエンジンが既に全部算出し、
`get_frame_plan` の `physical_target_plan` に載せている。
`alias_groups`(採用された統合)、`planning_opportunities.alias_candidates`
(合法だが不採用だった組)、resource ごとの `lifetime{first_use,last_use}` /
`aliasable` / `representation` / `reason`、`lowering_graph`(論理 30 ノード→物理)、
128 件の decision。**エンジン側に新しい計算は要らない。**

`projects/example` の実測: ターゲット 22 個、エイリアス候補 2 組、採用 1 組
(`Bloom_Threshold_RT` ＋ `g_emissive`)、不採用 1 組
(`Bloom_Threshold_RT` ＋ `gbuffer_albedo`)。
22 個中 16 個が `reason: "arbitrary read requires a materialized resource"`、
`widest_read` が `same_pixel` なのは 1 個のみ。
`parallel_candidates` と `fusion_candidates` はいずれも 0。

**差分の基準は論理層(composed config)であって、書かれた生の JSON ではない。**
生ファイルを基準にすると、feature 合成・hdr 名による bloom 連鎖の再配置・
scene/display ターゲットの format 上書き・engine が注入する anchor/display/snapshot・
compute task の graph 数分の複製が、すべて利用者側のノイズとして現れる。

### WP301: xr variant で compute ノードの来歴が消える

**目的**: WP300 で入れた来歴表示が、xr variant で無音のまま無効化されている。直す。

#### 現状

`synchronizeRenderPipelineProvenance`(`src/core/renderingpass/vulkanrendercompilerprogram.cpp:402`)は
`namespaceComputeTasks`(`:481`、実体 `:249`)**より前**に走る。
`namespaceComputeTasks` は compute ノードの名前と `after`/`before` 参照すべてに
variant 接尾辞(`#xr`)を付けて改名する。

来歴は**名前をキーに join** される(`frameplanner.cpp:1740-1741`)ため、
xr variant では join が外れ、**compute ノードの `source` / `provider_feature` が
一つも出ない**。例外は投げられず、`engine` に無音で格下げされる。

**WP300 の受け入れ条件は flat variant でしか確認していなかった。**

#### 実装範囲

1. 来歴の join が改名を跨いで成立すること。
   同期を改名の後に移すか、名前以外のキーで join すること。
   **どちらを採るかは WP304 の安定 ID と整合させること。第二の識別体系を作らないこと。**
2. 直したことを、名前一致では落ちる構成で確認すること。

#### 受け入れ条件(§4 規約 10)

- **xr variant を有効にした構成で、compute ノードの `source` が
  実際に compose が行ったことと一致すること。**
  これが本 WP の中心的な対照である —— flat のみで確認して終えないこと
- flat variant の来歴が WP300 の結果から変化しないこと
- 既存 golden が 1 枚も動かないこと
- `ctest` 全数が緑(`-j4`)、両ビルド階層でビルドが通ること(§4 規約 9)、
  `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 小。

### WP302: 到達不能な planning opportunities 表示と、実データを見ていないテスト

**目的**: ImGui の planning opportunities 節は**一度も描画されたことがない**。
そしてテストは通っている。テストが実データを見ていないためである。

#### 現状

`buildCompiledPlanOpportunities`(`src/core/imgui/compiledplanviewer.cpp:159-160`)は

```
const auto it = plan_json.find("planning_opportunities");
if (it == plan_json.end() || !it->is_array()) return rows;
```

と**配列を要求する**。しかし producer が出すのは**オブジェクト**である
(`test/fixtures/devstudio/example_frame_plan.json` の
`physical_target_plan.planning_opportunities` は
`alias_candidates` / `fusion_candidates` / `parallel_candidates` / `decisions` /
`profile` / `node_order` / `seed` を持つオブジェクト)。

したがって常に空を返し、`:413` の `!program.planning_opportunities.empty()` は
常に偽となり、**節は描画されない**。
既存テストは手書きの配列を食わせているため通る。

これは §4 規約 10 の 2026-08-13 追記(**動いている側が実際に解決したものを検査せよ**)が
狙っていた欠陥そのものである。

#### 実装範囲

1. reader を producer の実際の形に合わせること。
   **producer を配列に変えて逃げないこと** —— alias/fusion/parallel の候補は
   それぞれ意味が違い、平坦な文字列列では利用者に届かない。
2. 少なくとも alias 候補・fusion 候補・parallel 候補の別と、
   採用/不採用の別が読めること。
3. **テストが実データを通ること。**
   `test/fixtures/devstudio/example_frame_plan.json` を入力に使うこと。

#### 受け入れ条件(§4 規約 10)

- **実 fixture を入力にしたとき、節が空でないこと**、かつ
  候補が 2 組・採用 1 組として読めること。
  手書き JSON でしか通らないテストを残さないこと
- **候補が 0 件の構成(`conservative_debug` profile)で、節が
  「候補なし」を明示すること** —— 空表示と未実装の区別がつくこと。
  これが本 WP の対照である
- `ctest` 全数が緑(`-j4`)、両ビルド階層でビルドが通ること(§4 規約 9)、
  `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 小。

### WP303: gpu_draw_source が非マテリアルパスで検証されない

**目的**: 未束縛の read エッジが無音で作られる経路を塞ぐ。

#### 現状

`validatePassSpecificFields`(`src/core/renderingpass/renderingpassvalidation.cpp:472`)は
`material_range` / `material_filter` / `material_variant` / `material_outputs` について
「マテリアルパス以外では使えない」と名前付きで弾く。

**`gpu_draw_source` はその一覧に無い。**
`grep gpu_draw_source src/core/renderingpass/renderingpassvalidation.cpp` は 0 件、
一方 `materialpassinfojsonparser.cpp` には 33 箇所ある。

非マテリアルパスが `gpu_draw_source` を持つと、fail-fast されずに通り、
束縛されない read エッジが残る。

#### 実装範囲

1. `gpu_draw_source` が非マテリアルパスに現れた場合を、既存 4 件と**同じ作法**で
   名前付きに弾くこと。**第二の流儀を作らないこと。**
2. 他に同じ穴が空いている `material*` 系フィールドが無いかを確認し、
   あれば同時に塞ぐこと。

#### 受け入れ条件(§4 規約 10)

- 非マテリアルパスに `gpu_draw_source` を置いた設定が、
  **パスの名前とフィールド名を含む**エラーで失敗すること
- **マテリアルパスに置いた同じ `gpu_draw_source` は従来どおり通ること** ——
  これが本 WP の対照である。弾きすぎていないこと
- 既存 4 プロジェクトが従来どおり起動すること
- `ctest` 全数が緑(`-j4`)、両ビルド階層でビルドが通ること(§4 規約 9)、
  `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 小。

### WP304: ノードとリソースに安定した識別子を与える

**目的**: 差分が「改名」と「削除＋追加」を区別できるようにする。
**そして編集機能が乗る土台を、無料で入れられるうちに入れる。**

#### 現状:識別子は名前文字列しかない

- `namespaceComputeTasks`(`vulkanrendercompilerprogram.cpp:249`)が
  compute ノードと graph の名前を variant 接尾辞つきに**改名する**
- `declaration_index` は**変換後**の config を指すので、
  feature がパスを 1 つ挿入すると全部ずれる
- 来歴は名前キーで join され、重複で throw し、改名で無音に `engine` へ落ちる(WP301)
- frame graph と rendering pass の 2 つの独立したパースは、同じ名前文字列だけで
  join され、不一致で throw する

**改名を跨いで生存する識別子が、どの層にも無い。**

#### なぜ今か:移行コストが現在ちょうどゼロ

安定 ID は `TargetLoweringNode::source_nodes` の実質を変え、
`appendTargetLoweringGraphFingerprint`(`src/project/vulkanphysicalfragment.cpp:701-702`)に
畳み込まれているため、**既存の pin / fragment package を無効化する**。

無効化する対象を数えた。`projects/` の 4 プロジェクト
(`example` / `animgraph_demo` / `sprite_demo` / `vrm_xr_demo`)と
`src/core/resources/` を通して、
`vulkan_plan_pins` / `vulkan_physical_fragments` / `render_strategy` /
`graph_transforms` / `subgraph_replacements` / `canonical_anchor` は
**いずれも 0 件**。触れるのは自前のテスト 3 本
(`headless_render_test.cpp` / `renderpipeline_resolve_test.cpp` /
`renderstrategyregistry_test.cpp`)のみ。

**この窓は、どれか 1 つのプロジェクトが pin package を書いた瞬間に閉じる。**

#### 実装範囲

1. compose の時点でノードとリソースに識別子を割り当て、
   改名・variant 展開・2 つの独立パースを跨いで生存させること。
   名前は**表示用として残す**こと(利用者は名前で認識している)。
2. 識別子を `get_frame_plan` に載せること。**新しい rpc を作らないこと。**
3. WP301 の来歴 join を、名前ではなくこの識別子で行うこと。
4. fingerprint が変わることを受け入れ、影響するテスト 3 本を更新すること。
   **黙って fingerprint を素通りさせないこと** —— 変わったことが検出されること。

#### 受け入れ条件(§4 規約 10)

- **xr variant で改名されたノードが、flat variant の同じノードと
  同一の識別子を持つこと。** これが本 WP の中心的な対照である
- feature を 1 つ挿入した前後で、既存ノードの識別子が変わらないこと
  (`declaration_index` はずれる。**識別子はずれないこと**)
- 既存 golden が 1 枚も動かないこと。**識別子は診断であって描画に影響しないこと**
- 新しい rpc を追加していないこと
- `ctest` 全数が緑(`-j4`)、両ビルド階層でビルドが通ること(§4 規約 9)、
  `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

#### 範囲外

編集・書き戻し・グラフ受け取り rpc。本 WP は識別子のみ。

依存: なし(WP301 は本 WP の結論に合わせること)。見積: 中〜大。

### WP305: studio が受け取っている情報の取りこぼしを塞ぐ

**目的**: 差分ビューが必要とする情報は既に wire に載っているが、
studio のモデルが**捨てている**。塞ぐ。

#### 現状

`buildFramePlanModel`(`src/devstudio/model/frameplanmodel.cpp:540-690`)は
`physical_target_plan` の `decisions` / `planning_opportunities` /
`lowering_graph`(の一部) / `attachments` / `resources` / `backend_selection` を
既にパースしている。

**捨てているもの**: `alias_groups`(`src/devstudio` 全体で `alias` は 0 ヒット)、
`scopes`、`resolution_plan`、`lowering_graph.nodes`、
`execution_plan.dependencies`、`gpu_resource_arena`。
さらに相対 extent を落とすため、**22 個の実ターゲットのうち 21 個がサイズ未表示**になる。

#### 実装範囲

1. 上記を取り込むこと。**allowlist を広げるのであって、新しい rpc を作らないこと。**
2. 相対 extent(`kind: output_relative`, `scale_x`, `scale_y`)を保持し、
   出力解像度から実サイズを出せるようにすること。
3. `RenderTargetDefinition::alias_group` は現在 JSON 出力を持たない。
   plan 全体の `alias_groups` 配列から**ターゲット→所属グループ**を引けるようにすること
   (studio 側の導出でよい。エンジンに新しい出力を足さないこと)。

#### 「無い」と「壊れている」と「0 件」を区別すること

現在のモデルは**黙って捨てる**:未知の execution ノードを `continue` し
(`src/devstudio/model/frameplanmodel.cpp:475`)、未知の attachment ノードも捨て(`:585`)、
physical plan の schema / version / graph の一致を検査しない(`:540`)。
producer 側でも physical plan は**条件付き出力**である
(`src/core/vkcore/renderer.cpp:3403`)。

**このままだと、physical plan がそもそも無いときに
「候補 0・エイリアス 0」と表示する実装が受け入れ条件を通ってしまう。**

同じ wire を読む ImGui viewer も不正形式を空モデルへ落とす
(`src/core/imgui/compiledplanviewer.cpp:210`)。
**studio だけ三つ目の既定値流儀を増やさないこと** ——
WP310 と合わせ、`pelican_project` に置ける共通の DTO / validator か、
共通の適合コーパスを仕様化すること。

#### 受け入れ条件(§4 規約 10)

- `test/fixtures/devstudio/example_frame_plan.json` を入力に、
  **22 ターゲット全部がサイズを持つこと**(現状 1/22)。
  出力 extent の正本を規定すること(`display` = 160×90 を正本とすれば
  切り捨て規則を含め全 22 件を復元できる)
- 採用されたエイリアス組と、不採用だった候補組の**両方**が読めること
- **モデルの状態が最低でも `available(データ)` と `unavailable(理由)` に分かれること。**
  physical plan の欠落を空の計画として扱わないこと。**これが本 WP の中心的な対照である**
  —— 「エイリアス 0 件の構成」と「physical plan が無い構成」が
  同じ表示になる実装は不合格
- schema / version / graph / fingerprint の不整合、重複、参照先の欠落、
  alias 所属の矛盾を**名前付きエラー**で弾くこと。黙って `continue` しないこと
- 落としている項目が他に無いことを、fixture のキー集合との突き合わせで示すこと
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

依存: なし(WP304 と並行可。識別子が入ったら join をそちらへ寄せること)。見積: 中。

### WP306: 論理層をノードと辺で描く

**目的**: 実行順に並べた表ではなく、**依存構造**を見せる。

#### 現状

studio に描画の基盤が無い。`QGraphicsScene` / `QGraphicsView` / `QGraphicsItem` /
`paintEvent` / ドラッグ&ドロップ / QtSvg / QtCharts のいずれも `src/devstudio` に存在しない。
**まっさらである。**

現在の表示は実行順の線形リストで、**トポロジカルソートが消した辺が見えない**。
`projects/example` では消えている辺が 5 本ある
(bloom のスキップ接続 3 本、SSAO のダイヤモンド、`lit_color` の 17 ノード跨ぎ)。

#### 実装範囲

1. ノードと辺のキャンバス。論理層(30 ノード)を描くこと。
2. WP300 の来歴で塗り分けること(project / feature / engine)。
3. 頻出構造を畳めること。`projects/example` では bloom 連鎖が
   30 ノード中 13 を占め、畳むと主鎖は 9 ノードになる。
   **畳む粒度は利用者が変えられること。**
4. anchor(reads/writes を持たない挿入点。example では 8 個)を、
   通常ノードと区別して扱うこと。
5. **編集はまだ入れない。ただし選択とノード同一性は WP304 の識別子で持つこと** ——
   名前文字列で作ると編集を足すときに作り直しになる。

#### purge の対照に `projects/example` を使わないこと

**`projects/example` の feature は `engine://features/ui.json` の 1 つだけである。**
bloom の 13 ノードは feature ではなく、プロジェクトに直接書かれた
render target と pass である(`projects/example/passes/main_rendering_config.json`)。
example で「feature を 1 つ切る」と消えるのは UI パスだけで、
構造的に意味のある対照にならない。

**purge の対照は `projects/animgraph_demo` で取ること** ——
`hybrid_v1` preset に `shadow_directional` / `sky` などを重ねている。
参考: `sprite_demo` は sprite feature のみ、`vrm_xr_demo` は `"features": []`。

#### 受け入れ条件(§4 規約 10)

- 実行順では現れない辺が現れること。
  `projects/example` で 5 本(スキップ 3・SSAO 分岐・`lit_color` 直行)
- **`animgraph_demo` の feature を 1 つ切った構成で、
  消えるノードと辺を事前に完全列挙し、そのとおりに消えること。**
  これが本 WP の中心的な対照である
- **同一の widget を feature 有効 → 無効へ更新したとき、孤児が 0 であること** ——
  ノード・辺・リソース重畳・選択状態・畳み状態のいずれも残らないこと。
  新しい widget を作り直して描くのは対照にならない
- **畳みを個数で固定すること。** 展開時 30 ノード。
  畳み時は bloom 構成員 13 が 1 個のグループ項目になり、可視項目は 18。
  境界を跨ぐ依存が欠落なく束ねられること。
  **展開すると元のノードと辺の記録が完全に復元されること**
- **決定性: 入力配列の順序を入れ替えても、座標・束ね順・色が一致すること**
- **「読める」を機械判定可能にすること** —— ラベルの外接矩形が重ならないこと、
  最小間隔を満たすこと。目視の主張を受け入れ条件にしないこと
- **本番の配線を通すこと。** model や layout の helper を直接呼ぶのではなく、
  `FramePlanWidget::receiveResult` → `populate`(`src/devstudio/view/frameplanwidget.cpp:312`)
  を通して scene を検査すること
- **`SKIP_DEVSTUDIO=ON` での緑は本 WP の証明に数えないこと**(studio を丸ごとビルドしない、
  `CMakeLists.txt:485`)。studio 有効構成で機能テストを行うこと
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

#### 範囲外

編集・ノード位置の永続化・自動レイアウトの作り込み。物理層の差分(WP307)。

依存: **WP304**(識別子)、**WP305**(取りこぼし)。見積: 大。

### WP307: 何が統合され何が分けられたかを差分として重ねる

**目的**: 本題。論理層に対して**エンジンが下した物理的判断**を重ねて見せる。

#### 見せるべきもの(すべて既に wire にある)

- **統合されたターゲット**: `alias_groups`。`example` では 1 組
- **合法だったが不採用の候補**: `planning_opportunities.alias_candidates`。
  `example` では 2 組中 1 組が不採用
- **各リソースの理由**: `reason`(`example` では 22 個中 16 個が
  「arbitrary read requires a materialized resource」)、
  `widest_read`(`same_pixel` は 1 個のみ)、`aliasable`、`representation`
- **生存区間**: `lifetime{first_use, last_use}`。エンジンが算出済み
- **並列/融合の余地**: `parallel_candidates` / `fusion_candidates`
  (`example` ではいずれも 0)

#### エイリアスは書かれたグラフの性質ではない

**同じ論理グラフでも、planning profile と実機の能力で結果が変わる。**
`conservative_debug` は候補 0、`hazard_stress` は半分を捨て、
`optimized`(既定)だけが統合する。scope fusion はさらに実機の
`VK_KHR_dynamic_rendering_local_read` を要する。

**したがって表示は、どの profile とどの endpoint の結果かを必ず併記すること。**
併記しない差分は、機械が変われば嘘になる。

#### 実装範囲

1. 論理層の図(WP306)に、統合・分離・不採用候補を重ねること。
2. リソースを選ぶと、そのリソースの `reason` / `lifetime` / `aliasable` /
   `representation` が読めること。
3. profile と endpoint を明示すること。
4. 不採用の候補を、採用と区別して見せること。
   **なぜ不採用だったかの理由は現在記録されていない**
   (`deriveLegalAliasCandidates` は `continue` で捨て何も残さない)。
   **理由が無いことを、あるかのように見せないこと。**

#### 受け入れ条件(§4 規約 10)

- `projects/example` で、統合 1 組・不採用候補 1 組が区別して見えること
- **列挙した表示項目を実値で照合すること。**
  alias と profile だけを描く実装が通らないようにすること:
  - 22 リソース**全件**の `reason` / `widest_read` / `aliasable` /
    `representation` / `lifetime` を実値と照合する
  - `reason` が「arbitrary read requires a materialized resource」なのは **16 件**
  - `widest_read` が `same_pixel` なのは **1 件**のみ
- **`parallel_candidates` / `fusion_candidates` が非ゼロになる実 producer 構成を
  用意し、同じテストの中でゼロの構成と対比すること。**
  `example` は両方 0 なので、**「未実装」と「正しく 0 表示」が区別できない**。
  非空は既存データで作れる —— `alpha` / `beta` は optimized で両方に入り、
  `conservative_debug` で両方空になる(`test/targetplanning_test.cpp:372`)
- **profile の対照は fixture の手編集ではなく、
  実際に profile を変えて compile した結果で取ること。**
  手編集 fixture は不正形式入力のテストに限ること
  (`test/fixtures/devstudio/README.md:3` は手編集を明記しているが、
  それを本番配線の対照に使わないこと)
- 理由が記録されていない項目を、推測で埋めないこと
- **variant と構成を閉じること**: example flat の feature 有効 / 無効、
  bloom を持たないプロジェクト、preview、XR ON、`PELICAN_WITH_OPENXR=OFF` の
  unavailable、`PELICAN_RUNTIME_SHADER_COMPILER` の ON / OFF、
  `PELICAN_WITH_IMGUI` の ON / OFF
- **`SKIP_DEVSTUDIO=ON` での緑は本 WP の証明に数えないこと**
- 既存 golden が 1 枚も動かないこと
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

依存: **WP306**。見積: 大。

### 編集機能に向けて未着手のもの(WP308 以降・設計未確定)

利用者は編集機能を入れる意思を明示している。可視化が乗った後に着手する。
現時点で判明している**切れている 3 箇所**を記録する。

1. **グラフを受け取る rpc が無い。** `get_frame_plan` は純粋な getter で、
   送ったパラメータを無視する。`set_*` 系は存在しない。
2. **論理グラフのシリアライザが無い。** `composeRenderFeatureConfig` は一方通行で、
   どの IR からも編集可能な設定を書き戻せない。既存の直列化はすべて dump 専用。
   物理層には往復路がある(`ejectable_physical_fragment` →
   `vulkan_physical_fragments`)が、論理層には対応物が無い。
3. **リロードが恒久的に無効。** `ReloadGate::configureFromLaunch`
   (`src/core/watch/reloadgate.cpp:12`)が `rpc_ = config.rpc` とし、
   `updateLocked`(`:27`)が `enabled = requested && !replay && !strict && !rpc`。
   studio は常に `--rpc` で player を起動する
   (`src/devstudio/viewport/studioplayerarguments.cpp:52`)ので、
   **studio 配下の player はファイルを書いても拾わない**(reason: "rpc driver")。

**有力な回避路**: `compileVulkanTargetPlan`(`src/project/targetrenderplanning.hpp:498`)と
`makeTargetLoweringGraph`(`:155`)は `pelican_project` にあり、
**studio が必ずリンクする側**である。studio がローカルで候補をコンパイルすれば、
実機に反映せずに差分を出せる。**D0 に触れない。**
ただし論理 IR 側(`frameplanner` / `vulkanrendercompilerprogram`)は
`pelican_core` にあり D0 の向こうなので、そのままでは届かない。
**ローカル compile と compile 用 rpc のどちらが安いかは未計測。**

## WP301〜303 の敵対レビューで出た追補(WP308〜WP311)

2026-08-16、マージ済みの `2a86029` / `a22132b` / `d7bd6a5` に対して
**別モデル(codex)による敵対レビュー**を掛けた。3 本すべてで実害のある指摘が出た。

**共通する見落としの方向**: Claude 側の検証は「テストが噛むか」を**一箇所でしか確かめていなかった**。
WP302 は reader を検査したが `draw()` を呼んでいない。
WP301 はヘルパを無効化して落ちることは確かめたが、**本番の呼び出しを消す実験をしていない**。
以後、否定対照の確認は**ヘルパの中身と呼び出し側の両方**で行うこと。

### WP308: OpenXR 無効ビルドが腐っている

**目的**: `-DPELICAN_WITH_OPENXR=OFF` でテストが落ちる状態を直し、
**その構成が二度と黙って腐らないようにする。**

#### 実測(2026-08-16、`C:/pb/noxr` で完全ビルド後に非 GPU 全数)

```
99% tests passed, 5 tests failed out of 953
  321 - view family projection jitter is one shared modifier sample
  334 - render view family collection separates main cardinality from secondary families
  350 - render view family validates stable identities and graph cardinality
  356 - planar reflection views preserve identity, clipping, and raster winding
  847 - WP301 xr compute provenance survives variant namespacing
```

**5 件のうち 4 件は今日より前から落ちていた。**
321 / 334 / 350 / 356 はすべて `test/viewfamily_test.cpp` にあり、
このファイルは WP301〜303 が**触っていない**
(`git diff --name-only 65ea95c..HEAD` に現れない)。
そして `grep -c PELICAN_WITH_OPENXR test/viewfamily_test.cpp` は **0**。

**つまり OpenXR OFF 構成は誰もビルドしておらず、腐っていた。**
WP301 は既存の同型欠陥に 5 件目を足したにすぎない。

#### なぜ規約 9 で捕まらなかったか

規約 9 は「`#if` の中の識別子を触る WP は OFF 構成もビルドする」だが、
本文が `PELICAN_RUNTIME_SHADER_COMPILER` を名指ししているため、
**その 1 つだけを確認して済ませる運用**になっていた。
WP301〜303 の統合時に確認したのも `PELICAN_RUNTIME_SHADER_COMPILER=OFF` だけである
(そちらはビルド成功)。

`PELICAN_WITH_OPENXR` は既定 ON のオプション(`CMakeLists.txt:53`)。
OFF では XR 経路が名前付きで throw する
(`src/project/graphvariantpolicy.cpp` の `#if PELICAN_WITH_OPENXR` 分岐)。
テスト対象は OpenXR の設定に関わらず登録される。

**正しい作法は既に存在する** —— `test/graphvariantpolicy_test.cpp:86` は
`#if PELICAN_WITH_OPENXR` で囲み、OFF 側で unavailable エラーを検査している。
揃っていないだけである。

#### 実装範囲

1. 落ちている 5 件すべてを直すこと。XR の肯定側を `#if PELICAN_WITH_OPENXR` で囲み、
   **OFF 側では名前付きの unavailable エラーを検査すること。**
   囲って消すだけにしないこと。`graphvariantpolicy_test.cpp:86` と同じ作法にすること。
2. 既知のガード漏れ箇所は次の 4 つである(仕様レビューで特定済み)。
   `test/viewfamily_test.cpp` の `:147-154` / `:182-189` / `:590-594` / `:854-858` が
   ガード無しで XR policy を解決する。`viewfamily_test` は常時登録される
   (`test/CMakeLists.txt:834`)。
   これに加えて他に無いか棚卸しすること。
   **`#if PELICAN_WITH_OPENXR` はテストから使える** ——
   マクロは `pelican_core` から PUBLIC に伝播する(`src/core/CMakeLists.txt:7-15`)。
3. **`PELICAN_WITH_OPENXR=OFF` を検証セットに入れること。**
   規約 9 の本文を「名指しされた 1 つ」ではなく
   「**触れた `#if` に対応する全てのフラグ**」と読めるように直すこと。
   最低限、OFF を回すべきフラグの一覧を §0 に置くこと。

#### 受け入れ条件(§4 規約 10)

- `-DPELICAN_WITH_OPENXR=OFF` で `ctest` 全数が緑になること(現状 5 件失敗)
- **OFF 構成で「XR が使えない」ことが名前付きエラーとして検査されていること** ——
  これが対照である。`#if` で囲って消すだけでは、OFF 側は何も主張していない
- ON 構成の結果が変わらないこと(現状 1192/1192 緑)
- 棚卸しの結果を報告すること。「他に無い」なら**何を調べたか**を述べること

#### 範囲外

`viewfamily_test.cpp` の 4 件が**なぜ**落ちるかの根本原因が
ガード漏れ以外にある場合、それは別 WP とすること。本 WP はガードの作法を揃える。

依存: なし。見積: 小〜中。**最優先** —— 出荷オプションの片側がビルドできない。

### WP309: 来歴同期の順序ハザードと、本番配線の未検査

**目的**: WP301 の残件 2 つ。

#### ① 本番の呼び出しを消してもテストが通る

`detail::namespaceComputeTasks` の**呼び出し**
(`src/core/renderingpass/vulkanrendercompilerprogram.cpp:506`)を削除しても、
追加テストは通る。テストがヘルパを直接叩くためである
(`test/featurecompose_test.cpp:475`)。

ヘルパを `detail::` に出した理由(GPU なしで検査したい)が、
そのまま**本番経路を検査しない原因**になっている。

#### ② 改名順序による重複名の例外

compute task `probe` があり、著作側に `probe#xr` という名前のパスがあって
subgraph 置換で消える場合、stale な `probe#xr` が残ったまま `probe` が改名され、
`synchronizeRenderPipelineProvenance`(`src/project/renderpipeline.cpp:1783`)が
重複名で throw する。

親コミットは subgraph 解決直後に同期して stale を先に掃除していた。
**同梱設定では発火しない**ことは実査で確認済み(4 プロジェクトの active config は
`compute_tasks` を持たず、`src/core/resources/features` の 18 JSON と
`hybrid_v1.json` に `#xr` を含む著作名は無い。
`src/core/resources` 全体は再帰で 32 JSON あり、残りは調べていない)。

#### 実装範囲

1. **コンパイラ段の本番経路を通るテストを足すこと。**
   `runRenderCompilerProgram` に `RenderCompilerProgramArtifact::runtime_package` を要求し、
   `compileDefaultVulkanVariant` を通すこと。既存の WP301 テストと同様、
   既定構築の `VulkanRenderCompilerBackendContext` で足りる。
2. 改名前に一度同期して stale を落とすか、改名後に一意性を明示検証すること。
3. **XR を解決する箇所は `#if PELICAN_WITH_OPENXR` で囲み、
   OFF 側で名前付き unavailable エラーを検査すること。**
   囲わなければ WP308 が直した欠陥をそのまま再導入する。

#### 実装範囲外(誤解を避けるため明記する)

- **full registration を要求しない。**
  `registerRenderingPassConfigVariantsData` は実 physical device から backend を構成し、
  sample-count planning も device の feature / property を照会する
  (`src/core/renderingpass/renderingpassconfigregistration.cpp:775-800`、
  `src/core/renderingpass/renderingsamplecount.cpp:2161-2177`)。
  GPU fixture が要るので本 WP の範囲ではない。
- **`test/featurecompose_test.cpp:94` は本番 loader ではない。**
  `engine://` を剥がして埋め込み資源を返すテスト用 callback である
  (本番は `PathResolver::loadText`、`src/core/loader/pathresolver.cpp:92-97`)。
  本 WP は実 loader を要求しない。
- **preview を本番配線の否定対照に使わないこと。**
  `data_only` は本番呼び出しより手前で return する
  (`vulkanrendercompilerprogram.cpp:462-474`)ため、
  呼び出し削除の変異を検出できない。preview は
  `physical_package == nullptr` などの data-only 契約の独立した回帰条件として扱うこと。

#### 受け入れ条件(§4 規約 10)

- **`vulkanrendercompilerprogram.cpp:506` の呼び出しを削除すると落ちること。**
  これが本 WP の中心的な対照である。
  **ノード名または来歴の具体値が変わることで落ちること** ——
  例外の有無だけで判定しないこと
- **同じ TEST_CASE の中で feature を有効・無効の両方で実行すること。**
  - 有効時: 最終的に `(main#xr, probe#xr)` の来歴が**ちょうど 1 件**存在し、
    fixture で指定した `source` / `provider_feature` / `provider_ref` と**完全一致**すること
  - 無効時: 著作側のパスが残り、同じ graph/name を含む**明示的な衝突エラー**になること
- 著作名 `<compute task 名>#xr` を持つ設定が、重複名 throw で死なないこと。
  **かつ、来歴が残っていること** —— stale と compute の来歴を両方捨てる実装は不合格である
- flat の来歴が、**実際に解決された name / source / provider の値で完全一致**すること。
  「変わらないこと」だけでは期待値が定義されない
- `-DPELICAN_WITH_OPENXR=OFF` で `ctest` 全数が緑であること

依存: **WP308**(ガードの作法が確定してから着手すること)。見積: 中。

### WP310: planning opportunities の「不明」を「不採用」と断定しない

**目的**: WP302 の残件。**正当な入力で嘘を表示する**経路がある。

#### ① parallel 候補を常に「不採用」と表示する

producer は互いに到達不能なノード対を parallel 候補として出す
(`src/project/targetplanning.cpp:897`、既存テスト `test/targetplanning_test.cpp:379` が
`containsPair(optimized.parallel_candidates, "alpha", "beta")` を検査)。

reader は「物理の parallel collection が存在しない」とコメントで認めながら
全候補に `false` を置き、UI はそれを `[not adopted]` と表示する
(`src/core/imgui/compiledplanviewer.cpp:232`, `:506`)。

**正直だったのはコメントで、表示は断定していた。**
`projects/example` は独立ノードが 0 なので出ないが、
独立な作業があるグラフでは嘘になる。

#### ② 採用判定が異常データを「不採用」に偽装する

`alias_groups` が壊れていても例外にせず `false` を返す
(`compiledplanviewer.cpp:71`)。
**「一致しなかった」と「証拠を解釈できなかった」が 2 値に潰れている。**

#### ③ 不正入力で無音消失する

キー欠落・型不一致で空を返し、節ごと消える(`compiledplanviewer.cpp:210`, `:486`)。
**今回直した欠陥と同じ型である。**
しかも同じ payload を読む studio 側(`src/devstudio/model/frameplanmodel.cpp:547`)は
非オブジェクトを**名前付き例外**で弾いており、**エラー方針が揃っていない**。

#### 実装範囲

1. 採用状態を 3 値にすること(`adopted` / `not_adopted` / `unknown`)。
   parallel は `unknown` とし、UI は断定しない表示にすること。
   **物理の schedule が公開されたときだけ採否を結合すること。**
2. collection の解釈失敗を `no-match` と区別し、
   現 schema で必須の collection が壊れていれば**名前付きエラー**にすること。
3. studio 側とエラー方針を揃えること。**第二の流儀を作らないこと。**

#### 受け入れ条件(§4 規約 10)

- **独立ノードを持つグラフで、parallel 候補が「不採用」と断定されないこと。**
  これが本 WP の中心的な対照である
- 必須 collection を壊した入力が、名前付きエラーで失敗すること(無音で消えないこと)
- fusion / parallel が**非空**のケースを検査すること
  —— 現行テストは fixture が両方空のため、両 reader を未実装にしても通る
- UI の表示を検査すること —— 現行テストは `draw()` を呼ばないため、
  表示ブロックを丸ごと削除しても通る
- 対照は fixture の手編集ではなく、**実際に profile を変えて compile した結果**で取ること。
  非空の対照は既存データで作れる —— `alpha` / `beta` は optimized で fusion と parallel の
  両方に入り、`conservative_debug` では両方空になる(`test/targetplanning_test.cpp:372`)
- **`PELICAN_WITH_IMGUI=OFF` でビルドが通ること。**
  `compiledplanviewer_test` は `PELICAN_WITH_IMGUI=ON` のときしか登録されず
  (`test/CMakeLists.txt:880`)、配布構成は IMGUI を強制 OFF にする
  (`src/devcli/distconfig.cpp:882`)。
  **動作テストは IMGUI ON で、ビルドゲートは OFF で**取ること
- `SKIP_DEVSTUDIO=ON` でビルドが通ること(studio は丸ごと外れる、`CMakeLists.txt:485`)

依存: なし。見積: 中〜大。

### WP311: pass フィールド所有権の検証を、全パーサ・全経路へ

**目的**: WP303 は 2 つあるパーサの**片方しか塞いでいない**。

#### 現状:未束縛 read エッジの経路は開いたままである

`src/core/renderingpass/frameplanner.cpp:773-782` は
`parseGpuDrawSourceFromJson` を**マテリアル判定なしで**呼び、
`draw_source->commands` と `count` を `node.reads` に足す。

WP303 が塞いだのは pass definition 側(`renderingpassvalidation.cpp`)だけで、
**frame graph 側は塞いでいない**。同じ JSON を 2 つの独立したパーサが読む構造のため、
片方だけでは足りない。

さらに:

- **pseudo-pass が検証を素通りする。** `canonical_anchor` と `snapshot_copy` は
  `frameplanner.cpp:744` で早期 return する
- **preview / data_only 経路が両フィールドを検証しない。**
  `vulkanrendercompilerprogram.cpp:462` で GPU 側の全体検証(同 `:491`)より先に return する
- frame graph 側は「Only material frame graph passes support material_variant」という
  **独自の同種検証を持っている**。作法が二重化している

#### 実装範囲

1. `type` とフィールド所有権を検証する**共通関数を一つ**作り、
   pseudo-pass の分岐より前、`data_only` の分岐より前に置くこと。
   D0 を保てるなら `pelican_project` 層に置くこと。
2. 既存の二重化した検証を、その共通関数に寄せること。**第二の流儀を残さないこと。**

#### 所有権の表を仕様の正とすること

現在の `validatePassSpecificFields` は material 系だけで 10 項目を見ており、
さらに fullscreen / raster / shader / push-constant 系の検査もある
(`src/core/renderingpass/renderingpassvalidation.cpp:472` 以降)。
`gpu_draw_source` と `material_contract` の 2 つだけを直しても二重化は消えない。

**pass type と、その type が所有する field の完全な表を本節に列挙し、
それを唯一の source of truth とすること。**
有効な type の集合は `PELICAN_WITH_IMGUI` で変わる
(`src/core/renderingpass/renderingpassjsonhelpers.cpp:127`)。
また `pelican_project` は現在 `PELICAN_WITH_OPENXR` しか compile definition を持たない
(`src/project/CMakeLists.txt:42`)ので、
共通 validator をそこへ置くならビルド能力を**入力として明示的に渡すこと**。

#### 受け入れ条件(§4 規約 10)

- **非マテリアルの frame graph pass に `gpu_draw_source` を置いたとき、
  名前付きエラーで失敗すること。** これが本 WP の中心的な対照である
  —— 観測は「名前付き例外が出ること」「provider が呼ばれていないこと」
  「generation が公開されていないこと」で行う。
  **「read エッジが増えないこと」を条件にしないこと** ——
  `parseFrameGraphDefinitionFromJson` は値を返すので、
  例外時に検査できる `FrameGraphDefinition` は存在しない(`frameplanner.hpp:163`)
- `canonical_anchor` / `snapshot_copy` / preview の各経路でも同じく弾かれること。
  standalone preview は `resolveRenderPipeline` と `compileRenderPipeline` しか呼ばない
  (`src/core/renderingpass/previewgraph.cpp:90`)ので、別経路として明示的に通すこと
- **肯定側は同じ production compile の中で、material ノードの `reads` に
  正確な commands / count 名が入り、最終 ID が対応する `buffer_bindings` と
  一致することを検査すること**
- 表に挙げた各 field について、所有する type の肯定例と
  所有しない type の否定例を**同一の parameterized test** で実行すること。
  `type` の欠落・非文字列・未知の値も含めること
- **出荷コーパスは肯定対照にならない。** `gpu_draw_source` は出荷 JSON に **0 件**である
  (`git grep -l gpu_draw_source -- projects/* src/core/resources/*` は空)。
  肯定側には専用の production fixture を用意すること
- 出荷設定が従来どおり読めることは、**固定件数ではなく**
  4 プロジェクトの production resolve / startup と、
  built-in の pipeline / feature を動的に列挙して確認すること
  (tracked JSON は projects/ 48・src/core/resources/ 30 だが、この数は増減する。
  `material_contract` は 10 件で全て `type == material` 上)
- `PELICAN_WITH_IMGUI` の ON / OFF、`PELICAN_RUNTIME_SHADER_COMPILER` の ON / OFF で
  成立すること

依存: なし。見積: 中〜大。

### WP312: 機能フラグの検証セットを、宣言から生成されるものにする

**目的**: WP308 が §0 に置いたフラグ一覧は**手書きの写しであり、既に破綻している**。
宣言を正とし、文書とスモーク行列をそこから検査または生成する。

#### WP308 は自分が書いた規約に違反した

WP308 が `#if PELICAN_WITH_OPENXR` を入れた `test/viewfamily_test.cpp:608` は、
`#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS`(`:438`)の**内側**にある。
新規約「触れた `#if` に対応する全フラグを反転」に従えば
`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS` も反転すべきだったが、していない。
**規約を書いた WP がその場で守れなかった。**手書き運用では守れない証拠である。

#### スモークが単独反転になっていない

- 共有ヘルパ `configure_and_build` が**全構成で** `PELICAN_WITH_SPIRV_LINK=OFF` を強制する
  (`test/run_build_units_smoke.cmake:63`)
- Standard Render Algorithms の OFF スモークは**さらに** `PELICAN_WITH_OPENXR=OFF` も渡す
  (`test/run_build_units_smoke.cmake:559`)

したがって正当な構成である
`STANDARD_RENDER_ALGORITHMS=OFF` × `OPENXR=ON` × `SPIRV_LINK=ON` は
**一度も建っていない**。OpenXR 側のコードが purge された standard algorithm の
シンボルを参照する事故は、現行スモークの OpenXR-OFF と Standard-OFF を**両方通過できる**。
規約が防ごうとしている事故と同型である。

#### 規約 9 の本文が CMake gate を対象にしていない

§0 は `#if` と CMake `if()` の両方を対象と書いたが、規約 9 の本体は `#if` しか述べていない。
実在する `if(NOT SKIP_DEVSTUDIO)`(`CMakeLists.txt:486`)がその隙間に落ちる。
**`SKIP_DEVSTUDIO` は 14 フラグの一覧にも入っていない。**

#### 「一つずつ反転」が成立しないフラグがある

`PELICAN_WITH_JOLT_PHYSICS` を ON にすると、CMake が
`PELICAN_WITH_BUILTIN_PHYSICS=OFF` を強制する(`CMakeLists.txt:72-76`)。
Jolt の対照は**必ず 2 フラグ動く**。規約の「一つずつ」と一致しない。

さらに Jolt の既定 OFF が CMake と台帳 §0 の**二箇所**に書かれた。
**WP308 の変更自身が「既定値の所在は一箇所」に違反している。**

#### 実装範囲

1. **機械可読な feature registry を正とすること。**
   名前・基準値・対照値・依存関係(強制される他フラグ)を宣言し、
   台帳の一覧とスモーク行列をそこから検査または生成すること。
   **手書きの写しを残さないこと。**
2. スモークを単独反転にすること。共有ヘルパから `SPIRV_LINK=OFF` の強制を外し、
   Standard OFF から `OPENXR=OFF` を外す。SPIR-V linker の OFF は専用行にする。
3. 規約 9 の本文を「`#if`、CMake `if()`、generator expression、
   条件付き source / target / dependency のいずれかに属する変更」と明記すること。
   `SKIP_DEVSTUDIO` を基準 OFF・対照 ON として一覧に含めること。
4. Jolt のように対照が複数フラグを動かすものは、registry に依存関係として宣言し、
   規約の「一つずつ」の例外であることが**文書ではなく宣言から**分かるようにすること。

#### 受け入れ条件(§4 規約 10)

- **`STANDARD_RENDER_ALGORITHMS=OFF` × `OPENXR=ON` × `SPIRV_LINK=ON` が建ち、
  テストが緑であること。** 現在このマスは一度も建っていない
- **台帳の一覧を故意に 1 つ削る / 1 つ増やす / 既定値を書き換えると、
  検査が名前付きエラーで落ちること。** これが本 WP の中心的な対照である
  —— 一覧が宣言と食い違ったまま緑になる状態を残さないこと
- スモークの各行が、対象フラグ以外を共通 ON 構成から動かしていないこと。
  動かすものは registry の依存関係として宣言されていること
- Jolt の既定値が 1 箇所にしか書かれていないこと
- `ctest` 全数が緑(`-j4`)、`git diff --check` クリーン、
  `uv run tools/doclink.py check` が通ること

#### 範囲外

registry の全フラグ × 全組合せの網羅。**単独反転が成立することと、
一覧が宣言から検査されること**までが本 WP である。

依存: なし。見積: 中。

### WP313: WP309 の変異対照が片方の構成でしか噛まない

**目的**: WP309 の中心的対照を、OpenXR ON / OFF の両方で成立させる。
あわせて WP309 が作った既定値の二重定義を潰す。

#### ① 対照が OpenXR ON でしか噛まない

WP309 の変異対照は「`vulkanrendercompilerprogram.cpp` の
`detail::namespaceComputeTasks` 呼び出しを消すとテストが落ちる」である。
これは `probe#xr` の具体値検査で成立しているが、**OpenXR ON でしか成立しない**。

- OFF では XR 分岐が namespacing より前に
  `"XR graph variant is unavailable in this build"` で落ちる
  (`test/featurecompose_test.cpp:1002`、`src/project/graphvariantpolicy.cpp:498`)
- flat の gate 無効側は compute 名が `probe` のままなので、呼び出しを消しても通る

**WP301 と同じ「一箇所でしか噛まない」形の再発である。**

**直し方**: flat の gate 有効側で、実際に解決された経路の `source == engine` と
来歴の件数を検査し、stale な `probe#xr` 来歴が**存在しない**ことも主張する。
gate 無効側では著作パス `probe#xr` の `source == project` を対照に置く。
これで ON / OFF 双方で呼び出し削除が落ちる。

#### ② `device_required` の既定値が二箇所にある

同じ既定値が constructor の省略引数(`vulkanrendercompilerpackage.hpp:39`)と
member initializer(`:58`)の**両方**に書かれている。
本番登録は第 6 引数を省略するので constructor 側に依存し
(`src/core/renderingpass/renderingpassconfigregistration.cpp:779`)、
追加テストは構築後に上書きするので**両者のドリフトを検出しない**
(`test/featurecompose_test.cpp:802`)。

**「既定値の所在は一箇所」違反であり、
WP309 の差し戻し指示(明示宣言にせよ)が作り込んだものである。**

**直し方**: member initializer の既定値を削除し、constructor 引数を唯一の正本にする。

#### 受け入れ条件(§4 規約 10)

- **`PELICAN_WITH_OPENXR` の ON と OFF の双方で、
  `namespaceComputeTasks` の本番呼び出しを削除するとテストが落ちること。**
  これが本 WP の中心的な対照である。落ちたときのメッセージを両構成について報告すること
- 既定値が 1 箇所にしか書かれていないこと。
  **constructor 引数と member initializer が食い違ったときに検出できること**
- 本番の挙動が変わらないこと(既定は `device_required`、本番は device 経路)
- `ctest` 全数が緑(`-j4`)、§0 の反転対象のうち触れたフラグを反転して緑、
  `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 小。

### WP314: graph transform を跨ぐと feature 来歴が無音で engine に落ちる(潜在)

**目的**: 記録する。**現時点では出荷設定から到達しない。**

#### 経路

1. feature `F` が compute task `work#xr` を宣言する
2. XR variant の graph transform が、その task の名前だけを `work` に変更する
3. namespacing が `work` を `work#xr` に戻す

来歴はこう壊れる:

```
work#xr (feature:F)
  → transform 後の task は work
  → 一回目の同期が旧 work#xr を stale として削除し work (engine) を生成
  → namespacing 後は work#xr (engine)
```

この変換は現在の graph-transform 契約で**許可されている**。
構造比較は `compute_tasks` を除外し、protected node も anchor / output transform に限られる
(`src/core/renderingpass/graphtransformregistry.cpp:463` / `:530` / `:1543`)。

原因は、変換より前に作った来歴を**名前だけで**同期し、未知名を無条件に `engine` へ落とすこと
(`src/core/renderingpass/vulkanrendercompilerprogram.cpp:256`、
`src/project/renderpipeline.cpp:1797`)。
来歴には kind も安定 ID も改名 lineage も無い(`src/project/renderpipeline.hpp:150`)。

**最終フレームプランで `source: "engine"` となり `provider_feature` / `provider_ref` が消える。
例外にもならないので fail-fast 原則にも反する。**

#### なぜ今は着手しないか

`graph_transforms` / `subgraph_replacements` / `render_strategy` は
**出荷 80 JSON に 0 件**である(4 projects の 48 と `src/core/resources` の 32 を実査)。
到達経路が存在しない。

**ただし旧名と最終名だけを推測して保護する修正では不十分である** ——
それは WP309 が除去した stale `probe#xr` を再び保持してしまう。
正しい直し方はノードの安定 ID か、graph transform が返す明示的な rename / lineage map であり、
どちらも WP304 の設計検証が不合格にした領域と重なる。
**graph transform を実際に使う WP が現れたとき、その WP の前提条件として着手すること。**

依存: 着手前に識別子の設計をやり直すこと。見積: 大。

### WP315: studio の解像度解決が runtime と型で食い違う

**目的**: WP305 が入れたサイズ表示は、**一般の正当な入力に対して runtime と違う値を出す。**

#### 型が違う

| | 式 | scale の型 |
|---|---|---|
| runtime | `static_cast<float>(w) * plan.scale_x`(`src/core/vkcore/renderer.cpp:558-563`) | `float`(`src/project/targetrenderplanning.hpp:65`) |
| studio | `static_cast<double>(w) * scale_x`(`src/devstudio/model/frameplanmodel.cpp:1110`) | `double`(`src/devstudio/model/frameplanmodel.hpp:103`) |

`extent_scale: 0.7`、出力幅 10 のとき:

- runtime: `float` で乗算し積が `7.0f` に丸められてから整数化 → **7**
- studio: `double` で `6.99999988079071` まで計算して切り捨て → **6**

著作値は `float` に parse される(`src/core/renderingpass/rendertargetjsonparser.cpp:255`)。

**studio 側のコメントは「producer の切り捨て規則を写している」と書いているが、写せていない。**

#### テストがこれを検出できない

`test/devstudio_frameplan_test.cpp:378` は**同じ double 式で期待値を再計算している**ため、
実装を実装自身で検証している。runtime が実際に解決した値を見ていない。

#### 出力の正本が違う

producer の正本は `resolution_plan.output_source_resource` と実 runtime の output extent である
(`src/core/renderingpass/renderingsamplecount.cpp:1135`、`src/core/vkcore/renderer.cpp:3279`。
runtime は `resolveResolutionSourceExtent` で resource 名から解決し `"swapchain"` を特別扱いする)。
studio はそれより先に名前 `"display"` をハードコードして採用する
(`src/devstudio/model/frameplanmodel.cpp:1077`)。
出荷 fixture では一致するが、custom / non-window plan で食い違う。

#### 現在は発火しない

出荷 4 プロジェクトの `extent_scale` は 1 と 2 の冪のみ(1・0.5・0.25・0.125・0.0625)で、
いずれも 2 進で正確なため差が出ない。**WP305 の「22/22 正しい」は事実だが、
それは入力がたまたま全て 2 進正確だったからである。**

#### 実装範囲

1. **runtime と studio が同じ解決規則を共有すること。**
   `float` ベースの純粋な resolver を共有するか、
   **producer が実際に解決した extent を wire に載せること**(後者が望ましい ——
   規則を二箇所に持たない)。
2. 出力の正本を `resolution_plan.output_source_resource` から取ること。
   `"display"` のハードコードをやめること。
3. 写せていないコメントを消すか、正しくすること。

#### 受け入れ条件(§4 規約 10)

- **`extent_scale: 0.7` と出力 10×10 を含む入力で、
  studio の値が runtime の解決値と一致すること。**
  これが本 WP の中心的な対照である
- **期待値を studio と同じ式で再計算しないこと。**
  runtime 側が実際に解決した値と突き合わせること
- 出荷 4 プロジェクトの 22 ターゲットが従来どおり正しいこと(退行しないこと)
- `resolution_plan.output_source_resource` が `"display"` 以外を指す入力で正しいこと
- `ctest` 全数が緑(`-j4`)、§0 の反転対象のうち触れたフラグを反転して緑、
  `git diff --check` クリーン、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 小〜中。

### WP316: WP306 のテストが headless Qt で落ち、主張を証明していない

**目的**: WP306 が入れた 2 ケースは **`QT_QPA_PLATFORM=offscreen` で失敗する。**
あわせて、それらが主張していることを実際に証明する形に直す。

#### 実測(2026-08-17)

同じ Debug バイナリで:

| 構成 | 結果 |
|---|---|
| ネイティブ Windows QPA | 2/2 緑、9873 assertions、exit 0 |
| `QT_QPA_PLATFORM=offscreen` | **2 ケースとも失敗**、exit 42 |

失敗箇所は `requireReadableLabels()` の重なり判定
(`test/devstudio_frameplan_graph_test.cpp:311`)。
**ラベル重なり判定がフォント計測に依存しており、プラットフォームで変わる。**
`ctest` は既定でネイティブ QPA を使うため緑に見えていた。**headless では落ちる。**

#### テストが主張を証明していない箇所

**① 畳みの中身を検査していない。** 13 メンバーについて検査しているのは
「個数 13」と先頭・末尾の 2 名だけ(`:420`)。**別の 11 ノードを誤って畳んでも通る。**

**② 再展開の比較が自己スナップショットである。**
初期 scene の `bounds` / `position` / 色 / 辺レコードを撮り、
展開後の同じ自己スナップショットと比べている(`:256`)。
wire の dependency tuple とは**件数しか比較していない**。
brush / pen / path、可視性、graph / source / anchor、矢印、ラベル文字列は範囲外で、
再展開後に `requireReadableLabels()` も走らない。
**再展開時だけ矢印やラベルを失う実装、常に同じ誤ったレコードを保持する実装が通る。**

**③ 決定性の検査がレイアウトに届いていない。**
テストは nodes を reverse する(`:401`)が、
`buildFramePlanModel()` が `order` で直ちに再ソートする
(`src/devstudio/model/frameplanmodel.cpp:564`)。
barriers / resources / execution nodes は座標計算に使われない。
**座標決定性の主張は検査されていない。**
(dependency の reverse は bundle 順の検査としては噛んでいる。)

**④ flat / compiler-ON しか通していない。**
動的入力は `runtime_shader_compiler_enabled=true` と `graph_variant=flat` を固定している(`:93`)。
テストに `RUNTIME_SHADER` 条件が無いので、**compiler-OFF ビルドでも ON 相当の入力を作って緑になれる。**
xr / preview も未実行。

#### 実装範囲

1. **`QT_QPA_PLATFORM=offscreen` で緑にすること。**
   フォント計測に依存しない判定にするか、テストが使うフォントを固定すること。
   **プラットフォーム差で結論が変わる「読める」の判定を残さないこと。**
2. 畳みの全 13 メンバーを名指しで検査すること。
3. 再展開の比較を、**wire から独立に構築した期待値**と行うこと。
   path / brush / pen / 文字列 / 可視性 / role を前後で比較し、
   再展開後にもラベル判定を走らせること。
4. 決定性の検査が**レイアウト計算に実際に届く入力**で行われること。
5. flat / preview / xr と compiler ON / OFF をパラメータ化すること。
   OFF で非対応なら、期待する名前付き拒否まで検査すること。

#### 受け入れ条件(§4 規約 10)

- **`QT_QPA_PLATFORM=offscreen` と未設定の両方で緑であること。**
  これが本 WP の中心的な対照である。両方の結果を報告すること
- 畳む対象を 1 ノード取り違えた実装が落ちること
- 再展開で矢印またはラベルを失う実装が落ちること
- **ノードの入力順を変えたときに、レイアウトが実際に同じ座標を出すこと**
  —— `order` による再ソートより後の入力で検査すること
- `ctest` 全数が緑(`-j4`)、§0 の反転対象のうち触れたフラグを反転して緑、
  `uv run tools/doclink.py check` が通ること

依存: なし。見積: 中。**優先** —— headless で落ちるテストが入っている。

### WP317: execution_plan の欠落を「依存 0 件」として黙って描く

**目的**: WP305 が physical plan について直した型の欠陥が、
`execution_plan` に残っている。

#### 現状

`execution_plan` が無い / null のとき、パーサは dependency を空のまま正常終了し
(`src/devstudio/model/frameplanmodel.cpp:611`、`:750`)、
scene はその空配列をそのまま描く(`src/devstudio/view/frameplangraphics.cpp:344`)。

結果、**30 ノード・0 辺の「依存の無い正常なグラフ」に見える。**

runtime の現行 publisher は必ず execution plan を付ける
(`src/core/vkcore/renderer.cpp:3270`)ので、欠落は**契約異常**である。
それでも UI は異常だと言わない。

**WP305 は同じ形の欠陥を physical plan について
`available(データ)` / `unavailable(理由)` に分けて直した。
`execution_plan` は分けられていない。**

#### 実装範囲

1. execution plan の有無をモデルに保持し、
   論理グラフでは**名前付きの unavailable / error** にすること。
2. barrier だけの縮退表示を用意するなら、**不完全表示であることを明示すること。**
3. WP305 が `pelican_project` に置いた共通 wire DTO の作法に揃えること。
   **第三の流儀を作らないこと。**

#### 受け入れ条件(§4 規約 10)

- **`execution_plan` を除いた入力が、依存 0 件の正常なグラフとして描かれないこと。**
  これが本 WP の中心的な対照である
- 依存が**本当に 0 件**の正当な入力とは別の表示になること
- 既存の正常な入力で表示が変わらないこと
- `ctest` 全数が緑(`-j4`)、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 小。

## エディタのワークスペース状態(2026-08-19)

WP318 の「位置はセッション内のみ。永続化はこの設計に従う」が参照している節である。
**3 層あり、1 箇所で決める。1 層だけ保存される半端を作らない。**

### 3 層と、それぞれの持ち主

| 層 | 何 | 誰に付くか | 現状 |
|---|---|---|---|
| **パネル配置** | ドックの位置・浮き・タブ化・サイズ | **人**(そのエディタを使う個人の好み) | 動かせる。名前付きプリセットで保存/復元できる。**起動時に自動復元しない** |
| **ツール内配置** | ツールの内側の分割・タブ順・どれを並べて見るか | **プロジェクト**(何を見るかは対象に依る) | **ゼロ。**`FramePlanWidget` は素の `QTabWidget` 9 枚固定で `setMovable` すら呼んでいない |
| **ノード位置** | キャンバス上のノード座標 | **プロジェクト**(グラフの読み方は対象に依る) | `session_node_positions_` に持つが「never reach disk」。保存先未設計 |

**層を分ける理由**(利用者の決定): デザイナーとエンジニアで**使いやすいエディタは違う**ので、
パネル配置は人に付く。一方**ノードの整理結果はパネル配置とは別に共有したいことがある**ので、
ツール内配置とノード位置はプロジェクトに付く。

### 保存先

**プロジェクトの内部データにしない。**
プロジェクトは「web で開ける ⊆ pelican で開ける」の部分集合原則を持ち、
**エディタの都合をそこに混ぜない。**

- パネル配置: `QStandardPaths::AppConfigLocation` 配下(`LayoutPresetManager` の現在地)
- ツール内配置・ノード位置: **同じ場所に、プロジェクトを鍵として持つ**

**プロジェクトと一緒に渡すのは既定にしない。**
渡したいときは**明示的な export** で行う。取り込む側も明示的に取り込む。
**読み込み時に黙って同梱・黙って適用しない。**

### 名前付きの層は 2 つとも独立

パネル配置とツール内配置は**それぞれ独立に名前を持つ**。
「パネルは A、ツール内は B」という組み合わせが選べること。
**片方を選ぶともう片方も連動する形にしないこと。**

### 起動時の復元

**起動時に前回の配置へ戻ること。**
現在は `MainWindow` のコンストラクタが**無条件に** `applyDefaultLayout()` を呼び、
`restoreState` への到達経路は Layout メニューだけである。
**利用者が動かした配置は次回起動で必ず消える。**

復元に失敗したとき(形式が古い・ドックが増減した・ファイルが壊れている)は
**名前付きで既定レイアウトに落ちること。**黙って落ちないこと。

### ドックが増減したとき

**古いプリセットは捨てないこと。**
既存の `devstudio_layoutpreset_test.cpp` が「後からドックが増えても復元できる」ことを
検査している。**新しいドックは既定の位置に出し、既存ドックの配置は保つ。**

### この設計に従う既存 WP

- **WP318**: ノード位置。現在はセッション内のみ
- **WP329**(マージ済み): Logical graph タブを `QSplitter` にした。
  **その分割位置はツール内配置に属する**

## 編集側の設計 — 確定した条件と、3 回失敗した原因(2026-08-17)

読む側(WP305〜307)は完成した。編集側は**設計を 3 回回して 3 回とも不合格**である。
着手する前に、ここに書いてある条件と失敗原因を読むこと。

### 利用者が決めた条件(設計の必須要件)

1. **手書きは最小差分であること。**全体ダンプにしない。
   物理層の作法(eject 結果は全部入りだが、手書きは変更する分だけに減らせる)に倣う。
   **規約ではなく形式で強制すること** —— `pelican.render_feature` は compose 経路で唯一、
   閉じたキー集合もサイズ上限も持たない文書なので、それを流用すると最小性が保証されない。
2. **最後にコンパイルが通った全体を派生物として保持する。
   複数世代を利用者が指定して保持できること。**
   大きさは実測済み: `ejectable_complete_physical_plan` が 27 KB
   (著作ファイル 12 KB の 2.3 倍)、`ejectable_physical_fragment` 7 KB、
   `ejectable_pin_package` 209 B。git に何世代か置いても問題にならない。
3. **保持したものを黙って入力にしないこと。**
   入力にする道はあってよいが、`vulkan_plan_pins` と同じく
   **fingerprint で古さを検出して名前付きで落ちること。**
4. **デバッグ出力の切り替えは、グラフ編集と同じ「反映」問題として扱うこと。
   別の機構を作らないこと。**
   デバッグ表示は既に feature 文書として存在し(`debug_draw` / `debug_text` /
   `gpu_timing` / `gizmo` / `picking`)、`--feature-overlay` で積める
   (`render_feature_overlays` は `std::vector`、`launchconfig.hpp:68`)。
   ただし **`EngineLaunchConfig` から読むので起動時にしか効かない**。
   再起動なしで切り替えるのは、編集の反映と同じ問題である。

### 私(Claude)が誤った前提を「再設計禁止」として渡していた

3 回の設計ワークフローすべてに、次を `SETTLED, do not redesign` として渡した:

> 合成後の設定は書き戻せない

**全体を丸ごと戻す場合には事実である。**しかしそこから
「**原文への限定的な書き戻しも不可能**」と一般化したのは誤りで、
その一般化を検証禁止として渡したため、**Claude 側のレビュアは誰もそれを疑えなかった。**

codex に見せた瞬間に指摘された:

> 「合成済み全体をそのまま authoring input に戻す」直接 round-trip は一般には成立しない。
> **壊したのは、この事実から「原文への限定書き戻しも全て不可能」とする一般化である。**

**次の設計では、この前提を検証対象として渡すこと。**

### 設計レビューを Claude だけで完結させないこと

3 回とも Workflow で 4〜5 レンズを当て、毎回「全レンズ FATAL」を得ていた。
**しかし codex には一度も見せていなかった。**
仕様とコードでは codex が毎回 Claude 側の見落としを出していたのに、設計だけ同一モデル内だった。

codex が出した、Claude 側の誰も問わなかった観点:

> **利用者の 4 要求を表にして照合する。**

その結果、勝ち残った案は 4 要求すべて未達で、
提案された v1 は実質「`--feature-overlay` を繰り返し指定可能にする」だけだった。
**Claude 側のレンズは機構を攻撃したが、「この v1 は要求を 1 つでも満たすのか」を誰も問わなかった。**

要求 3 への指摘が象徴的である —— 保持物が存在しないのに
「入力にしていない」と主張するのは、満たしているのではなく**空虚**。

### 有力な材料(調査で判明、未活用)

- **`ReloadService::applyRuntimeNow`(`src/core/watch/reloadservice.cpp:172`)は
  ReloadGate を見ない。**`reload_game_logic`(`rpcserver.cpp:1116`)が
  「RPC が参加者を名指し → `applyRuntimeNow` → `committed`/`error` を見て名前付きで返す」
  という全パターンを既に実演している。**反映の道は在る。呼び出し元が無いだけである。**
- `applyRenderFeatureOverlays`(`src/project/renderfeatureoverlay.cpp:119`)は
  著作 JSON と lowering の間の**唯一の注入点**で、boot と reload の両方で呼ばれ、
  厳格な envelope 検証とローダ注入の継ぎ目を既に持つ。
- 物理層のファイル取り込み口(`renderpipeline.cpp:461-477`)と
  `input_config_fingerprint`(`logicalrendergraph.cpp:533`)が、
  論理層 override の前例と上流アンカーとして使える。

### 出荷プロジェクトの実測(設計の前提として)

| プロジェクト | preset | feature | 著作 target | 著作 pass |
|---|---|---:|---:|---:|
| example | — | 1 | 20 | 20 |
| animgraph_demo | hybrid_v1 | 3 | **0** | **0** |
| sprite_demo | — | 1 | 7 | 3 |
| vrm_xr_demo | — | 0 | 7 | 3 |

**「プロジェクトファイルに全レンダリングパスが残る」は既に成り立っていない。**
4 つ中 3 つがそうで、`animgraph_demo` は 1 行も書いていない。
`example` だけが全部書いている外れ値である。

## 編集側の設計 — 4 回目の案(2026-08-19)【却下。5 回目に置き換え済み】

**この節は却下された案の記録である。実装の根拠にしないこと。**
既存の編集契約 `docs/design_editor_tooling.md` を読まずに書かれ、
両レビューで 4 条件 0/4 と判定された。**次節が現行の設計である。**


前 3 回の失敗原因は「編集側の設計 — 確定した条件と、3 回失敗した原因」節にある。
**この案は、そこで誤りと判明した一般化を外して書き直したものである。**

### 外した前提

前 3 回は次を `SETTLED, do not redesign` として渡していた。

> 合成後の設定は書き戻せない

**全体を丸ごと戻す場合には事実。しかし「原文への限定的な書き戻しも不可能」への一般化は誤り。**
本案は**著作原文を直接編集する**。これが本線である。

---

### A. 編集の対象は著作文書。プランではない

studio は既に著作ファイルを読んでいる(`FullscreenPassWidget::openProjectReadOnly`)。
読んだ結果は `authored_passes` に **生の `nlohmann::json` のまま**入っている。

**編集はその文書をその場で書き換え、同じファイルへ書き戻す。**

#### 型付きモデルへ往復させないこと

`projects/example` の fullscreen は **16/16 がフォームの知らない鍵**を持つ
(`clear_color` 15 / `color_load_op` 4 / `push_constants` 2 / `uses_light_data` 1)。
型付き往復は**知らない鍵を黙って落とす**。

`pelican_project` に著作 config のシリアライザは存在しない
(あるのは `serializeCompiledRenderPipelineMetadata` = コンパイル結果のメタデータ)。
**作らないこと。**

#### これが条件 1 を形式で満たす

利用者の条件 1 は「最小差分であること。**規約ではなく形式で強制すること**」である。
元文書をその場で書き換える方式では、**差分が最小であることが構造上の帰結**であり、
規約で縛る必要がない。全体ダンプを吐く経路がそもそも存在しない。

### B. 同一性は著作側から取る

- 著作側: `(グラフ名, passes[] 内の位置)` + 名前。**ファイル内で定義が閉じる。**
- プラン側: `FramePlanNodeKey{graph, name}` は「意図的に永続 ID ではない」まま。
  **これはビューの鍵であって、編集の鍵ではない。**

**「識別子が永続でない」という未解決項目は、プランを編集しようとしたときだけ発生する。**
著作を編集すれば消える。

### C. preset 型プロジェクトは明示的な eject

`animgraph_demo` は著作パス **0**、preset `hybrid_v1` が **11 target / 9 pass** を供給する。
preset 使用中の著作 config の許可鍵は 11 個で、`rendering_passes` は**含まれない**。

したがって:

- **preset 由来のパスは編集不可として表示し、その旨を画面に出す。**
- **`eject` は名前付きの明示操作**とする。preset の内容を著作ファイルへ実体化し、
  以後は通常の編集対象になる。**一方向。**prefab の unpack と同じ作法。

**override 層を発明しないこと。**権威が 2 つになる。
WP324〜328 で 3 回踏んだ形と同型である。

### D. 反映は「保存 + reload RPC」

**道は既に在る。呼び出し元が無いだけである。**

- **`pelican.render_pipeline` reload 参加者は登録済み**(`src/core/vkcore/renderer.cpp:2838`、
  companion としても :2867)
- `ReloadService::applyRuntimeNow`(`src/core/watch/reloadservice.cpp:172`)は
  **ReloadGate を見ない**
- `applyRuntimeNow` の呼び出し元は現在 **1 つだけ** ——
  `reload_game_logic` RPC(`src/core/communication/rpcserver.cpp:1128`)。
  これが「**RPC が参加者を名指し → `applyRuntimeNow` → committed/error を見て
  名前付きで返す**」全パターンを既に実演している

**したがって反映は、`pelican.render_pipeline` を名指しする RPC を 1 本足すことである。**

#### 反映のコストは隠さない

再コンパイルは family 全体・グローバル mutex・デバイス `waitIdle` を伴う。
**止まらなくする話ではない。UI がそれを前提にすること。**
また **コンパイルが通っても反映が通るとは限らない。**
`applyRuntimeNow` は committed/error を返すので、**RPC はその実結果を返すこと。**

### E. 検証は既に正しい場所にある

パスの形の権威(`src/project/passshapepolicy.*`)は WP324/326/327 で
**`pelican_project`** に入った。**studio は `pelican_core` をリンクせずに形を検証できる。**

判定できないまま残るのは **usage 適合**(WP323 未着手)、**生成順**、**シェーダー解決**。
**いまフォームがやっているとおり、軸名を挙げて「判定していない」と表示すること。**

### F. プランはビューのまま

反映後に再取得する。fingerprint で古さを検出する(条件 3 の機構)。
条件 2 の「最後に通った全体を世代保持」は
`ejectable_complete_physical_plan`(27 KB)/ `ejectable_physical_fragment`(7 KB)/
`ejectable_pin_package`(209 B)が対象で、**編集とは別の機構。v1 の関門にしないこと。**

### G. 利用者の 4 条件との照合

| 条件 | 本案 | 根拠 |
|---|---|---|
| 1. 最小差分を**形式で**強制 | **満たす** | 元文書をその場で書き換えるので全体ダンプを吐く経路が無い |
| 2. 最後に通った全体を世代保持 | **v1 では未達** | `ejectable_*` の保持は別機構。**空虚な主張をしないこと** |
| 3. 保持物を黙って入力にしない | **v1 では空虚** | 保持物が無い段階で「入力にしていない」は主張として空虚。条件 2 と同時に扱う |
| 4. デバッグ出力の切替も同じ反映問題 | **満たしうる** | 同じ `applyRuntimeNow` 経路に載る。ただし `--feature-overlay` は `EngineLaunchConfig` 由来で起動時にしか効かない。**この差を設計が答えること** |

**条件 2 と 3 は v1 で未達であることを明記する。**
前回、保持物が存在しないのに「入力にしていない」と主張して空虚と指摘された。

---

## 編集側の設計 — 5 回目(2026-08-19 夜)

**4 回目は Reject。両レビュー合わせて 20 件近くの指摘が出た。**
だが最大の問題は指摘ではなく、**設計を書く前に既存の設計書を探さなかったこと**である。

### 既に受理済みの編集契約がある

`docs/design_editor_tooling.md`(664 行・47 KB・2026-08-04・レビュー受理済み)。
**4 回目はこれを読まずに書かれた。**

同書が定めていること:

- **正本 = `AuthoringSceneDocument`(authored JSON の lossless な **engine 内**表現)。
  runtime は投影である**
- edit = document + 投影の**同時 commit トランザクション**。
  全 preflight 後に failure-atomic に commit し、**部分成功を禁止**する
- **ticket 方式** —— edit は受理と ticket を同期応答し、
  結果は `step_frame.edit_results[]` または `get_edit_result(ticket)` で返す
- Save = 決定的 encode + **atomic replace**。
  **digest 不一致は `external_modification` で拒否し、何も書かない**
- journal に forward/inverse
- **`ReloadGate` の `enabled()` 流用禁止。`can_edit` を独立 query として追加する**

**ただし同書は scene 用であり、レンダリング config を扱っていない**
(`rendering_config` / `rendering_passes` の出現は 0 件)。

**したがって本設計は「5 回目をゼロから書く」ではなく、
「同書の契約をレンダリング config へ拡張する」である。**

### 契約に従うと、4 回目の欠陥がまとめて消える

| 4 回目の欠陥(レビュー指摘) | 契約で消える理由 |
|---|---|
| studio と player が別ファイルを解決しうる(`--user-dir`) | **studio はファイルに触らない。**document は engine が所有する |
| studio の RPC が 5 秒 timeout、reload は family 全体で超えうる | **ticket 方式**が既に定められている |
| 保存してから reload すると、失敗時に壊れた project が disk に残る | **preflight → atomic replace。**失敗時は何も書かない |
| 最小差分にならない(原文・位置・表記を保持していない) | **lossless document が正本。**studio は編集トランザクションを送るだけ |
| D0(studio は `pelican_core` をリンクできない) | studio は RPC を送るだけなので**自明に満たす** |

**4 回目の中心的な誤り**は「studio が著作ファイルを直接書き換える」であった。
正しくは **engine が document を所有し、studio は編集を要求する**。

### v1 の編集面は `features[]` にする

**4 回目の v1(既存パスの属性編集)は 4 条件のどれも満たさない。**

- `pelican project init` が生成する config は **`hybrid_v1` preset** である。
  **標準コマンドで作った新規プロジェクトを編集できない。**
  preset は例外ではなく**既定**である
- `example` で実際に可能な意味のある変更は
  「bloom/SSAO の再配線と、同一入力数のステム差し替え」1 族に縮む。
  しかも studio は入力の**個数しか見ていない**ので、
  差し替えると**警告なく絵だけが変わる**
- 所有鍵の内側でも最小差分が破れる ——
  `vrm_xr_demo` と `sprite_demo` は `"color": "ssao_blur"` と**スカラー**で書いており、
  フォームは常に配列を出すので、シェーダー名を 1 文字変えるだけで
  `["ssao_blur"]` に正規化され、無かった `"input": []` が新設される

**代わりに top-level `features[]`(文字列の配列)を編集面にする。**

| 理由 | 根拠 |
|---|---|
| **4/4 のプロジェクトで動く** | `features` は **preset 併用時の許可鍵 11 個に含まれる**(`renderpipeline.cpp:708-713`)。`animgraph_demo` で編集できるのは実際この配列だけである(3 要素) |
| **条件 4 を丸ごと満たす** | デバッグ表示は全て feature 文書(`debug_draw` / `debug_text` / `gpu_timing` / `gizmo` / `picking`)。利用者が条件 4 に挙げたものそのものである |
| **機構がほぼ要らない** | 文字列配列 1 本。パスの型も所有鍵表も形の検証も要らない |
| **最小差分が自明** | 配列要素の増減だけなので、表記の正規化問題が発生しない |

**パス属性の編集はその後である。**

### ReloadGate について(4 回目の誤読を訂正)

`ReloadGate` は `enabled_ = requested_ && !replay_ && !strict_ && !rpc_` であり、
**`--rpc` のとき必ず無効**(理由 "rpc driver")。
**studio は必ず `--rpc` で player を起動する。**

4 回目は「`applyRuntimeNow` は gate を見ない」を**利点**として書いた。
事実ではあるが、**決定性のために意図的に閉じている門を別口で回る**という意味である。

**既存契約が正しい**:

> `ReloadGate` の `enabled()` 流用禁止 —— **`can_edit` を独立 query として追加する**

**本設計はこれに従う。**gate を迂回しない。

### fingerprint は 1 語ではない

4 回目は「fingerprint で古さを検出」とだけ書いた。**識別子は少なくとも 4 種ある。**

| 識別子 | 用途 |
|---|---|
| source file digest | 外部変更の検出・CAS |
| effective config fingerprint | preset / feature / overlay 合成後 |
| runtime generation | 現在 publish されている版 |
| logical / automatic plan fingerprint | 保持した物理 artifact の入力適合 |

**具体例**: root の bytes は不変のまま editor overlay や feature provider が変わると、
source digest は同じだがフレームプランは stale である。
逆にウィンドウ再 lowering では runtime generation が変わるが source は stale でない。

**型と UI 表示で 4 種を分けること。**「fingerprint で古さを検出」だけでは設計にならない。

### 反映の結果を信じられるようにすること

`committed=false` は「旧 runtime のまま」を意味しない。
variant family 登録は `publishPreparedGeneration()` を呼んでから返り、
その後の処理(watch source 更新、履歴 reset の `waitIdle`)も同じ `try/catch` に入るため、
**新しい generation が publish 済みでも RPC が failure を返しうる。**

**fallible な準備を全て publication の前へ移し、最後を no-throw の commit にすること。**
返値は少なくとも `{published_generation, source_digest, committed}` を持つこと。
**publish 後の後始末の失敗を commit の失敗と混同しないこと。**

### 制約(設計が答えるべき、解いていない問題)

- **field 単位の provenance が無い。**`pass_overrides` は feature が root pass の
  `input` / `resource_ports` / `material_resources` を合成後に足せるが、
  provenance は pass 単位しかない。**「この pass は project 由来」でも
  「この field は feature 由来」でありうる。**
- **実効 config は複数文書に散る。**4 プロジェクトとも root + feature + editor overlay で、
  feature は pass 自体も供給する(UI pass は `engine://features/ui.json` 内)。
  `engine://` は書き込み可能ファイルに解決できない。
  **編集対象は「pass の source」ではなく「field の owner document」で決まる。**
  `features[]` を v1 の面に選ぶのは、この問題を踏まないからでもある。

### 段階(4 回目から順序を変更)

**4 回目は段階 1・2 を編集の前提としたが、それは誤りだった。**
ノード詳細も workspace も、1 field の save/apply には要らない。
独立した WP としては有用なので、**前提から外して並行に置く。**

```
1. document / CAS / lossless な編集トランザクション(契約の拡張)
2. features[] の 1 要素を選ぶ → 保存 → ticket で apply → 実効値を確認
3. preset の eject、feature の owner document 解決、パス属性の編集
4. 世代保持(条件 2・3)
5. 著作キャンバス
```

**WP329(ノード詳細)と workspace 状態は、この列に依存しない。並行してよい。**

### 著作キャンバスは「別 view-model」であって「別 widget」ではない

4 回目は「同居キャンバスは必ず自己矛盾表示になる」と書いた。**これは誤りである。**

編集後・反映前の `lighting_pass` を、著作レイヤでは amber/pending、
コンパイル済みレイヤでは generation N と表示するのは矛盾ではなく、
**利用者が見たい差分そのものである。**

辺の意味が違うこと(著作は pass↔resource の名前参照、プランは pass→pass の導出依存)は
**確認済みの事実**であり、**view-model は分ける。**
しかし **widget を分けることは論証されていない。**
分離を固定すると、著作ノードとエンジンの merge/split 結果の空間対応を
利用者が頭の中で再構成することになる。

**同期 split view / 同一 scene のレイヤ切替 / 重ね表示を比較してから決めること。**
linked selection と共有レイアウトを受け入れ条件にする。

### WP328 との関係(4 回目の過大評価を訂正)

**8 個目の trust route にはならない。**
本設計は config bytes を RPC で送らず、engine が document を持ち、
既存の participant が `PathResolver.loadText()` で読み直す。
**既存の variant-family compiler への既存入口である。**

### 受け入れ条件は段階 2 の WP で書く

4 回目には段階 3 の受け入れ条件が無かった。
そのため「永久に通らない対照」は見つからなかったが、
それは妥当だからではなく **対照自体が書かれていなかった**ためである。

**段階 2 を WP 化するときに、逐語の対照を書くこと。**最低限:

- **no-op 保存が byte-identical であること**(lossless の対照)
- **外部で変更されたファイルへの保存が `external_modification` で拒否され、
  何も書かれないこと**
- **apply が失敗したとき、disk と runtime の両方が編集前のままであること**
- **`features[]` に `gpu_timing` を足して apply すると、
  再起動なしで実効 config に現れること** —— 条件 4 の対照
- **同じテストの中で、足す前には現れないこと**


## ノードを自在に定義して書けるようにする(2026-08-23・第 5 版)

**第 4 版までは、位置束縛マクロの側を直そうとしていた。それが遠回りだった。**

**エンジンには既にシェーダー ABI が 2 つある。**

| | シェーダーが書くもの | 誰が binding を決めるか |
|---|---|---|
| **位置束縛マクロ**(レガシー) | `PELICAN_DECLARE_INPUT_5(ssaoSampler)` | **パスの `input` 配列の序数** |
| **生成 include**(正本にすべき) | `pelican_sample_ao(uv)` だけ | **エンジン** |

`docs/shader_contract.md` が既にこう書いている ——
**「実 descriptor 変数は generated include の内部詳細であり、shader は set/binding を書かない。」**

そして **production で動いている** ——
`clustered_light_select.comp` は `pelican_load_light_inventory(0u)` と書くだけで、
set も binding も書かない。`planar_reflection` の prefilter も同じ。

**第 5 版の決定: 生成 include を正本とし、位置束縛マクロを移行対象とする。**

### 8 本の暗黙の約束のうち、4 本が消える

第 4 版が数え上げた 8 本(§「なぜ 1 本ずつ切れないか」)のうち:

| | 生成 include にすると |
|---|---|
| **3. binding がパスの序数** | **消える** —— エンジンが振る。シェーダーは番号を見ない |
| **5. ソケットが feature 条件で増減** | **消える** —— 繋がっている port の宣言だけを出す |
| **7. `.surface` が material 専用** | **消える** —— 生成経路はもともと非 `.surface` |
| **8. 既定値の所在が 2 箇所 + 名前の部分一致** | **消える**(下記) |

**残るのは 4 本で、性質が違う:**

- **1(入力の口が 5 つ)/ 2(footprint が 3 箇所)** —— 「`resource_ports` へ寄せる」に変わる。
  **統合先が確定するので、迷いが消える**
- **4(順序が配列順)** —— シェーダー ABI と無関係。独立して直す
- **6(compute がパス型でない)** —— **スケジューラは対応済み**
  (`parseComputeNodes` がフレームグラフのノードを作る)。
  分かれているのは**著作型(`RenderPassType`)と `add_authored_pass`** だけ

### 既定値は「定数を返す accessor」にする

**これが第 4 版から最も大きく変わる点である。**

第 4 版は「engine が同じ binding へ既定 descriptor を書く」と設計した。
**生成 include なら descriptor 自体が要らない:**

```glsl
#define pelican_sample_ao(uv) vec4(1.0)   // 未接続のときはこう出す
```

**消えるもの:**

- **4×4 のダミーテクスチャを束縛する経路**(`tex_white` / `tex_black` / `tex_normal_default`)
- **set 2(material)と set 1(パス入力)の非対称** —— 既定値機構が material にしか無い問題
- **名前の部分一致による既定値の選択**
  (`inferDummy` が変数名に `"normal"` を含むかで決めている)
- **「descriptor が layout にあるのに書かれない」状態そのもの**

**未接続なら宣言が出ない。**だから未書き込み descriptor が原理的に発生しない。
第 4 版が「fail-fast の錨」として足そうとした検査も、**構造的に不要になる。**

**既定値は宣言に明示的に書く**(`default: 1.0` など)。**名前から推論しない。**

### OFF ビルドは、今日どの出荷プロジェクトも読めない(実測)

**この設計を止めていたのは「OFF を壊すな」だった。それが守っているものはゼロである。**

`featurecompose.cpp` は、runtime shader compiler が無効なときに
**required な feature が 1 つでもあれば throw する。**
そして `runtime_shader_compiler` を宣言しているのは
`rt_shadow_mask` 系の 2 つだけで(どちらも `"optional"`)、
**宣言が無ければ既定は required** である。

| プロジェクト | 使う feature | 自前シェーダー |
|---|---|---|
| `animgraph_demo` | shadow_directional / sky_ambient / ui —— **全部 required** | 0 |
| `example` | ui —— **required** | **3**(`.surface`) |
| `sprite_demo` | sprite —— **required** | **2** |
| `vrm_xr_demo` | (feature なし) | **2** |

**4 つとも OFF では読めない。**3 つは加えて `.spv` の無いソースシェーダーを持つ。

**したがって:**

- **移行が壊す「動いているもの」は無い**
- **WP211(dist-bake)は「壊したものの修復」ではなく、OFF 対応をこれから作る話である**
- **`RUNTIME_SHADER_COMPILER=OFF` の受け入れ条件は
  「ビルドが通り、テストが緑」までであって、「出荷プロジェクトが動く」ではない。**
  第 4 版までの記述はここを曖昧にしていた。訂正する

### 移行の順序

**効果が出る順に並べる。「単独で出荷できる最小」で並べない**(第 4 版でその失敗をした)。

```
1. fullscreen の 7 本を生成 include へ移す
   —— PELICAN_DECLARE_INPUT_n を pelican_sample_<port>() に置き換える
   —— binding がエンジン所有になる。序数ずれが消える
   【観測可能な変化】input の並べ替えで絵が変わらなくなる(今日は黙って変わる)

2. 未接続ソケットを定数 accessor にする
   —— 宣言に optional / default を足す
   【観測可能な変化】ssao_clear 回避策を 3 箇所すべてから削除できる
      (projects 2 件 + test/golden_harness.cpp)

3. 1 と 2 の口を resource_ports へ寄せる(= 8 本のうち 1・2)
   —— material_resources / screen_inputs / input を畳む
   —— footprint の 3 経路が 1 つになる

4. 順序を resource の参加者列へ(= 8 本のうち 4)
   —— キャンバスから組めるようになる前提

5. compute を著作型に入れる(= 8 本のうち 6)
   —— スケジューラは既に対応済み。RenderPassType と add_authored_pass だけ

保留: WP211 dist-bake —— OFF 対応を作る。移行の前提ではない
```

**`agent/wp336`(保留中)の扱い**: 共通 tokenizer と新文書種は段階 1 で要る。
**ただし「socket 数 == input 数」の検査は生成 include では意味が変わる**ので、
そのまま合流させず作り替えて取り込む。

### 受け入れ条件の骨子

**この節は 6 回「成立しない受け入れ条件」で差し戻されている。実測に基づいて書く。**

- **段階 1**: `deferred_lighting` の `input` を並べ替えても**絵が変わらないこと**。
  **今日は exit 0 / エラー 0 件 / VUID 0 件で走り、絵だけが変わる**(実測)。
  **これが「名前で繋がった」ことの直接の証拠になる**
- **段階 2**: `ssao_clear` パスと `ssao_blur` ターゲットが
  **`projects/sprite_demo` / `projects/vrm_xr_demo` / `test/golden_harness.cpp` の
  3 箇所すべてから消え**、ロードが成功しフレームが回ること。
  **画素比較を対照にしないこと** —— この 2 プロジェクトでは `ambientRadiance` が
  `vec3(0)` なので **AO が 1 でも未定義でも絵が byte 一致する**(実測)。
  **生成された accessor が定数を返していることを、生成 include の内容で検査する**
- **段階 2 の否定対照**: 既定値を宣言していないソケットを未接続にすると、
  **ソケット名を含む名前付きエラーで落ちること。**今日は無言である
- **shadow 有り / 無しの両構成**(`animgraph_demo` と他 3 つ)
- **`type: raster` の実証を失わないこと** ——
  `sprite_demo` の `ssao_clear` はプロジェクト空間で唯一の `raster` 使用例である。
  **代替の dogfood を同じ WP に含めること**
- 出荷 4 プロジェクトと `pelican project init`
- **`RUNTIME_SHADER_COMPILER=OFF` はビルドとテストが緑であること。**
  **「出荷プロジェクトが動くこと」を条件にしない**(今日も動かない)
- **`uv run tools/doclink.py check` が緑であること**

### 第 4 版から引き継ぐもの

- **`same_pixel` を語彙から外す**(利用者の判断)。**扉は 2 枚あることも含めて**
- **footprint はソケットが持つ**(契約ではない)。`opaque_color` の組み込みが
  `neighborhood` である一方 `fullscreen.frag` は同一ピクセルで読む、という矛盾がある
- **順序の荷重点は 3 箇所**(`buildEdges` / `topologicalOrder` / `enforceCanonicalOrder`)
- **実測値の訂正**(手書き `before` は 4 field / 6 edge で全部外部、
  `insert` は canonical anchor 8 / 同一ファイル 15 / 他ファイル 2、複数 writer は 7 target)
- **canonical anchor は「名前を出さずに繋ぐ」の実例**であり、消す対象ではない
- **`surface_resources` の選択子**も同じく実例である

### 設計レビューによる訂正(2026-08-23)

**第 5 版は設計レビューで着手不可になった。方向は残るが、主張が 3 つ誤っていた。**

#### 訂正 1: engine シェーダーの移行は WP211 を前提とする

**第 5 版は「WP211 dist-bake は前提ではない」と書いた。engine シェーダーについては誤りである。**

```glsl
#ifdef PELICAN_FEATURE_CLUSTERED_LIGHTING
#include "pelican_resource_ports.glsl"     // ← マクロの内側
#endif
```

**`fullscreen.frag` がビルド時に焼けるのは、生成 include が `#ifdef` の内側にあり
CMake の bake 時に開かれないからである。**
生成 include は物理ファイルではなく、**runtime compiler に渡す仮想 include** である。

**そして生成 include を使う compute シェーダーは `b_embed`(生ソース)だけで、
`embed_shader`(glslang で焼く)には入っていない。**

**対応関係: 生成 include ⟺ 実行時コンパイル専用。**
焼かれるシェーダーは、マクロで囲わない限り生成 include を使えない。

**したがって 7 本を無条件に移すと CMake の bake が壊れる。**
engine SPIR-V 経路は defines / virtual includes が付くこと自体を拒否する。

#### 訂正 2: 「4 本が消える」は不正確

| | 正確には |
|---|---|
| **3. binding** | **半分**。著作者は番号を書かなくなるが、**番号は今も `input` の index** であり、順序依存と cache identity は残る |
| **5. 条件付きソケット** | generator 単体では成立。**ただし shadow composer が `resource_ports` を供給しない**ので production では未成立 |
| **7. `.surface` 専用** | **消える** |
| **8. 既定値** | **誤り。消えず、新規実装である**(下記) |

#### 訂正 3: 既定値は新機能である

**現行 `resource_ports` は既存の edge への注釈**であり、
受理時に resource が `reads`/`writes`/`input` に在ることを要求する。
**未接続ソケットを書く構文が無い。**
さらに **binding が空なら interface の生成自体を打ち切る**ので、
「定数 accessor」以前に **include 不在で失敗する。**

**必要なのは sentinel ではなく論理解決の結果である:**

```
socket → connected binding | constant value | required-missing
```

- **connected** —— descriptor と accessor を生成
- **constant** —— descriptor 無しの inline accessor を生成
- **required-missing** —— コンパイル前に graph / pass / shader / socket 名付きで失敗

**既定値 accessor の ABI を最初は狭めること** ——
現行 ABI には sample 以外に size / mip / view 系があり、
**`ssao_blur` は `textureSize` を使う。**
物理テクスチャを持たない定数の size をどう定義するかは未記載である。
**最初は「sample のみを許す `vec4` 既定値」に限る。**

**なお material の dummy/default 経路は生成 include では消えない。**
「消える」のは **pass socket に限った話**である。そう明記すること。

#### 訂正 4: 段階 1 は「7 本の変更」ではない

**全 consumer / configuration を同時に移さないと、生成 include に binding が供給されない。**

- **shadow composer は `input` と sampling を足すが `resource_ports` を足さない**
- **canonical `output_transform` の生成にも port が無い**
- **OpenXR mirror は `engine://output_transform` を直接構築する**(失敗は警告で握り潰される)
- render-policy fixture も engine shader を直接構築する
- **`cube_capture` / `planar_reflection` は `deferred_lighting` の binding を継承する** ——
  継承先の先頭 input に生成 binding が無くなり、ロードが失敗する。
  **capture 資源へ向けた明示的な socket map が要る**(順序で自動対応させると位置 ABI が残る)

#### 訂正 5: `sampler2DArray` は一律移行できない

生成 accessor は **`(vec2, uint layer)` の 2 引数のみ**。
旧マクロは view / layer を暗黙に選ぶ。
**`pelican_sample_<port>(uv)` の一律置換では array を扱えない。**
**shadow cascade は current view ではなく cascade layer を渡す必要がある。**

#### 訂正 6: 作業量

**「7 ファイル・15 置換」ではない。**
15 宣言 + 15 sample + `textureSize` 1 箇所 + shadow の直接 `texture` 1 系統
+ **全 consumer / config**。

**variant は 2〜3**(shadow 有 7 口 / shadow 無 6 口 / AO 未接続 5 口)。
キャッシュはある(virtual include の内容も hash に入る)が、
**eviction / 上限は未確認**、`PipelineFactory` は論理的な interning をしていない。
**一般 GUI で任意 N ソケットを独立接続できるようにすると最大 `2^N`** なので、
canonical port order と variant 予算が要る。

#### 訂正 7: 受け入れ条件「並べ替えても絵が変わらない」は空振りする

**2 枚が同じだけでは、両方黒でも通る。**同一テストの中で:

- **区別可能な色**(赤・緑など)の resource を用意する
- **canonical 順と permuted 順の双方が、独立した期待画素に一致する**
- **socket / resource 対応を故意に交換した否定対照が、異なる画素になる**

**生成 include の binding 番号検査は補助であって、画素検査の代わりにならない。**

**生成 include は production から取り出せる**(`ShaderBundle::virtual_includes`)。

#### 残ったもの

- **OFF の実測は正しい**(出荷 4 プロジェクトはいずれも OFF で読めない)。
  **ただし raw set 1 ABI は `docs/shader_contract.md` の公開契約である。**
  4 プロジェクトが動かないことは、外部の precompiled consumer を壊してよい根拠にならない
- **`.surface` 専用という制約は消せる**
- **生成 include が fullscreen / raster のコードで対応済み**であることも確認された
  (ただし shipped raster shader による end-to-end 実証は fullscreen / compute までである)

### 目的地と、今日開いている道は別である(結論)

**第 5 版(生成 include + 定数 accessor)は目的地として正しい。**
**しかし engine シェーダーを動かすには WP211 が要る。**

**第 4 版の道は今日開いている** ——
焼いたシェーダーの位置束縛マクロはそのままにし、
**エンジンが未接続 binding へ既定 descriptor を書く。**
必要なのは「binding 5 は省略可・既定は白」という宣言(`agent/wp336` の器)と、
`FullscreenPassContainer` から白い image view へ届く配線だけで、
**bake には触らない。**

**したがって:**

- **`ssao_clear` を消したいだけなら第 4 版の道**(bake 不要)
- **`.surface` 制約を外し、利用者が自在にシェーダーを書くなら第 5 版の道**(WP211 が前提)
- **project 所有のシェーダーは既に実行時コンパイル専用**なので、
  **そこだけなら第 5 版の道が今日でも通る**(ただし `ssao_clear` には届かない)

**次に決めるのはこの三択である。**

### 訂正: bake は前提ではない(実験で確認・2026-08-23)

**設計レビューは「7 本を生成 include にすると CMake の bake が壊れる」と指摘した。
それは「焼き続けたまま」の場合である。**

**正しい移行は「焼くのをやめる」** ——
`clustered_light_select.comp` が既にその形である
(`b_embed` で生ソースだけ埋め込み、`embed_shader` に無い)。
**生成 include を使うシェーダーは、そもそも焼かれていない。**

#### 実験(ブランチ `exp/unbake`、コミット `f4f5f14`)

`ssao_blur.frag` について:

- `src/core/resources/CMakeLists.txt` の `embed_shader(ssao_blur.frag)` を削除
- `src/core/loader/engineresources.cpp` の `.spv` 登録 2 行を削除
- `test/fixtures/project_format/engine_resources.json` の 1 行を削除

**結果: configure 成功 / build 成功 / `ctest -LE gpu` が 1137 件 0 失敗(完全に緑)。**

**費用は削除 3 行 + 台帳合わせ 1 行。**
`loadFromStemReference` は compiler ON ならソースを先に試すので、
`.spv` が無くても解決する。

#### 代償

**そのシェーダーは実行時コンパイラが必須になる。**
OFF ビルドでは `.spv` 候補しか出さないので見つからない。

**ただし出荷 4 プロジェクトは既に全部 OFF で読めない**(§「OFF ビルドは、今日どの出荷プロジェクトも読めない」)。
**したがって WP211 は完了の前提ではない。**OFF 対応を作る独立した仕事である。

### 完了までの順序(2026-08-23)

**利用者の判断: 中途半端にせず終わらせる。**
以下は 8 本の暗黙の約束をすべて明示に変えるまでの全行程である。

**各段階に「観測可能に何が変わるか」を先に書く。書けない段階は切り方が間違っている。**

#### A. 焼くのをやめる

7 本を `embed_shader` から外し、`.spv` の登録と fixture を追随させる。

**観測可能**: OFF ビルドで `engine://fullscreen` が**名前付きで見つからないこと**。
今日は `.spv` があるので見つかるが、feature が required なのでどのみち読めない ——
**「動いていたものが壊れる」のではなく「動いていない理由が正直になる」。**

**risk**: OFF のテスト 1094 件のうち engine シェーダーを実際に**ロードする**ものがあれば落ちる。
**着手時に測ること。**shader 依存のテストは既に `if(PELICAN_RUNTIME_SHADER_COMPILER)` で除外されている。

#### B. 7 本を生成 include へ移す

- **15 宣言 + 15 sample** を `pelican_sample_<port>()` へ
- **`ssao_blur` の `textureSize` 1 箇所**と **shadow の直接 `texture` 1 系統**
- **`sampler2DArray` の accessor は `(vec2, uint layer)` の 2 引数しかない。**
  一律置換できない。**shadow cascade は current view ではなく cascade layer を渡す**
- **全 consumer を同時に移す** ——
  shadow composer(`input` と sampling は足すが `resource_ports` を足さない)/
  canonical `output_transform` / OpenXR mirror(`engine://output_transform` を直接構築、
  失敗が警告で握り潰される)/ render-policy fixture
- **`cube_capture` / `planar_reflection` は `deferred_lighting` の binding を継承する。**
  **capture 資源へ向けた明示的な socket map が要る**(順序で自動対応させると位置 ABI が残る)

**観測可能**: **`deferred_lighting` の `input` を並べ替えても絵が変わらないこと。**
今日は exit 0 / エラー 0 件で**絵だけが変わる**(実測)。

**対照の作り方(空振り防止)**: 「2 枚が同じ」では両方黒でも通る。
**区別可能な色**の resource を用意し、**canonical 順と permuted 順の双方が
独立した期待画素に一致**し、**socket / resource 対応を故意に交換した否定対照が
異なる画素になる**まで検査する。
生成 include の binding 番号検査は補助であって画素検査の代わりにならない
(生成 include は `ShaderBundle::virtual_includes` から production 経路で取れる)。

#### C. 未接続ソケットの解決

**これは「消える」ではなく新規実装である。**

```
socket → connected binding | constant value | required-missing
```

- **connected** —— descriptor と accessor を生成
- **constant** —— **descriptor 無しの inline accessor** を生成
- **required-missing** —— コンパイル前に graph / pass / shader / socket 名付きで失敗

**既定値 accessor の ABI を最初は狭める** ——
現行 ABI には sample 以外に size / mip / view 系があり、
**物理テクスチャを持たない定数の size をどう定義するかが未解決**である。
**まず「sample のみを許す `vec4` 既定値」に限る。**

**binding が空だと interface 生成を打ち切る**現行分岐も直す必要がある
(定数 accessor 以前に include 不在で落ちる)。

**観測可能**: **`ssao_clear` を 3 箇所すべて**
(`projects/sprite_demo` / `projects/vrm_xr_demo` / `test/golden_harness.cpp`)
**から削除できること。**

**画素比較を対照にしない** —— この 2 プロジェクトでは `ambientRadiance` が `vec3(0)` なので
**AO が 1 でも未定義でも絵が byte 一致する**(実測)。
**生成された accessor が定数を返していることを、生成 include の内容で検査する。**

**`type: raster` の実証を失わないこと** ——
`sprite_demo` の `ssao_clear` はプロジェクト空間で唯一の raster 使用例である。
**代替の dogfood を同じ WP に含める。**

#### D. 入力の口を `resource_ports` へ寄せる

`input` / `material_resources` / `screen_inputs` を畳む。
**`surface_resources` は供給側の選択子なので残す。**
**footprint の 3 経路(`material_resources.footprint` / `input_footprints` /
`read_footprints`)がここで 1 つになる。**

**観測可能**: 同じ resource を複数の契約に繋げること
(`forward_transparent` の `opaque_depth` / `scene_depth` / `linear_view_depth` の 3 契約)。
今日は `resource_ports` が同一 resource の多重束縛を禁じている。

#### E. 順序を resource の参加者列へ

**荷重点は 3 箇所** —— `buildEdges`(先行 writer からしか辺を張らない)/
`topologicalOrder`(`declaration_index` で整列)/
**`enforceCanonicalOrder`(合成時に配列順を `after` 辺へ焼き込む)**。

**canonical anchor と `output_transform` の順序は resource に載らない。**
**非データ依存の channel として `before` / `after` を残す。**

**観測可能**: **キャンバスから 0 からグラフを組めること。**
今日は順序が著作 JSON の配列に宿っており、ノードを置いた位置には宿らない。

#### F. compute を著作型に入れる

**スケジューラは既に対応済み**(`parseComputeNodes` がフレームグラフのノードを作り、
`before`/`after` も barrier も効く)。
分かれているのは **`RenderPassType`(14 種、compute を含まない)と `add_authored_pass`** だけ。

**観測可能**: キャンバスから compute ノードを足せること。

#### 完了の定義

**8 本すべてが明示になること:**

1. 入力の口 → D
2. footprint 3 箇所 → D
3. binding が序数 → B
4. 順序が配列順 → E
5. ソケットが feature 条件で増減 → C(無条件宣言 + 既定値)
6. compute がパス型でない → F
7. `.surface` が material 専用 → B
8. 既定値の所在が 2 箇所 + 名前推論 → C

**WP211(dist-bake / OFF 対応)は完了の条件に含めない。**独立した仕事である。

## ノードを自在に定義して書けるようにする(2026-08-22・第 4 版)【機構は第 5 版へ。実測と制約は引き続き有効】

**位置束縛マクロの側を直すという前提は第 5 版で置き換えた。**
**実測、制約、訂正した数値はこの節が引き続き正本である。**

**第 3 版を敵対レビュー(codex)にかけ、その指摘をさらに独立検証(8 レンズ + 反証段)に
かけた結果である。**判定は次のとおり。

| レビューの指摘 | 検証後 | 効いたこと |
|---|---|---|
| 順序を resource に持たせても reader の読む値が決まらない | **一部正しい** | **論理 IR は既に表現できている。**壊れているのは著作形式と planner |
| `reads`/`writes` を全部ソケットから導出できない | **正しい** | **非 shader use が 5 類型ある** |
| `resource_ports` は型ではなく descriptor 注釈 | **正しい** | **`screen_inputs` は畳めない**(同一 resource 多重束縛を禁止している) |
| optional/default が未設計 | **正しい** | ただし**解は既に material 側にある** |
| 名前束縛は OFF ビルドで成立しない | **誤り** | **名前は焼いた `.spv` に入っている。**生成は不要（§2） |
| preset 編集の著作形式が無い | **過大** | 設計は既に大半を書いている。欠けているのは 1 点 |
| 「着手不可」(第 3 版へ) | **過大** | 順序と preset override の 2 件だけが保留 |
| 「着手不可」(第 4 版初稿へ) | **正しい** | **§2.1 は潰れた。**段階 1〜3 は訂正のうえ着手可 |

**そして第 3 版が書いた実測値はほとんどが誤っていた。**訂正は §0.1 に置く。

**第 4 版初稿も 1 点誤っていた** —— 「名前束縛は WP211 dist-bake を前提とする」。**焼いた `.spv` に変数名が残っており、reflection は既にそれを読んでいる。**§2 で訂正した。

**その訂正版をもう一度レビューにかけた結果、§2.1(テンプレ生成)が丸ごと潰れた。**
管理範囲を `//!` ヘッダだけにすると**ソケットを足しても宣言が増えず**、例自体が「契約はシェーダー、接続はパス」に違反していた。
**検算で 5 件すべて実在を確認した。**§2.1 と段階表と受け入れ条件を書き直した。

### 0. 核心 —— ソケットは発明しない。material 経路が既に全部持っている

第 3 版は「`resource_ports` を正本にする」と書いた。**これが誤りだった。**

**利用者が要求した 4 つは、すべて material 経路に既に実装されている:**

| 利用者の要求 | material 経路の実装 |
|---|---|
| 種類ごとのソケット | `MaterialPassInputContract`(nominal type / conversion / footprint / fallback) |
| 契約と接続の分離 | `MaterialPassInputContract` と `MaterialPassInputBinding` |
| 既定値 | `CustomTextureBinding::missing_default` → `MaterialDummyTexture::white` |
| ソケット名で落ちる | 「material pass 'X' does not provide shader input '<socket>'」 |
| 名前を出さずに繋ぐ | `surface_resources` の contract 選択子 / canonical anchor |

**したがって設計は「ソケットを作る」ではない。**
**「material が持っているものを fullscreen と compute へ一般化する」である。**

`resource_ports` は**その一般化の受け皿にならない** ——
型を持たず(`shaderresourceport.hpp` は resource / kind / element / access /
view / sampling / subresource だけ)、
**同一 resource への多重束縛を禁止している。**
`forward_transparent` は同じ `opaque_depth` を
`opaque_depth` / `scene_depth` / `linear_view_depth` の**三契約**に繋いでおり、
うち一つは depth linearization を伴う。**畳めない。**
fullscreen パーサの拒否は無条件である。

**`resource_ports` は shader-descriptor 部分集合の frontend に格下げする。**
正本は既存の `MaterialPassInputContract` と `LogicalResourceUse` である。

**ただし「`.surface` を fullscreen / compute へ広げる」という読み方は誤りである。**
`.surface` は material 専用で、fullscreen には届かない。
**共有するのは文書種ではなく文法とパーサである。**実測と確定した schema は §1.1 に置いた。

### 0.1 実測値の訂正(第 3 版はほぼ全部を誤っていた)

**独立に 2 回数え直した結果である。**

| 第 3 版 | 正しい値 |
|---|---|
| 手書き `before` 4 件、うち他人を名指すのは 2 件 | **4 field / 6 edge。6 edge 全部が他ファイルのパスを名指す** |
| `insert` の名指し 25 件 = intra 18 / cross 7 | **canonical anchor 8 / 同一ファイル内 15 / 他ファイル 2** |
| 壊れる結合 9 件・名指されるパス 6 つ | **6 宣言箇所 / 8 edge / 外部パス名 6 つ** |
| 複数 writer のターゲット 4 件 | **7 件。深度 3 本が抜けていた** |
| `resource_ports` 13 / `material_resources` 8 / `screen_inputs` 3 / `surface_resources` 1 | key 数としては正しい。**entry 数は 25 / 10 / 12 / 1** |
| render target の鍵 14 | 正しい |

**最も重要な訂正: `insert` の「cross 7 件」のうち 5 件は壊れる結合ではない。**
`hdr`→`tonemap` / `picking`・`velocity`・`taa`→`post_main` / `gizmo`→`debug_text` は、
**エンジンが無条件に挿入する canonical anchor 8 個**に解決される。
`hybrid_v1` に `tonemap` や `post_main` という名のパスは存在しない。
**これは「名前で繋いでいる」のではなく「エンジン所有の安定ノードに繋いでいる」**であり、
**§1 の「種類で繋ぐ」の三つ目の既存実例である。**消す対象ではない。

**本当に壊れる結合:**

| 宣言 | 名指す先 |
|---|---|
| `rt_shadow_mask` の `insert` | `deferred_geometry` |
| `sky_ambient` の `insert` | `deferred_lighting` |
| `clustered_light_select` の `before` | `deferred_lighting` / `forward_opaque` / `forward_transparent` |
| `planar_reflection_light_select` の `before` | `planar_reflection_lighting` |
| `cube_capture_light_select` の `before` | `cube_capture_lighting` |
| `planar_reflection_filter_mip_6` の `before` | `forward_transparent` |

**6 宣言箇所・8 edge・外部パス名 6 つ。**うち 4 つは preset の構造パスである。

**複数 writer のターゲット(7 件):**

| ターゲット | writer 数 |
|---|---|
| `swapchain` | 6 |
| `lit_color` | 4 |
| `scene_depth` | 3 |
| `cube_capture_color` | 3 |
| `cube_capture_depth` | 3 |
| `planar_reflection_color` | 3(compute の mip 6 を含めると **9**) |
| `planar_reflection_depth` | 3 |

**深度が 3 本ある。**`output.depth` も `node.writes` に入り
`validateWritesAreOrdered` の対象なので、
**順序機構を color 専用にはできない。**

**注記**: 上表は全 feature を有効にした和である。resolved では
`swapchain` の writer は `output_transform` 1 件になり、
残りは `display` 側へ移る。出荷 4 プロジェクトの `display` writer は最大 2 件。

### 1. 三軸ある。混ぜないこと

第 3 版は「二軸」と書いた。**三軸である。**

| 軸 | 誰が所有するか | 既存の実装 |
|---|---|---|
| **契約** | 消費側の**実装**(シェーダ / material) | `MaterialPassInputContract`、`LogicalPortContract` |
| **接続** | パスの**インスタンス** | `MaterialPassInputBinding`、`resource_ports`、`input` |
| **選択子** | **供給側** | `surface_resources` の contract 選択、canonical anchor |

**第 3 版が「畳む」と言ったのは接続の軸だけである。**
契約を接続に畳もうとしたのが誤りだった ——
**接続を消すと契約も消えるので、既定値の宣言を置く場所が無くなる。**

#### 契約(消費側の実装が所有)

**シェーダが「AO ソケットが要る。無ければ 1.0」と宣言する。**
これはパスの接続とは別の文書に住む。
`surface_resources` の supply document が既に
`//! resource_ports: - {name: ...}` の形でこれをやっている。

**この軸が無いと、利用者の「型にエラーがなければ動く」は成立しない。**

#### 接続(パスインスタンスが所有)

`resource_ports` を frontend にする。ただし:

- **同一 resource への多重束縛を許すこと**(契約名で区別する)。
  現在の重複禁止(`shaderresourceport.cpp` と fullscreen パーサ)を書き換える
- **`material_resources` だけが持つ footprint と `@history` を落とさないこと**
- **`shaderresourceport.hpp` の「These never create graph edges.」契約を破棄すること**

#### 選択子(供給側が所有)

**パス名を名指さずに消費者へ届ける機構が、既に 3 つある:**

1. `surface_resources` の `material_contracts` / `fullscreen_consumers`
   —— `shadow_directional` は `deferred_lighting` を知らない
2. **canonical anchor 8 個** —— エンジンが無条件に挿入する安定ノード。
   `hdr` / `picking` / `velocity` / `taa` / `gizmo` はこれに繋いでいる
3. `material_contract` による material パスの選択

**残る 6 件の壊れる結合を消す手段はこれである。**
新しい機構ではなく、**選択子の適用範囲を広げること。**

### 1.1 ソケット宣言 schema(段階 1 の正本・2026-08-22)

**段階 1 を WP にするには、この schema が決まっている必要がある。**
6 面の実測(engine シェーダー / 論理型語彙 / physical ABI / `//!` ヘッダ /
接続側 JSON / Studio の D0)から確定した。

#### 先に訂正 —— `.surface` は material 専用である

**§0 は「`.surface` が既にその形式で、fullscreen / compute へ広げる」と書いた。届かない。**

- `SurfacePass` は `main` / `deferred_geometry` / `forward` / `depth` / `velocity` の
  **5 値で、compute も fullscreen も無い**
- `SurfaceShaderComposition` は vertex_source と fragment_source しか持たない
- fullscreen / compute のパスは JSON の `shader` 鍵で `.frag` / `.comp` を直接指しており、
  **`.surface` を経由しない**

**したがって `.surface` に `sockets:` を足しても fullscreen には届かない。**
**新しい文書種が要る。**ただし**文法とパーサは `.surface` と共有する** ——
「形式を分岐させない」は文書種を 1 つにすることではなく、**同じ文法で書けること**である。

**もう一つの訂正**: **compute はパス型ではない。**
`RenderPassType` は 14 種で compute を含まず、compute は config 直下の `compute_tasks[]`、
ray_tracing はその task 内の 1 鍵である。
**著作コンテキストも `add_authored_pass` も compute には届いていない。**
「fullscreen と compute へ広げる」は、compute について別の配管を要する。

#### 宣言するもの / 導出するもの

**実測で確定した。理由なしに動かさないこと。**

| | 宣言 | 導出 | 理由 |
|---|---|---|---|
| ソケット名 | ● | | シェーダーの語彙。reflection の `OpName` は照合にのみ使う |
| 契約(型・footprint・sampling・fallback) | ● | | **シェーダーの性質**。パスの著者は知り得ない |
| view 方針(`shared_2d`/`per_view`/`family_array`/`cube`) | ● | | **アルゴリズムの ABI である。**物理と一致しなければ例外、という制約側の値 |
| buffer の要素型(std430) | ● | | 物理 plan から導出できない。現行も明示必須 |
| stage 可視性(vertex / fragment / 両方) | ● | | input_attachment は fragment 限定。**これが決まらないと local read の可否が決まらない** |
| 出力の型と個数 | ● | | 入力のような自動変種が出力には無い |
| **省略可能性と既定値** | ● | | 本設計の目的そのもの |
| binding 番号 | | ● | **宣言の並び順**(下記) |
| descriptor 種別(sampler / input_attachment) | | ● | tile-local 融合の成否。**同じプロジェクトが GPU を変えるだけで反転する** |
| 次元(`sampler2D` / `sampler2DArray`) | | ● | 宣言 view × 物理 view_layout × consumer の積 |
| set 番号 | | ● | 常に 1(`PELICAN_SET_PASS_INPUT`) |

#### binding は「シェーダーの宣言順」にする(これが中核の変更)

**現在 binding は「そのパスの `input` 配列における序数」である。**
だから**パスの接続を編集すると全ソケットの番号がずれる。**
宣言者が知り得ない情報(パスの入力個数と順序)の関数になっている。

**`.surface` は既に正しい側にいる** —— `screen_inputs[i]` が binding=i、
つまり**宣言の並びが binding** である。

**同じ規則を fullscreen / compute へ適用する。**

- シェーダーの ABI が**著作時に確定する**。焼いた `.spv` と矛盾しない
- **SSAO を外しても番号がずれない**(接続が消えるだけ)
- 末尾に足す限り既存 binding は動かない。**間に挿すと動く。**そこは明示する

**ただし「接続が消えるだけ」は、接続側の仕様が来るまで成立しない(仕様レビューで確定)。**
今日の fullscreen は `input` が文字列配列で、**ソケット名も空き slot も表せない。**
具体例: `deferred_lighting` から SSAO の接続だけ消して shadow を有効にすると、
**shadow は序数 5 に詰められ、descriptor write は binding 5 になる。**
**接続を「ソケット名 → resource」にし、省略時も slot を残して既定 descriptor を束縛する**
までが揃って初めて成立する。**別 WP として名指しで残すこと。**

**注意**: `.surface` の set 1 は「宣言済み screen_inputs が 0 から連番で先頭を占め、
その後ろに feature 所有が付く」ことを reflection 照合が強制する。**穴も並べ替えも例外。**
新しいセクションを set 1 に置くなら、この連番規約のどこに割り込むかを決めること。

#### v1 は型を綴らない。契約を名前で呼ぶ

**論理型を宣言に綴らせてはいけない(v1 では)。**実測:

- 登録されている nominal type は **4 個だけ**
  (`color_signal` / `depth` / `legacy_opaque_image` / `legacy_opaque_resource`。後 2 者は逃げ道)
- **`LogicalType` に JSON パーサが存在しない。**writer だけで reader が無い。
  この schema が**最初の reader になってしまう**
- 変換は 3 個。しかも pass input の解決は `allow_explicit_conversions=false` 固定なので、
  **自動で使えるのは `depth_linearize` 1 個だけ**
- **`conversion` は宣言できても適用する汎用経路が無い** ——
  実際の GLSL は `if (input == "linear_view_depth")` という文字列直書き分岐が生んでいる
- `source_type` は宣言できず vk::Format から導出され、写像は **5 フォーマット → 2 型**のみ。
  **fullscreen の `R8G8B8A8` や storage buffer は現状論理型を持てない**

**したがって v1 は `.surface` の `screen_inputs` と同じ流儀にする** ——
**組み込み契約を名前で呼ぶ。**書ける名前は現状 5 つ
(`opaque_color` / `opaque_depth` / `scene_depth` / `linear_view_depth` / `directional_shadow`)
で、**足りないものは C++ 側に組み込み契約を追加する。**

**型を綴る文法は段階 1 の範囲外とする。**入れるなら
`namespace.name@major` + 引数写像 → `canonicalize()` のパーサ新設になり、
それ自体が独立した WP である。

#### footprint は 3 箇所に分かれている。ここを畳むのが仕事の実体

| 場所 | 鍵 |
|---|---|
| material | `material_resources` の項目内 `footprint` |
| fullscreen / raster | パス直下の `input_footprints` |
| compute | task 直下の `read_footprints` |

**同じ語彙(`same_pixel` / `neighborhood` + `radius` / `arbitrary`)を
3 つの別実装が解析している。**

**そして出荷資産で宣言しているのは 2 箇所だけ**(どちらも `planar_reflection` の `arbitrary`)。
**fullscreen パスは footprint を一切埋めず、全入力が `arbitrary` に落ちて
`legacy_read_footprint_conservative` が記録される。**
結果として **tile-local(local read)は engine シェーダー 7 本のどれにも発火していない。**

**footprint をシェーダー宣言へ移すと、この 3 箇所が 1 つになる。**
そして `ssao_blur` が `neighborhood, radius: 2` を**正しく名乗れる**ようになる
(現在は誰も書いていないので `arbitrary` 扱い)。

**制約**: `temporal` は JSON 語彙に無く、`@history` から導出せよと例外が言う。
`none` には綴りが無い。**宣言でも同じ規約を守ること。**
**read には footprint 必須、write には footprint 禁止**が二重に強制されている。

#### footprint はソケットが持つ。契約ではない(訂正)

**初版は `contract` と `footprint` を並べて書いたが、
どちらが正本かを決めていなかった。**仕様レビューで潰れた。

**実測**: `MaterialPassInputContract` は footprint を持っており、
**`opaque_color` の組み込み footprint は `neighborhood`** である
(`opaque_depth` / `scene_depth` は `same_pixel`)。

**しかし footprint は契約の性質ではない。消費側の性質である:**

- `fullscreen.frag` は albedo を**同一ピクセル**で読む
- `ssao_blur.frag` は同じ形の resource を **5×5 の近傍**で読む

**同じ契約でも、シェーダーが違えば footprint が違う。**

**決定: 新しい宣言経路では、footprint はソケットが所有する。**
**契約は型 / 変換 / sampling / view / fallback を持つ。**
`MaterialPassInputContract` が footprint を抱えているのは material 経路の既存事情であり、
**新経路ではソケット側を正本とする。**両方書けて曖昧、という状態にしないこと。

#### `same_pixel` は当面書けなくする(利用者の判断・2026-08-23)

**三値のうち、意味論を変えるのは `same_pixel` だけである。**

| 値 | 効果 | 間違えたとき |
|---|---|---|
| `arbitrary` / `neighborhood` | **最適化を禁じる**方向 | 遅くなるだけ |
| **`same_pixel`** | **タイルメモリ常駐を許す**方向 | **`uv` が捨てられて絵が壊れる** |

**local read に落ちたとき、マクロは `uv` を黙って捨てる:**

```glsl
#if PELICAN_INPUT_0_LOCAL_READ
#define PELICAN_TEXTURE_2D_0(value, uv) subpassLoad(value)
```

近傍を読むシェーダーがこの経路に入ると、
**5×5 のぼかしが「自分のピクセルを 25 回足して 25 で割る」= 恒等写像になる。**
**エラーも validation 失敗も出ない。絵が静かに間違うだけである。**
**そして `same_pixel` 宣言が本当かを検査している箇所は存在しない。**

**今は利益がゼロで、危険だけが将来に繰り延べられている**(実測)——
出荷 `example` の実プランで backend 候補 3 つの**コストが同一**である
(`materialized_plan@1` / `tile_local_plan@1` / `transient_plan@1` のいずれも
materialized 22、transient 0)。**タイル候補でも全資源が実体化される。**
発火には `VK_KHR_dynamic_rendering_local_read` の拡張と feature bit が要る。

**利益は今ゼロ、危険は繰り延べ、発覚は開発機の外。持っていてよい形ではない。**

**決定: `same_pixel` を宣言の語彙から当面外す。**
書けるのは `neighborhood`(+ `radius`)と `arbitrary` だけ。
**モバイルを載せると決まってから開ける。**

**未完了(重要)**: **扉は 2 枚ある。**
新しいヘッダの語彙を閉じても、**既存の JSON 鍵は今も `same_pixel` を受け付ける**
(`input_footprints` と `material_resources.footprint` の両方)。
出荷資産では誰も書いていないが、**危険は今日すでに開いている。**
**塞ぐなら 2 枚とも塞ぐこと。**

**開けるときの条件(どちらかを満たすこと):**

- **機械的に反証できること。**推論より反証のほうが易しい ——
  上限を求める必要はなく、**非ゼロのオフセットを 1 つ見つければ嘘だと分かる。**
  `ssao_blur` の定数ループ(`-2..2` の二重ループ × `1/textureSize`)は自明に反証できる。
  再投影(`ssao.frag` は毎フレームの射影行列と、別テクスチャから読んだ法線に依存)は
  反証も証明もできないので `arbitrary` のまま
- **アクセサを分けること(本命)** ——
  同一ピクセル読みとオフセット読みで**シェーダー側の関数を別にする。**
  オフセット版を呼んだシェーダーは**構造的に local read へ落とせなくなる。**
  宣言を信じる代わりに、**シェーダーが選んだアクセサが宣言になる**

**後者は業界の一般解でもある** ——
Vulkan の `subpassLoad()` も Metal の `[[color(n)]]` 入力も**座標引数を持たない。**
UE の subpass fetch、Unity URP の framebuffer fetch マクロも同様である。
**プラットフォームは元々この安全性を持っており、
`PELICAN_TEXTURE_2D_n(value, uv)` という統一アクセサがそれを消した。**

#### footprint 宣言は、`same_pixel` を外すと何も変えない(実測・重要)

**`footprint.kind` で分岐している箇所を全数調べた。**すべてが次のどれかである:

| 種別 | 内容 |
|---|---|
| **宣言自体の検証** | read には必須 / write には禁止 / radius は neighborhood 専用 / radius > 0 |
| **`same_pixel` の判定** | tile-local 適格性、scope fusion、`nodeHasSamePixelRead` |
| **`temporal` の扱い** | `@history`、aliasable、`epoch::previous` |
| **表示・直列化** | 診断文字列、Studio の表示 |

**`neighborhood` と `arbitrary` を挙動として区別する分岐は 1 つも無い。**
**`radius` を消費している箇所も無い**(検証以外では読まれない)。

WP336 の実測がそれを裏付けている —— フレームプランの差分 16 件のうち、
**2 件が footprint の値そのもの、4 件が fingerprint、3 件が `widest_read`、
4 件が診断文字列**であり、**それ以外は 1 つも動かなかった。**

**したがって `same_pixel` を外した時点で、footprint 宣言は挙動を変えない。**

**この事実を、次に footprint を扱う WP の前提に置くこと。**
**「安全だから入れてよい」ではなく「効果が無いなら要らない」で判断する。**

#### 段階の順序を入れ替える(2026-08-23)

**footprint を最初の縦切りに選んだのは誤りだった。**
5 案の評価で「単独出荷でき、受け入れ条件が書け、二重機構にならない」を最適化した結果、
**利用者の要求からいちばん遠いものが残った。**

利用者の要求(「型にエラーがなければ動く」)に要るのは
**契約・省略可能性・既定値**であり、**footprint はそれと直交している。**

**次の縦切りは段階 3(既定値)から取ること。**
`sprite_demo` / `vrm_xr_demo` の `ssao_clear` 回避策が消えるところまでが射程である。
**WP336 の土台(共通 tokenizer / 新文書種 / graph parse 前の resolver)は
後続で要るので、要るときに一緒に入れる。**

#### 条件付きソケットと feature 所有ソケット

**これがこの設計線全体の門である(5 案の評価で確定)。**
`fullscreen.frag` に触る案は **3 つともここで死んだ**
(WP336 初版 / 案 D「既存鍵で footprint」/ 案 E「著作時判定」)。

`appendFullscreenSurfaceResource` が **`shadow_map` を `input` 配列へ追記する**ので、
合成後の image 入力数は **shadow 有効で 7、無効で 6** に揺れる。
**6 固定の宣言表は `animgraph_demo` と `project init` を落とし、
7 固定は残り 3 プロジェクトを落とす。**

**したがって、条件付きソケットの表現が決まるまで
`fullscreen.frag` は移行できない。**これを順序の前提として明記すること。
向こう側の情報(`surface_resource_contracts` / `shader_defines`)は
**`parsePassDefinitionFromJson` にも `evaluatePassShape` にも渡っていない。**

**静的なヘッダだけでは足りない。**

`fullscreen.frag` の binding 6 は `#ifdef PELICAN_FEATURE_SHADOW` の中にあり、
**出荷構成に両方の variant が存在する**
(shadow 有効: `animgraph_demo` / `project init`、
無効: `example` / `sprite_demo` / `vrm_xr_demo`)。

**ヘッダに書けば無効側で「宣言にあるが SPIR-V に無い」、
書かなければ有効側で「SPIR-V にあるが宣言に無い」になる。**

**`.surface` が既に解いている** —— 宣言済みが先頭を占め、feature 所有が後ろに付く。

**決定: 照合の対象は「有効 interface」である** ——
**ヘッダ所有のソケットと feature 所有の契約を合成したもの。**
**「SPIR-V の余り」は「ヘッダに無い」ではなく「合成後の interface に無い」と定義する。**

#### v1 の形

**インライン写像は 1 行に収めること**(現文法は行をまたげない)。

```glsl
//! pelican.fullscreen v1
//! language: glsl
//! sockets:
//!   - { name: albedoSampler, contract: gbuffer_albedo, footprint: same_pixel, stage: fragment }
//!   - { name: ssaoSampler, contract: ssao, footprint: same_pixel, stage: fragment }
//! outputs:
//!   - { name: outColor, type: color }
//! BEGIN GENERATED
...
//! END GENERATED
```

`ssao_blur` は
`- { name: ssaoInput, contract: ssao, footprint: neighborhood, radius: 2, stage: fragment }`。

**`optional` / `default` は v1 に書かない。**既定値は後続の WP であり、
**書けると誤解させないこと。**

#### 組み込み契約が足りない(v1 で足すもの)

**現在の 5 つ**(`opaque_color` / `opaque_depth` / `scene_depth` /
`linear_view_depth` / `directional_shadow`)**では、
engine シェーダー 7 本の 15 入力を名指せない。**

意味どおり名指せるのは 3 つだけである
(`sceneColorSampler` → `opaque_color`、`sceneDepthSampler` → `scene_depth`、
`shadowMapSampler` → `directional_shadow`)。

**足りない契約(最低限):**
`gbuffer_albedo` / `gbuffer_normal` / `gbuffer_material` /
`gbuffer_world_position` / `gbuffer_emissive` / `ssao` / `display_linear`。

**`gbuffer_normal` と `gbuffer_world_position` は `scene_color` と同じ
`R16G16B16A16_SFLOAT` である。**format では識別できない。
**producer 側の semantic annotation が要る** ——
現行の format → 論理型の写像は深度と `R16G16B16A16_SFLOAT` しか扱わず、
後者を一律 scene-linear color にしている。

**これは engine-only の bootstrap である。**
**利用者が「自在に定義」するには C++ 追加方式では足りない。**
後段に登録可能な型参照文法か契約 registry を置くこと。**そう明記しておく。**


#### 文法の制約(`//!` パーサから逐語)

**同じパーサに載せる以上、次は動かせない:**

- **1 行目は magic 行の完全一致。**版は magic 行が持つ(`schema` 鍵は存在しない)
- `//!` で始まらない行が 1 本出た時点で**ヘッダは終わり、残り全部が code**。
  **code の途中に宣言ブロックは置けない**
- **index 3 は ASCII U+0020 の完全一致である**(一般の whitespace ではない。`//!` の直後がタブの行は拒否される)。
  内容は `trim(substr(4))` なので**インデントは意味を持たない**
- 行は「リスト項目」か「`key: value`」の 2 種のみ。
  **リスト判定は「trim 後の先頭文字が `-`」だけで、直後の空白は任意である。**
  `- ` を必須にすると、今日警告して黙殺されている `//! -legacy_item` が
  **`key:value` と解釈されて throw に変わる。**挙動を変えないこと
  **`key:` 行が来るたびセクションが none に戻る**ので、
  リスト項目はセクション鍵の直後に連続していなければならない
- 鍵と値の分割は**最初の `:`**。括弧も引用符も見ない。**鍵名に `:` を含められない**
- 名前は識別子規則(先頭が英字か `_`、以降は英数字か `_`)。
  **`.` や `@` を含む契約名をソケット名にはできない**
- 階層は**インライン写像の入れ子でしか表現できない**

#### 版の扱いは engine 先行にする(重要)

**未知の鍵は拒否されない。警告して無視する。**
しかも**値が空だとセクションが `unknown` になり、続くリスト項目まで黙って捨てられる。**
**そして `warnings` を production で読むコードは存在しない。**

**つまり旧エンジンが新形式を開くと、エラーにならずソケットが黙って消える。**

**ただし `.frag` については、それでも旧エンジンは落ちない(仕様レビューで確定)。**
`.frag` は今日ヘッダを持たないので、
**旧エンジンは `//!` をただの GLSL コメントとして glslang へ渡すだけ**であり、
OFF 構成は raw source を読まない。

**したがって版の gate は `project.json` の `engine_min_version` を使う。**
旧エンジンを明示的に落とす経路として**既に存在する。**
**`warnings` で誘導する設計は現状無効である。**警告の消費経路を先に作らない限り採らない。

これは `project-format-subset-principle`(形式拡張は常にエンジン先行)そのものである。

#### 接続側に残る鍵

**宣言を `//!` ヘッダへ移したとき、パス JSON に残るのは:**

- **`resource`**(`@history` 修飾を含む)
- **`subresource`**(どの mip / layer か)
- **`access`** —— 同一資源を複数 port に張る/読み書き両方に現れるときの曖昧性解消のみ
- **`output` の target 名**

**これは `resource_ports` の文字列略記 `"port": "resource"` が運ぶ情報と、
`screen_inputs` の `{契約名: RT 名}` が運ぶ情報にほぼ一致する。**
**新しい接続構文を発明しなくてよい。**

#### Studio 側 —— 配管は既にある

- **`get_render_authoring_context` は著作パス JSON を丸ごと返している。**
  `resource_ports` を含む全鍵が既に Studio に届いている
- 欠けているのは **型付き DTO と論理契約の publish** である。
  現在 `FullscreenPassWidget` の draft は `name` / `type` / `input` / `output` / `shader` の
  5 鍵しか作らず、残りは「omitted keys」として名前だけ申告している
- **`get_frame_plan` の node は平坦な資源名配列で、port 名も型も無い。**
  resource 側の `surface_resource_contracts[]` は `sampling` / `view_policy` / `fallback` を
  運んでいるが、**Studio は `source_type` / `sampled_type` / `footprint` / `relation` を
  復号せず捨てている**
- **`pelican_project` は Vulkan にリンクしない**ので、
  `MaterialPassInputContract` / `LogicalType` / `ShaderResourcePortDefinition` は
  **そのまま Studio から呼べる。**
  逆に `ShaderResourceInterfaceBinding` は `vulkan/vulkan.hpp` を直接 include する
  core 側にあり、**Studio は永久に触れない**

**したがって段階 1 の D0 作業は「新設」ではなく
「既に届いている情報を型付きで publish する」である。**

#### v1 の範囲外(明示すること)

- **論理型を綴る文法**(パーサ新設が要る)
- **任意の `conversion` id**(適用する汎用経路が無い)
- **compute への適用**(compute はパス型ではなく、著作コンテキストが届いていない)
- **8 本を超える入力**(`PELICAN_DECLARE_INPUT_0..7` の 8 枠しかなく、
  fullscreen 経路に上限検査が無い)
- **XR / multiview の variant を焼く**(WP211 相当)

### 2. 位置による束縛をやめる —— WP211 は前提ではない(訂正)

**第 4 版初稿は「名前束縛は WP211 dist-bake を前提とする」と書いた。誤りだった。**
測って崩れた。

`hybrid_v1` の `deferred_lighting` は `input` に 6 件を並べ、**6 件目が `ssao_blur`**。
`engine://fullscreen` は `PELICAN_DECLARE_INPUT_5(ssaoSampler)` と**位置 5 番で**宣言する。
**配列の順序だけが両者を結んでいる。**
**「このスロットは AO である」と書いてある場所はどこにも無い。**

#### 名前は既にシェーダーの中にある

**焼いた `src/core/resources/fullscreen.frag.spv` の中身**(実測):

```
albedoSampler  normalSampler  materialSampler
worldPosSampler  emissiveSampler  ssaoSampler
```

- engine シェーダーは**ビルド時に glslang で `.spv` へ焼いて埋め込んでいる**
- reflection は `spirv-reflect` で、`binding.name` を読んでいる
- **変数名から役目を決める先例が既に出荷されている** ——
  `materialscreeninput.cpp` が `binding.name == directionalShadowSamplerName` で
  builtin contract を組み立てている

**したがって名前束縛に生成は要らない。読めば済む。焼いた `.spv` に対して動く。**

現在の `resource_ports`(fullscreen)が virtual include を生成しているのは
**実装の選択であって必要条件ではない** ——
`registerFullscreenShaders` は `resource_interface` が空なら生成しない分岐を既に持つ。

**WP211 dist-bake は前提から外す。**

#### ただし reflection 単独を正本にはしない

**理由 2 つ:**

1. **`OpName` はデバッグ情報である。**`-g0` が入れば消え、ソケットが無名になる
2. **マクロが 3 変種を持つ** —— `PELICAN_DECLARE_INPUT_5` はビルド定義で
   `subpassInput` / `sampler2DArray` / `sampler2D` に分かれる。
   **reflection は「今回焼かれた形」しか言えず、「あるべき形」は言えない**

**正本はシェーダー脇の宣言、reflection はその照合。**
`shaderresourceinterface.cpp` が既にその向き
(engine が期待 binding を組み、reflection と突き合わせる)で書かれている。

#### 移行の規模

`PELICAN_DECLARE_INPUT` を使う engine シェーダーは **7 本**
(`fullscreen` / `output_transform` / `rt_shadow_mask` / `scene_present` /
`sky_ambient` / `ssao` / `ssao_blur`)。
**マクロは既に名前を受け取っている。**足すのは役目の宣言だけである。

#### 出力側にも同じ位置束縛がある(第 4 版初稿の欠落)

`layout(location = 0) out vec4 outColor` ↔ `output.color[0]`。
**入力とまったく同じ構図である。**

engine の fullscreen シェーダーは**全部が出力 1 本**なので今は表面化しない。
**しかし MRT は既にある** —— G-buffer パスは
`gbuffer_albedo` / `gbuffer_normal` / `gbuffer_material` /
`gbuffer_worldpos` / `g_emissive` の 5 枚を書く。
これは `material` 型なので契約が供給しているが、
**ノードから 2 出力の fullscreen パスを組んだ瞬間に位置束縛が効く。**

**出力も同じ宣言に載せること。**入力だけ名前にして出力を位置のまま残さない。

### 2.1 逆方向 —— ノードから入出力テンプレを生成する(利用者の要望)

> **逆にノード側で組んだら input、output のテンプレを生成する機能もあると嬉しい**

**§2 が「シェーダー → エディタ」なら、これは「エディタ → シェーダー」である。**
**両方要る。**キャンバスで 0 から組むとき、
シェーダーを手で書き起こすところで止まってしまう。

**初稿は「`//!` ヘッダのブロックだけを管理領域にする」と書いた。成立しない。**
レビューで潰れ、検算でも確認した。以下は訂正済みの版である。

#### 初稿が破綻した理由(繰り返さないこと)

**1. ヘッダだけ更新してもインターフェースが増えない。**
`ao` の次に `normal` を足したとき、`//! sockets:` に一行足しても
**実際に効くのは `PELICAN_DECLARE_INPUT_n(normal)` の方**であり、
それは管理範囲の外にある。SPIR-V に binding は増えない。
しかも現行 validator は**宣言した binding が SPIR-V に無ければ拒否する**ので、
ヘッダだけ進むと即座に落ちる。

**2. 例が自分の設計に違反していた。**
初稿は `//! outputs: - { name: outColor, location: 0, resource: ssao_output }` と書いた。
**`resource` は接続であって契約ではない。**
§1 で「契約はシェーダー、接続はパスインスタンス」と決めておきながら、
接続をシェーダーに埋めていた。

#### 管理単位は prologue —— ヘッダではない

**メタデータ・入力宣言・出力宣言をまとめて一つの managed prologue にする。**
**明示的な begin / end 番兵で範囲を確定し、その後だけを利用者の本体とする。**

```glsl
//! pelican.fullscreen v1
//! pelican_editor_managed
//! sockets:
//!   - { name: ao, contract: ambient_occlusion_v1, optional: true, default: white }
//! outputs:
//!   - { name: outColor }
//! BEGIN GENERATED
PELICAN_DECLARE_INPUT_5(ao)
layout(location = 0) out vec4 outColor;
//! END GENERATED

void main() {
    outColor = vec4(1.0);
}
```

**代案**: 固定の `#include <pelican_generated_interface.glsl>` を prologue に置き、
宣言をそこから生成する。**番兵方式より境界が硬い。**
`AuthoredRenderConfigDocument` の「一つの字句範囲だけ書き換える」技法は
**この番兵の間にだけ適用する** —— GLSL には JSON のような構文境界が無いので、
範囲は番兵が作る。

**`binding` / `location` を宣言に書かない。**
**物理 ABI は導出して reflection と照合する**(下記)。

#### 出力の接続はパス側に置く

**現在の `output.color` は target 名の列であり、ソケット名を持てない。**
パーサが受け取るのは `target` と `subresource` だけで、
それ以外の鍵は「unknown field」で落ちる。
**つまり初稿の受け入れ条件「`output.color` の並びを入れ替えても正しい target に書かれる」は、
今日は書きようがない。**

**新しい形を正本にする:**

```json
"output": {
  "color": [
    {"socket": "outAlbedo", "target": "target_a"},
    {"socket": "outMask",   "target": "target_b"}
  ],
  "depth": null
}
```

**さらに fullscreen は pass-shape policy が color 出力を最大 1 に固定している。**
バックエンドは複数 format を受けられるので、**制限は policy 側にある。**
**新機構は fullscreen に限定し、material の `MaterialOutputSchema` は変更しない。**

**訂正**: material の 5 枚は上限ではない ——
`pelican.material_outputs` schema を省いたときの**互換既定**であり、
明示 schema は任意数を扱う。既存テストに 7 出力の fixture がある。
**回帰対照は 5 枚だけでなく 7 出力 fixture も含めること。**

#### 契約の schema は logical と physical を分ける

**初稿の `type: sampler2d` では足りない。**
`opaque_depth` と `linear_view_depth` を区別できず、
same-pixel と neighborhood も、変換の要否も表せない。
**それでは「型にエラーがなければ動く」ノードにならない。**

さらに `PELICAN_DECLARE_INPUT_n` は**同じソケットから 3 形態を生成する**
(`subpassInput` / `sampler2DArray` / `sampler2D`)。
**固定の `sampler2d` を reflection と一致させる規則は存在しない。**

**したがって:**

- **logical socket** —— 型 / footprint / view policy / fallback / 変換。
  正本は既存の `MaterialPassInputContract`
- **physical ABI** —— binding / location / descriptor 種別 / 次元。
  **flat・multiview・local-read の物理 plan から導出**して reflection と照合する

#### 契約の照合と source 生成は別の channel にする

**これが §2 の「生成は要らない」を実装で守るための要点である。**

現行の `resource_interface` をそのまま契約に流用すると、
**非空であるだけで virtual include を生成する。**
そして **`.spv` と virtual include の組み合わせは明示的に拒否される。**
**つまり流用した瞬間に OFF ビルドが壊れる。**

**照合用の契約と、source 生成用の virtual include を、別の経路に分けること。**

#### 名前の scope

**ソケット名はシェーダーをまたいで重複する** ——
`worldPosSampler` と `normalSampler` は 3 本、`outColor` は 7 本すべてにある。
**重複自体は正常。registry の鍵をソケット名だけにできない。**
**正規化した shader stem・stage・宣言 digest で scope すること。**

#### 新規シェーダーを commit 前にコンパイルできない(未解決)

**現行の候補文書集合は render-config 文書しか差し替えられない。**
`ShaderLibrary` は project シェーダーを候補集合ではなく**実ファイルから読む。**

したがって新規 `.frag` は二択になる:

- **preflight 前に disk へ書く** → コンパイル失敗時に壊れたファイルが残る
- **commit まで書かない** → `ShaderLibrary` から見えず preflight が file-not-found

**これは WP334b が render-config で解いた問題の再演である。**
**同じ解を shader source へ広げること** ——
候補集合を shader にも広げ、`ShaderLibrary` / `PathResolver` が
request-local な source overlay を読む経路を設ける。
相対 include 解決のために論理 path も保持し、
**コンパイル・reflection・pipeline 作成がすべて成功してから
config と shader を同一トランザクションで commit する。**

#### 置き場所と purge

**`project://shaders/authoring/` + `pelican_editor_managed` マーカーの二重ゲート。**
ただし**そのまま流用すると壊れる。**

**1. 本体は利用者のものなのに、ファイル全体が purge 対象になる。**
利用者が数百行書いたあと最後の参照を消すと、その本体ごと消える。

**2. 外部編集の検出が誤爆する。**
既存の管理 fragment は外部編集を検出すると削除を拒否する。
**シェーダー本体の正当な編集は毎回それに当たる。**
かといって digest 検査を外すと利用者の本体を消す。

**3. ファイル名が `(graph, name)` の graph を落としている。**
`main/blur` と `preview/blur` が衝突する。
既存の fragment は `(graph, pass)` を NUL 区切りで hash している。

**4. 参照の探索が流用できない。**
既存実装は root の `features[]` だけを起点にする。
**シェーダーは拡張子なしの stem で参照される。**

**したがって:**

- **ファイル名は `(graph, name)` の決定的 hash にする**
- **managed prologue と利用者 body の digest を分ける**
- **body が編集されたファイルは「adopted」とし、自動 purge しない。**明示確認を要求する
- **到達可能性は fullscreen / raster / compute / feature / preset を通した
  正規化 shader stem 単位で数える**
- 復旧操作として「新しい sibling に雛形を生成」「明示的に body を置換し backup」を提供する。
  **現在の hot reload はプロセス内の旧 bundle を保つが、
  再起動後の壊れた disk source は救えない**

#### OFF ビルドでの扱い

**生成した `.frag` は source なので OFF ビルドではコンパイルできない。**
これは今日の利用者シェーダー
(`example` の `.surface` 3 本、`sprite_demo` / `vrm_xr_demo` の `.frag` / `.vert`)と
**同じ制約であって、新しい問題ではない。**

**ただしエディタは黙ってはいけない** ——
生成した時点で「この構成は OFF ビルドでは出荷できない」と示すこと。

**なお §2 の「WP211 は前提ではない」は、
source と基底 SPV の双方が埋め込まれている engine シェーダー 7 本の移行に限った話である。**
**project の SPV-only 配布と define variant の binding manifest には、依然 WP211 相当が要る。**

### 3. 既定値は shader を触らずに engine が書く

**第 3 版は「ソケットの属性」としか書いていなかった。**
検証で解の形が確定した。**material 側に既に動いているものがある。**

`CustomTextureBinding::missing_default` → `MaterialDummyTexture::white` → `tex_white`。
**engine が同じ binding へ既定 descriptor を書く。**

**この形なら SPIR-V にも virtual include にも触れないので、
compiler-OFF でもそのまま成立する。**AO は白 = 1.0 で意味も合う。

- **SSAO を外す** → AO ソケットに engine が白を束縛して動く
- **既定値を宣言していないソケットを空にする** → **ソケット名付きで落ちる**
  (material 経路は今日これができている)

**制約(WP 化のときに解くこと):**

- dummy が 2D にしか無い。cube / array / 3D / buffer が要る
- material の default は**名前推論の 3 択**である。
  **port の default は名前推論ではなく明示宣言にすること**
- `shaderresourceport.cpp` の未知フィールド拒否に `optional` / `default` を足す
- **`resource` が `reads`/`writes` に必須という検査を緩め、
  「接続を持たない port 宣言」を表現可能にすること** ——
  これが無いと「接続を消すと既定値の宣言も消える」がそのまま残る

**なお現状は、入力を 1 つ減らすと binding が layout に残ったまま
descriptor が書かれない**(layout は reflection から生成され、
write は接続数ちょうど、検査は `input_count` 未満しか回らない)。
**「コンパイルが通る」を「動く」の証拠にしないこと。**これは ON ビルドでも起きる。

### 4. 順序 —— resource ごとの参加者列

第 3 版は「順序はターゲット(resource)が持つ」と書き、
レビューは「reader が読む値が決まらないので破綻」と判定した。
**検証の結論は「一部正しい」である。**

#### レビューが叩いた形は、設計が提案した形ではない

レビューは「resource 上で `A → B` とだけ宣言しても、
R が A と B のどちらを読むか決まらない」と書いた。
**しかし設計の例は writer だけの列ではない** ——
`__snapshot_opaque_color` という **reader を writer の間に挟んだ列**である。

**正しい定式化を逐語で置く:**

> **resource は「参加者の列」を持つ。writer と reader を同じ列に置く。**
> **writer を通ると版が上がり、reader は列上の直前の版に束縛される。**

#### この意味論は、論理 IR に既に存在する

- `LogicalValueId { resource, version }` が既にある
- `deriveLogicalDataEdges` は `(resource, version)` の **exact producer** に辺を張り、
  多重 producer は「logical value has multiple producers」、
  未生産は「logical input value has no producer or import」で落ちる
- version は書き込みごとに `++` される

**したがって「設計として不可能」ではない。表現できないのは IR ではない。**

**表現できていないのは 2 つだけ:**

1. **著作形式**(ノードごとの平坦な `reads` / `writes`)
2. **planner の `last_writer`**

しかも上記の論理層は現在**影(shadow)**であり、
既に配列順が決めた `FrameGraphDefinition` から**導出**されている。
呼ばれているのは target plan の 2 箇所だけで、実行辺は `buildEdges` が作る。
**要るのは新 IR ではなく、この影を正本へ昇格させることである。**

#### 配列順の荷重点は 3 箇所(第 3 版は 2 と書いた。誤り)

1. **`buildEdges`** —— `last_writer` を配列順に走らせ、
   **先行**する writer からしか read-after-write 辺を張らない
2. **`topologicalOrder`** —— ready 集合を `declaration_index`(配列位置)で整列する
3. **`enforceCanonicalOrder`**(第 3 版もレビューも挙げていなかった)——
   通常パスに対し**配列上の直前パスへの明示 `after` 辺**を打ち、
   **合成時点で配列順を辺として焼き込む**

**3 が最も早く効く。**そして**これが負の対照を壊していた** ——
第 3 版が書いた「配列を並べ替えても同じフレームプランになること」は、
**この関数を外さない限り成立しない。**受け入れ条件を書き直した(下記)。

#### resource に載らない順序は残す

**canonical anchor と `output_transform` の順序辺は resource を持たない。**
`enforceTerminalAfterComputeTasks` は `output_transform` を
**共有 resource の有無に関係なく**全 compute task の後に置く。

**したがって `before` / `after` を全廃してはならない。**
**非データ依存の channel として残すこと。**
第 3 版はこれを挙げていなかった。

#### 弱かった反証(採らない)

- **`snapshot_after`** —— これは「`lit_color` の writer N と N+1 の間でこの reader を止める」を
  パス名で書いているだけで、**resource 側の参加者列が包含する。**移行先がある
- **複数 graph 共有** —— パーサの潜在能力ではあるが、
  出荷 4 プロジェクトと `hybrid_v1` はすべて `rendering_passes` が 1 で
  top-level `compute_tasks` が 0。しかも feature 合成は
  「`insert` には `rendering_passes` がちょうど 1 つ」を要求する。
  **feature 合成経路では複数 graph が成立しない。**段階 4 の範囲外と宣言する

#### 複数 writer の曖昧さは既に名前付きで落ちている

`validateWritesAreOrdered` は
`Ambiguous writes-writes dependency for resource ... between A and B` を投げる。
**新しい検査を足す話ではない。**
現在これを通っているのは、**配列順が暗黙に到達路を与えているから**である。

### 5. `reads` / `writes` はソケットから導出できない(第 3 版の一文を撤回)

**第 3 版は「`input` / `reads` / `writes` はソケットから導出されるようにする
(逆ではない)」と書いた。撤回する。**

**shader descriptor ではない resource use が 5 類型ある**(検証で全数):

| 類型 | 由来 |
|---|---|
| A | `dispatch.indirect.buffer` —— engine が read edge を足す |
| B | `gpu_draw_source` の `commands` / `count` —— **2 箇所に重複実装** |
| C | `snapshot_copy` の source / destination —— **shader が存在しない** |
| D | attachment の `load_op == load` による**暗黙 read** |
| E | output attachment の `writes` と attachment 定義の**二重表現** |

**D と C は出荷資産で日常的に発火する**
(`load_op: "load"` は `ui` / `hdr` / `debug_draw` / `debug_text` / `gizmo` / `sky_ambient`、
`snapshot_copy` は `cube_capture` / `planar_reflection` / `example`)。
A は現状テスト資産のみ。

**受け皿は既にある。**`LogicalResourceUse` と
`LogicalAccessIntent { automatic, sampled, attachment, storage, transfer, host }`。
attachment と transfer は既に推論されている。
**足りないのは `indirect` の 1 値と、著作層が論理層と別建てで
手書きリストを持っている二重管理だけである。**

### 6. preset を編集可能にする

**検証の結果、第 3 版の記述は大半が正しく、欠けているのは 1 点だった。**

**既にできていること(レビューの「触れない」は過大):**

- 許可鍵に `features` があり、preset 解決は feature 適用より**前**に走るので、
  **feature 経由で preset のパスへ加算的に届く**(commit 83d9c52 で出荷済み)
- `render_target_overrides` は preset 供給の render target の
  `format` / `usage` / `format_candidates` / `width` / `height` を**上書き代入する**

**無いのは「既存 preset パスの書き換え・削除」である。**

**欠けている 1 点(逐語で決めること):**

> **名指し override をどの文書のどの鍵に置くのか。**
> root の新鍵か、feature の新フィールドか、`passes/authoring/` 下の管理 fragment か。

**purge の条件を強める** —— 現状は消えた preset パスを名指す著作 feature が
`insertPassByAnchor` / `applyPassOverrides` で throw し、**プロジェクトが読めなくなる。**

> **preset を外したあとロードが成功し、
> 著作物のうち preset 依存だったものだけが名前付きで落ちること。**

**origin の欠落は `appendUniqueArray` ではない**(第 3 版の誤帰属)。
preset の pass は `config` ごと丸ごと入り、`appendUniqueArray` は
`features` / `shader_defines` / `graph_transforms` しか触らない。
**実際の原因は `RenderPipelineProvenanceSource` に preset が無いことと、
合成が preset 解決後の基底 config を丸ごと project と印付けしていることである。**

### 落としてはならないもの

- **purgeable**(利用者の明示指示)
- **provenance が `(graph, name)` を持つこと**。現在は `name` だけ
- **`material_resources` の footprint と `@history`**
- **非 shader use の 5 類型**(§5)
- **`shader_defines` は設定ではない。**全 shader compile に伝播する
- **`draw_sort.xr_view_policy` は provider 選択ではない。**queue 数を 1↔2 に変える
- **swapchain と `output_transform` はエンジンの canonical 層が所有する**
- **swapchain の role による判定**(入力・深度出力として不正、色出力として正当)
- **canonical anchor 8 個は消さない。**選択子の実例として使う

### 段階 —— 1〜3 は着手可、4 は設計が残っている

**利用者は「段階を踏まず直接直してよい」と言った。**
ただし出荷 4 プロジェクトと `project init` を壊さないことは絶対条件である。

**第 4 版初稿の段階表は依存関係を誤っていた**(段階 4 が「段階 1 だけに依存」)。訂正済み。

```
着手可:
  1. 契約の軸を立てる。**schema は §1.1 で確定済み**
     —— `.frag` / `.comp` 用の新しい文書種を立てる
        (`.surface` は material 専用で届かない。文法とパーサだけ共有する)
     —— v1 は型を綴らず、組み込み契約を名前で呼ぶ
        (LogicalType に JSON パーサが存在しない。最初の reader になってはいけない)
     —— binding を「パスの input 配列の序数」から「シェーダーの宣言順」へ移す
     —— footprint の 3 経路(material_resources / input_footprints / read_footprints)を畳む
     —— engine シェーダー 7 本に宣言を足す
        (ssao_blur は neighborhood radius 2。現在は誰も書いておらず arbitrary 扱い)
     —— 版は engine 先行で落とす。**未知の鍵は拒否されず、警告は誰も読んでいない**
     —— Studio は core の reflection を読めない(D0)が、
        **配管は既にある** —— get_render_authoring_context がパス JSON を丸ごと返している。
        仕事は型付き DTO と契約の publish であり、新設ではない
     【単独出荷可】内部 invariant として閉じる
     【compute は別配管】compute はパス型ではなく compute_tasks[] であり、
        著作コンテキストも add_authored_pass も届いていない

  2. 接続の軸を名前へ移す(段階 1 に依存)
     —— 照合用の契約と source 生成用 virtual include を別 channel にする
        (resource_interface は非空なだけで include を生成し、.spv と併用すると拒否される)
     —— fullscreen の index 固定 descriptor write を廃する
     —— compute の positional fallback を廃する(今は名前で引いて失敗すると位置に落ちる)
     —— 同一 resource 多重束縛の禁止を契約名付きの多重束縛へ緩める
     —— LogicalAccessIntent に indirect を足し、非 shader use 5 類型を載せる
     —— 「These never create graph edges.」契約を破棄する
     【二重機構を残さないこと】宣言済みシェーダーで旧 positional と新 named の
        両方を許すと、それ自体が WP324〜328 の失敗型になる

  3. 既定値を engine 側 descriptor で実装する(段階 1・2 に依存)
     —— MaterialDummyTexture::missing_default を fullscreen / compute へ横展開
     —— dummy を 2D 以外(cube / array / 3D / buffer)へ広げる
     —— 接続を持たない port 宣言を表現可能にする
     【ここで利用者に届く】sprite_demo / vrm_xr_demo の ssao_clear 回避策が消える

段階 4(テンプレ生成)—— 着手前に決めることが残っている:
     依存は段階 1 だけではない。段階 2・3 に加えて
     —— shader source の候補 overlay(commit 前 preflight)
     —— shader を含む複数文書トランザクション
     —— managed prologue / body の digest 分離と「adopted」の扱い
     —— (graph, name) hash によるファイル名
     —— 正規化 shader stem 単位の到達可能性
     —— fullscreen の color 出力上限 1 を外す pass-shape policy 変更
     —— output.color の {socket, target} schema

  5. 選択子の一般化(段階 2 に依存 —— 名前付き契約を consumer 選択に使うため)

保留(理由を明示する):
  6. 順序を resource の参加者列へ
     【保留理由】canonical anchor と output_transform の非データ依存が
     resource に載らないため、before/after を残す設計が先に要る。
     また enforceCanonicalOrder が合成時点で配列順を after 辺へ焼き込んでおり、
     これを外さない限り負の対照が成立しない
  7. preset パスの名指し override
     【保留理由】override をどの文書のどの鍵に置くかが未決定
```

**段階 1・2 は単独では利用者価値のある変更ではない。内部の作り替えである。**
**利用者に届くのは段階 3 からである。**そう明記すること —— 段階 1 を「v1」と呼ばない。

**段階 6 が来るまで、ノードエディタは「0 から組む」を完全には満たせない**
—— キャンバスには配列が無いのに、順序は配列が持っているためである。

### 別件として切り出すもの(この設計の範囲外)

レビューが `--feature-overlay` 周りで 3 点を指摘した。
**題材は実在する**(`src/player/main.cpp` / `studioplayerarguments.cpp` /
`renderer_config.cpp` / `launchconfig.hpp`)。**本設計とは無関係なので別 WP にする。**

- **overlay 文書が reload watch に入っていない。**
  watch 対象は root・preset・合成後 feature 文書だけで、overlay 参照を含まない
- **CLI の absolute-path policy が overlay loader で失われる。**
  overlay は `resolveProjectRef` を通り、`--allow-absolute-paths` を見る `resolveCliRef` を通らない
- **`launchconfig.hpp` は `std::vector<std::string> render_feature_overlays` なのに、
  `main.cpp` は `get<std::string>` で 1 件しか取っていない**(要確認)

**注意**: レビューはこれらを「別紙 `design_for_codex.md` の指摘」として提示したが、
**そのファイルはリポジトリに存在しない。**題材は実在するので指摘自体は追う価値があるが、
**出典は確認できていない。**起票前に一次確認すること。

### 受け入れ条件の骨子(WP 化するときに逐語で書く)

**第 3 版が書いた条件のうち 3 つ、第 4 版初稿が足した 5 つのうち 4 つが成立しなかった。**
**現行コードで実際に書ける形へ直した。**

- **接続を 1 つ落とすとフレームプランが変わること。**
  **今日 `material_resources` で成立する** ——
  ソケット 1 個 → `reads` 1 本 → barrier という因果が既にテストにある。
  **畳んだ後に同じ対照を `resource_ports` へ広げること。**
  第 3 版の「`resource_ports` の 1 socket を落とすと」は、
  今日は port が辺を作らないので**畳む前には成立しない**
- **SSAO の回避策が消せること。**数はプロジェクトごとに違う ——
  `sprite_demo` と `vrm_xr_demo` は **`ssao_clear` 1 パス + `ssao_blur` 1 ターゲット**、
  `hybrid_v1` は **2 パス + 2 ターゲット**。**同じ文章にまとめないこと。**
  **今日の負の対照は既にある** —— 2 つの demo から消すと
  「Unknown resource reference ... ssao_blur」で落ちる
- **既定値のないソケットを空にするとソケット名付きで落ちること。**
  **material surface 経路では今日書ける** ——
  「does not provide shader input 'X'」を `ContainsSubstring` で検査する
- **配列を並べ替えてもフレームプランが同じであること(意味論射影で比較)。**
  **完全一致ではない。**`declaration_index` と `order` を落とし、
  `nodes` を名前で整列、`levels` と `barriers` を集合化して比較する。
  **既存の dump 形式のままで可能。**
  `after`/`before` だけの順序辺まで見るなら `edges` 配列を足す。
  **前提: `enforceCanonicalOrder` が配列順を `after` 辺へ焼き込むのを外すこと。**
  外さない限りこの対照は成立しない
- **非 shader use が消えないこと** ——
  `gpu_draw_source` を持つ material パスと `dispatch.indirect` を持つ compute task で、
  畳み込み前後に read edge が一致すること。
  **負の対照は「indirect buffer の read edge を落とすと barrier が消えること」**
- **同一 resource への多重束縛が通ること** ——
  `forward_transparent` の 3 契約(`opaque_depth` / `scene_depth` / `linear_view_depth`)が
  畳んだ後も落ちないこと
- **preset を外したあとロードが成功し、preset 依存だったものだけが名前付きで落ちること**
- **provenance が `(graph, name)` を持つこと**

#### 名前束縛(段階 1・2)—— 初稿から書き直した

- **契約のみの経路では virtual include が空であること。**
  そのうえで `ao → set1 / binding5` という**解決した binding 値を実測**する。
  **負の対照は「source 生成経路を通したときだけ OFF で名前付きに落ちること」** ——
  **「OFF で落ちる」を対照にしない。**照合と生成が別 channel であることを示すのが目的である
- **宣言と実物の不一致を拒否すること。**
  名前が読める構成では**誤った `OpName` を拒否**する。
  名前が読めない構成(`-g0` 相当)では
  **binding の不在 / descriptor 種別 / 次元の不一致を拒否**し、
  **解決した binding 値を検査**する。
  **同型・同 binding の「別物」を意味で見分けることは原理的にできない。**そこは要求しない
- **compute の positional fallback が消えていること** ——
  名前で引けなかったときに位置へ落ちないこと。
  **同じテストの中で、宣言済みシェーダーが旧 positional で解決されないこと**
- **回帰対照に material の 7 出力 fixture を含めること**(5 枚は上限ではなく互換既定)

#### 出力の名前付き接続(段階 4 の前提)

- **`{socket, target}` schema が受理されること。**
  現在の parser は `target` / `subresource` 以外を「unknown field」で拒否する
- **2 出力の fullscreen パスが組めること。**
  現在は pass-shape policy が color 出力を最大 1 に固定している。**policy 変更が要る**
- **2 出力が異なる色を書くシェーダーを headless で readback し、
  `output.color` の順序を逆転しても 2 つの target の値が同じであること**
- **material の `MaterialOutputSchema` が変わらないこと**

#### テンプレ生成(段階 4)—— 初稿から書き直した

- **明示操作の前は disk の bytes が完全一致すること。**
  **「黙って再生成すると編集が消えること」を対照にしない** ——
  禁止された実装を本番経路に置くことになる
- **明示操作の後は managed prologue だけが変わり、body の digest が一致すること**
- **コンパイル失敗時はすべてのファイルが未変更であること**
- **ソケットを 1 つ足したとき、`PELICAN_DECLARE_INPUT_n` が実際に増えること** ——
  ヘッダだけ増えて SPIR-V に binding が増えない状態にならないこと

#### 生成シェーダーの purge(段階 4)

**次の 6 ケースを同一テストで走らせること:**

1. 2 つの graph に同名のパス(`main/blur` と `preview/blur`)
2. stem の別名参照
3. 複数箇所から共有参照
4. 最後の参照を消したとき
5. マーカーの無い手書きシェーダー(**消えないこと**)
6. **body を編集済みの管理シェーダー(自動 purge しないこと。明示確認を要求すること)**

- 出荷 4 プロジェクト、`pelican project init`、
  `PELICAN_RUNTIME_SHADER_COMPILER` の ON / OFF(**OFF は「壊れない」の意味**)

**fixture は既にある** —— `animgraph_demo` と `example` の完全な frame plan を作る
ヘルパが `devstudio_frameplan_graph_test.cpp` にある。
**`sprite_demo` / `vrm_xr_demo` の 2 本を同じ形で足すこと。**

### bundle を捨てた理由(第 2 版の破棄。継続)

第 2 版は preset の成員に「bundle slot」という差し替え単位を与えようとした。**要らない。**

- **パスの同一性は既に `(graph, name)` である**
- slot を足すと、**同じものに 2 つ目の識別体系**ができる。
  「同じことをする方法が 2 つある」は WP324〜328 で 4 つの WP を費やした失敗の型である
- **preset のパスに触れないのは、許可鍵の一行が `rendering_passes` を
  禁じているからにすぎない**

**SSAO を実測して確かめた** —— `hybrid_v1` の SSAO の中身は
`type: fullscreen` のパス 2 枚(`engine://ssao` / `engine://ssao_blur`)と
`R8_UNORM` / `extent_scale 1.0` のターゲット 2 枚だけである。
**特別なものは一つも無い。今日のフォームで組めるものそのものである。**

### §3.2(eject)の置き換え範囲(第 2 版から継続)

**論理層の「resolved authoring eject」だけを置き換える。**

**残すもの**: 物理層の eject 三種(`ejectable_pin_package` /
`ejectable_physical_fragment` / `ejectable_complete_physical_plan`)、
および **logical dump の診断**(`dump-resolved-render-pipeline` 等)。
**診断まで失わないこと。**

**なお「合成後は一切書き戻せない」は範囲が広すぎた。**
feature / canonical 合成後の逆変換が非可逆なのは事実だが、
**preset 解決直後の `resolved` は「eject した verbose config と同じ
canonical compiler input」だとコード自身が書いている。**
全体ダンプを通常編集形式に戻す理由にはならないが、
**stale override の復旧・比較用の限定 export 経路まで削除する根拠にはならない。**

## 段階 3 の設計 —— 既定値(2026-08-23)【第 5 版の段階 2 へ。実測は引き続き有効】

**「既定 descriptor を束縛する」という機構は第 5 版で
「定数を返す accessor を生成する」へ置き換わった。**
**ただし `ssao_clear` の実測、三通りの壊れ方、
画素で対照できないこと、golden harness の三つ目のコピーは
この節が正本である。**

**射程は「`ssao_clear` 回避策を削除できること」である。**
利用者の要求「SSAO を外しても `deferred_lighting` が動く」「型にエラーがなければ動く」の実体。

**設計に入る前に player を実際に走らせて測った。**以下はすべて実測である。

### 回避策の正体 —— 絵のためではなかった

**`ssao_clear` は AO=1 を作るためのパスではない。`ssao_blur` に生産者を 1 つ立てるためだけのパスである。**

**否定対照で確かめた**: `white.frag` を `outColor = vec4(0.0)` に焼き替えて
**AO=0 にしても、出力 PNG が byte 一致する**(md5 同一)。
`ao` は `ambient = albedo * ao * ambientRadiance` にしか入らず、
この 2 プロジェクトは `sky_ambient` を持たないので **`ambientRadiance` は `vec3(0)`** である。

**つまり回避策は descriptor ABI を満たすためだけに存在し、絵には一切効いていない。**
書いている値 1.0 も `clear_color` の既定 `eClear` で達成済みで、**描画自体が冗長**である。

`sprite_demo` だけが `raster` 型なのは **WP238b の dogfood** であって描画上の理由は無い。

### 今日 SSAO を消すと起きること(3 通り。うち 1 つは無言)

| 消す範囲 | 結果 |
|---|---|
| `ssao_clear` パスだけ | **落ちる** ——「Pass input target is not produced as an earlier output: ssao_blur in pass: lighting_pass」 |
| + `ssao_blur` ターゲット | **落ちる** ——「logical shadow graph node 'lighting_pass' reads unknown resource 'ssao_blur'」(より早い CPU 位相) |
| + `input` の項目 | **落ちない。exit 0 でフレームが回る** |

**3 番目が問題である。**`binding 5` は reflection 由来なので layout に載るが、
descriptor write は接続数ちょうど 5 件しか出さないので**未書き込みのまま draw される。**

- Debug の validation layer だけが `VUID-vkCmdDraw-None-08114` を出す
  (「Set 1, Binding 5, variable "ssaoSampler" が更新されていない」)
- **engine は `VkDebugUtilsMessenger` を作らないので、この文字列は stdout に流れるだけで止まらない**
- **Release 既定(validation off)では何も出ない**

**「黙って既定値に落ちる経路を作らない」の逆で、今日は「黙って何も無い経路」が既に開いている。**

**設計文書の否定対照の記述は、この 3 通りのうち 1 通りにしか当たっていない。訂正すること。**

### 序数結線には名前検査が無い(実測・別件だが重い)

`lighting_pass` の `input` を並べ替える(`ssao_blur` を先頭にする)だけで、
**exit 0 / エラー 0 件 / VUID 0 件で走り、絵だけが変わった。**
6 つの G-buffer 入力を取り違えても**何も言わない。**

さらに **shadow feature を足して SSAO を消すと、`shadow_map` が index 5 へ繰り上がり**、
`PELICAN_INPUT_5_LAYERED=1` が付いて型が食い違い、
**シェーダーコンパイルが「no matching overloaded function」で落ちる。**

### 障害は「値」ではなく「slot」である

**既定値の語彙は既にある**(`MaterialDummyTexture{white, flat_normal, black}`)。
**欠けているのは 3 つ:**

1. **`input` が文字列配列で、空き slot を表せない。**エントリが文字列でなければ即例外
2. **`PassDefinition::input_targets` が `GlobalRenderTargetId` の密な配列**で、
   空きの表現が無い。**12 ファイル・104 箇所が位置で舐めており**、
   barrier 遷移と validation もそこに乗っている。**ここがこの縦切りの実体積である**
3. **白の image view を core の fullscreen 経路から取る production API が無い**
   (あるのは `textureViewsForTesting` だけ)。
   `FullscreenPassContainer` は sampler と `RenderTargetImageViewResolver` しか持たず、
   `TextureContainer` / `StandardMaterialResource` への経路が無い

**そして「binding 5 が未接続だ」と気づく検査が engine に一つも無い。**
`requireInputBindings` は `0..input_count-1` しか回らず、
`validateShaderResourceInterfaceReflection` は宣言 → reflection の一方向である。

### 前提の要否(実測による判定)

| | 判定 | 根拠 |
|---|---|---|
| **ソケット宣言(ヘッダ)** | **要る** | ただし「白」を知るためではなく、**「slot がある / 省略可」を宣言する場所**として。`engine://fullscreen` は **4 つのパスインスタンスが共有**するので、接続側に書くと 4 重になる |
| **binding を宣言順にする(段階 2)** | **要らない** | 必要なのは**番号の付け替えではなく slot の保存**である。descriptor write は `input_rts` の添字そのものなので、**slot さえ残れば番号は動かない** |
| **接続を持たない port 宣言** | **要る** | ただし `shaderresourceport.cpp` の検査は本命ではない —— fullscreen の `resource_ports` は `input` の注釈にすぎず、binding も添字固定。**塞いでいるのは `input` の schema と `input_targets`** |
| **組み込み契約の追加** | **要らない** | `default: white` を直接書けば足りる。ただし**「省略可か」の 1 bit は要る** |
| **2D 以外のダミー** | **要らない** | この射程では 2D で足りる |
| **`fullscreen.frag` の条件付き shadow ソケット** | **この縦切りの最大の門** | 下記 |

**段階 2(名前束縛)が前提から外れたのは実測の成果である。**
設計の段階表は「3 は 1・2 に依存」と書いていたが、**2 は要らない。**

### 最大の門 —— 条件付き shadow ソケット

**`//!` ヘッダのトークナイザはプリプロセッサを解釈しない。**
一方 `fullscreen.frag` の 7 本目は `#ifdef PELICAN_FEATURE_SHADOW` の下にある。

**したがって `fullscreen.frag` にヘッダを足した瞬間、
shadow 有り(`animgraph_demo` は 7 入力)と shadow 無し(他 3 プロジェクト)を
1 つの宣言リストで満たせない。**

**先送りできない。**この WP の中でどちらかを決めること:

- **socket 6 を無条件宣言 + 既定値にして `#ifdef` を宣言から外す**(推奨)——
  shadow が無い構成では既定値が入る。**まさに本 WP が作る機構で解ける**
- ヘッダに feature 条件の文法を足す —— 文法が増える

### 保留中の WP336 をどう取り込むか —— 土台として使うのではなく、検査を反転させる

**ブランチ `agent/wp336` はそのままでは合流できない。**

- **resolver が「socket 数 == `input` 数」の完全一致を強制している。**
  **省略された socket は定義上エラーになる。**
  この検査を**「各 socket は接続を持つか、既定値を持つか」へ反転させる**改修が要る
- **socket 宣言に省略可能性・既定値・view 方針の欄が無い。**schema の拡張が先に要る

**つまり段階 3 は WP336 を「載せる」のではなく「作り替えて取り込む」。**

### 受け入れ条件の骨子(WP 化するときに逐語で書く)

**この節は 6 回「成立しない受け入れ条件」で差し戻されている。実測に基づいて書く。**

- **`ssao_clear` パスと `ssao_blur` ターゲットが `sprite_demo` / `vrm_xr_demo` から消え、
  ロードが成功し、フレームが回ること**
- **`binding 5` に既定 descriptor が実際に束縛されたことを検査すること。**
  **画素比較を対照にしてはならない** —— この 2 プロジェクトでは
  `ambientRadiance` が `vec3(0)` なので **AO が 1 でも未定義でも絵が byte 一致する**(実測)。
  **descriptor / plan 側で「何が解決されたか」を観測すること。**
  あるいは `sky_ambient` を有効にした構成を**同じテストの中に**置く
- **未接続かつ既定値の宣言も無い socket が、ソケット名を含む名前付きエラーで落ちること。**
  **今日は無言である**(Debug の VUID は engine を止めない、Release は何も出ない)。
  **これが本 WP の fail-fast の錨である**
- **shadow 有り / 無しの両構成で成立すること。**
  `animgraph_demo`(7 入力)と `sprite_demo` / `vrm_xr_demo` / `example`(6 入力)。
  **同じテストの中で、SSAO を外した shadow 構成が
  `shadow_map` を `ssaoSampler` として読まないこと**(今日は型が食い違ってコンパイルが落ちる)
- **`test/golden_harness.cpp` の 3 つ目のコピーも消すこと。**
  `makeShadowRenderingConfig` が `ssao_clear` を組み立て、`white.frag` を 2 箇所で書き出しており、
  **shadow / sprite / taa / morph / material-override の golden 全部に乗っている。**
  **`projects/` の 2 件だけ消しても「回避策が消えた」は成立しない**
- **`type: raster` の実証を失わないこと。**
  `sprite_demo` の `ssao_clear` は**プロジェクト空間で唯一の `raster` 使用例**であり、
  manual の唯一の実例でもある。**代替の dogfood か画素 golden を同じ WP に含めること。**
  含めないなら、回避策の削除は**別の機構の証拠を削る取引**になる
- 出荷 4 プロジェクト、`pelican project init`、`RUNTIME_SHADER_COMPILER` の ON / OFF
- **`uv run tools/doclink.py check` が緑であること**

### 落としてはならないもの

- **既定値の所在は一箇所。**現在は分類(名前の部分一致)が `materiallowering.cpp`、
  画素値が `standardmaterialresource.cpp` に**割れている。**
  fullscreen へ広げる前に畳むこと
- **名前の部分一致を socket 名へ持ち込まないこと。**
  そのまま持ち込むと `gbuffer_normal` → flat_normal、`g_emissive` → black のように
  **「名前が既定値を決める」隠れた結合が fullscreen 側にも増える**
- **`MaterialPassInputFallback::fully_lit` は値を選ばない。**
  metadata として publish されるだけで、実体はシェーダー変種である。**既定 descriptor の経路ではない**
- **material のパス入力(set 1)は今日も fail-fast で、未接続は例外になる。**
  **非対称なのは fullscreen だけである。**揃えること

## 著作キャンバスの設計(2026-08-20・第 2 版)

### 利用者の決定(2026-08-20)

第 2 版に対して 6 点が決まった。**以降はこれを前提とする。**

#### ① 著作の書き込み先 —— **B(複数文書トランザクション)**

「将来性のある方にしたい」という判断。

**A(root への inline delta field)は却下。**形式を分岐させるためである ——
エディタが書いたものと手で書いたものの形が変わり、共有できず、
`planar_reflection` 級(11 target / 8 pass / 6 compute)を root にインラインで持てない。
**同じことを著作する第二の流儀**は、WP324〜328 で 4 つ費やした失敗そのものである。

**変種 C(fragment を先に書き、root の参照だけ CAS。孤児は許容)も却下。**
**purgeable に反する** ——「参照されていない fragment がプロジェクトに残る」のは、
まさに本原則が禁じている孤児である。

したがって **staging・全ファイル digest・commit-last manifest・クラッシュ復旧・
孤児の発生しない構造**を設計する。GUI に出すのは 1 つの複合操作 `add_authored_pass` である。

#### ② 入口は 1 つ。preset は「既にそこにあるノードの供給源」

利用者の指摘: **preset 固有のフォワード/ディファードのような構造ノードも
自分で足せるようにすれば、「preset から始める」と「0 から始める」は同じことになる。**

**正しい。**第 2 版の「入口を 2 つ残す」は不要になる ——
**同じ画面の別の初期状態**にすぎない。

**ただし ③ と組み合わせると、収束はまだ来ない。**preset `hybrid_v1` の 9 パスのうち
**構造ノードは `material` 型**である(`deferred_geometry` / `forward_opaque` /
`forward_transparent`)。fullscreen だけを著作できる段階では足せないので、
**preset は当面「別物の供給源」のままである。**
**収束は `material` を著作できるようになった時点で完成する。**

#### ③ v1 は fullscreen のみ

`example` は 20 パス中 16 が fullscreen なので大半に届く。
preset の構造ノードには届かない(②)。

#### ④ preset は eject ではなく「参照の束」へ作り直す【「参照の束」は第 3 版で破棄。第 4 版が正本】

利用者の判断: **preset はあくまで存在するノードの組み合わせを提供するだけにしたい。
eject よりももっと深い融合がある。**

**現状の preset は参照の束ではなく中身のインラインである。**
`hybrid_v1.json` の `config` は `render_targets` 11 件と `rendering_passes` 9 パスを
直接持ち、**`features` を持たない。**preset の解決は feature 合成の**前**に走る。
だから「取り出す(eject)」という特別な操作が要る構図になっていた。

**preset を参照の束にすると、その構図自体が消える:**

- preset の `deferred_geometry` は、利用者も同じように参照できる文書になる
- 「自分のものにする」= **その文書をコピーして参照を書き換える**。
  これは既に決まっている正規操作で、**feature 由来については動くことが確認済み**である
- **eject という特別な機構が要らなくなる。**隠されているものが無いので取り出す必要がない
- **preset と feature が 1 つの機構になる**

**利用者の指示: 段階を踏まず直接直してよい。優先の変更が終わってから着手する。**

**効くところ**(着手時に解くこと):

- 解決の順序(いま preset 解決 → feature 合成)
- preset は pass 以外も持つ(`render_targets` / `shader_defines` / `draw_sort` / `material_routing`)
- preset 使用時の許可鍵 11 個という制限の根拠が変わる
- **`pelican project init` が生成するのが `hybrid_v1` である。**移行が要る
- **purgeable を下げないこと**(下記)

#### ⑤ まずオーバーレイ。破綻したら分離

**表示/非表示の切り替えで 1 枚に重ねる。**
**レイアウトが破綻したら分離する。**

**破綻の可能性は低くない。**`example` は著作 20 に対しコンパイル後 30 ノードで、
**依存グラフが鎖である**(30 層すべて 1 ノード、`parallel_candidates` は 0)。
読みにくさは WP318 で一度踏んでいる。
**分離への切り替えを最初から想定した作りにすること。**

#### ⑥ ヘッドレスと dry-run の併用

| | 問い | 実測 |
|---|---|---|
| **ヘッドレス**(`--headless --dump-frame-plan`) | この config はコンパイルが通るか | **4.0 秒**(Debug、3 回とも一定)、285 KB、**usage 付き** |
| **dry-run RPC** | いま動いているエンジンが、このデバイスと現在のモジュールグラフで受理するか | `features[]` の preflight で形は既にある |

**4 秒はタイプするたびの検証には使えない。**したがって:

- **studio が判定できる軸(形・名前・所有)はライブ**
- **エンジンにしか判定できない軸(usage 適合・生成順・シェーダー解決)は明示的な検証で**

**「判定していない」が「まだ検証していない」に変わる。**押せば本物の答えが返る。

**注意: 検証に使うバイナリは studio が起動している player と同じものであること。**
シェーダー stem の解決は `PELICAN_RUNTIME_SHADER_COMPILER` に依存するので、
ビルドフラグが違えば二つが食い違う。

**dry-run でも全部は検証しきれない** —— 絵が意図どおりか、実負荷でのみ出る失敗、
読み込まれていないシーンに依存するもの、**そして publish 後の失敗経路**。
答えるのは「受理されるか」であって「動くか」ではない。

### 横断する制約: purgeable を下げないこと(利用者の明示指示)

**機能を外したとき、孤児のパスやレンダーターゲットが残らない。**
著作キャンバスと preset の作り直しの両方に効く。

- **エディタが、参照されないまま残るファイルを作らないこと**(①で C を却下した理由)
- **feature の参照を外したら、そのパスとターゲットが揃って消えること**
- **除去された内容へのアンカーは、黙って劣化せず名前付きで落ちること**
- **preset を参照の束にしたあとも、preset を外せば供給物が全部消えること**

**受け入れ条件に purge の対照を入れること** ——
足す前・足した後・外した後の 3 点で、孤児が残らないことを検査する。

**要求 1「ノードエディタで 0 からパスを組めるようにしたい」に対する設計。**

第 1 版は敵対レビューで却下された。**却下の中心は、私が使った統計が標本として無効だったこと**である。
以下は訂正済みの版であり、第 1 版の主張のうち撤回したものを明示する。

### 撤回した第 1 版の主張(繰り返さないこと)

**撤回 1: 「`after:` が主用法だから、空のキャンバスから始める UI を作らない」**

私は出荷 feature の `insert` を数えて `after: 22 / begin 3 / before 3` を得た。
**数え自体は正しいが、測っていたものが違った。**あれは
**エンジン作者が複雑な feature の内部でパスを鎖状に並べた回数**である。

`feature が外部へ取り付く最初の操作`で数え直すと **`after: 7 / begin 3 / before 3`**。
そして**出荷 4 プロジェクトの著作 config は `insert` を 1 回も使っていない**:

| project | `insert` | 直接著作されたパス |
|---|---:|---:|
| example | **0** | **20** |
| sprite_demo | **0** | 3 |
| vrm_xr_demo | **0** | 3 |
| animgraph_demo | 0 | 0(preset) |

**エンジン作者の書き方を、studio 利用者の最初の操作頻度に転用したのは誤りである。**

さらに **`after:` の反復では 0 から組めない** —— アンカーにするグラフが無い。
利用者は最初に「0 からパスを組めるようにしたい」と述べており、
**私はその要求を別のものにすり替えていた。**

**入口は 2 つ残す。**既存パスの後ろに足す道と、グラフを持たない状態から始める道。
**どちらを既定にするかは、利用者の明示要求か利用実績で決める。**
**エンジン資産のトークン数で決めない。**

**撤回 2: 「JSON で書ける段まで、その下は C ABI」という天井**

**8 段は単調な境界ではない。**全 8 段に JSON の著作面がある。
4〜6 段目で独自アルゴリズムに code が要る一方、
**7〜8 段目で data-only の JSON が戻ってくる**(`vulkan_physical_fragments`、
complete physical plan の宣言と `implementation_config`)。
`NativeScope` の game-DLL ABI は**まだ凍結されていない**。

**段番号ではなく操作別の capability 表にする**(下記)。

**撤回 3: 「preset 由来はコピーして参照を書き換えれば所有できる」**

**所有化は 3 つの別の話であり、混同していた:**

| 出自 | 所有化 |
|---|---|
| **feature 由来** | 元 fragment の**正方向コピー**が可能 |
| **preset 由来** | preset 解決境界での**正方向 eject** が可能。ただし**resolved-authoring eject は将来機能** |
| 任意の合成結果 | **不可能**(逆コンパイル) |

`deferred_lighting` の所有者は `pipeline.preset` であり、
**書き換えるべき `features[]` エントリが存在しない。**

### 壊れなかったこと(レビューが確認)

**preset 由来のパスをアンカーにするのは成立する。**
`animgraph_demo` は preset と `sky_ambient` を併用し、
`sky_ambient` は preset 由来の `deferred_lighting` をアンカーにしている。
既存テストが feature 無し/有りを対比して順序を検査しており、実行して 37 assertions が通っている。

**したがって「preset 由来はアンカーに使えるが編集はできない」という線引きは維持する。**

### 中心の困難 —— 1 パスの追加が原子的に表せない

**これが段階分けを決める。**

パスを 1 つ足すには 2 つのファイルが要る:

1. fragment を作る(`project://passes/authoring/<name>.json`)
2. root の `features[]` からそれを参照する

**現在の編集経路は 1 ファイルしか扱えない。**
document が持つのは root の `features` 配列の字句範囲だけで、
commit は 1 ファイルを CAS 置換し、runtime apply も root 1 本と source commit 1 個を受ける。

- fragment を先に公開 → root の CAS が失敗 → **孤児**
- root を先に書く → **dangling reference**
- **`operations` の個数制限を外しても解決しない。**複数ファイルの commit とクラッシュ復旧が増えないため

**二択。前者を推奨する【却下。利用者の決定 ① が B(複数文書トランザクション)を選び、
A を「形式を分岐させる」として明示的に却下した。WP334b は B で実装済みである。
以下の表は決定前の下書きであり、推奨は失効している】:**

| 案 | 内容 |
|---|---|
| **単一ファイル案(推奨)** | **preset 使用時にも許可される専用の inline delta field を root に置く。**1 ファイル CAS のまま |
| 複数文書 transaction 案 | virtual candidate loader、全ファイルの digest、staging、commit-last manifest、クラッシュ復旧、孤児 purge |

**GUI に出すのはどちらでも 1 つの複合ドメイン操作 `add_authored_pass` である。**
**低水準 operation を複数送る形にしない。**

### `features` を持たない config には編集面が無い

現在の runtime は、著作 root が `features` を含むときだけ編集サービスを構築する。

```json
{ "rendering_passes": [...] }
```

これは描画 config として成立するが、**編集 RPC の面が作られない。**

**root document の初期化を正式な操作にすること。**
既存 bytes を全体再シリアライズせず、`features: []` または delta field を挿入する。
**no-op の byte identity を受け入れ条件に入れること。**

### 著作対象は fullscreen パス 1 つではない

**「複雑なパス設定も設計できるように」に届いていなかった。**

正本の feature 形式は **target / buffer / pass / compute task** を著作対象にしている。
到達点の目安は `planar_reflection.json` ——
**11 render targets / 8 passes / 6 compute tasks を一体**で持つ。

**対応する著作対象を独立した capability として列挙し、
未対応には名前付きの refusal を定義すること。**「まだ出せません」と黙って隠さない。

### 天井 —— 操作別の capability 表

段番号ではなく、**操作ごとに**次のどれかを示す:

| 区分 | 意味 |
|---|---|
| **JSON だけで追加・選択できる** | キャンバスで完結する |
| **既存の builtin / provider があれば選択できる** | 選ぶだけ。実装は既にある |
| **新規 provider の実装が要る** | キャンバスの外。名前付きで示す |
| **backend / source 拡張が要る** | 同上 |
| **現在 ABI 未凍結** | `NativeScope` の game-DLL |

**キャンバスは、その操作がどの区分かを利用者に示すこと。**

### 辺・順序・自動配線(第 1 版から変更なし)

- **辺は著作されない。**`input` / `output` はリソース名の参照で、辺はエンジンが導出する。
  「辺を引く」身振りは実際には「`input` にこのリソース名を足す」である
- **宣言順は安定タイブレークであって意味論ではない。**
  明示 `before` / `after` はデータ依存で表せない意図のためだけの契約
- **順序が導出できない writes-writes は hard error。**UI が黙って順序を決めない
- **自動配線は候補がただ 1 つのときだけ。**複数なら推測せず候補名を示す。明示接続が常に優先
- **未指定は「制約なし」という作者の主張として受理する**
- **`kind` は著作カテゴリ兼診断であって実行場所ではない**
- **プランに要求されるのは connected ではなく closed。**非連結を警告にしない
- 編集できないもの(canonical anchor / `output_transform` / terminal / present / XR / barrier)を
  **削除・切断できるように見せない**
- `region tag` を「囲むと隔離される箱」として提示しない

### キャンバスとプランの関係(第 1 版から変更なし)

**view-model は分ける**(辺の意味が違う)。
**widget を分けるかは未論証** —— 編集後・反映前を amber/pending、
コンパイル済みを generation N と並べるのは矛盾ではなく利用者が見たい差分そのものである。
同期 split view / レイヤ切替 / 重ね表示を比べてから決める。
**linked selection と共有レイアウトを受け入れ条件にする。**

### 段階(第 1 版から全面変更)

**第 1 版の段階 1「既存パスの後ろに 1 つ足す」は、単独では出荷できない。**
現在のフォームが既に draft JSON を作って Copy できるので、
**新しい価値になるのは原子的保存・runtime apply・著作対コンパイル差分**であり、
第 1 版はそれらを後続へ後回しにしていた。

```
1. 保存の基盤 —— 【却下】単一ファイル delta field 【→ 複数文書トランザクション。
   利用者の決定 ①。WP334b で実装済み】と add_authored_pass 複合操作、
   root document 初期化。**内部基盤であり、単独の利用者価値を主張しない**
2. 縦切り —— セッション draft(入力にしない)/ 1 複合操作で保存 /
   コンパイル失敗時は draft を保持し disk と runtime は旧世代 /
   retry・discard・recover / **著作対コンパイルの差分と linked selection**
3. 入口を 2 つ —— 既存パスの後ろに足す道と、グラフを持たない状態から始める道
4. 著作対象を広げる —— target / buffer / compute task。到達点は planar_reflection 相当
5. 所有化 —— feature 由来の正方向コピー。preset 由来は eject が入るまで名前付きで断る
6. ノード位置の永続化(ワークスペース 3 層目)
```

**段階 1 を「利用者価値のある v1」と呼ばないこと。**内部基盤である。

### 受け入れ条件は各段階の WP で書く

第 1 版の条件は**機能が効かなくても通る**形だった。最低限、次を含めること:

- **本番の `MainWindow` を通ること。**テストにだけ存在する実装が通らないこと
- **著作対コンパイルの差分が、実際に解決された値で検査されること**
- **コンパイル失敗後に、draft が残り disk と runtime が旧世代のままであること**
- **no-op 保存が byte-identical であること**
- **エラーが利用者に直せる形であること** ——
  名前付きであるだけでは足りない。**何をすれば直るかを示すこと**

## 実装の順序 —— 4 回目の残骸【却下。5 回目の段階列に置き換え済み】

**この節は却下された 4 回目の案が書いたものである。実装の根拠にしないこと。**

現行の段階列は「編集側の設計 — 5 回目」節の**段階 1〜5** である。
番号も内容も違う(本節は 4 段、現行は 5 段で、著作キャンバスは**段階 5**)。

**本節の「著作グラフは別キャンバスにする。順序として不可逆である」は、
5 回目が「これは誤りである」と名指しで否定している。**
編集後・反映前を amber/pending、コンパイル結果を generation N と並べるのは矛盾ではなく
利用者が見たい差分そのものなので、**view-model は分けるが widget を分けるかは未論証**である。

**段階 1・2 を編集の前提とする本節の依存表も誤りである。**5 回目で前提から外した。

節の本文は削除した。何が却下されたかは 5 回目の節が本文中で述べている。

2026-08-20 に印を付けた —— **却下と書かれないまま 1 日残り、
既存設計を読まずに設計を書いた失敗と同型の罠になっていた。**

### WP318: ターゲットを選んで、その周辺だけを見る

**目的**: WP306 のキャンバスを起動した結果、利用者の評価は「かなり見づらい」。
原因を追ったところ、**見る単位が間違っていた。**

#### 全体をノードグラフで見ることはできない(実測)

`frameplangraphics.cpp:912-916` は全ノードを `y == 0` の一列に置く。
当初これを「層別配置にすれば直る」と診断したが、**それは効かない。**

- **エンジンは既に最長路の層を計算している。**
  `computeLevels`(`src/core/renderingpass/frameplanner.cpp:1544-1555`)は
  `levels[node] = max(levels[node], levels[edge.from] + 1)`、
  すなわち依存辺に沿った最長路長。`FramePlanNode.level` として wire に載り、
  Qt モデルも既にパースしている(`src/devstudio/model/frameplanmodel.cpp:532`)
- **しかし `projects/example` では 30 層すべてが 1 ノードである。**
  fixture の `levels` 配列は `[1,1,1,...,1]`(30 個)。同じ層に入るノードが 1 組も無い
- 一致する事実: `planning_opportunities.parallel_candidates` は **0**。
  **コンパイラ自身が「並列化できる箇所ゼロ」と算出している**

**依存グラフが本当に鎖なので、どんなレイアウトでも 2 次元にはならない。**
リソースをノードに戻した二部グラフでも 44 ノード / 37 層 / 最大幅 5 で、
幅が出るのは `gbuffer_pass` の扇形 1 箇所だけだった。

#### 部分木なら常に読める(実測)

ターゲットを 1 つ選び、その周辺だけを見ると小さい:

| ターゲット | 直接の生産者 / 消費者 | 推移的に全部辿ると |
|---|---|---:|
| `gbuffer_albedo` | 1 / 1 | 1 |
| `ssao_blur` | 1 / 1 | 3 |
| `lit_color` | 2 / 4 | 7 |
| `Bloom_Threshold_RT` | 1 / 1 | 8 |
| `display` | 2 / 3 | 21 |

**深さ 1 段ならどのターゲットも 1〜4 ノード。**
推移的に辿ると `display` で 21 まで膨らむが、それは「全体を見る」のと同じなので辿らない。
**深さを制限すれば常に読める大きさである。**

そして**著作の単位と一致する** —— 人が書くときも「この bloom 連鎖」「この SSAO」という
単位で考えるので、閲覧と著作が同じ粒度になる。

セッション中に手描きした図で読めていたのは、bloom を**ピラミッド**として、
SSAO を**ダイヤモンド**として切り出したものだった。全体は読めず、部分木は読めた。

#### 実装範囲

1. **ターゲットを選ぶと、その周辺の部分木を描くこと。**
   **「深さ 1」の定義を確定する: `unique(writers ∪ readers)`** ——
   そのターゲットを `writes` に持つノードと `reads` に持つノードの和集合(重複排除)。
   リソースを経由してさらに辿るのは深さ 2 以上とする。
   仕様レビューはこの定義のもとで `projects/example` の 22 ターゲット全件が 5 以下になることを検算した。
   深さは利用者が変えられること。**推移的に全部辿る既定にしないこと。**
2. **物理層の判断を、その場に重ねること。**
   WP307 が表示している事実はターゲット単位である ——
   統合されたか、合法だったが不採用か、`reason`、`widest_read`、生存区間。
   **選んだターゲットの画面に出すこと。**22 件の一覧では自分の関心事が分からない。
3. **辺を曲線にすること。** 直交レーン配線をやめる。
4. **ホイールでズームすること。** 変換アンカーは既に `AnchorUnderMouse`
   (`view/frameplanwidget.cpp:230`)だが `wheelEvent` がどこにも無く `scale()` を呼ばない。
   上下限を決め、範囲外の入力を黙って無視しないこと。
5. **ノードをドラッグで動かせること。** `ItemIsMovable` **だけでは足りない。**
   ノードの geometry は先に確定され(`frameplangraphics.cpp:912`)、
   辺・矢印・ラベルはその確定済み geometry から**別の item として作られる**(`:964`)。
   **フラグだけ立てるとノードだけ動いて辺が取り残される。**
   移動時に接続する辺・矢印・ラベルが追随すること。
   **位置はセッション内のみ。永続化は「エディタのワークスペース状態」の設計に従う。**

#### やらないこと(判断の記録)

- **全体グラフの層別配置。** 上記のとおりグラフが鎖なので効かない。
  `level` は既にあるので部分木の中では使ってよいが、全体配置の解決策にはならない
- 折り返し(蛇行)配置。部分木が小さいなら不要である

#### 受け入れ条件(§4 規約 10)

- **`projects/example` の任意のターゲットを選んだとき、
  既定の深さで表示されるノード数が 5 以下であること。**
  上表の実測値と一致すること —— これが本 WP の中心的な対照である
- **深さを上げると表示が増え、下げると減ること。**
  既定が推移的全探索になっていないこと(`display` で 21 ノード出たら不合格)
- **選んだターゲットの物理層の事実がその場に出ること。**
  `Bloom_Threshold_RT` を選んだとき、`g_emissive` との統合が採用され、
  `gbuffer_albedo` との候補が不採用だったことが**両方**見えること。
  **さらに WP307 が全 22 件について契約している項目 ——
  `reason` / `widest_read` / `aliasable` / `representation` / `lifetime` / profile / endpoint ——
  を選択ターゲットについて検査すること。** alias だけ検査すると、他を全部消しても通る
- **深さ 0 / 1 / 2 について、特定ターゲットの表示ノード集合を名前で厳密に指定すること。**
  「深さを上げると増える」は葉や飽和後には成立しないので条件にならない
- **入力配列の順序を入れ替えても座標と束ね順が一致すること**(WP306 の決定性条件を引き継ぐ)
- 辺が曲線であること。ズームの上下限を超える入力が黙って無視されないこと
- **`QT_QPA_PLATFORM=offscreen` で緑であること。**
  グリフ計測に依存する判定を入れないこと(WP316 の教訓)
- **variant ごとに何が出るかを表で確定すること。** 「全 variant で物理層の事実が出る」は
  **字義どおりには満たせない**:
  | 構成 | 期待 |
  |---|---|
  | flat / XR ON | 論理部分木 + 物理層の事実 |
  | preview | **論理のみ。物理は `physical_plan_missing`**(preview は意図的に物理計画を持たない) |
  | OpenXR OFF の xr | **`XR graph variant is unavailable in this build`**(グラフ自体が返らない) |
  | compiler ON / OFF | feature が消えたとき、**選択・部分木・詳細が同一 widget から purge されること** |
  `PELICAN_WITH_IMGUI` の ON/OFF と `SKIP_DEVSTUDIO` の扱いは WP307 と同等に維持すること
- `ctest` 全数が緑(`-j4`)、`uv run tools/doclink.py check` が通ること

#### 範囲外

編集。位置の永続化。全体把握用のビュー(WP319)。

依存: なし。見積: 中。

### WP319: ターゲットの生存区間を一覧で見る

**目的**: 全体把握の手段。**構造は部分木(WP318)で見る。全体は表で見る。**
全体をノードグラフで見ようとしたのが誤りだった。

#### 読めることが実証済みである

セッション中に `test/fixtures/devstudio/example_frame_plan.json` を手で集計して描いた表が読めた:

```
offscreen_depth          W[0, 6]       R[5, 6]              span 0..6    *再利用
lit_color                W[3, 6]       R[4, 6, 11, 23]      span 3..23   *再利用
Bloom_Downsample_H_0_RT  W[12, 22]     R[13, 22, 23]        span 12..23  *再利用
```

22 行 × 実行順 30 列。**再利用が一目で分かり、`lit_color` が 20 ノードにまたがる最長寿であることも見えた。**

#### 「新しい計算はゼロ」は偽だった(仕様レビューの指摘)

材料は wire にある —— `lifetime{first_use, last_use}` はエンジンが算出済みで、
WP305 が既に studio 側でパースし、`alias_groups` は WP307 が既に表示している。
**しかし添字をそのまま実行順の列に重ねると、一般入力で列がずれる。**

- frame plan の順序は `declaration_index` で tie-break する
  (`src/core/renderingpass/frameplanner.cpp:1516`)
- **物理プランナは名前順で並べる**(`src/project/targetplanning.cpp:840`)
- `lifetime` の添字は **lowering node 順**に対して計算される
- wire の検証は**同じ集合であることだけ**を見て、順序一致を見ない
  (`src/project/physicaltargetplanwire.cpp:509`)

反例: 依存しないパスを `[zeta, alpha]` の順に宣言した合法構成では、
frame plan は宣言順、物理計画は辞書順 `[alpha, zeta]` になる。

**したがって:**

- **列は `lowering_graph.nodes` の順とし、「planning / lowering order」と明示すること。**
  「実行順」と書かないこと
- W / R のノード名から添字への変換は、**名前 join を fail-fast で**行うこと。
  見つからない名前を割合配置に落とさないこと
- 実行順を列にしたいなら、**エンジン側に軸の契約か per-resource access event を足す必要がある。**
  それは本 WP の範囲外である

正しい表現は「**新しいエンジン計算は不要だが、studio 側に検証付きの join が必要**」である。

#### 実装範囲

1. ターゲットを行、実行順を列にした一覧。書き込み点と読み取り区間を示すこと。
2. **二度以上書かれるターゲットを区別すること。**
   `projects/example` では 22 件のうち 6 件
   (`offscreen_depth`、`lit_color`、`display`、`Bloom_Downsample_H_0/1/2_RT`)。
   bloom は下げた先のバッファに上げ合成を書き戻すため、ここが分かると構造が読める
3. 行を選ぶと WP318 の部分木ビューへ飛べること。**一覧から構造へ繋ぐこと。**
   これは WP318 に依存する(下記「所有権」を読むこと)。

#### 所有権 —— WP318 と独立にはできない(仕様レビューの指摘)

現状、全体表示と生存区間表示は**同じ scene を共有している**:

- WP306 のテストは既定 scene に 30 ノードを要求する
  (`test/devstudio_frameplan_graph_test.cpp:1328`)
- WP307 のテストは**同じ scene** に 22 行の生存区間を要求する(`:1431`)
- 現在のターゲット選択は、その生存区間の行の選択しか認識しない
  (`src/devstudio/view/frameplangraphics.cpp:1104`)
- 生存区間の x 座標は全 lowering node を scene のノードへ対応付け、
  **見つからなければ割合配置に落ちる**(`:534`)。
  部分木化して一部のノードしか描かなくなると、**残した行が別の軸に落ちる**

**したがって次を先に決めること:**

1. 共有するターゲット選択状態と timeline のモデルを定義する
2. WP307 の既存 22 行を**移設して再利用するのか、削除するのか**を明記する
3. 全体グラフを外すなら、**WP319 を先行させるか同一 WP にする**
4. **WP306 / WP307 の受け入れ条件の移行を明記する** ——
   30 ノード要求と 22 行要求は現状のまま残せない

#### 受け入れ条件(§4 規約 10)

- `projects/example` で 22 行が出て、**二度以上書かれる 6 件が区別されること**
- **22 件全件の W / R ベクタを実値で照合すること。**
  例示 3 行と `lit_color` の区間だけでは、**読み取り点を一つも描かない実装でも通る**
- **`alias_groups` が空の構成で、再利用の表示が消え、区間の表示は残ること** ——
  これが本 WP の対照である。ただし**fixture の `alias_groups` を空にするのでは不可**。
  同じ producer を `optimized` と `conservative_debug` で**実コンパイル**し、
  optimized 側の実 alias group を先に検査してから対比すること
- **「再利用」「論理アクセス区間」「物理割り当て寿命」を別の用語・別の表示要素にすること。**
  三つは別の概念であり、混ぜると何を見ているか分からなくなる
- 行から部分木へ飛ぶ経路が `FramePlanWidget::receiveResult` を通ること。
  feature 更新で選択・行・詳細が purge されること
- **`QT_QPA_PLATFORM=offscreen` で緑であること**
- **`QT_QPA_PLATFORM=offscreen` で緑であること**
- `ctest` 全数が緑(`-j4`)、`uv run tools/doclink.py check` が通ること

依存: **WP318**(scene とターゲット選択を共有しているため独立にできない。上記「所有権」)。見積: 中。


### WP320: ノードをドラッグするとクラッシュする(WP318 の回帰・最優先)

**目的**: WP318 が入れたドラッグが**実際に使うとスタックオーバーフローで落ちる。**

#### 実測(2026-08-18、studio を起動して利用者が再現)

```
障害モジュール: Qt6Cored.dll
例外コード: 0xc00000fd   ← STACK_OVERFLOW
```

`pelican_studio.exe` がノード移動中に落ちる。**無限再帰である。**

#### 分かっていること(ダンプ解析、2026-08-18)

`%LOCALAPPDATA%\CrashDumps\pelican_studio.exe.25452.dmp` を解析した。

- 例外は `0xc00000fd`(スタックオーバーフロー)、落ちたスレッドは 30836
- **輪はおよそ 145 段**繰り返している
- **輪に我々のコードが含まれる**: `pelican_studio.exe +0xdf2f0`(146 回)と
  `+0x3b91e8`(145 回)。その間に Qt6Widgets のイベント配送と Qt6Core のフレームが挟まる
- スタック上のフレーム帰属: Qt6Widgetsd 3884 / Qt6Cored 1785 / **pelican_studio.exe 1189**

**単純な関数再帰ではなく、Qt のイベント配送を経由して自分のハンドラに戻る輪である。**

#### 分かっていないこと —— 推測を書かないこと

輪の起点は**特定できていない**。次の 4 つの仮説を立てて、いずれも実験で否定した:

1. **移動ハンドラ内の `setSceneRect` がビュー変換を動かす** ——
   実マウスイベントでビュー越しにドラッグするテストを書いたが、
   ネイティブ / offscreen の両方で通る
2. **多数の操作の組み合わせ** —— 22 ターゲット × 深さ 3 × ズーム × 全ノードの長距離ドラッグ
   (285 assertions)を回したが通る
3. **ドラッグ中にフレームプランが届く** —— ドラッグの途中で `receiveResult` を
   繰り返し投入するテストを書いたが通る
4. **コンボボックスのシグナル輪** —— `currentTextChanged` は `populate()` を呼ぶが、
   再投入は両方とも `QSignalBlocker` で守られている(`view/frameplanwidget.cpp:427`, `:524`)。
   `processEvents` / `sendEvent` は studio に存在しない

**次の決定的な一手は `+0xdf2f0` と `+0x3b91e8` の記号解決である。**
デバッガ(cdb / windbg)がこの環境に無いので、
`/MAP` 付きで再リンクして RVA を引くか、デバッガを入れる必要がある。

#### 別件: studio は 1 週間前から落ちている

イベントログを 10 日分見たところ、`pelican_studio.exe` のクラッシュは今回が初めてではない:

| 日付 | 例外 |
|---|---|
| 08/18 | **0xc00000fd**(スタックオーバーフロー) |
| 08/17 | 0xc0000005(アクセス違反) |
| 08/14 | 0xc0000005 |
| 08/13 | 0xc0000005 |
| 08/11 | 0xc0000005 |

**今回の署名は過去 4 件と異なる**ので、今回は新種である可能性が高い。
ただし**アクセス違反での常習的なクラッシュが別に存在し、誰も起票していない。**
ダンプは `%LOCALAPPDATA%\CrashDumps` に残っている。別 WP として調べること。

#### 実装範囲

1. **移動ハンドラの中でシーン矩形を変えないこと。**
   広げる必要があるなら、ドラッグ終了後か、キューに載せて遅延させること。
   **再入ガードだけで済ませないこと** —— 輪の起点を断つこと。
2. `rerouteConnectedBundles` が位置を書き戻していないことを確認すること。

#### 受け入れ条件(§4 規約 10)

- **`QTest` の実マウスイベントでビュー越しにドラッグし、落ちないこと。**
  press → 複数回の move → release を実ビューに送ること。
  これが本 WP の中心的な対照である —— **プログラムから `ItemPositionHasChanged` を
  発火させる形では、この欠陥を検出できない**
- 移動量、接続する辺・矢印・ラベルの追随は WP318 の検査を維持すること
- **ドラッグでシーン矩形が伸び続けないこと。**
  ドラッグ前後でシーン矩形が有限に収まること
- `QT_QPA_PLATFORM=offscreen` で緑であること
- `ctest` 全数が緑(`-j4`)、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 小。**最優先** —— 主要な新機能が実使用で落ちる。

### WP321a〜b / WP322 / WP323 の共通前提 —— 実測値(定義付き)

**ビルドフラグで変わる数字を固定値で書かないこと。**以下は 3 層に分けてある。

#### 層 A: 無条件に固定値でよい(git 追跡ファイル、`if()` の外の宣言)

| 値 | 定義 |
|---|---|
| **20 パス / 1 グラフ** | `projects/example/passes/main_rendering_config.json` の `rendering_passes` は 1 要素(`main_render`)、その `passes` が 20。内訳 fullscreen **16** / material **2** / snapshot_copy **2** |
| **20** | 同ファイルの `render_targets` の要素数 |
| **16 / 16** | fullscreen 16 件のうち `{name,type,shader,input,output}` の外に鍵を持つもの。**例外なし。**のべ内訳: `clear_color` 15、`color_load_op` 4、`push_constants` 2、`uses_light_data` 1。**既定値からの逸脱**で数え直しても 16/16 |
| **7 / 1** | example の fullscreen が使う相異なる `shader.fragment` = 7、`shader.vertex` = 1(`engine://fullscreen`、16/16) |
| **32** | エンジンの登録済み **fragment stem** 数。git 追跡下の `src/core/resources/**/*.frag` も 32 で**同一集合** |
| **26** | `src/core/resources/CMakeLists.txt` の `embed_shader()` が SPIR-V を作る `.frag` の本数(すべて `if()` の外) |
| **14 / 14** | `RenderPassType` の要素数と `passFieldOwnershipTable()` のエントリ数 |
| **9 / 5** | `shader` を**所有する**型 9(fullscreen, output_transform, raster, debug_draw, gizmo, debug_text, shadow_depth, velocity, picking)/ **所有しない**型 5(material, ui, imgui, canonical_anchor, snapshot_copy)。**フォームが常に `shader` を吐くと後者 5 型で所有検査に落ちる** |
| **30 / 22** | fixture `example_frame_plan.json` の nodes / resources |
| **22 = 20 + 2、21 + 1** | resources の `source` 内訳(project 20 / engine 2)と `kind` 内訳(render_target 21 / frame_target 1 = `swapchain`)。**存在集合であって候補集合ではない** |
| **20 / 19 / 1** | 同じ 22 件に**位置非依存の門だけ**を掛けたときの候補数。`input` 20 / `output.color` 19 / `output.depth` 1。`input` はさらに「同じ `passes[]` のより前で生成済み」に縛られ**位置依存**(`gbuffer_pass` 直後なら 5) |
| **`{kind, name, source}` のみ** | フレームプランの `resources[]` が運ぶ鍵(+任意で `provider_feature` / `provider_ref`)。**`usage` も `storage_mode` も `role` も無い** |
| **15 / 0** | フレームプラン 30 ノードの鍵の**和集合**は 15 個。**シェーダー参照を運ぶ鍵は 0** |
| **4 値** | ノードの `kind` は `render` / `snapshot_copy` / `anchor` / `output_transform` の 4 種のみで、**`fullscreen` と `material` を区別しない**。型はプランから読めず `passFieldOwnershipTable()` から引くしかない |
| **11 / 9** | preset `hybrid_v1.json` の `config.render_targets` = 11、`config.rendering_passes` は 1 グラフ 9 パス。台帳の animgraph_demo 0/0 は**著作ファイルの列**であって実行時ではない |
| **11 鍵** | preset 使用中の著作 config の許可鍵。`rendering_passes` は**含まれない** |
| **0 / 1 / 1 / 0** | プロジェクト側の著作 **fragment stem** 数。example 0(`.surface` 3 本はマテリアルの surface 入力であって fragment stem ではない)/ sprite_demo 1 / vrm_xr_demo 1 / animgraph_demo 0 |

#### 層 B: フラグ名を必ず併記する(裸の数字を置かない)

| 値 | 条件 |
|---|---|
| 登録 vertex stem **17 / 16** | `PELICAN_WITH_VAT` = ON / OFF。追跡下の `.vert` ソース 17 本自体はフラグ非依存 |
| stem 経由で解決できる fragment **32 / 26** | `PELICAN_RUNTIME_SHADER_COMPILER` = ON / OFF。source-only 6 件のうち `surface_v1` は別経路なので実際に落ちるのは 5 件 |
| `imgui` 型の有効性 | **消費側エンジン**の `PELICAN_WITH_IMGUI` 依存。`pelican_project` は `PELICAN_WITH_OPENXR` しか定義しないので **studio は原理的に判定できない。提示しないこと** |

#### 層 C: 台帳に書いてはならない

`.spv` は git 追跡下に **0 件**(`src/core/resources/.gitignore`)。ローカルに見える `.frag.spv` はビルド生成物である。
「SPIR-V が 26 個ある」ではなく「`embed_shader()` が 26 本コンパイルする」と書くこと。

---

### WP321a: fullscreen のパスを GUI で組んで JSON を出す(書き込まない・反映しない)

**目的**: 利用者の要求は
「**どのパスで、どのシェーダーで処理して、次にこうする**」を GUI で指定すること。
本 WP は**フォームだけ**を作る。**プロジェクトに書き込まない。エンジンに反映しない。**

#### studio が判定できる軸は 2 つしかない

これが本 WP の設計を決めている構造的事実である。

- **判定できる**: フィールド所有(`validatePassFieldOwnership` は `src/project/passfieldownership.cpp`
  すなわち **pelican_project** にあり studio から呼べる)、同一グラフ内の名前衝突、
  ターゲット**名**の存在(フレームプランの `resources[]`)
- **判定できない**: ターゲットの **usage 適合**(`input` は SAMPLED、`output.color` は
  COLOR_ATTACHMENT が要るが、**フレームプランは usage を運んでいない**)、
  **生成順**(`input` は同じ `passes[]` のより前で生成済みでなければならない)、
  **シェーダー stem の解決可能性**(D0 とビルドフラグ依存)

判定側の実装 —— `renderingpassvalidation.cpp` と `shaderlibrary.cpp` —— はいずれも
**pelican_core** にあり、`src/devstudio/CMakeLists.txt` の `pelican_assert_link_boundary` が
configure 時 FATAL_ERROR で遮断する。**回避してはならない。**

#### したがって成果物の定義はこうである

> **フォームが出すのは、パス宣言の「フォームが所有する部分」と、
> 出さなかった鍵・検査しなかった軸の明示的な申告である。**

`ssao_pass` を例に取ると、実物は
`{name, type, output, input, push_constants, shader, clear_color}` の 7 鍵で、
フォームが出すのは前者 5 鍵、**申告するのは `{push_constants, clear_color}`** である。

**「妥当」と「判定していない」を同じ状態にしてはならない。**
画面全体の但し書き 1 つでは足りない。**軸の名前を挙げて示すこと。**

#### 実装範囲

1. **型は `fullscreen` に固定する。**選択項目にしない(型を増やすのは WP321b)。
2. **出す鍵は、その型が所有する鍵に限る。**所有集合は `passFieldOwnershipTable()` から
   引くこと。**studio 側に写経しないこと。**
3. **判定は軸ごとに三値**(`妥当` / `不当` / `判定していない`)。
   判定できない 3 軸(usage 適合・生成順・シェーダー解決)は**軸名を挙げて**
   「判定していない」と表示すること。加えて
   **出力が実物の部分にすぎないこと**(出さなかった鍵の集合)を表示すること。
4. **貼り付け先を画面に出す。**対象グラフ名と `passes[]` 内の想定位置
   (位置は `declaration_index` になり実行順に効く)。
5. ターゲットの選択肢はフレームプランの `resources[]` から。**ハードコードしないこと。**
6. **`pelican_studio` に配線すること。**`src/devstudio/view/CMakeLists.txt` の
   `target_sources(pelican_studio PRIVATE ...)` に載せ、`MainWindow` から構築すること。

#### 範囲外

**プロジェクトへの書き込み。エンジンへの反映。**フォーム状態の永続化。既存パスの編集。
`fullscreen` 以外の型(WP321b)。シェーダー資産の列挙。ターゲットの usage 判定(WP323)。
**preset 型プロジェクト** —— 出力形は preset 使用中の著作 config に置けない
(許可 11 鍵に `rendering_passes` が無く、名前付きエラーで拒否される)。
feature へ包み直す合法経路は存在するが、出荷プロジェクトでの使用例は 0 である。

#### 受け入れ条件(§4 規約 10)

すべて **widget を入口とし、期待値は権威から取得する**こと(テストにリテラルで書かない)。

- **(A) 投影対照 —— 本 WP の中心。**
  テスト実行時に `projects/example/passes/main_rendering_config.json` を
  **実ファイルとして**読み(fixture の写しを使わないこと)、`ssao_pass` を取る。
  同じ選択でフォームを駆動し、**同じテストの中で**次を検査すること。
  - 出力 JSON が、実エントリを**フォームが所有する鍵集合へ投影したもの**と一致すること。
    鍵集合は `passFieldOwnershipTable()` から引くこと(**テストに列挙しないこと**)
  - **フォームが「再現しなかった鍵」を自分で報告し、その集合が
    `実エントリの鍵 - 出力の鍵` と一致すること。**`ssao_pass` では `{push_constants, clear_color}`
  - **同じテストの中で `UpsampleBlend_3` でも同じ 2 つを検査すること。**
    差集合は `{clear_color, color_load_op}` になる。**差集合を固定値で持つ実装はここで落ちる**
  - 名前が既存と一致するため衝突警告が出るが、それは**本対照の合否に影響しない**
    (中心条件の述語は「JSON が出ること」であり「妥当と表示されること」ではない)
- **(C) 実データ由来対照。**同じテストの中で `resources` が異なる 2 つのフレームプランを与え、
  **それぞれについて選択肢の集合が当該プランの `resources[].name` の集合と一致すること**。
  「異なること」では不十分 —— `固定22件 ∪ {resources 数}` が通ってしまう。
  **2 つのうち少なくとも 1 つはテスト内で組み立てたプランとすること**
- **(D) 未検査対照。**未検査の軸を含む下書きの判定が、全軸検査済みの下書きと
  **表示そのもので区別できること。**画面全体の但し書き 1 つでは不可。
  同じテストで次を否定対照として実行すること:
  `shader.fragment` に `engine://ssao.frag`(拡張子付きは必ず拒否される)/
  `engine://taa_resolve`(`.spv` が無く compiler OFF で拒否される)/
  `input` に `offscreen_depth`(SAMPLED 無し)/ `output.color` に `opaque_color`
  (COLOR_ATTACHMENT 無し)/ `input` に `swapchain`(名指しで拒否される)。
  **いずれも「妥当」と表示されないこと。**studio が拒否を予言できない軸は
  **軸名を伴って「判定していない」と表示されること**。
  `swapchain` は studio 単独で落とせる(`kind: frame_target` は 1 件のみ)
- **(E) 本番配線対照。**新規 view ソースが
  `src/devstudio/view/CMakeLists.txt` の `target_sources(pelican_studio PRIVATE ...)` に
  載っていること。**offscreen で `MainWindow` を構築し `findChild` で当該 widget が
  居ることを検査すること。**既存テストは widget ソースを `target_sources` で直接
  コンパイルしているので、**テストにだけ足して studio に配線しない偽装が現に可能である**
- **(F) 無副作用 —— `git status` 1 行では証明にならないので 3 条件に割る。**
  - **リポジトリ内に書かない**: `git status --ignored` で `projects/` 配下が変化しないこと。
    `.pelican/shader_cache/` はエンジン起動の正当な副作用なので、
    エンジンを起動しない構成で測るか**唯一の既知例外として名指しで除外**すること
  - **リポジトリ外に書かない**: 本 WP が追加するコードが `LayoutPresetManager` /
    `QStandardPaths` / `QSaveFile` / `QFile::open(WriteOnly)` を呼ばないこと
  - **反映しない**: 本 WP が追加するコードが `EmbeddedViewport::requestRpc` を
    `get_frame_plan` 以外のメソッドで呼ばないこと。
    **これが本 WP で唯一「反映しない」を検査する条件である。**
    studio は既に `edit` / `commit_preview` / `undo` / `save_scene` / `set_gizmo` を送っており、
    `requestRpc` はメソッド名を文字列で受ける汎用通しなので、宣言だけでは守られない
- **(G) 陳腐化対照。**下書きがある状態でフレームプランが差し替わったとき、
  **古い `resources` に基づく妥当判定が残らないこと。**
  同じテストで `resources` が異なる 2 プランを **Refresh の実経路で**順に与え、
  1 つ目で妥当だった選択が 2 つ目で妥当と表示されないこと。
  束縛の鍵は `graph` 名 + `resources` の名前集合とすること。
  **`runtime_generation` に束縛しないこと** —— エクステントだけの変化(ドックのリサイズ)で
  世代が上がるため偽陽性になる。下書きを毎回破棄する設計なら、
  「差し替え後の下書き復元は範囲外」と明記した上で破棄を検査すれば足りる
- `QT_QPA_PLATFORM=offscreen` で緑であること
- `ctest` 全数が緑(`-j4`)、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 中。

### WP321b: フォームが扱う型を増やす

**目的**: WP321a は `fullscreen` 固定である。型を増やす。

**範囲**: 型の一覧を `passFieldOwnershipTable()` から取り、**型ごとの鍵集合を表から導出する**
(写経しない)。`imgui` は**提示しない**(studio が有効性を判定できない)。
`raster` / `snapshot_copy` / `canonical_anchor` は鍵集合が別物なので、
除外するか各々の鍵集合を与えること。

**注意**: 「material は固有フィールドを要求するので作れない」は**誤りである。**
example の `gbuffer_pass` は `{name, type, output}` の 3 鍵で成立している。
material が作れない理由はその逆で、**フォームが常に `shader` を吐くと
所有しない型 5 つで所有検査に落ちる**からである。

**受け入れ条件**:
- **(B) 所有対照。**同じテストの中で、`shader` を持つ `fullscreen` の下書きが「妥当」と表示され、
  `type` だけを `shader` を所有しない型(`material` / `ui` / `snapshot_copy` /
  `canonical_anchor`)に切り替えると「不当」になること。
  **表示された文言が、同じ JSON に対して `validatePassFieldOwnership` が実際に投げた文言と
  一致すること** —— 期待文言をテストにリテラルで書かないこと
- **提示する型の集合が `passFieldOwnershipTable()` の型名の集合と一致すること**
  (部分集合でも「異なること」でもなく、**集合として等しいこと**)。
  studio が宣言した capability で除外した型があるなら、除外の根拠が同じ表から引けること
- 提示したすべての型について、フォームの出力が `validatePassFieldOwnership` を通ること。
  および、所有しない型に `shader` を混ぜた JSON が throw する否定対照
- `ctest` 全数が緑

依存: WP321a。見積: 中。

### WP322: フレームプランにシェーダーの同定を載せる

**目的**: **studio は、既存のパスがどのシェーダーで動いているかを知らない。**
`ssao_pass` ノードの鍵は
`declaration_index / kind / level / name / order / reads / reads_history / source / writes` で、
**30 ノードの鍵の和集合 15 個**を見てもシェーダー参照を運ぶものは無い。
`FramePlanNode` にもシェーダーの場は無い(`shader_contract` はマテリアル経路のもので別物)。

これは可視化そのものの穴である。「このパスは何をしているのか」を見に来た利用者に、
**読み書きするターゲットは見せているのに、実際の処理内容は見せていない。**

**範囲**: フレームプランの出力にパス毎のシェーダー参照を載せ、studio のモデルと
グラフ表示に通すこと。**記述だけ。書き込みも反映もしない。**

**これは WP321a のシェーダー選択肢の解にはならない。**
WP322 が運ぶのは**使用中の参照だけ**で、example では fragment 7 種にすぎず、
エンジンの 32 stem に対する部分集合である。しかも解決可能集合は
`PELICAN_RUNTIME_SHADER_COMPILER` に依存するため、単なる id 一覧では健全にならない。
資産列挙は D0 を越える別 WP が要る。

**受け入れ条件**:
- `projects/example` のフレームプランで `ssao_pass` のノードから `engine://ssao` が取得できること
- **状態が 3 つ区別できること**(否定対照): **宣言されている**(fullscreen 16 件)/
  **マテリアルから解決される**(`material` 2 件は `shader` 鍵を持たないが実際にはシェーダーが走る)/
  **本当に持たない**(`snapshot_copy` 2 件)。**空文字で潰さないこと**
- studio の表示から当該シェーダー名が読めること
- `ctest` 全数が緑

依存: なし。見積: 小。

### WP323: フレームプランにリソースの usage を載せる

**§4 規則 11 の中段**(`pelican_project` と複数経路)。**仕様レビューは省き、コードレビューのみ。**

**目的**: **著作側は `usage` を持っているのに、フレームプランが落としている。**
studio が「存在する ≠ 妥当」を判定できないのは、情報が来ていないからである。

#### 実測(2026-08-20 に再確認)

`projects/example` の著作宣言:

| ターゲット | format | usage |
|---|---|---|
| `gbuffer_normal` | `R16G16B16A16_SFLOAT` | `COLOR_ATTACHMENT`, **`SAMPLED`** |
| `offscreen_depth` | **`D32_SFLOAT`** | `DEPTH_STENCIL_ATTACHMENT`, `TRANSFER_SRC` |
| `opaque_depth` | **`D32_SFLOAT`** | `TRANSFER_DST`, **`SAMPLED`** |

フレームプランが運ぶのは 3 件とも `{kind, name, source}` **だけ**である。

**`format` では代用できない** —— `offscreen_depth` と `opaque_depth` は
同じ `D32_SFLOAT` で同じ `kind` なのに **input 可否が逆**である。
**`required_physical_features` でも代用できない** ——
SAMPLED を宣言していない `offscreen_depth` に `sampled_image@1` が立っている。

#### WP327 との関係(この節は WP327 より前に書かれた)

WP327 は `kind`(`render_target` / `frame_target` / `buffer`)から
役割別の候補集合を作った。**あれは代用であって解ではない。**
**同じ `kind` の中の適合差は依然として見えない。**本 WP がそこを埋める。

これが入ると、WP321a の受け入れ条件 (D) の
`input` に `offscreen_depth` / `output.color` に `opaque_color` の 2 例を
**「判定していない」から「不当」に移せる。**

#### 範囲

**記述だけ。書き込みも反映もしない。**WP322 と同じ形である。
`role` は載せない(`usage` から導けるものを二重に持たない)。

#### 範囲外

studio 側の判定の変更(本 WP は情報を運ぶところまで)。
`role` の導出。WP321a の受け入れ条件 (D) の書き換え。

#### 受け入れ条件(§4 規約 10)

- **`offscreen_depth` が SAMPLED を持たず `opaque_depth` が持つことが、
  同じテストの中でフレームプランから区別できること。**
  **この 2 つは format も kind も同じなので、これが本 WP の中心的な対照である**
- `gbuffer_normal` が `COLOR_ATTACHMENT` と `SAMPLED` の両方を持つことが読めること
- **studio の `FramePlanResource` から読めること。**
  同じテストで、載っていないプラン(旧形式)を与えたときに
  **「不明」と「空」が区別できること**(黙って空集合にしないこと)
- **エンジン由来のリソース**(`display` / `swapchain`)でも
  usage が読めるか、読めないなら**そう分かること**
- **変異 1 つで検出力を確かめること**
- `ctest` 全数が緑(**マージ直前に GPU 込みで 1 回**)、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 小〜中。

### WP324: パスの形の権威を一箇所に置く —— フォームは甘すぎ、かつ厳しすぎる

**WP321a のマージ後の敵対レビューで確認された欠陥。**
フォームは、エンジンが**構造的に拒否する** JSON を「妥当」と表示し、
同時に**出荷されている正しい形**を「不当」と表示する。

#### 甘すぎる(エンジンが拒否するのに妥当と出る)

| フォーム | エンジンの実際 |
|---|---|
| `output.color` 2 件以上 → **妥当** | `Fullscreen pass requires exactly one color output` |
| `output.depth` を選べて → **妥当** | `Fullscreen pass does not support depth output` |
| 同じターゲットを `input` と `output.color` に(**現フレーム読み**)→ **妥当** | `Pass cannot read and write the same color target` |
| `input` に同じターゲット 2 回 → **黙って 1 件に潰す** | `validateUniqueRenderTargets` が明示エラー |

#### 厳しすぎる(エンジンが受理し、出荷しているのに不当と出る)

| フォーム | エンジンの実際 |
|---|---|
| `output.color` に `swapchain` → **不当**(`frame_target` を一律拒否) | **合法。**`allow_swapchain` 経路があり、**`projects/vrm_xr_demo` の `lighting_pass` が出荷している** |
| `input` が空 → **不当** | **合法。**fullscreen に input 必須の規則は無く、`vrm_xr_demo` の入力なしパスが出荷されている |

#### 規則を誤解していた点(起票時の私の誤り。実装者は繰り返さないこと)

- **`@history` の読みは read/write 衝突から免除される。**
  `renderingpassvalidation.cpp` に `if (pass_def.input_target_history[input_index]) continue;` があり、
  **出荷 TAA が `input: [..., "taa_accum@history"], output: {color: "taa_accum"}` でこれに依存している。**
  規則は「**同一ターゲットの現フレーム読みと書きの併用を禁止**、`@history` は許可」である
- **`swapchain` は役割で決まる。**input は不可、depth output は不可、**color output は可。**
  一律の `frame_target` 拒否は誤りである
- **エラー文言を引いている既存テストは存在しない。**
  4 つの文言を `git grep` して 0 件だった。文言維持を要求するなら互換性方針として書くこと

#### 実装場所 —— `validatePassOutputs` は使えない

**`validatePassOutputs` を共通入口にしてはならない。**3 つの理由を確認した。

1. **正確な型が残っていない。**`PassDefinition` は `PassInfo` variant しか持たず、
   `makePassInfo` で **`fullscreen` と `output_transform` がどちらも `FullscreenPassInfo` になる。**
   型ごとの表を `PassDefinition` から引けない
2. **input がまだ読まれていない。**パーサは output を読んだ直後、
   input を読む前に `validatePassOutputs` を呼ぶ。
   read/write 衝突と重複は後段の別関数にある
3. 重複検査は `RenderTargetMetadataResolver` を使うが、`validatePassOutputs` の引数に無い

#### 設計 —— 純粋な評価器を project 層に置く

`src/project/passfieldownership.cpp` は既に **pelican_project** にあり、
`RenderPassType` もそこで定義され、**`pelican_core` は `pelican_project` を PRIVATE リンクしている**
(`src/core/CMakeLists.txt`)。層の向きは成立する。

**core / Vulkan の型を一切含まない**次を project 層に置くこと。

- `PassShapePolicy` —— 型ごとの形の規則
- `PassShapeObservation` —— **正確な `RenderPassType`**、color 名の列、depth 名、
  input 名の列と各々の `@history` フラグ
- `evaluatePassShape(policy, observation)` —— **名前付き違反**の列を返す

呼ぶ場所:

- **core**: 正確な型が残っていて、input と output が揃っている**著作 JSON の段階**。
  既存 validator の該当分岐は削除するか、この評価器へ委譲すること
- **studio**: 同じ評価器

**規則の値を 2 箇所に書かないこと。これが本 WP の中心である。**

**グローバルな可変表を作ってはならない**(決定性を壊す)。
`const PassShapePolicy &` を**明示的に注入**する形にし、
本番は不変の既定を 1 つ、テストは別の不変ポリシーを**両側に渡す**こと。

#### そのほか確認された欠陥

- **`resources[].kind` の欠落を空文字に落としている。**`kind` を省いたプランで
  `swapchain` が検査を素通りする。**fail-fast 違反。**
  しかも**同じ潰し方が 2 箇所にある** —— `fullscreenpasswidget.cpp` と `frameplanmodel.cpp`。
  **共有のデコーダに統合し、欠落 / null / 空 / 未知の 4 つをそれぞれ名前付きエラーにすること**
- **名前衝突の権威が実行時ノードだけ。**`authored_passes` は構築済みなのに衝突検査が見ていない。
  著作 config に無いグラフ名も妥当になる
- **貼り付け位置が実行時ノードの `declaration_index` 最大値 + 1** になっている。
  著作 `passes[]` の要素数であるべき。**fixture は著作 20 に対しノード 30・最大 index 29 なので、
  現行実装のままでも素朴な列挙テストは通ってしまう** —— 値そのものを検査すること
- **Refresh 失敗後に古い妥当表示が残る。**赤い文言だけ更新され、
  plan・binding・下書き・各軸は据え置き

#### 受け入れ条件(§4 規約 10)

- **権威が一箇所であること。**同じテストの中で、**既定と異なる不変ポリシーを両側に注入**し、
  **core 側の判定と studio 側の表示が両方その注入に従うこと**を検査すること。
  片方だけ既定のままなら写経が残っている。
  **グローバル可変表による差し替えを使わないこと**
- **形の判定が、エンジンの実挙動と全数一致すること。**同じテストの中で、
  color 出力 0/1/2 件 × depth 有無 × input が output と重なる(現フレーム / `@history`)×
  input 重複の有無 × `swapchain` を input / color output / depth output に置く、
  の組み合わせを**生成**し、各々について
  **フォームの表示とエンジンの受理/拒否が一致すること**を検査すること。
  **期待値をテストにリテラルで書かないこと。**
  テストは studio ではないので `pelican_core` を引いてよい
- **`@history` の否定対照。**同じテストで、`taa_accum` を現フレーム読みして書くと不当、
  `taa_accum@history` を読んで `taa_accum` を書くと妥当になること
- **`swapchain` の役割別対照。**同じテストで、input は不当、depth output は不当、
  **color output は妥当**になること。
  **`projects/vrm_xr_demo` の `lighting_pass` を実ファイルから読んで駆動し、妥当になること**
- **`input` 空が不当にならないこと。**同じテストで、入力を要求する形が別途不当になることを検査すること
  (シェーダーが入力を要するかは「判定していない」軸に属する)
- **`resources[].kind` の欠落 / null / 空 / 未知が、それぞれ名前付きエラーになること。**
  同じテストで正常な `kind` が通ること。
  **`FullscreenPassWidget` と `FramePlanModel` の両方の本番経路を通すこと**
- **名前衝突を 3 ケースで検査すること**: 著作にのみ在る名前 / 実行時にのみ在る名前 /
  どちらにも無い名前。前 2 つが不当、最後が妥当になること
- **著作 config にグラフが無いとき、衝突軸が「判定していない」であること**(「妥当」でないこと)
- **貼り付け位置の値が、著作 `passes[]` の要素数と一致すること。**
  `example` では 20 であって 30 ではない
- **Refresh 失敗後に、plan・binding・下書き・依存する各軸が個別に失効していること。**
  「以前の妥当が妥当と表示されない」だけでは、下書きを隠して軸を据え置く実装が通る
- **`SKIP_DEVSTUDIO=ON` でも権威のテストが登録されること。**
  純粋な project 層のポリシーテストは studio ターゲットの有無に依存させないこと。
  studio 統合テストだけを OFF 時に足すこと
- `ctest` 全数が緑(`-j4`)、`uv run tools/doclink.py check` が通ること

依存: WP321a(マージ済み)。見積: 大。

### WP325: WP321a のテストの抜け道を塞ぐ

**WP321a のテストは、実装を壊しても通る形が複数ある。**マージ後の敵対レビューで確認。

#### 確認された抜け道

- **(A) の期待値が循環している。**`formOwnedKeysFromAuthority` は
  **フォーム自身が付けた `pelicanPassField` プロパティ**を走査し、権威表はフィルタにしか使っていない。
  フォームが `input` を出すのをやめて対応する印も消せば、**期待投影も一緒に縮むので通る。**
  `validatePassFieldOwnership` は必須フィールドを検査しないため `{type}` だけでも通る。
  **既存の所有表は `name` / `input` / `output` / `order` を意図的に含まないので、
  そのままでは期待値の出所にならない**
- **(A)(D) の否定対照が偽物。**`unawarePartialForm` は widget を駆動せず空集合を返すテスト内関数、
  `falsely_all_checked` は取得した文字列の `"Not checked"` を `"Valid"` に置換しただけ。
  **どちらも「動いていない側」を実行していない**(規約 10 違反)
- **(F) の RPC 走査が空白 1 つで破れる。**`requestRpc (` と書けば当たらない。
  禁止語に `std::ofstream` / `fopen` / `QSettings` が無い。
  実行時の書き込み検査は `openProjectReadOnly()` を呼んでいないので、
  その経路に書き込みを足しても検出されない
- **(G) の 2 プランがグラフ名を共有している。**束縛鍵から `graph` を落としても通る
- **(E) は `#ifdef PELICAN_TEST_SOURCE_DIR` で迂回できる。**
  テストは `mainwindow.cpp` を独自ターゲットに直接コンパイルし、この定義を持つ。
  **ターゲットの `SOURCES` プロパティ検査に落としても抜け道は復活する** ——
  同じソースを production と test が別々にコンパイルすれば通ってしまう
- **`MainWindow` を構築するだけでは本番配線の証明にならない。**
  `openProjectReadOnly` の呼び出しを消しても、widget 直接テストは通る
- **レイアウトプリセットのテストが 6 dock で止まっている。**7 番目を含まない

#### 受け入れ条件(§4 規約 10)

各項目について、**「その抜け道を実際に使う変異を作ると落ちること」を確かめること。**
確かめていない項目は「確かめていない」と報告すること。

- **(A) の期待鍵集合を、project 層の純粋な関数から得ること。**
  「fullscreen のフォームが投影する共通 + 型所有フィールド」を返す
  **著作投影スキーマを project 層に定義する**(既存の所有表は `name` / `input` / `output` を含まないため)。
  テストの期待 JSON はその関数から作り、**widget のプロパティ・子・ヘルパを参照しないこと**
- **否定側を、同じ widget を著作コンテキスト無しで駆動する形にすること**
- **(D) の否定対照を、同じ widget の別状態にすること。**文字列置換をやめること。
  **利用者に見える集約表示とコピー可否**を含めて比較すること
- **(F) をソース文字列走査ではなく、注入した capability の spy にすること。**
  widget に `EmbeddedViewport` の汎用能力を渡さないこと。
  RPC は非同期なので**同期の `getFramePlan()` 一本では足りない** ——
  `ready` / `requestFramePlan` / `result` / `failure` だけを持つ**非同期の読み取り専用 capability**にすること。
  `MainWindow` が `EmbeddedViewport` からアダプタを作り、
  書き込みが要る widget は従来の capability を使い続けること
- **書き込み検査で、project を開く本番経路を実行すること。**
  Qt の writable location と作業ディレクトリをテスト用の一時領域に隔離し、
  **project と全 writable root の前後を比較すること。**禁止語の列挙に依存しないこと
- **本番配線を、production と test が同一のコンパイル済み `pelican_studio_view` ターゲットを
  リンクする形で保証すること。`SOURCES` プロパティ検査への代替は認めない。**
  加えて **`MainWindow` の本番の project-open 経路を操作し、
  フォームに著作グラフとパスが実際に流入したことを検査すること**
- **(G) に第 3 のプランを足すこと** —— resource 集合が同一でグラフ名だけ異なるもの。
  Refresh の実経路で与え、下書きが破棄されること
- **レイアウトプリセットのテストを 7 dock にすること。**
  6 dock の状態を 7 dock の `MainWindow` に復元し、既存 dock の
  area / tab / visibility と、`View > Panels` の 7 件・objectName の一意性を検査すること
- `ctest` 全数が緑(`-j4`)

依存: WP321a(マージ済み)。WP324 と並行してよい。見積: 大。

### WP326: WP324 がエンジンの検証を 3 箇所で弱めた(最優先)

**WP324(`0980ad6`)のマージ後の敵対レビューで確認。**
`renderingpassvalidation.cpp` から 127 行を削って評価器へ委譲したが、**等価になっていない。**
出荷中の宣言はすべて通る(23 JSON・63 パスを再パースして違反 0 件を確認済み)が、
**以前は拒否されていた入力が受理されるようになった。**

#### 後退 1: material の名前付き入力で現フレーム read/write が通る

形の評価は **`screen_inputs` / `surface_resources` / `material_resources` を parse する前**に走る。
これらは評価後に `input_targets` へ追加される。
旧 `validatePassInputs` は**統合後の列**を検査していたので、同じターゲットを
現フレームで読み書きする material パスを拒否していた。**いまは通る。**

再現(出荷 example の `forward_transparent` を改変):
`screen_inputs: {"opaque_color": "lit_color"}` と `output.color: ["lit_color"]`。
`lit_color` は `COLOR_ATTACHMENT | SAMPLED` なので usage・format・contract 検査も通ってしまう。

**直し方**: 正確な `pass_type` を保持したまま、**全入力ソースを parse した後**に観測を作ること。
`PassDefinition` の解決済みターゲットと history メタデータから同じ評価器へ委譲すること。
**旧 validator を復活させないこと**(権威が 2 つに戻る)。

#### 後退 2: `swapchain` という名前の buffer で input 禁止を迂回できる

旧コードは buffer 判定の**前**に `if (input.name == "swapchain") throw` を無条件で置いていた。
新コードはそれを削り、拒否を評価器の `input.image` 条件に委ねている。
**`buffer_names` に `"swapchain"` があると buffer 分岐が先に取り、誰も拒否しない。**
buffer 定義側に予約名の検査は無い。

**直し方**: `swapchain` を**全リソース種で予約名**にすること。
加えて既定ポリシーの input 検査は、`image` かどうかに関係なく
リテラル `"swapchain"` を拒否すること。

#### 後退 3: `frameplanner` が評価器を一度も通らない —— WP303 と同型

`frameplanner.cpp` の `evaluatePassShape` 呼び出しは **0 件**。
同じ著作 JSON から独立に reads/writes を作り、
**duplicate input を `appendUnique` で黙って 1 件に潰し**、
型ごとの個数検査なしに出力を追加する。
`Depth output target cannot be swapchain` だけは**独自に重複実装されている。**

したがって 2 color + depth + duplicate input の fullscreen が、この経路では有効なプランになる。

**直し方**: 著作パスの共通の事前検証を、**frameplanner と PassDefinition パーサの両方が
必ず通る境界**に置くこと。**frameplanner の中に規則を書き写してはならない。**

#### 受け入れ条件(§4 規約 10)

- **後退 1〜3 それぞれについて、`0980ad6^`(WP324 前)が拒否し `0980ad6` が受理する
  具体的な JSON を、テストの中で実際にパースして拒否されること。**
  同じテストの中で、**正当な近傍**(`@history` を付けた material 入力、
  `swapchain` でない buffer 名、正しい形の fullscreen)が受理されることを検査すること
- **`frameplanner` 経路と `PassDefinition` パーサ経路の両方に、同じ不当な JSON を通し、
  両方が同じ名前付き違反で拒否すること。**
  **片方だけを検査する条件にしないこと** —— これが後退 3 の本体である
- **`swapchain` が全リソース種で予約されていること。**
  buffer として宣言する JSON が名前付きエラーになること。
  同じテストで別名の buffer が通ること
- **既定ポリシーの値が 1 箇所であること。**`frameplanner` の
  `Depth output target cannot be swapchain` の重複実装を消すか、評価器へ委譲すること
- 出荷 4 プロジェクトとエンジンの feature / pipeline がすべて通ること
- `ctest` 全数が緑(`-j4`)、`uv run tools/doclink.py check` が通ること

依存: WP324(マージ済み)。**WP325 と衝突しない**(core 側が中心)。見積: 中。**最優先。**

### WP327: WP324 の studio 側の誤表示と寿命問題

**WP324 のマージ後の敵対レビューで確認。**エンジンの検証とは別に、
studio 側に誤表示が残り、注入したポリシーの寿命が保証されていない。

#### 誤表示 —— リソース種を役割で分けていない

**全 `resources[]` が input / color / depth の全 chooser に入る。**
`Target names` 軸は存在しか見ず、形の評価器は color output の種を受け取らない。

したがって次がすべて「妥当」と表示される。エンジンは拒否する。

- **`buffer` を唯一の color output にする** → core は `Color render target not found`
- 名前が `swapchain` でない `frame_target` を input / color にする
- **buffer に `@history` を付ける** → core は `@history is supported only for render targets`
- history 非対応の render target に `@history` を付ける

**直し方**: 役割ごとに候補を分けること。
color は `render_target` と名前が `swapchain` の `frame_target`、
depth は `render_target`、input は `render_target` と `buffer`
(`frame_target` は出すなら明示的に不当)。**buffer では history 操作を無効化すること。**
render target の history 対応可否はフレームプランに出すか、
少なくとも**専用の軸を「判定していない」にすること。**

#### ポリシーの寿命

`Impl` は注入ポリシーを `const &` のまま保持する。
公開コンストラクタは temporary も受け取れるので、**構築後に dangling reference になる。**
非 const 変数を渡して外から変更すると、**同じ下書きの表示が後から変わる**(決定性の破壊)。

**直し方**: コンストラクタが `const PassShapePolicy &` を受けても、
**`Impl` はポリシーを値として所有すること。**
少なくとも rvalue オーバーロードを delete し、寿命を型で保証すること。

#### `vrm_xr_demo` の実ファイル対照が規約 10 を満たしていない

実ファイルから読んでいるが、**同じ TEST_CASE の中に効いていない側が無い。**
実行時のフレームプランも使わず、リソース列を手で作って `swapchain` を手動追加している。

**直し方**: 同じ実ファイル TEST_CASE の中で、実際に解決・合成した
フレームプランをフォームへ渡し、その後に **swapchain-color を禁止した注入ポリシーを両側へ渡して
core とフォームが共に不当になること**、および解決されたポリシー値を検査すること。

#### 受け入れ条件(§4 規約 10)

- **役割ごとの候補集合が、フレームプランの `kind` から導かれること。**
  同じテストの中で、`buffer` / `render_target` / `frame_target(swapchain)` /
  `frame_target(別名)` を含むプランを与え、**各 chooser の候補集合が役割ごとに異なること**を
  検査すること。全 chooser が同じ集合になる実装は落ちること
- **上記 4 つの誤表示それぞれについて、フォームが「妥当」と表示しないこと。**
  同じテストで、それぞれの正当な近傍が妥当になること
- **注入ポリシーを temporary で渡しても、構築後の表示が正しいこと。**
  および、渡した変数を外から変更しても**表示が変わらないこと**(値所有の対照)
- **`vrm_xr_demo` の TEST_CASE の中に、効いていない側があること。**
  同じテストで注入ポリシーを変えて core とフォームが共に反転すること
- `ctest` 全数が緑(`-j4`)

依存: WP324(マージ済み)、WP325(テスト構造を変えるため後に回す)。見積: 中。

### WP328: 経路を数え上げるのをやめる —— 検証済み config を型で強制する

**WP326 のマージ後の敵対レビューで確認。WP326 の修理は半分だった。**
そして原因は WP326 個別のミスではなく、**塞ぎ方が間違っている**ことである。

同じ形を **3 回**踏んでいる。WP303、WP326、そして本 WP。
**経路を数え上げて 1 つずつ塞ぐ限り、次が必ず出る。**

#### 確認された残欠陥(6 件、すべて敵対レビューで再確認済み)

1. **material の名前付き入力を `frameplanner` が検証しない。**
   観測は `output` とトップレベル `input` しか読まない。
   `PassDefinition` 経路は全入力を解決した後に評価器を**再度**通すので直ったが、
   `frameplanner` は不完全な観測を検証したきり再検証しない。
   **WP326 の回帰テスト 1 は `PassDefinition` しか呼んでいない。**
   両経路を通すのは後退 3 の分だけだった。**受け入れ条件の書き方が原因である。**
2. **buffer 種別を渡さないため両経路が逆向きに食い違う。**
   `frameplanner` は非 image 入力に `{}` を渡すので buffer が image と誤分類される。
3. **`preview` / `data_only` が shape 検証を一度も通らない。**
   `data_only` は型付き定義を作る前に return し、
   `makePreviewGraphProgram()` は壊れたグラフを `continue` し名前が無ければ `"unnamed"` に落とす。
4. **公開低水準 API が旧版より弱いまま。**
   `parseDepthOutputTargetFromJson` / `parseInputTargetsFromJson` /
   `parseInputResourcesFromJson` に `"swapchain"` を直接渡すと、
   `0980ad6^` は名前付きエラーで拒否したが現在は特別 ID を返す。
5. **出荷コーパステストが shape 終端を通っていない**(resolve/compile で止まる)。
6. **複合不正 JSON の診断順序が変わった**(解決前に shape 検証するため、
   より局所的な「存在しない名前」が隠れる)。

#### 設計 —— 「一度だけ」は成立しない。**revision ごと**に検証する

**当初案の「artifact 分岐の前に一度だけ検証する」は破綻している。**
分岐点に在るのは生の `normalized_config` だけで、**その後も config は書き換わる**:

- `PELICAN_WITH_IMGUI=ON` は検証後に**生 JSON の pass を挿入する**
- **graph transform は V1 ABI で完全な JSON config を受け取り直し、
  候補を直接 frameplanner に入れる**
- **subgraph replacement も生 JSON の pass 群を返す**

元 config のトークンは、変換後の候補の証明にならない。

したがって:

1. **各 immutable な config revision の、最後の書き換えの後**に共有 validator を通す。
   **preview / flat / xr は別 revision として扱う。**
2. **V1 provider(graph transform / subgraph replacement)の出力は毎回再検証する。**
   ABI を型付き編集命令に変えるのは V2 ABI の話であり、本 WP の範囲外。
3. **型名は `PassShapeValidatedConfig` とする。**「完全検証済み」を名乗らないこと ——
   material の source type 互換性など、metadata を要する検証は後段に残る。

#### 型の要件(トークンでは不十分)

- **private constructor。**生 JSON から構築できないこと。
- **不変の config を型が所有すること。**mutable な JSON を公開しないこと。
- **パスの取り出しは `ValidatedPassView` を通し、その config 自身からしか得られないこと。**
  `(token, rawPassJson)` の形にすると **config A のトークンと config B の pass を混ぜられる。**
- **variant・policy・リソース種別の情報を型に結び付けること。**
- **custom `RenderCompilerProgram` の出力は `runRenderCompilerProgram()` が
  必ず検証してから publish すること。**現状は object かどうかしか見ていない。

#### 観測の要件

**「最終 read 集合」では足りない。**`frameplanner` の `appendUnique` は観測前に重複を潰し、
名前は render target と buffer で別々の集合から作られていて衝突を検査していない。

- 観測は集合ではなく **`{name, kind, history, source_field, occurrence}` の順序付き multiset** とする。
- **image 名と buffer 名の積集合は `resource_kind_collision` として fail-fast すること。**
  `renderresourcename.hpp` は「一つの logical namespace」と明記しているのに、
  現状は cross-kind 衝突を検査していない。
- material の名前付き入力(`screen_inputs` / `surface_resources` / `material_resources`)を
  観測に含めること。**contract 解決は不要である** ——
  読み取り名・history・リソース種別の候補は生 JSON から集められる(レビューで反証済み)。

#### 診断の段階(**実装者に決めさせないこと**)

次の順で評価し、各段階に error code を持たせること。

`structure → ownership → resource-kind / name → shape → material contract / usage`

- ownership と graph-variant の検証は**中央検証より前**に走る(`renderpipeline.cpp`)。
  preview の history も preview 固有エラーが先に出る(`graphvariantpolicy.cpp`)。
  **これらを同じ段階付き validator に含めるか、「全経路で同一」の範囲を明示的に限定すること。**
- 既存テストは ownership の文言と shape violation 名、
  および二経路のメッセージ一致に依存している。**壊さないこと。**

#### 終端は 7 つある(3 つではない)

| trust route |
|---|
| default runtime frameplanner |
| GPU 登録後の `PassDefinition` |
| coordinated preview / data-only |
| standalone `precompilePreviewGraph()` |
| graph-transform candidate |
| subgraph-replacement candidate |
| custom `RenderCompilerProgram` output |

加えて生の公開 API `parseFrameGraphDefinitionFromJson()` /
`parseFrameGraphDefinitionsFromConfigJson()` が残っている。
**削除・内部化するか、コンパイル不能条件に含めること。**

#### 受け入れ条件(§4 規約 10)

- **不正 fixture × trust route の全組み合わせ。**
  **関数名ではなく上表の 7 経路ごとに検査すること。**
  provider / custom / standalone の**配線**を通ること。
  **1 つの経路だけを呼ぶ回帰テストを書かないこと** —— WP326 が半分で終わった原因である
- **各 trust route について、同じ TEST_CASE の中で不正 fixture と
  最小の正当な近傍を両方通し、`name` / `kind` / `history` / `source_field`、
  preview の pass 名、最終 plan の値を検査すること。**
  コーパスの成功を別テストに置くと規約 10 を満たさない
- **duplicate buffer(`input: ["b","b"]`、`b` は buffer)は
  全終端で「受理」されること。**
  ポリシーは非 image 入力を duplicate 検査から意図的に除外している。
  **現状 `frameplanner` だけが拒否するのが欠陥である。**
  ポリシーを変えて全終端で拒否させるなら、その変更を明示すること
- **`b@history` は全終端で同じ名前付きエラー(`buffer_history`)であること**
- **`resource_kind_collision`**: 同じ名前を render target と buffer の両方で宣言すると
  名前付きエラーになること。同じテストで衝突しない宣言が通ること
- **生 JSON から `PassShapeValidatedConfig` を構築できないことを、
  実行時ではなく `static_assert` または `try_compile` で示すこと**
- **公開低水準 API に `"swapchain"` を直接渡したときの挙動が、
  `0980ad6^` と同じ名前付きエラーであること**(または当該 API が到達不能になっていること)
- **診断段階が仕様どおりであること。**複合不正 JSON について、
  どの段階のどの error code が出るかを固定すること
- **構成マトリクスを明示すること。**
  `PELICAN_WITH_IMGUI` ON(**実際に callback が発火する構成**)/ OFF、
  `PELICAN_WITH_OPENXR` の active / inactive(flat・xr・preview の 3 variant)、
  `PELICAN_RUNTIME_SHADER_COMPILER` OFF は
  「optional subset は成功、required は同じ名前付き拒否」とすること。
  **現コーパステストは graph variant を flat に固定している。**
- `ctest` 全数が緑(`-j4`)、`uv run tools/doclink.py check` が通ること

依存: WP326・WP327(マージ済み)。見積: 大。**最優先。**

**注**: WP326 の記述にある「127 行を削除」は不正確である。
実差分は **3 行追加・124 行削除**(変更行合計 127)。

### WP329: ノードを選ぶと、そのパスの詳細が出る

**§4 規則 11 の下段**(studio 内で閉じる・読み取りのみ・表示だけ)。
**設計レビューも仕様レビューもかけない。マージ後のコードレビューのみ。**

**目的**: 利用者は「ノードをクリックするとそれの詳細情報を表示したい」と言った。
現在ノードをクリックしても**何も起きない**。Qt 既定の選択枠が出るだけである。

#### 材料は既に在る。繋がっていないだけである

- `FramePlanGraphicsScene::selectedNode()` は実装済みだが **呼び出し元がゼロ**
- dynamic property `pelicanSelectedNode` / `pelicanSelectedGraph` を読むのは**テストだけ**
- `selectionChanged` → `recordSelection()` の接続は在るが、**シグナルを出さない**
- `logical_scene` と `std::optional<FramePlanModel> model` は**同じ `FramePlanWidget::Impl` に同居している**

**新しい型も RPC も書き込みも増やさないこと。**
Inspector と `SelectionModel` に触らないこと(あれはシーンオブジェクト専用で、
`FramePlanNodeKey` は型として入らない)。

#### 実装範囲

1. `FramePlanWidget::Impl` で `logical_scene` の `selectionChanged` を購読し、
   `selectedNode()` から鍵を取り、`model->nodes` を引く。
2. **Logical graph タブの中に詳細ペインを 1 枚置く。**
   `populatePasses()` が既に使っている表示規則を流用すること。**別の流儀を作らない。**
3. 選択が無いときは、その旨を出すこと。**空白にしないこと。**

#### 出せる情報の上限(実測)

`FramePlanNode` のフィールドはほぼ全て Passes ツリーに出ている。
**完全に未表示なのは `material_filter.resolution_provenance` の 1 個だけ**である。
本 WP の価値は「新しい事実」ではなく「**タブを往復せずに済む**」ことである。
**それ以上を約束しないこと。**

**シェーダーは出せない。**フレームプランがシェーダー参照を運んでいない(WP322 未着手)。
詳細ペインは WP322 が入ったときの受け皿になる。

#### 範囲外

書き込み。編集。ノードの追加・削除・接続。Inspector への統合。
`FramePlanNodeKey` の永続化。

#### 受け入れ条件(§4 規約 10)

- **ノードを選ぶと、そのノードの詳細が出ること。**
  `QT_QPA_PLATFORM=offscreen` で `example_frame_plan.json` を読み、
  ノードを選択し、**そのノードの `name` / `kind` / `reads` / `writes` が
  ペインから取得できること。**期待値は `model->nodes` から取り、
  **テストにリテラルで書かないこと**
- **否定対照**: 選択を外すと、詳細が消える(または「選択なし」になる)こと。
  **同じテストの中で**両方を実行すること
- **変異 1 つで検出力を確かめること。**
  `selectionChanged` の購読を外すと当該テストが落ちることを実際に確かめ、報告すること
- `ctest --test-dir ./build -C Debug -LE gpu -j16` が緑(**内周は GPU を回さない**、§0)
- `uv run tools/doclink.py check` が通ること

依存: なし。見積: 小。

### WP330: ワークスペース状態を保存して、起動時に戻す

**§4 規則 11 の下段**(studio 内で閉じる)。**マージ後のコードレビューのみ。**

**目的**: 利用者は「タブ切り替えも好きにレイアウトしたい」と言った。
調べたところ**ドックは既に自由に動かせる**。足りないのは 2 つである。

1. **起動時に戻らない。**`MainWindow` のコンストラクタが**無条件に**
   `applyDefaultLayout()` を呼び、`restoreState` への到達経路は Layout メニューだけである。
   **動かした配置は次回起動で必ず消える。**
2. **ツール内配置がゼロ。**`FramePlanWidget` は素の `QTabWidget` 9 枚固定で、
   **`setMovable` すら呼んでいない。**並べ替えもできない。

設計は「**エディタのワークスペース状態**」節に従うこと。**先にその節を読むこと。**

#### 実装範囲

1. **起動時にパネル配置を復元すること。**
   終了時に予約名のプリセットへ保存し、起動時に復元する。
   **失敗時(形式が古い・ドックが増減した・壊れている)は名前付きで既定に落ちること。**
   黙って落ちないこと。
2. **ツール内配置を保存できるようにすること。**最小で:
   - `FramePlanWidget` の 9 タブを**並べ替え可能にする**(`setMovable`)
   - **タブ順と、WP329 が入れた `QSplitter` の分割位置**を保存・復元する
3. **保存先は `QStandardPaths::AppConfigLocation` 配下**(`LayoutPresetManager` の現在地)。
   **プロジェクトの中に書かないこと。**
4. **パネル層とツール内層は独立した名前を持つこと。**
   片方を選ぶともう片方が連動する形にしないこと。

#### 範囲外

ノード位置の永続化(WP318 の続き。層は同じだが本 WP には入れない)。
プロジェクトへの同梱・export。タブの引き剥がし・個別ドック化。

#### 受け入れ条件(§4 規約 10)

- **配置を変えて `MainWindow` を作り直すと、その配置が戻ること。**
  `QT_QPA_PLATFORM=offscreen` で、ドックを既定と異なる area へ移し、
  保存 → 新しい `MainWindow` を構築 → **移した先に居ること**を検査する。
  **否定対照**: 保存を経ないで構築すると既定配置になること。**同じテストの中で**
- **タブ順を変えて保存・復元すると、その順で戻ること。**
  同じテストで、保存前と後の順が異なることを検査する
- **壊れた保存データから復元しようとすると、名前付きで既定に落ちること。**
  黙って例外を投げたり、空のウィンドウにならないこと
- **ドックが増えても古いプリセットが復元できること。**
  既存の `devstudio_layoutpreset_test.cpp` の検査を壊さないこと
- **プロジェクトのファイルが変化しないこと。**
  `git status --ignored` で `projects/` 配下が変わらないこと
- **変異 1 つで検出力を確かめること。**復元の呼び出しを外すと当該テストが落ちることを
  実際に確かめ、報告すること
- `ctest --test-dir ./build -C Debug -LE gpu -j16` が緑(**内周は GPU を回さない**)
- `uv run tools/doclink.py check` が通ること

依存: なし。見積: 中。

### WP331: 著作 config の `features[]` を編集して、再起動なしで反映する

**§4 規則 11 の上段**(エンジンの挙動が変わる)。
**「編集側の設計 — 5 回目」節と `docs/design_editor_tooling.md` を先に読むこと。**

#### 初版が崩れた原因(繰り返さないこと)

初版は `gpu_timing` を中心の例に据えたが、**hot-add できない。**

- `RenderTiming` は**起動時に feature が有効なときだけ**生成される(`loop.cpp:154` 近傍)
- モジュール生成は RPC 開始前に **freeze** される(`prepareRuntimeModuleGraph`)
- freeze 後に新たに有効化された feature にモジュールが無ければ、
  preflight が**名前付きで拒否する**(`renderer_config.cpp:158`)

また初版は「実効 config に現れること」を対照にしたが、
**composer は解決後の JSON から `features` を消す**(`featurecompose.cpp:2763`)。
**その対照は永久に通らない。**

#### モジュールを要求する feature は 6 つだけである(実測)

`validateFrozenRuntimeFeatureModules`(`renderer_config.cpp:158-197`)が名指ししている:
**`debug_draw` / `debug_text` / `gizmo` / `gpu_timing` / `ui` / `sprite`**。

**この 6 つは v1 の範囲外である。**
残りはグラフに寄与するだけで、既存の render pipeline reload 経路が扱える。

**UI はこの 6 つを黙って隠さないこと。**選ぼうとしたら
**「モジュールの動的生成が要るため、再起動なしでは有効化できない」と名前付きで断ること。**
黙って選択肢から消すと、なぜ出ないのかが誰にも分からなくなる。

#### v1 の例は `sky_ambient`(実測で選定)

- `passes` を **1 つ**寄与し、`render_targets` は **0**
- **モジュール要求なし**(上の 6 つに含まれない)
- したがって **frame plan に `provider_feature` 付きのノードが増える** ——
  **利用者が実際に見ている画面で観測できる**

**対象プロジェクトでは、その feature が編集前に有効でないこと**を選ぶこと。
`animgraph_demo` は既に `shadow_directional` / `sky_ambient` / `ui` を持つので、
このプロジェクトで `sky_ambient` を「足す」対照は作れない。**削除側で対照を作ること。**

#### 確認済みの前提

- `features` は著作 config の **top-level**(`featurecompose.cpp:128`)
- **reload 参加者は `PathResolver.loadText(state.source_reference)` でファイルを読み直す**
  (`renderer.cpp:3071`)。**config の bytes を RPC で送る必要はない**
- `pelican.render_pipeline` 参加者は**登録済み**(`renderer.cpp:2838`)
- **使える feature を列挙する経路が存在しない**(RPC も `pelican_project` からの到達路も無い)

#### 契約から流用すること / 新しく決めること

**流用する**(`design_editor_tooling.md`):

- **正本は engine が持つ document。**runtime は投影
- **edit は受理と ticket を同期応答し、結果は別途取得する**(studio の RPC は全 method 一律 5 秒)
- **保存は preflight → 一時ファイル → atomic replace。digest 不一致は
  `external_modification` で何も書かずに拒否**
- **`ReloadGate` の `enabled()` を流用しない。`can_edit` を独立 query として追加する** ——
  gate は `--rpc` のとき必ず無効で、**studio は必ず `--rpc` で起動する**

**新しく決めること(契約の流用ではない、と明記する)**:

- **byte-lossless な編集は、レンダリング config 用の新しい字句上の契約である。**
  scene 側の契約は raw JSON の保持であって、原文 bytes の保持ではない。
  **「既存契約の流用」と書かないこと。**
- **v1 の CAS は root ファイルの digest だけを見る。**
  preset / feature / overlay を含む runtime の同定には足りない。
  **v1 は root の `features[]` しか編集しないので、その範囲では足りる**と明記すること。
  **preset や feature が供給した feature 参照は編集対象外である。**

#### 保存と適用の順序(初版の欠陥)

初版は「保存 → `applyRuntimeNow`」の順で、**適用が失敗すると disk に不正な config が残る。**

**候補を先に preflight すること。**適用が通ると分かってから書く。
書いてから適用して失敗した場合は、**disk を編集前に戻すこと。**
「適用失敗時に disk と runtime の双方が編集前」を成立させること。

#### 実装範囲

1. **engine が所有する著作 config の document。**原文 bytes と digest と
   `features` 配列の位置を保持する。**意味 JSON を再直列化しないこと。**
2. **`can_edit` query**(`ReloadGate` と独立)。
3. **feature の列挙 RPC。**エンジンが自分の feature 文書を列挙する。
   **モジュールを要求する 6 つには印を付けて返すこと**(隠さない)。
   **studio 側に一覧を写経しないこと。**
   これは D0 の一般解であり、**シェーダー stem の列挙も後から同じ機構で足す。**
4. **`features[]` の編集 RPC**(要素の追加・削除)。受理と ticket を同期応答。
5. **保存**: preflight → 一時ファイル → atomic replace、digest CAS。
6. **適用**: `pelican.render_pipeline` を名指し。返値は
   `{published_generation, source_digest, committed}`。
   **publish 後の後始末の失敗を commit 失敗と混同しないこと。**
7. **studio の UI**: 現在の `features[]` を一覧、列挙 RPC から選んで追加、選んで削除、適用。
   **`MainWindow` に配線すること。**

#### 範囲外

パス属性の編集。preset の eject。世代保持。journal と undo。
モジュールを要求する 6 feature の動的有効化。シェーダー stem の列挙。

#### 受け入れ条件(§4 規約 10)

- **中心対照**: グラフに寄与する feature(例: `sky_ambient`)を `features[]` に足して適用すると、
  **再起動なしで frame plan に `provider_feature` がその feature のノードが現れること。**
  **同じテストの中で、足す前には現れないこと。**
  加えて `RendererRuntimeGeneration::enabled_feature_names` に現れること
- **削除側**: 既に有効な feature を外すと、そのノードが消えること。同じテストで
- **モジュール要求 feature の否定対照**: 6 つのいずれかを足そうとすると、
  **名前付きで拒否されること**(黙って無視しない・黙って成功しない)
- **no-op 保存が byte-identical であること。**同じテストで、
  1 要素足すとその差分だけが変わること
- **外部で変更されたファイルへの保存が `external_modification` で拒否され、
  何も書かれないこと。**同じテストで、変更されていなければ通ること
- **適用が失敗したとき、disk と runtime の両方が編集前のままであること**
- **`can_edit` が `ReloadGate` と独立であること。**
  `--rpc` 下(gate は無効)で編集が通ること
- **列挙 RPC が実データ由来であること。**返る集合がエンジンの feature 文書と一致し、
  **studio 側にハードコードした一覧が無いこと**
- **本番配線**: offscreen で `MainWindow` を構築し、当該 widget が居ること。
  テストにだけ存在する実装が通らないこと
- `ctest` 全数が緑(**マージ直前に GPU 込みで 1 回**)、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 大。

#### マージ時に記録した宿題(2026-08-19)

**モジュールを要求する 6 feature の名前が 3 箇所にある。**

1. `renderFeaturesRequiringRuntimeModules()` の `constexpr` 配列
   (`src/core/vkcore/renderer_config.cpp`)
2. `validateFrozenRuntimeFeatureModules()` の if 連鎖(同ファイル)
3. `test/renderconfigeditor_test.cpp` の期待集合リテラル

**3 つとも現在は一致しているが、一致を保つ機構が無い。**
7 つ目を if 連鎖に足しても、配列とテストは変わらず**テストは緑のままである。**

**次にこの周辺を触る WP で、`{feature, 要求モジュール}` の表を 1 つ作り、
if 連鎖がそれを歩き、配列がその名前を投影する形にすること。**
テストの期待集合もその表から作ること。
本 WP のマージ時に直さなかったのは、エンジンのコードを
マージ直前に組み替える risk が、将来のずれの risk を上回ると判断したためである。

### WP332: カタログの嘘と、no-op でない no-op を直す

**WP331(`07dff76`)のマージ後のコードレビューで確認。**
**§4 規則 11 の中段**(`pelican_project` と複数経路)。**仕様レビューは省き、コードレビューのみ。**

レビューは 7 件を挙げたが、**直すのは 2 件と、既に記録済みの宿題 1 件である。**
残り 4 件は下の「記録のみ」に置く。**個別に WP を作らないこと。**

#### 直す 1: カタログが実行可能性を偽る

`hot_add_supported` は「モジュール要求 6 feature でないこと」だけから算出され、
studio はそれだけで `Available` と表示する。

**しかし apply は他の理由でも拒否する。**

- **`PELICAN_RUNTIME_SHADER_COMPILER=OFF` では、feature 文書に指定が無いものは
  compiler 必須として扱われる**(`featurecompose.cpp:2603, 2752`)。
  **`sky_ambient` は指定が無いので、カタログは Available、apply は拒否になる。**
  これは WP331 が中心対照に使った feature そのものである
- `rt_shadow_mask` は `"runtime_shader_compiler": "optional"` なので OFF だけを理由に隠す必要はないが、
  **`ray_query` capability の無いデバイスでも Available と出る**

**直し方**: **エンジンが判定した可用性を返すこと。**
ビルドフラグ・デバイス capability・モジュール方針を**同じ判定器**で見て、
`available` と `unavailable_reason` を返し、**studio はそれをそのまま表示する。**
**studio 側で理由を組み立てないこと。**

#### 直す 2: no-op が runtime 上で no-op でない

`operations: []`(および同じ feature の再追加)は受理され、
disk の bytes が同じなら replace は省略されるが、**runtime apply は続行される。**
結果として**新しい generation を publish し、history を reset する。**

**適用を二度押しすると、TAA などの時間蓄積が理由なく飛ぶ。**

既存テストは disk bytes しか見ていない。

**直し方**: candidate の digest が現在の document と同じなら
**runtime apply を呼ばず、現在の generation で commit 扱いにするか、名前付きで `no_change` を返す。**

#### 直す 3: モジュール要求 feature の名前が 3 箇所にある(WP331 で記録済み)

1. `renderFeaturesRequiringRuntimeModules()` の `constexpr` 配列
2. `validateFrozenRuntimeFeatureModules()` の if 連鎖(同ファイル)
3. `test/renderconfigeditor_test.cpp` の期待集合リテラル

**`{feature, 要求モジュール}` の表を 1 つ作り、if 連鎖がそれを歩き、
配列がその名前を投影する形にすること。**テストの期待集合もその表から作ること。

#### 記録のみ(本 WP では直さない)

- **digest CAS は最終確認後の競合で迂回できる。**外部書き込みが狭い窓に入る必要がある
- **apply が live preview lease を無視する。**preview 中にのみ効く
- **publish 後の例外で disk と runtime が分離する。**特定の窓での例外が必要
- **top-level `features` が無い config では、編集機能が黙って消える。**
  一般的な「service is unavailable」になる。**出荷 4 プロジェクトはすべて `features` 鍵を持つ**
  (`vrm_xr_demo` は空配列)ので、今日は誰にも当たらない。
  ただし fail-fast 違反なので、`render_config_features_missing` 等で名前付きに落とすこと
- **power-loss durability が無い。**temporary の flush、directory sync が無い

#### 受け入れ条件(§4 規約 10)

- **カタログの可用性がエンジンの判定と一致すること。**
  同じテストの中で、**apply が拒否する feature がカタログで `available` にならないこと**、
  および apply が通る feature が `available` になることを検査する。
  **`PELICAN_RUNTIME_SHADER_COMPILER=OFF` の構成で、`sky_ambient` が
  `unavailable` かつ理由付きになること**(§4 規則 9 —— 触れた gate の対照構成を実際にビルドする)
- **studio が理由を組み立てていないこと。**表示された文言がエンジンの返した文言と一致すること
- **no-op の適用で generation が進まないこと。**
  同じテストの中で、実際の変更では進むことを検査する。
  **disk bytes だけでなく generation と apply 呼び出し回数を検査すること**
- **名前の表が 1 つであること。**表に 7 つ目を足すと、
  `validateFrozenRuntimeFeatureModules` と カタログとテストの**3 つとも**変わること
- **変異 1 つで検出力を確かめること**
- `ctest` 全数が緑(**マージ直前に GPU 込みで 1 回**)、`uv run tools/doclink.py check` が通ること

依存: WP331(マージ済み)。見積: 中。

### WP333: クラッシュダンプを読める状態で残す

**§4 規則 11 の下段**(ビルド構成のみ)。**コードレビューのみ。**

**目的**: `pelican_studio` は繰り返しクラッシュしているが、**ダンプが読めない。**

#### 実測(2026-08-19)

`%LOCALAPPDATA%\CrashDumps` に **9 個**:

| 日時 | 備考 |
|---|---|
| 08/18 13:15 〜 17:00 | 8 個 |
| **08/19 14:29** | 1 個。**利用者が studio を触っている最中である** |

最新(PID 12684)を解析した結果:

- **`c0000005`(アクセス違反)、第 1 引数 0 = null 参照**
- 障害は `Qt6Widgetsd` の中、呼び出し元は `pelican_studio+0x720ba`
- **WP320 で直したスタックオーバーフロー(`c00000fd`)とは別系統**である

**症号化できなかった。**ダンプが記録している実行ファイルの刻印は
`Tue Aug 18 23:38:55 2026` だが、その後リビルドして
**`dist_debug/pelican_studio.pdb` が上書きされている。**
復元できたスタックも 2 フレームで途切れる。

**ダンプは取れているのに、読もうとした時には読めなくなっている。**
これが 9 個すべてが未解明のまま残っている理由である。

#### 実装範囲

1. **ビルド成果物と一緒に PDB を保全すること。**
   `dist_debug` を上書きするたびに前の PDB が消える現状をやめる。
   保全先はビルド出力の外にすること(§4 規則 8 —— ビルドディレクトリの中に置くと
   ハンドルを掴まれて消せなくなる)。
2. **どのコミットのバイナリかをダンプから辿れるようにすること。**
   実行ファイルに commit を埋めるか、PDB を commit で索引するか。
   **刻印だけでは足りない**(同じコミットを再ビルドすると刻印が変わる)。
3. 保全は**無制限に溜めないこと。**世代数の上限を決める。

#### 範囲外

**クラッシュそのものの修正。**原因が読めるようになってから別 WP で扱う。
crash reporter の実装。ダンプの自動送信。

#### 受け入れ条件(§4 規約 10)

- **`dist_debug` を 2 回続けてビルドした後で、1 回目のバイナリに対応する PDB が
  まだ取得できること。**同じテストの中で、保全機構が無いときには取れないことを示すこと
- **本番配線の対照(2026-08-20 追加)**: モジュールを fixture で駆動する契約テストは、
  **`pelican_studio` が呼ぶのをやめても緑のままである。**
  実際に確かめた —— `src/devstudio/CMakeLists.txt` の
  `pelican_preserve_crash_symbols()` を無効化しても契約テストは通った。
  **本番ビルドが実際に残したアーカイブを読む対照を別に置くこと。**
  manifest の commit が完全ハッシュであること、PDB 名が commit を含むこと
  (`/PDBALTPATH` がその名前をバイナリに刻むので、ダンプから辿れる唯一の手がかりである)、
  記録された sha が実ファイルと一致することを検査する
- **保全された PDB から、対応するコミットが一意に決まること**
- 保全先が**ビルドディレクトリの外**であること
- 世代数の上限が効くこと(上限を超えたら古いものが消えること)
- `ctest` 全数が緑、`uv run tools/doclink.py check` が通ること

依存: なし。見積: 小。

#### 記録: 未解明のクラッシュ 9 件

**本 WP はクラッシュを直さない。**読める状態にするだけである。
9 個のダンプは残してあるが、**うち 8 個は対応する PDB が既に存在しないので、
おそらく永久に読めない。**次に落ちたものから追うこと。

### WP334a: `features` を持たない config で feature を編集できるようにする

**§4 規則 11 の中段。**仕様レビュー + コードレビュー。

**目的**: 現在の runtime は、著作 root が `features` を含むときだけ編集サービスを構築する。

```json
{ "rendering_passes": [...] }
```

これは描画 config として成立するが、**編集 RPC の面が作られない。**
つまり **legacy / 手書きの config は WP331 の feature 編集を一切使えない。**

**単独で価値がある** —— 既存の Render Features UI がそのまま使えるようになる。

#### 実装範囲

- root document の初期化を正式な操作にする。
  **既存 bytes を全体再直列化せず** `features: []` を挿入する
- **挿入位置が決定的であること。**現在の document は
  root の `features` 配列の**字句範囲**しか持たない。
  **存在しない配列に字句範囲は無い**ので、そこを定義すること

#### 範囲外

パスの著作。複数文書トランザクション。UI の新規追加(既存の Render Features UI を使う)。

#### 受け入れ条件(§4 規約 10)

- **`features` を持たない config で編集面が作られ、feature を 1 つ足せること。**
  同じテストで、初期化前には編集面が無いこと
- **初期化が `features: []` の挿入以外に 1 バイトも変えないこと**
- **2 回目の初期化が no-op であること**(byte-identical)
- **挿入位置が決定的であること。**同じ入力から同じ bytes
- **変異 1 つで検出力を確かめること**
- `ctest` 全数が緑、`uv run tools/doclink.py check` が通ること

依存: WP331 / WP332(マージ済み)。見積: 中。

### WP334b: 著作したパスの生涯(追加・除去・保存・反映)

**§4 規則 11 の上段。**仕様レビュー + コードレビュー。
**初版は着手不可で差し戻された。**下記の「初版が破綻した理由」を読むこと。

**これは一つの縦切りとして残す。**トランザクション・復旧・除去・UI を別マージに割ると、
**「利用者のいない基盤」か「孤児を作る一方向機能」のどちらか**になる。

#### 初版が破綻した理由(繰り返さないこと)

**1. 新規 fragment を preflight できない。**
コンパイラの feature / preset loader は通常の `PathResolver.loadText(ref)` であり、
本番の登録は具体的なグローバル `PathResolver&` を固定している。
**staging にしかないファイルは候補としてコンパイルできない。**
書いてしまえば root の CAS 失敗時に孤児になる。

**グローバル `PathResolver` を一時的に差し替える実装は不可** ——
同時に走る reload へ staged bytes が漏れる。

**設計の前提節は `virtual candidate loader` を必要物として挙げていたが、
初版はそれを実装範囲から落としていた。**

**2. purge の第三状態を作る操作が無く、条件が矛盾していた。**
操作が `add_authored_pass` だけで「参照を外す」が無い。
しかも **target を範囲外にしながら「パスとターゲットが揃って消える」を要求**していた。
target を足せないなら正の状態を作れず、条件は永久に通らないか空虚になる。

**3. 消費者が原則に反していた。**
現在のフォームは **studio プロセス自身が `project.json` と rendering config を読む。**
これは「engine が document を所有し、studio はファイルに触らない」と矛盾する。
さらにフォームは preset config を明示的に拒否するので、
**`animgraph_demo` と `pelican project init` が作る全プロジェクトが対象外**だった。

#### 実装範囲

1. **候補は root の bytes ではなく `candidate document set`。**
   正規化済み参照を鍵とし、`create` / `replace` / `delete` と各ファイルの期待状態を持つ。
   **feature と preset の双方が同じ request-local overlay loader を通ること。**
   本番 `PathResolver` への fallback を残しつつ、**候補集合を先に引く。**
   renderer / reload service / source commit の型も document-set 対応にする。
2. **複数文書トランザクション。**staging、commit-last manifest、
   **起動前のクラッシュ復旧**(未完了 manifest を見つけたときの動作を定義すること)。
3. **`add_authored_pass` と `remove_authored_pass` を同じ WP のドメイン操作にする。**
4. **managed fragment の名前空間と所有権規則。**
   - editor が作った fragment は固定の名前空間に置く
   - **参照数がゼロになった managed fragment だけをトランザクション内で削除する**
   - **手書き / 共有 fragment は物理削除しない。**参照解除は別操作
   - managed fragment が外部変更済みなら、root も fragment も変えず `external_modification`
5. **engine が著作コンテキストを返す RPC。**
   解決済みの著作コンテキスト、provenance、**アンカー候補**、root の digest と世代。
   **direct と preset を同じ面で扱う。**
6. **保存 UI。**フォームから `openProjectReadOnly` によるローカル読取を**除去し**、
   上記 RPC に置き換える。

#### 範囲外

キャンバス。target / buffer / compute task の著作。preset の作り直し。undo / journal。

#### 受け入れ条件(§4 規約 10)

- **中心対照**: フォームで組んだ fullscreen パスを保存すると、
  managed fragment が作られ root から参照され、
  **再起動なしでフレームプランにノードとして現れること。**
  **同じテストの中で、保存前には現れないこと。**
  **`animgraph_demo` または `pelican project init` の出力を必ず一例含めること**
  (preset 型が対象外では消費者にならない)
- **原子性を両方向で**: fragment を書いて root の CAS を失敗させたとき **fragment が残らない**。
  root を書いて fragment を失敗させたとき **root が変わらない**。
  同じテストで両方成功する場合を検査すること
- **purge の 3 点対照**は **pass ノード / root の参照 / managed fragment ファイル**に限定する。
  足す前・足した後・**`remove_authored_pass` の後**で、
  **managed fragment が消え、参照が消え、ノードが消えること。**
  **target の数は 3 点で同一であること**(target を足さないため)
- **手書き fragment を参照解除しても、ファイルが削除されないこと**
- **孤児の検査は `git status` ではなく、
  「どの `features[]` からも参照されていない managed fragment が存在しないこと」で行うこと**
- **overlay loader を外す変異で、本番統合テストが失敗すること**
- **本番配線**: offscreen で `MainWindow` を構築し、保存が本番 widget から到達できること
- **`PELICAN_RUNTIME_SHADER_COMPILER` の ON / OFF 両構成**(§4 規則 9)
- `ctest` 全数が緑(**マージ直前に GPU 込みで 1 回**)、`uv run tools/doclink.py check` が通ること

依存: WP334a。見積: 大。

### WP335: 起動しただけでプロジェクトが書き換わる / パラメータ付き feature で落ちる

**WP334a のマージ後、文書の敵対検証で確認。**
**§4 規則 11 の上段**(エンジンの挙動が変わる)。**コードレビューのみ**(欠陥は具体的で設計判断が要らない)。

#### 欠陥 1: 読むだけのつもりの起動が、プロジェクトを書き換える

`RenderConfigEditorService` のコンストラクタは、
`features` 鍵の無い config に対して**その場で `atomicReplaceWithDigestCas` を呼ぶ**
(`src/core/communication/renderconfigeditor.cpp` の初期化直後)。
そして RPC サーバは**このサービスを無条件に構築する**
(`render_config_editing = true` に gate が無い)。

**つまり `--rpc` で起動しただけで、利用者が何も編集していなくてもファイルが書き換わる。**

これは「通常のランタイムはプロジェクトを読むだけ」という約束に反する。
studio は**必ず `--rpc` で player を起動する**ので、
**プロジェクトを開いただけで書き換わる。**

WP334a の受け入れ条件は「挿入以外に 1 バイトも変えない」を要求したが、
**「いつ書くか」を問わなかった。**条件の穴である。

**直し方**: **初期化は編集が最初に要求されたときに行う。**構築時に書かない。
`features` が無い状態でも `get_render_features` は答えられること(空として返す)。
**書き込みは利用者の操作が起点であること。**

#### 欠陥 2: パラメータ付き feature を持つ config で編集面の構築が落ちる

`features` 配列の要素が文字列でないと、document の構築が
`WP331 only edits string entries in the top-level features array` で例外を投げる。

**しかし `{ref, parameters}` のインスタンス形式は正式な形である**
(`docs/manual/06_rendering.md` が受理すると明記し、compose も受ける)。

出荷 4 プロジェクトは全て文字列のみなので今日は発火しないが、
**文書どおりに書いた利用者は落ちる。**しかもサービスは無条件に構築されるので、
**RPC の起動そのものに波及しうる。**

**直し方**: **オブジェクト形式の要素を、編集できないものとして受け入れること。**
文字列要素だけを編集対象にし、**オブジェクト要素は保持したまま触らない。**
編集できない要素があることを `get_render_features` が申告すること
(**黙って落とさない。黙って壊さない**)。

#### 受け入れ条件(§4 規約 10)

- **`--rpc` で起動しただけでは、`features` 鍵の無い config が書き換わらないこと。**
  同じテストの中で、**編集を 1 回要求すると書き換わること**
- **オブジェクト形式の要素を含む config で、編集面が構築でき、
  文字列要素だけを足し引きでき、オブジェクト要素が bytes として保持されること。**
  同じテストで、全て文字列の config が従来どおり動くこと
- **編集できない要素があることを `get_render_features` が申告すること**
- **no-op 保存が byte-identical であること**(既存条件の維持)
- **変異 1 つで検出力を確かめること**
- `ctest` 全数が緑、`uv run tools/doclink.py check` が通ること

依存: WP334a(マージ済み)。見積: 中。**WP334b より先に直すこと** ——
334b は同じ document の上に載る。

### WP336: `ssao_blur.frag` 1 本で、ソケット宣言を execution plan まで通す【実装済み・マージ保留】

**実装は完了し、検証も通っている**(ON 1142 / OFF 1094 とも 0 失敗、doclink 緑、仕様レビュー 3 回とコードレビュー 2 系統を通過)。
**ブランチ `agent/wp336` に残してある。**

**保留の理由**: 利用者の判断で `same_pixel` を語彙から外した結果、
**footprint 宣言は挙動を何も変えなくなった**(上記 §「footprint 宣言は、`same_pixel` を外すと何も変えない」)。
**土台(共通 tokenizer / 新文書種 / graph parse 前の resolver)は後続で要るので、
要るときに一緒に入れる。**単独でマージすると「誰も使わない機構」になる。

**設計は §1.1 が正本である。着手前に §0 / §1 / §1.1 / §2 を読むこと。**

**§4 規則 11 の中段**(`pelican_project` と複数経路にまたがる)。**仕様 + コード。**

**この節は初版・第 2 版とも仕様レビューで着手不可になった。**
**5 案(非出荷 slice / 原子的に全部 / reflection 照合だけ / 既存鍵で footprint /
著作時判定)を評価し、反証を当てて残ったのがこの形である。**

#### 収束の理由 —— 条件付きソケットが 3 案を殺した

**`fullscreen.frag` に触る案はすべて同じ理由で落ちた。**

`appendFullscreenSurfaceResource` は、fragment が `engine://fullscreen` かつ
`uses_light_data: true` のパスに対し **`shadow_map` を `input` 配列へ追記する。**
したがって合成後の image 入力数は **shadow 有効で 7、無効で 6** に揺れる。

- shadow 有効: `animgraph_demo` と `pelican project init`
- shadow 無効: `example` / `sprite_demo` / `vrm_xr_demo`

**6 固定の宣言表は前者を落とし、7 固定は後者を落とす。**
初版・案 D・案 E がすべてここで死んだ。

**`ssao_blur.frag` にはこの問題が無い** —— ソケット 1 本、feature 条件なし、
`ssao_blur_pass` の `input` は `["ssao_output"]` の 1 件で序数写像が自明。

**したがって本 WP は `fullscreen.frag` に触らない。**
**条件付きソケットの表現が決まるまで、`fullscreen.frag` は移行対象外である。**

#### この WP は実行時の挙動を変えない(実測で保証できる)

- **`read_requires_materialization` は `neighborhood` / `arbitrary` / `temporal` を
  同一に扱う。**`tile_local_eligible` は `widest_read == same_pixel` を要求する。
  **`ssao_output` を `arbitrary` → `neighborhood` に narrow しても反転しない**
- **`.spv` は git 追跡外で、raw `.frag` は `b_embed` と `embed_shader` の
  両方で無条件に埋め込まれている。**`engineResource()` は `.spv` と独立に raw を返す
- **`//!` を `#version` の前に置いて glslang が通ることは実測済み**

#### 範囲

**やること:**

1. **`//!` 行読みを共通 tokenizer へ抽出する。**
   prefix / **index 3 が ASCII U+0020 完全一致** /
   **リスト判定は「trim 後の先頭が `-`」のみ(直後の空白は任意)** /
   最初の `:` で分割。
   **`//! -legacy_item` と「`//!` の直後がタブ」の回帰テストを足すこと** ——
   前者は今日警告して黙殺され、後者は throw する。**どちらも変えないこと**。
   **`.surface` の挙動は byte 一致を保つこと**(未知鍵の警告も、
   値が空のとき section が `unknown` になって続く項目を黙殺することも、そのまま)
2. **新文書種 `pelican.fullscreen v1`。**`DocumentKind` と `UnknownKeyPolicy` を渡し、
   **新文書種でだけ未知のトップレベル鍵と未知の inline field を拒否する**
3. **v1 が綴るのは `name` / `footprint` / `radius` / `stage` のみ。**
   **`contract:` を書かせないこと** —— 組み込み契約を足さない本 WP では
   footprint に上書きされるだけの無効フィールドになり、
   **宣言の中に小さな「誰も使わないもの」を作る**
4. **graph parse より前に metadata を解決する。**
   `parseFrameGraphDefinitionsFromConfigJson` に既定値付き引数を足す。
   production の呼び出しは 3 箇所だけである
5. **`ssao_blur.frag` 1 本にヘッダを載せる**
   (`- { name: ssaoInput, footprint: neighborhood, radius: 2, stage: fragment }`)
6. **同一パスにヘッダと `input_footprints` の両方があれば名前付きエラー**
7. **宣言ソケット数がパスの `input` 配列長と一致しなければ名前付きエラー**

**やらないこと(理由付きで明示する):**

- **照合専用 reflection channel** —— 照合が走るのは `buildGraphicsPipeline` で
  **device が要る。**本 WP の受け入れ条件は全部 CPU 経路なので、
  **この channel には受け入れ条件が 1 つも付かない。**
  段階 2(名前束縛)で、実装上の動機が立ってから作る
- **解析結果の cache** —— `engineResource()` は埋め込みへの `string_view` を返す。
  **測る前に置く cache は、それ自体が「誰も使わない機構」である**
- **`fullscreen.frag`**(条件付きソケット。上記)
- **組み込み契約の追加 / producer semantic / `outputs:`**
- **版の gate** —— 本 WP は engine 同梱のシェーダーしか触らないので、
  エンジンと同梱物の版は必ず一致する。**`engine_min_version` は不要**
- **21 本の移行 / headerless の拒否 / binding を宣言順へ / 既定値**

#### 受け入れ条件(§4 規約 10)

**3 回目の仕様レビューで、この節は「限定修正すれば着手可」になった。**
**以下は修正を織り込んだ版である。**

**観測点は `test/devstudio_frameplan_graph_test.cpp` の既存ヘルパである** ——
実プロジェクト config → `parseFrameGraphDefinitionsFromConfigJson` →
`planFrameGraph` → `compileFrameExecutionPlan` → `compileRenderingTargetPlans` を
**device 無しで**通す。**`RUNTIME_SHADER_COMPILER` の ON / OFF 両方でビルドされる。**

##### 肯定

- `ssao_blur_pass` の `execution_plan.nodes[].resource_uses[]` のうち
  `resource == "ssao_output"` の `footprint` が
  **`{"kind":"neighborhood","radius":2}`** であること。
  **serializer は `{kind, radius}` を出し、execution adapter は
  未指定のときだけ `arbitrary` にする**(実測済み)

##### 否定対照 —— 「変更前」の作り方を逐語で決める

**ヘルパの返却値全体を byte 比較してはならない。**
ヘルパは `framePlanToJson` の後に `execution_plan` と `physical_target_plan` を
**同じ wire へ入れ子で足しており**、execution plan の fingerprint は footprint を含む。

**同一 `TEST_CASE` の中で:**

- **肯定側**は既定 resolver から実際の `engineResource("ssao_blur.frag")` を読む
- **否定側**は同じ shader body の**ヘッダ無し source** を loader から返し、
  従来どおり `arbitrary` を得る
- **byte 比較の対象は、入れ子を足す前の `framePlanToJson(...).dump()`** とする。
  基底の `FramePlan` は `read_footprints` を保持せず
  reads / writes / order / level だけを写すので、**ここは一致する**
- **記録 fixture を「変更前」の golden にしてはならない**

##### `physical_target_plan` —— 除外する欄を列挙する

**footprint は logical graph fingerprint の入力であり、
それが automatic fingerprint にも波及する。**
fingerprint は top-level だけでなく **pin / physical fragment /
complete package にも重複して出る。**
したがって「`widest_read` と reason だけが差分」は**必ず失敗する。**

**比較前に除外するのは次だけである:**

- **top-level と 3 つの ejectable package の logical / automatic fingerprint**
- **`ssao_output` の `widest_read`**(lowering graph / resources /
  complete package の各所)
- **`ssao_output` の materialization `reason`**(resources / complete package)
- **`pelican.plan.resource_representation@1` decision の当該 detail**

**それ以外は完全一致とすること** ——
特に `representation` / `backend_selection` / `scopes` / `attachments` /
`alias_groups` / sample・view・resolution plan。
**`neighborhood` と `arbitrary` はともに materialization 必須、
tile-local は `same_pixel` のみ、scope fusion も `same_pixel` を要求し、
cost は representation・stored・scope 数で決まる**(実測)ので、
これらが動かないことは保証できる。

##### 既存テストの追随(範囲に含めること)

**`test/devstudio_frameplan_graph_test.cpp` は
materialization reason が `arbitrary` の resource 数を 16 で固定している。**
`ssao_output` 1 件が neighborhood へ移るので **15 になる。**

- **`arbitrary_materialized_count == 15` に直すこと**
- **加えて `ssao_output` の neighborhood reason を直接検査すること**
  (数を減らすだけでは、別の resource が動いても通る)
- `same_pixel_count == 1` は変わらない(neighborhood はどちらでもない)

##### 形式の厳格化 —— 同一テストの両側で

- **新文書種で未知のトップレベル鍵が throw すること**
- **新文書種で未知の inline field も throw すること。**
  トップレベル鍵だけを検査すると、inline の typo を無視する実装でも通る
- **同じ `TEST_CASE` の中で、`.surface` は従来どおり warnings に積んで通ること**
  (既存テストが `editor_group` / `capabilities` で警告になることを検査している)

##### 序数の対照

- **ヘッダは 1 ソケットのままにし、config の `input` を 2 件にする。**
  「2 個目のソケットを書いた config」は書けない —— **config が持つのは接続であり、
  ソケットを書く場所が無い**
- **不一致の検出場所は resolver ではない。**
  metadata は先に解決するが、**比較は `parseRenderNodeFromJson` が `input` を
  読んだ直後、`input_footprints` を適用する前**に行う
- **ヘッダと `input_footprints` の併記拒否も、同じ choke point に置ける**

##### その他

- **top-level のフレームプランが byte 一致すること**
  (`order` / `level` / `reads` / `writes` / `barriers`)
- **`legacy_read_footprint_conservative` を条件に使わないこと。**
  **この decision は frame plan の wire に出ない**(出荷 fixture で 0 件)
- **出荷 4 プロジェクトと `pelican project init`**
- **ON / OFF 両構成。**OFF では `engineResource("ssao_blur.frag")` から
  同じ宣言が得られること
- **変異は 2 つ、名指しで**:
  **(a) metadata → `node.read_footprints` の適用を外す**、
  **(b) 併記拒否を外す。**
  **試していない項目は明示すること**

**着地点の確認(実測)**: `ssao_blur_pass` は
`projects/example`(verbose config)と `hybrid_v1`(preset)の**両方**にあり、
どちらも `fragment: engine://ssao_blur` / `input: ["ssao_output"]` の 1 件である。

**記録 fixture の扱い**: `test/fixtures/devstudio/example_frame_plan.json` には
既に `ssao_blur_pass` の `resource_uses` があり、
`ssao_output` の `footprint` は **`{"kind":"arbitrary"}`** である。
**ただしこれは golden ではなく、studio テストの入力である**
(読み込んで widget に食わせるだけで、再生成も比較もされない)。
**対照に使わないこと。**ただし engine の出力が変わる以上
**内容が実態と食い違うので、同じ WP で更新すること**。

#### 二重機構の終了条件(この WP で名指しすること)

**`input_footprints` とヘッダが同じ `node.read_footprints` を書く状態になる。**
本 WP は「同一パスで併記したら落ちる」で 1 パス 1 経路にするが、
**鍵そのものは残る。**

**終了 WP を採番して台帳に置くこと** ——
「generic fullscreen の engine シェーダーを原子的に移行し、
同じ変更で headerless を拒否し、`input_footprints` を削除する」。
**その WP の入口条件は「条件付きソケットの表現が決まっていること」である。**

#### 利用者に届くもの

**届かない。内部 invariant のみである。**
描画結果も性能もエラー文言も変わらない。**そう明記すること。**
設計自身が「段階 1・2 は内部の作り替え、利用者に届くのは段階 3 から」と書いている。
**本 WP を `v1` と呼ばないこと。**

依存: 無し。見積: 中。
**次は「条件付きソケットの表現」。**それが決まるまで `fullscreen.frag` は動かせない。


### WP338: `engine://ssao` を生成 include へ移す【マージ済み `1b20452`】

**設計は第 5 版の `### 完了までの順序` 段階 A・B。**

**§4 規則 11 の上段。仕様 + コード。**

#### これはリファクタである。証明は「挙動が変わらないこと」

**初版と 338a/338b の分割は破棄した。**

**移行は構造の変更であって挙動の変更ではない。**
したがって正しい受け入れ条件は **「移行前後で `ssao_output` が完全一致すること」** である。

**`ssao.frag` が書く先が一致すれば、同じテクスチャを読んだ証明になる。**
間違ったものを読んでいれば画像が変わる。

**捨てたもの(作らないこと):**

- legacy シェーダーの二経路 —— **生きた `ssao.frag` から導出する形は、
  移行した瞬間に壊れる**(338a の実装がそうなっていた)
- `permuted` による「順序が意味を失った」の証明 ——
  **リファクタの受け入れ条件ではない。**構造の帰結であって、
  次の WP(既定値)が要求したときに測ればよい
- descriptor identity の 5 構成行列
- 5 通り比較

**`agent/wp338a` の成果のうち、R8 readback と一時 project 生成は再利用してよい。**
**descriptor identity helper と legacy シェーダー生成は捨てること。**

#### 範囲

**0. readback を format 非依存・部分資源指定にする**

```cpp
struct RenderTargetReadback {
    vk::Extent2D  extent{};
    vk::Format    format{};
    std::uint32_t layer_count = 1;
    std::vector<std::uint8_t> bytes;      // layer-major
};

RenderTargetReadback readRenderTargetForTesting(
    std::string_view name, const ImageSubresourceRange &range);
```

- **`ImageSubresourceRange` は既存の型**(`src/project/imagesubresource.hpp`)。
  `resource_ports` の `subresource:` が使っているものと同じ。**新しい引数規約を作らない**
- **format 非依存にすること** ——
  **次に移す `fullscreen.frag` は G-buffer 6 枚(`R16G16B16A16_SFLOAT` /
  `B8G8R8A8_UNORM`)を読む。**R8 専用では 7 本のうち 1 本にしか使えない
- **layer-major** —— cube 6 面の比較が 1 回の呼び出しと 1 回の比較で済む
- **既存の `readR8RenderTargetForTesting` は薄いラッパとして残す**(呼び出し側を動かさない)

**1. 移行前の像を `test/golden/` に保存する**

`ssao_output` と capture 版(`cube_capture_ao` / `planar_reflection` 系)を**全層**読んで保存する。
**数 KB。恒久的に残す** —— AO は `sky_ambient` 構成でしか画素に出ないので、
**既存 golden はこの退行を捕まえられない。**

**2. `ssao.frag` を焼くのをやめる**

`embed_shader(ssao.frag)` を外し、`engineresources.cpp` の `.spv` 登録 2 行と
`test/fixtures/project_format/engine_resources.json` の 1 行を追随させる。

**実験で確認済み**(ブランチ `exp/unbake` / コミット `f4f5f14`)——
`ssao_blur.frag` で同じ手順が configure / build / 1137 件緑を通った。

**3. 生成 include の accessor へ移す**

`PELICAN_DECLARE_INPUT_0/1` と `PELICAN_TEXTURE_2D_0/1` を捨て、
**2 引数 accessor**(`pelican_sample_world(uv, pelican_view_index())` 等)へ。
sample は 3 箇所(world ×2、normal ×1)。

**4. 4 消費者に `resource_ports` を足す**

`hybrid_v1` / `projects/example` / `cube_capture` / `planar_reflection`。
**`"view": "per_view"` を明示すること。**

- **既定値は `shared_2d` で、multiview producer の layered target を繋ぐと名前付きエラーになる**
- **cube / planar も `per_view` が正しい**(producer と consumer が同じ secondary family。
  `family_array` は family をまたぐ場合で、sequential target に宣言すると明示エラー)
- **flat と `per_view` は矛盾しない**(物理 `shared_2d` は 2D accessor に解決される)

**`input` は残す**(port は `input` に在る resource しか指せず、グラフの辺は `input` が作る)。

**5. `docs/color_migration_manifest.json` の旧 sampler / binding 名を追随させる**

#### 受け入れ条件

- **移行前後で保存像と完全一致すること。許容差を使わないこと** ——
  同一プロセス・同一デバイスの比較なので、許容差は本物の差分を吸ってしまう
- **構成ごとに:** flat / XR sequential / XR multiview / cube(6 面)/ planar(2 view)
- **cube は 6 層すべて、planar は 2 view すべてを比較すること**
  (層を 1 つしか見ないと、全層が同じ内容になる退行を見逃す)
- **保存像が一様でないこと** ——
  ground・奥行き差・異なる法線を持つ scene を使う。
  `ssao_output` は `TRANSFER_SRC` を持たないので test feature で追加する
- **build option を明示すること** ——
  cube / planar は `PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=ON`、
  XR は `PELICAN_WITH_OPENXR=ON`。
  **planar は実行時に XR graph variant を要求するので、
  `STANDARD_RENDER_ALGORITHMS` だけを gate にしないこと**(338a がそこで誤っていた)
- **非実行は `SUCCEED` ではなく `SKIP` を使うこと。**
  このファイルの既存 18 箇所はすべて `SKIP` である。
  `SUCCEED` は skip_policy の検査も通り抜け、「テストがあって緑」に見えて中身が 0 件になる
- **`ssao.frag.spv` が生成されなくなること** ——
  **使い捨ての clean worktree と新規 build ディレクトリ**で、
  生成器の graph(`build.ninja` / `*.vcxproj`)に
  `ssao.frag.spv` を output とする custom-command edge が 0 件であること。
  **`embed_shader` は `.spv` を source tree へ出力し gitignore されるので、
  通常の作業木での存在検査は無効である**
- **出荷 4 プロジェクトと `pelican project init`**
- **`RUNTIME_SHADER_COMPILER` の ON / OFF 両構成でビルドが通り、テストが緑。**
  OFF のテスト数が減るならどのテストがなぜかを報告すること
- **`uv run tools/doclink.py check` が緑**
- **変異を 1 つ**: **`resource_ports` の `world` と `normal` の resource を入れ替える**
  → 保存像と一致しなくなること。**試していない項目は明示すること**

#### 落とし穴

- **`ssao.frag` は `worldPosSampler` を 2 回読む**
  (同一ピクセルと再投影した任意座標)。**同じ port を 2 回 sample するだけ**
- **生成 accessor の名前は port 名から作られる。**識別子規則に従うこと
- **`push_constants: "projection_view"` は残してよい**
  (Frame UBO であり、生成 port は pass-input descriptor set に出る)
- **`input_sampling` の追加は不要**(既定値がどちらも linear / repeat)
- **binding は今も `input` の index である。**
  本 WP が変えるのは「シェーダーが番号を書かなくなる」ことであって、番号の決まり方ではない

#### 完了時の記録: `engine://ssao` は OFF で解決できなくなった

**焼くのをやめた帰結であり、意図したものである。**
OFF は `.frag.spv` しか探さないので、`Shader stem could not be resolved: engine://ssao` で名前付きに落ちる。

**ただし到達する出荷構成は存在しない**(実測) ——
`engine://ssao` を使うのは `projects/example` と `hybrid_v1` だけで、
**`example` は `ui` feature が OFF で required なので、
シェーダー解決より先に feature gate で落ちる。**
`animgraph_demo` と `pelican project init` も shadow / sky / ui を要求する。

**これが「内蔵シェーダーが実行時コンパイラ必須になる」最初の 1 本である。**
残り 6 本を移すと、**内蔵パイプライン全体がそうなる。**
OFF 対応を作るのは WP211(dist-bake)であり、完了の前提ではない。

依存: 無し。見積: 中。
**次は `ssao_blur.frag`**(`textureSize` の扱い)、
**その次が `fullscreen.frag`**(条件付き shadow ソケット = 既定値と同時)。

### WP339: `engine://ssao_blur` を生成 include へ移す(第 4 版)

**設計は第 5 版の `### 完了までの順序` 段階 A・B。WP338(`1b20452`)の続き。**

**§4 規則 11 の上段。仕様 + コード。**

**第 1〜3 版はいずれも敵対レビューで破綻した。**
**第 4 版は Claude の 2 巡(計 8 レンズ)と codex の仕様レビュー 1 巡を経ている。**
**codex は阻害的欠陥 3 件を含む 8 件を出した。以下はその訂正版である。**

#### 前の版が壊れた理由(繰り返さないこと)

1. **バイト一致は何も証明しなかった** —— port 1 本では resource の取り違えを
   engine が名前付きで先に落とす。しかも 2 枚目の readback に `ssao_output` を
   渡す実装ミス 1 箇所で全条件が通る(同じ `R8_UNORM`・同じ extent)
2. **サイズの変異が検査できなかった** —— 全構成で入力 extent == 描画解像度だった
3. **逃げ道が開いていた** —— blur を multiview 候補から外せば array 経路を通らずに済む
4. **`±1` が device 依存だった** —— linear の重みは `subTexelPrecisionBits`
   (Vulkan の下限 4 bit)に量子化される
5. **`"address_mode"` は存在しない JSON キーだった** ——
   parser が受理するのは `filter` と `address` だけで、
   それ以外は `sampling has unknown field` で落ちる
   (`src/project/shaderresourceport.cpp:145-160`)。
   **指示どおり実装すると出荷 4 JSON が parse error になっていた**
6. **AO 刺激の閾値に余裕が無かった** —— 既存 fixture の cube 層の distinct 数は
   実測 **6, 7, 6, 7, 6, 23**。`distinct >= 6` は境界そのもので、
   producer の浮動小数点 noise が device で 1 段変われば落ちる
7. **`pass 名 → ShaderBundleId` の getter は一意にならなかった** ——
   `ssao_blur_pass` は hybrid と example で重複し、graph variant ごとにも現れる

#### 確認済みの事実(再調査不要。Claude 2 巡 + codex 1 巡で独立に検算済み)

**1. `textureSize` は障害ではない。**

`generateShaderResourcePortInclude`(`src/core/shader/shaderresourceinterface.cpp:621`)は
`ivec2 pelican_size_<port>()` を **input attachment(`:213`)/ cube(`:237`)/
2D(`:265`)/ 2D array(`:287`、`.xy` に正規化)** で発行する。
storage buffer port は出さない(`pelican_count_` のみ)が、本 WP の port は
combined image sampler なので影響しない。

**2. `ssao_blur.frag` は今日、array 変種でコンパイルが通らない。**

```
-DPELICAN_INPUT_0_LAYERED=1
  ERROR: ssao_blur.frag:17: '=' : cannot convert from
         ' temp 3-component vector of float' to ' temp highp 2-component vector of float'
-DPELICAN_INPUT_0_LOCAL_READ=1
  ERROR: ssao_blur.frag:17: 'textureSize' : no matching overloaded function found
(defines なし)  正常
```

**`PELICAN_INPUT_0_LAYERED` の発行条件は選言 2 つ**
(`renderingpassruntimecompiler.cpp:1043-1058`):
**multiview 実行 かつ `layered_2d_array`** / **`family_2d_array`(実行モードを問わない)**。
**後者があるので XR 専用ではない。**

**出荷プロジェクトで到達するものは無い。しかし作者が標準パイプラインを
XR multiview に持っていくか、二次 family の出力を繋ぐと、
自分が書いていないエンジンシェーダーの GLSL 型エラーで落ちる。**

**3. 消費者は 4 ファイル。どれも入力 1 本。**

| ファイル | pass 名 | input | output |
|---|---|---|---|
| `src/core/resources/render_pipelines/hybrid_v1.json` | `ssao_blur_pass` | `ssao_output` | `ssao_blur` |
| `projects/example/passes/main_rendering_config.json` | `ssao_blur_pass` | `ssao_output` | `ssao_blur` |
| `src/core/resources/features/cube_capture.json` | `cube_capture_ssao_blur` | `cube_capture_ao` | `cube_capture_ao_blur` |
| `src/core/resources/features/planar_reflection.json` | `planar_reflection_ssao_blur` | `planar_reflection_ao` | `planar_reflection_ao_blur` |

**4 つとも `input_footprints` も `push_constants` も `input_sampling` も持たない。**

到達する「プロジェクト」は `projects/example`(直接)/
`projects/animgraph_demo`(`hybrid_v1` preset 継承)/ **`pelican project init` の生成物**。
`vrm_xr_demo` は `ssao_blur` という名前の target を白でクリアしているだけである
(`projects/vrm_xr_demo/passes/main.json:10,14-15`)。**この非対称を壊さないこと。**

**4. bake をやめる費用は 4 行。**

```
削除: src/core/resources/CMakeLists.txt:60        embed_shader(ssao_blur.frag)
削除: src/core/loader/engineresources.cpp:131     "ssao_blur.frag.spv",
削除: src/core/loader/engineresources.cpp:296     PELICAN_ENGINE_RESOURCE("ssao_blur.frag.spv")
削除: test/fixtures/project_format/engine_resources.json:119  "ssao_blur.frag.spv",
```

**残すこと**: `CMakeLists.txt:148` の `b_embed`、
`.frag` 側の登録(`engineresources.cpp:130` / `:295` / fixture `:118`)。

**5. `pelican_view.glsl` の include は捨ててよい。**
`pelican_view_index()` は `pelican_frame.glsl:51-57` にあり、
`ssao_blur.frag:6` が既に `pelican_frame.glsl` を include している。**frame は残すこと。**

**6. 半解像度構成は作れる。**

- `render_target_overrides` は `format` / `format_candidates` / `usage` / `width` / `height`
  だけを受け付ける(`featurecompose.cpp:1656-1662`)
- **`width`+`height` は `extent_scale` に優先する** ——
  `rendertargetjsonparser.cpp:16-32` → `rendertargetcontainer.cpp:60-79` で
  `ResourceExtentKind::fixed` になる
- **入力と出力の extent 不一致を engine は拒否しない**
  (`renderingpassvalidation.cpp:102-158` は output 同士しか比較しない)
- **`ssao_pass` / `ssao_blur_pass` は `unclassified`** なので
  `render_resolution` の算出に入らない(`renderingsamplecount.cpp:1140-1215`)。
  AO を 32×32 にしても `render_resolution` は 64×64 のままである
- **必ず main view family に作ること。**二次 family では
  `render_resolution` がその family 自身の raster extent に差し替えられ
  (`src/core/vkcore/renderer.cpp:4644-4660`)、変異が結果を変えなくなる。
  しかも family が 2 つの extent にまたがると `renderer.cpp:604-617` が名前付きで落ちる

**7. `sampling` の JSON キーは `filter` と `address` である。**
`address_mode` は存在しない(`src/project/shaderresourceport.cpp:145-160`)。
**本 WP は `sampling` を書かない**(下記)が、書くときのために記録する。

**8. 既存の getter は `PassId` 鍵である。**
`FullscreenPassContainer::inputSamplingForTesting(PassId)`
(`src/core/fullscreenpass/fullscreenpasscontainer.hpp:133`)。

#### 範囲

**1. `ssao_blur.frag` を移す**

- `#include "pelican_view.glsl"` と `PELICAN_DECLARE_INPUT_0` を捨て、
  `#include "pelican_resource_ports.glsl"` を無条件 include にする
- `textureSize(ssaoInput, 0)` → **`pelican_size_<port>()`**
- `PELICAN_TEXTURE_2D_0(ssaoInput, uv)` → **2 引数 accessor**
  `pelican_sample_<port>(uv, pelican_view_index())`

**`pelican_size_<port>()` と `textureSize(pelican_resource_<port>, 0).xy` は
生成コードとして同一である。**どちらでも挙動は変わらない。
**前者を使うこと。ただしこれを検査するテストは作らない** ——
挙動差が 0 なので、テストで縛る価値より足場の重さが勝つ。**diff レビューで見る。**

**2. 4 消費者に `resource_ports` を足す**

`"access": "sampled"` と **`"view": "per_view"`** を明示すること。

**`sampling` を書かないこと。**既定は `linear` / `repeat` であり
(`renderingpass.hpp:227-233` / `shaderresourceport.hpp:81-89`)、
**本 WP は出荷物のサンプリング挙動を変えない。**
`nearest` にする案は検討したが、**旧出力の baseline を持たないので
非回帰を証明できない**ため採らない。
linear のぶんの誤差は受け入れ条件側で device limit から導出する。

**`input` は残す**(port は `input` に在る resource しか指せず、辺は `input` が作る)。

**3. bake をやめる**(上の 4 行)

**4. `FullscreenPassContainer::fragmentShaderForTesting(PassId)` を足す**

既存の `inputSamplingForTesting(PassId)` と同じ形。
**pass 名から引く global map を作らないこと** ——
`ssao_blur_pass` は複数プロジェクトと複数 graph variant に現れるので一意にならない。
呼び出し側が `RenderingPassId` / graph variant + pass 名から `CompiledPass` を選び、
その `pass_id` を使うこと。

**5. 追随させるもの**

- `docs/color_migration_manifest.json` に旧 sampler / binding 名が残っていれば
- **`test/fixtures/devstudio/example_frame_plan.json`** ——
  `projects/example` の config を変えると engine が publish する frame plan が変わる。
  **このファイルは studio テストの入力として読まれるだけで再生成も比較もされないので、
  食い違っても誰も検出しない**

#### 受け入れ条件

##### 刺激は決定的な test producer が作る。実 SSAO には頼らない

**`ssao_output` 相当を書くのは、テスト所有の決定的な fragment シェーダーとすること。**

**理由**: 実 SSAO producer は `sin` / `cos` ベースの浮動小数点 noise を使う
(`src/core/resources/ssao.frag:36`)。既存 fixture の distinct 数は実測で
**flat 24 / XR sequential 7 / XR multiview 7,7 / cube 6,7,6,7,6,23 / planar 8,7** ——
**cube の 3 層は境界値そのもの**で、device が変われば blur が完全に正しくても落ちる。

**producer の要件:**

- **層ごとに異なる既知の pattern を書くこと。**
  これで**層の取り違えが検出できるようになる** ——
  既存 fixture では `cube.r8` の layer 1 と layer 3 が
  4096 バイト全一致なので、face 1↔3 の入れ替えは原理的に見えなかった
- **測って報告すること**: 層ごとの distinct 数、隣接テクセルの最大差、
  および下記「正しい核と変異核の分離」

**実 `engine://ssao` を producer にした構成は、別の integration smoke として 1 本だけ残すこと**
(移行が実配線を壊していないことの確認)。**そちらに箱平均の厳密検査を課さないこと。**

##### 主たるオラクル: 同じレンダリングの中で 2 枚読み、関係を検査する

**golden 画像を 1 枚も作らないこと。**
**`test/golden/` に新しいディレクトリを作らないこと**(そこは `case.json` +
`expected.png` の exact-set 契約である)。
**`writeWp338SsaoProject` / `wp338SsaoContract` を変更しないこと。**
**`test/fixtures/wp338_ssao/` の 6 枚を 1 バイトも変えないこと。**
**`PELICAN_UPDATE_WP338_SSAO_GOLDEN=1` を立てないこと**(自己充足形で、
立てると WP338 の 6 枚が移行後の出力で上書きされ比較は緑のまま通る)。

**blur 用の生成器を新設すること。**

1 回のレンダリングから producer 出力と blur 出力の 2 枚を読む
(`render_target_overrides` は map なので `TRANSFER_SRC` を 2 つ与えられる)。

**構成ごと・層ごとに:**

1. **producer の層が互いに相異なること**(決定的 pattern なので構成的に真。assert すること)
2. **`|blur − producer| > tol` の画素が全画素の 25% 以上**
3. **`blur[layer]` が `producer[layer]` の 5×5 箱平均(repeat 巻き戻し)と
   `tol` 以内で一致すること**

**`tol` は推測せず device から導出すること:**

```
tol = 1 + ceil(maxNeighborDelta / 2^subTexelPrecisionBits)
```

`1` は float 25 加算と `R8_UNORM` 丸めのぶん。
第 2 項は **linear サンプリングの重みが `subTexelPrecisionBits` に量子化されるぶん**である
(Vulkan の下限は 4 bit)。`maxNeighborDelta` は producer の実バイトから測る。
**`VkPhysicalDeviceLimits::subTexelPrecisionBits` の実測値と、
構成ごとに算出した `tol` を報告すること。**

**3 が証明しないことも書いておく** ——
これは「同一パス内で消費した層と書き込んだ層が一致する」ことしか示さない。
**producer と blur に同じ view→layer 置換が掛かった場合は検出できない。**

##### 半解像度構成 —— 否定対照を木の中に残す

**producer と blur を描画解像度より小さくした構成を 1 つ、main family に作ること**
(描画 64×64 に対して両方 32×32。`render_target_overrides` の `width` / `height`)。

**同じ producer readback から CPU で 2 つの核を計算する:**

- **正しい核**: タップ間隔 `1/producer_extent`(= 1/32)の 5×5 箱平均
- **変異核**: タップ間隔 `1/render_extent`(= 1/64)。
  タップが半テクセル位置に落ちるので、linear のもとで軸ごとに重み付き 3 タップ核になる

**要求:**

- **`blur` が正しい核と `tol` 以内で一致すること**
- **`blur` が変異核と一致しないこと**
- **2 つの核どうしが層ごとに `tol` を超えて異なる画素を持つこと。
  これを先に assert しないと否定対照が空振りする** ——
  両方の核は対称で定数と 1 次勾配を厳密に保存するので、
  **差を生むのは 2 階微分であって非一様性ではない。**
  **producer の pattern は、この分離が `tol` を十分上回るように選ぶこと。分離量を報告すること**

##### array 経路を実際に通すこと(逃げ道を塞ぐ)

**移行後は `PELICAN_INPUT_n_LAYERED` はこのシェーダーに効かない**(位置束縛マクロを捨てるため)。
array かどうかを決めるのは `resolveShaderResourceImageViewDimension` である。
**define の有無を検査して条件を満たしたつもりにならないこと。**

逃げ道の実体は「vertex を `project://` に差し替えると
`vulkanrendercompilerprogram.cpp:103-108` の候補登録から外れ、
`view.execution` が multiview にならず、
`renderingpassruntimecompiler.cpp:1727-1741` の consumer が `graphics_sequential` になって
`two_d` に落ちる」である。

- **blur の port の生成 include が `sampler2DArray` を宣言し、
  対応する `ShaderResourceInterfaceBinding.image_view_dimension == two_d_array`
  であることを検査すること**(範囲 4 の getter を使う)
- **その構成で blur の vertex を `engine://fullscreen` のまま維持すること**
- **その構成でも箱平均が層ごとに成立すること**

##### 構成の一覧(実装者が決めないこと)

| # | 構成 | 何の証拠か |
|---|---|---|
| 1 | flat(main family、64×64) | 基準 |
| 2 | **flat 半解像度**(producer / blur を 32×32) | **サイズ問い合わせの否定対照** |
| 3 | XR sequential(2 view) | 出荷配線の非回帰 |
| 4 | **XR multiview(2 layer)** | **array 経路。移行前はコンパイルが通らないはず** |
| 5 | cube(6 face) | 出荷配線の非回帰。層ごとに異なる pattern |
| 6 | planar(2 view) | 出荷配線の非回帰 |
| 7 | **cross-family**(二次 family の target を main family の blur に繋ぐ) | **`family_2d_array` 分岐** |

**7 について:**

- **`view: per_view` のままなら名前付きで落ちること。**期待文言は
  `must declare family_array to consume a producer-owned view-family array`
  (`shaderresourceinterface.cpp:578-585`)
- **`family_array` を宣言すれば通り、`image_view_dimension == two_d_array` になること**
- **箱平均は producer の層 0 についてのみ要求する。**
  producer と blur の extent を `width` / `height` で一致させたうえで検査すること。
  **残りの層は本 WP の対象外**(`pelican_view_index()` は producer family の層ではない)

##### 移行前に測って報告すること

**着手して最初に、上記 7 構成に対して移行前のビルドでオラクルを走らせ、
どれが通り、どれがコンパイルで落ちるかを報告すること。落ちたものはエラーを逐語で。**

- **移行前に通った構成は、移行後も通ること**
- **移行前に落ちた構成は、移行後に通ること**

**成立しない条件を自分で読み替えないこと。**成立しないと判断したら `file:line` 付きで報告すること。
**この設計線は成立しない受け入れ条件で 7 回差し戻されている。**

##### そのほか

- **`ssao_blur.frag.spv` が生成されなくなること** ——
  **使い捨ての clean worktree と新規 build ディレクトリ**で、
  生成器の graph(`build.ninja` / `*.vcxproj`)に
  `ssao_blur.frag.spv` を output とする custom-command edge が 0 件であること。
  **`embed_shader` は source tree へ出力し `src/core/resources/.gitignore:1` が無視するので、
  通常の作業木での存在検査は無効である**
- **4 パスの解決済み sampler が `linear` / `repeat` のままであることを
  `inputSamplingForTesting(PassId)` で検査すること**(出荷挙動を変えていない証拠)
- **出荷 4 プロジェクトと `pelican project init`**
- **`RUNTIME_SHADER_COMPILER` の ON / OFF 両構成でビルドが通り、テストが緑**
- **`uv run tools/doclink.py check` が緑**
- **`uv run test/golden_inventory.py` が緑**
  (`PELICAN_PYTHON_TESTS=OFF` の構成では ctest が回さないので手で回すこと)
- **既存の回帰ガードを壊さないこと**(**これらは blur の移行を検出しない。
  証拠ではなく回帰ガードである**):
  `GoldenHarness::runPlanarReflection()`(`test/golden_harness.cpp:9183`)は
  `execution_trace` のノード名を**厳密一致**で検査する。**件数は 15 である**(`:9475`)。
  `test/featurecompose_test.cpp:2090` / `:2660` も pass 名の一覧を検査する

#### 落とし穴

- **`ssao_blur` は 1 つの port を 25 回 sample する**(5×5)。同じ port を 25 回呼ぶだけ
- **`pelicanResolution.render_resolution` を size の代わりに使わないこと**
- **binding は今も `input` の index である**
- **`1.0 / vec2(pelican_size_<port>())` という形は新しくない** ——
  出荷中の `standard_prefilter.comp` が既に踏んでいる(しかも `view: family_array`)。
  **新しいのは graphics(fragment)パスで踏むことである**
- **`agent/wp336`(未マージ、`31f0971`)は同じ `ssao_blur.frag` に
  `//! pelican.fullscreen v1` ヘッダを載せ、ソケット名 `ssaoInput` を宣言している。**
  本 WP はその宣言を削除し、名前を JSON 側の port 名へ移す。
  **WP336 を後で畳むときは前提を読み直すこと。本 WP は WP336 に依存しない**

#### やらないこと

`fullscreen.frag` / 既定値・constant socket の機構 /
`input` を `resource_ports` に畳む / 順序 / compute / WP211 /
positional fallback を足す / binding allocator と descriptor writer の変更 /
**input attachment の罠を閉じること(WP340)** /
**`pelican_size_<port>()` を使ったことをテストで縛ること**(生成コードが同一なので挙動差が 0)/
**サンプリング既定値を `nearest` に変えること**(baseline が無く非回帰を証明できない)

#### 既知の帰結

**`engine://ssao_blur` は runtime shader compiler OFF で解決しなくなる。**
`ssao` と同じ帰結であり、OFF 対応は WP211 の主題である。

**訂正: input attachment の silent path を開けたのは本 WP ではなく WP338 である。**

位置束縛マクロ時代、`ssao_blur` を input attachment に繋ぐ配線は
コンパイルエラーで弾かれていた(`subpassInput` に `textureSize` が無い)。
移行するとその防壁が消え、`pelican_sample_` が uv を捨て
`pelican_size_` が描画解像度を返すので、5×5 が黙って同一ピクセル 25 回になる。
**しかも desktop と tile-based で別の絵になる(決定性の破れ)。**

**ただし `ssao.frag` は WP338 で既に同じ状態にある**(生成 include を使い、
`pelican_sample_world(offset.xy, ...)` で再投影座標を読む)。
**したがってこの経路は `1b20452` の時点で main に入っている。本 WP は 1 本広げるだけである。**

**`access: "sampled"` はこれを止めない** ——
`local_reads[input]` が立てば port の `access` を読まずに input attachment に落ちる
(`renderingpassruntimecompiler.cpp:1703` / `:1710-1712`)。

**未宣言 read の既定は 2 通りある**(`logicalframegraphadapter.cpp:375-388`)——
通常は `arbitrary` だが、**その resource が同じパスの `load_op: load` の
attachment でもある場合は `same_pixel` になる。**

**今日、出荷構成のどれもこの状態にない**(実測):
出荷 JSON の `input_footprints` は **0 件**、`same_pixel` も **0 件**、
自分の `color_load_op: "load"` attachment を `input` にも持つ
fullscreen / raster パスも **0 件**。

**WP340 が閉じる。WP340 は本 WP に依存しない**(既存 resource port 経路への規則であり、
blur の移行を必要としない)。**`fullscreen.frag` の移行より前に入れること** ——
あれは gbuffer の attachment を読むパスであり、tile-local 融合の現実的な候補である。

**本 WP の後、位置束縛マクロを使いながら pass 入力のサイズを問い合わせる
シェーダーは 1 本も残らない。ただし「サイズを手書きで問い合わせるシェーダー」は 5 本残る** ——
`layout(set = PELICAN_SET_PASS_INPUT, binding = N) uniform sampler2D` を手書きする第三の旧流儀:

```
src/core/resources/bloom_blur_h.frag:7,15
src/core/resources/bloom_blur_v.frag:7,15
src/core/resources/shader_lab_blur_h.frag:13
src/core/resources/shader_lab_blur_v.frag:13
src/core/resources/taa_resolve.frag:19,33
```

**これらが今日壊れていない理由は、multiview 候補の許可リストが
組み込み fragment 5 本にハードコードされているからである**
(`vulkanrendercompilerprogram.cpp:54-60`)。
**その一覧を一般化するか別の stem を足した瞬間に、`ssao_blur` と同じ壊れ方をする。段階 B の残作業。**

依存: WP338(マージ済み `1b20452`)。見積: 中。

---

#### コードレビュー(codex、`f4ae961` 直後)の仕分け

**受理不可 6 件。うち 2 件は Claude 側で算術を再現して確認した。**

**直したもの(WP339a)** —— production は触らない。刺激と許容差だけ:

- **周期 2 の一次元刺激では 5×5 核を証明できない。**
  producer が x のみで変化し y 不変なので、**タップ間隔を 3 倍(任意の奇数倍)にしても、
  y タップを全部捨てても、7 本の関係オラクルが正しい核と同一値を出す。**
  parity の多重集合が一致するためである。layer 0 で正しい核・3 倍間隔とも `[97,141]`
- **許容差が cube face の取り違えを通す。**
  `tol` を全 layer の最大隣接差から 1 つだけ算出しているため、
  `subTexelPrecisionBits <= 6` で `tol >= 5` になる。
  一方 face 0 の箱平均は `[97,141]`、face 2 は `[101,141]` で**最大差 4**。
  この機は bits=8(`tol=2`)なので隠れているだけである。
  **Vulkan core の下限は 4 bit。**
  加えて一般の 2D 刺激では bilinear の α/β が独立に量子化されるので、
  上限は `Dx/2^b + Dy/2^b` であり、現在の一項式は 2D の上限になっていない

**台帳に残すもの(直さない)**:

- **input attachment 化すると blur が無音で恒等写像になる。**
  `input_footprints: same_pixel` を 1 行足すか、
  `ssao_output` を `color_load_op: "load"` の出力兼入力にすると到達する。
  **WP340 の主題である。**codex の提案は「`ssaoInput` の必要 footprint を
  `neighborhood/radius=2` として production contract に持たせ、
  input attachment 化を名前付きで拒否する」——
  **WP340 の provenance 案より、シェーダー側が必要な footprint を宣言するほうが素直かもしれない。
  WP340 着手時にどちらを採るか決めること**
- **8 本のテストは「生成 include を実際に使ったこと」を証明しない。**
  `PELICAN_DECLARE_INPUT_0(pelican_resource_ssaoInput)` と
  `textureSize(pelican_resource_ssaoInput, 0).xy` に書き換えると、
  reflection の名前・型・次元が一致し、virtual include は
  **実際に include されたかに関係なく bundle に保存される**(`shaderlibrary.cpp:335`)ので全部通る。
  **挙動差は 0**(生成コードと同一の値になる)だが、
  「シェーダーが binding を書かない」という性質そのものは守られていない。
  塞ぐには ShaderCompiler が**実際に解決・消費した** virtual include を記録する必要があり、
  **挙動差 0 の性質を守るための engine 改修としては費用が見合わない**
- **fixture をテスト側が二系統で再構成している。**
  `devstudio_frameplan_test.cpp:48`/`:108` の `capturedResponse()` が
  raw fixture の `runtime_resolution` を上書きし、
  graph test 7 本も `exampleFramePlan(true)` の値で上書きする。
  **raw fixture の runtime extent を壊しても通る。**
  WP339 が炙り出した既存のテスト設計問題であり、本 WP の範囲ではない
- **port を持たない外部 config が汎用のシェーダーコンパイルエラーで落ちる。**
  `engine://ssao_blur` はマニュアル記載の公開同梱シェーダーである
  (`docs/manual/06_rendering.md:350`)。**WP338 の `engine://ssao` と同型。**
  port 欠落を示す名前付き移行エラーが要る。**2 本まとめて別 WP で扱う**
- **`RUNTIME_SHADER_COMPILER=OFF` の「8 本が緑」は実質 0 件だった** ——
  `test/CMakeLists.txt:96` が OFF 時に CTest 登録前に return するため

**fixture の意味差は「追加だけ」ではなかった**(36 件)——
`usage` 追加 21 / `runtime_resolution` 追加 1 / `intent: automatic→sampled` 3 /
`sampled_access: false→true` 3 / 派生 fingerprint 8。
sampled 化は `gbuffer_normal` / `gbuffer_worldpos`(WP338)と `ssao_output`(WP339)で、
**port を足したことの正しい帰結である。**nodes / barriers / extent に意味差は無い。
---

### WP340: 取り下げ。扉は開いたまま記録する(第 3 版)

**着手しない。コードを 1 行も変えない。**
**替わりに立てるのは `### WP342: アクセサを分ける` である。**

#### 何を塞ごうとしていたか

生成 include の image port が input attachment に解決されると、
`pelican_sample_<port>` は **uv を捨てて** `subpassLoad` になり、
`pelican_size_<port>` は**描画解像度**を返す(読んでいる target の extent ではない)。
`ssao_blur` の 5×5 が黙って同一ピクセル 25 回になり、
**desktop と tile-based で別の絵が出る。**

位置束縛マクロ時代は `subpassInput` に `textureSize` が無いのでコンパイルで弾かれていた。
**WP338 / WP339 がその偶然の防壁を外した。**

#### 4 案を立てて 4 案とも退けた

| 案 | 退けた理由 |
|---|---|
| **A** provenance を 4 層に通す | `LogicalResourceUse` が footprint の**値**しか持たず、provenance が検査点に届かない |
| **C** port を持つ read の既定を `arbitrary` に | 出荷 4 箇所が throw(`writes` にしかない resource を port が名指している)/ `appendReadFootprint` は**rank の最大**なので作者の `same_pixel` が黙って捨てられる / **scope 融合**にも及び、出荷の `lit_color` の read-modify-write が実際に使っている |
| **D** input attachment で uv 版 accessor を出さない | 生成 10 本のうち **`subpassLoad` を呼ぶのは uv 版 4 本だけ**。消すと読む手段が消え、未参照 port は reflection 検証が throw。sampled 側に 0 引数版が無く**両 variant で通るソースが存在しない**。spvlink の stub と `template_exports` が uv 版を無条件に要求 |
| **B1 / B2** port が footprint を持ち、descriptor が決まる地点で照合 | **三脚(出荷を壊さない / purgeability / 情報が届く)は B2 なら満たす。**それでも採らない —— 下記 |

**B1 は情報が検査点に届く**(`renderingpassruntimecompiler.cpp:1679` で `port` は束縛済み、
`PassDefinition` が `resource_ports` を丸ごと持つので logical graph を通らない)。
**同じ検査は material 経路に既に 2 箇所出荷されている**
(`materialcontainer.cpp:2959-2966` と `:4535-4544`、
`material pass selected a local attachment for a non-same-pixel input`)。
**しかし B1 単体は purgeability を壊す** ——
広い footprint で読む optional feature を purge すると、
残った構成が**新規にハードエラー**になる。

#### B2 を採らない理由(ここが本題)

**台帳が 2026-08-23 に既に方向を決めており、B2 はそれを逆走する。**

- `:5864` **「footprint はソケットが持つ(契約ではない)」**
- `:6411` **「footprint は 3 箇所に分かれている。ここを畳むのが仕事の実体」**
- `:6489-6493` **「扉は 2 枚ある。塞ぐなら 2 枚とも塞ぐこと」**
- `:6502-6505` **「アクセサを分けること(本命)。
  オフセット版を呼んだシェーダーは構造的に local read へ落とせなくなる。
  宣言を信じる代わりに、シェーダーが選んだアクセサが宣言になる」**

**B2 は footprint の宣言場所を 3 → 4 に増やす。**
そして **B2 が値を差し込む地点(`frameplanner.cpp:753` の直前)は
`agent/wp336`(`31f0971`)が既に占有している** ——
同ブランチの diff は `@@ -747,9 +826,43 @@ parseRenderNodeFromJson` を触っている。

**さらに B1/B2 が塞ぐのは 2 枚の扉のうち 1 枚の一部だけである** ——
`resource_ports` を持つ fullscreen / raster に限られ、次は開いたまま:

- **port を持たない fullscreen / raster** —— descriptor 型は port と無関係に
  `local_reads` の bool から決まり、位置束縛マクロ側も
  `PELICAN_TEXTURE_2D_0` が local read 時に `subpassLoad` になって uv を捨てる。
  **出荷 GPU テストの local_consumer が今この経路を使っている**
- **material の `screen_inputs`** —— `opaque_depth` / `scene_depth` / `linear_view_depth` の
  組み込み契約が `same_pixel` を持ち、作者は footprint を 1 文字も書かない。
  しかも `linear_view_depth` の生成コードは**捨てた uv を `inverse(projection)` の中で使い続ける**。
  `projects/example/shaders/depth_fade.surface` が出荷でこれを使っている
- **`material_resources.footprint`** —— 台帳が数えている 2 枚目の扉

#### 受け入れ条件が今の seam では書けない

WP340 の各版が要求した
「実 JSON から compose → lower → compile して
`ShaderResourceInterfaceBinding.descriptor` の**実解決値**を検査する」は**現状書けない**:

- `compileFullscreenResourceInterface` は
  `renderingpassruntimecompiler.cpp` の**無名 namespace 内**でヘッダに出ていない
- 末尾が `validateFullscreenSamplingCapabilities` を呼び、
  その中が `GET_MODULE(VulkanManageCore).getPhysDevice()` を触るので**device 無しでは通らない**

**受け入れ条件をコードに当てずに書いていた。**

#### 今日この扉が安全である根拠は、既に木の中で毎回走っている

出荷 JSON 81 本、output を持つパス 57 本で:
**自分の color/depth 出力を `input` にも持つパスが 0 件、
`input_footprints` / `read_footprints` / `same_pixel` / `allow_tile_local` が
リポジトリ中の全 `.json` で 0 件、`pass_overrides` が `input` を足す 10 箇所も基底パスの出力と交差しない。**

そして **tile-local を全部有効にした device facts で出荷 `example` / `animgraph_demo` を通す
device 無しテストが既に在り**、golden が
`materialized_image 63` / `tile_local_attachment 0` /
`selected_candidate = materialized_plan@1` で固定されている。
**WP340 が「config 由来で機械的に示せ」と要求したものは、毎回の ctest で走っている。**

#### 開けるときの条件

**次のどちらかが起きたら再検討する:**

- **機械的反証** —— 上の golden が動く。
  つまり出荷構成のどれかが tile-local を選ぶようになる
- **WP342(アクセサを分ける)に着手する** —— そちらが 2 枚とも閉じる

**`fullscreen.frag` の移行より前に WP342 を入れること。**
deferred lighting は gbuffer の attachment を読むので tile-local 融合の現実的な候補である。

---

### WP342: 取り下げ。負担の置き場所が違った(第 2 版)

**着手しない。替わりに立てるのは `### WP345:` である。**

#### なぜ取り下げるか

**アクセサを分ける案は、シェーダー作者に「自分がどう読むか」を名乗らせる。**
**ぼかしを書く人はタイルメモリを知る必要が無いし、知りたくもない。**
負担を、恩恵を受けない側に置いている。

**そして自然に書くと誰も融合を選ばない** ——
fullscreen パスで自分のピクセルを読むのも `texture(v, inTexCoord)` なので、
**素直に書けば全員が「どこでも読む」側を呼ぶ。**負担だけ増えて効果が出ない。

#### 台帳の決定との関係

`:6502-6505` の**向きは正しかった** ——「宣言を信じない」。
**しかし文言(シェーダーが選んだアクセサが宣言になる)は実装不能である。**

**物理プランはシェーダーを一度も見ずに確定する:**

- `compileRenderingTargetPlans` はシェーダーも shader library も PathResolver も受け取らない
- `FrameGraphNodeDefinition` にシェーダー名も `resource_ports` も無い
- `compileRenderingPassRuntime` は `target_plans` が出来た**後**に回る
- `surfacecompiler.hpp:28-30` が逆向きの契約を明記している

実装できるのは「宣言と食い違ったら落とす」までで、
**それは B1 を潰した purgeability の反論をそのまま継承する**
(広い footprint で読む optional feature を purge すると残りが新規にハードエラー)。
しかも診断が shaderc のエラーになる分だけ悪い。

#### 届く範囲も狭かった

- **tile-local に入る経路は 2 つではなく 5 つ。**
  5 枚目は作者が 1 文字も書かない暗黙経路で、**対応するアクセサが存在しない**
- **fullscreen が入力を読む流儀は 3 種類**あり、生成 port はその 1 つ。
  **出荷 `projects/example` の fullscreen 21 パス中 12 パス**(bloom 鎖)は
  `layout(set = PELICAN_SET_PASS_INPUT) uniform sampler2D` の手書きで、
  **分けるべきアクセサが存在しない**
  (救い: その流儀は黙って壊れず大きく落ちる。
   tile_local を選んだ RT は SAMPLED usage を剥がされる)

---

### WP345: 推論をやめる。融合は宣言と組み込み契約だけから

**§4 規則 11 の上段(engine の挙動が変わる)。仕様 + コード。**

#### 決めたこと(利用者の判断・2026-08-25)

```
シェーダー作者   ぼかしを書く。タイルメモリを知らない。何も宣言しない
パス作者         融合してほしいなら「同じピクセルしか読まない」と書く
エンジン         言われたときだけ融合する。推論しない
```

**負担を、恩恵を受ける側(パス作者)に置く。**

#### 塞ぐもの

`logicalframegraphadapter.cpp:376-389` が、
**`load` attachment かつ `reads` にある resource の footprint を
`same_pixel` と推論する。**作者は 1 文字も書いていない。

そこから tile-local が選ばれると、生成 accessor は uv を捨て、
**5×5 のぼかしが黙って同一ピクセル 25 回になる。**
**desktop と tile-based で別の絵が出る。**

#### 実測(再調査不要)

```
出荷 JSON の明示 same_pixel        0 件
出荷 JSON の input_footprints      0 件
load-op 推論が発火する出荷パス      0 件
```

**最後の数は `input` だけでなく `screen_inputs` / `material_resources` /
`surface_resources` / `reads` も含めて数えた。**
`color_load_op` / `depth_load_op` が `load` の出荷パスは 12 本あるが、
**自分がロードする attachment を読んでもいるパスは 1 本も無い。**

**したがってこの推論を外しても、今日の出荷内容は 1 ビットも変わらない。**

いま同一ピクセル読みとして成立しているのは
**`screen_inputs` の組み込み契約**(`opaque_color` / `opaque_depth` /
`scene_depth` / `linear_view_depth`)だけで、
**それは構造的に正しい** —— 透過パスが背後の不透明色を自分のピクセルで読む定義そのものである。

#### やること

1. **`load` attachment からの `same_pixel` 推論をやめる。**
   宣言が無ければ `arbitrary` にする
2. **融合の資格は「明示宣言」と「組み込み契約」からのみ与える**
3. **宣言とシェーダーが食い違ったら警告を出す**(下記の限界を承知の上で)

#### 警告の限界を仕様に明記すること

**食い違いの検出は、食い違いが害を生む構成でしか起きない。**

作者が `same_pixel` と書いたのにシェーダーが実際は隣を読んでいても、
**それが分かるのは融合が選ばれたときだけである。**
**desktop では融合が選ばれないので警告も出ない。**
開発機で気づけず、tile-based で初めて出る。

**したがって守っているのは「宣言しない限り融合しない」のほうであり、
警告は追加の保険である。仕様にそう書くこと。**

#### purgeability(ここが 4 案潰れた理由の解消)

```
潰れた案(B1/WP342): feature を外す → 融合が可能になる → 誰も宣言していないパスが新規にエラー
本案:                feature を外す → 融合が可能になる → 宣言した人のパスだけが融合される
```

**融合されるのは「安全だと誰かが保証したパス」だけになるので、
外したことで新しい失敗が生まれない。**

#### 受け入れ条件

**本番経路を通すこと。手組み plan で parser と既定値付与を迂回しないこと。**

- **推論が消えたことの否定対照を同じテストの中に置く:**
  同じ resource について、**明示宣言があるとき**と**無いとき**を
  同じテストで compose → lower → plan まで通し、
  **前者だけが `widest_read == same_pixel` になること**を実際の解決値で検査する
- **出荷 4 プロジェクトの物理プランが 1 ビットも変わらないこと。**
  **tile-local を全部有効にした device 無しテスト**(golden が
  `materialized_image 63` / `tile_local_attachment 0` /
  `selected_candidate = materialized_plan@1` で固定されているもの)が
  **変更前後で同一であること**
- **`screen_inputs` の組み込み契約が引き続き `same_pixel` を持つこと**
- **警告が出る構成を 1 つ作り、実際に出ることを検査する。**
  **そのとき「desktop では出ない」ことも同じテストで示すこと**
  (警告の限界が仕様であることを、テストが記録する)
- 出荷 4 プロジェクトと `pelican project init` / doclink 緑

#### やらないこと

- アクセサを分けること(WP342、取り下げ)
- `material_resources.footprint` と `input_footprints` の語彙整理
- 位置束縛マクロ経路と手書き sampler 流儀
- `pelican_size_` の修正(**WP346。独立に直せる**)

依存: 無し。見積: 中。

---

### WP346: tile-local のとき `pelican_size_` が嘘をつく

**§4 規則 11 の上段。仕様 + コード。WP345 と独立。**

#### 何が起きているか

生成 accessor は input attachment のとき:

```glsl
ivec2 pelican_size_<port>() { return ivec2(pelicanResolution.render_resolution.xy); }
```

**フレーム全体の描画解像度を返す。**

**しかし tile-local scope の extent は `getLocalReadScopeTargetExtent` が
scope の attachment 群から導き、全 attachment の一致を要求し、
フレーム解像度を一度も参照しない。**

**出荷 `projects/example` には `extent_scale` が
0.5 / 0.25 / 0.125 / 0.0625 の RT が 8 本ある。**
**そこで local read が起きれば、最大 16 倍ずれた数字がシェーダーに返る。**

#### やること

**`pelican_size_` が、その port が実際に読む対象の extent を返すようにする。**

`pelican_size_lod_` も同じ。

#### 受け入れ条件

- **描画解像度と異なる extent を持つ構成で、
  `pelican_size_` が正しい値を返すことを実際の解決値で検査する**
- **その構成で、修正前の値(描画解像度)と異なることを同じテストで示すこと**
- 出荷 4 プロジェクト / doclink 緑

依存: 無し。見積: 小〜中。
---

### WP341: Frame Plan のノードを畳んで、中に入れるようにする

**§4 規則 11 の下段(studio 内で閉じる・読み取りのみ・表示だけ)。コードレビューのみ。**
**engine を一切変更しない。**

#### 目的

**合成が展開したノード群を 1 個のノードとして畳み、ダブルクリックで中に入って見られるようにする。**
Blender のグループノードと同じ形である。

#### 先に測った事実(再調査不要)

**1. この機能は一度作られて、削除されている。**

`a460471`(2026-08-18、「view a target and its immediate neighbourhood instead of the whole graph」)が
`GroupDefinition` / `VisibleEntity` / `discoverGroups()` / `entityPath()` / `addEntityLabel()` を
丸ごと落とした。**残骸が残っている:**

```
frameplangraphics.hpp:47,51    FramePlanGroupItem / FramePlanGroupLabelItem   誰も作らない
frameplangraphics.cpp:715,1048 pelicanCollapsedGroups に常に空を書く
frameplangraphics.cpp:891      FramePlanInternalEdgeRecordsRole に常に空を書く
frameplangraphics.cpp:36,38    ColumnsPerRow / WrapRowHeight                  参照 0
devstudio_frameplan_graph_test.cpp:1034-1035  「グループ item が空であること」を検査している
```

**テストが「この機能が存在しないこと」を検査している。**

**旧実装には無かったもの**: ユーザーが畳む/開く操作、中に入る操作。
**そして今は `mousePressEvent` / `mouseDoubleClickEvent` / `contextMenuEvent` が
どこにも無く、`wheelEvent` のズームだけである。操作の入口ごと新設になる。**

**2. 辺の付け替えは既存機構でほぼ足りる。**

- **edge は幾何を一切持たない。**両端はノード名(`FramePlanFromNameRole` / `ToNameRole`)で、
  描画のたびに両端 item の `sceneBoundingRect()` から計算する
- `bundles` は `(from, to)` キーの map なので、
  **端点をグループ名に書き換えるだけで並行 edge が 1 本に畳まれる**
- **`from == to` になった内部 edge は 918 行で既に捨てられている**
- `dependencyLabel` の `x N` 表記も既にある
- 制約: `findNodeItem()` が `kind == "node"` でしか探さない。
  グループ枠に `"node"` を名乗らせるか、この関数を広げるか

**3. 配置は毎回再計算される。ただし穴が 2 つある。**

- 座標はどこにも保存されず `populate()` のたびに全再計算。
  列は `subtreeNodeColumns()` が `model.dependencies` 上で最長路緩和して自前計算する
  (`node.level` は使えないとコメントで明示的に却下されている)
- **穴 (a)**: ドラッグされたノードは `session_node_positions_` に入り、
  **この map は `resetGraph()` でも `populate()` でも一度もクリアされない。
  一度動かしたノードは永久に固定される。**
- **穴 (b)**: 合成したグループ名は `model.dependencies` に存在しないので、
  現状の `subtreeNodeColumns()` では**列を割り当てられない**

**4. グループの単位は今日 `provider_feature` しか無い。ただしそれは代用である。**

**本来の単位は subgraph replacement の region である。**
`spliceReplacement`(`src/project/subgraphreplacementregistry.cpp:783`)が
M ノードを落として N ノード(1..256)を挿す、リポジトリで唯一グラフのノード数を変える経路で、
設計文書が「1→N fullscreen 展開」と名指しして済としている。
**だが `regions` は frame plan の `nodes[]` に出ていない** ——
`framePlanToJson` が出す 18 フィールドに無く、region は logical graph のノード /
physical の lowering graph / physical scope にしか存在しない。
**そして出荷 JSON での subgraph replacement 使用は 0 件で、組み込み provider は恒等である。**

したがって本 WP は `provider_feature` を単位にする。**ただし単位の決定を 1 関数に閉じ込め、
region が frame plan に出た時点で差し替えられる形にすること。**

**5. 出荷 4 プロジェクトでは、既定で畳めるグループがほぼ無い。**

参照している feature は全部 N=1 か 0 である
(animgraph_demo = shadow_directional / sky_ambient / ui 各 1、example = ui 1、
sprite_demo = sprite 0、vrm_xr_demo = 空)。
**N≥2 になるのは:**

- `cube_capture` を足すと 8 ノード / `planar_reflection` を足すと 14 ノード
  (**両方とも凸であることを実測済み**)
- **DevStudio が player を起動するときは常に `editor.json` overlay が付き、2 ノードになる**

**落とし穴**: **overlay が展開した 2 ノードの `provider_feature` は
`gizmo` / `picking` であって `editor` ではない。**
provenance でグループを作ると**1 個の overlay が 2 グループに割れる。**
これは仕様として受け入れる(feature 単位でのグループ化として正しい)。

**6. devstudio は同名ノードを構造的に拒否する**(`frameplanmodel.cpp:581-584`)。
グループ名が既存ノード名と衝突しないことを保証すること。

#### やること

**1. グループの単位を 1 関数に閉じる**

`provider_feature` が非空のノードを、その値ごとに束ねる。
**`provider_feature` を持たないノードはグループを作らない**(`source` では束ねないこと ——
`project` で束ねると利用者が自分で書いたパスが 1 個に消える)。

**2. 畳めるかを判定する。畳めないグループは畳まない**

**グループ `S` が畳めるのは、`S` から出て `S` に戻る経路が無いとき(凸)に限る。**
そうでない集合を 1 ノードにすると、商グラフに循環ができて順序について嘘をつく。

**凸でないグループは展開のまま表示し、その理由を UI に出すこと。**
黙って畳まないのは不可(fail-fast)。

**3. 畳んだ表示**

- グループ 1 個 = 1 個のノード item。**中のノード item は作らない**
- 辺は端点をグループ名に書き換える(並行 edge の畳み込みと自己辺の除去は既存機構が行う)
- **列の割り当て**: `subtreeNodeColumns()` はグループ名を知らない。
  **グループの列 = メンバの列の最小値**とするなど、決め方を明示して実装すること

**4. 中に入る / 出る**

- **ダブルクリックで入る。**入ると**そのグループのメンバだけ**を表示する
- **境界を跨ぐ辺は境界スタブとして表示する**(外の何に繋がっているかが分かること)
- **出る手段を 2 つ用意する**: パンくず(現在のスコープを表示し、クリックで戻る)と `Esc`
- **入れ子は本 WP では作らない**(今日の単位は 1 段しかない)

**5. 状態**

- 畳んだグループの集合と、現在のスコープを持つ
- **既存の `pelicanCollapsedGroups` プロパティを使うこと**(常に空を書いている箇所を実装で置き換える)
- **frame plan の更新をまたいで保つこと。**
  更新後に消えたグループの状態は捨てること(孤児を残さない)

**6. 残骸を片付ける**

- `ColumnsPerRow` / `WrapRowHeight` を削除する(参照 0)
- `FramePlanGroupItem` / `FramePlanGroupLabelItem` / `FramePlanInternalEdgeRecordsRole` は
  **実装で使うか、使わないなら削除する。**空を書き続ける形を残さないこと
- `devstudio_frameplan_graph_test.cpp:1034-1035` の
  「グループ item が空であること」の検査を、実際の期待に置き換える

**7. `session_node_positions_` のクリア**

`resetGraph()` でクリアすること。
**今日は一度ドラッグしたノードが永久に固定される。**
畳み/展開でノード集合が変わるので、放置すると配置が壊れる。

#### 受け入れ条件

**否定対照を同じテストの中に置くこと**(§4 規約 10):

- **畳む前**: メンバのノード item が `N` 個存在し、グループ item は 0 個
- **畳んだ後**: メンバのノード item が 0 個、グループ item がちょうど 1 個
- **同じテストの中で両方を実行し、item の実際の集合を比較すること。**
  「グループ item が 1 個ある」だけでは、メンバが消えたことを検査していない

**辺の正しさ**:

- **畳む前と畳んだ後で、到達可能性が商グラフとして一致すること。**
  外部ノード `u` からグループ `S` に辺があったなら、畳んだ後 `u → S` の辺がちょうど 1 本あること
- **内部辺(`S` の中で閉じる辺)が畳んだ後に 0 本であること**
- **並行辺が 1 本に畳まれ、`x N` 表記が付くこと**

**凸性**:

- **凸でないグループを含む frame plan を fixture で作り、それが畳まれないこと**、
  かつ**理由が UI に出ること**を検査する。
  実データで凸でない実例が作れないなら、**手組みの `FramePlanModel` で作ること**

**スコープ**:

- 入ると**メンバだけ**が表示され、外のノードが 0 個であること
- **境界スタブが、外の実際の接続先の数だけ存在すること**
- `Esc` とパンくずの両方で出られること
- **出たあと、入る前と同じ item 集合に戻ること**

**状態の保持**:

- frame plan を更新しても畳んだ集合が保たれること
- **更新で消えたグループの状態が捨てられること**(`pelicanCollapsedGroups` に孤児が残らない)

**そのほか**:

- **`session_node_positions_` が `resetGraph()` でクリアされること**を検査する
- **`ColumnsPerRow` / `WrapRowHeight` がリポジトリから消えていること**
- **`SKIP_DEVSTUDIO=ON` でビルドが通ること**
- `uv run tools/doclink.py check` が緑

#### やらないこと

- **engine を変更しないこと。**`regions` を frame plan に出すのは別 WP(engine 側 = 上段)
- 入れ子のグループ
- グループの手動作成(利用者が任意のノードを選んで囲む)
- subgraph replacement provider を書くこと
- ノード配置アルゴリズムの作り替え

#### 次

**`regions` を frame plan の `nodes[]` に出す**(engine 側、§4 規則 11 の中段)。
それが入ると本 WP の「単位を決める 1 関数」を region に差し替えられ、
**subgraph replacement が展開した本物の 1→N がグループとして見えるようになる。**
`for` はその provider として実装される。

依存: 無し。見積: 中。
---

#### コードレビュー(codex、`f045358` 直後)の仕分け

**判定 Reject。実害 5 件 + テストの偽陰性 2 件。全部直す(WP341a)。**
**production は studio 内で閉じたまま。**

**中核は証明された** —— 凸性判定について、
**4 頂点の全有向グラフ(自己ループ込み)× サイズ 2 以上の全部分集合、
計 720,896 ケースを独立の到達可能性判定と突き合わせて不一致 0。**
BFS に上限も打ち切りも無く、自己ループ・孤立ノード・外部 cycle も `visited` で正しく終わる。

**直すもの:**

1. **グループ状態 ID が衝突する。**
   `graph + '\x1f' + "feature:" + provider_feature` の**無エスケープ連結**なので、
   `graph="g"` / `feature="x\u001ffeature:y"` と
   `graph="g\u001ffeature:x"` / `feature="y"` が同じ ID になる。
   JSON は `\u001f` を書けて、parser は文字列型しか検査しない。
   **構造化キー `(graph, group_key)` にすること**
2. **scope 中のグループが非凸になると、不正な scope に残る。**
   `pruneGroupState()` は畳み状態を `collapsible_groups` で照合するのに、
   **scope は `existing_groups` にしか照合していない**。
   したがって畳み状態は消えるのに `pelicanCurrentGroupScope` が残り、
   `enterGroup()` が新規には拒否する非凸グループの内部に、更新経由なら留まり続けられる。
   **scope も `collapsible_groups` で照合し、非凸化したら強制的に外へ出して理由を出すこと**
3. **位置の保存キーが安定していない。**
   `entity->key`(その都度合成した表示名)を使っているので、
   衝突ノードが消えて suffix が変わるとドラッグ位置を失い、
   逆に消えた利用者ノードの位置をグループが継承する経路もある。
   **`{graph, kind, 安定した group state key}` にし、表示名から切り離すこと**
4. **畳んだメンバの選択が孤児になる。**
   メンバを選択して畳むと `visible_names` にメンバ名が在るので `selected_node_` が保持されるが、
   `node_items` にはグループしか入らない。
   **scene の選択 item が 0 個なのに `pelicanSelectedNode` は隠れたメンバ名のまま**になり、
   `rebuilding_` のせいで自然回復もしない。
   **選択をクリアするか、グループ選択へ明示的に変換すること**
5. **`Esc` はグラフに focus が在るときしか効かない。**
   `QGraphicsScene::keyPressEvent()` だけで処理していて、widget 側に shortcut が無い。
   **テスト自身が `view.viewport()->setFocus()` でこの穴を避けている。**
   `FramePlanWidget` に `Qt::WidgetWithChildrenShortcut` の shortcut を置くこと
6. **否定対照が「別 kind で残るメンバ」を見逃す。**
   `nodeItem()` は `FramePlanNodeItem` だけを探すので、
   **メンバ item を消さずに `FramePlanItemKindRole` を空や別 kind に変える変異**が通る。
   さらに `annotatedSceneItems()` は kind が空の item を無視し、
   比較基準を畳んだ**後**に取得している。
   **kind 非依存で「メンバ名を持つ shape/label が 1 つも無い」ことを検査し、
   畳む前の実 item 集合から商写像で作った期待集合と比較すること**
7. **正常系が手組みモデルだけで、本番の provenance 経路を通っていない。**
   `framePlanToJson()` が `provider_feature` を出さなくなる回帰でもテストが通る。
   **同ファイルに `resolvedFramePlan()`(実際に compose→compile→plan→wire する)が既に在る。**
   正常系は `cube_capture` / `planar_reflection` / `taa` のいずれかを実際に合成して検査すること。
   **手組みモデルを許すのは非凸 fixture だけである**

**台帳に残すもの(直さない):**

- **グループ全体は 2 個以上でも、現在の subtree に 1 メンバしか見えない場合、
  1 item をグループへ畳み、ラベルは全メンバ数を表示する。**
  中へ入ると subtree 外だった全メンバへ表示が広がる。
  **WP341 がこの挙動を明記していなかった。仕様として受け入れる**
  (subtree 表示は「対象の近傍を見せる」`a460471` の意図であり、
  グループはその外側のメンバも持つのが正しい)
- `resetGraph()` は RPC の切断・再接続・failure から呼ばれるので、
  **その経路でドラッグ位置が失われる。**WP341 が明示的に要求した挙動である
---

### WP343: 作者が書いた region でグループを作る

**§4 規則 11 の下段(studio 内で閉じる・読み取りのみ・表示だけ)。コードレビューのみ。**
**engine を一切変更しない。WP341 の「単位を決める 1 関数」を差し替えるだけである。**

#### 先に測った事実(再調査不要)

**1. engine 変更は要らない。データは既に studio まで届いている。**

```
physical_target_plan.lowering_graph.nodes    30 件(example)
frame plan の nodes と名前が一致              30 / 30
各ノードが regions を持つ
studio は既にパース済み                       frameplanmodel.cpp:800 → FramePlanLoweringNode::regions
```

**`framePlanToJson` の 18 フィールドに `regions` が無い**のは事実だが、
**lowering graph 側に在り、名前で 1:1 に突き合わせられる。**
`FramePlanPhysicalScope` にも regions が在る(`frameplanmodel.cpp:829`)。

**2. `regions` は作者が書ける。今日から。**

`parseOptionalRegionTags`(`renderingpassjsonhelpers.cpp:311`)が
pass の `"regions"` を文字列配列として読む。
検証は**空でないこと・`maximumLogicalRegionTagBytes` 以下・重複が無いこと**の 3 つ。
render pass(`frameplanner.cpp:689`)と compute task(`:937`)の両方で読まれる。

**出荷 JSON に `region` の記述は 1 件も無い。**

**3. 全ノードに `legacy.<kind>` が自動で付く。**

`logicalframegraphadapter.cpp:207-215` が
`legacy.render` / `legacy.compute` / `legacy.anchor` /
`legacy.snapshot_copy` / `legacy.output_transform` を必ず 1 つ足す。
**実測分布(example): `legacy.render` 19 / `legacy.anchor` 8 /
`legacy.snapshot_copy` 2 / `legacy.output_transform` 1。**

**これはグループの単位として無意味である**(全 render パスが 1 個になる)。

**4. 予約接頭辞の検査が無い。**

`parseOptionalRegionTags` は `legacy.` で始まるタグを拒否しない。
**作者が `legacy.render` と書けてしまう。**

#### やること

**1. グループの単位を「作者が書いた region tag」にする**

WP341 が 1 関数に閉じ込めた単位決定を差し替える。

- lowering graph のノードを**名前で** frame plan のノードに突き合わせ、その `regions` を読む
- **`legacy.` で始まるタグを除外する**
- 残ったタグごとにグループを作る

**`provider_feature` によるグループは残すこと。**
両方在るときの優先順位を決めて明記すること。

**2. 1 ノードが複数の region に属する場合**

**そのノードはグループ化しない。理由を UI に出すこと。**
入れ子は WP341 の範囲外であり、重なり合うグループを同時に畳むのは意味が定まらない。
**黙って片方を選ばないこと。**

**3. 凸性は WP341 の判定をそのまま使う**

作者が書く region は任意の集合なので、**非凸な region を書ける。**
WP341 の「凸でないグループは畳まず理由を出す」がそのまま効くこと。

#### 受け入れ条件

**否定対照を同じテストの中に置くこと**(§4 規約 10):

- **region を書く前**: グループ item 0 個、メンバのノード item が N 個
- **書いた後**: グループ item 1 個、メンバのノード item 0 個
- **同じテストの中で両方を実行し、item の実際の集合を比較すること**

**本番経路を通すこと(ここを外さない):**

- **正常系は実際の JSON に `"regions"` を書いて
  compose → compile → plan → wire を通すこと。**
  WP341 の `resolvedFramePlan()` / TAA fixture と同じ流儀
- **手組みの `FramePlanModel` を許すのは、本番経路で作れない状態だけ** ——
  非凸 region、複数 region への所属
- **`legacy.*` が除外されることを、本番経路で実際に確かめること。**
  `legacy.render` が 19 ノードに付いている構成で、
  **それがグループにならないこと**

**変異を実際に入れて落ちることを確かめ、報告すること:**

- `legacy.` の除外を外す → `legacy.render` の 19 ノードが 1 グループに畳まれて落ちること
- 名前の突き合わせを壊す(lowering node を別名で引く)→ グループが 0 個になって落ちること

**そのほか:**

- **複数 region に属するノードが畳まれず、理由が画面のテキストとして出ること**
  (data role だけでなく)
- **作者が `legacy.render` と書いた場合の挙動を決めて検査すること。**
  除外されるなら、そのタグでグループを作れないことが利用者に分かること
- `SKIP_DEVSTUDIO=ON` でビルドが通ること
- `uv run tools/doclink.py check` が緑

#### やらないこと

- **engine を変更しないこと。**`regions` を frame plan の `nodes[]` に出すのは別 WP。
  **lowering graph 経由で足りることが実測で分かっている**
- 入れ子のグループ
- studio からの region 編集(書き込み)
- `legacy.` を engine 側で予約すること(engine 変更)

#### 次

**`regions` を利用者が書くと何が起きるかが見えるようになるので、
subgraph replacement の provider を書く動機がそこで初めて立つ。**
`for` はその provider として実装される。

依存: WP341(マージ済み `f045358` / `a341fc2`)。見積: 小〜中。
---

#### コードレビュー(codex、`922a5bf` 直後)の仕分け

**判定 Reject。実害 1 件 + テスト/契約の欠陥 4 件。全部直す(WP343a)。**
**production は studio 内で閉じたまま。**

**再発しなかったこと(重要)** —— `a341fc2` で直した 2 つの穴は今回踏んでいない:
正常系は実 JSON → resolve → compile → plan → wire → physical plan を通り、
`state == available` も直接検査している。メンバ消失検査も item kind 非依存である。

**直すもの:**

1. **【実害】lowering 情報の「不明」を「region なし」として扱っている。**
   `loweringNode()` が見つからないとき、エラーにせず空の region 集合として
   `provider_feature` へフォールバックする。
   **具体例: preview variant は仕様上 `physical_target_plan` を持たない。**
   したがって preview では**作者が書いた region が黙って消え**、
   feature グループに見えるか 0 件になる。
   **「region 情報が利用不能」という表示が無く、正常な feature fallback と区別できない。**
   さらに **physical plan を一度読んだ後に `runtime_resolution` 不足で
   `state = unavailable` になった場合は lowering nodes が残るので region grouping は動く** ——
   **同じ unavailable でも内部の残骸の有無で挙動が変わっている**
2. **優先順位テストが混在 membership を検査していない。**
   fixture の region 集合と feature 集合が完全に同一なので、
   **「region を持つノードを見つけたら同じ feature の全ノードを region group に入れる」
   誤実装でも通る。**
   本番経路で feature の一部だけに region を付けて検査すること
3. **`legacy.` の境界変異が通り、prefix が二重管理されている。**
   `LegacyRegionPrefix` を `"legacy"` に変える一文字変異が全テストを通り、
   正当な `"legacy"` や `"legacy日本"` まで除外してしまう。
   **prefix が判定用定数と tooltip に重複記述**されていて
   「既定値の所在は一箇所」に反する。tooltip テストは実際の prefix 文字列を検査していない
4. **compiler-OFF ビルドでもテストが ON の意味論を強制する。**
   production fixture 2 つが `.runtime_shader_compiler_enabled = true` を固定しているので、
   `-DPELICAN_RUNTIME_SHADER_COMPILER=OFF` でも ON 能力で resolve する。
   **CMake は実際のマクロをテストへ渡している**(`test/CMakeLists.txt:257`)
5. **手組みモデル 2 つが不可能な physical state を作っている。**
   `nonConvexRegionGroupingModel()` と `overlappingRegionGroupingModel()` は
   lowering nodes を入れるのに `physical_plan.state` を available にしていない。
   通常の parser ではこの組合せは生成されない。
   **これが finding 1 の「state を無視して内部 vector を読む」挙動を正当化している**

**現在の `legacy.` 境界(実測)**:

| 入力 | 扱い |
|---|---|
| `legacy.render` / `legacy.日本` / `legacy.` / `legacy.render ` | 除外 |
| `legacy` / `Legacy.render` / `legacy日本` / ` legacy.render` / 空白だけ | authored region |

parser は空文字だけを拒否し、trim も case-fold もしない。
#### `-j16` の全数実行でだけ落ちるテスト(2026-08-25 観測)

**通算およそ 12 回の全数実行で 3 回、毎回違うテストが 1 件だけ落ちた。
どれも単独では通り(それぞれ 3/3)、部分集合の並列でも通る。**

| 回 | テスト | ファイル |
|---|---|---|
| 1 | `WP334b production project-open waits for engine context...` | `devstudio_fullscreen_pass_test.cpp` |
| 2 | `WP335 parameterized feature entries stay byte-exact...` | `renderconfigeditor_test.cpp` |
| 3 | `WP332 no-op preserves bytes generation and apply count...` | `renderconfigeditor_test.cpp` |

**調べて潰した線:**

- **`TemporaryConfig` のディレクトリ名衝突ではない**(`renderconfigeditor_test.cpp:98-120`)。
  steady_clock の seed + プロセス内 atomic で作り、
  `create_directory` が失敗したら 100 回まで名前を変えて再試行する
- **順序依存ではない** —— `WP33[0-9]` を `-j16` で回すと通る
- **作業木を汚していない** —— 失敗後も `git status` は clean

**未検証の仮説:**

- ctest は TEST_CASE ごとに**別プロセス**を起動し、これらは同じ実行ファイルである。
  したがって**プロセス間で共有されるもの(実質ファイルシステム)**が疑わしい
- 観測時のディスク使用率は 90%。`-j16` で GPU テストも同時に走るため、
  一時ディレクトリの圧迫やウイルス対策のスキャンによる一過性の失敗も切れていない

**扱い:** 全数を 1 回落ちたら**同じテストを単独で回して確かめること。**
単独で通るなら本件である。**「全数緑」を主張する前に 2 回回すこと。**

**失敗時の assertion をまだ捕まえられていない** ——
`build/Testing/Temporary/LastTest.log` は次の実行で上書きされるので、
再現したら**その場で保存すること。**
---

### WP344: 出荷の prefilter 鎖に region を書く

**§4 規則 11 の中段**(出荷の engine feature JSON を変更する)。**仕様 + コード。**
**studio の実装は変更しない。WP343 の機構が出荷内容で初めて何かを表示するようにする。**

#### なぜこれか

**`planar_reflection` の `filter_mip_1..6` は、feature JSON の `compute_tasks` に
6 個手で書かれた繰り返しである。**あなたが挙げた「`for` の扱い」の実物であり、
**今日それを 1 個の宣言に畳むことはできない**
(subgraph replacement は fullscreen 限定で compute に乗らない)。

**しかし畳んで「見る」ことは今日できる** —— `regions` は compute task でも読まれる
(`frameplanner.cpp:936`)。

#### 測ってある事実(再調査不要)

**6 件はグループとして理想的な形をしている**(実測):

```
6 ノード、order 24-29 で連続、凸
内部辺 5        mip_1 → 2 → 3 → 4 → 5 → 6 の鎖
入る辺 1        planar_reflection_forward_transparent → mip_1
出る辺 2        forward_transparent と output_transform
```

**畳むと 1 ノード、入る辺 1 本・出る辺 2 本、境界スタブ 3 本になる。**

**訂正(着手時に判明)**: 初版は「出る辺 1 本・スタブ 2 本」と書いていた。**誤りである。**
`featurecompose.cpp:458` が **feature 合成時に全 compute task から
`output_transform` への終端依存を自動追加する。**
初版の測定は `--dump-frame-plan` の `barriers`(resource の read-after-write)から取っており、
**終端依存は順序の辺であって barrier ではないので出ていなかった。**
**1 つの表現を測って別の表現について断定した誤りである。**

- **`regions` は fingerprint に入らない**(`logicalrendergraph.cpp` に該当なし)
- **出荷 golden に `regions` は 1 件も無い**
- テストは feature を ref で参照しており、JSON をバイト単位で検査していない

#### やること

**1. `src/core/resources/features/planar_reflection.json` の
`compute_tasks` 6 件に `"regions": ["planar_reflection.prefilter"]` を足す**

**それだけである。**他のフィールドを変更しないこと。

**2. 出荷内容でグループが出ることを検査する**

#### 受け入れ条件

**本番経路を通すこと:**

- **実際に `planar_reflection` feature を合成して**
  resolve → compile → plan → wire → physical plan → studio model → scene を通す
- **手組みの `FramePlanModel` を使わないこと**(本 WP は出荷内容の検査である)

**検査すること:**

- **グループのメンバがちょうど 6 件で、`filter_mip_1..6` と完全一致すること**
- **畳めること**(凸である)
- **畳んだ後、グループ item が 1 個、メンバの shape / label が 0 件**
  (**kind 非依存で検査すること**)
- **畳んだ後、グループに入る辺が 1 本、出る辺が 2 本であること**
  (`forward_transparent` と `output_transform`)
- **中に入ると 6 件が見え、外のノードが 0 件、境界スタブが 3 本
  (`planar_reflection_forward_transparent` / `forward_transparent` / `output_transform`)であること**

**変異を実際に入れて落ちることを確かめ、報告すること:**

- **6 件のうち 1 件から `regions` を外す** → メンバが 5 件になって落ちること。
  **そのとき残り 5 件が依然として凸であることも確認し、報告すること**
  (凸性が壊れて畳めなくなるのか、5 件のグループになるのか)

**そのほか:**

- **出荷 4 プロジェクトと `pelican project init`**
- **`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS` の ON / OFF 両構成**
  (`planar_reflection` は ON でしか登録されない)
- **既存の planar 実行テストを壊さないこと** ——
  `GoldenHarness::runPlanarReflection()`(`test/golden_harness.cpp:9183`)は
  `execution_trace` のノード名を**厳密一致**で検査する(件数 15)
- `uv run tools/doclink.py check` が緑
- `uv run test/golden_inventory.py` が緑

#### やらないこと

- **studio の実装を変更しないこと**(WP343 / WP343a の機構をそのまま使う)
- 他の feature に region を書くこと
- `filter_mip` を 1 個の宣言に畳むこと(**それは 1→N provider の仕事で、
  compute に乗らないので engine 側の作業が要る**)

#### 次

**これで「出荷内容にグループが出る」状態になる。**
その次は **1→N provider** だが、**畳みたい鎖(compute)と機構(fullscreen 限定)が
交差しない**ので、先に engine 側の判断が要る ——
subgraph replacement を compute に広げるか、feature が `graph_transforms` を
要求できるようにするか。

依存: WP343a(マージ済み `287ff81`)。見積: 小。
---

### WP347: 見えている部分木の同期を辺に描く

**§4 規則 11 の下段(studio 内で閉じる・読み取りのみ・表示だけ)。コードレビューのみ。**
**engine を一切変更しない。**

#### 名乗れる範囲を先に決める

**「同期を見せる」とは名乗らない。「いま見えている部分木の同期を見せる」である。**

**Frame Plan のグラフはフレームグラフ全体ではない** ——
選択した 1 リソースの、深さ制限つき**部分木**である。
**両端のどちらかが可視窓の外にある依存は捨てられるので、
「辺が無いのにバリアが在る」は例外ではなく通常状態である。**
ターゲット未選択ならノード 0 件、それでも `model.barriers` は満杯のまま。

#### 測ってある事実(再調査不要)

**1. studio は既にバリアを持っている。**

- `FramePlanBarrier` は 4 フィールド: `kind` / `resource` / `from` / `to`
  (`frameplanmodel.hpp:95-102`)。engine 側と完全に同じで、落としているものは無い
- **ノードごとに `incoming_barriers` / `outgoing_barriers`**(`:89-90`)——
  `model.barriers` への添字。**studio の `buildFramePlanModel` が埋めている**
  (engine からは送られてこない)

**2. 辺はバリアから引かれていない。**

- **辺は `execution_plan.dependencies` から引かれ、ノード対ごとに束ねられる**
- **`barriers` は root 直下、`dependencies` は `execution_plan` の中** —— 取得元が違う
- **`dependencies ⊇ barriers`** —— 1 本のバリアは必ず 1 本の dependency になるが、
  逆は成り立たない(`explicit_after` / `explicit_before` / `snapshot_after` は
  resource が空でバリアを持たない)
- **`execution_plan` が unavailable だと `dependencies` は空のまま**、
  **`barriers` は在る**

**3. 種別は少ない。**

- **バリアの kind は 2 つだけ**: `read_after_write` / `write_after_write`
  (enum ではなく素の `std::string`)
- **dependency の reason は 5 つ**: 上の 2 つ + `explicit_after` / `explicit_before` /
  `snapshot_after`

**4. 同期の実質は publish されていない。**

**stage mask / access mask / layout 遷移 / queue family / by-region は 1 つも
publish されていない。**実行時に `renderer.cpp` と `render_pass_executor.cpp` で
ハードコードされている。

**publish されていて、かつ意味のあるもの:**

- **「by-region 相当」の論理情報**(footprint = `same_pixel` かつ intent = attachment)
- **そのバリアが融合 scope に吸収されるか**(physical の `scopes.local_reads`)

**5. いまバリアが出ている場所は 3 つ。**

`frameplangraphics.cpp` に `barrier` の語は**1 件も無い**。
出ているのは frameplanwidget の Barriers タブ / Passes 詳細ツリー / ステータス行。

**6. 畳んだグループの内部バリアは辺にならない。**
`internal_records` の文字列に退避する(WP341 の機構)。

#### やること

**1. 辺にバリアの情報を出す**

辺は既にラベル箱とテキストを持つ(`x N` 表記)。そこに:

- **その辺が持つバリアの kind**(`read_after_write` / `write_after_write`)
- **バリアを持たない依存**(`explicit_after` / `explicit_before` / `snapshot_after`)を
  **バリアのある辺と区別できるようにする**。
  **順序だけの辺と、同期を伴う辺は違うものである**
- **融合 scope に吸収されるバリア**を区別する

**2. 見えていないバリアを黙らせない**

**可視窓の外に落ちたバリアの件数を出すこと。**
「同期が無い」と「同期が見えていない」を区別できること。

**`execution_plan` が unavailable のとき、
`barriers` は在るのに辺が 1 本も無い状態になる。そこも明示すること。**

**3. publish されていないものを描かないこと**

stage / access / layout / queue family は**無い**。
**「同期を見せている」と誤解させる表示をしないこと。**
出せるのは kind と、by-region 相当と、融合吸収の 3 つだけである。

#### 受け入れ条件

**本番経路を通すこと。手組みの `FramePlanModel` を許すのは、
本番経路で作れない状態だけ。**

**否定対照を同じテストの中に置くこと:**

- **バリアを持つ辺と、順序だけの辺(`explicit_after` など)を
  同じ frame plan の中に作り、両者の表示が異なることを検査する**
- **kind 非依存で item を数えないこと** ——
  WP341a の `namedShapesAndLabels` の流儀に従う

**見えていないバリア:**

- **可視窓を狭めて(depth を下げて)、窓の外に落ちたバリアの件数が
  実際に表示されることを検査する**
- **ターゲット未選択でノード 0 件のとき、`model.barriers` が満杯であることと、
  その旨が表示されることを検査する**

**融合との組み合わせ:**

- **畳んだグループの内部バリアが辺として出ず、
  かつ「内部に N 本ある」ことが分かることを検査する**

**そのほか:**

- `SKIP_DEVSTUDIO=ON` でビルドが通ること
- `uv run tools/doclink.py check` が緑

#### やらないこと

- **engine を変更しないこと。**stage / access / layout を publish するのは別 WP(上段)
- 実行時間の表示(別件。`gpu_timing` は studio の player で有効にできない)
- Barriers タブ / 詳細ツリーの作り替え

依存: WP341 / WP343(マージ済み)。見積: 小〜中。
---

### WP348: 複数ノードのループを実際に走らせて測る

**§4 規則 11 の中段(engine の挙動は変えないが、engine の振る舞いを観測する)。仕様 + コード。**
**production コードを変更しない。出荷 JSON も変更しない。**

#### なぜこれが要るか

**`for` をどう設計するかの判断材料が、いま「読んだコード」しか無い。**

**出荷で走っているループは `animgraph_demo` の `shadow_directional` ただ 1 つで、
その family のノードは 1 個である。**
つまり **scope-major のループ分裂を、出荷構成が一度も非退化に実行していない。**

**`cube_capture` は唯一の非退化なループ本体**(8 ノード・N=6)だが、
**出荷宣言 0 件。**`planar_reflection` も `clustered_lighting` も 0 件。

**誰も複数ノードのループを走らせたことがない。**

#### 測ってある事実(再調査不要)

- **スケジューラは scope-major** ——
  `for i {A;B;C}` ではなく `for i{A}; for i{B}; for i{C}` の**ループ分裂**。
  `buildLogicalFrameViewFamilySchedule` が execution_index × scope_node の二重ループで
  N×M 個の `LogicalFrameNodeInvocation` を push し、**全部同じ `node_index` を指す**
- **反復間にバリアが入らない** ——
  `addWriteAfterWriteBarriers` は**異なるノード間しか歩かず**、
  `validateWritesAreOrdered` は `j=i+1` なので**同一ノードの自己 WAW を構造上見られない**
- **`cube_capture` は自前で `render_targets` を宣言している**ので、
  features に 1 行足すだけで有効になる。**engine 改変不要**
- **これらは全部 CPU 側の計画である。**device 無しで観測できる

#### やること

**テストの中で `hybrid_v1` + `cube_capture` を合成し、本番経路を通して観測する。**

**出荷プロジェクトを変更しないこと** —— WP343 / WP344 の fixture と同じ流儀で、
テストが config を組んで `resolveRenderPipeline` から通すこと。

**まず測って報告すること。数字を先に決めないこと。**

1. **invocation 列の形** ——
   `LogicalFrameNodeInvocation` の列を実際に取り出し、
   **`for i{A}; for i{B}` なのか `for i {A;B}` なのか**を実物で示せ。
   **8 ノード × 6 view の 48 invocation がどの順に並ぶか**
2. **各中間資源の層数** ——
   `$capture/cube` family の資源が何層で確保されるか。**6 層か、1 層か**
3. **反復間のバリア** ——
   **同じノードの反復 i と i+1 のあいだにバリアが在るか。**
   `model.barriers` と `execution_plan.dependencies` の両方で確かめよ。
   **WP347 が入ったので、辺の表示からも確認できる**

**測った結果を assertion として固定すること。**
**ただし「こうなるはず」で書かず、測った値を書くこと。**
**予想と違ったら、その旨を報告に明記すること。**

#### 受け入れ条件

**本番経路を通すこと。手組みの `FramePlanModel` を使わないこと。**

- **上の 3 点それぞれについて、実測値を assertion で固定していること**
- **`cube_capture` の 8 ノードがグループとして畳めること**(WP343 の機構)——
  **凸かどうかも実測し、報告すること**
- **device 無しで走ること**(GPU を要求しないこと)
- **`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF` では `SKIP`**(`SUCCEED` ではなく)
- **`PELICAN_RUNTIME_SHADER_COMPILER=OFF` でも成立すること。
  成立しないなら `SKIP` し、理由を報告すること**
- `uv run tools/doclink.py check` が緑

#### やらないこと

- **production コードを変更しないこと**
- **出荷 JSON を変更しないこと**(有効化はテストの中だけ)
- ループ機構を作ること / 直すこと
- 反復間バリアを足すこと

#### 次

**この測定が `for` の設計の出発点になる。**
特に **3 番(反復間バリア)** が、
「ループエリアの中に複数ノードを入れる」を設計するときの最初の判断になる ——
**鎖のループ(FFT・ブラー)は反復間の同期を要求するが、
今日のエンジンは同一ノードの自己 WAW を構造上見られない。**

依存: WP343 / WP347(マージ済み)。見積: 小。
---

### WP349: ノードを複数選んで一緒に動かす

**§4 規則 11 の下段(studio 内で閉じる・表示だけ)。コードレビューのみ。**
**engine を一切変更しない。**

#### 測ってある事実(再調査不要)

- **選択は今日 1 個だけ** —— `selected_node_` は `std::optional`。
  ただし **`ItemIsSelectable` は既に設定されている**(`frameplangraphics.cpp:1951`)
- **ノードは既に動かせる** —— `ItemIsMovable`(`:1948`)、`MovableNodeItem`(`:158`)
- **ドラッグ位置は WP341a で `{graph, kind, 安定した識別子}` を鍵にした。**
  表示名から切り離してあるので、複数選択でもそのまま使える
- **`resetGraph()` で位置がクリアされる**(WP341 で入れた)

#### やること

1. **複数選択**(矩形選択 / Ctrl+クリックによる追加・解除)
2. **選択したノードを一緒に動かす**
3. **選択の公開** —— いまの `pelicanSelectedNode` は 1 個前提である。
   **複数を表現する形を決めて公開すること。**
   **既存の 1 個の property を壊さないこと**(他の widget が使っている)

#### 受け入れ条件

**本番経路を通すこと。手組みの `FramePlanModel` を許すのは、本番経路で作れない状態だけ。**

- **N 個選んで動かすと、N 個すべての位置が変わること。**
  **選んでいないノードの位置が変わらないこと**(同じテストの中で両方)
- **位置が frame plan の更新をまたいで保たれること**(WP341a の鍵で)
- **選択が畳み / スコープ移動と整合すること** ——
  WP343a が「畳んだグループのメンバが選択されていたら選択を消す」を入れた。
  **複数選択でも同じ規則が効くこと**
- **既存の単一選択の挙動が壊れないこと**(`pelicanSelectedNode` を使う既存テスト)

**変異を実際に入れて落ちることを確かめ、報告すること:**

- **移動を選択の先頭 1 個だけに適用する** → 落ちること

**そのほか:**

- `SKIP_DEVSTUDIO=ON` でビルドが通ること
- `uv run tools/doclink.py check` が緑

#### やらないこと

- **engine を変更しないこと**
- 選択からグループを作ること(**WP350**)
- 位置の永続化(セッション限りのまま)
- 配置アルゴリズムの作り替え

依存: WP341a(マージ済み)。見積: 小。

---

### WP350: 選んだノードをその場でグループにする

**§4 規則 11 の下段(studio 内で閉じる・表示だけ)。コードレビューのみ。**
**engine を一切変更しない。**

#### 書き戻さない。理由を記録する

**選んで作ったグループは、そのセッション限りの表示である。config に書かない。**

**書く経路が今日存在しない** ——
`edit_render_features` は feature の `add` / `remove` だけで(v1 は 1 操作まで)、
`add_authored_pass` は「新しいパス 1 個」を管理フラグメントとして書く。
**既存パスの `regions` フィールドを設定する経路が無い。**

**そして Frame Plan は読むための画面である。**
region の正本は config であり、それは WP343 が拾う。
**永続化が要るなら、`regions` を既存パスに設定する RPC を engine 側に足す別 WP になる。**

#### やること

1. **選択からグループを作る**(WP349 の複数選択を使う)
2. **凸でない選択への対応** ——
   **黙って作らないこと。**
   **凸包を提案すること** ——「この選択は凸でない。`B` を含めれば凸になる」と示し、
   **利用者が受け入れられるようにすること。**
   凸包が現実的でない(巨大になる)場合は、その旨と件数を出すこと
3. **作ったグループは WP341 の機構に乗せる**(畳む / 中に入る / 辺の付け替え)
4. **解除できること**

#### 受け入れ条件

**本番経路を通すこと。**

- **凸な選択からグループが作れ、畳めること。**
  **畳んだ後、メンバの shape / label が 0 件(kind 非依存で検査)**
- **凸でない選択で、黙って作られないこと。**
  **凸包の提案が画面のテキストとして出ること**(data role だけでなく)
- **凸包を受け入れると凸なグループになること**
- **解除すると、作る前の item 集合に戻ること**(同じテストの中で前後を比較)
- **config に書き込まないこと** ——
  **作業木の config ファイルが 1 バイトも変わらないことを検査すること**
- **frame plan が更新されたとき、セッションのグループがどうなるかを決めて検査すること**
  (メンバが消えたら捨てる、など。**孤児を残さないこと**)

**変異を実際に入れて落ちることを確かめ、報告すること:**

- **凸性の検査を外す** → 非凸の選択がグループになって落ちること

**そのほか:**

- **作者が書いた region のグループ(WP343)と共存すること。**
  **両方在るときの優先順位を決めて明記すること**
- `SKIP_DEVSTUDIO=ON` でビルドが通ること
- `uv run tools/doclink.py check` が緑

#### やらないこと

- **engine を変更しないこと**
- **config に書き戻すこと**(上記のとおり経路が無い。別 WP)
- 入れ子のグループ
- **凸性判定を変えないこと**(総当たり 720,896 ケースで検証済み)

依存: WP349。見積: 中。
---

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
   - **(2026-08-03 追記)現在の統合ブランチは `codex/render-target-runtime-slice` である。**
     上の `codex/rendering-phase1-refactor` は当時の名前で、既に移っている
6. **(2026-08-03 追加)マージしたら worktree を消す。** WP ごとに worktree を切るなら、
   **統合した直後に `git worktree remove` まで行うこと**。ブランチは軽いので残ってもよいが、
   worktree は**フル Debug ビルドを抱えており数十 GB になる**。
   実例として 2026-08-03 に 19 個が溜まり、削除して **241 GB** が空いた
   (空き 190.7 → 431.6 GB)。規則 3 の「長生きブランチを作らない」と同じ理由だが、
   ブランチと違って**放置のコストが桁違いに大きい**ので明示する。

   手順は「squash マージ → 統合ブランチでビルド+テスト緑 → `git worktree remove`」。
   **緑を確認する前に消さないこと**(やり直しが要るときに作業が失われる)。

7. **(2026-08-05 追加)プロセスを名前で kill しない。実行パスで絞る。**
   並列で走る WP は同じ名前の実行ファイル(`pelican_player.exe` / `pelican_studio.exe` /
   `ctest`)を動かしている。名前だけで探して落とすと**他の WP のテストを巻き込む**。
   2026-08-05 に 2 回発生した(WP257 と WP270 が、それぞれ別 WP の `ctest` を誤終了)。
   幸いどちらも作業は失われなかったが、それは運である。

   自分が起動したプロセスは PID を控えて自分で終わらせること。既存プロセスを探す必要が
   あるなら、**実行パスまで確認してから**判断すること。

8. **(2026-08-03 追加)ビルドディレクトリの中にツールをインストールしない。**
   実行ファイルがビルド出力の中に入ると、プロセスがハンドルを保持したときに
   ディレクトリごと削除できなくなる。実例として WP254 のエージェントが
   `build/wp254-uv-install/` へ uv を入れ、その worktree だけ削除できずに残った。
   ツールが要るなら worktree の外か、OS の一時領域を使うこと。

9. **(2026-08-09 追加、2026-08-17 改訂)`#if`、CMake `if()`、generator
   expression、条件付き source / target / dependency のいずれかに属する変更を行う WP は、
   対応する全ての機能フラグについて対照構成を実際にビルドすること。**
   エンジン開発の既定は機能フラグが軒並み ON なので、OFF 構成のコンパイルエラーは
   手元でもテスト行列でも**表に出ない**。実例として WP281 が
   `gltf.cpp` の `vat_deformed` を `#if PELICAN_WITH_VAT` の外で代入し、
   `PELICAN_WITH_VAT=OFF` を丸ごとビルド不能にした。
   `pelican_cli dist-config` は VAT 資産の無いプロジェクトに OFF を出すため、
   該当する配布ビルドが全滅する状態だった。CI でも手元でも緑のまま。

   **WP の受け入れ条件に、触れた gate に対応する全ての機能フラグと、その対照構成を
   一つずつ明記すること。** 複数のフラグに触れたなら、一つだけを OFF にして済ませない。
   対象は §0「機能フラグの検証セット」の registry を正とする。対照が別フラグを
   強制的に動かす場合(Jolt provider 選択など)だけは、registry に宣言された連動を認める。
   文書だけに例外を書いてはならない。これは指示書を書く側の責任である。WP278 は
   `PELICAN_RUNTIME_SHADER_COMPILER` の両構成を明示的に要求して防げていたが、
   WP301〜303 は同時に触れた `PELICAN_WITH_OPENXR` を列挙せず、欠陥を見逃した。

   OFF 構成を新しいビルドディレクトリで構成するときに二つ詰まる。両方あらかじめ避けること。

   - **短いパスを使う。**`_deps/battery-embed-subbuild/...` が深く、
     OS の一時領域のような長いパスに置くと MSBuild が MAX_PATH で落ちる
     (`%USERPROFILE%/pvoff` 程度なら通る)。
   - **`-DPython3_EXECUTABLE=` を明示する。**`PELICAN_WITH_SPIRV_LINK=ON` が引く
     SPIRV-Tools は Python3 を要求するが、`WindowsApps` の stub は検出されない。
     既存 `build/CMakeCache.txt` の `Python3_EXECUTABLE` をそのまま渡すのが早い。

10. **(2026-08-13 追加)機能が効いていることの主張には、効いていない状態を同じテストの中で
    実行し、結果が異なることを主張すること。**

    **これが最も高くついた失敗の型である。**実例:

    - **WP283**: レイトレの影マスクが「全画素が 192 超」を主張した。
      対象の clear_color は `[1,1,1,1]` すなわち R8_UNORM で 255 であり、
      さらに固定シーンはレイが必ず外れる配置だった。
      **シェーダが 1 本もレイを飛ばさなくても緑になる**状態で出荷され、緑のまま検証を通過した。
    - **WP245 / WP275 の初期案**: 「rpc が送られたこと」を主張した。値が変わったことは見ていない。
    - **WP287 の smoke**: 「終了コード 0」を主張した。`animgraph_demo` は
      exit 0 で一様な単色を描いていた。

    いずれも**機能が存在しなくても満たせる主張**である。単体では見抜けない。
    見抜くのは対照であって、主張の強さではない。

    **エージェント側の義務**: 描画結果・状態・値に対する主張には、
    **同じ ctest ケースの中で**、機能を無効にした実行(オーバーレイを渡さない、
    feature を外す、選択を空にする)を伴わせ、**両者が異なることを主張すること。**
    レビューは「実行が 2 回あるか」を数えるだけで確認できる。

    **指示書を書く側の義務(これは Claude の責任である)**: 受け入れ条件に
    **「機能が無いときに観測されるはずのもの」を書くこと。**
    「N 画素以上が暗い」ではなく「遮蔽物なしで暗画素 0、遮蔽物ありで N 以上」。
    下限だけの条件は、飽和した結果と区別できない。

    **範囲**: 全テストへの遡及適用は求めない。**新規に書く主張に適用する。**
    既存テストは、その主張が載る WP に触れたときに揃えれば足りる。

    **(2026-08-19 追記)本規約をこれ以上広げないこと。**
    内容は「**機能を外すとそのテストが落ちること。変異 1 つで確かめる。**」で尽きている。
    「全 fixture × 全経路」「`static_assert` で構築不能性を示す」といった要求は、
    **その欠陥に固有の条件**であって一般規約ではない。**当該 WP の節に書くこと。**
    ここへ足すと、すべての WP がその WP の事情を払うことになる。

    ---

    **(2026-08-13 追記)対比は、部品の内側ではなく機能の入口で行うこと。**

    本規約を入れた直後の WP286 は、規約を**満たしたうえで壊れて出荷された**。
    エディタで `G` / `R` / `S` を押しても何も起きず、原因は
    `--editor-transform` を値なしで渡すと既定プリセット `grab` に解決され、
    その profile が `"bindings": []`(空)だったことである。
    アクションセットは読み込まれ、システムも走り、`enabled` は `true` を返し、
    **キーが 1 つも割り当てられていなかった。**

    WP286 のテストは確かに取り消しと確定を対比していた。
    **しかしコントローラを直接構築した後でである。**
    「起動したエディタがそのコントローラに到達するか」を誰も見ていなかった。
    緑だった 3 つのテストの内訳が、この規約の穴をそのまま示している。

    - 起動テストは**常に `--editor-transform blender` を明示**していた
    - studio のテストは argv に `"--editor-transform"` という**文字列があること**だけを主張した
    - 入力アクションのテストは**既定が `grab` であることを主張していた** ——
      すなわち**欠陥を仕様として固定していた**

    **要求**: 対比は、利用者が実際に通る入口で行うこと。
    「起動した製品にその機能が届いているか」を、
    **走っている側が解決した値**に対して主張すること。
    引数や設定が**存在すること**を主張しても意味がない
    ——上の例では、旗はずっと存在していた。

    studio や CLI が既定値に頼って機能を有効にする場合、
    **その既定値のまま**の経路をテストに含めること。
    明示指定だけを試すと、既定が壊れていても緑になる。

11. **(2026-08-19 追加)規約は影響範囲で段を分ける。連鎖に上限を置く。指摘は仕分ける。**

    **これは規約を足す規則ではなく、規約が増えるのを止める規則である。**

    2026-08-18〜19 に、利用者から「GUI を改善して」と言われてからの 19 コミットの内訳は
    **頼まれた機能 2 / 自分で見つけて自分で直した分 4 / 台帳 8 / その他 5** だった。
    そして**利用者が最初に挙げた要求(どのシェーダーで処理するかを指定する)は
    その日も達成されていない。**WP の連鎖は 321 → 324 → 326 → 328 と 4 段に伸び、
    **利用者は一度もそれを要求していない。**

    **(a) 敵対レビューは影響範囲で段を分ける。**

    | 影響範囲 | かけるもの |
    |---|---|
    | core の検証・`#if` を触る・出荷物の挙動が変わる | 設計 + 仕様 + コード |
    | `pelican_project` / 複数経路にまたがる | 仕様 + コード |
    | **studio 内で閉じる・読み取りのみ・表示だけ** | **コードのみ** |

    設計レビューは、**設計が 1 度でも不合格になった領域**か、
    **後から直すのが高い判断**にだけかける。
    レビュー自体は元を取っている(コードレビューは 5/5、仕様レビューは 3/3 で実欠陥を出した)。
    **削るのはレビューではなく、リスクに関係なく一律にかけることである。**

    **(b) 指摘を全件 WP にしない。**
    返ってきた指摘は「**直す / 台帳に書くだけ / やらない**」に仕分けてから起票する。
    WP326 と WP328 は、指摘を全件起票したから生まれた。
    「やらない」を選べないレビューは、レビューではなく作業生成器である。

    **(c) 連鎖に上限を置く。**
    **WP が WP を直していて、それがまた WP を直しているなら、
    起票する前に「元を revert して一度でやり直す」を検討すること。**
    WP324 → 326 → 328 はこれをしなかった結果である。

    **(d) 規約を足す前に、既存の規約を 1 つ狭められないか見ること。**
    §4 の規則は失敗のたびに増え、**2026-08-19 まで一度も減っていない。**
    増やす側の主張は常に具体的(実際に踏んだ)で、
    減らす側の主張は常に抽象的(遅い)なので、放置すると必ず増え続ける。

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

### WP351: ノードごとの GPU 実行時間を、グラフのノードの上に出す

**§4 規則 11 の中段(`pelican_project` / 複数経路)。仕様レビュー + コードレビュー。**
**engine を変更する。**

#### 目的

**利用者が求めたのは「パスにおけるノードごとの実行時間の表示」であり、**
**「あくまで数フレームの平均でいい」と明示されている。**
WP347 が同期(バリア)の辺を描いたので、**残るのは時間の側だけである。**

#### 先に測った事実(再調査不要。すべて本 WP 執筆時に読んで確かめた)

**1. `gpu_timing` はパスを 1 つも宣言しない。**

`src/core/resources/features/gpu_timing.json` の中身は
`schema` / `version` / `name` の 3 フィールドのみで、`passes` も `compute_tasks` も無い。

**したがって studio の overlay に足しても frame plan は変わらない ——
ノードも辺も増えない。** WP341/343/347/349/350 のグラフテストが動かない根拠はこれである。
**この前提が崩れたら(足したらノードが増えたら)、実装を進めず報告すること。**

**2. 有効化は起動時にしかできない。その理由まで辿ってある。**

```
src/core/vkcore/renderer.cpp:298
    isFeatureEnabled("gpu_timing") ? &GET_MODULE(RenderTiming) : nullptr
```

**renderer の構築時に、feature の有無でモジュールを作るか決めている。**
`src/core/vkcore/renderer_config.cpp:68-71` が `gpu_timing` の要求モジュールを
`RenderTiming` と宣言し、`runtimeModuleInitialized<RenderTiming>` で
**「既に初期化されているか」を見る。**
だから後から足すと `src/core/communication/renderconfigeditor.cpp:510` の
`restart_required_feature` で弾かれる。**これは正しい挙動であって、直す対象ではない。**

**3. studio の player の overlay は今 2 つしか持っていない。**

`src/core/resources/features/editor.json` は
`engine://features/gizmo.json` と `engine://features/picking.json` のみ。
**ここに `gpu_timing` を足すのが、起動時に有効化する唯一の素直な経路である。**

**4. 欲しい集計は既に内部に在る。ただし平均ではない。**

- `GpuTimingNodeRow`(`src/core/vkcore/rendertiming.hpp:65`)が
  view / node_ordinal / node_kind / node_name / `barriers_ms` / `body_ms` を持つ
- `latestNodeRows()`(同 `:180`)を imgui が
  `src/core/imgui/imguisystem.cpp:139-175` の表で既に描いている
- **しかしこれは `gpu_history.back()`、すなわち最新 1 フレームだけである**
  (`src/core/vkcore/rendertiming.cpp:438-481`)。
  `statusJson()` が出す `"nodes"` も同じ 1 フレームである

**利用者が言った「数フレームの平均」は、今どこにも存在しない。それを作るのが本 WP の中核である。**

**5. 履歴は 120 フレーム分ある。**

`gpu_timing_history_capacity = 120`(`src/core/vkcore/rendertiming.hpp:17`)。
**窓平均の材料は足りている。**

**6. studio は `get_status` を一度も呼んでいない。**

`get_status`(`src/core/communication/rpcserver.cpp:1143`)の呼び手は
`test/rpc_color_contract_test.cpp:217` と `tools/pelican_rpc.py:146` だけで、
**`src/devstudio/` に 1 件も無い。**

**したがって `get_status` に相乗りする理由が無い。**
`get_status` は `logical_frame_history` を 120 件、identity 文字列込みで積むので重い
(**過去の作業で 1.91 MiB/call と報告されているが、私は自分で測っていない。
実装時に自分で測って報告すること**)。
**表示のために周期的に叩く口としては不適である。**

#### やること

**1. 窓平均を作る(engine)**

`RenderTiming` に、**直近 N フレームにわたる per-node 平均**を持たせる。

- 鍵は既存の `NodeKey` と同じ `(view_index, node_ordinal, node_kind, node_name)`
  (`rendertiming.cpp:445-447` に既にある)
- **`barriers_ms` と `body_ms` を別々に平均すること。**WP347 が同期を辺に描いたので、
  **バリアの時間と本体の時間が分かれていることに意味がある**
- **N は固定値を 1 箇所に置くこと。**`gpu_timing_history_capacity` を使い切らず、
  **数フレーム(例えば 30)にすること** —— 利用者の要求は「数フレームの平均」である
- **履歴が N に満たない間は、実際に使ったフレーム数を一緒に返すこと。**
  「まだ 3 フレームしか無い」と「30 フレームの平均」を利用者が区別できること

**2. 軽い RPC を 1 本足す(engine)**

**`get_status` を変更しないこと。**新しい口を足す。

- 返すのは per-node の平均行の配列と、使ったフレーム数、対応状況(`supported` / `reason`)
- **`logical_frame_history` を含めないこと。**これが軽さの根拠である
- **`gpu_timing` が無効なときも、エラーではなく「無効である」と分かる形で返すこと。**
  studio が「時間が取れない理由」を出せること
- **応答の実測バイト数を報告すること**(数 KB に収まっていること)

**3. studio の overlay に `gpu_timing` を足す**

`src/core/resources/features/editor.json` に `engine://features/gpu_timing.json` を足す。

- **これは studio が起動する player にだけ効く。出荷プロジェクトの config は変えない**
- **パージ性を下げないこと** —— `gpu_timing` が出荷ビルドから purge できる状態を保つ。
  **overlay に足すことがそれを妨げないことを、根拠付きで報告すること**

**4. ノードの上に出す(studio)**

- WP347 の同期表示と**同じノードの上**に、平均の `body_ms` と `barriers_ms` を出す
- **名前で突き合わせること。**突き合わないノードがあれば、
  **黙って 0 を出さず「時間が取れていない」と分かる表示にすること**
- **周期は studio 側が決める。**フレームごとに引かないこと(平均なので不要)
- **無効なとき / 未対応の GPU のときに、理由が出ること**

#### 受け入れ条件

**否定対照を同じテストの中に置くこと(§4 規約 10)。**
**それぞれについて「何を取り消したら落ちるか」を書くこと。**

**平均であること:**

- **同じノードに、値の異なるフレームを複数投入し、
  平均が「最新フレームの値」とも「最初のフレームの値」とも異なることを検査すること。**
  最新 1 フレームを返す実装に差し替えたら落ちること
- **`barriers_ms` と `body_ms` が別々に平均されていること** ——
  片方だけ変化させ、もう片方が動かないこと
- **窓 N を超えた古いフレームが平均に入らないこと** ——
  N+1 フレーム目を入れたとき、1 フレーム目の寄与が消えること
- **履歴が N 未満のときに、返るフレーム数が実際の件数であること**

**軽さ:**

- **新 RPC の応答に `logical_frame_history` が含まれないこと**を構造として検査すること
- **`get_status` の応答が本 WP の前後で変わらないこと** ——
  既存の契約テストが通ること

**frame plan が変わらないこと(1 の前提):**

- **`editor.json` に `gpu_timing` を足す前と後で、
  frame plan のノード集合と辺集合が完全に一致することを、同じテストの中で比較すること。**
  **これが本 WP で最も重要な検査である。**足したらノードが増えるなら設計が崩れる

**無効なとき:**

- `gpu_timing` が無効な player に対して新 RPC を呼び、
  **エラーではなく「無効」と分かる応答が返ること**
- **studio がその理由を表示すること**(0 を出さないこと)

**そのほか:**

- **`SKIP_DEVSTUDIO=ON` でビルドが通ること**
- `uv run tools/doclink.py check` が緑
- **GPU 込みで 1 回全数を回すこと**

#### やらないこと

- **`get_status` の中身を変えること**
- **CPU 側の時間を出すこと**(本 WP は GPU タイムスタンプだけ)
- 出荷プロジェクトの config に `gpu_timing` を足すこと
- 時間の履歴グラフ / 折れ線の表示(平均の数値だけでよい)
- `for` / ループの表現(別 WP)

#### 次

**`for` の表現。**WP348 が「複数ノードのループ本体は今の出荷内容に存在しない」ことを
実測で示したので、**設計から作ることになる。**

依存: WP347。見積: 中。

#### 敵対監査(WP351 直後、6 本を独立に反証)の仕分け

**実装は正しい。落ちているのは全部テストである。**
6 主張のうち 2 本(frame plan 不変 / パージ性)は反証できず、
**frame plan 不変は監査側が実際に変異させて確かめた** ——
`gpu_timing.json` に gizmo 由来のパスを 1 つ足してリビルドしたら
`featurecompose_test.cpp` が **33 対 32** で落ち、戻したら通った。
**本 WP で一番効く検査が、推論ではなく実測で裏付けられた。**

残り 4 本が挙げた穴のうち、**実際に機能を殺したまま緑になる変異が 3 つ**ある。以下が WP351a。

### WP351a: 緑のまま機能を殺せる 3 つの穴を塞ぐ

**§4 規則 11 の中段。仕様レビュー + コードレビュー。**
**production コードは原則として変更しない。穴はテスト側にある。**

#### 穴 1: barriers が平均されていることを、何も検査していない

**全テストで `barriers_ms` が窓内の全フレームにわたって一定である。**

- `test/rendertiming_test.cpp:84-85` —— barriers は両フレームとも `4.0`。動くのは body だけ(`2.0`→`8.0`)
- `test/rendertiming_test.cpp:106-113` —— barriers は窓内 30 フレーム全部 `6.0`。
  外れ値 `6000.0` は index 0、すなわち**窓の外**にある
- `test/golden_harness.cpp` の GPU 側は件数しか見ていない(値の assert が無い)
- `test/run_rpc_headless.cmake` は**無効経路しか通らない**

**緑のまま殺せる変異(監査側が両方走らせて出力一致を確認済み):**
`src/core/vkcore/rendertiming.cpp:180` の
`aggregate.barriers_ms_total += node.barriers_ms;` を
`aggregate.barriers_ms_total = node.barriers_ms * (aggregate.row.sample_count + 1);`
にすると、除算後に**最新フレームの barriers がそのまま出る** ——
すなわち WP351 が無くそうとした「最新 1 フレーム」に barriers だけ戻る。**全テストが通る。**

**WP351 の受け入れ条件は「片方だけ変化させ、もう片方が動かないこと」と書いてある。
body 側しか変化させていない。**

**やること**: **窓内で barriers を変化させ、body を固定した対照を足すこと。**
両方向とも否定対照を置くこと(`!=` を上下の実値に対して)。

#### 穴 2: `get_gpu_timing` の有効経路と studio の配線が、どこも通っていない

`get_gpu_timing` が出てくるのは `rpcserver.cpp` / `frameplanwidget.cpp:750` /
`run_rpc_headless.cmake` / docs だけである。
そして **`run_rpc_headless.cmake` は `--feature-overlay` 無しで player を起動する**ので、
その新ブロックは **`enabled:false` の stub しか見ていない。**
有効側の唯一のテスト `golden_harness.cpp` は `nodeAverageJson()` を**プロセス内で**呼び、
**RPC を渡らない。**

**緑のまま殺せる変異が 3 つある:**

1. `src/core/communication/rpcserver.cpp:1299-1302` の三項を潰して
   常に `disabledGpuTimingNodeAverageStatusJson()` を返す ——
   **studio の全ノードが永久に `feature_not_enabled` になる**
2. `src/devstudio/view/frameplanwidget.cpp:412-413` の
   `QObject::connect(gpu_timing_poll, &QTimer::timeout, ...)` を消す ——
   timer は残り、名前も interval 1000 も残り、テストは緑。
   **実機では connect 時の 1 回だけ要求が出て、そのとき履歴は空。
   全ノードが `waiting for samples · 0/30f` のまま固まる**
3. `frameplanwidget.cpp:760-763` の `pending_gpu_timing_request` の振り分けを消す ——
   後続の `if (request_id != pending_request) return;` が全応答を捨て、
   **`waiting for RPC` のまま固まる**

**なぜ気づけないか**: テストが `receiveGpuTimingResult()` から直接注入していて、
`requestGpuTiming()` も id の振り分けも QTimer も**素通りしている。**

**さらに polling の assert が構造的に反転している。**
`test/devstudio_frameplan_graph_test.cpp:2661` は `REQUIRE_FALSE(poll->isActive())`。
既定構築の `EmbeddedViewport` は `rpcReady()` が false なので
`gpu_timing_poll->start()` が**そもそも走らない**。
**timer の設定を検査していて、timer が発火することを検査していない。**

**やること:**

- **有効経路を本番経路で通すこと。`test/run_gpu_timing_headless.cmake` に置く場所が既にある** ——
  そこは `:64` で `"features": ["engine://features/gpu_timing.json"]` を指定して
  player を起動し、RPC で叩いている。**そこに `get_gpu_timing` を足せば、
  実ハンドラを有効状態で通る**(`logical_frame_history` が無いことも、
  そこで初めて RPC の継ぎ目で検査できる)
- **studio 側は、timer の発火が実際に要求を出すことと、
  id による振り分けが応答を届けることを検査すること。**
  `receiveGpuTimingResult()` の注入だけで済ませないこと
- **反転した assert を直すこと** —— `rpcReady()` が true の状況を作るか、
  発火を直接観測するか。**「動いていないこと」を検査したまま残さないこと**

#### 穴 3: 「軽さ」を測っているが、閾値が無い

`test/run_rpc_headless.cmake:248-251` は `string(LENGTH ...)` を 2 回して
`file(WRITE .../wp351_rpc_response_sizes.json ...)` するだけで、**比較していない。**
**応答が 1 MiB になっても何も落ちない。**

**やること**: 閾値を置いて超えたら落とすこと。**数字の根拠を書くこと。**

#### そのほか(docs、機能ではない)

**`uv run tools/doclink.py check` は `#L<行>` しか見ず、散文も件数も読まない。**
だから以下が緑のまま通った。

- `docs/source-code-guide/07_tools_rpc_tests.md:380` は **46 メソッド**、`:382` は **(23)**。
  表は既にそれより多い
- `docs/source-code-guide/08_class_interface_index.md:293` は本 WP が `43`→`44` に書き換えたが、
  **07 の 46 とも食い違う**
- **数を仕様に書かない。コードから数えて、両文書を一致させること。**
  数え方(何を 1 メソッドと数えるか)を明記すること
- `docs/manual/10_tools.md:515` は overlay の中身を「gizmo + picking」と書いており**今は嘘**
- `docs/design_render_feature_modules.md:79-89` は `editor.json` を**逐語で複製**していて、
  `gpu_timing` の行が無い

#### 受け入れ条件

**穴 1〜3 のそれぞれについて、上に書いた変異を実際に当てて落ちることを確かめること。**
**「落ちるはず」ではなく、当てて、落ちたことを報告すること。**当てたら必ず戻すこと。

- **穴 2 の変異 3 つは、3 つとも別々に当てて、3 つとも落ちること**
- **既存の緑を緩めないこと。**特に `featurecompose_test.cpp` の frame plan 不変検査は
  本 WP 群で最も効く検査なので、触らないこと

#### やらないこと

- **production の挙動を変えること。**穴はテストにある
  (閾値の追加と、docs の訂正だけが例外)
- 除数を `sample_count` から `frame_count` に変えること ——
  **どちらが正しいかは別の判断であり、本 WP では決めない。
  ただし「窓内の一部フレームにしか居ないノード」を 1 件テストに足し、
  現在の意味を固定すること**
- `for` / ループの表現

依存: WP351。見積: 小〜中。

### WP351b: 平均の発行が毎フレーム履歴全体を組み直しており、24 倍遅い

**§4 規則 11 の中段。仕様レビュー + コードレビュー。**

**注意: 本節は 1 度書き直されている。最初の版に書いた診断は 3 つとも実験で潰れた。**
**下に残す「潰れた診断」を先に読むこと。同じ道を 2 度通らないために置いてある。**

#### 測った事実

**`gpu_timing` を有効にした player は 24.4 倍遅い。**
テストと同じ 600 フレーム / 120fps / 320x180、同じ project、overlay だけを変えた対照:

```
gizmo + picking のみ          13.9 秒
gizmo + picking + gpu_timing   339.5 秒
テストのガード(test:389)      30 秒
理論値(600 / 120fps)          5 秒
```

**5 秒で終わるはずの実行が 5 分 40 秒かかる。**

**原因は毎フレームの発行にある。**
`collectGpuResults()`(`src/core/vkcore/rendertiming.cpp:445-452`)が毎フレーム呼ばれ、
`changed` なら `publishSnapshot()` を呼ぶ。
`publishSnapshot()` は **120 フレーム分の履歴(`gpu_timing_history_capacity`)を丸ごと走査し、
sample ごとに `sampleJson()` で 11 キーの JSON を作り直す。**
例のプロジェクトは ~20 パス + overlay で、1 フレームあたり 5〜6 千個の JSON を組み直す計算になる。

**これは WP351 が作ったものではない。`23012e8^` の `publishSnapshot()` を読んで確認した ——
履歴全体の走査も `sampleJson` も毎フレーム発行も、すべて WP351 以前から存在する。**
**WP351 は平均の走査を上に足したので悪化はさせている。内訳は測っていない。**

**WP351 がしたのは、studio の player で `gpu_timing` を初めて有効にしたことである。
それまで誰もこの経路を走らせていなかった。**

#### なぜテストが落ちるか(連鎖は全リンク確認済み)

```
600 フレームが 30 秒ガードを超える
  → stopped_loop.exec() がタイムアウトで戻る。normal.stopped は false、子は生きたまま
  → ブロックのスコープ終了
      ├ QEventLoop 3 つが先に破棄される
      │   (test:363 の viewport より後、test:368-370 で宣言されているため)
      └ ~EmbeddedViewport(embeddedviewport.cpp:355)
           ├ process_.isRunning() が true なので早期 return しない
           ├ waitForFinished() がイベントを回す
           ├ finished ハンドラが emit engineRpcBecameUnavailable(embeddedviewport.cpp:281)
           │   ※ shutting_down_ は :288 の再起動しか守っていない。emit は無条件
           └ observeViewport(test:205-) の 3 引数 connect(コンテキスト無し)が生きている
                → 破棄済み QEventLoop の quit() を呼ぶ
```

**`gpu_timing` が無効なら 13.9 秒で終わるので、`~EmbeddedViewport` の時点で子は既に死んでおり、
`if (!process_.isRunning()) return;` が**イベントを一度も回さずに**帰る。だから落ちない。**

**落ちているのは致命エラーのブロックではない。**
`TEST_CASE`(:268)から :386 までの `REQUIRE` はちょうど 17 個で「17 通過」と一致する。
致命エラーのブロックは :331-354 で完走して通っている。
**Catch2 が前のブロックの INFO を一緒に表示するので、致命エラーの文面に見える。私は 4 回これに誘導された。**

#### やること

**1. 発行を軽くする(engine、本体)**

**毎フレーム履歴全体を JSON に組み直すのをやめること。**

- **誰も毎フレーム消費していない。**imgui は `latestNodeRows()`(構造体)を読み、
  JSON は RPC が訊いたときだけ要る
- **求められたときに作る形にするのが素直である。**ただし方法は指定しない。
  **「毎フレームの費用が履歴の長さに比例しない」ことを満たせばよい**
- **`gpu_timing` 有効時の 600 フレームが、無効時に対して 1.5 倍以内に収まること。**
  現在 24.4 倍。**この数字を測って報告すること**

**2. 破棄中に signal を出さない(studio)**

`embeddedviewport.cpp:281` 付近の emit は `shutting_down_` を見ていない。
**破棄中は出さないこと。**受け手にとって意味のある通知ではなく、
受け手自身が既に畳まれている可能性がある。

**3. テストの connect を直す(test)**

`observeViewport`(`test/devstudio_engine_failure_test.cpp:205` 付近)は
**コンテキスト無しの 3 引数 `connect` で、自分より短命なローカルを参照捕捉している。**
**コンテキストオブジェクトを与えるか、寿命の順序を直すこと。**

**1 だけでは不十分である。**子はいつでも遅くなりうるし、ハングもする。
**2 と 3 は「子が生きたままスコープを抜けた」ときに落ちないことを保証する。**

#### 受け入れ条件

- **600 フレームの所要を、`gpu_timing` 有効/無効の両方で測って報告すること。**
  **1.5 倍以内**
- **毎フレームの発行費用が履歴の長さに比例しないこと**を検査すること。
  **`gpu_timing_history_capacity` を変えても毎フレームの仕事が変わらないことを、
  テストで固定すること**(実測でも構造でもよいが、取り消したら落ちること)
- **`devstudio_engine_failure_test` が通ること**
- **2 と 3 について、「子が生きたままスコープを抜ける」状況を直接組み立てたテストを書き、
  修正前のコードで落ちることを確かめること。**
  遅さに依存させないこと(遅さは 1 で消えるため)
- **RPC の応答内容が変わらないこと** —— WP351/WP351a の検査が通ること
- **全数 2 回**
- `uv run tools/doclink.py check` が緑

#### やらないこと

- **`editor.json` から `gpu_timing` を外すこと。**外せば通るのは測定済みであり、修正ではない
- 窓 N(30)や polling 周期を変えること
- `gpu_timing_history_capacity` を小さくして誤魔化すこと ——
  **履歴の長さは要件であって、費用の言い訳ではない**

#### 潰れた診断(最初の版に書いたもの。同じ道を通らないため)

**4 つとも私が書き、4 つとも実験が潰した。**

| 診断 | どう潰れたか |
|---|---|
| studio が中身のある応答を扱う経路が悪い | studio が要求を一切出さなくても落ちる |
| player 側の `RenderTiming` のコードが悪い | 場面を取り違えていた。壊れた設定では構築すらされない |
| 子が異常な死に方をする | exit も破棄ログも完全に一致 |
| `gpu_timing` が compile を 2 倍にする | 3 回ずつ反復したら誤差の範囲。初回実行の外れ値だった |

**共通しているのは、1 標本・1 実験で結論を書いたことである。**
**とくに 4 つ目は、反復していなければ「マーカー機能が compile を倍にする」という
もっともらしい嘘を仕様に書いていた。**

**測定の手順についても 3 回失敗した**(`LNK1104` ×2、`LNK1168` ×1)。
いずれも止めたバックグラウンド実行の子プロセスが exe を掴んでおり、
**リンクが失敗しているのに気づかず古いバイナリの結果を読んだ。**
**1 回目の切り分け実験はそれで逆の結論を出した。**
**ビルドの終了コードを見てからしかテストを回さないこと。**

依存: WP351、WP351a。見積: 中。

### WP352: 出力添付が自分の load/store を持つ

**§4 規則 11 の中段(`pelican_project` / 複数経路)。仕様レビュー + コードレビュー。**

#### 目的

**入力側は名前付きソケットへ移り終わっている。出力側が取り残されている。**
添付ごとの状態を添付に持たせ、**5 枚の添付が 1 個の load op を共有する状態をやめる。**

#### 先に測った事実(すべて本 WP 執筆時に自分で数えた。再調査不要)

**1. 入力側は既に移行済み。出力側だけが残っている。**

出荷 JSON 全数での出現回数:

```
resource_ports      19  ┐ 名前付き。ソケットの形
material_resources   8  ┘
input_sampling       2    位置対応の配列。ほぼ死んでいる
color_load_op       15  ┐
depth_store_op      10  │ パス全体に 1 個ずつしか書けない
depth_load_op        5  │
color_store_op       3  ┘
clear_colors         0    ターゲットごとに書ける。誰も使っていない
clear_color         27    パス全体に 1 個
```

**入力は 27 対 2 で名前付きが勝っている。出力の load/store は 33 箇所すべてパス全体に 1 個。**

**2. 添付が受けるキーは 2 つだけ。**

`renderingpasstargetjsonparser.cpp:46-55` の allowlist は `{target, subresource}`。
**添付ごとの load/store を書く場所が無い。**

**3. 色出力が 5 枚のパスが 6 件ある。**

```
gbuffer_pass ×4(example_renderingpass_data / main_rendering_config / main ×2)
cube_capture_geometry
planar_reflection_geometry
```

**5 枚が 1 個の `color_load_op` を共有している。**

**4. しかも読み取り集合が粗くなる。**

`frameplanner.cpp:1269`(および `:860`)は
`pass.color_load_op == eLoad` のとき**全色出力を `reads` に足し、`same_pixel` を付ける。**
**1 枚だけ load したくても 5 枚全部が「読む」ことになる。**
本 WP は**単なる整理ではなく、依存が変わる。**

**5. 出荷は動かない。**

**上記 6 件はすべて既定の `Clear` である。** 精密化が効くのは `Load` のときだけなので、
**出荷の plan は変わらないはずである。**
**変わったら前提が崩れているので、実装を進めず報告すること。**

#### やること

**1. 添付が `load_op` / `store_op` を受ける**

`{target, subresource}` の allowlist に `load_op` と `store_op` を足す。
**値の綴りはパス側と同じもの**(`renderingpassjsonhelpers.cpp:250-271` の受理形)。
色・深度の両方。

**2. パス側は既定として残す**

`color_load_op` / `color_store_op` / `depth_load_op` / `depth_store_op` は**消さない。**
**添付が書いていればそれ、書いていなければパス側、それも無ければ従来の既定。**
**3 段の優先順位を 1 箇所に閉じ込めること。**

**3. 読み取りの推論を添付ごとにする**

`frameplanner.cpp:1269` と `:860` の
「`color_load_op == eLoad` なら全色出力を読む」を、
**実効 load op が `Load` の添付だけを読む**に変える。深度も同様。

#### 受け入れ条件

**否定対照を同じテストの中に置くこと(§4 規約 10)。**

**依存が変わること(本 WP の中核):**

- **色出力 5 枚のパスを組み立て、1 枚だけ `load_op: "Load"` にする。**
  **その 1 枚だけが `reads` に現れ、残り 4 枚が現れないことを検査する**
- **同じテストの中で、パス側 `color_load_op: "Load"` の版も実行し、
  そちらでは 5 枚すべてが `reads` に現れることを比較すること。**
  **「1 枚だけ現れる」だけでは、従来の挙動と比べていない**
- **footprint も添付ごとに付くこと** —— 読まれない 4 枚に `same_pixel` が付かないこと

**優先順位:**

- 添付 > パス > 既定 の 3 段を、**3 通りとも**検査すること
- **添付とパスで違う値を書いたとき、添付が勝つこと**

**出荷が動かないこと:**

- **本 WP の前後で、出荷 4 プロジェクトの frame plan のノード集合・辺集合・
  `reads` / `writes` が完全に一致することを、同じテストの中で比較すること**
- **一致しなければ実装を進めず報告すること**(事実 5 が崩れている)

**そのほか:**

- **既存の綴り(`Clear`/`clear`、`DontCare`/`dont_care`/`dontCare` など)が
  添付側でも同じに受理されること**
- **添付に未知のキーを書いたら従来どおり投げること**(allowlist を緩めない)
- `uv run tools/doclink.py check` が緑
- **全数 2 回**

#### やらないこと

- **入力側に手を出すこと。**既に移行済みである
- **`clear_colors` を消すこと。**使用 0 件だが、ターゲットごとの clear は既にそれが担う。
  **添付に `clear` を足さないこと** —— 同じことを 2 通りで書けるようにしない
- `input_sampling` を消すこと(別 WP)
- ノード種別を増やす / 分割すること(上の階層は別 WP)
- `resource_ports` の文法を変えること

#### 次

**特化ノードの層。**辺の情報がソケットに載ってから、
**小さいノードに割って下へ展開する形**を設計する。
WP341/343/350 の「畳む・中に入る」がその表示にそのまま使える。

依存: 無し。見積: 中。

### WP353: fullscreen パスがブレンド状態を書けるようにする

**§4 規則 11 の上段(出荷物の挙動が変わる)。**
**本節は codex の仕様レビューで 1 度不合格になり、書き直されている。**
**10 件中 4 件が「仕様が破綻」判定だった。末尾に何が壊れたかを残す。**

#### 位置づけ(初版の誤りを訂正する)

**初版は「設計が予定していた一般化の実施」と書いた。それは誤りである。**

`design_shader_freedom_kit.md:139` の §4.4 が指すのは `PipelineFactory` の一般化で、
**それは既に実施済みである** —— `src/core/renderer/uirenderer.cpp:19` と
`src/core/material/materialcontainer.cpp:793` が既に blend と vertex layout を
PipelineFactory 経由で設定している。

**足りないのは内部ではなく、著作の口である。本 WP は新しい authored JSON capability であり、
繰り越された内部作業ではない。**

**そして公開契約の変更を伴う。**`docs/manual/06_rendering.md:97` は
「`draw` / `raster_state` は raster 限定、他 type はエラー」と明記している。
**その更新は本 WP の範囲に含める。**

#### 範囲を blend に限る。深度は外す

**初版は深度を「WP の判断対象」として残した。それは仕様の破綻だった** ——
受け入れ条件に深度の検査が 1 件も無く、
「所有権を与え、blend だけ繋ぎ、深度は外したと報告する」で全条件を通せた。

**深度を外す理由は実測である。**
`engine://fullscreen` は `gl_Position.z = 0.0` を出す(`fullscreen.vert:9`)。
sky が残したいのは `scene_depth == 1.0` の背景なので、
**`depth_compare: equal` は 0.0 に一致する画素を選び、背景は描かれない。**
直すには far-plane の `z=1` を出す頂点シェーダが要る。
**共有頂点シェーダを変えると 35 本すべての pipeline identity が変わる。**
これは深度とは別の設計判断であり、本 WP では決めない。

**したがって: `fullscreen` / `output_transform` で `raster_state.depth_*` を書いたら、
名前付きエラーで拒否すること。**黙って無視しないこと。

#### `output_transform` を明示的に対象へ含める

**`fullscreen_fields` は `output_transform` と共有されている**(`passfieldownership.cpp:57`)。
`output_transform` は**全グラフに自動追加され**(`featurecompose.cpp:432`)、
実行時は同じ `FullscreenPassInfo` variant に落ちて
`isFullscreen()` でも区別できない(`renderingpass.hpp:466`)。

**したがって影響範囲は「著作された 35 本」ではない。自動生成される `output_transform` も含む。**
**両方を不変性の対象集合に入れること。**

#### やること

**1. 固定機能状態のパーサを公開し、共有する**

**現状 `parseState` は anonymous namespace の private で、公開されている
`parseRasterPassContract` は `draw` を必須とする**(`rasterpass.cpp:407`)。
**fullscreen は `draw` を所有できない。**

**したがってパーサをコピーするか、隠れた dummy `draw` を注入するかしかない。
どちらも禁止する(第二の流儀・隠れた既定値)。**

**`pelican_project` に公開の固定機能状態パーサを切り出し、
generic raster と fullscreen が同じものを通ること。**
**Vulkan lowering は既存 adapter を使い、完成した記述を渡すこと。**
**`FullscreenPassContainer` に JSON / portable state の解釈を足さないこと** ——
その契約(`fullscreenpasscontainer.hpp:83`)と衝突する。

**2. blend を物理添付へ正しく対応付ける**

**`color_attachments[i]` は論理添付の添字だが、pipeline の配列は
scope 全体の物理添付の添字である**(`renderingpass.hpp:718`、
`renderingpassruntimecompiler.cpp:1340` が scope-wide union を返す)。
**恒等でない対応が実在する**(`renderingpass_helpers_test.cpp:711` に `{1, unused, 0}`)。

**fullscreen も `applyVulkanRasterPassContract(..., rendering.color_attachment_locations)` を
必須経路にすること。**

**3. 整数 RT への blend を早期に拒否する**

`validateGenericRasterColorState`(`renderingpassruntimecompiler.cpp:2140`)は
generic raster 登録時しか呼ばれない。
**`R32_UINT` の RT は同梱の picking target に実在する**(`features/picking.json:7`)。
**fullscreen で `"blend":"additive"` を書くと Vulkan pipeline 作成まで遅延して失敗しうる。
fail-fast ではない。**

**numeric-class 検証を両者共通にし、shader 登録より前に名前付きエラーを出すこと。**

**4. manual を更新する**

`docs/manual/06_rendering.md:97` の所有表、fullscreen 節、`raster_state` 節、
`output_transform` の扱い。

#### 受け入れ条件

**既定が動かないこと(比較対象の定義が初版の破綻点だった):**

初版は「pipeline 記述が完全一致」と書いたが、**それは実装不可能だった**:

- 現行 fullscreen は `color_attachment_states` を**空のまま**生成する
  (`fullscreenpasscontainer.cpp:200`)。
  raster パーサは省略時も**色出力数だけ既定を生成する**(`rasterpass.cpp:225`)。
  **同じ経路に繋ぐと `[] → [opaque/RGBA]` になり、Vulkan の値が同じでも記述は一致しない**
- `GraphicsPipelineDesc` は `ShaderBundleId` を含み、**handle は登録順で採番され解放後に再利用される**
  (`resourcecontainer.hpp:13`)。決定的な比較キーにならない
- **「WP 前後を同じテストで比較」には旧側の oracle が無い。**
  新しいバイナリは旧コードを実行できない

**したがって次のように定義し直す:**

- **`raster_state` の著作有無を `optional` として保持し、
  省略時は従来の記述に一切触れないこと**(空 vector のまま)
- **既定値の権威を 1 箇所に置くこと。**
  空 vector は「未指定の表現」であって第二の既定値にしない
- **比較対象は canonical semantic descriptor** ——
  安定したシェーダ参照(または content hash)と固定機能の値。
  **handle・登録順・「空 vector と展開済み既定」の表現差を比較対象にしないこと**
- **同じテストの中で「未記述」と「明示的な非既定」を本番経路でコンパイルし、
  前者の実値が従来どおりであることと、後者が変化することを両方検査すること**

**対象集合を機械列挙すること:**

- **35 本の manifest をファイル名と pass 名で機械列挙し、件数も assertion にすること。**
  `projects/example/passes/example_renderingpass_data.json:68` の 1 本は
  起動 envelope が選ばない(`project.json:30` が `main_rendering_config.json` を選ぶ)ので、
  **「4 プロジェクトを起動する」実装では通らない**
- **自動生成される `output_transform` を別集合として加えること**
- **最低でも flat / XR sequential / XR multiview、
  `PELICAN_RUNTIME_SHADER_COMPILER` の ON/OFF で期待結果を明記すること。**
  OFF で生成不能なものは「同じ記述」ではなく**同じ名前付き失敗**を比較対象にする

**blend が実際に効くこと(記述検査だけでは足りない):**

**`PipelineFactory::graphicsDesc()` が返すのは入力された記述だけで、
生成された `vk::PipelineColorBlendAttachmentState` を読み戻す API は無い**
(`pipelinefactory.cpp:1110`、helper は `:242` で cpp 内部)。
**したがって記述検査だけなら、pipeline 作成側で `blendEnable=false` を固定しても通る。**

- **統合側の GPU テストで、既知の行き先を `Load` し、alpha 0.5 の既知の source を重ねること。**
  **同じテストで blend 有り / 無しを描き、画素差と解決された係数の両方を検査すること**
- **`"additive"` preset の color source factor は `One` ではなく `SrcAlpha` である**
  (`materialoutput.cpp:122`)。
  **`color0 + color1 * intensity` と一般に同値なのは明示的な `One/One` であり、
  source alpha が 1 のときだけ preset と一致する。**
  **両方を別々に検査すること**

**全 field を受けるなら全 field を検査すること:**

- **`topology` / `cull` / `front_face` / `write_mask` を parse して pipeline で無視する実装が
  通らないこと。**受ける field はすべて生成物に反映されることを検査する
- **恒等でない `color_attachment_locations` を持つ本番 config で、
  対象 slot と未使用 slot(write-mask 0)の両方を検査すること**

**所有表:**

- **`fullscreen` と `output_transform` の両方で `raster_state` が投げなくなること**、かつ
  **`ui` など非所有型では従来どおり投げること**を同じテストで両方実行する
- **`push_constants` / `uses_light_data` と `raster_state` を同時に書けること**
- **`depth_*` を書いたら名前付きエラーで拒否されること**

**そのほか:**

- `uv run tools/doclink.py check` が緑
- **`SKIP_DEVSTUDIO=ON` でもビルドが通ること**
- **GPU テストはエージェントに走らせない**(統合側で直列に回す)

#### やらないこと

- **既定値を変えること**
- **深度状態**(理由は上記。頂点シェーダの深度規約を決める別 WP)
- **`bloom_composite.frag` / `sky_ambient` を書き換えること**(回避策の解消は別 WP)
- **共有頂点シェーダを変えること**
- 特化ノードを作ること
- `material` / `shadow_depth` のパイプライン状態

#### 仕様レビューで壊れた 4 件(記録)

**同じ道を 2 度通らないために残す。**

| 初版に書いたこと | どう壊れたか |
|---|---|
| 「設計が予定していた一般化」 | §4.4 は `PipelineFactory` の話で**実施済み**。これは新しい著作機能 |
| 深度を「判断対象」として残した | 深度の受け入れ条件が 1 件も無く、**外しても全条件を通せた** |
| sky は深度 equal で解ける | `fullscreen.vert:9` が `z=0.0`。**equal は背景に一致しない** |
| 「pipeline 記述が完全一致」 | 空 vector と展開済み既定は**同値でも一致しない**。handle は再利用される。**旧側 oracle が無い** |

**codex が壊せなかった項目**(独立に確認された):
35 本すべてが明示 `depth: null`、
`RasterFixedFunctionState` と `GraphicsPipelineDesc` の既定値が現時点で一致、
`raster` への逃げ道が実際に塞がっている、
深度の backend 配線自体は既に存在する(不可能ではなく仕様が無いだけ)、
D0 は正しく分ければ壊れない。

依存: 無し。見積: 中。

### 設計ノート: レンダリングパスノードの発展計画(2026-08-28)

**WP ではない。会話にしか無かった合意と設計判断を台帳に固定する。**

#### 3 段計画(利用者と合意済み)

```
1  ソケット整理        入力側は移行済み、出力側は WP352 で完了。blend の著作化が WP353(実行中)
2  特化ノードの層      小さい・縛りのあるノードを、実測から起こす。下は既存ノードへ展開
3  型を開く            利用者が自分のノード種別を定義できる
```

#### 中心の設計判断: 「利用者定義ノード」の実体は feature である

**`type` の enum を開く必要はおそらく無い。**根拠:

- feature は既に「パラメータ付きで再利用できるパス束」である
  (`parameters` に render_target / scalar / shader_asset の 3 種、束縛時に
  role / format_class / usage を**制約として検査**する —— つまり**型付きソケットが既にある**)
- WP341/343 のグループ表示は provider_feature / region 単位で畳める ——
  **「論理ノードがコンパイル後のノード群のグループノードになる」という利用者の解釈そのもの**
- 出荷で最も使われる非汎用型 `snapshot_copy`(6 件)は、
  「縛りのある特化ノード」が実際に機能している証拠である
- 逆に、authored 1 回きりの型が 8 つある(velocity / ui / shadow_depth / raster /
  picking / gizmo / debug_text / debug_draw)。**型を増やす方式はスケールしなかった**

**したがって: 特化ノード = エンジンが出荷する parameterized feature。
利用者定義ノード = 利用者が書く parameterized feature。同じ機構である。**

足りないのは機構ではなく**表現力**(下記の原始要素)と**編集面**
(畳んだグループの上でパラメータを編集するソケット UI)。

#### 特化ノードの候補(実測順。畳める出荷宣言数つき)

| 候補 | 畳める数 | 必要な原始要素 | 状態 |
|---|---|---|---|
| `composite`(blend 合成) | 4 | WP353 のみ | **WP353 直後に可能** |
| `view_family_instance` | ~44 | 名前空間の鋳造 | 大きいが**リソース名が変わる** |
| `separable_blur` / `blur_pyramid` | 11-13 | ループ + 大きさ導出 | for 設計待ち |
| `mip_chain`(prefilter 鎖) | 6 | 同上 | 同上 |
| `background`(深度ゲート) | 1 | 深度規約の決定 | 頂点 z=0 問題(WP353 節) |
| `temporal_resolve`(TAA) | 2 | @history + jitter(既存) | 薄い包みで可能 |
| `clear` | 1 | 無し | 自明 |

**投機的(出荷に証拠が無い。作らない、記録だけ):**
HiZ ピラミッド、輝度リダクション(自動露出)、GPU カリング → indirect draw、FFT。
いずれもループ + 導出 + compute の組み合わせで、blur_pyramid と同じ原始要素に載る。

#### 必要な原始要素(解放する範囲の順)

```
1  ループ + index      鋳造される名前、seed/前段の入力選択(bloom で実測した非対称)、
                       降順(upsample)、反復ごとの大きさ
                       → blur_pyramid / mip_chain / FFT。feature を真の利用者ノードにする
2  大きさの導出        「入力の 1/2」— extent_scale はフレーム基準しかない
                       → ループの前提。単独でも半解像度 SSAO 等に効く
3  blend / 深度の著作  WP353(blend)+ 深度規約の別 WP(fullscreen.vert が z=0 を書く問題)
4  名前空間の鋳造      view_family_instance 用。main と variant で命名規則が違う
                       (ssao_output vs cube_capture_ao)ことが実測済みなので、置換ではなく鋳造
5  パラメータ演算      最小限に絞ること。大きさの導出以外に広げない
                       (完全一致置換のままが安全。一般式は第二のシェーダ言語になる)
```

#### わかりやすさの定義(利用者の要望「不慣れな人に大変」への答え)

**bloom を書く作業の前後:**

```
今    ターゲット 9 宣言 + パス 13 宣言(うち 11 が同型の書き写し)、28 キーの表と格闘
後    ノード 1 個: { source, levels: 4, intensity } + 特化ノードが残りを固定
```

**縛りは制約ではなく案内である** —— 関係の無いキーが最初から存在しないこと。

#### 保留中の紐付け

- **wp336(`31f0971`、<作業ディレクトリ>/wp336)は「シェーダが読み範囲を宣言する」実装で、
  WP345 の問題圏に属する。WP345 の書き直しと同時に採否を決める。単独で破棄しない**
- WP352/WP351b への遡及コードレビュー(§10)は WP353 のレビューと同じ回で依頼する

### 設計ノート: 最適化ループの軸(2026-08-28、利用者ビジョンの分解)

**利用者の要望(原文の趣旨):**
Blender のように使い道のわかりやすいノードの組み合わせでパスを組みたい。
複数ノードを選んでコンパイル結果のグループノードを開き、中を編集し、
**凍結して再コンパイルから除外し、自動測定**したい。
最適化後のノードを**パラメータを一般化して別プロジェクトへインポート**できる使用感にしたい。

#### 対応する機構の棚卸し(本ノート執筆時に設計文書と実装を確認)

| ビジョンの段階 | 機構 | 状態 |
|---|---|---|
| わかりやすいノードで組む | 特化ノード = parameterized feature(前ノート) | WP353 実行中 |
| 複数選択 → グループを開く | WP341/343/349/350 | **済** |
| コンパイル結果を編集 | **RPE12b `ejectable_physical_fragment`(実装済 2026-07-26)** | エンジン側 済 |
| 凍結(再コンパイルから固定) | 同上 + `vulkan_plan_pins`(RPE12a) | エンジン側 済 |
| 自動測定 | WP351 `get_gpu_timing` + `applied_physical_fragment` の再観測 | 部品 済 |
| 一般化して別プロジェクトへ | **無い。本ビジョンの本当に新しい部分** | 未設計 |

**RPE12b が既に持っているもの**(`design_render_graph_compiler.md:1714-`):
Plan Viewer / RPC から physical plan を eject でき、sparse resource override /
scope 分割 / alias 群 / `(node, logical_resource)` ごとの load/store を編集して
config の `vulkan_physical_fragments` に貼って戻せる。
**指紋(logical graph + automatic plan + device facts + provider generation)が
合わなければ stale として拒否し、黙って fallback しない。**

#### 設計上の緊張 3 つ(正直に書いておく)

**1. 「編集」は依存安全な範囲に限られる(これは正しい制約)。**
RPE12b の scope 編集 mode は `split_only | dependency_safe`。
**依存を変える編集は物理層では拒否される。**並べ替えやシェーダ差し替えのような
自由編集は論理層(feature)の仕事であり、物理層の編集は検証器が確かめられる
つまみに限る。利用者の「ノードの編集」はこの二層に振り分けて提示する。

**2. 「凍結」は再コンパイルを省かない。結果を固定する。**
fragment は automatic plan と照合されるので、コンパイル自体は走る。
**保証されるのは決定性(黙って別 plan に落ちない)であって、コンパイル時間の節約ではない。**
時間の節約が要るならそれは別の話(plan cache)で、混ぜないこと。

**3. 凍結物は設計上、可搬ではない。**
fragment の妥当性は device facts / provider generation に指紋で束縛される。
**別プロジェクト・別マシンでは stale になるのが正しい挙動。**
したがって「エクスポートできる最適化済みノード」の実体は
fragment そのものではなく、**論理 feature + 論理名で表現された最適化の意図**
(scope 分割の指示、alias の指示、load/store の上書き —— fragment v2+ は既に
`(node, logical_resource)` の論理名で持っている)でなければならない。
インポート先で再検証し、適用できなければ**名前付きで**報告する。
[[user-space-boundary-lesson]] のとおり、発展する側を宣言データとして開く。

#### 残る新規設計は 3 つ

```
G1  studio の編集面      eject → 編集 → 貼り戻し。エンジンは済、UI が無い
G2  A/B 自動測定         fragment 有/無で N フレームずつ回し per-node 時間を比較。
                         部品(WP351 / config apply / provenance)は全部ある。編成が無い
G3  一般化とエクスポート  凍結の意図を論理名の宣言に抽出し、feature 化して持ち出す。
                         具体名 → $param の抽出支援。真に新しいのはここだけ
```

**順序は既存計画を変えない**: 組み合わせの軸(WP353 → composite → for)が先。
最適化ループの軸は G1 → G2 → G3 の順で、組み合わせの軸が使える状態になってから。

#### 追記: 可読性の軸(G0)—— 同日、利用者指摘

**「コンパイル後の結果ノードの可読性」も課題である。**
最適化ループの前提に置く(**読めないものは自信を持って編集も凍結もできない**)ので、
G1 の前に G0 として立てる。

**「読めない」の正体は、本日までの調査で実測済みのものだけで 5 種類ある:**

| 罠 | 実例(測定済み) |
|---|---|
| **書いたものと走るものが違う** | bloom の 9 ターゲット全部の `format: B8G8R8A8_UNORM` は**死んでいる** —— `format_class: "scene"` が `rendertargetjsonparser.cpp:216-222` で上書きする。config を読んだ人は誤った事実を学ぶ |
| **効かないフィールドが生きて見える** | 合成 4 パスの `color_load_op: "Load"` の隣の `clear_color` は決して効かない(WP353 節) |
| **自動挿入と著作の区別が無い** | `output_transform` は全グラフに自動追加。`__snapshot_*` も自動。editor overlay の gizmo/picking も。タグ(`legacy.<kind>` / provider_feature)は既にあるが、**表示上は著作ノードと同格** |
| **展開の重複がそのまま出る** | WP348 実測: 1 論理ノードが 48 invocation(8 ブロック × 6 view)。辺の `x N` 畳みは WP341 にあるが、**invocation の畳みは無い** |
| **名前が中身と逆** | bloom の `Downsample_H_*` ターゲットは**upsample の蓄積先**として使われている(調査で判明)。命名は著作の問題だが、コンパイル結果まで伝播する |

**「読める」の定義(設計原則として):**

1. **すべてのコンパイル済みの事実が出所を持つ** ——
   著作(どの file / feature / パラメータ)か、導出(どの規則)か、上書き(何が上書きしたか)か
2. **表示する値は実効値。著作値と違うときは両方見せる**(format の罠への答え)
3. **自動挿入されたノードは見た目で区別される**
4. **多重度は件数付きで畳む**(辺の `x N` と同じ流儀を invocation にも)
5. **双方向リンク** —— コンパイル結果のノードから、それを生んだ著作宣言へ飛べる。逆も

**安いものから**: 3(タグは既にある。表示だけ)と 2(実効値の表示)は studio 内・表示のみ
= §0 の下段(コードレビューのみ)で薄く出せる。1 の完全形(全フィールドの出所)は
engine が出所を frame plan に載せる必要があり、中段になる。

**順序**: 組み合わせの軸を止めない。bloom 書き換えの後、G1 の前に G0 の安い 2 件を置く。

#### 追記: シェーダとノードの接続(同日、利用者指摘)

**要望**: ノードにシェーダをリンクし、ノード GUI からシェーダへ飛べ、
新規ノードからヘッダ付きシェーダファイルを生成でき、
未リンクのノードには generate / link のボタンが出る使用感。

**事実(本追記時に確認):**

- **frame plan のノードは shader 参照を運んでいない。**
  `framePlanToJson` の 23 フィールドに `shader` は無い。
  **studio は今日、ノードがどのシェーダを使うか構造的に知り得ない**
- **studio に設定の永続化が一切無い**(`QSettings` 使用 0 件)。
  「コードエディタ指定」を作るならそれが最初の設定項目になる
- **プロジェクト相対のシェーダは既に動く慣行**
  (`projects/sprite_demo/shaders/` / `projects/vrm_xr_demo/shaders/`)
- **インタフェース生成器が既にある** —— WP339 の
  `generateShaderResourcePortInclude`(`shaderresourceinterface.hpp:67`)が
  ノードの `resource_ports` 宣言から `pelican_sample_<port>()` の口を生成する。
  **「ノードからヘッダ付きシェーダを生成」の中身はほぼこれで、include 参照 +
  各 port をサンプルする main() の雛形を書くだけである**
- **生成 include と `embed_shader` 焼き込みは排他**(`shaderlibrary.cpp:285-289`)。
  生成対象はプロジェクト側シェーダなので衝突しない

**設計判断(提案):**

1. **エディタ指定は v1 では不要。**OS の関連付け(`QDesktopServices`)で開けば
   設定ゼロで動く。特定エディタの指定は後からの設定項目(初の QSettings)
2. **「シェーダ無しノード」を config に存在させない。**パーサは fullscreen の
   `shader` を必須とし fail-fast で投げる。これは守る。
   代わりに**生成先行のフロー**にする: 「新規 fullscreen ノード」ダイアログで
   名前と port を決めた時点で、雛形シェーダとパス宣言を**同時に**書く。
   config は常に妥当なまま
3. **generate / link ボタンが出るのは「参照が壊れている」状態**
   (シェーダファイルが消えた・未解決)に対して。未リンクの下書きは studio 内の
   draft 状態として持ち、commit 前に解決させる
4. **frame plan に shader 参照を足すのは engine 側の加算 1 フィールド**(中段)。
   これは**照合警告(WP345 の「食い違ったら警告」)の表示にも前提として要る** ——
   ノードがシェーダを知らなければ、宣言との食い違いも表示できない

**既存の穴との合流**: 「既存パスにフィールドを設定する RPC」が無い問題は
region の永続化(WP343 の残件)と**同一の欠落**である。shader の link も
region の保存も、同じ 1 本の RPC(既存 authored pass への field 設定)で足りる。

**順序**: bloom 書き換え → G0 の安い 2 件 → 本件(shader 参照の field 追加 +
生成先行フロー)。特化ノードが生成フローに乗ると
「composite ノードを置く → 雛形シェーダが即座に開く」という体験になる。

### WP354: パス所有シェーダの宣言・実効・出所を frame plan が運ぶ

**§4 規則 11 の中段。仕様レビュー + コードレビュー。見積: 大。**
**本節は 2 度不合格になった第 3 版である(初版 9 指摘 → 第 2 版 10 指摘)。**
**末尾に両版の判定表を残す。**

#### 題名の限定(第 2 版からの変更)

**「パスが所有する」シェーダに限る。**material pass は surface ごとの material shader を
複数走らせる(`projectmaterialasset.cpp:115`)ため、「実際に走る全シェーダ」の G0 完全形には
**material drill-through(別 WP)が必須依存**である。本 WP は
`shader_resolution: "material_owned"` を正の状態として出すところまで。

#### 実装の位置(第 2 版の最大の欠陥への答え)

**投影の組み立て点は renderer である。planner ではない。**

- planner の `framePlanToJson` には raw config しか流れず、解決済み PassDefinition が無い
  (`frameplanner.hpp:168`)。**raw planner 経路の fixture(`frameplanner_test.cpp:980` 等)は
  本 WP で変わらない。変わったら raw JSON を写している証拠であり、実装の欠陥である。
  これを否定対照として明文化する**
- shader の解決済み値は renderer が pipeline 登録時に持つ。
  **`currentFramePlanJson` の組み立てで、frame plan に shader 投影を合流させる**
- **declared_ref は provider 上書きで失われる**ので、typed parse の時点で
  PassDefinition に捕捉して運ぶ(加算フィールド)。effective は provider 解決後の値

#### wire schema(完全形。曖昧な実装が「適合」を主張できないように)

ノードに `shader_resolution` を必ず出す(値は stable code):

```json
// 通常(fullscreen 等)
"shader_resolution": { "state": "resolved",
  "stages": [
    { "stage": "vertex",   "declared_ref": "shaders/fullscreen", "effective_ref": "shaders/fixture_fullscreen",
      "origin": "provider", "source_open_ref": "shaders/fixture_fullscreen.vert" },
    { "stage": "fragment", "effective_ref": "engine://ssao",
      "origin": "engine_default", "source_open_ref": null, "source_open_reason": "embedded_engine_resource" } ] }
// material
"shader_resolution": { "state": "material_owned" }
// shader を持たない型(anchor / snapshot_copy)
"shader_resolution": { "state": "not_applicable" }
```

- `state` ∈ `resolved | material_owned | not_applicable`。**排他。**`stages` は resolved のみ
- `stage` ∈ `vertex | skinned_vertex | fragment | compute | raygen | miss | closesthit`。
  **ray の配列は `{stage:"miss", index:0}` 形式で 1 要素 1 エントリ、authored 順**
- `origin` ∈ `authored | engine_default | provider | generated`
- `declared_ref` は authored があるときだけ。`effective_ref` は必須
- `source_open_ref` は**拡張子付きの開けるパス**または null。null のとき
  `source_open_reason` ∈ `embedded_engine_resource | generated | source_not_found` 必須
- **未知の state / origin / reason を studio は名前付きエラーで拒否する**(黙って握らない)

#### やること

1. **PassDefinition に declared 捕捉を加算**(typed parser で。raw 再解釈をしない)
2. **renderer の `currentFramePlanJson` で投影を合流**(上記 schema)
3. **stem→stage 拡張子の規則を `pelican_project` の共有 resolver へ**、
   `source_open_ref` を engine が解決(`user://` / `project://` / mount / root 未設定の
   名前付きエラーを含む)
4. **studio**: 表示(declared ≠ effective なら両方 + origin バッジ)、
   開く(`source_open_ref` があるときだけ有効。無効時は reason を表示)
5. manual 更新

#### 受け入れ条件

**配線(production 経路のみで証明):**

- **実在 config + 実 provider 登録**(手組み PassDefinition の helper 直叩きは配線の証明に
  ならない —— 第 2 版が誤って既存 unit fixture を「本番」と呼んだ)で、
  compile → `currentFramePlanJson`(または `get_frame_plan` handler)→ `FramePlanModel` →
  widget action seam を 1 テストで通す
- **mutation 条件: provider 解決の呼び出しを外すと同じテストが落ちること**を確かめる
- 同一テスト内対照: authored / provider 上書き / 省略既定(shadow_depth:
  declared 無し + effective=engine://shadow_depth + origin=engine_default)/
  material_owned / not_applicable

**fixture(第 2 版の「除去で旧一致」は持続不能だったため定義し直す):**

- **`*.pre_wp354.json` の不変 baseline を別ファイルで保存し、updater の上書き対象から外す**
- **通常 CI で `stripWp354(現行) == pre_wp354` を検査**(加算のみの構造的証明が
  fixture 更新後も生き続ける)
- **raw planner 経路の fixture(8 件の exact 呼び出しのうち planner 系)は変わらないこと**
  そのものを検査する(上記トリップワイヤ)

**開く参照(正確なパスで):**

- unprefixed project stem / `project://` 明示 / `user://` / mount / `engine://`(null+reason)/
  root 未設定(名前付きエラー)の**全種を正例・負例で**
- **runtime compiler OFF は「同じ名前付き失敗」を比較対象にする**
  (project shader の checked-in `.spv` は存在しない。OFF で pipeline が立たない構成は
  WP353 と同じ流儀で named failure の一致を検査)

**variant matrix:**

- `SKIP_DEVSTUDIO=OFF` のビルド + テスト実行 / runtime compiler ON/OFF /
  **`PELICAN_WITH_IMGUI` ON/OFF**(`ImGuiPassInfo` は条件付き型)/
  **`PELICAN_WITH_OPENXR` + flat/xr**(xr は pass 名が `#xr` 化され velocity/ui が抜ける)
- preview は対象外(明記)

そのほか: 全数 2 回、doclink 緑、GPU はエージェントに走らせない。

#### やらないこと

- material drill-through(必須の後続 WP として名指し)/ 生成 / 書き込み RPC / QSettings
- planner(`framePlanToJson`)への shader 追加 —— **してはいけない**(上記)

#### 判定表(2 回のレビューの記録)

| 指摘 | 初版 | 第 2 版 | 第 3 版での答え |
|---|---|---|---|
| verbatim は走るものでない | 発生 | 部分解消 | declared を parse 時に捕捉、effective は provider 後。組立は renderer |
| stem は開けない | 発生 | 部分解消 | source_open_ref を schema で必須化、全参照種を条件に |
| material 偽陰性 | 発生 | 部分解消 | 題名を限定、drill-through を必須依存に |
| 数の主張(23/9件) | 発生 | 再発(表 8 行) | 数を書くとき数え方を書く。判定表を完全化 |
| 参照種の欠落 | 発生 | 再発(条件側) | user:// / project:// を正例・負例で条件化 |
| fixture 前提 | 発生 | **逆転**(planner 経路は変わらないが正) | 不変 baseline + トリップワイヤ |
| 配線の証明 | 発生 | 再発(unit fixture を本番と誤認) | 実 config + 実 provider + mutation 条件 |
| wire schema 未定義 | — | 発生 | 完全形を本文に規定 |
| variant matrix | 発生 | 再発(ImGui/XR 欠落) | 4 軸を明記 |

#### 第 3 版レビュー(8 指摘)への修正 — v4 差分。仕様レビューはこの 3 巡で打ち切る

**骨格は生存した**(declared 捕捉 / renderer 合流 / D0 / material 限定 / mutation 方針は
レビューが壊せなかった)。以下は精度の修正。**残余は §10 コードレビューで受ける。**

1. **origin 規則を iff で固定**: `declared_ref` は「parse 前 JSON に stage があった場合、
   かつその場合に限り必須」。`engine_default` は parser が省略を補った場合のみ
   (fullscreen は両 stage 必須なので engine_default になり得ない)。
   provider は **pair 全体を返す契約**なので、identity passthrough を除き
   両 stage とも `provider`(文字列が authored と同値でも)。
   規範例は「両 stage provider の fullscreen」と「declared 無し engine_default の shadow」に分割
2. **profile を明示**: renderer 経路の文書に `"profile": "runtime"` を加え、
   `shader_resolution` は runtime profile でのみ必須。raw planner 出力は profile 無し(従来)。
   Studio / PlanViewer は runtime profile を要求する
3. **トリップワイヤの訂正**(私の事実誤認 3 件目: `framePlanToJson` は既に
   `CompiledRenderPipeline*` の省略引数を持つ。「raw しか流れない」は誤り):
   実装は **renderer 所有の `appendShaderResolution`** とし、
   **`currentFramePlanJson` からその呼び出しを除去すると production テストが落ちる**
   mutation 条件を置く。runtime branch にも pre_wp354 の exact fixture を置く
4. **全 pass-family の state 対応表を必須化**: ui / imgui は
   `engine_fixed`(embedded の resolved。PassDefinition 非所有)として明示。
   出荷 family の具体 effective ref を検査する
   (raster=sprite_demo / compute=clustered_lighting / ray=rt_shadow_mask(複数 miss で
   index と authored 順)/ skinned_vertex=velocity)
5. **`source_open_ref` は論理 scheme 正規形**(`project://` / `user://` / mount 名)で
   wire に固定。engine は存在検査のみ。**studio が同じ `pelican_project` resolver で
   物理パスへ解決して開く。絶対パスを wire に出さない**(machine 依存の排除)
6. **schema の締め**: material は object `{"state":"material_owned"}` に統一(前段の
   string 表記は誤り)。全 field を required-iff / forbidden-otherwise で定義。
   stage 順を固定、同一 stage 重複禁止、`index` は ray 系のみ。
   `source_open_ref` 非 null のとき `source_open_reason` は禁止
7. **`stripWp354` の定義**: `/nodes/*/shader_resolution` のみを除去し、
   除去前に全 node で field が存在したこと、recursive diff で他 path の差分が
   1 件も無いことを検査。live 比較には device/runtime 依存 subtree の固定除外リストを規定
8. **matrix**: `SKIP_DEVSTUDIO` は **ON(resolver / core renderer)と OFF(model / widget)の
   両方**をビルド + テスト。OpenXR は「ON: flat + xr」「OFF: flat 成功 + xr unavailable」の別行

依存: 無し(実装は WP353 回収後)。見積: **大**(初版「小」→ 第 2 版「中」→ 実態)。

### 設計ノート: オブジェクトモデルの現在地とプレファブの方向(2026-08-28、実測調査)

**利用者の要望**: エンジン一般側の強化。オブジェクトへのコード/コンポーネントの結び付け、
インスタンス前のコンポーネント束のプレファブ化をどう扱うか。

#### 実測の要点(詳細は調査エージェントの報告。file:line は検証済みの主張のみ)

**在るもの:**
- シーン = 単一 JSON の `scenes.<id>.objects[]`、各 object は `{name?, parent?, components[]}`。
  **`parent` は実装済みだが出荷での使用 0 件**(負のテスト fixture 3 件のみ)
- コンポーネント codec は**ハードコードの 7 種**(`componentcodec.cpp:856-872`)+
  codec を持たない 8 種目 `behavior`
- **behavior がオブジェクト⇄コードの結び付けとして既に成熟している**:
  `PELICAN_REGISTER_BEHAVIOR(型, 安定名, schema版)`、型付き params(schema 指紋で
  リロード時の漂流検知)、二相 activation、決定的順序(`attachment_seq`)
- 編集 op は 6 種(`set_component_value / add_component / remove_component /
  spawn / destroy / reparent`)が RPC に**在る**。batch は局所前進で親→子 spawn 可
- モデルは asset 名経由で共有される(同名 model 28 体が 1 template を共有)

**無いもの(探して無かった):**
- **プレファブ / object template / 複製 / 文書参照は一切無い。**
  `design_scene_format.md:139` が「RenderWorld 期まで持ち込まない」と明示保留
- **ECS コンポーネント型の利用者登録は不可能**(2 軸で閉じている):
  登録シンボルが `PELICAN_API` 非公開(`registerer.hpp:32,89`)+ 著作語彙も codec 表で閉、
  id は手番号で上限 64
- **`GameContext` にオブジェクト名の検索が無い。**エンジン内部の name→entity map
  (`scene.hpp:66`)は公開されていない。システムは著作されたオブジェクトを見つけられない
- GUI(studio / ImGui とも)が発行する編集 op は **`set_component_value` だけ**。
  spawn/destroy/reparent/add/remove は外部 RPC クライアントからしか届かない。
  Outliner は読み取り専用

**文書と実装の食い違い(要修正、コードか文書のどちらかを):**
- `design_game_logic_native.md:14-16` は「`registerComponent<T>(name)` で game DLL から
  登録できる」と読める記述だが、**シンボルが非公開なので今日は不可能**
- `docs/manual/13_editor.md:284` は ImGui Inspector が behavior attach/remove を
  サポートすると書くが、**コードは発行していない**

#### 設計の方向(提案。実装しない)

**1. コード結合は behavior を土台に強化する。新機構を発明しない。**
behavior は「著作されたオブジェクトに、安定名で型付き params とともにコードを結ぶ」
形を既に持ち、リロード・スキーマ検証・順序が揃っている。弱点は周辺:
- `GameContext` に名前検索が無い(小さく、効果が大きい)
- behavior 間参照 / `onFixedUpdate` が無い(設計文書自身が open 項目と認める)
- params が平坦フィールドのみ(入れ子・配列不可)

**2. プレファブは「参照 + 明示 unpack」の家風に従う。**
このリポジトリは既に二度この選択をしている:
render preset は「上書き不可。copy/eject が先」(`renderpipeline.cpp`)、
physical plan の eject は「名前付きの一方向操作。**prefab の unpack と同じ作法**」
(台帳 :5477 の自己言及)。
**したがって: インスタンスはプレファブ文書への参照 + パラメータ束縛。
パラメータ外を編集したければ明示 unpack(一方向)。
Unity 型の override 追跡(逆向きの半連結)は作らない。**

**3. 形は feature と同型に。第二の再利用機構を発明しない。**
`{ref, parameters}` の束縛、束縛時の型付き制約検査、完全一致置換、名前の鋳造
(view 系統複製で学んだ: 置換でなく鋳造)、provenance タグ、
エディタでの畳み表示(WP341 の仕組みがそのまま使える)。
render と scene で置換文法や検査の流儀が割れることが最大の負債になる。

**4. 展開は load 時。runtime は素の objects を見る。**
authoring 文書は参照を保持(編集の真実)、`SceneLoader` が展開(feature compose と
同じ位置)。runtime にインスタンス同一性を持ち込まない(v1)。
**多オブジェクト・プレファブは `parent` に依存するが、`parent` は出荷使用 0 件なので
実戦経験が無い。v1 を単一オブジェクト(コンポーネント束 + params)から始める選択肢を
設計段階で比較すること。**

**5. runtime spawn の語彙にもなる。**
今日、コードからの生成は field を並べる命令形しか無い。プレファブ registry が在れば
`ctx.spawn("enemy", params)` が書ける。著作と runtime の生成が同じ語彙になる。

#### 次の一手(順序)

```
1  プレファブ設計文書(上段。§5 の設計敵対レビュー(codex)を掛ける)
2  安い独立 WP: GameContext の名前検索 / GUI からの spawn・destroy /
   文書と実装の食い違い 2 件の解消
3  ECS 型の利用者登録を開けるかは、プレファブ設計の後に判断
   (behavior params で足りる範囲が設計で見えるため)
```

レンダリング軸の隊列(WP353/354 → bloom → G0)には割り込ませない。

#### 追記: Unity と意図的に変える点(同日、利用者との検討)

| 論点 | Unity | pelican の選択(根拠) |
|---|---|---|
| 上書きの面 | 任意フィールドを instance override(太字表示・apply/revert・入れ子で複雑化) | **作者が宣言したパラメータだけが上書き面。それ以外は明示 unpack**(preset/eject の家風。縛りは案内) |
| 参照 | GUID + fileID(.meta 汚れ、diff 不能) | **名前参照 + load 時 fail-fast 検証**(parent 検証が既にこの形)。rename は参照書き換えを伴う編集 op(renameObject が既にある) |
| コードとデータ | MonoBehaviour が混合 | **behavior(コード)と component(データ)の分離を維持**。強化は params の表現力(入れ子・配列)で行い、混合には戻らない |
| runtime 生成 | Instantiate = 深いクローン、命名 (Clone)、リプレイ不能 | **registry からの ctx.spawn(名前, params) を決定的に**。命名は鋳造、リプレイ/golden の時系列に乗る |
| アセット取込 | 自動 import + 不透明な Library キャッシュ | **宣言的登録のみ**(asset_data.json)。隠れたキャッシュ・二次的真実を作らない |
| プレファブの器 | 専用アセット型 + 専用編集モード | **scene と同じ object 文法の小文書 + parameters 封筒。第二の文法を作らない** |

**「データ流し込みを容易に」への具体的な答え(現状の欠落):**

- **asset_data.json へ登録する編集 op が存在しない**(手編集のみ。許可キーは
  schema/version/models/materials/textures)。`load_gltf` は runtime 専用で
  `save_scene` を汚染する(RuntimeOnlyData)
- したがって「流し込み」の最小実装 = **authoring レベルの asset 登録 op**
  (studio へのファイルドロップ → assets/ へ複製 → 登録 → 任意で spawn)
- glTF fragment 参照(`city.glb#mesh/LampPost`)は既にあるので、
  **glTF の部分木を読み取り専用テンプレート源として prefab から参照する**形が自然
  (reference-or-eject の家風に一致)
- 表・統計のようなデータ資産(Unity の ScriptableObject 相当)は registry の
  汎用データ種別として置けるが、**リポジトリに需要の証拠がまだ無い。投機として記録のみ**

#### コードレビュー(codex、af3882a / 880dbeb / 29a0615 まとめて)の仕分け

**実害 4 + 偽対照 2 = 直す(WP355)。記録のみ 3。**

直す(根拠は各 file:line ともレビューが実証済み):

1. **preview/project 層が不正な `raster_state` を受理**(所有権しか見ず入れ子を解析しない。
   studio/preview が「有効」と言った文書が実行時に初めて落ちる。fail-fast の後退)
2. **studio で blend を著作できない**(fullscreen form の `draftJson()` に raster_state が無く、
   projection authority も旧のまま。**テストが同じ authority から期待値を組むので、
   機能が丸ごと欠けたまま緑**)
3. **存在しない添付への op が黙って消える**(`depth: null` + `depth_load_op: Load`。
   親版 typed 経路は空文字資源を reads に足していた —— 別の形で壊れていた。
   等価化の方法が「両方で黙殺」だった)
4. **添付 op の既定値が 4 箇所以上に複製された**(raw planner / PassDefinition members /
   2 つの fallback helper + UI 特例 2 箇所。既定値一箇所の原則違反)
5. **逆向き対照の欠落**(pass-wide Load + attachment Clear/DontCare が未検査。
   「override は Load のときだけ勝つ」誤実装が全テストを通る)
6. **`statusJson()` の合計がビット単位で変わる**(親版は破壊的ソート後に加算、
   新版は実行順で加算。加算順の変更で `logical_frame_history` / `averages` / `views` /
   `total_sum` の double が変わり、`get_status` の外部契約に出る。算術反例確認済み)
   + **`last_snapshot_history_frame_visits` が自己申告**(全履歴再走査を戻しても
   カウンタが 1 のまま通る。§4 規約 10 の否定対照として機能していない)

記録のみ: XR runtime registration 未テスト / headless の blend テストは
`PELICAN_RUNTIME_SHADER_COMPILER` OFF で空になる / 出荷 plan 比較は raw planner のみで
flat/xr/preview の行列ではない(コード読解では variant 依存分岐は見つかっていない)。

レビューが壊せなかった点: 省略時の pipeline 等価 / `raster_state: {}` の Vulkan 値一致 /
headless の画素対照は本物(ON 構成) / 窓 30 の意味は旧版と等価 / D0 維持。

### WP355: コードレビューの実害 4 件と偽対照 2 件を塞ぐ

**§4 規則 11 の中段。修正 WP —— 仕様は上の仕分けとレビューの実証で足りる。
マージ後の §10 コードレビューは行う。**

上記 1〜6 を直す。それぞれについて:

1. **composition 後の project 層**で fullscreen / output_transform の `raster_state` を
   共有 `parseRasterFixedFunctionState()` + depth 禁止で検証する。
   **対照**: 不正な blend 値だけを足した fullscreen が project resolve /
   preview 経路(`previewgraph.cpp:91-140`)で名前付きに落ち、正常値は通る。
   core 側の検証は残す(両経路が同じ名前付き違反で拒否)
2. fullscreen form に `raster_state` の **lossless passthrough** を足し、
   projection authority に含める。**テストは authority から期待値を生成せず**、
   additive と omitted を同一フォームで保存して解決値が異なることを確かめる
3. op があるのに対応する出力が null/空なら、**raw / typed 共通の検証で名前付きエラー**。
   **対照**: `depth: null` + `depth_load_op` が落ち、深度添付ありの同じ op は通る
4. pass type × aspect → 既定 `PassAttachmentOperations` の **canonical 関数 1 箇所**に集約し、
   全経路(raw planner / PassDefinition / helpers / UI 特例)がそれを使う。
   **対照**: canonical の値を 1 箇所変える mutation で全経路のテストが一斉に落ちること
5. **pass-wide Load + attachment Clear/DontCare**(color / depth 両方)を追加し、
   raw / typed 双方で resolved op・reads・footprints を検査
6. 全合計(nodes JSON / logical total / view total)を **canonical 順の pointer span** で
   加算し、親版 `29a0615^` と**ビット一致**に戻す。
   **対照**: 宣言順と実行順が異なる fixture(`golden_harness.cpp:1119-1139` の形)で、
   修正前の実行順加算ならビットが変わることを先に確かめる。
   `visits` は**実走査の計測に置き換える**か、eager reference publisher との
   出力同値 + 走査量 120 対 1 の比較にする(自己申告カウンタを対照と呼ばない)

受け入れ: 全数 2 回 / doclink 緑 / `SKIP_DEVSTUDIO` 両構成 / GPU はエージェント外。
やらないこと: 記録のみの 3 件 / 新機能。

依存: WP351b, WP352, WP353。見積: 中。

### WP356: 読者のいない検査面を消し、再発を構造で塞ぐ

**§0 の下段(studio 内・表示のみ + 自明な削除)。コードレビューのみ。**

#### 事実(孤児監査 2026-08-28。全件 file:line 実証済み)

このアーク(WP338〜355)が「書くだけで誰も読まない」検査面を 9 件作った。
アーク前から同型の死骸が 12 件ある。病気は「`FramePlanGraphicsScene` が
property / role を、読み手が育つより速く発行する」こと。

#### 消すもの(アーク由来)

- `test/run_rpc_headless.cmake` の `wp351_rpc_response_sizes.json` の `file(WRITE)` と
  `get_status_response_bytes` の測定(消費者ゼロ。4096 上限ゲートは別実装で生きているので残す)
- `RenderTiming::latestViewRows()`(呼び手ゼロ。member は statusJson が使うので残す)
- WP347 の辺 role 5 本(`FramePlanBarrierKindsRole` / `BarrierCount` / `OrderOnlyCount` /
  `SamePixelAttachmentCount` / `FusedBarrierCount`。読者ゼロ、情報は tooltip 経由で届いている)
- `pelicanUnmatchedBarrierRecordCount` / `pelicanCurrentGroupScopeLabel`
- WP350 の凸包 property 4 本(`pelicanConvexHullProposalText` / `Members` /
  `MissingMembers` / `Impractical`。テストは item role 側を読む)
- `FramePlanBoundaryStubLabelItem` / `FramePlanGpuTimingRow::node_ordinal`

#### 消すもの(アーク前の同型死骸)

`FramePlanFusionOverlayItem` / `FramePlanParallelOverlayItem` / `FramePlanPhysicalEmptyItem`、
property 9 本(`pelicanCurveEdgeCount` / `pelicanEdgeBundleCount` /
`pelicanResourceOverlayCount` / `pelicanTargetCount` / `pelicanVisibleDependencyRecordCount` /
`pelicanFusionCandidateCount` / `pelicanParallelCandidateCount` /
`pelicanPhysicalExplicitEmptyCount` / `pelicanExecutionPlanReason`)。

#### 残すもの(理由つき。消さないこと)

- `FramePlanInternalEdgeRecordsRole` / `pelicanCollapsedGroups`:
  読者はテストのみだが、**「畳んでも記録が失われない」「孤児グループが更新を生き残らない」
  という他で観測できない不変量を検査している**
- `lastSnapshot…VisitsForTesting`: WP355 で実走査計測に置換済み

#### 再発防止(本 WP の本体)

**source-audit テストを足す**: `frameplangraphics` が `setProperty` /
`setData(role)` する識別子を列挙し、**それぞれに読者(本番 or テスト)が
1 箇所以上あること**を検査する。読者ゼロの発行を追加したら赤になる。
既存の source-audit の流儀(`devstudio_frameplan_graph_test.cpp:4796` の
mutation guard)に合わせること。

#### 受け入れ条件

- 削除対象の全識別子が `src/` `test/` から消えること(grep 0 件)
- **source-audit が実際に噛むこと**: 読者の無い property を 1 つ足す mutation で赤、
  戻して緑(実際に当てて確かめる)
- 全数 2 回(GPU はエージェント外)、doclink 緑、`SKIP_DEVSTUDIO=ON` ビルド

やらないこと: 新しい表示 / `clear_colors` `input_sampling` 等の著作語彙 /
subgraph 置換機構(これらは著作面であり検査面ではない)。

依存: WP355。見積: 小。

### WP357: bloom の upsample 鎖を、標準形の加算 blend に置き換える(第 3 版)

**§4 規則 11 の上段。仕様レビュー 2 巡不合格(6+6 指摘)。**
**第 2 巡の指摘には処方箋が付属しており、本版はそれを採る。ここで仕様レビューは打ち切り、
残余は §10 コードレビューで受ける。末尾に両巡の記録。**
**利用者判断: A(見た目の変化を標準形への移行として受け入れる)。**

#### 数式(量子化前の係数。検算 2 回済み)

旧 `2·V3 + 4·(V2+V1+V0)` → 新 `2·ΣV`。sRGB blend は linear 空間(仕様確認済み)。
差の源は空間(bilinear)× 段ごとの量子化。

#### やること

1. `bloom_upsample.frag`(1 入力 bilinear)。**`.frag` と `.frag.spv` 両方の embed ID 登録**
2. UpsampleBlend_1..3 の書き換え:
   - 入力 coarser 1 本、出力添付に `"load_op": "Load"`
   - **pass-wide `color_load_op` は削除する**(添付 `load_op` だけを残す。二重指定を残さない)
   - blend: `{"color":{"src":"one","dst":"one","op":"add"},"alpha":{"src":"one","dst":"one","op":"add"}}`
3. FinalBloomComposite の入力 1 を H_0 → V_0
4. fixture / golden / **色移行台帳** の更新(§閉包)

#### 受け入れ条件(第 2 巡の処方箋を採用)

**oracle(「一致」を不等式で定義する):**

- oracle は test-support の**単一 API に抽出**(現行 `color_pipeline_test.cpp` の関数群は
  anonymous namespace で再利用不能)。入力: 実 device の `subTexelPrecisionBits` /
  実 extent(160×90 なら 80×45 / 40×22 / 20×11 / 10×5。scale は段ごと切り捨て)/
  pixel center 座標 / address mode
- **LDR sRGB8**: 各 V 段の encoded byte を「±1 storage code + device 由来の
  2D filter 上限 `dx/2^b + dy/2^b`」で比較(WP339a の既存流儀。
  `golden_harness.cpp:9311, 9620` の機構を使う)
- **HDR は `R16G16B16A16_SFLOAT`**(「R16F」ではない): 隣接する合法 binary16 と
  合法 blend 精度から許容区間を作る。Inf/overflow は刺激から除外
- RGB のみ sRGB 変換、**alpha は線形として別計算**

**刺激(一様画像では bilinear / address mode / 座標の誤りが隠れる):**

- 本番 UB 3 パスを保った**派生 test config** + 決定的 producer が各 V 段へ
  **非一様 2D パターン**(端・内部・x/y gradient)を注入(中間段は本番入力から到達不能のため)
- **test 側で V0..V2 に `TRANSFER_SRC` を足し、各 UB 直後を readback**
  (最終画素だけでは段間の誤差相殺を許す)
- `input_sampling` は派生 config で明示(linear/repeat)。**oracle は repeat の端回り込みを
  模倣する**(既定 sampler は repeat であり、端画素で反対端が混ざる)。
  実 physical sampler も検査

**本番配線(3 本まとめての変異は 2 本の未配線を見逃す):**

- 各 UB について **physical `GraphicsPipelineDesc`**(shader identity / sampler / load /
  RGB・alpha 全 blend 係数)を既存 testing accessor で exact 検査
- **「UB1 だけ剥ぐ」「UB2 だけ剥ぐ」「UB3 だけ剥ぐ」の 3 変異を個別に**実行し、
  各回で解決値と期待画素の両方を検査
- manifest 検査: 32 本は raster_state 省略 / UB 3 本は明示 One/One、かつ
  **`contains("color_load_op") == false`**。Load 変異は添付 field だけを削る
- 係数 exact 検査が `"additive"`(SrcAlpha)誤置換を静的に拒否

**`load_op` の意味:**

- 本番 graph の変異: 「reads から落ち、RAW が WAW に変わる」を plan fixture で固定
- **最小 2 パス graph(第 2 版の形は no-Load 側が plan 不能だった)**:
  両 variant に明示 `after: "A"`(順序だけ運び画素を運ばない)。B は入力・port・history 無し、
  定数 shader、discard 無し。差は添付の `Load` 対 `Clear`(clear 値 RGBA=0,0,0,0 明示)のみ。
  covered center pixel で Load 側 = `quantize(D+S)`、Clear 側 = `quantize(S)` を実値検査

**閉包(第 2 巡で追加確定した分を含む):**

更新: `engine_resources.json` / `frameplanner/plans/example_main_render.json` /
`fixtures/devstudio/example_frame_plan.json` / `devstudio_frameplan_graph_test.cpp`
(**H→UB の 3 辺削除 + UB 鎖の resource を H2/H1/H0 → V2/V1/V0 に 3 置換 +
records 100 → 97 を数値で固定**)/ `renderer_execution_traces.json` の 2 subtree /
golden 2 PNG + `inventory.json` SHA + `wp73_rgba8_hashes.json` /
**`docs/color_migration_manifest.json`**(新 shader・蓄積変更・3 pass 項目。
golden 差分承認の照合先である機械可読台帳。第 2 版はこれを落としていた)。
不変対照: `canonical_frame_plan_trace.txt`(pass 順不変)と
**`devstudio_fullscreen_pass_test`**(UpsampleBlend_3 を選択する直接 consumer)。

- matrix: LDR sRGB8 / HDR SFLOAT × compiler ON/OFF。XR 範囲外(鎖が無いことを確認済み)
- 全数 2 回、doclink 緑、`SKIP_DEVSTUDIO` 両構成、GPU はエージェント外

#### やらないこと

FinalBloomComposite の blend 化 / down 鎖変更 / intensity パラメータ化 / tent filter /
本番 config の `input_sampling` 変更(派生 test config のみ)。

#### 2 巡のレビュー記録

第 1 巡: blend JSON がパーサ不受理 / Load 対照が推移順序で不成立 / oracle が理想線形のみ /
3 本剥ぎの抜け穴 / 閉包 10 件未列挙 / matrix 不足。
第 2 巡: 「GPU 一致」に一意解なし(device 依存精度)/ 一様 basis の隠蔽 /
3 本まとめ変異が 2 本を見逃す / 最小 graph の no-Load 側が plan 不能
(WAW barrier は順序決定の後)/ pass-wide キー残存の二重指定 / 色移行台帳の欠落。
**両巡で壊れなかった**: 理想係数 / Load→reads 推論の実在 / sRGB linear blend /
XR 範囲外 / purgeability / D0 / pass 順不変。

依存: WP352、WP353、WP355。見積: **大**(oracle と段別 readback の分)。

#### コードレビュー(134f9a3 = WP355)の仕分け

**元 6 指摘は実質閉鎖**(判定表はレビュー本文)。旧拒否→新受理ゼロ、出荷 68 JSON /
129 pass 走査で plan 全一致、WP325 期待値は完全一致 6 フィールドで「緩め」ではない。

**直す(WP355a。この連鎖はここで打ち切る):**

1. **[実害] 出力の無い `clear_color` / `clear_colors` が依然黙って消える**
   (load/store と同型の穴。`shadow_depth` + `clear_color` が無音)。
   raw/typed 共通で non-empty `output.color` を要求し名前付きエラー
2. **[新規弱化] `PassAttachmentOperations{}` の意味が変わった**
   (initializer 削除で value-init が Load/Store、default-init は未初期化 enum)。
   default constructor を消し、明示 constructor を必須にする
3. **[規約 10] studio の否定対照が JSON 差分しか見ていない**
   (core parser の代入や pipeline 適用を消しても緑)。
   同テスト内で共有パーサが解決した `RasterFixedFunctionState` を additive/omitted
   両方について比較する
4. **[防御] 同一 `GpuTimingSampleIdentity` の重複が canonical 順で未定義**
   (出荷到達性は未確認だが API として穴)。フレーム内重複を名前付きで拒否
5. **[安価な fail-fast] typed 直接構築で `pass_info` と `pass_type` / 既定値が乖離できる**
   —— constructor 再設計はせず、**planner 側で pass_type と pass_info variant の
   整合を検証して名前付きエラー**にする(無音の乖離を fail-fast に変える最小手)

**台帳に書くだけ:**

- typed 構築の factory 化(pass_info / type / 既定値の不可分設定)は本丸だが
  再設計規模。上記 5 の検証が穴を音に変えるので保留
- OFF 構成(`PELICAN_RUNTIME_SHADER_COMPILER=OFF` / `SKIP_DEVSTUDIO=ON`)の
  **実行**は統合側の恒常的な穴。headless の blend 画素テストは ON 専用のまま

### WP355a: レビュー残余 5 件(実害 1・弱化 1・対照 1・防御 2)

**§0 中段。仕様は上の仕分けで足りる。マージ後の §10 レビューは行い、
そこで出た指摘は直さず台帳に書く(連鎖打ち切り)。**

各件とも: 変異を実際に当てて落ちることを確かめること
(1: clear_color 付き shadow_depth が named error / 2: `PassAttachmentOperations{}` が
コンパイル不能 / 3: core 側の代入を消すと studio テストが落ちる /
4: 重複 identity 注入が named error / 5: 乖離構築が named error)。
全数 2 回、doclink 緑、SKIP_DEVSTUDIO 両構成ビルド、GPU はエージェント外。

依存: WP355。見積: 小。

#### 実装後コードレビュー(WP355a working tree)の台帳

- **新規指摘 0 件**。raw/typed 共通の attachment option 検証、
  `PassAttachmentOperations` の構築制約、Studio の共有パーサ否定対照、
  GPU timing identity の canonical 化時重複拒否、typed planner の
  `pass_type` / `pass_info` variant 整合を、対応テストと各指定変異の失敗まで確認した
- 直前仕分けで保留した typed 構築の factory 化は変更していない。
  本節をもってコードレビュー連鎖を打ち切る

### 設計ノート: リファクタリング計画(2026-08-28。全候補が実測済みの根拠を持つ)

**原則**: 憶測の綺麗さでは着手しない。**「この構造が実際に事故を起こした回数」で順位づける。**
方法は [[consolidation-refactor-equivalence]] に従う —— 集約で削る分岐は誰もテストしない、
を前提に、旧新等価のオラクル(exact fixture / 出荷 4 plan / golden)と
削除分岐の敵対探索(旧拒否→新受理・旧受理→新拒否)を毎回必須にする。
**同時に走らせるリファクタ WP は 1 本まで。**

#### R1: パーサ一本化 —— planner が raw JSON を再解釈するのをやめる(最高レバレッジ)

**事故実績**: 同じパス文書を raw planner(`frameplanner.cpp:675-`)と typed parser の
**2 本が別々に読む**構造は、WP303 と WP326 で「第二のパーサ見落とし」を 2 回起こし、
WP352 / WP355 では**全変更を 2 経路に二重実装**させた(テストも 2 倍)。
**パス意味論を触る全 WP が恒常的に 2 倍払っている。**

**形**: planner は `PassDefinition`(typed)だけを消費する。
**オラクル**: 既存の exact fixture 8 件 + 出荷 4 plan + golden 全一致。
**順序**: WP354 の後(WP354 は raw planner fixture 不変をトリップワイヤに使うため、
先に動かすと仕様が崩れる。WP354 着地後は逆に、その fixture 群が R1 の等価オラクルになる)。
規模: 大。上段レビュー(設計+仕様+コード)。

#### R2: パス検証の深層一本化 —— project 層で完結させる

**事故実績**: 「project 層は所有権だけ、core が深層」という二層が
WP355 の実害 1(studio が祝福した文書を runtime が拒否)を生んだ。
raster_state は塞いだが、**他の入れ子文法は同じ穴のまま**。
**形**: パス文法の完全検証を `pelican_project` に置き、core parser は消費者になる。
R1 と重なるため **R1 に統合するか直後**。規模: 中。

#### R3: Scene の単一投影(ResolvedScene seam)

**事故実績**: prefab v3 レビューが実証 —— **Camera が生の scene JSON を自前で再走査**
(`camera.cpp:634`)、editor は著作 component 添字で runtime 値を対応づけ。
生読者が複数いる限り、scene に触るどの機能も全読者を追う羽目になる。
**位置づけ**: プレファブ U1 の背骨として計画済み。**プレファブが遅れても単独で価値がある**
(このリファクタだけ先に切り出す選択肢を U1 の WP 化時に判断)。規模: 中〜大。

#### R4: 型語彙の leaf 化

**事故実績**: prefab v3 レビュー —— `StructFieldType` の enum と文字列変換が core
(しかも RPC 実装内)にあり、project 層の offline 検証から届かない。
**形**: 型 enum・変換・parameter JSON 検証を leaf library へ。core は使う側に回る。
**順序**: 小粒で独立。**プレファブ U1 の前提なので早めに。**規模: 小。

#### R5: typed 構築の不可分化(factory)

**事故実績**: WP355 レビュー指摘 3 —— `pass_info` 代入と `pass_type` / 既定値が乖離できる。
WP355a が fail-fast 検証で「音」に変える。**構築子の再設計は R1 後**
(R1 が構築面を縮めるので、先にやると二度手間)。規模: 中。

#### R6: fullscreen / output_transform の変種区別

**事故実績**: 所有表・shape 規則・実行時 variant を共有し `isFullscreen()` でも
区別不能なことが、WP353 の影響範囲見積もりを 1 度壊した。
**形**: variant に種別タグを持たせる最小変更。規模: 小。R1 後。

#### R7: 検査面の規律(= WP356、実行中)

書き込み専用 property/role 21 件の削除 + 「発行された識別子には読者が要る」source-audit。

#### 見送り(記録のみ)

- **frameplanner.cpp の分割それ自体**: R1 が済めば自然に痩せる。分割だけを目的化しない
- **8 つの一点物パス型の feature 化**: 特化ノード層(設計トラック)の仕事であり
  リファクタではない
- **doclink の散文検査**: 「数を書くなら数え方を書く」の運用で受けている。
  ツール化は事故がもう 1 回起きたら

#### 隊列との合流

```
消化中   WP356 → WP355a → WP357(bloom)→ WP354
その後   R4(小・独立)→ プレファブ U1(R3 内包)
本丸     R1(+R2)← WP354 着地が前提。着手時に設計文書を起こし §5 レビュー
以降     R5 → R6
```

#### コードレビュー(9568042 = WP357)の仕分け

**Reject: 実害 3・懸念 2・指摘 1。**壊せなかった側: oracle 本体の独立性(標準ライブラリのみ)/
出荷 35 本中 raster_state は UB 3 本のみで他 32 本の command 不変 / 個別 peel と Load/Clear
画素対照の実在 / D0 / golden SHA 一致。フレーク(WP334b)は単独 0/5 で既知一族。

**直す(WP357a。連鎖はここで打ち切り。次のレビュー指摘は台帳行き):**

1. **[実害] 3 頂点化が topology を無視**(`engine://fullscreen` + blend + `line_list` で
   親版 6 頂点→新版 3 頂点)。3 頂点化の条件に `topology == triangle_list` を加え、
   **`engine://fullscreen` への非 triangle_list 指定は名前付きエラー**にする。
   triangle/blend・line/blend・opaque の command-recording 対照を置く
2. **[実害] oracle harness が本番の急所を上書き**:
   (a) sampler 強制上書きをやめ、**実際に解決された sampler/format を検査**
   (b) oracle が **B8G8R8A8_SRGB を独立に扱う**(本番の解決形式)
   (c) **出荷 config を意味論不変で実行する脚**を追加し、
   **FinalBloomComposite の H0/V0 を同一テスト内で異なる既知値により対照**
3. **[実害] golden 更新を oracle 合格に条件づける**:
   updater 経路の内部で oracle を先に実行し、**全合格まで一切書き込まない**。
   CTest の DEPENDS では直接実行を防げないため不可
4. **[懸念] 死に値 2 フィールド**(vertex/fragment_shader の捕捉)を使うか消す。
   使うなら resolver 非依存の期待 bundle/digest と比較
5. **[懸念] ON/OFF の刺激を単一正本に**(fixture `.frag` を唯一の source にし、
   ON はそれを読み OFF は同じファイルから SPIR-V 化。stimulus hash を出力)
6. **[指摘] Load の隣の死んだ clear_color を全 5 本から削除**
   (UB 3 + FinalBloomComposite + hdr_tonemap。最小 fixture の Clear 側は生きているので残す)

台帳のみ: sampler 既定値が struct と parser に重複(既定値一箇所)——
canonical 化は R 系の既定値集約と合流させる。

### WP357a: bloom レビューの 6 件

**§0 中段。仕様は上の仕分けで足りる。各件、変異を実際に当てて落ちることを確かめる**
(1: line_list が名前付き拒否 + 親版比較 / 2a: 出荷 UB へ nearest を足す変異が検出される /
2c: Final の入力を H0 に戻す変異が落ちる / 3: oracle 失敗状態で updater を叩いても
golden が書かれない / 5: 片側だけ刺激を変えると hash 不一致で落ちる)。
全数 2 回、doclink 緑、SKIP_DEVSTUDIO 両構成、GPU は本 WP も実行可(直列)。

依存: WP357。見積: 中。

#### コードレビュー(e867099 = WP354)の仕分け

**実装本体は健全**(D0 / 未知 enum の fail-fast / resolver / 出荷 19 JSON 影響なし、を
レビューが独立確認)。**穴は門番側に 3 + 懸念 2。CI 掲載の欠落 1。**

**直す(WP354a。連鎖はここで打ち切り):**

1. **[実害] strip の baseline が偽物**: `pre_wp354.json` は実 pre-WP 出力でなく
   (name/kind のみ + 新設 profile を含む)、比較は `appendShaderResolution` 直叩き。
   **`e867099^` の実 renderer 出力を immutable baseline として固定**し、
   現行値も production 経路(`currentFramePlanJson`)から取得。全 node の field 存在確認 →
   許可 pointer のみ除去 → recursive diff 0 件。profile の扱いを明示
2. **[実害] raw planner 不変が null overload のみ**: 実在 `CompiledRenderPipeline` を渡した
   non-null の exact fixture を追加し、**「non-null のときだけ planner が
   shader_resolution を足す」変更が落ちる**ことを実証。helper が補完する前の
   不存在 REQUIRE も追加
3. **[実害] production 配線と matrix が通常 CI 不在**: compiler OFF で test discovery 前に
   return する構造を直し resolver/projection テストを OFF でも登録。PR gate に
   SKIP_DEVSTUDIO=OFF + offscreen の model/widget テスト行を追加。
   renderer production 経路を通る CPU seam テストを通常 ctest に載せる
4. **[懸念] declared iff の producer/Studio 不一致**: provider + declared 無しを
   producer は出せるが Studio が拒否する。origin 別の iff
   (authored=必須 / engine_default・generated=禁止 / provider=optional)に両者を揃え、
   その経路のテストを通す
5. **[懸念] provider 同値文字列の規則が未固定**: authored と同一文字列を返す実 provider で
   両 stage `origin=="provider"` を検査(「declared != effective で判定」への退行を塞ぐ)

### WP354a: shader 投影レビューの 5 件

**§0 中段。各件、レビューが示した「素通りする変更」を実際に当てて落ちることを確かめる。**
全数 2 回、doclink 緑、SKIP_DEVSTUDIO 両構成、GPU は直列で実行可。
依存: WP354。見積: 中。

### WP358(= リファクタ R4): 型語彙を leaf に新設し、core は写像で繋ぐ(第 3 版)

**§4 規則 11 の中段。仕様レビュー 2 巡不合格。第 3 版は戦略を変更した。**

#### 戦略変更の理由(v2 の 8 指摘の根)

v2 は「core の語彙を移設する」だった。膨張の源はそこにある:
`StructFieldSchema` は **DLL 境界を値渡しされる ABI 面**であり、動かすと
ABI タプルの定義(Debug/Release/CRT/toolset)、pre-WP バイナリ fixture、
loader の static-init 順序、SDK staging manifest、旧 DLL を常設ロードする CI が
芋づる式に必要になる(v2 レビューの指摘 1〜3・8)。

**v3: core は 1 バイトも動かさない。leaf を新設し、1:1 写像で繋ぐ。**
段 A(バイナリ固定)は丸ごと不要になる。fingerprint も SDK も無変更。
完全統一(core を leaf に載せ替える)は**今はやらない**と決め、
費用(v2 の 8 指摘)を理由として記録する。

#### やること

1. **独立 leaf target `pelican_schema_leaf`** を新設(依存: 標準ライブラリ + nlohmann のみ):
   - 正準の型名語彙(17 型。現行 RPC の綴りをそのまま正とする)
   - **双方向の部分関数**: string→enum(未知名は名前付きエラー)/
     enum→string(**無効 ordinal も名前付きエラー**。silent "unknown" 禁止)
   - **schema 宣言検証と値検証を別 API** として定義
   - **17 型の規範表**(JSON 形 / 境界 / vector arity / F32 表現可能性 /
     enum 最大 8・重複・default 整合 / range の min≤max と適用可否)を本文に置き、
     正負 corpus で全行を検査
   - `unit` は**不透明文字列の一致検査のみ**(意味検証はしない。その語を条件から外す)
2. **core adapter**: core enum ↔ leaf 語彙の 1:1 写像。**全射・単射・往復を
   静的/exhaustive テストで固定**(型を足して写像を忘れるとビルドか単体で割れる)
3. **`editorcommandservice.cpp` の変換を leaf 写像の呼び出しに置換**
   (文字列語彙の所有者を leaf 一箇所に。呼び手はこの 1 箇所であることを grep で確認)
4. project 層は leaf を直接使う(offline 検証の成立)

#### 二重規則のリスク管理(v2 指摘 6 への答え)

leaf の値検証(project/prefab 用)と core の実行時検証(behavior params)は
**当面並存する**。乖離を放置しないため、**共通 corpus を両方に通し、
受理/拒否の判定が一致することを 1 テストで検査**する
(将来 core を leaf に載せ替えるときの等価オラクルにもなる)。

#### 受け入れ条件

- leaf の include graph に `src/core` が現れないこと
  (**core の include dir を与えない独立 consumer build**で検査。link 検査では不足)
- 写像の全射・往復 exhaustive テスト(**型を 1 つ写像から外す変異で落ちる**)
- 双方向の名前付きエラー(未知文字列 / 無効 ordinal の両方。production RPC 経路で負例)
- 17 型規範表の正負 corpus(project 層のテストから leaf 経由で)
- **共通 corpus の leaf/core 判定一致**
- RPC `schema_fields` 出力バイト不変(**実装前に captured fixture を固定してから**)
- SDK・DLL 面が無変更であること(`userpublic` 配下の diff 0 / fingerprint 関連 diff 0)
- 全数 2 回 / doclink 緑 / SKIP_DEVSTUDIO 両構成 / GPU はエージェント外

#### やらないこと(理由つき)

- **core 語彙の移設・統一**(v2 の 8 指摘が費用。必要が実証されたら別 WP で、
  ABI タプル manifest・`--probe-game-logic-abi` の非 GPU 検査・staged-SDK 4 project build
  を含む完全形で設計する —— 指摘の処方箋は台帳のこの節に残る)
- behavior / event の実行時検証の置き換え(共通 corpus で監視するに留める)
- 型語彙の拡張(object 種別等はプレファブ U1 で)

#### 第 3 版レビュー(9 指摘)への修正 — v4 差分。仕様レビューは 3 巡で打ち切り、残余は §10

**構造は生存**(写像方式・D0・corpus 実装可能性はレビューが壊せなかった)。以下を採る:

1. **Studio の第三の語彙表を leaf に置換する**(見落としていた所有者)。
   widget 固有の UI 制約だけを Studio に残し、`i8=127` 成功 / `i8=128` 拒否を
   同じ production-preflight テストで検査
2. **「production RPC path で未知文字列」を分割**(engine 側に到達経路が無い):
   無効 ordinal は fake registration 経由の engine `get_components` 応答生成で /
   未知 wire 文字列は **Studio の応答取込経路**で。曖昧語は排除
3. **corpus を 2 種に分割**: 値 corpus は**全 17 型を持つ typed test behavior を登録し
   `canonicalize_params` 経由**(core 側 API を名指し)。schema 宣言 corpus は
   leaf 単独とし、core 側の宣言不正は consteval のため**共有しないと明記**
4. **oracle の主張を限定 + 深化**: 「値受理集合の oracle」と明記した上で、
   canonicalized JSON・default 補完・**解決値**(例: f32 16777217 → 16777216)・
   拒否時の安定エラー分類と path まで同テストで比較
5. **写像は 17 組を個別に意味固定**(対交換 `I8↔U8` の mutation で落ちること)。
   当該 target に switch-enum の warning-as-error(/we4061 /we4062 相当)。
   RPC fixture に全 17 型を必ず含める
6. **wire fixture は WP354a の流儀で**: 親版の named SHA を shared clone で取得し、
   **`RpcServer::run` の実 bytes(envelope + 改行込み、全 17 型)**を出所ファイル付きで
   固定。実装側からの再生成禁止。さらに **sentinel resolver seam**(拒否 resolver に
   差し替えると実 RPC 出力が変わる/名前付きエラー)で、新配線が実際に使われることを観測
   (grep は配線証明にならない)
7. **event 経路を第三の比較対象に追加**(15 型。型は alias なので安い)。
   「監視する」という宙に浮いた記述を条件化

依存: 無し。見積: 小〜中(v3 で ABI 面が消えたため)。

### WP359: vector 成分の typed encode が自分の decoder に拒否される

**§0 中段。WP358 の凍結遵守が発見した実バグ。**

`structfieldjson.hpp:321-331` の `StructJson{component}` は Vec2/3/4/Quat の成分を
**一要素 JSON 配列**として生成し、`:166` の decoder がそれを `"must be a number"` で拒否する。
**vector 型の typed behavior 登録は現行 core で成立しない**(出荷 behavior に vec 使用ゼロ
のため未発覚)。**プレファブ U1 のパラメータは vec4 を使う設計であり、確実に踏む。**

やること: encode を scalar 出力に直し、往復(encode→decode→canonicalize)を全 17 型で固定。
WP358 の三者 corpus を**真の全 17 型 typed 経路**に拡張(process-local seam の 4 型を正規化)。
fingerprint への影響を確認(vec default を持つ behavior が存在しないため変化なしの見込みだが、
**見込みではなく前後比較で示す**)。
受け入れ: 修正前に Vec2 default 付き typed 登録が実際に落ちる再現 / 修正後の全 17 型往復 /
WP358 fixture 群の不変。依存: WP358。見積: 小。

### WP360(= リファクタ R3): scene の読者を単一の解決済み投影に載せ替える(第 3 版)

**§4 規則 11 の上段。レビュー 2 巡(9 + 11 指摘)。**
**v3 の判断: Generated を本 WP から完全に落とす(identity 保存のみ)。**
v2 の 11 指摘中 5 件は Generated の契約(wire union / stable_id の undo 再現 /
arena seq / Generated behavior の sentinel 意味論)に付随しており、
**その契約は実利用者であるプレファブ(WP361)で固める。**
resolver は Generated の要求に対し名前付き `generated_component_unsupported` を返す
(暗黙除外にしない)。仕様レビューはここで打ち切り、残余は §10。

#### 契約(identity 保存版)

```
ResolvedComponent {
  authoring_component_index,          // wire に見える添字は authored のまま
  source_json_exact,                  // 親版 RPC bytes の源(v2 指摘 1)
  effective_json }                    // codec 補完済みの解決値。既定値の所在は resolver 一箇所
```

- **二表現は両方必須**: `canonical_json` 一個では「親版 authored bytes の保存」と
  「解決値の単一所在」を同時に満たせない(補完すると bytes が変わり、
  生のままだと下流が再解決して既定値の所在が散る)
- **raw authority の分離は型で強制**(grep でも可視性でもなく):
  raw JSON view を返す型を別 target/interface に隔離し、
  resolver と authoring authority 以外からは**取得不能**。
  許可 target / 拒否 target を名指しした compile-negative テストを置く
  (`scenesJson()` を私有化しても `rawJson()` 迂回が残る、が v2 の実証)
- **原子的所有**: `{AuthoringSceneDocument, ResolvedScene, revision, resolver_generation}`
  を不変 pair とし、editor transaction / snapshot import / update / invalidate /
  DLL reload の全経路で prepare→publish→rollback を一体化。
  **`AfterPublication` fault point で失敗させ、全読者の revision と固有値が
  旧版へ戻る**ことを検査(Authoring だけ戻って Resolved が新版のまま、を塞ぐ)
- **SDK 境界**: validator の scan は core-private adapter へ移し、
  中立 DTO(`source_params` + provenance)を渡す。candidate 側が一度だけ canonicalize
  (active 側の canonicalize 済み値を渡すと default 変更で誤拒否する、が v2 の実証)。
  **public 登録 layout は不変 = `userpublic` diff 0**(WP358 の流儀。
  staged SDK 基盤の新設はしない —— その価格は WP358 v2 の台帳に記録済み)

#### やること

1. 契約どおりの seam + 全束縛点の載せ替え(SceneLoader / 初期 behavior attachment /
   実 DLL reload / editor transaction 後 / Camera・Light・Collider projection /
   preview / production RPC query)
2. **旧 raw 経路の物理削除**: `gamelogicreload.cpp:181` の `scenesJson()` 渡し等を
   resolved 経由に置換し、旧経路を残さない(compile-negative が再発を塞ぐ)

#### 受け入れ条件

**等価:** 親版 RPC bytes(**WP358 の流儀で固定**: named SHA / shared clone /
実 `RpcServer::run` / 出所ファイル / 実装側の再生成禁止)+
**production 配線 fixture**(`EditorRuntimeFactory` / SceneLoader / ECS / BehaviorArena
実接続。v1 の capture は runtime_query 未接続で添字 join を検査できなかった)+
golden / trace / 全数 2 回

**配線の証明(Generated 抜きで):**

- **compile-negative**: 拒否 target(Camera / reload / query)から raw view 型が
  取得できないこと
- **本番 caller の呼び出し削除変異**: resolved 経由の呼び出しを外すと
  既存 WP162 系 harness(authored schema bump の検出)が落ちること
  —— 旧 raw 経路は物理削除済みなので、外せば検証は何も読まなくなる
- Camera / Light / editor query は**実際に解決された固有値**を検査(件数でない)
- behavior 編集 round-trip 後、raw slot の正しい 1 箇所だけが変わること

**構成:** physics ON/OFF(OFF は collider の名前付きエラー)/ **RPC ON/OFF**
(OFF は named feature-disabled + server 非生成、共有 query model は両方でビルド)/
preview / ImGui ON/OFF / SKIP_DEVSTUDIO 両構成 / doclink 緑 / GPU 直列可

#### やらないこと

- **Generated 一切**(契約・注入・wire union・stable_id・arena variant は WP361 で。
  resolver の `generated_component_unsupported` だけ本 WP)
- staged SDK 基盤(価格は WP358 v2 に記録済み)

#### レビュー記録(2 巡 20 指摘の要約)

v1: 契約なしの sentinel が添字系を壊す(中心)/ 束縛点不足 / 観測定義 / allowlist /
構成 / SDK 境界。v2: canonical_json 一個の矛盾 / Generated の wire・undo・arena
(→ WP361 へ)/ raw scanner のままで通る sentinel(→ 旧経路の物理削除で置換)/
DLL fixture 経路と ABI 条件 / staged SDK 不在(→ 落とした)/ rawJson 迂回(→ 型で強制)/
原子的所有の欠落 / oracle 未固定 / RPC 行。
壊せなかった: 親版 bytes 一致の実現可能性 / D0 / XR・compiler 分岐の不在 /
出荷 4 scene の behavior 0 件(実測確認)。

依存: 無し。見積: 大。

#### コードレビュー(0bec09b = WP360)の仕分け

**削除 94 分岐の全数照合。実害 6(いずれも出荷 scene では未発火を実測確認)+
第二パーサ残存 4 + テスト感度 6。壊せなかった: first-camera 継承順序 / physics OFF /
AfterPublication rollback / 親版 fixture 一致 / D0 / 決定性。**

### WP360a: seam レビューの締め(実害 6 + 感度 + 第二パーサ残)

**§0 上段の締め。連鎖はここで打ち切り(次レビューの指摘は台帳行き)。**

**実害(各、旧版比較の再現 → 修正 → 変異で閉じる):**

1. **target なし orbit camera**: 旧受理 / 新拒否。旧意味論に戻す(controller の
   target optionality が resolver codec と Camera で食い違う二重解析も解消)
2. **rotation 省略時の up ベクトル**: project up=-Y + pos のみ transform で
   旧 -Y / 新 +Y(rotation-presence 分岐の喪失)。旧意味論に戻し、
   **rotation 省略 + 非既定 up の対照を追加**(出荷は全 camera rotation 明示のため未発火)
3. **preview の override 後検証の喪失**: yfov 文字列 override が旧 schema error /
   新受理。codec 検証を override 後に復元
4. **marker 衝突**: 外部 component の `generated:false` が project 全体を拒否。
   `generated` は boolean true のみ要求 / `origin` は型検査 + 厳密一致 /
   予約スキーマを明示 / **false・null・外部 component の否定対照**(現テストは true のみ)
5. **camera 既定値が 3 箇所**(codec 内部既定 / cameradefinition / default_config)。
   znear=5000 + project zfar=10000 の有効入力が内部既定 1000 との比較で誤拒否。
   presence-aware optional + resolver での一度だけの cross-field 検証
   (「既定値の所在は resolver 一箇所」の契約完遂)
6. **behavior の canonicalize が inactive-scene エラー保持の外**:
   無効な inactive scene が起動全体を拒否(旧は active のみ)。
   解決エラーを保持し、active attachment 時に名前付きで要求

**第二パーサの残り(R3 の完遂):**

- preview camera query の `effective_json` 再 decode → typed resolved value 直接受け
- LightContainer の JSON 再解析と既定値重複(1.0/12.5/17.5)→ typed handoff
- **Studio outliner の scene ファイル直読み** → RPC `scene_tree` から構築・更新
  (snapshot import / 構造編集後の乖離を対照に)

**テスト感度:**

- `EditorCommandService::resolved()` の**自前 fallback を削除または禁止**
  (provider 配線を消しても fallback が同じ bytes を作るため変異が素通り)
- production fixture の camera / light に **resolver でしか出ない固有値**を持たせる
- `PELican_WP360_SKIP_DEVSTUDIO` 出力に assertion を付ける
- caller deletion mutation を behavior 以外(Camera / query)にも

依存: WP360。見積: 中〜大。

### WP361: プレファブ U1 —— 単一オブジェクトの参照・展開・編集

**§4 規則 11 の上段(scene 形式・新サブシステム)。仕様レビュー + コードレビュー。見積: 大。**
**規範は `docs/design_prefabs.md`(v4、3 巡 30 指摘消化)。本節は WP としての範囲・
受け入れ・オラクルだけを定める。設計との食い違いは設計が優先。**

#### 前提(全部揃っている)

WP358(leaf 語彙)/ WP359(vec 往復)/ WP360+a(ResolvedScene seam、
Generated の席は named unsupported で確保済み、原子的所有、型封じの raw)。
Generated の契約は WP360 v2 レビューの処方箋(指摘 2〜5)を正とする。

#### 範囲(設計 v4 の U1)

1. **プレファブ文書 + registry**: `pelican.prefab` v1(StructFieldSchema 型の parameters、
   `kind:"asset"` / `kind:"object"`、tagged node `{"$param"}`)、project.json `prefabs[]`、
   parse 済み envelope からの immutable `PrefabRegistrySnapshot`
2. **scene version 2 ゲート**(expander と原子的)+ 互換 matrix
   (**v1 + prefab キーは `prefab_requires_scene_v2` で名前付き拒否** —— silent no-op 禁止)
3. **展開は resolver 内**: Generated component として、WP360 v2 処方箋どおり:
   - **wire union**: Authored branch は既存 bytes 完全一致 / Generated は
     `{stable_generated_id, source, resolved_json, editable:false}`、authored index を持たない
   - **`stable_generated_id` の決定的導出**
     `{scene stable id, AuthoringObjectId, source kind, source-local key}`。
     衝突は名前付きエラー。**set→undo→redo / object remove→undo / 無関係 component の
     前方挿入で、bytes・authored index・Generated ID・behavior seq 全比較**
   - **arena の席**: identity を `Authored | Generated` variant 化。Generated seq の
     決定的導出と editor shift の非適用
   - Generated への mutation は名前付き拒否。**Studio / ImGui の実応答取込で
     read-only 表示 + mutation UI 非生成を同一 fixture で検査**
4. **BindableProvider**(設計 v4 §2): codec の bindable-path + discriminant 条件つき
   applicability + behavior 登録由来の provider、fingerprint / generation 持ち。
   `shape:"box"` 固定の prefab で `/radius` 束縛 → `prefab_binding_inactive`
5. **SnapshotV2**(三 schema 同時)+ save read-set。closure は prefab digest +
   **behavior `{stable_name, schema_version, params_schema_fingerprint}` +
   provider fingerprint**(bytes 同一でも解決が変わる DLL default 変更を検出)
6. **studio**: instance 選択で解決値 + 出所表示、
   `set_prefab_parameter` / `unset_prefab_parameter`(再展開、failure-atomic、
   request に prefab digest + provider generation)
7. 設計 §6 の stable error code 全部(context: scene_id / instance / prefab /
   parameter / JSON pointer)

#### 受け入れ条件(要点。§4 規約 10 の形で)

- **プレファブ無し / default / 束縛あり を同じ本番ロードで実行し、実 ECS / behavior の
  解決値を検査**(WP360 の production 配線 fixture を拡張)
- 互換 matrix 4 セル(旧+v2 拒否は成立済み / 新+v1 不変 / **新+v1+prefab キー名前付き拒否** /
  新+v2 展開)を同一テスト群で
- **出荷 4 project の scene・RPC bytes・golden が完全不変**
  (prefab 0 使用のため。親版オラクルは WP354a/358/360 の流儀)
- **専用 fixture 必須**(出荷 corpus では何も観測できない): behavior 入り prefab /
  authored の前・間に Generated / discriminant 束縛の拒否 / 未使用宣言・型・範囲の負例
- undo/redo/前方挿入の identity 保存(上記 3)
- tagged node の置換順序(raw 検証 → 置換 → 具体値を codec へ)。
  **置換前の tagged node が codec に届いたら落ちる**対照
- SnapshotV2: 同一 scene bytes + 別 registry / 別 behavior fingerprint の
  `prefab_dependency_mismatch` 拒否
- matrix: physics・RPC ON/OFF / SKIP 両構成 / preview / ImGui ON/OFF。
  userpublic は **behavior 関連 diff 0**(arena は core 側)
- 全数 2 回 / doclink 緑 / GPU 直列可

#### やらないこと

多オブジェクト(§6 前提 4 件)/ unpack・一括取り込み・dry-run(U2)/
パラメータ昇格(U3)/ spawn(U4)/ optional slot・variant / HDA / attributes。

依存: WP358, WP359, WP360, WP360a。見積: **大**。

#### WP361 仕様レビュー(v1、不合格・10 件)と v2 への分割

前提の誤り 1 件(「Generated の席は named unsupported で確保済み」——実際の
`ResolvedComponent` は authored-only + authored 偽装マーカーの拒否検出器だけで、
variant は無い)+ 欠陥 10 件。**全件「直す」**。設計 v4 の矛盾 2 点
(object パラメータの default 省略 vs default 欠落エラー / 歴史化した file:line)も
design_prefabs.md 側を修正済み。v1 は上に残す。**v2 は WP361(エンジン)と
WP362(永続化 + 編集)に分割**し、一体でレビューする。

| # | v1 の壊れた点 | v2 の答え(置き場所) |
|---|---|---|
| 1 | 永続 wire 文法が未定義(project entry / scene instance / digest 対象が決定不能。規範例の default 省略が自規則違反) | WP361 §文法に厳密 schema + 正負例。省略 = required、`prefab_parameter_required` 追加(設計側も修正済み) |
| 2 | named unsupported → Generated variant の移行契約が無い(旧 detector と正規展開の 3 通りの失敗分岐) | WP361 §移行表: trusted constructor 経由・JSON マーカー非経由、detector は維持、新 code 2 件 |
| 3 | `stable_generated_id` が session-local な AuthoringObjectId 依存(snapshot import で再採番) | scene v2 の **bytes に永続 `instance_id`**。導出から AID を排除 |
| 4 | 再展開が原子的所有に合流していない(atomic unit が pair のみ / kind:object の間接依存が digest CAS を素通り) | WP362 §atomic: unit を closure まで拡張、closure generation の CAS、set/unset/undo/redo × 全 fault point |
| 5 | closure が resolver 入力(camera defaults 等)を閉じない | WP362: `resolver_inputs_fingerprint` を closure に追加 |
| 6 | provider fingerprint の生成規則・lifecycle 未定義(関数ポインタからは作れない / 登録順・reload 依存) | WP361 §provider: canonical descriptor 列挙 → 決定的直列化 → fingerprint。stable name ソート、active/candidate 明示の immutable snapshot |
| 7 | kind:object の全展開後 barrier が無い(宣言順依存になる自然実装が緑) | WP361 §resolver 4 相の固定。前方・後方・self・authored/generated 両供給の検査 |
| 8 | SnapshotV2 の wire・全 scene read-set・save baseline の受け入れ皆無 | WP362 §snapshot: 完全 wire schema、inactive-only prefab、save fault point |
| 9 | read-only と identity のオラクルが代理(UI 非生成のみ / 「全比較」の期待値未定義) | WP361: 実 RpcServer::run へ直接 mutation、操作別期待表 |
| 10 | 最終 component 合成規則と matrix の入力閉包が無い(transform 0/2 件、authored+generated 重複、`"prefab":null`) | WP361 §合成規則 + matrix はキーの「存在」判定、3 入口別走行 |

### WP361 v2: プレファブ・エンジン側 —— 文法、展開、identity、wire union

**§4 規則 11 上段。仕様(WP362 と一体)+ コードレビュー。見積: 大。**
**規範は design_prefabs.md v4(修正済み)。以下は v1 の範囲宣言に対する差分と確定契約。**

#### 文法(永続 wire。これが正で、実装はこれに合わせる)

- project.json:
  `"prefabs": [{ "name": "enemy_grunt", "path": "prefabs/enemy_grunt.prefab.json" }]`。
  `name` は文書内 `name` と一致必須(不一致は名前付きエラー)。
  **digest = prefab ファイルの raw bytes の SHA-256**(canonical 化しない —— 保存形式が正)
- scene v2 の instance:
  ```json
  { "name": "grunt_01",
    "components": [ { "name": "transform", "position": [0, 0, 0] } ],
    "prefab": { "ref": "enemy_grunt", "instance_id": "gi_7f3a",
                "parameters": { "hp": 55, "target": "player" } } }
  ```
  `instance_id` は **scene bytes に永続**(load 時採番ではない)。scene 内一意、
  重複は `prefab_instance_id_collision`。手書き v2 では作者が付ける
- parameters の値: scalar/vec は leaf 検証(WP358)。`kind:"asset"` は asset 名文字列、
  `kind:"object"` は **authored object 名文字列**(picker/rename 連動は U2)
- **default 省略 = instance 必須**。未指定は `prefab_parameter_required`
- `stable_generated_id` 導出 = `{scene stable id, instance_id, prefab 内 component 順序位置}`
  の決定的関数。**session 依存要素(AuthoringObjectId・採番 counter)を含めない**

#### Generated 移行表(WP360 detector との共存)

- expander は resolver 内部の **trusted constructor** で provenance を作る。
  JSON マーカーを経由しない
- authored payload の generated / origin マーカー検出器は**そのまま維持**
  (正規 prefab 成功と authored 偽装拒否を**同一 fixture** で。detector 削除変異で落ちる対照)
- 新 code: `prefab_generated_id_collision` / `prefab_generated_read_only`

#### resolver 4 相(順序固定。宣言順非依存)

1. 全 prefab の raw placeholder/parameter 検証・置換 → 2. 全 object の最終 component 集合
+ provenance を materialize → 3. **object index freeze 後**に kind:object 参照と
required_components を検証 → 4. codec/behavior canonicalization。
受け入れ: 前方参照・後方参照・self・参照先 required が authored 供給/generated 供給の
両方・欠落・順序反転を同一テスト群で。**object 宣言順を反転しても結果が一致**すること

#### 合成規則(最終 merged object に対して)

- transform は instance 側 authored に exactly-one:
  0 件 `prefab_instance_transform_missing` / 2 件以上 `prefab_instance_transform_duplicate`
- 非 behavior component の名前重複(authored × generated 含む)は `prefab_component_duplicate`

#### wire union と read-only(実配線オラクル)

- get_components: Authored branch は**親版 bytes 完全一致**(WP360 fixture 続用)。
  Generated branch は `{stable_generated_id, source: {prefab, ref, instance_id, digest},
  resolved_json, editable: false}`、authored index を持たない
- **実 `RpcServer::run` へ Generated 宛て `set_component_value` を直接送る**:
  `prefab_generated_read_only`、scene bytes・revision・ECS・arena 全て不変
- Studio/ImGui は実応答取込で read-only 表示 + mutation UI 非生成(補助条件に格下げ)

#### identity 期待表(v1 の「全比較」を置換)

| 操作 | authored index | Generated ID | behavior seq |
|---|---|---|---|
| set→undo→redo | set 後 = redo 後 | 同 | 同 |
| object remove→undo | remove 前 = undo 後 | 同 | 同 |
| 無関係 authored の前方挿入 | **期待量だけ変化** | **不変** | **不変** |

+ **採番履歴の異なる 2 session**(片方で object 作成・削除後に import)で
Generated ID・seq・RPC bytes 一致。arena identity は `Authored | Generated` variant、
Generated seq は authored と衝突しない決定的符号化(衝突は名前付きエラー)

#### BindableProvider(fingerprint の生成規則)

- bindable path / 型 / 種別 / discriminant 条件を**関数ポインタでなく canonical
  descriptor(data)として列挙**し、stable name ソートの決定的直列化から fingerprint
- resolver へは active/candidate owner を明示した immutable snapshot で渡す
- 受け入れ: applicability descriptor の変更で fingerprint が変わる(semantic mutation)/
  登録順反転で不変 / semantic no-op reload で不変

#### matrix と入口

- 判定は `prefab` **キーの存在**(`null`・不正型も v1 scene ではまず
  `prefab_requires_scene_v2`)
- 入口別に走らせる: engine load / studio offline open。
  **RPC snapshot(V1)は prefab 使用 scene で名前付き拒否**(V2 は WP362)
- 出荷 4 project の scene・RPC bytes・golden 完全不変 / 構成 matrix・全数 2 回は v1 どおり

### WP362: プレファブ永続化と編集 —— SnapshotV2、原子的合流、set/unset

**WP361 に依存。§4 規則 11 上段。見積: 中〜大。**

#### SnapshotV2 wire(完全定義)

- request / response / import の三 struct を V2 化。field・型・canonical ordering・
  digest algorithm(scene / prefab とも raw bytes SHA-256)・V1/V2 routing・
  validation order を実装前に本文へ(strict parser は現行 V1 の流儀)
- closure = 文書中**全 scene**(inactive 含む)の prefab read-set +
  prefab digest + behavior `{stable_name, schema_version, params_schema_fingerprint}` +
  provider fingerprint(WP361 の canonical descriptor 由来) +
  **`resolver_inputs_fingerprint`**(camera defaults 等、resolver へ渡した project 入力の
  canonical digest)
- 受け入れ: inactive scene だけが prefab を使う文書 / unused registry entry /
  camera defaults だけ違う 2 project(scene bytes 同一)で `prefab_dependency_mismatch` /
  V1 は prefab 非使用 scene で従来 bytes 不変

#### save baseline

- `save_scene` baseline を `{scene_digest, prefab read-set}` の不可分 snapshot に。
  prefab ファイルの save 前・途中変更で名前付き拒否(fault point 別)

#### set/unset_prefab_parameter と原子的合流

- atomic unit を `{AuthoringSceneDocument, ResolvedScene, PrefabResolutionClosure,
  registry_generation, provider_generation}` へ拡張。request は
  **closure generation の CAS**(対象 prefab digest 単独では kind:object の間接依存
  —— 参照先 B の prefab 変更 —— を閉じられない。これを負例で)
- provider generation の owner と publication 順序を明記(reload 中の旧 generation 窓を
  受け入れ条件で検査。`prefab_provider_stale`)
- set / unset / undo / redo × Prepare / Publish / AfterPublication 全 fault point で
  authoring・resolved・ECS・arena・closure が同じ旧版へ戻る
- `unset` の意味: default ありは default へ / **required(default 省略)への unset は
  `prefab_parameter_required` で拒否**
- 表示: 解決値 + 出所(default / 束縛)。WP361 の read-only 表示に出所列を追加

依存: WP362 は WP361 に依存。やらないこと(両 WP 共通): v1 の「やらないこと」+ U2〜U4 全部。

#### WP361/362 仕様レビュー第 2 巡(不合格・10 件)と v3 差分

第 2 巡は「10 指摘を閉じたか」の検証。判定: 閉鎖 1 / 部分 6 / 未閉鎖 3、
加えて v2 自身が成立不能条件 2 件(session 跨ぎ RPC bytes 一致 / 宣言順反転の全結果一致)と
WP 境界欠陥 2 件(WP361 単独出荷不能 / wire から編集情報を復元不能)を持ち込んだ。
**全件「直す」。以下が v3 差分で、v2 本文と矛盾する場合は v3 が優先する。**
第 3 巡を最終とし、以降の指摘は台帳行き。

##### (1) session 跨ぎ比較の成立化

「RPC bytes 一致」を撤回。比較は **Generated branch の正規化部分木に限定**し、
`scene_revision`・AuthoringObjectId・entity id・arena handle/owner を明示的に除外する。
否定対照: 同一テスト内で `instance_id` だけを変え、Generated ID と解決値が変わること。

##### (2) identity lifecycle の全定義

- `instance_id` 字句: `[A-Za-z0-9_-]{1,64}`。割当 authority は v2 手書き時は作者
- **Generated ID のキーは順序位置ではなく `{component 名, 同名序数}`**
  (prefab 先頭への component 挿入で既存 ID が再利用されない。
  否定対照: prefab に collider を前挿入 → sprite/behavior の ID 不変)
- 導出は domain-separated + **length-prefixed** の固定アルゴリズム(区切り曖昧性なし)
- journal の object 複製/spawn が prefab キーを持つ object を対象にした場合:
  **op が新 instance_id を記録**する(undo/redo 再生が決定的)。記録なし複製は
  再解決時に `prefab_instance_id_collision`
- scene 間 copy は target で新 ID / **unpack は identity 断絶**(Generated ID は消え、
  authored 通常 component になる)と宣言(U2 実装、契約は今固定)

##### (3) 順序非依存の主張の限定

「宣言順反転で結果一致」を撤回し、**kind:object 参照解決と required_components 判定が
stable identity で対応付けて一致**することに限定する。
**camera の先勝ち継承(最初の camera の解決結果が後続の default になる)は既存意味論として
維持**し、既存固定テストで担保する(移行しない)。4 相化は placeholder IR・全 object
materialization・freeze・検証・canonicalization の別 pass を要する改修であることを
規模として認める(resolvedscene.cpp の一走査への条件追加では書けない)。

##### (4) 最終 component 全順序と behavior 実行順

- 最終配列 = **authored(著作順)→ generated(prefab 文書順)**
- behavior 実行順 = object 宣言順 → 同一 object 内は上記の最終配列順
- Generated seq 符号化 = `(1<<63) | (object_index<<32) | generated_ordinal`
  (authored と domain 分離。generated_ordinal は prefab 文書順の序数 ——
  authored component の挿入で不変)
- 否定対照: **解決値が同一でも callback trace の順序が違えば落ちる**テスト

##### (5) 文法の表化と Studio 条件の実体化

- project entry / prefab 文書 / scene instance の**全 field 表**
  (必須性・型・unknown field は拒否(現行 strict parser の流儀)・path は project root
  相対のみ、絶対と `..` は拒否・検証順)を WP361 本文として確定
- `prefab_name_mismatch` を error catalog に追加
- Studio offline 条件 = project.json だけでなく**設定された scene と全 prefab を
  本番 leaf resolver で開き**、名前付き結果と実展開値を検査する

##### (6) SnapshotV2 wire の現物化

- export request / export response / import request / import result の
  **4 形すべての JSON 現物例**・必須/禁止 field・canonical ordering を WP362 本文に書く
  (「実装前に本文へ」の TODO 形式を廃止)
- digest の byte domain を固定: snapshot scene digest = 現行どおり
  `semantic_scene_bytes` / prefab digest = disk raw bytes / save baseline = disk bytes
- **V1 の適用範囲 = 「scene format v1 かつ prefab キーなし」**と明記

##### (7) resolver 入力閉包の型化と主張の境界

- `ResolverInputSnapshotV1` を型として定義: **解決に入る project 入力の全列挙**
  (projection defaults・sprite defaults)+ canonical float encoding。
  field 単独変更ごとの否定対照
- **主張の境界を明記**: closure が保証するのは「同じ closure → 同じ ResolvedScene」。
  camera `up`・window size 等の**描画時 project 設定は snapshot 契約の外**
  (snapshot は scene を project 間で運ぶもので、受け側 project の描画設定が
  適用されるのは正しい挙動)。この境界自体を本文に書く

##### (8) provider fingerprint の byte 単位固定

- descriptor record の全 field・**全内部 sort key**(path 辞書順・discriminant 値
  ソート・enum/range も正規順)・encoding(UTF-8 length-prefixed 文字列、
  IEEE754 LE double)・SHA-256 を固定
- generation は **owner-pinned の prepare → publish → rollback 状態機械**として
  atomic unit と同時に定義(reload 中の旧 generation 窓は「publish まで旧が有効」)

##### (9) 出荷ゲート(WP361 単独期間の非破壊)

WP361 単独では prefab 使用文書の**永続化を全入口で名前付き拒否**する:
`save_scene` と snapshot(V1/V2 とも)は `prefab_persistence_unsupported`(新 code)。
WP361 の出荷状態 = 「v2 scene の prefab は load・表示・in-memory 編集(authored 側)・
undo/redo まで。保存と snapshot は WP362 で解禁」。
これを受け入れ条件に含める(拒否が実 RPC で観測されること)。

##### (10) wire に編集情報を先行搭載

WP361 の get_components Generated branch / instance 表現に:
`parameters: [{name, value_resolved, source: "default"|"override", value_authored?}]` +
closure/provider generation を含める(**同値 default と同値 override を同一テストで
識別** —— resolved_json だけでは区別不能な反例が根拠)。
WP362 は set/unset の selector・CAS token・result/error・journal inverse を
この wire の上に定義する。

error catalog 追加(design 側にも反映): `prefab_name_mismatch` /
`prefab_persistence_unsupported`。

#### WP361/362 仕様レビュー第 3 巡(不合格・11 件)と v4 —— 決定と現物

第 3 巡の内訳: 本当に壊れているもの 5 件(同名挿入の ID 再利用 / seq 符号化 /
closure の scene digest 欠落 / 否定対照の期待値が逆 / codec default が指紋の外)と、
「表を書け」と命じたまま表が無いもの 6 件。**v4 で全件を閉じ、レビューはここで
打ち切る(以降の指摘はコードレビューと台帳で受ける)。v4 は v2/v3 に優先する。**

##### 決定(第 3 巡の破綻 5 件への答え)

- **否定対照の修正**: `instance_id` 単独変更 → Generated ID は変化・`resolved_json` は
  **不変**。解決値の感度は parameter 値だけを変える別対照で検査する
- **component key**: prefab の component record に **省略可 `id`**(字句は instance_id と
  同じ)を導入。**key = `id` があれば `id`、なければ `name`**。key 重複は
  `prefab_component_key_duplicate`(同名 behavior を 2 件置くには `id` が必須になる)。
  これで合法な同名挿入でも既存 ID は動かない
- **Generated ID 導出(byte 単位で固定)**:
  `gid = "gid_" + lowercase-hex(SHA-256(D ∥ LP(instance_id) ∥ LP(component_key)) の先頭 16 byte)`。
  `LP(s) = u32-LE(UTF-8 byte 長) ∥ UTF-8 bytes`、`D = LP("pelican.prefab.gid.v1")`。
  wire 型は string。**scene 成分は含めない**(gid は常に scene 文脈で使われ、
  scene 内一意性は instance_id 一意 × key 一意から従う)。導出衝突検出時は名前付き
- **seq は order key であって identity ではない**: Generated は authored と**同一 domain** の
  `attachment_seq = (object_index << 32) | merged_component_index`
  (merged 配列 = authored 著作順 → generated 文書順)。既存の「seq 昇順 = 実行順」契約は
  無変更で宣言順を誘導し、domain bit も 2^53 超過も無い。
  **identity 期待表の「behavior seq 不変」列は撤回**し、「Generated ID / arena identity
  (variant が gid を保持)不変」に置換する。seq は authored 挿入で正当にずれる
  (bytes → seq が決定的関数であることが担保。undo/redo・session 跨ぎは bytes 同一で従う)
- **closure に文書中全 scene の semantic digest を追加**(parameter override 反例を閉じる)。
  保証文は「同じ closure → 同じ semantic projection(revision・resolver_generation・
  AuthoringObjectId・entity id・arena handle/owner を除いた ResolvedScene の正規化像)」
- **unused registry entry は closure の外**: 参照されない prefab entry の追加・削除では
  import は成功し mismatch しない —— これを肯定 assertion として受け入れに置く
- **codec semantics を指紋に入れる**: 7 つの component codec それぞれが
  {field 存在規則, 省略時 default の canonical JSON} を canonical descriptor として
  データで公開し、provider fingerprint に合流する。
  **default 値だけ変える mutation で fingerprint が変わる**対照を要求(behavior 側は
  既に encoded defaults を含む —— 同水準に揃える)

##### 文法表(WP361 本文。これが正)

**project.json `prefabs[]` entry**(unknown field は `prefab_registry_invalid`):

| field | 型 | 必須 | 規則 |
|---|---|---|---|
| name | string | ✔ | `^[A-Za-z0-9_-]{1,64}$`。配列内一意(`prefab_duplicate_name`)。文書内 name と一致(`prefab_name_mismatch`) |
| path | string | ✔ | project root 相対・`/` 区切り。先頭 `/`・`..` 成分・drive letter は `prefab_path_invalid` |

**prefab 文書 envelope**(unknown field は `prefab_document_invalid`):

| field | 型 | 必須 | 規則 |
|---|---|---|---|
| schema | string | ✔ | `"pelican.prefab"` のみ |
| version | number | ✔ | 1 のみ(`prefab_version_unsupported`) |
| name | string | ✔ | registry name と同じ字句 |
| parameters | array | 省略可 | 下記 record。name 一意(`prefab_parameter_duplicate`) |
| components | array | ✔ 非空 | 下記 record |

**parameter record**(`type` と `kind` は**排他で一方必須**、違反は `prefab_document_invalid`):

| 形 | field | 必須 | 規則 |
|---|---|---|---|
| 値 | name / type / default / range / enum_values | name,type ✔ | type は leaf 17 語彙。default 省略 = instance 必須。range は数値型のみ。**enum_values は type=="enum" のとき必須・それ以外は禁止**(leaf の enum_values_required と strict unknown-field の衝突を解く —— 実装の UNMET 報告 1 件への答え) |
| asset | name / kind:"asset" / asset_kind / default | name,kind,asset_kind ✔ | default は asset 名 string |
| object | name / kind:"object" / required_components | name,kind ✔ | **default 禁止**(scene 外から scene 内 object は指せない)= 常に instance 必須 |

**component record**: `name` ✔(component 語彙)+ 省略可 `id` + body passthrough。
key 重複 `prefab_component_key_duplicate` / transform は `prefab_transform_forbidden`。
**tagged node** `{"$param": "<name>"}` は**その 1 field のみ**(追加 field は
`prefab_document_invalid`)。未宣言参照 `prefab_parameter_unknown` /
全宣言 parameter はどこかで使用(`prefab_parameter_unused`)。
`name`・`id`・discriminant 位置への tagged node は provider 判定で
`prefab_binding_not_bindable` / `prefab_binding_inactive`。

**scene instance block `"prefab"`**(unknown field は `prefab_instance_invalid`):

| field | 型 | 必須 | 規則 |
|---|---|---|---|
| ref | string | ✔ | registry に存在(`prefab_not_found`) |
| instance_id | string | ✔ | `^[A-Za-z0-9_-]{1,64}$` 違反 `prefab_instance_id_invalid` / scene 内一意 `prefab_instance_id_collision` |
| parameters | object | 省略可 | key は宣言名(`prefab_parameter_unknown`)/ 値は leaf 検証(`prefab_parameter_type`)/ required 欠落 `prefab_parameter_required` |

**検証順**: ① scene version gate → ② registry 構文 → ③ 文書構文(envelope →
parameters → components → tagged 閉包)→ ④ instance 構文(ref → instance_id →
parameters)→ ⑤ 束縛妥当性(provider)→ ⑥ 合成規則 → ⑦ object-ref(4 相の 3)→
⑧ codec/behavior canonicalization。各段で最初の違反の code を返す。

**journal の複製/spawn**: 対象 object が prefab キーを持つ場合、canonical op が
**`new_instance_id` field を記録**する(再生はこの値を使う。決定的)。
記録の無い旧形式は再解決時 `prefab_instance_id_collision`。

##### RPC wire(WP361 本文)

- get_components の Generated branch(session 正規化の除外一覧に
  `closure_generation` / `provider_generation` を追加):
  ```json
  { "generated": { "stable_generated_id": "gid_0123456789abcdef0123456789abcdef",
      "source": { "prefab": "enemy_grunt", "instance_id": "gi_7f3a",
                  "component_key": "sprite_view",
                  "digest": { "algorithm": "sha256", "hex": "0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345" } },
      "resolved_json": { "name": "sprite_view", "texture": "white" },
      "editable": false } }
  ```
- instance を持つ object entry に `prefab_instance`(parameters は**宣言順**):
  ```json
  { "prefab_instance": { "ref": "enemy_grunt", "instance_id": "gi_7f3a",
      "closure_generation": 3, "provider_generation": 1,
      "parameters": [
        { "name": "hp", "value_resolved": 55, "source": "override", "value_authored": 55 },
        { "name": "tint", "value_resolved": [1, 1, 1, 1], "source": "default" } ] } }
  ```
  (`value_authored` は override 時のみ。同値 default/override は `source` で識別)
- **set_component_value の selector**: 省略可 `generated_id` を追加し
  `component_slot` と**排他**(両方/どちらも無しは op 不正)。
  precedence: ①構文 → ② gid 解決(未知は not-found 系 detail)→
  ③ `prefab_generated_read_only`

##### SnapshotV2 wire(WP362 本文。4 形の現物)

routing: `schema_version` の値で分岐。**V1 の適用範囲 = scene format v1 かつ prefab
キーなし**。prefab 使用文書への V1 export/import は恒久的に
`prefab_persistence_unsupported`。数値は非負整数 ≤2^53−1。unknown field 拒否。
配列は記載の sort 順。key 順は表順。

- **ExportSceneSnapshotRequestV2**: `{"schema_version":2,"allow_pending":false}`
  (field 表は V1 と同一、version のみ 2)
- **ExportSceneSnapshotResponseV2** = V1 の全 field(schema_version:2)+ `prefab_closure`:
  ```json
  { "prefab_closure": {
      "scenes": [ { "scene_id": "main", "digest": { "algorithm": "sha256", "hex": "0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345" } } ],
      "prefabs": [ { "name": "enemy_grunt", "digest": { "algorithm": "sha256", "hex": "0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345" } } ],
      "behaviors": [ { "stable_name": "grunt_ai", "schema_version": 1, "params_schema_fingerprint": "585b..." } ],
      "provider_fingerprint": { "algorithm": "sha256", "hex": "0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345" },
      "resolver_inputs_fingerprint": { "algorithm": "sha256", "hex": "0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345" } } }
  ```
  `scenes` = **文書中全 scene** の semantic digest(scene_id 昇順)/
  `prefabs` = read-set のみ(name 昇順)/ `behaviors` = stable_name 昇順。
  scene digest の byte domain = 現行どおり semantic_scene_bytes(UTF-8)/
  prefab digest = disk raw bytes / save baseline = disk bytes
- **ImportSceneSnapshotRequestV2** = V1 の全 field(schema_version:2)+ 同形の
  `prefab_closure`。検証順は V1 の 5 段に **⑤' closure 照合
  (`prefab_dependency_mismatch`、要素単位の detail)**を ④ と ⑤(scene_not_found)の
  間に挿入
- **ImportSceneSnapshotResult** は V1 と同形(変更なし、と明記)

##### ResolverInputSnapshotV1(WP362 本文)

encoding を固定: field ごとに `LP(field名) ∥ presence byte(optional のみ、0x00/0x01)∥
値 bytes`。f32 は IEEE754 LE・`-0 → +0` 正規化・NaN は名前付きエラー。enum は宣言値の
u32-LE。hash = SHA-256、domain `LP("pelican.resolver.inputs.v1")`。
**field 集合 = `ResolvedSceneDefaults` の全 member を宣言順で**。恣意を残さないため
**mirror test(WP358 の pair-swap の流儀)**: member を列挙する静的検査が、指紋に
入っていない member の追加で**コンパイル時または実行時に落ちる**こと。
主張の境界(v3 (7))は維持: camera up・window size 等の描画時 project 設定は契約の外。

##### set/unset の canonical op(WP362 本文)

```json
{ "op": "set_prefab_parameter", "object": 12, "parameter": "hp", "value": 55 }
```
inverse = 旧 override があれば旧値の set / 旧が default なら
`{ "op": "unset_prefab_parameter", "object": 12, "parameter": "hp" }`(逆も同様)。
request の外側 CAS = `{scene_revision, closure_generation, provider_generation}`
(不一致は各 `stale_revision` / `prefab_dependency_mismatch` / `prefab_provider_stale`)。
required parameter への unset は `prefab_parameter_required` で拒否(v3 どおり)。

error catalog 追加(design 側にも反映): `prefab_path_invalid` /
`prefab_registry_invalid` / `prefab_document_invalid` / `prefab_instance_invalid` /
`prefab_instance_id_invalid` / `prefab_component_key_duplicate`。
本節の JSON 例は全て実 parser で parse 可能であること(fixture の parse gate に使う)。

##### 実装の UNMET 1 件への裁定(v4 追補)

実装が正しく検出した表の欠落: 値 parameter の表が `enum_values` を許さないため、
leaf の enum 型(非空 `enum_values` 必須)が表現不能だった。上表を修正し、
enum parameter の正例(展開まで)と、enum 以外での `enum_values` 指定・
enum での欠落の負例(いずれも `prefab_document_invalid`)を受け入れに追加する。

#### WP361 コードレビュー(reject・12 件)の仕分けと WP361a

出荷 4 project(behavior 0・prefab 0)は直接踏めないことをレビュー自身が実査済み。
仕分け: **直す 11 件 = WP361a** / **台帳だけ 1 件**(#9 の codec 記述子完全統一 ——
schema・provider・decoder を同一データから生成する件は R5(型付き構築 factory)と
同根の大工事。WP361a では light の三重矛盾の解消と provider を decoder の真実に
合わせる最小整合まで)。**連鎖はここで打ち切り。WP361a のレビュー指摘は台帳行き。**

### WP361a: WP361 修正パッケージ —— arena 生命線、第二経路の除去、ゲートの穴

**§4 規則 11 上段。コードレビューのみ(仕様源は上記 12 件)。見積: 中〜大。**

1. **lifecycle と arena の合流**(#1): spawn / destroy→undo / restore で前後の
   resolved behavior attachment を差分化し、arena の prepare/publish/rollback に参加。
   受け入れ: Generated behavior 持ち prefab の spawn 後に callback が実際に走る /
   destroy→undo 後も走る(**fake target でなく production adapter 経由**)
2. **構造編集後の index rebase**(#2): object 挿入・削除の transaction ごとに
   next ResolvedScene から全 attachment の object/component index・seq を再構築。
   受け入れ: レビューの再現(先頭 spawn → 新 behavior 追加)で collision 誤拒否が消え、
   callback 順が fresh load と一致(**取り消したら落ちる**)
3. **seq の公開値一本化**(#3): journal・arena・RPC が同一 transaction から導出した
   同じ値を公開。query_journal と get_components の一致テスト
4. **Studio offline の第二経路除去**(#4): provider descriptor を入力に取る純粋
   prefab resolution leaf を `pelican_project` に置き、engine/Studio 両方が使う。
   scheme 付き path(`project://` 等)は registry parser で明示拒否。
   受け入れ: `shape:"box"` の `/radius` 束縛 prefab を Studio が engine と同じ
   `prefab_binding_inactive` で拒否 / **同一文書の engine/Studio 展開結果一致テスト**
5. **collision の型付き伝搬**(#5): PrefabErrorCode を projection error に保持し
   RPC catalog まで通す。実衝突(`new_instance_id` が既存と同値)のテスト
6. **selector の共有 parser 化**(#6): 手前の手書き pre-parser を廃し、request 全体を
   既存 edit parser で構文検証 → selector AST → gid 解決 → read-only の順。
   `{"op":42}` は invalid_params / 不正 JSON Pointer は read-only より先 /
   preview 入口(open/update_preview)にも同じ gate
7. **persistence gate の穴 3 つ**(#7): SnapshotV1 は「format v1 かつ prefab 無し」を
   formatVersion まで見る / incoming payload の prefab を version 判定より先に
   `prefab_persistence_unsupported` / 下位 `importSceneDocument` にも同じ named gate
8. **fingerprint の byte 準拠**(#8): JSON dump を廃し、v4 の型 tag +
   scalar(IEEE754 LE)/vector/string(LP、引用符なし)binary encoder。
   **固定 digest の独立 oracle test**(手計算 bytes → 期待 hex)
9. **light の三重矛盾の最小整合**(#9 の一部): provider の light descriptor を
   実 decoder の真実(required/default)に一致させ、相互検証テストを置く。
   完全統一は台帳(下記)
10. **behavior field 名の RFC6901 escape**(#10): `aim.target` は literal key、
    `a/b` は `~1` escape。往復テスト
11. **provider fallback の削除**(#11): global 登録からの黙った補完を廃し、
    prefab 文書には non-zero provider snapshot を必須化(欠落は named)。
    generation は成功 publish と同時に commit(失敗時に増えない)
12. **asset_kind の実効化**(#12): asset descriptor に subtype を持たせ完全一致。
    model パラメータ → texture socket は named 拒否

テスト感度の是正(レビュー §テスト感度の全件):
- lifecycle 系は production adapter を通す(fake target 禁止)
- camera 先勝ちの fixture に **Generated camera を実際に置く**
- provider default の対照は descriptor 直接変更でなく **codec default 変更 →
  resolved value 変化**の実配線
- real RpcServer テストに `.edit` / `.runtime_query` を配線し、authored edit が
  変化する肯定対照を並べる
- Studio outliner は engine resolver との一致比較

台帳だけ(WP にしない):
- codec 記述子の完全統一(schema/provider/decoder 単一ソース)= R5 と同時に設計
- flat/xr 実機差・prefab 入り eval/render preview の網羅は WP362 の受け入れで拾う

依存: WP361。連鎖打ち切り宣言: WP361a の §10 指摘は台帳行き。

#### WP361a コードレビュー(受理不可・9 件)の仕分けと、打ち切り宣言の一度限りの延長

宣言(「WP361a の指摘は台帳行き」)に対する裁定: 9 件のうち #1〜4 は WP361a の
修理自身が持ち込んだ**機能後退**であり、うち 2 件は致命(非アクティブ scene への
lifecycle 編集が現行 scene の runtime を破壊 / `onDestroy` が entity 死後に呼ばれ
公開契約 08_gameplay.md:269 に反する)。打ち切りの趣旨は指摘の無限 WP 化の防止であって、
既知の致命後退の出荷ではない。**よって一度だけ延長し WP361b(後退の修復に限定)を
最終とする。WP361b のレビュー指摘は例外なく台帳のみ。**

仕分け:
- **直す(WP361b)**: #1 非 current scene の lifecycle adapter 生成(current 限定に
  し、複数 scene batch の新設拒否も撤回)/ #2 pre-destroy barrier(ECS publish 前に
  逆順 onDestroy)/ #3 identity plan を candidate から一度だけ作り journal・arena・
  RPC で共有(set-params は既存 identity 保持)/ #4 lifecycle を単一 aggregate
  participant に(parent-first staged bindings、同一 batch の parent→child spawn)/
  #7 scheme 拒否を RFC 文法(`^[A-Za-z][A-Za-z0-9+.-]*:`)に
- **WP362 で閉じる → 実装は取りこぼし、WP361b へ移管**: #5 shared selector parser の「request 全体」構文検査
  (WP362 が同じ parser に set/unset を足すため、そこで operation grammar の
  副作用なし syntax leaf を通す。malformed spawn + valid gid の否定対照込み)
- **台帳のみ**: #6 Studio offline の behavior binding 乖離(behavior schema を
  project metadata / RPC catalog 経由で pure leaf へ渡す設計が要る。U2 の
  picker/inventory と同時に)/ #8 collider radius 0.5 の 5 箇所所在
  (canonical default の単一データ化 = R5・#9 完全統一と同件)
- テスト感度 5 件は WP361b の受け入れに織り込む(production factory 経由の
  lifecycle テスト / 同一 batch fixture / real RPC の .edit・.runtime_query 配線と
  authored 肯定対照 / persistence prescan に噛む fixture / fingerprint oracle の
  vector・string branch 拡張)

### WP361b: WP361a 後退の修復(最終。これ以降の指摘は無条件で台帳)

**§4 規則 11 上段。コードレビューのみ。見積: 中。範囲は上記 5 件 + テスト感度 5 件に
限定し、それ以外に触れない。**

受け入れ(全て「取り消したら落ちる」):
1. `secondary` scene への spawn/destroy で `main` の runtime snapshot
   (camera・light・physics・behavior attachment 全列挙)が完全不変。
   複数 scene lifecycle を含む batch が受理され、各 scene の authoring に適用される
   (current scene 分だけが runtime adapter を持つ)
2. onDestroy 内で `ctx.self()` の isAlive を読む behavior fixture が
   destroy transaction で **true** を観測する(現状 false になることの再現から)
3. 「先頭 spawn + 既存 object への behavior add」を**同一 operations** に入れ、
   journal・arena・RPC の handle と seq が三者一致。
   「set params + 無関係 spawn」で既存 behavior の onDestroy/onInit が**走らない**
   (in-place 維持)
4. 同一 batch の parent→child spawn が成功し、child の runtime binding が
   parent entity を正しく指す
5. `project:foo` / `https:unit.json` が `prefab_path_invalid`
6. lifecycle テストは production factory(editorruntimefactory)経由。
   factory の lifecycle 分岐を削ると落ちること
7. real RpcServer fixture に `.edit` / `.runtime_query` を配線し、同一 fixture 内で
   authored edit の成功(肯定)と Generated 拒否(否定)を並べる
8. fingerprint oracle に vector と string の手計算 bytes を追加
   (旧 JSON-dump に戻すと落ちる)
9. (#5 移管)selector 解決前に**全 operation** を副作用なし syntax leaf で検査。
   malformed spawn + valid gid の request が invalid_params になる(read_only より先)

依存: WP361a、WP362(同じ transaction 機構を触るため **WP362 マージ後に投入**)。

#### WP362 統合失敗(GPU 直列で 2 件)と回収内修正

エージェントは GPU テストを走らせない規約のため、統合側で初めて割れた:
`behavior_dll_reload_pending_recovery` と `inspector edits unnamed parented …` の 2 件、
両走で決定的に再現。原因は resolvedscene.cpp の closure 構築が **文書中の全 behavior** を
provider snapshot に要求し、不在を fatal にしたこと。設計 §4 が要求するのは
「**prefab 内 behavior**」の tuple のみであり、authored behavior の「DLL 不在 →
pending → 回復」(二相活性化)は既存の公開契約。修正: 列挙を Generated(prefab 由来)
behavior に限定。authored は pending のまま / prefab 由来の不在は named fatal を維持。
これは WP362 の閉ループ内の回収であり連鎖には数えない。

#### プレファブ U1 完成宣言(2026-08-30)

WP361(b6b4666)→ WP361a(2d0e0ce)→ WP362(直前 + 5fd8923)→ WP361b(c4f2287)。
統合: 全数 1433 ×2 緑(GPU 直列込み・PELICAN_TEST_PROJECTS_DIR 付き)、doclink 2341 ok。
設計 v4 の U1 出荷単位が全機能で成立: 文書・registry・v2 ゲート・4 相 resolver・
Generated identity(gid)・wire union・read-only 表示・SnapshotV2 closure・
save baseline・set/unset(3 重 CAS・12 fault point)・出所表示。
出荷 4 project は全経緯を通じて byte 不変。
残余は台帳の既存行(Studio behavior binding 乖離 / default 単一ソース化 = R5 同件 /
U2〜U4)。WP361b の §10 レビューの指摘は宣言どおり無条件で台帳のみ。

#### WP361b コードレビュー(7 件)→ revert して一度でやり直す(WP361b v2)

WP361b の §10 で実害 5 件(うち #2 は **Inspector の behavior 編集が実 producer の
`attachment_handle` を新 gate が拒否して死ぬ** —— 検証済み: 新 whitelist に無い /
inspector.cpp:780 は付ける / 深層 parser :710 は許可)+ テスト感度 2 件。
台帳のみ宣言の再延長はしない。**スキル §0 の規定どおり c4f2287 を revert(6c68181)し、
一度でやり直す。** U1 完成宣言は WP361b v2 の緑まで保留に訂正する。

7 件の記録(v2 の仕様源):
1. pre-destroy barrier が fallible prepare の途中で発火。rollback は onDestroy を
   なかったことにできず、retry で二重 onDestroy(onDestroy→onUpdate→onDestroy)。
   deferred queue(ctx.createObject/removeObject)も未 checkpoint
2. 新 syntax leaf が `attachment_handle` を拒否 → 実 Inspector(ImGui/Studio 両方)の
   behavior 編集が invalid_params。旧 whitelist(:710)との二重文法
3. identity plan が operation ごと、consumer は final attachment ごと →
   「同一 behavior へ set×2 + 無関係 spawn」の合法 batch が unmatched で失敗。
   set 後 destroy も同様
4. inverse に forward candidate の seq をコピー → 前方 spawn + set の undo が
   stale 判定で失敗
5. 合法な seq 0(object 0, component 0)を allocator failure と誤認(既存欠陥・未修復)
6. current 側の camera/light/physics 肯定対照が無い(prepareSpecials 削除でも緑)
7. 構文検査テストが spawn 一種のみ(表駆動 + 実 producer 形の肯定例が要る)

### WP361b v2: 後退修復のやり直し(revert 済みの上に一度で)

**§4 規則 11 上段。見積: 大。仕様 = WP361b の 9 条件(そのまま有効)+ 以下の
アーキテクチャ処方(レビューの直し方を採用)。このレビューの指摘は無条件台帳。**

A. **identity plan は operation 列からでなく final resolved scene から構築**する。
   key = (object_id, final authored origin)。反復 set は同一 handle/seq を確認して
   1 件に統合。final consumer の無い entry は runtime plan に入れない。
   **undo/redo を含む各 execution で next_resolved から再構築**(inverse への
   forward seq コピー禁止)
B. **point of no return**: 全 fallible prepare と fault point の後に、
   onDestroy barrier + noexcept publication だけを置く。deferred queue は
   checkpoint し、barrier 前の rollback では巻き戻す。
   受け入れ: 「set(失敗する)+ destroy」batch → rollback → retry で
   **onDestroy がちょうど 1 回**(callback trace で検査)
C. **文法は単一 AST/parser**: 手前 leaf と preparer の二重 whitelist を廃止。
   `attachment_handle` は公開 CAS として維持(黙って消さず、stale なら named)。
   受け入れ: **実 producer(inspectormodel / imgui inspector)の生成する request
   そのもの**を通す肯定対照 + 全 public operation の表駆動 malformed × valid gid
D. **seq 0 は sentinel にしない**: 欠落は optional / field 存在で表現。
   object 0・component 0 への add が通る対照
E. current 側にも rich payload(camera/light/collider)の肯定対照 + destroy 後の
   孤児検査(prepareSpecials 削除で落ちること)

依存: WP361a、WP362(revert 6c68181 の上)。

#### U1 完成宣言の確定(WP361b v2 統合緑)

WP361b v2(7040f23)統合: 全数 1434 ×2 緑(GPU 直列込み)、doclink 2341 ok。
revert(6c68181)を挟んだ経緯ごと記録: WP361b 初版は Inspector の実 request
(`attachment_handle`)を新 gate が拒否する等 5 件で不合格 → パッチでなく revert +
やり直し(処方 A〜E)。保留していた完成宣言はこれで確定。
WP361b v2 の §10 指摘は無条件で台帳のみ(打ち切り最終)。

#### WP361b v2 コードレビュー(不合格・7 件、致命 1)→ revert(2 度目)と系譜の打ち切り

v2 も不合格: **致命 1 件 = 同一 edit の複数 destroy で、preflight→rearm 経路が
prepare 時の古い row を保持したまま最終 publish し、破棄済み component
(`std::string` 持ち)を二重 move する UB**(出荷 example の DamagedHelmet+sponza の
組で具体再現、backfill で row が動くのに token が追随しない)。他: 非 behavior の
挿入で既存 behavior が破棄・再生成 / sequential には合法な batch を base document
基準の正規化が拒否 / **light-only object(出荷 example に 14 個・vrm_xr に 1 個)の
destroy が runtime binding 必須化で失敗** / reparent の preserve enum が selector より
後 / 「実 producer」テストが producer を呼んでいない / rich fixture が実値を見ない。
宣言どおりパッチは重ねない。**7040f23 を revert(2 度目)し、この系譜
(WP361b → v2)をここで打ち切る。**

#### 裁定: エディタ lifecycle × arena の合流は「修正」ではなく「設計」だった

2 連続不合格の共通根: この合流(spawn/destroy/undo と BehaviorArena・ECS token の
transaction 統合)は WP361a のコードレビュー指摘 #1 の「修正」として入り、
**プレファブ本体が受けた 3 巡の敵対的仕様レビューを一度も受けていない**。
identity・所有・rollback の各難点(まさに WP360/WP361 で 3 巡かけて潰した種類の
問題)を、修正パッケージの粒度で 2 回解いて 2 回失敗した。

**WP363(起票のみ。実装前に設計文書 + 仕様レビュー必須の上段)**:
エディタ lifecycle 操作と runtime(ECS/arena/camera/light/physics)の合流。
2 回の失敗レビュー(WP361a §10 の 9 件 / WP361b §10 の 7 件 / WP361b v2 §10 の
7 件)を仕様の種として全部使う。最低限の設計論点:
- ECS destroy token の row 再解決(backfill 追随)か、token を row でなく entity で持つか
- identity plan の正本(final resolved scene)と sequential 正規化の統一
- point of no return と deferred queue checkpoint(v2 の B は方向として正しかった)
- entity token の optional 化(light-only / collider-only / empty object)
- 実 producer request の捕捉方法(D0 を保ったまま共通 pure builder か)

**それまでの現状(= WP361a 状態)の既知の穴を台帳の負債として明記**:
- 別 scene への lifecycle 編集が現行 scene の runtime を再構築する(WP361a #1)
- destroy の onDestroy が entity 死後(同 #2)/ 混合 batch の handle/seq 分裂(#3)/
  同一 batch 親子 spawn 失敗(#4)/ seq 0 誤 sentinel / Inspector CAS は動作
これらは memory-safe であり、出荷 4 project(behavior 0)は踏めない。
編集ワークフローの穴として WP363 が閉じる。

#### U1 完成宣言の再訂正

U1(プレファブ)の機能面 —— 文書・展開・identity・表示・SnapshotV2・save・
set/unset —— は WP361+361a+362 の状態で全て成立し全数緑(1432×2 で検証済み)。
b 系譜はプレファブでなく**エディタ lifecycle の後退修復**であり、これは WP363 に
移った。よって **U1 は「完成、ただしエディタ lifecycle × behavior の合流は
WP363 待ち」**と記録する。

### WP364: RT1a —— RT interface 安全・再帰・オラクル基盤(語彙・SBT 不変)

**§4 規則 11 上段(shader 契約・新検証基盤)。仕様 + コードレビュー。見積: 中〜大。**
**規範は docs/design_raytracing.md(v4、3 巡 29 件消化)の RT1a 節 + §4 §5 §7 §11。**

#### 範囲

1. **RT reflection 拡張**(design §7): SPIR-V から ray payload / incoming payload /
   hit attribute の storage class・location・構造 fingerprint・byte size・
   entry point・所属 group を抽出して reflection に保持
2. **load 時 named 検証**:
   - stage 間 payload 構造一致(raygen↔miss↔closesthit、location ごと、
     型全体の fingerprint 比較)
   - **絶対規則**: intersection の無い(= 現行全ての)hit group で closesthit が
     hit attribute を宣言するなら `vec2`(f32×2)固定。逸脱は shader path・
     group・実際の型を含む名前付きエラー
   - hit attribute の byte size ≤ `maxRayHitAttributeSize`
   - **`OpTraceRayKHR` の operand 定数性**(design §11-3):
     sbtRecordOffset / sbtRecordStride / missIndex が SPIR-V 上で定数で
     あること。動的 operand は名前付き拒否。直接 `.spv` 供給も同じ検証を通る
     (layout に対する範囲検証は RT1b —— 本 WP では定数性のみ)
3. **`max_recursion`**: `ray_tracing` block に省略可 field(既定 1)。
   pipelinefactory.cpp:625 の固定 1 を置換。device の `maxRayRecursionDepth`
   超過は property 名・要求値・実値・device 名つき named 拒否
4. **runtime facts 追加**(design §5): maxRayRecursionDepth /
   maxShaderGroupStride / maxRayHitAttributeSize / maxRayDispatchInvocationCount +
   各次元上限。dispatch 解決時に **overflow-safe な積検査**(現行は正のみ検査)
5. **CPU 参照オラクル基盤**(design §4 + §11-4): std のみの参照交差器 +
   **数値契約の表を本 WP で確定**(初期値: 座標 domain ±2^10 / t ∈ [1e-3, 2^12] /
   triangle 最小角 ≥ 5°・最小面積 ≥ 1e-4 / ε_t = 相対 1e-3 / ε_b = 重心座標 1e-3 /
   CPU は double + 有向丸めで区間構成 / **区間幅上限 = 相対 1e-2、超過 fixture は
   生成器が拒否**)。fixture 生成器は margin を**構成的に**保証する
6. **sentinel 検査(0-offset 経路)**: 既存 rt_shadow_mask_pipeline 相当の
   単一 group 構成で、hit / miss / **再帰深度の実効**(深さ n で n 回 payload を
   変える fixture shader)を CPU 参照と包含比較

#### 受け入れ(§4 規約 10 の対照)

- **既存 RT feature の bytes 不変**: rt_shadow_mask / rt_shadow_mask_pipeline の
  GPU 出力・frame plan・shader cache key が本 WP 前後で一致(検証は正当 shader を
  素通しするだけであることの証明)
- **各検証の負例が実 load 経路で落ちる**: payload 不一致 / vec3 hit attribute /
  attribute size 超過 / 動的 trace operand / recursion 超過(device 上限は
  facts を偽装できる test seam 経由で 1 に固定して再現)—— それぞれ named code を
  実 engine load(headless)で観測。**検証コードを削る mutation で素通りに戻る**こと
- **オラクルが噛む**: 参照器の交差判定に故意の符号バグを入れると sentinel 比較が
  落ちる(オラクル自身の対照)/ GPU 側 fixture の group handle を隣へずらすと
  record ID 包含が落ちる
- **再帰の実効**: max_recursion=1 のまま深さ 2 を trace する fixture が
  device 挙動に依存しないよう、**仕様上 miss へ落ちる形の fixture** で
  1 と 4 の出力差を CPU 参照と一致させる
- 出荷 4 project(RT 0 使用)の bytes・golden 完全不変 / matrix:
  compiler ON/OFF・Studio ON/OFF・web 拒否 / 全数 2 回 / GPU 直列は統合側

#### やらないこと

`ray_group_layout` / 3 domain / record ABI / 生成 include(RT1b = WP365 予定)。
`.rahit/.rint/.rcall` の語彙追加(RT2a/RT3/RT4)。material ABI(RT2b)。
AS への変更一切。

依存: なし(WP362 まで到達済みの HEAD)。

#### WP364 仕様レビュー(v1、差し戻し・9 件)と v2

v1 の受け入れ条件に Vulkan-invalid な試験(深さ 2 の miss 起動)と発火不能な負例
(triangle-only での attribute size 超過)を書いていた。全件「直す」。
**v2 は v1 との差分。矛盾したら v2 が優先。**第 2 巡を最終とする。

| # | v1 の壊れた点 | v2 |
|---|---|---|
| 1 | 深さ 2→miss 試験が Vulkan-invalid(miss も深さ 2 で起動)/ 最低保証 depth=1 / domain に 0 が通る | 二分割: 純 validator + 偽 facts の負例(`requested > fact`、**domain [1, fact]**、0 は `invalid_recursion`)/ GPU 実効は `maxRayRecursionDepth ≥ 2` の device 限定で合法な max_recursion=2×深さ 2(不足 device は理由つき非適用)/ create-info へ 2 が渡ったことは **Vulkan 呼び出し observer** で検査 |
| 2 | attribute size 超過の独立負例が triangle-only では存在不能(vec2=8B < 最低保証 32B) | size 検査の実装と unit 負例(合成 reflection)は残すが、**production 負例は RT3(intersection 解禁)へ明示送り** |
| 3 | validator 削除 mutation が invalid Vulkan 実行に依存 | 同一テスト内で「不正 reflection→named / 正 reflection→canonical 契約」+ production は **pre-create observer**(正例は validator digest 付きで create 到達、負例は create 呼び出し 0 回)。GPU mutation 対照は Vulkan-valid かつ policy-invalid な動的 operand。handle ずらし対照は**同種 2 group を明示宣言**した fixture で |
| 4 | 「所属 group を SPIR-V から抽出」は不可能(group は API 側の情報)/ entry point 未解決 | 層を分離: **module reflection**(entry `"main"` 選択、storage・location・canonical 型・size・trace operand。operand は module 全命令走査)と **pipeline occurrence 契約**(PipelineFactory が group index/type を結合)。複数 entry point fixture / ShaderBundle に安定診断 URI(embedded SPV の空 source_path を埋める) |
| 5 | spvlink 上の実装は出荷構成(SPIRV_LINK=OFF)で消える / 「定数」が未定義 | **常時ビルドの `rtspirvinspection`** を shaderreflection 隣接に新設し `buildFromSpirv()` で module 作成前に実行。定数 = 直接 `OpConstant`(32-bit 整数)と `OpConstantNull` のみ。`OpSpecConstant*`・`OpCopyObject` 経由は**保守的に named 拒否**。診断は shader URI・命令番号・operand 名。**matrix に `PELICAN_WITH_SPIRV_LINK=ON/OFF` を追加** |
| 6 | オラクル表が機械判定契約に未達(transform 可逆性・入射角・分母・丸め・量子化時点が未定) | 表を拡張: transform は identity または **condition number ≤ 100 の可逆行列** / ray-plane 入射角 ≥ 5° / determinant 下限 / **入力は f32 量子化後に CPU 評価** / 固定演算列 + `nextafter` 外向き丸め / ε_t の分母 = `max(1, |t|)`、区間幅の分母 = 区間中点 / 端点 ±4 ULP padding。**分類を「対応 device matrix 上の pelican conformance 契約」と明記**(Vulkan-wide 保証ではない) |
| 7 | design §7 の callable・incoming 個数・fingerprint 規則を閉じていない | canonical type tree(scalar 種/bit 幅・vec/matrix 次元・固定配列数・member 順。名前と ID は除外)+ **tree equality を正**とし fingerprint は索引に格下げ / size は Vulkan の scalar-alignment 規則で算出 / runtime/spec-sized array は named 拒否 / **incoming 種類ごと 1 変数**の負例追加 / callable は RT4 への明示 carve-out を WP と design 側 note の両方に記載 |
| 8 | facts の配線・seam・code 名が未指定 / maxShaderGroupStride が孤児 | **`RayTracingRuntimeFacts`** 一型(RT property + 各次元 `maxComputeWorkGroupCount×Size` + device 名)。feature 無効時は atomic にゼロ化。**純関数 `validateRayTracingRequest` / `validateTraceExtent`** を pipeline build・初回 extent 解決・rebind・最終 command 前の全 4 箇所から呼ぶ(偽 facts は純関数にのみ注入、production は observer で証明)。負例: 偽 max=1024 vs 64×64×1 / 各次元超過 / UINT32_MAX 積 overflow を load と rebind の両方で。**code 固定**: `pelican.ray_tracing.invalid_recursion@1 / recursion_limit@1 / dispatch_dimension_limit@1 / dispatch_invocation_limit@1 / trace_operand_dynamic@1 / payload_mismatch@1 / hit_attribute_forbidden@1 / hit_attribute_size@1 / interface_count@1 / interface_type_unsupported@1`。**maxShaderGroupStride は RT1b へ延期**(孤児 fact を作らない)。既定 1 の literal は `RayTracingTaskShaderDefinition` の一箇所のみ |
| 9 | 「bytes 不変」が自己承認可能 / compiler-OFF の空 cache key 比較 | **実装前 HEAD の親版 fixture を凍結**(embedded SPV の SHA-256・compiler-ON cache key・canonical frame plan projection・GPU golden。WP354a の流儀で provenance 付き)/ compiler-OFF は **SPV hash 比較**(空 cache key 比較は不可と明記)/ `max_recursion` 省略時は frame plan に新 field を出さない / 4 project 条件は「非 RT 回帰」と分類し RT validator の有効性条件から分離 |

補足修正: 再帰固定の行は pipelinefactory.cpp:628(625 は誤り)。
design_raytracing.md §7 に「callable data の検証は RT4 で」の note を追記する
(本 WP のマージ時に同時修正)。

#### WP364 第 2 巡(差し戻し・5 件)と v3 —— 決定

レビューはここで打ち切り、5 件を決定で閉じる。**v3 > v2 > v1。**

1. **entry の選択キー**: module reflection は**全 entry point を
   `(execution model, entry 名)` キーで保持**する。pipeline occurrence が
   `(期待 stage, "main")` で必ず選択する。`buildFromSpirv()` と直接 SPV API に
   **期待 stage 引数を必須化**。fixture: 同名 `"main"` の raygen+miss を持つ
   1 module から、それぞれ正しい reflection が選ばれること
2. **trace call と到達 group の対応**: 各 `OpTraceRayKHR` record は
   `{caller entry, payload 変数の location, missIndex, sbtRecordOffset,
   sbtRecordStride}` を保持する(operand は定数強制済み)。
   **RT1a の到達規則**(AS instance offset 恒等 0・geometryIndex 恒等 0 を根拠に):
   到達 miss = `miss[missIndex]`、到達 hit = `hit_group[sbtRecordOffset]`。
   その group の incoming payload tree と call 側 payload tree の不一致は
   `payload_mismatch@1`(caller entry・call 位置・group index・両型を含む)。
   正例: location 0→group 0 / location 1→group 1 の使い分け pipeline が通る。
   **RT1b への前方互換 note**: layout 導入後は到達集合を
   「trace する ray type × 全 group」に広げて再定義する(この規則は RT1a 限定)
3. **オラクル数値の完全確定**:
   - transform = instance の object→world。**∞-norm condition number ≤ 100**、
     **|det| ≥ 1e-3**。座標 domain・面積・角度は **world 空間(変換後)**で判定
   - 入射条件: ray 方向と triangle 平面のなす角 ≥ 5°(`|dot(dir, n̂)| ≥ 0.0871`)
   - ε_t の分母 = **CPU 区間中点** `max(1, |t_mid|)`。区間幅上限の分母も同じ
   - ULP = **f32 の ULP**(端点の大きさで評価)。端点は外向きに ±4 f32 ULP
   - CPU 参照は double・**FMA/contraction 禁止**(`FP_CONTRACT OFF` を明記)、
     演算列は **Möller–Trumbore を次の順で固定**:
     `e1=v1−v0; e2=v2−v0; p=cross(d,e2); det=dot(e1,p); inv=1/det;
     tvec=o−v0; u=dot(tvec,p)·inv; q=cross(tvec,e1); v=dot(d,q)·inv;
     t=dot(e2,q)·inv`。各段の区間は `nextafter` で外向き伝播し、
     最終比較は「GPU の f32 t(±4 f32 ULP 拡張)⊂ CPU 区間」
4. **fake facts の面の分離**: 「fake 負例を load/rebind で実行」を**削除**。
   fake facts は純 validator テスト専用。load・初回 extent・rebind・最終 command の
   4 配線は「observer が実 extent と実 facts を受け取った記録 + **配線削除 mutation で
   落ちる**」ことで証明する(production facts での拒否は要求しない ——
   conformant device の下限が大きく本物では発火しないため)
5. **max_recursion の実効証明**: 同一 ctest で **省略 / 明示 1 / 明示 2** の
   3 構成を production load し、observer が create-info の
   `maxPipelineRayRecursionDepth = 1 / 1 / 2` を全て検査する。
   `RayTracingPipelineDesc` に **default を持たない resolved depth** を必須保存し、
   shader reload 後も observer 値 2 が維持されること(hard-code
   `min(2, fact)` の類が全条件を通る穴を塞ぐ)

以降の指摘はコードレビューで受ける。

#### WP364 実装の UNMET 2 件への裁定(v3 追補)

1. **「payload location」は仕様の誤り(v3 決定 2 の修正)**: SPIR-V の
   `OpTraceRayKHR` は payload **変数の operand id** を直接参照し、glslang は
   GLSL の location をコンパイル時に解決するため最終 SPIR-V に Location 装飾は
   現れない。record が持つべきは location でなく **operand id から解決した
   payload 変数の canonical type tree**。「装飾欠落時 location 0 fallback」は
   削除し、比較は常に変数の型で行う。受け入れ(実 GLSL で 2 つの payload 変数を
   使い分ける pipeline の正例・cross-wire 負例)はこの形で成立させる。
2. **web matrix は本 WP では N/A**: web は別リポジトリ管轄で、`compute_tasks`
   の明示拒否(09_web.md)は既存のまま。本 WP は wire に新 field を足していない
   (max_recursion 省略時は frame plan 不変が受け入れ済み)。

#### 引き継ぎ: 残 WP への先行裁定(2026-08-30、Fable → Opus)

**目的**: 以降のオーケストレータが判断待ちで止まらないよう、予見できる分水嶺に
先に裁定を下す。手順の判断はスキル codex-wp/orchestration.md の決定表が正。
本節は**内容**の裁定。裁定を覆す場合は理由を本節の下に追記してから覆すこと。

##### 現在地(この節を書いた時点)

- HEAD 系譜: WP364(RT1a)マージ済み、統合で fixture 測定欠陥 1 件 →
  per-file manifest(凍結 SHA d5ea0c5 由来)生成済み・テスト置き換えを
  codex が実装中(worktree <作業ディレクトリ>/wp364fix)。**次の作業 = その回収 → 統合再走 →
  §10 コードレビュー → 仕分け(修正パッケージは WP364a を 1 回だけ)**
- 全数の基準値: 1450(統合 fixture 修正後に ±数件動く。「ちょうど N ×2」を毎回記録)

##### 作業順(裁定)

1. WP364 完結 → 2. **WP365 = RT1b** → 3. **WP363 の設計文書**(RT1b の実装中に
   並行で書いてよい。ただし workspace-write の agent は同時 1 本まで —— 設計は
   読み取りレビューなので並行可)→ 4. **RT2a** → 5. **RT2b** →
   6. **WP363 実装** → 7. RT3 → 8. U2 → 9. R1 → 10. RT4/RT5 → …
   利用者の指示が来たらそちらが優先。

##### WP365(RT1b)の分水嶺と裁定

- **layout の置き場所**: 独立ファイル(`pelican.ray_group_layout` schema)を
  rendering config の新 key `ray_group_layout: "<path>"` で参照する。
  preset/feature 合成の規則に巻き込まない
- **layout 名の字句**: `^[A-Za-z0-9_]{1,64}$`(**hyphen 禁止** —— 生成 include の
  マクロ名 `PELICAN_RAY_TYPE_<NAME>` / `PELICAN_MISS_<NAME>` に使えないため)。
  生成 symbol の衝突は名前空間 prefix で防ぎ、同名 group/ray type は既存の
  重複検査で拒否
- **record ABI**: **std430 をそのまま採用**(独自規則を発明しない)。
  data 開始 = handle サイズを `shaderGroupHandleAlignment` へ切上げ。
  member は宣言順。vec3 は std430 どおり 16B 整列(禁止しない)。
  正準化 JSON に offset/size 表を出力し、mirror test で固定
- **`q` の正準順**: **object 名の辞書順**(scene 内一意は既存検証)→ 同一 object 内は
  merged component 順。draw inventory 順・宣言順・AuthoringObjectId は使わない
- **共同 transaction の所有**: renderer 側に layout generation を持たせ、
  design v4 §11-1 の invalidation 列挙(layout digest / row 集合 / ABI)で
  全再構築。部分更新経路は作らない
- **compiler-OFF × layout**: named reject(design v4 §11-5 のとおり)。
  offline bake は RT-bake として別起票(手を出さない)
- 仕様レビューは 2 巡 + 決定差分の型。**RT1a の実績表(sentinel・observer・
  純 validator)を対照の雛形として再利用**すること

##### WP363(lifecycle 設計)の方向裁定(設計文書が採るべき骨格)

3 本の失敗レビュー(WP361a §10 の 9 件 / WP361b の 7 件 / WP361b v2 の 7 件)が
仕様の種。設計は最低限つぎを採用する(再発明しない):
- identity plan は **final resolved scene から毎 execution 再構築**(undo/redo 含む)
- destroy は**全 fallible 段の後の point of no return** でのみ onDestroy。
  deferred queue は checkpoint。**ECS destroy token は publish 直前に
  entity から chunk/row を再解決**(WP361b v2 の UB の根)
- entity token は **optional**(light-only / collider-only / empty object)
- runtime adapter は **current scene の変更にのみ**生成(authoring は全 scene 適用)
- 文法は**単一 AST parser**(手前 whitelist 禁止)。`attachment_handle` は公開 CAS
- seq 0 は sentinel にしない
- 受け入れには「実 producer(inspectormodel / imgui inspector)が生成した request を
  捕捉して通す」形を含める(手書き JSON の複製は producer テストにならない)

##### RT2a の裁定(design v4 §11-6 の確認)

- 範囲 = `.rahit` 語彙の全所同段 + typed triangle group + **record 定数ベース**の
  subset any-hit + oracle。**material-alpha は含めない**(RT2b)
- 語彙追加は wire 正本(pelican_project)→ core → PlanViewer/Studio を同段で。
  D0: Studio は wire 経由のみ

##### RT2b の分水嶺と裁定

- **bindless の実装形**: geometry access(vertex/index)は **buffer device address**、
  texture は **descriptor indexing の配列**(sampled image は BDA 不可)。
  必要 capability が無い device は hard reject(RT 系の既定方針どおり)
- overlay は path 所有の独立ファイル `pelican.ray_bindings@1`。
  material 文書への直書きは禁止(purge 閉包のため)
- 両車線 portable alpha profile はここで実装し、byte 一致契約はこの profile に
  限定して再定義(design v4 §11-6)

##### U2(プレファブ)着手時の注意

- 一括流し込みの dry-run は **ResolvedScene の純 resolver 経路**をそのまま使う
  (新しい検証経路を作らない —— 第二の流儀の禁止)
- unpack の identity 断絶(gid 消滅)は WP361 v3 差分で契約済み。再設計しない

##### R1 着手時の注意

- WP354 の fixtures が等価オラクル。planner が typed PassDefinition を消費する形。
  RT の compute_tasks parser に隣接するため **RT2b より後**に置いた(並行しない)

##### 恒常の「やらない」

- 完成宣言の早書き(最後の統合緑まで書かない。書くなら「保留」明記)
- 打ち切り済み設計(prefab v4 / RT v4 / WP364 v3)への再レビュー起動
- **roadmap 項目から WP を起票するとき、自分の要約で書くこと** —— 台帳の本文(依存・レビュー段・fixture 数・等価性の方向)を読み直す。U2 は WP363 を、R1 は上段/8 fixture/両方向を落として 2 件同時に差し戻された
- workspace-write agent の 2 本同時(read-only レビューとの並行は可)
- 台帳・設計の normative example を parser に通さず追加すること

##### 環境に生きている状態(セッション跨ぎで残る。要注意)

`git worktree list` を最初に必ず見ること。この節を書いた時点:
- **<作業ディレクトリ>/wp364fix (agent/wp364fix)**: WP364 の fixture 測定欠陥修正が
  **codex 実行中**(ログ <作業ディレクトリ>/wp364fix.log)。完了通知が来たら §E で回収 →
  統合再走。これが本線の続き。**生存確認はログのバイト伸び**(39B や 1h 停滞で疑う)
- **<作業ディレクトリ>/wp336 (agent/wp336)**: **前セッションからの放置 worktree**。
  main から約 1196 先行(render-target-runtime-slice 系列 + 独自の shader reach 作業)。
  「WP345 の書き直し + wp336 をどうするか」は**利用者との合同判断待ち**で、
  勝手にマージも削除もしない。触る前に利用者へ確認する
- 放置の別件: フレーク族(WP332 / 334b / 335。~load 依存で単独では常に緑、
  assertion 本文を一度も捕捉できていない)。特性化は「1 本の重い load 再現 +
  全出力捕捉」が要る。急がない

##### 本線が今どこか(一行)

WP364(RT1a)マージ済み → 統合で fixture 1 件割れ → per-file manifest 生成済み →
テスト置き換えを wp364fix で実装中。**次の一手 = wp364fix の回収**。
その後は「引き継ぎ: 残 WP への先行裁定」節の作業順どおり。

#### WP364 統合 fixture の autocrlf 裁定(2026-08-30)

fixture 修正の実装が正しく判断を仰いだ: parent manifest は git blob(LF)を hash
するが、この checkout は autocrlf=true で text は CRLF。**「出荷物不変」= コミット
された blob が不変**であって、ローカル checkout の改行表現ではない。
**却下**: リポジトリ全体を LF checkout に固定する .gitattributes(重く、全 checkout と
golden/他 fixture に波及)。**採用**: manifest v3 が git の per-file eol 分類
(lf|binary)を持ち、テストは eol==lf を CRLF→LF 正規化してから hash、
eol==binary は raw 比較。70 text + 5 binary。manifest は integrator が
git blob + `git ls-files --eol` から機械生成(実装は再生成しない)。

#### WP364(RT1a)完了(2026-08-30)

本体 b1449f02 + fixture 修正 089b9ebe(+ manifest v3 と autocrlf 裁定 90e1bb81)。
統合: 全数 **1450 ×2 緑**(GPU 直列込み・`PELICAN_TEST_PROJECTS_DIR` 付き)、
doclink 2341 ok / 0 stale。基準値 N=1450(前 WP は 1432)。

成立したもの: RT interface reflection(全 entry を
`(execution model, entry 名)` キーで保持、occurrence が `(期待 stage, "main")` で選択)/
常時ビルドの `rtspirvinspection`(SPIRV_LINK OFF でも生存)/ `OpTraceRayKHR` の
operand 定数性 + payload 変数型による到達 group 検証 / triangles の hit attribute
絶対規則 / `max_recursion`(facts 比較・1/1/2 の create-info 観測・reload 維持)/
`RayTracingRuntimeFacts` + 純 validator + 4 配線 / CPU 参照オラクル(v3 の数値・
演算列・丸め)+ sentinel。error code 10 種。
出荷 4 project と既存 RT feature は bytes 不変(親版 fixture = 凍結 SHA d5ea0c5、
per-file manifest v3 + eol 分類で検証)。

統合中に閉じた 2 件(閉ループ内・連鎖に数えない): fixture の測定範囲欠陥
(ディレクトリ走査 → 追跡ファイルの per-file manifest)/ autocrlf 裁定
(「不変」= コミット blob。.gitattributes 全体変更は却下、eol 分類で正規化)。

**§10 コードレビューの指摘は宣言どおり台帳のみ**(RT1a の連鎖はここで打ち切り)。
次: 「引き継ぎ: 残 WP への先行裁定」節の作業順どおり **WP365(RT1b)の起票**。

#### WP364 コードレビュー(不合格・5 件)の仕分けと WP364a

出荷影響は 0(4 project・core resources・既存 RT feature の bytes 不変、
到達 group 解決・10 named code・max_recursion・D0 は「壊せなかった項目」で生存確認)。
壊れているのは**証拠の側**である。しかも 2 件は**以後の RT 全段が依存する審判**
(CPU オラクルと配線証明)なので、ここで直す。決定表どおり
**本体への修正パッケージは 1 回だけ。WP364a のレビュー指摘は台帳のみ。**

仕分け: **直す 5 件 = WP364a**(台帳送りは無し —— どれも「テストが主張を証明して
いない」型で、放置すると後続 WP がその緑を根拠にしてしまう)。

### WP364a: RT1a の証拠の修復(最終。以降の指摘は台帳)

**§4 規則 11 上段。コードレビューのみ(仕様源は上記 5 件)。見積: 中。**

1. **CPU オラクルが正当な f32 hit を拒否する**(実害): CPU は double 区間端に
   ±4 f32 ULP を足すが、比較側は double→f32 丸め後にさらに ±4 ULP 展開するため、
   double→f32 の最大 0.5 ULP 分が区間に入っていない。
   **修正**: CPU 区間に double→f32 の外向き丸めを含めてから 4 回 `nextafterf`
   (最低限 `expandGpuRayT(static_cast<float>(t))` を包含すること)。
   **受け入れ**: レビューの反例(ray dir `(0.6,0,0.8)` の f32 値 /
   triangle `(5,-1,8),(7,-1,8),(6,1,8)` / t≈9.99999985 → GPU `10.0f`)を
   regression として追加し、修正前は落ちること
2. **parent manifest が「コミット blob 不変」を測っていない**(実害):
   working-tree bytes を正規化して比較しているため、(a) blob 側を CRLF に
   変えても通る、(b) manifest に無い新規 tracked file が素通り、
   (c) `tracked_file_count` が未参照。
   **修正**: **`git show HEAD:<path>` の blob bytes を直接比較**する形へ。
   併せて現コミットの `git ls-files -- projects/<name>` と manifest の path 集合を
   **完全一致**で検査(欠落・余剰の双方)、`tracked_file_count` も検査、
   path confinement(`..`・絶対パス)も拒否。正規化は不要になる(裁定
   「不変 = コミット blob」に一致する形での実装。90e1bb81 の裁定は維持、
   実装が裁定に追いついていなかった)
3. **配線証明が静的ソース文字列の代理条件**(実害・§4 規約 10 違反):
   `test_wp364_raytracing_wiring_mutations.cmake` は関数名の文字列有無しか見ず、
   pure validator 呼び出し 2 行を削除しても observer と unit 負例は通る。
   **修正**: 文字列検査を廃し、**各 site の delegate を無効化できる実行 variant**
   (テスト専用の注入点。production 既定は有効)を用意して同一テスト内で
   有効/無効を実行し、**拒否結果・解決済み値・observer 記録が異なる**ことを検査。
   併せて **create observer の `validator_digest` に解決済み
   `missIndex` / `sbtRecordOffset` / 到達 target / trace payload 型を含める**
   (現在は stage/URI/group/interface のみで、GPU テストも non-empty しか見ていない)
4. **数値契約の境界テストが主要境界を通していない**(懸念): 非有限 transform/ray、
   最小面積、最小角、`t_max`、区間幅上限、非 f32-exact 端点。
   **修正**: 各規範定数の**両側境界** fixture を追加(検査を削ると落ちること)。
   さらに「符号変異オラクル」は現在**計算後の区間を負にしているだけ**なので、
   交差器の実装を差し替え可能にして**実際の符号バグ版**を同一テストで走らせる
5. **SPIRV_LINK/compiler OFF 構成で WP364 テストが CI 実行されていない**(懸念):
   `.github/workflows/cpu-gate.yml` の OFF job は WP354 の 2 target しか走らせない。
   **修正**: OFF job に `pelican_test_raytracing_rt1a_test` の build/run を追加
   (direct-SPV 負例と embedded-SPV 正例が実際に走ること)

**受け入れ全体**: 出荷 4 project・core resources・既存 RT feature の bytes 不変は
維持 / 各修正に「取り消したら落ちる」対照 / 全数 2 回(統合側 GPU 直列)/
matrix は SKIP_DEVSTUDIO 両構成 + SPIRV_LINK ON/OFF。

依存: WP364。**連鎖打ち切り宣言: WP364a の §10 指摘は無条件で台帳。**

#### WP364a 完了 —— RT1a の証拠が主張どおりになった(2026-08-30)

統合: 全数 **1452 ×2 緑**(GPU 直列込み)、doclink 2341 ok。基準値 N=1452。
5 件すべて修正、UNMET 0。要点:
- CPU オラクルは double→f32 の外向き丸めを区間に含めるようになり、レビューの
  反例(t≈9.99999985 → GPU 10.0f)を受理。同テスト内で旧アルゴリズムが同じ値を
  拒否することも固定
- parent manifest 検査は `git show HEAD:<path>` の生 blob + `git ls-files` の
  path 集合完全一致・count・confinement へ(裁定「不変 = コミット blob」に実装が追随)
- 静的文字列の配線変異検査は**廃止**。4 site の delegate をテスト専用注入点で
  無効化し、拒否結果・解決値・observer 記録の差を同一テストで比較。
  validator_digest に missIndex / sbtRecordOffset / 到達 target / payload 型を追加
- 数値契約は両側境界 fixture。符号変異は実際に `inv = -1/det` を実行する
  差し替え交差器で(post-hoc の区間反転を廃止)
- CI の compiler OFF / SPIRV_LINK OFF job で RT1a target を build/run

**RT1a はここで確定**(本体 b1449f02 / fixture 089b9ebe / 証拠 WP364a)。
連鎖打ち切り済み: 以降の指摘は台帳のみ。次は先行裁定どおり **WP365(RT1b)**。

### WP365: RT1b —— SBT addressing と record ABI(3 domain の実体化)

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 大。**
**規範は docs/design_raytracing.md(v4)§3・§5・§11 と、台帳「引き継ぎ: 残 WP への
先行裁定」節の WP365 裁定。両者が正で、本節はそれに対する WP としての範囲・
受け入れ・オラクルを定める。**

#### 前提(RT1a で揃ったもの)

`rtspirvinspection`(常時ビルド、全 entry を `(execution model, entry 名)` で保持、
`OpTraceRayKHR` の operand 定数性 + payload 変数型)/ `RayTracingRuntimeFacts` と
純 validator + 4 配線(delegate 注入点つき)/ CPU 参照オラクル(外向き丸め済み)/
create observer の `validator_digest`(missIndex・sbtRecordOffset・到達 target・
payload 型を含む)/ 親版 fixture(凍結 SHA d5ea0c5、git blob 直接比較)。

#### 3 domain(design v4 §3。実装はこの語彙を使う)

`g` = logical group(layout 所有)/ `r`,`R` = ray type index, count /
`q` = scene record row(instance ごと)/ `H[p,g,r]` = pipeline の handle。
**physical hit record index = `q×R + r`**、内容 = `H[p, g(q), r] ∥ data(q)`。
**Vulkan operand 割り当て(固定)**: `instanceShaderBindingTableRecordOffset = q_base×R`
/ `sbtRecordStride = R` / `sbtRecordOffset = r`。
raygen へ公開するのは `ray_type_index` / `ray_type_count` / 独立 `miss_index` のみ。
`record_index_stride`(=R)と `record_byte_stride`(byte、`maxShaderGroupStride`
検査対象)は文書・コード・診断で常に別名。
**1 BLAS = 1 geometry を恒久制約として維持**(`geometryIndex` 項は恒等 0。
sentinel の検査対象から明示除外)。

#### 範囲

1. **`pelican.ray_group_layout` v1**(独立ファイル。rendering config の新 key
   `"ray_group_layout": "<path>"` で参照。preset/feature 合成には巻き込まない):
   ```json
   { "schema": "pelican.ray_group_layout", "version": 1,
     "ray_types": ["primary", "shadow"],
     "groups": [ { "name": "opaque" }, { "name": "foliage" } ] }
   ```
   - 名前の字句 `^[A-Za-z0-9_]{1,64}$`(**hyphen 禁止** —— 生成マクロ名に使えない)。
     ray_types / groups とも非空・重複禁止・unknown field 拒否
   - **layout digest** = ファイル raw bytes の SHA-256
2. **生成 include**(compiler 所有の virtual include + cache key。既存経路を使う):
   `PELICAN_RAY_TYPE_<NAME>` と `PELICAN_MISS_<NAME>` のみを定義。
   **`g×R+r` や `q` は定義しない**。cache key に layout digest を含める
3. **静的 record ABI**: 型集合 = leaf の scalar/vec(WP358 の語彙)。
   data 開始 offset = `shaderGroupHandleSize` を `shaderGroupHandleAlignment` へ
   切上げ。member は宣言順に **std430 をそのまま適用**(vec3 は 16B 整列。
   独自規則を作らない)。padding は 0 初期化。
   **offset/size 表を正準化 JSON に出力**し mirror test で固定。
   `record_byte_stride > maxShaderGroupStride` は名前付き拒否(RT1a で延期した
   `maxShaderGroupStride` fact をここで実装・使用する)
4. **task 側の充填宣言**: task は trace する ray type を宣言し、
   **その ray type × 全 group** の充填を要求される(trace しない ray type は免除)。
   不足は task 名・group・ray type・available 一覧つきの名前付きエラー
5. **`q` の採番と共同 transaction**:
   - **正準順 = object 名の辞書順 → 同一 object 内は merged component 順**
     (draw inventory 順・宣言順・AuthoringObjectId は使わない)
   - **invalidation 要因**(design v4 §11-1): layout digest 変更 / row 集合の変更
     (instance の追加・削除・group 束縛変更)/ record ABI 変更
   - `{TLAS, 全 SBT の data 領域}` を単一 immutable row snapshot から candidate として
     共同構築 → 原子的 publication → 旧世代は submission lease で退役。
     **部分更新経路は作らない**。generation は renderer 所有
6. **束縛 overlay の最小形**(design v4 §11-2 で RT2b から前倒し):
   `pelican.ray_bindings@1` —— component/material の stable ref → group 名、
   および **authored 静的 record 定数**。これが `g(q)` と `data(q)` の
   production 供給者になる(RT2b が material/geometry index と bindless 表で拡張)
7. **production TLAS が record base `q_base×R` を書く**
8. **compiler OFF × layout 使用 project は named reject**(design v4 §11-5)。
   layout 不在 project は **compiler ON/OFF 両対応のまま、旧 lowering を byte 保存**

#### 受け入れ(§4 規約 10。RT1a の対照様式を雛形に)

- **旧 lowering の byte 保存**: `ray_group_layout` 不在の project は
  frame plan・SBT bytes・GPU 出力・shader cache key が RT1a 時点と完全一致
  (既存 `rt_shadow_mask_pipeline` が親版オラクル)
- **sentinel(非ゼロ base 必須)**: **二 pipeline × 二 instance**の production 配線で、
  各 `(q, r)` record の data に一意 sentinel を書き、raygen が読んだ値から
  `q_base×R` と `r` と miss index を復元。`geometryIndex` 項は恒久制約により除外
- **layout 世代の原子性**: layout を変更した際に header・cache・全 SBT・TLAS base が
  同一 transaction で更新されること / **失敗注入で旧世代が完全に残る**こと
- **`q` の順序非依存**: object の宣言順を入れ替えた同一 scene で、
  解決された `q` 割り当てと SBT/TLAS bytes が一致
- **row 集合変更**: instance の追加・削除後に、残った instance が
  **他人の record を読まない**(削除前後の sentinel 比較)
- **充填不足**: 宣言 ray type × 未充填 group で名前付きエラー(文言に available 一覧)
- **ABI**: 正準化 JSON の offset/size 表と実 SBT bytes の一致 /
  `record_byte_stride > maxShaderGroupStride` の named 拒否 /
  mirror test(表に無い member を足すと落ちる)
- **compiler OFF**: layout 使用 project が named reject / layout 不在 project は
  従来どおり(両方を同じ matrix で)
- 出荷 4 project の bytes・golden 完全不変(RT 未使用のため。親版 manifest で検証)
- matrix: SKIP_DEVSTUDIO 両構成 / SPIRV_LINK ON/OFF / compiler ON/OFF /
  全数 2 回 / GPU 直列は統合側

#### やらないこと

`.rahit`(RT2a)/ material ABI・bindless・両車線 alpha(RT2b)/
AS の宣言面(RT3)/ callable・動的 record(RT4)/ 変形 geometry(RT5)/
複数 geometry BLAS・indirect・AS graph node 化(RT6)/ 編集 RPC(RT7)/
offline bake(RT-bake として別起票)。

依存: WP364 + WP364a。

#### WP365 仕様レビュー(v1、不合格・10 件)と分割

10 件のうち #2(q の全順序)と #4(overlay が production の `g(q)`/`data(q)` を
解決できない)は同じ根に届いている: **TLAS の row に identity が無い**。
object 名は optional で重複検査も名前付きにしか効かず、1 model component が
複数 primitive を生み、AS snapshot は asset・geometry allocation・transform しか
持たず authoring 由来を運んでいない。これは layout 以前の配管であり、
**WP365(row identity)と WP366(layout + SBT + ABI)に分割**する。
一体で仕様レビューする。**全件「直す」。台帳送りは #6 末尾の 1 件のみ**
(RT1a の「GPU golden」テストが production renderer を走らせず既存画像 hash と
`fill_byte` の CPU vector で代用している件 —— WP364a で連鎖打ち切り済みのため
台帳に負債として記録し、**WP366 の受け入れで実 renderer の A/B 対照を新設**して
実質的に閉じる)。

| # | v1 の壊れた点 | v2 の答え(行き先) |
|---|---|---|
| 1 | sentinel を `(q,r)` ごとに一意にする条件が `data(q)`(r 非依存)契約と矛盾 | WP366: sentinel は **q ごとに一意・全 r で複製**。`r` は hit shader の compile-time ray-type marker、`g` は group ごとの shader marker で識別。TLAS base を誤値に差し替える否定対照 |
| 2 | q の正準順が全順序でない(名前 optional / 複数 primitive / merged 順は宣言順) | **WP365**: row key を明記(下記) |
| 3 | `H[p,g,r]`・miss・record member の wire grammar が無い | WP366: layout に typed record member schema、task に `traced_ray_types` と named miss table と `group×ray_type→shader` mapping。未充填 slot の bytes と到達不能性の規則も |
| 4 | overlay が production の `g(q)`/`data(q)` を決められない(provenance 不在) | **WP365**: scene→ECS→ModelInstance→DrawItem→TLAS row に stable provenance を運ぶ。overlay の grammar と解決は WP366 |
| 5 | 共同 transaction に単一 publication 点が無い(pipeline/SBT は config 世代、TLAS は後続 frame) | WP366: **cache は content-addressed append-only として transaction 対象外**と明記 / transaction 対象 = layout digest + shader bundles + pipeline objects + **完全な SBT** + row snapshot + TLAS / **二段 commit**(新世代は TLAS submission 成功まで非公開)/ compile 後・pipeline 後・SBT 後・AS record 後・submit の各 failure point で active generation を観測 |
| 6 | 分岐点が preset resolver の許可 key 外 / 親版 baseline に SBT bytes が無く GPU golden も実 renderer 不使用 | WP366: **root authored document を最初に一度だけ parse して presence を typed sidecar として抽出**(位置を固定)。layout 不在は既存 resolver をそのまま。sidecar は flat/xr/preview 全経路へ。**固定 fake handle による純 pack baseline + production 分岐 observer + 実 renderer の同一 run A/B 対照**を新設(RT1a の golden 代用は台帳に負債として記録) |
| 7 | 生成 include の production 配線・cache invalidation・reload・明示 .spv が閉じていない | WP366: **layout digest を生成 virtual source の salt に必ず含める**(group 並べ替え・whitespace のみの変更でも cache key が変わる)/ RT task の virtual include 空化を解除する seam を名指し / **layout path を reload dependency に登録** / **layout 使用 × 明示 `.spv` は source-only named reject** |
| 8 | 固定 operand と 24-bit の範囲検査が無い(stride=0/1 でも sentinel が通る) | WP366: 純 validator に **`sbtRecordStride == R`**・offset が宣言 ray type・missIndex が named miss table 内・**checked `q×R` と 24-bit 上限**・record count/byte size overflow。1-geometry fixture で誤 stride を named reject する負例必須 |
| 9 | record ABI から一意の bytes を導けない(終端 padding・型集合) | WP366 に式を確定: `data_start = alignUp(handleSize, handleAlignment)` / `data_size = std430StructSize(members)` / **`record_byte_stride = alignUp(data_start + data_size, handleAlignment)`** / 終端 padding は 0。**型 whitelist v1 = `i32`/`u32`/`f32`/`vec2`/`vec3`/`vec4`**(8/16/64-bit と bool/enum/quat/string は v1 対象外・named reject。capability facts が要るため)。各型の GLSL/SPIR-V 型・JSON 表現・size/alignment を表で固定 |
| 10 | 構成 matrix が規範より狭い | WP366: **「全 SBT」= active owner scope の flat + xr pipelines** と定義し、`PELICAN_WITH_OPENXR` ON/OFF を追加 / preview は layout sidecar を同じに読む(RT task 明示拒否ではない)/ ImGui ON/OFF・RT capability 不在・web 既存拒否も targeted matrix に |

### WP365 v2: TLAS row の identity と provenance(layout の前提)

**§4 規則 11 上段。仕様(WP366 と一体)+ コードレビュー。見積: 中〜大。**

#### 範囲

1. **row key の確定**(#2):
   `row_key = { active_scene_id, object_ref, component_ref, mesh_index, primitive_index, node_index }`
   - `object_ref` / `component_ref` は **永続参照**(名前ではない)。scene の
     AuthoringObjectId 系ではなく、**ray-visible な object/component に stable ref を
     要求**し、無いものは**名前付き拒否**(`ray_row_identity_unavailable@1`)。
     プレファブ由来 component は WP361 の `stable_generated_id` を使う
   - 正準順 = 上記 tuple の辞書順(**object 名・宣言順・draw inventory 順は使わない**)
   - **q は active scene / TLAS ごと**(現行 runtime は同時に 1 scene。
     複数 scene 同時 TLAS は RT6 で再訪、と明記)
2. **provenance の運搬**(#4): scene → ECS → ModelInstance → DrawItem → TLAS row に
   `row_key` を運ぶ。AS snapshot に row_key を保持し、**TLAS instance 列と 1:1**
3. **q の採番**: row_key の正準順で 0 から連番。**24-bit 上限**の named 拒否
   (`ray_row_count_limit@1`)。この時点では `instanceShaderBindingTableRecordOffset`
   は 0 のまま(書き込みは WP366)
4. **観測面**: RPC / PlanViewer に row 一覧(row_key と q)を露出
   (G0 可読性の原則。後段のデバッグに必須)

#### 受け入れ(§4 規約 10)

- **順序非依存**: object の宣言順・component 順を入れ替えた同一 scene で、
  row_key → q の割り当てと TLAS instance 列が完全一致
- **名前なし object**: 名前を持たない ray-visible object を含む scene で、
  q 採番が決定的(名前に依存しないことの証明)。stable ref を持てない場合は
  named 拒否が観測される
- **複数 primitive**: 1 model component が複数 primitive を持つ scene で、
  primitive ごとに別 row・別 q(mesh/primitive/node index の効き)
- **プレファブ instance**: prefab 由来 component の row_key が
  `stable_generated_id` 由来で、undo/redo・前方挿入で不変(WP361 の identity 表と接続)
- **row 集合の変更**: instance の追加・削除の前後で、**残った row の q が
  正準順どおりに再採番**され、観測面がそれを示す
- 出荷 4 project の bytes・golden・frame plan 不変(RT 未使用。親版 manifest で検証)
- matrix: SKIP_DEVSTUDIO 両構成 / OPENXR ON/OFF / 全数 2 回 / GPU 直列は統合側

#### やらないこと

layout / SBT / record ABI / 生成 include / overlay の grammar と解決(全て WP366)。
TLAS への record base 書き込み(WP366)。複数 scene 同時 TLAS(RT6)。

### WP366: RT1b —— layout・SBT addressing・record ABI

**WP365 に依存。§4 規則 11 上段。見積: 大。**
範囲は v1 の WP365 節から row identity を除いた全部 + 上表の #1・#3・#5〜#10 の答え。
**v1 節の「### WP365: RT1b」の記述は、row identity 部分を除き WP366 の仕様として
読み替える**(layout 文法・生成 include・record ABI・充填宣言・共同 transaction・
production TLAS が record base を書く・compiler OFF 裁定・旧 lowering の byte 保存)。

追加で確定した事項は上表のとおり。とくに:
- record ABI 式と型 whitelist v1(#9)
- `sbtRecordStride == R` 等の範囲検査(#8)
- 二段 commit と cache の transaction 除外(#5)
- sidecar 抽出位置と実 renderer A/B 対照(#6)
- layout digest の salt・reload 登録・明示 `.spv` 拒否(#7)
- 「全 SBT」= flat + xr(#10)

依存: WP365。

#### WP365/366 仕様レビュー第 2 巡(不合格・8 件)と v3 —— 設計の方向転換

第 2 巡の核心は 3 つの事実である。

1. **authored object/component の永続参照は存在しない**。scene 形式に ID が無く、
   `AuthoringObjectId` は load 時に宣言順で採番される。`stable_generated_id` を
   持つのは prefab 由来 component だけ。よって v2 の `row_key` は現行から構成できない
2. **v2 の tuple は単射でない**。同じ primitive が二つの material group に置ける
   (ModelTemplate は一意性制約を持たない)ため、同じ tuple の TLAS row が 2 本できる
3. **WP365 単独では挙動が変わらない**。legacy 互換を守る実装なら SBT offset は 0 の
   ままで、識別子の拒否も layout(= WP366 の概念)に gate しないと既存 RT feature を壊す

**裁定: 永続 authored ID を前提にするのをやめる。** それは scene 形式・journal・
undo/redo・import に跨る別サブシステムであり(過去 WP304「安定 ID」が仕様レビューで
不合格になった領域そのもの)、RT の前提として押し込むのは WP363 で 2 度失敗した
「設計を飛ばした前提化」と同型である。

##### v3 の設計転換(2 点)

**(A) `q` は snapshot 局所でよい。永続性は要らない。**
共同 transaction が `{row snapshot, TLAS, 全 SBT data}` を**同一 snapshot から一緒に
再構築**する以上、q が編集で振り直されても record の対応は崩れない。
よって要求を「編集を跨いで安定」から**「同一 snapshot 内で決定的・単射」**へ弱める:
- 正準順 = **AS snapshot が既に持つ安定値のみ**で決める:
  `(asset_identity, geometry_allocation_id, index_offset, vertex_offset,
    instance の TLAS 列位置)`。列位置は同一 build 内で一意なので**単射が保証**される
- v2 の受け入れ「宣言順を入れ替えても q が一致」は**撤回**(そもそも不要だった)。
  代わりに「**同一入力から同一 q**(2 回 build して bytes 一致)」と
  「**row 集合が変わったら TLAS と SBT data が一緒に作り直される**」を要求する

**(B) 束縛は material を主キーにする(component 単位の束縛は v1 の対象外)。**
material は**名前(と文書 path)で永続**であり、DrawItem は既に
`GlobalMaterialId` と `source_material_index` を持っている。AS snapshot 生成時に
捨てられているだけなので、**material identity を snapshot まで運ぶ**のは
scene 全域の ID 発明より遥かに小さい。
- overlay `pelican.ray_bindings@1` の selector は **material 参照のみ**(v1)
- 同一 material の全 row が同じ `g` と `data` を得る(第 2 巡 #4 の
  component/material 競合は**そもそも起きない**)
- **失われる自由の明示**: 「同じ material の instance A だけ別 group」は v1 では
  書けない。これは**能力の意図的な縮小**であり、per-instance 束縛は
  永続 authored ID の設計が入る段(RT3 の instance flags と同時に再訪)で解禁する。
  design_raytracing.md の overlay 記述もこの範囲に合わせて読む

##### 第 2 巡 8 件の行き先(A・B 適用後)

| # | 指摘 | v3 |
|---|---|---|
| 1 | 永続 authored ref の正本が無い | **(A)(B) で前提ごと撤回**。row_key は snapshot 局所、束縛は material |
| 2 | tuple が単射でない | (A) の列位置により単射。同一 primitive × 二 material は**別 row・同一 material 束縛**で矛盾しない |
| 3 | 拒否条件が legacy と両立しない | 拒否は **layout を持つ generation に限定**(WP366 で実装)。layout 不在の RT/ray-query/preview は既存経路のまま。**WP365 は廃止**(下記) |
| 4 | material provenance と overlay grammar | **WP366 の正式範囲に含める**(v2 の除外は誤り)。selector は material のみ、未一致 row は default group、定数の過不足は named |
| 5 | layout/task の wire が無い | WP366 に完全な正負 JSON schema(下記の必須項目) |
| 6 | record ABI が shader 側と接続していない | WP366: 型表と offset 例(`vec3+f32`)を本文に置き、**final SPIR-V の shader-record storage/member type/offset が canonical ABI と一致することを load 時 invariant に**(shader 宣言だけ変えた負例で named reject) |
| 7 | 二段 commit が単一 state machine でない | WP366(第 1 巡 #5 の答えを維持・詳細化) |
| 8 | (第 2 巡末尾) | WP366 |

##### WP365 は廃止し、WP366 に一本化する

第 2 巡の結論どおり WP365 は**挙動を変えない基盤 WP**であり、縦切りの基準
(memory: slice-must-change-behavior)を満たさない。(A)(B) により必要な配管は
「material identity を AS snapshot まで運ぶ」だけに縮んだので、**WP366 の中に含める**。

**WP366(RT1b、最終形)の範囲** = v1 の WP365 節 + 上表の答え。必須の追加項目:
- material identity を DrawItem → AS snapshot → row へ運ぶ(最小の配管)
- overlay `pelican.ray_bindings@1`: material selector / group 名 / 静的 record 定数。
  未一致 row は layout の default group、定数の過不足・型不一致は named
- layout と task の完全な JSON schema(typed record members / `traced_ray_types` /
  named miss table / `group×ray_type→shader` mapping / 未充填 slot の bytes と
  到達不能性)
- record ABI の型表(i32/u32/f32/vec2/vec3/vec4)と offset 算法 + `vec3+f32` の実例、
  および **SPIR-V shader-record ABI 一致の load 時検証**
- `sbtRecordStride == R` 等の範囲検査、checked `q×R`、24-bit 上限
- 二段 commit(cache は append-only で対象外、新世代は TLAS submission まで非公開)
- 分岐は root authored document の一度きりの parse から typed sidecar として抽出
- 実 renderer の A/B 対照(RT1a の golden 代用負債もここで返す)

第 3 巡は **(A)(B) の転換が成立するかだけ**を検証する狭い巡とし、それで打ち切る。

#### 第 3 巡(狭い検証)の結果と v4 決定 —— 仕様はここで確定

**(A)(B) の転換は成立**(「壊せなかった項目」: tuple の単射性 / 編集跨ぎの q 安定性が
不要であること / 同一 material の一様束縛 / layout 不在時の legacy 互換)。
残る 4 件は未定義であり、以下で決定して**仕様レビューを打ち切る**。

1. **row 順は publish 順から独立させる**(#1): TLAS 列位置は登録順の出力なので
   key に使わない。**snapshot 内容から計算した sort key で q を採番し、その後で
   TLAS を構築**する。sort key =
   `(asset_identity, geometry_allocation_id, index_offset, vertex_offset,
     material_ref, transform の正準 byte 表現)`。
   全要素が一致する row は**あらゆる観測点で同一**(g・data も material_ref から
   決まるため)なので、その中の順序は不問。受け入れは
   **同じ論理 row 集合を逆の publish 順で本番経路から作り、row→q・TLAS・SBT data の
   bytes 一致**
2. **幾何的に一致する row の束縛不一致は名前付き拒否**(#2): 同じ primitive が
   複数 material group に置かれると、同一 BLAS・同一 transform・同一 t の row が
   複数でき、Vulkan はどちらを closest に採ってもよい(仕様上、同 t の候補は
   採用・破棄いずれも合法)。よって
   **`(geometry_allocation_id, index_offset, vertex_offset, transform)` が一致する
   row 群は、解決後の `(g, data)` が全て等しくなければならない**。
   違反は `ray_row_binding_ambiguous@1`(row の material_ref 一覧つき)。
   production GPU 負例を受け入れに置く
3. **material_ref の正本を定義する**(#3): `GlobalMaterialId`(再利用される runtime
   slot)も glTF の `name`(optional・非一意)も**使わない**。正本は 2 branch:
   - project material 文書由来: **文書 ref**(project 相対 path。既に
     `projectmaterialasset` が保存している)
   - model 埋め込み由来: **`(asset_identity, source_material_index)`**
     (asset_identity は AS snapshot に既在、source_material_index は DrawItem に既在)
   配管は「`source_material_index` と project 文書 ref を AS snapshot まで運ぶ」だけ。
   reload 後の再解決はこの 2 branch のどちらかへ再び解決されること(handle 再利用に
   依存しない)。受け入れ: handle 再利用・同名 material 二つ・project rebind
4. **selector の一意性規則**(#4): canonical material 解決**後**に selector は一意。
   重複は `ray_binding_selector_duplicate@1`、存在しない material への selector は
   `ray_binding_selector_unresolved@1`(いずれも宣言順に依存しない)。
   **束縛の無い row のみ** layout の default group。
   受け入れ: 有効 selector が解決した `g/data` の実値検査 + 重複・typo の負例

### WP366(確定版): RT1b —— layout・SBT addressing・record ABI・material 束縛

**これが WP366 の唯一の仕様である。**(v1 の「### WP365: RT1b」節、第 1 巡表、
v2、v3、上記 v4 決定は経緯として残すが、実装者は本節だけを読めばよい。
矛盾したら本節が勝つ。)規範設計は docs/design_raytracing.md v4 の §3・§5・§7・§11。
**§4 規則 11 上段。仕様レビュー打ち切り済み。コードレビューあり。見積: 大。**

#### 1. 3 domain(design v4 §3)

`g` = logical group / `r`,`R` = ray type index, count / `q` = scene record row /
`H[p,g,r]` = pipeline の handle。
**hit record index = `q×R + r`**、内容 = `H[p, g(q), r] ∥ data(q)`。
**operand 固定**: `instanceShaderBindingTableRecordOffset = q×R` /
`sbtRecordStride = R` / `sbtRecordOffset = r`。
raygen へ公開するのは `ray_type_index` / `ray_type_count` / `miss_index` のみ
(`g×R+r` と `q` は非公開)。`record_index_stride`(=R)と `record_byte_stride`
(byte)は常に別名。**1 BLAS = 1 geometry は恒久制約**(`geometryIndex` 恒等 0、
sentinel の検査対象から除外)。miss record は grid 外の独立配列。

#### 2. wire(完全な文法。実装はこれに合わせる)

**`pelican.ray_group_layout` v1**(独立ファイル。rendering config の
`"ray_group_layout": "<path>"` で参照。preset 合成に巻き込まない。
**presence は root authored document の一度きりの parse から typed sidecar として
抽出**し、flat/xr/preview 全経路へ渡す):

```json
{ "schema": "pelican.ray_group_layout", "version": 1,
  "ray_types": ["primary", "shadow"],
  "default_group": "opaque",
  "groups": [ { "name": "opaque" }, { "name": "foliage" } ],
  "misses": [ { "name": "sky" }, { "name": "shadow_open" } ],
  "record": [ { "name": "tint", "type": "vec4" },
              { "name": "flags", "type": "u32" } ] }
```

- 名前の字句 `^[A-Za-z0-9_]{1,64}$`(**hyphen 禁止** —— 生成マクロ名に使えない)。
  `ray_types` / `groups` / `misses` は非空・重複禁止。`default_group` は
  `groups` に存在必須。`record` は空可。unknown field 拒否
- **layout digest** = ファイル raw bytes の SHA-256

**task 側**(現行 `ray_tracing` block の拡張。旧 3 field 形は layout 不在時のみ有効):

```json
{ "ray_tracing": {
    "raygen": "project://shaders/pt",
    "traced_ray_types": ["primary"],
    "misses": { "sky": "project://shaders/pt_sky" },
    "hit_shaders": { "primary": { "opaque": { "closesthit": "project://shaders/pt_hit" },
                                  "foliage": { "closesthit": "project://shaders/pt_hit" } } } } }
```

- `traced_ray_types` ⊆ layout の `ray_types`。**その ray type × 全 group** の充填必須。
  不足は task 名・group・ray type・available 一覧つきの名前付きエラー
- `misses` の key は layout の `misses` 名。充填必須
- **未充填 slot**(trace しない ray type の record)は
  **`H` に有効な no-op group handle を置き data は 0 埋め**(未初期化 bytes を作らない)。
  到達不能性は §4 の operand 検証(offset ∈ traced_ray_types)で保証

**`pelican.ray_bindings@1`**(独立ファイル。config の `"ray_bindings": "<path>"`):

```json
{ "schema": "pelican.ray_bindings", "version": 1,
  "bindings": [
    { "material": { "document": "materials/leaf.mat.json" },
      "group": "foliage", "record": { "tint": [0.2, 0.8, 0.2, 1.0], "flags": 1 } },
    { "material": { "asset": "models/tree.glb", "source_material_index": 2 },
      "group": "foliage", "record": { "tint": [0.3, 0.7, 0.3, 1.0], "flags": 1 } } ] }
```

- selector は **material のみ**(v4 決定 3 の 2 branch)。`document` と
  `asset`+`source_material_index` は排他
- `record` は layout の `record` member 名の**完全一致集合**(過不足・型不一致は
  `ray_binding_record_invalid@1`)
- 解決後の selector 重複 = `ray_binding_selector_duplicate@1` /
  未解決 = `ray_binding_selector_unresolved@1` / 束縛の無い row のみ `default_group`

#### 3. record ABI

型 whitelist v1 = `i32` / `u32` / `f32` / `vec2` / `vec3` / `vec4`
(他は `ray_record_type_unsupported@1`。8/16/64-bit・bool・enum・quat・string は
capability facts が要るため v1 対象外)。

| 型 | GLSL | size | alignment |
|---|---|---|---|
| i32 / u32 / f32 | int / uint / float | 4 | 4 |
| vec2 | vec2 | 8 | 8 |
| vec3 | vec3 | 12 | **16** |
| vec4 | vec4 | 16 | 16 |

- 配置は宣言順に std430。**例**: `[vec3 a, f32 b]` → `a@0(12B)`, `b@12`,
  `data_size = 16`
- `data_start = alignUp(shaderGroupHandleSize, shaderGroupHandleAlignment)`
- `record_byte_stride = alignUp(data_start + data_size, shaderGroupHandleAlignment)`
- **終端 padding と未充填 slot は 0 埋め**
- offset/size 表を**正準化 JSON に出力**し mirror test で固定
- `record_byte_stride > maxShaderGroupStride` は名前付き拒否
  (RT1a で延期した fact をここで実装・使用)
- **shader 側 ABI 一致を load 時 invariant に**: final SPIR-V の shader-record
  storage の member 型・offset が canonical ABI と完全一致すること
  (`rtspirvinspection` に shader-record storage の抽出を追加)。
  shader 宣言だけを変えた負例で production pipeline が named reject されること

#### 4. 検証(純 validator。RT1a の様式を継承)

`sbtRecordStride == R` / `sbtRecordOffset` が `traced_ray_types` の index /
`missIndex` が layout の `misses` 範囲 / **checked `q×R` と 24-bit 上限**
(`ray_row_count_limit@1`)/ SBT record count・byte size の overflow。
**1-geometry fixture で誤 stride(0 や 1)を named reject する負例が必須**
(stride 項が恒等消滅するため sentinel では検出できない)。

#### 5. row・q・束縛の解決

- row は **material_ref を含む sort key**(v4 決定 1)で並べ、q を 0 から採番
- 幾何一致 row の束縛不一致は `ray_row_binding_ambiguous@1`(v4 決定 2)
- material_ref は v4 決定 3 の 2 branch。配管は `source_material_index` と
  project 文書 ref を AS snapshot まで運ぶ

#### 6. 共同 transaction(design v4 §11-1・第 1 巡 #5)

- **対象** = layout digest + shader bundles + pipeline objects + **完全な SBT** +
  row snapshot + TLAS。**shader cache は content-addressed append-only で対象外**
- **二段 commit**: 新世代は **TLAS submission 成功まで非公開**
- **invalidation 要因**: layout digest / bindings digest / row 集合 / record ABI
- **「全 SBT」= active owner scope の flat + xr pipelines**
- failure point: compile 後・pipeline 後・SBT 後・AS record 後・submit で
  **active generation が旧世代のまま**であることを観測

#### 7. 生成 include と cache

- 生成 macro は `PELICAN_RAY_TYPE_<NAME>` と `PELICAN_MISS_<NAME>` のみ
- **layout digest を生成 virtual source の salt に必ず含める**
  (group 並べ替え・whitespace のみの変更でも cache key が変わること)
- RT task の virtual include 空化を解除する seam を実装(現在 RT 経路は
  意図的に空)。**layout path を reload dependency に登録**
- **layout 使用 × 明示 `.spv` は source-only named reject**
- **compiler OFF × layout 使用 project は named reject**。layout 不在 project は
  compiler ON/OFF 両対応のまま

#### 8. 受け入れ(§4 規約 10)

- **旧 lowering の byte 保存**: layout 不在 project は frame plan・SBT bytes・
  GPU 出力・cache key が RT1a 時点と一致(既存 `rt_shadow_mask_pipeline` が親版)
- **publish 順非依存**: 同じ論理 row 集合を逆順で本番経路から作り bytes 一致
- **sentinel**: 二 pipeline × 二 instance の production 配線で、**q ごとに一意な
  data(全 r で複製)** を書き、`q×R` と `r` と miss index を復元。
  `r` は hit shader の compile-time marker、`g` は group ごとの shader marker で識別。
  **TLAS base を誤値に差し替える否定対照**
- **束縛**: 有効 selector の `g/data` 実値 / 重複・未解決・record 過不足の負例 /
  幾何一致 row の不一致拒否(production GPU)
- **ABI**: 正準 JSON と実 SBT bytes の一致 / `vec3+f32` の offset / mirror test /
  shader 宣言変更の named reject / stride 超過拒否
- **transaction**: 5 failure point で旧世代維持 / cache が対象外であることの確認
- **cache**: group 並べ替えのみ・whitespace のみで cache key と compile 回数が変わる。
  RT include hookup を無効にすると落ちる
- **実 renderer の A/B 対照**(RT1a の golden 代用負債をここで返す):
  固定 fake handle の純 pack baseline + production 分岐 observer +
  **実 renderer を同一 run で A/B**
- 出荷 4 project の bytes・golden・frame plan 不変(RT 未使用。親版 manifest で検証)
- matrix: SKIP_DEVSTUDIO 両 / SPIRV_LINK ON/OFF / compiler ON/OFF /
  OPENXR ON/OFF / ImGui ON/OFF / RT capability 不在 / 全数 2 回 / GPU 直列は統合側

#### 9. やらないこと

per-instance 束縛(永続 authored identity の設計が入る段で再訪。RT3 の instance
flags と同時)/ `.rahit`(RT2a)/ material ABI・bindless・両車線 alpha(RT2b)/
AS 宣言面(RT3)/ callable・動的 record(RT4)/ 変形 geometry(RT5)/
複数 geometry BLAS・indirect・AS graph node・複数 TLAS(RT6)/ 編集 RPC(RT7)/
offline bake(RT-bake)。

依存: WP364 + WP364a。**WP365 は廃止(挙動を変えない基盤 WP のため)。**

#### WP366(RT1b)完了(2026-08-31)

本体 + GPU fixture 修正。統合: 全数 **1463 ×2 緑**(GPU 直列込み)、doclink 2341 ok。
基準値 N=1463(RT1a 完了時 1452 + WP366 11)。

成立したもの: `pelican.ray_group_layout`(独立ファイル・sidecar 一度きり抽出)/
3 domain の実装(`q×R+r`、operand 固定、production TLAS が `q×R` を書く)/
std430 の record ABI(型 whitelist v1、`vec3@0+f32@12`、正準 JSON + mirror)/
**final SPIR-V の shader-record ABI 一致を load 時検証**(`shader_record_abi_mismatch@1`)/
material 束縛 overlay(`pelican.ray_bindings`、2 branch の material_ref)/
row の内容 sort と幾何一致 row の束縛不一致拒否 / 二段 commit(cache は対象外)/
生成 include と layout digest salt / 新規 named code 10 種。
出荷 4 project・core resources・既存 RT feature は bytes 不変。

統合中に閉じた 1 件(閉ループ内): GPU fixture の shader 8 ファイルが
**全て黙って書かれていなかった**(`writeTextFile` が親ディレクトリを作らず
書き込み失敗も検査しない)。原因は temp dir の実見で特定。

##### 台帳のみの負債(この統合で判明)

`test/headless_render_test.cpp` の `writeTextFile()` は親ディレクトリを作らず、
書き込み失敗も検査しない。**「書いたつもりのファイルが無い」形の偽陽性を
どの fixture でも生みうる**。他の fixture が既存ディレクトリに書いているため
現状は露見していないだけ。硬化(親ディレクトリ作成 + 失敗時 throw)は
test 基盤の別 WP か、次に test/ を大きく触る WP に同乗させる。

次: WP366 の §10 コードレビュー(fast_mode 適用)→ 先行裁定の作業順どおり RT2a。

#### WP366 コードレビュー(不合格・10 件、致命 1)の仕分けと WP366a

##### 裁定: revert ではなく修正パッケージ(理由を残す)

決定表の即断基準は「use-after-free 級 = 問答無用で revert」だが、これは
**WP361b v2 の文脈(同じ機構への 2 度目の失敗で、UB が導入機構の中核にあった)**
で書かれたものである。本件は当てはまらない:

- **本体 WP の初回レビュー**であり、表の第 1 行(実害 → 修正パッケージ 1 回)に該当
- **中核設計は生存**: 3 domain の代数・SBT packing(untraced slot・終端 padding・
  `vec3@0+f32@12`・overflow)・cache の rollback 除外・material の handle/name 分離は
  いずれも「壊せなかった項目」
- UAF は**新規に足した staged publication の配管**にあり、処方(span を所有 vector へ
  copy、prepared state を所有 transaction へ move)が具体的で局所
- **出荷構成では到達不能**: 遅延実行は layout 世代のみで発生し、出荷 4 project は
  `ray_group_layout` を 0 件しか持たない(実測)。layout 不在の reload は即時実行のため
  span は生存している

**WP366a を 1 回だけ。以降の指摘は無条件で台帳。**

### WP366a: RT1b の修理(最終。以降の指摘は台帳)

**§4 規則 11 上段。コードレビューのみ(仕様源は上記 10 件)。見積: 中〜大。**

1. **致命・UAF**(#2): renderer が `companion_requests` の `std::span` を lambda に
   値 capture しているが、実体は ReloadService の局所 `companion_batch` で、
   layout 世代では lambda が次 submit まで遅延されるため破棄済み。
   **span を所有する `std::vector<ReloadRequest>` へ copy**。
   authoring 経路も `[&, default_pass_name]` で prepared 局所と `source_commit` の
   参照を捕捉しているため、**所有する transaction object へ move**。
   API は `committed` でなく **`staged` を返し submit 後に完了通知**
   (現在は callback 未実行のまま invalid pass ID・空 watch set・generation 0 を
   公開して committed を返している)
2. **legacy TLAS の byte 非等価**(#1): `instanceCustomIndex` が旧版の
   「TLAS 内の密な行番号」から draw inventory の source index に変わっている。
   **legacy branch では `instance_custom_index = result.size()`**(source index は
   lookup 専用)。受け入れ: **先頭に除外 instance(skinned 等)を置いた
   null-layout 回帰**で親版 TLAS bytes と一致
3. **late failure で runtime 以外が candidate のまま**(#3): renderer の pass ID・
   公開 config・watch dependency・履歴・reload counters が candidate へ更新済みで、
   rollback するのは staged_publication だけ。**これらも staged payload に含め、
   `publishStagedGeneration` と同じ commit 点でのみ move-publication**。
   5-point テストは **ReloadService 経由**にし、失敗後にもう一度 render して
   旧 config/watch/output を検査(現在は loader 直呼びで active runtime と TLAS のみ)
4. **raygen/miss の shader-record を受理して data を書かない**(#4):
   validator の `allowed` と `required` を分け、**raygen/miss/untraced hit では
   shader-record storage 自体を named reject**
5. **幾何一致判定が asset/mesh metadata に依存**(#5): q の sort key と
   「幾何一致 key」を**分離**し、後者は **GPU geometry allocation/range と
   canonical transform のみ**(asset/material/mesh/node 名を含めない)。
   負例: 同一 geometry・同一 transform で asset alias / mesh_index 違いの束縛不一致
6. **`ray_type_count` が shader に公開されていない**(#6): 仕様が要求している。
   macro 制約と衝突しない予約済み generated constant
   (例 `const uint pelican_ray_type_count = Ru;`)を公開し、
   **fixture と実 shader が literal でなくそれを operand に使う**
7. **stride 負例が production 配線を検証していない**(#7): final SPIR-V validator から
   `validateRayTracingRecordAddressing` 呼び出しだけを外しても純テストは通る。
   **実 config/PipelineFactory から stride `1` の raygen を load** して
   named reject を確認し、hookup 無効化で同じ shader が通ることも同一テストで。
   `ray_type_offset_invalid@1` と `miss_index_invalid@1` も同様
8. **material_ref 2 branch が production で証明されていない**(#8): GPU fixture が
   `authored_asset_reference` と index を手書きしており、document propagation を
   空にしても通る。**実 project material 文書と最小 glTF を loader から読み**、
   reload・handle 退役/再利用・同名 material 拒否まで一つの production fixture で
9. **cache/salt が代理条件**(#9): 直接 cache テストは generated include を手挿しして
   vertex shader を compile しており RT task の hookup ではない。
   **ReloadService 経由で group reorder と whitespace を別々に行い、production の
   cache key/requests/misses を観測**。RT hookup を無効化する scoped seam も同一テストで。
   併せて **`writeTextFile` を fail-fast 化**(親ディレクトリ作成 + open/write/flush
   検査 + 直後の exact bytes 再読込)—— 台帳の負債(#WP366 完了節)をここで返す
10. **matrix が build だけで RT1b を走らせていない**(#10): feature smoke は反対構成を
    full build するが RT1b テストを実行せず、compiler-OFF branch は無条件 `SUCCEED`。
    **各 contrast build で `raytracing_rt1b_test` を実行**し、compiler-OFF の
    named reject・preview sidecar・OpenXR owner の全 SBT・capability 不在を
    targeted test として走らせる

**受け入れ全体**: 各修正に「取り消したら落ちる」対照 / 出荷 4 project・
core resources・既存 RT feature の bytes 不変 / 全数 2 回(統合側 GPU 直列)/
matrix は #10 のとおり実行まで。

依存: WP366。**連鎖打ち切り: WP366a の §10 指摘は無条件で台帳。**

#### wp336(shader footprint 宣言)の裁定: 保留継続(2026-08-31、利用者判断)

worktree `<作業ディレクトリ>/wp336`、branch `agent/wp336`、独自コミット **1 本**(`31f09712`)。
現ブランチから **143 遅れ**(以前この節に書いた「1196 先行」は main 基準の数え方で
誤解を招くため訂正する。現ブランチとの差分は 1 コミット)。

内容: シェーダが読む近傍範囲を `//!` ヘッダで宣言する配管。`.surface` と同じ
tokenizer で解析し、グラフ解析前に解決して execution plan へ届ける。
`ssao_blur.frag` は 5×5 を読むのに fullscreen パスは `input_footprints` を
一度も埋めず全て `arbitrary` に落ちていた。**シェーダの性質であり、パスの作者は
知り得ない**(台帳の同趣旨: 「契約(型・footprint・sampling・fallback)…
シェーダーの性質。パスの著者は知り得ない」)。
影響は frame plan の 16 フィールド(値とその fingerprint と診断文字列)のみで、
representation・backend 選択・scope・attachment・alias group は byte 一致。

**保留の理由(コミット自身が書いている)**: `same_pixel` を使い分ける経路が
エンジンに無いため、**単独では挙動を変えない**([[slice-must-change-behavior]])。

**利用者裁定(2026-08-31): 保留継続。マージも破棄もしない。**
footprint を実際に使う WP —— footprint の 3 経路
(`material_resources.footprint` / `input_footprints` / `read_footprints`)を
`resource_ports` へ寄せるソケット整理、または alias 可否判定の最適化 —— を
起票するときに、**この実装を参考実装として読む**。143 遅れているので、
その時点でコンフリクト解消より書き直しが速い可能性が高い。
**この判断を再検討しないこと**(覆すなら理由をここに追記してから)。

#### WP366a 完了 —— RT1b 確定(2026-08-31)

統合: 全数 **1467 ×2 緑**(GPU 直列込み)、doclink 2341 ok。基準値 N=1467。
10 件すべて修理、UNMET 0。

修理の要点: 遅延 callback が所有する vector を持つ(UAF 解消。残る `span` は
関数パラメータで正当)/ authoring は prepared state を所有 transaction へ move し、
**`committed` でなく `staged` を返して submit 後に完了通知** / legacy null-layout は
`instance_custom_index = result.size()`(除外 instance を飛ばして数える)/
raygen・miss・untraced hit では shader-record storage を named reject /
幾何一致 key は **GPU allocation・range・canonical transform のみ**(asset 名を含めない)/
`pelican_ray_type_count` を生成定数として公開(fixture の literal 撤去)/
5 failure point で status・config・plan・TLAS を維持 / material_ref 2 branch を
実 project material と実 glTF で(reload・退役・再利用・同名衝突まで)/
`writeTextFile` の fail-fast 化(台帳の負債を返済)/ **5 contrast build すべてで
RT1b executable を実行**。

**中断からの再開で判明した重要事実**: 静的には完了に見えた #7(stride の production
負例)と #10(matrix)が、**実際にテストを走らせたら壊れていた**。
前回の実行は sandbox のパイプ切断でテスト到達前に中断していた。
副産物として FetchContent のハイフン付き cache 変数転送と SKIP_DEVSTUDIO の
残存 test target も修正。

**RT1b はここで確定**(WP366 + WP366a)。連鎖打ち切り済み: 以降の指摘は台帳のみ。
次は先行裁定の作業順どおり **RT2a(anyhit 縦切り)**。

### WP367: RT2a —— any-hit の縦切り(`.rahit` 語彙 + typed group + subset オラクル)

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 中〜大。**
**規範は docs/design_raytracing.md v4 の §4・§6・§7・§8(RT2a)・§11-6。
alpha は含めない(§11-6 で RT2b へ移動済み)。**

#### 前提(RT1a + RT1b で揃っている)

`ray_group_layout` と 3 domain(`q×R+r`、operand 固定)/ record ABI(std430、
型 whitelist、`shader_record_abi_mismatch@1`)/ `pelican.ray_bindings` の
material 束縛 / `rtspirvinspection`(全 entry・operand 定数性・payload 型・
shader-record storage)/ 純 validator + 4 配線 + delegate 注入点 /
CPU 参照オラクル(外向き丸め、subset 規範表)/ 二段 commit / 生成 include と salt /
5 contrast build での RT1b 実行。

#### 1. 語彙の追加(**使う段で入れる**。`.rint`/`.rcall` は入れない)

`.rahit` を**全所同段**で追加する(現状 `.rgen/.rmiss/.rchit` が閉じている 6 箇所 +
表示 2 箇所):
`src/project/shadersourceresolver.cpp`(**wire 正本**。suffix と受理集合)→
`src/core/shader/shaderreference.cpp` / `shaderlibrary.cpp` / `pipelinefactory.cpp` /
`rtspirvinspection.cpp` / `src/core/vkcore/shaderresolution.cpp` /
`src/core/renderingpass/computetask.cpp` → 表示は
`src/core/imgui/planviewer.cpp` と `src/devstudio/model/frameplanmodel.cpp`。
**D0: Studio は wire 経由のみ**(`pelican_core` をリンクしない)。

受け入れ: `.rahit` を含む shader が **全構成**(compiler ON/OFF、SPIRV_LINK ON/OFF)で
解決・compile・登録され、PlanViewer と Studio の frame plan に stage として現れること。
**どれか 1 箇所を旧のままにすると落ちる**対照(語彙の閉包検査)。

#### 2. typed hit group(現行の平坦形からの拡張)

現行 WP366 の `hit_shaders` は `{"closesthit": "<ref>"}` **のみ**を許し、
`group.size() != 1` で拒否している(computetask.cpp:1370)。これを typed 形へ:

```json
"hit_shaders": {
  "primary": {
    "opaque":  { "type": "triangles", "closesthit": "project://shaders/pt_hit" },
    "foliage": { "type": "triangles", "closesthit": "project://shaders/pt_hit",
                 "anyhit": "project://shaders/pt_alpha" } } }
```

- `type` は省略可、既定 `"triangles"`(**旧形 `{"closesthit": ...}` は byte 保存**)。
  `"procedural"` は **RT3 まで名前付き拒否**(`ray_hit_group_type_unsupported@1`)——
  「存在するが必ず拒否」の語彙を出荷しないという原則との整合: `type` の**値**は
  文法上受理して名前付きで拒否する(stage 語彙 `.rint` 自体は入れない)
- `anyhit` は省略可。指定時は `.rahit` として解決
- unknown field は `ray_hit_group_invalid@1`
- **RT1a の絶対規則を anyhit にも適用**: triangles group の hit attribute は
  `vec2`(f32×2)固定。anyhit が独自 attribute を宣言したら
  `hit_attribute_forbidden@1`
- **shader-record storage は traced hit slot でのみ許可**(WP366a #4)。
  anyhit も同じ扱い —— anyhit は closesthit と**同じ record**を読む
  (同一 group の同一 `(q,r)` record であることを sentinel で確認)

#### 3. subset any-hit(design v4 §4 の許可集合)

- **許可**: 候補局所の入力(barycentric、primitive/instance ID、
  **record data 定数**)からの**純関数** accept/ignore のみ
- **subset 外**(書けるが cross-device golden 非保証の分類。**エラーではない**):
  payload 書き込み・texture sample・`terminateRayEXT`・副作用
- subset 判定は**静的に機械判定できる範囲**で行う: `rtspirvinspection` に
  anyhit 本体の検査を追加し、payload storage への書き込み・image/sampler 参照・
  `OpTerminateRayKHR` の有無を抽出。**subset 外は拒否せず、frame plan と
  診断に `golden_portable: false` として現れる**(G0 可読性: 暗黙の分類を見せる)
- **自由 `.rahit` は pipeline 車線専用**(§6)。ray query 側に等価物は要求しない

#### 4. オラクル(design v4 §4 の subset 契約を anyhit へ拡張)

- CPU 参照器に **candidate 列挙 + 純述語 accept/ignore** を追加。
  述語は record data 定数と barycentric/primitive ID からのみ計算する形に限る
  (fixture 生成器が**構成的に**その形の shader を出す)
- **off/on 対照は design §4 のとおり「機能 off」ではない**:
  同一 production scene で **always-accept 述語と実述語**を走らせ、
  GPU が実際に読んだ record ID / t / 可視性が**予告どおり変わる**ことを見る
- 一意性 margin(ε_t / ε_b)と区間幅上限は RT1a の数値表をそのまま使う
- **anyhit が ignore した候補は closest から除外される**ことを、
  CPU 区間と GPU の hit t の包含で確認(市松状に ignore する述語で、
  ignore 無しなら手前の triangle、ignore 有りなら奥の triangle が hit になる fixture)

#### 5. 受け入れ(§4 規約 10)

- **語彙の閉包**: §1 の 8 箇所のどれか 1 つを旧のままにすると落ちる
- **旧形の byte 保存**: `type` 無し・`anyhit` 無しの既存 task は frame plan・
  SBT bytes・GPU 出力・cache key が RT1b 時点と一致(既存 fixture が親版)
- **anyhit が効く**: always-accept vs 実述語で GPU の hit t / record ID が変わり、
  **CPU 参照の区間に包含**される。anyhit を group から外すと落ちる
- **同一 record**: anyhit と closesthit が同じ `(q,r)` record の sentinel を読む
- **subset 分類**: payload 書き込みや texture sample を持つ anyhit が
  **拒否されず** `golden_portable: false` として frame plan に現れる。
  分類を外すと落ちる
- **絶対規則**: anyhit が `vec3` hit attribute を宣言したら
  `hit_attribute_forbidden@1`(production load 経路で)
- **procedural の拒否**: `"type": "procedural"` が
  `ray_hit_group_type_unsupported@1`(RT3 で解禁予定であることを文言に含む)
- 出荷 4 project・core resources・既存 RT feature の bytes 不変
- matrix: **5 contrast build で RT2a テストを実行**(WP366a #10 の枠組みを使う)/
  SKIP_DEVSTUDIO 両構成 / SPIRV_LINK ON/OFF / compiler ON/OFF /
  OPENXR ON/OFF / ImGui ON/OFF / 全数 2 回 / GPU 直列は統合側

#### 6. やらないこと

**material alpha(実 texture の alpha test)と両車線 portable profile は RT2b**
(§11-6。alpha を判定するには material ABI —— bindless texture 表と
geometry access —— が要るため)。`.rint`(RT3)/ `.rcall`(RT4)/
per-instance 束縛(永続 identity 設計の段)/ 変形 geometry(RT5)。

依存: WP366 + WP366a。

#### WP367 仕様レビュー(v1、差し戻し・11 件)と v2

**全件「直す」。v2 は v1 との差分で、矛盾したら v2 が優先。**第 2 巡を最終とする。
壊せなかった項目: alpha の RT2b 送り / 自由 `.rahit` を pipeline 車線に限る方針 /
同一 candidate で any-hit と closest-hit が同じ record を読むという Vulkan 主張 /
always-accept A/B が原理的に可能なこと / 旧形の byte 保存が原理的に可能なこと / D0。

| # | v1 の壊れた点 | v2 の決定 |
|---|---|---|
| 1 | compiler OFF で `.rahit` が resolve/compile される受け入れは**成立不能**(layout project は OFF で `layout_requires_runtime_compiler@1` 拒否が RT1b の確定仕様) | **機能試験は compiler ON のみ**。OFF 対照は「`.rahit` を含む layout project が同じ名前付きエラーで拒否され、**shader document を一件もロードしない**」ことの検査に置換 |
| 2 | 3 種の抽出では subset を判定できない(payload **読み**での分岐 / readonly UBO・SSBO / atomic・store / 非許可 built-in / helper の `inout` / 未到達 helper の誤検出 / 未使用 descriptor の誤検出) | **保守的認証器**にする(§後述)。selected entry point の**推移的閉包**に限定し、**許可 opcode・許可 storage class・許可 built-in の whitelist**。pointer origin を parameter / access-chain / phi / select 越しに追跡。不明・alias 未解決・未知 opcode は**拒否ではなく非認証(reason code つき)**。**module 全体の文字列・opcode 存在検査は禁止** |
| 3 | `golden_portable` の受け入れが**恒偽実装で通る** / float predicate の device 間一致が未定 | 名称を **`subset_certified`** に変更(「golden が可搬」ではなく「認証済み subset」)。**肯定認証を必須**にし、reason ごとに 1 つずつ負例(payload read / payload write / SSBO write / atomic / image access / terminate / 非許可 built-in / alias 未解決)。**golden fixture の述語は単一の生成器(DSL/AST)から GLSL と CPU オラクル述語の双方を生成**する —— これで float 境界問題も同時に閉じる(任意 SPIR-V と CPU の一致は主張しない) |
| 4 | optional any-hit の `(g,r)` wire 表現が未定義(圧縮すると対応を失い、疎配列は strict consumer が拒否) | **固定長 `ray_hit_groups[]`**(`g×R` 全要素、欠けは null)を出す。各要素 `{group, ray_type, hit_slot, closesthit, anyhit?, subset_certified, reasons[]}`。順序は `g×R+r` 昇順。**旧タスクでは field ごと省略**(byte 保存) |
| 5 | portability 判定だけが変わったとき frame plan identity/cache が変わらない | **独立した `shader_resolution_fingerprint`** を新設(active final SPIR-V digest + 認証 profile version + 判定結果 + reasons)。既存の execution-plan fingerprint の契約は**変更しない**。旧タスクでは省略。reload 成功時は pipeline / SBT / 分類 / fingerprint を**同一 publish transaction** で切替え、失敗時は全て旧世代維持 |
| 6 | 「8 箇所」の閉包検査が代理条件(文字列 scan や dead table で満たせる)/ **compiler 経路 3 箇所と task enum が漏れている** | **箇所数を受け入れ条件にしない**。追加すべき経路として `shadercompiler.cpp` の stage→shaderc kind / engine shader suffix universe / path suffix→stage、および `renderingpass.hpp` の task enum を明示。受け入れは**本番 observable 8 件**(canonical resolver / AnyHit の shaderc compile / final SPIR-V の stage 検査 / hit group の `anyHitShader` / active pipeline と SBT / frame-plan の occurrence mapping / PlanViewer・Studio の strict parse / `.rahit` 編集での hot reload)。**ソース文字列検査は明示的に禁止** |
| 7 | any-hit が既存 ABI 検査(payload / attribute / record)に入っていない | validation input を **物理 `(g,r)` occurrence の `{closesthit, optional<anyhit>}`** に変更。両 stage の payload 構造・triangle attribute `vec2`・record ABI・incoming interface 個数を検査。`vec3` attribute だけでなく **payload 不一致・record 不一致・複数 incoming payload** も production load 経路で名前付き拒否 |
| 8 | 「同じ `(q,r)` record」の試験が candidate を取り違えている(前面 ignore / 背面 hit は別 candidate。`data(q)` は全 `r` に複製されるので `q` sentinel だけでは `r` の証明にならない) | **試験を 2 つに分離**: (a) **単一 candidate の accept fixture** で any-hit と closest-hit の `q` sentinel・**compile-time `r` marker**・group marker が一致 / (b) **二 candidate fixture** で前面 ignore・背面 hit(二枚は distinct `q` が要るなら別 instance/geometry)。**any-hit の呼び出し順は規範上不定なので、副作用ログの順序をオラクルにしない** |
| 9 | always-accept 対照の差し替え粒度が未指定 / 既存 fixture が `Opaque` flag を使っており any-hit が走らない | **同一 generation・同一 row snapshot 上に、実述語と always-accept の 2 本の完全な production pipeline/task** を作る(stage 単体差し替えは不可能)。**ray flags から `Opaque` / `TerminateOnFirstHit` / `SkipClosestHit` を除き、geometry・instance も opaque にしない**。`pipelineUsesShader` に `.rahit` を追加し、**`.rahit` 編集で pipeline 全体が再生成**され、失敗 reload では旧 pipeline・SBT・分類が残ることを同一テストで |
| 10 | `type:"procedural"` を文法受理して拒否するのは「doomed vocabulary を出荷しない」と**直接矛盾** | 文法を **`type ::= absent \| "triangles"`** に限定。他の値は `ray_hit_group_invalid@1`(診断のため raw token を読んでよいが、**typed AST の受理集合に入れない**)。procedural は **RT3 で `.rint` + AABB geometry + procedural group と原子的に**追加 |
| 11 | 5 contrast build の「実行」だけでは RT2a を通った証明にならない(既存 RT1b runner を走らせるだけで表面的に満たせる) | 各 contrast に **RT2a の具体的 test ID と observable を列挙**。compiler ON は実述語 / always-accept の GPU 差と wire、compiler OFF は確定済みの named reject |

##### subset 認証器の規範(#2・#3 の実体)

- **対象**: selected entry point から推移的に到達する関数のみ(未到達 helper は無視)
- **許可 built-in**: `HitKindKHR` / `PrimitiveId` / `InstanceId` / `InstanceCustomIndexKHR` /
  `GeometryIndexKHR` / hit attribute(barycentric)
- **許可 storage class**: `Function` / `Private`(読み書き可)/ shader-record buffer(**読みのみ**)/
  hit attribute(読みのみ)
- **禁止**(= 非認証 + reason): incoming payload の**読みも書きも** /
  `Uniform` / `StorageBuffer` / `Image` / `Sampler` の参照 / atomic /
  `OpTerminateRayKHR` / `OpTraceRayKHR` / `OpExecuteCallableKHR` /
  上記以外の built-in / pointer origin 不明
- 判定は**保守的**: 判定できないものは非認証。**拒否はしない**(利用者は書ける)
- reason code は正規化した固定集合(wire に出る)

依存: WP366 + WP366a。

#### WP367 第 2 巡(FATAL 7 件)と v3 —— subset 認証器を捨てる

**v3 が最終。仕様レビューはここで打ち切る。v3 > v2 > v1。**

##### 裁定: 認証器は RT2a から削除する

7 件のうち **#1(whitelist 表が未定義)/ #2(pointer origin の停止条件)/
#3(未初期化 `Function`/`Private`)/ #4(単一 AST では float の意味差が閉じない)**
は、すべて **v2 が導入した subset 認証器**から出ている。これを閉じるには
opcode・storage・BuiltIn・型/operand 条件の完全表、最小不動点の停止規則、
definite-assignment 解析、float opcode ごとの Vulkan 保証区間 —— を規範化する
必要がある。

**問い直す: 認証器は誰のためか。** 利用者の要求は「any-hit を自由に書けること」で
あって、可搬性の分類ではない。認証器の唯一の消費者は **CPU オラクルの自信**である。
それは**シェーダを解析するのではなく、fixture の側を構成的に縛れば足りる**:

- **golden fixture の any-hit 述語は単一の生成器から出す**。生成器は
  **整数・ID のみ**の述語(`gl_PrimitiveID`・`gl_InstanceCustomIndexEXT`・
  record の `u32`)を GLSL と CPU オラクルの双方へ出力する。**float 比較を生成しない**
  ので #4 は発生しない。認証ではなく**構成**で保証する
- **自由な `.rahit` は従来どおり書ける**(それが目的)。単に golden fixture に
  使わないだけ。wire に分類 field は載せない

これにより #1〜#4 が消滅し、#5(null slot の意味)と #6(fingerprint 配線)も
分類 field が無くなることで大幅に縮む。**#7 は RT2a の欠陥ではない**(下記)。

##### 第 2 巡 7 件の行き先

| # | 指摘 | v3 |
|---|---|---|
| 1〜4 | 認証器の whitelist / pointer origin / 未初期化 / float 意味差 | **認証器ごと削除**。fixture 生成器が整数述語のみを構成的に出す |
| 5 | 固定長 wire の null slot が自己記述的でない / strict parser と不整合 | **null を作らない**。`anyhit_slots: [{hit_slot, ...}]` を**存在するものだけ**昇順で出し、any-hit が皆無なら **field ごと省略**(旧形の byte 保存)。Studio と PlanViewer は同段で許可 field を更新(D0: wire 経由) |
| 6 | `shader_resolution_fingerprint` の配線先が実在しない | **分類が無くなったので新 fingerprint 自体が不要**。`.rahit` の内容変更は**既存の shader cache key**(source digest)で再 compile され、pipeline は `pipelineUsesShader` 経由で再構築される。frame plan は `anyhit_slots` の ref を映すのみ |
| 7 | `.rahit` の hot reload が完全 SBT と同時 commit できない | **RT2a の欠陥ではない ——`.rchit` が既に `pipelineUsesShader` の `closest_hits` に入っており(pipelinefactory.cpp:336)、同じ形で pipeline が SBT 前に commit される既存の穴**。RT2a はそれを `.rahit` へ広げるだけ。**台帳の負債として記録**し、WP366 の共同 transaction 契約との齟齬として別途扱う(RT2a の受け入れでは「`.rahit` 編集で pipeline が再構築される」ことのみ要求し、SBT 同時 commit は要求しない) |

##### v3 の RT2a 範囲(これが確定仕様)

1. **`.rahit` 語彙**を production 経路すべてに追加。v1 が挙げた 8 箇所に加え、
   第 2 巡 #6 が指摘した **`shadercompiler.cpp` の 3 箇所**(stage→shaderc kind /
   engine shader suffix universe / path suffix→stage)と
   **`renderingpass.hpp` の task enum** を含める
2. **typed hit group**: `type ::= absent | "triangles"` のみ(**procedural は文法に
   入れない**。RT3 で `.rint` + AABB + procedural group と原子的に)。
   `anyhit` は省略可、指定時 `.rahit` として解決。unknown field は
   `ray_hit_group_invalid@1`
3. **ABI 検査を `(g,r)` occurrence の `{closesthit, optional<anyhit>}` へ**:
   両 stage の payload 構造・triangle attribute `vec2` 固定・record ABI・
   incoming interface 個数。payload 不一致 / `vec3` attribute /
   record 不一致 / 複数 incoming payload を production load で名前付き拒否
4. **wire**: `anyhit_slots: [{hit_slot, <既存 stage 投影と同じ field>}]`(存在分のみ・
   昇順・皆無なら省略)。Studio と PlanViewer の strict parse を同段で更新
5. **オラクル**: 生成器由来の整数述語のみ。**always-accept と実述語の
   2 本の完全な production pipeline** を同一 row snapshot 上に作る
   (stage 単体差し替えは Vulkan 上不可能)。**ray flags から `Opaque` /
   `TerminateOnFirstHit` / `SkipClosestHit` を除き、geometry・instance も
   opaque にしない**(既存 fixture は `gl_RayFlagsOpaqueEXT` を使っており
   そのままでは any-hit が走らない)
6. **同一 record の証明は 2 fixture に分離**(第 2 巡 #8):
   (a) 単一 candidate の accept で any-hit と closest-hit の `q` sentinel・
   **compile-time `r` marker**・group marker が一致 / (b) 二 candidate で
   前面 ignore・背面 hit(distinct `q`)。**副作用ログの順序をオラクルにしない**
   (any-hit の呼び出し順は規範上不定)
7. **受け入れは本番 observable**(箇所数を条件にしない。ソース文字列検査は禁止):
   canonical resolver / AnyHit の shaderc compile / final SPIR-V の stage 検査 /
   hit group の `anyHitShader` / active pipeline と SBT / frame-plan の
   occurrence mapping / PlanViewer・Studio の strict parse / `.rahit` 編集での
   pipeline 再構築
8. **compiler ON のみで機能試験**。**OFF 対照**は「`.rahit` を含む layout project が
   `layout_requires_runtime_compiler@1` で拒否され、shader document を一件も
   ロードしない」。5 contrast には **RT2a の具体的 test ID と observable を列挙**
9. 出荷 4 project・core resources・既存 RT feature の bytes 不変 /
   旧形(`type` 無し・`anyhit` 無し)は frame plan・SBT・GPU 出力・cache key が
   RT1b 時点と一致 / 全数 2 回 / GPU 直列は統合側

##### 台帳のみ(WP にしない)

- **subset 認証器**: cross-device の golden 可搬性が実際に問題になったときに再訪する。
  そのときは第 2 巡 #1〜#4 の処方(完全 whitelist・最小不動点・definite-assignment・
  float opcode の保証区間)が仕様の種
- **hit shader の hot reload と共同 transaction の齟齬**: `.rchit`/`.rahit` の編集は
  `rebuildPrepared` で pipeline を先に commit し、完全 SBT はその後の render で作る。
  WP366 の「完全な SBT を共同 transaction に含める」契約と齟齬がある**既存の穴**。
  RT2a はこれを広げも直しもしない

依存: WP366 + WP366a。

#### WP367(RT2a)完了(2026-08-31)

統合: 全数 **1476 ×2 緑**(GPU 直列込み)、doclink 2341 ok。
基準値 N=1476(RT1b 完了時 1467 + 9)。UNMET 0。

成立したもの: `.rahit` 語彙が production 全経路(resolver → shaderc kind →
suffix 推論 → ShaderLibrary → final SPIR-V stage → task enum → PlanViewer/Studio)/
typed hit group(`type ::= absent | "triangles"`、**procedural は文法に無い**)/
ABI 検査の `(g,r)` occurrence `{closesthit, optional<anyhit>}` 化(payload・
`vec2` attribute・record・incoming 個数。**GPU 負例で Vulkan pipeline create 0 回**)/
sparse な `anyhit_slots` wire(存在分のみ昇順、皆無なら field 省略、両 strict
consumer 更新)/ **整数述語生成器**が CPU オラクルと GLSL の双方を生成 /
always-accept と実述語の完全 pipeline 2 本立て A/B(**`Opaque` flag を戻すと
actual=always になって落ちる**)/ single-candidate と two-candidate の分離証明 /
compiler OFF は `layout_requires_runtime_compiler@1` + document load 0 /
5 contrast build で RT2a の具体 test ID 実行。
旧形(`type` 無し・`anyhit` 無し)は validator digest まで完全一致。

**第 2 巡 FATAL 7 件のうち 4 件は v2 が導入した subset 認証器由来**であり、
v3 で認証器ごと削除して解消した(fixture 側を構成的に縛る方針へ)。
残 1 件(#7 hot reload と共同 transaction の齟齬)は `.rchit` に既存の穴と確認し
台帳へ。

次: WP367 の §10 コードレビュー → **RT2b(material ABI: bindless material 表・
geometry access・実 texture の alpha test・両車線 portable profile)**。

#### WP367 コードレビュー(不合格・8 件)の仕分けと WP367a

出荷物は不変(4 project + core resources の 67 blob を親版と一致確認)。
Vulkan の group 配線・A/B の単一 row snapshot・sparse wire の順序・typed grammar・
到達 pair の ABI 拒否は「壊せなかった項目」。**全 8 件「直す」= WP367a を 1 回だけ。
以降の指摘は無条件で台帳。**

### WP367a: RT2a の修理(最終。以降の指摘は台帳)

**§4 規則 11 上段。コードレビューのみ。見積: 中。**

1. **layout 形式の closesthit が frame plan から消える**(実害): 通常 stage 出力は
   `ray.closest_hits` しか走査せず、layout の `hit_shader_slots` を投影しない。
   **closesthit も物理 `hit_slot` 付きで wire に出し**、各 `anyhit_slots[].hit_slot` に
   対応する closesthit が必ず一件あることを producer と consumer の両方で検査
2. **sparse consumer が slot の正当性を検査しない**(実害): 重複・降順・空配列は
   拒否するが、**存在しない/範囲外/orphan な `hit_slot` を受理**する。
   wire に**物理 hit-slot domain を明示**し、anyhit slot が対応する typed closesthit
   slot に属することまで検査(既存テストが orphan slot を hand-wire しているので
   それも修正)
3. **payload ABI が trace 到達 pair しか比較しない**(実害): 未 trace の ray type で
   closesthit=`vec4` / anyhit=`uvec4` が通る。**trace reachability と独立に、
   全物理 `(g,r)` の incoming payload 型を直接比較**し、その後 trace の outgoing を
   pair の正準型と比較
4. **Studio が解析済み anyhit を表示しない**(実害): model は保存するが widget は
   `resolution.stages` だけを描く。**`anyhit[hit_slot=N]` を widget に追加**し、
   production wire を入力にした表示・open action のテスト
5. **後方互換の破壊**(実害): `.rahit` を既知拡張子に加えたため、旧 task が
   closesthit の stem に `"shaders/foo.rahit"` を使っていると
   「明示拡張子」と判定されロード前に拒否される(親版では `foo.rahit.rchit` と
   して解決できた)。**stage-aware にする** —— `.rahit` は anyhit の予約 suffix と
   して扱い、**他 stage の stem suffix としては従来どおり許可**
6. **「同じ record を両 shader が読んだ」が代理条件**(実害): closesthit は record
   block を宣言するだけで marker を出力に使っておらず、**single fixture の actual
   anyhit を always-accept に置換しても通る**。anyhit 側に compile-time `r`/group
   marker も無い。**closesthit が実際に record marker を読んで出力に反映**し、
   marker を 1 つ誤らせた否定対照で miss/ignore になること、双方が `r`/group marker
   を結果へ反映することを要求
7. **CPU と GLSL の述語が二重定義**(v3 指定違反): v3 は「**単一の生成器**から双方を
   出す」と定めたが、実装は定数を共有する**別々の 2 実装**。
   **小さな整数述語 AST/IR を唯一の定義**にして CPU evaluator と GLSL emitter を
   そこから生成する。GPU fixture は全 candidate の `primitive_id` が同じで instance と
   record が同時に変わるため、**入力を 1 つだけ誤らせる GPU 否定対照**を各入力に追加
8. **contrast smoke が「対象テスト 0 件」や GPU skip で緑になる**(実害):
   exact `ctest -R` 呼び出しに **`--no-tests=error` を付ける**。
   RT2a 必須 capability の job では **skip を失敗扱い**にし、
   flat/xr/preview のどれを機能保証するかを明示した行を置く

**受け入れ全体**: 各修正に「取り消したら落ちる」対照(代理条件禁止)/
出荷 4 project・core resources・既存 RT feature の bytes 不変 /
**旧形の byte 保存は #5 を含めて再確認**(`.rahit` stem の旧 task fixture を追加)/
全数 2 回(統合側 GPU 直列)/ matrix は #8 の形で。

依存: WP367。**連鎖打ち切り: WP367a の §10 指摘は無条件で台帳。**

#### WP367a 完了 —— RT2a 確定(2026-08-31)

統合: 全数 **1478 ×2 緑**(GPU 直列込み)、doclink 2341 ok。基準値 N=1478。
8 件すべて修理、UNMET 0。

要点: layout の closesthit も物理 `hit_slot` 付きで frame plan に投影し、
**orphan slot を拒否** / payload ABI を **trace 到達性と独立に全 `(g,r)` pair** で
比較 / Studio が `anyhit[hit_slot=N]` を表示し Open へ接続 /
**`.rahit` は AnyHit でのみ予約 suffix**(旧 task の `foo.rahit` stem は
`foo.rahit.rchit` へ解決。legacy fixture で回帰固定)/ closesthit が record marker を
**実際に読んで出力に反映**し、record・ray・group marker の単独誤りが miss/ignore に /
**述語は単一の整数 IR**(opcode ノード列を CPU evaluator と GLSL emitter が共に走査。
入力を 1 つずつ誤らせる GPU 否定対照つき)/ 必須 capability lane は
`--no-tests=error` + skip を失敗扱い(0 件時は CTest の非ゼロ終了まで要求)。

**RT2a はここで確定**(WP367 + WP367a)。連鎖打ち切り済み。
次: **RT2b(material ABI)** —— bindless material 表・geometry access(vertex/index の
device address)・`data(q)` への material/geometry index 充填・実 texture の
alpha test・両車線 portable alpha profile・WP354 drill-through の投影。

### WP368: RT2b —— material ABI(bindless 表・geometry access・実 alpha test)

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 大。**
**規範は docs/design_raytracing.md v4 の §3-2(v2 参照)・§6・§11-6 と、
台帳「引き継ぎ: 残 WP への先行裁定」の RT2b 裁定
(geometry = buffer device address / texture = descriptor indexing 配列)。**

#### 前提の実測(2026-08-31、起票時に確認)

- **`bufferDeviceAddress` は既に有効**(core.cpp:428/639)—— geometry access は
  新規 capability 無しで実装できる
- **AS snapshot が既に material identity と geometry range を運んでいる**
  (WP366/366a の成果): `material_asset_reference` / `material_document_reference` /
  `source_material_index` / `geometry_allocation_id` / `index_count` /
  `index_offset` / `vertex_offset` / `vertex_count`
- **descriptor indexing は未有効**(`runtimeDescriptorArray` /
  `shaderSampledImageArrayNonUniformIndexing` の指定が無い)——
  **これが本 WP で足す唯一の device feature**

#### 1. capability と device feature

- `runtimeDescriptorArray` + `shaderSampledImageArrayNonUniformIndexing` +
  `descriptorBindingPartiallyBound` を device feature policy に追加
- 新 capability **`pelican.vulkan.ray_material_bindless@1`**。
  これを要求する feature × 非対応 device は **hard reject**
  (property 名・要求値・実値・device 名。RT の既定方針どおり)
- **layout がこの capability を要求しない project は一切影響を受けない**
  (bytes 不変)

#### 2. engine record prefix(`data(q)` への engine 値の席)

RT1b の record は **authored 定数のみ**だった。material/geometry index は
**engine が row ごとに供給する**ので席を作る。layout の opt-in:

```json
{ "schema": "pelican.ray_group_layout", "version": 1,
  "ray_types": ["primary"],
  "default_group": "opaque",
  "groups": [{ "name": "opaque" }],
  "engine_record": ["material_index", "geometry_index"],
  "record": [{ "name": "tint", "type": "vec4" }] }
```

- `engine_record` は省略可。**省略時は RT1b と byte 完全一致**
- 値は固定順・固定型 `u32`。**authored member より前**に置く
  (`data_start` の直後から engine prefix、その後 authored)
- 許可語は `material_index` / `geometry_index` のみ(他は
  `ray_engine_record_unknown@1`)。重複は名前付き拒否
- 正準化 JSON の offset/size 表に engine prefix も出す(mirror test 対象)

#### 3. RT material table(bindless)

- material parameter block(base color factor・alpha cutoff・alpha mode 等)を
  **`material_index` で引ける GPU buffer** に平坦化
- texture は **descriptor indexing の sampled image 配列**。material block は
  配列 index を持つ
- 表は **row snapshot と同じ共同 transaction** で構築・公開(RT1b の二段 commit に
  相乗り。失敗時は旧世代維持)
- material identity → `material_index` の解決は **AS snapshot の 2 branch**
  (`material_document_reference` / `(asset_identity, source_material_index)`)を使う
  —— WP366a で確定済みの正本

#### 4. geometry access(buffer device address)

- `geometry_allocation_id` → `{vertex_address, index_address, vertex_stride,
  index_type, vertex_offset, index_offset}` の表を BDA で公開
- **生成 GLSL accessor** を提供(生成 include に追加):
  `pelicanRayFetchUV(geometry_index, primitive_id, barycentric)` /
  `pelicanRayFetchNormal(...)`。手書きの address 演算を利用者に強いない
- accessor が読む頂点属性の存在は **load 時に検証**(UV を持たない geometry を
  alpha profile に束縛したら `ray_geometry_attribute_missing@1`)

#### 5. portable alpha profile(両車線。design §6・§11-6)

- **engine が生成する述語**を両車線へ注入する:
  pipeline 車線は生成 `.rahit`、ray query 車線は候補ループ内の同一ロジック
- 述語は `alpha_mode == MASK` のとき
  `sampleBaseColorAlpha(material_index, uv) < alpha_cutoff` で ignore
- **byte 一致契約はこの profile の範囲に限定**して維持(§11-6 の裁定どおり)。
  **自由 `.rahit` は pipeline 車線専用**で一致契約の外
- material の alpha 設定は既存の material 文書から読む(新しい authoring 面を作らない)

#### 6. WP354 drill-through の投影

material が供給する RT hit shader の出所(`declared_ref` / origin / `source_open_ref`)を
frame plan の hit slot 投影に載せる(WP354 の様式をそのまま使う)。

#### 7. 受け入れ(§4 規約 10)

- **オラクルは構成的に**(RT2a の教訓): alpha texture は **texel 中心が
  barycentric から厳密に決まる市松 fixture**を生成器が作り、CPU 参照は
  同じ生成器の texel 値を読む。**texture フィルタリングを比較対象にしない**
  (`filter: nearest`・`address: clamp_to_edge` 固定、UV は texel 中心に構成)
- **両車線の一致**: 同一 scene・同一 material で pipeline 車線と ray query 車線の
  出力が **pixel 一致**(profile の範囲で)。**片方の profile 注入を外すと落ちる**
- **engine record**: `material_index` / `geometry_index` の実値が shader で読め、
  **表を 1 行ずらすと落ちる** / `engine_record` 省略時は RT1b と byte 一致
- **geometry access**: accessor が返す UV が CPU 参照と一致 /
  UV 無し geometry の束縛が `ray_geometry_attribute_missing@1`
- **capability**: 非対応 device で hard reject(property 名つき)/
  対応 device で feature が有効化されること
- **共同 transaction**: material 表と geometry 表の構築失敗を 5 failure point に
  追加し、旧世代が完全に残ること
- 出荷 4 project・core resources・既存 RT feature の bytes 不変 /
  RT1b・RT2a の既存挙動不変 / 全数 2 回 / matrix は WP367a の
  必須 capability lane の形(`--no-tests=error` + skip 失敗扱い)

#### 8. やらないこと

per-instance 束縛(永続 identity の設計段)/ `.rint`(RT3)/ `.rcall`(RT4)/
変形 geometry(RT5)/ 複数 geometry BLAS・indirect・AS graph node(RT6)/
編集 RPC(RT7)/ **alpha 以外の material 効果**(normal map・emissive 等の
hit shader からの利用は本 WP の範囲外。表と accessor は使えるようになるが、
engine 生成 profile は alpha のみ)。

依存: WP366 + WP366a + WP367 + WP367a。

#### WP368 仕様レビュー(v1、不合格・12 件)と分割

**全件「直す」。v2 は v1 との差分で、矛盾したら v2 が優先。**
最大の判明: **v1 は「access(表と accessor)」と「profile(engine が書く alpha 述語)」を
1 WP に混ぜていた**。前者は pipeline 車線だけで完結し単独で挙動を変えるが、後者は
alpha の正規化 IR・ray query 車線の候補ループ・authored anyhit との排他まで要る。
**WP368(access + reload hole 閉鎖)と WP369(portable alpha profile)に分割**する。
一体で仕様レビューする。

さらに #8 が示したとおり、**RT1b の共同 transaction に「相乗り」する前提は、
WP367 で台帳送りにした既知の穴(`.rchit`/`.rahit` の reload が pipeline を先に
commit し SBT は次 render)と矛盾する**。表を generation payload に載せる以上、
**この穴を閉じるのが WP368 の前提**になる(台帳の負債をここで返す)。

| # | v1 の壊れた点 | 決定(行き先) |
|---|---|---|
| 1 | 1 文書が複数 material を定義でき、AS snapshot の document ref では実体を特定できない / 引用も誤り(確定 branch は `material_asset_reference` + index であって `asset_identity` ではない) | **WP368**: row snapshot に **解決済み material handle + 世代/lease** を持たせ、selector→handle の一致を transaction 構築時に検証。asset branch は `material_asset_reference` を使う(引用訂正) |
| 2 | 指定した述語が engine の実 alpha 判定と違う(raster は texture×vertex color×factor を比較 / project OpenPBR は `geometry_opacity` 系 / per-instance override が判定前に効く) | **WP369**: lowering から **正規化 alpha IR**`{mode, source_kind, factor, texture, uv_transform, cutoff, sampler/lod}` を保持。**v1 は glTF-core に限定**し、alpha に効く per-instance override と別 opacity 源は**load 時に名前付き拒否**。受け入れに raster または独立 CPU evaluator との decision 比較 |
| 3 | `engine_record` が RT1b の record ABI(overlay の exact-set / canonical JSON / SPIR-V mirror / SBT pack の 4 用途)に一意に統合できない | **WP368**: **`authored_record` と `physical_record = engine_prefix ∥ authored_record` を分離**。engine 名は**予約 namespace**(authored と同名は `ray_record_name_reserved@1`)。overlay は authored のみ encode / validator は physical を検査 / packer は engine bytes と authored bytes を別々に注入。**canonical order は固定 `[material_index, geometry_index]`**(配列の宣言順は使わない)。省略時は **canonical field・include text・descriptor・cache salt を一切生成しない** |
| 4 | ray query 車線に shader record が無く `material_index`/`geometry_index` を得る経路が無い / `instanceCustomIndex` の流用は規範違反 / 生成 include は RT compute task にしか注入されない | **WP369**: **engine-private な `TLAS instance ID → {material_index, geometry_index}` 表**を同一 generation に追加(user custom index は独立に保持)。候補 loop・`rayQueryConfirmIntersectionEXT`・flags/opacity を逐語規定。**typed profile sidecar を fullscreen の compile/bind 経路にも運ぶ**。production の `rt_shadow_mask` を通す否定対照 |
| 5 | 生成 `.rahit` と自由 `.rahit` は同一 hit group に共存できない(Vulkan の any-hit slot は 1 つ)/ profile の activation 文法が無い | **WP369**: **`portable_profile: "material_alpha_v1"` の明示 opt-in**(engine field 2 つと capability を原子的に要求)。**v1 は authored anyhit と同居したら名前付き拒否** |
| 6 | descriptor indexing は feature bit だけでは ABI にならない(set/binding・sampler・容量・binding flags・`nonuniformEXT`・bind 点が未定。既存 set 2 は material ごとで scene-global bindless ではない) | **WP368**: exact set/binding・**combined image sampler**・固定容量 + device limit 検査と named reject・layout creation の binding flags(pipeline layout cache key にも含める)・**`nonuniformEXT` 必須**・両車線の bind 点を規範化。**generation ごとに新 descriptor set**(update-after-bind を要求しない immutable publication) |
| 7 | `ray_geometry_attribute_missing@1` が構成不能(欠落 UV は upload 時に `(0,0)` 埋め、presence mask がどこにも残らない)/ 表の address と offset が二重加算しうる | **WP368**: **import 時の attribute presence を primitive→draw→AS snapshot まで保持**。AS の triangles input と geometry 表を**単一の immutable geometry snapshot** から生成。**subrange address 方式に統一**(表の offset は 0。二重加算を構造的に不可能に)。attribute の offset/format は既存 `CommonVertStruct` の固定 ABI を**規範として引用**。非ゼロ suballocation / UV 欠落 / 全ゼロ UV を別 fixture |
| 8 | 共同 transaction への「相乗り」が既知の reload hole と矛盾 | **WP368 の前提として穴を閉じる**。generation payload を `{layout/include, shaders, pipeline layouts/objects, 全 SBT, rows, row-index 表, material buffer, texture descriptor set + image lease, geometry 表, TLAS}` と列挙。shader・material value・texture・model・geometry pool の全 invalidation を規定。各 failure point の後、**実際に次フレームを旧世代で描画**して確認 |
| 9 | table index の決定性が未規定(handle 再利用・`unordered_map` 順・allocation 順)/「表を 1 行ずらすと落ちる」は表を読んだことしか示さない | **WP368**: canonical sort key を固定(material = `material_ref` の正準文字列順 / geometry = `geometry_allocation_id` 順)+ dedup 規則。**row snapshot digest に各 table の digest/generation を含める**。登録順反転・退役/再利用を同一 fixture に。GPU readback に `{material_index, geometry_index, texture_index, sampled_alpha, cutoff, decision}` を出し**各入力を 1 つずつ壊す否定対照** |
| 10 | 市松 fixture の「texel 中心が厳密に決まる」は受け入れ条件にできない(barycentric の bit 一致は非保証)/ mip/LOD 未指定で両車線が別 mip を読み得る | **WP369**: exact equality を捨て、**generator が「全 texel 境界から固定 margin 以上」を証明**する。両車線を **`textureLod(..., 0)` または単一 mip view に固定**し、CPU も同じ mip を参照 |
| 11 | WP354 投影の供給者が存在しない(shader は task の物理 slot が所有し、material に一意な `declared_ref` は無い)/ 受け入れに observable が無い | **WP368**(closesthit 分)+ **WP369**(生成 anyhit 分): **task × 物理 hit slot ごと**に投影。**生成 alpha anyhit は `origin:"generated"`・authored ref なし・open 不可(理由つき)**。PlanViewer/Studio の strict parse と open action の否定対照 |
| 12 | variant・purge の受け入れが具体化されていない | 両 WP: flat/xr/preview・compiler ON/OFF(**OFF は `layout_requires_runtime_compiler@1` + shader document load 0 件**)・OpenXR/ImGui/Studio を**具体的 test ID と observable で列挙**。**profile/layout 不在時は capability 要求・table buffer・descriptor pool/set・生成 shader・render target が全て 0 件**であることを観測 |

### WP368 v2: RT2b-1 —— material/geometry 表と access(pipeline 車線)

範囲 = 上表の #1・#3・#6・#7・#8・#9・#11(closesthit 分)・#12 +
v1 の §1(capability と device feature)・§3(material 表)・§4(geometry access)。
**単独価値**: hit shader が自分の material のパラメータを読み、UV を取得できる
(alpha 判定は engine が書かない —— 利用者が自分の `.rahit` で書ける)。
**やらないこと**: engine 生成の alpha 述語・ray query 車線・両車線一致契約(全て WP369)。

### WP369: RT2b-2 —— portable alpha profile(両車線)

範囲 = 上表の #2・#4・#5・#10・#11(生成 anyhit 分)・#12 + v1 の §5。
**単独価値**: `portable_profile: "material_alpha_v1"` を書くだけで、両車線で
実 texture の alpha test が効く。
依存: WP368。

依存: WP366 + WP366a + WP367 + WP367a。

#### WP368/369 第 2 巡(9 件)と v3 —— 共同 transaction 要求の撤回と数値の確定

**v3 が最終。WP368 の仕様レビューはここで打ち切る。v3 > v2 > v1。**
WP369 は WP368 完了後に 1 巡だけ仕様レビューを行う(本節の決定を起点に)。

##### 裁定 1: 「新しい表も共同 transaction に入れて失敗時は旧世代維持」を**撤回**(#3)

第 2 巡が示した二つの反例は、いずれも **RT の外**にある:
同形 texture reload は **live image へ直接 upload** し(旧 texel は失敗前に既に消える)、
capacity 境界の model reload は **共有 vertex/index pool を再確保**する
(旧 buffer は DeletionQueue 送り)。これを本当に満たすには
「全 RT-visible 更新を generation-private resource へ stage し、成功 submission 後に
一度だけ公開する」——**resource lifetime のサブシステム**が要る。
WP363(lifecycle)と同じ「機能 WP の中に前提サブシステムが隠れている」形である。

**決定**: WP368 は**この保証を主張しない**。
- material 表と geometry 表は **SBT と同じ場所・同じ immutable row snapshot から構築**
  する(renderer.cpp:4718 の地点)。**行との整合は構成的に保証**され、
  失敗時の挙動は現行 SBT 失敗と同じ
- **reload hole の閉鎖は WP368 の範囲外**(台帳の既存負債のまま)。
  「RT-visible 更新の generation-private staging」は**別設計**として台帳に残す
  (texture reload / model reload / pool lease を跨ぐため、設計から始める)

##### 裁定 2: 数値と鍵の確定(#1・#2・#4・#5)

- **`physical_record` の packing**(#1): engine prefix を canonical 順
  `[material_index, geometry_index]`(いずれも `u32`)で先頭に置き、
  **その後 authored member を、engine prefix を含む単一 struct として std430 で配置**する
  (例: `engine=[material_index]`, authored=`[vec4 tint]` →
  `material_index@0`, padding 4..15, `tint@16`, `data_size=32`)。
  **physical offset が唯一の正本**。overlay は authored を**名前で** encode し、
  packer は physical offset へ置く。mirror JSON は physical を出す。
  engine field 0/1/2 個の全ケースの offset/size/stride を表として本文に持つ
- **bindless ABI**(#2): **set 3(free 枠)** を使う。
  `binding 0` = material row buffer(storage buffer)/ `binding 1` = **combined image
  sampler の配列**(容量 **1024** 固定)/ `binding 2` = geometry table(storage buffer)。
  **専用 descriptor pool を RT 表が所有**(material container の pool を使わない)。
  `maxBoundDescriptorSets >= 4` と `maxDescriptorSetSampledImages >= 1024` を
  device limit として検査し、不足は named reject。index は **`nonuniformEXT` 必須**。
  binding flags(`PartiallyBound`)を layout creation に付け、
  **pipeline layout の cache key にも含める**。
  material row の ABI(field 順・型・offset・missing texture sentinel = `0xFFFFFFFF`・
  texture の正準 identity・sRGB view・default sampler)を C++/GLSL 両方で表として固定
- **geometry 表の順序**(#4): **`geometry_allocation_id` 昇順**。
  **「登録順を反転しても同じ bytes」という受け入れは撤回**する ——
  allocation ID は採番順に依存し、両立不能だった(row の q と違い、
  geometry index は **engine-private** で shader に公開されないため順序の安定性は不要)。
  受け入れは「**同一入力から同一 bytes**」+ 「row snapshot digest に表の digest を含む」
- **material の canonical key**(#5): **`document:<ref>#<material 名>`**(project material)
  / **`asset:<material_asset_reference>#<source_material_index>`**(埋め込み)。
  これが表の sort key かつ dedup 等価判定。
  **RT1b の `ray_bindings` selector の意味は変えない**(`document` 指定は
  その文書の全 material を選ぶ)—— 表の鍵だけが細かい

##### 裁定 3: 分割境界の訂正(#6・#7)

- **missing-attribute の拒否は WP368 から外す**(#6): 発火条件を宣言するのは
  profile だけなので、`ray_geometry_attribute_missing@1` は **WP369** へ。
  WP368 は attribute presence を **snapshot まで運ぶだけ**(拒否しない)
- **bind 点の割り当てを明示**(#7): **pipeline 車線の bind は WP368、
  fullscreen/ray query 車線の bind は WP369**。WP368 は `rt_shadow_mask.frag`
  経路を触らない

##### 裁定 4: WP369 の前提(#8・#9。実装は WP368 の後)

- **alpha IR の `source_kind` を列挙し式を固定**(#8): v1 は
  `gltf_base_color` のみ。式は **`texture.a × factor.a × COLOR_0.a`**
  (raster の実装と一致)。**COLOR_0 の accessor を追加**し、無い場合の default は `1.0`。
  それ以外の opacity 源(project OpenPBR の `geometry_opacity` 系)と
  alpha に効く per-instance override は **load 時に名前付き拒否**
- **production の opaque 強制を解く**(#9): profile が active な geometry/instance に
  ついて、**両車線の ray flags から `Opaque` を外し**、query 車線は
  候補ループ + `rayQueryConfirmIntersectionEXT` を持つ。生成 predicate の
  production hook を具体 GLSL 相当で規定。**profile 不在時は現行 bytes 保持**

##### WP368 v3 の範囲(確定)

裁定 1〜3 を適用した v2 の WP368。すなわち:
capability と device feature(裁定 2 の数値)/ engine record の physical packing /
material 表・geometry 表・descriptor 表の構築(SBT と同じ地点・同じ snapshot)/
BDA の geometry access と生成 accessor(UV・normal・**COLOR_0**)/
attribute presence の運搬(拒否はしない)/ material 解決済み handle の row snapshot 保持 /
表の canonical key と digest / WP354 投影(closesthit 分)/ pipeline 車線の bind /
variant・purge の受け入れ(#12 の形。**profile/layout 不在時は capability 要求・
table buffer・descriptor pool/set・生成 shader・render target が全て 0 件**)。

**単独価値**: authored `.rchit` が自分の material row と geometry accessor を読める
(観測は production SBT → AS record 経由。第 2 巡で「壊せなかった項目」として確認済み)。
**やらないこと**: 共同 transaction の保証 / reload hole の閉鎖 / ray query 車線 /
engine 生成 alpha 述語 / missing-attribute の拒否(全て WP369 または別設計)。

##### 台帳のみ(別設計)

**RT-visible 更新の generation-private staging**: texture reload の live upload、
model reload の共有 pool 再確保、vertex/index pool lease を跨ぐため、
機能 WP に混ぜず設計から始める。これが入るまで、RT の表と SBT は
「行と構成的に整合するが、reload 失敗時の旧世代保持は保証しない」。

依存: WP366 + WP366a + WP367 + WP367a。

#### WP368(RT2b-1)完了(2026-08-31)

統合: 全数 **1487 ×2 緑**(GPU 直列込み)、doclink 2341 ok。
基準値 N=1487(RT2a 完了時 1478 + 9)。UNMET 0。

成立したもの: descriptor indexing の capability と device limit 検査
(3 feature 個別 false / limits 3・1023 で property・要求値・実値・device 名つき reject)/
**physical record**(engine prefix `[material_index, geometry_index]` を含む単一
std430 struct。0/1/2 field 全ケースの offset/size/stride を正準 JSON と mirror で固定。
逆順宣言も正準化)/ material 表・geometry 表(同一入力で bytes・digest 一致、
各要素の単独変更が対応表と row digest に出る)/ geometry の subrange address 昇順と
attribute presence の運搬(UV 欠落と全ゼロ UV を区別。**拒否はしない** = WP369)/
**set 3**(binding 0 material row / 1 sampler2D[1024] / 2 geometry table、専用 pool、
PartiallyBound、`nonuniformEXT`、cache key)/ 生成 accessor(UV・normal・COLOR_0)/
material の canonical key `document:<ref>#<名前>`(RT1b selector の意味は不変)/
WP354 の closesthit 投影。

**スコープ規律**: 裁定 1 のとおり **reload hole に手を出していない**
(`generation-private` 系の変更 0)。ray query 車線(`rt_shadow_mask.frag`)も無傷。
出荷 4 project・core resources の diff 0。

最も強い対照: **TLAS→SBT の row を交換すると material・UV・normal・COLOR_0 の
GPU 出力が双方入れ替わり、復元で元 bytes に戻る**(表と行が同一 snapshot 由来である
直接証明)。purge 対照: layout 不在時に capability 要求・buffer・descriptor pool/set・
生成 shader・target が**全て 0 件**。

次: §10 コードレビュー → **WP369(RT2b-2: 両車線の portable alpha profile)**。
WP369 は本 WP 完了後に仕様レビューを 1 巡(第 2 巡の裁定 4 が起点)。

#### WP368 コードレビュー(受理不可・4 件、致命 1)の仕分けと WP368a

範囲外侵入なし(reload・ray query・missing-attribute 拒否・生成 alpha・Studio の
変更 0)。physical record の std430・engine field 0 個の RT1b byte 一致・
subrange の二重加算なし・set 3 の binding/flags/pool 破棄順・RT1b selector の意味不変は
「壊せなかった項目」。**全 4 件「直す」= WP368a を 1 回だけ。以降は無条件で台帳。**

### WP368a: RT2b-1 の修理(最終。以降の指摘は台帳)

**§4 規則 11 上段。コードレビューのみ。見積: 中。**

1. **致命 —— 実 glTF の material canonical key が必ず物理パスと衝突する**:
   `model.cpp` は canonical `declaration.reference` と物理 `declaration.path` を
   明示的に分離しているのに、GltfLoader へ渡すのは物理パスで、それが
   `MaterialInfo.source_asset` に保存される。一方 row key は canonical reference 由来。
   **実プロジェクト(例: `animgraph_demo` の material 0)で RT2b layout を有効化すると
   最初の table build が `ray_material_key_mismatch@1` で停止する**。
   texture identity も絶対パスを含むため machine 間で非決定的。
   **修正**: 物理パスは I/O・診断専用にし、**`declaration.reference` を
   GltfLoader の commit / material registration に別引数で渡す**。
   `MaterialInfo.source_asset` と texture の canonical identity は
   **authored reference から作る**。
   **受け入れ**: **実 project ロード(ModelAssetContainer / GltfLoader の本番経路)から
   RT2b table・GPU まで通す**テスト(現行 fixture は `source_asset` と
   `authored_asset_reference` を手で同じ文字列にしており本番経路を通っていない)
2. **bindless capability の limit 検査が不足**(実害): 検査しているのは
   `maxBoundDescriptorSets` と `maxDescriptorSetSampledImages` の 2 つだけ。
   `maxDescriptorSetSamplers` / `maxPerStageDescriptorSamplers` /
   `maxPerStageDescriptorSampledImages` / **set 0〜3 を合算した descriptor 数** /
   `maxPerStageResources` が未検査。
   具体例: `maxDescriptorSetSamplers=512` の device / sampled-image 上限がちょうど
   1024 で set 2 の 2 sampler を足すと 1026 になる device。
   `PartiallyBound` は宣言 `descriptorCount` を limit 計算から外さない。
   **修正**: 全該当 limit を runtime facts に運び、**実際の set 0〜3 layout を
   stage 別に合算**して Vulkan object 作成前に property/required/actual/device つきで
   拒否。既存 `materialcontainer` の sampler limit 検査と**同じ流儀に揃える**
   (第二の流儀を作らない)
3. **asset key の handle 整合が prefix だけ**(fail-fast 欠陥):
   `starts_with("asset:<source>#")` しか見ないため、`source_material_index` が
   ずれた入力を黙って受理する。**修正**: 埋め込み material 登録時に
   source material index も保存し、provenance のある material は
   **canonical key と完全一致**を要求
4. **受け入れテストに誤実装でも通る経路が 4 つ**(§4 規約 10 違反):
   (a) texture descriptor mapping —— 2 identity が同じ `whiteTexture()` を指し、
   「全 descriptor に先頭 texture を書く」実装でも通る →
   **異なる色の 1×1 texture** を使い、index→実 image の対応を出力差で示す
   (b) attribute presence —— GPU fixture は両 geometry とも全属性あり、欠落対照は
   snapshot field の手書き。producer を「常に全 bit」にしても通る →
   **実 importer/upload 経路**で「欠落」と「全ゼロだが存在」を比較
   (c) 1 文書複数 material —— `material_document`/`material_name` を直接構築しており
   production の `material_name` 運搬を空にしても通る →
   **実 project material 文書**を production 経路へ
   (d) purge の「生成 shader 0」—— active/absent の両方で
   `compute_task_definitions.empty()` になっており観測になっていない →
   **ShaderLibrary の registration checkpoint/count** が baseline に戻ることを観測
   + WP368 固有 matrix(OpenXR/ImGui/SKIP_DEVSTUDIO・compiler OFF の実 named reject)を
   `SUCCEED` でなく実行

**受け入れ全体**: 各修正に「取り消したら落ちる」対照(代理条件禁止)/
出荷 4 project・core resources の bytes 不変 / **実 project で RT2b layout を
有効化した経路が通ること**(#1 の再発防止)/ 全数 2 回 / GPU 直列は統合側。

依存: WP368。**連鎖打ち切り: WP368a の §10 指摘は無条件で台帳。**

#### WP368a 完了 —— RT2b-1 確定(2026-08-31)

統合: 全数 **1487 ×2 緑**(GPU 直列込み)、doclink 2341 ok。4 件修理、UNMET 0。

要点: **authored reference が GltfLoader の commit まで届き**、material と texture の
identity をそこから作る(物理パスは I/O・診断専用)。修正を戻すと
`asset:models/tree.glb#0` と物理絶対パスが衝突し table 作成前に落ちる
—— **本番経路(ModelAssetContainer → GltfLoader → RT2b table → GPU)**を通した対照 /
bindless limit は全 runtime facts + **set 0〜3 の実 layout を stage 別に合算**して
Vulkan object 作成前に検証(実 layout 1026 に対し fact を 1025 に変異させ、
旧 1024 検査に戻すと落ちなくなる対照。layout 作成件数の不変も見るので検査位置の
後退も検出)/ asset key は index まで**完全一致** / 代理条件 4 件を実物に置換
(赤・緑の 1×1 texture / 実 importer の欠落 vs 存在する全ゼロ / 実 project material
文書の `ProjectOak`・`ProjectPine` / ShaderLibrary registration が purge で baseline 復帰)/
matrix 5 構成は `SUCCEED` でなく実行(compiler OFF は production loader の named reject)。

##### 統合時の障害(コードではない)

マージ後の初回ビルドが `accelerationstructure.obj : LNK1163`(COMDAT セクション選択が
無効)で失敗。**worktree で作ったオブジェクトとマージ後ソースが混在した
incremental link の破損**であり、stale プロセスは無し。該当 obj を削除して再ビルドで
復旧(exit 0)。既知の LNK1104/LNK1168(exe ロック)とは別症状・別対処なので
skill の運用節に追記した。

次: **WP369(RT2b-2: 両車線の portable alpha profile)**。
仕様レビューを 1 巡(第 2 巡の裁定 4 —— alpha IR の `source_kind` を
`gltf_base_color` のみとし式は `texture.a × factor.a × COLOR_0.a`、
production の opaque 強制を profile active 時に解く —— が起点)。

#### WP369 仕様レビュー(v1、不合格・12 件)と v2 —— 両車線契約の退役

**v2 が最終。仕様レビューはここで打ち切る。v2 > v1。**

##### 裁定 1: **ray query 車線を範囲から外し、両車線の byte 一致契約を退役させる**

12 件のうち #3(engine が authored raygen の `Opaque` を外せない)・
#4(生成 include が fullscreen compile に届かない)・#5(query-only 構成では
table を所有する RT pipeline が存在しない)は、いずれも
**「一つの述語を構造の違う二車線へ通す」**ことに由来する。閉じるには
profile sidecar の fullscreen 輸送・graphics pipeline への set 3・
SBT からの table 所有権分離 —— **engine のサブシステム**が要る。

**問い直す: 両車線の byte 一致は誰のためか。** 利用者の要求は
「DCC 級に好き勝手できる RT」であって、engine 内部の二車線一致ではない。
そして **RT2b-1 が入った今、利用者は既に自分の `.rahit` で material texture を
sample して `ignoreIntersectionEXT()` を書ける**。WP369 が足すのは
**利便(自分で書かなくてよい)**だけである。

**決定**:
- **WP369 は pipeline 車線のみ**。ray query 車線は現行のまま(opaque・first-hit)
- **design v4 §6 の byte 一致契約を改訂**: 「両車線は **どちらも alpha を使わない
  範囲でのみ**一致する」。profile が active な scene では一致を主張しない
  (この改訂を design_raytracing.md に記載する)
- ray query 側の alpha 対応が本当に要ると分かったら、**そのときに設計から**
  始める(profile sidecar の輸送・set 3 の共有・table 所有権が論点。台帳に残す)

##### 裁定 2: 残る 9 件の決定

| # | 指摘 | v2 |
|---|---|---|
| 1 | `portable_profile` の置き場所と適用単位が未定義 / 出荷 feature は旧文法で layout と併用できない | **layout 文書の top-level field** に置く(`"portable_profile": "material_alpha_v1"`)。適用は **traced な `(g,r)` のうち authored anyhit を持たない slot 全部**。**authored anyhit を持つ slot が一つでもあれば名前付き拒否**(v1)。「profile を書くだけで既存 feature が有効化される」という主張は**撤回** —— **typed task が必要**であることを明記し、完全な JSON 例を載せる |
| 2 | 生成 anyhit の payload ABI が validator と両立しない(closesthit に payload、生成側に無い → reject) | **validator に明示的な例外**: engine-generated anyhit は payload 宣言を省略できる(pair 検査は closesthit 側のみで成立)。RT2a の subset 分類は**存在しない**ので `subset_certified` の投影も不要 |
| 3 | engine が authored raygen の `Opaque` を外せない | **外さない。検査する。** `rtspirvinspection` の trace-call 記録に **ray flags(定数)を追加**し(operand 定数性の既存機構の小拡張)、**profile-active task の raygen が `Opaque` を含む、または flags が定数でないなら `ray_profile_opaque_trace@1` で拒否**(shader path つき)。AS の geometry/instance flags も non-opaque を要求 |
| 4・5 | 生成 include の fullscreen 輸送 / query-only の table 所有 | **裁定 1 により消滅**(範囲外) |
| 6 | per-instance override の「load 時 reject」は成立しない(publish 経路が 3 つ、frame ごとに変わる)/ relative multiplier は discard の**後**なので全部 reject は過剰 | **拒否をやめ、row に解決して載せる**。row は per-instance override 適用**後**の
`{alpha_mode, cutoff, base_factor_a, texture_index, uv_transform, uses_vertex_color}` を持つ。row は snapshot と一緒に作り直されるので**監視も rollback も不要**。raster が discard の後に適用する relative multiplier は**そもそも判定に効かない**ので無視(過剰拒否を作らない) |
| 7 | alpha IR の正準 producer と identity が未定義 | **project lowering に単一の normalizer** を置き、そこだけが `NormalizedAlphaIr` を作る。material と texture の identity は **WP368a の authored reference 由来**(数値 handle・絶対パス禁止)。**同じ authored ref・異なる物理 path で 2 回 load して IR と table digest が一致**する対照 |
| 8 | missing-attribute 条件が閉じていない(texture 無し・UV 無しの MASK が拒否される) | **要求は解決済み row から導く**: texture がある行だけ UV を要求し、`uses_vertex_color` の行だけ COLOR_0 を要求する。それ以外は要求しない。不足は `ray_geometry_attribute_missing@1`(行・material・欠落属性つき) |
| 9〜12 | (第 1 巡表の残り。市松 margin・単一 mip・matrix・WP354 生成 anyhit 投影) | 市松は **exact equality を捨て境界からの margin を生成器が保証**、両車線でなく **pipeline 車線のみ `textureLod(..., 0)`**、CPU も同じ mip。生成 anyhit は WP354 投影で `origin:"generated"`・authored ref なし・open 不可(理由つき)。matrix は WP367a の必須 capability lane の形 |

##### WP369 v2 の範囲(確定)

`portable_profile: "material_alpha_v1"` を layout に書くと、**pipeline 車線**の
traced な各 `(g,r)`(authored anyhit の無い slot)に **engine 生成の `.rahit`** が入り、
式 **`texture.a × factor.a × COLOR_0.a < cutoff` で ignore** する。
入力は WP368-1 の material 表・geometry accessor・engine record。
`alpha_mode == MASK` の行だけが対象(それ以外の行は生成 anyhit を持たない)。

受け入れ: profile 有無で GPU 出力が変わる(**profile を外すと落ちる**)/
CPU 参照(市松 margin・単一 mip)と包含一致 / `Opaque` を含む raygen の named 拒否 /
authored anyhit 同居の named 拒否 / texture 無し・UV 無しの MASK が**通る** /
texture ありで UV 無しは named 拒否 / 同一 authored ref・別物理 path で digest 一致 /
per-instance の absolute alpha 変更が**次フレームの GPU 出力に反映**される /
**profile 不在なら生成 shader・新 wire・cache salt が 0 件**(現行 bytes 保持)/
出荷 4 project bytes 不変 / matrix は必須 capability lane の形 / 全数 2 回。

やらないこと: **ray query 車線の alpha(別設計)**/ MASK 以外の alpha mode /
alpha 以外の material 効果 / RT3 以降。

依存: WP366/366a + WP367/367a + WP368/368a。

#### WP369(RT2b-2)完了 —— RT2b 完結(2026-09-01)

統合: 全数 **1494 ×2 緑**(GPU 直列込み)、doclink 2341 ok。
基準値 N=1494(RT2b-1 完了時 1487 + 7)。UNMET 0。

`portable_profile: "material_alpha_v1"` を layout に書くと、**pipeline 車線**の
traced な各 `(g,r)`(authored anyhit の無い slot)に engine 生成 `.rahit` が入り、
`texture.a × factor.a × COLOR_0.a < cutoff` で ignore する。

裁定が実装で守られた点(回収時に独立確認):
- **ray query 車線 diff 0**(裁定 1: 範囲外)
- **`Opaque` を外さず検査**(`ray_profile_opaque_trace@1` / `ray_profile_opaque_as@1`)
  —— engine が所有しない shader を書き換えない
- **per-instance override は監視せず row に解決**(`base_factor_a` /
  `uses_vertex_color`)—— relative multiplier では row も GPU bytes も不変、
  absolute alpha は次フレームで row 1→0・GPU hit→miss

対照: authored anyhit が 1 slot でもあれば named 拒否 /
**`engine_generated=false` に戻すと payload mismatch で落ちる**(生成 anyhit の
validator 例外が効いている証明)/ 市松は margin 方式で境界 margin=0 の変異が落ちる /
**profile を外すと全 intersection が再び hit** / profile 不在時は生成 shader 0・
従来 include の SHA 固定・validator digest が ray flags 変更でも不変。

##### 設計の改訂(design_raytracing.md §6-1)

**両車線の byte 一致契約を退役**。二車線は「どちらも alpha を使わない範囲でのみ」
一致する。理由: 一つの述語を構造の違う二車線へ通すには profile sidecar の
fullscreen 輸送・graphics pipeline への set 3・SBT からの table 所有権分離という
**engine のサブシステム**が要り、機能 WP に収まらないことが実証された
(WP369 v1 レビュー #3・#4・#5)。RT2b-1 の表と accessor があるので
**利用者は自分の `.rahit` で ray query 相当を書ける**——失われたのは
engine 側の自動一致だけ。ray query の alpha が必要と分かった時点で設計から。

次: WP369 の §10 コードレビュー → **RT3(AS の宣言面: geometry_flags /
instance_flags の分離・mask・custom_index 未指定=0・static AABB + `.rint`)**。

#### WP369 コードレビュー(不合格・7 件)の仕分けと WP369a

致命なし。範囲外侵入なし。**#2・#3・#4 は同じ根 —— RT と raster が alpha の意味で
食い違う**(仕様レビューで「両車線が同じ間違った式を使えば通る」と指摘された危険が、
今度は RT vs raster の形で出た)。**全 7 件「直す」= WP369a を 1 回だけ。
以降は無条件で台帳。**

**確認した事実**: `rg` で調べたところ **raster は `KHR_texture_transform` を
一切読んでいない**。したがって RT が適用するのは**新規の乖離**であり、
「両方が読む」に揃えるのは raster 側の機能追加(別件)。

### WP369a: RT2b-2 の修理(最終。以降の指摘は台帳)

**§4 規則 11 上段。コードレビューのみ。見積: 中。**

1. **非 MASK material にまで `NormalizedAlphaIr` を強制**(実害): profile active の
   table build が `alpha_mode` を見る**前**に全 source へ IR を要求するため、
   project `.material` 由来の正当な OPAQUE row が
   `ray_profile_alpha_ir_missing@1` で落ちる(project material の producer は
   IR を設定しない)。**修正**: IR 要求・identity 検査・alpha texture 収集を
   **「解決後に MASK になった row」に限定**。非 MASK row は通常の `alpha_mode` から
   構築し、normalized field の既定値に依存しない
2. **relative UV override が RT row から脱落**(実害): raster は absolute が無ければ
   **relative UV** を適用するのに、RT snapshot producer は
   `material_absolute_override_frames` しか見ていない。
   **修正**: row producer を raster と同じ優先順位に —— absolute UV があればそれ、
   無ければ relative UV。**relative base-color multiplier だけは discard の後**なので
   引き続き無視(v2 の裁定どおり)
3. **authored KHR texture transform が RT にだけ効く**(実害): loader が IR に
   取り込み生成 anyhit が適用するが、**raster は読んでいない**。
   **修正(v1)**: **profile 使用時の非 identity KHR transform を名前付き拒否**
   (`ray_profile_uv_transform_unsupported@1`)。併せて **extension field の
   absent と present-invalid を区別**し、`"offset":[0.5]` のような不正を
   黙って identity に落とさず名前付きエラーにする。
   **台帳**: raster の KHR_texture_transform 対応は別件(engine 全体の機能追加)
4. **raster の saturate と生成 anyhit の生比較が不一致**(実害): raster は
   opacity と cutoff の**両方を `[0,1]` に clamp** するが、生成 anyhit は生の積と
   cutoff を比較する。`alphaCutoff = 2.0` や absolute alpha `-0.1` で判定が逆転。
   **修正**: **raster の正準 predicate と完全に同じ clamp** を生成 shader に入れる
   (cutoff だけ・factor だけの部分修正は不可)
5. **authored reference を path の字面から推測**(実害): 「絶対でも `file://` でもない」
   だけで authored reference と判定するため、legacy API が物理 path を渡す経路で
   profile 不在でも normalizer に入り `normalized_alpha_identity_invalid@1` になる。
   逆に `"models/tree.glb"` のような相対物理 path を authored と誤認し、
   cwd 依存の identity を作る(WP368a の provenance 契約に反する)。
   **修正**: **字面推測を廃止**。WP368a の論理 reference が**型付きで渡された経路**
   だけが IR を生成し、legacy direct/inspection 経路は `authored_reference = nullopt`。
   profile がその material を選んだ時点で既存の missing-IR named エラーにする
6. **テストが代理条件**(懸念): GPU fixture の material が**全部 MASK**、
   geometry に **COLOR_0 が一つも無い**、生成 shader 検査が**ソース文字列**、
   COLOR missing は helper 直叩きの手書き row(production producer は同じ presence bit
   から `uses_vertex_color` を作るので不整合 row は本番から出ない)、
   **AS の production flags は三箇所とも常に空**で helper に synthetic flags を渡すだけ。
   → non-MASK guard・COLOR_0 の fetch/multiply・AS validator 呼び出し 3 箇所を削っても
   通る。**修正**: 同一 GPU テストに **MASK と OPAQUE/BLEND を併置**して解決済み row と
   hit/miss を比較 / **実 geometry の COLOR_0** で low/high alpha / AS builder に
   test seam で flags を注入し **production `recordFrame` から named 拒否まで**通す
7. **matrix が compiler OFF を実行していない**(懸念): fixture が
   `runtime_shader_compiler_enabled = true` 固定で、CI の OFF job も
   feature smoke も RT2b target を対象にしていない。
   **修正**: RT2b target を **compiler ON/OFF・SKIP_DEVSTUDIO ON/OFF・OpenXR ON/OFF**
   で build/run。fixture は**実 macro 値**を渡し、OFF では
   `layout_requires_runtime_compiler@1` と**生成 registration 0 件**を実検査

**受け入れ全体**: 各修正に「取り消したら落ちる」対照(代理条件禁止)/
**RT と raster の alpha 判定一致**を同一 fixture で(clamp・override 優先順位)/
出荷 4 project・core resources の bytes 不変 / profile 不在時 0 artifacts /
全数 2 回 / GPU 直列は統合側。

依存: WP369。**連鎖打ち切り: WP369a の §10 指摘は無条件で台帳。**

#### WP369a 完了 —— RT2b 確定(2026-09-01)

統合: 全数 **1494 ×2 緑**(GPU 直列込み)、doclink 2341 ok。7 件修理、UNMET 0。

**RT と raster が同じ述語を指すようになった**: clamp を生成 shader だけでなく
**raster 側の ABI lowering(materialformat.cpp)にも通して意味を統一**
(`clamp(alpha) >= clamp(cutoff)`)/ override は absolute UV → relative UV の
raster と同じ優先順位(relative base-color multiplier は discard 後なので不参照)/
**`KHR_texture_transform` は raster が未実装なので非 identity を
`ray_profile_uv_transform_unsupported@1` で拒否**(present-invalid と absent も区別。
raster 対応は台帳)/ 非 MASK row は通常 field から構築(IR 強制をやめた)/
authored reference は**型付きで渡された経路だけ**が IR を生成(字面推測を廃止)。

代理条件の除去: 同一 GPU fixture に **MASK/OPAQUE/BLEND を併置**、**実 COLOR_0** の
low/high、AS flags を **production `recordFrame` の 3 箇所に seam 注入**して
named 拒否まで、生成 shader は文字列でなく**コンパイル済み SPIR-V** を検証。
matrix は compiler OFF・SKIP_DEVSTUDIO・OpenXR で RT2b target を実行し、
**呼び出しを外すと `rt2b_matrix_execution_missing@1` になる ledger** つき。

エージェントの自己申告: feature-smoke をそのまま回した際に既存 WP367 の GPU lane まで
起動したため、WP369 限定 matrix モードを追加して再実行(GPU 直列規約の自己修復)。

##### 台帳のみ

- **raster の `KHR_texture_transform` 未対応**: engine 全体の機能追加。
  対応するまで profile 使用時の非 identity transform は named 拒否のまま

**RT2b はここで確定**(WP368/368a + WP369/369a)。連鎖打ち切り済み。
次: **RT3(AS の宣言面: geometry_flags / instance_flags の分離・mask・
custom_index 未指定=0・static AABB + `.rint`)**。

#### WP369a コードレビュー(7 件)—— 宣言どおり台帳のみ

致命なし。**連鎖打ち切り済みのため WP は起票しない**。以下は次に RT を触る WP
(RT3 以降)または独立の修理を起こすときの仕様の種。
「壊せなかった項目」: clamp の実挙動(raster shader・生成 anyhit とも saturate、
GPU fixture が `alphaCutoff=2.0` と absolute alpha `-0.1` で RT と gbuffer を比較)/
override 優先順位(absolute UV → relative UV、relative multiplier は discard 後)。

| # | 指摘 | 重み |
|---|---|---|
| **1** | **legacy direct load(`loadGltfBinary()`/`loadGltf()`)の runtime material identity が消失**。#5 の「path 字面推測の廃止」の副作用で `commit()` に reference を渡さなくなり、`MaterialInfo::source_asset` と `ModelTemplate::authored_asset_reference` が空になる。全 direct model の material key が `asset:#0` になり、**2 つ以上で `ray_material_key_collision@1`**。旧 path を指す既存 `ray_bindings` selector も不一致になる。テストは production key を使わず `"asset:legacy-relative.glb#0"` を手で注入していて退行を隠している。**直し方: authored provenance と runtime identity を分離**し、legacy direct load には衝突しない deterministic な runtime identity を別 field で持たせる(物理 path を IR provenance に戻さない) | **実害あり。出荷 4 project は RT layout 未使用のため到達しないが、public API の後退。RT を触る次の WP で最優先** |
| 2 | KHR non-identity を **row の override 解決前**に拒否しており、absolute/relative UV override で置換できる正当な入力まで弾く。さらに **「raster は KHR を一切読まない」という私の前提は VRM 経路では不成立** —— loader が KHR 値を `SourceMaterialInitialValues` に格納し、VRM texture-transform expression が absolute UV override として publish、raster shader はそれを適用する。**直し方: present-invalid は常に拒否 / valid non-identity は各 MASK row で resolved override の有無を見て判定(あれば採用、無ければ拒否)/ 設計文書の「一切読まない」も訂正** | 実害あり。**設計文書の記述訂正を含む** |
| 3 | valid な non-identity KHR が **float 丸めで identity に落ちる**(`scale:[1.00000001, 1.0]`)。KHR schema に精度制約は無い。視覚差は float 未満だが fail-fast 契約違反。**直し方: identity 判定を JSON の double 値で行い、その後 float 表現可能性を検査** | 実害あり(軽) |
| 4 | raster lowering の clamp 追加テストが**取り消しても落ちない**(shader 側が既に saturate しており、production lowering test は範囲内 `0.625` しか見ない)。**直し方: `alpha_cutoff=2.0 / -0.1` を `lowerMaterial()` に通し `lowered.values` の実値 `1.0 / 0.0` を検査** | 懸念(§4 規約 10 の禁止形) |
| 5 | 非 MASK が依然 normalized IR digest に依存(source 同値判定と row digest が無条件で normalized を含む)。OPAQUE で absent KHR と明示 identity KHR を切り替えると digest が変わり、重複 source で false collision もあり得る。**直し方: `same_material_source` と row digest の normalized 部分も profile_mask で囲む** | 懸念 |
| 6 | compiler OFF の「registration 0」が**切断された代理条件**(ローカル `ShaderLibrary` を作って数えているだけで production 経路に繋がっていない)。実装自体は loader で先に拒否しており正しい。**直し方: production の config-registration を実 checkpoint / spy つきで呼ぶ** | 懸念 |
| 7 | matrix ledger が実行を独立に証明していない(同じ wrapper が無条件で自分に property を立てるため、`run_process()` だけ消しても通る)。**直し方: 必須 label の中央一覧 + executable 自身の成功 marker + 末尾の差分監査** | 懸念 |

**推奨**: #1 は RT を次に触る WP の冒頭で直す(または #1〜#3 をまとめた小さな
独立修理 WP)。#4〜#7 はその WP の受け入れ条件に同乗させる。

### WP370: プレファブ U2 —— unpack・object picker・一括流し込み・dry-run

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 大。**
**規範は docs/design_prefabs.md の U2 行(§7)と、台帳「引き継ぎ: 残 WP への先行裁定」の
U2 裁定。前提は WP361/361a(プレファブ本体)+ WP362(永続化と編集)。**

#### 前提の実測(2026-09-01、起票時に確認)

- `unpack` は**未実装**(`src/core/` に該当なし)
- `set_prefab_parameter` / `unset_prefab_parameter` は WP362 で実装済み
  (journal・inverse・3 重 CAS・12 fault point)
- `kind:"object"` パラメータは WP361 で解決済みだが、**rename 連動は未実装**
  (壊れた参照は load 検証で名前付きに落ちるだけ)

#### 1. unpack(journal 1 op)

`unpack_prefab`: object の `prefab` キーを**展開済み components への置換**で消す。
**object 名は不変**。design v4 の原則どおり **一方向**(re-pack は作らない)。

- **identity 断絶は WP361 v3 差分で契約済み**(gid は消え、authored な通常
  component になる)。**再設計しない**
- journal の inverse は「prefab キーの復元 + 展開 component の除去」
  (undo のための逆操作であって、利用者向けの re-pack 機能ではない)
- unpack 後の component は authored なので `set_component_value` で編集できる
  (これが unpack の目的)
- 受け入れ: unpack → undo → redo で **authored index・component bytes・
  behavior seq が完全復元** / unpack 後に元 prefab を編集しても
  **unpack 済み object は影響を受けない**(参照が切れている証明)/
  Generated だった component が **`editable: true` になり `set_component_value` が通る**

#### 2. object picker と rename 連動

- `kind:"object"` パラメータの候補一覧を RPC で公開(scene 内の authored object 名 +
  `required_components` を満たすかの判定)
- **object の rename が `kind:"object"` パラメータ値を追随**する。
  同一 transaction・同一 inverse(undo で両方戻る)
- 受け入れ: rename → 参照が追随 → undo で両方戻る /
  `required_components` を満たさない候補は picker に**出るが選ぶと named 拒否**
  (出さないと「なぜ無いのか」が分からない。G0 可読性)

#### 3. Studio の behavior inventory(台帳の負債を返す)

WP361a レビュー #6 で台帳送りにした
「**Studio offline の behavior binding 乖離**」をここで閉じる ——
Studio は engine を link しないため behavior schema を知らず、
`behavior:<type>` provider を要求する prefab を offline で開くと
engine と違う結果になる。**behavior schema を project metadata / RPC catalog 経由で
Studio の pure leaf へ渡す**(D0: `pelican_core` を link しない)。
picker の inventory と同じ経路なので同段で行う。

#### 4. 一括流し込み面と純粋 dry-run

- **複数 object の prefab パラメータを 1 transaction で設定**する面
  (AI・表・外部データからの流し込み。design v4 の「parameters as bulk-import contract」)
- 既存の batch(all-or-nothing・inverse-list undo)・schema 検証・CAS を**そのまま使う**
- **dry-run は `ResolvedScene` の純 resolver 経路をそのまま使う**(裁定)。
  **新しい検証経路を作らない —— 第二の流儀の禁止**。
  適用せずに「何が起きるか・何が失敗するか」を返す
- 受け入れ: N 件の流し込みで 1 件だけ型不正 → **全件適用されない**(all-or-nothing)/
  dry-run の結果と実適用の結果が**同一の named エラー**になる /
  dry-run が **scene revision も ECS も arena も動かさない**(観測で)

#### やらないこと

パラメータ昇格(U3)/ `ctx.spawn`(U4)/ 多オブジェクト prefab(§7 の前提 4 件)/
optional slot・variant / HDA / attributes。

依存: WP361 + WP361a + WP362。

---

### WP371: R1 —— planner のパーサ統一(typed PassDefinition の単一消費)

**§4 規則 11 中段(複数経路にまたがる)。仕様レビュー + コードレビュー。見積: 中〜大。**
**規範は台帳のリファクタ計画 R1 と「引き継ぎ」の R1 裁定。**

#### 前提の実測(2026-09-01、起票時に確認)

`src/core/renderingpass/frameplanner.cpp` が **独自の JSON パーサを持っている**:
`requireString` / `parseStringList` / `parseOptionalStringList` と、
`nlohmann::json` を直接受ける関数群(121・185・219 行ほか)。
これは `passinfojsonparser` / `PassDefinition` が既に解析している**同じ文書の
第二のパーサ**であり、WP303・WP326 で 2 回踏んだ「同じ文書を読む第二経路」そのもの。

#### 範囲

1. **planner は typed `PassDefinition` だけを消費する**。`frameplanner` から
   raw JSON パースを**物理的に削除**(残すと第三の流儀になる)
2. パース責務は `passinfojsonparser` / project 層に一本化。
   planner が必要とする情報が typed 側に無ければ **typed 側に足す**
   (planner 側で JSON を読み直さない)
3. `framePlanToJson` は**出力**なので現状維持(WP354 で
   `CompiledRenderPipeline*` を受ける形になっている)

#### 受け入れ(§4 規約 10)

- **WP354 の fixtures が等価オラクル**: raw fixture(`runtime_projection.pre_wp354.json`
  ほか)と出荷 4 project の frame plan・GPU golden・cache key が**完全不変**
- **第二パーサの物理的消失**: `frameplanner.cpp` に `nlohmann::json` を引数に取る
  パース関数が**残っていないこと**を検査(削除の実測。ソース文字列検査ではなく、
  typed 経路を無効化すると planner が動かないことで示す)
- **旧が拒否し新が受理する JSON が無いこと**: 削除した分岐を
  `git show <SHA>^:frameplanner.cpp` と突き合わせ、**同じ名前付き違反で
  両経路が拒否する**ことを確認(WP324 の教訓 —— 集約で検証が弱くなる)
- RT の `compute_tasks` パーサに隣接するため、**RT1b/RT2a/RT2b の layout・
  hit_shaders・ray_bindings の解析経路が影響を受けないこと**を明示的に検査
- 全数 2 回 / GPU 直列は統合側 / matrix は SKIP_DEVSTUDIO 両構成

#### やらないこと

R5(型付き構築 factory・codec 記述子の単一ソース化)/ R6(fullscreen・
output_transform の variant tag)/ RT の parser 統合。

依存: WP354(fixtures がオラクル)+ RT2b 完了(隣接のため並行しない)。

#### WP370(U2)/ WP371(R1)仕様レビュー —— **両方とも前提不足で着手不能**

両方 Reject。**共通の根: 私が台帳の roadmap 項目を「要約」で扱い、本文の条件を
落として起票した**。教訓は §恒常の「やらない」に追記した。

##### WP370(U2)—— WP363 を依存から落としていた

- **前提の誤り**: 「引き継ぎ: 残 WP への先行裁定」の作業順は
  **WP363 → U2** と明記しているのに、WP370 の依存に WP363 が無い
- **なぜ致命か**: unpack は component を **generated → authored** へ移す。
  arena の再利用判定は authored↔authored か同一 gid の generated↔generated だけで、
  **generated↔authored は必ず不一致 → 新 handle 採番 → attachment の破棄と再生成**。
  そのときの callback・handle・rollback の正解を決めるのが WP363 である。
  現行 runtime routing も prefab 用 adapter を付けるのは set/unset だけ
- 他の実害:
  - **unpack の inverse が元の展開を特定できない**(prefab の bytes が変わった後の
    undo / 解放された `instance_id` の再利用)。inverse に prefab ref・instance id・
    parameter overrides・**prefab digest と provider fingerprint** を保存し、
    digest 不在時の named error を定義する必要
  - **「展開 bytes を保つ」と「authored として有効」が両立しない入力**がある
    (prefab body は `generated:true` を許すが authored では resolver が拒否)。
    さらに **codec の無い component は authored でも編集不能**なので
    「authored だから編集可能」は一般に偽
  - **rename の閉包が足りない**: `parent` 参照も同時に更新しないと
    `unknown parent` になる。閉包は `name + 全 parent refs + 全 object-parameter`。
    単一 PreparedOperation・複数 command・単一 inverse なら構造的に可能
  - **dry-run と実適用の named error 一致は現在の意味論では不可能**:
    resolver は cross-field failure を throw せず `runtime_resolution_error` に
    保存して成功扱いにする。実適用は adapter が失敗して `method_unavailable` になる

##### WP371(R1)—— `PassDefinition` は planner の入力になれない(循環)

- **致命**: `PassDefinition` は **物理 target plan 確定後**に構築される runtime 型で、
  concrete target metadata(sample count・array layer)に依存する。
  **早く parse すれば metadata 未確定、遅く parse すれば planner は終了済み** ——
  field を足しても解けない循環
- **表現力不足**: `PassDefinition` は compute / anchor / snapshot node を表せない。
  出荷 example には **snapshot node が 2 つ**あり、`clustered_lighting` は
  出荷 core resource の compute node
- **D0**: 現 `PassDefinition` は core 所属で Vulkan 型を含む。project 層の正本に
  そのまま移すと `pelican_project` を汚す
- **WP354 fixtures はオラクルにならない**: `familyFramePlan()` は 15 node を
  **手で構築**しており planner/parser を通らない。immutable fixture も barriers と
  reads/writes が空。`framePlanToJson` は footprint・resource access・attachment・
  region・resolution domain を**出力しない**が、それらは lowering で消費される
- **私が台帳の R1 本文を落としていた**: 正本は **上段(設計+仕様+コード)**・
  **exact fixture 8 件 + 出荷 4 plan + golden**・**等価性は両方向**
  (旧拒否→新受理 と 旧受理→新拒否)・**R2 は R1 に統合するか直後**。
  WP371 は中段・WP354 中心・片方向にしてしまった
- **旧拒否→新受理の具体例が既にある**: `"after": 7`(raw は非 string/array を拒否、
  typed は `after` を読まない)/ `"input_footprints":{"ghost":...}`(raw は
  current read でない名前を拒否、typed は無視)
- **正しい形**: `pelican_project` に **Vulkan 非依存の `TypedRenderGraphDocument`**
  (resource 宣言 + `variant<render, compute, anchor, snapshot>`)を設計し、
  **一度だけ parse → planner へ → physical plan 後に runtime `PassDefinition` へ lower**
  という二段構造。raw API の 3 caller(production compiler・graph transform registry・
  subgraph replacement registry)も範囲に含める

##### 結論と次の分岐

- **U2 は WP363(lifecycle × arena の設計)の後**。WP363 は起票のみで、
  実装前に設計文書 + 仕様レビューが必須(2 度失敗した領域)
- **R1 は設計文書(`TypedRenderGraphDocument`)から**。台帳の R1 本文どおり上段
- どちらも「設計から始める」段であり、**機能 WP として今すぐ実装するものではない**

#### 引き継ぎ更新(2026-09-01): U2/R1 の前提設計 2 本を起草済み

WP370(U2)/ WP371(R1)は両方とも仕様レビューで「設計が先」と差し戻された
(前節)。**必要最低限の設計文書を 2 本起草した**。次の担当(モデル問わず)は
orchestration.md §A の手順でここから回す:

| 文書 | 開くもの | 状態 |
|---|---|---|
| `docs/design_editor_lifecycle.md` | WP363 → U2(unpack・一括流し込み) | v1 草稿。**設計レビュー 3 巡 → 決定差分 → WP 分割(L1/L2 目安)** |
| `docs/design_render_graph_document.md` | R1(+R2 = G2) | v1 草稿。同上(G1/G2 目安) |

両文書とも「仕様の種」節に失敗レビューの確定処方を埋め込み済み
(lifecycle は 3 本 23 件 + WP370 レビュー、graph は WP371 レビューの全指摘)。
§未決がレビューで攻撃させたい点。**設計レビュー合格前の実装着手は禁止**
(lifecycle は同じ機構で 2 度 revert した領域)。

レビュー起動の形(スキル §5 どおり、codex read-only):
設計本文 path を先頭に置き、§未決を「重複不要・構造的な見落としを探せ」の
名指し対象として渡す。3 巡で打ち切り、残余は WP 仕様レビューへ。

### WP372: E1 —— create/destroy を含まない構造編集(unpack・rename 閉包・dry-run)

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 大。**
**規範は `docs/design_editor_lifecycle.md` の v3 + v4(設計レビュー 3 巡完了)。
矛盾したら v4 > v3 > v2 > v1。**

#### 0. 最初に判定すること(設計 v4 決定 1)

**この WP の成立条件は「prefab パラメータの変更を、既存 instance への `set_params`
として適用できること」である。** 現状は `prefab_scenes` 経路が
`set_params` junction を飛ばし(`prefab_scenes.empty()` でしか使われない)、
scene 全体の `BehaviorSceneProjectionAdapter` に切り替わり、arena の再利用条件に
`canonical_params` の一致があるため、**param を 1 つ変えるだけで新 handle →
旧 instance の `destroyInstance` + `onDestroy`** を通る。

**実装不能と判明した場合は UNMET として報告し、本 WP を中止すること**
(U2 は E2 = spawn/destroy の原子性設計待ちに戻る)。

#### 1. 範囲

1. **param 変更の instance 再利用**(決定 1): reconcile の照合が
   **entity + stable_name + origin(transition map 込み)が一致すれば、
   canonical params の差分を `set_params` として適用**する。
   params 不一致だけで再利用を諦めない。
   **これは利用者に見えるバグの修正でもある**(今は prefab の数値を 1 つ動かすと
   behavior instance の内部状態が失われる)
2. **`OriginTransitionMap` と unpack op**: `unpack_prefab` を journal の 1 op として
   追加。`sameSceneBehaviorOrigin` が **その execution の写像**を参照して
   generated(gid)↔ authored(component index)を同一と判定できるようにする。
   **attach/detach を通らない** —— `onInit`・`onDestroy` は発火しない。
   写像は journal の forward/inverse に載る(**永続 ID は作らない**。
   `AuthoringComponentId` は設計 v3 決定 3 で撤回済み)
3. **`ObjectReferenceIndex` と rename 閉包**: codec の semantic `object_ref`
   (JSON path つき)宣言に加え、**behavior の params schema
   (`BehaviorRegistration` 由来)にも object 参照を宣言できるようにする**
   (現在 provider は behavior field を一律 `BindableSemanticKind::Value` にしている)。
   rename は **単一 PreparedOperation・複数 command・単一 inverse** で
   `name + 全 parent refs + index 全体 + SceneLoader の name map` を原子更新
4. **エラー面の統一と dry-run**: `runtime_resolution_error` を**型付き named error**
   に格上げし、dry-run(純 resolver 経路)と実適用が**同一 code**。
   inactive scene の解決失敗は**保持**し、current へ切り替える瞬間に named 失敗
5. **CAS の拡張**: structural domain に**対象 object の component 列**を含め、
   他 actor の挿入を silent shift でなく**名前付き衝突**にする

#### 2. 受け入れ(§4 規約 10。全て「取り消したら落ちる」)

- **決定 1**: **実 producer request** の一括 prefab パラメータ変更で
  **handle 不変・create/destroy/`onDestroy` がちょうど 0 回**・
  behavior instance の内部状態が保存される。**照合を戻すと落ちる**
- **unpack**: attach/detach callback **ちょうど 0 回** / handle 不変 /
  arena の origin が generated→authored / **codec のある component は**
  `set_component_value` が通るようになる(**codec の無い component は
  `not_editable` のまま** —— 「authored だから編集可能」は偽) /
  undo で origin・gid・handle が復元
- **marker 除去**: prefab body が許す `generated:true` / `origin:"generated"` は
  authored では resolver が拒否するため、**unpack 時に除去**する。
  除去対象の**規範表**を本文に持ち、表に無い field が消えたら落ちる対照
- **rename**: `behavior.params.target` 経由の参照も追随(**unpack 後・reload 後も**) /
  参照 field を宣言しない external component が対象 scene にあれば named 拒否 /
  **出荷 4 project と core resources で実測**して壊さないこと
- **dry-run**: 実適用と同一 named code / scene revision・ECS・arena が不動
- **他 actor**: 対象 object の component 列を触られたら named 衝突(silent shift でない)
- 出荷 4 project・core resources の bytes 不変 / 全数 2 回 /
  matrix は SKIP_DEVSTUDIO 両構成 / GPU 直列は統合側

#### 3. やらないこと

**spawn / destroy の原子性(E2)** —— WP361a 状態の既知の穴 4 件はそこ。
object picker の UI・Studio の behavior inventory・一括流し込みの面・
Studio 表示(**U2 本体 = 別 WP**)。パラメータ昇格(U3)/ `ctx.spawn`(U4)/
多オブジェクト prefab。

依存: WP361 + WP361a + WP362。

#### WP372 仕様レビュー(v1、Reject・4 件)と v2

**§0 の成立条件は「実装可能」と検算された**(再利用条件から `canonical_params`
一致だけを外し、prepare で新 `RawBehaviorParams` を作り、publish で既存 instance を
next attachment へ移した後 **noexcept swap**、rollback で再 swap、finish で旧 params
破棄。Params の nothrow-swappable は登録時に要求済み)。
**したがって UNMET ではない。U2 は E2 待ちに戻さない。**
残り 4 件は「仕様が閉じていない」型。**全件「直す」。v2 > v1。第 2 巡を最終とする。**

| # | v1 の壊れた点 | v2 の決定 |
|---|---|---|
| 1 | 再利用条件の緩和が広すぎる(`pending` や registration owner まで落ちる)/ 既存 `set_params` token は **publish 後の rollback が no-op** で、`AfterPublication` 失敗時に live params だけ新値のまま残る | **外すのは `canonical_params` 一致だけ**と明記。`pending`・entity・stable_name・origin(写像込み)・**registration owner** は一致を要求。params 差分時は既存 `set_params` と同様に **activation failure / error / delay も更新**し、**rollback でそれらも復元**する。受け入れに **Prepare / Publish / AfterPublication の各 fault と rollback 後 retry** を入れ、`next_handle`・`next_attachment_seq`・params・activation fields・内部状態・callback trace を前後比較。**origin は汎用 runtime snapshot では観測できない**ので **arena の型付き snapshot を直接検査**する |
| 2 | CAS の新条件(component 列 domain)は**否定対照が恒真** —— 現行 domain は既に `object` を含み、overlap は同一 object で無条件 true を返す。新配線を全部消してもテストが落ちない | **v1 の範囲 5(component 列 domain)を削除**する。object 粒度の既存 CAS で十分。代わりに**設計が要求していたのに v1 が落とした rename の name reservation** を入れる: rename の structural domain に `{scene_id, object, name_reservations: [old, new]}` を必須化。**対照**: Actor A が rename → Actor B が new name で spawn → A が undo、で named conflict |
| 3 | unpack inverse の **prefab 依存閉包が消えている**(WP370 v1 レビューで確定済みだったものを引き継いでいない)。現行 undo は journal 作成時ではなく**undo 時点の registry/provider** で inverse を再構築する | canonical forward/inverse に **`ref` / `instance_id` / parameter overrides / prefab digest / provider fingerprint** を固定 schema で保存。**unpack/restore を prefab CAS の対象に含める**。**digest 不一致・provider 不一致・instance_id 再利用をそれぞれ別の named error** に。**prefab bytes と provider を変更した後の undo/redo を必須試験**にする |
| 4 | `OriginTransitionMap` の scope・最終化・production 配線が未定義(gid は scene を含まず scene-local / batch は operation ごとに逐次 commit するため記録 index が final とずれる / journal に map の文法が無い / **`unpack_prefab` は BehaviorScene adapter に route されていない**) | entry を **`{scene_id, authoring_object_id, old_gid, final_authored_component_index}`** とする。**request から受け取らず、preparer が全 operation 適用後の final ResolvedScene に対する第 2 pass で生成**する。上記 tuple で正準ソートし、**全単射・同一 object・同一 stable behavior・過不足なし**を検証。forward/inverse に正写像と逆写像を保存し、**undo/redo/verifier の各 execution で再検証**。**current scene の `unpack_prefab` を BehaviorScene adapter に明示 route** し、map を arena まで渡す。検査は**型付き arena snapshot** |

##### 壊せなかった点(v1 のまま維持)

reconciliation の publish/rollback 自体(desired attachment が新 origin を持ち、
publish は vector swap、rollback は旧 vector と count の復元)。

##### v2 の受け入れ(v1 §2 をこれで置換・追加)

v1 の受け入れに加えて:

- **決定 1 の fault matrix**: Prepare / Publish / AfterPublication × (成功・失敗・
  失敗後 retry)で、handle・seq・params・activation fields・内部状態・callback trace が
  **旧値へ完全復帰**(型付き arena snapshot で)
- **決定 2**: rename の name reservation 衝突が named。**同一 object の無関係な列を
  編集する正例が成功する**(過剰拒否でないこと)
- **決定 3**: prefab bytes 変更後・provider 変更後・`instance_id` 再利用後の undo が
  **それぞれ別の named error**。即時 undo は成功
- **決定 4**: batch `[unpack, その前の authored component を remove]` で index が
  ずれても写像が正しい / scene A・B に同一 gid が存在しても取り違えない /
  同一 stable behavior 2 個の entry を入れ替えると検証が落ちる

依存: WP361 + WP361a + WP362。

#### WP372 第 2 巡(FATAL 6 件)と v3 —— 分割(**v3 が最終。仕様レビュー打ち切り**)

6 件の内訳は「**文法 / 表 / API を定義せよ**」が 4 件(#2 判別子・#3 path 文法と
rename wire・#4 marker 規範表・#6 dry-run の error taxonomy)、
「**規範との矛盾**」が 1 件(#1 CAS)、「**受け入れが範囲外に依存**」が 1 件(#5)。

**判定: WP372 には未規定のサブシステムが 4 つ入っていた。** 1 WP に 4 つの
文法を書かせるのは、WP370/371 で 2 件同時に差し戻された形の再演である。**分割する。**

| WP | 範囲 | 定義すべき文法/表 | 状態 |
|---|---|---|---|
| **WP372a** | **決定 1 のみ**(prefab param 変更を既存 instance の `set_params` として適用) | **無し**(既存の照合条件から 1 項目を外すだけ) | **下記に確定版。起票済み** |
| WP372b | unpack op + `OriginTransitionMap` + inverse の prefab 依存閉包 | **source discriminator の規則**(#2)+ **marker 規範表**(#4) | 起票のみ |
| WP372c | `ObjectReferenceIndex` + rename op + CAS overlap | **共通 path 文法**(#3)+ **overlap 規範表**(#1)+ rename の request/forward/inverse wire | 起票のみ |
| WP372d | dry-run + 型付き resolution error | **error taxonomy と payload**(#6)+ 呼出境界(RPC method か edit mode か) | 起票のみ |

**#1 の解消**: v2 で「component 列 CAS を削除」としたのは**誤り**だった ——
設計 v4 は細粒度 CAS を要求しており、v2 の正例(同一 object の無関係な列編集後に
rename の undo が成功)は現行の「同一 object なら無条件 overlap」と両立しない。
**WP372c で overlap 規範表として決める**(削除は撤回)。

**#5 の解消**: `EditorCommandRpcAdapter::setPrefabParameter` は**単発の
`set_prefab_parameter` を出す実 producer として実在**する。
**状態消失バグはパラメータ 1 つの変更で観測できる**ので、WP372a は
一括 request を必要としない。一括流し込み面は U2 本体のまま。

### WP372a(確定版): prefab パラメータ変更で behavior instance を殺さない

**§4 規則 11 上段。仕様レビュー完了(2 巡 + 本裁定)。コードレビューあり。見積: 中。**
**規範は `docs/design_editor_lifecycle.md` v4 決定 1。**

#### 何が壊れているか(利用者に見える形)

prefab のパラメータを 1 つ変えると、`prefab_scenes` 経路が scene 全体の
`BehaviorSceneProjectionAdapter` に切り替わり、arena の再利用条件に
`canonical_params` の一致があるため **behavior instance が破棄・再生成される** ——
**内部状態が失われ、`onDestroy` / `onInit` が走る**。

#### 範囲

**再利用条件から `canonical_params` 一致だけを外す。**
`pending` / entity / stable_name / origin / **registration owner** の一致は
**引き続き要求**する(第 1 巡 #1: 緩めすぎると DLL 不在の pending attachment と
active attachment を同一視する)。

- prepare で新しい `RawBehaviorParams` を生成
- publish で既存 instance を next attachment へ移した後、**noexcept swap** で params 交換
- **rollback で再 swap** し、instance を旧 attachment へ戻す(既存 `set_params` token の
  publish 後 rollback が no-op である問題を、本 WP の token では起こさない)
- params 差分時は既存 `set_params` と同様に **activation failure / error / delay も更新**、
  **rollback でそれらも復元**
- Params の nothrow-swappable は登録時に要求済み。instance 移送と publish 後 rollback も
  arena に既存

#### 受け入れ(§4 規約 10)

- **実 producer**: `EditorCommandRpcAdapter::setPrefabParameter` が出す
  **単発 request** を実 `RpcServer::run` へ流し、**handle 不変・
  create/destroy/`onDestroy`/`onInit` がちょうど 0 回・behavior instance の内部状態が
  保存される**。**照合条件を戻すと落ちる**
- **fault matrix**: Prepare / Publish / **AfterPublication** × (成功・失敗・失敗後 retry)。
  失敗時は `next_handle`・`next_attachment_seq`・params・activation fields・内部状態・
  callback trace が**旧値へ完全復帰**
- **検査は型付き arena snapshot**(汎用 runtime snapshot は origin も params も
  観測できない)
- **緩めすぎの否定対照**: pending attachment(DLL 不在)と active attachment が
  **同一視されない** / registration owner が異なる場合は再利用しない
- 出荷 4 project・core resources の bytes 不変 / 既存テスト(全数 1494)を壊さない /
  全数 2 回 / matrix は SKIP_DEVSTUDIO 両構成

#### やらないこと

unpack(372b)/ rename・`ObjectReferenceIndex`・CAS(372c)/ dry-run(372d)/
一括流し込み面・picker・Studio inventory(U2 本体)/ spawn・destroy の原子性(E2)。

依存: WP361 + WP361a + WP362。

#### WP372a 完了(統合済み、`a6272980`)

**全数 1497/1497 緑**(GPU 171 件を直列で含む。基準 1494 → +3)。
`SKIP_DEVSTUDIO` OFF 構成 + `PELICAN_TEST_PROJECTS_DIR` あり。doclink 2341 ok。

**利用者に見えていた欠陥が閉じた**: prefab のパラメータを 1 つ変えると
behavior instance が破棄・再生成され内部状態が失われていた。
params は prepare で用意し、publish で生存中の instance へ swap、
rollback で再 swap して戻す。identity には **registration owner と pending** を
加えたので、DLL 不在の pending attachment が active と同一視されることはない。

**検出力は統合側で 2 つの変異により自分で確認した**(エージェントの報告を額面で
受け取っていない):

| 変異 | 結果 |
|---|---|
| 再利用条件に `canonical_params` 一致を戻す(= 本 WP の取り消し) | **2 件とも失敗**。実 producer は `after.handle == before.handle` で落ちる |
| rollback の再 swap を 1 行コメントアウト | **fault matrix が失敗**(`wp372aArenaSnapshot(arena) == before`) |

**運用の教訓(2 件、いずれも自分の手順の穴)**:

1. `git commit -F <(...)`(プロセス置換)が**無言で失敗**し、squash merge が
   index に載ったまま HEAD は台帳コミットのままだった。ビルドと全数は
   作業ツリーに対して走っていたので結果自体は有効だったが、
   **`&&` 連鎖の最後の `git log` を読まなかったので気付かなかった**。
   → **コミット後は必ず `git log --oneline -1` を読む。**
2. 変異テストの perl 正規表現が**当たらず**、変異ゼロのまま「緑」を得て
   一度「対照が効かない」と誤判定しかけた。
   → **変異は `git diff` で適用を確認してからビルドする。**
   (§4 規約 10 の対照は、対照が適用されていることの確認までが対照である)

`apply_prepared_params` が本物の `swap`(noexcept)であることは
`registerer.hpp:256` で確認済み ―― 二度呼べば戻るので rollback は正しい。

### WP372b: unpack —— origin 遷移・marker 規範表・inverse の prefab 依存閉包

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 中〜大。**
**規範は `docs/design_editor_lifecycle.md` v4 の裁定 2〜4。**
**前提は WP372a(既存 instance を殺さない params 適用)。**

#### 起票時の実測(2026-09-01。この節を書く前に読んだもの)

- `GeneratedComponentOrigin` は **`{stable_generated_id, prefab, instance_id,
  component_key, digest_sha256}`** を持つ([resolvedscene.hpp:117](../src/core/loader/resolvedscene.hpp#L117))。
  **判別子は既に存在する**(WP372 第 2 巡 #2 は「id が body から消える」と述べたが、
  消えるのは *body* であって *origin* ではない)
- generated marker は **2 綴りだけ**: `generated`(boolean true)と
  `origin`(string `"generated"`)([resolvedscene.cpp:130](../src/core/loader/resolvedscene.cpp#L130))
- authored component がその marker を持つと
  **`generated_component_unsupported` で拒否される**([resolvedscene.cpp:771](../src/core/loader/resolvedscene.cpp#L771))。
  呼び出しは `resolveComponent` の 1 箇所のみ
- **出荷 4 project にも `src/core/resources/` にも marker は 1 件も無い**(grep 実測)。
  したがって marker 除去は出荷物の bytes を動かさない
- 逆に **prefab の component body は marker を持てる**(generated origin では
  検査されないため)。**unpack するとその body が authored になり拒否される** ——
  これが規範表を書く理由である

#### 1. marker の規範表(**これが無いと unpack は出荷できない**)

unpack が authoring document へ書く component body に対し、**この表の行だけ**を適用する。

| # | JSON path | 述語 | unpack での動作 | inverse に保存するもの |
|---|---|---|---|---|
| 1 | `/generated` | `is_boolean() && value == true` | **キーごと除去** | キーの存在と厳密値(`true`) |
| 2 | `/origin` | `is_string() && value == "generated"` | **キーごと除去** | キーの存在と厳密文字列 |

**除去してはならない否定行**(精度の対照。取り違えると利用者の宣言を壊す):

| # | 例 | 理由 |
|---|---|---|
| 3 | `"generated": false` | 述語を満たさない。**そのまま残す** |
| 4 | `"origin": "authored"` ほか任意の他文字列 | 述語を満たさない。**そのまま残す** |
| 5 | `/generated` が数値・文字列など非 boolean | 述語を満たさない。**そのまま残す**(型検査は resolver の仕事) |

表の**行数が仕様**である。`requestsGeneratedComponent` に第 3 の綴りを足す改修は、
**この表を同時に更新しなければ通らない**ようにテストで縛ること。

#### 2. `OriginTransitionMap` の生成規則(第 2 巡 #2 の答え)

- **キーは `stable_generated_id`**。body の値一致に頼る推測は一切しない
- 生成は preparer が **最終 `ResolvedScene` を 1 度走査**して行い、
  対象 instance の generated component を **resolved の出現順**に列挙、
  同じ順に unpack 後 authoring object で得る index を割り当てる
- 検証(**生成時に行い、違反は名前付き拒否**):
  1. gid 集合が、その instance の generated component 集合と**厳密一致**
  2. **`i` 番目の entry の gid が、resolved 出現順の `i` 番目と一致**
  3. `authored_index` が**狭義単調増加**
- **条件 2 が「2 つの entry を入れ替える」攻撃を落とす。**
  同一 `type`・同一 params の behavior が 2 つあっても、gid が異なるので確定する
- 写像は **journal の forward と inverse の両方に保存**する。
  unpack 後は gid が scene から消えるため、**undo 時には再導出できない** ——
  保存する理由はここにあり、「決定的だから省ける」ではない
- 有効範囲は**その 1 回の execution 内のみ**(v4 裁定 3。永続 ID は作らない)

`sameSceneBehaviorOrigin` が execution の写像を参照し、gid ↔ authored index を
同一と判定する。これにより unpack は attach/detach を通らない。

#### 3. unpack op と inverse

`unpack_prefab`: object の `prefab` キーを展開済み components への置換で消す。
**object 名は不変。一方向**(re-pack は作らない)。identity 断絶は WP361 v3 で契約済み。

inverse は **prefab 依存閉包を復元**する。保存し、復元し、
**不一致はそれぞれ別の名前付きエラー**にすること:

| 保存する物 | 不一致時 |
|---|---|
| `ref` | 別コード |
| `instance_id` | 別コード |
| overrides(パラメータ値) | 別コード |
| prefab digest | 別コード |
| provider fingerprint | 別コード |

「まとめて 1 つの `unpack_inverse_mismatch`」にしないこと ——
どれが動いたのかが分からないと利用者は復旧できない。

#### 受け入れ(§4 規約 10)

- **実 producer**: RPC 経由の `unpack_prefab` を実 `RpcServer::run` へ流し、
  **attach/detach callback がちょうど 0 回・`onInit`/`onDestroy` 0 回・handle 不変・
  behavior instance の内部状態が保存**。origin 遷移を外すと落ちる
- **marker 表の全 5 行**を個別に検査。行 1・2 は除去され、**行 3〜5 は保存される**。
  行 1 を除去しない変異で、unpack 後の reload が
  `generated_component_unsupported` で落ちることを示す
- **写像の入れ替え攻撃**: 同一 `type`・同一 params の behavior 2 つを持つ prefab で、
  手で写像の 2 entry を入れ替えたものが**条件 2 で名前付き拒否**される
- **undo/redo**: origin・gid・authored index・component bytes・behavior seq が完全復元。
  **undo 後に元 prefab を編集すると unpack 済み object が追随し、
  redo 後は追随しない**(参照が切れている証明)
- **inverse 閉包の 5 項目**を 1 つずつ壊し、**それぞれ別の名前付きエラー**
- `set_component_value` が unpack 後に通る(**codec がある component のみ**。
  「authored だから編集可能」は偽)
- 出荷 4 project・`src/core/resources/` の bytes 不変 /
  既存テスト(全数 1497)を壊さない / 全数 2 回 / matrix は `SKIP_DEVSTUDIO` 両構成

#### やらないこと

rename・`ObjectReferenceIndex`・CAS(WP372c)/ dry-run と型付き resolution error
(WP372d)/ picker・Studio inventory・一括流し込み(U2 本体)/
spawn・destroy の原子性(E2)/ re-pack。

#### WP372b 仕様レビュー第 1 巡(11 件・破綻 3)と v2

**v2 が本節の正。v1 の §1〜§3 と受け入れは v2 で置換される。**

**自分の退行を先に書く**: 第 1 巡 #3 は「gid は `instance_id` + `component_key`
だけから作られ、scene も object も含まない」([prefab.cpp:1040](../src/project/prefab.cpp#L1040))
—— **scene A と B が同じ `instance_id` を持てば gid は衝突する**(一意性検査は
どちらも scene-local: [resolvedscene.cpp:420](../src/core/loader/resolvedscene.cpp#L420))。
WP372 v2 では entry を `{scene_id, authoring_object_id, old_gid, final_index}` と
**既に決めていた**のに、372b で裸の gid へ**自分で退行させた**。
台帳の本文ではなく自分の要約から書いたときに起きる、記録済みの失敗そのものである。

##### 決定 1(#1): 規範参照

正確な規範は「**v4 により維持された v3 の裁定 2〜4**」。
v4 の決定 2(`ObjectReferenceIndex`)・決定 3(component 列 CAS)は **WP372c** で扱う。
**372c 以前は既存の object 粒度 conflict がそのまま安全性を担う**(弱くしない)。

##### 決定 2(#2): 写像は 2 snapshot から作る。`unpack_prefab` は単独 operation

- 旧側 witness(`scene_id`・`authoring_object_id`・gid・`component_key`・
  behavior stable name)は **`base_resolved`** から取る
- 最終 index は **`next_resolved`** から取る
  ([editorprojectiontransaction.hpp:166](../src/core/loader/editorprojectiontransaction.hpp#L166))
- **`unpack_prefab` を含む request は、その 1 operation のみを含むこと。**
  他 operation との混在は `unpack_must_be_singleton@1` で名前付き拒否。
  batch を許すと「全 operation 適用後に final index を確定する第 2 pass」が要り、
  それは E2 の原子性設計に属する

##### 決定 3(#3): entry の形と全単射検証

entry は **`{scene_id, authoring_object_id, gid, component_key, final_authored_index}`**。
検証は集合一致ではなく**全単射**とし、**生成時に行い違反は名前付き拒否**:

1. gid 集合が `base_resolved` の当該 instance の generated component 集合と厳密一致
2. `final_authored_index` の集合が、**`next_resolved` の当該 object 上の
   期待 index 集合と厳密一致**(#3 入力 2 の「正しい index が `[2,3]` のとき
   `[0,1]` にする」改竄はここで落ちる)
3. index に重複が無く、範囲内である
4. 各 entry の **component kind と behavior stable name** が両 snapshot で一致
5. `(scene_id, authoring_object_id)` が operation の対象と一致

##### 決定 4(#4): 行数ではなく**単一の production-owned 分類器**

「表の行数を検査する」は代理条件だった(第 3 の綴りを足しても固定 fixture は
動かず、検出には source 文字列検査が要る)。**撤回する。**

- marker 判定を **`prefabGeneratedMarkerRule` 1 箇所**に置き、
  **`requestsGeneratedComponent`(resolver)と unpack の両方がそれを呼ぶ**
- 規則は「**分類器が marker と報告した field だけを除去し、
  それ以外の root field と値はすべて byte 保存**」という普遍則にする
- 検査は **property corpus**: `generated` と `origin` の**両キーの直積**を、
  値域(`true` / `false` / `"generated"` / `"authored"` / 他文字列 / 数値 / `null` /
  キー不在)で回し、除去されたものが分類器の報告と厳密一致することを確認する
- **第 3 の綴りを分類器に足すと unpack へ自動的に伝わる。** 表の同期は不要になる

##### 決定 5(#5): 否定対照は初回 Prepare で取る

candidate は authoring command 適用直後に resolve される
([editorprojectiontransaction.cpp:829](../src/core/loader/editorprojectiontransaction.cpp#L829))
ので、「marker を残して unpack が成功し、後の reload で落ちる」は**本番経路で到達不能**。

対照は同じ実 RPC テスト内で: **正常な分類器は commit し、除去を無効にした変異は
初回 Prepare で `generated_component_unsupported` により拒否され、
scene revision・journal・arena が不変**。

##### 決定 6(#6): route と arena origin を受け入れに入れる

v1 の受け入れ(callback 0・handle 不変・内部状態保存)は、
**document command だけ実装して `editorruntimefactory` の route を足さなくても通る**
(arena が古い generated origin のままなので何も起きない)。**規約 10 違反だった。**

- 受け入れに **「型付き arena snapshot の origin が generated から authored に変わる」**を
  追加。**route を外す変異で落ちること**
- `unpack_prefab` を **`prefab_mutation` として分類**し、undo CAS 分類にも加える
  ([editorjournal.cpp:2097](../src/core/communication/editorjournal.cpp#L2097))
- **object 全体を覆う structural domain** を持たせる
  (別 actor の同 object 編集を黙って捨てない。既存 overlap で衝突扱いになる)
- RPC は固定 schema: `actor_id`, `scene_revision`, `closure_generation`,
  `provider_generation`, `object`

##### 決定 7(#7): 権威は 1 つ。forward と inverse は機械生成し整合を検証

inverse の index を全部 `+1` した改竄は v1 の 3 条件を通り、undo で handle 保持が壊れる。

- **canonical transition witness を 1 つ作り、forward map と inverse map は
  そこから機械生成**する
- journal 追加時に **`inverse == invert(forward)`** を検証する
- 両方に載せること自体は合理的(undo は inverse operation しか実行しない:
  [editorjournal.cpp:3133](../src/core/communication/editorjournal.cpp#L3133))。
  欠けていたのは単一権威だった
- 対照: **片側だけを壊した fixture が拒否される**

##### 決定 8(#8): 閉包を実体に合わせ、object parameter は名前付き拒否

v1 の 5 項目は現行の closure identity ではない
([resolvedscene.hpp:94](../src/core/loader/resolvedscene.hpp#L94)、
`sameWireIdentity`: [resolvedscene.cpp:363](../src/core/loader/resolvedscene.cpp#L363))。
provider fingerprint は descriptor しか hash せず `behavior_dependencies` を含まない。

- witness に **behavior dependency tuple(stable name・schema version・fingerprint)**と
  **resolver input fingerprint** を含める
- **`kind:"object"` パラメータを宣言する prefab の unpack は
  `prefab_unpack_object_parameter_unsupported@1` で名前付き拒否**する。
  参照追随には `ObjectReferenceIndex` が要り、それは WP372c である
  (#8 反例 2: undo までの間に参照先が destroy されると
  `PrefabObjectRefUnresolved` で落ちる)。**372c で拒否を解除する。**
  372b を 372c 依存にはしない —— 分割の目的は「1 WP に 1 つの文法」である

##### 決定 9(#9): エラー表と、到達不能な項目の除外

「5 つを別コード」は名前付きエラーの仕様になっていなかった。**表にする**:

| 検査順 | 条件 | code | payload |
|---|---|---|---|
| 1 | journal 内部(map・保存 `prefab` block・witness)の改竄 | `journal_integrity_error@1` | 何が矛盾したか |
| 2 | `ref` 不一致 | `prefab_ref_changed@1` | expected / current |
| 3 | `instance_id` 不一致 | `prefab_instance_id_changed@1` | expected / current |
| 4 | prefab digest 不一致 | `prefab_digest_changed@1` | expected / current |
| 5 | provider fingerprint 不一致 | `prefab_provider_changed@1` | expected / current |
| 6 | behavior dependency と resolver input fingerprint の不一致 | `prefab_behavior_dependency_changed@1` | expected / current |

- 検査は inverse の preflight resolve **より前**に置く
  ([editorjournal.cpp:1687](../src/core/communication/editorjournal.cpp#L1687))。
  でないと generic な prefab/schema error が先に出る
- **override 単独の不一致は 5 分割から除外**する ——
  digest と provider が同じなら保存した override は同じ resolver で再び有効になり、
  **本番状態から独立に壊せない**(壊すには journal 改竄しかなく、それは検査 1)

##### 決定 10(#10): 書き込む bytes の権威

候補が 3 つあった(`resolvePrefabComponentBody` 直後 / `source_json_exact` /
`effective_json`)。**権威を明示する**:

- unpack が authoring document へ書くのは
  **`resolvePrefabComponentBody` の結果から、分類器が報告した marker だけを除いたもの**
- **codec の default canonicalization(`effective_json`)は書かない。**
  behavior の canonical params による上書き
  ([resolvedscene.cpp:183](../src/core/loader/resolvedscene.cpp#L183))も書かない
- inverse は **元の `prefab` block を ordered JSON として逐語保存**する。
  semantic な `ref` / `id` / `overrides` だけを保存すると
  **`"parameters":{}` とキー不在を区別できない**([prefab.cpp:629](../src/project/prefab.cpp#L629))。
  依存 witness は**別フィールド**に置く

##### 決定 11(#11): pending 構成を範囲に入れる

editor の adapter は availability を常に `active` として prepare している
([editorprojectionadapters.cpp:709](../src/core/loader/editorprojectionadapters.cpp#L709))が、
通常の scene loader は DLL 不在を pending として扱う
([scene.cpp:311](../src/core/loader/scene.cpp#L311))。

**adapter へ実際の availability を渡し**、**active と DLL 不在の両方**で
pending flag・handle・seq・origin が保存され callback 0 であることを本番テストにする。

##### 受け入れ(v1 を置換)

- **実 producer**: 実 `RpcServer::run` 経由の `unpack_prefab` で
  attach/detach と `onInit`/`onDestroy` が**ちょうど 0 回**、handle 不変、内部状態保存、
  **かつ型付き arena snapshot の origin が generated から authored に変わる**。
  **route を外すと落ちる**
- **marker**: 分類器の property corpus(両キー直積 × 8 値域)。
  **除去を無効にした変異が初回 Prepare で `generated_component_unsupported`** となり、
  revision・journal・arena が不変
- **写像**: entry 2 件 swap / index を `[2,3]` から `[0,1]` へ / inverse だけ `+1` /
  別 scene の同 gid —— **それぞれ別の名前付き拒否**
- **`set_component_value`**(規約 10。positive だけにしない):
  **unpack 前は同じ論理 field が `prefab_generated_read_only` で拒否**され、
  **unpack 後は成功し実 resolved 値が変わる**
  ([editorcommandservice.cpp:1208](../src/core/communication/editorcommandservice.cpp#L1208))。
  **codec がある component のみ**
- **参照断絶**: unpack 後に prefab **asset** を更新しても unpacked object は変わらず、
  **undo すると追随する**。(v1 の「undo 後に編集して redo」は
  **新規 commit が redo stack を消す**ため到達不能:
  [editorjournal.cpp:2633](../src/core/communication/editorjournal.cpp#L2633)。**撤回**)
- **inverse 閉包**: 表の code 2〜6 を 1 つずつ、**本番状態から**壊して個別に確認。
  改竄由来は code 1 に集約
- **pending**: DLL 不在構成でも同じ保存性
- **`unpack_must_be_singleton@1`** と
  **`prefab_unpack_object_parameter_unsupported@1`** の名前付き拒否
- 出荷 4 project と `src/core/resources/` の bytes 不変
  (**回帰 guard であって feature 有効性の証拠ではない** —— 対象 root に prefab は 0 件)
- 既存テスト(全数 1497)を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成

#### WP372b 第 2 巡(8 件)と v3 決定差分 —— **v3 が最終。仕様レビューはここで打ち切る**

v3 は v2 への差分であり、**触れていない決定は v2 のまま生きている**。

##### 差分 1(#3): 写像 entry を**発生源に束縛**する

第 2 巡が通した攻撃: **同一 `stable name`・異なる `component_key`** の behavior 2 つ
(同一 stable name の behavior 複数は許可されている:
[resolvedscene.cpp:515](../src/core/loader/resolvedscene.cpp#L515)、
既存 fixture にも 2 個ある: [wp361_prefab_test.cpp:267](../test/wp361_prefab_test.cpp#L267))。
gid 集合・index 集合・重複・kind・stable name・scene/object の 6 条件を**全部通り**、
A の内部状態が論理 component B へ移植される。
**`component_key` を entry に持たせながら、一度も突き合わせていなかった。**

追加する条件(v2 の 5 条件に足す):

6. `entry.component_key` が **`base_resolved` の当該 generated origin の
   `component_key` と一致**
7. `entry.gid` が **`prefabGeneratedId(instance_id, component_key)` と一致**
   (gid の導出規則そのものに戻して検算する)
8. `final_authored_index` の component が、**その `component_key` の権威 body
   (差分 5 / v2 決定 10)から生成されたもの**であること

必須対照: **同一 stable name の behavior 2 個の swap** と、
**`component_key` だけを書き換えた改竄**。

##### 差分 2(#4): oracle を分類器から独立させる

v2 の property corpus は「除去結果が分類器の報告と一致」しか見ておらず、
**分類器自身を oracle にしていた**。分類器を「キーが在れば値によらず marker」と
誤らせても、resolver と unpack が同じ誤りを共有するので全条件が通る。

**仕様に期待値を固定する**(分類器の実装とは独立に):

- `/generated` は **boolean `true` のときだけ**除去
- `/origin` は **string `"generated"` のときだけ**除去
- `false` / `"authored"` / 他文字列 / 数値 / `null` / 他の root field は**保持**

production helper は 1 つのままでよい(resolver と unpack が共有する)。
綴りを足す改修は、**分類器と独立 oracle の両方を明示的に更新しなければ通らない**。

##### 差分 3(#1): asset 更新と undo の順序

v2 の受け入れは「asset 更新後に undo して追随」と
「digest 不一致を `prefab_digest_changed@1` で拒否」を**同時に要求**していた。
digest は raw bytes から作られる([prefab.cpp:599](../src/project/prefab.cpp#L599))ので、
両立しない。**受け入れの手順を確定する**:

1. unpack する
2. prefab asset を更新 → **unpacked object は変化しない**(参照断絶の証明)
3. **asset を元の bytes へ戻す**
4. undo が成功する
5. もう一度 asset を更新 → **復元された prefab 参照が追随する**

##### 差分 4(#2): `ref` と `instance_id` は integrity に畳む

unpack 後は object の `prefab` キー自体が消えるため、**current state に `ref` も
`instance_id` も存在しない**。両者は inverse に保存した block からしか読めない
([prefab.cpp:629](../src/project/prefab.cpp#L629))ので、
「本番状態から独立に壊す」ことができない。

- code 2(`prefab_ref_changed@1`)と code 3(`prefab_instance_id_changed@1`)を**削除**し、
  **`journal_integrity_error@1` の下位 reason**(`ref` / `instance_id`)にする
- **本番状態から壊せる code は 3 つだけ**: digest / provider / behavior dependency
- 受け入れの「code 2〜6 を本番状態から 1 つずつ」は
  **「digest・provider・behavior dependency の 3 つを 1 つずつ」**に置換

##### 差分 5(#5): 検査点を core の callback にする

`commandsFromCanonical` の明示 transaction より前に `LocalDocumentTarget` の
constructor が resolver を呼ぶ([editorjournal.cpp:439](../src/core/communication/editorjournal.cpp#L439))ので、
v2 が名指しした seam には置けない。undo 受付時も既存 CAS と precondition が先にある。

- **core 内 callback を足し、`projection_state` または完全な
  `PrefabResolutionClosure` を返す**(runtime dependencies は registry・provider・
  closure generation までしか届かず、resolver input fingerprint は
  実 defaults から作られる: [resolvedscene.cpp:673](../src/core/loader/resolvedscene.cpp#L673))
- **journal 取得直後・generic CAS/precondition より前**に、
  **受付時と実行時の両方で同じ検査関数**を呼ぶ
- preflight にも**現在の defaults** を渡す
- 対照: **複数の不一致を同時に作り、表の優先順が守られること**

##### 差分 6(#6): 決定 11 を格下げする(generated pending は到達不能)

production provider は**登録済み behavior からしか dependency を作らない**
([prefabprovider.cpp:401](../src/core/loader/prefabprovider.cpp#L401))。
そして generated behavior の dependency が provider に無ければ、resolver は
pending にせず **`PrefabProviderStale` で fatal** にする
([resolvedscene.cpp:716](../src/core/loader/resolvedscene.cpp#L716))。
**「DLL 不在の generated behavior」は作れない。**

- 決定 11 は **「availability 配線が、無関係な authored pending を壊さないこと」**に限定
- **origin 遷移の証拠として数えない**(そう数えるのは代理条件)
- **台帳の負債**: DLL 不在でも behavior dependency schema を供給できる
  manifest-backed provider。これが入るまで generated pending は存在しない

##### 差分 7(#7): undo/redo の本番 lifecycle 受け入れを**復活**させる

v2 が受け入れを全面置換したとき、v1 にあった undo/redo の確認を落とした。
通る変異: **「写像を通常 edit では渡し、undo/redo ticket では渡さない」** ——
forward・singleton・journal 整合・document 復元は全部通り、
undo/redo でだけ behavior が detach/create される。

**同一 RPC テストで unpack → undo → redo を実行**し、各遷移で
callback 0 / handle・seq・内部状態不変 / origin と gid の往復 /
**forward と inverse が各 1 operation**であることを検査する。

##### 差分 8(#8): **object parameter の拒否を撤回する**(v2 決定 8 の後半を取り消し)

第 2 巡は「決定 8 が設計の規範例と主要 fixture を全部拒否し、
372b が単独出荷の有用性を証明できない」と指摘した。**指摘は正しいが、
正しい解決は 372c との合体ではなく、拒否そのものの撤回である。**

`PrefabObjectRefUnresolved` は **対象 object 名が scene に存在しない**ときだけ出る
([resolvedscene.cpp:565](../src/core/loader/resolvedscene.cpp#L565))。
名前が消えるのは **destroy(E2。E1 の範囲外)か rename(372c。今日は未実装)**だけである。
**E1 には destroy 操作が存在しない**ので、第 1 巡 #8 反例 2 の状況は E1 内で作れない。

- **object parameter を持つ prefab も通常どおり unpack する。**
  置換済みの値(object 名)を権威 body から逐語で書けば、resolve 結果は同一
- `prefab_unpack_object_parameter_unsupported@1` は**作らない**
- **正の受け入れ**: 設計の規範例([design_prefabs.md:51](design_prefabs.md#L51))と
  主要 fixture([wp361_prefab_test.cpp:244](../test/wp361_prefab_test.cpp#L244))の形、
  すなわち **object parameter を持つ prefab を unpack して、
  解決後の参照が変わらない**こと
- rename 追随は 372c のまま(**今日も未実装なので後退は無い**)
- **台帳の負債**: E2 が destroy を入れた後、参照先が destroy された状態での
  unpack の undo は既存の `PrefabObjectRefUnresolved` で落ちる。
  これは真の衝突であり、名前付きで落ちること自体は正しい

##### 実装可否

**実装する。** 第 2 巡の 8 件はすべて上記で閉じた。
「1 WP に 1 つの文法」という分割の前提は #8 の撤回により回復している
(372b = 写像と marker の文法、372c = path と overlap の文法)。

### WP372b-fix: 受け入れが本番のループを再実装していた(第二の流儀)

**§4 規則 11 中段。コードレビューのみ。見積: 小。連鎖打ち切り —— 本 WP で閉じる。**

#### 統合側で見つけたこと(エージェントの報告は誤りだった)

WP372b の報告は「route を無効化する実変異で arena origin が generated のままとなり失敗」
と書いていた。**統合後に自分で変異を当てたところ、5 件すべて緑のまま通った。**

- 変異: `editorruntimefactory.cpp:831-832` の
  `prefab_origin_transitions.insert_or_assign(route.scene_id, route.origin_transition)`
  を 2 行ともコメントアウト(`git diff` で適用を確認済み)
- 結果: **`ctest -R WP372b` が 5/5 緑**
- 対照として `resolvedscene.cpp:579-582` の `component_key` 束縛検査を外した変異は
  **正しく赤くなった**(`ComponentKeyMismatch`)。変異の当て方が悪いのではない

**原因**: 受け入れテストが**本番の adapter 構築ループを再実装している**
([wp361_prefab_test.cpp:3948](../test/wp361_prefab_test.cpp#L3948))。
`planEditorPrefabProjectionRoutes` までは本番を通るが、その後は
`route.origin_transition` を adapter へ**直接**渡しており、本番側の

- `if (!route.origin_transition.entries.empty())` guard と dict への insert(830-833)
- `transition == end() ? OriginTransitionMap{} : transition->second` の引き当て(1049-1057)
- availability の同型の引き当て

を**一度も通らない**。これは WP303・WP326 で 2 回踏んだ
「**同じ仕事をする第二の経路**」であり、今回はそれが**テスト側**に生えた。

**したがって第 2 巡 #6 は閉じていない。** 「document command だけ実装して route を
足さなくても通る」は塞がったが、**route の最後の一跳ねを消しても通る**状態が残った。
本番の editor で unpack すると behavior が破棄されるのに、全数は緑のままになる。

#### 範囲

**本番と受け入れが同じ 1 つの関数を呼ぶようにする。**

- `internal::` に **routes から「scene_id → {`OriginTransitionMap`, availability}」を作る
  関数**を 1 つ置く(`empty()` guard と引き当ての既定値もその中に入れる)
- `makeEditorRuntimeService` の該当ループ(830-833 と 1049-1057)を**それに置き換える**。
  本番側にロジックを残さない
- 受け入れテストの手書きループを**同じ関数の呼び出しに置き換える**

ヘッダの既存コメントが planner の存在理由を
「an unpack operation cannot silently omit the behavior scene reconciliation adapter」
と書いている([editorruntimefactory.hpp:30](../src/core/communication/editorruntimefactory.hpp#L30))。
その seam を**もう一段だけ**伸ばすことであり、テストのために新しい設計を入れるのではない。

#### 受け入れ(§4 規約 10)

- **`prefab_origin_transitions` への insert を消す変異で、WP372b の実 RPC テストが落ちる**
  (これが本 WP の存在理由。統合側で私が実際に当てて確認する)
- **availability の引き当てを消す変異でも落ちる**
- 受け入れテストに `BehaviorSceneProjectionAdapter` を**直接構築する箇所が残っていないこと**
  を、テストを通して示す(構築を関数に寄せたので、関数を壊せば全部落ちる)
- 既存 1502 件を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成 /
  出荷 4 project と `src/core/resources/` の bytes 不変

#### やらないこと

WP372b の仕様変更・写像の意味論の変更・`makeEditorRuntimeService` 全体の
テスト可能化(重い。今回の穴は最後の一跳ねだけ)/ WP372c / WP372d。

#### 教訓(恒常)

**「本番経路を通す」は入口だけでは足りない。** 実 `RpcServer::run` を通していても、
テストが途中から本番のループを写経していれば、**写経した範囲は検証されていない**。
受け入れが本番と同じ関数を呼んでいるかを、**変異で**確かめること ——
エージェントの「この変異で落ちる」という報告は、**当てて確かめるまで主張にすぎない**。

#### WP372b + WP372b-fix 完了(統合済み、`2620f640` + `a7ef7eef`)

**全数 1502/1502 緑**(GPU 171 件を直列で含む。1497 → +5)。`SKIP_DEVSTUDIO` 両構成。
出荷 4 project と `src/core/resources/` の bytes 不変。

**閉じたもの**: unpack が behavior を殺さなくなった。generated → authored の
origin 遷移は、その 1 回の execution の `OriginTransitionMap` を
`sameSceneBehaviorOrigin` が参照することで attach/detach を通らずに行われる。
写像の entry は `{scene_id, authoring_object_id, gid, component_key,
final_authored_index}` で、**gid は `component_key` から再導出して検算する** ——
同一 `type` の behavior 2 個は正当なので、entry の入れ替えは状態の移植になる。

##### 検出力(統合側で自分で当てた変異。**エージェントの報告は 1 件が誤りだった**)

| 変異 | 1 回目(WP372b 時点) | 2 回目(fix 後) |
|---|---|---|
| `component_key` 束縛検査を外す | **赤**(`ComponentKeyMismatch`) | — |
| `prefab_origin_transitions` への insert を外す | **緑のまま(すり抜け)** | **赤**(`unpacked.handle` が `2 == 1`) |

すり抜けの原因は、受け入れテストが `.execute` の中で
**本番の adapter 構築ループを写経していた**こと。`planEditorPrefabProjectionRoutes`
までは本番だが、その先は `route.origin_transition` を adapter へ直接渡しており、
本番側の dict への insert と引き当てを一度も通らなかった。
WP372b-fix で**本番と受け入れが同じ 1 つの関数を呼ぶ**ようにし、
**同じ変異が赤くなること**を確認した。

##### 恒常の教訓

**「本番経路を通す」は入口だけでは足りない。**
実 `RpcServer::run` を通していても、テストが途中から本番のループを写経していれば、
**写経した範囲は検証されていない**。WP303・WP326 で踏んだ「第二の経路」は
本番コードだけでなく**テスト側にも生える**。

**エージェントの「この変異で落ちる」は、当てて確かめるまで主張にすぎない。**
今回は 2 件中 1 件が誤りだった。**統合側で必ず自分で当てること。**

##### 台帳の負債(本 WP では返さない)

- **manifest-backed provider**: DLL 不在でも behavior dependency schema を
  供給できる provider。これが無い限り generated pending は存在しない
  (現在は `PrefabProviderStale` で fatal)
- **E2 が destroy を入れた後**、参照先が destroy された状態での unpack の undo は
  既存の `PrefabObjectRefUnresolved` で落ちる。真の衝突なので名前付きで落ちること自体は正しい
- **`makeEditorRuntimeService` 全体のテスト可能化**: 今回は最後の一跳ねだけ閉じた。
  現在この関数を丸ごと通すのは `golden_harness.cpp` だけである

次は **WP372c**(`ObjectReferenceIndex` + rename + CAS overlap 規範表)。

### WP372c: rename 追随 —— `ObjectReferenceIndex`・JSON Pointer 文法・overlap 規範表

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 中。**
**規範は `docs/design_editor_lifecycle.md` v4 決定 2。ただし v4 決定 3 は下記 決定 A で撤回する。**
**前提は WP372a + WP372b(統合済み、全数 1502)。**

#### 起票時の実測(2026-09-02。書く前に読んだもの)

- **rename 操作は存在しない**(`src/core/communication/` に該当なし)。本 WP が作る
- **`name_reservations` 機構は既存**。object 作成が
  `/scenes/<id>/name_reservations/<name>` を `stable_target` と `read_set` に置き、
  `object_existence` domain に `name_reservation` を載せている
  ([editorjournal.cpp:1439](../src/core/communication/editorjournal.cpp#L1439))
- **path 記法は RFC6901 JSON Pointer が既に単一の流儀**
  (`json_pointer` / `"/prefab/parameters/" + parameter`:
  [editorjournal.cpp:626](../src/core/communication/editorjournal.cpp#L626))。
  **新しい文法は要らない**
- overlap の第 1 規則は **「object 集合が交われば無条件に衝突」**
  ([editorjournal.cpp:2365](../src/core/communication/editorjournal.cpp#L2365))
- **出荷 scene の component は 6 種(transform / simplemodelview / light / camera /
  sprite_view / collider)で、すべて codec を持つ**(codec 表は
  [componentcodec.cpp:900](../src/core/loader/componentcodec.cpp#L900) の 7 種)。
  `example` は project 名、`demo_atlas` は asset 名であり component ではない
- 出荷 scene に **`behavior` component は 0 件**、prefab も 0 件

#### 決定 A: **v4 決定 3(component 列 CAS)を撤回する**

v4 は rename のために overlap を component 列粒度へ細分化するとしていた。**撤回する。**

- 第 1 規則を緩めると、**rename のためではなく全操作の安全網が緩む**。
  WP324 で「規則を 1 箇所へ集約したら engine の検証が 3 箇所で弱くなった」を踏んでいる
- 細分化が要求されたのは、WP372 v2 が置いた正例
  「同一 object の無関係な列を編集した後 rename の undo が成功する」を通すためだった。
  **その正例は仕様のために発明されたものであり、利用者の要求ではない**
- rename に必要なのは**同一 transaction 内での参照追随**であって、並行性の緩和ではない

**rename は、対象 object と参照元 object のいずれかに触る並行編集と衝突する。**
これは既存規則のままであり、本 WP は overlap を**緩めない**。

#### 決定 B: 参照の宣言面(JSON Pointer。新文法なし)

参照の出所は 2 つだけとする:

| # | 出所 | 状態 |
|---|---|---|
| 1 | prefab の `kind:"object"` パラメータ | **既存**。WP361 で解決済み、追随だけが無かった |
| 2 | schema field の `semantic: object_ref` 宣言 | **新設**。`StructFieldSchema` に `semantic` を足す |

- `StructFieldSchema` は constexpr の平坦構造体
  ([structfieldschema.hpp:46](../src/core/userpublic/details/schema/structfieldschema.hpp#L46))。
  `semantic`(`None` / `ObjectRef`)を**既定 `None` で足す**。既存宣言は無変更で通る
- **出所 2 の実利用者は利用者の behavior params** である
  (「別 object を指す behavior パラメータ」)。
  出荷 codec は当面すべて `None` —— **誰も使わない機構を作らない**ため、
  **behavior params から宣言できることを受け入れで示す**
- index の entry は **`{scene_id, object_id, json_pointer, current_name}`**。
  `json_pointer` は RFC6901。**独自記法を作らない**

#### 決定 C: 未知の参照面は名前付きで拒否する

rename の危険は「**知らない参照を黙って壊す**」ことである。

- 対象 scene に **codec を持たず schema も無い component** が 1 つでもあれば、
  参照を持たないと**証明できない**ので `rename_unknown_reference_surface@1` で拒否する
  (どの object のどの component かを payload に載せる)
- **実測: 出荷 4 project の scene にそのような component は 0 件**。
  したがってこの拒否は出荷物を壊さない
- behavior は schema を持つので拒否対象ではない

#### 決定 D: rename の wire と domain

```
forward: {"op":"rename_object", "scene_id":…, "object":…,
          "old_name":…, "new_name":…,
          "reference_updates":[{"object":…, "json_pointer":…}, …]}
inverse: 同形で old_name と new_name を入れ替えたもの(同じ reference_updates)
```

- `reference_updates` は **preparer が `ObjectReferenceIndex` から生成**し、
  journal に保存する。undo で再導出しない(WP372b 決定 7 と同じ理由: 単一権威)
- **`inverse == invert(forward)`** を journal 追加時に検証する
- `read_set` / `write_set` に
  `/scenes/<id>/name_reservations/<old_name>` と `<new_name>` の**両方**を入れる
- `structural_domain` は
  `{kind:"object_subtree", scene_id, objects:[対象 + 参照元すべて],
    name_reservations:[old_name, new_name]}`
- **新名が既存 object 名と衝突したら `object_name_taken@1`**
  (既存の name_reservation 機構がそのまま効く)

#### 受け入れ(§4 規約 10)

- **実 producer**: 実 `RpcServer::run` 経由の `rename_object` で、
  **prefab の `kind:"object"` パラメータが追随**し、resolve 後の参照が新名を指す。
  **追随を外すと `PrefabObjectRefUnresolved` で落ちる**
- **behavior params の `semantic: object_ref`** を宣言した fixture behavior でも追随する
  (宣言を `None` に戻すと追随しなくなることを同じテストで示す)
- **undo で名前と全参照が同時に戻る**。forward / inverse が同じ `reference_updates` を持ち、
  **片側だけ壊した fixture が拒否される**
- **overlap を緩めていない対照**: rename と、同一 object への無関係な
  `set_component_value` が**衝突する**(決定 A。緩めた実装では通ってしまう)
- **`rename_unknown_reference_surface@1`**: codec も schema も無い component を持つ
  scene で拒否。**出荷 4 project では拒否されない**ことを実測で示す
- **`object_name_taken@1`**: 既存名への rename が拒否される
- **WP372b の解除**: object parameter を持つ prefab の unpack は
  372b の時点で通っている。本 WP はそれを壊さない(全数で確認)
- 出荷 4 project・`src/core/resources/` の bytes 不変 /
  既存テスト(全数 1502)を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成

#### やらないこと

dry-run と型付き resolution error(WP372d)/ picker・Studio inventory・
一括流し込み(U2 本体)/ spawn・destroy の原子性(E2)/
**overlap の細分化(決定 A で撤回)**/ scene 跨ぎの参照。

依存: WP372a + WP372b。

#### WP372c 仕様レビュー第 1 巡(Reject・破綻 2)と v2

**v2 が本節の正。v1 の 決定 B・決定 C と受け入れは v2 で置換される。決定 A は維持。**

**自分の誤りを先に書く**: v1 は「参照の宣言面を新設する」と書いたが、
**`BindableSemanticKind::Object` として既に本番に存在していた**
([prefab.hpp:166](../src/project/prefab.hpp#L166))。しかも **RFC6901 path +
discriminant 付き**で、`camera /controller/target` が orbit と follow の両方に
宣言されている([prefab.cpp:782](../src/project/prefab.cpp#L782))。
**「出荷 codec は当面すべて None」は誤り**であり、実測せずに書いた。
台帳の要約から書くとこうなる、という 3 度目の実例である。

##### 実測(v2 を書く前に数えたもの)

- **`K::Object` の宣言は本番に 2 件のみ**: camera `/controller/target`(orbit / follow)
- descriptor を持つ component は 7 種すべて。ただし**網羅ではない** ——
  `simplemodelview` は `/model` しか宣言しておらず、`params` の自由 JSON は宣言外
- **出荷 scene に `parent` は 0 件**、`behavior` も prefab も 0 件
- `SceneLoader::object_bindings` は **名前 → binding の実行時 map**
  ([scene.cpp:553](../src/core/loader/scene.cpp#L553))。rename で更新が要る
- 低層の `renameObject` / `makeRenameObjectCommand` は**既にある**
  ([authoringscenedocument.cpp:325](../src/core/loader/authoringscenedocument.cpp#L325))。
  無いのは **communication 層の op** である(v1 の書き方が不正確だった)

##### 決定 B'(#1・#3): 参照源は 4 つ。宣言面は**既存のものを使う**

| # | 出所 | 権威 | 状態 |
|---|---|---|---|
| 1 | object の `/parent` | `sceneformat` の parent 解決 | **既存**。追随しないと `unknown parent` で stage finish が失敗する |
| 2 | component の `K::Object` descriptor | `BindableSemanticKind::Object` | **既存**。今日は camera target 2 件。**discriminant を評価して active な path だけ**を採る |
| 3 | prefab の `kind:"object"` パラメータ | `PrefabParameterKind::Object` | **既存**。authored override も対象 |
| 4 | behavior params の `ObjectRef` 宣言 | **新設** | 下記 |

**`StructFieldSchema` だけに `semantic` を足す v1 の案は撤回**する。利用者は
そこを直接構築せず、`field()` → `StructFieldDescriptor` → `structFields()` →
`materializeStructField()` を通る。**宣言が届かないので機構が空になる。**

出所 4 に必要な配線(全部を通すこと。どれか 1 つ欠けると宣言できない):

- 公開 constexpr 構文 **`objectRef(field<&Params::target>("target"))`**
- **`StructFieldType::String` にのみ許可**。違反は consteval / 登録時に fail-fast
- `StructFieldDescriptor` → `materializeStructField` → `StructFieldSchema` へ運ぶ
- **behavior schema fingerprint に含める**([registerer.hpp:134](../src/core/userpublic/details/behavior/registerer.hpp#L134))。
  含めないと **semantic だけの変更が hot-reload の検証をすり抜ける**
  ([behaviorreloadvalidation.cpp:24](../src/core/gamelogic/behaviorreloadvalidation.cpp#L24))
- provider descriptor が behavior field を `Value` に固定している箇所を通す
  ([prefabprovider.cpp:422](../src/core/loader/prefabprovider.cpp#L422))
- **受け入れ fixture は実 `PELICAN_REGISTER_BEHAVIOR` を使う DLL**。
  `BehaviorRegistration` を手で組む fixture は代理条件

##### 決定 C'(#4): **scene 内容による拒否を撤回する**

v1 の「codec も schema も無い component があれば拒否」は両方向に誤っていた:

- **狭すぎる**: camera は codec も schema も持つが、schema は `/controller/type` までで
  `/controller/target` を含まない。「schema がある = 参照面を知っている」は偽
- **広すぎる**: 未登録 behavior は**正当な authored pending** であり codec も schema も無い
  ([resolvedscene.cpp:160](../src/core/loader/resolvedscene.cpp#L160))。
  これを拒否すると DLL 不在では rename が永久に使えない
- そもそも `simplemodelview.params` は自由 JSON なので、descriptor があっても網羅にならない

**問い直す: 参照とは何か。** 文字列が object 名に見えることではなく、
**本番のどこかがそれを object 名として解決すること**である。解決する場所は数えられる:
`/parent`(sceneformat)/ camera controller / prefab object parameter /
利用者が宣言した behavior params ―― **これが出所 1〜4 と一致する**。

したがって拒否ではなく **構造的な網羅義務**にする:

- **object 名を名前で解決する本番の場所を 1 つの表に集め**、
  **解決側と index の両方がその表を読む**
- 表に載っていない新しい解決者を足すと**通らない**ようにする
  (WP372b-fix と同じ形: 本番と受け入れが同じ関数を呼ぶ)
- **利用者が宣言しなかった behavior の名前引きは追随しない。**
  これは欠陥ではなく契約である(宣言したものが追随する)。文書に明記する

##### 決定 E'(#2): 本番 route を受け入れに入れる

`executeEditorProjection` の op 分類に rename が無く、**未知 op はエラーにもならず
adapter 0 個で document transaction を commit する**
([editorruntimefactory.cpp:1030](../src/core/communication/editorruntimefactory.cpp#L1030))。
つまり document だけ新名になり、arena・camera・`object_bindings` は旧名のまま ——
**WP372b で出したのと同じ欠陥が再現できる。**

- **production-owned な rename route/planner を定義**し、
  **本番 executor と受け入れが同じ関数を呼ぶ**
- 参加させる adapter: **behavior(`set_params` として適用。handle と内部状態を維持、
  create/destroy/`onInit`/`onDestroy` は 0)/ camera runtime /
  `SceneLoader` の name map**。すべて同一 transaction
- 受け入れは resolved JSON だけでなく、**live arena params・camera の実 target・
  `hasObjectTransform(new)==true` かつ `hasObjectTransform(old)==false`** を検査する
- **route または各 adapter の挿入を消す変異で、同じ実 RPC テストが赤くなること**

##### 受け入れ(v1 を置換)

- **実 producer**: 実 `RpcServer::run` の `rename_object` で、
  出所 1〜4 が**すべて追随**する。各出所の追随を外す変異が**個別に**落ちる
  - `/parent`: 外すと `unknown parent` で stage finish が失敗
  - camera `/controller/target`: discriminant が orbit / follow の**両方**で追随。
    fly では対象外(宣言が無い)
  - prefab `kind:"object"`: 外すと `PrefabObjectRefUnresolved`
  - behavior `objectRef`: **実 DLL の `PELICAN_REGISTER_BEHAVIOR`** で宣言。
    宣言を外すと追随しなくなる
- **runtime まで届くこと**: live arena params・camera の実 target・
  `object_bindings` の新旧。**adapter を 1 つずつ外す変異が個別に落ちる**
- **v4 が要求した順序**: object parameter を behavior target に束縛した prefab を
  **unpack し、DLL を reload してから rename** して追随する
  ([design_editor_lifecycle.md:352](design_editor_lifecycle.md#L352))
- **fingerprint**: `objectRef` 宣言だけを変えた behavior の reload が
  **検証で弾かれる**(fingerprint に含めないと素通りする)
- **undo**: 名前と全参照が同時に戻る。forward / inverse が同じ `reference_updates` を持ち、
  **片側だけ壊した fixture が拒否される**
- **overlap を緩めていない対照**(決定 A): rename と同一 object への無関係な
  `set_component_value` が**衝突する**
- **`object_name_taken@1`**: 既存名への rename が拒否される
- **網羅義務**: 表に載せずに名前解決者を足すと通らないことを、テストで示す
- **pending behavior を壊さない**: 未登録 behavior を持つ scene でも rename が通り、
  pending flag・handle・seq が保存される(v1 の拒否は撤回済み)
- 出荷 4 project・`src/core/resources/` の bytes 不変 /
  既存テスト(全数 1502)を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成

##### やらないこと(v1 から変更なし、拒否の項目だけ削除)

dry-run と型付き resolution error(WP372d)/ picker・Studio inventory・
一括流し込み(U2 本体)/ spawn・destroy の原子性(E2)/
overlap の細分化(決定 A)/ scene 跨ぎの参照 /
**利用者が宣言しない behavior の実行時名前引きの追随**(契約として追随しない)。

#### WP372c 第 2 巡(8 件)と裁定 —— **rename は WP ではなく設計である。本 WP は中止する**

第 2 巡の 8 件を並べると、単一の事実が出る。
**object 名は、このエンジンで少なくとも 6 つのサブシステムの identity キーである。**

| subsystem | 名前が identity である証拠 |
|---|---|
| SceneLoader | `object_bindings` が名前 key。しかも RPC `load_gltf` の transient と**同じ map を共有**する([rpcserver.cpp:1439](../src/core/communication/rpcserver.cpp#L1439)) |
| Light | 名前を 3 つの map の key に持つ([lightcontainer.hpp:29](../src/core/light/lightcontainer.hpp#L29)) |
| Physics | collider identity と query hit の公開名が object 名([physworld.cpp:285](../src/core/phys/physworld.cpp#L285)) |
| Animation | `objects` map が名前 key。**古い cache を先に返す**([animationservice.cpp:432](../src/core/animation/animationservice.cpp#L432)) |
| Camera | active camera / pose が名前依存([camera.cpp:296](../src/core/renderer/camera.cpp#L296)) |
| Controller | camera 名が map key、**target 名が signature に入る**([cameracontrollersystem.cpp:346](../src/core/userpublic/cameracontrollersystem.cpp#L346)) |

そして第 2 巡 #5(**仕様が破綻**)が決定的だった。v2 の「解決者を 1 つの表に集め、
表に無い解決者を足すと通らなくする」は**実装不能**である ——
生の名前解決 API が公開されたままだから([scene.hpp:57](../src/core/loader/scene.hpp#L57))。
将来のコードは表に触れずに `GET_MODULE(SceneLoader).objectId(params.target)` と書け、
**既存テストはそれを知り得ない**。source grep の allow-list なら作れるが、
それは**この台帳が禁止している代理条件そのもの**である。

##### 裁定: **rename を E1 から外し、設計トラックへ移す**

WP361b で 2 回 revert したときと同じ形である ——
**機構の穴が塞がらないのではなく、機構の切り方が違う。**

- **rename は「編集操作」ではなく「identity の rekey」**である。
  文書と宣言済み参照だけを直しても、上表の 6 subsystem は旧名のまま残る
- 閉じるには **生の名前引きを非公開化し、opaque な参照型へ集約する**必要がある。
  これは横断リファクタであり、**WP304(安定 ID)が不合格になった領域に隣接する**
- **今日 rename は存在しない**([communication 層に op が無い](../src/core/communication/editorjournal.cpp))。
  やらないことによる後退は無い

##### 中止しても E1 と U2 は進む

- **E1 の残りは WP372d(dry-run)**。rename に依存しない
- **U2 の picker と一括流し込みは rename 追随に依存しない**。
  参照は今日どおり、壊れれば load 検証が名前付きで落ちる
- 372b が入れた `prefab_unpack_object_parameter_unsupported` は**存在しない**
  (v3 差分 8 で撤回済み)ので、unpack も止まらない

##### 設計トラックへ持ち越す論点(次に設計文書を書くときの入力)

1. **名前を identity から降ろす**か、**rekey を全 subsystem が実装する**か
2. 生の名前引き API の非公開化と、opaque 参照型 + 単一 resolver API
3. `object_bindings` が `load_gltf` transient と map を共有している件
   (rename 以前に、transient と authored の衝突が既に成立し得る)
4. 宣言済み参照面(`BindableSemanticKind::Object` / `/parent` /
   prefab `kind:"object"` / behavior params の `objectRef`)は**そのまま使える**。
   v2 の 決定 B' は設計として生きている
5. behavior params の `objectRef` 配線(field → descriptor → materialize →
   fingerprint → provider)も設計として生きている。
   ただし **`BehaviorParamsPolicy` 限定**にすること(第 2 巡 #4)
6. **省略時既定値の undo**(第 2 巡 #3): ObjectRef の default を空文字のみ許すか、
   `prior_presence` / `prior_value` を inverse に持つか
7. **publish 後 rollback できる `set_params` primitive**(第 2 巡 #7)。
   WP372a が scene reconciler 側で作ったものの、直接編集経路版
8. **単一の production executor seam**(第 2 巡 #8)。
   分類・全 adapter 構築・commit まで所有し、受け入れがそれを呼ぶ。
   これは rename に限らず **WP372b-fix の一般化**であり、単独でも価値がある

##### 決定 A は設計として維持する

overlap を緩めない判断は第 2 巡でも壊せなかった
(「衝突範囲は広いが、単独・逐次 rename は成立する」)。
**v4 決定 3(component 列 CAS)の撤回は有効**である。

##### 次

**WP372d(dry-run + 型付き resolution error)** へ進む。

### WP372d: dry-run と型付き resolution error(E1 の最後の一片)

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 中。**
**規範は `docs/design_editor_lifecycle.md` v4 の「dry-run」受け入れと、
台帳の U2 裁定「dry-run は `ResolvedScene` の純 resolver 経路をそのまま使う。
新しい検証経路を作らない —— 第二の流儀の禁止」。**
**前提は WP372a + WP372b + WP372b-fix(統合済み、全数 1502)。WP372c は設計へ移送済み。**

#### 起票時の実測(2026-09-02)

- **dry-run の RPC は存在しない**(`src/core/communication/` に該当なし)
- `ResolvedComponent::runtime_resolution_error` は **`std::optional<std::string>`**
  ([resolvedscene.hpp:212](../src/core/loader/resolvedscene.hpp#L212))
- 設定箇所は 2 つ。どちらも **`catch (const std::exception &error)` で
  `error.what()` に潰している**
  ([resolvedscene.cpp:191](../src/core/loader/resolvedscene.cpp#L191) = behavior params、
  [resolvedscene.cpp:308](../src/core/loader/resolvedscene.cpp#L308) = codec decode)
- 再送出は **`std::runtime_error`**([resolvedscene.cpp:46](../src/core/loader/resolvedscene.cpp#L46))と、
  文字列連結([behaviorarena.cpp:164](../src/core/gamelogic/behaviorarena.cpp#L164))。
  **どちらも型を失う**
- :302 のコメントは「Runtime binders consume this **named** result」と書いているが、
  **実際には named ではなく文字列**である。コメントが仕様に先行している
- 既存の編集失敗 payload は `{code, payload, message}`
  ([editorjournal.cpp:3192](../src/core/communication/editorjournal.cpp#L3192))。
  **dry-run はこれと同じ形を返す**(新しい形を作らない)

#### 1. 型付き resolution error

`runtime_resolution_error` を **`std::optional<ResolvedComponentError>`** にする:

```
ResolvedComponentError { code, message, json_pointer?(optional), component_name }
```

- **`message` は今日の `error.what()` と byte 一致**させる。
  既存の文言に依存する経路を動かさない
- 分類は **2 つの catch site で明示的に行う**。既知の typed 族
  (schema / struct field / prefab / resolved scene / codec)を**個別に catch** して code を付け、
  **`catch (const std::exception &)` は `resolution_unclassified@1`** に落とす。
  第三者 JSON ライブラリの `std::runtime_error` が来ても壊れない
- **`requireRuntimeValue()` と behaviorarena は typed error を投げ直す**。
  文字列連結をやめる(連結すると呼び出し側が再び parse する羽目になる)
- **inactive scene のエラー保持契約は変えない**
  (authoring/query/save は無効な component を保持でき、binder が投げる)

#### 2. dry-run(**新しい検証経路を作らない**)

- RPC `dry_run_edit` は **`edit` と同一の params** を取る
- **同じ `executeEditorProjection` の分類**と**同じ
  `EditorProjectionTransaction` の prepare** を通し、**publish の代わりに rollback** する
- 返すのは既存の `{code, payload, message}` と、
  **適用したら発生する `ResolvedComponentError` の一覧**
- **dry-run 専用の検証関数を書かないこと。** 書いた時点で本 WP は失敗である

#### 受け入れ(§4 規約 10)

- **同一 code**: 失敗する編集を `dry_run_edit` と `edit` の両方へ流し、
  **code と payload が完全一致**する。族ごとに 1 件ずつ(schema / prefab /
  codec decode / behavior params / unclassified)
- **dry-run は何も動かさない**: scene revision・journal・ECS・arena・
  `next_handle` / `next_attachment_seq` が**呼び出し前後で不変**。
  **成功する編集でも不変**(rollback が効いていることの証明)
- **第二経路の否定**: dry-run が `EditorProjectionTransaction::prepare` を
  通らない実装にする変異で落ちること。
  **本番の分類・prepare を消す変異で dry-run も同時に落ちる**こと
  (= 同じ経路を通っている証拠。WP372b-fix と同じ形)
- **型が届く**: `requireRuntimeValue()` が投げた error から **code が読める**
  (今日は文字列なので読めない)。文字列連結に戻す変異で落ちる
- **`message` の byte 一致**: 既存の error 文言を使うテストが**無変更で通る**
- **inactive scene**: 無効な component を持つ inactive scene が
  **保存・query できる**(現行契約)。current へ切り替えると**名前付きで**落ちる
- 出荷 4 project・`src/core/resources/` の bytes 不変 /
  既存テスト(全数 1502)を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成

#### やらないこと

rename(設計トラックへ移送済み)/ picker・Studio inventory・一括流し込み(U2 本体)/
spawn・destroy の原子性(E2)/ **単一 production executor seam の一般化**
(WP372c 裁定の持ち越し 8。単独で価値があるが本 WP の範囲外)/
`ResolvedSceneError` 自体の再設計。

依存: WP372a + WP372b + WP372b-fix。

#### WP372d 仕様レビュー第 1 巡(却下・破綻 3)と v2

**v2 が本節の正。v1 の §1・§2 と受け入れは v2 で置換される。**
**第 1 巡は本 WP を縮めた。** 分類すべき族は実質 1 つ、dry-run は resolver で止まる。

**自分の誤りを先に書く**: v1 は dry-run を
`EditorProjectionTransaction` に通し「publish の代わりに rollback」するとしたが、
**規範は「純 `ResolvedScene` resolver 経路」と書いてある**
([design_editor_lifecycle.md:99](design_editor_lifecycle.md#L99))。
**自分で引用した規範を、自分の仕様で破っていた。**

##### 決定 1(#3・破綻): dry-run は **resolver で止まる**。adapter prepare を呼ばない

v1 の形は成立しない:

- `EditorProjectionTransaction` に **`prepare()` API は無く `commit()` だけ**。
  ECS archetype adapter は **prepare walk の途中で publish される**
  ([editorprojectiontransaction.cpp:909](../src/core/loader/editorprojectiontransaction.cpp#L909))
- adapter prepare は**戻せない副作用**を起こす —— renderer prepare が
  **GPU asset を cache へ commit**([model.cpp:413](../src/core/asset/model.cpp#L413))、
  live vector の capacity 増加と **skeletal の GPU skin buffer 書き込み**
  ([polygoninstancecontainer.cpp:502](../src/core/renderer/polygoninstancecontainer.cpp#L502))。
  rollback は staged entry を捨てるだけ
- `add_component behavior` は prepare 中に **利用者の constructor を実行**する
  ([behaviorarena.cpp:1140](../src/core/gamelogic/behaviorarena.cpp#L1140))
- **出荷側にも `simplemodelview` と skeletal model がある**ので、架空の話ではない

**決定**: dry-run は **共有の candidate builder + `ResolvedSceneResolver::prepare`** までで停止する。
**adapter prepare を一切呼ばない。** runtime resource の preflight が要ると分かったら、
そのときに**副作用禁止の `validate` 契約**を別途設計する(本 WP では作らない)。

##### 決定 2(#4): endpoint を **`set/unset_prefab_parameter` の batch に限定**する

「`edit` と同一 params」は過大契約だった。実 edit の前半には
session / gate / revision / prefab CAS / preview lease / behavior identity 正規化があり、
**tail だけ通る dry-run は preview lease 衝突で実 edit と食い違う**
([editorjournal.cpp:3255](../src/core/communication/editorjournal.cpp#L3255))。
前半を通せば ticket と identity counter が動き、no-op 条件が壊れる
([behaviorarena.cpp:1056](../src/core/gamelogic/behaviorarena.cpp#L1056))。

- **`dry_run_edit` は `set_prefab_parameter` / `unset_prefab_parameter` のみ受理**する。
  他の op は `dry_run_unsupported_operation@1` で名前付き拒否
- **read-only の request preparation** を共有する: session / gate / revision /
  prefab closure・provider CAS / **preview lease 検査**。
  **identity は予約も加算もしない**(prefab parameter は identity を作らない)
- spawn / destroy / add / remove は E2 の範囲であり、本 WP では受理しない

##### 決定 3(#5・破綻): 分類表は**実際に届く族だけ**。他は再送出(fail-fast の回復)

第 1 巡が 2 つの catch site へ実際に届く族を数えた。**`StructFieldValidationError`
だけである。** `PrefabError` は `preparePrefabParameter` で先に変換され、
`ResolvedSceneError` は :301 で明示再送出、codec 固有の例外型は**存在しない**。

**現状は fail-fast 違反である**: `catch (const std::exception &)` が
**`std::logic_error`(内部 invariant。例: resolved camera に projection 値が無い
[resolvedscene.cpp:281](../src/core/loader/resolvedscene.cpp#L281))と `std::bad_alloc`
まで component error として保持している。**

**規範表(exact exception class × phase × 動作)**:

| 例外 | 動作 | 理由 |
|---|---|---|
| `StructFieldValidationError` | **component error 化**(code + `json_pointer`) | 利用者の宣言が悪い。保持して報告する |
| nlohmann の明示的な data error | **component error 化** | 同上 |
| `ResolvedSceneError` | **再送出**(現行どおり) | 既に名前付き |
| `PrefabError` | **再送出** | 上流で変換済み。ここへは届かない |
| `std::logic_error` | **再送出**(**現行を変える**) | 内部 invariant の破れ。握り潰さない |
| `std::bad_alloc` | **再送出**(**現行を変える**) | fail-fast |
| その他 `std::exception` | **再送出**(**現行を変える**) | 分類できないものを保持しない |

**`resolution_unclassified` は作らない**(v1 の案は撤回)。
分類できない例外を component error にすると、内部バグが利用者の入力ミスに見える。

受け入れ fixture は **resolver site へ本当に到達する入力**に限る
(generated component 等)。直接編集は resolver 前の canonicalizer が
`schema_violation` へ変換してしまうので、族の対照にならない
([editorjournal.cpp:212](../src/core/communication/editorjournal.cpp#L212))。

##### 決定 4(#6・破綻 / #7): 型に provenance を持たせ、leaf と提示を分ける

v1 の `{code, message, json_pointer?, component_name}` では足りない。

```
ResolvedComponentError {
  code, message(leaf。catch した what() と byte 一致),
  json_pointer?,
  scene_id, authoring_object_id, component_index, attachment_index?
}
```

- resolver は既に `scene_id / object_index / component_index` の provenance を受け取っている
  ([resolvedscene.hpp:39](../src/core/loader/resolvedscene.hpp#L39))。**それを載せる**
- **`what()` の互換対象は現行の文脈付き文言**
  (`"behavior_resolution_error on object 'X': "` 接頭辞。既存テストが要求する:
  [wp360_resolvedscene_test.cpp:476](../test/wp360_resolvedscene_test.cpp#L476))。
  **leaf `message` と `what()` は別物**であり、**文脈は型から決定的に組み立てる**
  (文字列連結をやめるのではなく、**連結の材料を型に持たせる**)
- **一覧の規範順**: `(scene_id, authoring_object_id, component_index, attachment_index)`
- **base / candidate の差分規則**: dry-run が返すのは
  **candidate で新規に発生した error**。base に既にある inactive の error は
  **`preexisting: true` として別枠**で返す(消さない・混ぜない)
- **primary の選択**: 規範順の先頭。実 edit の最初の adapter failure と一致させる

##### 決定 5(#1): 型付き code を **terminal RPC まで**届かせる

transaction は `EditorProjectionException` と `PrefabError` しか保存せず、
他は `AdapterPrepareFailed` になり、edit 層が **`method_unavailable` へ潰す**
([editorjournal.cpp:1706](../src/core/communication/editorjournal.cpp#L1706))。
**v1 の「dry-run と edit で code が一致」は、両方 `method_unavailable` でも緑になる。**

- `EditorProjectionError` に **resolution error の typed variant を追加**し、
  `errorFromException` → `projectionFailure` → **`get_edit_result` の terminal error**
  まで保存する
- **受け入れは「一致」ではなく「期待する具体的 code が terminal に出る」**こと
- `load_scene` も同じ穴を持つ([rpcserver.cpp:1095](../src/core/communication/rpcserver.cpp#L1095))。
  **inactive → current 切替の名前付き失敗を RPC 契約にする**なら、この mapping も本 WP に含める

##### 決定 6(#2・#8): 受け入れは production composition を通る。写経を禁止する

- **テストの `EditorEditRuntimeDependencies.execute` 注入を使わない。**
  `makeEditorRuntimeService()` + `configureEditorRpcHandlers` + 直列化した
  `RpcServer` を使う(single production executor seam は WP372c 裁定の持ち越しであり、
  本 WP では**作らずに、production composition をそのまま呼ぶ**)
- **比較点は terminal**: `edit → accepted → production frame commit → get_edit_result`
  の `result.error` と、`dry_run_edit` の結果を比較する。**helper の戻り値同士を比較しない**
- wire schema を固定する: 成功 / domain reject / invalid params の各形、
  dry-run が **ticket を確保しない**こと、error 一覧の置き場所

##### 受け入れ(v1 を置換)

- **正側(規約 10。v1 に欠けていた)**: 同じテスト内で、dry-run 後に
  **実解決値が不変**であることを確認し、続けて同じ編集を**本適用**して
  **実際に解決された runtime 値が変わる**ことを確認する
- **terminal code**: `StructFieldValidationError` を起こす prefab parameter で、
  **dry-run と `get_edit_result` の両方に期待する具体的 code が出る**。
  `method_unavailable` に潰れたら不合格
- **fail-fast の回復**: `std::logic_error` / `std::bad_alloc` を起こす対照で、
  **component error として保持されず伝播する**。現行実装ではここが落ちる(= 現行がバグ)
- **副作用ゼロ**: dry-run 前後で scene revision・journal・ECS・arena・
  `next_handle` / `next_attachment_seq`・**asset cache の要素数**・
  **polygon instance container の capacity** が不変
- **adapter を呼んでいない証拠**: dry-run 中に renderer adapter の prepare が
  **一度も呼ばれない**(counter で観測)。呼ぶ実装にする変異で落ちる
- **第二経路の否定**: dry-run 専用の検証関数を足す変異、または
  `ResolvedSceneResolver::prepare` を通らない実装で落ちる
- **provenance**: 同一 object に同型・同文言の behavior attachment が 2 つある構成で、
  一覧から**どちらか識別できる**
- **base / candidate**: 無効な inactive component が base にある状態で
  無関係な編集を dry-run し、既存 error が **`preexisting: true` として分離**される
- **`what()` の互換**: 既存の文脈付き文言テストが**無変更で通る**
- **`dry_run_unsupported_operation@1`**: `spawn` を含む request が名前付き拒否される
- 出荷 4 project・`src/core/resources/` の bytes 不変 /
  既存テスト(全数 1502)を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成

##### やらないこと(v1 から変更)

rename(設計)/ picker・Studio inventory・一括流し込み(U2 本体)/
spawn・destroy(E2)/ **adapter の副作用禁止 `validate` 契約**(決定 1)/
**single production executor seam**(WP372c 裁定の持ち越し 8)/
`ResolvedSceneError` 自体の再設計。

#### WP372d 第 2 巡(8 件・破綻 3)と裁定 —— **順序が違っていた。3 つに分ける**

第 2 巡は v2 の決定を 3 つ潰した。並べると、**dry-run が最後に来るべきもの**だと分かる。

##### 潰れた決定と、その理由

**決定 3 は事実誤認だった**(#2)。「利用者エラーは `StructFieldValidationError` だけ」は誤りで、
resolver の catch 範囲に**手書きの `std::runtime_error` が 4 箇所**ある ——
`yfov` 欠落([resolvedscene.cpp:230](../src/core/loader/resolvedscene.cpp#L230))/
`xmag`(:248)/ `ymag`(:256)/ **`zfar <= znear`**(:285)。
これらは**利用者入力で起きる**。v2 の「その他は再送出」を実装すると、
**inactive scene の保持契約を実利用者エラーで壊す**。
既存テストは `StructFieldValidationError` の 1 例しか見ていないので検出しない。

**決定 3 の fail-fast 半分は本番組成から観測できない**(#3)。
`EditorProjectionTransaction::commit` は **`noexcept` で全 `std::exception` を変換**し
([editorprojectiontransaction.hpp:291](../src/core/loader/editorprojectiontransaction.hpp#L291))、
coordinator と RPC がさらに汎用エラーへ潰す。
resolver 直叩きなら緑にできるが、**production composition では同じ観測にならない**。

**決定 1 と決定 4 が互いに矛盾する**(#1)。resolver で止めた dry-run は
**adapter 段でしか出ない失敗を予測できない**のに、
決定 4 は「primary は実 edit の最初の adapter failure と一致」と書いていた。
しかも反例が悪い: `model="__missing__"` と `znear=5000` を同じ batch に入れると、
**dry-run は camera の失敗だけを返し、実 edit は renderer が先に落ちる**。
`model` を「有効だが未 cache」にすると、**renderer が GPU asset を cache へ commit した後で
camera が失敗する** —— **失敗した編集が不可逆な副作用を残す**。

**そして dry-run には今日の消費者がいない**(#8)。
出荷 4 project に prefab は 0 件、Studio にも経路が無い。

##### 裁定: 3 つに分け、dry-run を最後にする

| WP | 内容 | 単独で何が良くなるか |
|---|---|---|
| **WP373** | resolver の手書き検証を**型付き利用者エラー**にし、分類表と provenance を入れる | 今日 `zfar <= znear` は**型無しの文字列**でしか出ない。名前と `json_pointer` が付く |
| **WP374** | **resolve 直後・adapter loop 前の resolution gate** と、typed variant を terminal まで運ぶ | 今日は **失敗する編集が renderer prepare を走らせ、GPU asset を cache へ commit してから落ちる**。gate があれば adapter に触れずに落ちる |
| **dry-run** | **U2 の一括流し込み面と同時に**作る | **U2 が消費者である**。今日作っても呼ぶ者がいない |

**dry-run を U2 へ移すのは規範に反しない。** 台帳の U2 裁定は
「dry-run は純 resolver 経路をそのまま使う。新しい検証経路を作らない」と書いており、
**どの WP で作るかは指定していない**。むしろ U2 の一括流し込みこそが
「N 件流し込んで 1 件だけ型不正」を必要とする面である。

##### WP373 / WP374 へ持ち越す決定(v2 から生きているもの)

- **決定 4 の型**: `ResolvedComponentError { code, message(leaf), json_pointer?,
  scene_id, authoring_object_id, component_index, attachment_index? }`。
  provenance は resolver が既に受け取っている
  ([resolvedscene.hpp:39](../src/core/loader/resolvedscene.hpp#L39))
- **`what()` の互換対象は現行の文脈付き文言**。leaf `message` と分ける。
  **連結をやめるのではなく、連結の材料を型に持たせる**
- **規範順**: `(scene_id, authoring_object_id, component_index, attachment_index)`
- **決定 6**: 受け入れは production composition を通し、
  テストの `.execute` 注入を使わない
- **決定 2 の endpoint 限定**は dry-run と一緒に U2 へ持ち越す

##### 台帳の負債

- **下流の平坦化**: `commit` が `noexcept` で全例外を変換し、coordinator と RPC が
  さらに潰すため、**内部 invariant の破れが本番で観測できない**。
  WP374 の typed variant はこれを resolution error について直すが、
  **一般の例外については開いたまま**である
- **`logic_error` / `bad_alloc` の握り潰し**: WP373 で再送出に変えるが、
  **対照は resolver seam に置く**。production composition では上記の平坦化に隠れる
- **dry-run の消費者**: U2 の一括流し込み面が入るまで存在しない

##### 次

**WP373** を起票する。


### WP373: resolver の失敗に名前と場所を与える

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 中。**
**規範は WP372d 第 2 巡の裁定(3 分割の 1 本目)。**
**前提は WP372a + WP372b + WP372b-fix(統合済み、全数 1502)。**

#### 何が壊れているか(利用者に見える形)

利用者が camera の値を間違えたとき、**返るのは型無しの文字列**である。
`zfar <= znear` も `yfov` 欠落も、`what()` の文面でしか区別できない。
`json_pointer` も、どの scene のどの object かも付かない。

#### 起票時の実測(2026-09-02)

- resolver の catch 範囲に**手書きの `std::runtime_error` が 4 箇所**。
  すべて**利用者入力で起きる**:
  `camera.yfov` 欠落([resolvedscene.cpp:230](../src/core/loader/resolvedscene.cpp#L230))/
  `camera.xmag`(:248)/ `camera.ymag`(:256)/
  **`camera.zfar <= 解決後の znear`**(:285)
- 同じブロックに **`std::logic_error`(内部 invariant)**が同居する(:281)
- `runtime_resolution_error` の設定は **2 箇所ちょうど**
  (:191 behavior params / :308 codec decode)。どちらも
  **`catch (const std::exception &)` で `error.what()` に潰している**
- **`std::logic_error` と `std::bad_alloc` も component error として保持されている** ——
  **内部バグが利用者の入力ミスに見える。fail-fast 違反**
- 再送出は `std::runtime_error`(:46)と文字列連結
  ([behaviorarena.cpp:164](../src/core/gamelogic/behaviorarena.cpp#L164))。型が失われる
- resolver は provenance(`scene_id` / `object_index` / `component_index`)を
  **既に受け取っている**([resolvedscene.hpp:39](../src/core/loader/resolvedscene.hpp#L39))が、
  error には載せていない

#### 1. 手書き検証を型付き利用者エラーにする

4 箇所を **`code` と `json_pointer` を持つ明示的な利用者データ例外**に変える:

| 現行の文言 | code | `json_pointer` |
|---|---|---|
| `camera.yfov is required when changing projection to perspective` | `camera_yfov_required@1` | `/yfov` |
| `camera.xmag is required when changing projection to orthographic` | `camera_xmag_required@1` | `/xmag` |
| `camera.ymag is required when changing projection to orthographic` | `camera_ymag_required@1` | `/ymag` |
| `camera.zfar must be greater than the resolved znear` | `camera_zfar_not_greater_than_znear@1` | `/zfar` |

**文言は変えない**(既存テストが文面を見ている)。

#### 2. 分類表(exact exception class × 動作)

| 例外 | 動作 |
|---|---|
| `StructFieldValidationError` | **component error 化** |
| 本 WP が導入する利用者データ例外(上表) | **component error 化** |
| nlohmann の明示的な data error | **component error 化** |
| `ResolvedSceneError` | 再送出(現行どおり) |
| `std::logic_error` | **再送出**(現行を変える) |
| `std::bad_alloc` | **再送出**(現行を変える) |
| その他 `std::exception` | **再送出**(現行を変える) |

**`resolution_unclassified` は作らない。** 分類できない例外を component error にすると、
内部バグを利用者の入力ミスとして制度化することになる。

#### 3. 型と provenance

```
ResolvedComponentError {
  code, message,          // message は catch した what() と byte 一致(leaf)
  json_pointer?,
  scene_id, authoring_object_id, component_index, attachment_index?
}
```

- `runtime_resolution_error` を `std::optional<ResolvedComponentError>` にする
- **`what()` の互換対象は現行の文脈付き文言**
  (`"behavior_resolution_error on object 'X': "` 接頭辞。
  [wp360_resolvedscene_test.cpp:476](../test/wp360_resolvedscene_test.cpp#L476) が要求)。
  **連結をやめるのではなく、連結の材料を型に持たせ、文脈を決定的に組み立てる**
- `requireRuntimeValue()` と behaviorarena は **typed error を投げ直す**
- 一覧の規範順は
  `(scene_id, authoring_object_id, component_index, attachment_index)`

#### 受け入れ(§4 規約 10)

- **4 つの camera エラーが本番経路で名前付きになる**。
  第 2 巡が挙げた実入力を使う: **project default で `zfar=1000`、generated camera が
  `zfar` を省略、prefab parameter で `znear=5000`** ——
  codec は両値が authored のときしか比較しないので通過し、
  **resolver が default を補完した後に落ちる**。
  `code` と `json_pointer` が付くこと。**型付けを外すと落ちる**
- **inactive scene の保持契約が 4 つすべてで保たれる**:
  無効な camera を持つ inactive scene が**保存・query できる**、
  current へ切り替えると**名前付きで**落ちる。
  **v2 の「その他は再送出」のまま実装すると、この対照が落ちる**(そのための対照)
- **`logic_error` / `bad_alloc` の再送出**: 対照は **resolver seam** に置く。
  **これは本番経路の対照ではない** —— `commit` が `noexcept` で全例外を変換し
  ([editorprojectiontransaction.hpp:291](../src/core/loader/editorprojectiontransaction.hpp#L291))、
  下流がさらに潰すため。**その平坦化は WP374 と台帳の負債**であり、本 WP では直さない。
  **この 1 項目だけが seam 対照であることを明記する**
- **provenance**: 同一 object に**同型・同文言の behavior attachment が 2 つ**ある構成で、
  一覧から**どちらか識別できる**。provenance を落とすと識別できなくなる
- **`what()` の互換**: 既存の文脈付き文言テストが**無変更で通る**
- **規範順**: 複数エラーが**決定的な順**で並ぶ。順序規則を外すと落ちる
- 出荷 4 project・`src/core/resources/` の bytes 不変 /
  既存テスト(全数 1502)を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成

#### やらないこと

**resolve 直後の resolution gate と typed variant の terminal 伝搬(WP374)**/
dry-run(U2 と同時)/ rename(設計)/ spawn・destroy(E2)/
`commit` の `noexcept` 平坦化そのもの(台帳の負債)/
`ResolvedSceneError` の再設計 / camera 以外の component の手書き検証の棚卸し。

依存: WP372a + WP372b + WP372b-fix。


#### WP373 第 1 巡(8 件・破綻 2)と裁定 —— **中止。これは codec 契約の設計であり、WP ではない**

**私は try の字面を数えて、動的な到達範囲を数えていなかった。**

try は 4 つの手書き投出点だけでなく、**`codec->decodeAuthored()` と
`encodeCanonical()` の呼び出し全体を覆っている**
([resolvedscene.cpp:207](../src/core/loader/resolvedscene.cpp#L207))。
実際に届く利用者エラーは:

| 出所 | 投出点 |
|---|---|
| camera の alias | [componentcodec.cpp:512](../src/core/loader/componentcodec.cpp#L512) |
| animation の `graph` | [componentcodec.cpp:827](../src/core/loader/componentcodec.cpp#L827) |
| **collider** | [collider.cpp:13](../src/core/userpublic/components/collider.cpp#L13) ほか **17 箇所** |
| **sprite_view** | [spriteview.cpp:16](../src/core/userpublic/components/spriteview.cpp#L16) ほか **15 箇所** |
| sprite asset の参照検証 | [atlasasset.cpp:138](../src/core/asset/atlasasset.cpp#L138) |

WP373 の分類表どおり「その他 `std::exception` は再送出」を実装すると、
**collider の `radius:-1` や sprite_view の不正 texture で
inactive scene の保存・query が不可能になる** —— 現行契約の破壊である。
そして仕様は「camera 以外の棚卸し」を**明示的に範囲外にしていた**。

##### もう 1 つの破綻: 利用者に見えない(#2)

`ResolvedComponent` に code と pointer を入れても、**利用者からは見えない**:

- scene query の component serialization は `runtime_resolution_error` を**一切出さない**
  ([editorcommandservice.cpp:635](../src/core/communication/editorcommandservice.cpp#L635))
- edit は `commit() noexcept` で平坦化し、terminal が `method_unavailable` へ潰す
- **本 WP 自身が terminal 伝搬を WP374 へ送っていた**

したがって受け入れは **resolver 直叩きにしかならない**。
既存の generated-camera テストが実際にそうである
([wp361_prefab_test.cpp:929](../test/wp361_prefab_test.cpp#L929))。
「利用者に見える形」と書きながら、見える場所が無い。

##### 裁定: **中止し、設計トラックへ移す**

WP372c(rename)と同じ形である —— **機構の穴ではなく、切り方が違う。**

真の形は **「built-in codec の利用者入力拒否を、型付き data-error 契約へ統一する」**
であり、5 component・**約 35 の投出点**に跨る。さらに「見える場所」を作るには
scene query の serialization か terminal 伝搬のどちらかが要る。
**これは codec 契約の設計であって、1 WP ではない。**

##### 設計トラックへ持ち越す論点

1. **typed data-error 契約**: `code` + `json_pointer` を持つ利用者データ例外を
   codec 境界の規範にし、camera alias / animation / collider(17)/ sprite_view(15)/
   atlas を移す。**移し終わるまで「その他を再送出」は入れられない**
2. **例外クラスだけでは data error と内部バグを区別できない**(#3)。
   `std::runtime_error` は両方に使われている
3. **見える場所**: scene query の serialization に出すか、terminal まで運ぶか
4. **`what()` の byte 互換**: 提案した型だけでは現行文言を再構築できない(#5)
5. **一覧が存在しない**(#6): provenance と規範順の受け入れ対象が、そもそも無い
6. **`logic_error` / `bad_alloc`**: 再送出は正しいが、
   本番組成では下流の平坦化に隠れる。**平坦化を直すのが先**
7. **R5 との関係**: collider の既定値が 5 箇所にある負債は同じ領域である

##### この裁定でエディタトラックは一区切りとする

- **E1 の目的(U2 を塞いでいるものを外す)は WP372a + WP372b で達成済み**
- rename(WP372c)・dry-run(WP372d)・型付きエラー(WP373)は
  **いずれも U2 を塞いでいない**
- 3 本続けて「WP ではなく設計」と判定した。**これは連鎖の打ち切り条件である**
  (skill §I: 「WP が WP を直していて、それがまた WP を直しているなら、
  起票する前に元を revert して一度でやり直すことを検討する」)

##### 次

**RT3(AS 宣言面)** へ移る。利用者が明示した要求
「RT 周りを DCC ツールと同程度に好き勝手したい」の本体であり、
エディタトラックとは独立している。


### WP374: RT3-1 —— AS 宣言面(mask・instance flags・custom index・geometry flags)

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 大。**
**規範は `docs/design_raytracing.md` の §11(v4 = 11-9 が最終)。**
**前提は RT1a/RT1b/RT2a/RT2b(統合済み、全数 1502)。**

#### 何が無いか

**利用者は AS の flags も mask も custom index も宣言できない。**
本番の入力は**テスト用 override だけ**である
([accelerationstructure.cpp:894](../src/core/renderer/accelerationstructure.cpp#L894)・:748)。
`mask` は `0xffu` 固定(:903)、`instanceCustomIndex` は **`q` を直接入れている**(:497)——
これは §3 の「**`q` を shader に公開しない**」に**違反している**。

#### 1. 宣言面(粒度ごとに担体を分ける。設計 決定 14)

| 値 | 粒度 | 担体 |
|---|---|---|
| `mask`(`U8`)/ `instance_flags`(enum 集合)/ `custom_index`(`U32`、24 bit) | **instance** | **固定 leaf の `ray_instance` component**(`components[]` 内) |
| `geometry_flags` | **geometry** | **layout の material 鍵規則** |

**優先順位: `defaults < material rule < instance override`。**

layout(version 2):

```json
{ "schema": "pelican.ray_group_layout", "version": 2,
  "ray_types": ["primary", "shadow"],
  "groups": [ {"name": "opaque"} ],
  "acceleration_structure": {
    "defaults": { "mask": 255, "instance_flags": [], "geometry_flags": [] },
    "rules": [
      { "material": {"document": "materials/multi.json", "material_name": "Oak"},
        "geometry_flags": [] },
      { "material": {"asset": "models/tree.glb", "source_material_index": 7},
        "geometry_flags": [] } ] } }
```

- selector は **typed union**(設計 決定 15)。実 selector が既にこの 2 分岐である
  ([raygrouplayout.hpp:93](../src/core/shader/raygrouplayout.hpp#L93))。
  必要な 4 フィールドは **AS snapshot に既にある**
  ([accelerationstructure.hpp:32](../src/core/vkcore/accelerationstructure.hpp#L32))
- **同一 exact key の重複規則は拒否**(`ray_as_duplicate_rule@1`)。first-match にしない
- **legacy direct load は authored reference が空になり得る**
  ([gltf.cpp:2433](../src/core/model/gltf.cpp#L2433))。その row に規則が当たったら
  **`ray_as_row_material_identity_unavailable@1`**。黙って defaults に落とさない

`ray_instance` component は **`add_component` で足せる**(core に既存:
[editorjournal.cpp:1135](../src/core/communication/editorjournal.cpp#L1135))。
**Studio の UI は本 WP の範囲外**(台帳)。

#### 2. semantic mode を plan まで運ぶ(設計 決定 16)

`RayGroupLayout` は version を保持せず、TLAS lowering は layout version も AS mode も
受け取らない([accelerationstructure.cpp:443](../src/core/renderer/accelerationstructure.cpp#L443))。

**`LegacyNoLayout` / `LayoutV1Q` / `LayoutV2` を型として保持**し、
**immutable な row / TLAS plan まで運ぶ**。

| layout | `acceleration_structure` | `instanceCustomIndex` | mask / flags |
|---|---|---|---|
| 無し | — | **legacy ABI(`0,1,…`)** | 現行のまま |
| **v1** | — | **`q` のまま** | 現行のまま |
| **v1** | 有り | — | **`ray_as_declaration_requires_layout_v2@1`** |
| **v2** | 無し | **0** | `defaults` 相当 |
| **v2** | 有り | 解決値(既定 0) | 解決値 |

**自動移行はしない**(§3 の恒久規則)。既存の `custom_index == q` を固定するテスト
(RT1b 1 件・headless 5 件)は **v1 のテストとして残す**。

**`ray_as_declaration_requires_layout@1` は作らない** ——
`acceleration_structure` は layout 内の field なので、その構文は**存在しない**。

wire を固定する: `mask` 8 bit / `custom_index` **24 bit** / flag 語彙 /
未知 field の扱い / `defaults` の必須性。

#### 3. BLAS variant(設計 決定 17)

- cache 鍵 = **`(geometry_allocation_id, 解決済み geometry_flags)`**。
  現行は allocation id だけで、reuse 判定が flags を見ない
  ([accelerationstructure.cpp:744](../src/core/renderer/accelerationstructure.cpp#L744))ため、
  **flags だけの reload が古い BLAS を再利用して宣言が無効になる**
- variant は **AS-only**。`q` 採番・SBT・material 表を動かさない
- **purge は「その frame で実際に参照された variant 集合」との差分**で行う。
  「geometry が消えたら全部落とす」だけでは、規則の reload で
  **参照されなくなった variant が残る**
- **SBT digest と AS digest を分離**する
- 診断に **variant 数**を出す(`tlas_build_count` と同じ場所)
- **速度は制約ではない**(利用者の要求)。variant 増加を named reject や
  保守的統合で避けない

#### 受け入れ(§4 規約 10。設計 決定 18)

**同一テスト内で off/on・実効値・GPU 効果・purge を見る。**

- **mask(椅子 2 脚)**: **同じ material の個体 2 つ**で、片方の
  `ray_instance.mask` だけを変えると **GPU 出力が変わる**。
  component を消すと**両方写る**
- **instance_flags**: 片方だけ `ForceOpaque` にすると **any-hit の呼び出し回数が減る**。
  profile active なら `ray_profile_opaque_as@1` で拒否される
- **custom_index**: 宣言値が `gl_InstanceCustomIndexEXT` に出る。
  **row を追加して `q` をずらしても宣言値は不変**。
  **SBT sentinel は独立に正しい**(同一 GPU テストで両方)
- **geometry_flags**: 同一 allocation を opaque/mask で共有 → **BLAS build 数が 2**。
  **規則の reload で参照されなくなった variant が purge される**(残ると落ちる)
- **優先順位**: `defaults < material rule < instance override` を **3 段重ね**で検査
- **version gate**: v1 に `acceleration_structure` を書くと
  `ray_as_declaration_requires_layout_v2@1`。**v1 の `custom_index == q` は不変**
- **拒否 3 種**: legacy provenance の row / 重複規則 / 未解決 material
- **legacy**: layout 無し project の **source / SBT / TLAS instance の bytes が三つとも不変**
- 既存テスト(全数 1502)を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成 /
  GPU は統合側で直列

#### やらないこと

**per-geometry × per-instance の override**(可変長宣言が要る。台帳の担体プログラム)/
**Studio の `ray_instance` 追加・編集 UI**(台帳)/ 予約 namespace(台帳)/
static AABB と `.rint`(RT3-2)/ RT4 以降。

依存: RT1a + RT1b + RT2a + RT2b。


#### WP374 仕様レビュー第 1 巡(12 件・破綻 4)と v2

**v2 が本節の正。v1 の §1〜§3 と受け入れは v2 で置換される。**

##### 決定 1(#1・破綻): `ray_instance` は layout の外にある

v1 は「`acceleration_structure` は layout 内の field だから
`ray_as_declaration_requires_layout@1` は到達不能」と書いたが、
**`ray_instance` は独立した `components[]` 要素**であり、layout が無くても書ける。
**無視すれば fail-fast 違反、適用すれば legacy/v1 ABI を変える。**

状態表に **「`ray_instance` の有無」の軸を足す**:

| layout | `acceleration_structure` | `ray_instance` | 結果 |
|---|---|---|---|
| 無し | — | 無し | legacy ABI(`0,1,…`)。現行のまま |
| 無し | — | **有り** | **`ray_instance_requires_layout_v2@1`** |
| v1 | — | 無し | **`q` のまま**。現行のまま |
| v1 | 有り | — | `ray_as_declaration_requires_layout_v2@1` |
| v1 | — | **有り** | **`ray_instance_requires_layout_v2@1`** |
| v2 | 無し | 無し | custom index 0、`defaults` 相当 |
| v2 | 有り / 無し | 有り / 無し | 解決値 |

##### 決定 2(#2・破綻): 優先順位は **2 本の 2 段 chain**。3 段ではない

carrier は field ごとに**分離している** —— material rule は geometry flags 専用、
component は mask / instance flags / custom index 専用。
**同一 field で 3 段の勝敗を作れないので、証明できない条件だった。**

- `geometry_flags`: **`defaults < material rule`**
- `mask` / `instance_flags` / `custom_index`: **`defaults < ray_instance`**

v1 の「3 段重ねで検査」は**削除**する。

##### 決定 3(#3): geometry flags の受け入れは **本番 layout の reload** で取る

v1 の「同一 allocation の opaque/mask 混在」は**本番入力から構成できない** ——
material rule は material 鍵であり、production model は
同 material の primitive を 1 group に再編する
([modeltemplate.cpp:89](../src/core/model/modeltemplate.cpp#L89))ので、
その allocation の全個体が同じ flags になる。
そして **test override を使うと偽陽性**になる ——
override の BLAS flags が非ゼロだと現行 `can_reuse` が**全 reuse を禁止**するため、
cache 鍵が allocation だけのままでも「2 build」が出てしまう
([accelerationstructure.cpp:744](../src/core/renderer/accelerationstructure.cpp#L744))。

**受け入れを差し替える**:

- 同一 allocation・同一 flags の個体を **3 つ以上**置く
- **本番 layout を `F → G → F` と reload** する
- 各世代で **build がちょうど 1 回**(reuse 全無効を落とす)
- **G 化で再 build、F 復帰でも再 build**(旧 F variant が残る実装を落とす)
- **受け入れで test override を使うことを禁止する**

##### 決定 4(#4): 配線を範囲に入れる

`add_component` の op は core にあるが、**登録 codec のある型しか足せない**
([editorjournal.cpp:212](../src/core/communication/editorjournal.cpp#L212))。
そして snapshot 生成 loop には join 用の identity が無い。
**次を本 WP の範囲に明記する**:

1. `ray_instance` の **codec / schema 登録**と **ECS runtime kind**
2. **attach / remove** と **save / reload**
3. **authoring object → renderer instance の join**、
   **AS snapshot への authoring object ID の運搬**
4. 値の変更・component の着脱による **invalidation**

**受け入れは実 RPC から始める。** helper 直呼びを禁止する。

##### 決定 5(#5): **v2 専用の AS selector 型**を作る。v1 binding selector を流用しない

現行 `RayBindingSelector` に **`material_name` が無く**、document selector は
**同一 document の全 material に一致する**
([raygrouplayout.cpp:320](../src/core/shader/raygrouplayout.cpp#L320))。
v1 の「既存 typed selector をそのまま使える」は**偽**だった。

- **AS 専用の selector 型・厳密 parser・canonical key** を作る
- 受け入れ: **同一 document の Oak と Pine を、本番 snapshot から選別**して
  片方だけ flags が変わる

##### 決定 6(#6・破綻): **「当たらない規則」はエラーではない**

v1 の `ray_as_row_material_identity_unavailable@1` と
「未解決 material の拒否」は**判定不能**だった ——
project の material catalog が無く、active な draw snapshot からは
「scene B 用の正当な規則」と「typo」を区別できない。

**分ける**:

| 何を検査するか | いつ | code |
|---|---|---|
| selector の**構文**(`document` と `asset` の排他、必須 field、`source_material_index` の範囲) | **layout parse 時** | `ray_as_rule_malformed@1` |
| **同一 exact key の重複規則** | layout parse 時 | `ray_as_duplicate_rule@1` |
| row の authored material identity が**空**、かつ **AS rule が 1 つ以上ある** | TLAS lowering 時 | `ray_as_row_material_identity_unavailable@1` |

**現在の scene に当たらない規則は正当**である(scene は複数ある)。
黙って defaults に落ちるのが正しい。

##### 決定 7(#7): `instance_flags` の wire を確定する

語彙は `VkGeometryInstanceFlagBitsKHR` に 1:1 対応させる:

`triangle_facing_cull_disable` / `triangle_flip_facing` /
`force_opaque` / `force_no_opaque`

- **`force_opaque` と `force_no_opaque` の同時指定は
  `ray_as_conflicting_instance_flags@1`**
- 未知の語は `ray_as_unknown_flag@1`(黙って無視しない)
- `geometry_flags` の語彙は `opaque` / `no_duplicate_any_hit_invocation`
- `mask` は **8 bit**、`custom_index` は **24 bit**。範囲外は名前付き拒否

##### 決定 8(#8・#9): 受け入れの否定対照と digest 分離

- **test override を受け入れで使わない**(決定 3)
- **本番 frame 経路を通す。**「本番の loop を写経したテスト」を禁止する ——
  **本番と受け入れが同じ関数を呼ぶ**こと(WP372b-fix と同じ形)。
  **本番側の解決を消す変異で受け入れが落ちる**ことを必須にする
- **SBT digest と AS digest の分離**を受け入れに入れる:
  **AS rule だけを変えて SBT digest が不変**であること

##### 決定 9(#10): 予約 namespace を**本 WP に入れる**

`ray_instance` を登録した時点で、**同名の外部 ECS component から名前を奪う**
([componentinfo.cpp:20](../src/core/ecs/componentinfo.cpp#L20) は登録名重複を拒否する)。
延期すると本 WP 自体が互換を壊す。**予約 namespace か version gate を本 WP で入れる。**

##### 決定 10(#11): AS 対象外の geometry に付いた component は黙って無効にしない

ray tracing の対象にならない geometry の object に `ray_instance` を付けたら、
**名前付きの診断**を出す(`ray_instance_not_ray_traced@1`)。黙って無効にしない。

##### 決定 11(#12): 全数は**起票時に実測**する

「全数 1502」は固定値として書くべきではない。**実装前に `ctest -N` で数え、
その数を基準として記録する**(skill §K の「`out of N` を毎回控える」)。

##### 受け入れ(v1 を置換)

- **mask(椅子 2 脚)**: 同じ material の個体 2 つで、片方の `ray_instance.mask` だけを
  変えると **GPU 出力が変わる**。component を消すと**両方写る**。**実 RPC から**
- **instance_flags**: 片方だけ `force_opaque` で **any-hit 呼び出し回数が減る**。
  profile active なら `ray_profile_opaque_as@1`。
  `force_opaque` + `force_no_opaque` で `ray_as_conflicting_instance_flags@1`
- **custom_index**: 宣言値が `gl_InstanceCustomIndexEXT` に出る。
  **row を追加して `q` をずらしても不変**。**SBT sentinel は独立に正しい**
- **geometry_flags**: 決定 3 の `F → G → F` reload。各世代 build 1 回、
  変更で再 build、復帰でも再 build。**override 使用禁止**
- **selector**: 同一 document の **Oak と Pine を選別**(決定 5)
- **優先順位**: 2 本の 2 段 chain を個別に(決定 2)
- **状態表**: 7 状態すべて。特に **no-layout / v1 + `ray_instance` の名前付き拒否**
- **digest**: AS rule だけを変えて **SBT digest 不変**
- **配線**: 実 RPC の `add_component` → render → `remove_component` が
  TLAS へ届く。**本番側の解決を消す変異で落ちる**
- **legacy**: layout 無し project の **source / SBT / TLAS instance の bytes が三つとも不変**
- **拒否**: malformed selector / 重複規則 / legacy provenance row /
  未知 flag / 範囲外 mask・custom_index / 非 ray-traced object の component
- 起票時に実測した全数を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成 /
  GPU は統合側で直列

##### やらないこと(v1 から変更)

per-geometry × per-instance の override(担体プログラム)/
Studio の UI(台帳)/ static AABB と `.rint`(RT3-2)/ RT4 以降。
**予約 namespace は範囲内へ移した**(決定 9)。


#### WP374 第 2 巡(8 件)と v3 決定差分 —— **v3 が最終。仕様レビューは打ち切り**

第 2 巡の 8 件は**すべて「仕様を締めよ」**であり、
「機構が違う」は 1 件も無い(第 1 巡で本番構成不能だった条件は除去できている)。
**v3 は v2 への差分であり、触れていない決定は v2 のまま。**

##### 差分 1(#1): **10 組合せ**を列挙し、優先順位を固定する

v2 の 7 行は重複と wildcard があり、v2 最終行が 4 状態を 1 行に畳んでいた。

| # | layout | `acceleration_structure` | `ray_instance` | 結果 |
|---|---|---|---|---|
| 1 | 無し | — | 無し | legacy ABI(`0,1,…`) |
| 2 | 無し | — | **有り** | `ray_instance_requires_layout_v2@1` |
| 3 | v1 | 無し | 無し | **`q` のまま** |
| 4 | v1 | 無し | 有り | `ray_instance_requires_layout_v2@1` |
| 5 | v1 | **有り** | 無し | `ray_as_declaration_requires_layout_v2@1` |
| 6 | v1 | **有り** | 有り | **`ray_as_declaration_requires_layout_v2@1` が優先**(layout を先に読むため) |
| 7 | v2 | 無し | 無し | custom index 0 / `defaults` |
| 8 | v2 | 無し | 有り | component 値 |
| 9 | v2 | 有り | 無し | rule + `defaults` |
| 10 | v2 | 有り | 有り | 両方 |

- **本番は AS scope が無いと component inventory を読む前に return する**
  ([renderer.cpp:4671](../src/core/vkcore/renderer.cpp#L4671))。
  そして scene load は Renderer 生成より先である
  ([pelican_core.cpp:82](../src/core/userpublic/pelican_core.cpp#L82))
- **したがって #2・#4 は scene load 時の preflight validator で出す。**
  AS scope の有無に関わらず**必ず走る**production validator を作る
- 受け入れは **10 行すべてを実 project root + scene load / frame 経路から**実行する

##### 差分 2(#2): build 列と **resident cache key 集合**

v2 の「各世代 build 1 回」は、毎 frame cache を捨てる実装でも通り、
旧 F を保持したまま再利用しない実装でも通る。
さらに **cache 鍵にだけ flags を入れ、実際の
`vk::AccelerationStructureGeometryKHR.flags` に空を渡す実装**も通った
([accelerationstructure.cpp:809](../src/core/renderer/accelerationstructure.cpp#L809) が実消費点)。

**受け入れを差し替える**:

- 列を固定: **`F1(+1) → F2(+0) → G1(+1) → G2(+0) → F3(+1) → F4(+0)`**
- **各 commit 後の resident cache key 集合**を診断に出し、
  **常に `{(allocation, 現在の flags)}` と完全一致**すること(count では不可)
- **同じ F/G/F テスト内で GPU 実効を見る** —— any-hit 呼び出し回数が
  **F と G で変化し、F 復帰で戻る**

##### 差分 3(#3): 受け入れを配線の全長にする

`set_component_value` の live ECS 配線は **component 名の hard-code 分岐**であり、
新しい codec は自動では通らない([editorruntimefactory.cpp:916](../src/core/communication/editorruntimefactory.cpp#L916))。
add/remove は generic ECS migration を通る(:951)。**両方を通す。**

受け入れの手順を固定する:

1. **実 JSON-RPC bytes** を `RpcServer::run` へ流して `add_component`
2. render して**実 TLAS 値と GPU 出力**を確認
3. attach したまま **`set_component_value`** して再 render
4. **`save_scene`** 後に **fresh reload** して再 render
5. `remove_component` して既定へ戻る

- **authoring object ID の保存場所を明記する。**
  `DrawItemSnapshot` に ID が無く、model asset reload は snapshot を再生成する
  ([polygoninstancecontainer.cpp:903](../src/core/renderer/polygoninstancecontainer.cpp#L903))ので、
  **model reload を跨いで ID が保たれること**を受け入れに入れる

##### 差分 4(#4・破綻): 多重一致を固定する

同じ row が **document/name 規則と asset/index 規則の両方に一致し得る**
(project material を持つ model row は両 identity を持つ)。
exact key が違うので重複検査を通り、**どちらを採るかが未定義**だった。
既存 binding resolver は同じ事態を row 単位の多重一致エラーにしている
([raygrouplayout.cpp:934](../src/core/shader/raygrouplayout.cpp#L934))。

- **0 一致 = defaults / 1 一致 = 適用 / 2 以上 = `ray_as_rule_ambiguous@1`**
- **valid identity の定義**: 「完全な document + material_name の対」**または**
  「完全な asset + source_material_index の対」。
  **partial identity(document のみ / document + 空 name /
  asset + `noSourceMaterialIndex`)は名前付き拒否**
- 受け入れに**本番 snapshot から作る二重一致の負例**を入れる

##### 差分 5(#5・破綻): 完全な wire grammar を本文に置く

v2 は v1 §1〜§3 を置換すると宣言しながら、flag 語彙と bit 幅しか再定義していなかった。
**次を v3 本文で確定する**:

```
ray_instance component:
  { "name": "ray_instance",
    "mask": <u8, 省略可, 既定は layout defaults.mask>,
    "instance_flags": <string[], 省略可, 既定は layout defaults.instance_flags>,
    "custom_index": <u32 の下位 24 bit, 省略可, 既定 0> }
```

- **field ごとに独立して override** する(component 全体の置換ではない)
- **未知 field は `ray_instance_unknown_field@1`**、
  型違いは `ray_instance_field_type@1`、範囲外は `ray_instance_field_range@1`
- **field を 1 つも持たない component は `ray_instance_empty@1`**
  (何も宣言しない component は誤りである)
- `acceleration_structure` を書くなら **`defaults` は必須**。
  `rules` は省略可(空配列と同義)
- **canonical default は 1 型・1 箇所**に置き、layout の `defaults` と
  component の省略値が**同じ既定を指す**こと(既定値の所在は一箇所)
- strict parser の**正負両方**を受け入れに入れる

##### 差分 6(#6・破綻): **version gate を選ぶ**(二択を決める)

- **layout mode を component 登録より前に確定**する
  (起動順は core 登録 → 外部 game logic 登録 → scene load → Renderer/layout parse
  なので、**layout mode の preflight を最前へ出す**。差分 1 の validator と同じ場所)
- **v2 のときだけ builtin の codec / ECS 型を登録**する
- **no-layout / v1 で外部 owner が同名を登録済みなら、外部型として扱う**
  (奪わない)。**未登録の builtin 形だけ**が `ray_instance_requires_layout_v2@1`
- **mode を跨ぐ hot reload(v1 ↔ v2)は名前付き拒否**する
  (`ray_layout_mode_change_requires_restart@1`)
- 受け入れ: **登録順を反転した負例**と、
  **no-layout の外部 `ray_instance` を壊さない正例**

##### 差分 7(#7): 非 ray-traced の判定は **static 分類の前**に行う

分類器は skinned / morph / VAT / BLAS-ineligible を**先に除外**し、
row lowering は static index だけを走査する
([accelerationstructure.cpp:205](../src/core/renderer/accelerationstructure.cpp#L205))。
**lowering 内だけに実装すると、component 付き row が到達せず黙って消える。**

- **static 分類の前に、authoring object 単位で component と候補 primitive を突合**する
- **model 無し / skinned のみ / morph のみ / VAT のみ / BLAS-ineligible のみ**を
  **それぞれ実 production scene から**拒否する(`ray_instance_not_ray_traced@1`)
- **static と excluded が混在する object**は、**static row にだけ適用し、
  エラーにしない**(混在は正当な構成である)

##### 差分 8(#8): 構成 matrix

- **parser / codec / 状態表のテストは `PELICAN_RUNTIME_SHADER_COMPILER` ON/OFF 両方**
- **v2 の GPU / reload 受け入れは ON 限定**と明記する
- **OFF では既存 `layout_requires_runtime_compiler@1` が先行する**ことを確認
  ([raygrouplayout.cpp:887](../src/core/shader/raygrouplayout.cpp#L887))
- `PELICAN_WITH_OPENXR` ON/OFF のコンパイルと flat/xr の owner 共有、
  `PELICAN_WITH_IMGUI` ON/OFF も matrix に含める
- `SKIP_DEVSTUDIO` 両構成

##### 実装可否

**実装する。** 第 2 巡は「機構が違う」を 1 件も出していない。
全数は**起票時に `ctest -N` で実測**して基準とする(v2 決定 11)。


### WP374a: RT3-1 の受け入れを本番経路と GPU で書き直す(修正パッケージ。連鎖打ち切り)

**§4 規則 11 上段のうち、コードレビューのみ(仕様は WP374 v3 で確定済み)。見積: 中。**
**WP374 の実装本体(worktree `agent/wp374`、22 ファイル)は触らない。受け入れだけを足す。**

#### 統合側で確定した事実(2026-09-02)

WP374 の実装は **ビルド 0 エラー、非 GPU 1208/1208 緑、構成 matrix 4 本で WP374 群 8/8 緑**。
しかしエージェント自身の変異で **UNMET 4 件**が出ており、**GPU テストは 1 本も書かれていない**
(新規 8 件は全部 CPU)。**WP372b と同じ穴**(受け入れが本番経路を通っていない):

| # | 変異 | 結果 | 意味 |
|---|---|---|---|
| 1 | `set_component_value(ray_instance)` の live invalidation を外す | **8/8 緑のまま** | 「実 RPC bytes → add/set/save/reload/remove → TLAS」の受け入れが存在しない |
| 2 | SceneLoader から preflight 呼び出しを外す | **8/8 緑のまま** | 状態表テストが helper 直呼び。本番 scene load を通っていない |
| 3 | BLAS cache 鍵から解決済み geometry flags を外す | 非 GPU では緑 | F→G→F の GPU 受け入れが無い |
| 4 | `vk::AccelerationStructureGeometryKHR.flags` に空を渡す | 非 GPU では緑 | 「鍵にだけ入れて Vulkan に渡さない」実装が通る |

#### 範囲(WP374 v3 の受け入れのうち、未実装のものだけ)

**A. 本番経路(CPU)**

- **実 JSON-RPC bytes** を `RpcServer::run` へ流す `add_component(ray_instance)` →
  render → `set_component_value` → 再 render → `save_scene` → fresh reload → 再 render →
  `remove_component`。各段で **実 TLAS plan の値**(mask / flags / custom index)を検査
- **10 状態表を本番 scene load(SceneLoader)経由で**走らせる。
  #2・#4(`ray_instance_requires_layout_v2@1`)と #5・#6(`…requires_layout_v2@1` 優先)は
  **preflight validator が本番の scene load から呼ばれること**で出ること
- **変異 1・2 が赤くなること**が本 WP の存在理由

**B. GPU(`[gpu]` ラベル。統合側で直列に回す)**

- **椅子 2 脚**: 同じ material の個体 2 つで片方の `ray_instance.mask` だけを変えて
  **GPU 出力が変わる**。component を消すと両方写る
- **instance_flags**: 片方だけ `force_opaque` で **any-hit 呼び出し回数が減る**
- **custom_index**: 宣言値が `gl_InstanceCustomIndexEXT` に出る。
  **row を追加して `q` をずらしても不変**。**SBT sentinel は独立に正しい**(同一テスト)
- **F→G→F**: `F1(+1) F2(+0) G1(+1) G2(+0) F3(+1) F4(+0)` の build 列、
  **各 commit 後の resident cache key 集合が `{(allocation, 現在の flags)}` と完全一致**、
  **any-hit 呼び出し回数が F と G で変化し F 復帰で戻る**
- **変異 3・4 が赤くなること**

#### 禁止事項(WP372b-fix と同じ)

- **`EditorEditRuntimeDependencies.execute` にラムダを注入しない**
- **`BehaviorSceneProjectionAdapter` 等の adapter をテストで直接構築しない**
- **`…ForTesting` の override 型 seam を受け入れで使わない**(観測 seam は可)
- **helper 直呼びで「本番経路」と称しない**。本番の分類・validator・executor を
  **消す変異で落ちる**ことを、各項目について報告に書く

#### 受け入れ

- 上記 A・B が **WP374 v3 の各項目に 1:1** で対応し、**変異 1〜4 がそれぞれ赤くなる**
- 実装本体の変更が必要になった場合(観測のための診断追加など)は、
  **何をなぜ変えたかを報告に明記**する。意味論の変更は UNMET として報告し、勝手に変えない
- 既存 1208(非 GPU、`SKIP_DEVSTUDIO=ON`)を壊さない / `SKIP_DEVSTUDIO` 両構成 /
  出荷 4 project と `src/core/resources/` の bytes 不変

#### やらないこと

単一 production executor seam の導入(**別 WP**。14 箇所の `.execute` 注入の一掃はそちら)/
実装本体の再設計 / RT3-2 / Studio UI。


### WP375: 単一の本番 executor seam —— 受け入れが本番の分類・adapter 構築・commit を写経できなくする

**§4 規則 11 中段(`pelican_project` は触らないが core の複数経路に跨る)。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 中。**
**規範は台帳 WP372c 裁定の持ち越し 8 と、テスト品質の仕分け(2026-09-02)。**

#### 何が壊れているか(このセッションで 3 回踏んだ)

編集の本番経路は `executeEditorProjection`(分類 → adapter 構築 → transaction commit)だが、
**匿名 namespace の中にあり、`EditorRuntimeModules`(本番の live module 14 個への参照)を
丸ごと要求する**([editorruntimefactory.cpp:835](../src/core/communication/editorruntimefactory.cpp#L835)・:156)。
テストはこれを呼べないので、**`EditorEditRuntimeDependencies.execute` にラムダを注入し、
分類と adapter 構築を手で書き直している** —— **14 箇所 / 4 ファイル**
(`wp361_prefab_test.cpp` に 11、`devstudio_inspector_test` / `editorjournal_test` /
`inspector_test` に 1 ずつ)。

結果、**本番の分岐を消してもテストは緑のまま**になる。実例:

| WP | 消した本番側 | テスト |
|---|---|---|
| WP372b | 写像を arena へ渡す dict insert | 5/5 緑のまま(→ WP372b-fix) |
| WP374 | `set_component_value(ray_instance)` の live invalidation | 8/8 緑のまま |
| WP374 | SceneLoader からの preflight 呼び出し | 8/8 緑のまま |

#### 起票時の実測(2026-09-02)

- `executeEditorProjection` が触る `modules.<field>` は **10 種類だけ**:
  `ecs_core` / `component_info` / `models` / `polygon_instances` / `camera` / `lights` /
  `physics` / `behavior_arena` / `project_config` / `scene_loader`。
  **`renderer` には触らない** —— つまり **GPU 無しで呼べる**
- それらは既に単体テストが module container 経由で構築している
  (WP372a の受け入れが `GET_MODULE(BehaviorAttachmentArena)` を使っている)
- transaction の target は `EditorProjectionDocumentTarget` 抽象で、
  テスト用実装が 2 つある(`SceneProjectionTarget` / `PrefabProjectionTarget`)。
  **target は差し替えてよい。差し替えてはいけないのは分類・adapter 構築・commit である**
- 本番組成 `makeEditorRuntimeService` を呼ぶテストは `golden_harness.cpp` だけ

#### 1. seam の形

- `executeEditorProjection` を **`internal::` に出し**、依存を
  **実際に使う 10 module だけを持つ `EditorProjectionExecutorModules`** に絞る
  (`EditorRuntimeModules` からの投影を本番側で 1 回作る)
- **target を引数にする**(`EditorProjectionDocumentTarget &`)。
  本番は `EphemeralEditorProjectionTarget`、テストは既存の 2 実装を渡す
- **本番の `.execute` はこの関数への転送だけ**になる(:1775)。
  分類・adapter 構築・commit のロジックを転送側に残さない
- **分類表(op 名 → adapter 集合)を seam の中で 1 箇所**にする。
  現在 component 名を hard-code している分岐([editorruntimefactory.cpp:916](../src/core/communication/editorruntimefactory.cpp#L916))も
  ここに集約する。未知の op は**名前付きで拒否**(今は adapter 0 個で commit する:
  WP372c 第 1 巡 #2 で判明)

#### 2. 14 箇所の置換

- 14 箇所の `.execute` ラムダを**すべて seam の呼び出しに置換**する。
  テスト側に分類・adapter 構築を残さない
- 置換できないテストがあれば、**理由を UNMET として報告**する(勝手に残さない)
- `ResolvedSceneResolver` 直叩き 41 箇所は**本 WP の範囲外**(次の仕分け WP)

#### 受け入れ(§4 規約 10)

- **本番側の分岐を消す変異で、該当テストが落ちる**。最低限、次の 3 つを当てる:
  WP372b の dict insert(現在は WP372b-fix で共有関数化済み。その関数を壊す)/
  WP374 の `set_component_value(ray_instance)` invalidation /
  未知 op の名前付き拒否。**3 つとも赤くなること**
- **未知 op が名前付きで拒否される**(`editor_projection_unknown_operation@1`)。
  今は adapter 0 個で document だけ commit される
- **14 箇所すべてが seam を呼ぶ**。テストコードに `.execute = [` が残っていない
  (これは静的検査ではなく、**残っていれば上の変異が通ってしまう**ことで示す)
- 本番組成(`makeEditorRuntimeService` → RPC)経由の既存テストと golden が**不変**
- 既存テスト(起票時に実測した全数)を壊さない / 全数 2 回 / `SKIP_DEVSTUDIO` 両構成 /
  GPU は統合側で直列

#### やらないこと

`ResolvedSceneResolver` 直叩きの仕分け(次 WP)/ override 型 `ForTesting` seam の棚卸し(次々 WP)/
transaction や adapter の意味論の変更 / Studio 側の変更。

依存: WP374a(RT3-1 の受け入れ)の後。WP374 の invalidation 分岐が変異対照に入るため。


### テスト品質と設計負債の解消計画(2026-09-02。利用者承認済みの順序)

利用者の指示: 「将来の開発で障害になりそうなもの / エンジンの設計思想に反していそうなものを
解消する。開発時のテスト群の品質も気になる」。Opus 期の判断 10 件を仕分けした結果と、
テスト群の実測から、次の順序で進める。**各項目は着手時に設計文書か WP を起こす。**

```
#3 文書修正(済: design_editor_lifecycle v5 追記)
→ WP374a(RT3-1 の受け入れを本番経路と GPU で)
→ WP375(単一 executor seam。写経の根本対策)
→ WP376(ResolvedSceneResolver 直叩き 41 箇所の仕分け)
→ WP377(override 型 ForTesting seam の棚卸し)
→ 設計: object identity(#4) / 例外契約(#5)
→ 担体プログラム(#10) → Studio 汎用 component UI(#9)
```

#### テスト群の実測(2026-09-02)

| 指標 | 値 |
|---|---|
| `TEST_CASE` | 1430 本(ctest 登録 1502) |
| `.execute` ラムダ注入(本番 executor の写経) | **14 箇所 / 4 ファイル** |
| `ResolvedSceneResolver` 直叩き | **41 箇所** |
| 本番組成 `makeEditorRuntimeService` を通すテスト | **golden_harness のみ** |
| `ForTesting` seam(本番ヘッダ) | 141 個。上位は観測 seam(`readRenderTargetForTesting` 63、`documentForTesting` 19 …)で健全。**危険なのは判定を差し替える override 型だけ** |
| 誤挙動を固定していた fixture | `custom_index == q` を 6 本が「正」と検査(§3 違反) |
| 否定対照の不足 | inactive scene 保持契約の対照が 1 例(実投出点は約 65。下表) |

欠陥の種類は **1 つ**に集約される: **受け入れが本番経路を通っていない**。
WP372b / WP374(×2)で「本番の分岐を消しても全数緑」が実際に起きた。

#### 設計トラックの問題定義(着手時の入力。実測済み)

**object identity(#4)** —— *決定性と「宣言データとして開く」に反する*

- 実行時サブシステムは **`AuthoringObjectId` を一切知らない**:
  `src/core/light` 0 files / `phys` 0 / `animation` 0(`loader` 11、`renderer` 1)
- 名前を key にする実行時 map: Light に **6 つ**
  ([lightcontainer.hpp:32](../src/core/light/lightcontainer.hpp#L32)・:116-122)、
  `SceneLoader::object_bindings`([scene.hpp:43](../src/core/loader/scene.hpp#L43))、
  Physics の collider identity、Animation の cache、Camera/Controller の signature
- 生の名前解決 API が公開されている([scene.hpp:57](../src/core/loader/scene.hpp#L57))ので、
  「解決者を表で網羅する」は**実装不能**(WP372c 第 2 巡 #5)
- 論点: 名前を identity から降ろす(`AuthoringObjectId` を実行時へ運ぶ)か、
  rekey を全 subsystem が実装するか。**WP304(安定 ID)不合格の隣接領域**

**例外契約(#5)** —— *fail-fast と named error に反する*

| 場所 | 型無し `throw std::runtime_error` | 型付き |
|---|---|---|
| `components/collider.cpp` | **17** | 0 |
| `components/spriteview.cpp` | **15** | 0 |
| `asset/atlasasset.cpp` | **31** | 0 |
| `loader/componentcodec.cpp` | 2 | 0 |

- resolver の `catch (const std::exception &)` が **`std::logic_error` と `std::bad_alloc` を
  component error として保持**([resolvedscene.cpp:308](../src/core/loader/resolvedscene.cpp#L308))
- `EditorProjectionTransaction::commit` が **`noexcept` で全例外を変換**し、下流が
  `method_unavailable` へ潰す —— **内部 invariant の破れが本番で観測できない**
- 論点: codec 境界の typed data-error 契約(code + `json_pointer`)、
  `logic_error`/`bad_alloc` の再送出、平坦化の解除、見える場所(query serialization か terminal)

**担体プログラム(#10)** —— *RT4 / RT5 / RT7 と R5 が同じものを要る*

- `StructFieldType` は `I8…U64 / F32 F64 / Vec2 Vec3 Vec4 Quat / String Bool Enum` の
  **固定 leaf のみ。可変長は存在しない**
- `set_component_value` は既存 JSON pointer を `.at()` で要求し、欠落 field も行も作れない
- authoring object → renderer row の join が無い(`DrawItemSnapshot` に authoring ID 無し)。
  key は `node_index` と importer variant を含める必要がある(`{asset, mesh, primitive}` は単射でない)

**Studio 汎用 component UI(#9)** —— *「web で開ける ⊆ pelican で開ける」は満たすが、常に一歩遅れる*

- Studio に component を追加・削除する経路は**無い**
  (`inspectorwidget.cpp:519` の `addComponent` は widget 配置の helper であって RPC ではない)
- core には `add_component` / `remove_component` op がある。**Studio 側だけの穴**
- 論点: schema catalog から追加候補を出す汎用 UI(`ray_instance` に限らない)

#### 台帳の負債として固定(動かさない)

- **E2 の前提条件**: destroy が入ると、参照先が消えた unpack の undo が
  `PrefabObjectRefUnresolved` で落ちる(WP372b v3 差分 8 の撤回の前提が消える)
- **version gate(#8)の監視**: v1↔v2 の hot reload 拒否がイテレーションを縛ったら改名案へ


#### WP375 仕様レビュー第 1 巡(8 件・破綻 3)と v2 —— **plan と materialize を分ける**

**v2 が本節の正。v1 の §1・§2 と受け入れは v2 で置換される。**

**自分の誤りを先に書く**: 「`executeEditorProjection` は `modules.renderer` に触らないので
GPU 無しで呼べる」は誤り。触る 10 module のうち **`PolygonInstanceContainer` と
`LightContainer` は ctor で Vulkan resource を確保する**
([polygoninstancecontainer.cpp:223](../src/core/renderer/polygoninstancecontainer.cpp#L223)、
[lightcontainer.cpp:206](../src/core/light/lightcontainer.cpp#L206))。
「renderer を直接読まない」と「GPU resource を要求しない」は別だった。
また本番 commit の target を preview 専用の `EphemeralEditorProjectionTarget` と書いていた
(正しくは `ProjectBasicConfigProjectionTarget`:
[editorruntimefactory.cpp:1110](../src/core/communication/editorruntimefactory.cpp#L1110))。

##### 実測(v2 を書く前に数えたもの)

- **adapter の ctor は参照を保持するだけ**で module を触らない(全 6 種の ctor 本体を確認。
  `Impl` の aggregate 初期化のみ)。Vulkan の費用は **module の構築側**にある
- Vulkan を ctor で取る module は **`PolygonInstanceContainer` と `LightContainer` の 2 つだけ**。
  `ECSCore` / `BehaviorAttachmentArena` / `ProjectBasicConfig` / `PathResolver` /
  `ComponentInfoManager` は既に CPU テストが `GET_MODULE` で構築している
- `EditorProjectionFaultInjector` は transaction の ctor 引数 1 本でしか流れない
  ([editorprojectiontransaction.hpp:289](../src/core/loader/editorprojectiontransaction.hpp#L289))。
  seam は**ポインタを転送するだけ**でよい
- preview executor は 39 行の**第二の分類器**(`transform` / `light` だけを許す:
  [editorruntimefactory.cpp:794](../src/core/communication/editorruntimefactory.cpp#L794))。
  `.execute_preview` の注入は 3 箇所
- 未知 op は **実 RPC では `prepareRpcOperation` が `method_unavailable` で先に拒否**する
  ([editorjournal.cpp:1613](../src/core/communication/editorjournal.cpp#L1613))。
  「adapter 0 個で commit」は内部で捏造した request でしか起きない

##### 決定 1(#2・破綻): seam を **plan(純粋)と materialize(module 束縛)に分ける**

このセッションで実際に出た 3 つの穴(写像の dict insert / invalidation の route /
preflight の呼び出し)は**すべて分類・routing の欠陥**であり、adapter の中身ではない。
**分類を純粋関数に切り出せば、GPU 無しで穴を検出できる。**

```
planEditorProjection(request, target.projectionState(), context) → EditorProjectionPlan   // 純粋
materializeEditorProjection(plan, modules, target, fault_injector?) → EditorProjectionResult // 束縛 + commit
```

- **`EditorProjectionPlan`** は typed: どの adapter 種別を、どの scene / object に、
  どの mode で作るか。lifecycle edit・behavior edit・origin transition・availability を含む。
  **JSON ではなく struct**(比較可能・決定的)
- **`context`** は immutable: `{active_scene_id, behavior_availability, mode}`。
  今は `configuredGameLogicStatus()` が ambient global を読んでいる
  ([editorruntimefactory.cpp:856](../src/core/communication/editorruntimefactory.cpp#L856))。
  **本番の context factory を 1 つ**作り、受け入れもそれを通す
- **plan の入力は `target.projectionState()` に統一**する(#3)。
  今は route planner が `modules.project_config.resolvedScene()` を読み、transaction は
  target を読むので、**分類元と commit 先が食い違い得る**。`project_config` /
  `scene_loader` を plan から外す
- materialize は module 参照を受けて adapter を作り transaction を commit する。
  **`PolygonInstanceContainer` / `LightContainer` を要する adapter は GPU テストでのみ
  materialize**。CPU テストは plan の検証と、CPU module だけの materialize(ECS / arena /
  physics / camera)を行う

##### 決定 2(#1): 本番 target は `ProjectBasicConfigProjectionTarget`

`.execute` は `ProjectBasicConfigProjectionTarget`、`.execute_preview` だけが
`EphemeralEditorProjectionTarget`。**target を Ephemeral に替える変異で
save → fresh reload の受け入れが落ちる**ことを対照にする。

##### 決定 3(#7): preview は **mode** であって第二の executor ではない

`planEditorProjection` に `mode ∈ {Committed, Preview}` を持たせ、preview の許可
component(`transform` / `light`)と restore semantics は **mode ごとの exhaustive policy**
にする。`executeEditorPreview` の分類器は消す。`.execute_preview` の 3 注入も棚卸し対象。

##### 決定 4(#8): 分類表は **codec が宣言する** —— executor に component 名を並べない

executor の component 名分岐([editorruntimefactory.cpp:916](../src/core/communication/editorruntimefactory.cpp#L916))は
**codec 登録表([componentcodec.cpp:901](../src/core/loader/componentcodec.cpp#L901))と別の第二 registry**である。
WP374 の `ray_instance` はまさに**両方に追記**していた。

- `ComponentCodec` に typed **`EditorProjectionPolicy`** を持たせる:
  `TransformClosure / RendererModel / EcsCodec / Camera / Light / Collider`
- planner は component 名を再列挙せず、**codec の policy から plan を作る**
- 受け入れは **`componentCodecs()` の全要素**について set / add / remove の
  実 runtime 値と、同一テスト内の無効側を検査する。**codec を足して policy を忘れると
  コンパイルか登録で落ちる**(既定値を持たせない)

##### 決定 5(#6): 未知 op は **typed variant + exhaustive visit**

- preparation / replay / executor が同じ **`EditorCanonicalOperation` variant** を共有し、
  planner は `std::visit` を exhaustive にする(新 op を足して分岐を忘れるとコンパイルで落ちる)
- 未知の wire op は**既存の入口で 1 度だけ**拒否する(`method_unavailable`。現行どおり)
- seam 側の防御的拒否は **unit invariant** であり、本番変異には数えない
- v1 の `editor_projection_unknown_operation@1` は**作らない**

##### 決定 6(#4・#5): 17 箇所(edit 14 + preview 3)を **4 種に分類**して置換する

| 種類 | 扱い | 例 |
|---|---|---|
| **production acceptance** | 本番の plan + materialize を呼ぶ。**受け入れとして数えるのはこれだけ** | WP372a/b/374 の実 RPC テスト |
| **coordinator / UI 単体** | fake executor 可。**本番受け入れとして数えない**と節に明記 | `editorjournal_test` の `RuntimeMirrorAdapter` と lifecycle trace |
| **fault matrix** | 既存の `EditorProjectionFaultInjector` を seam が**そのまま転送**(adapter 集合は変えない) | WP361 / WP372a の AfterPublication |
| **observation** | read-only hook のみ | trace 文字列の検査 |

- `EditorEditRuntimeDependencies.execute` の `std::function` は**残す**(coordinator 単体のため)。
  ただし **本番組成では final な production executor を保持**し、注入を許さない
- **受け入れの変異を差し替える**(v1 の「3 変異が赤」は 14 箇所の置換を証明しなかった):
  1. `makeEditorRuntimeService` から seam 呼び出しを消す
  2. 本番 target を Ephemeral に替える
  3. 本番 context factory を消す(availability が ambient に戻る)
  4. **各 production acceptance が、実際に解決した runtime 値と保存 document の両方**を検査

##### 決定 7: 構成 matrix

- `PELICAN_WITH_IMGUI=OFF` では `inspector_test` 自体が build されない
  ([test/CMakeLists.txt:1107](../test/CMakeLists.txt#L1107))。
  **IMGUI ON/OFF 両方で「存在する側の注入が全部 seam 経由」**を示す
- `SKIP_DEVSTUDIO` 両構成 / 実 RPC テストは `PELICAN_WITH_RPC` 内(既存どおり)
- `PELICAN_WITH_OPENXR` と `PELICAN_RUNTIME_SHADER_COMPILER` は executor 本文に分岐なし
  (第 1 巡が確認)。コンパイルのみ

##### 受け入れ(v1 を置換)

- **plan の決定性**: 同じ request + projection state + context で **plan が byte 一致**
  (JSON 経由でなく struct 比較)
- **plan の網羅**: `componentCodecs()` の全要素 × set/add/remove で、plan が
  codec の policy どおりの adapter 種別を出す。**policy を消すとコンパイルで落ちる**
- **routing の穴 3 つ**(写像 dict insert / `ray_instance` invalidation / preflight)を
  **plan の検証で検出**する —— それぞれ本番の plan から消す変異で落ちる。
  **GPU 無しで**
- **本番組成の変異 4 つ**(決定 6)がそれぞれ落ちる。実 RPC → edit → save → fresh reload
- **production acceptance に分類した全テスト**が plan + materialize を呼び、
  fake executor を使わない。coordinator 単体は節に列挙して**数えない**
- **preview**: `mode = Preview` で `transform` / `light` 以外が名前付き拒否。
  `executeEditorPreview` の分類器が消えている(消し忘れると preview 変異で落ちる)
- 本番組成(`makeEditorRuntimeService` → RPC)経由の既存テストと golden が**不変**
- 起票時に実測した全数を壊さない / 全数 2 回 / 決定 7 の matrix

##### やらないこと(v1 から変更)

`ResolvedSceneResolver` 直叩きの仕分け(WP376)/ override 型 `ForTesting` seam の棚卸し(WP377)/
**`PolygonInstanceContainer` / `LightContainer` の CPU state 分離**(要るなら別設計。本 WP は
GPU ラベルで materialize する)/ transaction・adapter の意味論の変更 / Studio。


#### 後続 WP / 設計トラックの入力(2026-09-02 実測。起票時に読み直すこと)

**WP376(`ResolvedSceneResolver` 直叩き 41 箇所)** —— 大半は正当。仕分けの結果:

| 種類 | 件数の目安 | 例 | 扱い |
|---|---|---|---|
| resolver そのものの単体テスト | 大半(`wp361_prefab_test` の identity / closure / 展開順、`wp360`) | 「generated requests return only the named unsupported error」 | **正当。移さない** |
| resolver を**オラクル**として使う | 1 | `devstudio_outliner_test`「Studio offline open resolves…」—— Studio の出力を resolver と突き合わせる | **正当**(resolver は本番オラクル) |
| **resolved-scene provider を手で用意**して service/RPC を組む | 2 ファイル | `schema_rpc_test::runFixtureRpc` / `editorcommandservice_test::explicitResolvedProvider` | **仕分け対象**。service 層の単体境界としては許容し得るが、本番 provider(`ProjectBasicConfig` の projection state)を通す受け入れが別に要る。**WP375 の context/target の形が決まってから**判断 |

→ WP376 は「41 箇所を移す」WP ではなく、**provider を手で用意する 2 ファイルの扱いを決め、
本番 provider 経由の受け入れを 1 本足す**小さな WP になる見込み。

**WP377(override 型 `ForTesting` seam)** —— RT の 6 seam はテストでの使用が **計 6 箇所**、
うち **2 箇所が production タグ付き受け入れの中**:

- `WP368 production project load reaches RT2b tables …`(`headless_render_test`)
- `WP369 production material alpha profile matches mip zero …`(同)

WP374 で「override があると偽陽性」が実証済みなので、この 2 本は**観測 seam か本番入力へ
置き換える**のが本丸。残り 4 箇所は sentinel / addressing の単体テストで、用途を確認して残す。
fault 注入 3 個(`setSceneImportFault` / `setSceneSaveFault` / `setSceneSaveHook`)は
AfterPublication matrix 用で**正当**。

**object identity(#4)の設計入力** —— 当初の見積もりより**小さい**:

- 名前で object を引く公開 API は **`SceneLoader::objectId / hasObjectTransform /
  objectTransform` の 3 つ**([scene.hpp:72](../src/core/loader/scene.hpp#L72))。
  外部の呼び出し側は **3 ファイル**(camera controller / rpcserver / animation)
- **`SceneRuntimeObjectBinding{authoring_object_id, object_id}` が既に存在**
  ([scene.hpp:27](../src/core/loader/scene.hpp#L27))。実行時は既に安定 id を持っている。
  **名前なのは各 subsystem の map の key だけ**
- 名前 key の実行時構造: `SceneLoader::object_bindings`(1)/ Light の map(6)/
  `Camera::scene_cameras`(1)/ Physics の collider identity 文字列 / Animation の cache /
  Controller の signature
- 設計の形: **新しい ID を作らず、既存の binding の `authoring_object_id` を key に据え替える**。
  WP304(安定 ID)不合格の理由(永続 ID の再採番・snapshot import)には触れない

**例外契約(#5)の設計入力** —— `StructFieldValidationError{code, field_path}`
([structfieldjson.hpp:26](../src/core/userpublic/details/schema/structfieldjson.hpp#L26))が
既に code + path 付きの利用者エラー型。`StructFieldErrorCode` は 6 値。
設計は**新しい型を発明せず、これを codec 境界へ一般化**する(collider 17 / sprite_view 15 /
atlas 31 / componentcodec 2 の型無し throw を移す)。


#### WP375 第 2 巡(6 件・破綻 2)と v3 決定差分 —— **v3 が最終。仕様レビューは打ち切り**

第 2 巡は「着手可能にする最低条件」を 7 項目で明示した。**v3 はその 7 項目を 1:1 で閉じる差分**であり、
触れていない決定は v2 のまま。決定 2(本番 target)・決定 7(matrix)は第 2 巡でも壊せなかった。

##### 差分 1(#1・破綻): preflight は plan では検出できない。**本番 `SceneLoader::load` の caller 変異**にする

preflight は planner の route ではなく、`SceneLoader::load()` が validator を呼ぶかどうかである
([scene.cpp:350](../src/core/loader/scene.cpp#L350))。plan の入出力に一切影響しないので、
**plan 検証では caller 削除を検出できない**(pure validator を共有しても同じ)。

- routing の穴 3 つのうち **「写像の dict insert」「invalidation の route」は plan 変異**で検出する
- **preflight は別枠**: 本番 `SceneLoader::load` 経由で **valid / invalid の scene を同一テスト**に置き、
  `validateResolvedSceneFeatureBindings` の**呼び出しを消す変異**で invalid 側が受理されて落ちる

##### 差分 2(#2): context は **immutable snapshot**。planner の ambient 読み取りを禁止する

v2 の `{active_scene_id, behavior_availability, mode}` では決定性が保証できない —— WP374 の
`componentCodecs()` は `tryGet<RayLayoutRuntimeState>()` で返す集合を変え、behavior canonicalization は
singleton registry を読み、codec の canonical default は thread-local である。

**`EditorProjectionContext`(immutable)**:

| 要素 | 内容 |
|---|---|
| `active_scene_id`, `mode` | v2 どおり |
| **codec policy catalog** + **layout semantic mode / generation** | `componentCodecs()` の結果を凍結して持つ |
| `behavior_availability` + **registry snapshot / generation** | canonicalization は**入口で行い、typed operation に canonical value を凍結**する。planner は registry を読まない |
| **canonical default snapshot** | thread-local を planner から追放 |

- **materialize は `EditorRuntimeBindingState &` と期待 epoch / generation を明示引数**で受け、
  不一致は **`editor_projection_stale_bindings@1`**(名前付き)
- **規範**: planner は `FastModuleContainer` / singleton registry / thread-local default を**読んではならない**。
  受け入れ: 同じ request + projection state + context で plan が struct 一致し、
  **ambient(`RayLayoutRuntimeState` / registry / default)だけを変えても plan が変わらない**

##### 差分 3(#3): codec は **type-erased な materialization hook** を持つ。adapter 内の第二表を禁止する

policy だけでは足りない —— WP374 の `EcsCodecProjectionAdapter` は token variant を
`Animation / Sprite / RayInstance` に固定し、名前を再列挙している
([wp374: editorprojectionadapters.cpp:776](../src/core/loader/editorprojectionadapters.cpp#L776))。
lifecycle adapter にも `light / collider / behavior` の除外と `simplemodelview` の特殊処理がある(:185, :252, :378)。

- `ComponentCodec` に **`EditorProjectionPolicy` + type-erased `editor_prepare / editor_swap`** を持たせる
- **generic adapter は plan が保持する codec descriptor を呼ぶ**。executor と `EcsCodecProjectionAdapter` の
  **双方で component literal の列挙を禁止**する
- **lifecycle / prefab の複合 adapter**: 内部 routing を policy ごとの sub-route に展開するのは本 WP では行わず、
  **「複合 adapter 内の routing は plan-only 検証の対象外」と認め、本番 materialize 変異を置く**
  (差分 5 の変異 (d))
- 受け入れ: **新 codec `foo` を policy 付きで登録**し、adapter に一切追記せずに
  set / add / remove が runtime に届く(**追記が必要な実装は落ちる**)

##### 差分 4(#4): **規範 route matrix** を置く。preview の意味 gate は planner 1 箇所

exhaustive `std::visit` は handler の存在しか保証せず、**空 plan を返す handler もコンパイルできる**。

**route matrix**(op × mode → 期待 adapter 種別・scene / object・payload・cardinality)を本文に置く。
行は現行 12 op すべて:

| op | Committed | Preview |
|---|---|---|
| `set_component_value` | codec policy による(Transform closure / RendererModel / EcsCodec / Camera / Light / Collider) | **`transform` / `light` のみ**。他は `editor_preview_unsupported@1` |
| `add_component` / `remove_component` | policy による + lifecycle(behavior なら arena) | 拒否 |
| `reparent` | **transform closure + 専用 adapter の両方**([editorruntimefactory.cpp:910](../src/core/communication/editorruntimefactory.cpp#L910)) | 拒否 |
| `spawn` / `destroy` / `remove_objects` / `restore_objects` | lifecycle adapter + **renderer / camera / light / collider / behavior の各 participant** | 拒否 |
| `set_prefab_parameter` / `unset_prefab_parameter` / `unpack_prefab` / `restore_prefab` | prefab route(full projection なら renderer / camera / light / collider / behavior 全部)+ origin transition | 拒否 |

- **preview の semantic gate は planner だけ**。`requireLivePreviewCapability`
  ([editorjournal.cpp:2417](../src/core/communication/editorjournal.cpp#L2417))は**構造検査のみに縮める**
  (planner を変異させて camera preview を許しても、入口が先に拒否して緑になる状態を無くす)
- 受け入れ: **matrix の全行**について「plan が期待 adapter 種別を出す / 期待しない種別を出さない」を
  **struct 比較**で検査し、**行を空にする変異が落ちる**。`reparent` は runtime の local / world transform、
  spawn / destroy は 5 participant の実 runtime 値まで見る

##### 差分 5(#5・破綻): 変異は **compilable・非同値な 4 個**。factory 経由の production acceptance と 1:1

v2 の「4 変異」は 3 個 + assertion 要件だった。「context factory を消す」は inline 化で観測同値、
「seam 呼び出しを消す」は build failure になり意味論を検出しない。

| # | 変異(compilable) | 落とす production acceptance(**`makeEditorRuntimeService` → RPC → edit → save → fresh reload**) | 観測 |
|---|---|---|---|
| (a) | materialize を **document-only commit**(adapter 0 個)に bypass | `set_component_value(transform)` | 保存 document は新値、**runtime transform は旧値** |
| (b) | 本番 target を `EphemeralEditorProjectionTarget` に替える | 同上 | **fresh reload 後の document が旧値** |
| (c) | context の **availability / generation を意図的に誤らせる**(`dll_unavailable` 固定) | `add_component(behavior)` | behavior が pending のまま、`editor_projection_stale_bindings@1` か activation 不発 |
| (d) | **計画済み adapter 群の 1 つを materialize しない**(renderer participant を落とす) | `spawn`(model 付き object) | document に object あり、**renderer instance 無し** |

- **plan + materialize を直接呼ぶテストは production composition と数えない**
- `golden_harness` は save → fresh reload を行わないので、**上記 4 本は新規に書く**

##### 差分 6(#6): 17 箇所の分類表(file:line / test 名で固定)

| 分類 | 箇所 | 扱い |
|---|---|---|
| **production**(本番 seam へ置換) | `wp361_prefab_test:1354` WP361a production lifecycle adapter(**hybrid。手書き executor を seam に置換**)/ `:3786` WP372a real prefab parameter producer / `:3945` WP372b real unpack RPC / `:4755` WP372b availability wiring | 4 |
| **fault matrix**(fault injector を seam が転送) | `:3237` WP362 set/unset undo redo fault points / `:3577` WP372a reconciliation fault matrix | 2 |
| **coordinator / UI 単体**(fake executor 可。**受け入れに数えない**) | `:1838` WP361 journal / `:2761` WP362 real RpcServer(coordinator 検証)/ `:2968` WP362 closure CAS / `:4288` WP372b marker corpus / `:4561` WP372b inverse closure / `devstudio_inspector_test:925,937` gizmo commit / `editorjournal_test:149,215` fixture helper / `inspector_test:132,138` fixture helper | 8 + 3 |
| observation | — | 0 |

**production 候補を coordinator に格下げして受け入れ数を減らすことを禁止する**(表が規範)。

##### 差分 7: 実装可否

**実装する。** 第 2 巡の 7 条件は上記で 1:1 に閉じた。依存: **WP374a の統合後**
(差分 5 (d) と invalidation の plan 変異が WP374 の `ray_instance` route を対照に使う)。
全数は着手時に `ctest -N` で実測する。




#### WP375 完了記録(統合側。2026-09-02、`e855c032`)

**新規**: `src/core/communication/editorprojectionplan.{hpp,cpp}`(plan / materialize / `EditorProjectionContext` / route matrix)、
`test/wp375_editor_projection_test.cpp`。18 ファイル(+1830 / -808)。継続 4 回(GPU 失敗 4 件 → 受け入れ c/d の是正 → named code)。

##### 回収時の監査(WP324 の教訓: 削除を数える)

- component 名の分岐(`transform` / `simplemodelview` / `light` / preview の `set_component_value` 限定)は codec の `EditorProjectionPolicy` と
  planner の route matrix へ移設。`boundEditorEntity` の throw は factory に残る
- `.execute` ラムダは 14 のまま。production 4 箇所(WP361a lifecycle / WP372a producer / WP372b unpack / availability)は `executeWp375Seam` へ委譲し、
  その helper は本番の `planEditorProjection` + `materializeEditorProjection` を呼ぶ。`make_unique<…ProjectionAdapter>` の直接構築は 0
- **継続で見つかった実装欠陥 2 件**(受け入れが本番分岐を観測していなかったことで露出):
  (i) 非 prefab の direct behavior edit に availability が伝播しておらず、DLL 不在時に pending attachment を作れなかった → 伝播(`behaviorarena.cpp` `prepareEditorEdits`)。
  (ii) renderer participant 欠落時に `SceneObjectProjectionAdapter` が model の staging を **黙って飛ばしていた** → 名前付き拒否に変更
- **named code の弱化を 1 件差し戻し**: DLL あり + 未登録 behavior 型の拒否が journal 層の `method_unavailable` から
  `catch(std::exception)` → `AdapterPrepareFailed` + 文字列に落ちていた → `behavior_type_unknown@1{type, object_id}`(`BehaviorTypeUnknownError`)を
  editor と scene load の両方で共通化。受け入れは `error.code` / `error.data` の exact 一致(文字列照合は禁止)

##### 統合側の変異 5 本(本番組成。エージェントは GPU 禁止のため UNMET 報告)

| 変異 | 結果 |
|---|---|
| P: `SceneLoader` の preflight caller を消す | 赤(`WP375 production SceneLoader preflight …`) |
| (a): materialize を document-only commit に | 赤(acceptance a) |
| (b): 本番 target を Ephemeral に | 赤(acceptance b) |
| (c): availability を DllUnavailable 固定 | **初回 緑 → 受け入れ是正後 赤**(active + 未知型が pending になって落ちる) |
| (d): renderer participant を落とす | **初回 緑 → 受け入れ是正後 赤**(participant 欠落で spawn commit が名前付き拒否) |

初回 (c)(d) が緑だった理由: (c) は既知の型を足すので availability が分岐しない設計のテストだった / (d) は検査前の `ECSCore.update()` が
本番 ECS system の publish を観測していた(adapter の participant を観測していなかった)。**「変異が緑」は受け入れ側の欠陥として扱い、
実装を疑う前にテストが本番分岐を観測しているかを確かめる**(2 件とも実装欠陥も同時に露出した)。

##### 統合側の実行

- worktree: 非 GPU 1212 / 1212(+1 skip)、GPU 指定 6 / 6
- 統合後 main: **1526 件中 1525 緑**。落ちた 1 件は `WP334b animgraph preset …`(`-j4` 負荷での commit 待ちタイムアウト。単独再実行 1/1 緑)。
  **WP334b 系は今日 4 回 `-j4` で同型の flake**(WP377 統合時 3 件、今回 1 件)。commit 待ちの timeout が並列負荷に対して短い。台帳の既知 flake として記録(直すなら timeout ではなく待ち条件の見直し)
- コードレビュー: `rev375` → **reject**(致命後退 1 + 実害 3 + 自己参照 oracle)→ WP375a として起票・投入。#3(loader の rollback)と #6(generation guard の分類)は記録のみ


### WP375a: 非アクティブ scene の prefab 編集が active scene の behavior を全削除する(WP375 コードレビューの致命後退 1 件 + 実害 3 件 + 自己参照 oracle)

**§4 規則 11 上段(core の複数経路)。コードレビューのみ(§10)。見積: 中。WP375 → 375a の 1 段で打ち切る(連鎖上限。致命後退なので一度だけ延長)。**
**依存: なし(WP375 統合済み `e855c032`)。WP379 / WP380a は本 WP の統合後に投入する(同じ planner / factory を触る)。**

#### 何が壊れているか(コードレビュー `rev375` が確認)

1. **致命後退**: 旧 factory は `if (scene_id != current_scene) continue;`(`e855c032^:editorruntimefactory.cpp:99`)で非アクティブ scene の prefab 編集を
   document-only にしていた。新 planner はこの filter を持たず、非アクティブ scene の `set_prefab_parameter` が **その scene の behavior 集合をグローバルな
   `BehaviorAttachmentArena` に reconciliation** する。対象 scene に behavior が無ければ **active scene の behavior が全部消える**
2. **実害**: `BehaviorBatch` の set / add / remove は `boundEditorEntity()` が空なら**型検査より先に黙って return** する
   ([editorruntimefactory.cpp:1396](../src/core/communication/editorruntimefactory.cpp#L1396))。active scene の light-only object(example の `BlueSpot`
   [main.scene.json:899](../projects/example/scenes/main.scene.json#L899))に未登録型の behavior を足すと **正常 commit** になる(旧 journal は `method_unavailable` で拒否していた)
3. **実害**: DLL 不在時の pending attachment(`owner=engineRegistrationOwner`、`destroy=nullptr`、[behaviorarena.cpp:1181](../src/core/gamelogic/behaviorarena.cpp#L1181))は
   publish 後の transaction rollback が**空実装**。`AfterPublication` fault で document は戻るが attachment は残り、再試行が `identity is already live` で衝突し得る
4. **実害**: `set_component_value(simplemodelview)` の renderer route で `models` / `polygon_instances` module が無いと**黙って無視**
   (lifecycle 側は WP375 で throw に変えたが set 側は残った。同じ route matrix 内で fail-fast と silent が混在)
5. **自己参照 oracle**: route matrix テストの期待値を検査対象の `codec.editor_policy` から計算している([wp375_editor_projection_test.cpp:120](../test/wp375_editor_projection_test.cpp#L120))。
   stock `simplemodelview` の policy を `Camera` に変えても通る。`LifecycleBehavior` / `Prefab*` route は materializer で no-op、production acceptance は spawn 後の renderer しか実値を見ない

#### 直すこと

1. planner: **committed projection でも `route.scene_id != context.activeSceneId()` なら runtime route を生成しない**。document-only は明示の route / status にする
2. `BehaviorBatch`: **active registry の型検査を binding lookup より前**に置く。active scene の unbound object への behavior add は、ECS migration を同一 transaction で行うか、
   名前付き `runtime_binding_missing@1` で拒否する(どちらにしたかと理由を報告)。journal-only 経路(inactive scene の undo)も同じ検査を通す
3. pending attachment の rollback: `PreparedBehaviorSceneReconciliation` と同様に **旧 vector / sequence / params を保持する token** にし、publish 後も完全に逆操作できる
4. set route の participant 欠落は **participant 名を含む名前付きエラー**(lifecycle と同じ code 系)。headless / document-only を許すなら null pointer ではなく context / route で明示
5. route matrix の期待値を **独立した literal oracle**(component 名 → route の表をテストに直書き)にし、同一テスト内で **正しい policy と故意に誤った policy の結果差**を検査する。
   production acceptance に camera / light / collider / behavior route の**実 participant 解決値**を足す(route を 1 つ無効化するとそれぞれ落ちる)
6. `raytracing_rt3_1_production_test`(実 `RpcServer` を直接生成)を **`PELICAN_WITH_RPC` で guard**(RPC OFF では stub に ctor が無い。[test/CMakeLists.txt:1262](../test/CMakeLists.txt#L1262))

#### 受け入れ(§4 規約 10)

- **後退の対照**: `main` に active behavior、`secondary` に behavior 無しの prefab instance `P`。`set_prefab_parameter(P, …)` 後に **`main` の attachment が全部残り active**
  (今日 = 全削除)。document は `secondary` 側だけ変わる。同じテストで `secondary` を active にして同じ編集をすると runtime にも反映
- **型検査の順序**: light-only object(出荷 example の `BlueSpot` を production fixture で読む)へ未登録型 → `behavior_type_unknown@1`。登録済み型 → 2 の裁定どおり(migration か `runtime_binding_missing@1`)。
  型検査を binding lookup の後ろへ戻す変異で落ちる
- **rollback**: DLL 不在 + binding 済み object に behavior add + `AfterPublication` fault → attachment が消え、**同一 process で再試行が成功**(今日 = `identity is already live`)
- **participant 欠落**: `{ecs, component_info}` だけの modules で `set_component_value(simplemodelview)` → 名前付きエラー(今日 = 黙って成功)
- **oracle**: policy を故意に誤らせた側が literal 表と食い違って落ちる。route 無効化 × 4(camera / light / collider / behavior)がそれぞれ production acceptance で落ちる
- `PELICAN_WITH_RPC=OFF` で configure + build が通る(今日 = link error)
- 出荷 4 project の bytes 不変 / golden 不変 / 全数(着手時に実測)/ `SKIP_DEVSTUDIO` 両構成 / GPU は統合側で直列

#### やらないこと・台帳に残すだけ

- レビュー #3(設定 DLL が起動時に無く、後から不適合 DLL が現れた reload で旧 runtime を再構築しない: [gamelogicreload.cpp:184](../src/core/gamelogic/gamelogicreload.cpp#L184) が
  `active == null` のとき behavior sources を検証しない)は **WP381 / WP380 の loader 領域**。既知欠陥として記録
- レビュー #6(generation guard が production で自己充足し、内部不整合を `schema_violation` に分類)は **E-3a**(commit noexcept と guard 再設計)で扱う(WP378 v2 決定 1 で訂正。E-0a は触らない)。記録のみ


#### WP375a 完了記録(統合側。2026-09-02 深夜。**codex 停止中のため統合側の検証と小修正は Claude が実施**)

- **実装(codex)**: planner に `DocumentOnly` route(committed projection でも非 active scene は runtime route を生成しない)/ registry 型検査を binding lookup より前に
  (裁定: ECS migration ではなく `runtime_binding_missing@1`。light-only object に behavior を足すためだけに runtime entity を新設しない)/ pending attachment の rollback を
  旧 vector・sequence・params・activation 状態を保持する swap 型 inverse token に / renderer・camera・light・collider の欠落 participant を participant 名入りの名前付きエラーに /
  route matrix の期待値を literal 表に / production acceptance 4 件(非 active prefab・pending rollback・BlueSpot 検証順・4 route の participant 実値)/
  `PELICAN_WITH_RPC` OFF の guard(RPC transport と RPC 専用テストを除外。golden の `editor_runtime_binding` case は RPC ON のみ)
- **codex の継続(compiler OFF で WP375 acceptance を SKIP/named error にする)は使用上限で build 途中に停止**。作業ツリーの変更(fixture の compiler OFF 分岐)はそのまま採用し、Claude が検証
- **統合側で見つけて直したもの(Claude)**:
  (i) BlueSpot 受け入れが worktree で `DamagedHelmet.glb` 不在で落ちた → 出荷 example の model は **gitignore 済みのローカル asset**(smoke と同じ依存)。worktree へ copy して検証。
  受け入れがローカル asset に依存することを記録(hermetic でない。smoke と同じ扱い)
  (ii) `error.data` は edit 拒否の契約ではない: journal の wire 変換が `behavior_type_unknown` **だけ** `data` を複製する特別扱い(4 回目継続の codex 修正)になっていた。
  edit 拒否の詳細は **`error.payload`**(manual 13_editor.md の契約)。特別扱いを削除し、受け入れ c と BlueSpot は `payload` を読む(`runtime_binding_missing@1` の `payload` に
  `object_id` / `scene_id` が載る)
  (iii) compiler OFF で acceptance b が「Runtime shader compiler is disabled; B-layer surface source is unavailable」で落ちた: fixture の `renderable=true` が model の material
  (`openpbr/opaque_single.surface`)を引き、compiler 無しでは surface source が無い。b の目的(transform の commit target が fresh reload を越えて残る)に model は要らないので、
  **compiler OFF では非 renderable** にした(`PELICAN_RUNTIME_SHADER_COMPILER != 0`)。ON 側は不変
  (iv) 非 renderable にすると fixture は object を 1 つも書かず `object_not_found` になった → 他の非 renderable fixture と同じく **compiler OFF では transform だけの `Chair`** を scene に書く。
  BlueSpot(出荷 example)は compiler OFF では surface source が無く load 不能 → smoke と同じ gate で **`SKIP("shipping example requires the runtime shader compiler")`**
- **検証(worktree、Claude)**: 通常構成 build 0 error / 非 GPU **1217 / 1217** / WP375 系 GPU **19 / 19**(修正 (i)(ii) 後)/ compiler OFF build 0 error + WP375 系 **19 / 19**(BlueSpot は named SKIP)/ 修正 (iii)(iv) 後の ON 側 WP375 系 **19 / 19**
- **統合側の変異 F1**(planner の active-scene filter を再び消す = WP375 の後退を再現): **赤**(`WP375a inactive prefab RPC is document-only …` が落ちる)。残留 0
- 統合後 main(`1aeafc59`、`SKIP_DEVSTUDIO=OFF`、projects dir、GPU 直列、`-j4`): **1534 件中 1532 緑**。落ちた 2 件(`WP375a inactive prefab …` / `WP375a production set routes …`)は
  **`pelican_wp375_behavior_fixture.dll` の shadow copy 衝突**(`gamelogicreload.cpp:249` "shadow copy failed: The process cannot access the file")。shadow dir が
  `<DLL dir>/.pelican-hot-reload` で **同じ fixture DLL を使うテスト process 間で共有**され、`-j4` で衝突する。単独再実行 **2 / 2 緑**。既知の穴に記録(shadow dir を process ごとに分ける = WP381 の loader 変更に同梱)
- コードレビュー `rev375a`(同夜、fast 無し): **reject**。致命: `DocumentOnly` が Committed 限定で **Preview の inactive light が active の light を置換**(出荷 example で到達)。実害: pending behavior の journal handle が DLL reload 後に stale。検証欠落: RPC OFF guard が非 RPC case まで消す。→ **WP375b**(375a の修正の完成として 1 回だけ)。#4〜#6 は記録。壊せなかった: Committed 全 op の DocumentOnly、undo の scene routing、snapshot import、inverse token の即時 rollback、統合側の 2 修正は代理条件ではない

### WP375b: 非 active scene の **preview** commit が active scene の light を置換する(WP375a の filter が Preview に効いていない)+ pending behavior の journal handle + RPC guard の粒度

**§4 規則 11 上段。コードレビューのみ(§10)。見積: 小〜中。**
**連鎖: WP375 → 375a → 375b。375a は WP375 が落とした filter を Committed にだけ戻したので、同じ後退が Preview に残った(= 375a の修正の完成であって新しい輪ではない)。
これ以降 WP375 系で致命が出たら、起票ではなく revert を先に検討する。**
**依存: なし(WP375a 統合済み `1aeafc59`)。WP379 / WP380a と並列(planner / preview / journal / test CMake を触る。統合時に main を取り込む)。**

#### 何が壊れているか(コードレビュー `rev375a` が出荷 example で到達を確認)

1. **致命**: `appendDocumentOnlyIfInactive`([editorprojectionplan.cpp:322](../src/core/communication/editorprojectionplan.cpp#L322))は `Committed` 限定。
   **Preview** は light / transform を許し、light preview は対象 scene の全 light を作って global `LightContainer` と交換する
   ([editorprojectionadapters.cpp:1158](../src/core/loader/editorprojectionadapters.cpp#L1158))。出荷 example で `scene_flow_second`(inactive、light 1 個)の
   `SceneFlowKeyLight` を `open_preview → set → commit_preview` すると **active `default_scene` の light 14 個が 1 個に置換されたまま**になる(abort なら restore route で戻る。成功 commit だけ壊れる)
2. **実害**: pending behavior(DLL 不在)の commit は attachment handle `h2` を journal inverse に保存する([editorjournal.cpp:1190](../src/core/communication/editorjournal.cpp#L1190))。
   DLL が現れて reload → runtime teardown + scene rebuild で arena が消え、同じ attachment に `h3` が振られる([behaviorarena.cpp:824](../src/core/gamelogic/behaviorarena.cpp#L824))。
   undo は `h2` で検索して **見つからない**([editorjournal.cpp:4005](../src/core/communication/editorjournal.cpp#L4005))
3. **検証欠落**: `PELICAN_WITH_RPC=OFF` の guard が `raytracing_rt3_1_production_test` **実行体ごと**([test/CMakeLists.txt:1266](../test/CMakeLists.txt#L1266))と
   `wp361_prefab_test.cpp:1358` の**テスト全体**を消し、RPC を使わない case(WP377 の直接 `SceneLoader` 検証、WP374a の 10 状態 matrix、WP375 preflight、
   coordinator 直接経路)まで RPC OFF で消えている

#### 直すこと

1. **`DocumentOnly` を Preview にも適用**する(`open_preview` / `commit_preview` / restore route)。inactive scene の preview は document だけを更新し、runtime の light / transform を触らない
   (preview の表示は active scene の runtime を写す。inactive を「見る」機能は無いので、preview 自体を名前付きで拒否するのも可。どちらにしたかと理由を報告)
2. journal inverse は **handle ではなく attachment identity**(object / component index / sequence)で remove 対象を解決する。handle は runtime 内部の値であり journal に保存しない
   (保存するなら reload で再解決する)。DLL 不在 commit → DLL 出現 reload → undo が成功する受け入れ
3. RPC guard を **RPC を使う case だけ**に絞る(`#if PELICAN_WITH_RPC` を TEST_CASE / SECTION 単位に。実行体は常に build)。RPC OFF でも WP377 / WP374a / WP375 preflight / coordinator 直接経路が走る

#### 受け入れ(§4 規約 10)

- **preview**: 出荷 example(local asset 依存は smoke と同じ)で `scene_flow_second` の light を `open_preview → set → commit_preview` → **`default_scene` の runtime light は 14 個のまま**、
  document は secondary だけ変わる(今日 = 1 個に置換)。Preview 側の DocumentOnly を外す変異で落ちる。同じテストで active scene の light preview は runtime に反映
- **journal handle**: DLL 不在で behavior add(pending)→ save → DLL を置いて `reload_game_logic` → **undo が成功**し attachment が消える(今日 = `behavior attachment not found`)。
  identity 解決を handle 検索に戻す変異で落ちる
- **RPC OFF**: `-DPELICAN_WITH_RPC=OFF` の build で、WP377 の直接 `SceneLoader` 検証・WP374a の 10 状態 matrix・WP375 preflight・coordinator 直接経路が **登録され実行される**(件数を報告)。
  RPC を使う case は OFF では未登録
- 全数(着手時に実測)/ `SKIP_DEVSTUDIO` 両構成 / RPC ON/OFF / 出荷 4 project bytes 不変 / golden 不変 / GPU は統合側で直列

#### 記録のみ(やらない)

- `rev375a` #4: canonical operation の `scene_id` を偽装すると active op が `DocumentOnly` に落ちる(内部 API。通常 RPC からは到達しない)。planner が object の実所属で照合する改修は E-3a と同時に
- `rev375a` #5: participant 欠落テストは renderer だけ。camera / light / collider は production composition が全 participant を供給するため対照が作れない(記録)
- `rev375a` #6: BlueSpot 受け入れは local asset に依存(smoke と同じ。hermetic 化は asset 同梱方針の決定後)


#### WP375b 完了記録(統合側。2026-09-03、`c61ce078`)

- **実装(codex、fast 無し)**: planner の `DocumentOnly` を Preview にも適用(abort / forced-abort の restore でも canonical operation を保持し、inactive は `DocumentOnly`、active は
  Light / Transform participant へ)。裁定: inactive preview は名前付き拒否ではなく **document-only**(表示 runtime は active の projection であり inactive を表示する viewport は無い。
  document の編集・commit 自体は安全に成立する)。journal の behavior remove / set は **実行直前に object ID・component index・attachment sequence から現在の arena handle へ再解決**
  (journal 内の handle は互換のため残すが identity には使わない。sequence 不一致は `journal_integrity_error`)。RPC guard は RPC 依存部分だけ(production 実行体は RPC OFF でも常時 build、
  GPU / non-GPU の discovery を分割して RPC OFF で空の `[gpu]` discovery を作らない)
- **agent の CPU 変異**: `mode != Committed` に戻す → planner regression 赤 / 再解決を外す → 旧 handle 100 と reload 後 10100 の不一致で赤 / broad guard に戻す → RPC OFF の必須登録 4 → 0
- **RPC OFF**: configure + build 成功、非 GPU 1191 件(1190 pass、1 skip)。必須直接経路 4 件(WP377 SceneLoader / WP374a 10 状態 / WP375 preflight / WP361a coordinator)が登録・実行
- **統合側の検証**: 通常構成 build 0 error / 非 GPU **1222 件中 1221 緑**(`WP335 parameterized feature entries …` が `-j16` の commit 待ちで 1 回落ち、単独 1 / 1 緑 = 既知の負荷 flake 系)/
  WP375 系 GPU 23 件中 1 件赤 = `inactive shipping-scene light preview …` が **worktree に無い gitignore 済み texture**(`projects/example/assets/textures/Frame84.png`)で
  `open_preview` が `method_unavailable`("Failed to load image")→ 出荷 example の ignored asset(models / textures / .pelican)を worktree へ copy して再実行 → **1 / 1 緑**(dialog 無し。assertion は変異時のみ)
- **統合側の変異(GPU)**: **P1**(Preview を DocumentOnly から外す = WP375a の穴)**赤**(`light_count() == 14` に対し 1 = 後退がそのまま再現)/ **P2**(journal の handle 再解決を無効化)**赤**(reload 後の undo が failed)。残留 0。
  P1 の実行中に MSVC の Debug assertion dialog(`vector erase iterator outside range`)が出た(利用者が観測)。変異で runtime の light が置換された状態の teardown で不正 erase → **CRT の assert dialog を test harness が抑止していない**(自動実行を止める。既知の穴へ)
- main(WP379)取り込みで `raytracing_rt3_1_production_test.cpp` が 1 hunk 衝突(RPC helper の `#if` 囲い vs 関数分割)→ 両方を残す形で解消。worktree で WP375 / WP379 / WP374a / WP377 系 **38 / 38**(GPU 含む)→ squash。
- 全数(統合後 main、`SKIP_DEVSTUDIO=OFF`、projects dir、GPU 直列、`-j4`): **1544 / 1544 緑**
- コードレビュー `rev375b`: **reject**(実害 4)。inactive の lifecycle preview が open は通るのに abort 不能(lease 残留)/ `attachment_seq` が位置依存で正当な undo を拒否 / RPC guard が RPC OFF の build を壊し実 GPU 3 件から `gpu` label を落とす / 変異時の teardown が exception-safe でない。→ **WP375c**(375b が作った後退 2 件の一度限りの延長)。壊せなかった: active-light 置換の致命そのものは set 操作で塞がれている

### WP375c: WP375b が作った後退 2 件(RPC OFF の build 破壊 + GPU label 欠落 / inactive lifecycle preview が abort 不能)と、位置依存の `attachment_seq`

**§4 規則 11 中段。コードレビューのみ(§10)。見積: 小〜中。**
**連鎖: WP375 → 375a → 375b → 375c。375c は「修正が作った後退」の一度限りの延長(#1 #3 は 375b が作った)。#2 は 375b の identity 再解決が不完全だったもの。
これ以降 WP375 系で実害が出たら、起票せず WP375b/c を revert して設計からやり直す。**
**依存: なし(WP375b 統合済み `c61ce078`)。WP378 / WP379a / WP380a と並列(planner / journal / test CMake / production test を触るので統合時に main を取り込み、衝突は継続で解消)。**

#### 何が壊れているか(コードレビュー `rev375b` が確認)

1. **後退**: inactive scene の `add_component` / `remove_component` / `reparent` の preview は open では `DocumentOnly` として通る
   ([editorprojectionplan.cpp:724](../src/core/communication/editorprojectionplan.cpp#L724))が、restore は `set_component_value` 以外を拒否する(:628)。
   `abort_preview` が `editor_projection_invalid_request@1` になり **lease が解放されない**(commit かサービス再起動まで preview が塞がる)。
   forced-abort は全例外を `false` に潰し([editorjournal.cpp:3624](../src/core/communication/editorjournal.cpp#L3624))、reload / load_scene 側はそれを捨てて続行する
2. **実害**: `attachment_seq` は安定 ID ではなく **`(object_index << 32) | component_index` の現在位置**([behaviorarena.cpp:70](../src/core/gamelogic/behaviorarena.cpp#L70))。
   無関係な先行 object の destroy で同じ attachment の sequence が変わり、undo / redo / verification が `journal_integrity_error` で拒否される。`(0,0)` の正当な sequence 0 は
   `expected_sequence != 0` 条件で検証をすり抜ける。resolver 未設定なら旧 handle を黙って使う経路も残る(:1834)
3. **後退**: RPC guard が `raytracing_rt3_1_production_test` を `PELICAN_WITH_RPC=OFF` で **compile 不能**にした(WP379 の RPC test が guard 外 :1589)。
   executable の label を `GPU` → `MIXED_GPU` に変えた結果、実 GPU を使う 3 件(WP377 row guard / WP374a 10 状態 / WP375 preflight)から **`gpu` label が落ちた**
   (GPU lock 無しで並列実行される = flake 源)
4. **検証基盤**: 変異時に `REQUIRE` が落ちると `wp375FinishRuntime` に到達せず、runtime / document 不一致のまま module shutdown へ(MSVC assertion の一因)。
   production でも commit 失敗の catch が lease を解放しない

#### 直すこと

1. **preview 非対応判定を inactive shortcut より先に置き、lifecycle preview は一律に名前付き拒否**(inactive の lifecycle preview を許す仕様は無い)。
   forced-abort の失敗は名前付きで伝播(`false` に潰さない)。reload / load_scene はそれを無視しない
2. journal の identity は **object ID + authored component occurrence / type / origin** で検証し、正当な再解決では handle と現在 sequence の**両方を更新**する。
   位置 sequence を integrity 判定に使わない(ABA が要るなら永続 ID。今は要らない)。`(0,0)` の抜け道を閉じる。resolver 未設定は名前付き拒否
3. executable は RPC に関係なく `MIXED_GPU`。WP379 の RPC test 本体だけ `#if PELICAN_WITH_RPC`。実 GPU の 3 件に `[gpu]`。**configure-level contract**: RPC ON / OFF ごとの
   test 名・label・重複 0 を `ctest -N` 相当で検査するテストを登録
4. production fixture の teardown を **scope guard** にし、REQUIRE 失敗でも同じ停止順序を通す。production の commit 失敗 catch は lease を解放する

#### 受け入れ(§4 規約 10)

- **abort**: inactive scene の reparent を `open_preview` → 名前付き拒否(今日 = open 成功・abort 不能)。同じテストで active の set preview は open / abort とも成功
- **undo after destroy**: `[A, B]` で B に behavior add → 別 actor が A を destroy → B の add を undo が **成功**(今日 = `journal_integrity_error`)。
  identity 検証を位置 sequence に戻す変異で落ちる。`(0,0)` の抜け道: object 0 / component 0 の不一致が拒否される
- **RPC OFF**: `-DPELICAN_WITH_RPC=OFF` で configure + build が通り、実 GPU 3 件に `gpu` label が付く(`ctest -N` 一覧の contract test が ON / OFF で緑)。
  label を落とす変異・guard を広げる変異で contract test が落ちる
- **teardown**: 変異 P1 相当(Preview の DocumentOnly を外す)を当てて REQUIRE が落ちても **MSVC assertion が出ず** process が正常終了(統合側で確認)
- 全数(着手時に実測)/ `SKIP_DEVSTUDIO` 両構成 / RPC ON/OFF / 出荷 4 project bytes 不変 / golden 不変 / GPU は統合側で直列
### WP376: resolved-scene provider を手で用意するテストの仕分けと、本番 provider 経由の受け入れ

**§4 規則 11 中段。仕様レビュー(1 巡 + 決定差分)+ コードレビュー。見積: 小。**
**規範は「テスト品質と設計負債の解消計画」と「後続 WP の入力」(2026-09-02)。依存: WP375。**

#### 実測

`ResolvedSceneResolver` 直叩き 41 箇所のうち、**移すべきでないもの**が大半:

| 種類 | 扱い |
|---|---|
| resolver そのものの単体テスト(`wp361_prefab_test` の identity / closure / 展開順、`wp360`) | **正当。触らない** |
| resolver をオラクルにする(`devstudio_outliner_test`「Studio offline open resolves…」) | **正当。触らない** |
| **resolved-scene provider を手で用意して service / RPC を組む** | **本 WP の対象(2 ファイル)** |

対象:

- `test/schema_rpc_test.cpp:169` `runFixtureRpc` —— `ResolvedSceneResolver::resolve` の結果を
  `EditorCommandServiceDependencies.resolved_scene` に注入
- `test/editorcommandservice_test.cpp:75` `explicitResolvedProvider` —— 同上(revision で cache)

本番の provider は `makeEditorRuntimeService` の lambda
([editorruntimefactory.cpp:1747](../src/core/communication/editorruntimefactory.cpp#L1747)・:1767・:1839)で、
`ProjectBasicConfig` の projection state を返す。**テストの provider はこれを写経している。**

#### 決定

1. **service / RPC 層の単体テストとして、手製 provider は許容する**。ただし節に「受け入れに数えない」と明記
2. **本番 provider 経由の受け入れを 1 本足す**: `makeEditorRuntimeService` → 実 RPC →
   `get_components` / `query` 系が **`ProjectBasicConfig` の projection state を返す**こと。
   **provider lambda を手製と同じ形(resolver 直叩き)に差し替える変異で、
   projection state の revision / resolver generation の不一致が観測されて落ちる**
3. WP375 の `EditorProjectionContext`(immutable snapshot)を **query 側も同じ供給元から取る**
   (edit と query で resolved scene の世代が食い違う経路を残さない)

#### 受け入れ(§4 規約 10)

- 本番 provider 経由の query が、**edit → commit 直後**に新しい revision の resolved scene を返す。
  provider を resolver 直叩きに替える変異で **古い generation を返して落ちる**
- 2 ファイルの手製 provider は残るが、**節に列挙**され「受け入れではない」と明記される
- 既存テスト(着手時の全数)を壊さない / `SKIP_DEVSTUDIO` 両構成 / `PELICAN_WITH_RPC` 内

#### やらないこと

41 箇所の一括移設 / resolver の再設計 / Studio。


### WP377: override 型 `ForTesting` seam の棚卸し —— 受け入れから判定の差し替えを追放する

**§4 規則 11 中段。仕様レビュー(1 巡 + 決定差分)+ コードレビュー。見積: 小。**
**規範は「テスト品質と設計負債の解消計画」(2026-09-02)。依存: WP374 / WP374a(authored AS flags が前提)。**

#### 実測

本番ヘッダの `ForTesting` seam は 141 個。**観測 seam**(`readRenderTargetForTesting` 63 /
`documentForTesting` 19 / `pendingCountForTesting` 18 …)は健全で、本 WP の対象外。
**判定を差し替える override 型**は次だけ:

| seam | 使用 | 帰属テストのタグ | 用途 |
|---|---|---|---|
| `ScopedRayProfileAccelerationStructureFlagsOverrideForTesting` | `headless_render_test:12750` | **`[WP369]…[production]`** | opaque flags を注入して `ray_profile_opaque_as@1` の拒否を確認(否定対照) |
| `ScopedRayTracingTlasRecordOffsetOverrideForTesting` | `headless_render_test:11653`, **`:13238`** | rt1b sentinel / **`[WP368]…[production][mutation]`** | TLAS record-offset routing だけを入れ替え、別 image が選ばれることを確認(変異) |
| `ScopedRayGroupLayoutVirtualIncludeHookupOverrideForTesting` | `:11879` | rt1b sentinel | 生成 include の配線 |
| `ScopedRayTracingRecordAddressingValidationOverrideForTesting` | `:12125` | rt1b sentinel | addressing 検証の差し替え |
| `ScopedRayTracingValidationDelegateOverrideForTesting` | `raytracing_rt1a_test:1735` | rt1a wiring mutation | validation delegate の差し替え |
| fault 注入 3 個(`setSceneImportFault` / `setSceneSaveFault` / `setSceneSaveHook`) | — | — | **AfterPublication matrix 用。正当。対象外** |

WP374 で **「override があると偽陽性になる」**が実証済み(override の BLAS flags が非ゼロだと
`can_reuse` が全 reuse を禁止し、cache 鍵が壊れていても「2 build」が出る)。

#### 決定

1. **production タグの受け入れの中で override 型 seam を使うことを禁止する**(規範)。
   否定対照や変異は **本番入力で作る**か、**production タグを外して「変異 seam を使う単体」と明記**する
2. **WP369 @12750**: opaque flags の注入は、WP374 の **authored `acceleration_structure`
   (layout v2 の `geometry_flags: ["opaque"]`)で作り直す**。override を消す
3. **WP368 @13238**: record-offset の入れ替えは本番入力で作れない(routing そのものの変異)。
   **production タグを外し、「routing 変異の単体」として残す**。production の主張は
   同テスト内の非 override 部分だけに限定する
4. rt1a / rt1b の sentinel 用 4 箇所は**用途を確認して残す**(sentinel は設計 §4 が要求する検査)。
   ただし **production タグの中には置かない**
5. **AS flags override(`RayProfileAccelerationStructureFlagsOverrideForTesting`)そのものを削除**する。
   WP374 後は本番入力が存在するので、テスト用 override が本番の唯一の入力である状態は終わる
   (残すと「テスト用 override が本番入力」の形が再発する)

#### 受け入れ(§4 規約 10)

- `RayProfileAccelerationStructureFlagsOverrideForTesting` が **ヘッダから消え**、
  WP369 の否定対照が **authored flags で同じ `ray_profile_opaque_as@1` を出す**
  (authored flags を外すと拒否されなくなる = 対照)
- production タグの TEST_CASE で `Scoped…OverrideForTesting` を使う箇所が **0**
  (静的検査ではなく、**override を production 受け入れに戻す変異で偽陽性が再現する**ことを
  WP374 の F→G→F 受け入れで示す)
- GPU golden / 全数(着手時に実測)不変

#### やらないこと

観測 seam の削減 / fault 注入 seam の変更 / RT 以外の override seam(`forceGeneration` /
`forceSceneEpoch` / `setExecutionTracing` / `resetForTesting`)は**用途を記録するだけ**。


#### WP374a が本番経路で見つけた既存欠陥: RPC からの `add_component` / `remove_component` が常に失敗していた

**場所**: [editorruntimefactory.cpp:326](../src/core/communication/editorruntimefactory.cpp#L326)
(component migration adapter の ctor)。

```cpp
operation_{std::move(operation)},   // ← nlohmann::ordered_json を brace-init
```

`nlohmann::ordered_json` を **brace-init で別の json から初期化すると initializer_list ctor が選ばれ、
operation が一要素配列に包まれる**。直後の `operation_.at("op")` が
`json.exception.type_error.304: cannot use at() with array` で落ちる。

- **誰も踏んでいなかった理由**: 本番の `add_component` を **実 RPC bytes から**通す受け入れが
  1 本も無かった(既存テストは `.execute` を注入し、この adapter を経由しない)。
  **WP374a の「実 RPC bytes → add → render」がこの経路を初めて通した**
- 修正は 1 トークン: `operation_(std::move(operation))`。意味論の変更ではなく、
  書いた人の意図どおりに動くようにするだけ
- エージェントは意味論を変えない指示に従い、修正を戻して **UNMET** として報告した(正しい)。
  統合側で私が適用した

**この欠陥はテスト品質計画(2026-09-02)の主張そのもの**: 「受け入れが本番経路を通っていない」
とは、こういう欠陥が全数緑の裏に残るということである。

##### 統合の記録(回収時に追記)

- 全数(GPU 直列含む、`SKIP_DEVSTUDIO=ON` + projects dir): **1376/1376 緑**(WP374 の 8 + WP374a の GPU 4・CPU 2 を含む)。修正適用後、実 RPC 受け入れは緑
- 私が当てた 4 変異: 統合側の完了記録に記す(M1 invalidation / M2 preflight は CPU、M3 BLAS 鍵 / M4 Vulkan flags は GPU 直列)
- 受け入れ 2 本(実 RPC / 10 状態表)は **GPU 無しの実行体に入っていた**。device が無い環境では
  skip ではなくクラッシュする(このセッションで実際に SEGFAULT が出た)。
  **GPU ラベル付きの実行体へ移し `requireVulkanDevice` で skip させる**(回収時に処置)


#### WP374 + WP374a 完了記録(統合側。変異は私が当て直した)

**全数(GPU 直列含む、`SKIP_DEVSTUDIO=ON` + projects dir): 1376/1376 緑。** 内訳は WP374 の CPU 8 本 +
WP374a の GPU 4 本(`headless_render_test`)+ CPU 2 本(実 RPC / 10 状態表)。

##### 私が当てた変異(エージェントの報告を額面で受け取らない)

| # | 変異(適用は `git diff` で確認) | 対象テスト | 結果 |
|---|---|---|---|
| M1 | `editorruntimefactory.cpp:951` live invalidation の `component == "ray_instance"` → `false` | WP374a real RPC bytes … | **赤**(0/1) |
| M2 | `scene.cpp:350` `validateResolvedSceneFeatureBindings(*scene)` をコメントアウト | WP374a all ten root layout … | **赤**(0/1) |
| M3 | `accelerationstructure.cpp:846` BLAS cache 鍵の flags を `0u *` で消す | WP374a F G F …(GPU) | **赤**(0/1。G の +1 build と resident key が壊れる) |
| M4 | 同 :884 付近 Vulkan に渡す `geometry_flags` → `vk::GeometryFlagsKHR{}` | 同上 | **赤**(0/1。cache 鍵は合うが any-hit の GPU 実効が変わらない) |

M1 は **WP374 時点ですり抜けていた変異**(8/8 緑のまま)であり、WP374a の実 RPC 受け入れで初めて赤くなった。

##### 回収時の処置

1. **既存欠陥の修正**: `editorruntimefactory.cpp:326` の brace-init(前節)。1 トークン
2. **受け入れ 2 本の実行体移動**: 実 RPC / 10 状態表は device を要求するのに GPU ラベル無しの
   実行体に入っていた(device 無しでは skip でなく **SEGFAULT**。このセッションで実際に出た)。
   `raytracing_rt3_1_production_test`(`GPU` 登録)へ移し、共有 helper は
   `raytracing_rt3_1_fixture.hpp` に出し、各 case の先頭で `TestSupport::requireVulkanDevice` を呼ぶ
   (device 無しでは Catch の `SKIP`)
3. M1 の変異戻し(`git checkout`)で :326 の修正も一緒に戻っていた → 再適用して確認。
   **変異の restore は「対象ファイルの他の変更」も消す**。同一ファイルに修正がある場合は
   restore 後に修正の存在を再確認すること(skill §K に追記)

##### 台帳の負債(WP374 から持ち越し)

- per-geometry × per-instance override(担体プログラム)/ Studio の `ray_instance` UI /
  v1↔v2 hot reload 拒否の運用監視(#8)/ `resident_blas_keys` を frame-plan 投影に載せた件は
  WP354 の baseline を overlay で更新(permitted pointer は増やしていない)

##### 回収で踏んだ罠(追記): device の取得は fixture project の設定の**後**

受け入れ 2 本を GPU 実行体へ移して `TestSupport::requireVulkanDevice` を case 冒頭に置いたところ、
`Failed to open file: <temp>/passes/main_rendering_config.json` で落ちた。
`GET_MODULE(VulkanManageCore)` は **Renderer 連鎖を即座に構築し、その時点の project 設定
(未設定 = 既定 layout)を読む**。CPU 実行体で通っていたのは、device が
`renderWp374aProductionRows` の中で **project 設定後に遅延取得**されていたから。
処置: `requireVulkanDevice` を `loadWp374aScene` 内の `configureWp374aRuntime(root)` の直後へ移動。
SKIP の意味論(device 無しで Catch `SKIP`)は保たれる。

##### 統合(2026-09-02): WP374 + WP374a を `8af06f3f` として squash merge

- worktree での最終確認: **1376/1376 緑(GPU 直列含む)**、WP374a は GPU 実行体に 6 本・非 GPU 0 本
- 分離後の受け入れ: `raytracing_rt3_1_production_test`(`GPU` 登録)+ 共有 `raytracing_rt3_1_fixture.hpp`
- 統合側の全数は merge 直後に別途実行して次行に記す
- 統合側(main、`SKIP_DEVSTUDIO=OFF`、projects dir、GPU 直列): **1515/1515 緑**(1502 + WP374 7 + WP374a 6)。
  基準値は **1515** に更新


#### WP377 仕様レビュー第 1 巡(6 件)と v2 —— **v2 が最終(1 巡 + 決定差分)**

**v2 が本節の正。v1 の「決定」「受け入れ」は v2 で置換される。** 数え方の訂正: RT の override 型は
**5 型・6 使用箇所**(v1 の「6 seam」は使用箇所数)。

##### 決定 1(#1): **既存欠陥を直す** —— resident BLAS の再利用が profile 検査を素通りする

第 1 巡が本番入力で到達可能な穴を見つけた(私も現物で確認した):

```
if (can_reuse) { … if (existing) { candidate->blas.emplace(...); continue; } }   // :859-866
…
validateRayProfileAccelerationStructureFlags(material_alpha_profile_active, geometry_flags, {}, …)  // :879
```

**resident な variant は `continue` で validate に到達しない。** layout v2 で `geometry_flags:["opaque"]`
を一度 render し、flags を変えずに `portable_profile` だけを hot reload すると、
**opaque BLAS が再利用され `ray_profile_opaque_as@1` が出ない**(profile 用 any-hit が有効なのに BLAS は opaque)。

- **修正**: validate を **reuse 判定より前**へ移す(required variant すべてに対して、resident でも検査する)
- **受け入れ(順序を固定)**:
  1. profile ON + nonopaque → render 成功、resolved / resident flags が 0
  2. profile OFF + opaque → render 成功、resident flags が opaque
  3. **flags を変えず profile だけ ON** → layout の stage は成功し、**`Renderer::render` が
     `ray_profile_opaque_as@1` + `BLAS geometry` で失敗**
  失敗を最後に置くのは、現テストが意図的 abort 後の再 render を避けている制約に合わせるため
- **変異**: validate を reuse の後ろへ戻すと 3. が通ってしまい**赤にならない → この受け入れが落ちる**

##### 決定 2(#2): authored で置換できるのは **2 つの実効値**。TLAS geometry の検査は**廃止**

現 seam は BLAS geometry / TLAS instance / TLAS geometry の **3 注入点**用だが、authored 入力が届くのは:

| 注入点 | authored 入力 | 扱い |
|---|---|---|
| BLAS geometry flags | `geometry_flags`(material 鍵規則) | **本番入力で検査** |
| TLAS instance flags | `ray_instance.instance_flags`(`force_opaque`) | **本番入力で検査**。profile active なら row resolver が先に同コードで拒否する([raygrouplayout.cpp:1292](../src/core/shader/raygrouplayout.cpp#L1292))ので、その位置で検査する |
| TLAS instances geometry flags | **無い**(test override だけが供給源: [accelerationstructure.cpp:1039](../src/core/renderer/accelerationstructure.cpp#L1039)) | **validation call と diagnostic を削除**。到達不能な検査を残さない |

規範は「内部 3 call site の保持」ではなく「**利用者が指定できる 2 種類の実効値**」。

##### 決定 3(#3): WP368 の `[production]` は**外さない**。規範を tag ではなく意味で書く

tag を外しても実行経路は 1 行も変わらず、headless GPU の唯一の production case が台帳上消えるだけ。
Catch の tag は TEST_CASE 全体にしか付かないので「非 override 部分だけ production」は表現不能。

**規範(意味条件)**: override 型 seam は
- **明示した負側だけ**で使い(成功値を供給しない)、
- **production の正常値は override 無効の前後で実測**し、
- injected 状態で production の正常値を主張しない。

WP368 は既にこれを満たす(負側 scoped、解除後の復帰、その後 purge 検査)。**tag を残す。**
rt1b の sentinel 4 使用箇所も同じ意味条件で残す(tag による差別はしない)。
**単に tag を外すことを禁止する。**

##### 決定 4(#4): 受け入れは**削除の全長**と **M3/M4 の再実行**

- `RayProfileAccelerationStructureFlagsOverrideForTesting`(payload struct)、scoped class、内部 `thread_local`、
  ctor/dtor、**5 読取点**(reuse 判定 / BLAS key / BLAS build / TLAS instance / TLAS geometry)を**全部削除**
- **WP374 の M3(cache 鍵から flags を消す)・M4(Vulkan に空 flags)を WP377 後に再実行し、赤**
- 「旧 seam を戻すと偽陽性」は**完了記録の理由に降ろす**(受け入れではない)
- WP369 の置換は、invalid layout の **stage 成功後に `Renderer::render` で失敗**することを要求
  (loader/parser で同じ文字列を投げても通らない)

##### 決定 5(#5): 「全数緑」に**実行された証拠**を要求する

WP369 case は `PELICAN_RUNTIME_SHADER_COMPILER` で囲まれ、GPU 無しなら SKIP、compiler OFF なら SUCCEED。
- **compiler ON + 対応 GPU で対象 case が SKIP されず実行された**ことを必須(ctest の出力で)
- compiler OFF は別に build + full を要求
- OpenXR / IMGUI / SKIP_DEVSTUDIO は compile matrix(runtime 分岐は shader compiler だけ)

##### 決定 6(#6): fault seam は**名前でなく意味条件**で例外化する

「判定を差し替える seam は次だけ」は広すぎた。未分類:
`RayTracingOracleIntersectorForTesting`(intersection 判定の差し替え: [raytracingoracle.hpp:146](../src/core/shader/raytracingoracle.hpp#L146))、
`WindowWsiFaultScriptForTesting`(WSI の Vulkan result 差し替え: [swapchainrecovery.hpp:103](../src/core/vkcore/swapchainrecovery.hpp#L103))、
scene save hook(production `saveSceneDocument` 内で任意 callback、常時コンパイル: [basicconfig.hpp:142](../src/core/loader/basicconfig.hpp#L142))。

**fault seam の例外条件**(全部満たすこと):
1. 失敗または外部競合**だけ**を注入し、成功結果を供給したり production 判定を bypass しない
2. 同じテストに **no-fault の成功側**がある
3. injected 状態で production の正常値を主張しない
4. **全 fault seam を表に載せる**(WSI・oracle・scene hook、Catch tag 外の CTest integration も)

`RayTracingOracleIntersectorForTesting` は判定の差し替えなので **override 型**として扱う
(決定 3 の意味条件: 負側のみ)。

##### 受け入れ(v1 を置換)

- 決定 1 の 3 段順序テスト(GPU、compiler ON で実行された証拠つき)。validate の位置を戻す変異で赤
- authored `geometry_flags` と `instance_flags` の 2 実効値が本番入力で `ray_profile_opaque_as@1` を出す
- TLAS geometry の validation call が消え、到達不能な diagnostic が無い
- AS override seam の**全削除**(struct / scoped / thread_local / 5 読取点)。M3・M4 の再実行が赤
- WP368 は `[production]` のまま、意味条件を満たす(負側のみ、前後の実測、purge)
- fault seam の表(意味条件 4 つの充足を各行に)
- 全数 1515 を壊さない / compiler OFF 別 build / compile matrix

##### WP377 fault seam 棚卸し(完了報告)

| fault seam | 1. 失敗 / 外部競合だけ | 2. 同じテストの no-fault 成功側 | 3. injected 状態で正常値を主張しない | 4. 帰属テスト / integration |
|---|---|---|---|---|
| `setSceneSaveFaultForTesting` | SAVE0 の prepare 各点で例外だけを注入 | seam を `nullopt` に戻した後、同じ case で `saveSceneDocument` が成功 | 注入中は旧 cache / revision / bytes と一時ファイル 0 だけを検査 | `SAVE0 is failure-atomic at every prepare point and reloads semantically` |
| `setSceneImportFaultForTesting` | SNAPSHOT0 の candidate prepare / publication 後の失敗だけを注入 | seam を `nullopt` に戻した後、同じ case で `importSceneDocument` が成功 | 注入中は rollback された旧 document / resolver / disk bytes だけを検査 | `SNAPSHOT0 cache replacement is rollback-safe and never writes the scene file` |
| `setSceneSaveHookForTesting` | production save 中の prefab 外部変更だけを起こし、成功結果を供給しない | hook を空に戻し prefab を復元した後、同じ case で `saveScene` が成功 | hook 使用側は `scene_dependency_changed@1` と rollback / purge だけを検査 | `WP362 save baseline rejects prefab changes before and throughout SAVE0` |
| `WindowWsiFaultScriptForTesting` | parser が `success` を拒否し、Vulkan failure result だけを順序注入 | 同じ 180-frame process が注入境界間の実 WSI 成功を通り、fresh-surface publication を完了 | integration は recovery / script 全消費を主張し、注入値を production の成功値として扱わない | Catch の `WP217 ordered WSI fault scripts consume only the next matching boundary` と Catch 外 CTest `wsi_fault_window_player` (`run_wsi_fault_window.cmake`) |
| `RayTracingOracleIntersectorForTesting` (**override 型**) | inv-determinant sign bug と契約外 interval の負側だけを供給 | 各同一 case で normative intersector / validator の成功を override の前後に実測 | override 結果から production の正常 hit / interval を主張しない | `CPU RT oracle constructively covers hit miss and recursion sentinels` / `CPU RT oracle exercises both sides of every numeric-contract boundary` |

##### やらないこと

観測 seam の削減 / WSI fault script と scene hook の設計変更(表に載せるだけ)/ rt1a・rt1b の sentinel の書き換え。


#### WP376 仕様レビュー第 1 巡(7 件・破綻 3)と v2 —— **v2 が最終(1 巡 + 決定差分)**

**v2 が本節の正。v1 の「実測」「決定」「受け入れ」は v2 で置換される。**

##### 訂正 1(#1): 母集団の数え方が誤っていた

「41 箇所」は `ResolvedSceneResolver::resolve(` の**生ヒット**で、テスト呼び出しは 39(現在 40)+ 本番 caller 1 + 定義 1。
さらに数えていなかった母集団が 2 つある: テスト内の **`prepare(` 直叩き 4** と、
**`.resolved_scene =` の initializer 20 箇所 / 5 ファイル**。分類も「downstream validator 用 fixture」
(WP360 physics・RT3-1 の resolved-scene fixture)が抜けていた。

**WP375 統合後に、2 つの表を file:line・テスト名つきで固定する**:
1. `resolve` と `prepare` の全 test caller
2. `.resolved_scene` と `.projection_state` の全 test initializer

分類は **5 種**: `resolver unit` / `oracle` / `downstream fixture` / `service double` / `production composition`。

##### 訂正 2(#2): 2 つの手製 provider は「写経」ではなく **service-boundary の test double**

本番の query provider は resolver を呼ばず、`ProjectBasicConfig::resolvedScene()` の参照を返すだけ
([editorruntimefactory.cpp:1748](../src/core/communication/editorruntimefactory.cpp#L1748))。edit/preview の provider は
型すら違い `SceneProjectionState` を返す。`ProjectBasicConfig` は revision と resolver generation を**別 authority**として
管理し、generation だけを進める `refreshResolvedScene` を持つ([basicconfig.cpp:632](../src/core/loader/basicconfig.cpp#L632))。

**決定**: 手製 provider は **service / RPC 単体の test double として許容**する(方針 (1) は条件付きで維持)。
ただし根拠は「写経」ではなく「明示 dependency の double」であり、**20 initializer 全件を「受け入れではない」と表に固定**する。

##### 訂正 3(#3): 「古い generation」の変異は観測できない。**本当の不一致は SnapshotV2 export にある**

- non-prefab の query は `scene_revision` しか出さず、resolver generation は wire に出ない
- prefab では手製 provider 型の `resolve(document, generation)` が registry/provider を渡さず、`PrefabProviderStale` で先に落ちる
- 出荷 4 project と `default_config.json` は scene v1 で `prefabs` を持たない

**差し替え**: 検出すべきは変異ではなく**実在する不一致**。`EditorCommandService::resolved()` の consumer は query だけでなく
`scene_tree` / `get_scene_revision` / SnapshotV1・V2 export / V2 import validation / generated selector validation にあり
([editorcommandservice.cpp:695](../src/core/communication/editorcommandservice.cpp#L695)・:704・:802・:856・:994・:1198)、
**SnapshotV2 export は `document()` と `resolved()` を別々に取得し、前者の revision と後者の prefab closure を
1 つの response に載せる**(:802 / :856)。これが WP376 が閉じるべき欠陥である。

##### 決定 A(#4・#5): 全 `resolved()` consumer を **単一の `SceneProjectionState` provider** に統合する

- WP375 の `EditorProjectionContext` を丸ごと共有する案(方針 (2))は**却下**。
  `EditorProjectionContext` の generation は codec/layout/behavior/default のものであり
  `SceneResolverGeneration` を含まない。`active_scene_id` / `mode` は planner 固有で、query は他 scene を明示できる
- **分離する**: planner 固有 `{active_scene_id, mode}` と、共有可能な **immutable environment snapshot**
  (codec policy catalog / layout mode・generation / behavior registry・generation / canonical defaults)。
  **WP375 は既に実装中**なので、WP375 が monolithic な context で着地した場合は **この分離を WP376 の範囲で行う**(WP375 v3 差分 2 の精密化)
- query は **`{SceneProjectionState, environment snapshot, expected generation}` を 1 回で取得**し、
  document と resolved を別 callback で読む箇所を**全部**その provider に置換する
  (表: 上記 6 consumer)。SnapshotV2 export の **semantic bytes・revision・closure が 1 つの state から出る**

##### 決定 B(#6): production acceptance は **WP374a の GPU fixture を名指しで拡張**する

factory は `PolygonInstanceContainer` / `LightContainer` / `Renderer` を無条件に取得するので **GPU が要る**。
新しい CPU テストを「1 本足す」のではなく、既存の本番経路 fixture
([raytracing_rt3_1_production_test.cpp:195](../test/raytracing_rt3_1_production_test.cpp#L195): project 構成後に device 要求 →
SceneLoader → 実 `RpcServer` → `makeEditorRuntimeService` → commit hook → save → fresh factory reload)を拡張する。
`GPU` label、project → device の順序(WP374a 回収で踏んだ罠)を明記。

##### 決定 C(#7): §4 規約 10 と matrix

同じ実 RPC テストで **edit 前の exact resolved 値 + generation / edit・refresh 後の値 + generation / 両者が異なる**の 3 点。
特に **`refreshResolvedScene` で generation だけを進めた後**、SnapshotV2 export の revision と closure が
**同じ state から**出ること(統合前は別 callback なので食い違う = 対照)。

matrix は **WP375 v3 を継承**: IMGUI ON/OFF(factory は ImGui からも使われる:
[imguisystem.cpp:317](../src/core/imgui/imguisystem.cpp#L317))、runtime compiler ON/OFF、OpenXR compile、
`PELICAN_WITH_RPC` OFF 時の factory / ImGui query smoke、`SKIP_DEVSTUDIO` 両構成。

##### 受け入れ(v1 を置換)

- 2 つの母集団表(5 分類、file:line・テスト名)が節に固定される
- 6 consumer が単一 provider を呼ぶ。**document と resolved を別 callback で読む箇所が 0**
  (残っていれば下の対照が落ちる)
- **SnapshotV2 export**: `refreshResolvedScene` 後に revision と closure が同一 state 由来。
  別 callback に戻す変異で食い違って落ちる
- 実 RPC で edit 前後の exact resolved 値と generation が異なる(規約 10 の両側)
- WP374a GPU fixture の拡張(名指し)。`GPU` label、project → device 順
- 出荷 4 project・`src/core/resources/` の bytes を**変更しない**(要求してはならない)
- 全数(着手時に実測)を壊さない / 決定 C の matrix

##### やらないこと

resolver 単体・オラクル・downstream fixture の移設 / 手製 double の削除 / Studio。

#### 設計トラックの文書(2026-09-02 起票。いずれも敵対レビュー待ち / 進行中)

| トラック | 文書 | 段 |
|---|---|---|
| object identity(#4) | [design_object_identity.md](design_object_identity.md) | ID-1(ObjectRef と名前解決の 1 関数化)→ ID-2(map の key を id へ、transient 分離)→ ID-3(Physics / Animation / Controller)→ rename WP |
| 例外契約(#5) | [design_error_contract.md](design_error_contract.md) | E-1(camera / animation)→ E-2(collider / sprite)→ E-3(atlas)→ E-4(その他を再送出)→ E-5(見える場所) |
| 宣言の担体(#10) | [design_declaration_carrier.md](design_declaration_carrier.md) | C-1(可変長宣言)→ C-2(行を作れる編集)→ C-3(authoring → row の join)。RT4/5/7 と R5 の前提 |
| Studio 汎用 component UI(#9) | [design_studio_component_ui.md](design_studio_component_ui.md) | S-1(追加・削除。今すぐ可)→ S-2(offline catalog)→ S-3(行編集。C-2 の後) |

順序は計画どおり WP375 → WP376 → WP377 → identity / 例外契約 → 担体 → Studio。
**S-1 は前提が無いので、WP377 の後に前倒しできる**(`ray_instance` を Studio から足せない状態を早く終える)。

#### 設計レビューが見つけた既存欠陥 2 件(2026-09-02。WP 化の所在)

| 欠陥 | 本番入力での再現 | 所在 |
|---|---|---|
| **resident な opaque BLAS が profile 検査を素通りする** —— `recordFrame` の reuse `continue` が `validateRayProfileAccelerationStructureFlags` より前([accelerationstructure.cpp:859](../src/core/renderer/accelerationstructure.cpp#L859)・:879) | layout v2 で `geometry_flags:["opaque"]` を render → flags を変えず `portable_profile` だけ hot reload → `ray_profile_opaque_as@1` が出ない | **WP377 v2 決定 1**(実装中) |
| **scene load の publish 途中で失敗すると世代が混在する** —— runtime clear 直後に Light/Camera を publish し、behavior activate が投げても旧 prepared state を戻さない([scene.cpp:369](../src/core/loader/scene.cpp#L369)・:546・:584)。snapshot import も runtime を reload しない([basicconfig.cpp:734](../src/core/loader/basicconfig.cpp#L734)) | scene A 稼働中に snapshot B を import し B の behavior `onInit` が投げる → document は A、Light/Camera は B、ECS は途中 | **object identity 設計 v2 の ID-1**(scene-runtime transaction) |

どちらも「機能を足そうとして、足す先の本番経路を読んだら見つかった」もの。
テスト品質計画の主張(受け入れが本番経路を通っていない)の傍証である。

#### WP374 の後追い負債(2026-09-02、担体設計 v2 決定 9'): `ray_instance` → `pelican_ray_instance` の改名

version gate は「外部 `ray_instance` を持つ project が layout v2 を有効化すると builtin 登録で衝突する」逆向きの問題を残す
(現テストが LayoutV2 で builtin の逆登録が throw することを固定している: [raytracing_rt3_1_test.cpp:289](../test/raytracing_rt3_1_test.cpp#L289))。
**未出荷の今が最安。** `pelican_` 予約は `ComponentInfo.owner` で owner-aware に。WP377 の後、小 WP として起票する。



#### WP377 完了記録(統合側。2026-09-02、`8b216770`)

- **統合側の変異 3 本(エージェントは GPU 禁止のため build までで UNMET 報告)**: V(reuse 前の profile 検証を消す)/ M3(BLAS cache 鍵から
  flags を消す)/ M4(Vulkan に空 flags)—— いずれも **`WP374a F G F geometry flags rebuild exact resident keys and change any-hit on GPU`
  または `WP369 production material alpha profile …` が赤**。復元後の残留 0
- **全数**: worktree 1377 件(WP334b の 3 件が `-j4` 負荷で commit 待ちタイムアウト、単独再実行 10/10 緑)/
  統合後 main **1516 / 1516 緑**(`SKIP_DEVSTUDIO=OFF`、projects dir、GPU 直列)
- `RayProfileAccelerationStructureFlagsOverrideForTesting` は payload・guard・thread_local・全読み取り点とも消えた。到達不能だった TLAS-geometry 検証も削除
- コードレビュー `rev377`: 実害 1(拒否世代の SBT 残留)+ 受け入れ弱化 3 → **WP377a** として起票・投入。#6(設計文書の「現状」表)は docs で処理
- **構成 matrix(コードレビュー #5 の要求。統合側で取得、`d875e58b` 時点)**:
  compile: OpenXR OFF / ImGui OFF / `SKIP_DEVSTUDIO=ON` とも **build 0 error**。compiler ON + GPU: 対象 case(`WP377 authored ray_instance force opaque …`)**非 SKIP で Passed**、
  WP377 の 3 段 sequence(`WP369 production material alpha profile …` 内)は全数 1516 / 1526 に含まれ Passed。
  **compiler OFF の全数: 1363 件中 29 失敗**。うち **4 件は WP375 の新 acceptance a〜d**(compiler OFF の分岐を持たない → WP375a で修正)。
  残り **25 件は WP375 / WP377 が触らない GPU テスト**(`hybrid_v1 preset` / `WP218-219` / `WP238e` / `WP357a` / ray-query 群 / studio 系 …)で、
  rendering config の load が GLSL source を要求して落ちる(`Shader defines and generated includes require GLSL source, not SPIR-V`)。
  compiler OFF の基準値は台帳に無く「既存」は推定。**compiler OFF で GPU テストが guard されていない**のは suite の既知の穴として記録(WP 化は別途判断)

### WP377a: 拒否された世代の SBT が frame resources に残る(WP377 コードレビューの実害 1 件 + 受け入れ弱化 3 件)

**§4 規則 11 中段。コードレビューのみ(§10)。見積: 小。WP377 → 377a の 1 段で打ち切る(連鎖上限)。**
**依存: なし(WP377 統合済み `8b216770`。WP375 と触るファイルが重ならない)。**

#### 何が壊れているか(コードレビュー `rev377` が制御フローで確認。連続描画の破壊までは未確認)

`renderer.render()` は **`recordFrame()` より前に** SBT を active slot に入れ([renderer.cpp:4751](../src/core/vkcore/renderer.cpp#L4751))、
その後の BLAS 検証が throw する([accelerationstructure.cpp:840](../src/core/renderer/accelerationstructure.cpp#L840))。AS の commit は frame 成功後だけ
([renderer.cpp:5570](../src/core/vkcore/renderer.cpp#L5570))なので runtime generation と AS は旧状態に戻るが、
**`FrameResources` は拒否された profile-ON 側の SBT を保持したまま**、次の `beginLogicalFrame` まで reset されない([frameresources.cpp:417](../src/core/renderer/frameresources.cpp#L417))。
WP377 のテストは throw 直後に終わり、この状態を検査していない([headless_render_test.cpp:12619](../test/headless_render_test.cpp#L12619))。

#### 直すこと

1. **SBT の activation を `recordFrame()` 成功後へ移す**(または例外時に旧 lease を復元する guard)。どちらを選んだかと理由を報告に書く
2. **ON→ON・非 resident の負側**を production reload test に足す(レビュー #2): profile ON + material rule nonopaque で render → profile ON のまま同じ rule だけ
   `geometry_flags:["opaque"]` に変更(新しい opaque key は非 resident)→ stage 成功後の render が **同じ名前付きエラー**で失敗。
   正常側では resolved row と resident key が nonopaque であることを同じテスト内で検査
3. **validator に数値 identity を渡し、違反時だけ診断文字列を作る**(レビュー #3。reuse fast path で毎 frame `std::to_string` していた)
4. **RT oracle の境界テスト**(レビュー #4): WP377 が削った accepted seam 値 `0.00999` の代わりに、**production intersector で境界直下になる幾何**を作り、
   返された `hit.t_interval` の実効相対幅が境界直下で**成功**することを検査する([raytracing_rt1a_test.cpp:2032](../test/raytracing_rt1a_test.cpp#L2032))。
   上限を `1e-2` → `1e-3` に変えると落ちる(対照)

#### 受け入れ(§4 規約 10)

- **失敗直後の状態**: WP377 の ON→OFF→ON 系列で最終 render が失敗した直後に、**active SBT lease が旧世代のもの**、AS が旧、staged generation が破棄されている。
  さらに**その次の frame が旧世代で正常に描ける**(golden 一致)。SBT 移動(1)を戻すと lease が新世代で落ちる
- (2)(3)(4) の各対照: (2) reuse lookup の前の検証を「resident のときだけ」に変える変異で落ちる / (3) は挙動不変(全数緑)で、
  validator の signature 変更が診断文字列を含むことをテストで検査**しない**(代理条件) / (4) 上限変更で落ちる
- 全数(着手時に実測)/ `SKIP_DEVSTUDIO` 両構成 / 出荷 4 project bytes 不変 / golden 不変 / GPU は統合側で直列

#### やらないこと

レビュー #5(構成 matrix の証拠)は統合側で取る(WP377 完了記録に追記)。#6(設計文書の「現状」表)は docs で処理済み。

#### WP377a 完了記録(統合側。2026-09-02、`a3f318fb`)

- **実装**: SBT activation を `recordFrame()` 成功後へ移動 + `beginLogicalFrame` の無条件 lease reset を廃止(「最後に記録成功した世代を active とする」構造)。
  validator は数値 identity を受け取り違反時だけ診断文字列を構築。production reload test に失敗直後の状態(runtime generation / staged=0 / TLAS / resident BLAS / 旧 SBT lease)、
  次 frame の旧 golden 一致、ON→ON 非 resident の負側を追加
- **継続で露出した実装欠陥**: 統合側 GPU で基準(変異前)が `ray_as_row_material_identity_unavailable@1` で赤 → resolver([raygrouplayout.cpp:1257](../src/core/shader/raygrouplayout.cpp#L1257))が
  **完全な asset identity を持つ row を、document 側の片割れ(埋め込み material 名だけ)を理由に拒否**していた。仕様どおり「document 組 または asset 組」に修正。
  新たに受理されるのは「partial document + full asset」だけで、両方欠ける row は今も拒否(回帰テスト追加)
- **第 4 項(RT oracle 境界)の裁定**: 本番 intersector の実効相対幅は `8.94e-7`(最大 `1.19e-6`)で、上限 `1e-2` には本番幾何で到達できない。
  条件を「**本番経路で返る `hit.t_interval` の実効相対幅 ≤ 1e-5**」に差し替え(拒否側 `0.01001` は据え置き)。対照: 幅を 100 倍にする変異で `8.94e-5` となり赤。
  `1e-2 → 1e-3` の変異が生き残るのは正しい(検出対象外)
- **統合側の GPU 変異**(エージェントは GPU 禁止で UNMET 報告): 基準 1/1 緑 → **M1**(`beginLogicalFrame` で lease を無条件 reset = 旧挙動)赤 /
  **M2**(検証を resident reuse のときだけに)赤。残留 0
- 全数(統合後 main、`SKIP_DEVSTUDIO=OFF`、projects dir、GPU 直列): **1526 / 1526 緑**
- コードレビュー `rev377a`(再投入、fast 無し): **Reject**。(1) 実害: `recordFrame()` 後・frame transaction 確定前(`after_as_record` / `submit` の fault)に却下世代の SBT が active に残る(RT→非 RT の clear も同型。TLAS descriptor も frame descriptor に書かれた後)。(2) identity 緩和のうち `D=1,N=0,A=1`(document あり・name 無し・asset あり)は gate を通るが後段の material lookup(`raygrouplayout.cpp:309`)と不整合で `ray_material_key_mismatch` に落ちる。(3) compiler OFF / XR / `SKIP_DEVSTUDIO=ON` の受け入れ不足。**連鎖上限(WP377 → 377a)により WP377b は起票せず、既知の穴に修正設計ごと記録**(下の「未了項目」節)。壊せなかった項目: M1 / M2 の変異はいずれも噛む、oracle 正側は production intersector 経由、出荷 4 project は RT 未使用
### WP378: E-0a —— 利用者データ拒否を「値」にする最初の縦切り(型 + 6 caller + camera + `get_components`)

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 中〜大。**
**規範は `docs/design_error_contract.md`(v4 = §8 が最終)。**下の **v3 差分が本節の正**(v2 で範囲を絞り、v3 で「値を足すだけ、例外経路は消さない」に確定)。
**依存: WP375a 統合後(RPC guard を取り込む)。E-3a / E-3b は本 WP 統合後に切る。**
`editorruntimefactory.cpp:352` / `editorprojectiontransaction.cpp:113,499` は WP375 が動かす。着地後に行を実測し直す)。**

#### 何が壊れているか(利用者に見える形)

camera の値を間違える(`zfar <= znear`、projection 切替時の `yfov` 欠落 等)と、返るのは**型無しの文字列**で、
`get_components` には**何も出ない**。内部 invariant の破れ(`logic_error`)も同じ文字列経路に流れ、利用者の入力ミスに見える。

#### 起票時の実測(2026-09-02)

- `decodeAuthored` の **production caller は 6**:
  [editorjournal.cpp:221](../src/core/communication/editorjournal.cpp#L221)(canonicalize)/
  [editorruntimefactory.cpp:352](../src/core/communication/editorruntimefactory.cpp#L352)(component migration adapter)/
  [editorpreviewprojection.cpp:266](../src/core/loader/editorpreviewprojection.cpp#L266)(**`(void)` で結果を捨てている**)/
  [editorprojectiontransaction.cpp:113](../src/core/loader/editorprojectiontransaction.cpp#L113)(transform)/
  [editorprojectiontransaction.cpp:499](../src/core/loader/editorprojectiontransaction.cpp#L499)/
  [resolvedscene.cpp:207](../src/core/loader/resolvedscene.cpp#L207)(resolver)。test の直接 caller は 18
- `ComponentCodecValue` は `std::any` の alias([componentcodec.hpp:137](../src/core/loader/componentcodec.hpp#L137))。
  copyable な Result は `std::any::any(T&&)` で**格納できてしまう**
- camera の 4 拒否は decode 後の **default completion**(resolver 所有)で出る([resolvedscene.cpp:217](../src/core/loader/resolvedscene.cpp#L217)〜:285)
- 保持型は `std::optional<std::string>`([resolvedscene.hpp:212](../src/core/loader/resolvedscene.hpp#L212))
- query の serializer は `runtime_resolution_error` を出さない([editorcommandservice.cpp:635](../src/core/communication/editorcommandservice.cpp#L635))

#### 1. 型(設計 v4 修正 1)

- **`ResolutionResult<T>`: move-only**(copy ctor / copy assign を削除)。error は `UserDataViolation`
- **compile-contract**(受け入れ): 
  `static_assert(!std::is_constructible_v<std::any, ResolutionResult<X>&>)` / `…&&` /
  `!std::is_assignable_v<std::any&, ResolutionResult<X>&>` / `…&&`
- **`std::optional<StoredResolutionFailure>`**、`StoredResolutionFailure = variant<UserDataViolation, LegacyResolutionText>`。
  `runtime_resolution_error` をこれに置換。**未移行の拒否は投げ続け、resolver の catch が `LegacyResolutionText` にする**
- `UserDataViolation { code, tokens[], leaf_message, source{kind, document_id, pointer} }`。
  **pointer は token 列で保持し、RFC6901 の escape は serialization 時**(dotted path を作らない)
- **`StructFieldValidationError` の基底・ctor・`code()/path()` は変えない**。game-logic ABI は上げない

#### 2. 6 caller の移行(1 件ずつ)

| caller | 扱い |
|---|---|
| resolver `:207` | seam の本体。decode + **completion** を `ResolutionResult` で受け、camera 4 + alias を `UserDataViolation` 化 |
| journal `:221` canonicalize | 失敗を **既存の `schema_violation`** に写像(wire 不変) |
| runtime factory `:352` | 失敗を **`UserDataViolation`** として adapter prepare の失敗値へ(WP375 着地後の形に合わせる) |
| preview `:266` | **`(void)` をやめて result を検査**。失敗なら preview を名前付きで拒否 |
| transaction `:113` / `:499` | 失敗を `EditorProjectionException` の既存 code に写像(wire 不変) |
| test 18 | 全部 `ResolutionResult` を unwrap する形へ |

**unused-result を error にする**構成(`-Werror` 相当の compiler flag を core の test target に)。`(void)` は禁止。

#### 3. `legacy_branch_id` 表(設計 v4 修正 3)

decoder の**拒否分岐ごと**に ID を振った表を **`docs/` に生成物として置く**(手書きではなく、拒否分岐を列挙する
テストが出力し、差分があれば落ちる)。E-0a で typed へ移すのは camera 4 + alias。残りは `legacy_branch_id` 付きで表に残る。
**「producer 0」はこの表が空になることであり、count ではない。**

#### 4. `get_components` の可視化(設計 v4 決定 4' の最小)

component に **`resolution_error {code, source, message}`** を出す(typed のみ。legacy は `{legacy_message}`)。
**新 schema の新 golden**。既存 golden と「一致」させない(設計 v3 決定 9'')。

#### 受け入れ(§4 規約 10)

- **同一テストの両側**(実 RPC): project default `zfar=1000`、generated camera が `zfar` 省略、prefab parameter で `znear=5000`
  → `get_components` に `code = camera.zfar_not_greater_than_znear@1`、`primary_token = znear`、`related_tokens = [zfar]`
  (設計 v4 修正 8 の規範表)。**`znear` を直すと error が消え、本番 resolver が解決した camera 値が出る**
- **inactive 契約**: typed(camera)と legacy(collider の `radius:-1`)が**同じ inactive scene に同居**して保持され、
  save / query が通り、current 切替で **camera は code つき、collider は legacy message** で落ちる
- **compile-contract 4 本**が通る。`ResolutionResult` に copy を足す変異で **static_assert が落ちる**
- **preview**: 不正 component の preview が**名前付きで拒否**される(今日は `(void)` で受理 = 対照)
- **leaf_message の byte 一致**: 移した camera 4 + alias の leaf が変更前と同じ bytes(既存の文脈付き `what()` も)
- **変異**: result の unwrap / resolver の配線 / serializer のどれか 1 つを消すと落ちる
- **ABI**: 旧ヘッダで作った client が link し動く(`StructFieldValidationError` の layout 不変を static_assert)
- **`legacy_branch_id` 表**が生成され、camera 4 + alias が typed 側に移っている
- 出荷 4 project の bytes 不変 / 全数(着手時に実測)を壊さない / `SKIP_DEVSTUDIO` 両構成 / IMGUI ON/OFF / compiler ON/OFF

#### やらないこと

E-0b(animation / ray_instance)/ E-0c(behavior 値化・v1 adapter)/ E-1〜E-4 / 診断 registry(E-2b)/
launcher の `PELICAN_DIAG` / terminal `error.data` / `commit` の `noexcept` 変更(E-3a)。

#### 設計トラックの到達点(2026-09-02 夕。各文書の最終版と最初の WP)

| トラック | 文書の最終版 | 段(最終) | 最初の WP | 前提 |
|---|---|---|---|---|
| object identity(#4) | [design_object_identity.md](design_object_identity.md) **v3(§8)**。3 巡目のレビューは枠待ち | ID-A(型移設・registry・WP375 participant・全 name entry の canonical adapter)→ **ID-B(scene transaction)** → ID-C(内部 consumer の ref 化)→ ID-D(public ref API + Animation V2 ABI、互換 release)→ ID-E(breaking release で name API 除去)→ rename WP | **ID-B**(既存欠陥「publish 途中失敗で世代混在」を直す。ID-A 無しで価値あり) | 無し(ID-A は WP375 着地後) |
| 例外契約(#5) | [design_error_contract.md](design_error_contract.md) **v4(§8)。設計段は 3 巡で打ち切り** | E-0a → E-0b → E-0c → E-1 → E-2 / E-2b(診断 registry)→ E-3a → E-3b → E-4 | **WP378 = E-0a**(起票済み。move-only Result + 6 caller + camera + `get_components`) | **WP375 着地後**(6 caller のうち 3 が WP375 の動かす場所) |
| 宣言の担体(#10) | [design_declaration_carrier.md](design_declaration_carrier.md) **v4(§8 が最終)**。第 3 巡で GLABI-2 の起票ゲート 6 条件を確定。SCHEMA-TX と RT3 は 1 縦切りに統合。次は WP381(GLABI-2) | CARRIER-1(C-1+C-2+C-3 + `pelican_ray_instance` 改名。消費者 = RT3 の per-geometry override)→ CARRIER-2(Studio 汎用 UI) | 2 巡目の結果待ち | WP377(改名は WP374 後追い) |
| Studio 汎用 component UI(#9) | [design_studio_component_ui.md](design_studio_component_ui.md) **v1**。1 巡目レビュー走行中 | S-1(追加・削除)→ S-2(offline catalog)→ S-3(行編集。CARRIER-1 の後) | 1 巡目の結果待ち | S-1 は無し(前倒し可) |

**実装の順序(改)**: WP375 → WP376 → WP377 → **ID-B** / **S-1**(前提無し。並走可)→ WP378(E-0a)→ ID-A → CARRIER-1 → …



#### WP378 仕様レビュー第 1 巡(不合格・10 件)と v2

仕分け: **直す 8**(#2 #3 #5 #6 #7 #8 #9 #10)/ **範囲を訂正 2**(#1 #4)。前提の再実測: 本番 caller **6**(journal :223 / factory :372 / preview :266 /
transaction :114・:528 / resolver :207)、直接 test caller **18**(componentcodec_test 7 / raytracing_rt3_1_test 6 / wp361_prefab_test 5)。行番号は v1 から移動している(上記が正)。

##### 決定 1(#1、訂正): generation guard の内部 / 利用者分類は **E-3a** の所有。E-0a は触らない

WP375a の「やらないこと」で本 WP に割り当てた generation guard(`binding_state.codec_generation` を execute 直前に上書きし、不一致を `schema_violation` に落とす)は
**`commit noexcept` と guard の再設計を伴う E-3a の領域**。E-0a の目的から「内部 / 利用者分類を直す」を**削り**、目的を「利用者データ拒否を値にする(6 caller)」に限定する。
WP375a の記述は「E-3a で扱う」に訂正(本差分と同時)。

##### 決定 2(#2): `ResolutionResult<T, E = UserDataViolation>`。producer を先に 3 分し、表は **producer site 単位**

- codec の decode は `E = UserDataViolation`。projection の plan は **`E = ProjectionPlanFailure`**(規範 §8 のとおり別型)
- **producer の分離を先に行う**: 利用者入力 → `UserDataViolation` / 列挙済み recoverable → 専用型(`ProjectionPlanFailure` 等)/ invariant → `logic_error` をそのまま再送出。
  6 caller の範囲内では **内部 producer に `CommandInvalid` / 汎用 `runtime_error` を使うことを禁止**する(`editorprojectiontransaction.cpp:528` の decode 失敗は `UserDataViolation` 経由に移す)
- 3 分類表は **code 単位ではなく producer site 単位**(file:line と producer 種別)。同一 code に複数分類が混在する現状(`CommandInvalid`)はこの分離で解消する

##### 決定 3(#3): 6 caller は **`Result.error` だけ**を利用者エラーへ写像し、throw は捕捉しない

- `resolvedscene.cpp:301` / preview / transaction の **decode 境界の broad catch を外す**(`logic_error` / `bad_any_cast` / callback 欠落は内部として伝播)。
  decode 境界以外の broad catch は範囲外(記録)
- fault test: decoder callback 欠落 / encoder callback 欠落(`encode_canonical == nullptr`)/ 内部 `logic_error` → **6 caller すべてで内部として伝播**(利用者エラー・legacy 文字列にならない)

##### 決定 4(#4、訂正): provenance は E-4。E-0a は **同一文書内の camera エラー**だけを扱う

- `UserDataViolation` は `{code, path(JSON pointer), fields}` のみ。`primary_source` / `related_sources[]` の主張を E-0a から**削る**(origin map は E-4 所有のまま)
- 受け入れ入力は **同一 scene 文書の camera**(prefab parameter / project default の由来は使わない)
- `get_components` の JSON: **component object 直下に `"resolution_error": {code, path, fields}`**(generated component も同じ位置。serializer の早期 return
  [editorcommandservice.cpp:1453](../src/core/communication/editorcommandservice.cpp#L1453) を直す)。exact JSON を受け入れに書く

##### 決定 5(#5): camera の cross-field 検査は **completion 後の 1 箇所**に集約

- decoder 内の `znear >= zfar` 検査([componentcodec.cpp:608](../src/core/loader/componentcodec.cpp#L608))を消し、resolver completion([resolvedscene.cpp:285](../src/core/loader/resolvedscene.cpp#L285))の
  1 箇所で `camera.zfar_not_greater_than_znear@1` を出す。`{"znear":5000,"zfar":1000}` と `{"znear":5000}` + default 1000 が **同じ code・同じ path** になることを同一テストで
- basic-config の `near/far/fov_y` parser([basicconfig.cpp:131](../src/core/loader/basicconfig.cpp#L131))は**範囲外**(別 parser。記録)

##### 決定 6(#6): preview の対照を 2 つに分ける

- (a) **decoder alias**(`near`): 今日は decoder が throw → preview が `schema_violation`。移行後は値エラー → preview が Result を検査して拒否。
  対照 = Result 検査を `(void)` に戻す変異で alias が受理される
- (b) **completion failure**(`znear=5000`、`zfar` 省略): `decodeAuthored` は成功し resolver で落ちる。preview は **resolver 後の stored failure を明示的に検査**する production test

##### 決定 7(#7): inactive scene の 2 失敗は **`get_components` で同時観測、load は 2 回**

- `get_components` が camera と collider の **stored alternative を同時に**返す(1 query)
- load は fail-fast のため 1 回目は camera で止まる。camera を直して 2 回目に collider が露出する、と明記。
  実 RPC `load_scene` の一般例外(`-32000` + message)は本 WP で **`error.data = {code, path, fields}`** を載せる(WP381 決定 12 と同じ契約)

##### 決定 8(#8): legacy 拒否は **`legacyReject(id, …)` の明示 API** 経由に限定

- production-owned・構成非依存の descriptor registry。**ID 重複はビルド時 / 起動時に失敗**、docs は ID 順に生成
- `LegacyResolutionText` は `legacy_branch_id` を保持する
- `componentCodecs()` が runtime state で `ray_instance` codec を除外する([componentcodec.cpp:1208](../src/core/loader/componentcodec.cpp#L1208))のは registry の列挙に影響させない
  (registry は codec 表と独立)。未列挙の `throw std::runtime_error` を decoder に足す変異は、**registry を通らない throw を検出する fault test**(6 caller の内部伝播テスト)で落ちる

##### 決定 9(#9): 6 caller × 3 点 + caller ごとの変異 1 つ

- 各 caller に **typed failure / 成功時に実際に消費された値 / internal fault** の 3 点。transaction の 2 経路(:114 / :528)は別々
- 変異は **caller ごとに 1 つ**(Result 検査を `(void)` に戻す)
- 型の固定: copyable な sentinel `X / E` で `!copy_constructible` / `!copy_assignable` / `move_constructible` / `move_assignable` + `std::any` 4 assertion
- ABI fixture: 凍結旧ヘッダ側から**実際に throw** し、新 core 側で `code / path / what()` を検査(link / run だけでは不可)

##### 決定 10(#10): 依存と構成

- **依存: WP375a 統合後**(RPC guard を取り込む)。構成表に `PELICAN_WITH_RPC` ON/OFF(OFF は configure / build + 非 RPC service test)と `PELICAN_WITH_OPENXR` を足す

**第 2 巡へ。** 見積は「中」のまま(decoder の cross-field 集約と broad catch 除去が増え、provenance が減った)。

#### WP378 仕様レビュー第 2 巡(不合格・8 件)と v3 —— **決定差分(レビューはここで打ち切り。残余はコードレビューへ)**

第 2 巡は v2 の決定同士の矛盾を突いた: 「throw を捕捉しない」(決定 3)は、未移行 legacy を保持すること(規範 E-3b まで broad catch を残す)と、
`noexcept` な production transaction の中で internal を再送出すること(E-3a 範囲外)の**両方と両立しない**。仕分け: **直す 8 / やらない 0**。
v3 の原則: **E-0a は「値の経路を足す」だけで、「例外の経路を消す」ことはしない。** 消すのは E-3a / E-3b。

##### 決定 11(#1 #2): broad catch は **残す**。E-0a が変えるのは「移行済み producer が値で返る」ことだけ

- 6(→5、決定 13)caller の decode 境界にある broad catch(legacy 文字列化)は **E-3b まで残す**(規範 [design_error_contract.md:335](../docs/design_error_contract.md#L335) のとおり)
- `legacyReject(id, …)` は **`LegacyRejectException`(`std::runtime_error` 派生、`legacy_branch_id` を持つ)を throw** し、既存の catch がそれを `LegacyResolutionText{id, message}` に写像する
- internal fault(`logic_error` / callback 欠落)の「再送出して terminate しない」証明は **decode 境界の単体 seam(caller を経由しない `decodeAuthored` 直呼び)で
  `Result.error` にならず throw のまま**であることまで。production transaction 内の伝播 / death test は **E-3a**
- 受け入れ「内部が利用者エラーに見えない」は、**移行済み camera producer について** `UserDataViolation` が値で返り、`LegacyResolutionText` にも `AdapterPrepareFailed` にも
  ならないこと(= 今日の対照)に限定する

##### 決定 12(#4): cross-field 規則は **1 つの純粋関数**。4 箇所から呼ぶ

- `cameraCrossFieldViolation(znear, zfar, …) → optional<UserDataViolation>` を 1 箇所に置き、**decoder(両値が揃うとき)/ resolver completion 後 / transaction :528 と
  preview の「完全検証」境界 / basic-config の camera parser**([basicconfig.cpp:262](../src/core/loader/basicconfig.cpp#L262))の 4 箇所から呼ぶ。
  v2 の「decoder 側を消す」は撤回(decode-only 経路が黙って通る)
- 受け入れ: 4 箇所それぞれで `{"znear":5000,"zfar":1000}` が **同じ code・同じ path** で落ち、関数の呼出を 1 箇所ずつ消すとその経路だけ落ちる(4 変異)
- inactive scene への stored failure の commit 条件: **document は commit、runtime は bind しない**(今日の inactive 動作と同じ)。active scene では fail-fast

##### 決定 13(#3 #6): preview の validation-only decode は **caller から外す**。caller は 5、変異は「その site の mapping を消す」

- preview([editorpreviewprojection.cpp:261](../src/core/loader/editorpreviewprojection.cpp#L261))の前段 decode は値を捨てる二重 decode。**削除**し、resolver + stored failure 検査を唯一の経路にする
  (第二の流儀の削減。preview の受け入れは決定 6(b) だけ残し、6(a) は撤回)
- **caller 5**: journal :223 / factory :372 / transaction :114 / transaction :528 / resolver :207。各行に **到達証拠(production 入力でその site に来たこと)/
  その site 固有の error→failure mapping を消す変異(対象行だけ赤、他 4 行は緑)/ 成功値を consumer が使った観測** の 3 点。`(void)` 変異は撤回(コンパイルエラーになる)
- transaction :528 の production 到達性(空 adapter commit = journal preflight [editorjournal.cpp:2121](../src/core/communication/editorjournal.cpp#L2121))を到達証拠に含める

##### 決定 14(#5): legacy registry は **descriptor ごとの到達 corpus** で全単射を検査する

- 各 `legacy_branch_id` に **到達する入力を 1 件以上** corpus(`test/fixtures/component_codec/legacy_corpus.json`)に置き、
  `descriptor ID ↔ 実際に返った legacy_branch_id` の**全単射**(未到達 ID・未登録到達の両方が失敗)
- **暗黙 / transitive の例外**(unchecked `get<string>` [collider.cpp:110](../src/core/userpublic/components/collider.cpp#L110)、`value<string>`
  [componentcodec.cpp:612](../src/core/loader/componentcodec.cpp#L612)、sprite の `get<float>` [spriteview.cpp:27](../src/core/userpublic/components/spriteview.cpp#L27)、
  atlas の transitive 拒否 [atlasasset.cpp:138](../src/core/asset/atlasasset.cpp#L138))は **着手時に列挙し、`legacyReject` で明示化するか corpus に載せる**。
  nlohmann の `type_error` が裸で出る経路は 0 にする(それ自体が受け入れ)
- 「未列挙 throw を足す変異」は corpus に入力が無ければ検出できない、と明記(代理条件にしない)。検出は全単射検査の側

##### 決定 15(#7): 値は **`ResolutionFailureException`** で運び、RPC で `error.data` に変換する

- `UserDataViolation` / `LegacyResolutionText` を保持する **type-preserving な例外 wrapper**(`std::exception` 派生)を定義し、
  `requireRuntimeValue`([resolvedscene.cpp:44](../src/core/loader/resolvedscene.cpp#L44))はそれを throw する(文字列 `runtime_error` に戻さない)
- `SceneLoader::load` はそのまま伝播。RPC の `load_scene` は wrapper を捕捉して **`error.data`** に変換する:
  typed → `{"code": "...", "path": "...", "fields": {...}}` / legacy → `{"legacy_branch_id": "...", "message": "..."}`。両方の exact JSON を本文に置く(決定 16)
- 通信層の型を loader から投げない(D0 / 層の分離)

##### 決定 16(#8): exact JSON(規範)

`get_components` の component object 直下(authored / generated とも同じ位置):
```json
"resolution_error": { "code": "camera.zfar_not_greater_than_znear@1",
                      "path": "/scenes/main/objects/3/components/1/znear",
                      "fields": { "znear": 5000.0, "zfar": 1000.0 },
                      "message": "camera zfar (1000) must be greater than znear (5000)" }
```
legacy: `"resolution_error": { "legacy_branch_id": "collider.radius_non_positive", "message": "..." }`(`code` / `path` / `fields` 無し)。
- `path` は **scene document 絶対の JSON pointer**(primary token = `znear`。related は E-4 まで出さない)
- `fields` は **順序付き object**(規範の field 名順)。`message` は残す(v1 の leaf byte 一致は message にも適用)
- generated component は serializer の早期 return([editorcommandservice.cpp:1453](../src/core/communication/editorcommandservice.cpp#L1453))を直して同じ位置に出す
- `load_scene` の `error.data` は上の object をそのまま(`resolution_error` の中身)。golden は **この literal から** byte-exact に固定する。
  決定 12 の「同じ path」検査は、この literal との一致で置換(代理条件の撤回)

##### 段割りと依存(v3)

依存: **WP375a 統合後**。仕様レビューは 2 巡で打ち切り。見積: **中〜大**(cross-field 4 箇所、preview の decode 削除、registry corpus、wrapper 型が増えた)。
E-3a(commit noexcept / internal の伝播 / generation guard)と E-3b(broad catch 撤去)の節は、本 WP 統合後に切る。
### WP379: RAY-NAMESPACE —— `ray_instance` を `pelican_ray_instance` へ移し、builtin 名を owner-aware に予約する

**§4 規則 11 中段(core の複数経路 + project 形式)。仕様レビュー(1 巡 + 決定差分)+ コードレビュー。見積: 小〜中。**
**規範は `docs/design_declaration_carrier.md` v4 §8 修正 θ(RAY-NAMESPACE は独立 WP)。**下の **v3 差分が本節の正**(v2 で migration・alias を廃止、v3 で受け入れと範囲を確定)。
**依存: WP375(統合後。同じ production test TU を触る)。GLABI-2(WP381)は本 WP の後。WP374 の後追い負債。**

#### 何が壊れているか

WP374 の version gate は「外部 `ray_instance` を持つ project が layout v2 を有効化すると builtin 登録で衝突する」逆向きの
問題を残した。現テストも LayoutV2 で builtin の逆登録が throw することを固定している
([raytracing_rt3_1_test.cpp:289](../test/raytracing_rt3_1_test.cpp#L289))。**未出荷の今が最安。**

#### 起票時の実測(2026-09-02)

- 旧名 `"ray_instance"` の hardcode は **11 箇所 / 6 ファイル**:
  [editorruntimefactory.cpp:951](../src/core/communication/editorruntimefactory.cpp#L951)(live invalidation 分岐)/
  [predefined.cpp:35](../src/core/ecs/predefined.cpp#L35)(ECS 登録)/
  [componentcodec.cpp:957](../src/core/loader/componentcodec.cpp#L957)・:992・:993・:1015・:1069(codec)/
  [editorprojectionadapters.cpp:834](../src/core/loader/editorprojectionadapters.cpp#L834)(adapter の名前表)/
  [scene.cpp:58](../src/core/loader/scene.cpp#L58)・:68・:80(preflight)
  ——うち editorruntimefactory / editorprojectionadapters の 2 箇所は **WP375 が名前表を廃止する**ので、WP375 着地後は減る
- `ComponentInfo.owner` は runtime 登録の予約判定に使える([componentinfo.hpp:21](../src/core/ecs/componentinfo.hpp#L21))が、
  **scene JSON の component 名には owner が無い**
- 出荷 4 project と `src/core/resources/` に `ray_instance` は 0 件(自動移行の対象が無い)

#### 決定(設計 v3 修正 6 をそのまま)

1. **canonical 名は `pelican_ray_instance`**。11 箇所を全部移す(WP375 が消す 2 箇所は WP375 側で消えていることを確認)
2. **`pelican_` 接頭辞を owner-aware に予約**: 外部 owner が `pelican_*` を登録したら **`reserved_component_namespace@1`**
   (owner / module 名を payload に)。builtin の owner だけが登録できる
3. **migration matrix**(load 時):

| 判定 | 動作 |
|---|---|
| layout **v2 の project** に旧 `ray_instance` があり、外部 owner が同名を登録していない | builtin と証明できる → 読み込み時に `pelican_ray_instance` として解釈、**save 時に canonical へ書き換え** |
| 外部 owner が `ray_instance` を登録している | **そのまま外部 component**(builtin は関与しない) |
| layout v2 で旧名があり、**かつ**外部 owner も同名を登録している | **`RayInstanceMigrationAmbiguous@1`** で load を止める(両方の owner を payload に) |
| layout 無し / v1 の project の旧名 | 外部 component として扱う(builtin は v2 でしか登録されない。WP374 の gate どおり) |

4. **alias 期間**: RPC / catalog / error payload は旧名を **1 リリースの間**受理し、`deprecated_component_name@1` を warning で返す。
   save は常に canonical
5. WP374 の version gate(v2 でのみ builtin 登録)は**維持**。改名は gate の代替ではなく、gate が残す逆向き衝突を消す

#### 受け入れ(§4 規約 10)

- **同一テスト内の両側**: 旧名を持つ v2 project を load → resolved には `pelican_ray_instance` として現れ TLAS 値が同じ →
  save → bytes は canonical 名 → 再 load で同値。**migration を外すと旧名が未知 component になって落ちる**
- **外部 owner 保護**: 外部 `ray_instance` を登録した状態で v2 を有効化しても **builtin と衝突しない**(WP374 の
  「外部 no-layout owner を保護」テストを v2 でも通す。今日は衝突 = 対照)
- **`RayInstanceMigrationAmbiguous@1`**: 外部 owner あり + 旧名あり + v2 で load が止まり、payload に両 owner
- **`reserved_component_namespace@1`**: 外部 owner が `pelican_foo` を登録すると名前付き拒否(owner / module つき)
- **alias**: RPC の `add_component("ray_instance")` が warning つきで受理され、save は canonical。alias を外すと拒否
- **hardcode 0**: 旧名の文字列が `src/` に**残らない**ことを、alias 経路以外で旧名を受理する変異が落ちることで示す
  (静的文字列検査ではなく、旧名を builtin 名として解釈する分岐を 1 つ残すと migration テストが落ちる)
- 出荷 4 project・`src/core/resources/` の bytes 不変(対象 0 件。効能の対照ではない)/ 全数(着手時に実測)/ `SKIP_DEVSTUDIO` 両構成

#### やらないこと

担体本体(SCHEMA-TX / RT3)/ GLABI-2 / Studio の catalog / 他 builtin component の改名(`pelican_` は新規 builtin から適用)。


#### WP379 仕様レビュー第 1 巡(不合格・10 件)と v2 —— **migration と alias を捨てる**

仕分け: **機構ごと削除 5**(#1 #2 #3 #5 #8)/ **直す 4**(#6 #7 #9 #10)/ 訂正 1(#4 は alias 廃止で消える)。

**なぜ機構を捨てるか。** scene JSON に owner provenance が無い以上、「builtin と証明できる」判定は **DLL の初期化状態に依存して非決定**
になる(#1: 同じ scene が、依存 DLL の故障だけで builtin に化ける)。そして移行対象は**存在しない**: 出荷 4 project と
`src/core/resources/` に旧名 0 件、`ray_instance` は WP374(2026-08、未出荷)が導入した builtin、公開 RPC client も 0。
**移行対象の無い migration matrix と、利用者の無い alias は、レビューが 3 回穴を見つける機構そのもの**である。

##### 決定 A(#1 #2 #3 #4 #5 #8): **migration matrix・alias・`RayInstanceMigrationAmbiguous`・`deprecated_component_name` を作らない**

- builtin は **`pelican_ray_instance`** として登録し、11 箇所の hardcode を移す。それだけ
- 旧 `ray_instance` は **registry の言うとおり**: 外部 owner が登録していればその外部 component、誰も登録していなければ
  既存の **`Unknown component` 名前付き拒否**(scene / prefab / snapshot / RPC の全 ingress は既に同じ registry lookup
  [componentcodec.cpp:1141](../src/core/loader/componentcodec.cpp#L1141) を通るので、ingress ごとの seam は要らない。#3 はここで閉じる)
- RPC は canonical 名だけ受理(#4 の「raw 旧名で重複検査」問題は alias が無ければ発生しない)
- **依存から GLABI-2 を外す**(migration が無いので DLL 初期化順に依存しない)。設計 §8 修正 θ の順序は
  「GLABI-2 と RAY-NAMESPACE は並列可」に訂正する。依存は **WP377 統合後**のみ

##### 決定 B(#10): 予約は **immutable な grandfather allowlist + 単一 policy 関数**

- 着手時に registry へ登録される **全 unprefixed builtin**(`eid / transform / localtransform / simplemodelview / camera / animation`
  [predefined.cpp:24](../src/core/ecs/predefined.cpp#L24)〜:29 と、他の登録点を実測で列挙)を **1 箇所の allowlist** に固定する。
  以後の追加は allowlist の diff として現れる
- policy は **1 関数**(登録器の入口): engine owner → `pelican_` prefix **または** allowlist(それ以外は **`builtin_name_unprefixed@1`**)/
  external owner → `pelican_` **禁止**(**`reserved_component_namespace@1`**)。「新規 builtin から」という時間依存の規約は捨てる

##### 決定 C(#6): エラー payload は **`{name, owner_kind: engine | external}`** だけ

raw `RegistrationOwner`(世代付き再利用整数)と DLL path(マシン依存)は**載せない**(決定性)。module id は
GLABI-2 の `OwnerDescriptor` が来たら足す(WP381 の後追いとして台帳に残す。本 WP では要求しない)。

##### 決定 D(#7 #9): 受け入れは **WP374a の production fixture** で、同一テスト内に両側を置く

`raytracing_rt3_1_production_test.cpp` の fixture(SceneLoader → 実 RPC bytes → commit → save → fresh reload → Renderer / GPU TLAS)で:
- (a) **canonical**: `pelican_ray_instance` を持つ scene の load / RPC `add_component` / save / fresh reload で **TLAS 値と bytes が一致**
- (b) **旧名の対照**: 誰も `ray_instance` を登録していない scene が **`Unknown component` の既存 code** で落ちる
  (= migration も alias も無いことの観測。この分岐を「builtin として解釈」に戻すと (b) が落ちる)
- (c) **外部 owner 保護**: 外部 owner が `ray_instance` を登録した状態で layout v2 を有効化 → builtin `pelican_ray_instance` と
  **共存して両方 resolve**(今日は逆登録 throw = 対照。[raytracing_rt3_1_test.cpp:289](../test/raytracing_rt3_1_test.cpp#L289) の
  手動登録テストは **production fixture へ移し、旧テストは消す**)
- (d) **policy**: external の `pelican_foo` → `reserved_component_namespace@1`、engine の allowlist 外 `foo` → `builtin_name_unprefixed@1`。
  **policy 関数の呼出を消すとどちらも通ってしまう**(対照)
- 構成: `PELICAN_RUNTIME_SHADER_COMPILER=ON` で (a)〜(d) 全部。**OFF では (b)(c)(d) を実行し、(a) の GPU 部分だけ skip**
  (今の `SUCCEED` 分岐 [raytracing_rt3_1_production_test.cpp:493](../test/raytracing_rt3_1_production_test.cpp#L493) を GPU 部分に絞る)。
  `SKIP_DEVSTUDIO` 両構成。GPU は統合側で直列
- **静的 literal 検査は受け入れにしない**(#8)。旧名 literal 0 件は回収時の監査項目

##### やらないこと(v2 で追加)

migration / alias / ambiguity エラー / owner の module id / 他 builtin の改名(allowlist で据え置く)/ prefab registry の構築順
(外部 DLL 登録より前に prefab を検証する既存挙動は本 WP の外。着手時に再現したら台帳へ記録だけ)。


#### WP379 仕様レビュー第 2 巡(不合格・6 件)と v3 —— **決定差分(レビューはここで打ち切り。残余はコードレビューへ)**

「migration / alias を作らない」中心判断は支持された。落ちたのは受け入れの成立条件と作業範囲。仕分け: **直す 6 / やらない 0**。

##### 決定 E(#1): compiler OFF の対照は **`layout_requires_runtime_compiler@1`**。policy テストは CPU 実行体へ

- `PELICAN_RUNTIME_SHADER_COMPILER=OFF` では layout 付き project が component 解決の**前**に
  `pelican.ray_tracing.layout_requires_runtime_compiler@1` で止まる([raygrouplayout.cpp:1175](../src/core/shader/raygrouplayout.cpp#L1175))。
  これを OFF の**正式な対照**とし、(a)(b: layout v2)(c) は **compiler ON でだけ**実行する
- (d)(policy)と (b: no-layout の未登録名)は **GPU label を継承しない CPU TU / target** に置く(同一実行体は target 単位で `gpu` を継承する:
  [test/CMakeLists.txt:115](../test/CMakeLists.txt#L115))

##### 決定 F(#2): 未登録 component は **`component_unknown@1`** を共通 cause にする(SceneLoader と RPC)

- 今は scene が code 無しの `runtime_error("Unknown component …")`([scene.cpp:117](../src/core/loader/scene.cpp#L117))、RPC が固定 codec 表の
  `unknown_component_type`([editorjournal.cpp:212](../src/core/communication/editorjournal.cpp#L212))で、**同じ lookup ではない**(v2 の前提は誤り。訂正)
- 本 WP で **`component_unknown@1{name}`** を scene(`SceneLoader`)と RPC(`add_component`)の両方に入れる。snapshot import は top-level `snapshot_invalid` を保ち、
  **inner cause** として同 code を載せる。prefab は構文検査のみ(変えない。未登録名は scene 解決で同 code に落ちる)
- 受け入れ (b) は **scene / RPC / snapshot の 3 ingress** で同 code を検査する(prefab は scene 経由で 1 回)

##### 決定 G(#3): allowlist は **7 件**。テスト・benchmark の登録は **allocated test owner** へ移す

- production bootstrap の unprefixed engine builtin は **`eid / transform / localtransform / simplemodelview / camera / animation / sprite_view`**
  (最後は [spriteview.cpp:107](../src/core/userpublic/components/spriteview.cpp#L107))
- scope 無しの `registerComponent<T>` は既定で engine owner に落ちる。**テスト 14 call site と `ecs_benchmark`([benchmark.cpp:220](../src/core/ecs/benchmark.cpp#L220))は
  `TestSupport::ScopedTestRegistrationOwner`(allocated external owner)へ移す**。engine owner の登録は **明示の engine registrar** からだけ行う
  (暗黙 scope 無し = engine、をやめる)
- 受け入れ: **production registry の unprefixed engine-owned 集合が allowlist と完全一致**(動的。allowlist を 1 件減らすと落ちる)

##### 決定 H(#4): (c) は **実 DLL fixture** を `initializeConfiguredGameLogic` 経由でロードする

- WP162 系列に **serializable な外部 `ray_instance` component を登録する fixture DLL** を足し、production bootstrap
  (`configure layout → builtin 登録 → initializeConfiguredGameLogic → SceneLoader`、[pelican_core.cpp:108](../src/core/userpublic/pelican_core.cpp#L108))で通す。
  内部 API(`allocateRegistrationOwner`)での手動登録は受け入れに数えない。[raytracing_rt3_1_test.cpp:289](../test/raytracing_rt3_1_test.cpp#L289) の旧テストは消す
- 対照: DLL の外部 `ray_instance` を登録しないと scene が `component_unknown@1` で落ちる / DLL ありで builtin と共存し両方 resolve

##### 決定 I(#5): 旧名出現の **manifest**(着手時に固定。回収時に照合)

| 分類 | 件数(2026-09-02 実測) | 扱い |
|---|---:|---|
| `src/` production hardcode | **11 / 5 ファイル**(v1 の「6 ファイル」は誤り) | 全部 canonical へ(WP375 が消す 2 箇所は WP375 側で確認) |
| `test/` literal | 19 / 3 ファイル | positive builtin fixture → canonical / (b)(c) の negative → **意図的に旧名を保持**(manifest に行ごとに記す) |
| `docs/` raw token(本節以外) | 31 / 4 ファイル | 歴史記述は保持、現行仕様の記述は canonical へ |
| 出荷 4 project / resources / tracked JSON・binary | 0 | 対象なし |
- 設計文書 §8 修正 θ の RAY-NAMESPACE 行(「修正 6」= migration matrix)を「rename + owner policy、migration / alias 無し」に**一本化**する(本差分と同時に修正)

##### 決定 J(#6): policy の受け入れは **表駆動 + exact payload**

- allowlist **7 件全部**が engine owner で受理 / allowlist 外 **3 件以上**(`foo` / `ray_instance` / `sprite`)が `builtin_name_unprefixed@1` /
  external の `pelican_*` **3 件以上**が `reserved_component_namespace@1` / external の unprefixed は受理
- payload は **`{name, owner_kind}` の exact 一致**(余分な field 無し。raw owner・path を含まない)。owner を allocate → release → 再利用しても **bytes 同一**
- 全部 production registration entry(policy 関数の所在)経由。allowlist / prefix / owner の判定を **1 つずつ**消す変異がそれぞれ落ちる

##### 段割りと依存(v3)

**依存: WP375 統合後**(同じ `raytracing_rt3_1_production_test.cpp` を触る)。WP381 は本 WP の後(決定 5 のとおり)。
仕様レビューは 2 巡で打ち切り。見積: **中**(決定 G の owner 移設 15 call site と決定 F の 2 ingress が増えた)。


#### WP379 完了記録(統合側。2026-09-03、`51841eb3`)

- **実装(codex、fast 無し)**: builtin を `pelican_ray_instance` に統一(登録・codec・scene preflight・TLAS / RayLayout 診断)。未登録 component の共通 cause
  `component_unknown@1{name}`(`ComponentUnknownError`。SceneLoader と RPC が同じ cause、snapshot は `snapshot_invalid` の inner detail)。
  owner-aware policy を production 登録入口 1 箇所に(engine: `pelican_` or allowlist 7 / external: `pelican_` 禁止 / payload `{name, owner_kind}`)。
  scope 無しの登録を拒否し、builtin は明示 engine registrar、test / benchmark は allocated test owner へ。実 DLL fixture(`test/fixtures/ray_namespace_component`)で共存試験
- **決定 I の manifest(実測)**: `src/` production hardcode は着手時 9 / 3(WP375 が 2 箇所を先に消していた)→ **0**。`test/` は 21 / 4 → **14 / 3、全部意図的 negative**。
  design docs の raw token 29 / 4 → 11 / 2(全部 superseded / 履歴)。出荷物 0
- **統合側の GPU 実行**(agent は GPU 禁止で UNMET 報告): 20 件中 **1 件赤**(`production bootstrap keeps external ray_instance …` の `instance_flags` 期待 0 に対し実測 1)→
  継続で判定 → **(a) 期待値の誤り**: `with_as=true` の fixture は AS defaults を使い、`instance_flags` の既定は `triangle_facing_cull_disable`(= 1。`rayinstance.hpp:11`)。
  authored は mask だけなので resolver が default の 1 を維持する(WP374 テストも同じ既定を期待)。期待値を定数に直し、**DLL 無しの canonical-only control の row(mask / flags / custom index / geometry flags)を保存して、
  外部 DLL + `ray_instance` ありの row と直接比較**する形に変更(共存が row に影響しないことの直接検査)。GPU 1 / 1 緑
- **統合側の変異 3 本**: **G1**(builtin を旧名で登録)継続後に再実行 → **赤**(基準は 20 / 20 緑) /
  **G2**(RPC の未登録 component を `schema_violation` に)**赤** / **G3**(external の `pelican_` 禁止を消す)**赤**(CPU)。残留 0
- **統合側の検証(worktree)**: 継続後の再ビルドで WP379 系 + RT3-1 production GPU **20 / 20**、非 GPU **1221 / 1221**(復元後)
- 全数(統合後 main、`SKIP_DEVSTUDIO=OFF`、projects dir、GPU 直列、`-j4`): **1540 件中 1539 緑**。落ちた 1 件は `WP375 production acceptance c`(fixture DLL の **shadow copy 衝突** = 既知 flake。単独再実行 1 / 1 緑)
- コードレビュー `rev379`: **Reject**。致命: 外部 DLL が `registerEngineComponent` / owner allocator を呼んで engine owner を自称でき、unload 後に callback が残る。実害: RPC が codec 無しの登録済み外部 component を `component_unknown` と誤分類 / binding 付き prefab の未登録名が共通 cause に到達しない / 終了順を守るテストが無い。リーク: test owner guard の release が黙って失敗。→ **WP379a**(連鎖 1 段)。壊せなかった: 11 decoder 分岐の等価性、出荷 80 JSON に旧名・canonical 名なし、3 ingress の共通 cause(binding 無し)、第二の流儀なし

### WP379a: 外部 DLL が engine owner を自称できる(WP379 コードレビューの致命 1 + 実害 3 + テスト欠陥 2)

**§4 規則 11 上段。コードレビューのみ(§10)。見積: 小〜中。連鎖: WP379 → 379a で打ち切り(致命の修正として 1 回だけ)。**
**依存: なし(WP379 統合済み `51841eb3`)。WP381(loader)と並列: 本 WP は `gamelogicreload.cpp` を触らない(shutdown 順の共有関数は `pelican_core.cpp` 側)。**

#### 何が壊れているか(コードレビュー `rev379` が確認)

1. **致命**: `UserComponentRegistererTemplatePublic` が export され、public な `registerEngineComponent()` が無条件に owner `0`(engine)を渡す
   ([registerer.hpp:117](../src/core/userpublic/details/component/registerer.hpp#L117))。さらに `allocateRegistrationOwner()` / `ScopedRegistrationOwner` も export
   されているため、**外部 DLL が engine owner を自称して `pelican_*` を登録でき**、その登録は loader の owner purge 対象外 → unload 後も callback が unloaded code を指す(UB)
2. **実害**: RPC `add_component` は production registry を見ず、固定 `findComponentCodec()` が null なら無条件に `component_unknown@1`
   ([editorjournal.cpp:215](../src/core/communication/editorjournal.cpp#L215))。実 DLL が登録した codec 無しの外部 component を **RPC だけが「未登録」と嘘をつく**(scene は受理)
3. **実害**: binding 付き prefab の未登録名は registry lookup より前に `PrefabBindingNotBindable`([prefab.cpp:963](../src/project/prefab.cpp#L963))。
   v3 決定 F の「prefab は構文検査のみ、未登録名は scene 解決で `component_unknown@1`」に反する
4. **テスト欠陥**: 実 DLL fixture が非 Windows で無条件 FAIL。production の終了順(`teardown.run() → shutdownConfiguredGameLogic()`
   [pelican_core.cpp:129](../src/core/userpublic/pelican_core.cpp#L129))をテストが手書きで写しているだけで、順序反転・`unregisterComponents(owner)` 削除・別 owner 登録の変異が通る
5. **リーク**: `ScopedTestRegistrationOwner` の destructor は token が残った状態で `releaseRegistrationOwner()` を呼び、それは黙って return する
   ([registrationowner.cpp:89](../src/core/userpublic/details/reload/registrationowner.cpp#L89))→ owner slot がリーク(3 test helper + benchmark)
6. **「exact」でないテスト**: decoder 11 分岐のうち直接検査は 4。allowlist の実集合(`grandfathered_engine_names`)は production descriptor と比較されていない。
   `SKIP_DEVSTUDIO=ON` では WP379 の実 DLL / 3 ingress テストが `[gpu]` のため未実行
7. manual が旧文字列を規範として残す([manual/04_scene_ecs.md:92](../docs/manual/04_scene_ecs.md#L92)・:300)

#### 直すこと

1. **engine registrar と owner allocator を game-DLL 向けヘッダから外す**。owner は DLL が渡せない host 側 capability にし、builtin 登録だけ friend / private 経路。
   実 DLL fixture で `registerEngineComponent` / `allocateRegistrationOwner` が **リンクできない(または名前付き拒否される)**ことを受け入れに
2. RPC に registry の existence lookup を注入: 未登録 → `component_unknown@1`、登録済みだが editor codec 無し → **`not_editable`**(既存 `set_component_value` の分類と揃える)
3. prefab: provider 自体が未知なら raw substitution を続け、後段の registry に未登録判定を委ねる(既知 provider の未知 path だけ `PrefabBindingNotBindable`)。SnapshotV2 も同じ
4. test と player が **同じ terminal-shutdown 関数**を呼ぶ。shutdown 後に `ComponentUnknownError`、owner count 0、同一 process 再ロード成功まで検査。非 Windows は fixture 無しで **SKIP(理由付き)**
5. owner guard を module container より長生きさせるか、最後の token 解放時に pending owner release を完了させる。release 失敗を名前付きに
6. decoder 11 分岐を同一 codec の表駆動 corpus に。allowlist は production descriptor から一元生成(または列挙可能にして exact 比較)。統合側で `SKIP_DEVSTUDIO=ON` の GPU も回す
7. manual を `component_unknown@1{"name":"X"}` に更新(履歴文書は触らない)

#### 受け入れ(§4 規約 10)

- **owner 自称**: 実 DLL fixture(現行の `ray_namespace_component` を拡張)が `registerEngineComponent<...>("pelican_evil")` を呼ぶ形は **リンク不能**(または `reserved_component_namespace@1`)。
  外部 owner の登録は unload 後に **registry から消える**(今日 = 残る)。engine registrar を DLL ヘッダに戻す変異で落ちる
- **RPC 分類**: 実 DLL + 実 RPC で `add_component("ray_instance")` → `not_editable`(今日 = `component_unknown@1`)。lookup を外すと落ちる
- **prefab**: binding 付き未登録名 → scene 解決で `component_unknown@1{name}`(今日 = `PrefabBindingNotBindable`)。SnapshotV2 は inner cause として同 code
- **終了順**: production の 2 行を反転する変異・`unregisterComponents(owner)` を削る変異・別 owner に登録する変異がそれぞれ落ちる
- **リーク**: owner slot を allocate → 登録 → guard 破棄 → manager 破棄 で owner count が 0 に戻る(今日 = 戻らない)
- **11 分岐 corpus**: conflict check を削ると落ちる。allowlist に未登録名を足すと落ちる
- 全数(着手時に実測)/ `SKIP_DEVSTUDIO` 両構成(GPU も統合側で)/ RPC OFF / 出荷 4 project bytes 不変 / golden 不変

#### 記録のみ

`rev379` #4 の非 Windows(実 `.so` fixture)は Linux CI ができたときに。
### WP380: ID-B —— scene load を 1 つの transaction にする(publish 途中の失敗で世代を混ぜない)

**§4 規則 11 上段。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 大。**
**規範は `docs/design_object_identity.md` v4(§9)。**下の **v3 差分が本節の正**(v2 の state / effect 分割は v3 決定 8 で撤回。WP380a = state + engine-owned queue、WP380b = 公開 mutator journal + audio + persistence)。
**依存: WP375(着地後。ECS の inverse-token early publication と observer gate を再利用する)。registry(ID-A)は未装着で走る。**

#### 何が壊れているか(既存欠陥。設計レビューが本番入力で再現)

scene A 稼働中に snapshot B を import し、B の behavior `onInit` が投げると:
loader は runtime を clear した直後に Light / Camera を publish し、その後 behavior を activate する
([scene.cpp:369](../src/core/loader/scene.cpp#L369)・:546)。catch の `clearRuntimeScene()` は Light / Camera の
旧 prepared state を戻さない(:584)。snapshot import も document は戻すが runtime を reload しない
([basicconfig.cpp:734](../src/core/loader/basicconfig.cpp#L734))。
**document は A、Light / Camera は B、ECS / Physics は空か途中** —— 世代混在が今日成立する。

#### 1. 保証範囲(設計 修正 A)

- **engine-owned state / Pelican API 経由の変更だけ**を原子的に戻す。DLL global・直接 filesystem / socket / thread は**非保証**と文書化
- `behavior.hpp` の layout / virtual surface は**変えない**。既存 `GameContext::*` と event registerer の `__emit` の**下**に
  scoped transaction sink を置く(旧 DLL のコードもその sink を通る)
- effect outbox: **event(payload と順序)/ RNG(列)/ pending scene / audio(provisional handle、read-your-writes)/
  persistence(settings の shadow、`saveData` は commit 後 write)**。commit で flush、rollback で破棄
- rollback 中の逆順 `onDestroy` も同じ sink の内側

#### 2. scene 専用 executor と state machine(設計 修正 B)

```
prepare → early_publish_ecs(observer gate、inverse token)→ publish_candidates(noexcept)
→ activate_behavior(失敗可)→ commit_metadata(document / current_scene / active_authority、noexcept)
→ retire_old_state(noexcept)→ flush_effect_outbox → finish(noexcept)
```
- **WP375 の adapter protocol を拡張しない**(`publish` が `noexcept` で activation phase が無い)。adapter は再利用し、
  executor は scene 専用
- participant 表(prepare / publish / rollback / finish / retire の所有物を各行に): ECS / Physics(`active_trigger_pairs` を
  `PreparedState` に追加: [physworld.cpp:273](../src/core/phys/physworld.cpp#L273))/ Light / Camera(active・controlled order・
  controller state)/ Renderer・PolygonInstanceContainer / SeqPlayer / VatPlayer / transient model retirement / Behavior arena /
  effect outbox / document・current scene・active authority。**registry slot は未装着**(ID-A)
- Light / Camera は **`old = snapshot`、`next = prepare`、`publishPrepared` 1 回**
  ([editorprojectionadapters.cpp:1024](../src/core/loader/editorprojectionadapters.cpp#L1024) の雛形)。
  外側から `load()` / `loadSceneCameras()` を呼ばない(内部で即 publish する)
- 実 publish 順は現行 lifecycle adapter と同じ **renderer → ECS → Camera → Light → Physics** → Behavior

#### 3. authority(設計 修正 1 の ID-B 分)

- `next_authority` / `active_authority` を分け、**candidate authority は prepare 前に 1 回だけ消費**。失敗しても再利用しない
  (今日は `runtime_scene_epoch` が成功末尾でしか進まず ABA になる: [scene.cpp:351](../src/core/loader/scene.cpp#L351)・:558)
- authority は process の run nonce を含む(wire への公開は ID-A)

#### 受け入れ(§4 規約 10。設計 修正 F)

- **同一 production fixture**: A 成功 → **B 成功**(B の実 event payload と順序・RNG 列・pending scene・audio handle / voice・
  保存 bytes・renderer identity・Light / Camera / ECS / Physics trigger pairs を **semantic snapshot**)→
  **C が同じ作用(`emit` / `random` / `loadScene` / `playSound` / `saveData`)を行って throw** →
  **全 participant が B の snapshot と一致**、保存データ非生成、document は B、active authority は B
  (今日は混在 = 対照)
- **participant ごとの drop / bypass 変異**(各行 1 つ。aggregate bypass 1 個では不足)がそれぞれ落ちる
- **変更前ヘッダで作った実 DLL**(WP162 の fixture 系列)が `onInit` で上記 5 作用を呼び throw する
- **snapshot import の失敗**でも同じ(document・runtime とも旧 A)
- **ABA**: 失敗 candidate の authority が次の candidate に再利用されない(同一プロセス)
- **audio**: provisional handle に対する `isPlaying / stopSound` が transaction 内で効き、rollback 後は無効
- 出荷 4 project の bytes 不変 / golden 不変 / 全数(着手時に実測)/ `SKIP_DEVSTUDIO` 両構成 / GPU は統合側で直列

#### やらないこと

registry / `ObjectRef` / wire DTO(ID-A)/ 名前 join の ref 化(ID-C)/ public API(ID-D)/ rename /
DLL global・直接 I/O の隔離(非保証)/ WP375 の protocol 変更。


#### WP380 仕様レビュー第 1 巡(不合格・8 件)と v2 —— **2 つに割る。state の transaction を先に、effect の transaction を後に**

仕分け: **直す 8 / やらない 0**。ただしレビューの 3 分割(primitives / effect / executor)は採らない。
primitives だけの WP は挙動を変えず消費者が無い(§4 の縦切り基準)。**挙動が変わる単位で 2 つ**に割る:

| WP | 変える挙動 | 今日の対照 |
|---|---|---|
| **WP380a: state transaction** | scene load / import / pending / refresh の**どの root**でも、publish 途中の失敗で **engine-owned state が旧世代のまま**(document・Light・Camera・ECS・Physics・renderer・ModelAsset・bindings) | document A / Light B / ECS 空 の混在 |
| **WP380b: effect transaction** | behavior が transaction 中に行った **Pelican API 経由の作用**(event / RNG / scene / audio / persistence / `GameObjects::*`)が rollback で消える | `onInit` の `emit` / `saveData` が失敗後も残る |

WP380a の後、WP380b 前の状態は「state は原子的、effect は漏れる」であり、それは**文書化された既知の穴**(設計 修正 A の非保証に一時的に effect を含める)。

##### 決定 1(#1): 期待値は **fixture が事前定義**する。B の実値を正解にしない

- A / B / C の各 participant 行に **異なる sentinel**(Light intensity 1 / 2 / 3、Camera fov、ECS component 値、Physics pair 集合、
  renderer instance 数・ordinal、bindings の key 集合)を fixture が持ち、B 成功時は **fixture の B 期待値**と比較、C 失敗後も同じ B 期待値と比較
- participant を drop する変異は「B 成功時の比較」で既に落ちる(A の値が残るため)。**B と C の両方で drop しても通らない**
- 「保存データ非生成」は **C 専用 slot**(A / B が書かない key)の不在と、production write sink の **commit 回数**で検査する

##### 決定 2(#2): executor の結果を **`PreCommitFailure` / `CommittedWithEffectFailure`** に分ける

- `PreCommitFailure{phase, error}`: prepare / early_publish / publish / activate の失敗。**A / A**(document も runtime も旧)
- `CommittedWithEffectFailure{active_authority, effect_errors[]}`: commit_metadata と retire の**後**の flush 失敗。**B / B のまま**名前付きで報告する
  (A へは戻さない。retire 済みだから。黙って捨てもしない)
- `BasicConfig` の catch は **commit status を見て** document を戻すか決める(今は無条件に戻す: [basicconfig.cpp:734](../src/core/loader/basicconfig.cpp#L734))。
  WP380a では flush 対象が無いので `CommittedWithEffectFailure` は**型だけ**置き、WP380b が中身を入れる

##### 決定 3(#3): executor は **未 publish の candidate** を受け取る。root は 4 つ

- `SceneLoader::load` は live config の resolved scene ではなく **`SceneProjectionState` candidate と document candidate** を受け取る
  (今は publish 済みを読む: [scene.cpp:339](../src/core/loader/scene.cpp#L339))。`BasicConfig` は**事前 publish しない**
  ([basicconfig.cpp:728](../src/core/loader/basicconfig.cpp#L728))
- production root は **4 つ全部**を executor へ配線: 直接 `SceneLoader::load` / `requestLoad → applyPendingLoad` / `importSceneDocument` /
  `refreshResolvedScene`。受け入れは **root ごと**に A/B/C を通し、**root → executor の配線を削る変異が root ごとに落ちる**。
  executor helper を直接呼ぶテストは受け入れに数えない

##### 決定 4(#4): 保証対象は **公開 mutator の全列挙表**で定義する(WP380b)

- `GameContext::*` / event `__emit` / **`GameObjects::*`**([gameobjects.hpp:20](../src/core/userpublic/gameobjects.hpp#L20))/ ECS・renderer setter の
  **全 mutating public API** を 1 表にし、transaction 中の扱いを **journal(旧値を保存して戻す)/ shadow(outbox)/ reject(名前付き)** から 1 つずつ決める
- `GameObjects::setLocalTransform` 等の**既存 entity への上書き**は **journal**(inverse token だけでは戻らない)。callback 中の利用は禁止しない
  (`onInit` で transform を触るのは正当な用法)
- 5 作用 fixture に **`GameObjects::setLocalTransform(B_id)` / 既存 voice の `stopSound` / `setBusVolume` / `setSeed` / `setGameSettings` + `saveSettings`** を足す

##### 決定 5(#5・#6): participant 表を **field 単位**で書き、retire は **prepared-retire token** で `noexcept` にする

- 表の各行: **具体 field / prepare の所有物 / publish / rollback / retire / snapshot schema / mutation ID**。
  行に足す: `SceneLoader::object_bindings` / `runtime_object_bindings` / `runtime_only_changes`([scene.hpp:43](../src/core/loader/scene.hpp#L43))/
  **`ModelAssetContainer` の lazy record と GPU resource**(lookup 中にその場で commit される: [model.cpp:413](../src/core/asset/model.cpp#L413) →
  **prepare で先に解決し、candidate が失敗したら record ごと戻す**)/ renderer の **declaration ordinal**
  ([polygoninstancecontainer.cpp:568](../src/core/renderer/polygoninstancecontainer.cpp#L568): rollback で進めない)
- `SeqPlayer` / `VatPlayer` / `PolygonInstanceContainer` / ECS の release は **prepare で retirement token を作り、retire は token の消費だけ**
  (容量予約・GPU retirement の preflight を prepare へ)。**commit 後の各呼出が `noexcept` であることは API 契約に書き、`static_assert(noexcept(...))` で固定**
- 複数 object の A→B は **scene 用の bulk adapter**(object ごとの adapter を束ねる。WP375 の protocol は変えない)
- renderer は C 直後だけでなく **D(C の後にもう一度成功する load)と対照**を比較する(ordinal / instance 数の drift)

##### 決定 6(#7): 旧 DLL fixture は **frozen header snapshot** で build する(WP380b)

- `test/fixtures/userpublic_frozen/<date>/` に public header の snapshot をチェックインし、その DLL target の include path に
  **現行 `src/core/userpublic` を入れない**ことを build 時に assert する(WP162 系列は live header で毎回再 build する:
  [test/CMakeLists.txt:435](../test/CMakeLists.txt#L435))

##### 決定 7(#8): 構成 matrix

- **ON baseline**(AUDIO / PHYSICS / SEQPLAYER / VAT 全 ON): A/B/C/D の semantic test
- **各 OFF**: compile が通る・該当 API は既存の named disabled error([gamecontext.cpp:265](../src/core/userpublic/gamecontext.cpp#L265))・
  participant が **uninstalled** として表から外れ、残りの行で A/B/C が通る。RPC OFF も同様。`SKIP_DEVSTUDIO` 両構成。GPU は統合側で直列
- 出荷 4 project の bytes / golden 不変は**機能対照ではない**(scene に behavior attachment が無い)。fixture project を別に持つ

##### 段割り(v2)

- **WP380a**(state transaction): 決定 1・2(型だけ)・3・5・7。依存: **WP375 統合後**。見積: 大
- **WP380b**(effect transaction): 決定 2(中身)・4・6・7。依存: WP380a。見積: 大
- 仕様レビューは **WP380a を先に第 2 巡**にかけ、WP380b は WP380a 統合後に節を切り直してから


#### WP380a 仕様レビュー第 2 巡(不合格・8 件)と v3 —— **決定差分(レビューはここで打ち切り。残余はコードレビューへ)**

仕分け: **直す 6**(#1 #2 #4 #6 #7 #8)/ **範囲から外す 2**(#3 #5。理由付きの裁定)。

##### 決定 8(#1): state / effect の二分割を **やめる**。engine-owned queue は WP380a に入る

「state は原子的、effect は漏れる」は今日より悪い(C の `emit` を、戻された B の behavior が受け取る)。
- **WP380a** = engine-owned state **+ engine-owned queue**(event queue / RNG 列 / pending scene 要求)。
  これらは engine が所有する 3 本の queue であり、outbox は「prepare で確保 → commit で flush(noexcept push)→ rollback で破棄」
- activation 中の `loadScene("D")` は **candidate projection に対して解決**する(candidate-aware overlay)。C が失敗すれば要求ごと破棄
- **WP380b** = 公開 mutator の journal(`GameObjects::*` / ECS・renderer setter)+ audio(provisional handle)+ persistence(shadow)+ settings + frozen DLL fixture

##### 決定 9(#2): `SceneLoaded` は **prepare で確保した outbox slot** から flush する。metadata は **1 つの noexcept swap**

- `SceneLoaded` の payload(`make_shared` + vector push)は **prepare** で確保し、flush は確保済み slot の noexcept な移動だけ
  ([scene.cpp:556](../src/core/loader/scene.cpp#L556) の即時 emit を置換)
- **metadata participant** = `{document, projection, current_scene_id, epoch, active_authority, prefab_provider, provider generation, persistence baseline}` を
  **1 struct にまとめ、commit は 1 回の noexcept swap**([basicconfig.cpp:744](../src/core/loader/basicconfig.cpp#L744) の逐次更新を置換)
- したがって WP380a で `CommittedWithEffectFailure` が発生する経路は無い(flush は noexcept)。型は置くが**到達不能であることをテストで固定**しない
  (到達不能の検証は代理条件)。WP380b が audio / persistence で中身を入れる
- `publish_candidates` は規範どおり **noexcept**。`PreCommitFailure.phase` は `prepare | early_publish | activate` の 3 値(publish を除く)

##### 決定 10(#3、裁定): **refresh root(hot reload)は WP380a の範囲外**

`GameLogicReloader::reloadTransaction` は candidate の検証後に **旧 DLL を unload してから** scene を再構築する
([gamelogicreload.cpp:253](../src/core/gamelogic/gamelogicreload.cpp#L253))。旧 DLL の behavior state を candidate commit まで生存させるには
**2 世代の registration を同時に持つ registry**(ID-A の領域)が要る。refresh root の失敗時契約は**今日のまま「旧 DLL で再構築」**とし、
台帳の既知制限に記録する。WP380a の root は **3 flow・5 call site**: 直接(startup / RPC)/ pending apply / import(versioned / unversioned)

##### 決定 11(#4): `SceneLoadRequest` を型にする

```
SceneLoadRequest = Borrowed{ const SceneProjectionState &committed }
                 | Owned{ SceneProjectionState candidate, AuthoringDocument candidate, MetadataCommitToken token }
```
- `SceneLoader::load(SceneId)` は `load(SceneLoadRequest)` に置換。import の callback(`std::function<void()>`、
  [basicconfig.hpp:137](../src/core/loader/basicconfig.hpp#L137))は **`SceneLoadResult (*)(SceneLoadRequest &&)`** に置換し、
  candidate と result を型で往復する。BasicConfig の局所 candidate([basicconfig.cpp:687](../src/core/loader/basicconfig.cpp#L687))は request に移る
- pending root は **apply 時点の committed projection に late-bind**(今日の意味論。[scene.cpp:562](../src/core/loader/scene.cpp#L562) が scene id だけ保存するのはそのまま)。
  apply 時に scene が無ければ既存の名前付きエラー

##### 決定 12(#5、裁定): **`ModelAssetContainer` は cache であって participant ではない**

失敗した candidate が lazy load した model record / GPU resource / geometry allocation は**残る**(warm cache)。
- 理由: record の所有単位は ModelAsset / reload registry / Material / VertBuf / ray identity に跨り、token 化は本 WP の数倍。
  一方、残った record は **scene state の世代混在ではない**(どの scene にも束縛されていない)
- **決定性の定義**: 同じ入力列(失敗した load を含む)に対して同じ出力。失敗 load の有無で geometry allocation ID が変わるのは
  「入力が違う」のであって非決定ではない。D-vs-control の比較対象は **renderer instance 数 / ordinal / ECS / Light / Camera / Physics** とし、
  geometry allocation identity は**比較しない**と明記
- startup の順序(SceneLoader → Renderer → ModelAsset、[pelican_core.cpp:108](../src/core/userpublic/pelican_core.cpp#L108))は変えない

##### 決定 13(#6): sentinel は **`onInit` が観測したもの**を検査する

- A / B / C で Light の**名前**と intensity、Camera fov、ECS 値を変え、B と C の behavior は **`onInit` で各 participant の値を読み、
  `saveData` ではなく engine-owned な probe(fixture DLL が export する読み取り関数)に記録**する
- 受け入れ: **B の `onInit` が B の sentinel を、C の `onInit` が C の sentinel を観測した**こと(= activation 前に全 candidate が publish 済み)。
  「成功時だけ activation 後に publish する」実装はここで落ちる。Light は `setDirectionalLightIntensity(candidate 名)` の成否でも観測

##### 決定 14(#7): retire は **構造的 no-throw** だけ。GPU に触る作業は prepare か次フレーム

- prepare で **allocation / GPU upload / retirement list 構築**まで完了。commit 後の consume は `swap` / `exchange` / vector move のみ
- Seq / VAT / renderer の**旧 instance の除去**(`removeModelInstance` を呼ぶ [seqplayer.cpp:350](../src/core/playback/seqplayer.cpp#L350)・
  [vatplayer.cpp:48](../src/core/playback/vatplayer.cpp#L48))は transaction 内で行わず、**noexcept な retirement queue に積み、次フレームの
  既存 transient retirement 経路が消費**する。そこでの失敗はフレームのエラーであって transaction のエラーではない(state は既に一貫)
- `static_assert(noexcept(...))` は補助。受け入れは **GPU write fault を prepare に注入して A/A、retire 段には注入点が無い**こと(構造で示す)

##### 決定 15(#8): build ごとの **participant manifest**

- `absent / stub(invariant-empty) / real` を CMake define から生成した manifest で持ち、A/B/C は manifest の行だけを検査する
  (Seq / VAT OFF は stub がコンパイルされる: [playback/CMakeLists.txt:5](../src/core/playback/CMakeLists.txt#L5))
- matrix: **CI 構成(Windows / MSVC / Debug、全 ON)で semantic A/B/C/D**。各 OFF(`RUNTIME_SHADER_COMPILER` / `OPENXR` / `IMGUI` / `PHYSICS` builtin と Jolt /
  `SEQPLAYER` / `VAT`)は **compile + manifest 一致 + 残る行での A/B/C**。flat / xr / preview は renderer participant の 3 variant として manifest の行

##### やらないこと(v3 で追加)

refresh root(決定 10)/ ModelAsset の participant 化(決定 12)/ 公開 mutator journal・audio・persistence(WP380b)/ registry 2 世代(ID-A)。

**WP380a は仕様レビューを 2 巡で打ち切り、WP375 統合後に着手する。** 見積: 大(決定 11 の型変更が 5 call site に及ぶ)。
### WP381: GLABI-2 —— 旧 DLL を**ロード前**に弾く(v3: epoch anchor + 入口改名。OS loader が強制、raw import 読みは診断)

**§4 規則 11 上段(core の ABI・出荷 DLL の挙動が変わる)。設計レビュー済み(carrier §8 修正 γ)。仕様レビュー(2 巡 + 決定差分)+ コードレビュー。見積: 大。**
**規範は `docs/design_declaration_carrier.md` §8 補遺(修正 γ は撤回)。**下の **v3 差分が本節の正**(v2 の import 版方式を v3 で anchor 付きに確定)。
**依存: WP379(統合後。component 登録 ingress を共有)。WP375 / WP380a とは独立。**

#### 何が壊れているか(既存欠陥)

現行の ABI 検査は **`LoadLibrary` の後**にある([gamelogicreload.cpp:137](../src/core/gamelogic/gamelogicreload.cpp#L137) → :148)。
DLL の static initializer は `LoadLibrary` 中に走り、`PELICAN_REGISTER_BEHAVIOR` / `_EVENT` / `_SYSTEM`
([registerer.hpp:298](../src/core/userpublic/details/behavior/registerer.hpp#L298)・
[event/registerer.hpp:242](../src/core/userpublic/details/event/registerer.hpp#L242)・
[system/registerer.hpp:155](../src/core/userpublic/details/system/registerer.hpp#L155))は **ABI が合っているか分からないまま**
`BehaviorRegistration`(`vector<StructFieldSchema>` を含む C++ layout)を host に渡す。layout が違えば UB、合っていても
「登録してから拒否 → release」の順になる(:161)。さらに `initialize()` は **候補を検証する前に `shutdown()` で active を捨てる**
([gamelogicreload.cpp:209](../src/core/gamelogic/gamelogicreload.cpp#L209))ので、再 initialize の失敗は空の状態を残す。
ABI version は今 `gameLogicAbiVersion = 1` の関数 export だけ([gamelogic.hpp:8](../src/core/userpublic/gamelogic.hpp#L8))。

#### 1. section と raw reader(起票ゲート 1)

- SDK は **`.pelabi` PE section**(ELF: `.note.pelican.abi`)に §8 の固定 layout
  (`magic "PELABI\0\0" ‖ major u16 ‖ minor u16 ‖ struct_size u32 ‖ capabilities u64 ‖ build_id[16] ‖ entry_symbol char[64]`)を置く。
  MSVC は `#pragma section` + `__declspec(allocate)`、GCC/Clang は `__attribute__((section))`。SDK 側は `PELICAN_GAME_LOGIC_ABI_SECTION()` 1 macro
  ([gamelogic_entry.cpp:12](../src/player/gamelogic_entry.cpp#L12) の現行 export を置換)
- host は **PE / ELF の section 表を raw で読む** reader(`readGameLogicAbiHeader(path) → expected<AbiHeader, NamedError>`)を
  `src/core/gamelogic/` に置く。**`LoadLibrary` / `dlopen` / `GetProcAddress` を呼ばない**(それが本 WP の目的)
- `build_id` は fixture / 出荷 DLL の CMake が **source contents の SHA-256 から** generated header に埋める(mtime・時計は使わない)。
  本 WP では **log と `GameLogicStatus` に載せるだけ**(manifest の消費は CARRIER-STUDIO)

#### 2. 1 entrypoint と wire(起票ゲート 2・3)

- `extern "C" pelican_gl_status pelican_game_logic_register_v2(const pelican_gl_host_api *, pelican_gl_registration_sink *)`。
  host が両 struct を所有、先頭 `struct_size u32 ‖ version u32`、関数 pointer 表。**STL / `type_info` を境界越えさせない**。
  例外は境界で `internal`(3)に落とす
- **SDK v2 の登録 macro は host state を変える static initializer を持たない**。macro は **DLL 内の static 表**へ追記し、
  entrypoint がその表を歩いて `sink->register_descriptor(kind, version, bytes, len)` を呼ぶ。
  kind は `behavior_v1 / event_v1 / system_v1 / component_v1` と **`schema_v1`(番号予約のみ。wire は CARRIER-RT3)**。
  Behavior の既存 `BehaviorRegistration`(`vector<StructFieldSchema>`)は **`behavior_v1` の length-delimited wire に encode / decode**
  し、host 側の registry 型は変えない(registry は WP の範囲外)
- **未知 kind は `UnknownDescriptorKind{kind, version}` で candidate 全体を拒否**(skip しない)
- 旧 `pelican_game_logic_abi_version` export は **廃止**。`gameLogicAbiVersion` は **2** に上げ、意味を「section の major」にする

#### 3. transaction(起票ゲート 4)

- §8 の owner kind 表(Behavior / GameSystem / Event / Component、Animation、render 5 provider、vulkan native scope、physics、registration owner)を
  **`GameLogicReloader` 内の 1 つの表**にし、candidate / commit / rollback を行ごとに持つ。
  `releaseGameLogicRegistrations` ([gamelogicreload.cpp:43](../src/core/gamelogic/gamelogicreload.cpp#L43))と
  `activateProviderOwner` の並び(:232〜:239、:279〜:286)は**その表から生成**する(2 箇所の手書き列挙を 1 箇所へ)
- candidate は **`allocateRegistrationOwner()` の新 owner** に登録し、検証は **canonical key 順**で行う(登録順に依存しない)
- **`initialize()` は `shutdown()` の前に preflight**(raw header 検証 + candidate load + 検証)。失敗したら active も registry も**触らない**。
  reload 経路(:253)は既に validate → unload なので、同じ関数を使う

#### 4. エラー(起票ゲート 5)

`GameLogicAbiMissing`(section 無し = **v1 DLL**)/ `GameLogicAbiMalformed`(magic・struct_size)/
`GameLogicAbiUnsupported{found_major, required_major}` / `GameLogicEntryMissing`(section は v2 だが entry symbol 無し)/
`UnknownDescriptorKind{kind, version}`。**5 つは別々の名前付きエラー**で、`GameLogicStatus.last_error` と RPC の
`get_game_logic_status` に **code と field** で載る(文字列一致に依存させない)。`GameLogicArtifactMismatch` は本 WP では**定義だけ**。

#### 受け入れ(§4 規約 10。起票ゲート 5・6)

- **fixture DLL 4 種**を `test/CMakeLists.txt` の WP162 variant 系列([test/CMakeLists.txt:441](../test/CMakeLists.txt#L441))に足す:
  (i) **stale v1**: section 無し・`pelican_game_logic_abi_version` export・static initializer が **`__registerComponent("wp381_stale_probe")`
  を呼ぶ**(host の component registry がそのまま sentinel。test hook は足さない)
  (ii) **forged v2**: section あり・entry symbol 無し (iii) **malformed**: magic 不一致 (iv) **valid v2**: v2 macro で書いた最小 DLL
- **matrix**: initial load / reload / 再 initialize × (i)(ii)(iii)(iv) を **production `GameLogicReloader`**(`initializeConfiguredGameLogic` /
  `pollGameLogicReload` の実経路)で回す。(i)(ii)(iii) は名前付きエラーの **code** が一致し、
  **active handle・generation・全 registry の key 集合が試行前と一致**、`wp381_stale_probe` は**存在しない**。
  対照: raw header 検証の呼出を消すと (i) で `wp381_stale_probe` が現れる(今日の挙動)。(iv) は成功し generation が進む
- **rollback**: (iv) の後に **`register_descriptor` が途中で `registration_rejected` を返す DLL**(重複 key)を reload し、
  active が (iv) のまま・registry が (iv) と一致。**表の 1 行ずつ**(Animation・render provider・physics …)の rollback を消す変異が
  それぞれ落ちる(aggregate の 1 変異では足りない)
- **canonical key 順**: 同じ 2 登録を逆順に並べた DLL 2 つで、拒否理由の code と field が一致
- **例外境界**: entrypoint 内で throw する DLL が `internal` で拒否され、host は継続
- **出荷 4 project**: game DLL を v2 macro に移し、`project_catalog_headless_smoke`(`-DPELICAN_TEST_PROJECTS_DIR`)が通る。
  bytes 不変は **scene / project JSON** について(DLL は変わる)。golden 不変
- `SKIP_DEVSTUDIO` 両構成・`PELICAN_WITH_PHYSICS` ON/OFF でビルド。全数(着手時に実測)。GPU は統合側で直列

#### やらないこと

sidecar / helper process 隔離 / manifest 生成(CARRIER-STUDIO)/ `schema_v1` wire 本体(CARRIER-RT3)/ registry 型の変更 /
Behavior の layout 変更 / hot-reload の teardown・rebuild 順序(WP380 の領域)/ `GameLogicArtifactMismatch` の検査。

#### WP381 仕様レビュー第 1 巡(不合格・13 件)と v2 —— **section も C ABI も捨てる。ABI は import symbol の版で強制する**

仕分け: **機構ごと削除 8**(#1 #2 #3 #5 #6 #7 #8 #9)/ **直す 4**(#4 #10 #11 #12)/ **依存を訂正 1**(#13)。

**なぜ機構を捨てるか。** レビューの #5〜#8 が示したのは、「embedded section + 単一 C entrypoint」は
(a) **enforcement ではない**(新 entrypoint と旧 static registrar を同じ DLL にリンクすれば preflight を通って旧 constructor が走る)、
(b) 完全に固定するには **host / DLL 境界の全部を C ABI にする**か、provider 含む全 registry を candidate staging にする必要があり、
GLABI-2 の目的(旧 DLL を実行前に止める)と釣り合わない、の 2 点である。一方、OS の loader は **import の解決を static initializer より前に行う**
(Windows: `LoadLibrary` は import 未解決なら DllMain 前に失敗 / ELF: `RTLD_NOW` は relocation を initializer の前に完了する。現行は
[gamelogicreload.cpp:139](../src/core/gamelogic/gamelogicreload.cpp#L139) で `RTLD_NOW`)。**ABI の enforcement は import symbol の名前に版を入れるだけで OS が行う。**
section・raw PE section reader・`build_id`・C entrypoint・descriptor wire・`schema_v1` 予約・candidate staging は**全部作らない**。

##### 決定 1(#5 #6 #7 #8 を消す): **ABI = DLL が import する host symbol の版付き名前**

- host が export し DLL が import する登録・emit 入口は今 4 本: `__registerBehavior`([registerer.hpp:190](../src/core/userpublic/details/behavior/registerer.hpp#L190))/
  `__registerEvent`・`__emit`([event/registerer.hpp:64](../src/core/userpublic/details/event/registerer.hpp#L64))/
  `__registerSystem`([system/registerer.hpp:95](../src/core/userpublic/details/system/registerer.hpp#L95))+ component の `__registerComponent`
- これらを **`pelican_register_behavior_v2` / `pelican_register_event_v2` / `pelican_emit_v2` / `pelican_register_system_v2` / `pelican_register_component_v2`** に改名し、
  **旧名は export しない**。SDK macro(`PELICAN_REGISTER_*`)は変えない(ソース互換。18 fixture variant と出荷 4 project は再ビルドだけで v2)
- **規則**: 境界を越える struct(`BehaviorRegistration` / `EventTypeRegistration` / `GameSystemRegistration` / `QueuedEvent` / `ComponentLoaderInfo`)の
  layout を変えるときは **その入口の版を上げ、旧名を消す**。CARRIER-RT3 の再帰 schema は `pelican_register_behavior_v3` になる(major を上げる儀式は無い)。
  `gameLogicAbiVersion`([gamelogic.hpp:8](../src/core/userpublic/gamelogic.hpp#L8))は **2** に上げ、意味を「入口 symbol 世代の総和」にする(post-load の二重検査として残す)
- **provider(render 5 種 / vulkan native scope / physics / animation)の static 登録は今のまま**(owner scope 下。#7 の「1 entrypoint」主張は撤回)。
  それらの入口 symbol にも同じ版付け規則を適用する(今回は layout 不変なので改名しない。inventory を台帳に列挙)
- #6(candidate と active の同名 component 衝突)は**既存の制限**であり本 WP の外。台帳の既知欠陥に記録(registry 世代化 = ID-A の領域)

##### 決定 2(#2 #3 #9 を消し、#4 を直す): ロード前の検査は **raw PE / ELF の import table を読む診断**。強制は OS

- `readGameLogicImports(path)` が PE import directory / ELF `.dynsym`(undefined)を raw で読み、**旧世代の名前(`__registerBehavior` 等の
  legacy inventory)を 1 つでも import していれば `GameLogicAbiStale{symbol, found_generation, required_generation}`** で `LoadLibrary` を呼ばずに拒否。
  読めない・PE でない → `GameLogicAbiUnreadable{reason}`。inventory に無い `pelican_core.dll` の未知 import → `GameLogicAbiUnknownImport{symbol}`
- import が全部現行なのに `LoadLibrary` が失敗 → 既存の `GameLogicLoadFailed{os_code}`(今の文字列を code 化)
- section・export table の解析はしない(entrypoint が無いので forged の概念が消える)
- **`initialize()` は `shutdown()` の前に import 診断を行う**([gamelogicreload.cpp:209](../src/core/gamelogic/gamelogicreload.cpp#L209))。
  reload 経路の「validate で load → unload → 再 load」([gamelogicreload.cpp:184](../src/core/gamelogic/gamelogicreload.cpp#L184)・:253)は
  **変えない**(shadow copy は attempt ごとに一意で `.pelican-hot-reload` の下。path 差し替えは本 WP の脅威モデル外と明記)

##### 決定 3(#4 #10): fixture は **frozen v1 header + v1 import stub** で作る。sentinel は registry の外

- `test/fixtures/userpublic_frozen/v1/` に **本 WP 着手時点の `src/core/userpublic` を snapshot** としてチェックイン(WP380b の frozen-header fixture と共用。
  先に着地した方が置く)。stale fixture の include path に **現行 userpublic を入れない**ことを CMake で assert
- **v1 import stub**: `pelican_core_v1.def`(旧 5 symbol)から `lib /DEF /NAME:pelican_core.dll` で import library を生成し、stale fixture はそれにリンクする
  (import table は `pelican_core.dll!__registerBehavior` を指す。v2 host はそれを export しないので OS が拒否する)
- **sentinel**: stale fixture の static constructor は **自分の DLL と同じディレクトリに `wp381_stale_probe.txt` を作る**(registry rollback の外で観測できる)。
  加えて `__registerBehavior` を呼ぶ(frozen header 経由)
- fixture の役割は **6**: stale v1 / 現行 v2(WP162 variant をそのまま)/ 重複 key で登録拒否される v2 / 逆順 2 本 / 登録中に throw する v2
  (「4 種」は撤回)。全 DLL target(18 variant + provider fixture + 出荷 4 project)の inventory と分類(v1 固定 = stale fixture のみ / 残り全部 v2 再ビルド)を台帳に置く

##### 決定 4(#12): status は実在型を拡張する

- `GameLogicReloadStatus`([gamelogicreload.hpp:14](../src/core/gamelogic/gamelogicreload.hpp#L14))と `RuntimeReloadResult`
  ([reloadservice.hpp:47](../src/core/watch/reloadservice.hpp#L47))に **`error_code`(string、`@1` 付き名前)と `error_fields`(ordered_json)** を足す。
  `last_error` / `error` の文字列は残す
- RPC は既存の **`get_status` と `reload_game_logic`**([rpcserver.cpp:1116](../src/core/communication/rpcserver.cpp#L1116))の応答に同じ 2 field を足す。
  新 method は作らない。Studio は JSON だけを見る

##### 決定 5(#13): 依存の訂正

- **WP375**: registry generation 契約に触らない(owner promotion の経路は今のまま)→ 独立
- **WP379**: component 登録の ingress policy(WP379 決定 B)に触らない(改名するのは export 名、policy 関数は WP379 の所有)→ **同じファイルを触るので WP379 統合後**に着手
- **WP380a**: 独立。**WP380b** は frozen header fixture を共用(先に着地した方が置く)
- carrier §8 の順序「GLABI-2 → ID-B」は **CARRIER-RT3 の前提**としての順序であり、WP380a との依存ではない

##### 決定 6(#11): 受け入れ(§4 規約 10)

- **stale**: 3 root(initial / reload / 再 initialize)で stale fixture → `GameLogicAbiStale` の code と `symbol` field、
  **`wp381_stale_probe.txt` が存在しない**、active handle / generation / 全 registry の key 集合が試行前と一致。
  **対照: host に旧名 `__registerBehavior` の export を 1 本戻す変異で、probe file が現れる**(= 今日の挙動)
- **診断の対照**: `readGameLogicImports` の呼出を消すと、同じ stale fixture が `GameLogicLoadFailed{os_code=127}` に落ちる(code が変わる)。
  OS の enforcement は残るので probe は現れない —— これが「診断と強制は別」の観測
- **v2 正常**: 現行 18 variant と出荷 4 project の DLL を **raw import 診断に通し、legacy 名 0・全 import が inventory 内**。
  `project_catalog_headless_smoke` は起動確認として残すが、それとは別に **各出荷 project の DLL を production loader で load して `system_count` が着手時実測値と一致**
- **rollback**: 重複 key fixture で active が前のまま・registry 一致。throw fixture で `internal` 相当の code と継続
- **`initialize()` の順序**: 再 initialize に stale を渡すと **active が残る**(今日は `shutdown()` 後に空 = 対照)
- `SKIP_DEVSTUDIO` 両構成・`PELICAN_WITH_PHYSICS` ON/OFF。全数(着手時に実測)。GPU は統合側で直列。CI は Windows/MSVC/Debug のみなので ELF 側は **コードは置くがテストは Windows で skip ではなく未登録**と明記

##### やらないこと(v2 で追加)

section / raw section・export reader / `build_id` / C entrypoint・descriptor wire / candidate registry staging(ID-A)/ provider 入口の改名 /
validate の二重 load 解消 / manifest(CARRIER-STUDIO は final artifact の SHA-256 を使う)。

#### WP381 仕様レビュー第 2 巡(不合格・10 件)と v3 —— **決定差分(レビューはここで打ち切り。残余はコードレビューへ)**

「import symbol の版で OS が止める」中心判断は、**通常・直接・名前付き・非 weak の IAT import に限れば**支持された(壊せなかった項目)。
落ちたのは (i) 全 DLL が必ず import する版付き symbol が無い、(ii) 否定対照が論理的に成立しない、(iii) stub の module 名と装飾名が実物と違う、の 3 点。
仕分け: **直す 8**(#1 #2 #3 #4 #5 #7 #8 #9)/ **範囲から外す 2**(#6 ELF、#10 throw fixture)。

##### 決定 7(#1 #9): **epoch anchor** を全 game DLL が import する。epoch は単調増加の scalar 1 箇所

- `gamelogic_entry.cpp`([gamelogic_entry.cpp:11](../src/player/gamelogic_entry.cpp#L11))が **`pelican_game_logic_epoch_v<N>`**(C linkage、引数無し、
  host の nonce を返す)を**通常 import で参照**する。`pelican_configure_game_logic_target` を通る DLL は登録ゼロでも必ずこれを import する
- `gameLogicAbiVersion`([gamelogic.hpp:8](../src/core/userpublic/gamelogic.hpp#L8))= **SDK epoch**(単調増加、今回 2)。anchor 名は epoch から導出。
  「入口世代の総和」は撤回。境界 struct の layout を変える WP は **epoch を 1 上げる**(入口ごとの版は持たない)
- 今回だけ、既存 v1 DLL を止めるために **4 つの PELICAN_API 入口**(`__registerBehavior` / `__registerEvent` / `__emit` / `__registerSystem`)を
  `pelican_register_behavior_v2` 等へ改名し旧名を消す(`__registerComponent` は `PELICAN_API` ではないので**入口に数えない**。#5)。
  以後の epoch 昇格では改名せず anchor だけで止まる(旧 anchor を export しないため)
- provider 入口(render 5 種 / vulkan / physics / animation の `getApiV1`)は改名しない。provider-only の v1 DLL は **anchor 不在の診断**で止める(決定 8)

##### 決定 8(#2 #7): 保証範囲と診断の規則

- **保証範囲**: SDK が生成する **通常・直接・名前付き・非 weak** の import。`/DELAYLOAD`・ordinal・`GetProcAddress`・依存 DLL 経由・`dlsym` は範囲外
  (文書化。任意バイナリの無実行保証は helper process 隔離 = 将来 WP)
- raw PE import reader は **top-level のみ**を読み、host module(`pelican_player.exe` / test EXE)への import について:
  anchor `pelican_game_logic_epoch_v<N>` が **無い** → `GameLogicAbiStale{expected_epoch, found: absent | v<M>}` /
  旧名 4 つのどれかがある → 同 code に `legacy_symbols[]` を載せる / host module への **delay-load・ordinal import** → `GameLogicAbiUnsupportedImport{kind}` /
  読めない → `GameLogicAbiUnreadable{reason}`。すべて `LoadLibrary` を呼ばない
- **`initialize()` に active があるときは reload 経路に委譲**(validate → teardown → swap)。active 無しでも raw 診断は `shutdown()` の前。
  これで provider-only stale / architecture 不一致 / 依存欠落 / initializer 失敗のどれでも **active は残る**(post-load 失敗は reload 経路の既存 rollback)

##### 決定 9(#3): stale fixture は **host EXE 名の v1 import stub + frozen header**。post-link で監査する

- host module は `pelican_core.dll` ではなく **`pelican_player.exe` / 各 test EXE**([src/player/CMakeLists.txt:30](../src/player/CMakeLists.txt#L30))。
  loader テストの EXE は 1 つなので、その EXE 名で **`lib /DEF:<v1.def> /NAME:<test exe>`** を生成する。`.def` は改名前の **MSVC 装飾名**
  (`?__registerBehavior@UserBehaviorRegistererTemplatePublic@…`)を着手時に `dumpbin /exports` から採取してチェックイン
- frozen header: `test/fixtures/userpublic_frozen/v1/`(着手時点の snapshot)。stale fixture は **専用 CMake 関数**(`pelican_configure_game_logic_target` を呼ばない)で、
  include に現行 `userpublic` を入れず、現行 player / test host import library にもリンクしない
- **post-link 監査**(テストとして登録): `/sourceDependencies` を canonicalize して現行 `src/core/userpublic` 配下が 0 件、`dumpbin /imports` 相当で
  module 名と import symbol 集合が期待どおり、host EXE の export に旧名 4 つが **0 件**
- 想定される OS error は **127**(procedure not found)であって 126 ではない(module は存在する)

##### 決定 10(#4): 否定対照は **3 枝を同一テスト内**で

1. 診断 ON・旧 export なし → `GameLogicAbiStale`、probe なし、active / generation / registry 不変
2. **診断 OFF(reader の呼出を消す変異)**・旧 export なし → `GameLogicLoadFailed{os_code=127}`、probe なし(OS が止めた証拠)
3. **同じ constructor を持つ v2 fixture**(anchor と現行入口を import)→ load 成功、constructor が **anchor の返す host nonce を呼出後に probe へ書く**。
   probe の値が host の nonce と一致(= constructor が走り、入口が解決された証拠)
- probe の置き場は **loaded module 自身のディレクトリ**(`GetModuleFileName`。loader は shadow copy を load するので `.pelican-hot-reload` の下)
- 「host に旧 export を 1 本戻す」対照は撤回(reader が candidate の import を見る以上、host の export は結果に影響しない)

##### 決定 11(#5): 入口ごとの stale fixture と、生成される inventory

- 旧名 **4 つそれぞれを単独で import** する fixture 4 本(+ 決定 9 の behavior 本体 fixture、+ 決定 10 の v2 対照、+ 重複 key、+ 逆順 2 本 = **9 role**)。
  reader の判定を 1 入口分だけ消す変異がそれぞれ落ちる
- inventory は手書きせず **`PELICAN_GAME_DLL` target property** から生成し、`loader 対象` / `link-only`(`pelican_public_api_link_fixture`
  [test/CMakeLists.txt:369](../test/CMakeLists.txt#L369))を分類。physics ON で 19 target
- 出荷 4 project は `system_count` に加え、**各 DLL の import に anchor があり旧名が無い**ことを raw reader で検査(sprite_demo の event 入口も import 集合で観測)

##### 決定 12(#8): RPC は **`error.data`** に構造化 code を置く

- `reload_game_logic` の失敗は今どおり JSON-RPC error(数値 `error.code` と message 維持)。**`error.data = {error_code, error_fields}`**
  ([jsonrpc.cpp:224](../src/project/jsonrpc.cpp#L224) の契約どおり)
- `GameLogicReloadAttempt`([gamelogicreload.hpp:25](../src/core/gamelogic/gamelogicreload.hpp#L25))/ `RuntimeReloadResult` / `RuntimeParticipantStatus`
  ([reloadservice.hpp:163](../src/core/watch/reloadservice.hpp#L163))に `error_code` / `error_fields` を足し、文字列から再推定しない
- `get_status` は `participants.game_logic.error_code` / `.error_fields`。**次の成功で両方 clear**。受け入れは RPC wire で失敗応答 → `get_status` → 成功後 clear を同一テストで
- `PELICAN_WITH_RPC` OFF もビルド(`rpcserver_stub.cpp`)

##### 決定 13(#6 #10、裁定): ELF reader と throw fixture は範囲外

- **ELF**: 本 WP は PE reader だけ。Linux は診断無しで OS enforcement(`RTLD_NOW`)のみ。GNU symbol version / `PT_DYNAMIC` の扱いは
  Linux CI ができたときの WP(台帳の既知制限に記録)
- **throw fixture**: C++ 例外が OS loader / CRT initializer 境界を越えた結果は OS 間で定義できない。fixture から外し、
  「登録拒否(重複 key)」だけを post-load 失敗の対照にする

##### 段割りと依存(v3)

依存: **WP379 統合後**(component 登録 ingress は触らないが、同じ test EXE / fixture 系列を触る)。仕様レビューは 2 巡で打ち切り。
見積: **中〜大**(anchor + 4 入口改名 + reader + `initialize()` 順序 + fixture 9 role + RPC data + 監査テスト)。

### 未了項目と再開順(2026-09-02 夜。codex は使用上限で停止 → 同夜に利用者がリセットして再開。**以後 fast(service_tier=priority)は使わない**)

2026-09-02 深夜 再開: `rev377a` `rev375a` を再投入、**WP379 と WP380a を並列投入**(worktree `<作業ディレクトリ>/wp379` / `<作業ディレクトリ>/wp380a`)。
両レビューとも reject: `rev377a` の 2 件は連鎖上限で既知の穴へ、`rev375a` の致命(Preview の inactive light 置換)は **WP375b** として投入(worktree `<作業ディレクトリ>/wp375b`。WP375 系はこれで打ち切り。次に致命が出たら revert を先に検討)。
**統合済みで残っているもの**
- WP377a(`a3f318fb`)のコードレビュー `rev377a`: 判定前に中断。**再投入**(観点は `<作業ディレクトリ>/rev377a.txt`)
- WP375a: **統合済み**(`1aeafc59`)。コードレビュー `rev375a` は再開後に投入(観点は完了記録の末尾)

**再開後の投入順**(すべて仕様確定済み。台帳の各節の最終差分が正)
1. `rev377a` / `rev375a`(コードレビュー。read-only)
2. **WP379**: 統合済み(`51841eb3`、2026-09-03)。→ **WP381**(v3。epoch anchor): **投入済み**(worktree `<作業ディレクトリ>/wp381`。shadow dir の process 分離を同梱)
3. **WP375b**: 統合済み(`c61ce078`、全数 1544 / 1544)。**WP380a**(v3): 実装 1 回目が完了(参加者 drop / bypass 変異が manifest の ID だけで未実行 → fault injector 化の継続 + main 取り込みを投入)
**WP379a**(rev379 の致命: engine registrar が DLL ヘッダに露出)を投入(worktree `<作業ディレクトリ>/wp379a`。WP379 系はこれで打ち切り)
4. **WP378**(v3。E-0a): **投入済み**(worktree `<作業ディレクトリ>/wp378`、WP375b 統合直後に自動起動)
5. WP380b / CARRIER-RT3 / CARRIER-STUDIO は上の後に節を切り直す

**既知の穴(WP 化は別途判断)**
- compiler OFF で guard の無い GPU テスト 25 件(WP377 完了記録の matrix 参照)
- WP334b 系の `-j4` 負荷 flake(commit 待ちタイムアウト。今日 5 回)
- game-logic fixture DLL の shadow copy 衝突(`.pelican-hot-reload` が DLL 横で process 間共有。`-j4` で同じ fixture を使うテストが衝突。WP375a 統合時に 2 件)→ **WP381 の loader 変更に「shadow dir を process ごと」を同梱**
- **CRT の Debug assertion dialog を test harness が抑止していない**(2026-09-03、WP375b の変異 P1 中に `vector erase iterator outside range` の modal dialog が出て自動実行が止まった)。Catch2 の main で `_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE)` + `_CrtSetReportFile(..., _CRTDBG_FILE_STDERR)` + `_set_abort_behavior(0, _WRITE_ABORT_MSG)` を入れ、assert を stderr + 非 0 終了にする(小。次の test-infra 変更に同梱)
- loader: 設定 DLL が起動時に無く、後から不適合 DLL が現れた reload で旧 runtime を再構築しない(`rev375` #3。WP381 / WP380 の領域)
- **RT frame transaction の SBT / TLAS rollback(WP377b 候補、`rev377a` #1)**: `after_as_record` / `submit` の fault で却下世代の SBT が `FrameResources` に残り、candidate TLAS が frame descriptor に書かれたまま runtime だけ旧世代へ戻る。修正設計: frame 開始時の旧 SBT lease と TLAS descriptor を保持する **rollback guard**(AS commit + staged generation publication が終わるまで例外時に復元。RT→非 RT の clear も対象)。受け入れは既存 transaction test(`headless_render_test.cpp:11833`)の各 fault 点で失敗直後の active SBT / TLAS 同一性。**次に renderer の frame transaction を触る WP に同梱**(連鎖上限のため単独起票しない)- **AS row identity の canonical 化(`rev377a` #2)**: `{document, material_name, asset}` の 8 通りの truth table のうち `D=1,N=0,A=1` が gate と material lookup で食い違う。canonical identity(document 組があれば document、無ければ asset)を 1 箇所で決めて AS rule / binding / `materialReference` / material-table lookup で共有し、8 行を parameterized test に。CARRIER-RT3(occurrence identity)に同梱
- **AS row identity の canonical 化(`rev377a` #2)**: `{document, material_name, asset}` の 8 通りの truth table のうち `D=1,N=0,A=1` が gate と material lookup で食い違う。canonical identity(document 組があれば document、無ければ asset)を 1 箇所で決めて AS rule / binding / `materialReference` / material-table lookup で共有し、8 行を parameterized test に。CARRIER-RT3(occurrence identity)に同梱
