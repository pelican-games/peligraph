# pv1_history_accum

PV-1: a direct rendering root whose authored render target `accum` keeps
history. `accumulate` adds 1/16 to last frame's `accum` every frame and
`present` shows it, so successive frames differ. Normal play runs it; the
one-shot preview (`render_preview`) refuses authored history, so the engine
starts with preview unavailable (`preview.unsupported_authoring@1`, feature
`history`, target `accum`) instead of refusing the project.
