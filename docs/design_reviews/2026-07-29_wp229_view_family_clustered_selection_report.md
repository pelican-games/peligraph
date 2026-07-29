# WP229 ViewFamily-local clustered light selection 実装レポート

日付: 2026-07-29

## 1. 結果

clustered light selectionをmain view専用bufferから、ViewFamilyとviewに帰属するABIへ
一般化した。flat、XRの左右眼、planar reflectionはscalable light inventoryを共有するが、
tile selectionはそれぞれのview/projection/clip planeから生成する。別familyのselectionを
再利用せず、reflectionもLightUBOの8 directional light上限へ退避しない。

標準featureを外せばbuffer、compute task、shader defineは従来どおり全て消える。
同梱`clustered_lighting.json`と`planar_reflection.json`は特権のないfeatureであり、
projectへコピーしてselector、buffer、integrationを差し替えられる。

## 2. selection ABI v2

selection bufferは最大2 view分の領域を持つ。`size_from_extent.copies`は一領域の
header + tile payload全体を反復し、標準featureは2 copiesを指定する。各領域の12-word
headerは次を格納する。

1. magic / version
2. tile count X/Y
3. tile width/height
4. max lights per tile / inventory light count
5. view index / view count
6. stable 64-bit ViewFamily token

その後にtileごとのencoded countと最大64 light indexが続く。selector taskは
`schedule: "per_view"`で、現在のview領域だけを書く。flatは先頭領域、XR sequentialは
左右眼の二領域を使用する。2 viewを超えるfamilyは現行標準bufferの対応範囲外であり、
header/region検証に失敗してLightUBOへ安全側fallbackする。

FrameUBOの既存`frame_index` uvec4はサイズを変えず、`xy`を64-bit logical frame、
`zw`をdeterministic ViewFamily tokenとして使う。consumerはmagic/version/sizeに加え、
headerのview index/count/tokenが現在のFrameUBOと一致した場合だけselectionを読む。
未生成領域、古いbuffer、誤binding、別familyはshader側でも拒否される。

## 3. late cross-feature integration

clustered feature単体へreflection pass名を常時仮定すると、feature順序とproject版の
pass縮小に弱い。そこでcomposerへ`integrations`を追加した。

- 全base featureを通常どおり合成した後に評価する
- `requires`のfeature名が全て存在するときだけfragmentを適用する
- `requires_passes`はintegration適用前のbase pass集合に対して評価する
- fragmentは通常featureと同じtarget/buffer/pass/task/override/resource/defineの
  collision・field検証を受ける

clustered + planar reflectionではintegrationが
`planar_reflection_light_selection`と`planar_reflection_light_select`を追加する。
taskは`$reflection/planar`を使い、Deferred lighting、Forward opaque、
Forward transparentのselection portだけをreflection-local bufferへ置き換える。
inventory bindingはcanonical passから継承されるため共有したままである。

Forward passのoverrideは`requires_passes`付きの独立integrationに分けた。
これにより標準の全capture、opaque-only、deferred-onlyのいずれも、存在しないpassを
参照せず同じfamily-local selectorを利用できる。

## 4. 検証

Debug構成で以下を通した。

- feature composition: 467 assertions / 30 cases
- extent-derived buffer helpers: 525 / 68
- ViewFamily contract: 158 / 8
- surface compiler: 308 / 19
- clustered flat + XR sequential Vulkan: 147 / 1
- planar reflection Vulkan golden: 61 / 1
- multiview execution regression: 79 / 1
- graph variant policy: 95 / 9
- XR feature policy: 19 / 6

clustered fixtureは70 directional lightsをuploadし、flatとXR左右眼の各領域でversion、
tile shape、view index/count、main-family token、64灯truncateとoverflow bitをGPU readback
した。planar reflection fixtureは41 directional lightsを使い、reflection-family tokenと
全41灯のselection、Deferred-only / opaque-only / transparentまでの三構成を実GPUで確認した。

Hybrid全体を`xr.view_execution: "multiview"`必須にすると、現時点ではsnapshot、
geometry、Forwardなど既存nodeのmultiview implementation不足をcompilerが名指しで拒否する。
今回のABIは`gl_ViewIndex`を含むFrameUBO経路へ対応しているが、実行gateは既存Hybridの
secondary/mixed multiview対応が完了するまでXR sequentialを正とする。

## 5. 残る境界

- Hybrid/secondary family全体のmultiview lowering
- planar reflectionのoblique near-plane projection
- roughness mip/prefilterと標準sampling policy
- point/spot shadowやreflection probe向けcube provider/attachment
- 2 viewを超えるfamily向けの動的view-region sizing

次は画質上の欠落を閉じるならoblique near-plane projection、構造上のG6bを進めるなら
secondary multiviewまたはpoint/spot cube providerが候補である。
