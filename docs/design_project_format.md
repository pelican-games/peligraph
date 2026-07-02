# プロジェクトファイル設計(エンジンとコンテンツの分離)

対象読者: エンジン担当 + DCC/ツール側担当。
ステータス: v6(2026-06-12 レビュー 5 巡・最終整理: locator 引数の適用除外、
`project://` の v1 正式対応、setup 再呼び出し方針)。**設計凍結・実装中**。
v6.1(2026-07-02 追記・形式自体の変更なし): §5-3 の暗黙プロジェクトに
**開発ビルド向け fallback** を追加 — exe ディレクトリに project.json が無い場合、
親ディレクトリ方向へ `projects/example/project.json` を探索して暗黙ルートにする
(WP18c 実装レビューで受理。POST_BUILD コピー廃止後も `--project` なし互換起動を
成立させるため。選ばれた root は従来どおり WARN ログで開示される)。
実装順: P0(ProjectSource バグ、684494f で完了)→ PathResolver 型+
EngineResourceRegistry 最小実装 → `--project` / 読み込み置換 / example 切り出し。
前提: `design_roadmap_renderworld.md`(ロードマップ・肥大化対策 §6)、
`docs/implementation_plan.md`(WP1 EngineLaunchConfig、WP17 SeqPlayer)、
`docs/dcc_integration_qa_2026-06-12.md` §1(DCC bridge)。

## 0. 目的と現状の問題

「エンジン(このリポジトリ)」と「コンテンツ(ゲーム/映像プロジェクト)」を分離し、
Unity の Project / Godot の project.godot に相当する**プロジェクトディレクトリ**を定義する。

現状の仕組みと問題(2026-06-12 時点の実地調査):

| 現状 | 問題 |
|------|------|
| `PelicanCore(settings_str)` に JSON 文字列を渡す(player は `"{}"`)。不足分は埋め込み `default_config.json` で補完 | プロジェクトという単位が存在せず、コンテンツの置き場が定義されていない |
| `scene_data_json` / `asset_data_json` / モデルパスは **cwd 相対**で解決 | **exe のあるディレクトリから起動しないと即死**(実際に踏んだ罠。DCC bridge の subprocess 呼び出しでも必ず踏む) |
| バイナリアセットは手コピーで exe 隣に配置(POST_BUILD コピーは応急処置) | クリーンで消える/マシン間で揃わない/「自分の環境では出るが他では黒背景」の温床 |
| `ProjectSource::loadSource()` の path 経路は**読み込んだデータを返さない**(`loaded_data` を構築後に破棄して fallthrough) | 潜在バグ。現状 raw_data(settings_str)経路しか機能していない |

## 1. 原則

1. **プロジェクト = 1 ディレクトリ。** ルートに `project.json`(マニフェスト)。
   これを「開く・実行する・配布する」の単位にする。
2. **パス解決は §3 の分類に従い、cwd は一切参照しない。**
   エンジン内のファイルアクセスは PathResolver(§5)経由に統一する。
3. **エンジン既定リソースは embed 継続**(`engine://` 名前空間、§3)。
   設定値(basic_config)は §4 の優先順位で合成する。
4. **「機能はなるべくアセットに」(ロードマップ §6)との接続**: パス定義 JSON・
   ランタイムコンパイルシェーダ・UI 定義もプロジェクト側に置ける。
   エンジン標準資産(bloom 等)は embed、プロジェクトはそれを参照 or 自前を置いて参照先を変える。
5. **外部ツール(mocap lab / droplet_lab / cloth_lab / toon_baker)の納品物は
   プロジェクト内に着地する。** ツール側は R3/R4 の標準形式を書くだけ(契約は不変)。

## 2. ディレクトリレイアウト(推奨形)

```
myproject/
  project.json              # マニフェスト(必須。これがあるディレクトリ = プロジェクトルート)
  scenes/
    main.scene.json
  assets/
    models/   *.glb *.vrm
    textures/ *.png
  passes/     *.json        # レンダリングパス定義(エンジン既定を差し替える場合)
  shaders/    *.vert *.frag # ランタイムコンパイル対象(シェーダ自由化キット後)
  ui/         *.json
  imports/                  # 外部ツール納品物の着地点(motion.glb / *.jsonl / VAT)
```

- v1 では**ディレクトリ構成は推奨に留め、`project.json` 内のパスが正**とする
  (規約を強制しない。整理はプロジェクト側の自由)。
- `imports/` だけは DCC bridge / ツール CLI の既定出力先として意味を持たせる。

## 3. パス参照の分類と解決規則(v1 で確定)

すべてのファイル参照は次の 3 種類に分類される。JSON・CLI・将来のコマンド層で共通。

| 形式 | 解決先 | 用途 |
|------|--------|------|
| 素の相対パス(`assets/models/a.glb`)| **常にプロジェクトルート基準**。`project://assets/models/a.glb` の省略形 | 通常のコンテンツ参照(これが既定) |
| `engine://<id>`(例 `engine://passes/bloom.json`)| エンジン埋め込みリソース(b::embed の id 空間にマップ) | 標準パス定義・標準シェーダ・default_config 等 |
| 絶対パス | **CLI 引数限定のデバッグ用**。`--allow-absolute-paths`(既定 OFF)指定時のみ許可し、使用のたびに WARN ログ | ローカル実験。CI・配布物では禁止 |

規則(曖昧さを残さないための決定):

1. **nested JSON の相対基準は「その JSON ファイルの場所」ではなく、常にプロジェクトルート。**
   scene / asset / pass / ui / shader / texture すべての参照に適用(v1 固定)。
   理由: JSON を移動しても参照が壊れない、解決則が 1 つで実装・デバッグが単純。
2. **暗黙のシャドーイングはしない。** プロジェクトに同名ファイルがあっても
   `engine://` 参照は埋め込みを読む。差し替えたいときは参照側を
   `project://` パスに書き換える(どちらを読んでいるかが常に参照を見れば分かる)。
3. **絶対パスの許可は文脈で分ける**: `--allow-absolute-paths` が効くのは
   **CLI 引数として渡されたコンテンツ参照のみ**(`--play-seq` 等の一時指定)。
   `project.json`・scene/asset/pass/ui 等の**永続化された JSON 内の絶対パスは
   フラグに関係なく常に reject**(配布したプロジェクトが他人のマシンで壊れる事故を
   形式レベルで防ぐ)。
   ただし**プロジェクトの外を指すことが本務の起動引数(locator/出力先)は
   この規則の対象外**: `--project <abs>` や `--render-out <abs>` はフラグなしで
   絶対パスを受ける。これらは PathResolver を通らない素のパスであり、
   PathResolver が管轄するのは「プロジェクト内へのコンテンツ参照」だけ、と整理する。
4. **`project://` プレフィックスは v1 で正式対応**: parse して strip し、素の相対パスと
   完全等価に扱う(`project://assets/a.glb` ≡ `assets/a.glb`)。明示したい場面
   (ドキュメント・エラーメッセージ・将来のプロトコル)で使えるようにしておく。
5. **`engine://` の id 規則(v1 確定)**: id = **b::embed のキー文字列**
   (= `src/core/resources/` からの相対パス)。例: `engine://default_config.json`。
   v1 で必要なのは現状 embed されている `default_config.json` のみで、標準 shader /
   標準 pass はシェーダ自由化キット(WP12/13/15)で embed 化する際に同じ規則で
   id が付与される。embed 一覧と id の対応表はビルド時自動生成に将来昇格(§8)。
6. 命名は ASCII(R7 と同じ理由: プロトコル・パス安全)。

## 4. `project.json` v1 と設定の優先順位

```json
{
  "schema": "pelican.project",
  "version": 1,
  "name": "example",
  "generator": "hand-written",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Pelican App",
    "window_size": {"width": 1920, "height": 1080},
    "fullscreen": false,
    "framerate": 60,
    "camera": {"fov_y": 60.0, "near": 0.1, "far": 100.0, "up": [0, 1, 0]},
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main_rendering_config.json",
    "ui_config_json": "ui/ui_overlay.json",
    "default_rendering_pass": "..."
  }
}
```

- `basic_config` の中身は**現行 `default_config.json` / settings_str と同形を維持**。
  追加必須フィールドは `schema` / `version` / `name` のみ(R10 と同じバージョニング流儀)。
- **`engine_min_version` を満たせない場合は hard error で起動しない**("min" の意味論どおり)。
  実験用に `--ignore-engine-version` フラグで WARN への降格を許す(既定 OFF)。
  警告止まりだと「動いたように見えて新機能の挙動だけ壊れる」が起きるため。
- 同様に **`schema` が `"pelican.project"` でない/`version` がエンジンの対応範囲より
  新しい場合も hard error**(読めるふりをしない。`--ignore-engine-version` の対象外)。
- **設定の優先順位(v1 で確定): CLI 引数 > project.json > 埋め込み default。**
  `EngineLaunchConfig`(WP1)が CLI 値を保持し、`ProjectBasicConfig` は読み出し時に
  この順で合成する(現行 JsonLoader の 2 段 fallback を 3 段にする)。
  例: `--size 640x360` は project.json の window_size に勝つ。headless 検証で多用する。
- 肥大化対策(将来課題として予告): basic_config が「起動設定 / コンテンツ参照 /
  デバッグ設定」で膨らんできたら**キー空間で分離**する(ファイル分割より先に)。
  v1 では分割しない。

## 5. エンジン側の実装(WP18 として提案)

依存: WP1(EngineLaunchConfig)。規模: 中。

1. **P0 — `ProjectSource::loadSource()` の path 経路バグ修正**(読んだ `loaded_data` を
   return する。2 行)。WP18 を待たず単独で直してよい
2. **PathResolver モジュール**(`core/loader/pathresolver.{hpp,cpp}`)。

   **戻り値型(確定)**: `engine://` は `std::filesystem::path` に解決できないため、
   resolve の結果は variant で返し、ファイル前提の API と分離する:

   ```cpp
   struct EngineResourceId { std::string id; };               // b::embed のキー
   using ResolvedRef = std::variant<std::filesystem::path, EngineResourceId>;

   DECLARE_MODULE(PathResolver) {
     public:
       // 起動配線が一度だけ呼ぶ。root は存在必須(ここで canonical 化して保持)。
       // allow_absolute_paths は EngineLaunchConfig(--allow-absolute-paths)から注入され、
       // 以後 Resolver 内部の状態となる(呼び出しごとの引数にはしない — 呼び側に判定材料を持たせない)。
       // 二重呼び出しは throw(本番経路の配線ミス検出)。テストは resetForTesting() で再構成する
       void setup(const std::filesystem::path &project_root_abs, bool allow_absolute_paths);
       void resetForTesting();  // テスト専用。本番コードからの呼び出しはレビューで禁止
       // 永続化 JSON・プロジェクト文脈の参照(解決のみ・存在チェックなし)。絶対パスは常に reject
       ResolvedRef resolveProjectRef(std::string_view ref) const;
       // CLI 引数由来の参照。絶対パスは setup で注入された allow_absolute_paths が真のときのみ許可+WARN
       ResolvedRef resolveCliRef(std::string_view ref) const;
       // ファイル限定+存在チェック(プロジェクト文脈)。engine:// や欠落時は「解決後の絶対パス」入りで throw
       std::filesystem::path resolveExistingFile(std::string_view ref) const;
       // file / engine 両対応の統一読み出し(プロジェクト文脈。ほとんどの呼び出し側はこれだけ使う)
       std::string loadText(std::string_view ref) const;
       std::vector<std::byte> loadBytes(std::string_view ref) const;
   };
   ```

   - **参照の由来はフラグ引数ではなくメソッド分離で表す**(`resolveProjectRef` /
     `resolveCliRef`)。絶対パス許可の判定材料が Resolver の外にある状態を作らず、
     呼び間違いがシグネチャに現れるようにする(レビュー 3 巡の指摘)
   - **呼び出し側の原則**: 内容が欲しいだけなら `loadText/loadBytes`(バックエンド非依存)。
     OS パスが本当に必要な箇所(ホットリロードの監視、外部プロセスへのパス渡し)だけ
     `resolveExistingFile` を使う — そこは `engine://` 非対応であることが型と例外で明示される
   - **EngineResourceRegistry(最小 runtime dispatch)**: `b::embed<"...">()` は
     コンパイル時キーのため、文字列 id からの解決には小さな実行時対応表が要る。
     `core/loader/engineresources.{hpp,cpp}` に
     `std::optional<std::string_view> engineResource(std::string_view id)` を置き、
     v1 は `default_config.json` の 1 エントリを手書き登録。未知 id の throw メッセージには
     **登録済み id の一覧**を含める(タイポ即発見)。embed を増やすときはこの表に 1 行
     追加する運用とし、ビルド時自動生成(§8 未決 2)に将来置換する
   - **`engine://` は project root 未設定でも解決できる**(Registry 直のためファイルシステム
     不要)。初期化順で詰まらないことを保証する: `loadText("engine://...")` は
     `setup()` 前でも成功し、プロジェクト相対参照を root 未設定で解決しようとした場合のみ
     明確なエラー(「setup 前」と分かるメッセージ)で throw。なお loader 内部が
     `b::embed<"default_config.json">()` を直接読む現行コードは規律違反ではない
     (Registry は文字列 id を受ける境界のための仕組み)
   - **依存の散逸防止**: `GET_MODULE(PathResolver)` を直接呼んでよいのは
     **`core/loader/` 配下と起動配線のみ**。それ以外(レンダラ・ECS・feature・playback)が
     パス解決を必要とする場合は、§0 共通規則の依存構造体パターン(`XxxDependencies`、
     `implementation_plan.md` §0 参照)で PathResolver か解決済みの値を受け取る。
     直接 GET_MODULE をレビューで弾く

   解決手順(ファイル系):

   ```
   1. scheme 判定(engine:// → EngineResourceId を返して終了)
   2. 絶対パス・UNC(\\server\...)は「CLI 由来 かつ --allow-absolute-paths」のみ許可+WARN。
      永続化 JSON 由来なら常に reject(§3-3)
   3. joined = project_root / ref
   4. canon = std::filesystem::weakly_canonical(joined, ec)   // 例外でなく error_code 版を使う。
      存在しない末尾は字句正規化される(resolve 段階では存在不要)
   5. canon と保持済み canonical(project_root) を **path component 単位**で先頭一致比較。
      Windows では component ごとに case-fold して比較する。
      文字列 starts_with は使わない(C:\proj と C:\project2 を誤一致させるため)
   6. 一致しなければ reject(throw)。".." も symlink/junction 経由の脱出も
      canonical 化後の判定なのでここで落ちる
   ```

   実装注意(レビュー指摘の事故ポイント): Windows の case-insensitive 比較、
   symlink / junction、UNC、長パスプレフィックス(`\\?\`)、`weakly_canonical` の
   error_code 処理。判定は必ず**正規化後の path component 列**に対して行い、
   入力文字列への `..` 字句検査や文字列 prefix 比較で済ませない
3. **CLI**: `--project <dir | path/to/project.json>`(WP1 の argparse に追加)。
   **省略時は exe のあるディレクトリを暗黙プロジェクトとし、起動ログに
   `implicit project root = <path> (pass --project to silence)` を WARN で必ず出す**。
   ctest・golden テスト・DCC bridge からの起動は `--project` 明示を必須とする
   (テスト規約に追記。暗黙モードはあくまで人間の互換起動用)
4. **ProjectBasicConfig / ModelAssetContainer / シーン・UI・パス読み込みの
   ファイルアクセスを PathResolver 経由に置換**(cwd 依存の根絶)
5. **example プロジェクトの切り出し**: `src/player/resources/` → `projects/example/` に
   §2 レイアウトで移設。POST_BUILD コピーは**削除**(2026-06-12 に入れた
   バイナリコピー拡張は本 WP までの応急処置)
6. **`projects/example/README.md` を必須成果物にする**: 期待バイナリアセットの
   一覧表(ファイル名 / 入手元 / sha256 / サイズ)を記載。JSON は git 管理、
   バイナリは ignore 継続。「環境によって黒背景」の再発防止はこの表が第一防衛線。
   将来は `assets.manifest.json` + 起動前検証(`--check-assets` or pelican_cli doctor)に
   昇格させる(v1 ではやらない。代わりに 7-c のエラー仕様で補う)

### 受け入れ基準(WP18)

- a. **任意の cwd から** `pelican_player --project <repo>/projects/example` で 3D シーンが表示される
- b. `--project` なしの互換起動も従来どおり動き、暗黙プロジェクトの WARN ログが出る
- c. アセット欠落時のエラーメッセージに**解決後の絶対パス**が含まれる
  (どこを探して無かったのかが一目で分かる)
- d. `../outside.png` のような脱出参照が reject される(単体テスト。symlink 経由も)
- e. scene / asset / pass / ui 内の参照がプロジェクトルート基準で解決される(単体テスト)
- f. POST_BUILD コピーが削除されている
- g. 永続化 JSON 内の絶対パスが `--allow-absolute-paths` の有無に関係なく reject される(単体テスト)
- h. `engine_min_version` が現行より新しい project.json は起動拒否、
  `--ignore-engine-version` で WARN 降格(単体テスト)
- i. `schema` 不一致・`version` 超過の project.json が起動拒否される
  (`--ignore-engine-version` でも降格しない)(単体テスト)
- j. 未知の `engine://` id が登録済み id 一覧入りのメッセージで throw される(単体テスト)
- k. `project://assets/a.glb` と `assets/a.glb` が同一に解決される(単体テスト)。
  `--project` の絶対パス指定は `--allow-absolute-paths` なしで通る

### 後続(WP18 のスコープ外、設計だけ整合)

- WP14(ホットリロード)の監視対象 = プロジェクトの `shaders/`(`engine://` シェーダは対象外)
- WP17(SeqPlayer)の `--play-seq` 相対パスはプロジェクトルート基準。
  「CLI 由来+存在チェック」が必要になるため **`resolveExistingCliFile`** を WP17 側で追加する
  (名前だけここで予約。WP18 には含めない)
- コマンド層 stage 3 の `load_gltf {path}` はプロジェクト相対のみ受ける
  (PathResolver の脱出防止がそのまま外部入力の防壁になる)
- devstudio は起動時にプロジェクトを開く(ダイアログ+最近使ったプロジェクト)
- 配布パッケージ化(プロジェクトの zip/pak 化)は需要が出るまで設計しない

## 6. ツール連携との接続

- DCC bridge(M1)の「出力先」と SeqPlayer の入力は**プロジェクトの `imports/`** が既定。
  Blender 側 UI は「プロジェクトディレクトリ」を 1 つ覚えるだけでよくなる
- mocap lab / droplet_lab 等のツール CLI は出力先パスを受けるだけ(現行仕様のまま)。
  プロジェクトを知る必要はない — 知るのは呼び出し側(人間 or bridge)
- 将来の `pelican_cli import`(納品物を asset_data_json に登録する補助)は需要が出てから

## 7. Git と配布の扱い

- **エンジンリポジトリ**: `projects/example/` の JSON + README(アセット一覧表)のみコミット。
  バイナリアセットは ignore 継続。Git LFS はエンジン repo では使わない(クローンを重くしない)
- **ユーザプロジェクト**: エンジンとは独立のディレクトリ/リポジトリ。LFS 採用は
  プロジェクト側の自由
- エンジンとプロジェクトのバージョン整合は `engine_min_version` の **hard error ゲート**(§4)
  で行う(破壊的変更は schema version で管理。R10 と同じ運用)

## 8. 決定済み事項と未決事項

レビュー(2026-06-12・全 5 巡)で確定したもの:

- nested JSON の相対基準 = **常にプロジェクトルート**(ファイル基準は採用しない)
- パス分類 = 素の相対(=project://)/ `engine://` / 絶対(CLI 限定デバッグ・要フラグ)
- **絶対パスの文脈分離** = CLI 引数のみフラグで許可、永続化 JSON 内は常に reject
- 設定優先順位 = **CLI > project.json > embedded default**
- `--project` 省略 = 互換起動として許可、ただし WARN ログ必須+CI/テストでは明示必須
- escape 判定 = weakly_canonical(error_code 版)正規化後、**path component 単位**の
  先頭一致(Windows は case-fold。文字列 starts_with 禁止)
- **PathResolver の戻り値** = `variant<fs::path, EngineResourceId>`。
  参照の由来は **メソッド分離**(`resolveProjectRef` / `resolveCliRef`)で表し、
  ファイル限定+存在チェックの `resolveExistingFile` / 統一読み出しの
  `loadText・loadBytes` を分離
- **`engine://` の id** = b::embed のキー(`src/core/resources/` 相対)。
  v1 必須は `engine://default_config.json` のみ。文字列 id → embed の実行時対応は
  **EngineResourceRegistry**(v1 手書き 1 エントリ、未知 id は登録済み一覧入りで throw)
- **PathResolver の利用範囲** = `GET_MODULE` 直呼びは core/loader と起動配線のみ。
  他層は依存構造体(`XxxDependencies`)経由
- **`engine_min_version` 不適合は hard error**(`--ignore-engine-version` で WARN 降格)。
  **`schema` 不一致・`version` 超過も hard error**(降格フラグの対象外)
- **locator 引数(`--project`・`--render-out` 等)は絶対パス規則の対象外**
  (PathResolver を通らない。管轄は「プロジェクト内へのコンテンツ参照」のみ)
- **`project://` プレフィックスは v1 正式対応**(parse して strip、素の相対と完全等価)
- **setup 二重呼び出しは throw、テストは `resetForTesting()`**(本番からの呼び出し禁止)

未決(実装前に決めなくてよいもの):

1. シーン JSON の分割(1 シーン 1 ファイル)を v1 でやるか(本書は据え置きを提案)
2. embed 一覧 ↔ `engine://` id の対応表のビルド時自動生成(規則は確定済み、生成は将来)
3. devstudio のプロジェクト UX(最近開いた一覧など)の優先度
4. assets.manifest.json(チェックサム検証)への昇格時期
