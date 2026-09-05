# レイトレーシング拡張設計 — DCC 級の自由度へ(v4)

状態: **v4。設計レビューはここで打ち切る**(1 巡: 14 件 / 2 巡: 9 件 / 3 巡: 6 件。
3 巡の代数・共有 q 配列・互換分岐・D0 は「壊せなかった項目」で生存確認済み)。
§11 が 3 巡への裁定。以降の残余は各 WP の仕様レビューで受ける。

**2026-09-05 利用者裁定:** §4-Pの未実装の数値適合性基盤は拡張を停止し、RT機能・GUI・学習サンプルの実装を優先する。区間oracle/generatorや任意shaderの意味認定の完成は、RT4以降の着手条件にしない。既存検証は有用な範囲で残す。配置・AABB・混在の機能確認は実描画と編集経路で行う。詳細は [v1開発方針](v1_development_plan.md) の最新裁定を正とする。以下の数値規範は参考資料であり、未実装項目を完了とするものではない。

## 0. 要求

利用者要求(2026-08-30): **速度性を重視しない DCC ツールと同程度に「好き勝手」できる
RT**。到達点は視覚的正しさと表現の自由度。リアルタイム性能は制約にしない。
恒常制約: パージ性を下げない / fail-fast / 名前付きエラー / 決定性 / D0。

## 1. 設計開始時の固定点(2 巡で検算済み)

v2 §1 に同じ。追加の検算事実: 再帰は pipelinefactory.cpp:625 で 1 固定 /
SBT record は handle と 1:1(pipelinefactory.cpp:682)/ TLAS offset・mask・
custom index は accelerationstructure.cpp:584 / BLAS は `geometryCount=1`
(同 :507)/ traceRays は次元の正のみ検査(pipelinefactory.cpp:1088)/
生成 include と cache key の既存経路は shadercompiler.cpp:205 が使える。

## 2. 到達点と非目標

v2 §2 に同じ(変形 geometry を含む。denoiser・motion blur AS・SER は別設計)。

## 3. 背骨: SBT の 3 domain モデル(第 2 巡 #1・#2 の採用)

**用語を固定する。混同がこの設計の最大リスクである。**

| 記号 | 名前 | 所有 | 意味 |
|---|---|---|---|
| `g` | logical group | project の `ray_group_layout` | shader の束(closesthit/anyhit/…)の名前 |
| `r` / `R` | ray type index / count | 同上 | primary/shadow 等。`R` = 個数 |
| `q` | scene record row | scene(TLAS build 時に採番) | instance(×geometry)ごとの行。**per-instance データの席** |
| `H[p,g,r]` | shader group handle | pipeline `p` | コンパイル産物 |

- **physical hit record index = `q×R + r`**。record 内容 = `H[p, g(q), r] ∥ data(q)`
  (data(q) = `{material_index, geometry_index}` 等の per-row 定数)
- **Vulkan operand への割り当て(固定)**:
  `instanceShaderBindingTableRecordOffset = q_base×R` /
  `sbtRecordStride = R` / `sbtRecordOffset = r`。
  **raygen へ公開するのは `ray_type_index` / `ray_type_count` / 独立 `miss_index`
  のみ。`g×R+r` や `q` を shader に公開しない**
- stride の別名化: **`record_index_stride`(= R、record 単位)** と
  **`record_byte_stride`(byte 単位、`maxShaderGroupStride` 検査対象)** を
  文書・コード・診断で常に区別する
- **全 pipeline の SBT は同じ `q` 配列を共有**する(handle 列だけが pipeline 差)。
  これにより task ごとの group 並び差は存在せず、TLAS は一意に書ける
- **miss は grid の外**: miss record は独立配列で、task が `miss_index` 順に宣言する
- **RT6で複数geometry BLASを導入する。** 互換な連続qの行をgeometry配列に並べ、
  `q = q_base + geometryIndex`とする。TLAS offsetは先頭q×R、shaderのstrideはRを保つ。
  これにより既存のphysical hit record indexとper-geometry recordを変更しない。
  詳しい結合条件は以下のRT6節に記す。

### ray_group_layout(project 所有)

```json
{ "schema": "pelican.ray_group_layout", "version": 1,
  "ray_types": ["primary", "shadow"],
  "groups": [ { "name": "opaque" }, { "name": "foliage" } ] }
```

- **slot 番号の寿命 = layout digest の世代内のみ**(第 2 巡 #9)。ray type や
  group の追加・並べ替えは digest を変え、**生成 header・shader cache・全 SBT・
  TLAS record base を一 transaction で再構築**する(遅くて良い。速度非重視)。
  部分更新の経路は作らない。この共同 transaction は **TLAS record base を書く
  最初の段(RT1b)で導入**する(RT7 を待たない)
- task 側は「自分が trace する ray type」を宣言し、**その ray type × 全 group の
  充填**を要求される(trace しない ray type は免除)。不足は
  task 名・group・ray type・available 一覧つきの名前付きエラー
- **`ray_group_layout` が無い project は旧 lowering を byte 単位で完全保存**
  (第 2 巡 #6)。自動移行はしない。layout を書いた project だけが新契約に入る
- slot 定数の供給は **compiler 所有の生成 include**(既存 virtual include /
  cache key 経路)のみとし、生成定数は `ray_type_*` と `miss_*` に限る。
  「手書き数値の lint」は撤回(判別不能)。代わりに**範囲検査**(offset/stride の
  device 上限・record 数超過)を dispatch 解決時に行う(第 2 巡 残余)

## 4. オラクル: 決定的部分集合の規範表(第 2 巡 #4)

CPU 参照レイトレーサ(std のみ)との比較は、次の**規範表で機械判定できる subset**
に限って golden 化する:

- **入力 domain**: triangle は非退化(面積 > ε_a)・transform は有限・ray は正規化
  方向・NaN/Inf は fixture 生成時に拒否
- **一意性 margin**: 最近接 hit と次候補の `|t1−t2| > ε_t`、かつ交点は共有辺・頂点
  から ε_b 以上離れる(fixture 生成器がこの条件を**構成的に保証**する。検査に
  頼らない)。ε は device 非依存の設計定数として表に固定
- **any-hit の許可集合**: 候補局所の入力(barycentric・primitive/instance ID・
  record data 定数)からの純関数 accept/ignore のみ。payload 書き込み・
  texture sample・`terminateRay`・副作用は subset 外
- **比較述語**: CPU は区間 `[t_lo, t_hi]` を出し、GPU の hit t・record ID が
  区間と ID 集合に**包含**されること。payload は比較しない
- **off/on 対照**: 機能 off ではなく、**同一 production scene で always-accept
  predicate と実 predicate を走らせ、GPU が実際に読んだ record ID / t /
  可視性が予告どおり変わる**ことを見る
- **sentinel 検査**(SBT 計算の各項): 各 `(q, r)` record の data に一意 sentinel を
  書き、raygen が読んだ sentinel を書き出して復元する。
  検査対象項: `q_base×R`(**非ゼロ必須**)・`r`・miss index・再帰深度の実効。
  `geometryIndex` 項は §3 の恒久制約により除外(明記)
- subset 外の自由 shader は**書ける**が cross-device golden 非保証の区分
  (エラーではなく分類)。golden fixture は subset 内で書く


### 4-P. procedural subset の規範表(2026-09-04。RT3-2 の前提。§11 決定 6(b)・決定 19 が要求した「先に書く」文書)

§4 の入力 domain は三角形だけで、CPU oracle の入力型も `RayTracingOracleTriangle` しかない
([raytracingoracle.hpp:51](../src/core/shader/raytracingoracle.hpp#L51))。RT3-2(static AABB + `.rint`)を golden 化するには、
手続き幾何を **同じ区間算術・同じ包含述語**で判定できる subset を先に固定する。**本節が無い限り procedural は §4 subset 外**であり、
RT3-2 の受け入れから CPU golden を外すことになる。以下は三角形の表([raytracingoracle.hpp:15](../src/core/shader/raytracingoracle.hpp#L15)-26 の定数)を**拡張**する。

#### 4-P.1 入力 domain(fixture 生成器が構成的に保証する。検査に頼らない)

| 項目 | 契約 | 定数 |
|---|---|---|
| AABB | 有限、`min ≤ max` を**全軸で strict**(`min_i < max_i`)。零厚は subset 外(名前付き拒否 `procedural_aabb_degenerate`) | 各軸の厚み `≥ ε_box = 1.0e-2`(座標 domain ±2^10 に対して相対 1e-5) |
| AABB と幾何 | 手続き幾何(球)は AABB に**厳密に内包**され、AABB の各面から `ε_pad = 1.0e-2` 以上離れる(球: `center ± (radius + ε_pad)` が box 内) | `ε_pad = 1.0e-2` |
| 球 | `center` は座標 domain 内、`radius ∈ [r_min, r_max] = [1.0e-1, 2^8]`。`radius` は f32 で正確に表現できる値(dyadic)に限る | `r_min = 0.1`、`r_max = 256` |
| ray | §4 と同じ(正規化方向、origin は domain 内、`t ∈ [t_min, t_max]`)。**origin は球の内側に置かない**(内側からの root 選択は subset 外) | `t_min = 1e-3`、`t_max = 4096` |
| 入射 | ray と球面の交差角の余弦 `|d · n| ≥ minimum_incidence_dot`(掠め入射を排除。三角形と同じ) | `0.0871`(約 85°) |
| 判別式 margin | `b² − 4ac` の**下限**を fixture 生成器が保証: 判別式の区間が 0 を含まない(接線・準接線を排除) | `disc_min = 1.0e-2 · radius²` |
| 一意性 | 最近接 hit と次候補(同一球の第 2 root、他の instance、三角形)の `|t1 − t2| > ε_t`。`ε_t` は §4 の `relative_t_epsilon` を t に乗じたもの | `relative_t_epsilon = 1e-3` |
| AABB の slab と hit | ray は AABB の**辺・頂点から `ε_b` 以上離れて**入射する(slab の縮退を排除) | `barycentric_epsilon` と同じ `1e-3`(box 面上の正規化距離) |
| transform | §4 と同じ(条件数 ≤ 100、行列式 ≥ 1e-3)。**非一様 scale は subset 外**(球が楕円体になり root の式が変わる)。`.rint` は object 空間で交差し、t は world へそのまま(rigid + uniform scale なら t は scale 倍) | `maximum_transform_condition`、`minimum_transform_determinant` |
| NaN / Inf | fixture 生成時に拒否(§4 と同じ) | — |

#### 4-P.2 CPU oracle(double + 有向丸めの区間算術。三角形と同じ構成法)

- **AABB slab**: 各軸で `t_near_i = (min_i − o_i) / d_i`、`t_far_i = (max_i − o_i) / d_i` を区間で計算(`d_i` の区間が 0 を含む軸は「無限 slab」として除外し、`o_i ∈ (min_i, max_i)` を区間で確認)。`t_enter = max_i t_near_i`、`t_exit = min_i t_far_i`。
  `t_enter.upper < t_exit.lower` を要求(交差の区間が確定しない fixture は生成器が拒否)
- **球**: `oc = o − c`、`b = 2 (d · oc)`、`c' = |oc|² − r²`、`disc = b² − 4 c'`(`a = 1`、正規化方向)。`disc` の区間 lower > `disc_min`。root は `t = (−b − √disc) / 2`(origin が外側なので**小さい方が最近接**)。
  `√` は区間の両端に `sqrt` を当て、`nextDown / nextUp` で丸める。`t` の区間が `[t_min, t_max]` を跨ぐ fixture は拒否
- **AABB 内包含**: `t` の区間全体が `[t_enter.lower, t_exit.upper]` に入ること(手続き幾何の hit は必ず box の中で起きる)
- **hit attribute**: `.rint` が報告する属性は **`t` と `hitKind(u32)` だけ**を subset に入れる。法線・uv は payload 扱いで比較しない(§4 の「payload は比較しない」と同じ)。`hitKind` は fixture が宣言した定数(sentinel)で、GPU が読み戻した値と一致すること
- **端点の f32 padding**: 三角形と同じ `endpoint_f32_ulp_padding = 4`(`padEndpoints`)
- **区間幅の上限**: `(t.upper − t.lower) / t_mid ≤ maximum_relative_interval_width = 1e-2`。超える fixture は生成器が**拒否**する(「区間を広げれば常に通る」を防ぐ。§4 と同じ)

#### 4-P.3 GPU との比較述語

- GPU の hit `t`(f32)が CPU 区間に**包含**され、record ID 集合と `hitKind` が一致すること。any-hit は §4 の整数述語 IR(`RayTracingAnyHitIntegerPredicate`)をそのまま使う(`.rint` の後の any-hit も同じ許可集合)
- **三角形と球の混在**: 最近接判定は両方の候補を同じ区間算術で比較する。一意性 margin は候補の型を問わず適用
- **off / on 対照**: AABB geometry を宣言しない同一 scene では球が写らない(三角形だけ)。`.rint` を `unused` に戻すと GPU が交差を報告しない = 三角形との最近接が変わる

#### 4-P.4 subset 外(書けるが golden 非保証)

零厚 AABB / 接線・掠め入射 / 球の内側からの ray / 非一様 scale / 楕円体・カプセル等の他の手続き幾何 / 法線・uv の比較 / 複数 `.rint` の同一 AABB 内競合 / `reportIntersectionEXT` を複数回呼ぶ shader。

#### 4-P.5 受け入れの骨(RT3-2 WP へ引き継ぐ。§4 規約 10)

- 生成器: 上の各拒否条件を 1 つずつ満たさない fixture を作ると名前付きで拒否される(零厚 / 接線 / 内側 origin / 非一様 scale / 区間幅超過)。拒否を消すと落ちる
- oracle: 球 1 個・三角形 1 個・ray 1 本の最小 scene で `t` の区間と GPU の `t` の包含。区間の padding を消すと落ちる(f32 の GPU 値が double 区間から外れる fixture を 1 つ含める)
- 混在: 球が三角形の手前 / 奥の 2 fixture で最近接の record ID と `hitKind` が入れ替わる
- 数値契約は RT3-2 WP の表で確定し、**変えるときは本節の定数を上げてから**(定数は `raytracingoracle.hpp` に `Contract` として実装される。設計と実装で同じ名前)

#### 4-P.6 設計レビュー第 1 巡(6 件・破綻 3)と v2 —— **v2 が 4-P の正。4-P.1〜4-P.3 と矛盾する箇所は v2 が勝つ**

##### 修正 1(#1): transform は **rigid のみ**。交差は world 空間、GPU の `.rint` は `a = dot(d, d)` で書く

uniform scale は subset 外にする(球のまま保てても `t` の意味が object / world で変わる)。CPU oracle は三角形と同じく **球の center を world へ区間変換**し、正規化 world ray と交差する(`a = 1`)。
GPU の `.rint` は object ray(`gl_ObjectRayOriginEXT / DirectionEXT`)で交差するが、rigid なら方向は単位長のまま・`t` は不変。ただし shader は **`a = dot(d, d)` を明示**して単位長を仮定しない(規範。fixture の transform が rigid でも generator は `|det − 1| ≤ 1e-6` を検査する)。

##### 修正 2(#2): GPU の球判定は **geometric form** を規範にし、domain を f32 の相殺に対して縛る

`b² − 4c'` の直接計算は `origin = (−1024,0,0)`, `center = (1023,0,0)`, `r = 0.125` で f32 の判別式が 0 になる(実例)。規範:
- **GPU shader の式を固定**: `l = c − o`、`s = dot(l, d)`、`m² = dot(l, l) − s²`、`disc = r² − m²`、`t = s − sqrt(disc)`(`a = dot(d,d)` で割る)。`b² − 4ac` の形は subset 外
- **domain**: `|l| = |c − o| ≤ 2^9`(origin から center までの距離。座標 domain ±2^10 の中で更に縛る)、`r ≥ 2^-3`、かつ **垂直 margin** `r² − m² ≥ disc_min = 2^-8 · r²`(接線からの距離が半径の 1/16 以上)。`disc_min` は radius² 相対に加えて **`m²` の f32 誤差上限 `4 · 2^-24 · |l|²` を超える**ことも要求する(`2^-8 r² > 2^-22 |l|²` ⇔ `|l| < 2^7 · r`。generator が両方を検査)
- CPU oracle は同じ geometric form を double の区間で計算し、`sqrt` の両端を有向丸め、端点に f32 の 4 ulp padding(三角形と同じ)。**generator は f32 で geometric form を emulate し、その値が CPU 区間に入らない fixture を拒否**(構成的保証の補助。検査に頼らないという §4 の原則には「拒否した fixture は golden にしない」ことで従う)

##### 修正 3(#3): slab は方向符号に依存しない形

各軸で `t_a = (min_i − o_i) / d_i`、`t_b = (max_i − o_i) / d_i` を区間で計算し、`near_i = min(t_a, t_b)`、`far_i = max(t_a, t_b)`(区間の min / max は端点ごと)。`|d_i| < 2^-12` の軸は「無限 slab」として除外し、`o_i ∈ (min_i, max_i)` を区間で要求。`t_enter = max_i near_i`、`t_exit = min_i far_i`、`t_enter.upper < t_exit.lower`。受け入れは **8 象限すべての方向**の ray を fixture に含める。

##### 修正 4(#4): 一意性は **区間同士**で、any-hit の後にもう一度

`nearest.t.upper + ε_t < next.t.lower`(scalar の差ではなく区間の分離)。any-hit 述語で候補が落ちた後の **accepted candidates** についても同じ分離を要求する(述語が最近接を落として次候補が最近接になる fixture は、その次候補と第 3 候補の分離も generator が保証する)。

##### 修正 5(#5): `ε_t` の式を現行 oracle に合わせる

`ε_t(t) = relative_t_epsilon · max(1, |t|)`(現行 oracle の適用式。[raytracingoracle.cpp](../src/core/shader/raytracingoracle.cpp) の一意性検査と同じ)。区間幅の上限も同じ分母 `max(1, |t_mid|)` で `≤ maximum_relative_interval_width`。純 relative(`t = 1e-3` で `1e-6`)は採らない。

##### 修正 6(#6): 「AABB 内包含」を 2 つの検査に分ける

Vulkan は `reportIntersectionEXT` の `t` が AABB slab 内であることを要求しない(current ray interval だけ)。したがって:
(a) **幾何が量子化後の AABB に含まれる**: generator が `center ± (r + ε_pad)` を **f32 に丸めた AABB**(BLAS に渡す実 bounds)に含まれることを検査 / (b) **GPU の `t` が CPU の root 区間に含まれる**(4-P.3 の包含述語)。「hit が box の中」は受け入れから外す。

##### 受け入れの骨(v2 差分)

- 8 象限の ray / 負方向からの AABB hit / `|l|` 上限と `|l| < 2^7 r` の generator 拒否 / f32 emulate の拒否 / 区間分離の一意性 / any-hit 後の再検査 / rigid でない transform の拒否 / (a)(b) の分離
- 定数の追加: `sphere_maximum_center_distance = 512`、`sphere_minimum_radius = 0.125`、`sphere_discriminant_margin = 2^-8`、`slab_direction_epsilon = 2^-12`。`Contract` に同名で実装する

#### 4-P.7 設計レビュー第 2 巡(8 件・破綻 1)と v3 —— **v3 が 4-P の最終。設計レビューは 2 巡で打ち切り、残余は RT3-2 の仕様レビューへ**

##### 修正 7(#1): off 対照は **report しない valid な `.rint`**

`intersectionShader = unused` は procedural hit group では VUID 違反で作れない。off / on は **同じ AABB・SBT・AS** のまま、on は規範 `.rint`(`reportIntersectionEXT`)、off は **同じ interface で report せずに return する valid な `.rint`** を同一テストで走らせる。
最終 SPIR-V と resolved group が実際に切り替わったこと(§11 決定 3 の load 時検査と同じ経路で、`.rint` の entry point digest を読む)も同じテストで検査する。

##### 修正 8(#2 #4): generator の補助検査は **本番の包含述語そのもの**。演算列は SPIR-V で固定し `NoContraction` を要求

- generator の f32 emulate は scalar 包含ではなく、**`rayTracingOracleContainsGpuHit` と同じ述語**(GPU 値を前後 4 ulp に展開した区間が CPU 区間に含まれる)で判定する
- 規範の演算列を **scalar SPIR-V の命令順**で固定する。修正10の一般形を正とし、`l = c − o` の3減算 → `s = l·d` の3乗算+2加算 → `ll = l·l` の3乗算+2加算 → `a = d·d` の3乗算+2加算 → `ss = s*s` → `lla = ll*a` → `m² = lla − ss` → `rr = r*r` → `rra = rr*a` → `disc = rra − m²` → `sqrt(disc)` → `numerator = s − sqrt` → `t = numerator/a` の**27演算**とする。基本浮動小数点演算に **`NoContraction` decoration** を要求する(`GLSL.std.450 Sqrt`は対象外)。load時に最終SPIR-Vの命令順・operand依存・decorationを検査する。検査に通らないSPIR-Vは **`procedural_oracle_sequence_mismatch@1`** でgolden対象外(名前付き分類。通常loadのエラーではない)。旧記述の `m² = ll − s*s` と `disc = r*r − m²` は撤回。
- generator は **逐次 f32** と **FMA 縮約 f32** の両方で emulate し、**両方**が包含述語を満たす fixture だけを golden にする(device の `precise` 実装差を吸収する保険)

##### 修正 9(#3): 誤差上限は演算列ごとの γ_n で導く

以下は修正10以前の単位長を仮定した履歴式であり、一般形のgolden受け入れにそのまま使用しない。WP386cで `a` を含む27演算とsqrt/divの精度上限から導き直して確定する。

`γ_n = n·u / (1 − n·u)`(`u = 2^-24`)。上の演算列で `m²` の絶対誤差上限は `γ_12 · (|l|² + s²) + γ_2 · |l|²`、`disc` はそれに `γ_1 · r²` を足したもの。**disc_min はこの上限の 8 倍以上**(`r² − m² ≥ 8 · (γ_12 (|l|² + s²) + γ_2 |l|² + γ_1 r²)`)かつ従来の `≥ 2^-8 r²` の両方を要求する。v2 の「`4·2^-24·|l|²`」は撤回(反例あり: 比 1.81)。定数は `Contract::sphere_gamma_terms = {12, 2, 1}` として実装し、式を設計と実装で共有する。

##### 修正 10(#5): CPU も GPU も **一般形**(`a = dot(d, d)`)。単位長の仮定を捨てる

CPU oracle は `a` を区間で持ち、`t = (s − sqrt(disc·a … ))` の一般形(`disc = r² a − (|l|² a − s²)`、`t = (s − sqrt(disc)) / a`)で計算する。GPU も同じ一般形。v1 の `a = 1` と v2 の `1e-6` 許容は撤回。generator は方向を正規化するが、**正規化の誤差は一般形が吸収する**ので単位長の検査は要らない。

##### 修正 11(#6): rigid の検査は `RᵀR ≈ I` と `det ≈ +1`

generator は object→world の回転部 `R` について **`|RᵀR − I|_∞ ≤ 2^-20`** と **`det R ∈ [1 − 2^-20, 1 + 2^-20]`**(向きを保つ rigid のみ。reflection は subset 外)を検査する。shear / scale は `procedural_transform_not_rigid` で名前付き拒否。`|det − 1|` だけの検査は撤回(shear の反例あり)。

##### 修正 12(#7): slab は **object 空間**で、量子化後の AABB に対して

- object ray = world ray の逆 rigid 変換(`o_obj = Rᵀ(o − T)`、`d_obj = Rᵀ d`)を **区間で**計算する(`R` の各成分は f32 に量子化した値の double 区間)
- AABB は **BLAS に渡す f32 の object bounds**(`center ± (r + ε_pad)` を f32 に丸めたもの)
- `2^-12` の「無限 slab」判定と `o_i ∈ (min_i, max_i)` は object ray に適用。`t_enter / t_exit` は object 空間の `t`(rigid なので world と同値)
- 修正 6 の (a)「幾何が量子化後 AABB に含まれる」も object 空間で検査する

##### 修正 13(#8): 候補列挙 API は predicate を受け取る 1 本。procedural の `primitive_id` は AABB index

- oracle の候補列挙を **predicate 付きの 1 本**(`enumerateAcceptedCandidates(scene, ray, predicate)`)にし、validation(一意性)と最終選択が同じ列を使う。最近接 accepted の `t.upper + ε_t` を **他の全 accepted の `t.lower` の最小値**と比較する
- 受け入れ fixture: A(t=8)/ B(t=16)/ C(t=16.005)で、always-accept は通過、A だけ ignore の predicate では B / C の分離不足で **名前付き拒否**(`oracle_uniqueness_after_predicate`)。同一テストで両方
- procedural の `primitive_id` = **その geometry の AABB buffer 内の index**(`gl_PrimitiveID` と同じ)。1 geometry に複数 AABB を許す(1 AABB = 1 球)。any-hit 述語の整数入力に `primitive_id` をそのまま使う

##### 定数(v3 で確定。`Contract` に同名)

`sphere_maximum_center_distance = 512` / `sphere_minimum_radius = 0.125` / `sphere_discriminant_margin_relative = 2^-8` / `sphere_gamma_terms = {12, 2, 1}` / `sphere_discriminant_margin_factor = 8` /
`slab_direction_epsilon = 2^-12` / `rigid_orthogonality_epsilon = 2^-20` / `aabb_pad = 2^-7`(v1 の `1.0e-2` を dyadic に変更)/ `aabb_minimum_thickness = 2^-7`。

##### 受け入れの骨(v3 差分。RT3-2 WP へ引き継ぐ)

- off / on: report しない valid `.rint` との対照 + SPIR-V の entry digest 切替の観測
- SPIR-V 検査: 縮約した SPIR-V(`-ffast-math` 相当で compile した fixture)が `procedural_oracle_sequence_mismatch@1` で golden 対象外に分類され、規範列は通る
- generator: 逐次 / FMA 両 emulate の包含、γ 上限、`RᵀR`、object 空間 slab、A/B/C の predicate 後一意性。各拒否を 1 つずつ消すと落ちる
- 数値契約の変更は本節の定数を上げてから。RT3-2 WP の仕様レビューは本節を前提として攻撃してよい(設計レビューは打ち切り)
#### 4-P.8 RT3-2 の実装段(WP386 仕様レビュー第 1 巡の反映。規範は変えない)

- 1 geometry 複数 AABB(修正 13)は規範のまま、実装は **RT3-2a(1 asset = 1 AABB。`gl_PrimitiveID` は常に 0)→ RT3-2b(`gl_PrimitiveID` で引く primitive table)** に分ける
- golden の前提は **Vulkan のfloat controls(f32 RTEとsigned zero / inf / nanのpreserve) + 精度表**。満たさないdeviceは `procedural_oracle_device_unsupported@1`。2026-09-05の対応条件見直しでDenormPreserve必須を撤回し、演算入力・結果のsubnormal保持/FTZ双方をCPU区間に含める。曖昧な分岐はfixture対象外。`sqrt` / `div`には精度表の誤差を伝播し、単位長を仮定した旧γ式は使わない。
- hit attribute の照合対象は `HitAttributeKHR` 変数の存在と canonical type(`reportIntersectionEXT` の第 2 引数は hitKind)
- geometry kind → group kind は layout レベルの契約(pipeline ごとの宣言ではない)。procedural row は material join を通らず、triangle / procedural は renderer の row resolve 前に 1 つの typed union snapshot に確定する
- 第2巡のidentityとgeometry単位は維持。float modeは上記更新後の `RTE + SignedZeroInfNanPreserve` を `GL_EXT_spirv_intrinsics` で生成し、最終SPIR-VをInspectorが検査する。`sqrt`は `1/inversesqrt` の継承式による誤差を厳密値の両側へ対称に広げる。paddingはdevice非依存の規格端点値で検証する。出典: [Vulkan SPIR-V精度表](https://github.khronos.org/Vulkan-Site/spec/latest/appendices/spirvenv.html)。

## 5. capability・構成(v2 §4 + 第 2 巡 #8)

- 選択済み feature × 非対応 device = hard reject(property 名・要求値・実値・device 名)
- runtime facts 追加: maxRayRecursionDepth / maxShaderGroupStride /
  maxRayHitAttributeSize / **maxRayDispatchInvocationCount + 各次元上限**
  (積は overflow-safe に検査、load と dispatch 解決の両方)
- web は compute_tasks 明示拒否のまま。4 project × off/on、compiler ON/OFF、
  Studio ON/OFF の matrix は全段共通

## 6. 二車線の去就(第 2 巡 #7 で限定)

byte 一致契約は **「engine-authored material-alpha profile」に限定**して維持する:
RT2a/RT2 の alpha 判定は両車線に**同じ生成 predicate を注入**する portable profile
とし、その範囲で一致テストを置く。**自由 `.rahit` は pipeline 車線専用**と明示
(ray query 側に等価物は要求しない)。

### 6-1. 改訂(2026-08-31、WP369 仕様レビューによる)

**両車線の byte 一致契約を退役させる。**「同じ生成 predicate を両車線へ注入する
portable profile」は、profile sidecar の fullscreen 輸送・graphics pipeline への
set 3・SBT からの table 所有権分離という **engine のサブシステム**を要し、
機能 WP には収まらないことが実証された(WP369 v1 レビュー #3・#4・#5)。

**改訂後の契約**: 二車線は **どちらも alpha を使わない範囲でのみ**一致する。
profile が active な scene では一致を主張しない。
**RT2b-2(WP369)は pipeline 車線のみ。**利用者は RT2b-1 の material 表と
accessor を使って**自分の `.rahit` で ray query 相当のことを書ける**ので、
表現力は失われない(失われるのは engine 側の自動一致だけ)。
ray query 車線の alpha 対応が必要と分かった時点で、**設計から**始める。

`material_alpha_v1` は layout 単独では shader を有効化しない。次のように
top-level field を持つ layout と、それを使う typed ray-tracing task の両方が必要である。
`ray_layout.json` の完全例:

```json
{
  "schema": "pelican.ray_group_layout",
  "version": 1,
  "portable_profile": "material_alpha_v1",
  "ray_types": ["primary"],
  "default_group": "opaque",
  "groups": [{"name": "opaque"}],
  "misses": [{"name": "sky"}],
  "engine_record": ["material_index", "geometry_index"],
  "record": []
}
```

対応する rendering config の完全例:

```json
{
  "ray_group_layout": "ray_layout.json",
  "features": [],
  "render_targets": [
    {
      "name": "alpha_result",
      "extent_scale": 1.0,
      "format": "R32G32B32A32_UINT",
      "format_class": "data",
      "role": "data",
      "usage": ["STORAGE", "TRANSFER_SRC"]
    }
  ],
  "rendering_passes": [
    {
      "name": "main_render",
      "passes": [
        {
          "name": "scene",
          "type": "material",
          "output": {"color": ["swapchain"], "depth": null}
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "alpha_trace",
      "ray_tracing": {
        "raygen": "project://shaders/alpha_trace",
        "traced_ray_types": ["primary"],
        "misses": {"sky": "project://shaders/alpha_miss"},
        "hit_shaders": {
          "primary": {
            "opaque": {
              "type": "triangles",
              "closesthit": "project://shaders/alpha_hit"
            }
          }
        }
      },
      "writes": ["alpha_result"],
      "resource_ports": {
        "alpha_result": {
          "resource": "alpha_result",
          "access": "storage"
        }
      },
      "dispatch": {"rays_from": {"port": "alpha_result"}}
    }
  ],
  "target_planning": {
    "graphs": {
      "main_render": {
        "required_capabilities": [
          "pelican.vulkan.ray_tracing_pipeline@1"
        ]
      }
    }
  }
}
```

この raygen の各 `traceRayEXT` は定数かつ `Opaque` を含まない ray flags を使う。
`hit_shaders` に authored `anyhit` を加えると profile との同居拒否になる。

## 7. RT interface 検証(第 2 巡 #5)

reflection に storage class・構造 fingerprint・size・stage・entry point・group を
保持し、次を load 時 named エラーに:
- stage 間の payload / callable data の構造一致(location だけでなく全体)
- **絶対規則**: triangles hit group の hit attribute は `vec2`(f32×2)固定
  (intersection の無い group に独自 hit attribute を許さない)/
  procedural group は intersection の書く attribute と一致 + `maxRayHitAttributeSize`
- incoming interface の個数制約(種類ごとに 1 変数)
- **callable data の検証は RT4(callable 解禁)と同段**(RT1a は payload / hit attribute まで)

## 8. 段(第 2 巡の分割線を採用)

### RT1a: interface 安全と再帰(語彙・SBT 不変)

現行 `.rgen/.rmiss/.rchit` のまま: §7 の reflection 拡張と絶対規則 /
`max_recursion`(facts 比較)/ §5 の facts・dispatch 上限 / §4 オラクルの
基盤(subset 規範表・CPU 参照器・hit/miss/再帰の sentinel は既存 0-offset 経路で
可能な範囲)。**単独価値**: 既存 RT feature の誤 shader が load 時に名指しで
止まり、再帰が書けるようになる。

### RT1b: SBT addressing と record ABI

§3 の全部: `ray_group_layout` + 3 domain + 生成 include + 静的 record ABI
(型集合 = leaf の scalar/vec、data 開始 offset = handle サイズを
`shaderGroupHandleAlignment` へ切上げ、member は宣言順に std430 相当規則で配置、
padding は 0 初期化、表は正準化 JSON に出力)+ **production TLAS が record base
`q_base×R` を書く**(共同 transaction 込み)+ **二 pipeline × 二 instance ×
非ゼロ base の sentinel fixture**(第 2 巡 #3)+ 旧 flat 構文の byte 保存。
**単独価値**: 既存 stage のまま、複数 group と record 定数が使える。

### RT2a: any-hit 縦切り

`.rahit` を全所(wire 正本 → core → PlanViewer/Studio)へ追加(**語彙は使う段で
入れる**。`.rint` は RT3、`.rcall` は RT4 —— 「存在するが必ず拒否」の語彙は
出荷しない、第 2 巡 分割案)/ typed triangle group `{type:"triangles", anyhit}` /
portable alpha profile(§6)を両車線へ / subset 内 any-hit の oracle。

### RT2b: material ABI

v2 §3-2 の全部(bindless material table・geometry access・
`pelican.ray_bindings@1` overlay・`data(q)` への material/geometry index 充填・
WP354 drill-through の投影)。

### RT4 callable の実装契約 (2026-09-05)

`ray_tracing.callable` は省略可能なshader参照文字列または非空配列。layout有無の両方で使え、配列の宣言順が `executeCallableEXT` のSBT indexになる。拡張子は `.rcall`。callableは独立したgeneral groupとしてhit領域の後ろに配置し、pipeline世代と同じ寿命のSBT bufferを共有する。

callable dataは既存の構造型比較を使用する。定数indexは対象のincoming dataと比較し、動的indexはtable全体の型が一致する場合に受理する(実行時indexの範囲はshader側の責任)。callable内の呼び出しとtraceも同じinterface検査を通す。shader単独のreloadでpipelineとSBTを更新する。

callable独自データは `callable_record` にmember配列、`callable_records` にcallable順の初期値を宣言する。型は既存hit recordと同じi32/u32/f32/vec2/vec3/vec4、配置はstd430。shaderRecordEXTのmember型・offsetをこのschemaと照合する。schemaなしのデータ宣言は拒否する。

```json
"callable": ["project://shaders/shade"],
"callable_record": [{"name": "gain", "type": "f32"}],
"callable_records": [{"gain": 1.0}]
```

実行時はrender threadから `ComputeTaskContainer::setCallableRecord(task_id, index, values)` で1recordを更新する。型不正なら変更せず、同値ならGPU bufferも維持する。更新後のdispatchで新しいSBTを作り、旧SBTはそれを使うGPU submissionのleaseで完了まで保持する。固定のframe数に依存せず、未変更なら同じimmutable SBTを再利用する。shader reloadでpipeline世代が変わった場合は、新しいhandleでrecordを再配置する。record更新自体はpipeline再作成を伴わない。

ゲーム側は `GameContext::setRayTracingCallableRecord(task_name, index, values)` を使用する。`BehaviorContext` からも継承した同じAPIを呼べる。内部task handleやVulkan型への依存は不要。ゲーム更新threadから呼び、存在しないtask・範囲外index・不正な値は例外で通知し、前のrecordを維持する。シーンactivate/destroyのtransaction内ではlive taskを更新せず `SceneTransactionMutationRejected` を返す。初期値は宣言の `callable_records` に置く。

```cpp
context.setRayTracingCallableRecord("trace_lighting", 0, {{"gain", 0.8f}});
```

StudioのRender Features内「Callable Records...」で、taskとcallableを選び、schemaに沿って
初期値を編集する。Applyでproject rootの`ray_task_callable_records`を保存する。
このobjectはvariant suffix付与前のtask名から、callable順のrecord配列への上書き指定である。
schemaとshader数は元宣言のまま、共有featureのbytesは変更しない。Reset taskは元の宣言値へ戻し、
Applyで上書き指定を除く。ゲームコードの実行時更新はここに表示・保存しない。
`get_ray_callable_records`はsource digestとtaskごとのschema・callables配列・authored_records・recordsを返す。
`edit_ray_callable_records`はbase_source_digestとtask_records objectを受け、既存の設定編集ticketで
再構築・公開・保存する。型や件数の不一致は、読み込みと同じencoderで拒否する。
学習サンプルの追加よりエンジン中核の機能完成を優先する。

### RT3〜RT7

v2 §6 の RT3(AS 宣言面: geometry_flags/instance_flags 分離・mask・
custom_index 未指定=0・**static AABB + `.rint`**)/ RT4(callable `.rcall` +
per-in-flight 動的 record)/ RT5(変形 geometry の毎フレーム rebuild/refit)/
RT6(dispatch 一般化・indirect・AS graph node 化・複数 TLAS・
複数 geometry BLAS の再訪)/ RT7(ノード一級化・編集 RPC・fork・CAS)。

依存: RT1a → RT1b → RT2a → RT2b → RT3 → {RT4, RT5} → RT6 → RT7。

### RT6: scene ASの実行ノード（2026-09-06）

現在のscene全体・固定のframe AS bindingに対応する構築処理は、frame graphの
`__scene_acceleration_structure`（kind: `acceleration_structure`）として実行する。
ray project、RT task、またはgraphのray query / ray tracing pipeline capabilityから
compilerが生成し、同じvariantの各graphへ挿入する。既存nodeはこのnodeに依存する。
変形compute、BLAS build/refit、TLAS、対応するSBTとframe publicationをまとめて扱う。
単なる表示用nodeではなく、rendererのこのnodeの実行箇所でcommandを記録する。

XRではview非依存の処理とし、同じ論理フレームの後続viewは同じpublicationを使う。
submission完了までのAS/SBT所有権と、失敗時のpublication取消しは従来のtransactionに載せる。
ray queryをshaderから使うgraphは`target_planning.graphs`の`required_capabilities`へ
`pelican.vulkan.ray_query@1`を宣言する。宣言もRT taskもray projectも無いまま
reflectionだけでASが必要になった場合、graph外で暗黙に構築せずロード時に明示拒否する。

compilerは共有構築nodeに続けて、以下の名前付きTLASごとに
`__scene_acceleration_structure:<name>`を生成する。共有nodeは変形・BLAS・default TLASを構築し、
名前付きnodeはそのBLASとinstance列を使って対象TLASだけを構築する。再利用可能な集合では
名前付きnodeはbuild commandを再発行しない。各nodeの実行traceで記録の有無を観測できる。
全consumerはAS集合の完成を待ち、最後のAS操作で一つのdescriptor bundleを公開する。
途中失敗では既存のframe transactionが公開を取り消し、記録済み資源はsubmissionまで保持する。
AS定義とtask接続は編集RPCから変更できるが、生成nodeの任意移動や独立した公開・
一般のgraph edge編集は未実装。BLAS geometry結合は以下のRT6節に記す。

### RT6: 名前付きTLASの選択（2026-09-06）

projectのrendering configに`scene_acceleration_structures`を宣言する。preset使用時も
project側が所有する。各行は`name`と`instance_mask`（省略時255、整数0〜255）を持つ。
名前の重複・空文字・予約名`scene`は拒否する。

```json
"scene_acceleration_structures": [
  {"name": "actors", "instance_mask": 1},
  {"name": "world", "instance_mask": 2}
]
```

RT taskは`ray_tracing.acceleration_structure`にこの名前を指定する。省略時の`scene`は
現在scene全体を指す。名前付きTLASは、instanceのmaskと宣言maskのbit ANDが非ゼロの
行を含む。mask 0は空のTLASを作る。選択後も元のinstance mask・custom index・flags・
SBT offsetを保持し、shaderのray cull maskは通常どおり適用される。

すべてのTLASは共通のBLASとgeometry/record/SBTを使用し、`q`を詰め直さない。
各TLASに独立したframe descriptorを用意し、taskのbind時に選ぶ。shaderのAS bindingは
変更せず、別taskの選択で使用中descriptorを書き換えない。ASとdescriptor群は一つの
論理フレームpublication・submission leaseで所有する。定義・instance mask変更時は
reuse条件を再評価し、対象集合を更新する。

compute taskは`ray_query: {"acceleration_structure":"actors"}`で同じ名前を選択できる。
この宣言はray query capabilityとAS構築nodeを有効にする。`scene`の明示指定も可能で、
宣言省略時の既存compute ray queryとfragment ray queryは`scene`を参照する。
`ray_query`は`ray_tracing`と併記できず、`acceleration_structure`以外のキーは受理しない。
graphでは共通のAS構築nodeが全TLASを用意する。個別ASへの編集・依存表示は後続であり、
複数sceneを同時にロードする機能や任意object predicateの選択まで完了したとは扱わない。

### RT6: 複数geometry BLAS（2026-09-06）

同じモデル個体・componentのtriangle行が連続qを持ち、world transformと
instance mask/flags/custom indexが一致するとき、1つのBLASへまとめる。
geometryごとのmaterial、record、geometry flagsは独立のまま保持する。
配列順はq順であり、shaderの`gl_GeometryIndexEXT`はこのBLAS内の位置を返す。
SBTの参照はVulkanの[instance offset + geometryIndex × stride + ray type](https://docs.vulkan.org/spec/latest/chapters/raytracing.html)
に従い、2番目以後のgeometryも元のqに対応するrecordを読む。

静的BLASはgeometry allocationとflagsの列をkeyとして共有・再利用する。
変形を含むBLASは初回buildをAllowUpdateで作り、以後はgeometry列全体の
source identity・順序・topology・flagsが一致するとき別allocationへrefitする。
前frameのASと入力bufferはsubmission完了まで保持する。

異なる姿勢・mask・個体設定は別TLAS instanceへ分割する。layout v1のcustom index=qも
同じ条件により分割され、既存の意味を変えない。AABBは現在singletonのまま。
deviceのmaxGeometryCountと総maxPrimitiveCountに達する前にgroupを分割する。
単独geometry自体がprimitive上限を超える場合は拒否する。
診断の`geometry_count`はBLAS内のgeometry数、`tlas_instance_count`は結合後の実instance数を示す。
異なるtransformのgeometryをまとめる操作、AABBの複数geometry化は残る。

### RT7: 名前付きASの編集（2026-09-06）

StudioのRender Features内「Scene AS...」でname/maskの追加・変更・削除を行う。
`get_scene_acceleration_structures`はrootのdefinitionsとsource digestを返し、
`edit_scene_acceleration_structures`はbase_source_digestとdefinitions配列を受け取る。
定義は既存compilerのschemaで検査し、rootの該当JSON値だけを置換する。
他の設定のbytesを保持し、既存のrender config ticket・candidate compile・複数file保存・
publication/rollbackを共用する。使用中ASを削除してtask参照が解決できなければ適用しない。

GUIは失敗時にローカル行を保持し、成功時にengineの値を再取得する。
未公開のticket名はfeature固有からrender-config-editへ共通化する。
taskの接続先は同じ画面で選び、definitionsと一度に保存する。
rootの`ray_task_acceleration_structures`はtask名からAS名へのobjectで、共有featureの
元設定を変更せず`ray_tracing.acceleration_structure`または`ray_query.acceleration_structure`を上書きする。
task名はvariant suffix付与前の宣言名。通常compute taskと未知taskへの指定は拒否する。
getの`task_connections[]`はname・acceleration_structure・authored_acceleration_structureを返す。
editの省略可能な`task_connections` objectはrootの接続指定全体を置換し、空objectで元宣言へ戻す。
ASごとの実行nodeはcompilerが生成する。
Studioの「Fork preset...」は選択中のpreset文書を新しいprojectファイルへコピーし、
rootの`pipeline.preset`を同じ保存transactionで切り替える。
`fork_render_pipeline`は`base_source_digest`、`base_preset_digest`、
`destination_reference`を受ける。元presetのdigestは`get_render_authoring_context`の
`pipeline_preset.source_digest`から取得する。受付時と保存時に元presetを照合する。
コピー対象はfeature合成前の文書で、rootのsettings・features・RT上書き指定は保持する。
コピー先は未作成の`project://`ファイルに限り、保存失敗時には新規ファイルとroot変更を戻す。
コピーしたpresetは通常のproject資産として残る。任意のnode移動・edge編集はまだ含まない。

project所有presetの既存fullscreen passは`edit_preset_pass`で`input`・`output`・`shader`を編集する。
対象は合成前presetのgraph/nameで解決し、その他のpass設定は保持する。
root bytesが変化しない場合もpresetの置換・compiler検査・publicationを実行する。
rootとpresetのdigestを照合し、保存失敗は既存複数文書transactionで復元する。
StudioはFrame Planで選択したnodeから既存fullscreenフォームへ接続する。
contextの`preset_declaration`をフォームの値に使い、合成後の値を元設定へ逆流させない。

compute/RT taskは`get_compute_task_authoring`で元宣言と保存先digestを取得し、
`edit_compute_task`で宣言全体を置換する。名前は維持し、shader/resource_ports/dispatch等は既存compilerで検査する。
provenanceからproject feature・project preset・直接記述rootを解決し、engine所有文書は読み取り専用。
rootとtask元ファイルのCAS、candidate document overlay、保存・GPU publication/rollbackは既存処理を共用する。
StudioのTasks詳細画面とFrame Plan選択から呼び出す。視覚的なport接続操作・node配置編集は別途残る。
`add_compute_task`は新規project featureにtaskを保存し、rootのfeatures参照を同じtransactionで追加する。
`remove_compute_task`は元文書の宣言だけを削除し、空になったfeatureも通常のproject assetとして残す。
追加・削除も合成とcompilerを通すため、名前重複や未解決依存を公開せず、既存task編集と保存経路を共有する。

## 9. エラー文言・受け入れゲート

v2 §7 に同じ + §5 の matrix + 「off/on は同一テスト内」+ sentinel 表。

## 10. 第 2 巡指摘(9 件)への対応表

| # | 指摘 | v3 |
|---|---|---|
| 1 | physical slot の二重加算 | §3 operand 固定(`I=q_base×R, S=R, O=r`)。`g×R+r` は非公開 |
| 2 | group slot に per-instance data が入らない | §3 の 3 domain(`q` 行の導入、全 SBT が `q` 配列共有) |
| 3 | RT1 の sentinel が production で到達不能 | RT1b に TLAS record base の production 配線 + 非ゼロ base fixture。geometryIndex 項は恒久制約で除外を明記 |
| 4 | subset が機械判定不能 | §4 規範表(構成的 margin・許可集合・包含述語・always-accept 対照) |
| 5 | stage 間一致だけでは絶対規則を通す | §7 絶対規則 + 構造 fingerprint + size 検査 |
| 6 | 旧 flat 互換と全充填 hard error の矛盾 | layout 不在 project は旧 lowering を byte 保存。自動移行なし |
| 7 | 自由 any-hit と二車線一致の矛盾 | §6 一致契約を portable profile に限定、自由 `.rahit` は pipeline 専用 |
| 8 | dispatch 上限の facts 不足 | §5 maxRayDispatchInvocationCount + 次元上限 + overflow-safe 積 |
| 9 | slot が ray type 追加で不安定 / 共同 transaction 不在 | §3 digest 世代スコープ + 共同再構築を RT1b へ前倒し |

残余(WP 仕様で固定するもの): record ABI の完全表(RT1b WP)/
生成 include の symbol 命名と衝突規則(RT1b WP)/ portable alpha profile の
生成方法(RT2a WP)。

## 11. 第 3 巡指摘(6 件)への裁定(v4 差分。v3 本文と矛盾したらこちらが優先)

1. **`q` の identity と世代**(採用): row 順は draw inventory 順でなく、
   **永続 scene identity の正準ソート**で決める。`{TLAS, 全 SBT の data 領域}` は
   単一 immutable row snapshot から candidate として共同構築し、原子的 publication、
   旧世代は submission lease で退役。**invalidation 要因を列挙**: layout digest 変更 /
   row 集合の変更(instance の追加・削除・group 束縛変更)/ record ABI 変更。
   受け入れ: 登録順を入れ替えた同一 scene で出力 bytes 一致 / add→remove 後の
   sentinel が旧 row を読まない
2. **RT1b に本番の `g(q)`・`data(q)` 供給者が無い**(採用):
   `pelican.ray_bindings@1` overlay の**最小形を RT2b から RT1b へ前倒し**する:
   component/material stable ref → group 名、および **authored 静的 record 定数**。
   RT2b はこれを material/geometry index と bindless 表で拡張する側に回る。
   これで RT1b の非ゼロ base sentinel は production 配線で書ける
3. **shader 内 trace operand は dispatch 時に検査できない**(採用):
   検査面を SPIR-V に移す。**load 時に最終 SPIR-V の全 `OpTraceRayKHR` operand
   (sbtRecordOffset / sbtRecordStride / missIndex)が定数であることを検証**し、
   動的 operand は名前付き拒否(直接 `.spv` 供給も同じ検証を通る)。
   定数性検証は RT1a、layout に対する範囲検証は RT1b
4. **オラクルの数値契約**(採用): 設計段で枠を固定し、数値は RT1a WP の表で確定:
   座標 domain ±2^10 / t ∈ [1e-3, 2^12] / triangle は最小角と最小面積の下限 /
   CPU は double + 有向丸めで `[t_lo, t_hi]` を誤差伝播式から構成 / GPU は f32。
   ε は相対 1e-3 級を初期値に、**「区間を広げれば常に通る」を防ぐため
   区間幅の上限も同じ表で固定**する(幅超過 fixture は生成器がリジェクト)
5. **compiler-OFF × 生成 include**(採用・正直化): matrix を修正する。
   **layout 不在 project は compiler ON/OFF 両対応のまま**(旧 bytes 保存)。
   **layout 使用 project は当面 runtime compiler ON を要求**し、OFF では
   named reject(黙って壊さない)。layout digest に結び付いた offline bake
   (焼出し `.spv` + reflection artifact + 欠落時 named error)は
   design_build_tiers の bake 完成と合流する別段(RT-bake)として明記
6. **RT2a が material ABI に先行**(採用): RT2a から material-alpha profile を
   除き、**RT2a = `.rahit` 語彙 + typed group + record 定数ベースの subset any-hit**
   に縮小。**両車線 portable alpha profile は RT2b(material ABI と同段)へ移動**。
   二車線 byte 一致契約の適用範囲も RT2b で定義する


## 11. RT3 設計: AS 宣言面(2026-09-02。実測から書く)

RT3 を WP として起票しようとして、**per-instance の RT 宣言経路が
エンジンに一つも無い**ことが分かった。宣言面を WP の中で発明するのは、
このセッションで 3 本続けて中止になった形そのものなので、設計として書く。

### 11-1. 実測(2026-09-02 起票時点 = RT3 以前の履歴。`blas_geometry_flags` と test override は WP374 / WP377 で置換・削除済み。現行は台帳 WP374 / WP377 が正)

| 項目 | 現状 | 場所 |
|---|---|---|
| `mask` | **`0xffu` 固定** | [accelerationstructure.cpp:903](../src/core/renderer/accelerationstructure.cpp#L903) |
| `instanceCustomIndex` | **`q` を直接入れている** | 同 :497 / :902 |
| instance flags | **テスト用 override だけが入力**。本番に宣言面が無い | 同 :894 |
| geometry flags | **同上**(`blas_geometry_flags`) | 同 :748 |
| profile 検査 | `ray_profile_opaque_as@1` で opaque を拒否(WP369) | 同 :333 |
| `.rint` | `group.intersectionShader = unused` 固定。手続き幾何は皆無 | [pipelinefactory.cpp:699](../src/core/shader/pipelinefactory.cpp#L699) |
| per-instance の RT 宣言 | **無い**。`alpha_override_mask` は material override からの派生であって author 面ではない | [polygoninstancecontainer.cpp:1069](../src/core/renderer/polygoninstancecontainer.cpp#L1069) |

TLAS 構築は **1 箇所**で、pipeline 車線と ray query 車線が共用する。

### 11-2. 先に閉じるべき欠陥: **`q` が shader に漏れている**

§3 は「**raygen へ公開するのは `ray_type_index` / `ray_type_count` /
独立 `miss_index` のみ。`g×R+r` や `q` を shader に公開しない**」と規定している。
しかし `instanceCustomIndex = q` は、**`gl_InstanceCustomIndexEXT` で `q` を
すべての hit shader に公開している**。**現行実装は自分の規範に違反している。**

`material_index` は shader record(`shaderRecordEXT`)で運ばれており
custom index に依存していないので、**custom index は解放できる**。

**決定 1: `instanceCustomIndex` の既定は 0。`q` は入れない。**
利用者が宣言した値だけが入る。既存の「custom index == q」を検査するテスト
(`headless_render_test.cpp` に 5 件)は**この規範に合わせて変更する** ——
規範違反を固定していたテストである。

### 11-3. 宣言の粒度(ここが設計の本体)

flags / mask / custom index は**粒度が違う**:

| 項目 | 自然な粒度 | 理由 |
|---|---|---|
| geometry flags | **geometry(BLAS 入力)** | opaque かどうかは形状と material の性質 |
| instance flags | **instance** | 同じ geometry を opaque な個体と非 opaque な個体で使い分ける |
| `mask` | **instance** | mask は個体を分類するためにある。全体で 1 つなら無意味 |
| `instanceCustomIndex` | **instance** | 利用者の per-instance データの席 |

**したがって layout 文書だけの宣言は成立しない。** mask を project 全体で 1 つに
するのは、mask を導入しないのと同じである(「半端を増やさない」)。

### 11-4. 候補と評価

**候補 A: scene object に RT ブロックを足す**
`{"ray": {"instance_flags": [...], "mask": 12, "custom_index": 7}}` を object へ。
- 利点: 粒度が正しい。既存の component codec 機構に乗る
- 欠点: **scene 形式の拡張**。web で開ける ⊆ pelican で開ける の原則により、
  形式拡張は常にエンジン先行だが、**Studio の編集面と save 経路にも波及**する
- 懸念: RT を使わない project の bytes を動かさないこと(既定は宣言なし)

**候補 B: material に紐づける**
material 表(WP368)が既に per-material・per-row である。
- 利点: 新しい author 面を作らない。RT2b の表をそのまま拡張
- 欠点: **mask と custom index は material の性質ではない**。
  同じ material の 2 個体を別 mask にできない。**mask の目的を満たさない**

**候補 C: layout に「規則」を書き、instance に解決する**
layout に `{"when": {"material": "foliage"}, "instance_flags": [...]}` のような
規則を並べ、TLAS 構築時に解決する。
- 利点: scene 形式を変えない。project 所有のまま
- 欠点: **述語の文法を発明することになる**。
  このセッションで「文法を発明する WP」は 3 本中止になっている

**評価: 候補 A。** 粒度が唯一正しく、既存の codec 機構に乗り、
「宣言データとして開く」という user-space 境界の原則にも合う。
候補 C の述語文法は、必要になったときに候補 A の上へ足せる。

### 11-5. 段割り

| 段 | 内容 | 単独で何が良くなるか |
|---|---|---|
| **RT3a** | **`q` 漏れの停止**(custom index 既定 0)+ layout の `acceleration_structure` 既定ブロック(geometry flags / instance flags / mask の**既定値**) | 規範違反が消える。テスト用 override が本番入力である状態が終わる |
| **RT3b** | **object の `ray` ブロック**(instance flags / mask / custom index の per-instance 宣言) | mask が意味を持つ。同じ geometry を opaque/非 opaque で使い分けられる |
| **RT3c** | **static AABB + `.rint`**(手続き幾何) | 三角形以外を書ける。DCC 級自由度の本体の一つ |

**RT3a は RT3b の前提**(既定値の置き場所が要る)。
**RT3c は独立**(AABB geometry と intersection shader は別の話)。

### 11-6. 受け入れの骨(各段の WP へ引き継ぐ)

- **RT3a**: 宣言のない project で `gl_InstanceCustomIndexEXT` が **0 を読む**
  (今日は `q`)。`q` が shader から観測できないことを GPU テストで示す。
  テスト用 override を消しても本番が動くこと
- **RT3b**: **同じ geometry・同じ material の 2 個体**を、
  片方だけ `mask` で除外して**GPU 出力が変わる**。宣言を外すと両方写る
- **RT3c**: AABB + `.rint` で球を描き、**CPU 参照(§4 の規範表)と包含一致**
- 全段: **layout を書いていない project の bytes 不変**(§3 の恒久規則)


### 11-7. 設計レビュー第 1 巡(10 件・破綻 3)と v2

**v2 が §11 の正。11-2 の規範文・11-4 の担体・11-5 の段割り・11-6 の受け入れは v2 で置換される。**

#### 決定 1(#1): 規範は「`q` を秘密にする」ではなく「**custom index の供給源を `q` にしない**」

11-2 の受け入れ「`q` が shader から観測できない」は**恒偽**だった ——
§3 は `data(q)` を **shader record に置く**と規定し、§4 は
**`(q,r)` ごとの一意 sentinel を shader が読み戻す**ことを要求している。
**私が §4 で自分に課した検査が、私が §11 で置いた受け入れを否定していた。**

規範を書き直す:

> **`instanceCustomIndex` の供給源は `q` ではない。** 利用者が宣言した値だけが入る。
> `q` は shader record 経由でのみ shader に届く(§3 のとおり)。

受け入れも書き直す: **row を追加して `q` をずらしても、宣言済みの custom index は
同値のまま**であり、**SBT sentinel は独立に正しい**。同一 GPU テストで両方見る。

#### 決定 2(#2・破綻): layout 無しは **legacy ABI のまま**。既定 0 は layout 有りだけ

「宣言のない project は custom index 0」と「layout 無し project の bytes 不変」は
両立しない —— legacy branch は `result.size()` を custom index にしており
([accelerationstructure.cpp:451](../src/core/renderer/accelerationstructure.cpp#L451))、
それは**過去のレビューで byte 互換のために意図的に修復された仕様**である。

| 構成 | `instanceCustomIndex` |
|---|---|
| **layout 無し** | **legacy ABI(`0,1,…`)を維持。変更しない** |
| layout 有り・AS 宣言無し | **0** |
| layout 有り・AS 宣言有り | 宣言値 |

**「bytes 不変」の対象を固定する**: source file の bytes /
SBT の bytes / TLAS instance の bytes の**三つすべて**。
layout 無し project ではこの三つが不変であること。

#### 決定 3(#3): **RT3a と RT3b を 1 つの出荷単位にする**

RT3a 単独では表現力が**後退する** —— `gl_InstanceCustomIndexEXT` で instance を
区別している shader は、全 row が 0 になった時点で**エラーにならず出力だけ変わり**、
代替手段は RT3b まで来ない。既存 fixture も `custom_index == q` を固定している
([raytracing_rt1b_test.cpp:1026](../test/raytracing_rt1b_test.cpp#L1026) ほか、
headless に 5 件)。

**段割りを改める**:

| 段 | 内容 |
|---|---|
| **RT3-1** | AS 宣言面(geometry flags / instance flags / mask / custom index)を**担体ごと**入れる。custom index の供給源変更も**同時** |
| **RT3-2** | static AABB + `.rint`(手続き幾何)。**§4 の procedural subset 規範を先に書く**(決定 6) |

#### 決定 4(#5・破綻): 担体は **`components[]` の中の登録済み `ray_instance` component**

候補 A を「object の sibling」として置く案は**成立しない**:

- scene parser は object をコピーするが `name` / `parent` / `components` しか検査せず、
  未知の sibling は**保持するが意味付けしない**
  ([sceneformat.cpp:75](../src/project/sceneformat.cpp#L75))
- `ResolvedObject` に raw object field が無く、resolver は component だけを移送する
  ([resolvedscene.hpp:234](../src/core/loader/resolvedscene.hpp#L234))
- Studio の query/edit 面は `components[]` と `set_component_value` だけ
  ([editorcommandservice.cpp:627](../src/core/communication/editorcommandservice.cpp#L627))

つまり失敗の形は「disk から消える」ではなく **「保存はされるが編集不能で、
runtime では無効」** —— **最も悪い形**である。

**決定**: `components[]` の中の **登録済み `ray_instance` component** にする。
codec・public schema・runtime adapter・RPC・Studio の既存経路に**そのまま乗る**。
`BindableDescriptor` にも自然に載る(prefab パラメータで束縛できる)。

**subset 原則**(web で開ける ⊆ pelican で開ける)は、
**web が新 component を対応まで明示拒否**すれば満たされる。

#### 決定 5(#6): 粒度は **object 既定 + geometry override**。key に実行時 ID を使わない

**scene object と TLAS instance は 1:1 ではない** —— 1 object が 2 primitive を持てば
**2 row** になる(`q` は instance×geometry の row。
[polygoninstancecontainer.cpp:1061](../src/core/renderer/polygoninstancecontainer.cpp#L1061))。

- `ray_instance` component は **object 既定**を持つ
- **geometry override** を `{asset ref, mesh index, primitive index}` の
  **安定 key** で宣言する。**`geometry_allocation_id` のような実行時 ID を
  author key にしない**(決定性を壊す)
- **geometry flags は geometry override 側**(粒度が違う)
- 受け入れ: **1 object × 2 primitive を別設定にして GPU 出力が変わる**

#### 決定 6(#4・#7): 先に書くべき規範が 2 つある

**(a) BLAS の解決規則**(#7)。BLAS は `geometry_allocation_id` **だけ**を key に共有され、
reuse 判定は geometry pool / model generation とテスト override しか見ない
([accelerationstructure.cpp:744](../src/core/renderer/accelerationstructure.cpp#L744))。
**flags だけを変えた reload は古い BLAS を再利用し、宣言が無効になる。**

規範化する:

- **BLAS cache key に解決済み geometry flags を含める**
- 同一 geometry に**衝突する要求**が来たら、**`(geometry, flags)` ごとの BLAS variant**
  を作る(**利用者の要求は「速度を重視しない DCC 級の自由度」**なので、
  variant 増加は許容する。named reject や保守的統合で自由度を削らない)
- material routing からの自動導出と authored flag の**優先順位**を明記する
- `material_alpha_v1` と自由 any-hit・opaque flag の同居規則
  (profile active なら opaque は既に `ray_profile_opaque_as@1` で拒否される)

**(b) §4 の procedural subset 規範**(#4)。§4 の入力 domain は**非退化三角形だけ**で、
CPU oracle の入力型も `RayTracingOracleTriangle` しかない
([raytracingoracle.hpp:51](../src/core/shader/raytracingoracle.hpp#L51))。
**RT3-2 の前に**、有限 AABB / `min <= max` / ray-sphere の root 選択 /
`t` の一意性 margin / AABB 内包含 / hit attribute / 区間幅上限を規範表に足す。
足さないなら **procedural は §4 subset 外**と明記し、CPU golden を受け入れから外す。

#### 決定 7(#8): 受け入れを **no-op 実装で落ちる**形にする

第 1 巡の受け入れは no-op でも通った。書き直す:

- **custom index**: 宣言した値が **GPU の `gl_InstanceCustomIndexEXT` に出る**。
  **row を追加して `q` をずらしても宣言値は不変**。宣言を消すと 0 になる
- **mask**: 同じ geometry・同じ material の **2 個体**で、片方だけ mask で除外すると
  **GPU 出力が変わる**。宣言を外すと両方写る
- **instance flags**: `ForceOpaque` を宣言した個体だけ any-hit が**呼ばれない**
  (呼び出し回数を数える)。profile active なら `ray_profile_opaque_as@1` で拒否される
- **geometry flags**: 同一 allocation を opaque/mask で共有する構成で、
  **flags ごとに別 BLAS が作られる**(build 数で観測)。
  **flags だけの reload で BLAS が作り直される**(cache key に入っていないと落ちる)
- **担体**: `ray_instance` を **Studio から編集 → save → 再起動 → 同じ実効値**。
  **古い engine が名前付きで拒否**する
- **粒度**: 1 object × 2 primitive を別設定にして GPU 出力が変わる
- **legacy**: layout 無し project の **source / SBT / TLAS instance の bytes が三つとも不変**
- 全数 2 回 / GPU は統合側で直列 / `SKIP_DEVSTUDIO` 両構成

#### 決定 8: 段の順序(改)

```
§4 procedural subset 規範(文書のみ)
        ↓
RT3-1(宣言面 + 担体 + custom index 供給源 + BLAS cache key)
        ↓
RT3-2(static AABB + .rint)
        ↓
RT4 / RT5 → RT6 → RT7
```


### 11-8. 設計レビュー第 2 巡(8 件・破綻 3)と v3 —— 担体を **material 鍵の layout 規則**に変える

**v3 が §11 の正。v2 の決定 2・4・5 は v3 で置換される。決定 1・6・7 は維持。**

#### 第 2 巡が確定させたこと: **per-instance 担体は今日のエンジンに無い**

v2 の「`ray_instance` を登録すれば既存経路にそのまま乗る」は**誤り**だった:

| 環 | 実測 |
|---|---|
| runtime kind | **4 種だけ**(ECS / Camera / Light / Collider)([componentcodec.hpp:19](../src/core/loader/componentcodec.hpp#L19)) |
| schema | scalar / vec / string / bool / enum の**固定 leaf のみ。可変長 collection が無い**([structfieldschema.hpp:21](../src/core/userpublic/details/schema/structfieldschema.hpp#L21)) |
| RPC edit | `set_component_value` は**既存 JSON pointer を `.at()` で要求**。欠落 field も override 行も**作れない**([editorjournal.cpp:1057](../src/core/communication/editorjournal.cpp#L1057)) |
| Studio | 固定 schema field のみ。**`add_component` / `remove_component` を呼ぶ経路が無い**([inspectormodel.cpp:558](../src/devstudio/model/inspectormodel.cpp#L558)) |
| runtime AS | live edit routing が component 名を hard-code。AS snapshot に **authoring object ID が無い**([accelerationstructure.hpp:27](../src/core/vkcore/accelerationstructure.hpp#L27)) |

さらに **`{asset ref, mesh index, primitive index}` は row に単射でない**:
**1 つの glTF mesh を 2 つの node が参照**すれば同じ key が複数 row に当たり
([gltf.cpp:2261](../src/core/model/gltf.cpp#L2261))、auto-split の variant も同じ key を持つ。
実 row は `node_index` まで保持している
([polygoninstancecontainer.cpp:1169](../src/core/renderer/polygoninstancecontainer.cpp#L1169))。

**つまり v2 は「宣言面を作る」WP ではなく、
「schema 体系に可変長宣言を足し、Studio に追加編集面を作り、
新しい runtime kind と authoring→row の join を作る」プログラムだった。**
エディタトラックで 3 本中止したのと同じ形である。

#### 決定 9(v2 決定 4・5 を置換): 担体は **layout の material 鍵規則**

第 1 巡で候補 C(規則)を「述語文法を発明することになる」と退けたが、
**発明ではなかった**。material 鍵は既に存在する:

- material 表は **per-material・per-row**(WP368)
- row は `material_index` を持ち、**shader record で運ばれている**
- material の identity は **WP368a の authored reference**(数値 handle・絶対パス禁止)
- `BindableDescriptor` には既に **`discriminant`** という述語機構がある

```json
{ "schema": "pelican.ray_group_layout", "version": 2,
  "ray_types": ["primary", "shadow"],
  "groups": [ {"name": "opaque"}, {"name": "foliage"} ],
  "acceleration_structure": {
    "defaults": { "mask": 255, "instance_flags": [], "geometry_flags": [] },
    "rules": [
      { "material": "materials/foliage.json",
        "geometry_flags": [], "instance_flags": [], "mask": 2,
        "custom_index": 7 }
    ] } }
```

- **鍵は authored material reference ただ 1 つ**。述語を増やさない
- 規則は **上から順に最初の一致**(決定的。順序が規範)
- 一致しない row は `defaults`
- **同じ material の 2 個体を区別できない**のは**承知の上の制限**である。
  per-instance の区別が要ると分かったときに、**担体プログラム**を別途起こす

**なぜこれで十分か**: DCC の運用で mask と flags は
**「foliage は影を落とさない」「この material は非 opaque」**のように
**material 単位で決まることが多い**。per-instance の自由度は
**担体が無い今、どう書いても半端になる**。

#### 決定 10(#1・破綻): 状態は 4 つ。**layout version 2 で opt-in**

v2 の三分岐は **4 つ目の状態**(layout 無しなのに宣言がある)を持たず、
既存 layout v1 利用者(`gl_InstanceCustomIndexEXT == q` を読む shader)を
**無言で壊す**。

| layout | `acceleration_structure` | `instanceCustomIndex` |
|---|---|---|
| 無し | — | **legacy ABI(`0,1,…`)。変更しない** |
| **v1** | — | **`q` のまま。変更しない**(既存利用者を壊さない) |
| **v2** | 無し | **0** |
| **v2** | 有り | 規則の解決値(既定 0) |

- **`ray_group_layout` の version を 2 に上げることが opt-in である。**
  自動移行はしない(§3 の恒久規則と同じ形)
- **layout 無しで `acceleration_structure` を書いたら名前付き拒否**
  (`ray_as_declaration_requires_layout@1`)
- **v1 で `acceleration_structure` を書いたら名前付き拒否**
  (`ray_as_declaration_requires_layout_v2@1`)
- 既存の `custom_index == q` を固定するテスト(RT1b 1 件・headless 5 件)は
  **v1 のテストとして残る**。v2 の対照を新規に足す

#### 決定 11(v2 決定 6a を精密化): BLAS variant の join と purge

BLAS は `geometry_allocation_id` **だけ**を鍵に共有され、reuse 判定が flags を見ない。

- **BLAS cache 鍵 = `(geometry_allocation_id, 解決済み geometry_flags)`**
- variant は **AS-only**(`q` 採番・SBT・material 表に影響しない)。
  TLAS instance は自分の variant の BLAS address を指すだけ
- **purge**: variant を持つ geometry が消えたら**全 variant を落とす**。
  機能のパージ性を下げない
- **観測面**: `tlas_build_count` と同じ診断に **variant 数**を出す
- 受け入れ: **flags だけの reload で BLAS が作り直される**
  (cache 鍵に入っていないと落ちる)/ 同一 allocation の opaque/mask 混在で
  **build 数が 2 になる**

#### 決定 12(#7): 受け入れから充足不能なものを外す

- **「古い engine が名前付きで拒否」は充足不能**(古い engine を試験に持ち込めない)。
  **撤回**し、代わりに **version gate の否定対照**を置く:
  v1 に `acceleration_structure` を書くと `ray_as_declaration_requires_layout_v2@1`
- **geometry flags の no-op 排除**: 宣言を外すと **BLAS build 数が変わる**
- **担体の no-op 排除**: 規則の material 参照を存在しない物に変えると
  **名前付きで落ちる**(`ray_as_rule_material_unresolved@1`)。
  黙って defaults に落ちない

#### 決定 13: 段割り(最終)

| 段 | 内容 | 単独の価値 |
|---|---|---|
| **RT3-1** | layout v2 + `acceleration_structure`(defaults + material 鍵規則)+ custom index の供給源変更 + BLAS variant 鍵 | mask と flags が**初めて author できる**。今日はテスト用 override しか入力が無い |
| **RT3-2** | §4 の procedural subset 規範(文書)→ static AABB + `.rint` | 三角形以外が書ける |

**per-instance 担体は別プログラム**として台帳に残す(下記)。

#### 台帳へ残す: **per-instance 宣言の担体プログラム**

RT4(callable の per-in-flight record)・RT5(変形 geometry)・RT7(ノード編集)も
**同じ担体を必要とする**。要るものは:

1. schema 体系の**可変長宣言**(今は固定 leaf のみ)
2. **欠落 field と行を作れる編集 RPC**(今の `set_component_value` は `.at()`)
3. **Studio の component 追加・行編集 UI**(今は経路が無い)
4. 新しい **runtime kind と adapter**、AS snapshot への **authoring object ID**
5. **authoring → row の join**。key に `node_index` と importer variant を含める
   (`{asset, mesh, primitive}` は**単射でない**)
6. **予約 namespace か version gate**(外部 ECS の同名 component と衝突する)

これは R5(型付き構築 factory・codec 記述子の単一ソース化)と**同じ領域**である。


### 11-9. 設計レビュー第 3 巡と v4(最終) —— 担体は**分割できる**

**v4 が §11 の最終。v3 の決定 9 は v4 で置換される。決定 10〜13 は v4 の修正を受ける。**
**設計レビューは 3 巡で打ち切り、以降は WP の仕様レビューで受ける。**

#### 第 3 巡の裁定を受け入れる: **material 鍵では目的を果たさない**

第 3 巡 #5 は正しい。**v3 の retreat は目的を捨てていた**:

- 同じ木材 material の椅子 2 脚を、**片方だけ shadow ray から外せない**
- mirrored instance だけ `TriangleFlipFacing` を設定できない
- `custom_index` で object table を引けない(同 material の全個体が同値)
- 複数 material の 1 object を「除外」すると**一部だけ残る**

そして §11-3 で**私自身が**「mask は個体を分類するためにある。全体で 1 つなら無意味」と
書いていた。v3 はそれを自分で覆していた。

#### しかし担体は第 2・3 巡の見積もりより小さい(実測)

第 2 巡は担体を「schema に可変長宣言を足し、Studio に編集面を作り、
runtime kind と join を作るプログラム」と見た。**分けられる**:

| 必要とされたもの | 実測 |
|---|---|
| schema の**可変長宣言** | **per-instance の値には不要**。`mask` は `U8`、`custom_index` は `U32`、flags は enum —— **全部固定 leaf**([structfieldschema.hpp:21](../src/core/userpublic/details/schema/structfieldschema.hpp#L21))。可変長が要るのは **per-geometry override だけ** |
| **component を足す編集経路** | **core に既にある**。`add_component` / `remove_component` は journal の op として実装済み([editorjournal.cpp:1135](../src/core/communication/editorjournal.cpp#L1135)・:1621)。**欠けているのは Studio の UI だけ**であり、engine と RPC は塞がっていない |
| typed material selector | **AS snapshot に既にある** —— `material_document_reference` / `material_name` / `material_asset_reference` / `source_material_index`([accelerationstructure.hpp:32](../src/core/vkcore/accelerationstructure.hpp#L32)) |
| authoring object ID の運搬 | **これだけが新規配線**。snapshot は `instance_index` / `instance_generation` を持つが authoring object を持たない |

#### 決定 14(v3 決定 9 を置換): **粒度ごとに担体を分ける**

| 値 | 粒度 | 担体 | 段 |
|---|---|---|---|
| `mask` / `instance_flags` / `custom_index` | **instance** | **固定 leaf の `pelican_ray_instance` component**(`components[]` 内) | **RT3-1** |
| `geometry_flags` | **geometry** | **layout の material 鍵規則**(typed selector) | **RT3-1** |
| per-geometry × per-instance の override | row | **可変長宣言が要る。台帳の担体プログラム** | 先送り |

**優先順位を固定する**: `defaults < material rule < instance override`。

**Studio の UI は RT3-1 の範囲外**とし、台帳に残す ——
engine・RPC・project 形式は塞がっておらず、**Studio が追いつくのは常にエンジン先行**
(サブセット原則と同じ形)。

#### 決定 15(#1): material selector は **typed union**。単一文字列をやめる

```json
{"material": {"document": "materials/multi.json", "material_name": "Oak"}}
{"material": {"asset": "models/tree.glb", "source_material_index": 7}}
```

- 実 selector が既に `document` と `asset + source_material_index` の分岐である
  ([raygrouplayout.hpp:93](../src/core/shader/raygrouplayout.hpp#L93))。**それに合わせる**
- document material の canonical key は **material name まで含む**
  ([raygrouplayout.cpp:309](../src/core/shader/raygrouplayout.cpp#L309))
- **legacy direct load は authored reference が空になり得る**
  ([gltf.cpp:2433](../src/core/model/gltf.cpp#L2433))。
  その row に規則が当たったら **`ray_as_row_material_identity_unavailable@1`** で拒否する
  (黙って defaults に落とさない)
- **同一 exact key の重複規則は first-match にせず拒否**する
  (`ray_as_duplicate_rule@1`)。存在しない asset・範囲外 index も別コード

#### 決定 16(#2): semantic mode を **plan まで運ぶ**

`RayGroupLayout` は version を保持せず、parser は v1 固定、
TLAS lowering は layout version も AS mode も受け取らない
([accelerationstructure.cpp:443](../src/core/renderer/accelerationstructure.cpp#L443))。

- **`LegacyNoLayout` / `LayoutV1Q` / `LayoutV2` を型として保持**し、
  **immutable な row / TLAS plan まで運ぶ**
- **`ray_as_declaration_requires_layout@1` は削除**する ——
  `acceleration_structure` は layout 内の field なので、
  「layout 無しで書く」構文が存在せず**到達不能**だった
- **4 状態表を custom index だけでなく mask と両 flags まで広げる**
- **wire を固定**: `mask` は 8 bit、`custom_index` は **24 bit**、
  flag 語彙、未知 field の扱い、`defaults` の必須性

#### 決定 17(#3): variant の purge は **exact active set** で行う

- BLAS cache 鍵は `(geometry_allocation_id, 解決済み geometry_flags)`
- **purge は「この frame で実際に参照された variant 集合」との差分で行う** ——
  「geometry が消えたら全部落とす」だけでは、
  **規則の reload で参照されなくなった variant が残る**
- **SBT digest と AS digest を分離**する(variant は AS 側だけを動かす)

#### 決定 18(#4): 受け入れを全面改訂(no-op と充足不能を両方潰す)

**同一テスト内で off/on・実効値・GPU 効果・purge を見る**:

- **mask**: 同じ material の**個体 2 つ**で、片方の `pelican_ray_instance.mask` だけを変えると
  **GPU 出力が変わる**。component を消すと両方写る(**第 3 巡 #5 の椅子 2 脚**)
- **instance_flags**: 片方だけ `ForceOpaque` にすると **any-hit の呼び出し回数が減る**。
  profile active なら `ray_profile_opaque_as@1`
- **custom_index**: 宣言値が `gl_InstanceCustomIndexEXT` に出る。
  **row を追加して `q` をずらしても不変**。SBT sentinel は**独立に正しい**
- **geometry_flags**: 同一 allocation を opaque/mask で共有 → **BLAS build 数が 2**。
  **規則の reload で参照されなくなった variant が purge される**(残ると落ちる)
- **優先順位**: `defaults < material rule < instance override` を 3 段重ねで検査
- **version gate**: v1 に `acceleration_structure` を書くと
  `ray_as_declaration_requires_layout_v2@1`。**v1 の `custom_index == q` は不変**
- **legacy**: layout 無し project の **source / SBT / TLAS instance の bytes が三つとも不変**
- **拒否**: legacy provenance の row / 重複規則 / 未解決 material

#### 決定 19: 段割り(最終)

| 段 | 内容 |
|---|---|
| **RT3-1** | layout v2 + typed material 鍵の geometry flags + **固定 leaf `pelican_ray_instance` component**(mask / instance_flags / custom_index)+ custom index の供給源変更 + semantic mode の運搬 + BLAS variant 鍵と exact purge |
| **RT3-2** | §4 の procedural subset 規範(文書)→ static AABB + `.rint` |

#### 台帳へ残す(RT3-1 の範囲外)

- **per-geometry × per-instance の override**(可変長宣言が要る。担体プログラム)
- **Studio の `pelican_ray_instance` 追加・編集 UI**(core の `add_component` は既にある)
- **外部 ECS との名前衝突**に備えた予約 namespace か version gate
