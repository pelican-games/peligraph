# 設計: codec 境界の例外契約 —— 利用者の入力ミスと内部バグを分け、名前を付け、見える場所へ出す(v1、2026-09-02)

## 0. なぜ今か

WP372d(dry-run)と WP373(型付き resolution error)を中止した裁定が根拠である。**中止の理由は毎回同じ**で、
「型付きエラー」は 1 WP ではなく **codec 契約の設計**だった。fail-fast と named error(エンジンの原則)に
今日の codec 境界は反している。利用者向けエラー・dry-run・Studio の診断は全部ここに乗る。

## 1. 実測(2026-09-02。書く前に数えたもの)

| 事実 | 場所 |
|---|---|
| **型無し `throw std::runtime_error`**: collider **17** / sprite_view **15** / atlas **31** / componentcodec 2。型付きは **0** | [collider.cpp](../src/core/userpublic/components/collider.cpp) / [spriteview.cpp](../src/core/userpublic/components/spriteview.cpp) / [atlasasset.cpp](../src/core/asset/atlasasset.cpp) |
| resolver 本体にも手書き `runtime_error` 4(camera `yfov` / `xmag` / `ymag` / `zfar <= znear`)と `logic_error` 1 が同居 | [resolvedscene.cpp:230](../src/core/loader/resolvedscene.cpp#L230)・:248・:256・:281・:285 |
| resolver の catch は **`catch (const std::exception &)`** で、**`std::logic_error` と `std::bad_alloc` も component error として保持** | [resolvedscene.cpp:191](../src/core/loader/resolvedscene.cpp#L191)・:308 |
| catch の try は **`decodeAuthored()` / `encodeCanonical()` 全体**を覆う(到達範囲は上表全部) | :207 |
| 保持された error は `std::optional<std::string>`。再送出は `std::runtime_error` と文字列連結で**型が失われる** | :46 / [behaviorarena.cpp:164](../src/core/gamelogic/behaviorarena.cpp#L164) |
| **既に code + path を持つ利用者エラー型がある**: `StructFieldValidationError{code, field_path}`、`StructFieldErrorCode` 6 値 | [structfieldjson.hpp:17-36](../src/core/userpublic/details/schema/structfieldjson.hpp#L17) |
| `EditorProjectionTransaction::commit` は **`noexcept` で全 `std::exception` を変換**、edit 層が `method_unavailable` へ潰す | [editorprojectiontransaction.hpp:291](../src/core/loader/editorprojectiontransaction.hpp#L291) / [editorjournal.cpp:1706](../src/core/communication/editorjournal.cpp#L1706) |
| scene query の serialization は `runtime_resolution_error` を**一切出さない** | [editorcommandservice.cpp:635](../src/core/communication/editorcommandservice.cpp#L635) |
| **契約(壊してはならない)**: inactive scene の無効 component は保持され、authoring / save / query は通り、current へ切り替えると失敗 | :302 のコメント、[wp360_resolvedscene_test.cpp:476](../test/wp360_resolvedscene_test.cpp#L476) |
| 例外クラスだけでは data error と内部バグを区別できない(`runtime_error` が両方に使われている) | WP373 レビュー #3 |

## 2. 決定

### 決定 1: 型を **2 つ**に分ける。「利用者データの拒否」と「それ以外」

| 型 | 意味 | 扱い |
|---|---|---|
| **`UserDataError`**(新設。`StructFieldValidationError` を**この派生**にする) | 利用者が書いた JSON / asset が契約に反する。`code`(名前付き、`@1` 付き)+ `json_pointer`(RFC6901)+ leaf `message` | **保持して報告**(inactive 契約)。dry-run の予測対象 |
| それ以外(`std::logic_error` / `std::bad_alloc` / 型無し `runtime_error` / 未知) | 内部 invariant・資源・分類不能 | **再送出**(fail-fast)。component error にしない |

`StructFieldErrorCode` の 6 値は `UserDataError` の code の一部になる(既存の型を捨てない)。

### 決定 2: **移す順序**が本体。「その他を再送出」は**最後**

WP373 が破綻したのは順序が逆だったから。**型無しの利用者エラーが 1 つでも残る間は、catch の「その他」を
再送出に変えてはならない**(inactive 契約が実利用者エラーで壊れる)。

```
E-1  camera(resolver 手書き 4 + alias)/ animation graph を UserDataError へ     ← 小。resolver 内で完結
E-2  collider(17)/ sprite_view(15)を UserDataError へ                            ← 中
E-3  atlas(31)を UserDataError へ                                                ← 中
E-4  ここで初めて catch を「UserDataError だけ保持、他は再送出」にし、logic_error / bad_alloc の握り潰しを終える
E-5  見える場所: query serialization に code + json_pointer + provenance を出す。
     terminal RPC まで typed variant を運ぶ(commit の noexcept 平坦化は UserDataError については解除)
```

各段の受け入れは **「その段で移した投出点の corpus」**: 移す前は文字列でしか出ないものが、
移した後は `code` と `json_pointer` で出て、**inactive scene で保持され current 切替で名前付きで落ちる**
(規約 10 の両側)。E-4 の対照は resolver seam に置く(production composition では下流の平坦化に隠れる —— WP373 レビュー #3。
**E-5 が平坦化を解くまでは seam 対照であることを明記**)。

### 決定 3: 型の中身と provenance

```
UserDataError { code, message(leaf), json_pointer?, scene_id?, authoring_object_id?, component_index?, attachment_index? }
```
- `message` は今日の `what()` と **byte 一致**(既存の文言テストを壊さない)。文脈付き文言は**型から決定的に組み立てる**
- 一覧の規範順 `(scene_id, authoring_object_id, component_index, attachment_index)`。同一 object の同型 behavior 2 つを識別できる
- 例外クラス名ではなく **`code` の名前空間**で分類する(`schema.*` / `camera.*` / `collider.*` / `sprite.*` / `atlas.*`)

### 決定 4: 「見える場所」は **query serialization と terminal RPC の両方**

- `get_components` / `query` の component に `resolution_error {code, json_pointer, message}` を出す(今日は出ない)
- edit の terminal(`get_edit_result`)に typed variant を運ぶ。`EditorProjectionError` に `UserDataError` の variant を足し、
  `errorFromException → projectionFailure → terminal` を保存する。**`method_unavailable` へ潰さない**
- dry-run(U2 と同時)はこの型をそのまま返す。**dry-run 専用の検証経路は作らない**(台帳の恒久裁定)

### 決定 5: 段割り = E-1 〜 E-5。各段が単独で利用者に見える改善を持つ

| 段 | 単独の価値 |
|---|---|
| E-1 | `zfar <= znear` 等が名前と場所つきになる(今日は文字列) |
| E-2 / E-3 | collider / sprite / atlas の 63 投出点が同上 |
| E-4 | **内部バグが利用者の入力ミスに見えなくなる**(fail-fast の回復) |
| E-5 | Studio と RPC 利用者がエラーの場所を**見られる** |

## 3. 受け入れの骨

- 各段: 移した投出点ごとに「入力 → `code` + `json_pointer`」の corpus。文字列に戻す変異で落ちる
- inactive 契約: 無効 component を持つ inactive scene が保存・query でき、current 切替で **`code` つきで**落ちる
- E-4: `logic_error` を起こす対照(resolver seam)が保持されず伝播する
- E-5: 実 RPC で `resolution_error` が component に出る。terminal に `code` が出る(`method_unavailable` なら不合格)
- 全段: 出荷 4 project の bytes 不変、既存文言テスト無変更で緑

## 4. やらないこと

`ResolvedSceneError` / `PrefabError` の再設計(既に名前付き)/ dry-run 本体(U2)/ Studio の UI /
`commit()` の `noexcept` を一般の例外について解くこと(UserDataError に限る)。

## 5. 敵対レビューへの問い

(1) `UserDataError` の派生に `StructFieldValidationError` を組み替えて既存 catch(`catch (const StructFieldValidationError &)`)が壊れないか
(2) E-1〜E-3 の corpus は「資産の実ファイル」から作れるか(atlas 31 は asset loader。resolver 経由で到達するか)
(3) 「その他は再送出」を E-4 まで待つ間、`logic_error` の握り潰しが残る。中間状態の受け入れ方
(4) E-5 の terminal 伝搬は WP375 の seam(plan/materialize)とどう噛むか。順序依存
(5) provenance を型に持たせると、resolver の外(asset loader)で投げる UserDataError は provenance を持てない。誰が付けるか


## 6. 設計レビュー第 1 巡(10 件・破綻 4)と v2

**v2 が本文書の正。§2 決定 1〜5 は v2 で置換される。§1 の実測は次の訂正を受ける。**

訂正: componentcodec の型無し投出は 2 ではなく **3**(`rayInstanceCodecError` が **11 の拒否分岐**から呼ばれる:
[componentcodec.cpp:884](../src/core/loader/componentcodec.cpp#L884))。atlas 31 は静的件数として正しいが、
resolver から実際に到達するのは `SpriteViewComponent::validate → parseSpriteAssetReference` の **5 投出点**だけ。
そして **明示 `throw` 以外に、camera の `value<string>` / collider・sprite の未検査 `get<float>` から
nlohmann の型例外が利用者入力で到達する**([componentcodec.cpp:612](../src/core/loader/componentcodec.cpp#L612))。

### 決定 1'(#2・#8): **既存の例外型を変えない。decode 境界で「値」に変換する**

`StructFieldValidationError` を `UserDataError` の派生にする v1 は 2 重に誤りだった:
- 同じ例外型が **encode 側の invariant 失敗**にも使われる(`encodeAnimation` / `encodeBehaviorParams`:
  [componentcodec.cpp:858](../src/core/loader/componentcodec.cpp#L858))。基底を変えると内部バグが利用者エラーになる
- userpublic の公開ヘッダで **game-logic DLL(ABI v1)が inline canonicalizer からこの型を投げる**
  ([registerer.hpp:228](../src/core/userpublic/details/behavior/registerer.hpp#L228))。基底変更は旧 DLL との ODR 不整合

**決定**: 例外の基底は触らない。**`decodeAuthored` の境界だけ**で既存例外(`StructFieldValidationError`、
nlohmann の型例外、各 codec の `runtime_error`、`rayInstanceCodecError`)を **`UserDataViolation` 値**へ変換する。
encode 側で同じ例外が出たら **invariant failure として伝播**させる(変換しない)。
形は **`decodeAuthored → expected<Value, UserDataViolation>`**。例外クラスで分類しない。

### 決定 2'(#1・#3): **E-0 を置く。中間 carrier は variant**

- **E-0**: `StoredResolutionFailure = variant<UserDataViolation, LegacyResolutionText>`。
  `runtime_resolution_error` の `optional<string>` をこれに替える。**E-4 の完了条件 = `LegacyResolutionText` の
  producer が 0 になること**(そのとき alternative を削除する)
- **棚卸しの単位は「decoder の拒否分岐」**であって `throw` の数ではない。pelican_ray_instance の 11 分岐と
  暗黙の nlohmann 例外を **E-4 より前**に閉じる。E-1〜E-3 の対象表を分岐単位で作り直す
- E-1〜E-3 の各段は、移した分岐の `UserDataViolation` と、未移行分岐の `LegacyResolutionText` が
  **同じ inactive scene に同居して保持される**ことを受け入れにする(第 1 巡 #3 の構成そのもの)

### 決定 3'(#5): fail-fast の回復は **E-4 より先に、本番境界で**

`commit()` が `noexcept` で全例外を結果値に変換する限り、E-4 で resolver の catch を狭めても本番の内部バグは
`method_unavailable` に平坦化される。WP373 裁定「平坦化を直すのが先」と一致する。

- **E-4a(先)**: materialize / commit の結果型に **`InternalInvariantFailure` を載せない**。内部例外は
  変換せず**伝播して停止**する(fail-fast)。`UserDataViolation` だけが結果値になる
- **E-4b(後)**: resolver の catch を「`UserDataViolation` だけ保持」に狭める
- resolver seam で `logic_error` を起こす試験は **unit contract** と明記し、production acceptance に数えない
  (camera の `logic_error` は補完後の invariant で authored 入力から到達しない)

### 決定 4'(#4): 見える境界は **3 つ**

| 境界 | 今日 | 後 |
|---|---|---|
| query(`get_components`) | `runtime_resolution_error` を出さない | `resolution_error {code, source, message}` |
| edit terminal(`get_edit_result`) | `method_unavailable` に潰れる | `UserDataViolation` を variant で運ぶ |
| **`load_scene` と cold start / launcher status** | 数値 application error / ログ文字列で終了 | **構造化した `UserDataViolation`** |

「current 切替で code つきで落ちる」は 3 つ目が無いと実 RPC で成立しない
([rpcserver.cpp:1450](../src/core/communication/rpcserver.cpp#L1450)、[pelican_core.cpp:111](../src/core/userpublic/pelican_core.cpp#L111))。

### 決定 5'(#6): **WP375 を明示的な依存**にし、failure variant と terminal wire を先に確定する

terminal は固定 `EditorEditErrorCode` enum([editorjournal.hpp:46](../src/core/communication/editorjournal.hpp#L46))から
code を生成し、`projectionFailure` は固定 switch で adapter failure を `method_unavailable` にする。
- plan / materialize の **共通 failure variant**(canonicalization / resolver / adapter / internal)を WP375 の
  着地後に固定し、terminal JSON の exact schema(**outer code と payload code のどちらが規範か**)を先に決める
- E-5 はその variant の上に載る。**WP375 の前には E-5 を起票しない**

### 決定 6'(#7): pointer は **token 列で保持**し、`source` を分離する

- throw 時から **token 列**を持ち、RFC6901 の escape は serialization 時に行う(`a/b` → `/params/a~1b`)。
  現行の dotted path(`root + "." + field`: [structfieldjson.hpp:53](../src/core/userpublic/details/schema/structfieldjson.hpp#L53))
  からの変換は非可逆なので、**dotted を作らない**
- `UserDataViolation { code, tokens[], leaf_message, source{kind ∈ {component, prefab_parameter, project_default, asset}, document_id, pointer} }`
- **occurrence provenance(scene_id / object / component index)は別の値**(`DiagnosticOccurrence`)。
  下層(asset loader)は source を持ち、resolver が occurrence を付ける(§5(5) の答え)

### 決定 7'(#9・#10): wire 契約は **分岐ごとの表**と **全 surface の golden bytes**

- **拒否分岐ごとに `{code, tokens, leaf_message bytes}`** を表にする(namespace の列挙ではなく exact)。
  animation / pelican_ray_instance を含める
- 5 surface(direct `what()` / stored leaf / 文脈付き `what()` / query / terminal)を **変更前の golden bytes** と比較する。
  「既存文言テスト無変更」は証明にならない(sprite の大半は `REQUIRE_THROWS` だけ)
- **`DiagnosticOccurrence { violation, provenance, introduced_by_candidate, base_revision }`**。
  dry-run(U2)は base / candidate の差分を返す(WP372d v2 の `preexisting` の後継)

### 決定 8': 段割り(v1 を置換)

```
E-0  StoredResolutionFailure(variant)+ decode 境界の値変換の器(expected)+ 分岐単位の棚卸し表
E-1  camera(4 + alias)/ animation graph / pelican_ray_instance(11)を分岐単位で UserDataViolation へ
E-2  collider(17)/ sprite_view(15、うち resolver 到達 5)
E-3  atlas(resolver 到達 5。残りは asset loader 境界)
E-4a materialize / commit で内部例外を結果値にしない(fail-fast の回復。本番境界)
E-4b resolver の catch を狭め、LegacyResolutionText を削除
E-5  見える境界 3 つ(WP375 着地後。failure variant と terminal wire を先に固定)
```

各段の単独の価値は v1 と同じ。**E-4a は E-4b より先**、**E-5 は WP375 の後**。

### 決定 9': ABI

`StructFieldValidationError` の名前・constructor・`code()/path()` を保持する。**game-logic ABI は上げない**。
旧 DLL が投げた同型例外は core 境界で同じ値変換を受ける(基底に依存しない)。


## 7. 設計レビュー第 2 巡(9 件・破綻 4)と v3 —— **v3 が最終。設計レビューは 3 巡目で打ち切る**

**v3 が本文書の正。§6 の決定 1'〜9' は v3 の修正を受ける。触れていない決定は §6 のまま。**
訂正: 「sprite_view 15、うち resolver 到達 5」は誤記(`decodeSprite` は full parser を呼ぶ)。
resolver 到達 5 は **atlas の参照構文**の話であり、sprite_view 15 は全部 resolver から到達する。

### 決定 1''(#1): Result 型を**自前で 1 つ**定め、`std::any` への暗黙 boxing を禁じる

- core は C++20 で `std::expected` は無い。**`ResolutionResult<T>`** を自前で置く: `[[nodiscard]]`、
  暗黙変換なし、`std::any` へ入れられない(コンストラクタを explicit、`std::any` 構築を `= delete` する強い wrapper)。
  **unwrap 忘れはコンパイルで落ちる**(`ComponentCodecValue` = `std::any` に result が入ると `any_cast` で内部エラーになる穴を塞ぐ)
- **seam は decode ではなく「decode + default 補完」**(resolver 所有)。camera の 4 エラーは decode 後の補完中に出る
  ([resolvedscene.cpp:217](../src/core/loader/resolvedscene.cpp#L217))
- `decodeAuthored` の production caller **6 箇所**(resolver / journal / runtime factory / preview / transaction ×2)を
  E-0 で**全部**列挙し、preview の「戻り値を捨てる」呼び出し([editorpreviewprojection.cpp](../src/core/loader/editorpreviewprojection.cpp))は
  **result を検査する形へ**変える(捨てると不正値を受理する)
- **behavior params の ABI callback(`std::string (*)(json const&)`)は変えない**。core 境界で `StructFieldValidationError` を
  値へ変換する(決定 1' どおり)

### 決定 2''(#2): 遷移中の型を正確に書く。E-0 に単独価値を持たせる

- 保持型は **`std::optional<StoredResolutionFailure>`**、`StoredResolutionFailure = variant<UserDataViolation, LegacyResolutionText>`
  (成功状態は `nullopt`)
- **未移行の拒否は投げ続ける**。resolver の catch が **`LegacyResolutionText` へ変換**する(decode 境界で変換するのは移行済みの
  分岐だけ)。**§6 決定 1' の「decode 境界で既存 runtime_error も値へ変換」は撤回**
- 混在受け入れ(typed と legacy の同居)は **E-1 / E-2 のみ**。**E-3 の受け入れは「producer が 0」**であり、
  E-4b はそれを条件に `LegacyResolutionText` を削除する
- **E-0 = 型 + camera(E-1 相当)+ `get_components` への最小の可視化**を 1 段に束ねる。
  可視化無しの段には単独価値が無い(第 2 巡 (f))

### 決定 3''(#3): **3 分類表**。internal は rollback してから再送出。coordinator の `noexcept` を越えて fail-fast

| 分類 | 例 | transaction | coordinator / frame hook(`noexcept`) |
|---|---|---|---|
| **recoverable**(既存 `EditorProjectionException`、fault 注入 `AfterPublication` 等) | fault matrix | rollback → **失敗結果値**(現行どおり。matrix の `status=="failed"` を保つ) | 結果値 |
| **UserDataViolation** | 利用者の入力 | rollback → **結果値** | 結果値 → terminal |
| **internal**(`logic_error` / `bad_alloc` / 未知) | invariant 破れ | **`catch (...) { rollback; throw; }`**(cleanup してから再送出) | **`noexcept` の中で `std::terminate`** = fail-fast。ログに分類と場所を出してから止まる |

**§4「一般例外について `commit` の `noexcept` を解かない」は撤回**する(v2 と矛盾していた)。
`commit` は `noexcept` を外し、internal は伝播する。既存 fault matrix は recoverable 経路なので**壊れない**(受け入れで確認)。

### 決定 4''(#4): failure variant は **本設計で決める**。WP375 を待たない

WP375 の `plan → EditorProjectionPlan` / `materialize → EditorProjectionResult` は失敗値の型を持たない。**本設計側で二経路を固定**:
- `plan → ResolutionResult<EditorProjectionPlan>`(失敗は `UserDataViolation` のみ)
- `materialize → 既存 domain result(recoverable)+ UserDataViolation`。**internal は rollback 後に throw**(variant に載せない。
  §6 決定 5' の「internal を variant に含める」は撤回)
- terminal JSON: **outer の `EditorEditErrorCode` は不変**(既存 client を壊さない)+ **`error.data.user_data_violation`** を加える。
  規範は `data` 側(outer は互換のため残る)

### 決定 5''(#5): 第 3 境界は **2 つに分ける**。RPC は加算、launcher は handshake

| 境界 | 契約 |
|---|---|
| RPC `load_scene` | outer `code=-32000` と旧 message bytes を**保ち**、`error.data.user_data_violation` を**加える**(JSON-RPC は既に `error.data` を運べる: [jsonrpc.hpp:28](../src/project/jsonrpc.hpp#L28)) |
| cold start / launcher | 終了コード 0/1/-1 を**保ち**、**versioned NDJSON を stderr へ**(または専用 handshake)。Studio は unsolicited JSON を RPC と認めないので、**process failure の経路で読む** |
| Studio | wire DTO / serializer / parser は **`pelican_project`** に置く(D0) |

`requireRuntimeValue()` と collider / light / behavior binder の**文字列再包装も除去**する([resolvedscene.cpp:44](../src/core/loader/resolvedscene.cpp#L44)、[scene.cpp](../src/core/loader/scene.cpp))。

### 決定 6''(#6): atlas 本体は resolver で評価されない。**wire 契約に「未評価」を明記**する

atlas 本体の parse は `SpriteScene::resolve`、呼び出しは ECS system の worker dependency preparation
([spriteviewsystem.cpp:25](../src/core/ecs/predefined/spriteviewsystem.cpp#L25))。**同期 `load_scene` が成功した後に落ち得る**。
- v3 は **同期 preflight を作らない**(publish 前の asset 全走査 + digest cache は別設計)。代わりに
  **query の wire 契約に `asset_evaluated: false`** を持たせ、未評価 asset のエラーは**非同期 diagnostic event**(`resolution_error` と同型)で
  届く、と明記する。**rollback 契約**: 非同期失敗は scene を落とさず、該当 sprite を非表示 + diagnostic
- E-3 の範囲は resolver 到達の 5 分岐 + **非同期 event の型付け**。残り 26 は event 経由で E-3 内に入る

### 決定 7''(#7): ABI v1 DLL からは token が届かない。**lossless 復元 adapter + degraded alternative**

DLL 内 inline canonicalizer が投げる型は `code + field_path`(`root + "." + field`)しか持たず、field 名は `aim.target` や `a/b` を許す
(fixture に実在: [wp361_prefab_test.cpp:130](../test/wp361_prefab_test.cpp#L130))。**naive な dot split は誤り**。
- **v1 adapter**: `params_schema` の field 名と authored key を突き合わせて **lossless に復元**できる場合だけ token 列にする
- 一意に復元できなければ **`LegacySourcePath`**(pointer 無しの degraded alternative)。**pointer を捏造しない**
- ABI v2(token callback)は**必要になったら**追加し v1 と併存。本設計では作らない

### 決定 8''(#8): 合成後の violation は **`primary_source + related_sources[]`**。token 単位の origin map

prefab substitution と default completion を通る **token 単位の origin map** を `PendingComponent` に持たせる
(今は concrete JSON + component-level origin だけ: [resolvedscene.cpp:338](../src/core/loader/resolvedscene.cpp#L338))。
- cross-field violation(`zfar <= znear`)は **`primary_source`**(比較で「後に読まれた側」= 規則を固定)+ `related_sources[]`
- 台帳の実例(default `zfar=1000` / prefab parameter `znear=5000`)を受け入れに使う: primary は `znear` の prefab parameter、
  related に project default の `zfar`

### 決定 9''(#9): golden の受け入れを **surface ごとに分ける**

- **変更前 bytes と一致させるのは `leaf_message` と既存の文脈付き message だけ**。
  atlas の leaf に filesystem path が入る件は、**path を `source.document_id` へ移し leaf からは外す**(leaf は path 非依存)
- query / terminal は **新 schema の新 golden**
- **同一テストの両側**: 不正側で structured error が出る / 修正後に error が消え **実 subsystem が解決した値**が出る。
  result unwrap / resolver wiring / serializer のどれか 1 つを消す変異で落ちる

### 決定 10'': 段割り(最終)

```
E-0  ResolutionResult<T> + optional<StoredResolutionFailure> + 6 caller の列挙と preview の result 検査
     + camera 4+alias / animation graph / pelican_ray_instance 11 の値化 + get_components の最小可視化   ← 単独価値あり
E-1  collider 17 / sprite_view 15(混在受け入れ)
E-2  atlas 5 + 非同期 diagnostic event の型付け(producer 0 が受け入れ)
E-3a 3 分類表の実装: commit の noexcept 解除、internal は rollback → throw、coordinator で terminate
E-3b resolver の catch を狭め LegacyResolutionText を削除
E-4  terminal(`error.data`)/ load_scene(`error.data`)/ launcher handshake / Studio parser(project 層)
     + origin map(決定 8'')+ v1 adapter(決定 7'')
```
**WP375 との関係**: E-3a は WP375 の materialize の失敗経路に触るので **WP375 着地後**。E-0〜E-2 は独立。


## 8. 設計レビュー第 3 巡(9 件)と v4 —— **設計段は 3 巡で打ち切り。以降は E-0a の WP 仕様レビューで受ける**

第 3 巡は不合格だが、**「着手可能にする最小修正」8 項目**を明示した。v4 はその 8 項目を 1:1 で閉じる差分であり、
**これで設計段を終える**(skill §5: 設計レビューは最大 3 巡)。閉じ切れない点は E-0a の仕様レビューへ持ち越す。

### 修正 1(#1): `ResolutionResult<T>` は **move-only**。compile-contract を受け入れに置く

「`std::any` 構築を `= delete`」は方向が逆で強制できない(`std::any::any(T&&)` が選ばれる)。
- **copy constructor / copy assignment を削除**する(move-only)。`std::any` の格納型制約(`is_copy_constructible`)で
  実際に排除される
- E-0a の受け入れに次の **static_assert 4 本**を置く:
  `!is_constructible_v<any, R&>`, `!is_constructible_v<any, R&&>`, `!is_assignable_v<any&, R&>`, `!is_assignable_v<any&, R&&>`
- `[[nodiscard]]` は診断であり `(void)result` を防げない。preview の「捨てる」呼び出しは **production test で result を検査**し、
  `-Werror` 相当(unused-result を error)を E-0a の構成に加える

### 修正 2(#5): **E-0 を分割**。最初の WP は **E-0a = 型 + 6 caller + camera + `get_components`**

| 段 | 内容 |
|---|---|
| **E-0a** | move-only `ResolutionResult` + `optional<StoredResolutionFailure>` + `decodeAuthored` の **6 production caller** 移行(5 ファイル。test の直接 caller 18 も同時)+ camera 4 + alias(resolver 所有の completion seam)+ `get_components` の `resolution_error` |
| E-0b | animation graph + pelican_ray_instance 11 分岐の corpus |
| E-0c | behavior callback の値化(`StructFieldValidationError → UserDataViolation`、当面は `LegacySourcePath` 付き)。**E-3b より前** |

E-0a 単独で「不正 camera が structured error / 修正後に本番 resolver が解決した camera 値」を検査できる。

### 修正 3(#2): 「producer 0」を **検査可能な単位**で定義し直す

- 「producer」= **列挙表に残る `legacy_branch_id`**(decoder の拒否分岐ごとに ID を振った表。E-0a で表を作る)。
  broad な `catch (const std::exception &) → LegacyResolutionText` が残っている間は**「producer 0」と呼ばない**
- E-3b の完了条件 = **表の全 `legacy_branch_id` が typed へ移行済み**、かつ broad catch を狭めた後に
  **internal fault 注入で legacy count が増えず再送出される**こと
- behavior の値化(E-0c)は **E-3b より前**に置く(修正 2 の表どおり)

### 修正 4(#4): plan の失敗型は **2 系統の variant**

`plan` の前段には `UserDataViolation` でも internal でもない recoverable(stale revision、存在しない object への edit、
`prepare()` の `PrefabError`)がある。
```
using ProjectionPlanFailure = std::variant<EditorProjectionRecoverable, UserDataViolation>;
ResolutionResult<EditorProjectionPlan, ProjectionPlanFailure> plan(...);
```
codec 用 `ResolutionResult<T>`(error = `UserDataViolation` 固定)と projection 用は**別の error 型**を取る。internal だけ throw。

### 修正 5(#3): recoverable の **完全な分類表**と **noexcept terminal zone**

- 分類表は「例」ではなく **型 / 値ごと**に 1 件ずつ: `EditorProjectionException`(全 code)、`PrefabError`(全 code)、
  各 adapter が今投げる `runtime_error`(**1 件ずつ列挙して recoverable / internal を判定**)。E-3a の仕様がこの表を持つ
- catch 順: **(1) 列挙された recoverable → rollback + result、(2) `UserDataViolation` → rollback + result、(3) その他 → rollback + rethrow**
- `publish() / rollback() / finish()` の `noexcept` 領域は **「rollback を保証できない terminal zone」**と明記。
  transaction-level internal は rollback 後 throw、coordinator-level は **subprocess death test** で terminate を検査
- `AfterPublication` は同じ matrix で引き続き `Failed` を要求(第 3 巡が両立を確認)
- `bad_alloc` 時のログは **no-allocation logger による best effort** に弱める

### 修正 6(#6): atlas 診断は **sticky state + replay 可能な query**。非同期 event は通知だけ

「`asset_evaluated: false` + event」では Studio 未接続時に event が失われ、query は再取得できない。
- 状態は **`not_applicable | pending | succeeded | failed | disabled`**
- stable key = **`(resolver_generation, scene_revision, scene_id, object_id, component_index, asset document / digest)`**
- **sticky な評価状態と最後の violation を所有する registry** を置き、**`get_resolution_diagnostics(since_sequence)`** で
  polling / replay できる(JSON-RPC notification は**採らない** —— Studio transport は unsolicited JSON を受理しない)
- event は状態変更通知に限定(`schema_version`, `sequence`, stable key, revision, payload, `cleared`)
- scene unload / edit / asset reload 時の **stale 廃棄と purge** を定義
- production acceptance: **失敗した 1 件だけ非表示、他 sprite は描画**
- worker item に scene revision / object / component index を運ぶ配線が要る(現在は `EntityId` と component pointer のみ)
- **これは E-2 の範囲を超える**。**E-2b(診断 registry)として分離**し、E-2 は resolver 到達 5 分岐に限定する

### 修正 7(#7): launcher は **専用 prefix 付き 1 行 JSON を stderr** に固定(二択を決める)

stderr は既存ログの sink なので裸の NDJSON は混ぜられない。**予約 prefix + exact schema** に固定:
```
PELICAN_DIAG {"schema":"pelican.launch_diagnostic","version":1,"run_id":"…","kind":"user_data_violation","violation":{…}}
```
UTF-8 / LF の 1 行、**最大長 64 KiB**、複数 record は sequence 順、partial line は捨てて exit code のみを信頼、
parse failure 時の fallback は「exit code + 生 stderr」。exit code 0/1/-1 は不変。

### 修正 8(#8・#9): v1 adapter と `primary_source` を **byte-exact な規範アルゴリズム**に

**v1 adapter(lossless 復元)**:
1. `path == "params"` → tokens `["params"]`
2. `path` が byte-exact に `"params."` で始まらなければ **degraded**(`LegacySourcePath`)
3. suffix を `params_schema.name ∪ authored object keys` の各文字列と **byte-exact 比較**
4. distinct な token 列が**ちょうど 1 つ**で、legacy path に再 encode して元 bytes と一致するときだけ採用
5. 0 または複数 → `LegacySourcePath`
6. Unicode 正規化・dot split・slash escape・登録順の tie-break は**一切しない**
corpus: `aim.target` / `a/b` / dot を含む unknown key / dot を含む missing field / payload-not-object / synthetic 不正 path

**`primary_source`**: 「後に読まれた側」は撤回。**error code ごとの規範表**にする:
```
camera.zfar_not_greater_than_znear@1   primary_token = znear   related_tokens = [zfar]
```
`related_sources[]` の順序も token 順で固定(実行時の読取順・実装順・登録順から導出しない)。

### 段割り(最終)

```
E-0a  型(move-only)+ 6 caller + camera 4+alias + get_components + legacy_branch_id 表   ← 最初の WP
E-0b  animation + pelican_ray_instance 11
E-0c  behavior callback の値化(LegacySourcePath 付き)+ v1 lossless adapter
E-1   collider 17 / sprite_view 15
E-2   atlas 5(resolver 到達)      E-2b  診断 registry + get_resolution_diagnostics(別設計に近い規模)
E-3a  3 分類表の実装(commit の noexcept 解除、terminal zone、死活 test)   ← WP375 着地後
E-3b  broad catch を狭め LegacyResolutionText 削除(producer 表が空になってから)
E-4   terminal error.data / load_scene error.data / launcher PELICAN_DIAG / Studio parser(project 層)/ origin map
```
