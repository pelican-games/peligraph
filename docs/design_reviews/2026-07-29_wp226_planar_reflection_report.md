# WP226 planar reflection ViewFamily 実装レポート

日付: 2026-07-29

## 1. 結果

planar reflectionをdirectional CSMに続く二つ目のsecondary ViewFamily consumerとして
production runtimeへ縦に接続した。標準featureのplaneと解像度から、stable reflection
view、per-view clip plane、独立解像度G-buffer、SSAO、lighting、material resource binding、
draw submission cullingまで解決する。

```json
{
  "ref": "engine://features/planar_reflection.json",
  "parameters": {
    "resolution": 1024,
    "plane_x": 0.0,
    "plane_y": 1.0,
    "plane_z": 0.0,
    "plane_offset": 0.0,
    "preserve_raster_winding": true
  }
}
```

featureを参照しなければtarget、pass、provider、descriptorはいずれも追加されない。
同梱JSONをprojectへコピーしてtarget format、pass chain、bindingを変更する既存の
purge/override境界も維持する。

## 2. ViewFamily provider

planeは`dot(normal, world_position) + offset = 0`として受け取り、normalとoffsetを同じ
倍率で正規化する。main familyの各source viewについてworld reflection transformをviewへ
合成し、camera positionも反転する。view IDは`$mirror/<source-view-id>`なのでframe間で安定し、
family IDは`$reflection/planar`である。

reflection transformは座標系のhandednessを反転する。既定の
`preserve_raster_winding=true`ではprojection Xを反転してraster windingを戻す。
callerが同名familyを明示した場合はbuiltin providerを生成せず、project/runtime固有の
reflection cameraをそのまま使う。無効なzero normal、NaN/Inf、mainとのcardinality不一致、
clip plane欠落はsubmission前に名指しで拒否する。

## 3. clip plane ABI

`RenderViewParameters`へoptional world clip planeを追加し、FrameUBOの末尾へ
`vec4(normal, offset)`として格納した。scalarとmultiviewの両shader ABIが同じview slotを
参照する。標準surface fragmentはworld positionがplaneの反対側にあるfragmentをdiscardする。

clip planeを持たないmain/shadow viewはzero vectorになり、従来surfaceの挙動は変わらない。
ABIはCPUのsize/offset static assert、generated GLSL、SPIR-V reflection、
view別FrameUBO testで固定する。

## 4. 標準feature

標準featureは64〜8192の固定解像度、2-layer targetとして次を追加する。

- albedo、normal、material、world position、emissive
- depth
- SSAO、SSAO blur
- final reflection color

全passは`resolution_domain: independent`かつ`view_family: "$reflection/planar"`である。
geometryは`deferred_geometry_v1`、後段は既存standard fullscreen shaderを再利用する。
feature合成後にsurface resource consumerを解決するよう順序を修正したため、
directional shadow featureを前後どちらに記述してもreflection lightingへ同じshadow inputが
注入される。

final targetはcanonical `forward_transparent` passの`planar_reflection` material resource
portへ割り当てる。materialがportを宣言したときだけdescriptor ABIへ現れるため、
reflection sampling policyはsurface/material側で自由に定義できる。

## 5. generic secondary-family draw preparation

WP225のcascade専用compactionを、family IDとview indexで引く共通prepared indirect bufferへ
一般化した。canonical `RenderCommand` layoutとmaterial/skinned rangeを維持し、既知world
AABBがfrustumまたはclip planeから完全に外れるcommandだけ`instanceCount=0`にする。
交差するboundsとbounds不明のdrawは残す。

material、shadow depth、velocity consumerは同じfamily metadataからbufferとbyte offsetを
選ぶ。GPU-written indirect sourceとsecondary prepared sourceは同時に使わない。
現在の固定cache上限32 secondary viewsを超えた場合はcanonical queueへfallbackし、
描画欠落を起こさない。

## 6. 境界

- 標準captureは`deferred_geometry_v1` routeのopaque geometryを対象とする。
- forward opaque geometryをreflectionへcaptureするpassは未実装。
- transparent geometryのreflection内描画、family別transparent depth sortは未実装。
- secondary familyはsequential実行。secondary multiviewは未実装。
- standard targetはflat/XRを覆う2 layers固定。cube/3D runtime targetは別拡張である。
- oblique near-plane projection、roughness prefilter、Fresnel/歪みは標準providerではなく
  projectのcamera/material algorithmとして後続に残す。

## 7. 検証

Debug構成で以下を通した。

- reflection provider/ViewFamily: 155 assertions / 7 cases
- feature composition: 397 / 29
- generic draw queue/culling: 184 / 10
- surface compiler: 301 / 19
- shader reflection/ABI: 98 / 8
- multiview FrameUBO clip plane: 79 / 1
- planar reflection Vulkan golden: 27 / 1
- cascaded shadow Vulkan regression: 40 / 1

planar GPU goldenは64×64×2のtarget metadata、reflection family invocation、
albedo/depth/final colorの実GPU書き込み、prepared culling view/countを検証する。
CSM goldenも再実行し、専用bufferからgeneric family bufferへの移行後も3-layer shadow、
split、draw exclusionが不変であることを確認した。
