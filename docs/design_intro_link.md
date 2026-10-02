# intro link: 外の小さな Vulkan program へ曲線とシェーダを送る口と、焼いて切り離す道

対象読者: 開発ツール担当、intro を作る利用者。
ステータス: v1 向け。IL-1 / IL-2 が `develop_v1` に着地済み(2026-10-02)。`develop` には入っていない。
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
提出時:  project ──(bake:  同じ glslc・同じ引数)──> C header ──(#include)──> 作品(提出の形)
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

振り分けは [runIntroLinkCommand](../src/devcli/introlinkcommand.cpp#L98)。
例外はここで受けて stderr に 1 行出し、終了コード 1。未知の verb と引数なしは usage を出して -1。

### 2.1 引数

| 引数 | 意味 |
|---|---|
| `<project>` | `project.json` がある dir、または `project.json` そのもの |
| `--out <header>`(bake、必須) | 書く C header。親 dir は作る。失敗したら書かない |
| `--shaders <dir>` | 既定 `shaders`。project からの相対(絶対 path も可) |
| `--glslc <path>` | glslc の場所。探す順は `--glslc` → `$VULKAN_SDK/Bin`(POSIX は `Bin` と `bin`)→ PATH([findGlslc](../src/devcli/introlinkproject.cpp#L257)) |
| `--port N`(serve) | 既定 47611。`0` なら空いている port を取る |
| `--exit-after-client`(serve) | 最初の client が切れたら 0 で終了する(test 用) |

bake の成功時は stdout に `wrote <path> (<N> tracks, <M> shaders)` を 1 行出す。
bake は shader が 1 本も無ければ glslc を探さない。serve は起動時に必ず探す([serve の引数](../src/devcli/introlinkcommand.cpp#L72))。

### 2.2 集める対象と名前の規則

| 種類 | 集める対象 | 名前 |
|---|---|---|
| track | `passes/*.json` のうち `schema: "pelican.graph"` の文書。top-level の `nodes` と `definitions.*.nodes` の `op: "value.curve"` の node([collectTracks](../src/devcli/introlinkproject.cpp#L209)) | node の `id` |
| shader | `<shaders>` 直下の `.vert` / `.frag` / `.comp`。下位 dir は対象にせず、include の探索先にだけなる([collectShaders](../src/devcli/introlinkproject.cpp#L236)) | `<file の stem>_<stage>`(例 `intro_frag`) |

- 名前は C の識別子(`[A-Za-z_][A-Za-z0-9_]*`)でなければ拒否する。project の中で重複したら拒否する(§5)。track と shader の名前は別の名前空間。
- curve は `parameters.keys` を読む([readKeys](../src/devcli/introlinkproject.cpp#L63))。`time` / `value` 必須、`interpolation` は `linear`(既定)/ `step` / `cubic`、`in_tangent` / `out_tangent` は任意。
- key の時刻は float に落とした後で狭義単調増加を確かめる。値が float に収まらなければ拒否する。
- curve の `time` 入力の繋ぎ(`value.playback` など)は見ない。作品が渡す秒をそのまま key の時刻として評価する(§9)。

### 2.3 glslc を子 process で呼ぶ

`pelican_cli` に shaderc を link しない。glslc を `runProcess` の子 process で呼び、SPIR-V を stdout で受ける(timeout 60 s)。
引数は serve と bake で同じ([compileShader](../src/devcli/introlinkproject.cpp#L305))。開発中と提出物で SPIR-V を変えないため。

```
glslc --target-env=vulkan1.2 -Os -I <shaders> -I <shaders>/include <file> -o -
```

stage は拡張子から glslc が決める。

### 2.4 bake の出力

[renderBakedHeader](../src/devcli/introlinkproject.cpp#L333) が C header を 1 本書く。

- `pil_key` の型、`enum pil_baked_track_id { PIL_TRACK_<name> = i, … }`、`pil_baked_keys[]`、`pil_baked_track_first[]` / `pil_baked_track_count[]`。
- `enum pil_baked_shader_id { PIL_SHADER_<name> = i, … }`、shader ごとの `pil_baked_shader_<name>[]`(1 行 8 word の hex)、`pil_baked_shader_words[]` / `pil_baked_shader_word_count[]`。
- 決定的。並びは名前の昇順、改行は LF。時刻も絶対 path も書かない(文書名の comment は project からの相対)。
- 小数は `std::to_chars` の最短表記に `f` を付ける。wire にも同じ float を流す。
- track / shader が 0 本なら `PIL_BAKED_TRACK_COUNT 0` などだけを出し、enum も配列も出さない。
- SPIR-V は glslc の版に依存する。決定性は「同じ glslc で」の範囲。

### 2.5 serve

- 127.0.0.1 の TCP で待つ。起動したら stdout の 1 行目に `port <N>` を出して flush する。stdout にはそれ以外を書かない(log は stderr)。
- client の HELLO が合えば snapshot を送る([snapshot](../src/devcli/introlinkserve.cpp#L476)): 全 TRACK(名前の昇順)→ 全 shader(SPIR-V があれば SHADER、compile error があれば SHADER_ERROR)→ TIME → SNAPSHOT_END。
- 以後 100 ms ごとに file を見る。
  - `passes/*.json` の (mtime, size) が変わったら全 track を読み直し、keys が変わった track だけ TRACK を送る([watchTracks](../src/devcli/introlinkserve.cpp#L525))。
  - shader は file ごとに見て、変わったものだけ compile し直す。下位 dir のどれかが変われば全 shader を compile し直す。新しい shader file も拾う([watchShaders](../src/devcli/introlinkserve.cpp#L551))。
  - compile に失敗したら SHADER_ERROR(glslc の stderr)を送る。client は前の SPIR-V を持ち続ける。直ったら SPIR-V が前と同じでも SHADER を送る(client の error を消すため)。
  - 読み直しの失敗(書きかけの JSON など)は stderr に 1 行出して前の状態を保ち、次の tick でまた試す。同じ文言は繰り返さない。
- 起動時の project のエラー(名前の不正・重複、curve の不正、glslc が無い)は bake と同じ名前付きエラーで終了する。起動時の shader の compile 失敗では終了せず、snapshot に SHADER_ERROR を入れて続ける。

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
- `src/core`・`src/player`・`src/devstudio` には 1 行も無い。IL-1 / IL-2 の commit はこの 3 つの dir の file を含まない。
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

## 7. 実例 program と実測(IL-2)

置き場は `tools/intro_link/example/`。

### 7.1 構成

- project `tools/intro_link/example/project/`: `value.curve` が 3 本。`fade`(linear、0→1 を 1 秒、7→8 秒で 0 へ)、`pulse`(cubic、1 秒ごとに 0/1)、`scene`(step、0 / 3 秒で 1 / 5.5 秒で 2)。最後の key はどれも 8 秒。
- `shaders/intro.vert`: 頂点 buffer なしの全画面三角形(`gl_VertexIndex`)。
- `shaders/intro.frag`: push constant `{float time; float aspect; float track[3];}`(20 byte)だけを読む。球を raymarch し、`pulse` が半径と rim、`fade` が明るさ、`scene` が形を決める。色の定数は `const vec3 BASE`。
- 作品 `intro.c`(C の 1 file、2 つの形を `#ifdef PELICAN_INTRO_LINK_BAKED` で分ける):
  - Win32 の窓(1280×720 固定)と Vulkan 1.3。dynamic rendering(`vkCmdBeginRendering`)、push constant だけ、頂点 buffer・descriptor・depth・memory の確保なし。swapchain の image に直接描く。
  - Vulkan の関数は core と WSI の 33 個で、すべて `vulkan-1.lib` から import する(`vkGet*ProcAddr` なし)。
  - validation、device の選別、swapchain の作り直し、error の検査は書かない(intro の割り切り。最初の physical device、queue family 0 を仮定)。
  - 時刻は `QueryPerformanceCounter`。`pil_time` が 0 で 8 秒(`INTRO_SECONDS`)を越えたら終える。ESC と窓の × でも終わる([終了の判定](../tools/intro_link/example/intro.c#L440))。
- 開発の形 `intro_link_example_dev`: CRT あり。引数 `[port] [--frames N] [--shot path.bmp]`。`PIL_SHADER_CHANGED(intro_vert) | PIL_SHADER_CHANGED(intro_frag)` で `vkDeviceWaitIdle` → 古い pipeline を破棄 → 作り直す([作り直し](../tools/intro_link/example/intro.c#L445))。server に繋がらないか SPIR-V が無ければ 1 で終わる。
- 提出の形 `intro_link_example`: bake の header を build 時に `add_custom_command` で作る(DEPENDS は `pelican_cli` と project の全 file)。CRT なし: 自前の entry(`/ENTRY:entry`、`ExitProcess`)、`_fltused`、`memset` / `memcpy` を `#pragma function` で自前(`__stosb` / `__movsb`)。
- build する条件: `PELICAN_WITH_INTRO_LINK=ON`、64-bit Windows + MSVC、Vulkan の header と `vulkan-1.lib`、glslc。欠ければ configure の log に 1 行出して飛ばす([条件](../tools/intro_link/example/CMakeLists.txt#L6))。

### 7.2 提出の形の flag

build type に関係なく固定。example の dir の scope で、上から来る directory property と `CMAKE_C_FLAGS*` / `CMAKE_EXE_LINKER_FLAGS*` / `CMAKE_C_STANDARD_LIBRARIES` を空にしている([scope の初期化](../tools/intro_link/example/CMakeLists.txt#L23))。Debug の tree でも `/RTC1` `/MDd` `/Zi` は乗らない。

```
compile: /O1 /Os /GS- /Gs9999999 /GR- /Gy /Gw /Zl /W3
link:    /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:WINDOWS /MACHINE:X64
         /OPT:REF /OPT:ICF /MERGE:.rdata=.text /MERGE:.pdata=.text
         /DYNAMICBASE:NO /FIXED
         /INCREMENTAL:NO /DEBUG:NONE /MANIFEST:NO /EMITPOGOPHASEINFO
libs:    kernel32 user32 vulkan-1
```

([compile](../tools/intro_link/example/CMakeLists.txt#L64)、[link](../tools/intro_link/example/CMakeLists.txt#L66))。dumpbin で Resource Directory = 0、Debug Directory = 0。

### 7.3 大きさ

提出の形 `intro_link_example.exe`(上の flag):

| 項目 | byte |
|---|---|
| file の大きさ | **17,920** |
| headers(DOS + PE、FileAlignment 0x200) | 1,024 |
| `.text`(`.rdata` と `.pdata` を merge 済み)virtual / raw | 14,488 / 14,848 |
| うち SPIR-V `intro_frag`(2133 word) | 8,532 |
| うち SPIR-V `intro_vert`(233 word) | 932 |
| 残り(機械語、文字列、key の表 17 個 × 20 byte、`.pdata` 0x54 など) | 約 5,024 |
| `.data` virtual / raw | 40 / 0 |
| `.idata` virtual / raw | 1,804 / 2,048 |

圧縮した大きさ。**packer は使っていない。python の `zlib` / `lzma` で exe 全体を圧縮した値で、packer の代わりの目安。**

| 対象 | 元 | zlib level 9 | xz(preset 9e) | lzma raw(LZMA1、9e) |
|---|---|---|---|---|
| 提出の形 | 17,920 | 7,189 | 5,924 | 5,867 |
| 開発の形(Debug) | 58,368 | 22,632 | 19,512 | 19,452 |
| 参考: `/ALIGN:16 /FILEALIGN:16` の exe | 16,880 | 7,151 | 5,900 | 5,840 |

開発の形(Debug の tree、`/MDd /Od /Zi /RTC1`)は 58,368 byte。import は KERNEL32 / USER32 / WS2_32 / vulkan-1 / VCRUNTIME140D / ucrtbased。

link の flag ごとの効き(同じ obj を手で link し直した。共通は `/NODEFAULTLIB /ENTRY /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:NO /EMITPOGOPHASEINFO /DEBUG:NONE /INCREMENTAL:NO`):

| 足した flag | file |
|---|---|
| なし | 18,944 |
| `/MERGE:.rdata=.text` | 18,944(512 byte の境界を越えず効かない) |
| 上 + `/DYNAMICBASE:NO` | 18,432(`.reloc` が消える) |
| 上 + `/FIXED` | 18,432(上乗せ 0) |
| 上 + `/MERGE:.pdata=.text`(採用) | **17,920** |
| `/MERGE:.idata=.text` | LNK1272(merge できない) |
| `/FILEALIGN:16` | 16,928。image として map できない(error 193) |
| `/ALIGN:16 /FILEALIGN:16` | 16,976。map は通る |
| `/ALIGN:16 /FILEALIGN:16` と `.pdata`・`.data` の merge | 16,880。map は通る |

`/ALIGN:16` は約 1 KB 減るが、確かめたのは `LoadLibraryExW(AS_IMAGE_RESOURCE)` で map できることだけ。process として走らせていないので採用していない。

compile の flag では `/Gw` が効く(付けないと 20,480、付けると 17,920)。`/O2` は 18,944、`/fp:fast` は変化なし。

### 7.4 SPIR-V の削り(測っただけ。bake の道には足していない)

SPIRV-Tools v2025.1 / SDK 1.4.309.0 で測った。

| 処理 | frag | vert | 備考 |
|---|---|---|---|
| bake の出力(glslc `-Os`) | 8,532 | 932 | |
| `spirv-opt --strip-debug --strip-nonsemantic` | 8,532 | 932 | 変化なし。glslc の `-O*` は既に OpName などを出していない |
| `spirv-remap --map all --strip all` | 8,532 | 932 | 大きさは同じで中身が変わる(spirv-val は通る) |
| 上の圧縮(zlib9 / lzma raw) | 3,019 / 1,987 | 439 / 393 | remap 前は 2,796 / 1,959 と 384 / 330。この shader では remap すると圧縮後が少し大きくなった |
| `spirv-opt --canonicalize-ids` | — | — | この SDK の spirv-opt は未対応(`Unknown flag`) |
| 参考: normal の 6 点差分を 4 点の loop にした frag | 5,168 | — | glslc は関数を全部 inline するので、frag の大きさは主に呼ぶ回数で決まる。作品の shader は変えていない |

### 7.5 実機の確認(IL-2、この機械の最初の physical device で 1 回ずつ)

- 開発の形: serve に繋いで 240 frame。途中で `intro.frag` の `BASE` を書き換え、pipeline の作り直しがちょうど 1 回(書き換えから約 0.28 秒後)。dev = 0、serve は `quit` で 0。書き換えたのは scratch に写した project。
- screenshot(`--shot`、開発の形だけの道): 黒一色でなく、書き換え後の色の球が出ていた。
- 提出の形: 終了コード 0、8.78 秒で自分から終わった。

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
   - `tools/intro_link/example/project/shaders/intro.frag` の `const vec3 BASE`。色が変わり、dev の端末に `intro: pipeline rebuilt (1)` が出る。壊すと compile error が dev の端末に出て、前の絵が残る。
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

### 9.1 残した穴(IL-1 / IL-2 の報告と実物から)

- curve の `time` 入力の写像(`value.playback` の範囲・速度・wrap・scale/offset)は見ない。作品が渡す秒をそのまま key の時刻にする。
- track を集めるのは top-level の `nodes` と `definitions.*.nodes` まで。loop の body など、より深い所の curve は拾わない。
- project から消えた track / shader を client に知らせる frame が v1 に無い。
  - track: serve は stderr に 1 行出す。client は最後の keys を持ち続ける。後から繋いだ client の snapshot には入らない。
  - shader: serve は何も出さず、内部の状態からも消さない。後から繋いだ client の snapshot にも最後の SPIR-V が入る。
- server の send は blocking。client が長く poll しないまま大量の更新が来ると、server の loop が止まりうる(localhost で作品が毎 frame poll する前提)。
- 監視は (mtime, size)。同じ時刻・同じ size の書き換えは見逃す。include の依存は解析せず、下位 dir のどれかが変われば全 shader を compile し直す。
- POSIX の経路は compile も実行もしていない(Windows だけで確認)。C99 strict の POSIX では `_POSIX_C_SOURCE=200112L` が要る。
- 本番の build-unit smoke(`-DPELICAN_BUILD_UNIT_SMOKE_ONLY=intro_link`、OFF 構成の全体 build)は未実行。確かめたのは `pelican_cli` だけの OFF build と、parse-only の `feature_registry_smoke_matrix_contract`。
- Studio から serve を起動して時刻を握る接続は未着手(`src/devstudio` には何も無い)。
- 実例 program について:
  - 最初の physical device と queue family 0 を仮定する。present できない構成では動かない。
  - 窓の大きさは固定で、swapchain は作り直さない。
  - loader が 1.3 未満だと `vkCmdBeginRendering` の import が解決できず起動しない。
  - 開発の形は server が無いと起動しない(SPIR-V を server からしか受けないため)。
  - ESC と × での終了、提出の形の画面の見た目、他の GPU での動きは確かめていない。
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
- SPIR-V を SMOL-V 型に変換して焼く(§7.4 は測っただけ)。
- 複数の entry point を 1 つの SPIR-V にまとめる。

## 10. 完了記録

| WP | 日付 | commit(`develop_v1`) | test |
|---|---|---|---|
| IL-1 `pelican_cli intro-link serve` / `bake`、client header、`PELICAN_WITH_INTRO_LINK` | 2026-10-02 | da7d5b64 | `ctest -R "intro_link\|devcli_dist_config\|feature_registry"` 7/7 緑(新 3 本: `devcli_intro_link_bake`、`intro_link_eval_matches_engine`、`devcli_intro_link_serve`) |
| IL-2 Vulkan の実例 program と大きさの実測 | 2026-10-02 | 241ed9b8 | `ctest -R intro_link` 4/4 緑(新 1 本: `intro_link_example_baked_is_small`、上限 27,648 byte) |

台帳(`docs/implementation_plan.md`)には、`develop_v1` を `develop` に戻すときにこの表を写す。
