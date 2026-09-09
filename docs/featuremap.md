# Feature Map - MO2 vs GMM

Living document tracking MO2 feature parity and GMM-exclusive features.

**Legend:**
- ✅ = Implemented in GMM
- ⚠️ = Partially implemented (gaps remain)
- ❌ = Missing in GMM (MO2 has it)
- 🚀 = GMM-exclusive (surpasses MO2)

---

### Features comparison table

|  Criteria   | Amount | Procent |
|-------------|--------|---------|
| MO2 parity  | 248/699|   35%   |
|GMM surpasses|   163  |   23%   |
| Missing ⚠️  |    37  |    5%   |
| Missing ❌  |   458  |   66%   |

[Jump to **summary**](#summary)

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
| Plugin file-mapper mappings | ✅ `IPluginFileMapper::mappings` | ✅ `FileMapperRegistry` + `cb_v2_register_file_mapper()` | ✅ |
| VFS auto-mapping (BSA-aware) | ✅ `DirectoryEntry::addFromBSA` | ❌ | ❌ |
| Archive load order injection | ✅ `enabledArchives` priority | ⚠️ `archives.txt` read/write, no dynamic injection | ⚠️ |
| Forced library loading | ✅ `usvfs::setForcedLibraries` | ⚠️ preserved in profile copy only, no runtime loading | ⚠️ |
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
| Steam -- elevation mismatch | ✅ `canAccess` + admin dialog | ⚠️ `is_elevated()` generic admin check, not Steam-specific | ⚠️ |
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
| File association lookup | ✅ `env::getAssociation()` | ❌ | ❌ |
| Steam-related error dialogs | ✅ `badSteamReg()`, `startSteamFailed()`, `confirmStartSteam()` | ❌ | ❌ |

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
| Crash dump pruning | ✅ `cycleDiagnostics` | ⚠️ `max_core_dumps()` setting exists, no pruning logic | ⚠️ |
| USVFS log worker thread | ✅ `LogWorker` (QThread) | ✅ `std::thread` | ✅ |
| USVFS log file output | ✅ `logs/usvfs-<ts>.log` | ✅ `logs/usvfs-<ts>.log` | ✅ |
| USVFS log viewer | ✅ MO2 log panel | ❌ | ❌ |
| EventLog service check | ✅ | ❌ | ❌ |
| Sanity checks on startup | ✅ `sanityChecks()` | ❌ | ❌ |
| Sanity check: blocked files (Zone.Identifier ADS) | ✅ `sanity::checkBlocked()` | ❌ | ❌ |
| Sanity check: missing files (AV deleted) | ✅ `sanity::checkMissingFiles()` | ❌ | ❌ |
| Sanity check: incompatible OSD/DLL modules | ✅ `sanity::checkBadOSDs()` | ❌ | ❌ |
| Sanity check: USVFS-incompatible DLLs | ✅ `sanity::checkUsvfsIncompatibilites()` | ❌ | ❌ |
| Sanity check: protected/system directory paths | ✅ `sanity::checkProtected()` | ❌ | ❌ |
| Sanity check: Microsoft Store game detection | ✅ `sanity::checkMicrosoftStore()` | ❌ | ❌ |
| Spawn error: makeContent (contextual) | ✅ `spawn::dialogs::makeContent()` | ❌ | ❌ |
| Spawn error: spawnFailed dialog | ✅ `spawn::dialogs::spawnFailed()` | ❌ | ❌ |
| Spawn error: helperFailed dialog | ✅ `spawn::dialogs::helperFailed()` | ❌ | ❌ |
| Spawn error: confirmRestartAsAdmin | ✅ `spawn::dialogs::confirmRestartAsAdmin()` | ❌ | ❌ |
| Spawn error: makeRightsDetails | ✅ `spawn::dialogs::makeRightsDetails()` | ❌ | ❌ |
| Windows error formatting | ✅ `MOShared::windows_error` exception | ❌ | ❌ |
| Windows compatibility mode detection | ✅ `WindowsInfo::compatibilityMode()` | ❌ | ❌ |
| Windows version info collection | ✅ `WindowsInfo` (reported, real, BuildLab, UBR) | ❌ | ❌ |
| Process elevation detection | ✅ `WindowsInfo::isElevated()` | ✅ `is_elevated()` | ✅ |
| Module detection and version info | ✅ `env::Module` (path, version, timestamp, MD5) | ❌ | ❌ |
| Process enumeration and tree | ✅ `env::Process` + `getRunningProcesses()` | ❌ | ❌ |
| DLL load notification (ntdll LdrRegisterDllNotification) | ✅ `Environment::onModuleLoaded()` | ❌ | ❌ |
| Security product enumeration (WMI) | ✅ `env::SecurityProduct` + `getSecurityProducts()` | ❌ | ❌ |
| File security/permissions check | ✅ `env::getFileSecurity()` + `FileRights` | ❌ | ❌ |
| Display metrics collection | ✅ `env::Metrics` + `env::Display` (DPI, refresh rate) | ❌ | ❌ |
| NT API filesystem walker | ✅ `env::DirectoryWalker` (NtQueryDirectoryFile) | ❌ | ❌ |
| Windows service status query | ✅ `env::Service` + `getService()` | ❌ | ❌ |
| Registry cleanup | ✅ `env::deleteRegistryKeyIfEmpty()` | ❌ | ❌ |
| Full environment dump | ✅ `Environment::dump()` (version, timezone, security, modules, disks) | ❌ | ❌ |
| Environment timezone collection | ✅ `Environment::timezone()` | ❌ | ❌ |
| Core dump creation (self + other process) | ✅ `env::coredump()` / `env::coredumpOther()` | ❌ | ❌ |
| Log list (in-app viewer, 1000 entries) | ✅ `LogModel` + `LogList` | ❌ | ❌ |
| Log initialization and configuration | ✅ `initLogging()` (spdlog, UTC timestamps) | ❌ | ❌ |
| Log blacklisting (privacy - username masking) | ✅ `log::getDefault().addToBlacklist()` | ❌ | ❌ |
| Console attach/alloc (CLI) | ✅ `env::Console` (RAII attach/alloc/free) | ❌ | ❌ |
| CopyEventFilter (Ctrl+C in views) | ✅ `CopyEventFilter` | ❌ | ❌ |
| Problems dialog (plugin diagnostics) | ✅ `ProblemsDialog` (tree, HTML description, Fix button) | ❌ | ❌ |
| Message dialog (fire-and-forget toast) | ✅ `MessageDialog` (borderless, auto-timeout) | ❌ | ❌ |
| Diagnostics settings tab | ✅ `DiagnosticsSettingsTab` (log level, dump type, max dumps) | ✅ `build_diagnostics_tab()` + `settings.h` | ✅ |

## 4. Settings & Configuration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Executables blacklist | ✅ `Settings::executablesBlacklist` | ✅ `Settings::executables_blacklist()` + UI | ✅ |
| Skip file suffixes | ✅ `Settings::skipFileSuffixes` | ⚠️ data model only | ⚠️ |
| Skip directories | ✅ `Settings::skipDirectories` | ⚠️ data model only | ⚠️ |
| Force load libraries | ✅ `ExecutableForcedLoadSetting` | ⚠️ preserved in profile copy only | ⚠️ |
| USVFS log level | ✅ `Settings::logLevel` | ✅ `Settings::log_level()` + UI diagnostics tab | ✅ |
| USVFS crash dump type | ✅ `Settings::coreDumpType` | ✅ `Settings::core_dump_type()` + UI diagnostics tab | ✅ |
| USVFS spawn delay | ✅ `Settings::spawnDelay` | ⚠️ data model only | ⚠️ |
| Geometry persistence | ✅ `GeometrySettings` (window, splitter, toolbar) | ✅ `saveGeometry()`/`restoreGeometry()` across dialogs | ✅ |
| Widget state persistence | ✅ `WidgetSettings` (tree expand, combo, tab index) | ✅ `saveState()`/`restoreState()` for splitters, headers | ✅ |
| Color settings (conflict coloring) | ✅ `ColorSettings` (8+ color options) | ✅ `Settings` color pickers (separator + conflict colors) | ✅ |
| Plugin blacklist | ✅ `Settings::blacklisted` | ⚠️ executables blacklist exists, no plugin-specific blacklist | ⚠️ |
| Network settings (proxy, offline mode) | ✅ `NetworkSettings` | ✅ `Settings` (offline_mode, proxy, custom_browser) | ✅ |
| Splash screen | ✅ `Settings::useSplash` | ❌ | ❌ |
| Prerelease updates toggle | ✅ `Settings::usePrereleases` | ✅ `Settings::use_prereleases()` + updater filter | ✅ |
| Low-priority extraction | ✅ | 🚀 `extraction_low_priority` | 🚀 |
| Full UI mode (tabs vs popups) | ❌ | 🚀 `full_ui_mode` setting | 🚀 |
| Multi-core processing toggle | ❌ | 🚀 `performance/enable_multicore` (parallel::for_each gate) | 🚀 |
| Language selection (i18n picker) | ✅ `InterfaceSettings::language()` | ❌ | ❌ |
| Style/Theme selection (QStyle + .qss) | ✅ `InterfaceSettings::styleName()` | ❌ | ❌ |
| Collapsible separators settings | ✅ `InterfaceSettings` (ascending, descending, highlight, icons) | ✅ 10+ settings in `Settings` class | ✅ |
| Save filters toggle | ✅ `InterfaceSettings::saveFilters()` | ✅ `Settings::save_filters()` + UI checkbox | ✅ |
| Auto-collapse on hover | ✅ `InterfaceSettings::autoCollapseOnHover()` | ✅ `Settings::auto_collapse_on_hover()` + UI checkbox | ✅ |
| Display foreign mods | ✅ `InterfaceSettings::displayForeign()` | ❌ | ❌ |
| Meta downloads display | ✅ `InterfaceSettings::metaDownloads()` | ❌ | ❌ |
| Hide downloads after installation | ✅ `InterfaceSettings::hideDownloadsAfterInstallation()` | ❌ | ❌ |
| Show download notifications | ✅ `InterfaceSettings::showDownloadNotifications()` | ❌ | ❌ |
| Hide API counter | ✅ `InterfaceSettings::hideAPICounter()` | ❌ | ❌ |
| Lock GUI during executables | ✅ `InterfaceSettings::lockGUI()` | ❌ | ❌ |
| Center dialogs on parent | ✅ `GeometrySettings::centerDialogs()` | ❌ | ❌ |
| Show change game confirmation | ✅ `InterfaceSettings::showChangeGameConfirmation()` | ❌ | ❌ |
| Show menubar on Alt | ✅ `InterfaceSettings::showMenubarOnAlt()` | ❌ | ❌ |
| Double-clicks open previews | ✅ `InterfaceSettings::doubleClicksOpenPreviews()` | ❌ | ❌ |
| Tutorial completion tracking | ✅ `InterfaceSettings::isTutorialCompleted()` | ❌ | ❌ |
| Filter widget options | ✅ `InterfaceSettings::filterOptions()` | ❌ | ❌ |
| Archive parsing toggle | ✅ `Settings::archiveParsing()` | ❌ | ❌ |
| Keep backup on install | ✅ `Settings::keepBackupOnInstall()` | ❌ | ❌ |
| Profile default settings (local INIs, saves, archive invalidation) | ✅ `Settings::profileLocalInis()` etc. | ❌ | ❌ |
| Refresh thread count | ✅ `Settings::refreshThreadCount()` | ❌ | ❌ |
| Force enable core files | ✅ `GameSettings::forceEnableCoreFiles()` | ❌ | ❌ |
| Base directory variable (%BASE_DIR%) | ✅ `PathSettings::BaseDirVariable` | ❌ | ❌ |
| Recent directories | ✅ `PathSettings::recent()` | ❌ | ❌ |
| Offline mode | ✅ `NetworkSettings::offlineMode()` | ✅ `Settings::offline_mode()` | ✅ |
| Custom browser command | ✅ `NetworkSettings::customBrowserCommand()` | ✅ `Settings::custom_browser_command()` | ✅ |
| Download speed tracking per server | ✅ `NetworkSettings::setDownloadSpeed()` | ❌ | ❌ |
| Server preference list | ✅ `NetworkSettings::servers()` | ❌ | ❌ |
| Nexus endorsement integration setting | ✅ `NexusSettings::endorsementIntegration()` | ✅ `Settings::endorsement_integration()` | ✅ |
| Nexus tracked integration setting | ✅ `NexusSettings::trackedIntegration()` | ✅ `Settings::tracked_integration()` | ✅ |
| Nexus category mappings setting | ✅ `NexusSettings::categoryMappings()` | ✅ `Settings::category_mappings()` | ✅ |
| NXM handler registration (settings) | ✅ `NexusSettings::registerAsNXMHandler()` | ❌ | ❌ |
| MODL handler registration (settings) | ✅ `Settings::registerAsMODLHandler()` | ❌ | ❌ |
| Download handler registration | ✅ `Settings::registerDownloadHandlers()` | ❌ | ❌ |
| Steam app ID override | ✅ `SteamSettings::appID()` | ❌ | ❌ |
| Steam login (credential store) | ✅ `SteamSettings::login()` (Windows Credential Store) | ❌ | ❌ |
| First start detection | ✅ `Settings::firstStart()` | ❌ | ❌ |
| MO version tracking in settings | ✅ `Settings::version()` | ❌ | ❌ |
| Settings migration (auto-upgrade between versions) | ✅ `Settings::processUpdates()` | ❌ | ❌ |
| BSA date backdating | ✅ `settingsdialogworkarounds` (on_bsaDateBtn_clicked) | ❌ | ❌ |
| Reset geometry settings | ✅ `settingsdialogworkarounds` (on_resetGeometryBtn_clicked) | ❌ | ❌ |
| Reset dialog choices | ✅ `settingsdialoggeneral` (onResetDialogs) | ❌ | ❌ |
| Color separator scrollbar | ✅ `ColorSettings::colorSeparatorScrollbar()` | ✅ `Settings::color_separator_scrollbar()` | ✅ |
| Toolbar state persistence | ✅ `GeometrySettings::saveToolbars()` / `restoreToolbars()` | ✅ `saveState()`/`restoreState()` includes toolbar | ✅ |
| Dock state persistence | ✅ `GeometrySettings::saveDocks()` / `restoreDocks()` | ❌ | ❌ |
| Widget visibility persistence | ✅ `GeometrySettings::saveVisibility()` / `restoreVisibility()` | ❌ | ❌ |
| Remember question dialog buttons | ✅ `WidgetSettings::QuestionBoxMemory` | ❌ | ❌ |
| Tree expand/check state persistence | ✅ `WidgetSettings::saveTreeCheckState` / `saveTreeExpandState` | ❌ | ❌ |
| Tab widget index persistence | ✅ `WidgetSettings::saveIndex(QTabWidget)` | ❌ | ❌ |
| Combobox index persistence | ✅ `WidgetSettings::saveIndex(QComboBox)` | ❌ | ❌ |
| Checkable button state persistence | ✅ `WidgetSettings::saveChecked(QAbstractButton)` | ❌ | ❌ |
| Tab-based settings dialog (8 tabs) | ✅ `settingsdialog.cpp` (General, Theme, ModList, Paths, Diagnostics, Nexus, Plugins, Workarounds) | ✅ `settings_content_widget` (multi-tab) | ✅ |
| Settings change logging | ✅ `settingsutilities.h` `logChange()` | ❌ | ❌ |
| Color table (visual color picker with delegates) | ✅ `colortable.h/cpp` | ❌ | ❌ |
| Mod info tab order persistence | ✅ `GeometrySettings::modInfoTabOrder()` | ❌ | ❌ |
| Center on main window monitor | ✅ `GeometrySettings::centerOnMainWindowMonitor()` | ❌ | ❌ |

## 5. Executable Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Custom executables list | ✅ `ExecutablesList` (CRUD) | ✅ `Executables::Entry` + `ExecControlsBar` | ✅ |
| Per-executable arguments | ✅ `Executable::arguments` | ✅ `Executables::Entry::arguments` | ✅ |
| Per-executable working directory | ✅ `Executable::workingDirectory` | ✅ `Executables::Entry::start_in` | ✅ |
| Per-executable Steam App ID | ✅ `Executable::steamAppID` | ⚠️ MO2 importer reads `steamAppID`, not per-executable runtime | ⚠️ |
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
| Executables list proxy model | ✅ `ExecutablesListProxy` | ❌ | ❌ |

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
| Mod author/uploader metadata | ✅ `ModInfo::author()`, `uploader()` | ✅ `GamePlugin::author`, `ModInfoResult::author` from Nexus/LoversLab/ModPub | ✅ |
| Mod description | ✅ `ModInfo::getDescription()` | ✅ `GamePlugin::description`, BBCode-to-HTML rendering pipeline | ✅ |
| Mod creation time | ✅ `ModInfo::creationTime()` | ✅ `mod_scanner` (install_time, changed_time) | ✅ |
| Mod internal name | ✅ `ModInfo::internalName()` | ❌ | ❌ |
| Mod validated flag | ✅ `ModInfo::markValidated` | ✅ `mark_validated()` | ✅ |
| Mod repository tracking | ✅ `ModInfo::repository()` | ✅ `source_type` / `source_id` | ✅ |
| Plugin settings per mod | ✅ `ModInfoRegular::pluginSetting` | ✅ `Settings::plugin_setting()` + `PluginSettingsRegistry` | ✅ |
| Nexus file IDs tracking | ✅ `ModInfoRegular::installedFiles` | ✅ `file_id` on `DownloadNxm` + `InstalledFileInfo` | ✅ |
| Mod tags (deprecated/note/warning/incompatible) | ❌ | 🚀 `ModTag` system with messages | 🚀 |
| Visual nesting (parent_id, indent, fold) | ❌ | 🚀 `mod_list_model` nesting | 🚀 |
| Vendor icons (per-source badges) | ❌ | 🚀 `mod_list_model` (nexusmods, loverslab, steam, moddb) | 🚀 |
| MERGED pseudo-mod | ❌ | 🚀 `kMergedModId` constant | 🚀 |
| Game-native mod band | ❌ | 🚀 `native_band_first/last` | 🚀 |
| ModInfoForeign (non-official plugins) | ✅ `ModInfoForeign` | ❌ | ❌ |
| ModInfoSeparator | ✅ `ModInfoSeparator` | ❌ | ❌ |
| ModInfoWithConflictInfo (conflict data) | ✅ `ModInfoWithConflictInfo` | ❌ | ❌ |
| Hidden file extension detection | ✅ `ModInfo::s_HiddenExt` | ❌ | ❌ |
| getByName/getByIndex/getByModID accessors | ✅ `ModInfo::getByName()` etc. | ❌ | ❌ |
| Import strategy (Merge/Overwrite/None) | ✅ `ImportStrategy` enum | ❌ | ❌ |
| Activate mods dialog (save-game asset resolution) | ✅ `ActivateModsDialog` | ❌ | ❌ |

## 7. Mod Categories

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Category tree system | ✅ `Categories` (hierarchical) | ✅ `Category::Factory` | ✅ |
| Nexus category mapping | ✅ `NexusCategory` + `resolveNexusID` | ✅ `NexusCat` mapping | ✅ |
| Category import/export | ✅ `CategoryImportDialog` | ❌ | ❌ |
| Multi-category assignment | ✅ `ModInfo::setCategories` | ✅ `category_ids` CSV in meta.ini | ✅ |
| Primary category | ✅ `ModInfo::primaryCategory` | ✅ first entry in category CSV | ✅ |
| Special filter categories | ✅ `SpecialCategories` (Checked, UpdateAvailable, Conflict, etc.) | ❌ | ❌ |
| Category CRUD editor | ✅ | ✅ `CategoriesDialog` (editable table, full CRUD) | ✅ |
| Category filter panel | ✅ | ✅ `CategoryFilterPanel` (checkable tree, OR semantics) | ✅ |
| Category import dialog (Merge/Overwrite/None strategy) | ✅ `CategoryImportDialog` | ❌ | ❌ |
| Categories table view | ✅ `CategoriesTable` | ❌ | ❌ |

## 8. Mod Conflict Detection

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Loose file conflicts | ✅ `EConflictFlag::OVERWRITE` | ✅ `conflict_engine` | ✅ |
| Archive vs loose conflicts | ✅ `FLAG_ARCHIVE_LOOSE_CONFLICT_*` | ✅ four conflict color settings (loose/archive) | ✅ |
| Archive vs archive conflicts | ✅ `FLAG_ARCHIVE_CONFLICT_*` | ❌ | ❌ |
| Overwrite folder conflicts | ✅ `FLAG_OVERWRITE_CONFLICT` | ❌ | ❌ |
| Conflict dialog (general) | ✅ `GeneralConflictsTab` with counters | ❌ | ❌ |
| Conflict dialog (advanced) | ✅ `AdvancedConflictsTab` tree view | ❌ | ❌ |
| Conflict context menu | ✅ open/run hooked/preview/explore/hide/goto | ✅ `on_custom_context_menu()` (Merge in ImageDiff) | ✅ |
| Conflict highlighting | ✅ `EHighlight` (INVALID, CENTER, IMPORTANT, PLUGIN) | ✅ row tinting + scrollbar marks + color config | ✅ |
| Per-mod conflict stats | ❌ | 🚀 `conflict_engine` (wins/losses/total) | 🚀 |
| Blake2b fingerprint cache | ❌ | 🚀 `ConflictIndex` (SQLite + blake2b) | 🚀 |
| Image diff (conflict comparison) | ❌ | 🚀 `conflicts_tab` image_diff_requested | 🚀 |
| ConflictListModel / ConflictItem | ✅ `ConflictListModel`, `ConflictItem` | ❌ | ❌ |
| Data tab conflict mode | ✅ `dataTabShowOnlyConflicts` | ❌ | ❌ |

## 9. Mod Content Analysis

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod data content types | ✅ `ModDataContentHolder` | ✅ `ModContentId` enum | ✅ |
| Mod flags (INVALID, BACKUP, SEPARATOR, etc.) | ✅ `EFlag` | ✅ `ModState` + mod_meta flags | ✅ |
| Mod content icons | ✅ `ModContentIconDelegate` | ❌ | ❌ |
| Mod conflict icons | ✅ `ModConflictIconDelegate` | ✅ `FlagsDelegate` (per-icon tooltips) | ✅ |
| Mod flag icons | ✅ `ModFlagIconDelegate` | ✅ `FlagsDelegate` (wrap + tooltips) | ✅ |
| Empty mod flag (dummy icon, tooltip) | ✅ | ✅ `ModList::set_empty` + `plugin-dummy` icon + tooltip | ✅ |
| Mod version delegate (color-coded) | ✅ `ModListVersionDelegate` | ❌ | ❌ |
| INI tweaks detection | ✅ `ModInfo::getIniTweaks()` | ⚠️ profile-level initweaks.ini + plugin INI tooltip | ⚠️ |
| Archive listing per mod | ✅ `ModInfo::archives()` | ✅ `archives_html()` in plugin tooltip | ✅ |
| ModDataContent updated signal | ✅ `ModDataContentUpdated` | ❌ | ❌ |
| CombinedModDataContent | ✅ `CombinedModDataContent` | ❌ | ❌ |

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
| Tab reordering | ✅ `onTabMoved` + `saveTabOrder` | ⚠️ internal tab_order_ vector, not user-draggable | ⚠️ |
| Tab color coding (data presence) | ✅ `setTabsColors` | ❌ | ❌ |
| Mod navigation (prev/next) | ✅ `onPreviousMod`, `onNextMod` | ✅ `prev_btn_`, `next_btn_`, separator-aware nav | ✅ |
| Mod info tab order persistence | ✅ `GeometrySettings::modInfoTabOrder()` | ❌ | ❌ |

## 11. Version & Update Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Version checking (installed vs newest) | ✅ `ModInfo::updateAvailable` | ✅ `ModUpdateDbClient::has_update()` + `newestVersion` in meta | ✅ |
| Newest version tracking | ✅ `ModInfo::newestVersion()` | ✅ `ModInfoResult::newest_version` + `nexusnewestversion` in meta | ✅ |
| Ignored version | ✅ `ModInfo::ignoredVersion()` | ❌ | ❌ |
| Downgrade detection | ✅ `ModInfo::downgradeAvailable()` | ❌ | ❌ |
| Batch update check | ✅ `checkAllForUpdate` | ❌ | ❌ |
| Check update after install | ✅ `Settings::checkUpdateAfterInstallation` | ❌ | ❌ |
| ModUpdateDbClient (per-game DB poll, ETag/304, offline cache) | ❌ | 🚀 `ModUpdateDbClient` (ISO 8601 compare, `by_game` index) | 🚀 |
| Per-game index pipeline (by_game directory, manifest) | ❌ | 🚀 `ModUpdateDbClient` per-game index + shard lookup | 🚀 |
| GitHub releases API (not Nexus) | ✅ `SelfUpdater` queries GitHub API | ❌ | ❌ |
| Prerelease filtering | ✅ `selfupdater.cpp` filters by draft/prerelease flags | ✅ `self_updater.cpp` filters by prerelease | ✅ |
| Update dialog with Markdown changelogs | ✅ `UpdateDialog` (version diff + release notes) | ❌ | ❌ |
| MOTD (Message of the Day) | ✅ `motdAvailable` signal | ❌ | ❌ |

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
| Master/Light/Medium/Blueprint flags | ✅ `isMasterFlagged`, `isLightFlagged`, etc. | ⚠️ master, light, medium implemented; no Blueprint flag | ⚠️ |
| Missing masters detection | ✅ `testMasters` + `missingMasters` | ✅ `set_missing_masters()` + tooltip + emblem | ✅ |
| Plugin lock (pin at position) | ✅ `isESPLocked`, `lockESPIndex` | ✅ `set_locked()` + `apply_locked_order()` + lockedorder.txt | ✅ |
| Plugin relationship fix | ✅ `fixPluginRelationships` | ❌ | ❌ |
| Plugin priority shift | ✅ `shiftPluginsPriority` | ❌ | ❌ |
| Plugin send to priority | ✅ `sendToPriority` | ⚠️ `send_to_highest/lowest_priority()` exists, no arbitrary position | ⚠️ |
| Enable/disable all plugins | ✅ `setEnabledAll` | ✅ `set_all_enabled()` | ✅ |
| Plugin index generation (FE/FD) | ✅ `generatePluginIndexes` | ✅ `generate_mod_indexes()` | ✅ |
| LOOT messages per plugin | ✅ `Plugin::messages` | ✅ `DiagnosticsRegistry` + `GamePlugin::messages` | ✅ |
| LOOT dirty info (ITM, deleted refs) | ✅ `Dirty` struct | ❌ | ❌ |
| LOOT incompatibilities | ✅ `Plugin::incompatibilities` | ❌ | ❌ |
| LOOT stats (time, version) | ✅ `Stats` | ❌ | ❌ |
| Plugin author/description display | ✅ `pluginlist.h` | ✅ `GamePlugin::author`/`description` in tooltip HTML | ✅ |
| Plugin FormVersion/HeaderVersion | ✅ `formVersion`, `headerVersion` | ✅ `esp_header` | ✅ |
| Plugin archive loading detection | ✅ `loadsArchive` | ⚠️ `GamePlugin::archives` + `archives_html()`, implicit detection | ⚠️ |
| Drag-and-drop plugin reorder | ✅ `dropMimeData` | ✅ `PluginTable` with `InternalMove` + `on_reorder` | ✅ |
| Plugin foreground coloring (LOOT-based) | ✅ `foregroundData()` | ✅ state-based `setForeground()` for locked/missing/master | ✅ |
| Plugin tooltip data (LOOT messages) | ✅ `tooltipData()` | ✅ `plugin_tooltip_html()` with full sub-blocks | ✅ |
| Plugin highlight from mod selection | ✅ `highlightPlugins()` | ✅ `set_contained_plugins()` / `set_master_plugins()` | ✅ |
| Transitive master enable/disable | ❌ | 🚀 `set_enabled()` enables/disables masters | 🚀 |
| Band reassertion (native+CC invariant) | ❌ | 🚀 `reassert_band()` | 🚀 |
| Locked order application | ❌ | 🚀 `apply_locked_order()` | 🚀 |
| Plugin type classification | ✅ | ✅ Regular/Master/Light/Medium enum | ✅ |
| Plugin counter (by type) | ✅ `ModCounters` | ✅ `plugins_tab` active/total breakdown | ✅ |
| Zero-record plugin dummy icon | ✅ | ✅ `plugin-dummy` icon for HEDR record count == 0 | ✅ |
| Plugin list sort proxy | ✅ `pluginlistsortproxy.h` (column filtering, custom sorting) | ❌ | ❌ |
| Plugin list highlight masters | ✅ `PluginList::highlightMasters()` | ❌ | ❌ |
| Plugin list ChangeBracket (RAII layout notifications) | ✅ `PluginList::ChangeBracket` | ❌ | ❌ |
| Plugin list view (filter, counter, keyboard nav) | ✅ `pluginlistview.h/cpp` | ❌ | ❌ |
| Plugin list context menu | ✅ `pluginlistcontextmenu.h/cpp` (enable/disable, send-to, lock, open origin) | ❌ | ❌ |
| Plugin list model (metadata, type flags) | ✅ `pluginlist.h` (form/header version, author, description) | ❌ | ❌ |

## 13. LOOT Integration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| LOOT sort execution | ✅ `Loot::sort()` | ✅ `Sorter::Loot` | ✅ |
| LOOT report generation | ✅ `Loot::createReport()` | ⚠️ `loot_report.json` (JSON only, no HTML/markdown) | ⚠️ |
| LOOT report viewer (markdown + web) | ✅ `LootDialog` + `MarkdownDocument` | ❌ | ❌ |
| LOOT progress display | ✅ `LootDialog::setProgress()` | ✅ `on_loot_progress(int stage, QString)` signal | ✅ |
| LOOT dirty info details | ✅ `Dirty` (CRC, ITM, deleted refs, navmesh, utility) | ❌ | ❌ |
| LOOT incompatibilities details | ✅ `File` (name + displayName) | ❌ | ❌ |
| LOOT missing masters | ✅ `Plugin::missingMasters` | ✅ `missing_masters_html()` in plugin tooltip | ✅ |
| Masterlist manager (GitHub walk-down) | ✅ | 🚀 `MasterlistManager` (branch walk, 24h TTL, offline fallback) | 🚀 |
| LOOT sorted plugin list application | ✅ `lootdialog.cpp` applySortedLoadOrder() | ❌ | ❌ |
| LOOT sorted plugin list Markdown rendering | ✅ `loot.h` getSortedPluginListMarkdown() | ❌ | ❌ |
| LOOT clean info display | ✅ `Plugin::clean` vector ("Verified clean by X") | ❌ | ❌ |
| LOOT plugin flags (loadsArchive, isMaster, isLightMaster) | ✅ `Plugin` struct per-plugin metadata | ❌ | ❌ |
| LOOT cancel/terminate | ✅ `Loot::cancel()` terminates lootcli process | ❌ | ❌ |
| LOOT statistics (timing, versions) | ✅ `Stats` struct (execution time, lootcli version) | ❌ | ❌ |
| LOOT general messages | ✅ `Report::messages` (non-plugin-specific) | ❌ | ❌ |
| LOOT PluginList integration (addLootReport) | ✅ `PluginList::addLootReport()` integrates LOOT data | ❌ | ❌ |
| LOOT log level setting | ✅ `DiagnosticsSettings::lootLogLevel()` | ❌ | ❌ |
| LOOT async pipe communication | ✅ `AsyncPipe` (Windows Named Pipe IPC) | ❌ | ❌ |

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
| Local INI settings | ✅ `Profile::localSettingsEnabled` | ✅ `ProfileManager::local_settings()` + UI checkbox | ✅ |
| Profile INI tweaks | ✅ `Profile::getProfileTweaks` | ✅ `write_tweaked_ini()` + `initweaks.ini` | ✅ |
| Archive invalidation toggle | ✅ `Profile::invalidationActive` | ✅ `automatic_archive_invalidation()` + UI + BSA feature | ✅ |
| Profile locking (plugin order) | ✅ `Profile::getLockedOrderFileName` | ✅ `read_locked_order()` / `write_locked_order()` + `apply_locked_order()` | ✅ |
| Profile transfer saves | ✅ `TransferSavesDialog` | ❌ | ❌ |
| Profile rename | ✅ `Profile::rename` | ✅ `engine::profile::rename_profile()` | ✅ |
| Profile copy | ✅ `Profile::createPtrFrom` | ✅ `engine::profile::copy_profile()` | ✅ |
| Profile forced libraries | ✅ `Profile::determineForcedLibraries` | ⚠️ preserved in profile copy only, no runtime loading | ⚠️ |
| Profile switch result + callbacks | ❌ | 🚀 `ProfileSwitchResult` + `ProfileSwitchCallbacks` | 🚀 |
| Profile switch EventBus emission | ❌ | 🚀 `kProfileChanged` event | 🚀 |
| Profile bar (combo + folder shortcuts) | ❌ | 🚀 `ProfileBar` (12 FolderKind types, export/import) | 🚀 |
| Profile deletion | ✅ `profilesdialog.cpp` on_removeProfileButton_clicked() | ✅ `on_delete_profile()` with confirmation + active guard | ✅ |
| Active profile protection (cannot rename/delete active) | ✅ `profilesdialog.cpp` | ✅ `profile_manager_dialog.cpp` active-profile guard | ✅ |
| Profile creation with default vs copy choice | ✅ `ProfileInputDialog` (getPreferDefaultSettings) | ❌ | ❌ |
| Mod priority management per profile | ✅ `Profile::setModPriority()`, `getModPriority()`, `setModsEnabled()` | ❌ | ❌ |
| Mod status signal | ✅ `Profile::modStatusChanged` signal | ❌ | ❌ |
| Tweaked INI creation | ✅ `Profile::createTweakedIniFile()` | ✅ `write_tweaked_ini()` | ✅ |
| Profile settings as arbitrary key/value | ✅ `Profile::setting()`, `storeSetting()`, `settingsByGroup()` | ❌ | ❌ |
| Rename mod across all profiles | ✅ `Profile::renameModInAllProfiles()` | ❌ | ❌ |
| Active mods retrieval | ✅ `Profile::getActiveMods()` | ❌ | ❌ |
| Profile existence check | ✅ `Profile::exists()` | ❌ | ❌ |
| Profile find settings (auto-detect) | ✅ `Profile::findProfileSettings()` | ✅ `detect_local_settings()` in profile_creation | ✅ |
| Modlist write cancellation | ✅ `Profile::cancelModlistWrite()` | ❌ | ❌ |
| Profile debug dump | ✅ `Profile::debugDump()` | ✅ `debug_window.cpp` profile display | ✅ |
| Profile INI files (full set - 7 file paths) | ✅ `Profile` (plugins.txt, loadorder.txt, lockedorder.txt, modlist.txt, archives.txt, ini, tweaks) | ❌ | ❌ |

## 15. Download Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Download state machine | ✅ `DownloadState` (14 states) | ✅ `curl_download` states | ✅ |
| Pause/resume downloads | ✅ `pauseDownload`, `resumeDownload` | ✅ `downloads_tab` pause/resume | ✅ |
| Cancel downloads | ✅ `cancelDownload` | ✅ abort callback | ✅ |
| Download speed tracking | ✅ `downloadSpeed` rolling average | ❌ | ❌ |
| MD5 lookup | ✅ `queryInfoMd5` | ❌ | ❌ |
| Download meta files (sidecar) | ✅ `createMetaFile` | ⚠️ `ModMeta` for mod dirs, not download archive meta | ⚠️ |
| Hidden downloads | ✅ `isHidden`, `restoreDownload` | ✅ `downloads_tab` show hidden checkbox | ✅ |
| Automatic retry (3x) | ✅ `AUTOMATIC_RETRIES` | ✅ `NetworkOptions::max_retries` + `retry_backoff_ms` + Retry-After | ✅ |
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
| NXM protocol download handler | ✅ `addNXMDownload()` (nxm:// link processing) | ❌ | ❌ |
| Plugin download API (startDownloadURLs, etc.) | ✅ `IDownloadManager` plugin API | ❌ | ❌ |
| Nexus collection link rejection | ✅ `downloadmanager.cpp` detects collection links | ❌ | ❌ |
| Duplicate download detection (file name + auto-rename) | ✅ `downloadmanager.cpp` checks same-name files | ❌ | ❌ |
| Duplicate NXM prevention (mod+file ID dedup) | ✅ `downloadmanager.cpp` checks pending+active for (modID, fileID) | ❌ | ❌ |
| .unfinished extension for in-progress files | ✅ `downloadmanager.cpp` (.unfinished suffix) | ❌ | ❌ |
| HTTP/2 download support | ✅ `QHttp2Configuration` (16 MiB windows) | ❌ | ❌ |
| Login-gated resume | ✅ `downloadstab.cpp` loggedInAction() | ❌ | ❌ |
| Empty URL resume prevention | ✅ `downloadmanager.cpp` "No known download urls" | ❌ | ❌ |
| Orphan meta file cleanup | ✅ `downloadmanager.cpp` refreshList() | ❌ | ❌ |
| Server preference sorting (mirror priority) | ✅ `ServerByPreference`, `evaluateFileInfoMap()` | ❌ | ❌ |
| Per-server speed history (persistent stats) | ✅ `ServerInfo` rolling speed list | ❌ | ❌ |
| Download speed signal for stats persistence | ✅ `downloadSpeed(serverName, bytesPerSecond)` signal | ❌ | ❌ |
| Open meta file (sidecar viewer) | ✅ `downloadmanager.cpp` openMetaFile() | ❌ | ❌ |
| Manual metadata re-query (info + MD5) | ✅ `queryInfo()`, `queryInfoMd5()` | ❌ | ❌ |
| Batch metadata query for all incomplete | ✅ `queryDownloadListInfo()` | ❌ | ❌ |
| MD5 multi-game namespace search | ✅ `queryInfoMd5()` queries alternate game short names | ❌ | ❌ |
| MD5 result disambiguation | ✅ `downloadmanager.cpp` filename + active file matching | ❌ | ❌ |
| File type classification display | ✅ `getFileTypeString()` (Main/Update/Optional/etc.) | ❌ | ❌ |
| Mark installed / mark uninstalled | ✅ `markInstalled()`, `markUninstalled()` | ❌ | ❌ |
| Install from download (double-click / context menu) | ✅ `downloadlistview.cpp` install action | ❌ | ❌ |
| Download status color coding | ✅ `downloadlist.cpp` ForegroundRole colors | ❌ | ❌ |
| Remaining time estimation | ✅ `downloadmanager.cpp` ETA calculation | ❌ | ❌ |
| Batch delete operations (all/installed/uninstalled) | ✅ `downloadlistview.cpp` issueDeleteAll/Completed/Uninstalled | ❌ | ❌ |
| Batch hide operations + un-hide all | ✅ `downloadlistview.cpp` issueRemoveFromView/RestoreToView | ❌ | ❌ |
| Filter widget for downloads | ✅ `FilterWidget` (fuzzy match) | ❌ | ❌ |
| Meta/display name toggle setting | ✅ `downloadlist.cpp` metaDownloads | ❌ | ❌ |
| Keyboard shortcuts (Enter=install, Delete=remove, Space=pause) | ✅ `downloadlistview.cpp` | ❌ | ❌ |
| Visit on Nexus (from download) | ✅ `downloadmanager.cpp` visitOnNexus() | ❌ | ❌ |
| Visit uploader profile | ✅ `downloadmanager.cpp` visitUploaderProfile() | ❌ | ❌ |
| Plugin download callbacks (onDownloadComplete/Paused/Failed/Removed) | ✅ `IDownloadManager` boost::signals2 | ❌ | ❌ |
| TaskProgress integration (Windows taskbar progress) | ✅ `downloadmanager.cpp` TaskProgressManager | ❌ | ❌ |
| S3 signed URL filename extraction | ✅ `downloadmanager.cpp` response-content-disposition= | ❌ | ❌ |
| File time fallback chain (birthTime -> metadataChangeTime -> lastModified) | ✅ `downloadmanager.cpp` getFileTime() | ❌ | ❌ |

## 16. Nexus Integration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| NXM handler registration | ✅ `registerAsNXMHandler` | ✅ `nxm_router` + `nxm_ipc` | ✅ |
| Nexus account / auth | ✅ | ✅ `nexus_account` + `nexus_auth` | ✅ |
| Nexus HTTP API | ✅ | ✅ `nexus_http` + `nexus_servers` | ✅ |
| Endorsement system | ✅ `ModInfo::endorse()` | ⚠️ UI button + setting exist, no API call (opens browser) | ⚠️ |
| Tracking system | ✅ `ModInfo::track()` | ⚠️ UI button + setting exist, no API call (opens browser) | ⚠️ |
| "Never endorse" option | ✅ `ModInfo::setNeverEndorse` | ❌ | ❌ |
| Nexus description (HTML) | ✅ `ModInfo::getNexusDescription` | ✅ `nexusdescription` in meta + BBCode rendering | ✅ |
| Nexus category ID tracking | ✅ `ModInfo::getNexusCategory` | ✅ `nexuscategory` in meta + read/write | ✅ |
| Nexus update timestamps | ✅ `getLastNexusUpdate/Query` | ⚠️ `nexuslastmodified` key recognized in meta, not actively used | ⚠️ |
| OAuth login flow | ✅ `NexusOAuthLogin` | ❌ | ❌ |
| Nexus user account info | ✅ `ApiUserAccount` | ✅ `NexusUserInfo` | ✅ |
| Rate limit tracking | ✅ | ✅ `RateLimitInfo` (hourly + daily) | ✅ |
| Download mirror registry | ❌ | 🚀 `NexusServers` (speed samples, preferred ordering) | 🚀 |
| Nexus API key manual entry | ✅ | ✅ `NexusManualKeyDialog` (Open Browser/Paste/Clear) | ✅ |
| Tier-derived queue defaults | ❌ | 🚀 `nexus_queue_default_for()` (Regular=queue, Premium=parallel) | 🚀 |
| Nexus connection UI (reusable component) | ✅ `NexusConnectionUI` (shared between Settings and CreateInstance) | ❌ | ❌ |
| Endorsement state tracking | ✅ `EndorsementState` enum (Accepted/Refused/NoDecision) | ❌ | ❌ |
| Nexus FileStatus (REMOVED, ARCHIVED) | ✅ `NexusInterface::FileStatus` | ❌ | ❌ |

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
| modl:// protocol handler (mod.pub / MO2 modlhandler) | ❌ | 🚀 `modl://` (Win registry + XDG desktop, route through nxm_ipc) | 🚀 |
| ModPub metadata provider (mod.pub API) | ❌ | 🚀 `ModPubProvider` (page scrape, mod_id, Refresh) | 🚀 |
| modl:// transport-protocol distinction (origin attribution) | ❌ | 🚀 modl:// is transport, not source; real origin attributed | 🚀 |

## 18. Mod List Features

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Multi-criteria sorting | ✅ `ModListSortProxy` | ✅ `mod_table_view` + sort proxy | ✅ |
| Category/content/special filtering | ✅ `FilterList` | ✅ `CategoryFilterPanel` + `mod_filter_bar` | ✅ |
| Grouping (by separator, category, Nexus ID) | ✅ `QtGroupingProxy` | ✅ visual nesting (parent_id, indent, fold) | ✅ |
| Drag-and-drop reorder | ✅ `ModList::dropMimeData` | ✅ `mod_table_view` drop support | ✅ |
| Scroll markers | ✅ `ViewMarkingScrollBar` | ✅ `ModMarkingScrollBar` (separator marks) | ✅ |
| CSV export | ✅ `exportModListCSV` | ❌ (removed; replaced by combined import/export) | ❌ |
| Bulk enable/disable | ✅ `setActive(indices)` | ✅ `toggle_selected_mods()` | ✅ |
| Priority shift (bulk) | ✅ `shiftModsPriority` | ✅ `priority_move_selected()` | ✅ |
| Send to top/bottom/priority | ✅ `sendModsToTop/Bottom/Priority` | ✅ `send_to_highest/lowest_priority()` | ✅ |
| Send to separator | ✅ `sendModsToSeparator` | ✅ `move_to_separator()` | ✅ |
| Send to First/Last Conflict | ✅ `sendModsToFirstConflict/LastConflict` | ❌ | ❌ |
| Collapseable separators | ✅ `collapsibleSeparators` | ✅ 10+ collapsible separator settings | ✅ |
| Auto-collapse on hover | ✅ `autoCollapseOnHover` | ✅ `Settings::auto_collapse_on_hover()` | ✅ |
| Filter persistence | ✅ `saveFilters` | ✅ `Settings::save_filters()` | ✅ |
| Filter AND/OR mode | ✅ `FilterAnd`/`FilterOr` | ❌ | ❌ |
| Column visibility toggle | ✅ `setColumnVisible()` | ✅ `column_toggle_header` | ✅ |
| Mod counter display | ✅ `ModCounters` (LCD) | ✅ `status_bar` (counts) | ✅ |
| Create separator | ✅ | ✅ `create_separator()` / `create_separator_named()` | ✅ |
| Create empty mod | ✅ `createEmptyMod` | ✅ `create_empty_mod()` | ✅ |
| Import archives | ✅ | ✅ `import_archives()` | ✅ |
| Export/import modlist | ✅ | ✅ `export_modlist()` / `import_modlist()` | ✅ |
| Overwrite file drop-to-mod | ❌ | 🚀 `overwrite_files_dropped` signal | 🚀 |
| IndentDelegate (nesting) | ❌ | 🚀 `IndentDelegate` for Name column | 🚀 |
| FlagsDelegate with tooltips | ❌ | 🚀 per-emblem hover text | 🚀 |
| ModListSortProxy criteria system | ✅ `ModListSortProxy::Criteria` | ❌ | ❌ |
| ModListSortProxy separator mode | ✅ `ModListSortProxy::SeparatorMode` | ❌ | ❌ |
| ModList column roles (IndexRole, PriorityRole) | ✅ `ModList::IndexRole`, `PriorityRole` | ❌ | ❌ |
| ModList signals (showMessage, modRenamed, modUninstalled, fileMoved, modPrioritiesChanged) | ✅ `ModList` signals | ❌ | ❌ |
| ModListProxy / ModListByPriorityProxy | ✅ proxy models | ❌ | ❌ |

## 19. Mod Context Menu

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Visit on Nexus | ✅ `visitOnNexus` | ✅ `source_visit_info` (Nexus/LoversLab) | ✅ |
| Visit web page | ✅ `visitWebPage` | ✅ source-aware context menu | ✅ |
| Reinstall mod | ✅ `reinstallMod` | ❌ | ❌ |
| Create backup | ✅ `createBackup` | ⚠️ deploy-level `backup_original()`, no standalone user action | ⚠️ |
| Restore backup | ✅ `restoreBackup` | ⚠️ deploy-level `remove_and_restore()`, no standalone user action | ⚠️ |
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
| Enable/disable plugin | ✅ `PluginListContextMenu` | ❌ | ❌ |
| Send-to priority | ✅ `sendToPriority` | ❌ | ❌ |
| Lock/unlock plugin | ✅ `setESPLock` | ❌ | ❌ |

## 21. Archive & Installation

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| FOMOD installer | ✅ `IPluginInstaller` | ✅ `FomodInstaller` plugin + engine | ✅ |
| FOMOD XML parsing | ✅ | ✅ `module_config` (GroupType, Dependency, PluginType) | ✅ |
| FOMOD condition tester | ✅ | ✅ `condition_tester` (file/flag/game/composite) | ✅ |
| FOMOD C# script detection | ✅ | ✅ `hasCSharpScript()` | ✅ |
| FOMOD view model (step nav) | ✅ | ✅ `FomodViewModel` (step forward/back, flag map) | ✅ |
| FOMOD file installer | ✅ | ✅ `FomodFileInstaller::apply()` | ✅ |
| Archive password support | ✅ `queryPassword` | ❌ (encrypted archives rejected, not supported) | ❌ |
| Installation merge/replace | ✅ `merged`, `replaced` | ❌ | ❌ |
| Backup on install | ✅ `keepBackupOnInstall` | ❌ | ❌ |
| Installation result tracking | ✅ `InstallationResult` | ❌ | ❌ |
| Staging layout normalization | ❌ | 🚀 `analyze_staging_layout()` + `normalize_staging_root()` | 🚀 |
| BSA/BA2 archive listing | ❌ | 🚀 `DataArchive` (libarchive-backed) | 🚀 |
| Install name dialog (smart candidates) | ❌ | 🚀 `InstallNameDialog` (editable combobox) | 🚀 |
| Install progress dialog (modeless) | ❌ | 🚀 `InstallProgressDialog` (300ms show delay) | 🚀 |
| IPluginInstaller::EInstallResult | ✅ `IPluginInstaller::EInstallResult` | ❌ | ❌ |
| Archive file tree representation | ✅ `ArchiveFileTree`, `ArchiveFileEntry` | ❌ | ❌ |

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
| Save game open in explorer | ✅ | ⚠️ open_explorer exists for mod files, not saves specifically | ⚠️ |
| Save game fix missing assets | ✅ `SavesTab::fixMods()` | ⚠️ detection + display exists, no fix action | ⚠️ |
| Transfer saves dialog | ✅ `TransferSavesDialog` | ❌ | ❌ |
| Save game streaming (per-save entryReady, binary-insert sorted list) | ❌ | 🚀 `SavesScanWorker::entryReady` + `SavesTab::on_entry_ready` | 🚀 |
| Save Information dialog (2-column details, thumbnail, plugin list) | ✅ `GamebryoSaveGameInfoWidget` | ✅ `SaveInfoDialog` (2-column, thumbnail, plugin load-order status) | ✅ |
| Parallel save scan (parallel parse + provider indexing) | ❌ | 🚀 `parallel::for_each` + `SavesScanWorker` double-fire fix | 🚀 |
| Disabled-but-present plugins excluded from missing assets | ✅ | ✅ `find_save_missing_assets` (enabled OR force_loaded check) | ✅ |

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
| Sort provider / registry | ❌ | 🚀 `Sorter::Interface` + `Sorter::Registry` | 🚀 |
| C ABI sort provider | ❌ | 🚀 `Sorter::Abi` | 🚀 |
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
| Create instance wizard (7-page) | ✅ `CreateInstanceDialog` (Intro, Type, Game, Variants, Name, Paths, Profiles, Nexus, Confirmation) | ❌ | ❌ |
| Game variant selection | ✅ `CreateInstanceDialog` (game variants) | ❌ | ❌ |
| Microsoft Store game handling | ✅ `InstanceManager` | ❌ | ❌ |

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
| Data tab (virtual data browser) | ✅ `DataTab` | ✅ `data_tab` (dual view, background build worker, context menu) | ✅ |
| Downloads tab | ✅ `DownloadsTab` | ✅ `downloads_tab` (drag-drop, watcher, compact) | ✅ |
| Saves tab | ✅ `SavesTab` | ✅ `saves_tab` (background scan, hover info) | ✅ |
| Conflicts tab | ✅ | ✅ `conflicts_tab` (image diff) | ✅ |
| Archives tab | ✅ | ✅ `archives_tab` | ✅ |
| Right panel tab system | ✅ | ✅ `right_panel` + `tab_panels` | ✅ |
| Main tab container (Full UI mode) | ❌ | 🚀 `MainTabContainer` (permanent Main + dynamic tabs) | 🚀 |
| Desktop shortcut management | ✅ `env::Shortcut` (IShellLink COM) | ❌ | ❌ |
| Shell context menu integration | ✅ `env::ShellMenu` / `env::ShellMenuCollection` (IContextMenu COM) | ❌ | ❌ |
| CopyEventFilter (Ctrl+C in views) | ✅ `CopyEventFilter` | ❌ | ❌ |
| Message dialog (fire-and-forget toast) | ✅ `MessageDialog` (borderless, auto-timeout) | ❌ | ❌ |
| About dialog | ✅ `AboutDialog` | ✅ `QMessageBox::about` (version string) | ✅ |

## 31. Log System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Log model (QAbstractItemModel) | ✅ `LogModel` | ✅ `Logger` (callback-based, replay buffer) | ✅ |
| Log list view | ✅ `LogList` | ✅ `console_panel` | ✅ |
| Log copy to clipboard | ✅ `LogList::copyToClipboard()` | ❌ | ❌ |
| Log open logs folder | ✅ `LogList::openLogsFolder()` | ❌ | ❌ |
| Log clear | ✅ `LogList::clear()` | ✅ `ConsolePanel::clear()` | ✅ |
| Log highlighter | ✅ `LogHighlighter` | ❌ | ❌ |
| Log level filtering | ✅ | ✅ `Logger::set_level()` | ✅ |
| Group logging (begin/end) | ❌ | 🚀 `Logger::begin_group()/end_group()` | 🚀 |
| Replay buffer (256 entries) | ❌ | 🚀 `Logger` late subscriber replay | 🚀 |
| Fork-safe append | ❌ | 🚀 `Logger::raw_append()` | 🚀 |
| Log initialization (spdlog, UTC timestamps, pattern) | ✅ `initLogging()` | ❌ | ❌ |
| Log blacklisting (privacy - username masking) | ✅ `log::getDefault().addToBlacklist()` | ❌ | ❌ |
| Log to stdout (via Console attach) | ✅ `logToStdout()` | ❌ | ❌ |

## 32. System Tray

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| System tray icon | ✅ `SystemTrayManager` | ⚠️ `SystemTrayManager` exists, context menu works, not wired for minimize | ⚠️ |
| Minimize to system tray | ✅ `minimizeToSystemTray()` | ⚠️ `closeEvent()` does not check minimize-to-tray setting | ⚠️ |
| Restore from system tray | ✅ `restoreFromSystemTray()` | ⚠️ tray icon Show action exists, no restore-from-minimized logic | ⚠️ |
| Tray notification | ✅ `showNotification()` | ⚠️ `SystemTrayManager::show_notification()` (exists, not wired up) | ⚠️ |

## 33. Self Updater

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Self-update system (GitHub releases) | ✅ `SelfUpdater` | ✅ `SelfUpdater` with platform-specific updaters (Windows/macOS/Linux/Flatpak/AUR/AppImage) | ✅ |
| Update candidates (version-sorted) | ✅ `CandidatesMap` | ❌ | ❌ |
| Update backup + restart | ✅ `installUpdate()` + `restart()` | ❌ | ❌ |
| Check for updates setting | ✅ | ✅ `check_for_updates()` | ✅ |
| Update download with progress dialog | ✅ `selfupdater.cpp` showProgress(), QProgressDialog | ❌ | ❌ |
| Offline mode check before update | ✅ `selfupdater.cpp` testForUpdate() respects offline mode | ❌ | ❌ |

## 34. Multi-Process / IPC

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Multi-process guard (shared memory) | ✅ `MOMultiProcess` (QSharedMemory + QLocalServer) | ✅ `single_instance` (QtSingleApplication) | ✅ |
| Ephemeral process (forward download) | ✅ `MOMultiProcess::ephemeral()` | ❌ | ❌ |
| Secondary instance (allow multiple) | ✅ `MOMultiProcess::secondary()` | ❌ | ❌ |
| Message passing between instances | ✅ `sendMessage()` / `messageSent()` | ✅ `nxm_ipc` (nxm:// forwarding) | ✅ |
| Command-line global options (--pick, --multiple, --logs, -i, -p) | ✅ `CommandLine` global options | ❌ | ❌ |
| Forward to primary instance | ✅ `CommandLine::forwardToPrimary()` | ❌ | ❌ |
| NXM/moshortcut:// link protocol parsing | ✅ `CommandLine` handles moshortcut:// and nxm:// | ❌ | ❌ |

## 35. Text Editor

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Text editor (line numbers, syntax, word wrap) | ✅ `TextEditor` | ❌ (read-only QPlainTextEdit in generic_files_tab, no line numbers/syntax/wrap) | ❌ |
| HTML editor | ✅ `HTMLEditor` | ❌ (DescriptionBrowser is read-only viewer) | ❌ |

## 36. Browser

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Integrated browser (QWebEngineView) | ✅ `BrowserDialog` | ❌ (external browser launch only via `custom_browser_command`) | ❌ |
| Browser tabs | ✅ `BrowserDialog::m_Tabs` | ❌ | ❌ |
| Browser download interception | ✅ `unsupportedContent()` | ❌ | ❌ |

## 37. Dialogs

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| About dialog | ✅ `AboutDialog` | ✅ `QMessageBox::about` (version string) | ✅ |
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
| Message dialog (fire-and-forget toast) | ✅ `MessageDialog` | ❌ | ❌ |
| Category import dialog | ✅ `CategoryImportDialog` | ❌ | ❌ |
| Activate mods dialog (save-game asset resolution) | ✅ `ActivateModsDialog` | ❌ | ❌ |
| Disable proxy plugin dialog | ✅ `DisableProxyPluginDialog` | ❌ | ❌ |

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
| Headless launcher (CLI) | ❌ | 🚀 `cli::HeadlessLauncher` (`HeadlessLauncher::Config`) | 🚀 |
| Proton version discovery | ❌ | 🚀 `find_proton()`, `enumerate_proton_versions()` | 🚀 |
| Wine binary discovery | ❌ | 🚀 `find_wine()` | 🚀 |
| Admin elevation check | ❌ | 🚀 `is_elevated()` | 🚀 |
| Symlink/junction capability check | ❌ | 🚀 `symlinks_available()` / `junctions_available()` | 🚀 |
| Environment variable management (get/set/path) | ✅ `env::get()`, `env::set()`, `env::path()` | ❌ | ❌ |
| PATH manipulation helpers (append/prepend/set) | ✅ `env::appendToPath()`, `prependToPath()` | ❌ | ❌ |

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

## 43. CLI Command System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| CLI crashdump command | ✅ `cl::CrashDumpCommand` (dump running MO process) | ❌ | ❌ |
| CLI launch command (spawn-wait) | ✅ `cl::LaunchCommand` (CreateProcessW + WaitForSingleObject) | ❌ | ❌ |
| CLI run command (executable with USVFS) | ✅ `cl::RunCommand` (-e name, -a args, -c cwd) | ❌ | ❌ |
| CLI reload-plugin command | ✅ `cl::ReloadPluginCommand` (hot-reload by name) | ❌ | ❌ |
| CLI download-file command | ✅ `cl::DownloadFileCommand` (URL + metadata, HTTPS validation) | ❌ | ❌ |
| CLI refresh command (F5 equivalent) | ✅ `cl::RefreshCommand` | ❌ | ❌ |
| CLI --help | ✅ `CommandLine::showHelp()` | ❌ | ❌ |
| CLI --multiple (allow multiple instances) | ✅ `CommandLine` --multiple flag | ❌ | ❌ |
| CLI --pick (show instance selector) | ✅ `CommandLine` --pick flag | ❌ | ❌ |
| CLI --logs (duplicate logs to stdout) | ✅ `CommandLine` --logs flag | ❌ | ❌ |
| CLI -i (instance selection) | ✅ `CommandLine` -i flag | ❌ | ❌ |
| CLI -p (profile selection) | ✅ `CommandLine` -p flag | ❌ | ❌ |

---

# Summary

[↑ Back to the top ↑](#feature-map---mo2-vs-gmm)

| Category | Matched ✅ | Partial ⚠️ | Surpasses 🚀 | Missing ❌ |
|----------|-----------|-----------|-------------|-----------|
| Virtual Filesystem | 8 | 2 | 5 | 2 |
| Launch Pipeline | 13 | 1 | 6 | 8 |
| Error Handling | 11 | 1 | 0 | 40 |
| Settings | 23 | 3 | 2 | 30 |
| Executable Management | 12 | 1 | 3 | 4 |
| Mod Management | 20 | 0 | 5 | 10 |
| Mod Categories | 6 | 0 | 0 | 4 |
| Mod Conflict Detection | 4 | 0 | 3 | 6 |
| Mod Content Analysis | 5 | 1 | 0 | 5 |
| Mod Info Dialog | 10 | 1 | 1 | 2 |
| Version & Update Management | 4 | 0 | 2 | 8 |
| Plugin Management | 18 | 3 | 4 | 10 |
| LOOT Integration | 3 | 1 | 1 | 14 |
| Profile Management | 16 | 1 | 3 | 10 |
| Download Management | 10 | 1 | 5 | 35 |
| Nexus Integration | 6 | 3 | 2 | 7 |
| Source Providers | 4 | 0 | 7 | 0 |
| Mod List Features | 19 | 0 | 4 | 6 |
| Mod Context Menu | 13 | 2 | 0 | 2 |
| Plugin Context Menu | 1 | 0 | 0 | 6 |
| Archive & Installation | 6 | 0 | 4 | 4 |
| Deploy System | 0 | 0 | 12 | 0 |
| Overwrite System | 6 | 0 | 3 | 0 |
| Save Game System | 10 | 2 | 5 | 1 |
| Game Detection & Knowledge | 2 | 0 | 10 | 0 |
| Pipeline System | 0 | 0 | 16 | 0 |
| Plugin Host System | 0 | 0 | 12 | 0 |
| Sort System | 0 | 0 | 4 | 0 |
| Instance Management | 2 | 0 | 6 | 3 |
| UI Layer | 21 | 0 | 17 | 4 |
| Log System | 5 | 0 | 3 | 5 |
| System Tray | 0 | 4 | 0 | 0 |
| Self Updater | 2 | 0 | 0 | 4 |
| Multi-Process / IPC | 2 | 0 | 1 | 4 |
| Text Editor | 0 | 0 | 0 | 2 |
| Browser | 0 | 0 | 0 | 3 |
| Dialogs | 7 | 0 | 0 | 8 |
| Platform Abstraction | 2 | 1 | 12 | 2 |
| Theme System | 0 | 0 | 3 | 0 |
| Event System | 0 | 0 | 4 | 0 |
| External Tool System | 0 | 0 | 3 | 0 |
| Packaging | 2 | 1 | 3 | 0 |
| CLI Command System | 0 | 0 | 0 | 12 |
| **TOTAL** | **248** | **37** | **163** | **451** |
