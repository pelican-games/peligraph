# WP222 coordinated render reload 実装レポート

日付: 2026-07-29

状態: **実装・CPU fixture・headless Vulkan gate完了**

## 結論

render graph、surface shader、graphics pipeline、material metadata/valueを別々に
hot reloadしていた公開境界を、一つのcandidate transactionへ統合した。
候補graphがmaterial output schema、format、sample count、local-read ABI、
attachment stateを変えても、全live materialを候補generationへ再bindし、
必要なshader/pipelineを完成させてからだけruntime rootを公開する。

途中のparse、shader compile、reflection、device capability、pipeline作成、
material value検証、stale publicationのいずれかが失敗した場合、graph、shader、
pipeline、material schema/valueはすべて旧世代を維持する。

## 実装したtransaction

1. flat/preview/XRをprivate `RendererRuntimeGeneration`と
   `RenderPipelineGpuRegistrationArena`へcompileする
2. `MaterialContainer::prepareRuntimeGenerationReload()`が全live materialについて
   route/pass、output schema/format/sample、local-read mapping、attachment state、
   resource descriptor ABIを候補generationから解決する
3. `ShaderLibrary`がphysical defineとmaterial output schemaのoverrideを受け、
   file-backedまたはembedded surface recipeから影響shader bundleをprepareする
4. `PipelineFactory`がshader変更の有無にかかわらず、変化した
   `GraphicsPipelineDesc`で既存handleのreplacementを全て構築する
5. material valuesを候補layoutへvalidate/lowerし、material metadata commitとともに
   pre-publication callbackへまとめる
6. `FrameGraphRuntimeContainer`のpublication mutex内でbase-generation stale検査、
   pre-publication commit、active-root CASを連続して行う
7. 成功後だけGPU arena、shader/pipeline replacement、watch dependencyを確定し、
   旧GPU payloadは既存generation leaseとdeferred deletionで退役する

stale検査より前にmaterial commitを行わない。これにより、別transactionに先を越された
candidateがgraph公開には失敗したのにshader/materialだけ更新する競合を防ぐ。

## watcher batchの所有

`ReloadService::ReloadParticipant`へ、通常claimとは別に次を追加した。

- `companion_participants`
- request単位の`companion_claims`
- ownerがまとめて適用する`apply_with_companions`

render-pipeline participantは同じwatcher batchのshader requestとmaterial-values requestを
選択する。選択されたrequestはownerの成否を共有し、各participantから二重適用しない。
texture requestはgraph physical ABI transactionへ不要なため選択せず、通常のasset batchで
処理する。通常claimの一意性は維持する。

## surface / pipeline ABI

standard material等のengine-generated surfaceも、生成時の
`SurfaceFormatDocument`をembedded compiler recipeとして保持する。したがってprojectの
`.surface`ファイルを持たなくても、graph schema変更時に同じsurface compilerから
fragment output ABIを再生成できる。

同じshader bundleまたはpipeline handleへ複数materialから異なるcandidate ABIが要求された
場合は、最後の一つを黙って採用せずconflictとしてpublish前に拒否する。必要な独立性は
material登録時にhandleを分けて表現する。

## 対応する変更

- material output schemaの名前、順序、型、枚数
- color/depth formatとsample count
- sampled imageとdynamic-rendering local-readの切替、input attachment index
- attachment別blend equation / write mask
- graph変更と同じwatcher batchのsurface shader body変更
- graph変更と同じwatcher batchの同layout material value変更
- file-backed / engine-generated surface

logical schemaの枚数上限は追加していない。実際の上限は引き続きdevice capabilityと
surface source/reflectionで決まる。

## 意図的に残した境界

次は単なるphysical graph ABI追従ではなくmaterial authoring構造の変更なので、
既存material-values structural validationの対象に残す。

- material route / exact pass selection
- surface `render_state`
- screen input / resource port declaration
- custom material value layout
- 一つの共有shader/pipeline handleを互換でない複数ABIへ分岐する変更

これらを将来解禁する場合も、WP222のcandidate/publicationを再利用し、in-placeな
部分更新経路は追加しない。`PELICAN_WITH_SHADERC=OFF`の配布環境で新しいsurface ABIを
生成する経路はWP211 `dist-bake`の責務である。

## 受け入れ結果

- `pelican_test_reloadtransaction_test.exe`
  - 11 test cases / 7080 assertions
  - selected companionのatomic groupingと、非選択texture requestの独立適用
- `pelican_test_shader_library_test.exe`
  - 9 test cases / 104 assertions
  - source/include/surface reverse dependencyとcandidate rollback
- `pelican_test_materialvaluesreload_test.exe`
  - 67 assertions、1 pass / 3既存skip
  - value commitとstructural rejection
- `pelican_test_renderpipelinetransaction_test.exe`
  - 11 test cases / 78 assertions
  - stale candidateがpre-publication callbackを呼ばないこと
- `pelican_test_rendercompilerprogram_test.exe`
  - 2 test cases / 18 assertions
  - top-level compiler program transactionの回帰
- `pelican_test_headless_render_test.exe "[wp218]"`
  - 1 test case / 53 assertions
  - graph-only output state変更の実描画
  - graph schema + valid surface body同時変更の同世代公開
  - graph schema + invalid surface body同時変更のgraph/shader/material/pixel完全rollback

OpenXR OFFのDebug buildで上記を通過した。コンパイル時の既知
`VulkanUtils::executeOneTimeCmd` nodiscard warning以外に、この変更による新規build errorはない。
