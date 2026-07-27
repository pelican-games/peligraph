# WP210f XR per-view GPU draw 実装レビュー

日付: 2026-07-28
状態: per-view compute / mixed multiview受け入れ実装済み

## 結論

WP210d/eのsegmented depth-pyramid cullingを、flat専用の1回dispatchからlogical viewごとの
実行へ拡張した。project側は`compute_tasks[].schedule: "per_view"`とper-view resource portを
宣言し、shaderは`pelican_view_index()`を使う。エンジンは同じlogical graphを次の物理形へ
lowerする。

- flat/shared 2D: scalar descriptor、view index 0
- sequential XRで1枚を再利用: scalar descriptorを眼ごとの実行で再利用
- sequential XRでlayerを保持: 眼別2D descriptor set
- layered resource: 2D-array descriptorとindexed accessor
- mixed execution: per-view computeはsequential、対応済みrendering scopeだけmultiview

これによりculling algorithmはproject shaderに残しつつ、Vulkan image-viewとdescriptorの
選択はphysical compiler/runtimeが所有する。

## frame / shader ABI

`FrameUniformData`の末尾にあった8 byte paddingを次へ置き換えた。size 352 byteと既存field
offsetは変わらない。

```glsl
uint view_index;
uint view_count;
```

`pelican_view_index()`はsequential graphics / computeでFrameUBO、multiview graphicsで
`gl_ViewIndex`を返す。`pelican_view_count()`はlogical view familyの大きさを返す。

generated resource interfaceはscalar 2Dにもindexed overloadを生成する。scalarの場合は
descriptorが既に当該viewを選んでいるためindexを無視し、arrayの場合だけlayer indexとして
使用する。

## variant / resource修正

XR fixtureで、flat variantがdepth targetをtransient attachment、XR variantがexternal depth
exportのtransfer sourceとして要求する組み合わせが見つかった。同名targetはvariant間で同じ
allocation contractを使うため、usageだけでなくstorage modeとalias groupも統合する。

- storage modeが異なる場合は`materialized`へ昇格
- alias groupが全variantで一致しない場合はaliasを解除して`materialized`へ昇格
- format / mip contractの不一致は従来どおりhard error

また`sequential_2d`には1枚再利用とlayer-per-eyeの両方がある。fullscreen / compute
descriptorは実layer数から両者を区別し、前者をlayer 0固定、後者をview別layerへbindする。

## 実Vulkan受け入れ

2-layer synthetic view-family targetを追加し、異なる左右眼transformで次の3経路を1 logical
frameずつ実行した。

1. sequential XR + CPU DrawQueue fallback
2. sequential XR + segmented GPU culling
3. `auto` mixed execution + segmented GPU culling

受け入れ条件:

- logical frame begin/endは各1回
- sequential targetは2回、view-family targetは1回のrecord/submit
- GPU sequentialとmixed multiviewの左右RGBA8がbyte一致
- 全count bufferと選択segmentが両GPU経路で一致
- viewごとのsegmentは異なるcount slot / output command範囲を持つ
- 両眼で可視数が異なる視点でも結果を共有しない
- depth seed/reduce、count reset、cullは各viewちょうど1回
- physical planはmixed executionを選び、sequential-only pyramidを1-layer
  `sequential_2d`として再利用

既存のlayer-per-eye fullscreen経路と、新しいper-view compute descriptor選択も
`pelican_test_multiview_execution_test`で回帰した。

## 残る境界

1. culling compute、indirect material draw、CPU fallbackのworkload別GPU timingを記録する。
2. 絶対時間をCI閾値にせず、query identityとbreak-even入力契約を固定する。
3. descriptor pressureが実測blockerになるまではbindless state keyを追加しない。
