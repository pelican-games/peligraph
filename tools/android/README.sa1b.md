# SA1b Android headless の再現

Git Bash を使い、リポジトリのルート <作業ディレクトリ>/sa1 で実行する。
通常の build は validation が必須。取得失敗時に成功扱いにはしない。

```bash
export MSYS_NO_PATHCONV=1
bash tools/android/sa1_configure.sh
bash tools/android/sa1_fetch_validation.sh
NINJA='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
"$NINJA" -C build-android -j 4 pelican_player >build-android/sa1b-build.log 2>&1 &
# 上の job を短い間隔で確認。完了・exit code 0 を確認してから次へ。
wait $!
bash tools/android/sa1_build_apk.sh
PYTHON='<home>/AppData/Roaming/uv/python/cpython-3.12-windows-x86_64-none/python.exe'
"$PYTHON" tools/android/sa1_run_device.py --apk build-android/sa1b-apk/pelican-sa1b-debug.apk --label validation-first
"$PYTHON" tools/android/sa1_run_device.py --label validation-second
```

端末は必ず <serial>。package は org.pelicanengine.sa1b。
`sa1_run_device.py` は install（指定時）、この package の force-stop、起動、
正常終了待ち、有限の logcat dump、run-as による PNG・ログ取得を行う。
120 秒後も実行中ならそのまま残してエラーにし、process を kill しない。
同じ結果 label は上書きしない。
出力は <作業ディレクトリ>/android/results/sa1b/。次の手動コマンドも使える。

```bash
ADB=<作業ディレクトリ>/android/platform-tools/adb.exe
"$ADB" -s <serial> install -r build-android/sa1b-apk/pelican-sa1b-debug.apk
"$ADB" -s <serial> shell am start -n org.pelicanengine.sa1b/android.app.NativeActivity
"$ADB" -s <serial> logcat -d -v threadtime pelican:V '*:S' > <作業ディレクトリ>/android/results/sa1b/manual-logcat.txt
"$ADB" -s <serial> exec-out run-as org.pelicanengine.sa1b cat files/sa1_posegraph.png > <作業ディレクトリ>/android/results/sa1b/manual.png
"$ADB" -s <serial> exec-out run-as org.pelicanengine.sa1b cat files/engine.log
"$ADB" -s <serial> exec-out run-as org.pelicanengine.sa1b cat files/engine-result.txt
```

PNG の `>` は Git Bash で行う。Windows PowerShell 5 の文字列経由のリダイレクトは使わない。
Python の取得スクリプトは bytes のまま保存する。
`engine-result.txt` が 0 であり、今回の capture があることを確認する。
worker は各起動の先頭で以前の PNG を削除する。

VVL の入手先は KhronosGroup/Vulkan-ValidationLayers の
vulkan-sdk-1.4.309.0 release の android-binaries-1.4.309.0.zip。
取得スクリプトは <作業ディレクトリ>/android/deps/ にだけ保存し、archive と .so の SHA256 を
vvl-vulkan-sdk-1.4.309.0/provenance.json に記録する。
取得済み ZIP がある場合は <作業ディレクトリ>/android/deps/android-binaries-1.4.309.0.zip に置き、
同じ取得スクリプトを再実行する。keystore も <作業ディレクトリ>/android/debug.keystore にだけ置く。

`ALLOW_MISSING_VALIDATION=1` は、欠損を明示した APK 構造の診断専用。
これを指定しても `--vulkan-validation on` の engine は layer が無ければ停止する。
今回保存した build-android/sa1b-apk-validation-off/ の APK は一時的な off build であり、
validation の検証には使えない。最終ソースと通常 APK は on。
端末の log.tag=S の環境では、process 内 threshold を INFO にして write が 1 を返しても
logcat dump が空だった。同じ quill ログは files/engine.log にも保存する。
端末の global property は変更しない。

Windows の比較はすでに 1 回実行済みなので繰り返さない。
実行した command と元 EXE の SHA256 は results/sa1b/windows-command.json に保存。
rc2 の EXE を results/sa1b/windows-player/ にコピーし、project も .pelican を除いて
results/sa1b/windows-project/ にコピーして、rc2 と元 project への cache 書き込みを避けた。
960x540、3 frames、headless の条件は Android と同じ。

```bash
"$PYTHON" tools/android/sa1_compare_png.py <作業ディレクトリ>/android/results/sa1b/windows.png <作業ディレクトリ>/android/results/sa1b/diagnostic-off-version-change.png <作業ディレクトリ>/android/results/sa1b/review-comparison
```

比較は外部 Python package 不要。8bit RGB/RGBA・非 interlace の PNG を読み、
RGB MAE/RMSE/max、差のある pixel 数、背景色以外の mask の IoU、左右比較と差分を保存する。
今回の詳細・未検証は <作業ディレクトリ>/sa1_2_report.md を参照。
