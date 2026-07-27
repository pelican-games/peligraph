# WP210g GPU draw timing / break-even 実装レビュー

日付: 2026-07-28

状態: workload sweep・型付き観測契約・実 Vulkan 受け入れ実装済み

## 結論

WP210 の GPU-written segmented draw を、単に「動く」状態から、CPU DrawQueue と比較して
採用判断の根拠を残せる状態へ進めた。live timing を runtime policy へ戻す学習ループは
作らず、測定値を immutable な `GpuDrawTimingObservation` として切り出した。

判断に使う v1 metric は両経路の `frame_gpu_ms` である。`culling_gpu_ms` と
`material_draw_gpu_ms` は query attribution の証拠、`host_frame_ms` は
`Renderer::render()` の wall-clock 診断値であり、queue/fence pacing を含み得るため
v1 の選択 metric にはしない。

## 型付き観測契約

`src/project/gpudrawtiming.hpp` は次をデータだけで表す。

- device: vendor/device/driver ID と device name
- graph variant
- workload: candidate / visible / segment / view / output capacity の record 数
- GPU path: graph 全体、count reset + cull、material draw、host frame、sample 数
- CPU path: graph 全体、material draw、host frame、sample 数
- 測定元

JSON は `pelican.gpu_draw_timing` version 1 とし、未知フィールド、0 sample、非有限値、
capacity を超える visible record を reject する。正本 fixture は
`test/fixtures/gpu_draw_timing_observation.json` である。

`evaluateGpuDrawBreakEven()` は minimum sample 数を満たさなければ `inconclusive`、
満たした場合は CPU path に対する GPU path の `frame_gpu_ms` 改善率が
`minimum_gain_percent` 以上のときだけ `gpu_culling` を返す。これは offline 評価器であり、
描画中の観測値から次 frame の経路を変更しない。

## 実 Vulkan sweep

`pelican_test_golden_timing_test "[wp210]"` は hidden candidate を増やし、次を各 24 sample
測る。

| candidate records | visible records | segment records |
|---:|---:|---:|
| 10 | 4 | 4 |
| 130 | 4 | 4 |
| 1024 | 4 | 4 |

両経路には depth prepass と depth-pyramid seed/reduce を共通に残す。GPU path だけが
`occlusion_count_reset` / `occlusion_cull` と compacted indirect-count draw を持ち、
CPU path は通常の CPU DrawQueue indirect draw を使う。これにより、共通の pyramid
生成コストではなく cull と最終 material draw の差を比較する。

CI は次だけを gate にする。

1. GPU path に compute body 2 個と `gbuffer_pass/body` が正しい identity で存在する
2. CPU path に cull node がなく、`gbuffer_pass/body` が存在する
3. query が全回収され、両経路の最終 RGBA8 が一致する
4. 観測 JSON が strict parser を round-trip する

絶対時間や勝者は gate にしない。結果は毎回
`build-off-openxr/test_artifacts/wp210_gpu_draw_break_even.json` 1 ファイルへ上書きし、
一時 project は各 capture 後に削除する。

## 2026-07-28 ローカル参考値

RTX 5080、16×16、4 segment、24 sample、5% gain 条件では次だった。

| candidate records | GPU path ms | CPU path ms | GPU gain | 選択 |
|---:|---:|---:|---:|---|
| 10 | 0.0540 | 0.0459 | -17.7% | CPU |
| 130 | 0.0601 | 0.0464 | -29.6% | CPU |
| 1024 | 0.0832 | 0.0758 | -9.7% | CPU |

この極小 render target / 単純 geometry では対応上限まで crossover が無かった。これは
「GPU culling を削除する」根拠ではなく、candidate 数だけで汎用 threshold を
ハードコードしてはいけない証拠である。実際の preset/device profile を作る前に、
対象解像度、mesh cost、segment 数、visibility ratio、XR view 数、tile GPU ごとに
同じ観測契約で測り直す。

## WP210 の完了境界

WP210a〜f の transport、実 culling、segment、hot reload、XR per-view に加え、
WP210g で timing identity と判断入力契約を閉じた。bindless state key、自動 arena sizing、
runtime の device profile 選択は、実 workload で必要性が確認されたときの別 WP とする。
