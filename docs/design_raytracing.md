# レイトレーシング拡張設計 — DCC 級の自由度へ(v2)

状態: v2。第 1 巡レビュー(Reject・構造欠陥 14 件)を全面反映。§9 に対応表。

## 0. 要求

利用者要求(2026-08-30): **速度性を重視しない DCC ツールと同程度に「好き勝手」できる
RT**。到達点は視覚的正しさと表現の自由度。リアルタイム性能は制約にしない
(遅い rebuild・毎フレーム再構築も目的上は許容)。
恒常制約: パージ性を下げない / fail-fast / 名前付きエラー / 決定性 / D0。

## 1. 現状の固定点(第 1 巡で検算済み。引用は検算後の正しい行)

- 2 車線: ray query(fullscreen)と RT pipeline(compute_tasks)。**両車線は
  byte 一致契約を持つ**(source-code-guide/11 §29)——拡張時はこの契約の去就を明示する
- stage: raygen/miss/closesthit のみ(computetask.cpp:1190)。SBT は handle-only
  (shaderbindingtable.cpp:72、コピーは pipelinefactory.cpp:682)。
  **再帰は pipeline 作成時に 1 固定**(pipelinefactory.cpp:625)
- TLAS: custom index = 描画列順、mask 0xff、SBT offset 0、flags 0
  (accelerationstructure.cpp:584)。BLAS は静的 geometry のみ(同 :199 で
  skinned/morph/VAT 除外)。**AS build は frame graph の外**(renderer.cpp:4862 →
  task 実行 :4926 の前)
- material: MaterialDefinition に RT field なし(materialformat.hpp:89)。
  RT task は material set(set 2)を bind しない(computetask.cpp:2957)
- dispatch: 単一 image port の `rays_from` のみ(computetask.cpp:1260)
- runtime facts: RT property は alignment/size 3 種のみ(core.cpp:363)。
  recursion / maxShaderGroupStride / maxRayHitAttributeSize は未取得
- shader reflection は RT interface(payload/hit attribute/callable data)を検査しない
  (shaderreflection.hpp:51)。stage 語彙は resolver / core / wire / Studio の
  全所で閉じている

## 2. 到達点と非目標

書けるべきもの: anyhit(実 material alpha で)/ intersection(procedural)/
callable / material ごとの hit shader / instance・geometry の flags・mask・
custom index / 再帰パストレ / shader record(静的+動的)/ 複数 TLAS /
**変形 geometry(skinned・morph・VAT)の RT 描画**(第 1 巡 #9: 速度非重視ゆえ
毎フレーム rebuild でも到達点に含める)/ buffer・1D・3D・indirect の trace。
非目標: denoiser、motion blur AS、SER 等の性能系(需要実測後に別設計)。

## 3. 背骨の 3 決定(第 1 巡の破綻 3 件への答え)

### 3-1. group layout は project 所有の global 契約(#1)

TLAS は scene に一つ、SBT は pipeline ごと。ゆえに **`group name → stable slot` の
表を task-local にせず、project 所有の `ray_group_layout` として一枚**にする:

```json
{ "schema": "pelican.ray_group_layout", "version": 1,
  "ray_types": ["primary", "shadow"],
  "groups": [ { "name": "opaque" }, { "name": "foliage" } ] }
```

- 物理 slot = `group_index × ray_type_count + ray_type_index`(宣言順で固定・全 SBT 共通)
- 各 RT task は「どの slot にどの shader を挿すか」を宣言する側に回る。
  **未充填 slot は名前付きエラー**(task 名・group 名・ray type・available 一覧つき)
- slot 定数は **生成 shader 定数**(`pelican_ray_layout.h` を build 時生成)として
  raygen から参照する。手書き数値 offset は lint で拒否
- 旧 flat 構文は slot 順保存の sugar として完全互換(第 1 巡「壊せなかった項目」)

### 3-2. material データ ABI(#2)

shader code の選択だけでは alpha texture を読めない。RT2 で:

- **RT material table**: bindless descriptor array(または device-address table)に
  material parameter block + texture index を平坦化。SBT hit record に
  `{material_index, geometry_index}` を持たせる(instanceCustomIndex とは独立。
  custom index を engine 用途と兼用しない)
- **geometry access**: vertex/index buffer の device address 表(UV・normal の
  取り出し accessor を生成 glsl として提供)
- material 側の束縛は **base 文書への直書きではなく** path 所有の overlay
  `pelican.ray_bindings@1`(#12): material/component の stable reference →
  hit group・mask・flags。**feature purge = overlay ごと削除**で孤児ゼロ

### 3-3. オラクルは「決定的部分集合」契約(#3, #4)

CPU 参照と GPU の一意比較は一般には存在しない(候補順・tie・順序依存 payload)。

- **保証対象の subset を文書で定義**: any-hit は候補局所の accept/ignore のみ /
  payload の順序依存更新をしない / tie・共有辺・近接 t は分離述語で assertion 対象外 /
  比較は最終 visibility と最近接区間のみ
- subset 外の自由 shader は書ける(拒否しない)が、**cross-device golden 非保証の
  escape hatch** として明示区分(golden 化する fixture は subset 内で書く)
- **sentinel 検査を一級に**(#4): 各 slot/record に異なる sentinel を書かせ、
  resolved slot 表・record stride・`instanceOffset + geometryIndex×stride + offset`
  の各項・miss index・再帰の実効を**同一テスト内で** off/on 比較。
  交差だけのオラクルでは SBT 計算は未検証、を設計上の警句として残す

## 4. capability・構成(#5)

- 「名前付き skip」を撤回。**選択済み feature × 非対応 device = hard reject**
  (現行原則どおり)。authoring UI は選択前に unavailable reason を提示
- capability は既存 `pelican.vulkan.ray_tracing_pipeline@1`(@N は major version)。
  数値上限は capability でなく **runtime facts に追加**:
  maxRayRecursionDepth / maxShaderGroupStride / maxRayHitAttributeSize。
  超過は device property 名・要求値・実値・device 名つきの名前付きエラー
- web は `compute_tasks` 明示拒否のまま(subset 原則、09_web.md:60)
- 全段の受け入れ matrix: 4 project × feature off/on(off で AS build 0・frame plan
  不変・描画 bytes 不変)/ shader compiler ON/OFF / Studio ON/OFF / web 拒否

## 5. 二車線の去就(#10)

ray query 車線と pipeline 車線の byte 一致契約は **RT2 で意味が割れる**
(pipeline は anyhit alpha、query は Opaque first-hit)。裁定:
**RT2 で ray query 側にも material alpha candidate loop を実装し、一致契約を維持**
(速度非重視だから許容)。両車線同時有効の production 一致テストを RT2 の受け入れに。
mask/flags(RT3)も両車線の AS に同時適用。

## 6. 段(1 段 = 1 出荷単位)

### RT1: layout 契約と SBT 一般化(単 pipeline 内で完結)

- `ray_group_layout`(§3-1)+ 生成 shader 定数
- anyhit 解禁(hit group は `{ "type": "triangles", "closesthit": …, "anyhit": … }`
  の typed 形。#8 の型分離を先取り)
- `max_recursion`(facts 比較、超過 hard reject)
- record 定数(静的): ABI は leaf 語彙 → **明示 offset/size 表を正準化 JSON に出力**
  (std430 相当の導出規則を文書に固定。整合は静的検査)
- **stage 語彙拡張を全所同段で**: `.rahit`(+ RT3 用に `.rint`、RT4 用 `.rcall` の
  語彙自体は今回入れ、未対応段では named 拒否)を pelican_project wire 正本 →
  core enum / compiler / frame-plan wire / PlanViewer / Studio(D0: Studio は
  wire 経由のみ)
- **SPIR-V interface 照合**(#11): payload / hit attribute の location・型を
  reflection に追加し、stage 間不一致を group/path 名付き load 時エラーに
- CPU 参照オラクル(§3-3 subset + sentinel)を同段で導入
- 互換: 旧 flat 構文 sugar、既存 rt_shadow_mask_pipeline は bytes 不変

### RT2: material 束縛と両車線 alpha

§3-2 の ABI 全部 + overlay `pelican.ray_bindings@1` + per-instance
`instanceShaderBindingTableRecordOffset`(= global slot)+ ray query 車線の
alpha loop(§5)+ WP354 material drill-through の RT 面への投影。

### RT3: AS 宣言面(静的)

- **`geometry_flags`(BLAS 側: opaque / no_duplicate_anyhit)と
  `instance_flags`(TLAS 側: force_opaque / force_no_opaque / …)を別名で分離**(#7)。
  geometry_flags は BLAS cache key に入り、同一 geometry への矛盾宣言は
  instance 名一覧つきで拒否
- mask(既定 0xff)/ custom_index(**未指定は 0。TLAS 列順を既定値として
  shader に晒さない**、#12 末尾)
- intersection + procedural AABB は **CPU-authored / static のみ**(#8。
  storage-port 供給は RT6 の AS graph node 化とセット)。
  hit group `{ "type": "procedural", "intersection": …, … }`

### RT4: callable と動的 record

- callable 解禁(SBT に callable region)
- 動的 record: **per-in-flight SBT storage / immutable frame generation**(#13)、
  producer → record copy → trace の graph 依存と barrier、退役は submission lease。
  N/N+1 で異なる resolved record を同時検査

### RT5: 変形 geometry(#9)

skinned / morph / VAT の current-frame vertex からの BLAS rebuild/refit
(毎フレーム再構築で可。deform 済み頂点は既存 compute 出力を再利用)。
vrm_xr_demo の歩行 character が RT 影に正しく現れることが受け入れ。

### RT6: dispatch 一般化と AS の graph node 化

- `rays_from` に加え explicit extent / buffer count / **indirect** dispatch(#9 末尾)
- AS build を frame graph node へ(storage-port AABB・GPU driven rebuild が可能に。
  producer→AS build→trace の resource edge と BLAS invalidation generation を定義)
- 複数 TLAS(名前付き宣言、component の `ray.scene` 所属、raygen へ複数 binding)

### RT7: ノード一級化と編集 RPC(#14)

- generic RT task の add/update/remove RPC(source origin/digest 持ち)
- engine/preset node → project fragment への fork 操作
- candidate compile → SBT/TLAS/material binding 共同検証 → CAS commit
  (失敗時旧 generation 維持、material・task・group・available・source path を返す)
- RPE12b 凍結・A/B 測定ループへの搭載

依存: RT1 → RT2 → RT3 → {RT4, RT5} → RT6 → RT7。

## 7. エラー文言の最低内容(第 1 巡ゲート要求の採用)

group 不足: material 文書/名前/variant + task/path + 要求 group + available 一覧。
device limit: property 名 + 要求値 + 実値 + device 名。
SBT 範囲: instance/geometry/shader 各項 + record 数。
BLAS flag 衝突: geometry identity + 競合 component path 一覧。
編集失敗: 旧 generation 維持 + candidate digest + 復旧方法。

## 8. 未決事項

1. record ABI の導出規則の詳細(RT1 の WP 仕様で表として固定)
2. bindless の実装形(descriptor indexing vs buffer device address。device 普及率実測後)
3. per-path TLAS(§3-1 で不要化したが、mask では足りない演出要求が出た場合の再訪)
4. ray query 車線の alpha loop の shader 生成方法(材質ごとの分岐 vs uber-loop)

## 9. 第 1 巡指摘(14 件)への対応表

| # | 指摘 | v2 |
|---|---|---|
| 1 | 共有 TLAS × pipeline 別 SBT で offset 非一意 | §3-1 global `ray_group_layout` |
| 2 | material data ABI 欠落 | §3-2(bindless 表 + geometry access + overlay) |
| 3 | CPU オラクル述語が一般に不在 | §3-3 決定的部分集合 + escape hatch |
| 4 | 交差オラクルでは SBT/record 未検証 | §3-3 sentinel 検査を一級化 |
| 5 | named skip が fail-fast と衝突 / @N 誤用 / facts 不足 | §4 hard reject + facts 追加 |
| 6 | 複数 group の default 順序意味が不安定 | §3-1 stable slot 表 + 生成定数(task-local 配列を廃止) |
| 7 | geometry/instance flag 混同 | RT3 で別名分離 + BLAS cache key |
| 8 | storage-port AABB が同フレームで届かない | RT3 は static 限定、GPU 供給は RT6(AS node 化)へ |
| 9 | 変形 geometry が最後まで消えたまま / dispatch 硬直 | RT5 新設 + RT6 dispatch 一般化 |
| 10 | 二車線の意味分裂 | §5 ray query alpha を RT2 同段、一致契約維持 |
| 11 | RT interface 検証と stage 語彙の閉鎖 | RT1 で reflection 拡張 + 語彙を wire 正本から全所同段 |
| 12 | base 文書直書きの purge 崩れ / custom index 既定の列順露出 | §3-2 overlay / RT3 で未指定=0 |
| 13 | 動的 record の in-flight 競合 | RT4 per-in-flight generation |
| 14 | RT5(旧)編集が保存経路に無い | RT7 に RPC・fork・CAS を明記 |
