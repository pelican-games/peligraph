# pass の融合: 意味は分けて書き、実行時は compiler が 1 本にまとめる

対象読者: エンジン担当(描画コンパイラ / shader library / renderer)。
ステータス: 実装方針決定(2026-10-22。「これは実装しないとね」)。細部は WP 化で詰める。
前提: [design_render_target_memory_reduction.md](design_render_target_memory_reduction.md)(feature は RT の枚数を気にせず宣言してよい)、
[design_stage2_typed_pipeline.md](design_stage2_typed_pipeline.md)(`logical_graph` の footprint を入力にする)、
[design_plan_layer_inventory.md](design_plan_layer_inventory.md) 2-3(policy 一本化。`fuse` はその語彙に入る)、
[design_macos_moltenvk_proof.md](design_macos_moltenvk_proof.md)(tile_local は Apple GPU で初めて効く)。
マニュアル [第6章 §6.6](manual/06_rendering.md)(`tile_local_attachment`、`dynamic_rendering_local_read`)。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- 実行時に効くのは**シェーダファイルの数ではなく pass の数**。fullscreen pass 1 本は画面全体の 1 読み 1 書き(1080p RGBA16F で 32 MB)。
  dGPU で約 0.1 ms、帯域 50 GB/s の iGPU や統合メモリで約 0.6 ms。小さい後処理 8 本なら iGPU で 5 ms 近く、融合すれば 0.6 ms。
- 融合できるのは**前段の出力を同じ画素だけ読む**段(tonemap、grading、vignette、grain、合成)。近傍を読む段(blur、bloom、DoF、SSAO、TAA)は境界になる。
  Pelican はこの区別を `LogicalReadFootprintKind`(`same_pixel` / `neighborhood` / `arbitrary` / `temporal`、[`logicalrendergraph.hpp:115`](../src/project/logicalrendergraph.hpp#L115))として既に持つ。
- 方針: **作者は pass を分けて書く。planner が融合を決める。** 機構は 2 つ。
  (1) 既にある `tile_local_attachment` + `local_read`(tile GPU で API 側の融合。シェーダはそのまま)、
  (2) 足す **シェーダの縫合(stitching)**: `same_pixel` の鎖を 1 本の fragment shader に生成して desktop でも帯域を減らす。

## 1. 融合できる条件(鎖の検出)

鎖の候補は、論理 graph 上で次を満たす連続する fullscreen pass の列:

1. 各段の主入力(鎖の資源)の read footprint が `same_pixel`。
2. 各段の出力が次段の主入力で、extent と format class が同じ(1 画素 1 値の鎖)。
3. 中間の出力を鎖の外の誰も読まない。読む者がいる段は、**その段の出力を MRT の 2 本目として書き出して鎖は続ける**(読み手が `same_pixel` 以外なら materialized に落とす)。
4. 各段の他の読み(LUT、noise texture、parameter buffer、history)は `arbitrary` / `temporal` でもよい(鎖の資源でなければ縫合を妨げない)。
5. どの段も `fuse: false` の policy を持たない(2-3 の policy 語彙に `fuse` を足す。計測・デバッグ用)。

検出は project の planner(純ロジック)。出力は plan の `fused_nodes[]`(members、鎖の資源、MRT で残した中間)。Frame Plan で「この 5 pass は 1 本」と見える。

## 2. 縫合(stitching)

- **作者の契約**: 鎖に入れる段は、fragment の本体を関数 `vec4 <stage>(vec4 color, vec2 uv)` として書く(今の include 機構で `.glsl` に置く)。
  自分の parameter と texture は従来どおり binding で受ける。関数の中で鎖の資源を `texture()` しない(引数の `color` が前段の値)。
  契約を満たさない pass は融合されず、従来どおり単独で走る(名前付きの診断 `fusion.stage_not_fusable@1` を出す)。
- **生成**: compiler が鎖ごとに 1 本の fragment shader を作る。include で各段の関数を取り込み、`main` が順に呼ぶ。
  feature の on / off は生成時に決まるので permutation は実在する組だけ。生成は compile 時(runtime shader compiler がある)、hash(段の集合 + defines + 段の shader の digest)で cache。
- **binding**: 各段の descriptor は段ごとの set を保つか、生成時に 1 つの set に詰め直すか(未決 §6)。
- **精度**: 融合前は段の間に RGBA16F の量子化が入り、融合後は fp32 のまま流れるので、結果は byte 一致しない。
  **融合後を正とし**、`fuse: false` を A/B と golden の対照に使う。golden は融合既定で撮り直す(段階 3 で 1 回)。

## 3. tile_local との関係

- `tile_local_attachment` は同じ scope 内の `same_pixel` 読みを local_read で渡す機構で、シェーダを変えずに pass 境界の store / load を消す。
  効くのは tile GPU(Apple silicon は MoltenVK 1.4.0 の local_read で対象)。desktop では帯域は減らない。
- 縫合は適用できる所では常に tile_local より強い(pass そのものが消える)。tile_local は縫合できない形(material pass が gbuffer を書き、lighting が同じ画素を読む deferred の典型)に残る。
- 優先順位: 縫合できる鎖は縫合 → 残りを tile_local → 残りを materialized。

## 4. やらないこと

- 近傍読みの段の融合(compute の shared memory と halo の再計算で可能だが、別件)。
- 作者に 1 本の über-shader を書かせること。permutation の爆発、occupancy の低下、段ごとの計測の喪失があるので、融合は compiler の判断に限る。
- material pass(geometry を描く pass)の融合。対象は fullscreen の鎖だけ。

## 5. 段階

| 段 | 内容 | 検証 |
|---|---|---|
| 0 | 計測: example と bloom / taa の鎖で `same_pixel` の連続を数え、帯域の節約を見積もる。iGPU と dGPU で前後の GPU 時間 | 数値(RT メモの `memory_summary` と同じ場所に `bandwidth_estimate`) |
| 1 | planner: 鎖の検出、`fused_nodes[]`、policy `fuse` | planner 単体(GPU 不要)。example の鎖が期待どおり |
| 2 | 縫合の生成器と cache。同梱 feature の後処理段を関数の契約に書き換える(tonemap、hdr、bloom の合成、grading があれば) | 生成 GLSL の fixture、shader compile の単体 |
| 3 | runtime: 融合 pipeline、GPU 時間の node、Frame Plan の表示、golden の撮り直し | headless golden(`fuse: false` との差が精度の範囲)、GPU gate |
| 4 | tile_local の検証(mac 実証の段 4) | Apple silicon で tile_local が選ばれ、validation が通る |

依存: 段 1 は `logical_graph` の footprint を読むので段階 2 強化メモの段 1 以降が楽。policy `fuse` は 2-3 の語彙と同じ WP。

## 6. 未決

- binding の詰め方(段ごとの set を保つか、生成時に 1 つにまとめるか)。descriptor indexing を使えば set を保ったまま 1 pipeline にできる。
- 鎖の途中で解像度が変わる段(upsample 合成)の扱い。鎖の境界にするのが単純。
- 縫合した node の GPU 時間を段ごとに出す手段(timestamp は挟めない)。`fuse: false` での計測を Studio から 1 クリックで。
- 精度の方針(fp32 で流すのを正とする)を決定性ゲートの文書に明記する。
