# Feature Map — MO2 vs GMM

Living document tracking MO2 feature parity and GMM-exclusive features.

**Legend:**
- ✅ = Implemented in GMM
- ⚠️ = Partially implemented (gaps remain)
- ❌ = Missing in GMM (MO2 has it)
- 🚀 = GMM-exclusive (surpasses MO2)

---

## 1. Virtual Filesystem

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| USVFS controller (dll loading) | ✅ `usvfs_x64.dll` | ✅ `UsvfsLibrary` | ✅ |
| VFS create/reset | ✅ `usvfsCreateVFS` | ✅ `UsvfsConnector` | ✅ |
| Directory-level virtual links | ✅ `usvfsVirtualLinkDirectoryStatic` | ✅ `UsvfsConnector::updateMapping` | ✅ |
| File-level virtual links | ✅ `usvfsVirtualLinkFile` | ❌ | ❌ |
| Priority-ordered mod mapping | ✅ `OrganizerCore::fileMapping` | ✅ `build_usvfs_mapping` | ✅ |
| Create-target (write destination) | ✅ `LINKFLAG_CREATETARGET` | ✅ | ✅ |
| Custom overwrite target | ✅ `customOverwrite` param | ✅ | ✅ |
| Local saves redirect | ✅ `LocalSavegames::mappings` | ✅ `bind_mount` in overlay | ✅ |
| Plugin file-mapper mappings | ✅ `IPluginFileMapper::mappings` | ❌ | ❌ |
| VFS auto-mapping (BSA-aware) | ✅ `DirectoryEntry::addFromBSA` | ❌ | ❌ |
| Archive load order injection | ✅ `enabledArchives` priority | ❌ | ❌ |
| Forced library loading | ✅ `usvfs::setForcedLibraries` | ❌ | ❌ |
| OverlayFS (Linux) | ❌ | 🚀 `OverlayFsLauncher` | 🚀 |
| LD_PRELOAD intercept (Linux) | ❌ | 🚀 `PreloadInterceptor` | 🚀 |
| Case-insensitive path resolution | ❌ | 🚀 `PathResolver` + `resolve_regular_file_ci` | 🚀 |
| PathResolver registry | ❌ | 🚀 `PathResolverRegistry` (per-root cache) | 🚀 |
| FUSE-based VFS (Linux) | ❌ | 🚀 `VfsStrategy` (FUSE + file_map) | 🚀 |

## 2. Launch Pipeline

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Hooked process creation | ✅ `usvfsCreateProcessHooked` | ✅ `UsvfsLauncher::launch` | ✅ |
| Plain process creation | ✅ `CreateProcessW` | ✅ `NativeRuntime` | ✅ |
| Process monitoring (Job Object) | ✅ `CreateJobObjectW` | ✅ `UsvfsProcessMonitor` | ✅ |
| Process monitoring (cgroup v2) | ❌ | 🚀 `cgroup_is_empty` + subreaper | 🚀 |
| Exponential backoff | ✅ 50ms-2s | ✅ `UsvfsProcessMonitor` | ✅ |
| Interesting process selection | ✅ `findInterestingProcessInTrees` | ✅ `isHiddenProcess` + `Interest` enum | ✅ |
| Hidden process filtering | ✅ `conhost.exe` + MO2 exe | ✅ `conhost.exe` + GMM exe | ✅ |
| Cancel / force-unlock | ✅ `UILocker::Session` | ✅ cancel atomic + timeout | ✅ |
| Exit code capture | ✅ `GetExitCodeProcess` | ✅ `GetExitCodeProcess` | ✅ |
| Wait-for-all on app exit | ✅ `waitForAllUSVFSProcessesWithLock` | ❌ | ❌ |
| Process tree descendant walk | ✅ | ✅ `get_process_descendants()` (PPID chain) | ✅ |
| Steam -- set SteamAPPId | ✅ `env::set("SteamAPPId", ...)` | ✅ | ✅ |
| Steam -- auto-start | ✅ `checkSteam` + registry `SteamExe` | ❌ | ❌ |
| Steam -- elevation mismatch | ✅ `canAccess` + admin dialog | ❌ | ❌ |
| Steam -- Proton/Wine compat | ❌ | 🚀 `STEAM_COMPAT_*` env | 🚀 |
| Proton tooling (winetricks/protontricks) | ❌ | 🚀 `run_proton_tool()` fallback chain | 🚀 |
| PATH manipulation | ✅ `env::appendToPath` | ✅ `appendToPath` | ✅ |
| CWD resolution | ✅ `Executable::workingDirectory` | ✅ `weakly_canonical` + fallback | ✅ |
| Virtualized binary in mods/ | ✅ `adjustForVirtualized` | ❌ | ❌ |
| File type dispatch (.bat, .jar) | ✅ `getFileExecutionContext` | ❌ | ❌ |
| Java detection for .jar | ✅ `findJavaInstallation` | ❌ | ❌ |
| CREATE_BREAKAWAY_FROM_JOB | ✅ | ✅ `UsvfsLauncher` | ✅ |
| Subreaper + supervisor | ❌ | 🚀 `PR_SET_CHILD_SUBREAPER` | 🚀 |
| Wine runtime (non-Steam Windows exe) | ❌ | 🚀 `WineRuntime` | 🚀 |
| LaunchParams (structured) | ✅ `SpawnParameters` | ✅ `LaunchParams` (pid, overlay, cgroup, capture) | ✅ |

## 3. Error Handling & Diagnostics

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| ERROR_INVALID_PARAMETER (AV quarantine) | ✅ `makeContent` | ✅ `describe_usvfs_error` | ✅ |
| ERROR_ACCESS_DENIED (AV blocking) | ✅ `makeContent` | ✅ `describe_usvfs_error` | ✅ |
| ERROR_FILE_NOT_FOUND (exe missing) | ✅ `makeContent` | ✅ `describe_usvfs_error` | ✅ |
| ERROR_DIRECTORY (bad cwd) | ✅ `makeContent` | ✅ `describe_usvfs_error` | ✅ |
| ERROR_ELEVATION_REQUIRED (admin restart) | ✅ `confirmRestartAsAdmin` + `helper.exe` | ⚠️ error mapped, no restart flow | ⚠️ |
| makeDetails (owner, ACL, DLL presence) | ✅ | ❌ | ❌ |
| Blacklist warning dialog | ✅ `confirmBlacklisted` | ❌ | ❌ |
| Crash dump type selection | ✅ `CrashDumpsType` | ✅ `CrashDumpsType` enum | ✅ |
| USVFS child crash capture | ✅ `usvfsCreateMiniDump` | ✅ SEH + usvfs integration | ✅ |
| Crash dump pruning | ✅ `cycleDiagnostics` | ⚠️ setting exists, not wired | ⚠️ |
| USVFS log worker thread | ✅ `LogWorker` (QThread) | ✅ `std::thread` | ✅ |
| USVFS log file output | ✅ `logs/usvfs-<ts>.log` | ✅ `logs/usvfs-<ts>.log` | ✅ |
| USVFS log viewer | ✅ MO2 log panel | ❌ | ❌ |
| EventLog service check | ✅ | ❌ | ❌ |
| Sanity checks on startup | ✅ `sanityChecks()` | ❌ | ❌ |

## 4. Settings & Configuration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Executables blacklist | ✅ `Settings::executablesBlacklist` | ⚠️ data model only | ⚠️ |
| Skip file suffixes | ✅ `Settings::skipFileSuffixes` | ⚠️ data model only | ⚠️ |
| Skip directories | ✅ `Settings::skipDirectories` | ⚠️ data model only | ⚠️ |
| Force load libraries | ✅ `ExecutableForcedLoadSetting` | ⚠️ data model only | ⚠️ |
| USVFS log level | ✅ `Settings::logLevel` | ⚠️ data model only | ⚠️ |
| USVFS crash dump type | ✅ `Settings::coreDumpType` | ⚠️ data model only | ⚠️ |
| USVFS spawn delay | ✅ `Settings::spawnDelay` | ⚠️ data model only | ⚠️ |
| Geometry persistence | ✅ `GeometrySettings` (window, splitter, toolbar) | ❌ | ❌ |
| Widget state persistence | ✅ `WidgetSettings` (tree expand, combo, tab index) | ❌ | ❌ |
| Color settings (conflict coloring) | ✅ `ColorSettings` (8+ color options) | ❌ | ❌ |
| Plugin blacklist | ✅ `Settings::blacklisted` | ❌ | ❌ |
| Network settings (proxy, offline mode) | ✅ `NetworkSettings` | ❌ | ❌ |
| Splash screen | ✅ `Settings::useSplash` | ❌ | ❌ |
| Prerelease updates toggle | ✅ `Settings::usePrereleases` | ❌ | ❌ |
| Low-priority extraction | ✅ | 🚀 `extraction_low_priority` | 🚀 |
| Full UI mode (tabs vs popups) | ❌ | 🚀 `full_ui_mode` setting | 🚀 |

## 5. Executable Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Custom executables list | ✅ `ExecutablesList` (CRUD) | ✅ `Executables::Entry` + `ExecControlsBar` | ✅ |
| Per-executable arguments | ✅ `Executable::arguments` | ✅ `Executables::Entry::arguments` | ✅ |
| Per-executable working directory | ✅ `Executable::workingDirectory` | ✅ `Executables::Entry::start_in` | ✅ |
| Per-executable Steam App ID | ✅ `Executable::steamAppID` | ❌ | ❌ |
| Per-executable custom overwrite | ✅ `Executable::customOverwrites` | ❌ | ❌ |
| Per-executable forced libraries | ✅ `Executable::forcedLibraries` | ❌ | ❌ |
| Per-executable environment variables | ❌ | 🚀 `Executables::Entry::environment` (KEY=VALUE) | 🚀 |
| Per-executable output-to-mod routing | ❌ | 🚀 `Executables::Entry::output_mod` | 🚀 |
| Toolbar pinning | ✅ `ShowInToolbar` flag | ✅ `add_shortcut_to_toolbar()` | ✅ |
| Desktop shortcut creation | ❌ | 🚀 `add_shortcut_to_desktop()` (.desktop file) | 🚀 |
| Executable ordering (up/down) | ✅ `EditExecutablesDialog` | ✅ up/down buttons + drag-drop | ✅ |
| Clone executable | ✅ `EditExecutablesDialog::clone()` | ✅ `on_clone_selected()` | ✅ |
| JAR binary detection | ✅ `setJarBinary` + `findJavaInstallation` | ❌ | ❌ |
| Icon extraction (wrestool/QFileIconProvider) | ❌ | 🚀 `extractExeIcon()` | 🚀 |
| Executable editor widget | ✅ `EditExecutablesDialog` | ✅ `Executables::ContentWidget` (mode-agnostic) | ✅ |

## 6. Mod Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod priority ordering | ✅ `profile.getActiveMods` | ✅ `mod_model` priority | ✅ |
| Mod enable/disable | ✅ `ModInfo::enabled` | ✅ `mod_model` | ✅ |
| Overwrite directory | ✅ `Settings::paths().overwrite` | ✅ | ✅ |
| Custom overwrite target | ✅ `customOverwrite` param | ✅ | ✅ |
| Mod data-to-game mapping | ✅ `getModMappings` | ✅ `knowledge_` keys | ✅ |
| Mod metadata | ✅ `ModInfo` | ✅ `mod_meta` + `category_set_registry` | ✅ |
| Core category sets | ❌ | 🚀 `CategorySetRegistry` + plugin hook | 🚀 |
| Plugin-contributed categories | ❌ | 🚀 `IPluginCategoryFactory` | 🚀 |
| Mod file tree / conflict display | ✅ `DirectoryEntry` | ✅ `file_tree` + `conflict_engine` | ✅ |
| BSA/archive extraction | ✅ `BSAExtractor` | ✅ `archive_extractor` | ✅ |
| Case-insensitive mod matching | ✅ USVFS handles it | 🚀 `PathResolver` | 🚀 |
| Mod cache | ✅ | ✅ `mod_cache` (SQLite) | ✅ |
| Mod scanner | ✅ | ✅ `mod_scanner` | ✅ |
| Mod renaming | ✅ `ModList::renameMod` | ✅ `rename_mod_inline()` | ✅ |
| Mod notes | ✅ `ModInfo::notes()` | ✅ `NotesTab` (comments + HTML notes + color) | ✅ |
| Mod comments | ✅ `ModInfo::comments()` | ✅ `NotesTab::comments` | ✅ |
| Mod color coding | ✅ `ModInfo::color()` | ✅ `NotesTab` (Set/Reset color) | ✅ |
| Mod author/uploader metadata | ✅ `ModInfo::author()`, `uploader()` | ❌ | ❌ |
| Mod description | ✅ `ModInfo::getDescription()` | ❌ | ❌ |
| Mod creation time | ✅ `ModInfo::creationTime()` | ✅ `mod_scanner` (install_time, changed_time) | ✅ |
| Mod internal name | ✅ `ModInfo::internalName()` | ❌ | ❌ |
| Mod validated flag | ✅ `ModInfo::markValidated` | ✅ `mark_validated()` | ✅ |
| Mod repository tracking | ✅ `ModInfo::repository()` | ✅ `source_type` / `source_id` | ✅ |
| Plugin settings per mod | ✅ `ModInfoRegular::pluginSetting` | ❌ | ❌ |
| Nexus file IDs tracking | ✅ `ModInfoRegular::installedFiles` | ❌ | ❌ |
| Mod tags (deprecated/note/warning/incompatible) | ❌ | 🚀 `ModTag` system with messages | 🚀 |
| Visual nesting (parent_id, indent, fold) | ❌ | 🚀 `mod_list_model` nesting | 🚀 |
| Vendor icons (per-source badges) | ❌ | 🚀 `mod_list_model` (nexusmods, loverslab, steam, moddb) | 🚀 |
| MERGED pseudo-mod | ❌ | 🚀 `kMergedModId` constant | 🚀 |
| Game-native mod band | ❌ | 🚀 `native_band_first/last` | 🚀 |

## 7. Mod Categories

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Category tree system | ✅ `Categories` (hierarchical) | ✅ `CategoryFactory` | ✅ |
| Nexus category mapping | ✅ `NexusCategory` + `resolveNexusID` | ✅ `NexusCat` mapping | ✅ |
| Category import/export | ✅ `CategoryImportDialog` | ❌ | ❌ |
| Multi-category assignment | ✅ `ModInfo::setCategories` | ❌ | ❌ |
| Primary category | ✅ `ModInfo::primaryCategory` | ❌ | ❌ |
| Special filter categories | ✅ `SpecialCategories` (Checked, UpdateAvailable, Conflict, etc.) | ❌ | ❌ |
| Category CRUD editor | ✅ | ✅ `CategoriesDialog` (editable table, full CRUD) | ✅ |
| Category filter panel | ✅ | ✅ `CategoryFilterPanel` (checkable tree, OR semantics) | ✅ |

## 8. Mod Conflict Detection

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Loose file conflicts | ✅ `EConflictFlag::OVERWRITE` | ✅ `conflict_engine` | ✅ |
| Archive vs loose conflicts | ✅ `FLAG_ARCHIVE_LOOSE_CONFLICT_*` | ❌ | ❌ |
| Archive vs archive conflicts | ✅ `FLAG_ARCHIVE_CONFLICT_*` | ❌ | ❌ |
| Overwrite folder conflicts | ✅ `FLAG_OVERWRITE_CONFLICT` | ❌ | ❌ |
| Conflict dialog (general) | ✅ `GeneralConflictsTab` with counters | ❌ | ❌ |
| Conflict dialog (advanced) | ✅ `AdvancedConflictsTab` tree view | ❌ | ❌ |
| Conflict context menu | ✅ open/run hooked/preview/explore/hide/goto | ❌ | ❌ |
| Conflict highlighting | ✅ `EHighlight` (INVALID, CENTER, IMPORTANT, PLUGIN) | ❌ | ❌ |
| Per-mod conflict stats | ❌ | 🚀 `conflict_engine` (wins/losses/total) | 🚀 |
| Blake2b fingerprint cache | ❌ | 🚀 `ConflictIndex` (SQLite + blake2b) | 🚀 |
| Image diff (conflict comparison) | ❌ | 🚀 `conflicts_tab` image_diff_requested | 🚀 |

## 9. Mod Content Analysis

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod data content types | ✅ `ModDataContentHolder` | ✅ `ModContentId` enum | ✅ |
| Mod flags (INVALID, BACKUP, SEPARATOR, etc.) | ✅ `EFlag` | ✅ `ModState` + mod_meta flags | ✅ |
| Mod content icons | ✅ `ModContentIconDelegate` | ❌ | ❌ |
| Mod conflict icons | ✅ `ModConflictIconDelegate` | ✅ `FlagsDelegate` (per-icon tooltips) | ✅ |
| Mod flag icons | ✅ `ModFlagIconDelegate` | ✅ `FlagsDelegate` (wrap + tooltips) | ✅ |
| Mod version delegate (color-coded) | ✅ `ModListVersionDelegate` | ❌ | ❌ |
| INI tweaks detection | ✅ `ModInfo::getIniTweaks()` | ❌ | ❌ |
| Archive listing per mod | ✅ `ModInfo::archives()` | ❌ | ❌ |

## 10. Mod Info Dialog

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| File tree tab | ✅ `ModInfoDialogFileTree` | ✅ `filetree_tab` | ✅ |
| ESP/plugin tab | ✅ `ModInfoDialogEsps` | ✅ `esps_tab` | ✅ |
| Nexus tab (embedded page) | ✅ `ModInfoDialogNexus` (endorse/track) | ✅ `source_tab` | ✅ |
| Images tab (gallery, DDS) | ✅ `ModInfoDialogImages` (380 lines) | ✅ `images_tab` | ✅ |
| Text files tab | ✅ `ModInfoDialogTextFiles` | ✅ `text_files_tab` | ✅ |
| INI files tab | ✅ `IniFilesTab` | ✅ `config_files_tab` | ✅ |
| Categories tab | ✅ `ModInfoDialogCategories` | ✅ `categories_tab` | ✅ |
| Conflicts tab | ✅ `ModInfoDialogConflicts` | ✅ `conflicts_tab` | ✅ |
| Notes tab (comments + notes + color) | ✅ `ModInfoNotesTab` | ✅ `notes_tab` | ✅ |
| Generic files tab | ❌ | 🚀 `generic_files_tab` | 🚀 |
| Tab reordering | ✅ `onTabMoved` + `saveTabOrder` | ❌ | ❌ |
| Tab color coding (data presence) | ✅ `setTabsColors` | ❌ | ❌ |
| Mod navigation (prev/next) | ✅ `onPreviousMod`, `onNextMod` | ❌ | ❌ |

## 11. Version & Update Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Version checking (installed vs newest) | ✅ `ModInfo::updateAvailable` | ❌ | ❌ |
| Newest version tracking | ✅ `ModInfo::newestVersion()` | ❌ | ❌ |
| Ignored version | ✅ `ModInfo::ignoredVersion()` | ❌ | ❌ |
| Downgrade detection | ✅ `ModInfo::downgradeAvailable()` | ❌ | ❌ |
| Batch update check | ✅ `checkAllForUpdate` | ❌ | ❌ |
| Check update after install | ✅ `Settings::checkUpdateAfterInstallation` | ❌ | ❌ |

## 12. Plugin Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Plugin list | ✅ `plugins.txt` | ✅ `plugin_database` | ✅ |
| Load order sorting (LOOT) | ✅ `LOOT` | ✅ `gmm_lootcli` | ✅ |
| Plugin enable/disable | ✅ | ✅ | ✅ |
| ESP header parsing | ✅ `ESPInfo` | ✅ `esp_header` | ✅ |
| Plugin diagnostics | ✅ | ✅ `diagnose_registry` | ✅ |
| Plugin file mapper | ✅ `IPluginFileMapper` | ✅ `file_mapper_registry` | ✅ |
| Plugin save parser | ✅ | ✅ `save_parser_registry` | ✅ |
| Plugin requirements check | ✅ | ✅ `requirements_registry` | ✅ |
| Plugin order encoding | ✅ | ✅ `order_encoding_registry` | ✅ |
| Master/Light/Medium/Blueprint flags | ✅ `isMasterFlagged`, `isLightFlagged`, etc. | ❌ | ❌ |
| Missing masters detection | ✅ `testMasters` + `missingMasters` | ❌ | ❌ |
| Plugin lock (pin at position) | ✅ `isESPLocked`, `lockESPIndex` | ✅ `set_locked()` | ✅ |
| Plugin relationship fix | ✅ `fixPluginRelationships` | ❌ | ❌ |
| Plugin priority shift | ✅ `shiftPluginsPriority` | ❌ | ❌ |
| Plugin send to priority | ✅ `sendToPriority` | ❌ | ❌ |
| Enable/disable all plugins | ✅ `setEnabledAll` | ✅ `set_all_enabled()` | ✅ |
| Plugin index generation (FE/FD) | ✅ `generatePluginIndexes` | ✅ `generate_mod_indexes()` | ✅ |
| LOOT messages per plugin | ✅ `Plugin::messages` | ❌ | ❌ |
| LOOT dirty info (ITM, deleted refs) | ✅ `Dirty` struct | ❌ | ❌ |
| LOOT incompatibilities | ✅ `Plugin::incompatibilities` | ❌ | ❌ |
| LOOT stats (time, version) | ✅ `Stats` | ❌ | ❌ |
| Plugin author/description display | ✅ `pluginlist.h` | ❌ | ❌ |
| Plugin FormVersion/HeaderVersion | ✅ `formVersion`, `headerVersion` | ✅ `esp_header` | ✅ |
| Plugin archive loading detection | ✅ `loadsArchive` | ❌ | ❌ |
| Drag-and-drop plugin reorder | ✅ `dropMimeData` | ❌ | ❌ |
| Plugin foreground coloring (LOOT-based) | ✅ `foregroundData()` | ❌ | ❌ |
| Plugin tooltip data (LOOT messages) | ✅ `tooltipData()` | ❌ | ❌ |
| Plugin highlight from mod selection | ✅ `highlightPlugins()` | ✅ `set_contained_plugins()` / `set_master_plugins()` | ✅ |
| Transitive master enable/disable | ❌ | 🚀 `set_enabled()` enables/disables masters | 🚀 |
| Band reassertion (native+CC invariant) | ❌ | 🚀 `reassert_band()` | 🚀 |
| Locked order application | ❌ | 🚀 `apply_locked_order()` | 🚀 |
| Plugin type classification | ✅ | ✅ Regular/Master/Light/Medium enum | ✅ |
| Plugin counter (by type) | ✅ `ModCounters` | ✅ `plugins_tab` active/total breakdown | ✅ |

## 13. LOOT Integration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| LOOT sort execution | ✅ `Loot::sort()` | ✅ `Sorter::Loot` | ✅ |
| LOOT report generation | ✅ `Loot::createReport()` | ❌ | ❌ |
| LOOT report viewer (markdown + web) | ✅ `LootDialog` + `MarkdownDocument` | ❌ | ❌ |
| LOOT progress display | ✅ `LootDialog::setProgress()` | ❌ | ❌ |
| LOOT dirty info details | ✅ `Dirty` (CRC, ITM, deleted refs, navmesh, utility) | ❌ | ❌ |
| LOOT incompatibilities details | ✅ `File` (name + displayName) | ❌ | ❌ |
| LOOT missing masters | ✅ `Plugin::missingMasters` | ❌ | ❌ |
| Masterlist manager (GitHub walk-down) | ✅ | 🚀 `MasterlistManager` (branch walk, 24h TTL, offline fallback) | 🚀 |

## 14. Profile Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Profile creation | ✅ `Profile` | ✅ `profile_creation` | ✅ |
| Profile switching | ✅ | ✅ `profile_switching` | ✅ |
| Local saves per profile | ✅ `localSavesEnabled` | ✅ `local_saves` | ✅ |
| Mod order persistence | ✅ | ✅ | ✅ |
| Plugin order persistence | ✅ | ✅ | ✅ |
| Delayed file writer | ✅ `DelayedFileWriter` | ✅ `delayed_file_writer` | ✅ |
| Safe write file | ✅ | ✅ `safe_write_file` | ✅ |
| Local INI settings | ✅ `Profile::localSettingsEnabled` | ❌ | ❌ |
| Profile INI tweaks | ✅ `Profile::getProfileTweaks` | ❌ | ❌ |
| Archive invalidation toggle | ✅ `Profile::invalidationActive` | ❌ | ❌ |
| Profile locking (plugin order) | ✅ `Profile::getLockedOrderFileName` | ❌ | ❌ |
| Profile transfer saves | ✅ `TransferSavesDialog` | ❌ | ❌ |
| Profile rename | ✅ `Profile::rename` | ❌ | ❌ |
| Profile copy | ✅ `Profile::createPtrFrom` | ❌ | ❌ |
| Profile forced libraries | ✅ `Profile::determineForcedLibraries` | ❌ | ❌ |
| Profile switch result + callbacks | ❌ | 🚀 `ProfileSwitchResult` + `ProfileSwitchCallbacks` | 🚀 |
| Profile switch EventBus emission | ❌ | 🚀 `kProfileChanged` event | 🚀 |
| Profile bar (combo + folder shortcuts) | ❌ | 🚀 `ProfileBar` (12 FolderKind types, export/import) | 🚀 |

## 15. Download Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Download state machine | ✅ `DownloadState` (14 states) | ✅ `curl_download` states | ✅ |
| Pause/resume downloads | ✅ `pauseDownload`, `resumeDownload` | ✅ `downloads_tab` pause/resume | ✅ |
| Cancel downloads | ✅ `cancelDownload` | ✅ abort callback | ✅ |
| Download speed tracking | ✅ `downloadSpeed` rolling average | ❌ | ❌ |
| MD5 lookup | ✅ `queryInfoMd5` | ❌ | ❌ |
| Download meta files (sidecar) | ✅ `createMetaFile` | ❌ | ❌ |
| Hidden downloads | ✅ `isHidden`, `restoreDownload` | ✅ `downloads_tab` show hidden checkbox | ✅ |
| Automatic retry (3x) | ✅ `AUTOMATIC_RETRIES` | ❌ | ❌ |
| Pending download queue | ✅ `PendingDownload` | ✅ `queue_controller` deferred queue | ✅ |
| Multi-URL fallback | ✅ `m_Urls`, `m_CurrentUrl` | ❌ | ❌ |
| Hide after install | ✅ `hideDownloadsAfterInstallation` | ❌ | ❌ |
| Download notifications | ✅ `showDownloadNotifications` | ❌ | ❌ |
| Compact downloads view | ✅ `compactDownloads` | ✅ `downloads_tab` compact/standard rows | ✅ |
| Drag-and-drop import | ❌ | 🚀 `downloads_tab` drop archive to add | 🚀 |
| Directory watcher (auto-detect) | ❌ | 🚀 `downloads_tab` QFileSystemWatcher | 🚀 |
| Manifest serialization (JSON) | ❌ | 🚀 `downloads_tab` serialize/deserialize | 🚀 |
| Content-Disposition filename parsing | ❌ | 🚀 RFC 5987 `filename*` + `percent_decode` | 🚀 |
| HTTP Range resume | ❌ | 🚀 `curl_download` Range header | 🚀 |

## 16. Nexus Integration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| NXM handler registration | ✅ `registerAsNXMHandler` | ✅ `nxm_router` + `nxm_ipc` | ✅ |
| Nexus account / auth | ✅ | ✅ `nexus_account` + `nexus_auth` | ✅ |
| Nexus HTTP API | ✅ | ✅ `nexus_http` + `nexus_servers` | ✅ |
| Endorsement system | ✅ `ModInfo::endorse()` | ❌ | ❌ |
| Tracking system | ✅ `ModInfo::track()` | ❌ | ❌ |
| "Never endorse" option | ✅ `ModInfo::setNeverEndorse` | ❌ | ❌ |
| Nexus description (HTML) | ✅ `ModInfo::getNexusDescription` | ❌ | ❌ |
| Nexus category ID tracking | ✅ `ModInfo::getNexusCategory` | ❌ | ❌ |
| Nexus update timestamps | ✅ `getLastNexusUpdate/Query` | ❌ | ❌ |
| OAuth login flow | ✅ `NexusOAuthLogin` | ❌ | ❌ |
| Nexus user account info | ✅ `ApiUserAccount` | ✅ `NexusUserInfo` | ✅ |
| Rate limit tracking | ✅ | ✅ `RateLimitInfo` (hourly + daily) | ✅ |
| Download mirror registry | ❌ | 🚀 `NexusServers` (speed samples, preferred ordering) | 🚀 |
| Nexus API key manual entry | ✅ | ✅ `NexusManualKeyDialog` (Open Browser/Paste/Clear) | ✅ |
| Tier-derived queue defaults | ❌ | 🚀 `nexus_queue_default_for()` (Regular=queue, Premium=parallel) | 🚀 |

## 17. Source Providers

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Nexus Mods integration | ✅ | ✅ `NexusProvider` | ✅ |
| Steam Workshop | ✅ | ✅ `SteamWorkshopProvider` | ✅ |
| LOVERS LAB | ❌ | 🚀 `LoversLabProvider` | 🚀 |
| Download manager | ✅ `DownloadManager` | ✅ `curl_download` | ✅ |
| Remote cache | ✅ | ✅ `RemoteCache` (6-layer fetch chain) | ✅ |
| Steam Workshop client (Web API) | ❌ | 🚀 `WorkshopClient` (SQLite cache, dead ID tracking) | 🚀 |
| LoversLab session-cookie auth | ❌ | 🚀 `LoversLabAuth` (Cloudflare stripping) | 🚀 |
| Managed games tracking | ❌ | 🚀 `ManagedGames` (source_id, website_url, nexus_domain) | 🚀 |

## 18. Mod List Features

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Multi-criteria sorting | ✅ `ModListSortProxy` | ✅ `mod_table_view` + sort proxy | ✅ |
| Category/content/special filtering | ✅ `FilterList` | ✅ `CategoryFilterPanel` + `mod_filter_bar` | ✅ |
| Grouping (by separator, category, Nexus ID) | ✅ `QtGroupingProxy` | ✅ visual nesting (parent_id, indent, fold) | ✅ |
| Drag-and-drop reorder | ✅ `ModList::dropMimeData` | ✅ `mod_table_view` drop support | ✅ |
| Scroll markers | ✅ `ViewMarkingScrollBar` | ✅ `ModMarkingScrollBar` (separator marks) | ✅ |
| CSV export | ✅ `exportModListCSV` | ❌ | ❌ |
| Bulk enable/disable | ✅ `setActive(indices)` | ✅ `toggle_selected_mods()` | ✅ |
| Priority shift (bulk) | ✅ `shiftModsPriority` | ✅ `priority_move_selected()` | ✅ |
| Send to top/bottom/priority | ✅ `sendModsToTop/Bottom/Priority` | ✅ `send_to_highest/lowest_priority()` | ✅ |
| Send to separator | ✅ `sendModsToSeparator` | ✅ `move_to_separator()` | ✅ |
| Send to First/Last Conflict | ✅ `sendModsToFirstConflict/LastConflict` | ❌ | ❌ |
| Collapseable separators | ✅ `collapsibleSeparators` | ❌ | ❌ |
| Auto-collapse on hover | ✅ `autoCollapseOnHover` | ❌ | ❌ |
| Filter persistence | ✅ `saveFilters` | ❌ | ❌ |
| Filter AND/OR mode | ✅ `FilterAnd`/`FilterOr` | ❌ | ❌ |
| Column visibility toggle | ✅ `setColumnVisible()` | ✅ `column_toggle_header` | ✅ |
| Mod counter display | ✅ `ModCounters` (LCD) | ✅ `status_bar` (counts) | ✅ |
| Create separator | ✅ | ✅ `create_separator()` / `create_separator_named()` | ✅ |
| Create empty mod | ✅ `createEmptyMod` | ✅ `create_empty_mod()` | ✅ |
| Import archives | ✅ | ✅ `import_archives()` | ✅ |
| Export/import modlist | ✅ | ❌ | ❌ |
| Overwrite file drop-to-mod | ❌ | 🚀 `overwrite_files_dropped` signal | 🚀 |
| IndentDelegate (nesting) | ❌ | 🚀 `IndentDelegate` for Name column | 🚀 |
| FlagsDelegate with tooltips | ❌ | 🚀 per-emblem hover text | 🚀 |

## 19. Mod Context Menu

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Visit on Nexus | ✅ `visitOnNexus` | ✅ `source_visit_info` (Nexus/LoversLab) | ✅ |
| Visit web page | ✅ `visitWebPage` | ✅ source-aware context menu | ✅ |
| Reinstall mod | ✅ `reinstallMod` | ❌ | ❌ |
| Create backup | ✅ `createBackup` | ❌ | ❌ |
| Restore backup | ✅ `restoreBackup` | ❌ | ❌ |
| Restore hidden files | ✅ `restoreHiddenFiles` | ❌ | ❌ |
| Mark as converted | ✅ `markConverted` | ❌ | ❌ |
| Ignore missing data | ✅ `ignoreMissingData` | ✅ `mark_validated()` | ✅ |
| Ignore update | ✅ `setIgnoreUpdate` | ❌ | ❌ |
| Set color | ✅ `setColor`, `resetColor` | ✅ `NotesTab` Set/Reset color | ✅ |
| Open in Explorer | ✅ `openExplorer` | ✅ Ctrl+double-click | ✅ |
| Create empty mod | ✅ `createEmptyMod` | ✅ `create_empty_mod()` | ✅ |
| Create separator | ✅ `createSeparator` | ✅ `create_separator()` | ✅ |
| Overwrite: create mod from overwrite | ✅ `createModFromOverwrite` | ✅ `OverwriteController::createModFromOverwrite()` | ✅ |
| Overwrite: move to existing mod | ✅ `moveOverwriteContentToExistingMod` | ✅ `OverwriteController::moveContentToMod()` | ✅ |
| Overwrite: clear | ✅ `clearOverwrite` | ✅ `OverwriteController::clearOverwrite()` | ✅ |
| Set categories (batch) | ✅ `setCategories`, `setPrimaryCategory` | ✅ `add_category_menus()` (checkable + radio) | ✅ |
| Rename mod | ✅ `renameMod` | ✅ `rename_mod_inline()` | ✅ |
| Remove mod | ✅ | ✅ `remove_selected_mods()` | ✅ |
| Root override toggle | ✅ | ✅ `toggle_root_override()` | ✅ |

## 20. Plugin Context Menu

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Send plugins to priority | ✅ `PluginListContextMenu` | ❌ | ❌ |
| Set ESP lock (from context) | ✅ `setESPLock` | ✅ `lock_requested` signal | ✅ |
| Open origin explorer | ✅ `openOriginExplorer` | ❌ | ❌ |
| Open origin information | ✅ `openOriginInformation` | ❌ | ❌ |

## 21. Archive & Installation

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| FOMOD installer | ✅ `IPluginInstaller` | ✅ `FomodInstaller` plugin + engine | ✅ |
| FOMOD XML parsing | ✅ | ✅ `module_config` (GroupType, Dependency, PluginType) | ✅ |
| FOMOD condition tester | ✅ | ✅ `condition_tester` (file/flag/game/composite) | ✅ |
| FOMOD C# script detection | ✅ | ✅ `hasCSharpScript()` | ✅ |
| FOMOD view model (step nav) | ✅ | ✅ `FomodViewModel` (step forward/back, flag map) | ✅ |
| FOMOD file installer | ✅ | ✅ `FomodFileInstaller::apply()` | ✅ |
| Archive password support | ✅ `queryPassword` | ❌ | ❌ |
| Installation merge/replace | ✅ `merged`, `replaced` | ❌ | ❌ |
| Backup on install | ✅ `keepBackupOnInstall` | ❌ | ❌ |
| Installation result tracking | ✅ `InstallationResult` | ❌ | ❌ |
| Staging layout normalization | ❌ | 🚀 `analyze_staging_layout()` + `normalize_staging_root()` | 🚀 |
| BSA/BA2 archive listing | ❌ | 🚀 `DataArchive` (libarchive-backed) | 🚀 |
| Install name dialog (smart candidates) | ❌ | 🚀 `InstallNameDialog` (editable combobox) | 🚀 |
| Install progress dialog (modeless) | ❌ | 🚀 `InstallProgressDialog` (300ms show delay) | 🚀 |

## 22. Deploy System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Symlink strategy | ❌ | 🚀 `SymlinkStrategy` (CI target resolution) | 🚀 |
| Direct deploy strategy | ❌ | 🚀 `DirectDeployStrategy` (ledger + backup) | 🚀 |
| Hardlink strategy | ❌ | 🚀 `HardlinkStrategy` | 🚀 |
| Junction strategy (Windows) | ❌ | 🚀 `JunctionStrategy` | 🚀 |
| OverlayFS deploy strategy | ❌ | 🚀 `OverlayFsDeployStrategy` (O(1) reorder) | 🚀 |
| FUSE VFS strategy | ❌ | 🚀 `VfsStrategy` (FUSE + file_map) | 🚀 |
| Deploy ledger (incremental tracking) | ❌ | 🚀 `DeployLedger` (diff for priority changes) | 🚀 |
| Parallel deploy (thread pool) | ❌ | 🚀 `deploy_all_enabled_mods_parallel()` | 🚀 |
| Root override ([General] rootOverride) | ❌ | 🚀 `RootOverride` + `classify_registry_path()` | 🚀 |
| Case-insensitive deploy aliases | ❌ | 🚀 `add_case_insensitive_aliases()` | 🚀 |
| Deploy backup + restore | ❌ | 🚀 `remove_deployed_files()` restores originals | 🚀 |
| Binary detection (PE/ELF/SH) | ❌ | 🚀 `is_executable_binary()` | 🚀 |

## 23. Overwrite System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Move overwrite to mod | ✅ | ✅ `move_overwrite_to_mod()` | ✅ |
| Sync overwrite file | ✅ | ✅ `sync_overwrite_file()` | ✅ |
| Clear overwrite (trash) | ✅ | ✅ `clear_overwrite()` | ✅ |
| CI directory merge (overlay captures) | ❌ | 🚀 `normalize_overwrite_casing()` | 🚀 |
| Overwrite sync plan (apply/preview) | ❌ | 🚀 `apply_sync_plan()` | 🚀 |
| Overwrite info dialog (file browser) | ✅ `OverwriteInfoDialog` | ✅ `OverwriteInfoDialog` (QFileSystemModel, context menu) | ✅ |
| Query overwrite dialog (merge/replace) | ✅ `QueryOverwriteDialog` | ✅ `QueryOverwriteDialog` (thread-safe `ask_overwrite()`) | ✅ |
| Sync overwrite dialog (selective) | ✅ `SyncOverwriteDialog` | ✅ `SyncOverwriteDialog` (per-file combo, game-origin) | ✅ |
| Move to mod dialog | ❌ | 🚀 `MoveToModDialog` (destination picker) | 🚀 |

## 24. Save Game System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Save game model | ✅ | ✅ `SaveGame` (path, pc_name, level, location, plugins) | ✅ |
| Skyrim SE/LE save parsing | ✅ | ✅ `parse_skyrim_save()` / `parse_skyrimse_save()` | ✅ |
| Save scanning | ✅ | ✅ `scan_saves()` | ✅ |
| Save missing assets resolver | ✅ | ✅ `find_save_missing_assets()` | ✅ |
| Local saves | ✅ | ✅ `local_saves` | ✅ |
| Pluggable save parser (per-game) | ❌ | 🚀 `SaveParserRegistry` | 🚀 |
| Script extender file detection | ❌ | 🚀 `has_script_extender_file()` | 🚀 |
| Save screenshot extraction (RGBA) | ❌ | 🚀 `SaveGame::screenshot` | 🚀 |
| Save game list (QTreeWidget) | ✅ `SavesTab` | ✅ `saves_tab` | ✅ |
| Save game hover info popup | ✅ `GamebryoSaveGameInfoWidget` | ✅ `saves_tab` hover info | ✅ |
| Save game background scan | ✅ | ✅ `SavesScanWorker` | ✅ |
| Save game delete | ✅ `SavesTab::deleteSavegame()` | ✅ `on_delete_key()` | ✅ |
| Save game context menu | ✅ `SavesTab::onContextMenu()` | ✅ `saves_tab` context menu | ✅ |
| Save game open in explorer | ✅ | ❌ | ❌ |
| Save game fix missing assets | ✅ `SavesTab::fixMods()` | ❌ | ❌ |
| Transfer saves dialog | ✅ `TransferSavesDialog` | ❌ | ❌ |

## 25. Game Detection & Knowledge

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Game detection (Steam library) | ✅ | ✅ `detect_steam_games()` | ✅ |
| Per-game knowledge (key-value) | ✅ | ✅ `GameKnowledge` | ✅ |
| Game capabilities (tab display) | ❌ | 🚀 `GameCapabilities` (CapabilityInfo, visible_tabs_for) | 🚀 |
| Game feature registry (MO2 IGameFeatures port) | ❌ | 🚀 `GameFeatureRegistry` (priority + replace, typed resolve_feature) | 🚀 |
| ModDataChecker feature | ❌ | 🚀 `ModDataContentFeature` (standard Bethesda catalog) | 🚀 |
| ScriptExtender feature | ❌ | 🚀 `ScriptExtenderFeature` (binary_name, loader_name) | 🚀 |
| DataArchives feature | ❌ | 🚀 `DataArchivesFeature` (vanilla archive list) | 🚀 |
| AnimationParser feature | ❌ | 🚀 `AnimationParserFeature` (frames, layers, RGBA pixels) | 🚀 |
| UnmanagedMods feature (DLC/CC) | ❌ | 🚀 `UnmanagedModsFeature` | 🚀 |
| BSAInvalidation feature | ❌ | 🚀 `BSAInvalidationFeature` | 🚀 |
| Game icons (download-on-demand) | ❌ | 🚀 `GameIconCache` (async, placeholder avatars) | 🚀 |
| Multi-game detection | ❌ | 🚀 `detect_steam_games_multi()` | 🚀 |
| VDF/ACF parsing | ❌ | 🚀 `parse_library_folders()` + `parse_acf_value()` | 🚀 |

## 26. Pipeline System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Pipeline with ordered stages | ❌ | 🚀 `Pipeline` + `PipelineContext` | 🚀 |
| Fetch stage (download) | ❌ | 🚀 `FetchStage` | 🚀 |
| Extract stage (archive) | ❌ | 🚀 `ExtractStage` (low_priority) | 🚀 |
| FOMOD stage (wizard) | ❌ | 🚀 `FomodStage` | 🚀 |
| Install stage (deploy) | ❌ | 🚀 `InstallStage` | 🚀 |
| Deploy stage (symlink/overlay) | ❌ | 🚀 `DeployStage` | 🚀 |
| Resolve stage (path resolution) | ❌ | 🚀 `ResolveStage` | 🚀 |
| Sync stage (overwrite) | ❌ | 🚀 `SyncStage` | 🚀 |
| Launch stage (game execution) | ❌ | 🚀 `LaunchStage` | 🚀 |
| Plugin claim stage | ❌ | 🚀 `PluginClaimStage` | 🚀 |
| Stage registry + hook registry | ❌ | 🚀 `StageRegistry` + `HookRegistry` | 🚀 |
| Overwrite decision (Merge/Replace/Rename/Cancel) | ❌ | 🚀 `OverwriteAction` enum | 🚀 |
| FOMOD decision (accept/manual/choices_json) | ❌ | 🚀 `FomodDecision` | 🚀 |
| Trace recorder (pipeline workflow) | ❌ | 🚀 `TraceRecorder` (flow_id, stages, durations) | 🚀 |
| Pipeline visualization (2D canvas) | ❌ | 🚀 `PipelineContentWidget` (stage cards, arrows, status) | 🚀 |
| Pipeline worker (background) | ❌ | 🚀 `PipelineWorker` | 🚀 |

## 27. Plugin Host System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| C ABI plugin loading (dlopen) | ❌ | 🚀 `PluginLoader` (load_plugin, load_directory) | 🚀 |
| v2 ABI registration | ❌ | 🚀 `gmm_register_v2()` | 🚀 |
| Python plugin loader | ❌ | 🚀 `PythonLoader` | 🚀 |
| Tool registry (IPluginTool) | ❌ | 🚀 `ToolRegistry` (tool_id, kind, fn) | 🚀 |
| Diagnostics registry | ❌ | 🚀 `DiagnosticsRegistry` + `DiagnoseRegistry` | 🚀 |
| Deploy strategy registry | ❌ | 🚀 `DeployStrategyRegistry` (deploy/remove) | 🚀 |
| Hook registry (behavior injection) | ❌ | 🚀 `HookRegistry` (tag-based, priority-ordered) | 🚀 |
| Save parser registry | ❌ | 🚀 `SaveParserRegistry` | 🚀 |
| File mapper registry | ❌ | 🚀 `FileMapperRegistry` | 🚀 |
| Order encoding registry | ❌ | 🚀 `OrderEncodingRegistry` | 🚀 |
| Requirements registry | ❌ | 🚀 `RequirementsRegistry` | 🚀 |
| Plugin settings registry | ❌ | 🚀 `PluginSettingsRegistry` | 🚀 |

## 28. Sort System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Sort provider / registry | ❌ | 🚀 `SortProvider` + `SortRegistry` | 🚀 |
| C ABI sort provider | ❌ | 🚀 `AbiSortProvider` | 🚀 |
| LOOT sorter | ❌ | 🚀 `Sorter::Loot` (run_sort with progress) | 🚀 |
| Masterlist manager | ❌ | 🚀 `MasterlistManager` (GitHub branch walk-down, 24h TTL) | 🚀 |

## 29. Instance Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Instance manager | ✅ `InstanceManager` | ✅ `Instance` (TOML, per-folder overrides) | ✅ |
| Create instance dialog | ✅ `CreateInstanceDialog` | ✅ `GameSelectionWidget` (game cards + filter) | ✅ |
| Instance switcher | ✅ | ✅ `InstanceSwitcherDialog` + `InstanceSwitcherContentWidget` | ✅ |
| Instance TOML persistence | ❌ | 🚀 `parse_instance_toml()` + JSON-to-TOML repair | 🚀 |
| Instance scan + last-used | ❌ | 🚀 `scan_instances()` + `read/write_last_instance()` | 🚀 |
| Game icons (download-on-demand) | ❌ | 🚀 `GameIcons` (ensure_icon_cached) | 🚀 |
| Masterlist fetch (GitHub cache) | ❌ | 🚀 `MasterlistFetch` (branch walk-down) | 🚀 |
| Instance statistics dialog | ❌ | 🚀 `StatsContentWidget` (sizes + open in explorer) | 🚀 |
| Instance options panel | ❌ | 🚀 `instance_options_panel` | 🚀 |

## 30. UI Layer

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod list view | ✅ | ✅ `mod_table_view` | ✅ |
| Plugin list view | ✅ | ✅ `plugins_tab` | ✅ |
| Mod info dialog | ✅ `ModInfoDialog` | ✅ `mod_info_dialog` (15 tabs) | ✅ |
| Settings dialog | ✅ `SettingsDialog` | ✅ `settings_content_widget` | ✅ |
| Toolbar | ✅ | ✅ `main_toolbar` | ✅ |
| Game lock overlay | ✅ `UILocker` | 🚀 `game_lock_overlay` | 🚀 |
| Process Tree View | ❌ | 🚀 `process_tree_checkbox` | 🚀 |
| FOMOD wizard UI | ✅ | ✅ `fomod_wizard_dialog` | ✅ |
| FOMOD image viewer | ✅ | ✅ `fomod_image_viewer` | ✅ |
| Profile bar (combo + folders) | ❌ | 🚀 `ProfileBar` (12 FolderKind, export/import) | 🚀 |
| Profile manager dialog | ✅ | ✅ `profile_manager_dialog` | ✅ |
| Profile settings widget | ❌ | 🚀 `profile_settings_widget` | 🚀 |
| Console panel | ❌ | 🚀 `console_panel` | 🚀 |
| Debug window (Konami code) | ❌ | 🚀 `debug_window` (easter egg) | 🚀 |
| Preview system (images/text/video) | ✅ | ✅ `preview_registry` + `preview_widget` | ✅ |
| File viewer (image, video, 3D scene) | ❌ | 🚀 `ImageViewer`, `VideoViewer`, `SceneViewer` | 🚀 |
| Plugin-provided preview | ✅ | ✅ `preview_window` (v2 IPluginPreview) | 🚀 |
| ANM2 animation playback | ❌ | 🚀 `preview_window` (frame-based timer) | 🚀 |
| Variant browsing (prev/next) | ❌ | 🚀 `preview_window` multi-provider | 🚀 |
| Zoom/fit controls | ❌ | 🚀 `preview_window` zoom_by/set_fit | 🚀 |
| Smooth scroll | ❌ | 🚀 `smooth_scroll` | 🚀 |
| Zoom controls | ❌ | 🚀 `zoom_controls` | 🚀 |
| Column toggle header | ❌ | 🚀 `column_toggle_header` | 🚀 |
| Game path banner | ❌ | 🚀 `game_path_banner` | 🚀 |
| Status bar (custom) | ✅ `StatusBar` | 🚀 `status_bar` | 🚀 |
| Notification backend | ❌ | 🚀 `notification_backend` | 🚀 |
| Single instance guard | ❌ | 🚀 `single_instance` (QtSingleApplication) | 🚀 |
| BBCode parser (Nexus descriptions) | ✅ | ✅ `bbcode` | ✅ |
| Menu bar (File/Edit/View/Tools/Help) | ✅ | ✅ `AppMenuBar` (dynamic per-game tools) | ✅ |
| Data tab (virtual data browser) | ✅ `DataTab` | ✅ `data_tab` (dual view, background build, context menu) | ✅ |
| Downloads tab | ✅ `DownloadsTab` | ✅ `downloads_tab` (drag-drop, watcher, compact) | ✅ |
| Saves tab | ✅ `SavesTab` | ✅ `saves_tab` (background scan, hover info) | ✅ |
| Conflicts tab | ✅ | ✅ `conflicts_tab` (image diff) | ✅ |
| Archives tab | ✅ | ✅ `archives_tab` | ✅ |
| Right panel tab system | ✅ | ✅ `right_panel` + `tab_panels` | ✅ |
| Main tab container (Full UI mode) | ❌ | 🚀 `MainTabContainer` (permanent Main + dynamic tabs) | 🚀 |

## 31. Log System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Log model (QAbstractItemModel) | ✅ `LogModel` | ✅ `Logger` (callback-based, replay buffer) | ✅ |
| Log list view | ✅ `LogList` | ✅ `console_panel` | ✅ |
| Log copy to clipboard | ✅ `LogList::copyToClipboard()` | ❌ | ❌ |
| Log open logs folder | ✅ `LogList::openLogsFolder()` | ❌ | ❌ |
| Log clear | ✅ `LogList::clear()` | ❌ | ❌ |
| Log highlighter | ✅ `LogHighlighter` | ❌ | ❌ |
| Log level filtering | ✅ | ✅ `Logger::set_level()` | ✅ |
| Group logging (begin/end) | ❌ | 🚀 `Logger::begin_group()/end_group()` | 🚀 |
| Replay buffer (256 entries) | ❌ | 🚀 `Logger` late subscriber replay | 🚀 |
| Fork-safe append | ❌ | 🚀 `Logger::raw_append()` | 🚀 |

## 32. System Tray

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| System tray icon | ✅ `SystemTrayManager` | ❌ | ❌ |
| Minimize to system tray | ✅ `minimizeToSystemTray()` | ❌ | ❌ |
| Restore from system tray | ✅ `restoreFromSystemTray()` | ❌ | ❌ |
| Tray notification | ✅ `showNotification()` | ❌ | ❌ |

## 33. Self Updater

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Self-update system (GitHub releases) | ✅ `SelfUpdater` | ❌ | ❌ |
| Update candidates (version-sorted) | ✅ `CandidatesMap` | ❌ | ❌ |
| Update backup + restart | ✅ `installUpdate()` + `restart()` | ❌ | ❌ |
| Check for updates setting | ✅ | ✅ `check_for_updates()` | ✅ |

## 34. Multi-Process / IPC

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Multi-process guard (shared memory) | ✅ `MOMultiProcess` (QSharedMemory + QLocalServer) | ✅ `single_instance` (QtSingleApplication) | ✅ |
| Ephemeral process (forward download) | ✅ `MOMultiProcess::ephemeral()` | ❌ | ❌ |
| Secondary instance (allow multiple) | ✅ `MOMultiProcess::secondary()` | ❌ | ❌ |
| Message passing between instances | ✅ `sendMessage()` / `messageSent()` | ✅ `nxm_ipc` (nxm:// forwarding) | ✅ |

## 35. Text Editor

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Text editor (line numbers, syntax, word wrap) | ✅ `TextEditor` | ❌ | ❌ |
| HTML editor | ✅ `HTMLEditor` | ❌ | ❌ |

## 36. Browser

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Integrated browser (QWebEngineView) | ✅ `BrowserDialog` | ❌ | ❌ |
| Browser tabs | ✅ `BrowserDialog::m_Tabs` | ❌ | ❌ |
| Browser download interception | ✅ `unsupportedContent()` | ❌ | ❌ |

## 37. Dialogs

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| About dialog | ✅ `AboutDialog` | ❌ | ❌ |
| Update dialog (changelog) | ✅ `UpdateDialog` | ❌ | ❌ |
| MOTD dialog | ✅ `MotDDialog` | ❌ | ❌ |
| Problems dialog (guided fixes) | ✅ `ProblemsDialog` | ❌ | ❌ |
| Selection dialog (generic picker) | ✅ `SelectionDialog` | ✅ `ListDialog` (filter, auto-select, geometry) | ✅ |
| Overwrite info dialog | ✅ | ✅ `OverwriteInfoDialog` (QFileSystemModel) | ✅ |
| Query overwrite dialog | ✅ | ✅ `QueryOverwriteDialog` (thread-safe) | ✅ |
| Sync overwrite dialog | ✅ | ✅ `SyncOverwriteDialog` (per-file combo) | ✅ |
| Credentials dialog | ✅ `CredentialsDialog` | ❌ | ❌ |
| List dialog | ✅ `ListDialog` | ✅ `ListDialog` | ✅ |
| Save text as dialog | ✅ `SaveTextAsDialog` | ❌ | ❌ |

## 38. Platform Abstraction

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Windows (native) | ✅ | ✅ | ✅ |
| Windows (MSVC compile) | ✅ | ✅ | ✅ |
| Linux (native) | ❌ | 🚀 full Linux support | 🚀 |
| Linux (OverlayFS) | ❌ | 🚀 `OverlayFsLauncher` | 🚀 |
| Linux (cgroup v2) | ❌ | 🚀 `cgroup_is_empty` | 🚀 |
| Linux (subreaper) | ❌ | 🚀 `PR_SET_CHILD_SUBREAPER` | 🚀 |
| Linux (Proton/Wine) | ❌ | 🚀 `ProtonRuntime` | 🚀 |
| macOS | ❌ | ⚠️ platform_interface stubs | ⚠️ |
| PlatformInterface (XDG, Steam, Proton) | ❌ | 🚀 `platform_interface.h` (home_dir, temp_dir, etc.) | 🚀 |
| PathResolver (canonical paths) | ❌ | 🚀 `PathResolver` + `PathResolverRegistry` | 🚀 |
| Keyring (OS-backed + file fallback) | ❌ | 🚀 `Keyring` + `FileKeyring` (XOR+base64) | 🚀 |
| Thread priority (low) | ❌ | 🚀 `set_low_priority()` | 🚀 |
| Headless launcher (CLI) | ❌ | 🚀 `HeadlessLauncher` (HeadlessConfig) | 🚀 |
| Proton version discovery | ❌ | 🚀 `find_proton()`, `enumerate_proton_versions()` | 🚀 |
| Wine binary discovery | ❌ | 🚀 `find_wine()` | 🚀 |
| Admin elevation check | ❌ | 🚀 `is_elevated()` | 🚀 |
| Symlink/junction capability check | ❌ | 🚀 `symlinks_available()` / `junctions_available()` | 🚀 |

## 39. Theme System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Theme manager (QSS token substitution) | ❌ | 🚀 `ThemeManager` (scan, load, apply, live-reload) | 🚀 |
| Icon manager | ❌ | 🚀 `IconManager` | 🚀 |
| Style manager | ❌ | 🚀 `StyleManager` | 🚀 |

## 40. Event System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Event bus (subscribe/dispatch) | ❌ | 🚀 `EventBus` (17 canonical events) | 🚀 |
| Event history ring buffer | ❌ | 🚀 500-entry `EventRecord` history | 🚀 |
| Plugin-scoped unsubscription | ❌ | 🚀 `clear_source()` on plugin unload | 🚀 |
| JSON payload helpers | ❌ | 🚀 `json_obj()` | 🚀 |

## 41. External Tool System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| External tool registry | ❌ | 🚀 `ToolRegistry` (Advisory/Workshop kinds) | 🚀 |
| Dynamic tools in menu bar | ❌ | 🚀 `AppMenuBar::update_tools_for_game()` | 🚀 |
| Proton prefix tools | ❌ | 🚀 `run_proton_tool()` (winetricks/protontricks) | 🚀 |

## 42. Packaging & Distribution

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| NSIS installer | ✅ | ⚠️ cmake target exists | ⚠️ |
| Standalone zip | ✅ | ✅ `package-windows-standalone` | ✅ |
| USVFS binaries in package | ✅ | ✅ `cmake/usvfs.cmake` | ✅ |
| AppImage (Linux) | ❌ | 🚀 `linux/appimage` | 🚀 |
| Flatpak (Linux) | ❌ | 🚀 `linux/flatpak` | 🚀 |
| DMG (macOS) | ❌ | 🚀 `macos/dmg` | 🚀 |

---

## Summary

| Category | Matched | Surpasses | Missing |
|----------|---------|-----------|---------|
| Virtual Filesystem | 8 | 4 | 5 |
| Launch Pipeline | 13 | 6 | 6 |
| Error Handling | 8 | 0 | 3 |
| Settings | 7 | 1 | 8 |
| Executable Management | 12 | 3 | 3 |
| Mod Management | 18 | 5 | 4 |
| Mod Categories | 4 | 0 | 2 |
| Mod Conflict Detection | 1 | 3 | 7 |
| Mod Content Analysis | 4 | 0 | 4 |
| Mod Info Dialog | 9 | 1 | 3 |
| Version & Update Management | 0 | 0 | 6 |
| Plugin Management | 14 | 4 | 8 |
| LOOT Integration | 1 | 1 | 7 |
| Profile Management | 7 | 2 | 8 |
| Download Management | 8 | 5 | 5 |
| Nexus Integration | 7 | 2 | 7 |
| Source Providers | 4 | 4 | 0 |
| Mod List Features | 15 | 4 | 4 |
| Mod Context Menu | 12 | 0 | 2 |
| Plugin Context Menu | 1 | 0 | 3 |
| Archive & Installation | 6 | 4 | 4 |
| Deploy System | 0 | 12 | 0 |
| Overwrite System | 6 | 3 | 0 |
| Save Game System | 8 | 3 | 4 |
| Game Detection & Knowledge | 2 | 10 | 0 |
| Pipeline System | 0 | 16 | 0 |
| Plugin Host System | 0 | 12 | 0 |
| Sort System | 0 | 4 | 0 |
| Instance Management | 2 | 6 | 0 |
| UI Layer | 20 | 17 | 1 |
| Log System | 4 | 3 | 4 |
| System Tray | 0 | 0 | 4 |
| Self Updater | 1 | 0 | 3 |
| Multi-Process / IPC | 2 | 1 | 2 |
| Text Editor | 0 | 0 | 2 |
| Browser | 0 | 0 | 3 |
| Dialogs | 6 | 0 | 5 |
| Platform Abstraction | 2 | 12 | 1 |
| Theme System | 0 | 3 | 0 |
| Event System | 0 | 4 | 0 |
| External Tool System | 0 | 3 | 0 |
| Packaging | 2 | 3 | 0 |
| **TOTAL** | **202** | **153** | **160** |

**MO2 parity: 202/362 features (56%)**

**GMM surpasses MO2: 153 features (42%)**

**Missing from GMM: 160 features (44%)**
