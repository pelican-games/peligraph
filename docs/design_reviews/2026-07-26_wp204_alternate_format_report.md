# WP204 verified alternate physical format 実装レポート

日付: 2026-07-26

対象: RPE12b / WP204 Phase B follow-up

判定: **pure verifier、Vulkan runtime適用、hot reload実描画、OpenXR OFF/ON回帰まで完了**

## 1. 実装した境界

### 1.1 authoring contract

render targetへ任意の`format_candidates`を追加した。

- `format`はautomatic/defaultのままで、候補宣言だけでは選択を変えない
- primary formatは候補列の先頭へ必ず補い、重複を除去する
- render featureの`render_target_overrides`から候補を重複なしで追加できる
- logical target compilerは候補列を`ResourcePattern::format_candidates`へ運ぶ

`pelican.vulkan_physical_fragment`のschema versionは1のままである。既存の任意
`resource.format` fieldへ、eject結果と同じcanonical Vulkan format名を指定する。
新しいunchecked Vulkan fieldは追加していない。

### 1.2 target-specific capability evidence

pure project層へ`VulkanPhysicalResourceFormatCapability`を追加した。証拠は
logical resourceとformatの組に束縛し、次だけをdataとして渡す。

- required image usageを満たすか
- 対応sample count集合
- 最大array layer数
- external depth export用transfer-source契約を満たすか

alternate formatは次をすべて満たす場合だけlinkする。

1. logical `ResourcePattern`で候補として宣言されている
2. 最終representationが`materialized_image`
3. 対象resource/formatのcapability evidenceが存在する
4. 選択済みsample countとarray layerが証拠の範囲内
5. external depth sourceならtransfer-source契約を満たす

証拠なし、未宣言、非対応の場合は同じbackend candidateのまま失敗し、別candidateや
automatic formatへfallbackしない。

### 1.3 linkerの再閉包

linkerは選択formatを次へ同時反映する。

- `VulkanPhysicalResourcePlan`
- `ResolvedSampleCountPlan`のresource format
- lowering graphのrequired physical feature
- external depth export plan

その後、selected endpoint capabilityと同じbackend candidateのfeature closureを再検証する。
logical層へ逆変換して再compileする経路は追加していない。

## 2. Vulkan runtime vertical slice

runtime bridgeは実`vk::PhysicalDevice::getImageFormatProperties`から各候補を照会する。
照会usageはtargetの全usageを使い、history targetでは実allocationと同じ
`TRANSFER_DST`も加える。external depth用にはさらに`TRANSFER_SRC`を加えた照会を分ける。

同じcapability snapshotをsample-count planningとphysical fragment linkerへ渡し、
compiler結果から`RenderingTargetFormatAssignment`を作る。選択formatはGPU登録前に
`RenderTargetDefinition`へ適用されるため、image/viewとdynamic-rendering pipelineが
同じformatで生成される。

同名targetを複数graphが共有する場合、またはflat/XR variantが同じtargetを共有する場合は、
全planのphysical format一致を必須にした。競合はmutable GPU registrationを始める前に
rejectする。異なるformatを同時に使う場合は別target名を使う。

hot reloadは既存のgeneration-owned GPU arenaをそのまま利用する。候補formatを選んだ
新image/view/pipelineをprepareし、runtime generationを一括publishした後、旧resourceを
既存lease/fence規則でretireする。

## 3. 実描画fixture

headless upscale fixtureへ次の縦切りを追加した。

1. `low_color`を`R8G8B8A8_UNORM`で生成して描画・readbackする
2. automatic physical fragmentをejectする
3. 宣言済み候補`R16G16B16A16_SFLOAT`へresource formatを変更する
4. `vulkan_physical_fragments.flat`へ貼り、実watcher経路でhot reloadする
5. runtime generation更新、target metadata、physical planのR16F適用を確認する
6. 再描画し、reload前と同じ赤/青画素パターンをreadbackで確認する

候補formatのusageをdevice factsが否定するpure runtime-bridge fixtureも追加し、
GPU登録前に名指しrejectされることを固定した。

## 4. コミット

| commit | 内容 |
|---|---|
| `22b38b3` | target固有format capability evidenceとpure fragment verifier |
| `e23f7e6` | authoring候補、実device query、runtime assignment、hot reload実描画 |

## 5. ローカル検証

既存の`build-off-openxr`だけをOFF→ONへ再構成した。新しいbuild treeは作成していない。
最終cacheは`PELICAN_WITH_OPENXR=ON`である。

### OpenXR OFF

| 対象 | assertions | cases |
|---|---:|---:|
| target planning / parser / feature / runtime bridge / pipeline / strategy / transaction / headless | 1148 | 126 |

### OpenXR ON

| 対象 | assertions | cases |
|---|---:|---:|
| 共通target/pipeline/runtime/headless fixture | 1191 | 128 |
| XR action / activation / composition target / discovery / feature policy / session / view space | 2470 | 44 |
| **合計** | **3661** | **172** |

全対象が成功した。追加したheadless alternate-format test単独では23 assertions / 1 caseが
成功している。

## 6. 残る境界

- explicit load/store
- automatic scopeをまたぐfusion/reorder
- queue family / barrierの手動記述
- tile-local / alias planのruntime実行と対象GPU gate
- open external boundary、complete raw physical plan、`NativeScope`

alternate formatは`materialized_image`の宣言済み候補に限定したままにする。上記の物理制御は、
実行機構とverifierを同時に追加できる独立sliceとして進める。
