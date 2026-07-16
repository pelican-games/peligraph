# WP109 Side-Scroller Vertical Slice

```powershell
pelican_player.exe --project projects/sprite_demo
```

WP106 の strict pixel policy / `FlipbookClip` と WP107 の `shapeCastAll` を、
WP109 の非特権 `platformer::moveAndSlide` で接続した side-scroller 縦切り。

- `A` / `D`: 移動
- `Space`: ジャンプ
- `S`: one-way platform をすり抜ける

床、斜面、one-way platform、壁は scene collider で、player の dynamics と
方向付き one-way 判定は project code 側にある。物理 Provider を Builtin / Jolt /
ゲーム DLL 実装へ差し替えても controller policy は変わらない。着地は E1 event を
次フレームへ配送し、歩行中は公開 `FlipbookClip` だけで atlas frame を更新する。

```powershell
cmake -S . -B build-sprite-demo -DPELICAN_PROJECT=projects/sprite_demo -DSKIP_DEVSTUDIO=ON
cmake --build build-sprite-demo --config Debug
```
