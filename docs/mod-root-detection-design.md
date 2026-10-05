# Mod root detection and layout correction

Design note. No code in this document has been written yet.

---

## 1. What MO2 actually does

MO2 does **not** show a "please locate the Data folder" dialog on the happy
path. It has three mechanisms and they fire in this order:

```
archive arrives
      |
      v
[1] AUTOMATIC. InstallerQuick walks down single-directory wrappers until the
    current level "looks valid" per the GAME plugin's declaration.
    Found  -> install silently, wrapper stripped. User sees nothing.
    Not found (multi-entry level that does not look valid) -> [2]
      |
      v
[2] AUTOMATIC. The GAME plugin may declare the archive FIXABLE and supply
    fix(), which rewrites the tree. Install silently. User sees nothing.
      |
      v
[3] MANUAL. Only if no installer claimed the archive, or the user pressed
    "Manual": a content tree with a <data> pseudo-root, live
    valid/invalid verdict, right-click "Set as <data> directory",
    and a "Continue?" confirm whose default button is CANCEL.
```

So the user's recollection is accurate about the *dialog* but the dialog is the
**last resort**, not the first thing you see. The primary MO2 behaviour is a
silent, plugin-driven, single-wrapper peel. This design keeps that order.

### 1.1 The declaration: `ModDataChecker` (a game feature)

MO2's per-game extension point. In
`ModOrganizer2/libmodstar` / `modorganizer-uibase`,
`include/uibase/game_features/moddatachecker.h`:

```cpp
enum class CheckReturn { INVALID, FIXABLE, VALID };

virtual CheckReturn dataLooksValid(std::shared_ptr<const IFileTree> tree) const = 0;

virtual std::shared_ptr<IFileTree> fix(std::shared_ptr<IFileTree> tree) const
{ return nullptr; }   // only called when dataLooksValid returned FIXABLE
```

Header comments worth quoting because they set the contract:

- "This method does not have to be exact, it only has to indicate if the given
  tree looks like a valid mod or not by quickly checking the structure (heavy
  operations should be avoided)."
- "FIXABLE should only be returned when it is guaranteed that `fix()` can fix
  the tree."

Resolved per game through `IGameFeatures` (combined across every plugin that
registered one for that game).

### 1.2 The Bethesda declaration, as written

`references/modorganizer-game_bethesda/src/gamebryo/gamebryomoddatachecker.cpp`:

- `:8-45`  `possibleFolderNames()` - 34 directory names (`fonts`, `interface`,
  `menus`, `meshes`, `music`, `scripts`, `shaders`, `sound`, `strings`,
  `textures`, `trees`, `video`, `facegen`, `materials`, `skse`, `obse`, `mwse`,
  `nvse`, `fose`, `f4se`, `distantlod`, `asi`, `SkyProc Patchers`, `Tools`,
  `MCM`, `icons`, `bookart`, `distantland`, `mits`, `splash`, `dllplugins`,
  `CalienteTools`, `NetScriptFramework`, `shadersfx`)
- `:50-54` `possibleFileExtensions()` - `esp`, `esm`, `esl`, `bsa`, `ba2`,
  `modgroups`, `ini`
- `:59-76` `dataLooksValid()` - walk only the **top level**; a directory whose
  name is in the folder set, or a file whose suffix is in the extension set,
  returns `VALID` immediately. Otherwise `INVALID`.

Both lists are `virtual` (`src/gamebryo/gamebryomoddatachecker.h:38,43`), so a
game overrides them by subclassing, not by branching.

### 1.3 The reference FIXABLE case (the rewrite hook)

`references/modorganizer-game_bethesda/src/games/nehrim/nehrimmoddatachecker.cpp`:

- `:3-20`  `dataLooksValid()` - try the Gamebryo set; if that fails and **every**
  top-level entry is a file whose name starts with `OBSE`, return `FIXABLE`.
- `:22-29` `fix()` - build a fresh tree containing `OBSE/Plugins/` and merge the
  original tree into it. Returns the fixed tree.

This is the only mechanism by which a game plugin reshapes an archive's
contents rather than just judging them. It is the thing we have no equivalent
of.

### 1.4 The automatic flatten: `getSimpleArchiveBase`

`references/modorganizer-installer_quick/src/installerquick.cpp:93-112`:

```cpp
while (true) {
  if (checker->dataLooksValid(dataTree) == VALID ||
      isDataTextArchiveTopLayer(dataTree, dataFolderName, checker)) {
    return dataTree;                                    // this is the root
  } else if (dataTree->size() == 1 && dataTree->at(0)->isDir()) {
    dataTree = dataTree->at(0)->astree();                // peel one wrapper
  } else {
    return nullptr;                                      // give up
  }
}
```

Three properties that matter:

1. It descends **only** through a level holding exactly one entry, and that
   entry must be a directory. Two siblings at any level stop the walk.
2. It stops at the **first** level that looks valid. A nested valid subtree is
   found; a nested invalid one is not searched further.
3. It never searches sideways or by name. `resources` buried two levels down
   beside a readme is not found.

`isDataTextArchiveTopLayer` (`:66-91`): exactly one directory named like the
game's data dir, plus one or more of `txt/pdf/md/jpg/jpeg/png/bmp`, and nothing
else. Applied at `:152-157` by detaching the data dir and merging the root into
it. This is MO2's "Data plus README" handling.

### 1.5 Acceptance and the FIXABLE branch

- `installerquick.cpp:114-125` `isArchiveSupported()` - claims the archive when
  `getSimpleArchiveBase` found a base **OR** the checker said `FIXABLE`.
- `installerquick.cpp:127-172` `install()`:
  - `base == nullptr && FIXABLE` -> `tree = checker->fix(tree)`
  - otherwise `tree = base`
  - then `SimpleInstallDialog` (`:143`) - a name box plus a `Manual` button and
    nothing else (`references/modorganizer-installer_quick/src/simpleinstalldialog.cpp:29-43`,
    `simpleinstalldialog.ui:14` window title "Quick Install")
  - `Manual` -> `RESULT_MANUALREQUESTED` (`:160-162`)
  - a `silent` plugin setting (`:49-54`, `:144`) skips the dialog entirely

### 1.6 Installer selection and the give-up path

`references/modorganizer/src/installationmanager.cpp`:

- `:759-764` installers sorted by `priority()` descending
- `:786-791` each is offered the archive via `isArchiveSupported(filesTree)`
- `:776-779` once a non-manual installer returns `RESULT_MANUALREQUESTED`, the
  loop continues but **only** installers with `isManualInstaller() == true` are
  tried from then on
- `:797-807` after a successful simple install the (possibly reshaped) tree is
  detached and mapped back onto the archive
- `:873-877` if nothing claimed the archive:

  > "None of the available installer plugins were able to handle that archive.
  >  This is likely due to a corrupted or incompatible download or unrecognized
  >  archive format."

  That is the whole answer. A flat error, no browse, no layout dialog. MO2 does
  not help the user with a genuinely ambiguous archive.

### 1.7 The layout dialog (MO2's `InstallerManual`)

**Provenance note.** This plugin lives in `ModOrganizer2/modorganizer-installer_manual`
and is **not** in our `references/` tree - I read it from the upstream public
repository. Line numbers below are from that repo's own
`src/installer_manual_en.ts` catalogue, which records the line of every
`tr()` call. It is GPL-3; nothing is copied, only the behaviour is described.

`src/installermanual.cpp`:

- `:52` name "Manual Installer"
- `:62` description "Fallback installer for mods that can be extracted but can't
  be handled by another installer"
- `:70-73` `priority()` returns 0 - lowest, so it runs only when everything else
  declined
- `:75-78` `isManualInstaller()` returns true
- `:80-83` `isArchiveSupported()` returns true unconditionally
- `:103-121` `install()` constructs

  ```cpp
  InstallDialog dialog(tree, modName,
                       gameFeatures()->gameFeature<ModDataChecker>(),
                       managedGame()->dataDirectory().dirName().toLower(),
                       parentWidget());
  ```

  then installs `dialog.getModifiedTree()`. The checker and the data-dir name
  come from the **game plugin and the active game**, never from a hardcode.

`src/installdialog.cpp` - the dialog:

- `:55-62` the widget is a `treeContent` whose top item is synthesised as
  `"<" + dataFolderName + ">"` (`src/archivetree.cpp`, `ArchiveTreeWidget::setup`).
  `installdialog.ui:75` help text: "<data> represents the base directory which
  will map to the game's data directory. You can change the base directory via
  the right-click context menu and you can move around files via drag&drop"
- `:167` right-click a directory -> **"Set as <%1> directory"**
- `:174` when a sub-root is set -> **"Unset <%1> directory"**
- `:185` "Create directory..." (prompts for a name, `:138`)
- `:189` "&Open" (extract one file and hand it to the OS handler)
- `:86-93` `testForProblem()`:

  ```cpp
  return m_Checker->dataLooksValid(m_Tree->root()->entry()->astree()) ==
         ModDataChecker::CheckReturn::VALID;
  ```

  Note `FIXABLE` counts as a problem here. The verdict is recomputed against the
  subtree the user has designated as the data root, so the label is live.
- `:95-122` the verdict renders as a label **and a tree border**:
  - green / "The content of <%1> looks valid." (`:109`, tooltip `:111`)
  - red / "The content of <%1> does not look valid." (`:117`, tooltip `:119`)
  - darkYellow / "Cannot check the content of <%1>." (`:100`) when the game
    registered no checker at all
- `:196-210` on OK with a problem:

  ```cpp
  QMessageBox::question(this, tr("Continue?"),
      tr("This mod was probably NOT set up correctly, most likely it will NOT "
         "work. You should first correct the directory layout using the "
         "content-tree."),
      QMessageBox::Ignore | QMessageBox::Cancel,
      QMessageBox::Cancel);        // <-- Cancel is the DEFAULT button
  ```

  Backing out is the default action. Choosing `Ignore` installs as-is.
- Cancel -> `reject()` -> `RESULT_CANCELED`, nothing installed

`src/archivetree.cpp` `ArchiveTreeWidget::setDataRoot` is the flatten primitive:
the previous data root's children go back under it, the newly chosen item is
force-populated, and its children become the visible root's children. Drag and
drop moves entries with type-conflict guards ("Cannot drop '%1' into one of its
subfolder." `:436`).

### 1.8 The same trick, second instance

`references/modorganizer-installer_fomod/src/installerfomod.cpp:99-111`
`findFomodDirectory` recurses through single-directory wrappers looking for
`fomod/`. Same descend-one-wrapper idiom, different marker. Confirms this is
MO2's standard shape for "where is the real root in this archive", not a
Bethesda special case.

---

## 2. Our current state

### 2.1 What already exists

**The declaration, twice over, and neither is used at install time.**

| Declaration | Isaac | Skyrim SE | Consumer |
| --- | --- | --- | --- |
| CSV hooks `mod_valid_dirs` | `resources,resources-dlc3` | 35 names | `mod_scanner.cpp:264-265` |
| CSV hook `mod_valid_exts` | *absent* | `esp,esm,esl,bsa,ba2,modgroups,ini` | `mod_scanner.cpp:265` |
| ABI game feature `mod_data_checker` | *not registered* | *not registered* | `mod_scanner.cpp:304` |

- Isaac declares at
  `projects/Plugins/src/plugins/GameSupport/TheBindingOfIsaacRebirth/TheBindingOfIsaacRebirth.cpp:335-336`
  (`conflict_scan_dirs`, `mod_valid_dirs`, both `resources,resources-dlc3`).
  It also declares `metadata_file=metadata.xml` (`:300`) and
  `deploy_include_mod_id=true` (`:325`).
- Skyrim SE declares at
  `.../SkyrimSpecialEdition/SkyrimSpecialEdition.cpp:85-91`.
- Resolution order, registry first then CSV fallback, is
  `src/engine/game/detect/mod_scanner.cpp:304-311`. The registered-feature seam
  exists and works (`ModDataCheckerFeature`,
  `src/engine/game/registry/game_features/game_feature.h:56-79`;
  combining resolver `game_feature_registry.cpp:64-84`; C++ ABI
  `gmm_abi_v1.h:375-382`, `plugin_loader.cpp:415-431`; Python ABI
  `python_loader.cpp:1009`). **No shipped game plugin registers one.** Only
  tests do (`tests/engine/plugins/override_mod_data_checker.cpp`).

**The checker.** Two of them, both boolean, neither per-game at install time:

- `engine::ModDataChecker::data_looks_valid` -
  `src/engine/game/registry/game_features/mod_data_checker.cpp:57-71`, with the
  static Gamebryo folder set at `:9-48` and extension set at `:50-55`. This is
  the port of `GamebryoModDataChecker::dataLooksValid`.
- `ModDataCheckerFeature::data_looks_valid` -
  `src/engine/game/registry/game_features/game_feature.cpp:7`. Instance-based,
  per-game, registered. Correct shape, nobody registers it.

**The scanner already computes `invalid_data`, and the app already uses it.**

`src/engine/game/detect/mod_scanner.cpp:605`:

```cpp
mod.invalid_data = !mod.validated && !content_looks_valid(cfg, entry_path);
```

with `content_looks_valid` at `:210-244` (top-level recognised dir, a
`.esp/.esm/.esl/.bsa/.ba2` at top level, or the game's metadata file present).

Everything the flag touches is **post-install, mod-list only**:

| Where | What |
| --- | --- |
| `mod_list_model.cpp:140` | warning icon in the row |
| `mod_list_model.cpp:308-311` | italic dark-grey mod name |
| `mod_list_model.cpp:402-406` | name tooltip: "contains no esp/esm/esl and no asset (textures, meshes, interface, ...) directory" |
| `mod_list_model.cpp:460-462` | "No valid game data" line in the Flags tooltip |
| `mod_list_model.cpp:2257` | feeds an aggregate `invalid` roll-up |
| `mod_context_menu.cpp:337-345` | "Ignore missing data" writes `[General] validated=true` into the folder's `meta.ini` |
| `mod_list_controller.cpp:1536-1540`, `:1815-1818` | pushes the flag into the model |

So: **yes, `invalid_data` is already computed and already shipped to the user.
It is a list decoration and a per-mod mute. Nothing in the install path reads
it.**

One consequence worth knowing: `content_looks_valid` returns true the moment the
game's metadata file exists (`mod_scanner.cpp:215-217`), and
`InstallStage` always writes one (`install_stage.cpp:267`). **A mod we installed
can therefore never be flagged `invalid_data`, whatever its layout.** The flag
only ever fires on a folder a human made by hand.

**Flattening already exists. Twice.**

- MO2-style, generic, on a tree: `src/engine/mod/filetree/staging_layout.cpp`.
  `analyze_staging_layout` (`:79-86`) is a faithful port of
  `getSimpleArchiveBase`; `analyze_rec` (`:47-75`) is the same loop, and
  `is_data_text_top_layer` (`:16-39`) ports `isDataTextArchiveTopLayer`.
  `normalize_staging_root` (`:88-143`) applies the verdict on disk: peel the
  recorded wrapper chain (`:108-119`), merge a DataText data dir (`:121-141`).
  Called once, from `extract_stage.cpp:201`.
- Isaac-style, hardcoded, on paths: `src/engine/pipeline/extract_stage.cpp:144-150`

  ```cpp
  auto has_resources = [](const std::filesystem::path &dir) {
    return std::filesystem::exists(dir / "resources") ||
           std::filesystem::exists(dir / "resources-dlc3") ||
           std::filesystem::exists(dir / "resources-dlc4") ||
           std::filesystem::exists(dir / "resources-dlc5") ||
           std::filesystem::exists(dir / "mod.asm");
  };
  ```

  then `:164-193` finds the one common top-level component across all extracted
  files and flattens it with a `std::filesystem::rename` loop (`:184-192`).
  Gated on `ctx.deploy_include_mod_id` (`:156`), fed from the
  `deploy_include_mod_id` hook (`TheBindingOfIsaacRebirth.cpp:325`).

  This is the game-specific branch the work order wants gone. It never consults
  `mod_valid_dirs`, it hardcodes `resources-dlc4` and `resources-dlc5` which no
  plugin declares, and it invents its own "one common component" rule instead of
  using the shared peel loop.

### 2.2 What does not exist

1. **No tri-state.** Both checkers return `bool`. There is no `FIXABLE`, and no
   `fix()`. A game cannot say "this is wrong but I know how to repair it".
2. **The install path cannot ask what the game considers valid.**
   `PipelineContext` carries `deploy_prefix` (`pipeline.h:70`),
   `deploy_include_mod_id` (`:75`) and `metadata_file` (`:80`). It carries **no
   checker and no allow-lists**. So `normalize_staging_root` is driven by the
   static Bethesda set even for a non-Bethesda game, and the Isaac branch
   sidesteps the whole thing with a hardcode.
3. **No warning at install time, at all.** `ExtractStage` peels or does not and
   reports nothing. `InstallStage` copies and writes metadata and reports
   nothing. `invalid_data` is not consulted. An archive with a nonsense layout
   installs silently and lands in the mods dir looking installed.
4. **No layout dialog.** No tree widget over a `FileTree` anywhere in the UI.
   `src/ui/modinfo/filetree_tab.h:16-39` is a read-only `QTreeView` +
   `QFileSystemModel` over a folder already on disk - the right visual language
   but the wrong model (it edits nothing, and it edits the real filesystem).
5. **No "give up" reporting.** Nothing ever tells the user "I could not find a
   valid layout here".

### 2.3 Metadata placement - already correct

The user asked that `metadata.xml` / `meta.ini` live at the top level of the
mod directory. That already holds:

- `InstallStage` writes the game's metadata file into `dest_dir`, the mod folder
  root (`install_stage.cpp:264-268`).
- Both flatten branches run **before** `InstallStage`, on the staging dir, and
  both move a wrapper's children up to the staging root
  (`extract_stage.cpp:184-192`, `staging_layout.cpp:108-119`). A
  `metadata.xml` that shipped inside the wrapper therefore arrives at the mod
  folder root.

No change needed. Do not add code here.

---

## 3. The plugin declaration shape

What a game plugin must be able to say, for this to work generically. This is
MO2's `ModDataChecker` with the parts we already have removed:

```cpp
// Already exists, per game, combined across plugins:
//   - valid top-level directory names
//   - valid top-level file extensions
// Declared EITHER by the CSV hooks mod_valid_dirs / mod_valid_exts
// (already shipped by Isaac and Skyrim SE)
// OR by register_game_feature("mod_data_checker", priority,
//                             folder_names[], file_extensions[])
// (ABI exists, no plugin uses it yet).

// NEW - the only genuinely new declaration:
//   verdict: INVALID | FIXABLE | VALID
//     FIXABLE only when repair is guaranteed, per MO2's contract.
//   repair:  an optional tree rewrite, called only on FIXABLE.
//             MO2's reference case is NehrimModDataChecker::fix, which
//             wraps the root in OBSE/Plugins/.
```

Three decisions this forces, and why:

**One allow-list source, not two.** The CSV hooks and the registered feature
already coexist with a defined precedence (`mod_scanner.cpp:304-311`). The
install path must use that same resolver, so the scanner and the installer can
never disagree about whether a mod is valid. That is a correctness requirement,
not tidiness - today they already can disagree, because the installer uses a
third source (the static Gamebryo set).

**`FIXABLE` needs code, so it needs a function pointer.** The C ABI is
append-only by convention - every prior addition is annotated "appended last to
stay binary-compatible" (`gmm_abi_v1.h:363-364`, `:384-385`, `:412-419`). One
more appended slot, shaped like `subscribe_event` (`gmm_abi_v1.h:366-369`), is
the ABI-compatible way in:

```c
/* Mod layout checker - appended last to stay binary-compatible.
   Lets a game own the VALID/FIXABLE/INVALID verdict and the repair, i.e.
   MO2's ModDataChecker including fix(). NULL game_id = this plugin's own
   game. Registered checker wins over the mod_valid_dirs /
   mod_valid_exts CSV hooks and over the engine default, in that order. */
int (*register_mod_layout)(GmmRegistrationCtx* ctx,
                           const char* game_id,
                           int priority,
                           const char* const* folder_names, size_t folder_count,
                           const char* const* file_extensions, size_t extension_count,
                           GmmModLayoutFn fn,   /* verdict + repair */
                           void* user_data);
```

**No new hook names.** `mod_valid_dirs` already is the answer to "what is a
valid folder for this game". Adding `mod_valid_root_dirs` or
`mod_layout_hints` would be a second dialect for the same fact.

---

## 4. The design

### 4.1 Detection (engine, Qt-free)

One function, one seam, called from exactly one place.

```
resolve_layout_checker(game_id)          -> ModLayoutChecker
    precedence: registered mod_layout  >  mod_data_checker feature
              >  mod_valid_dirs / mod_valid_exts CSV
              >  engine default (static Gamebryo set)

ModLayoutChecker::verdict(tree) -> { INVALID, FIXABLE, VALID }
ModLayoutChecker::fix(tree)     -> tree | null     (FIXABLE only)
```

`extract_stage.cpp` becomes the single decision point, and the two branches
collapse into one:

```
extract to staging_dir
      |
      v
analyze_staging_layout(staging, deploy_prefix, checker)
      |  fomod?               -> leave alone, FomodStage owns it
      |  verdict == VALID     -> done, no peel
      |  verdict == FIXABLE   -> tree = checker.fix(tree), re-analyze once
      |  single-dir wrapper   -> record, peel, retry
      |  anything else        -> NEEDS_REVIEW  (this is the new signal)
      v
if NEEDS_REVIEW and the install is interactive -> ask the user (4.2)
if NEEDS_REVIEW and headless / silent          -> install as-is, stamp validated
```

What is deleted:

- `extract_stage.cpp:144-150` `has_resources` (the hardcode)
- `extract_stage.cpp:156-207` the `deploy_include_mod_id` fork. That flag still
  has a real job downstream - it decides the deploy target path
  (`pipeline.h:72-75`) - but it must no longer decide the archive's root.
- `extract_stage.cpp:164-193` the hand-rolled common-prefix flatten, which the
  shared peel loop already covers for the Isaac case: `resources` and
  `resources-dlc3` are in Isaac's `mod_valid_dirs`, so
  `analyze_staging_layout` peels `funnymod/` and stops there on its own.

What changes:

- `analyze_staging_layout` and `normalize_staging_root` take a checker instead
  of calling the static `ModDataChecker` (`staging_layout.cpp:54`).
- `StagingNormalizeResult` gains `needs_review` (and `fixable`, if the caller
  wants to say why).
- `PipelineContext` gains `valid_dirs` / `valid_exts` (or a resolved checker
  handle), filled from the same `GameKnowledge` + registry the scanner uses, in
  `settings_controller.cpp` next to `deploy_prefix` (`:414-416`).

Behaviour change to be deliberate about: `analyze_rec` today falls through
silently when the tree is neither valid, DataText, nor a single wrapper
(`staging_layout.cpp:74-75`). MO2 returns `nullptr` there. Turning that
fall-through into `needs_review` is the entire behavioural delta, and it is the
point of the exercise.

### 4.2 The user's navigate-and-pick flow

MO2's `InstallDialog`, our engine callbacks, our widgets.

```
+--------------------------------------------------------------+
| Install Mod                                             [X]  |
+--------------------------------------------------------------+
| Name:  [ TheFunnyMod                        v ]              |
|                                                              |
| Content                                                     |
| +----------------------------------------------------------+ |
| | v <mods>                                                 | |
| |   v funnymod            <- right-click: "Set as <mods>"  | |
| |       v resources                                        | |
| |           gizmo.anm2                                     | |
| |           *.png                                          | |
| +----------------------------------------------------------+ |
| The content of <mods> looks valid.                          |
+--------------------------------------------------------------+
|                          [ OK ]  [ Cancel ]                 |
+--------------------------------------------------------------+
```

- A tree over the **extracted staging directory**, not over an in-memory
  archive. Our `FileTree` and `extract_stage` already work on disk
  (`staging_layout.cpp:96-98`), so no archive-tree abstraction is needed. This
  is a deliberate deviation from MO2, which edits the tree in memory and maps it
  back onto the archive (`installationmanager.cpp:797-807`). Editing on disk is
  less machinery and the archive is not precious.
- The pseudo-root is `"<" + ctx.deploy_prefix + ">"`. MO2 uses the game's data
  dir name (`installermanual.cpp:107`); ours is `deploy_prefix`, already on the
  context (`pipeline.h:70`).
- **Right-click a directory -> "Set as <prefix> directory"**, plus "Unset" once
  set. Selecting it moves that subtree's contents up to the staging root and
  re-evaluates. Same semantics as `ArchiveTreeWidget::setDataRoot`, expressed as
  the same on-disk peel the automatic path already performs - one code path,
  driven by a user instead of by the verdict.
- **Live verdict label + coloured border**, recomputed on every change, driven
  by `checker->verdict(current_root_subtree)`. Green / red / amber, MO2's three
  states (`installdialog.cpp:95-122`). Amber covers "this game registered no
  checker", which is a real state for a plugin that never declared one.
- **OK on a red verdict -> confirm, Cancel is the default.** MO2's exact strings
  and button order (`installdialog.cpp:198-207`).
- Cancel -> `ctx.canceled = true`, nothing installed. `PipelineResult::Canceled`
  already means "the user aborted an interactive stage, do not mark it failed"
  (`pipeline.h:49-56`).

Wiring, following the four existing callbacks exactly
(`settings_controller.cpp:335-400` - `hide_install_progress()`, then marshal to
the main thread):

```cpp
// pipeline.h, next to the existing four callbacks
std::function<LayoutDecision(const std::filesystem::path &content_root,
                             const std::string &suggested_name,
                             const std::string &data_prefix)>
    layout_query_cb;
```

Headless / CLI: unset callback means auto-accept and install as-is. That matches
today's `name_query_cb` convention (`pipeline.h:94-102`) and means a
headless install never blocks on a question nobody can answer.

Widgets to reuse rather than invent:

- the tree pattern from `src/ui/modinfo/filetree_tab.h` (custom context menu,
  tri-state checkboxes not needed here, right-click menu shape is the same)
- the marshalling and `hide_install_progress()` pattern from
  `settings_controller.cpp:335-341`
- the "No valid game data" strings already shipped in `mod_list_model.cpp:402`
  and `:460`, so the dialog and the list speak the same language

There is **no** filesystem directory picker in this flow. MO2 does not use one -
you are choosing a node inside the archive, not a folder on disk. The existing
`QFileDialog::getExistingDirectory` callers (`settings_content_widget.cpp`,
`main_window.cpp:439`) are unrelated and should not be touched.

### 4.3 Flattening

One implementation, already written, used by both the automatic and the
user-driven path.

- Automatic: `analyze_staging_layout` decides, `normalize_staging_root` applies.
- User-driven: the dialog's "Set as <prefix> directory" produces the same
  verdict - "these are the children of the new root" - and the same
  `normalize_staging_root` call applies it.

`data_folder_name` keeps its current role: it is the *name hint* guard, so a
wrapper literally called `Data` is not taken as the mod's name
(`extract_stage.cpp:201-206`). It is not the validity test. Validity comes from
the checker.

Two sharp edges in the existing mutator, both to be fixed while it is being
touched, both silent today:

- `staging_layout.cpp:114` and `:133`: `std::filesystem::rename` into an
  occupied destination. The error goes into `ec` and the loop just `break`s, so
  a wrapper containing two same-named entries loses one silently.
- `extract_stage.cpp:187-191` has the identical pattern in the branch being
  deleted, so deleting the branch deletes one copy of the bug.

### 4.4 Metadata

No change. See 2.3. `InstallStage` already writes to the mod folder root after
the peel, and both branches peel before install.

One thing to verify during implementation, not to code now: `ExtractStage`
reads `ctx.metadata_file` off the staging root *before* normalizing
(`extract_stage.cpp:132-141`), so an archive whose `metadata.xml` sits inside
the wrapper is currently read from the wrong place and the name falls back to
the archive stem. Whether to re-read after normalizing is an implementation
detail, not a design decision.

---

## 5. Explicitly out of scope

1. **Drag and drop inside the dialog.** MO2 has it
   (`archivetree.cpp`, `dropEvent`). It is the single most expensive part of
   that widget and the user did not ask for it. "Set as <prefix> directory" plus
   checkboxes covers the reported problem. Revisit if users move files.
2. **"Create directory..." in the dialog.** Useful for repairing a broken
   archive, but it is a mod-authoring tool, not an install tool. Separate.
3. **MO2's `isDataTextArchiveTopLayer`.** Already ported
   (`staging_layout.cpp:16-39`). Not extended, not reimplemented.
4. **The FOMOD installer.** `findFomodDirectory`'s descend
   (`installerfomod.cpp:99-111`) is the same idiom and could be unified with the
   peel loop, but `FomodStage` already owns that archive and it works. Leave it.
5. **Repairing an already-installed mod folder.** `invalid_data` already tells
   the user, and "Ignore missing data" already mutes it. A re-root tool for
   existing mods is a different feature.
6. **BSA / loose-file inventory-driven detection.** MO2's checker is a
   name-based allow-list on purpose ("does not have to be exact"). We match it.
7. **Any behaviour change for archives that are already fine.** Everything here
   is a no-op when `verdict == VALID` at the root.

---

## 6. Risks

**A genuinely ambiguous archive.** Two or more candidate subtrees both look
valid, e.g. `resources/` and `resources-dlc3/` are present but the archive is
really `<dlc>/resources/...` plus a loose `resources/`. MO2's peel loop stops at
the first valid level and never compares candidates; the manual dialog defaults
to the archive root, which is *not* valid, so the user starts from a red label
and must choose. That is the honest behaviour and we should copy it rather than
add a confidence score nobody can calibrate. Our version is slightly better off
than MO2's because the flat case is pre-solved by the peel loop, so the dialog
only opens when the tree genuinely has no valid single-wrapper root.

**Two sibling wrappers, no valid level.** `ModA/resources` and `ModB/resources`
at the archive root. The peel loop refuses (two entries), the dialog opens at
the root, verdict red, and the user picks one subtree - which silently drops the
other half of the mod. MO2 has exactly this failure. Mitigation: when the dialog
opens on a red root, list the subtrees that *would* be valid, so the user can
see what the other option costs. That is a small addition to the context menu
and worth doing; it is not in MO2.

**Data loss from the peel.** Peeling moves files up. If the wrapper and the root
share a name, `rename` overwrites. Silent today (6, section 4.3). Must fail
loudly before this ships.

**Ambiguity for the `metadata_file` hook.** `content_looks_valid` treats the
presence of the metadata file as valid content (`mod_scanner.cpp:215-217`) but
MO2's `dataLooksValid` does not (`installerquick.cpp` checks folders and
suffixes only; the *scanner* is what special-cases `meta.ini`). If we let the
checker treat `metadata.xml` as validity, every Isaac archive becomes valid at
its root and the peel never runs - which would break the Isaac case outright.
**The install-time checker must not count the metadata file.** Worth stating
because it is a live trap.

**Scope creep into a second dialect.** `deploy_include_mod_id` currently
selects a branch. Leaving it in place "just for the deploy path" while also
adding a checker is exactly how two sources of truth reappear. The install-time
decision must key off the checker only.

**Regression risk in the existing peel.** It is exercised by the install path
today for every Bethesda game. Replacing the static set with a per-game
resolver changes behaviour for any game whose CSV allow-list differs from the
hardcoded Gamebryo set. Skyrim SE's lists match it; other games may not. This
is the highest-risk line in the change and wants a test that runs the same
archive through two games with different allow-lists.

**Uncertainty, stated.** I could not verify the exact wording MO2 shows for a
non-Bethesda game, because `InstallerManual` is a Bethesda-package plugin and
`modorganizer-game_bethesda` does not ship an installer of its own in our
reference tree. The mechanism is game-agnostic in the code
(`gameFeature<ModDataChecker>()`, `managedGame()->dataDirectory().dirName()`,
both from the active game, not from Gamebryo), so the shape generalises, but the
exact strings for a game with no data dir are unverified.

---

## 7. Phasing

### P1 - Per-game validity at install time. No new ABI.

- `resolve_layout_checker(game_id)` in the engine, precedence
  registered-feature > CSV > engine default.
- `analyze_staging_layout` / `normalize_staging_root` take the checker.
- `StagingNormalizeResult::needs_review`.
- `PipelineContext` carries the allow-lists; `settings_controller.cpp` fills
  them beside `deploy_prefix`.
- Delete `extract_stage.cpp:144-150` and `:156-207`. Isaac now flows through
  the shared peel loop.
- Fix the two silent `rename` collisions in `staging_layout.cpp:114` / `:133`.

Ships: every Isaac and Bethesda archive that already worked keeps working, and
`funnymod.zip/resources` now flattens through the shared path instead of a
hardcode. No user-visible change yet. Smallest useful slice, and it is a
prerequisite for everything else.

### P2 - The "locate the Data folder" dialog. **This is the phase that delivers
the UI on its own.**

- `LayoutDecision` + `layout_query_cb` on `PipelineContext`.
- One new UI dialog: tree + pseudo-root + "Set as <prefix> directory" + live
  verdict + green/red/amber + "Continue?" with Cancel default.
- `ExtractStage` calls it when `needs_review` and the callback is set.
- Headless: callback unset, install as-is, no behaviour change.
- Fix the existing `ExtractStage` metadata read-before-normalize ordering
  (4.4).

Ships: a user installing a wrong-layout archive is told, shown the tree, and
given the choice, and picking a subdirectory installs correctly. Depends only
on P1. **No dependency on P3.**

### P3 - Tri-state and plugin-owned repair.

- `ModLayoutChecker::verdict` returns INVALID / FIXABLE / VALID.
- `register_mod_layout` appended to the C ABI (`gmm_abi_v1.h`), Python
  equivalent.
- `extract_stage` calls `fix()` on FIXABLE and re-analyses once.
- Port `NehrimModDataChecker`'s pattern as the reference shape. Isaac and
  Skyrim SE need nothing here; this exists for plugins that have a repair.

Ships: a game that can repair an archive without asking. Only reachable for
plugins that opt in.

### Ordering note

P1 and P2 are separable PRs. P3 touches the ABI and every game plugin repo, so
it is worth doing only once P2 has shown which repairs are actually wanted -
otherwise we are guessing at the shape of the repair hook.

---

## Open questions

1. Should a *malformed* (not merely unrecognised) archive reach the dialog, or
   keep the flat error MO2 shows at `installationmanager.cpp:873-877`? The
   proposal above sends everything the checker rejects to the dialog, which is
   friendlier and deviates from MO2 at exactly that point.
2. Does the checkbox-to-exclude-files part of MO2's tree belong in P2? Without
   it, "Set as <prefix> directory" is the only edit available.
3. Should `conflict_scan_dirs` (`TheBindingOfIsaacRebirth.cpp:335`) become the
   source for `mod_valid_dirs` instead of a separate CSV? They are currently
   two hooks carrying the same string. Consolidating is out of scope here but it
   is the kind of duplication that costs later.