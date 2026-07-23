# Scene3D WebAssembly PR stack — reviewer guide

This is a temporary review aid for the four-PR Scene3D WebAssembly stack. It
is introduced by PR 1, updated as each capability lands, and deleted by PR 4
once the complete design is represented by permanent architecture and
requirements docs.

## Goal and reference implementation

The stack ports the existing Scene3D product to Qt's `QRhiWidget`/WebGL2 path
without changing the desktop OpenGL implementation. The fully integrated,
locally validated reference is commit `25b7b0f1` on the local backup branch
`backup/wasm-scene3d-integration-25b7b0f`. The stacked result may differ from
that reference only through deliberate reviewability improvements: smaller
translation units, dependency scoping, semantic acceptance tests, consolidated
fixtures, and removal of screenshot baselines.

Scene3D stays disabled by default (`PJ_WASM_WITH_SCENE3D=OFF`) throughout the
stack. Every PR must nevertheless configure, build, and test its enabled subset
independently.

## Stack topology

1. **PR 1 — QRhi foundation, TF/grid, and generic layouts**
   Branch: `feat/wasm-reboot` (PR #432).
   Base: `main` after WASM platform PR #456.
2. **PR 2 — sensor-data layers**
   Branch: `feat/wasm-scene3d-data`.
   Initial base: PR 1; retargeted to `main` after PR 1 merges.
3. **PR 3 — SceneEntities, models, PBR, and URDF**
   Branch: `feat/wasm-scene3d-models`.
   Initial base: PR 2; retargeted to `main` after PR 2 merges.
4. **PR 4 — shadows and rendering quality**
   Branch: `feat/wasm-scene3d-quality`.
   Initial base: PR 3; retargeted to `main` after PR 3 merges.

Code, tests, fixtures, CI, and documentation travel with the capability they
validate. There is intentionally no tests-only or dependencies-only PR.

## Published base: PR 1

PR 1 establishes only the platform and interaction foundation:

- retained WebGL2/QRhi capability probe, including two simultaneous widgets,
  vertex-stage 3D texture access, floating-point targets, instancing, and
  32-bit indexed geometry;
- product `SceneViewWidget`/`Scene3DDockWidget` platform selection for WASM;
- camera models and real pointer/wheel input;
- grid, frame axes/connections, TF ingestion, timeline seek, and fixed-frame
  selection;
- generic-layout export that removes browser dataset identities and restores a
  Scene3D source only when topic and object type have one unambiguous match;
- one semantic product acceptance scenario plus focused layout unit tests.

PR 1 deliberately has no Assimp, Draco, Cloudini, network model loading,
sensor-data layer classes, SceneEntities, robot models, shadows, HDR, SSAO, or
EDL. Their absence is a review boundary, not a missing implementation.

Its retained capability probe and focused product/layout acceptance scenario
are the baseline for this PR. PR 2 does not replace or weaken those checks.

## Published base: PR 2

PR 2 adds the bounded sensor-data path:

- raw PointCloud2, Cloudini, Draco, depth cloud, poses, occupancy grid/update,
  and voxel grid;
- asynchronous compressed/depth decode with latest-request coalescing,
  timeline seek, retained GPU uploads, per-layer and per-view budgets, and
  deterministic heterogeneous submission order;
- generic layer replay by unique topic plus object type, failing closed on an
  ambiguous match;
- Draco and Cloudini only when
  `PJ_WASM_WITH_COMPRESSED_POINTCLOUDS=ON`, isolated in
  `cmake/PjWasmScene3DDependencies.cmake`.

The browser acceptance delta is three behavior scenarios, not one test per
class or fixture: point/depth worker convergence, occupancy/voxel bounded
updates, and physical mixed-family reorder plus generic-layout replay. CPU-side
budget/codec tests retain exhaustive edge cases. The browser probes report
submitted geometry and decoder lifecycle from a dedicated test translation
unit; there are no PNG comparisons.

### PR 2 review focus

- Desktop safety: the native `QOpenGLWidget` branch and native dependency graph
  must remain unchanged.
- Decode lifecycle: a stale worker result must never replace the latest tracker
  request, failures must clear only their own layer, and work must complete off
  the browser main thread.
- Budget altitude: malformed or over-limit inputs must be rejected before large
  retained allocations; the view budget is charged only for drawable layers.
- Ordering: the Settings list, saved XML order, and actual QRhi command
  submission must remain identical across mixed families.
- Dependency boundary: no Assimp/model importer, SceneEntities, PBR, or
  post-processing code belongs in this slice.
- Layout honesty: generic layer XML contains no browser dataset identity and
  ambiguous topic/type rebinding fails closed.

## Current slice: PR 3

PR 3 adds the bounded content and model path:

- URDF/robot descriptions, bounded URL/package resolution, Assimp mesh import,
  SceneEntities, procedural markers, axes, and five-map PBR materials;
- browser file-source semantics that persist a logical name but never claim
  durable access to host bytes;
- focused parser/model tests plus two compact product scenarios for mixed
  SceneEntities/PBR and local-URDF layout replay.

Assimp enters only this PR. Shadows, HDR presentation, SSAO, EDL, their shader
packs, and their resource/fallback probes remain absent so reviewers can first
audit which geometry reaches the direct renderer.

### PR 3 review risks

- Model sources are browser content capabilities, not durable local paths;
  URL/package resolution must be bounded and layout restore must not claim a
  local file can be reopened automatically.
- Assimp must be fetched only for Scene3D and only in PR 3.
- SceneEntities and robot models must reuse the PR 2 ordering, TF, budget, and
  generic-layout contracts rather than introduce parallel mechanisms.
- Model import and procedural-marker rejection must be charged before retained
  CPU/GPU allocations, with stale asynchronous results ignored.

## Planned PR 4

PR 4 completes rendering parity with mesh shadows, HDR presentation, SSAO, and
EDL. It extracts their resource lifecycle from the main QRhi scene-view source,
adds independent failure/recovery acceptance, and replaces this temporary guide
with final permanent architecture and requirements documentation.

### PR 4 review risks

- Every quality pass must fail independently while the direct renderer stays
  live; clearing a forced or capability failure must rebuild resources.
- Per-view and shared-QRhi resource ceilings must be checked before allocation.
- Depth replay, mesh-only EDL masking, shadow fitting, and HDR presentation must
  reuse PR 3 model submissions instead of creating a second scene graph.
- The native OpenGL implementation and dependency graph must remain unchanged.

## Cross-stack invariants

- No local patch queue for official plugins. Plugin fixes belong in
  `pj-official-plugins`; PR #235 already supplied the required changes.
- No PNG snapshot assertions. Evidence comes from public state, submitted
  geometry, framebuffer digests/transitions, real input, and layout replay.
- No MCAP parse-option UI restoration.
- No new public SDK/plugin API; WASM implementation details remain private to
  the Scene3D targets.
- The production feature remains opt-in until the complete stack lands.
