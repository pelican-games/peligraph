# センサス決定表への codex 判断(+ 裏取り)

日付: 2026-07-26
対象: [決定表](2026-07-26_census_decision_table.md)
性格: **codex の推奨であって最終決定ではない。** 一部は私が実コードで裏取りし、
食い違いを注記した。

## 0. この文書の読み方

センサス 3 本は**判断を書かずに証拠だけ**を埋めた。本書は codex(このコードの
実装を担ってきた側)に判断を求めた結果である。**codex がセンサスの証拠を
訂正した箇所が特に価値がある** — 実装意図を知っている側にしか出せない情報だから。

ただし codex も間違える。§3 に裏取り結果を示す。

## 1. codex の判定

### 削除推奨

| 項目 | codex の理由 |
|---|---|
| `applyLoweredMaterial` 2 overload | テスト専用の未接続 integration seam。実ローダーは別経路で `MaterialInfo` を構築 |
| `applyLoweredMaterialForRoute` 2 overload | route-aware 後継として追加されたが**これも本番未接続**。新旧を同時に残す根拠がない |
| `FrameGraphRuntimeContainer::registerExecutionPlan` / `find` / `findProgram` | `prepareGeneration`/`publish`/`snapshot` と重なる内部 compat 面。テストと旧文書を移行できる |
| `lowerMaterialWithSnapshots` | WP83 時点の名前ベース検証 wrapper。現在の typed screen-input 契約と重複 |
| `acceptingResourcesForTesting` / `notifyPathForTesting` / `publicApiCallCountForTesting` / `watcherForTesting` | 利用経路なし(未完成のテスト seam) |
| `VulkanProcessType::compute` | producer なし。**async compute に必要な queue ownership 実装も伴わない孤立分岐** |
| `queryFormatCapability` compat 分岐 | 旧 callback から `max_array_layers=UINT_MAX` 等を合成し、**完全な device evidence を要求しない**。pure caller も新 callback へ移すべき |
| camera 旧名拒否表の重複 | 拒否動作は保持し、**3 箇所**の同型表を 1 個の共通 policy へ集約 |
| import manifest の version 省略許容 | versioned な 4 形式で省略を許す契約は曖昧。外部 manifest を移行して version 1 必須に |

### 保持推奨(= 誰のためかを答えたもの)

| 項目 | codex の理由 |
|---|---|
| **split sampler `7+2i`** | **build + 環境変数で明示起動する experimental 機能。`src/` が環境変数を設定しないことは到達不能の証拠ではない** |
| `payloadFields` | SDK 公開面。**ゲーム DLL の型付き event schema 作者**が利用者 |
| `ResourcePatternFallback::reject` | materialize を許さない厳格な planner policy。分岐も実装済み |
| `drawSortInputV1LegacyPrefixSize` | **ABI tail 追加前の境界を固定し、旧 draw-sort provider との `struct_size` 交渉を守る** |
| animation ABI の複数 enum 値 | **凍結済み外部 DLL ABI なので削除・再採番不可**。ただし未実装値の検査には欠陥がある |
| `LogicalPortRelationKind` の未使用値 | half-resolution / view / sample 関係を表す論理 graph 語彙として**設計に記録済み** |
| `ShaderCompileOptions::entry_point` | **named entry point を使う HLSL 等の backend** が利用者 |
| UI `Align`/`Justify`/`Visibility` | 公開 authoring schema の基本機能。リポジトリ内 document が未使用でも**ゲーム側利用を否定できない** |
| `legacy_gbuffer_v1` | 現在も既定値・旧 5-MRT pass 互換判定として動作 |
| `allow_legacy_type_fallback` | untyped 旧 graph を typed shadow graph へ移行・監査するための**明示的 migration seam** |
| `serializeCompiledRenderPipelineMetadata` | frame-plan dump / PlanViewer / テスト / 外部観測用の serializer として現役 |
| **`PlanViewer` / `CompiledPlanViewer`** | **前者は authoring/lowering intent、後者は publish 済み physical runtime generation を表示。観測層が異なる** |
| `LoweredMaterial::target_pass` | PlanViewer が現用。ただし実 PassId でなく route 名なので、viewer を typed route へ移行後に再判断 |
| `material_absolute_override_fixture.hpp` | 初期値 0 の material への absolute override を画像で検証する **WP122b 専用 fixture** |
| 型 6 件(`ResolvedSampleCountResource`/`DebugDraw`・`DebugText`/`RenderingTarget*Assignment` ほか) | 層が異なる / variant の pass kind を型で区別 / 値型の取り違えを防ぐ typed bridge。**型は残し、変換だけ 1 関数へ集約** |

### 保留(オーナー判断が要る)

| 項目 | 決めること |
|---|---|
| `SKIP_TEST` | downstream CMake 利用者の廃止猶予・削除版 |
| `multiviewSlotResolutionForTesting` | 当日追加の仕掛かり。assertion を足すか seam を消すか |
| `depth_test` / `depth_write` | 公開 schema の第二の綴り。互換期間と canonical spelling |
| synthesized `fallback_schedule` | **`target_plan == nullptr` を今後も契約とするか** |
| physical fragment の多版受理 | 「ランタイムは v1 だけを読む」との整合 |

## 2. codex によるセンサス証拠の訂正(8 件)

| # | 訂正 |
|---|---|
| 1 | **split sampler**: 環境変数は外部プロセスが設定する設計。「`src/` が set しない」は本番到達性を否定しない |
| 2 | `*ForTesting` のうち alias/memoryDependencyCount は `Renderer` wrapper 経由でテストから到達する(→ **§3 で反証**) |
| 3 | **camera 拒否表は 2 重ではなく 3 箇所**(`componentcodec.cpp` にもある) |
| 4 | animation の enum は 6 でなく **5**。`ValueKind` は**出力 tag** であり「外部選択可能」は不正確 |
| 5 | **physical fragment は v1/v2 を v3 へ書き換えていない**。`schema_version` を保持した共通内部型へ parse し、旧版へ `split_only` 意味論を適用する。ただし**ランタイムが 3 版を読むという規約上の問題は残る** |
| 6 | `drawSortInputV1LegacyPrefixSize` の `static_assert` は自己参照ではなく、**ABI offset を壊す変更を compile 時に止める実効的な利用**。外部 DLL 側の利用は repo grep で数えられない |
| 7 | `ResourcePatternFallback::reject` は setter 数だけで判断できない。planner の reject 分岐と「fallback または error」という設計語彙が存在 |
| 8 | `shader_contract.md` は全体として「source 経路が既定、split は experimental」と**正しく書いている**。乖離は experimental 節で combined を「fallback」と呼ぶ局所表現だけで、決定表の「標準・fallback が全面的に逆」は**強すぎる** |

## 3. codex の訂正への裏取り(私が実施)

| 訂正 | 裏取り結果 |
|---|---|
| #3 camera 3 箇所 | **正しい**。`basicconfig.cpp` / `camera.cpp` / `componentcodec.cpp` の 3 つを確認 |
| #2 ForTesting のテスト到達 | **誤り**。`renderer.hpp:114,117` に wrapper は実在するが、**`test/` に呼び出しは 0 件**。センサスの「本番 0・テスト 0」が正しい。ただし**両方とも今日の進行中作業で追加**されたものなので、「保持」という結論自体は(仕掛かり中という別の理由で)妥当 |
| #1 #6 #8 | コード・文書と整合。センサス側(および私)の過剰主張を訂正するものとして受け入れる |

**教訓**: 実装を担った側の訂正は価値が高いが、**それも検証を要する**。今日の
バグ監査で 54 主張中 4 件が誤りだったのと同じ比率で、判断側も間違える。

## 4. codex の推奨実装順

1. **オーナー判断を先に固定**: physical fragment / `SKIP_TEST` / depth 綴り / null target plan
2. **correctness を先に直す**: animation 未実装 enum の明示 reject / 文書乖離 4 件 / shader-compiler-OFF の CI
3. **低リスクの leaf 削除**: 未使用 `*ForTesting` / `VulkanProcessType::compute`
4. **意味を変えない整理**: camera policy 共通化 / `LogicalFrameNodeInvocation` rename / sample-count 変換集約
5. **compat caller を移行してから削除**: FrameGraph 旧面 / `lowerMaterialWithSnapshots` / `applyLoweredMaterial*` / format capability compat
6. **公開データ変更**: manifest version を一括移行し、省略を負例化
7. 最後に full build + 全 CTest + golden inventory/verbose + player smoke で
   **「削除前後の観測不変」**を確認

## 5. 私と codex で判断が割れた/補われた点

- **PlanViewer と CompiledPlanViewer**: センサスは「理由が記録に無い」と分類したが、
  codex は明確な理由(観測層が異なる)を示した。**実際 `compiledplanviewer.hpp` の
  冒頭コメントに書いてある** — センサスが docs だけを検索し、コードのコメントを
  見なかったための取りこぼし
- **split sampler / drawSortInputV1LegacyPrefixSize**: 「本番 0」を削除候補と
  読んだのは私の側の過剰解釈。**外部プロセス・外部 DLL は repo grep の外にある**
- **`applyLoweredMaterial*`**: ここは両者一致で削除推奨。**新旧どちらも本番未接続**
  という異常な状態
