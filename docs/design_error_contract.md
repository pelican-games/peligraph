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
