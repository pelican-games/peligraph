# WP228 planar reflection Forward transparent capture 実装レポート

日付: 2026-07-29

## 1. 結果

標準planar reflectionをDeferred + Forward opaqueから、Forward transparentを含むcaptureへ
拡張した。透明drawはmain viewで作ったdepth順を流用せず、reflection familyの各viewから
同じ公開draw-sort providerを再評価する。projectが標準sort providerを置換した場合も、
main viewとsecondary viewの両方に同じ選択が適用される。

transparent passが読むscene color/depthはreflection-local opaque snapshotである。
main-view snapshotの誤用と、現在書き込み中のreflection attachmentへの自己samplingを避け、
opaque reflectionを背景に通常のForward transparency/refraction ABIを再利用する。

## 2. family-local draw queue

`PolygonInstanceContainer`はcanonical resolved draw itemとmaterial filterを保持し、
局所sortを要求されたsecondary familyについて、各viewのcamera origin/forwardから
`CompiledDrawQueueSet`を再構築する。opaque/transparent phase、sort provider名、
material eligibility、fixed-state groupingはmain queueと同じ経路を通る。

prepared secondary drawはper-view indirect commandとper-view draw rangeを対で公開する。
material renderer、shadow-depth renderer、velocity rendererはbufferだけでなくrangeも同じ
queueから選ぶため、sort後のfixed-state segmentとcommand offsetがずれない。

局所sort対象はcompiled planに`forward_transparent_v1`のsecondary material passがある
familyだけである。CSMのように透明passを持たないsecondary familyはcanonical orderを共有し、
viewごとの再sortとqueue複製を行わない。prepared view数は既存の32-view上限に従い、
上限外は既存どおりcanonical queueへ保守的にfallbackする。

## 3. reflection-local opaque snapshot

標準featureはForward opaque capture後に次を追加する。

1. `planar_reflection_color`から`planar_reflection_opaque_color`へのsnapshot copy
2. `planar_reflection_depth`から`planar_reflection_opaque_depth`へのsnapshot copy
3. `planar_reflection_forward_transparent`

snapshot targetはreflection targetと同じextent、2-layer構成で、sampled inputとして使う。
transparent passはreflection color/depthをloadし、opaque snapshotをscene color/depthへ
bindingする。canonical transparent passから継承した`planar_reflection` material resourceは
opaque color snapshotへ局所overrideする。これにより同一attachment read/write feedbackを
作らず、transparent surfaceから一段前のreflectionをsampleできる。

## 4. 検証

Debug構成で以下を通した。

- feature composition: 447 assertions / 30 cases
- draw queue builder: 184 / 10
- multiview execution: 79 / 1
- planar reflection Vulkan golden: 45 / 1
- cascaded directional shadow Vulkan regression: 40 / 1

Vulkan fixtureは同じscreen位置に二つのemissive transparent objectを置き、main cameraと
鏡映cameraでdepth順が反転するようにする。runtimeでmain transparent orderとreflection
family orderが異なることを確認したうえで、Deferred-only、Forward opaqueまで、
Forward transparentまでの三構成を比較した。

opaque追加時はreflection color/depthが変わりG-buffer albedoは不変、transparent追加時は
reflection colorだけが変わりdepthとG-buffer albedoは不変であることをreadbackで検証した。
reflection geometry、SSAO、blur、lighting、Forward opaque、color/depth snapshot、
Forward transparentの8 nodeが実行されたこともtraceで固定した。

## 5. 残る境界

- family-local clustered light selection
- secondary familyのmultiview lowering
- oblique near-plane projection
- roughness mip/prefilterと標準sampling policy
- point/spot shadowやreflection probe向けcube provider/attachment

次の優先はfamily-local clustered selectionである。現在もreflection viewではLightUBOへ
fallbackするため描画は正しいが、多灯時の選択精度と効率はmain-view clusterより低い。
