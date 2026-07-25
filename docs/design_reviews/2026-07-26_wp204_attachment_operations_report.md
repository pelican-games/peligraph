# WP204 verified attachment operations 実装レポート

日付: 2026-07-26

対象: RPE12b / WP204 Phase B v2 follow-up

判定: **authoring lowering、pure verifier、Vulkan runtime、hot reload実描画、
OpenXR OFF/ON回帰まで完了**

## 1. 実装した境界

通常のproject設定は従来どおりpass-wideの`color_load_op` / `color_store_op` /
`depth_load_op` / `depth_store_op`を使う。FrameGraph compilerが各color/depth outputへ
展開し、automatic physical planは次の型付き契約を保持する。

- raster node名
- logical resource名
- color/depth aspect
- `load|clear|discard`
- `store|discard`

これにより、一般利用者はattachmentごとの記述を常に要求されない。低層を変更する利用者は
ejectしたphysical fragmentだけを編集できる。

## 2. physical fragment version 2

`pelican.vulkan_physical_fragment` version 2へ任意の`attachments`配列を追加した。
entryのidentityは`(node, logical_resource)`で、`load_op`と`store_op`はそれぞれ省略できる。
省略したfieldと、配列に含めなかったattachmentはautomatic planを維持する。

eject結果は全attachmentを含むため、現在のaspect、操作、正確なnode/resource名を推測せず
コピーできる。aspect自体はfragmentで変更できず、automatic logical/physical contractを
正本にする。

version 1はresource/scope/alias packageとして引き続きparseできる。version 1へ
`attachments`を混ぜた文書はunknown fieldとして拒否する。attachment契約を持たないpure
planでは従来のautomatic fingerprintを維持し、不要なartifact churnを起こさない。

## 3. fail-closed verifier

link時に次を検証する。

1. node、physical resource、`(node, resource)` identityが存在し重複しない
2. nodeがraster/output-transformで、対象resourceを論理的にwriteする
3. logical read dependencyがあるattachmentだけが`load`であり、readを`clear` /
   `discard`で破壊したり、存在しないreadを`load`として捏造したりしない
4. 操作を変更するresourceが`materialized_image`またはexternal targetである
5. `Store`→`Discard`は別MSAA resolveが論理値を保存する場合だけ許可する
6. 後続attachmentが同じmultisample surfaceを`Load`する場合はstore discardを拒否する

ClearとDiscardの選択、または保存を増やす変更は上記契約内で許可する。single-sample
surfaceのStore→Discardは、後続のsample/transfer/host利用まで証明するliveness契約を
まだ持たないため拒否する。

## 4. Vulkan runtime vertical slice

runtime compilerはlink済みphysical attachment契約を`PassDefinition`のruntime-only
color/depth操作へ変換する。authoringのpass-wide値は変更せず、physical planが無い既存経路の
fallbackとして残す。

次のconsumerをattachmentごとの操作へ統一した。

- 通常rasterとoutput transformのdynamic rendering attachment
- separate MSAA attachmentのLoad時に必要なlayout/copy判定
- UI renderer
- ImGui renderer
- execution traceのload/store/clear表示

物理planにattachment契約があるのに実行passの出力が見つからない、identityが重複する、
aspectが一致しない場合はcompile時に拒否する。配列長の不一致を黙ってpass-wide設定へ
fallbackしない。

## 5. hot reload実描画

既存headless upscale fixtureで、同じphysical fragmentに次の二つを同時指定した。

1. `low_color`のformatを`R8G8B8A8_UNORM`から`R16G16B16A16_SFLOAT`へ変更
2. `produce_low -> low_color`のloadを`clear`から`discard`へ変更

watcher経路で新しいruntime generationをprepare/publishし、compiled passが
`vk::AttachmentLoadOp::eDontCare`を保持することを確認した。その後に実描画して、
reload前と同じ赤/青画素パターンをreadbackした。format、attachment操作、image/view、
pipelineは一つのgeneration transactionとして切り替わる。

## 6. コミット

| commit | 内容 |
|---|---|
| `900cf37` | version 2 schema、automatic fingerprint、attachment verifier |
| `1a6cf5c` | pass authoringからFrameGraph/physical attachment契約へのlowering |
| `4d23f51` | runtime compiler、Vulkan/UI/ImGui実行、hot reload実描画 |

## 7. ローカル検証

既存の`build-off-openxr`だけを再利用し、OpenXR ON→OFF→ONへ再構成した。最終cacheは
`PELICAN_WITH_OPENXR=ON`である。

### OpenXR OFF

| 対象 | assertions | cases |
|---|---:|---:|
| target planning / frame planning / sample-count / runtime compiler / pipeline resolve / graph variant / headless Vulkan | 1000 | 116 |

### OpenXR ON

| 対象 | assertions | cases |
|---|---:|---:|
| 共通target/frame/pipeline/runtime/headless fixture | 962 | 110 |
| multiview / graph variant / XR action・activation・composition・view・feature・session・discovery | 2596 | 53 |
| **合計** | **3558** | **163** |

全対象が成功した。追加したheadless hot reload test単独では27 assertions / 1 caseが成功した。
ONへ戻した後もruntime compiler、XR composition、headless Vulkanの代表3バイナリを再実行した。

## 8. 残る境界

- automatic scopeをまたぐfusion/reorder
- single-sample surfaceのstore elisionと一般liveness証明
- queue family / barrierの手動記述
- tile-local / alias planのruntime実行と対象GPU gate
- open external boundary、complete raw physical plan、`NativeScope`

次は、実device上で効果を測れるtile-local runtime adapterを先に作り、その証拠を使って
scope fusionとstore elisionの許可範囲を拡張する。queue/barrierは同じ検証なしにraw fieldだけを
公開しない。
