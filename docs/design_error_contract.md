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
- **棚卸しの単位は「decoder の拒否分岐」**であって `throw` の数ではない。ray_instance の 11 分岐と
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
  animation / ray_instance を含める
- 5 surface(direct `what()` / stored leaf / 文脈付き `what()` / query / terminal)を **変更前の golden bytes** と比較する。
  「既存文言テスト無変更」は証明にならない(sprite の大半は `REQUIRE_THROWS` だけ)
- **`DiagnosticOccurrence { violation, provenance, introduced_by_candidate, base_revision }`**。
  dry-run(U2)は base / candidate の差分を返す(WP372d v2 の `preexisting` の後継)

### 決定 8': 段割り(v1 を置換)

```
E-0  StoredResolutionFailure(variant)+ decode 境界の値変換の器(expected)+ 分岐単位の棚卸し表
E-1  camera(4 + alias)/ animation graph / ray_instance(11)を分岐単位で UserDataViolation へ
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
