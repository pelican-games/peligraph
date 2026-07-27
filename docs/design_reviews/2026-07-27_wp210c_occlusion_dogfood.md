# WP210c depth-pyramid occlusion dogfood 実装レビュー

日付: 2026-07-27
状態: fixed-state project-owned vertical slice 実装済み

## 結論

WP210bのGPU-written draw transportをidentity copyだけで終わらせず、実geometry depthから
depth pyramidを作り、world AABBをproject compute shaderで判定し、visible command列へ
compactする経路を実Vulkanで閉じた。material/pipeline状態は引き続き1つのCPU DrawQueue
rangeへ固定する。engineは候補dataの安定したABIと同期を提供し、culling algorithm自体は
projectが交換できる。

## 追加した契約

- `host_source: "scene_draw_bounds_v1"`
  - 1 commandにつき32 byte
  - `vec4 minimum` + `vec4 maximum`
  - `minimum.w == 1`: xyzのworld AABBが有効
  - `minimum.w == 0`: boundsなし。consumerはconservative keepする
- sampled image resource port accessor
  - `pelican_sample_lod_<port>(...)`
  - `pelican_size_lod_<port>(int lod)`
  - `pelican_mip_count_<port>()`
- render-target format string `R32_SFLOAT`

commandsとboundsは、view-major、各view内opaque/transparentの同じflattened DrawQueue順で
公開する。両host bufferのsource record数は一致する。allocation容量とwritten record数は
bufferごとに独立なので、shaderはruntime array lengthの小さい方だけを処理する。

## dogfood構成

```text
fixed-state material depth prepass
  → D32 scene depth
  → R32 mip 0 seed
  → max reduction mip 1..4
  → AABB projection + hierarchy sampling
  → visible VkDrawIndexedIndirectCommand compact + count
  → fixed-state material pass
```

fixtureのproject shaderは8つのAABB cornerをcamera clip spaceへ投影し、screen rectangleに
応じたLODを選ぶ。standard non-reversed depthに対して各mipは2x2の最大値を保持し、
rectangle cornerのfarthest depthよりAABBのnearest depthが後ろにある場合だけrejectする。
eye plane、frustum外、boundsなしは保守的に残す。このアルゴリズムは既定engine policyでは
なく、交換可能なdogfood実装である。

prepassは最終G-bufferとは別のthrowaway attachmentとdepthを使う。同一attachmentへの
prepass/final WAWを暗黙順序に依存させず、
`prepass → pyramid → cull → final geometry`を一方向のresource graphにするためである。

## 検証

CPU contract:

- bounds host sourceのparse/name
- non-zeroかつ32 byte単位のsize
- command layoutを誤って付けた構成の拒否
- R32 format往復
- generated sampled LOD accessorを実shader compile/reflectionで検証

実Vulkan:

- fully hidden candidate: GPU count 1
- sideへ移動したvisible candidate: GPU count 2
- `execution: "cpu"`: GPU culling countは1のまま、最終画像はGPU hidden pathと一致
- command/bounds host populationはいずれもsource/written 2
- prepass、seed、最終reduce、cull、geometryのplan順序

## 意図的に残した境界

1. 複数material/pipeline/skinned-state segmentをまたぐcompactは未対応。
2. XR per-view pyramid/bounds判定は未検証。
3. culling shader hot reload/rollbackの受け入れは未検証。
4. large workloadでのGPU timingとbreak-even pointは未計測。
5. built-in occlusion feature/presetは追加していない。まずproject-owned実装で契約を固めた。

次はsegment tableを追加し、各segmentがcommand offset/countと固定state rangeを結ぶ。
その前に同じfixtureをhot reloadとXR view familyへ広げ、bounds ABIを変えずに済むことを
確認する。
