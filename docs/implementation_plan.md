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

### WP358(= リファクタ R4): 型語彙を leaf へ移し、offline 検証から届くようにする

**§4 規則 11 の中段。仕様レビュー + コードレビュー。プレファブ U1 の前提。**

#### 事実(プレファブ設計レビュー 2 巡で確定済み)

- `StructFieldType` の enum は `src/core/userpublic/details/schema/structfieldschema.hpp:19-39`
  にあり、**型名への文字列変換は core の RPC 実装内**(`editorcommandservice.cpp:65-106`)
- 依存方向は **core → `pelican_project`**(逆は不可)。studio は project のみリンク
- したがって **project 層(offline の文書検証、将来のプレファブ parameter 検証)から
  型語彙に届かない**。変換が RPC 実装に埋まっているため二重実装しか道が無い

#### やること

1. 型 enum・文字列変換・parameter JSON 検証(値の型/range/enum/unit 検査)を
   **`pelican_project` 配下(または両者の下の leaf)へ移す**
2. core の `StructFieldSchema` はそれを**使う側**に回る
3. **`editorcommandservice.cpp` の変換は移設先の呼び出しに置き換える**(複製を残さない)

#### 地雷(先に名指しする)

- **`structfieldschema.hpp` は game DLL の SDK 面である**(behavior の `Params::schema` が使う)。
  **DLL のソース互換を壊さないこと**: `userpublic` 側の header は移設先への
  forwarding include として残し、既存の behavior コード(fixture 3 種 +
  projects/*/code)が**無変更でコンパイルできる**ことを検査する
- schema fingerprint(reload の漂流検知)が**移設の前後でバイト同一**であること。
  変わると全 behavior が「schema 漂流」と誤検知される
- 型名の文字列(RPC の schema_fields が返す語彙)が 1 文字も変わらないこと
  (studio の widget 生成が読む)

#### 受け入れ条件

- **移設先の関数を project 層のテストから直接呼べること**(offline 検証の成立証明)
- **behavior fixture(wp155/162/179)と 4 project の code/ が無変更でビルドされること**
- **schema fingerprint の前後一致**を、実 behavior 登録で検査(変異: 語彙を 1 つ
  変えると fingerprint が変わって検知される、も同じテストで)
- RPC の `schema_fields` 出力がバイト不変(既存 captured fixture との一致)
- 変換の複製が残っていないこと(旧実装の grep 0 件)
- 全数 2 回、doclink 緑、SKIP_DEVSTUDIO 両構成、GPU はエージェント外

#### やらないこと

プレファブ本体 / bindable inventory(U1 側)/ 型語彙の拡張(object 種別等は U1 で)。

依存: 無し。見積: 小〜中。
