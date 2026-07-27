# WP210e segmented GPU draw hot reload 実装レビュー

日付: 2026-07-28
状態: graph / shader transaction受け入れ実装済み

## 結論

WP210dのsegmented GPU draw構成は、専用のreload機構を追加せず、既存の2種類の
transactionへそのまま載せられた。

- rendering config変更は、pass、compute task、shader、pipeline、commands/count/segments
  bufferを候補GPU arenaへ構築し、`RendererRuntimeGeneration`を1回だけpublishする
- shaderファイル変更は、既存`ShaderLibrary` / `PipelineFactory` transactionで依存bundleと
  pipelineを一括更新する。この場合render graph generationとbuffer IDは変えない

この区別により、graph ABIの変更とshader実装だけの変更を同じ「ホットリロード」に見せつつ、
不要なgraph再compileを避けられる。

## graph generationの切替

実Vulkan fixtureでは`draw_segments`を512 byteから544 byteへ変更した。reload成功後は次を
確認した。

- renderer root generationが正確に1増える
- `visible_draws`、`visible_draw_count`、`draw_segments`の3 IDが全て新しくなる
- material passの`GpuDrawSourceDefinition`とframe graphのname bindingが同じ新IDを指す
- 旧snapshotが保持する旧segment bufferは512 byteのまま参照可能
- public nameは544 byteの新segment bufferだけを指す
- reload後もsegment countは`{1, 1}`で、reload前と最終RGBA8画像が一致する

つまり、passだけ新しくbufferだけ古いmixed generationは公開されない。

## rollback境界

### segment ABI違反

`scene_draw_segments_v1` bufferを545 byteにして32 byte strideを破った候補は、
GPU arenaのpublish前に拒否される。失敗後はroot pointer、generation、3 buffer ID、
cull shader version、public name bindingの全てが直前の成功generationのまま残る。
その状態で次フレームを描画し、segment countと画像も維持されることを確認した。

### shader compile失敗

`occlusion_cull.comp`への有効なコメント変更では、対象shader bundle versionだけが1増える。
graph rootとcommands/count/segments IDは変わらず、描画結果も同じである。

続いて同shaderへ構文エラーを入れると、shader/pipeline candidateは拒否される。
公開bundle version、graph root、buffer ID、segment count、画像は有効な直前状態を維持する。

## 資源寿命

fixtureは旧generation snapshotを意図的に保持して、新旧bufferを同時に検査する。
検査後に旧snapshotを解放し、device idle後に`DeletionQueue`をflushしてpending resource 0を
確認する。これにより、rollbackの正しさだけでなく、テスト終了時のsafety-net flushへ
依存しないretirementも受け入れ条件に含めた。

## 実Vulkan受け入れ

`pelican_test_golden_cases_test "[wp210]"`で次の4ケース、57 assertionsが通過する。

1. GPU-written indexed draw count 0/1/max/overflowとCPU fallback
2. fixed-state depth-pyramid occlusion
3. multi-state segmented depth-pyramid occlusion
4. segmented graph/shader hot reloadとrollback

## 残る境界

1. XRのper-view depth pyramid/cullingで`sort_view_index`と`visibility_view`を実GPU検証する。
2. 大量command/segmentでGPU timingを取り、CPU pathとのbreak-even pointを記録する。
3. descriptor pressureが実測blockerになるまではbindless state keyを追加しない。

次はXR per-view受け入れを実装し、その後GPU timingへ進む。
