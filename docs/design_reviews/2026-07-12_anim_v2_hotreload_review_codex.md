# アニメーション基盤 v2 + アセットホットリロード v1 敵対レビュー

日付: 2026-07-12  
対象: `docs/design_animation_graph.md` v2、`docs/design_asset_hot_reload.md` v1  
照合正本: `docs/design_reviews/2026-07-12_animgraph_research_codex.md`

## 0. 結論

| 文書 | 判定 | 結論 |
|---|---|---|
| `design_animation_graph.md` v2 | **条件付き** | 「A1 を太らせ A2 を小さくする」方向とサーベイ骨子の大半は受理された。しかし A0 はまだ概念一覧であり、G2 を跨ぐ具体 ABI、Clip/cursor metadata、区間 overflow、snapshot 遷移の tick 規則、前フレーム skin pose/history が未契約である。従って **v2 文書の方向は承認するが、A0 完了または public freeze の宣言は不可**。本書 §4 の条件を A0/A2 の受入条件へ添付すること。根拠: `docs/design_animation_graph.md:23-70`, `docs/design_animation_graph.md:144-158`。 |
| `design_asset_hot_reload.md` v1 | **Reject** | Watcher の overflow/停止復帰プロトコルがなく、WP82 hash の「流用」は現行コード上成立せず、WP62 EntityId を model resource ID と取り違え、現行 GPU resource container には安全な replace/rollback 面がない。現 H1/H2 は見積単位として成立しない。**方向は妥当だが、§9 の HR0〜HR2 へ再分割して再起草するまで着手不可**。根拠: `docs/design_asset_hot_reload.md:25-38`, `docs/design_asset_hot_reload.md:40-67`, `docs/design_asset_hot_reload.md:85-91`。 |

レビューは文書、関連 WP report、現行 C++ を行番号で照合した。Win32 固有条件は Microsoft の一次資料 [ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw) と [CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex) を参照した。

---

## 1. アニメ v2: サーベイ §5.7「A1 の最小安定語彙」照合

サーベイの七項を「文書に単語があるか」ではなく「A0 で凍結できる契約か」で判定した。

| # | §5.7 の要求 | v2 反映 | 敵対判定 |
|---:|---|---|---|
| 1 | AnimationRig / PoseLayout: generation identity、parent/rest/name/semantic | `AnimationRig / PoseLayout`、full hierarchy、generation identity、互換照会を明記した (`docs/design_animation_graph.md:27-31`) | **反映**。ただし identity のビット表現、generation 更新条件、互換性の種類（exact/layout/retargetable）は A0 ABI に未記載。 |
| 2 | SkinBinding: node→palette、inverse bind、mesh/skin identity、1 rig:N skin | mapping、inverseBind、1:N を分離した (`docs/design_animation_graph.md:32-33`) | **部分反映**。`SkinBinding` 自身の identity/generation と mesh/skin identity が抜けた。model reload 後の stale binding を Pose だけの generation 検査で代用してはいけない。 |
| 3 | caller-writable Pose buffer/view、layout identity、lifetime、rest init | engine 管理 buffer、writable view、frame/asset generation lifetime、rest init を明記した (`docs/design_animation_graph.md:34-37`) | **部分反映**。実装形を A0 後の未決事項に残している (`docs/design_animation_graph.md:154-157`)。scratch/opaque handle の選択は ABI と release 規則を変えるため、fixture の前に public header で決める必要がある。 |
| 4 | Clip metadata/cursor: source rig、time range、wrap、channel kind、curve、marker/event、sampling context | 独立 Clip と source rig、cursor advance はある (`docs/design_animation_graph.md:38-48`) | **漏れが大きい**。time range、wrap mode、channel inventory、annotation identity、sampling context/cursor generation が列挙されていない。graph 側の「marker は将来」だけでは Clip ABI の予約にならない (`docs/design_animation_graph.md:77-80`)。 |
| 5 | point sample と interval advance の分離、root/crossing/phase/discontinuity | 分離と sideband の内容を明記した (`docs/design_animation_graph.md:42-48`) | **部分反映**。方向は正しいが `versioned descriptor` の C++ 形、可変長結果の capacity/overflow、失敗時 cursor 原子性、seek と `dt` の区別がない。§4.2 に最小面を提案する。 |
| 6 | blend layer: pose/weight/per-joint/normal-additive/reference/side policy | ほぼ全項を列挙した (`docs/design_animation_graph.md:50-57`) | **部分反映**。local-space と model/mesh-space additive の区別、および event/marker 抑制 policy が抜ける。N-way 規範は「明文化する」と書いただけで規範値はまだない。 |
| 7 | local-to-model と final commit | hierarchy view/local↔model と frame revision 一括 commit を明記した (`docs/design_animation_graph.md:59-70`) | **部分反映**。現在 revision はあるが previous committed revision の保持/reset がない。これは既存 WP88 の skinned velocity と既に衝突する（§5）。 |

総括: 七項の名称は取り込まれたが、完全反映は #1 の概念面だけである。特に #3〜#5 は A0 の public ABI を左右するため、「A1 で実装しながら決める」へ送ってはならない。

## 2. アニメ v2: サーベイ §7「ABI/形式の地雷 20 項」全照合

凡例: ○=破断回避を設計へ反映、△=方向だけ／契約不足、×=未反映。

| # | 地雷 | 判定 | 根拠と残件 |
|---:|---|:---:|---|
| 1 | Pose が skin joint 順だけ | ○ | full animation hierarchy と別 SkinBinding にした (`docs/design_animation_graph.md:27-33`)。 |
| 2 | Pose に skeleton identity がない | △ | generation identity と compatibility query は入った (`docs/design_animation_graph.md:29-30`)。exact compatibility の規則・handle 表現は未確定。 |
| 3 | by-value point sample | △ | by-value を凍結しないとは書いたが、`samplePoseAt` が「Pose を返す」とも書く (`docs/design_animation_graph.md:34-35`, `docs/design_animation_graph.md:44-48`)。caller output/opaque handle の署名を A0 で固定せよ。 |
| 4 | allocator/lifetime 未定 | △ | lifetime は明記したが buffer 実装形は未決 (`docs/design_animation_graph.md:34-37`, `docs/design_animation_graph.md:154-157`)。明示 acquire/release と DLL unload 時の扱いがない。 |
| 5 | scalar-only blend | ○ | versioned layer、mask、additive、reference、fallback、sideband policy を入れた (`docs/design_animation_graph.md:50-56`)。 |
| 6 | N-way quaternion rule 未定 | △ | 規範項目は列挙したが実際の algorithm/tolerance は未記載 (`docs/design_animation_graph.md:55-56`)。A0 reference implementation と golden が必要。 |
| 7 | `setPalette` が公開 terminal | △ | animation-frame commit へ上げた (`docs/design_animation_graph.md:61-64`)。ただし previous pose/palette history を含まず、temporal consumer まで閉じていない。 |
| 8 | Clip が model と同一 GLB | ○ | 独立 Clip resource と source rig を明記し、現行制約を sugar に降格した (`docs/design_animation_graph.md:38-40`)。 |
| 9 | state が閉じた `clip XOR blend1d` union | △ | component 側は source slot/authority へ逃がした (`docs/design_animation_graph.md:94-103`)。一方 graph state の pose-source typed boundary/converter 方針は未記載。 |
| 10 | blend1d clock/phase 未定 | △ | shared normalized phase、length sync、reset は入った (`docs/design_animation_graph.md:77-80`)。clip speed/start offset/reverse/loop endpoint の規則がサーベイ要求 (`docs/design_reviews/2026-07-12_animgraph_research_codex.md:261-269`) から落ちた。 |
| 11 | transition order/priority 未定 | ○ | priority→宣言順を固定した (`docs/design_animation_graph.md:81-84`)。 |
| 12 | interruption source 未定 | △ | blended pose snapshot を採用した (`docs/design_animation_graph.md:84-87`)。ただし同 tick の評価順、複数 interrupt、seek/reload 時の snapshot reset がない（§4.3）。 |
| 13 | `events` の置き場だけ予約 | △ | interval crossing と commit candidate は入った (`docs/design_animation_graph.md:45-47`, `docs/design_animation_graph.md:61-64`)。annotation sidecar、duration begin/end、fade filter/routing は後続へ送られた (`docs/design_animation_graph.md:150-152`)。 |
| 14 | root motion policy がない | ○ | authority ごとの extract/apply と二重 writer 禁止を明記した (`docs/design_animation_graph.md:101-103`)。 |
| 15 | component が `clip XOR graph` のみ | ○ | AnimationSource/slot と sink 単位 writer/handoff を導入した (`docs/design_animation_graph.md:94-100`)。 |
| 16 | parameter namespace/trigger 未定 | △ | bool lifetime と trigger 予約は入った (`docs/design_animation_graph.md:89-92`)。nested/reusable graph 用 namespace/binding は抜けた。 |
| 17 | morph/expression/gaze typed output がない | △ | typed channel と commit sink を入れた (`docs/design_animation_graph.md:61-64`, `docs/design_animation_graph.md:119-120`)。channel element ABI と per-instance application sink は未設計。 |
| 18 | graph/model hot reload handle 規則なし | △ | asset generation と stale-handle error は入った (`docs/design_animation_graph.md:34-37`)。atomic rebind/reset、cursor/snapshot/SkinBinding の invalidation 規則がない。 |
| 19 | evaluator phase が一列 | △ | named phase を導入した (`docs/design_animation_graph.md:65-70`)。VRM 実行順を「写像」とするだけで登録点を列挙せず (`docs/design_animation_graph.md:115-118`)、temporal history publish もない。 |
| 20 | determinism の platform 範囲未定 | △ | same build/architecture、stable orders、version/hash は入った (`docs/design_animation_graph.md:135-142`)。float `==`、root/curve blend order、forceState と条件遷移の競合順、snapshot state trace が抜ける。 |

20 項中、明確に ○ とできるものは 6 項（1, 5, 8, 11, 14, 15）である。#7 の terminal 昇格も方向は正しいが、残りと同様に ABI/意味論の決定が必要である。これは全面差し戻し理由ではないが、A0 完了宣言を止める理由になる。

## 3. アニメ v2: サーベイ §9「改稿骨子 10 行」全照合

| # | 骨子 | 判定 | 根拠 |
|---:|---|:---:|---|
| 1 | 旧 API は experimental/v0、三層維持 | ○ | `docs/design_animation_graph.md:4-7`, `docs/design_animation_graph.md:11-21` |
| 2 | engine-managed Pose + writable view、Rig/Skin 分離 | △ | 反映はした (`docs/design_animation_graph.md:27-37`) が stable view の ABI/取得解放は未記載。 |
| 3 | point/interval 分離 + versioned sideband | △ | `docs/design_animation_graph.md:42-48`。descriptor と可変長出力規則が未記載。 |
| 4 | normal/additive/reference/per-joint/fallback/side policy | △ | `docs/design_animation_graph.md:50-57`。additive space と event policy がない。 |
| 5 | hierarchy/rest/name/semantic と local↔model | ○ | `docs/design_animation_graph.md:27-31`, `docs/design_animation_graph.md:69-70` |
| 6 | frame commit へ昇格 | △ | `docs/design_animation_graph.md:59-64`。previous revision/history がない。 |
| 7 | graph v1 に clock/phase/priority/interrupt/force | △ | `docs/design_animation_graph.md:72-92`。clock の端点と interrupt tick 規則が未完成。 |
| 8 | AnimationSource/slot と sink writer/handoff | ○ | `docs/design_animation_graph.md:94-103`。具体 policy table は A0/A2 条件へ送れる。 |
| 9 | glTF/VRMA semantic→retarget、body/expression/gaze、DCC provenance | △ | pipeline と三 channel は反映 (`docs/design_animation_graph.md:105-123`)。**DCC provenance が脱落**した。 |
| 10 | math/order/loop/live/hash/platform determinism | △ | platform/math/order/version と live bake は反映 (`docs/design_animation_graph.md:124-142`)。§2 #20 の残件がある。 |

## 4. アニメ v2 の条件（WP へ添付可能な粒度）

### 4.1 CA0-ABI: A0 は「fixture で契約を決める」のではなく「公開契約を fixture で検証する」

A0 表は Rig/Pose/interval/blend/commit/phase/G2 ABI を小 fixture で確定するとする (`docs/design_animation_graph.md:144-150`)。しかし Pose の実装形を未決に残す (`docs/design_animation_graph.md:154-157`) ため、このままでは fixture が検査する正本がない。G2 は shared CRT でも ABI version を持つ境界であり (`docs/design_reviews/2026-07-12_wp90_report.md:34-45`)、STL が使えることと additive ABI であることは別である。

**A0 の必須成果物を二つに分けること。**

1. **CA0-Spec（実装なしで凍結可能）**
   - public header に固定幅 opaque handle、invalid 値、generation、`struct_size`、`version`、reserved-zero、status/error を定義する。
   - Rig/PoseLayout/SkinBinding/Clip/Cursor の identity と exact compatibility、stale、destroy/unload の規則を文章化する。
   - frame lifetime、read/write alias、thread ownership、failure atomicity、phase registration/tie-break、commit revision を規範化する。
   - old client/new engine と new client/old engine の descriptor negotiation を決める。
2. **CA0-Probe（最小実装がないと凍結不可）**
   - arena/opaque buffer の acquire→writable view→frame reset、64-byte alignment、二体 parallel、DLL reload/stale generation を実装する。
   - N-way quaternion reference algorithm、wrap/reverse/multi-loop interval、local-to-model、commit 一往復を実装し golden 化する。
   - 外部 game DLL を旧/new header でビルドし、`sizeof/offsetof/struct_size` と unknown tail 無視を検査する。

A0 完了条件は「fixture が green」だけでなく、**凍結 public header と normative semantics が存在し、その header を二世代 fixture が検査すること**とする。

### 4.2 CA0-Interval: additive に育つ最小 C++ 面

次は署名の例であり、名前より ABI 性質を受入条件とする。

```cpp
struct AnimAdvanceDescV1 {
    uint32_t struct_size;
    uint32_t version;          // = 1
    AnimationCursorHandle cursor;
    double delta_seconds;
    uint32_t flags;            // seek/reverse/discontinuity policy
    uint32_t reserved0;
};

struct AnimIntervalResultV1 {
    uint32_t struct_size;      // caller が自分の sizeof を設定
    uint32_t version;
    uint32_t result_flags;     // looped/discontinuous/seeked...
    float normalized_phase;
    RootDeltaV1 root_delta;

    AnimCrossingV1* crossings; // caller-owned、element_size 付き
    uint32_t crossing_capacity;
    uint32_t crossing_count;   // BUFFER_TOO_SMALL 時は required count

    AnimValueV1* values;       // curve/attribute の typed key/value
    uint32_t value_capacity;
    uint32_t value_count;
};

AnimStatus advanceCursor(const AnimAdvanceDescV1*, AnimIntervalResultV1*);
```

必須規則は次である。

- engine は `min(caller.struct_size, known_size)` までしか書かず、unknown tail/flag は version policy に従う。
- crossings/value は `capacity/count` または query→fill の二段式とし、`std::vector` を ABI にしない。
- capacity 不足、stale generation、validation error では **cursor を進めない**。再実行で同じ結果を得る。
- `dt` advance と absolute seek を区別し、forward/reverse、0/1/N loop、端点の半開区間、同時刻の `(source, ordinal)` 順を規範化する。v2 はこれらを要求すると書くが具体規則がない (`docs/design_animation_graph.md:45-48`, `docs/design_animation_graph.md:135-142`)。
- point sample は caller-owned PoseView へ書き、interval result に Pose を混ぜない。

### 4.3 CA2-Interrupt: snapshot は決定性と両立するが、現在の一文では足りない

snapshot 自体は、同一 fixed-step 入力から毎回同じ pose bytes を作るなら決定的である。WP89 も同じ入力を新規 process で二回 replay して capture byte 一致を検証している (`docs/design_reviews/2026-07-12_wp89_report.md:52-63`)。問題は状態を持つことではなく、**どの revision をいつ materialize するかが未定義**なことである。

A2 の受入条件へ以下を添付する。

- 一 tick の transition decision は最大一回。`forceState` と条件遷移は `forceState(sequence) → priority → declaration index` など一意な順を持つ。
- interrupt 判定前に旧 transition の当該 tick pose を一度評価し、その PoseLayout generation と frame revision を付けて snapshot 化する。新 transition の alpha=0 出力は snapshot と byte 一致する。
- snapshot は pose だけを遷移元とし、過去の root delta/event candidate を再 emit しない。将来 inertialization を足す時は velocity/history を別 versioned source state とする。
- 再 interrupt 時は snapshot chain を保持せず、現在出力を一枚に materialize して旧 scratch を解放する。
- `set_time`、replay seek、graph/model reload、layout generation mismatch では reset/reconstruct/error のいずれかを明記する。現行文書は hot reload stale Pose しか規定しない (`docs/design_animation_graph.md:34-37`, `docs/design_animation_graph.md:81-88`)。
- deterministic trace/status に current state、各 cursor、transition progress、snapshot revision/layout と semantic pose hash を出す。pointer/arena offset は hash に含めない。

### 4.4 CA2-Clock: graph v1 の残りの意味論

shared phase と length sync だけではサーベイ §6.1 の条件を満たし切らない。A2 schema/fixture に次を追加する。

- clip speed、negative speed、start offset、loop endpoint、zero-duration clip。
- blend1d の leader 選択と同値 weight tie-break。marker 未対応時は全 clip が shared normalized phase を time range へ写すこと。
- re-enter reset の「同 tick の exit→enter」、`duration:0`、force と interrupt、NaN/Inf parameter error の順。
- parameter namespace は v1 で単一 flat namespace と明記し、nested graph 導入時は version 2 converter が必要と明記する。

### 4.5 CVRM: 4 段 pipeline は最低 4 WP、実質 5 WP

文書は container→semantic channel→retarget profile→AnimationSource の四段を正しく書いた (`docs/design_animation_graph.md:105-123`) が、実装順では「VRM semantic + VRMA retarget」の一塊である (`docs/design_animation_graph.md:150-152`)。現行 `SkeletalModelData` は source path、node TRS、joint mapping、clips しか持たない (`src/core/model/skeletalanimation.hpp:39-45`)。repository の runtime に `VRMC_*` decoder/application はなく、これは一つの中 WP ではない。

推奨分割:

| WP | 範囲 | 最小 gate | 見積感 |
|---|---|---|---|
| VRM-S0 | target `.vrm` semantic decode/storage: humanoid、expression bind、lookAt、constraint metadata、extension version validation | optional/unknown/duplicate bone、node index、round-trip dump fixture | 中〜大 |
| VRM-S1 | per-instance application sink と phase: morph/material/UV expression、gaze/lookAt、constraint hook | 二体で別 expression、同 frame revision、golden | **大**。renderer/material per-instance 化を含むため別 WP 必須 |
| VRMA-C0 | `.vrma` alias、`VRMC_vrm_animation`、unnamed `#animation/0`、body/expression/gaze typed channel | body-only/expression-only/gaze-only/全部なし fixture | 中 |
| VRMA-R0 | versioned retarget/application profile: humanoid map、rest/T-pose normalization、optional bone、hips scale | source/target rig hash、解析 pose、cross-build golden | 大 |
| VRMA-I0 | AnimationSource/graph/timeline 接続、authority、hot reload generation | graph と direct clip の同結果、reload stale/reset | 中 |

SpringBone solver はこの五つへ混ぜず、既存 cloth/secondary motion 設計と phase hook だけ共有する。DCC provenance（source URI/hash、import profile/tool version）も §9 骨子から脱落しているため、VRMA-C0/R0 の Clip/Profile metadata に戻す。

---

## 5. アニメ phase と WP88 velocity/history の不整合

これは v2 で新たに発見した blocking 条件である。

v2 commit は local/model pose、palette、socket、curve、root、event を「同一 frame revision」にする (`docs/design_animation_graph.md:59-68`)。しかし WP88 は previous object/camera history だけを保持し (`docs/design_reviews/2026-07-12_wp88_report.md:19-25`)、現行 skinned velocity shader は **current skin matrix で作った同じ `local_position` を current/previous の両方へ使用**している (`src/core/resources/velocity_skinned.vert:13-19`)。従って actor/camera が静止して骨だけが動く場合、deformation velocity はゼロになる。`PolygonInstanceContainer` にも skin palette は current buffer 一本しかない (`src/core/renderer/polygoninstancecontainer.hpp:33-40`, `src/core/renderer/polygoninstancecontainer.cpp:216-222`)。

CA0/CA1 の commit 契約へ次を追加する。

1. instance は committed animation revision N と N-1 の pose/palette（または previous skinned positions を再構築できる state）を持つ。
2. render/velocity pass は current=N、previous=N-1 を同じ instance identity で読む。animation commit は current を publish するだけで previous を上書きしない。
3. frame の temporal advance は velocity consumer 後に一度だけ行う。現行 object/camera history も render 終端で advance している (`src/core/vkcore/renderer.cpp:792-807`)。
4. 初回、resize、`set_time`、model/rig hot reload、teleport/discontinuity は previous=current として zero velocity に reset する。
5. model reload で instance ID を変える設計にするなら previous history の移送または明示 reset が必要である。

これを入れない場合、§1-4 の「commit」は animation consumer には足りても temporal renderer の commit ではない。名前を `publishAnimationFrame` と `advanceTemporalHistoryAfterRender` に分ける方が誤用を防ぐ。

---

## 6. ホットリロード v1: blocking findings

### HR-B1: `ReadDirectoryChangesW` は関数名だけで設計になっていない

文書は primary/fallback と 200 ms debounce だけを規定する (`docs/design_asset_hot_reload.md:25-36`)。Windows 実装には少なくとも次が必要である。

- directory handle は `FILE_LIST_DIRECTORY`、share read/write/delete、`FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED` で開く。buffer/`OVERLAPPED` は request completion まで生存させ、DWORD alignment を保証する。
- 最初の read で kernel buffer size が handle lifetime 中固定される。overflow は成功 + 0 bytes または `ERROR_NOTIFY_ENUM_DIR` になり、個別 event は全破棄されるため、**必ず subtree inventory rescan**へ落とす。Microsoft 仕様も 0 bytes/`ERROR_NOTIFY_ENUM_DIR` 時の enumeration を要求する。
- network directory では 64 KiB 超 buffer が `ERROR_INVALID_PARAMETER` になる。local/network 共通の既定を 64 KiB 以下とし、overflow は buffer 拡張でなく rescan で正しく回復する。
- rename は OLD/NEW の別 record であり、completion 境界を跨ぐ可能性を pairing 前提にしない。old/new path の両方を dirty として content inventory で確定する。
- completion を処理したら即 re-arm し、hash/debounce は watcher thread 外へ渡す。重い hash 中に read が未発行の窓を作らない。
- `CancelIoEx` は完了を待たない。stop は cancel request→completion (`ERROR_OPERATION_ABORTED` を含む) 回収→buffer/OVERLAPPED 解放→handle close→thread join の順にする。

特に「監視スレッドを止め、イベントを溜めない」という原則 (`docs/design_asset_hot_reload.md:19-21`) は、thread を止めるだけでは成立しない。ReadDirectoryChangesW は同じ handle の呼出し間にも kernel buffer へ変更を蓄積するため、disable 時は **directory handle を閉じる**必要がある。

### HR-B2: WP82 hash の流用可能性は「SHA-256 ライブラリだけ」

WP82 の key は shader source graph、defines、toolchain、target、contract salt の複合 hash である (`docs/design_reviews/2026-07-12_wp82_report.md:13-28`)。実装の `sourceGraphSha256` と `shaderCacheKey` は `shadercompiler.cpp` 内部関数で、file content digest API ではない (`src/core/shader/shadercompiler.cpp:203-215`, `src/core/shader/shadercompiler.cpp:305-328`)。別に assets manifest の `fileSha256` も `.cpp` private である (`src/project/assetsmanifest.cpp:157-164`, `src/project/assetsmanifest.hpp:60-73`)。

従って `design_asset_hot_reload.md:34-36` の「WP82 のハッシュ機構を流用」は現状のままでは偽である。受入条件:

- `ContentDigest` 共通 utility を抽出し、streaming SHA-256、canonical AssetKey、read/share violation、cancel、byte count を API 化する。
- watcher は「最後に**適用成功した** hash」を baseline とする。検知しただけ／失敗した candidate hash を baseline に進めない。
- hash 前後の size/mtime/file identity を比較し、書込み中なら debounce を再開する。安定 read が得られない時は旧 resource 継続 + retry であり、永久失敗にしない。
- editor self-write token は `(AssetKey, expected hash, watcher epoch)` で登録し、一致時だけ consume する。単なる path TTL にしない。

### HR-B3: 「WP62 の ID 参照でモデル再バインド」は根拠が違う

WP62 が generation 化したのは ECS `EntityId` であり、SceneLoader/PhysWorld が Transform を use-time resolve する契約である (`docs/design_reviews/2026-07-10_wp62_report.md:43-54`)。一方 `SimpleModelViewComponent` は `model_name` と世代なし `ModelInstanceId` を保持する (`src/core/userpublic/components/modelview.hpp:10-18`, `src/core/renderer/modelinstance.hpp:7-9`)。更新 system は dirty component の旧 instance を remove し、名前で template を引いて新 instance を place する (`src/core/ecs/predefined/modelviewupdatesystem.cpp:8-20`)。ここに WP62 EntityId は関与しない。

さらに現行 `removeModelInstance` は slot を free list へ返さず行列を identity にするだけで、`placeModelInstance` は vector 末尾へ append する (`src/core/renderer/polygoninstancecontainer.cpp:93-110`, `src/core/renderer/polygoninstancecontainer.cpp:134-146`)。remove→place をリロードごとに行うと 1024 slot 上限へ到達する。従って表のモデル行 (`docs/design_asset_hot_reload.md:47`) は **実装可能ではあるが、現在の前提では実装不能**である。

H2 前提として以下が必要:

- stable `ModelAssetId {index,generation}` と canonical container path + fragment の `AssetKey`。
- generation/free-list 付き `ModelInstanceId`、または同じ instance record/render-command span を in-place rebuild する API。
- `model_name/fragment → AssetKey → live instances` の reverse dependency index。現行 container は constructor-local declaration を捨て、名前→template map しか残さない (`src/core/asset/model.hpp:9-16`, `src/core/asset/model.cpp:17-23`, `src/core/asset/model.cpp:58-61`)。静的 `ModelTemplate` 自身にも source identity はない (`src/core/model/modeltemplate.hpp:11-26`)。
- reload 成功時は ECS EntityId、Transform、physics state を維持しつつ render commands/skinning binding を差し替える。rig layout 変更時は animation cursor/snapshot を generation error→reset する。
- model/fragment 一個の GLB 変更で、その container を参照する全 fragment template を再評価する。

### HR-B4: 「新を構築→検証→差し替え」は現行 model/GPU loader では transaction にならない

CPU `PreparedGltf` はよい分離だが、`GltfLoader::commit` は即座に global Material/VertBuf container へ登録する (`src/core/model/gltf.cpp:1308-1329`)。texture/material 登録は loader 中に行われ (`src/core/model/gltf.cpp:1016-1025`, `src/core/model/gltf.cpp:1083-1099`)、vertex/index buffer は append offset を進める (`src/core/model/vertbufcontainer.cpp:42-76`, `src/core/model/vertbufcontainer.cpp:111-117`)。後段で失敗しても offset/resource を rollback する transaction はない。

したがって `docs/design_asset_hot_reload.md:54-65` の pipeline は次の二段に分ける必要がある。

1. CPU candidate: parse/decode/lower/reflect/全 capability validation。global ID を発行しない。
2. GPU staged commit: candidate resource set を作り、全成功後に logical handle table を atomic swap。旧 set は DeletionQueue へ。途中失敗は candidate set のみ破棄し、global allocator/offset を回復する。

append-only mega-buffer を維持するなら suballocation/free-list/compaction か、reload candidate 専用 buffer ownership が必要である。「失敗時旧継続」は map の代入を遅らせるだけでは満たせない。

### HR-B5: H1 の texture/material values は「低難度」ではない

現行 `MaterialContainer` の public 面は texture/material の `register` だけで replace/update がない (`src/core/material/materialcontainer.hpp:64-83`)。generic `ResourceContainer` も monotonic `reg` と erase/get だけで generation がない (`src/core/resourcecontainer.hpp:8-23`)。texture image を再生成すると、各 material descriptor set が保持する image view を再 bind する必要があり、descriptor は material 登録時に書かれている (`src/core/material/materialcontainer.cpp:389-463`)。material values も登録時に SSBO へ一度 write するだけで update API がない (`src/core/material/materialcontainer.cpp:465-479`)。

従って H1 (`docs/design_asset_hot_reload.md:87-90`) は watcher と同時に resource identity/reverse descriptor dependency/transaction を新設する大 WP になっている。少なくとも watcher 基盤 HR0 と resource replace HR1 を分けること。

### HR-B6: シェーダ hot reload は未実装ではなく、既存経路の移行問題である

文書はシェーダ hot reload を「未実装の候補」とする (`docs/design_asset_hot_reload.md:37-38`) が、現行 renderer は毎 frame `ShaderLibrary::reloadModifiedSources` を呼び、成功時 pipeline を rebuild/rebind する (`src/core/vkcore/renderer.cpp:705-715`)。ShaderLibrary は 1 秒 mtime poll、candidate build 後の bundle swap、失敗時旧 bundle 継続を既に持つ (`src/core/shader/shaderlibrary.cpp:337-393`)。

ただし `.surface` 由来 bundle は source path を空で構築するため (`src/core/shader/shaderlibrary.cpp:293-325`)、この poll 対象外である。また include だけの変更は root `source_path` mtime poll では発火しない。H2 は新規実装でなく、次を行う WP と書き直す。

- 既存 shader poll を FileWatcher/ContentDigest へ移し、二重監視を除去する。
- root/include/virtual source→ShaderBundle/Pipeline/Material の reverse dependency を WP82 source graph と同じ入力集合から記録する。
- `.surface` parse/lower、全 variant compile/reflect、pipeline candidate build を完了してから bundle/pipeline/material layout を一括 swap する。
- compile failure は現在同様に旧 bundle/pipeline を維持し、error log/status だけ更新する。

### HR-B7: 固定の種別順は dependency transaction を代替しない

文書の適用順は texture→values→shader→model→input (`docs/design_asset_hot_reload.md:65-67`) だが、同じ保存操作で `.surface` の params と `.material.json` values が変わる場合、values を旧 layout で先に bind してしまう。GLB model は内部 texture/material/skin を含み、fragment 参照も同じ container に依存するため「種別が浅い順」では表現できない。

ReloadQueue は path extension の FIFO ではなく、`AssetKey` と dependency DAG から **reload transaction group** を作るべきである。group 内は parse all→validate all→stage all→commit topological order とし、surface→material values、container→fragments、texture→descriptors の edge を持つ。無関係 group は個別成功でよい。

### HR-B8: 停止→復帰一括 scan には arm/scan race がある

文書は復帰時一括再 scan とだけ書く (`docs/design_asset_hot_reload.md:19-21`)。先に scan して後から watcher を arm すると、その間の変更を失う。安全な gate protocol は次である。

1. **disable**: gate epoch を増やし、新規 queue を拒否。pending apply を破棄し、全 overlapped I/O を cancel/completion 回収、handle close、thread join。最後に適用成功した content inventory は保持する。
2. **resume/reconcile**: 新 epoch の watcher を **先に arm**。適用はまだ禁止したまま全 store を scan/hash し、保持 inventory と比較する。
3. scan 中に buffer へ来た event を drain し、該当 path を再 hash。hash 前後に変化した file は静穏まで retry する。overflow は scan をもう一周する。
4. delete/rename を含む最終 delta を canonical AssetKey 順に queue し、epoch と gate が不変なら次 frame 先頭から apply。途中で replay/strict/rpc gate が再度立ったら全 candidate を捨てる。
5. `watching=true` は arm 完了でなく reconcile barrier 完了を意味させる。status は `disabled | reconciling | watching | polling | degraded` を返す。

「rpc 駆動中」の開始/終了点、strict gate、queued reload の purge が現行文書では未定義である。WP89 は replay 開始時に既存 shader hot reload flag を false にする (`docs/design_reviews/2026-07-12_wp89_report.md:27-38`)。新 FileWatcher は同じ centralized gate を購読し、各 handler が独自判定して race を作らないこと。

---

## 7. ホットリロード: 非 blocking だが修正必須の指摘

1. `get_status.last_reload_error` (`docs/design_asset_hot_reload.md:16-18`) と `get_status.reload.last_error` (`docs/design_asset_hot_reload.md:69-73`) が不一致。後者へ統一し、watcher error と resource reload error を分ける。
2. project root と mounted store が重なる場合、同じ実 file が二重通知される。canonical store/file identity で dedupe し、reparse point/symlink が store 外へ出る path は既存 PathResolver と同じ escape policy で拒否する。
3. file delete を「load failure」と同一視しない。logical asset が manifest/declaration に残る delete は旧継続 + missing error、declaration 自体の削除は dependency transaction として unbind するなど policy が要る。
4. 200 ms は既定値としてよいが correctness 境界ではない。quiet 後の stable read/retry が正本である (`docs/design_asset_hot_reload.md:32-36`)。
5. model reload 時は WP88 history と animation generation を reset する。単に transform/physics が不変というだけでは temporal ghost と stale palette を防げない (`docs/design_asset_hot_reload.md:47`, `src/core/resources/velocity_skinned.vert:13-19`)。

## 8. ホットリロード: テスト戦略の差し戻し

mock queue 注入は handler の unit test には必要だが、Watcher correctness は mock だけでは検証できない。実 FS end-to-end を texture 一本だけにする方針 (`docs/design_asset_hot_reload.md:75-83`) は Windows primary 実装の最大リスクを全て未検査にする。

HR0 の Windows integration suite（render/GPU 不要）として最低限次を要求する。

- normal modify、create/delete、atomic-save temp→rename、rename OLD/NEW、Unicode/long relative path。
- burst による synthetic 0-byte/`ERROR_NOTIFY_ENUM_DIR` 注入→full rescan→変更全回収。実 overflow storm は環境依存なので deterministic injection と stress の両方を持つ。
- pending overlapped read 中の stop→`CancelIoEx`→join。buffer/OVERLAPPED UAF なし。
- disabled 中の変更、resume の arm→scan 中の再変更、resume 中の再-disable。最終 hash が一度だけ queue される。
- unsupported/failing watch を polling fallback へ落とし、復帰または degraded status を確認。
- overlapping mounted store dedupe、store 外 reparse escape 拒否。
- self-write `(path,hash,epoch)` は一致一回だけ抑制し、異なる外部 hash は抑制しない。

resource handler suite は別に、candidate failure 後の live ID/count/絵不変、1000 回 reload で instance/texture/material/vertex allocation が単調 leak しないこと、GPU in-flight 遅延破棄、model rig layout change の stale animation reset を検査する。

## 9. ホットリロード: 再分割案と再受理条件

現 H1/H2 は Reject する。次の順ならレビュー可能である。

| WP | 内容 | exit gate |
|---|---|---|
| **HR0 Watch/Reconcile** | Win32 overlapped watcher、poll fallback、ContentDigest、inventory、debounce、overflow rescan、gate epoch、resume barrier、ReloadQueue。resource handler は fake のみ | §6 HR-B1/B2/B8 と §8 の非 GPU integration が全 green。既存 shader poll との二重適用なし |
| **HR1 Identity/Transaction** | canonical AssetKey、logical resource generation、reverse dependency DAG、CPU candidate/GPU staged commit、DeletionQueue、status/error schema | candidate failure で global counts/IDs/live bytes 不変。old/new generation と dependency group fixture |
| **HR1-T Texture** | texture replace、same shape upload、shape/format/mip recreate、全 descriptor rebind | same GlobalTexture logical ID、1000 reload leak なし、KTX2 mip/format fixture |
| **HR1-M Values** | `.material.json` 再 parse/lower、同 layout SSBO update、surface layout change は transaction group | WP76 layout fixture 再利用、旧 values 継続、二体/material 単位更新 |
| **HR2-S Surface/Shader** | 既存 ShaderLibrary poll の移行、include/source dependency、全 variant/pipeline transaction | root/include/.surface edit、compile failure 旧絵、cache hit/miss、in-flight old pipeline |
| **HR2-G Model/Fragment** | ModelAsset/Instance generation、fragment reverse index、staged GLTF GPU commit、live instance in-place rebuild、animation/history reset | WP77 fragment 全種、複数 instance、rig change、1000 reload、failure rollback |
| **HR2-I Input** | input_actions/profile parse candidate と frame-boundary swap | held input/consume policy、invalid candidate rollback、gate epoch |

UI は既定どおり U3 transaction に合流してよい。ただし HR0 の `AssetKey/epoch/reconcile` 契約だけは共有し、UI 独自 watcher を作らない。

### 再受理に必要な文書差分

1. §1 を Win32 state machine と overflow/rescan protocol へ置換する。
2. §2 の各 resource に logical identity、reverse dependency、candidate/commit/rollback、generation/history reset を追記する。
3. §3 の固定「種別順」を dependency transaction group に置換する。
4. §5 に platform watcher integration と leak/rollback fixture を追加する。
5. §6 を HR0/HR1/HR2-S/G/I に再分割し、既存 shader reload の移行を明記する。

この五点が入るまでは `design_asset_hot_reload.md` v1 を実装計画の正本にしてはならない。
