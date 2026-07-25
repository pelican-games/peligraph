# WP204 Phase A physical plan pin / eject 実装レポート

日付: 2026-07-26

対象: RPE12a — target policy authoring / Vulkan decision pin

判定: **Phase A 実装・ローカル自動テスト完了、Phase B physical fragment verifier待ち**

## 1. 実装結果

### 1.1 portable target planning control

- rendering config の `target_planning` を typed `TargetPlanningPolicy` へcompileする
- `optimized`、`conservative_debug`、`hazard_stress` profileとdeterministic seedを持つ
- graph単位でnodeの`serial` / `isolate`、resourceの`no_alias`を指定できる
- diagnostic warningのstrict化はopt-inとし、通常経路はoptimize-by-defaultを維持する
- XR variantではgraph selectorへcompiled suffixを付け、variantの実graph名へ解決する

### 1.2 versioned Vulkan decision pin

- 自動`VulkanTargetPlan`から、選択済みbackend candidateを
  `ejectable_pin_package`として常にdumpする
- package schemaは`pelican.vulkan_target_plan_pins` version 1とし、
  graph、compiled logical graph fingerprint、backend candidateを持つ
- configは`vulkan_plan_pins.flat|preview|xr`のvariant別配列としてpackageを受け取る
- 適用したpackageは`applied_pin_package`で再観測できる
- packageのparse / dump / parseを同じphysical layerでround-tripできる

### 1.3 fail-closed validation

- logical graphが変わったpackageはstaleとしてrejectする
- packageのgraphが違う場合、未知candidate、現在のdevice/provider上で
  infeasibleなcandidateは理由付きでrejectする
- pinned candidateが失敗しても別candidateへ黙ってfallbackしない
- graphごとの重複package、未知variant key、unknown field、schema/version違反をrejectする

### 1.4 compiler layer separation

`target_planning`と`vulkan_plan_pins`はrenderer strategyより下流のcontrolである。
strategyのABI seedとinput/output config fingerprintへ含めると、ejectしたpackageを
同じconfigへ貼ったこと自体でlogical graph fingerprintが変わる自己参照になる。

そのため両fieldはstrategy callbackの前に退避し、strategyが同名fieldを生成した場合は
責務衝突としてrejectし、正常終了後に元の値を復元する。これによりpinの有無で
strategy provenanceとcompiled logical graph fingerprintは変化しない。

## 2. 公開境界

Phase Aで凍結するのは、有限なbackend candidate選択を固定する狭いdecision pinだけである。
format、resource lifetime、alias、sample/view/extent、scope、load/store、queue、
barrierを任意のVulkan値で上書きするAPIは公開していない。

次のPhase Bでは次を実装する。

1. resource/valueとscope nodeのversioned physical fragment schema
2. logical boundary / capability / lifetime / synchronization verifier
3. automatic plan、sparse fragment、complete planが共有するphysical linker
4. accepted fragmentを現在のmaterialized runtimeへ渡す明示的なcapability gate
5. same-layer dump/import、stale provenance、hot reload failure-atomic fixture

## 3. コミット

| commit | 内容 |
|---|---|
| `ac13002` | typed target planning controls |
| `d616d33` | versioned Vulkan plan pins and runtime bridge |

## 4. ローカル検証

既存の`build-off-openxr` build treeを再利用した。ディレクトリ名と異なり、現在の
cacheは`PELICAN_WITH_OPENXR=ON`である。新しいbuild treeは作成していない。

| test | assertions | cases |
|---|---:|---:|
| target planning | 153 | 8 |
| target render planning | 117 | 15 |
| render pipeline resolve | 120 | 12 |
| rendering sample count / target bridge | 69 | 9 |
| render strategy registry | 74 | 5 |
| Vulkan headless render | 187 | 5 |
| **合計** | **720** | **54** |

全対象が成功し、headless Vulkan runtimeのplan dumpでも
`ejectable_pin_package`とtyped planの一致を確認した。

OpenXR OFFの再構成は、このPhase A検証では追加の大容量build treeを作らない方針から
未実施である。次のcompiler回帰時に同じbuild treeを一時的にOFFへ再構成し、
ONへ戻す一往復で確認する。
