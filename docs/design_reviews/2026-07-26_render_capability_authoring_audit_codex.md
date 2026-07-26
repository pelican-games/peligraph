# 描画機構カバレッジ / authoring 使い勝手監査

日付: 2026-07-26
対象:

- `docs/render_mechanism_coverage.md` v2
- `docs/render_authoring_ergonomics.md` v1
- commit `0579614` / `c283a2e` と、その時点の現行コード・テスト

## 1. 結論

2 文書の問題意識は妥当だが、そのまま WP 優先順位には使えない。

1. 実装済み経路を未実装とした誤判定がある。
2. 「技法を理論上組める」と「実際の feature が動いた」を混同している。
3. authoring の摩擦として挙げた一部は、実行時経路ではなく補助的な byte-size
   推定コードを読んだ結果である。
4. 本当に重要な問題は、機構不足、書き味、配布可能性、対象 device 固有機能の
   4 種に分けないと優先順位を誤る。

以後は次の 4 レーンを別々に管理する。

| レーン | 問い | 代表例 |
|---|---|---|
| mechanism | ユーザー空間から到達できるか | shadow resource、compute sampled input |
| authoring | 到達できる機能を安全に読み書きできるか | draw tag、named shader input |
| delivery | 開発時に動いたものを配布できるか | `dist-bake` |
| platform | 特定 device/runtime に実装が要るか | VRS、OpenXR runtime integration |

## 2. カバレッジ文書の訂正

### 2-1. G2「graphics pass は buffer input 不可」は誤り

fullscreen pass は frame-graph buffer を解決し、set 1 の storage buffer として
bind する。

- `PassDefinition::input_buffers`:
  `src/core/renderingpass/renderingpass.hpp:212`
- JSON input の buffer 解決:
  `src/core/renderingpass/renderingpasstargetjsonparser.cpp:155-160`
- descriptor write:
  `src/core/fullscreenpass/fullscreenpasscontainer.cpp:512-519`
- compute write → fullscreen read の実 GPU fixture:
  `test/headless_render_test.cpp:131-152`

`[gpu-arena]` headless Vulkan test は 103 assertions で成功した。

残っている穴は **material/geometry pass が frame-graph buffer を入力に取れない**
ことである。`src/core/renderingpass/renderingpassvalidation.cpp:17-26` が明示的に
拒否する。したがって G2 はこの範囲へ狭める。

### 2-2. G13「front cull / additive / depth authoring 不可」は誤り

`.surface` の `render_state` は次を受理する。

- `blend: opaque | blend | additive`
- `cull: none | front | back`
- `depth: read_write | read_only | disabled`
- `depth_test` / `depth_write`
- depth compare 全種

根拠:

- parse: `src/project/surfaceformat.cpp:530-602`
- Vulkan lowering: `src/core/material/materialcontainer.cpp:284-325`
- test: `test/materiallowering_test.cpp:70-100`

material lowering、surface format、surface compiler の対象テストは合計
1952 assertions で成功した。

よって加算合成は現行機構で書ける。反転ハルも front cull 不足ではない。ただし
同一 material/mesh を base pass と別 surface/state の overlay passへ参加させる
一般的な multipass route が無いため、反転ハル全体は条件付きのままである。
G13 を残すなら **pass-local surface/render-state variant 不足**へ狭める。

### 2-3. material screen input は実配線・実描画済み

`opaque_color` / `linear_view_depth` は型だけでなく descriptor、resize/rebind、
実描画まで接続済みである。

- runtime binding:
  `src/core/material/materialcontainer.cpp:1393-1502`
- refraction surface と headless Vulkan:
  `test/headless_render_test.cpp:2444-2569`

`[headless][render][hybrid]` は 50 assertions で成功した。D5、G5t、I3 の
「実配線次第」は古く、既定の opaque snapshot という制約付きで実装済みとする。

### 2-4. sampler と texture dimension の記述が広すぎる

fullscreen input は filter と address mode を指定できる。

- `src/core/renderingpass/fullscreenpassinfojsonparser.cpp:1-103`
- `src/core/fullscreenpass/fullscreenpasscontainer.cpp:121-168`

固定なのは主に material custom texture の sampler であり、比較 sampler と
anisotropy は未 authoring である。

static material texture の cube/array/3D と、ユーザー authored RT の mip/layer は
未対応だが、XR runtime は 2D array image/view を内部利用している。したがって
「array texture が存在しない」ではなく、**ユーザーが dimension/subresource view を
宣言できない**が正しい。

### 2-5. raster tiled lighting の例は現状の proof にならない

提示された `R32G32B32A32_UINT` は
`src/core/renderingpass/renderingpassjsonhelpers.cpp:11-28` の whitelist に無く、
parse 時点で `Unknown format` になる。

また標準 light inventory は次の 32 灯で固定される。

- directional 8
- point 16
- spot 8

根拠: `src/core/light/light.hpp:8-10`

fullscreen から Frame/Light set を読むこと自体は可能だが、128 灯マスクの例、
forward material での mask 消費、area-light data を含めた縦切りは未検証である。
この技法は「今日書ける ○」ではなく、lighting data contract と実 fixture を要する
dogfood 候補とする。

### 2-6. 件数

v2 は「57 件」と書くが表には 84 行ある。実際の v2 label は次であった。

- ○: 34
- △→○: 1
- △: 22
- ✕: 27

本文集計の 30 + 17 + 20 = 67 とも一致しない。また gap 表は 15 行しかなく、
G5/G7 が存在しなかった。複数技法を 1 行にまとめた項目もあるため、今後は
単純な「解禁数」を優先順位の主根拠にしない。

## 3. authoring 使い勝手文書の仕分け

| 指摘 | 判定 | 訂正 |
|---|---|---|
| A-1 feature 使用 project を shaderc OFF で配布できない | **採用** | `dist-bake` 未実装は delivery の重大 gap |
| A-2 `material_range` が不安定 | **採用、原因訂正** | material ID ではなく `materialrender.cpp:50-56` の draw-call ordinal を選別している |
| A-3 `extent_scale` が display RT に暗黙依存 | **棄却** | 実 RT は registration の `base_extent` 基準。該当コードは snapshot byte-size 推定だけ |
| A-4 custom texture binding を手計算 | **棄却** | `.surface` は `surfacecompiler.cpp:55-84` で宣言と accessor を生成済み |
| A-5 同 anchor の順序が暗黙 | **一部採用** | pass の `after`/`before` は既に有効。残件は cross-feature relation の発見性と曖昧 tie の診断 |
| A-6 compute/graphics が接続不能 | **原因訂正** | planner は両方を同じ reads/writes graph へ正規化し、fullscreen buffer input も接続済み。material port が残件 |
| B-1 計画型が多い | **観測として採用** | 横断 rename はしない。層別 codemap と所有 WP 内の局所整理を優先 |
| B-2 view enum が紛らわしい | **局所負債** | WP204/XR の所有変更と同時にだけ変換関数へ集約する |
| B-3 authoring の入口がない | **棄却寄り** | `manual/06_rendering.md` と `adding_features.md` が既に入口。設計文書の status 過多は別の保守負債 |
| B-4 pass kind が閉集合 | **採用、優先度低** | provider は実装差し替え、kind は v1 閉集合という現契約を文書化。需要前の汎用 registry 化はしない |

追加で残る実用上の摩擦は次である。

1. raw fullscreen/compute shader は `input[]` の順から binding を手で合わせる。
   `.surface` と同様の named generated include が無い。
2. unknown pass/format/contract error が合法候補一覧を出さない箇所がある。
3. material の選択的 pass 参加に、人が安定して指定できる tag/layer がない。
4. feature の実装可否を知るにはコードを読む必要があり、device capability と
   compiled logical/physical plan を一つの診断面で見られない。

## 4. 設計原則

後続実装は次を守る。

1. **技法名を engine enum にしない。** 追加するのは typed resource、view、
   execution、selection など再利用できる機構語彙だけとする。
2. **最低 1 本の user-space dogfood で縦切りする。** parser/API だけを実装完了と
   呼ばない。
3. **既定は自動・高速。** logical authoring に安全宣言 boilerplate を要求しない。
   曖昧だが正しいケースは deterministic tie-break + advisory、矛盾は hard error とする。
4. **raw binding は escape hatch。** 通常経路は logical name から generated accessor
   を作り、番号を人に計算させない。
5. **physical control は WP204 の同層 fragment を使う。** logical feature に Vulkan
   field を漏らさない。
6. **bindless、VRS、NativeScope は具体 workload/device gate を先に置く。**
   「解禁数」だけで大工事を開始しない。

## 5. 推奨実装順

### 5-0. 現在の WP204 slice を閉じる

現作業中の physical scope execution を build/test/commit し、aggressive fusion、
一般 queue/barrier、NativeScope は別 WP に残す。レンダリング技法の追加と
WP204 の無制限な拡張を混ぜない。

### 5-1. public shadow contract

標準 directional shadow を B-layer surface で受けられる縦切りを作る。

- shadow image/matrix/light relation を typed public contract として供給
- `pelican_shadow()` の常時 1.0 stub を実装へ置換
- feature 未参照時は描画 work を増やさず 1.0
- manual compare で開始し、comparison sampler は sampler WP へ分離可能
- copied project feature でも同じ契約を使える
- flat / preview / sequential XR / multiview / hot reload を回帰

これは見た目への効果が最大で、かつ「標準 feature に特権を与えない」境界を
dogfood できる。

### 5-2. stable draw selection と multipass material route

二段階に分ける。

1. material/draw tag と pass の include/exclude filter を追加し、
   `material_range` の draw ordinal 依存を通常 authoring から外す。
2. 同一 mesh/material が base pass と pass-local surface/state variant に参加できる
   typed route を設計する。

反転ハル outline を、entity/material 複製なし、専用 `outline` pass kind の
hardcode なしで書けることを dogfood とする。既存の front cull/additive/depth
surface state を再利用する。

### 5-3. compute と geometry の typed resource port

二段階に分ける。

1. compute pipeline に graphics と同じ Frame/Light set を bind し、
   sampled image + sampler input を追加する。
2. material vertex/fragment から typed frame-graph buffer/image を読める port を追加する。

同時に fullscreen/compute/material の logical input name から generated include を作り、
通常 authoring から binding 番号を隠す。既存 `input` / `output` / `reads` / `writes`
は planner 内部で既に統一されているので、schema を無理に一語彙へ改名しない。

dogfood は compute write → material vertex displacement または固定数 GPU particle とする。

### 5-4. lighting data contract v2 と clustered dogfood

5-3 の resource port 上に構築する。

- 32 灯固定 UBO を scalable inventory へ移す
- directional/point/spot は標準 typed preset として維持
- area/cookie/IES 等の追加データを feature-owned buffer で拡張可能にする
- raster または compute clustered lighting の一方を実動 feature にする
- forward/deferred の両 consumer が同じ light selection 結果を読める

ここで初めて tiled/clustered lighting を ○ と判定する。

### 5-5. texture dimension / subresource view / sampler

static asset と runtime target を別 slice にする。

1. KTX2 2D array/cubemap/3D と generated surface accessor
2. authored RT mip/layer、per-subresource view、dispatch/render target binding
3. material sampler の filter/address/compare/anisotropy

depth pyramid、runtime IBL、froxel/3D LUT の順に dogfood する。XR 内部 array
実装をそのまま public schema とみなさず、logical dimension から device view へ
loweringする。

### 5-6. GPU-driven execution

5-3 以後に、indirect dispatch → GPU-written draw arguments の順で進める。
buffer usage、barrier、count clamp、device feature、fallback を typed plan に残す。
固定 descriptor で成立する workloadでは bindless を前提にしない。

GPU particleか occlusion cullingの実測で descriptor pressureがボトルネックに
なった時点で bindless を別 WP 化する。

### 5-7. delivery lane: `dist-bake`

上記 mechanism と並行可能だが、配布や Quest 着手前には必須である。

- composed feature instance と実使用 define 集合から variant manifest を生成
- `.surface` / fullscreen / compute の必要 SPIR-V を host PC で焼く
- shaderc OFF runtime で feature 使用 project を起動・golden
- target profile と reflection/binding manifest を同梱
- 出力は決定的で、通常 build に Python を再導入しない

### 5-8. device-gated lane

- VRS/foveation: Quest SA2 の実 device facts と計測後
- ray tracing/AS: 対象 GPU と user-space RT technique の具体要求後
- NativeScope: typed/physical fragment で表現不能な実例が出た後

## 6. 先に実装しないもの

- render state authoringの再実装
- fullscreen graphics buffer inputの再実装
- 全 plan 型の一括 rename
- pass kind の汎用 plugin registry
- workloadなしの bindless
- deviceなしの VRS

これらは既存機能との重複、差分規模、または検証不能性に対して投資効果が低い。
