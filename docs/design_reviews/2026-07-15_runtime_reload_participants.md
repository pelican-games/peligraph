# Runtime reload participant unification

Date: 2026-07-15

Status: implemented and regression-tested

Update: HR2-S was completed by WP108 later on 2026-07-15. The shader participant is now
file-backed by `FileWatcher`/`ContentDigest`; its render-start callback only consumes a committed
publication for renderer rebinding or performs an explicit forced reload. See
`2026-07-15_wp108_report.md` for the superseding shader transaction details.

## Outcome

`ReloadService` is now the single orchestration boundary for file-backed resource reload,
shader/pipeline reload, and game-logic DLL reload. The service owns when a participant may run,
why it was triggered, exception containment, counters, and status publication. Each participant
continues to own its domain-specific candidate, commit, rollback, and retirement mechanics.

This avoids two bad outcomes: duplicating frame/gate/error logic at every call site, and forcing
Vulkan objects or DLL handles into the logical asset registry when their lifetimes do not fit it.

## Contract

A named `ReloadParticipant` may expose either or both capabilities:

- File-backed resources: `claims`, `enqueue`, and optional `retire`, committed by
  `ReloadCoordinator` at frame start.
- Runtime transactions: a declared `frame_start` or `render_start` boundary, an `apply` callback,
  and an optional status descriptor.

Runtime callbacks receive one of three triggers: periodic `poll`, queued developer `requested`, or
synchronous `manual`. They return `attempted`, `committed`, and an error string. `ReloadService`
normalizes exceptions into the same result and records per-participant attempted/applied/failed
counters plus the last error.

## Built-in participants

| Participant | Boundary | Transaction owner | Failure/retirement behavior |
|---|---|---|---|
| `pelican.materials` | frame start | `ReloadCoordinator` + material handlers | parse/validate/stage as a group; failed candidates do not publish; old GPU payloads use the participant retire sink |
| `pelican.game_logic` | frame start | `GameLogicReloader` | shadow-copy and ABI validation precede teardown; load/rebuild failure reloads the previous DLL; obsolete shadow copies and registrations are retired |
| `pelican.shaders` | render start | `ShaderLibrary` + `PipelineFactory` | failed shader candidates keep the old bundle; failed pipeline candidates keep the old pipeline; replaced pipelines/layouts go through `DeletionQueue` |

Game-logic automatic polling remains interactive-only. Replay/golden drivers suppress queued and
periodic reloads, while headless/RPC drivers change DLLs only through the explicit RPC command.
F5 now queues a forced reload for the next frame boundary instead of mutating runtime state after
the game-update phase. A successful or failed forced DLL attempt acknowledges the observed source
timestamp so the following poll cannot apply the same file twice.

Shader publication is prepared and committed at frame start from the watcher batch. Its
render-start callback consumes the publication before command recording so fullscreen descriptor
rebinding remains at the correct boundary. `ShaderLibrary` has no independent timestamp poll.

## RPC and status

`reload_game_logic` invokes `ReloadService::applyRuntimeNow("pelican.game_logic")`; it no longer has
a parallel reload path. `get_status.reload.runtime` publishes each runtime participant's boundary,
queued state, counters, last error, and domain details. Successful attempts clear the participant's
last error, while idle polls preserve it for diagnosis.

## Regression gates

- Runtime participant unit coverage fixes boundary selection, queued/manual trigger semantics,
  exception/error accounting, and status JSON.
- Shader tests cover structured success and invalid-candidate failure while retaining the old
  bundle and version.
- The Vulkan fullscreen fixture covers shader compile, pipeline rebuild, and descriptor rebind.
- The DLL integration fixture covers two successful generations, invalid PE and bad-ABI rollback,
  continued old behavior, unified counters, and error clearing.
- Replay coverage verifies manual rejection and silent suppression of a queued boundary reload.
- A source-architecture test rejects reintroduction of direct Shader/GameLogic reload calls in the
  renderer, loop, or RPC server, including direct pipeline rebuilding from the renderer.

## Verification

- Debug: complete build succeeded; CTest reported 0 failures across 374 tests. The Win32
  symlink-escape case was the single expected platform skip.
- Release: complete build succeeded; the runtime-participant, game-logic DLL, and fullscreen
  shader-rebind regressions passed.
- Release player: a two-frame `64x64` headless run of `projects/example` exited successfully.
- `git diff --check` reported no whitespace errors (only the repository's existing LF/CRLF
  conversion notices on Windows).

## Superseded remaining boundary

The all-variant shader dependency transaction deliberately left by this refactor was closed by
WP108. Every affected shader variant, reflection, dependent pipeline, and participating material
layout is now prepared before one group-wide publication.
