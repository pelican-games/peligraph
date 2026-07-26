# WP204 physical image alias runtime 実装レポート

日付: 2026-07-26

対象: RPE12b / WP204 alias-runtime vertical slice

判定: **compiled lifetime groupのruntime選別、VMA allocation共有、Vulkan alias同期、
hot reload generation分離、rollback/recreate、実Vulkan描画まで完了**

## 1. 実装した範囲

logical graphや通常のrendering configへVulkan memory情報を露出せず、physical plannerが
生成・検証したalias groupのうち、次をすべて満たすimageだけをruntime allocationへ接続する。

- non-history
- single-sample
- `materialized_image`
- `COLOR_ATTACHMENT | SAMPLED`の完全一致usage
- format、extent、array layer、view layout、storage modeが完全一致
- compiled lifetimeが非重複
- display targetではない

MSAA、history、depth、storage/transfer image、transient/tile-local image、bufferとの混在は
このsliceでは受理しない。対応範囲を狭く保ち、未知の組み合わせを通常allocationから
暗黙にaliasしない。

## 2. compilerからruntime targetへのbridge

各`VulkanTargetPlan`のalias groupをそのまま一つの共有target集合へ適用すると、
flat/preview/XRやhot-reload variant間でlifetime前提が異なる可能性がある。そのためmergeは
次の保守的規則を使う。

1. あるmemberへ触れる全planを収集する。
2. その全planが同じmember集合を持つ完全に同一のalias groupを含む場合だけ採用する。
3. group不在、部分一致、別groupとの交差が一つでもあればruntime assignmentを落とす。

不一致はcompile errorではない。allocation共有だけを無効化し、各imageを独立allocationへ戻す。
採用groupはsorted member名から安定したruntime idを生成し、
`RenderTargetDefinition` / metadataへ型付きassignmentとして運ぶ。runtime適用時にも
group id、member重複、format/usage/extent/sample/layer/storage契約を再検証する。

## 3. Vulkan/VMA所有権

groupの最初のmemberは次でallocationを作る。

- VMA allocation: `VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT`
- Vulkan image: `VK_IMAGE_CREATE_ALIAS_BIT`

後続memberは`vmaCreateAliasingImage`相当で、同じallocationへ別のVkImageをbindする。
logical resourceごとにimage/view/layout stateは分離し、memoryだけを共有する。

`ImageWrapper`はallocationをshared ownerとして保持し、C++の逆順破棄でVkImageを先、
allocationを最後に破棄する。これによりalias memberのretire順と無関係に、最後のimageが
破棄されるまでallocationが生存する。通常imageについても従来のallocation先行破棄を避ける
所有順へ統一した。

## 4. 同期とlayout

同じallocationのmember AからBへ切り替えるとき、layout transitionだけではmemory hazardを
表せない。layout trackerはactive alias memberをgroup tokenごとに追跡し、切替時に
保守的な`ALL_COMMANDS` memory read/write dependencyを発行する。

切替前後のlogical image layoutは`Undefined`として扱い、新しいmemberを必要layoutへ遷移する。
graph variant切替でlayout trackerをresetしても、直前にactiveだったgroupは次回利用時の
dependency要求を保持する。これは最適なstage/access maskへ絞る前のcorrectness-first境界である。

## 5. hot reloadとgeneration

同じ文字列group idを複数runtime generationが持っても、allocationは共有しない。
render-target registrationごとに単調増加する固有tokenを発行し、次を保証する。

- fully existingな同一generationの再登録は同じtokenを再利用する
- live memberと新memberを混ぜたgroup登録は拒否する
- hidden-name candidate generationは新tokenと新allocationを使う
- candidate rollback後は旧name bindingと旧group ownershipを復元する
- deferred retire中の旧imageはshared allocation ownerを保持する
- 同extent再生成では新しいowner allocationにgroup memberを再bindする

この境界により、publish前candidateや旧in-flight frameが新generationと同じmemoryを
同時使用することはない。

## 6. 検証

pure planning fixtureはlifetime非重複groupの選択、runtime assignment、target definitionへの
適用、複数graph variant不一致時の共有無効化、重複group idのrejectを確認する。

headless Vulkan fixtureは次の順序で実描画する。

1. alias Aへ赤を書き、別targetへsampleする
2. Aのlifetime終了後、同じallocationのalias Bへ緑を書く
3. Bをswapchainへsampleし、最終画素が緑であることを確認する

同じfixtureで別VkImage、同一VMA allocation、alias memory dependency、candidate generationの
allocation分離、rollback、同extent再生成後の共有維持、再描画を検証した。

回帰対象:

| 対象 | assertions | cases |
|---|---:|---:|
| target planning | 153 | 8 |
| runtime sample/target adapter | 179 | 18 |
| render-pipeline transaction | 63 | 10 |
| reload transaction | 7074 | 10 |
| headless Vulkan / hot reload / alias | 255 | 7 |
| synthetic Vulkan multiview | 59 | 1 |
| XR composition protocol | 1041 | 13 |
| render-pipeline resolve | 124 | 13 |
| graph variant policy | 80 | 8 |
| **合計** | **9028** | **88** |

## 7. コミット境界

compiler assignment、runtime allocation/synchronization、generation safety、E2E、設計文書を
`feat(render): execute physical image alias groups`の一つへまとめる。以後も、単独では
利用できない作業段階ごとのmicro commitではなく、revert可能な縦の機能を基本単位にする。
独立して戻す必要がある修正や移行だけを別commitにする。

## 8. 残る境界

- MSAA resolved imageとmultisample attachmentを同時に扱うalias
- history/temporal、depth、storage/transfer image、buffer/image間のsuballocation
- graphを同時実行できる将来execution domainでの相互排他証明
- alias切替barrierのstage/access mask最適化
- physical fragmentによるaggressive group追加、scope fusion/reorder、raw queue/barrier
- Quest standalone等のtile GPUにおける実memory budget、帯域、GPU時間の計測

