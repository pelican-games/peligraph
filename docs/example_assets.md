# example 大容量アセットの取得・ライセンス・出所台帳

## 目的と境界

`projects/example` の JSON、shader、game code は git 管理するが、5 個の
GLB と 16 個の PNG（合計 21 file、73,275,201 byte）は管理しない。
これらは showcase 用であり、clean clone、CPU CI、build-unit smoke の入力ではない。
実バイナリをこの repository や Git LFS へ追加してはならない。

`AliciaSolid.vrm` は VRM 0.x の semantic を現行ランタイムが扱えず、scene と
game code のどちらからも参照されていなかったため、WP296 で example の資産表と
manifest から除外した。VRM 1.0 の表示例は自己完結した
[`projects/vrm_xr_demo`](../projects/vrm_xr_demo/README.md) が担う。

byte の正本は
[`projects/example/assets.manifest.json`](../projects/example/assets.manifest.json) の
size と SHA-256 である。本ページは取得経路、権利、出所の正本である。両者が
矛盾した場合は配布・更新を止め、manifest の byte を優先して調査する。

## 取得・配置手順

Pelican は任意 URL から asset を download する機能を持たない。作業者は、各行の
権利を確認できる自分またはチームの管理 store から、manifest の 25 entry
（21 binary と tracked text 4 file）を同じ相対構造で復元する。出所または
ライセンスが「未記録」の byte を第三者へ再配布して取得手段にしてはならない。

推奨は repository 外の store を使う方法である。

1. 権利確認済み bundle を、例えば `D:/pelican-assets/example` に展開する。
2. `projects/example/.pelican/local.json` をローカルだけに作る。

   ```json
   { "asset_stores": { "main": "D:/pelican-assets/example" } }
   ```

3. 解決先と byte を検査する。

   ```powershell
   build\src\devcli\Debug\pelican_cli.exe assets status --project projects/example
   build\src\devcli\Debug\pelican_cli.exe assets verify --full --project projects/example
   ```

4. `25 matched / 0 warning / 0 error` を確認してから example を起動する。

`.pelican/local.json`、hash cache、binary は commit しない。意図的に byte を更新する
場合は、取得 URL/管理番号、取得日、著作者、ライセンス名とライセンス本文または
固定 URL、加工ツールと版を下表へ先に記録する。その後に
`pelican_cli assets manifest --project projects/example` を実行し、21 file の
size/SHA-256 差分をレビューする。権利情報が欠ける更新は受け入れない。

## 21 file 台帳

「未記録」は public-domain や自由利用を意味しない。正確な upstream byte と権利の
対応を repository から立証できないため、第三者への再配布を禁止する状態である。

| file | size | SHA-256 | 出所・生成情報 | ライセンス / 配布判定 |
|---|---:|---|---|---|
| `models/DamagedHelmet.glb` | 3,773,916 | `a1e3b04de97b11de564ce6e53b95f02954a297f0008183ac63a4f5974f6b32d8` | embedded generator: Khronos Blender glTF 2.0 exporter。exact upstream URL/版は未記録 | 未記録、再配布不可 |
| `models/Sotai_D.glb` | 9,149,820 | `a0fec93b3d82f7fbf4e32d16afcfb68cf54e25d6f3fbe18a8be00d142cd008a7` | embedded generator: Khronos glTF Blender I/O v4.2.83。source scene/author は未記録 | 未記録、再配布不可 |
| `models/character.glb` | 3,416,852 | `b4c7a0f835da99c2a9ca86e9d6ff69a72450ea8b8e1ffdd8792e4031dfc7992e` | embedded generator: Khronos glTF Blender I/O v4.2.83。source scene/author は未記録 | 未記録、再配布不可 |
| `models/ground.glb` | 1,948 | `d84c3de963c7e9c274f4aadd5f4539a1fb3098e6566551ea9016cbe2fbd7915b` | embedded generator: Khronos glTF Blender I/O v4.2.83。source scene/author は未記録 | 未記録、再配布不可 |
| `models/sponza.glb` | 52,608,696 | `8eade0d66be99f2925558845680c01abfca8035fac14cc4ead03919309b3bd9f` | embedded generator: Granite glTF 2.0 exporter。exact source revision/変換手順は未記録 | 未記録、再配布不可 |
| `textures/01.png` | 544,154 | `9a6ba679408cadd48dc6fefee21463cb2d5ce2d39194bc78c065c08824f160e3` | source/生成手順未記録 | 未記録、再配布不可 |
| `textures/02.png` | 63,162 | `8dd70c6ee42afe83968e4f45a502df483b386cd6757fe17640d434fabd998e03` | source/生成手順未記録 | 未記録、再配布不可 |
| `textures/03.png` | 169,451 | `cc68cfe6da2d234c1206e139318a0c59da07edeb71e4fabef16352e6dce31b49` | source/生成手順未記録。`04.png` と同一 byte | 未記録、再配布不可 |
| `textures/04.png` | 169,451 | `cc68cfe6da2d234c1206e139318a0c59da07edeb71e4fabef16352e6dce31b49` | source/生成手順未記録。`03.png` と同一 byte | 未記録、再配布不可 |
| `textures/05.png` | 312,723 | `5f22f4df549eec30395f2e8700f1b8410d164a07b1bce3450a5e339e72709584` | source/生成手順未記録。`06.png`/`explosion06.png` と同一 byte | 未記録、再配布不可 |
| `textures/06.png` | 312,723 | `5f22f4df549eec30395f2e8700f1b8410d164a07b1bce3450a5e339e72709584` | source/生成手順未記録。`05.png`/`explosion06.png` と同一 byte | 未記録、再配布不可 |
| `textures/07.png` | 413,411 | `d9ea03b09f644bc4d9dc6adb98b1077b705677738ace10c4032300dede3c625f` | source/生成手順未記録。`08.png` と同一 byte | 未記録、再配布不可 |
| `textures/08.png` | 413,411 | `d9ea03b09f644bc4d9dc6adb98b1077b705677738ace10c4032300dede3c625f` | source/生成手順未記録。`07.png` と同一 byte | 未記録、再配布不可 |
| `textures/09.png` | 429,152 | `caf15ebc6eed09813466a5a22ecdf6738c71f58f152bb202e37516f20687fcab` | source/生成手順未記録。`10.png` と同一 byte | 未記録、再配布不可 |
| `textures/10.png` | 429,152 | `caf15ebc6eed09813466a5a22ecdf6738c71f58f152bb202e37516f20687fcab` | source/生成手順未記録。`09.png` と同一 byte | 未記録、再配布不可 |
| `textures/11.png` | 238,781 | `a0ef1a48f009eb9e3902e98124c41bc5d59bad6bc6cd26ab9037da18a0195af0` | source/生成手順未記録。`12.png` と同一 byte | 未記録、再配布不可 |
| `textures/12.png` | 238,781 | `a0ef1a48f009eb9e3902e98124c41bc5d59bad6bc6cd26ab9037da18a0195af0` | source/生成手順未記録。`11.png` と同一 byte | 未記録、再配布不可 |
| `textures/Frame84.png` | 252,840 | `15f9b40f684b3332364b4d34da1f96e46a50c02d23922d61e3ed8a247cf32476` | UI atlas source。upstream/source project は未記録 | 未記録、再配布不可 |
| `textures/alpha.png` | 4,318 | `4c50715a578e7b4b488a37472df6e7bc3244ed6a9b3d92281361481a0e8576be` | source/生成手順未記録 | 未記録、再配布不可 |
| `textures/explosion06.png` | 312,723 | `5f22f4df549eec30395f2e8700f1b8410d164a07b1bce3450a5e339e72709584` | UI atlas source。`05.png`/`06.png` と同一 byte | 未記録、再配布不可 |
| `textures/test.png` | 19,736 | `4e69d8f97843a94cd15e81f79581a70067c1902481d6b0dc56c8b426150d84d8` | source/生成手順未記録 | 未記録、再配布不可 |

`models/ground.glb` と同じ形・同じ material 値の箱は、下の「repository が追跡する
小さな資産」の `ground.glb` として [`tools/generate_ground_glb.py`](../tools/generate_ground_glb.py)
が生成する(byte は違う)。example の manifest はまだ上表の byte を指している。

## repository が追跡する小さな資産

上の 21 file とは別に、出荷 sample と test fixture として git が追跡している小さな
binary / 資産の出どころ。「生成」の行は、列に書いた script や test の出力をそのまま
置いたもの。

| file | size | SHA-256 | 出所・生成情報 | 配布判定 |
|---|---:|---|---|---|
| `projects/vrm_xr_demo/assets/ground.glb`、`test/fixtures/ground.glb`、`test/fixtures/studio_lighting_pass_template/assets/ground.glb`(3 つとも同じ byte) | 1,964 | `4f752036b3418b2ead6d03530802c3eade32a93ef7f85fae1cd307fe31e1da6a` | 生成: `uv run tools/generate_ground_glb.py`(`--check` で 3 つが出力と一致するかを見る)。2×0.618×2 の赤い箱、24 頂点。以前の手書き出しの file と幾何・material 値・頂点と index の並びが同じ | この repository の生成物 |
| `projects/pathtrace_demo/assets/checker_box.glb` と `test/fixtures/studio_*/checker_box.glb` 6 つ | 2,356 | `141f78d560de3727c6fe8e0a534a77f88916c5668e971f0e1cca6c70a127987f` | 生成: `tools/pt1b_make_checker_box.py` | この repository の生成物 |
| `projects/vrm_xr_demo/assets/vrm_xr_character.vrm` | 8,156 | `a6dff2f234853ec550bbdd9b0db88309104d948c042c77a205ea7f3de20fde47` | 生成: `test/vrm_xr_demo_fixture.hpp`(test「WP135 committed VRM is the deterministic full demo fixture」が byte を照合する)。VRM の meta は空 | この repository の生成物 |
| `test/fixtures/audio/sine440_1s.ogg` | 6,258 | `e040520b7c0097a41620232ea0877bdc121743420dfce06a8463105bc8f201b9` | 生成手順は隣の `sine440_1s.ogg.provenance.txt` | この repository の生成物 |
| `test/fixtures/wp338_ssao/*.r8`(6 file) | 4,096〜24,576 | — | 生成: engine の SSAO の出力。`PELICAN_UPDATE_WP338_SSAO_GOLDEN=1` で golden harness が書き直す | この repository の生成物 |
| `test/golden/*/expected.png` | — | — | 生成: engine の出力。`PELICAN_UPDATE_GOLDEN` で golden harness が書き直す(`test/golden/README.md`) | この repository の生成物 |
| `projects/animgraph_demo/assets/animgraph_demo.gltf`、`projects/posegraph_demo/assets/posegraph_demo.gltf`(同じ byte) | 3,201 | git の blob の値は `test/fixtures/wp364/parent_manifest.json` | WP101 で手書きした 2 骨の板と 3 つの clip(generator 文字列 `Pelican WP101 animgraph demo`)。生成 script は無い | 自作(推定)。生成 script 無し |
| `projects/animgraph_demo/assets/status.png`、`projects/posegraph_demo/assets/status.png`、`projects/sprite_demo/assets/demo.png`(同じ byte) | 189 | `f865102680a5f85cbbdb9e0d3c1ecd6d2ab44df09456c100f25caadb16ca9a53` | 16×16 の sprite atlas。作った手順の記録は無い | 自作(推定)。生成 script 無し |
| `test/fixtures/import_manifest/{valid,invalid/sha_mismatch}/debris.glb` | 43 | `50247eeaca40ca0bb1a5fb171799f32aba3cae2c8e33030bb4ba21f8d2c146c6` | model ではなく、1 行の英文の text(import manifest の test が byte と hash だけを見る) | 自作 |
| `docs/design_reviews/assets/2026-07-17_wp140_renderdoc_labels.png` | 54,526 | `1aaf91966f47734f6c16615d12d6073ca7430f3892bb8c026126f88b7abdeb42` | RenderDoc の UI のスクリーンショット(第三者の UI が写る) | 要確認 |

`test/fixtures/usd0b/`、`test/fixtures/usd0c/` の model と texture は別 repository の
`pelican-import-tools` が生成したもので、元の USD の出どころはこの repository からは
確かめられない。

## CI との分離

週次/手動の CI1 は、fresh checkout に ignored binary が 0 件であることを確認してから
configure、build、`ctest -LE gpu` を実行する。project-code smoke は
`test/fixtures/ground.glb`(上の生成物)から一時 project を作るため、本台帳の 21 file を読まない。
feature-OFF smoke も各 build directory 内に最小 fixture を生成する。この分離により、
showcase store の有無や権利状態が CPU 回帰 gate の成否へ混入しない。
