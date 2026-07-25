# WP204 Phase B v1 physical fragment 実装レポート

日付: 2026-07-26

対象: RPE12b — verified Vulkan physical fragment

判定: **Phase B v1 実装・OpenXR OFF/ON ローカル自動テスト完了**

> 後続でalternate formatのpure verifierとVulkan runtime vertical sliceを実装した。
> 現在の境界は
> [`2026-07-26_wp204_alternate_format_report.md`](2026-07-26_wp204_alternate_format_report.md)
> を参照。本書の「alternate format未対応」はPhase B v1完了時点の履歴である。

## 1. 実装結果

### 1.1 versioned same-layer package

- `pelican.vulkan_physical_fragment` version 1を追加した
- graph、compiled logical graph fingerprint、automatic plan fingerprint、
  backend candidateを必須にした
- resource overrideはsparse、scope partitionとalias groupは省略可能にした
- 自動planから全resource/scope/aliasを含むpackageをejectできる
- parse → canonical dump → parseを同じphysical層でround-tripできる

`VulkanTargetPlan`のdumpには`automatic_plan_fingerprint`と
`ejectable_physical_fragment`を常に含める。fragmentを適用した場合は
`applied_physical_fragment`も残す。

### 1.2 environment-bound stale detection

logical fingerprintだけでは、同じgraphを別device/providerへ持ち込んだ場合を検出できない。
そこでautomatic plan fingerprintへ次を含めた。

- canonical target topologyとdevice/endpoint facts
- backend candidate、probe結果、provider identity/generation
- disposable lowering graphとrequired physical feature
- resource、scope、alias、sample-count、view execution
- external depth export contract

hot reloadやdevice/provider変更後に前のpackageを貼った場合は、別案へfallbackせず
stale artifactとしてrejectする。

### 1.3 conservative resource edit

v1のrepresentation変更は次だけを許可する。

- automatic representationを維持する
- automatic transient/tile-local imageを`materialized_image`へ安全側に倒す

image/buffer/external materialization contractを破る変更はrejectする。format fieldは
自動planのformatをcanonical round-tripするために存在する。別formatの選択は
format usage / sample-count capabilityを再解決できるまでfail-closedでrejectする。

resource編集後はstore/aliasable/required featureを再導出し、lowering graph側の
feature metadataも同期する。selected endpoint capabilityとbackend feature closureを
再検証する。

### 1.4 scope / alias verifier

scope指定は全nodeを一度ずつ含むexact ordered partitionでなければならない。
単一automatic scopeのsplitは許可するが、異なるautomatic scopeのfusionやreorderは
許可しない。link後にresource境界を再検証し、tile/transient attachmentのscope越境、
不正なsampled dependency、nodeの欠落・重複をrejectする。

alias groupは次をすべて照合する。

- resourceが存在しaliasableである
- 一つのresourceが複数groupへ所属しない
- representation、format、sample-count、view layout、extentが一致する
- lifetimeが重ならない

### 1.5 pipeline / runtime route

rendering configへ`vulkan_physical_fragments.flat|preview|xr[]`を追加し、現在のimmutable
graph variantに対応するpackageだけをruntime target compilerへ渡す。graphごとの
重複package、未知variant、malformed schemaはprepare前にrejectする。

fragmentはrenderer strategyより下流のcontrolであるため、
`target_planning` / `vulkan_plan_pins`と同様にstrategy ABI seedとconfig fingerprintから
除外し、callback後にfailure-atomicに復元する。

現在のruntime adapterはmaterialized imageだけを実行可能としてadvertiseする。
verifierがtile-local/alias planを受理できても、executor未実装の結果はruntime capability
gateで名指しrejectし、parse成功を実行可能性として扱わない。

## 2. v1で意図的に残した制約

- alternate formatの選択
- explicit load/store
- automatic scopeをまたぐfusion、任意reorder
- queue family / barrierの手動記述
- open external boundaryとcomplete raw physical plan
- `NativeScope`
- tile-local / aliasのruntime実行

これらは既存schemaへunchecked fieldを足さず、device capabilityと同期境界を検証できる
次のversioned sliceで追加する。

## 3. コミット

| commit | 内容 |
|---|---|
| `a0c1139` | physical fragment schema、fingerprint、pure verifier/linker |
| `700589b` | pipeline authoring、strategy分離、runtime target bridge |
| `47fc665` | OpenXR OFF/ONに追従するvariant fixture |

## 4. ローカル検証

既存の`build-off-openxr` build treeだけを再利用し、同じtreeをOFF→ONへ再構成した。
新しい大容量build treeは作成していない。最終cacheは
`PELICAN_WITH_OPENXR=ON`へ戻している。

### OpenXR OFF

| test | assertions | cases |
|---|---:|---:|
| target render planning | 144 | 18 |
| render pipeline resolve | 108 | 12 |
| rendering sample count / target bridge | 74 | 9 |
| render strategy registry | 47 | 4 |
| Vulkan headless render | 189 | 5 |
| **合計** | **562** | **48** |

### OpenXR ON

| test | assertions | cases |
|---|---:|---:|
| target render planning | 144 | 18 |
| render pipeline resolve | 124 | 13 |
| rendering sample count / target bridge | 74 | 9 |
| render strategy registry | 74 | 5 |
| Vulkan headless render | 189 | 5 |
| **合計** | **605** | **50** |

両構成で全対象が成功した。ONではXR固有のpipeline resolutionとstrategy facade fixtureが
追加で実行され、OFFではflat/preview共通機能を維持したままXR専用fixtureだけを除外する。
