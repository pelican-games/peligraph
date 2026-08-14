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

## CI との分離

週次/手動の CI1 は、fresh checkout に ignored binary が 0 件であることを確認してから
configure、build、`ctest -LE gpu` を実行する。project-code smoke は
`test/fixtures/ground.glb` から一時 project を作るため、本台帳の 21 file を読まない。
feature-OFF smoke も各 build directory 内に最小 fixture を生成する。この分離により、
showcase store の有無や権利状態が CPU 回帰 gate の成否へ混入しない。
