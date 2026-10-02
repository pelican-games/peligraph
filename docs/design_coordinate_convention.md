# 座標の規約 —— 鏡像の解消と、world から画面までの約束

状態: **着地**。CC-1(CC-1a / 1b / 1c を 1 本に squash)は develop `c8367b87`(2026-10-03)、CC-2 は `1501685a`(2026-10-03)。v0 に入る。§1 は着地前の実測と code で確かめた事実(develop `bdc96e2d` の時点。今の code ではない)、§2 は裁定、§3 以降は形、§5・§6 に結果。書いた時点は v1(2026-10-02、branch `feature/coordinate-convention`)。利用者向けの説明は [manual 第6章 §6.1](manual/06_rendering.md) の「座標の規約」と [第3章 §3.13](manual/03_project_format.md) の「camera の向きの規約の変換」。

## 0. 経緯と裁定

2026-10-02、v1 向けの 2D module の `canvas.place`(layer stack を 3D に置く)で、正面から見た板の文字が左右反転して出た。利用者の指示「まず glTF で確かめて」で実測し、engine 全体の性質だと分かった(§1)。

| 日付 | 裁定(利用者) |
|---|---|
| 10-02 | 「これはそう急にやるべきだしここは v0 に入れましょう。すべて適切な形にして」 |
| 10-02 | engine の内部は「射影の中で y を反転する」(clip・NDC・画素・uv は Vulkan のまま y 下)。利用者が clip を直接書く raster の pass には、clip の y の向きを選ぶ option を足す。engine 全体を設定で切り替える形にはしない |

world 空間は変えない(右手系、+Y 上、メートル、glTF と同じ)。壊れているのは world のデータではなく、画面への写し方である。

## 1. 今の作り(develop `bdc96e2d`、実測と code)

### 1.1 実測(`canvas` を使わない project)

- 文字「F」を貼った glTF の板(XY 平面、法線 +Z、標準の UV)を (0,0,3) から −Z 向きに見ると、F が**左右反転**で出る。+X に置いた赤い立方体は画面の**左**に出る。camera の向きを変えても同じ
- project の設定 `basic_config.camera.up` を +Y に変えると**上下反転**で出る
- camera の object を 180° ロールさせても同じ絵が出る(回転から up が取られていない)

### 1.2 原因(3 つ重なっている)

1. **射影が y を反転していない。** view は `glm::lookAt`(右手系)、射影は `glm::perspectiveRH_ZO` / `glm::orthoRH_ZO`、viewport の height は正(NDC の +y が画面の下)。右手系の world を y 下向きの画面にそのまま写すので、画面は必ず world の鏡像になる
2. **camera の向きの規約が glTF と違う。** 前方は +Z(`camerasystem.cpp` は dir = R·(0,0,+1) を設定)、up は回転から取らず project の設定値(`setActiveCamera` を通ったときだけ R·Y)。操作系は right = cross(up, dir)(左手系の形)。既定の `up` は −Y(`default_config.json`、blank の雛形、example)。他の project 6 本は +Y
3. **面の表を時計回りに固定して辻褄を合わせている。** material / velocity / shadow depth の pipeline が `vk::FrontFace::eClockwise`。raster pass の既定は反時計回りで食い違っている

打ち消している所は無い(glTF の loader に軸の反転も UV の変換も無い)。XR だけは、射影の中で既に y を反転している(`openxrviewspace.cpp`)。

### 1.3 射影と view を作る所(重複している)

| 所 | 今の形 |
|---|---|
| flat の camera | `camera.cpp` の `rebuildPreparedProjection` と `rebuildProjectionMatrix`(重複)。view は `camera.hpp` と `camera.cpp` の `getVPMatrix` に `lookAt(pos, pos+dir, up)` が 2 か所 |
| editor の preview | `editorpreviewprojection.cpp`(R·(0,0,+1) と自前の射影)、`editorpreviewservice.cpp`(自前の射影) |
| XR | `openxrviewspace.cpp`(既に目標の形。view は `inverse(pose)`、stage は camera の view に固定) |
| cube への描画・点光源の影 | `cubecaptureview.cpp`(lookAt の up = −Y、y 反転なし。Vulkan の cube の面の規約に合っている) |
| spot の影 | `punctualshadowview.cpp`(自前) |
| directional の影 | `vulkanOrtho` が `lightcontainer.cpp` と `directionalshadowcascade.cpp` に重複 |
| planar reflection | main の view に反射行列と clip の x 反転を掛ける(main に連動) |
| golden の harness | `test/golden_harness.cpp`(camera を `setUp(+Y)` で直接置く。射影を自前で作る所もある) |

## 2. 目標の規約

| 空間 | 規約 |
|---|---|
| world | 右手系、+Y が上、メートル。glTF と同じ(今のまま) |
| object | glTF のまま。変換なしで読む(今のまま) |
| view | +X が右、+Y が上、**−Z が前方**。glTF の camera と同じ。up は transform の回転から取る(R·(0,1,0))。project の設定の `up` は無くす |
| clip・NDC | Vulkan のまま(+y が下、z は 0..1)。**world から clip への変換点は射影の行列だけ**(射影が y を反転する) |
| 画素・uv・画像 | 左上が原点、y 下(今のまま)。NDC と uv は同じ向き(`uv = ndc * 0.5 + 0.5`) |
| 面の表 | 反時計回り(glTF と同じ)。鏡像になる view(cube の面、§3.3)だけ時計回り |

規則は 1 つ: **画面の側(clip・画素・uv・画像・2D)は全部 y 下、world の側は全部 y 上。両者をつなぐのは camera の射影だけ。**

## 3. 形

### 3.1 共通の関数(`src/core/renderer/clipspace.hpp`。新設)

- `vulkanPerspective(yfov, aspect, znear, zfar)` / `vulkanOrtho(...)` / `vulkanAsymmetric(...)`: 右手系・depth 0..1・**y を反転**(`[1][1]` が負)
- `viewFromWorld(position, rotation)`: transform の逆行列。前方は −Z、up は R·(0,1,0)
- `cameraBasis(rotation)`: forward = R·(0,0,−1)、up = R·(0,1,0)、right = cross(forward, up) = R·(1,0,0)
- `viewIsMirrored(projection, view)`: view が world の鏡像かを返す(面の表の選択に使う)
- §1.3 の表の flat camera・editor preview・XR・spot・directional を、この関数に寄せる。cube だけは「cube の面の規約」の行列を明示して使う(共通の関数を通さない)
- **y の反転は、jitter を掛ける前の射影の中で行う**(TAA の jitter・velocity・再投影は NDC と画素の対応に依るので、この順なら変更不要)
- 射影の成分を直接読む code は符号に依らない形にする(視野角は `abs([1][1])` など。`clustered_light_select.comp` は既にそうなっている)

### 3.2 camera

- 前方 −Z、up は回転から。`camerasystem.cpp`、`camera.cpp`(既定の dir、`setActiveCamera`、最初の scene camera)、`launchcameraoverride.cpp`、`camerabake.cpp`(transform_seq の rot)、`editorpreviewprojection.cpp`
- 操作系(`cameracontrollersystem.cpp`): world の up は +Y 固定。right = cross(forward, up)。yaw / pitch の符号、fly の右移動、free camera の縦の orbit と縦の pan を、画面に対して今と同じ操作感になるよう直す
- project の設定の `up` と、controller の `up` の鍵は撤去する(移行で消す。§4)
- glTF の camera の取り込み(`gltfsceneextract.cpp`)は変換なしのまま正しくなる

### 3.3 面の表

- material と velocity は反時計回りを表にする
- **cube への描画は、Vulkan の cube の面の規約上、鏡像の絵になる。** material の pipeline は view の種類で面の表を切り替える(`VK_DYNAMIC_STATE_FRONT_FACE` を使うか、pipeline を 2 種類持つ。実装で小さい方を採り、理由を記録する)。切り替えの判定は `viewIsMirrored`
- 影(directional / spot)は、射影を共通の関数に寄せて反時計回りにする。寄せて acne や裏面の扱いが変わる場合は、実測して決める(据え置くなら時計回りのまま、鏡像の view として扱う)
- `gl_FrontFacing` を読む shader(`openpbr_lighting.glsl`)と、接線の向きの `sign(determinant)` は、直した後に実測で確かめる
- raster の pass の既定(反時計回り・cull なし)は変えない

**結果(CC-1 `c8367b87`)**: `VK_DYNAMIC_STATE_FRONT_FACE` を採った(Vulkan 1.3 の core で、pipeline の数が増えない)。material・velocity・shadow depth の pipeline は `GraphicsPipelineDesc::dynamic_front_face` を立て、記録のときに `viewIsMirrored` で view ごとに選ぶ(`materialrender.cpp` の `setViewFrontFace`。bind の直後に設定する)。影(spot / directional)の射影は共通の関数(`vulkanPerspective` / `vulkanOrtho`)に寄せ、反時計回りの鏡像でない view になった。点光源の影と cube capture は cube の面の行列のまま(鏡像、時計回り)。`gl_FrontFacing` と接線の `sign(determinant)` の実測の記録は commit に無い(未確認)。

### 3.4 shader と描画で直す所

| 所 | 直し方 |
|---|---|
| `pt.rgen` の `ndc.y = -ndc.y` | 削除(反転していない射影への補正だった) |
| `spheres.rgen`(自前の固定 camera、+Z 向き) | −Z 向きにし、scene の配置を合わせる |
| `previewexecutor.cpp` の `1 - (ndc.y * .5 + .5)` | `ndc.y * .5 + .5` に |
| `spriterenderer.cpp`(corner の y+ が画像の下。今の上下反転への合わせ込み) | 直す。sprite_demo の y の意味を実測で確かめる |
| `modaltransform.cpp` の view 軸まわりの回転の向き | 画面上の向きを今と同じに |
| `golden_harness.cpp` の自前の射影 | 共通の関数に |

変えなくてよい所(NDC と uv が同じ向きのままなので): SSAO、TAA の resolve、lighting の影の参照、light の選択、cascade、surface の画面入力、gizmo の投影と drag、`rayThroughPixel`、frustum の判定、UI・debug text・debug draw。

### 3.5 利用者の raster の pass の option(CC-2)

- `render.raster`(と rendering 節の raster の pass)に `clip_y: "down" | "up"`(既定 `down` = engine と同じ)。`up` は、その pass の viewport の height を負にする。出力の画像の向きは変わらない
- ~~`up` のとき、その pass の面の表の判定も反転するので、engine が `front_face` を入れ替えて、作者が書いた `front_face` の意味(world で見た巻き方)を保つ~~ → **この案は誤りで、engine は `front_face` を入れ替えない**(CC-2 `1501685a`)。Vulkan は viewport を通した後の framebuffer の座標で面を判定するので、`front_face` と `cull` は `clip_y` がどちらでも**出来上がった画像の上で見た巻き方**を指す。engine は viewport だけを変える(`render_pass_dispatch.cpp` の `setClipYViewport`。描く直前に負の height を設定し、描いた後に戻す)。`up` では絵が上下に反転するので、同じ頂点データの表と裏も入れ替わる。raster の pass の clip は利用者の shader が書くもので、「world で見た巻き方」を engine は知らない
- GLSL の関数を `pelican_view.glsl`(か新しい include)に足す: `pelican_ndc_to_uv(vec2)`、`pelican_uv_to_ndc(vec2)`。manual と雛形の shader はこの関数を使う

**結果(CC-2 `1501685a`)**: `clip_y` は `RasterPassContract` の pass 直下の field(`src/project/rasterpass.{hpp,cpp}` の `RasterClipY`。`raster_state` は fullscreen と共有するので入れない)。正規形の JSON に常に出て、fingerprint に入る。未知の値は `raster pass '<名前>' field 'clip_y' has unknown value '<値>'`、graph では `graph.render_node_parameter_invalid@1`。fullscreen の pass に書くと `Pass '<名前>' type 'fullscreen' does not own field 'clip_y'`(`passfieldownership.cpp`)。catalog の `render.raster` に enum `clip_y`(既定 `down`)。関数は `pelican_view.glsl` に入った。「manual と雛形の shader はこの関数を使う」のうち雛形の shader の書き換えは CC-2 の commit に無い(未確認)。test は `rasterpass_test` と `headless_render_test` の `CC-2 …`(GPU。同じ vertex shader を `down` / `up` で描き、`cull: back` と `gl_FrontFacing` で表裏の入れ替わりを見る)

## 4. 文書の書き換え(版は上げない)

利用者の指示(2026-10-02):「v0 の前だから無駄すぎることはしない」。v0 の tag の前なので、**文書の版は上げず、移行の手順の登録もしない**。直した後の形を v0 の形とする。

- 古い project を黙って誤って開かないための手当ては 1 つ: `basic_config.camera.up` を撤去し、書いてあったら名前付きで断る(今までの project.json は `up` が必須だったので、古い project は必ずここで止まる)。message は変換の script を案内する
- **変換の script** `tools/convert_camera_convention.py`: project の directory を受け、`up` を消し、scene / prefab の camera の回転と controller の yaw / pitch、transform_seq の camera の rot を書き換える。利用者の手元の project(playground など)にも使える
- 書き換えるのは、`up` を書いている project の文書と、camera を持つ scene / prefab、camera を code で作る test だけ。camera を持たない文書は触らない

**変換の規則**: camera の前方は今の向きのまま(今の前方 = R·(0,0,+1))。up は world の +Y に揃える(今の設定の `up` が −Y でも +Y でも)。前方が ±Y にほぼ平行なときは、局所 Y 軸まわりに 180° 回す。

- 設定の `up` が −Y だった project(blank の雛形、example): 絵は**今の左右反転**になる
- +Y だった project(6 本): 今は上下逆さまに描かれている。変換後は**正立**する(今の上下反転)
- 古い回転のロールは今まで効いていなかった経路が多いので捨て、捨てたことを script が報告する
- controller を持つ camera の yaw / pitch は、同じ向きを指すように書き換える

出荷の project 側で手で直すもの: 鏡像や逆さまを打ち消すために置いた補正(vrm_xr_demo の主役の Z 軸 180°)、world の X を直接動かす game code の左右(example の `playercontrol.cpp`、sprite_demo の `flipbook_demo.cpp`、vrm_xr_demo の視線の yaw)。

**結果(CC-1 `c8367b87`)**: 撤去と断りは `project.camera_up_removed@1`(`src/project/projectformat.cpp` の envelope の検査と、`basicconfig.cpp` の `rejectRemovedCameraUp` の 2 か所。controller の `up` は codec の閉じた鍵の一覧から外して未知の鍵として断る)。変換の script は `tools/convert_camera_convention.py`(`--dry-run`、`--assume-up X,Y,Z`。prefab の camera は transform が無いので報告だけ、camera bake 以外の transform_seq も報告だけ。test `convert_camera_convention_unit`)。出荷の 7 project、`test/fixtures/project_corpus/v0/`、camera を持つ fixture と test を変換した。手で直したのは example の `playercontrol.cpp`(D で画面の右へ)、vrm_xr_demo の主役の補正の除去と視線の yaw、RT の shader(`pt.rgen` の `ndc.y` の反転の削除、`spheres.rgen` を −Z 向きに、`spheres.rchit` の光の向き)。**sprite_demo の game code(`flipbook_demo.cpp`)は直す必要が無かった**(commit に差分が無い)。scene に camera が無いときの既定の向きは +X から −Z に、blank の雛形の MainCamera は z = +3・回転なしになった。

## 5. 検査

| 何を | どう | 何を取り消したら落ちるか |
|---|---|---|
| 鏡像でないこと | 新しい test: 「F」の板と +X(赤)・+Y(緑)の印を、回転なしの camera((0,0,3)、−Z 向き)で写し、四象限の色と F の向きを検査(GPU)。CPU でも、画面の右半分の ray が camera の +X を向くこと | 射影の y の反転を外す / 面の表を戻す |
| camera の規約 | 回転から up が取られること(ロールで絵が回る)、glTF の camera を取り込んで同じ絵になること | CameraSystem の up を戻す |
| cube | cube に描いた結果を `samplerCube` で引いて、面と上下が正しいこと(点光源の影の既存の test と、非対称な 1 本) | cube の面の表を反時計回りにする |
| 変換 | 出荷の project 7 本と雛形で、変換後の絵が規則どおり(−Y は左右反転、+Y は上下反転)になること。script の単体 test。`up` を書いた project が名前付きで断られること | 規則の up を逆にする |
| golden | 3D に依存する 36〜42 件を作り直す。旧 golden を反転した絵と比べ、差が縁の画素だけであることを確かめてから置き換える(harness は camera を直接置くので、反転の向きは件ごとに確かめる) | — |
| TAA | jitter だけの fixture で velocity が 0 のまま | 反転を jitter の後に掛ける |
| XR | 左右の眼の像と面の表(今は鏡像でないのに時計回りで描いている疑い。実測) | — |

**結果(CC-1 `c8367b87`)**:

- 鏡像の検査は `coordinate_mirror_headless`(GPU。`test/coordinate_mirror_check.cpp` が project を書き、player が headless で描き、同じ program が画像を読む。camera は `front`(+Z から回転なし)/ `side`(camera と scene を Y 軸まわりに 90°)/ `roll`(front を Z 軸まわりに 90° ロール。+Y が画面の右、+X が下に出る)の 3 通り)。CPU は `camera_test` の CC-1 の case(回転なしの camera が −Z を向き鏡像でない、対称の `vulkanAsymmetric` が `vulkanPerspective` と一致、`cameraRotation` が `cameraBasis` の逆、cube の面は鏡像で flat の camera は鏡像でない、scene の camera の基底が回転から来て画面の右半分の ray が camera の +X を向く)
- golden は 27 件を作り直した(`test/golden/*/expected.png` と `inventory.json`)。統合時に各件を旧 golden と比べた内訳(commit の記録と統合側の報告による。比べた script は repo に無い): 比べた 52 件のうち同一 25、上下反転で完全一致 8、上下反転 + 丸めや縁(TAA の縁の行など)12、sprite の修正によるもの 6、その他 1。上下反転が多いのは、golden の harness の project が `up` を +Y にしていて(CC-1 で外した)、旧 golden が上下逆さまの絵だったため
- XR と planar の SSAO の旧 byte golden(`test/fixtures/wp338_ssao/*.r8`)は物体が写っていなかった(面の表が cull されていた。commit の記録)。作り直した。XR の実機での確認は未
- RGBA8 の hash(`wp73_rgba8_hashes.json`)と trace の fixture(`wp364`)、向きを数値で見る test(`gizmo_test`、`camera_test`、`projection_jitter_consumers.json`)も更新した
- 着地の gate の件数は commit に書かれておらず、この文書では確かめていない(未確認)

## 6. 段(WP の順。1 本の統合 branch で順に積み、develop へは一度に着地する)

| WP | 中身 | 結果 |
|---|---|---|
| **CC-1a** | `clipspace.hpp`、camera の規約(−Z、up は回転から)、操作系の符号、editor の preview、面の表(material / velocity / cube の切り替え / 影)、§3.4 の shader と描画、鏡像の検査(§5 の 1〜3 行目) | ✅ CC-1a / 1b / 1c を 1 本に squash して develop `c8367b87`(2026-10-03) |
| **CC-1b** | `up` の撤去と名前付きの断り、変換の script、雛形と `default_config.json`、出荷の project 7 本と test の文書の書き換え(camera を持つものだけ)、game code の左右、補正の除去、RT の shader | ✅ `c8367b87`(同上)。sprite_demo の game code は直す必要が無かった |
| **CC-1c** | golden の作り直しと確かめ、RGBA8 の hash と trace の fixture、向きを数値で見ている test(`gizmo_test`、`camera_test`、`projection_jitter_consumers.json`) | ✅ `c8367b87`(同上)。golden 27 件(§5 の結果) |
| **CC-2** | raster の pass の `clip_y` と、NDC と uv の変換の関数、雛形の shader | ✅ `1501685a`(2026-10-03)。`front_face` は入れ替えない(§3.5 の訂正)。雛形の shader の書き換えは commit に無い |
| docs | manual(2・3・4・6・10 章)、source-code-guide(5 章の +Z 前方の難所、10 章の viewport の節)、`design_camera_system.md` ほか §1 の記述 | ✅ manual 2・3・4・6・8・10・13 章、source-code-guide 5・6・8・9・10 章、`design_camera_system.md` / `design_project_format.md` / `design_project_format_web_profile.md` / `design_openxr.md` / `design_taa_jitter.md` / `design_2d_game_layer.md` / `design_pathtracing.md`、`v0_freeze.md`、台帳(branch `agent/cc-docs`) |
| Studio(GUI session) | viewport の左右に依る scenario 6〜8 本の期待値、Inspector の方位角 / 仰角と yaw / pitch の欄の確かめ | GUI session が持つ(この文書では未確認) |

着地は、GUI session と engine session が develop への着地を止めた状態で行う(両 session と合意済み)。外部の web viewer(別 repo)は同じ規約に追随する必要がある。

## 7. 残る論点

決着したもの(CC-1 `c8367b87`):

- ~~影の射影を共通の関数に寄せるか据え置くか(実測で決める)~~ → 寄せた(spot は `vulkanPerspective` と `viewFromBasis`、directional は `vulkanOrtho`。`lightcontainer.cpp` と `directionalshadowcascade.cpp` の重複した `vulkanOrtho` は消えた)。点光源の影は cube の面の行列のまま
- ~~`VK_DYNAMIC_STATE_FRONT_FACE` か pipeline 2 種類か~~ → `VK_DYNAMIC_STATE_FRONT_FACE`(§3.3 の結果)
- ~~移行でロールを保つ camera の扱い(今まで無視されていたロールが効くようになる)~~ → ロールは捨て、捨てたことを変換の script が `camera_roll_dropped` で報告する(§4 の規則のまま)

残るもの:

- v1 の `canvas.place`(branch `feature/v1-vector2d`)は右手系で実装済みなので、この修正が develop_v1 に入れば正しく読める。canvas の 3D の test の期待値は入った後に直す
- 外部の web viewer(別 repo)の追随(§6 の末尾)
