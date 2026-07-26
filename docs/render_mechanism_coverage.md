# 描画機構カバレッジ分析: 何がユーザー空間で書けて、何が書けないか(v1)

対象読者: エンジン担当、および feature / material をユーザー空間で書く人(= 現状は
主に作者自身)。

ステータス: v1 分析(2026-07-19)。**実コード実測に基づく**。設計文書の意図ではなく
「今のコードで JSON とシェーダだけ書いて到達できるか」を判定した。

## 0. この文書の目的

エンジンの理念は「機構はエンジン、技法はユーザー空間」である。したがって
「機能が無い」ことは必ずしも欠陥ではない。問題になるのは次の一点だけである。

> **その技法を、エンジンのコードに触らずに書き切れるか。**

本書は代表的な描画技法 22 件について、`○ 書ける` / `△ 条件付き` / `✕ 機構不足` を
判定し、`✕` については**どの機構が欠けているか**を file:line で特定する。判定は
2026-07-19 時点の `codex/render-target-runtime-slice` 系の実装に対して行った。

## 1. 現在ユーザー空間に露出している機構(棚卸し)

feature JSON(`pelican.render_feature` v1)+ シェーダ + material `.surface` で
書けるものは次で全部である。

| 機構 | 宣言できること | 根拠 |
|---|---|---|
| render target | name / `width`,`height` または `extent_scale` / `format` / `format_class` / `usage[]` / `history` | [frameplanner.cpp:585](../src/core/renderingpass/frameplanner.cpp:585), [graphvariantpolicy.cpp:78](../src/project/graphvariantpolicy.cpp:78) |
| pass | 8 canonical anchor への `insert`(before/after)、`type` は**閉じた 9 種** | [featurecompose.cpp:28](../src/project/featurecompose.cpp:28), [renderingpassjsonhelpers.cpp:83](../src/core/renderingpass/renderingpassjsonhelpers.cpp:83) |
| pass_overrides | 既存パスへの `input`(RT 画像)追加 | [shadow_directional.json](../src/core/resources/features/shadow_directional.json) |
| shader_defines | variant 分岐 | 同上 |
| compute_tasks | name / shader / `reads[]` / `writes[]` / `after[]` / `before[]` / `dispatch.groups`(定数)/ `schedule: per_frame` のみ | [computetask.cpp:342](../src/core/renderingpass/computetask.cpp:342) |
| buffers | 名前付き storage buffer(固定サイズ) | [computetask.cpp:380](../src/core/renderingpass/computetask.cpp:380) |
| material B 層 | `.surface` の hook(displace / surface / brdf / lighting)、custom texture(set 2 split sampler `7+2i`)、params | [shader_contract.md:206](shader_contract.md) |
| frame データ | per-view 行列・時間・解像度(`views[gl_ViewIndex]`)、object / material / skin / morph SSBO | [pelican_frame.glsl:28](../src/core/resources/shaders/include/pelican_frame.glsl:28), [pelican_sets.hpp](../src/core/shader/pelican_sets.hpp) |
| temporal 基盤 | `history: true` RT、velocity feature、projection jitter(数表指定) | [taa.json](../src/core/resources/features/taa.json), [velocity.json](../src/core/resources/features/velocity.json) |
| 画像入力 | PNG / KTX2 / **EXR**(`PELICAN_WITH_EXR`)、ファイル由来 mip | [imageloader.cpp:22](../src/core/loader/imageloader.cpp:22), [materialcontainer.cpp:466](../src/core/material/materialcontainer.cpp:466) |

**pass type の閉集合**: `material` / `fullscreen` / `output_transform` / `debug_draw` /
`debug_text` / `shadow_depth` / `velocity` / `ui` / `imgui`。未知の type は起動時エラー。

## 2. 技法別 判定表

| # | 技法 | 判定 | 決め手 |
|---|---|---|---|
| 1 | **Tiled / clustered ライティング(raster 方式)** | **○** | tile 解像度 RT(`extent_scale: 0.0625`)へ fullscreen パスでライトマスクを書き、lighting パスが `input` で読む。frame UBO と light UBO は fullscreen から読める |
| 2 | Tiled / clustered ライティング(compute 方式) | **✕** | **G1**(compute から UBO/カメラ・ライトが読めない)+ **G2**(graphics が buffer を読めない) |
| 3 | **IBL(事前 prefilter 済み)** | **△** | cubemap が無い(**G4**)→ オクタヘドラル/equirect 2D + 外部ツールで mip 焼き込み → custom texture として bind すれば書ける。runtime prefilter は ✕ |
| 4 | **SSAO** | **○** | depth を input に取る fullscreen + noise を custom texture + blur。全部既存機構 |
| 5 | **SSR(post 方式)** | **○** | scene color / depth を input に取る fullscreen。history RT でテンポラル安定化も可 |
| 6 | **被写界深度 / モーションブラー** | **○** | velocity RT と depth が既にある |
| 7 | **カスタム BRDF(髪・肌・布)** | **○** | B 層の `brdf` / `lighting` hook がそのまま受け口。**設計の勝ち筋** |
| 8 | **トゥーン輪郭(post edge)** | **○** | depth/normal から fullscreen で検出 |
| 9 | **LUT カラーグレーディング** | **○** | 3D LUT は無いので 2D strip LUT。`output_transform` 前に fullscreen |
| 10 | **時間的アップスケール(FSR2 相当)** | **△** | jitter / velocity / history は完備。低解像 RT → 高解像 RT の fullscreen で書ける。ただし**描画解像度と表示解像度の分離が feature 側の手作業**になる |
| 11 | 屈折 / 水面(material から scene color) | **△** | material screen-input は型契約まで実装(RPE6b1)。実配線の確認が要る |
| 12 | デカール(deferred) | **△** | G-buffer への read-modify-write ができないため ping-pong RT が要る(冗長だが可) |
| 13 | GPU パーティクル | **△** | compute でシミュレートは可(storage buffer)。**描画数を GPU が決められない**(**G8**)ので最大数固定 |
| 14 | **カスケードシャドウ(CSM)** | **✕** | **G6b**: `shadow_depth` パスの view 行列がエンジン固定([materialrender.cpp:105](../src/core/renderer/materialrender.cpp:105))。カスケードごとに別行列を与える口が無い |
| 15 | **点光源 / スポット影** | **✕** | 同上 **G6b** + shadow atlas が公開リソースでない(**G6a**) |
| 16 | **影を受ける B 層マテリアル** | **✕** | **G6a**: [pelican_lighting_v1.glsl:50](../src/core/resources/shaders/include/pelican_lighting_v1.glsl:50) の `pelican_shadow()` が常に `1.0` |
| 17 | **GPU カリング / 深度ピラミッド** | **✕** | **G10**(mip 付き RT が宣言できない)+ **G1**(compute で filtered sample 不可)+ **G8**(draw args を GPU が書けない) |
| 18 | **ボリュメトリック フォグ(froxel)** | **✕** | **G4**: 3D テクスチャが無い。2D atlas 展開なら △ だが実用性は低い |
| 19 | **OIT(順序独立透明)** | **✕** | fragment から storage buffer + atomic を書く経路が無い |
| 20 | **Foveated rendering / VRS** | **✕** | VRS / fragment density map の露出ゼロ。**XR 必須なのにユーザー空間では絶対に書けない** |
| 21 | レイトレーシング | **✕** | bindless(**G9**)+ acceleration structure がエンジン専管 |
| 22 | メッシュシェーダ / meshlet | **✕** | 同上 |

**集計: ○ 6 / △ 6 / ✕ 10。** 「書けない 10 件」のうち **6 件は 4 つの機構ギャップに集中**
している(G1・G2・G4・G6)。

## 3. 機構ギャップ一覧(`✕` の原因)

| ID | ギャップ | 影響する技法 | 根拠 |
|---|---|---|---|
| **G1** | **compute の descriptor が storage buffer / storage image のみ**。UBO も combined image sampler も無い → **カメラ行列もライトも読めず、フィルタ付きサンプリングもできない** | 2, 17 | [computetask.cpp:217](../src/core/renderingpass/computetask.cpp:217), [computetask.cpp:528](../src/core/renderingpass/computetask.cpp:528) |
| **G2** | **graphics パスが buffer を input に取れない**(RT 画像のみ)→ compute → 描画のデータ受け渡しが storage image 経由しかない | 2, 13 | fullscreen パーサに buffer 入力の受理が無い |
| **G3** | dispatch が**定数 groups のみ**。解像度連動も indirect dispatch も無い | 2, 17 | [computetask.cpp:246](../src/core/renderingpass/computetask.cpp:246) |
| **G4** | **cubemap / 配列 / 3D テクスチャが存在しない**。RT 宣言に mip も layer も無い | 3, 18 | [ktx2.cpp:140](../src/core/loader/ktx2.cpp:140)「cubemaps are not supported」 |
| **G6a** | **shadow が公開リソースでない**。`pelican_shadow()` はスタブ、実装は `fullscreen.frag` の `#ifdef` 内に private に存在 | 15, 16 | [pelican_lighting_v1.glsl:50](../src/core/resources/shaders/include/pelican_lighting_v1.glsl:50) vs [fullscreen.frag:86](../src/core/resources/fullscreen.frag:86) |
| **G6b** | shadow_depth パスの **view 行列をユーザーが指定できない**(エンジンが 1 本だけ供給) | 14, 15 | [materialrender.cpp:105](../src/core/renderer/materialrender.cpp:105) |
| **G8** | draw 引数を GPU が書けない(indirect buffer は CPU 構築、count buffer 無し) | 13, 17 | [drawqueuebuilder.cpp](../src/core/renderer/drawqueuebuilder.cpp) |
| **G9** | bindless / descriptor indexing が無い | 21, 22 | 全リポジトリで 0 ヒット |
| **G10** | RT に mip チェーンを持てない・per-mip view が作れない | 17 | [frameplanner.cpp:585](../src/core/renderingpass/frameplanner.cpp:585) の受理キー |
| **G11** | VRS / fragment density map の露出が無い | 20 | 0 ヒット |

## 4. 重要な発見: tiled ライティングは**今日書ける**

`✕` に見えて実は `○` だった代表例。compute でやろうとすると G1/G2 で詰むが、
**raster でやれば今の機構で完結する**。

```jsonc
// feature: tiled_lighting.json（エンジン改変ゼロ）
"render_targets": [
  { "name": "light_mask", "extent_scale": 0.0625,      // 1/16 = タイル解像度
    "format": "R32G32B32A32_UINT", "format_class": "data",
    "usage": ["COLOR_ATTACHMENT", "SAMPLED"] }
],
"passes": [
  { "insert": "before:lighting_pass",
    "pass": { "name": "light_cull", "type": "fullscreen",
              "output": { "color": "light_mask" },
              "input": ["depth"],                       // タイルの深度範囲で錐台を絞る
              "shader": { "fragment": "light_cull" } } }
],
"pass_overrides": { "lighting_pass": { "input": ["light_mask"] } },
"shader_defines": ["PELICAN_FEATURE_TILED_LIGHTING"]
```

- `light_cull.frag` は 1 フラグメント = 1 タイル。frame UBO のカメラ行列と light UBO を
  読み、タイル錐台に交差するライトを **128 bit マスク**(RGBA32_UINT)に立てる
- lighting 側(B 層の `pelican_lighting` hook)はマスクを nearest sample し、立っている
  ビットのライトだけループする
- depth を input に取れるので、**深度スライスを足せば clustered 化も同じ枠で可能**

制約: タイルあたり 128 ライト上限(RT を増やせば拡張可)、atomic を使う可変長ライト
リストは不可。**実用上はこれで十分**で、これが「アルゴリズムの書き方次第で繋がる」
典型例である。

同様に **IBL も今日書ける**: EXR ローダは既にあるので、外部ツールで prefilter 済みの
オクタヘドラル 2D HDR(mip 付き)を焼き、custom texture として `.surface` に bind し、
自前の `pelican_lighting` hook で GGX 分布に応じた mip を引く。cubemap が無いことは
**オクタヘドラル写像で回避できる**(サンプリング数式がユーザー空間にある以上、これは
機構不足ではなく書き方の問題)。

## 5. 「書ける 6 件 + 条件付き 6 件」の実装優先度

エンジンを触らずに絵が良くなる順:

1. **tiled ライティング**(§4)— ライト数の壁が消える。最も効果が大きい
2. **IBL(オクタヘドラル)**— OpenPBR の金属・粗さが初めて意味を持つ
3. **SSAO** — 接地感。depth だけで書ける
4. **カスタム BRDF** — 髪・肌。B 層の設計が正しかったことの実証にもなる
5. SSR / DOF / LUT — 余力で

## 6. エンジン側 WP 候補(`✕` を潰す順)

| 優先 | WP 案 | 潰すギャップ | 規模 | 理由 |
|---|---|---|---|---|
| 1 | **shadow 公開化**(atlas を公開 descriptor + `pelican_shadow()` 実装) | G6a | 中 | **原則違反の是正**。B 層で影が受けられないのは機構の穴 |
| 2 | **compute の frame set 接続**(UBO + sampler を compute descriptor に開放)+ **graphics の buffer input** | G1, G2 | 中〜大 | compute 系技法(GPU カリング、froxel、粒子)が**まとめて解禁**される。単一で最も波及が大きい |
| 3 | **shadow view 行列のユーザー指定**(shadow_depth パスに VP 供給口) | G6b | 中 | CSM / 点光源影がユーザー空間に落ちる |
| 4 | **テクスチャ次元の拡張**(cubemap / array / 3D + RT の mip・layer 宣言) | G4, G10 | 大 | IBL runtime prefilter、froxel、深度ピラミッドが解禁 |
| 5 | **foveation / VRS** | G11 | 中 | Quest 単体で必須。ユーザー空間では原理的に不可能 |
| 6 | **bindless(opt-in)→ GPU draw args** | G9, G8 | 大 | GPU 駆動・RT の前提。ここまで来たら次世代 |

**注目**: 優先 2 の一手で技法 2・13・17・18 が同時に射程に入る。「compute に frame set を
見せる + graphics に buffer input を許す」という**小さな 2 つの穴埋めが、機構投資の
回収効率として突出している**。

## 7. 検証方法(推奨)

各判定は静的読解なので、**実際に書いて確かめる**のが最終確認になる。既存の
dogfooding 停止パターンをそのまま使う:

> エンジンコード(`src/`)変更禁止で §5 の 1 番(tiled ライティング)を feature +
> シェーダだけで書く。**詰まったら実装を止め、詰まった箇所を file:line で報告して
> 終了する。** 動けば絵が良くなり、止まればその報告書が §6 の WP 定義になる。

この方式なら、機構の不足は想像ではなく**実測で**出てくる。過去 5 回(A1.1 / J1b /
WP120 / WP157 / WP164)と同じ運用である。

## 8. 判定の限界

- 静的読解のみ。実際に書くと別の細部(RT フォーマット受理範囲、integer RT の
  サンプラ、fullscreen の入力数上限)で詰まる可能性がある
- XR(multiview)との相互作用は未検証。中間 RT は multiview パスで自動レイヤ化される
  実装([render_pass_executor.cpp:83](../src/core/vkcore/render_pass_executor.cpp:83))
  があるため per-view 中間 RT は動く見込みだが、**tiled ライティングを XR で動かす
  検証は別途必要**
- material パスの render state(blend / cull / depth)の authoring 範囲は本書では
  未調査。反転ハル輪郭など一部技法の判定はこれに依存する
