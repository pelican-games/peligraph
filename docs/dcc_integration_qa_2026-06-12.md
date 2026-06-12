# DCC 連携 Q&A と決定事項(2026-06-12)

対象読者: pelican2 実装担当(Codex)+ ツール側担当(Claude / 人間)。
実装者レビュー(10 項目)への回答。決定は本書、契約への反映は
`docs/external_tools_requirements.md`(R4/R5/R8 を 2026-06-12 に追補済み)。

## 0. 決定サマリ

| # | 問い | 決定 |
|---|------|------|
| 1 | DCC プラグイン MVP | **export + headless preview ボタン**(ファイル渡し)。live link は抽象だけ用意 |
| 2 | プロトコル | **最初から JSON-RPC 2.0、NDJSON フレーミング、stdio 先行**(独自行プロトコルは作らない) |
| 3 | 対文書の扱い | **契約はコピー+正本明記、設計文書は参照のみ**。当該ファイルは実在(未コミットが原因) |
| 4 | E0 の前提 | 「作業なし」は言い過ぎ。**受け側 API は現行で存在、ただし SeqPlayer(小 WP)が必要**。WP17 として定義(§4) |
| 5 | scale [0,0,0] | **不採用**。フレーム行のオプション `"hidden": [index...]` に変更(R4 追補済み) |
| 6 | VAT 仕様 | v1 を明文化(§6)。E1 の「ほぼなし」は楽観的で、**小さな C++ glue WP が 1 つ要る** |
| 7 | cloth_setup | 正本=ツール側 schemas/。**エンジンは validator を持たない**(tolerant reader)。参照は**ノード名**(一意性必須) |
| 8 | ClothWorld | `core/phys` は誤記。**`src/feature/phys/`(新設)の feature 層**。step はアプリ層が呼ぶ |
| 9 | DeletionQueue | **plain class `DeletionQueueCore` + 薄いモジュールラッパに分離**。単体テストは GPU 不要のまま |
| 10 | パッケージ | bridge アドオンは **pelican2 リポ所有・単一ファイル・依存ゼロ・Blender 4.2 LTS+**(§10) |

---

## 1. DCC プラグインの MVP

### 所有とスコープ

- **エンジン対応プラグインは pelican2 リポジトリが所有する**: `dcc/blender_pelican2_bridge/`(新設)。
  契約相手がエンジン CLI なので、CLI フラグ変更と同じ PR でアドオンを直せるのが利点。
- ツール側アドオン(`mmd_mocap_bridge` 等)は各ツールのエクスポート/編集 UI に専念し、
  エンジンプレビューはこの bridge に任せる(役割が混ざらない)。

### MVP 定義 = 「export は Blender 標準に任せ、プラグインの価値は preview ボタン」

「glTF を書き出すだけ」のアドオンは作らない。glb 出力は Blender 標準エクスポータ、
transform_seq はツール CLI が書く。プラグイン固有の価値は
**「Blender を離れずに Pelican2 の絵で確認する」**であり、これを MVP とする。
live link は最初は**考慮のみ**(下記 M2)。

マイルストーン(エンジン側 WP に対応):

| 段階 | 内容 | エンジン側前提 |
|------|------|----------------|
| M0(配管) | "Render in Pelican2" ボタン: `pelican_player --headless --frames N --size WxH --render-out tmp/%04d.png` を subprocess 実行 → 完了後 Blender の Image Editor に表示。レンダ対象はエンジン同梱シーン | WP6 |
| **M1 = 公開 MVP** | transform_seq(droplet / cloth C2 出力)+ メッシュ指定で連番レンダ → Blender で確認。「ツール成果物をエンジンの絵で見る」最初の本物の体験 | WP6 + **WP17(§4)** |
| M2(live) | 同じ UI の裏側を stdio JSON-RPC セッションに差し替え、タイムラインスクラブ(`set_time` → `render_frame` → `capture`) | コマンド層 stage 2 |
| M3 | `load_gltf` でキャラ込みシーンプレビュー | RenderWorld + スケルタル |

### live link を「考慮だけ」で崩さない仕掛け

アドオン内部に `EngineSession` 抽象を 1 つ置き、operator はこれしか触らない:

```python
class EngineSession:
    def run_oneshot(self, args: list[str]) -> RunResult: ...   # M0/M1: subprocess 一発
    # M2 で追加: open() / call(method, params) / close()       # stdio JSON-RPC
```

subprocess は modal operator + timer でポーリングし UI をブロックしない。
stderr は Blender テキストブロックに保存(失敗時の一次情報)。

### M1 の入力 UI(2 巡目で確定)

設定は「マシン固有」と「ショット固有」で置き場所を分ける:

| 置き場所 | 持つもの | 理由 |
|----------|----------|------|
| Addon Preferences | エンジン exe パス、一時出力ディレクトリ、タイムアウト秒 | マシン固有。.blend に入れない(共有時に他人のパスが混入しない) |
| Scene の PropertyGroup + 3D View サイドバー「Pelican2」パネル | seq path(`.jsonl`)、mesh source(enum: `builtin:sphere` / glb パス)、出力先 override(空なら一時dir) | ショット固有。.blend に保存され、ファイルを開き直しても残る |
| シーンから自動取得(UI に出さない) | frames(シーンの frame range)、fps(`scene.render.fps`)、size(`scene.render.resolution_x/y`) | Blender 側の設定と二重管理しない。operator が CLI 引数に転記するだけ |

operator(Render in Pelican2)は上記を読んで CLI 引数を組むだけで、独自状態を持たない。
mmd_mocap_bridge の既存パターン(Scene PropertyGroup + パネル + operator)と同じ構造。

## 2. JSON-RPC と stdio 行 JSON の整合

**決定: 最初から「stdio 上の JSON-RPC 2.0、NDJSON フレーミング」。** 両文書の表記は
矛盾ではなく曖昧さだったので、次のとおり確定する(R8 追補済み):

- 1 行 = 1 個の完全な JSON-RPC 2.0 オブジェクト(request または response)。
  改行を含まない UTF-8 JSON。Content-Length(LSP 流)フレーミングはやらない
- コマンド層 stage 2 の実装も独自行プロトコルではなく JSON-RPC エンベロープ
  (`{"jsonrpc":"2.0","id":1,"method":"set_time","params":{"t":1.5}}`)。
  コストは 1 行あたり数十バイトで、id による相関・error object 規約・将来の
  TCP 化(フレーミング不変でソケットに載せ替え)が無償で手に入る
- ロードマップの「JSON-RPC サーバはまだ作らない」は「**TCP 常駐サーバはまだ**」と読み替える
  (`design_roadmap_renderworld.md` §2.1 / `implementation_plan.md` §3 の文言は次回改訂で修正)
- notification は使わない(全部 request/response)。アプリ固有エラー code は -32000〜-32099

## 3. 対文書(クロスリポジトリ参照)の扱い

まず事実関係: `mmd_blender_mocap_lab/docs/cloth_design_2026-06-12.md` は**実在する**が
**git 未コミット(untracked)**だった。見つからなかった原因はこれ(ツール側でコミット予定)。

規約として確定する:

- **契約はコピー、設計は参照。**
  - 契約文書 = `external_tools_requirements.md` と `schemas/*.json`。両リポにコピーを置き、
    冒頭に正本を明記(requirements の正本は pelican2、スキーマの正本はツール側 schemas/)。
    変更は「正本を更新 → コピーへ同期」の順。同期日を正本明記行に添える
  - 設計文書(`design_*.md`、`*_design_*.md`)はコピーしない。
    「リポ名 + 相対パス + 日付」で参照し、コミット後はハッシュを添えてよい。
    コピーすると乖離が必ず起きる(今回の scale 0 のように、議論中の案が
    片側で確定扱いになる事故を防ぐ)
- 参照を書く側の義務: 参照先が**未コミットでないこと**を確認してから参照を書く

## 4. E0「エンジン側作業なし」の正確化 + WP17: SeqPlayer

### 分けて書くと

- **現行エンジンのまま**: 受け側 API は存在する
  (`GltfLoader::loadGltfBinary` → `ModelTemplate`、`PolygonInstanceContainer::placeModelInstance` / `setTrs`)。
  ただし「transform_seq ファイルを読んで毎フレーム流す」**シーケンスプレイヤーは未実装**。
  よって E0 の正しい表現は「**新規描画機能・ECS 変更は不要。ただし小さな glue WP
  (SeqPlayer)が 1 つ必要**」
- **RenderWorld 後**: SeqPlayer の書き込み先が `setTrs` ループから
  `updateTransforms(span)` 一括に変わるだけ。プレイヤー実装は無駄にならない

`design_cloth_simulation.md` §2 の E0 行は上記に合わせて修正すること(§11 の編集リスト参照)。

### WP17: SeqPlayer(提案 — implementation_plan へ追記)

依存: WP1(CLI)、WP2(EngineTime)。規模: 中の小。リスク: 低。
**SeqPlayer 単体は WP2 完了後に実装可能で、通常ウィンドウ起動で動作確認できる。
DCC bridge M1 として成立するのは WP6(headless PNG)+ WP17 の両方完了後**。
droplet/cloth の最初の納品経路の前提でもある。

1. `src/core/playback/seqplayer.{hpp,cpp}`(core モジュール。`SceneLoader` が core/loader に
   居る前例に合わせる)。JSONL パースとサンプリングは**モジュール非依存の plain 関数/クラス**に
   分離し、GPU 不要で単体テスト可能にする(WP9 と同じ分離方針)
2. CLI(WP1 の argparse に追加): `--play-seq <path.jsonl>`、`--seq-mesh <builtin:sphere|path.glb>`、
   `--seq-loop`(flag)
3. 起動時: ヘッダ行を検証(schema/version 不一致は fail-fast)、objects ごとに
   `placeModelInstance`。v1 は**全オブジェクト共有メッシュ 1 つ**(builtin:sphere は
   単位球を生成、glb 指定時は最初のメッシュ)。オブジェクト名→glb ノード名の個別対応は v2
4. 毎フレーム(`ecs.update()` 後・`renderer.render()` 前に Loop から呼ぶ。core→core なので
   レイヤ規則に抵触しない): `t = EngineTime.now()` で該当サンプル行を選択
   (v1 は floor サンプル・補間なし、末端は clamp、`--seq-loop` で周回)、全インスタンスに `setTrs`
5. `hidden`(R4 追補)の扱い: 契約は「描かない」。現行の indirect draw 構成で
   インスタンス単位スキップが重い場合、**v1 の内部実装としては scale 1e-6 への縮退で代用してよい**
   (0 ではなく ε にするのは法線行列の特異化を避けるため)。RenderWorld の
   draw command seam が入った時点で真の draw skip に置換。契約(ファイル形式)は変わらない
6. テスト: (a) パース+サンプリング単体(GPU 不要、固定 fixture)、
   (b) headless 結合: 2 オブジェクト×3 フレームの golden jsonl → `--render-out` 連番で
   位置が動くこと(WP7 の流儀)

受け入れ基準: `pelican_player --headless --frames 90 --play-seq droplets.jsonl --seq-mesh builtin:sphere --render-out out/%04d.png` で球が動く連番が出る。通常起動無変更。

## 5. scale [0,0,0] = 非表示 → 不採用

指摘のとおり。決定済み(R4 追補):

- フレーム行にオプション `"hidden": [3, 17]`(ヘッダ objects の添字リスト)。省略時は全可視
- 不採用理由: (i) 法線行列(model 行列の逆転置)が特異になり NaN の温床、
  (ii) 将来の culling / bounds / skinning / 物理が scale を読むとき意味が混線、
  (iii) 「見えないが存在する」と「存在しない」の区別が消える
- 非表示中も transforms には有効値(直前値保持で可)を入れる(配列対応の単純さを維持)
- ツール側文書(tool_suite_design §4.4)は同期修正済み

## 6. `pelican.vat` v1 仕様(確定案)

R5 追補に要点を記載済み。完全版:

| 項目 | 仕様 |
|------|------|
| 意味論 | ベース glb の POSITION(任意で NORMAL)を**置換**する。トポロジ・インデックス・UV・頂点数・**頂点順序**はベース glb と完全固定。ベイカーは glTF primitive の頂点順をそのまま使い、メタに vertex_count を書く。再生側は不一致で fail-fast |
| 座標空間 | **メッシュノードのローカル空間**(ワールドではない)。ノード TRS は通常どおり適用される=インスタンスごと配置可能。glTF 規約(R6) |
| 正規化 | `texel = (pos - bounds_min) / (bounds_max - bounds_min)`、RGBA16F(A 未使用)。bounds はメタに記載 |
| レイアウト | 幅 = vertex_count(**v1 上限 8192**。超過はベイカー側でメッシュ分割。2D タイル化は v2)、高さ = frame_count、1 テクセル = 1 頂点 1 フレーム |
| frame sampling | 等間隔 fps(メタ記載)。`row_f = clamp(t * fps, 0, frame_count - 1)`。**シェーダは floor(row_f) と ceil(row_f) の 2 行を texelFetch し mix で補間**。サンプラは NEAREST(ハードウェア線形は頂点方向に滲むため禁止) |
| 時間 | `t = EngineTime.now() - clip_start`(uniform で供給)。末端 clamp、`loop: true` で周回 |
| 法線 | optional の第 2 テクスチャ(RGB16F 生値、同レイアウト)。**タンジェントは v1 非対応**と明記(異方性ハイライトなしのトゥーン/布では許容) |
| 格納 | テクスチャ実体は **glb バッファ内 bufferView**(raw half float、little endian)。extras `pelican.vat` が bufferView index を指す。EXR/KTX2 依存を作らない |
| extras の配置 | **対象 mesh primitive の extras**(`meshes[i].primitives[j].extras["pelican.vat"]`)を正とする。理由: vertex_count の照合対象が primitive 単位、メッシュを複数ノードで共有しても壊れない、material は外観であって形状ではないので不適、root extras は対象への間接参照が必要になるだけで利点がない。読み手は primitives を走査して発見する |
| 多重クリップ | **1 primitive につき 1 クリップ(v1 で複数クリップは禁止)**。別テイク・別カットは別 glb にする。1 つの glb 内に VAT 付き primitive が複数あるのは可(衣装+髪など) |
| メタ | `{schema:"pelican.vat", version:1, generator, fps, frame_count, vertex_count, bounds_min, bounds_max, loop, position_view, normal_view?}` |

### E1「ほぼなし」の正直な再見積もり

シェーダとパス JSON はアセットで済むが、C++ glue が確実に残る:

1. raw half float bufferView → `ImageWrapper`(R16G16B16A16_SFLOAT)アップロード経路
   (現状の画像ロードは stb の 8bit PNG 想定のはず)
2. extras `pelican.vat` のパースと、bounds / fps / frame_count / 時刻の uniform 供給
3. マテリアルパスへのテクスチャ/uniform バインド(WP15 の desc 一般化に乗れる)

規模は**小 WP 1 個**(シェーダ自由化キット後)。`design_cloth_simulation.md` §2 の
E1 行は「ほぼなし」→「アセット+小 glue WP(VAT テクスチャアップロードとメタ読み)」に
修正すること。

## 7. `cloth_setup` の所有権・validator・参照方式

- **正本**: ツール側 `mmd_blender_mocap_lab/schemas/cloth_setup.schema.json`(将来 cloth_lab へ
  移管可)。pelican2 はコピー+正本明記(§3 の規約どおり)
- **エンジンは JSON Schema validator を持たない。** プロデューサ検証原則:
  書き手(ツール CLI)が書き出し直後に検証し、ツール側 CI でも検証する(R9)。
  エンジンは tolerant reader — 必須フィールド欠落・型不一致は fail-fast で throw、
  **未知フィールドは無視**(前方互換)。エンジンのテストフィクスチャは
  ツール CLI で検証済みのものをコミットする
- **参照方式: ノード名(文字列)参照。** glTF はアセット合成・再エクスポートで
  index が振り直されるが名前は保たれるため。制約:
  - cloth_setup から参照されるノード名は **glb 内で一意**(スキーマ要件。
    ツール側 validator が一意性も検査。ASCII は R7 で保証済み)
  - collider は setup 内ローカルな文字列 id を持ち、chains からは id で参照
  - チェーンのジョイント列は配列順 = 親→末端が正
  - 参考: VRM の `VRMC_springBone` はノード index 参照だが、本資産は再合成耐性を優先して
    意図的に名前参照にする(VRM からの変換ツールが index→名前を解決する)

## 8. ClothWorld の配置(層の整理)

指摘のとおり `core/phys/clothworld.hpp` は矛盾(本文で「feature 層」と言いながら core パス)。
確定:

- **`src/feature/phys/clothworld.{hpp,cpp}`**。`src/feature/` を新設し、CMake ターゲットを
  分割(`pelican_feature_phys` → `pelican_core` にリンク。逆方向は禁止)。
  現在空の `src/core/phys/` は E3 着手時に削除
- リンク方向: feature → core(RenderWorld、EngineTime)は適法。
  **core から feature を `GET_MODULE` したら違反**(ロードマップ §6 の規律そのまま)
- `step()` の呼び出し元: core の Loop からは呼べない(core→feature 違反)ので、
  **アプリ層(player のシーン側)または ECS のゲームロジック update が呼ぶ**。
  v1 は「player が ecs.update() 後に明示的に呼ぶ」で開始し、汎用の
  feature update 登録機構は **2 つ目の feature が現れてから**作る(肥大化対策の規律と同じ)
- `rigidResults()` → `RenderWorld::updateTransforms` は feature→core 呼び出しで適法

## 9. DeletionQueue の GPU 不要テスト

指摘は正しい。ctor 直 `GET_MODULE(VulkanManageCore)` だと単体テストで Vulkan 初期化が走り、
GPU なし環境で成立しない。**ロジックと寿命ピン留めを分離する**(WP9 仕様を以下に改訂):

```cpp
// 純ロジック。モジュール機構・Vulkan に非依存 → GPU 不要で単体テスト可能
class DeletionQueueCore {
  public:
    DeletionQueueCore(uint32_t in_flight_frames, std::function<void()> wait_idle_hook);
    template <class T> void defer(T &&resource);
    void beginFrame();
    void flushAll();              // 契約: 呼び出し前に wait idle 済み
    ~DeletionQueueCore();         // pending 非空なら wait_idle_hook() + flushAll + 警告ログ
};

DECLARE_MODULE(DeletionQueue) {   // 薄いラッパ。寿命ピン留めはこちらだけが担う
    DeletionQueueCore core;
  public:
    DeletionQueue();              // ここで GET_MODULE(VulkanManageCore) を呼び
                                  // 「core より後に生成」をピン留めし、
                                  // 実 waitIdle を hook として core に注入
    // defer / beginFrame / flushAll を委譲
};
```

- 単体テストは `DeletionQueueCore` を直接生成(hook はカウンタ付きフェイク)。
  WP9 の「GPU 不要」要件はこれで維持される
- `in_flight_frames_num` は ctor 引数化(core クラスから rendertarget.hpp 依存も外れる)
- 生成順ピン留めの意味論はラッパ ctor に残るので、破棄順の安全性は従来案と同じ

## 10. DCC プラグインのパッケージ方針

### blender_pelican2_bridge(§1、pelican2 リポ所有)

| 項目 | 方針 |
|------|------|
| 配置 | `pelican2/dcc/blender_pelican2_bridge/`(単一 .py から開始) |
| 対応バージョン | **Blender 4.2 LTS 以上**(同梱 Python 3.11)。bl_info の min を 4.2 に |
| 依存 | **外部 Python 依存ゼロ**(stdlib の subprocess / json / pathlib のみ)。numpy 等を import したら設計違反 |
| エンジン発見 | Addon Preferences の exe パス。既定値は環境変数 `PELICAN2_PLAYER` → 既知ビルドパス探索の順 |
| 実行 | modal operator + timer による非同期 subprocess。タイムアウトと kill ボタン必須 |
| 配布 | zip(Blender 標準インストール)。Blender Extensions 形式は需要が出てから |

### ツール側アドオン(mmd_mocap_bridge 等)への波及

- 現行の `sys.path` 直挿し + core 直 import は短期容認だが、方針として
  **「addon は設定済みパスの CLI(venv の console script)を subprocess で呼ぶ」方式に移行**する。
  Blender 同梱 Python に numpy/scipy を入れない・GPL 境界も自然に切れる・
  「CLI で再現可能」(R1)が UI 経路でも常に検証される
- R1 の「addon は UI のみ」の機械的なレビュー基準: **addon 内で bpy 以外の
  数値計算ライブラリを import したら違反**(subprocess 呼び出しと JSON 読み書きだけが許される)
- 各 addon の README に対応 Blender バージョン・インストール手順・CLI 設定方法を記載

## 11. 既存文書への反映リスト

**状況(2026-06-12 2 巡目): 下記 1〜5 はすべて適用済み**(design_cloth_simulation.md、
implementation_plan.md(WP9 改訂 + WP17 追加)、design_roadmap_renderworld.md)。
リストは経緯の記録として残す。

本書での決定を受けた、pelican2 側文書の修正箇所:

1. `design_cloth_simulation.md` §2 の表:
   - E0 行「**なし**」→「描画機能・ECS 変更なし。**WP17 SeqPlayer(小)が必要**(`implementation_plan.md`)」
   - E1 行「ほぼなし」→「アセット+**小 glue WP**(RGBA16F bufferView アップロード、`pelican.vat` メタ読み、uniform 供給)」
   - E0 前提列「transform_seq の搬入経路」→「WP17」
2. `design_cloth_simulation.md` §3: scale `[0,0,0]` 規約案 → R4 追補(`hidden`)に確定済みとして書き換え。
   §6 チェックリスト 1(scale 0)・2(cloth_setup 正本)・3(VAT)・6(搬入経路 = 当面ファイル+WP17)を「合意済み」へ
3. `design_cloth_simulation.md` §4.2: `core/phys/clothworld.hpp` → `feature/phys/clothworld.hpp`(本書 §8)
4. `implementation_plan.md`: WP9 仕様を本書 §9 の Core 分離版に差し替え。WP17(本書 §4)を追加。
   §3 コマンド層の「stdio 行 JSON」→「stdio NDJSON 上の JSON-RPC 2.0(R8 追補参照)」
5. `design_roadmap_renderworld.md` §2.1: 同上のプロトコル文言修正と、E0 の「追加設計不要」→「WP17 で受ける」

ツール側は requirements コピー同期・tool_suite_design の hidden 化を 2026-06-12 に適用済み。
cloth_design_2026-06-12.md ほか未コミット文書のコミットはツール側 TODO。
