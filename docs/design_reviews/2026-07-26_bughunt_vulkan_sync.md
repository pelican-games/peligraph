# Vulkan 同期・リソース状態バグハント監査

- 監査日: 2026-07-26
- 監査対象: `audit/sync` / `20efe46ab1683850c385d4640226707a9608a783`
- 方法: ソースコードの読み取り専用静的監査。指示に従い build / test は実施していない。
- 仕様根拠:
  - [Vulkan Guide: Swapchain Semaphore Reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
  - [Vulkan Guide: Depth](https://docs.vulkan.org/guide/latest/depth.html)
  - [Vulkan Guide: Synchronization Examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
  - [Vulkan Specification: Synchronization and Cache Control](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)

## 結論

現状を「同期面で安全」とは判定できない。確定指摘は HIGH 5 件、MEDIUM 1 件である。特に、present 待ちセマフォの再利用、描画例外後の未 signaled fence 再待機、DeletionQueue の fence 非連動解放は、既定の二重バッファリングや継続稼働する RPC 経路に直結する。

| ID | 重大度 | 確度 | 結論 |
|---|---|---|---|
| VKSYNC-01 | HIGH | HIGH | present 待ちの binary semaphore を in-flight frame 単位で再 signal している |
| VKSYNC-02 | HIGH | HIGH | acquire 後の例外で fence が未 signaled のまま残り、次回描画が永久待機する |
| VKSYNC-03 | HIGH | HIGH | DeletionQueue が GPU 完了より先に Vulkan オブジェクトを破棄し得る |
| VKSYNC-04 | HIGH | HIGH | 読み取りを伴わない、順序付け済み WAW に memory dependency が生成されない |
| VKSYNC-05 | HIGH | HIGH | depth attachment の barrier stage scope が EARLY/LATE の片方ずつ欠落する |
| VKSYNC-06 | MEDIUM | HIGH | tile-local fusion が同居する materialized attachment の中間 load/clear/store 契約を消す |

## 指摘詳細

### VKSYNC-01 — present 待ち semaphore を swapchain image ではなく in-flight frame で再利用する

- 重大度: **HIGH**
- 確度: **HIGH**
- 主因:
  - `src/core/vkcore/swapchainframetarget.cpp:211-215` — `rendered_semaphores` を `in_flight_frames_num` 個だけ生成する。
  - `src/core/vkcore/swapchainframetarget.cpp:244-255` — frame slot の再利用条件は graphics submit fence の完了だけである。
  - `src/core/vkcore/swapchainframetarget.cpp:421-432` — 同じ `rendered_semaphores[in_flight_frame_index]` を submit で signal し、`vkQueuePresentKHR` で wait する。
  - `src/core/vkcore/swapchainframetarget.cpp:456-457` — semaphore の選択を acquired image index ではなく循環 frame index で進める。
  - `src/core/vkcore/swapchainframetarget.cpp:94-100` — graphics / presentation queue family が異なる構成も明示的にサポートしている。

具体的な失敗シーケンス:

1. frame slot 0 の graphics submit が `rendered_semaphores[0]` を signal し、presentation queue がその semaphore を wait する present を受理する。
2. presentation queue が滞留したまま、graphics queue 側の slot 0 submit fence は完了する。
3. CPU は二巡後にその submit fence だけを wait して slot 0 を再利用する。
4. 新しい graphics submit が `rendered_semaphores[0]` を再び signal するが、以前の present が同じ binary semaphore をまだ使用中であり、仕様違反になる。

submit fence は graphics workload とその semaphore signal の完了を示すだけで、present operation が semaphore wait を消費したことを保証しない。Khronos の “Swapchain Semaphore Reuse” が示す誤りと同型であり、SDK 1.4.313 以降では `VUID-vkQueueSubmit-pSignalSemaphores-00067` または「semaphore may still be in use by VkSwapchainKHR」として検出される。

- 症状: validation error、フレーム停止、presentation の乱れ、driver によっては device loss / hang。
- 必要な修正方向: present 待ち semaphore を swapchain image 数だけ持ち、`current_image_index` で選ぶ。代替は `VK_EXT_swapchain_maintenance1` の presentation fence で再利用完了を証明すること。

### VKSYNC-02 — acquire 後の例外で frame slot の fence が永久に未 signaled になる

- 重大度: **HIGH**
- 確度: **HIGH**
- 主因:
  - `src/core/vkcore/swapchainframetarget.cpp:244-294` — fence を wait/reset し、image acquire 後に command buffer recording を開始する。
  - `src/core/vkcore/cmdbuf.cpp:8-15` — `recordBegin()` も fence を reset するが、対応する submit を保証する guard はない。
  - `src/core/vkcore/renderer.cpp:2903-2937` — `target.beginView()` の後にも resize 処理や format 検証があり、例外を送出できる。
  - `src/core/vkcore/renderer.cpp:2969-3001` — pass 実行中にも多数の検証例外が到達可能である。
  - `src/core/vkcore/renderer.cpp:3016`、`src/core/vkcore/renderer.cpp:1946-1951` — submit は関数末尾の `endLogicalFrame()` に到達した場合だけ実行される。
  - `src/core/vkcore/swapchainframetarget.cpp:456-457` — in-flight index も submit/present 完了経路でしか進まない。
  - `src/core/communication/rpcserver.cpp:737-751`、`src/core/communication/rpcserver.cpp:1059-1064` — RPC は描画例外を application error に変換してサーバー処理を継続する。

具体的な失敗シーケンス:

1. `beginView()` が現在 slot の fence 完了を待ち、swapchain image を acquire して fence を reset する。
2. 例として frame-target format 不一致 (`renderer.cpp:2933-2937`) が例外を送出し、`endLogicalFrame()` へ到達しない。
3. この slot の fence を signal する queue submit は存在せず、acquired image と acquire semaphore も途中状態で残る。in-flight index も進まない。
4. RPC server は例外を応答へ変換するため、次の `render_frame` は同じ slot を再利用しようとする。
5. `waitForFences(..., UINT64_MAX)` が、どの submit にも関連付いていない未 signaled fenceを永久に待つ。

offscreen target も `src/core/vkcore/offscreenframetarget.cpp:142-154` で同じ `recordBegin()` を使い、submit は `src/core/vkcore/offscreenframetarget.cpp:211-235` に到達した場合だけなので、例外 unwind に対する同型の rollback 欠落がある。

- 症状: 例外を返した次の描画 RPC が永久停止する。swapchain 側では image/acquire semaphore の回収不能も併発する。
- 必要な修正方向: acquire/recording をトランザクション化し、例外時にも slot を再利用可能な状態へ戻す abort 経路を設ける。少なくとも RPC が継続する前に、acquire semaphore を消費する submit、fence の再 signal、image の解放/再作成、index と lease の整合回復を一体で行う必要がある。

### VKSYNC-03 — DeletionQueue の logical epoch が fence 完了より先に資源を解放する

- 重大度: **HIGH**
- 確度: **HIGH**
- 主因:
  - `src/core/vkcore/deletionqueue.hpp:31-39`、`src/core/vkcore/deletionqueue.hpp:54-60` — defer 時点の logical `current_frame` だけを保存する。
  - `src/core/vkcore/deletionqueue.cpp:74-81` — `beginFrame()` 呼び出し回数が in-flight 数に達すると release し、fence/timeline の完了値は参照しない。
  - `src/core/vkcore/rendertarget.hpp:40` — in-flight 数は 2。
  - `src/core/vkcore/renderer.cpp:2490-2491` — DeletionQueue の進行と解放は target の fence wait (`renderer.cpp:2718` / `renderer.cpp:2903`) より前である。
  - `src/core/appflow/framephase.cpp:119-124` — hot reload publication は次の描画より前の frame boundary で行われる。
  - `src/core/shader/pipelinefactory.cpp:567-579` — hot reload で置換した旧 pipeline/layout を DeletionQueue に積む具体的な経路がある。

具体的な失敗シーケンス:

1. epoch N の frame が slot 0 へ submit され、旧 pipeline `P` を使用する。
2. 次の frame boundary で hot reload が `P` を置換し、まだ `current_frame == N` の DeletionQueue に `P` を defer する。
3. epoch N+1 は queue 冒頭を進めた後、slot 1 の fence を待って submit する。これは slot 0 の frame N の完了を保証しない。
4. epoch N+2 冒頭では `releaseEligible(N)` が **slot 0 の fence wait より先に** `P` を破棄する。
5. GPU が二フレーム遅延していれば、slot 0 はまだ `P` を参照中である。

したがって、例外がなくても「2 回 begin した」ことは「該当 submit fence が完了した」ことの代用にならない。さらに VKSYNC-02 のような例外でも `deletion_queue.beginFrame()` は既に進んでおり、RPC retry が submit/fence の進捗なしに epoch を進めるため、危険性は増す。`wait_idle_hook` は teardown のために保持されているが、通常の `beginFrame()` 解放では呼ばれない。

- 症状: `VkPipeline`、`VkPipelineLayout`、descriptor、image/view などの in-use destroy、validation error、描画破損、device loss / crash。
- 必要な修正方向: retire epoch を CPU の描画試行数ではなく GPU completion serial にする。submission lease へ旧資源を含める、または timeline semaphore / slot fence の完了を確認した serial だけを release する。単に `beginFrame()` の回数を増やす方法では保証にならない。

### VKSYNC-04 — pure WAW に frame graph barrier が生成されない

- 重大度: **HIGH**
- 確度: **HIGH**
- 主因:
  - `src/core/renderingpass/frameplanner.cpp:417-420` — render output は write として登録される。
  - `src/core/renderingpass/frameplanner.cpp:465-481` — attachment の load op が `LOAD` の場合だけ同じ output を read にも登録する。
  - `src/core/renderingpass/frameplanner.cpp:875-888` — graph 構築は read にだけ last-writer data edge/barrier を追加し、write は `last_writer` の更新だけで終わる。
  - `src/core/renderingpass/frameplanner.cpp:857-866` —明示 `after` / `before` edge に対しても、producer write と consumer read の交差だけを barrier 化する。
  - `src/core/renderingpass/frameplanner.cpp:928-951` — WAW validation は reachability があれば合格させるが、memory dependency の存在は検証しない。
  - `src/core/vkcore/render_target_layout_tracker.cpp:167-180` —二つの pass が同じ attachment layout を使うと、二回目の transition は early return し、barrier を発行しない。
  - `src/core/vkcore/renderer.cpp:835-918` — runtime が発行する graph memory dependency は compiled barrier が存在する場合だけである。

具体的な失敗シーケンス:

1. pass A と pass B が同じ materialized render target `C` を output に持ち、B を `after: A` で順序付ける。
2. B の `color_load_op` を `CLEAR` または `DONT_CARE` にする。B は `C` の reader にはならず、pure WAW になる。
3. reachability があるため ambiguous WAW validation は通過するが、write→read の交差がないので compiled barrier は生成されない。
4. A と B は別 dynamic-rendering scope で `C` を `COLOR_ATTACHMENT_OPTIMAL` のまま連続使用する。layout tracker も同一 layout のため何も発行しない。
5. A の color write と B の clear/write の間に必要な memory dependency がなく、WAW data race になる。

Vulkan 仕様は RAW/WAW に適切な memory dependency が必要で、欠落は data race と明記している。Synchronization Examples も、前内容を保持せず `UNDEFINED` から再利用する depth attachment でさえ WAW dependency が必要としている。

- 症状: synchronization validation の WAW hazard、clear/write の順序乱れ、ちらつき・破損、実装依存の device loss。
- 必要な修正方向: 同一 resource の連続 writer 間にも data edge と image/buffer memory dependency を生成する。`LOAD` で read に見せることへ依存せず、write-after-write を独立に表現する必要がある。

### VKSYNC-05 — depth attachment barrier の EARLY/LATE stage scope が不完全

- 重大度: **HIGH**
- 確度: **HIGH**
- 主因:
  - `src/core/vkcore/render_target_layout_tracker.cpp:25-34` —旧 layout が `eDepthAttachmentOptimal` のとき、source stage は `eLateFragmentTests | eColorAttachmentOutput` であり `eEarlyFragmentTests` がない。
  - `src/core/vkcore/render_target_layout_tracker.cpp:72-77` —新 layout が `eDepthAttachmentOptimal` のとき、destination stage は `eEarlyFragmentTests | eColorAttachmentOutput` であり `eLateFragmentTests` がない。
  - `src/core/vkcore/render_target_layout_tracker.cpp:184-215` — layout を変えない explicit memory dependency も同じ `makeTransitionInfo(layout, layout)` を使うため、この欠落を引き継ぐ。

具体的な失敗シーケンス:

1. pass A の pipeline/implementation が early fragment tests で depth attachment を書く。
2. pass B が同じ depth image を sample するため `DepthAttachmentOptimal -> ShaderReadOnlyOptimal` を発行する。
3. source stage scope に EARLY がないので、A の early depth write が availability scope に含まれず、B の fragment read と競合し得る。

逆方向でも、sampled depth を次の depth attachment passへ戻すとき destination scope に LATE がなく、late depth read/write を barrier が対象にできない。depth resolve 用に `COLOR_ATTACHMENT_OUTPUT` を併記することは妥当だが、EARLY/LATE の欠落を補わない。Khronos の depth guide と synchronization examples は depth attachment access の stage mask に EARLY と LATE の双方を使っている。

- 症状: depth sampling、shadow、SSAO 等の不定なちらつき/欠落、synchronization validation hazard。
- 必要な修正方向: depth attachment の source/destination scope の双方へ `eEarlyFragmentTests | eLateFragmentTests` を含め、resolve が必要な経路だけ `eColorAttachmentOutput` を追加する。

### VKSYNC-06 — tile-local fusion が別 attachment の中間 CLEAR/LOAD/STORE を無視する

- 重大度: **MEDIUM**
- 確度: **HIGH**
- 主因:
  - `src/project/targetrenderplanning.cpp:1555-1624` — tile-local crossing edge があれば producer/consumer を単一 rendering scope に fuse する。
  - `src/project/targetrenderplanning.cpp:1627-1633` — scope に `local_reads` がある場合、materialized attachment 契約を比較する `canAppendMaterializedRenderingScope()` を呼ばない。
  - `src/project/vulkanphysicalfragment.cpp:1482-1489` — physical fragment 側の materialized operation validation も `local_reads` がある scope を丸ごと除外する。
  - `src/core/renderingpass/renderingpassruntimecompiler.cpp:487-590` — fused scope の load/store/attachment 一致検証は `!result.local_read_scope` の場合だけである。
  - `src/core/renderingpass/renderingpassruntimecompiler.cpp:593-644` — scope attachment operation は「最初の writer の load」と「最後の writer の store」だけに集約される。
  - `src/core/vkcore/render_pass_executor.cpp:395-427` —実コマンドは scope 全体で `beginRendering` 一回、各 pass は mapping と draw だけであり、中間 clear/load/store command はない。

具体的な失敗シーケンス:

1. pass A が tile-local 候補 `T` と通常の materialized color target `C` を出力する。
2. pass B が `T` を same-pixel local read し、同じ `C` を `color_load_op = CLEAR` で再出力する。
3. `T` の edge により A/B は fuse されるが、`local_reads` があるため `C` の attachment operation 互換性検証が省略される。
4. runtime は `C` に A の load op と B の store opだけを採用し、A/B 間では draw 前に B の clear を実行しない。
5. B は宣言上 clear 済みの `C` ではなく A の出力上へ描画し、以後の consumer に誤った内容が渡る。中間 `DONT_CARE` / `STORE` の契約差も同様に失われる。

`src/core/vkcore/render_pass_executor.cpp:429-457` の BY_REGION dependency は tile-local producer write→input read の同期自体を満たすが、attachment operation の意味を実行しない問題は解消しない。

- 症状: validation error を伴わない決定的な合成ミス、clear 色の消失、後段 pass の入力内容破損。
- 必要な修正方向: local-read scope でも materialized attachment ごとの中間 operation 契約を検証する。単一 rendering instance で表現不能な CLEAR/DONT_CARE/attachment-set 変更があれば fusion を拒否するか、同値な explicit clear 等へ lower する。

## LOW 確度の要確認事項

### LOW-01 — swapchain 再作成時の `device.waitIdle()` は presentation 完了を仕様上は証明しない

- 重大度: **LOW**
- 確度: **LOW**
- 根拠箇所:
  - `src/core/vkcore/swapchainframetarget.cpp:198-203` —再作成前の同期は `device.waitIdle()` だけである。
  - `src/core/vkcore/swapchainframetarget.cpp:159-170` —直後に image view と旧 swapchain を破棄する。
  - `src/core/vkcore/swapchainframetarget.cpp:429-451` —直前まで旧 swapchain への `presentKHR` が存在し得る。
  - リポジトリ内に `VK_EXT_swapchain_maintenance1` / presentation fence の利用は見つからなかった。

具体的な疑わしいシーケンス:

1. 旧 swapchain への present が presentation engine/queue に残る。
2. resize の `OUT_OF_DATE` 経路が `recreateSurfaceDependants()` を呼ぶ。
3. `device.waitIdle()` は fence を受け取る workload の完了を待つが、unextended `vkQueuePresentKHR` の完了 fence はない。
4. `surfaceDependantsSetup()` が旧 swapchain を破棄する。

Khronos guide も、extension なしでは WaitIdle による presentation resource 破棄安全性は理論上保証されず、実用上広く使われる妥協であるとしている。このため確定 HIGH には含めない。

- 症状: maintenance1 有効時の validation、稀な WSI/driver 依存の破棄中使用。
- 確認/修正方向: 対応環境では `VK_EXT_swapchain_maintenance1` の present fence を使用する。未対応環境の fallback 方針は platform/driver の保証と合わせて文書化する。

## 監査済み・問題なしと判断した範囲

以下は上記指摘を除き、planner の宣言と実コマンドの整合を確認できた。

### Alias group

- `src/project/targetrenderplanning.cpp:1074-1089` は canonical node 順の全 use から lifetime を構築し、`src/project/targetrenderplanning.cpp:2459-2492` は lifetime 非重複、representation、format、sample、view layout、array layers、extent の一致を要求する。
- `src/project/targetrenderplanning.cpp:2526-2606` は group 追加/merge 時に全 member pair の合法性を要求する。
- `src/core/renderingpass/rendertargetcontainer.cpp:404-410` は alias を materialized・non-history・single-sample に制限し、`src/core/renderingpass/rendertargetcontainer.cpp:456-510` は物理 contract の完全一致後に aliasing image を作る。
- `src/core/vkcore/render_target_layout_tracker.cpp:135-166` は alias member の切替時に `ALL_COMMANDS` / `MEMORY_READ|MEMORY_WRITE` の global memory barrier を発行し、旧/新 member の layout を `UNDEFINED` に無効化する。`src/core/vkcore/render_target_layout_tracker.cpp:251-260` は command-buffer/frame reset をまたぐ alias group も次回 dependency 必須として保持する。

このため、合法 group 内の alias WAR/WAW と layout tracker の実 allocation 切替には必要な dependency がある。VKSYNC-04 は同一 image の通常 WAW であり、この alias 切替処理とは別問題である。

### Transient / tile-local の基本 locality

- `src/project/targetrenderplanning.cpp:1984-2002` は tile-local を same-pixel、attachment access、single-sample、non-history/non-export/non-storage 等へ限定する。
- `src/project/targetrenderplanning.cpp:2027-2050` は transient を write-only、virtual、store 不要、single-sampleへ限定する。
- `src/project/targetrenderplanning.cpp:1674-1778` は transient/tile-local read edge が同一 fused scope 外へ出る候補を拒否し、tile read use 数と edge 数も照合する。
- `src/core/renderingpass/rendertargetcontainer.cpp:340-358` は transient image usage を attachment only に制限して `eTransientAttachment` を付ける。
- `src/core/vkcore/render_pass_frame_setup.cpp:423-449`、`src/core/vkcore/render_pass_frame_setup.cpp:473-498` は local-read usage と LOAD 元内容を検証してから `eRenderingLocalReadKHR` へ遷移する。
- `src/core/vkcore/render_pass_executor.cpp:429-457` は fused pass 間に BY_REGION の attachment-write→input/attachment dependency を発行する。

したがって単純な tile-local producer→consumer の store/read 生存と transient の scope 外利用拒否は整合している。VKSYNC-06 の「同じ fused scope に別 materialized attachment の異なる operation が同居する場合」だけが例外である。

### Multiview

- `src/project/targetrenderplanning.cpp:1856-1918` は multiview write または sequential/multiview 境界の resource を layered 2D array とし、`array_layers = view_count` にする。
- `src/core/vkcore/render_pass_executor.cpp:49-64` は 2..32 の view count、contiguous view mask、単一 multiview invocation を検証する。
- `src/core/vkcore/render_pass_executor.cpp:77-107` は非 shared output/input target の array layers が view count 以上であることを検証する。
- `src/core/vkcore/render_pass_frame_setup.cpp:29-47` は multiview では layered image view、sequential では view ごとの layer view を選ぶ。
- `src/core/vkcore/render_pass_executor.cpp:314-318`、`src/core/vkcore/render_pass_executor.cpp:372-377` は dynamic rendering で `layerCount = 1` と compiled `viewMask` を設定する。
- `src/core/renderingpass/renderingpassruntimecompiler.cpp:838-858` は multiview pass が sequential-only input を消費する契約を拒否する。

view mask、array layer、view state 切替の間に追加の同期/状態不整合は見つからなかった。

### Acquire-submit-present のうち正常な部分と submission lease

- `src/core/vkcore/frametarget.hpp:25-50` は slot ごとに一つの generation lease を保持する。
- `src/core/vkcore/swapchainframetarget.cpp:244-255` は acquire semaphore/command buffer slot の再利用前にその submit fence を待ち、以前の lease を解放する。
- `src/core/vkcore/swapchainframetarget.cpp:421-427` は submit 成功直後、present より前に lease を保持するため、present の `OUT_OF_DATE` でも submit が参照する generation は残る。
- `src/core/vkcore/offscreenframetarget.cpp:211-231` も submit 後の fence 完了まで lease を保持する。
- `src/core/vkcore/swapchainframetarget.cpp:257-284`、`src/core/vkcore/swapchainframetarget.cpp:434-451` は vulkan-hpp の `OutOfDateKHRError` を result へ戻し、既存の再作成分岐へ到達させている。

したがって通常 submit の `submission_leases` と acquire semaphore の slot 再利用には fence 対応がある。問題は VKSYNC-01 の present semaphore と、VKSYNC-02 の「submit されなかった slot」に限定される。

## 最終判定

修正優先順位は、(1) present semaphore を acquired image 単位にする、(2) frame acquire 後の例外 rollback を保証する、(3) DeletionQueue を GPU completion serial へ接続する、(4) WAW edge と depth stage scope を修正する、(5) local-read fusion の operation contract を閉じる、の順を推奨する。少なくとも VKSYNC-01〜05 が解消されるまでは、本経路を Vulkan 同期上安全とは扱えない。
