# WP210d GPU draw segment table 実装レビュー

日付: 2026-07-27
状態: multi-state vertical slice 実装済み

## 結論

fixed-stateのGPU-written draw経路を、CPUが確定した複数のmaterial/pipeline stateへ拡張した。
GPUへpipeline handleやdescriptor indexを渡して自由選択させるのではなく、DrawQueueの各
fixed-state rangeをsegmentとして公開する。project compute shaderはsegment内のcommandだけを
選別し、rendererはsegmentごとに従来のstateをbindして`drawIndexedIndirectCount`を発行する。

この境界により、交換可能なculling algorithmと既存material systemを疎結合に保てる。
将来bindless state selectionを追加しても、segment ABIはcoarse binning/fallbackとして残せる。

## 公開ABI

`host_source: "scene_draw_segments_v1"`は32 byte strideで、次の8個の`uint32_t`を持つ。

1. `source_first_command`
2. `command_capacity`
3. `output_first_command`
4. `output_count_index`
5. `sort_view_index`
6. `phase`（0 opaque、1 transparent）
7. `visibility_view`（0 third person、1 first person）
8. `material_filter_index`（`0xffffffff`はfilterなし）

source範囲はpacked `scene_draw_commands_v1` / `scene_draw_bounds_v1`を指す。
output範囲はsegmentごとに重ならず、count slotも1つずつ割り当てる。view visibilityや
material filterが同じsource commandを共有する場合があるため、output容量はsource command数
より大きくなり得る。

## material pass契約

```json
{
  "material_range": {"start": 0, "count": 2},
  "gpu_draw_source": {
    "layout": "draw_queue_segments_v1",
    "segments": "draw_segments",
    "commands": "visible_draws",
    "count": "visible_draw_counts",
    "max_draw_count": 16
  }
}
```

- `segments` bufferは`scene_draw_segments_v1` host source必須
- `material_range`は非空
- `max_draw_count`はoutput command slotの総容量
- commands/count/segmentsはactive frame-graph generationのIDへpinする
- 各選択rangeのsegment record、output範囲、count slotが全て収まる場合だけGPU経路を使う
- 1つでも収まらなければpass全体をCPU DrawQueueへfallbackする

pipeline、material descriptor、material push constant、static/skinned vertex bindingは
segmentごとにCPUが設定する。GPU commandはそのstate境界を越えられない。

## 実Vulkan検証

既存depth-pyramid fixtureへ別materialのgeometryを追加し、1つのproject compute taskで
2つのstate segmentを処理した。

- 1つ目のsegment: occluderを残し、背後のcandidateをrejectしてcount 1
- 2つ目のsegment: 別materialのvisible geometryを残してcount 1
- 選択した2 rangeのmaterial IDが異なることを確認
- segment host populationはsource/writtenとも4
  （third-person 2 state + first-person 2 state）
- GPU segmented pathと`execution: "cpu"`の最終画像が一致
- depth pyramid cullが最終material passより前に実行される

## 残る境界

1. XRではsegment metadataを公開するが、per-view depth pyramid/cullingの実Vulkan検証は未完。
2. culling shaderとsegment構成のhot reload/rollbackはWP210eで完了。
3. 大量segment時のGPU timingとCPU fallbackとのbreak-even pointは未計測。
4. GPUが新規pipeline/material stateを生成するbindless state keyは未実装。
5. output command容量はauthorが確保する。自動arena sizingは後続範囲。

WP210eの結果は
[`2026-07-28_wp210e_segmented_draw_hot_reload.md`](2026-07-28_wp210e_segmented_draw_hot_reload.md)。
次はXR per-viewでsort viewとvisibility metadataが正しく選択されることを実GPUで確認する。
