# エディタツーリング v2 + object behavior v2 再レビュー

日付: 2026-07-18  
対象 commit: `4ac2ff086c4c60c514328e58fb226be87d4a5c5b`  
対象: `docs/design_editor_tooling.md` v2、`docs/design_object_behaviors.md` v2  
方法: 前回レビューの E-1〜E-7 / B-1〜B-7、§4 の逐語条件、§5 の最小提出物と、現行 C++ の静的照合。依頼どおりビルド・実行は行っていない。

## 0. 結論

| 文書 | 判定 | 結論 |
|---|---|---|
| `docs/design_editor_tooling.md` v2 | **条件付き** | 正本を `AuthoringSceneDocument`、runtime を nullable projection とした決定、codec 五つ組、二段 edit 応答、transaction journal、全 document 保存は E-1〜E-7 の方式上の blocker を解消した (`docs/design_editor_tooling.md:15-59`, `docs/design_editor_tooling.md:79-130`, `docs/design_editor_tooling.md:150-163`)。ただし、document と ECS/Light/Phys/renderer を合成する projection transaction protocol がなく、「runtime apply NG なら rollback」は現行 API からは導けない。また transform の local edit を同一 frame の world 観測へ反映する規範、前回逐語の ECS-MUT0 と競合 fixture、七種の canonical JSON 例、共通 schema WP の所有者が欠ける。ED-AUTH0 だけは §7 の境界で単独着地可。その他の edit 系 WP は E-C1〜E-C5 を添付してから開始すること。 |
| `docs/design_object_behaviors.md` v2 | **条件付き** | attachment arena/special binder、typed event、activation barrier、`attachment_seq`、owner-safe G2、AuthoringSceneDocument だけを reload 復元元にする決定により、B-1〜B-7 の中核は解消した (`docs/design_object_behaviors.md:16-63`, `docs/design_object_behaviors.md:82-154`)。ただし StructFieldSchema 統合時に既存 event の「bool 拒否・全 required」を維持する use-site policy がなく、§8-3 の「schema_version が変わる reload 時のみ recreate」は同文書の reload 列および WP90 full-reset と矛盾する。BehaviorSystem の実 order/name と schema 非互換 reload gate も未確定である。B-C1〜B-C3 を WP に添付する条件で受理する。 |

これは v1 の Reject を維持する判定ではない。正本と lifecycle の方式は実装へ進める水準まで改められた。以下は、その方式を failure-atomicity・既存 event 互換・reload safety の実装可能な契約に閉じるための受け入れ条件である。

## 1. 前回 blocker の照合

### 1.1 editor E-1〜E-7

| ID | v2 反映 | 照合結果 |
|---|---|---|
| E-1 codec/serializer | **反映、提出物一部不足** | loader を serializer とみなす記述を撤回し、`decode / encode / schema / runtime apply / runtime project` と七種 coverage を採った (`docs/design_editor_tooling.md:48-71`)。ただし前回 §5 が要求した七種の canonical JSON 例はなく、transform の mapping も E-C2 の意味論が必要。 |
| E-2 identity | **反映** | 全 object の `AuthoringObjectId`、`SceneRevision`、nullable `EntityId` を明記した (`docs/design_editor_tooling.md:26-37`, `docs/design_editor_tooling.md:81-87`)。無名・light-only・collider-only を EntityId で代用する穴は閉じた。 |
| E-3 同期 RPC/deadlock | **方式は反映、gate 欠落** | enqueue 時は ticket、commit 後は `step_frame.edit_results[]` / `get_edit_result(ticket)` とした (`docs/design_editor_tooling.md:91-102`)。ただし前回逐語にあった RPC/windowed phase trace、stale revision、scene transition、callback/reload 競合 fixture が脱落している (`docs/design_reviews/2026-07-17_editor_behavior_review_codex.md:205-207`)。 |
| E-4 API 共有 | **反映** | JSON/RPC handler の外に typed `EditorCommandService` を置き、RPC/ImGui fake adapter の等価性を gate にした (`docs/design_editor_tooling.md:79-87`, `docs/design_editor_tooling.md:135-143`)。 |
| E-5 ReloadGate | **反映** | `ReloadGate::enabled()` の流用を禁止し、`can_edit` を独立 query とし、`rpc_driver` を許可した (`docs/design_editor_tooling.md:93-98`)。 |
| E-6 journal | **反映、実行 transaction は未閉包** | revision、ordered forward/inverse、affected closure、配列位置、parent edge、coalescing を採った (`docs/design_editor_tooling.md:108-120`)。journal の形は閉じたが、その forward を document と全 runtime adapter へ原子的に適用する protocol は E-C1 が必要。 |
| E-7 Save/data loss | **反映** | runtime ECS の列挙を禁止し、全 envelope/scene/raw component、digest、temporary、parse validation、atomic replace、cache/revision の失敗時不変を採った (`docs/design_editor_tooling.md:150-163`)。scene HR を暗黙に期待する記述もない。 |

### 1.2 behavior B-1〜B-7

| ID | v2 反映 | 照合結果 |
|---|---|---|
| B-1 duplicate component/stable name | **反映** | behavior は ECS component でなく special binder + attachment arena とし、反復・同 type 複数・behavior-only object を許可した。永続名も C++ 綴りから分離した (`docs/design_object_behaviors.md:16-20`, `docs/design_object_behaviors.md:39-60`)。 |
| B-2 params schema | **方式は反映、event 互換未規範** | generic `StructFieldSchema`、Bool/Enum/default、temporary decode、stable error path、共通 descriptor を採った (`docs/design_object_behaviors.md:65-78`)。ただし既存 EventPayloadSchema の policy を分離しておらず B-C1 が必要。 |
| B-3 EventView | **反映** | EventView を撤回し、matching typed overload だけを既存 E1 の order 内で配送する (`docs/design_object_behaviors.md:82-91`)。 |
| B-4 onInit/structural mutation | **反映** | 全 publish 後の activation、resolve 可能な pre-destroy、逆順 destroy、callback 中 mutation の defer、frame-start snapshot を明記した (`docs/design_object_behaviors.md:93-112`)。 |
| B-5 order/EntityId reuse | **大半反映、定数未確定** | `attachment_seq` を scene/runtime transaction の tuple から作り、EntityId/storage 順を禁止した (`docs/design_object_behaviors.md:114-124`)。ただし `BehaviorSystem の固定 system order/name tie-break を文書化` は将来形で、実際の literal order/name と undo restore 時の seq 規則がない。B-C3 が必要。 |
| B-6 G2 candidate/owner | **反映** | 登録は宣言のみ、candidate 中 instance/onInit 禁止、owner 別 destroy/purge と全 failure/unload branch の unregister を BEH0 に戻した (`docs/design_object_behaviors.md:43-47`, `docs/design_object_behaviors.md:128-142`)。 |
| B-7 reload state | **反映。ただし §8-3 と矛盾** | AuthoringSceneDocument commit 済みの値だけを復元し、runtime/unsaved params/internal state は消えると訂正した (`docs/design_object_behaviors.md:144-154`)。一方、§8-3 は recreation を schema_version 変更時だけに限定しており、B-C2 が必要 (`docs/design_object_behaviors.md:182-188`)。 |

## 2. editor の追加判断

### 2.1 E-C1: document + runtime projection は現状の文面だけでは failure-atomic でない

v2 は staged document や adapter の prepare/publish protocol を定義せず、`preflight` 後に document と runtime adapter へ commitし、runtime apply 失敗なら両方 rollback とだけ書く (`docs/design_editor_tooling.md:99-102`, `docs/design_editor_tooling.md:122-130`)。

現行 ECS の entity batch create は、ID table/free-list/chunk を snapshot し、例外時に deinit・tail rollback・cache rebuild まで行う強い transaction である (`src/core/userpublic/details/ecs/coretemplate.cpp:93-148`, `src/core/userpublic/details/ecs/coretemplate.cpp:212-263`)。しかし、これは全 projection の transaction ではない。

- 既存 component の `setComponent` は live object へ先に copy-assign し、その後 version を進めるだけで、旧値または rollback token を持たない (`src/core/userpublic/details/ecs/coretemplate.hpp:123-131`)。
- `LightContainer::load` は live vector/map を先に clear し、その後の JSON decode/name 登録が throw し得る (`src/core/light/lightcontainer.cpp:146-202`)。個別 setter も直接代入である (`src/core/light/lightcontainer.cpp:243-280`)。
- `PhysWorld` の公開面は clear/bind/query で、既存 binding の transactional update/remove API がない (`src/core/phys/physworld.hpp:36-76`)。
- transform setter は LocalTransform と world Transform を順に mutate し、さらに renderer instance を別呼出しで更新する (`src/core/userpublic/gameobjects.cpp:38-56`)。

従って ECS-MUT0 を作っても、それが保証するのは archetype migration であり、set-existing、Light、Phys、renderer、camera、behavior attachment まで自動的に原子的にはならない。「document を元へ戻す」だけでは runtime の部分変更が残る。

次を **E-PROJTX0（E-RPC1 の先行 WP）** として逐語添付すること。

> EditorProjectionTransaction は immutable な base AuthoringSceneDocument と staged next document を持ち、全 ordered command を検証してから、対象となる ECS existing-value/archetype、transform descendant closure、renderer/model、camera、LightContainer、PhysWorld、behavior attachment の各 adapter に `prepare` を行う。`prepare` は live state を変更せず、全 allocation/decode/validation と publish 後状態を構築し、失敗時は staged document と全 prepared state を破棄する。全 prepare 成功後の frame-boundary publish は observer が走らない区間で行い、各 adapter の publish を `noexcept` swap/handle publication にする。staging できない adapter は最初の live mutation より前に完全な inverse token を作り、rollback を `noexcept` とする。AuthoringSceneDocument と SceneRevision は runtime publish 成功後に no-throw swap し、途中 revision を公開しない。ECS-MUT0 は generic add/remove migration だけを提供し、この aggregate transaction の代用としない。
>
> fault injection は各 adapter の prepare 点と、inverse-token 方式を使う各 publish 点に置く。失敗後に base document semantic equality、SceneRevision、runtime query、EntityId/free-list/component version、Light/Phys binding、renderer handle/値、behavior lifecycle trace が実行前と一致し、journal 追記がなく ticket が stable error code 付き `failed` になることを gate とする。同じ transaction の複数 command および ECS + special adapter 混在を必須 fixture とする。

この条件なら document rollback は「公開済み document を戻す」のではなく、基本的には staged document を公開しないことで達成できる。runtime 側も prepare→no-throw publish に寄せるため、異種 subsystem を best-effort compensation でつなぐ設計にならない。

### 2.2 E-C2: transform の local/world mapping は階層 edit まで閉じていない

scene v1 の既定は parent 使用時の authored transform が local TRS、world が `parent_world × child_local` である (`docs/design_scene_format.md:80-85`)。現行 `LocalTransformSystem` の正確な式は scale の成分積、rotation の積、`parent_pos + parent_rotation * (parent_scale * local_pos)` である (`src/core/ecs/predefined/localtransformsystem.cpp:26-43`)。

v2 の coverage 表は `authored=local / runtime=world` としながら、「runtime project は階層合成の逆算」と一行で済ませる (`docs/design_editor_tooling.md:63-66`)。これは次を決めない。

1. local field edit が `LocalTransformComponent` を更新するのか、world `TransformComponent` を更新するのか。
2. commit 点は `freeze_events` 前だが、現行 world 合成は `deliver_events` 後の ECS update で初めて走るため、同 frame の event callback が旧 world を見る問題 (`src/core/appflow/framephase.cpp:90-128`)。
3. reparent の preserve-local / preserve-world の既定、zero scale で inverse が存在しない場合、cycle、descendant closure の更新。
4. `runtime_json` が local 値、world 値、または両方のどれか。runtime world の表示を authored local へ暗黙 write-back するか否か。

次を ED-CODEC0 と E-PROJTX0 に逐語添付すること。

> transform の authored_json は常に local TRS を正本とする。`set_component_value(..., transform, ...)` は staged document と runtime `LocalTransformComponent` を更新し、現行 LocalTransformSystem と同じ式で対象 object および全 descendant の world `TransformComponent` を commit 区間内に再計算してから event/update/query の observer を再開する。runtime_json は `{local_trs, world_trs}` を区別して返し、world は表示用 projection であって AuthoringSceneDocument へ自動反映しない。
>
> reparent command は `preserve: "local" | "world"` を必須とし、既定値に依存しない。`local` は authored local TRS を不変、`world` は reparent 前 world を現行合成式の逆算で新 local へ変換する。parent scale のいずれかが 0、非有限値、循環、または canonical TRS へ表現不能なら preflight で stable error code と authoring object path を返し、何も変更しない。forward/inverse/forward は対象と全 descendant の local/world query equality を検査する。root、二階層以上、回転 + non-uniform scale、zero scale reject を fixture に含める。

### 2.3 E-C3: 「レビュー逐語」とされているが脱落した条件がある

次は骨抜きにしてはならない。

1. **ECS-MUT0**: v2 は一行の要約しかなく (`docs/design_editor_tooling.md:172-178`)、前回の fault/rollback coverage を本文または WP gate に転記していない。次をそのまま復元する。

   > live entity の component add/remove に必要な archetype migration を ECS transaction として実装する。重複/EntityId remove/不存在、construct-populate-init fault、deinit/destroy、over-alignment、move-only、system chunk cache/version、free-list/ID、external binding の rollback を WP62 と同じ failure-atomicity で検査する。special attachment(light/collider/behavior)は generic ECS migration と同一視せず adapter を持つ。E-RPC1 はこの WP 完了前に add/remove_component を約束しない。

2. **E-RPC1 競合 gate**: v2 §1-2 の末尾へ次を復元する。

   > RPC/windowed の同一 phase trace、stale revision、scene transition 競合、callback/reload 競合を fixture に含める。

3. **ED-CODEC0 最小提出物**: coverage 表だけでなく、transform、simplemodelview、camera、light、collider、animation、sprite_view の各一件以上について authored input、canonical output、fresh load 後の semantic assertion を示す。transform は parent-child、animation は JSON bool、sprite_view は `flip` 配列と文字列 `billboard` を必須例にする。前回 §5 は coverage 表と canonical JSON 例の両方を要求していた (`docs/design_reviews/2026-07-17_editor_behavior_review_codex.md:256-265`)。

### 2.4 E-C4: edit operation set と WP scope を復元する

v2 §1-2 は ticket/commit 規範と add/remove の延期だけを記し、E-RPC1 が `set_component_value / add / remove / spawn / destroy / reparent` のどこまでを同時に提供するかを再掲しない (`docs/design_editor_tooling.md:91-106`)。全面改稿文書なので、v1 から暗黙継承してはならない。

> E-RPC1 の WP 表に operation matrix を置き、各 operation について authoring forward/inverse、runtime adapter、必要先行 WP、preflight error、activation/deactivation boundary を列挙する。full v1 edit 群を一 WP で約束するなら E-RPC1 は ECS-MUT0 と E-PROJTX0 の完了後とする。add/remove を後続へ分けるなら RPC method を未提供として stable `method_unavailable` を返し、journal schema だけを先に予約する。「実装はないが一部だけ document に適用」は禁止する。

### 2.5 E-C5: Save の「同一 transaction」は no-throw publication で具体化する

atomic file replace と複数の in-memory object は OS 上の一個の atomic primitiveにはならない。v2 の failure rule (`docs/design_editor_tooling.md:128-130`, `docs/design_editor_tooling.md:152-163`)を実装可能にするため、SAVE0 に次を添付する。

> encode 済み bytes、parse validation 結果、次 cache object、次 AuthoringSceneDocument metadata/revision を file replace 前にすべて構築する。replace 後の cache/document 公開は allocation・decode・I/O を行わない `noexcept` swap とし、その間 reader を入れない。replace 前 failure は旧 file/cache/revision、replace 後は新 file/cache/revisionだけが観測可能で、中間の組合せを観測させない。各 prepare 点と replace 直前を fault injection し、新規 process reloadも gate に含める。

## 3. behavior の追加判断

### 3.1 B-C1: StructFieldSchema は use-site policy を分けないと既存 event を変える

v2 は Bool/default/optional を持つ generic schema を event/component/behavior/inspector の三役に使う (`docs/design_object_behaviors.md:65-78`)。共通 descriptor 自体は正しい。しかし現行 event 契約は意図的に異なる。

- event の型集合に Bool はなく、unsupported type は compile-time error である (`src/core/userpublic/details/event/payloadschema.hpp:75-108`)。bool compile-fail fixture も存在する (`test/fixtures/event_payload_schema/fail_unsupported_type.cpp:3-6`)。
- `EventPayloadSchema` に required/default field はなく、validator は全 descriptor field の存在を要求する (`src/core/userpublic/details/event/payloadschema.hpp:42-56`, `src/core/userpublic/details/event/registerer.cpp:205-228`)。
- unknown field は先に reject し、その後 missing/type/range を検査する現在の error precedence も既存 observable behavior である (`src/core/userpublic/details/event/registerer.cpp:205-228`)。

generic enum に Bool と default を足し、既存 `field<&T::member>` が無条件にそれを受理すると、既存の compile-fail と missing-field error が変わる。次を共通 schema の先行 WP に逐語添付すること。

> StructFieldSchema は field descriptor の型情報を共通化するが、decode/validation は `EventPayload`、`BehaviorParams`、`Component` の明示 use-site policy を必須とする。EventPayload policy は既存どおり Bool/Enum/default/optional を許可せず全 field required、unknown-field reject、現在の validation/error precedence、`load_json_payload` 呼出回数を不変にする。BehaviorParams policy だけが Bool/Enum/default と存在 key のみの atomic apply を許可する。Component policy は codec ごとに required/default を明示する。同じ field 宣言 primitiveを使っても、policy の省略または暗黙 default は compile error とする。
>
> WP71 の全既存 runtime/compile-fail fixtureを無変更で通し、bool event compile-fail、missing field、unknown+missing 同時入力の error code優先順位、typed/payloadless/opaque分類、descriptor/ref一致検査、payload load call count が refactor 前と一致する regression gateを追加する。既存 event JSON の受理/拒否集合を一件も広げず狭めない。

### 3.2 B-C2: `schema_version が変わる reload 時のみ recreate` は成立しない

同文書の BEH1 成功列は、旧 instance を destroyし、旧 DLLを unloadし、新 DLL/scene rebuild後に新 instanceを onInitする (`docs/design_object_behaviors.md:128-137`)。さらに WP90 full-reset を維持すると明記する (`docs/design_object_behaviors.md:144-150`)。現行 G2 も teardown→old unload→new load→rebuild の順である (`src/core/gamelogic/gamelogicreload.cpp:205-244`)。teardown は ECS/physics/model instance を clear する (`src/core/appflow/teardown.cpp:31-42`)。

従って schema_version が同じコード-only reload でも、旧 DLL の vtable/function pointer を保持した instance は unload 前に必ず destroyし、新世代 factoryで recreateしなければならない。逆に BEH2 の同一 generation params edit は schema_version と無関係に atomic live applyであり、recreateしない。§8-3 (`docs/design_object_behaviors.md:182-188`) は両方と矛盾する。

§8-3 と BEH1 に次を逐語添付すること。

> accepted game-logic DLL generation が変わる reload は schema_version の一致/不一致にかかわらず、全旧 behavior instance を onDestroy/destroyしてから旧 owner/DLLを unloadし、scene rebuild後に新 factoryで全 instanceを recreate/onInitする。同一 DLL generation 内の BEH2 set-param は atomic live applyとし recreateしない。schema_version は recreate の trigger ではなく params schema evolution の検証 key とする。
>
> candidate の stable_name が同じで schema descriptor fingerprint が変わったのに schema_version が同じ場合は `schema_changed_without_version_bump` で旧 runtimeを維持する。version が変わった場合、migration WP がない v1 では全 AuthoringSceneDocument raw paramsを candidate schemaへ side-decodeし、全件成功時だけ通常 reloadを許可し、一件でも失敗すれば名前/version/field path入り `schema_incompatible` で旧 DLL/runtimeを維持する。scene record は attachment schema versionを保存していないため、cross-process migrationを提供すると主張しない。将来 migrationを行う場合は persisted source versionとversioned migration chainを別WPで追加する。
>
> 同 version・同 schema の code-only reloadでも destroy/recreate が各一回、same version schema drift reject、version bump + decode成功、version bump + decode失敗 rollback、type削除 hard error、candidate validate中 lifecycle trace 0を二世代 fixtureにする。

### 3.3 B-C3: BEH0/BEH1 の残りの逐語条件を実値と gate にする

BEH0 は `BehaviorSystem の固定 system order/name tie-break を文書化` とだけ書き、値を固定していない (`docs/design_object_behaviors.md:114-124`)。現行 game system は `(order, name)` で event と update の双方を sortする (`src/core/userpublic/details/system/registerer.hpp:26-31`, `src/core/userpublic/details/system/registerer.cpp:23-53`)。ここは実装が偶然選んだ値を後から追認してはならない。

> BEH0 本文と registration fixtureに `BehaviorSystem` の literal `order` と stable `name` を記載し、同 order の user systemとの name tie-breakを含む event/update total orderを expected traceで固定する。scene attachmentの undo restoreは元 `attachment_seq` を復元し、redoも同じ seqを使う。runtimeで新規 attachしたものだけが新しい `(commit_seq, command_index, attachment_index)` を得る。forward→inverse→forwardで attachment seq列と lifecycle/event/update traceを一致させる。

また、BEH1 の表は pending復旧・queued event purge・onInit faultを挙げるが、前回逐語の「新 DLL で type削除/schema非互換」fixtureを全部は残していない (`docs/design_object_behaviors.md:174-180`, `docs/design_reviews/2026-07-17_editor_behavior_review_codex.md:239-243`)。B-C2 の type削除/schema drift/version bump fixtureを BEH1 gateへ明記すること。

## 4. §4 逐語条件と §5 最小提出物の監査

### 4.1 §4 editor 条件

| 条件 | 状態 |
|---|---|
| ED-AUTH0 | **同値に反映** (`docs/design_editor_tooling.md:26-37`) |
| ED-CODEC0 | **本文は同値、canonical 例と hierarchy 意味論不足** (`docs/design_editor_tooling.md:48-75`) |
| E-RPC0 | **実質反映**。WP62 は完了済み前提なので依存欄からの省略は blocker としない (`docs/design_editor_tooling.md:79-87`) |
| ECS-MUT0 | **未転記**。要約一行では逐語条件の fault matrix を代替しない (`docs/design_editor_tooling.md:172-178`) |
| E-RPC1 | **末尾 fixture が脱落** (`docs/design_editor_tooling.md:91-102`) |
| JOURNAL0 | **同値に反映** (`docs/design_editor_tooling.md:108-120`) |
| UI-INS0/UI-AB0 | **同値に反映** (`docs/design_editor_tooling.md:135-148`) |
| SAVE0 | **同値に反映、publication 具体化は E-C5** (`docs/design_editor_tooling.md:150-163`) |

### 4.2 §4 behavior 条件

| 条件 | 状態 |
|---|---|
| BEH-P0 | **本文は反映、既存 event policy の後方互換は未規範** (`docs/design_object_behaviors.md:65-78`) |
| BEH0 | **大半を分散反映**。fixed order/name の実値と undo seq restore が不足 (`docs/design_object_behaviors.md:43-47`, `docs/design_object_behaviors.md:49-124`, `docs/design_object_behaviors.md:139-142`) |
| BEH1 | **大半を反映**。schema/type evolution fixtureと §8-3 整合が不足 (`docs/design_object_behaviors.md:128-154`, `docs/design_object_behaviors.md:174-188`) |
| BEH2 | **atomic live applyを選択して反映**。ただし recreate の記述は B-C2 に置換必要 (`docs/design_object_behaviors.md:156-165`) |

### 4.3 §5 最小提出物

| 提出物 | 状態 |
|---|---|
| AuthoringSceneDocument 状態図 | **あり** (`docs/design_editor_tooling.md:39-46`) |
| 七種 codec coverage + canonical JSON 例 | **coverage はあり、例なし** (`docs/design_editor_tooling.md:61-71`) |
| edit/journal/Save 失敗遷移 | **概略あり、異種 projection の prepare/publish/rollback なし** (`docs/design_editor_tooling.md:108-130`) |
| behavior lowering/barrier/typed dispatch 擬似コード | **あり** (`docs/design_object_behaviors.md:49-63`, `docs/design_object_behaviors.md:82-112`) |
| G2 candidate/success/rollback/pending trace | **あり** (`docs/design_object_behaviors.md:126-154`) |
| WP 表 + 条件対応表 | **あり。ただし hidden schema dependency と逐語脱落あり** (`docs/design_editor_tooling.md:165-180`, `docs/design_object_behaviors.md:167-180`) |

## 5. StructFieldSchema の後方互換判断

結論は **共通 descriptor への抽出は可能だが、現在の v2 のままでは後方互換を証明できない** である。

共通化するのは member pointer、field name、scalar kind、range、unit、canonical field orderまでに留める。Bool/Enum、required/default、decode、encode は use-site profileが決める。とくに EventPayload profile は現行 `PayloadFieldType` 集合と全 requiredを凍結し、BehaviorParams profileだけを additiveに拡張する。これなら「field macroを三系統作らない」と「既存 event の挙動不変」を両立できる。B-C1 の regression gateを通さずに `payloadschema.hpp` の public surfaceを置換してはならない。

## 6. WP 依存グラフの判断

現行二文書は ED-CODEC0 と BEH-P0 が「同じ StructFieldSchema」を所有すると書く一方、両者を相互に依存しない先行 WP として並べる (`docs/design_editor_tooling.md:73-75`, `docs/design_editor_tooling.md:165-180`, `docs/design_object_behaviors.md:167-179`)。これは同一 public API を二 WP が並行設計する hidden dependency である。また `ECS-MUT0 + E-RPC1/JOURNAL0` の `+` は prerequisite か並行 sibling か読めない。

共通 schema と projection transaction を独立させ、依存を次で固定する。

```text
WP71(済)                              → STRUCT-SCHEMA0
STRUCT-SCHEMA0                       → ED-CODEC0
STRUCT-SCHEMA0                       → BEH-P0
ED-AUTH0 + ED-CODEC0                 → E-RPC0
WP62(済)                              → ECS-MUT0
ED-AUTH0 + ED-CODEC0 + ECS-MUT0      → E-PROJTX0
E-RPC0 + E-PROJTX0                   → E-RPC1/JOURNAL0
E-RPC0                               → UI-AB0
E-RPC0 + E-RPC1/JOURNAL0             → UI-INS0
ED-AUTH0 + E-RPC1/JOURNAL0           → SAVE0
ED-AUTH0 + BEH-P0                     → BEH0 → BEH1
BEH0 + E-RPC1/JOURNAL0               → BEH2
```

- `STRUCT-SCHEMA0`: B-C1 の generic descriptor + 三 use-site policy。BEH-P0 から共通部を分離する。
- `BEH-P0`: BehaviorParams の default construct/partial atomic decode/canonical encode。
- `ED-CODEC0`: component profileと七種 codec。共通 schema API を新設しない。
- `E-PROJTX0`: E-C1 の異種 projection 合成。ECS-MUT0 はその generic ECS migration participant。
- `E-RPC1/JOURNAL0`: full edit operation setを約束するなら E-PROJTX0 完了後。部分版にするなら E-C4 の operation matrixで明示分割する。

## 7. 最初の ED-AUTH0 は単独で着地できるか

**できる。ただし scope を次に固定することが条件である。** 現在の §0-1 は raw document、revision/object identity、cache統合、lossless fixtureだけで自己完結しており、codec や edit RPC を本質的に必要としない (`docs/design_editor_tooling.md:26-37`)。

ED-AUTH0 に次を逐語添付する。

> ED-AUTH0 は scene v1 bytesの parse、全 envelope/scene/object/component raw JSON保持、SceneRevision/AuthoringObjectId割当、ProjectBasicConfig cacheとの単一 read/update/invalidate面、deterministic semantic encode fixtureまでを所有する。`query` はテスト用の raw authoring traversalを意味し、EditorCommandService/RPC schema、component codec、runtime edit、journal、Save file replaceを含めない。runtime bindは既存 SceneLoaderへ同じ semantic JSONを供給するだけとし、未編集 load、scene切替、G2 currentScene rebuildの観測挙動を変えない。既存全 scene fixtureに加え、複数scene、無名、light-only、collider-only、unknown/read-only componentを含む未編集 documentの load→encode→fresh load semantic equalityをgateとする。

この境界なら ED-AUTH0 は `STRUCT-SCHEMA0` / ED-CODEC0 / E-RPC0 より先に mergeでき、後続は stable authoring identity と cache authorityへ依存できる。反対に ED-AUTH0 へ codec-driven runtime projectionや外部 file replaceを混ぜると単独着地性を失う。

## 8. 最終判定と着手可否

- **ED-AUTH0**: §7 を WP に添付すれば単独で着手・着地可。
- **STRUCT-SCHEMA0 / BEH-P0**: B-C1 と既存 WP71 regression gateを添付後に着手可。
- **ED-CODEC0**: STRUCT-SCHEMA0 完了後、E-C2/E-C3 の canonical例を gateにして着手可。
- **ECS-MUT0**: E-C3 の前回逐語条件を復元後に着手可。
- **E-PROJTX0 / E-RPC1 / JOURNAL0**: E-C1/E-C2/E-C4 を設計・WPへ添付するまで着手不可。
- **SAVE0**: E-C5 を添付し、ED-AUTH0 と edit authority の公開順が固定された後に着手可。
- **BEH0 / BEH1 / BEH2**: B-C1〜B-C3、特に §8-3 の置換を添付するまで着手不可。

両文書の判定は以上の条件付き Accept とする。条件は機能の追加要求ではなく、v2 が既に主張する failure-atomicity、既存 event 不変、G2 owner safetyを実際に成立させるための境界と gateの具体化である。
