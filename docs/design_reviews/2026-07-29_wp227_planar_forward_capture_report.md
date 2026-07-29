# WP227 planar reflection Forward opaque capture 実装レポート

日付: 2026-07-29

## 1. 結果

標準planar reflectionをDeferred-only captureからDeferred + Forward opaque captureへ拡張した。
`forward_opaque_v1`へ自動routeされたmaterialをreflection familyで再描画し、Deferred
lighting後のreflection color/depthへloadして合成する。projectはmaterialごとに追加の
reflection routeを指定しなくてよい。

同時に、canonical passへfeatureが後付けしたshadow、clustered lighting、その他の
surface/material resource bindingを再描画passへ安全に再利用するlate composition helperを
追加した。標準featureを参照しなければ新しいpass、target、bindingは生成されず、同梱JSONを
projectへコピーして改造できる既存のpurge/override境界も変わらない。

## 2. Forward opaque capture

標準`planar_reflection` featureはDeferred lightingの後に
`planar_reflection_forward_opaque` material passを置く。契約は
`forward_opaque_v1`、view familyは`$reflection/planar`、resolution domainは独立である。
color/depthはloadし、reflection colorとdepthへ同じopaque scene queueを再描画する。

material routingはcanonical `forward_opaque` passと同じ契約を使うため、OpenPBR surfaceの
自動routing結果をそのまま利用する。materialやinstanceへplanar-reflection専用tagを要求せず、
Deferred objectを重複描画しない。

## 3. late pass binding inheritance

feature passへcomposer-only fieldとして次を指定できる。

```json
{
  "name": "planar_reflection_forward_opaque",
  "type": "material",
  "material_contract": "forward_opaque_v1",
  "inherit_bindings_from": "forward_opaque"
}
```

解決は全featureのmerge/overrideとsurface resource consumer解決後に行う。material passは
`surface_resources`、`material_resources`、`screen_inputs`を、fullscreen passは
`resource_ports`と対応する`input`を継承する。destination側の明示値を優先し、継承chainも
許可する。unknown source、self/cycle、pass kind不一致、material contract不一致は
compile errorにする。

展開後に`inherit_bindings_from`を削除するため、logical pass parser、scheduler、backend、
plan dumpへ継承という新しい状態を持ち込まない。これは標準featureの追加順を自由に保ちつつ、
最終的なcanonical ABIだけを再利用するためのauthoring sugarである。

## 4. clustered lightingとの整合

clustered lightingとの実GPU dogfoodで二つの既存の境界不整合が見つかった。

1. `PELICAN_FEATURE_CLUSTERED_LIGHTING`がDeferred surface variantにも入り、Forward専用の
   implicit buffer interfaceが無いshaderを生成していた。surface compilerはこのdefineを
   non-Forward variantから除去する。
2. Forward surface compilerが合成した`light_inventory` / `light_selection` portはSPIR-Vに
   存在する一方、authored surface metadataに戻らずmaterial登録時に失われていた。
   ShaderBundleへcompiler-owned resource interfaceを保持し、authored material portと
   collision検査付きで合成する。

これによりcanonical passから継承したclustered descriptor ABIと実shader reflectionが一致する。
ただし現在のselection bufferはmain-view cluster空間である。world clip planeを持つreflection
viewではcluster selectionを無効化し、LightUBOの固定light selectionへfallbackする。
family-local cluster生成は別の性能・品質sliceとする。

## 5. 検証

Debug構成で以下を通した。

- feature composition: 418 assertions / 30 cases
- surface compiler: 308 / 19
- clustered surface compiler slice: 32 / 2
- shader library/compiler-owned ABI: 104 / 9
- material binding: 16 / 3
- planar reflection Vulkan golden: 35 / 1
- cascaded directional shadow Vulkan regression: 40 / 1

Vulkan goldenはDeferred objectと赤いemissive Forward objectを空間的に分離する。
Forward capture passを除いた同一featureとの比較で、G-buffer albedoは完全一致し、
reflection color/depthだけが変化することを確認する。さらにruntimeのopaque draw rangeが
reflection Forward passの契約へeligibleであること、reflection五passが実行されたことを
検証する。clustered lightingも同時に有効化し、feature記述順とimplicit descriptor ABIを
実際のpipeline作成・描画まで通した。

## 6. 残る境界

- transparent geometryのreflection内描画とfamily別depth sort
- family-local clustered light selection
- secondary familyのmultiview lowering
- oblique near-plane projection
- roughness mip/prefilterと標準sampling policy
- point/spot shadowやreflection probe向けcube provider/attachment

これらもplanar reflection専用分岐ではなく、stable ViewFamily relation、継承済みpass ABI、
generic secondary draw preparationを再利用して拡張する。
