# マテリアル/シェーディング接続点(v1)

対象読者: エンジン担当・プロジェクトでカスタムシェーダを書く人。
ステータス: v1.2(2026-07-10 — **codex 敵対的レビュー
`docs/design_reviews/2026-07-08_material_bc_codex.md` の差し戻しを受理**。
主な改訂: .surface 自己記述コンテナ / spv-link を experimental に降格 /
ターミナルフック排他 / 版付きシンボル / 名前付きスナップショット /
実装順の組み替え。M1 = WP58 実装済みだが **v1.2 形式には未追従**(M2a で解消))。
前提: `design_render_feature_modules.md`(shader_defines・variant 機構)、
`design_scene_format.md`、[PF] シェーダ stem 規約、[PFW] サブセット原則。

## 0. 目的とスコープ

「ユーザーが自分のシェーディングを書く」ときの入口を形式として定義する。

1. **pelican.material v1** — マテリアル定義のプロジェクト形式
   (glTF PBR 1:1 + カスタムシェーダ接続 + パラメータ)
2. **マテリアルシェーダの契約** — set / push constant / 頂点入力の規約を
   文書化された安定 API に昇格
3. **variant 管理の規律** — feature defines × マテリアル defines の爆発抑制

スコープ外: シェーディングモデル自体の刷新(現行 forward を維持)、
ノードグラフ的マテリアルエディタ(devstudio の遠い将来)、bindless。

## 1. 現状(2026-07-08 実装ベース)

- `MaterialInfo` は **マテリアル単位の vert/frag シェーダを既に持つ**
  (ShaderBundleId)+ 固定 PBR スロット(base_color / metallic_roughness /
  normal / emissive)+ VAT 拡張。ランタイムの器はほぼある
- set 規約は `pelican_sets.hpp` に定数として存在:
  set 0 = FRAME、1 = PASS_INPUT、2 = MATERIAL、3 = FREE。
  push constant は engine 64B(mvp)+ shader 64B
- 欠けているのは**プロジェクト側**: glb のマテリアルは固定経路でロードされ、
  「このマテリアルはこのシェーダ・このパラメータ」を**データで**言う手段がない

## 2. カメラと同じ「glTF 同等以上」方針

1. **同等** = glTF `pbrMetallicRoughness` を損失なく読む(factor/texture、
   normal/occlusion/emissive、alphaMode/alphaCutoff/doubleSided)。
   glb 内マテリアルはそのまま既定 PBR シェーダで出る
2. **以上** = カスタムシェーダ・追加パラメータは **glTF を汚さない層**
   (glb では extras、プロジェクトでは pelican.material ファイル)に置く

## 3. pelican.material v1(形式)

置き場所: `materials/*.json`(project://)。エンベロープは他形式と同じ
schema/version。**web でも読める形式にする**(サブセット原則 — shader 参照は
stem 規約なので native/web が同じ記述で成立する)。

```json
{
  "schema": "pelican.material",
  "version": 1,
  "materials": [
    {
      "name": "lava",
      "base": {
        "baseColorFactor": [1, 1, 1, 1],
        "baseColorTexture": "project://textures/lava_albedo.png",
        "metallicFactor": 0.0,
        "roughnessFactor": 0.8,
        "emissiveFactor": [2.0, 0.5, 0.1]
      },
      "shader": "project://shaders/lava",
      "defines": ["LAVA_FLOW"],
      "params": { "flow_speed": 0.35, "distortion": [0.1, 0.2] }
    }
  ]
}
```

- `base`: glTF pbrMetallicRoughness のキー名を**そのまま**使う(1:1)。
  省略時は glTF 既定値。texture 参照は project:// / engine://
- `shader`: 拡張子なし stem(省略 = エンジン既定 PBR)。vert/frag は
  stem から解決(既存規約)。**vert のみ・frag のみの差し替えも可**
  (`shader_vert` / `shader_frag` で個別指定 — 未決 1)
- `defines`: bool フラグのみ(§5 の規律)
- **params/textures の宣言はマテリアル JSON に置かない(v1.2 改訂)** —
  宣言は `.surface` コンテナ(§3-10)がコード側で持ち、マテリアル JSON は
  **`values`(値の上書き)だけ**を持つ:
  `{"name": "lava_blue", "surface": "shaders/lava.surface", "values": {"tint": [0.2,0.5,1.0]}}`。
  Unity の Shader/Material、Godot の shader/material と同じ分業。
  旧 v1.1 の「JSON 側 params 宣言 + 宣言順 std140」は**仕様バグとして廃止**
  (JSON object は順序を持たない — レビュー指摘)
- 実体は**全マテリアル struct を並べた SSBO**(§3-5 と統一。v1.1 の
  「params UBO」表記は誤りとして削除)。GLSL 宣言はコンテナのヘッダから
  エンジンが生成しシム注入(手書きレイアウト一致という概念を廃止)。
  64B の shader push constant は「毎フレーム変わる少量」用として残す

### 3-1. 供給の 3 段(A/B/C 梯子 — 2026-07-08 合意)

**書きやすさの主役は A と B**。旗ではなくキーの選択がモードを決める
(参照 = 存在の原理):

| 段 | 書き方 | 書く量 | 位置づけ |
|----|--------|--------|---------|
| A: uber | `base` + `defines` + `params` + `textures` のみ(shader キーなし) | ゼロ | 既定。カスタムマテリアルの大半はここで足りる |
| B: surface | `"surface": "shaders/lava.surface"` — ユーザーは表面関数(+任意で頂点変位)だけ書く。**結合機構の第一候補 = SPIR-V ABI リンク(§3-6、WP59 スパイクで検証中。成立すれば B も言語自由)**。fallback = GLSL 逆 include | 数行〜数十行 | ライティング・影・スキニング・パス対応はテンプレートが所有 — **機能追加でユーザーファイルは壊れない** |
| C: raw | `"shader": "shaders/weird"` — main() まで全部書く。エンジンライブラリ(`engine://shaders/lib/`)の利用は任意 | 全部 | 完全な自由・自己責任(契約追従義務はユーザー側)。**.spv 直接供給もここ** |

B の規律(レビューで確定):

1. **フックは 2 点から開始**(`pelican_surface(inout PelicanSurface)` /
   `pelican_vertex_displace(...)`)。フック追加はエンジン側の凍結改訂扱い
   (Unity surface shader の迷宮化を規律で防ぐ)
2. **ライティングモデルはフックでなく選択肢**: `"lighting": "standard" | "toon"`
   (defines でテンプレート内ライブラリが切替)。**トゥーンを B の範囲内に
   収める鍵**(VRM 需要)
3. `PelicanSurface` 構造体は公開 API — **`PELICAN_SURFACE_V1` で版数を刻み**、
   意味変更は凍結改訂の流儀
4. テンプレートのピン留め(project:// へコピー)は**許すが C と同義**
   (自己責任へ移行)。同時に B→C の公式移行手順を兼ねる(崖をスロープに)
5. **web は A まで対応**。B/C は native 限定(web は WARN + 既定 PBR
   フォールバック — サブセット原則の明示的適用)
6. エラーは**ユーザーファイル起点に翻訳**して表示(include 展開後の
   テンプレート行番号を出さない)+ マゼンタフォールバック + ホットリロード

### 3-2. SPIR-V ABI が真の契約(自由化との整合)

梯子はすべて**ソースレベルの糖衣**であり、最終形は同一の SPIR-V ABI
(shader_contract.md)。precompiled .spv の自由は C 層として無傷
(既存の割り切りどおり .spv は defines variant 対象外の固定 1 バリアント)。
**B はソース供給専用**(テンプレートと混ぜてからコンパイルするため —
明文化された唯一の制約)。dist-bake(B4)で B/C も全 variant を .spv 化
するので、**ランタイム契約に include は漏れない**。

### 3-3. パス variant 規約(影の剥離防止)

マテリアルの頂点ロジック(変位含む)は main パス専用ではなく、
**同一シェーダを `PELICAN_PASS_DEPTH` / `PELICAN_PASS_VELOCITY` 付きで
再コンパイルして depth / velocity パスでも使う**(揺れる草の影が揺れない
事故の構造的防止)。B ではテンプレートが自動で満たす。C では作者責務
(contract に記載)。M2 実装が main 専用の作りにならないよう最初から要件化。

### 3-4. 属性と欠損の既定

- 頂点属性は現行 5 つ(pos/normal/uv/color/tangent — contract 記載)+
  **予約 location**(uv1 等)を契約に確保し、存在は `PELICAN_HAS_*` defines
- 未バインドのテクスチャスロットは**ダミー自動バインド**(白 / 平坦法線)—
  variant 爆発より「とりあえず動く」を優先。defines 分岐は opt-in
- `render_state`(v1.1 追加、マテリアルの拡張キー): blend
  (`opaque|blend|additive`)・depth_write・cull の最小セット。
  glTF 由来の alphaMode/doubleSided はこれへの別名として受理
- 適用: scene v1 のオブジェクトに `material` コンポーネント
  (`{"name": "material", "ref": "lava"}`)で割当。glb 側マテリアルの
  上書き。**glb extras**(`pelican_material: "lava"`)でも同じことが言える
  (DCC 側で割当を焼く経路 — R6 の extras 規約と同じ)

### 3-5. バインディングモデルの併用(classic + bindless、2026-07-08 方向決定)

**方針: web/モバイルの床に PC を縛らせない。** web に載せたいものは
シンプルな 3D/2D 程度(ユーザー確認済み)なので、classic を可搬性の床として
維持しつつ、PC 向けの bindless を併用で進める。

- **規律は 1 つ**: A/B 層のシェーダはテクスチャを直接宣言せず、
  **生成アクセサ(`<stem>.params.glsl`)経由**で触る。生成 include が
  `PELICAN_BINDLESS` defines でモード差を吸収し、ユーザーコードは
  どちらのモードかを知らない
- classic = set 2 の従来スロット(web・古いモバイル・MoltenVK 安全圏の床)。
  bindless = set 3(PELICAN_SET_FREE)のグローバル配列 + 添字
- **M2 の必須要件**: マテリアルデータは「全マテリアル struct を並べた
  SSBO + テクスチャ参照」で持つ(個別 UBO にしない)。classic 実装でも
  この形にしておけば、bindless 移行はテクスチャ欄の添字化だけになる
- bindless バックエンドは後続ユニット/feature(`PELICAN_BINDLESS`)。
  起動時にデバイス能力(descriptorIndexing・nonUniformIndexing・
  updateAfterBind limits)を見て variant を選択(実行時コンパイル資産が
  ここで効く)。dist-bake は両 variant を焼く
- テスト: golden を bindless on/off で回す(WP40 の OFF スモークと同じ型)
- C 層(生シェーダ)は明示選択(契約文書に両モード記載)。GPU 駆動系の
  bindless 専用技法(GPU 側 draw list 等)は native 専用機能として自然に
  C/native 側に落ちる

### 3-6. B 層の一般化: SPIR-V ABI リンク(2026-07-08 方向・WP59 スパイク検証中)

B の結合を「テキスト include(同一言語必須)」ではなく **SPIR-V の関数 ABI**
で切る — 全言語が唯一共有する層で契約する:

```
template.spv(pelican_surface を Import 宣言して呼ぶ・我々が 1 回コンパイル)
user.spv(pelican_surface を Export。GLSL/HLSL/Slang 何産でもよい)
→ pelican-spv-link(下記)で結合 → 完成モジュール
```

- **pelican-spv-link** = SPIRV-Tools 既存パスのオーケストレーション + 薄い糊:
  ①名前規約で対象関数を特定(マングリング差を無視)②型正規化(行列レイアウト
  修飾・RelaxedPrecision の掃除)③spirv-link 結合 ④spirv-opt でインライン +
  DCE ⑤**ユーザー宣言 descriptor の binding remap**(reflection から CPU 側
  バインド表を自動生成 — ソースレベルの調停不要)⑥spirv-val 必須ゲート
- **ABI 一致は運でなく構成で保証**: 言語ごとの生成シムヘッダ
  (`pelican_surface.{glsl,slang,hlsl}` — 同一レイアウトの PelicanSurface 定義)
  を必ず include させる。ABI 表面の型は scalar/vecN/単純 struct に制限
- 帰結: **B と C は「契約の広さ」だけの違いに統一**(B = 1 関数の ABI、
  C = パイプライン全体の ABI)。B スニペットの .spv 直接供給も可能になる
  (「B はソース専用」制約の解消)。SPIR-V を吐ける将来言語は自動参加
- 変換は実行時コンパイル時点(variant キャッシュに乗る)。dist-bake は
  リンク済み最終 .spv を焼く — ランタイム・配布物にツールは漏れない
- コスト(正直に): 小さなコンパイラツールのオーナーになる。緩和 =
  spirv-val ゲート + GLSL/Slang 両産スニペットの golden 常設
- **v1.2 改訂: 本採用 → experimental に降格**(codex レビューの差し戻し受理)。
  スパイクが証明したのは「特定バージョン 3 つ組 + 人工的小関数 + 1 環境」のみで、
  製品 ABI の将来互換・意味同値・性能・デバッグ・web bake は未検証。
  **B の production baseline = ソース経路(テンプレート合成)、spv-link は
  同じ B API の裏の experimental バックエンド**として競争させ、以下の
  受け入れゲートを全部通過したら昇格を再判断する:
  ①言語別 10 本以上のコーパス(struct/配列リソース/制御フロー/derivative/
  discard/複数フック版数)②コンパイラ latest/latest-1 CI + 正規化前後 golden +
  旧 .spv 互換テスト ③AMD/NVIDIA/Intel/MoltenVK の実機 pipeline+画像テスト +
  naga web bake テスト ④RenderDoc でユーザーソース行が追える debug プロファイル
  ⑤monolithic 比の GPU time/ISA/コンパイル時間/hot reload 閾値 ⑥fuzzer による
  不正 SPIR-V 耐性 ⑦toolchain 全版数 + 入力 hash を含む再現可能キャッシュ。
  SPIRV-Headers/Tools/Reflect は pin/vendor する(SDK 追従禁止)
- **WP59 スパイク結果(2026-07-08): 機構成立の実証**(`experiments/spvlink/REPORT.md`
  が一次資料)。GLSL 産と Slang 産の同一 `pelican_surface` が両方リンク・
  spirv-val 通過・パイプライン生成成功。variant 再リンクも実証。
  判明した本実装への要件:
  1. 本実装はテキストアセンブリ書換でなく **SPIRV-Tools/SPIRV-Reflect の API** で
  2. ABI 表面は scalar/vecN/単純 struct 限定を**確定**(行列・配列・リソース
     ハンドル・ブロックレイアウト依存型は禁止 — Slang の Offset 修飾除去で
     型が合流した実測に基づく)
  3. **combined vs split sampler 問題**: GLSL は combined image sampler、
     Slang は texture/sampler 分離を出す — CPU バインド表は set/binding に
     加えて **descriptor type を持つ**こと。シム側で形を強制するか論理
     テクスチャ→2 リソースの写像規約を生成する
  4. Slang スニペットは `[noinline]` 必須(勝手に main へインライン化される)、
     glslang は `--keep-uncalled` 必須
  5. GLSL include fallback は本リンカの CI 常設まで維持

### 3-7. B 層の拡張確定(2026-07-08 第 2 次レビュー — 本節が §3-1 の B 規律 1・2 を改訂)

**原則: エンジンは機構だけを持ち、表現はスニペット(コンテンツ)に置く。
エンジン同梱の標準マテリアル/アセットは増やさない**(ユーザー方針)。

1. **フックは surface 系 2 点で固定**(`pelican_vertex_displace` /
   `pelican_surface`)+ **カスタム補間チャネル** `pelican_custom0/1`(vec4 ×2、
   displace→surface 間の自由データ路)
2. **ライティングモデル = スニペット**(enum 列挙の廃止): モデルの契約は
   `pelican_brdf(s, light_dir, view_dir, radiance)`(+任意 `pelican_ambient`)。
   ライトループ・影・減衰はテンプレート所有のまま、応答だけをモデルが書く。
   `"lighting": "standard" | "toon"` は**エンジン同梱スニペット(engine://
   コンテンツ)への別名**で、任意の stem も指せる(surface と同じ spv-link
   機構・同じ言語自由)。同梱はこの 2 個で打ち止め — 髪・布等は
   プロジェクト側スニペットの領分。注記: IBL/環境項は独自 BRDF と厳密には
   整合しない(v1 は標準近似)。独自 BRDF は forward 専用(将来 deferred の
   G-buffer は固定モデル前提)
3. **スクリーン入力 = 名前付きスナップショット(v1.2 改訂)**: マテリアルが
   資源名を並べて自動配線させる方式は**廃止**(Godot の SCREEN_TEXTURE が
   踏んだ「コピーは 1 回だけ・透明は写らない・重なる屈折は前の結果を見ない」
   というトレードオフを隠して再実装するだけ — レビュー指摘)。改訂:
   **rendering config が名前付きスナップショット(コピー点)を定義**し、
   マテリアルは `"screen_inputs": ["opaque_color"]` で**参照するだけ**。
   v1 は「opaque 後スナップショット 1 点」のみ・透明同士の逐次屈折は
   **非対応と明記**・コピーのバイト数はプラン dump に表示・マテリアル起点の
   暗黙グラフ変更は禁止。入力は宣言順の set 1 binding と生成 accessor
   `pelican_screen_<name>(uv)` へ lower され、未定義名は名前入りエラーになる。
   plan dump はコピー実ノードと消費 pass の read 依存だけを追加し、RT/マテリアルを
   横断する集約ビューは持たない。屈折・水・深度フェードは v1 の範囲で成立する
4. **web は B まで対応を目標に格上げ**(ユーザー決定): 経路は
   naga(リンク済み SPIR-V → WGSL)一択 — スニペットの言語を問わない。
   web サブセット版テンプレートでリンク → naga → .wgsl を **dist-bake の
   web 版**として焼く(ランタイム変換なし)。C は native 専用のまま
5. **split sampler を標準形に統一**: texture と sampler を分離宣言
   (Vulkan GLSL / Slang / WGSL すべて表現可能 — シムのアクセサが強制)。
   WP59 で発見した combined vs split 問題の解であり、web 対応の要件が
   スパイクの宿題を解く

### 3-8. フックの深さ梯子とエンジンシェーダライブラリ ABI(2026-07-08 第 3 次レビュー)

**ユーザー方針: B でなるべくすべてを書けるように。** §3-7-1 の「フック 2 点固定」を
改訂し、フックを**浅い→深いの梯子**にする(深いほど自由と責任が増える。
定義された最深フックが優先):

| 深さ | フック | テンプレートが守り続けるもの |
|------|--------|------------------------------|
| 1 | `pelican_vertex_displace` | 変換・スキニング・パス variant |
| 2 | `pelican_surface` | ライティング全部 |
| 3 | `pelican_brdf`(+`pelican_ambient`) | ライトループ・影・減衰 |
| 4 | `pelican_lighting`(ライティング段の全権) | パス構造・descriptor・スキニング・variant(**最後まで不可侵**) |

**フックの合成規則(v1.2 で「最深優先」を廃止)**: displace と surface は
**直交**(共存可 — 別ステージ)。**brdf と lighting は排他**(ライティング段の
ターミナルフックは 1 つ — 両方定義は名前入りエラー。黙った無効化の禁止)。
surface は brdf/lighting と共存する(surface が struct を埋め、ライティング段が
それを消費する — 役割が違う)。

**版付きシンボル(v1.2)**: `pelican_surface_v1` のように**シンボル名に版を
焼く**。v1 struct は永久凍結(field 追加も禁止)、拡張は v2 シンボル + 
テンプレート側の v1→v2 アダプタで行う。マクロ(PELICAN_SURFACE_V1)は
ソース再コンパイルの分岐にしかならず、**配布済み .spv の ABI を守れない**
(レビュー指摘)ため版管理の主手段にしない。

- **エンジンシェーダライブラリ ABI**(深さ 3/4 の部品): `pelican_light_count()` /
  `pelican_light(i)` / `pelican_shadow(i, world_pos)` / `pelican_env_ambient(n)`
  等を安定 ABI の関数群として提供。実体は engine lib.spv が Export し
  スニペットが Import(**spv-link 機構の双方向適用** — WP59 実証の逆向き)。
  この関数 ABI が抽象境界なので、エンジン内部が clustered 等に進化しても
  ユーザーのライティングは壊れない
- **dogfooding 原則**: 同梱の standard / toon ライティングスニペット自体を
  **この公開ライブラリだけで実装**する(D0「エディタに特権なし」のシェーダ版 —
  標準シェーダに特権なし)。ABI の十分性をエンジン自身が常時証明し、
  標準スニペットが C への最良の実例を兼ねる
- **保証のグラデーション(契約に明記)**: 深さ 1〜3 = エンジンの機能追加が
  自動で乗る。深さ 4 = コンパイル・動作は壊れないが、新機能はライブラリ関数を
  呼んでいる範囲で乗る(呼ばなければ乗らない — 自由の対価)。
  フック・ライブラリ関数の追加は凍結改訂の流儀、削除・意味変更は不可
- **深さの指定は不要 — 書いた関数が宣言(2026-07-08 ユーザー決定)**:
  コンパイル後の反射で定義済みフックを列挙し、存在するものを繋ぐ
  (合成規則は上記 v1.2 — displace/surface は直交、brdf/lighting は排他)。
  マテリアル側は `"surface"` の stem 参照 1 行のみ。
  1 ファイルに任意の深さまで書き足せば保存のたびその形で動く。
  variant キャッシュキーに検出結果を含める。事故防止 3 規則:
  ①`pelican_` 始まりの未知エクスポート = 名前入りエラー(タイポの無言
  フォールバック根絶)②スニペット内 brdf/lighting と `"lighting"` キーの
  併存 = 起動時エラー(推測しない)③既知フックゼロの参照スニペット = エラー。
  副産物: 同梱 standard/toon も「lighting フックだけのスニペット」として
  完全に同型になる(dogfooding の純化)

### 3-9. deferred 展望とパス振り分け規約(2026-07-08)

**deferred 化してもユーザー契約は無傷**という設計検証済みの見通し:

- A / 深さ 1〜2: 無傷 — `PelicanSurface` は論理 G-buffer(forward = その場で
  照らす、deferred = エンコードして後で照らす。スニペットは 1 文字も
  変わらない)。ABI を小さな struct に制限した決定がエンコード可能性を保証
- 深さ 3(brdf): **deferred 再利用は effect 検証付き opt-in(v1.2 改訂)** —
  任意のユーザー関数は derivative・discard・screen 入力等を使えるため、
  機械的に全画面パスへ移せない(レビュー指摘)。純粋関数性の allowlist を
  通ったスニペットのみ ID switch で deferred パスに同居。**既定は
  深さ 3・4 とも forward 行き**
- blend 系: forward パスへ自動ルーティング
  (ハイブリッド — deferred は半透明のためどのみち forward を併設する)
- 順序制御は統一フレームグラフの既存三層がそのまま効く(依存導出・宣言順・
  after/before + アンカー標準名)。プラン dump / 比較テストで検証可能

**パス振り分け規約(2026-07-08 ユーザー決定 — deferred 以前の今も有効)**:

- **既定 = エンジンが自動振り分け**: blend/alpha → 透明パス、深さ 4 /
  screen_inputs → forward、それ以外 → deferred 有効時は G-buffer パス。
  マテリアルの性質から機械的に決定
- **明示したいマテリアルだけ** `"pass": "<パス名>"` を書く(rendering config で
  定義した任意パスも指せる — アウトライン専用パス等への出口)
- 成立しない指定(blend を deferred_geometry 等)は**名前入りの起動時エラー**
  (黙って直さない)
- deferred 設計書(将来)の残宿題 = ハイブリッド用アンカー標準名の追加のみ

### 3-10. .surface 自己記述コンテナ(v1.2 — 形式の中核改訂)

**「シェーダがインターフェースを宣言し、マテリアルは値を与える」**。
1 ファイル = 構造化ヘッダ(ファイルの正式な一部)+ コード本体:

```glsl
//! pelican.surface v1
//! language: glsl
//! params:
//!   - { name: flow_speed, type: float, default: 0.35, min: 0.0, max: 2.0 }
//!   - { name: tint,       type: vec3,  default: [1.0, 0.5, 0.1], hint: color }
//! textures:
//!   - { name: flow_map, default: "engine://textures/flat_gray", color_space: linear }
//! screen_inputs: []

void pelican_surface(in PelicanSurfaceInput i, inout PelicanSurface s) { ... }
```

- **params は順序付き配列 + 明示型 + default 必須**(min/max/hint は任意 —
  将来のエディタが widget 生成に使う)。レイアウトと GLSL 宣言はヘッダから
  エンジンが生成・シム自動注入(作者はレイアウトを書かない。source map 上は
  仮想ヘッダ)
- texture の default(engine:// 可)により**アセットなしでも絵が出る**
- binding 割当はエンジンが一元管理(VAT の 4/5 との衝突もここで解消)
- マテリアル JSON は `values` で default を上書きするだけ(§3 参照)
- **合格条件(機械テスト化)**: 「新規プロジェクトに .surface を 1 個置き、
  マテリアルから 1 行参照するだけで絵が出る」— ブログ共有可能性の定義

### 3-11. B = C の糖衣の構成的保証(v1.2)

dogfooding(標準スニペットを公開ライブラリで書く)は必要条件だが十分条件では
ない(バインダ/スケジューラの特権を検出できない — レビュー指摘)。追加:

- **`dump-lowered-material`**: すべての B マテリアルを公開 C 記述
  (pelican.material + resources + パス選択)へ機械的に降ろせること。
  CI が「降ろした C 記述の直接ロード」と pipeline layout・frame plan・
  最終 SPIR-V・画像の一致を検証する — **「B は C で作れるものの糖衣」という
  不変条件のテスト化**
- 前提として **C の基盤が先**(M2b): 公開 resources manifest(buffer/image/
  sampler の宣言)、semantic なエンジン供給データ(FrameUBO 再編を含む)、
  render state 拡張(depth compare / stencil / attachment 別 blend 等、
  capability 検証付き)。B テンプレートだけが使える私的 descriptor を禁止
- variant キャッシュキーの拡張: (stem, defines, パス) では不足 —
  toolchain 名/版/全オプション・シム/テンプレート/lib の hash・ABI 版・
  フック集合・bindless モードを含める
- precompiled C(.spv)は**パス別 artifact map**(`stem.depth.spv` 等の命名
  規約)を持つ — 「.spv は variant 非対応」と §3-3 のパス variant 要求の
  矛盾の解消

## 4. マテリアルシェーダの契約(安定 API 化)

`docs/shader_contract.md`(新設、adding_features.md から参照)に以下を明文化:

| 項目 | 規約 |
|------|------|
| set 0 (FRAME) | カメラ行列・時間・ライト UBO(エンジン管理、読み取りのみ) |
| set 1 (PASS_INPUT) | パス入力(前段 RT — feature が宣言) |
| set 2 (MATERIAL) | PBR テクスチャ固定スロット + **全 material data の SSBO 配列** |
| set 3 (FREE) | 予約(ユーザー/将来機能) |
| push constant | engine 64B(触るな)+ shader 64B(自由) |
| 頂点入力 | position/normal/uv/(tangent)の location 固定表 |
| defines 合成 | feature 由来(`PELICAN_FEATURE_*`)→ マテリアル由来の順に結合 |

契約の変更 = 破壊的変更としてバージョン管理(形式凍結の流儀)。
これが「エンジンを読まずにシェーダが書ける」状態の定義。

## 5. variant 管理の規律

- キャッシュキー = (shader stem, ソート済み defines 集合, パス種)。
  WP28 の実行時コンパイル機構をそのまま使う(新設なし)
- **爆発抑制の規律**: defines は bool のみ・数値は material SSBO へ。
  feature defines は config 全体で一様(マテリアル毎に変えない)。
  よって variant 数 = マテリアル defines の実使用組合せ × feature 組合せ
  (プロジェクト実測で管理 — `--dump-frame-plan` の流儀で
  `dump-shader-variants` を用意、未決 3)
- 配布: dist-bake(B4)がこの variant 列挙を焼く対象になる(設計整合のみ、
  実装は B4)

## 6. 移行(WP 候補 — v1.2 で組み替え)

| 段階 | 内容 | 依存 |
|------|------|------|
| M1 | pelican.material パーサ + 契約文書 | **完了(WP58 — ただし旧 v1 形式のみ。v1.2 形式は M2a)** |
| M2a | **形式 drift 解消**: surfaceformat 新設(.surface ヘッダのパース — 順序付き params・明示型・default)+ materialformat を values 方式へ改訂 + 未知キーの扱い確定 | M1 |
| M2b | **C 基盤**: FrameUBO 新設 + set 0 意味統一 + push constant 分割 enforcement + マテリアル SSBO 化 + 公開 resources manifest + render state 拡張(capability 検証付き)+ `dump-lowered-material` の器。既定 PBR は現行挙動維持(golden 全維持) | M2a |
| M3a | **B production baseline(ソース経路)**: テンプレート合成(shaderc includer)+ シム自動注入 + ターミナルフック排他 + 版付きシンボル + 同梱 standard/toon(公開ライブラリのみで実装 = dogfood)+ B→C lowering 同値 CI + エラー翻訳 + example 1 個 + golden | M2b |
| M3b | **spv-link を experimental バックエンドとして追加**(同じ B API の裏)。§3-6 の受け入れゲート 7 項目を全通過するまで既定にしない | M3a |
| M3.5 | **名前付きスナップショット**(rendering config 定義 + マテリアル参照。opaque 後 1 点から)— 屈折/深度フェードの golden | M3a |
| M4 | **web = B-web capability プロファイル**(native B の無条件サブセットとしない): dist-bake が link → naga parse/validate → WGSL → wgpu 検証を hard gate に。非対応理由を artifact に記録 | M3a |

## 7. 未決事項

1. vert/frag 個別差し替え(`shader_vert`/`shader_frag`)を v1 に入れるか
   (推奨: 入れる — VAT が vert 差し替えの現実例)
2. surface params の SSBO レイアウト生成規則(宣言順 std430 と型ごとの padding を
   M3a の生成シムで固定する)
   (推奨: 宣言順。シンプル優先、ツールが offset を計算)
3. variant 列挙ダンプ(`dump-shader-variants`)を M1 に含めるか
4. alphaMode blend の描画順(半透明ソート)は本設計のスコープ外 —
   必要になった時点で別文書(ソートはフレームグラフでなくパス内の問題)
5. occlusion texture スロットは現行 MaterialInfo に無い — M2 で追加するか
   IBL(ライティング設計)と同時か
