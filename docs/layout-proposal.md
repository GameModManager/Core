# Core layout proposal

Research deliverable. Nothing in `src/` was moved, renamed or edited. No build
was run. No test was run.

Evidence for every claim is a `file:line` citation or a path. Where I could not
prove something, it is marked **UNCERTAIN** rather than asserted.

---

## 0. Measurements (verified, not assumed)

`src/` is **136,597 lines across 698 `.cpp`/`.h` files**.

| Slice | Files | Lines |
|---|---:|---:|
| `src/engine/` | 387 | 66,953 |
| `src/ui/` | 285 | 65,173 |
| `src/platform/` | 13 | 2,351 |
| `src/core/` | 2 | 921 |
| `src/runtime/` | 4 | 381 |
| `src/cli/` | 4 | 592 |
| `src/keyring/` | 2 | 217 |

The user's numbers check out: `src/ui/widgets/` is 81 files / 15,870 lines,
`src/ui/controllers/` is 20 files / 13,285 lines.

CMake surface that a move has to touch:

- `src/engine/CMakeLists.txt` lists **173** `.cpp` entries
- `src/ui/CMakeLists.txt` lists **138** `.cpp` entries
- `tests/CMakeLists.txt` lists **189** `.cpp` entries

**The single most important fact for migration cost:** the include root is
`src/` (`target_include_directories(gmm_engine PUBLIC ${PROJECT_SOURCE_DIR}/src`,
`src/engine/CMakeLists.txt:216-220`), and every include is fully rooted from
there. Of all includes across `src/` and `tests/`, the only non-rooted ones are
8 files that include their own directory (`usvfs_mapping.h`, `path_resolver.h`,
`game_file.h`) and the 3 sibling libraries (`core/core.h`, `cli/*.h`,
`platform/platform.h`, `runtime/*.h`, `keyring/*.h`, `ui/ui_locker.h`).

Consequence: **a move is `git mv` + N mechanical include-line rewrites + 1
CMake entry.** There is no include-path guessing and no path remapping layer to
get wrong. That makes this far safer than the ticket's `c3mb` history suggests,
provided each move is done alone.

---

## 1. The fragmentation

### 1.1 `engine/core/` is a bucket, not a concept

This is the origin of the whole problem and it is fully documented in git.
Commit `5467e0d` ("refactor(engine): consolidate engine into 7 top-level
directory groups", phase 2 of issue #33) took **28 previously flat directories**
in `src/engine/` and stacked them seven deep:

```
src/engine/{util,log,events,trace,keyring,instance}  ->  src/engine/core/...
src/engine/{meta,cache,model,overwrite,fomod,archive,filetree} -> src/engine/mod/...
src/engine/{download,nxm,workshop}                   ->  src/engine/source/...
src/engine/{detect,plugins,saves,registry/*}          ->  src/engine/game/...
src/engine/{plugin_host,registry/*}                   ->  src/engine/pipeline/...
src/engine/{launch}                                   ->  src/engine/deploy/...
src/engine/{tools,theme/theme_manager}                ->  src/engine/platform/...
```

The grouping criterion was *what kind of thing the file is* (a log, a util, an
event). The result is that `engine/core/` names nothing at all. It holds six
unrelated former top-level directories:

| Subdir | Files | Lines | What it actually is |
|---|---:|---:|---|
| `core/instance/` | 22 | 4,913 | mod-list persistence, instance lifecycle |
| `core/log/` | 4 | 727 | logging + crash handler |
| `core/util/` | 6 | 1,186 | fs helpers, thread priority, debug env |
| `core/vfs/` | 5 | 547 | game path -> real path resolution |
| `core/keyring/` | 2 | 547 | secret storage abstraction |
| `core/trace/` | 2 | 247 | trace recording |
| `core/events/` | 2 | 274 | the EventBus |
| *(flat)* `module.*`, `github.*`, `directory_refresher.*` | 6 | ~2,000 | module registry, GitHub API client, dir watcher |

**There is no name that covers "logging, keyring, and the mod-list database".**
That is the proof that `engine/core/` should not exist: its contents have no
common denominator.

Note `core/events/` (274 lines) and `core/trace/` (247 lines) already have
perfect, self-explanatory names. They are one level too deep for no reason.

### 1.2 Every source provider is split across two directories

This is the cleanest fragmentation in the tree and it is systematic. The
declarations live in per-provider subdirectories; the implementations live flat
at `engine/source/`:

| Implementation (flat) | Declaration (subdir) | Class |
|---|---|---|
| `source/nexus_provider.cpp` | `source/nexus/provider.h` | `engine::Source::Nexus::Provider` |
| `source/nexus_auth.cpp` | `source/nexus/auth.h` | `engine::Source::Nexus::Auth` |
| `source/nexus_http.cpp` | `source/nexus/http.h` | `engine::Source::Nexus::Http` |
| `source/nexus_account.cpp` | `source/nexus/account.h` | (validate / rate limits) |
| `source/nexus_servers.cpp` | `source/nexus/servers.h` | `engine::Source::Nexus::Servers` |
| `source/loverslab_provider.cpp` | `source/loverslab/provider.h` | `engine::Source::LoversLab::Provider` |
| `source/loverslab_auth.cpp` | `source/loverslab/auth.h` | `engine::Source::LoversLab::Auth` |
| `source/steam_workshop_provider.cpp` | `source/steam/provider.h` | `engine::Source::Steam::Provider` |

Verified by reading the `.cpp` bodies: `source/nexus_provider.cpp:37` defines
`Provider::parse_file_list` matching `source/nexus/provider.h:24`, and
`source/loverslab_provider.cpp:73` defines `Provider::is_loverslab_url`.

Steam Workshop is worse: it is split **three** ways -
`source/steam_workshop_provider.cpp` (impl), `source/steam/provider.h`
(decl), and `source/workshop/workshop_client.cpp` +
`source/workshop/remote_cache.cpp` (the HTTP client it calls).

`engine/source/` is 6,652 lines across 52 files with **no consistent rule** about
where a file sits.

### 1.3 One feature, four directory names: the modpack story

`gmmpack` is not a directory, it is a *format*. The format lives in
`engine/gmmpack/`. The format *detection* lives in `engine/pack/`. The format
*update* workflow lives in `engine/modpack/`. The format's *ini side effects*
live in `engine/mod/overwrite/`. And the UI lives in `ui/modpack/`.

Four engine namespaces confirm they are all talking about one thing:

| Namespace | Directory | What it holds |
|---|---|---|
| `engine::gmmpack` | `engine/gmmpack/` (22 files, 5,746 ln) | packer, unpacker, sha256, bsdiff, patch, tree parser, schema validator, uuid, ini edit parser, executable pipeline |
| `engine::Pack` | `engine/pack/` (4 files, 673 ln) | `Interface`, `SourceDetector` |
| `engine::modpack` | `engine/modpack/` (4 files, 1,571 ln) | `IncrementalUpdate`, `IniEdits` |
| `engine` (no sub-namespace) | `engine/mod/overwrite/` (2 files, 862 ln) | `OverwriteUtils` |
| `ui` | `ui/modpack/` (10 files, 4,505 ln) | import / install / export wizards |

A developer looking for "how do I write a gmmpack archive" has to read
`engine/gmmpack/packer.cpp`. A developer looking for "why did my archive fail to
import" has to read `engine/pack/source_detector.cpp` **and** `ui/modpack/
modpack_import_dialog.cpp` **and** `engine/gmmpack/schema_validator.cpp`. Three
directories, one user-visible operation.

### 1.4 Mod management is spread over six directories

The "what is a mod and what state is it in" concern lives in:

- `engine/core/instance/mod_state.*` - per-mod enable/priority/nesting, persisted to `mod_state.json`
- `engine/core/instance/installed_pack_state.*` - per-mod state for pack-installed mods, persisted to `installed_pack.json`
- `engine/mod/meta/mod_meta.*`, `engine/mod/meta/categories.*` - mod metadata
- `engine/mod/model/mod.h` - the `Mod` struct itself
- `engine/profile/*` - `modlist.txt` / `lockedorder.txt` read+write
- `engine/game/detect/mod_scanner.cpp` (1,010 lines) - discovering mods on disk
- `engine/index/*` - conflicts between them
- `engine/mod/cache/mod_cache.*`
- `ui/controllers/mod_list_controller.cpp` (4,592 lines) - all of the above, driven
- `ui/panels/*` - the tabs that display it

Ten directories. Two of the three most important state files
(`mod_state.json`, `installed_pack.json`) sit in a directory named after
*instance*, not after *mod*.

### 1.5 Four different meanings of "plugin"

| Location | What "plugin" means there |
|---|---|
| `engine/game/plugins/` | Skyrim `.esm`/`.esp` plugin files (TES4 header parsing, `esp_header.h:9`) |
| `engine/pipeline/plugin_host/` | the GMM plugin **system** (ABI host, python loader, 12 registries) |
| `ui/panels/plugins_tab.*`, `plugin_view.*` | GMM plugin system UI (the "Plugins" tab) |
| `ui/main_window/plugin_db_load_worker.*` | loading the **`.esp` plugin database** (`engine/game/plugins/plugin_database.cpp`, 1,197 lines) |

So `plugin_view.cpp` renders a list of GMM plugins, while
`plugin_db_load_worker.cpp` loads Skyrim `.esp` metadata. The word is overloaded
across an engine/ui boundary with nothing to disambiguate.

### 1.6 Two classes called "profile"

- `engine::profile::Profile` in `engine/profile/profile.h:12` - reads and writes
  `modlist.txt` / `lockedorder.txt`
- `engine::ProfileModel` in `engine/mod/model/profile.h:7` - a plain data
  struct holding a mod list, used by the UI, `deploy/order_hook.h`,
  `pipeline/plugin_host/abi_bridge.cpp`, and 2 tests

Different namespaces, different meaning, same word, and the second one is filed
under `mod/model/` inside the engine.

### 1.7 Which directories should not exist at all

Ranked by how much they cost:

| Directory | Files / Lines | Verdict |
|---|---:|---|
| **`engine/core/`** | 47 / 9,142 | **Dissolve.** Lift all 6 children to `engine/` top level. `core/` names nothing. |
| **`engine/install/`** | 10 / 916 | **Delete.** Zero production consumers (proved in section 4). |
| **`engine/pack/`** | 4 / 673 | **Absorb** into `engine/modpack/`. `Pack::Interface` and `SourceDetector` are modpack format detection. |
| **`engine/modpack/`** | 4 / 1,571 | **Absorb** into `engine/modpack/` (i.e. it becomes the destination), keeping only `incremental_update` + `ini_edits` under it. |
| **`engine/parallel/`** | 1 / 87 | **Keep.** Already correctly placed and correctly named. A 6-file header-only helper with a real name is not fragmentation. |
| **`engine/backup/`** | 2 / 752 | **Keep** but rename to `engine/restore/`? See section 2. **UNCERTAIN** - `backup_service.h` is included by 3 real files. Not dead, so not urgent. |

Note on `engine/pack/` vs `engine/gmmpack/`: the user listed
`engine/pack/adapter.*` as a dead-code candidate. It is dead *in production*
(section 4) but it declares `engine::Pack::Interface`, which
`source/interface.h`, `pipeline/pipeline.h` and 5 UI files depend on. So the
*header* is load-bearing; only `adapter.cpp`'s implementation is dead.

---

## 2. The misleading names

Each rename justified by what the directory actually contains.

| Current | Actually holds | Proposed | Why |
|---|---|---|---|
| `engine/core/log/` | logging + crash handler | `engine/log/` | Already correctly named; one level too deep. |
| `engine/core/events/` | the EventBus | `engine/events/` | Already correctly named. |
| `engine/core/trace/` | trace recorder | `engine/trace/` | Already correctly named. |
| `engine/core/util/` | `fs_utils.*`, `thread_priority.*`, `debug_env.h`, `process_utils.h` | `engine/util/` | Already correctly named. |
| `engine/core/keyring/` | `Keyring` interface + `FileKeyring` fallback | `engine/keyring/` | Already correctly named. Collides conceptually with `src/keyring/` (the Qt backend) - see the decision list. |
| `engine/core/vfs/` | `game_file.h`, `path_resolver.*`, `path_resolver_registry.*` | `engine/vfs/` | Already correctly named. `engine::vfs` namespace matches. |
| `engine/core/instance/` | `instance.*`, `mod_state.*`, `installed_pack_state.*`, `instance_snapshot.*`, `toml_utils.*`, `game_icons.*`, `masterlist_fetch.*`, + 2 dead | `engine/instance/` | Already correctly named. The *bucket* is the problem, not the child. |
| `engine/core/` (the 6 flat files) | `module.*` (module registry), `github.*` (GitHub API), `directory_refresher.*` | `engine/module/`, `engine/github.*`, `engine/dirwatch/` | Three unrelated things sharing a bucket. |
| `engine/pack/` | `Pack::Interface`, `Pack::SourceDetector` | `engine/modpack/detect/` | The "pack" it detects is a gmmpack. |
| `engine/modpack/` | `modpack::IncrementalUpdate`, `modpack::IniEdits` | `engine/modpack/update/`, `engine/modpack/ini/` | Destination, not source. |
| `engine/gmmpack/` | the on-disk archive format | `engine/modpack/format/` | `gmmpack` is a format name, not a place. Folding it under `modpack/` makes the four-way split one tree. |
| `engine/game/plugins/` | `.esm`/`.esp` file parsing | `engine/game/esp/` | "plugin" already means something else three directories away. |
| `engine/pipeline/plugin_host/` | the GMM plugin host | `engine/plugin_host/` | It is not a pipeline stage; `pipeline/plugin_claim_stage.cpp` is the stage. Lifting it out stops `plugin` nesting inside `pipeline`. |
| `engine/mod/overwrite/` | `OverwriteUtils` | `engine/modpack/overwrite/` | Belongs with the pack workflow, not with mod metadata. |
| `engine/network/nexus_v2/` | Nexus **API v2** client | `engine/source/nexus/api/` | It is a Nexus source concern; `network/network_manager.h` is the sanctioned facade and stays put. |
| `engine/update/` | app self-update **+** per-mod update check | `engine/selfupdate/` + `engine/source/update/` | Two unrelated features share a name. `self_updater.h:11` is a platform strategy; `mod_update_db_client.h:4` polls a mod dataset. |
| `engine/platform/theme/theme_manager.cpp` | Qt-free theme asset manager | `engine/theme/` | `src/ui/theme/` already exists for the Qt side. `engine/platform/` and `src/platform/` are two different libraries that both claim "platform". |
| `src/core/core.{h,cpp}` | `engine::Core::Application`, the composition root | `src/app/` | `src/core/` and `src/engine/core/` are two unrelated directories with the same name at different levels. |
| `ui/widgets/` | 81 files: models, dialogs, chrome, primitives | split (section 5) | A directory named "widgets" holding an item model, a debug dashboard and a 1734-line chart window. |

**Not renamed:** `engine/parallel/` (correct), `engine/index/` (correct, 4
files, one meaning), `engine/backup/` (correct, one meaning - see the decision
list), `engine/sort/` (correct after `dc77723`), `src/platform/`,
`src/runtime/`, `src/cli/`.

**Not renamed because the name is already honest and I could not justify it:**
`engine/pipeline/`. Its 13 stage files plus `plugin_host/` plus 2 registries is
one coherent thing. The user's instinct that install logic is spread across
`engine/install/`, `engine/pipeline/` and `engine/mod/` is right, but the fix is
to delete `engine/install/`, not to rename `pipeline/`.

---

## 3. The files that outgrew their purpose

### 3.1 `ui/controllers/mod_list_controller.cpp` - 4,592 lines (worst)

~75 member functions. It is not "the mod list controller"; it is six classes
that never got separated. The seams, by line range:

| Responsibility | Lines | Count | Extract to |
|---|---:|---:|---|
| **File-local CSV + sidecar helpers** | 117-281 | 7 free fns + 1 struct | `modlist_csv.{h,cpp}` |
| **Widget tree construction** | 345-755 | 1 (`setup_mod_list`) | keep here |
| **Profile switching** | 756-957 | 4 | `mod_profile_actions.{h,cpp}` |
| **Model sync / status bar** | 958-1349 | 6 | `mod_list_sync.{h,cpp}` |
| **Mod discovery + scan wiring** | 1350-1713 | 8 | `mod_discovery.{h,cpp}` |
| **Mod-list load + meta parse** | 1769-2274 | 2 (`load_meta_for_mods` is 413 ln alone) | `mod_metadata_loader.{h,cpp}` |
| **Conflict scan orchestration** | 2294-2530 | 8 | `mod_conflict_scan.{h,cpp}` |
| **Data-tab event dispatch** | 2545-2674 | 9 | `mod_actions.{h,cpp}` (it already exists, 1,082 ln) |
| **Mod-info dialog data assembly** | 2675-3025 | 3 (`build_mod_info_data` is 273 ln) | `mod_info_builder.{h,cpp}` |
| **Plugins-tab refresh** | 3043-3183 | 1 | `ui/panels/plugin_view.*` |
| **Priority / separator commands** | 3504-3601 | 12 | `mod_priority_actions.{h,cpp}` |
| **modlist.txt export / import** | 3690-3885 | 3 | `modlist_csv.{h,cpp}` |
| **open_folder dispatch** | 3886-3941 | 1 | `mod_actions.{h,cpp}` |
| **save_order / load_order** | 3966-4332 | 3 (`load_order` is **294 ln alone**) | `modlist_file.{h,cpp}` |

The two pathological single functions: `load_order` at 4,009-4,302 (294 lines)
and `load_meta_for_mods` at 1,861-2,274 (413 lines). Each is a line-oriented
file parser that has absorbed every edge case inline. Both belong in a
dedicated `modlist_file` / `mod_metadata_loader` unit with the CSV helpers from
line 117.

Realistic split: 4,592 lines becomes roughly 900 in the controller (widget
construction + dispatch) plus six 400-700 line units. That is below the 900
line ceiling the rest of the tree already respects.

### 3.2 `ui/widgets/mod_list_model.cpp` - 2,491 lines

Two very different problems stacked:

1. **`ModList::data()` is lines 70-561 - 492 lines, one function.** It is a
   hand-rolled `if (role == X && column == Y)` chain with ~25 custom roles.
   The standard Qt answer is to move the per-role work into a private
   `QVariant value_for(role, column, entry) const` per column, so `data()` is
   a 10-line dispatcher. This is the single highest-value change in the file.
2. **Lines 1434-1694 are 20 near-identical `set_*` setters**, each 4-10 lines
   (`set_conflict_stats`, `set_hidden_files`, `set_empty`, `set_fomod`,
   `set_root_override`, `set_invalid_data`, `set_no_metadata`, `set_content_dir`,
   `set_mirror_info`, `set_toggle_error`, `set_tags`, `set_source_info`,
   `set_git_info`, `set_category`, `set_category_names`, `set_category_ids`,
   `set_timestamps`, `set_separator_id`, ...). They differ only in which
   `ModEntry` field they assign and which `dataChanged` span they emit. One
   `auto set_field(ModEntry& e, auto ModEntry::*field, auto value)` template
   plus a span collapses all 20 to 20 three-line bodies.

The fold and nesting machinery (1,804-2,110, ~300 lines) is a **coherent third
concern** and is already separated enough to stay.

### 3.3 `engine/pipeline/plugin_host/plugin_loader.cpp` - 2,233 lines

**50 `static void cb_*` ABI trampoline functions** (`cb_register_identity`,
`cb_register_meta`, `cb_register_category`, `cb_register_categories`,
`cb_register_settings`, `cb_register_settings_tab`, `cb_register_diagnostics`,
`cb_register_save_parser`, `cb_register_animation_parser`,
`cb_register_game_feature`, `cb_register_game_feature_data`, ...). Each is
8-30 lines of "unpack C args, cast `ctx->user_data`, call a registry". They
share one `RegistrationBridge` context (`plugin_loader.cpp:67-68`).

This is a table, not a file. The natural seam is one `.cpp` per registry family
(`cb_identity`, `cb_settings`, `cb_diagnostics`, `cb_game_feature`,
`cb_sorter`, ...) with the bridge in a shared `registration_bridge.h`.

### 3.4 `engine/pipeline/plugin_host/python_loader.cpp` - 2,045 lines

**17 `struct Py*Provider` pybind11 adapters**, each 20-45 lines and each doing
the identical thing: cast a `PyObject*` to `py::function`, check it, call it,
translate the exception. `PyDiagnosticsProvider` (line 43), `PyEventHandler`
(89), `PyRequirementsProvider` (134), `PyGuidedFixProvider` (194),
`PyDiagnoseProvider` (212), `PyFileMapperProvider` (354),
`PyOrderEncodingProvider` (410), `PyDeployProvider` (436), `PyHookProvider`
(476), `PyToolProvider` (495), `PyPreviewProvider` (518), ...

Same fix as 3.3: one file per callback family, and a shared
`py_call_guard` helper for the call-and-translate boilerplate.

**3.3 and 3.4 together are 4,278 lines of the same pattern** and are the best
value-per-line split in the codebase, because the pattern is mechanical.

### 3.5 Also over 1,000 lines

| File | Lines | Seam |
|---|---:|---|
| `ui/controllers/launch_controller.cpp` | 2,034 | Proton/Wine launch options, elevation, profile switching, per-executable config. Split the Proton concern out (it already has an engine home: `engine/deploy/launch/proton_tools.*`). |
| `ui/modpack/modpack_install_wizard.cpp` | 1,875 | Only 2 section banners exist (line 917, 1343), so the file was never structured. Split: dependency resolution, per-mod install, post-install ini/tweak application. |
| `ui/controllers/settings_controller.cpp` | 1,826 | Zero section markers. Almost certainly "one method per settings page" - a natural split by page. |
| `ui/widgets/debug_window.cpp` | 1,734 | 7 section banners at 148, 260, 451, 498, 657, 896, 1212. Already self-segmented; the split is free. |
| `engine/gmmpack/packer.cpp` | 1,641 | 6 section banners at 341, 457, 513, 566, 726, 893. Archive framing vs. tree serialisation vs. bsdiff patching vs. executable embedding. |
| `ui/settings/settings_content_widget.cpp` | 1,580 | One method per settings page. |
| `engine/network/network_manager.cpp` | 1,433 | **Leave it.** `network_manager.h:7-10` declares it the single sanctioned network facade. It is a facade, and facades earn their line count. |
| `ui/panels/downloads_tab.cpp` | 1,397 | Check for section markers before splitting. |

---

## 4. The dead code

**Method.** For every candidate I checked: (a) who includes the header, (b) who
instantiates the class, (c) whether any string literal names it, (d) whether
CMake builds it, (e) whether any test file references it. The string-dispatch
check was done first, per the warning in the work order.

### 4.1 Alive via string dispatch - NOT dead, do not delete

The deploy factory `engine/deploy/core.cpp:16-54` dispatches on a **string**:

```cpp
if (name == engine::kDeployStrategyOverlayFs) ...   // core.cpp:20
if (name == kDeployStrategyDirect || name.empty() || name == "symlink")
  return std::make_unique<Symlink>(case_sensitive); // core.cpp:44-45
```

The two string constants are `engine/game/registry/game_knowledge.h:83-84`
(`"overlayfs"`, `"direct"`). The only user-facing choices are the two items
added to the combo box at `ui/instance_options/instance_options_widget.cpp:582`
and `:586`.

| File | Verdict | Proof |
|---|---|---|
| `deploy/strategy_symlink.cpp` | **ALIVE** | `core.cpp:27,33,45,53` - the fallback for *every* unrecognised string, including empty and typo'd settings values. |
| `deploy/strategy_overlayfs_deploy.cpp` (`Deploy::OverlayFsDeploy`) | **ALIVE** | `core.cpp:35` (string `"overlayfs"`), `deploy_utils.cpp:656`. |
| `deploy/strategy_direct.cpp` (`Deploy::Direct`) | **ALIVE** | Not via the factory - constructed directly at `core/instance/instance_utils.cpp:557`. Also `"direct"` is a live UI choice (`instance_options_widget.cpp:582`) and `tests/engine/deploy_direct_test.cpp` exercises it. **The comment at `core.cpp:42-43` explicitly says the factory does not construct it.** |

This is exactly the case the work order warned about, and the warning was
correct to raise. All three survive.

### 4.2 Truly dead - zero references anywhere, safe to delete

Verified: each header's only includer is its own alias header; each class name
appears in no other file; no test references it; no string literal names it.

| Unit | Lines | Class / content | Proof |
|---|---:|---|---|
| `deploy/hardlink.h` + `deploy/strategy_hardlink.{h,cpp}` | 98 | `Deploy::Hardlink` | `strategy_hardlink.h` has **zero** includers outside itself |
| `deploy/junction.h` + `deploy/strategy_junction.{h,cpp}` | 178 | `Deploy::Junction` | `strategy_junction.h` has **zero** includers outside itself |
| `deploy/vfs.h` + `deploy/strategy_vfs.{h,cpp}` | 176 | `Deploy::Vfs`, FUSE3-only | `strategy_vfs.h` has **zero** includers outside itself; only compiled `if(FUSE3_FOUND)` (`engine/CMakeLists.txt:199`) |
| `deploy/overlay_fs_kernel.h` + `deploy/strategy_overlayfs.{h,cpp}` | 287 | `Deploy::OverlayFsKernel`, FUSE3-only | `strategy_overlayfs.h` has **zero** includers outside itself. **Distinct class from the live `OverlayFsDeploy`** - same name prefix, different class. |
| `deploy/abi_adapter.h` + `deploy/abi_deploy_strategy.h` + `deploy/abi_order_hook.h` | 96 | alias headers | `abi_deploy_strategy.h` and `abi_order_hook.h` have **zero** includers; `abi_adapter.h`'s only includer is `abi_deploy_strategy.h` |
| `deploy/launch/usvfs_mapping.{h,cpp}` | 5 | `UsVfsMapping` | Only self-reference (`usvfs_mapping.cpp:2`) plus the two CMake lines at `engine/CMakeLists.txt:181-182`. The `.cpp` is **2 lines**. |
| `core/instance/instance_manager.{h,cpp}` | 473 | `engine::InstanceManager` | `instance_manager.h` has **zero** includers outside `instance_manager.cpp:1`. Zero test references. |
| `core/instance/mo2_importer.{h,cpp}` | 364 | Mo2 importer | `mo2_importer.h` has **zero** includers outside `mo2_importer.cpp:1`. Zero test references. (Note: the *tested* `ModMeta::from_mo2_import` is a different function in `engine/mod/meta/mod_meta.h:34` and is alive.) |

**Total truly dead: 8 units, 1,677 lines.**

`InstanceManager` is the one I would double-check with a human before deleting.
384 lines implementing `create`, `create_portable`, `rename`, `list_all`,
`find_by_name`, `last_active_name` is a complete, coherent instance-manager API.
It looks like a *superseded* implementation that was replaced by
`InstanceRegistry` + `Instance` + `instance_utils.cpp`, not like scratch code.
The evidence says delete; the shape says ask.

### 4.3 Dead in production, live in tests only

**This is the important bucket, and "delete it" is the wrong recommendation.**

| Unit | Prod lines | Test lines | Proof |
|---|---:|---:|---|
| `engine/install/` (all 5: `conflict_resolver`, `fresh_install`, `append_install`, `instance_router`, `game_match_validator`) | 916 | 883 | Zero production references. Verified by grepping `engine::Install::`, all five header paths, and the exported symbol names (`plan_fresh_install`, `plan_append_install`, `plan_instance_route`) across all of `src/` - **zero hits outside `src/engine/install/`**. |
| `engine/pack/adapter.{h,cpp}` | 283 | 241 | `adapter.cpp` is included only by `tests/engine/pack_adapter_test.cpp:5` and transitively by `engine/install/conflict_resolver.h:17` (itself test-only). The *header* is load-bearing: `engine::Pack::Interface` is what `source/interface.h` and 5 UI files use. |
| `engine/collection/batch_installer.{h,cpp}` | 262 | 309 | `batch_installer.h` included only by `tests/engine/collection_batch_install_test.cpp:12`. |

**I need to check one thing before recommending anything here:** I enumerated
every `engine/` header included by `ui/modpack/`, `ui/install/`, `ui/fomod/` and
`mod_list_model.cpp`. The modpack install wizard - the natural caller for
`plan_fresh_install` / `plan_append_install` - includes
`engine/gmmpack/{packer,types,unpacker}.h`, `engine/source/registry.h` and
`engine/pipeline/pipeline.h`. It does not touch `engine/install/` at all.

So these are **8 test binaries and 1,433 lines of passing tests guarding code
that no shipped path calls.** That is a half-finished migration or a
deliberately parked feature, not dead code. Deleting 1,433 lines of green tests
on my authority would be the expensive mistake this work order warned about.

**Recommendation: do not delete. File a ticket, ask the user.** The likely
answers are (a) the wizard was rewritten to bypass these planners and they are
now genuinely obsolete - delete both halves; or (b) these are the intended
design and the wizard bypass is the bug - wire them up. I cannot tell which
from the tree.

### 4.4 Ambiguous - needs a human

| Item | Lines | Question |
|---|---:|---|
| `deploy/launch/ci_intercept.c` | 541 | `src/CMakeLists.txt:141-153` labels it `=== BROKEN FEATURE - DO NOT ENABLE ===`, says it "broke Pandora's game-tree reads (2026-08-09)", and that it is "built + unit-tested only as kept-reference". It **is** still built (`CMakeLists.txt:147`) and still tested (`tests/CMakeLists.txt:771-776`). 541 lines plus one test binary kept alive by an explicit "do not enable" comment. Delete, or keep as reference? **The comment argues for deleting it; someone deliberately kept it. Ask.** |
| `engine/mod/model/profile.{h,cpp}` | ~400 | `engine::ProfileModel` is a Qt-free data struct in the engine, consumed by `deploy/order_hook.h`, `abi_bridge.cpp`, `ui/widgets/debug_window.cpp`, `ui/controllers/mod_list_controller.cpp` and 2 tests. Alive but misfiled - it is a mod-list model, not "a model of a mod". |
| `engine/backup/backup_service.{h,cpp}` | 752 | Live (`ui/controllers/backup_actions.h`, `tests/engine/backup_service_test.cpp`). Name is honest. Left alone. |
| `engine/network/network_manager.{h,cpp}` | 1,433 | Live, 30 includers, and self-documented as the sanctioned facade (`network_manager.h:7-10`). Left alone. |

**Bucket counts: 8 truly dead (1,677 lines) / 3 test-only units (1,461 prod
lines + 1,433 test lines) / 3 alive-via-string-dispatch (do not touch) / 4
ambiguous.**

---

## 5. The proposed structure

```
src/
  main.cpp
  app/                                  <- NEW. from src/core/ (renamed, 2 files)
    app.h  app.cpp
  cli/                                  unchanged
  platform/                             unchanged (gmm_platform)
  runtime/                              unchanged (gmm_runtime)
  keyring/                              unchanged (Qt keyring, app-only)
  engine/
    log/                                <- engine/core/log/            [M1]
    events/                             <- engine/core/events/         [M2]
    trace/                              <- engine/core/trace/          [M3]
    util/                               <- engine/core/util/           [M4]
    vfs/                                <- engine/core/vfs/            [M5]
    keyring/                            <- engine/core/keyring/        [M6]
    instance/                           <- engine/core/instance/       [M7]
    module/                             <- engine/core/module.*        [M8]
    github.{h,cpp}                      <- engine/core/github.*        [M8]
    dirwatch/                           <- engine/core/directory_refresher.*
    parallel/                           unchanged

    modpack/                            [M9] one feature, one tree
      format/                           <- engine/gmmpack/{packer,unpacker,sha256,
        packer.{h,cpp}                     bsdiff,patch,tree_parser,schema_validator,
        unpacker.{h,cpp}                   uuid,codec.h,types.h}
        sha256.{h,cpp}
        bsdiff.{h,cpp}
        patch.{h,cpp}
        tree_parser.{h,cpp}
        schema_validator.{h,cpp}
        uuid.{h,cpp}
        codec.h  types.h
      ini/
        ini_edit_parser.{h,cpp}         <- engine/gmmpack/ini_edit_parser.*
        ini_edits.{h,cpp}               <- engine/modpack/ini_edits.*
      executable/
        executable_pipeline.{h,cpp}     <- engine/gmmpack/executable_pipeline.*
      detect/
        source_detector.{h,cpp}         <- engine/pack/source_detector.*
        interface.h                     <- engine/pack/interface.h (content of
                                           pack/adapter.h that defines
                                           engine::Pack::Interface - SEE NOTE)
      update/
        incremental_update.{h,cpp}      <- engine/modpack/incremental_update.*
      overwrite/
        overwrite_utils.{h,cpp}         <- engine/mod/overwrite/overwrite_utils.*

    mod/                                unchanged dirs
      meta/  cache/  model/  archive/  filetree/  fomod/
      (model/profile.* -> renamed to mod/model/mod_list_model.* if the user
       agrees; the class ProfileModel is a mod-list model)

    source/                             [M10] impl + decl together, per provider
      interface.h  router.h  registry.{h,cpp}  http_util.{h,cpp}
      update_policy.{h,cpp}  source_provider.{h,cpp}
      nexus/
        provider.{h,cpp}     <- source/nexus/provider.h  + source/nexus_provider.cpp
        auth.{h,cpp}         <- source/nexus/auth.h      + source/nexus_auth.cpp
        http.{h,cpp}         <- source/nexus/http.h      + source/nexus_http.cpp
        account.{h,cpp}      <- source/nexus/account.h   + source/nexus_account.cpp
        servers.{h,cpp}      <- source/nexus/servers.h   + source/nexus_servers.cpp
        api/                 <- engine/network/nexus_v2/   (6 files)
      loverslab/
        provider.{h,cpp}     <- source/loverslab/provider.h + loverslab_provider.cpp
        auth.{h,cpp}         <- source/loverslab/auth.h     + loverslab_auth.cpp
      steam/
        provider.{h,cpp}     <- source/steam/provider.h    + steam_workshop_provider.cpp
        client.{h,cpp}       <- source/workshop/workshop_client.*
        remote_cache.{h,cpp} <- source/workshop/remote_cache.*
      modl/                unchanged
      modpub/              unchanged
      nxm/  git/  download/ unchanged
      update/                       <- mod_update_db_client.{h,cpp} + install_method.*

    game/                               [M11]
      detect/    unchanged
      esp/                            <- game/plugins/{esp_header,plugin_database,plugin_file,plugin_info}
      saves/     unchanged
      registry/  unchanged

    plugin_host/                       <- engine/pipeline/plugin_host/   [M12]
      abi_bridge.*  plugin_loader.cpp  python_loader.cpp  gmm.pyi
      settings.*  diagnostics.*  diagnose.*  file_mapper.*  save_parser.*
      requirements.*  order_encoding.*  deploy_strategy.*  hook.*  category_factory.*
      tool_registry.*

    pipeline/                          stage files + registry/ only
      *.cpp (13 stages)  registry/
    deploy/                            unchanged (+ strategy deletions, section 4.2)
      strategy/  launch/
    sort/  index/  profile/  backup/  selfupdate/
    selfupdate/                        <- engine/update/{self_updater*, windows_*, macos_*}
    proton/                            unchanged
    theme/                             <- engine/platform/theme/         [M13]
    tools/                             <- engine/platform/tools/
    (engine/platform/ then has no remaining children and is deleted)

  ui/
    app/                unchanged
    controllers/        unchanged dir, mod_list_controller split (section 3.1)
    main_window/        unchanged
    workers/            unchanged
    modlist/
      model/                     <- widgets/mod_list_model.*  + mod_table_view.*
      csv.{h,cpp}                <- (extracted from mod_list_controller.cpp:117-281)
      file.{h,cpp}               <- (extracted: save_order/load_order)
    profile_bar/  toolbar/  status_bar/  menu_bar/  chrome/    <- widgets/, split by role
    dialogs/                                                  <- widgets/*_dialog.*, *_popup.*
    modinfo/  panels/  modpack/  install/  fomod/  settings/  overwrite/
    viewer/  preview/  profile/  instance_options/  game_selection/
    system_tray/  theme/  notify/  nxm/  network/
    primitives/                                                  <- the 7 MO2-parity
                                                                   primitives from
                                                                   ui/CMakeLists.txt:73-77
```

### 5.1 Non-1:1 mappings (old path -> new path), explicit

| Old | New | 1:1? |
|---|---|---|
| `src/core/core.{h,cpp}` | `src/app/app.{h,cpp}` | yes, renamed |
| `src/engine/core/{module.cpp,module.h}` | `src/engine/module/` | dir |
| `src/engine/core/{github.cpp,github.h}` | `src/engine/github.{h,cpp}` | yes |
| `src/engine/core/directory_refresher.*` | `src/engine/dirwatch/directory_refresher.*` | dir |
| `src/engine/gmmpack/*` | `src/engine/modpack/format/*` | regrouped (10 pairs + 2 headers) |
| `src/engine/modpack/ini_edits.*` | `src/engine/modpack/ini/ini_edits.*` | regrouped |
| `src/engine/modpack/incremental_update.*` | `src/engine/modpack/update/incremental_update.*` | regrouped |
| `src/engine/mod/overwrite/overwrite_utils.*` | `src/engine/modpack/overwrite/overwrite_utils.*` | regrouped |
| `src/engine/pack/source_detector.*` | `src/engine/modpack/detect/source_detector.*` | regrouped |
| `src/engine/pack/adapter.*` | `src/engine/modpack/detect/interface.*` | **renamed file** - see note |
| `src/engine/source/{nexus,loverslab,steam,workshop}/*.h` | `src/engine/source/{nexus,loverslab,steam}/*.{h,cpp}` | **merged with the .cpp** |
| `src/engine/source/nexus_*.cpp` etc. | merged as above | **merged** |
| `src/engine/network/nexus_v2/*` | `src/engine/source/nexus/api/*` | regrouped |
| `src/engine/game/plugins/*` | `src/engine/game/esp/*` | dir rename |
| `src/engine/pipeline/plugin_host/*` | `src/engine/plugin_host/*` | lifted |
| `src/engine/update/{self_updater*,windows_*,macos_*}` | `src/engine/selfupdate/*` | regrouped |
| `src/engine/update/mod_update_db_client.*` | `src/engine/source/update/*` | regrouped |
| `src/engine/update/install_method.*` | `src/engine/source/update/*` | regrouped |
| `src/engine/platform/theme/*` | `src/engine/theme/*` | lifted |
| `src/engine/platform/tools/*` | `src/engine/tools/*` | lifted |
| `src/ui/widgets/*` (81 files) | split across 5 dirs | **the judgement call** |
| `src/engine/update/self_updater_p.h` | `src/engine/selfupdate/self_updater_p.h` | with its owner |

**NOTE on `pack/adapter.h` -> `modpack/detect/interface.h`:** this rename is the
one I am least confident about. `adapter.h` declares `engine::Pack::Interface`
**and** a second class. The header is load-bearing (5 UI files + 2 engine
files). The right split is: move the `Interface` declaration to
`modpack/detect/interface.h` and delete the rest of `adapter.h` along with
`adapter.cpp` per section 4.3 - **but only if the user agrees to drop the
`pack_adapter_test.cpp` tests.** If they do not, leave `adapter.h` where it is
and only move `source_detector.*`. **Do not do this rename unilaterally.**

### 5.2 The `ui/widgets/` split - a judgement call, not a recommendation

81 files, 15,870 lines. The directory holds at least six different kinds of
thing:

| Kind | Files | Line count |
|---|---:|---:|
| Qt models | `mod_list_model.*`, `mod_table_view.*` | ~3,800 |
| Screen-level widgets | `right_panel.*`, `menu_bar.*`, `main_toolbar.*`, `profile_bar.*`, `status_bar.*`, `right_filter_bar.*`, `exec_controls_bar.*`, `zoom_controls.*` | ~2,100 |
| Content-tab widgets | `install_widget.*`, `pipeline_content_widget.*`, `executables_content_widget.*`, `stats_content_widget.*`, `instance_switcher_content_widget.*` | ~2,600 |
| Dialogs | `task_dialog.*`, `save_info_dialog.*`, `instance_switcher_dialog.*`, `instance_statistics_dialog.*`, `list_dialog.*`, `error_popup.*`, `executables_dialog.*`, `pipeline_window.*` | ~2,400 |
| Reusable primitives | the 7 MO2-parity ones at `ui/CMakeLists.txt:73-77`, plus `rolling_chart.*`, `line_number_edit.*`, `web_link.*`, `dialog_placement.*`, `line_edit_clear.*`, `column_toggle_header.*`, `smooth_scroll.h`, `event_filter.*` | ~2,800 |
| Feature-specific | `game_icon_cache.*`, `category_filter_panel.*`, `debug_window.*` (1,734 + 264) | ~2,200 |

`debug_window.cpp` alone is 1,998 lines and is a debug dashboard, not a widget.

**I am not going to dictate this split.** It changes 229 include lines and is
the single largest blast radius in the proposal. It should be the **last** thing
attempted, one sub-group at a time, and only after the directory has a written
rule. My one hard recommendation: `mod_list_model.*` (2,491 lines) is a model,
not a widget, and moving it to `ui/modlist/model/` buys the most clarity per
include rewritten. Everything else about `ui/widgets/` is the user's call.

---

## 6. The migration

### 6.1 Risk reality check

**This codebase has been damaged before.** `.clang-format:8` sets
`UseCRLF: false`, so any `clang-format -i` over an existing file silently
rewrites every line ending and produces churn unrelated to the change. Two rules
for this migration:

1. **`clang-format -i` is forbidden.** Not once, not on a "harmless" whitespace
   fix, not on a file you are only renaming. If a file needs reformatting after
   a move, that is a separate `edit` call, one hunk at a time, verified with
   `clang-format --dry-run --Werror <file>` (read-only).
2. **A `git mv` is the only move mechanism.** Not `mv`, not `cp`+`rm`, not a
   Python script, not `sed`. `git mv` records the rename in the index so
   `git log --follow` and `git blame` survive the move. After every move,
   `git diff --stat -M` must show renames, not delete+add pairs. **If it shows
   delete+add, stop and investigate** - that means the file changed enough that
   git could not detect the rename, which on a pure move means something
   unintended happened.

The reassuring fact, from section 0: **every include is rooted at `src/` and
carries its full relative path.** There is no `#include "../.."`, no relative
shelling, no path-mapping layer. A wrong rewrite produces a loud compile error
pointing at the exact file, never a silent mis-resolution. That is the main
reason this is tractable at all.

### 6.2 Phase 0 - deletions (zero include churn)

Delete the 8 truly-dead units from section 4.2. Remove their 4 CMake entries
(`engine/CMakeLists.txt:69-70` hardlink, `:72` junction, `:181-182` usvfs,
`:199-200` vfs+overlayfs, plus the flat `abi_*.h` are header-only so they are
already unreferenced). **No include line changes at all, because nothing
includes them.** One build. This is the cheapest, safest, most immediately
valuable phase.

Do **not** include the section 4.3 units. Ask first.

### 6.3 Phase 1 - dissolve `engine/core/` (7 moves, ~330 include lines, 7 CMake lines)

Each subdirectory moves **on its own**, in ascending order of include count, one
PR each. Order matters: doing the small ones first means the first three moves
touch 41 include lines total and you learn the mechanics cheaply.

| # | Move | Include lines | CMake lines | Risk |
|---|---|---:|---:|---|
| 1.1 | `core/keyring/` -> `keyring/` | 6 | 1 | lowest |
| 1.2 | `core/trace/` -> `trace/` | 10 | 1 | low |
| 1.3 | `core/events/` -> `events/` | 16 | 1 | low |
| 1.4 | `core/vfs/` -> `vfs/` | 18 | 1 | low |
| 1.5 | `core/util/` -> `util/` | 55 | 1 | low |
| 1.6 | `core/log/` -> `log/` | **125** | 1 | medium - most includers |
| 1.7 | `core/instance/` -> `instance/` | 98 | 1 | medium |
| 1.8 | `core/{module,github,directory_refresher}` -> 3 homes | ~6 | 1 | low, but splits |

Total: ~336 include rewrites across ~8 commits. `clang-format --dry-run
--Werror` on every touched file.

### 6.4 Phase 2 - merge the four modpack directories (~102 include lines, 5 CMake lists)

Order: move `gmmpack/` to `modpack/format/` first (65 include lines, biggest
single risk in this phase), then the small regroupings (`modpack/ini_edits` 12,
`pack/source_detector` 6, `mod/overwrite/overwrite_utils` ~5, `network/nexus_v2`
~14). Four commits.

`engine/pack/adapter.*`: **blocked on a user decision** (section 5.1).

### 6.5 Phase 3 - merge source providers impl+decl (~143 include lines)

The highest-value move in the proposal: `source/nexus/` becomes a real
directory containing `.h` **and** `.cpp` for each of the 5 Nexus units. This is
143 include rewrites (the 143 figure is `engine/source/`'s current total, so
the real number is lower - only the 8 affected units move). Four commits:
nexus, loverslab, steam+workshop, then `nexus_v2` -> `nexus/api/`.

### 6.66. Phase 4 - renames with judgement attached (4 commits)

`game/plugins/` -> `game/esp/`, `pipeline/plugin_host/` -> `plugin_host/`,
`update/` split, `platform/{theme,tools}` -> `{theme,tools}` + delete
`engine/platform/`. Low include counts each (86 for all of `deploy/` is not
touched here; these are all under 45).

### 6.7 Phase 5 - `src/core/` -> `src/app/` (2 files, 3 include lines, 1 CMake line)

Trivial. Do it early, not late - it is the cheapest demonstration that the
method works.

### 6.8 Phase 6 - file splits, NOT moves (separate decision per file)

Section 3. These are the **highest-risk** items because they move code between
functions, not between directories. They are also the only changes in this
proposal that can change behaviour.

Recommended order, cheapest first:

| Order | File | Method | Behaviour risk |
|---|---|---|---|
| 6.a | `mod_list_model.cpp` | extract the 20 `set_*` setters behind one template; split `data()` | low - mechanical, no logic |
| 6.b | `python_loader.cpp` | one file per `Py*Provider` | low - no logic change, only file boundaries |
| 6.c | `plugin_loader.cpp` | one file per `cb_*` family | low - same |
| 6.d | `packer.cpp`, `debug_window.cpp` | cut at the existing section banners | low - banners already exist |
| 6.e | `mod_list_controller.cpp` | the 14-way split from section 3.1 | **high** - this is where a mistake hides |

**Every one of 6.a-6.d needs a build AND the relevant `ctest -R <pattern>` run,
not just a build.** The user's memory records that
`cmake --build build --target <test>` does **not** rebuild the engine library the
test links, so a negative control can silently stay green. Always
`cmake --build build` (full, incremental) before trusting any test result here.

`mod_list_controller.cpp` should be split **last, and one seam at a time**, with
a build plus `ctest -R mod_list` between each of the 14 extractions. That is 14
cycles. It is the right number. It is also the single highest-risk item in this
document and I would not start it until phases 0-5 have landed and settled.

### 6.9 Blast radius summary

| Phase | Moves | Include rewrites | CMake lists | Build+test cycles |
|---|---:|---:|---:|---:|
| 0. deletions | 8 units | **0** | 1 | 1 |
| 1. dissolve `engine/core/` | 8 | ~336 | 1 | 8 |
| 2. modpack merge | 5 | ~102 | 1 | 4 |
| 3. source providers | 4 | ~90 | 1 | 4 |
| 4. renames | 4 | ~120 | 2 | 4 |
| 5. `src/core/` -> `src/app/` | 1 | 3 | 1 | 1 |
| 6. file splits | 6 | 0 (no includes) | 0-1 | 14+ |
| 7. `ui/widgets/` split | **deferred, the user's call** | 229 | 1 | ? |

**`src/ui/widgets/` blast radius, stated plainly:** 81 files, 229 include
lines, 78 CMake entries, 1,198 test-file references to check, and 6 of the 10
largest files in the entire UI. It is roughly **one third of the entire migration
cost for a change that is purely organisational.** It should not be in the same
plan as anything else.

---

## 7. Decisions that are the user's, not mine

1. **`engine/install/` + `pack/adapter` + `collection/batch_installer`** -
   1,461 production lines, zero production callers, **1,433 lines of passing
   tests**. Delete the tests and the code, or wire the code up? I cannot tell
   from the tree which was intended. **Do not delete without asking.**
2. **`ci_intercept.c` (541 lines)** - explicitly labelled broken and
   "DO NOT ENABLE", deliberately kept and tested. Delete or keep as reference?
3. **`ui/widgets/`** - does it get split at all, and if so where? 229 include
   lines for pure organisational gain. My only strong view is that
   `mod_list_model.*` is not a widget.
4. **`engine/mod/model/profile.*` -> rename to `mod_list_model.*`?** The class
   is `ProfileModel` but it models a mod list, and it collides by name with
   `engine::profile::Profile`. Rename or leave?
5. **`engine/backup/`** - name is honest, feature is live. I found no reason to
   touch it. Confirm that is right rather than assuming I missed something.
6. **`src/keyring/` vs `engine/keyring/`** - after move 1.1 both exist at
   `src/keyring/` and `src/engine/keyring/`. One is the Qt backend, one is the
   interface. Acceptable, or rename the engine one `secret_store/`?
7. **Order** - should the file splits (phase 6, highest risk) come before or
   after the moves? I recommend after. Reversing is defensible if the splits are
   wanted sooner.

---

## What was NOT done

- No file in `src/` was moved, renamed, edited or deleted.
- No build was run. No test was run. No `ctest`.
- No commit, no push, no PR, no version bump.
- No `clang-format` invocation of any kind.
- The dirty `resources` submodule was not touched. `.wt/` was not touched. No
  stash was dropped.
- `bd` has no ticket matching `layout`. The closest open tickets are
  `Workspace-y0f2` ("aon nits: dead code, ...") which overlaps section 4, and
  `Workspace-07bi`. **No beads ticket was created for this** - the user should
  decide whether this proposal becomes one before the work starts.

## Uncertainties, stated

- **`InstanceManager` (473 lines)** - evidence says delete, shape says ask. A
  complete, coherent, unreferenced API is more often *superseded* than dead.
- **`engine/install/`** - proved zero production callers by three independent
  methods (namespace grep, header-path grep, symbol-name grep, plus an
  enumeration of every `engine/` header the install UI includes). The call-graph
  conclusion is solid. **What is unclear is intent**, not fact.
- **`pack/adapter.h`'s `Interface`** - I know the header is load-bearing and I
  know `adapter.cpp` is not, but I have not read all 222 lines of the header to
  split it cleanly. Treat the `interface.h` rename as unproven.
- **`ui/modinfo/` (56 files, 8,573 lines)** and **`ui/settings/` (12 files,
  4,423 lines)** - both are large single-feature directories that I did not audit
  for internal fragmentation. They may contain the same "impl flat, header in a
  subdir" pattern found in `engine/source/`. **Unaudited.**
- I did not verify whether any of these paths are referenced by the **Plugins**
  or **ABI** submodules. `src/engine/CMakeLists.txt:8` notes
  "cross-repo include path in Plugins repo" for `engine/sort/`. If `Plugins`
  includes `engine/core/log/logger.h` or similar, phases 1-2 are not
  `git mv`-local to this repo and need coordination. **Check this before
  starting.**