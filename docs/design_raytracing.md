# レイトレーシング拡張設計 — DCC 級の自由度へ(v4)

状態: **v4。設計レビューはここで打ち切る**(1 巡: 14 件 / 2 巡: 9 件 / 3 巡: 6 件。
3 巡の代数・共有 q 配列・互換分岐・D0 は「壊せなかった項目」で生存確認済み)。
§11 が 3 巡への裁定。以降の残余は各 WP の仕様レビューで受ける。

## 0. 要求

利用者要求(2026-08-30): **速度性を重視しない DCC ツールと同程度に「好き勝手」できる
RT**。到達点は視覚的正しさと表現の自由度。リアルタイム性能は制約にしない。
恒常制約: パージ性を下げない / fail-fast / 名前付きエラー / 決定性 / D0。

## 1. 現状の固定点(2 巡で検算済み)

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
- **当面の恒久制約として「1 BLAS = 1 geometry」を明記**(現行どおり)。
  `geometryIndex` 項は常に 0 であり、複数 geometry BLAS の導入(するなら
  `q = q_base + geometryIndex`)は RT6 で再訪する。sentinel 検査の対象からも
  この項は「制約により恒等 0」と明示して除外する

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

### RT3〜RT7

v2 §6 の RT3(AS 宣言面: geometry_flags/instance_flags 分離・mask・
custom_index 未指定=0・**static AABB + `.rint`**)/ RT4(callable `.rcall` +
per-in-flight 動的 record)/ RT5(変形 geometry の毎フレーム rebuild/refit)/
RT6(dispatch 一般化・indirect・AS graph node 化・複数 TLAS・
複数 geometry BLAS の再訪)/ RT7(ノード一級化・編集 RPC・fork・CAS)。

依存: RT1a → RT1b → RT2a → RT2b → RT3 → {RT4, RT5} → RT6 → RT7。

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
