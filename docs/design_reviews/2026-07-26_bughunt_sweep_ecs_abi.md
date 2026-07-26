# 類型別バグハントスイープ: ECS / User Public ABI / Animation / Game Logic / App Flow

- 実施日: 2026-07-26
- 対象:
  - `src/core/ecs/`
  - `src/core/userpublic/`
  - `src/core/animation/`
  - `src/core/gamelogic/`
  - `src/core/appflow/`
- 対象規模: 137 files / 22,798 lines
- 方法: 指定された T1〜T17 を、宣言・実装・呼び出し元・兄弟経路・利用ライブラリの実装まで静的に追跡した。
- 制約: 読み取り専用監査。ソース変更、ビルド、テスト、実行による再現は行っていない。
- 集計単位: 同じ根本原因を共有する兄弟箇所は 1 件にまとめた。

## 結果サマリー

確認できた欠陥は 13 件である。

| Severity | 件数 |
|---|---:|
| Critical | 1 |
| High | 3 |
| Medium | 6 |
| Low | 3 |
| 合計 | 13 |

## 類型別集計

| 類型 | 判定 | 件数 | 監査結果 |
|---|---|---:|---|
| T1 ラッパーと呼び出し先の契約不一致 | No match | 0 | 対象内の ABI ラッパー、登録ラッパー、公開薄層を追跡したが、T1 として独立に立証できる不一致はなかった。 |
| T2 保護対象の操作より後にあるガード | No match | 0 | ポインター、サイズ、reserved、handle、generation の検査順を確認したが、該当なし。 |
| T3 早期 return / filter による本来の処理の横取り | No match | 0 | update、reload、event、animation sampling の早期終了を追跡したが、該当なし。 |
| T4 ライブラリ仕様により定数化する述語 | No match | 0 | nlohmann JSON、Vulkan-Hpp、動的ローダーの実装を照合したが、対象内で定数化による死んだ分岐は確認できなかった。 |
| T5 `noexcept` コールバックを囲む `try/catch` | Match | 1 | `Behavior::onDestroy` の外側 catch は例外を捕捉できない。EABI-002。 |
| T6 不変条件上交わらない集合の積 | No match | 0 | owner、registration kind、ECS component set、animation authority の集合条件を確認したが、該当なし。 |
| T7 上流で検証済みのため到達不能な fallback | No match | 0 | descriptor validation、registration validation、asset validation の fallback を追跡したが、該当なし。 |
| T8 `GetLastError` / `dlerror` / `errno` の遅延取得 | Match | 1 | DLL load 失敗後、登録 cleanup を挟んでから loader error を取得する。EABI-005。 |
| T9 cleanup 失敗を握り潰して成功扱い | Match | 1 | reload teardown の失敗が unload を止めず、成功した teardown と同じ制御になる。EABI-001。 |
| T10 reset omission | Match | 1 | reset 時だけ VRMA clip の generation tombstone が欠落する。EABI-011。 |
| T11 部分失敗後のロールバック欠落 | Match | 3 | asset reload、registration publication、animation source ABI の 3 根本原因。EABI-006〜008。 |
| T12 未処理の環境条件 | Match | 1 | Vulkan device lost を分類・回復せず、一般 fatal shutdown にする。EABI-003。 |
| T13 エラー hook の欠落 | No match | 0 | ログ、status、例外の伝播先を確認したが、T13 として独立に立証できる欠落はなかった。 |
| T14 `true` が成功でなく継続を意味する契約の誤読 | No match | 0 | bool callback、reload result、event dispatch、loop continuation を確認したが、該当なし。 |
| T15 全域関数に見える accessor が throw | Match | 1 | `json::value()` が present-but-wrong-type で `type_error` を送出し、公開例外契約を破る。EABI-012。 |
| T16 兄弟経路にだけ guard がない | Match | 3 | generic JSON vector、native quaternion、descriptor status の兄弟差。EABI-004、009、013。 |
| T17 epoch / generation の off-by-one | Match | 1 | ECS change detection が前回 tick と等しい version を新規変更として扱う。EABI-010。 |

追加類型 T18 以降に相当する、既存 17 類型へ収まらない新しい反復パターンは確認しなかった。

## 詳細

### EABI-001 — teardown 失敗後も旧 game-logic DLL を unload する

- 類型: T9
- Severity: Critical
- Confidence: HIGH
- 根拠:
  - `src/core/appflow/teardown.cpp:18` の `cleanupStep` は各 teardown action の全例外を捕捉し、`:21`〜`:29` でログだけを残す。
  - `src/core/appflow/teardown.cpp:98` の production teardown は全 step をこの no-throw helper で実行する。
  - `src/core/appflow/teardown.cpp:109` の `RuntimeTeardownGuard::run()` は先に `completed = true` とし、`:119` で no-throw teardown を呼ぶ。
  - `src/core/gamelogic/gamelogicreload.cpp:440` の reload 用 `runtimeTeardown()` はこの guard を呼ぶだけである。
  - reload transaction は `src/core/gamelogic/gamelogicreload.cpp:270` で teardown 後、`:271`〜`:273` で旧 DLL を unload する。
  - 同ファイル `:318`〜`:323` には「teardown が旧 DLL unload 前に失敗した」rollback 分岐があるが、production teardown step の例外はそこへ到達しない。
- 到達トレース:
  1. reload が `teardown()` を呼ぶ。
  2. `waitIdle`、behavior owner callback、ECS clear 等のいずれかが throw する。
  3. `cleanupStep` がログだけを残し、残りの step と呼び出し元へ通常 return する。
  4. reload transaction は teardown 完了と区別できず、旧 DLL を unload する。
- 実害: ECS entity、behavior instance、deferred callback 等の除去に失敗した状態で、その lifecycle code を所有する DLL が unload され得る。残存ポインターの後続利用だけでなく、cleanup 途中の不整合を抱えたまま新 DLL の構築へ進む。
- 注記: cleanup の継続自体ではなく、reload transaction が失敗を観測できず unload 可否の判断に使えない点が欠陥である。

### EABI-002 — `noexcept onDestroy` の外側 catch は到達不能

- 類型: T5
- Severity: High
- Confidence: HIGH
- 根拠:
  - `src/core/userpublic/behavior.hpp:64` は `Behavior::onDestroy(BehaviorContext &) noexcept` を公開契約にしている。
  - `src/core/gamelogic/behaviorarena.cpp:420`〜`:428` はその呼び出しを `try/catch (...)` で囲み、例外をログして継続しようとしている。
- 到達トレース:
  1. override 内、または override が呼ぶ処理から例外が `onDestroy` の外へ出る。
  2. `noexcept` 境界で `std::terminate` が呼ばれる。
  3. 呼び出し元の `catch (...)` には到達しない。
- 実害: `destroyInstance` は `noexcept` callback の失敗を隔離するように見えるが、実際は teardown / reload 中にプロセスを hard terminate する。`:425` の `"noexcept behavior onDestroy escaped"` 診断も出ない。

### EABI-003 — Vulkan device lost が一般 fatal shutdown に畳み込まれる

- 類型: T12
- Severity: High
- Confidence: HIGH
- 根拠:
  - `src/core/appflow/loop.cpp:309`、`:322`、`:330`、`:413` の render 呼び出しと `:287` の `waitIdle` 周辺に device-lost 分類はない。RenderDoc 用 catch は capture failure の文脈だけである。
  - `src/core/userpublic/pelican_core.cpp:85`〜`:89` は `Loop::run()` と `VulkanManageCore::waitIdle()` を呼ぶ。
  - `src/core/userpublic/pelican_core.cpp:91`〜`:100` は全 `std::exception` を一般の `"Pelican fatal error"` として記録し、runtime teardown 後に `false` を返す。
  - 対象 `src/` に `DeviceLost` の検出分岐はなく、インストール済み Vulkan-Hpp `C:/VulkanSDK/1.4.350.0/Include/vulkan/vulkan.hpp:8522` は `DeviceLostError` を定義し、`:8826` は `eErrorDeviceLost` でそれを throw する。
- 到達トレース:
  1. GPU hang、TDR、driver reset により Vulkan call が `vk::DeviceLostError` を throw する。
  2. `Loop::run()` 内では分類されず `PelicanCore::run()` まで抜ける。
  3. 一般例外として runtime 全体を shutdown し、呼び出し元には通常の失敗 `false` だけを返す。
- 実害: device-lost 固有の回復、再初期化、終了理由の識別がなく、一時的・環境依存の GPU 障害がエンジン一般の fatal error と同じ結果になる。

### EABI-004 — generic JSON vector loader が短い配列を範囲外参照する

- 類型: T16
- Severity: High
- Confidence: HIGH
- 根拠:
  - `src/core/userpublic/serialize/jsonarchive.cpp:39`〜`:54` の `vec2` / `vec3` / `vec4` / `quat` loader は、値が配列か、必要長を持つかを検査せず `dat[0]` 以降を読む。
  - 固定された nlohmann JSON v3.12.0 は `CMakeLists.txt:143`〜`:149` で確認できる。
  - 同版 `json.hpp:2133`〜`:2141` の const numeric `operator[]` は array 型だけを検査し、内部 vector の `operator[]` を使うため、短い配列の index は検査されない。
  - 実使用は `src/core/ecs/predefined/transform.hpp:14`〜`:25` と `src/core/userpublic/components/localtransform.hpp:16`〜`:20` にあり、`src/core/ecs/componentinfo.cpp:109`〜`:115` から scene JSON が generic loader へ入る。
  - 兄弟実装の `src/core/userpublic/components/collider.cpp:13`〜`:25`、`:35`〜`:48` と `src/core/userpublic/components/spriteview.cpp:27`〜`:45` は array 型と厳密な要素数を検査し、`at()` を使う。
- 到達トレース:
  1. transform の `pos`、`rotation`、`scale` に必要要素数未満の JSON array が入る。
  2. component loader が generic `JsonArchiveLoader::prop` を呼ぶ。
  3. const `json::operator[]` から内部 vector の範囲外要素を読む。
- 実害: malformed scene data が通常の検証例外で止まらず、release build では未定義動作、debug 構成では assert 等になり得る。

### EABI-005 — loader error を cleanup 後に取得する

- 類型: T8
- Severity: Medium
- Confidence: HIGH
- 根拠:
  - `src/core/gamelogic/gamelogicreload.cpp:132`〜`:135` が `LoadLibraryW` / `dlopen` を呼ぶ。
  - 失敗時は同ファイル `:137` で `releaseGameLogicRegistrations(owner)` を先に呼び、`:138` で初めて `platformLoadError()` を呼ぶ。
  - `releaseGameLogicRegistrations` は同ファイル `:39`〜`:57` で behavior、animation、render、physics、system、event、component、owner の cleanup を多数実行する。
  - 実際の error-state 読み出しは同ファイル `:71`〜`:78` の `GetLastError()` / `dlerror()` である。
- 到達トレース:
  1. dynamic loader が thread-local error state を設定して失敗する。
  2. 複数の cleanup と user-owned destructor/callback が実行される。
  3. その後に error state を取得する。
- 実害: cleanup 中の OS / loader 呼び出しが error state を上書きまたは消費すると、報告される loader error が本来の DLL load 失敗原因ではなくなる。

### EABI-006 — 失敗した asset reload が VRMA handle generation だけを進める

- 類型: T11
- Severity: Medium
- Confidence: HIGH
- 根拠:
  - `src/core/animation/animationservice.cpp:467`〜`:504` が reload 対象と旧 resource を収集する。
  - 同ファイル `:505`〜`:510` は、replacement の構築前に affected object の VRMA clip generation を increment する。
  - replacement 構築はその後の `:517` で `assets.reloadAsset` を呼ぶ。
  - `src/core/animation/animationjobs.cpp:128`〜`:205` の `buildAsset` は empty nodes、invalid parent、skin binding gap、invalid joint 等で throw する。
  - `src/core/animation/animationjobs.cpp:216`〜`:235` は replacement を先に build し、成功後に generation state と asset map を置換する。
  - `src/core/animation/animationservice.cpp:208`〜`:215` は generation 不一致を `stale_generation` にする。
- 到達トレース:
  1. VRMA clip を持つ object の replacement reload を開始する。
  2. VRMA generation が先に進む。
  3. replacement の構造検証が throw する。
  4. native asset は旧状態のまま残るが、呼び出し元が保持する旧 VRMA handle だけが stale になる。
- 実害: reload の失敗が observable state を変更する。旧 asset を保持して継続できても、既存 VRMA handle は使用不能になる。

### EABI-007 — registration token の公開が例外安全でない

- 類型: T11
- Severity: Medium
- Confidence: HIGH
- 発生条件: 文字列代入または vector 拡張時の allocation failure。
- 根拠:
  - `src/core/userpublic/details/reload/registrationowner.cpp:121`〜`:138` は free token を pop または新規 slot を追加し、`:134`〜`:137` で generation、active、kind、owner を公開した後、`:138` で `slot.name.assign(name)` を行う。代入失敗時の rollback はない。
  - `src/core/userpublic/details/reload/registrationowner.cpp:160`〜`:170` は、この ghost slot も owner の active token として列挙する。
  - `src/core/ecs/componentinfo.cpp:48`〜`:59` は token acquire 後の `infos.resize` (`:51`) を rollback 用 try block の外で実行する。
  - component owner cleanup は `src/core/userpublic/details/component/registerer.cpp:42`〜`:52` で列挙 token を unregister し、対応する component がなければ terminate する。missing token は `src/core/ecs/componentinfo.cpp:64`〜`:72` で throw になる。
  - event owner cleanup も `src/core/userpublic/details/event/registerer.cpp:526`〜`:540` で missing registration を terminate に変える。
- 到達トレース:
  1. token slot が active として owner に結び付けられる。
  2. name copy、または component table resize が throw する。
  3. registration object は公開されないが token slot は active のまま残る。
  4. owner cleanup が ghost token を実 registration として処理する。
- 実害: 本来回収可能な allocation failure が、後続の DLL load cleanup / unload cleanup で `std::terminate` に昇格する。free-token pool との対応も失われる。

### EABI-008 — animation source ABI が `emplace` 前に authority を切り替える

- 類型: T11
- Severity: Medium
- Confidence: HIGH
- 発生条件: map insertion 時の allocation failure。
- 根拠:
  - ABI は `src/core/userpublic/animation/abi_v1.hpp:52`〜`:67` に `Status::out_of_memory` を持ち、同ファイル `:624`〜`:631` で source claim / handoff を status-returning callback として公開する。
  - `src/core/animation/animationservice.cpp:1496`〜`:1519` の `handoffSource` は、`:1515`〜`:1518` で旧 source を inactive/stale にし、sink と output descriptor を新 handle に切り替えた後、`:1519` で `sources.emplace` する。
  - `src/core/animation/animationservice.cpp:1421`〜`:1439` と `:1443`〜`:1471` の claim 兄弟経路も、sink/output を書いた後に `emplace` する。
  - `src/core/animation/animationservice.cpp:918`〜`:947` の sink resolve は `instances.emplace` 後に `sinks.emplace` するため、後段失敗時に片側 map だけが残る。
  - これら callback 本体には `bad_alloc` の status 変換も state rollback もない。一方、同ファイル `:889`〜`:898` の別 ABI entry point は `bad_alloc` を `out_of_memory` に変換している。
- 到達トレース:
  1. valid な source handoff を受理する。
  2. 旧 source generation と sink authority を先に変更する。
  3. 新 source の map insertion が throw する。
  4. status-returning ABI から例外が抜け、旧 authority は復元されない。
- 実害: allocation failure 後、sink が map に存在しない新 source を active として指し、旧 source も stale になる。ABI の `out_of_memory` status で回収できない。

### EABI-009 — native animation quaternion だけ finite / zero-norm guard がない

- 類型: T16
- Severity: Medium
- Confidence: HIGH
- 根拠:
  - `src/core/animation/animationjobs.cpp:83`〜`:103` は keyframe shape だけを検査し、rotation key を `glm::normalize` する。finite / non-zero norm の検査はない。
  - endpoint key も `src/core/animation/animationjobs.cpp:86`〜`:87` で未検査のまま返り、`:295` で再度 normalize される。
  - `src/core/animation/animationjobs.cpp:128`〜`:205` の native asset build も channel value の finite / norm を検査しない。
  - 兄弟経路の VRMA は `src/core/animation/vrmaretarget.cpp:37`〜`:45` で non-finite と zero quaternion を拒否し、`:244`〜`:271` で全 key value と quaternion norm を検証する。
- 到達トレース:
  1. native `SkeletalModelData` に zero または non-finite rotation key が入る。
  2. asset build は受理する。
  3. sampling が無効 quaternion を normalize し、NaN pose を生成する。
- 実害: pose、model matrix、skin palette へ NaN が伝播し得る。VRMA と native で同種データの受理契約が一致しない。

### EABI-010 — ECS change detection が前回 tick と同値を変更扱いする

- 類型: T17
- Severity: Medium
- Confidence: HIGH
- 根拠:
  - `src/core/userpublic/details/ecs/coretemplate.hpp:396` は system ごとに `last_run_tick` を持ち、`:402` は `global_tick` を 1 から開始する。
  - batch path は同ファイル `:498`〜`:505` で `max_version >= start_last_run_tick` を変更ありと判定する。
  - per-chunk path は同ファイル `:541`〜`:543` で `max_version < start_last_run_tick` のときだけ skip する。
  - 実行後は同ファイル `:520`〜`:524` / `:555`〜`:557` で write component を現在の `global_tick` に stamp し、`:561`〜`:563` で `last_run_tick` も同じ tick にする。
  - `src/core/userpublic/details/ecs/coretemplate.cpp:663`〜`:664` は次 frame の update 冒頭で global tick を increment する。
- 到達トレース:
  1. non-force system が tick N で実行し、自己 write component version と `last_run_tick` をともに N にする。
  2. tick N+1 で外部変更がなくても `max_version == last_run_tick` である。
  3. `>=` 判定により再実行され、再び component version と `last_run_tick` を同値にする。
  4. 以後毎 frame 繰り返す。
- 実害: public API で登録した change-driven write system が、最初の実行後は変更がなくても恒久的に実行される。コメント `no changes since last run` とも一致しない。
- 注記: 現在の predefined systems が force-update でも、公開されている non-force 登録経路の欠陥は消えない。

### EABI-011 — reset が VRMA clip generation tombstone を残さない

- 類型: T10
- Severity: Low
- Confidence: HIGH
- 根拠:
  - `src/core/animation/animationserviceabi.hpp:38`〜`:47` は、reload/reset 境界で stale handle が invalid handle に劣化しないため generation tombstone を保持すると明記する。
  - `src/core/animation/animationservice.cpp:628`〜`:640` は rig、layout、native clip、binding を tombstone 化する。
  - 同ファイル `:659` は `vrma_clips` を tombstone 化せず clear する。
  - VRMA 登録は同ファイル `:251`〜`:275` で ledger に identity を記録しない。
  - reset 後、`getClipMetadata` は同ファイル `:1087`〜`:1088` で VRMA map entry がある場合だけ VRMA resolver を使い、なければ `:1140`〜`:1143` の native resolver へ落ちる。
  - ledger miss は `src/core/animation/animationserviceabi.cpp:23`〜`:28` で `invalid_handle` になる。
- 到達トレース:
  1. caller が valid な VRMA `ClipHandle` を保持する。
  2. runtime reset が `vrma_clips` を clear する。
  3. caller が旧 handle の metadata を問い合わせる。
  4. identity の tombstone がなく `invalid_handle` になる。
- 実害: native clip は `stale_generation`、VRMA clip は `invalid_handle` となり、明記された reset 境界の handle 診断契約が種類によって分裂する。

### EABI-012 — animation graph parser の公開例外契約を `json::value()` が破る

- 類型: T15
- Severity: Low
- Confidence: HIGH
- 根拠:
  - `src/core/userpublic/animation/animgraph.hpp:61`〜`:64` は malformed document で `std::runtime_error` を throw すると明記する。
  - `src/core/userpublic/animation/animgraph.cpp:132`〜`:136` の JSON exception catch は parse 呼び出しだけを覆う。
  - その後、同ファイル `:141` の `root.value("schema", std::string{})` と `:210` の `value("interrupt", std::string{"never"})` は型 guard なしで呼ばれる。
  - 固定版 nlohmann JSON v3.12.0 の `json.hpp:2253`〜`:2268` は key が存在すると `get<ValueType>()` を呼び、型不一致で throw する。
  - 同版 `detail/exceptions.hpp:50` は JSON exception を `std::exception` から派生させ、`:237` は `type_error` を JSON exception から派生させる。`std::runtime_error` ではない。
- 到達トレース:
  1. JSON 自体は構文的に valid だが、`"schema": 7` または `"interrupt": false` を含む。
  2. parse 用 catch を正常通過する。
  3. `value<string>` の `get<string>()` が `nlohmann::json::type_error` を throw する。
  4. 公開 API から `std::runtime_error` でない例外が出る。
- 実害: caller が明記された例外型だけを処理している場合、malformed input の通常エラーを捕捉できない。

### EABI-013 — null pointer/非ゼロ size が reserved-field error に分類される

- 類型: T16
- Severity: Low
- Confidence: HIGH
- 根拠:
  - `src/core/userpublic/animation/abi_v1.hpp:52`〜`:67` は `invalid_argument` と `reserved_not_zero` を別 status にしている。
  - `src/core/animation/animationservice.cpp:918`〜`:923` の `resolveSink` は null pointer と non-zero size の組み合わせを `invalid_argument` にする。
  - 兄弟経路の `resolveClip` は同ファイル `:994`〜`:998` で `reserved2 != 0` と null `clip_name` / non-zero size を同じ条件にまとめ、両方を `reserved_not_zero` にする。
  - `src/core/animation/vrmapplication.cpp:736`〜`:743` の expression `queryWeight` も reserved field と null `name` / non-zero size をまとめて `reserved_not_zero` にする。
- 到達トレース:
  1. reserved fields はすべて zero の descriptor に、`name == nullptr && name_size != 0` を渡す。
  2. descriptor header validation は通過する。
  3. pointer/size guard が `reserved_not_zero` を返す。
- 実害: caller は reserved-field 初期化不良と pointer/size 不良を区別できず、同一 ABI 内の兄弟 API で同じ malformed descriptor の status が一致しない。

## 読み取りファイル

### 監査対象（全 137 files）

```text
src/core/animation/animationjobs.cpp
src/core/animation/animationjobs.hpp
src/core/animation/animationprobe.cpp
src/core/animation/animationprobe.hpp
src/core/animation/animationservice.cpp
src/core/animation/animationservice.hpp
src/core/animation/animationserviceabi.cpp
src/core/animation/animationserviceabi.hpp
src/core/animation/CMakeLists.txt
src/core/animation/vrmapplication.cpp
src/core/animation/vrmapplication.hpp
src/core/animation/vrmaretarget.cpp
src/core/animation/vrmaretarget.hpp
src/core/appflow/CMakeLists.txt
src/core/appflow/enginetime.cpp
src/core/appflow/enginetime.hpp
src/core/appflow/framephase.cpp
src/core/appflow/framephase.hpp
src/core/appflow/framerate.cpp
src/core/appflow/framerate.hpp
src/core/appflow/loop.cpp
src/core/appflow/loop.hpp
src/core/appflow/teardown.cpp
src/core/appflow/teardown.hpp
src/core/ecs/archetypemigration.cpp
src/core/ecs/archetypemigration.hpp
src/core/ecs/benchmark.cpp
src/core/ecs/CMakeLists.txt
src/core/ecs/component.hpp
src/core/ecs/componentinfo.cpp
src/core/ecs/componentinfo.hpp
src/core/ecs/core.hpp
src/core/ecs/entity.hpp
src/core/ecs/predefined.cpp
src/core/ecs/predefined.hpp
src/core/ecs/predefined/animationsystem.cpp
src/core/ecs/predefined/animationsystem.hpp
src/core/ecs/predefined/camera.hpp
src/core/ecs/predefined/camerasystem.cpp
src/core/ecs/predefined/camerasystem.hpp
src/core/ecs/predefined/CMakeLists.txt
src/core/ecs/predefined/localtransformsystem.cpp
src/core/ecs/predefined/localtransformsystem.hpp
src/core/ecs/predefined/modelview.cpp
src/core/ecs/predefined/modelview.hpp
src/core/ecs/predefined/modelviewtransformsystem.cpp
src/core/ecs/predefined/modelviewtransoformsystem.hpp
src/core/ecs/predefined/modelviewupdatesystem.cpp
src/core/ecs/predefined/modelviewupdatesystem.hpp
src/core/ecs/predefined/spriteviewsystem.cpp
src/core/ecs/predefined/spriteviewsystem.hpp
src/core/ecs/predefined/transform.hpp
src/core/gamelogic/behaviorarena.cpp
src/core/gamelogic/behaviorarena.hpp
src/core/gamelogic/CMakeLists.txt
src/core/gamelogic/gamelogicreload.cpp
src/core/gamelogic/gamelogicreload.hpp
src/core/userpublic/animation/abi_v1.hpp
src/core/userpublic/animation/animgraph.cpp
src/core/userpublic/animation/animgraph.hpp
src/core/userpublic/animation/pose_staging_v1.hpp
src/core/userpublic/animation/vrm_application_v1.hpp
src/core/userpublic/behavior.hpp
src/core/userpublic/cameracontrollersystem.cpp
src/core/userpublic/CMakeLists.txt
src/core/userpublic/color.hpp
src/core/userpublic/components/animation.hpp
src/core/userpublic/components/CMakeLists.txt
src/core/userpublic/components/collider.cpp
src/core/userpublic/components/collider.hpp
src/core/userpublic/components/localtransform.hpp
src/core/userpublic/components/modelview.hpp
src/core/userpublic/components/predefined.hpp
src/core/userpublic/components/spriteview.cpp
src/core/userpublic/components/spriteview.hpp
src/core/userpublic/details/behavior/CMakeLists.txt
src/core/userpublic/details/behavior/registerer.cpp
src/core/userpublic/details/behavior/registerer.hpp
src/core/userpublic/details/component/CMakeLists.txt
src/core/userpublic/details/component/registerer.cpp
src/core/userpublic/details/component/registerer.hpp
src/core/userpublic/details/ecs/chunk.cpp
src/core/userpublic/details/ecs/chunk.hpp
src/core/userpublic/details/ecs/CMakeLists.txt
src/core/userpublic/details/ecs/component.hpp
src/core/userpublic/details/ecs/componentdeclare.hpp
src/core/userpublic/details/ecs/coretemplate.cpp
src/core/userpublic/details/ecs/coretemplate.hpp
src/core/userpublic/details/ecs/entity.hpp
src/core/userpublic/details/event/CMakeLists.txt
src/core/userpublic/details/event/payloadschema.hpp
src/core/userpublic/details/event/registerer.cpp
src/core/userpublic/details/event/registerer.hpp
src/core/userpublic/details/reload/registrationowner.cpp
src/core/userpublic/details/reload/registrationowner.hpp
src/core/userpublic/details/schema/structfieldjson.hpp
src/core/userpublic/details/schema/structfieldschema.hpp
src/core/userpublic/details/system/CMakeLists.txt
src/core/userpublic/details/system/registerer.cpp
src/core/userpublic/details/system/registerer.hpp
src/core/userpublic/deterministicrng.cpp
src/core/userpublic/deterministicrng.hpp
src/core/userpublic/events.hpp
src/core/userpublic/export.hpp
src/core/userpublic/gamecontext.cpp
src/core/userpublic/gamecontext.hpp
src/core/userpublic/gamelogic.hpp
src/core/userpublic/gameobjects.cpp
src/core/userpublic/gameobjects.hpp
src/core/userpublic/gamesystem.hpp
src/core/userpublic/geom/quat.hpp
src/core/userpublic/geom/vec.hpp
src/core/userpublic/pelican_core.cpp
src/core/userpublic/pelican_core.hpp
src/core/userpublic/physics/abi_v1.hpp
src/core/userpublic/physics/abi_v2.hpp
src/core/userpublic/platformer/charactercontroller2d.cpp
src/core/userpublic/platformer/charactercontroller2d.hpp
src/core/userpublic/render/draw_sort_abi_v1.hpp
src/core/userpublic/render/graph_transform_abi_v1.hpp
src/core/userpublic/render/pass_implementation_abi_v1.hpp
src/core/userpublic/render/render_strategy_abi_v1.hpp
src/core/userpublic/render/subgraph_replacement_abi_v1.hpp
src/core/userpublic/serialize/binaryarchive.hpp
src/core/userpublic/serialize/CMakeLists.txt
src/core/userpublic/serialize/jsonarchive.cpp
src/core/userpublic/serialize/jsonarchive.hpp
src/core/userpublic/serialize/serialize.hpp
src/core/userpublic/sprite/flipbook.cpp
src/core/userpublic/sprite/flipbook.hpp
src/core/userpublic/sprite/pixelpolicy.cpp
src/core/userpublic/sprite/pixelpolicy.hpp
src/core/userpublic/sprite/sampler.hpp
src/core/userpublic/sprite/spriteworld.cpp
src/core/userpublic/sprite/spriteworld.hpp
src/core/userpublic/userinput.cpp
src/core/userpublic/userinput.hpp
```

### 指示・補助根拠として読み取ったファイル

```text
%LOCALAPPDATA%/Temp/claude/<scratch>/2b01b5c1-9872-4511-acc7-5eb40780aa54/scratchpad/goals/bughunt_sweep_goal.md
docs/design_reviews/2026-07-26_bughunt_findings_raw.md
CMakeLists.txt
C:/src/pelican2/build-off-openxr/_deps/nlohmann-src/include/nlohmann/json.hpp
C:/src/pelican2/build-off-openxr/_deps/nlohmann-src/include/nlohmann/detail/exceptions.hpp
C:/VulkanSDK/1.4.350.0/Include/vulkan/vulkan.hpp
```
