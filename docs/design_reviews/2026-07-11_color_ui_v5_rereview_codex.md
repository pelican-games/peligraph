# 色パイプライン v2 / UI 基盤 v5 / EventPayloadSchema v1 敵対的再レビュー (round 2)

## 冒頭判定

- `docs/design_color_pipeline.md` v2: **Reject**
- `docs/design_ui_2d_foundation.md` v5: **Reject**
- `docs/design_event_payload_schema.md` v1: **Reject**

前版からの前進は大きい。色 v2 は working space、RGB/alpha、tone curve と OETF、
terminal resource、dual-use image、capture 契約、移行時の三者比較を初めて一つの設計にした。
UI v5 は負値 round と negative remaining の文章上の矛盾を直し、draft 2020-12 の実 schema と
fixture を置いた。EventPayloadSchema も、現 registry に reflection が無いという事実誤認を撤回した。

それでも開始ゲートを開けられない理由は三つである。

1. 色 v2 は、SRGB view を作れない dual-use image を「警告して誤色のまま続行」し、冒頭の
   linear shader invariant 自身を破る。また SRGB 中間 RT に必要な sampled/filter/blend capability と、
   HDR 時に project-defined bloom RT 全体を 16F 化する graph rewrite が閉じていない。
2. UI の `floor(x + 0.5)` は、規範を「全実数の式」と「各演算を binary64 で丸める」のどちらに
   するかで半境界直下の結果が変わる。semantic schema は構造を閉じたが、逆転 rect、重複 id、
   非単調 `event_seq`、非 quad index count 等を valid とする。
3. EventPayloadSchema は explicit schema を登録するのでなく、依然として default instance の
   `ref()` 実行結果を正本にする。二重実行と observed-load 比較では「無条件 field 列挙」を証明
   できず、static registration に constructor/ref の副作用も新しく持ち込む。

従って C1、UI U0、EventPayloadSchema WP はいずれも着手不可である。以下の blocker を設計へ
反映した後に再審査すること。C0 の read-only 監査だけは引き続き先行してよい。

---

# 1. 色パイプライン v2

## 1.1 前回 C1〜C7 の解消照合

| 前回指摘 | 判定 | round 2 の照合 |
|---|---|---|
| C1 tone curve / OETF 所有点 | **部分解消** | 四経路と回数を表にした点は正しい (`docs/design_color_pipeline.md:98-117`)。ただし intermediate SRGB encode/decode を回数から除外しており、HDR graph を既存 example/golden に適用する規則も未完成。 |
| C2 authored color / alpha / HDR 値 | **解消** | linear-sRGB/Rec.709/D65、RGB のみ transfer、alpha linear、encoding×role、glTF multiplier 不変、radiometric unclamped を明記した (`docs/design_color_pipeline.md:10-24`, `docs/design_color_pipeline.md:143-166`)。 |
| C3 同一 image 二用途・format/capability | **部分解消** | mutable image + SRGB/UNORM view は成立する方向 (`docs/design_color_pipeline.md:168-182`)。しかし非対応時に誤色を許し、RT capability の必要集合も不足。 |
| C4 terminal graph rewrite | **部分解消** | `display` と purge 不可 `output_transform` は前回条件への正しい回答 (`docs/design_color_pipeline.md:62-88`)。一方、既存 anchor/resource と conditional RT rewrite の具体規則が無い。 |
| C5 blend / bloom / additive | **部分解消** | linear blend、HDR bloom は tonemap 前、threshold 単位、analytic fixture を決めた (`docs/design_color_pipeline.md:184-204`, `docs/design_color_pipeline.md:251-268`)。8-bit SRGB multipass の変換回数と誤差 budget は未定義。 |
| C6 capture / RPC | **部分解消** | encoding/alpha/order/PNG metadata/status/RPC known-value を契約化した (`docs/design_color_pipeline.md:206-221`)。ただし「versioned」と称しながら API version が無く、capture source も `display` と final target の二通りに読める。 |
| C7 安全な再基準化 | **解消** | manifest、同一環境三者比較、case ごとの承認、既存 tolerance 維持、analytic fixture、frame-plan trace の五条件を全て入れた (`docs/design_color_pipeline.md:223-249`)。 |

## 1.2 [Blocker] dual-use fallback が色 invariant を意図的に破る

設計の大原則は「shader I/O は常に linear、decode/encode は SRGB view 又は terminal pass だけ」
である (`docs/design_color_pipeline.md:16-24`)。ところが mutable-format strategy が使えない場合、
color slot を UNORM view のままサンプルし、warning だけで継続するとしている
(`docs/design_color_pipeline.md:172-180`)。これは劣化の程度の問題ではない。188 byte の color texel が
linear 0.5 でなく 188/255 として shader に入り、同じ文書が要求する dual-use fixture
(`docs/design_color_pipeline.md:258-260`)を必ず失敗させる。

前回条件は compatible view、用途別複製、又は asset reject のいずれかであった。正しさを失う
warning fallback はそのどれでもない。SRGB view を作れないなら、(a) image 複製、(b) 明示 decode
variant、(c) load error のいずれかにし、正常終了した全経路で linear invariant を守る必要がある。

同様に `display` の capability query は `COLOR_ATTACHMENT` しか列挙していない
(`docs/design_color_pipeline.md:90-96`)。実際の用途には少なくとも sampled image
(`output_transform` が読む)、color attachment blend (overlay が blend する)、bloom の linear filter、
capture 元にするなら transfer source が要る。SRGB attachment 対応と SRGB attachment **blend** 対応は
同じ条件ではない。必要 feature bit の完全な集合、非対応時の正しい fallback 又は明示失敗を
resource role ごとに表にすること。

## 1.3 [Blocker] HDR graph rewrite は既存 example と既存 hdr golden の両方をまだ規定していない

本文の現状説明は、`hdr_tonemap` が example の bloom/UI を上書きするとする
(`docs/design_color_pipeline.md:37-43`)。しかし同梱 feature の実体は `before:present`
(`src/core/resources/features/hdr.json:10-28`)であり、example の末尾は
`FinalBloomComposite` と `ui_pass` で `present` という pass が無い
(`projects/example/passes/main_rendering_config.json:297-316`)。現状の直接の結果は overlay 消失以前に
**anchor not found で compose 失敗**である。現状認識をまず訂正すべきである。

目標側も「bloom 合成後・overlay 前の明示位置」としか書かず
(`docs/design_color_pipeline.md:109-117`)、どの予約 anchor/resource がその位置を表すか決めていない。
別正本は `post_main → tonemap → post_ldr → swapchain` を標準名としている
(`docs/design_postprocess_temporal.md:69-90`)一方、UI v5 は
`pelican_ui → debug_text → imgui → present` を予約列にする
(`docs/design_ui_2d_foundation.md:339-347`)。C1 はこの二つと `display/output_transform` を一つの
canonical frame-plan schema に統合しなければ実装者ごとに違う graph になる。

さらに HDR feature が現在 override するのは `lit_color` 一個だけ
(`src/core/resources/features/hdr.json:5-8`)であるのに、v2 は project-defined bloom RT 全てを HDR on で
RGBA16F にすると決めた (`docs/design_color_pipeline.md:122-140`)。名前列挙で override するのか、
resource role から format class を派生させるのか、feature compose 後に target graph を複製するのかが
未決である。これは「tonemap pass の移設」だけではなく RT 宣言、pipeline compatibility、descriptor、
barrier、trace fixture を横断する rewrite であり、C1 を単一小変更と見積もってはならない。

既存 `hdr_on` golden も十分な証拠ではない。生成 graph は `hdr_source → copy_to_swapchain → present`
で (`test/golden_image_test.cpp:843-885`)、bloom chain も実描画 overlay も持たない。新しい gate は
example 相当の bloom + UI/debug を含み、HDR on で bloom が tonemap 前、overlay が tonemap 後、
output transform が最後の一個であることを trace と pixel の両方で検査する必要がある。

## 1.4 [Blocker] 「encode 1 回」と 8-bit SRGB multipass の実体が一致しない

文書は各表示経路で display encode がちょうど一回とする
(`docs/design_color_pipeline.md:22-24`, `docs/design_color_pipeline.md:102-107`)。しかし hdr off では
`lit_color`、bloom RT 群、`display` が全て SRGB attachment である
(`docs/design_color_pipeline.md:118-120`, `docs/design_color_pipeline.md:124-140`)。各 fullscreen pass は
入力で HW decode、出力で HW encode、8-bit quantize を繰り返し、最後に `display` を再度 decode して
target へ encode する。net の表示 transfer は一回でも、storage transfer/quantization は一回ではない。

この区別が trace schema に無いまま「OETF 一個」を gate にすると、不正な shader OETF と正当な
paired SRGB storage encode を機械的に区別できない。各 resource edge に
`linear value → SRGB storage → linear sample` の paired conversion を記録し、terminal encode とは
別カウンタにすること。また現 bloom は複数の downsample/upsample/composite pass を通るため
(`projects/example/passes/main_rendering_config.json:58-109`,
`projects/example/passes/main_rendering_config.json:270-305`)、round-trip ごとの解析値、最大/平均誤差、
許容 pass 数を fixture にすること。`±0` の SRGB-vs-shader fallback parity
(`docs/design_color_pipeline.md:253-268`)を要求するなら、HW conversion と shader OETF の丸めを
byte-exact に一致させられることも先に実証しなければならない。

## 1.5 [Major] debug color API と capture の互換境界を version と型で表すべき

debug API を sRGB input に変える決定は、単独の見た目を維持しやすい合理的な選択である
(`docs/design_color_pipeline.md:131-136`)。ただし現在の API は無印の `glm::vec4` で
(`src/core/renderer/debugdraw.hpp:40-46`, `src/core/renderer/debugtext.hpp:78-83`)、linear 計算結果を渡す
既存 caller と authored literal を区別できない。現リポジトリにも 0/1 以外の caller がある
(`src/core/phys/physworld.cpp:21-24`, `src/core/phys/physworld.cpp:103-109`)。`SrgbColor` / `LinearColor`、
又は明示名 overload と deprecation/migration test を用意し、無印 API の意味を黙って変えないこと。

capture は一方で `display` を読むとし (`docs/design_color_pipeline.md:85-88`)、他方で headless final
offscreen の readback と書く (`docs/design_color_pipeline.md:90-96`)。両者が byte-identical であることを
契約にするのか、capture point を一つに固定するのかを決めるべきである。また “versioned contract”
なら、RPC の追加 metadata だけでなく C++/CLI の contract version と旧 consumer の移行記録を持つこと
(`docs/design_color_pipeline.md:206-221`)。

## 1.6 §1.3 の 8 再審査条件

| 条件 | 判定 | 根拠 |
|---|---|---|
| 1. hdr off/on の tone curve/OETF pass 列 | **部分** | 表は追加されたが、実 anchor/conditional RT rewrite と intermediate transfer count が未確定。 |
| 2. primaries、RGB/alpha、field role/range | **解消** | `docs/design_color_pipeline.md:10-24`, `docs/design_color_pipeline.md:143-166`。 |
| 3. dual-use image と全 texture/RT capability | **部分** | view 戦略はあるが誤色 fallback と capability 欠落がある。 |
| 4. feature 合成後 terminal rewrite | **部分** | `display/output_transform` は正しいが canonical anchors と非対応 device 経路が未完。 |
| 5. bloom/transparent/UI/additive の意味と fixture | **部分** | 意味と fixture 名はあるが SRGB multipass error budget と実 HDR graph fixture が無い。 |
| 6. capture/RPC contract と絶対値 test | **部分** | encoding は固定したが capture point と version 境界が曖昧。 |
| 7. manifest/三者比較/case 承認 | **解消** | `docs/design_color_pipeline.md:223-245`。 |
| 8. 非ゼロ tolerance の正直な扱い | **解消** | 既存二 case を維持し「全 exact」を撤回 (`docs/design_color_pipeline.md:238-242`)。 |

色 v2 の再審査条件は、1/3/4/5/6 がまだ部分である。従って C1 は開始不可。

---

# 2. UI 基盤 v5 と semantic schema

## 2.1 U-B2〜U-B4 の解消照合

| 前回指摘 | 判定 | round 2 の照合 |
|---|---|---|
| U-B2 round / negative remaining | **部分解消** | 負値 test vector、binary64 の積和順、FMA 禁止、min 優先 overflow rect は明文化した (`docs/design_ui_2d_foundation.md:251-291`)。ただし half-up 式と binary64 実装が境界直下で矛盾し、整数 overflow 規則も無い。 |
| U-B3 payload schema | **未解消** | UI が依存 WP を明示した構造は正しい (`docs/design_ui_2d_foundation.md:198-229`, `docs/design_ui_2d_foundation.md:472-484`)。しかし参照先 EventPayloadSchema v1 が本レビューの blocker を持つ。 |
| U-B4 closed semantic fixture | **部分解消** | object shape、oneOf、additionalProperties、範囲、実 fixture は成立し AJV 10/10。ただし semantic invariant が schema/gate の外で、enum coverage の主張も満たさない。 |

## 2.2 [Blocker] `floor(x+0.5)` の数学規範と binary64 評価規範が一致しない

本文は `round_half_up(x) = floor(x + 0.5)` を「全実数でこの一式」としつつ、anchor の積と和を
binary64 で各一回丸めるとする (`docs/design_ui_2d_foundation.md:254-270`)。しかし representable な
`x = 0.49999999999999994` (0.5 の直前の binary64)では、実数としての `x + 0.5` は 1 未満なので
floor は 0、一方 binary64 の加算結果は 1.0 に丸まり、素直な `floor(x + 0.5)` 実装は 1 を返す。
`parent_size=1, anchor=0.49999999999999994, parent_edge=0` だけで到達できる反例である。

従って次のどちらかを正本に固定する必要がある。

1. **binary64 operation semantics が正**: `+0.5` も含め RN-even binary64 一回と明記し、上の入力は
   1 が normative とする。JSON decimal→binary64 の correctly-rounded 要件、FP rounding mode、
   fast-math/excess precision 禁止も gate にする。
2. **binary64 値に対する数学的 half-up が正**: `floor(x)` と fractional comparison 等で、
   `x+0.5` の丸め上がりを避ける規範 algorithm を示す。上の入力は 0 が normative。

どちらでも `nextafter(±0.5, ±∞/0)`、大きな parent edge、`ui_scale` の同型境界を test vector に加えること。
また canonical rect は schema 上 int32 なのに (`docs/schemas/pelican.ui_semantic_fixture.schema.json:184-200`)、
layout の中間整数型、加減算 overflow、最終 int32 範囲外時の error が未定義である。負 remaining を
一本化しただけでは overflow algorithm はまだ完全ではない。

## 2.3 schema の良い点と AJV 再現

`input_trace` の `oneOf` は kind が互いに disjoint であり、`pointer_move` の button と
`pointer_cancel` の position/target/button は `additionalProperties:false` により実際に拒否される
(`docs/schemas/pelican.ui_semantic_fixture.schema.json:70-117`)。lifecycle も kind ごとに閉じている
(`docs/schemas/pelican.ui_semantic_fixture.schema.json:119-166`)。root を含む全 object の
`additionalProperties:false`、error code/path、数値上限も実在する。この部分は前回から明確な改善である。

AJV **8.20.0**、`Ajv2020({strict:true, allErrors:true})` で正本 schema を compile し、
`valid/` 4 件は valid、`invalid/` 6 件は invalid、合計 **10/10** を再現した。従って提出された
「10 fixture の期待分類」は真である。

## 2.4 [Blocker] closed “semantic” schema ではなく closed structural schema に留まる

同じ validator で valid fixture を一箇所だけ変えた次の反例は、全て **schema-valid** になった。

- `rect_ui: [160,56,40,24]` (left>right / top>bottom)
- `draw_runs[0].index_count = 5` (quad index 列でない)
- 同じ widget object/id を二回入れる
- 後続 `event_seq` を 11 から 9 に戻す
- `effects: ["hover_enter", "hover_enter"]`
- `texture: "atlas:../secret/page:0"`

原因は、rect が int32×4 しか要求しない
(`docs/schemas/pelican.ui_semantic_fixture.schema.json:195-200`)、widgets に id uniqueness がない
(`docs/schemas/pelican.ui_semantic_fixture.schema.json:36-51`)、draw run は単なる非負整数で
index_count の 6 の倍数性や run の連続性がない
(`docs/schemas/pelican.ui_semantic_fixture.schema.json:53-68`)、trace 配列に strict monotonicity がない
(`docs/schemas/pelican.ui_semantic_fixture.schema.json:70-118`)、effect list に `uniqueItems:true` がない
(`docs/schemas/pelican.ui_semantic_fixture.schema.json:214-218`)ためである。

cross-field/order 制約を JSON Schema だけで全て表せないこと自体は問題ではない。問題は、正本を
この schema と呼び、U0 gate を schema validation + expected との semantic equality の二段だけにした
こと (`docs/design_ui_2d_foundation.md:395-452`)である。同じ不正値を implementation と expected の
両方が出せば equality は通る。`pelican.ui_semantic_fixture` 用の normative semantic validator を
第三の gate として定義し、少なくとも rect ordering、content rect containment、stable-id uniqueness、
decl_seq/event_seq ordering、run index continuity/multiple-of-6、effect/consumed uniqueness、texture name の
path traversal 禁止を検査すべきである。又は名称を structural schema に下げ、別の invariant 正本を置くこと。

## 2.5 [Major] normative fixture は各 enum variant / invalid class を覆っていない

input fixture は left/right だけで `button:middle` が無い
(`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:51-86`)。lifecycle は
`capture_cancel.reason` の 7 値中 `reload` 一個、`command_dropped.reason` の 3 値中
`reload_swap` 一個だけである
(`test/fixtures/ui_semantic/normative/valid/lifecycle_variants.json:25-50`、enum の正本は
`docs/schemas/pelican.ui_semantic_fixture.schema.json:134-163`)。key/pad enum も各一例に留まる
(`test/fixtures/ui_semantic/normative/valid/input_trace_variants.json:80-87`,
`docs/schemas/pelican.ui_semantic_fixture.schema.json:224-239`)。

invalid 6 件は additional property、未知 kind、event_seq max、variant field 禁止、rect item type、error code
だけである。required root field、array length、string pattern、各 min/max、null、non-integer、lifecycle
variant 禁止 field、consumed pattern 等は未検査である。前回条件の「各 enum variant と各 invalid class」
を満たしたとは判定できない。schema keyword/branch と fixture を対応付ける coverage manifest を置き、
CI で未被覆 branch を失敗させること。

---

# 3. EventPayloadSchema v1

## 3.1 [Blocker] 記録アーカイブは explicit schema ではなく、依然 `ref()` 由来の暗黙 introspection

設計は `registerEvent<Type>` が default instance を作り、`SchemaArchiveBuilder` へ一回通した
`prop()` 列を **schema の正本**にする (`docs/design_event_payload_schema.md:50-62`)。これは mechanism に
契約という名前を付けた点では前進だが、「`ref` から暗黙 reflection できるという前提を捨て、explicit
EventPayloadSchema を registration に追加する」という前回条件への回答にはなっていない。

二回実行が証明するのは同じ default instance/path が二回同じ列を出したことだけである
(`docs/design_event_payload_schema.md:63-67`)。次は検出できない、又は全入力を試さない限り検出保証が無い。

- default 値に対する deterministic branch (二回とも同じ枝)
- archive type に対する `if constexpr`
- 先に load した field 値に依存し、特定 payload だけで変わる branch
- duplicate prop、同じ集合だが違う順序 (`集合 ≡` 検査は宣言順 contract より弱い)
- loop 回数が状態依存だが、そのテスト入力では同じだった場合

debug observed-load check は補助的 drift detector としては有用だが、無条件列挙の機械的証明ではない。
release ではその補助すら無い。schema の正本を `EventTraits<T>::payloadSchema()`、static constexpr
descriptor、又は schema 引数付き registration として明示し、recording archive は **明示 schema と
ref の一致を debug で検査する側**へ降格すべきである。既存一引数 macro は payload-less/opaque を
明示登録し、勝手に default instance を実行しない方が安全である。

## 3.2 [Blocker] static registration に constructor/ref 実行という新しい互換破壊を入れる

現 macro は static initializer から `registerEvent<Type>` を呼ぶ
(`src/core/userpublic/details/event/registerer.hpp:121-134`)。現在の registration は loader function pointer を
組み立てるだけで、event instance を作らない
(`src/core/userpublic/details/event/registerer.hpp:49-76`)。新案ではこの時点で default constructor と
`ref()` を複数回実行する (`docs/design_event_payload_schema.md:52-67`)。

従って、既存 event の default constructor/ref が module 初期化、logging、allocation、乱数、時計、
global state、未初期化 member に依存していれば、起動前の static initialization order hazard 又は副作用が
新しく発生する。重複登録でも `EventTypeRegistration` を作ってから registry 内で重複判定する現構造なので、
schema build の副作用は重複排除より先に起きる。これは「既存 emit 経路は不変」「規模: 小」
(`docs/design_event_payload_schema.md:92-94`, `docs/design_event_payload_schema.md:107-111`)と両立しない。

explicit static descriptor ならこの問題は消える。どうしても builder を使うなら、static init ではなく
明示的 engine-init phase で実行し、constructor/ref pure/noexcept 契約、失敗の error 経路、重複時一回性を
設計する必要がある。

## 3.3 [Blocker] 型・PropMeta・Archive concept が現在の archive 群と閉じていない

schema enum は Bool/Int/Float/Vec2/Vec3/Vec4/String
(`docs/design_event_payload_schema.md:20-35`)だが、現 `JsonArchiveLoader` に bool overload は無く、逆に
quat overload はある (`src/core/userpublic/serialize/jsonarchive.hpp:10-27`)。全整数幅と signedness を
`Int` 一個へ、float/double を `Float` 一個へ潰すと、UI static JSON の型/range と C++ destination の
表現可能範囲を事前検査できない。`PropMeta` の min/max が double なのも int64/uint64 の 2^53 超を
正確に表せず、VecN の range が各成分か長さかも未定義 (`docs/design_event_payload_schema.md:23-31`)。

さらに PropMeta 付き `prop` を Archive concept に追加するとするが、挙動を決めているのは loader と
schema builder だけ (`docs/design_event_payload_schema.md:68-80`)。既存には `JsonArchiveSaver` と
`BinaryArchive` も同じ二引数 prop surface を持つ
(`src/core/userpublic/serialize/jsonarchive.hpp:30-46`, `src/core/userpublic/serialize/binaryarchive.hpp:10-26`)。
三引数 `ref()` をそれらで instantiate すると compile しない。全 archive に metadata passthrough/no-op
overload を要求するのか、metadata を `prop` から分離するのかを決めること。

最低限、schema type は exact scalar width/signedness 又は明示 conversion policy を持ち、range metadata は
型付き variant にすること。field 名重複、空名、min>max、NaN/Inf、型に不適切な range、vector range の
validation も registration error として定義する必要がある。

## 3.4 [Blocker] 三状態 lookup と unknown-field policy が消費側の必要情報を返さない

表は schema あり / 空 / opaque の三状態を置く (`docs/design_event_payload_schema.md:38-45`)一方、公開 API は
`const EventPayloadSchema *findEventSchema(name)` だけである
(`docs/design_event_payload_schema.md:47-48`)。nullptr は未知イベントと opaque を区別できず、空 schema も
「payload-less」と「serializable だが field 0」の意味差を表せない。UI は unknown_event と
no_payload_event を別 code にするため (`docs/design_ui_2d_foundation.md:220-229`)、この API 単体では必要な
判定を実装できない。

`LookupResult { UnknownEvent, Payloadless, Opaque, TypedSchema }` のように状態を明示して返すべきである。
また UI mapping の unknown field は拒否すると書くが、raw by-name/RPC JSON の additional property policy は
data model に無い。現 loader は各 `.at(name)` を読むだけで余分な key を列挙しない
(`src/core/userpublic/serialize/jsonarchive.cpp:6-52`)。schema に
`unknown_fields: reject` を置き、UI 経由だけでなく全 JSON construction path で同じ policy を使うこと。

## 3.5 EventPayloadSchema の再審査条件

1. schema を default instance の実行結果で生成せず、registration/traits の explicit descriptor を正本にする。
2. recording archive は explicit descriptor との sequence-level debug 検査に使い、重複名・順序・全 access を照合する。
3. registration 中の event construction/ref 実行を廃止するか、安全な明示 init phase と副作用契約を設ける。
4. exact type/conversion/range model、Bool/quat/VecN、int64/uint64、metadata validation を閉じる。
5. PropMeta 付き ref が Json loader/saver/Binary/Schema の全 archive で compile する規約と compile fixture を置く。
6. lookup が Unknown/Payloadless/Opaque/Typed を区別し、unknown-field policy を全 by-name 経路に適用する。
7. conditional ref の反例、duplicate/order drift、constructor side effect、typed range edge を acceptance test にする。

---

# 4. 結論と開始ゲート

- 色 C0: **先行可**。ただし manifest は上記 graph/capability/API 項目を追加してから確定する。
- 色 C1: **開始不可**。色 §1.6 の部分条件、特に誤色 fallback と HDR graph rewrite を閉じること。
- EventPayloadSchema WP: **開始不可**。explicit descriptor 方式へ設計を改めること。
- UI U0: **開始不可**。round 規範、semantic invariant validator、fixture coverage、Accept 済み
  EventPayloadSchema に依存させること。

AJV 10/10 は再現済みであり、提出 fixture が壊れているという判定ではない。Reject の核心は、
その 10 件が schema/semantic contract の未閉包を観測していないこと、そして EventPayloadSchema が
前回求めた explicit schema をまだ正本にしていないことである。
