# WP223 ViewFamily runtime foundation 実装レポート

日付: 2026-07-29

状態: **flat / OpenXR runtime移行・CPU fixture・既存jitter inventory更新完了**

## 結論

`Renderer::renderLogicalFrame()`へ行列配列だけを渡していた境界を、
`RenderViewFamily`へ置き換えた。flat Cameraは`$main/$mono`、OpenXRは
`$main/$xr/0..N`を供給し、どちらも同じcardinality検証、projection modifier、
temporal snapshot、sequential/multiview実行へ入る。

これはCSM、point shadow、planar reflection、cube captureをCameraやOpenXRの特例として
増やさないためのruntime foundationである。WP223ではsecondary familyのpass接続までは
行わず、既存flat/XRの描画意味を維持した。

## 型と責務

`src/core/renderer/viewfamily.hpp`へ次を追加した。

- `RenderViewParameters`
  - providerが供給するnon-jittered view/projection/camera position
  - frame間で安定した`view_id`
- `RenderViewFamily`
  - `family_id`
  - 現frameの実行順に並ぶview列
- `TemporalViewFamilyHistory`
  - 添字でなく`view_id`をkeyとするsnapshot履歴
  - 現行history imageとの安全な整合用に実行順も記録
- `RenderViewFamilyProjectionModifiers`
  - 現在は既存projection jitterを保持
  - provider行列とmodifier適用を分離

`GraphVariantViewFamily`はmono/stereoというcardinality policyのまま維持した。
runtime family identity、view identity、sequential/multiview physical executionをこのenumへ
詰め込んでいない。

## runtime flow

```text
flat Camera ─────────┐
                    ├─ RenderViewFamily($main)
OpenXR located views ┘       │
                             ├─ graph variant/cardinality validation
                             ├─ stable history synchronization
                             ├─ one family-level jitter sample
                             └─ snapshots in current execution order
                                      │
                         sequential or Vulkan multiview
```

従来はsequential pathとmultiview pathが各viewで同じjitter sample/snapshot構築を
重複していた。現在は両方が`buildRenderViewFamilySnapshots()`を使う。

## temporal identityと並び替え

matrix履歴は`family_id + view_id`で解決するため、providerがview順を変えても別viewの
previous matrixを誤って読むことはない。

ただし現行render-target historyはlayer/実行添字で所有される。このためruntime統合では
view membership変更だけでなく順序変更もresource history resetを要求する。ID-keyed
physical layer割当が入る前に「行列だけ正しくTAA imageは左右逆」という状態を作らないための
意図的な制約である。

## projection jitterとの整合

- provider出力はnon-jitteredのまま
- jitterはscene render extentに対しfamilyごとに一度sample
- 同じfamilyの全viewへ同じsampleを適用
- compiled graph policyが`forbid`ならmodifier適用を拒否
- consumer inventoryは新しいfamily resolverをsource of truthとして追跡
- Cameraのculling/RPC用VP、shadow light-space VP、UI/previewは従来どおりjitter非依存

## 検証

- `pelican_test_viewfamily_test.exe`
  - 4 cases / 22 assertions
  - stable ID、reorder、topology change、family jitter、XR jitter拒否
- `pelican_test_xrviewspace_test.exe`
  - OpenXR providerのstable eye IDと`$main` family
- `pelican_test_projectionjitter_test.exe`
  - 7 cases / 382 assertions
  - consumer inventoryを含む既存jitter契約
- OpenXR有効Debug build
  - `pelican_core`、上記test target、`pelican_golden_harness`のcompile成功

`golden_temporal_test`全体はWP222後の既存fixtureで、live material
`deferred_geometry / gbuffer_v1`にcompatible passが無いとしてRenderer構築前に3件失敗する。
WP223のViewFamily runtimeへ到達する前のfixture不整合なので、本変更へ混ぜて修正していない。

## 次の実装順

1. directional shadow providerをsecondary ViewFamilyへ移し、stable cascade IDを付ける
2. logical passに`view_family` relationを追加し、`$main`以外を選択可能にする
3. CSMの複数depth target/subresourceとmaterial shadow relationを公開する
4. stencil contractと任意raster task
5. writable material/resource port
6. instance/draw selection domain
7. backend physical scopeのqueue/barrier/fusion policy
8. Vulkan `NativeScope`

この順なら2〜8をRendererの新しい特例として積まず、logical relationから同じphysical
compilerへ接続できる。
