# 不要な render target の削減: 寿命の正確化と区間詰めによるメモリ共有

対象読者: エンジン担当(描画コンパイラ / renderer)。
ステータス: 改善案メモ(2026-10-16。議論の記録。WP 化・レビュー前)。
前提: マニュアル [第6章 §6.6 target planning](manual/06_rendering.md#target-planning-tree)、
[design_render_pipeline_extensibility.md](design_render_pipeline_extensibility.md)、
[v1 開発計画](v1_development_plan.md)「保守上の整理候補」。
数字は `test/fixtures/devstudio/example_frame_plan.json`(example の実機の物理計画)から 1920×1080 で手計算したもの。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- example(1080p)で **全 RT の合計 120 MiB に対し、同時に生きている最大は 81 MiB**。今の alias が節約しているのは 1 組(7.9 MiB)だけ。
- 「不要」は 3 種類あり、効く手段が違う: **(A) 人為的に伸びた寿命**、**(B) alias 条件の狭さ**(同 format・同 extent・同 usage の画像同士しか共有できない)、**(C) 宣言・パス単位の無駄**(unread な出力・パス、不要な store)。
- 提案の本丸は B: alias を**寿命区間による汎用メモリ共有(区間詰め)**に一般化し、gbuffer の跡地に bloom 鎖が入るようにする。
  まず計測の口(合計 / 同時最大 / 資源ごとの bytes と寿命)を `get_frame_plan` に出す。

## 1. 現状の数字(example、1080p)

| 資源 | format | extent | MiB | 寿命(order) | aliasable |
|---|---|---|---:|---|---|
| gbuffer_albedo / g_emissive / gbuffer_material | B8G8R8A8 / R8G8B8A8 | 1920×1080 | 7.9 ×3 | 0–3 | ○ |
| gbuffer_normal / gbuffer_worldpos | R16G16B16A16 | 1920×1080 | 15.8 ×2 | 0–3 | ○ |
| offscreen_depth | D32 | 1920×1080 | 7.9 | 0–**29** | ×(required) |
| ssao_output / ssao_blur | R8 | 1920×1080 | 2.0 ×2 | 1–2 / 2–3 | ○ |
| lit_color | R16G16B16A16 | 1920×1080 | 15.8 | 3–**29** | ×(required) |
| opaque_color / opaque_depth(snapshot) | R16G16B16A16 / D32 | 1920×1080 | 15.8 / 7.9 | 4–**29** / 5–**29** | ×(require_store) |
| Bloom_Threshold_RT | B8G8R8A8 | 1920×1080 | 7.9 | 11–12 | ○(g_emissive と alias) |
| Bloom_Downsample_H_k / Upsample_V_k(k=0..3) | B8G8R8A8 | 1/2 … 1/16 | 2.0 … 0.03 | 12–23 | ○ |
| display | B8G8R8A8 | 160×90 | 0.05 | 23–29 | × |

- 合計 120.0 MiB、同時最大 81.1 MiB(order 3 = lighting_pass)。
- gbuffer 5 枚 + depth = 63 MiB は lighting の後に空く。bloom 鎖 9 枚 = 13 MiB は order 11 から始まるので、全部その跡地に入る大きさ。
- `opaque_*`(24 MiB)は `forward_transparent`(order 6)しか読まないのに末尾まで生きている。
- 物理計画の `reason`: 21 枚中 15 枚が `arbitrary read requires a materialized resource`(sampler 読みなので妥当)。
- `unread_output` 診断: 0 件(example には無駄な出力は無い)。

## 2. A: 人為的に伸びている寿命

[`targetrenderplanning.cpp:1700-1710`](../src/project/targetrenderplanning.cpp#L1700): `materialization == required / external` か
`pattern.require_store` の資源は **`last_use` を末尾ノードに強制**する。

- `required` の出所([`logicalframegraphadapter.cpp:120-150`](../src/core/renderingpass/logicalframegraphadapter.cpp#L120)):
  (a) history 付き資源(`legacy history resource must survive across frames`)、
  (b) 論理型の宣言に `materialization` があるもの(`required_targets` / `render.target_ref` が運ぶ型。`lit_color` は bloom の
  `required_targets` の `pelican.render.color_signal@1`、`offscreen_depth` は depth 型と思われる — **型側の既定を確認する**)。
- `require_store` の出所: snapshot の出力(`snapshot output requires a stable sampled image`)。

調べること: (b) と snapshot が「フレーム末尾まで」要るのは何のためか(headless の `--render-out` の読み出し、picking の depth、
Studio / RenderDoc のキャプチャ、外部 depth export)。用途が特定の経路だけなら、その経路が有効なときだけ末尾まで伸ばす
(opt-in)。example では最大 −24(opaque_*)〜−47 MiB(lit_color / offscreen_depth 含む)の寿命短縮になり、B の効果も上がる。

## 3. B: alias を区間詰めに一般化する(本丸)

### 3.1 現状

- 計画: [`deriveLegalAliasCandidates()`](../src/project/targetrenderplanning.cpp#L3124) は **同 representation・format・samples・view layout・mip・layers・dimension・extent** かつ寿命非重複の組だけを候補にする。
- runtime: [`allocAliasingImage()`](../src/core/vkcore/core.cpp) は owner の `VkImageCreateInfo` をそのまま複製して owner の allocation に bind
  (offset 0、同サイズ)。[`rendertargetcontainer.cpp:710`](../src/core/renderingpass/rendertargetcontainer.cpp#L710) も format / usage / samples / mip の一致を要求。
- 結果、「大きい gbuffer の跡地に小さい bloom を置く」ができない。

### 3.2 提案

```text
planner(src/project/targetrenderplanning、純ロジック)
  入力: 資源ごとの bytes(format × extent × layers × mips、alignment)と寿命 [first_use, last_use]、aliasable、no_alias
  処理: 寿命が重ならない資源を 1 つ以上のヒープに区間詰め(interval packing。first-fit で十分、決定的順序)
  出力: alias_groups → memory_regions [{heap, offset, size, resources[]}]
        + alias_reuse barrier の要求 [{from: 前の住人の最終アクセス node, to: 次の住人の最初の書き込み node, heap, offset}]
runtime(rendertargetcontainer / vkcore)
  ヒープごとに allocation 1 つ(VMA)。各 image は vmaCreateAliasingImage2(allocation, offset, createInfo) で bind。
  VK_IMAGE_CREATE_ALIAS_BIT は既に付けている。
  memoryTypeBits はヒープ内の全 image で AND を取り、空なら計画段階で拒否(device facts として planner に渡す)。
renderer(executePlannedFrameGraph)
  alias_reuse barrier: 前の住人の最終アクセス → 次の住人の最初の書き込みに execution + memory dependency、
  次の住人の oldLayout = UNDEFINED(内容は引き継がない)。最初の使用が `load` の資源は共有不可(既存の aliasable 判定と同じ)。
```

- `no_alias` / `isolate` / `hazard_stress` profile はそのまま効く(詰め方の撹拌にも使える)。
- 期待値: example で 120 → 85 MiB 前後、A と合わせて 81 に近づく。RT を多く宣言する feature(bloom、planar reflection)ほど効く。
  **「feature は RT の枚数を気にせず宣言してよい」を成立させる手段**でもあり、preset レシピ化([design_render_preset_feature_recipe.md](design_render_preset_feature_recipe.md))と相性がよい。
- 正しさの難所は barrier。planner が `alias_reuse` を**自動で出す**形にして、手書き barrier ゼロの原則を崩さない。
  frame をまたぐ再利用(frame N の最後の住人 → frame N+1 の最初の住人)も同じ規則で、frame 先頭の layout 遷移に含める。

### 3.3 やらないこと

- format / extent が違う image の**内容**の共有(alias は内容を引き継がない)。
- 複数 queue / async compute(graphics queue 1 本のまま)。
- tile-local / transient 候補の変更(別件。desktop では選ばれない)。

## 4. C: 宣言・パス単位の無駄

- **dead pass elimination**: `unread_output` 診断([`findFramePlanUnreadOutputs()`](../src/core/renderingpass/frameplanner.cpp#L1689))は出すだけ。
  出力が全部 unread で副作用(snapshot / readback / 外部 export)を持たないパスを実行対象から外す。feature 除外や編集中に出やすい。
- **store の自動 discard**: 最終読み手の後の `store_op` を `discard` に(帯域)。寿命解析は既にあるので planner で決められる。
  既定は color 全 `store`、depth `dont_care`([`passattachmentoperations.cpp:7`](../src/core/renderingpass/passattachmentoperations.cpp#L7))。
- 宣言だけされて一度も使われない RT は、lifetime.used = false の資源として既に計画から落ちている(確保されない)ことを確認する。

## 5. 段階

| 段 | 内容 | 規模 | 検証 |
|---|---|---|---|
| 0 | 計測: `get_frame_plan` に `memory_summary {total_bytes, peak_live_bytes, peak_node}` と資源ごとの `bytes` を出す。Studio の Frame Plan に表示 | 小 | planner の単体テスト(GPU 不要)。手計算の 120 / 81 MiB と一致 |
| 1 | A: required / require_store の末尾延長の理由を特定し、必要な経路だけ opt-in に | 小〜中 | 物理計画の fixture 差分、golden 不変、headless 読み出し・picking・capture の回帰 |
| 2 | B: 区間詰め alias(planner → runtime → barrier) | 大 | planner 単体(詰め結果の決定性、no_alias、memoryTypeBits 拒否)、GPU gate 全数、`hazard_stress` での揺さぶり、validation layer |
| 3 | C: dead pass elimination、store 自動 discard | 中 | frame plan の fixture、golden 不変 |

0 と 1 は純ロジック中心。2 は GPU gate 必須(Windows)。

## 6. 未決(2026-10-17 更新)

議論で決まった・動いたもの:

- **計測の表示場所** → arena の `declared_bytes`([棚卸しメモ](design_plan_layer_inventory.md) 2-4)。今 0 固定の field を埋めれば
  Frame Plan の bytes 列がそのまま表示になる。`get_status` の VRAM 区分は後。
- **優先度** → 上がった。mac(統合メモリ)と Blender の教訓(OpenGL ドライバは GPU メモリを RAM に退避できるが Vulkan はできない。
  Blender 5.0 が Vulkan を既定にしなかった理由)で、同時最大の VRAM がそのまま体験に効く。
- **Apple GPU での効き方** → MoltenVK では transient 候補が memoryless texture に落ちるので、A の寿命短縮は alias だけでなく
  transient 判定にも効く。段 1 の価値が増す。

残るもの:

- `required` を付けている論理型の既定が末尾までの生存を要求しているか(調べる。変わらず)。
- **ヒープの単位** → MoltenVK は 1.3.0 から MTLHeap が既定なので heap 1 つで素直に動く見込み。Windows は memoryTypeBits ごとに分ける。
  device facts で切り替える形にする。
- **`alias_reuse` barrier の表現** → 別の表を推す(`FramePlanBarrier` は resource 1 つ。2 つ目の resource のために既存の形を歪めない)。
- **同じ WP に入るもの** → 物理計画の固定ルートを `target_planning` の policy に一本化する件([棚卸しメモ](design_plan_layer_inventory.md) 2-3、方針決定 2026-10-22)。`no_alias` は既にあり、`alias_group` / `materialize` などを足す語彙は区間詰めの入力と同じ場所。
