# pj_app

## Purpose

The executable shell. Owns `MainWindow`, the application menus/toolbars/status bar, the `AppSession` (from `pj_runtime`), and the wiring between runtime services and concrete widgets (`pj_plotting`, `pj_scene2D/widgets`, …). On WebAssembly it also owns the browser boundary: upload staging/identity, asynchronous file selection, the deliberately narrow durable-settings bridge, and source-aware layout replay.

## What belongs here

- `MainWindow` and its `.ui` file.
- App-level dialogs (`PreferencesDialog`, `DiagnosticsDetailDialog`, etc.) and their navigation rows.
- The shell's left panel, curve list, file loader, theme manager, title bar.
- Glue code that constructs an `AppSession`, then wires it to docked widgets.
- Browser-only host integration (`BrowserFileStore`, `FileSelectionService`,
  `BrowserPersistence`) whose lifetime and policy are tied to the executable.
- `main.cpp`.

## What does NOT belong here

- **Reusable Qt controls** → `pj_widgets`.
- **Business logic / data services** → `pj_runtime`.
- **Plotting / 2D / 3D widgets** → the owning widget-family module.
- **Plugin dialog runtime** → `pj_dialog_host`.

If you're tempted to add a class here that could be reused by another Qt app, move it to `pj_widgets` instead.

## UI convention

Per root CLAUDE.md: **prefer `.ui` files** over programmatic widget construction. `pj_app/CMakeLists.txt` uses `AUTOUIC`. Drop to hand-written `QWidget` subclasses only for genuinely dynamic construction (plugin-driven widgets) or when explicitly requested.

## Layout

- `src/` — top-level shell sources and root `.ui` files (`MainWindow.ui`, `PreferencesDialog.ui`, `TitleBar.ui`).
- `src/ui/` — embedded shell sub-widgets: left-panel widgets (`CurveListPanel`, `LeftPanel`, `DiagnosticsCard`, `DiagnosticsPopup`, `DiagnosticsDetailDialog`), the bottom timeline strip (`TimelineWidget`), and the right-sidepanel scene config panels (`Scene2DConfigPanel`, `Scene3DConfigPanel`).
- `tests/wasm_acceptance_probes.inc` — acceptance-build-only observation and
  input bridge, included behind `PJ_WASM_ENABLE_INGRESS_PROBE`; it is never part
  of desktop or production WASM behavior.

`pj_app` has no `docs/` folder by design — the shell's intent is "wire the services to the widgets," and the wiring is best read directly from `MainWindow.cpp` and `main.cpp`.

## Browser persistence boundary

Native builds retain their normal `QSettings` behavior. WASM redirects ordinary
`QSettings` callers to page-lifetime MEMFS and lets `BrowserPersistence` alone
write `WebLocalStorageFormat`. Only its exact, typed preference allowlist and up
to five bounded, source-free generic layout recipes are durable. File paths,
upload identities, plugin configuration, credentials, and source-bound layouts
must never enter durable browser storage. If the active preference envelope is
unreadable, this version leaves it intact rather than deleting data that a newer
version may understand; it never imports rejected values into the session.
