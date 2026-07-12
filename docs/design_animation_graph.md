# アニメーション基盤とグラフ(v2.1)

対象読者: エンジン担当・キャラクターを動かす人・VRM/DCC コンテンツを扱う人。
ステータス: v2.1(2026-07-12)。v1 は業界サーベイ
`docs/design_reviews/2026-07-12_animgraph_research_codex.md`(以下「調査」)の
差し戻しを受理して全面改稿 — **v1 の 3 関数 API は凍結せず experimental/v0
扱い**。根拠・出典は調査が正。
v2 は敵対レビュー
`docs/design_reviews/2026-07-12_anim_v2_hotreload_review_codex.md`(以下
「v2 レビュー」)で**条件付き承認** — 方向は承認、A0 完了/public freeze
宣言は同レビュー §4-5 の条件充足まで不可。v2.1 = その条件の正本反映
(A0 二分割・commit と temporal history の契約・VRMA 5 WP 分割・
DCC provenance 復活)。
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

- `samplePoseAt(clip, t)` = **時点**関数 — **caller-owned PoseView へ
  書き込む**(Pose を by-value で返さない。interval 結果にも Pose を
  混ぜない)
- `advanceCursor(cursor, dt)` = **区間**関数 — 戻りは版付き sideband:
  root motion delta / event・marker crossing(loop 跨ぎ・seek・逆再生
  対応、0 個以上)/ 正規化 phase / 不連続フラグ / curve・attribute。
  **後から戻り値を足しても ABI が壊れない構造**(versioned descriptor)
- **versioned descriptor の ABI 規範**(v2 レビュー §4.2 CA0-Interval —
  名前でなく性質を凍結対象とする):
  - in/out 構造体は `struct_size` + `version` 先頭固定。engine は
    `min(caller.struct_size, known_size)` までしか書かず、unknown
    tail/flag は version policy に従う
  - 可変長出力(crossing / curve value)は caller-owned 配列 +
    `capacity/count`(不足時 `BUFFER_TOO_SMALL` + required count)
    または query→fill の二段式。`std::vector` 等を ABI にしない
  - **失敗時の cursor 原子性**: capacity 不足・stale generation・
    validation error では cursor を進めない。再実行で同じ結果
  - `dt` advance と absolute seek を区別。forward/reverse・0/1/N loop・
    端点の半開区間・同時刻 crossing の `(source, ordinal)` 順を規範化
- **Clip metadata の A0 列挙対象**(v2 レビュー §1 #4): source rig、
  time range、wrap mode、channel inventory(kind 列挙)、annotation
  identity、sampling context / cursor generation。marker/event 本体は
  後続でも、**ABI 上の予約(識別子と版)** は A0 で行う

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

### 1-5. commit と temporal history の契約(v2 レビュー §5 — blocking)

WP88 の velocity/history と commit を接合する規範。現行実装は previous
object/camera 行列しか持たず、skinned velocity は current palette で作った
同一 `local_position` を current/previous 両方に使う
(`src/core/resources/velocity_skinned.vert`)ため、**骨だけが動く場合の
deformation velocity がゼロ**になる。commit 契約に以下を含める:

1. instance は committed animation revision **N と N-1** の pose/palette
   (または previous skinned position を再構築できる state)を保持する
2. render/velocity pass は current=N・previous=N-1 を**同じ instance
   identity** で読む。animation commit は current を publish するだけで
   previous を上書きしない
3. frame の temporal advance は velocity consumer の後に**一度だけ**行う
   (現行 object/camera history の render 終端 advance と同じ位置)
4. 初回・resize・`set_time`・model/rig hot reload・teleport 等の不連続は
   previous=current として zero velocity に reset する
5. model reload で instance ID が変わる設計なら previous history の移送
   または明示 reset を規定する
6. 誤用防止のため命名を分ける: **`publishAnimationFrame`**(評価側の
   commit)と **`advanceTemporalHistoryAfterRender`**(render 終端の
   history 前進)

現行 skinned velocity の欠陥自体は anim 基盤と独立に修正可能
(previous palette バッファの追加 — 別 WP)。

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
   ユーザー責務と明記(trigger 型は v2 予約)。**v1 の namespace は
   単一 flat** と明記(nested graph 導入時は version 2 converter 必須)
4. v2 予約キー(存在 = エラー): `layers` / `events` / `graphs` /
   `sync`(marker)/ `trigger`
5. **A2 受入条件として v2 レビュー §4.3(CA2-Interrupt)・§4.4
   (CA2-Clock)を逐語添付する**。要点:
   - 一 tick の transition decision は最大一回。
     `forceState(sequence) → priority → 宣言順` の一意順
   - interrupt 判定前に旧 transition の当該 tick pose を一度評価し、
     PoseLayout generation + frame revision 付きで snapshot 化。
     新 transition の alpha=0 出力は snapshot と byte 一致
   - snapshot は pose のみを遷移元とし、過去の root delta/event を
     再 emit しない。再 interrupt は chain せず現在出力を一枚に
     materialize
   - `set_time`・replay seek・graph/model reload・layout generation
     mismatch での reset/reconstruct/error を明記
   - deterministic trace に state・cursor・transition progress・
     snapshot revision/layout・semantic pose hash(pointer/offset は
     hash に含めない)
   - clock: clip speed(負含む)・start offset・loop endpoint・
     zero-duration clip・blend1d leader 選択と同値 weight tie-break・
     「同 tick の exit→enter」の re-enter reset 順

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
- **DCC provenance**(調査 §9 #9・v2 レビューで脱落指摘): Clip / retarget
  profile の metadata に source URI・content hash・import profile・tool
  version を保持(VRMA-C0 / VRMA-R0 の成果物に含める)
- **WP 分割は 5 WP**(v2 レビュー §4.5 — 「VRM semantic + VRMA retarget」
  一塊は不可。SpringBone は混ぜない):

  | WP | 範囲 | 規模 |
  |----|------|------|
  | VRM-S0 | `.vrm` semantic decode/storage(humanoid・expression bind・lookAt・constraint metadata・extension version 検証) | 中〜大 |
  | VRM-S1 | per-instance application sink + phase(morph/material/UV expression・gaze・constraint hook)— renderer/material の per-instance 化を含むため**必ず独立 WP** | 大 |
  | VRMA-C0 | `.vrma` alias・`VRMC_vrm_animation`・`#animation/0` 既定・body/expression/gaze typed channel | 中 |
  | VRMA-R0 | versioned retarget/application profile(humanoid map・rest/T-pose 正規化・optional bone・hips scale) | 大 |
  | VRMA-I0 | AnimationSource/graph/timeline 接続・authority・hot reload generation | 中 |

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

## 7. 実装順(調査 §8 + v2 レビュー §4.1 — A0 は二成果物)

**A0 の性格を訂正(v2 レビュー §4.1)**: 「fixture で契約を決める」のでは
なく「**公開契約を fixture で検証する**」。完了条件 = fixture green だけで
なく、**凍結 public header + normative semantics が存在し、その header を
二世代 fixture が検査する**こと。

| 段階 | 内容 | 依存 |
|------|------|------|
| **A0 / CA0-Spec** | 実装なしで凍結可能な契約: public header に固定幅 opaque handle・invalid 値・generation・`struct_size`/`version`/reserved-zero・status/error。Rig/PoseLayout/SkinBinding/Clip/Cursor の identity・exact compatibility・stale・destroy/unload 規則の文章化。frame lifetime・read/write alias・thread ownership・failure atomicity・phase 登録/tie-break・commit revision の規範化。old client/new engine と new client/old engine の descriptor negotiation | WP38(済) |
| **A0 / CA0-Probe** | 最小実装がないと凍結不可な契約の検証: arena/opaque buffer の acquire→writable view→frame reset・64-byte alignment・二体 parallel・DLL reload/stale generation。N-way quaternion reference algorithm・wrap/reverse/multi-loop interval・local-to-model・commit 一往復の golden 化。外部 game DLL を旧/新 header でビルドし `sizeof/offsetof/struct_size` と unknown tail 無視を検査 | CA0-Spec |
| **A1** | 機構 jobs: WP38 sampler の caller-owned Pose 分離・normal blend・local-to-model/commit(**§1-5 の N/N-1 palette 契約を含む**)・既存 clip component 互換 | A0 |
| **A1.5** | 敵対 fixture: non-joint 祖先・同骨数別 rig・loop 跨ぎ event/root・N-way 順序・hot reload stale handle・2 体並列 | A1 |
| **A2** | 最小コントローラ: clip state + blend1d + state machine(共有 phase・priority・interrupt/force・get_status)。**受入条件 = v2 レビュー §4.3/§4.4 逐語** | A1.5 |
| 後続 | clip annotation/events → mask/additive → **VRM-S0 → VRM-S1 → VRMA-C0 → VRMA-R0 → VRMA-I0**(§4 の 5 WP)→ timeline/live source。motion matching・ragdoll は基礎 API の別評価器として遠くへ | — |

## 8. 未決事項

1. Pose バッファの実装形(scratch arena vs opaque handle)—
   **CA0-Spec の public header で決定**(A1 送りにしない。選択が ABI と
   release 規則を変えるため — v2 レビュー §1 #3)
2. ルートモーションの movement/物理との解決順の詳細 — phase 設計で
3. 2D blend space・named sync marker・inertialization — v2 形式改訂時
   (inertialization の velocity/history は別 versioned source state と
   する — v2 レビュー §4.3)
4. timeline director(typed track 束ね)の設計 — SeqPlayer v2 として別起草
5. blend layer の additive space(local vs model/mesh)区別と
   event/marker 抑制 policy — CA0-Spec の blend 記述に含める
   (v2 レビュー §1 #6)
