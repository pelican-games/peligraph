# WP210a typed indirect compute dispatch 実装記録

日付: 2026-07-27
状態: 実装・CPU契約テスト・実Vulkan headless検証済み

## 完了境界

WP210を一度にrenderer全体へ広げず、最初のvertical sliceとしてGPUが同一フレーム内に
生成した3次元group countで次のcompute taskを起動する経路を閉じた。

- buffer authoringへ`command_layout: "compute_dispatch"`を追加した
- layoutは`uint x, y, z`の12 byte command contractであり、raw Vulkan usageではない
- physical allocation時だけ`eIndirectBuffer`を付与する
- compute taskへ`dispatch.indirect.buffer`とoptional `offset`を追加した
- indirect command resourceはshader descriptorとは分離しつつ、frame graphのread edgeへ
  自動追加する
- producerのcompute shader writeからconsumerのindirect command readへ
  `ComputeShader/ShaderWrite → DrawIndirect/IndirectCommandRead` barrierを発行する
- runtimeはdirect taskを`dispatch`、indirect taskを`dispatchIndirect`へ分岐する

## 起動前検証

CPU compilationで次をhard errorにする。

1. `dispatch.indirect`と`groups` / `groups_from` / `local_size`の混在
2. unknown buffer
3. `command_layout: "compute_dispatch"`がないbuffer
4. 4 byte alignmentに合わないoffset
5. offsetから12 byteのcommandがbuffer外へ出る構成
6. consumerが自身のindirect command bufferを書く、frame間feedbackが暗黙になる構成

この検証はGPU resource登録前に実行する。runtime registrationも同じ不変条件を再確認するため、
直接C++ APIから構築したdefinitionでも不正なcommand bufferを実行しない。

## dogfoodと検証

既存のcompute headless fixtureを次の依存へ変更した。

```text
build_dispatch ── writes dispatch_arguments
                         │ automatic RAW edge + indirect barrier
                         ▼
transform_color ── vkCmdDispatchIndirect
```

`build_dispatch.comp`が`{1,1,1}`を書き、従来direct dispatchだったconsumerを
indirectへ切り替えた。最終画像の意味は変えず、追加fixtureや大きな生成物を増やしていない。
golden harnessの`compute_buffer`も同じ経路を使う。

実行したgate:

```text
pelican_test_renderingpass_helpers_test.exe "[wp210]"
ctest -C Debug -R ^compute_buffer_headless_player$ --output-on-failure
pelican_test_golden_cases_test.exe "golden image cases match expected output" --section compute_buffer
```

契約テストは20 assertions、headless playerは実Vulkan上で成功し、絞り込んだgolden
sectionも既存`expected.png`と一致した。

## 意図的な残件

これはWP210全体の完了ではない。次のWP210bではrenderer側のGPU-written indexed
draw arguments/countを、既存CPU draw queueのfallbackを保ったまま追加する。
count 0/1/max/overflow、hot reload rollback、XR view count、CPU fallbackとのsemantic image一致、
GPU timingで有利になるworkload範囲はWP210b以降の受け入れ条件として残す。
