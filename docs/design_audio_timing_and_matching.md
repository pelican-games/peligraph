# 音とゲームの合わせ: 音の時計を正にする土台と、graph で育てる audio 領域

対象読者: エンジン担当(audio / appflow / input)。
ステータス: 改善案メモ(2026-10-22。議論の記録。WP 化・レビュー前)。
前提: マニュアル [第8章 §8.11 オーディオ](manual/08_gameplay.md)(AU-1 / AU-2 / AU-3、AB-1)、
[design_render_stage_as_canvas_filter.md](design_render_stage_as_canvas_filter.md)(graph の領域フィルター)、
package(`pelican.package` v1。[第3章 §3.11](manual/03_project_format.md))。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- ねらいは 2 つ。**(a) 音楽ゲーム級の合わせ**(判定 ±2 ms 程度、音の発火はサンプル精度、ドリフト 0)、**(b) 音とゲームのマッチング**
  (ゲーム状態 → 音、音 → ゲームと絵、アニメ / 物理 → 音)を graph の audio 領域で書けること。
- (a) の本質は精度ではなく **どの時計を正にするか**。今は timeline(`scene_time`)が正で、graph の voice は timeline に追随して
  ずれたら seek する(AU-1 決定 3)。音楽ゲームでは **音の DAC の時計が正**で、timeline が音に追随しなければならない。
  この逆向きを「master の voice」だけに許すモードを足す。
- CRI(ADX2)が低層で担うもの(codec、ストリーミング、メモリ、ボイス管理、時間精度)のうち、desktop + mac + 決定性という
  Pelican の前提で価値があるのは **時間精度・audio スレッドの規律・ボイス予算**の 3 つ。自前 codec と機種別ドライバは要らない。
- 判定・譜面・スコアは engine の中核ではなく **package**(graph と component)で配る。engine は時計・入力時刻・予約・遅延報告の土台だけを持つ。

## 1. CRI(ADX2 / CRIWARE)が担っていること(対応表)

| 層 | CRI | Pelican の今 | 置き場 |
|---|---|---|---|
| 作家側 | Cue / CueSheet(束ね・ランダム・スイッチ・ゆらぎ)、AISAC(ゲーム値 → curve → 音量 / ピッチ / フィルタ)、セレクタ / カテゴリ、REACT(ダッキング)、ボイスリミット、3D(減衰 curve、ドップラー)、DSP bus、snapshot、ブロック再生とビート同期 | `audio.playback` / `audio.volume` / `audio.pitch`、bus は `se` / `bgm`、`value.curve`、`value.event_clock` | graph の audio 領域と package(§3) |
| 低層 | HCA / HCA-MX(周波数領域で混ぜて IMDCT 1 回)、CRI File System(優先度付き I/O、先読み)、固定プール、ボイスプールと仮想ボイス、専用スレッドのミキサ(ASR)、サンプル精度の開始とループ、機種別出力 | miniaudio `ma_engine`、wav / Ogg Vorbis、ロード時 decode(mutex)、10 秒超は streaming、ループ点はサンプル精度、`mixOffline` | §2 |
| 計測 | Atom Viewer(ボイス数、CPU、メモリ、ドロップ)、実機プレビュー | 無し | Studio の dock(§3.6) |

## 2. 低層で価値があるもの(やる順)

1. **サンプル精度の予約**: 「device の N サンプル目で鳴らす」を API に持つ(`playSound(path, at)`、`VoiceStart` に device frame の始点)。mixer 側に開始 frame 付きの queue。
2. **audio スレッドの規律**: 確保しない、ロックしない(command queue)。decode は別スレッドか事前。今の `decode_mutex`([`audio.cpp:533`](../src/core/audio/audio.cpp#L533))を audio 経路から外す。
3. **ボイス予算と仮想ボイス**: 上限、優先度、距離で切る、超えた分は進行だけ追う。compile 時に plan として出す(§3.6)。
4. ストリーミングの先読み(BGM と長尺)。アーカイブは asset store の仕事。
5. リサンプラとピッチの品質は miniaudio の設定で足りるかを計測してから。
6. 遅延: device の period と buffer を設定に出し、報告された遅延を `get_status` に。

やらないもの: 自前 codec(Vorbis / Opus で足りる。HCA の軽さは desktop では効かない)、HCA-MX 流の周波数領域ミックス(数百ボイスの要件が出てから)、機種別ドライバ(miniaudio の層)。

## 3. マッチング: graph の audio 領域で育てる(順)

1. **ゲーム → 音(AISAC 相当)**: `audio.filter`(lowpass / highpass)、`audio.send`(bus への送り)を node にし、value の `curve` で写像。snapshot は「状態 → bus の値の組」を value 領域で。REACT 相当は `audio.duck`(sidechain)。
2. **音 → ゲーム / 絵**: AB-1 の bake([`audioanalysis.hpp`](../src/core/audio/audioanalysis.hpp)。rms / peak / 帯域)に beat / onset / tempo を足し、`audio.analysis` を graph の源にする。帯域で bloom を動かす、拍でイベントを出す。bake なので決定的で golden が撮れる。
3. **アニメ / 物理 → 音**: clip のイベントタグ(足音)と物理の接触(速度 → 音量)を cue に結ぶ。cue(束ね・ランダム・スイッチ・素材の表)は JSON 文書にして Studio で編集。
4. **3D**: listener / emitter、減衰 curve、物理クエリで遮蔽。miniaudio の空間化を使う。
5. **インタラクティブミュージック**: 小節 / 拍の clock を `event_clock` に乗せ、ブロック切替と stinger を audio 領域の node に。
6. **ボイス管理と可視化**: 同時発音の上限と優先度を compile 時の「audio の plan」に。Studio に Frame Plan と同じ流儀の dock(bus の木、voice、レベル計、遅延、ドリフト)。

CRI と違う形になるのは、作家のツールが Studio の graph そのもので、音の挙動が render や pose と同じ canvas と同じ決定性の中に入ること。

## 4. 音楽ゲーム級の合わせ

### 4.1 今の時計の関係

| 要素 | 今 | 要るもの |
|---|---|---|
| 正の時計 | timeline(`scene_time`。[`loop.cpp:63`](../src/core/appflow/loop.cpp#L63) の steady_clock)。voice は timeline に追随し、cursor が `tolerance` より離れたら seek([`audio.hpp:59-72`](../src/core/audio/audio.hpp#L59) `VoiceDrive`、AU-1 決定 3) | **音の DAC の時計**。timeline が音に追随。seek で合わせる方式はずれるたびに音が飛ぶ |
| 出力遅延 | miniaudio の既定。period と遅延を設定も報告もしていない | backend から取る(WASAPI / CoreAudio は報告する)、period を設定できる、ユーザー校正の offset |
| 入力の時刻 | GLFW の callback([`window.cpp:259`](../src/core/os/window.cpp#L259))を frame ごとに poll([`:375`](../src/core/os/window.cpp#L375))。入力の時刻 = frame の時刻 | frame より細かい時刻。OS のイベント時刻か専用スレッドの標本化を、音の時計に写す |
| 音の発火 | `playSound` は「今」。`VoiceStart.start` は source 秒 | 「device の N サンプル目で」の予約。keysound は前もって queue に |
| 絵 | in-flight 2 + swapchain で 2〜3 frame の遅延。present 時刻は未計測 | present 時刻の音の時間を予測して描く |
| 決定性 | offline の fixed step、`mixOffline`、入力の record / replay(`--record-input` / `--replay`) | そのまま強み。入力を音の時刻で記録すれば判定を offline で再現できる |

### 4.2 変えること(順)

1. **音の時計を正にするモード**(決定として): backend が「再生済みサンプル数 + 出力遅延」を返し、`value.input` に `audio_time` を足す。
   音楽ゲームの timeline はこれから導き(steady_clock との写像は線形回帰でドリフト補正)、**master に指定した voice は seek しない**。
   AU-1 決定 3 の逆向きを master の voice だけに許す。`audio.clock_mismatch@1` の規則(events を繋ぐなら `scene_time`)はこのモードでは `audio_time` も受ける。
2. **入力の時刻**: 入力イベントに steady_clock の時刻を付ける(Windows: raw input か `GetMessageTime`、mac: `NSEvent.timestamp`、無ければ 1 kHz の標本化スレッド)。判定は音の時刻の領域で。record / replay の時刻も音の時刻に。
3. **サンプル精度の予約**(§2 の 1 と 2)。
4. **遅延の設定と校正**: period size と exclusive mode を設定に、報告された遅延を `get_status` に、ゲーム設定に audio offset / visual offset、校正ツール(クリックに合わせて叩く、または loopback 計測)。
5. **絵の側**: present 時刻の予測値で譜面を描く。in-flight は 2 のまま予測で補う。将来 `VK_KHR_present_timing` 系で実測。
6. **検証**: offline で映像と音を同時に書き出し、譜面・判定・音・絵の一致を golden にする(決定性だからできる検証)。

### 4.3 目標値

- 判定: 校正後の定常 offset を除き ±2 ms 程度(入力の時刻付けの精度で決まる)。
- 音の発火: サンプル精度(予約方式なら誤差 0)。
- ドリフト: 5 分の曲で 0(時計を音に固定する)。
- 絵: 1 frame 以内(予測値で描く)。

### 4.4 判定の置き場

譜面、判定窓、スコアは package(graph と component を provides)で配る。engine 側は 4.2 の 1〜5 の土台だけ。
feature / package を「誰でも作れる単位」にする方向([design_plan_layer_inventory.md](design_plan_layer_inventory.md) の議論)の最初の実例になる。

## 5. 段階

| 段 | 内容 | 検証 |
|---|---|---|
| 0 | 計測: 出力遅延の報告、`audio_time` の読み出し、入力の時刻付け。Studio / overlay に遅延とドリフト | 実機で数値が出る |
| 1 | 音の時計を正にするモード(master の voice)と、`audio_time` の `value.input` | offline で timeline が `mixOffline` のサンプル数と一致 |
| 2 | サンプル精度の予約と audio スレッドの規律 | 予約した frame で鳴る(offline で byte 一致) |
| 3 | 遅延の設定と校正、present の予測 | 校正ツールで offset が収束 |
| 4 | A/V の offline 書き出しと golden | 譜面 / 判定 / 音 / 絵の一致 |
| 5 | §3 の 1〜6 を順に | graph の golden |
| 6 | 判定 package の雛形 | package template の smoke |

## 6. 未決

- master の voice が複数あるとき(曲の切替、クロスフェード)の時計の受け渡し。
- `audio_time` と `scene_time` の両方を持つ graph の規則(`audio.clock_mismatch@1` の拡張)。
- 入力の時刻付けの実装(OS のイベント時刻か標本化スレッドか)を機種ごとに決める。mac(MoltenVK 実証)と同時に確認。
- offline の A/V 書き出しの形式(frame ごとの画像 + wav か、動画か)。
