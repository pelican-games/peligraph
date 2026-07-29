# WP225 cascaded secondary ViewFamily 実装レポート

日付: 2026-07-29

## 1. 結果

secondary ViewFamilyを複数viewへ拡張し、directional cascaded shadow mapを
production runtimeで縦に接続した。標準featureの一つのパラメータブロックから
view provider、array target、schedule、LightUBO、shadow sampling、draw submissionまで
解決する。既定値は1 cascadeなので、既存projectへ常時追加記述を要求しない。

```json
{
  "ref": "engine://features/shadow_directional.json",
  "parameters": {
    "cascade_count": 4,
    "resolution": 2048,
    "max_distance": 120.0,
    "split_lambda": 0.7,
    "stabilize": true
  }
}
```

同梱featureをprojectへコピーして変更する従来のpurge/override境界も維持する。

## 2. providerとtarget

providerはcameraのnearから`max_distance`までをlog/uniform hybridで分割し、
stable ID `$cascade/0`〜`$cascade/N`を持つ`$shadow/directional` familyを生成する。
`split_lambda=0`はuniform寄り、`1`はlog寄りである。`stabilize=true`ではlight-space
projection centerをshadow texelへ揃える。

XRでは左右眼を別々のshadow mapへしない。各splitで全main viewのfrustum cornerを集め、
一つのconservative light-space boundsへunionする。このためflatとXRは同じ
`cascade_count`とtarget layer数を持つ。

feature compilerは`resolution × resolution × cascade_count`のD32 targetを作る。
secondary familyの各viewはsequential invocationに展開され、同じ番号のarray layerへ
描画される。family固有のFrame/Resolution UBOにはshadow target extentが入る。

## 3. light ABIと受光

`LightUBO`はruntime cascade count、2個のpacked split vec4、最大8個の
view-projection matrixを持つ。CPU側はstd140 offsetをstatic assertし、family view数、
target layer数、depth rangeの連続性を実行前に検証する。

standard forward/deferred lightingはmain-view linear depthからcascadeを選び、
`sampler2DArray`の対応layerをmanual depth compareする。最後のsplitより遠いsurfaceは
fully litとして扱う。shadow生成のpush constantと受光のmatrixは同じfamilyから来るため、
callerが同名familyを置換しても座標系が分離しない。

## 4. cascade別draw submission

`PolygonInstanceContainer`が既に公開しているcanonical indexed commandとworld AABBを
入力に使う。各AABBの8 cornerをcascade view-projectionでVulkanのzero-to-one
clip spaceへ変換し、六つのplaneのどれか一つの外側へ全cornerがあるときだけ除外する。
境界交差とbounds不明のdrawは残るためfalse-negativeを作らない保守的判定である。

visible commandはcascadeごとの固定領域へ密な
`vk::DrawIndexedIndirectCommand`として再配置する。material ID、source material、
skinned flagのrange境界は維持するので、shadow pipeline stateのbind回数を増やさない。
通常material/velocity drawとユーザー定義の別familyはcanonical indirect bufferを使い続ける。

## 5. 境界

- cascade ABI上限は8。上限変更はLightUBOとshader ABIの同時変更になる。
- secondary familyはsequential実行。secondary multiviewはまだlowerしない。
- culling consumerはdirectional shadow専用。汎用family policyとして公開するのは
  point/cubeまたはreflectionの二つ目の用途で共通性を確認してからにする。
- PCF/PCSS品質algorithm、point/spot shadow、virtual shadow mapは別機能である。

## 6. 検証

Debug構成で以下を通した。

- CSM provider/view family: 141 assertions / 6 cases
- feature composition: 337 / 28
- draw bounds/frustum: 184 / 10
- surface compiler: 301 / 19
- shader reflection/ABI: 98 / 8
- multiview execution/LightUBO: 77 / 1
- 3-layer B-layer Vulkan golden: 40 / 1

GPU goldenは3 cascadeのtarget metadata、連続split、cascadeごと一回のinvocation、
全D32 layerへのdepth書き込み、最終画像を検証する。さらにsceneへ全cascadeから遠い
casterを置き、main draw queueには存在する一方で各cascadeのcompacted submissionから
除外されることを確認する。
