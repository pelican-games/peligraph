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

コード不要。rendering config に `buffers` / `render_targets` と
`compute_tasks`(reads/writes 宣言、順序は書かない)+ `.comp` シェーダ(stem)を書く。
順序・バリアはプランナが導出し、導出不能な writes-writes は hard error
(解消は明示エッジ or 中間リソース)。確認は `--dump-frame-plan` /
rpc `get_frame_plan`。

画像を読む通常経路は `resource_ports` で論理名へ名前付き shader port を与える。
port は既存の `reads` / `writes` を注釈するだけで、依存 edge を新設しない。
read-only image の `access` は既定で `sampled`、write image は `storage` になるが、
公開契約を明確にする場合は次のように明記する。

```json
{
  "compute_tasks": [{
    "name": "filter",
    "shader": "shaders/filter",
    "reads": ["scene_color"],
    "writes": ["filtered_color"],
    "resource_ports": {
      "source": {
        "resource": "scene_color",
        "access": "sampled",
        "sampling": {
          "filter": "linear",
          "address": "clamp_to_edge"
        }
      },
      "destination": {
        "resource": "filtered_color",
        "access": "storage"
      }
    },
    "dispatch": {"groups": [8, 8, 1]}
  }]
}
```

GLSL は generated include を読み、set/binding 番号を持たない。
compute でも同じ `pelican_frame.glsl` から camera/time/resolution/light を読める。

```glsl
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
#include "pelican_resource_ports.glsl"
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = (vec2(p) + 0.5) / vec2(pelican_size_source());
    vec4 color = pelican_sample_source(uv);
    pelican_store_destination(p, color);
}
```

`view: "shared_2d"` が既定で、XR の eye ごとの target には
`view: "per_view"` を指定する。fullscreen pass でも同じ `resource_ports` と
`pelican_sample_<port>()` を使える。既存の raw
`layout(set=1,binding=N)` は buffer、特殊 descriptor、低レベル実験用の
escape hatch として残る。

GPU が次の task の dispatch 数を生成する場合は、buffer に
`"command_layout": "compute_dispatch"`を付け、consumer を
`"dispatch": {"indirect": {"buffer": "<name>", "offset": 0}}`にする。
command は `uint x, y, z` の12 byteで、producer の`writes`からconsumerの
indirect read edgeと必要なbarrierは自動導出される。consumerのshader入力ではないため、
同じbufferを`reads`へ重複記述しない。

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

For a normal fullscreen shader, add named `resource_ports` for current color, history,
and velocity and use the generated `pelican_sample_<port>(uv)` accessors. A deliberately
raw fragment shader may still declare set 1 bindings 0/1/2 in `input` declaration order.
`.surface` keeps using its generated `pelican_screen_<name>(uv)` accessors; temporal
post-process passes normally use a fullscreen shader because `.surface` describes
geometry materials. Copy a bundled feature JSON into the project before modifying it;
bundled features are not privileged.

`@history` appears as `reads_history` in `pelican.frame_plan` and creates no same-frame
edge or barrier. A normal `velocity` read is an ordinary dependency. TAA, camera jitter,
and accumulation policy are outside WP88.

## Recipe 7: 標準 TAA をコピーして改造する

標準 TAA は `engine://features/taa.json` として提供されるが、エンジン特権は
使っていない。通常の `pelican.render_feature` v1、named render-target binding、
scalar parameter → shader define、history RT、FrameUBO の公開フィールドだけで動く。
まず velocity と TAA をこの順で有効にする。

```json
{
  "features": [
    "engine://features/velocity.json",
    {
      "ref": "engine://features/taa.json",
      "parameters": {
        "scene_color": "lit_color",
        "velocity": "velocity",
        "depth": "offscreen_depth",
        "downstream_color": "lit_color",
        "alpha": 0.1,
        "disocclusion_tau": 0.1,
        "depth_epsilon": 0.00001
      }
    }
  ]
}
```

- `scene_color` / `downstream_color` は同じ scene RT にできる。resolve と composite
  は別 pass なので、同一 pass の read/write 禁止には触れない。
- `depth` は D32 等の depth attachment を bind する。TAA feature が既存 usage
  override で `SAMPLED` を追加するため、project 側で重複指定する必要はない。
- `alpha` は新フレーム寄与率、`disocclusion_tau` は相対深度差の棄却閾値、
  `depth_epsilon` は分母の下限。いずれも compose 時の scalar parameter であり、
  変更時は shader define と cache key が変わって再コンパイルされる。
- TAA feature 自身が Halton(2,3) 8 phase の `projection_jitter` provider になる。
  同時に別の jitter provider を有効にすると feature 名入りで reject される。
- 改造時は `src/core/resources/features/taa.json`、`taa_resolve.frag`、
  `taa_composite.frag` を project の `features/` / `shaders/` へコピーし、JSON の
  shader ref を project 側の stem へ変更する。履歴参照は `taa_accum@history`、
  reset 判定は `temporal_reset_epoch != previous_temporal_reset_epoch` を維持する。

## Temporal 系のユーザー管理枠

Temporal effect は、品質やアルゴリズムのように今後も発展する部分を project 側で
管理する。`engine://features/taa.json` と同梱 shader は出発点にすぎない。project の
`features/` / `shaders/` へコピーした後は、次の部分を自由に改造してよい。

| コピーして管理する部品 | 変更してよいこと |
|---|---|
| TAA feature JSON | pass の段数、RT と history 面、anchor、reads / writes |
| resolve / composite shader | clamp、disocclusion、blend、responsive mask など品質の全部 |
| scalar parameter | α / τ / ε の値、宣言、任意 parameter の追加 |
| projection jitter 系列 | `pattern: "table"` の位相数と pixel offset の配置 |
| feature 全体 | checkerboard、accumulation、upscaler など別方式への差し替え |

ユーザー定義の jitter は feature JSON に数表を直接書く。`phases` は省略時に
`offsets_px` の要素数から決まり、明記する場合は要素数と一致させる。

```json
{
  "projection_jitter": {
    "pattern": "table",
    "offsets_px": [[0.0, -0.1667], [-0.25, 0.1667], [0.25, -0.3889]]
  }
}
```

一方、次の 6 項は安全境界を作る版付きのエンジン語彙であり、feature のコピーでは
変更しない。

1. jitter を scene raster だけへ適用し、culling / RPC / gameplay へ見せない位置
2. projection jitter provider を同時に一つだけ許す排他
3. FrameUBO が供給する jitter pair と temporal reset epoch pair の形式
4. history RT の二面を frame ごとに flip するタイミング
5. velocity 用の前 frame matrix / palette を保持する機構
6. resize / `set_time` / camera discontinuity で reset epoch を増やす規則

改造案がこの 6 項のどれかを変えないと実現できない場合は、feature 内の回避策や
専用の隠し経路を足さない。それは公開語彙が不足している合図なので、必要な入力・
適用点・安全条件を整理し、既存 schema と互換な additive vocabulary の追加 WP を
依頼する。承認された語彙が入った後に feature 側の実験を続ける。

- Determinism rule: wall clock / std::rand / random_device must not drive deterministic decisions.
- ログは quill(stdout は rpc プロトコル専用)
- 常駐プロセスをテストで起動したら必ず停止する(エージェント向け)
- **新しい外部依存(FetchContent)を足すときは Emscripten 対応の有無を
  一行メモする**(将来の WASM ビルド保険。描画系の依存は対象外 — web には
  行かない。ゲーム側レイヤの依存のみ)

## Recipe 8: RenderDocで絵の不具合を追う

`PELICAN_WITH_RENDERDOC=ON` の開発buildで使う。PelicanはRenderDocをロードせず、
RenderDoc UI/CLIからprocess起動時に注入済みの `renderdoc.dll` を
`GetModuleHandleA` で受動検出するだけである。通常起動の `get_status` は
`renderdoc:"absent"` となり、描画とgolden byteは変わらない。

1. replay、`set_time`、`step_frame` で不具合を同じlogical frameに再現する。
2. RenderDocのLaunch Applicationからplayerを起動する。pass木を読む場合は
   player引数へ `--gpu-labels` も加える。起動後のattachや、applicationからの
   `LoadLibrary`、`VK_LAYER_RENDERDOC_Capture` の通常layer化は使わない。
3. windowed flatはF11を押す。PelicanがRenderDoc側のcapture hotkeyを無効化し、
   次のlogical frameを明示 `StartFrameCapture/EndFrameCapture` で1枚だけ囲む。
4. headlessはstdio RPCへ次を送る。`capture_gpu` はpending transformをflushし、
   `render_frame` と同じcurrent-time sequence sample + renderを1回だけ行う。

```jsonl
{"jsonrpc":"2.0","id":1,"method":"set_time","params":{"t":1.25}}
{"jsonrpc":"2.0","id":2,"method":"capture_gpu","params":{}}
```

成功応答の `path` はtemplateではなく、`GetNumCaptures` が実際に1増えたindexを
`GetCapture`の二段呼び出しで引いたcanonical absolute `.rdc` pathである。
`frame` はcaptureしたEngineTime frame indexで、time/frame自体は進まない。

5. `.rdc` を開き、Event Browserの
   `frame/<logical-frame>/graph/flat/view/0/node/<ordinal>:<kind>:<name>` 配下に
   `barriers` と `body` がplan順に並ぶことを確認する。

未注入RPCは `-32010` / `renderdoc_not_injected`、処理中の二重要求は要求元名を含む
`capture_busy`、XR active中は `capture_xr_unsupported` で拒否する。XRのVulkan
二眼captureは後続契約であり、このv1のflat/headless captureをHMD compositor像の
captureと呼んではならない。

## テストの型 早見表

| 対象 | 型 |
|------|-----|
| パース・検証・計画 | fixture 駆動 + GPU 不要 |
| 描画結果 | golden(on/off 両方。tolerance 比較) |
| CLI・プロトコル | cmake スクリプト結合テスト(`test/run_*.cmake` の流儀。validation エラー検出込み) |
| 実行計画 | プラン比較 fixture(`pelican.frame_plan`) |
| バイト厳密な fixture | `.gitattributes` の `-text` を忘れない(CRLF 事故) |
