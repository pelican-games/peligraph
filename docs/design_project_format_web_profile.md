# 共通プロジェクト形式: Web プロファイル(pelican-webgpu 連携)

対象読者: エンジン担当 + web(my_webpage / pelican-webgpu)担当。
ステータス: v1.3 **設計凍結**(2026-07-02 ユーザー承認。以降の形式変更は版数を上げてレビュー。
v1.0: 初版(パス解決写像・project.json 解釈・stem 規約・fixture 共有)。
v1.1: WGSL エントリポイント名 `vs_main`/`fs_main` を決定済みに昇格。
v1.2: 参照文字列の共通規則(`/` 区切りのみ・空参照 reject、§2-6)を WW1 実装から昇格。
v1.3: **サブセット原則を不変条件に昇格** — web の受理集合 ⊆ pelican の受理集合。
`"web"` トップレベルキー予約を撤回、engine:// web レジストリは鏡像サブセットに限定)。
前提: `design_project_format.md`(v6・設計凍結)を**唯一の正**とする。本書は
それを変更せず、(a) web ランタイムでの解釈規則、(b) 両ランタイムで通用させる
ための**唯一の形式追加**(シェーダ stem 参照、§4)、(c) 適合性検証の共有方法を定める。
関連: `my_webpage/docs/pelican2-webgpu-compat.md`(rendering config 互換層、実装済み)。

## 0. 目的と現状

pelican2 プロジェクト(`project.json` + scenes/assets/passes/shaders/ui)を、
ネイティブ(Vulkan)と web(pelican-webgpu)の**両方でそのまま開ける**ようにする。

現状:

| 項目 | ネイティブ | web | 共有可否 |
|------|-----------|-----|---------|
| rendering config(render_targets / rendering_passes) | 一次形式 | `parsePelicanRenderingConfig()` が同名で読む(validation も同等) | **ほぼ可**(シェーダ参照のみ非互換) |
| シェーダ参照 | `.spv` 明示パス(現行)/ `.vert`・`.frag`(WP12 実行時コンパイル) | `.wgsl` パス | **不可** ← 本書 §4 で解決 |
| project.json | WP18(凍結済み・未実装) | 未対応 | WP18 + §3 で可 |
| scene / asset JSON | 一次形式 | 未対応(web は独自に glTF 直読み) | 後続(§7) |
| engine:// / project:// | WP18 で導入 | compat 文書に記載済み・実装は部分的 | 規則を共有(§2) |

shader lab bridge(dc33a15)では web の WGSL パスをエンジン側 GLSL
(`shader_lab_*.frag`)に**手移植**した。この重複が §4 の規約で「同 stem の
2 ファイル」として整理され、機械的に対応付く。

## 1. 原則

1. **形式の正はエンジン側**: `design_project_format.md` と WP18 実装が normative。
   web は同じ JSON を読み、**理解しないキーは無視してよいが、理解するキーの
   意味論を変えてはならない**。
2. **web プロファイルは [PF] の純サブセット(v1.3 で不変条件に昇格)**:
   pelican 側は自由に使えるゲームプロジェクトとして拡張していく。web 側は
   技術検証ブログに必要な範囲だけを載せ、pelican の拡張を少しずつ反映する。
   その際の不変条件: **web で開けるプロジェクトは確実に pelican でも開ける**
   (web の受理集合 ⊆ pelican の受理集合)。web は pelican より厳しくてよいが、
   緩くしてはならない — pelican が reject する入力を web が受理したら規約違反。
   逆方向は保証しない(pelican 専用機能を使ったプロジェクトが web で
   開けないのは正常)。
3. **web 専用の形式要素は作らない**(キー・参照形式・`engine://` id のいずれも)。
   web 固有の表示設定はプロジェクトではなくサイト側(`shaderDemos.ts` 等)に置く。
   v1 で予約した `"web"` トップレベルキーは**撤回**(サブセット原則と矛盾するため)。
   形式の拡張は常にエンジン側([PF] / 本書の改訂)が先で、web 実装は
   「無視 → 対応」の順で追随する。

## 2. パス解決の web 対応表

WP18 の PathResolver と同じ 3 分類を URL 空間に写像する。web 側は
`UrlPathResolver`(仮称)として pelican-webgpu に実装する(WW1、§8)。

| 形式 | ネイティブ(WP18) | web |
|------|------------------|-----|
| 素の相対 / `project://` | project root(fs)基準 | **project base URL 基準**で URL join |
| `engine://<id>` | EngineResourceRegistry(b::embed) | **パッケージ同梱アセットレジストリ**(同じ id 空間) |
| 絶対パス | CLI 限定+フラグ。永続化 JSON 内は常に reject | **常に reject**(`http(s)://` 等スキーム付き URL も reject。CDN 直参照はプロジェクト配信側の仕事であり、JSON には書かせない) |

規則(WP18 と同義になるように):

1. **project base URL** = `project.json` を取得した URL のディレクトリ。
   例: `https://site/scenes/demo/project.json` → base は `https://site/scenes/demo/`。
2. 相対基準は**常に project base URL**(nested JSON でもファイル基準にしない。
   WP18 §3-1 と同一)。
3. **脱出防止**: join 前に参照文字列を正規化し、正規化後のパスセグメント列が
   base の外(`..` で上へ抜ける)を指したら reject。URL エンコード
   (`%2e%2e` 等)は**デコード後に判定**する。fs の symlink 問題は web には
   ないが、判定を「正規化後のセグメント列」で行う点は WP18 と揃える。
4. **`engine://` の id 空間は両ランタイムで共通**(= b::embed のキー)。
   web レジストリは同 id で WGSL / 既定 JSON を返す。未知 id は
   **登録済み id 一覧入りで throw**(WP18 受け入れ基準 j と同じ)。
   サブセット原則(§1-2)により、**web レジストリはエンジンレジストリの
   鏡像サブセット**: web が持つ id は必ずエンジン側にも存在しなければならない。
   web 独自 id の追加は禁止。
5. 存在チェックの web 対応 = fetch の 404。エラーメッセージには**解決後の
   完全 URL** を含める(WP18 受け入れ基準 c の対応物)。
6. **参照文字列の共通規則(WW1 実装レビューで昇格・2026-07-02)**:
   区切りは `/` のみ — `\` を含む参照は reject(Windows native では動くが
   Linux/web で壊れる非可搬パスを形式レベルで防ぐ)。空文字列の参照も reject。
   web 実装(WW1)は適用済み。native は WP18b で同じ判定を PathResolver に
   追加し、共有 fixture にケースを足す(それまで fixture には入れない —
   fixture は常に両実装がグリーンな状態を保つ)。

## 3. project.json の web 解釈

web ランタイムが読むキーと無視するキーを固定する。

| キー | web での扱い |
|------|--------------|
| `schema` / `version` | **ネイティブと同じ hard error 意味論**。`"pelican.project"` 以外・対応範囲超過は読まない(読めるふりをしない) |
| `engine_min_version` | **無視**(これはネイティブエンジンのバージョンゲート)。web 互換層は `schema`/`version` でゲートする |
| `name` / `generator` | 表示用に読む |
| `basic_config.camera` | 使う(fov / near / far / up) |
| `basic_config.scene_data_json` / `asset_data_json` / `rendering_config_json` / `ui_config_json` / `default_rendering_pass` / `default_scene_id` | 使う(§2 の規則で解決。scene/asset は WW3 まで任意対応) |
| `basic_config.window_title` / `window_size` / `fullscreen` / `framerate` | **無視**(canvas 側が決める)。debug ログにのみ出す |
| 未知キー | 無視(将来の前方互換) |

### 3-1. `asset_data.json` の版導入と `materials[]`(2026-07-31 決定・WW 未着手)

[`design_material_shading.md`](design_material_shading.md) §3-12 の決定により、
`asset_data.json` は次の 2 点が変わる。**web 側の対応が決まるまで、エンジン側の実装は
「web が何を拒否すべきか判別できる形」を先に用意する義務がある。**

1. **`schema: "pelican.asset_data"` / `version: 1` を持つ versioned 形式になる。**
   これまで未版だったため、web は「読めない版」を検出できなかった。導入後は `project.json` と
   同じ hard error 意味論を適用する — `pelican.asset_data` 以外・version 1 以外は
   **読めるふりをしない**。
2. **`materials[]` が追加される。** 要素は `{ "path": <project 相対の pelican.material 文書> }`。

`materials[]` は §1-1 の分類でいう「**描画の意味を変えるキー**」である。索引が非空なら
その project のメッシュは glTF 由来ではないマテリアルで描かれる。したがって web は
**`materials[]` というキーの存在自体に対して**次のいずれかを選ばねばならず、
黙って無視して glTF マテリアルで描くことは**サブセット原則違反**になる。

- **索引を読む** — `pelican.material` 文書を解決し、
  `pelican.material_bindings` の解決先へ加える(エンジンと同じ意味論)
- **明示的に拒否する** — `materials[]` が非空の project を「この web ランタイムでは
  再現できない」として名指しで読み込み拒否する

**個々の material をどこまで再現するかは本節の管轄ではない。**
[`design_material_shading.md`](design_material_shading.md) §3-1 の規律 5 が既に
「web は A 段(`base` + `defines` + `values` + `textures`、`shader` / `surface` キー無し)まで
対応。B/C 段は native 限定で web は WARN + 既定 PBR フォールバック」と定めており、
それが正である。索引を読むことを選んだ場合も、`surface` を持つ material に出会ったら
その規律に従えばよい。

`materials[]` が**空または不在の project は従来どおり**であり、この判断を要しない。
拒否を選んだ場合でも `models[]` だけの project は引き続き読めるべきである。

未決: 索引を読むか拒否するか。`my_webpage/docs/implementation_plan_web.md` へ
WW を起こす必要がある。

## 4. シェーダ stem 参照(共通形式への唯一の追加)

### 問題

同じ pass JSON を共有するには `shader.vertex` / `shader.fragment` の値が
バックエンド非依存でなければならないが、実体は GLSL(→SPIR-V)と WGSL で別ファイル。

### 規約(提案)

**拡張子なしの stem を書く。ステージはキー名(vertex / fragment)が既に持っている。**

```json
"shader": {
  "vertex": "engine://shaders/fullscreen",
  "fragment": "shaders/bloom_blur_h"
}
```

解決規則:

| バックエンド | 解決 | エントリポイント |
|-------------|------|-----------------|
| ネイティブ | `<stem>.vert` / `<stem>.frag`(ShaderLibrary 実行時コンパイル)。ソースが無ければ `<stem>.vert.spv` / `<stem>.frag.spv`(ビルド済み) | `main` |
| web | `<stem>.wgsl`(1 ファイル。probing はしない) | ステージキーで固定: vertex → `vs_main`、fragment → `fs_main` |

追加規則:

1. **既知拡張子付きの参照(`.spv` `.vert` `.frag` `.wgsl`)はバックエンド固有の
   明示参照**として引き続き有効。ただしサブセット原則(§1-2)に照らすと
   方向で意味が違う:
   - `.spv` / `.vert` / `.frag` 明示 = **pelican 専用プロジェクト**。正当
     (不変条件は逆方向を保証しない)。web は「stem 形式にすれば可搬」と
     案内して throw
   - `.wgsl` 明示 = **不変条件を破る唯一の穴**(web で開けるが pelican で
     開けない)。project 文脈では WARN(文言は「この参照は pelican で
     開けない。stem 形式 + .spv 並置で可搬になる」)。開発中の便宜として
     読み込み自体は許すが、公開プロジェクトでは stem 形式を必須とする。
     なお stem 参照でも native 対応物(`.spv` 等)の実在は web 単体では
     検証できないため、「確実に開ける」の機械的保証は §6-3 の example
     プロジェクト検証と将来の可搬性 lint が担う
2. vertex と fragment が同じ stem を指す場合、web は 1 つの `.wgsl` 内の
   `vs_main` / `fs_main` を使う(WGSL の慣習どおり)。ネイティブは
   `<stem>.vert` と `<stem>.frag` の 2 ファイル。
3. **可搬プロジェクトの出荷形は 2 通り**(いずれも stem 規約と JSON は不変):
   - **(a) WGSL 正**(推奨・将来形): `<stem>.wgsl` を正として書き、ネイティブ用
     `<stem>.vert.spv` / `<stem>.frag.spv` を **naga で生成**して並置する。
     web repo には既に naga(WASI)の WGSL→SPIR-V 実装がある
     (`apps/site/src/lib/shader/nagaSpirvCompiler.ts`、shader lab bridge で使用中)。
     ネイティブは `.spv` 直読み(WP12)なのでエンジン側変更は不要
   - **(b) GLSL / WGSL 手書き並置**: `<stem>.frag` + `<stem>.wgsl`。
     エンジン既存シェーダの移行期・naga が受けない表現のフォールバック
4. `engine://` シェーダも同規則: id は stem で書き、ネイティブレジストリは
   SPIR-V/GLSL を、web レジストリは WGSL を同 id で返す。
   例: `engine://shaders/fullscreen` ⇔ embed キー `shaders/fullscreen.vert(.spv)` ⇔
   web 同梱 `shaders/fullscreen.wgsl`。
5. compute 等ステージが増えた場合も同じ(キー名がステージを決める。
   web エントリは `cs_main`)。

### shader lab bridge への適用

dc33a15 の `shader_lab_*.frag` は web 側 WGSL と**同 stem に揃える**(WW3)。
以後、shader lab のパス定義 JSON は 1 本になり、bridge の「手移植」が
「同 stem ファイルの追加」に置き換わる。

## 5. rendering config の共有可否(確認)

`my_webpage/docs/pelican2-webgpu-compat.md` の対応済み範囲
(render_targets / passes / format・usage 変換 / validation 同等化)を前提に、
§4 適用後は**同一 JSON が両ランタイムで valid** になる。

- material pass は web では引き続き stub(定義は保持)。
- フォーマット対応表(`B8G8R8A8_UNORM` → `bgra8unorm` 等)は web 側 compat 文書が正。
  エンジン側で新フォーマットを使うときは**対応表にあるものだけ**を共有プロジェクトで
  使う(表外は web で hard error になる — これは意図どおり)。

## 6. 適合性検証の共有(fixture 同期)

パーサが 2 実装(C++ / TS)ある以上、**同じ入力で同じ合否**を機械的に保証する。

1. **正となる fixture はエンジン repo に置く**: `test/fixtures/project_format/`
   に valid / invalid の JSON 一式と、機械可読な期待値
   `expectations.json`(`{file, expect: "ok"|"error", error_kind}`)。
   WP18 の受け入れ基準 d, e, g, i, j, k の各ケースをそのまま fixture 化する。
2. **web repo は同期スクリプトで取り込む**: `npm run sync:pelican-fixtures` が
   ローカルの pelican2 checkout からコピーし、`SYNC_INFO.json`(元コミット hash)を
   書く。vitest が同じ fixture + expectations を食う。
   **submodule は使わない**(2 repo の開発サイクルが独立しており、
   クローンと CI を重くしないため。hash 記録で追跡性は足りる)。
3. `projects/example`(WP18 で切り出し)を**共有プロジェクトの実例**に育てる:
   §4 適用(stem 化 + WGSL 並置)後は、同じディレクトリを web の
   `public/scenes/` に配置するだけで Shader Dock が開けることを CI 相当の
   手動チェック項目にする。

## 7. スコープ外(順序だけ合意)

- **scene / asset JSON の web 対応**は rendering config の後(WW3 以降)。
  web の `gallery.gltf` 直読みは web 独自デモとして当面併存させる。
  対応時も形式はエンジン側 scene/asset JSON が正(web 用に別形式を作らない)。
- ui_config_json の web 対応は需要が出てから。
- 配布パッケージ化(zip/pak)は WP18 側の方針どおり設計しない。
- コマンド層(JSON-RPC 2.0 / NDJSON、implementation_plan §3)を WebSocket に
  載せると devstudio と web viewer が同じプロトコルで engine を叩けるが、
  これは本書のスコープ外(将来トラック)。

## 8. 実装 WP(提案)

### エンジン側

- **WP18**(既存・凍結済み): 先行必須。本書は WP18 の仕様を一切変えない。
- **WP19: シェーダ stem 解決**(小。依存: WP12, WP18)
  1. ShaderLibrary に stem+ステージ → モジュール解決を追加
     (`<stem>.vert|.frag` → ソースコンパイル、無ければ `<stem>.<stage>.spv`)
  2. rendering pass parser: 拡張子なし参照を stem として受理、
     既知拡張子付きは従来どおり+共有プロジェクト文脈で WARN
  3. `projects/example` の rendering config を stem 形式へ書き換え
     (`../../../../src/core/resources/*.spv` 参照の根絶は WP18 と合わせて完了)
  4. fixture: stem 解決の valid / invalid ケースを §6-1 に追加

  受け入れ基準: (a) stem 参照で従来と同一の描画(golden テスト、WP16 基盤)
  (b) `.spv` 明示参照の後方互換 (c) 未解決 stem のエラーに試行したパス一覧が入る

### web 側(my_webpage。WW = Web Work Package)

- **WW1: ProjectLoader + UrlPathResolver + EngineAssetRegistry**(中。依存: WP18 の形式のみ — エンジン実装完了は待たなくてよい)
  1. `project.json` ロード(§3 の must-understand / ignore 表どおり)
  2. §2 の解決規則(URL join、脱出 reject、engine:// レジストリ、絶対 reject)
  3. エラーは解決後 URL / 登録済み id 一覧入り(WP18 基準 c, j のミラー)

  受け入れ基準: `public/scenes/<proj>/project.json` を指すだけで
  rendering config が Shader Dock に載る。単体テストで脱出・絶対・未知 id reject。
- **WW2: fixture 同期 + 適合性テスト**(小。依存: WW1、エンジン側 fixture)
- **WW3: シェーダ stem + shader lab 統合**(中。依存: WW1, WP19)
  `shader_lab_*` を同 stem WGSL/GLSL 並置に整理し、パス定義 JSON を 1 本化。

## 9. 決定済み事項と未決事項

決定済み(2026-07-02):

- **サブセット原則を不変条件に昇格**(方針決定): web の受理集合 ⊆ pelican の
  受理集合。pelican = 自由に拡張するゲームプロジェクト、web = 技術検証ブログに
  必要な範囲を追随。web 専用の形式要素は作らず、v1 で予約した `"web"`
  トップレベルキーは撤回(§1-2, §1-3)。
- **WGSL エントリポイントは `vs_main` / `fs_main` で固定**(compute は `cs_main`)。
  根拠: WebGPU の慣習であることに加え、web 側既存コードが既に全面採用している
  (`ShaderRuntimeCanvas.tsx` / `HomeShaderStage.tsx` のインライン WGSL と
  pipeline の `entryPoint` 指定、`ShaderDockHost.tsx` の
  `normalizeWgslEntryPointForPelican()`)。移行リネームは不要。
  自動検出(唯一の `@vertex`/`@fragment` を拾う)は不採用 — 2 つ目の関数を
  書いた瞬間に挙動が変わる暗黙さを避ける。

未決:

1. §4-3(a) の naga 変換を CLI / ビルドスクリプト化する時期(ブラウザ内 WASI 実装は
   実績あり。プロジェクトの `shaders/` を一括変換する node スクリプトにするだけ。
   naga 生成 SPIR-V の set/binding が [SF] §5 規約と噛み合うかは golden テストで実物検証)
2. asset manifest(sha256)導入時に web のキャッシュ検証へ流用するか
3. コマンド層の WebSocket 展開(§7)を WP 化するタイミング
