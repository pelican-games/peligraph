# WP204 automatic transient attachment runtime 実装レポート

日付: 2026-07-26

対象: RPE12b / WP204 transient-runtime follow-up

判定: **独立backend候補、automatic Store elision、device/format gate、Vulkan image allocation、
hot reload実描画、OpenXR OFF/ON回帰まで完了**

## 1. 実装した範囲

通常のrendering configに低層storage modeを追加せず、次をすべて満たすrender targetだけを
自動transient候補にした。

- usageがcolor/depth attachmentだけ
- non-history
- physical sample countが1
- logical graphでwriteされるが後段からreadされないvirtual resource
- persistent Store、resolve、external depth exportを要求しない
- `optimized` profile
- 対象device/formatがattachment usageと`VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT`を受理する

materialized、transient、tile-localは別々の有限backend候補である。transient選択は
tile-local/local-read capabilityに依存しない。`conservative_debug`、非対応format、後段read、
MSAAではmaterializedへ戻る。

## 2. automatic physical lowering

選択候補は`pelican.vulkan.transient_plan@1`で、対象resourceのrepresentationを
`transient_attachment`にする。同じresourceをwriteするphysical attachmentのStoreは
automatic plan内でDiscardへloweringし、
`pelican.plan.transient_attachment_store_elided@1` decisionを残す。

この省略はtransientに選ばれたresourceだけに適用する。一般のmaterialized single-sample
surfaceは、後段のsample/transfer/host liveness証明なしにStoreを落とさない。

ejectしたphysical fragmentでは、transient resourceを`materialized_image`へ変更し、
attachmentをStoreへ戻せる。これは保存を増やす保守的escape hatchである。

## 3. device capabilityとruntime boundary

runtime target adapterはgenericなVulkan transient capabilityだけで決めず、宣言済みformatごとに
`vkGetPhysicalDeviceImageFormatProperties`相当のqueryを行う。automatic候補はdefault formatの
証拠がある場合だけtransientにする。physical fragmentによるalternate format選択は従来どおり
`materialized_image`を要求し、transientのままformatだけを変える編集はfail-closedで拒否する。

複数graphが同名targetへ異なるrepresentationを要求した場合はGPU登録前に拒否する。
physical planからは`RenderingTargetRepresentationAssignment`を作り、
`RenderTargetStorageMode`へ変換する。allocatorは次を行う。

1. image usageへ`VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT`を追加
2. attachment-only、non-history、single-sample契約を再検証
3. VMAへ`VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT`をpreferred flagとして渡す
4. lazy memory typeがないdesktop deviceでは通常のdevice-local memoryへfallback

VkImage自体はgeneration resourceとして保持する。transientは毎frameのobject再生成ではなく、
attachment内容をrender scope後まで永続化しない契約である。

## 4. 実Vulkanとhot reload

既存headless upscale fixtureへ、半解像度のwrite-only `transient_scratch` passを追加した。
実device上で次を確認した。

- selected backendが`pelican.vulkan.transient_plan@1`
- resource representationが`transient_attachment`
- metadataにtransient storage modeとimage usageが反映される
- automatic Storeが`vk::AttachmentStoreOp::eDontCare`になる
- 本来のupscale出力画素が変わらない
- alternate-format physical fragmentを含むpipeline hot reload後も同じtransient契約を維持する
- reload後の実描画画素が一致する

## 5. コミット

| commit | 内容 |
|---|---|
| `ae0ae67` | materialized/tile-localから独立したtransient候補とautomatic Store lowering |
| `9d43393` | device/format gate、representation assignment、runtime image allocation、実Vulkan test |

## 6. ローカル検証

既存の`build-off-openxr`をOpenXR ON→OFF→ONへ再構成した。最終cacheは
`PELICAN_WITH_OPENXR=ON`である。

| 構成 | 対象 | assertions | cases |
|---|---|---:|---:|
| OpenXR ON | target/frame/sample planning、render helpers、pipeline resolve、multiview、transient headless | 874 | 109 |
| OpenXR OFF | target/frame/sample planning、render helpers、pipeline resolve、transient headless | 812 | 107 |
| ON復元後 | sample planning、実Vulkan multiview、transient headless | 200 | 14 |

すべて成功した。pure runtime adapter testは未対応format、alternate format、profile fallback、
physical representation適用を含む。headless testは実allocation、描画、physical fragment、
hot reloadを同じ1 caseで検証する。

## 7. 残る境界

- same-pixel readを持つtile-local attachmentとscope fusion
- 一般のmaterialized single-sample Store elisionと完全なliveness証明
- alias groupの実memory binding
- scope fusion/reorder、queue family、barrierのaggressive physical fragment
- open external boundary、complete raw physical plan、`NativeScope`

次はtile-local readのshader ABIとnative render scopeを一緒に実装する。transient runtimeを
tile-local実装済みの代用にはせず、対象タイルGPUでscope fusionと帯域効果を測れるgateまでを
一つのsliceにする。
