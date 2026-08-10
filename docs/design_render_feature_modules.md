# レンダリング機能モジュール: パージ可能な GPU 機能と 1 行有効化

対象読者: エンジン担当。
ステータス: v1.3 ドラフト(2026-07-31。v1: 2026-07-04)。
前提: [SF](シェーダ自由化キット・実装済み)、[PF]/[PFW](凍結)、
`design_roadmap_renderworld.md` §6(機能はなるべくアセットに)。

## 0. 要求(2026-07-04 ユーザー方針)

shadow / HDR・トーンマップ / IBL / デバッグ描画 / GPU 計測 / スキニング等の
GPU 機能を追加するにあたり:

1. **パージ可能**: 自作シェーダーと同様、使わないプロジェクトでは参照ゼロ =
   パスもリソースもコードパスも立ち上がらない。エンジンに機能を「埋め込まない」
2. **1 行有効化**: 有効にするのに面倒なセットアップを要求しない。
   既定構成が宣言 1 つで立ち上がる
3. **差し替え可能**: エンジン既定をプロジェクト側アセットで置換できる
   (暗黙シャドーイング禁止の原則 [PF] §3-2 は維持 — 参照の書き換えで明示的に)

この 3 つは「全部 config に書け(パージ可能だが面倒)」と「エンジン組み込み
(楽だがパージ不能)」の中間を要求している。答えは **feature fragment =
エンジン標準アセットの束 + config 合成機構**。

## 1. 機構(既存資産の延長 3 つ)

### 1.1 config 合成 — `features` 宣言

rendering config に 1 行:

```json
{
  "features": [
    "engine://features/hdr.json",
    "engine://features/shadow_directional.json"
  ],
  "render_targets": [ ... ],
  "rendering_passes": [ ... ]
}
```

feature fragment(engine:// 埋め込み or プロジェクト内 JSON)は次を宣言できる:

```json
{
  "schema": "pelican.render_feature",
  "version": 1,
  "name": "hdr",
  "render_targets": [ ... ],                 // 追加(名前衝突は hard error)
  "render_target_overrides": {               // 既存 RT の属性差し替え(§1.3)
    "lit_color": {"format": "R16G16B16A16_SFLOAT"}
  },
  "passes": [                                // パス挿入。位置はアンカー指定
    {"insert": "before:present", "pass": { ... "shader": {"fragment": "engine://tonemap"} }}
  ],
  "shader_defines": ["PELICAN_FEATURE_HDR"], // §1.2 の variant に合流
  "required_capabilities": ["pelican.vulkan.ray_query@1"],
  "runtime_shader_compiler": "required"      // 既定。埋め込みだけなら optional
}
```

合成規則(v1 確定させたいもの):

1. 合成順 = `features` 配列順。結果は**合成後の 1 つの config** として
   既存の parser / validation(WW 互換層と同じ制約)を通る —
   合成は解釈レイヤの前段の純ロジック(モジュール・GPU 非依存)
2. 名前衝突(RT・パス)は hard error(暗黙マージしない)
3. `insert` アンカー(`before:<pass名>` / `after:<pass名>` / `end`)が
   見つからなければ hard error
4. **feature を 1 つも書かなければ、合成機構は完全に素通り**(挙動不変)。
   これがパージ可能性の実装形: 機能は fragment を参照したときだけ存在する
5. 差し替え = fragment をプロジェクトにコピーして `features` の参照を
   `engine://` からプロジェクトパスへ書き換える(シェーダ stem と同じ運用)

### 1.2 シェーダ variant — defines

feature がマテリアル/ライティングシェーダに合流する点(影のサンプリング、
トーンマップ前提の出力、スキニングの頂点変形)は **`#define` による variant**:

- `ShaderCompileOptions` に `std::vector<std::string> defines` を追加(小改修)。
  合成後 config の `shader_defines` 集合を、その config が参照する全シェーダの
  コンパイルに注入する
- シェーダ側は `#include "pelican_features.glsl"`(新設)+
  `#ifdef PELICAN_FEATURE_SHADOW` で合流。**feature を使わなければ
  define が付かない = 命令もリソース宣言も消える**(パージのシェーダ側)
- PipelineFactory のパイプラインキャッシュキーに define 集合を含める
- **v1 の割り切り**: defines は実行時コンパイル前提。feature を使う config は
  `PELICAN_RUNTIME_SHADER_COMPILER=ON` を要求し、OFF ビルドでは
  「feature には実行時コンパイラが必要」と明確なエラーで拒否する。
  variant ごとの .spv ビルド時焼き出しは将来課題(§5)。ただし、define や生成 include を
  使わない専用 shader の SPIR-V を feature と一緒に埋め込める場合だけ、fragment が
  `"runtime_shader_compiler": "optional"` を明示できる。省略時は従来どおり `required` である
- `required_capabilities` は feature を挿入する全 graph の
  `target_planning.graphs.<name>.required_capabilities` へ重複なく合流する。未対応環境の拒否は
  target planning の既存 error kind を使い、feature 固有の capability 判定経路を作らない

### 1.3 RT パラメータ化 — `render_target_overrides`

HDR のように「既存パスグラフの RT フォーマットだけ変えたい」ケース用。
override は format / usage の追加に限定(サイズ・名前は不可)。
validation は合成後に走るので、feature が壊れた組を作れば従来どおり弾かれる。

**renderer compiler 追補(2026-07-23)**: 現行 `render_target_overrides` は互換 authoring
frontend として維持する。新しい logical graph では色・深度等の意味を
`LogicalType`、物理候補と品質既定を `ResourcePattern`、exact format pin を
`VulkanPhysicalPlan` 側へ分離する。usage は logical resource use と選択 mechanism から
導出し、feature が常時過剰な usage bit を足す構造を終える。移行中は既存 override を
pattern / pin へ変換する adapter を使い、RPE6a で parser や runtime 挙動を変更しない。
詳細は [`design_render_graph_compiler.md`](design_render_graph_compiler.md) §3.5、§6、§11。

### 1.4 実行時スカラーと既定値の所有

`parameters.scalars[].shader_define: false` は、値を shader variant へ焼かず、
合成済み `CompiledRenderFeatureInstance` に保持する実行時パラメータである。
薄い runtime adapter がこの値を Frame/Light UBO 等へ写し、shader は既存の
set 0 ABI から読む。値を変えるたびに全 shader を再コンパイルしてはならない機能に使う。

機能の非ゼロ既定値は **feature fragment だけが所有する**。engine C++ / shader に同じ
fallback 値を置き、feature が存在するときだけ上書きする方式は禁止する。feature が無い
合成結果に対する runtime adapter の値は zero / neutral とし、pass と define も存在しない。
これにより「参照 = 存在」と既定値の単一所有を同時に守る。

**異種 execution 追補(v1.2)**: `pelican.render_feature` は render authoring frontend であり、
logical compile 後は typed import / export / effect を持つ `GraphFragment` へ変換する。fragment
は合成、所有、hot reload、dump grouping の単位だが、暗黙の barrier / materialization /
最適化境界ではない。非連結 component は許し、publish 前に必須 import と宣言済み
external / state effect の接続が閉じていることを検証する。共通 fragment / closed forest 規則は
[`design_heterogeneous_execution_graph.md`](design_heterogeneous_execution_graph.md) §6 を正とする。
通常featureのdata dependencyはtyped portから導出し、semantic effectの明示はhistory / external
write等がある場合だけでよい。純粋passへeffect boilerplateを要求しない。

## 2. 要求された各機能の適合表

| 機能 | fragment の中身 | シェーダ合流(define) | 備考 |
|------|----------------|----------------------|------|
| デバッグ描画 | line 用 pass + 頂点ストリーム | なし(専用シェーダのみ) | CPU 側 API(`DebugDraw` モジュール)は feature 不参照時 no-op。最も独立性が高く**実証第 1 号に最適** |
| GPU 計測 | パスなし(フラグのみの fragment) | なし | timestamp query をパス境界に挿入。結果は quill ログ + 将来 rpc `get_gpu_timings`。「アセットでなくエンジン機構」だが有効化 UX を features に統一 |
| HDR / トーンマップ | tonemap pass + RT overrides(RGBA16F 化) | `PELICAN_FEATURE_HDR`(出力の意味論) | RT override の実証。EXR(WP26)と接続 |
| shadow(v1: directional 1 灯) | depth-only pass + shadow map RT | `PELICAN_FEATURE_SHADOW`(lighting でサンプリング)+ light UBO 拡張 | シェーダ合流の実証・**最難**。cascade は v2 |
| sky + solid ambient | `scene_depth` を読む背景 pass + runtime 色/強度 | `PELICAN_FEATURE_SKY_AMBIENT` + LightUBO radiance | WP240b。単色の可視性 fallback。IBL / cubemap / irradiance / BRDF LUT は別 feature |
| IBL | サンプリング側のみ(fragment + define) | `PELICAN_FEATURE_IBL` | prefiltered env の生成は**インポート時ベイク**(devcli / import-tools。起動時 compute は compute 基盤待ち) |
| スキニング | vertex 変形(define + joint palette SSBO) | `PELICAN_FEATURE_SKINNING` | クリップ再生・サンプリング(CPU 側)は別軸のスケルタルアニメーション WP。GPU 合流点だけ本モデルに乗る |

## 3. サブセット原則との関係(web)

feature は**描画結果を変える**ので、scene の未知コンポーネント(表示物が
減るだけ → skip 可)とは違い、**web が理解しない feature は hard error** が正しい
(黙って絵が壊れるのを防ぐ)。web は理解できる feature から順に追随
([PFW] の「無視 → 対応」とは意味論が異なることを [PFW] 改訂時に 1 行追記する)。

## 4. 実装順(WP 候補)

| 候補 | 内容 | 依存 | 規模 |
|------|------|------|------|
| WP28 | 合成機構(features/fragment/アンカー/overrides)+ ShaderCompiler defines + PipelineFactory キー拡張。**feature 0 個で挙動不変**(golden 全維持) | 12, 13, 18 | 中 |
| WP29 | 実証 2 本: debug_draw + gpu_timing | 28 | 中 |
| WP30 | HDR / トーンマップ(RT override 実証) | 28, 26 | 中 |
| WP31 | shadow directional(シェーダ合流実証) | 28, 30 | 大 |
| WP32 | IBL(インポートベイク連携) | 30, 21 | 中〜大 |

スケルタルアニメーション(クリップ再生 + スキニング)と入力システムは
本文書のスコープ外(非 GPU 軸)。ロジック実行方式は別設計文書。

## 5. 未決事項

1. variant × ビルド済み .spv の共存(変種ごとの焼き出し規約)— 実行時コンパイラ
   必須の割り切りで v1 は回避。配布最適化が要るときに設計
2. feature のパラメータ表現(影解像度等)— v1 は「fragment をコピーして数値を
   書き換える」で代替(パラメータスキーマの発明を遅延)
3. compute 依存 feature(GPU パーティクル、起動時 IBL ベイク等)— compute パス
   基盤(ロードマップ予約)後
4. web 側 features 対応の順序(おそらく hdr → debug_draw から)
