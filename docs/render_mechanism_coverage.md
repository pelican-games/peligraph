# 描画機構カバレッジ分析: 何がユーザー空間で書けて、何が書けないか(v2)

対象読者: エンジン担当、および feature / material をユーザー空間で書く人。

ステータス: **v2(2026-07-19)— 57 技法へ拡張**。v1(22 技法)に AAA 級・
アニメ調 AAA(Arknights: Endfield 相当)の技法を追加し、機構ギャップを G1〜G16 に
再整理した。判定はすべて 2026-07-19 時点の実コード実測に基づく。

## 0. この文書の目的

エンジンの理念は「機構はエンジン、技法はユーザー空間」。したがって「エンジンが
その機能を持たない」ことは欠陥ではない。問題は一点だけである。

> **その技法を、`src/` に触れずに書き切れるか。**

`○ 書ける` / `△ 条件付き・妥協あり` / `✕ 機構不足` で判定し、`✕` は**どの機構が
欠けているか**を ID で特定する。

## 1. ユーザー空間に露出している機構(実測)

### 1-1. 宣言できるもの

| 機構 | 宣言できること | 根拠 |
|---|---|---|
| render target | name / `width`,`height` または `extent_scale` / `format` / `format_class` / `usage[]` / `history` | [frameplanner.cpp:585](../src/core/renderingpass/frameplanner.cpp:585) |
| **MRT** | **1 パスで複数 color 出力が可能**(独自 G-buffer が書ける) | [frameplanner.cpp:417](../src/core/renderingpass/frameplanner.cpp:417) `parseOutputColors` |
| pass | 8 anchor への `insert`、type は**閉じた 9 種**(`material` / `fullscreen` / `output_transform` / `shadow_depth` / `velocity` / `debug_draw` / `debug_text` / `ui` / `imgui`) | [renderingpassjsonhelpers.cpp:83](../src/core/renderingpass/renderingpassjsonhelpers.cpp:83) |
| material パス | `material_contract`(**4 種の閉集合**)+ `material_range`(index の start/count) | [renderpipeline.cpp:509](../src/project/renderpipeline.cpp:509), [materialpassinfojsonparser.cpp:30](../src/core/renderingpass/materialpassinfojsonparser.cpp:30) |
| pass_overrides | 既存パスへの `input`(RT 画像)追加 | [shadow_directional.json](../src/core/resources/features/shadow_directional.json) |
| compute_tasks | shader / `reads[]` / `writes[]` / `after[]` / `before[]` / `dispatch.groups`(**定数のみ**)/ `per_frame` のみ | [computetask.cpp:342](../src/core/renderingpass/computetask.cpp:342) |
| buffers | 名前付き storage buffer(固定サイズ) | [computetask.cpp:380](../src/core/renderingpass/computetask.cpp:380) |
| material B 層 | hook(`displace` / `surface` / `brdf` / `lighting`)、custom texture、params、**`custom0`/`custom1` 補間チャネル** | [pelican_surface_v1.glsl:9](../src/core/resources/shaders/include/pelican_surface_v1.glsl:9) |
| temporal | `history: true` RT、velocity、projection jitter(数表) | [taa.json](../src/core/resources/features/taa.json) |
| 画像入力 | PNG / KTX2 / **EXR**、ファイル由来 mip | [imageloader.cpp:22](../src/core/loader/imageloader.cpp:22) |

### 1-2. 宣言できないもの(v2 で新たに判明)

| 制約 | 実態 | 根拠 |
|---|---|---|
| **render state** | blend / depth_write / cull は material の `alpha_mode` + `double_sided` から**自動導出**。**加算合成も前面カリングも指定できない** | [materialformat.cpp:807](../src/project/materialformat.cpp:807) |
| **sampler** | filter 以外は固定。address mode 決め打ち、**`compareEnable = false`**、異方性なし | [materialcontainer.cpp:421](../src/core/material/materialcontainer.cpp:421) |
| **stencil** | 露出なし | 0 ヒット |
| **material contract** | 4 種のみ。新しい幾何パス契約(depth-only、outline hull 等)を定義できない | [renderpipeline.hpp:46](../src/project/renderpipeline.hpp:46) |
| **オブジェクト選別** | `material_range`(index 範囲)のみ。layer / tag による選別が無い | 同上 |

## 2. 技法別 判定(57 件)

### A. マテリアル / 陰影(B 層 hook の主戦場)

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| A1 | カスタム BRDF(髪の異方性・布 sheen・clear coat) | **○** | `brdf` hook がそのまま受け口 |
| A2 | セル/ランプシェーディング(PBR ベース) | **○** | `lighting` hook 全権 + ランプ texture |
| A3 | リムライト / フレネル装飾 | **○** | `lighting` hook 内 |
| A4 | **SDF フェイスシャドウ**(アニメ顔の影) | **○** | SDF マスクを custom texture、`lighting` hook で光方向と比較。Endfield/原神系の中核技法が**そのまま書ける** |
| A5 | マットキャップ / 疑似環境 | **○** | custom texture + `lighting` |
| A6 | 視差遮蔽マッピング(POM) | **○** | `surface` hook で UV を反復補正 |
| A7 | ディテールマップ / 三平面投影 | **○** | `surface` hook |
| A8 | 頂点アニメーション(風・揺れ) | **○** | `displace` hook + 時間(frame UBO) |
| A9 | 薄膜干渉 / 虹色 | **○** | `brdf` hook |
| A10 | 目のシェーディング(角膜屈折・視差) | **○** | `surface`+`brdf` hook |
| A11 | 肌のサブサーフェス(前方散乱近似) | **△** | `brdf` 内の解析近似は可。**スクリーン空間拡散(SSSSS)は別パスが要り、対象オブジェクト選別ができない**(G14) |
| A12 | 濡れ・雪の積もり(ウェザリング) | **○** | `surface` hook でマスク合成 |
| A13 | 仮想テクスチャ / テクスチャストリーミング | **✕** | ページテーブル・フィードバックバッファ機構なし(G2/G8) |

### B. ライティング

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| B1 | **タイル/クラスタライティング(raster 方式)** | **○** | §4 参照。**今日書ける最重要技法** |
| B2 | 同(compute 方式) | **✕** | **G1**(compute からカメラ/ライトが読めない)+ **G2** |
| B3 | 面光源(LTC) | **○** | `lighting` hook で LTC 行列を LUT texture から |
| B4 | ライトクッキー / IES プロファイル | **△** | texture は bind できるが、**ライトごとのテクスチャ割り当て**が light 側スキーマに無い |
| B5 | 光源のシャドウ以外の減衰カスタム | **○** | `lighting` hook |
| B6 | 露出制御(自動露出・ヒストグラム) | **△** | 縮小 RT の連鎖で平均輝度は出せる。**ヒストグラム(atomic)は compute が要り G1** |
| B7 | 事前計算ライトプローブ(SH) | **△** | SH 係数を texture/params で持ち込めば `lighting` で評価可。**プローブ配置・ベイクは外部ツール前提** |
| B8 | 動的 GI(DDGI / SSGI) | **✕** | probe 更新に compute + 3D/array texture(G1/G4) |
| B9 | ライトマップ | **△** | 第 2 UV が頂点属性に無ければ不可(要確認)。テクスチャ持ち込み自体は可 |

### C. 影

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| C1 | **B 層マテリアルが影を受ける** | **✕** | **G6a**: [pelican_lighting_v1.glsl:50](../src/core/resources/shaders/include/pelican_lighting_v1.glsl:50) の `pelican_shadow()` が常に `1.0` |
| C2 | カスケードシャドウ(CSM) | **✕** | **G6b**: shadow_depth の view 行列がエンジン固定 |
| C3 | 点光源 / スポット影 | **✕** | G6a + G6b + cube/array なし(G4) |
| C4 | ソフトシャドウ(PCSS) | **✕** | G6a。加えて**比較サンプラが無い**(G12)ので hardware PCF も不可 |
| C5 | **スクリーン空間コンタクトシャドウ(SSCS)** | **○** | depth を input に取る fullscreen でレイマーチ。**影が無い現状の実用的な代替になる** |
| C6 | シャドウキャッシング / 仮想シャドウマップ | **✕** | G6b + mip/tile 管理(G10) |
| C7 | カプセルシャドウ(キャラの接地) | **○** | カプセル配列を params で渡し fullscreen で解析評価 |

### D. 反射 / 環境

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| D1 | **IBL(事前 prefilter 済み)** | **△→○** | cubemap 不在(G4)は**オクタヘドラル 2D で回避可能**。外部ツールで mip 焼き → custom texture |
| D2 | IBL(実行時 prefilter・動的環境) | **✕** | mip 付き RT・per-mip view が無い(G10) |
| D3 | 視差補正リフレクションプローブ | **△** | D1 と同じ持ち込み方 + `lighting` hook で視差補正。**プローブ切替はマテリアル単位に限定** |
| D4 | **SSR(post 方式)** | **○** | scene color + depth を input に取る fullscreen |
| D5 | SSR(material 内・屈折と統合) | **△** | material screen-input は型契約まで実装済(RPE6b1)。実配線は要確認 |
| D6 | 平面反射(鏡・水面) | **✕** | **反射カメラで再描画する口が無い**(G6b と同種: パスに任意の view 行列を与えられない) |
| D7 | ハイブリッド RT 反射 | **✕** | G9 + acceleration structure |

### E. ボリューメトリック / 大気

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| E1 | 高さフォグ(解析) | **○** | fullscreen + depth |
| E2 | 光芒(ゴッドレイ・radial blur 方式) | **○** | fullscreen 連鎖 |
| E3 | ボリューメトリックフォグ(froxel) | **✕** | **3D テクスチャが無い**(G4)。2D atlas 展開なら△だが実用性低 |
| E4 | レイマーチ雲 | **△** | fullscreen でレイマーチ自体は可。**ノイズ 3D テクスチャが無い**ので 2D スライス合成で妥協 |
| E5 | 物理ベース大気散乱(Bruneton) | **△** | LUT を外部ベイクして持ち込めば可。**実行時 LUT 更新は 3D texture が要る**(G4) |

### F. ポストプロセス

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| F1 | ブルーム | **○** | 実装済(hdr.json) |
| F2 | TAA | **○** | 実装済 |
| F3 | **時間的アップスケール(FSR2 相当)** | **△** | jitter/velocity/history 完備。低解像 RT → 高解像 fullscreen で書ける。解像度分離が手作業 |
| F4 | SSAO / HBAO | **○** | depth + ノイズ texture |
| F5 | GTAO(可視性ベイク付き) | **△** | 本体は書けるが**ベント法線を G-buffer に足す**には MRT 追加(可)+ 既存 lighting 改変が要る |
| F6 | 被写界深度 | **○** | depth + 縮小 RT 連鎖 |
| F7 | モーションブラー | **○** | velocity RT |
| F8 | LUT カラーグレーディング | **○** | 3D LUT 無いので 2D strip LUT |
| F9 | 色収差 / ビネット / グレイン / レンズフレア | **○** | fullscreen |
| F10 | シャープネス(CAS 相当) | **○** | fullscreen |
| F11 | **ポスト輪郭線(depth/normal エッジ)** | **○** | アニメ調輪郭の**実用解**。深度・法線から検出 |

### G. 透明 / 特殊描画

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| G1t | ソート済み透明描画 | **○** | `forward_transparent_v1` 契約 + draw sort provider |
| G2t | **反転ハル輪郭線**(アニメ輪郭の王道) | **✕** | **G13**: **前面カリングが指定できない**(`double_sided` bool のみ)。`displace` hook で膨らませても裏面が描けない |
| G3t | OIT(順序独立透明) | **✕** | fragment からの storage buffer + atomic 経路なし |
| G4t | 確率的透明(髪) | **△** | dither + alpha mask は書ける。**TAA との統合**が要調整 |
| G5t | 屈折 / ガラス | **△** | D5 と同じ(screen-input の実配線次第) |
| G6t | 加算合成エフェクト | **✕** | **G13**: blend mode が `opaque`/`mask`/`blend` の 3 択で**加算が無い** |
| G7t | デカール(deferred) | **△** | ping-pong RT なら可。読み書き同時は不可 |

### H. ジオメトリ / 性能

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| H1 | GPU カリング / 深度ピラミッド | **✕** | G1 + G8 + G10 |
| H2 | オクルージョンカリング | **✕** | 同上 |
| H3 | LOD 切替 | **△** | エンジン側に LOD 機構が無い。**別モデルを距離でスワップする実装をゲーム側で書けば可** |
| H4 | インポスター | **△** | H3 と同様。ビルボード生成は外部ツール |
| H5 | メッシュシェーダ / meshlet | **✕** | エンジン専管 |
| H6 | テッセレーション / ディスプレイスメント | **✕** | テッセレーションステージの露出なし |
| H7 | 仮想ジオメトリ(Nanite 相当) | **✕** | G8/G9 + 専用パイプライン |
| H8 | GPU パーティクル | **△** | compute でシミュレートは可。**描画数を GPU が決められない**(G8)ので最大数固定 |
| H9 | 植生の風・大量描画 | **△** | 風は `displace` hook で ○。大量描画は instancing 既存だが**カリングが CPU** |
| H10 | 地形(クリップマップ) | **△** | メッシュ生成をゲーム側で。**高さマップからの頂点変位は `displace` で可** |

### I. 水 / 環境

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| I1 | Gerstner 波 | **○** | `displace` hook |
| I2 | FFT 海面 | **✕** | compute FFT に G1(カメラ不要だが**中間バッファを描画側が読めない** G2) |
| I3 | 水面の屈折 | **△** | screen-input 次第(D5) |
| I4 | 水面の反射 | **△** | SSR で妥協すれば ○。平面反射は ✕(D6) |
| I5 | 泡 / 岸辺の白波 | **○** | depth 差分から fullscreen or material |
| I6 | コースティクス | **○** | 投影テクスチャを `lighting` hook で |
| I7 | 水中(フォグ・歪み) | **○** | fullscreen |

### J. XR

| # | 技法 | 判定 | 根拠 |
|---|---|---|---|
| J1 | マルチビュー(両眼一括) | **○** | 実装済([render_pass_executor.cpp:317](../src/core/vkcore/render_pass_executor.cpp:317)) |
| J2 | **フォービエイテッドレンダリング / VRS** | **✕** | **G11**: 露出ゼロ。**ユーザー空間では原理的に不可能。Quest 単体には必須** |
| J3 | スペースワープ / 再投影 | **✕** | ランタイム連携が要る |
| J4 | 眼球別解像度・注視点連動 | **✕** | G11 + eye tracking 未接続 |
| J5 | ユーザー feature の XR 動作 | **△** | 中間 RT は自動レイヤ化される見込み([render_pass_executor.cpp:83](../src/core/vkcore/render_pass_executor.cpp:83))。**要実測** |

### K. 光線追跡 / 次世代

| # | 技法 | 判定 |
|---|---|---|
| K1 | RT 影 / 反射 / GI | **✕**(G9 + AS) |
| K2 | パストレーシング参照レンダラ | **✕** |
| K3 | ニューラル系(NRC 等) | **✕** |

### L. 集計

**○ 30 / △ 17 / ✕ 20**(重複計上なし)。`✕` の 20 件は **16 のギャップ**に帰着し、
**上位 5 ギャップ(G1・G2・G6・G13・G4)で 14 件**を占める。

## 3. 機構ギャップ一覧

| ID | ギャップ | 塞ぐと解禁される技法 | 根拠 |
|---|---|---|---|
| **G1** | compute の descriptor が storage buffer/image のみ(**UBO もサンプラも無い** → カメラもライトも読めない) | B2, B6, B8, E3, H1, H8, I2 | [computetask.cpp:217](../src/core/renderingpass/computetask.cpp:217) |
| **G2** | graphics パスが **buffer を input に取れない** | B2, H1, H8, I2, A13 | fullscreen パーサに buffer 入力なし |
| **G3** | dispatch が定数のみ・indirect dispatch なし | H1, H8 | [computetask.cpp:246](../src/core/renderingpass/computetask.cpp:246) |
| **G4** | **cubemap / 配列 / 3D テクスチャが存在しない** | D2, E3, E4, E5, C3 | [ktx2.cpp:140](../src/core/loader/ktx2.cpp:140) |
| **G6a** | shadow が公開リソースでない(`pelican_shadow()` スタブ) | C1, C3, C4 | [pelican_lighting_v1.glsl:50](../src/core/resources/shaders/include/pelican_lighting_v1.glsl:50) |
| **G6b** | **パスに任意の view 行列を与えられない** | C2, C3, C6, **D6(平面反射)** | [materialrender.cpp:105](../src/core/renderer/materialrender.cpp:105) |
| **G8** | draw 引数を GPU が書けない | H1, H2, H8, A13 | [drawqueuebuilder.cpp](../src/core/renderer/drawqueuebuilder.cpp) |
| **G9** | bindless なし | H5, H7, K1〜K3 | 0 ヒット |
| **G10** | RT に mip / layer を持てない | D2, C6, H1 | [frameplanner.cpp:585](../src/core/renderingpass/frameplanner.cpp:585) |
| **G11** | VRS / fragment density map 未露出 | **J2, J4** | 0 ヒット |
| **G12** | **比較サンプラ / サンプラ設定が固定** | C4, 影の品質全般 | [materialcontainer.cpp:432](../src/core/material/materialcontainer.cpp:432) |
| **G13** | **render state が material 由来のみ**(前面カリング不可・加算合成不可) | **G2t(反転ハル輪郭)**, G6t | [materialformat.cpp:807](../src/project/materialformat.cpp:807) |
| **G14** | オブジェクト選別が material index 範囲のみ(layer/tag 無し) | A11, 選択的パス全般 | [materialpassinfojsonparser.cpp:30](../src/core/renderingpass/materialpassinfojsonparser.cpp:30) |
| **G15** | material contract が 4 種の閉集合 | 独自幾何パス全般 | [renderpipeline.hpp:46](../src/project/renderpipeline.hpp:46) |
| **G16** | stencil 未露出 | マスク系技法 | 0 ヒット |

## 4. 今日書ける最重要技法: tiled ライティング(raster 方式)

compute でやると G1/G2 で詰むが、**raster なら今の機構で完結する**。

```jsonc
// feature: tiled_lighting.json（エンジン改変ゼロ）
"render_targets": [
  { "name": "light_mask", "extent_scale": 0.0625,          // 1/16 = タイル解像度
    "format": "R32G32B32A32_UINT", "format_class": "data",
    "usage": ["COLOR_ATTACHMENT", "SAMPLED"] }
],
"passes": [
  { "insert": "before:lighting_pass",
    "pass": { "name": "light_cull", "type": "fullscreen",
              "output": { "color": "light_mask" },
              "input": ["depth"],                           // 深度でクラスタ化も可
              "shader": { "fragment": "light_cull" } } }
],
"pass_overrides": { "lighting_pass": { "input": ["light_mask"] } },
"shader_defines": ["PELICAN_FEATURE_TILED_LIGHTING"]
```

1 フラグメント = 1 タイル。frame UBO のカメラ行列と light UBO を読み、タイル錐台に
交差するライトを 128bit マスクに立てる。lighting 側(B 層 `pelican_lighting` hook)は
nearest sample して立っているビットだけループする。**上限 128 ライト/タイル、atomic 無し**
だが実用上は十分。

## 5. 「Endfield 級のアニメ調 AAA」を今の機構で狙うと

**到達できる絵**(エンジン改変ゼロ):

- PBR ベースのセルシェーディング(A2)+ **SDF フェイスシャドウ(A4)**+ リムライト(A3)
- 髪の異方性 BRDF(A1)+ 確率的透明(G4t)
- タイルライティングで多数光源(B1)+ 事前ベイク IBL(D1)+ 面光源(B3)
- **ポスト輪郭線(F11)**+ SSAO(F4)+ SSR(D4)+ ブルーム/DOF/グレーディング(F1/F6/F8)
- **SSCS でキャラの接地影(C5)**+ カプセルシャドウ(C7)
- 高さフォグ(E1)+ 光芒(E2)+ 水面(I1/I5/I6/I7)+ 植生の風(A9→A8)
- TAA + 低解像描画からのアップスケール(F3)

これは**十分に「それっぽい」絵**になる。実際、上の組み合わせは原神・崩壊系の
初期世代とほぼ同等の構成である。

**現状の機構では届かない絵**(= 工事が要る):

1. **通常の影が B 層マテリアルに乗らない**(C1)— これが最大の穴。SSCS で代替しても
   遠景の落ち影が出ない
2. **反転ハル輪郭が引けない**(G2t)— アニメ調の要。ポスト輪郭で代替はできるが、
   線幅・色をマテリアル単位で制御する表現が失われる
3. **加算合成が無い**(G6t)— エフェクト表現が根本的に制限される
4. **ボリューメトリックフォグ / 雲**(E3/E4)— 空気感が出ない
5. **平面反射**(D6)— 水鏡・鏡面床
6. **動的 GI**(B8)— 間接光がベイク頼み

## 6. 機構 WP 優先順位(v2 更新)

| 優先 | WP 案 | 潰すギャップ | 解禁数 | 規模 |
|---|---|---|---|---|
| **1** | **shadow 公開化**(atlas を公開 descriptor + `pelican_shadow()` 実装 + 比較サンプラ) | G6a, G12 | 3 | 中 |
| **2** | **render state の authoring**(cull front / blend mode / depth 制御をパスまたは material で指定) | G13 | 2 | **小** — 費用対効果が最良。**反転ハル輪郭と加算合成が同時に解禁** |
| **3** | **compute の frame set 接続 + graphics の buffer input** | G1, G2 | 7 | 中〜大 — **解禁数が最大** |
| **4** | **パスへの view 行列供給**(CSM・点光源影・平面反射が同じ口で解決) | G6b | 4 | 中 |
| **5** | **テクスチャ次元拡張**(cubemap / array / 3D + RT の mip・layer) | G4, G10 | 5 | 大 |
| **6** | **foveation / VRS** | G11 | 2 | 中 — **Quest 単体に必須。ユーザー空間では不可能** |
| 7 | layer/tag による選別 + material contract 開放 | G14, G15 | 選択的パス全般 | 中 |
| 8 | bindless → GPU draw args | G9, G8 | 次世代一式 | 大 |

**注目**: 優先 2(render state)は**規模が小さいのに解禁効果が大きい**。アニメ調を
狙うなら反転ハル輪郭は事実上必須で、加算合成が無いのはエフェクト表現の根本的制約。
優先 1 と 2 を先にやると「今書ける絵」の完成度が一段上がる。

## 7. 検証方法

静的読解なので、**書いて確かめる**のが最終確認になる。既存の dogfooding 停止パターン:

> `src/` 変更禁止で §4 の tiled ライティング(または §5 の組み合わせ)を feature +
> シェーダだけで書く。**詰まったら実装を止め、file:line で報告して終了する。**

動けば絵が良くなり、止まればその報告書が §6 の WP 定義になる。

## 8. 判定の限界

- 静的読解のみ。RT フォーマットの受理範囲、integer RT のサンプラ挙動、
  fullscreen の入力数上限などで追加の壁がある可能性
- XR(multiview)下でのユーザー feature 動作は未実測(J5)
- 第 2 UV・頂点カラー等の頂点属性の露出範囲は未調査(B9 の判定はこれに依存)
- material screen-input(D5 / G5t / I3)は型契約まで実装済みだが実配線を未確認
