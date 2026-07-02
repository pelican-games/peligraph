# プロジェクト解釈レイヤ: エンジンとの疎結合化

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-02。レビュー前)。
前提: [PF] v6 凍結、[PFW] v1.3 凍結、WP18a/b 実装済み(レビュー時点)、
`design_project_dcc_houdini.md`・`design_asset_format_policy.md`(v1 ドラフト)。

## 0. 動機

プロジェクト形式の解釈をゲームエンジンから疎結合にする。構造は 3 層:

```
プロジェクト形式(ファイル群)
    ↓
[解釈レイヤ]  pelican_project ライブラリ(エンジン非依存)
    パース・検証・パス解決・正規化 → 中間表現(plain struct)
    ↓
[バインダ]    エンジン側の薄い変換(core/loader)
    中間表現 → エンジンモジュール(RenderingPass / Material / UI / ECS)
    ↓
ゲームエンジン本体
```

新しい形式機能(stem・VAT・import manifest・pointcache)は
**「解釈レイヤにパーサを足す + バインダを足す」の対**で追加され、
エンジン内部には波及しない。

## 1. 現状観察(2026-07-02)— なぜ今か

1. **web は既にこの構造で動いている**: pelican-webgpu は
   parser / projectLoader(解釈。純 TS・fetch 注入)→ 正規化構造体
   (`PelicanLoadedProject`)→ webgpuAdapter(束縛)と分かれ、
   エンジンなしで 22 テストが回る。本書はネイティブ側を同型に収斂させる話。
2. **WP18b で解釈と束縛の交差が始まった**: `basicconfig.cpp` の
   `rewriteAssetPaths` / `resolveShaderPaths` / `rewriteUiPaths` は
   「JSON 文字列を受け、パス参照を解決済み絶対パスに書き換えた JSON 文字列を
   返す」変換で、まさに解釈レイヤの仕事。ただし現状は無名名前空間の関数として
   モジュール(ProjectBasicConfig)内に堆積しており、形式機能が増えるたびに
   ad-hoc rewrite が積み上がる構造になっている。動くが、スケールしない。
3. **エンジン内に先例がある**: DeletionQueueCore(WP9)=
   純ロジッククラス + 薄いモジュールラッパ。pathresolver_test /
   projectconfig_test が GPU 不要で回るのも解釈が本来純粋である証拠。

## 2. 提案

### 2.1 pelican_project ライブラリ(解釈レイヤ)

- CMake 静的ライブラリターゲット `pelican_project`(置き場所は §5 未決 1)。
  **許可依存: std / std::filesystem / nlohmann::json のみ。**
  vulkan.hpp・ECS・quill・モジュール機構(container.hpp)・b::embed に依存しない
  (ログはエラー文字列と戻り値で表現。embed への解決はバインダ側から
  関数注入で受ける)
- 内容:
  - project.json のパース・検証(schema/version/engine_min_version ゲート)
  - 設定 3 段マージ(CLI > project > default。default の実体は注入)
  - パス解決の純ロジック(現 PathResolver の中身。モジュールラッパは
    エンジン側に残す — DeletionQueueCore 方式)
  - scene / asset / rendering config / ui の**構造体へのパース**
    (JSON 文字列の再 dump ではなく plain struct + 解決済み参照)
  - schema 付き交換形式のパーサ(transform_seq は既に分離実装済み=WP17 の
    plain クラス。vat / pelican.import / pointcache も同様にここへ)
- 出力 = **正規化中間表現**: `ResolvedProject`(名称仮)。
  web の `PelicanLoadedProject` と概念対応(フィールド名まで揃えなくてよい)

### 2.2 バインダ(エンジン側)

- `core/loader` に残る薄い層。中間表現を受けてエンジンモジュールに流し込む。
  `GET_MODULE` を呼んでよいのはここと起動配線のみ([PF] §5 の規律を継承)
- 新形式のバインダ例: VAT → MaterialContainer/頂点シェーダ登録、
  import manifest → `pelican_cli import`(バインダをエンジン外ツールが
  使う初のケース。ライブラリ分離の直接の受益者)

### 2.3 拡張規約

- パーサの登録は**静的表**(schema 文字列 → パース関数)。
  プラグイン機構・動的ロードは作らない(過剰抽象化の防止)
- web は同じ形式に対しバインダ相当(webgpuAdapter)を必要な分だけ実装する。
  **サブセット原則([PFW] §1-2)が層構造としてそのまま現れる**

## 3. 移行計画(ビッグバン禁止。挙動不変を golden で担保)

1. **WP18c(example 切り出し)は現行のまま進める**(本書を待たない)
2. **WP19(stem 解決)を新レイヤ最初の住人にする**: stem 参照は
   拡張子なしのため `resolveExistingFile` による書き換え方式と両立しない
   (実在ファイル名が確定しない)。どのみち解釈コードを触るので、
   このタイミングで rendering config の解釈を「JSON→JSON 書き換え」から
   「struct へのパース + 解決済み参照」に寄せる(§5 未決 3 の範囲で)
3. 小 WP で CMake ターゲット分離: pathresolver 純ロジック・basicconfig の
   解釈部・transform_seq パーサを `pelican_project` へ移動(コード移動 +
   薄い委譲のみ。WP3 の流儀)
4. 以後、devcli / `pelican_cli import`(WP21)/ devstudio は
   `pelican_project` だけをリンクしてプロジェクトを解釈できる

## 4. 効果

- 適合性 fixture([PFW] §6)のテスト対象がライブラリ層と一致し、
  「C++ 解釈と TS 解釈が同じ入力で同じ合否」の検証がエンジン抜きで完結する
- 形式の進化(DCC 文書・フォーマット方針の各追加)がエンジン内部に波及しない
- ツール類(import/doctor/devstudio)がエンジン起動なしで形式を扱える

## 5. 未決事項

1. ライブラリの置き場所: `src/project/`(トップレベル)か `src/core/project/`
   (core 内サブターゲット)か。トップレベルを推奨(依存方向が名前で分かる:
   project → なし、core → project)
2. PathResolver モジュールラッパの残し方(setup/reset の寿命管理はエンジン側の
   関心事なのでラッパ残置を推奨。純ロジックだけ移す)
3. 中間表現の粒度: v1 では rendering config と asset・ui のパス参照部分のみ
   struct 化し、scene JSON は当面文字列パススルーを許す(scene スキーマは
   RenderWorld トラックで動くため、先に固めない)
4. 「汎用シーングラフ」への肥大防止: 中間表現には**バインダが今使う
   フィールドだけ**を持たせる(将来のためのフィールドを先置きしない)
