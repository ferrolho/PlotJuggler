# Scene3D WebAssembly PR stack — reviewer guide

This is a temporary review aid for the three-PR Scene3D WebAssembly stack. It
is introduced by PR 1, updated by PR 2, and deleted by PR 3 once the complete
design is represented by the permanent architecture and requirements docs.

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
3. **PR 3 — models, SceneEntities, and rendering quality**
   Branch: `feat/wasm-scene3d-models`.
   Initial base: PR 2; retargeted to `main` after PR 2 merges.

Code, tests, fixtures, CI, and documentation travel with the capability they
validate. There is intentionally no tests-only or dependencies-only PR.

## Current slice: PR 1

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

### PR 1 review focus

- Desktop safety: the native `QOpenGLWidget` branch and native dependency graph
  must remain unchanged.
- Resource isolation: two live QRhi widgets must not share mutable GPU state.
- Layout honesty: a generic browser layout must contain no upload URI/path or
  numeric dataset qualifier; ambiguous rebinding must fail closed.
- Test ROI: browser tests should prove integration seams and framebuffer
  transitions, not duplicate parser/budget unit tests or compare PNGs.
- Dependency altitude: the foundation must not fetch model or compressed-cloud
  libraries.

## Planned PR 2

PR 2 adds the bounded sensor-data path:

- raw PointCloud2, Cloudini, Draco, depth cloud, poses, occupancy grid/update,
  and voxel grid;
- asynchronous decode/coalescing, timeline seek, layer configuration,
  deterministic ordering, and generic-layout replay;
- CPU-side budget/codec tests and only the shaders/resources needed by these
  layers;
- Draco/Cloudini fetched only when
  `PJ_WASM_WITH_COMPRESSED_POINTCLOUDS=ON`.

The acceptance band will be split by behavior: point/depth decoding, structured
layers, and heterogeneous ordering. Fixture generators will share common
ROS2/MCAP utilities and commit minimal native `.mcap` files with reproducible
provenance. PR 2 will update this document with its actual status and PR 3
review risks.

## Planned PR 3

PR 3 completes the product path:

- URDF/robot descriptions, bounded URL/package resolution, Assimp mesh import,
  SceneEntities, procedural markers, axes, and PBR materials;
- shadows, HDR presentation, SSAO, and EDL with behavior-preserving fallbacks;
- focused parser/model tests and compact URDF/model end-to-end scenarios;
- final permanent architecture and requirements documentation.

Assimp enters only this PR. PR 3 deletes this temporary guide after verifying
that the stacked tree is behaviorally equivalent to the reference integration
commit, apart from the documented reviewability changes.

## Cross-stack invariants

- No local patch queue for official plugins. Plugin fixes belong in
  `pj-official-plugins`; PR #235 already supplied the required changes.
- No PNG snapshot assertions. Evidence comes from public state, submitted
  geometry, framebuffer digests/transitions, real input, and layout replay.
- No MCAP parse-option UI restoration.
- No new public SDK/plugin API; WASM implementation details remain private to
  the Scene3D targets.
- The production feature remains opt-in until the complete stack lands.
