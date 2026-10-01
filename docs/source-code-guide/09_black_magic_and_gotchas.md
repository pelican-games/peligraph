# 第9章 マクロ・型消去・変更時の注意

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

[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L84) の先頭で local `FastModuleContainer` を作ります。その destructor は、初期化と逆順に module の `optional.reset()` を行います。

```text
GET_MODULE(A) -> A constructor内でGET_MODULE(B)
初期化: Aのemplace開始 -> B登録 -> A登録
cleaners: [B, A]
破棄: A -> B
```

依存を constructor 内で取得すれば、通常は dependent が先に壊れます。ただし GPU/ECS は destructor だけへ任せず、[`RuntimeTeardownGuard`](../../src/core/appflow/teardown.cpp#L148) が明示 cleanup を先に行います。

> 🧩 **難所 — 遅延生成の 49 行**([`FastModuleContainer::get<T>()`](../../src/core/container.hpp#L149))
>
> **何をする所か**: module の遅延生成です。初回だけ default 構築して cleaner を積み、2 回目以降は同じ参照を返します。
>
> **素朴に読むと**: この 49 行には**独立した仕掛けが 5 つ同居**していて、どれか 1 つを知らないと「なぜこの順序なのか」が読めません。1 つずつ分けると次のとおりです。
>
> 1. **速い経路は lock を取らない(double-checked locking)** — 二重チェックロックとは、lock を取らずにフラグを読み、まだ初期化前に見えたときだけ lock を取って**もう一度**確かめる方式です。ここでは `__ready()` がそのフラグで、初期化済みなら lock 無しで参照を返します。
> 2. **公開は [`store(release)`](../../src/core/container.hpp#L192) の 1 点だけ** — `emplace` と `cleaners.push_back` の**両方**が終わった後に置いてあります。`ready` を先に立てると「ready なのに cleaner が無い module」ができ、teardown で破棄されずに漏れます。これは書く順序の話です。
> 3. **その 1 点が `release`、読む側が `acquire`** — 生成は owner スレッド限定でも読み取りは他スレッドから自由なので([`test` 内](../../test/module_container_test.cpp#L147))、`release` で書き `acquire`([`isInitialized()` 側](../../src/core/container.hpp#L141) / [`tryGet()` 側](../../src/core/container.hpp#L145) / [`get()` の速い経路](../../src/core/container.hpp#L151))で読む対にして、「`ready` が true に見えたスレッドからは、その前に済ませた `emplace` も必ず見える」を保証します。`relaxed` へ緩めると、`ready` だけが先に見えて未構築の optional を掴み得ます。2 と 3 は**別々の理由が同じ 1 行に重なっている**ので、片方だけ直しても壊れます。
> 4. **[`state_mutex`](../../src/core/container.hpp#L62) が `recursive_mutex`** — `obj_ref.emplace()` が走らせる `T` のコンストラクタの中で `GET_MODULE(U)` が呼ばれ、同じスレッドが `get()` へ再入するからです。ただの `mutex` なら自己デッドロックします。
> 5. **宣言位置と検査順そのものが意味を持つ** — `ConstructionScope`([`ConstructionScope`](../../src/core/container.hpp#L174))を `construction_stack.push_back` の直後・`emplace()` の直前に置くのは、コンストラクタが throw しても必ず pop させるためです。依存辺の記録を自分を積む前に行うのは `construction_stack.back()` を「親」にするため、循環検出を `requireCreationAllowedLocked()`([`requireCreationAllowedLocked()`](../../src/core/container.hpp#L98))より先に置くのは freeze 済みでも「循環」という正しい診断を出すためです。
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
- **[`FastModuleContainer::freezeCreation()`](../../src/core/container.hpp#L199)**(Loop 開始直前、[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L538) で呼ばれる)以降は新規 module 生成が禁止されます。「render 中に初めて `GET_MODULE` する」コードは freeze 後に失敗するため、`Renderer::prepareRuntimeModules()` / `prepareFrameStateModules()` のように起動時に依存を先解決するパターンが必須です。生成しない読み取りには [`tryGet<T>()`](../../src/core/container.hpp#L144) があります。shutdown 側には `beginShutdown()`(#L213)が加わりました。
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

世代付きの識別子は増えました。ECS の `EntityId`(index + generation)に加え、[`ModelInstanceId`](../../src/core/renderer/modelinstance.hpp#L11) が `index + generation + scene_epoch` になり(WP146 / INSTANCE0)、[`RegistrationToken`](../../src/core/userpublic/details/reload/registrationowner.hpp#L29) と `RegistrationOwner` も identity + generation です。

**それでも `ResourceContainer` 系の GPU ハンドルだけは依然として世代なし** です。GPU resource handle と、世代付きの Entity / ModelInstance / Registration handle を同じ寿命モデルだと考えないでください。

> 🧩 **難所 — generation は 0 を跨がない**([`ResourceGenerationLedger`](../../src/core/animation/animationserviceabi.hpp#L40) / [`isValid()`](../../src/core/userpublic/animation/abi_v1.hpp#L48))
>
> **何をする所か**: animation ABI ハンドルの世代を進めます。`if (++x == 0) ++x;` という同じ 3 語が [`animationservice.cpp`](../../src/core/animation/animationservice.cpp#L649) だけで 7 か所に現れます。
>
> **素朴に読むと**: `isValid()` が `identity != 0 && generation != 0 && reserved == 0` で判定するので、**generation 0 は「無効ハンドル」の予約値**です。予約値とは、値域の中の 1 つを通常の意味から外し、「特別な状態」を表すためだけに取っておくことを指します。0 が選ばれている効果ははっきりしていて、`memset` やゼロ初期化で作った struct が**そのまま無効**として読まれます(この ABI は §9.2 の `struct_size` 交渉のとおり、呼び出し側にゼロ初期化を要求します)。1 を予約値にするとゼロ初期化した handle が有効に見えてしまいます。**この選択の理由を書いた設計文書は見つかっていません(未確認)** — ここに書いたのは実装から読み取れる帰結です。`uint32` の世代が一周して 0 に戻ると、生きているリソースのハンドルが突然「無効」になる — `if (++x == 0) ++x;` はその 1 値だけを飛ばすイディオムですが、1 行に畳まれているので「なぜ 2 回インクリメントするのか」がコードからは読めません。もう 1 段深いのが `ResourceGenerationLedger` で、リソースが消えたあとも **identity を消さずに世代だけ覚え続けます**([`tombstone()`](../../src/core/animation/animationserviceabi.cpp#L36) は世代を 1 進めて `remember()` する)。これが無いと消えたリソースのハンドルは「そんな identity は知らない」= `invalid_handle` に落ちますが、`invalid_handle` は「そのハンドルは初めからおかしい」、`stale_generation` は「正しかったが古い」で、**呼び出し側の回復経路が違います**。tombstone を外すと、モデルの hot reload のたびに [`EvaluatorV1::rebind()`](../../src/core/userpublic/animation/animgraph.cpp#L1148) での再解決ができなくなり、anim graph がパラメータもクロックも遷移も失います。
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
> **手がかり**: [`resolveRigResource()`](../../src/core/animation/animationservice.cpp#L176) は世代を **2 系統**で照合します — ledger の記録と、アセット側の `generation_state->current.load()` です。前者は「サービスが知っている世代」、後者は「アセットが実際に進めた世代」で、reload の途中では一時的に食い違い得ます(片方だけ見ると差し替え途中のアセットを掴みます)。pose だけは arena 所有でリソース台帳に載らないため、[`stale_poses`](../../src/core/animation/animationposearena.hpp#L64) という別表へ退避されます(arena も別表も、`animationservice.cpp` から切り出された [`PoseArena`](../../src/core/animation/animationposearena.hpp#L23) の持ち物です)。テストは [`animgraph_test.cpp` 内](../../test/animgraph_test.cpp#L467)(reload 後に各 API が `stale_generation` を返し、`rebind()` が graph 状態を保つ)、[`animation_jobs_test.cpp` 内](../../test/animation_jobs_test.cpp#L330)。
>
> **不変条件**: generation 0 を有効値として外へ出さない。消えたリソースの identity を ledger から**消さない**(tombstone を残す)。`invalid_handle` と `stale_generation` の使い分けを崩さない。

### `ModelInstanceId` の落とし穴

`ModelInstanceId` は 3 フィールドの比較可能な値型で、`toString()` は `"index:generation@scene_epoch"` を返します([`toString()`](../../src/core/renderer/modelinstance.hpp#L21))。

- **identity は `PolygonInstanceContainer` から [`ModelInstanceSlots`](../../src/core/renderer/modelinstanceslots.hpp#L22) へ切り出されました。** `index` / `generation` / `scene_epoch` を持っているのはこの class で、`PolygonInstanceContainer` 側にはもう `scene_epoch` も世代表もありません(`instance_slots` メンバ越しに問い合わせます)。
- `scene_epoch` が進むのは [`ModelInstanceSlots::clearPrepared()`](../../src/core/renderer/modelinstanceslots.cpp#L107) の **1 か所だけ** で、これは [`PolygonInstanceContainer::clear()`](../../src/core/renderer/polygoninstancecontainer.cpp#L924)(scene clear / 再ロード)から呼ばれます。個別 slot が死んで再利用されるときに進むのは [`ModelInstanceSlots::retire()`](../../src/core/renderer/modelinstanceslots.cpp#L87) が上げる slot generation の方です。いずれも animation generation や model asset content revision からは独立です。
- `clear()` 自体も §9.17 の prepare/publish 形です。epoch 枯渇(`scene epoch exhausted`)を投げうる検査は [`ModelInstanceSlots::prepareClear()`](../../src/core/renderer/modelinstanceslots.cpp#L98) が先に済ませ、実際の書き換えは `noexcept` の `clearPrepared()` が行います。`clear()` を読むときはこの 2 段を 1 つの操作として読んでください。
- 生存確認は [`isModelInstanceAlive(id)`](../../src/core/renderer/polygoninstancecontainer.hpp#L573)。
- [`removeModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L517) は `void` ではなく **`bool`** を返します。戻り値を無視すると「消したつもりで消えていない」を見逃します。
- `instanceCountForTesting()` は **live 数** を返すよう変わり、スロット総数は `slotCountForTesting()` です。両者の差は空きスロットです。

登録は 3 段階に分かれました(WP144 / TRANSIENT0)。[`preflightModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L511)(Vulkan 資源確保前の容量拒否)→ [`stageModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L512) → [`publishModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L515)(`noexcept`、**唯一の no-fail 公開点**)です。詳細は §9.17。

> 🧩 **難所 — `struct_size` の 3 段ルール**([`getApiV1()`](../../src/core/animation/animationservice.cpp#L1830) / [`validateDescriptor()`](../../src/core/animation/animationserviceabi.hpp#L15))
>
> **何をする所か**: 別々にコンパイルされた DLL とエンジンの間で、C の plain struct を版下位互換のまま受け渡します。呼び出し側が `struct_size` を書き、エンジンが自分の知る分だけを書き戻します。
>
> **素朴に読むと**: `validateDescriptor(*desc)` がほぼ全 API の頭に並ぶので「サイズは常に交渉されている」と読んでしまいますが、実際には **3 種類の異なるルールが混ざっています**。交渉の入口 3 本([`getApiV1`](../../src/core/animation/animationservice.cpp#L1830) / [`Impl::getService`](../../src/core/animation/animationservice.cpp#L1678) / [`getPoseStagingServiceV1`](../../src/core/animation/animationservice.cpp#L1855))は `sizeof(DescriptorHeaderV1)` = 16 バイトしか要求せず、加算的テールを持つ [`getClipMetadata`](../../src/core/animation/animationservice.cpp#L1513) は凍結プレフィクスの長さを手書きし、**それ以外は既定の `sizeof(T)`(= エンジン側の現在サイズ)を下限**として要求します(検査は [`struct_size < minimum`](../../src/core/animation/animationserviceabi.hpp#L17) の下限比較だけなので、大きすぎる `struct_size` は弾かれません。[`ProbeRuntime` 側の同名関数](../../src/core/animation/animationprobe.cpp#L38)は既定が `headerSize` である点も別物です)。つまり既存 descriptor の末尾にフィールドを足すと、古い DLL は即 `invalid_argument` です — 「加算的だから安全」という一般則はこのコードベースでは成り立ちません。書き戻しは `memcpy(out, &produced, min(caller_size, sizeof(produced)))` で **エンジンは呼び出し側のテールをゼロ埋めしません**から、呼び出し側は descriptor を必ずゼロ初期化してから使う必要があります(テストの `descriptor<T>()`、[`descriptor()`](../../test/animation_abi_dll_test.cpp#L44))。成功後の `out->struct_size` は**エンジンの `sizeof`** に上書きされるので、自分が確保したバッファ長として再利用しないでください。
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
> **手がかり**: 版数の軸が 3 本あります — `version`(descriptor のレイアウト版)、`engine_abi_version` / `service_version`(関数表の意味論の版)、`capability_bits`(機能単位)。`reserved0/1 != 0` を `reserved_not_zero` で弾くのは、新しい ABI の呼び出し側が古いエンジンに当たったことを検出するためです。[`getApiV1()`](../../src/core/userpublic/animation/abi_v1.hpp#L653) の `static_assert` は 2 つのサイズと 4 か所のオフセットだけを固定しており、**それ以外のフィールド順はコンパイラが何も守ってくれません**。テストは [`old and new animation header DLLs preserve the frozen prefix`](../../test/animation_abi_dll_test.cpp#L124)(凍結プレフィクス)/ [`old-client new-engine and new-client old-engine negotiate additively`](../../test/animation_abi_dll_test.cpp#L154)(old-client と new-engine の双方向)で、`storage[caller_size]` の番兵によって越境書き込みが無いことを検査しています。
>
> **不変条件**: 既存フィールドの順序・型・サイズを変えない(追加は末尾のみ)。末尾に足したら、その descriptor を読む側の `minimum` を凍結プレフィクスのサイズへ明示的に下げる(前例は engine 側で 5 か所 — [`getClipMetadata`](../../src/core/animation/animationservice.cpp#L1513) と、同じ descriptor を検証する `ProbeRuntime` 側の [advanceCursor](../../src/core/animation/animationprobe.cpp#L256)(desc / result の 2 つ)/ [publishAnimationFrame](../../src/core/animation/animationprobe.cpp#L448) / [advanceTemporalHistoryAfterRender](../../src/core/animation/animationprobe.cpp#L467)。片側だけ直すと取りこぼします)。書き戻しは必ず `min(caller_size, sizeof(produced))` にする。ABI 面の関数は `noexcept` を保ち、例外を `Status` へ変換する。

## 9.3 `PELICAN_REGISTER_EVENT` / `PELICAN_REGISTER_SYSTEM` / `PELICAN_REGISTER_BEHAVIOR`

これらは runtime reflection ではなく、C++ の static object constructor と compile-time overload lookup の組み合わせです。

### event 登録の展開

[`PELICAN_REGISTER_EVENT_IMPL`](../../src/core/userpublic/details/event/registerer.hpp#L393) は概念的に2つを生成します(macro 本体は [`PELICAN_REGISTER_EVENT`](../../src/core/userpublic/details/event/registerer.hpp#L408))。

1. `EventCatalogTag<N>` に対する `pelicanEventCatalogEntry(...) -> EventCatalogEntry<MyEvent>` overload。
2. anonymous namespace の static object。constructor で `registerEvent<MyEvent>("MyEvent")`。

`N` は `__COUNTER__` です。

### system 登録の展開

[`PELICAN_REGISTER_SYSTEM_IMPL`](../../src/core/userpublic/details/system/registerer.hpp#L185) も static object を作ります(macro 本体は [`PELICAN_REGISTER_SYSTEM`](../../src/core/userpublic/details/system/registerer.hpp#L203))。その constructor は、system macro より前に見えている event catalog entry を `0..N-1` まで compile-time に探索します。`System` に `onEvent(const Event&, GameContext&)` があれば function pointer table へ追加し、最後に system 本体を登録します。生成される static object は登録の戻り値である [`RegistrationToken token;`](../../src/core/userpublic/details/system/registerer.hpp#L188) をメンバとして保持します。

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
> 仕掛けは 2 段で、**「番号 N の event があるか」を調べる段**と、**「どの範囲の宣言が見えるか」を決める段**です。前者が下の SFINAE、後者が次項の二相名前解決で、混ぜて読むと解けません。
>
> **第 1 段: 存在検査(SFINAE)**。本体 `{ return {}; }` は飾りで、**意味があるのは戻り値型の `decltype` だけ**です。その番号の event が登録されていなければ戻り値型の置換に失敗し、`operator()<Index>` が ill-formed になる — これを requires 式で bool にして「あるかどうか」を判定しています。これが **SFINAE**(スフィネ。"Substitution Failure Is Not An Error" の略で、テンプレート引数を当てはめた結果おかしな型になっても**コンパイルエラーにはせず、その候補を黙って外す**という C++ の規則。「エラーにならない」性質を逆手に取って、存在検査に使う常套手段です)です。`if constexpr` で存在しない番号を黙って捨てているのも必須で、これが無いと `__COUNTER__` の抜け番(他のマクロが消費した番号)でビルドごと落ちます。素朴に「全 event を実行時に走査」する実装にすると `onEvent` の有無を実行時に判定できず、`virtual onEvent` にすると event 型ごとに vtable が要ります。この方式は両方を避けています。
>
> **第 2 段: なぜ「前だけ」見えるのか(二相名前解決)**: 呼び出し引数がテンプレート引数 `Index` に依存するため、**二相名前解決**(テンプレートの名前解決を「定義を書いた位置」と「型が決まって実体化される位置」の 2 段階で行う C++ の規則)になります。候補になるのは「マクロを展開した位置での通常の名前探索」と「実体化時の **ADL**(実引数依存探索 — 引数の型が属する名前空間も探しに行く仕組み)」の和です。ADL が探すのは引数の型が属する `Pelican::internal` だけですが、マクロが生む `pelicanEventCatalogEntry` の宣言は展開先(普通はグローバルかゲームの名前空間)にあるので **ADL では見つかりません**。つまり ADL 側の寄与は常に空で、実際に効くのは展開位置での通常の名前探索だけ — 結果として「同じ翻訳単位で、登録マクロより前にある宣言だけ」が見えます。逆に言うと、`EventCatalogTag` をマクロの展開先と同じ名前空間へ移すと ADL の寄与が空でなくなり、**ADL は実体化時点で探す**ので、登録マクロより後ろに書かれた overload まで拾えるようになります。
>
> **手がかり**: `unique_id`(= `__COUNTER__`)は **catalog の添字と走査上限を兼ねます**。overload は宣言だけで、本体は一度も呼ばれません。走査コストはテンプレート実体化 O(`__COUNTER__`) なので、event ヘッダを大量に include した翻訳単位で system を登録すると、その TU だけコンパイルが目に見えて遅くなります。behavior 側([behavior/registerer.hpp](../../src/core/userpublic/details/behavior/registerer.hpp#L136))は完全に同じ構造の複製なので、片方を直すなら両方直します。
>
> **不変条件**: `EventCatalogTag` は `Pelican::internal` に置いたままにする(マクロの展開先と同じ名前空間へ動かすと ADL 経路が生えて「後ろの event も見える」ようになり、TU 順序依存が静かに変わります)。ハンドラの第 2 引数は system が `GameContext&`、behavior が `BehaviorContext&` — 取り違えると concept が false になり、**コンパイルは通るのに登録されません**。

### behavior 登録の展開

[`PELICAN_REGISTER_BEHAVIOR(Type, stable_name, schema_version)`](../../src/core/userpublic/details/behavior/registerer.hpp#L385)(IMPL は [`PELICAN_REGISTER_BEHAVIOR_IMPL()`](../../src/core/userpublic/details/behavior/registerer.hpp#L364))も **まったく同じ `__COUNTER__` + catalog 走査方式** です。[`collectBehaviorEventHandlers<Type>()`](../../src/core/userpublic/details/behavior/registerer.hpp#L148) が `onEvent(const Event&, BehaviorContext&)` を探します。

したがって **翻訳単位の順序の罠がそのまま適用されます**。event 宣言 header は behavior の `.cpp` の先頭で include してください。加えて 2 点、System とは違う注意があります。

- handler の第2引数は `BehaviorContext&` です。`GameContext&` 版を書くと、compile は通るのに **catalog に載らず、呼ばれません**。
- `onEvent` は `Behavior` の virtual メンバではありません。`override` を付けられない代わりに、typo が黙って無視されます。

### system 登録の第3のフック

[`HasGameSystemQueuedEvent`](../../src/core/userpublic/details/system/registerer.hpp#L48)(`void dispatchQueuedEvent(const QueuedEvent&, GameContext&)`)が加わりました。これにより「update も onEvent も無い System は登録エラー」の条件は `!has_queued_event && event_handlers.empty()` に変わっています([`registerer.hpp` 内](../../src/core/userpublic/details/system/registerer.hpp#L107))。

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

runtime update 順は static 初期化順ではなく、[`sortGameSystemRegistrations()`](../../src/core/userpublic/details/system/registerer.cpp#L35) が `(order, name)` で決めます。ここは決定論的です。Behavior もこの全順序に order 50 の 1 点として参加します([第5章 §5.13](05_gameplay_and_services.md))。

registry の各登録には [`RegistrationOwner`](../../src/core/userpublic/details/system/registerer.hpp#L33)(engine / game DLL)が付きます。game DLL reload では [`unregisterGameSystems(owner)`](../../src/core/userpublic/details/system/registerer.hpp#L172) が旧 DLL の static 登録を外し、新 DLL の static 初期化が再登録します。個別解除用に [`unregisterGameSystem(token)`](../../src/core/userpublic/details/system/registerer.hpp#L171) もあります。また event 型は静的記述子 `pelican_payload`([`EventPayloadDescriptor`](../../src/core/userpublic/details/event/payloadschema.hpp#L43) / 実体化後は [`EventPayloadSchema`](../../src/core/userpublic/details/event/payloadschema.hpp#L26))で payload の field schema を宣言できるようになり、`fail_*` fixture は compile-time 検証([`run_event_schema_compile.cmake`](../../test/run_event_schema_compile.cmake))になっています。

### event 名と RPC payload

[`eventDisplayName()`](../../src/core/userpublic/details/event/registerer.cpp#L22) は namespace qualifier を落とします。`foo::Changed` と `bar::Changed` は同じ `Changed` になり、異なる型なら duplicate error です。

RPC の `inject_event` 用 JSON loader は [`registerEvent<Event>()`](../../src/core/userpublic/details/event/registerer.hpp#L144) で選ばれます。

- default constructible かつ `ref(JsonArchiveLoader&)` がある: payload を field へ load。
- default constructible だが `ref` がない: default event を作り、渡された payload は使わない。
- default constructible でない: name injection 不可。

C++ の `GameContext::emit(event)` は copy した値をそのまま queue に置くため、この JSON 制約とは別です。

> 🧩 **難所 — void payload の消し方**([`QueuedEvent::payload`](../../src/core/userpublic/details/event/registerer.hpp#L37) / [`dispatchEventToRegisteredGameSystems()`](../../src/core/userpublic/details/system/registerer.cpp#L97))
>
> **何をする所か**: 任意の event 型を `shared_ptr<const void>` 1 個へ消して queue に積み、配送時に `std::type_index` で照合してから元の型へ `static_cast` して呼び戻します。
>
> **素朴に読むと**: thunk(サンク — 型を消した引数を本来の型へ戻して本体を呼ぶだけの、極小の中継関数)側の `*static_cast<const Event *>(event)`([system/registerer.hpp](../../src/core/userpublic/details/system/registerer.hpp#L65))には**何の検査もありません**。型の健全性を担保しているのは配送側の [`handler.event_type == event.type`](../../src/core/userpublic/details/system/registerer.cpp#L101) の 1 行だけで、ここが唯一の関門であるという事実はコードの見た目からは分かりません。次に、`shared_ptr<const void>` は「型を捨てたポインタ」ではありません — `make_shared<EventType>` の control block(`shared_ptr` が参照カウントと「どう破棄するか」を保持している内部ブロック)が `~EventType` を持ったまま `const void` へ暗黙変換されています。DLL 境界での破棄は次の 3 行になります。
>
> 1. ゲーム DLL の中で `emit<E>(e)` が `make_shared<E>` を呼び、その瞬間に **`~E` の実体(= DLL 内のコード)へのポインタ**が control block に焼き込まれる。
> 2. engine 側は `const void` として持ち回るので `E` を知らないが、参照カウントが 0 になったとき control block が**その焼き込まれたコード**を呼ぶ。
> 3. したがって `FreeLibrary` でそのコードが消えた後に最後の参照が落ちると、**存在しないアドレスへジャンプします**。破棄は必ず `FreeLibrary` より前でなければなりません。
>
> (`void*` + `delete` にすると 1 の情報がそもそも残らず即 UB になる、というのがこの型を選んでいる理由です。)この順序を守らせているのが、一見無意味に見える [`drainForTeardown()`](../../src/core/userpublic/details/event/registerer.cpp#L511)(空の局所 vector に swap して個数だけ返す)は、**owner がまだ生きているこのスコープ内で全 payload を破棄する**のが目的です。[`unregisterEvents()`](../../src/core/userpublic/details/event/registerer.cpp#L618) が `catch (...) { std::terminate(); }` するのも同じ理由で、例外を握り潰して `FreeLibrary` へ進むとアンロード済みの型を指す登録が残ります。
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
> **手がかり**: [`QueuedEvent::owner`](../../src/core/userpublic/details/event/registerer.hpp#L41) は DLL reload 時に「消えた owner の queued event」を捨てるためにあります(`unregisterEvents` の `std::erase_if`)。一緒に読むテストは [`Event bus freeze is separate from delivery`](../../test/eventlayer_test.cpp#L69)「Event bus freeze is separate from delivery」と、[`run_behavior_dll_reload.ps1`](../../test/run_behavior_dll_reload.ps1) の `queued_event_purge` ケースです。
>
> **不変条件**: 配送前の `event_type == event.type` 照合を外さない。teardown の順序は `drainForTeardown()` / `unregisterEvents(owner)` → その後に DLL アンロード(逆にしない)。`payload` を `shared_ptr<const void>` 以外(生ポインタ・`any`・自前 deleter)へ変えない — control block が型のデストラクタを運ぶことが DLL 境界の安全性そのものです。

### system instance の寿命

[`gameSystemInstance<System>()`](../../src/core/userpublic/details/system/registerer.hpp#L53) は function-local static です。module container と違い、`PelicanCore::run()` ごとには再生成されません。同一 process で engine を複数回 run する test/tool では、System の member state が明示 reset されない限り次の run に残ります。ただしこの注意が残るのは **engine 側の System のみ**です。game System は DLL 内 static なので、DLL reload 後は新インスタンスになり、member state は持ち越されません。

## 9.4 `GameObjects::add()` の typestate builder

[`GameObjects`](../../src/core/userpublic/gameobjects.hpp#L31) は fluent API に見えますが、内部では呼ぶたびに別 template 型を返します。

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

`finish()` で初めて ID span と populate lambda へ落ち、[`GameObjects::create()`](../../src/core/userpublic/gameobjects.cpp#L14) から ECS の transactional create へ入ります。template の目的は、runtime に型情報が消えた `void*` 配列へ、compile-time に正しい型と index で代入することです。

### reference lifetime の罠

値付き `addComponent(const T&)` は、値を copy して builder に保存するのではなく [`std::tuple<const T&>`](../../src/core/userpublic/gameobjects.hpp#L108) を保存します。

```cpp
// 安全: temporaryが破棄される前、同じfull-expressionでfinishする
auto id = GameObjects::add().addComponent(MyComponent{...}).finish();

// 危険: builderだけ保存すると内部referenceがdanglingになる
auto builder = GameObjects::add().addComponent(MyComponent{...});
auto id2 = builder.finish();
```

builder を後で使うなら、component value を別の lvalue として `finish()` まで生存させてください。重複 Component ID は最終的に [`createEntities()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L365) が例外にします。

## 9.5 Component type erasure と lifecycle callback

ECS chunk は C++ 型を知りません。登録時に [`registerComponent<T>()`](../../src/core/userpublic/details/component/registerer.hpp#L185) が次を callback に変えます。

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

non-trivial component の swap-delete(削除した要素の穴へ配列の末尾要素を移して末尾を縮める方式。詰め直しが 1 要素で済む代わりに、並び順は保たれません)は assignment ではなく、destination へ move-construct し source を destroy します。[`relocate` lambda](../../src/core/userpublic/details/component/registerer.hpp#L139) と [`VariedArray::removeAt()`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) が対になっています。

制約は compile-time に固定されます。

- default constructible。
- nothrow move constructible。
- nothrow destructible。
- `deinit()` があるなら `noexcept`。

これにより create rollback と remove/teardown を `noexcept` cleanup として実装できます。`init()` だけは例外を許し、既に init 済みの component を逆順 deinit して transaction を戻します。

### Component ID は dense index でもある

[`ComponentInfoManager::getIndexFromComponentId()`](../../src/core/ecs/componentinfo.cpp#L131) は値としては ID をそのまま index に使いますが、**現在は `get(id)` 経由になりました**。未登録スロットの読み出しは [`getFromIndex()`](../../src/core/ecs/componentinfo.cpp#L134) が `std::out_of_range("component index N is not registered")` を投げます。黙って壊れた metadata を返すことはありません。さらに archetype mask(archetype は「同じ component の組み合わせを持つ entity をまとめた区画」で、mask はその組み合わせを component ID 1 個 = 1 ビットとして表した値)は [`MAX_COMPONENTS = 64`](../../src/core/userpublic/details/ecs/componentdeclare.hpp#L21) の `uint64_t` です。

したがって Component ID は次を満たす必要があります。

- process 全体で一意。
- `0..63` の範囲。
- 同じ型は全 translation unit で同じ ID specialization を見る。

**登録側の検査は全面的に強化されました**(WP163 / ECS1)。[`ComponentInfoManager::registerComponent()`](../../src/core/ecs/componentinfo.cpp#L25) は以下をすべて例外で拒否します。旧版の「黙って slot を上書きする」挙動はもうありません。

| 条件 | 文言 |
|---|---|
| 空名 | `component registration name must not be empty` |
| ID >= 64 | `component '<name>' id N exceeds the registration limit (<64)` |
| ID 重複 | `component '<name>' duplicates id N already registered by '<other>'` |
| 名前重複 | `component name '<name>' duplicates registered id N while incoming id is M` |
| lifecycle metadata 欠落 | `component '<name>' registration requires typed lifecycle metadata` |

ID 割当を engine-wide schema として管理する必要がある点は変わりませんが、間違いは **起動時に落ちて分かる** ようになりました。

登録解除 API も入りました。[`unregisterComponent(token)`](../../src/core/ecs/enginecomponentregistrar.hpp#L31) / [`unregisterComponents(owner)`](../../src/core/ecs/enginecomponentregistrar.hpp#L32) / `componentRegistrationCount(owner)` です。ただし [`ComponentInfoManager::unregisterComponent()`](../../src/core/ecs/componentinfo.cpp#L86) は **依存 System が残っていると拒否** します。

```text
cannot unregister component '<name>': dependent ECS system '<sys>' remains
```

この 3 つは WP379a で game DLL 公開面から host 専用 header へ移りました([`ecs/enginecomponentregistrar.hpp`](../../src/core/ecs/enginecomponentregistrar.hpp#L13))。game DLL 側は宣言が見えないので build できません。owner を自分で確保する [`allocateRegistrationOwner()`](../../src/core/registrationownerhost.hpp#L13) も同じく host 専用で、game DLL が観測できるのは loader が選んだ [`currentRegistrationOwner()`](../../src/core/userpublic/details/reload/registrationowner.hpp#L65) だけです。これを保つのが build 拒否テスト 2 本(`wp379a_game_dll_rejects_engine_owner` / `wp379a_game_dll_rejects_owner_allocator`、[test/CMakeLists.txt](../../test/CMakeLists.txt#L2505))で、実 SHARED DLL を build して期待した診断で落ちなければ失敗します。

### custom Component の現状

型 ID 宣言 macro は公開 header にありますが、runtime 登録入口は [`internal::getComponentRegisterer()`](../../src/core/userpublic/details/component/registerer.hpp#L216) です。production で自動登録されるのは [`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L22) の built-in 群です。

game code 向けの安定した `PELICAN_REGISTER_COMPONENT` public macro/boot hook は **現在も存在しません**(リポジトリ全体を grep しても不在)。`DECLARE_COMPONENT` しただけでは `ComponentInfoManager` に metadata が入らず、create/scene load できません。テストは internal API を直接呼んで登録しています。[`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L192) が例です。

なお `registerComponent<T>()` の戻り値は `void` から [`RegistrationToken`](../../src/core/userpublic/details/component/registerer.hpp#L185) へ変わりました。

## 9.6 Archetype/SoA chunk と pointer lifetime

各 [`VariedArray`](../../src/core/userpublic/details/ecs/chunk.hpp#L25) は constructor で `stride * 4096` bytes を一度に aligned allocation します。[`chunk.cpp`](../../src/core/userpublic/details/ecs/chunk.cpp#L17) を参照してください。同じ component の値は連続し、system は `T* + count` で batch 処理できます。

storage 自体は chunk の生存中に再 allocation されません。しかし pointer が永久に同じ entity を指すわけではありません。

- entity remove は穴へ末尾 entity を move する swap-delete。
- 別 entity の remove でも、自分が末尾なら自分の値が別 address へ移る。
- 移った entity は **id 表の側も直す**必要があります。`EntityId` から行を引くのは [`id_table[id.index].ref`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L108)(= `{chunk_index, array_index}`)なので、[`remove()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L550) は末尾 entity を穴へ move した直後に、その entity の `ref` を穴の位置へ書き換えます([swap-delete の実体](../../src/core/userpublic/details/ecs/coretemplate.cpp#L561))(例: E1 が row 5、末尾の E2 が row 10 で、E1 を remove すると E2 は row 5 へ移るので、`id_table[E2.index].ref.array_index` を 10 → 5 にする)。この更新を落とすと E2 は消えた row 10 を指したままになり、[`resolve()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L308) の範囲検査で当面は「いない」扱い、その後 create が row 10 を埋めると別 entity の行を指します。
- scene load/clear は全 component を破棄。
- `GameObjectId` は generation で再解決できるが、生 pointer には generation がない。

したがって component pointer/reference は、structural mutation をまたいで cache しないでください。必要時に EntityId から [`tryComponent()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L363) で再取得します。

create/remove/clear の再入は [`MutationScope`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L297) が拒否します。component `init()` / `deinit()` callback からさらに structural mutation すると、部分更新を防ぐため例外になります。

## 9.7 内部 ECS scheduler の並列性

内部 ECS system は template 引数の pointer constness から read/write component index を抽出します。[`registerSystem()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L418) で `const T*` は read、`T*` は write です。現在の signature は instance 参照を受け取り `SystemId` を返す形で、登録時に [`static_assert`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L419) が「batch か per-chunk のどちらかの process 形」を必須にします。

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

[`predefined.cpp` 内](../../src/core/ecs/predefined.cpp#L44) の現在の依存です(全て `registerSystemForce`)。

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

non-force system は matching chunk の component version と `last_run_tick` を比較します。component を raw pointer から直接書き換えただけでは version が上がりません。公開 mutation は [`setComponent<T>()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L373) または [`markComponentChanged()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L640) を通す必要があります。

一方、built-in system の多くは `registerSystemForce` なので毎 frame 動きます。最適化時に force を外すなら、すべての mutation 経路が version を更新するか先に確認してください。

## 9.8 Camera、light、collider はすべて同じ ECS component ではない

scene の `components` 配列に見えても runtime binding は一様ではありません。[`prepareSceneBindings()`](../../src/core/loader/scene.cpp#L305) が分岐します。系統は **4 つ**になりました。

| scene name | runtime 経路 |
|---|---|
| `transform`, `simplemodelview`, `camera` | ComponentInfo 経由で ECS chunk へ作成 |
| `light` | ECS へ入れず `LightLoadEntry` として `LightContainer::prepareLoad()` → `publishPrepared()`(scene load は 2 段を直接使い、`load()` は通らない) |
| `collider` | ECS へ入れず `ColliderComponent` を parse し `PhysWorld::prepareSceneBindings()` → `publishPrepared()`。`PELICAN_WITH_PHYSICS` OFF の build では collider を含む scene は prepare で `physics.module_unavailable@1`([`resolvedObjectFeatureRefusal()`](../../src/core/loader/resolvedscene.cpp#L78)。codec の `required_feature` を見る共有の検査で、editor の `add_component` / `spawn` も同じ関数を通る。PG-1) |
| `behavior` | ECS へ入れず [`prepareSceneBehaviorAttachments()`](../../src/core/gamelogic/behaviorarena.cpp#L95) 経由で `BehaviorAttachmentArena` へ([`component_name == "behavior"`](../../src/core/loader/scene.cpp#L382))。`type` は非空文字列必須。game DLL 未ロードなら **pending** 扱いで警告のみ |

`ColliderComponent` に `init/deinit` があっても、現在の scene loader は special case です。`ECSCoreTemplatePublic::tryComponent<ColliderComponent>()` で取れる通常 ECS component だとは考えないでください。behavior も同様で、ECS の component として問い合わせても見つかりません。

behavior は scene load executor(第 3 章 §3.5 の 3。WP380a)の participant の 1 つです。candidate は [`prepareSceneReconciliation()`](../../src/core/loader/scene.cpp#L1539) で作られ、publish 段で `noexcept` に swap され([`behavior_candidate->publish()`](../../src/core/loader/scene.cpp#L1546))、`onInit` は activate 段([`behavior_candidate->activate()`](../../src/core/loader/scene.cpp#L1548))で走ります。activate は executor で**唯一の失敗しうる段**で、投げると [`rollback` ラムダ](../../src/core/loader/scene.cpp#L1734) が renderer / ECS / camera / light / physics / behavior / event outbox / RNG / pending scene を**全部**旧世代へ戻し、結果は [`PreCommitFailure{phase = activate}`](../../src/core/loader/scene.cpp#L1789) です。document / projection / current scene / authority の commit([`metadata.commit()`](../../src/core/loader/scene.cpp#L635))はその後なので旧世代のままです。「behavior 型名を間違えると scene が半分だけロードされる」「document は新、runtime は旧」ということはありません(型名の誤りはそもそも prepare 段で落ちます)。旧経路の `clearRuntimeScene()` は `load()` から呼ばれません。

### ライトのマジックネームは撤去された(WP142 / LIGHT0)

以前は engine が `"KeyLight"` / `"FillLight"` / `"PointLight1"` / `"SpotLight1"` といった名前を特別扱いし、時刻を渡すと engine 側が勝手にアニメーションさせていました。**この挙動と、原本値を保持していた配列は削除されました。**

現在の [`LightContainer`](../../src/core/light/lightcontainer.hpp#L29) には **時間を引数に取る更新関数がありません**。ライト値を書き換える口は、scene load([`prepareLoad()`](../../src/core/light/lightcontainer.hpp#L46) → [`publishPrepared()`](../../src/core/light/lightcontainer.hpp#L48) の §9.17 型。`load()` はこの 2 段に警告出力を挟んだ入口です)と、名前指定の setter 4 本([`setDirectionalLightDirection()`](../../src/core/light/lightcontainer.hpp#L79) ほか。名前が引けなければ **`false` を返すだけ** で例外にはなりません)の 2 系統だけです。`update()` の overload 群は shadow 行列と sky ambient を受け取って現在値を GPU バッファへ書き出すもので、値そのものは動かしません。

- 代替は公開 API 4 本([`RenderContext::setDirectionalLightDirection()` ほか](../../src/core/userpublic/gamecontext.hpp#L140))で、ライトの時間変化は **ユーザー空間の責務** になりました。実例は [`projects/example/code/playercontrol.cpp`](../../projects/example/code/playercontrol.cpp#L31) の `updateLightAnimation()` です。
- setter の結果は `Renderer` の per-frame [`updateFrameLights()`](../../src/core/vkcore/renderer.cpp#L493)(呼び出しは [ここ](../../src/core/vkcore/renderer.cpp#L1554))で GPU バッファへ反映されます。
- 「アップグレード後にライトが動かなくなった」は仕様です。scene 名に依存した暗黙アニメーションを期待しているコードを探してください。

代わりに **上限超過の警告** が入りました。[`collectLightCapWarnings()`](../../src/core/light/lightcontainer.hpp#L27) が `MAX_DIRECTIONAL_LIGHTS` / `MAX_POINT_LIGHTS` / `MAX_SPOT_LIGHTS` を超えた分について次を出します([`lightcontainer.cpp` 内](../../src/core/light/lightcontainer.cpp#L55))。

```text
Light cap exceeded: <type> light #<ordinal> '<name>' will not be rendered (cap <N>)
```

`<name>` が空なら `<unnamed>` です。「ライトを足したのに 1 個だけ描かれない」ときはこの WARNING を先に探してください。テストは [`test/lightpolicy_test.cpp`](../../test/lightpolicy_test.cpp) です。

### camera の二重経路

- `Camera::loadSceneCameras()` が scene document を再走査し、projection、controller、名前付き camera を module 内に構築。[`Camera::loadSceneCameras()`](../../src/core/renderer/camera.cpp#L403)
- 同じ object の `camera` marker と `transform` は ECS にも入り、forced [`CameraSystem`](../../src/core/ecs/predefined/camerasystem.cpp#L7) が最初の camera transform を module camera へ反映。

名前付き camera/controller と「最初の ECS camera」の責務が重なるため、camera 変更では両方を追う必要があります。`CameraSystem::process()` は現在 [`count == 0` で早期 return](../../src/core/ecs/predefined/camerasystem.cpp#L14) するようになりましたが、「先頭1件のみ使用」は変わっていません。複数 camera entity を扱う修正ではここを重点的にテストしてください。

## 9.9 Frame graph が保証するもの、しないもの

第6章の要点を、変更時の安全条件として再掲します。

### planner

- 自動 edge は宣言順で「直前 writer → reader」の RAW(read-after-write — 書いた後に読む依存。以下 WAW は write-after-write、WAR は write-after-read で、いずれも順序が入れ替わると結果が変わる組み合わせです)。
- WAW は明示 edge で全 writer を順序付けないと [`validateWritesAreOrdered()`](../../src/core/renderingpass/frameplanner.cpp#L1505) が拒否。
- WAR は自動 edge なし。
- `after` / `before` は control edge。
- cycle は例外。
- stable topological order は作るが、[`levels`](../../src/core/renderingpass/frameplanner.cpp#L1593) は現在並列実行に使わない。

### barrier

- ordered edge の同 resource write→read を barrier record にする。
- 実行側 [`bufferReadAfterWriteBarrier()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1926) は storage buffer に `vk::BufferMemoryBarrier` を出す。compute consumerの`command_layout: "compute_dispatch"`とrender consumerの`indexed_draw` / `draw_count`では、通常のshader readに加えて`DrawIndirect` / `IndirectCommandRead`をdestinationへ含める。
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

根拠は [`parseDispatch()`](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L828)、[`registerBuffers()`](../../src/core/renderingpass/framegraphbuffercontainer.cpp#L33)、[`registerComputeTask()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1297) です
(旧 `computetask.cpp` はこの 3 ファイルに分割されました)。

画像全体または特定 mip を処理する task は、`{"groups_from":{"port":"reduced_depth"}}` のように typed image port を指定できます。port の `subresource.mip`、render target の現在 extent、shader reflection の [`local_size`](../../src/core/shader/shaderreflection.hpp) を runtime が結合し、X/Y group 数を切り上げ除算で求めます。`local_size_z` は 2D image contract のため 1 が必須です。固定 `groups`、image-derived `groups_from`、GPU-produced `indirect` は相互排他的です。粒子数のような任意の scalar parameter 由来 dispatch はこの契約へ混ぜず、typed indirect command buffer または将来の別契約で扱います。

compute task は dedicated compute queue へ submit せず、graphics frame command buffer に記録します。一方 [`pickQueues()`](../../src/core/vkcore/core.cpp#L228) の fallback は graphics と compute を別 family として受理できます。現 frame graph compute は graphics queue に compute capability があることを実質仮定していますが、fallback path はそれを必須検証していません。async compute を実装する場合は command pool/submit だけでなく queue family ownership transfer も必要です。

## 9.11 Shader reflection と hot reload の境界

reflection は descriptor layout と pipeline layout を source/SPIR-V から自動生成します。便利ですが、C++ 側の resource contract がなくなるわけではありません。

- set 0/1/2/3 の意味は [`pelican_sets.hpp`](../../src/core/shader/pelican_sets.hpp#L15) の convention。
- fullscreen/compute binder は特定 descriptor type を要求。
- material renderer は engine vertex layout/push constant の構造を前提にする。
- reflection は field の semantic を理解せず、set/binding/type/count/name だけを見る。

hot reload は shader compile と pipeline rebuild を transactional にします。しかし descriptor layout を変更する edit は、shader body だけの edit より危険です。

- shader reload の runtime 公開は **`RuntimeReloadBoundary::render_start`** の 1 点に集約されています。[`consumeShaderReloadPublication()`](../../src/core/vkcore/renderer.cpp#L2772) が `ReloadService::applyRuntimeBoundary(render_start)` を呼び、**その summary の `committed` が 0 でないときだけ** [`rebindFullscreenInputs()`](../../src/core/vkcore/renderer.cpp#L2737) が走ります。呼び出しは view の記録へ入る前([`renderer.cpp` の frame 前段](../../src/core/vkcore/renderer.cpp#L5767))で、shader 側の participant がこの boundary を宣言している箇所は [`reloadservice.cpp` の shader participant 登録](../../src/core/watch/reloadservice.cpp#L679) です。
- `rebindFullscreenInputs()` が貼り直すのは 3 系統です — 公開済み generation 内の fullscreen / generic raster pass の input resource、material の screen input、compute task の render target。したがって **reload 専用の処理ではありません**。logical target の extent が変わった直後にも同じ関数が呼ばれます([`renderer.cpp` の extent 変更後](../../src/core/vkcore/renderer.cpp#L2848))。逆に言うと、この 3 系統の外側で descriptor を自前 cache している pass は、reload でも resize でも取り残されます。
- compute descriptor set は [`registerComputeTask()`](../../src/core/renderingpass/computetaskcontainer.cpp#L1297) 時に一度作り、hot reload path では作り直していません。resource port の binding も同じで、RS-5 から登録のときに shader の反映の名前で決まりますが、hot reload の pipeline の組み直しは保存した desc の binding のまま行い、名前で引き直しません。
- material は [`MaterialContainer::prepareSurfaceMaterialReload()`](../../src/core/material/materialcontainer.hpp#L377) により surface/material 連動 reload に対応しました。UI/debug の descriptor ownership は各 container に分散したままです。

したがって hot reload の安全な基本範囲は、既存 set/binding/type と push constant layout を保った shader body の変更です。layout-changing reload を正式対応するなら、pipeline 使用者ごとの descriptor rebuild notification が必要です。

## 9.12 GPU object は「C++で不要」になった時点では壊せない

pipeline、image view、buffer などは、CPU では旧 object に見えても GPU が前 frame の command から参照中かもしれません。[`DeletionQueueCore`](../../src/core/vkcore/deletionqueue.hpp#L17) は resource type を virtual base([`DeferredResourceBase`](../../src/core/vkcore/deletionqueue.hpp#L22))へ型消去して溜めます。

**溜めた資源の寿命を決めるのは「defer した frame 番号」ではなく「実 submission への紐付け」です。** `DeletionQueueCore` には frame を数える口も、「もう解放してよいものを掃き出す」口もありません。frame を名乗る唯一の残骸は [`currentFrame()`](../../src/core/vkcore/deletionqueue.hpp#L105) ですが、返すのは完了 submission 数です。現在は次の 3 手です。

1. [`defer()`](../../src/core/vkcore/deletionqueue.hpp#L91) は現在の [`RetirementBatch`](../../src/core/vkcore/deletionqueue.hpp#L50) に積むだけ。
2. [`leaseForNextSubmission()`](../../src/core/vkcore/deletionqueue.cpp#L63) がその batch を握る [`GpuSubmissionLease`](../../src/core/vkcore/frametarget.hpp#L53)(実体は `shared_ptr<const void>`)を返す。
3. [`confirmSubmission()`](../../src/core/vkcore/deletionqueue.cpp#L78) が次の submission 用に新しい batch へ切り替える。

**破棄が走るのは lease の最後の参照が消えた瞬間**です。frame target は [`GpuSubmissionLeaseSlots`](../../src/core/vkcore/frametarget.hpp#L223) に in-flight slot ごとに lease を持ち、その slot の completion fence を待ってから [`complete(slot)`](../../src/core/vkcore/offscreenframetarget.cpp#L178) で手放します。`Renderer` 側の 3 点は [`deletion_queue.leaseForNextSubmission()`](../../src/core/vkcore/renderer.cpp#L5584) → [`target.submitLogicalFrame(submission_lease)`](../../src/core/vkcore/renderer.cpp#L6250) → [submission 確定の呼び出し](../../src/core/vkcore/renderer.cpp#L5546) です(それぞれ `LogicalFrameRecorder` の `collectPassPlan()` / `submitFrame()` / `sealRetirementBatch()` に属し、出力の公開は別 step の [`publishFrame()`](../../src/core/vkcore/renderer.cpp#L6254) です)。

つまり「何 frame 後に消えるか」を数えるコードは deletion queue からは消えました([`DeletionQueueCore`](../../src/core/vkcore/deletionqueue.hpp#L17) は `in_flight_frames_num` を一度も参照しません)。ただし定数そのものは健在で、[`in_flight_frames_num`](../../src/core/vkcore/rendertarget.hpp#L16) は frame target の command buffer 配列や [`GpuSubmissionLeaseSlots`](../../src/core/vkcore/offscreenframetarget.hpp#L17)、[`FrameResources::configureViewCount()`](../../src/core/renderer/frameresources.cpp#L110) の descriptor slot 数、swapchain / OpenXR / ImGui の image count など `src/` 全体で 27 か所に残っています — lease slot 専用の定数ではありません。hot reload の [`replacePipeline()`](../../src/core/shader/pipelinefactory.cpp#L1363) が代表的な defer 元です。

変更時の原則は次です。

- GPU が参照し得る旧 object を local temporary の destructor に任せない。
- new object を公開した後、old object を deletion queue へ移す。
- **新しい submit 経路を足したら、lease を取って submission 完了まで手放さない。** lease を持たずに `confirmSubmission()` を呼ぶと、その batch は下の難所ブロックの理由で**その場で**破棄されます。
- queue に入れる object が dependent object より先に破棄されても Vulkan 規約上安全か確認する。
- shutdown は wait-idle → pending flush → module destruction の順を維持する。[`flushAll()`](../../src/core/vkcore/deletionqueue.cpp#L88) は fence も wait-idle も見ずに全 batch を落とすので、呼ぶ側が先に idle にする責任を持ちます(実際 [`loop.cpp` 内](../../src/core/appflow/loop.cpp#L309) は `waitIdle()` の直後、teardown は [`runtime_teardown_order`](../../src/core/appflow/teardown.hpp#L23) の `wait_idle` を先頭・`deletion_queue` を末尾に置いています)。

### teardown 開始後の `defer()` はエラー

`DeletionQueueCore` は [`accepting` / `draining`](../../src/core/vkcore/deletionqueue.hpp#L78) フラグと [`requireAccepting()`](../../src/core/vkcore/deletionqueue.hpp#L81) を持ちます。**`defer()` だけでなく `leaseForNextSubmission()` と `confirmSubmission()` も先頭でこれを呼ぶ**ので、受け入れ停止後はこの 3 本すべてが `std::logic_error` になります([`deletionqueue_test.cpp` 内](../../test/deletionqueue_test.cpp#L156) が 3 本とも検査しています)。

teardown の最終段は phase で分岐します。

| phase | 動作 |
|---|---|
| `ModuleRuntimePhase::shutting_down` | [`drainForTeardown()`](../../src/core/vkcore/deletionqueue.hpp#L103)。以後 `defer()` 不可 |
| それ以外(`runtime_reset`) | `flushAll()`。queue は再利用可能なまま |

したがって「module の destructor から GPU object を defer する」コードは、terminal shutdown では失敗します。destructor は既に所有している資源で完結させるか、明示 teardown 段階へ移してください。受け入れ状態の確認は `acceptingResources()`(core)/ `acceptingResourcesForTesting()`(module ラッパ)です。

> 🧩 **難所 — 破棄の時刻を決めているのは refcount 1 本**([`confirmSubmission()`](../../src/core/vkcore/deletionqueue.cpp#L78) / [`flushAll()`](../../src/core/vkcore/deletionqueue.cpp#L88))
>
> **何をする所か**: defer された資源を、それを参照している実 GPU submission が完了するまで生かし、完了したものだけ本物のデストラクタへ落とします。
>
> **素朴に読むと**: 鍵は [`std::vector<std::weak_ptr<RetirementBatch>> batches`](../../src/core/vkcore/deletionqueue.hpp#L77) が **`weak_ptr` である**ことで、この vector は寿命を一切持ちません。batch を生かしている強参照は 2 本だけ — 次に積む先である [`current_batch`](../../src/core/vkcore/deletionqueue.hpp#L75) と、`leaseForNextSubmission()` が配った `GpuSubmissionLease` です。`confirmSubmission()` は `current_batch` を新しい batch で**上書き**するので、**lease を誰も持っていなければその瞬間に旧 batch の refcount が 0 になり、GPU がまだ読んでいる資源がその場で破棄されます**。「queue に積んだのだから遅延している」と読むと、この 1 本の依存が見えません。`batches` が `weak_ptr` なのは `flushAll()` のためで、生きている batch だけ `releaseAll()` し、失効したものは [`batches.erase(it)`](../../src/core/vkcore/deletionqueue.cpp#L103) で落とします — つまり `batches` は所有者ではなく「まだ破棄されていない batch の台帳」です。
>
> もう 1 つが `draining` で、`draining` を立てて 4 行の `DrainScope` で必ず倒す形は「念のための再入禁止」に見えますが、塞いでいるのは具体的な事故です。`release()` の実体は [`resource.reset()`](../../src/core/vkcore/deletionqueue.hpp#L45)、つまり**走るのは寝かせた型のデストラクタで、その中身は各 container のコード**です([`VertBufContainer::releaseModelGeometry()`](../../src/core/model/vertbufcontainer.cpp#L584) が defer する `DeferredRelease` のデストラクタは、lease を取り直したうえで owner のメソッドを呼び戻します)。そこから `defer()` が来ると [`resources.push_back`](../../src/core/vkcore/deletionqueue.hpp#L58) が走査中の vector を再確保し、`confirmSubmission()` が来ると `batches.push_back` が `flushAll()` のイテレータを無効にします。だから 3 本とも先頭で [`DeletionQueueCore::requireAccepting()`](../../src/core/vkcore/deletionqueue.cpp#L54) を呼び、drain 中は `std::logic_error` にします。これは「壊れる」を「その場で分かる」へ変える置き換えで、`DrainScope` が try-catch でなく RAII なのは、`releaseAll()` が throw しても、後から早期 return を足しても、1 か所でフラグを倒せるからです。`flushAll()` 自身の再入も同じく `std::logic_error` です。
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
> **手がかり**: `flushAll()` は fence も wait-idle も見ません(待つのはデストラクタの [`wait_idle_hook()`](../../src/core/vkcore/deletionqueue.cpp#L34) と、`wait_idle` step を先に通す [`runtime_teardown_order`](../../src/core/appflow/teardown.hpp#L23) と、それを走査する [`teardownRuntimeNoThrow()`](../../src/core/appflow/teardown.cpp#L122))。`pendingCount()` は [`PendingCounter`](../../src/core/vkcore/deletionqueue.hpp#L18) を `DeferredResource` の生成/解放で増減させた live 数で、デストラクタはこれが 0 でなければ WARNING を出して wait-idle → flush する安全網に入ります([`DeletionQueueCore::~DeletionQueueCore()`](../../src/core/vkcore/deletionqueue.cpp#L22))。テストは [`"DeletionQueueCore releases resources only after the bound GPU submission completes"`](../../test/deletionqueue_test.cpp#L37)(lease を手放すまで破棄されない)/ [`"WP384 DeletionQueue keeps the old generation until its submission fence"`](../../test/deletionqueue_test.cpp#L60)(batch ごとに独立して retire する)/ [`"DeletionQueueCore teardown drain closes the queue and rejects late resources"`](../../test/deletionqueue_test.cpp#L145)(drain 後は 3 本とも throw)です。
>
> **不変条件**: submission に紐付けたい資源は `leaseForNextSubmission()` の lease を submission 完了まで保持する(保持しないなら `confirmSubmission()` の時点で破棄されてよい資源だけを defer する)。`draining` を立てる区間には必ず RAII で対になる復帰を置く。`releaseAll()` の実行中に `resources` / `batches` を触らない。`batches` を `shared_ptr` に変えない — 所有しないことが「lease だけが寿命を決める」という契約そのものです。

## 9.13 宣言・schema はあるが、runtime が未完成または別経路のもの

ソースを読むときに「型がある = 利用可能」と誤解しやすい箇所です。

| 項目 | 調査時点の状態 | コード |
|---|---|---|
| `JsonArchiveLoader` | 実装済み。scene component/event JSON load に使用 | [`jsonarchive.cpp`](../../src/core/userpublic/serialize/jsonarchive.cpp#L6) |
| `JsonArchiveSaver` | `prop` 宣言のみで、この repository 内に定義なし | [`jsonarchive.hpp`](../../src/core/userpublic/serialize/jsonarchive.hpp#L32) |
| `BinaryArchive` | `prop` 宣言のみで、この repository 内に定義なし | [`binaryarchive.hpp`](../../src/core/userpublic/serialize/binaryarchive.hpp#L10) |
| pose action | **実装済み**(WP130/132)。pose は `poses` map から返り、未サンプルなら default `ActionPose`。flat 環境では pose サンプルが来ないので default が返る点に注意 | [`actionmap.cpp`](../../src/core/os/actionmap.cpp#L873) |
| `.surface` / material format | **runtime 接続済み**(WP116/117/122)。`.surface` は surfacecompiler で pipeline に、`.material.json` は lowering を経て `MaterialContainer` へ | [`surfacecompiler.hpp`](../../src/core/shader/surfacecompiler.hpp) / [`materiallowering.hpp`](../../src/project/materiallowering.hpp) |
| Studio project editor | D2、WP264 の viewport / Outliner 選択同期、WP266 の schema-driven property Inspector、WP275 の gizmo 操作まで実装済み。project は同じ child へ `--rpc --project` で渡り、identity は名前でなく `(scene_id, declaration_index)`。property edit / gizmo / live preview / undo / redo / save は公開 RPC を使い、terminal result まで成功扱いしない。picking / gizmo feature 無効は理由を警告し、自動有効化しない | [`SelectionModel`](../../src/devstudio/model/selection.hpp) / [`GizmoModel`](../../src/devstudio/model/gizmomodel.hpp) / [`InspectorModel`](../../src/devstudio/model/inspectormodel.hpp) / [`InspectorWidget`](../../src/devstudio/view/inspectorwidget.hpp) / [`EmbeddedViewport`](../../src/devstudio/viewport/embeddedviewport.hpp#L34) |
| swapchain capture | surface が TRANSFER_SRC を持てば windowed でも readback 実装済み。不可時のみ `capture unavailable_windowed` 例外 | [`swapchainframetarget.cpp`](../../src/core/vkcore/swapchainframetarget.cpp#L436) |
| frame graph levels | 計算/JSON 出力のみ。runtime は直列 node loop | [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1585) |
| custom Component public registration | ID macro はあるが安定 public boot hook なし。ただし登録解除 API と重複拒否は入った。解除と engine owner 登録は WP379a で host 専用へ | [`component/registerer.hpp`](../../src/core/userpublic/details/component/registerer.hpp#L78) / [`ecs/enginecomponentregistrar.hpp`](../../src/core/ecs/enginecomponentregistrar.hpp#L13) |
| behavior attachment | ✅実装済み(WP155 / 162 / 167) | [`behaviorarena.hpp`](../../src/core/gamelogic/behaviorarena.hpp#L213) |
| 物理 trigger event | ✅実装済み(WP179) | [`PhysWorld::updateTriggers()`](../../src/core/phys/physworld.cpp#L698) |
| 編集 RPC(query / snapshot / edit / undo / preview / journal) | ✅実装済み(WP153〜172) | [`editorcommandservice.hpp`](../../src/core/communication/editorcommandservice.hpp#L258) |
| ImGui inspector / asset browser | ✅実装済み(WP159 / 164 / 167)。ただし `--rpc` / headless / replay / golden / XR では無効 | [`inspector.hpp`](../../src/core/imgui/inspector.hpp#L167) |
| preview graph(第3 variant) | 🚧実装済みだが CPU 模式ラスタ(WP172)。隔離契約が本体で、見た目の忠実度は保証しない | [`previewgraph.hpp`](../../src/core/renderingpass/previewgraph.hpp#L15) |
| RenderDoc capture | 🚧受動のみ(WP140)。**エンジンは RenderDoc をロードしない** | [`renderdoccapture.hpp`](../../src/core/renderdoc/renderdoccapture.hpp#L66) |
| VRMA decode / retarget / AnimationSource | ✅実装済み(WP176 / 177 / 178) | [`vrmadecoder.hpp`](../../src/core/loader/vrmadecoder.hpp) / [`vrmaretarget.hpp`](../../src/core/animation/vrmaretarget.hpp) |
| `.vrma` の root motion 抽出(容器側) | 📐設計スロットのみ。`VrmaRootMotionPolicy` は `preserve_hips_translation` の 1 値だけ。**ただし root motion 自体は v0 で出荷済み**で、容器ではなく共通グラフ側の `pose.clip_sample{root_motion: "extract"}` が担います(AG-2)。抽出・pin・オブジェクトへの加算は §9.24 の難所「unwrapped time から作る root delta」 | [`VrmaRootMotionPolicy`](../../src/core/animation/vrmaretarget.hpp#L21) / [`poseClipSampleExtractsRootMotion()`](../../src/project/posegraphmodule.hpp#L45) / [`extractRootMotion()`](../../src/core/animation/posegraphruntime.cpp#L322) |
| authoring 側のオブジェクト宣言 identity | ✅実装済み(WP360)。raw viewとstageは [`AuthoringSceneAuthority`](../../src/core/loader/authoringsceneauthority.hpp#L35) に隔離し、runtime読者はidentityを保存した単一の [`ResolvedScene`](../../src/core/loader/resolvedscene.hpp#L267) を読む | [`SceneProjectionState`](../../src/core/loader/resolvedscene.hpp#L325) |

optional build feature には stub 実装もあります。たとえば SeqPlayer/VAT/RPC/audio/physics/renderdoc は build option により実装または disabled behavior が選ばれます。header が同じでも build artifact の能力は [`build_features.hpp`](../../src/core/build_features.hpp#L1) と各 `*_stub.cpp` を確認してください。

## 9.14 症状から読む場所を決める

| 症状 | 最初の確認 | 次の確認 |
|---|---|---|
| 起動中に module constructor 例外 | module initialization log、[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L84) | constructor 内の `GET_MODULE` 依存 chain |
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
| ライトが動かなくなった | engine 側の名前ベース自動アニメーションは撤去済み(§9.8) | `ctx.render().set*Light*()` をユーザーコードから毎フレーム呼ぶ |
| ライトが 1 個だけ描かれない | ログの `Light cap exceeded: ...` | `MAX_*_LIGHTS` |
| ModelInstance の描画が別モデルに化ける | `ModelInstanceId{index, generation, scene_epoch}` の generation | `isModelInstanceAlive()`、scene 再ロードによる epoch 進行 |
| `save_scene` が `RuntimeOnlyData` で失敗 | `load_gltf` で transient モデルを足していないか | `SceneLoader::hasRuntimeOnlyChanges()` |
| `edit` が `stale_revision` | `get_scene_revision` の `EditorWatchToken` | preview lease が `preview_epoch` を進めていないか |
| windowed で RPC 応答が来ない | フレームが進んでいるか | queue busy 応答(`-32000` / `reason:"busy"`)が来ていないか |
| RPC event payload が空 | event に `ref(JsonArchiveLoader&)` があるか | default-only JSON loader branch |
| frame graph の順が違う | [`currentFramePlanJson()`](../../src/core/vkcore/renderer.cpp#L3997) | reads/writes、after/before、declaration index |
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

`renderLogicalFrame()` は次を破ると例外にします([`renderer.cpp` 内](../../src/core/vkcore/renderer.cpp#L1585)): 全 view が同じ in-flight frame index を共有すること、全 view の extent が等しいこと、target の color format がコンパイル済み graph と一致すること。FrameUBO slot は `in_flight × view_count + view` の式で選ばれます(WP128 レポート: [`docs/design_reviews/2026-07-17_wp128_report.md`](../design_reviews/2026-07-17_wp128_report.md))。

### XR mirror は「drop 可能な optional sink」

frame の開始は成否 bool ではなく [`FrameBeginResult`](../../src/core/vkcore/frametarget.hpp#L203) を返します([`IFrameTarget::beginFrame()`](../../src/core/vkcore/frametarget.hpp#L291))。判断の軸は [`FrameBeginDisposition`](../../src/core/vkcore/frametarget.hpp#L143) の 4 値で、frame が取れなかった内訳は [`FrameUnavailableReason`](../../src/core/vkcore/frametarget.hpp#L150) の `reason` に別途入ります。mirror はこれを [`classifyMirrorBeginResult()`](../../src/core/openxr/openxrmirrorsink.cpp#L58) で 3 つの行動へ落とします。

| disposition | 行動 | mirror の挙動 |
|---|---|---|
| `ready` | `present` | 描いて `submit()`。`presented` を数える |
| `unavailable` | `drop` | 何もせず return。`dropped` と `last_drop_reason` だけ進む。**エラーではありません** |
| `device_rebuild_required` / `fatal` | `disable` | WARNING を出して以後は永久に諦める。`failures` を数える |

**mirror 経路にエラー処理を足すときに、`unavailable` を error へ昇格させないでください。** 分類が見るのは `disposition` だけで `reason` は見ないので、`reason` が `surface_lost` であっても `unavailable` なら drop です([テストが 4 分類を明示的に固定しています](../../test/xrfeaturepolicy_test.cpp#L166))。surface の回復は frame target 側の仕事で、mirror から同期的に回復を呼ばないことが設計上の要求です([`docs/design_wsi_epoch_recovery.md`](../design_wsi_epoch_recovery.md) の「9.4 XR mirror」)。

mirror が best-effort でいられるのは、入力が engine 所有の中間 target `__xr_mirror_left` だけで、OpenXR swapchain image を受け取らず、`FrameBeginMode::nonblocking` で開始するため desktop swapchain を待ちもしないからです([`XrMirrorSink`](../../src/core/openxr/openxrmirrorsink.hpp#L39) 直前のコメント)。[`XrMirrorSink::tryPresent()`](../../src/core/openxr/openxrmirrorsink.cpp#L274) は全体が `noexcept` で、例外が出れば frame を [`abandon()`](../../src/core/vkcore/frametarget.hpp#L313) して自分を disable するだけです — HMD 側の logical frame は止めません。集計は [`XrMirrorSinkStats`](../../src/core/openxr/openxrmirrorsink.hpp#L29) に載り、`OpenXR mirror diagnostic: milestone=...` の INFO ログに出ます(WP133 が入れた「mirror は drop してよい」規約を、現在はこの 3 分類が担っています)。

drop は `beginFrame()` が `ready` を返した後にも起こります。letterbox 矩形が空(destination が 0 サイズ = 最小化)なら、取得済みの frame をそのまま `submit()` してから drop として数えます。**`beginFrame()` で得た frame は必ず `submit()` か `abandon()` へ渡してください** — 渡さなければ `FrameTargetFrame` のデストラクタが abandon 扱いで回収するので壊れはしませんが、どの経路で落ちたのかが追えなくなります。

### XR の forced-off は決定的

headless / RPC / golden / replay では XR は決定的に off です([`xrForcedOffDriver()`](../../src/core/xractivation.hpp#L55))。`--xr on` とこれらの組み合わせはエラー方向です([`resolveXrActivation()`](../../src/core/xractivation.hpp#L89))。「headless テストで XR コードが動かない」のはこの activation 規約によるものです。

## 9.17 編集 transaction の落とし穴(prepare / publish / CAS / ticket)

WP144〜WP172 で、変更のプロトコルがコードベース全体で統一されました。この節が第9章で最も重要な追加です。

以下は 3 層に分かれています。**(a) 1 回の編集をどう原子的に適用するか**(1〜2)、**(b) その編集をいつ受理してよいか**(3 の CAS / 4 の lease / 5 の gate)、**(c) どこで確定し、何を拒否・出力するか**(6〜8)です。(a) を束ねているのは [`EditorProjectionTransaction::commit(commands, adapters)`](../../src/core/loader/editorprojectiontransaction.hpp#L303) の 1 関数で、`base_revision` の照合 → command を staged document へ適用 → **全 adapter の `prepare()`** → **全 adapter の `publish()`** → document 公開 → 逆順に `finish()`、という並びです。どこかで例外が出れば、そこまでに `prepare()` した adapter を**逆順に `rollback()`** して `Rejected` / `Failed` を返します([`EditorProjectionTransaction::commit()`](../../src/core/loader/editorprojectiontransaction.cpp#L883))。

### 1. prepare は throw してよいが、publish は絶対に失敗できない

[`ECSArchetypeMigrationAdapter`](../../src/core/ecs/archetypemigration.hpp#L45) のコメントが規範です。

> prepare() may throw but must not change published state. rollback() is invoked for
> every attempted prepare (including the one that threw). Publication must not fail.

同じ形が各所にあります。

| 場所 | API |
|---|---|
| ECS archetype | [`ECSArchetypeMigration::prepareAdd/prepareRemove`](../../src/core/ecs/archetypemigration.hpp#L121) → token の `publish()` / `rollback()` / `finish()`(全て `noexcept`) |
| ECS entity | [`ECSEntityMutation::prepareCreate/prepareDestroy`](../../src/core/ecs/archetypemigration.hpp#L109) |
| ECS 既存値 | `prepareComponentValue()` / `publishComponentValue()` / `rollbackComponentValue()` |
| model instance | `preflightModelInstance()` → `stageModelInstance()` → `publishModelInstance()` |
| behavior | [`PreparedBehaviorAttachmentEdits`](../../src/core/gamelogic/behaviorarena.hpp#L157) の `publish()` / `rollback()` / `finish()`(全て `noexcept`) |
| physics | [`PhysWorld::prepareBindings()`](../../src/core/phys/physworld.hpp#L78) → [`publishPrepared()`](../../src/core/phys/physworld.hpp#L85)(`noexcept`) |
| 編集投影 | [`EditorProjectionPublicationMode{StagedNoexcept, InverseToken}`](../../src/core/loader/editorprojectiontransaction.hpp#L39) |

`EditorProjectionPublicationMode` は「公開をどう戻せる形にしてあるか」の宣言です。`StagedNoexcept` は publish 時点に失敗要因が残らないよう prepare 側へ寄せ切る形、`InverseToken` は publish 後でも `rollback()` が完全な逆操作を復元できる形です。`publish()` / `rollback()` / `finish()` はどちらのモードでも `noexcept` で、`rollback()` は publish 前(prepared 状態を捨てる)・publish 後(逆トークンで戻す)のどちらの経路も allocation-free であることが要求されます([`EditorProjectionAdapter`](../../src/core/loader/editorprojectiontransaction.hpp#L185))。

> **設計決定:** 新しい adapter を足すときは **prepare / rollback / publish の三点セットを必ず作ってください**。「途中まで適用された状態」を許す実装を1つ混ぜるだけで、その adapter だけでなく、**同じ `commit()` が束ねる transaction 全体**(= 編集・reload・scene 遷移が共有するこのプロトコル)の原子性が崩れます。他の adapter が正しく rollback できても、その 1 つが戻らなければ transaction は半端な状態で終わるからです。

`load_gltf` の単一公開点がわかりやすい実例です([`instances.publishModelInstance()`](../../src/core/loader/scene.cpp#L2267))。

```cpp
// No operation below allocates: this is the single publication point for
// the entity's slot, draw commands, resources, and optional name.
instances.publishModelInstance(std::move(staged_instance));
```

名前バインディングも `unordered_map::node_type` を先に `extract()` して確保しておき、公開時に allocation が起きないようにしています。

### 2. 投影 adapter は 8 種で閉じている

[`EditorProjectionAdapterKind`](../../src/core/loader/editorprojectiontransaction.hpp#L25) は `EcsExistingValue` / `EcsArchetype` / `TransformClosure` / `RendererModel` / `Camera` / `Light` / `PhysWorld` / `BehaviorAttachment` の 8 種です。`archetypemigration.hpp` のコメントが境界を書いています。

> This is only a composition boundary. Light, collider, behavior, and other special
> attachments remain separate adapters owned by their respective projection WPs.

つまり ECS archetype 移行は「ECS の中だけ」を見ており、light / collider / behavior は別 adapter が責任を持ちます。

### 3. CAS は `SceneRevision` で行う。ただし revision だけでは足りない

`edit` は `base_revision` を伴い(これが節題の CAS — compare-and-swap、「読んだときの値から変わっていなければ書き換える」条件付き更新のことです)、ズレていれば `EditorEditErrorCode::stale_revision` です。**watch トークンは [`EditorWatchToken{scene_revision, preview_epoch}`](../../src/core/communication/editorcommandservice.hpp#L196) の 2 要素** で、preview の open/commit も epoch を進めます。`get_scene_revision` の戻り値を丸ごと持ち回ってください。

### 4. preview は lease(ticket)

`open_preview` → `update_preview` → `commit_preview` / `abort_preview` の流れです(lease は「開いている間その対象の編集を 1 人が占有する権利」、ticket はその lease 1 件を指す識別子です)。関連するエラーコードは [`EditorEditErrorCode`](../../src/core/communication/editorjournal.hpp#L50) の `preview_lease_conflict` / `preview_lease_busy` / `not_lease_owner` / `ticket_not_found` / `undo_conflict` です。composition root 用の緊急口として `EditorCommandService::forceAbortPreview(reason)` があります。

### 5. 編集ゲートは 5 ビット。「受理時に開いていた」は実行してよい理由にならない

[`EditorGateReason`](../../src/core/communication/editorjournal.hpp#L21) は `replay` / `golden` / `strict` / `reload_scene_transition` / `preview_lease_conflict` のビットフラグです。[`EditorGateObservation`](../../src/core/communication/editorjournal.hpp#L33) の `transition_epoch` のコメントが規範です。

> A counter the composition root advances when it observes a reload or
> scene transition (internal::EditorTransitionCounter), so an edit that
> straddles one is rejected even if the gate is open again.

behavior コールバック実行中 / DLL リロード中の追加ゲートは [`internal::applyBehaviorEditConcurrencyGate()`](../../src/core/communication/editorruntimefactory.hpp#L115) です。

その「遷移」は 2 段で数えます。

- reload service の [`transitionFacts()`](../../src/core/watch/reloadservice.hpp#L227) が、watcher の state と epoch、各 runtime participant の reload 結果(requested / attempted / applied / failed / error)を型付きの [`ReloadTransitionFacts`](../../src/core/watch/reloadtransition.hpp#L17) で返します。participant の `details` は**型として入る場所がありません**。
- editor runtime が 1 つだけ持つ [`internal::EditorTransitionCounter`](../../src/core/communication/editorruntimefactory.hpp#L127) が、現在の scene id とその facts を前回と比べ、違えば数を 1 進めます。これが `transition_epoch` です。hash ではないので衝突せず、元の値に戻る遷移(A→B→A)も遷移として数えます。

受理した編集を commit してよいかの判定は、journal・render config・procedural asset の 3 つの editor が同じ [`EditorGateEpoch`](../../src/core/communication/editorgate.hpp#L18) で行います。gate の観測(開閉・理由・`transition_epoch`)が前回と違えば epoch を進め、受理時の epoch と commit 時の epoch が違えば `gate_closed` です。閉じてから開き直した gate も変化として数えます。

`details` は participant の自己記述で、たとえば window を resize すると render pipeline の再 lowering が `details.generation` を進めます。以前は reload 状態の JSON 全体を hash していたため、受付と frame 境界の間に viewport の大きさが変わるだけで編集が `gate_closed` で捨てられ、v0 の GPU gate で `studio_scenario_graph_observe_roundtrip` が負荷次第で落ちていました(`ba82495f` で応急修正、上の形への一本化で構造ごと解消)。**participant に状態を足すときは、遷移なら `ReloadTransitionFacts` の側、観測値なら `details` の側に置いてください。**

### 6. 編集コミットはフレーム境界に 1 点だけ

[`invokeEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L146) が reload 公開の後・`freeze_events` の直前に走ります。フックは **単一所有**で、2 回目の [`installEditorCommitQueueHook()`](../../src/core/appflow/framephase.hpp#L40) は `false` を返します。未設置なら zero-state no-op で、モジュールを作りません。

### 7. 保存拒否の理由コード

[`EditorCommandErrorCode`](../../src/core/communication/editorcommandservice.hpp#L29) は 13 種です。特に注意すべきものを挙げます。

- **`RuntimeOnlyData`**: [`SceneLoader::hasRuntimeOnlyChanges()`](../../src/core/loader/scene.hpp#L244) が真のとき、つまり `load_gltf` で持ち込んだ transient モデルがあるときに出ます。「RPC で読み込んだモデルは保存できない」という意味です。
- `ExternalModification`: ディスク上の scene が外部で書き換わっていた。
- 上限は [`maxSceneSnapshotBytes = 64 MiB`](../../src/core/communication/editorcommandservice.hpp#L27)、JSON 整数の安全上限は [`maxExactEditorJsonInteger = 9007199254740991`](../../src/core/communication/editorcommandservice.hpp#L26)。

### 8. snapshot は「ファイル内容」ではなく semantic bytes

`export_scene_snapshot` の `semantic_scene_bytes` は **キー順が正規化された文字列** です。実物([`test/fixtures/editor_command_service/export_scene_snapshot_v1.json`](../../test/fixtures/editor_command_service/export_scene_snapshot_v1.json) の `expected_response`)を見ると `scenes` → `schema` → `version` の順になっています。

```json
{
  "schema_version": 2,
  "scene_revision": 42,
  "current_scene_id": "main",
  "semantic_scene_bytes": "{\"scenes\":{\"main\":{\"objects\":[]}},\"schema\":\"pelican.scene\",\"version\":2}",
  "digest": {"algorithm": "sha256", "hex": "e74c47dd1a3d08a001a0e9d5eeeaf1d2f0ed78d5c55ad94c1c55e5ee4904d06d"},
  "pending_ticket_ids": [],
  "preview_epoch": 7
}
```

(ファイル名は `_v1` のままですが、MG-2b から中身は SnapshotV2 の要求と scene v2 の文書です。実際の export の応答にはこのほか `prefab_closure` が付きます。)

外部ツールで digest を再計算するときは `AuthoringSceneDocument::encodeSemantic()` と同じ正規化が必要です。**ファイルを読んで sha256 を取っても一致しません。**

## 9.18 windowed RPC ホストの落とし穴

- `--rpc` は `--headless` 無しでも通るようになりましたが、**ImGui UI は無効になります**([`isImGuiRuntimeEnabled()`](../../src/core/imgui/imguiruntime.cpp#L9) が `!config.rpc` を要求)。windowed RPC と ImGui inspector は排他です。
- リクエストは **フレーム境界でしか処理されません**。windowed は毎フレーム進むので通常は問題になりませんが、ウィンドウ最小化などでフレームが止まると応答も止まります。
- queue 容量 [`64`](../../src/core/communication/rpcserver.hpp#L179) を超えると即座に `-32000` / `data.reason == "busy"` が返ります。この応答は **reader スレッドから** 出るため、engine スレッドの応答と行が混ざり得ます。順序保証は JSON-RPC の `id` に依存してください。
- reader スレッドは `std::istream` にキャンセル手段がないため、ブロックしたまま `detach()` されることがあります。テストで `WindowedRpcHost` を使い捨てるときは入力ストリームを閉じて `readerFinished()` を待ってください。本番は process 寿命の `std::cin` 前提です。

## 9.19 preview / `render_preview` の隔離

- `render_preview` は **`Renderer::renderLogicalFrame()` を通りません**([`PreviewGraphProgram`](../../src/core/renderingpass/previewgraph.hpp#L20) 直前のコメント)。したがって temporal history、FrameResources slot、layout tracker などのライブ状態を汚しません。
- 汚していないことの証明が [`previewStateInventory()`](../../src/core/vkcore/previewexecutor.hpp#L59)(「並び順も診断契約の一部」)と [`Renderer::previewIsolationStateJson()`](../../src/core/vkcore/renderer.cpp#L3857) です。
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
| [`rpc_color_contract_test.cpp` 内](../../test/rpc_color_contract_test.cpp#L896) | [`MaterialContainer::validateRuntimeGenerationCompatibility()`](../../src/core/material/materialgenerationplanner.cpp#L1682) の `render-pipeline candidate has no compatible pass for live material 0 (route 'deferred_geometry', shader contract 'gbuffer_v1')` |
| [`materialvaluesreload_test.cpp` 内](../../test/materialvaluesreload_test.cpp#L369) | [`validateMaterialTextureReflection()`](../../src/core/material/materialcontainer.cpp#L485) の `material texture 'albedo_detail' is absent from shader reflection at binding 7` |
| [`materialvaluesreload_test.cpp` 内](../../test/materialvaluesreload_test.cpp#L491) | 同上 |
| [`HR1-M watcher gate and 1000 reloads keep resources bounded`](../../test/materialvaluesreload_test.cpp#L766) | 同上 |

`try` の位置は 2 通りありました。`materialvaluesreload_test.cpp` の 3 件は `FastModuleContainer modules;` から `waitIdle()` まで**本体まるごと**([`"HR1-M updates one same-layout material and rolls back invalid candidates"`](../../test/materialvaluesreload_test.cpp#L369) の `try` など)、`rpc_color_contract_test.cpp` は engine 起動部だけ([`runEngineRpcServer()`](../../test/rpc_color_contract_test.cpp#L405) を囲む `try`)ですが、fail-fast は起動時に出るので結果は同じです。

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
> **手がかり**: 「capability が無い」と「実行して失敗した」を分ける方法は2つです。(1) **明示的な能力問い合わせ** — [XR segmented draw の multiview capability 検査](../../test/golden_harness.cpp#L12781) / [`.dynamic_rendering_local_read`](../../test/headless_render_test.cpp#L4964) を見て skip し、本体は囲まない。(2) **device 初期化エラーを厳密に絞って再送出** — [`requireVulkanDevice()`](../../test/vulkan_test_support.hpp) は `No suitable Vulkan physical device found` のときだけ skip し、それ以外は `throw;` します。後始末の catch も同じ判定を使い、非該当なら再送出します。**(1) が本来の形**で、(2) は既存テストを最小限の変更で救う形です。
>
> **不変条件**: skip は「実行できない理由」を**問い合わせて**決める。`std::exception` を捕まえて skip にしない。どうしても囲むなら、囲む範囲を bring-up だけに限り、bring-up を抜けたら再送出する。

### この idiom がどこにあるか

`test/*.cpp` で `catch (const std::exception ...)` のハンドラ本体に `SKIP(` を持つブロックは調査時点で 42 か所あり、そのうち再送出・メッセージ絞り込み・`runtime_ready` ラッチのいずれかで判断を狭めているのは 7 か所だけでした。WP241 後は broad handler を除去し、直接 `SKIP` を含む残りは device 不在を絞って再送出する3か所だけです。探すときはこの形で当たれます。

```powershell
rg -n -A3 "catch \(const std::exception" test --glob "*.cpp" | rg -B1 "SKIP\("
```

### gate 側の受け止め

skip が緑を汚さない以上、`ctest` の exit code だけでは検出できません。そのため gate 側が **許可した名前以外の skip をすべて失敗にする**方針を持っています。判定は [`validate_skip_policy()`](../../test/ci/skip_policy.py#L74)(CPU/GPU 両 gate 共通)で、GPU 側の driver が [`run_gpu_gate.py`](../../test/ci/run_gpu_gate.py)、許可リストが [`test/ci/gpu_skip_allowlist.txt`](../../test/ci/gpu_skip_allowlist.txt) です。現在の GPU allowlist は、clone / worktree 外の corpus を `PELICAN_TEST_PROJECTS_DIR` で指定する `project_catalog_headless_smoke` の未指定 skip と、Debug 専用の故障注入を持つ `wsi_fault_window_player` の非 Debug skip を載せています。上の 4 件の broad exception skip は載せません。entry を消せば未指定時の skip が非許可になり、逆にテスト名自体が現れなければ stale entry として失敗するので、リストは腐りません([`skip_policy.py` 内](../../test/ci/skip_policy.py#L98))。

なお `gpu` ラベルは Catch2 の `[gpu]` タグではなく CTest の LABELS で、付き方が 2 通りある点に注意してください。Catch2 テストは [`pelican_define_test(<name> GPU ...)`](../../test/CMakeLists.txt#L81) が target 単位で付け、CTest 名は [`catch_discover_tests()`](../../test/CMakeLists.txt#L207) が `TEST_CASE` の文字列をそのまま使います。もう一方は `add_test()` で登録した e2e / player テストに [`pelican_gpu_test_properties(seqplayer_headless_player ...)`](../../test/CMakeLists.txt#L36) の形で個別に付けるもので(呼び出しは 26 か所。うち 1 つは `foreach` の中なので、登録されるテスト数はこれより多くなります)、この場合の CTest 名は `add_test()` の名前です。この関数は label だけでなく `RESOURCE_LOCK pelican_gpu` と生成した `TIMEOUT` も同時に付けます([定義](../../test/CMakeLists.txt#L36))。allowlist は完全一致の名前を要求するので、どちらの経路で付いたラベルかで書くべき名前が変わります。gate の起動方法は [`docs/ci.md`](../ci.md) にあります。


## 9.21 prepare / publish 世代のコードで踏む新しい罠

§9.17 までの落とし穴は「型と寿命」でしたが、scene load transaction・schema wire・登録 ABI の周りには**同じ形をした別の罠**が増えています。どれも「素直に書くと通ってしまう」ものです。

### `OptionalOverride` の absent と「0 を set した」は別

`std::optional<T>` の schema field には [`StructFieldPresence::OptionalOverride`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L67) が付きます。decode は、キーが**無ければ何も代入せず**(`std::nullopt` のまま)、あれば値を decode して `member = decoded` します([decode の presence 分岐](../../src/core/userpublic/details/schema/structfieldjson.hpp#L575))。encode は逆で、`if (!member) return;` により**キーごと出しません**([encode の skip](../../src/core/userpublic/details/schema/structfieldjson.hpp#L718))。

つまり次の3つはすべて別の状態です。

```text
{}                          → nullopt          「宣言していない」
{"custom_index": 0}         → optional{0}      「0 を宣言した」
{"instance_flags": []}      → optional{空集合}  「フラグ無しを宣言した」
```

`optional<T>` を `value_or(0)` や `.value_or(T{})` で読み出した瞬間、この区別は消えます。`pelican_ray_instance` が field ごとに optional なのはまさにこのためで([第6章 §6.20](06_rendering_vulkan_shader.md))、ヘッダのコメントが「authored の空の `instance_flags` 配列は本物の override」と明記しています。同じ理由で AS row の [`source_material_index`](../../src/core/shader/raygrouplayout.hpp#L256) も optional のまま運ばれ、コメントが「**欠損した index が黙って 0 になってはいけない**」と規範を書いています。0 に丸めると、たまたま 0 番 material を指す asset selector が誤って一致します。

### `$list.next_id` は「行の最大値より大きい」しか強制されない

`List<Row>` の wire は object container で、`$list` 封筒に `v` と `next_id`、各行に `$row` 封筒(`id` / `v` / 任意の `hint`)が付きます。decode が拒否するのは、未知キー・`$row` 欠落・`id == 0`・重複 id・未知 version、そして最後の [`$list.next_id must exceed every row id`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L393) です。

ここが読み違えやすい点です。`next_id` は **id を発行するための水位**であって、「削除したら戻る」値ではありません。行を消しても `next_id` を下げずに配れば id は再利用されず、`hint` や外部参照が別の行へ付け替わることもありません。ただし **codec がその単調性を検査しているわけではありません** — 最大 id の行を消したあとに `next_id` を下げた文書は、decode を通ります。encode 側も [`{{"v", 1}, {"next_id", member.next_id}}`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L726) を verbatim に書くだけです。単調性は**生成側の契約**であって、wire の検査で守られてはいません。

### phase trace は participant が記録する。executor に `record` 文を足さない

[`SceneLoadPhaseTrace`](../../src/core/loader/sceneload.hpp#L40) の各 phase を記録するのは、その phase を実際に実行する participant です。`publishPrepared(candidate, phase_trace)` のように **trace を引数で受け取る側**が [`record()`](../../src/core/loader/sceneload.hpp#L47) を呼びます。executor 本体に `phase_trace.record(...)` を書き足すと、participant を丸ごと bypass する変異が trace 上は正常に見えてしまい、順序テストが変異を検出しなくなります。

現行の executor に残っている裸の `record` は2行だけで、どちらもコメント付きの例外です。

| 行 | 理由 |
|---|---|
| [`phase_trace.record(SceneLoadPhase::flush)`](../../src/core/loader/scene.cpp#L1800) | 直後の `pending_scene_id.swap()` は participant 型を持たない |
| [`phase_trace.record(SceneLoadPhase::finish)`](../../src/core/loader/scene.cpp#L1807) | **退役 batch 自体が 1 人の participant**。entity 0 件の scene でも `finish` を観測できるようにする |

逆向きの例も1つあります。[`PreparedEffectTransaction::flush()`](../../src/core/loader/effecttransaction.cpp#L494) は `phase_trace` を受け取りながら `(void)phase_trace;` で**使いません**。コメントが理由を書いています — `flush` の印は `EventOutbox` が1つだけ持つ、という取り決めです。「引数にあるのだから記録すべきだ」と足すと、`record()` の重複抑止に助けられて症状は出ないまま、印の所有者が2人になります。

### `ReleaseReport` は heap を使わない — 8 件 + `overflow_count`

解放の失敗は `std::vector<std::string>` ではなく [`ReleaseReport`](../../src/core/model/modelresourcerelease.hpp#L36) に積みます。`std::array<ReleaseError, 8>` + `error_count` + `overflow_count` の固定 record で、[`add()`](../../src/core/model/modelresourcerelease.hpp#L43) と [`append()`](../../src/core/model/modelresourcerelease.hpp#L51) は `static_assert` で `noexcept` が固定されています。9 件目以降は捨てるのではなく `overflow_count` を増やします。

「8 件を超えたら vector に切り替える」という善意の改修が、**`noexcept` の destructor / 遅延解放経路から `bad_alloc` を投げる**形になります。報告先を返せない経路(destructor、deferred release)は戻り値ではなく process 全体の [`ReleaseErrorSink::record()`](../../src/core/model/modelresourcerelease.hpp#L74) へ積む、という分担もヘッダのコメントが規範です。件数は `ReleaseErrorSinkStatus` で観測できるので、**溢れたこと自体はテストで見えます**。

### effect sink の有効範囲は「early publish の前から flush まで」

scene load 中に game DLL のコールバック(component の JSON loader を含む)が `playSound()` や `saveData()` を呼ぶと、それは即時実行ではなく [`PreparedEffectTransaction`](../../src/core/loader/effecttransaction.hpp#L27) に溜まります。scope を張る位置が要点で、[`EffectTransactionScope`](../../src/core/loader/scene.cpp#L1759) は **early publish より前**、prepare が終わった直後に張られます。コメントが規範です — 「early publication が呼ぶ component JSON loader を含め、**すべての game-DLL コールバックが同じ transaction-local な effect を観測**しなければならない」。

したがって「publish の直前で張れば十分」という縮め方は誤りです。prepare 中の loader が本物の audio / persistence を直接叩き、rollback しても戻らない副作用が残ります。逆側の端は [`effect_transaction->flush(...)`](../../src/core/loader/scene.cpp#L1644) で、commit 後の noexcept tail の中にあります。

### `noexcept` の commit tail の中で文字列や JSON を作らない

metadata の commit 以降は「失敗しない尾部」です。executor はその区間の全操作が `noexcept` であることを [`static_assert` の列](../../src/core/loader/scene.cpp#L633)で固定しています。ここで `std::string` を組んだり `nlohmann::json` を作ったりすると、`bad_alloc` が `noexcept` 関数から伝播して `std::terminate` になります。

実装はその制約を明示しています。effect のエラーは固定長 record([`FixedText<512>` / `FixedText<384>`](../../src/core/loader/effecttransaction.hpp#L119))に詰めておき、**公開用の文字列と JSON は tail を抜けてから**作ります([`effectErrors()` の呼び出し](../../src/core/loader/scene.cpp#L1815) の直前のコメントが「deliberately materialized only after the noexcept commit tail」)。同じ理由で `SceneLoadPhaseTrace` も固定長です。

### `createObject()` は基底の `GameContext` から呼んでも defer される

`BehaviorContext::createObject()` が [`deferCreateObject()`](../../src/core/gamelogic/behaviorarena.cpp#L1572) に落ちるのは自然に見えます。見落としやすいのは**基底側**で、[`GameContext::createObject()`](../../src/core/userpublic/gamecontext.cpp#L105) も先頭で `internal::behaviorCallbackActive()` を見て、真なら同じ defer をして `invalidGameObjectId` を返します。

つまり behavior のコールバック中は、`BehaviorContext` を捨てて `GameContext&` として呼んでも即時生成にはなりません。**戻り値が invalid であること自体が仕様**で、これを「基底なら即時に作れる」と読んで戻り値を使うと、`resolve()` に弾かれる ID を掴みます。生成された entity を触りたいなら次フレーム以降に名前や別の手段で引き直します。

### persistence の key は case-fold した canonical path — ただし Windows だけ

同じファイルを別の綴りで指した保存要求を1つに畳むため、`Persistence` の resource key は [`canonicalResourceKey()`](../../src/core/persistence/persistence.cpp#L40) が作ります。`weakly_canonical()` で正規化し、`generic_string()`(区切りは `/`)にしたうえで、**`_WIN32` のときだけ** `std::tolower` を全体に掛け、`"path:"` を前置します。

罠は2つあります。(1) case-fold は `#ifdef _WIN32` の内側なので、**key の同一性ドメインがプラットフォームで違います**。「大文字小文字違いの2つの save slot」は Windows では衝突し、Linux では別物です。(2) `weakly_canonical()` は失敗しうるので、この関数は `std::runtime_error("failed to canonicalize persistence path: ...")` を投げます — key を作る処理が例外を出す点は、`noexcept` の中で呼ばないための前提です。

### `sizeof` だけの ABI 凍結は、同サイズ field の並べ替えを通す

game DLL 境界の v2 record は「凍結」と書かれていますが、実際に固定しているのは**大きさだけ**です。

| record | 凍結 |
|---|---|
| [`ComponentLoaderInfo`](../../src/core/userpublic/details/component/registerer.hpp#L81) | `sizeof(FrozenComponentLoaderInfoV2Layout)` と一致 |
| [`BehaviorRegistration`](../../src/core/userpublic/details/behavior/registerer.hpp#L50) | `sizeof(FrozenBehaviorRegistrationV2Layout)` と一致 |
| [`EventTypeRegistration`](../../src/core/userpublic/details/event/registerer.hpp#L54) | `sizeof(FrozenEventTypeRegistrationV2Layout)` と一致 |

`BehaviorRegistration` の `create` と `destroy` のように**同じ大きさの隣接 field を入れ替えても `static_assert` は通ります**。古い DLL は入れ替え前の順で書き込み、host は入れ替え後の順で読むので、症状は「関数ポインタが 1 つずれて呼ばれる」という追いにくい形になります。台帳の裁定は `sizeof` + `alignof` + **全 field の `offsetof`** へ移すこと(WP382a-fix。起票済み・未着地)で、既に同じ形を採っている面もあります(例: [`offsetof(ProviderV2, shape_cast_all) == offsetof(ProviderV2, overlap_all) + sizeof(...)`](../../src/core/userpublic/physics/abi_v2.hpp#L170))。ここを触るときは「大きさが変わらないから安全」と読まないでください。

### raw import 診断は provider-only な旧 DLL の唯一の enforcement

game DLL のロード前診断([`diagnoseGameLogicImports()`](../../src/core/gamelogic/gamelogicimports.cpp#L559))は、OS loader の重複だと読まれがちですが、そうではありません。SDK 入口か epoch anchor を import する DLL なら未解決 import になるので loader 自身が拒否します。一方 **`getApiV1` 系を export するだけで SDK 入口を import しない旧 DLL** は未解決 import を持たないため loader は素通しで、この診断([`game_logic_abi_stale@1`](../../src/core/gamelogic/gamelogicimports.hpp#L40))だけが止めます。

だから診断は無効化できず、`LoadLibraryW` の**前**に走ります(constructor を動かさないため)。reader は PE の import directory と delay-import directory だけを読み、[`GameLogicPeImportKind`](../../src/core/gamelogic/gamelogicimports.hpp#L12) の 4 種(`named` / `ordinal` / `delay_named` / `delay_ordinal`)へ分類します。host module への delay-load / ordinal import は unsupported として拒否します。

Linux 側にこの reader はありません。`RTLD_NOW` が解決に失敗しない provider-only な旧 `.so` を止める仕組みは、**今のところありません**([第1章](01_architecture_and_build.md))。

## 9.22 横断パターン(初読で 5 つ以上の領域に同じ形で現れるもの)

この節と次の §9.23 は、章の本文ではなく**逆引きの索引**です。第9章のここまでは「マクロ展開、型消去、静的初期化、寿命、現在の未実装点」を順に説明してきましたが、実際の読解で詰まるのは順番に読んでいるときではなく、**知らない場所を先に開いてしまったとき**です。そのときに「この形は何か」「どこに答えが書いてあるか」を引くための表がこの 2 節です。

説明の**本体は各章の `🧩 難所` ブロック**にあります。ここは答えを二重に書かず、既に難所ブロックがある機構は 1 文とリンクだけで送り出します。索引にしか答えが無いものだけ、その場で 2〜4 文書いています。

以下の 6 つは、初読で 5 つ以上の領域に**同じ形**で現れます。1 つ知ると複数の領域が同時に読めるので、領域別索引(§9.23)より先に読んでください。

### 三段トランザクション(prepare → publish → finish)

**形**: 失敗しうる作業(確保・検証・容量チェック・id 採番)を prepare に全部集め、publish を `noexcept` の swap 列にして「公開後は失敗しない」を型で固定し、finish は「もう巻き戻さない」と決まってから初めて破棄・`deinit` を行う。

**どこに出るか**: loader は [`SceneLoader::load()`](../../src/core/loader/scene.cpp#L1650) が prepare → early_publish → publish → activate → commit_metadata の 5 段。ECS は [`ECSArchetypeMigrationAdapter`](../../src/core/ecs/archetypemigration.hpp#L45) のヘッダコメントが「Publication must not fail」を宣言し、**公開済みの実体**に `cb_deinit` を呼ぶのは [`finishPublished()`](../../src/core/ecs/archetypemigration.cpp#L352) とその逆手順の [`rollbackPublished()`](../../src/core/ecs/archetypemigration.cpp#L229) だけ(未公開の候補側は `discardPrepared()` / `deinitializeStaged()` / `rollbackCreate()` が担当する)。描画は [`stagePreparedSceneModel()`](../../src/core/renderer/polygoninstancecontainer.cpp#L623) が容量検査と確保、物理は [`PhysWorld::prepareBindings()`](../../src/core/phys/physworld.cpp#L286)、behavior は [`PreparedBehaviorAttachmentEdits::publish()`](../../src/core/gamelogic/behaviorarena.cpp#L472)、アセットは [`model.cpp` の validate/stage](../../src/core/asset/model.cpp#L657)。

**読むときの手順**: まず `publish` の宣言に `noexcept` が付いているかを見て、付いていれば「この関数より前に全ての失敗が済んでいる」と読み替えます。次に、その関数が触るメンバが *candidate* 側か *live* 側かを 1 つずつ確かめてください。

### `noexcept` なコミットテール

**形**: 「戻せないのに失敗しうる」状態を作らないため、コミット点以降に呼ぶ全関数が throw しないことを **20 個の `static_assert(noexcept(...))`** でコンパイル時に固定する。「確保もしない」の側は static_assert では固定できないので、報告構造を固定長にすることで担保する。

**どこに出るか**: [`scene.cpp` の static_assert 群](../../src/core/loader/scene.cpp#L1082) が publish/finish/retire/flush を固定し、不可逆点は [`metadata.commit()`](../../src/core/loader/scene.cpp#L635) の単一 metadata swap 1 点。公開用の文字列と JSON は [tail を抜けてから materialize](../../src/core/loader/scene.cpp#L1813) します。ECS 側の同じ宣言は [archetypemigration.hpp のヘッダコメント](../../src/core/ecs/archetypemigration.hpp#L44)、`noexcept` の中で確保しないための固定長報告は [`ReleaseReport`](../../src/core/model/modelresourcerelease.hpp#L36)、同じ理由の固定長診断が [`PreparedEffectTransaction::errors_`](../../src/core/loader/effecttransaction.hpp#L142)。

**読むときの手順**: `PreCommitFailure` が取りうる phase 値の集合を先に見てください。prepare / early_publish / activate の 3 つしかないこと自体が、「それ以降は失敗しない」の裏返しです(→ §9.21 の「`noexcept` の commit tail の中で文字列や JSON を作らない」)。

### rollback は公開の逆順(LIFO)、cleanup と discard は別物

**形**: 後に触ったものから先に戻す。既に live へ公開したものを逆トークン/逆 swap で旧世代へ戻すのが *cleanup*、一度も公開していない候補をスコープ終了で捨てるだけなのが *discard* で、後者は「何もしない」のが正しい rollback。

**どこに出るか**: [`scene.cpp` の rollback ラムダ](../../src/core/loader/scene.cpp#L1734) は [`rollback_order` の表](../../src/core/loader/scene.cpp#L1724) をそのまま回すだけで、順は behavior → physics → light → camera → VatPlayer → SeqPlayer → loader state → renderer → procedural → ECS 退役(destroyed の inverse token を `rbegin` から) → ECS(created を `rbegin` から) → event outbox → RNG → pending scene。ECS は [`rollbackPublished()`](../../src/core/ecs/archetypemigration.cpp#L229)、behavior arena は [`activate()`](../../src/core/gamelogic/behaviorarena.cpp#L620) の失敗時に `activated` を逆順に破棄、シェーダは [`discard_new_layouts` ラムダ](../../src/core/shader/pipelinefactory.cpp#L1915) が「今回追加した分だけ」を消し、journal は [子孫を declaration_index 降順に並べてから削除](../../src/core/communication/editorjournalops.cpp#L1134)。

**読むときの手順**: rollback の本体を読む前に「この参加者は公開済みか」を判定してください。event outbox / rng / pending scene のように**何もしないのが正しい**参加者を「巻き戻し漏れ」と誤読するのが最も多い間違いです。

### generation による stale ハンドル検出

**形**: index(= スロット)は再利用するが generation は再利用しない。index と generation を**対で持てば**、古いハンドルが新しい実体を指す ABA を必ず検出できます。持ち方は 1 語の 64bit に詰める形([`LogicalAssetId`](../../src/core/watch/reloadtransaction.hpp#L21))と、フィールドを分けて scene epoch まで足す形([`ModelInstanceId`](../../src/core/renderer/modelinstance.hpp#L11))の両方があります。index を伴わない単調カウンタ(`ray_generation` / `geometry_address_generation`)は形が違い、ハンドルの ABA ではなく「手元のスナップショットが古いか」の判定に使います。

**どこに出るか**: 論理アセットは [`makeLogicalAssetId()`](../../src/core/watch/reloadtransaction.cpp#L19) が `(generation << 32) | index`、entity は [`prepareCreate()` の `reuses_id`](../../src/core/ecs/archetypemigration.cpp#L1029)、描画インスタンスは [`stagePreparedSceneModel()` の `nextId`](../../src/core/renderer/polygoninstancecontainer.cpp#L623)、マテリアルは [`ray_generation` の採番](../../src/core/material/materialcontainer.cpp#L804)、ジオメトリのデバイスアドレスは [`geometry_address_generation`](../../src/core/model/vertbufcontainer.cpp#L321)。

**読むときの手順**: [第4章「generation 0 を跨がない台帳」](04_ecs_deep_dive.md)を先に読み、そのうえで「この世代は何が変わったら進むのか」を軸ごとに分けてください。scene epoch・slot generation・content revision・compatibility revision は**互いに独立**で、1 本だと思って読むと必ず外します(→ §9.2)。

### identity / canonical key / lease の三層

**形**: (a) 内容の同一性を決める canonical key、(b) それを固定長にした digest、(c) 実行時の生存を証明する handle / lease。片方だけでは「内容は同じだが実体が死んでいる」と「実体は生きているが内容が変わった」を区別できない。

**どこに出るか**: RT の行は [3 つの identity を同時に組み立て](../../src/core/renderer/accelerationstructure.cpp#L674)、マテリアル表は [`source_by_handle` と `source_by_key` の 2 名前空間](../../src/core/shader/raygrouplayout.cpp#L1835)を両方照合し、マテリアル側は [`ray_generation` と `ray_lease` の生成](../../src/core/material/materialcontainer.cpp#L804) の対で答えます。resolver は [`resolverInputsFingerprint()`](../../src/core/loader/resolvedscene.cpp#L756)、editor journal は [`attachment_handle` / `attachment_seq` / `attachment_identity`](../../src/core/communication/editorjournalops.cpp#L351) の 3 本立て。

**読むときの手順**: 「どちらが欠けたときにどのエラーコードになるか」を先に引いてください。三層のどれを検査しているかは、エラー名(`*_stale` / `*_ambiguous` / `*_mismatch`)がほぼそのまま表しています。

### fault injection seam と mutation id

**形**: 本番コードの分岐上に注入点を置き、「この分岐を壊すとどのテストが赤くなるか」を名指しできるようにする。manifest の `mutation_id` 列がその索引。

**どこに出るか**: scene load の入口は [`sceneLoadMutationEnabled()`](../../src/core/loader/sceneloadfault.cpp#L30) と [manifest の `root.import.versioned` 行](../../src/core/loader/sceneparticipantmanifest.cpp#L199)、保存の TOCTOU 窓は [`BeforeReplace` の注入位置](../../src/core/loader/basicconfig.cpp#L1028)、SBT は [production branch observer](../../src/core/shader/pipelinefactory.cpp#L1710)、TLAS は [`overrideRayTracingTlasRecordOffsetForTesting`](../../src/core/renderer/accelerationstructure.cpp#L818)、present は [本物の `presentKHR` を呼んでから戻り値だけ差し替える](../../src/core/vkcore/swapchainframetarget.cpp#L2606)。

**読むときの手順**: [第3章「注入口はproductionバイナリに存在しない」](03_project_and_loading.md)を先に読んでください。注入点は「テスト用の別経路」ではなく**本番経路そのもの**で、経路を差し替える方向に直すと A/B が破綻します。

## 9.23 仕様の難所索引(領域別)

「読んでいて詰まった場所から引く」ための逆引きです。領域ごとに、**★ 印(初読で最も助けが要ると報告された箇所)は 1 件ずつ**、それ以外は表 1 つにまとめてあります。表の「答え / 参照」が既存の難所ブロックを指しているものは、**そちらが本体**です。答えを二重に書かないため、ここには 1 句しか置いていません。

答えが `—` になっているものは、この索引の調査時点で**コードにも設計文書にも根拠が見つからなかったもの**です。推測は書いていません。

### 9.23.1 loader / scene transaction

#### [`SceneLoader::load()`](../../src/core/loader/scene.cpp#L1650) — 5 段の責務と、どこから不可逆か

**分からなくなること**: prepare / early_publish / publish / activate / commit_metadata のどこを越えると引き返せなくなるのか。

**答え**: prepare は live に触れず candidate だけを作ります(authority 消費・metadata token・resolved 検証・各サブシステムの候補・retirement queue の容量予約)。early_publish は ECS entity 生成 / component 適用 / 親子 transform 投影 / 旧 entity の unpublish で、逆トークンで戻せます。publish は `noexcept` swap の列、activate は `behavior_candidate->activate()` の 1 行で「全 participant が候補に切り替わった後に失敗しうる」唯一の段です。**不可逆点は [`metadata.commit()`](../../src/core/loader/scene.cpp#L635) の単一 metadata swap** で、コメントが "The commit point is exactly one metadata swap" と明記しています。

**台帳**: manifest 側の語彙は WP375(inverse token)/ WP380(mutation id の同居)。

#### [manifest 配列](../../src/core/loader/sceneparticipantmanifest.cpp#L81) — この表は何の権威か

**分からなくなること**: 巨大な定数配列が何を数えた表で、行数がいくつなのか。

**答え**: 「scene transaction に関与する**独立に観測できるフィールド 1 個 = 1 行**」の表で、snapshot 比較・変異テスト・契約テストが参照する唯一の分類権威です。列は [`SceneParticipantManifestEntry`](../../src/core/loader/sceneparticipantmanifest.hpp#L36) の **10 個**(owner / field / presence / role / prepare_owner / publication / rollback / retirement / snapshot_schema / mutation_id)で、「16 フィールド」は誤りです。行数も **100 行**(`manifest`)で、166 は配列末尾の行番号にすぎません。[`public_api_manifest`](../../src/core/loader/sceneparticipantmanifest.cpp#L206) は別表で 32 行です。

#### [`resolveParentOrder()`](../../src/core/loader/componentcodec.hpp#L115) — 巡回検査の fresh→active→done

**分からなくなること**: なぜ 3 値なのか、なぜ既に検証済みのはずの循環をもう一度見るのか。

**答え**: 白灰黒 DFS です。done は再計算を省くメモ化で、active への再入が循環。active と done を分けないと「親を共有する複数の子」を循環と誤報します。歩く機構は `resolveParentOrder()` 1 つで、scene load([scene.cpp の呼び出し](../../src/core/loader/scene.cpp#L1358))・編集 transaction・preview が同じ template を使い、**誰を起点にするか(participant filter)と、循環 / 不在親で何を投げるかだけが呼び手ごと**です。scene load では循環・未知親・曖昧親が parse 段で既に弾かれているので、ここでの active への再入は内部不整合として `logic_error` です(投影は early_publish 内、entity 生成・親リンク後・behavior の `onInit` より前)。parse 段側の DFS は[第3章「3値のDFSが名前を出す」](03_project_and_loading.md)が本体です。

#### [`ProjectBasicConfig::saveSceneDocument()`](../../src/core/loader/basicconfig.cpp#L947) — 複数の注入点と baseline 再検証

**分からなくなること**: baseline を取ったのに、置換直前でもう一度検証するのはなぜか。

**答え**: [第3章「保存のTOCTOU窓」](03_project_and_loading.md)が本体です。1 点だけ補うと、[919 行のコメント](../../src/core/loader/basicconfig.cpp#L953)が baseline を「scene bytes + prefab read-set」の 1 つの不変物として固定すると明言しており、注入点は各区間の外部変更をテストで作るためのものです。

#### [`ProjectBasicConfig::publishSceneDocument()`](../../src/core/loader/basicconfig.cpp#L728) — revision と resolver generation が進む条件

**分からなくなること**: revision と resolver generation がいつ進むのか(prepare で進むのか、publish で進むのか)。

**答え**: **公開 1 回につき 1 進みます**。prepare しただけでは進みません。不変式は「公開した projection の値 + 1 に揃っている」ことで、初回公開はこの関数の中で [`++scene_metadata.next_scene_revision`](../../src/core/loader/basicconfig.cpp#L753) が、2 回目以降は [`publishPreparedSceneState()`](../../src/core/loader/basicconfig.cpp#L764) が `revision + 1` / `generation + 1` を代入して揃えます。上限到達は[関数先頭の 2 つの `overflow_error`](../../src/core/loader/basicconfig.cpp#L729) が公開自体を止めます。lazy init 側の入口は [`sceneProjectionState()`](../../src/core/loader/basicconfig.cpp#L784) です。

#### [`PreparedEffectTransaction::errors_`](../../src/core/loader/effecttransaction.hpp#L142) — なぜ capacity + 1 か

**分からなくなること**: 末尾の 1 スロットが何のためにあるのか。

**答え**: 先頭 256 スロットは診断 1 件 1 スロットで [`record()`](../../src/core/loader/effecttransaction.cpp#L97) が使い、満杯なら黙って捨てます(`if (error_count_ >= sceneEffectCapacity) return;`)。**末尾の +1 は診断用ではなく outbox 溢れ専用の予約席**で、[`reserveEntry()`](../../src/core/loader/effecttransaction.cpp#L119) がここだけを使い、積めなかった副作用の件数を `effect_outbox_full` の `dropped` として数え続けます。溢れ報告が診断枠を食って消えると「何件落ちたか分からない」状態になるため、席を分けてあります。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`rollback` ラムダ](../../src/core/loader/scene.cpp#L1734) | 失敗時の巻き戻し | 順序の原則と、何もしない参加者 | → §9.22「rollback は公開の逆順」 |
| [static_assert 群](../../src/core/loader/scene.cpp#L1082) | commit tail の `noexcept` 固定 | 20 個の表明が何を守るか | → §9.22「`noexcept` なコミットテール」 |
| [`PendingSceneTransactionScope`](../../src/core/loader/scene.cpp#L225) | thread_local + RAII の ambient context | なぜ引数で渡さないのか | early publication が呼ぶ**全 game-DLL callback**に同じ transaction-local な effect を観測させるため。`requestLoad()` は `active_pending_scene_transaction->loader == this` を見て live ではなく candidate へ書く |
| [run_nonce の初期化](../../src/core/loader/scene.cpp#L167) | `ticks ^ pointer`、0 を回避 | なぜ 0 を避けるのか | 0 は既定構築の `SceneAuthority{}`(= まだ何も commit していない)に予約されている |
| [`stagePreparedSceneModel()`](../../src/core/renderer/polygoninstancecontainer.cpp#L623) と publish の分離 | 容量検査 + 確保 / 挿入 | なぜ 2 つに割るのか | stage が `ModelInstanceId` を返す失敗しうる側で、loader は公開前にその id を `SimpleModelViewComponent` へ書く必要があるから |
| [`SceneLoadRequest`](../../src/core/loader/sceneload.hpp#L222) の Borrowed / Owned | 入口ごとの所有形 | 2 形態がある理由 | Borrowed は commit 済み projection の読み直し(metadata は複製されるだけ)、Owned は import root で未公開の候補を token ごと所有し、失敗時は token を捨てるだけ |
| [`MetadataCommitToken`](../../src/core/loader/sceneload.hpp#L195) | opaque pointer | なぜ中身を隠すのか | 呼び手が「document だけ」「projection だけ」を公開できないようにするため。唯一の commit は `SceneMetadataState` 全体の swap |
| [transform → localtransform 自動付与](../../src/core/loader/scene.cpp#L397) | `transform` を持つ全 object への補完(GP-1) | なぜ loader が component を足すのか、足した local は保存されるのか | authored なのは local TRS、runtime の読み手は world の `TransformComponent`。`LocalTransformSystem` は local を持つ object の world を local から毎フレーム作り直す。GP-1 から scene の `localtransform` 宣言が無い全 transform object に足す(以前は scene に `parent` が 1 つでもあるときの親子参加者だけで、parent の無い scene では `setLocalTransform` が `false` を返していた — 解消済み)。足した分は JSON を持たない `injected_local` で、値は投影段が authored の transform から書く。authored object の component ではないので**保存には出ない**。world を直接書く経路(`SceneLoader::applyObjectTransform`・editor の spawn)は local も書く。足さないのは editor の `add_component` で後から足した `transform`(次の load まで)と `load_gltf` の transient object |
| [`sceneLoadMutationEnabled()`](../../src/core/loader/sceneloadfault.cpp#L30) | SceneLoadRoot の 5 値 | 分類の意味 | 本番の入口は startup / rpc / pending / import_versioned の **4 種**で、manifest の `role = production_root` 行(4 行)と 1:1(`import_unversioned` は MG-2b で SnapshotV1 と一緒に撤去)。enum 先頭の `unspecified` だけは manifest 行も mutation id も持たない |
| [`create_object` ラムダ](../../src/core/loader/scene.cpp#L1253) | prepareCreate → publish → finish | 三段の不変式 | → §9.22「三段トランザクション」/ [第4章「生成transactionの三段構え」](04_ecs_deep_dive.md) |
| [`markComponentChanged`](../../src/core/loader/scene.cpp#L1340) | 親を書いた直後の版上げ | なぜ手動で呼ぶのか | `tryComponent<T>()` は生ポインタを返すだけで版を上げない(版を上げるのは [`setComponent()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L373) 側)。`local->parent` を直接書いた経路では手動で上げないと change detection が親リンクの変化を見落とす |
| [`loadColliderComponent()`](../../src/core/loader/scene.cpp#L285) | `runtime_codec` の null 検査 | なぜ `logic_error` なのか | resolver が具体 component に codec を必ず付ける契約なので、null は authoring の誤りではなく内部不整合。同関数の `ResolutionFailureException` 経路(= 作者の誤り)と行き先を分けている |
| [`loadTransientGltf()`](../../src/core/loader/scene.cpp#L2179) | 二重予約と `owns_resources` | なぜ 2 回予約するのか | ハッシュノードとバケット成長を**パース・GPU staging より前**に確保し(コメントが明言)、entity 生成後の公開が確保できない = transaction を割れないようにする。容量拒否は副作用の無い [`inspect()`(WP110)](../../src/core/loader/scene.cpp#L2218) + `preflightModelInstance` で先に行う |
| [`displayObjectName()`](../../src/core/loader/scene.cpp#L275) | 診断用の表示名 | 常に `<unnamed>` に見える | 空のときだけ `<unnamed>` を返す 1 行。identity ではなく**表示**なので、同一性の判定に使わない |
| [`scenePersistenceBaseline()`](../../src/core/loader/basicconfig.cpp#L380) | prefab closure と read-set | baseline が何の複合か | scene ファイルの digest + resolve が実際に読んだ prefab 集合(name/path/digest)。registry に無い prefab が closure にあれば `logic_error` |
| [`SceneMetadataState`](../../src/core/loader/basicconfig.hpp#L106) | 3 本の番号の役割分担 | revision / generation / object_id の違い | revision = authoring document の改訂、resolver generation = 解決結果の世代(読み手のキャッシュ無効化)、next_authoring_object_id = 宣言 ID の払い出し高水位(scene をまたいで再利用しない) |
| [`temporaryScenePath()`](../../src/core/loader/basicconfig.cpp#L447) | atomic カウンタ + nonce | 2 系統ある理由 | atomic は同一プロセス内の連続 save、steady_clock nonce は別プロセス・再起動・前回の残骸 tmp。保存先と同じ親ディレクトリに置くので `EXDEV` を踏まない |
| [`JsonLoader` の CLI / project / default 3 層](../../src/core/loader/basicconfig.cpp#L64) と [prefab_registry 構築](../../src/core/loader/basicconfig.cpp#L639) | 設定のフォールスルー | 初期化順序の根拠 | 優先順位は実装どおり(CLI > project > default)。**順序をその形にした理由はコードにも文書にも無く、未執筆** |
| [`importSceneDocument()`](../../src/core/loader/basicconfig.cpp#L804) | import のトランザクション意味論 | schema version 1/2 の扱い | **未執筆**(§9.23.10 の gate 順序と対で読む必要がある) |
| [`CallbackPhase`](../../src/core/loader/effecttransaction.hpp#L29) と [destroy 相で診断を積む catch(`playSound()` の例)](../../src/core/loader/effecttransaction.cpp#L163) | activate / destroy / flush | destroy でだけ再送出しない理由 | destroy の呼び出し元は `noexcept` かつ**既に commit 済みで rollback 不能**なので、throw は terminate か中途半端な破棄にしかならない。固定長診断へ落として `CommittedWithEffectFailure` として報告する |
| [`busValue()`](../../src/core/loader/effecttransaction.cpp#L36) / [`effectErrors`](../../src/core/loader/effecttransaction.cpp#L678) | const 版の copy、ErrorFields 分岐 | 何を分けているのか | **未執筆**(根拠がコードにない) |

### 9.23.2 resolver / componentcodec / prefab

#### [`validateOriginTransitionMap()`](../../src/core/loader/resolvedscene.cpp#L493) — gid / component_key / authored_index の三者

**分からなくなること**: 3 つの識別子が何を結び、この検査が何を守っているのか。

**答え**: `component_key` は prefab 側のスロット名、`gid = prefabGeneratedId(instance_id, component_key)` はシーン内で一意な生成物 ID、`final_authored_index` は unpack 後に authored 配列のどこへ着地したかです。三つ組は「prefab 展開済み snapshot → authored snapshot」の遷移が**損失なく可逆な改名**であることを守ります。[`buildOriginTransitionMap()`](../../src/core/loader/resolvedscene.cpp#L626) が作り、[`invertOriginTransitionMap()`](../../src/core/loader/resolvedscene.cpp#L684) が反転させます。**この表は editor journal(§9.23.9)と behavior arena(§9.23.16)でも同じものが使われます。**

**台帳**: 生成 ID のバイト定義は [prefab.hpp のコメント](../../src/project/prefab.hpp#L224)どおり WP361 v4。

#### [`ComponentCodec::completeDefaults()` の camera 補完](../../src/core/loader/componentcodec.cpp#L2174) — aspect を継承しない

**分からなくなること**: 省略されたフィールドが「既定から継承される」ものと「クリアされる」ものに分かれる基準。

**答え**: camera の既定補完は `resolveComponent()` の中の分岐ではなくなりました。resolver は codec へ[一行で委譲する](../../src/core/loader/resolvedscene.cpp#L302)だけで、規則は [`completeCameraDefaults()`](../../src/core/loader/componentcodec.cpp#L1085) にあります。その中の[コメント](../../src/core/loader/componentcodec.cpp#L1121)が "aspect is intentionally not inherited when it is omitted" と書いているとおり、**aspect はビューポート由来の値**なので、**projection を明示したうえで** aspect を省略したときは継承せずクリアします(`projection` 自体を書かなければ既定の投影を丸ごと継承するので、そのときは aspect にも既定値が入ります)。perspective は yfov、orthographic は xmag/ymag が必須で、既定の projection kind が同種のときだけ既定値へフォールバックできます(異種なら `camera.*_required_for_*` 違反)。znear/zfar は両方で継承、ortho では aspect を reset します。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [4 集合の照合](../../src/core/loader/resolvedscene.cpp#L644) | 全単射の検査 | なぜ 4 つも要るのか | `expected_*` は 2 つの snapshot から導いた真値、`actual_*` は map entries 側の実測。挿入の一意性 + 集合の完全一致で全単射を証明し、取りこぼし・重複・区間外 index へのエイリアスを排除する(これが `invertOriginTransitionMap` の健全性の前提) |
| [`resolverInputsFingerprint()`](../../src/core/loader/resolvedscene.cpp#L756) | resolver 入力の SHA256 | 何のための指紋か | camera 既定投影 + sprite 既定の正準 SHA256 を closure に載せ、[期待値比較](../../src/core/communication/editorcommandservice.cpp#L415)で不一致なら stale として再解決を強制する |
| [`resolveComponent()`](../../src/core/loader/resolvedscene.cpp#L211) | origin タグと遷移規則 | authoring / generated / prefab の別 | origin は型で保持され(generated は gid・prefab・instance_id・component_key・digest を持つ)、非 generated 経路に生成マーカーが混入したら拒否する |
| [behavior の canonicalize_params 呼び出し](../../src/core/loader/resolvedscene.cpp#L255) | behavior だけ別経路 | なぜコア codec を通さないのか | behavior はコア codec を持たず、**登録済み behavior の schema が受理の正**だから。[211 行のコメント](../../src/core/loader/resolvedscene.cpp#L255)どおり pre-WP360 の親 RPC 表現を再現するため、正準化結果を `source_json_exact["params"]` へ書き戻す |
| [`ResolvedSceneResolver::resolve()`](../../src/core/loader/resolvedscene.cpp#L779) | 4 フェーズ | 相の前提と依存 | 1a は codec を見ずに instance 検証、1b は provider 検証後に具体 JSON へ置換、2 は authored-first / generated-second で materialize し transform 個数・重複名・gid 一意性を**デコード前に**強制、3 でオブジェクト同一性を凍結、4 で宣言順に canonicalize。「構成規則の違反は runtime decode より前に落とす」ため 2 が 4 に先行する |
| [`name != "behavior"` の重複検査](../../src/core/loader/resolvedscene.cpp#L903) | 重複 component の拒否 | behavior だけ通る理由 | behavior は 1 オブジェクトに複数付けられる唯一の種別(`type` で区別)。gid 一意性は behavior にも等しく適用される。なおこの重複検査が走るのは **prefab instance の merged component 集合に対してだけ**で、素の object の重複はこの経路では見ていない |
| [`requireNonZeroQuat()`](../../src/core/loader/componentcodec.cpp#L407) | 四元数の検査 | なぜ正規化しないのか | 弾くのは長さゼロの退化四元数だけで(`OutOfRange` "must be a non-zero quaternion")、値には触れない。宣言 schema は成分ごとの range しか持てないので、値全体の規則であるこの検査だけが手書きで残る。正規化しない理由は関数のコメントに書かれた: decode → `encodeCanonical` のバイト厳密な往復を守るためで、正規化するのは world への合成側([`composeWorldTransform()`](../../src/core/loader/componentcodec.cpp#L2277)) |
| [`projectTransform()`](../../src/core/loader/componentcodec.cpp#L650) | 親からの逆変換 | 順序と epsilon の意味 | `applyTransform` の合成を厳密に逆順で解く(親並進を引く → 親回転の逆 → 親スケールで割る)。epsilon は親スケールの軸が 0 近傍のとき inf/NaN を静かに作らず `transform.parent.scale` の OutOfRange として名指しで落とすため |
| [camera.controller の二重指定](../../src/core/loader/componentcodec.cpp#L989) | nested と非 nested | どちらが優先か | **優先順位は存在しません**。両方あれば `UnknownField` で拒否するので、`controller = nested_controller` の代入に到達するのは非 nested が無いときだけです(「nested 優先」は誤読) |
| [`decodeRayInstanceFlags()`](../../src/core/loader/componentcodec.cpp#L1649) | instance_flags の decode | 制約と排他の理由 | 「文字列配列」「既知 4 名のみ」「重複禁止」「`force_opaque` と `force_no_opaque` は排他」。最後は AS インスタンスフラグとして同時指定が矛盾するため。各失敗は `LegacyBranchDescriptor` 表に登録された `pelican_ray_instance.*` の安定 branch id を持つ(**その id を何に使うかを述べた記述はコードにない**) |
| [`decodeCollider()`](../../src/core/loader/componentcodec.cpp#L1349) | normalized にしてから宣言 schema | なぜ二段なのか | **向きが逆です**。受理の唯一の権威は codec の宣言 schema 側で、[`ColliderComponent::loadFromJsonArchive()`](../../src/core/userpublic/components/collider.cpp#L39)(= `ref()` 経路)は `requireComponentCodec("collider")` を引いてこの codec へ委譲します。前段の `normalized` が残るのは、`shape` 自体の既定(`sphere`)と shape 依存の既定値(sphere/capsule の radius、box の half_extents、capsule の half_height)が codec 側の正準既定だからで、加えて **その shape が読まない鍵を `erase` してから** schema へ渡します(`{"shape":"sphere","half_height":-1}` が受理されたままなのはこのため。宣言 range に見せると落ちる)。値全体の規則(厳密な正値、どの shape にどれが効くか)は JSON を通らない生成経路のために [`ColliderComponent::validate()`](../../src/core/userpublic/components/collider.cpp#L47) に残ります |
| [`decodeSchemaFieldsWithLegacyBranches()`](../../src/core/loader/componentcodec.cpp#L605) | schema の失敗を v1 の branch id へ訳す | 宣言化で拒否 id が変わらない理由 | `decodeSchemaFields` を try で包み、`StructFieldValidationError` の path と [`reasonOf()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L136) の `StructFieldErrorReason` を規則表([collider の表](../../src/core/loader/componentcodec.cpp#L1263))で引いて、旧手書きパーサと同じ `collider.*` / `sprite_view.*` の branch id で `legacyReject` する。表に無い理由は素通しで `<codec>.schema_validation` になるので、[`legacyRulesAreTotal()`](../../src/core/loader/componentcodec.cpp#L551) の `static_assert` が「その field の型と presence から到達しうる理由」を全部覆っているかを **compile 時に**要求する。shape のように規則が分岐する field は `discriminant` を持ち、[`legacyRuleDiscriminantsAgree()`](../../src/core/loader/componentcodec.cpp#L572) が「どの shape でも同じ理由集合を覆う」ことを課す |
| [`decodeCamera()`](../../src/core/loader/componentcodec.cpp#L877) / [`findCameraNumber()`](../../src/core/loader/componentcodec.cpp#L808) | nested / fallback / type 推論 | 既定値表が読めない | 個別規則はコードから読めるが、**既定値の一覧表としては未執筆**(camera defaults 表と対で図示が要る) |

### 9.23.3 ECS

#### [`State::publish()` の backfill](../../src/core/ecs/archetypemigration.cpp#L98) — swap-erase が id_table の参照をどう保つか

**分からなくなること**: 行を 2 回動かしているのに、どちらの entity の参照も壊れない理由。

**答え**: [第4章「移行の反映時に行う要素移動と参照の更新」](04_ecs_deep_dive.md)が本体です。1 点だけ補うと、`backfilled_entity != entity` のときだけ最終行の `ref` を書き戻すのは、**対象 entity 自身が最終行だったときの二重書き込みを避ける**ためで、この条件を落とすと参照が 1 つ前の位置に固定されます。

#### [`populate` → `cb_init` の順序](../../src/core/ecs/archetypemigration.cpp#L562) — publish 前に init する理由

**分からなくなること**: ユーザー値の書き込み(populate)と型のライフサイクル初期化(`cb_init`)の順序と、なぜ公開前に済ませるのか。

**答え**: `populate` は呼び出し側の authored / ユーザー値を staging へ書くコールバック、`cb_init` は登録型の初期化フックで必ず populate の後に走り、走ったことが記録されます(discard / rollback が `cb_deinit` を対称に呼べるように)。記録先は経路で違い、component 1 個を足す migration は bool の `added_initialized`、複数 component を作る entity 生成は `initialized_indices` の配列です。公開前に済ませるのは **prepare が唯一失敗してよい局面**だからで、[ヘッダの "Publication must not fail"](../../src/core/ecs/archetypemigration.hpp#L44) の契約上、公開時点でコンポーネントは完全構築・初期化済みでなければなりません。[`prepareCreate()` の populate → cb_init](../../src/core/ecs/archetypemigration.cpp#L1047)も同じ形です。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`rollbackPublished()`](../../src/core/ecs/archetypemigration.cpp#L229) | 新しい `MutationScope` を作る | なぜ作り直すのか | publish 末尾で `mutation.reset()` してスコープを手放しているから(「集約エディタトランザクションの残りにコア全体の構造ガードを持ち越さない」)。`mutation_active` が再入を `logic_error` で弾くので巻き戻し中も排他は保たれる |
| [`finishPublished()`](../../src/core/ecs/archetypemigration.cpp#L352) | 取り除いた component の `cb_deinit` | なぜ finish なのか | **remove 経路で退避した実体を deinit するのはここだけ**(add 側の候補は `discardPrepared()` / `rollbackPublished()` が対称に deinit する)。削除が最終確定するのは finish の時点だけで、それ以前は `rollbackPublished` が退避物を source chunk へ復活させる。prepare/publish で deinit すると巻き戻しに必要な状態を壊す |
| [`prepareContext()` 経由の adapter 登録](../../src/core/ecs/archetypemigration.cpp#L620) | 「prepare は throw してよいが状態を変えない」 | どう強制しているのか | adapter は prepare を呼ぶ**前**に `state->adapters` へ push されるので、prepare が投げても外側の `catch (...)` → `discardPrepared()` が逆順に全 adapter の `rollback()` を呼ぶ。`rollback()` / `publish()` は純粋仮想側で `noexcept` を要求 |
| [staging construct](../../src/core/ecs/archetypemigration.cpp#L125) | 既存 chunk への構築とプレースホルダ | tail capacity と SoA 配置の関係 | コメントどおり「prepare が live storage に触れずに失敗できるよう全 target component を staging に構築する。既存 chunk は raw tail capacity を既に持っているので、**新しく足した値だけを残し、entity 由来の component のプレースホルダは捨てる**」。SoA 配置との関係そのものは**未執筆** |
| [live / dead の判定](../../src/core/ecs/archetypemigration.cpp#L466) | `core.resolve(entity)` | live の定義 | **未執筆**(id_table の live フラグと free_indices の関係を書く必要がある) |
| [reusable chunk の逆順探索](../../src/core/ecs/archetypemigration.cpp#L594) / [生成側の同じ探索](../../src/core/ecs/archetypemigration.cpp#L1063) | 空き chunk の選択 | 逆順に探す理由 | **未執筆**(根拠がコードにない。移行側と生成側が同じ `rbegin` 走査で、"packing" という語も概念もコードには無い) |
| [destroy 時の staged_chunk への退避](../../src/core/ecs/archetypemigration.cpp#L1115) | 破棄前の退避 | なぜ退避するのか | rollback-after-destroy の逆操作を可能にするため(finish まで実体を捨てない。上の `finishPublished` と同じ原理)。実体は copy ではなく relocate(move) |
| [adapter の境界](../../src/core/ecs/archetypemigration.hpp#L41) | composition boundary | light / collider が別 adapter な理由 | コメントが "This is only a composition boundary. Light, collider, behavior, and other special attachments remain separate adapters owned by their respective projection WPs." と明記している |
| [`registerComponent()`](../../src/core/ecs/componentinfo.cpp#L25) | ComponentId == index | 添字と ID が同一である帰結 | → §9.5「Component ID は dense index でもある」 |
| [`recordDependencyLocked()`](../../src/core/container.hpp#L83) | 依存循環の実行時検出 | なぜ事前宣言できないのか | → §9.1 の難所「遅延生成の 49 行」(`construction_stack` と `recursive_mutex` の対) |
| [root の近道](../../src/core/ecs/predefined/localtransformsystem.cpp#L74) | `LocalTransformSystem` が root を毎フレーム local から作り直す | `TransformComponent` を直接書いたのに次のフレームで元に戻る | GP-1 から scene の transform object は全部 local を持つので、root も毎フレーム `world = compose(local)` で上書きされる(以前は親子の無い object には local が無く、world の直書きが残っていた)。world を動かすなら local を書く: 公開 API は `setLocalTransform`、エンジン内は [`SceneLoader::applyObjectTransform()`](../../src/core/loader/scene.cpp#L2134) のように world と一緒に local も書く(親があれば親の world で逆算)。test で world を直接書いていたものも同じ理由で次フレームに戻る |

### 9.23.4 renderer / frame

#### [`plan` と `target_plan` の照合](../../src/core/vkcore/renderer.cpp#L1764) — 2 つのプランが並ぶ理由

**分からなくなること**: 論理プランと物理プランのどちらが「正」なのか、`fused_rendering_scope` はいつ true になるのか。

**答え**: `plan` はバックエンド非依存の論理 FramePlan で**常に存在し**、実行ノードの name/kind 一致検証に使います。`target_plan` は物理スコープ / 解像度計画を持つ**任意項目**で、null で**かつ** authored schedule も渡されていないときだけ [fallback schedule](../../src/core/vkcore/renderer.cpp#L1596) をその場で組み立てます(どちらか一方でもあれば渡された schedule で走ります)。[`fused_rendering_scope`](../../src/core/vkcore/renderer.cpp#L1818) は「native scope executor が無い」かつ「render パスで compiled pass が fused 指定」のときのみ true = プロバイダ所有スコープとエンジン所有の dynamic-rendering 融合スコープを**排他**にする条件です。プランの層構造そのものは[第6章「物理プランは『置換可能な完全パッケージ』」](06_rendering_vulkan_shader.md)が本体です。

#### ["The provider owns all ordering"](../../src/core/vkcore/renderer.cpp#L2032) — 責任分界

**分からなくなること**: エンジンが barrier を出す区間と出さない区間の境目。

**答え**: native scope は 1 つのコールバックが物理スコープ全体を記録するので、source ノードが同一 native scope 内なら executor は barrier を出しません。エンジンが張るのは**スコープ境界をまたぐ依存だけ**です。前提として[スコープ区間の連続性検証](../../src/core/vkcore/renderer.cpp#L1835)が scope_node_index / scope_node_count / execution_index を全部照合し、プロバイダに渡す区間が「スケジュール上で連続した、ちょうどそのノード集合」であることを保証しています。

#### [fused scope の内部依存](../../src/core/vkcore/renderer.cpp#L2098) — なぜ dynamic rendering で実行できないか

**分からなくなること**: 同じスコープに入れたはずのノード間依存が、なぜエラーになるのか。

**答え**: fused scope は 1 つの dynamic rendering インスタンス内で全ノードを記録するため、内部依存を解く image layout 遷移バリアを**レンダリング中に発行できません**。例外は `passReadsBarrierAsLocalAttachment` / `isPhysicalScopeAttachment` が真のケース(同一スコープの attachment を local read で読む)で、これは dynamic rendering local read が保証するので barrier 不要です。判定は [fused scope 側の検証](../../src/core/vkcore/renderer.cpp#L1904)にあります。

#### [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1585) — main family と他 family で schedule 構築が違う

**分からなくなること**: なぜ main family 以外では `view_index != 0` を捨てるのか。

**答え**: main family だけが view ごとに記録されます(sequential 実行・`logical_view_count` を受け取る)。他 family は view 非依存で single_view / view 数 1 として組まれるので、`view_index != 0` を入れると**同じノードを view 数だけ二重記録**します。スケジュールの並び自体は[第6章「スケジュールは scope-execution-major」](06_rendering_vulkan_shader.md)が本体です。

#### [`transaction->generation == 0`](../../src/core/vkcore/renderer.cpp#L3652) — 番兵としての 0

**分からなくなること**: 世代 0 が「世代が 0 番」なのか「世代が無い」なのか。

**答え**: [`after_publish` フック](../../src/core/vkcore/renderer.cpp#L3628)が未実行、つまりランタイム世代がまだ publish されていない状態を表す**番兵**です。その場合は [`stagedGeneration()`](../../src/core/vkcore/renderer.cpp#L3651) を代用世代として採用し、staged と一致するときだけ staged 扱い(commit ではなく保留)にします。同じ番兵は [`StagedRuntimeRollbackGuard`](../../src/core/vkcore/renderer.cpp#L3104) 側でも「staged 無し = ロールバック不要」として使われます。

#### [`beginLogicalFrameTarget()`](../../src/core/vkcore/renderer.cpp#L5985) — 出力待ちで飛ばした frame は staged な世代を巻き戻さない

**分からなくなること**: frame が途中で終わったとき、staged な ray tracing の世代を巻き戻すのか残すのか。

**答え**: 記録 object は staged な世代を巻き戻す guard を持ち、frame が途中で失敗すれば巻き戻します。ただし窓の出力にまだ image が無くて frame を飛ばす場合(`OutputTemporarilyUnavailable`)は、GPU の作業も確定もしていないので **guard を外して残します**。ray tracing の起動世代は最初に提出した frame で初めて公開されるので、ここで巻き戻すと公開済みの世代が 1 つも無くなり、次の frame が `Renderer logical frame requires a compiled render pipeline` で落ちます。Studio に埋め込んだ player は shader cache が効くと窓の出力より先に最初の frame が来るので、この経路を実際に通ります(`7cb39996`)。出力の条件が変わった場合(`OutputRelowerRequired`)は今まで通り巻き戻して作り直します。公開前の世代は非公開のままで、`get_frame_plan` はその間「plan が登録されていない」と答えます(WP384 の契約)。

3 つの行き先(公開 / 次の frame へ持ち越し / 巻き戻し)は [`StagedRuntimeRollbackGuard`](../../src/core/vkcore/renderer.cpp#L3104) の上のコメントにまとめてあり、呼び手は `published()` と `keepForNextFrame()` で行き先を名指しします。どちらでもない終わり方は destructor が巻き戻します。この経路はふつう実窓でしか通らないので、GPU テスト「a frame skipped for want of an output image keeps the staged ray-tracing startup generation」は [`RenderTargetTestAccess`](../../test/rendertarget_testaccess.hpp#L14) で次の N frame を「出力に image が無い」にして、2 frame 飛ばしても staged の世代が残り、次の frame が同じ世代を公開することを確かめます。`keepForNextFrame()` を外すと赤になります。

#### [`buildLogicalFrameViewFamilySchedule()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L120) — multiview と sequential の差

**分からなくなること**: schedule に view の数(cardinality)が要る理由。

**答え**: multiview は 1 回の記録で全 view を出すので schedule は view 非依存、sequential は view ごとに [`selectLogicalFrameSequentialViewSchedule()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L494) が別 schedule を選び、同じノードを view 数だけ実行します。cardinality が要るのは、**main family の view 数が物理 target plan の view 数と一致することを検証**し、二次 family(shadow / reflection)の物理スコープは単一 view のテンプレートなので**それを何回 sequential に展開するか**をスケジュール側で決める必要があるからです([コメント](../../src/core/renderingpass/viewexecutionscheduler.hpp#L283)「Runtime family cardinality expands that template sequentially」)。

#### [`makeFrameDescriptorPoolPlan()`](../../src/core/renderer/frameresources.cpp#L28) — multiplier 3 / 6 の根拠

**分からなくなること**: 3 と 6 という定数が何を数えているのか。

**答え**: 1 スロットあたり **uniform 3 個**(frame UBO / resolution UBO / light UBO)と **storage 3 個**(object / previous object / directional shadow)を消費するので係数 3 です。ray_query 時はスロットごとに通常セットと ray_query セットの 2 セットを確保する(`max_sets = slot_count * 2`)ので 3×2 = 6 になり、加えて acceleration structure を `slot_count` 分積みます。内訳は [frame/resolution UBO の 2 write](../../src/core/renderer/frameresources.cpp#L205) と [`updateSceneDescriptors()` の 4 write](../../src/core/renderer/frameresources.cpp#L312)(storage 3 + light UBO 1)を足すと一致します。

#### [`GpuSubmissionLease`](../../src/core/vkcore/frametarget.hpp#L53) — 型消去リースが 2 種類ある

**分からなくなること**: `submission_lease_` と `target_epoch_lease_` が同じ型なのに別物である理由。

**答え**: 型は「1 回のキュー submit が参照する全 GPU オブジェクトの型消去された所有ルート」で、任意型の資源束を型を知らずに保持するため `shared_ptr<const void>` です。`submission_lease_` は**その 1 回の submit が参照する資源をフェンス完了まで生かす**もの、[`target_epoch_lease_`](../../src/core/vkcore/frametarget.hpp#L97) は**出力ターゲット側の epoch(swapchain 世代など)を記録中に生かす**別軸で、submit 時に [`consumed.target_epoch_lease.get() != active.epoch.get()`](../../src/core/vkcore/swapchainframetarget.cpp#L2474) として「このフレームは今の epoch のものか」の照合にも使われます。破棄の時刻そのものは §9.12 の難所「破棄の時刻を決めているのは refcount 1 本」が本体です。

#### [`RetirementBatch`](../../src/core/vkcore/deletionqueue.hpp#L50) — なぜ weak_ptr で持つのか

**分からなくなること**: [`flushAll()`](../../src/core/vkcore/deletionqueue.cpp#L88) の `lock()` が失敗する条件。

**答え**: 実所有者は `current_batch` と [`SubmissionLeaseBundle`](../../src/core/vkcore/deletionqueue.hpp#L67) で、キュー自身は weak 参照しか持ちません。submit 失敗でリースが捨てられた場合や、フレームターゲットがフェンス完了後にリースを解放した場合、batch は自動で消えます。`flushAll` の `lock()` はその**既に解放済みバッチ**で失敗し、エントリを erase します。

#### [`stagePreparedSceneModel()` の reuse 判定](../../src/core/renderer/polygoninstancecontainer.cpp#L623) — スロット再利用と世代

**分からなくなること**: 「再利用」と「新規確保」をどこで見分けているのか。

**答え**: `instance_slots.nextId(size)` は空きスロットがあればその index を、無ければ現在サイズを返すので、`index != model_instances_data.size()` が再利用、等しければ新規確保です。これで reserve 量や `animation_generations` の引き継ぎを切り替えます。ID の generation は再利用スロットを指す古いハンドルを無効化するためのもので、scene epoch とは別軸です(→ §9.2「`ModelInstanceId` の落とし穴」)。

#### [`uploadMaterialAbsoluteOverrides()`](../../src/core/renderer/polygoninstancecontainer.cpp#L1974) — instance_identity が 1-indexed

**分からなくなること**: なぜ添字を 1 つずらすのか。

**答え**: header は `maxModelInstances` 個の密配列で `headers[instance_identity - 1]` として索引します。**0 は「オーバーライド無し / 未設定」の sentinel** なので、`instance_identity == 0` は範囲外として例外になります。morph frame identity([`instance_identity = index + 1`](../../src/core/renderer/polygoninstancecontainer.cpp#L521))も同じ 1-indexed 規約です。

#### [phase × sort_view_index の直積](../../src/core/renderer/polygoninstancecontainer.cpp#L1075) — variants を事前展開する目的

**分からなくなること**: なぜフレーム中に選ぶのではなく、全組み合わせを先に作るのか。

**答え**: コメント "Compile phase/view variants without changing the live inventory" のとおりです。opaque と transparent は別 sort provider でソートされ、透過ソートは view の origin / forward に依存するので **sort view ごとに順序が変わります**。全組み合わせを事前コンパイルし、`CompiledDrawQueueSet::combine` が 1 本の indirect buffer に range リベースして平坦化します。ソート鍵の作り方は[第6章「draw queue は provider に鍵だけ作らせる」](06_rendering_vulkan_shader.md)が本体です。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [timing の first / last](../../src/core/vkcore/renderer.cpp#L1640) | GPU timing のクエリスロット | 2 つ持つ理由 | 1 つのノードが複数 schedule position に現れうる(view ごと繰り返し)ため、スロットは初出時に 1 つだけ割り当て、first を区間開始・last を計測終了に使って**繰り返し全体を 1 区間として囲む** |
| [`resolveFrameResolutionExtents()`](../../src/core/vkcore/renderer.cpp#L811) | render_extent と output_extent | 2 つある理由 | `render_source_resource` と `output_source_resource` を別々に解決し、内部レンダー解像度と最終出力解像度が異なるアップスケール構成を表す。両者はコンパイル済み plan と一致検証される |
| [jitter の 2 値](../../src/core/vkcore/renderer.cpp#L873) | `view_projection_jittered` と `jitter_ndc` | なぜ両方要るのか | 前者はラスタライズ時にサブピクセルオフセットを掛ける行列、後者はシェーダが速度ベクトルや TAA リプロジェクションでそのオフセットを打ち消すための生の量。→ [第6章「ジッタは w 倍で足す」](06_rendering_vulkan_shader.md) |
| [`windowOutputFactsChanged()`](../../src/core/vkcore/renderer.cpp#L2795) | fingerprint と compile_facts | 二重比較の理由 | `compile_fingerprint` は安価な一致判定用ハッシュで先に比較し、加えて実体も比較して**ハッシュ衝突と fingerprint 未包含フィールドの差分**を取りこぼさない二段確認 |
| [companion 3 点セット](../../src/core/vkcore/renderer.cpp#L3221) | companion_participants / claims / apply_with_companions | 3 つの役割 | companion が「一緒に取り込む他参加者(shader / material)」を宣言し、claims が所有リクエストを判定し、apply_with_companions が 1 回で処理する。グラフ・シェーダ・マテリアル値の変更が**同一トランザクション**として一括成否になることを保証(→ §9.23.12) |
| [`view_index == 0` の `triggerUpdate`](../../src/core/vkcore/renderer.cpp#L6048) | GPU 状態の凍結点 | なぜ 1 回だけか | コメント "Object, skin, morph, and material-override GPU state is frozen after target acquisition and before the first view records" のとおり、**同一論理フレームの全 view が同じインスタンス / 描画キュー状態を見る**ことを保証するため |
| [`LogicalFrameAbortGuard`](../../src/core/vkcore/renderer.cpp#L3077) | 2 つの RAII ガード | 何が違うのか | 前者は例外離脱時に必ず `abortLogicalFrame()`。後者は `generation != 0` のときだけ runtime を保持し、`complete()` されずに壊れたら rollback する |
| [record / end の分離](../../src/core/vkcore/renderer.cpp#L2186) | `latest_scene_color/depth` の追跡 | 役割分離の理由 | **未執筆**(根拠がコードにない) |
| [sprite anchor の layer 計算](../../src/core/vkcore/renderer.cpp#L2320) / [snapshot copy の layer](../../src/core/vkcore/renderer.cpp#L2465) | sequential layer の選択 | いつ view_index が層になるか | sequential 実行で、かつ対象の `array_layers` が view 数以上(snapshot copy 側は source が layered)のときだけ `scheduled.view_index` を層として使い、それ以外は 0。ただし swapchain 出力の color だけは `view_index` をそのまま層として引く。逆順スキャンの理由は[第6章「sprite anchor の逆順スキャン」](06_rendering_vulkan_shader.md) |
| [`per_view_sort` から `draw_sort_view_index` へ](../../src/core/vkcore/renderer.cpp#L2682) | multiview / sequential の選択 | 対応関係 | → 上記「phase × sort_view_index の直積」と対 |
| [jitter provider / pattern / phases](../../src/core/vkcore/renderer.cpp#L2950) | ジッタ設定の解決 | 3 つの関係 | → [第6章「ジッタは w 倍で足す」「epoch ペアが reset 信号」](06_rendering_vulkan_shader.md) |
| [`watched_sources`](../../src/core/vkcore/renderer.cpp#L3191) | 監視対象の集合 | 複数参照を入れる根拠 | **未執筆**(§9.23.12 の claim モデルと対で書く必要がある) |
| [`shader_oetf_unorm_fallback`](../../src/core/vkcore/renderer.cpp#L1166) | 出力変換のトレース | 分岐と paired_storage_edges | **未執筆**(§9.23.14 の format スコアリングと対) |
| [`native_scope_has_work`](../../src/core/vkcore/renderer.cpp#L1315) | 空スコープの判定 | 判定基準 | **未執筆**(根拠がコードにない) |
| [staged 世代の遷移](../../src/core/vkcore/renderer.cpp#L139) と [`after_publish`](../../src/core/vkcore/renderer.cpp#L3628) | finalized / ready / runtime_published | 対称性 | 上記「`transaction->generation == 0`」と同じ世代機構。個々の遷移表は**未執筆** |
| [`graph_variant_policy`](../../src/core/vkcore/renderer.cpp#L5181) | variant と view family の方針 | RenderGraphVariant との対応 | **未執筆**(根拠がコードにない) |
| [`configureExternalDepthSubmission()`](../../src/core/vkcore/renderer.cpp#L5968) | XR への depth 提出 | true でも export が null になりうる | **未執筆**(§9.23.14 の OpenXR depth submission と対) |
| [family / view ごとの jitter と reset epoch](../../src/core/vkcore/renderer.cpp#L3912) | トレース出力 | なぜ family/view ごとに追うのか | **未執筆**(根拠がコードにない) |
| [RT 無しの空 diagnostics](../../src/core/vkcore/renderer.cpp#L4231) | `ray_query_acceleration_structures` | 空になる条件 | **未執筆**(根拠がコードにない) |
| [`sequential_count < count` の拒否](../../src/core/renderer/frameresources.cpp#L116) | スロット数の下限 | 何を保証するか | in-flight frame ごとに sequential_count 個のスロットを確保するので、**main family の全 view が自分専用の frame/resolution UBO スロットを持つ**(view 同士の UBO 別名衝突の防止) |
| [`updateSceneDescriptors()`](../../src/core/renderer/frameresources.cpp#L312) | 全 set の無条件更新 | 差分検知しない理由 | 呼び出し側に同値比較ガードがあり、buffer identity が変わったときと `configureViewCount` の再構築時にしか来ないので、関数内で差分を取る意味がなく、全スロット書き直しの方が漏れがない |
| [multiview のスロット分離](../../src/core/renderer/frameresources.hpp#L117) | seq / multiview の別配列 | last_data が vector になる理由 | **未執筆**(上記スロット確保と対で書く必要がある) |
| [RT SBT のリース](../../src/core/renderer/frameresources.cpp#L386) / [RT descriptor bundle の publication](../../src/core/renderer/frameresources.cpp#L403) | 世代の保持と二重 bind | 何を指しているのか | **未執筆**(根拠がコードにない) |
| [`SubmissionLeaseBundle`](../../src/core/vkcore/deletionqueue.hpp#L67) | 入れ子リースの upstream | 完了順序 | **未執筆**(根拠がコードにない) |
| [`declaration_ordinal`](../../src/core/renderer/polygoninstancecontainer.cpp#L718) | 単調増加の宣言順 | 単調性が要る理由 | `DrawQueueBuilder` 側の[一意性検査](../../src/core/renderer/drawqueuebuilder.cpp#L504)(重複したら拒否)を満たし、`stableOrderKey` の**最後の要素=同点時の決定的なタイブレーク(宣言順)**になる |
| [`prepareSceneReplacement()`](../../src/core/renderer/polygoninstancecontainer.cpp#L571) / [`drainSceneRetirements()`](../../src/core/renderer/polygoninstancecontainer.cpp#L748) | 次フレーム回収 | いつ実際に解放されるか | **未執筆**(manifest の `next-frame-retirement-queue` 語彙と対で書く必要がある) |
| [`canRebuildModelInstances()`](../../src/core/renderer/polygoninstancecontainer.cpp#L1181) | 再構築前の容量計算 | 何を数えているか | **未執筆**(根拠がコードにない) |
| [world_bounds / history / `resetTemporalHistory()`](../../src/core/renderer/polygoninstancecontainer.cpp#L1159) | 前フレーム状態 | lazy 更新の条件 | **未執筆**(TAA / モーションベクタ用。→ [第6章「epoch ペアが reset 信号」](06_rendering_vulkan_shader.md)) |
| [VRM transaction の `noexcept` rollback ラムダ](../../src/core/renderer/polygoninstancecontainer.cpp#L2134) | `noexcept` な巻き戻し | なぜ投げないのか | → §9.22「三段トランザクション」「`noexcept` なコミットテール」 |
| [alpha override の優先順位](../../src/core/renderer/polygoninstancecontainer.cpp#L1434) | 絶対 / 相対の合成 | どちらが勝つか | **未執筆**(§9.23.5 の per-row 表と対) |

### 9.23.5 RT(acceleration structure・SBT・oracle)

#### [3 つの identity の組み立て](../../src/core/renderer/accelerationstructure.cpp#L674) — scene / geometry / geometry_match

**分からなくなること**: 似た文字列を 3 本も作る理由と、どれがソートキーでどれがガードなのか。

**答え**: [第11章「`RaySceneRowInput` の identity 3 種」](11_ray_query_acceleration_structures.md)が本体です。1 点だけ補うと、[コードコメント](../../src/core/renderer/accelerationstructure.cpp#L676)が「instance slot / generation / draw publication ordinal を使うと q が publish 順に依存してしまう」と明記していて、**scene_identity が authored/content 由来だけで作られているのはこの一文が理由**です。

#### [`row.q != canonical_index` の拒否](../../src/core/renderer/accelerationstructure.cpp#L805) — q と record offset

**分からなくなること**: `q` が何番なのか、SBT のレコード番号とどう繋がるのか。

**答え**: `q` は**正準ソート済み scene row 列の 0 起点連番**そのものです。この検査は TLAS 行が「静的シーンの正準置換」であることを保証し、q を配列添字として SBT / マテリアル表へ読み替えられるようにします。[`rayTracingInstanceRecordOffset(q, ray_type_count)`](../../src/core/shader/raygrouplayout.cpp#L2284) = `q * ray_type_count` は、1 インスタンスあたり ray_type_count 本の hit record を連続確保する前提での先頭 record 番号です。

#### [`index >= (1u << 24u)` の上限](../../src/core/shader/raygrouplayout.cpp#L1748) — 24 ビットの出所

**分からなくなること**: なぜ 2^24 なのか(エンジン都合か Vulkan 都合か)。

**答え**: **Vulkan の `VkAccelerationStructureInstanceKHR::instanceShaderBindingTableRecordOffset` が 24 ビットのビットフィールド**だからです。GPU に載る値は `q * ray_type_count` なのでその積を 2^24−1 で制限し、行数側でも先に弾きます(`pelican.ray_row_count_limit@1`)。**別のフィールドですが**同じ 24 ビット幅の上限が [v2 の custom_index override の 24 ビット検査](../../src/core/shader/raygrouplayout.cpp#L1600) にも掛かります(こちらは `instanceCustomIndex` 側の 24 ビットで、超過は `pelican.ray_instance_field_range@1`)。

#### [`resolveRaySceneRows()`](../../src/core/shader/raygrouplayout.cpp#L1519) — 同じ geometry_match_identity に別 binding を許さない

**分からなくなること**: 「曖昧」と判定される条件が何を意味するのか。

**答え**: geometry_match_identity は「同じ GPU ジオメトリ範囲が同じ transform で置かれている」ことしか意味しません。**物理的に区別できない同一ヒットに異なる group_index / record_data が割り当てられると、どちらのマテリアル束縛が当たるか決められない**ので、`pelican.ray_row_binding_ambiguous@1` で拒否します。

#### [profile モードの material_index](../../src/core/shader/raygrouplayout.cpp#L2190) — なぜ `material_index == q` になるか

**分からなくなること**: 通常モードとの表の作り方の違い。

**答え**: profile では alpha override が **scene row ごとに異なり得る**(base_factor_a / uv_offset_scale / uv_rotation を行単位で上書きし、`uses_vertex_color` も行のジオメトリ属性依存)ため、1 行 1 エントリの per-row 表になります。キーが `"row:<q>:<material_reference>"` なので material_index は必然的に q と一致します。通常モードは行固有の上書きが無いので canonical key で参照し、同一マテリアルを 1 エントリに共有化してテーブルを縮めます([表の構築](../../src/core/shader/raygrouplayout.cpp#L1775))。

#### [硬コードされた頂点オフセット](../../src/core/shader/raygrouplayout.cpp#L1187) — `10u` / `3u` / `12u` の出所

**分からなくなること**: 生成される GLSL に埋まる裸の定数がどこから来たのか。

**答え**: [`CommonVertStruct`](../../src/core/model/vertbufcontainer.hpp#L23)(`vec3 pos; vec3 normal; vec4 tangent; vec2 texcoord; vec4 color`、[サイズ 64・offset 0/12/24/40/48 を static_assert で固定](../../src/core/model/vertbufcontainer.hpp#L36))です。バイトオフセットを word 単位に割ると normal = 3、texcoord = 10、color0 = 12 になります。生成側は [`makeRayGroupLayoutVirtualInclude()`](../../src/core/shader/raygrouplayout.cpp#L1045)。

#### [`BlasVariantKey`](../../src/core/renderer/accelerationstructure.cpp#L916) — 一意性と再利用判定

**分からなくなること**: BLAS のキャッシュキーに transform が入らない理由。

**答え**: BLAS の build 入力は geometry_allocation_id が決める頂点 / インデックス範囲と `VkGeometryFlagsKHR` の 2 つだけで完全に決まるからです(transform は TLAS 側)。[`can_reuse`](../../src/core/renderer/accelerationstructure.cpp#L1275) は `active_state_ != nullptr && !pool_invalidated && !model_invalidated` で、この 2 つの世代は[別カウンタ](../../src/core/renderer/accelerationstructure.cpp#L1233)として診断に出ます。

#### [`descriptorSetLayoutKeysFor()` の set 数決定](../../src/core/shader/pipelinefactory.cpp#L494) — reflection と ray_layout の階層

**分からなくなること**: set の枚数を誰が決め、誰が上書きするのか。

**答え**: set 0 は frame として常に固定、それ以外は reflection の最大 set 番号から決まり、ray_layout が ray table を使う場合**のみ** set 3 をエンジン定義の bindless material / texture / geometry レイアウトで上書きします。つまり **reflection が基底、ray layout が特定 set の権威**という階層です。set 0 の契約は[第11章](11_ray_query_acceleration_structures.md)にも書かれています。

#### [shader group の stage index と SBT](../../src/core/shader/pipelinefactory.cpp#L709) — regions への対応

**分からなくなること**: stage 配列・group 配列・SBT バッファ・`traceRaysKHR` の 4 つの並びの対応。

**答え**: stage は raygen = 0、miss = 1..M、closest_hit = 1+M..、any_hit はその後ろ。group はその順で general(raygen)→ general(miss)×M → triangles hit group×N と並びます(**any_hit は独立した group にならず、対応する hit group の `anyHitShader` に畳み込まれる**ので、group 数は stage 数より any_hit の分だけ少ない)。[SBT 構築](../../src/core/shader/pipelinefactory.cpp#L818)はこの group 順に handle を取り、handle_alignment で stride を、base_alignment で各領域 offset とバッファ先頭を揃えるので、raygen / miss / hit の `VkStridedDeviceAddressRegionKHR{address+offset, stride, size}` がそのまま regions になります。

#### [`RayTracingOracleContract`](../../src/core/shader/raytracingoracle.hpp#L14) — 定数群の根拠

**分からなくなること**: 5° や 1e-3 といった閾値が経験則なのか契約なのか。

**答え**: **`docs/design_raytracing.md` の fixture subset 契約が出所**です。最小角 5°(`minimum_triangle_angle_degrees`)は退化三角形の排除、掠め入射の排除はそれとは別の `minimum_incidence_dot = 0.0871`(約 85°)、`ε_t = relative_t_epsilon · max(1,|t|)` は最近接 hit と次候補の分離(一意性)、barycentric マージンは辺・頂点近傍の回避、`endpoint_f32_ulp_padding = 4` は [GPU の f32 端点誤差の吸収](../../src/core/shader/raytracingoracle.cpp#L695)です。

#### [`intersect()`](../../src/core/shader/raytracingoracle.cpp#L226) — double と区間演算を並走させる

**分からなくなること**: 同じ式を 2 回計算しているように見える理由。

**答え**: double 側は GPU と同じ ["Normative Moller-Trumbore sequence"](../../src/core/shader/raytracingoracle.cpp#L229) を**式順序込みで再現**して u/v/t の代表値と inside 判定を出し、有向丸めの区間側は同じ式で t の**保証付き区間**を作ります。前者だけでは真値包含の保証が無く、後者だけでは一意な代表値が得られません。

#### [`parse()`](../../src/core/shader/rtspirvinspection.cpp#L615) — SPIR-V 命令走査

**分からなくなること**: 走査そのものの読み方。

**答え**: ここは pelican 固有の設計ではなく **SPIR-V モジュールの構造という前提知識**です。[第10章 前提知識の補足](10_background_knowledge.md)を引いてください。この索引にはエンジン固有の判断(何を拒否するか)だけを下の表に置いています。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`classifyRayQueryGeometry()`](../../src/core/renderer/accelerationstructure.cpp#L383) | AS 対象の分類 | skin/morph/VAT の変形出力 | skin/morph/VAT は毎フレームの変形頂点から BLAS を構築する。不適格な三角形範囲は除外診断へ出す |
| [`primitiveCount == 0` の TLAS](../../src/core/renderer/accelerationstructure.cpp#L235) | 空 TLAS の確保 | なぜ確保が要るのか | コメントどおり「Vulkan requires a valid, 16-byte-aligned instances address even when primitiveCount is zero」。非アクティブ 1 件分を必ず確保・書き込み、build 時の primitiveCount は 0 のままにして miss 専用の有効な TLAS にする |
| [三角形 branch の primitive_count](../../src/core/renderer/accelerationstructure.cpp#L1465) | primitive 数の計算 | 3 で割ってよい前提 | 三角形リスト前提(→ [第10章](10_background_knowledge.md)) |
| [`validateRayProfileAccelerationStructureFlags()`](../../src/core/renderer/accelerationstructure.cpp#L531) | eOpaque / eForceOpaque | profile 時の制約 | 両者は「片方でも立つと any-hit が省略され得る」点で等価な不透明化指定。profile 有効時は any-hit でのアルファ判定が必須なので、BLAS geometry でも TLAS instance 行でも**どちらのビットも立っていないこと**を要求し、立っていれば `pelican.ray_profile_opaque_as@1` |
| [`lowerRayTracingTlasInstancePlan()`](../../src/core/renderer/accelerationstructure.cpp#L757) | ray_scene_rows の有無で分岐 | source_index の役割 | source_index は元の instance 配列への戻り参照。分岐の全体像は**未執筆** |
| [`geometryName()`](../../src/core/renderer/accelerationstructure.cpp#L49) | asset/geometry/mesh/primitive/node | 5 階層の意味 | **未執筆**(§9.23.17 の glTF 階層と対) |
| [`RaySceneRow`](../../src/core/shader/raygrouplayout.hpp#L316) | uint32 max 初期化 | 何を表す値か | 「未解決」の sentinel。詳細は**未執筆** |
| [`MaterialRowDocumentIdentity`](../../src/core/shader/raygrouplayout.hpp#L282) | document / asset / unavailable | 3 状態の使い分け | → [第11章「`MaterialRowIdentity`: gate と lookup が同じ値を見る」](11_ray_query_acceleration_structures.md) |
| [`resolveRaySceneRows()` の宣言](../../src/core/shader/raygrouplayout.hpp#L526) | 引数と optional bindings | 何が任意なのか | **未執筆**(根拠がコードにない) |
| [layout digest から AS 宣言を外すコメント](../../src/core/shader/raygrouplayout.cpp#L859) | ダイジェストの分離 | なぜ AS だけ外すのか | コメントどおり **AS 宣言はシェーダ / SBT の ABI に影響しない**ので、AS だけを変えたときにシェーダ・SBT のダイジェストが動いて不要な再コンパイル / 再構築が起きないよう構造的に分離する(AS 側は独立ダイジェスト) |
| [`source_by_handle` / `source_by_key`](../../src/core/shader/raygrouplayout.cpp#L1835) | 2 名前空間の同時照合 | 両方見る理由 | 前者は(handle, generation, canonical_key)という**ランタイムのマテリアルリース同一性**、後者は**内容由来の正準キー同一性**で、別々に崩れうる(同一世代が違うデータを持つ / 別世代が同じキーに衝突する)。→ §9.22「identity / canonical key / lease の三層」 |
| [`texture_identity`](../../src/core/shader/raygrouplayout.cpp#L1898) | profile_active での切替 | 参照先が変わる理由 | profile の表は base color 全体ではなく**アルファ専用 IR** を載せるので、テクスチャ参照も MASK のときだけ `alpha_texture_identity` を使い、非 MASK は空にする |
| [`validateRayProfileGeometryAttributes()`](../../src/core/shader/raygrouplayout.cpp#L2236) | UV0 / COLOR_0 の必須化 | なぜ profile だけ必須か | profile の any-hit はアルファをテクスチャサンプルと頂点カラーから計算するので、texture_index が有効なら TEXCOORD_0 が、`uses_vertex_color` なら COLOR_0 が BLAS ジオメトリ側に実在しなければならない(欠ければ `pelican.ray_geometry_attribute_missing@1`) |
| [`alpha_override_mask`](../../src/core/shader/raygrouplayout.cpp#L2060) | override を行に持つ | material 側でない理由 | override は**マテリアルの改変ではなく「そのインスタンス配置におけるアルファの上書き」**で、同じマテリアルを共有する他の行に波及させてはいけないため、行に持って per-row 表に焼き込む |
| [`normalized_*` フィールド](../../src/core/shader/raygrouplayout.cpp#L2024) | 誰が計算したか | 出所 | §9.23.17 の `normalized_alpha_ir`(glTF の material lowering)。[検証側](../../src/core/shader/raygrouplayout.cpp#L1862)は WP368a の authored reference 由来であることを要求する |
| [material selector の 2 形態(document_material / asset_material)](../../src/core/shader/raygrouplayout.cpp#L659) / [`selectorMatches()`](../../src/core/shader/raygrouplayout.cpp#L470) | document 形式と asset 形式 | 比較方法が違う理由 | **未執筆**(§9.23.7 の canonical key と対) |
| [engine フィールドの必須化](../../src/core/shader/raygrouplayout.cpp#L948) | profile 有効時 | material_index と geometry_index の両方が要る理由 | **未執筆**(根拠がコードにない) |
| [`validName()`](../../src/core/shader/raygrouplayout.cpp#L132) | 名前の文字集合 | どこ由来の制約か | 外部ツール契約 R7(`docs/external_tools_requirements.md`)と同系統。→ §9.23.11 |
| [`ePartiallyBound` を binding 1 だけに](../../src/core/shader/pipelinefactory.cpp#L205) | set 3(ray material)の binding flags | なぜ binding 1 だけか | binding 1 だけが `rayBindlessTextureCapacity` 個の bindless サンプラ配列で、使うスロットはシーン依存でまばら。全スロットに有効な descriptor を書く義務を外すため必要で、単一 descriptor の binding 0/2 には不要 |
| [`ray_query_frame_set` 必須化](../../src/core/shader/pipelinefactory.cpp#L1217) | frame set の TLAS binding | 無いと作成拒否になる理由 | RT パイプラインは frame set の TLAS binding を必ず参照する契約なので、reflection に現れなければ `pelican.ray_tracing.frame_tlas_required@1`(descriptor が無ければシーンをトレースできない)。この検査を通した後、ray table 使用時は続けて[bindless device 要件](../../src/core/shader/pipelinefactory.cpp#L1227)も検証する |
| [production branch observer](../../src/core/shader/pipelinefactory.cpp#L1710) | SBT 書き込み直前の通知 | 本番に観測点がある理由 | コメントどおり「removing the actual GPU write or routing different bytes makes its A/B test fail」。→ §9.22「fault injection seam」 |
| [`discard_new_layouts` ラムダ](../../src/core/shader/pipelinefactory.cpp#L1915) | descriptor layout キャッシュの巻き戻し | all-or-nothing の作り方 | 開始時のキー集合を控え、失敗時に**今回追加された分だけ**消す。→ [第6章「reload の swap は 3 回」](06_rendering_vulkan_shader.md) |
| [`allocate_table`](../../src/core/shader/pipelinefactory.cpp#L1531) | minimum_size より大きく確保 | 余分に取る理由 | **未執筆**(根拠がコードにない) |
| [`active_owner_pipelines` の検索](../../src/core/shader/pipelinefactory.cpp#L1521) / [hit table の pack](../../src/core/shader/pipelinefactory.cpp#L1658) | 線形探索と row snapshot | 意図 | **未執筆**(根拠がコードにない) |
| [`RayTracingHitGroupOccurrence::any_hit`](../../src/core/shader/raytracingvalidation.hpp#L37) | any_hit を optional に | 省略できる理由 | §9.23.6 の any_hit 自動生成と対。**未執筆** |
| [blend attachment 分岐](../../src/core/shader/pipelinefactory.cpp#L307) / [hot reload の差し替え](../../src/core/shader/pipelinefactory.cpp#L2070) | 生成と再構築 | 分岐の基準 | **未執筆**(根拠がコードにない) |
| [`directUint32Constant()`](../../src/core/shader/rtspirvinspection.cpp#L293) | OpConstant / OpConstantNull のみ許可 | 動的値を拒む理由 | 配列長や trace 呼び出しの sbtRecordOffset / missIndex は**静的検証で確定値が要る**ため。実行時に決まる命令結果を許すと SBT / インタフェース契約を静的に検証できない(`trace_operand_dynamic@1` / `interface_type_unsupported@1`) |
| [`canonicalType()`](../../src/core/shader/rtspirvinspection.cpp#L337) | `SpvOpTypeRuntimeArray` の拒否 | なぜ失敗にするのか | 要素数がコンパイル時に決まらず、payload / hit attribute のサイズとレイアウトを確定できないため |
| [`ScalarLayout`](../../src/core/shader/rtspirvinspection.cpp#L78) | boolean を 4 バイト扱い | 根拠 | SPIR-V の `OpTypeBool` には**メモリ表現が規定されていない**(bit_width も持たない)ので、ホスト / GLSL の慣行に合わせて 4 バイトに固定する。他のスカラーは bit_width/8 で、8 の倍数でなければ「byte-addressable でない」として拒否 |
| [hit-group pair の payload 正規化](../../src/core/shader/raytracingvalidation.cpp#L428) | closest-hit と any-hit の型合わせ | 「physical hit-group pair」とは | 同じ hit group の 2 段が持つ incoming ray payload 型を、**どの `OpTraceRayKHR` もその ray slot に届いていない段階で**先に一意な正準型として揃える(エンジン生成 any-hit の payload 省略のみ例外) |
| [`isTriangleHitAttribute()`](../../src/core/shader/raytracingvalidation.cpp#L108) | `vec2<f32>` 固定 | なぜ 2 成分か | Vulkan の組み込み三角形交差が重心座標 2 成分の 32bit float を渡す仕様だから |
| [恒真述語の既定構築](../../src/core/shader/raytracingoracle.cpp#L267) | 「常に true」を式で組む | 定数 true にしない理由 | 「値とその補数のビット和が全ビット 1」という形を各入力(primitive_id / instance_custom_index / record_u32)について組み、論理積するので、**既定でも全入力オペランドと bitwise / 比較 / 論理ノード経路が実際に評価される**(定数畳み込みで経路が消えない) |
| [fixture の `require()` 群](../../src/core/shader/raytracingoracle.cpp#L486) / [入射角](../../src/core/shader/raytracingoracle.cpp#L556) | 数値境界の検査 | 個々の境界値の由来 | 契約側の定数(上記 `RayTracingOracleContract`)に対応。**個別の値の導出は未執筆** |

### 9.23.6 shader / computetask

#### [`slot_index = group_index * ray_count + ray_index`](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L605) — 線形化の向き

**分からなくなること**: 2 次元表をどちら major で潰しているのか、その並びに意味があるのか。

**答え**: (hit group, ray type) の 2 次元表を **group major** で線形化したもので、各ヒットグループの ray type 分が連続ブロックになります。これは SBT のヒットレコード索引(`q × ray_type_count + ray type` — §9.23.5 の `rayTracingInstanceRecordOffset()`)と同じ major の取り方なので、そのまま SBT のスロット配置へ写せます(→ §9.23.5 の q・record offset)。入力表は [`hit_shaders[ray][group][...]` という 3 層ネストの入力](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L546)です。

#### [layer_count の自動展開](../../src/core/renderingpass/computetaskcontainer.cpp#L354) — 条件と目的

**分からなくなること**: 省略時と明示時で意味が変わるのはなぜか。

**答え**: 対象が 2D array 次元で port の view が `per_view` または `family_array` のときに限り、`layer_count == 1`(かつ `base_array_layer == 0`)を `physical_view_count` に展開します。目的は「1 レイヤだけ書けば family 全体を指す」省略記法を許しつつ、**明示指定は論理 view 数と完全一致することを強制する**ことです。descriptor 側の正規化は[第6章「`family_array` は物理 layout をまたいで 1 つの descriptor に正規化する」](06_rendering_vulkan_shader.md)が本体です。

#### [`sequential_layer`](../../src/core/renderingpass/computetaskcontainer.cpp#L1118) と [`shares_storage_layout`](../../src/core/renderingpass/computetaskcontainer.cpp#L1155)

**分からなくなること**: `base_array_layer += view_index` が要る構成と、sampled なのに `eGeneral` になる構成。

**答え**: `sequential_layer` は非 cube・非 layered で `sequential_2d` レイアウトの多 view リソース、つまり **view ごとに別 array layer を使う**構成なので、その view の層を選ぶ必要があります。`shares_storage_layout` は同一 render target が同じディスパッチ内で storage image としても束縛されている場合で、**1 つの image を 2 つのレイアウトに同時に置けない**ため sampled 側も `eGeneral` に揃えます。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`material_alpha_v1` の any_hit 自動生成](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L629) | 生成の条件 | なぜ勝手に足すのか | profile が要求するアルファ判定を全 group×ray に行き渡らせるため。authored な anyhit と衝突したら検出する(→ §9.23.5 の profile 制約) |
| [`traced_ray_type_indices` の構築](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L475) | 追加と重複チェック | 何の集合か | **未執筆**(根拠がコードにない) |
| [`physical_view_count` の決定](../../src/core/renderingpass/computetaskcontainer.cpp#L277) | 論理 view との関係 | 2 つの view 数の別 | **未執筆**(§9.23.4 の view family と対) |
| [sampled 判定](../../src/core/renderingpass/computetaskcontainer.cpp#L1090) | descriptor type の対応 | 対応表 | Vulkan の descriptor 種別の前提知識(→ [第10章](10_background_knowledge.md)) |
| [`next_extra_binding`](../../src/core/renderingpass/computetaskcontainer.cpp#L148) | 同一 resource に複数 port | binding の割当規則 | **未執筆**(根拠がコードにない) |
| [layered view の subresource range](../../src/core/renderingpass/computetaskcontainer.cpp#L1198) | mip の省略 | 既定値の規約 | **未執筆**(根拠がコードにない) |
| [`descriptor_variant_count`](../../src/core/renderingpass/computetaskcontainer.cpp#L1490) と [(view × frame) の事前生成](../../src/core/renderingpass/computetaskcontainer.cpp#L1261) | 2 次元の事前確保 | 2 軸の意味 | → §9.23.7「variant 次元と parity 次元」(同じ 2 軸) |

### 9.23.7 material

#### [`descriptor_abi_changed`](../../src/core/material/materialgenerationplanner.cpp#L1537) — 全 descriptor 破棄と revision

**分からなくなること**: なぜ `waitIdle()` してから捨てるのか、revision は誰が読むのか。

**答え**: `pass_inputs` / `resource_interface` が変わると既存の descriptor set はレイアウト・束縛内容と ABI 不一致になります。[commit 側](../../src/core/material/materialgenerationplanner.cpp#L1537)ではまず **`waitIdle()` して in-flight フレームが古い set を参照し終えるのを待ち**、その後 clear して `++descriptor_revision` します。revision は消費側キャッシュの世代無効化用です。同じ理由の待機が [live な `MaterialBuffer` への in-place 書き込み](../../src/core/material/materialcontainer.cpp#L1484)にもあります(こちらはバッファの差し替えではなく、コメントどおり「in-flight フレームが共有する live SSBO の読み手を守る」ための `waitIdle()`)。

#### [variant 次元と parity 次元](../../src/core/material/materialdescriptorbuilder.cpp#L441) — 2 軸が直交する

**分からなくなること**: 「variant」と「parity」がどちらも 2 値の添字に見えて区別できない。

**答え**: **variant は sequential なスクリーン入力の array layer**(view ごとの層。[全リソースの layer 数一致を要求](../../src/core/material/materialdescriptorbuilder.cpp#L599))、**parity はダブルバッファされた render target のどちらの実体を読むか**です。2 軸は直交するので (variant, parity) の全組み合わせ分の descriptor set を事前確保し、フレーム中は選ぶだけにします。

#### [`rayMaterialTableSource()` の canonical key 検証](../../src/core/material/materialcontainer.cpp#L1684) — `document:` / `asset:` プレフィックス

**分からなくなること**: プレフィックス付き文字列が何の識別なのか。

**答え**: §9.23.5 の `source_by_handle` / `source_by_key` の照合先で、プレフィックスは [`MaterialRowIdentity`](../../src/core/shader/raygrouplayout.hpp#L298) の 3 択(`MaterialRowDocumentIdentity` / `MaterialRowAssetIdentity` / `MaterialRowIdentityUnavailable`)に対応します(`MaterialRowDocumentIdentity` 自体は document 側の 1 状態にすぎません)。ゲートと lookup が同じ値を見る理由は[第11章](11_ray_query_acceleration_structures.md)が本体です。**プレフィックス文字列そのものの版管理規約は未執筆。**

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`ray_generation` / `ray_lease` の対](../../src/core/material/materialcontainer.cpp#L804) | 世代とリース | 2 つで何を保証するか | `ray_generation` は登録ごとに単調増加する数値 ID(枯渇時は例外)で同一 material id の再登録を区別し、`ray_lease`(型は `shared_ptr<const void>`、実体は `make_shared<const uint64_t>(ray_generation)`)は**その登録実体がまだ生きていることをポインタ同一性で証明**する。不一致なら [`pelican.ray_material_snapshot_stale@1`](../../src/core/material/materialcontainer.cpp#L1682) |
| [`input_attachment_index` と same_pixel](../../src/core/material/materialcontainer.cpp#L2107) | LogicalReadFootprint の same_pixel | なぜ同一ピクセルだけか | dynamic rendering local read の条件そのもの(→ §9.23.4 / [第6章](06_rendering_vulkan_shader.md))。詳細は**未執筆** |
| [pass variant 間の ABI 一致検証](../../src/core/material/materialgenerationplanner.cpp#L936) | 同じ shader input の検証 | 何を比べているか | **未執筆**(根拠がコードにない) |

### 9.23.8 vertex / model

#### [`++geometry_address_generation`(index 側)](../../src/core/model/vertbufcontainer.cpp#L339) — 再確保で世代を上げる理由

**分からなくなること**: バッファを太らせただけなのに、なぜ世代を進めるのか。

**答え**: **バッファを作り直すとデバイスアドレスが変わり、旧アドレス上に構築された BLAS が無効になる**からです。値は [`geometryAddressGeneration()`](../../src/core/model/vertbufcontainer.hpp#L118) として [renderer 側へ渡され](../../src/core/vkcore/renderer.cpp#L5299)、ray-query のジオメトリ / BLAS キャッシュ無効化キーになります。頂点側とインデックス側で[2 か所](../../src/core/model/vertbufcontainer.cpp#L376)進みます。

#### [`ReleaseReport`](../../src/core/model/modelresourcerelease.hpp#L36) — 固定容量 8 + overflow_count

**分からなくなること**: なぜ `vector` ではないのか。

**答え**: デストラクタや deferred 解放経路は `noexcept` で、そこで動的確保すると `bad_alloc` を投げうるため**報告自体が失敗します**。固定 8 要素のインライン配列なら確保も例外もなしに記録でき、溢れは件数だけ残します。同じ設計は §9.21「`ReleaseReport` は heap を使わない」でも扱っています。

#### [parse → validate → stage](../../src/core/asset/model.cpp#L657) — 3 段階と `sameRigLayout`

**分からなくなること**: どの段で GPU に触るのか、どの差分が `compatibility_revision` を進めるのか。

**答え**: parse は IO / デコードのみ、[`validate`](../../src/core/asset/model.cpp#L657) は `inspect` で **GPU コミットなしのプレビュー template** を作って `sameRigLayout` でリグ差分を判定、[`stage`](../../src/core/asset/model.cpp#L665) で初めて実 GPU コミットとリビジョン付与です。失敗しても live に触れずに巻き戻せます。rig_changed(joint_nodes・親子・skin_bindings のレイアウト一致)**だけ**が `compatibility_revision` を進めます。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [allocation id の払い出し](../../src/core/model/vertbufcontainer.cpp#L445) | 単調 ID | 再利用してはいけない理由 | 解放されたレンジは後続モデルに**同じ offset で再割り当てされうる**ので、offset を identity にすると古い BLAS キャッシュにエイリアスする。単調 ID(枯渇時は例外)なら「解放済みレンジが後のモデルのキャッシュ済み BLAS にエイリアスできない」ことが保証される |
| [`releaseGeometryNow()`](../../src/core/model/vertbufcontainer.cpp#L526) | `noexcept` な解放 | 例外を握って report に積む理由 | デストラクタ / DeletionQueue から呼ばれるので投げれば terminate。段ごとに try/catch して report に積むことで、metadata クリア失敗がそのレンジを保持し続けたり、**後続 primitive の独立したレンジ返却を妨げたり**しないようにしている |
| [morph metadata と delta](../../src/core/model/vertbufcontainer.cpp#L242) | 2 つの別バッファ | 住所空間の違い | metadata は vertex プール上の `vertex_offset + vertex` で直接索引される固定長配列(static/skinned で別)、delta は free-range で `delta_offset` を確保する共有プール。不変式は `metadata[vertex].delta_offset = delta_offset + vertex` と `delta[target*vertex_count + vertex]` |
| [`releaseRange()` の merge](../../src/core/model/vertbufcontainer.cpp#L113) | free range の併合 | `std::max` を使う理由 | 判定が `>=` で「隣接」だけでなく**重なり / 内包**も併合対象になるため、後続レンジが直前の併合済みレンジに完全に含まれる場合に末尾を縮めないよう両終端の max を取る |
| [`allocateRange()`](../../src/core/model/vertbufcontainer.cpp#L95) | free list + high water mark | 2 系統ある理由 | まず free list を first-fit で走査して解放済みレンジを再利用し(**リロード周回でバッファが際限なく伸びるのを防ぐ**)、合う穴が無いときだけ high water mark を進め、そこで初めて `ensure*Capacity` の再確保が起きる |
| [`content_revision` / `compatibility_revision`](../../src/core/model/modeltemplate.hpp#L159) | 2 本の版 | 分ける理由 | content は毎リロード必ず進む「中身が変わった」印、compatibility はリグレイアウトが変わったときだけ進む「バインドを張り直す必要がある」印。後者だけが staged resource の互換性キーとして公開されるので、**内容だけの更新で依存側の再解決を誘発しない** |
| [importer mapping の安定座標](../../src/core/model/modeltemplate.hpp#L83) | mesh/primitive/node index | 「regrouping を生き延びる」の意味 | ソース GLB 上の**安定座標**で、GPU 側の allocation offset とは意図的に分離されている。再グループ化で offset が変わっても、それらの座標で addressing されたインポータ由来のマテリアル割当はそのまま解決できる |
| [`publishBatch()`](../../src/core/asset/model.cpp#L459) / [`retireReloadPayload()`](../../src/core/asset/model.cpp#L826) | 公開と退役 | 逆にできない理由 | WP382c では renderer の rebuild を prepare し、participant の publish で template と instance を新世代へ張り替える。旧 payload は coordinator の退役経路から GPU resource の遅延解放へ渡す。旧 primitive レンジを公開前に解放すると、生きた draw の参照を壊しうる |
| [`DeferredRelease` の `acquire()`](../../src/core/model/vertbufcontainer.cpp#L588) | lease semantics | null のとき何が起きたか | §9.23.4 の `GpuSubmissionLease` と同じ(フェンス完了まで生かす券)。lease が null なら既に回収済み |
| [`deferOldBuffer()`](../../src/core/model/vertbufcontainer.cpp#L135) | 旧バッファの遅延破棄 | いつ drain されるか | DeletionQueue(submission fence 完了時)。→ §9.12 |
| [例外時の morph range](../../src/core/model/vertbufcontainer.cpp#L390) | 部分 cleanup | 解放されない範囲がある | **未執筆**(`vertex_allocated` フラグの cleanup 範囲を確かめる必要がある) |
| [morph metadata の空 vector 埋め](../../src/core/model/vertbufcontainer.cpp#L562) | ゼロクリア | なぜ空 vector か | **未執筆**(根拠がコードにない) |
| [`deferred_callbacks` の初期化位置](../../src/core/model/vertbufcontainer.cpp#L74) | メンバ初期化順 | なぜその位置か | **未執筆**(根拠がコードにない) |
| [`Impl` の所有関係](../../src/core/asset/model.cpp#L344) | PayloadTracker / CommitBatch / ModelPayload / PendingCandidate | 誰が誰を持つか | **未執筆**(3 段階 staging の実装詳細) |
| [`const void*` での payload 追跡](../../src/core/asset/model.cpp#L646) | 型を消した比較 | なぜ型を消すのか | **未執筆**(根拠がコードにない) |
| [`source_material_index` が optional](../../src/core/model/modeltemplate.hpp#L134) / [bounds](../../src/core/model/modeltemplate.hpp#L70) | 任意性と空間 | bounds は local か world か | **未執筆**。ただし「スキニングは後で現在のパレットから適用」とコメントされているので local space と読める |
| [material bindings のパス解決](../../src/core/asset/model.cpp#L161) | reference からの解決 | fragment を返しうるか | **未執筆**(根拠がコードにない) |

### 9.23.9 editor journal

#### [OriginTransitionMap の journal 側](../../src/core/communication/editorjournal.cpp#L1560) — forward / inverse の向き

**分からなくなること**: 同じ表を「展開」と「復元」の両方で使うとき、方向がどこで反転するのか。

**答え**: 表の意味論は §9.23.2 が本体です。反転は [`invertOriginTransitionMap()`](../../src/core/loader/resolvedscene.cpp#L684) が **entries はそのままに direction フラグだけ入れ替える**実装で、`unpack_prefab` = GeneratedToAuthored / `restore_prefab` = AuthoredToGenerated が [journal の整合検査](../../src/core/communication/editorjournalintegrity.cpp#L160)で強制されます。エントリは [`OriginTransitionEntry`](../../src/core/loader/resolvedscene.hpp#L124) の 5 フィールドです。

#### [`attachment_handle` / `attachment_seq` / witness](../../src/core/communication/editorjournalops.cpp#L351) — 3 識別子の主キー

**分からなくなること**: どれが主キーで、どれが照合用なのか。

**答え**: **`attachment_handle` が主キー相当**(runtime arena が採番する不透明な実体ハンドル。0 は無効)、`attachment_seq` は `(object_index<<32)|component_index` の**宣言位置由来の順序キー**、`attachment_identity`(witness)は object / component_index / behavior type / origin から作る**内容照合用の指紋**で、[resolver が返した runtime identity と食い違えば](../../src/core/communication/editorjournal.cpp#L1828) `journal_integrity_error` です。→ §9.22「identity / canonical key / lease の三層」。

#### [`sceneBehaviorAttachmentSeq()`](../../src/core/gamelogic/behaviorarena.cpp#L72) — 存在する関数

**分からなくなること**: journal 側から呼ばれているこの名前が、どこで定義されているのか。

**答え**: **behavior arena 側に実在します**(宣言は [`behaviorarena.hpp`](../../src/core/gamelogic/behaviorarena.hpp#L204))。`sceneBehaviorAttachmentSeq(object_index, component_index)` は両者が u32 に収まるか検査して上位 / 下位 32bit に詰めた 64bit を返すだけの関数で、journal は [1950 行付近](../../src/core/communication/editorjournal.cpp#L1819)でこれを呼びます。

#### [`structuralDomainsOverlap()`](../../src/core/communication/editorjournal.cpp#L2750) — kind が競合検出でどう使われるか

**分からなくなること**: `kind` 文字列が判定にどう効くのか。

**答え**: まず **object 集合・同一 scene 内の name_reservation・declaration_index の交差**で判定し、その後の追加規則として **`component_slot` と `value_field` の組み合わせに限り「同一 object かつ同一 slot」を衝突**とみなします(スロット追加 / 削除と同スロットの値編集を競合扱いにする)。他の kind(prefab_parameter / behavior_params / behavior_attachment / parent_edge / object_existence / object_subtree)は自分が載せた object / index / name フィールド経由で交差判定されます。

#### [パス重複と domain 重複](../../src/core/communication/editorjournal.cpp#L2679) — 両方が要る理由

**分からなくなること**: JSON パスが重ならなければ安全ではないのか。

**答え**: [第7章「曖昧な重なり判定」](07_tools_rpc_tests.md)が本体です。1 点だけ補うと、`stablePathsOverlap` は `/authoring_objects/7` と `/authoring_objects/7/...` のような**祖先・子孫関係しか見ない**ので、パス表現が異なるまま同一意味領域に触れる編集(名前予約・declaration_index・同一 slot への異種 op)を取りこぼします。だから [`recordOverlaps()`](../../src/core/communication/editorjournal.cpp#L2844) が両者を OR で結びます。

#### [`last_writers`](../../src/core/communication/editorjournal.cpp#L3311) — 「active writer history」の意味

**分からなくなること**: 履歴という名前なのに、undo で要素が消える理由。

**答え**: [コメント](../../src/core/communication/editorjournal.cpp#L3311)どおり**その write_set を現在保持している有効な書き手のスタック**であって、積まれた操作の履歴ではありません。よって undo は「取り消された書き手を除去する」操作なので [top を pop](../../src/core/communication/editorjournal.cpp#L3713) し、redo は[新しい transaction を top として push](../../src/core/communication/editorjournal.cpp#L3723) します。

#### [`final_authored_index` の連続性](../../src/core/communication/editorjournalintegrity.cpp#L211) — `max()` はスキップではない

**分からなくなること**: `numeric_limits<size_t>::max()` の明示チェックが「その値をスキップしてよい」と読めてしまう。

**答え**: **スキップは許されていません**。index は重複なしかつ直前 +1 の連続でなければ整合エラーです。`max()` のチェックは、連続性判定の `prev + 1U` が**ラップアラウンドして偽の一致を作る**のを防ぐガードで、**直前の要素が `max()` なら、その次の要素は無条件に整合エラー**になります(検査は `entries[index-1]` を見るので、`max()` が末尾に来た場合だけはこのガードに掛かりません)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [gate epoch と transition_epoch](../../src/core/communication/editorjournal.cpp#L3396) | 2 つのエポック | 違い | `transition_epoch` は**ゲート提供側(runtime の `EditorTransitionCounter`)が数える外部の状態遷移カウンタ**、[`gate_tracker`](../../src/core/communication/editorjournal.cpp#L3318)(`EditorGateEpoch`)の epoch はその観測が変わるたびに進む**内部の単調エポック**。受理時に epoch を控え、[commit 時に食い違えば](../../src/core/communication/editorjournal.cpp#L4101)拒否する。transition の変化は必ず epoch を進めるので、比べるのは epoch だけでよい |
| [`requireRevertPreconditions()`](../../src/core/communication/editorjournal.cpp#L3539) | undo 前の 3 段検証 | 何を見ているか | → [第7章「undo前提の三段検証」](07_tools_rpc_tests.md) |
| [`Impl::preview` の reservation と lease](../../src/core/communication/editorjournal.cpp#L3289) | 予約と実効リース | 2 つある理由 | reservation は `open_preview` を受理した瞬間(まだ frame boundary 前)の**先行予約**、lease はプレビュー適用後の**実効リース**。[`requireNoLeaseOverlap()`](../../src/core/communication/editorjournal.cpp#L3518) が両者と新規編集の重複を見て `preview_lease_conflict`。状態機械は[第7章「preview leaseの状態機械」](07_tools_rpc_tests.md) |
| [unpack の単独 op 強制](../../src/core/communication/editorjournal.cpp#L2043) | batch の形の制限 | なぜ 1 command か | unpack は object subtree 全体を作り替え、その inverse を `origin_transition` 込みの forward/inverse ペアとして**受理時点で確定**させる必要があるため。他 op と混ぜるとこの単一ペアの整合検査が成立しない |
| [`prefabGeneratedId()`](../../src/project/prefab.cpp#L1060) | gid の生成 | アルゴリズム | ドメイン分離タグ `"pelican.prefab.gid.v1"` / `instance_id` / `component_key` を**長さ前置で連結**したバイト列の SHA-256 の先頭 16 バイトを hex 化し `gid_` を前置する([宣言側コメント](../../src/project/prefab.hpp#L224)= WP361 v4)。component_key は `component.id`、無ければ `component.name` |
| [`canonicalBehaviorComponent()`](../../src/core/communication/editorjournal.cpp#L260) | behavior だけ codec を通さない | 理由 | behavior attachment は登録済み codec を持つ通常 component ではなく `{name,type,params}` の固定形で、**型登録が DLL 由来で欠けている可能性がある**。未登録でも情報を落とさず保持し、「レジストリが生きている(名前拒否)」と「DLL 不在(pending attachment)」をコミット境界で区別するため |
| [子孫の降順ソート](../../src/core/communication/editorjournalops.cpp#L1134) | prepareDestroy の並べ替え | 降順の理由 | 子孫から順に remove を積む必要があり、降順で削除すれば**削除に伴う後続要素のインデックスずれが未処理分に波及しない**。同時に inverse(restore_objects)が逆順で元の宣言位置に挿し戻せる |
| [`field_path` の `/name` 禁止](../../src/core/communication/editorjournalops.cpp#L659) | 値編集の対象制限 | なぜ name だけ禁止か | `name` は **component のスロット同一性(と object 名前予約)を決める識別子**なので、値編集経路で書き換えられると slot / domain 計算が破綻する。detail も "field_path must be a JSON pointer and cannot change name" と明言 |
| [`prefabGeneratedMarkerRule`](../../src/core/communication/editorjournal.cpp#L1515) | マーカー削除と body 検証 | 何を消すのか | 上記 gid / OriginTransitionMap と一体。詳細は**未執筆** |
| [unpack の forward/inverse 整合](../../src/core/communication/editorjournal.cpp#L1585) | commit 時の structural_changes | 何を突き合わせるか | **未執筆**(根拠がコードにない) |
| [`behaviorAttachmentWitness()`](../../src/core/communication/editorjournal.cpp#L564) | witness の組み立て | generated と authored の分岐 | witness 自体は常に 4 フィールド(`authoring_object_id` / `authored_component_occurrence` / `type` / `origin`)。分岐するのは [`origin` の中身](../../src/core/communication/editorjournal.cpp#L538)で、authored は 2 フィールド(`kind` / `authoring_component_index`)、generated は 6 フィールド(`kind` / `stable_generated_id` / `prefab` / `instance_id` / `component_key` / `digest_sha256`) |
| [`expanded_components`](../../src/core/communication/editorjournalintegrity.cpp#L23) と [末尾からの削除](../../src/core/communication/editorjournal.cpp#L1449) | 展開区間の位置 | 逆操作が末尾削除でよい理由 | unpack は authored 配列の**末尾に展開区間を置く**ので、逆操作は末尾から authority.size() 分を削るだけでよい(→ final_authored_index の連続性) |
| [behavior edit の attachment_index](../../src/core/communication/editorjournalops.cpp#L72) | 必須引数 | なぜ必須か | 同一 object に複数の behavior が付くので index なしでは対象が決まらない(→ §9.23.2 の「behavior だけ重複可」) |
| [`closureJson()`](../../src/core/communication/editorjournal.cpp#L378) | previous/next による linked-list | 宣言順の復元 | → [第3章「closureがidentityを運ぶ」](03_project_and_loading.md) |
| [`resolvePrefabInstance` の context 引数](../../src/core/communication/editorjournal.cpp#L1238) | scene_id / parameter / json_pointer | 何を運ぶか | **未執筆**(根拠がコードにない) |
| [replay 時の witness 再検証](../../src/core/communication/editorjournal.cpp#L1905) | rebind | undo/redo で張り替わる理由 | **未執筆**(根拠がコードにない) |
| [`projection_state->revision() == source.revision()`](../../src/core/communication/editorjournal.cpp#L2055) | incremental update | 判定の意味 | **未執筆**(根拠がコードにない) |
| [`ReparentPreserve`](../../src/core/communication/editorjournalops.cpp#L1189) | Local / World | transform 計算への影響 | → [第3章「preserve-worldの逆算」](03_project_and_loading.md) |
| [behavior canonical 化の `old_component` 保持](../../src/core/communication/editorjournalops.cpp#L276) | retain する理由 | 何のために残すか | コメントどおり「`BehaviorParamsPolicy` の既定値は編集可能な authored view の一部なので、field pointer を解決する前に一時オブジェクトへ展開する。その間 `old_component` は inverse / raw 復元のために**逐語のまま**保持する」 |
| [`domainObjects` / `domainNames` / `domainIndices` の定義](../../src/core/communication/editorjournal.cpp#L2688) ほか(prefab_cas フラグ、generated_id と component_slot の排他、`operationsUsePrefabParameters`、`forceAbort` と preview_boundary、`normalizeBehaviorAttachmentIdentities`、`batchWriteSet` の集約、`coalesce_key`、`finalizeRevertJournal` の逆順再構築) | 競合判定の下請けと journal の細部 | 個々の規則 | **未執筆**(いずれも根拠がコードにない。エラーコードの `@1` は §9.23.18 の契約バージョン) |

### 9.23.10 editor command service

#### [5 つの PrefabError を dependency_failure に分類](../../src/core/communication/editorcommandservice.cpp#L982) — 分類の基準

**分からなくなること**: 同じ `PrefabError` がどうして 2 つの外部エラーに分かれるのか。

**答え**: コメントどおり**「欠けている入力が受け側プロジェクトに属するか」**です。PrefabNotFound / PrefabBindingNotBindable / PrefabBindingInactive / PrefabProviderStale / PrefabDependencyMismatch は受信側の registry・provider・closure が揃っていないことを示すので `PrefabDependencyMismatch`、それ以外の prefab 意味検証失敗は payload 側の違反なので `SnapshotInvalid` に留めます。detail が [`/prefab_closure`](../../src/core/communication/editorcommandservice.cpp#L993) になるのも「欠けている入力が closure だから」です。

#### [`trackPreviewTransition()`](../../src/core/communication/editorcommandservice.cpp#L1565) — PreviewWatch の erase 条件

**分からなくなること**: watch がいつ消えるのか(成功時なのか失敗時なのか)。

**答え**: `trackPreviewTransition` は応答が `status=="accepted"` のときだけ pending に積み、`synchronizePreviewWatch` が `getPreviewResult` を polling して open → lease 生成、updated → state 更新、commit/abort → lease 破棄と遷移させます。**erase は「status が accepted 以外になった」時点で一律**(kind 不一致も含む)で、加えて `hasOpenPreviewLease()` が false なら lease をリセットします。journal 側の状態機械は[第7章「preview leaseの状態機械」](07_tools_rpc_tests.md)。

#### [generated component の出力](../../src/core/communication/editorcommandservice.cpp#L692) — editable 判定

**分からなくなること**: 同じ component なのに、返る JSON の形が違う理由。

**答え**: generated component は prefab 解決の産物で**authoring 側に対応する編集可能ソースが無い**ため、`resolved_json` + `editable=false` + schema missing を返します。behavior が editable になる条件は[直後の分岐](../../src/core/communication/editorcommandservice.cpp#L700)で、非 generated かつ behavior 登録が存在し、pending でなく、`behavior_canonical_params` が取れていること(このとき codec は `behavior_params`、schema は registration の params_schema)。出力の組み立ては [`editorComponentQueryJson()`](../../src/core/communication/editorcommandservice.cpp#L1624)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`parsePrefabClosure()`](../../src/core/communication/editorcommandservice.cpp#L262) | closure を strict sorted 順に強制 | 順序を縛る理由 | closure は fingerprint / digest と突き合わせる**正準表現**なので、順序の自由度を残すと同一内容が別バイト列になり closure 一致判定が非決定的になる |
| [版を最初に断る行](../../src/core/communication/editorcommandservice.cpp#L431) / [gate 3](../../src/core/communication/editorcommandservice.cpp#L449) / [SNAPSHOT0 のコメント](../../src/core/communication/editorcommandservice.cpp#L476) | 5 ゲートの評価順 | 順序そのものが契約である理由 | **SNAPSHOT0 が定めた 5 ゲートの評価順(version → size → digest → parse/semantic → closure)がワイヤ契約**で、どの入力不正がどのコードで返るかの優先順位を版間で固定する。「digest の algorithm 検査は gate 3 であってリクエスト形状検査ではない」ため前倒しできない。MG-2b で SnapshotV1 の parser が消え、`schema_version` が 2 以外の要求は他の鍵を見る前に [版を断る関数 `rejectSnapshotVersion`](../../src/core/communication/editorcommandservice.cpp#L210) で `unsupported_snapshot_version` になる |
| [`parseGetComponentsRequest()`](../../src/core/communication/editorcommandservice.cpp#L168) | id と name の排他 | なぜちょうど一方か | 宣言 id と名前は**独立な参照系**で、両方指定は互いに矛盾しうる(片方が別オブジェクトを指す)、両方省略は対象未定。`has_value() == has_value()` で強制 |
| [`SceneRevision{1}` のダミー](../../src/core/communication/editorcommandservice.cpp#L976) | import 検証用の使い捨て文書 | なぜ固定値でよいか | ここで作る `validated` は**ゲート 4 専用で公開されない**ため。実際に公開される revision は `import_scene_snapshot(...)` が返す `imported.revision` |
| [`ResolvedSceneResolver::resolve()` の 5 引数](../../src/core/communication/editorcommandservice.cpp#L978) | defaults / registry / provider を別に渡す | 分ける理由 | **未執筆**(§9.23.2 の resolver 入力 fingerprint と対) |
| [runtime state と authored state の融合](../../src/core/communication/editorcommandservice.cpp#L650) | 2 つの状態の突き合わせ | size mismatch が `logic_error` な理由 | **未執筆**(runtime と authored の component 数は一致するのが不変式、という以上の根拠がコードにない) |
| [`closureMismatchDetail()`](../../src/core/communication/editorcommandservice.cpp#L368) | 最初のズレで停止 | なぜ全部見ないか | **未執筆**(エラー detail の決定性を狙ったものと読めるが、根拠となる記述がない) |
| [`render-config-edit-` プレフィックスの判定](../../src/core/communication/editorcommandservice.cpp#L1492) | ticket ルーティング | render config編集の共通ticketを既存結果照会へ送る | featureと名前付きASの編集が共用 |
| [revision token が `edit_` の preview_epoch を吸収する行](../../src/core/communication/editorcommandservice.cpp#L769) | `edit_` の未確定分 | なぜ吸収するのか | **未執筆**(根拠がコードにない) |
| [error detail のパス手組み(`root + "/scenes/" + index`)](../../src/core/communication/editorcommandservice.cpp#L287) | index 入り JSON pointer 風 | 手で組む理由 | **未執筆**(根拠がコードにない) |
| [`findScene()` の直後](../../src/core/communication/editorcommandservice.cpp#L944) | scene 存在検査 | `selectScene()` も呼ぶのか | **呼びません**。ここは `findScene(current_scene) == nullptr` で `SceneNotFound` を投げるだけの存在検査で、選択の副作用はありません |
| [import reload 後の再 catch](../../src/core/communication/editorcommandservice.cpp#L1073) | `PrefabError` の再捕捉 | 二重に捕まえる理由 | **未執筆**(根拠がコードにない) |
| [`ordered_json` の扱い](../../src/core/communication/editorcommandservice.cpp#L593) | キー順序の保存 | いつ順序が意味を持つか | JSON ライブラリの前提知識(→ [第10章](10_background_knowledge.md))。使い分けの実例は §9.23.11 の `resolution_error` 行 |

### 9.23.11 RPC

#### [`gameLogicParticipantRpcStatus()` による participant 照会](../../src/core/communication/rpcserver.cpp#L1299) — 参加者名の正

**分からなくなること**: 参加者名の文字列が何なのか(`"game_logic_reload"` に見える資料がある)。

**答え**: 定数は [`watch::gameLogicReloadParticipantName = "pelican.game_logic"`](../../src/core/watch/reloadservice.hpp#L47) です。ReloadService が未登録なら [`registerParticipant` で自己登録](../../src/core/watch/reloadservice.cpp#L741)し(frame_start 境界の RuntimeReloadParticipant)、RPC は [`statusJson()["runtime"][participant]`](../../src/core/communication/rpcserver.cpp#L1306) で照会、`applyRuntimeNow(participant)` で明示適用します。

#### [`SceneLoadResult` の受け取り](../../src/core/communication/rpcserver.cpp#L1959) — 状態機械

**分からなくなること**: 「失敗したのに committed:true」が起きる理由。

**答え**: 型は [`variant<SceneLoadCommitted, PreCommitFailure, CommittedWithEffectFailure>`](../../src/core/loader/sceneload.hpp#L188) です。`load()` 自体は 3 状態を**戻り値で**返し、RPC の入口が通す [`requireSceneLoadCommitted()`](../../src/core/loader/scene.cpp#L500) が `PreCommitFailure` だけを**例外に変換して再送出**します(従来の throwing 契約)。この経路ではシーンは差し替わりません。`CommittedWithEffectFailure` は「公開自体は成功したが個別 effect が失敗した」状態で、`load_scene` は `committed:true` のまま `effect_errors` を添えて返します(→ §9.23.1 の destroy フェーズ)。

#### [`parseGizmoSelection()`](../../src/core/communication/rpcserver.cpp#L529) — 2 つの identity を受ける理由

**分からなくなること**: なぜギズモの選択だけ 2 形式あるのか。

**答え**: declaration identity(`scene_id` + `declaration_index`)は**永続する authoring 上の宣言位置**、runtime identity(`object_id{index, generation}`)は**生存中のエンティティの世代付きハンドル**で、ギズモはシーン再ロード前後どちらの文脈からも指定されうるため両方を受けます。混在は `gizmo_selection_mixed_identity` で拒否し、解決系を一意にします。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [リロード中の一律拒否](../../src/core/communication/rpcserver.cpp#L1508) | `isGameLogicReloadInProgress()` | なぜ全メソッドか | DLL のシンボル・system 登録・behavior 実体が**差し替え途中**で、この状態でハンドラを走らせると無効化されたコードやハンドルを触るため。拒否は transport の中ではなく、[`configureEngineRpcHandlers()`](../../src/core/communication/rpcserver.cpp#L1496) が [`RpcServer::setRequestGate()`](../../src/core/communication/rpcserver.cpp#L1168) で差し込む述語になり、`RpcServer` は [parse 後・dispatch 前に gate を 1 回引く](../../src/core/communication/rpcserver.cpp#L1183)だけです |
| [before_apply / after_apply フックの登録](../../src/core/communication/rpcserver.cpp#L1516) | リロードの前後フック | 2 つで何を挟むか | `before_apply` でプレビューリースを強制中断して未確定編集をコミット済み状態に戻し、`after_apply` で `pending_transforms.clear()` により**旧 DLL 世代のエンティティを指す未フラッシュ transform を破棄**する |
| [`"resolution_error"` の再帰探索](../../src/core/communication/editorrpchandlers.cpp#L50) | 結果 JSON の走査 | 何を見ているか | どこかに `"resolution_error"` キーがあるかだけを見る。**含まれる場合はキー順序が意味を持つ**ため `nlohmann::json` への変換(順序喪失)を避け、`ordered_json` のまま手組みで dump する分岐に入る。走査は `RpcServer` から editor 側へ移り、`embedsResolutionError` は [`setOrderedHandler()` に渡す順序ポリシー](../../src/core/communication/editorrpchandlers.cpp#L85)として登録されます。transport は [ポリシーが真を返した応答だけ](../../src/core/communication/rpcserver.cpp#L1200)を ordered dump に回し、キー名は知りません |
| [`isR7Identifier()`](../../src/core/communication/rpcserver.cpp#L273) | "R7" の由来 | R7 とは何か | **外部ツール契約の要求番号 R7**(`docs/external_tools_requirements.md`)「オブジェクト / ジョイント名は `[a-zA-Z0-9_]` のみ」の実装。`docs/design_scene_format.md` と `docs/manual/04_scene_ecs.md` も同じ R7 を参照 |
| [`RpcServer::run()` の CRLF 除去](../../src/core/communication/rpcserver.cpp#L1241) | framing 契約 | なぜ `\r` を落とすか | 「1 行 = 1 JSON-RPC メッセージ、区切りは `\n`」で、CRLF 出力の相手から来た行末 `\r` を落とすことで Windows クライアントでも同じ契約が成立する。応答は常に `'\n'` 終端 + flush |
| [逆引き](../../src/core/communication/rpcserver.cpp#L426) / [名前からの変換](../../src/core/communication/rpcserver.cpp#L758) | model_instance → object_id → authoring_object_id | 橋渡しの規則 | 上記 2 つの identity の橋渡し。詳細は**未執筆** |
| [`ResolutionFailureException`](../../src/core/loader/resolutionresult.hpp#L176) | `failure()` の中身 | 何を表すか | **未執筆**(§9.23.2 の resolver 失敗形式と対) |
| [`kbd:` プレフィックス](../../src/core/communication/rpcserver.cpp#L649) | binding 表示 | modal transform との関係 | **未執筆**(根拠がコードにない) |
| [OpenXR inactive スキーマ](../../src/core/communication/rpcserver.cpp#L99) / [診断](../../src/core/communication/rpcserver.cpp#L1596) | 診断 JSON | スキーマの安定性契約 | **未執筆**(→ §9.23.18) |
| [frame index と continuous time](../../src/core/communication/rpcserver.cpp#L936) / [replay 開始](../../src/core/communication/rpcserver.cpp#L1889) | どちらが真か、設定順序 | 真の時刻源 | **未執筆**(根拠がコードにない) |

### 9.23.12 reload / watch

#### [`ReloadService::registerParticipant()`](../../src/core/watch/reloadservice.cpp#L136) — 型が言えないことだけを検査する

**分からなくなること**: 「両方あると不正」という検査がどこへ行ったのか。

**答え**: 適用ルートの排他は検査ではなく型になりました。[`FileReloadRole::apply`](../../src/core/watch/reloadservice.hpp#L164) は [`PerRequestReloadApply`](../../src/core/watch/reloadservice.hpp#L128)(1 件ずつ)/ [`BatchReloadApply`](../../src/core/watch/reloadservice.hpp#L136)(まとめて)/ [`CompanionBatchReloadApply`](../../src/core/watch/reloadservice.hpp#L147)(他 participant の request を吸い上げて 1 世代で publish)の `std::variant` で、二つ持つこと自体が書けません。ここに残っているのは**型が言えないことだけ**です — 名前が空でないこと、選ばれたルートの callback が null でないこと、companion apply なら companion 名が 1 つ以上あり空・自己参照・重複でないこと、そして名前が一意であること。batch owner 同士の順序も宣言から導かれます: ある owner が別の owner の companion に名指されていれば、名指した側の**後**に適用されるので、どちらが先に登録されたかで外側のトランザクションが companion を失うことがありません([宣言のコメント](../../src/core/watch/reloadservice.hpp#L144))。

#### [`applyRenderPipelineCompanionReload()`](../../src/core/watch/reloadservice.cpp#L420) — 流れと早期 return

**分からなくなること**: 何もせずに戻る条件が読み取りづらい。

**答え**: companion リクエストを shader キーと material ドキュメントに仕分け → `materials->prepareRuntimeGenerationReload()` で material_plan を作り、その `shader_overrides` 込みで `library->prepareReload()` → material commit と generation commit を 1 つの `coordinated_commit` に合成 → `pipelines->rebuildPrepared()` で一括コミット、という流れです。早期 return は「prepared 空 かつ pipeline_overrides 空 かつ coordinated_commit なし」= **実質変更なし**のときです。swap の三重構造は[第6章「reload の swap は 3 回」](06_rendering_vulkan_shader.md)が本体です。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`applyRuntimeBoundary()`](../../src/core/watch/reloadservice.cpp#L262) | frame_start と render_start | 境界の使い分け | 境界ごとに参加者を絞って実行する二段構え。**ゲームロジック DLL のような論理状態の入れ替えは frame_start、シェーダ / パイプライン再構築は描画直前の render_start** |
| [`makeLogicalAssetId()`](../../src/core/watch/reloadtransaction.cpp#L19) | generation を上位 32bit・index を下位 32bit に詰めた 64bit | stale 検出の仕組み | [`liveRecord()`](../../src/core/watch/reloadtransaction.cpp#L77) は index でスロットを引いた後 generation 不一致なら stale。`destroyResource` がスロットを free list へ返し再利用時に `++generation` するので、古いハンドルは同じ index を指しても必ず検出される(飽和時は例外)。→ §9.22 |
| [`eraseEdges()`](../../src/core/watch/reloadtransaction.cpp#L90) と逆引き map | `AssetKey -> set<LogicalResourceRef>` | 何に使うのか | 依存元アセットから「それに依存している論理リソース」を引く。**ファイル変更時に再ロード対象を特定するため**で、publish/destroy のたびに同期し、空集合はエントリごと削除する |
| [`topologicalOrder()`](../../src/core/watch/reloadtransaction.cpp#L105) | DAG の必須性 | 循環したらどうなるか | 1 グループ内の actor は `after` で「先にコミットすべき対象」を宣言し、Kahn 法で処理順を決める。循環があると順序が決まらず `"reload transaction dependency cycle"`(重複ターゲットも先に弾く) |
| [`ResourceRegistry::publish()`](../../src/core/watch/reloadtransaction.cpp#L247) | revision と retire | 破棄のタイミング | content は publish のたびに +1、compatibility は互換性 / レイアウト / リグの変更を表し、stale handle 判定に使う slot generation とは**意図的に独立**。retire は旧 payload の `shared_ptr` を集めて返し、呼び出し側が公開後に受け取って GPU の DeletionQueue へ遅延破棄する(→ §9.23.8 の rebuild → release と同じ順序原則) |
| [thread_local な companion observer](../../src/core/watch/reloadservice.cpp#L23) | 入れ子と並行の安全化 | なぜ thread_local か | **未執筆**(コメントが無く、「ctor で退避・dtor で復元」以上の根拠がコードにない) |
| [built-in participant の lazy 登録](../../src/core/watch/reloadservice.cpp#L633) | 4 つの既定参加者 | ポインタ比較で更新判定する理由 | **未執筆**(根拠がコードにない) |
| [`applyRequests()`](../../src/core/watch/reloadservice.cpp#L1111) | claimants / owned / companion / consumed | 4 つの集合の役割 | **未執筆**(根拠がコードにない) |
| [`applyShaderReloadBatch()`](../../src/core/watch/reloadservice.cpp#L358) | prepared 空での早期 error | cache_hits/misses の意味 | **未執筆**(根拠がコードにない) |
| [authored candidate の completion callback](../../src/core/watch/reloadservice.cpp#L295) | 完了通知 | 誰が呼ぶか | **未執筆**(根拠がコードにない) |
| [`applyRequest()`](../../src/core/watch/reloadservice.cpp#L919) | enqueue false と failed count | 同期の機構 | **未執筆**(根拠がコードにない) |
| [`runtimeStatusJson()`](../../src/core/watch/reloadservice.cpp#L1312) | describe callback | 誰が読むか | §9.23.11 の participant 照会が読む先。詳細は**未執筆** |

### 9.23.13 vkcore

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`createLogicalDevice()` の StructureChain](../../src/core/vkcore/core.cpp#L562) と [`unlink<T>()` の呼び出し](../../src/core/vkcore/core.cpp#L722) | feature 構造体の動的削除 | 削除の判定基準 | 全 feature 構造体を一度チェーンに組み、実行時に**「その機能が物理デバイスでサポートされ、かつ有効化選択されたか」**で使わないものだけ `unlink<T>()` する。未サポート機能の構造体を渡して device 作成が失敗するのを避けるため |
| [`pickQueues()`](../../src/core/vkcore/core.cpp#L228) | フォールバック優先順位 | 順序の理由 | 非 headless では ①graphics+present+compute を満たす単一 family、②graphics+present、③present だけ、の順。**1 family で済めばキュー跨ぎの所有権移転やセマフォ同期が不要**なので最優先 |
| [`pickPhysicalDevice()` の `score += 100`](../../src/core/vkcore/core.cpp#L541) | スコアの加点 | 100 という値の根拠 | headless か `graphic_queue == presentation_queue` のとき +100。同一 family なら swapchain image のキューファミリ所有権移転が要らず有利だから優先する(必須要件は score ではなく `continue` で除外)。**100 という数値そのものの根拠は未執筆** |
| [`VulkanManageCore()` の関数ポインタ初期化](../../src/core/vkcore/core.cpp#L925) | 拡張関数の条件付き解決 | null チェック後に throw する理由 | Vulkan 拡張の関数ポインタ解決という前提知識(→ [第10章](10_background_knowledge.md)) |

### 9.23.14 swapchain / OpenXR

#### [`replacement_anchor` の保存](../../src/core/vkcore/swapchainframetarget.cpp#L1146) — 成功時に退避する理由

**分からなくなること**: 作成に成功したのに、なぜ古いハンドルを持ち続けるのか。

**答え**: [コメント](../../src/core/vkcore/swapchainframetarget.cpp#L1146)どおり **`vkCreateSwapchainKHR` が成功した時点で oldSwapchain は即退役する**ので、その後の依存オブジェクト生成が失敗すると「まだ退役していない有効な swapchain」が 1 つも残らなくなります。だから新ハンドルを依存リソース作成前に退避し、リトライ時の oldSwapchain アンカーとして使います。

#### [`releaseAbandonedImage()`](../../src/core/vkcore/swapchainframetarget.cpp#L1293) — `vkReleaseSwapchainImagesEXT` をいつ呼ぶか

**分からなくなること**: 呼ぶ条件と対象がいずれも狭いこと。

**答え**: maintenance1 が有効で、旧 epoch に「acquire 済みだが present せず放棄した image」(`request.release_image_index`、pending_abandonment 由来)がある場合**のみ**、その 1 枚に対して cutover 直前に呼びます。失敗は警告ログのみです。

#### [present 完了追跡の 2 系統](../../src/core/vkcore/swapchainframetarget.cpp#L556) — 証明と推論

**分からなくなること**: maintenance1 の有無で「完了した」の意味が変わること。

**答え**: maintenance1 があるときは image ごとの present fence(+ 再利用時に退避した `superseded_present_fences`)で**厳密に証明**します。無い場合は `BasePresentRetirementTracker` が「後続 epoch が同 image を reacquire した」ことを watermark として記録し([`noteAcquired()`](../../src/core/vkcore/swapchainframetarget.cpp#L608) → `noteSuccessorImageReacquired`)、それ以前の epoch は present 完了と**推論**します。WSI の戻り値の読み方は[第6章「WSI の回復は『どこで返ったか』で意味が変わる」](06_rendering_vulkan_shader.md)。

#### [`surfaceLossRequiresDeviceRebuild()`](../../src/core/vkcore/swapchainframetarget.cpp#L1540) — "unproven presentation"

**分からなくなること**: surface lost で device ごと作り直す条件。

**答え**: [実装](../../src/core/vkcore/swapchainrecovery.cpp#L36)は `!exact_present_retirement && has_unproven_present` の 1 行なので、true になるのは **maintenance1 が無く、かつ present 完了を証明できていない epoch が残っているとき**だけです。base Vulkan には surface lost 後の present 完了プリミティブが無く、退役できない swapchain がネイティブウィンドウを掴み続けて `NATIVE_WINDOW_IN_USE` の無限リトライになるため、device 再構築を要求します。"unproven presentation" とは `mayRetire()` が false のまま(= 後続 reacquire による証明が得られていない)present 済み epoch のことです。

#### [2 種類の image view](../../src/core/openxr/openxrcompositiongraphics.cpp#L115) — 2D Array と層ごとの 2D

**分からなくなること**: 同じ image に 2 通りの view を作る理由。

**答え**: **multiview 一括描画([`array_layers == xr_stereo_view_count`](../../src/core/openxr/openxrcompositiongraphics.cpp#L247))では layered view を attachment に使い、片目ずつ順次描画するときは `layer_views[base_array_layer]` を使う**ためです。両経路を同じ swapchain image 上で切り替えられるようにしてあります。

#### [`Window::process()`](../../src/core/os/window.cpp#L374) — close flag が立たないまま窓が消える

**分からなくなること**: close を待つだけの main loop に、なぜ窓の存在確認があるのか。

**答え**: Studio に埋め込まれた player の窓は Studio の窓の子で、Studio が先に親を壊すと子も一緒に消えます。Studio が送った `WM_CLOSE` が届く前に窓が消えると GLFW は close flag を立てないので、`glfwWindowShouldClose()` だけを見る loop は回り続け、swapchain が消えた窓に surface を毎 frame 作り直そうとします(`ErrorSurfaceLostKHR` が数十行出て、Studio に kill されるまで続いた)。Windows では `IsWindow()` で窓の実在も見て、無ければ close と同じく loop を終えます。配布確認([`run_distribution_check.py`](../../test/ci/run_distribution_check.py#L180))は Studio 終了時に生きていた player の log で surface lost の数を数え、3 以下であることを確かめます(外すと 168〜172)。

#### [depth だけ `eTransferDstOptimal` へ遷移させる barrier](../../src/core/openxr/openxrcompositiongraphics.cpp#L209) — color と違う遷移先

**分からなくなること**: depth attachment なのに transfer レイアウトへ遷移する理由。

**答え**: depth は色と違い**レンダーパスの出力ではなく、エンジン側の depth を外部からコピーして XR に渡す**運用だからです。`eUndefined` → `eTransferDstOptimal` へ遷移させ、dstAccess を `eTransferWrite` にしています。

#### [depth extension の 4 値を書く行](../../src/core/openxr/openxrcompositiontarget.cpp#L915) — minDepth/maxDepth と nearZ/farZ

**分からなくなること**: 深度の範囲を表す値が 2 組あること。

**答え**: `minDepth` / `maxDepth` は depth image に書かれている**深度値そのものの値域**(ここでは 0.0〜1.0 の Vulkan 標準)、`nearZ` / `farZ` はその正規化深度が対応する**実際のビュー空間距離**(`new_near_z > 0 && new_far_z > new_near_z` で検証済み)です。ランタイムはこの 4 値で深度を線形化して reprojection に使います。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`pollRetiredEpochs()`](../../src/core/vkcore/swapchainframetarget.cpp#L1641) | quarantine と retire の分岐 | 隔離される条件 | GPU 完了済みだが present 未証明で surface が lost の場合、maintenance1 ありなら present fence が lost 後も完了を証明できるのでポーリング続行。無い場合は**「後続 reacquire は同一 surface 内でしか意味を持たない」ため証明が永久に得られず**、隔離して device teardown まで保持しつつリストからは外す |
| [`SwapchainEpoch` のメンバー宣言順](../../src/core/vkcore/swapchainframetarget.cpp#L490) | 逆順破棄の設計 | 宣言順が仕様である理由 | (a) `surface_epoch` リースを先頭に置いて全 swapchain 子オブジェクトが `VkSurfaceKHR` より先に壊れるようにし、(b) command_pool を command_buffers より前、(c) swapchain を image view / semaphore / fence より前に置く。**Vulkan の親子生存期間規則を宣言順だけで自動的に守る** |
| [present 後の注入](../../src/core/vkcore/swapchainframetarget.cpp#L2606) | 戻り値だけ差し替える | 本物を呼ぶ理由 | present は**エラーを返してもセマフォ待ちを実際にキューへ積み**、maintenance1 の present fence も既にシグナル対象になっている。本番と同じリソース lifetime 契約を保ったまま故障を注入するため。→ §9.22「fault injection seam」 |
| [`recovery_anchor` へ candidate を移す行](../../src/core/vkcore/swapchainframetarget.cpp#L1767) / [anchor 選択](../../src/core/vkcore/swapchainframetarget.cpp#L1797) | framebuffer.revision 不一致 | 捨てずに回す理由 | candidate は完成しているが framebuffer が変わって陳腐化した場合、破棄すると**「非退役 swapchain が 2 つ」になる / ウィンドウが掴まれたままになる**。candidate を `recovery_anchor` に移し、次の再構築の oldSwapchain として再利用しつつリトライを予約する |
| [format スコアリング](../../src/core/vkcore/swapchainframetarget.cpp#L264) | 20 / 10 / 0 の重み | 数値の根拠 | **未執筆**(スコア値の根拠コメントが無い)。実装は sRGB nonlinear の SRGB を 20、同 colorSpace の UNORM を 10、他 0 で `stable_sort` し、present mode も mailbox を先頭に安定ソートするだけ |
| [`chooseSwapchainImageCount()`](../../src/core/vkcore/swapchainframetarget.cpp#L176) / [composite alpha](../../src/core/vkcore/swapchainframetarget.cpp#L205) / [preTransform 固定](../../src/core/vkcore/swapchainframetarget.cpp#L339) | swapchain 作成パラメータ | ヒューリスティックの根拠 | image count は Vulkan の一般則(→ [第10章](10_background_knowledge.md))。**composite alpha の優先順と preTransform=current + clipped=VK_TRUE の固定化の根拠は未執筆** |
| [present fence の pNext](../../src/core/vkcore/swapchainframetarget.cpp#L2568) / [`present_wait_pending`](../../src/core/vkcore/swapchainframetarget.cpp#L2634) | 完了追跡の部品 | 個々の役割 | 上記「present 完了追跡の 2 系統」の一部。詳細は**未執筆** |
| [acquire の lock](../../src/core/vkcore/swapchainframetarget.cpp#L2170) / [`markSurfaceLost()`](../../src/core/vkcore/swapchainframetarget.cpp#L1498) / [`abandonFrameSerial()`](../../src/core/vkcore/swapchainframetarget.cpp#L2757) | surface_epoch まわり | 一体で何を守るか | 上記 2 項目と一体。詳細は**未執筆** |
| [bootstrap epoch](../../src/core/vkcore/swapchainframetarget.cpp#L884) / [BuildRequest](../../src/core/vkcore/swapchainframetarget.cpp#L660) / [nonblocking](../../src/core/vkcore/swapchainframetarget.cpp#L2135) | 初期化と要求の粒度 | 別枠にする理由 | **未執筆**(根拠がコードにない) |
| [XR swapchain の状態機械](../../src/core/openxr/openxrcompositiontarget.cpp#L399) / [reusable 判定](../../src/core/openxr/openxrcompositiontarget.cpp#L676) | idle → acquired → waited → submitted → released | 再利用できる状態 | `beginLogicalFrame` は idle / released のみ再利用可。[「wait 成功前の release は不正」](../../src/core/openxr/openxrcompositiontarget.cpp#L484)なので、acquired 止まりの image は release せず `teardown_required` を立てて世代ごと破棄する |
| [`beginView()`](../../src/core/openxr/openxrcompositiontarget.cpp#L711) / [endView の分岐](../../src/core/openxr/openxrcompositiontarget.cpp#L820) / [view_family の終端](../../src/core/openxr/openxrcompositiontarget.cpp#L818) | 順序強制と 2 経路 | sequential と multiview の同期差 | 上記 2 経路の排他性から従う。詳細は**未執筆** |
| [`FrameStage`](../../src/core/openxr/openxrcompositiontarget.cpp#L56) / [`ViewState` と `SwapchainFrameState`](../../src/core/openxr/openxrcompositiontarget.cpp#L123) | フレームの段と、それに直交する状態 | 有効な組み合わせ | フレームの段は bool の集合ではなく 7 値の enum です。遷移の合法表は [`frameStageMoveIsLegal()`](../../src/core/openxr/openxrcompositiontarget.cpp#L86) が持ち、違反は [`requireFrameStageMove()`](../../src/core/openxr/openxrcompositiontarget.cpp#L112) が投げます。bool として残るのは眼ごとの `ViewState` と、段に直交する `depth_submission_active` / `teardown_required` だけです |
| [command buffer slot](../../src/core/openxr/openxrcompositiongraphics.cpp#L73) / [`in_flight_frames_num`](../../src/core/openxr/openxrcompositiongraphics.cpp#L335) | 索引式と枚数 | 決定基準 | **未執筆**(根拠がコードにない) |
| [`queryViews()`](../../src/core/openxr/openxrcompositiontarget.cpp#L202) | 両眼で同じ extent を強制 | 揃える理由 | **未執筆**(max(recommended) と min(maxImageRect) の組み合わせという実装以上の根拠がない) |

### 9.23.15 physics

#### [`PhysWorld::updateTriggers()`](../../src/core/phys/physworld.cpp#L698) — 2-way merge と canonical pair

**分からなくなること**: Enter と Exit が同じフレームに混ざったときの発火順。

**答え**: [コメント](../../src/core/phys/physworld.cpp#L766)どおり、両集合を **canonical な (min,max) EntityId ペアでソート済みにして 2-way merge** することで、混在フレームでも全順序が 1 つに決まります。イベントは self/other 視点なので、canonical ペア 1 つから低い側 self・高い側 self の 2 本を emit して両エンティティに届けます。差分の取り方は[第5章「差分は 1 パスのマージ」](05_gameplay_and_services.md)が本体です。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`prepareBindings()`](../../src/core/phys/physworld.cpp#L286) | 候補構築と二重チェック | なぜ全部ここで検査するか | **publish が `noexcept` になるよう失敗しうる検査を全部集める**(→ §9.22)。name 重複・collider の validate・transform 成分の存在確認を行い、id は未割り当てなら採番、既存 id が next 以上なら next を進める。上限一致で `overflow_error` にして **id の巻き戻し = 退役 collider とのエイリアス**を防ぐ |

### 9.23.16 game DLL / schema

#### [`reloadTransaction()`](../../src/core/gamelogic/gamelogicreload.cpp#L584) — previous_unloaded と rollback の分岐

**分からなくなること**: rollback の 2 分岐と、例外メッセージを退避してから unload する理由。

**答え**: 例外まわりは[第5章「例外を捨ててから unload」](05_gameplay_and_services.md)が本体です。分岐の側だけ補うと、`previous_unloaded` は「旧 DLL を実際に unload 済みか」を記録し、true なら旧シャドウコピーを**新しい owner で読み直し**(`loadCopy` + `activateProviderOwner`)、false(teardown が unload 前に失敗)なら**静的登録の owner が曖昧になる二重ロードを避けて**元のハンドル / owner をそのまま復帰させます。

#### [`activation_delay`](../../src/core/gamelogic/behaviorarena.cpp#L572) — 1 フレーム遅らせる理由

**分からなくなること**: 生成直後に init が走らない理由と、失敗時の破棄順。

**答え**: 新規 / 再構築された attachment は [`activation_delay = 1`](../../src/core/gamelogic/behaviorarena.cpp#L787) を持ち、[`update()` 末尾でデクリメント](../../src/core/gamelogic/behaviorarena.cpp#L1200)されるまで [`activateReadyEditorAttachments()` の入口で読み飛ばされます](../../src/core/gamelogic/behaviorarena.cpp#L1129)。**生成直後の同一フレームで init が走らないようにして、シーン公開が完全に終わってから 1 フレーム後に活性化する**ためです。失敗時は `activated` を逆順に辿って `destroyInstance` し、`deferred_mutations` を base まで巻き戻します(構築の逆順で壊して依存順を守り、部分活性化を残さない。→ §9.22)。

#### [`sameSceneBehaviorOrigin()`](../../src/core/gamelogic/behaviorarena.cpp#L489) — 許される遷移方向

**分からなくなること**: 既存 attachment を再利用してよい条件。

**答え**: 既存 attachment と desired の origin が同一かで再利用可否を決めます。Authored 同士は `authoring_component_index` 一致、Generated 同士は `stable_generated_id` 一致。**異種間は `OriginTransitionMap.direction` が該当方向のときだけ**(Authored→Generated なら AuthoredToGenerated、逆なら GeneratedToAuthored)、entries の gid / component_key / final_authored_index が一致すれば同一とみなします = 遷移マップが宣言した**片方向のみ**許されます(→ §9.23.2 / §9.23.9)。

#### [`diagnoseGameLogicImports()`](../../src/core/gamelogic/gamelogicimports.cpp#L559) — expected_epoch と host_schema_entrance

**分からなくなること**: 似た 2 つの版チェックが両方要る理由と、anchor が `extern "C"` である理由。

**答え**: `expected_epoch` は「ホスト実行ファイルからインポートされているべき epoch anchor シンボル([`gameLogicEpochAnchorSymbol`](../../src/core/userpublic/gamelogic.hpp#L23))」の一致検査で、**DLL が別世代の SDK でビルドされていないこと**を見ます。`host_schema_entrance` は**登録 API の世代(v2/v3)**で、DLL が host より新しい entrance を要求していないかを見ます。ABI 世代と登録スキーマ世代という別軸なので両方要ります。anchor が未装飾の `extern "C"` シンボルなのは、DLL が初期化子の前にホストから解決を強制され、**PE インポートテーブルを読むだけで世代不一致を静的に検出できる**ようにするためです(→ §9.21「raw import 診断は provider-only な旧 DLL の唯一の enforcement」)。

#### [`StructFieldRangeDescriptor`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L107) — "constexpr-in-incomplete-owner" 不変式

**分からなくなること**: 記述子が POD なのに、materialize 後は `std::variant` になる理由。

**答え**: 不変式は**「所有者クラスがまだ不完全な段階(クラス定義内での schema 宣言時)に `std::variant` を構築しない」**ことです。[POD 形式のコメント](../../src/core/userpublic/details/schema/structfieldschema.hpp#L358)が「クラス定義の中で variant を構築しない」という WP71/MSVC 不変式をそのまま書いています。MSVC ではこの状況で `StructFieldRange` variant の constexpr 構築が壊れるため、記述子は POD(descriptor + フラットな `enum_values[]` 配列)で保持し、自己参照 span を避けつつ variant の構成は型が完成した後の materialize 時まで遅らせます。同じ制約の別側面は[第5章「不完全型のまま consteval」](05_gameplay_and_services.md)。

**台帳**: コメントが名指しする WP71(MSVC 回避)。

#### [List の `$list` envelope](../../src/core/userpublic/details/schema/structfieldjson.hpp#L589) — row id の規約

**分からなくなること**: 素の配列ではなく封筒 + 行オブジェクトにした理由と、`id=0` / `next_id` の意味。

**答え**: (`docs/manual/04_scene_ecs.md` §4.4)**素の配列だと添字が行の同一性になり、先頭に 1 行挿入しただけでエディタの参照や undo が別の行を指す**ため、行は不変の `$row.id` を持つオブジェクトコンテナにしました(決定「添字を identity にしない」)。`$list.v` / `$row.v` は前方互換のためのバージョン(1 以外は `RowEnvelopeUnsupported`)、**`id=0` は `OutOfRange`("row id must be non-zero")で拒否**されます(0 を「行なし」の予約値とする明文はコードにも文書にも無く、**禁止の理由そのものは未執筆**)。`next_id > max(row id)` は「未使用番号」の意味を成立させ、新規発行 id が既存行と衝突しないことを保証します。`next_id` の弱さ自体は §9.21「`$list.next_id` は『行の最大値より大きい』しか強制されない」を参照。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [ロック検出と debounce のコメント](../../src/core/gamelogic/gamelogicreload.cpp#L774) | source_busy_retries | 失敗と pending の別 | ビルドは DLL をその場で truncate → 書き戻すことがあるので、まずサイズ / 更新時刻の同一サンプルを 2 回取って debounce し、次に `FILE_SHARE_READ` のみで開いてライタを排除してからシャドウコピーを取る。`ERROR_SHARING_VIOLATION` / `ERROR_LOCK_VIOLATION` は**失敗ではなく pending 扱い**で、retries が 3 回に達したときだけ `source_busy` |
| [`makeShadowCopy()`](../../src/core/gamelogic/gamelogicreload.cpp#L373) / [`sweepStaleShadowDirectories()`](../../src/core/gamelogic/gamelogicreload.cpp#L335) | シャドウコピーの規約 | ディレクトリ名の意味 | シャドウ先は `<source_dir>/.pelican-hot-reload/<PID>/` で、**PID をディレクトリ名にすることで同一 DLL を複数プロセスが同時にホットリロードしても衝突しない**。PDB があれば同名で併走コピーする(失敗は警告のみ)。掃除は「名前を PID としてパースでき、その PID が生存していない」ものだけ削除 |
| [`validateBehaviorReloadSources()`](../../src/core/gamelogic/behaviorreloadvalidation.cpp#L85) | schema_changed_without_version_bump | 何を比べているか | `schema_version` が同じなのに `params_schema_fingerprint` が違えば**「バージョンを上げずにスキーマを変えた」**として拒否(同一版で表現が変わると既存 canonical params の解釈が壊れる)。version が上がっている場合は authored の生 `source_params` を candidate の `canonicalize_params` に通す。指紋そのものは[第5章「スキーマの構造をハッシュ値で識別する」](05_gameplay_and_services.md) |
| [`sceneBehaviorAttachmentSeq()`](../../src/core/gamelogic/behaviorarena.cpp#L72) / [`refreshSceneAttachmentSequence()`](../../src/core/gamelogic/behaviorarena.cpp#L455) | seq の符号化と再計算 | scene_ordered のときだけ再計算する理由 | seq は「シーン内の出現順」をそのまま単調な 64bit キーにする符号化。**scene_ordered でない(ランタイム生成の)attachment は自分の識別子由来の seq を持ち、シーンの index 変化に追随してはいけない** |
| [attach 時の component_index シフト](../../src/core/gamelogic/behaviorarena.cpp#L1324) / [`reused` に載っているかの判定](../../src/core/gamelogic/behaviorarena.cpp#L1399) | 挿入 / 削除時の詰め替え | ずらさないと何が起きるか | component_index は同一 entity 内の並び順そのものなので、attach で挿入位置以降を +1、remove で以降を −1 してから seq を再計算しないと**実体とずれて衝突する**([公開時に seq 重複チェックがある](../../src/core/gamelogic/behaviorarena.cpp#L807))。所有権は「`reused` に載っている attachment のインスタンスは既存アリーナ側が持つ」モデル |
| [detached 配列との交換](../../src/core/gamelogic/behaviorarena.cpp#L472) / [`transferRuntime`](../../src/core/gamelogic/behaviorarena.cpp#L330) | publish/rollback の実体 | 所有権の移り方 | → §9.22「三段トランザクション」を behavior arena で実現したもの。詳細は**未執筆** |
| [5 種の編集の事前/事後条件](../../src/core/gamelogic/behaviorarena.cpp#L1261) / [pending/active の組み合わせ](../../src/core/gamelogic/behaviorarena.cpp#L912) | 条件表 | 各編集の前提 | 上記 identity 三層と activation_delay から従う。**個別の条件表は未執筆** |
| [`BehaviorCallbackScope`](../../src/core/gamelogic/behaviorarena.cpp#L21) | callback_owner の切り替え | 何を追跡するか | 「今どの DLL 世代のコードが走っているか」を追跡して、リロード時に古い owner のインスタンスを先に破棄できるようにするため。詳細は**未執筆** |
| [候補 DLL の検証](../../src/core/gamelogic/gamelogicreload.cpp#L466) / [Windows 固有の import 診断](../../src/core/gamelogic/gamelogicreload.cpp#L404) / [active の有無での分岐](../../src/core/gamelogic/gamelogicreload.cpp#L497) | ロード前の関門 | 入口の全体像 | 上記 `diagnoseGameLogicImports` と reloadTransaction の入口。詳細は**未執筆** |
| [`appendThunkImports()`](../../src/core/gamelogic/gamelogicimports.cpp#L306) | ordinal_flag と終端判定 | PE の読み方 | PE Import Table の仕様そのもの(→ [第10章](10_background_knowledge.md)) |
| [`recursive()`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L160) | Flags / List で true | v2/v3 分岐の判定 | v3 の記述子は所有型の再帰ツリーで、**Flags と List はフラットな V2 `StructFieldSchema` では表現できない唯一のタグ**(語彙と行 struct という子構造を持つ)。`recursive()` はそれらを部分木に含むかを返す |
| [`schemaNodeFingerprint()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L1040) | 指紋が JSON dump そのもの | hash にしない理由 | 用途は schema 同一性の厳密比較で、**正準 JSON dump は全順序かつ自己記述的なので、不一致時にどのフィールドが変わったかをそのまま diff でき、ハッシュ衝突を仮定する必要もない** |
| [Flags の文字列配列表現](../../src/core/userpublic/details/schema/structfieldjson.hpp#L404) | ビットを名前で書く | 数値にしない理由 | 語彙を持つタグの JSON 表現。数値ビットを直接書かせないことで語彙変更に耐える |
| [`Flags`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L194) | enum を unsigned storage へ | 変換の理由 | **未執筆**(flag ビット操作の正当性という以上の根拠がコードにない) |
| event 側の 3 分岐(`has_descriptor` / `recursive_descriptor` / `json_serializable`)と `EventSchemaState` | 登録時の分類 | 組み合わせ表 | **未執筆**(3 条件の組み合わせ表が要る) |

### 9.23.17 glTF

#### [`next_generated_material = -2`](../../src/core/model/gltf.cpp#L1297) — 負値が生成マテリアルを示す

**分からなくなること**: マテリアル ID に負値が現れる理由。

**答え**: [第3章「-1を空けて負へ伸ばす」](03_project_and_loading.md)が本体です。1 点だけ補うと、[型そのもの](../../src/core/model/gltf.cpp#L833)が `int` で、glTF の material index は 0 以上しか取らず −1 が「マテリアル無し」なので、−2 から負方向へ数えれば単一のキー空間でソース index と絶対に衝突しません。

#### [`resolveFragmentCandidate()`](../../src/core/model/gltf.cpp#L1832) — full_path と alias

**分からなくなること**: 2 通りの指定が両方一致したときに、なぜエラーにするのか。

**答え**: `full_path` はシーングラフ上の一意な経路指定、alias は node 名 / mesh 名という短縮指定で、**短縮名は複数の経路に出現しうる**からです。複数一致したらローダが勝手に選ばずエラーにして、呼び出し側にフルパスでの曖昧性解消を要求します(重複フルパス自体はその手前で拒否)。

#### [VRM firstPerson auto の primitive 分割](../../src/core/model/gltf.cpp#L1094) — なぜ分けるしかないのか

**分からなくなること**: 三角形単位の可視性を、なぜ primitive を増やして表すのか。

**答え**: [第3章「頭に触れたら三角ごと」](03_project_and_loading.md)が本体です。1 点だけ補うと、可視性は `PrimitiveViewVisibility` として **primitive 単位の属性**なので、同一 primitive を both 用と third_person_only 用の 2 つの index セットに分割して別 primitive として登録するしかありません。

#### [morph target layout の generation](../../src/core/model/gltf.cpp#L47) — 何を追跡する世代か

**分からなくなること**: GPU 割り当てとの関係。

**答え**: プロセス全体で単調増加する**「不変なモデル世代の identity」**([`morphtarget.hpp` のコメント](../../src/core/model/morphtarget.hpp#L40))で、[ロード完了時に 1 つの `MorphTargetLayout` へ付与](../../src/core/model/gltf.cpp#L2525)されます。リロードでレイアウトが差し替わったことを消費側が検出でき、かつ **GPU 割り当て順に依存しない**同定ができます。

#### [`geometry_allocation_id`](../../src/core/model/modeltemplate.hpp#L98) — 何を識別し、どこで使うか

**分からなくなること**: この ID が指す粒度。

**答え**: 1 回のジオメトリ suballocation(index / vertex レンジの組)を識別する単調 ID で、[払い出しは vertbufcontainer 側](../../src/core/model/vertbufcontainer.cpp#L445)、[`modeltemplate.hpp` のコメント](../../src/core/model/modeltemplate.hpp#L93)どおり **ray-query の BLAS キャッシュキー**として [renderer](../../src/core/vkcore/renderer.cpp#L5299) 付近で使われます(→ §9.23.5 / §9.23.8)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [palette 超過](../../src/core/model/gltf.cpp#L2289) | error でなく `skinned=false` | 落とさない理由 | [コメント](../../src/core/model/gltf.cpp#L2290)どおり**「v1 パレット超過のスキンを持つアセットも読み込めなければならない」**(WP38 以前は skin を丸ごと無視していた挙動とのパリティ)。明示的にそのスキンをアニメーションで使った場合だけ `selectSkin` が例外にする |
| [`transformVertexData()`](../../src/core/model/gltf.cpp#L711) | morph の有無で normal 正規化を分ける | 正規化しない場合がある理由 | morph がある場合、base normal/tangent を正規化すると**同じ normal_transform を掛けただけの morph delta と線形空間が食い違い、`normal + Σw·delta` の合成が壊れる**ため、非正規化のまま残して合成後にシェーダ側で正規化させる |
| [`selectSkin()`](../../src/core/model/gltf.cpp#L1632) | 複数 skin を 1 パレットへ | joint_offset の意味 | → [第3章「128枚の結合palette」](03_project_and_loading.md) |
| [`gltfSurface()`](../../src/core/model/gltf.cpp#L1390) | `engine://` surface の解決 | authoring 由来を受けない理由 | ローダは glTF のソース値を運ぶだけで、**routing とラッパ選択は project 側の lowering が行い**、その結果選ばれる `engine://surfaces/openpbr/*.surface` を組み込みリソースから解決・パース・キャッシュする。非 engine 参照は例外 |
| [`skinnedMaterial()`](../../src/core/model/gltf.cpp#L902) | skinned variant の複製 | 複製が要る理由 | 同じ glTF マテリアルが skinned / 非 skinned の両方の primitive から参照されうるが、**skinned 側は別 vert shader と `skinned = true` が要る**ため別登録が必要。`local_material_id` をキーに memo 化する |
| [`authored_reference` があるときだけ IR を計算](../../src/core/model/gltf.cpp#L2146) | normalized_alpha_ir の条件 | 空のままにする理由 | **typed な project / path-resolver 経路だけが authored provenance を持ち、IR の identity は authored reference から作られる**ので、inspection や legacy 直接ロードでは identity を捏造せず空のままにする(→ §9.23.5 の profile 表) |
| [`selectLoad()`](../../src/core/model/gltf.cpp#L1889) | animation_only | mesh を読まない理由 | animation fragment は **CPU 側のクリップ / スキンデータのみ**を持ち、意図的に無関係な mesh・material・texture・ECS ボーンリソースを一切作らない(`roots` が空なので mesh 経路にも入らない) |
| [`meshMorphTargetCount()`](../../src/core/model/gltf.cpp#L1422) | 全 primitive で同数を要求 | 揃える理由 | `mesh.weights` / `node.weights` は **mesh 単位で 1 本の配列**で、`appendMorphDefaults` も mesh 単位で 1 つの `morph_weight_offset` ブロックしか確保しないので、primitive ごとに数が違うと 1 本の weight レンジを共有できない。→ [第3章「morphはnode単位で切る」](03_project_and_loading.md) |
| [morph accessor の sparse 拒否](../../src/core/model/gltf.cpp#L761) | morph accessor の制限 | 2 つの理由 | sparse は morph target v1 のデコード経路が未実装なので明示拒否。finite 検査は delta が GPU バッファと bounds ソースに入るため、**NaN/Inf が bounds とドローソートを汚染するのを入口で止める** |
| [`collectNodeOccurrence()`](../../src/core/model/gltf.cpp#L1685) | (node_index, full_path) で重複検出 | DAG を許す理由 | 同一 node が複数の親から参照される再利用を許しつつ、**同じ (node, path) の再訪だけを打ち切る**ため。パスが違えば別インスタンスとして記録され、同じパスでの再帰は 1 回で止まる |
| [`~LiveGltfResourceSink()`](../../src/core/model/gltf.cpp#L573) / [`finish()`](../../src/core/model/gltf.cpp#L560) | release と finish の排他 | `committed_` の意味 | → §9.22「三段トランザクション」(`committed_` が「もう解放してはいけない」印)。詳細は**未執筆** |
| [`firstPersonAnnotation()`](../../src/core/model/gltf.cpp#L655) / [`headRelatedNodes()`](../../src/core/model/gltf.cpp#L681) | 4 値とメモ化 | 分割の入力 | 上記 VRM 分割の入力。詳細は**未執筆** |
| [lowering → surface compilation → routing](../../src/core/model/gltf.cpp#L2085) / [named_materials の構築](../../src/core/model/gltf.cpp#L2508) / [VAT テクスチャ](../../src/core/model/gltf.cpp#L870) | 3 段の材質処理 | 段の切れ目 | **未執筆**(→ [第3章「逸脱がforwardへ落とす」](03_project_and_loading.md) が routing 側の一部を扱う) |

### 9.23.18 領域をまたぐ 3 つの規約

- **診断・トレース JSON は外部から読まれる前提で書かれています。** `basicconfig` の保存診断、`effecttransaction` の effect 診断、`renderer` のフレームトレース、`rpcserver` の status JSON はいずれも RPC / ログ越しに外部ツールが読む出力です。契約側の明文は `docs/external_tools_requirements.md` の **R8**(JSON-RPC 2.0 / NDJSON フレーミング)と **R10**(出力に schema 名 + version を埋め、互換を壊す変更は version を上げる)で、**個々の診断 JSON のどのキーまでが契約かを定めた記述は無く、未執筆**です。
- **エラーコードには `@1` が付きます。** `pelican.ray_row_count_limit@1`、`pelican.ray_profile_opaque_as@1`、`pelican.ray_tracing.frame_tlas_required@1`、editor 側の各コードがこの形です。`docs/design_error_contract.md` も `UserDataError` の `code` を「名前付き、`@1` 付き」と書いていますが、**この番号が何の版で、いつ上げるのかを定めた記述はコードにも文書にも無く、未執筆**です(「コード名の意味の版」と読むのは推測)。
- **表示用文字列は identity ではありません。** [`displayObjectName()`](../../src/core/loader/scene.cpp#L275) の `<unnamed>` のような値は診断出力の見た目で、同一性の判定には使いません。identity 側は §9.22「identity / canonical key / lease の三層」を参照してください。

なお **「機械判定できる subset を先に固定する」**(`docs/design_raytracing.md` §4 / §4-P の規範表。「決定論的適合部分集合」という語自体は文書にありません)という考え方 — GPU や数値の非決定性を避けるため、形状と数値範囲を先に制限してから比較する — は RT だけのものではありません。physics の tie-break(§9.23.15)、draw order の tie-break(§9.23.4 の `declaration_ordinal`)にも同じ発想が現れます。

## 9.24 v0 で足した機構の難所(共通グラフ・package・節 locator・オフライン時刻)

ここに並ぶ 12 件は、v0(AG / P / O / X / R 群と、V0-ADD の RS-1)で入った機構のうち、**素朴に読むと逆の結論に着地する**箇所です。他章の `🧩 難所` ブロックと同じ役割なので、書式もそれに合わせて「素朴に読むと / 実際は / 不変条件 / 読む場所」の 4 つで書いてあります。**この節だけは実装が新しいぶん、既存の設計文書側に説明が無い**ものが多く、根拠はすべてコードとヘッダのコメントです。

### 難所 — byte 正確な節 locator と絶対 offset

**素朴に読むと**: `pelican.graph` root の `rendering` 節を編集するなら、JSON を parse して部分木を取り出し、書き換えて dump し直せばよい、と読めます。

**実際は**: 編集は最後まで **byte 列のまま**行われます。[`locateSectionMembers()`](../../src/project/renderconfigdocument.cpp#L305) は root のメンバを字句走査する [`locateObjectMembers()`](../../src/project/renderconfigdocument.cpp#L200) を 2 回使い、まず root で `rendering` の値スパンを見つけ、その位置から同じ走査を節の中へ入れ直します。要点は**入れ子にしても offset を相対化しないこと**で、ヘッダのコメントがそれを規範として書いています — 「同じ bytes の中に位置づけるので、offset は絶対のまま、文書 digest は file 全体を覆い続ける」([コメント](../../src/project/renderconfigdocument.cpp#L303))。この性質があるので、節を編集しても CAS の単位([`renderConfigSourceDigest()`](../../src/project/renderconfigdocument.cpp#L427))は文書全体のままで変わりません。書き手は [`withSectionValue()`](../../src/project/renderconfigdocument.cpp#L671) 1 本で、graph root 自身のキー(と editor の `layout`)は [`withRootValue()`](../../src/project/renderconfigdocument.cpp#L650) へ落ちます(MG-2b で legacy root は読み手ごと無くなりました)。

読み手と書き手の非対称も意図です。[`renderingSectionOf()`](../../src/project/renderconfigsection.hpp#L36) は節が無ければ `RenderConfigSectionUnlocated` を投げ、[`renderingSectionOrEmpty()`](../../src/project/renderconfigsection.hpp#L49) は空 object を返します。**読みは節が無くても失敗せず、編集だけが `render_config_section_unlocated` という名前で拒否されます**([拒否の実例](../../src/core/communication/renderconfigeditor.cpp#L1277))。

**不変条件**: 節の中の offset を「節の先頭からの相対」に変えないこと(digest が節だけを覆う別物になり、CAS の意味が変わります)。読みの経路に `renderingSectionOf()` を使わないこと — 節を持たない graph root を読んだだけで例外になります。authored key の住所判定を各 caller が `if (graph)` で持たないこと。

**読む場所**: [`renderconfigsection.hpp`](../../src/project/renderconfigsection.hpp#L12) → [`locateSectionMembers()`](../../src/project/renderconfigdocument.cpp#L305) → [`withSectionValue()`](../../src/project/renderconfigdocument.cpp#L671)。

> **N-6 の例外**: `edit_graph_document` の op 列が `set_layout` だけのとき、editor は候補文書を `apply_candidate` に通さず(`Ticket::source_only`)、文書を書くだけで commit を終えます。「すべての commit は候補経路を通る」の唯一の例外で、置き場所は engine の挙動に触れないためです([経路](../../src/core/communication/renderconfigeditor.cpp#L2454))。

### 難所 — 排他 create の committed marker

対象は project 文書の transaction です。rendering config の編集のために作られ(G-3a)、MG-1 で `src/project/projectdocumenttransaction.*` へ移って `project migrate` の apply と共用になりました。ディレクトリ(`.pelican/render-authoring.transaction`)も形式も移設の前と同じなので、以下の規則は両方の書き手に効きます。

**素朴に読むと**: transaction が commit されたかどうかは `manifest.json` の `status` を読めば分かる、と読めます。

**実際は**: 決定は**別ファイルの存在**です。`manifest.json` は [`writeManifest()`](../../src/project/projectdocumenttransaction.cpp#L430) が**一度だけ**書いて二度と上書きせず、commit の印は [`publishCommittedMarker()`](../../src/project/projectdocumenttransaction.cpp#L463) が `committed.next` → `committed` の hard link で公開します。使う primitive は [`linkExclusive()`](../../src/project/projectdocumenttransaction.cpp#L306) で、「名前を公開し、既に取られていれば失敗する」を 1 操作で行います。rename にしなかった理由はコメントが規範で、**生きたファイルを置き換えないので、置き換え側が飽和した scanner の下で負け続けていた問い(宛先のハンドルを壊せるか)を訊かずに済む**からです。

marker は自分が属する manifest に digest で縛られます。[`transactionDecision()`](../../src/project/projectdocumenttransaction.cpp#L498) は marker を先に見て、記録された digest と現在の `manifest.json` の digest が食い違えば `render_authoring_manifest_mismatch@1` で[名前付きに落とします](../../src/project/projectdocumenttransaction.cpp#L508) — 既に durable な編集を黙って rollback しないためです。旧形式(marker を持たず、`manifest.json` の `status == "committed"` で決定を書いていた形)は MG-2a から**読みません**。R-1 の裁定では読み取り互換を残していました(`docs/v0_freeze.md` §2 行 6。消すと legacy なディレクトリを持つ project が二度と commit できない wedge になる、という理由)。MG-2a は「黙った wedge を名前付きの停止に変える」([design_v0_production_gaps.md](../design_v0_production_gaps.md) §2.3 決定 3)を採り、2 つの読み手を同時に消しました。今は [`rejectLegacyTransactionDirectory()`](../../src/project/projectdocumenttransaction.cpp#L488) が recovery の判定(`transactionDecision()` の先頭)と commit 前の guard の両方で呼ばれ、`render_config.legacy_transaction_dir@1`(error_fields は `path`)を投げます。prepared として rollback すれば commit 済みの編集を消し、committed として片付ければ知らない形式を黙って読むことになるので、どちらもせずディレクトリを残して開発者に渡します。edit の結果にもこの code と `path` が載ります(G-4 の carried refusal の仕組みに相乗り)。

掃除の順序も難所です。[`removeTransactionDirectory()`](../../src/project/projectdocumenttransaction.cpp#L673) は `manifest.json` を**明示的に先に**消します。ディレクトリの列挙順は照合順で `committed` が `manifest.json` より先に来るため、素直に `remove_all` へ任せると「marker だけ消えて prepared な manifest が残る」= 復旧が「rollback すべき transaction」と読む最悪の残骸ができるからです([順序のコメント](../../src/project/projectdocumenttransaction.cpp#L680))。

**不変条件**: marker が見えたら rollback 経路に入らないこと。旧形式の dir を rollback も片付けもせず、名前付きで止めること。marker の公開を rename 系に戻さないこと。`manifest.json` を後から書き換えないこと(digest の束縛が切れます)。掃除で `manifest.json` を最初に消す順序を変えないこと。

**読む場所**: [`linkExclusive()`](../../src/project/projectdocumenttransaction.cpp#L306) → [`publishCommittedMarker()`](../../src/project/projectdocumenttransaction.cpp#L463) → [`transactionDecision()`](../../src/project/projectdocumenttransaction.cpp#L498) → [commit 前の guard](../../src/project/projectdocumenttransaction.cpp#L792)。

### 難所 — unwrapped time から作る root delta(AG-2)

**素朴に読むと**: root motion は「今フレームの root 位置 − 前フレームの root 位置」で足りるように見えます。

**実際は**: clip の時刻は境界で折り返されるので、そのままの差分はループのたびにクリップ先頭へ飛び戻ります。[`extractRootMotion()`](../../src/core/animation/posegraphruntime.cpp#L322) は**折り返す前の時刻**(`unwrapped`)を受け取り、`R(u) = loops(u)·(root(end) − root(start)) + root(canonical(u))` を作ります([式のコメント](../../src/core/animation/posegraphruntime.cpp#L363))。`loops` は[非ループ clip では 0](../../src/core/animation/posegraphruntime.cpp#L369) で、この項があるおかげで「負の速度」も「1 フレームで複数ループ」も同じ算術に収まります。clip 定数(`root_start` と 1 ループ分の移動量)は binding ごとに 1 回だけサンプルし、**そのフレームの姿勢を一時的に使って測ってから元に戻します**([サンプル部](../../src/core/animation/posegraphruntime.cpp#L339))。

フレーム差分は `R(u) − R(u_prev)` で、`set_time` を観測したフレームは [`drop_root_previous`](../../src/core/animation/posegraphruntime.cpp#L376) によって前回値を捨てます — 時刻のカットは移動ではないからです。再バインドでも同じく状態を捨てます([rebind 時の reset](../../src/core/animation/posegraphruntime.cpp#L242))。最後に **pin**: 公開する姿勢の root translation を `root_start` へ戻し([pin](../../src/core/animation/posegraphruntime.cpp#L386))、移動はオブジェクトの transform 側からだけ出ていくようにします。回転の抽出は v0 にありません。

**不変条件**: `canonical` 済みの時刻から delta を作らないこと(ループごとに逆向きの跳ねが出ます)。pin を外したまま変位も足さないこと(二重に動きます)。`set_time` / 再バインドで前回値を残さないこと。

**読む場所**: [`canonicalTime()`](../../src/core/animation/posegraphruntime.cpp#L37) → [`extractRootMotion()`](../../src/core/animation/posegraphruntime.cpp#L322) → [変位をオブジェクトへ渡す所](../../src/core/animation/posegraphruntime.cpp#L807)。適用側は[第4章 §4.13](04_ecs_deep_dive.md)。

### 難所 — per-node の評価分類は `max()` でしか動かない

**素朴に読むと**: 「この op の評価分類」はモジュールが登録時に宣言した 1 値で、node ごとには変わらない、と読めます。

**実際は**: ports を authored parameters から作る factory は、**分類を引き上げることだけ**ができます。[`resolveOperation()`](../../src/project/graphmodule.cpp#L550) は factory の結果に対して `contract.evaluation = std::max(...)` を取り([該当行](../../src/project/graphmodule.cpp#L569))、`direct < replay < baked < recorded` の順で強い方を採ります。理由は AG-2 で現物が出ました — root motion を抽出する `pose.clip_sample` は前フレームの変位を保持するので `replay` で、抽出しない同じ op は `direct` です。`max` なので**カタログの宣言より弱くはなれず**、catalog と実際の node が食い違いません。[`domain`](../../src/project/graphmodule.cpp#L560) / [`parameter_change`](../../src/project/graphmodule.cpp#L570) / [`parameter_changes`](../../src/project/graphmodule.cpp#L571) の 3 つは、factory の値を捨てて登録側の値へ戻します。

評価分類を実際に強制するのは値バックエンドで、状態を渡されずに replay 系の op を評価しようとすると [`graph.evaluation_requires_state@1` の送出](../../src/core/userpublic/valuegraph.cpp#L50) で拒否します。対象は `audio.playback` / `value.event_clock` / `value.trigger_clock` の 3 つ([拒否点](../../src/core/userpublic/valuegraph.cpp#L718))で、**op 名は authored 文書の綴り**で返ります(lowering がモジュール名前空間を落とすので、拒否を読む側がその事情を知らなくて済むように、という取り決めです)。

**不変条件**: factory で `evaluation` を下げないこと。分類の順序(`direct < replay < baked < recorded`)を [`graphcomposition.hpp`](../../src/project/graphcomposition.hpp#L223) の enum 順と別に持たないこと。拒否に lowering 後の綴りを載せないこと。

**読む場所**: [`GraphOperationRegistration`](../../src/project/graphmodule.hpp#L25) → [`resolveOperation()`](../../src/project/graphmodule.cpp#L550) → [`statelessEvaluationRejected()`](../../src/core/userpublic/valuegraph.cpp#L49)。

### 難所 — package の SDK は「転送ヘッダのディレクトリ」そのもの

**素朴に読むと**: package のコードが public SDK だけを使っていることは、レビューか命名規約で守るものに見えます。

**実際は**: **include path で守られています**。[`pelican_package_sdk_include_dir()`](../../src/player/CMakeLists.txt#L161) が build ディレクトリの下に、public SDK が公開するヘッダ 1 本につき 1 行だけの転送ヘッダ([`package_sdk_header.hpp.in`](../../src/player/package_sdk_header.hpp.in))を生成します。対象は `src/core/userpublic/**`、`src/core/ecs/**`、`handle.hpp`、`phys/physquery.hpp`。[`pelican_package_sources()`](../../src/player/CMakeLists.txt#L205) が作る package target の include path には **`src/core` が入りません**([その省略に付いたコメント](../../src/player/CMakeLists.txt#L232))。したがってエンジン内部ヘッダを include した package コードは、規約違反ではなく**コンパイルエラー**になります。コピーではなく転送なので、実ヘッダは自分の相対 include を自分で解決でき、内容が腐りません。

呼ぶ順序も強制されます。[`pelican_packages()`](../../src/player/CMakeLists.txt#L262) は project の `packages/*/package.json` を名前順に調べ、`code/CMakeLists.txt` を持つものだけを `add_subdirectory` し、生成された OBJECT ライブラリを project の game logic DLL へ足します。DLL がまだ無ければ [`FATAL_ERROR`](../../src/player/CMakeLists.txt#L310) で「`pelican_game_sources(...)` の後に呼べ」と言います。中断した install が残した [`.tmp-` 付きディレクトリは読み飛ばし](../../src/player/CMakeLists.txt#L279)ます。

**不変条件**: package target の include path に `src/core` を足さないこと。転送ヘッダの生成対象を「SDK が公開するもの」以外へ広げないこと。`pelican_packages()` を `pelican_game_sources()` より前に置かないこと。

**読む場所**: [`package_sdk_header.hpp.in`](../../src/player/package_sdk_header.hpp.in) → [`pelican_package_sources()`](../../src/player/CMakeLists.txt#L205) → [`pelican_packages()`](../../src/player/CMakeLists.txt#L262)。

### 難所 — publish snapshot の `node_values` は「グラフを持った」ベクタ

**素朴に読むと**: 評価結果は名前付きの map で返る、あるいは名前を持たない値の配列を受け取って後からグラフに問い合わせればよい、と読めます。

**実際は**: [`PublishResult::node_values`](../../src/core/userpublic/valuegraph.hpp#L105) は**グラフ自身の node 順に並んだ `optional<double>` のベクタ**です。名前は載りません — 名前の解決は「読む側が 1 回だけ払う」もので、描画経路が毎フレーム払うものではない、という判断です。非活性スコープで飛ばされた node は 0 ではなく**不在**になります。

スナップショット側の [`PublishedInstanceInputs`](../../src/core/userpublic/valuegraph.hpp#L109) が重要で、**順序を決めたグラフを `shared_ptr` で保持します**([コメント](../../src/core/userpublic/valuegraph.hpp#L116))。借用ではないので、描かれた世代が差し替わった後に到着した読み手も、その世代の名前で値を読めます。公開の唯一の入口は [`publishGraphValues()`](../../src/core/loader/scenegraphruntime.cpp#L144) で、ここで `published = true` が立ちます。したがって **ベクタだけを持ち回る reader は、再 publish 後に別の展開へ index することになります**。

**不変条件**: `node_values` をグラフと切り離して保持しないこと。欠けている値を 0 で埋めないこと(「評価されなかった」と「0 だった」は別)。値を読むために再評価しないこと — 再評価は clock を進め、イベントを消費します([コメント](../../src/core/userpublic/valuegraph.hpp#L94))。

**読む場所**: [`PublishResult`](../../src/core/userpublic/valuegraph.hpp#L91) → [`PublishedInstanceInputs`](../../src/core/userpublic/valuegraph.hpp#L109) → [`publishGraphValues()`](../../src/core/loader/scenegraphruntime.cpp#L144)。

### 難所 — `$pose/` のスカラー橋と 2 つの EvaluationState

**素朴に読むと**: 1 つの文書なのだから評価状態も 1 つで、animation 側と rendering 側が同じ状態を共有すればよい、と読めます。

**実際は**: 同じ文書が 2 つの区画に分かれ、**同じフレームの中で 1 段ずれて**評価されます。[`GraphExecutionPartitions`](../../src/project/graphpartition.hpp#L7) の `animation` は values より 1 段早く走る、という順序の宣言です([宣言とコメント](../../src/project/graphpartition.hpp#L10))。ただしこれは runtime の受け渡しではありません。animation フェーズは values 区画を**自分の `EvaluationState` で自分で評価し直し**([`evaluateLowered()` の呼び出し](../../src/core/ecs/predefined/animationsystem.cpp#L368))、その出力から `$pose/<node>/<port>` という名前を拾います。この名前は区画分割のときに values 側の出力として作られる**宣言上の名前**で([分割側](../../src/project/graphpartition.cpp#L304))、綴りは両側が [`graphAnimationScalarOutput()`](../../src/project/graphpartition.cpp#L380) の 1 箇所を呼びます(instance 入力側は同じ理由で [`graphInstanceInputOutput()`](../../src/project/graphpartition.cpp#L384) の `$value/<入力>`)。**供給は一方向**で、逆流はありません。

その帰結として、`SceneLoader` は 1 つの文書に対して評価状態を 2 つ持ちます — [`graphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L149) と [`poseGraphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L153)。共有すると、replay 系の op(`value.trigger_clock` など)が 1 フレームに 2 回進みます。2 つは必ず一緒に動きます: 世代が変わるときは[両方を移送](../../src/core/loader/scenegraphruntime.cpp#L337)し、owner が変わったスコープは[両方を reset](../../src/core/loader/scenegraphruntime.cpp#L99) します。片方だけ残すと、姿勢側の clock だけが巻き戻ります。

**不変条件**: 2 つの評価状態を 1 本にまとめないこと。移送と reset を片側だけで行わないこと。animation 側から values 側へ値を書き戻す経路を作らないこと。

**読む場所**: [`graphpartition.hpp`](../../src/project/graphpartition.hpp#L7) → [`poseGraphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L153) → [消費側](../../src/core/ecs/predefined/animationsystem.cpp#L366)。

### 難所 — `--start-time` は「量子化された frame 番号」でしか engine に入らない

**素朴に読むと**: 「この時刻から出力する」なので、engine には秒が渡ってどこかで seek する、と読めます。

**実際は**: 秒が frame 番号になるのは [`parseStartFrame()`](../../src/player/main.cpp#L326) の 1 箇所だけで、`llround(seconds * fps)` が量子化です([規範コメント](../../src/player/main.cpp#L317))。**engine はフレームの間の時刻を渡されません**。parse 自体は [`std::stod()`](../../src/player/main.cpp#L330) を呼びますが、**消費した文字数を突き合わせる**ので([検査](../../src/player/main.cpp#L334))、綴り間違い・非有限・負のすべてが同じ [`offline.time_invalid@1` のメッセージ](../../src/player/main.cpp#L336) になります(「そのようなフレームは無い」という 1 つの答えだから、という理由がコメントにあります)。

seek が無い代わりに置かれているのが **replay prologue** です。[`loop.cpp` の該当分岐](../../src/core/appflow/loop.cpp#L672)が `start_frames − 1` フレームを capture 無しで回し、`offline: start_time=… start_frame=… replay=… frames=…` を stderr に出してから通常の capture ループへ入ります。理由はコメントが規範で、**フレームの見た目を決める状態は時計だけからは到達できない**(グラフの clock は `render()` の中でしか進まず、project のシステムは自分で dt を積分する)からです。

**不変条件**: 量子化を `floor` などへ変えないこと(同じ時刻指定が別のフレームを指し、連番出力の byte 比較が壊れます)。replay フレームで capture 以外の処理を省かないこと。3 つの失敗を別々の名前に分けないこと。既知の穴として、`llround` の overflow は検査の前に溢れます(溢れるのは `seconds * fps` が 9.223e18 を超えるときなので、既定の **fps=60 でおよそ 1.54e17 秒**。`--fps` は[利用者が指定できます](../../src/player/main.cpp#L559))(`docs/v0_freeze.md` §4)。

**読む場所**: [`parseStartFrame()`](../../src/player/main.cpp#L326) → [`--start-time` の宣言](../../src/player/main.cpp#L373) → [replay prologue](../../src/core/appflow/loop.cpp#L672)。

### 難所 — 解決できない owner は警告を出して非アクティブになる、警告は 1 回だけ

**素朴に読むと**: `graph.instance` が指すオブジェクトが見つからなければ、毎フレーム警告が出るかエラーになる、と読めます。

**実際は**: **静かに inactive になります**。[`inactiveGraphScopes()`](../../src/core/loader/scenegraphruntime.cpp#L409) は解決できなかったスコープを非活性集合に入れるだけで、何も言いません。これは描画経路で毎フレーム呼ばれるからで、オブジェクトが再公開されている最中の正常な状態と区別が付かないためです。代わりに、グラフ世代の同期時に [`reportUnresolvedGraphOwners()`](../../src/core/loader/scenegraphruntime.cpp#L59) が「このシーンで一度も解決したことのないスコープ」だけを 1 回警告します。2 度目を出さないのは [`graph_owner_reported_scopes`](../../src/core/loader/scenegraphruntime.cpp#L70) への `emplace` の戻り値で、両方のメモはシーンのロードでだけクリアされます([理由のコメント](../../src/core/loader/scenegraphruntime.cpp#L75))。

これが v0 で最も踏みやすい無音の失敗で、**グラフはロードされているのに何も起きない**という形で出ます。以前はこれに「`graph.instance.owner` の authoring object id が保存 → 再読込を跨ぐと別個体を指しうる」穴が重なっていましたが、保存が `authoring_object_id` と `next_authoring_object_id` を書くようになって解消しました(`efb30dad`、[`resolveAuthoringIdentities()` の読み取り](../../src/project/sceneformat.cpp#L45)。OL-1 が `duplicate` と挿入の後の保存・再読込を `test/sceneformat_test.cpp` の `[ol1]` で確かめています。`docs/v0_freeze.md` §4.1)。

**不変条件**: 描画経路側で警告を出さないこと。メモをフレーム境界でクリアしないこと。「解決できない」を評価時のエラーへ格上げしないこと(再公開中の正常状態を殺します)。

**読む場所**: [`graphOwnerObject()`](../../src/core/loader/scenegraphruntime.cpp#L40) → [`reportUnresolvedGraphOwners()`](../../src/core/loader/scenegraphruntime.cpp#L59) → [`inactiveGraphScopes()`](../../src/core/loader/scenegraphruntime.cpp#L409)。

### 難所 — `pelican.pose@1` は意図して不透明

**素朴に読むと**: port を流れる型なのだから、姿勢のバイト列(joint の TRS 配列)が流れている、と読めます。

**実際は**: 流れるのは **staged pose のハンドルと、それが作られた `PoseLayout` の世代・フレーム revision** です([型の説明](../../src/project/posegraphmodule.hpp#L11))。バイトでないからこそ、別の layout 世代で作られた 2 つの姿勢を `memcpy` で混ぜる代わりに、名前で `pose.layout_mismatch@1` として弾けます([宣言](../../src/core/animation/posegraph.hpp#L86))。判定の材料はトレースにも出ており、sink ごとに `layout_identity` / `layout_generation` が記録されます([`SinkTrace`](../../src/core/animation/posegraphruntime.hpp#L112))。`pelican.rig@1` は、op が clip と出力 layout を解決するための rig 同一性です。

契約(型と op)が `src/project` 側にあるのも同じ理由です — カタログ、エディタ、そして**アニメーションバックエンドを外した build** の 3 者が同じ宣言を見るためで、評価だけが `src/core/animation` の仕事です。

**不変条件**: `pelican.pose@1` に bytes の意味を与えないこと。layout 世代の比較を「等しいはず」として省かないこと。契約を評価側へ移さないこと(OFF build とカタログが食い違います)。

**読む場所**: [`posegraphmodule.hpp`](../../src/project/posegraphmodule.hpp#L7) → [`PoseGraph::raiseLayoutMismatch()`](../../src/core/animation/posegraph.hpp#L86) → [`SinkTrace`](../../src/core/animation/posegraphruntime.hpp#L112)。

### 難所 — 再生パラメータの既定値を本当に持つのは 2 関数だけ

**素朴に読むと**: `speed` / `start_time` / `loop` の既定値は、契約側と評価側の両方に書いてある(あるいは構造体の初期化子が正)と読めます。

**実際は**: project 層の 4 関数が入口ですが、**既定値そのものを持っているのは 2 つ**です — [`poseClipSampleLoop()`](../../src/project/posegraphmodule.hpp#L35) が `true`、[`poseClipSampleExtractsRootMotion()`](../../src/project/posegraphmodule.hpp#L45) が `false`。[`poseClipSampleNumber()`](../../src/project/posegraphmodule.hpp#L33) は `double fallback` を**引数で受け取る**ので、`speed = 1.0` / `start_time = 0.0` のリテラルは呼び出し側に書かれています([契約側](../../src/project/posegraphmodule.cpp#L166) と[評価側](../../src/core/animation/posegraph.cpp#L107) の 2 か所)。[`poseClipSampleRootNode()`](../../src/project/posegraphmodule.hpp#L49) には既定値がなく、不在・負・過大はすべて `pose.root_node_unknown@1` です。アニメーションバックエンドも同じ関数を通るので、node が「カタログにとっての意味」と「評価器にとっての意味」を持ち分けることはできません。`PoseGraphNode` 側のメンバ初期化子は同じ値を持っていますが、コメントが「既定値は `posegraphmodule.hpp` にある。ここでは言い直さない」と明記しています([`PoseGraphNode` のコメント](../../src/core/animation/posegraph.hpp#L35))。

エラーの使い分けも意図的です。`root_motion` に `"none"` / `"extract"` 以外を書いたときは**素の `invalid_argument`** で、名前付きの拒否は「rig と clip とオブジェクトが揃って初めて分かるもの」に予約されています([コメントの規範](../../src/project/posegraphmodule.hpp#L36))。`pose.root_node_unknown@1` は後者で、範囲外も layout に無いのも同じ名前です。

**不変条件**: 既定値を評価側やスキーマ側で言い直さないこと。文書だけで決まる誤りに名前付きコードを与えないこと(逆に、rig / clip / オブジェクトを要する誤りを素の例外にしないこと)。

**読む場所**: [`posegraphmodule.hpp`](../../src/project/posegraphmodule.hpp#L28) → [`PoseGraphNode` の 3 メンバ](../../src/core/animation/posegraph.hpp#L37) → [rig 依存の拒否](../../src/core/animation/posegraph.hpp#L101)。

### 難所 — 物理の契約の拒否は最後に投げ、editor はそれだけを commit する(RS-1)

**素朴に読むと**: runtime が候補を断ったら editor は必ず巻き戻す、また [`MaterialGenerationPlanner::validateCompatibility()`](../../src/core/material/materialgenerationplanner.cpp#L1202) の検査は見つけた順に投げる、と読めます。

**実際は**: 物理の描画の契約(color / depth の format・sample 数・local read・attachment state)の違いだけは、見つけてもすぐには投げません。最初の 1 件を `std::optional<MaterialPhysicalContractChanged>` に取っておき([取っておく所](../../src/core/material/materialgenerationplanner.cpp#L1209))、受け皿の検査(`requireEveryMaterialServed()`)と、全 material・全 pass の output schema と `ray_query` の検査が通った後に投げます([投げる所](../../src/core/material/materialgenerationplanner.cpp#L1290))。型は [`MaterialPhysicalContractChanged`](../../src/core/material/materialphysicalcontractchanged.hpp#L46)(`std::runtime_error` の派生。`info()` が `{material_id, material, pass, detail[]}`)で、code は [`materialPhysicalContractChangedCode`](../../src/core/material/materialphysicalcontractchanged.hpp#L13) です。**この順があるので、editor はこの型を見たら「これが唯一の拒否だった」と読めます。**

editor の側では、runtime の完了が [`completeCommitTransaction()`](../../src/core/communication/renderconfigeditor.cpp#L5108) に戻ったとき、staged でも committed でもなく source commit も呼ばれていない結果の例外を、[`physicalContractRestartReason()`](../../src/core/communication/renderconfigeditor.cpp#L139) が rethrow して型を見ます。この型のときだけ、editor 自身が ticket の preset の CAS(`CommitTransaction::verify_preset`。[`commitPending()` で取っておく所](../../src/core/communication/renderconfigeditor.cpp#L5454))と複数ファイルの commit(`commitRenderConfigCandidateDocuments`)を行い、`applied.committed = true`、`published_generation` を runtime の今の世代にして、成功の経路へ合流します([合流する所](../../src/core/communication/renderconfigeditor.cpp#L5154))。結果に `restart_required` と `restart_reason` を付けるのは最後の 1 か所で、G-4 の `material_route_default_changed` より先に見ます([結果に載せる所](../../src/core/communication/renderconfigeditor.cpp#L5308))。runtime に行く ticket はすべて [`commitPending()`](../../src/core/communication/renderconfigeditor.cpp#L5339) → `apply_candidate` → `completeCommitTransaction()` を通るので、この 1 か所が graph の編集・`add_authored_pass`・restore などのすべてに効きます。`set_layout` だけの ticket(`source_only`)はここを通りません。

live の material を見るのは authored の経路だけです。[`Renderer::applyRenderPipelineAuthoringCandidate()`](../../src/core/vkcore/renderer.cpp#L3698) は `validate_live_materials = true`、ディスクからの [`reloadRenderPipelineFromDisk()`](../../src/core/vkcore/renderer.cpp#L3505) は reload service がいるとき `false` で、material の組み直しは reload service の companion reload(`MaterialContainer::prepareRuntimeGenerationReload()`)に任せます。検査は flat の variant の `validate_prepared_generation` の中にあり、XR の variant の検査と `before_publish`(publish の直前の準備と source commit)はその後です([variant ごとに検査を呼ぶ所](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1381))。だから RS-1 の経路ではそれらは走りません。

**不変条件**: 物理の契約の拒否より後ろで新しい検査を走らせないこと —— RS-1 の経路はそこで止まるので、その検査が見るはずの誤りを抱えた候補が commit されます。足すなら `validateCompatibility()` の中で、`physical_change` を投げる前に。editor で型付きの拒否以外を commit に回さないこと。

**読む場所**: [`materialphysicalcontractchanged.hpp`](../../src/core/material/materialphysicalcontractchanged.hpp#L39) → [`validateCompatibility()`](../../src/core/material/materialgenerationplanner.cpp#L1202) → [`physicalContractRestartReason()`](../../src/core/communication/renderconfigeditor.cpp#L139) → [`completeCommitTransaction()` の RS-1 の枝](../../src/core/communication/renderconfigeditor.cpp#L5154)。利用者から見た結果の形は manual の第13章 §13.5。

---

Pelican の複雑さは、ECS と Vulkan そのものよりも「compile-time 型情報を runtime table へ落とす境界」と「CPU 上の寿命を frame/GPU 上の寿命へ写す境界」に集まっています。その2か所では、便利な macro や RAII の表面だけでなく、登録時刻、pointer の有効期間、barrier、破棄順まで追うのが安全です。
