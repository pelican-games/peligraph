# intro link: 外の小さな Vulkan program へ曲線とシェーダを送る口と、焼いて切り離す道

対象読者: 開発ツール担当、intro を作る利用者。
ステータス: v1 向け。IL-1〜IL-3 が `develop_v1` に着地済み(2026-10-02)。`develop` には入っていない。
前提: `design_build_tiers.md`(`PELICAN_WITH_*` の機能ユニットと配布 preset)、[第10章 §10.4](manual/10_tools.md)(`pelican_cli`)。

## 0. 要求と位置づけ(2026-10-02 利用者方針)

デモシーンのサイズ制限つき作品(intro)を、自作の小さな Vulkan program で作る。
そのとき開発中だけ peligraph を「付けて」起動し、提出前に切り離したい。GNU Rocket と同じ形。

利用者の条件: **この機能は本筋から外れた付け足しなので、確実にパージ可能にする。** だから

- engine の実行時(`src/core`、`src/player`)と Studio(`src/devstudio`)には 1 行も足さない
- 足すのは `pelican_cli` の subcommand、単一 header の client、実例 program、test、build option の登録だけ
- 専用の `PELICAN_WITH_INTRO_LINK`(既定 ON)。OFF で source ごと外れる(§6)

v1 向けの機能で、置き場は統合 branch `develop_v1`。

## 1. 形

```
開発中:  project ──(serve: glslc を子 process で呼ぶ)── TCP 127.0.0.1 ──> 作品(開発の形)
提出時:  project ──(bake:  同じ glslc・同じ規則)──> C header ──(#include)──> 作品(提出の形)
```

- 作品側の runtime は単一 header 1 本(`tools/intro_link/pelican_intro_link.h`)。
- データの出所だけが 2 つある。開発中は socket、提出時は bake が焼いた表。
- 曲線の評価は両方の形で同じ関数 1 本([`pil_eval`](../tools/intro_link/pelican_intro_link.h#L78))。engine の `ScalarCurve::sample` を float で写したもの。
- 範囲は時刻・曲線・シェーダだけ。pass の繋ぎ(pipeline、描く順)は作品側の手書き。

## 2. `pelican_cli intro-link serve` / `bake`

```
pelican_cli intro-link bake  <project> --out <header> [--shaders <dir>] [--glslc <path>]
pelican_cli intro-link serve <project> [--port N] [--shaders <dir>] [--glslc <path>] [--exit-after-client]
```

振り分けは [runIntroLinkCommand](../src/devcli/introlinkcommand.cpp#L112)。
例外はここで受けて stderr に 1 行出し、終了コード 1。未知の verb と引数なしは usage を出して -1。

### 2.1 引数

| 引数 | 意味 |
|---|---|
| `<project>` | `project.json` がある dir、または `project.json` そのもの |
| `--out <header>`(bake、必須) | 書く C header。親 dir は作る。失敗したら書かない |
| `--shaders <dir>` | 既定 `shaders`。project からの相対(絶対 path も可) |
| `--glslc <path>` | glslc の場所。探す順は `--glslc` → `$VULKAN_SDK/Bin`(POSIX は `Bin` と `bin`)→ PATH([findGlslc](../src/devcli/introlinkproject.cpp#L348)) |
| `--port N`(serve) | 既定 47611。`0` なら空いている port を取る |
| `--exit-after-client`(serve) | 最初の client が切れたら 0 で終了する(test 用) |

bake の成功時は stdout に `wrote <path> (<N> tracks, <M> shaders)` を出し、続けて大きさの行を出す(§2.4)。
bake は shader が 1 本も無ければ glslc を探さない。serve は起動時に必ず探す([serve の引数](../src/devcli/introlinkcommand.cpp#L106))。

### 2.2 集める対象と名前の規則

| 種類 | 集める対象 | 名前 |
|---|---|---|
| track | `passes/*.json` のうち `schema: "pelican.graph"` の文書。top-level の `nodes` と `definitions.*.nodes` の `op: "value.curve"` の node([collectTracks](../src/devcli/introlinkproject.cpp#L300)) | node の `id` |
| shader | `<shaders>` 直下の `.vert` / `.frag` / `.comp`。下位 dir は対象にせず、include の探索先にだけなる([collectShaders](../src/devcli/introlinkproject.cpp#L327)) | `<file の stem>_<stage>`(例 `intro_frag`) |

- 名前は C の識別子(`[A-Za-z_][A-Za-z0-9_]*`)でなければ拒否する。project の中で重複したら拒否する(§5)。track と shader の名前は別の名前空間。
- curve は `parameters.keys` を読む([readKeys](../src/devcli/introlinkproject.cpp#L71))。`time` / `value` 必須、`interpolation` は `linear`(既定)/ `step` / `cubic`、`in_tangent` / `out_tangent` は任意。
- key の時刻は float に落とした後で狭義単調増加を確かめる。値が float に収まらなければ拒否する。
- curve の `time` 入力の繋ぎ(`value.playback` など)は見ない。作品が渡す秒をそのまま key の時刻として評価する(§9)。

### 2.3 glslc を子 process で呼ぶ

`pelican_cli` に shaderc を link しない。glslc と spirv-opt を `runProcess` の子 process で呼び、SPIR-V を stdout で受ける(1 回の呼び出しごとに timeout 60 s)。
serve と bake は同じ [compileShader](../src/devcli/introlinkproject.cpp#L396) を通る。開発中と提出物で SPIR-V を変えないため。

shader ごとに 2 通り作り、小さい方を使う(IL-3)。

| 組 | 作り方 |
|---|---|
| `Os` | `glslc --target-env=vulkan1.2 -Os -I <shaders> -I <shaders>/include <file> -o -` |
| `no-inline` | 同じ引数の `-O0` で glslc → 一時 file → `spirv-opt --target-env=vulkan1.2 <kSizePasses> <一時 file> -o -` |

- [`kSizePasses`](../src/devcli/introlinkproject.cpp#L196) は 36 個の pass の並び。spirv-opt 自身の `-Os` の並びから `--inline-entry-points-exhaustive` を抜き、最後に `--unify-const --eliminate-dead-const --remove-duplicates --compact-ids` を足したもの(報告では `--loop-unroll` も抜いたとある)。関数を呼び出し箇所ごとに inline しないので、同じ関数を何度も呼ぶ shader で小さくなる。
- 選ぶ規則:
  - `no-inline` が厳密に小さいときだけ `no-inline`。同じ大きさなら `Os`。
  - glslc の隣に `spirv-opt(.exe)` が無ければ `Os` だけを作る([spirvOptBeside](../src/devcli/introlinkproject.cpp#L209))。新しい名前付きエラーは無い。
  - `-O0` の glslc か spirv-opt が失敗したら(error、timeout、出力が 20 byte 未満か 4 の倍数でない)`Os` を使う。その stderr は捨てる。
  - `-Os` の glslc が失敗したら `no-inline` は作らず、compile の失敗になる(bake は `shader_compile_failed@1`、serve は SHADER_ERROR)。
  - 比べるのは大きさだけなので、同じ入力と同じ SDK なら同じ組が選ばれる。
- 一時 file は `<temp>/pelican_intro_link_<pid>_<tick>_<n>.spv`。scope を出ると消す([TemporaryFile](../src/devcli/introlinkproject.cpp#L221))。child に stdin を渡せないため file を経由する。
- 選んだ組の名前(`Os` / `no-inline`)は bake と serve の表示に出る(§2.4、§2.5)。header には書かない。
- どちらが小さいかは shader 次第。実例の frag は `no-inline` が 3,920 byte(`Os` は 4,432)、IL-1 の fixture の frag は `Os` が 564 byte(`no-inline` は 812)。1 回だけ呼ぶ小さな関数では、関数を関数のまま残す費用が inline の得を上回る。
- spirv-opt がある場合、glslc を 2 回と spirv-opt を 1 回呼ぶ。compile の時間の増え方は測っていない(報告は「約 2 倍」と見積もっている)。

stage は拡張子から glslc が決める。

### 2.4 bake の出力

[renderBakedHeader](../src/devcli/introlinkproject.cpp#L442) が C header を 1 本書く。

- `pil_key` の型、`enum pil_baked_track_id { PIL_TRACK_<name> = i, … }`、`pil_baked_keys[]`、`pil_baked_track_first[]` / `pil_baked_track_count[]`。
- `enum pil_baked_shader_id { PIL_SHADER_<name> = i, … }`、shader ごとの `pil_baked_shader_<name>[]`(1 行 8 word の hex)、`pil_baked_shader_words[]` / `pil_baked_shader_word_count[]`。
- 決定的。並びは名前の昇順、改行は LF。時刻も絶対 path も書かない(文書名の comment は project からの相対)。
- 小数は `std::to_chars` の最短表記に `f` を付ける。wire にも同じ float を流す。
- track / shader が 0 本なら `PIL_BAKED_TRACK_COUNT 0` などだけを出し、enum も配列も出さない。
- SPIR-V は glslc と spirv-opt の版に依存する。決定性は「同じ SDK で」の範囲。

header を書いた後、stdout に作品へ入る大きさを 1 行ずつ出す(IL-3、[bake の表示](../src/devcli/introlinkcommand.cpp#L69))。
track は key 数 × 20 byte(`pil_key` の大きさ)、shader は SPIR-V の byte と選んだ組。実例 project では次のとおり。

```
wrote .../intro_baked.h (3 tracks, 2 shaders)
track fade: 4 keys, 80 bytes
track pulse: 9 keys, 180 bytes
track scene: 4 keys, 80 bytes
shader intro_frag: 3920 bytes (no-inline)
shader intro_vert: 656 bytes (Os)
total: 340 bytes of keys + 4576 bytes of SPIR-V = 4916 bytes
```

- 表示の行を足しただけで、header の中身は変えていない。
- `devcli_intro_link_bake` は、fixture の bake の stdout に `track fade_linear: 3 keys, 60 bytes`、`shader main_frag: `、` bytes (Os)`、`total: 180 bytes of keys + ` があることを見る。

### 2.5 serve

- 127.0.0.1 の TCP で待つ。起動したら stdout の 1 行目に `port <N>` を出して flush する。stdout にはそれ以外を書かない(log は stderr)。
- client の HELLO が合えば snapshot を送る([snapshot](../src/devcli/introlinkserve.cpp#L476)): 全 TRACK(名前の昇順)→ 全 shader(SPIR-V があれば SHADER、compile error があれば SHADER_ERROR)→ TIME → SNAPSHOT_END。
- 以後 100 ms ごとに file を見る。
  - `passes/*.json` の (mtime, size) が変わったら全 track を読み直し、keys が変わった track だけ TRACK を送る([watchTracks](../src/devcli/introlinkserve.cpp#L533))。
  - shader は file ごとに見て、変わったものだけ compile し直す。下位 dir のどれかが変われば全 shader を compile し直す。新しい shader file も拾う([watchShaders](../src/devcli/introlinkserve.cpp#L559))。
  - compile に失敗したら SHADER_ERROR(glslc の stderr)を送る。client は前の SPIR-V を持ち続ける。直ったら SPIR-V が前と同じでも SHADER を送る(client の error を消すため)。
  - 読み直しの失敗(書きかけの JSON など)は stderr に 1 行出して前の状態を保ち、次の tick でまた試す。同じ文言は繰り返さない。
- 起動時の project のエラー(名前の不正・重複、curve の不正、glslc が無い)は bake と同じ名前付きエラーで終了する。起動時の shader の compile 失敗では終了せず、snapshot に SHADER_ERROR を入れて続ける。
- compile が通るたびに、stderr に SPIR-V の byte と選んだ組を出す。2 回目からは前回との差も付く(IL-3、[serve の表示](../src/devcli/introlinkserve.cpp#L511))。差が 0 でも `+0` と出る。起動時と、実例の `BASE` を書き換えた後:

```
intro-link serve: shader intro_frag: 3920 bytes (no-inline)
intro-link serve: shader intro_vert: 656 bytes (Os)
intro-link serve: shader intro_frag: 3920 bytes (no-inline, +0)
```

### 2.6 stdin の命令

stdin は別 thread が OS の生の read で読み、行を queue に積む。EOF ではその thread が終わるだけで、server は続く。
命令は main loop が処理する([drainCommands](../src/devcli/introlinkserve.cpp#L350))。

| 命令 | 動き | 送る TIME |
|---|---|---|
| `play` | paused なら再生に戻す | 状態が変わったときだけ。flags = 0 |
| `pause` | 再生中なら今の時刻で止める | 状態が変わったときだけ。flags = 1 |
| `seek <秒>` | 時計をその秒に合わせる(paused なら止まったまま) | 毎回。flags = paused \| 2 |
| `quit` | 0 で終了する | — |

数でない `seek` と未知の命令は stderr に 1 行出して無視する。時計は server が持ち、再生中は「最後の anchor + 経過時間」で進む。

## 3. 通信の形 v1

little-endian。枠は `u32 type, u32 payload_bytes, payload`。

| type | 向き | payload |
|---|---|---|
| 0 HELLO | client → server | `u32 magic 'PLNK'(0x4B4E4C50), u32 version = 1` |
| 1 TRACK | server → client | `u16 name_len, name, u32 key_count, key × N`。key = `f32 time, f32 value, f32 in_tangent, f32 out_tangent, u32 interpolation(0 linear / 1 step / 2 cubic)` |
| 2 SHADER | server → client | `u16 name_len, name, u32 word_count, u32 × N` |
| 3 SHADER_ERROR | server → client | `u16 name_len, name, u32 text_len, text`(glslc の stderr) |
| 4 TIME | server → client | `u32 flags(bit0 = paused, bit1 = seek あり), f32 time` |
| 5 SNAPSHOT_END | server → client | 空 |

- magic か version が違う HELLO には何も送らずに切り、stderr に 1 行出す([receive](../src/devcli/introlinkserve.cpp#L448))。
- server は client → server の HELLO 以外の frame を読み飛ばす。payload の上限は 1 MiB(超えたら切る)。
- client は知らない type を読み飛ばす。
- snapshot の TIME の bit1 は「server が一度でも seek を受けたか」。一度立つと残る。time は server の時計の今の値。

## 4. client の header

`tools/intro_link/pelican_intro_link.h`。単一 header で、C99 と C++17 の両方で通る(IL-1 で `/W4 /TP` 警告 0 を確認)。

- 開発の形(既定): 1 つの翻訳単位で `#define PELICAN_INTRO_LINK_IMPLEMENTATION` してから include する。winsock / POSIX socket。MSVC では `#pragma comment(lib, "ws2_32")`。
- 提出の形: `#define PELICAN_INTRO_LINK_BAKED`。bake の header を先に include する(無ければ `#error`)。include するのは `<stdint.h>` だけで、socket・malloc・stdio・string.h のコードは入らない([提出の形の分岐](../tools/intro_link/pelican_intro_link.h#L104))。
- Windows の開発の形では、この header を `<windows.h>` より先に include するか、`WIN32_LEAN_AND_MEAN` を先に define する(winsock.h との衝突を避けるため)。

| API | 開発の形 | 提出の形 |
|---|---|---|
| `int pil_connect(host, port, timeout_ms)` | 50 ms 間隔で繋ぎ直し、SNAPSHOT_END まで待つ。成功 1、timeout で 0 | 何もせず 1 |
| `void pil_poll(void)` | block せずに届いた分を反映する。server が切れても最後の値を保つ | 空 |
| `void pil_disconnect(void)` | socket を閉じ、受けたものを解放する(IL-1 で足した) | 空 |
| `PIL_TRACK(name, t)` → `float` | 名前の文字列で引く。無い名前は 0 | `PIL_TRACK_<name>` で焼いた表を引く([定義](../tools/intro_link/pelican_intro_link.h#L125))。無い名前は compile error |
| `PIL_SHADER(name, &word_count)` → `const uint32_t *` | 最後に届いた SPIR-V。`word_count` は `uint32_t` | 焼いた配列 |
| `PIL_SHADER_CHANGED(name)` | 前に聞いてから SHADER が届いていれば 1(読むと 0 に戻る)。snapshot の分は数えない | 定数 0 |
| `PIL_SHADER_ERROR(name)` → `const char *` | 最後の glslc の stderr。SHADER が届くと NULL に戻る | NULL |
| `int pil_time(float *t, int *paused)` | TIME が一度も届いていなければ 0。届いていれば 1(`*paused` を入れる)。seek の直後の 1 回だけ `*t` を入れて 2([実装](../tools/intro_link/pelican_intro_link.h#L578)) | 定数 0 |
| `pil_eval(keys, count, t)` | 同じ 1 本(static inline) | 同じ 1 本 |

`pil_eval` の意味: key の範囲外は端の値。範囲内は左の key の interpolation で決まる(step は保持、linear は lerp、cubic は tangent を秒あたりの傾きとした Hermite)。

最小の使い方(1 つの source で両方の形になる):

```c
#ifdef PELICAN_INTRO_LINK_BAKED
#include "intro_baked.h"              /* bake が書いた header */
#else
#define PELICAN_INTRO_LINK_IMPLEMENTATION
#endif
#include "pelican_intro_link.h"       /* <windows.h> より先 */

pil_connect("127.0.0.1", 47611, 5000);
for (;;) {
    pil_poll();
    if (pil_time(&seek, &paused) == 2) t = seek;   /* server の seek に合わせる */
    float fade = PIL_TRACK(fade, t);
    if (PIL_SHADER_CHANGED(intro_frag)) { /* pipeline を作り直す */ }
}
```

## 5. 名前付きエラー

stderr に `code: 説明` の 1 行を出し、終了コード 1。devcli で `@1` の形を使うのはこの subcommand が最初。

| code | 出る時 | 説明に入るもの |
|---|---|---|
| `intro_link.name_not_identifier@1` | track / shader の名前が C の識別子でない | 名前、文書(shader は file)の path |
| `intro_link.name_duplicate@1` | 同じ名前が 2 箇所にある | 名前と両方の path |
| `intro_link.curve_invalid@1` | keys が空、時刻が狭義単調増加でない、知らない interpolation、数でない値、float に収まらない値 | 文書の path と node id |
| `intro_link.glslc_not_found@1` | glslc が見つからない、`--glslc` が file でない | 探した場所 |
| `intro_link.shader_compile_failed@1` | bake で compile に失敗した | shader の path。2 行目以降に glslc の stderr がそのまま続く(厳密な 1 行ではない) |
| `intro_link.port_unavailable@1` | serve の bind / listen に失敗した、`--port` が 0..65535 の外 | `127.0.0.1:<port>` と OS の error 番号 |
| `build_feature_disabled@1` | `PELICAN_WITH_INTRO_LINK=OFF` の `pelican_cli` で `intro-link` を呼んだ | `This binary was built with PELICAN_WITH_INTRO_LINK=OFF: pelican_cli intro-link is unavailable`([stub](../src/devcli/introlinkdisabled.cpp#L9)) |

実例(IL-1 の報告から):

```
intro_link.name_duplicate@1: track "fade_linear" is defined in passes/extra.json and passes/main.json
intro_link.curve_invalid@1: curve "late" in passes/extra.json: key times must be strictly increasing
intro_link.glslc_not_found@1: searched --glslc, $VULKAN_SDK (unset), PATH
intro_link.port_unavailable@1: 127.0.0.1:47699 (error 10048)
```

名前の無い失敗もある。`<project>` に `project.json` が無い、graph 文書が JSON として読めない、argparse の引数エラーなど。これらも stderr に 1 行で終了コード 1。

## 6. パージ

- option: [`PELICAN_WITH_INTRO_LINK`](../cmake/pelican_feature_registry.cmake#L225)(BASELINE ON、CONTRAST OFF、SMOKE_ID `intro_link`)。`docs/build_features.md` の表にも 1 行ある(生成物)。
- OFF のとき:
  - `pelican_cli` には stub([`introlinkdisabled.cpp`](../src/devcli/introlinkdisabled.cpp#L9))だけが入る。`intro-link` は `build_feature_disabled@1` を出して終了コード 1([source の切り替え](../src/devcli/CMakeLists.txt#L27))。
  - 実例 program の dir は参照されない([root の CMakeLists](../CMakeLists.txt#L455))。
  - test は 1 本も登録されない([test/CMakeLists.txt](../test/CMakeLists.txt#L3312))。
  - OFF の build で確かめたこと(`pelican_cli` だけを build): devcli の obj は `introlinkdisabled.cpp.obj` だけ、`pelican_cli.exe` の import に WS2_32 が無い、`build.ninja` に `pelican_test_intro_link*`(IL-1)も `intro_link_example`(IL-2)も 0 件。
- 配布 preset(`pelican_cli dist-config`)は常に `set(PELICAN_WITH_INTRO_LINK OFF CACHE BOOL "" FORCE)` を書く([distconfig](../src/devcli/distconfig.cpp#L897))。IMGUI / RENDERDOC / SPIRV_LINK と同じ扱い。
- `src/core`・`src/player`・`src/devstudio` には 1 行も無い。IL-1〜IL-3 の commit はこの 3 つの dir の file を含まない。
- build-unit smoke に `run_intro_link_smoke`(OFF で configure と build、intro link の obj が無いこと、disabled の文言)がある([smoke](../test/run_build_units_smoke.cmake#L1178))。ただし本番の smoke は未実行(§9)。

消すときに外すもの:

| 種類 | 対象 |
|---|---|
| dir ごと | `tools/intro_link/`(header と実例)、`test/fixtures/intro_link/` |
| file | `src/devcli/introlink.hpp`、`introlinkcommand.hpp`、`introlinkcommand.cpp`、`introlinkproject.cpp`、`introlinkserve.cpp`、`introlinkdisabled.cpp` |
| file(test) | `test/intro_link_tests.cmake`、`intro_link_eval_test.cpp`、`intro_link_test_client.c`、`intro_link_baked_probe.c`、`intro_link_print_values.h`、`run_devcli_intro_link_bake.cmake`、`run_devcli_intro_link_serve.cmake`、`run_intro_link_example_size.cmake` |
| registry の 1 件 | `cmake/pelican_feature_registry.cmake` の `PELICAN_WITH_INTRO_LINK`。消したら `cmake -P cmake/update_feature_ledger.cmake` で `docs/build_features.md` を作り直す |
| 参照の数行 | `src/devcli/CMakeLists.txt` の if 節、`src/devcli/main.cpp` の振り分けと usage、`src/devcli/distconfig.cpp` の 2 行、root `CMakeLists.txt` の if 節、`test/CMakeLists.txt` の include、`test/run_build_units_smoke.cmake` の関数と `elseif`、`test/run_devcli_dist_config.cmake` の期待値 1 行 |
| 文書 | この文書と、[第10章](manual/10_tools.md) の `intro-link` の節 |

## 7. 実例 program と実測(IL-2 / IL-3)

置き場は `tools/intro_link/example/`。IL-2 で作り、IL-3 で提出の形を 17,920 → 10,704 byte に削った。

### 7.1 構成

- project `tools/intro_link/example/project/`: `value.curve` が 3 本。`fade`(linear、0→1 を 1 秒、7→8 秒で 0 へ)、`pulse`(cubic、1 秒ごとに 0/1)、`scene`(step、0 / 3 秒で 1 / 5.5 秒で 2)。最後の key はどれも 8 秒。
- `shaders/intro.vert`: 頂点 buffer なしの全画面三角形。`gl_VertexIndex` の bit から `(-1,-1) (3,-1) (-1,3)` を作る。`gl_PerVertex` を `gl_Position` だけで宣言し直し、varying は無い。
- `shaders/intro.frag`: push constant `{vec4 v;}`(16 byte、`v = (time, fade, pulse, scene)`)だけを読む。球を raymarch し、`pulse` が半径と rim、`fade` が明るさ、`scene` が形を決める。色の定数は `const vec3 BASE`。
  - 大きさのための書き方(IL-3): 距離関数は 1 本、`scene` の分岐は `step` / `mix` の算術、push constant は関数ごとに 1 回だけ読む、normal は前進差分 3 点。
  - 座標は `gl_FragCoord` から作る。窓が 1280×720 であることを前提にしている(aspect の push constant と varying を消したため)。
  - IL-3 の書き直しで絵が変わったのは 2 つ。背景の縞を `BASE.bgr` から作る(暗い側が紺から黒寄りの青になる)、光の向きを `(0.6, 0.8, 0)` にした(z 成分が無くなり、球の陰の付き方が変わる)。構図・形・scene の切り替えは IL-2 と同じ(§7.7)。
  - 出力は `color.xyzz * fade` で、alpha には青 × fade が入る。swapchain は `COMPOSITE_ALPHA_OPAQUE` なので画面には出ない。
- 作品 `intro.c`(C の 1 file、2 つの形を `#ifdef PELICAN_INTRO_LINK_BAKED` で分ける):
  - Win32 の窓(1280×720 固定)と Vulkan 1.3。dynamic rendering(`vkCmdBeginRendering`)、push constant だけ、頂点 buffer・descriptor・depth・memory の確保なし。swapchain の image に直接描く。
  - Vulkan の関数はすべて `vulkan-1.lib` から import する(`vkGet*ProcAddr` なし)。dumpbin で数えると、提出の形は 28 個、開発の形は 38 個(`--shot` の 7 個と、作り直しの `vkDestroyShaderModule`・`vkDestroyPipeline`・`vkDeviceWaitIdle`)。
  - validation、device の選別、swapchain の作り直し、error の検査は書かない(intro の割り切り。最初の physical device、queue family 0 を仮定)。
  - 決め打ち(IL-3): surface の format と capability を問い合わせない。format は `B8G8R8A8_UNORM`、image は 3 枚、extent は窓の 1280×720、transform は identity、color space は 0(sRGB nonlinear)、present mode は FIFO。
  - 窓は system の `"static"` class で作る(IL-3)。`RegisterClassA`・window proc・`DefWindowProcA` が要らない。`WS_VISIBLE` を付け、`CreateWindowExA` の y に show command を渡すので `ShowWindow` も呼ばない。module handle は `GetModuleHandleA` でなく `__ImageBase`。
  - create-info は関数ごとに 1 つの構造体にまとめて 1 回で 0 初期化し、毎 frame 同じ値の field は loop の前に設定する(IL-3)。
  - 提出の形は shader module も pipeline も破棄せず、最後の `vkDeviceWaitIdle` もせずに `ExitProcess` で終わる(IL-3)。開発の形は pipeline を作った直後に shader module を破棄する。
  - 時刻は `QueryPerformanceCounter`。`pil_time` が 0 で 8 秒(`INTRO_SECONDS`)を越えたら終える([終了の判定](../tools/intro_link/example/intro.c#L462))。ESC は message queue から読む。窓の × で窓が壊れると、次の `vkAcquireNextImageKHR` が負の値を返して loop を抜ける。
- 開発の形 `intro_link_example_dev`: CRT あり。引数 `[port] [--frames N] [--shot path.bmp]`。`PIL_SHADER_CHANGED(intro_vert) | PIL_SHADER_CHANGED(intro_frag)` で `vkDeviceWaitIdle` → 古い pipeline を破棄 → 作り直す([作り直し](../tools/intro_link/example/intro.c#L467))。server に繋がらないか SPIR-V が無ければ 1 で終わる。`--frames` を付けると窓を `SW_SHOWNOACTIVATE` で出し、focus を取らない。
- 提出の形 `intro_link_example`: bake の header を build 時に `add_custom_command` で作る(DEPENDS は `pelican_cli` と project の全 file)。CRT なし: 自前の entry(`/ENTRY:entry`、`ExitProcess`)、`_fltused`、`memset` / `memcpy` を `#pragma function` で自前(`__stosb` / `__movsb`)。
- build する条件: `PELICAN_WITH_INTRO_LINK=ON`、64-bit Windows + MSVC、Vulkan の header と `vulkan-1.lib`、glslc。欠ければ configure の log に 1 行出して飛ばす([条件](../tools/intro_link/example/CMakeLists.txt#L6))。

### 7.2 提出の形の flag

build type に関係なく固定。example の dir の scope で、上から来る directory property と `CMAKE_C_FLAGS*` / `CMAKE_EXE_LINKER_FLAGS*` / `CMAKE_C_STANDARD_LIBRARIES` を空にしている([scope の初期化](../tools/intro_link/example/CMakeLists.txt#L23))。Debug の tree でも `/RTC1` `/MDd` `/Zi` は乗らない。

```
compile: /O1 /Os /GS- /Gs9999999 /GR- /Gy /Gw /Zl /W3
link:    /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:WINDOWS /MACHINE:X64
         /OPT:REF /OPT:ICF /MERGE:.rdata=.text /MERGE:.pdata=.text
         /DYNAMICBASE:NO /FIXED
         /ALIGN:16 /FILEALIGN:16 /STUB:<example>/dos_stub.bin /EMITTOOLVERSIONINFO:NO /IGNORE:4108
         /INCREMENTAL:NO /DEBUG:NONE /MANIFEST:NO /EMITPOGOPHASEINFO
libs:    kernel32 user32 vulkan-1
```

([compile](../tools/intro_link/example/CMakeLists.txt#L64)、[link](../tools/intro_link/example/CMakeLists.txt#L73))。compile は IL-2 のまま。link の 4 行目が IL-3 で足したもの。

- `/ALIGN:16 /FILEALIGN:16`: section と file の alignment を 16 byte にする。IL-2 では map できることだけを確かめて採らなかった。IL-3 で lease の下で実際に走らせ、終了コード 0 で終わったので採った。
- `dos_stub.bin`(64 byte): `MZ`、`e_cparhdr = 4`、`e_lfarlc = 0x40`、残りは 0。`e_lfanew` は link が書く。`e_lfarlc` を 0 にすると LNK4060(完全な MS-DOS header でない)が出るので 0x40 にした。`LINK_DEPENDS` に入れてある。
- `/EMITTOOLVERSIONINFO:NO`: Rich header を出さない。
- `/IGNORE:4108`: `/DRIVER` なしの `/ALIGN` の warning を抑える。意図した使い方。
- 書き込み可かつ実行可の section は無い。`.data` は `.text` に merge しない(§7.6)。
- dumpbin で Resource Directory = 0、Debug Directory = 0。

### 7.3 大きさ

提出の形 `intro_link_example.exe` の内訳(IL-3 の最終、dumpbin):

| 項目 | 属性 | byte |
|---|---|---|
| file の大きさ | | **10,704** |
| headers(64 byte の stub + PE + section 表 3 つ、alignment 0x10) | | 0x1C0 = 448 |
| `.text`(code、定数、SPIR-V。`.rdata` と `.pdata` を merge 済み)virtual / raw | Execute Read | 0x2218 / 0x2220 = 8,736 |
| ├ うち SPIR-V | | 4,576(`intro_frag` 980 word = 3,920、`intro_vert` 164 word = 656) |
| ├ うち key の表 | | 340(17 key × 20 byte) |
| └ 残り(機械語、文字列、`.pdata`) | | 約 3,820 |
| `.data` virtual / raw | Read Write | 0x18 / 0x20 = 32 |
| `.idata` virtual / raw | Read Only | 0x5C6 / 0x5D0 = 1,488 |

- import は 35 個。KERNEL32 の 3 個(`ExitProcess`、`QueryPerformanceCounter`、`QueryPerformanceFrequency`)、USER32 の 4 個(`CreateWindowExA`、`AdjustWindowRect`、`PeekMessageA`、`DispatchMessageA`)、vulkan-1 の 28 個。
- IL-2 から消えた import: `GetModuleHandleA`、`RegisterClassA`、`DefWindowProcA`、`ShowWindow`、`vkGetPhysicalDeviceSurfaceFormatsKHR`、`vkGetPhysicalDeviceSurfaceCapabilitiesKHR`、`vkDestroyShaderModule`、`vkDeviceWaitIdle`。
- `intro_link_example_baked_is_small` の上限は 16,384 byte(10,704 × 1.5 を 1 KB 単位に切り上げ。IL-2 では 27,648)。import の期待値(KERNEL32 / USER32 / vulkan-1)は IL-2 のまま。

IL-2 と IL-3 の比較:

| | IL-2 | IL-3 |
|---|---|---|
| 提出の形 exe | 17,920 | **10,704**(-40.3 %) |
| zlib level 9 / lzma raw | 7,188 / 5,867 | 4,849 / 4,195 |
| SPIR-V frag + vert | 8,532 + 932 = 9,464 | 3,920 + 656 = 4,576 |
| 開発の形 exe(Debug) | 58,368 | 53,760 |

**zlib / lzma は packer の代わりの目安。** packer は使っていない。python の `zlib`(level 9)と `lzma`(LZMA1 raw、preset 9e)で exe 全体を圧縮した値。IL-2 の zlib は、IL-2 の測定では 7,189、IL-3 の測り直しでは 7,188。

IL-2 → IL-3 の段ごとの表(IL-3 の報告から):

- 「既定 layout」は IL-2 の link flag(FileAlignment 512)、「最終 layout」は §7.2 の flag。
- zlib / lzma は最終 layout の exe の値(\* は既定 layout の exe の値)。
- 段 3 の行は、その上の行に足していった累積。

| 段 | 変更 | exe(既定 layout) | exe(最終 layout) | SPIR-V | zlib9 | lzma |
|---|---|---|---|---|---|---|
| 0 | IL-2 | 17,920 | — | 9,464 | 7,188 | 5,867 |
| 1a | shader の書き直しだけ(glslc `-Os` のまま) | 13,312 | — | 5,088 | 5,776\* | 5,038\* |
| 1b | 最適化の組だけ(IL-2 の shader + spirv-opt) | 14,336 | — | 5,864 | 6,031\* | 5,202\* |
| 1 | 1a + 1b | 12,800 | 11,808 | 4,576 | 5,444 | 4,739 |
| 3-1 | surface の format / capability を決め打ち | 12,800 | 11,520 | 〃 | 5,315 | 4,629 |
| 3-2 | 提出の形は shader module の破棄と最後の `vkDeviceWaitIdle` をしない | 12,800 | 11,376 | 〃 | 5,256 | 4,558 |
| 3-3 | `ShowWindow` をやめて `WS_VISIBLE`、`GetModuleHandleA` をやめて `__ImageBase` | 12,800 | 11,296 | 〃 | 5,205 | 4,529 |
| 3-4 | 窓を system の `"static"` class にする | 11,776 | 11,136 | 〃 | 5,085 | 4,419 |
| 3-5 | create-info をまとめて 1 回で 0 初期化し、毎 frame 同じ値の field を loop の外へ出す | 11,776 | 10,672 | 〃 | 4,839 | 4,189 |
| 3-6 | image view の info もまとめに入れる | 11,776 | 10,656 | 4,576 | 4,828 | 4,176 |
| 最終 | `.data` の merge と `.text` の書き込み可を外す(§7.6) | 11,776 | **10,704** | 4,576 | **4,849** | **4,195** |

段 1〜3-6 の「最終 layout」は、`.data` を `.text` に merge して `.text` を書き込み可かつ実行可にした layout で測った。最後の行でそれを外した(+48 byte)。

layout の flag ごとの効き(段 1 の obj で 1 つずつ足した):

| link に足した flag | exe | zlib9 | lzma |
|---|---|---|---|
| なし(IL-2 と同じ) | 12,800 | 5,583 | 4,875 |
| `/EMITTOOLVERSIONINFO:NO` + 64 byte の stub(FileAlignment は 512 のまま) | 12,288 | 5,463 | 4,762 |
| `/ALIGN:16 /FILEALIGN:16` | 11,984 | 5,572 | 4,876 |
| 上 + `/EMITTOOLVERSIONINFO:NO`(Rich header が消える) | 11,920 | — | — |
| 上 + `/STUB:dos_stub.bin`(64 byte) | 11,856 | 5,458 | 4,745 |
| 上 + `/MERGE:.data=.text /SECTION:.text,ERW`(採らない。§7.6) | 11,808 | 5,444 | 4,739 |
| `/MERGE:.idata=.text`、`/MERGE:.idata=.rdata` | LNK1272 | | |

IL-2 で測ったこと(FileAlignment 512 の頃。同じ obj を手で link し直した。共通は `/NODEFAULTLIB /ENTRY /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:NO /EMITPOGOPHASEINFO /DEBUG:NONE /INCREMENTAL:NO`):

| 足した flag | file |
|---|---|
| なし | 18,944 |
| `/MERGE:.rdata=.text` | 18,944(512 byte の境界を越えず効かない) |
| 上 + `/DYNAMICBASE:NO` | 18,432(`.reloc` が消える) |
| 上 + `/FIXED` | 18,432(上乗せ 0) |
| 上 + `/MERGE:.pdata=.text` | 17,920 |
| `/FILEALIGN:16` だけ | 16,928。image として map できない(error 193) |

compile の flag では `/Gw` が効く(IL-2 で、付けないと 20,480、付けると 17,920)。`/O2` は 18,944、`/fp:fast` は変化なし。

開発の形(Debug の tree、`/MDd /Od /Zi /RTC1`)の import は KERNEL32 / USER32 / WS2_32 / vulkan-1 / VCRUNTIME140D / ucrtbased。

### 7.4 shader の書き直しと最適化の組

frag の SPIR-V の byte 数(IL-3)。「`no-inline`」は §2.3 の組。

| 版 | 変更 | `-Os` | `no-inline` |
|---|---|---|---|
| IL-2 | | 8,532 | 4,932 |
| f01 | normal を 4 点の loop にする(tetrahedron) | 5,116 | — |
| f02 | `scene` の分岐を `step` / `mix` の算術にまとめる | 4,744 | — |
| f03 | 波の `sin` 3 回をベクトルの 1 回にする | 4,672 | — |
| f04 | push constant を `float v[5]` にする | 4,632 | — |
| f05a | `right` を `(cos a, 0, -sin a)` で直接作る(同値) | 4,596 | — |
| f05b | normal を前進差分 3 点にする | 4,448 | 4,572 |
| f05 | march の条件を loop の条件にまとめ、陰影を loop の後へ出す | 4,880 | 4,428 |
| f06 | camera を `mat3 *`、rim を `1+min(dot(n,rd),0)`、出力を `vec4(c,1)*fade` | 4,848 | 4,332 |
| f07b | `gl_FragCoord` を使う(aspect の push constant と varying を消す) | 4,868 | 4,352 |
| f09 | y の反転を camera の up 側で行う、`0.35*(2+pulse)`、波の係数 `s*(2-s)` | 4,820 | 4,312 |
| f10 | 背景を `BASE.bgr*(0.3+0.2 sin)`、光を `(0.6,0.8,0)`(**絵が少し変わる**) | 4,680 | 4,172 |
| f11 | push constant を `vec4` にして、関数ごとに 1 回だけ読む | 4,496 | 4,044 |
| f12 | 距離関数の中で引数を局所変数へ写す。出力は `color.xyzz*fade` | 4,444 | 3,932 |
| 最終 | 当たりの点に march の点をそのまま使う、`q = ro` で初期化 | 4,432 | **3,920** |

- 「—」の行は、途中まで使った別の inline しない組で測った(報告の B 組。この表には載せない)。
- vert は IL-2 の 932 → `gl_Position` だけ・varying なしで 732 → 頂点を整数の bit から作って **656**。vert では `-Os` と `no-inline` が同じ大きさなので `Os` が選ばれる。

最適化の組を比べた表:

| 組 | IL-2 の frag | 最終の frag | 最終の vert | IL-1 の fixture の frag |
|---|---|---|---|---|
| glslc `-Os` | 8,532 | 4,432 | 656 | **564** |
| glslc `-O0` + strip + compact | 6,140 | — | — | — |
| `no-inline`(§2.3) | **4,932** | **3,920** | 656 | 812 |

- 採る条件は「`-Os` より 5 % 以上小さく、絵が同じ」だった。IL-2 の frag(-42.2 %)と最終の frag(-11.6 %)の両方で満たした。
- 絵は、同じ時刻(0.5 / 2.0 / 4.0 / 6.5 秒)で止めて撮り、画素ごとに比べた。IL-2 の frag の `-Os` と `no-inline`、最終の frag の `no-inline` と `-Os` は、4 時刻とも差 0。
- pass を 8 個(`--strip-debug --simplify-instructions --convert-local-access-chains --eliminate-local-single-block --merge-blocks --ssa-rewrite --redundancy-elimination --eliminate-dead-code-aggressive`)まで減らしても、測った 5 本の shader の大きさは変わらなかった。それでも全部を残したのは、利用者の shader(多重 return、dead branch、private 変数など)に偏らないためと、圧縮のための `--compact-ids` / `--unify-const`。
- IL-1 の fixture の frag は `no-inline` で大きくなる(564 → 812)。だから「両方を作って小さい方」にした(§2.3)。

IL-2 で測ったこと(SPIRV-Tools v2025.1 / SDK 1.4.309.0。bake の道には入れていない):

- `spirv-opt --strip-debug --strip-nonsemantic` は変化なし。glslc の `-O*` は既に OpName などを出していない。
- `spirv-remap --map all --strip all` は大きさが同じで中身が変わる。この shader では圧縮後が少し大きくなった(frag の lzma raw 1,959 → 1,987)。
- `spirv-opt --canonicalize-ids` はこの SDK の spirv-opt では使えない(`Unknown flag`)。

### 7.5 符号化と圧縮の見積もり(測っただけ。作品には入れていない)

SPIR-V を python の script で変換して測った。

- varint: word を LEB128 にし、opcode と word 数は別の varint、result id は前の result id との差(zigzag)。
- +rel: さらに他の id operand を「result id − operand」にしたもの(SMOL-V 型)。

| SPIR-V | 元(lzma) | varint(lzma) | varint+rel(lzma) | 文法なしの単純 varint(lzma) | byte 面に並べ替え(lzma) |
|---|---|---|---|---|---|
| IL-2 frag(`-Os`) | 8,532(1,959) | 3,649(1,762) | 3,510(1,728) | 4,123(2,043) | (2,148) |
| IL-2 frag(`no-inline`) | 4,932(1,285) | 1,892(1,100) | 1,875(1,066) | — | (1,456) |
| 最終 frag(`no-inline`) | 3,920(1,099) | 1,465(944) | 1,441(**891**) | 1,652(1,136) | (1,177) |
| 最終 vert | 656(256) | 239(201) | 239(198) | 271(235) | (233) |

- SMOL-V 型(varint+rel)にすると、frag + vert の lzma は 1,355 → 1,089 byte(**約 -266**)、生の大きさは 4,576 → 1,680。
- ただし作品側に、文法を知る復号器(opcode ごとに result id の位置を知る表)が要る。その大きさは数百 byte と見ている(**推測**、作っていない)。lzma の減りと相殺するので入れていない。
- 文法なしの単純 varint は、生の大きさは減るが lzma 後は少し増える。
- byte 面への並べ替え(全 word の byte0 → byte1 → byte2 → byte3)は、lzma 後に増える。exe 全体でも、SPIR-V を byte 面にすると lzma が 4,176 → 4,306(+130)になった(段 3-6 の 10,656 byte の exe で測った)。LZMA の pb=2(4 byte の位置文脈)が word の並びを既に使っているため、と報告は見ている(推測)。

最初の `OpFunction` より前(型・定数・decoration・entry point などの前置き)の byte:

| SPIR-V | 全体 | 前置き | 内訳 |
|---|---|---|---|
| IL-2 frag | 8,532 | 1,280(15.0 %) | 定数 784、型 220、decoration 120、header 類 88、変数 48 |
| IL-2 vert | 932 | 600(64.4 %) | |
| 最終 frag | 3,920 | 1,096(28.0 %) | 定数 632、型 244、header 類 88、decoration 64、変数 48 |
| 最終 vert | 656 | 392(59.8 %) | 型 124、定数 96、header 類 72、decoration 48、変数 32 |

### 7.6 採用しなかったもの

| 案 | 結果 | 採らない理由 |
|---|---|---|
| `.idata` を他の section に merge | LNK1272 | MSVC の link が許さない |
| `.data` を `.text` に merge し `.text` を書き込み可かつ実行可にする(`/MERGE:.data=.text /SECTION:.text,ERW`) | 48 byte 減る(10,704 → 10,656)、lzma 4,195 → 4,176 | 48 byte のために、書き込み可かつ実行可の section を持たない |
| create-info を関数の `static` にする(loader が 0 で埋める) | 既定 layout では -512 byte。最終 layout では +1.5 KB(12,144〜12,368)。lzma は 4,058〜4,139 まで下がる | 低 alignment では bss も file に 0 で置かれる。bss を `.idata` の後ろへ動かすと image として map できない(error 193)。packer を使うなら有力(§9.3) |
| SPIR-V を byte 面に並べ替える | §7.5 | lzma 後に増える |
| `QueryPerformanceCounter` → `GetTickCount` | 測っていない | 分解能が約 15.6 ms で、60 Hz の動きに段差が出る。import 1 つ(約 30 byte)のために絵の滑らかさを落とさない |
| 枠なしの窓(`WS_POPUP`。`AdjustWindowRect` が消える) | 測っていない | 開発の形の窓が動かせなくなる |
| per-frame の field を loop の外へ出すことだけ(まとめての 0 初期化なし) | -32 byte、lzma +20 | 単独では lzma が増えた。0 初期化をまとめた後(段 3-5)では両方減ったので、そちらで採った |

### 7.7 実機の確認(この機械の最初の physical device で)

IL-2:

- 開発の形: serve に繋いで 240 frame。途中で `intro.frag` の `BASE` を書き換え、pipeline の作り直しがちょうど 1 回(書き換えから約 0.28 秒後)。dev = 0、serve は `quit` で 0。書き換えたのは scratch に写した project。
- screenshot(`--shot`、開発の形だけの道): 黒一色でなく、書き換え後の色の球が出ていた。
- 提出の形: 終了コード 0、8.78 秒で自分から終わった。

IL-3(GPU lease の下で直列に。窓は focus を取らない形で出した):

- 絵の比較: serve に `pause` と `seek t` を送り、開発の形を `--frames 3 --shot` で撮った。t = 0.5 / 2.0 / 4.0 / 6.5 秒(scene 0、0、1、2)。
  - 最適化の組を変えても絵は同じ(§7.4、差 0)。
  - IL-2 と IL-3 の絵の差(平均 |d| / |d| > 32 の画素)は 4.5 / 4.0 %、6.7 / 3.3 %、6.3 / 2.7 %、15.2 / 16.5 %。差は f10 の 2 つ(§7.1)で、scene 2 の差が大きいのは光の向きのため。
  - 16 回とも dev は終了コード 0、format 44(`B8G8R8A8_UNORM`)、1280×720。
- 開発の形: serve に繋いで 240 frame、最初の frame の 0.4 秒後に `BASE` を緑へ書き換えた。stderr は `pipeline rebuilt (1) at frame 37`、`240 frames, 1 pipeline rebuilds, 1280x720 format 44`。作り直しはちょうど 1 回。
- 提出の形: 終了コード 0、8.77 秒。最終の flag(§7.2)の exe で走らせ直して終了コード 0、8.65 秒。
  - 起動の前後と 4.5 秒の時点で foreground の process は変わらなかった(focus を奪わない)。
  - 提出の形の絵を `PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT)` で撮れた。非黒の画素 100 %。
- ESC と ×(window message を post して確かめた):

| 対象 | post した message | 結果 |
|---|---|---|
| 提出の形 | `WM_KEYDOWN VK_ESCAPE` | 0.18 秒後に終了コード 0 |
| 提出の形 | `WM_SYSCOMMAND SC_CLOSE`(× と同じ道) | 1.17 秒後に終了コード 0 |
| 開発の形 | ESC | 0.19 秒後に 0(stats の行も出た) |
| 開発の形 | `SC_CLOSE` | 1.17 秒後に 0 |

× の場合、`vkAcquireNextImageKHR` が負の値を返すまで約 1 秒かかった(driver の動き)。

## 8. 試す手順

repo の root で。Windows、Vulkan SDK あり、`PELICAN_WITH_INTRO_LINK=ON` の build tree `build`。
`pelican_cli.exe` の場所は build の構成による(IL-2 の tree では `build\dist\Debug\`)。

1. 2 つの形を build する(提出の形の build で bake も走る)。

```bat
cmake --build build --target pelican_cli intro_link_example_dev intro_link_example
```

2. serve を起動する。1 行目に `port 47611` が出る。この端末は開けたままにする。

```bat
build\dist\Debug\pelican_cli.exe intro-link serve tools\intro_link\example\project
```

3. 別の端末で開発の形を起動する(既定の port は 47611。ESC で閉じる)。

```bat
build\tools\intro_link\example\intro_link_example_dev.exe
```

4. 走らせたまま file を書き換えて保存する。
   - `tools/intro_link/example/project/shaders/intro.frag` の `const vec3 BASE`。色が変わり、dev の端末に `intro: pipeline rebuilt (1) at frame <N>` が出る。serve の端末には `shader intro_frag: <byte> bytes (<組>, <差>)` が出る(§2.5)。壊すと compile error が dev の端末に出て、前の絵が残る。
   - `passes/main.json` の key。次の frame から曲線が変わる。
5. serve の端末で `pause` / `play` / `seek 3` / `seek 0` を打つ。intro の時計が従う。serve が時刻を握っている間は 8 秒を過ぎても閉じない。`quit` で serve が終わる。
6. 提出の形を作って走らせる。serve は要らない。8 秒で自分から閉じる。

```bat
cmake --build build --target intro_link_example
```

```bat
build\tools\intro_link\example\intro_link_example.exe
```

## 9. 制限と未決

### 9.1 残した穴(IL-1〜IL-3 の報告と実物から)

- curve の `time` 入力の写像(`value.playback` の範囲・速度・wrap・scale/offset)は見ない。作品が渡す秒をそのまま key の時刻にする。
- track を集めるのは top-level の `nodes` と `definitions.*.nodes` まで。loop の body など、より深い所の curve は拾わない。
- project から消えた track / shader を client に知らせる frame が v1 に無い。
  - track: serve は stderr に 1 行出す。client は最後の keys を持ち続ける。後から繋いだ client の snapshot には入らない。
  - shader: serve は何も出さず、内部の状態からも消さない。後から繋いだ client の snapshot にも最後の SPIR-V が入る。
- server の send は blocking。client が長く poll しないまま大量の更新が来ると、server の loop が止まりうる(localhost で作品が毎 frame poll する前提)。
- 監視は (mtime, size)。同じ時刻・同じ size の書き換えは見逃す。include の依存は解析せず、下位 dir のどれかが変われば全 shader を compile し直す。
- 最適化の組(§2.3):
  - `no-inline` の出力は大きさ(20 byte 以上、4 の倍数)しか見ない。`spirv-val` は通さない。IL-3 では bake の出力を手で `spirv-val --target-env vulkan1.2` に掛け、実例の frag / vert と fixture の frag が valid だった。
  - `-O0` の glslc か spirv-opt が失敗しても、何も出さずに `Os` を使う。spirv-opt が壊れていても気付けない。
  - spirv-opt の版が変わると、選ばれる組と SPIR-V が変わりうる。決定性は「同じ SDK で」の範囲(§2.4)。
- POSIX の経路は compile も実行もしていない(Windows だけで確認)。C99 strict の POSIX では `_POSIX_C_SOURCE=200112L` が要る。
- 本番の build-unit smoke(`-DPELICAN_BUILD_UNIT_SMOKE_ONLY=intro_link`、OFF 構成の全体 build)は未実行。確かめたのは `pelican_cli` だけの OFF build と、parse-only の `feature_registry_smoke_matrix_contract`。
- Studio から serve を起動して時刻を握る接続は未着手(`src/devstudio` には何も無い)。
- 実例 program について:
  - 最初の physical device と queue family 0 を仮定する。present できない構成では動かない。
  - 窓の大きさは固定で、swapchain は作り直さない。frag は 1280×720 を前提に `gl_FragCoord` から座標を作る。
  - 決め打ちにした format(`B8G8R8A8_UNORM`)・image 3 枚・extent・color space が、他の GPU / driver で通るかは確かめていない。確かめたのはこの機械の最初の physical device だけ。
  - 最小化したときの動きは確かめていない。acquire が失敗すると intro が終わる可能性がある(推測)。
  - DPI の拡大がかかった画面での extent は確かめていない。
  - loader が 1.3 未満だと `vkCmdBeginRendering` の import が解決できず起動しない。
  - 開発の形は server が無いと起動しない(SPIR-V を server からしか受けないため)。
  - ESC と × での終了と提出の形の絵は IL-3 で確かめた(§7.7)。ただし ESC と × は window message を post したもので、実際の keyboard focus 経由の ESC は確かめていない。
  - size の test は exe を走らせない(窓と GPU を使うため)。intro の動きの退行は ctest では捕まらない。
  - `PELICAN_WITH_INTRO_LINK=ON` の Windows では実例が `all` に入る。bake の custom command は `pelican_cli` に依存するので、全体の build で `pelican_cli` が作り直されるたびに bake も走る。

### 9.2 header / cli に直したい所(IL-2 の報告。直していない)

- **長さを得る口が無い。** 実例は 8 秒を `INTRO_SECONDS` で手で持つ。案: bake が `PIL_BAKED_END_TIME`、開発の形が `pil_end_time()`。
- **繋いだ直後の時刻。** snapshot の TIME は bit1(一度でも seek があった)が立っていなければ `pil_time` が `*t` を返さない。server が一度も seek していないと、作品の時計は次の seek まで server とずれる。案: snapshot の TIME を常に「seek あり」と扱う、または今の時刻を読む API。
- **`PIL_SHADER_ERROR` の変化を知る口が無い。** 実例は pointer の比較で新しい error を判断している(同じ address に再確保されると見逃しうる)。
- **切れたことを知る口が無い。** server が切れても `pil_time` は 1 を返し続け、実例は 8 秒で閉じない。案: `pil_connected()`。
- **`--glslc` の既定。** 実例と test の custom command は毎回 `--glslc` を渡している。cli の既定の探索で足りるので必須ではない。
- SMOKE_ID は `intro_link`。既存は `spirv-link` のような hyphen 区切りで、揃えるなら `intro-link`。

### 9.3 後段の案(利用者が言い出してから)

- グラフの pass の繋ぎまで握る形(作品側の手書きの pipeline を project から作る)。

実例の exe をこれ以上削るなら(IL-3 の報告から。数字の見込みは推測):

- **packer は利用者が入れる。** peligraph は packer を同梱しない(IL-3 では何も download していない)。§7.3 の zlib / lzma はその代わりの目安。
  - packer を使うなら、create-info の `static` 化(§7.6)を一緒に見直す。lzma で約 -120 byte だが、低 alignment の生の exe は +1.5 KB になる。
- **32-bit + Crinkler。** import が hash になり、`.idata`(1,488 byte)と名前の文字列が消える。context modeling で圧縮されるので 4 KB 前後に入る見込み(推測)。
  - 要るもの: 32-bit の build(SDK の `Lib32/vulkan-1.lib`)、`__stdcall` の扱い、Crinkler の入手。
  - x64 のまま使える成熟した packer は少ない、と報告は見ている(推測)。
- **vert と frag を 1 つの module にする**(複数 entry point)。module の header、型、`GLSL.std.450` の import、memory model の重複が消える。vert の前置き 392 byte の多く(300 byte 前後、推測)が減る見込み。bake の形(shader 1 file = 1 名前)を変える必要がある。
- **import の名前。** 35 個の名前の文字列は約 700 byte。`vkGetInstanceProcAddr` で hash から引けば消えるが、「名前で引かない」(§7.1)という今の約束を変えることになる。
- **SMOL-V 型の変換**(§7.5)。shader の lzma は約 -266 byte だが、作品側の復号器(数百 byte、推測)と相殺する。
- **shader。** 最終の frag の約 28 % が前置きで、定数が 632 byte。定数をさらに共有するか、仮数部の下位 bit を 0 にした値を使えば圧縮後が減る(推測)。vert の使わない `GLSL.std.450` の import(6 word)は、SDK の pass では消せない。
- semaphore を image ごとに持つのをやめる案もあるが、IL-2 で避けた競合が戻る。

## 10. 完了記録

| WP | 日付 | commit(`develop_v1`) | test |
|---|---|---|---|
| IL-1 `pelican_cli intro-link serve` / `bake`、client header、`PELICAN_WITH_INTRO_LINK` | 2026-10-02 | da7d5b64 | `ctest -R "intro_link\|devcli_dist_config\|feature_registry"` 7/7 緑(新 3 本: `devcli_intro_link_bake`、`intro_link_eval_matches_engine`、`devcli_intro_link_serve`) |
| IL-2 Vulkan の実例 program と大きさの実測 | 2026-10-02 | 241ed9b8 | `ctest -R intro_link` 4/4 緑(新 1 本: `intro_link_example_baked_is_small`、上限 27,648 byte) |
| IL-3 実例を 17,920 → 10,704 byte に削る(shader の書き直し、最適化の組、layout、Vulkan の code)、bake / serve の大きさの表示 | 2026-10-02 | 517c5ab5 | `ctest -R "intro_link\|devcli_dist_config\|feature_registry"` 8/8 緑(新しい test なし。`intro_link_example_baked_is_small` の上限を 16,384 byte に、`devcli_intro_link_bake` に大きさの表示の確認を足した) |

台帳(`docs/implementation_plan.md`)には、`develop_v1` を `develop` に戻すときにこの表を写す。
