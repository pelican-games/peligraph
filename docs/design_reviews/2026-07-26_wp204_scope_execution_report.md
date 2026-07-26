# WP204 dependency-safe physical scope execution report

日付: 2026-07-26
対象: `pelican.vulkan_physical_fragment` version 3

## 結論

automatic Vulkan target planを基準にしたsame-layer編集として、依存関係を保つscope/node
reorderと、互換なmaterialized rendering scopeの単一dynamic-rendering instance化をruntimeまで
接続した。旧version 1/2のsplit-only契約は変更せず、version 3の明示
`scope_edit_mode: "dependency_safe"`だけが広い編集を受理する。

このsliceで実行可能になったのは次の範囲である。

- data edgeと明示`after` / `before`を保つ全nodeのexact partition
- 編集後のphysical orderに基づくresource lifetime再計算
- 再計算後lifetimeによるalias group再検証
- 元のframe-graph node indexと異なるscope orderのscheduler/runtime実行
- internal、single-sample、materialized、同一ordered attachment/view contractを持つ
  rendering scopeの融合
- 2 pass目以降のLoad、非終端passのStoreを守る一つのVulkan dynamic rendering instance
- flat、sequential、multiviewの既存view-execution contract維持
- 既存Vulkan compute scopeとrender scopeの同一physical schedule上での実行

## schemaと互換性

version 3は`scope_edit_mode`を必須にする。

- `split_only`: version 1/2と同じexact ordered partition。automatic scopeを分割できるが、
  異なるautomatic scopeの融合やreorderはできない。
- `dependency_safe`: data/after/beforeを証明したreorderと、後述の限定fusionを許可する。

現在のejectはversion 3 / `dependency_safe`を生成する。version 1/2 parserとround-tripは残し、
古いpackageへ新しい意味を暗黙付与しない。logical graph、automatic plan、device facts、
provider generationのfingerprint gateも従来どおりである。

## linkerの証明

linkerはauthoring順ではなく次をcorrectness edgeとして扱う。

1. versioned logical valueから導出したproducer→consumer data edge
2. nodeの明示`after`
3. nodeの明示`before`

全endpointの存在、node重複/欠落、edge方向を検証したあとだけlowering graphを物理順へ並べ替える。
各resourceのinput/output useを新しいpositionで走査し、required/external/require-store resourceは
terminalまで生存させる。その後にscope boundary、tile/transient越境、attachment operation、
alias lifetime、feature closureを再検証する。

通常のrendering pass列はfeature compositionが安定順の明示`after`へ正規化するため、
physical fragmentはその関係を消せない。実際に独立なcompute taskなど、logical graph上で
dependencyを持たないnodeだけが追い越せる。

## rendering scope fusion

異なるautomatic scopeを一つのrendering instanceへ融合する条件は次である。

- 全nodeがrendering kind
- rasterization sample countが1
- view execution、view count、execution count、view maskが一致
- ordered color/depth attachment集合が完全一致
- attachmentがinternal `materialized_image`
- 2 pass目以降の全attachmentがLoad
- 非終端passの全attachmentがStore
- UI/ImGui、swapchain attachment、local-read以外のsame-frame sampled dependencyを含まない

runtime contractでは`single_rendering_instance`と`local_read_scope`を分離した。前者が
dynamic renderingをscope全体で一度だけ開始する広い契約、後者がinput-attachment mappingと
`VK_KHR_dynamic_rendering_local_read`も必要とするsubsetである。scope内pass間にはBY_REGIONの
attachment memory dependencyを発行する。

## 実config経路で見つかった差

CPU fixtureはattachment Loadを一つのlogical `read_write` useとして作っていたが、
実際の`FrameGraphDefinition`→logical shadow adapterは同じresourceを独立したreadとwriteへ
分解していた。このためunit verifierは通っても、hot reloadした実configのmaterialized fusionは
scope-boundary verifierに拒否された。

adapterを修正し、Load attachmentかつ同一nodeがwriteするresourceを
`input_output` portとversioned `LogicalAccessMode::read_write` /
`LogicalAccessIntent::attachment`へcompileするよう統一した。明示sampled readやLoadでない
同名read/writeは従来どおり分離される。

## 実行時の変更

- view schedulerはscopeを元node indexの連続区間と仮定せず、名前からindexへ解決してexact
  membershipを検証する。
- rendererのbarrier source確認は`from_index < to_index`ではなく、現在のcommand-recording
  callで実際にsource nodeが完了したかを見る。
- fused scope内部のbarrierは元index範囲でなくscope membershipで分類する。
- sprite anchorが参照する直前color/depthは元配列の逆走査でなく実行済みpassから更新する。
- sequential XRをviewごとにrecordする場合も、completionはそのrecording call内で更新する。

## 検証

純CPU:

- version 1/2 backward compatibilityとversion 3 strict schema
- independent reorder、data dependency反転reject
- 明示after/before反転reject
- reorder後lifetime
- attachment/view/sample mismatch fusion reject
- materialized fused runtime contract
- 元index非連続のphysical schedule
- sequential/multiview fused scope schedule
- attachment Loadのlogical read-write version edge

Vulkan:

- headless hot reloadでautomatic planからversion 3 fragmentをeject/import
- dependencyのないcompute scopeをphysical order先頭へ移動
- base/overlay materialized scopeを一つのdynamic rendering instanceへ融合
- overlayがdiscardした画面左半分でbase内容が保持され、右半分だけoverlay出力になることをreadback
- synthetic multiview GPU fixtureとtile-local local-readを回帰

## 未対応

- MSAA attachmentを含むscope fusion
- swapchain/external attachmentを含むscope fusion
- attachment集合やextentが異なるscopeのunionを手動authoringする形式
- arbitrary sampled、storage、transfer dependencyをscope内同期へloweringする一般verifier
- queue family/queue assignment、手動barrier、event/semaphore authoring
- CPU physical scheduler、CPU↔GPU bridge、video/external domain
- Meta XR Simulator、Quest実機、対象tile GPUでの性能・表示受け入れ
