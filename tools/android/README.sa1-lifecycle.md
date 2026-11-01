# SA1 lifecycle の確認

<作業ディレクトリ>/sa1 で PowerShell を開く。端末は <serial> 固定、package は
org.pelicanengine.sa1b。最新の windowed APK を install し、利用者が手で
画面を点灯・ロック解除してから実行する。runner は既存 app を force-stop
して今回のログ・終了結果を消し、`--frames 0` で起動する。

```powershell
$python = '<home>/AppData/Roaming/uv/python/cpython-3.12-windows-x86_64-none/python.exe'
& <作業ディレクトリ>/android/platform-tools/adb.exe -s <serial> install -r build-android/sa1e-fix-apk/pelican-sa1b-debug.apk
& $python tools/android/sa1_run_lifecycle.py --manual-sleep-cycles 3 --label manual-sleep-3
```

stdout の `1/3: 電源で消灯…` が出たら、利用者が端末の電源ボタンで消灯し、
点灯して、普段の方法でロックを解除し、app に戻る。`完了` と次の回の案内を
待ってから繰り返す。Claude はこの runner を起動して進行を監視し、端末の
消灯・点灯・ロック解除は利用者に任せる。runner はその操作を adb で行わない。
PIN・パスワード等を Claude に渡す必要はない。

runner は各回の PAUSE → RESUME を待つ。TERM_WINDOW があれば、その解放ackと
新しい INIT_WINDOW / revision を確認する。各復帰後は現在の revision の
accepted present 累計が増えることを30秒以内に必須とする。TERM_WINDOW のない回も
同じ累計で描画継続を確認する。これは present の受付成功であり scanout の測定ではない。

手動操作の待ちは全3回で最大10分。必要なら `--manual-timeout 900` のように
秒数を指定する。全回完了後は runner が BACK を送り、DESTROY と process 終了、
今回の `engine-result.txt` の `0`（writer の改行1個のみ）、validation enabled / error 0、
全TERMのackとsurface epoch列を確認する。DESTROY期限による強制_exitは失敗。
進行と各回の TERM / ack / INIT 件数は stdout に1行ずつ出る。

証跡は `build-android/sa1e-results/<label>/`（同名は上書きしない）。
各回の `manual-N-commands.log`、`manual-N-restored-engine.log` / pump.log / PNG、
最終ログ、engine-result、pipeline cache、全adb argvの commands.json を保存する。
期限切れやロック検出では非0で停止し、証跡を残す。失敗後の app は自動で終了させない。
初めからロック中なら exit 3。利用者が解除してから新しいlabelで再実行する。

自動確認は `--case home`（3回）、`--case startup`（shader compile中HOME）、
`--case back`、`--case fault`（maintenance proof unavailableによるfinish）を使う。
例: `& $python tools/android/sa1_run_lifecycle.py --case home --label home-fix`。
`--case sleep` は従来の自動消灯・点灯用で、点灯後のロックを検出すると停止する。
ロックのある今回の端末では `--manual-sleep-cycles` を使う。
