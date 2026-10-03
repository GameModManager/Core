# GMM Modpack Format (`.gmmpack`) — v1.0.0 (supersedes v0.1 draft)

The normative spec for the format GMM's exporter writes and its importer
refuses anything that deviates from. The JSON Schemas it references live in
`schemas/` next to this file (`manifest.schema.json`, `mod.schema.json`,
`executable.schema.json`, `patch.schema.json`, `ini.schema.json`,
`tree.schema.json`) and are installed alongside the binary - the importer
validates every archive against them before it will open it.

## Archive layout

```
mypack.gmmpack                       (zip, deflate)
├── manifest.json                    # identity, schema version, info, tools, rules, load order hint, platform reqs, archive integrity hashes
├── tree.json                        # mod list order — separators, nested separators, mods. Pure UI organization.
├── mods/
│   ├── skyui.json
│   ├── awesome-mod.json
│   └── ...
├── files/                               # optional — mods that ship their own files
│   └── my-manual-mod/
│       ├── main.esp
│       └── scripts/foo.pex
├── executables/
│   ├── nemesis.json                 # setup tools + the pack's designated launch exe, see Executables below
│   └── ...
├── patches/
│   ├── skyui.json                   # single patch
│   ├── awesome-mod-1.json           # chain patch, applied in ascending numeric order
│   ├── awesome-mod-2.json
│   └── ...
├── ini/
│   ├── skyrim.json                  # ALL edits to Skyrim.ini, from any mod or the pack author, consolidated
│   ├── skyrimprefs.json
│   └── ...
└── instructions.md                  # optional, single file, shown in the install widget's side panel
```

No per-mod instructions files, no adult-content field, no bundled binary diffs sitting loose — every diff lives inside a small JSON wrapper (base64 payload + metadata), same as everything else in the archive.

A pack is a recipe, not a full modlist: every `mods/<id>.json` normally points at a provider GMM can resolve the archive from later. The one exception is a mod with no resolvable source at all — a manual or unrecognised-source mod. Those cannot be pointed at, so `files/<mod-id>/` carries their actual content, and the mod entry's `source.provider` is `embedded` (see `sourceEmbedded` in `schemas/mod.schema.json`). That is what makes "export my local mod" a real operation rather than a silent drop:

## `manifest.json`

```jsonc
{
  "gmmpackSchema": "1.0.0",            // strict semver — see Schema versioning below
  "id": "b3f1e2a0-...-uuid4",          // immutable identity, forever
  "revision": 2,                        // strictly increasing integer
  "info": {
    "name": "Frostpunk Survival Overhaul",
    "author": "...",
    "description": "...",
    "gmmGameId": "skyrim_se",
    "homepage": "https://...",
    "createdAt": "2026-09-01T00:00:00Z",
    "updatedAt": "2026-09-14T00:00:00Z"
  },
  "tools": [
    { "id": "loot", "name": "LOOT", "homepage": "https://loot.github.io" }
  ],
  "platform": {
    "linux": {
      "protonVersionPin": "GE-Proton9-27",   // advisory — see Platform-specific behavior below
      "steamOverlay": true,                   // maps directly onto LaunchParams.inject_steam_overlay
      "prefixFiles": [
        { "path": "drive_c/users/steamuser/AppData/Local/ENB/enblocal.ini", "sourceModId": "enb-preset" }
      ],
      "launchOptions": "WINEDLLOVERRIDES=\"d3d11=n,b\" %command%"
    },
    "macos": { /* same shape */ },
    "windows": { /* same shape, usually empty */ }
  },
  "rules": [
    { "type": "requires", "from": "off-site-patch", "to": "awesome-mod" },
    { "type": "before",   "from": "cleaned-plugin",  "to": "awesome-mod" }
  ],
  "loadOrder": {
    "pluginHint": ["AwesomeMod.esp", "OffSitePatch.esp"]   // weak — see Load order below
  },
  "archive": {
    "fileHashes": {
      "mods/skyui.json": "sha256:...",
      "patches/awesome-mod-1.json": "sha256:...",
      "ini/skyrim.json": "sha256:...",
      "tree.json": "sha256:..."
    }
  }
}
```

`rules[]` references stable `id`s, not filename/regex expressions — this is a deliberate departure from Nexus's `fileExpression` matching. It sidesteps the cross-platform case-sensitivity problem entirely for mod-level rules, since you're never matching a filename string, just comparing two ids. File-*path* matching (inside patches and ini targets) still needs case-insensitive handling — see below.

## `mods/<id>.json`

```jsonc
{
  "id": "skyui",
  "name": "SkyUI",
  "phase": 0,                          // install-sequencing bucket — NOT display order, see note above
  "category": "required",              // required | optional | recommended
  "source": {
    "provider": "nexus",
    "resolution": "api",
    "gameDomain": "skyrimspecialedition",
    "modId": 12345,
    "fileId": 67890,
    "version": "1.4.2",
    "fileName": "SkyUI-1.4.2.7z",
    "fileSize": 48213311,
    "sha256": "...",
    "updatePolicy": "exact"              // exact | latest — see note below
  },
  "installerChoices": { "type": "fomod", "selections": { "stepId": ["optionId"] } }
}
```

`updatePolicy` defaults to `exact` — pin to the exact `fileId`/version tested, matching the "rock solid, reproducible" goal. `latest` opts a specific mod out of that pinning (e.g. a script extender or a utility the author wants to always track current) — an update check for that mod compares against whatever the source reports as newest, not against the pack's own `revision` at all. This didn't exist in the v1.0.0 draft and is a real gap against what Collections already supports.

**One refinement worth stating precisely, since not every source can actually honor `exact`:** when `updatePolicy: exact`, `version`/`sha256` (and, on Nexus, `fileId`/`fileSize`) are genuinely required — the schema itself enforces this conditionally. When `updatePolicy: latest`, none of those are required or even meaningful to include, because there's no fixed value to pin — a pack author fabricating a hash for a "latest" mod would just guarantee a mismatch the moment the source updates. Steam Workshop can't offer `exact` at all — no discrete per-file identity or hash exists there, it's `latest` or nothing, so the schema locks that field to `const: "latest"` for that provider rather than pretending the choice exists.

Verification is correspondingly two-tier, not one strict rule everywhere: for `exact` mods, the resolved file's hash is checked against the pack's declared `sha256` — a match proceeds silently, a **mismatch surfaces as a non-blocking, integrity-flagged warning** (distinct from routine "unrecognized provider" noise) rather than a hard failure, and install proceeds with whatever was actually obtained rather than trusting a possibly-stale pack claim. For `latest` mods, there's nothing pack-declared to check against by design — the resolved hash still gets recorded into GMM's own instance state, purely so a later incremental update can detect drift without needing to have known the value in advance. This is the one place in the format that's deliberately *not* as strict as the schema versioning — third-party file hosting is outside GMM's control, and a hard failure there would make the format brittle exactly where it has the least leverage.

`id` is a filesystem-safe slug assigned once at pack creation and never changed, even if `name` is later edited — it's the stable key every other part of the format (rules, tree.json, patches, ini attribution, update diffing) hangs off of. That stability is what makes incremental updates possible.

### `source.provider: "embedded"` — a mod that ships its own files

```jsonc
"source": {
  "provider": "embedded",
  "resolution": "archive",
  "root": "my-manual-mod",             // directory under files/
  "fileCount": 2,
  "files": [
    { "path": "main.esp", "size": 4096, "sha256": "..." },
    { "path": "scripts/foo.pex", "size": 128, "sha256": "..." }
  ],
  "updatePolicy": "latest"
}
```

A mod with no resolvable source — installed by hand, from a folder the user dropped in, from a source GMM has no provider for — has nothing to point at. Rather than drop it from the export (which is what silently losing a mod looks like to the user), the packer copies the folder's files into `files/<root>/` and lists each one with its size and sha256. Install verifies **every** file's size and hash before writing anything, so a tampered archive fails without leaving a half-extracted mod behind, then writes the payload straight into the instance's mods folder under its pack id.

`updatePolicy` is locked to `latest` for the same reason Steam Workshop's is: the content is whatever *this revision of the pack* carries, so there is no separate version identity to pin. The per-file `sha256` list above is the integrity pin instead.

Every `files/**` entry is also covered by `manifest.archive.fileHashes` like any other archive member, so the whole-payload integrity check runs before the per-file one.

## `patches/<id>[-N].json`

```jsonc
{
  "modId": "awesome-mod",              // redundant with filename, deliberately — see integrity note
  "sequence": 1,                        // null/absent for a single non-chained patch
  "targetPath": "AwesomeMod.esp",
  "baseFileSha256": "...",              // hash of the file this patch expects to apply to
  "algorithm": "bsdiff",
  "payloadBase64": "..."
}
```

`modId` and `sequence` are stored inside the file *and* encoded in the filename. On load, GMM cross-checks the two and rejects the archive if they disagree — cheap insurance against a mis-extracted or hand-edited archive silently applying the wrong patch to the wrong mod.

**Patch consent, at install and whenever an update changes them:** before touching disk, scan `patches/`, build the affected-mod list, and block on a single dialog — scrollable list of patched mods, `Allow` / `Do Not Allow`. Allow → patches applied as authored. Do Not Allow → every patched mod installs at its base (unpatched) file state, and the diagnostics panel notes that the installed result differs from what the pack author tested, since that's genuinely useful for the user to know later when something doesn't behave as expected.

## `ini/<targetFile>.json`

```jsonc
{
  "targetFile": "Skyrim.ini",
  "tweaks": [
    {
      "id": "anisotropy",
      "name": "Anisotropic filtering",
      "status": "required",
      "enabled": true,
      "sourceModId": null,
      "content": "[Display]\niMaxAnisotropy=16\n"
    },
    {
      "id": "skyui-archives",
      "name": "SkyUI resource list",
      "status": "recommended",
      "enabled": true,
      "sourceModId": "skyui",
      "content": "[Archive]\nsResourceArchiveList2=...\n"
    }
  ]
}
```

Each tweak is one named, toggleable unit: `id` is a stable slug that never changes even if `name` is edited (same id-vs-name split as mod ids) — it's the key for update diffing, instance state, and tweak-scoped retract. `status` is `required` (the pack needs this to work as tested) or `recommended` (safe to leave disabled). `enabled` is the author default; it seeds fresh installs, thereafter the instance record owns the toggle. `content` is plain INI text — `[section]` headers plus `key=value` lines, blanks and `;`/`#` comments allowed; the engine parses it into key/value edits internally (duplicate keys within one tweak: last wins; keys without a section or lines without `=` are rejected).

`sourceModId: null` = pack-author tweak. Non-null = attributed to a specific mod's recommended tweak — lets the install widget (and later, the update diff) retract a tweak automatically if its owning mod is removed, and lets the UI show provenance if a user wants to know why a value changed. Key-level merge still applies inside the engine: a pack-author edit beats a mod-attributed one for the same key, otherwise last wins; same-valued duplicates are not conflicts.

## `tree.json`

Mirrors whatever internal tree model the nested-separator mod list view already uses — this should be a near-direct serialization of that structure, not a parallel format GMM has to keep in sync by hand:

```jsonc
{
  "nodes": [
    { "type": "separator", "name": "Core", "collapsed": false, "children": [
        { "type": "mod", "id": "skyui", "enabled": true },
        { "type": "separator", "name": "ENB", "collapsed": true, "children": [
            { "type": "mod", "id": "enb-preset", "enabled": true }
        ]}
    ]},
    { "type": "mod", "id": "awesome-mod", "enabled": true }
  ]
}
```

**Correction to how I described this earlier:** the flattened top-to-bottom sequence of mod nodes *is* the file-conflict-resolution priority order — same as any MO2-style manager, where list position decides which mod's file wins when two mods ship the same path. That's functionally load-bearing, not cosmetic. What's genuinely cosmetic is the separator/nesting structure itself — grouping and visual containers have no install effect. `phase` and `rules` remain a separate axis from both: they govern install/download *sequencing*, not final file priority. Practically this changes nothing about the design above — `placement: diverged` already protects a user's manual reorder correctly regardless of whether that reorder is decorative or priority-determining — it just means the stakes of getting the diff-and-preserve logic right are higher than "the list looks different," so worth being accurate about it.

## Schema versioning — strict

`gmmpackSchema` is full semver. GMM ships one JSON Schema document per **major** version and validates the entire archive — manifest, every `mods/*.json`, every `patches/*.json`, every `ini/*.json`, `tree.json` — against it before any resolution, download, or disk write happens. A major version GMM doesn't have a schema for is a hard refusal with a clear "unsupported modpack version" message, full stop, no best-effort parsing. Minor/patch are for additive, backward-compatible fields only; unknown fields within a supported major are rejected too, not silently ignored — that's what "strict" buys you: a pack that validates is guaranteed to mean exactly what the schema says, never "probably fine, extra fields ignored."

This is a separate concern from unknown-*provider* handling: schema validity is checked once, structurally, before touching any external source. Provider resolvability is checked per-mod at runtime, and unresolvable ones become warnings, not aborts — the archive was well-formed, one mod's source just couldn't be reached right now.

## Unknown providers and other runtime issues → collected diagnostics

No fail-fast on a `source.provider` GMM doesn't recognize, a stale hash mismatch, a missing Proton build, etc. Every such issue gets appended to a diagnostics list as install proceeds and surfaces inline on the relevant step of the install widget (see below), plus a running count badge the user can expand for the full list. The install still completes for everything that *did* resolve.

## Load order — LOOT, weakly hinted

1. Run LOOT against the installed plugin set using its masterlist plus any bundled masterlist rules.
2. Where LOOT's constraints leave real ambiguity (no dependency relationship either direction), break the tie using `loadOrder.pluginHint`'s relative ordering.
3. `pluginHint` can never override an actual LOOT dependency constraint — it only fills gaps LOOT leaves open. This matches how LOOT-vs-collection-metadata already resolves in existing third-party Nexus tooling, so it's a well-trodden default, not a novel risk.
4. Final resolved order is what gets written; it's re-derived (not cached-and-reused) on every install and every incremental update, since the plugin set can change between revisions.

## Platform-specific behavior

`platform.<os>` blocks at the manifest level, with an optional per-mod override inside `mods/<id>.json` for a mod with OS-specific quirks the whole pack doesn't share. `steamOverlay` maps straight onto the existing `inject_steam_overlay` field on `LaunchParams` rather than introducing a second mechanism. `protonVersionPin` is advisory: GMM tries to honor it if that build is available locally, otherwise falls back and logs a diagnostic — same "warn, don't abort" treatment as an unresolvable mod source, since a missing Proton build is an environment fact, not a structural pack defect.

All path/filename comparisons anywhere in the format — patch `targetPath`, ini `targetFile`, hash verification paths — go through the same NFC-normalized, case-folded comparison GMM already uses for the OverlayFS case-insensitivity layer. One normalization function, reused here rather than reinvented, so a pack authored on Windows resolves identically on ext4.

## Archive integrity

`manifest.archive.fileHashes` lists every other file in the archive by relative path with its sha256. This is checked first, before any JSON in those files is even parsed — a corrupted or tampered archive fails immediately with "archive integrity check failed" instead of surfacing as a confusing downstream parse error or, worse, a partially-applied install.

## Incremental updates

Identity is `id` (UUID, forever) + `revision` (int, strictly increasing). This lives alongside an **installed-pack record** GMM maintains per instance — not part of the `.gmmpack` file itself:

```
{ packId, installedRevision, resolvedMods: { id: { actualFileId, actualVersion, actualHash, userOverridden } },
  treeSnapshot, appliedPatches: [...], appliedIniEdits: [...] }
```

### Per-mod state tracking

User intent always wins over pack-authored placement. Every pack-originated mod in `resolvedMods` carries two independent state axes — independent because "where a mod sits" and "what version it's running" are different questions with different owners:

```jsonc
"resolvedMods": {
  "skyui": {
    "origin": "pack",                    // pack | manual
    "presence": "installed",             // installed | removed
    "placement": "conforming",           // conforming | diverged — tree.json position only
    "lastAppliedRevision": 4,
    "actualFileId": 67890, "actualVersion": "1.4.2", "actualHash": "..."
  }
}
```

- **`origin: manual`** — a mod the user added that was never part of any pack revision. Fully outside pack-diff logic, forever. Already established, unaffected by anything below.
- **`presence`** — did the user remove a pack-originated mod? `removed` means exactly that: it stops existing in this instance, stops receiving version updates (there's nothing to update), and is skipped entirely by future revisions unless the user takes an explicit "restore from pack" action. This is a stronger statement than divergence — it's an exit from the pack's mod set, not a repositioning within it.
- **`placement`** — scoped *only* to `tree.json` position/grouping. Set to `diverged` the moment the user manually reorders, regroups, or moves a still-installed pack mod. A diverged mod is skipped by the tree-diff step of every future update — its position is permanently user-owned from that point on — but everything else about it keeps flowing normally from the pack: version/source updates, patch changes, ini edits, and full participation in `rules`/`phase` install-sequencing (those are a completely separate axis from display position, per the three-axes split above). `diverged` doesn't self-heal back to `conforming`; only an explicit user "reset to pack layout" action would do that, and that's a nice-to-have, not required for v1.

`placement`/`presence` are per-mod, not per-instance — one mod being diverged doesn't affect how any other mod in the same pack is diffed.

### Update algorithm

When a `.gmmpack` is opened whose `id` matches an existing instance:

- `revision` ≤ installed → refused by default with a clear message; reinstall-from-scratch is available only as an explicit user action, never automatic.
- `revision` > installed → update path:
  1. Full schema + integrity validation, same as a fresh install.
  2. Diff `mods/` by stable `id`, gated by `origin`/`presence`/`placement` from the state table above: new `id` not seen before → fresh-install it, `presence: installed, placement: conforming`; `id` present in old but not new revision → if `presence` is still `installed`, uninstall it as the pack intends; if the user already set `presence: removed` themselves, it's already gone, nothing to do; `id` in both with `origin: manual` → untouched, always; `id` in both with `placement`/`presence` unmodified by the user and source/version/hash/patches changed → re-resolve and reinstall just that mod; unchanged → skip, zero network calls. Version/source updates apply to a mod regardless of its `placement` value — divergence never blocks an update, only a position change.
  3. Diff `tree.json` against the snapshot, but only for mods still `placement: conforming` — those follow the new authored layout exactly. Every `diverged` mod is left exactly where the user put it; new mods from the pack are inserted per the new `tree.json`, and if a `diverged` mod's former separator no longer exists, its now-orphaned position is left alone rather than force-migrated. No merge heuristics, no "insert sensibly" guessing — conforming mods follow the pack, diverged mods don't move, full stop.
  4. Re-run LOOT over the new plugin set regardless of what else changed.
  5. Diff `ini/*.json` per `(targetFile, tweakId)`: new tweakId → apply; tweakId gone → retract by tweak id; content changed within the same tweakId → re-merge + apply (the existing user-modified detection already flags hand-edited keys instead of overwriting them); sourceModId change alone → retract old source, apply new. Retract tweaks whose owning mod has `presence: removed`. An author flipping the `enabled` default between revisions never touches user state: `enabled` in a fresh install seeds from the author default, thereafter the instance record owns it.
  6. Diff `patches/`: a changed or removed patch triggers re-resolution of that mod's installed files (unless `presence: removed`); if new patches appeared where none existed before, re-trigger the consent dialog scoped to just the newly-patched mods.
- The result of steps 2–6 is a plan, not an immediate action — it's handed to the same install widget as a fresh install would be, just with the step list computed from a diff instead of from scratch. Diagnostics list every mod currently running `diverged` or `removed` state for that run, purely informational — this is disclosure, not a permission gate; the whole point is that user placement decisions execute without being second-guessed.

## Executables

Mods that are themselves tools — Nemesis, Pandora, FNIS, BodySlide, Synthesis/zEdit-style patchers, xEdit smash patches — install like any other mod (they still get a `mods/<id>.json` entry with a normal `source`), but they also need to be *registered and, often, run*, which nothing above covers. New directory, same id-keyed pattern as everything else:

```
executables/<id>.json
```

```jsonc
{
  "id": "nemesis",
  "sourceModId": "nemesis",              // the mods/nemesis.json entry this exe ships inside
  "relativePath": "Nemesis Engine/Nemesis.exe",
  "arguments": ["-forceD3D9"],
  "envVars": { "SOME_VAR": "value" },     // cross-platform baseline, applies on every OS
  "workingDir": "Nemesis Engine",
  "role": "setup",                        // setup | launcher
  "autoRun": true,                        // run automatically, no user click required
  "rerunOnModsetChange": true,             // re-fire on incremental updates that touch its inputs, not just at first install
  "requiresVirtualFsVisible": true,        // needs the full merged/deployed mod view before it can run correctly
  "output": {
    "path": "Nemesis_Engine/Output",       // where the tool actually writes, relative to its working dir
    "argName": "--output",                  // if the tool accepts an output-dir flag, GMM injects the *resolved* absolute path here instead of assuming a fixed default — this is the actual "overwriting the output location" knob: it's computed per-instance, not hardcoded into the pack
    "capture": "syntheticMod",              // syntheticMod | inPlace
    "syntheticModId": "nemesis-output"      // only when capture=syntheticMod
  },
  "platform": { "linux": { "envVars": { "WINEDEBUG": "+file" } } }   // additive on top of the base envVars above
}
```

**`envVars`** is cross-platform and always applied; `platform.<os>.envVars` adds to (never replaces) that base set for a specific OS — so a tool needing one Linux-only variable doesn't require duplicating the whole env block per platform.

**`role: setup`** covers one-shot generators that read the deployed mod set and produce output — Nemesis/FNIS/Pandora behavior generation, Synthesis-style smash patches, BSA/BA2 repacking. `autoRun: true` + `rerunOnModsetChange: true` is the click-and-run default for these: they fire once after install, and again after any incremental update that changes the animation/plugin mods they depend on — otherwise a pack update can silently leave stale generated output behind, which is a real correctness bug, not just a convenience gap. `role: launcher` is different: it doesn't run during install at all, it designates which exe the pack's "Play" button should launch (e.g. `skse64_loader.exe` instead of the vanilla game exe) — this is what makes "press play" actually correct out of the box instead of requiring the user to manually add an exe entry. `autoRun: false` (the default for anything not explicitly configured) just registers the exe in GMM's normal exe list for the user to run manually — right for something like default BodySlide, where the author hasn't baked a batch-build preset and running it non-interactively would silently skip real user choices.

**`output`** is the piece that makes "overwriting the output location" an actual instance-time decision instead of a pack-time assumption. `path` tells GMM where the tool is going to write relative to its own working directory — necessary just to *find* the output afterward. `argName`, when the tool supports it, lets GMM pass the real resolved absolute path (wherever this specific instance's mod storage actually lives, whatever the user's OverlayFS/VFS root happens to be) instead of trusting the tool's own default, which is exactly the thing that breaks portability across instances and, under Proton, across the prefix boundary. `capture: syntheticMod` turns the result into a proper tracked mod — `origin: "generated"` in `resolvedMods`, back-referencing `producedBy: "nemesis"`, with its own `tree.json` position. Its content is fully replaced (not merged) on every `autoRun` firing, since it's derived output, not user-authored — worth being explicit that generated mods don't get the same "never overwrite user edits" protection tree-diverged mods get; if a user hand-touches generated output between runs, that's on them, GMM isn't tracking it as intentional the way it tracks a manual reorder. `capture: inPlace` skips all of that and just leaves the output wherever it lands, untracked — an escape hatch for tools whose output genuinely shouldn't be managed as a mod (rare, but exists). Resolving `path`/`argName` correctly when the tool runs inside a Wine prefix and writes through to the Linux-side merged view is the same path-mapping problem as the rest of the OverlayFS work, not a new one — this is where that work does double duty rather than needing its own solution.

Two more things this needs, both reused rather than reinvented:

- Sequencing between multiple executables (or between an executable and the mods it depends on) reuses `rules[]` rather than inventing a second ordering mechanism — executable ids and mod ids share the same id-space for `before`/`after`/`requires` purposes.
- Launch mechanics — env vars, overlay injection, Proton handling — route through the same `LaunchParams`/three-launcher-tier path everything else uses, including `platform.linux` overrides here. Worth naming directly: this is the same class of problem as the Pandora/Wine CI-shim inheritance bug from the OverlayFS work — an executable-mod not inheriting the `LD_PRELOAD` shim its parent process has. Whatever fix that bug gets should be the thing `executables/*.json`-launched processes go through by construction, not a second code path that can independently regress.

## Choice groups — a genuine gap against Collections

Collections has no structural way to express "pick exactly one of these three mods" (competing texture packs, alternate combat overhauls) — it's handled loosely via `optional` + author instructions, which is exactly the kind of prose-dependent step this format is trying to get away from. Worth adding as a top-level manifest array:

```jsonc
"choiceGroups": [
  { "id": "texture-pack", "name": "Choose a texture overhaul", "mode": "exactly-one",
    "memberModIds": ["static-mesh-improvement", "skyrim-202x"] }
]
```

`mode: exactly-one | at-most-one`. Surfaces as a single radio-button step in the install widget's left-pane step list — not a detour from the linear flow, just one more step with a choice instead of a progress bar. On incremental update, the user's prior choice is remembered per `choiceGroups[].id` and re-applied automatically unless the group's membership actually changed between revisions, in which case it's presented again.

## Install widget

Two panes. Left: ordered step list with live status per step (pending/running/done) — resolving sources, the patch-consent gate when applicable, downloading, installing, applying ini edits, building the tree, running `role: setup` executables, running LOOT, any `choiceGroups` prompts inline as their own step, done. Diagnostics (unresolvable provider, missing Proton build, stale hash, etc.) attach inline to whichever step produced them, plus a persistent expandable count badge for the full list. Right: `instructions.md` rendered statically, available the whole time, nothing else competing for that space.

End state: pack installed, plugin order resolved, tree built, ready to launch. The only unavoidable manual steps are ones genuinely outside GMM's control — a browser-only source's download click-through, and the one patch-consent dialog if the pack has any. Everything else is zero-interaction by design.
