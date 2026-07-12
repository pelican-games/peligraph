# アニメーション基盤とグラフ(v2)

対象読者: エンジン担当・キャラクターを動かす人・VRM/DCC コンテンツを扱う人。
ステータス: v2 ドラフト(2026-07-12。v1 は業界サーベイ
`docs/design_reviews/2026-07-12_animgraph_research_codex.md`(以下「調査」)の
差し戻しを受理して全面改稿 — **v1 の 3 関数 API は凍結せず experimental/v0
扱い**。根拠・出典は調査が正)。
前提: WP38(クリップ v1)、「feature 層 = ユーザー空間」方針、G1a/G2、
WP89(収録/リプレイ)、E1。

## 0. 三層と設計原則(維持 + 改訂)

| 層 | 内容 | 帰属 |
|----|------|------|
| **機構語彙(A0/A1)** | rig/pose/clip の型・サンプル・ブレンド・commit・phase(§1) | エンジン(版付き・決定的) |
| **グラフアセット(A2)** | `pelican.anim_graph` v1 — 小さいまま(§2) | データ形式(凍結規律) |
| **評価器** | AnimGraphSystem = 特権なし標準ライブラリ | ユーザー空間(自作・改造自由) |

**改訂原則(調査 §8)**: **A1 を太らせ、A2 を小さく保つ**。語彙が痩せたまま
schema に機能を積むと標準評価器だけが特権化し「評価器ごと自作できる」が
空文化する。ozz-animation の責務分離(job 語彙のみ・グラフなし)が先行例。

## 1. 機構語彙(A0 = 契約凍結ゲートの対象)

### 1-1. 型と同一性

- **AnimationRig / PoseLayout**: 全アニメ階層(**skin joint 集合では
  ない** — non-joint ノード・animation-only rig を含む)の parent・
  rest ローカル TRS・名前/semantic。**generation 付き identity** を持ち、
  互換照会 API を提供(骨数が同じ別 rig の誤ブレンドを検出)。
  ※現行 SkeletalModelData は node 名を保存していない — A1 で追加
- **SkinBinding**: rig node → palette joint 写像 + inverseBind。
  1 rig : N skin を分離(WP38 の複数 skin 正規化を昇格)
- **Pose**: PoseLayout identity 付きの **engine 管理バッファ +
  書換可能 view**(by-value 返却を凍結しない)。lifetime は評価フレームと
  asset generation に拘束(hot reload 時の stale handle は generation
  不一致エラー)。rest 初期化
- **Clip**: **model GLB から独立したリソース**(source rig 参照を持つ)。
  animation-only コンテナ(VRMA)・共有クリップ・retarget の前提。
  現行「clip と model は同一 GLB」制約は簡易経路の糖衣として残す

### 1-2. 時点と区間の分離(調査 §5.2)

- `samplePoseAt(clip, t)` = **時点**関数(Pose を返す)
- `advanceCursor(cursor, dt)` = **区間**関数 — 戻りは版付き sideband:
  root motion delta / event・marker crossing(loop 跨ぎ・seek・逆再生
  対応、0 個以上)/ 正規化 phase / 不連続フラグ / curve・attribute。
  **後から戻り値を足しても ABI が壊れない構造**(versioned descriptor)

### 1-3. ブレンド記述(調査 §5.3)

scalar 重みの単関数でなく**版付き blend layer 記述**:
pose・重み・**per-joint weights(ボーンマスク)**・normal/additive・
additive reference pose・rest fallback・sideband(root/curve)の合成
policy。**N-way quaternion の規範を明文化**(入力順・sign 正準化・
正規化アルゴリズム・zero weight・許容誤差 — byte 決定性の土台)。
ozz の BlendingJob(per-joint weights・additive layer)が実在証明。

### 1-4. commit と phase(調査 §5.4・§4.1)

- 公開終端は `setPalette` でなく **instance animation-frame commit**:
  local/model pose・GPU palette・socket/attachment 用 model-space
  transform・morph/expression curve・gaze・root delta・event candidate が
  **同一 frame revision** に属す。palette は内部派生物
- **named animation phases**(エンジン所有の順序語彙):
  `param snapshot → base pose + root modifier 評価 → movement/root 解決 →
  world 依存 post-process(foot IK 等)→ commit`。
  標準評価器も特権なしでこの phase に登録するだけ
- local↔model 変換・階層 view を公開(IK・retarget・motion matching を
  ユーザー空間で書ける下地)

## 2. `pelican.anim_graph` v1(小さいまま + 意味論の穴埋め)

v1 スコープ = clip state + blend1d + crossfade(調査 §6.1 で条件付き承認)。
形式は v1 の JSON を維持し、**意味論を確定**:

1. **state clock / 共有 phase**: state は正規化 phase を 1 個持ち、
   blend1d の全 clip は phase 同期で time-scale(length sync)。
   named sync marker は将来の additive 拡張(clip metadata 側)。
   enter/re-enter の phase は reset(continue は v2)
2. **遷移の順序と割込み(調査 §6.3 — v1 の「割込みなし」を撤回)**:
   - transition に `priority`(int・省略 0)。複数 true は
     priority → 宣言順で決定
   - `interrupt: "never" | "higher_priority" | "always"`(省略 never)
   - **割込み時の遷移元 = その tick の現在ブレンド済み pose の
     snapshot**(state 名でない — inertialization への将来互換)
   - `duration: 0` の緊急カットは常に許可
   - ユーザーシステムからの `forceState(object, state)`(明示遷移)
3. パラメータ: NaN/Inf は set 時に拒否。bool の一回性 consume は
   ユーザー責務と明記(trigger 型は v2 予約)
4. v2 予約キー(存在 = エラー): `layers` / `events` / `graphs` /
   `sync`(marker)/ `trigger`

## 3. ソースと所有権(調査 §4.1)

- **AnimationSource / slot**: graph・timeline(SeqPlayer/将来 director)・
  live・単発 clip を同じ「pose source」抽象に統一。
  component の `clip XOR graph` は簡易糖衣として維持し、共存・切替は
  source slot + **sink ごとの単一 writer + 引き継ぎ(handoff)規則**で
  調停(sink = object transform / skeletal pose / expression curve)
- root motion: pose commit と別の sideband。SeqPlayer が transform を
  所有するショットでは extract-only、graph 所有時は apply — source
  authority ごとの policy(暗黙の二重 writer を作らない)

## 4. glTF / VRMA の受理(調査 §4.2 — VRM 一級化の要件)

4 段パイプライン: ①コンテナ decode(`.vrma` = glb alias +
`VRMC_vrm_animation`。**unnamed/first animation の既定規則 =
`#animation/0`** を K1 に追加)② source rig + optional semantic
channel 化(humanoid / expression / gaze)③ **versioned retarget /
application profile**(humanoid 写像・T-pose 正規化・hips 身長比 —
**P1 課題**。汎用リターゲットとは別物)④ typed AnimationSource として
graph/timeline が消費。

- **VRM semantic decoder が VRMA より先**(現状は GLB として描ける
  だけ — humanoid map・expression bind・lookAt の保持と適用 sink が
  先行課題)。VRM の実行順(humanoid→lookAt→expression→constraint→
  springBone)は §1-4 の phase に写像
- morph weights / expression / gaze は **typed channel**(joint Pose に
  混ぜない — UAF の Pose+Curve+Attribute と同方向)
- **VRMA の hips translation を自動で root motion と解釈しない**
  (抽出は import/clip profile の明示 policy)

## 5. live streaming の将来穴(調査 §4.3)

- 穴 = 終端 API でなく **transport 非依存の AnimationSource**
  (SubjectId・revision 付き static rig・timestamp/discontinuity 付き
  frame・buffer・EngineTime への clock mapping・missing frame policy)
- transport は G2 ユーザー DLL(unregister/quiesce/generation 検出の
  unload 契約)。受信 thread は buffer へ push のみ、適用はフレーム境界
- live は非決定 — **raw take(再 retarget 可能)と deterministic bake
  (fixed tick + profile/hash 記録)を別成果物**とし、replay 中は live
  停止(WP89 の思想と一致)

## 6. 決定性の規範(調査 §5.6)

- 保証範囲を明記: **同一 build/architecture での byte 一致**(cross-CPU
  は非目標)。math 実装・order を変える時は version/hash を上げ golden
  再基準化手続き(色 §3 の流儀)
- 規範化対象: N-way blend 順・quaternion sign・fmod/wrap・同時刻 event の
  安定順(source, ordinal)・loop crossing 順・graph/resource の走査順・
  同値 weight の tie-break

## 7. 実装順(調査 §8 — A0 ゲートを新設)

| 段階 | 内容 | 依存 |
|------|------|------|
| **A0** | 公開契約の凍結ゲート: Rig/PoseLayout/SkinBinding/Pose 所有・point/interval 分離・blend 記述・commit・phase・G2 ABI を**小 fixture で確定**(実装は最小) | WP38(済) |
| **A1** | 機構 jobs: WP38 sampler の caller-owned Pose 分離・normal blend・local-to-model/commit・既存 clip component 互換 | A0 |
| **A1.5** | 敵対 fixture: non-joint 祖先・同骨数別 rig・loop 跨ぎ event/root・N-way 順序・hot reload stale handle・2 体並列 | A1 |
| **A2** | 最小コントローラ: clip state + blend1d + state machine(共有 phase・priority・interrupt/force・get_status)| A1.5 |
| 後続 | clip annotation/events → mask/additive → **VRM semantic + VRMA retarget** → timeline/live source。motion matching・ragdoll は基礎 API の別評価器として遠くへ | — |

## 8. 未決事項

1. Pose バッファの実装形(scratch arena vs opaque handle)— A0 で決定
2. ルートモーションの movement/物理との解決順の詳細 — phase 設計で
3. 2D blend space・named sync marker・inertialization — v2 形式改訂時
4. timeline director(typed track 束ね)の設計 — SeqPlayer v2 として別起草
