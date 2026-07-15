# WP106 Sprite / Pixel-Policy Demo

```powershell
pelican_player.exe --project projects/sprite_demo
```

`sprite_view` の layer、flip、tint、回転を単一 atlas page で表示する自己完結 demo。
プロジェクトコードも含めて configure すると、非特権の公開 `FlipbookClip` が
`left` / `right` atlas frame を 0.2 秒ごとに切り替える。

```powershell
cmake -S . -B build-sprite-demo -DPELICAN_PROJECT=projects/sprite_demo -DSKIP_DEVSTUDIO=ON
cmake --build build-sprite-demo --config Debug
```
