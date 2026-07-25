# WP203c OpenXR array/depth composition 実装レポート

日付: 2026-07-26

対象: XR2b-c — OpenXR array swapchain / depth submit / GPU gate

判定: **ローカル実装・自動テスト完了、外部受け入れ gate 待ち**

本書は完了レポートではない。Meta XR Simulator、物理 HMD、対象 GPU の実測を
この実装ではまだ実行していないため、WP203c は active のまま保持する。

## 1. 実装結果

### 1.1 OpenXR composition

- stereo color target を view ごとの二個の 2D swapchain から、一個の
  `arraySize=2` swapchain へ移行した
- PRIMARY_STEREO の共通 extent を決定し、左右 projection view は同じ image の
  `imageArrayIndex=0/1` を参照する
- color image の acquire / wait / release は logical frame ごとに一回だけ行う
- sequential layer 描画と view-family/multiview 描画が同じ target 契約を使う
- mirror、submission fence、runtime-generation lease、history once の既存境界を維持した

### 1.2 optional composition depth

- discovery で `XR_KHR_composition_layer_depth` を optional extension として交渉する
- logical/physical planner に typed external depth export を追加し、producer、
  format、extent、layer layout、理由を compiled plan に保持する
- extension、compiled depth export、OpenXR format、Vulkan
  depth-attachment/transfer-destination が成立するときだけ
  `arraySize=2` depth swapchain を作る
- 各フレームで source format/extent も一致したときだけ depth submission を
  active にし、不一致なら作成済み depth swapchain も idle のままにする
- renderer は sequential resource を layer ごと、multiview resource を array 全体で
  depth swapchain へ copy する
- 左右 projection view に `XrCompositionLayerDepthInfoKHR` を chain する
- 非対応 extension、depth export 不在、format/extent/usage 不一致、depth swapchain
  作成失敗は color-only へ縮退する。利用しない depth image は acquire しない

### 1.3 measured multiview auto gate

- rendering config に `xr.multiview_auto` を追加した
- profile は Vulkan `vendor_id` を必須とし、`device_id`、`driver_version`、
  `device_name_contains`、compiled `graph` で任意に絞り込める
- measurement は `sequential_gpu_ms`、`multiview_gpu_ms`、`sample_count`、
  `source` を必須とする
- より具体的な profile を優先し、同じ specificity は宣言順で決定する
- profile 未一致時の `auto` は optimize-by-default で multiview を選ぶ
- profile 一致時は multiview が strictly faster かつ
  `minimum_gain_percent` を満たす場合だけ multiview、それ以外は sequential を選ぶ
- 明示 `sequential` と required `multiview` は性能 profile より優先する。
  required mode の device/pass capability error は fallback しない
- physical target plan の `view_execution_plan.auto_gate` に device identity、
  graph、selection、profile、measurement、gain、reason を残す
- hot reload の candidate compile でも active device に対して再解決する

### 1.4 GPU timing evidence

`get_status.gpu_timing` schema v2 に次を additive に追加した。

- `logical_frame_history[]`
  - `logical_frame`
  - `graph_variant`
  - `total_ms`
  - `supported_sample_count`
- `logical_frame_averages[]`
  - `graph_variant`
  - `frame_count`
  - `average_total_ms`
  - `min_total_ms`
  - `max_total_ms`

timing ring 自体は execution mode を持たないため、sequential と multiview は
別 process で測る。live measurement は実行中の policy へ feedback せず、
人が config に固定した profile だけを compiler が読む。

## 2. コミット

| commit | 内容 |
|---|---|
| `71e4999` | optional composition depth extension negotiation |
| `aaac4b9` | 2-layer OpenXR stereo color swapchain |
| `1b32812` | typed external composition depth export planning |
| `c2787e3` | XR composition depth source materialization |
| `3e20df0` | external depth export contract hardening |
| `2ed8fde` | rendered composition depth copy/submission |
| `67ad4ab` | measured device profile based multiview auto gate |
| `7738f4f` | logical-frame GPU timing history/averages |

## 3. ローカル検証

同じ `build-off-openxr` build tree を再利用し、Debug の
`PELICAN_WITH_OPENXR=OFF` と `ON` を順に再構成した。新しい複製 build tree は作っていない。

| 構成 | 実行範囲 | 結果 |
|---|---|---|
| OpenXR OFF | graph variant、target planning、sample count、render helpers、GPU timing、実 Vulkan multiview、golden timing | 653 assertions / 77 cases、全成功 |
| OpenXR ON | 上記 + XR activation、discovery、session、action、composition target、view space、feature policy | 3,149 assertions / 122 cases、全成功 |

主要な個別 gate:

- OpenXR composition protocol fake:
  1,041 assertions / 13 cases
- synthetic Vulkan multiview execution:
  46 assertions / 1 case。sequential reference と左右 layer の byte 一致を含む
- GPU timing unit:
  21 assertions / 3 cases
- golden timing:
  157 assertions / 4 cases
- target planning:
  104 assertions / 14 cases
- graph variant/profile parser:
  OpenXR ON 80 assertions / 8 cases、OFF 54 assertions / 7 cases
- `git diff --check`: 成功

## 4. 外部受け入れ gate

次が揃うまで WP203c を完了 archive へ移さない。

1. 現コードを Meta XR Simulator で起動し、左右 image、mirror、
   color-only/depth-enabled の両経路、validation error 0 を確認する
2. Quest Link/Air Link の物理 HMD で表示、pose/input、depth occlusion、
   focus loss/regain を確認する
3. 同じ scene・解像度・runtime 条件で sequential と optimize-by-default
   multiview を別 process で測る
4. `logical_frame_averages[]` と `auto_gate.device` から device profile を作り、
   `auto_gate` の選択と実測 gain を保存する
5. session loss 後の generation 再生成は別の既知未配線事項として、
   実装するか WP203c の受け入れ条件から明示分離する

profile の記述例と測定手順は
[`manual/06_rendering.md`](../manual/06_rendering.md) §6.12 / §6.14 を参照する。
