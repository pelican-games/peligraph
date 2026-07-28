# 第9章 Web プロファイル

対象: pelican2(エンジン側 2026-07-21 時点、HEAD=`d13fc26`)+ my_webpage リポジトリ / このマニュアルはコードを正とする

> **この章の鮮度について:** エンジン側(pelican2 リポジトリ)の数値・語彙は 2026-07-21 に再確認しました。**web 側(my_webpage リポジトリ)は 2026-07-10 の調査のまま再調査していません** — §9.1 / §9.3 の「web の扱い」列・§9.6 の WW 台帳・§9.7 の 1〜4 は当時の観測です。

## この章で学ぶこと

- web ビューア(my_webpage の WebGPU「Shader Dock」)と pelican2 の関係
- **サブセット原則**の厳密な定義と、それを支える仕組み(fixture 共有・鏡像レジストリ)
- プロジェクトを web で開く手順
- web が読むもの/無視するもの — rendering config の web 対応範囲
- シェーダ stem 規約(WGSL と GLSL/SPIR-V の対応)
- 既知のサブセット原則違反と修理待ちの箇所

## 9.1 web ビューアとは

pelican2 のプロジェクト(`project.json` + passes/scenes/assets)は、ネイティブの `pelican_player`(Vulkan)だけでなく、**ブラウザ(WebGPU)でも同じファイルのまま開ける**ことを目指しています。web 側の実装は別リポジトリ `my_webpage` にあります:

| 場所 | 内容 |
|---|---|
| `my_webpage/packages/pelican-webgpu/src/pelican2/` | パーサ・ローダー・パスリゾルバ(**ランタイム依存ゼロ**の TS パッケージ。公開リポジトリ化構想あり) |
| `my_webpage/apps/site/` | Astro サイト。`/lab` ページの **Shader Dock** がビューア本体(Result / Passes ノードグラフ / Code / Buffers タブ) |
| `my_webpage/docs/` | web 側の設計文書(`implementation_plan_web.md`、`pelican2-webgpu-compat.md` = rendering config 互換の正) |

## 9.2 サブセット原則(不変条件)

> **設計決定(サブセット原則・[PFW] v1.3 で不変条件に昇格):**
> **「web で開けるプロジェクト ⊆ pelican で開けるプロジェクト」。**
> - 形式の正は常にエンジン側([../design_project_format.md](../design_project_format.md) v6 凍結 + 実装)。形式拡張は**常にエンジン先行**で、web は「無視 → 対応」の順に追随する。
> - web は pelican より**厳しくてよい**が、**緩くしてはならない**(pelican が拒否する入力を web が受理したら規約違反)。
> - web 専用の形式要素(キー・参照形式・`engine://` id)は作らない。web 固有の表示設定はプロジェクトではなくサイト側(`shaderDemos.ts`)に置く。
> - web の `engine://` レジストリはエンジンレジストリの**鏡像サブセット**(web 独自 id の追加禁止 — テストで機械検証)。

原則を機械的に支える仕組み:

- **fixture 共有(✅WW2)**: 適合性テストの正はエンジン側 `test/fixtures/project_format/`。web は `npm run sync:pelican-fixtures` で git オブジェクトストアから固定 ref を同期し、同じ入力に対する合否一致を `node --test` で検証します。未知の fixture mode は「偶然 error になった」偽陽性を防ぐため必ず fail、native 専用 mode は理由付きで明示 skip します。
- **鏡像サブセットテスト**: web が登録する engine:// id がエンジンの id 一覧(`engine_resources.json`)の部分集合であることを assert。エンジン側の id は **84 件**です(2026-07-21 時点。正は [test/fixtures/project_format/engine_resources.json](../../test/fixtures/project_format/engine_resources.json) の `ids` 配列 = fixture 共有の対象そのもの)。

## 9.3 プロジェクトを web で開く手順

1. プロジェクト一式を `my_webpage/apps/site/public/scenes/<name>/` にコピーする(静的配信される)。
2. `apps/site/src/lib/site/shaderDemos.ts` にエントリを追加し、`projectJsonUrl: "/scenes/<name>/project.json"` を指定する。
3. `npm install`(初回)→ `npm run dev` → `http://localhost:4321/lab/` を開き、デモボタンをクリックすると Shader Dock がプロジェクトをロードします。

ロードに失敗した場合はエラーメッセージ(解決後の完全 URL 付き — native と同じ fail-fast 流儀)が Config 欄に表示されます。

### web が検証するもの・無視するもの

| project.json のキー | web の扱い |
|---|---|
| `schema` / `version` | **hard error ゲート**(native と同一意味論: `"pelican.project"` / `1` ちょうど) |
| `basic_config.rendering_config_json` | **web では必須**(native は埋め込み既定へフォールバックするので、web の方が厳しい = 原則に適合) |
| `engine_min_version`, `window_title`, `window_size`, `fullscreen`, `framerate` | native 専用として**無視**(debug ログに記録) |
| `input_actions_json`, 未知キー | 黙って無視(前方互換) |
| `scene_data_json` / `asset_data_json` / `ui_config_json` | 参照解決のみ(**シーン・アセット JSON の web 対応は未着手** 📐) |

パス解決は native と同じ規則の URL 版です: `engine://` / `project://` / 素の相対のみ受理、絶対パス・`\`・URL スキーム・`%2e%2e` を含む脱出はすべて拒否。`user://`・asset store・`#` フラグメントは native 専用(web は未知スキームとして拒否 = サブセット原則どおり)。

### rendering config の web 対応範囲

web が理解するのは([第6章](06_rendering.md) の語彙のうち): RT フォーマット 7 種(**エンジン側は 45 種**)・usage 6 種、パス 3 種 `material` / `fullscreen` / `ui`(**エンジン側は 8 種 + `PELICAN_WITH_IMGUI` ビルド時のみの `imgui` = 8+1 種**)、`push_constants` 3 種、従来の G-buffer 5 枚契約です。native の `pelican.material_outputs` v1（任意枚数・float/SINT/UINT output）と`material_output_states`（output別blend/write-mask）はまだ web 未対応なので、そのキーを持つ config は web 側で**明示的に拒否**する必要があります。未知キーとして読み飛ばして5枚扱いにするとサブセット原則違反です。`shadow_depth` / `debug_draw` / `debug_text` / `features` / `compute_tasks` も web 未対応です。互換の正は `my_webpage/docs/pelican2-webgpu-compat.md` を参照してください。

括弧内のエンジン側の数はサブセット原則の実測値です(2026-07-28 時点。いずれも [renderingpassjsonhelpers.cpp](../../src/core/renderingpass/renderingpassjsonhelpers.cpp) が正 — RT フォーマットは `stringToFormat` の表、パス種別は `makePassInfo` の分岐)。**web の語彙はエンジンの語彙の真部分集合**であり、これは「web は pelican より厳しくてよいが緩くしてはならない」に適合しています。エンジン側の RT フォーマットとパス種別の説明は [第6章](06_rendering.md) を参照してください。

## 9.4 シェーダ stem 規約 — 可搬シェーダの書き方

> **設計決定(stem 参照は共通形式への唯一の追加):** シェーダ参照は拡張子なしの stem で書く。ステージ情報はキー名(`vertex` / `fragment`)が既に持っているため。同じ stem から **native は `<stem>.vert` → `<stem>.vert.spv`**、**web は `<stem>.wgsl`** を解決する。WGSL のエントリポイントは **`vs_main` / `fs_main` / `cs_main` 固定**(自動検出は「2 つ目の関数を書いた瞬間に挙動が変わる」暗黙さを避けるため不採用)。

```json
"shader": { "vertex": "engine://fullscreen", "fragment": "shaders/bloom_blur_h" }
```

可搬プロジェクトの出荷形(✅WW5):

```sh
# shaders/*.wgsl を書く → native 用の .spv を並置生成
npm run shaders:spirv -- <project-dir>
```

`<stem>.wgsl` から `<stem>.vert.spv` / `<stem>.frag.spv` / `<stem>.comp.spv` が生成され、同じ stem 参照が両方で解決できるようになります(GLSL を手書きして `.wgsl` と並置する形も可)。

- `.wgsl` の明示参照は web でのみ通り、warn が出ます(開発中の便宜。公開プロジェクトでは stem 必須)。
- `.spv` / `.vert` / `.frag` の明示参照は **native・web の両方で拒否**されます(WP63 strict v1。※設計文書の「.spv 明示は native の後方互換」という記述は失効済みで、実装が正)。
- 補足: `engine://` の stem(`engine://fullscreen` 等)は web では fetch されず、Shader Dock 内蔵の合成 WGSL で代替描画されます。実際に `<stem>.wgsl` が fetch されるのはプロジェクト内 stem だけです。

## 9.5 プロジェクトを web 互換に保つチェックリスト

- [ ] カメラは `yfov`(**ラジアン**)/ `znear` / `zfar` で書く(旧 `fov_y`/`near`/`far` は native で hard error)
- [ ] シェーダ参照はすべて拡張子なしの stem。WGSL を書いたら `npm run shaders:spirv` で `.spv` を並置
- [ ] `rendering_config_json` を明示する(web では必須)
- [ ] rendering config は web 対応範囲(material / fullscreen / ui、features なし)に収める — feature を使う config は web では開けない
- [ ] パス区切りは `/` のみ・プロジェクト相対のみ(これは native でも同じ)
- [ ] シーン・アセット参照に依存した見た目は web では再現されない(scene JSON 未対応)ことを理解しておく

## 9.6 実装状況(WW 台帳)

> ⚠ **この表は 2026-07-10 時点の観測で、2026-07-21 のエンジン側監査では検証していません。** WW の実体は my_webpage リポジトリにあり、pelican2 リポジトリからは状態を確認できません。最新の状態は `my_webpage/docs/implementation_plan_web.md` を直接見てください。

| WW | 内容 | 状態 |
|---|---|---|
| WW1 | ProjectLoader + UrlPathResolver + EngineAssetRegistry | ✅ |
| WW2 | fixture 同期 + 適合性テスト | ✅(ただし同期が停滞中 — §9.7) |
| WW3 | シェーダ stem 解決 + Shader Lab 統合 | ✅ |
| WW4 | (欠番 — scene の web 対応として暗黙予約) | 📐 未着手 |
| WW5 | naga による WGSL→SPIR-V 変換 CLI | ✅ |
| WW6 | GUI primitives v1(tokens → primitives → surfaces の三層)+ スタイルガイド | ✅ |
| WW7 | Shader Dock の primitives 移行 | 🚧 進行中 |
| WW8 | Plan viewer(web 版) | ❌ **撤回**(2026-07-12 ユーザー決定: プランビューアは**エンジン内 ImGui ツール**として実装 — ✅WP86、[第10章](10_tools.md) §10.6。web 側の着手分は撤去済み) |
| — | 可搬性 lint / scene・asset JSON 対応 / WebSocket クライアント | 📐 設計のみ |

## 9.7 既知の問題(1〜4 は 2026-07-10 調査、5〜6 は 2026-07-21 追記)

調査で判明した、**修理が必要な箇所**です。チュートリアルどおりに動かない場合はまずここを疑ってください。1〜4 は web 側を実際に見た 2026-07-10 の調査結果で、**その後 web 側は再調査していません**(直っている可能性も、増えている可能性もあります)。

1. **camera キーの世代ズレ(サブセット原則違反の実例)** — web 側のサンプル `public/scenes/hello-project/project.json`、web の型定義、web 同梱の `default_config.json` 写しは旧形式(`fov_y`/`near`/`far`)のままです。web は camera を検証せず素通しするため「web で開けるが pelican で開けない」プロジェクトになっています。新規プロジェクトでは必ず `yfov`(ラジアン)形式で書いてください。
2. **fixture 同期の停滞** — web 側の同期は 2026-07-02 の hash で止まっており、WP63(strict v1)で追加された invalid fixture(旧形式 reject 群)が反映されていません。`npm run sync:pelican-fixtures` の再実行が必要です。
3. **native ブリッジが非互換** — Shader Dock から native の pelican_player を叩いて比較レンダリングする HTTP ブリッジ(`npm run pelican:bridge`)は、削除済みの `--project-settings` フラグを使っており、camera も旧形式で生成するため、**現行エンジンでは動きません**(要修理)。
4. **web パーサの拡張語彙** — web の rendering config パーサは `shader_modules` / `scenes` / `active_scene` / インライン `source` などエンジンに存在しないキーを受理します(サイト内デモ専用)。これらをプロジェクトに書くと「web で開けて pelican で開けない」JSON になるため、**プロジェクトでは使わないでください**(形式としての位置づけは未文書化)。

### scene 対応に着手するときの追随項目(2026-07-21 追記)

scene JSON の web 対応(WW4)は未着手のままですが、**その間にエンジン側の scene v1 の受理仕様が動きました**。web が scene に着手するときは、以下がエンジン側の正であることを前提にしてください(サブセット原則より、web はこれ**以上に緩く**なってはいけません)。

5. **scene v1 に `behavior` コンポーネントが入った(✅WP155/162/167)** — オブジェクトの `components` に `{"name": "behavior", "type": "<登録名>", "params": {...}}` を書けるようになりました。`type` は必須の非空文字列、`params` は任意 object です。振り分けは [src/core/loader/scene.cpp](../../src/core/loader/scene.cpp)(`behavior` だけ ECS 経路から外れる)、受理は [src/core/gamelogic/behaviorarena.cpp](../../src/core/gamelogic/behaviorarena.cpp) の `prepareSceneBehaviorAttachments()` が担当します。詳細は [第4章](04_scene_ecs.md) / [第8章](08_gameplay.md)。
6. **コンポーネントの受理仕様が component codec に一本化され、closed schema になった(✅WP151)** — `transform` / `simplemodelview` / `camera` / `light` / `collider` / `animation` / `sprite_view` の 7 コンポーネントは [src/core/loader/componentcodec.cpp](../../src/core/loader/componentcodec.cpp) の codec テーブルが受理仕様の正です。いずれも**未知キーを名指しで拒否する closed schema**で、フィールドの必須/任意と既定値は codec ごとに決まります(例: `transform` の `pos` / `rotation` / `scale` はすべて任意で既定値あり)。web が scene を読むときは、コンポーネント側の `ref()` 実装ではなくこの codec テーブルを鏡像の元にしてください。

## 関連文書

- [../design_project_format_web_profile.md](../design_project_format_web_profile.md) — [PFW] v1.3(凍結)。サブセット原則の正(※§0 の「現状」表や camera 例は古い)
- `my_webpage/docs/implementation_plan_web.md` — WW 指示書(web リポジトリ)
- `my_webpage/docs/pelican2-webgpu-compat.md` — rendering config 互換の正(web リポジトリ)
- `my_webpage/docs/design-engine-gui-system.md` — GUI 三層(tokens/primitives/surfaces)
- [第3章 プロジェクト形式](03_project_format.md) / [第6章 レンダリング](06_rendering.md)
