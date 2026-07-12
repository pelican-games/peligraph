# アニメーショングラフ業界機構サーベイと pelican v1 設計レビュー

日付: 2026-07-12  
対象: [`design_animation_graph.md`](../design_animation_graph.md) v1 ドラフト  
調査時点: 2026-07-12。実装提案ではなく、機構境界と形式凍結の設計助言である。

## 結論

判定は **「三層の責務分離には賛成、A1 の3関数と `pelican.anim_graph` を安定版 v1 として凍結することには反対」** である。

1. 「エンジン = 小さなポーズ機構語彙、評価ロジック = 特権なし標準ライブラリ、グラフ = 版付きデータ」という中核判断は pelican のユーザー空間方針に合っている。ozz-animation は、この責務分離が実用になる強い先行例である。
2. ただし ozz の実際の低レベル面は sampling / normal・partial・additive blend / local-to-model / interval event scan / root-motion track / IK まで持つ。現 A1 の `samplePose` / `blendPoses` / `setPalette` は、ozz 型設計の「最小閉包」にまだ達していない。
3. `blend1d` だけで v1 を名乗ること自体はよい。名称と説明を「最小 locomotion controller」と限定し、同一 state 内の共通位相、state clock、遷移順序、緊急割込みを定義することが条件である。2D blend、layer、montage、motion matching を v1 に積む必要はない。
4. 遷移条件を「1 parameter 比較」に限定する判断は妥当である。ゲームコードで `can_jump` のような派生 bool を作ればよい。一方、遷移完了まで一切割り込めない仕様は実用に耐えない。0.2 秒の idle→move fade 中に jump / hit / death を無視するのは、小規模ゲームでも入力遅延または破綻になる。
5. pelican は普通の小型ゲームエンジンより、retarget、表情・視線 curve、timeline/gameplay の所有権調停、外部 pose source を早く要する。理由は規模ではなく、VRM/VRMA・DCC 往復・カットシーンを第一級要件に置いたためである。

従って、増やすべきなのは高レベル graph node の数ではなく、**skeleton identity、書換可能 pose、区間時間、root motion、curve/attribute、event/marker crossing、pose source と最終 commit の契約**である。ここを先に安定させれば、state machine や montage はユーザー空間に置いたまま発展できる。

## 1. 調査範囲と規模格付け

pelican 側は [`design_animation_graph.md`](../design_animation_graph.md)、[`2026-07-12_wp38_report.md`](2026-07-12_wp38_report.md)、[`design_asset_format_policy.md`](../design_asset_format_policy.md)、[`design_project_dcc_houdini.md`](../design_project_dcc_houdini.md) の必読4文書に加え、[`design_game_logic_native.md`](../design_game_logic_native.md)、[`design_event_layer.md`](../design_event_layer.md)、[`design_camera_system.md`](../design_camera_system.md)、[`2026-07-12_wp89_report.md`](2026-07-12_wp89_report.md)、[`2026-07-12_wp90_report.md`](2026-07-12_wp90_report.md)、現行の [`skeletalanimation.hpp`](../../src/core/model/skeletalanimation.hpp) / [`skeletalanimation.cpp`](../../src/core/model/skeletalanimation.cpp) / [`animationsystem.cpp`](../../src/core/ecs/predefined/animationsystem.cpp) を確認した。

以下の規模は業界仕様に書かれた閾値ではなく、各機構が解く組合せ数・制作コストからの本レビューの格付けである。人数や売上ではなく、アニメーションの複雑さを表す。

| 等級 | 想定 |
|---|---|
| P0 | clip viewer、技術デモ、固定カット再生 |
| P1 | 小規模ゲーム／単一 avatar player。1体、数本〜数十本の clip、基本 locomotion |
| P2 | キャラクター中心の製品。戦闘・割込み action・複数 rig・地形適応・cutscene |
| P3 | 大量 mocap、large roster、高い接地品質、AAA 相当の data-rich production |

## 2. 業界機構の全体地図

| 機構 | 解く問題 | 通常の導入目安 | pelican での格付けと判断 |
|---|---|---:|---|
| **Blend tree / blend space** | speed、方向など連続量から複数 clip の重みを求め、組合せ爆発なしに locomotion を作る。Unity は 1D/複数の 2D/Direct、Godot は 1D/2D、Unreal は Blend Space を持つ。([Unity](https://docs.unity3d.com/6000.0/Documentation/Manual/class-BlendTree.html), [Godot](https://docs.godotengine.org/en/4.7/tutorials/animation/animation_tree.html), [Unreal](https://dev.epicgames.com/documentation/unreal-engine/blending-animations-in-unreal-engine)) | P1 | **今必要**。`blend1d` は速度 locomotion の縦切りとして十分。同じ絶対秒での sample は全 clip が同長・同位相という厳格な asset precondition なら成立するが、一般の異長 clip には共通 normalized phase または sync policy が要る。方向移動が必要になった時だけ 2D を追加する。 |
| **State machine** | idle / move / jump / fall のような離散 mode、state local time、遷移、再入を管理する。([Unity](https://docs.unity3d.com/6000.0/Documentation/Manual/AnimationStateMachines.html), [Unreal](https://dev.epicgames.com/documentation/en-us/unreal-engine/state-machines-in-unreal-engine), [Godot](https://docs.godotengine.org/en/4.7/classes/class_animationnodestatemachine.html)) | P1 | **今必要**。条件式言語は不要だが、clock/reset、優先順位、割込み、複数 true 時の tie-break は機構側の意味論である。 |
| **Layer + bone/track mask** | 下半身 locomotion を維持しながら上半身射撃、顔、片腕などを同時合成する。Unity は Override/Additive layer + AvatarMask、Unreal は per-bone blend、Godot は track filter を持つ。([Unity layers](https://docs.unity3d.com/6000.0/Documentation/Manual/AnimationLayers.html), [Unreal masks](https://dev.epicgames.com/documentation/unreal-engine/blend-masks-and-blend-profiles-in-unreal-engine), [Godot filters](https://docs.godotengine.org/en/4.7/tutorials/animation/animation_tree.html)) | P2。上半身 action がある小作なら P1 | graph node は後でよいが、**per-joint weight を表せる pose/blend 面は今予約**する。`layers` という top-level key の予約だけでは不足する。 |
| **Additive** | 基準 pose との差分として breathing、recoil、aim、lean、表情の微調整を重畳する。reference pose と local/model-space の定義が本体。([ozz additive](https://guillaumeblanc.github.io/ozz-animation/samples/additive/), [Unreal blend nodes](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-blueprint-blend-nodes-in-unreal-engine)) | P2。avatar の look/breath なら P1 | graph 対応は後でよい。通常 blend と同じ関数へ曖昧に押し込まず、mode、reference、mask を持つ版付き blend 記述か別 primitive が要る。 |
| **Sync group / sync marker** | 長さ・歩数・stride の違う walk/run 間で左右の接地位相を合わせ、foot slide や拍の反転を防ぐ。Unreal は leader/follower と同名 marker 間の相対位置を使い、marker がなければ length sync へ戻る。([Unreal Sync Groups](https://dev.epicgames.com/documentation/unreal-engine/animation-sync-groups-in-unreal-engine)) | 通常 P2。ただし walk/run blend を製品品質にする時点で P1 | **時間機構は今必要、named marker authoring は後でもよい**。v1 `blend1d` は少なくとも shared phase または上記 asset precondition を持つ。marker metadata は別 API として additive に追加できるが、現3関数だけを「十分な閉じた surface」として凍結する根拠はなくなる。 |
| **Root motion** | clip 内 root の区間移動を actor/collision movement へ渡し、攻撃、乗越え、正確な接触、cinematic motion を成立させる。最終 pose とは別の delta である。([Unreal](https://dev.epicgames.com/documentation/en-us/unreal-engine/root-motion-in-unreal-engine), [Godot](https://docs.godotengine.org/en/4.7/tutorials/animation/animation_tree.html), [ozz](https://guillaumeblanc.github.io/ozz-animation/samples/motion_playback/)) | P2。in-place locomotion だけなら不要 | 実適用は v2 でよいが、**区間 delta の出力と transform 所有権を今予約**する。Pose の root joint に暗黙に残すだけでは不可。 |
| **Animation events / notifies** | footstep、VFX、attachment、hit window 等を clip 時刻に同期する。現在値 sample ではなく previous→current 区間を走査しないと low fps、loop、seek で取りこぼす。([Unreal Notifies](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-notifies-in-unreal-engine), [ozz user channel](https://guillaumeblanc.github.io/ozz-animation/samples/user_channel/)) | P1 | **早期に必要**。E1 の次フレーム配送へ event candidate を安定順で積む。point event、duration begin/end、seek/loop/fade 中の policy を Pose と分離する。 |
| **Montage / one-shot action / slot** | 持続 mode の state machine と別に、attack、reload、hit、interaction を命令的に再生し、section、loop、abort、named insertion point、部分 body overlay を扱う。([Unreal Montage](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-montage-in-unreal-engine), [Godot OneShot](https://docs.godotengine.org/en/4.7/classes/class_animationnodeoneshot.html)) | P2 | engine asset として今は不要。標準ライブラリ／ユーザー評価器に置く。ただし **named pose slot、playback handle、interruptible source** を後付けできる出力合成面が要る。 |
| **Motion matching** | current pose と desired trajectory から database 内の最適 frame を検索し、手書き遷移を減らしつつ大量 motion の反応性を上げる。Unreal 本体では 5.4 から production-ready だが、mocap corpus と tuning/debug の制作費は残る。([Unreal Motion Matching](https://dev.epicgames.com/documentation/en-us/unreal-engine/motion-matching-in-unreal-engine), [UE 5.4](https://www.unrealengine.com/blog/unreal-engine-5-4-is-now-available), [GDC original](https://www.gdcvault.com/play/1022985)) | P3、常に任意 | v1/v2 graph node には入れない。将来 user-space evaluator が作れるよう、random sample、SkeletonView、model-space pose、pose history/velocity、root trajectory を公開する。 |
| **IK: foot placement / look-at** | sampled pose を環境・target に合わせ、足を地面へ置く、頭・眼・手を目標へ向ける。通常は sample/blend 後の pose modifier。([ozz IK](https://guillaumeblanc.github.io/ozz-animation/documentation/ik/), [Unreal skeletal controls](https://dev.epicgames.com/documentation/unreal-engine/animation-blueprint-skeletal-controls-in-unreal-engine)) | P2。avatar look-at は P1 | solver 自体は後でよいが、**階層、rest pose、local↔model、writable pose、post-process phase** は A1 に必要。VRM の gaze 要件により look-at は前倒し。 |
| **Physics blend / ragdoll** | keyframed pose と rigid-body simulation を bone/subtree 単位で混ぜ、倒れ・被弾・secondary motion を作る。([Unreal Physics Animation](https://dev.epicgames.com/documentation/unreal-engine/physics-driven-animation-in-unreal-engine)) | P3。ragdoll が gameplay の核なら P2 | runtime physics 未実装なので延期でよい。ただし pose modifier と per-joint blend の穴は共通。なお VRM SpringBone は ragdoll と別物で、VRM player では P1 相当である。([VRM execution order](https://github.com/vrm-c/vrm-specification/blob/master/specification/VRMC_vrm-1.0/README.md)) |
| **Retargeting** | source/target の骨数、名前、向き、rest pose、比率差を吸収し、motion を別 avatar へ再利用する。([Unreal IK Retargeting](https://dev.epicgames.com/documentation/en-us/unreal-engine/ik-rig-animation-retargeting-in-unreal-engine), [VRMA pose compatibility](https://github.com/vrm-c/vrm-specification/blob/master/specification/VRMC_vrm_animation-1.0/how_to_transform_human_pose.md)) | 通常 P2 | **pelican では P1/現在の設計課題**。VRMA と live stream が別 skeleton を前提にするため。同一 GLB 制限を保つ clip v1 と、将来 retarget 可能な Pose ABI は分ける。offline bake を既定、runtime を VRMA/live 用にする。 |

### リスト外だが pelican では欠かせない二領域

- **curve / morph / expression / attribute**: glTF animation は node TRS だけでなく morph `weights` を持つ。VRMA は optional な body、expression scalar、gaze channel を持ち得る。Unreal の次世代 UAF も評価値を Pose だけにせず Curves と Attributes を併置する。([glTF animation](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#animations), [VRMA](https://github.com/vrm-c/vrm-specification/blob/master/specification/VRMC_vrm_animation-1.0/README.md), [UAF FKeyframeState](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/UAFAnimGraph/FKeyframeState))
- **inertialization / transition profile**: v1 の linear crossfade だけでも開始はできるが、割込み時の現在の blended pose を新しい遷移元にする規則が必要である。後で inertialization を足せるよう、transition は「source state 名」ではなく実際の source pose/history を扱えるべきである。

## 3. 他エンジンの所有境界

| 系 | エンジン／ライブラリが所有 | ユーザーへ逃がすもの | pelican との関係 |
|---|---|---|---|
| **Unity Mecanim** | Animator、state machine、BlendTree、layer/mask、Humanoid retarget、root motion、event、評価 lifecycle | controller asset、parameter、transition 条件、StateMachineBehaviour | 高レベル graph evaluator は engine-owned。pelican より上の境界。([state machine](https://docs.unity3d.com/6000.0/Documentation/Manual/AnimationStateMachines.html), [retarget](https://docs.unity3d.com/6000.0/Documentation/Manual/Retargeting.html), [root motion](https://docs.unity3d.com/6000.0/Documentation/Manual/RootMotion.html), [event](https://docs.unity3d.com/6000.0/Documentation/Manual/AnimationEventsOnImportedClips.html)) |
| **Unity Playables** | PlayableGraph/Playable/Output の実体、評価規則、AnimationStream、Animator への適用 | graph topology、time/weight、Play/Stop/manual Evaluate の指示、custom PlayableBehaviour/IAnimationJob | 「高レベル controller を迂回し、低レベル面を公開」という思想は似るが、公開物は pose algebra ではなく engine-owned dataflow runtime。A1 と同型ではない。([Playables](https://docs.unity3d.com/6000.0/Documentation/Manual/Playables.html), [PlayableGraph](https://docs.unity3d.com/6000.0/Documentation/Manual/Playables-Graph.html)) |
| **Unreal AnimBP** | Skeleton/Pose、graph compiler/evaluator、state/BlendSpace、sync、notify、montage、root motion、IK/physics、retarget | AnimGraph/EventGraph、variables/rules、linked graph/layer、custom C++ node/Blueprint notify | 最も機能豊富だが、評価器は特権 engine framework 内。pelican の user-space evaluator 方針とは異なる。([Animation Blueprints](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-blueprints-in-unreal-engine)) |
| **Unreal AnimNext → UAF** | RigVM、functional dataflow、trait/interface、worker-thread evaluation、Pose/Curve/Attribute infrastructure、および Chooser/StateTree/PoseSearch 等の engine-owned companion plugin | user graph/data/assets、compiled C++ trait/plugin extension | 5.8 時点でも Experimental。安定 ABI の手本ではないが、「Pose 配列だけに縮退せず typed channel/interface を持つ」方向は有力。([Epic status](https://www.unrealengine.com/tech-blog/explore-the-updates-to-the-game-animation-sample-project-in-ue-5-7), [UAF](https://dev.epicgames.com/documentation/unreal-engine/API/PluginIndex/UAF)) |
| **Godot AnimationTree** | AnimationPlayer の track、AnimationMixer/Tree の評価・適用、built-in blend/state と root-motion side channel | graph resource、parameter、script からの travel、custom node extension (4.7 Experimental、raw Pose API ではない) | 小型 engine の現実的 baseline。ただし evaluator は engine-owned。現行でも OneShot、nested graph、priority、xfade curve、cycle sync、root delta がある。([AnimationTree](https://docs.godotengine.org/en/4.7/tutorials/animation/animation_tree.html), [extension](https://docs.godotengine.org/en/4.7/classes/class_animationnodeextension.html)) |
| **ozz-animation** | immutable runtime Skeleton/Animation、SamplingJob/Context、BlendingJob、LocalToModel、track triggering、IK 等の job | state machine、blend tree、clock policy、memory/output wiring、renderer | **責務分離は pelican A1 に最も近い**。公式も high-level blend tree ではないと明記する。ただし語彙面は A1 の3関数より広い。([Overview](https://guillaumeblanc.github.io/ozz-animation/documentation/), [runtime jobs](https://guillaumeblanc.github.io/ozz-animation/documentation/animation_runtime/)) |

### 3.1 Unity Playables は A1 と同型か

答えは **「狙いは部分的に同じ、所有境界は同じでない」** である。

Playables は AnimatorController を作らず単 clip を再生でき、ユーザーコードが runtime graph の生成・接続・破棄、weight/local time、Play/Stop/manual Evaluate を指示できる。これは「高レベルロジックを engine 固定 asset から解放する」という意味では A1 と同じである。一方、Playable/graph の実体と評価規則、AnimationStream、最終 Animator binding は Unity runtime 内にある。ユーザーが受け取るのは独立した `Pose` 値と sample/blend 関数ではなく、custom animation job も engine の stream lifecycle 内で動く。([Playable examples](https://docs.unity3d.com/6000.0/Documentation/Manual/Playables-Examples.html), [manual Evaluate](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Playables.PlayableGraph.Evaluate.html), [AnimationScriptPlayable](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Animations.AnimationScriptPlayable.html), [AnimationPlayableOutput](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Animations.AnimationPlayableOutput.html))

従って pelican の説明には「Playables と同じ」と書かず、**「Playables と同じく高レベル controller を迂回できるが、API の粒度は ozz の jobs に近い」** と書くのが正確である。

なお確認した現行 Unity 公式資料では Unreal 型の一般 named sync marker/group API は確認できず、BlendTree は足接地など対応点を normalized time 上で素材側整列する前提を説明する。これは「marker が必須」ではなく、shared phase または明示的な asset authoring contract が先に要ることの別例である。([Unity BlendTree](https://docs.unity3d.com/6000.0/Documentation/Manual/class-BlendTree.html))

### 3.2 Unreal と AnimNext/UAF の方向性

現行 AnimBP は graph/node runtime のほぼ全部を engine が持ち、ユーザーは graph と rule を作る。Montage、Sync Group、Notify、IK、Retarget、Motion Matching は同じ animation framework に統合され、必要な箇所で AnimGraph/Slot/side channel へ接続される。pelican がこの機能表を模倣すると、ユーザー空間方針を失い実装規模も破綻する。

一方、相談文の AnimNext は現行資料では UAF (Unreal Animation Framework) として追うべきである。5.6 でも公開ブランドは UAF で、plugin/module/path に `AnimNext` 名が残り、5.7 以降は内部 path/module も UAF へ再編された。Epic は将来の AnimBP 置換方向として説明しているが、5.8 でも Experimental である。UAF は graph、Control Rig、Pose Search、StateTree、Layering、Warping を別 plugin に分け、trait/interface と dataflow で構成する。さらに評価 state は Pose に加え Curve/Attribute を持ち、timeline、group sync、notify source、root-motion attribute も別 interface である。([5.6 UAF/AnimNext](https://dev.epicgames.com/documentation/unreal-engine/API/PluginIndex/AnimNext?application_version=5.6), [UAF plugin](https://dev.epicgames.com/documentation/unreal-engine/API/PluginIndex/UAF), [trait interfaces](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/UAFAnimGraph/ITraitInterface), [notify](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/UAFAnimGraph/INotifySource), [attribute](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/UAFAnimGraph/IAttributeProvider))

これは A1 を巨大 graph runtime にせよという根拠ではない。UAF 自体は Pose/Curve/Attribute を型付き state に束ねている。得るべき教訓は、**評価値を joint Pose 配列だけへ縮退させず、時間・event・attribute には型付き channel/interface を与える** ことである。

### 3.3 Godot から得る最低線

Godot は `AnimationPlayer` が clip/汎用 property track を持ち、`AnimationTree` がそれらを reactive に blend する。BlendTree、BlendSpace1D/2D、StateMachine、OneShot、TimeSeek/Scale、track filter は engine node/feature である。root motion は node ではなく、選択 root track の見た目を cancel し、delta/accumulator をユーザーへ返す side channel である。state transition には Immediate/Sync/AtEnd、priority、reset、xfade time/curve、condition/expression がある。BlendSpace には cycle length を揃える sync mode もある。

pelican が同じ機能数を v1 に持つ必要はない。しかし、小型 engine の baseline と比べても現案は **state clock、sync、priority、abortable OneShot と緊急割込み** が抜けている。Godot の OneShot abort や commandable transition/fading state は Unity 型の ordered interruption policy と同一ではないが、再生中の要求を一切受けない現案より広い。この差は「機能が少ない」より「後から意味論を足すと既存 graph の再生結果が変わる」ことが問題である。

### 3.4 ozz は A1 の先行例か

はい。ただし限定付きである。ozz は low-level、renderer/game-engine agnostic であり、高レベル blend tree を提供せず、データ編成と blend logic を利用者へ任せると明言する。この責務分離は pelican の方針を強く支持する。

同時に ozz は、sampling cache、bind pose fallback、per-joint partial blend、additive layer、local-to-model、区間 edge triggering、root-motion track、two-bone/look-at IK を語彙として持つ。つまり ozz の教訓は「graph を engine に入れなくてよい」であって、「sample/blend/submit の3関数だけで十分」ではない。

## 4. リアルタイム 3DCG player としての整理

### 4.1 Timeline と gameplay graph は競合物ではなく、時間所有者が異なる

Unity は AnimatorController を反応的 character logic、Timeline を複数 object/audio/signal を束ねる authored sequence とする。PlayableDirector が Timeline asset instance、clock (DSP/Game/Unscaled/Manual)、scene binding を持ち、AnimationTrack は Animator へ binding される IPlayableAsset なので、AnimatorController と同じ Animator output を明示的に調停できる。常に自動 blend されるという意味ではない。([Timeline](https://docs.unity3d.com/Packages/com.unity.timeline@1.8/manual/index.html), [PlayableDirector](https://docs.unity3d.com/Packages/com.unity.timeline@1.8/manual/playable-director.html), [AnimationTrack](https://docs.unity3d.com/Packages/com.unity.timeline@1.8/api/UnityEngine.Timeline.AnimationTrack.html), [SignalTrack](https://docs.unity3d.com/Packages/com.unity.timeline@1.8/api/UnityEngine.Timeline.SignalTrack.html)) Unreal も AnimBP を per-character reactive pose、Sequencer を scene-wide editorial time とし、安定した経路では Slot を介して gameplay pose と cinematic pose を blend できる。([Sequencer Animation Track](https://dev.epicgames.com/documentation/en-us/unreal-engine/cinematic-animation-track-in-unreal-engine), [AnimBP bridge](https://dev.epicgames.com/documentation/unreal-engine/blending-animation-blueprints-with-sequencer-in-unreal-engine)) 5.8 の ABP/Slot 不要な Sequencer Animation Mixer は UAF と接続する将来方向だが、まだ Experimental なので安定契約の根拠にはしない。([UE 5.8 release notes](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes))

Godot には Unity Timeline と同じ別 package 境界はなく、`AnimationPlayer` 自体が property/transform/Bezier/call-method/audio/sub-animation track を持つ authored player で、`AnimationTree` がその animation を reactive に mix する。([Godot AnimationPlayer](https://docs.godotengine.org/en/4.7/classes/class_animationplayer.html)) これは pelican が同じ実装形を採る根拠ではなく、「authored time」と「reactive mix」の責務は共通 sink で調停すべきという比較例である。

pelican でも本流は次である。

```text
clip / gameplay graph / timeline clip / live subject
                    ↓  retarget・time mapping
        pose-source arbiter / compositor
                    ↓
      additive・IK・constraint・spring/physics
                    ↓
         instance pose commit → palette
                    └→ root delta / curve / event candidate
```

- `SeqPlayer(transform_seq)` は object/world TRS の焼き込み track、AnimGraph は skeleton-local pose の reactive controller であり、本来は同時利用できる。前者が actor を移動し、後者が骨を動かすだけなら競合しない。
- 競合するのは同じ sink へ二者が書く時である。root motion と transform_seq、timeline skeletal clip と gameplay graph、live pose と graph が典型である。last-writer-wins にせず、object transform / skeletal pose / expression curve ごとに **単一 writer、override/blend weight、handoff** をフレーム境界で決める。
- 将来の timeline は `transform_seq` を肥大させるより、scene-wide director が typed track を束ねる形がよい。transform track は既存 SeqPlayer、skeletal track は glTF/VRMA clip、reactive character は graph parameter/state、event は E1 へ routing する。
- 現行 component の `clip XOR graph` は「簡易 skeletal player と graph」の排他としてはよいが、timeline/live/graph 全体の排他規則には使わない。将来 source slot または authority binding を別面にする。

現行 frame order は game systems の後に SeqPlayer が走る。ドラフトは AnimGraphSystem もユーザー system 後とする。root motion、movement collision、ground query を使う foot IK を足すと、この一列では順序が足りない。エンジンが所有すべき追加語彙はロジックではなく、**parameter snapshot → base pose と root-motion modifier の評価 → movement/root 解決 → world-dependent post-process (foot IK 等) → commit** のような named phase である。Motion Warping のように root delta 自体を modifier が変えるため、root consumer を全 post-process より常に先へ固定してはいけない。([Unreal Motion Warping](https://dev.epicgames.com/documentation/unreal-engine/motion-warping-in-unreal-engine)) 標準評価器は特権なしのまま、この phase contract に登録すればよい。

### 4.2 glTF animation と VRMA を受ける層

glTF 2.0 は node TRS と morph weight の keyframe 保存形式であり、再生順、loop、state、timeline mapping を定義しない。従って glTF animation は graph ではなく **clip source / asset decode 層**で受ける。`KHR_animation_pointer` のように camera/material/extension property まで target が広がっても、それらは typed timeline/property track へ routing し、skeletal Pose を太らせない。([glTF](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#animations), [KHR_animation_pointer](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_animation_pointer/README.md))

VRMA 1.0 は glTF core animation を用いる animation-only の別 glTF で、humanoid bone、expression、look-at と node の対応を optional extension data として持つ。VRM model 内への併載を想定した形式ではなく、推奨拡張子は `.vrma` である。従って [`design_asset_format_policy.md`](../design_asset_format_policy.md) の「VRM を glTF 拡張として受理」は VRMA 対応を自動的には意味しない。pelican の採用方針として runtime の閉じた表へ `.vrma` を「binary glTF alias + `VRMC_vrm_animation`」と明記する必要がある。([VRMA specification](https://github.com/vrm-c/vrm-specification/blob/master/specification/VRMC_vrm_animation-1.0/README.md), [VRM official guide](https://vrm.dev/vrma/))

さらに現行 [`SkeletalModelData`](../../src/core/model/skeletalanimation.hpp) は VRM の humanoid map、expression bind、lookAt を保持せず、renderer も expression の material color / texture transform を per-instance に適用する sink を持たない。従って現状の「VRM 受理」は、VRM extension を semantic に実行することではなく、GLB container として model を描画できる範囲である。VRMA より先に target `.vrm` の semantic decoder と application path が要る。([VRM expressions](https://github.com/vrm-c/vrm-specification/blob/master/specification/VRMC_vrm-1.0/expressions.md))

推奨 pipeline は次の四段である。

1. glTF/GLB/VRMA container decode と、named/unnamed animation addressing。
2. source AnimationRig と、存在する humanoid / expression / gaze の optional semantic channel 化。
3. target VRM bind 時の versioned retarget/application profile による決定的変換。
4. optional body Pose / expression curves / gaze を持つ typed AnimationSource として graph/timeline が利用。

VRMA retarget は骨名一致ではない。異なる T-pose rest rotation と optional bone を扱い、hips translation を身長比で補正する profile も選べる。公式非規範資料は source pose を一度 normalized local rotation へ写して target pose へ戻す例を示す。source Clip/Rig と retarget context には source identity が必須であり、retarget 後の Pose には target PoseLayout identity があればよい。

WP38 との具体的な不整合は四つある。

- 現行 [`animationsystem.cpp`](../../src/core/ecs/predefined/animationsystem.cpp) は clip と model が同一 GLB でなければ拒否する。animation-only VRMA はこの前提と正面衝突する。
- 現行 [`gltf.cpp`](../../src/core/model/gltf.cpp) は fragment 付き binary を `.glb` に限定し、animation fragment 候補を名前付き animation から作る。一方 VRMA は原則 first animation を読み、名前を必須にしない。`.vrma` alias だけでなく、K1/loader に `#animation/0` または default-first、unnamed/multiple animation の規則が要る。
- WP38 は CUBICSPLINE と一般 glTF morph `weights` channel を拒否する。CUBICSPLINE は import-tools で決定的に LINEAR resample できるが、morph は resample だけでは renderer/application 不在を解けない。また VRMA expression input は mapped node の `translation.x` を semantic weight として読み、target VRM の morph/material/UV bind へ適用する。一般 glTF morph 対応不足と VRMA semantic decoder 不足を混同しない。
- VRMA の body、expression、gaze はいずれも optional で joint Pose とは異なる channel である。body だけを sample して互換と呼ばない。VRM 本体仕様が定める humanoid→lookAt→expression→constraint→springBone の実行順も post-process phase に反映する。

なお VRMA の hips translation を自動で actor root motion と解釈してはいけない。VRM は model 移動には scene root を動かすことを期待し、VRMA は humanoid pose として hips translation を許す。root motion へ抽出するかは clip/import profile の明示 policy にする。

### 4.3 DCC pose streaming の将来穴

Unreal Live Link は接続を Source、個別 stream を Subject、静的 skeleton を static data、各時刻の bone/curve を frame data とし、Latest / Engine Time / Timecode の評価、buffer/interpolation、retarget を分離する。retarget 後の Live Link Pose は普通の AnimGraph pose となり、他の modifier/blend を通る。([Live Link](https://dev.epicgames.com/documentation/en-us/unreal-engine/live-link-in-unreal-engine), [Live Link data](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-live-link-data-in-unreal-engine))

pelican が開ける穴も `setPaletteFromNetwork` のような終端 API ではなく、sample/blend 前の **transport-independent AnimationSource** である。body channel がある source は通常の PoseSource として graph に入る。

- transport/source plugin は G2 のユーザー DLL または外部 tool 側。DLL unload 前に source unregister、受信 thread の quiesce/join、callback 失効、queue payload の engine-owned copy/release、generation mismatch 検出を完了する。
- engine vocabulary は SubjectId、revision 付き StaticAnimationRig、timestamp/sequence/discontinuity と optional body/curve/gaze/root channel を持つ AnimationFrame、small buffer、EngineTime への clock mapping、missing frame の hold/rest/error policy。
- retarget adapter の body 出力を普通の pose source とし、graph、mask、IK、VRM post-process を同じ経路で通す。curve/gaze は対応する typed sink へ同じ frame revision で渡す。
- 受信 thread は ECS/GPU を触らず buffer へ push し、animation phase がフレーム境界で pull する。
- live 自体は byte deterministic ではない。正本の raw source take は source rig、元 cadence/timecode、frame、discontinuity を保持し、再 retarget 可能にする。別成果物として EngineTime fixed tick で評価した deterministic bake に interpolation/retarget profile と source/target hash を記録し、replay 中は live source を止める。これは WP89 の「外部入力を同じ replay backend へ正規化する」思想と一致する。
- raw take と deterministic bake は将来 clip/timeline へ変換し、`pelican.import` manifest 付きで `imports/` に置く。Unity Live Capture も live source と Take/Timeline playback を分けている。([timecode](https://docs.unity.cn/Packages/com.unity.live-capture%404.0/manual/timecode-synchronization.html), [take recording](https://docs.unity.cn/Packages/com.unity.live-capture%404.0/manual/take-system-recording.html))

## 5. A1「ポーズ API 3関数」への敵対的批評

### 5.1 既存 WP38 のデータ形を正しく表せていない

ドラフトは `Pose = joint ローカル TRS 配列(skin の joint 順)` とする。しかし現行 [`SkeletalModelData`](../../src/core/model/skeletalanimation.hpp) は全 `nodes` の rest TRS と、その部分集合への `joint_nodes` mapping を別に持つ。現行 evaluator は全 node の rest pose を複製し、animation channel を任意 node へ適用し、全階層の world matrix を作った後、joint だけを palette へ抜く。

従って joint 配列だけの Pose へ凍結すると、次を失う。

- joint 間または joint の上にある non-joint node の transform。
- animation が skin joint 外の node を target し、その子 joint に影響する場合。
- glTF node order と skin joint order の明示 mapping。
- model scene root、retarget root、複数 skin の layout identity。

これは将来機能以前に、既存 WP38 の意味論を A1 が縮退させる問題である。Pose は少なくとも full animation hierarchy を表す `PoseLayoutId` に結び付き、palette 作成は別 `SkinBinding` の node→joint mapping を使うべきである。また現 `SkeletalModelData` は node 名を保存していないため、「骨集合不一致を名前入りエラー」にする実装根拠も A1 で追加が要る。

### 5.2 時点の Pose と区間の結果を混同している

`clip, t → Pose` は時点関数として正しい。しかし次は `[previous cursor, current cursor]` の関数である。

- loop を跨ぐ root-motion delta。
- 0個以上の event/marker crossing。
- reverse、seek、複数 loop、discontinuity。
- sync follower の time remap と leader 交代。

`samplePose` の戻り値へ後から root/event を足すと ABI が変わる。Pose 内へ入れても、Pose は「時点」、event/root は「区間」なので意味が壊れる。必要なのは、pose-at-time と cursor advance/interval query を別契約にし、後者が root delta、event candidate、marker phase、curve/attribute を版付き sideband として返せることだ。

### 5.3 `blendPoses` は full-body normal blend しか表せない

現在の scalar weight + normalized sum では以下を表せない。

- per-joint mask / blend profile。
- override と additive、additive reference pose。
- local-space と model/mesh-space additive。
- bind/rest pose fallback、欠落 channel の初期値。
- root-motion delta、curve/expression の blend policy。
- sync leader/follower と Notify 抑制。

さらに「N 本の rotation = shortest-path slerp」は数学的に未定義である。2 quaternion の slerp は定義できるが、N-way は順次 slerp の順序、基準 quaternion、sign canonicalization により結果が変わる。input order、zero/negative weight、sum tolerance、normalization algorithm、同一結果の丸めを規範化しないと byte determinism の土台にならない。

高レベル layer node を v1 に入れなくても、少なくとも writable PoseView と SkeletonView を公開し、user evaluator が per-joint operation を書ける必要がある。engine が canonical performance/math を保証するなら、通常・masked・additive を版付き blend layer 記述へ分離する。

### 5.4 `setPalette` は公開終端として早すぎる

関数名は GPU skin palette を公開抽象にしているが、将来必要なのは「character instance へ最終 animation frame を commit」することである。palette はその派生物にすぎない。

最終 commit では、少なくとも次が同じ frame revision に属する。

- local/model pose と GPU palette。
- socket/attachment や CPU query が見る model-space transform。
- morph/expression curve と gaze。
- root-motion sideband。
- event candidate の source/time/order。

公開名を palette に固定すると、後から expression、socket、CPU pose cache を別 API で同期させることになる。`setPalette` を renderer 内部 helper に残し、userpublic は instance pose/frame の commit とする方がよい。IK/live source の穴も commit 後ではなく、その前に置く。

### 5.5 所有・ABI・hot reload

`Pose` の毎フレーム allocation が未決なのに by-value return を先に凍結している。必要なのは engine/scratch arena が所有する buffer、caller-provided output、または opaque handle + stable view のいずれかであり、lifetime を evaluation frame と asset generation に結び付けることだ。

G2 は shared CRT により STL の DLL 境界解放を現実的にしているので、`std::vector` を跨ぐだけで直ちに不正とは言わない。([WP90 report](2026-07-12_wp90_report.md)) しかし [`gameLogicAbiVersion`](../../src/core/userpublic/gamelogic.hpp) が示す通り、Pose layout、allocator、alignment、GLM/STL 型を変えるたびに ABI bump と DLL 全再ビルドが必要になる。hot model reload で rig/skin layout が変わる場合も、古い Clip/Pose handle を generation mismatch として検出しなければならない。

版付き descriptor の `struct_size` 相当、opaque resource/scratch handle、明示 destroy/release、read/write view の要素型を先に決める方が、公開 API を additive に育てやすい。

### 5.6 決定性の約束範囲

WP38/WP89 は同一経路・同一 fixed step の再実行で byte 一致を確認しており、よい基盤である。一方、現在の記述から cross-CPU/ISA/compiler まで同じ byte を保証したとは読めない。`std::fmod`、quaternion normalization/slerp、N-way blend、unordered resource traversal、NaN、同値 weight の leader 選択は別途規範が要る。

Unity の manual `PlayableGraph.Evaluate(delta)` や Godot の manual `advance(delta)` は clock/scheduling control であり、bit determinism の保証ではない。また Godot の `deterministic` は欠落 track を RESET/initial 値として一貫して accumulate する blend semantics を指し、cross-CPU byte equality ではない。pelican の byte 決定性とは明確に区別する。([Unity update mode](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Playables.DirectorUpdateMode.html), [Godot AnimationMixer](https://docs.godotengine.org/en/4.7/classes/class_animationmixer.html))

最低限、次を明記する。

- 保証範囲が「同一 build/architecture」か「対応 platform 間」か。
- graph/resource の安定 iteration order と全 tie-break。
- parameter の NaN/Inf 拒否、float `==` の意味。
- marker/event の loop crossing 順、同時刻 event の stable source/ordinal 順。
- root-motion と curve の blend order。
- math 実装または golden を変える時の version/hash。
- 将来の motion matching では database/schema hash、feature quantization、同 cost の ordinal、index builder/search order。

### 5.7 A1 の最小安定語彙

具体的な C++ 署名より、次の概念を先に固定すべきである。

1. **AnimationRig / PoseLayout view**: generation 付き identity、parent、rest local TRS、name/semantic。skin を持たない VRMA/live source にも使う。
2. **SkinBinding view**: target rig node→palette joint mapping、inverse bind、mesh/skin identity。一つの rig と複数 skin を分離する。
3. **Pose buffer/view**: PoseLayout identity を持つ caller-writable local pose、明示 lifetime、rest 初期化。
4. **Clip metadata/cursor**: source rig、time range、wrap、channel kind、curve、marker/event annotation、sampling context。
5. **Point sample と interval advance の分離**: 前者は Pose、後者は root delta / crossings / phase / discontinuity を返す。
6. **Blend layer description**: pose、weight、per-joint weight、normal/additive、reference、side-channel policy。
7. **Local-to-model と final commit**: IK/retarget/user modifier が palette 前に pose を読書きでき、instance の全 animation output を一括適用する。

これは graph evaluator を engine に戻す提案ではない。ozz と同様、engine は決定的な jobs/data view、標準ライブラリは clock・state・weights・routing を所有する。

## 6. `pelican.anim_graph` v1 の敵対的評価

### 6.1 `blend1d` だけで v1 を名乗ってよいか

**条件付きでよい。** v1 は「すべての animation graph」を代表する必要はない。clip state + 1D locomotion + crossfade が end-to-end で dogfood され、独自 evaluator が同じ public pose API だけで書けることの方が重要である。

ただし次を満たさない現状のままでは、看板機能の walk/run 自体が未定義である。

- blend point 群は一つの normalized state phase を共有するのか、各 clip が独立秒を持つのか。
- duration の違う clip をどう time-scale するか。
- state enter/re-enter 時に phase を reset/continue するか。
- clip speed、start offset、negative/reverse、loop endpoint をどう扱うか。
- named sync marker がない時の fallback を length sync とするか。

named marker の authoring UI まで v1 に入れなくても、shared cycle phase と clip metadata の穴は必要である。

### 6.2 「1 parameter 比較のみ」は実用に耐えるか

**耐える。** `grounded && jump_pressed` を graph expression にせず、game code が `can_jump` を bool parameter として作るのは pelican 方針に合う。式言語、reflection、debugger を engine schema に持ち込まずに済む。

ただし evaluator 固有の時間・遷移情報は game code で代替できない。exit time、time remaining、destination offset、transition progress、現在 state への再入、global transition は、必要なら明示 parameter/query として渡す。また複数 outgoing transition が同 tick に true の時、**配列順または priority + 配列 index** のどちらで選ぶかを v1 に固定する。

bool を一回性 command として使う場合の reset/consume はユーザー責務だと明記する。将来 trigger type を加えるなら、保存状態と同 tick の複数 consumer を定義する必要がある。

### 6.3 「遷移中は割込みなし」は実用に耐えるか

**耐えない。** idle→move の 0.15 秒、move→idle の 0.2 秒でも、jump、hit、death、scene interaction をその間拒否する。複合条件を user-space へ逃がすこととは別問題で、これは evaluator の temporal policy である。Unity Mecanim も exit time/duration/offset と current/next state 由来の interruption、ordered interruption を別の transition 機構として持つ。([Unity transitions](https://docs.unity3d.com/6000.0/Documentation/Manual/class-Transition.html))

v1 の最小解は豪華な Unreal 型 interruption ではなく、次で足りる。

- transition に integer priority と、`never / higher-priority / always` 程度の interrupt policy。
- global/any-state transition、または user system からの明示 `force/request state` のどちらか。
- 同 priority は declaration index で決める。
- 割込み時の新 source は古い source state でなく、**その tick の現在 blended pose snapshot**。
- 0 duration の emergency cut は常に許せる。

このためにも Pose scratch/cached source が A1 に要る。

### 6.4 E1、root motion、frame order

animation event は evaluator が callback を即時実行せず、typed candidate を決定順で E1 に emit する。E1 は次フレーム配送なので、footstep/VFX には一貫する。hit 判定のように現在 tick の branch を変える用途は animation event に依存させず、game state/parameter を正とするか、明示的に一フレーム遅延を受け入れる。

root motion は pose commit と別に movement consumer へ渡す。SeqPlayer が object transform を所有する shot では ignore/extract-only、gameplay graph が所有する時は apply、handoff 中は blend など、source authority ごとの policy が必要である。

## 7. v2 追加で ABI/形式が壊れる具体的な地雷

schema は `version: 2` で意図的に非互換化できるため、以下は「不可能」という意味ではない。凍結した v1 asset の一括 converter、公開 DLL ABI bump、既存再生結果の変更を強いる箇所である。予約 key は名前の衝突を防ぐだけで、データ形と意味論までは予約しない。

| 地雷 | v2 で起こる破断 | 今予約するもの |
|---|---|---|
| Pose が skin joint 順だけ | non-joint hierarchy、animation-only rig、複数 skin、retarget root を入れると Pose layout 全変更 | full AnimationRig/PoseLayout と別 SkinBinding/node→joint mapping |
| Pose に skeleton identity がない | count が同じ別 rig を誤 blend。retarget の source/target を持てない | generation 付き layout identity と compatibility query |
| `samplePose` が by-value point sample | root delta、event、marker、curve を戻り値へ足すと ABI 変更 | caller output + point/interval API 分離 + versioned result |
| Pose の allocator/lifetime 未定 | pool/arena、parallel eval、DLL reload で所有規則が変わる | opaque scratch/buffer handle、frame/generation lifetime |
| scalar-only `blendPoses` | mask/additive/reference/curve/root policy 追加で signature 変更 | versioned layer descriptor または別 primitive |
| N-way quaternion rule 未定 | 実装順変更だけで golden/byte が変わる | input order、sign、normalization、fallback の規範 |
| `setPalette` が公開 terminal | expression、socket、CPU model pose、gaze を別 commit にして同期破綻 | instance animation-frame commit。palette は内部派生物 |
| ClipHandle が model と同一 GLB 前提 | animation-only VRMA、shared clip、runtime retarget で handle 意味変更 | source AnimationRig 付き独立 Clip resource と bind/retarget step |
| state が `clip XOR blend1d` の閉じた union | nested graph、slot、motion matching source を入れる時 states 全書換え | state の pose-source reference/typed node boundary、または v1→v2 converter 方針 |
| blend1d の clock/phase 未定 | sync 導入後に同じ asset の再生速度・接地位相が変わる | state clock、normalized phase、sync group/fallback metadata |
| transition order/priority 未定 | 複数 true の結果が parser/array traversal に依存 | declaration index、priority、global transition の明文化 |
| interruption source 未定 | v2 割込みで source-state pose に戻り pop、または cached pose が ABI に追加 | current blended pose/history を source にできる runtime state |
| `events` の置き場所だけ予約 | clip annotation、graph routing、duration event、fade filter のいずれかで schema 衝突 | clip metadata sidecar + interval event model + graph filter/routing |
| root motion policy がない | actor transform、collision、SeqPlayer と二重 writer | root track/delta、extract/apply/ignore、authority/handoff |
| component が `clip XOR graph` のみ | Timeline/live/graph の共存を component shape 変更で追加 | named AnimationSource/slot と per-sink authority binding |
| parameter namespace/trigger 未定 | nested graph、reusable subgraph、一回性 action で名前・consume 意味が衝突 | binding/namespace、bool lifetime、将来 trigger の consume contract |
| morph/expression/gaze の typed 出力がない | glTF weights/VRMA 対応時に戻り値と commit を破壊 | optional curve/attribute/gaze channels と VRM expression application sink |
| graph/model hot reload の handle 規則なし | old Pose/Clip が新 rig/skin layout を参照 | asset generation、stale-handle error、atomic rebind/reset |
| evaluator phase が「user system 後」だけ | root movement、physics query、IK、timeline の順序追加で loop を再設計 | named animation phases と deterministic registration point |
| byte determinism の platform 範囲未定 | SIMD/math/library 更新で golden と replay 互換が不明 | scope、math/version hash、stable traversal/tie-break |

## 8. 実装順への助言

現 A1/A2 の二段を、設計上は次の gate に分けるとよい。

1. **A0: public contract freeze gate** — Skeleton/Pose/Clip identity、ownership、point/interval、side channel、commit、phase、G2 ABI を小 fixture で確定する。
2. **A1: mechanism jobs** — current WP38 sampler を caller-owned Pose へ分離し、normal blend、local-to-model/commit、既存 clip component の互換経路を作る。graph はまだ作らない。
3. **A1.5: hostile fixtures** — non-joint ancestor、異なる同数 skeleton、loop crossing event/root、N-way quaternion order、hot reload stale handle、2体 parallel evaluation。
4. **A2: minimal controller** — clip state + blend1d + state machine。shared phase、priority、interrupt/force、status/reload を含める。
5. **後続** — clip annotation/events、mask/additive、VRMA retarget、timeline/live source の順。Motion Matching、ragdoll は基礎 API を消費する別評価器として遠くへ置く。

現在の三層方針を守るために、A1 を太らせ A2 を小さく保つのが正しい。逆に A1 を3関数に固定して A2 schema へ機能を積むと、標準評価器だけが特権的になり「ユーザーは evaluator ごと自作できる」という約束が空文化する。

## 9. 推奨する v2 改稿の骨子（10行）

1. 現案は public freeze 前の `experimental/v0` とし、三層の責務分離そのものは維持する。
2. Pose を PoseLayoutId 付きの engine-managed buffer + writable stable view にし、AnimationRig と SkinBinding を分離する。
3. point sample と cursor/interval advance を分け、root delta・event/marker crossing・curve/attribute を版付き sideband にする。
4. blend は normal/additive、reference pose、per-joint weight、rest fallback、side-channel policy を表せる記述にする。
5. Skeleton hierarchy/rest/name/semantic と local↔model を公開し、IK・retarget・motion matching を user-space で可能にする。
6. `setPalette` を instance animation-frame commit へ上げ、palette・socket・expression を同じ frame revision から派生させる。
7. graph v1 は clip/state/blend1d のまま、state clock/shared phase、transition priority/tie-break、interrupt/force だけを追加する。
8. graph・timeline・live を named AnimationSource/slot に統一し、transform/pose/curve 各 sink の単一 writer と handoff を定義する。
9. glTF/VRMA は asset→optional semantic channels→retarget/application で受け、body・expression・gaze と DCC provenance を保持する。
10. 決定性は fixed tick だけでなく math/order/loop crossing/live recording/version hash と保証 platform 範囲まで規範化する。
