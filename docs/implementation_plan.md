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

- 完了条件は常に「ビルド成功 + 全テストグリーン + `git diff --check` クリーン」
- 描画挙動に触れる WP は `pelican_player.exe` の短時間起動確認も行う(`docs/rendering_phase1_review.md` の Validation Run と同じ流儀)

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

WP206b の pass-local material variant slice を閉じた後の描画候補は次。番号は実装順を固定するための
予約であり、各候補は着手前に下記の設計/受け入れ条件をレビューして active へ昇格する。

| 候補 | 内容 | 状態 |
|---|---|---|
| WP207a | compute Frame/Light + sampled resource port | ✅ 完了（2026-07-26、archive） |
| WP207b | material/geometry typed frame-graph resource port | ✅ 完了（2026-07-26、archive） |
| WP208 | lighting data contract v2 + clustered dogfood | ✅ 完了（2026-07-26、archive） |
| WP209a | static texture dimension + material sampler authoring | ✅ 完了（2026-07-26、archive） |
| WP209b | RT mip/layer/subresource view | ✅ 完了（2026-07-26、archive） |
| WP210 | indirect dispatch + GPU-written draw arguments | 🟡 WP210a + WP210b fixed-state draw/count完了（2026-07-27）。一般GPU culling受け入れは継続 |
| WP211 | `dist-bake` + shaderc OFF feature delivery | 並行候補・配布/Quest前必須 |
| WP212 | VRS/foveation backend contract | Quest SA2 device/計測待ち |

完了済み WP の一覧・依存関係・本文は
[`implementation_archive.md`](implementation_archive.md) に逐語保存する。
(最新の全受け入れ完了: WP214、2026-07-26。WP203c はローカル実装・自動テスト済みだが、
Simulator/物理 HMD と対象 GPU の実測を残すため active のまま。)

## 2. WP 詳細

### WP213 / WP214: 版の単一化

両 WP は2026-07-26に完了した。実装内容・移行判断・全受け入れ結果は
[`implementation_archive.md`](implementation_archive.md) と次の完了レポートを参照:

- [`design_reviews/2026-07-26_wp213_report.md`](design_reviews/2026-07-26_wp213_report.md)
- [`design_reviews/2026-07-26_wp214_report.md`](design_reviews/2026-07-26_wp214_report.md)

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
4. open external boundary、complete raw physical plan、`NativeScope`

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
実GPU dogfood済み。raster attachmentの任意mip/layer出力、material portのsubresource、
runtime 3D/cube targetは後続の明示拡張であり、未対応設定は黙って無視せず拒否する。

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
WP210の残りは、hot reload、XR per-view、GPU timing受け入れである。

**受け入れ条件**:

- ✅ occlusion cullingをproject-owned fixed-state dogfoodにする
- GPU生成count 0/1/max/overflow、rollback、hot reload、XR view count test
- CPU fallbackとのsemantic image一致
- GPU timingでCPU pathより有利なworkload範囲を記録
- descriptor pressureが実測blockerになるまでbindless WPを作らない

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
same-pixel fullscreen readのtile-local scope fusion、lifetime非重複imageのallocation共有まで
実行できる。次はscope fusion/reorder、一般のmaterialized store elision、queue/barrier等の
aggressive controlと、現runtime subsetの範囲拡張を具体的なGPU gate付きで進める。
`NativeScope`は具体的な
Vulkan-only利用例を得てから進める。

WP205でfeature-owned `directional_shadow` contractをstandard surfaceのpass-input ABIへ
接続した。shadow image、shared 2D view policy、manual depth compare、light indexと
light-space transform relationをtyped metadataとして保持し、同梱featureとprojectへ
コピーしたfeatureを同じ契約で実行する。feature未参照時は追加pass/resource/descriptorを
持たず、`pelican_shadow()`は従来どおり`1.0`へ解決する。B-layer Vulkan golden、
feature-off shader byte、flat/preview/XR view contract、resize/hot reload/rollback、
全894 CTestとvalidation error 0を確認済みである。

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
