# EventPayloadSchema(v1)

対象読者: イベント層・UI・エディタ(D0)担当。
ステータス: v1 ドラフト(2026-07-11。UI v5 U-B3 の依存として新設 —
「`PELICAN_REGISTER_EVENT` が payload schema を持つ」という v4 の前提は
事実誤認であり、本書がその schema を**明示的に**導入する)。
前提: イベント層 E1、`design_ui_2d_foundation.md` v5 §3。

## 0. 現状と目的

現行 `EventTypeRegistration` は `name` / `type_index` / `load_json_payload`
関数ポインタのみを持ち(`src/core/userpublic/details/event/registerer.hpp:34-38`)、
field 名・型・required の**事前列挙ができない**。`JsonArchiveLoader::prop` は
JSON の `.at(name)` をその場で型変換するだけ
(`src/core/userpublic/serialize/jsonarchive.cpp`)。

UI の semantic emit(v5 §3)・将来の rpc/エディタのイベント補完(D0)は、
**emit せずに** field 名・型・制約を照会できる正本を要求する。

## 1. データモデル

```cpp
enum class PayloadFieldType { Bool, Int, Float, Vec2, Vec3, Vec4, String };

struct PayloadFieldSchema {
    std::string name;            // ref() の prop 名(1 段 — ネストは v2 予約)
    PayloadFieldType type;
    bool required = true;        // v1 は全 field 必須(§2)
    std::optional<double> min, max;  // 数値型のみ
    std::string unit;            // 空 = 無次元。表示/lint 用(変換はしない)
};

struct EventPayloadSchema {
    std::vector<PayloadFieldSchema> fields;  // ref() の宣言順
};
```

`EventTypeRegistration` に `std::optional<EventPayloadSchema> payload_schema`
を追加する。3 状態:

| イベント型 | payload_schema |
|-----------|----------------|
| serializable(`ref` あり・全 field が §1 の型) | fields 列挙済み schema |
| default 構築のみ(`ref` なし) | **空 schema**(fields 0 個)— 「payload なし」を明示 |
| `ref` はあるが §1 外の型を含む | `std::nullopt` = **opaque** — by-name emit は従来どおり可・UI バインドは不可(`no_payload_event` エラー) |

公開 API: `const EventPayloadSchema *findEventSchema(std::string_view name)`
(UserEventRegisterer 上。UI モジュール・rpc・エディタが使用)。

## 2. schema の取得 = 記録アーカイブ(暗黙 reflection ではなく規約付き機構)

`PELICAN_REGISTER_EVENT` のシグネチャは変えない。`registerEvent<Type>` が
登録時に **`SchemaArchiveBuilder`**(Archive concept を満たす記録専用
アーカイブ)を default 構築インスタンスの `ref()` に 1 回通し、`prop()` 呼出
列から fields を構築する。

これが成立するための**規約(normative)**:

1. schema を持つイベントの `ref()` は**無条件・決定的な field 列挙**で
   なければならない(分岐・ループ・入力依存の prop 禁止)。
   これは「ref から何でも reflection できる」という主張ではなく、
   schema 対象イベントに課す**契約**である
2. 契約の機械検査(debug ビルド):
   - 登録時に `ref()` を 2 回走らせ、prop 列が一致しなければ assert
   - `load_json_payload` 実行時、アクセスした prop 名集合 ≡ schema fields を
     照合(JsonArchiveLoader に検査フック)— ずれたら assert
     (release では従来どおりゼロコスト)
3. range/unit は prop の**明示メタデータ**で宣言する(推測しない):

   ```cpp
   void ref(auto &ar) {
       ar.prop("amount", amount, Pelican::PropMeta{.min = 0, .max = 99});
       ar.prop("delta", delta);
   }
   ```

   `PropMeta` 付き overload を Archive concept に追加。JsonArchiveLoader は
   これを **range 検証に使い**(逸脱 = ロードエラー — UI 経由でない by-name
   emit でも同じ検証)、SchemaArchiveBuilder は記録する。既存 `prop` 2 引数は
   そのまま(メタなし)

required/default: v1 は**全 field 必須**(JsonArchiveLoader が `.at()` で
落ちる現実装と一致)。optional field は `prop_opt(name, value, default)` を
v2 で予約 — 導入時に schema の `required=false`/`default` が意味を持つ。

## 3. 消費側の規則(UI v5 §3 と対)

- UI ロード時: emit 定義の各 field を schema と突き合わせる。
  型不一致 / required 欠落 / schema 外 field(unknown-field 拒否)/
  opaque・payload なしイベントへの fields 指定 — すべて**名前入り
  ロードエラー**(error code は UI v5 §7 の closed enum)
- rpc / by-name emit: 従来どおり `load_json_payload` で構築。PropMeta の
  range 検証が加わる以外は挙動不変(schema は追加情報であり、emit 経路の
  互換を壊さない)

## 4. テスト(WP 受け入れ基準)

1. 代表イベント(全 field 型 + PropMeta)で schema が宣言順・型・range を
   正しく列挙する
2. 3 状態(schema あり / 空 / opaque)の分類が正しい
3. ドリフト検査: 条件分岐 ref のイベントが debug assert で検出される
4. range 逸脱 payload の by-name emit がロードエラーになる
5. `findEventSchema` の未知名 → nullptr
6. UI v5 §3 の invalid ①〜⑦ に対応するロードエラー fixture(UI 側 WP と分担
   — 本 WP は ④range・⑥unknown-field・⑦no_payload の判定材料を提供)

## 5. WP 化

- 規模: 小(イベント層 + serialize 拡張 + テスト。既存 emit 経路は不変)
- 依存: なし(E1 済)。**U0 の前提**(UI v5 §9)
- 排他: `src/core/userpublic/details/event/` / `src/core/userpublic/serialize/`
