# プロジェクトファイル設計(エンジンとコンテンツの分離)

対象読者: エンジン担当 + DCC/ツール側担当。
ステータス: ドラフト(レビュー待ち)。
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
| バイナリアセットは手コピーで exe 隣に配置(POST_BUILD コピーは応急処置) | クリーンで消える/マシン間で揃わない/git に置けない |
| `ProjectSource::loadSource()` の path 経路は**読み込んだデータを返さない**(`loaded_data` を構築後に破棄して fallthrough) | 潜在バグ。現状 raw_data(settings_str)経路しか機能していない |

## 1. 原則

1. **プロジェクト = 1 ディレクトリ。** ルートに `project.json`(マニフェスト)。
   これを「開く・実行する・配布する」の単位にする。
2. **パス解決はすべてプロジェクトルート相対。cwd は一切参照しない。**
   エンジン内のファイルアクセスは PathResolver(§4)経由に統一する。
3. **エンジン既定リソースは embed 継続。** プロジェクト側に同名設定があれば上書き
   (現行の JsonLoader の fallback 構造をそのまま流用)。
4. **「機能はなるべくアセットに」(ロードマップ §6)との接続**: パス定義 JSON・
   ランタイムコンパイルシェーダ・UI 定義もプロジェクト側に置けるようにする。
   エンジンの資産(bloom 等の標準パス)は embed、プロジェクトはそれを参照 or 差し替え。
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

## 3. `project.json` v1

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

- `basic_config` の中身は**現行 `default_config.json` / settings_str と同形を維持**
  (ProjectBasicConfig の読み手がほぼそのまま使える)。
- 追加必須フィールドは `schema` / `version` / `name` のみ(R10 と同じバージョニング流儀)。
- `asset_data_json` が指すファイル内のモデルパス(`AliciaSolid.vrm` 等)も
  プロジェクトルート相対に統一する。
- 命名は ASCII(R7 と同じ理由: プロトコル・パス安全)。

## 4. エンジン側の実装(WP18 として提案)

依存: WP1(EngineLaunchConfig)。規模: 中。

1. **P0 — `ProjectSource::loadSource()` の path 経路バグ修正**(読んだ `loaded_data` を
   return する。2 行)。これは WP18 を待たず単独で直してよい
2. **PathResolver モジュール**(`core/loader/pathresolver.{hpp,cpp}`):
   `setProjectRoot(abs_path)` / `resolve(rel) -> abs`。プロジェクトルート外への
   `..` 脱出は reject(将来のコマンド層で外部入力がパスに乗るため、今から閉じる)
3. **CLI**: `--project <dir | path/to/project.json>`(WP1 の argparse に追加)。
   省略時は「exe のあるディレクトリ」をプロジェクトルートとみなす(後方互換。
   現行の exe 隣コピー構成がそのまま「暗黙のプロジェクト」になる)
4. **ProjectBasicConfig / ModelAssetContainer / シーン・UI・パス読み込みの
   ファイルアクセスを PathResolver 経由に置換**(cwd 依存の根絶)
5. **example プロジェクトの切り出し**: `src/player/resources/` → `projects/example/` に
   §2 レイアウトで移設(JSON は git 管理、バイナリは現行どおり ignore)。
   POST_BUILD コピーは**削除**(`--project` が刺されば不要になる。2026-06-12 に入れた
   バイナリコピー拡張は本 WP までの応急処置)
6. 受け入れ基準: **任意の cwd から** `pelican_player --project <repo>/projects/example`
   で 3D シーンが表示される。`--project` 省略の従来起動も従来どおり動く

### 後続(WP18 のスコープ外、設計だけ整合)

- WP14(ホットリロード)の監視対象 = プロジェクトの `shaders/`
- WP17(SeqPlayer)の `--play-seq` 相対パスはプロジェクトルート基準
- コマンド層 stage 3 の `load_gltf {path}` はプロジェクト相対パスのみ受ける
  (PathResolver の脱出防止がそのまま効く)
- devstudio は起動時にプロジェクトを開く(ダイアログ+最近使ったプロジェクト)
- 配布パッケージ化(プロジェクトの zip/pak 化)は需要が出るまで設計しない

## 5. ツール連携との接続

- DCC bridge(M1)の「出力先」と SeqPlayer の入力は**プロジェクトの `imports/`** が既定。
  Blender 側 UI は「プロジェクトディレクトリ」を 1 つ覚えるだけでよくなる
- mocap lab / droplet_lab 等のツール CLI は出力先パスを受けるだけ(現行仕様のまま)。
  プロジェクトを知る必要はない — 知るのは呼び出し側(人間 or bridge)
- 将来の `pelican_cli import`(納品物を asset_data_json に登録する補助)は需要が出てから

## 6. Git と配布の扱い

- **エンジンリポジトリ**: `projects/example/` の JSON のみコミット。バイナリアセットは
  ignore 継続(入手方法を projects/example/README に明記)。Git LFS はエンジン repo では
  使わない(クローンを重くしない)
- **ユーザプロジェクト**: エンジンとは独立のディレクトリ/リポジトリ。LFS 採用は
  プロジェクト側の自由
- エンジンとプロジェクトのバージョン整合は `engine_min_version` + 起動時警告(将来は
  schema version で破壊的変更を管理。R10 と同じ運用)

## 7. 未決事項(レビューで決めたい)

1. `project.json` 1 ファイル集約か、`basic_config` を別ファイル参照にするか
   (本書は 1 ファイル集約を提案 — 小さいプロジェクトで散らばらない)
2. シーン JSON の分割(1 シーン 1 ファイル)を v1 でやるか
   (現行 `example_scene_data.json` は全シーン 1 ファイル。本書は据え置きを提案)
3. `--project` の既定値: exe ディレクトリ(本書の提案)か、必須引数にするか
4. devstudio のプロジェクト UX(最近開いた一覧など)の優先度
