# EventPayloadSchema(v2)

対象読者: イベント層・UI・エディタ(D0)担当。
ステータス: v2 — **条件付き Accept**(2026-07-11 round 3、
`docs/design_reviews/2026-07-11_color_ui_v6_rereview_codex.md` §3。
条件 E-C1〜E-C6 は **WP71** の受け入れ基準に添付済み —
`implementation_plan.md` WP71 が正)。
v2 の骨子: **schema の正本は default instance の `ref()` 実行結果ではなく、
イベント型が宣言する explicit static descriptor**。記録アーカイブは
debug の一致検査へ降格し、static init 中のイベント構築は行わない。
**E-C5 採用決定: Opaque の by-name emit は廃止**(typed C++ emit のみ —
§2 の 3 状態表の「従来どおり可」は WP71 で置き換わる)。
前提: イベント層 E1、`design_ui_2d_foundation.md` v6 §3。

## 0. 現状と目的

現行 `EventTypeRegistration` は `name` / `type_index` / `load_json_payload`
関数ポインタのみを持ち(`src/core/userpublic/details/event/registerer.hpp:34-38`)、
field 名・型・required の**事前列挙ができない**。UI の semantic emit
(v6 §3)・将来の rpc/エディタ補完(D0)は、**emit せずに** field 名・型・
制約を照会できる正本を要求する。

v1(記録アーカイブ正本)を撤回した理由(round 2 §3.1〜3.3):
①二重実行は「同じ入力で同じ列が出た」ことしか証明せず、決定的分岐・
`if constexpr`・重複 prop を検出できない ②static initializer 中に
constructor/`ref()` を実行すると初期化順 hazard と副作用を新規に持ち込む
③PropMeta 付き 3 引数 `prop` は既存の JsonArchiveSaver / BinaryArchive を
コンパイル不能にする。

## 1. データモデル

```cpp
// 型は JsonArchiveLoader の prop overload 集合と 1:1(jsonarchive.hpp:13-27)
enum class PayloadFieldType : uint8_t {
    I8, I16, I32, I64, U8, U16, U32, U64,
    F32, F64, Vec2, Vec3, Vec4, Quat, String,
};

// range は型付き variant — double へ潰さない(int64/uint64 の 2^53 超を保全)
using PayloadRange = std::variant<
    std::monostate,                          // range なし
    std::pair<int64_t, int64_t>,             // 符号付き整数
    std::pair<uint64_t, uint64_t>,           // 符号なし整数
    std::pair<double, double>>;              // F32/F64/VecN(VecN は各成分に適用)

struct PayloadFieldSchema {
    std::string_view name;      // descriptor 由来の静的文字列
    PayloadFieldType type;
    PayloadRange range;         // Quat/String は monostate 固定
    std::string_view unit;      // 空 = 無次元。表示/lint 用
    // required: v1 実装では全 field 必須(loader が .at() — 現実装と一致)。
    // optional は将来 prop_opt 導入時に bool を追加
};

struct EventPayloadSchema {
    std::span<const PayloadFieldSchema> fields;  // 宣言順
    bool unknown_fields_reject = true;           // §3 — 全 by-name 経路共通
};
```

## 2. 正本 = explicit static descriptor(構築・ref 実行なし)

イベント型自身が **constexpr descriptor** を宣言する。member pointer から
型を導出するため、**C++ の field 型と schema 型のドリフトは構造的に
起きない**(名前と range だけが人手):

```cpp
struct MenuOpened {
    std::string source;
    int32_t amount = 0;
    vec2 delta{};

    void ref(auto &ar) {
        ar.prop("source", source);
        ar.prop("amount", amount);
        ar.prop("delta", delta);
    }

    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&MenuOpened::source>("source"),
        Pelican::field<&MenuOpened::amount>("amount", Pelican::irange(0, 99)),
        Pelican::field<&MenuOpened::delta>("delta"));
};
```

- `Pelican::field<&T::member>(name, range?, unit?)` は member pointer の
  型から `PayloadFieldType` を **コンパイル時に**導出する。§1 の enum に
  ない型(bool・ネスト struct 等)を渡すと **static_assert で失敗**
  (現 loader に `bool` overload は無い — 対応型集合は loader と常に同一)
- `registerEvent<Type>` は `requires { Type::pelican_payload; }` で検出し、
  `EventTypeRegistration` に schema を載せる。**default instance の構築も
  `ref()` の実行も一切しない**(static init は現行どおり関数ポインタ
  組み立てのみ — 副作用ゼロを維持)
- descriptor の検証(名前重複・空名・min>max・NaN/Inf range・型に不適合な
  range 種別)は constexpr 評価でコンパイルエラーにする(可能な範囲)+
  エンジン init phase の registry 検証で二重化

3 状態分類(v1 から変更なし、判定源のみ変更):

| イベント型 | 分類 |
|-----------|------|
| `pelican_payload` 宣言あり | **Typed**(fields 列挙済み) |
| 宣言なし・`ref` なし・default 構築可 | **Payloadless**(fields 0 個を明示) |
| 宣言なし・`ref` あり | **Opaque** — **by-name emit 不可(E-C5)**・UI バインド不可(binding 自体を `no_payload_event` で拒否)。typed C++ emit のみ |

serializable なのに descriptor を書き忘れた型は Opaque になる(UI で使おうと
した時点で `no_payload_event` — 暗黙に何かを推測しない)。

## 3. 検証パイプライン(archive 変更ゼロ)

by-name emit(rpc / UI / 将来の replay)の JSON payload は、
**`ref()` を実行する前に** descriptor で検証する:

1. unknown field(descriptor に無い key)→ 拒否(`unknown_fields_reject` は
   UI 経由に限らず**全 by-name 経路**で同一 — rpc も同じエラー)
2. required 欠落 / JSON 型不一致(表現可能範囲チェック含む: 例 I8 に 300、
   U32 に負数、2^53 超の JSON number は I64/U64 でも入力エラー)→ 拒否
3. range 逸脱 → 拒否
4. 検証通過後、従来の `load_json_payload`(default 構築 + `ref()`)を
   **無変更で**実行

`prop` のシグネチャ・Archive concept・JsonArchiveSaver・BinaryArchive は
**一切変更しない**(v1 の PropMeta 案を撤回 — range/unit は descriptor 側)。

## 4. debug 一致検査(記録アーカイブの降格先)

descriptor と `ref()` のずれ(field の追加し忘れ・順序変更・名前 typo)は
**エンジン init phase**(static init ではない・明示呼出・debug ビルドのみ)で
検出する:

- 記録アーカイブ(`prop` 呼びを記録するだけの Archive)を default 構築
  インスタンスに 1 回通し、**名前列(順序・重複込み)**を descriptor と照合。
  不一致 = 起動エラー(型名・field 名入り)
- これは**補助検査**である: 条件分岐 `ref` は原理的に完全検出できない
  (round 2 の受理)。正本は descriptor なので、検査をすり抜けた分岐 ref が
  壊すのは load 挙動であり schema 消費側(UI/エディタ)ではない。
  分岐 ref + descriptor 宣言の組は「契約違反(未定義)」と文書化し、
  検査は最善努力の early warning と位置づける
- この phase 実行の契約: Typed イベントの default constructor は
  副作用なし・noexcept を要求(違反はレビュー対象)。release ビルドでは
  実行しない(構築ゼロ)

## 5. 公開 API(消費側の判定を完全にする)

```cpp
struct EventSchemaLookup {
    enum class State { UnknownEvent, Payloadless, Opaque, Typed };
    State state;
    const EventPayloadSchema *schema;  // Typed のとき非 null(Payloadless は
                                       // 空 fields の静的 schema を返す)
};
EventSchemaLookup findEventSchema(std::string_view name) const;
```

nullptr 1 本だった v1 API を撤回。UI は `UnknownEvent → unknown_event`、
`Opaque / Payloadless への fields 指定 → no_payload_event` を**この戻り値
だけで**判定できる(UI v6 §3・§7 の error code と 1:1)。

## 6. テスト(WP 受け入れ基準)

1. 代表イベント(全 15 型 + 各 range 種別 + unit)の descriptor 列挙が
   宣言順・型・range を正しく返す
2. 4 状態(Typed / Payloadless / Opaque / Unknown)の lookup 分類
3. **static init 中にイベント構築が起きない**こと(counting constructor で
   検証 — 登録だけでは構築数 0)
4. debug 一致検査: field 追加し忘れ / 順序違い / 重複 prop の 3 種が
   init phase で検出される(条件分岐 ref の限界はテスト名で明示)
5. 検証パイプライン: unknown field / required 欠落 / JSON 型不一致
   (I8 範囲・U32 負数・2^53 超)/ range 逸脱 / Quat・String に range 宣言
   (コンパイルエラー fixture)
6. rpc by-name emit でも同一エラー(UI 専用でないこと)
7. 非対応型 field の static_assert(compile-fail fixture)
8. UI v6 §3 の invalid ①〜⑦ の判定材料提供(UI 側 WP と分担)

## 7. WP 化

- 規模: 小〜中(イベント層 + descriptor ヘルパ + init phase 検査 + テスト。
  **既存 emit 経路・serialize 層は不変**)
- 依存: なし(E1 済)。**U0 の前提**(UI v6 §9)— 本設計の Accept が先行条件
- 排他: `src/core/userpublic/details/event/`(serialize には触れない)
