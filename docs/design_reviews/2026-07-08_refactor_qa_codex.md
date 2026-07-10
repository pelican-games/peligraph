# リファクタリング監査フォローアップ

調査対象は 2026-07-10 時点の作業ツリーである。結論を先にまとめると、legacy render 経路の撤去自体は可能だが、現行 golden 16 件だけを受け入れ基準にするのは危険である。PhysWorld の生ポインタは再現可能な use-after-lifetime/誤 alias であり、ECS 外で entity ID 再解決へ逃がす応急処置は可能である。一方、非自明型コンポーネント問題は `remove` だけでなく構築・全消去にも及ぶため、`userpublic/details` だけで直すとしても小修正ではない。

## Q1: legacy 実行系の撤去リスク

### 現在の分岐

`FrameGraphRuntimeContainer` 自体は全 rendering pass に execution plan を登録するが、実行時は `frame_graph != nullptr && frame_graph->has_compute` のときだけ planned 経路へ入り、それ以外は legacy 経路へ落ちる。`has_compute` は compiled pass に compute task が一つでもあるかだけで決まる。従って「純 render config を planned に切り替える」とは、実質的にはこの `has_compute` gate を外す変更である（`src/core/renderingpass/framegraphruntime.cpp:30-40`, `src/core/vkcore/renderer.cpp:259-265`）。

### 実行意味論の差分

| 観点 | legacy | planned | 撤去時の評価 |
|---|---|---|---|
| render node 本体 | `CompiledRenderingPass::passes` を添字順に `RenderPassExecutor::execute` | plan node から元 pass の index を引き、同じ `RenderPassExecutor::execute` | 同じ executor なので、同じ順序なら draw、UI、fullscreen、attachment 設定は同じ（`src/core/vkcore/renderer.cpp:171-180`, `src/core/vkcore/renderer.cpp:210-219`） |
| 実行順 | JSON/compiled pass の宣言順 | planner の stable topological order | 現行 config 群では宣言順保存テストがあるが、`before`/`after` は planned だけが実行順に反映する。legacy は plan を無視する（`src/core/renderingpass/frameplanner.cpp:386-425`, `src/core/renderingpass/frameplanner.cpp:470-504`, `test/frameplanner_test.cpp:190-211`） |
| plan 整合性 | 検査なし | 毎 node、plan と execution node の name/kind/order を照合し、不一致なら throw | planned 化で新しい fail-fast 点が増える（`src/core/vkcore/renderer.cpp:195-204`） |
| RAW barrier | なし | node ごとに plan の全 barrier を走査し、source 実行済みを検査して発行 | buffer にだけ実 Vulkan barrier が出る。render target の barrier entry は `hasBuffer` が false なら no-op（`src/core/vkcore/renderer.cpp:141-161`, `src/core/renderingpass/computetask.cpp:501-518`） |
| image layout | 各 render pass の入力を shader-read、出力を color/depth attachment へ transition | render node は同じ処理。compute node だけ read/write image を `GENERAL` へ transition | 純 render の layout 意味論は同一 executor 由来。planned の plan barrier は image memory barrier の代用ではない（`src/core/vkcore/render_pass_executor.cpp:15-18`, `src/core/vkcore/render_pass_frame_setup.cpp:18-33`, `src/core/renderingpass/computetask.cpp:470-483`） |
| compute | 実行不能 | resource transition、dispatch を実行 | 既存 compute config は既に planned。legacy 撤去による新規差分ではない（`src/core/vkcore/renderer.cpp:213-219`） |
| clear/load/store | 共通 executor が attachment の `loadOp/storeOp/clearValue` を設定 | 同左 | 順序が同じ限り同一。UI も共通 executor の早期分岐（`src/core/vkcore/render_pass_frame_setup.cpp:36-68`, `src/core/vkcore/render_pass_executor.cpp:20-43`） |
| fullscreen input rebind | resize/hot reload 時に実行経路の外で全 pass を再 bind | 同左 | 経路差ではない。ただし撤去リファクタで呼出位置を動かしてはならない（`src/core/vkcore/renderer.cpp:268-303`） |
| swapchain/offscreen transition | `render_begin`/`render_end` が担当 | 同左 | 経路差ではない。swapchain は `UNDEFINED -> COLOR_ATTACHMENT -> PRESENT`、offscreen は `COLOR_ATTACHMENT -> TRANSFER_SRC` を外側で行う（`src/core/vkcore/swapchainframetarget.cpp:195-278`, `src/core/vkcore/offscreenframetarget.cpp:116-147`） |
| GPU timing | compiled pass 名と pass index を記録 | plan node 名と plan index を記録 | 現行順が同じなら見かけは同じ。明示 edge で並べ替えると timing の順・index も変わる（`src/core/vkcore/renderer.cpp:169-182`, `src/core/vkcore/renderer.cpp:191-227`） |
| CPU コスト | pass 数を V として O(V) | node kind map/set の毎フレーム構築、各 node から全 barrier を走査するため概ね O(V + V×B)、hash allocation あり | 絵が同じでも退行し得る。特に pass 数の多い example は golden 画像だけでは検出不能（`src/core/vkcore/renderer.cpp:133-161`, `src/core/vkcore/renderer.cpp:192-224`） |

重要な点は、pure-render graph で planner が生成する RAW barrier の大半は render target 名であり、現実の同期は plan barrier ではなく `RenderTargetLayoutTracker` の image transition に依存していることである。planned 化しただけで image hazard coverage が強くなるわけではない。また planner は「直前の writer -> reader」は結ぶが、write-after-read の一般的な memory dependency を barrier として表現していない（`src/core/renderingpass/frameplanner.cpp:391-405`）。現状の直列 command buffer と layout transition で偶然十分なケースと、将来 storage image/load-store が増えたケースを混同してはならない。

### golden 16 件が捕まえない差分

1. 16 件のうち `clear`、`fullscreen`、`triangle` は `Renderer::render()` を通らず、テスト自身が command buffer を直接記録する。legacy/planned 切替の検証になっていない（`test/golden_image_test.cpp:314-331`, `test/golden_image_test.cpp:488-510`, `test/golden_image_test.cpp:1308-1317`）。
2. `compute_buffer` は既に planned、残る Renderer 使用ケースは現在 legacy である。変更後の画像比較にはなるが、同一 build 内で旧経路との differential 比較をしていない（`test/golden_image_test.cpp:1179-1182`, `src/core/vkcore/renderer.cpp:259-265`）。
3. 各 case は 16×16、1 frame で、headless offscreen target を使う。swapchain acquire/present、複数 in-flight frame、suboptimal/out-of-date、resize は通らない（`test/golden_image_test.cpp:47-48`, `test/golden_image_test.cpp:1302-1306`, `src/core/vkcore/offscreenframetarget.cpp:160-165`）。
4. Renderer 使用 helper は直後に `waitIdle()` するため、frame 間同期の不足を覆い隠す（例 `test/golden_image_test.cpp:1155-1158`, `test/golden_image_test.cpp:1174-1181`）。
5. 描画中の任意の `std::exception` を `SKIP` に変える。planned 固有の整合性例外が出ても CI が緑になり得る（`test/golden_image_test.cpp:1402-1407`）。
6. 判定対象は最終 RGBA の average/max tolerance だけで、barrier 数、validation message、pass trace、GPU timing、CPU allocation、frame time は見ない（`test/golden_image_test.cpp:1352-1372`, `test/golden_image_test.cpp:1416-1430`）。
7. shader hot reload は明示的に無効で、offscreen target は extent change を返さないため、fullscreen descriptor rebind の二つの契機を試さない（`test/golden_image_test.cpp:1302-1306`, `src/core/vkcore/offscreenframetarget.cpp:168`）。

### 撤去 WP に追加すべき受け入れ基準

1. pure-render config に対して legacy/planned をテスト限定 selector で A/B 実行し、全 Renderer 使用 golden の画像だけでなく、実行 node trace、load/store/clear、最終 layout を一致させる。`SKIP` は「Vulkan device なし」だけに限定し、実行開始後の例外は fail にする。
2. `framePlanOrder(plan)` と実際に executor/dispatch へ渡った node 列が一致する instrumentation test を追加する。既存の静的 order test（`test/frameplanner_test.cpp:190-211`）は計画だけで、実行を見ていない。
3. pure-render の `before`/`after` で宣言順が実際に変わる case、同じ target への `LOAD` overlay、depth `LOAD`、UI overlay、fullscreen chain を追加する。順序変更を新仕様として採用しないなら、pure-render では「plan order == declaration order」を runtime/registration 時に拒否条件にする。
4. Vulkan validation と synchronization validation を有効にし、少なくとも in-flight 数を超える連続 frame を `waitIdle` なしで実行する。compute→render、render→compute、compute→compute の buffer/image を個別に含め、validation error 0 件を要求する。
5. window/swapchain smoke を別に持ち、acquire/present、resize、out-of-date recovery 後も validation error 0、最終 present 成功を要求する。headless golden で代用しない。
6. resize と shader hot reload のそれぞれで fullscreen input image/buffer descriptor が再 bind され、次 frame が新しい image view を使うことを検証する。
7. GPU timing 有効時に node 名・件数・順序が plan と一致すること、query overflow/欠落がないことを検証する。
8. representative な example config で CPU frame time、allocation 回数、recorded pipeline barrier 数を旧経路比で記録する。最低限「planned 化で CPU p95 が 10% 超悪化しない」等の budget を置く。現在の O(V×B) barrier scan は事前に node 別へ compile しておくのが望ましい。

## Q2: PhysWorld 生ポインタの実害範囲と freeze 内の応急処置

### 再現シナリオ

前提として scene load は `GameObjects::allocateRaw` が返す entity ID を捨て、そこで得た `TransformComponent*` を PhysWorld と SceneLoader の二管所へ保存する（`src/core/loader/scene.cpp:204-230`, `src/core/loader/scene.cpp:266-276`）。PhysWorld は query ごとにそのポインタを dereference する（`src/core/phys/physworld.hpp:32-40`, `src/core/phys/physworld.cpp:235-246`）。

同じ archetype chunk に A、B の順で entity を置き、A と B に collider を bind した後 `GameObjects::remove(A)` を呼ぶケースを考える。

1. `GameObjects::remove` は live ID vector から A を消して ECS の `remove` を呼ぶ（`src/core/userpublic/gameobjects.cpp:35-41`）。PhysWorld/SceneLoader には通知しない。
2. ECS は A の array slot へ chunk 末尾 B の各 component を `memcpy` し、末尾を shrink する。B の `id_to_ref` だけを A の slot に更新する（`src/core/userpublic/details/ecs/coretemplate.cpp:113-134`）。
3. A の PhysWorld binding が保持する pointer は A の旧 slot、すなわち現在の B transform を指す。従って A collider が B の位置に出る「ghost A」になる。
4. B の binding は旧末尾 slot を指したままである。その slot は byte vector の論理 size 外なので dereference は object lifetime 外アクセスである。capacity が残っていれば古い B 値に見えることがあるが、次の allocate で C に再利用されれば B collider が C に追従し、chunk 破棄後なら明白な dangling pointer になる（`src/core/userpublic/details/ecs/chunk.hpp:26-35`）。
5. `raycastClosest` は最初に全 binding を dereference して world collider を作るため、結果は ghost hit、誤 ID、hit 消失、または未定義動作による crash のいずれにもなり得る（`src/core/phys/physworld.cpp:235-251`）。

A が末尾だった場合も安全ではない。copy 元と先が同じ後に shrink され、A binding は論理 size 外を指す。削除済み collider がしばらく当たり続けるか、次 object に化ける。

実害は PhysWorld に閉じない。SceneLoader の `objectTransform` と `applyObjectTransform` も同じ `void*` を dereference するため、`update_transforms` RPC が別 object を動かす、削除済み object 名で解放後領域を更新する、といった破損が起きる（`src/core/loader/scene.cpp:283-309`, `src/core/communication/rpcserver.cpp:180-215`）。

### `src/core/ecs/` を変更しない v1 応急処置

可能である。ただし「現行 `GameObjects::localTransform` をそのまま呼ぶ」だけでは不十分である。scene loader の object は `TransformComponent` だけを持つ場合があり、公開 API は `LocalTransformComponent` の値取得しか提供しない。また scene loader は entity ID を現在捨てている（`src/core/userpublic/gameobjects.hpp:119-124`, `src/core/loader/scene.cpp:211-229`）。

最小構成は次のとおりである。

1. `GameObjects` 側に live ID の `unordered_set`（または ID→状態 map）を追加し、`tryTransform(GameObjectId)` が「生存確認後、現在の ECS slot から transform の値をコピーして返す」API を持つ。これは `src/core/userpublic/` の変更で済む。
2. SceneLoader は `allocateRaw` の戻り値を保持し、`object_bindings` を `void*` から entity ID へ変更する。RPC の object transform も毎回 ID から再解決する。
3. `PhysWorld::Binding` は `TransformComponent*` ではなく entity ID（static collider だけは値の `static_transform`）を保持する。`collectColliders` で current transform を値として解決し、削除済み ID は skip/prune する。
4. scene 全切替時は現行どおり PhysWorld/bindings を先に clear してから `removeAll` すればよい（`src/core/loader/scene.cpp:259-264`）。個別 remove は live set により即時無効化できる。

計算量は collider 数を M、live object 数を N とすると以下である。

- live ID を現行 vector の線形検索で済ませると、各 collider の生存確認が O(N) になり、1 回の再解決が O(MN)、M≈N なら O(N²) で不採用。
- hash set と ECS の ID→slot lookup を使えば期待 O(M)。現行も `collectColliders` は M 件を構築して ID sort するため O(M log M) であり、再解決自体は漸近コストを悪化させない（`src/core/phys/physworld.cpp:191-208`）。
- raycast/overlap を 1 frame に Q 回呼ぶ場合、毎 query で再構築すると現行どおり O(Q×M log M)。frame 冒頭に snapshot を一度 O(M log M) で作り、各 query を O(M) にすれば O(M log M + Q×M) にできる。transform 更新後の可視タイミングを「次 frame」と固定すれば決定的である。
- 追加メモリは live set O(N)、binding/snapshot O(M)。

従ってこの応急処置は実装可能かつ v1 として妥当である。受け入れテストには「A 削除で A は hit せず B は同じ位置/ID で hit」「末尾削除」「削除 slot 再利用」「scene RPC binding」「ASan/UBSan」を入れるべきである。なおこれは ECS の byte-move 自体は直さず、外部長寿命 pointer だけを排除する封じ込めである。

## Q3: ECS の非自明型コンポーネント棚卸し

### 本番 player で登録される component

組み込み登録は `eid`, `transform`, `localtransform`, `simplemodelview`, `simplemodelviewupdate`, `camera`、player 固有登録は `mychar` である（`src/core/ecs/predefined.cpp:19-31`, `src/player/main.cpp:363-369`, `src/player/main.cpp:430-436`）。

| ID / 型 | 所有・副作用 | memcpy 危険度 | 判定 |
|---|---|---|---|
| 0 `EntityId` | 整数 | 低 | byte copy 可 |
| 1 `TransformComponent` | glm vec/quat の値 | 低 | heap なし（`src/core/ecs/predefined/transform.hpp:9-26`） |
| 2 `SimpleModelViewComponent` | `optional<ModelInstanceId>` は heap なし。ただし `deinit()` が renderer の model instance を解放 | 高（資源リーク/ghost） | byte relocation より、削除時に `cb_deinit` が呼ばれないことが実害。削除 object の GPU-side instance が残る（`src/core/ecs/predefined/modelview.hpp:10-16`, `src/core/ecs/predefined/modelview.cpp:8-12`） |
| 3 `CameraComponent` | `int dummy` | 低 | byte copy 可（`src/core/ecs/predefined/camera.hpp:5-9`） |
| 16 `LocalTransformComponent` | vec/quat/EntityId の値 | 低 | heap なし（`src/core/userpublic/components/localtransform.hpp:10-20`） |
| 18 `SimpleModelViewUpdateComponent` | `std::string model_name` | **致命的** | raw storage 上の未構築 string に代入し、remove で内部 pointer を shallow-copy する。ソース自身に「データぶっ壊れる」とある（`src/core/userpublic/components/modelview.hpp:11-29`） |
| 32 `MyCharComponent` | float | 低 | byte copy 可（`src/player/main.cpp:363-369`） |

別枠の重大不整合として、`SphereColliderComponent`（実体は `ColliderComponent`、`std::string shape` を所有）は ID 17 として宣言され system query にも使われるが、`ECSPredefinedRegistration::reg()` に登録行がない（`src/core/userpublic/components/predefined.hpp:14-16`, `src/core/userpublic/components/collider.hpp:13-19`, `src/core/ecs/predefined.cpp:27-31`, `src/core/ecs/predefined.cpp:42-43`）。現在 scene collider は ECS component から除外して PhysWorld へ別保存するので通常 scene load では踏まないが、`GameObjects::add().addComponent<SphereColliderComponent>()` を使うと size/lifecycle metadata が正しくない。登録漏れを直した瞬間、`std::string` のため危険度は `SimpleModelViewUpdateComponent` と同じ「致命的」になる。

### `remove` 以前から存在する UB

問題は `memcpy` relocation だけではない。

- component storage は `vector<uint8_t>::resize` で byte 数を増やすだけで placement construction をしない（`src/core/userpublic/details/ecs/chunk.hpp:14-35`）。
- `GameObjects::copy` はその raw address に `operator=` する（`src/core/userpublic/gameobjects.hpp:54-60`）。
- JSON load も serializer を先に呼び、その後 `initComponent` を呼ぶだけで C++ constructor は呼ばない（`src/core/ecs/componentinfo.cpp:18-31`）。
- `cb_deinit` は metadata に登録されるが、`remove` も `clearEntities` も呼ばない（`src/core/ecs/componentinfo.hpp:16-23`, `src/core/userpublic/details/component/registerer.cpp:15-24`, `src/core/userpublic/details/ecs/coretemplate.cpp:113-145`）。

従って heap-owning component は生成時点で lifetime UB、remove 時点で shallow-copy、clear 時点で destructor/deinit 漏れの三重故障である。SSO 文字列なら copied pointer が旧末尾 object 内を指し、heap 文字列なら allocation が解放されず leak する。後の代入・slot 再利用で crash/二重所有に発展し得る。

### `userpublic/details` 側だけで直せるか

パス上は可能だが、現実的な「応急修正」ではない。`ComponentInfoManager` の変更を避けるなら `userpublic/details/component/registerer` に component ID→`LifecycleOps` の別 registry を作り、少なくとも次を型消去 callback として保持する必要がある。

- default construct / copy construct
- move construct（move 不可なら copy construct）
- C++ destructor
- application-level `deinit`

そのうえで `userpublic/details/ecs/chunk` と `coretemplate` の全経路を次の規約へ変える必要がある。

1. allocate: storage 確保後に各 slot を placement-new。
2. remove target: target の `deinit`→destructor、末尾を target へ move-construct、末尾は destructor のみ（moved resource を二重 deinit しない）。target==末尾なら deinit→destructor だけ。
3. clear: 全 live slot へ deinit→destructor 後に byte storage を clear。
4. trivially-copyable component だけ fast memcpy を許可し、未登録 lifecycle の非自明型は registration 時に拒否する。

これはファイル配置上 `src/core/ecs/` を触らず実装できるが、実体として ECS storage semantics の全面変更である。custom component、alignment、例外安全、chunk capacity、clear/remove、model instance side effect を sanitizer 込みで再検証する必要がある。v1 freeze 中の現実的対応は、(a) 非自明型の新規登録を `static_assert(std::is_trivially_copyable_v<T>)` で禁止、(b) 現在の string component を interned/fixed-size handle に置換、(c) `SimpleModelViewComponent` の解放を ECS 外の明示破棄へ一時退避、である。一般解は freeze 後の独立 WP にすべきである。

## Q4: strict v1 化の爆風半径

ここでは「RPC 一語彙」を、現在唯一食い違っている `update_transforms.transforms[].rot` を scene/ECS と同じ `rotation` に統一する意味として監査した。RPC parser は現状 alias を複数受理しておらず、method 名にも別名はない（`src/core/communication/rpcserver.cpp:164-184`, `src/core/communication/rpcserver.cpp:522-639`）。

### version == 1

project と scene は現在「1 より新しければ reject」で、0/負数を受理する。project は runtime と devcli の二箇所を `!= 1` にする必要がある（`src/core/loader/basicconfig.cpp:236-249`, `src/devcli/distconfig.cpp:277-281`）。scene も同様で、さらに schema 無し top-level scene を legacy として受理する（`src/project/sceneformat.cpp:47-61`, `src/project/sceneformat.cpp:145-158`）。

現存する正常 project は version 1 なので、`==1` 化だけで壊れる実 project はない。壊れる fixture は以下である。

- `test/fixtures/project_format/valid/scene_legacy.json:1` — schema/version 無し、top-level scene、legacy `lights` を使用。
- `test/fixtures/project_format/expectations.json:76` — 上記を `expect: ok` としているため、strict 化後は外部 converter test へ移すか `expect: error` にする。

`project_future_version.json` / `scene_future_version.json` は既に error fixture なので変更不要。strict の穴を固定するため version 0 と -1 の project/scene error fixture を新設すべきである。

### camera alias 削除で直接壊れるもの

canonical は glTF 系の `yfov`（radian）、`znear`, `zfar` である。loader は現在 `fov_y` だけ degree→radian 変換する（`src/core/loader/basicconfig.cpp:131-159`, `src/core/loader/basicconfig.cpp:174-198`）。scene camera も同じ alias を持つ（`src/core/renderer/camera.cpp:198-208`, `src/core/renderer/camera.cpp:260-273`）。

- 実データ: `projects/example/project.json:16-18`。
- embedded default: `src/core/resources/default_config.json:12-14`。
- devcli init template: `src/devcli/projectinit.cpp:102-106`。生成済み新規 project が即起動不能になるため、runtime 変更と同一 WP で必須。
- camera unit data: `test/camera_test.cpp:82`（alias 受理を明示的に期待する test も `test/camera_test.cpp:195`）。
- golden の project generator 6 箇所: `test/golden_image_test.cpp:148`, `test/golden_image_test.cpp:171`, `test/golden_image_test.cpp:194`, `test/golden_image_test.cpp:217`, `test/golden_image_test.cpp:240`, `test/golden_image_test.cpp:263`。
- CMake test project generator 11 本: `test/run_build_units_smoke.cmake:127`, `test/run_project_code_smoke.cmake:105`, `test/run_gpu_timing_headless.cmake:31`, `test/run_compute_headless.cmake:29`, `test/run_rpc_headless.cmake:36`, `test/run_frame_plan_dump_headless.cmake:33`, `test/run_rpc_inject_event_headless.cmake:33`, `test/run_rpc_inject_input_headless.cmake:39`, `test/run_rpc_scene_flow_headless.cmake:35`, `test/run_vatplayer_headless.cmake:37`, `test/run_seqplayer_headless.cmake:37`。

`projects/example/scenes/main.scene.json:1199-1202` と `test/run_rpc_set_camera_headless.cmake:35` は既に canonical。`test/fixtures/project_format/valid/project_basic_config.json` は camera を省略して default へ fallback するので default 更新後は壊れない。

### collider alias 削除で直接壊れるもの

実 project/scene には legacy `size`/`height` collider は見つからなかった。壊れるのは互換テストだけである。

- box `size`: `test/physworld_test.cpp:59`。
- capsule `height`: `test/physworld_test.cpp:68`。

canonical case は golden 内に既にある（`test/golden_image_test.cpp:757`）。parser の alias は `size/2 -> half_extents`, `height/2 -> half_height` である（`src/core/userpublic/components/collider.cpp:60-80`）。

### RPC 語彙統一で直接壊れるもの

runtime parser 自身は `rot` 固定なので `rotation` へ変更が必要（`src/core/communication/rpcserver.cpp:164-171`）。RPC 結合データで壊れるのは以下である。

- `test/run_rpc_headless.cmake:83`, `test/run_rpc_headless.cmake:86`, `test/run_rpc_headless.cmake:91`。
- `test/run_rpc_set_camera_headless.cmake:93`。
- `test/run_rpc_scene_flow_headless.cmake:103`, `test/run_rpc_scene_flow_headless.cmake:110`。

`transform_seq` は別の schema/protocol であり、今回「RPC のみ」の統一なら `test/fixtures/seqplayer_two_objects.jsonl:2-5` と `test/seqplayer_test.cpp:12-14` の `rot` は変更対象外である。全 transform payload を一語彙にする方針まで広げるなら、これらと `src/core/playback/seqplayer.cpp:61` も同時移行が必要になる。

### shader stem only で直接壊れるもの

実 project の rendering config と engine feature は既に extensionless stem である（例 `projects/example/passes/main_rendering_config.json:130-132`, `src/core/resources/features/debug_text.json:13-15`）。`test/fixtures/project_format/valid/shader_stem_spv.json:1-6` も参照値自体は stem で、backing file が SPIR-V なだけなので壊れない。物理 `.spv` ファイルや `engine_resources.json` の inventory 名を stem に改名してはならない。

壊れるのは explicit-file 後方互換を直接テストする埋込データである。

- `test/projectconfig_test.cpp:78-97`, および ref の一致 assertion `test/projectconfig_test.cpp:142-146`。
- `test/renderingpass_helpers_test.cpp:116-131`, `test/renderingpass_helpers_test.cpp:161-188`, `test/renderingpass_helpers_test.cpp:285-297`, `test/renderingpass_helpers_test.cpp:355-423`。
- `test/shader_library_test.cpp:110-113` の explicit `.spv` load test。stem resolution/error-order test 自体は残す。

`test/fixtures/material_format/invalid/bad_shader_ref.json:7` は既に extension 付き shader を error と期待しており、strict 化後もそのままでよい。現在 `makeShaderReference` は `.spv/.vert/.frag/.comp/.wgsl` を明示 file として受理するため、stem-only は parser の呼出側だけでなくこの入口の方針も決める必要がある（`src/core/shader/shaderreference.cpp:46-58`）。

### 一括移行対応表

| 旧 | strict v1 | 変換 |
|---|---|---|
| project/scene `version` 欠落、0、負数 | `version: 1` | legacy scene は `{"schema":"pelican.scene","version":1,"scenes":{...}}` で包み、top-level `lights` は named object の `light` component へ外部変換 |
| camera `fov_y: D` | `yfov: R` | `R = D × π / 180`。例 45°→0.78539816339、50°→0.872664625997、60°→1.04719755120 |
| camera `near` | `znear` | 値は同じ |
| camera `far` | `zfar` | 値は同じ |
| box collider `size: [x,y,z]` | `half_extents: [x/2,y/2,z/2]` | 各軸を 1/2 |
| capsule collider `height: h` | `half_height: h/2` | 1/2（現 parser の意味を保存） |
| RPC transform `rot: [x,y,z,w]` | `rotation: [x,y,z,w]` | 値・順序は同じ |
| shader `foo.vert.spv`, `foo.vert`, `foo.frag.spv`, `foo.comp` | stage field 内で stem `foo` | stage と `.spv` を参照から落とす。物理 file は `foo.<stage>` または `foo.<stage>.spv` のまま。scheme は保持（例 `project://shaders/foo`） |
| shader `foo.spv` / `foo.wgsl` | stage field 内で stem `foo` | stem resolver は `<stem>.<stage>` と `<stem>.<stage>.spv` しか探索しないため、そのまま拡張子を落とすだけでは不可。field の stage に合わせ `foo.<stage>.spv` へ外部 bake/rename する（`src/core/shader/shaderlibrary.cpp:218-254`） |

一括変更は「runtime parser → embedded default/devcli template → projects/example → fixtures/test generators → compatibility test を rejection/converter test へ変更」の順ではなく、同一 commit で atomic に行うべきである。途中状態では default fallback や `project init` が全 test を連鎖的に壊す。

## Q5: `implementation_plan.md` の分割案

### 現状評価

実物は 106,848 bytes、1,287 行で、共通規則、完了 WP の詳細、将来 backlog、並列開発の時系列、依頼テンプレートが一ファイルに混在する。WP 一覧は WP41 で止まるのに、詳細は WP59 まである（`docs/implementation_plan.md:54-102`, `docs/implementation_plan.md:104-1199`）。これは検索性よりも、status の真偽を人間が判別できないことが主リスクである。

### 提案する分割構成

```text
docs/
  implementation_plan.md              # 現在の入口。5〜10KB
  backlog.md                           # 未着手候補だけ
  contributor_guide.md                 # 恒久的な実装・テスト・レビュー規則
  work_packages/
    README.md                          # active WP の status/owner/依存/evidence 表
    WPxx_<slug>.md                     # active/approved WP だけ。完了時 archive へ移動
  archive/
    implementation_plan/
      README.md                        # WP1〜59 の索引、最終 status、日付、検証 evidence
      2026-06_rendering_wp01-17.md
      2026-07_formats_wp18-27.md
      2026-07_framegraph_input_wp28-41.md
      2026-07_gameplay_services_wp42-59.md
      2026-06_to_07_wave_history.md
```

移動内容は次のとおりである。

- 現 `§0 全 WP 共通規則` と有効な `§4` の競合回避原則、`§5` の依頼テンプレートを `docs/contributor_guide.md` へ移す（`docs/implementation_plan.md:16-52`, `docs/implementation_plan.md:1225-1247`, `docs/implementation_plan.md:1279-1287`）。特定日・特定 branch の運用は恒久規則から外し wave history へ送る。
- 現 `§2 WP 詳細` を上記 4 archive へ年代/機能群で分ける。完了文書の先頭に `status: complete`, 完了日、検証 test、後続で判明した debt を付け、本文は履歴として変更しない。
- 現 `§3 保留中のトラック` は、実装済み項目を除去して `docs/backlog.md` へ移す。各行を `proposed / design-needed / approved / blocked` のいずれか、依存、次の decision で表にする（`docs/implementation_plan.md:1201-1223`）。
- wave 表は実行履歴なので archive の `wave_history` へ移す（`docs/implementation_plan.md:1248-1277`）。今後の並列実行は active WP metadata から生成する。
- 新 `docs/implementation_plan.md` は「現在の release 目標」「active WP への link」「次の 3 件」「既知の blocker」「backlog/archive/contributor guide への link」だけにする。完了 WP の本文を置かない。

### 「実装済みなのに未着手/承認待ち扱い」の箇所

| 計画書の古い記述 | 実装済みの根拠 | 判定 |
|---|---|---|
| WP31 を「設計文書側で予約、近づいたら詳細登録」とする（`docs/implementation_plan.md:92-93`） | WP31 詳細が既にあり、shadow feature も実装済み（`docs/implementation_plan.md:943-968`, `src/core/resources/features/shadow_directional.json:17-27`） | 明白に stale |
| compute/GPU 計測/コマンド層を「設計合意・前提 WP 完了待ち」とする（`docs/implementation_plan.md:102`）、さらに compute/GPU 計測を「設計後 WP 化」とする（`docs/implementation_plan.md:1223`） | planned compute 実行、GPU timing、frame-plan RPC が存在（`src/core/vkcore/renderer.cpp:185-227`, `src/core/communication/rpcserver.cpp:628-631`） | 実装済みを未着手扱い |
| stage 3 `load_gltf/update_transforms` は「実装 GO 決定」とだけ書く（`docs/implementation_plan.md:1203`） | 両 handler と pending transform 適用が実装済み（`src/core/communication/rpcserver.cpp:573-591`） | status 未更新 |
| camera system を「方向決定、設計文書を起草」とする（`docs/implementation_plan.md:1206`） | C1/C2 と set_camera、controller system が実装済み（`src/core/communication/rpcserver.cpp:602-608`, `src/core/userpublic/cameracontrollersystem.cpp:186-240`） | status 未更新 |
| 物理 query を「必須決定、再評価から着手」とする（`docs/implementation_plan.md:1209`） | PhysWorld raycast/overlap/scene binding が実装済み（`src/core/phys/physworld.cpp:191-265`, `src/core/loader/scene.cpp:204-230`） | status 未更新。ただし Q2 の debt あり |
| event layer は user review 後に E1 を WP 化するとする（`docs/implementation_plan.md:1214`） | WP56 詳細、event dispatch、SceneLoaded emit、inject_event が存在（`docs/implementation_plan.md:1122-1139`, `src/core/userpublic/details/event/registerer.cpp:114`, `src/core/loader/scene.cpp:233`, `src/core/communication/rpcserver.cpp:553-565`） | 承認待ち記述が stale |
| `user://` / asset store / fragment の承認待ち（`docs/implementation_plan.md:1215-1217`） | PathResolver に `user://`, `asset_stores`, fragment parser が実装済み（`src/core/loader/pathresolver.cpp:27`, `src/core/loader/pathresolver.cpp:256-282`, `src/core/loader/pathresolver.cpp:336-447`） | WP55 完了状態が未反映 |
| project interpretation target 分離を「後続の小 WP」とする（`docs/implementation_plan.md:1222`） | wave 表自身が WP44 完了とする（`docs/implementation_plan.md:1269`） | 同一文書内矛盾 |
| wave 20〜22 を今後の並列依頼のように置く（`docs/implementation_plan.md:1271-1273`） | WP31/49/50、WP51〜54、WP55〜58 の artifact/test が存在。例: inject_input（`src/core/communication/rpcserver.cpp:544-551`）、audio（`src/core/audio/audio.cpp:435`）、scene flow（`src/core/loader/scene.cpp:236-251`）、RNG（`src/core/userpublic/deterministicrng.cpp:77`）、debug_text（`src/core/renderer/debugtext.cpp:337-400`）、project init（`src/devcli/projectinit.cpp:390`）、material parser（`src/project/materialformat.cpp:377-385`） | wave history を active plan と混在させた結果 |
| WP59 を探索 WP の指示形のまま置き、wave/status 表に載せない（`docs/implementation_plan.md:1174-1199`） | spike report と pipeline 成功記録が存在（`experiments/spvlink/REPORT.md:1-13`, `experiments/spvlink/REPORT.md:54-56`） | 完了 archive へ移すべき |

逆方向の重大な誤記もある。WP35 節は「WP34 が render/compute 両 node を plan 駆動で実行し、golden 全維持で意味論保存を実証済み」とするが、実装は `has_compute` のない pure-render config を legacy に落としている（`docs/implementation_plan.md:678-681`, `src/core/vkcore/renderer.cpp:259-265`）。これは「完了扱いだが中核が未完」のため、archive metadata に debt として残し、legacy 撤去 WP を新規 active WP にする必要がある。

さらに共通規則は `src/core/ecs/` 変更禁止のままだが、後段では禁止解除済みと書く（`docs/implementation_plan.md:47-50`, `docs/implementation_plan.md:1218`）。strict v1 についても後段で互換受理追加禁止とする一方、Q4 の aliases/legacy 受理が残る。分割時には後段の決定を contributor guide の唯一の現行規則へ昇格し、古い規則は日付付き archive にだけ残すべきである。

### 分割の受け入れ基準

1. WP1〜59 が archive index に一度ずつ現れ、各 status が `complete/active/backlog` のいずれかである。
2. active plan に完了 WP の詳細本文を置かない。
3. backlog に実装済み artifact のある項目を置かない。残すなら「追加 debt」を別項目として命名する。
4. `rg` で「承認待ち」「WP 化待ち」「変更禁止」を監査し、現行規則との矛盾 0 件にする。
5. 全相対 link を確認し、既存 design 文書から旧 `implementation_plan.md` の anchor を参照している箇所には archive への redirect/link を用意する。
