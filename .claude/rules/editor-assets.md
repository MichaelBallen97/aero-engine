---
paths:
  - "editor/src/asset*"
  - "editor/src/material*"
  - "editor/src/thumbnail*"
  - "editor/src/texture_*"
  - "editor/src/*cook_source*"
  - "editor/src/scene_asset*"
  - "editor/src/instantiate_plan*"
  - "editor/src/meta_utils*"
  - "editor/src/import_details_panel*"
  - "editor/src/audio_decode*"
  - "editor/include/aero/editor/asset*"
  - "editor/include/aero/editor/material*"
  - "editor/include/aero/editor/thumbnail*"
  - "editor/include/aero/editor/texture*"
  - "tests/editor/asset*"
  - "tests/editor/material*"
  - "tests/editor/thumbnail*"
  - "tests/editor/*cook_source*"
  - "tests/editor/scene_asset*"
  - "tests/editor/instantiate_plan*"
  - "tests/editor/audio_decode*"
---

# Editor: the asset pipeline

Split out of `editor.md` so that no editor rules file is cut at the 150 000-character load limit. A reference to another section may point into a sibling `editor*.md` file.

## Assets (task 3.1.1)

- **A valid `.meta` is NEVER rewritten (D6).** A scan of a fully-described tree writes **zero bytes**
  to disk — not "identical bytes", zero. `AssetDatabase::rescan` calls `writeTextFileAtomic` from
  exactly ONE call site (INV-A1), and only for records the planner marked `Created` or `Repaired`; an
  `Ok` record's `.meta` is untouched, mtime and all. Get this wrong and every user's repository dirties
  itself the moment they open a project — sabotage seed S13 (a planner bug that flags a valid record
  for write) is the most important seed in the whole matrix for exactly this reason.
- **An invalid `.meta` is never overwritten (D7).** A parse failure, a bad version, a nil GUID — none
  of them get "fixed" by a rewrite. One rule, no exceptions: a merge-conflicted sidecar still holds the
  real GUID one `git checkout --theirs` away, and a carve-out for "obviously broken" cases gets applied
  to that case by analogy the first time someone is in a hurry.
- **An orphaned `.meta` is reported and left on disk, never deleted (D8).** A transient
  `exists() == false` is indistinguishable from a real deletion, and the destructive reading is the
  irreversible one. This is enforced doubly: by discipline in `asset_database.cpp`, and by the widened
  `check-project-no-delete.sh` guard (see the Projects section above) — sabotage seed S21 (adding a
  `std::filesystem::remove` call for an orphan) reddens the guard **before any test even runs**, and
  `project-no-delete.no_delete_e2e` stays green throughout because it exercises the script against its
  own hermetic scratch tree, not the real source — both facts together are the intended CI behavior.
- **Duplicate GUIDs ARE repaired, deterministically (D9).** Sort byte-lexicographically by
  project-relative path; the FIRST claimant keeps the GUID, every later one gets a fresh, unclaimed
  GUID and is rewritten. The determinism (never case-folded — `'Z.png'` sorts before `'a.png'`, byte
  order) is what stops two developers on two machines ping-ponging the "fix" back and forth forever.
- **The move/rename invariant (INV-A8, D10) — a rule for 3.1.3 to meet, not yet enforced:** an editor
  operation that moves, renames or copies an asset MUST move, rename or copy its `.meta` in the same
  operation. A move that drops the sidecar is a silent identity loss — every scene reference to that
  asset dangles, and **no test in this tree can see it** happen.
- **INV-A8 is ENFORCED since task E.4.3.** Every rename, move and delete carries the `.meta` in the
  SAME operation, planned by `planAssetOp` before a byte moves.

  **The order is fixed and is not a preference: the ASSET renames FIRST, the SIDECAR second, and a
  failure on the second rolls the first back.** There is no portable two-file atomic rename, so the
  question is only which torn state is least bad and which failure is most likely — and the
  overwhelmingly common failure (the destination name is taken, the volume is read-only, the OS
  refuses) happens on the FIRST rename, where nothing has happened yet and the refusal is free. A
  failure of the rollback itself is a loud WARN naming BOTH paths, and 3.1.2's content-hash
  re-attachment (`planReattachments`, run by every scan's phase 5) is the net beneath it — **named as
  a net and never as the plan**: it needs the import cache to hold an entry for that GUID, which a
  fresh clone, an `invalidateCache()` or a `Reimport All` leaves it without.

  **Every operation is ALL-OR-NOTHING.** `AssetOpResult::performed` is true only when BOTH renames
  landed, for every operation including Delete. The tempting exception — the file went, only the
  sidecar stayed — produces an orphaned sidecar the user did not ask to create, and a single rule
  with no exceptions is the only kind this tree has been able to keep.

  **A folder operation is ONE rename and needs no loop**: a folder has no GUID and no sidecar, and
  everything beneath it travels by construction. `AA55` asserts that structurally rather than by
  iteration.

  **A REFUSAL still rescans** (`deleteOrphanMeta`'s rule): every refusal reason means "the tree
  changed under us". The ONE exception is the dirty-open-material refusal, where nothing changed.

  **THE CASE-ONLY CARVE-OUT IS GATED ON `std::filesystem::equivalent`, never on "same leaf modulo
  case, same directory" alone.** That lexical condition justifies itself with "the only entry it can
  be is the source itself", which is true on a case-INSENSITIVE volume and FALSE on a case-sensitive
  one — where the executor's live free-name check would then wave through a rename that OVERWRITES a
  genuinely different file, in exactly the window (something created between the `listDirectory` and
  the act) that check exists to close. It shipped that way once and the code-review round caught it.
  The equivalence is true precisely on the volumes where the carve-out is legitimate; the
  `error_code` overload fails SAFE, to a refusal. **The refusing arm is unobservable on macOS and
  Windows** — `AA64` states the `TMPDIR`-on-a-case-sensitive-image recipe that reaches it locally.

  **`RollbackFailed` and `AssetOpResult::torn` have NO automated cover anywhere, and cannot.** A
  rollback fails only when something occupies the source path as a non-empty directory (measured on
  APFS: renaming onto an existing FILE succeeds silently; renaming onto a non-empty directory fails
  with EISDIR), and step 5 is what VACATES that path — so the occupant would have to appear between
  step 5 and the rollback, which only a concurrent external actor can do. `AA60` keeps the reachable
  arm and says so in its own comment.
- **The panel holds a `const AssetDatabase*`, reconciled every tick — never a reference member (D13).**
  `EditorApp` is movable and `create()` returns `std::optional<EditorApp>`, so every live editor has
  been through at least one move. A reference bound at panel-construction time binds to whatever
  address the pre-move `EditorApp` happened to occupy. Sabotage seed S23 tried exactly this and — on
  this machine, under ASan, through every test this tree drives — **reddened nothing**, a real,
  documented coverage gap rather than proof the reference form is safe: the likely reason is that this
  build's `EditorApp::create()` elides the conversion-move of its named local into the returned
  `std::optional`, so the address a reference would have captured never actually moves on this specific
  compiler/build combination. Do not read that green run as license to switch back to a reference.
- **`.aero-tmp` is skipped by the scan, not deleted (D16).** `writeTextFileAtomic` transiently creates
  `<path>.aero-tmp` inside the user's own assets tree; a killed editor leaves one behind. Without its
  entry in the ignore roster (`IGNORED_ASSET_NAME_SUFFIXES`, which names `ATOMIC_TEMP_SUFFIX` by
  identifier and which `isIgnoredAssetName` reads -- task E.4.4), a leftover `wood.png.meta.aero-tmp`
  would itself be treated as a scannable asset and given `wood.png.meta.aero-tmp.meta`.
- **The panel's root and the database's root are the SAME string by construction (INV-A9/A16).** Both
  are reconciled from `project.assetsRoot()` in the same `EditorApp::tick()` block, which is the only
  reason `AssetBrowserPanel::selectedEntry` (relative to the panel's root) is a valid
  `AssetDatabase::findByPath` key. If they ever diverge, the footer silently shows `no .meta` for
  **every** file — no crash, no log, no red test (confirmed directly: sabotage seed S25, dropping the
  panel's database-pointer reconcile, reddens nothing in this tree's automated suite either — AC-37's
  whole surface is human validation rows 3 and 8).
- **`asset_meta.cpp` and `asset_database.cpp` never log (INV-A3).** `rescan` returns a report; ALL
  logging for the asset scan lives in `editor_app.cpp`'s `logAssetScan`, called from `tick()`'s
  reconcile block AFTER `rescan()` returns — a pre-write announcement was considered (D14) and rejected
  as unachievable through a report-returning function and worthless anyway, since the Console panel's
  `pumpLog()` runs at the top of the NEXT tick regardless of when within the current tick a line was
  logged.
- **`EditorApp::tick()`'s reconcile block does double duty, and that is load-bearing to remember when
  reading or changing it.** The block that scans the database (D12, task 3.1.1) is the SAME block that
  already existed for the Asset Browser's panel-root reconcile (D10, task 2.6.1) — 3.1.1 extended the
  existing block rather than adding a second one beside it. A seed that deletes "the reconcile block"
  wholesale (S22) therefore reddens FOUR GPU-tier cases, not one: I21 (2.6.1's own panel-root case),
  I27, I28 and I29 (3.1.1's database cases) — confirmed directly, and a real deviation from what a
  reader might expect from the two tasks' separate D-numbers.
- **`AssetBrowserPanel::database()`'s accessor and the `databasePtr` member cannot share a name (D13's
  naming note).** Neither `AssetDatabase::findByGuid` nor `AssetDatabase::findByPath` is involved — the
  collision is entirely within `AssetBrowserPanel` itself (code-review finding 6, correcting this
  paragraph's earlier, wrong attribution). The member is `databasePtr`, the accessor `database()` — the
  `RenderTarget::depthFormatValue` / `depthFormat()` precedent from 2.3.1, applied a second time to this
  exact class of collision.
- **The ignore roster is ONE value, and a name-freedom check never filters by it (task E.4.4, D9).**
  `IGNORED_ASSET_NAMES`, `IGNORED_ASSET_NAME_SUFFIXES` and `BLEND_BACKUP_STEM` in `asset_meta.hpp` are the
  whole of "not an asset". Add an entry THERE, and to `docs/09` §5.10's table in the same commit — never a
  fifth term in `isScannableAssetName`. Every consumer COMPOSES `isIgnoredAssetName` rather than restating
  it: the scan refuses hidden + sidecar + ignored; the browser's listing refuses sidecar + ignored through
  `isBrowserVisibleName`, whose `isDirectory` term comes FIRST (a folder named `backup.bak` is a folder the
  user made) and which deliberately omits the hidden rule (`listDirectory` owns it, behind `Show hidden`);
  the watcher is scannable-or-sidecar by composition. **But a check that asks "is this name already TAKEN?"
  lists UNFILTERED**: an ignored file still owns its name on disk, so filtering New Material's listing
  (`createMaterialAsset`) or E.4.3's New Folder / Rename / Move free-name listings (`createAssetFolder`,
  `renameAssetEntry`, `moveAssetEntry`) by either predicate would hand out a name that exists and let a
  write or a rename land over the user's file. **Refusing an ignored name as a rename or new-folder TARGET
  is the opposite question and is correct** — the file would vanish from the browser — and
  `validateAssetName`'s `IgnoredName` arm does it, for a folder name too, because it sees a leaf with no
  file/folder context. `asset_view_test.cpp`'s `BV9` pins the exact set of editor files that may name
  either predicate; a new consumer adds itself there, out loud. **`deleteOrphanMeta`'s `AssetPresent`
  refusal (E22) is scoped to a SCANNABLE NAME** — `isScannableAssetName(assetLeaf) && fileExists(...)`,
  on the LEAF and never the relative path — and it must stay exactly that. Widening it back to "any file
  exists" makes every pre-E.4.4 Blender project's orphaned `.blend1.meta` undeletable from the editor,
  since the ignored backup beside it never goes away (`AA67`, `AD76`); narrowing it to an exact-case
  pairing test deletes the sidecar a case-only rename leaves behind on a case-insensitive volume, and
  the GUID with it.

## Import cache (task 3.1.2)

- **The cache is NEVER in `.meta` (D3).** `.meta` v1 is untouched by this task: no new key, no version
  bump, no parser change beyond the additive `Reattached` state. A content hash in a committed file
  would need a third write path, repealing D6 — the whole reason the cache lives at
  `<projectRoot>/Library/asset-cache.json` instead.
- **Derived data is disposable (D7), and the policy is the deliberate INVERSE of `.meta`'s.** `.meta`
  discards a bad key and preserves everything else, because losing a `.meta` loses an identity
  permanently. The cache discards the WHOLE file on any structural failure and never repairs a single
  bad entry, because losing the cache costs exactly one re-import — the cheaper failure mode gets the
  cheaper, blunter policy.
- **A scan of an unchanged project writes ZERO bytes anywhere (D15 / INV-C5) — not "identical bytes",
  zero — and now across TWO files, not one.** The index is written only when its text differs from the
  text it was read from. The writer must stay deterministic (fixed key order, entries sorted by
  `guid`), because non-determinism silently reintroduces a per-scan write with no error and no failing
  test to catch it.
- **The cache is keyed by GUID, never by path (D11).** Moving an asset together with its sidecar must
  not invalidate its import — the entry's `path` is updated in place, its `contentHash` stays valid.
  An asset with no valid identity (no `.meta`, or an invalid one) is never in the cache at all.
- **Orphan re-attachment needs ALL FIVE of D13's conditions, or it does nothing — never a near-miss
  log.** The candidate has no sidecar; its content hash was computed this scan (not skipped by budget);
  exactly one previous cache entry has that hash **and** a path this scan no longer sees; that entry's
  GUID is claimed by no live asset; and exactly one orphaned sidecar on disk parses to that GUID. Any
  one condition failing means nothing happens — no match, no partial state. The old, now-redundant
  sidecar is never deleted; a "Delete orphaned .meta" action is user-initiated and is 3.1.3's, not
  this task's.
- **D9 and its correction — the alias rule is DIRECTORY-ONLY and keys on canonical path, never on
  content.** A content hash CANNOT fix the symlinked-directory duplicate-GUID defect: it cannot
  distinguish one file reached twice from two legitimate identical copies, and using it to suppress a
  second record would silently delete the identity of every duplicated texture in every project.
  File-level symlinks and hardlinks are deliberately **not** deduped — only a symlinked *directory*,
  by its resolved canonical path. **This corrects an earlier, wrong rationale recorded in
  `docs/10-engineering-log.md`'s 3.1.1 entry**, which said the content hash was why 3.1.2 would own
  this defect; it was not, and the correction is recorded in that same file. 2.2.4's D13 ("a symlinked
  asset folder behaves like a real one") **remains true for the Asset Browser** and changes **only for
  the asset scan** — the split is deliberate, record it explicitly whenever either D13 is cited.
- **D18's guard nuance, easy to misread as a contradiction of D7.** The cache's DATA is disposable, but
  nothing in this task deletes a file to dispose of it: a discard means "do not carry entries
  forward", `Reimport All` means clear the in-memory index, a rebuild means an atomic overwrite. Listing
  `asset_cache.cpp` in `check-project-no-delete.sh`'s allowlist costs nothing today and stops a future
  "clean the Library folder" `remove_all` from being written without a review; if that is ever wanted,
  it is a deliberate, reviewed relaxation that must delete only inside `Library/`, never inside the
  assets tree.
- **The amended INV-A1.** 3.1.1's "exactly one `writeTextFileAtomic` call site" was about the assets
  tree only. This task adds two more call sites, both building their path from the library directory —
  the amended invariant is exactly **one** call built from the **assets** root and exactly **two**
  built from the **library** directory, each path assembled into a named local first
  (`metaAbsolutePath`, `ignorePath`, `indexPath`) so the invariant stays grep-decidable rather than
  heuristic.
- **A4's trap carries over from `Guid` to `ContentHash`.** `ContentHash::valid()` is false for the
  digest of the empty input, which is a perfectly legitimate value — it is NOT a "was this hashed?"
  flag. The only such flag is engagement of `std::optional<ContentHash>`. `parseAssetCache` accepts a
  nil `contentHash`/`metaHash`; only `guid` may never be nil.
- **A2's trap: `FileEntry`'s three new fields (`mtime`, `mtimeKnown`, `isSymlink`) are APPENDED, never
  inserted.** `tests/editor/project_files_test.cpp` holds two positional aggregate initializers of
  `FileEntry`; inserting a field ahead of `isDirectory` would silently re-map them (`bool` → `int64` is
  a promotion, not a narrowing conversion, so nothing diagnoses it). Both helpers were converted to
  designated initializers as defence in depth for the *next* field addition.
- **A3's cost, measured, not assumed.** `isSymlink` is free everywhere (the cached `d_type`/symlink
  status from the directory iteration). `mtime` costs one extra `stat` per file on POSIX (libc++ and
  libstdc++ both take the uncached branch) and nothing on Windows (`FindFirstFileW` already returns it).
  Do not add `entry.refresh(ec)` to make it "free" — that changes the dangling-symlink asymmetry
  2.2.4's code review already had to discover and fix once.
- **INV-C9 is WIDENED at task E.4.2: `canonicalDirectory`'s result is a dedup key OR a COMPARISON key,
  and nothing else.** The second use is `scene_containment.cpp`, which canonicalises a project root and a
  scene's parent directory, compares them and **discards both** — nothing is stored in a record, put in a
  report, written to the cache, or shown to the user, which is exactly what INV-C9 forbids and exactly
  what that use does not do. The forward-slash normalisation inside `canonicalDirectory` is what makes
  such a comparison valid on Windows, and it exists for the identical reason. The widening is spelled in
  `project_files.hpp`'s own header comment and here, changed together — **a third use must state which of
  the two it is, and a use that RECORDS or DISPLAYS the value is still forbidden**: project roots are
  `absolute`, not `weakly_canonical`, on purpose, so a project reached through a symlink must never be
  silently recorded under its target.

## Asset browser v1 (task 3.1.3)

- **THE LEDGER AND THE STORE LIVE IN `ThumbnailService` (task E.3.3), owned by `EditorApp` through a
  `unique_ptr`, with THREE consumers and ONE shared budget.** A `unique_ptr` and not a value member,
  because the service owns a `ThumbnailStore` whose header is src-private and GPU-touching while
  `editor_app.hpp` is a PUBLIC header held ImGui-free and GPU-free by file placement — the
  `sceneAssetLoader` posture. The heap object's address survives an `EditorApp` move, so each consumer
  is handed the pointer **once, in `create()`**, never reconciled per tick. The service pass keeps its
  slot and its statement position in `tick()`'s post-draw block and its internal order
  (touch → reimport clear → superseded sweep → evict → decode); **`++frame` is its FIRST statement**,
  and advancing the clock after the touch loop instead frees a texture this frame's draw list still
  names — synchronous on Vulkan/D3D12, deferred on Metal. A consumer **notes** keys in the draw walk
  and **never** mutates. `thumbnailKeyForRecord` is the pure five-guard half, over a RECORD, because a
  picker candidate is a record and not a `(FileEntry, path)` pair; the browser keeps guards 1 (a folder)
  and 3 (no database) and composes the rest through it.
  **Since task E.4.5:** the service owns a SECOND backing store, `MaterialThumbnailRenderer`
  (`material_thumbnail.{hpp,cpp}`), and the pass spends TWO per-producer budgets —
  `MAX_THUMBNAIL_DECODES_PER_TICK` and `MAX_THUMBNAIL_RENDERS_PER_TICK` — beside the card cache's own
  `MAX_MATERIAL_CARD_READS_PER_TICK`, so there is no longer ONE shared budget. And the internal order gains
  the card pass, which runs after `++frame` and ABOVE the device gate: `++frame` → card pass → device gate →
  touch → reimport clear → superseded sweep → evict → the decode/render walk.
- **Thumbnails are a strict two-phase system, and the phases live in different TUs on purpose.**
  `editor/include/aero/editor/thumbnail_cache.hpp` (`ThumbnailLedger`) is PURE — no ImGui, no
  `<filesystem>`, no GPU — and owns only the key, the `Absent`/`Ready`/`Failed`/`Skipped` state
  machine, and a budget/LRU eviction policy; `editor/src/thumbnail_store.hpp` (src-private) is the
  ONLY stb_image TU and the ONLY GPU-touching TU for thumbnails, and does the actual
  read → decode → resample → upload. Nothing above the pair (the panel, `EditorApp`) ever sees a
  decoded pixel or an `rhi::TextureHandle` — only `ThumbnailState`/`ThumbnailKey`.
  **Since task E.4.5:** `material_thumbnail.cpp` is a SECOND GPU-touching TU for thumbnails (it renders, and
  includes no stb_image of its own — its slot textures load through `loadTextureFromSourceFile`), and
  `EditorApp::materialThumbnailTargetFor` returns a `const render::RenderTarget*` — a read-only observable
  for the GPU tier's pixel read, whose `colorTexture()` is an `rhi::TextureHandle` — so `EditorApp` does see
  one. No panel does, and no decoded pixel crosses either.
- **A `ThumbnailKey` is `{Guid, ContentHash}`, never a bare `Guid`.** A record whose content hash was
  never computed this scan (skipped by budget, or `metaWriteFailed`) has no valid key at all and is
  never touched — a garbage/zero key would decode a file that was never actually hashed. Dropping
  either guard is sabotage seeds S9/S10, both `I36`-visible only when the GPU-tier test's own budget
  is small enough to force an un-hashed record into existence.
- **`Failed` and `Skipped` are STICKY, and `nextDecodes()` is where that stickiness is enforced — not
  at the call site, where a future edit could forget it.** A file that failed to decode once is never
  retried, forever, in the same session: `nextDecodes` returns `Absent` keys only, oldest-touched
  first. This is the single most important property in the whole subsystem (D9) — without it, a
  broken image in a folder re-reads and re-fails every tick, forever, at the decode budget's full
  rate.
- **Eviction excludes anything touched THIS frame, even when that leaves the resident count above the
  cap.** Evicting a tile that is currently on screen would decode it again next tick, forever, in any
  folder with more visible tiles than `MAX_THUMBNAILS_RESIDENT` (E12) — a thrash loop, not a bug that
  merely wastes memory.
- **`serviceThumbnails()` runs OUTSIDE the ImGui draw walk, in the same slot as `renderScene()`
  (`EditorApp::tick()`, between `drawShellUi` and `endFrame`), never from inside `onDraw()`.** This is
  a real, load-bearing architectural rule, but **no automated test tier can see it violated in the
  general case** — sabotage proved this precisely: moving the call to the very START of `onDraw()`
  (before any tile has been touched that frame) DOES redden four GPU-tier cases, because thumbnails
  then decode a full frame behind their own visibility; moving it to the natural END of `onDraw()`
  (after every tile has already been drawn and touched) reddens nothing at all, 64/64 green. Get the
  call site right by construction — human validation row 4 ("never stutters") is this rule's only
  general-case cover.
- **`fitRgbaIntoTile`'s box-filter resampler is 100% INTEGER — no floating point anywhere.** This is
  what makes a decoded thumbnail's output bytes identical on macOS, Windows and Linux, and therefore
  assertable in a portable test at all; a float accumulation would be exactly representable for the
  sample counts this project's images produce (≤ 500 000 uint8 samples fit exactly below 2²⁴), so a
  seed swapping in `float` accumulation is a documented, machine-dependent non-discriminator here
  (S7) — never read that as license to introduce floating point into this one function.
- **The orphan-`.meta`-delete action re-verifies ALL FIVE conditions on the real filesystem, in a
  fixed order, immediately before deleting — never trusts a stale scan result.** `validateOrphanPath`
  (no `..` segment, no absolute path, no drive letter, no `\` separator, a real `.meta` filename) →
  the sidecar still exists → it still reads as text → it still parses as a `.meta` v1 sidecar with a
  valid GUID → **the asset it describes does not exist again**, for a scannable name only since E.4.4
  (the race-closing check: something
  could have re-created the asset between the scan and the click). Any single failure refuses the
  delete and leaves the file untouched — never a partial state, never a "probably fine" heuristic.
  Reordering this sequence (sabotage seed S22) is the single most important seed in the delete half:
  it reddens every "still on disk after a refusal" case in the suite simultaneously.
- **Check B (`check-project-no-delete.sh`) is a POSITIVE two-file allowlist, added beside Check A's
  six-file denylist, not instead of it.** Check A only catches a delete/rename/copy written into one
  of six NAMED files; a new destructive call in a SEVENTH, unnamed `editor/src/*.cpp` file passes
  Check A silently. Check B closes that hole by scanning every tracked `editor/src/*.cpp` and refusing
  a `remove_all`/`std::filesystem::remove`/`std::filesystem::rename` outside
  `PERMITTED_DELETERS = {text_file.cpp, asset_actions.cpp}`. **`::copy` is deliberately NOT in Check
  B's pattern** — unlike Check A's `FORBIDDEN_RE`, Check B's `DELETE_RE` would false-positive on the
  first ordinary `std::copy` (an `<algorithm>` call, not a filesystem one) written anywhere under
  `editor/src/`, and a guard that cries wolf is a guard that gets relaxed. `text_file.cpp` is
  permitted because its internal `rename` IS the mechanism that makes `writeTextFileAtomic` atomic;
  `asset_actions.cpp` is permitted because it holds the ONE sanctioned orphan-sidecar delete (D12/D13
  above). A third self-test (B-self-test 3) makes renaming `asset_actions.cpp` without updating
  `PERMITTED_DELETERS` fail LOUDLY (exit 2, "cannot self-verify") rather than silently widening the
  check to "nobody is permitted, so nothing matches, so we pass" (sabotage seed S24).
- **`inputTextString`, not `imgui_stdlib.h`, for the search box (A1)** — see the corrected
  `<imgui_stdlib.h>` entry in the ImGui section above; this task's search box is imgui_stdlib.h's
  second confirmed non-use, not its first.
- **A hidden Asset Browser panel still runs `serviceThumbnails()` every tick (A5/AC-34).** The
  thumbnail budget/eviction pass is NOT gated on the panel's own visibility — a tabbed-away Asset
  Browser keeps decoding and evicting exactly as if it were on screen. This is a deliberate choice,
  not an oversight: gating the service call on visibility would make "switch to the Assets tab" a
  visible multi-tick stutter every time, trading a background cost nobody sees for a foreground one
  everybody would.

## Hot-reload watcher (task 3.1.4)

- **The watcher's VISIBLE SET must equal the scan's, byte for byte, or the editor rescans forever
  (D4/INV-W3).** If the watcher observes one file the scan ignores, and that file changes, the
  watcher fires a rescan; the rescan writes nothing and reports nothing; the watcher fires again next
  sweep; forever. `.DS_Store` alone would do it on every macOS machine. The equality is held by
  `isWatchableAssetName` (`asset_meta.hpp` — exactly `isScannableAssetName(n) || isMetaFileName(n)`,
  defined beside the two predicates it composes) plus IDENTICAL traversal bounds:
  `includeHidden=false`, `MAX_TREE_DEPTH`, `MAX_ASSETS`, the symlink-only `canonicalDirectory`
  optimisation, and the canonical `Library/` exclusion. **A change to either walk is a change to
  both.**
- **`Library/` is excluded by CANONICAL PATH, derived from the PROJECT root — never from
  `assetsRoot + "/.."`.** `paths.assets` is user-configurable, and a project whose assets root is
  `"."` puts `Library/` inside the watched tree, where every scan may rewrite
  `Library/asset-cache.json`. **This is the one exclusion no normal project's tests can reach**, and
  `AW34`'s `Library/` arm — driven with an assets root equal to the project root — is the only place
  in the tree that proves it (sabotage confirmed `AW41`'s arm (c), a project-root-only variant, also
  discriminates the same guard).
- **A sub-directory whose listing FAILS carries its committed children forward; an unreadable ROOT
  aborts the sweep (D5/INV-W6).** An antivirus lock, a cloud-sync pass or a permission change would
  otherwise present as "every file under it was deleted" — which means dropped records, orphan
  reports, released GPU thumbnails, and the exact reverse one sweep later. **Absence observed through
  a failure is not absence.** This is 3.1.1 D8's instinct ("an orphan is reported and left on disk,
  never deleted") and `canonicalDirectory`'s ("a caller that cannot prove two paths are distinct must
  refuse to descend, never guess"), applied a third time.
- **Settlement has TWO conditions and both are decided in `isSettled`, never at a call site
  (INV-W4).** Age alone is wrong on Windows (NTFS does not flush last-write-time while a handle is
  open, so a 2 GB copy can present a stale, OLD mtime for its whole duration); stability alone is
  wrong on FAT32/HFS+ (a 1–2 s mtime quantum makes two same-size writes indistinguishable). That is
  why `WATCH_SETTLE_MS` is **2000** — above FAT32's own quantum, not merely "a bit".
- **An unsettled entry's stamp never enters `committed` (INV-W5/AC-12)**, including on a FORCED fire:
  `applyChanges` applies only the settled changes. The single sanctioned exception is
  `noteExternalScan()`, which adopts `lastProbe` wholesale — safe because a torn stamp means the file
  was still growing, so its final stamp necessarily differs and the next two sweeps still report it.
  Do not "fix" that into `primeOnNextSweep`.
- **`asset_watcher.{hpp,cpp}` never log, never write, never rename, never copy and never delete
  (INV-W1).** `asset_watcher.cpp` must **never** be added to `check-project-no-delete.sh`'s Check B
  `PERMITTED_DELETERS` allowlist — being outside it is exactly what makes a future
  `std::filesystem::remove` there a hard CI failure. It is also `<filesystem>`-free, `<fstream>`-free,
  `<thread>`/`<mutex>`/`<atomic>`-free, and performs **zero file reads**: `poll()` opens directories
  and reads nothing.
- **The watcher POLLS, and that is a decision with recorded reversal conditions, not a default.** No
  FSEvents, no `ReadDirectoryChangesW`, no inotify, no owned thread, no `#ifdef` — the watcher itself
  contains no per-OS branch and no `JobSystem` reference, and this task keeps it that way.
  **The claim this bullet used to make — that `git grep -n '_WIN32\|__APPLE__\|__linux__' -- editor/`
  is EMPTY — has been false since task 3.2.2**, which vendored ufbx into `editor/third_party/` with
  three of upstream's own per-OS lines, and it is false a second time since task 3.2.4, whose
  `currentHostOs()` adds exactly three more. The live invariant is the one in the Blender CLI section
  below: over `editor/src` + `editor/include`, exactly three lines in exactly one file. A native backend replaces `poll()` behind the
  same `AssetWatcher` seam without touching a consumer, and only once a measured sweep cost exceeds
  ~2 ms/tick or a script-reload loop makes 1–2 s of latency painful.
- **GPU textures are destroyed ONLY from `serviceThumbnails()`, only AFTER its touch loop, and only
  for keys not touched at `currentFrame` (D9/INV-W8).** `ThumbnailLedger::supersededBy` takes
  `currentFrame` as a PARAMETER precisely so this is structural rather than a call-site convention.
  This is 3.1.3's BLOCKING-1: `SDL_ReleaseGPUTexture` frees synchronously on Vulkan/D3D12 and only
  defers on Metal, so the bug is deterministic on Windows and Linux and invisible on the only
  platform with a human pass. **Confirmed directly**: sabotage seed S23 (moving the sweep into
  `onDraw()`) reddened no runtime test on Metal, matching 3.1.3's own S25 precedent exactly, and is
  caught only by a human reading `git grep -n 'supersededBy' -- editor/src/`'s two lines and noticing
  which function the call now sits inside.
- **`AssetDatabase::generation()` is the ONE signal that drives every downstream refresh (D8).** It is
  bumped as the FIRST statement of `rescan()` — not the last — because `rescan()` has three return
  paths. Do not derive "a scan happened" from the report's contents or from `assetRescanRequested`;
  both miss a path.
- **`EditorApp::tick()`'s reconcile block now does QUADRUPLE duty** — 2.6.1's panel root, 3.1.1's
  database, 3.1.3's report, and 3.1.4's watcher. Extend it; never twin it. A seed that deletes the
  block wholesale reddens a growing number of GPU cases (four at 3.1.1; more now) — confirmed
  directly: sabotage seed S15 (skipping ONE STATEMENT inside it, the priming branch, not even the
  whole block) already reddens **twelve** tier-0 cases by itself, since every case that builds a
  quiescent baseline depends on the very first sweep priming silently.
- **`AssetWatcher` holds no `std::unordered_map` and no `std::set` (INV-W9).** It is a value member of
  `EditorApp`, whose move is `noexcept = default`; MSVC's node-based containers are not
  nothrow-movable (3.1.2's R9, measured in CI as C2607). Sorted `std::vector`s only, with the
  `recordList` / `byGuid` / `ThumbnailLedger::entries` precedents. **Confirmed as a Windows-only
  discriminator**: sabotage seed S25 (swapping `visitedCanonical` for a `std::set`) compiles clean and
  reddens nothing on macOS/libc++, whose `std::set` move constructor happens to be `noexcept` too —
  the aggregate `static_assert`s are the ONLY thing standing between this and a real MSVC C2607.
- **The FIRST scan of a fresh project WRITES `.meta` sidecars, which the next sweep legitimately sees
  as additions and acts on exactly once (D4/E17) — and this is NOT limited to the first scan.**
  **Confirmed directly, correcting the task's own plan**: ANY brand-new file with no prior sidecar
  produces the SAME two-trigger echo (the file, then its freshly-written `.meta`) — a plan draft that
  asserts `+1` trigger for a later file is wrong for the identical reason the first-scan case is `+2`,
  not `+1`. Every GPU-tier case in this task ticks to QUIESCENCE first and asserts a trigger-count
  DELTA, never an absolute value, and — after any write of a genuinely new file — a SECOND quiescence,
  never a fixed sweep count.
- **Proving `noteExternalScan()`'s call site needs a MODIFICATION and a sweep that cannot complete —
  never a poison file.** Two independently-designed GPU-tier scenarios (`I50`'s) both failed to
  discriminate the call site, and the conclusion first recorded here — that no count-only accessor
  ever could, and that closing it needed a new seam exposing `lastDiff()`'s paths — was **wrong**, as
  the code-review round showed. The defeating property was the scenarios' own **permanently unsettled
  poison file**: the eventual forced fire produces a trigger whether or not the call ran, so the
  re-reported path merely rides along inside it and the signal collapses from "0 vs 1 trigger" to
  "2 vs 3 items inside one trigger". `I51` closes it with the existing accessors: a **modification**
  (D6 never rewrites a valid `.meta`, and the hash lives in the excluded `Library/`, so there is no
  sidecar echo for a duplicate to hide inside) plus `dirsPerPoll == 1` over a two-directory tree, so
  the tick carrying the manual rescan provably cannot complete a sweep and `watchFired` is therefore
  false — the only branch that reaches `noteExternalScan()`. With the call, delta 0; without it,
  delta 1. **The general lesson: a scenario that ends in a FORCED fire cannot discriminate anything
  about what a trigger contained, because the trigger was going to happen regardless.**

## Materials (task 3.4.2)

- **`"Material"` is FROZEN** — the panel id is both the ImGui window name and the `imgui.ini` settings
  key, so renaming it orphans every saved layout. `imgui_layer_test.cpp`'s `frozenPanelIds` array is the
  pin, and it now covers all eight ids rather than the first six.
- **The target is STICKY: only a DIFFERENT, EXISTING `.aeromat` retargets the panel.** Selecting a
  texture, a folder or nothing leaves it alone. Import Details keeps retargeting on everything; the two
  are **tabs, not shared state**. Invert this and every click made while hunting for a texture to
  reference destroys an unapplied edit session — the browser has exactly one selection. The target
  clears only when its record disappears from the database, or on a project swap.
- **`.aeromat` bytes are written by exactly two logical operations — Apply and New Material — through
  ONE helper**, `saveMaterialFile` in `material_session.cpp`, whose body holds the single
  `writeTextFileAtomic` call site. **The amended INV-A1**: the assets-root call-site count is now
  **two** (`asset_database.cpp`'s `metaAbsolutePath`, `material_session.cpp`'s helper) and the
  library-directory count is still two, with every absolute path assembled into a **named local**
  (`materialAbsolutePath`) so the invariant stays a grep rather than a heuristic. No scan, rescan,
  watcher, selection or draw path writes one, ever.
- **Dirty is `sessionCopy != fileCopy`** through `MaterialDocument`'s defaulted `==` — never "would
  Apply change the bytes". A valid but non-canonical file therefore loads **clean** and the editor never
  rewrites a file nobody edited. Apply validates first, writes **only when dirty**, and a validation
  failure changes nothing anywhere.
- **New Material refuses a directory it could not enumerate IN FULL.** `listDirectory` signals
  incompleteness three ways — `status`, `truncated`, `skipped` — and a truncated or partially-skipped
  listing still carries `ScanStatus::Ok` while handing back a **prefix**. Ask `listingIsComplete`, never
  `status == Ok`: a prefix cannot prove a name is unused, and acting on one renames the default document
  over an authored material with no warning and no undo. A caller that merely **displays** a listing is
  still right to ignore all of it.
- **`AssetRecord::contentHash` is meaningless unless the scan hashed the file this pass.** Ask
  `assetContentHashUsable`, never "is the digest zero?" — an all-zero digest is the **empty file's real
  value**, not a sentinel, and `metaWriteFailed` records are never assigned a `change` at all, so they
  read as `UpToDate` to any test on `change` alone. An unhashed record gets no cache key and no
  external-change notice.
- **Every preview GPU create and destroy lives in `servicePreview`**, never in `onDraw` — with **one
  deliberate exception, which is the whole rule below**.
- **THE RESIZE-BEFORE-READ ORDERING RULE — any future panel embedding a `RenderTarget` inherits this
  trap.** ImGui **records** an `ImTextureID` during the draw walk and **binds** it inside
  `ImGuiLayer::endFrame`, which runs **after** `tick()`'s post-draw service pass. `RenderTarget::allocate`
  destroys the previous pair first, and `RenderTarget`'s own comment — "the backend defers the actual GPU
  release" — is true of the device **memory** and **false of the handle**: in the pinned SDL 3.4.12 tree,
  Vulkan (`SDL_gpu_vulkan.c:7070-7073`) and D3D12 (`SDL_gpu_d3d12.c:1385, :1460`) `SDL_free` the container
  **immediately** ("Containers are just client handles, so we can destroy immediately") while Metal
  (`SDL_gpu_metal.m:936-944`) queues it. So a reallocation between the record and the bind is a **heap
  use-after-free on Vulkan and D3D12 and benign on Metal** — deterministic on the two platforms with no
  validation pass, invisible under every sanitizer on the one that has one. **Resize where the handle is
  read: inside the draw walk, immediately before the read** (`MaterialPreview::prepareFrame`,
  `ViewportPanel::onDraw`'s steps 5–8), and return false when there is nothing to bind — including the
  allocation-failure arm, where the previous pair is already gone and the `Image` must be skipped that
  very frame. This is why the viewport has never had the defect, and it shipped green in the Material
  panel until the code-review round.
- **A source-text pin must encode the property that matters, not a proxy for it.** `I96` originally
  greped for `destroyTexture(`/`destroyMaterial(` and required them inside the service function — and the
  destroy that mattered was inside `RenderTarget::resize` → `allocate`, invisible to that grep, so the
  defective code **satisfied** the pin. It now pins **ordering against ImGui's consumption**: `resize`
  belongs to `prepareFrame` and nowhere else, cache destroys stay in the service pass and the destructor,
  and `material_panel.cpp`'s three preview statements must read prepare, read, `Image`.
- **The reconcile block now does SEXTUPLE duty and the post-draw slot QUADRUPLE.** Extend; never twin.
- **`aero::render` is PUBLIC on `aero_editor_core`** so `material_edit.hpp` can be a public, tier-0
  testable header — the `aero::assets` precedent from 3.3.1, criterion for criterion.
  `aero::scene_render` stays PRIVATE; that distinction is the point, not an oversight, and it is why a
  public editor header may name `render::MaterialParams` but not a `SceneRenderer`.
- **The slot→colour-space rule is COMPOSED from `render::defaultTextureKindForSlot`**, never restated
  beside it — 3.4.1 deleted a hand-written per-slot table for exactly this reason. The preview's texture
  cache key is `(guid, contentHash, srgb)`, so one source in two slots with different spaces loads twice.
- **The Material panel writes files, and it still mutates nothing in the draw walk.** Every control
  writes into a per-frame copy and records ONE pending whole-document edit; `tick()` drains it. Numeric
  edits clamp **in C++** against the same ranges `material_format.cpp` validates with — an ImGui slider
  with a `v_min` still lets a Ctrl+Click type anything at all, and no tier in this tree can perform that
  click, so the clamp's only mechanical witness is a source-text pin (`I98`).
- **A reduced-configuration claim must name which binaries it ran.** `I88`–`I92` failed in a
  tools-OFF build (115/120) after an earlier probe of that path built `aero_editor_shell_test` only.
  `aero_editor_imgui_test` now carries `AERO_SHADER_TOOLS_ENABLED=1` inside its own
  `if(AERO_SHADER_TOOLS)` block, and both arms **assert** — a skip would leave AC-32 untested in the one
  configuration that can test it.

## Material thumbnails and names (task E.4.5)

- **A second producer, not a second cache.** `ThumbnailService` owns ONE `ThumbnailLedger` and TWO backing
  stores — `ThumbnailStore` (decoded images) and `MaterialThumbnailRenderer` (rendered materials) — routed
  by the total enumeration `thumbnailSourceForName`. **`releaseKey` is the one release site**: it asks both
  stores and forgets the key, so the "never free a texture drawn this frame" protection covers both. A third
  producer is a third `ThumbnailSource` enumerator, a third store, and a third line in `releaseKey` — the
  `-Wswitch` diagnostic at the walk says where. It fails CI's Linux clang-tidy step, where `.clang-tidy`
  enables `clang-diagnostic-switch`, and is at most a warning in the compile lanes. `I242(a)`/`(b)` pin both.
- **The walk sees every `Absent` key.** Tiles drawn in one frame share `lastTouched`, so the ledger offers
  them in GUID order; asking `nextDecodes` for "decodes + renders" keys hands the walk only the oldest few,
  which can all be one kind. `I233` pins it with GUIDs chosen through pre-written sidecars.
- **`residentCount()` and `loadAttempts()` mean the DECODE store; `readyCount()`/`unavailableCount()` mean
  the ledger.** The render store has its own counters, and at the end of every service pass
  `residentCount() + materialResidentCount() == readyCount()`.
- **The card cache (names and swatches) runs ABOVE the device gate, gates on kind BEFORE reading, and reads
  capped** (`readFileBytes` with `MAX_THUMBNAIL_SOURCE_BYTES`). It has its own request queue,
  `noteCardWanted` — never `noteVisible`, whose meaning E.3.3's `S34`/`I167` pin — and a card outlives its
  picture's eviction. A `cardFor` pointer, and a view into its name, are valid for the whole draw walk.
- **Never cache a file-NAME-dependent answer under a CONTENT key.** A rename keeps the `ThumbnailKey` (the
  import cache is keyed by GUID and a move carries its hash), so the stem suppression runs at DRAW time
  against the current leaf (`materialCardSubtitle`), in every host — `I242(d)` counts the calls per host.
  `I240` renames through E.4.3's seam and pins the behaviour.
- **A document name is user text: `TextUnformatted` or `ImDrawList::AddText`, never a format function.**
  `I242(e)` fails any `ImGui::Text`/`TextDisabled`/`TextWrapped`/`SetTooltip`/`BulletText` call in the four
  hosts whose first argument is not a string literal.
  **The full set since the code-review rounds:** the first-argument functions are `Text`, `TextDisabled`,
  `TextWrapped`, `SetTooltip`, `BulletText` and `SetItemTooltip`; `TextColored(col, fmt, ...)` and
  `LabelText(label, fmt, ...)` take the format string SECOND, and the pin reads their second argument after
  the first comma at parenthesis depth 0 OUTSIDE a string literal, honouring an escaped quote. A new format
  function a host starts calling joins the matching list.
- **The thumbnail's picture is a function of its key alone.** `produce()` takes no lighting and no tonemap;
  the rig is `material_card.hpp`'s. `I238` re-renders under a moved scene and requires identical bytes.
  **Except its slot textures (the code-review round):** `produce()` resolves each slot's GUID to the
  texture's CURRENT bytes at render time, so the picture is a function of the key AND those textures. A
  texture edit does not refresh it (R4, a recorded handoff); Reimport All refreshes it only for a tile NOT
  drawn in the frame the clear runs, because the clear spares every key drawn that frame; otherwise it
  refreshes once the `.aeromat`'s own bytes change, or once its key is evicted and the tile is drawn again.
- **A material GPU fixture must set `metallicFactor` to 0** — `MaterialDocument` defaults it to glTF's 1.0,
  and a metal with nothing to reflect renders near-black (E.1.4's trap).
- **An Apply reaches the browser through the watcher, not a nudge** — the contract every asset kind has; with
  Auto-refresh off, on the next Refresh. `I241` drives it at `settleMs = 0`.
- **A material RENDER is spent only on a tile drawn THIS frame, and an off-screen `Absent` material key is
  RELEASED, not kept pending (the code-review rounds).** The walk is oldest-touched-first, so without the first
  half the materials a user scrolled past render ahead of the page on screen; without the second, a key met off
  screen stays `Absent` for the session — `supersededBy` keeps a live key and `evictions` touches `Ready` keys
  only — and every idle tick's walk scans it. The release goes through `releaseKey` and is safe because the key
  was not drawn this frame. Decodes keep their own rule. `I243` and `TS10` pin it, through `absentCount()`.
- **A card is stored only when the bytes read hash to its key.** The gate trusts the DATABASE's key, which is
  stale from an external edit until the watcher's rescan, so the cache hashes what it read (`hashBytes`, the
  scan's own hasher) and stores a card only on a match. A parse failure of the key's own bytes is sticky; a
  failure the file's content did not decide — a mismatch, an OS read failure, a cap refusal — waits for the
  next rescan (`database.generation()` must change). `CC11`–`CC14`.
- **A subtitled tile's first line keeps the FILE NAME.** A search hit's caption source is `parent/leaf`, and a
  right-elision keeps the folder and drops the name, so line one is `subtitledTileCaptionLine`: the source when
  it fits, else an ellipsis and the longest suffix that still holds the whole leaf, else the leaf right-elided.
  Both caption rules are pure over an injected measurer (`asset_view.hpp`) and bisect over code-point
  boundaries, never bytes. `AV60`–`AV65`, and `I242(h)` for the call site.
- **`produce()` answers a key that already holds a target BEFORE any read or GPU work.** It is unreachable
  today — `releaseKey` destroys before the ledger forgets — and it is what keeps a mistake in that ordering a
  stale picture rather than a render into, and a replacement of, a texture ImGui may be sampling this frame.
  `I242(i)` pins the order as source text.
- **The thumbnail's framing is its OWN constant, deliberately different from the Material panel's preview.** A
  thumbnail is an identity cue, so the sphere must dominate the tile: `MATERIAL_THUMBNAIL_RIG`
  (`material_card.hpp`) frames it at about 80% of the width, where `DEFAULT_MATERIAL_PREVIEW_RIG` frames about
  27%. The sphere primitive's radius is **0.5**, not 1. Narrowing the field of view at the preview's pitch
  puts the whole frame below the horizon, so the elevation drops with it, and the key light is re-derived for
  the new eye with the same recipe. Never share one rig between the two: a retune of either would move the
  other. `MB23`, `MB25`, `MB26`, `I237(c)`/`(d)` and `I242(g)` pin it.

## Asset file operations (task E.4.3)

**THE SEGMENT-WISE PREFIX RULE NOW LIVES IN FIVE PLACES (E.4.3), AND A SIXTH NEEDS A SIXTH CASE.**
`assetOpPathLadder`'s rung 6 (`isInsideOrEqual`), `countRecordsUnder`, `assetOpBlockedByDirtyMaterial`,
`listingHolds`'s case-only carve-out, and `AA45(c)`'s claim — each with its own `("tex", "textures/a")`-shaped
case, because a raw `starts_with` is right on every input except the one that matters. Seed `S8` came back
green against the third of them until `I226` was written for it.

**`beginAssetDragSource`'s `isDirectory` IS NON-DEFAULTED AND THE SOURCE CANNOT DERIVE IT (E.4.3).** It has
only a path, and `classifyAssetKind`'s own `isDirectory` argument was hardcoded `false` there because until
E.4.3 a folder and an extension-less file took the same early return. They now take DIFFERENT arms — a folder
has no sidecar, so its plan has one step and not two — so a default would let a future call site silently
encode a folder as a file, with no error and no failing test. Three call sites; `DR27` pins the one surviving
`/*isDirectory=*/false` to the search-hit site, where it is correct because `searchAssets` never matches a
folder.
