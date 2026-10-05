# Non-Bethesda Engine Support: Ground Work

Status: research only. No production code, no refactoring. This document maps
what would have to change so that Frostbite, Unreal, Unity and Resident Evil
titles become supportable, and names the places in the current tree where a
container layer must be allowed to hook in.

## 1. The load-bearing fact

GameModManager's entire content model is **loose files under a data
directory**. A mod is a folder. A conflict is two mods providing the same
path. Install is copy. Uninstall is delete. Deploy is symlink or copy of
individual files.

The target engines do not work that way. Their assets ship **packed into
container archives**, and the engine's own loader reads the container, not
the filesystem. Three consequences follow, and every later section is a
consequence of them:

1. A mod can be **inside** a container. The path a conflict is computed on is
   not a path on disk.
2. "Install" means **repack**, not copy. The game must be pointed at the
   repacked archive.
3. The game's own file list is not the mod list, and a path inside container A
   and a path inside container B are the same logical resource.

## 2. What the containers actually are

### 2.1 Unreal Engine 4 and 5

`.pak` is UE's own container with a footer index that can be AES encrypted, so
reading one generally needs the title's AES key. Formats other than 7z
(zlib, LZ4, Oodle) are used for entries.

UE5 and late UE4 add **IoStore**: the asset metadata moves to a `.utoc`
container and the asset data to a `.ucas`, while `.pak` shrinks to carry only
non-asset files. This is not a cosmetic split. The decisive facts, from an
Epic developer forum answer on loading external pak files (2024-11-20,
forums.unrealengine.com/t/external-pak-files-and-use-io-store-option/2137290):

- "IOStore containers now longer expose individual files, so the .uasset
  files do not exist any more when packaging to IOStore. Instead the new
  containers work on **package paths**."
- "the AssetRegistry cannot scan a .utoc/.ucas for contents, since there is no
  folder structure/files to scan anymore. Instead the information about which
  assets/packages are contained in an IOStore Container are written to a
  separate **AssetRegistry.bin** file."
- Mounting at a custom mount point with IoStore is not supported, because the
  package store manifest pre-computes package dependency information containing
  package IDs that are a hash of the package name.

Practical consequence for a mod manager: with IoStore enabled there is **no
file tree to enumerate**. Enumeration has to come from `AssetRegistry.bin`, and
identity is a package path, not a filename. A conflict engine built on
"same relative path" cannot see a conflict at all.

### 2.2 Unity

Unity's archive format is documented, and it is a real archive, not a
proprietary blob. Unity's own manual (docs.unity3d.com, AssetBundle file
format reference) states the archive format "is a generic packaging format
that can store any type of file, similar to a .zip file" and that "Archive
files are mounted into Unity's virtual file system (VFS), which allows them to
be accessed in a uniform way across different platforms."

Structure, per AssetStudio's `BundleFile.cs`
(github.com/Perfare/AssetStudio/blob/master/AssetStudio/BundleFile.cs), which
is the clearest public reference:

- Signature is one of `UnityFS`, `UnityWeb`, `UnityRaw`, `UnityArchive`.
  `UnityFS` is the modern one (Unity 5.3+).
- Header carries version, unity version string, compressed and uncompressed
  block-info sizes, and flags. Version >= 7 aligns the stream to 16 bytes.
- The block-info section holds a 16-byte data hash, a block count with
  `{uncompressedSize, compressedSize, flags}` per block, and a node count with
  `{offset, size, flags, path}` per file. `BlocksInfoAtTheEnd` moves the
  block-info block to the end of the file.
- Per-block compression is None, LZMA, LZ4 or LZ4HC.
- Paths inside are virtual (`CAB-<md4 of name>` for the serialized file, plus
  `.resource` and `.resS` companions).

Unity is the **most tractable of the four** because (a) the format is
documented and has working open-source readers, and (b) Unity already has a
documented mount point (its own VFS), so the "point the game at the modded
archive" problem is solved by the engine.

### 2.3 Frostbite

Frostbite stores assets in a three-layer archive system, from the Frostbite
documentation of the format and the `frostbite2-stuff` and `anthemtool`
reverses:

```
Game Root/
  Data/
    layout.toc                  # master table of contents
    Win32/<catalog>/
      cas.cat                   # catalog index over the cas files
      cas_01.cas .. cas_NN.cas   # the actual data
      <bundle>.sb / <bundle>.toc # superbundle metadata
  Patch/                        # same structure, overlaid
  initfs_win32                  # bootstrap filesystem
```

- `layout.toc` is the entry point: it lists superbundles, the install
  manifest, catalog definitions, and the base version.
- CAS entries are content-addressed. Each is `FACE0FF0`, a 20-byte SHA1, and a
  data length (frostbite2-stuff). The **SHA1 of the content is the identity**,
  which is what the engine looks up.
- A CAS identifier is a packed 32-bit value carrying Layout ID (0 data, 1
  patch), installation chunk id, and CAS file index (anthemtool).
- Data is split into 0x10000-byte chunks, each with an 8-byte header giving
  compressed size, uncompressed size, and a compression type. Oodle is one of
  the compression types observed.
- `.sb` superbundles map bundles to parts: **Ebx** (metadata, the partitions
  the mod community calls partitions), **Res** (mesh/texture headers,
  animations, shaders), and **Chunks** (the actual mesh, texture, audio data).

How Frostbite modding works in practice, from the FrostyToolsuite
documentation (Frosty Mod Manager): a mod is **not** a folder of loose files.
A mod is a `.fbmod`, and applying it **writes a second set of CAS archives**.
The result lands in `ModData/<ModPackName>/`, contains `cas_*.cas`, `*.toc`,
`*.sb` and a `mods.json` of applied mods, and is symlinked into the game
directory. The game is then launched with `-dataPath <mod data path>` or the
`GAME_DATA_DIR` environment variable. The engine overlays the second data
path over the first. Load order is top to bottom, last mod wins.

Note on the brief: the `ebacRoot` / `Mods.txt` convention named in the task
could not be confirmed in the sources consulted. What is confirmed is the
`ModData` + `-dataPath` / `GAME_DATA_DIR` + `mods.json` convention above, and
`ebac` appears in Frostbite tooling as the EBX asset container root. Treat
`Mods.txt` as unverified.

### 2.4 Resident Evil (RE Engine)

RE Engine's `.pak` is its own format with magic **`KPKA`**, not a Rockstar RPF.
Structure per the XentaxWiki RE Engine PAK page
(wiki.xentax.spektr.name/index.php/RE_Engine_PAK):

```
"KPKA" | version (u32) | num_files (u32) | nulls (u32)
per file: offset (u32) | nulls | size (u32) | nulls | hash1 (u32) | hash2 (u32)
data blocks
```

Version 4 differs somewhat and can use zlib. Read and write implementations
exist (Ekey/REE.PAK.Tool ships REE.Unpacker and REE.Packer; eigeen/ree-pak-rs
is a Rust port with mmap and legacy backends). RE Engine titles: RE2, RE3, RE4,
RE7, RE8, Devil May Cry 5, Monster Hunter Rise, Resistance.

Two hard problems:

- **File names are not in the pak.** Both existing tools require a separate
  `.list` file shipped per project, listing every path. Entry identification is
  by two u32 hashes. Enumerating a RE Engine pak from the pak alone is not
  possible today; you need a title-specific list.
- RPG and MT Framework (older Resident Evil) titles use `.mrs` / `.pkm`, which
  are different again. Supporting "Resident Evil" as one target is wrong;
  RE Engine is one target and the older engines are separate targets with
  separate formats.

For contrast, **RPF7** is Rockstar's RAGE format (magic `0x52504637`), the
one behind GTA V mods. It is a different engine entirely and is listed here
only so it is not confused with RE Engine. Its header packs name-table length
and a name-offset shift into one dword, and file offsets are stored in 512-byte
units in a 23-bit field, which caps any single archive at 4 GB.

## 3. What GMM already has, and what is actually reusable

### 3.1 Reusable, and already aimed at this

The good news is that the abstraction a container layer needs **already
exists and is already used**.

`src/engine/mod/filetree/data_archive.h` defines the read seam. Its own header
comment says: "A DataArchive is a source of named, typed entries that a
FileTree can be built from. libarchive provides the implementation today
(zip/7z/tar/rar/gz/bz2/xz); **BSA/BA2 backends plug in here later.**" The
interface is one method, `list()`, returning `ArchiveEntryInfo { path, is_dir,
size, index }`. A `.pak`, `.ucas`/`.utoc`, `.cas` or `.bundle` backend is a new
implementation of that one method. No change to `FileTree` is required.

`src/engine/mod/filetree/file_tree.h` is MO2's `IFileTree`, already ported.
It has the two things a container needs: lazy per-node population
(`do_populate()`, a protected virtual, populated under `std::call_once`) and a
directory entry that **is** a tree (`as_tree()`). Two factories exist,
`make_tree_from_directory()` and `make_tree_from_archive()`; a
`make_tree_from_container()` is a third.

The conflict engine is **already written against `FileTree`, not against
`std::filesystem`**. `src/engine/index/conflict_engine.cpp:108` builds a
`DirectoryFileTree` and walks it, so a container-backed tree is a
substitution at the construction site rather than a rewrite of the engine.

The UI file tree is likewise `FileTree`-based (`src/ui/modinfo/filetree_tab.cpp`,
`src/ui/panels/data_tab.cpp`).

Also reusable: `ArchiveExtractor` and the vendored 7-Zip / libarchive
(`src/engine/mod/archive/`) give the process-level services a container
backend needs, namely a total-size pre-pass for a progress bar
(`ExtractProgressFn`), a bounded passphrase prompt for encrypted archives
(`PassphraseFn`, which is exactly what a UE pak AES key wants), cancellation,
and thread-priority lowering.

### 3.2 Fundamentally incompatible

**7-Zip 26.03 does not help.** The official 7-Zip supported-formats list
(7-zip.org) covers 7z, xz, bzip2, gzip, tar, zip, wim, rar, cab, arj, iso, msi,
deb, rpm, squashfs, vhdx and friends. Unreal `.pak`, Frostbite `.cas`, Unity
`.bundle` and RE Engine `.pak` are all absent, and they are absent for a
structural reason: none of them has a ZIP central directory. A generic
archiver cannot read them, and the vendored libarchive path is dead for this
work. This is not a "add a codec" task.

**FOMOD assumes a flat staging tree.** `FomodFileInstaller`
(`src/engine/mod/fomod/file_installer.h`) "applies the same
selection/priority/copy semantics directly to the extracted staging directory"
and "copies them into a fresh tree, and swaps it in place of the staging dir."
A FOMOD installer describing a mod is describing **files on disk**. For a
container mod, the files are entries at offsets inside an archive. FOMOD can
still be used as a **metadata** layer (it decides which entries are in scope)
but its execution model has to be replaced.

**The mod cache token is a directory token.** `ConflictEngine::compute_quick_token`
(`src/engine/index/conflict_engine.cpp:53`) is `last_write_time(mod_dir)` plus
a **count of top-level directory entries**. A mod folder containing one
`mymod.pak` yields a token with count 1. The token still changes when the pak
is rewritten, so it is not useless, but the **file list** it guards is one
entry long, and the file is named after the container, not after what is
inside it. Two mods that ship `mod_a.pak` and `mod_b.pak` will be scored as
**one conflict between two pak files** and the winner will be decided by mod
order, with no knowledge of what either contains.

**Incremental update is keyed to a whole-mod hash.** `modpack::incremental_update`
diffs a new pack revision against an installed-pack record using sha256 plus a
(removed) bsdiff layer, keyed on `<mod>[#seq]:<hash8>` patches and
`<lower target>::<tweak>:<hash8>` INI tweaks. Those keys are path- and
mod-shaped, not container-shaped. For a container mod, the minimal correct
unit of change is "this entry inside this archive", and a mod that adds one
entry invalidates the whole archive's compression. The existing sha256 layer
can be reused to decide *whether* a container changed, but not to describe
*how*.

## 4. Where the deploy path breaks

The three live strategies are `Symlink`, `OverlayFsDeploy` and `Direct`
(`src/engine/deploy/core.cpp:16-54` dispatches only on those strings, with
`Symlink` as the fallback for anything unrecognised). All three operate on
loose files, and the breakage is more fundamental than "they need a new
branch".

**The deploy interface is per-file.** `src/engine/deploy/interface.h` is
eleven lines and two methods:

```cpp
virtual bool deploy(const std::filesystem::path &source,
                    const std::filesystem::path &target) = 0;
virtual bool remove(const std::filesystem::path &target) = 0;
```

One file in, one file out, `std::filesystem::path` on both sides. **There is no
verb for "repack".** A container cannot be the `target` of this interface
because a container is not a file the game reads; it is a file the *game's
loader* reads. Specifically:

- **`Symlink`** creates a symlink in the game dir pointing at a real file in
  the mod folder. If the mod is a pak, this symlinks the pak to one
  predictable name. That is actually the **one case that works**, provided the
  game accepts a pak placed in its content directory and mounts every pak it
  finds. This is worth testing first because it is nearly free.
- **`OverlayFsDeploy`** unions a lowerdir (the real game) with an upperdir (the
  mod tree) on Linux. The upperdir must contain loose files. An upperdir
  containing a `.pak` is fine as a *file*; the union cannot unpack it. So
  OverlayFS helps only in the same limited way Symlink does, and the whole
  point of overlay (per-file precedence across many dirs) is unused.
- **`Direct`** walks all enabled mods, resolves winners per file, diffs against
  a ledger, and links or removes in parallel. Its ledger records individual
  deployed paths, so undeploy is exact. With a container, the ledger would have
  to record archive names, and one archive write replaces N path records. The
  ledger format changes.

**Overwrite resolution breaks in a specific way.** GMM resolves a winner per
path: `deploy_utils.h`'s `resolve_deploy_target_ci` folds CI-equal filenames
and there is a documented winner map so `0_Master.hxk` and `0_master.hxk`
collapse to one staged file won by the last mod in folder order. For a
container mod the resource identity is not a path. In Frostbite it is a SHA1
of content. In UE5 IoStore it is a package path plus a dependency id baked at
cook time. Neither can be recovered by folding a filename, so **CI folding and
per-path winner maps have no meaning** and the conflict engine would need a
second, container-aware notion of identity alongside
`vfs::PathResolver::normalize`.

**Whether a mod can even be a file breaks.** The mod list assumes
`instance/mods/<name>/` is a directory: `ConflictEngine::compute_quick_token`
does a `directory_iterator` on it, `Symlink` walks it, the ledger keys on
paths beneath it. A mod whose unit of installation is one archive is a
different kind of object. MO2 does not have this problem because every mod
MO2 knows about is a directory or a single Bethesda archive, and Bethesda
archives are enumerated into a virtual tree and **never rewritten** (see
section 7).

## 5. Seams the layout cleanup must not cut

The tree is mid-way through a layout cleanup. These are the specific places a
container layer has to be able to hook in, and the specific moves that would
make it harder.

### 5.1 Hook points, by path

| Path | Why it must stay open |
|---|---|
| `src/engine/mod/filetree/data_archive.h` | The `DataArchive` read seam. Already documented as the place non-libarchive backends go. **Do not fold this into `mod/archive/`** - the two do different jobs (listing vs extracting) and keeping them apart is what makes a new backend a one-method class. |
| `src/engine/mod/filetree/file_tree.h` | `do_populate()` is the lazy-population contract a container tree implements, and `as_tree()` is what makes a directory entry navigable. Keep the factory pattern (`make_tree_from_*`) so a third factory has an obvious home. |
| `src/engine/index/conflict_engine.cpp:108` | The one construction site where a `DirectoryFileTree` becomes the tree the engine walks. A container-aware build picks a different tree here. Keep it a single site; do not scatter directory iteration back through the engine. |
| `src/engine/deploy/interface.h` | The per-file deploy contract. This **has** to grow a container verb eventually. Keeping it as a small standalone header (not folded into `deploy_utils.h`) is what makes adding a second interface cheap. |
| `src/engine/deploy/core.cpp:16-54` | The strategy-name dispatch. Note it currently falls back to `Symlink` for **every** unrecognised string. A new strategy must be added *before* that fallback, and the silent fallback should become a hard failure for a game that declares a container format - otherwise a typo ships a mod that deploys wrongly rather than not at all. |
| `src/engine/mod/cache/`, `ConflictEngine::compute_quick_token` | The cache token. A container mod needs a token derived from the archive's own identity and mtime, not from a directory entry count. |
| `src/engine/mod/fomod/file_installer.h` | The FOMOD execution model. Needs to be separable from the FOMOD choice model (section 3.2). |
| `src/engine/mod/overwrite/overwrite_utils.h` | The overwrite-to-mod path bridge, and therefore the natural home for the "which container wins" rule. See the warning below. |
| `src/engine/game/detect/mod_scanner.cpp` | Where a game declares that its assets are containers. Today every game is a loose-file data directory. |

### 5.2 Cleanup moves that would make a container layer harder

**Phase 2: `engine/pack/` -> `engine/modpack/detect/` (layout-proposal 6.4).
This is the one to refuse.** `engine/pack/` currently holds `adapter.h` and
`source_detector.*` after the dead-code trim, so the directory looks cheap to
absorb. But "pack" is also the natural, honest name for a container backend
(`pack/pak.cpp`, `pack/cas.cpp`, `pack/bundle.cpp`). Folding it into
`modpack/detect/` buries a game-content-container concept inside a
**parked** gmmpack-installer concept and forces the name to mean two things.
Give the container layer its own directory, `engine/container/`, and leave
`engine/pack/` for whatever survives the parked-tree decision.

**Phase 2: `mod/overwrite/overwrite_utils` -> into `modpack/` (6.4).** Same
objection, smaller. The overwrite bridge is a general content-resolution
primitive, not part of the gmmpack format. Merging it into `modpack/` couples
"which file wins" to the pack installer.

**Phase 1: `core/vfs/` -> `vfs/` (1.4) is safe but name with care.** `vfs/`
holds `PathResolver::normalize`, which is currently the single source of file
identity for conflict detection. A container layer needs a second, different
identity notion (content SHA1 for Frostbite, package path for UE5 IoStore).
Do not put a container normaliser into `vfs::PathResolver`; they answer
different questions and merging them makes both wrong.

**Phase 4 renames (`game/plugins/` -> `game/esp/`, `pipeline/plugin_host/` ->
`plugin_host/`, `update/` split, `platform/` deletions) are safe.** Nothing in
those moves touches `mod/filetree/`, `mod/archive/`, `index/`, `deploy/` or
`mod/overwrite/`. The proposal itself notes `deploy/` (86 include lines) is
not touched in Phase 4. That is the right call and should stay true.

**Phase 6 file splits are safe except `mod_list_controller.cpp`,** which is
irrelevant to containers. `packer.cpp` in 6.d is the gmmpack packer, not a
container packer; do not let the name collision mislead a future reader.

## 6. Phased approach

Sized, smallest first. Phase 1 is the one that proves the concept and it is
small.

**Phase 1 - one read-only container backend, one game, prove the tree.**
Implement a `DataArchive` for Unreal `.pak` (UE4, the simpler pre-IoStore
layout). Add `FileTree::make_tree_from_pak()`. Add a capability flag to the
game registry saying "this game's content includes pak archives". Stop there.
No deploy change, no FOMOD change, no UI change. The deliverable is: point
GMM at a game directory containing paks and **see the pak contents in the
existing mod-info file tree**. That is the whole phase. It tests the one
assumption everything else depends on, namely that a container can be presented
as a `FileTree`, and it costs one new `.cpp` plus one factory.

**Phase 2 - make the mod list container-aware.** Teach
`compute_quick_token` and the conflict engine to treat an archive entry as a
first-class path, and give the mod scanner a way to declare "this mod folder
contains an archive". At this point conflict detection works read-only over
containers. Still no writing.

**Phase 3 - the cheap deploy win.** Test whether a UE4 title mounts every
`.pak` it finds in its content directory, ignoring the original. If yes, a
container deploy strategy is nearly free: symlink or copy the archive to a
deterministic name and record it in the ledger. This is the Frosty `ModData`
plus symlink shape, minus the repack. If the answer is no, Phase 3 becomes
Phase 5 and the phase is deferred, not abandoned.

**Phase 4 - repack.** Add a write side, the counterpart to `DataArchive::list`,
and the real `Deploy` verb. This is the expensive phase and it is per-format.
`mapToArchive`-style write-back, as MO2 has for its archives, is the shape.

**Phase 5 - the games with no mount point.** Frostbite and UE5 IoStore need
the game told about a second data root (`-dataPath`, `GAME_DATA_DIR`) and
cannot use a symlink. That is a launch-argument concern, not a deploy concern,
and it is a separate piece of work per engine.

**Phase 6 - Unity, which is the cheapest of the four** once Phase 1 has
proved the machinery, because UnityFS is fully documented and Unity's VFS is
a documented mount point. Arguably Unity could be Phase 1 instead of UE4 pak;
if a reader wants the fastest proof of concept, swap it.

**Deliberately not scoped here:** asset-level editing. Reading and repacking a
`.pak` is a container problem. Changing a value inside an `.uasset`, an `.ebx`
or a `.rez` is a **format** problem, orders of magnitude larger, and needs a
different kind of expertise. Do not let container support imply asset
support.

## 7. Does the reference implementation do any of this?

Mod Organizer is the reference point, and the answer is no, and that is the
most important finding in this document.

The vendored MO2 tree's own list of game plugins is **entirely Bethesda**:
game_ceral, game_fallout3, game_fallout4, game_fallout4vr, game_falloutnv,
game_features, game_gamebryo, game_morrowind, game_oblivion, game_skyrim,
game_skyrimSE, game_skyrimVR, game_starfield, game_ttw. There is no
Frostbite, Unreal, Unity or Resident Evil plugin.

A search of the MO2 and usvfs sources for `pak`, `IoStore`, `ucas` and `utoc`
returns nothing but unrelated `autocheck_update_install` settings strings. MO2
knows exactly one container format: **BSA/BA2**, Bethesda's own, and it
**only reads it** (`src/shared/directoryentry.cpp` lists a bsa and logs
"invalid bsa" on failure; `src/mainwindow.cpp` only uses bsas to associate
files with plugins). It never writes one.

What MO2 *does* have is the abstraction, and it is the right one. MO2 defines
`IFileTree` and provides three implementations: `QDirFileTree` (real
directory), `VirtualFileTree` (the VFS, read-only, lazily populated), and
`ArchiveFileTree`. `ArchiveFileTree` is built on **libarchive**
(`#include <archive/archive.h>`), so it handles zip, 7z, tar and friends, and
it carries a `mapToArchive(Archive&)` method that writes tree changes back into
the archive. GMM has already ported all of that: `FileTree`, `DirFileTree`,
`ArchiveFileTree` and the `DataArchive` seam are in the tree and are the same
design.

The distinction that matters: **MO2 treats an archive as transport for a mod
(handles zip/7z/tar) and treats the game's own BSA as content to enumerate
read-only. It has no notion of repacking a game asset container, because no
game it supports requires one.** Read for behaviour, never copy source: MO2 is
GPL-3 and this codebase is MIT.

So the honest summary of the reference: MO2 solved the *presentation* problem
generically years ago, and GMM has already inherited that solution. What no
reference has solved is the *repack* problem, because until recently no
supported game needed it.

## 8. What breaks today, and what a user would see

If a Frostbite, Unreal, Unity or RE Engine title were added to GMM as it
stands, with no code changed:

- **The mod list would show the archive, not the mod.** A mod folder holding
  `mymod.pak` appears as one file. Mod-info's file tree shows one row. There
  is no way to see the 400 files inside.
- **Two mods would be reported as conflicting for no visible reason.** Both
  appear to "provide" their pak. The conflict dialog names two files the user
  has never heard of. The user's actual conflict, two mods shipping the same
  texture, is invisible.
- **Deploy would do something plausible and wrong.** `Symlink` links the pak
  into the game dir under its own name. If the game mounts it, it works by
  luck. If it does not, the user gets an unmodified game and a green deploy
  log. `Direct` copies the pak and the ledger records one path. `OverlayFS`
  unions a tree containing one file.
- **Uninstall would be correct by accident.** Delete or unlink the one
  recorded path, and the mod is gone. This is the one operation that happens
  to work.
- **FOMOD installers would install nothing.** The installer XML references
  paths like `meshes/foo.dds` that do not exist on disk, so the selection
  resolves to zero files. The wizard would complete with a warning and deploy
  nothing.
- **Overwrite would be meaningless.** The game's files are not on disk, so
  there is no overwrite folder to capture and no vanilla copy to diff against.
- **The cache would not notice content changes.** The token is the mod
  folder's mtime plus a top-level entry count, so a repacked archive with the
  same filename at the same size looks unchanged.
- **No crash, no error, no red anything.** Every one of these is a silent
  wrong result. That is the real risk, and it is the argument for doing the
  read-only Phase 1 before any deploy work: Phase 1 has a visible,
  checkable result, and the failure modes above do not.

## 9. Sources

- Unreal IoStore / external pak mounting:
  `https://forums.unrealengine.com/t/external-pak-files-and-use-io-store-option/2137290`
- Unreal UGC best practices, pak and IoStore overview:
  `https://docs.mod.io/unreal/ugc-best-practices`
- Unity archive format, official:
  `https://docs.unity3d.com/6000.7/Documentation/Manual/assetbundles-file-format.html`
- UnityFS structure and compression, source:
  `https://github.com/Perfare/AssetStudio/blob/master/AssetStudio/BundleFile.cs`
- Frostbite CAS/CAT and superbundles:
  `https://github.com/mitsuhiko/frostbite2-stuff`
- Frostbite bundle layout, chunks, Oodle:
  `https://github.com/xyrin88/anthemtool`
- Frostbite data model, partitions:
  `https://docs.veniceunleashed.net/vext/guides/data/`
- Frostbite archive system and Frosty mod application:
  `https://github.com/FrostyToolsuite/FrostyToolsuite`
- Frostbite mod application detail (ModData, `-dataPath`, `mods.json`):
  `https://mintlify.wiki/CadeEvs/FrostyToolsuite/modding/mod-installation`
- RE Engine PAK, KPKA layout:
  `https://wiki.xentax.spektr.name/index.php/RE_Engine_PAK`
- RE Engine PAK tools, read and write:
  `https://github.com/Ekey/REE.PAK.Tool`, `https://github.com/eigeen/ree-pak-rs`
- RPF7 for comparison (RAGE, not RE Engine):
  `https://github.com/yamp-project/rpflib`
- 7-Zip supported formats, official:
  `https://www.7-zip.org/`
- Mod Organizer, local reference tree:
  `references/modorganizer/src/`, `references/modorganizer/readme.md`
