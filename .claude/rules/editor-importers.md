---
paths:
  - "editor/src/*import*"
  - "editor/src/blender*"
  - "editor/src/assimp*"
  - "editor/src/gltf*"
  - "editor/src/fbx*"
  - "editor/src/obj*"
  - "editor/include/aero/editor/*import*"
  - "editor/include/aero/editor/blender*"
  - "tests/editor/*import*"
  - "tests/editor/blender*"
  - "tests/editor/assimp*"
  - "tests/editor/fbx*"
  - "tests/editor/obj*"
  - "editor/src/tool_process*"
  - "editor/src/esbuild*"
  - "editor/src/script_transpiler*"
  - "editor/include/aero/editor/esbuild*"
  - "tests/editor/esbuild*"
  - "tests/editor/script_transpiler*"
---

# Editor: model importers

Split out of `editor.md` so that no editor rules file is cut at the 150 000-character load limit. A reference to another section may point into a sibling `editor*.md` file.

## Model import (task 3.2.1)

- **fastgltf NEVER touches the filesystem; the editor supplies every byte (D3).**
  `GltfDataBuffer::FromBytes` over bytes `readFileBytes` already read is the ONLY construction path.
  `Options::LoadExternalBuffers`, `Options::LoadExternalImages`, `GltfDataBuffer::FromPath`,
  `MappedGltfFile` and `GltfFileStream` must NEVER appear anywhere in this tree -- a `.gltf` is a
  user-supplied document from the internet, and `"uri": "file:///etc/passwd"` is syntactically legal
  glTF that fastgltf's own `isLocalPath()` would accept. With the editor resolving, every URI passes
  `classifyUri` first and a refusal is a WARNING, not a read.
- **`classifyUri` receives an ALREADY-PERCENT-DECODED URI.** `fastgltf::URI` decodes on construction
  (`src/fastgltf.cpp`: `URI::URI` calls `decodePercents`). Do not decode again -- `100%2520.png` would
  become `100 .png`. Decode-then-classify is also the SECURE order: an encoded scheme (`%68ttp:`) is
  already `http:` when we classify it.
- **THE IMPORTER CONVERTS NOTHING, and this is the easiest thing in the task to get catastrophically
  wrong.** `aero/core/math.hpp`'s own header comment says the engine's conventions were chosen to match
  glTF 2.0 and names importers as one of the four consumers that inherit them. Verified at the bit
  level against fastgltf 0.9.0: `math::quat` is `{x,y,z,w}` defaulting to `{0,0,0,1}`; `math::mat`
  stores COLUMNS. **No handedness flip, no axis swap, no winding reversal, no quaternion reorder, no
  transpose, no degree conversion.** There is no `convertFromGltfSpace()`, and `MI40b` plus sabotage
  seeds S29/S30 exist to prove that adding one is caught. **3.2.2 (FBX: Z-up, centimetres) IS the task
  that needs a conversion**, precisely because glTF is the canonical format these types were built
  around.
- **Every accessor is validated BEFORE any fastgltf tool touches it.** fastgltf `assert`s on a type
  mismatch (a Debug abort in our lanes), dereferences a possibly-disengaged `Optional` for a sparse
  accessor with no `bufferView` (its own header says that case is real), and never bounds-checks the
  accessor -> bufferView -> buffer chain; `fastgltf::span` is unchecked and
  `DefaultBufferDataAdapter` range-checks nothing. `validateAccessor` is what makes a hostile document
  cost a warning instead of a crash or an out-of-bounds read.
- **The custom buffer-data adapter must apply the bufferView's own `byteOffset`/`byteLength`.**
  fastgltf's callers apply only `accessor.byteOffset` on top. An adapter that returns the whole buffer
  reads from the wrong offset with NO error -- plausible-looking garbage geometry.
- **`Structure` and `Full` are ONE function with a depth parameter (INV-M4).** They can never disagree
  about the URI set, the counts, the names or the hierarchy, because they compute them with the same
  code. The SCAN runs `Structure` only, budgeted, and only for assets already in `plan.jobIndices`; the
  PANEL runs `Full`, on demand, for one asset, in two passes.
- **A scan still writes ZERO bytes to a fully-described tree (D7).** The `importer` block is written by
  exactly ONE code path -- `ModelImportSession::applySettings`, reached from the panel's Apply button,
  driven by a person who changed a value. The scan never mints, upgrades or normalizes a block.
  `writeMetaText(guid, ImportSettings{})` is byte-identical to `writeMetaText(guid)` BY CONSTRUCTION
  (the one-argument overload delegates), so `minimal.meta`'s 65-byte fixpoint keeps passing unedited --
  and THAT is the evidence this task did not disturb the committed format.
- **`.meta` STAYS AT VERSION 1 (D6).** A v2 bump makes an older build nil EVERY GUID in the project
  (`parseMeta` rejects the version -> `Invalid` -> nil guid; D7 then forbids repairing it), with no
  recovery path that does not involve hand-editing files. An additive optional key degrades instead.
- **A malformed `importer` block NEVER invalidates an identity (AC-12/INV-M11).** `error` stays
  `MetaError::None`, `guid` stays engaged, and the failure surfaces only through `importer`
  (disengaged) + `importerMessage` (non-empty). Version and guid are IDENTITY; the block is PREFERENCE.
- **`ImportInput::probe` engagement is the "was this probed?" signal (D9).** A disengaged probe makes
  `commitImports` carry the PREVIOUS entry's importer, importerVersion and dependencies forward
  verbatim. Passing `""`/`0` instead would flip the importer, make `planImports` report
  `ImporterChanged` next scan, flip it back, and oscillate FOREVER.
- **The probe records THIS scan's dependencies for the NEXT scan's cascade (D8/E11).** It runs after
  `planImports` and before `commitImports`. A brand-new model plus its brand-new texture cascade
  correctly from the SECOND scan onward -- correct, since both are `New` on the first scan anyway.
  **A test that edits a texture immediately after adding a model must tick TWICE.**
- **`dependencies` never contains a nil GUID, is sorted, is deduplicated, and drops self-edges
  (INV-M8/E9).** A nil would make the cascade's `find` return `nullptr` for a reason unrelated to the
  graph; a self-edge would make the worklist treat the node as permanently dirty.
- **The Full import runs from `EditorApp::tick()` only, never from `onDraw()` (INV-M12).** Like
  3.1.3's BLOCKING-1 and 3.1.4's D9, **no automated test tier can see the general-case violation** --
  `I60` reads `editor_app.cpp`'s own source text and manual row 8 is the only behavioural cover. Get it
  right by construction.
- **`"Import Details"` is FROZEN.** It is the ImGui window name AND the `imgui.ini` settings key.
  Renaming it orphans every user's saved layout for that panel.
- **Editing import settings is NOT undoable, and that is a decision (D19).** `CommandStack` is
  scene-scoped by construction and is cleared on every World swap; a file edit does not belong in it.
  Apply is a plain, explicit, single-shot atomic write, and the file itself is the record. This is the
  same posture the Project Settings panel takes toward `project.json` (2.6.2 D6).
- **`extensionsRequired` fails the parse INSIDE fastgltf, before we see the asset.** Both
  `Error::MissingExtensions` and `Error::UnknownRequiredExtension` map to
  `ImportStatus::MissingExtension`, and neither error names the extension -- **but only one of them is
  unrecoverable.** `MissingExtensions` means fastgltf KNOWS the extension and it was merely not enabled,
  so a **throwaway re-parse with `~fastgltf::Extensions::None`** recovers the names; the `Asset` is
  discarded, so D20 is intact. `UnknownRequiredExtension` means fastgltf does not know it at all, and no
  setting changes that. **`Extensions::All` does not exist in 0.9.0** -- that `All` belongs to
  `Category`. See §A-6 and §D's `describeRequiredExtensions`.
- **`gltf_import.cpp` sees `<filesystem>` transitively and cannot avoid it** (`Parser::loadGltf` takes
  a `std::filesystem::path` positionally). The rule that IS true and IS checked: **it performs no file
  operation at all.** `directory` is `{}`, which fastgltf explicitly supports when no `LoadExternal*`
  option is set.
- **`Options::DecomposeNodeMatrices` destroys the fact that a node used a `matrix`.** The AC-19 warning
  is therefore a deliberate, labelled HEURISTIC: one `find` for `"\"matrix\""` over the source bytes,
  producing ONE aggregate warning per document. A false positive costs one warning; a false negative
  costs none; neither affects correctness.
- **UPSTREAM DEFECT, do not trip it:** `fastgltf::URI::decodePercents` reads out of bounds on a URI
  ending in `%` (`chars = {x[i+1], x[i+2]}` with `i == size()-1`). No fixture or test in this tree may
  contain one. `%zz` (three characters) is in range and is the form used for the control-character
  refusal case.
- **The panel's service() call site has NO automated general-case cover, the identical
  `serviceThumbnails()` shape 3.1.3 and 3.1.4 both already carry.** `ModelImportSession::service()` runs
  from `EditorApp::tick()`'s post-draw slot, the THIRD occupant after `renderScene()` and
  `serviceThumbnails()` -- never from `ImportDetailsPanel::onDraw()`. `I60` proves it the only way this
  target can: reading `editor_app.cpp`'s own source text (comment-stripped) and asserting the ONE call
  site sits textually AFTER `drawShellUi(`, the single call that invokes every panel's `onDraw()`. That
  is a proof about THIS file's current text, not a structural guarantee against a future refactor that
  moves the call somewhere else that still reads as `editor_app.cpp` -- manual validation row 8 (ten
  models selected one after another, no stutter) is the only behavioural cover, exactly as it is for
  `serviceThumbnails()` and the watcher's superseded-texture sweep.
- **`AssetBrowserPanel::selectedEntry` needed a SECOND public seam beyond `selection()` to be drivable
  from the ImGui-free-at-source GPU tier, not anticipated by the plan's own literal count.**
  `requestSelectEntry` (panel) / `requestAssetBrowserSelectEntry` (`EditorApp`) are the
  code-review-finding-4 shape (`requestViewMode`/`requestSearchQuery`/`requestKindFilter`/
  `requestDeleteOrphanClick`), a fifth application: each records EXACTLY what a real single click on a
  row/tile records (`ActionKind::SelectEntry`). Without it, `ModelImportSession`'s whole reconcile path
  has no way to be driven from `imgui_layer_test.cpp` at all. **A case that calls it still needs
  `AssetBrowserPanel::onDraw()` to actually RUN for the click-equivalent action to be drained at all** --
  "Assets" shares `DockSlot::Bottom` with "Console" (registered first, so it wins the tab by default),
  so every such case calls `app->panels().setVisible("Console", false)` first (2.2.4's C5 precedent,
  restated for a sixth reason).

## FBX import (task 3.2.2)

- **ufbx is VENDORED at `editor/third_party/ufbx/` and is BYTE-IDENTICAL TO UPSTREAM v0.23.0. No local
  patches, ever.** It is in no vcpkg registry (measured twice), so vendoring is not a preference. The
  decisive property: `ufbx.c` compiles with THIS project's directory-scope options, so it is
  ASan/UBSan-instrumented in all three Debug lanes -- the single most valuable place in this tree for a
  sanitizer to be watching, since ufbx parses untrusted binary files. A needed fix goes upstream or
  into a wrapper in `fbx_import.cpp`. A patch here must be re-applied by hand at every bump, correctly,
  with its reason invisible at the call site.
- **ufbx NEVER touches the filesystem.** `ufbx_load_memory` is the only load call. `ufbx_load_file`,
  `ufbx_load_file_len`, `ufbx_load_stdio*` and `ufbx_load_stream` must never appear anywhere in this
  tree. `load_external_files` stays FALSE, and `open_file_cb` is set to a callback that CANNOT SUCCEED
  -- the default is stdio, so a future ufbx that opens a file for a reason `load_external_files` does
  not gate finds a closed door. `obj_mtl_path`/`obj_mtl_data` are never set: ufbx's own header notes
  those "sidestep `load_external_files` as they are explicitly requested."
- **`MODIFY_GEOMETRY` is the only correct `space_conversion`, and the reason is the Bounds row.**
  `ADJUST_TRANSFORMS` converts node translations but not geometry, so every mesh's positions and bounds
  stay in the source's units while the hierarchy is in metres -- the panel prints 100x the truth and
  nothing is internally inconsistent enough to notice. `TRANSFORM_ROOT` parks the factor on every root
  node, where it compounds with `ImportSettings::scale`. **Do not re-litigate this.** Measured against
  ufbx v0.23.0: a Z-up centimetre source converts with `geometry_scale 0.01`, `root_scale 1`, a -90 deg
  X rotation on the roots, and `mirror_axis 0` -- a pure rotation, hence no winding change.
- **`settings.axes.up` is the source's declared up axis; `original_axis_up` is NOT.** The latter reads
  the Autodesk round-trip properties `OriginalUpAxis`/`OriginalUpAxisSign`, which most exporters never
  write, and comes back `UNKNOWN` for an ordinary file. ufbx preserves `settings.axes` across the
  conversion by design. `SourceSpace` reads `axes.up`, with `original_axis_up` only as a fallback.
  **A real Blender export still declares `UpAxis: 1` (Y) while requiring the identical -90 deg-about-X
  geometric correction a Z-up source would** (measured against `cube-binary.fbx`, `FI76`) -- an
  internal convention of Blender's own FBX exporter, not evidence the rule above is wrong.
- **`UnitScaleFactor` in a hand-written ASCII FBX is in CENTIMETRES.** `1` means `unit_meters == 0.01`.
  **A Y-up METRE fixture needs `UnitScaleFactor: 100`.** Writing `1` makes it a centimetre fixture that
  the importer then scales by 100, and the case silently tests the wrong thing while looking green.
- **Geometry transforms, inherit modes: HELPER NODES. Pivots: RETAIN.** `ImportedMesh` is SHARED --
  `ImportedNode::meshIndex` points into one flat list -- so baking a geometry transform into vertices
  would force per-node mesh copies, changing the canonical model's shape for one format. A helper node
  is an ordinary `ImportedNode` to every consumer. `geometry_transform_helper_name` and
  `scale_helper_name` are set EXPLICITLY so the names are stable across a ufbx bump.
- **`opts.ignore_geometry`/`opts.ignore_animation` are set individually at `Structure` depth;
  `opts.ignore_embedded` NEVER is.** An earlier draft set `opts.ignore_all_content` instead, which is
  EXACTLY `ignore_geometry + ignore_animation + ignore_embedded` (`ufbx.c`'s own `ufbxi_load` folds it
  into the three sub-flags before anything else runs) -- so it also zeroed embedded-texture content at
  `Structure`, and an embedded texture fell through to the external-URI path and was recorded as a
  dependency: `Structure` and `Full` disagreed about the URI set for every embedded texture
  (AC-20/INV-M4), measured on the `FI54` fixture (`Structure`: `externalUris=[embedded.png]`; `Full`:
  `externalUris=[]`). At `Structure` depth `num_vertices`, `num_triangles`, `max_face_triangles` and
  `material_parts` are all zero/empty (gated on `ignore_geometry` alone, unaffected by the fix). What
  survives: nodes with full converted transforms, mesh/material/skin/animation IDENTITY, an embedded
  texture's own CONTENT (identical at both depths now), and all of `scene.settings`/`metadata` --
  including the three `*_ignored` flags, which make the depth OBSERVABLE rather than asserted:
  `geometry_ignored`/`animation_ignored` are true and `embedded_ignored` is FALSE at `Structure`, never
  "all three true". So `summary.primitiveCount == meshCount` and `jointCount == 0` at `Structure` for
  FBX, where glTF reports both structurally. That asymmetry is a property of the containers.
- **Fold `\` -> `/` BEFORE `classifyUri`, never after.** `..\..\..\etc\passwd` has already become
  `../../../etc/passwd` when the escape check runs, so the refusal fires. Folding after classification
  would let a backslash traversal straight through. `classifyUri` is NOT modified --
  `UriClass::RefusedBackslash` stays reachable for glTF, where a backslash really is a Windows path
  leaking into a format that has no such concept.
- **`ufbx_texture.absolute_filename` is NEVER read.** It is `C:\Users\bob\Desktop\wood.png` in every
  Autodesk export. `relative_filename`, falling back to the BASENAME of `filename`, and nothing else.
- **Materials come from `material->pbr` ONLY. `material->fbx` is never read.** There is no correct
  Phong -> metallic-roughness formula; every candidate disagrees with the DCC tool's own viewport, and
  the disagreement is invisible until someone compares renders. `use_blender_pbr_material = true` lets
  ufbx invert Blender's own deterministic PBR->Phong export, which is the most likely real input.
  `AlphaMode::Mask` and `TextureWrap::MirroredRepeat` are UNREACHABLE from this backend.
- **Animations are BAKED, never translated.** FBX animates Euler angles under a per-node rotation order
  composed with pre/post rotation and pivots; there is no key-for-key mapping onto quaternion TRS, and
  writing one is a re-implementation of FBX's transform chain. `ufbx_bake_anim` exists for exactly
  this. Every field of `ufbx_bake_opts` that affects output is PINNED EXPLICITLY, even where it equals
  ufbx's default, so a bump cannot move key counts or sample times. Every channel is `Linear`.
- **Skin weights arrive ALREADY SORTED by descending weight, and NOT normalized.** The four-largest
  take is a prefix; the explicit sort is kept for the tie-break and for a future ufbx. Renormalization
  is mandatory. A zero-total-weight vertex gets all-zero joints AND weights -- never `{1,0,0,0}`, which
  would bind it to joint 0.
- **`mesh->skin_deformers` is a LIST, not an optional -- a mesh CAN carry more than one Skin
  deformer.** Only the FIRST survives (`ImportedPrimitive` has exactly one set of four joints/weights,
  so a second is not representable at all), but that drop is NAMED: one warning per mesh giving the
  dropped count, unlike an earlier draft that dropped it silently. The Structure-depth shell pass
  MIRRORS phase 7's own per-mesh, first-only selection (`mesh->skin_deformers.data[0]`, walking
  `s.meshes`) rather than iterating `s.skin_deformers` (every deformer in the scene) -- iterating the
  scene-wide list is what let Structure's `skinCount` disagree with Full's whenever one mesh carried
  more than one deformer. Any future code resolving "the skin for this mesh" must use the identical
  selection, in both places, or the two depths diverge again.
- **NO FIFTH `ImportSettings` KEY, EVER, without making it OPTIONAL.** The four `importer.settings`
  keys are REQUIRED once the block is present, so a required fifth makes every sidecar written by an
  older build report `missing required key` and degrade to defaults. Axis and unit conversion is
  correctness, not preference: a toggle would let a person produce a model that is wrong and then file
  a bug about it. `.meta` STAYS AT VERSION 1.
- **`ImportedNode::localId` is the RAW ufbx `typed_id`, not the position in `ImportedModel::nodes`.**
  `ufbx_baked_node::typed_id` (the animation target) maps to `scene.nodes[]`, and resolving it without
  a side table is why. `parent`, `children` and `roots` all hold `localId`s. For glTF the two happen to
  coincide; for FBX they do not. **`ImportedSkin::joints` holds `localId`s too** (`fbx_import.cpp`
  pushes `cluster->bone_node->typed_id` verbatim) -- the identical rule, one field over.
- **BLOCKING, FOUND BY THIS TASK'S OWN GPU-TIER CASES: `import_details_panel.cpp`'s Hierarchy and
  Skins sections indexed `ImportedModel::nodes` DIRECTLY by `localId`, and that is a real ASan
  heap-buffer-overflow, not a cosmetic bug.** `drawHierarchy`/`drawNodeTree` (`model.roots`/
  `ImportedNode::children`) and `drawSkeletonAndAnimation`'s skin loop (`ImportedSkin::joints`) all
  shipped this way at task 3.2.1, and it was invisible for glTF because glTF's `localId` and its
  `ImportedModel::nodes` position always coincide (no root exclusion, see 3.2.1's own rule above). The
  FIRST FBX model with more than a single unparented node reads out of bounds the instant the Hierarchy
  section draws it -- `I64`'s own four-deep chain is what caught it, as a real ASan abort, not a wrong
  picture. Both draw functions now resolve `localId -> nodes[] position` through a
  `std::unordered_map<std::uint32_t, std::uint32_t>` built once per call, the identical shape
  `fbx_import.cpp`'s own `nodeIndexByLocalId` already uses on the import side. **Any future panel code
  that reads `ImportedNode::children`, `ImportedModel::roots`, `ImportedSkin::joints` or
  `ImportedAnimationChannel::targetNode` must resolve through a map like this one -- never index
  `ImportedModel::nodes` with the raw value.**
  **`skeleton_cook_source.cpp` (task 3.5.1) is the fourth named consumer of this rule and the first
  outside a panel**: it reads `ImportedSkin::joints` AND `ImportedNode::parent` for its ancestor
  closure, and every one of those resolutions goes through one `localId -> position` **sorted vector**
  built once per call (a sorted vector rather than a hash container, because the adapter's output
  order must not depend on an iteration order) -- never `nodes[localId]`.
  **`animation_cook_source.cpp` (task 3.5.2) is the FIFTH named consumer of this rule and the FIRST
  that must NOT convert.** It reads `ImportedAnimationChannel::targetNode` and writes it into the
  cooked clip **verbatim**, as `.aeroanim`'s `targetNodeLocalId`, because `.aeroskel`'s
  `sourceNodeLocalId` (`docs/09` section 12.3) is the same kind of value and the two must be
  comparable at bind time. Everything above still holds for anyone who **indexes**
  `ImportedModel::nodes`; this adapter indexes nothing, which is why the inversion is safe here and
  only here. Mapping the target through a `localId -> position` table would make every FBX clip bind
  to the wrong joints, silently, and `AS9` is the case that reddens if anyone does -- it is hand-built
  precisely because glTF, whose `localId` and position coincide, cannot see the difference.

  **`instantiate_plan.cpp` (task 3.1.5) is the SIXTH named consumer, and it is the one that has to
  hold BOTH numbers at once.** Its BFS reads `ImportedModel::roots` and `ImportedNode::children`, so
  every link crosses one sorted `localId -> position` map built once per call -- while
  `ImportedNode::meshIndex` is **already a position** (into `ImportedModel::meshes`, the same number
  `CookedSubmesh::sourceMeshIndex` records) and is stored **verbatim**. Resolving `meshIndex` through
  that map is seed `S6`: it works for glTF, where the two coincide, and picks the wrong mesh for FBX --
  the exact shape of the bug the map exists to prevent, wearing the map as a disguise. **The rule is
  "resolve links, never positions", and the two are one field apart.**
- **Every platform-dependent ufbx default is set EXPLICITLY.** `path_separator` above all: its default
  is `'\'` on Windows and `'/'` everywhere else, inside a function whose output three CI lanes must
  agree about byte for byte in tests comparing `relativePath` against literals.
- **`ufbx_load_opts` and `ufbx_bake_opts` MUST be value-initialised** (`= {}`). Both carry
  `_begin_zero`/`_end_zero` guards and ufbx returns `UFBX_ERROR_UNINITIALIZED_OPTIONS` otherwise.
- **The `ufbx_error_type` switch has NO `default:`.** A ufbx bump that adds an enumerator must be a
  `-Wswitch` failure on the Linux lane, not a silent "unknown".
- **The MAX_FBX_TEMP_BYTES/MAX_FBX_RESULT_BYTES/MAX_FBX_ALLOCATIONS caps are wired but their OWN
  "fires on a real document" half is UNPROVEN, and this is a documented, deliberate gap, not an
  oversight.** Unlike `MAX_FBX_NODE_DEPTH` (a "many minimal nested objects" shortcut, `FI27`) or
  `MAX_PRIMITIVES_PER_MODEL` (`FI38`, "many minimal meshes"), there is no cheap tier-0 shortcut for a
  1 GiB running total or 4 000 000 individual allocations: ufbx's temp/result allocators back EVERY
  allocation the parser makes for the WHOLE document. The error-to-status MAPPING is proven by
  construction (the shared `ufbxStatusFor` switch arm `FI27` already exercises for the sibling
  `NODE_DEPTH_LIMIT` enumerator); a real document tripping either byte cap is not, and the comment
  beside `FI27` in `fbx_import_test.cpp` says so by name rather than shipping a case that only looks
  like proof.
- **The importer identity is a per-format FUNCTION (`modelImporterIdentity`), never a hard-coded
  constant, and task 3.2.2 found it hard-coded in THREE places, not the two the spec named.**
  `asset_database.cpp`'s phase 7.5 probe and `import_details_panel.cpp`'s Overview line were the two
  the plan expected; `asset_meta.cpp`'s `writeMetaText` (which used to write `GLTF_IMPORTER_NAME`/
  `GLTF_IMPORTER_VERSION` unconditionally into every sidecar's `importer` block) was the third, found
  while wiring `ModelImportSession::applySettings()` for FBX. `writeMetaText` now takes the identity as
  two trailing, DEFAULTED parameters (`importerName`/`importerVersion`, defaulting to the glTF pair) --
  not an `ImporterIdentity`, because `asset_meta.hpp` deliberately never includes `model_import.hpp`
  (plan §A-11's boundary, unchanged). **Any future format-dispatching site that reads or writes an
  importer's name/version must go through `modelImporterIdentity`, never repeat the pair as a literal.**

## OBJ import (task 3.2.3)

- **`.mtl` IS A CLAIMED IMPORTABLE FILE, and that is the whole reason texture dependencies work for
  OBJ (D4).** Phase 7.5 probes at `Structure` depth with NO external bytes, so a probe of `chair.obj`
  can name `chair.mtl` and can NEVER name `wood.png`. Claiming `.mtl` turns one two-hop problem into two
  ordinary one-hop edges, and 3.1.2's cascade is transitive (`planImports` step 4 is a BFS over reverse
  edges that pushes on the clean->dirty transition only), so `wood.png` dirty => `chair.mtl` =>
  `chair.obj` falls out with no new mechanism, no second probe round and no extra read. `asset_view.cpp`
  is NOT touched: a `.mtl` stays `AssetKind::Unknown`, so it is invisible while the Asset Browser's kind
  filter is set to Model. That is accepted and documented, not a defect.
- **`modelImporterNeedsExternalBuffers` SPLITS BY EXTENSION WITHIN ONE IMPORTER, and this is the first
  place in the tree that happens.** `.obj` -> TRUE (its `.mtl` is a genuine external file the Full pass
  needs); `.mtl` -> FALSE (its whole content is local). Answering TRUE for `.mtl` would make
  `ModelImportSession` read every texture the library names, hand them to an arm that ignores every one,
  and -- once they exceed `MAX_EXTERNAL_BYTES_PER_MODEL` -- take the E21 branch and report `Truncated`
  for a result that was COMPLETE at `Structure` depth. Answering FALSE for `.obj` would import every
  model with zero materials, status `Ok`, and no warning at all. **Confirmed by direct sabotage: the
  `.obj`-false seed reddens `MS29` (materials silently vanish, no status change); the `.mtl`-true seed
  is structurally UNOBSERVABLE through `ImportResult` (`importMtlOnly` discards its `external` parameter
  unconditionally) and is caught only at the pure `modelImporterNeedsExternalBuffers` level, never at the
  session/behavioural level -- proven by reading the source, not merely by a seed reddening nothing.**
- **`.obj` at `Structure` depth is a PURE TEXT SCAN, and this is a STATED DEVIATION FROM INV-M4 (D5).**
  `scanObjMtlLibs` is the whole pass: tinyobjloader is not entered, no stream is constructed, no vertex
  is allocated. `Structure` and `Full` therefore DISAGREE about counts, names and the URI set for
  `.obj` -- Full additionally names every accepted texture. The `.mtl` arm keeps INV-M4 perfectly (D6):
  the depth parameter is accepted and deliberately ignored, and `AC-23`'s field-for-field equality is
  what stops a future "optimisation" splitting the two. **Measured, not assumed: at ~150 MB the scan
  alone costs ~107-113 ms warm, roughly 3x the read's own ~34-46 ms -- the SCAN dominates, not the read,
  because the per-line walk does real work on every line of an overwhelmingly `v`/`f` file. Steady-state
  stays ~0 (D15/INV-C5 unaffected); only a scan where the file actually changed pays this cost.**
- **tinyobjloader NEVER touches the filesystem. `ParseFromFile`, `MaterialFileReader`,
  `ObjReaderConfig::mtl_search_path` and the whole `ObjReader` class must appear NOWHERE in this tree.**
  A `.obj` is a user-supplied document and `mtllib /etc/passwd` is syntactically legal Wavefront. The
  only sanctioned entry points are the two `std::istream` overloads, `LoadObj` and `LoadMtl`, fed from
  bytes the editor already read. `ObjReader` is banned for a second reason too: its only from-memory
  entry point copies both inputs, up to ~768 MB resident at the 256 MiB file cap.
- **`TINYOBJLOADER_USE_MAPBOX_EARCUT` is a COMPILE ERROR, not a hope (D21).** The mapbox branch is the
  only unchecked position read in the library's triangulation, guarded by a bare `assert()` that vanishes
  under `NDEBUG` -- a Debug abort on the sanitiser lanes and a heap over-read in Release, both from an
  ordinary broken `.obj`. The `#error` sits BEFORE the library include. `TINYOBJLOADER_IMPLEMENTATION`
  must never be defined either: vcpkg compiles the library, and a second copy risks an ODR conflict.
  **Confirmed by sabotage that this guard has NO automated cover**: removing it reddens nothing in 297
  targeted cases -- AC-13's one-time manual check is deliberately its only cover, outside CI.
- **A vcpkg-prebuilt static library linked into an MSVC ASan lane is a Windows-only `LNK2038` waiting to
  happen, and `tinyobjloader.lib` was CI-caught hitting it -- the second library in this tree to do so,
  after `imguid.lib`/`imgui_stdlib.cpp.obj` (task 2.2.1, see the ImGui section's own entry above).** MSVC's
  STL stamps every object with an `annotate_string`/`annotate_vector` record matching its own ASan state,
  and the linker's `/FAILIFMISMATCH` makes a 0-vs-1 disagreement a hard link error, not a warning; vcpkg
  never builds a port with `/fsanitize=address`, so ANY of its prebuilt static libraries mismatches the
  instant it lands beside this project's own ASan-instrumented objects on the Windows Debug lane --
  `aero_ufbx`/`fastgltf` never hit this because the former compiles in-tree and the latter evidently emits
  no such records. Fixed project-wide in `cmake/sanitizers.cmake`'s `if(MSVC)` branch
  (`_DISABLE_STRING_ANNOTATION=1`/`_DISABLE_VECTOR_ANNOTATION=1`), trading away only the `[size(),
  capacity())` slice of container-overflow detection -- every out-of-allocation heap overflow, including
  this task's own INV-O4 checks, is unaffected on all three lanes. The next vcpkg static library added to
  an ASan-lane target inherits this fix automatically; a library with NO vcpkg port at all (this tree's
  actual bar for vendoring, per ufbx) can instead be compiled in-tree and keep full instrumentation.
- **EVERY INDEX IS RANGE-CHECKED BY US BEFORE ANY ARRAY ACCESS, and the library's own checks do not make
  that redundant (INV-O4).** `fixIndex` (`tiny_obj_loader.h:819`) maps a positive Wavefront index to
  `idx - 1` with NO upper bound; an out-of-range index produces only a WARNING
  (`:3109`, "Vertex indices out of bounds") and `LoadObj` still returns `true`. The built-in ear clipping
  bounds-checks its reads, but one branch SUBSTITUTES ZEROS rather than skipping, so a triangle carrying
  a bad index is still emitted. A face failing any of the three checks is dropped WHOLE, never partially,
  with one capped warning. **Confirmed load-bearing by REAL ASan reports, not merely failed `CHECK`s**:
  disabling any one of the three checks in turn produces a genuine `AddressSanitizer: BUS on unknown
  address` abort against the vertex/normal/texcoord arrays respectively -- the single most important
  proof in this task.
- **The library has TWO GRADES of malformed index, and the asymmetry is upstream.** `f 99999 1 2`
  (past the end) warns and returns `true` -> `Ok`, that face dropped by us. `f 0 1 2` (a zero index) and
  `f -99 1 2` (a relative index below zero) make `LoadObj` `return false` -> `ParseFailed` for the WHOLE
  file. Do not "correct" that into a uniform rule; delivering one needs our own parser.
- **A missing, unreadable or refused `.mtl` is a WARNING, never `MissingBuffer` (D7).** glTF's external
  `.bin` holds the geometry; Wavefront's `.mtl` holds only appearance, so without it there is still a
  mesh. The Full pass proceeds with an empty material stream, every face gets
  `materialIndex = INVALID_SUBASSET`, one warning per `mtllib` line whose candidates were all unsupplied
  names the raw operand, and status stays `Ok`. The warning is OURS, derived from the candidate list
  versus the supplied buffers -- never inferred from a library string.
- **TWO library sentences are dropped, and only two -- reachable at THREE `mtllib` directives, not two,
  a code-review-round correction of this section's own earlier claim.** `"Material stream in error
  state."` (`:2536`, `MaterialStreamReader`'s own guard) and `"Failed to load material file(s). Use
  default material."` (`:2926`/`:3344`, `LoadObj`'s mtllib branch) both describe OUR single-stream
  plumbing -- but `MaterialStreamReader`'s own `if (!m_inStream)` guard tests `std::istream::fail()`
  (failbit/badbit), NEVER `eof()`, so the SECOND `mtllib` directive's `readMatFn` call does not take that
  branch: it silently re-enters `LoadMtl` on an already-exhausted stream, which parses nothing but still
  flushes its own phantom. This section and the engineering log both used to claim, on the strength of a
  two-`mtllib` fixture, that both sentences were "PROVEN unreachable for this library version" -- true for
  two lines, false for three. Probe-confirmed directly against the real vendored header: it is the THIRD
  `mtllib` directive that flips `fail()` true, and the library's raw `warn` string then reads verbatim
  `"Material stream in error state. \nFailed to load material file(s). Use default material.\n"`. `OI23`
  now uses three directives and is confirmed, by direct sabotage, to redden when either filter clause is
  dropped -- both sentences are genuinely reachable and the filter is genuinely load-bearing, not merely
  defence in depth. The library's `"Both \`d\` and \`Tr\` parameters defined"` warning (`:2237`/`:2249`)
  is the OPPOSITE -- a real statement about the file -- and is NEVER dropped; confirmed load-bearing by
  sabotage (`OI62` reddens when it is).
- **`LoadObj`'s RETURN VALUE is the authority, never the `err` string -- and `LoadMtl`'s `err` parameter
  is PROVEN dead code for this library version, not merely unobserved.** Both `LoadMtl` overloads
  (`:2069`, `:2532`) open with `(void)err;` and never touch it again anywhere in the body, so `err_mtl`
  (`LoadObj`'s own internal call, `:2905-2913`/`:3322-3331`) can never become non-empty for ANY input.
  Mapping "non-empty `err` -> `ParseFailed`" was sabotage-seeded directly and reddened nothing, confirming
  the mapping is unreachable rather than merely untested. A `true` return with a non-empty `err` (from
  some OTHER source, should one ever exist) appends each line as a WARNING, never a failure.
- **THE ZERO-FACTOR RULE, and it exists because the library cannot tell you (D14).** `InitMaterial`
  (`:1386`) zeroes `diffuse`, `roughness` and `metallic`, and there is NO "was it set" flag for any of
  them. Read verbatim, every classic `.mtl` imports as a black perfect mirror and `newmtl wood / map_Kd
  wood.png` imports as an invisible texture. So: where a zero factor would ANNIHILATE a texture the same
  material supplies, it is read as the neutral 1; and `roughness` additionally takes 1 when there is no
  map at all -- UNCONDITIONALLY, the one clause with no "has a texture" gate. Nothing else is adjusted --
  `Kd 0 0 0` with NO texture stays black, which is a legitimately black material. **Each of the four
  clauses is confirmed independently load-bearing by sabotage**: dropping the roughness clause turns
  every mirrorless material into one; applying the `Kd` clause unconditionally turns a legitimately
  black, textureless material white.
- **`d` ALWAYS beats `Tr`, in either order -- verified at the literal source (`:2229-2260`), where `Tr`'s
  own handler comment reads "`d` wins. Ignore `Tr` value."** The library warns from BOTH handlers when
  both appear. No `Ns` -> roughness curve, ever: there is no standard behind any of the candidate
  formulas, and the panel prints `roughnessFactor` as a number. `map_Ka` is NOT mapped to occlusion --
  `Ka` is an ambient COLOUR map, and mapping it would be a guess the panel prints as a fact.
- **Fold `\` -> `/` BEFORE `classifyUri`, never after** -- `foldBackslashesToSlashes`, the function
  3.2.2 added for FBX, reused verbatim (D15). A `mtllib`/`map_Kd` operand is a FILESYSTEM PATH, never a
  URI, so `UriClass::RefusedBackslash` is right for glTF and wrong here. `classifyUri` is NOT modified;
  the class simply becomes unreachable from this importer. **The stated, accepted cost: a backslash
  cannot be both a separator and an escape, so `mtllib my\ file.mtl` is NOT supported.** An ambiguous
  operand is instead offered BOTH ways -- the whole trimmed operand first, then each token -- and the
  filesystem decides. This function's own doc comment now names BOTH backends and BOTH reasons, since
  "CALLED BY THE FBX BACKEND ONLY" became false the moment this task's own `obj_import.cpp` called it.
- **The importer CONVERTS NOTHING (D12/INV-O6):** no axis flip, no winding reversal, no unit scaling, no
  handedness change, and no setting for one. Wavefront agrees with glTF on everything that matters. FBX
  is the format that needs conversion, precisely because it declares Z-up and centimetres.
- **`ImportedNode::localId == its position in `nodes` == `meshIndex` for OBJ (D11)**, because there are
  exactly N root nodes, one per mesh, in source order, with no synthetic parent. That means 3.2.2's
  BLOCKING `nodes[localId]` heap-overflow cannot resurrect here -- but only BY CONSTRUCTION. The panel's
  `indexByLocalId` map still runs and still must; a case pins `nodes[i].localId == i` so a future change
  that breaks the coincidence is caught rather than discovered.
- **An empty mesh's `bounds` is `Aabb{}` -- a POINT BOX -- never the `Aabb::empty()` sentinel; and
  `summary.bounds` is folded from SURVIVING PRIMITIVES, never from mesh bounds.** OBJ is the first format
  in this tree that produces empty meshes routinely (an `l`/`p`-only shape, or one whose faces were all
  dropped by INV-O4), so this pairing became reachable for the first time here. Both shipped backends
  already do exactly this and their comments say why: a mesh left at the sentinel has a NaN centre, and
  folding the model bounds from mesh bounds would leak the world origin into it for every empty mesh.
  **Confirmed as two genuinely independent properties by sabotage: one seed reddens only the mesh-bounds
  half, the other only `summary.bounds`, never both at once.**
- **`.obj` is NEVER routed through ufbx, permanently.** ufbx v0.23.0 genuinely parses `.obj`
  (`UFBX_FILE_FORMAT_OBJ`) and will even open a sibling `.mtl` FROM DISK via `obj_mtl_path`/
  `obj_mtl_data`, which its own header notes "sidestep `load_external_files`". That is a filesystem read
  behind a user-supplied document, which is exactly what D3 forbids. 3.2.2 already banned those two
  fields by name; this entry is the answer to the temptation, written down.
- **No `.obj`/`.mtl` WRITING, ever.** This importer is read-only; the only write anywhere in the model
  import subsystem stays `ModelImportSession::applySettings`'s `.meta` sidecar (INV-M9).
- **TWO build-time discoveries about `LoadMtl` share ONE observable signature (an empty `name`) and are
  closed with ONE mechanism, not two special cases.** First: `LoadMtl` unconditionally flushes a
  trailing "material" at end-of-parse (`:2459`'s own comment, "// flush last material.") with NO guard
  on whether any `newmtl` was ever seen -- a completely empty `.mtl` stream produces `materials.size()
  == 1`, not 0. Second: because `MaterialStreamReader`'s guard tests `fail()` not `eof()`, a SECOND
  `mtllib` line's `readMatFn` call does not take the "stream in error" branch at all -- it silently
  re-enters `LoadMtl` on an already-exhausted stream, which flushes ITS OWN second, empty-named phantom.
  `declaredWithEmptyName(combinedWarnings)` -- checking for the library's OWN "empty material name in
  `newmtl`" warning -- is the ONE signal that tells a genuine, if malformed, user-authored empty-named
  `newmtl` apart from either phantom shape; `convertMaterials` filters on it with a separate `nextLocalId`
  counter so a dropped phantom never consumes a local id. **Any future change to material parsing must
  route through this same filter, never assume `LoadMtl`'s `materials` vector holds only real
  declarations.**

## Blender CLI (task 3.2.4)

- **Never parse `.blend`.** ADR-003 and the GPL boundary (`docs/01-tech-stack.md`). Blender is an
  external PROCESS: no header, no library, no vcpkg entry, ever. A contributor reaching for a
  `.blend`-reading library is repealing an ADR, not optimising. The only read of a `.blend`'s bytes
  anywhere is 3.1.2's opaque `hashFileContents`, which treats every file identically as a byte stream.
- **`isBlendFileName` is NOT `isImportableModelName`, and `.blend` must never join the latter's table.**
  Phase 7.5 gates its probe on that predicate; adding `.blend` there makes the SCAN feed raw `.blend`
  bytes to `importModel` and every `.blend` in the project reports an import failure on every scan.
  Sabotage-confirmed to be worse than that in practice: the seeded six-entry table did not merely fail
  cases, it **aborted the test binary** (`SIGABRT` in `MI28`). `MI133` pins both halves — including
  `modelImporterNeedsExternalBuffers("x.blend") == false`, a SECOND fact that breaks independently
  (proven by seed S35, which reddens only that half while S22 reddens both).
- **The `<guid>.glb` artifact bypasses `modelImporterNeedsExternalBuffers` on purpose.** ONE
  `importModel` call, `Full` depth, empty external span, empty `assetRelativeDir`. It lives in
  `Library/`, so it has no assets-relative directory and any URI it named would resolve against an
  unrelated tree. If it names any, ONE warning is appended and the list is cleared. **The path and the
  observable need SEPARATE proofs**: `MS41` reads `serviceBlend`'s own source text (one `importModel`,
  one `readFileBytes`, zero gate consults, one empty `assetRelativeDir`) and `BS41` asserts the warning
  against a GLB fixture that genuinely declares an external URI. Measured: routing the artifact through
  the two-pass driver reddens `MS41`/`MS22` and **not** `BS41` — an ordinary two-pass route produces an
  identical `ImportResult` — so neither proof substitutes for the other.
- **`SDL_WaitProcess` is only ever called with `block == false`, and no pipe is ever created.** SDL's
  own header documents the deadlock. `SDL_ReadProcess`/`SDL_GetProcessOutput`/`SDL_GetProcessInput`
  appear nowhere in this tree and must not start. `tool_process.cpp`, the `ToolProcess` class (task 4.3.1 renamed it
  from `BlenderProcess`), is the ONLY translation unit that spells SDL's process symbols, and Blender and esbuild share it. Sabotage-confirmed: a blocking wait does not fail the
  suite, it **hangs** it past every budget — the comment-stripped gate grep is the real cover.
- **`BlenderService::poll()` has exactly ONE call site in the tree, inside
  `ModelImportSession::serviceBlend()`, and `EditorApp::tick()` gains no fourth post-draw call.** The
  service is reached through the session that owns it. `I74` re-asserts `I60`'s ordering proof against
  this task's own edits; `I75` is the only mechanical cover for "the panel never polls", because no
  runtime tier in this tree can see that violation.
- **`currentHostOs()` is the only per-OS branch in first-party editor code**, and it is exactly three
  lines, in the `#elif defined(__linux__)` + `#error` form. The bare-`#else` form produces TWO lines and
  silently falls back to Linux on an unknown host. Do not cite the three platform macros in any comment
  under `editor/src` or `editor/include`: the gate grep does not strip comments, unlike the platform and
  rhi guards. Vendored sources under `editor/third_party/` are out of scope, exactly as the clang-format
  glob already excludes them.
- **The `.blend` arm sits BEFORE `service()`'s `isImportableModelName` early return**, and the
  `(target, generation)` consume guard has exactly ONE exception: a `.blend` target whose session is
  `Converting` or whose service is `Probing`/`Converting`. It is narrow on both axes on purpose — a
  `.gltf` selected while some probe is alive must not be re-imported every tick, and a settled `.blend`
  must still cost ten early returns over ten ticks. **A case proving that second half needs a cache
  HIT**: on a cache MISS nothing is imported either way, so a widened guard reddens nothing (measured —
  an earlier `MS36` was exactly that vacuous and was rewritten).
- **A nil-GUID `.blend` is `NeedsConversion` with a DISABLED button, never `NotImportable`.** That
  enumerator's panel branch renders one sentence and returns before any section, so it would draw no
  button to disable — and after this task it would be telling the user something false.
- **The provenance record and the run's status document SHARE the path `<guid>.json`, deliberately.**
  On success the record overwrites the status; a half-finished run leaves a document
  `parseExportProvenance` rejects, which is exactly the "no valid cache entry" answer. A test asserting
  "no provenance was written" must assert what the document IS, not that the path is empty.
- **`blender_tool.cpp`, `tool_process.cpp` and `blender_service.cpp` never log, and none of them is
  in either of `check-project-no-delete.sh`'s lists** — being outside both is exactly what makes a
  future `std::filesystem::remove` there a hard CI failure (sabotage-confirmed: the guard fires before
  any test binary runs). All three are also `<thread>`/`<mutex>`/`<atomic>`/`<future>`/
  `<condition_variable>`-free: the OS runs the child concurrently and one non-blocking wait per tick is
  the whole mechanism.
- **Any test that can reach the Blender resolve path MUST set `EditorAppConfig::toolPrefsPath`**, or it
  reads — and through `Locate…`/`Re-detect`, WRITES — the developer's real machine-wide
  `editor_tools.json`. This is the third instance of the `recentProjectsPath`/`layoutIniPath` lesson.
- **A RE-ENTRY THROUGH THAT EXCEPTION POLLS, AND DOES NOTHING ELSE.** The exception exists so a child
  gets waited on, not so the cache is re-evaluated: falling through to the probe re-read the artifact and
  re-ran `importModel` on every frame for the whole duration of an unrelated asset's run, and wiped
  `resultValue` on the way in. Nothing below the poll can change a settled target's answer anyway —
  `setTarget`, `requestConversion` and `cancelConversion` all clear `serviced`, so none of them arrives
  as a re-entry. `BS35` is the case, and it asserts `importCount()`, which reads 39 instead of 1 without
  the guard.
- **`SessionState` and `BlenderState` are the first values in the editor that span frames, and all three
  of the code-review round's worst findings are the same mistake: a per-tick OUTPUT read as the next
  tick's INPUT with nothing resetting it.** `setTarget()` must reset `stateValue` with everything else it
  already resets — a stale `Converting` made the panel report a run against an asset that had none, and
  made the session consume another asset's result. `serviceBlend` additionally requires
  `blender().conversionGuid()` to match its own target before consuming `Converting`/`Converted`. **Any
  new cross-frame field on either type inherits both obligations.**
- **The panel names the version from the ARTIFACT'S OWN provenance record
  (`ModelImportSession::artifactBlenderVersion()`), never `BlenderService::versionString()`.** The latter
  is the currently installed Blender: empty on a pure cache hit, and — once anything has probed — a
  binary that did not produce the file on screen. "Convert with 4.2, upgrade to 5.2, and the panel still
  says 4.2" is only true if this distinction is kept.
- **EVERY child this service starts is bounded, not just the export.** A hung `blender --version` left
  the service in `Probing` for the life of the editor, which also re-entered the `.blend` arm every tick.
  `BLENDER_PROBE_TIMEOUT_SECONDS` and `BLENDER_TIMEOUT_SECONDS` are three orders of magnitude apart on
  purpose. **A bounded WAIT loop in a test must inject ZERO seconds** (`WAIT_DT`): forking a fake tool
  costs thousands of poll iterations, so a plausible-looking 16 ms per iteration is minutes of fake time
  and trips a real timeout while the child takes milliseconds. Every timeout is driven by an explicit
  one-shot injection instead.
- **`cancel()` is `noexcept`, so it RECORDS and `poll()` completes** — the same division of labour
  `requestConversion`/`startExport` already has. A cancel arriving between the request and the spawn must
  still produce a verdict, or the session waits forever on a run that will never start; and assigning the
  message inside `cancel()` is an allocation clang-tidy rejects outright.
- **The panel's Blender log node is DEFAULT-OPEN, and that is what makes its contents testable at all.**
  No tier in this tree can click a `TreeNode`, so a closed node's branches never execute anywhere, under
  any sanitizer — which is precisely how the refused-by-cap branch shipped undriven. The same reasoning
  the six `CollapsingHeader` sections already carry.
- **`<projectRoot>/Library/BlenderExports` has ONE rule, `blenderExportDir()`, and it returns EMPTY for
  an empty project root.** Concatenating onto an empty root yields `/Library/BlenderExports` — an
  absolute path at the filesystem root that the probe's own directory creation then attempts, harmlessly
  on POSIX and for real on Windows. An empty result is the service's own documented "resolve, but do not
  spawn"; `EditorApp::resolveBlender()` defers on it, leaving the state `Unknown`, which is exactly what
  the lazy resolve re-tests once a project is open.

## Assimp import (task 3.2.5)

- **`ReadFileFromMemory` is NOT enough, and `SetIOHandler(nullptr)` is a TRAP.** The from-memory entry
  point *wraps* the installed IO handler in a `MemoryIOSystem` that serves one magic filename from the
  buffer and **delegates every other path to the wrapped handler** — which out of the box is a live,
  unrestricted `DefaultIOSystem`. And `SetIOHandler(nullptr)` does not clear the handler, it **installs**
  a fresh `DefaultIOSystem`. The only sanctioned sequence in this tree is
  `SetIOHandler(new RefusingIoSystem())` **then** `ReadFileFromMemory`. `Importer::ReadFile`,
  `DefaultIOSystem`, the whole C API, `Assimp::Exporter`, `ZipArchiveIOSystem` and `.zae` must appear
  nowhere in this tree, permanently. **All twelve `IOSystem` virtuals are overridden** — four are pure and
  the compiler forces them, the other eight are not, and an un-overridden one silently inherits a base
  that keeps a directory stack and compares real paths.
- **The loader that ran is ASSERTED, never assumed** — `AI_METADATA_SOURCE_FORMAT`, compared against a
  distinctive fragment of the expected loader's own `aiImporterDesc::mName`. `UnregisterLoader` is
  rejected because it leaks. **Assert `Get(AI_METADATA_SOURCE_FORMAT,`, never the bare key**: the bare
  token is a PREFIX of `AI_METADATA_SOURCE_FORMAT_VERSION`, which this same file reads for a display
  string, and a gate spelled the short way stays green when the whole assertion is deleted (sabotage
  seed S4 found exactly that).
- **The forbidden post-process flags**, with `aiProcess_FindInvalidData` called out by name: *it deletes
  meshes, and if it deletes them all it fails the whole file*, which collides head-on with 3.2.1's D11
  (an empty mesh survives so the panel can show the user why their file looks empty). Its one genuine
  benefit — nulling an all-zero normal/tangent/UV channel — is taken over by this backend's own check,
  which keeps the mesh, keeps the counts and names the channel in a warning. `PreTransformVertices`,
  `MakeLeftHanded`, `ConvertToLeftHanded`, `FlipUVs`, `FlipWindingOrder`, `GlobalScale`, `GenNormals`,
  `GenSmoothNormals`, `CalcTangentSpace`, `JoinIdenticalVertices`, `RemoveRedundantMaterials`,
  `GenBoundingBoxes` and `FindDegenerates` are forbidden for the reasons `assimp_import.cpp`'s own header
  lists.
- **`aiMatrix4x4` is ROW-major and `aiQuaternion` is `{w,x,y,z}`.** `engine::Mat4` is column-major and
  `engine::Quat` is `{x,y,z,w}`, so every matrix conversion **transposes** and every quaternion conversion
  is a **named field copy** — never a `memcpy`, never a `bit_cast`, never a positional construction. There
  are **three** call sites (`convertNodes`, `convertSkins`, `convertAnimations`) and they break
  independently: sabotage confirmed that one reordered `toQuat` reddens cases in two of them and one
  un-transposed `toMat4` reddens only the third. Both defects produce a plausible **wrong model**, not a
  failure.
- **`metallic 0` / `roughness 1` are this importer's DEFAULTS, not claims about the file**, and they must
  be written EXPLICITLY because `ImportedMaterial`'s own default for `metallicFactor` is 1 — leaving it
  ships every DAE/PLY/STL material as a full metal. No `SHININESS` → roughness curve, ever. No
  `LIGHTMAP` → occlusion: `aiTextureType_LIGHTMAP` is where the Collada loader puts `<ambient>`, and an
  ambient colour map is not an occlusion map.
- **Collada CANNOT express a black diffuse COLOUR together with a diffuse TEXTURE.** `<diffuse>` holds a
  `<color>` or a `<texture>`, and `ColladaLoader` writes `AI_MATKEY_COLOR_DIFFUSE` unconditionally from
  `Effect::mDiffuse`, whose default is `(0.6, 0.6, 0.6)`. The zero-factor rule's paired arms are therefore
  driven from a `.ply`, whose `element material` supplies the colour independently of the `TextureFile`
  comment. Do not re-write that case against a `.dae`; it cannot be written.
- **`.dae` bounds are in the SOURCE's own units** while the hierarchy carries the unit/axis factor,
  because Assimp's Collada loader bakes the conversion into the root node's transform and exposes neither
  input. **This is not to be "fixed" by re-scaling geometry behind the user's back.** It is the named
  asymmetry with the FBX path, whose `MODIFY_GEOMETRY` choice was made for exactly the opposite reason.
- **`thumbnail_store.cpp` defines `STB_IMAGE_STATIC` and must keep defining it.** Removing it re-opens a
  duplicate-symbol collision with the second, unprefixed `stb_image` implementation assimp's vcpkg port
  compiles (its `build_fixes.patch` turns upstream's `assimp_stbi_*` prefixing off) — a static-link
  concern on macOS and Linux, structurally absent on Windows where the port builds a DLL.
- **The library LEAKS on its own error paths, and `tests/lsan.supp` carries exactly two frame-scoped
  entries for it.** `ColladaParser`'s CONSTRUCTOR parses the whole document from its ctor body, so a
  rejected document throws before the object exists and its destructor never runs — every node, mesh,
  submesh, input channel and transform already allocated is orphaned; `STLImporter::LoadASCIIFile` does
  the same on a truncated file. This backend's failure-mode fixtures are the first things in the tree to
  reach those paths at all, so the frames surfaced here (CI run 31559183254, 4704 B / 17 allocations)
  and only on the Linux Debug lane, the one lane that runs LSan — invisible to every macOS run, local or
  CI, and to Windows. **A future format added to this backend may surface further such frames. Each
  needs its OWN entry, gated on observed CI evidence and naming its own function — never a module-wide
  `leak:assimp`**, which would blind LSan to a first-party leak allocated anywhere inside the library
  (the same reason that file refuses `leak:SDL`). The two entries create no first-party blind spot
  because nothing of ours allocates inside either frame: the only code of ours reachable from there is
  `RefusingIoSystem`'s twelve virtuals, none of which allocates. Do not weaken the triggering cases —
  the leak is the library's, and the port is never patched locally.
- **Never write the character sequence `assimp/` in prose** anywhere under `editor/`, `tests/`, `engine/`,
  `runtime/` or `tools/`: the include-boundary gate greps those roots and a prose mention turns it red for
  a reason that is not a violation. Write "the assimp port", "assimp 6.0.4", "lib/cmake/assimp".
- **`.stl` and `.ply` do NOT enter Assimp at Structure depth**, which is a stated deviation from INV-M4 —
  the two depths disagree about counts, names and hierarchy, exactly as `.obj` has since 3.2.3. The one
  thing they owe each other is an identical `externalUris`, and **the `.ply` Full pass must seed it from
  the same header scan the Structure pass uses**. With a single `TextureFile` line the loader's own
  material happens to reproduce the scan's answer, so deleting the seeding step is invisible until a
  header names TWO different textures — Assimp keeps only the LAST, the scan returns both.
- **`.ply` needs a first-party pre-allocation bound and `.stl` does not**, and that asymmetry is a property
  of the two LOADERS, not of the two formats. `STLLoader.cpp` refuses at
  `mFileSize < 84ull + mNumFaces * 50ull` in 64-bit arithmetic **before** its own
  `new aiVector3D[mNumFaces * 3]`; the `Ply` sources contain no comparison against the file size anywhere.
  A `.ply` header declaring 2³²−1 vertices over an empty body sends the loader into a multi-minute grind at
  unbounded, climbing resident memory, reachable from a 120-byte file. `plyDeclaredCountsExceedBytes` runs
  **after** `seedPlyExternalUris` (so the depth-equality still holds on a refused file) and **before**
  `runAssimp`. Do not add an `.stl` equivalent: it would be dead code duplicating a guarantee the loader
  already makes.
- **`aiProcess_ValidateDataStructure` runs FIRST**, before `ScenePreprocessor` and before every other
  post-process step, and it **throws**. Its message begins `Validation failed:`, which is what maps a
  refusal to `ImportStatus::Malformed` rather than `ParseFailed`. Two consequences worth knowing before
  writing a case: an out-of-range face index or bone-weight vertex id is refused whole, so this backend's
  own range checks are **unreachable while that flag is on** (they stay as defence in depth for a
  validation-off build and are pinned in the source text, not by a runtime case); and an out-of-range `<p>`
  index in a `.dae` never reaches validation at all — `ColladaParser` rejects it first with
  `Invalid data index (n/m) in primitive specification`, which is a PARSE failure. The input that does
  reach the validation arm is two identically-named joints.
- **`aiProcess_SortByPType` + `AI_CONFIG_PP_SBP_REMOVE` DELETES a mesh** whose only primitive type is
  removed, rewrites the node graph around it, and throws `No meshes remaining` when nothing is left. A
  lines-only `.dae` is therefore refused outright and a mixed one keeps exactly the triangle mesh — there
  is no surviving empty mesh to observe, and the "no triangles survived" arm is unreachable for the same
  family of reasons as the range checks.
- **A NODE-ID MAP IS BUILT BY THE WALK THAT ASSIGNS THE IDS, NEVER BY A SECOND WALK OF THE SAME TREE —
  3.2.2's `localId` lesson in a new costume, and this task's BLOCKING finding.** `convertNodes` returns the
  `aiNode*` -> localId map `convertSkins` resolves `aiBone::mNode`/`mArmature` through; it used to be
  re-derived by a second, plain pre-order walk numbering `0,1,2,…`, one index per `aiNode`. The two diverge
  in two places, **both ordinary content**: a MULTI-MESH node consumes `mNumMeshes` slots (its own plus the
  synthesized `<name>.<n>` children), where a plain walk consumes one — and
  `ColladaLoader::BuildMeshesForNode` pushes one mesh ref per submesh **per material**, so a node whose
  geometry carries two materials is one — and a DEPTH-DROPPED subtree is never emitted while an unbounded
  walk descends straight into it. **A count bound truncates the tail; it realigns nothing.** The failure is
  silent: joints bind to the wrong nodes, `skeletonRoot` lands on a synthesized split child, status stays
  `Ok`, no warning. **The split children are deliberately NOT in the map and cannot be** — they have no
  `aiNode`, so no key — and nothing is lost, because every key the map is asked for is a pointer the
  LIBRARY produced and can never name a node this file invented. Any future pass needing "the localId of
  this `aiNode`" takes the map as a parameter; it does not rebuild one.
- **Every cap site LATCHES its report with its own bool, and a `break` inside a nested loop is a cap site
  too.** The multi-mesh split loop's `break` claimed "the shared cap message above already fired or will",
  which is false on the LAST node processed: the main loop's check runs only when another node is POPPED,
  so a childless multi-mesh node hitting the cap with an empty stack reported `Ok` with nodes dropped
  (AC-53). `convertSkins`' two `escalate` calls had no latch at all, so N over-cap skins appended N copies
  of one sentence. **Two cap sites with different CAUSES get different WORDING**, even when the constant is
  the same one: `convertMeshes`' model-wide vertex total and `convertSkins`' single-mesh influence table
  both trip `MAX_VERTICES_PER_MODEL`, and one shared sentence leaves the message unable to say which fired.
- **`aiNode::mMeshes[i]` is range-checked like `face.mIndices[k]` and `aiVertexWeight::mVertexId`, and for
  the same reason.** All three are unreachable while `aiProcess_ValidateDataStructure` is on and all three
  are pinned in `AI34`'s comment-stripped source text rather than by a runtime case. The `mMeshes` one
  matters most downstream: 3.1.5 resolves `ImportedNode::meshIndex` into `ImportedModel::meshes`, where an
  unchecked value is an out-of-bounds READ rather than a wrong picture.
- **A refused image is FOUND-OR-APPENDED on its RAW uri, not on `relativePath`** — a refusal has no
  resolved path, so the accepted branch's dedup can never match one, and two materials naming the same bad
  path would append two identical `ImportedImage`s and spend two identical warnings out of the
  `MAX_IMPORT_WARNINGS` budget.
- **`Assimp::SkipSpaces` is `' '` OR `'\t'`, and `scanPlyTextureFiles` must mirror it exactly.**
  `PLY::Element::ParseElement` opens with `PLY::DOM::SkipSpaces`, which forwards to it, so a TAB-indented
  `comment TextureFile wood.png` reaches the loader. A scan skipping only `' '` made Structure return `{}`
  where Full returned `{wood.png}` — the depth disagreement AC-19 forbids — and phase 7.5's Structure-depth
  probe recorded **no dependency**, so editing the texture never marked the model `DependencyChanged`.
  `plyDeclaredCountsExceedBytes` carries the identical rule; it fails safe there (it under-counts, never
  rejects), and the two must still never diverge.
- **A node instancing the SAME geometry twice is not a multi-mesh node** — the loader caches by
  `(geometry, submesh, material)`, so the two refs collapse onto one index, which `ValidateDataStructure`
  then refuses outright (`aiNode::mMeshes[i] is already referenced by this node`). A multi-mesh fixture
  needs two DISTINCT geometries, or one geometry with two materials.
- **`ArmaturePopulate::GetArmatureRoot` walks UP from a bone until it finds a node that is not one**, so
  `skeletonRoot` lands on the wrapper node. A fixture whose joints hang directly off the visual scene gets
  `skeletonRoot == 0` under every numbering and cannot discriminate a shifted map — wrap the joints in an
  explicit `Armature` node when that is the property under test.
- **A `.dae` fixture in a doctest macro must be hoisted into a named local first** — MSVC's legacy
  preprocessor breaks on a raw string literal containing `\"` passed directly as a macro argument, and XML
  is nothing but escaped quotes.
- **A reduced-configuration probe must be configured with `-G Ninja`.** `CMAKE_GENERATOR` enters the
  shadercross bootstrap's option hash, so a Makefiles configure reads the cached toolchain as COLD and pays
  a twenty-minute from-source DXC rebuild that has nothing to do with what the probe tests.

## esbuild (task 4.3.1)

- **The command line is pinned, and every flag earns its place.** `--bundle` (one module per script, with its
  imports inlined), `--format=esm`, `--platform=neutral`, `--target=es2023 --supported:using=false` (R21: the
  VM's `using` hang is reachable through un-lowered output, and the tsconfig `target` does not lower it),
  `--preserve-symlinks` (a symlinked directory inside the assets root must resolve to the path the asset database walks),
  `--sourcemap=linked`, a `--tsconfig-raw` that restates `verbatimModuleSyntax`, `useDefineForClassFields` and
  `experimentalDecorators` (they change emit, and the project's own `tsconfig.json` is never read), and
  `--log-level=warning --color=false --log-limit=0` (a clean run writes a 0-byte log). The arguments are relative, run
  from the project root, so no absolute path leaks into the output. Changing any element changes
  `esbuildFlagsFingerprint()` and so invalidates every record: that is the point.
- **Publishing is map, then JS, then record, each through `writeTextFileAtomic`.** The staged files are read with
  `readFileBytes` and written again; a direct rename is forbidden by `check-project-no-delete.sh`. The record is the
  only thing that makes the trio fresh, so it lands last.
- **Record hashes come from the scan, never from a re-read, and a scan completing during a run re-runs the script.**
  The database's hashes predate the run; a file hashed from disk when the run ends could record a save made during the
  run as the new state while the JavaScript holds the old bytes. `finish` compares `database.generation()` with the
  value taken at the start and pushes the job back on the front of the queue when they differ.
- **A tool failure writes no record.** A missing binary, a spawn failure, a run past the 30 s limit and an unpreparable
  staging directory say nothing about the script, so it stays stale and the next completed scan retries it. Such a
  failure logs one error per pass, kills the child, fails the running and queued jobs, and ends the pass.
- **An error run writes no metafile**, so a failed record's inputs are the script plus every diagnostic location under
  the assets root, and its `sourceSet` fingerprint makes creating a missing file retry it. **A failing run is recorded
  only when its log was read whole and held 1 to 64 diagnostics**: only then does the record name every file the
  failure points at. A log that is empty, unparseable, longer than 64 diagnostics or over 256 KiB writes no record, and
  the next scan runs the script again (ST31, ST32, ST34).
- **A fresh `failed` record is a failure at every pass.** `noteScan` files fresh records as ok (`freshAtLatest`,
  counted under K) or failed (`failedAtLatest`); `endPass` prints each fresh failure's stored diagnostics through
  `show` and counts it under M -- after every run's lines, before the summary, skipping a script that has an outcome
  in the pass. Printing at evaluation instead repeats the lines for every scan a pass spans (ST30). A stored
  diagnostic is exactly a printed line: a refusal is location-less, so its re-shown line is the live one (ST29), and a
  `failed` record with no diagnostic is a parse miss (EB26).
- **`readFileBytes`' cap is its own outcome.** Branch on `refusedByCap` before parsing: a refused read parsed as
  empty turns an oversized log into "wrote nothing" (ST34) or drops every warning of a clean run (ST33).
- **A script of the scan is one predicate, `scriptJobOf`**: a `.ts` (not `.d.ts`) with a valid GUID and a usable hash
  whose own project-relative path classifies `Accepted`. `collectScripts` and `requeue` both use it, so a package under
  `node_modules` inside the assets root is never run as a script, at a scan or after a mid-run move (ST35).
- **The binary is identified by content hash, not by `--version`**, because a version probe is a process and an
  unchanged project starts none. The hash is computed once per session, at first need, and an unknown identity compares
  nothing.
- **esbuild's log columns are 0-based and its lines 1-based.** The Console line and the record store 1-based columns.
- **esbuild's error mark is `X` on Windows outside Windows Terminal.** In 0.28.2 `MsgKind.Icon` prints `X` on Windows
  when `WT_SESSION` is unset and U+2718 otherwise, so the mark follows the environment, not only the OS. The parser
  accepts both before `[ERROR] ` on every host; the warning mark U+25B2 is the same everywhere; nothing looser is a
  header (`x [ERROR] `, `XX [ERROR] `, `X[ERROR] ` and `X [WARNING] ` are not). `EB30` pins it. The pinned darwin binary
  cannot print the `X` form, so only a Windows run shows it live.
- **Every new test sets the esbuild path** (`ScriptTranspilerConfig::esbuildPath` or `EditorAppConfig::esbuildPath`,
  taken from `AERO_TEST_ESBUILD`), the `toolPrefsPath` rule again: resolution is lazy, so a project without a script
  never reads `AERO_ESBUILD_PATH` or the editor's own directory, and a test that reaches it would run the local machine's
  binary. Tests run the cached binary, never the copy beside `aero_editor`.
- **The transpiler never logs.** `esbuild_tool.cpp`, `script_transpiler.cpp` and `tool_process.cpp` return values;
  `EditorApp::drainScriptLog` is the one site, and its switch over `ScriptLogLevel` has no `default:`. Every line starts
  `scripts: `.
- **`ST22`'s textual check stays before any evaluation.** It reads the output for a `using` declaration first, so a
  regression in the flags is reported as that and not as a hang in the VM. The VM is linked into `aero_editor_shell_test`
  only (`aero::script`, PRIVATE); `aero_editor_core` and the editor link none.
- **A UTF-8 log fixture is written as `\x` escapes split into separate literals**, and the header glyph bytes in
  `esbuild_tool.cpp` are a `std::array<char, 3>` of byte values: every literal under `editor/` stays 7-bit.
