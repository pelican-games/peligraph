# オーディオ(v1)

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-08。レビュー前)。
前提: `design_asset_format_policy.md`(WAV+Ogg 決定済み)、
`design_build_tiers.md`(新サブシステムは誕生時からユニット化)。

## 0. 方針

- **`PELICAN_WITH_AUDIO` ユニットとして誕生**(build tiers 要件の初適用例)。
  OFF = 明確エラー、dist-config が音声アセット有無から導出
- バックエンドは **miniaudio** を推奨(単一ヘッダ・MIT-0/public domain・
  WASAPI 等の OS 差を吸収・WAV デコーダ内蔵・Ogg は stb_vorbis 連携)。
  FetchContent 追加 1 件で済む
- **headless / rpc 駆動時は null バックエンド**(デバイス不要・無音で全 API が
  成功する)。CI と決定性のため必須 — 音は golden 化しない、テストは
  「再生状態の観測」(playing / stopped / 再生位置)で行う

## 1. 三層構造

| 層 | 内容 |
|----|------|
| バックエンド | miniaudio デバイス + null。ミキシングは miniaudio のエンジン機能を使う |
| バス | master / bgm / se の 3 バス固定(v1)。バス毎音量。設定永続化(`design_persistence.md`)と連携予約 |
| API | GameContext 経由(下記) |

## 2. API(v1)

```cpp
SoundHandle playSound(std::string_view path);        // SE: 発火して忘れる(se バス)
SoundHandle playMusic(std::string_view path, bool loop); // BGM: ストリーム再生(bgm バス)
void stopSound(SoundHandle);
void setBusVolume(std::string_view bus, float volume); // "master"/"bgm"/"se"
bool isPlaying(SoundHandle) const;
```

- path は project:// / engine://(PathResolver 経由 — 絶対パス拒否も同じ)
- 形式: WAV = SE(全読み)、Ogg = BGM(ストリーミング)。
  他形式はソース層(変換して imports/ へ)— asset policy 準拠
- 3D 音響 v1 = **距離減衰 + パンのみ**(listener = アクティブカメラ)。
  HRTF・オクルージョンはスコープ外

## 3. データ駆動(v2 予約)

- scene v1 に `audio_source` コンポーネント(ループ環境音の配置)
- pelican.audio_banks(SE 名 → ファイルの間接層。ゲームコードにパスを
  直書きしないための辞書)— 需要が出たら

## 4. 実装順(WP 候補)

| 段階 | 内容 |
|------|------|
| A1 | ユニット + miniaudio + null バックエンド + WAV SE 再生 + バス音量 + GameContext API + OFF スモーク |
| A2 | Ogg ストリーミング(BGM)+ フェードイン/アウト + 設定永続化連携 |
| A3 | 距離減衰/パン + audio_source コンポーネント |

### 4.1 A2 の置き換え(2026-09-25 AU-1 `feca9dce`)

A2 の `playMusic(path, loop)`・BGM バス・フェードは、**AU-1 で次のものに置き換えた**。§2 の `playMusic` は実装しない。

- BGM とループ: 共通グラフの `audio.playback`(parameter `bus: "bgm"`、`wrap: "repeat"` など)。
  graph の時計が「今どこを鳴らすか」を決める(一体版 = 写像を parameter で持つ / 分解版 = `value.playback → position`)
- ゲームコードから BGM バスへ: `AudioContext::playSound(path, "bgm")`(`src/core/userpublic/gamecontext.hpp`)
- フェード: `value.curve → audio.volume`。音量は 1 frame かけて寄せる
- **Ogg / ストリーミングは AU-2**(AU-1 の時点で対応形式は WAV だけ)

あわせて §0 の「headless / rpc 駆動時は null バックエンド」も AU-1 で変わった: 窓あり(rpc で動く Studio の埋め込み player を含む)は device、
headless は device を開かない offline エンジン、null は開けなかったときの退避先(理由は `get_status.audio.fallback_reason`)。
利用者向けの説明は `docs/manual/08_gameplay.md` §8.11。

## 5. 未決事項

1. Ogg デコーダ: miniaudio 同梱の stb_vorbis で足りるか(A2 で判断)
2. web プロファイル: WebAudio 写像は形式(WAV/Ogg)がそのまま通るので
   サブセット原則は保てる — WW 化は web 側に音の需要が出てから
3. SoundHandle の世代管理(再利用事故防止)— handle.hpp の既存流儀に従う

## 6. 音の解析の焼き込み(AB-1、`892a1455`、2026-10-02)

利用者の方針(2026-10-02): DCC 寄りの機能は入れるが、完成時は完全に焼き込む。v0 の音の解析は焼き込みにする。将来のリアルタイムを塞がない。
決定性(scrub / headless / null backend で同じ値)を保つ。形は GUI session と合意した(2026-10-02)。利用者向けの説明は
`docs/manual/05_assets.md` §5.8「音の解析を焼いた表」、読み方は `docs/manual/06_rendering.md` §6.18「焼いた音の解析の表を読む」。

### 6.1 焼く

- 操作は明示の Bake だけ。取り込みのたびには走らない。口は 2 つで同じ関数を呼ぶ:
  RPC `bake_audio_analysis {asset, out, settings}`(render config editor の非同期 ticket。worker の thread で焼き、frame と RPC を止めない。
  committed の結果は `{out, manifest, layout}`)と、CLI `pelican_cli audio analyze`
- decode は engine の既存の decode を使い、新しい decoder は入れない。CLI が engine を link せずに同じ decode を使えるよう、
  WAV / Ogg の decode を `src/core/audio/sounddecode.cpp` に移した
- 解析は rms・peak・N 本の帯域。帯域は 20 Hz から Nyquist までの対数間隔で、Hann 窓と自前の FFT で計算する(依存を足さない)
- settings: `features`(rms / peak / bands、既定は全部)、`bands`(既定 8)、`hop_ms`(既定 10)、`window_ms`(既定は hop の 2 倍)、
  `smoothing {attack_ms, release_ms}`(既定 0 = なし)、`channels`(mix / left / right、既定 mix)。範囲は manual 05 の表

### 6.2 保存形

- 表は 1 つの KTX2(`R32_SFLOAT`、2D)。幅は 4096 で固定。行の並び(feature)は rms、peak、band:0 .. band:N-1(settings で省いたものは入れない)。
  各 feature は `ceil(標本の数 / 4096)` 行を縦に使い、feature f の標本 i は `(i % 4096, f × rows_per_feature + i / 4096)`
- 隣に既存の `pelican.import` manifest(`<out>.import.json`)を置く。新しい manifest の形は作らない。
  tool は `pelican.audio_analysis`(版 `"1"`)、`source.metadata` に元の音の sha256・settings(既定を埋めた全部)・settings_sha256・layout、
  output の schema は `pelican.audio_analysis_table` v1
- `created` は書かない。同じ bytes と settings からは同じ 2 つの file になる

### 6.3 読む

- CPU: graph の op `value.sample_table {asset, row, time?}`(module `pelican.value`、`evaluation: baked`)。row は名前か番号。
  time の edge を省くと scene_time(`audio.playback` と同じ時計)。表は graph を組む時に読むので、焼き直しは graph の組み直しまで届かない
- GPU: engine の include `pelican_sample_table.glsl` の `pelican_sample_table(...)`。CPU と同じ読み方(2 標本の線形補間)。
  表は `render.image` の `initial: {asset}`(IA-1)で読み込む想定。GPU で実際に描く test は無い(未確認)

### 6.4 古いかどうか

- 元の音の sha256 が manifest の値と違うか、manifest の settings が settings_sha256 と合わなければ stale。表そのものの欠けや sha256 の違い、tool の版の違いも stale の理由になる
- RPC `list_derived_assets {}` が asset store の `*.import.json` を全部読み、`get_frame_plan` の `initial_asset` も同じ判定を付ける。
  古くなっても自動では焼き直さない

### 6.5 将来のリアルタイム(未実装)

- 将来の op `audio.analysis {voice|bus, features}` は、bake と同じ行の並び(rms、peak、band:0 ..)で、**今の frame の値**を出す形にする。
  表の形式と row の名前を bake と共有するので、`value.sample_table` で組んだ graph をリアルタイムの値へ差し替えられる
  (`src/project/audioanalysisformat.hpp` の冒頭の注記も同じことを書いている)
- 今回は design に書くだけで、op も parameter も登録していない
