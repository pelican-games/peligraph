# Procedural ray tracing

メッシュを使わず、intersection shader が球との交点を求める最小の表示例です。
青い大球と橙色の小球を、固定カメラから表示します。

Vulkan ray tracing pipeline 対応 GPU と、runtime shader compiler を有効にした
Player が必要です。Player のプロジェクト指定でこのディレクトリを開いてください。
カメラ移動の操作はありません。ウィンドウを閉じると終了します。

リポジトリのルートからの起動例:

```powershell
.\build\src\player\Debug\pelican_player.exe --project projects/raytracing_demo
```

読む順序:

1. `scene.json`: 二つの物体の配置と procedural geometry コンポーネント。
2. `assets.json`: ローカル座標の包囲箱 AABB、球の半径・中心。
3. `ray_layout.json` / `ray_bindings.json`: hit group と shader record の対応。
4. `features/spheres.json`: raygen → intersection → closest-hit / miss の接続。
5. `shaders/sphere.rint`: 二次方程式で球の交点を求める、編集可能な intersection shader。
6. `shaders/spheres.*`: カメラのレイ、球の陰影、背景色。
7. `rendering.json`: レイトレ出力画像を fullscreen パスで画面へ表示。

`scene.json` の `pos` を変更して再起動すると配置が変わります。半径を変える場合は
`assets.json` の `record.radius` と、球全体を含む `aabb` を一緒に変更します。
交点計算もこのプロジェクトの `shaders/sphere.rint` で編集できます。
`scene.json` の `scale` を軸ごとに変えると楕円体になります。AABBも物体とともに変換されるので、こちらはローカルの半径を変更する場合と異なりassetの書き換えは不要です。
固定カメラの位置と画角は `shaders/spheres.rgen` の `cameraOrigin` / `verticalFieldOfViewDegrees` で変更します。
二つの交点のうちレイの有効範囲にある手前側を採用し、球の内側から出るレイも扱います。
陰影は closest-hit 内の簡単な指向性照明で、影や反射の二次レイはまだ使っていません。
