# 第9章 黒魔術・制約・変更時の注意

[索引へ戻る](README.md) / [前章](08_class_interface_index.md)

ここでいう「黒魔術」は、コードが不可解という意味ではありません。macro、template、型消去、static 初期化、GPU 非同期寿命など、実際の処理が呼び出し箇所から離れて見える実装を指します。この章では、展開後に何が起きるかと、調査時点での制約を明示します。

## 9.1 `DECLARE_MODULE` / `GET_MODULE`: 小さな service locator

定義は [`container.hpp`](../../src/core/container.hpp#L15-L16) です。

```cpp
#define DECLARE_MODULE(name) class name : public ModuleBase<name>
#define GET_MODULE(name) FastModuleContainer::get<name>()
```

`DECLARE_MODULE(Foo)` は概念的に次を足します。

```cpp
class Foo : public ModuleBase<Foo> { ... };

// ModuleBase<Foo> の中
static std::optional<Foo>& __get() {
    static std::optional<Foo> obj;
    return obj;
}
```

`GET_MODULE(Foo)` の初回は `optional.emplace()` で default constructor を呼び、型と cleanup function を global `cleaners` stack へ積みます。2回目以降は同じ object reference を返します。[`FastModuleContainer::get()`](../../src/core/container.hpp#L149) が全処理です。

### 破棄順

[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L46) の先頭で local `FastModuleContainer` を作ります。その destructor は、初期化と逆順に module の `optional.reset()` を行います。

```text
GET_MODULE(A) -> A constructor内でGET_MODULE(B)
初期化: Aのemplace開始 -> B登録 -> A登録
cleaners: [B, A]
破棄: A -> B
```

依存を constructor 内で取得すれば、通常は dependent が先に壊れます。ただし GPU/ECS は destructor だけへ任せず、[`RuntimeTeardownGuard`](../../src/core/appflow/teardown.cpp#L145) が明示 cleanup を先に行います。

> 🧩 **難所 — 遅延生成の 49 行**([`FastModuleContainer::get<T>()`](../../src/core/container.hpp#L149))
>
> **何をする所か**: module の遅延生成です。初回だけ default 構築して cleaner を積み、2 回目以降は同じ参照を返します。
>
> **素朴に読むと**: この 49 行には独立した仕掛けが 5 つ同居していて、どれか 1 つを知らないと「なぜこの順序なのか」が読めません。速い経路が lock を取らずに `__ready()` を読む double-checked locking(二重チェックロック — lock を取らずにフラグを読み、まだ初期化前に見えたときだけ lock を取って**もう一度**確かめる方式)なので、**公開の [`store(release)`](../../src/core/container.hpp#L192) は `emplace` と `cleaners.push_back` の両方が終わった後の 1 点だけ**です。この 1 点には理由が 2 つ重なっています。第 1 に**書く順序**で、`ready` を先に立てると「ready なのに cleaner が無い module」ができ、teardown で破棄されずに漏れます。第 2 に**スレッド間の可視性**で、生成は owner スレッド限定でも読み取りは他スレッドから自由なため([`test` 内](../../test/module_container_test.cpp#L147))、`release` で書き `acquire`([`isInitialized()` 側](../../src/core/container.hpp#L141) / [`tryGet()` 側](../../src/core/container.hpp#L145) / [`get()` の速い経路](../../src/core/container.hpp#L151))で読む対にして「`ready` が true に見えたスレッドからは、その前に済ませた `emplace` も必ず見える」を保証します。`relaxed` へ緩めると、`ready` だけが先に見えて未構築の optional を掴み得ます。[`state_mutex`](../../src/core/container.hpp#L62) が `recursive_mutex` なのは、`obj_ref.emplace()` が走らせる `T` のコンストラクタの中で `GET_MODULE(U)` が呼ばれ、同じスレッドが `get()` へ再入するからで、ただの `mutex` なら自己デッドロックします。`ConstructionScope`([`ConstructionScope`](../../src/core/container.hpp#L174))を `construction_stack.push_back` の直後・`emplace()` の直前に置くのは、コンストラクタが throw しても必ず pop させるためで、**宣言位置そのものが意味を持ちます**。依存辺の記録を自分を積む前に行うのは `construction_stack.back()` を「親」にするため、循環検出を `requireCreationAllowedLocked()`([`requireCreationAllowedLocked()`](../../src/core/container.hpp#L98))より先に置くのは freeze 済みでも「循環」という正しい診断を出すためです。
>
> **骨子**:
> ```text
> [fast] ready.load(acquire) なら 依存辺を記録して返す    ← 読み取りは lock なし(辺を記録する時だけ lock)
> [slow] scoped_lock(recursive_mutex) → ready を再確認
>        依存辺を記録 → construction_stack に自分がいれば循環エラー
>        shutdown中 / freeze済み / owner スレッド違い → エラー
>        stack.push(id) + ConstructionScope(RAII pop)
>        obj_ref.emplace()                          ← ここで再入しうる
>        try { cleaners.push(id, destroyModule<T>) } catch { pop; reset(); rethrow }
>        ready.store(true, release)                 ← 公開はここ 1 点
> ```
>
> **手がかり**: 失敗ロールバック([catch 節](../../src/core/container.hpp#L187))は `cleaners.back().id == id` を確認してから pop し `obj_ref.reset()` します。`ready` はまだ false なので、外からは一度も見えていません。[`destroyModule<T>`](../../src/core/container.hpp#L116) は `ready=false` → `reset()` の順で、逆にすると `tryGet` が破棄済み optional へのポインタを配ります。テストは [`module_container_test.cpp` 内](../../test/module_container_test.cpp#L114)(依存記録と逆順破棄)/ [`module construction failures and cycles never publish partial modules`](../../test/module_container_test.cpp#L132)(失敗と循環で部分公開しない)/ [`module creation is owner-thread-only while initialized access remains available`](../../test/module_container_test.cpp#L147)(生成は owner スレッド限定、読み取りは自由)。
>
> **不変条件**: `ready.store(true)` は「emplace 済み かつ cleaner 登録済み」の後だけ。`state_mutex` は再帰的でなければなりません。`tryGet` は lock を取らないので、破棄と並行に読めば dangling になり得ます(生存期間の規律だけで守っています)。

### 注意点

- dependency は constructor 本体の `GET_MODULE` に隠れます。include graph だけでは実行時依存が分かりません。
- `cleaners` と `optional.emplace/reset` は現在 [`std::recursive_mutex state_mutex`](../../src/core/container.hpp#L62) で保護されています(執筆時点の「mutex なし」は失効)。ただし複数 runtime 同時実行を想定しない設計自体は変わりません。
- **[`FastModuleContainer::freezeCreation()`](../../src/core/container.hpp#L199)**(Loop 開始直前、[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L378) で呼ばれる)以降は新規 module 生成が禁止されます。「render 中に初めて `GET_MODULE` する」コードは freeze 後に失敗するため、`Renderer::prepareRuntimeModules()` / `prepareFrameStateModules()` のように起動時に依存を先解決するパターンが必須です。生成しない読み取りには [`tryGet<T>()`](../../src/core/container.hpp#L144) があります。shutdown 側には `beginShutdown()`(#L213)が加わりました。
- module reference/pointer は `PelicanCore::run()` の外へ保持してはいけません。container destructor 後は無効です。
- destructor から新しい `GET_MODULE` を呼ぶと、teardown 中に module を再生成し得ます。destructor は既に所有する dependency を使うか、明示 teardown で完結させる方が安全です。
- `get()` は dependency injection seam ではありません。pure algorithm をテストしたい場合は、frame planner のように module から切り離した free function/value 層を作る設計が合います。

## 9.2 typed handle は何を守り、何を守らないか

[`PELICAN_DEFINE_HANDLE`](../../src/core/handle.hpp#L21) は `struct PassId : BasicHandle<PassId,int>` のような薄い型を作ります。`PassId` と `RenderingPassId` の取り違えを compile 時に防ぎつつ、内部値は整数1個です。

一方、一般的な [`ResourceContainer`](../../src/core/resourcecontainer.hpp#L13) は monotonic counter と `unordered_map` だけです。

- handle は再利用しません。
- `unreg()` 後の handle に generation はありません。
- stale handle の `get()` は `unordered_map::at()` 例外になります。
- counter overflow の明示検査はありません。
- `BasicHandle` は base 整数への暗黙 conversion を持つため、logging/indexing は楽ですが、整数へ落とした後の型安全性は失われます。

> 🧩 **難所 — handle の CRTP と穴**([`BasicHandle`](../../src/core/handle.hpp#L9) / [`PELICAN_DEFINE_HANDLE`](../../src/core/handle.hpp#L21))
>
> **何をする所か**: `struct PassId : BasicHandle<PassId, int> {};` の 1 行で、整数 1 個分の強い型を作ります。
>
> **素朴に読むと**: まず、なぜ CRTP なのかがコードから読めません。`T` は本体で 1 回しか使われず、[`struct Hash { size_t operator()(T key) const ... }`](../../src/core/handle.hpp#L15) だけです — つまり CRTP は**派生型を引数に取る `Hash` を基底の中で定義するため**だけにあり、`unordered_map<PassId, V, PassId::Hash>` が書けるのは、この CRTP の定義があるからです。次に、型安全が片側だけであることが読み取れません。[`operator Base()`](../../src/core/handle.hpp#L11) の暗黙変換があるので、**異なる handle 型どうしの `==` はコンパイルが通ります** — メンバの `operator==` は候補集合には入るものの右辺を変換できず viable になりませんが、組み込みの `Base == Base`(この例なら `int == int`)が両辺のユーザー定義変換を経て候補になるためです。さらに `value` に既定メンバ初期化子が無いので `H h;` は不定値、`H h{};` がゼロで、[`invalidBehaviorAttachmentHandle{}`](../../src/core/userpublic/behavior.hpp#L15) がわざわざ `{}` なのはそのためです。「一度包めば全部守られる」と読むと、この 3 点を取り違えます。
>
> **骨子**:
> ```text
> 守る:     H1 h = uint64_t;  /  takesH1(h2)    → コンパイルエラー
> 守らない: h1 == h2(別 handle 型)             → 組み込み == 経由で通る
>          takesUint64(h1)                      → 暗黙変換で通る
>          H h;(未初期化)                       → 警告のみ
> ```
>
> **手がかり**: 基底クラス持ちの集成体なので `A a{{1}}` も `A a{1}`(brace elision — 集成体初期化で入れ子の内側の `{}` を省略してよい、という規則)も通ります。リテラルからの生成に書き方が 2 通りあるように見えるのはこれが理由です。animation ABI 側の [`PELICAN_ANIM_HANDLE`](../../src/core/userpublic/animation/abi_v1.hpp#L25) は名前が似ているだけの別物で、C ABI 用の 16 バイト POD です(暗黙変換も比較演算子も持ちません)。
>
> **不変条件**: `operator Base()` を消すと logging/indexing の呼び出し側が広範囲に壊れるので、当面は「別 handle 型の `==` は通る」を前提に読んでください。`Hash` は派生型を受けるため、`BasicHandle` を非 CRTP に書き換えると連想コンテナが全部壊れます。

世代付きの識別子は増えました。ECS の `EntityId`(index + generation)に加え、[`ModelInstanceId`](../../src/core/renderer/modelinstance.hpp#L11) が `index + generation + scene_epoch` になり(WP146 / INSTANCE0)、[`RegistrationToken`](../../src/core/userpublic/details/reload/registrationowner.hpp#L30) と `RegistrationOwner` も identity + generation です。

**それでも `ResourceContainer` 系の GPU ハンドルだけは依然として世代なし** です。GPU resource handle と、世代付きの Entity / ModelInstance / Registration handle を同じ寿命モデルだと考えないでください。

> 🧩 **難所 — generation は 0 を跨がない**([`ResourceGenerationLedger`](../../src/core/animation/animationserviceabi.hpp#L40) / [`isValid()`](../../src/core/userpublic/animation/abi_v1.hpp#L48))
>
> **何をする所か**: animation ABI ハンドルの世代を進めます。`if (++x == 0) ++x;` という同じ 3 語が [`animationservice.cpp`](../../src/core/animation/animationservice.cpp#L635) だけで 7 か所に現れます。
>
> **素朴に読むと**: `isValid()` が `identity != 0 && generation != 0 && reserved == 0` で判定するので、**generation 0 は「無効ハンドル」の予約値**です。`uint32` の世代が一周して 0 に戻ると、生きているリソースのハンドルが突然「無効」になる — `if (++x == 0) ++x;` はその 1 値だけを飛ばすイディオムですが、1 行に畳まれているので「なぜ 2 回インクリメントするのか」がコードからは読めません。もう 1 段深いのが `ResourceGenerationLedger` で、リソースが消えたあとも **identity を消さずに世代だけ覚え続けます**([`tombstone()`](../../src/core/animation/animationserviceabi.cpp#L36) は世代を 1 進めて `remember()` する)。これが無いと消えたリソースのハンドルは「そんな identity は知らない」= `invalid_handle` に落ちますが、`invalid_handle` は「そのハンドルは初めからおかしい」、`stale_generation` は「正しかったが古い」で、**呼び出し側の回復経路が違います**。tombstone を外すと、モデルの hot reload のたびに [`EvaluatorV1::rebind()`](../../src/core/userpublic/animation/animgraph.cpp#L909) での再解決ができなくなり、anim graph がパラメータもクロックも遷移も失います。
>
> **骨子**:
> ```text
> generation の値域: 0 = 無効の予約値、1..UINT32_MAX = 有効
>                    ++g; if (g == 0) ++g;      // 一周しても 0 を踏まない
> ledger.validate(identity, gen):
>   未知       → invalid_handle
>   世代不一致 → stale_generation     ← tombstone がこの行き先を守る
>   一致       → ok
> rig の解決 = ledger.validate() ∧ sameHandle() ∧ アセット側 atomic == handle.generation
> ```
>
> **手がかり**: [`resolveRigResource()`](../../src/core/animation/animationservice.cpp#L173) は世代を **2 系統**で照合します — ledger の記録と、アセット側の `generation_state->current.load()` です。前者は「サービスが知っている世代」、後者は「アセットが実際に進めた世代」で、reload の途中では一時的に食い違い得ます(片方だけ見ると差し替え途中のアセットを掴みます)。pose だけは arena 所有でリソース台帳に載らないため、[`stale_poses`](../../src/core/animation/animationservice.cpp#L137) という別表へ退避されます。テストは [`animgraph_test.cpp` 内](../../test/animgraph_test.cpp#L363)(reload 後に各 API が `stale_generation` を返し、`rebind()` が graph 状態を保つ)、[`animation_jobs_test.cpp` 内](../../test/animation_jobs_test.cpp#L330)。
>
> **不変条件**: generation 0 を有効値として外へ出さない。消えたリソースの identity を ledger から**消さない**(tombstone を残す)。`invalid_handle` と `stale_generation` の使い分けを崩さない。

### `ModelInstanceId` の落とし穴

`ModelInstanceId` は 3 フィールドの比較可能な値型で、`toString()` は `"index:generation@scene_epoch"` を返します([`toString()`](../../src/core/renderer/modelinstance.hpp#L21))。

- **identity は `PolygonInstanceContainer` から [`ModelInstanceSlots`](../../src/core/renderer/modelinstanceslots.hpp#L14) へ切り出されました。** `index` / `generation` / `scene_epoch` を持っているのはこの class で、`PolygonInstanceContainer` 側にはもう `scene_epoch` も世代表もありません(`instance_slots` メンバ越しに問い合わせます)。
- `scene_epoch` が進むのは [`ModelInstanceSlots::clearPrepared()`](../../src/core/renderer/modelinstanceslots.cpp#L82) の **1 か所だけ** で、これは [`PolygonInstanceContainer::clear()`](../../src/core/renderer/polygoninstancecontainer.cpp#L600)(scene clear / 再ロード)から呼ばれます。個別 slot が死んで再利用されるときに進むのは [`ModelInstanceSlots::retire()`](../../src/core/renderer/modelinstanceslots.cpp#L62) が上げる slot generation の方です。いずれも animation generation や model asset content revision からは独立です。
- `clear()` 自体も §9.17 の prepare/publish 形です。epoch 枯渇(`scene epoch exhausted`)を投げうる検査は [`ModelInstanceSlots::prepareClear()`](../../src/core/renderer/modelinstanceslots.cpp#L73) が先に済ませ、実際の書き換えは `noexcept` の `clearPrepared()` が行います。`clear()` を読むときはこの 2 段を 1 つの操作として読んでください。
- 生存確認は [`isModelInstanceAlive(id)`](../../src/core/renderer/polygoninstancecontainer.hpp#L367)。
- [`removeModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L342) は `void` ではなく **`bool`** を返します。戻り値を無視すると「消したつもりで消えていない」を見逃します。
- `instanceCountForTesting()` は **live 数** を返すよう変わり、スロット総数は `slotCountForTesting()` です。両者の差は空きスロットです。

登録は 3 段階に分かれました(WP144 / TRANSIENT0)。[`preflightModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L336)(Vulkan 資源確保前の容量拒否)→ [`stageModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L337) → [`publishModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L340)(`noexcept`、**唯一の no-fail 公開点**)です。詳細は §9.17。

> 🧩 **難所 — `struct_size` の 3 段ルール**([`getApiV1()`](../../src/core/animation/animationservice.cpp#L1726) / [`validateDescriptor()`](../../src/core/animation/animationserviceabi.hpp#L15))
>
> **何をする所か**: 別々にコンパイルされた DLL とエンジンの間で、C の plain struct を版下位互換のまま受け渡します。呼び出し側が `struct_size` を書き、エンジンが自分の知る分だけを書き戻します。
>
> **素朴に読むと**: `validateDescriptor(*desc)` がほぼ全 API の頭に並ぶので「サイズは常に交渉されている」と読んでしまいますが、実際には **3 種類の異なるルールが混ざっています**。交渉の入口 3 本([`getApiV1`](../../src/core/animation/animationservice.cpp#L1726) / [`Impl::getService`](../../src/core/animation/animationservice.cpp#L1596) / [`getPoseStagingServiceV1`](../../src/core/animation/animationservice.cpp#L1751))は `sizeof(DescriptorHeaderV1)` = 16 バイトしか要求せず、加算的テールを持つ [`getClipMetadata`](../../src/core/animation/animationservice.cpp#L1078) は凍結プレフィクスの長さを手書きし、**それ以外は既定の `sizeof(T)`(= エンジン側の現在サイズ)を下限**として要求します(検査は [`struct_size < minimum`](../../src/core/animation/animationserviceabi.hpp#L17) の下限比較だけなので、大きすぎる `struct_size` は弾かれません。[`ProbeRuntime` 側の同名関数](../../src/core/animation/animationprobe.cpp#L38)は既定が `headerSize` である点も別物です)。つまり既存 descriptor の末尾にフィールドを足すと、古い DLL は即 `invalid_argument` です — 「加算的だから安全」という一般則はこのコードベースでは成り立ちません。書き戻しは `memcpy(out, &produced, min(caller_size, sizeof(produced)))` で **エンジンは呼び出し側のテールをゼロ埋めしません**から、呼び出し側は descriptor を必ずゼロ初期化してから使う必要があります(テストの `descriptor<T>()`、[`descriptor()`](../../test/animation_abi_dll_test.cpp#L42))。成功後の `out->struct_size` は**エンジンの `sizeof`** に上書きされるので、自分が確保したバッファ長として再利用しないでください。
>
> **骨子**:
> ```text
> [ header 16B ][ frozen v1 prefix ][ additive tail ]
>  ^struct_size  ^古い DLL はここまで  ^新しいエンジンだけが書ける
>
> エンジン側: struct_size >= 16 / version 一致 / reserved==0 / client_abi 範囲内
>            caller_size = out->struct_size        ← 先に退避
>            produced{} を完全に埋める(struct_size = 自分の sizeof)
>            memcpy(out, &produced, min(caller_size, sizeof(produced)))
> ```
>
> **手がかり**: 版数の軸が 3 本あります — `version`(descriptor のレイアウト版)、`engine_abi_version` / `service_version`(関数表の意味論の版)、`capability_bits`(機能単位)。`reserved0/1 != 0` を `reserved_not_zero` で弾くのは、新しい ABI の呼び出し側が古いエンジンに当たったことを検出するためです。[`getApiV1()`](../../src/core/userpublic/animation/abi_v1.hpp#L653) の `static_assert` は 2 つのサイズと 4 か所のオフセットだけを固定しており、**それ以外のフィールド順はコンパイラが何も守ってくれません**。テストは [`old and new animation header DLLs preserve the frozen prefix`](../../test/animation_abi_dll_test.cpp#L73)(凍結プレフィクス)/ [`old-client new-engine and new-client old-engine negotiate additively`](../../test/animation_abi_dll_test.cpp#L103)(old-client と new-engine の双方向)で、`storage[caller_size]` の番兵によって越境書き込みが無いことを検査しています。
>
> **不変条件**: 既存フィールドの順序・型・サイズを変えない(追加は末尾のみ)。末尾に足したら、その descriptor を読む側の `minimum` を凍結プレフィクスのサイズへ明示的に下げる(前例は engine 側で 5 か所 — [`getClipMetadata`](../../src/core/animation/animationservice.cpp#L1078) と、同じ descriptor を検証する `ProbeRuntime` 側の [advanceCursor](../../src/core/animation/animationprobe.cpp#L256)(desc / result の 2 つ)/ [publishAnimationFrame](../../src/core/animation/animationprobe.cpp#L448) / [advanceTemporalHistoryAfterRender](../../src/core/animation/animationprobe.cpp#L467)。片側だけ直すと取りこぼします)。書き戻しは必ず `min(caller_size, sizeof(produced))` にする。ABI 面の関数は `noexcept` を保ち、例外を `Status` へ変換する。

## 9.3 `PELICAN_REGISTER_EVENT` / `PELICAN_REGISTER_SYSTEM` / `PELICAN_REGISTER_BEHAVIOR`

これらは runtime reflection ではなく、C++ の static object constructor と compile-time overload lookup の組み合わせです。

### event 登録の展開

[`PELICAN_REGISTER_EVENT_IMPL`](../../src/core/userpublic/details/event/registerer.hpp#L242) は概念的に2つを生成します(macro 本体は [`PELICAN_REGISTER_EVENT`](../../src/core/userpublic/details/event/registerer.hpp#L257))。

1. `EventCatalogTag<N>` に対する `pelicanEventCatalogEntry(...) -> EventCatalogEntry<MyEvent>` overload。
2. anonymous namespace の static object。constructor で `registerEvent<MyEvent>("MyEvent")`。

`N` は `__COUNTER__` です。

### system 登録の展開

[`PELICAN_REGISTER_SYSTEM_IMPL`](../../src/core/userpublic/details/system/registerer.hpp#L155) も static object を作ります(macro 本体は [`PELICAN_REGISTER_SYSTEM`](../../src/core/userpublic/details/system/registerer.hpp#L173))。その constructor は、system macro より前に見えている event catalog entry を `0..N-1` まで compile-time に探索します。`System` に `onEvent(const Event&, GameContext&)` があれば function pointer table へ追加し、最後に system 本体を登録します。生成される static object は登録の戻り値である [`RegistrationToken token;`](../../src/core/userpublic/details/system/registerer.hpp#L158) をメンバとして保持します。

```cpp
PELICAN_REGISTER_EVENT(Damage)
PELICAN_REGISTER_SYSTEM(CombatSystem, 100)
```

この順序なら `CombatSystem::onEvent(const Damage&, ...)` を発見できます。

> 🧩 **難所 — `decltype` で catalog を掃く**([registerer.hpp](../../src/core/userpublic/details/system/registerer.hpp#L71) の `HasEventCatalogLookup` / `collectGameSystemEventHandlers`)
>
> **何をする所か**: 「自分より前に登録された event 型」をコンパイル時に列挙し、`onEvent` を持つものだけを関数ポインタ表に詰めます。
>
> **素朴に読むと**: 核心はマクロの中のこのラムダ 1 個で、初見ではまず読めません。
>
> ```cpp
> []<int Index>() -> decltype(pelicanEventCatalogEntry(::Pelican::internal::EventCatalogTag<Index>{}))
> { return {}; }
> ```
>
> 本体 `{ return {}; }` は飾りで、**意味があるのは戻り値型の `decltype` だけ**です。その番号の event が登録されていなければ戻り値型の置換に失敗し、`operator()<Index>` が ill-formed になる — これを requires 式で bool にして「あるかどうか」を判定しています。これが **SFINAE**(スフィネ。"Substitution Failure Is Not An Error" の略で、テンプレート引数を当てはめた結果おかしな型になっても**コンパイルエラーにはせず、その候補を黙って外す**という C++ の規則。「エラーにならない」性質を逆手に取って、存在検査に使う常套手段です)です。`if constexpr` で存在しない番号を黙って捨てているのも必須で、これが無いと `__COUNTER__` の抜け番(他のマクロが消費した番号)でビルドごと落ちます。素朴に「全 event を実行時に走査」する実装にすると `onEvent` の有無を実行時に判定できず、`virtual onEvent` にすると event 型ごとに vtable が要ります。この方式は両方を避けています。
>
> **なぜ「前だけ」見えるのか**: 呼び出し引数がテンプレート引数 `Index` に依存するため、**二相名前解決**(テンプレートの名前解決を「定義を書いた位置」と「型が決まって実体化される位置」の 2 段階で行う C++ の規則)になります。候補になるのは「マクロを展開した位置での通常の名前探索」と「実体化時の **ADL**(実引数依存探索 — 引数の型が属する名前空間も探しに行く仕組み)」の和です。ADL が探すのは引数の型が属する `Pelican::internal` だけですが、マクロが生む `pelicanEventCatalogEntry` の宣言は展開先(普通はグローバルかゲームの名前空間)にあるので **ADL では見つかりません**。つまり ADL 側の寄与は常に空で、実際に効くのは展開位置での通常の名前探索だけ — 結果として「同じ翻訳単位で、登録マクロより前にある宣言だけ」が見えます。逆に言うと、`EventCatalogTag` をマクロの展開先と同じ名前空間へ移すと ADL の寄与が空でなくなり、**ADL は実体化時点で探す**ので、登録マクロより後ろに書かれた overload まで拾えるようになります。
>
> **手がかり**: `unique_id`(= `__COUNTER__`)は **catalog の添字と走査上限を兼ねます**。overload は宣言だけで、本体は一度も呼ばれません。走査コストはテンプレート実体化 O(`__COUNTER__`) なので、event ヘッダを大量に include した翻訳単位で system を登録すると、その TU だけコンパイルが目に見えて遅くなります。behavior 側([behavior/registerer.hpp](../../src/core/userpublic/details/behavior/registerer.hpp#L87))は完全に同じ構造の複製なので、片方を直すなら両方直します。
>
> **不変条件**: `EventCatalogTag` は `Pelican::internal` に置いたままにする(マクロの展開先と同じ名前空間へ動かすと ADL 経路が生えて「後ろの event も見える」ようになり、TU 順序依存が静かに変わります)。ハンドラの第 2 引数は system が `GameContext&`、behavior が `BehaviorContext&` — 取り違えると concept が false になり、**コンパイルは通るのに登録されません**。

### behavior 登録の展開

[`PELICAN_REGISTER_BEHAVIOR(Type, stable_name, schema_version)`](../../src/core/userpublic/details/behavior/registerer.hpp#L319)(IMPL は [`PELICAN_REGISTER_BEHAVIOR_IMPL()`](../../src/core/userpublic/details/behavior/registerer.hpp#L298))も **まったく同じ `__COUNTER__` + catalog 走査方式** です。[`collectBehaviorEventHandlers<Type>()`](../../src/core/userpublic/details/behavior/registerer.hpp#L99) が `onEvent(const Event&, BehaviorContext&)` を探します。

したがって **翻訳単位の順序の罠がそのまま適用されます**。event 宣言 header は behavior の `.cpp` の先頭で include してください。加えて 2 点、System とは違う注意があります。

- handler の第2引数は `BehaviorContext&` です。`GameContext&` 版を書くと、compile は通るのに **catalog に載らず、呼ばれません**。
- `onEvent` は `Behavior` の virtual メンバではありません。`override` を付けられない代わりに、typo が黙って無視されます。

### system 登録の第3のフック

[`HasGameSystemQueuedEvent`](../../src/core/userpublic/details/system/registerer.hpp#L48)(`void dispatchQueuedEvent(const QueuedEvent&, GameContext&)`)が加わりました。これにより「update も onEvent も無い System は登録エラー」の条件は `!has_queued_event && event_handlers.empty()` に変わっています([`registerer.hpp` 内](../../src/core/userpublic/details/system/registerer.hpp#L106))。

エラー文言自体は据え置きです。

```text
registered game systems must define update(ctx) or onEvent(event, ctx)
```

文言と実条件がややズレているので、`dispatchQueuedEvent` だけを持つ System を書くときは「このメッセージが出たら 3 つとも無い」と読み替えてください。

### translation unit と宣言順の罠

- event macro は system macro より前に、同じ translation unit の lookup から見える必要があります。通常は event 宣言と登録を header に置き、その header を system `.cpp` で先に include します。
- 別の `.cpp` だけで登録された event は、system 側の compile-time catalog からは見えません。
- system macro より後に event macro を置いても、その system の handler list には入りません。
- object file が最終 executable へ link されなければ static constructor も走りません。
- `__COUNTER__` は translation unit ごとであり、process-wide event ID ではありません。runtime の同一性は `std::type_index` と名前で判定します。

runtime update 順は static 初期化順ではなく、[`sortGameSystemRegistrations()`](../../src/core/userpublic/details/system/registerer.cpp#L33) が `(order, name)` で決めます。ここは決定論的です。Behavior もこの全順序に order 50 の 1 点として参加します([第5章 §5.13](05_gameplay_and_services.md))。

registry の各登録には [`RegistrationOwner`](../../src/core/userpublic/details/system/registerer.hpp#L33)(engine / game DLL)が付きます。game DLL reload では [`unregisterGameSystems(owner)`](../../src/core/userpublic/details/system/registerer.hpp#L142) が旧 DLL の static 登録を外し、新 DLL の static 初期化が再登録します。個別解除用に [`unregisterGameSystem(token)`](../../src/core/userpublic/details/system/registerer.hpp#L141) もあります。また event 型は静的記述子 `pelican_payload`([`EventPayloadDescriptor`](../../src/core/userpublic/details/event/payloadschema.hpp#L39) / 実体化後は [`EventPayloadSchema`](../../src/core/userpublic/details/event/payloadschema.hpp#L22))で payload の field schema を宣言できるようになり、`fail_*` fixture は compile-time 検証([`run_event_schema_compile.cmake`](../../test/run_event_schema_compile.cmake))になっています。

### event 名と RPC payload

[`eventDisplayName()`](../../src/core/userpublic/details/event/registerer.cpp#L20) は namespace qualifier を落とします。`foo::Changed` と `bar::Changed` は同じ `Changed` になり、異なる型なら duplicate error です。

RPC の `inject_event` 用 JSON loader は [`registerEvent<Event>()`](../../src/core/userpublic/details/event/registerer.hpp#L87) で選ばれます。

- default constructible かつ `ref(JsonArchiveLoader&)` がある: payload を field へ load。
- default constructible だが `ref` がない: default event を作り、渡された payload は使わない。
- default constructible でない: name injection 不可。

C++ の `GameContext::emit(event)` は copy した値をそのまま queue に置くため、この JSON 制約とは別です。

> 🧩 **難所 — void payload の消し方**([`QueuedEvent::payload`](../../src/core/userpublic/details/event/registerer.hpp#L29) / [`dispatchEventToRegisteredGameSystems()`](../../src/core/userpublic/details/system/registerer.cpp#L54))
>
> **何をする所か**: 任意の event 型を `shared_ptr<const void>` 1 個へ消して queue に積み、配送時に `std::type_index` で照合してから元の型へ `static_cast` して呼び戻します。
>
> **素朴に読むと**: thunk(サンク — 型を消した引数を本来の型へ戻して本体を呼ぶだけの、極小の中継関数)側の `*static_cast<const Event *>(event)`([system/registerer.hpp](../../src/core/userpublic/details/system/registerer.hpp#L65))には**何の検査もありません**。型の健全性を担保しているのは配送側の [`handler.event_type == event.type`](../../src/core/userpublic/details/system/registerer.cpp#L58) の 1 行だけで、ここが唯一の関門であるという事実はコードの見た目からは分かりません。次に、`shared_ptr<const void>` は「型を捨てたポインタ」ではありません — `make_shared<EventType>` の control block(`shared_ptr` が参照カウントと「どう破棄するか」を保持している内部ブロック)が `~EventType` を持ったまま `const void` へ暗黙変換されており、**そのデストラクタは game DLL 側のコード**です(`void*` + `delete` にすると即 UB になる、という理由でこの型が選ばれています)。したがって DLL を `FreeLibrary` する前に payload を必ず解放する必要があり、一見無意味に見える [`drainForTeardown()`](../../src/core/userpublic/details/event/registerer.cpp#L421)(空の局所 vector に swap して個数だけ返す)は、**owner がまだ生きているこのスコープ内で全 payload を破棄する**のが目的です。[`unregisterEvents()`](../../src/core/userpublic/details/event/registerer.cpp#L526) が `catch (...) { std::terminate(); }` するのも同じ理由で、例外を握り潰して `FreeLibrary` へ進むとアンロード済みの型を指す登録が残ります。
>
> **骨子**:
> ```text
> emit<E>(e)  payload = make_shared<E>(e) → shared_ptr<const void>(deleter は ~E のまま)
>             type    = type_index(typeid(E))
> frame 先頭  deliver_now_events.swap(pending_events)
> dispatch    for system in (order,name)順 / for handler in system.event_handlers:
>               if handler.event_type == event.type:      ← 唯一の型検査
>                  handler.dispatch(payload.get(), ctx)   ← 検査なしの static_cast
> teardown    drainForTeardown() で owner 生存中に payload を全破棄 → その後 DLL アンロード
> ```
>
> **手がかり**: [`QueuedEvent::owner`](../../src/core/userpublic/details/event/registerer.hpp#L33) は DLL reload 時に「消えた owner の queued event」を捨てるためにあります(`unregisterEvents` の `std::erase_if`)。一緒に読むテストは [`Event bus freeze is separate from delivery`](../../test/eventlayer_test.cpp#L69)「Event bus freeze is separate from delivery」と、[`run_behavior_dll_reload.ps1`](../../test/run_behavior_dll_reload.ps1) の `queued_event_purge` ケースです。
>
> **不変条件**: 配送前の `event_type == event.type` 照合を外さない。teardown の順序は `drainForTeardown()` / `unregisterEvents(owner)` → その後に DLL アンロード(逆にしない)。`payload` を `shared_ptr<const void>` 以外(生ポインタ・`any`・自前 deleter)へ変えない — control block が型のデストラクタを運ぶことが DLL 境界の安全性そのものです。

### system instance の寿命

[`gameSystemInstance<System>()`](../../src/core/userpublic/details/system/registerer.hpp#L53) は function-local static です。module container と違い、`PelicanCore::run()` ごとには再生成されません。同一 process で engine を複数回 run する test/tool では、System の member state が明示 reset されない限り次の run に残ります。ただしこの注意が残るのは **engine 側の System のみ**です。game System は DLL 内 static なので、DLL reload 後は新インスタンスになり、member state は持ち越されません。

## 9.4 `GameObjects::add()` の typestate builder

[`GameObjects`](../../src/core/userpublic/gameobjects.hpp#L20) は fluent API に見えますが、内部では呼ぶたびに別 template 型を返します。

```cpp
auto id = GameObjects::add()
    .addComponent<LocalTransformComponent>()
    .addComponent(SimpleModelViewComponent{...})
    .finish();
```

型の中に次を埋め込みます。

- `ComponentIdHolder<...>`: compile-time の Component ID 列。
- `IndexHolder<...>`: 値を渡した component が pointer 配列の何番目か。
- `tuple<...>`: populate 時に代入する component data。

`finish()` で初めて ID span と populate lambda へ落ち、[`GameObjects::create()`](../../src/core/userpublic/gameobjects.cpp#L13) から ECS の transactional create へ入ります。template の目的は、runtime に型情報が消えた `void*` 配列へ、compile-time に正しい型と index で代入することです。

### reference lifetime の罠

値付き `addComponent(const T&)` は、値を copy して builder に保存するのではなく [`std::tuple<const T&>`](../../src/core/userpublic/gameobjects.hpp#L113) を保存します。

```cpp
// 安全: temporaryが破棄される前、同じfull-expressionでfinishする
auto id = GameObjects::add().addComponent(MyComponent{...}).finish();

// 危険: builderだけ保存すると内部referenceがdanglingになる
auto builder = GameObjects::add().addComponent(MyComponent{...});
auto id2 = builder.finish();
```

builder を後で使うなら、component value を別の lvalue として `finish()` まで生存させてください。重複 Component ID は最終的に [`createEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L365) が例外にします。

## 9.5 Component type erasure と lifecycle callback

ECS chunk は C++ 型を知りません。登録時に [`registerComponent<T>()`](../../src/core/userpublic/details/component/registerer.hpp#L36) が次を callback に変えます。

```text
T
├─ sizeof / alignof
├─ default construct
├─ destroy noexcept
├─ relocate noexcept
├─ optional init
├─ optional deinit noexcept
└─ optional ref(JsonArchiveLoader&)
```

non-trivial component の swap-delete(削除した要素の穴へ配列の末尾要素を移して末尾を縮める方式。詰め直しが 1 要素で済む代わりに、並び順は保たれません)は assignment ではなく、destination へ move-construct し source を destroy します。[`relocate` lambda](../../src/core/userpublic/details/component/registerer.hpp#L53) と [`VariedArray::removeAt()`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) が対になっています。

制約は compile-time に固定されます。

- default constructible。
- nothrow move constructible。
- nothrow destructible。
- `deinit()` があるなら `noexcept`。

これにより create rollback と remove/teardown を `noexcept` cleanup として実装できます。`init()` だけは例外を許し、既に init 済みの component を逆順 deinit して transaction を戻します。

### Component ID は dense index でもある

[`ComponentInfoManager::getIndexFromComponentId()`](../../src/core/ecs/componentinfo.cpp#L86) は値としては ID をそのまま index に使いますが、**現在は `get(id)` 経由になりました**。未登録スロットの読み出しは [`getFromIndex()`](../../src/core/ecs/componentinfo.cpp#L89) が `std::out_of_range("component index N is not registered")` を投げます。黙って壊れた metadata を返すことはありません。さらに archetype mask(archetype は「同じ component の組み合わせを持つ entity をまとめた区画」で、mask はその組み合わせを component ID 1 個 = 1 ビットとして表した値)は [`MAX_COMPONENTS = 64`](../../src/core/userpublic/details/ecs/componentdeclare.hpp#L21) の `uint64_t` です。

したがって Component ID は次を満たす必要があります。

- process 全体で一意。
- `0..63` の範囲。
- 同じ型は全 translation unit で同じ ID specialization を見る。

**登録側の検査は全面的に強化されました**(WP163 / ECS1)。[`ComponentInfoManager::registerComponent()`](../../src/core/ecs/componentinfo.cpp#L20) は以下をすべて例外で拒否します。旧版の「黙って slot を上書きする」挙動はもうありません。

| 条件 | 文言 |
|---|---|
| 空名 | `component registration name must not be empty` |
| ID >= 64 | `component '<name>' id N exceeds the registration limit (<64)` |
| ID 重複 | `component '<name>' duplicates id N already registered by '<other>'` |
| 名前重複 | `component name '<name>' duplicates registered id N while incoming id is M` |
| lifecycle metadata 欠落 | `component '<name>' registration requires typed lifecycle metadata` |

ID 割当を engine-wide schema として管理する必要がある点は変わりませんが、間違いは **起動時に落ちて分かる** ようになりました。

登録解除 API も入りました。[`unregisterComponent(token)`](../../src/core/userpublic/details/component/registerer.hpp#L90) / [`unregisterComponents(owner)`](../../src/core/userpublic/details/component/registerer.hpp#L91) / `componentRegistrationCount(owner)` です。ただし [`ComponentInfoManager::unregisterComponent()`](../../src/core/ecs/componentinfo.cpp#L64) は **依存 System が残っていると拒否** します。

```text
cannot unregister component '<name>': dependent ECS system '<sys>' remains
```

### custom Component の現状

型 ID 宣言 macro は公開 header にありますが、runtime 登録入口は [`internal::getComponentRegisterer()`](../../src/core/userpublic/details/component/registerer.hpp#L89) です。production で自動登録されるのは [`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L20) の built-in 群です。

game code 向けの安定した `PELICAN_REGISTER_COMPONENT` public macro/boot hook は **現在も存在しません**(リポジトリ全体を grep しても不在)。`DECLARE_COMPONENT` しただけでは `ComponentInfoManager` に metadata が入らず、create/scene load できません。テストは internal API を直接呼んで登録しています。[`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L190) が例です。

なお `registerComponent<T>()` の戻り値は `void` から [`RegistrationToken`](../../src/core/userpublic/details/component/registerer.hpp#L36) へ変わりました。

## 9.6 Archetype/SoA chunk と pointer lifetime

各 [`VariedArray`](../../src/core/userpublic/details/ecs/chunk.hpp#L25) は constructor で `stride * 4096` bytes を一度に aligned allocation します。[`chunk.cpp`](../../src/core/userpublic/details/ecs/chunk.cpp#L17) を参照してください。同じ component の値は連続し、system は `T* + count` で batch 処理できます。

storage 自体は chunk の生存中に再 allocation されません。しかし pointer が永久に同じ entity を指すわけではありません。

- entity remove は穴へ末尾 entity を move する swap-delete。
- 別 entity の remove でも、自分が末尾なら自分の値が別 address へ移る。
- 移った entity は **id 表の側も直す**必要があります。`EntityId` から行を引くのは [`id_table[id.index].ref`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L108)(= `{chunk_index, array_index}`)なので、[`remove()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L550) は末尾 entity を穴へ move した直後に、その entity の `ref` を穴の位置へ書き換えます([swap-delete の実体](../../src/core/userpublic/details/ecs/coretemplate.cpp#L561))(例: E1 が row 5、末尾の E2 が row 10 で、E1 を remove すると E2 は row 5 へ移るので、`id_table[E2.index].ref.array_index` を 10 → 5 にする)。この更新を落とすと E2 は消えた row 10 を指したままになり、[`resolve()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L308) の範囲検査で当面は「いない」扱い、その後 create が row 10 を埋めると別 entity の行を指します。
- scene load/clear は全 component を破棄。
- `GameObjectId` は generation で再解決できるが、生 pointer には generation がない。

したがって component pointer/reference は、structural mutation をまたいで cache しないでください。必要時に EntityId から [`tryComponent()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L354) で再取得します。

create/remove/clear の再入は [`MutationScope`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L297) が拒否します。component `init()` / `deinit()` callback からさらに structural mutation すると、部分更新を防ぐため例外になります。

## 9.7 内部 ECS scheduler の並列性

内部 ECS system は template 引数の pointer constness から read/write component index を抽出します。[`registerSystem()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L409) で `const T*` は read、`T*` は write です。現在の signature は instance 参照を受け取り `SystemId` を返す形で、登録時に [`static_assert`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L410) が「batch か per-chunk のどちらかの process 形」を必須にします。

**WP148 / ECS0 でこの節は大きく変わりました。** 以前の「read/write は conflict graph を作らない」「cycle は例外にならない」という記述は失効しています。

実行計画は [`internal::buildECSExecutionPlan(nodes, hazard_policy)`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L206) が作ります。[`ECSCoreTemplatePublic::update()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L663) は計画を Kahn 法(カーン法 — 依存先が残っていない(入次数 0 の)ノードを取り出しては、その分だけ相手の入次数を減らす、を繰り返すトポロジカルソート(依存の向きに矛盾しないよう一列に並べること)のアルゴリズム。同時に入次数 0 になったノードの集まりが、そのまま「並列に走らせてよい level」になります)で level 化し、level ごとに「全 System の `prepare_func` を owner thread で実行 → 全部を `JobSystem` へ schedule → `wait()`」を行います。

```text
level 0: A, B, C  -> parallel jobs -> wait
level 1: D, E     -> parallel jobs -> wait
```

### hazard は自動検出される

[`conflictingComponents()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L170) が「同じ component index を触り、少なくとも片方が write」の組を競合として抽出します。ただし **競合の検出であって、依存 edge の自動導出ではありません**。扱いは policy 次第です。

| policy | 挙動 |
|---|---|
| `automatic_serialization`(既定) | 登録 ID の小さい方を先に実行する暗黙 edge を足し、**WARNING ログを出す** |
| `strict` | 全 hazard を集めて 1 本の例外にする |

> **設計決定:** policy の切替トリガは **`--strict-assets`** です([`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L664))。コメントが理由を書いています。「`--strict-assets` は既存の起動時 strict/determinism ゲートであり、それを再利用することで WP148 を scheduler だけの変更境界に収めた」。名前から ECS scheduler を連想しにくいので注意してください。

自動直列化の WARNING 文言です。

```text
ECS auto serialization: '{}' before '{}' for component(s) {}; add an explicit dependency edge (strict mode rejects this hazard)
```

strict 側の例外はすべての hazard を `; ` で連結し、末尾に `; add dependency edges or use automatic serialization` が付きます。個々の hazard 行は次の形です。

```text
ECS unordered component hazard between systems '<A>' and '<B>' on component(s) '<name>'
```

### 現在の注意点

- **無警告の race はもう起きません。** ただし逆に、**自動直列化は WARNING でしか通知されません**。ログを見ていないと「なぜか並列化されない」「なぜか順序が登録順に固定された」ことに気づけません。性能を気にするなら WARNING を潰して明示 edge を書いてください。
- **cycle は例外になりました。** `makeExecutionLevels()` が `executed_count != nodes.size()` を検査し、`ECS dependency cycle detected; unexecuted systems: '<name>' ...` を投げます([`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L138))。
- **存在しない System への依存、重複依存も起動時エラー** です([`coretemplate.cpp` 内](../../src/core/userpublic/details/ecs/coretemplate.cpp#L228))。`... depends on missing system id N; system would be unexecuted` / `... declares dependency on system '<name>' more than once; system would be unexecuted`。
- 計画作成時に node は `node.id` で sort されるため([`std::sort()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L209))、同 level 内の並びは登録順で決定的です。**それでも同 level は並列実行されるので、開始順に依存しないでください。**
- 実行計画は `execution_levels` にキャッシュされ、`execution_plan_dirty` か policy 変化のときだけ再構築されます。`registerSystem()` / `unregisterSystem()` が dirty を立てます。
- system が batch 版 `process(std::vector<ChunkView<...>>)` と per-chunk 版 `process(tuple,count)` の両方を定義すると、独立した2つの `if constexpr` により **両方が呼ばれます**(この点は変わっていません)。

### 組み込み System の依存はほぼ全順序になった

[`predefined.cpp` 内](../../src/core/ecs/predefined.cpp#L31) の現在の依存です(全て `registerSystemForce`)。

| System | 依存 |
|---|---|
| `LocalTransformSystem` | なし |
| `SimpleModelViewUpdateSystem` | なし |
| `AnimationSystem` | `{model_update}` |
| `SimpleModelViewTransformSystem` | `{local_transform, model_update, animation}` |
| `CameraSystem` | `{local_transform, model_transform}` |
| `SpriteViewRenderSystem` | `{local_transform, camera}` |

### prepare フェーズ: worker job 内の `GET_MODULE` を避ける新しい作法

level 実行前に、scheduler は各 System の [`prepare_func`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L735) を owner thread で呼びます。System は `prepareEcsWorkerDependencies()`(例: [`CameraSystem::prepareEcsWorkerDependencies()`](../../src/core/ecs/predefined/camerasystem.cpp#L9))で `GET_MODULE` を owner thread 上で済ませます。9.1 の `freezeCreation()` により worker job 内からの新規 module 生成は失敗するため、この prepare 契約が新しい落とし穴であり作法です。

game system registry はこれとは別機構で、現在は `(order,name)` 順の直列 update です。2種類の「System」を混同しないでください。

### change detection の意味

non-force system は matching chunk の component version と `last_run_tick` を比較します。component を raw pointer から直接書き換えただけでは version が上がりません。公開 mutation は [`setComponent<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L364) または [`markComponentChanged()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L640) を通す必要があります。

一方、built-in system の多くは `registerSystemForce` なので毎 frame 動きます。最適化時に force を外すなら、すべての mutation 経路が version を更新するか先に確認してください。

## 9.8 Camera、light、collider はすべて同じ ECS component ではない

scene の `components` 配列に見えても runtime binding は一様ではありません。[`prepareSceneBindings()`](../../src/core/loader/scene.cpp#L93) が分岐します。系統は **4 つ**になりました。

| scene name | runtime 経路 |
|---|---|
| `transform`, `simplemodelview`, `camera` | ComponentInfo 経由で ECS chunk へ作成 |
| `light` | ECS へ入れず `LightLoadEntry` として `LightContainer::load()` |
| `collider` | ECS へ入れず `ColliderComponent` を parse し `PhysWorld::bindCollider()`。`PELICAN_WITH_PHYSICS` OFF の build では collider を含む scene は明示エラー([`throwBuildFeatureDisabled()`](../../src/core/loader/scene.cpp#L323)) |
| `behavior` | ECS へ入れず [`prepareSceneBehaviorAttachments()`](../../src/core/gamelogic/behaviorarena.cpp#L70) 経由で `BehaviorAttachmentArena` へ([`component_name == "behavior"`](../../src/core/loader/scene.cpp#L165))。`type` は非空文字列必須。game DLL 未ロードなら **pending** 扱いで警告のみ |

`ColliderComponent` に `init/deinit` があっても、現在の scene loader は special case です。`ECSCoreTemplatePublic::tryComponent<ColliderComponent>()` で取れる通常 ECS component だとは考えないでください。behavior も同様で、ECS の component として問い合わせても見つかりません。

behavior の公開は [`arena.publishSceneAttachments()`](../../src/core/loader/scene.cpp#L483) の 1 点で、失敗すると [`clearRuntimeScene()`](../../src/core/loader/scene.cpp#L519) してから rethrow します。「behavior 型名を間違えると scene が半分だけロードされる」ということはありません。

### ライトのマジックネームは撤去された(WP142 / LIGHT0)

以前は engine が `"KeyLight"` / `"FillLight"` / `"PointLight1"` / `"SpotLight1"` といった名前を特別扱いし、時刻を渡すと engine 側が勝手にアニメーションさせていました。**この挙動と、原本値を保持していた配列は削除されました。**

現在の [`LightContainer`](../../src/core/light/lightcontainer.hpp#L26) には **時間を引数に取る更新関数がありません**。ライト値を書き換える口は、scene load([`prepareLoad()`](../../src/core/light/lightcontainer.hpp#L43) → [`publishPrepared()`](../../src/core/light/lightcontainer.hpp#L45) の §9.17 型。`load()` はこの 2 段に警告出力を挟んだ入口です)と、名前指定の setter 4 本([`setDirectionalLightDirection()`](../../src/core/light/lightcontainer.hpp#L74) ほか。名前が引けなければ **`false` を返すだけ** で例外にはなりません)の 2 系統だけです。`update()` の overload 群は shadow 行列と sky ambient を受け取って現在値を GPU バッファへ書き出すもので、値そのものは動かしません。

- 代替は公開 API 4 本([`GameContext::setDirectionalLightDirection()` ほか](../../src/core/userpublic/gamecontext.hpp#L50))で、ライトの時間変化は **ユーザー空間の責務** になりました。実例は [`projects/example/code/playercontrol.cpp`](../../projects/example/code/playercontrol.cpp#L31) の `updateLightAnimation()` です。
- setter の結果は `Renderer` の per-frame [`updateFrameLights()`](../../src/core/vkcore/renderer.cpp#L341)(呼び出しは [ここ](../../src/core/vkcore/renderer.cpp#L1377))で GPU バッファへ反映されます。
- 「アップグレード後にライトが動かなくなった」は仕様です。scene 名に依存した暗黙アニメーションを期待しているコードを探してください。

代わりに **上限超過の警告** が入りました。[`collectLightCapWarnings()`](../../src/core/light/lightcontainer.hpp#L24) が `MAX_DIRECTIONAL_LIGHTS` / `MAX_POINT_LIGHTS` / `MAX_SPOT_LIGHTS` を超えた分について次を出します([`lightcontainer.cpp` 内](../../src/core/light/lightcontainer.cpp#L56))。

```text
Light cap exceeded: <type> light #<ordinal> '<name>' will not be rendered (cap <N>)
```

`<name>` が空なら `<unnamed>` です。「ライトを足したのに 1 個だけ描かれない」ときはこの WARNING を先に探してください。テストは [`test/lightpolicy_test.cpp`](../../test/lightpolicy_test.cpp) です。

### camera の二重経路

- `Camera::loadSceneCameras()` が scene document を再走査し、projection、controller、名前付き camera を module 内に構築。[`Camera::loadSceneCameras()`](../../src/core/renderer/camera.cpp#L725)
- 同じ object の `camera` marker と `transform` は ECS にも入り、forced [`CameraSystem`](../../src/core/ecs/predefined/camerasystem.cpp#L7) が最初の camera transform を module camera へ反映。

名前付き camera/controller と「最初の ECS camera」の責務が重なるため、camera 変更では両方を追う必要があります。`CameraSystem::process()` は現在 [`count == 0` で早期 return](../../src/core/ecs/predefined/camerasystem.cpp#L14) するようになりましたが、「先頭1件のみ使用」は変わっていません。複数 camera entity を扱う修正ではここを重点的にテストしてください。

## 9.9 Frame graph が保証するもの、しないもの

第6章の要点を、変更時の安全条件として再掲します。

### planner

- 自動 edge は宣言順で「直前 writer → reader」の RAW(read-after-write — 書いた後に読む依存。以下 WAW は write-after-write、WAR は write-after-read で、いずれも順序が入れ替わると結果が変わる組み合わせです)。
- WAW は明示 edge で全 writer を順序付けないと [`validateWritesAreOrdered()`](../../src/core/renderingpass/frameplanner.cpp#L1458) が拒否。
- WAR は自動 edge なし。
- `after` / `before` は control edge。
- cycle は例外。
- stable topological order は作るが、[`levels`](../../src/core/renderingpass/frameplanner.cpp#L1546) は現在並列実行に使わない。

### barrier

- ordered edge の同 resource write→read を barrier record にする。
- 実行側 [`bufferReadAfterWriteBarrier()`](../../src/core/renderingpass/computetask.cpp) は storage buffer に `vk::BufferMemoryBarrier` を出す。compute consumerの`command_layout: "compute_dispatch"`とrender consumerの`indexed_draw` / `draw_count`では、通常のshader readに加えて`DrawIndirect` / `IndirectCommandRead`をdestinationへ含める。
- image は layout tracker に依存。tracker のキーは `(rt_id, surface_index)` になり、history 付き target の現/旧 surface を別々に追跡します([`render_target_layout_tracker.cpp` 内](../../src/core/vkcore/render_target_layout_tracker.cpp#L51))。
- layout が変われば layout transition が memory dependency を含む。
- storage image が `GENERAL`→`GENERAL` のままなら tracker は早期 return するため、compute→compute の image RAW 専用 barrier は現在も出ない([同](../../src/core/vkcore/render_target_layout_tracker.cpp#L72))。

graph の node 順が正しいことと、Vulkan memory visibility が正しいことは別問題です。新 resource type を足すときは planner edge、runtime resource binding、stage/access mask、queue ownership の4点を一緒に設計します。

## 9.10 Compute 設定の「parse 済み」と「実行済み」を区別する

調査時点の実装範囲です。

| 設定 | parse | runtime behavior |
|---|---:|---|
| `dispatch.groups` | 済 | `vkCmdDispatch(x,y,z)` に使用 |
| `dispatch.indirect` | 済 | typed command bufferから`vkCmdDispatchIndirect`。command resourceのread edgeも自動導出 |
| `dispatch.groups_from: {"port":"..."}` | 済 | typed image port の選択 mip extent と shader reflection の local size から direct group 数を導出。resize/rebindでも再計算 |
| `dispatch.local_size` | 検出 | 二重 authority を避けるため明示 error。shader の `layout(local_size_*=...)` が authority |
| `schedule: per_frame` | 済 | 対応 |
| その他 schedule | 検出 | 明示 error |
| buffer `size > 0` | 済 | device-local storage buffer を一度確保 |
| buffer `size == 0` | 済 | graph 名だけ。実 buffer はなし |
| buffer `command_layout: compute_dispatch` | 済 | 12 byte以上を検証し`INDIRECT_BUFFER` usageを付与 |
| buffer `command_layout: indexed_draw` | 済 | packed 20 byte以上を検証し`INDIRECT_BUFFER` usageを付与 |
| buffer `command_layout: draw_count` | 済 | u32 1個以上を検証し`INDIRECT_BUFFER` usageを付与 |
| buffer `host_source: scene_draw_commands_v1` | 済 | CPU DrawQueue commandを20 byte strideへcompactし、容量超過はゼロ埋め+診断 |
| buffer `host_source: scene_draw_bounds_v1` | 済 | commandと同じflatten順でworld AABBを32 byte strideへpack。`minimum.w`がvalid flag |
| buffer `host_source: scene_draw_segments_v1` | 済 | CPU-bound state rangeをsource/output command範囲とcount slotへ対応付ける32 byte record |
| material pass `gpu_draw_source` | 済（fixed/segmented state） | fixed rangeまたは複数segmentを`drawIndexedIndirectCount`で置換。state bindはCPU、容量不足/未対応device/CPU強制はpass全体を既存DrawQueueへfallback |
| `lifetime: persistent` | 済 | 実質全 buffer が container lifetime |
| `lifetime: transient` | 済 | frame ごとの確保/recycle は未実装 |

根拠は [`computetask.cpp`](../../src/core/renderingpass/computetask.cpp) の
`parseDispatch()`、`registerBuffers()`、`registerComputeTask()`です。

画像全体または特定 mip を処理する task は、`{"groups_from":{"port":"reduced_depth"}}` のように typed image port を指定できます。port の `subresource.mip`、render target の現在 extent、shader reflection の [`local_size`](../../src/core/shader/shaderreflection.hpp) を runtime が結合し、X/Y group 数を切り上げ除算で求めます。`local_size_z` は 2D image contract のため 1 が必須です。固定 `groups`、image-derived `groups_from`、GPU-produced `indirect` は相互排他的です。粒子数のような任意の scalar parameter 由来 dispatch はこの契約へ混ぜず、typed indirect command buffer または将来の別契約で扱います。

compute task は dedicated compute queue へ submit せず、graphics frame command buffer に記録します。一方 [`pickQueues()`](../../src/core/vkcore/core.cpp#L228) の fallback は graphics と compute を別 family として受理できます。現 frame graph compute は graphics queue に compute capability があることを実質仮定していますが、fallback path はそれを必須検証していません。async compute を実装する場合は command pool/submit だけでなく queue family ownership transfer も必要です。

## 9.11 Shader reflection と hot reload の境界

reflection は descriptor layout と pipeline layout を source/SPIR-V から自動生成します。便利ですが、C++ 側の resource contract がなくなるわけではありません。

- set 0/1/2/3 の意味は [`pelican_sets.hpp`](../../src/core/shader/pelican_sets.hpp#L7) の convention。
- fullscreen/compute binder は特定 descriptor type を要求。
- material renderer は engine vertex layout/push constant の構造を前提にする。
- reflection は field の semantic を理解せず、set/binding/type/count/name だけを見る。

hot reload は shader compile と pipeline rebuild を transactional にします。しかし descriptor layout を変更する edit は、shader body だけの edit より危険です。

- shader reload の runtime 公開は **`RuntimeReloadBoundary::render_start`** の 1 点に集約されています。[`consumeShaderReloadPublication()`](../../src/core/vkcore/renderer.cpp#L2524) が `ReloadService::applyRuntimeBoundary(render_start)` を呼び、**その summary の `committed` が 0 でないときだけ** [`rebindFullscreenInputs()`](../../src/core/vkcore/renderer.cpp#L2491) が走ります。呼び出しは view の記録へ入る前([`renderer.cpp` の frame 前段](../../src/core/vkcore/renderer.cpp#L4340))で、shader 側の participant がこの boundary を宣言している箇所は [`reloadservice.cpp` の shader participant 登録](../../src/core/watch/reloadservice.cpp#L445) です。
- `rebindFullscreenInputs()` が貼り直すのは 3 系統です — 公開済み generation 内の fullscreen / generic raster pass の input resource、material の screen input、compute task の render target。したがって **reload 専用の処理ではありません**。logical target の extent が変わった直後にも同じ関数が呼ばれます([`renderer.cpp` の extent 変更後](../../src/core/vkcore/renderer.cpp#L2594))。逆に言うと、この 3 系統の外側で descriptor を自前 cache している pass は、reload でも resize でも取り残されます。
- compute descriptor set は [`registerComputeTask()`](../../src/core/renderingpass/computetask.cpp#L2375) 時に一度作り、hot reload path では作り直していません。
- material は [`MaterialContainer::prepareSurfaceMaterialReload()`](../../src/core/material/materialcontainer.hpp#L371) により surface/material 連動 reload に対応しました。UI/debug の descriptor ownership は各 container に分散したままです。

したがって hot reload の安全な基本範囲は、既存 set/binding/type と push constant layout を保った shader body の変更です。layout-changing reload を正式対応するなら、pipeline 使用者ごとの descriptor rebuild notification が必要です。

## 9.12 GPU object は「C++で不要」になった時点では壊せない

pipeline、image view、buffer などは、CPU では旧 object に見えても GPU が前 frame の command から参照中かもしれません。[`DeletionQueueCore`](../../src/core/vkcore/deletionqueue.hpp#L17) は resource type を virtual base([`DeferredResourceBase`](../../src/core/vkcore/deletionqueue.hpp#L22))へ型消去して溜めます。

**溜めた資源の寿命を決めるのは「defer した frame 番号」ではなく「実 submission への紐付け」です。** `DeletionQueueCore` には frame を数える口も、「もう解放してよいものを掃き出す」口もありません。frame を名乗る唯一の残骸は [`currentFrame()`](../../src/core/vkcore/deletionqueue.hpp#L104) ですが、返すのは完了 submission 数です。現在は次の 3 手です。

1. [`defer()`](../../src/core/vkcore/deletionqueue.hpp#L90) は現在の [`RetirementBatch`](../../src/core/vkcore/deletionqueue.hpp#L50) に積むだけ。
2. [`leaseForNextSubmission()`](../../src/core/vkcore/deletionqueue.cpp#L63) がその batch を握る [`GpuSubmissionLease`](../../src/core/vkcore/frametarget.hpp#L53)(実体は `shared_ptr<const void>`)を返す。
3. [`confirmSubmission()`](../../src/core/vkcore/deletionqueue.cpp#L73) が次の submission 用に新しい batch へ切り替える。

**破棄が走るのは lease の最後の参照が消えた瞬間**です。frame target は [`GpuSubmissionLeaseSlots`](../../src/core/vkcore/frametarget.hpp#L223) に in-flight slot ごとに lease を持ち、その slot の completion fence を待ってから [`complete(slot)`](../../src/core/vkcore/offscreenframetarget.cpp#L178) で手放します。`Renderer` 側の 3 点は [`deletion_queue.leaseForNextSubmission()`](../../src/core/vkcore/renderer.cpp#L4120) → [`target.endLogicalFrame(submission_lease)`](../../src/core/vkcore/renderer.cpp#L4752) → [submission 確定の呼び出し](../../src/core/vkcore/renderer.cpp#L4898) です。

つまり「何 frame 後に消えるか」を数えるコードは deletion queue からは消えました([`DeletionQueueCore`](../../src/core/vkcore/deletionqueue.hpp#L17) は `in_flight_frames_num` を一度も参照しません)。ただし定数そのものは健在で、[`in_flight_frames_num`](../../src/core/vkcore/rendertarget.hpp#L16) は frame target の command buffer 配列や [`GpuSubmissionLeaseSlots`](../../src/core/vkcore/offscreenframetarget.hpp#L17)、[`FrameResources::configureViewCount()`](../../src/core/renderer/frameresources.cpp#L110) の descriptor slot 数、swapchain / OpenXR / ImGui の image count など `src/` 全体で 27 か所に残っています — lease slot 専用の定数ではありません。hot reload の [`replacePipeline()`](../../src/core/shader/pipelinefactory.cpp#L1000) が代表的な defer 元です。

変更時の原則は次です。

- GPU が参照し得る旧 object を local temporary の destructor に任せない。
- new object を公開した後、old object を deletion queue へ移す。
- **新しい submit 経路を足したら、lease を取って submission 完了まで手放さない。** lease を持たずに `confirmSubmission()` を呼ぶと、その batch は下の難所ブロックの理由で**その場で**破棄されます。
- queue に入れる object が dependent object より先に破棄されても Vulkan 規約上安全か確認する。
- shutdown は wait-idle → pending flush → module destruction の順を維持する。[`flushAll()`](../../src/core/vkcore/deletionqueue.cpp#L83) は fence も wait-idle も見ずに全 batch を落とすので、呼ぶ側が先に idle にする責任を持ちます(実際 [`loop.cpp` 内](../../src/core/appflow/loop.cpp#L291) は `waitIdle()` の直後、teardown は [`runtime_teardown_order`](../../src/core/appflow/teardown.hpp#L23) の `wait_idle` を先頭・`deletion_queue` を末尾に置いています)。

### teardown 開始後の `defer()` はエラー

`DeletionQueueCore` は [`accepting` / `draining`](../../src/core/vkcore/deletionqueue.hpp#L77) フラグと [`requireAccepting()`](../../src/core/vkcore/deletionqueue.hpp#L80) を持ちます。**`defer()` だけでなく `leaseForNextSubmission()` と `confirmSubmission()` も先頭でこれを呼ぶ**ので、受け入れ停止後はこの 3 本すべてが `std::logic_error` になります([`deletionqueue_test.cpp` 内](../../test/deletionqueue_test.cpp#L129) が 3 本とも検査しています)。

teardown の最終段は phase で分岐します。

| phase | 動作 |
|---|---|
| `ModuleRuntimePhase::shutting_down` | [`drainForTeardown()`](../../src/core/vkcore/deletionqueue.hpp#L102)。以後 `defer()` 不可 |
| それ以外(`runtime_reset`) | `flushAll()`。queue は再利用可能なまま |

したがって「module の destructor から GPU object を defer する」コードは、terminal shutdown では失敗します。destructor は既に所有している資源で完結させるか、明示 teardown 段階へ移してください。受け入れ状態の確認は `acceptingResources()`(core)/ `acceptingResourcesForTesting()`(module ラッパ)です。

> 🧩 **難所 — 破棄の時刻を決めているのは refcount 1 本**([`confirmSubmission()`](../../src/core/vkcore/deletionqueue.cpp#L73) / [`flushAll()`](../../src/core/vkcore/deletionqueue.cpp#L83))
>
> **何をする所か**: defer された資源を、それを参照している実 GPU submission が完了するまで生かし、完了したものだけ本物のデストラクタへ落とします。
>
> **素朴に読むと**: 鍵は [`std::vector<std::weak_ptr<RetirementBatch>> batches`](../../src/core/vkcore/deletionqueue.hpp#L76) が **`weak_ptr` である**ことで、この vector は寿命を一切持ちません。batch を生かしている強参照は 2 本だけ — 次に積む先である [`current_batch`](../../src/core/vkcore/deletionqueue.hpp#L75) と、`leaseForNextSubmission()` が配った `GpuSubmissionLease` です。`confirmSubmission()` は `current_batch` を新しい batch で**上書き**するので、**lease を誰も持っていなければその瞬間に旧 batch の refcount が 0 になり、GPU がまだ読んでいる資源がその場で破棄されます**。「queue に積んだのだから遅延している」と読むと、この 1 本の依存が見えません。`batches` が `weak_ptr` なのは `flushAll()` のためで、生きている batch だけ `releaseAll()` し、失効したものは [`batches.erase(it)`](../../src/core/vkcore/deletionqueue.cpp#L98) で落とします — つまり `batches` は所有者ではなく「まだ破棄されていない batch の台帳」です。
>
> もう 1 つが `draining` で、`draining` を立てて 4 行の `DrainScope` で必ず倒す形は「念のための再入禁止」に見えますが、塞いでいるのは具体的な事故です。`release()` の実体は [`resource.reset()`](../../src/core/vkcore/deletionqueue.hpp#L45)、つまり**走るのは寝かせた型のデストラクタで、その中身は各 container のコード**です([`VertBufContainer::releaseModelGeometry()`](../../src/core/model/vertbufcontainer.cpp#L542) が defer する `DeferredRelease` のデストラクタは、lease を取り直したうえで owner のメソッドを呼び戻します)。そこから `defer()` が来ると [`resources.push_back`](../../src/core/vkcore/deletionqueue.hpp#L58) が走査中の vector を再確保し、`confirmSubmission()` が来ると `batches.push_back` が `flushAll()` のイテレータを無効にします。だから 3 本とも先頭で [`DeletionQueueCore::requireAccepting()`](../../src/core/vkcore/deletionqueue.cpp#L54) を呼び、drain 中は `std::logic_error` にします。これは「壊れる」を「その場で分かる」へ変える置き換えで、`DrainScope` が try-catch でなく RAII なのは、`releaseAll()` が throw しても、後から早期 return を足しても、1 か所でフラグを倒せるからです。`flushAll()` 自身の再入も同じく `std::logic_error` です。
>
> **骨子**:
> ```text
> defer(r)                 requireAccepting(); current_batch->add(r)
> leaseForNextSubmission() requireAccepting(); return shared_ptr{ upstream, current_batch }
> confirmSubmission()      requireAccepting(); ++completed_submissions
>                          batch が空なら何もしない
>                          batches.push_back(new); current_batch = new
>                          ↑ ここで旧 batch の強参照は lease だけになる
>                            lease が無ければ refcount 0 → 即破棄
>
> フレーム側                target.endLogicalFrame(lease) → slot に保持
>                          fence 完了を観測 → slots.complete(slot) → lease 解放 → 破棄
>
> flushAll()               draining=true + DrainScope(RAII で false へ戻す)
>                          batches を走査し lock() できた batch を releaseAll()
>                          ↑ この中から defer / lease / confirm が来たら logic_error
> ```
>
> **手がかり**: `flushAll()` は fence も wait-idle も見ません(待つのはデストラクタの [`wait_idle_hook()`](../../src/core/vkcore/deletionqueue.cpp#L34) と、`wait_idle` step を先に通す [`runtime_teardown_order`](../../src/core/appflow/teardown.hpp#L23) と、それを走査する [`teardownRuntimeNoThrow()`](../../src/core/appflow/teardown.cpp#L107))。`pendingCount()` は [`PendingCounter`](../../src/core/vkcore/deletionqueue.hpp#L18) を `DeferredResource` の生成/解放で増減させた live 数で、デストラクタはこれが 0 でなければ WARNING を出して wait-idle → flush する安全網に入ります([`DeletionQueueCore::~DeletionQueueCore()`](../../src/core/vkcore/deletionqueue.cpp#L22))。テストは [`"DeletionQueueCore releases resources only after the bound GPU submission completes"`](../../test/deletionqueue_test.cpp#L37)(lease を手放すまで破棄されない)/ [`"DeletionQueueCore tracks retirement batches by actual submission lease"`](../../test/deletionqueue_test.cpp#L60)(batch ごとに独立して retire する)/ [`"DeletionQueueCore teardown drain closes the queue and rejects late resources"`](../../test/deletionqueue_test.cpp#L118)(drain 後は 3 本とも throw)です。
>
> **不変条件**: submission に紐付けたい資源は `leaseForNextSubmission()` の lease を submission 完了まで保持する(保持しないなら `confirmSubmission()` の時点で破棄されてよい資源だけを defer する)。`draining` を立てる区間には必ず RAII で対になる復帰を置く。`releaseAll()` の実行中に `resources` / `batches` を触らない。`batches` を `shared_ptr` に変えない — 所有しないことが「lease だけが寿命を決める」という契約そのものです。

## 9.13 宣言・schema はあるが、runtime が未完成または別経路のもの

ソースを読むときに「型がある = 利用可能」と誤解しやすい箇所です。

| 項目 | 調査時点の状態 | コード |
|---|---|---|
| `JsonArchiveLoader` | 実装済み。scene component/event JSON load に使用 | [`jsonarchive.cpp`](../../src/core/userpublic/serialize/jsonarchive.cpp#L6) |
| `JsonArchiveSaver` | `prop` 宣言のみで、この repository 内に定義なし | [`jsonarchive.hpp`](../../src/core/userpublic/serialize/jsonarchive.hpp#L30) |
| `BinaryArchive` | `prop` 宣言のみで、この repository 内に定義なし | [`binaryarchive.hpp`](../../src/core/userpublic/serialize/binaryarchive.hpp#L10) |
| pose action | **実装済み**(WP130/132)。pose は `poses` map から返り、未サンプルなら default `ActionPose`。flat 環境では pose サンプルが来ないので default が返る点に注意 | [`actionmap.cpp`](../../src/core/os/actionmap.cpp#L867) |
| `.surface` / material format | **runtime 接続済み**(WP116/117/122)。`.surface` は surfacecompiler で pipeline に、`.material.json` は lowering を経て `MaterialContainer` へ | [`surfacecompiler.hpp`](../../src/core/shader/surfacecompiler.hpp) / [`materiallowering.hpp`](../../src/project/materiallowering.hpp) |
| Studio project editor | D2、WP264 の viewport / Outliner 選択同期、WP266 の schema-driven property Inspector、WP275 の gizmo 操作まで実装済み。project は同じ child へ `--rpc --project` で渡り、identity は名前でなく `(scene_id, declaration_index)`。property edit / gizmo / live preview / undo / redo / save は公開 RPC を使い、terminal result まで成功扱いしない。picking / gizmo feature 無効は理由を警告し、自動有効化しない | [`SelectionModel`](../../src/devstudio/model/selection.hpp) / [`GizmoModel`](../../src/devstudio/model/gizmomodel.hpp) / [`InspectorModel`](../../src/devstudio/model/inspectormodel.hpp) / [`InspectorWidget`](../../src/devstudio/view/inspectorwidget.hpp) / [`EmbeddedViewport`](../../src/devstudio/viewport/embeddedviewport.hpp#L28) |
| swapchain capture | surface が TRANSFER_SRC を持てば windowed でも readback 実装済み。不可時のみ `capture unavailable_windowed` 例外 | [`swapchainframetarget.cpp`](../../src/core/vkcore/swapchainframetarget.cpp#L435) |
| frame graph levels | 計算/JSON 出力のみ。runtime は直列 node loop | [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1380) |
| custom Component public registration | ID macro はあるが安定 public boot hook なし。ただし登録解除 API と重複拒否は入った | [`component/registerer.hpp`](../../src/core/userpublic/details/component/registerer.hpp#L20) |
| behavior attachment | ✅実装済み(WP155 / 162 / 167) | [`behaviorarena.hpp`](../../src/core/gamelogic/behaviorarena.hpp#L109) |
| 物理 trigger event | ✅実装済み(WP179) | [`PhysWorld::updateTriggers()`](../../src/core/phys/physworld.cpp#L548) |
| 編集 RPC(query / snapshot / edit / undo / preview / journal) | ✅実装済み(WP153〜172) | [`editorcommandservice.hpp`](../../src/core/communication/editorcommandservice.hpp#L222) |
| ImGui inspector / asset browser | ✅実装済み(WP159 / 164 / 167)。ただし `--rpc` / headless / replay / golden / XR では無効 | [`inspector.hpp`](../../src/core/imgui/inspector.hpp#L167) |
| preview graph(第3 variant) | 🚧実装済みだが CPU 模式ラスタ(WP172)。隔離契約が本体で、見た目の忠実度は保証しない | [`previewgraph.hpp`](../../src/core/renderingpass/previewgraph.hpp#L15) |
| RenderDoc capture | 🚧受動のみ(WP140)。**エンジンは RenderDoc をロードしない** | [`renderdoccapture.hpp`](../../src/core/renderdoc/renderdoccapture.hpp#L66) |
| VRMA decode / retarget / AnimationSource | ✅実装済み(WP176 / 177 / 178) | [`vrmadecoder.hpp`](../../src/core/loader/vrmadecoder.hpp) / [`vrmaretarget.hpp`](../../src/core/animation/vrmaretarget.hpp) |
| `.vrma` の root motion 抽出 | 📐設計スロットのみ。`VrmaRootMotionPolicy` は `preserve_hips_translation` の 1 値だけ | [`VrmaRootMotionPolicy`](../../src/core/animation/vrmaretarget.hpp#L21) |
| authoring 側のオブジェクト宣言 identity | 🚧部分。`stage()` は「object declaration identity を後続 WP まで意図的に固定」 | [`authoringscenedocument.hpp` 内](../../src/core/loader/authoringscenedocument.hpp#L117) |

optional build feature には stub 実装もあります。たとえば SeqPlayer/VAT/RPC/audio/physics/renderdoc は build option により実装または disabled behavior が選ばれます。header が同じでも build artifact の能力は [`build_features.hpp`](../../src/core/build_features.hpp#L1) と各 `*_stub.cpp` を確認してください。

## 9.14 症状から読む場所を決める

| 症状 | 最初の確認 | 次の確認 |
|---|---|---|
| 起動中に module constructor 例外 | module initialization log、[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L46) | constructor 内の `GET_MODULE` 依存 chain |
| 起動後の `GET_MODULE` で例外 | `freezeCreation()` 後の新規 module 生成でないか | `prepareRuntimeModules()` / prepare フェーズへの依存先解決の移動 |
| entity が突然無効 | [`EntityId` generation](../../src/core/userpublic/details/ecs/entity.hpp#L12)、scene transition | remove/clear と stale handle test |
| component pointer の値が別 entity になる | [`swap-delete`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) | pointer を structural mutation 越しに保持していないか |
| ECS system が動かない | matching component mask、force/version | explicit dependencies の cycle、empty chunk |
| ECS system が時々壊れる | 同 level の read/write conflict | `depends_list`、raw mutation、JobSystem race |
| ECS System が急に直列化された / 順序が変わった | ログの `ECS auto serialization: ...` WARNING | `read_indices` / `write_indices` の重なり、`predefined.cpp` の依存 |
| `--strict-assets` を付けた途端に起動しない | `ECS unordered component hazard between systems ...` | 明示 `depends_list` を足す |
| game system event が来ない | event macro が system macro より前に可視か | `onEvent` の完全な型 signature、event 短縮名 |
| behavior が動かない / `Unknown behavior type` | game DLL がロード済みか(`get_status.reload`) | scene の `behavior.type` と `PELICAN_REGISTER_BEHAVIOR` の stable name |
| behavior の `onEvent` が呼ばれない | event macro が behavior macro より前に可視か | 第2引数が `BehaviorContext&` か(`GameContext&` 版ではない) |
| `OverlapEnter` が 1 フレーム遅れる | `PhysicsTriggerSystem` は order `INT_MAX` | event は通常キュー経由なので配送は次フレーム |
| ライトが動かなくなった | engine 側の名前ベース自動アニメーションは撤去済み(§9.8) | `GameContext::set*Light*()` をユーザーコードから毎フレーム呼ぶ |
| ライトが 1 個だけ描かれない | ログの `Light cap exceeded: ...` | `MAX_*_LIGHTS` |
| ModelInstance の描画が別モデルに化ける | `ModelInstanceId{index, generation, scene_epoch}` の generation | `isModelInstanceAlive()`、scene 再ロードによる epoch 進行 |
| `save_scene` が `RuntimeOnlyData` で失敗 | `load_gltf` で transient モデルを足していないか | `SceneLoader::hasRuntimeOnlyChanges()` |
| `edit` が `stale_revision` | `get_scene_revision` の `EditorWatchToken` | preview lease が `preview_epoch` を進めていないか |
| windowed で RPC 応答が来ない | フレームが進んでいるか | queue busy 応答(`-32000` / `reason:"busy"`)が来ていないか |
| RPC event payload が空 | event に `ref(JsonArchiveLoader&)` があるか | default-only JSON loader branch |
| frame graph の順が違う | [`currentFramePlanJson()`](../../src/core/vkcore/renderer.cpp#L3254) | reads/writes、after/before、declaration index |
| XR だけ表示が壊れる | `#xr` variant の feature 除外(`xr_excluded_features`) | `graph_variant_transition_trace`、XR feature policy |
| TAA の ghosting・再投影が乱れる | temporal reset のトリガ(set_time / camera 不連続 / resize / view 数 / variant 切替) | `resetTemporalHistory()`、previous object/skin/morph buffer |
| game DLL reload 後に状態が消える/残る | `RegistrationOwner` と DLL 内 static の寿命 | engine 側 System の function-local static(こちらは残る) |
| Vulkan validation の RAW error | buffer/image、stage/access、layout | `GENERAL→GENERAL` image case、queue family |
| shader reload 後だけ壊れる | compile log、reflection diff | descriptor/push layout を変更していないか |
| resize 後だけ texture が古い | target recreate と fullscreen rebind | 該当 pass が独自 descriptor を cache していないか |
| headless capture が真っ黒 | frame plan と execution trace | target layout、pass output、golden fixture |
| shutdown crash | wait-idle と deletion queue | ECS `deinit()` が既に壊れた GPU module を触っていないか |
| defer した GPU 資源が早すぎるタイミングで消える | `leaseForNextSubmission()` の lease を submission 完了まで保持しているか(§9.12) | `flushAll()` の前に wait-idle しているか |
| テストは緑なのに機能が壊れている | `ctest` の Skipped 件数(特に `-L gpu`) | テストが `catch (const std::exception &) → SKIP` で engine の例外を skip へ変えていないか(§9.20) |

## 9.15 大きな変更の安全チェックリスト

### Component/ECS を変更する

- Component ID は一意かつ64未満か。
- lifecycle callbacks は construct された object にだけ呼ばれるか。
- init failure の逆順 rollback が保たれるか。
- remove の swap entity(穴へ移した末尾 entity)に対して `id_table[...].ref.array_index` を更新するか(§9.6)。
- raw write 後に component version を更新するか。
- 並列 system の read/write conflict に明示 dependency があるか。
- empty chunk と zero entity batch を処理できるか。

### rendering resource/pass を変更する

- JSON definition、validation、runtime compilation、execution dispatch の4層を更新したか。
- resize 後の image view rebind があるか。
- hot reload 後の descriptor rebuild があるか。
- old GPU object を遅延破棄したか。submit 経路を足したなら submission lease を取って完了まで保持したか(§9.12)。
- buffer と image の両方に正しい stage/access barrier があるか。
- headless と swapchain の両 frame target で成立するか。
- flat と `#xr` の両 graph variant で成立するか。
- temporal history(history RT、object/skin/morph/override history)の reset 経路を更新したか。
- view 数変化時の FrameResources slot 再構成(device idle 待ち)を守ったか。
- plan fixture、execution trace、golden image を確認したか。
- `gpu` ラベルのテストを走らせ、**新しい skip が 1 件も出ていない**ことを確認したか(§9.20)。`ctest` の「0 失敗」だけでは足りません。

### public API/RPC を変更する

- `GameContext` から内部 module 型を漏らしていないか。
- frame boundary のどこで反映されるかを定義したか。
- error を invalid params と application error のどちらにするか決めたか。
- stdout へ protocol 外文字列を出していないか。
- 公開する関数/型に `PELICAN_API` を付けて DLL export したか。
- game DLL ABI(`gameLogicAbiVersion`)を壊していないか。
- pure parser test と actual player subprocess test の両方があるか。

## 9.16 logical frame と XR の不変条件(WP128〜135)

### logical frame の不変条件

`renderLogicalFrame()` は次を破ると例外にします([`renderer.cpp` 内](../../src/core/vkcore/renderer.cpp#L1404)): 全 view が同じ in-flight frame index を共有すること、全 view の extent が等しいこと、target の color format がコンパイル済み graph と一致すること。FrameUBO slot は `in_flight × view_count + view` の式で選ばれます(WP128 レポート: [`docs/design_reviews/2026-07-17_wp128_report.md`](../design_reviews/2026-07-17_wp128_report.md))。

### XR mirror は「drop 可能な optional sink」

frame の開始は成否 bool ではなく [`FrameBeginResult`](../../src/core/vkcore/frametarget.hpp#L203) を返します([`IFrameTarget::beginFrame()`](../../src/core/vkcore/frametarget.hpp#L290))。判断の軸は [`FrameBeginDisposition`](../../src/core/vkcore/frametarget.hpp#L143) の 4 値で、frame が取れなかった内訳は [`FrameUnavailableReason`](../../src/core/vkcore/frametarget.hpp#L150) の `reason` に別途入ります。mirror はこれを [`classifyMirrorBeginResult()`](../../src/core/openxr/openxrmirrorsink.cpp#L58) で 3 つの行動へ落とします。

| disposition | 行動 | mirror の挙動 |
|---|---|---|
| `ready` | `present` | 描いて `submit()`。`presented` を数える |
| `unavailable` | `drop` | 何もせず return。`dropped` と `last_drop_reason` だけ進む。**エラーではありません** |
| `device_rebuild_required` / `fatal` | `disable` | WARNING を出して以後は永久に諦める。`failures` を数える |

**mirror 経路にエラー処理を足すときに、`unavailable` を error へ昇格させないでください。** 分類が見るのは `disposition` だけで `reason` は見ないので、`reason` が `surface_lost` であっても `unavailable` なら drop です([テストが 4 分類を明示的に固定しています](../../test/xrfeaturepolicy_test.cpp#L166))。surface の回復は frame target 側の仕事で、mirror から同期的に回復を呼ばないことが設計上の要求です([`docs/design_wsi_epoch_recovery.md`](../design_wsi_epoch_recovery.md) の「9.4 XR mirror」)。

mirror が best-effort でいられるのは、入力が engine 所有の中間 target `__xr_mirror_left` だけで、OpenXR swapchain image を受け取らず、`FrameBeginMode::nonblocking` で開始するため desktop swapchain を待ちもしないからです([`XrMirrorSink`](../../src/core/openxr/openxrmirrorsink.hpp#L39) 直前のコメント)。[`XrMirrorSink::tryPresent()`](../../src/core/openxr/openxrmirrorsink.cpp#L182) は全体が `noexcept` で、例外が出れば frame を [`abandon()`](../../src/core/vkcore/frametarget.hpp#L299) して自分を disable するだけです — HMD 側の logical frame は止めません。集計は [`XrMirrorSinkStats`](../../src/core/openxr/openxrmirrorsink.hpp#L29) に載り、`OpenXR mirror diagnostic: milestone=...` の INFO ログに出ます(WP133 が入れた「mirror は drop してよい」規約を、現在はこの 3 分類が担っています)。

drop は `beginFrame()` が `ready` を返した後にも起こります。letterbox 矩形が空(destination が 0 サイズ = 最小化)なら、取得済みの frame をそのまま `submit()` してから drop として数えます。**`beginFrame()` で得た frame は必ず `submit()` か `abandon()` へ渡してください** — 渡さなければ `FrameTargetFrame` のデストラクタが abandon 扱いで回収するので壊れはしませんが、どの経路で落ちたのかが追えなくなります。

### XR の forced-off は決定的

headless / RPC / golden / replay では XR は決定的に off です([`xrForcedOffDriver()`](../../src/core/xractivation.hpp#L55))。`--xr on` とこれらの組み合わせはエラー方向です([`resolveXrActivation()`](../../src/core/xractivation.hpp#L89))。「headless テストで XR コードが動かない」のはこの activation 規約によるものです。

## 9.17 編集 transaction の落とし穴(prepare / publish / CAS / ticket)

WP144〜WP172 で、変更のプロトコルがコードベース全体で統一されました。この節が第9章で最も重要な追加です。

以下は 3 層に分かれています。**(a) 1 回の編集をどう原子的に適用するか**(1〜2)、**(b) その編集をいつ受理してよいか**(3 の CAS / 4 の lease / 5 の gate)、**(c) どこで確定し、何を拒否・出力するか**(6〜8)です。(a) を束ねているのは [`EditorProjectionTransaction::commit(commands, adapters)`](../../src/core/loader/editorprojectiontransaction.hpp#L276) の 1 関数で、`base_revision` の照合 → command を staged document へ適用 → **全 adapter の `prepare()`** → **全 adapter の `publish()`** → document 公開 → 逆順に `finish()`、という並びです。どこかで例外が出れば、そこまでに `prepare()` した adapter を**逆順に `rollback()`** して `Rejected` / `Failed` を返します([`EditorProjectionTransaction::commit()`](../../src/core/loader/editorprojectiontransaction.cpp#L785))。

### 1. prepare は throw してよいが、publish は絶対に失敗できない

[`ECSArchetypeMigrationAdapter`](../../src/core/ecs/archetypemigration.hpp#L44) のコメントが規範です。

> prepare() may throw but must not change published state. rollback() is invoked for
> every attempted prepare (including the one that threw). Publication must not fail.

同じ形が各所にあります。

| 場所 | API |
|---|---|
| ECS archetype | [`ECSArchetypeMigration::prepareAdd/prepareRemove`](../../src/core/ecs/archetypemigration.hpp#L118) → token の `publish()` / `rollback()` / `finish()`(全て `noexcept`) |
| ECS entity | [`ECSEntityMutation::prepareCreate/prepareDestroy`](../../src/core/ecs/archetypemigration.hpp#L106) |
| ECS 既存値 | `prepareComponentValue()` / `publishComponentValue()` / `rollbackComponentValue()` |
| model instance | `preflightModelInstance()` → `stageModelInstance()` → `publishModelInstance()` |
| behavior | [`PreparedBehaviorAttachmentEdits`](../../src/core/gamelogic/behaviorarena.hpp#L87) の `publish()` / `rollback()` / `finish()`(全て `noexcept`) |
| physics | [`PhysWorld::prepareBindings()`](../../src/core/phys/physworld.hpp#L64) → [`publishPrepared()`](../../src/core/phys/physworld.hpp#L68)(`noexcept`) |
| 編集投影 | [`EditorProjectionPublicationMode{StagedNoexcept, InverseToken}`](../../src/core/loader/editorprojectiontransaction.hpp#L34) |

`EditorProjectionPublicationMode` は「公開をどう戻せる形にしてあるか」の宣言です。`StagedNoexcept` は publish 時点に失敗要因が残らないよう prepare 側へ寄せ切る形、`InverseToken` は publish 後でも `rollback()` が完全な逆操作を復元できる形です。`publish()` / `rollback()` / `finish()` はどちらのモードでも `noexcept` で、`rollback()` は publish 前(prepared 状態を捨てる)・publish 後(逆トークンで戻す)のどちらの経路も allocation-free であることが要求されます([`EditorProjectionAdapter`](../../src/core/loader/editorprojectiontransaction.hpp#L159))。

> **設計決定:** 新しい adapter を足すときは **prepare / rollback / publish の三点セットを必ず作ってください**。「途中まで適用された状態」を許す実装を1つ混ぜるだけで、その adapter だけでなく、**同じ `commit()` が束ねる transaction 全体**(= 編集・reload・scene 遷移が共有するこのプロトコル)の原子性が崩れます。他の adapter が正しく rollback できても、その 1 つが戻らなければ transaction は半端な状態で終わるからです。

`load_gltf` の単一公開点がわかりやすい実例です([`instances.publishModelInstance()`](../../src/core/loader/scene.cpp#L705))。

```cpp
// No operation below allocates: this is the single publication point for
// the entity's slot, draw commands, resources, and optional name.
instances.publishModelInstance(std::move(staged_instance));
```

名前バインディングも `unordered_map::node_type` を先に `extract()` して確保しておき、公開時に allocation が起きないようにしています。

### 2. 投影 adapter は 8 種で閉じている

[`EditorProjectionAdapterKind`](../../src/core/loader/editorprojectiontransaction.hpp#L20) は `EcsExistingValue` / `EcsArchetype` / `TransformClosure` / `RendererModel` / `Camera` / `Light` / `PhysWorld` / `BehaviorAttachment` の 8 種です。`archetypemigration.hpp` のコメントが境界を書いています。

> This is only a composition boundary. Light, collider, behavior, and other special
> attachments remain separate adapters owned by their respective projection WPs.

つまり ECS archetype 移行は「ECS の中だけ」を見ており、light / collider / behavior は別 adapter が責任を持ちます。

### 3. CAS は `SceneRevision` で行う。ただし revision だけでは足りない

`edit` は `base_revision` を伴い(これが節題の CAS — compare-and-swap、「読んだときの値から変わっていなければ書き換える」条件付き更新のことです)、ズレていれば `EditorEditErrorCode::stale_revision` です。**watch トークンは [`EditorWatchToken{scene_revision, preview_epoch}`](../../src/core/communication/editorcommandservice.hpp#L178) の 2 要素** で、preview の open/commit も epoch を進めます。`get_scene_revision` の戻り値を丸ごと持ち回ってください。

### 4. preview は lease(ticket)

`open_preview` → `update_preview` → `commit_preview` / `abort_preview` の流れです(lease は「開いている間その対象の編集を 1 人が占有する権利」、ticket はその lease 1 件を指す識別子です)。関連するエラーコードは [`EditorEditErrorCode`](../../src/core/communication/editorjournal.hpp#L46) の `preview_lease_conflict` / `preview_lease_busy` / `not_lease_owner` / `ticket_not_found` / `undo_conflict` です。composition root 用の緊急口として `EditorCommandService::forceAbortPreview(reason)` があります。

### 5. 編集ゲートは 5 ビット。「受理時に開いていた」は実行してよい理由にならない

[`EditorGateReason`](../../src/core/communication/editorjournal.hpp#L20) は `replay` / `golden` / `strict` / `reload_scene_transition` / `preview_lease_conflict` のビットフラグです。[`EditorGateObservation`](../../src/core/communication/editorjournal.hpp#L32) の `transition_epoch` のコメントが規範です。

> Incremented by the composition root when a reload or scene transition
> passes between acceptance and execution, even if the gate is open again.

behavior コールバック実行中 / DLL リロード中の追加ゲートは [`internal::applyBehaviorEditConcurrencyGate()`](../../src/core/communication/editorruntimefactory.hpp#L16) です。

### 6. 編集コミットはフレーム境界に 1 点だけ

[`invokeEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L122) が reload 公開の後・`freeze_events` の直前に走ります。フックは **単一所有**で、2 回目の [`installEditorCommitQueueHook()`](../../src/core/appflow/framephase.hpp#L40) は `false` を返します。未設置なら zero-state no-op で、モジュールを作りません。

### 7. 保存拒否の理由コード

[`EditorCommandErrorCode`](../../src/core/communication/editorcommandservice.hpp#L24) は 13 種です。特に注意すべきものを挙げます。

- **`RuntimeOnlyData`**: [`SceneLoader::hasRuntimeOnlyChanges()`](../../src/core/loader/scene.hpp#L71) が真のとき、つまり `load_gltf` で持ち込んだ transient モデルがあるときに出ます。「RPC で読み込んだモデルは保存できない」という意味です。
- `ExternalModification`: ディスク上の scene が外部で書き換わっていた。
- 上限は [`maxSceneSnapshotBytes = 64 MiB`](../../src/core/communication/editorcommandservice.hpp#L22)、JSON 整数の安全上限は [`maxExactEditorJsonInteger = 9007199254740991`](../../src/core/communication/editorcommandservice.hpp#L21)。

### 8. snapshot は「ファイル内容」ではなく semantic bytes

`export_scene_snapshot` の `semantic_scene_bytes` は **キー順が正規化された文字列** です。実物([`test/fixtures/editor_command_service/export_scene_snapshot_v1.json`](../../test/fixtures/editor_command_service/export_scene_snapshot_v1.json) の `expected_response`)を見ると `scenes` → `schema` → `version` の順になっています。

```json
{
  "schema_version": 1,
  "scene_revision": 42,
  "current_scene_id": "main",
  "semantic_scene_bytes": "{\"scenes\":{\"main\":{\"objects\":[]}},\"schema\":\"pelican.scene\",\"version\":1}",
  "digest": {"algorithm": "sha256", "hex": "353a3317a58437bb4c5a0d746a29afa378d1d743532ed699b8740e0ecc4bcedf"},
  "pending_ticket_ids": [],
  "preview_epoch": 7
}
```

外部ツールで digest を再計算するときは `AuthoringSceneDocument::encodeSemantic()` と同じ正規化が必要です。**ファイルを読んで sha256 を取っても一致しません。**

## 9.18 windowed RPC ホストの落とし穴

- `--rpc` は `--headless` 無しでも通るようになりましたが、**ImGui UI は無効になります**([`isImGuiRuntimeEnabled()`](../../src/core/imgui/imguiruntime.cpp#L9) が `!config.rpc` を要求)。windowed RPC と ImGui inspector は排他です。
- リクエストは **フレーム境界でしか処理されません**。windowed は毎フレーム進むので通常は問題になりませんが、ウィンドウ最小化などでフレームが止まると応答も止まります。
- queue 容量 [`64`](../../src/core/communication/rpcserver.hpp#L99) を超えると即座に `-32000` / `data.reason == "busy"` が返ります。この応答は **reader スレッドから** 出るため、engine スレッドの応答と行が混ざり得ます。順序保証は JSON-RPC の `id` に依存してください。
- reader スレッドは `std::istream` にキャンセル手段がないため、ブロックしたまま `detach()` されることがあります。テストで `WindowedRpcHost` を使い捨てるときは入力ストリームを閉じて `readerFinished()` を待ってください。本番は process 寿命の `std::cin` 前提です。

## 9.19 preview / `render_preview` の隔離

- `render_preview` は **`Renderer::renderLogicalFrame()` を通りません**([`PreviewGraphProgram`](../../src/core/renderingpass/previewgraph.hpp#L20) 直前のコメント)。したがって temporal history、FrameResources slot、layout tracker などのライブ状態を汚しません。
- 汚していないことの証明が [`previewStateInventory()`](../../src/core/vkcore/previewexecutor.hpp#L59)(「並び順も診断契約の一部」)と [`Renderer::previewIsolationStateJson()`](../../src/core/vkcore/renderer.cpp#L3160) です。
- 上限は 2048px / 16 MiB([`previewexecutor.hpp` 内](../../src/core/vkcore/previewexecutor.hpp#L54))。超過は `PreviewCaptureTooLarge` です。
- `PreviewCaptureRequest::graph_generation` が `PreviewGraphProgram::generation` と食い違えば `std::invalid_argument("preview graph generation mismatch")` で拒否されます(stale preview の防止)。
- **現在の出力は CPU 模式ラスタです**([第6章 §6.19](06_rendering_vulkan_shader.md))。material も shader も評価しないので、`render_preview` の画像を最終描画の代用と見なさないでください。

## 9.20 テストの `catch (const std::exception &) → SKIP` は、回帰を緑のまま隠す

この章で扱ってきた不変条件のほとんどは、**破ると engine が `throw std::runtime_error` する**形で守られています。fail-fast がこのコードベースの検出手段です。一方、GPU を要するテストの多くは device の無い環境で落ちないよう、次の形を持っています。

```cpp
TEST_CASE("...") {
    try {
        // ... テスト本体 ...
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan ... unavailable: "} + error.what());
    }
}
```

**この 2 つが噛み合うと、engine が正しく検出した回帰が、そのまま「環境が無い」という skip になります。** skip したテストは赤くなりません。落ちたテストより見つけにくい、というのがこの落とし穴の本体です。

### 実際に起きたこと(WP241)

2026-07-31 時点で、`gpu` ラベルの 122 件のうち 4 件がこの形で skip しており、`ctest` は同じ実行を「0 失敗」と報告していました。残る 118 件は同じ実行で実 device 上を通っているので、**device 不在による skip ではありません**。

| skip していたテスト | 握り潰されていた engine の例外 |
|---|---|
| [`rpc_color_contract_test.cpp` 内](../../test/rpc_color_contract_test.cpp#L853) | [`MaterialContainer::validateRuntimeGenerationCompatibility()`](../../src/core/material/materialcontainer.cpp#L3181) の `render-pipeline candidate has no compatible pass for live material 0 (route 'deferred_geometry', shader contract 'gbuffer_v1')` |
| [`materialvaluesreload_test.cpp` 内](../../test/materialvaluesreload_test.cpp#L368) | [`validateMaterialTextureReflection()`](../../src/core/material/materialcontainer.cpp#L928) の `material texture 'albedo_detail' is absent from shader reflection at binding 7` |
| [`materialvaluesreload_test.cpp` 内](../../test/materialvaluesreload_test.cpp#L490) | 同上 |
| [`HR1-M watcher gate and 1000 reloads keep resources bounded`](../../test/materialvaluesreload_test.cpp#L765) | 同上 |

`try` の位置は 2 通りありました。`materialvaluesreload_test.cpp` の 3 件は `FastModuleContainer modules;` から `waitIdle()` まで**本体まるごと**([`"HR1-M updates one same-layout material and rolls back invalid candidates"`](../../test/materialvaluesreload_test.cpp#L368) の `try` など)、`rpc_color_contract_test.cpp` は engine 起動部だけ([`runEngineRpcServer()`](../../test/rpc_color_contract_test.cpp#L376) を囲む `try`)ですが、fail-fast は起動時に出るので結果は同じです。

> 🧩 **難所 — 捕まるものと捕まらないものが逆に見える**(`catch (const std::exception &)` と Catch2)
>
> **何をする所か**: 「device が無ければ skip、あれば実行」を 1 つの `try/catch` で表現しようとしています。
>
> **素朴に読むと**: 「全部囲んでいるのだから、テストが失敗しても skip になってしまうのでは」と読み、逆に「失敗が握り潰されている」報告を見ると「`REQUIRE` が効いていないのでは」と読みます。どちらも外れです。Catch2 の `REQUIRE` 失敗が投げるのは `Catch::TestFailureException`、`SKIP()` が投げるのは `Catch::TestSkipException` で、**どちらも `std::exception` を継承していない空の struct** です(`catch2/internal/catch_test_failure_exception.hpp`)。だから `catch (const std::exception &)` は**アサーション失敗を一切拾いません**。拾うのは engine が投げる `std::runtime_error` のような `std::exception` 由来の例外だけ — この非対称が誤解の源です。
>
> さらに 1 段ややこしいのは、**engine の throw が `REQUIRE(...)` の式の中で起きた場合は捕まらない**ことです。`REQUIRE` の展開は式評価を自前の `try { ... } catch(...) { handleUnexpectedInflightException(); }` で囲んでおり、そこで失敗として記録したうえで `TestFailureException` を投げ直します。つまり **同じ例外でも、素の文として投げられれば skip、`REQUIRE` の中から投げられれば失敗**になります。上の 4 件が skip になったのは前者だったからで、テストの書き手が意図して分けた区別ではありません(この分岐は `CATCH_CONFIG_FAST_COMPILE` を定義すると消え、その場合は `REQUIRE` 内の例外も外側の `catch` へ届きます。このリポジトリでは未定義です)。
>
> **骨子**:
> ```text
> engine の throw(素の文)      → std::runtime_error → 外側の catch が拾う → SKIP  ← 隠れる
> engine の throw(REQUIRE 内)  → Catch2 が記録 → TestFailureException  → 素通り → 失敗
> REQUIRE(a == b) の不一致      → TestFailureException                 → 素通り → 失敗
> SKIP(...)                     → TestSkipException                    → 素通り → skip
> ```
>
> **手がかり**: 「capability が無い」と「実行して失敗した」を分ける方法は2つです。(1) **明示的な能力問い合わせ** — [XR segmented draw の multiview capability 検査](../../test/golden_harness.cpp#L9191) / [`.dynamic_rendering_local_read`](../../test/headless_render_test.cpp#L3276) を見て skip し、本体は囲まない。(2) **device 初期化エラーを厳密に絞って再送出** — [`requireVulkanDevice()`](../../test/vulkan_test_support.hpp) は `No suitable Vulkan physical device found` のときだけ skip し、それ以外は `throw;` します。後始末の catch も同じ判定を使い、非該当なら再送出します。**(1) が本来の形**で、(2) は既存テストを最小限の変更で救う形です。
>
> **不変条件**: skip は「実行できない理由」を**問い合わせて**決める。`std::exception` を捕まえて skip にしない。どうしても囲むなら、囲む範囲を bring-up だけに限り、bring-up を抜けたら再送出する。

### この idiom がどこにあるか

`test/*.cpp` で `catch (const std::exception ...)` のハンドラ本体に `SKIP(` を持つブロックは調査時点で 42 か所あり、そのうち再送出・メッセージ絞り込み・`runtime_ready` ラッチのいずれかで判断を狭めているのは 7 か所だけでした。WP241 後は broad handler を除去し、直接 `SKIP` を含む残りは device 不在を絞って再送出する3か所だけです。探すときはこの形で当たれます。

```powershell
rg -n -A3 "catch \(const std::exception" test --glob "*.cpp" | rg -B1 "SKIP\("
```

### gate 側の受け止め

skip が緑を汚さない以上、`ctest` の exit code だけでは検出できません。そのため gate 側が **許可した名前以外の skip をすべて失敗にする**方針を持っています。判定は [`validate_skip_policy()`](../../test/ci/skip_policy.py#L74)(CPU/GPU 両 gate 共通)で、GPU 側の driver が [`run_gpu_gate.py`](../../test/ci/run_gpu_gate.py)、許可リストが [`test/ci/gpu_skip_allowlist.txt`](../../test/ci/gpu_skip_allowlist.txt) です。現在の GPU allowlist は、clone / worktree 外の corpus を `PELICAN_TEST_PROJECTS_DIR` で指定する `project_catalog_headless_smoke` の未指定 skip だけを載せています。上の 4 件の broad exception skip は載せません。entry を消せば未指定時の skip が非許可になり、逆にテスト名自体が現れなければ stale entry として失敗するので、リストは腐りません([`skip_policy.py` 内](../../test/ci/skip_policy.py#L83))。

なお `gpu` ラベルは Catch2 の `[gpu]` タグではなく CTest の LABELS で、付き方が 2 通りある点に注意してください。Catch2 テストは [`pelican_define_test(<name> GPU ...)`](../../test/CMakeLists.txt#L30) が target 単位で付け、CTest 名は [`catch_discover_tests()`](../../test/CMakeLists.txt#L124) が `TEST_CASE` の文字列をそのまま使います。もう一方は `add_test()` で登録した e2e / player テストに [`set_tests_properties(seqplayer_headless_player PROPERTIES LABELS gpu)`](../../test/CMakeLists.txt#L1230) の形で個別に付けるもので(現在 22 か所)、この場合の CTest 名は `add_test()` の名前です。allowlist は完全一致の名前を要求するので、どちらの経路で付いたラベルかで書くべき名前が変わります。gate の起動方法は [`docs/ci.md`](../ci.md) にあります。

---

Pelican の複雑さは、ECS と Vulkan そのものよりも「compile-time 型情報を runtime table へ落とす境界」と「CPU 上の寿命を frame/GPU 上の寿命へ写す境界」に集まっています。その2か所では、便利な macro や RAII の表面だけでなく、登録時刻、pointer の有効期間、barrier、破棄順まで追うのが安全です。
