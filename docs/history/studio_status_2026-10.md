# Studio と編集 RPC の実装状況(2026-10)

2026-10-06 に [第13章](../manual/13_editor.md) §13.12「実装状況」から移した表。
内容は当時の観測・進行中の判断であり、現在の作業指示や未完了一覧ではない。
本文は第13章にあったときのまま移したので、「§13.1」「§13.5」は第13章(Studio の操作は今は [第15章](../manual/15_studio.md))を指す。
現在の WP の状態は [第11章 §11.1](../manual/11_status.md) と [実装台帳](../implementation_plan.md) を参照する。

## 実装状況の表

凡例: ✅実装済み / 🚧実装中(部分的に使える)/ 📐設計のみ(未実装)。付いている `WPnnn` は実装した作業単位の番号です([第11章 §11.1](../manual/11_status.md))。
2026-09 の作業単位は通し番号ではなく記号 ID(`N-1` / `P-1` など)なので、**記号とコミットの両方**を書きます。

| 機能 | 状態 |
|---|---|
| オーサリング文書・オブジェクト ID・revision | ✅WP149 |
| コンポーネント codec(受理・正規化・スキーマ公開) | ✅WP150/151 |
| 投影トランザクション(失敗時完全巻き戻し) | ✅WP152/158 |
| 読み取り RPC + スナップショット出力 | ✅WP154 |
| ウィンドウモード RPC ホスト | ✅WP156 |
| 編集 RPC + 履歴(journal) | ✅WP157 |
| undo / redo + チケットプレビュー | ✅WP161 |
| Asset Browser / Inspector(編集可) | ✅WP159/164/167 |
| 保存(アトミック全文書) | ✅WP166 |
| スナップショット取り込み | ✅WP168 |
| 変更検出トークン | ✅WP170 |
| 隔離プレビュー(評価) | ✅WP172 |
| 隔離プレビュー(描画) | 🚧WP172(契約は完成・**絵は模式図**) |
| チケットプレビューの対象拡大 | 🚧(transform / light のみ) |
| GUI と RPC の同居 | 📐未実装 |
| Qt Studio shell / native engine viewport | ✅WP249/251 |
| Qt Studio からの編集 RPC 利用(D2) | ✅WP266/275。Inspector と gizmo が同じ preview lease / undo / save を利用 |
| ID バッファピッキングの engine/RPC 基盤 | ✅WP262 |
| Qt Studio の viewport / Outliner 選択同期 | ✅WP264 |
| Qt Studio の Outliner の複製・削除・drag での付け替え(選択は authoring id で保つ) | ✅OL-1 `546a4e70` |
| 汎用 gizmo feature/RPC と Qt Studio のドラッグ操作 | ✅WP274/275 |
| 解決失敗を値で返す(`resolution_error`) | ✅WP378 |
| `not_editable` / `component_unknown@1` の切り分け | ✅WP379/379a |
| scene load の commit 後 effect 失敗を result で返す | ✅WP380b/380e |
| game DLL の startup / ロード前診断を status に出す | ✅WP381/381a |
| 可変長宣言(`List` / `$list`)の**行編集** | ✅RPCとQt Inspectorの追加・削除・移動・行内field編集 |
| `graph.instance` の装着と入力の供給元(Inspector の 4 導線) | ✅P-1 `3611e390` |
| feature パラメータの編集と materialize(`set_parameters` / `materialize`) | ✅P-2a-1c `e3a2d3d0` |
| authoring 面が graph root の `rendering` 節を読む / 共有部品の export・import | ✅P-2a-4a + P-2a-3 `7e983ed9` |
| package の install / uninstall(RPC と `pelican_cli package`) | ✅X-1 `64eae566`。**Studio に package の UI はありません**(カタログ経由で見えるだけ) |
| `get_status.animation` と `get_graph_node_values` | ✅AG-3a `32d9a20b` |
| 一時停止と 1 歩ずつ(`set_play_state` の `playing` / `paused`、`advance_frames`、`get_status.play`、窓ありの `step_frame` の拒否) | ✅PM-1a `c654e65c` |
| `edit` 状態(`--start-state edit`、眠った behavior、`set_play_state` の `edit`、`set_view_camera`、`get_status.play.view_camera`) | ✅PM-1b `17938ee3`。Stop で authoring を戻すのは ✅PM-2、Studio の Play / Stop は ✅PM-3b `9c169f61` |
| 再生のセッション(Stop で authoring・今の scene・editor の履歴を戻す、`keep`、`get_status.play` の `session` / `recorded_edits`、Play のたびの DLL の読み直し、`play_busy` / `play_session_active` / `play_keep_unknown_transaction`、`edit` 起動の input の記録と再生の拒否、Stop で音を全部止める) | ✅PM-2 `04666d59`(§13.5「再生のセッション」)。Studio の Play / Stop と keep のボタンは ✅PM-3b `9c169f61` |
| Studio の Pause / Step / Resume と再生の状態の表示(status bar の行、paused の枠と帯の色、`playing` の間は Timeline の scrub を止める) | ✅PM-3a `1eaf28a6`(§13.1「Studio で game を再生・停止する」) |
| Studio を edit で起動し、Play / Stop・edit からの Step・Play changes と Keep・Take from game・戻らない編集の帯・再生中の Save の無効・View: Game \| Editor・Reload game logic・再起動などの前の確認・playing の枠と帯の色 | ✅PM-3b `9c169f61`(§13.1「Studio で game を再生・停止する」) |
| 再生中に spawn された object を Outliner に `(runtime)` の行で出し、Inspector で読むだけの Transform を見せる | ✅PM-3c `e283aa42`(§13.1「Studio で game を再生・停止する」) |
| Studio の render-tools dock に Graph タブ(読む) | ✅N-1 `edafc41d` |
| ノードの見た目(見出し + 型付きソケット。形は JSON で足す) | ✅N-1b `6c47a8a4` |
| ノードキャンバスの編集(接続 / 切断 / 値 / 追加 / 削除 / ラベル / definition の中) | ✅N-2 `f2eccbfd` |
| Studio の自動化面(`--automation` + シナリオ + report。**実物の窓と engine を通す e2e**) | ✅Q-1 `c026002d` |
| Frame Plan のノードの見た目を共通グラフと揃える / 2 つのキャンバス共通のズーム・パン・fit | ✅N-3 `d8d7387d` |
| Studio のテーマ(View ▸ Theme のライト / ダーク。両キャンバスが追随、QSettings `studio/theme` に保存) | ✅N-5 `6a26eef3` |
| 時間軸 view・definition scope の値表示 | ✅AG-3b(`ce5e96c0`。出力 port の帯、Live、Timeline 帯と scrub。シナリオ `graph_observe_roundtrip`) |
| グラフ編集の undo / redo | ✅Graph タブのセッション内100操作。digest照合付き逆編集。scene journalとは独立 |
| Graph の線の吸着と強調、ゾーンと body の node の drag、分岐ごとの body、Auto layout / Fit view のボタン、画面に書くものを最後の列に、notice の Copy message | ✅UX-1 `7620fe1f`(drag の間の scene rect と、読んでいる image の二重の接続の拒否は `81b62382`) |
| Graph の `render.image` の欄(format / type と引数 / usage / 大きさ)、render pass の shader の欄、値の帯の一覧 | ✅UX-2 `5f048297` |
| Graph の palette の render node の種、Place の欄、入力ソケットの設定、Rename の提案、`depth_compare`、EL-1 の一覧(tag・material・variant・region の provider) | ✅UX-3 `f34adcc7`(シナリオ `graph_palette_kinds`) |
| Pass Editor の JSON の欄をフォームの部品に(`passforms/`)、skinned の stem の一覧、Ray scene の combo | ✅UX-4a `ad1e0771`(Graph への埋め込みは ✅UX-11) |
| render の編集の完了を 1 か所で読み、どのパネルでも `restart_required` で起動し直す / Rendering settings タブ(`edit_rendering_keys`) | ✅RSX `6a1069c8`(シナリオ `rendering_settings_from_blank`) |
| Blank から GUI だけで描画を組むシナリオ 5 本 | ✅GB-1 `3c6442a9`(A・B は最後まで、C・D・E は止まる所まで) |
| Blank から GUI だけで全部(deferred・影・RT の mask・bloom のループ・tone map)を組むシナリオと、今の GUI でできる最適化を測るシナリオ | ✅GB-2 `ed615734`(`gui_build_full` / `gui_build_optimize`) |
| Graph の pass の欄の Inputs(Filter / Address / mip)、New… の名前の提案し直し、Graph タブの pass の shader の監視、Studio が作る image を float・`TRANSFER_SRC` 無しに、Physical plan の Remove TRANSFER_SRC、Deferred lighting の雛形の `shadow_mask` | ✅UX-6a `1d8644fc` |
| Graph で root の node を fold group にまとめ、決定を一覧から選び、Verify で効かせる / Frame Plan の行から graph 全体の Serial・Isolate・No alias | ✅GL-3a / GL-4a `f5579426`(シナリオ `graph_fold_decisions`。pins と physical fragment の UI は無し) |
| Blank から GUI だけで FFT の海と LTC の面光源を組むシナリオ | ✅GB-3 `ba3b8a72`(`gui_build_ocean` / `gui_build_ltc`) |
| 外のエディタで書き換えた render の文書を見張って `reload_render_documents` で取り込む、Graph の Reload from disk、読み直しで履歴を切る | ✅RL-1s `e58c8da9`(シナリオ `render_reload_external`) |
| 番号付きの入力の Filter / Address / mip と fullscreen の新しい入力の `clamp_to_edge`、pass の Defines、raster の Depth / Raster / Draw(Grid)、palette の必須の parameter のダイアログ、image の history、開いた editor の下での描き直し、Play の間の `quit`、`get_status` の parse を 1 回 | ✅UX-7 `1fcd155e` |
| Frame Plan と Graph の GPU 時間・plan の数の 1 行、node の帯の GPU 時間、編集の前後の 1 行、`output unread` / `binding N unused` | ✅UX-6b `b1ed3356`(シナリオ `gui_build_optimize`) |
| Blank から GUI だけで音楽を鳴らすシナリオ | ✅GB-4 `e056fa25`(`gui_build_music`。曲線から shader への道は UX-8) |
| ループの body の雛形に `pelican_loop.glsl`、`reads element` の帯と pass の欄の 1 行、`shader.loop_push_overlap@1` の 1 文、海の FFT を 1 本の body に | ✅LC-1s `ed4ed5f9`(シナリオ `gui_build_ocean`) |
| `render.buffer` の value のソケット、pass の Buffers と名前付きの buffer の port、palette の初期値(曲線の ramp と時計、volume / pitch の 1)と catalog の必須を読むダイアログ、`value.curve` の Keys の表・小さな曲線・Timeline の点、`value.playback` / `render.scalar` の欄、root の Timeline、`audio.volume` / `audio.pitch` を voice につないで置く、音声の Import、一緒に来る node を重ねない、New definition…、log2 のボタンと image の層・mip の式、RS-15 / CT-1 の拒否の 1 文 | ✅UX-8 `cabf7691`(シナリオ `gui_build_curve` / `gui_build_music`) |
| palette の `render.compute` で project の `.comp` と RT の組を置き、`get_shader_interface` から ports・image・線、compute の欄の Shader と reflection error、shader の見張りと Update ports | ✅EL-5s `708c667d`(シナリオ `gui_build_ocean_compute`) |
| 保存中の palette、変わらない帯の値を送らない、確定前の値を手放す前に送る、palette と必須のダイアログを `open()` に、`audio.playback` の帯の voice の状態、起動時の致命の 1 文、definition の body だけ fit し直す | ✅UX-9 `4d11aa96`(シナリオ `gui_build_music` / `gui_build_curve`) |
| pass / task の Schedule(Once で書く image を同じ編集で Persistent に)と Run the body once、image の Lifetime、once の帯と `persistent` の帯、Recompute(`rerun_once`)と Recompute once、once の 2 つの拒否と `node_unknown` の 1 文 | ✅PS-1s `349af5e4`(シナリオ `gui_build_ocean` / `gui_build_ocean_compute`) |
| image の Initial contents・Size from the image・Import…(`.hdr` は `pelican_cli image ktx2` で焼く)、initial の 3 つの拒否の 1 文、Frame Plan の Resources の initial、initial の asset の見張りと `changed_assets`、拒否と起動時の致命が CT-2 の `document` / `definition` を読む、シナリオの prepare の口と `file` step の `copy` | ✅IA-1s `e2fd6dad`(シナリオ `gui_build_ltc`) |
| Graph の material pass の欄に Pass Editor の `MaterialSettingsForm` / `AttachmentOpsForm`、Tags を engine の `material_tags` から選ぶ(Graph と Pass Editor)、Materials drawn(近似)から Frame Plan の Materials へ | ✅UX-11 `0ac261f4`(シナリオ `graph_from_blank`。`screen_inputs` / `surface_resources` の名前は固定の表のまま —— ✅UX-12c で Graph の欄は engine の一覧に) |
| Materials drawn を engine の `drawn_materials` から(無い間は近似と言う)、Frame Plan の material pass の描いた数と Materials のタブの Drawn の列、binding の名前を `material_pass_contracts` の一覧から、Reads を semantic type で絞る | ✅UX-12c `708c4055`(engine の MP-1。シナリオ `gui_build_D`) |
| Graph の node の欄を catalog の `parameters` の表から(kind ごとの部品・必須の印・Reset・More parameters)、必須のダイアログを表から(Studio の表を外す)、値の帯は表の kind を先に、`graph.render_node_parameter_invalid@1` の 1 文と赤い行、`pose.clip_sample` の欄を汎用の欄の上に | ✅UX-12a `ac7c5250`(engine の CT-3。シナリオ `gui_node_parameters`) |
| feature の `required_targets` / `insertion` を足す前に読む(Add with targets・Place … in the Graph)、`graph.target_ref_type_mismatch@1` の Fix、Snapshots の after を `snapshot_after_candidates` から、`snapshot.after_unknown@1` と `material_route_unserved@1` の 1 文、Frame Plan の `Loop <loop>: its copies are picked.` が上書きされない | ✅UX-12b `99f645fb`(engine の FR-1 / FR-2。シナリオ `gui_settings_choose` ほか 4 本の期待。着地時に最後まで通らなかった `sky_ambient` と `cascade_count` 3 は ✅UX-12d で通る) |
| `choices_from` の編集できる combo(一覧に無い値は `not listed`)、asset の欄を表の `extensions` と `asset_kind` で、`union` の形の combo、image の Mip levels の Count / Full chain、`soft_max` までの slider と `above the usual range`、root の palette で `owned_instance` の op を灰色にして New definition… から definition の中の palette へ、`graph.op_requires_owned_instance@1` の 1 文 | ✅UX-12e `eeacce3e`(engine の CT-4。シナリオ `gui_node_parameters`) |
| 音の Bake analysis… のダイアログ(`bake_audio_analysis`、Baking / Baked の行、5 つの拒否の 1 文)、Use in Graph(`value.sample_table`)と必須のダイアログの焼いた表と行、Use in a shader(persistent の image・layout の buffer・2 つの `render.scalar` を 1 回の編集で)、`list_derived_assets` の stale の帯と Re-bake | ✅AB-1s `149c0ec8`(engine の AB-1。シナリオ `gui_build_audio_reactive`。Re-bake の後の同じ値の `set_node_parameters` は UX-13a で外した) |
| `needs_writer` を見て書き手を問いに入れる、Add with targets と cascade の Fix を 1 本の `edit_batch` に(取り消しの編集を外す、`batch_index` の段の文)、`feature.required_target_unwritten@1` の Write … from …、Connect <target> to present、`edit_batch` の Undo | ✅UX-12d `6c7dc8bb`(engine の FR-3。シナリオ `gui_settings_choose` で `sky_ambient` と `cascade_count` 3 が最後まで通る) |
| pass の buffer の port の binding を engine の規則(読む buffer の名前の順、`buffer_binding_base` から)で数える(ソケット・Buffers の欄・Use in a shader)、雛形と写す 1 行を `pelican_resource_ports.glsl` の形に、Re-bake の後の編集を外して表を見張りに入れる | ✅UX-13a `0e0f4481`(engine の IA-3。シナリオ `gui_build_curve` / `gui_build_audio_reactive`) |
| `choices_from` の `where`・`get_graph_catalog`・`asset_manifest`、`format_class` の Explicit…、`render.scalar` の書き先を `(will be set to runtime_f32)` で選んで同じ編集で整える、`graph.render_scalar_target_incompatible@1` の 1 文と Fix、definition の中の palette の灰色(`definition_instances`)、`value.sample_table` の必須のダイアログを汎用の欄に、`edit_graph_instances` の名前付きの拒否の 1 文 | ✅UX-13b `342142d5`(engine の CT-5。シナリオ `gui_node_parameters`) |
| Outliner と Inspector が `scene_tree` の代わりに `scene_outline` を読む(revision 付きの poll と `not_modified`、親を id で、`-32601` の engine は `scene_tree`) | ✅UX-13c `4405641a`(engine の `scene_outline` `b29ccfea`) |
| Connect <target> to present を 1 本の `edit_batch` に(Undo 1 回、断られたら段の名前)、繋ぎ替えで target_ref の型を書き換えるのは legacy の型のときだけ | ✅UX-13d `b289d706`(engine の FR-4。シナリオ `gui_settings_choose`) |
| light の方位角を camera の yaw と同じ基準に(+Z が 0、−X の側へ)、方位角・仰角・yaw・pitch の欄の tooltip、`output_locked` の 1 文と Bake again、CC-1 の後の viewport の期待(海・LTC・選ぶ設定) | ✅UX-14 `aaba15a6`(engine の CC-1 / AB-2。`gui_settings_choose` は CC-1c の後に `a954d38c` で直した) |
| Assets のドック(View > Assets、行ごとの Reload when saved outside とドックの Undo、監視の 1 行、Track asset contents…、照合の印と一覧、Verify again / Update manifest の確認の 1 行、`edit_project_settings` と AV-1 の拒否の 1 文)、監視が開いているときの `reload_shaders` の `committed: false` を読み直しとして扱う | ✅HW-2s / AV-1s `78737fdf`(engine の HW-1 / AV-1 / PJ-1。シナリオ `gui_assets_watch_verify`) |
| Frame Plan の Backends の Pin this candidate / Unpin(`edit_vulkan_plan_pins`)、Physical plan の Freeze / Remove / Eject again(`put_vulkan_physical_fragment`)と `physical_fragment_stale` の 1 文、常駐 BLAS の節、置き換えの来歴(Replaced by と Compile rewrites)、Layers の `is replaced by` の穴の直し | ✅UX-15b `5c5110ef`(engine の EL-2。シナリオ `gui_lower_layer_pin_freeze`) |
| Rendering settings の render_strategy の name と parameters、draw_sort の XR の並べ方(Per view)、graph_transforms の行、XR の Measured gate と minimum gain、preset の値の灰色の行、material の `gpu_draw_source` の欄(Other (JSON) を外す) | ✅UX-15a `06a9cd2d`(シナリオ `rendering_settings_from_blank` を延長) |
| rendering 節の pass を Graph の詳細欄で Pass Editor と同じ部品・同じ `presetPassEditParams()` で直す、詳細欄の Serial / Isolate / No alias、畳んだ node の tooltip を出どころごとに | ✅GL-5a `6565221b`(シナリオ `gui_folded_pass_edit`。task の欄は未着手。GL-5c は着地時は未着手で、✅GL-5c で着地(下の行)。GL-5b は下の行) |
| `render.compute` の `ray_tracing` を汎用の欄に(stage ごとの shader の候補、union の Shader / List)、`dispatch` の `groups_from` / `rays_from` の port を task の image port の combo に、compute の欄と palette の New… で RT の雛形(raygen / miss / closest hit)、Task Editor の New ray tracing も同じ雛形、RT の拒否 16 種の 1 文 | ✅CT-6s `827eddd7`(engine の CT-6。シナリオ `gui_build_rt_from_blank`。mode の変更は再起動の道に乗らず拒否の 1 文まで) |
| Materials のドック(View > Materials、`list_materials` の一覧と見えている間の poll、surface の宣言から作る欄 —— slider / spin / 色の swatch(linear は sRGB で見せる)/ check / テクスチャの combo、default と違う値の太字と Reset to default、Tags / Alpha mode / Surface と `restart_required` の再起動、`list_surfaces` から New material…、Assign to selected object、ドックの Undo、`material.*` と `external_modification` の 1 文)、Inspector の Material の行(model 単位の 1 行と使う object の数、primitive ごとの行、自分の Undo)、screenshot の `rgb` | ✅MT-1s `2d81ef51`(engine の MT-1 / MT-1b。シナリオ `gui_material_from_blank`。object ごとの割り当ては下の MT-2s の行) |
| New render target を Frame Plan と Graph で 1 つの関数に(`presentNewRenderTarget()` / `proposedRenderTargetName()`)、palette の `render.target_ref` の combo の New render target…、詳細欄の color 出力の行(宣言済みの target から選び直す・`New target: <pass>_color…`)、詳細欄の編集が断られたら root から読み直す、Add の入口と書き先の表 | ✅GL-5b `60ff3a2c`(シナリオ `gui_graph_new_target`。Add は束ねていない —— GL-5b の着地時は寄せ方は GL-6 の判断で、GL-5c は未着手だった。graph root の Add は ✅GL-5c で palette に寄せた(下の行)) |
| Inspector の Material の行を object ごとに: 範囲の切り替え(This object が既定で `set_object_material`、Every <model> は `bind_material`)、`get_object_materials` から描いている material と `(this object)` / `(model default: …)`、Use model default、primitive の行も範囲に従う、`objects` / `object_overrides` の「N objects, M with their own material」を revision ごとに読み直す、object の割り当ては scene の Undo、Material の行の Undo は送った model へ、`material.object_unknown@1` の 1 文と missing の印、Inspector が simplemodelview の list 型の欄で止まっていた件の直し | ✅MT-2s `7112072e`(engine の MT-2。シナリオ `gui_material_from_blank` を延ばした。ドックの Assign to selected object は model 単位のまま。基本形の Model default / Undo は着地時に engine が断った —— engine の MT-2b `213562bb` の後、✅MT-2s-b でシナリオが通す) |
| 基本形の Every <model> の Model default と Material の行の Undo をシナリオで通す、`material.object_unknown@1` を engine の `name` で言う、`changed: false` の答えで行が止まらない | ✅MT-2s-b `9d15ddc0`(engine の MT-2b。シナリオ `gui_material_from_blank` の `material_2` の代わりの手順を置き換えた) |
| graph root(preset を選ばない root)で Frame Plan の Add material pass… / Add fullscreen pass… / Add compute task… を Graph の palette と同じ道(graph の `nodes`)に、Edit pass / Edit task と行のダブルクリックを Graph が編集する pass なら Graph の詳細欄に、Graph の New render target… の既定の種類を material pass の slot の表から(forward の color は Explicit `R16G16B16A16_SFLOAT`)、委譲した入口の tooltip「Opens in the Graph」、View > Pass Editor、Frame Plan にしか無い操作の表 | ✅GL-5c `9cd89c5c`(利用者の決定 2026-10-04。シナリオ `gui_frameplan_delegates` と、期待を変えた既存の 15 本。preset root は form のまま。graph root で deferred の material pass を palette で置くと engine が `material_physical_contract_changed` で起こし直す件は engine の MC-1 で調査中。→ ✅MC-1s で直った) |
| Graph の palette の `render.material_pass` が作る image の format を Frame Plan の form と同じ表 `materialPassSlots(contract, hdr)` から(表と `materialPassHdr()` を model の `frameplanmodel` に移した)。`hdr` の無い Blank では albedo / emissive が `B8G8R8A8_SRGB` で、deferred の pass を置いても engine が起こし直さない | ✅MC-1s `1d10bd4c`(engine の MC-1。シナリオ `input_needs_shader`・`deferred_loop_from_scratch`・`frameplan_layout_then_edit` を再起動しない流れに戻した。契約が本当に変わる場合の再起動は v0 では今のまま、その場の組み直しは v0 の後 —— 2026-10-04 利用者の判断) |
| engine をいつも `--render-fallback` で起こす、起動時に組めない構成を viewport の上の帯と Frame Plan の status 行の 1 文に(Reload from disk / Open in Graph / Open file)、`render_document_syntax@1` の 1 文と Show the document(`restore_render_documents`)、module が frozen の拒否で Restart engine。Materials のドックの `rebuilt`(`Applied; rebuilt N models.`、再起動しない)、`rewritten_bindings` で Inspector の Material の行を読み直す、`material.routing_conflict@1` の 1 文、答えを待つ間 combo が戻らない | ✅BS-1s / MT-3s `19be8983`(engine の BS-1 / BS-1b / MT-3。シナリオ `gui_render_fallback` と `gui_material_from_blank` の tag の段。UX-9 の GPU test を fallback の形に書き換えた) |
| 自動化面の `list` が check できるボタンに `checked` を返す。シナリオ: Materials のドックの Surface を別の variant に替えて engine が補った routing が欄に出ることと Undo、MC-1s の後に赤だった `sockets_from_shader`・`sockets_fix_and_show` を再起動しない流れに | ✅SK-1 `67fd3d29`(engine の VR-1。シナリオ `gui_material_from_blank` の VR-1 の段。ドックのコードは変えていない) |
| シナリオの画素を読む点を、viewport の縦横比が変わっても箱の外に落ちる所へ(`gui_settings_choose` の影と床、`gui_build_C` の bloom の広がりを上下で) | ✅PX-1 `e3627606`(シナリオだけ。run が窓と viewport の大きさを固定しない根本は PX-2) |
| Problems のドック(View > Problems、Severity / Problem / Document / Line / Fix in の列、並べ替え、重さと文字の絞り込み)、engine の `get_problems` を `get_status.problems.revision` が動いたときと読み直しの答えの後に読む、Assets のドックの照合の warning / error の行、行の押下で直す場所へ(起動時の node は Graph、reload で断られた node と構文は Show the document、material は Materials、shader は外のエディタ、照合は Assets、route_unserved は Rendering settings)、status bar の件数、帯の ` See Problems.`、再起動の案内を `render_feature_requires_restart@1` と `fields.feature` で、reload で断られた文書の Show the document の Save はファイルを書く | ✅PB-1s `93934318`(engine の PB-1。シナリオ `gui_problems_window`。WP298 の GPU test は期待を書き換えたが engine の morph の上限で赤。→ reload で断られた node は Graph、Save は restore に、が ✅RR-1s) |
| シナリオの run は engine の viewport を 720x450 に固定する(自動化面の命令 `viewport_size`、driver が最初の step の前に送る、シナリオの最上位の `"viewport": [w, h] \| null`)、画素を読む 33 本の点を固定した大きさで測り直し、縁に近い点を選び直す | ✅PX-2 `28bc5ef9`(`whole_frame_first_steps` は `"viewport": null`。既定の大きさを変えると 33 本の点を全部測り直す) |
| reload に断られた版を engine が持つ間の帯(`live: false` の `Showing the saved version; …`、`held_unparsed` の `… is not valid JSON; the Graph shows the last version that was.`、fallback の帯が優先)、Show the document の Save をいつも restore に、断られた node は Graph で、断られた後に panel が保存した版の digest で読み直す、「could not be loaded」は `render_document_syntax@1` の間だけ | ✅RR-1s `471476d0`(engine の RR-1。シナリオ `gui_problems_window` を 79 step に延ばした) |
| Add Object > Model from file…(外の `.glb` / `.vrm` を `assets/models/` へコピーして `register_model`、名前が重なれば `_001`…、model の名前の object を置く、外の `.gltf` は断る)、Inspector の model 欄の Import…、`restart_required` の案内と Outliner の下の Restart engine、Assets のドックの Remove from project…(`unregister_model`、ファイルは残す、bindings が戻らないことを確認の文で)、`model.*@1` の拒否と未登録の model の edit の 1 文 | ✅MI-1s `70298f88`(engine の MI-1。シナリオ `gui_model_from_file`。VRM は一時的なシナリオで 1 回) |
| Graph の node の右クリックと詳細欄のボタンの列に、Frame Plan にしか無かった操作: rendering 節の pass の Order…(Frame Plan の Order... と同じ関数)、Fix sockets / Make sockets variadic(graph の node の pass は複製を `set_node_parameters` で node に載せる)、`render.target_ref` の Edit target… / Delete target(Frame Plan と同じ form と要求、`graph.target_ref_unknown@1` の 1 文)、畳んだ node の右クリックの Expand to edit…。隠れた Frame Plan は読み直してから動く(`whenRead()`)、Frame Plan の tooltip が Graph の同じ操作を言う | ✅GL-5d `f818f10a`(利用者の決定 2026-10-04。シナリオ `gui_graph_only_edits` と `gui_graph_order`。rendering 節の pass の入れ替えは engine が依存の循環として断る —— 以前からの engine の穴で FO-1 で直す予定。→ engine の ✅FO-1 `a4d7760a` で直り、依存と矛盾する並びは `frame_node_order.conflicts_dependency@1`。Studio 側は FO-1s。Shift ドラッグと Pass Editor の New pass は切った。graph root で Frame Plan にしか残らない操作は GL-5e で移す予定。→ GL-5e / GL-5f で移した) |
| Graph に、rendering 節の pass の View の欄、passthrough のつながった ○ のクリック(graph の node は `shader.defines` の `PELICAN_PASSTHROUGH_INPUT`)、managed fragment の pass の Delete pass(右クリック・詳細欄・Delete キー)、shader の契約と reload の拒否の Use engine://passthrough / Open the shader / Disconnect、Layers の行の右クリック(Pin / Unpin / Freeze / Eject again / Remove / Remove TRANSFER_SRC)、畳んだ node への線の drop(Frame Plan の Shift ドラッグと同じダイアログ。engine の node は読むだけの 1 行) | ✅GL-5e `74767e46`(シナリオ `gui_graph_only_edits_2` と `gui_graph_folded_edits`。契約・reload の拒否・Layers の右クリックは unit test だけ。Pass Editor の New pass は口だけで未配線) |
| Pass Editor の New pass を graph root では Graph の palette の `render.fullscreen` に(シナリオ 4 本を書き直し)、authored な compute task の Delete task と Edit task…(Frame Plan と同じ Task Editor)、隠れた Frame Plan が task の catalog も読み直す、loop の body の node の Layers に copy の行と Remove TRANSFER_SRC、何も選ばない Layers の見出しの右クリックに graph 全体の Unpin / Freeze / Eject again / Remove | ✅GL-5f `42715e8f`(シナリオ `gui_graph_task_delete`。loop の copy と見出しの右クリックは unit test だけ。残る 2 つ —— rendering 節の raster pass の shader と、rendering 節以外の pass の Serial / Isolate / No alias —— は GL-5g で進行中) |
| 順序のダイアログの拒否を `<second> must stay before <first>: <理由>` の 1 文と赤い 2 行に、起動時に断られた material 文書へ Problems から飛んで行・列の 1 文と Show the document で直す、material の Delete… / Rename…(`in_use` の 1 文と場所ごとのボタン、Undo)、`graph.image_key_unknown@1` / `graph.buffer_key_unknown@1` / `assets_verification_running@1` の 1 文 | ✅FO-1s `f186320f`(シナリオ `gui_graph_order` を広げ、`gui_material_repair` を足した。Rename と拒否の 1 文 3 種は unit test だけ。直した文書の取り込みは MR-1s で置き換え) |
| Rendering settings タブの Ray layout の節(Enable / Disable hit groups、Ray types・Hit groups・Miss shaders・Record fields の表で足す・消す・改名・並べ替え、Hit group assignments、Acceleration structures...、読めない layout の Show the document)、Graph の RT の task の節(Raygen、Traced ray types、miss の行、hit shader の格子、callable、raygen の 2 つの切り替え)、layout の形の雛形 1 組、RT の拒否の 1 文と Graph のセルの赤、Frame Plan の Hit groups (SBT) と Layers の L3、Inspector の Assign hit group... と Enable hit groups... | ✅RTL-2s a `16129867` / b + c `c8fd51cb`(シナリオ 5 本を足した。Callable records の開く道・record を外す・Engine record・procedural の record の Undo・拒否の実物・TLAS ごとの BLAS は未確認か unit test だけ) |
| 壊れた material を直して Save で戻す: Show the document の Save が `register_material` を送り `recovered` で窓を閉じて `<name> is loaded again.`、parser の拒否は行・列の 1 文で窓に、`material.path_conflict@1` は `list_materials` を読み直して決める、Register again、起動後に壊れた文書(source `hot_reload`)の行の印と watch の取り込みの待ち、Problems の material の行の変化でドックを読み直す、自動化面の `wait_engine` の `process_id` | ✅MR-1s `5c26d492`(`gui_material_repair` を再起動しない流れに書き直した。Register again のボタン・path_conflict の道・parser の拒否の窓は unit test だけ) |
| rendering 節の raster pass の詳細欄(Vertex / Fragment・Defines・Raster・Attachments を `edit_preset_pass` で)、root の graph の node と 1 pass の folded node の Serial / Isolate / No alias、Graph の notice に shader の compile の失敗、Graph の Undo / Redo を tab が送った順に(render edit と graph edit が混ざっても)、render edit の後も Graph の Redo を保つ、シナリオの常に通る確認 6 件と開き直した後の絵の比較 | ✅GL-5g `cfa9859f`(raster の欄は unit test だけ。Frame Plan にしか残らない編集は 3 件) |
| RT の Studio の仕上げ: SBT の表を `get_ray_table` で(Show more)、割り当ての行に `used_by`、procedural の record の Undo を engine の restore に、raygen の check を Undo に、traced の check で行を雛形で埋める・外す、拒否の 1 文とセルを fields から、Problems の `material.pass_contract_unmet@1` を pass へ、Materials の `unusable` の行と Open the pass | ✅RTL-3s `c80bc6a1`(シナリオ 2 本を足した。Show more・除外の表示・Call callable 0 の Undo は未確認、割り当ての行の名前と preset / feature の pass への行き先は unit test だけ) |
