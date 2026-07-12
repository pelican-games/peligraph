# Pelican Animation ABI v1 規範

日付: 2026-07-12

状態: **Frozen / normative**

正本 header: `src/core/userpublic/animation/abi_v1.hpp`

本書の「しなければならない」「禁止」は ABI v1 の規範である。A0 probe はこの契約を
検査するための最小実装であり、既存 WP38 の sampler/描画経路を置換しない。

## 1. ABI と descriptor negotiation

1. DLL 境界へ STL、例外、所有権付き C++ object を出さない。整数は固定幅、handle は
   `identity:uint64 + generation:uint32 + reserved:uint32` の 16 byte standard-layout 値とする。
   invalid handle は全 field が 0。valid handle は identity/generation が非 0、reserved が 0 である。
2. 全 in/out descriptor は offset 0 から `struct_size, version, reserved0, reserved1` をこの順で持つ。
   caller は自身の `sizeof`、version 1、reserved zero を設定する。要素配列は各要素にも
   `element_size, version` を持つ。
3. engine は `min(caller.struct_size, known_size)` より後を読み書きしない。v1 の unknown tail は無視し、
   unknown flag bit、non-zero reserved、必須 prefix 不足は拒否する。拒否時は `Status` を返し例外を越境させない。
4. old client/new engine は version 1 と旧 `struct_size` で v1 prefix を取得できる。new client/old engine はまず
   新しい ABI version を照会し、`unsupported_version` なら version 1 と old engine が返した `struct_size` へ
   fallback する。必須 function pointer/capability が prefix に無い場合だけ feature unavailable とする。
5. `getApiV1` は caller tail を保存し、engine の known size、対応最小 client version、capability、function table を返す。
   ABI major を暗黙に downcast してはならない。

## 2. identity、互換性、破棄

- `RigHandle` は parent/rest local TRS/name/semantic を含む animation hierarchy の identity であり、skin joint 集合の
  identity ではない。`PoseLayoutHandle` は rig node の個数・順序・channel layout の identity である。
- Pose の exact compatibility は `PoseLayoutHandle` の identity と generation が双方一致する場合だけ成立する。
  同じ node 数、同じ名前集合、同じ byte size は互換性の根拠にならない。
- `SkinBindingHandle` は rig-node→palette-joint 写像と inverse bind の独立 identity/generation を持つ。さらに source
  rig identity と mesh/skin identity を検査する。Pose generation の一致で SkinBinding 検査を代用してはならない。
- `ClipHandle` は model から独立し source rig を参照する。`CursorHandle` は clip、sampling context、annotation table の
  generation に拘束される。別 clip への付替えは禁止し、新 cursor を生成する。
- unload/reload/destroy は、参照メモリを解放する前に対象 generation を進める。同じ identity の古い handle は
  `stale_generation` となり、別 resource を指してはならない。identity は同一 process epoch 内で別 resource に再利用しない。
  destroy の最初の成功後、同じ handle の destroy/利用は `stale_generation`。全 zero は `invalid_handle` である。
- game DLL reload は登録 owner の全 cursor/arena/phase registration generation を失効させてから DLL を閉じる。
  新 DLL が同じ address や ordinal を得ても旧 handle は復活しない。

## 3. Pose arena、寿命、alias、thread ownership

ABI v1 の Pose 実装形は **engine-owned scratch arena + opaque `PoseHandle` + caller-writable `PoseViewV1`** に凍結する。
個別 Pose の release API はない。

1. owner thread が frame ごとに `beginFrame(frame_revision)` し、arena offset を reset して generation を進める。
   acquire は translations/rotations/scales の各先頭を 64-byte aligned にし、rest 初期値
   (T=0、R=identity、S=1)を返す。
2. view と pose handle は次の frame reset、layout/rig reload、owner DLL unload の最も早い時点までだけ有効。
   reset 後の access は禁止し、handle を engine API に渡すと `stale_generation` になる。
3. 同一 arena から別 acquire した writable range は重ならない。異なる actor の Pose は alias しない。同じ PoseView を
   明示的に複製した値だけは同じ range の alias であり、同時 write は caller の data race となる。
4. arena は作成/登録した thread が所有する。owner 以外の begin/acquire は `wrong_thread`。並列 actor 評価は thread ごとに
   独立 arena を使い、commit で合流する。engine は API call 終了後に caller pointer を保持しない。
5. capacity/validation 失敗時は arena allocation cursor と out view を変更しない。

## 4. point sample と interval advance

`samplePoseAt(clip,t)` は一点を caller-owned PoseView へ書く point operation であり、event/root/curve crossing を返さない。
`advanceCursor` は区間 operation であり、Pose を結果へ含めない。

### 4.1 可変長出力と失敗原子性

- crossing/value は caller-owned array と `capacity/count` を使う。query は pointer null/capacity 0 で行える。
- required count が capacity を超えた場合は `buffer_too_small` と required count を返す。配列は書かず、cursor、root accumulator、
  sampling context を一切進めない。caller は必要量を確保して同じ入力を再実行すると同じ順序・内容を得る。
- stale generation、reserved/flag/version、非 finite 数、その他 validation error でも cursor は不変。成功時だけ cursor と全 sideband を
  一括 publish する。失敗時に変更してよい out field は required count と診断 field だけである。

### 4.2 時間と端点

- dt advance は current cursor time から signed `delta_seconds` を進む。正が forward、負が reverse、0 は空区間である。
- repeat clip の canonical cursor time は `[start,end)`。forward は **(old,new]**、reverse は **[new,old)** を列挙する。
  したがって往復で同じ境界を二重 emit しない。0/1/N loop を unwrapped time 上で列挙し、結果の loop index を保持する。
- traversal time が異なる crossing は forward で昇順、reverse で降順。同時刻は再生方向によらず
  `(source, ordinal)` の昇順である。source/ordinal は annotation table 内で安定かつ一意でなければならない。
- clamp clip は範囲外を端へ clamp し、端到達後に同じ annotation を再 emit しない。duration <= 0 は validation error。
- `advance_absolute_seek` は absolute time を canonical range へ写し、crossing/value を emitせず root delta を identity にし、
  `seeked|discontinuous` を返す。seek に dt を混用しない。通常 advance の root delta は走査した signed interval に対応する。

## 5. Clip metadata

A0 で列挙を凍結する metadata は次のとおり。

- source rig identity/generation
- time range と wrap mode
- channel inventory: translation、rotation、scale、morph weight、scalar/vector curve、attribute
- annotation table identity/generation、および event/marker の element version
- sampling context generation と cursor generation

未知 channel kind は v1 inventory mask で黙って解釈せず、version/capability negotiation 後にだけ利用する。

## 6. N-way quaternion と blend layer

normal blend の quaternion reference algorithm は次で固定する。

1. 入力順は descriptor 配列順。非 finite または 0 以下の weight は 0 と扱う。
2. 最初の正 weight quaternion を hemisphere reference とし、各入力との dot が負ならその入力の符号を反転する。
3. x/y/z/w を入力順に double で weighted accumulate し、Euclidean norm で一度 normalize、float に丸める。
4. 最終符号は w、z、y、x の順に最初の非 zero 成分が正になるよう canonicalize する。
5. 正 weight がない、または長さが `1e-12` 以下/非 finite の場合は identity quaternion。fixture 許容誤差は各成分 `1e-6`。

`BlendLayerV1` は scalar weight、per-joint weight、normal/additive、reference pose、rest fallback、root/curve/event-marker
policy を持つ。additive space は **local / model / mesh** を区別し、暗黙変換しない。model/mesh additive は必要な
local↔model 変換を明示してから評価する。v1 の event/marker policy は `suppress`、`dominant_weight`、`all` のいずれかを
明示しなければならず、未初期化既定値に依存しない。同 weight の dominant は layer 配列の早い方で tie-break する。

## 7. local-to-model

hierarchy は parent-before-child 順、root parent は -1 とする。local matrix は column-major `T * R * S`、model は
root で local、child で `parent_model * local`。cycle、自己/後方 parent、layout count 不一致では output 全体を publish せず
validation error とする。PoseLayout identity が異なる input/output の変換は禁止する。

## 8. phase registration と failure atomicity

phase の engine-owned 順序は次である。

1. parameter snapshot
2. base pose + root modifier evaluation
3. movement/root resolution
4. world-dependent post-process
5. commit

同 phase 内は `(priority, registration_identity, source_ordinal)` 昇順。完全同一 key の二重登録は error とし、登録時刻や
thread scheduling を tie-break にしない。phase callback 失敗時はその instance の frame commit を publish せず、直前の
committed revisionを維持する。標準 evaluator も同じ registration API を使い、特権順序を持たない。

## 9. commit revision と temporal history

`publishAnimationFrame` は local/model Pose、palette、socket/attachment model transform、morph/expression/gaze curve、
root delta、event candidate を一つの非 zero・単調増加 `frame_revision` として instance identity へ publish する。部分 publish と
同 revision の再 commit は禁止する。consumer は同じ revision の組だけを読む。

temporal renderer との契約は以下の 6 項である。

1. instance は committed revision N と N-1 の pose/palette、または previous skinned position を復元できる state を保持する。
2. velocity pass は同一 instance identity から current=N、previous=N-1 を読む。publish は current だけを交換し previous を上書きしない。
3. `advanceTemporalHistoryAfterRender` は velocity consumer 完了後、frame に一度だけ呼び、そこで current を previous へ進める。
4. 初回、resize、`set_time`、model/rig reload、teleport/discontinuity は previous=current として zero velocity に reset する。
5. model reload で instance identity が変わる場合、history を明示移送するか reset する。無関係な新 instance に旧 history を継承しない。
6. 評価側操作名は `publishAnimationFrame`、render 終端操作名は `advanceTemporalHistoryAfterRender` とし、一つの `commit` に統合しない。

destroy/unload、capacity不足、revision逆行、velocity 未消費、二重 temporal advance の失敗では current/previous の双方を変更しない。
