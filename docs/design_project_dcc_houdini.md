# プロジェクト形式の DCC 強化: Houdini 入力と import manifest

対象読者: エンジン担当 + DCC/ツール側担当。
ステータス: v1 ドラフト(2026-07-02。レビュー前)。
前提: `external_tools_requirements.md`(R1〜R10、正本)、`design_project_format.md`
([PF] v6 凍結)、`docs/dcc_integration_qa_2026-06-12.md` §6(pelican.vat v1)、
`design_project_format_web_profile.md`([PFW] v1.3 凍結)。

## 0. 目的と設計姿勢

「最近のゲーム開発で使う DCC(特に Houdini)からの入力に対応した強力な
プロジェクト形式」を作る。ただし**強力さは project.json を太らせることでは
実現しない**。方針(ロードマップ §6「機能はなるべくアセットに」の延長):

1. **project.json は薄いまま**。DCC 対応の実体は
   (a) 交換フォーマット(R3/R4/R5 + 本書の追加)
   (b) `imports/` の規約と **import manifest**(§3、本書の中心)
   (c) 登録・検証ツール(`pelican_cli import` / doctor)
   の 3 点に置く。
2. **ツール側は R 番号契約に書き出すだけ**という現行構図を崩さない。
   Houdini アダプタも mocap lab 等と同格の「R3/R4/R5 を書く 1 ツール」である。
3. サブセット原則([PFW] §1-2)により、DCC 拡張は**すべて pelican 側の
   自由領域**。web は必要になった分だけ追随する(VAT の web 対応等は
   需要が出てから)。

## 1. Houdini 入力の分類と対応マップ

| Houdini の出力 | 用途例 | 交換形式 | 状態 |
|----------------|--------|----------|------|
| プロシージャルメッシュ(SOP → ROP glTF) | 背景・小物・破片 | glTF/.glb(R3) | **契約済み**。追加作業なし |
| RBD / packed prim の剛体シミュ | 破壊・群集の粗い動き | transform_seq JSONL(R4) | **契約済み**。エンジン再生 = SeqPlayer(WP17 実装済み)。Houdini 側エクスポータのみ必要 |
| 布・ソフトボディ・流体表面(頂点キャッシュ) | クロス・水面・変形 | pelican.vat v1(R5) | **仕様凍結済み・両側未実装**。Houdini は VAT 文化の本場(SideFX Labs VAT ROP)で相性最良 |
| パーティクル(POP / FLIP の点群) | 火花・飛沫・群れ | **ギャップ**: 適切な形式がない | §4-1 で方向だけ決定、仕様は別文書 |
| カメラ(ショットカメラ) | シーケンス再生・映像 | **ギャップ**: transform_seq は fov 等を持てない | §4-2。transform_seq v2 の課題として予約 |
| ボリューム(VDB: 煙・炎) | エフェクト | 対応しない(v1 スコープ外) | エンジンにボリュームレンダラがない。設計もまだしない |
| ハイトフィールド(地形) | 地形 | 当面 glTF メッシュ化で代替 | エンジン地形システムは別トラック |

その他の DCC も同じ写像に乗る: Marvelous Designer(布)→ VAT、
Substance(テクスチャセット)→ 通常アセット、Gaea(地形)→ glTF メッシュ化、
モーキャプ → 既存 mocap lab(R3 スケルタルアニメ)。**新しいツールを
増やすたびに形式を増やさない**ことが「強力」の条件。
Houdini 以外を含むフォーマット全般の取り回し(FBX/USD/EXR/音声等の
二層モデルと変換レーン)は `design_asset_format_policy.md` が正。

## 2. Houdini アダプタ(ツール側。R1 構造)

```
houdini-adapter/
 ├─ core   (Python 純粋ライブラリ: hou 非依存の変換・書き出しロジック)
 ├─ cli    (hython から叩ける CLI。PDG/バッチの単位)
 └─ hda    (薄い HDA: パラメータ UI と hou → core の変換のみ。ロジック禁止)
```

- R1 の bpy と同様、`hou` 依存は hda 層(と hython エントリ)に隔離。
  決定性(R2)・座標系変換(R6: Houdini は +Y up だが単位・軸まわりの
  変換責務はツール側)・命名(R7)・バージョン埋め込み(R10)は既存契約のまま
- 最初の納品物は 2 レーン:
  **(1) RBD → transform_seq**(SeqPlayer が実装済みのため、エンジン側
  追加コストゼロで動く。Houdini bridge M1 に相当)
  **(2) SOP 頂点キャッシュ → pelican.vat**(Labs VAT ROP の出力を
  pelican.vat v1 に正規化するコンバータから始めると HDA 開発が軽い)
- PDG からの一括実行は「cli を N 回叩く」だけで成立する(R1 を守っていれば
  自然に得られる。PDG 専用対応は作らない)

## 3. import manifest(pelican.import v1)— 本書の中心提案

### 問題

[PF] §6 の `imports/` は「着地点」でしかなく、**何が・どこから・どう作られて
届いたか**を機械可読に持たない。DCC パイプラインが太るほど、
(a) 再現性(どの .hip のどのシードから焼いたか)
(b) 検証(ファイルが揃っているか・壊れていないか)
(c) 登録(asset_data_json への反映)
が手作業になり崩壊する。

### 提案

納品 1 回 = 1 ディレクトリ + 1 マニフェスト:

```
imports/
  <tool>/<delivery_name>/
    manifest.json          # pelican.import v1(必須)
    <出力ファイル群>        # *.glb / *.jsonl / VAT 入り glb など
```

```json
{
  "schema": "pelican.import",
  "version": 1,
  "tool": {"name": "houdini-adapter", "version": "0.3.0", "dcc": "houdini 21.0.512"},
  "source": {"file": "shots/destruction_a.hip", "node": "/out/pelican_rbd", "seed": 42},
  "created": "2026-07-02T12:00:00Z",
  "outputs": [
    {"file": "debris.glb",        "schema": "gltf",                 "sha256": "..."},
    {"file": "debris_sim.jsonl",  "schema": "pelican.transform_seq", "version": 1, "sha256": "..."}
  ]
}
```

規則:

1. `source.file` は**ツール側マシンのパスでよい**(再現の手がかりであり、
   エンジンは解決しない。[PF] の絶対パス禁止は「エンジンが解決する参照」の
   規則なので競合しない)
2. `outputs[].file` は manifest からの相対パスのみ(ディレクトリ内に閉じる)
3. sha256 は必須。これが**保留中の assets.manifest.json(検証)と
   web キャッシュ検証の供給源**になる — 手書きの README 一覧表([PF] §5-6)を
   ツール納品分について自動化する位置づけ
4. manifest は**ツールが書く**(R10 の埋め込みバージョンと同源)。
   手置きのアセット(ストアで買った glb 等)には要らない — imports/ 外の
   assets/ に普通に置く
5. エンジン本体は manifest を**読まなくても動く**(参照は従来どおり
   scene/asset JSON が張る)。manifest を読むのは周辺ツール:
   - `pelican_cli import <delivery_dir>`: outputs を検証(sha256・スキーマ)し、
     asset_data_json への登録を対話または自動で行う([PF] §6 で「需要が
     出てから」としていたものを、Houdini 対応と同時に昇格)
   - `pelican_cli doctor`(将来): プロジェクト全体の manifest 検証
   - devstudio(将来): 納品一覧・再インポート UI

## 4. 新形式の方向決定(仕様は別文書・必要になってから)

1. **パーティクル点群 `pelican.pointcache`(方向のみ v1 決定)**:
   transform_seq の JSONL テキストは数万点で破綻する。方向は
   **「VAT と同じ思想のテクスチャ焼き込み(位置 + 任意属性)を glb バッファに
   格納」**とし、pelican.vat の設計(bounds 正規化・NEAREST + 手動 lerp・
   extras メタ)を最大限流用する。頂点キャッシュとの違いは「トポロジ固定が
   ない(点の生成消滅)」で、ここ(id/alive の表現)が仕様の本体になる。
   独自バイナリコンテナは作らない(R3 の「ハブは glTF」を点群にも適用)
2. **transform_seq v2(予約のみ)**: カメラトラック(fov/near/far)、
   スパースフレーム、補間指定。v1 の据え置き課題(R4)と合わせて 1 回で改訂する。
   それまでカメラは `--camera` CLI 指定(WP17)で代替
3. どちらも**ヘッダ/extras の schema+version 必須**(R10)は不変

## 5. エンジン側の受け入れ実装(WP 候補、依頼順)

| 候補 | 内容 | 依存 | 規模 |
|------|------|------|------|
| WP20 | VAT 再生(pelican.vat v1 の読み込み + 頂点シェーダ再生 + golden テスト) | WP15, 16, 18 | 大 |
| WP21 | `pelican_cli import`(manifest 検証 + asset_data_json 登録) | WP18 | 中 |
| WP22 | pointcache 再生 | WP20、形式仕様の合意 | 大 |

- WP 番号は仮。着手前に implementation_plan.md へ正式記載する
- **Houdini bridge M1(RBD → transform_seq → SeqPlayer)はエンジン側
  追加実装なしで成立する**ため、ツール側アダプタ(§2)が最初の一手。
  順序: houdini-adapter(RBD レーン)→ WP21 → WP20 → VAT レーン → WP22

## 6. project.json への影響

**なし**(意図的に)。`imports/` のツール別サブディレクトリと manifest は
規約であり、project.json のキーを増やさない。[PF] の凍結は保たれる。
将来 manifest 検証を起動時に組み込む場合も `--check-assets` フラグ
([PF] §5-6 の予告)であり形式変更ではない。

## 7. 未決事項

1. pelican.pointcache の id/生成消滅表現(仕様書を書く際の本体)
2. `pelican_cli import` の登録先 — asset_data_json の現行スキーマで
   十分か、アセット種別(vat/pointcache)の追記が要るか(WP21 設計時に確定)
3. Houdini アダプタのリポジトリ配置(外部ツール群と同じ独立リポジトリを推奨。
   external_tools_requirements.md のコピー同期対象に追加)
4. manifest の `source` に HDA パラメータ全量を含めるか(再現性 vs 肥大。
   v1 は file/node/seed のみで開始を提案)
