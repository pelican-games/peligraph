# WP230 planar reflection oblique near-plane projection 実装レポート

日付: 2026-07-29

## 1. 結果

標準planar reflectionのworld clip planeを、fragment discardだけでなくrasterizerのnear clip
境界として使うようにした。鏡面の反対側にあるtriangleはrasterization前にclipされ、
鏡面を横切るgeometryが通常cameraのnear planeまで張り出す問題を避ける。

標準featureの`oblique_near_plane`は既定で`true`である。`false`にすると従来どおり
反射view、raster winding補正、CPU submission culling、FrameUBO clip planeだけを使う。
同名`$reflection/planar` ViewFamilyをruntime callerが提供する全面置換経路も変更していない。

## 2. Vulkan ZO一般式

Pelicanのcamera projectionは右手系、forward-Z、Vulkan zero-to-one clip volume
`-w <= x,y <= w`、`0 <= z <= w`を使う。world plane
`dot(normal, position) + offset >= 0`を反射view spaceへinverse-transposeし、係数`p`を得る。

projection `P`のnear/far faceにある8 cornerを`inverse(P)`でview spaceへ戻し、far側へ
retained距離が増える向きであることを確認する。そのうえでfar faceの
`dot(p, q)`が最大のcorner `q`を選び、projectionのnear rowだけを次で置換する。

```
near_row = p / dot(p, q)
```

これによりplane上は`clip.z = 0`、retained側は`clip.z >= 0`となり、選んだfar cornerは
`clip.z = clip.w`を保つ。projectionの特定要素や対称frustumを仮定しないため、perspective、
off-center XR、orthographic、`preserve_raster_winding`によるclip-X反転を同じ実装で扱える。
row 0/1/3は変えないので、既存projection jitterを後段で適用してもnear plane式は変わらない。

## 3. fallbackと検証

retained half-spaceがcameraを既に含む場合、planeはnear境界としてcamera前方に無い。
またretained側がfar faceと交差しない場合や、near面よりfar面でplane距離が増えない向きも
安定したforward-Z oblique volumeを作れない。これらの条件では
元のprojectionを維持し、同じsemantic clip planeをCPU cullingとfragment discardで使う。
NaN/Inf、zero normal、特異なview/projectionは設定不備として拒否する。

Debug構成で以下を通した。

- ViewFamily/provider: 188 assertions / 9 cases
- feature composition: 469 / 30
- planar reflection Vulkan: 61 / 1

CPU testはperspective + clip-X反転、orthographic off-center、plane上/retained/rejected point、
jitter後のnear式、opt-out、camera-side/receding-plane fallback、特異行列を検証した。
Vulkan fixtureはDeferred、Forward opaque、Forward transparent、
reflection-local clustered selectionを同じoblique projectionで実行した。

## 4. 残件

- planar reflection roughness mip/prefilterと標準sampling policy
- secondary ViewFamily multiview lowering
- point/spot shadowとreflection probe向けcube provider/runtime attachment
