# 機能追加レシピ集(cookbook)

対象読者: 実装エージェントと人間。「何かを足す」とき、どのレシピに当たるかを
まず判定し、その手順とチェックリストに従う。

全レシピ共通の原則:

- **パージ可能**: 参照しなければ存在しない(素通り原則)。使わない構成での
  挙動不変を golden / 既存テスト維持で示す
- **fail-fast**: silent skip 禁止。reject は必ず理由と手がかり(解決後パス・
  登録済み一覧・OFF ビルド明示)を言う
- **schema + version**(R10): 新しい JSON には必ず付ける。ゲートは hard error
- **純ロジック分離**: パース・検証・計画はモジュール/GPU 非依存の plain
  クラスにし、GPU 不要でテストする(手本: `core/loader/sceneformat.{hpp,cpp}`)
- 命名は R7(`[a-zA-Z0-9_]`)。パス区切りは `/` のみ

---

## レシピ 1: GPU 機能(ポストエフェクト・描画機能)

エンジンコード不要のことが多い。手本: WP30(HDR)。

1. feature fragment を書く: `src/core/resources/features/<name>.json`
   (`pelican.render_feature` v1 — passes 挿入 / render_targets 追加 /
   render_target_overrides / shader_defines。挿入アンカーは自動的に
   after/before エッジ化される)
2. シェーダを stem で書く(`<name>.frag` 等)。マテリアル系に合流する場合は
   `#include "pelican_features.glsl"` + `#ifdef PELICAN_FEATURE_<NAME>`
   (set / push constant / 頂点入力は `docs/shader_contract.md` を確認)
3. **engine:// 登録 3 点セット**(下のチェックリスト)
4. golden: 機能 on/off の両ケースを追加。off = 既存 golden 全維持
5. エンジン側データが要る場合(shadow のライト行列等)のみ C++: 供給側の
   モジュール拡張 + define でシェーダ合流

## レシピ 2: 交換形式(pelican.* の新形式)

手本: sceneformat(WP25)/ vatformat(WP20)/ importmanifest(WP21)/ actionmap(WP39)。

1. 純ロジックパーサ `core/<層>/<name>format.{hpp,cpp}`: schema/version ゲート、
   検証は fail-fast、出力は plain struct
2. バインダ(エンジンへの流し込み)は呼び出し側に薄く。`GET_MODULE` は
   バインダ側のみ
3. fixture 駆動テスト: `test/fixtures/<name>/` + `expectations.json`
   (`{file, expect: "ok"|"error", error_kind}`)。JSON をテストコードに
   インライン埋め込みしない(web が同じ fixture を食うため)
4. web 側: 対応するまで web conformance の `nativeOnlyModes` に理由付き登録
   (未知 mode は必ず fail — 偽陽性禁止)
5. 形式が project.json に参照キーを足す場合は [PF] の版数を上げて追記
   (形式拡張は常にエンジン先行 — サブセット原則)

## レシピ 3: コンポーネント(シーンに置ける新しい振る舞い)

1. `src/core/userpublic/components/` に定義し ComponentInfoManager へ登録
   (`src/core/ecs/` コアには触れない)
2. scene JSON は自動対応(形式は「name + params のレコード」までしか規定しない
   — `design_scene_format.md` §2-1)。**コンポーネント params にファイルパスを
   直接書かない**(asset id 間接参照の原則 §2-2)
3. light のようにエンジン側コンテナへ振り分ける特別扱いが要る場合のみ
   SceneLoader(バインダ)に分岐を足す

## レシピ 4: compute タスク

コード不要。rendering config に `buffers` / `compute_tasks`(reads/writes 宣言、
順序は書かない)+ `.comp` シェーダ(stem)。順序・バリアはプランナが導出し、
導出不能な writes-writes は hard error(解消は明示エッジ or 中間リソース)。
確認は `--dump-frame-plan` / rpc `get_frame_plan`。

## レシピ 5: プロジェクト内C++コード(静的リンク)

プロジェクト固有のゲームロジックを player に取り込む。エンジンはSDKとして扱い、
DLL境界は作らない。

1. プロジェクトに `code/CMakeLists.txt` を置き、そこでは
   `pelican_game_sources(<src>...)` だけを呼ぶ。相対パスは `code/` 基準で解決される。
2. 実装 `.cpp` は `gamesystem.hpp` など `src/core/userpublic/` の公開APIを使う。
   毎フレーム処理は `PELICAN_REGISTER_SYSTEM(Type, order)` で登録し、
   `void update(Pelican::GameContext &ctx)` に書く。
3. ゲームコードから `GET_MODULE` や `src/core/ecs/` コアへ直接触れない。
   入力・時刻・transform 操作は `GameContext` / userpublic facade を通す。
4. ビルドは `cmake -S <engine> -B <build> -DPELICAN_PROJECT=<project>`。
   未指定時はプロジェクトコードを一切includeせず、従来挙動を維持する。
5. 入力アクションを使う場合は project.json の
   `basic_config.input_actions_json` に `pelican.input_actions` JSON を参照させ、
   `input_profiles` と `input_profile` で `pelican.input_profile` を選ぶ。

---

## チェックリスト: engine:// リソース登録(3+1 箇所)

新しい埋め込みリソース(シェーダ・fragment・既定 JSON)を足すとき:

1. `src/core/resources/CMakeLists.txt` — `b_embed(pelican_resources <id>)`
2. `src/core/loader/engineresources.cpp` — `registered_ids` 配列(**サイズ定数も
   更新**)+ `PELICAN_ENGINE_RESOURCE(...)` 行
3. `test/fixtures/project_format/engine_resources.json` — id 追加
   (レジストリ一致テストがズレを検出する)
4. (web が同じ id を提供する場合のみ)my_webpage の `engineAssets.ts` —
   **web は鏡像サブセット**: エンジンに無い id を web に足すのは禁止

この 3 箇所重複はビルド時自動生成([PF] §8 未決 2)で将来解消予定。

## チェックリスト: 新サブシステムの誕生時要件

- **パージ要件**: 参照ゼロで素通り(レシピ共通)+ 規模が大きい場合は
  `PELICAN_WITH_*` ビルドユニット(`design_build_tiers.md` §2。既定 ON、
  OFF は明確エラー)。音声・ロジック VM・OpenXR は必須
- **決定性**: 同じ入力 → 同じ出力を壊さない(EngineTime / スナップショット /
  rpc 応答列一致の資産を守る)

## Recipe 6: temporal history and velocity (WP88)

Temporal effects remain user-space render features. A target with `"history": true`
owns two images. Read the previous image by writing `<target>@history` in a fullscreen
pass `input`; write the current image with the unqualified target in `output.color`.
The authored `clear_color` initializes both images on startup, resize, and `set_time`
reset (default: transparent black).

Add `"engine://features/velocity.json"` to `features` to produce the purgeable
`velocity` target. It is `format_class: data`, `R16G16_SFLOAT`, and stores
`(currentNdc.xy - previousNdc.xy) * 0.5`. It includes object and camera motion and
does not add projection jitter.

```json
{
  "render_targets": [{
    "name": "accum", "extent_scale": 1.0,
    "format": "R16G16B16A16_SFLOAT",
    "format_class": "explicit(R16G16B16A16_SFLOAT)", "role": "color",
    "usage": ["COLOR_ATTACHMENT", "SAMPLED"],
    "history": true, "clear_color": [0.0, 0.0, 0.0, 1.0]
  }],
  "passes": [{"insert": "after:post_main", "pass": {
    "name": "accumulate", "type": "fullscreen",
    "input": ["current_color", "accum@history", "velocity"],
    "output": {"color": "accum", "depth": null},
    "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/accumulate"}
  }}]
}
```

Inputs become set 1 bindings in declaration order, so a raw fragment shader declares
bindings 0/1/2 for current color, history, and velocity. `.surface` keeps using its
generated `pelican_screen_<name>(uv)` accessors; temporal post-process passes normally
use a raw fullscreen shader because `.surface` describes geometry materials. Copy a
bundled feature JSON into the project before modifying it; bundled features are not
privileged.

`@history` appears as `reads_history` in `pelican.frame_plan` and creates no same-frame
edge or barrier. A normal `velocity` read is an ordinary dependency. TAA, camera jitter,
and accumulation policy are outside WP88.

- Determinism rule: wall clock / std::rand / random_device must not drive deterministic decisions.
- ログは quill(stdout は rpc プロトコル専用)
- 常駐プロセスをテストで起動したら必ず停止する(エージェント向け)
- **新しい外部依存(FetchContent)を足すときは Emscripten 対応の有無を
  一行メモする**(将来の WASM ビルド保険。描画系の依存は対象外 — web には
  行かない。ゲーム側レイヤの依存のみ)

## テストの型 早見表

| 対象 | 型 |
|------|-----|
| パース・検証・計画 | fixture 駆動 + GPU 不要 |
| 描画結果 | golden(on/off 両方。tolerance 比較) |
| CLI・プロトコル | cmake スクリプト結合テスト(`test/run_*.cmake` の流儀。validation エラー検出込み) |
| 実行計画 | プラン比較 fixture(`pelican.frame_plan`) |
| バイト厳密な fixture | `.gitattributes` の `-text` を忘れない(CRLF 事故) |
