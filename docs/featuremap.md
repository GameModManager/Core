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
| MO2 parity  | 287/1055 |   27%   |
|GMM surpasses|   174  |   16%   |
| Missing ⚠️  |   136  |   13%   |
| Missing ❌  |   458  |   43%   |

[Jump to **summary**](#summary)

---

## 1. Virtual Filesystem

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| USVFS controller (dll loading) | ✅ `usvfs_x64.dll` | ❌ UNPROVEN (was: `UsvfsLibrary` - absent; `usvfs_mapping.h:3` is a stub, Windows USVFS = ticket Workspace-3br4, `launcher.cpp:841`) | ❌ |
| VFS create/reset | ✅ `usvfsCreateVFS` | ❌ UNPROVEN (was: `UsvfsConnector` - symbol absent repo-wide) | ❌ |
| Directory-level virtual links | ✅ `usvfsVirtualLinkDirectoryStatic` | ❌ UNPROVEN (was: `UsvfsConnector::updateMapping` - symbol absent repo-wide) | ❌ |
| File-level virtual links | ✅ `usvfsVirtualLinkFile` | ❌ | ❌ |
| Priority-ordered mod mapping | ✅ `OrganizerCore::fileMapping` | ✅ `instance_utils.cpp:362` (staging lowerdir; priority deploy `deploy_utils.cpp:644`) | ✅ |
| Create-target (write destination) | ✅ `LINKFLAG_CREATETARGET` | ✅ `overlay_launcher.cpp:118` (upper dir) + `launcher.h:53` (bind-mount target) | ✅ |
| Custom overwrite target | ✅ `customOverwrite` param | ✅ `fs_utils.h:230` `relay_output_to_mod` (MO2 Custom Overwrites parity, `fs_utils.h:214`) | ✅ |
| Local saves redirect | ✅ `LocalSavegames::mappings` | ✅ `overlay_launcher.cpp:358` (bind-mount install) | ✅ |
| Plugin file-mapper mappings | ✅ `IPluginFileMapper::mappings` | ✅ `plugin_loader.cpp:1202` `cb_v2_register_file_mapper` + `file_mapper_registry.h:25` | ✅ |
| VFS auto-mapping (BSA-aware) | ✅ `DirectoryEntry::addFromBSA` | ❌ | ❌ |
| Archive load order injection | ✅ `enabledArchives` priority | ⚠️ `archives.txt` write only (`profile_switching.cpp:76`), no dynamic injection | ⚠️ |
| Forced library loading | ✅ `usvfs::setForcedLibraries` | ⚠️ preserved in profile copy only (`profile_creation.cpp:199`), no runtime loading | ⚠️ |
| OverlayFS (Linux) | ❌ | 🚀 `overlay_launcher.h:14` `OverlayFsLauncher` | 🚀 |
| LD_PRELOAD intercept (Linux) | ❌ | 🚀 `preload_interceptor.h:23` `PreloadInterceptor` | 🚀 |
| Case-insensitive path resolution | ❌ | 🚀 `path_resolver.h:34` + `fs_utils.h:80` `resolve_regular_file_ci` | 🚀 |
| PathResolver registry | ❌ | 🚀 `path_resolver_registry.h:25` `PathResolverRegistry` (per-root cache) | 🚀 |
| FUSE-based VFS (Linux) | ❌ | 🚀 `vfs.h:14` `Vfs` (FUSE + `file_map_`) | 🚀 |

## 2. Launch Pipeline

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Hooked process creation | ✅ `usvfsCreateProcessHooked` | ❌ UNPROVEN (was: `UsvfsLauncher::launch` - symbol absent; Windows USVFS path stubbed `launcher.cpp:841`, ticket Workspace-3br4) | ❌ |
| Plain process creation | ✅ `CreateProcessW` | ✅ `runtime.cpp:69` (execvp) + `windows_platform.cpp:175` (CreateProcessW) | ✅ |
| Process monitoring (Job Object) | ✅ `CreateJobObjectW` | ❌ UNPROVEN (was: `UsvfsProcessMonitor` - symbol and `CreateJobObjectW` both absent) | ❌ |
| Process monitoring (cgroup v2) | ❌ | 🚀 `launcher.cpp:811` `cgroup_is_empty` + subreaper | 🚀 |
| Exponential backoff | ✅ 50ms-2s | ❌ UNPROVEN (was: `UsvfsProcessMonitor` - absent; only network backoff exists `network_manager.cpp:888`) | ❌ |
| Interesting process selection | ✅ `findInterestingProcessInTrees` | ❌ UNPROVEN (was: `isHiddenProcess` + `Interest` enum - both absent) | ❌ |
| Hidden process filtering | ✅ `conhost.exe` + MO2 exe | ❌ UNPROVEN (was: `conhost.exe` + GMM exe - no such filter found) | ❌ |
| Cancel / force-unlock | ✅ `UILocker::Session` | ✅ Unlock button `launch_controller.cpp:1655` | ✅ |
| Exit code capture | ✅ `GetExitCodeProcess` | ✅ `launch_controller.cpp:829` | ✅ |
| Wait-for-all on app exit | ✅ `waitForAllUSVFSProcessesWithLock` | ❌ | ❌ |
| Process tree descendant walk | ✅ | ✅ `launcher.cpp:689` `get_process_descendants()` (PPID chain) | ✅ |
| Steam -- set SteamAPPId | ✅ `env::set("SteamAPPId", ...)` | ⚠️ only Proton path sets `STEAM_COMPAT_APP_ID` (`runtime.cpp:184`), no native SteamAPPId env | ⚠️ |
| Steam -- auto-start | ✅ `checkSteam` + registry `SteamExe` | ❌ | ❌ |
| Steam -- elevation mismatch | ✅ `canAccess` + admin dialog | ⚠️ `is_elevated()` generic admin check `windows_platform.cpp:187`, not Steam-specific | ⚠️ |
| Steam -- Proton/Wine compat | ❌ | 🚀 `runtime.cpp:181` `STEAM_COMPAT_*` env | 🚀 |
| Proton tooling (winetricks/protontricks) | ❌ | 🚀 `proton_tools.cpp:122` `run_proton_tool()` fallback chain | 🚀 |
| PATH manipulation | ✅ `env::appendToPath` | ❌ UNPROVEN (was: `appendToPath` - symbol absent repo-wide) | ❌ |
| CWD resolution | ✅ `Executable::workingDirectory` | ✅ `launcher.cpp:315` `weakly_canonical` + fallback | ✅ |
| Virtualized binary in mods/ | ✅ `adjustForVirtualized` | ❌ | ❌ |
| File type dispatch (.bat, .jar) | ✅ `getFileExecutionContext` | ❌ | ❌ |
| Java detection for .jar | ✅ `findJavaInstallation` | ❌ | ❌ |
| CREATE_BREAKAWAY_FROM_JOB | ✅ | ❌ UNPROVEN (was: `UsvfsLauncher` - absent, no BREAKAWAY handling) | ❌ |
| Subreaper + supervisor | ❌ | 🚀 `launcher.cpp:193` `PR_SET_CHILD_SUBREAPER` | 🚀 |
| Wine runtime (non-Steam Windows exe) | ❌ | 🚀 `wine_runtime.h:13` `WineRuntime` | 🚀 |
| LaunchParams (structured) | ✅ `SpawnParameters` | ✅ `launcher.h:12` `LaunchParams` (pid, overlay, cgroup, capture) | ✅ |
| File association lookup | ✅ `env::getAssociation()` | ❌ | ❌ |
| Steam-related error dialogs | ✅ `badSteamReg()`, `startSteamFailed()`, `confirmStartSteam()` | ❌ | ❌ |
| U032 Exit confirm while downloads in progress ("Downloads in progress" dialog, pauseAll, wait-for-processes w/ cancel) | ✅ `mainwindow.cpp:1450-1470` | ❌ | ❌ |
| U170 UILocker dialog messages (locked/running/output text; Unlock / Exit Now / Cancel) | ✅ `uilocker.cpp:295-349` | ❌ | ❌ |
| U235 Env vars set/read (SteamAPPId, STEAM_USERNAME/PASSWORD blacklist, USVFS_*, MO2 vars) | ✅ `settingsdialogworkarounds.cpp:17-30` + `usvfsconnector.cpp` | ⚠️ only Proton/Steam-compat env `runtime.cpp:184`; no STEAM credentials or USVFS_* passthrough | ⚠️ |
| U270 SteamUtility (steam process/VDF helpers) | ✅ `steamutility.cpp` (uibase) | ⚠️ VDF/ACF parsing only `game_detector.h:34`; no steam process helpers | ⚠️ |
| U284 processrunner (run w/ w/o VFS, waiting UI, cancel, exit code, runApplication API) | ✅ `processrunner.cpp` | ⚠️ cancel + exit code proven `launch_controller.cpp:1655` + `launch_controller.cpp:829`; per-call waiting dialogs unproven | ⚠️ |

## 3. Error Handling & Diagnostics

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| ERROR_INVALID_PARAMETER (AV quarantine) | ✅ `makeContent` | ❌ UNPROVEN (was: `describe_usvfs_error` - symbol absent repo-wide) | ❌ |
| ERROR_ACCESS_DENIED (AV blocking) | ✅ `makeContent` | ❌ UNPROVEN (was: `describe_usvfs_error` - symbol absent repo-wide) | ❌ |
| ERROR_FILE_NOT_FOUND (exe missing) | ✅ `makeContent` | ❌ UNPROVEN (was: `describe_usvfs_error` - symbol absent repo-wide) | ❌ |
| ERROR_DIRECTORY (bad cwd) | ✅ `makeContent` | ❌ UNPROVEN (was: `describe_usvfs_error` - symbol absent repo-wide) | ❌ |
| ERROR_ELEVATION_REQUIRED (admin restart) | ✅ `confirmRestartAsAdmin` + `helper.exe` | ❌ UNPROVEN (was: "error mapped, no restart flow" - no elevation-required mapping found) | ❌ |
| makeDetails (owner, ACL, DLL presence) | ✅ | ❌ | ❌ |
| Blacklist warning dialog | ✅ `confirmBlacklisted` | ❌ | ❌ |
| Crash dump type selection | ✅ `CrashDumpsType` | ✅ `settings.cpp:520` `core_dump_type()` + UI `settings_content_widget.cpp:1347` | ✅ |
| USVFS child crash capture | ✅ `usvfsCreateMiniDump` | ⚠️ UNPROVEN (was: "SEH + usvfs integration"); self MiniDumpWriteDump only `crash_handler.cpp:126`, no child/USVFS capture | ⚠️ |
| Crash dump pruning | ✅ `cycleDiagnostics` | ✅ `core.cpp:116` `prune_old_dumps(max_core_dumps())` | ✅ |
| USVFS log worker thread | ✅ `LogWorker` (QThread) | ❌ UNPROVEN (was: `std::thread` - Logger is synchronous, no worker thread `logger.h`) | ❌ |
| USVFS log file output | ✅ `logs/usvfs-<ts>.log` | ❌ UNPROVEN (was: `logs/usvfs-<ts>.log` - actual log is `gamemodmanager.log` `core.cpp:64`) | ❌ |
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
| Process elevation detection | ✅ `WindowsInfo::isElevated()` | ✅ `windows_platform.cpp:187` `is_elevated()` | ✅ |
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
| Diagnostics settings tab | ✅ `DiagnosticsSettingsTab` (log level, dump type, max dumps) | ✅ `settings_content_widget.cpp:1324` `build_diagnostics_tab()` | ✅ |
| U033 Crash-on-exit dialog ("MO crashed while exiting. Some settings may not be saved.") | ✅ `mainwindow.cpp:645-650` | ❌ | ❌ |
| U193 UILocker exit flow (canExit download/VFS sequence) | ✅ `uilocker.cpp`, `mainwindow.cpp:1450` | ❌ | ❌ |
| U238 IPluginDiagnose (activeProblems -> ProblemsDialog count + Fix) | ✅ `mainwindow.cpp:1031-1054` | ⚠️ diagnose registry exists `plugin_loader.cpp:16` (diagnose_registry); Problems dialog itself absent (see section 46) | ⚠️ |
| U268 Report/reportError global error popup | ✅ `report.cpp` (uibase) | ❌ | ❌ |
| U272 ErrorCodes shared error code mapping | ✅ `errorcodes.cpp` (uibase) | ❌ | ❌ |
| U273 DiagnosisReport diagnose report formatting | ✅ `diagnosisreport.cpp` (uibase) | ❌ | ❌ |

## 4. Settings & Configuration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Executables blacklist | ✅ `Settings::executablesBlacklist` | ✅ `settings_content_widget.cpp:1292` `executables_blacklist()` UI | ✅ |
| Skip file suffixes | ✅ `Settings::skipFileSuffixes` | ⚠️ `settings.h:184` + UI `settings_content_widget.cpp:1275`, no scanner consumer | ⚠️ |
| Skip directories | ✅ `Settings::skipDirectories` | ⚠️ `settings.h:186` + UI `settings_content_widget.cpp:1276`, no consumer | ⚠️ |
| Force load libraries | ✅ `ExecutableForcedLoadSetting` | ⚠️ preserved in profile copy only (`profile_creation.cpp:199`) | ⚠️ |
| USVFS log level | ✅ `Settings::logLevel` | ✅ `settings.h:196` `log_level()` applied `core.cpp:170` + UI tab | ✅ |
| USVFS crash dump type | ✅ `Settings::coreDumpType` | ✅ `settings.cpp:520` `core_dump_type()` + UI tab | ✅ |
| USVFS spawn delay | ✅ `Settings::spawnDelay` | ❌ UNPROVEN (was: "data model only" - `spawn_delay` absent repo-wide) | ❌ |
| Geometry persistence | ✅ `GeometrySettings` (window, splitter, toolbar) | ✅ `mod_info_dialog.cpp:329` `saveGeometry()`/`restoreGeometry()` | ✅ |
| Widget state persistence | ✅ `WidgetSettings` (tree expand, combo, tab index) | ✅ `settings_controller.cpp:789` `saveState()`/`restoreState()` (splitters, headers) | ✅ |
| Color settings (conflict coloring) | ✅ `ColorSettings` (8+ color options) | ✅ `settings_content_widget.cpp:353` QColorDialog + `settings.h:215` conflict colors | ✅ |
| Plugin blacklist | ✅ `Settings::blacklisted` | ⚠️ executables blacklist exists (`settings_content_widget.cpp:1292`), no plugin-specific blacklist | ⚠️ |
| Network settings (proxy, offline mode) | ✅ `NetworkSettings` | ✅ `settings_content_widget.cpp:1227` offline_mode + `:1239` custom_browser, proxy bridge `network_options_bridge.cpp:7` | ✅ |
| Splash screen | ✅ `Settings::useSplash` | ❌ | ❌ |
| Prerelease updates toggle | ✅ `Settings::usePrereleases` | ✅ `settings_content_widget.cpp:113` `use_prereleases()` UI toggle | ✅ |
| Low-priority extraction | ✅ | 🚀 `pipeline_worker.cpp:199` `extraction_low_priority()` | 🚀 |
| Full UI mode (tabs vs popups) | ❌ | 🚀 `settings_content_widget.cpp:120` `full_ui_mode()` | 🚀 |
| Multi-core processing toggle | ❌ | 🚀 `settings.h:111` `performance/enable_multicore` + `parallel.h:31` gate | 🚀 |
| Language selection (i18n picker) | ✅ `InterfaceSettings::language()` | ❌ | ❌ |
| Style/Theme selection (QStyle + .qss) | ✅ `InterfaceSettings::styleName()` | ❌ | ❌ |
| Collapsible separators settings | ✅ `InterfaceSettings` (ascending, descending, highlight, icons) | ✅ `settings.h:35` `collapsible_separators_*` + UI `settings_content_widget.cpp:493` | ✅ |
| Save filters toggle | ✅ `InterfaceSettings::saveFilters()` | ✅ `settings_content_widget.cpp:459` `save_filters()` UI | ✅ |
| Auto-collapse on hover | ✅ `InterfaceSettings::autoCollapseOnHover()` | ✅ `settings_content_widget.cpp:462` `auto_collapse_on_hover()` UI | ✅ |
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
| Force enable core files | ✅ `GameSettings::forceEnableCoreFiles()` | ✅ `Settings::force_enable_core_files()` `settings.cpp:478`, setting `settings_content_widget.cpp:1286` | ✅ |
| Base directory variable (%BASE_DIR%) | ✅ `PathSettings::BaseDirVariable` | ❌ | ❌ |
| Recent directories | ✅ `PathSettings::recent()` | ❌ | ❌ |
| Offline mode | ✅ `NetworkSettings::offlineMode()` | ✅ `settings_content_widget.cpp:1227` + `network_options_bridge.cpp:16` | ✅ |
| Custom browser command | ✅ `NetworkSettings::customBrowserCommand()` | ✅ `settings_content_widget.cpp:1239` `custom_browser_command()` | ✅ |
| Download speed tracking per server | ✅ `NetworkSettings::setDownloadSpeed()` | ❌ | ❌ |
| Server preference list | ✅ `NetworkSettings::servers()` | ❌ | ❌ |
| Nexus endorsement integration setting | ✅ `NexusSettings::endorsementIntegration()` | ✅ `settings.h:161` + applied `nexus_source_panel.cpp:41` | ✅ |
| Nexus tracked integration setting | ✅ `NexusSettings::trackedIntegration()` | ✅ `settings.h:163` + applied `nexus_source_panel.cpp:46` | ✅ |
| Nexus category mappings setting | ✅ `NexusSettings::categoryMappings()` | ✅ `settings.h:165` `category_mappings()` | ✅ |
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
| Color separator scrollbar | ✅ `ColorSettings::colorSeparatorScrollbar()` | ✅ `settings.h:204` + gate `mod_list_model.cpp:125` | ✅ |
| Toolbar state persistence | ✅ `GeometrySettings::saveToolbars()` / `restoreToolbars()` | ✅ `settings_controller.cpp:789` `saveState()` / `:876` `restoreState()` | ✅ |
| Dock state persistence | ✅ `GeometrySettings::saveDocks()` / `restoreDocks()` | ❌ | ❌ |
| Widget visibility persistence | ✅ `GeometrySettings::saveVisibility()` / `restoreVisibility()` | ❌ | ❌ |
| Remember question dialog buttons | ✅ `WidgetSettings::QuestionBoxMemory` | ❌ | ❌ |
| Tree expand/check state persistence | ✅ `WidgetSettings::saveTreeCheckState` / `saveTreeExpandState` | ❌ | ❌ |
| Tab widget index persistence | ✅ `WidgetSettings::saveIndex(QTabWidget)` | ❌ | ❌ |
| Combobox index persistence | ✅ `WidgetSettings::saveIndex(QComboBox)` | ❌ | ❌ |
| Checkable button state persistence | ✅ `WidgetSettings::saveChecked(QAbstractButton)` | ❌ | ❌ |
| Tab-based settings dialog (8 tabs) | ✅ `settingsdialog.cpp` (General, Theme, ModList, Paths, Diagnostics, Nexus, Plugins, Workarounds) | ✅ `settings_content_widget.cpp:68` `tabs_->addTab(...)` multi-tab | ✅ |
| Settings change logging | ✅ `settingsutilities.h` `logChange()` | ❌ | ❌ |
| Color table (visual color picker with delegates) | ✅ `colortable.h/cpp` | ❌ | ❌ |
| Mod info tab order persistence | ✅ `GeometrySettings::modInfoTabOrder()` | ❌ | ❌ |
| Center on main window monitor | ✅ `GeometrySettings::centerOnMainWindowMonitor()` | ❌ | ❌ |
| U006 Ctrl+S = Settings shortcut | ✅ `mainwindow.ui:1736-1756` | ✅ `menu_bar.cpp:62` (QKeySequence::Preferences) | ✅ |
| U040 Alt key reveals hidden menubar (showMenubarOnAlt, suppressed while UILocker locked) | ✅ `mainwindow.cpp:4052-4070` | ❌ | ❌ |
| U044 Restart-after-settings dialog (Restart / Continue variants) | ✅ `mainwindow.cpp:2768-2780` | ❌ | ❌ |
| U046 Network proxy activation progress dialog | ✅ `mainwindow.cpp:2115-2140` | ❌ | ❌ |
| U047 Downgrade notice after version drop | ✅ `mainwindow.cpp:2219` | ❌ | ❌ |
| U061 dataTabShowFromArchives gated on archiveParsing setting | ✅ `mainwindow.cpp:532-542` | ❌ | ❌ |
| U129 General > Language group (languageBox + "Help translate" LinkLabel) | ✅ `settingsdialog.ui:68-124` | ❌ | ❌ |
| U130 General > Download List group (4 checkboxes + MODL associate button) | ✅ `settingsdialog.ui:125-197` | ⚠️ compact-downloads checkbox exists `settings_content_widget.cpp:428`; other 3 options + associate button unproven | ⚠️ |
| U133 General > Miscellaneous checkboxes (center dialogs, instance-change confirm, Alt menubar, previews on double-click) | ✅ `settingsdialog.ui:264-325` | ❌ | ❌ |
| U134 General buttons (Reset Dialog Choices, Configure Mod Categories) | ✅ `settingsdialog.ui:343-372` | ⚠️ categories dialog exists `settings/categories_dialog.h:21`; the two settings buttons unproven | ⚠️ |
| U138 Paths tab (7 path rows + %BASE_DIR% hint + writability footer) | ✅ `settingsdialog.ui:846-1054` | ❌ | ❌ |
| U139 Paths error strings (create failed, invalid game install) | ✅ `settingsdialogpaths.cpp:100-101`, `:236-237` | ❌ | ❌ |
| U140 Nexus settings tab full page (account, statistics, connection, options, servers groups) (NEXUS-LENS: genericize/provider-scope) | ✅ `settingsdialog.ui:1056-1504` | ⚠️ Sources tab stands in `settings_content_widget.cpp:839` (queue/hide-counter/manual-key `source_pages.cpp:271`, `:463`); account/statistics/servers groups unproven | ⚠️ |
| U142 Nexus custom browser picker file dialog | ✅ `settingsdialognexus.cpp:500-510` | ❌ | ❌ |
| U143 Settings > Plugins tab (plugin details, Enabled 3-state tooltip, settings table, blacklist) | ✅ `settingsdialog.ui:1506-1746` | ❌ | ❌ |
| U145 Workarounds options (force-enable game files, archives parsing, lock GUI) | ✅ `settingsdialog.ui:1810-1860` | ⚠️ force-enable + archive parsing checkboxes `settings_content_widget.cpp:1286`, `:1288`; lock-GUI unproven | ⚠️ |
| U146 Workarounds > Steam group (AppID/username/password + log blacklist) | ✅ `settingsdialog.ui:1864-1928`, `settingsdialogworkarounds.cpp:17-30` | ❌ | ❌ |
| U147 Workarounds > Network group (offline mode, system proxy, custom browser) | ✅ `settingsdialog.ui:1931-2010` | ⚠️ offline mode + custom browser `settings_content_widget.cpp:1227`, `:1238`; system-proxy option unproven | ⚠️ |
| U148 Workarounds buttons (Reset Geometries, Back-date BSAs, Executables Blacklist, Skip Suffixes/Directories) | ✅ `settingsdialog.ui:2035-2144`, `settingsdialogworkarounds.cpp:96-200` | ⚠️ workarounds tab present `settings_content_widget.cpp:1292`; the multiline dialogs unproven | ⚠️ |
| U149 Workarounds footer warning text | ✅ `settingsdialog.ui:2187` | ❌ | ❌ |
| U150 Diagnostics tab controls (log level, crash dumps, max dumps, LOOT log level, links) | ✅ `settingsdialog.ui:2197-2320`, `settingsdialogdiagnostics.cpp:22-90` | ⚠️ diagnostics tab exists `settings_content_widget.cpp:1324` (log-level combo `settings.cpp:504`); dump/LOOT controls unproven | ⚠️ |
| U151 Settings tabs use scroll areas with grouped GroupBoxes | ✅ `settingsdialog.ui:28`, `:1062` | ❌ | ❌ |
| U223 Per-plugin translators (every loaded plugin file basename) | ✅ `mainwindow.cpp:~545`, `:2897-2911` | ❌ | ❌ |
| U262 QuestionBoxMemory per-dialog choice persistence (.ui + IDs) | ✅ `questionboxmemory` (uibase) | ❌ | ❌ |
| U263 FileDialogMemory::restore (remembers dir per named dialog) | ✅ `mainwindow.cpp:506` | ❌ | ❌ |
| U287 splash.png + useSplash display component | ✅ `src/splash.png` | ❌ | ❌ |

## 5. Executable Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Custom executables list | ✅ `ExecutablesList` (CRUD) | ✅ `executables_entry.h:26` `Entry` + `exec_controls_bar.h:31` `ExecControlsBar` | ✅ |
| Per-executable arguments | ✅ `Executable::arguments` | ✅ `executables_entry.h:30` `Entry::arguments` (+ UI `executables_content_widget.cpp:313`) | ✅ |
| Per-executable working directory | ✅ `Executable::workingDirectory` | ✅ `executables_content_widget.cpp:317` `start_in_edit_` (`Entry::start_in`) | ✅ |
| Per-executable Steam App ID | ✅ `Executable::steamAppID` | ⚠️ MO2 importer reads `steamAppID` (`mo2_importer.cpp:220`), not per-executable runtime | ⚠️ |
| Per-executable custom overwrite | ✅ `Executable::customOverwrites` | ❌ | ❌ |
| Per-executable forced libraries | ✅ `Executable::forcedLibraries` | ❌ | ❌ |
| Per-executable environment variables | ❌ | 🚀 `executables_content_widget.cpp:45` `parse_environment_text` (KEY=VALUE) | 🚀 |
| Per-executable output-to-mod routing | ❌ | 🚀 `executables_content_widget.cpp:119` `output_mod` load + `:324` combo | 🚀 |
| Toolbar pinning | ✅ `ShowInToolbar` flag | ✅ `main_window.cpp:229` `add_shortcut_to_toolbar` | ✅ |
| Desktop shortcut creation | ❌ | 🚀 `main_window.cpp:232` `add_shortcut_to_desktop` (.desktop file) | 🚀 |
| Executable ordering (up/down) | ✅ `EditExecutablesDialog` | ✅ `executables_content_widget.cpp:268` up/down + `:289` InternalMove drag | ✅ |
| Clone executable | ✅ `EditExecutablesDialog::clone()` | ✅ `executables_content_widget.cpp:276` `on_clone_selected` | ✅ |
| JAR binary detection | ✅ `setJarBinary` + `findJavaInstallation` | ❌ | ❌ |
| Icon extraction (wrestool/QFileIconProvider) | ❌ | 🚀 `exec_controls_bar.cpp:111` `extractExeIcon` | 🚀 |
| Executable editor widget | ✅ `EditExecutablesDialog` | ✅ `executables_entry.h:84` `class ContentWidget` (mode-agnostic) | ✅ |
| Executables list proxy model | ✅ `ExecutablesListProxy` | ❌ | ❌ |
| U004 Ctrl+E = Executables... | ✅ `mainwindow.ui:1697-1717` | ❌ | ❌ |
| U016 Run menu (one action per pinned exe, statusTip path, custom__ objectName, hidden if none) | ✅ `mainwindow.cpp:755-795` | ⚠️ exec controls bar exists `exec_controls_bar.cpp:224`; Run menu itself unproven | ⚠️ |
| U022 Toolbar context menu "Remove '%1' from the toolbar" | ✅ `mainwindow.cpp:3785-3805` | ❌ | ❌ |
| U024 Link button menu (Toolbar and Menu / Desktop / Start Menu shortcuts) | ✅ `mainwindow.cpp:360-367`, `:2695-2719` | ⚠️ desktop shortcut exists `launch_controller.cpp:1340` (add_shortcut_to_desktop); Toolbar-and-Menu/Start-Menu variants unproven | ⚠️ |
| U051 Executables combo sentinels ("<Edit...>", "(no executables)") | ✅ `mainwindow.cpp:1866-1920` | ✅ `exec_controls_bar.h:22` kAddNewEntryText = "<Edit...>" | ✅ |
| U069 Pinned exe toolbar actions (iconForExecutable, statusTip, custom__ objectName) | ✅ `mainwindow.cpp:769-795` | ⚠️ pinned toolbar action wiring `main_window.cpp:229`; icon/statusTip/objectName trio unproven | ⚠️ |
| U155 File tree "Enter Name" add-as-executable dialog | ✅ `filetree.cpp:280-301` | ❌ | ❌ |
| U156 EditExecutables list context menu (Add from file / Add empty / Clone selected) | ✅ `editexecutablesdialog.cpp:91-99` | ⚠️ "Clone selected" exists `executables_content_widget.cpp:276`; Add-from-file/Add-empty unproven | ⚠️ |
| U188 EditExecutablesDialog full control set (list, buttons, fields, Steam AppID override) | ✅ `editexecutablesdialog.ui` | ⚠️ executables content widget exists `executables_content_widget.cpp:276`; MO2's exact control set unproven | ⚠️ |
| U189 EditExecutables validation dialogs (empty output mod, reset confirm, Java required) | ✅ `editexecutablesdialog.cpp:214-887` | ❌ | ❌ |
| U277 ExecutableInfo PE parsing of binaries | ✅ `executableinfo.cpp` (uibase) | ❌ | ❌ |

## 6. Mod Management

Note: Track B item U200 is skipped - it is an alias of U116 (integrated once in this section).

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod priority ordering | ✅ `profile.getActiveMods` | ✅ `mod_meta.cpp:484` `ModMeta::set_priority` + `mod_cache.cpp:125` | ✅ |
| Mod enable/disable | ✅ `ModInfo::enabled` | ✅ `mod_scanner.cpp:786` `enable_mod` / `:778` `disable_mod` | ✅ |
| Overwrite directory | ✅ `Settings::paths().overwrite` | ✅ `instance.h:61` `overwrite_dir` + `instance_utils.cpp:306` create | ✅ |
| Custom overwrite target | ✅ `customOverwrite` param | ✅ `fs_utils.h:230` `relay_output_to_mod` (MO2 Custom Overwrites parity) | ✅ |
| Mod data-to-game mapping | ✅ `getModMappings` | ✅ `knowledge_` keys `debug_window.cpp:881` `knowledge_->get` | ✅ |
| Mod metadata | ✅ `ModInfo` | ✅ `mod_meta.h` (`ModMeta`) + `category_set_registry.h:16` | ✅ |
| Core category sets | ❌ | 🚀 `category_set_registry.h:16` `CategorySetRegistry` + plugin hook `register_set` `category_set_registry.cpp:14` | 🚀 |
| Plugin-contributed categories | ❌ | 🚀 `plugin_loader.cpp:191` `cb_register_categories` -> `Category::Factory::merge` (was cited: `IPluginCategoryFactory`, symbol does not exist) | 🚀 |
| Mod file tree / conflict display | ✅ `DirectoryEntry` | ✅ `filetree/file_tree.h:3` + `conflict_engine.cpp:341` | ✅ |
| BSA/archive extraction | ✅ `BSAExtractor` | ✅ `archive_extractor.cpp:79` | ✅ |
| Case-insensitive mod matching | ✅ USVFS handles it | 🚀 `path_resolver.h:34` `PathResolver` | 🚀 |
| Mod cache | ✅ | ✅ `mod_cache.h:20` `ModCache` (SQLite) | ✅ |
| Mod scanner | ✅ | ✅ `mod_scanner.cpp:434` | ✅ |
| Mod renaming | ✅ `ModList::renameMod` | ✅ `mod_actions.cpp:573` `rename_mod_inline` | ✅ |
| Mod notes | ✅ `ModInfo::notes()` | ✅ `notes_tab.cpp:24` `NotesTab` (comments + HTML notes + color) | ✅ |
| Mod comments | ✅ `ModInfo::comments()` | ✅ `notes_tab.cpp:28` `comments_` + `:88` `on_comments_edited` | ✅ |
| Mod color coding | ✅ `ModInfo::color()` | ✅ `notes_tab.cpp:36` Set/Reset color; sidecar `color` key merged on scan, `mod_scan_worker.cpp:364` | ✅ |
| Mod author/uploader metadata | ✅ `ModInfo::author()`, `uploader()` | ✅ `plugin_database.cpp:338` `GamePlugin::author` from ESP CNAM (`esp_header.cpp:74`) | ✅ |
| Mod description | ✅ `ModInfo::getDescription()` | ✅ `plugin_database.cpp:339` `GamePlugin::description` + BBCode pipeline `bbcode.cpp` | ✅ |
| Mod creation time | ✅ `ModInfo::creationTime()` | ✅ `mod_scanner.cpp:434` `install_time`/`changed_time` | ✅ |
| Mod internal name | ✅ `ModInfo::internalName()` | ❌ | ❌ |
| Mod validated flag | ✅ `ModInfo::markValidated` | ✅ `mod_scanner.cpp:853` `mark_validated` | ✅ |
| Mod repository tracking | ✅ `ModInfo::repository()` | ✅ `mod_list_controller.cpp:1927` `meta.source_type()` / `source_id` | ✅ |
| Plugin settings per mod | ✅ `ModInfoRegular::pluginSetting` | ✅ `settings.h:239` `plugin_setting()` + `plugin_settings_registry.h:43` | ✅ |
| Nexus file IDs tracking | ✅ `ModInfoRegular::installedFiles` | ✅ `download_nxm.file_id` (`modpack_install_wizard.cpp:291`) + meta.ini persistence `gmmpack/packer.cpp:194` (no `InstalledFileInfo` class) | ✅ |
| Mod tags (deprecated/note/warning/incompatible) | ❌ | 🚀 `mod_list_model.h:22` `struct ModTag` + `set_tags` `:209` | 🚀 |
| Visual nesting (parent_id, indent, fold) | ❌ | 🚀 `mod_list_model.cpp:156` indent + `:1590` `set_folded` | 🚀 |
| Vendor icons (per-source badges) | ❌ | 🚀 `mod_list_model.cpp:46` (nexusmods, loverslab, steam, moddb) | 🚀 |
| MERGED pseudo-mod | ❌ | 🚀 `mod_list_model.h:17` `kMergedModId` | 🚀 |
| Game-native mod band | ❌ | 🚀 `mod_list_model.h:291` `native_band_first/last` | 🚀 |
| ModInfoForeign (non-official plugins) | ✅ `ModInfoForeign` | ❌ | ❌ |
| ModInfoSeparator | ✅ `ModInfoSeparator` | ❌ | ❌ |
| ModInfoWithConflictInfo (conflict data) | ✅ `ModInfoWithConflictInfo` | ❌ | ❌ |
| Hidden file extension detection | ✅ `ModInfo::s_HiddenExt` | ❌ | ❌ |
| getByName/getByIndex/getByModID accessors | ✅ `ModInfo::getByName()` etc. | ❌ | ❌ |
| Import strategy (Merge/Overwrite/None) | ✅ `ImportStrategy` enum | ❌ | ❌ |
| Activate mods dialog (save-game asset resolution) | ✅ `ActivateModsDialog` | ❌ | ❌ |
| U055 5s periodic timer saving mod metas (saveModMetas) | ✅ `mainwindow.cpp:500-504` | ❌ | ❌ |
| U116 Rename toasts ("Invalid name", "Name is already in use by another mod") | ✅ `modlist.cpp:490-496` | ❌ | ❌ |
| U117 Mod remove confirmation dialog | ✅ `modlist.cpp:1233-1234` | ❌ | ❌ |
| U201 meta.ini full key set enumerated (~29 keys incl. Endorsed/Abstained, tracked, nexus*) | ✅ `modinfo.cpp:80-330`, `modinforegular.cpp:957` | ⚠️ mod_meta covers core keys `mod_meta.cpp:117` (nexuscategory), `:124` (repository), `:736` (modid); full 29-key set unproven | ⚠️ |
| U202 meta.ini version / newestVersion / ignoredVersion keys | ✅ `modinforegular.cpp` | ⚠️ newest-version plumbing exists `nexus_source_panel.cpp:255` + version group `mod_meta.h:130`; ignoredVersion unproven | ⚠️ |
| U203 meta.ini modId / fileId / repository / gameName keys | ✅ `modinforegular.cpp` | ⚠️ modid + repository handled `mod_meta.cpp:736`, `:297`; fileId/gameName unproven | ⚠️ |
| U204 meta.ini category (primary) / categories CSV | ✅ `modinfo.cpp` | ✅ `engine/mod/meta/mod_meta.cpp:739` (category=0 write; categories CSV parse) | ✅ |
| U205 meta.ini comments / notes / color (hex) | ✅ `modinfo.cpp` | ⚠️ meta store exists `mod_meta.cpp:292` (General-section get/set pattern `:297`); comments/notes/color accessors unproven | ⚠️ |
| U281 DirectoryRefresher progress + error signal ("Loading..." progress) | ✅ `directoryrefresher.cpp`, `mainwindow.cpp:2441-2460` | ⚠️ progress signal exists `engine/core/directory_refresher.h:95`; error signal unproven | ⚠️ |

## 7. Mod Categories

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Category tree system | ✅ `Categories` (hierarchical) | ✅ `category_factory.h:15` `Category::Factory` + `load` `category_factory.cpp:33` | ✅ |
| Nexus category mapping | ✅ `NexusCategory` + `resolveNexusID` | ✅ `mod_meta.cpp:117` `nexuscategory` key applied `mod_list_controller.cpp:1996` (no `NexusCat` type) | ✅ |
| Category import/export | ✅ `CategoryImportDialog` | ❌ | ❌ |
| Multi-category assignment | ✅ `ModInfo::setCategories` | ✅ `category_ids` CSV `[General]` (`mod_list_model.h:65`) + `set_category_ids` `mod_list_model.h:218` | ✅ |
| Primary category | ✅ `ModInfo::primaryCategory` | ✅ first entry in category CSV (`mod_list_model.h:65`), `mod_list_controller.cpp:2055` | ✅ |
| Special filter categories | ✅ `SpecialCategories` (Checked, UpdateAvailable, Conflict, etc.) | ❌ | ❌ |
| Category CRUD editor | ✅ | ✅ `categories_dialog.h:24` `CategoriesDialog` (editable table, full CRUD) | ✅ |
| Category filter panel | ✅ | ✅ `category_filter_panel.h:21` `CategoryFilterPanel` (checkable tree) | ✅ |
| Category import dialog (Merge/Overwrite/None strategy) | ✅ `CategoryImportDialog` | ❌ | ❌ |
| Categories table view | ✅ `CategoriesTable` | ❌ | ❌ |
| U029 Category setup: GMM ruling = download Nexus category mappings per instance (opt-in import); NO MO2 3-button first-run chooser (user ruling) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1281-1300` | ⚠️ opt-in Nexus category-mapping import via existing NexusCat infra `settings.cpp:418` + `source_pages.cpp:345` (intentionally not wired yet) | ⚠️ |
| U030 Category migration dialog (Import Nexus Categories / Open Dialog / Disable Mappings / Close + Don't show again) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1312-1345` | ❌ | ❌ |
| U076 Category menus as QPushButton-with-menu (addMenuAsPushButton) | ✅ `modlistcontextmenu.cpp:264-271` | ❌ | ❌ |
| U078 "Change Categories" menu (recursive checkable items, parent check icon, aboutToHide apply) | ✅ `modlistcontextmenu.cpp:110-156` | ⚠️ add_category_menus exists `mod_context_menu.cpp:459`; parent check-icon + aboutToHide apply unproven | ⚠️ |
| U079 "Primary Category" menu (QRadioButton per assigned category) | ✅ `modlistcontextmenu.cpp:180-215` | ⚠️ radio primary-category menu exists `mod_context_menu.cpp:541` (QActionGroup `:549`); aboutToHide apply unproven | ⚠️ |
| U112 Category column "Non-MO" for foreign + auto-unset on removal | ✅ `modlist.cpp:222-244` | ❌ | ❌ |
| U126 Separator display strips "_separator" suffix | ✅ `modlist.cpp:110-122` | ❌ | ❌ |
| U171 Categories dialog (Refresh from Nexus, import column, drag-assign pane) (NEXUS-LENS: genericize/provider-scope) | ✅ `categoriesdialog.ui` | ⚠️ categories dialog CRUD exists `settings/categories_dialog.h:21`; Nexus refresh/drag-assign pane unproven | ⚠️ |
| U172 Category import dialog (Merge/Replace strategy + mapping options) | ✅ `categoryimportdialog.ui` | ❌ | ❌ |
| U196 "No category found" install dialog (Proceed / Disable / Stop && Configure) (NEXUS-LENS: genericize/provider-scope) | ✅ `installationmanager.cpp:672-682` | ❌ | ❌ |
| U229 Category menu commits on aboutToHide | ✅ `modlistcontextmenu.cpp:145-151` | ❌ | ❌ |

## 8. Mod Conflict Detection

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Loose file conflicts | ✅ `EConflictFlag::OVERWRITE` | ✅ `conflict_engine.cpp:341` | ✅ |
| Archive vs loose conflicts | ✅ `FLAG_ARCHIVE_LOOSE_CONFLICT_*` | ✅ four conflict color settings `settings.h:216` (overwritten/overwriting x loose/archive) | ✅ |
| Archive vs archive conflicts | ✅ `FLAG_ARCHIVE_CONFLICT_*` | ❌ | ❌ |
| Overwrite folder conflicts | ✅ `FLAG_OVERWRITE_CONFLICT` | ❌ | ❌ |
| Conflict dialog (general) | ✅ `GeneralConflictsTab` with counters | ❌ | ❌ |
| Conflict dialog (advanced) | ✅ `AdvancedConflictsTab` tree view | ❌ | ❌ |
| Conflict context menu | ✅ open/run hooked/preview/explore/hide/goto | ✅ `conflicts_tab.cpp:182` `on_custom_context_menu` (ImageDiff `:201`) | ✅ |
| Conflict highlighting | ✅ `EHighlight` (INVALID, CENTER, IMPORTANT, PLUGIN) | ✅ row tint `mod_list_model.cpp:137` + `conflict_highlight_color` `mod_list_model.cpp:2022` + scrollbar marks | ✅ |
| Per-mod conflict stats | ❌ | 🚀 `mod_list_controller.cpp:2256` `set_conflict_stats(wins, losses)` | 🚀 |
| Blake2b fingerprint cache | ❌ | 🚀 `conflict_index.h:92` blake2b + `conflict_index.cpp:33` `scan(db_path)` | 🚀 |
| Image diff (conflict comparison) | ❌ | 🚀 `main_window.cpp:283` `ConflictsTab::image_diff_requested` | 🚀 |
| ConflictListModel / ConflictItem | ✅ `ConflictListModel`, `ConflictItem` | ❌ | ❌ |
| Data tab conflict mode | ✅ `dataTabShowOnlyConflicts` | ❌ | ❌ |
| U094 Conflict flag tooltip texts (9 exact strings + FLAG_OVERWRITE_CONFLICT) | ✅ `modlist.cpp:156-180`, `modinfo.h:70-81` | ⚠️ conflict column plumbing `mod_list_model.cpp:345`; exact 9 tooltip strings unproven | ⚠️ |
| U119 EConflictFlag enum (10 values: loose/archive/archive-loose + overwrite) | ✅ `modinfo.h:70-81` | ⚠️ conflict-scan result plumbing `mod_list_controller.h:84` (ConflictScanResult); 10-flag enum unproven | ⚠️ |
| U120 EHighlight bit values (NONE/INVALID/CENTER/IMPORTANT/PLUGIN) | ✅ `modinfo.h:99-105` | ❌ | ❌ |

## 9. Mod Content Analysis

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod data content types | ✅ `ModDataContentHolder` | ✅ `ModContentId` enum `game_feature.h:154` | ✅ |
| Mod flags (INVALID, BACKUP, SEPARATOR, etc.) | ✅ `EFlag` | ⚠️ `ModState` is pipeline state (not an MO2-style flag enum); per-mod flags set at `mod_scanner.cpp:604` | ⚠️ |
| Mod content icons | ✅ `ModContentIconDelegate` | ❌ | ❌ |
| Mod conflict icons | ✅ `ModConflictIconDelegate` | ✅ `FlagsDelegate` (per-icon tooltips) `mod_table_view.h:89` | ✅ |
| Mod flag icons | ✅ `ModFlagIconDelegate` | ✅ `FlagsDelegate` (wrap + tooltips) `mod_table_view.h:49` | ✅ |
| Empty mod flag (dummy icon, tooltip) | ✅ | ✅ `ModList::set_empty` + `plugin-dummy` icon + tooltip `mod_list_model.cpp:39`, `mod_list_model.cpp:1327` | ✅ |
| Mod version delegate (color-coded) | ✅ `ModListVersionDelegate` | ❌ | ❌ |
| INI tweaks detection | ✅ `ModInfo::getIniTweaks()` | ⚠️ profile-level initweaks.ini `profile_switching.cpp:36` + plugin INI tooltip `plugin_view.cpp:246` | ⚠️ |
| Archive listing per mod | ✅ `ModInfo::archives()` | ✅ `archives_html()` in plugin tooltip `plugin_view.cpp:90`, `plugin_view.cpp:248` | ✅ |
| ModDataContent updated signal | ✅ `ModDataContentUpdated` | ❌ | ❌ |
| CombinedModDataContent | ✅ `CombinedModDataContent` | ❌ | ❌ |
| U042 BSA list context menu "Extract..." | ✅ `mainwindow.cpp:3720-3728` | ❌ | ❌ |
| U065 BSA extract errors (read/extract failures, invalid hashes warning) | ✅ `mainwindow.cpp:3614-3715` | ❌ | ❌ |
| U093 Flag tooltip texts (8 exact strings incl. different-game + tracked warnings) | ✅ `modlist.cpp:124-153` | ⚠️ per-icon flag tooltip plumbing `mod_table_view.h:93` (flag_tooltips_role); exact strings unproven | ⚠️ |
| U104 Mod content column per-icon tooltip (helpEvent -> contentsTooltip table) | ✅ `modcontenticondelegate.cpp:40-56`, `modlist.cpp:1376` | ❌ | ❌ |
| U118 EFlag enum (11 flags incl. PLUGIN_SELECTED, ALTERNATE_GAME, TRACKED) | ✅ `modinfo.h:84-97` | ⚠️ flag icon/tooltip delegate roles `mod_table_view.h:96` (FlagsDelegate); 11-flag enum unproven | ⚠️ |
| U121 Flag->emblem icon map (7 mapped, 4 without icons, unknown warns) | ✅ `modflagicondelegate.cpp:47-80` | ⚠️ flag-icon delegate roles `mod_table_view.h:49`; exact 7-map + unknown-warning unproven | ⚠️ |
| U122 Flag delegate returns zero icons for FLAG_OVERWRITE rows | ✅ `modflagicondelegate.cpp:20-24` | ❌ | ❌ |
| U123 Flag icon sizeHint = count*40 x 20 clamped | ✅ `modflagicondelegate.cpp:90-110` | ❌ | ❌ |
| U125 ModInfoRegular flag conditions (NOTENDORSED, TRACKED, INVALID, NOTES, PLUGIN_SELECTED, ALTERNATE_GAME, HIDDEN_FILES) | ✅ `modinforegular.cpp:689-711` | ⚠️ endorsement/tracked gating `nexus_source_panel.cpp:41` + hidden-file flags `data_tab_build_worker.h:49`; full 7-condition matrix unproven | ⚠️ |
| U128 Generic icon + no-edit delegates (beyond version delegate) | ✅ `modlistversiondelegate.cpp`, `genericicondelegate.cpp`, `noeditdelegate.cpp` | ❌ | ❌ |
| U161 Data tab checkboxes + tooltips (conflicts/archives/hidden filters, Refresh tip) | ✅ `mainwindow.ui:1095-1195` | ⚠️ refresh + status-tips exist `data_tab.cpp:830` (tip `:833`); the three filter-checkbox tooltips unproven | ⚠️ |
| U211 markConverted (converted/working flag) | ✅ `modlistviewactions` | ❌ | ❌ |
| U212 validated flag (ignore missing data) | ✅ `modinfo` | ✅ `engine/game/detect/mod_scanner.cpp:853` mark_validated writes [General] validated=true (read `:599`) | ✅ |
| U213 hidden files list (restoreHiddenFiles) | ✅ `modinforegular` | ❌ | ❌ |
| U218 FileTree menu ordering + bold-first-enabled via doubleClicksOpenPreviews | ✅ `filetree.cpp:749-772` | ❌ | ❌ |

## 10. Mod Info Dialog

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| File tree tab | ✅ `ModInfoDialogFileTree` | ✅ `filetree_tab` `filetree_tab.cpp:117` | ✅ |
| ESP/plugin tab | ✅ `ModInfoDialogEsps` | ✅ `esps_tab` (registered `mod_info_dialog.cpp:8-14`) | ✅ |
| Nexus tab (embedded page) | ✅ `ModInfoDialogNexus` (endorse/track) | ✅ `source_tab` (registered `mod_info_dialog.cpp:8-14`) | ✅ |
| Images tab (gallery, DDS) | ✅ `ModInfoDialogImages` (380 lines) | ✅ `images_tab` (registered `mod_info_dialog.cpp:8-14`) | ✅ |
| Text files tab | ✅ `ModInfoDialogTextFiles` | ✅ `text_files_tab` `text_files_tab.cpp:16` | ✅ |
| INI files tab | ✅ `IniFilesTab` | ✅ `config_files_tab` (registered `mod_info_dialog.cpp:8-14`) | ✅ |
| Categories tab | ✅ `ModInfoDialogCategories` | ✅ `categories_tab` `categories_tab.cpp:83` | ✅ |
| Conflicts tab | ✅ `ModInfoDialogConflicts` | ✅ `conflicts_tab` `conflicts_tab.cpp:182` | ✅ |
| Notes tab (comments + notes + color) | ✅ `ModInfoNotesTab` | ✅ `notes_tab` `notes_tab.cpp:24` | ✅ |
| Generic files tab | ❌ | 🚀 `generic_files_tab` `generic_files_tab.cpp:1` | 🚀 |
| Tab reordering | ✅ `onTabMoved` + `saveTabOrder` | ⚠️ internal `tab_order_` vector `mod_info_dialog.cpp:58`, not user-draggable | ⚠️ |
| Tab color coding (data presence) | ✅ `setTabsColors` | ❌ | ❌ |
| Mod navigation (prev/next) | ✅ `onPreviousMod`, `onNextMod` | ✅ `prev_btn_`, `next_btn_`, separator-aware nav `mod_info_dialog.cpp:99`, `mod_info_dialog.cpp:159` | ✅ |
| Mod info tab order persistence | ✅ `GeometrySettings::modInfoTabOrder()` | ❌ | ❌ |

## 11. Version & Update Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Version checking (installed vs newest) | ✅ `ModInfo::updateAvailable` | ✅ `ModUpdateDbClient::has_update()` `mod_update_db_client.h:150` + `newestVersion` in meta | ✅ |
| Newest version tracking | ✅ `ModInfo::newestVersion()` | ✅ `ModInfoResult::newest_version` `provider.h:16` + `nexusnewestversion` in meta `nexus_source_panel.cpp:255` | ✅ |
| Ignored version | ✅ `ModInfo::ignoredVersion()` | ❌ | ❌ |
| Downgrade detection | ✅ `ModInfo::downgradeAvailable()` | ❌ | ❌ |
| Batch update check | ✅ `checkAllForUpdate` | ❌ | ❌ |
| Check update after install | ✅ `Settings::checkUpdateAfterInstallation` | ❌ | ❌ |
| ModUpdateDbClient (per-game DB poll, ETag/304, offline cache) | ❌ | 🚀 `ModUpdateDbClient` (ISO 8601 compare, `by_game` index) `mod_update_db_client.h:124`, `mod_update_db_client.h:56`, `mod_update_db_client.h:17` | 🚀 |
| Per-game index pipeline (by_game directory, manifest) | ❌ | 🚀 `ModUpdateDbClient` per-game index + shard lookup `mod_update_db_client.h:164` | 🚀 |
| GitHub releases API (not Nexus) | ✅ `SelfUpdater` queries GitHub API | ❌ | ❌ |
| Prerelease filtering | ✅ `selfupdater.cpp` filters by draft/prerelease flags | ✅ `self_updater.cpp` filters by prerelease `self_updater.cpp:107` | ✅ |
| Update dialog with Markdown changelogs | ✅ `UpdateDialog` (version diff + release notes) | ❌ | ❌ |
| MOTD (Message of the Day) | ✅ `motdAvailable` signal | ❌ | ❌ |
| U064 Update check dialogs (no-recent-updates + rate-limit notices) | ✅ `mainwindow.cpp:3151-3170` | ❌ | ❌ |
| U207 meta.ini nexus timestamps (nexusLastModified/nexusLastQuery/lastNexus*) | ✅ `modinforegular.cpp` | ⚠️ mod_meta [Nexusmods] section exists `mod_meta.h:32`; the 4 timestamp keys unproven | ⚠️ |
| U209 versioningScheme (changeVersioningScheme) | ✅ `modlistviewactions changeVersioningScheme` | ❌ | ❌ |
| U210 ignoreUpdate flag (setIgnoreUpdate) | ✅ `modlistviewactions setIgnoreUpdate` | ❌ | ❌ |
| U254 finishUpdateInfo batch flow (per-mod newest versions, update-all) | ✅ `mainwindow.cpp:3151-3310` | ❌ | ❌ |
| U271 GuessedValue/Versioning version comparison rules | ✅ `versioning.cpp` (uibase) | ❌ | ❌ |

## 12. Plugin Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Plugin list | ✅ `plugins.txt` | ✅ `plugin_database` `plugin_database.h:18` | ✅ |
| Load order sorting (LOOT) | ✅ `LOOT` | ✅ `gmm_lootcli` `mod_list_controller.cpp:2909` | ✅ |
| Plugin enable/disable | ✅ | ✅ `plugin_database.cpp:719` | ✅ |
| ESP header parsing | ✅ `ESPInfo` | ✅ `esp_header` `esp_header.cpp:1` | ✅ |
| Plugin diagnostics | ✅ | ✅ `diagnose_registry` `diagnostics_registry.cpp:10` | ✅ |
| Plugin file mapper | ✅ `IPluginFileMapper` | ✅ `file_mapper_registry` `file_mapper_registry.cpp:1` | ✅ |
| Plugin save parser | ✅ | ✅ `save_parser_registry` `save_parser_registry.cpp:8` | ✅ |
| Plugin requirements check | ✅ | ✅ `requirements_registry` `requirements_registry.h:42`, registered at `plugin_loader.cpp:962` | ✅ |
| Plugin order encoding | ✅ | ✅ `order_encoding_registry` `order_encoding_registry.h:25` | ✅ |
| Master/Light/Medium/Blueprint flags | ✅ `isMasterFlagged`, `isLightFlagged`, etc. | ⚠️ master, light, medium implemented `plugin_info.h:52-54`; no Blueprint flag (explicitly unsupported `plugin_view.cpp:228-231`) | ⚠️ |
| Missing masters detection | ✅ `testMasters` + `missingMasters` | ✅ `set_missing_masters()` `mod_list_controller.cpp:2865` + tooltip + emblem `plugin_view.cpp:183` | ✅ |
| Plugin lock (pin at position) | ✅ `isESPLocked`, `lockESPIndex` | ✅ `set_locked()` `mod_list_controller.cpp:3069` + `apply_locked_order()` `plugin_database.cpp:871` + lockedorder.txt `profile.h:63` | ✅ |
| Plugin relationship fix | ✅ `fixPluginRelationships` | ❌ | ❌ |
| Plugin priority shift | ✅ `shiftPluginsPriority` | ❌ | ❌ |
| Plugin send to priority | ✅ `sendToPriority` | ⚠️ `send_to_highest/lowest_priority()` exists `mod_actions.cpp:234`, `mod_actions.cpp:249`, no arbitrary position | ⚠️ |
| Enable/disable all plugins | ✅ `setEnabledAll` | ✅ `set_all_enabled()` `mod_list_controller.cpp:2864` | ✅ |
| Plugin index generation (FE/FD) | ✅ `generatePluginIndexes` | ✅ `generate_mod_indexes()` `mod_list_controller.cpp:2869` | ✅ |
| LOOT messages per plugin | ✅ `Plugin::messages` | ✅ `LootReport::messages` `plugin_info.h:23` rendered in tooltip `plugin_view.cpp:187` | ✅ |
| LOOT dirty info (ITM, deleted refs) | ✅ `Dirty` struct | ✅ `LootReport::DirtyEntry` (ITM/deleted refs/navmesh) `plugin_info.h:24-31` rendered `plugin_view.cpp:195` | ✅ |
| LOOT incompatibilities | ✅ `Plugin::incompatibilities` | ✅ `LootReport::incompatibilities` `plugin_info.h:17` rendered `plugin_view.cpp:178` | ✅ |
| LOOT stats (time, version) | ✅ `Stats` | ❌ | ❌ |
| Plugin author/description display | ✅ `pluginlist.h` | ✅ `GamePlugin::author`/`description` in tooltip HTML `plugin_info.h:78-79` | ✅ |
| Plugin FormVersion/HeaderVersion | ✅ `formVersion`, `headerVersion` | ✅ `esp_header` rendered `plugin_view.cpp:292`, `plugin_view.cpp:297` | ✅ |
| Plugin archive loading detection | ✅ `loadsArchive` | ⚠️ `GamePlugin::archives` `plugin_info.h:83` + `archives_html()` `plugin_view.cpp:248`, implicit detection | ⚠️ |
| Drag-and-drop plugin reorder | ✅ `dropMimeData` | ✅ `PluginTable` `plugin_view.cpp:389` with `InternalMove` `plugin_view.cpp:486` + `on_reorder` `plugin_view.cpp:490` | ✅ |
| Plugin foreground coloring (LOOT-based) | ✅ `foregroundData()` | ✅ state-based `setForeground()` for locked/missing/master `plugin_view.cpp:576`, `plugin_view.cpp:580` | ✅ |
| Plugin tooltip data (LOOT messages) | ✅ `tooltipData()` | ✅ `plugin_tooltip_html()` with full sub-blocks `plugin_view.cpp:271`, `plugin_view.cpp:610` | ✅ |
| Plugin highlight from mod selection | ✅ `highlightPlugins()` | ✅ `set_contained_plugins()` / `set_master_plugins()` `plugin_view.cpp:795`, `plugin_view.cpp:800` | ✅ |
| Transitive master enable/disable | ❌ | 🚀 `set_enabled()` enables/disables masters (transitive closure) `plugin_database.cpp:719`, `plugin_database.cpp:732` | 🚀 |
| Band reassertion (native+CC invariant) | ❌ | 🚀 `reassert_band()` `plugin_database.cpp:460` | 🚀 |
| Locked order application | ❌ | 🚀 `apply_locked_order()` `plugin_database.cpp:871` | 🚀 |
| Plugin type classification | ✅ | ✅ Regular/Master/Light/Medium enum `plugin_view.h:85` | ✅ |
| Plugin counter (by type) | ✅ `ModCounters` | ✅ `plugins_tab` active/total breakdown `plugins_tab.cpp:67` | ✅ |
| Zero-record plugin dummy icon | ✅ | ✅ `plugin-dummy` icon for HEDR record count == 0 `plugin_view.cpp:356` | ✅ |
| Plugin list sort proxy | ✅ `pluginlistsortproxy.h` (column filtering, custom sorting) | ❌ | ❌ |
| Plugin list highlight masters | ✅ `PluginList::highlightMasters()` | ❌ | ❌ |
| Plugin list ChangeBracket (RAII layout notifications) | ✅ `PluginList::ChangeBracket` | ❌ | ❌ |
| Plugin list view (filter, counter, keyboard nav) | ✅ `pluginlistview.h/cpp` | ❌ | ❌ |
| Plugin list context menu | ✅ `pluginlistcontextmenu.h/cpp` (enable/disable, send-to, lock, open origin) | ❌ | ❌ |
| Plugin list model (metadata, type flags) | ✅ `pluginlist.h` (form/header version, author, description) | ❌ | ❌ |
| U043 BSA enabled-in-INI warning | ✅ `mainwindow.cpp:2071` | ❌ | ❌ |
| U059 Filter shortcut wiring also on espList + downloadView | ✅ `mainwindow.cpp:495-497` | ❌ | ❌ |
| U090 Plugin tooltip full block (every field incl. Loads Archives/INI, ESL/ESH, blueprint, dummy, force-disabled) | ✅ `pluginlist.cpp:1492-1662` | ⚠️ plugin_tooltip_html exists `plugin_view.cpp:271`; MO2's full field list unproven | ⚠️ |
| U107 Plugin list 8 columns (Name..Description) | ✅ `pluginlist.h:92-101`, `pluginlist.cpp:91-107` | ⚠️ 5 of 8 columns proven `plugin_view.cpp:471` + `tab_panels.cpp:523-530`; Mod Index/Form Version/Header Version missing | ⚠️ |
| U114 Plugin header tooltips (8 exact strings) | ✅ `pluginlist.cpp:115-133` | ⚠️ header tooltips now exist `right_panel.cpp:218` (audit G9); exact 8 strings unproven | ⚠️ |
| U197 Plugin invalid-names warning + Workarounds plugin settings dialogs | ✅ `gamebryogameplugins.cpp:131` | ❌ | ❌ |
| U198 PluginList reportError strings (5 exact) | ✅ `pluginlist.cpp:296`, `:513`, `:843`, `:857`, `:2041` | ❌ | ❌ |
| U220 Check-BSA single-shot debounce timer | ✅ `mainwindow.cpp:~497` | ❌ | ❌ |
| U251 BSA list + extract flow (SortableTreeWidget, progress, errors) | ✅ `mainwindow.cpp:1922-2090`, `:3668-3730` | ❌ | ❌ |
| U293 OBJ Group / LCD counters / managedArchiveLabel hover QToolTip | ✅ `mainwindow.cpp:3942-3945` | ❌ | ❌ |

## 13. LOOT Integration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| LOOT sort execution | ✅ `Loot::sort()` | ✅ `Sorter::Loot` `mod_list_controller.cpp:2909` | ✅ |
| LOOT report generation | ✅ `Loot::createReport()` | ⚠️ `loot_report.json` (JSON only, no HTML/markdown) `sorter.cpp:26` | ⚠️ |
| LOOT report viewer (markdown + web) | ✅ `LootDialog` + `MarkdownDocument` | ❌ | ❌ |
| LOOT progress display | ✅ `LootDialog::setProgress()` | ✅ `on_loot_progress(int stage, QString)` signal `mod_list_controller.cpp:2973` | ✅ |
| LOOT dirty info details | ✅ `Dirty` (CRC, ITM, deleted refs, navmesh, utility) | ⚠️ `LootReport::DirtyEntry` has ITM/deleted refs/navmesh/utility but NO CRC field `plugin_info.h:24-31` | ⚠️ |
| LOOT incompatibilities details | ✅ `File` (name + displayName) | ✅ `LootReport::incompatibilities` (name + displayName) `plugin_info.h:17` rendered `plugin_view.cpp:178` | ✅ |
| LOOT missing masters | ✅ `Plugin::missingMasters` | ✅ `missing_masters_html()` in plugin tooltip `plugin_view.cpp:183` | ✅ |
| Masterlist manager (GitHub walk-down) | ✅ | 🚀 `MasterlistManager` (branch walk, 24h TTL, offline fallback) `masterlists.h:21`, `masterlists.cpp:65` | 🚀 |
| LOOT sorted plugin list application | ✅ `lootdialog.cpp` applySortedLoadOrder() | ✅ `apply_load_order(result.sorted_names)` after sort `mod_list_controller.cpp:3001` | ✅ |
| LOOT sorted plugin list Markdown rendering | ✅ `loot.h` getSortedPluginListMarkdown() | ❌ | ❌ |
| LOOT clean info display | ✅ `Plugin::clean` vector ("Verified clean by X") | ✅ `LootReport::clean` `plugin_info.h:32-35` rendered `plugin_view.cpp:209` | ✅ |
| LOOT plugin flags (loadsArchive, isMaster, isLightMaster) | ✅ `Plugin` struct per-plugin metadata | ✅ `is_master_flagged`/`is_light_flagged`/`is_medium_flagged` `plugin_info.h:52-54` + `archives` `plugin_info.h:83` | ✅ |
| LOOT cancel/terminate | ✅ `Loot::cancel()` terminates lootcli process | ❌ | ❌ |
| LOOT statistics (timing, versions) | ✅ `Stats` struct (execution time, lootcli version) | ❌ | ❌ |
| LOOT general messages | ✅ `Report::messages` (non-plugin-specific) | ✅ rendered `plugin_view.cpp:162` (messages_ul_html), loop `plugin_view.cpp:187` (not yet written to DB) | ✅ |
| LOOT PluginList integration (addLootReport) | ✅ `PluginList::addLootReport()` integrates LOOT data | ✅ `set_loot_reports()` side-map `mod_list_controller.cpp:3011-3012`, `plugin_database.h:192` | ✅ |
| LOOT log level setting | ✅ `DiagnosticsSettings::lootLogLevel()` | ❌ | ❌ |
| LOOT async pipe communication | ✅ `AsyncPipe` (Windows Named Pipe IPC) | ❌ | ❌ |
| U054 sortButton LOOT tooltip variants | ✅ `mainwindow.cpp:3064-3075` | ❌ | ❌ |
| U091 makeLootTooltip strings ("Incompatible with %1", missing-dep li, Warning/Error prefixes) | ✅ `pluginlist.cpp:1665-1700` | ✅ `plugin_view.cpp:181` (LOOT tooltip strings) | ✅ |

## 14. Profile Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Profile creation | ✅ `Profile` | ✅ `profile_creation` `profile_creation.h:48` `create_fresh_profile` | ✅ |
| Profile switching | ✅ | ✅ `profile_switching` `profile_switching.cpp:109` `switch_profile()` | ✅ |
| Local saves per profile | ✅ `localSavesEnabled` | ✅ `local_saves` `local_saves.h:51` `resolve_local_saves()` + UI `profile_settings_widget.cpp:15` | ✅ |
| Mod order persistence | ✅ | ✅ `do_write_modlist()` `profile.cpp:512` + priority map `profile.cpp:485` | ✅ |
| Plugin order persistence | ✅ | ✅ `save_profile()` `plugin_database.cpp:1076` / `load_profile()` `plugin_database.cpp:936` | ✅ |
| Delayed file writer | ✅ `DelayedFileWriter` | ✅ `delayed_file_writer` `delayed_file_writer.cpp:1` | ✅ |
| Safe write file | ✅ | ✅ `safe_write_file` `safe_write_file.cpp:50` | ✅ |
| Local INI settings | ✅ `Profile::localSettingsEnabled` | ✅ `ProfileManager::local_settings()` `profile.cpp:357` + UI checkbox `profile_settings_widget.cpp:16` | ✅ |
| Profile INI tweaks | ✅ `Profile::getProfileTweaks` | ✅ `write_tweaked_ini()` `profile_switching.cpp:34` + `initweaks.ini` `profile_switching.cpp:36` | ✅ |
| Archive invalidation toggle | ✅ `Profile::invalidationActive` | ✅ `automatic_archive_invalidation()` `profile.cpp:360` + UI `profile_settings_widget.cpp:47` + BSA feature | ✅ |
| Profile locking (plugin order) | ✅ `Profile::getLockedOrderFileName` | ✅ `read_locked_order()` `profile.cpp:543` / `write_locked_order()` `profile.cpp:560` + `apply_locked_order()` `plugin_database.cpp:871` | ✅ |
| Profile transfer saves | ✅ `TransferSavesDialog` | ❌ | ❌ |
| Profile rename | ✅ `Profile::rename` | ✅ `engine::profile::rename_profile()` `profile_creation.cpp:213` | ✅ |
| Profile copy | ✅ `Profile::createPtrFrom` | ✅ `engine::profile::copy_profile()` `profile_creation.cpp:154` | ✅ |
| Profile forced libraries | ✅ `Profile::determineForcedLibraries` | ⚠️ preserved in profile copy only `profile_creation.cpp:199`, no runtime loading | ⚠️ |
| Profile switch result + callbacks | ❌ | 🚀 `ProfileSwitchResult` `profile_switching.h:22` + `ProfileSwitchCallbacks` `profile_switching.h:52` | 🚀 |
| Profile switch EventBus emission | ❌ | 🚀 `kProfileChanged` event `event_bus.h:41`, dispatched `profile_switching.cpp:204` | 🚀 |
| Profile bar (combo + folder shortcuts) | ❌ | 🚀 `ProfileBar` `profile_bar.h:28` (FolderKind enum `profile_bar.h:14`, export/import `profile_bar.cpp:107-109`) | 🚀 |
| Profile deletion | ✅ `profilesdialog.cpp` on_removeProfileButton_clicked() | ✅ `on_delete_profile()` with confirmation + active guard `profile_manager_dialog.cpp:229` | ✅ |
| Active profile protection (cannot rename/delete active) | ✅ `profilesdialog.cpp` | ✅ `profile_manager_dialog.cpp` active-profile guard `profile_manager_dialog.cpp:204`, `profile_manager_dialog.cpp:234` | ✅ |
| Profile creation with default vs copy choice | ✅ `ProfileInputDialog` (getPreferDefaultSettings) | ✅ `ProfileCreateDialog` copy-source combo ("(fresh)" + existing profiles) `profile_create_dialog.h:10-25` | ✅ |
| Mod priority management per profile | ✅ `Profile::setModPriority()`, `getModPriority()`, `setModsEnabled()` | ✅ `set_mod_priority()` `profile.h:185` + `priority_of()` `profile.h:177` + enable/disable `mod_scanner.cpp:786` | ✅ |
| Mod status signal | ✅ `Profile::modStatusChanged` signal | ✅ EventBus `kModStateChanged` `event_bus.h:36`, dispatched `mod_list_controller.cpp:994` | ✅ |
| Tweaked INI creation | ✅ `Profile::createTweakedIniFile()` | ✅ `write_tweaked_ini()` `profile_switching.cpp:34` | ✅ |
| Profile settings as arbitrary key/value | ✅ `Profile::setting()`, `storeSetting()`, `settingsByGroup()` | ✅ `get_setting()` / `set_setting()` arbitrary key/value over settings.ini `profile.h:225-226` | ✅ |
| Rename mod across all profiles | ✅ `Profile::renameModInAllProfiles()` | ❌ | ❌ |
| Active mods retrieval | ✅ `Profile::getActiveMods()` | ❌ | ❌ |
| Profile existence check | ✅ `Profile::exists()` | ✅ `Profile::exists()` `profile.h:101` | ✅ |
| Profile find settings (auto-detect) | ✅ `Profile::findProfileSettings()` | ✅ `detect_local_settings()` in profile_creation `profile_creation.cpp:38` | ✅ |
| Modlist write cancellation | ✅ `Profile::cancelModlistWrite()` | ✅ `cancel_modlist_write()` `profile.cpp:510` (DelayedFileWriter cancel `profile.cpp:244`) | ✅ |
| Profile debug dump | ✅ `Profile::debugDump()` | ✅ `debug_window.cpp` profile display `debug_window.cpp:852-853` | ✅ |
| Profile INI files (full set - 7 file paths) | ✅ `Profile` (plugins.txt, loadorder.txt, lockedorder.txt, modlist.txt, archives.txt, ini, tweaks) | ✅ path accessors `profile.h:212-217` (settings/modlist/plugins/loadorder/lockedorder/archives) + `initweaks.ini` `profile_switching.cpp:36` | ✅ |
| U003 Ctrl+P = Profiles... | ✅ `mainwindow.ui:1676-1696` | ❌ | ❌ |
| U050 profileBox "<Manage...>" sentinel entry opening profiles dialog | ✅ `mainwindow.cpp:1822-1864`, `:2936` | ✅ `profile_bar.cpp:19` kManageProfilesText = "<Manage...>" (sentinel at index 0, `:156`) | ✅ |
| U132 General > Profile Defaults group (Local INIs / Local Saves / Archive Invalidation) | ✅ `settingsdialog.ui:234-261` | ⚠️ per-profile settings editor maps MO2's checkboxes `profile_settings_widget.h:11`; archive-invalidation parity unproven | ⚠️ |
| U166 Profiles dialog full control set (3 checkables + 7 buttons w/ tooltips) | ✅ `profilesdialog.ui` | ⚠️ profile manager dialog port `profile_manager_dialog.h:13` (create/copy/rename/delete + per-profile settings `:45`); Transfer/Select + tooltips unproven | ⚠️ |
| U167 Profiles dialog messages (invalid name, active-profile guards, broken profile) | ✅ `profilesdialog.cpp:78-300` | ❌ | ❌ |
| U173 ProfileInputDialog (name prompt + Default Game INI Settings checkbox) | ✅ `profileinputdialog.ui` | ⚠️ profile create dialog exists `profile_create_dialog.h:10`; the checkbox parity unproven | ⚠️ |
| U214 Profile 7-file set + settingsByGroup arbitrary keys + meta timer | ✅ `profile.cpp` | ⚠️ 7 path accessors proven `profile.h:212-217`; periodic meta-save timer absent (see U055) | ⚠️ |
| U215 Profile initweaks / invalidation archive-list interactions | ✅ `mainwindow.cpp:2585-2603` | ⚠️ initweaks + archives.txt writers `profile_switching.cpp:36`, `:76`; invalidationActive toggling unproven | ⚠️ |
| U221 Archive list saved via DelayedFileWriter on modPrioritiesChanged | ✅ `mainwindow.cpp:564-566` | ❌ | ❌ |

## 15. Download Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Download state machine | ✅ `DownloadState` (14 states) | ✅ `curl_download` states; `downloads_tab.cpp:44` | ✅ |
| Pause/resume downloads | ✅ `pauseDownload`, `resumeDownload` | ✅ `downloads_tab` pause/resume, `downloads_tab.cpp:413`, `:867-871` | ✅ |
| Cancel downloads | ✅ `cancelDownload` | ✅ abort callback, `pipeline.h:160` | ✅ |
| Download speed tracking | ✅ `downloadSpeed` rolling average | ⚠️ live speed only (no rolling average/history), `downloads_tab.cpp:319` | ⚠️ |
| MD5 lookup | ✅ `queryInfoMd5` | ❌ | ❌ |
| Download meta files (sidecar) | ✅ `createMetaFile` | ⚠️ `ModMeta` for mod dirs, not download archive meta, `mod_scan_worker.cpp:351` | ⚠️ |
| Hidden downloads | ✅ `isHidden`, `restoreDownload` | ❌ UNPROVEN (was: show hidden checkbox - only a hide-installed filter exists, `downloads_tab.cpp:122`) | ❌ |
| Automatic retry (3x) | ✅ `AUTOMATIC_RETRIES` | ✅ `NetworkOptions::max_retries` + `retry_backoff_ms` + Retry-After, `network_manager.cpp:887`, `network_options_bridge.cpp:25` | ✅ |
| Pending download queue | ✅ `PendingDownload` | ✅ `queue_controller` deferred queue, `queue_controller.h:13`, `pipeline_worker.h:180` | ✅ |
| Multi-URL fallback | ✅ `m_Urls`, `m_CurrentUrl` | ❌ | ❌ |
| Hide after install | ✅ `hideDownloadsAfterInstallation` | ✅ `settings.h:25`, `downloads_tab.cpp:145`, `:696-704` | ✅ |
| Download notifications | ✅ `showDownloadNotifications` | ❌ | ❌ |
| Compact downloads view | ✅ `compactDownloads` | ✅ `downloads_tab` compact/standard rows, `downloads_tab.cpp:201` | ✅ |
| Drag-and-drop import | ❌ | 🚀 `downloads_tab` drop archive to add, `downloads_tab.cpp:159` | 🚀 |
| Directory watcher (auto-detect) | ❌ | 🚀 `downloads_tab` QFileSystemWatcher, `downloads_tab.cpp:191` | 🚀 |
| Manifest serialization (JSON) | ❌ | 🚀 `downloads_tab` serialize/deserialize, `downloads_tab.cpp:951`, `:975` | 🚀 |
| Content-Disposition filename parsing | ❌ | 🚀 RFC 5987 `filename*` + `percent_decode`, `network_manager.cpp:342` | 🚀 |
| HTTP Range resume | ❌ | 🚀 `curl_download` Range header, `manager.h:37`, `fetch_stage.cpp:73` | 🚀 |
| NXM protocol download handler | ✅ `addNXMDownload()` (nxm:// link processing) | ✅ `downloads_controller.cpp:221` (NxmIpcServer), `nxm_ipc.h` | ✅ |
| Plugin download API (startDownloadURLs, etc.) | ✅ `IDownloadManager` plugin API | ❌ | ❌ |
| Nexus collection link rejection | ✅ `downloadmanager.cpp` detects collection links | ❌ | ❌ |
| Duplicate download detection (file name + auto-rename) | ✅ `downloadmanager.cpp` checks same-name files | ✅ Overwrite/Rename/Ignore conflict resolver, `downloads_tab.cpp:165-184` | ✅ |
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
| Mark installed / mark uninstalled | ✅ `markInstalled()`, `markUninstalled()` | ⚠️ `mark_installed()` only, no mark-uninstalled, `downloads_tab.cpp:397`, `downloads_controller.cpp:151` | ⚠️ |
| Install from download (double-click / context menu) | ✅ `downloadlistview.cpp` install action | ✅ `downloads_tab.cpp:135` (double-click), `:850` (context menu) | ✅ |
| Download status color coding | ✅ `downloadlist.cpp` ForegroundRole colors | ✅ state label colors, `downloads_tab.cpp:377`, `:1105-1120` | ✅ |
| Remaining time estimation | ✅ `downloadmanager.cpp` ETA calculation | ❌ | ❌ |
| Batch delete operations (all/installed/uninstalled) | ✅ `downloadlistview.cpp` issueDeleteAll/Completed/Uninstalled | ❌ | ❌ |
| Batch hide operations + un-hide all | ✅ `downloadlistview.cpp` issueRemoveFromView/RestoreToView | ❌ | ❌ |
| Filter widget for downloads | ✅ `FilterWidget` (fuzzy match) | ⚠️ shared RightFilterBar text filter, not a dedicated fuzzy widget, `downloads_tab.cpp:725` | ⚠️ |
| Meta/display name toggle setting | ✅ `downloadlist.cpp` metaDownloads | ❌ | ❌ |
| Keyboard shortcuts (Enter=install, Delete=remove, Space=pause) | ✅ `downloadlistview.cpp` | ❌ | ❌ |
| Visit on Nexus (from download) | ✅ `downloadmanager.cpp` visitOnNexus() | ❌ | ❌ |
| Visit uploader profile | ✅ `downloadmanager.cpp` visitUploaderProfile() | ❌ | ❌ |
| Plugin download callbacks (onDownloadComplete/Paused/Failed/Removed) | ✅ `IDownloadManager` boost::signals2 | ❌ | ❌ |
| TaskProgress integration (Windows taskbar progress) | ✅ `downloadmanager.cpp` TaskProgressManager | ❌ | ❌ |
| S3 signed URL filename extraction | ✅ `downloadmanager.cpp` response-content-disposition= | ⚠️ generic Content-Disposition parser covers it (same code as row above), `network_manager.cpp:342` | ⚠️ |
| File time fallback chain (birthTime -> metadataChangeTime -> lastModified) | ✅ `downloadmanager.cpp` getFileTime() | ❌ | ❌ |
| U037 Drop-onto-downloads duplicate dialog (Overwrite / Rename / Ignore) | ✅ `mainwindow.cpp:4002-4017` | ✅ `downloads_tab.cpp:165` (duplicate handling on drop) | ✅ |
| U038 dragEnterEvent accepts Copy/Move + supported archives over downloads | ✅ `mainwindow.cpp:3947-3991` | ⚠️ drag-enter acceptance `downloads_tab.cpp:656`; exact extension/URL-reject filter unproven | ⚠️ |
| U039 dropEvent (TargetMoveAction coercion, shellCopy/Move, URL -> startDownloadURLs) | ✅ `mainwindow.cpp:4034-4050` | ⚠️ drop handling exists `downloads_tab.cpp:80`; coercion detail unproven | ⚠️ |
| U045 "Can't change download directory while downloads are in progress" toast | ✅ `mainwindow.cpp:2803` | ❌ | ❌ |
| U062 showHiddenBox toggles downloadManager setShowHidden | ✅ `mainwindow.cpp:3818-3821` | ❌ | ❌ |
| U096 Download row tooltip (filename, info-missing hint, modName version desc, Pending) | ✅ `downloadlist.cpp:214-232` | ❌ | ❌ |
| U097 Download status colors (READY/UNINSTALLED/PAUSED foregrounds) | ✅ `downloadlist.cpp:205-214` | ⚠️ status color mapping exists `downloads_tab.cpp:1105` (audit G24); exact 3-color set unproven | ⚠️ |
| U108 Download list 8 columns (4 hidden by default, ini override) | ✅ `downloadlist.h:40-47`, `downloadlistview.cpp:150-156` | ❌ | ❌ |
| U109 Downloads header right-click per-column checkbox menu (QWidgetAction) | ✅ `downloadlistview.cpp:178-210` | ❌ downloads header has no column-visibility menu (audit G23; closest helper `panel_utils.h:43`) | ❌ |
| U124 Warning_16 icon on download Name cell when metadata incomplete | ✅ `downloadlist.cpp:233-240` | ❌ | ❌ |
| U162 Downloads bar buttons (Refresh / Query download info / show-hidden tips) | ✅ `mainwindow.ui:1319-1430` | ❌ | ❌ |
| U216 Double-click download row (READY->install, PAUSED->resume) | ✅ `downloadlistview.cpp:164-177` | ⚠️ state-driven row activation `downloads_tab.cpp:135`; READY/PAUSED gating unproven | ⚠️ |
| U217 Downloads keyboard Enter/Delete state gating | ✅ `downloadlistview.cpp:326+` | ❌ | ❌ |
| U225 Downloads drag accepted only over downloadTab rect | ✅ `mainwindow.cpp:3947` | ❌ | ❌ |
| U226 MoveAction -> TargetMoveAction coercion on drop | ✅ `mainwindow.cpp:4034-4044` | ❌ | ❌ |
| U243 IDownloadManager API + downloadmanagerproxy requestDownload slot | ✅ `mainwindow.cpp:1576` | ❌ | ❌ |
| U279 ServerInfo per-server speed history + preferred-servers drag lists (NEXUS-LENS: genericize/provider-scope) | ✅ `serverinfo.cpp`, `settingsdialognexus.cpp:366` | ❌ | ❌ |

## 16. Nexus Integration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| NXM handler registration | ✅ `registerAsNXMHandler` | ✅ `nxm_router` + `nxm_ipc`, `nxm_router.h:14`, `nxm_ipc.h:27`, `linux_platform.cpp:780` | ✅ |
| Nexus account / auth | ✅ | ✅ `nexus_account` + `nexus_auth`, `nexus/auth.h:39` | ✅ |
| Nexus HTTP API | ✅ | ✅ `nexus_http` + `nexus_servers`, `nexus/http.h:15`, `nexus/servers.h:28` | ✅ |
| Endorsement system | ✅ `ModInfo::endorse()` | ⚠️ UI button + setting exist, no API call (opens browser), `nexus_source_panel.cpp:41` | ⚠️ |
| Tracking system | ✅ `ModInfo::track()` | ⚠️ UI button + setting exist, no API call (opens browser), `nexus_source_panel.cpp:46` | ⚠️ |
| "Never endorse" option | ✅ `ModInfo::setNeverEndorse` | ❌ | ❌ |
| Nexus description (HTML) | ✅ `ModInfo::getNexusDescription` | ✅ `nexusdescription` in meta + BBCode rendering, `mod_meta.cpp:118` | ✅ |
| Nexus category ID tracking | ✅ `ModInfo::getNexusCategory` | ✅ `nexuscategory` in meta + read/write, `mod_meta.cpp:117` | ✅ |
| Nexus update timestamps | ✅ `getLastNexusUpdate/Query` | ⚠️ `nexuslastmodified` key recognized in meta, not actively used, `mod_meta.cpp:115-116` | ⚠️ |
| OAuth login flow | ✅ `NexusOAuthLogin` | ❌ | ❌ |
| Nexus user account info | ✅ `ApiUserAccount` | ✅ `NexusUserInfo`, `nexus/auth.h:29` | ✅ |
| Rate limit tracking | ✅ | ✅ `RateLimitInfo` (hourly + daily), `nexus/auth.h:15`, `:52` | ✅ |
| Download mirror registry | ❌ | 🚀 `NexusServers` (speed samples, preferred ordering), `nexus/servers.h:28` | 🚀 |
| Nexus API key manual entry | ✅ | ✅ `NexusManualKeyDialog` (Open Browser/Paste/Clear), `source_pages.h:37` | ✅ |
| Tier-derived queue defaults | ❌ | 🚀 `nexus_queue_default_for()` (Regular=queue, Premium=parallel), `source_pages.h:24` | 🚀 |
| Nexus connection UI (reusable component) | ✅ `NexusConnectionUI` (shared between Settings and CreateInstance) | ❌ | ❌ |
| Endorsement state tracking | ✅ `EndorsementState` enum (Accepted/Refused/NoDecision) | ❌ | ❌ |
| Nexus FileStatus (REMOVED, ARCHIVED) | ✅ `NexusInterface::FileStatus` | ❌ | ❌ |
| U007 Visit Nexus action (Ctrl+N; genericized to "visit modding sites" per U001 ruling - site list knowledge/plugin-driven) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.ui:1757-1777` | ❌ | ❌ |
| U013 Endorse ModOrganizer submenu (Endorse / Won't Endorse) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1061-1078` | ❌ | ❌ |
| U014 Browse Mod Page menu (IPluginModPage entries + "Visit <game> on Nexus") (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1568-1686` | ❌ | ❌ |
| U058 Endorse-MO dialogs + toasts (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:2995-3020`, `:3468` | ❌ | ❌ |
| U063 Nexus failure dialogs (blocked action, mod ID gone, request failed) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:3577-3612` | ❌ | ❌ |
| U066 "Browse Mod Page" action exists but hidden by default | ✅ `mainwindow.ui:1778-1798` | ❌ | ❌ |
| U098 API counter tooltip (pools + exhaustion warning paragraph) (NEXUS-LENS: genericize/provider-scope) | ✅ `statusbar.cpp:44-50` | ❌ | ❌ |
| U099 API counter text/colors (Queued/Daily/Hourly + 500/200 thresholds) (NEXUS-LENS: genericize/provider-scope) | ✅ `statusbar.cpp:96-135`, `:145-147` | ⚠️ "Queued:" counter exists `status_bar.cpp:101`; threshold colors + hide flag unproven | ⚠️ |
| U141 Nexus connection state machine strings + Connect->Cancel flip (NEXUS-LENS: genericize/provider-scope) | ✅ `settingsdialognexus.cpp:117-334` | ❌ | ❌ |
| U206 meta.ini endorsed / tracked states (NEXUS-LENS: genericize/provider-scope) | ✅ `modinforegular.cpp:689-711` | ⚠️ endorsement/tracked integration gating exists `nexus_source_panel.cpp:41`, `:46`; Endorsed/Abstained enum parity unproven | ⚠️ |
| U236 IPluginModPage plugin API (pageURL, useIntegratedBrowser, icon) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1568` (uibase) | ❌ | ❌ |
| U242 IPluginGame fields (getSupportURL, primarySources/validShortNames, blueprintPrefix, steamAPPId...) | ✅ `mainwindow.cpp:1060-1660`, `pluginlist.cpp` | ⚠️ game feature registry layer exists `game_feature_registry.h:110`; support-URL + primarySources symbols absent | ⚠️ |
| U252 NexusInterface full API surface (every request type signalled + userData routing) (NEXUS-LENS: genericize/provider-scope) | ✅ `nexusinterface.cpp` | ⚠️ Nexus auth/queue/settings bridge exists `source_pages.cpp:472` + `network_manager.h:154`; full signal surface unproven | ⚠️ |
| U253 nxm*Available signal handlers w/ per-error UI (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:3077-3612` | ❌ | ❌ |
| U255 NXMAccessManager (OAuth flow, credentialsReceived title update, cookie jar) (NEXUS-LENS: genericize/provider-scope) | ✅ `nxmaccessmanager.cpp`, `mainwindow.cpp:465-468` | ❌ no OAuth flow found (API-key auth only `source_pages.cpp:472`) | ❌ |
| U256 API account shown in window title (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:653-668` | ❌ | ❌ |
| U257 API stats -> StatusBar::setAPI via requestsChanged (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:467-469` | ⚠️ API counter text exists `status_bar.cpp:101`; requestsChanged wiring unproven | ⚠️ |
| U258 Per-mod endorsement state from API (EndorsedState incl. unknown-disabled) (NEXUS-LENS: genericize/provider-scope) | ✅ `modlistcontextmenu.cpp:553-575` | ❌ | ❌ |
| U259 Nexus FileStatus incl. ARCHIVED_HIDDEN (NEXUS-LENS: genericize/provider-scope) | ✅ `modlist.cpp:434-436` | ❌ | ❌ |
| U260 Nexus manual API-key dialog + disconnect clears stored auth (NEXUS-LENS: genericize/provider-scope) | ✅ `settingsdialog.ui:1241` | ⚠️ NexusManualKeyDialog exists `source_pages.cpp:98` (apply `:463`); disconnect-clears-auth flow unproven | ⚠️ |
| U276 NxmUrl parsing helpers (uibase nxmurl.cpp) (NEXUS-LENS: genericize/provider-scope) | ✅ `nxmurl.cpp` (uibase) | ⚠️ NxmIpcServer receives parsed URLs `downloads_controller.cpp:223`; encode helpers unproven | ⚠️ |
| U278 PersistentCookieJar (Nexus login cookie persistence) (NEXUS-LENS: genericize/provider-scope) | ✅ `persistentcookiejar.cpp` | ❌ | ❌ |

## 17. Source Providers

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Nexus Mods integration | ✅ | ✅ `NexusProvider`, `nexus_provider.h` | ✅ |
| Steam Workshop | ✅ | ✅ `SteamWorkshopProvider`, `steam_workshop_provider.h` | ✅ |
| LOVERS LAB | ❌ | 🚀 `LoversLabProvider`, `loverslab/provider.h:35` | 🚀 |
| Download manager | ✅ `DownloadManager` | ✅ `curl_download`, `download/curl_download.cpp` | ✅ |
| Remote cache | ✅ | ✅ `RemoteCache` (6-layer fetch chain), `workshop/remote_cache.h:19` | ✅ |
| Steam Workshop client (Web API) | ❌ | 🚀 `WorkshopClient` (SQLite cache, dead ID tracking), `workshop/workshop_client.h:29` | 🚀 |
| LoversLab session-cookie auth | ❌ | 🚀 `LoversLabAuth` (Cloudflare stripping), `loverslab/auth.h:41` | 🚀 |
| Managed games tracking | ❌ | 🚀 `ManagedGames` (source_id, website_url, nexus_domain), `nxm/managed_games.h:23` | 🚀 |
| modl:// protocol handler (mod.pub / MO2 modlhandler) | ❌ | 🚀 `modl://` (Win registry + XDG desktop, route through nxm_ipc), `router.h:47`, `nxm_router.cpp:239`, `cli/command_line.cpp:38` | 🚀 |
| ModPub metadata provider (mod.pub API) | ❌ | 🚀 `ModPubProvider` (page scrape, mod_id, Refresh), `modpub/provider.h:64` | 🚀 |
| modl:// transport-protocol distinction (origin attribution) | ❌ | 🚀 modl:// is transport, not source; real origin attributed, `install_stage.cpp:331`, `modl/provider.h:17` | 🚀 |
| U275 ModRepositoryFileInfo + imodrepositorybridge repository abstraction | ✅ `imodrepositorybridge.cpp` (uibase) | ❌ | ❌ |

## 18. Mod List Features

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Multi-criteria sorting | ✅ `ModListSortProxy` | ⚠️ game sort provider only, no user criteria/sort proxy (cited "sort proxy" absent), `mod_list_controller.cpp:1097` | ⚠️ |
| Category/content/special filtering | ✅ `FilterList` | ✅ `CategoryFilterPanel` + `mod_filter_bar`, `category_filter_panel.h:21`, `mod_filter_bar.h:11` | ✅ |
| Grouping (by separator, category, Nexus ID) | ✅ `QtGroupingProxy` | ✅ visual nesting (parent_id, indent, fold), `mod_list_model.h:57`, `:63`, `:139` | ✅ |
| Drag-and-drop reorder | ✅ `ModList::dropMimeData` | ✅ `mod_table_view` drop support, `mod_table_view.h:185-187` | ✅ |
| Scroll markers | ✅ `ViewMarkingScrollBar` | ✅ `ModMarkingScrollBar` (separator marks), `mod_table_view.h:116` | ✅ |
| CSV export | ✅ `exportModListCSV` | ❌ (removed; replaced by combined import/export) | ❌ |
| Bulk enable/disable | ✅ `setActive(indices)` | ✅ `toggle_selected_mods()`, `mod_list_controller.h:140` | ✅ |
| Priority shift (bulk) | ✅ `shiftModsPriority` | ✅ `priority_move_selected()`, `mod_list_controller.h:139` | ✅ |
| Send to top/bottom/priority | ✅ `sendModsToTop/Bottom/Priority` | ✅ `send_to_highest/lowest_priority()`, `mod_list_controller.h:135-139` | ✅ |
| Send to separator | ✅ `sendModsToSeparator` | ✅ `move_to_separator()`, `mod_list_controller.h:133` | ✅ |
| Send to First/Last Conflict | ✅ `sendModsToFirstConflict/LastConflict` | ❌ | ❌ |
| Collapseable separators | ✅ `collapsibleSeparators` | ✅ 10+ collapsible separator settings, `settings.h:35-40` | ✅ |
| Auto-collapse on hover | ✅ `autoCollapseOnHover` | ✅ `Settings::auto_collapse_on_hover()`, `settings.h:33` | ✅ |
| Filter persistence | ✅ `saveFilters` | ✅ `Settings::save_filters()`, `settings.h:31` | ✅ |
| Filter AND/OR mode | ✅ `FilterAnd`/`FilterOr` | ❌ | ❌ |
| Column visibility toggle | ✅ `setColumnVisible()` | ✅ `ColumnToggleHeaderView`, `column_toggle_header.h:7` | ✅ |
| Mod counter display | ✅ `ModCounters` (LCD) | ✅ `status_bar` (counts), `mod_list_controller.cpp:648` QLCDNumber | ✅ |
| Create separator | ✅ | ✅ `create_separator()` / `create_separator_named()`, `mod_actions.h:63-64` | ✅ |
| Create empty mod | ✅ `createEmptyMod` | ✅ `create_empty_mod()`, `mod_actions.h:65` | ✅ |
| Import archives | ✅ | ✅ `import_archives()`, `mod_list_controller.cpp:3348` | ✅ |
| Export/import modlist | ✅ | ✅ `export_modlist()` / `import_modlist()`, `mod_list_controller.h:59-60` | ✅ |
| Overwrite file drop-to-mod | ❌ | 🚀 `overwrite_files_dropped` signal, `mod_table_view.h:178` | 🚀 |
| IndentDelegate (nesting) | ❌ | 🚀 `IndentDelegate` for Name column, `mod_table_view.h:138` | 🚀 |
| FlagsDelegate with tooltips | ❌ | 🚀 per-emblem hover text, `mod_table_view.h:89` | 🚀 |
| ModListSortProxy criteria system | ✅ `ModListSortProxy::Criteria` | ❌ | ❌ |
| ModListSortProxy separator mode | ✅ `ModListSortProxy::SeparatorMode` | ❌ | ❌ |
| ModList column roles (IndexRole, PriorityRole) | ✅ `ModList::IndexRole`, `PriorityRole` | ❌ | ❌ |
| ModList signals (showMessage, modRenamed, modUninstalled, fileMoved, modPrioritiesChanged) | ✅ `ModList` signals | ❌ | ❌ |
| ModListProxy / ModListByPriorityProxy | ✅ proxy models | ❌ | ❌ |
| U010 Ctrl+F focuses+selects filter (modList, espList, downloadView) | ✅ `mainwindow.cpp:217`, `:495-497` | ❌ no Ctrl+F filter focus wiring found (audit G15) | ❌ |
| U011 Escape clears filter and returns focus to list | ✅ `mainwindow.cpp:224` | ❌ | ❌ |
| U053 Wheel-scroll blocked on groupCombo/profileBox | ✅ `mainwindow.cpp:378-385` | ❌ | ❌ |
| U074 Row-with-children menu (Collapse all / Collapse others / Expand all) | ✅ `modlistcontextmenu.cpp:236-243` | ❌ | ❌ |
| U077 "Send to..." conditionality (priority-sort gating + First/Last conflict flags) | ✅ `modlistcontextmenu.cpp:273-332` | ⚠️ send-to tree w/ conditional entries `mod_context_menu.cpp:280` (separator-aware `:307`); conflict-flag conditionals unproven | ⚠️ |
| U092 Mod-list cell tooltips per column (flags/conflicts/name/version cooldown/category/notes) | ✅ `modlist.cpp:384-470` | ❌ | ❌ |
| U095 Mod list header tooltips (13 exact strings) | ✅ `modlist.cpp:1345-1390` | ⚠️ header tooltips exist `mod_list_controller.cpp:569` (audit G9); exact 13 strings unproven | ⚠️ |
| U106 Mod list 13 columns | ✅ `modlist.h:83-96`, `modlist.cpp:1315-1339` | ⚠️ column enum covers the set `mod_list_model.h:114`; per-column parity unproven | ⚠️ |
| U110 Editable-cell rules (priority/version/ModID; foreign guards; auto-priority) | ✅ `modlist.cpp:620-650` | ❌ | ❌ |
| U111 Version column "?" when empty + canBeUpdated | ✅ `modlist.cpp:198-206` | ❌ | ❌ |
| U113 Priority cell hidden for automatic-priority mods | ✅ `modlist.cpp:207-213` | ❌ | ❌ |
| U127 HIGHLIGHT_CENTER centers Name cell alignment | ✅ `modlist.cpp:640-647` | ❌ | ❌ |
| U136 Mod List settings (separator scrollbar colors, out-of-MO mods, remember filters, update-on-install, drag collapse) | ✅ `settingsdialog.ui:557-635` | ⚠️ Mod List settings tab exists `settings_content_widget.cpp:450`; the 5 MO2 checkboxes unproven | ⚠️ |
| U137 Collapsible Separators settings group (sort-direction, highlights, icon toggles, per-profile) | ✅ `settingsdialog.ui:653-830` | ⚠️ 12 collapsible-separator settings keys `settings.h:35` + settings UI `settings_content_widget.cpp:498`; plugin-highlight nuance unproven | ⚠️ |
| U163 Filter panel controls (Clear/Edit, And/Or radios, filters tree) | ✅ `mainwindow.ui:137-200`, `:505` | ❌ | ❌ |
| U164 openFolderMenu / listOptionsBtn / displayCategoriesBtn tooltips+roles | ✅ `mainwindow.ui:283-460` | ⚠️ ProfileBar folders menu exists `profile_bar.h:14` (kinds `profile_bar.cpp:51-65`); listOptions/displayCategories buttons unproven | ⚠️ |
| U165 LCD counters ("Active:" activeMods/activePlugins QLCDNumber) | ✅ `mainwindow.ui:354`, `:880` | ⚠️ mod-count QLCDNumber `mod_list_controller.cpp:648` + plugin counter `plugin_view.cpp:514`; "Active:" labels unproven | ⚠️ |
| U232 Escape/Filter shortcut scope = WidgetWithChildren, autoRepeat off | ✅ `mainwindow.cpp:217-231` | ❌ | ❌ |
| U280 csvbuilder for exportModListCSV | ✅ `csvbuilder.cpp` | ❌ | ❌ |

## 19. Mod Context Menu

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Visit on Nexus | ✅ `visitOnNexus` | ✅ `source_visit_info` (Nexus/LoversLab), `mod_context_menu.cpp:398-408`, `mod_list_controller.cpp:309` | ✅ |
| Visit web page | ✅ `visitWebPage` | ✅ source-aware context menu, `mod_context_menu.cpp:402` | ✅ |
| Reinstall mod | ✅ `reinstallMod` | ❌ | ❌ |
| Create backup | ✅ `createBackup` | ⚠️ deploy-level `backup_original()`, no standalone user action, `deploy_utils.cpp:165` | ⚠️ |
| Restore backup | ✅ `restoreBackup` | ⚠️ deploy-level `remove_and_restore()`, no standalone user action, `deploy_utils.cpp:195` | ⚠️ |
| Restore hidden files | ✅ `restoreHiddenFiles` | ❌ | ❌ |
| Mark as converted | ✅ `markConverted` | ❌ | ❌ |
| Ignore missing data | ✅ `ignoreMissingData` | ✅ `mark_validated()`, `mod_context_menu.cpp:339` | ✅ |
| Ignore update | ✅ `setIgnoreUpdate` | ❌ | ❌ |
| Set color | ✅ `setColor`, `resetColor` | ✅ `NotesTab` Set/Reset color, `notes_tab.h:27-28` | ✅ |
| Open in Explorer | ✅ `openExplorer` | ✅ Ctrl+double-click, `mod_table_view.cpp:362` | ✅ |
| Create empty mod | ✅ `createEmptyMod` | ✅ `create_empty_mod()`, `mod_actions.h:65` | ✅ |
| Create separator | ✅ `createSeparator` | ✅ `create_separator()`, `mod_actions.h:64` | ✅ |
| Overwrite: create mod from overwrite | ✅ `createModFromOverwrite` | ✅ `create_mod_from_overwrite()`, `overwrite_controller.h:21` | ✅ |
| Overwrite: move to existing mod | ✅ `moveOverwriteContentToExistingMod` | ✅ `move_overwrite_content_to_mod()`, `overwrite_controller.h:22` | ✅ |
| Overwrite: clear | ✅ `clearOverwrite` | ✅ `clear_overwrite()`, `overwrite_controller.h:20` | ✅ |
| Set categories (batch) | ✅ `setCategories`, `setPrimaryCategory` | ✅ `add_category_menus()` (checkable + radio), `mod_context_menu.h:39` | ✅ |
| Rename mod | ✅ `renameMod` | ✅ `rename_mod_inline()`, `mod_list_controller.h:63` | ✅ |
| Remove mod | ✅ | ✅ `remove_selected_mods()` (moves folder to trash), `mod_list_controller.h:132`, `mod_actions.cpp:158` | ✅ |
| Root override toggle | ✅ | ✅ `toggle_root_override()`, `mod_list_controller.h:142` | ✅ |
| U052 listOptionsBtn hosts ModListGlobalContextMenu | ✅ `mainwindow.cpp:374-376` | ❌ | ❌ |
| U071 ModListGlobalContextMenu full tree (install/create above-below-inside, collapse, enable-matching, update, auto-categories, refresh, csv) | ✅ `modlistcontextmenu.cpp:33-102` | ⚠️ global menu subset exists `mod_context_menu.cpp:75` (audit G12); filter-aware labels + position-aware entries unproven | ⚠️ |
| U072 Type-dispatched context menus (Overwrite/Backup/Separator/Foreign/Regular trees) | ✅ `modlistcontextmenu.cpp:225-245` | ❌ no per-row-type menu variants found | ❌ |
| U073 "All Mods" submenu (global menu nested in row menu) | ✅ `modlistcontextmenu.cpp:230-234` | ❌ no All-Mods submenu found | ❌ |
| U075 "Information..." default (bold) action, omitted for foreign | ✅ `modlistcontextmenu.cpp:247-255` | ⚠️ Information action exists `mod_context_menu.cpp:114`; setDefaultAction bolding unproven | ⚠️ |
| U081 Separator row menu (rename/remove, color select/reset, send-to) | ✅ `modlistcontextmenu.cpp:422-447` | ❌ | ❌ |
| U082 Foreign row menu (Send to... only - no Information) | ✅ `modlistcontextmenu.cpp:449-454` | ❌ | ❌ |
| U083 Backup row menu (Restore/Remove Backup, Ignore missing, Mark converted, Visit blocks, Explorer) (NEXUS-LENS: genericize/provider-scope) | ✅ `modlistcontextmenu.cpp:456-497` | ❌ | ❌ |
| U084 Regular row menu full order (versioning, force-check, endorse/track blocks, colors, visits) (NEXUS-LENS: genericize/provider-scope) | ✅ `modlistcontextmenu.cpp:499-637` | ❌ | ❌ |
| U208 modid-less custom URL / installationFile / uploader fields (Visit on host) | ✅ `modinforegular.cpp` | ❌ | ❌ |
| U230 Global context menu rebuilt on aboutToShow | ✅ `modlistcontextmenu.cpp:22-30` | ❌ | ❌ |
| U231 Position-aware installMod (above/below/inside by sort) | ✅ `modlistcontextmenu.cpp:33-70` | ❌ | ❌ |

## 20. Plugin Context Menu

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Send plugins to priority | ✅ `PluginListContextMenu` | ❌ | ❌ |
| Set ESP lock (from context) | ✅ `setESPLock` | ✅ `lock_requested` signal, `plugin_context_menu.h:35`, `plugin_context_menu.cpp:25` | ✅ |
| Open origin explorer | ✅ `openOriginExplorer` | ❌ | ❌ |
| Open origin information | ✅ `openOriginInformation` | ❌ | ❌ |
| Enable/disable plugin | ✅ `PluginListContextMenu` | ❌ | ❌ |
| Send-to priority | ✅ `sendToPriority` | ❌ | ❌ |
| Lock/unlock plugin | ✅ `setESPLock` | ✅ Lock/Unlock load order actions, `plugin_context_menu.cpp:24-28` | ✅ |
| U085 Plugin "Enable all"/"Disable all" + confirm dialog | ✅ `pluginlistcontextmenu.cpp:36-48` | ⚠️ plugin context menu + lock/enable handling `plugin_context_menu.cpp:24`; bulk enable + confirm unproven | ⚠️ |
| U086 Lock/Unlock load-order labels conditional on lock state | ✅ `pluginlistcontextmenu.cpp:56-77` | ⚠️ lock/unlock actions exist `plugin_context_menu.cpp:24`; state-conditional labels unproven | ⚠️ |
| U087 Plugin Send to... (Top/Bottom/Priority QInputDialog) | ✅ `pluginlistcontextmenu.cpp:111-133` | ❌ | ❌ |
| U088 "Open Origin in Explorer" gated on origin resolving | ✅ `pluginlistcontextmenu.cpp:80-95` | ❌ | ❌ |
| U089 "Open Origin Info..." default action (single non-foreign) | ✅ `pluginlistcontextmenu.cpp:96-107` | ❌ | ❌ |
| U228 Lock conditionals consider only enabled plugins | ✅ `pluginlistcontextmenu.cpp:56-77` | ❌ | ❌ |

## 21. Archive & Installation

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| FOMOD installer | ✅ `IPluginInstaller` | ✅ `FomodInstaller` plugin + engine, `fomod_view_model.h:52` | ✅ |
| FOMOD XML parsing | ✅ | ✅ `module_config` (GroupType, Dependency, PluginType), `module_config.h` | ✅ |
| FOMOD condition tester | ✅ | ✅ `condition_tester` (file/flag/game/composite), `condition_tester.h:10` | ✅ |
| FOMOD C# script detection | ✅ | ✅ `hasCSharpScript()`, `module_config.h:314` | ✅ |
| FOMOD view model (step nav) | ✅ | ✅ `FomodViewModel` (step forward/back, flag map), `fomod_view_model.h:52` | ✅ |
| FOMOD file installer | ✅ | ✅ `FomodFileInstaller::apply()`, `file_installer.h:24`, `:32` | ✅ |
| Archive password support | ✅ `queryPassword` | ❌ (encrypted archives rejected, not supported) | ❌ |
| Installation merge/replace | ✅ `merged`, `replaced` | ❌ | ❌ |
| Backup on install | ✅ `keepBackupOnInstall` | ❌ | ❌ |
| Installation result tracking | ✅ `InstallationResult` | ❌ | ❌ |
| Staging layout normalization | ❌ | 🚀 `analyze_staging_layout()` + `normalize_staging_root()`, `staging_layout.h:50`, `:58` | 🚀 |
| BSA/BA2 archive listing | ❌ | 🚀 `DataArchive` (libarchive-backed), `data_archive.h:25` | 🚀 |
| Install name dialog (smart candidates) | ❌ | 🚀 `InstallNameDialog` (editable combobox), `install_name_dialog.h:18` | 🚀 |
| Install progress dialog (modeless) | ❌ | 🚀 `InstallProgressDialog` (300ms show delay), `install_progress_dialog.h:19` | 🚀 |
| IPluginInstaller::EInstallResult | ✅ `IPluginInstaller::EInstallResult` | ❌ | ❌ |
| Archive file tree representation | ✅ `ArchiveFileTree`, `ArchiveFileEntry` | ✅ `ArchiveFileTree` (libarchive-backed), `archive_file_tree.h:19` | ✅ |
| U194 Installer error/progress strings (extraction failed, invalid name, no installer plugins, password prompt...) | ✅ `installationmanager.cpp:73-880` | ❌ | ❌ |
| U195 7z error code strings (9 exact) | ✅ `installationmanager.cpp:887-912` | ❌ | ❌ |
| U282 VirtualFileTree/qdirfiletree/archivefiletree abstractions | ✅ `virtualfiletree.cpp`, `qdirfiletree.cpp`, `archivefiletree.cpp` | ⚠️ archive file tree exists `archive_file_tree.h:19`; qdirfiletree/virtualfiletree parity unproven | ⚠️ |
| U285 validationprogressdialog.ui (extract validation progress) | ✅ `validationprogressdialog.ui` | ❌ | ❌ |

## 22. Deploy System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Symlink strategy | ❌ | 🚀 `SymlinkStrategy` (CI target resolution), `strategy.h:10` | 🚀 |
| Direct deploy strategy | ❌ | 🚀 `DirectDeployStrategy` (ledger + backup), `strategy_direct.h:7` | 🚀 |
| Hardlink strategy | ❌ | 🚀 `HardlinkStrategy`, `strategy_hardlink.h:7` | 🚀 |
| Junction strategy (Windows) | ❌ | 🚀 `JunctionStrategy`, `strategy_junction.h:7` | 🚀 |
| OverlayFS deploy strategy | ❌ | 🚀 `OverlayFsDeploy` (O(1) reorder; cited name OverlayFsDeployStrategy is alias), `overlay_fs_deploy.h:14` | 🚀 |
| FUSE VFS strategy | ❌ | 🚀 `VfsStrategy` (FUSE + file_map), `strategy_vfs.h:7`, `vfs.h:11` | 🚀 |
| Deploy ledger (incremental tracking) | ❌ | 🚀 `DeployLedger` (diff for priority changes), `deploy_ledger.h:8` | 🚀 |
| Parallel deploy (thread pool) | ❌ | 🚀 `deploy_all_enabled_mods_parallel()`, `deploy_utils.h:128` | 🚀 |
| Root override ([General] rootOverride) | ❌ | 🚀 `RootOverride` + `classify_registry_path()`, `root_override.h:34` | 🚀 |
| Case-insensitive deploy aliases | ❌ | 🚀 `add_case_insensitive_aliases()`, `deploy_utils.h:219` | 🚀 |
| Deploy backup + restore | ❌ | 🚀 `remove_deployed_files()` restores originals, `deploy_utils.h:194` | 🚀 |
| Binary detection (PE/ELF/SH) | ❌ | 🚀 `is_executable_binary()`, `deploy_utils.h:86` | 🚀 |

## 23. Overwrite System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Move overwrite to mod | ✅ | ✅ `move_overwrite_to_mod()`, `overwrite_utils.cpp:391` | ✅ |
| Sync overwrite file | ✅ | ✅ `sync_overwrite_file()`, `overwrite_utils.h:73` | ✅ |
| Clear overwrite (trash) | ✅ | ✅ `clear_overwrite()`, `overwrite_controller.h:20` | ✅ |
| CI directory merge (overlay captures) | ❌ | 🚀 `normalize_overwrite_casing()`, `launch_controller.cpp:1194` | 🚀 |
| Overwrite sync plan (apply/preview) | ❌ | 🚀 `apply_sync_plan()`, `overwrite_utils.h:124` | 🚀 |
| Overwrite info dialog (file browser) | ✅ `OverwriteInfoDialog` | ✅ `OverwriteInfoDialog` (QFileSystemModel, context menu), `overwrite_info_dialog.h:20` | ✅ |
| Query overwrite dialog (merge/replace) | ✅ `QueryOverwriteDialog` | ✅ `QueryOverwriteDialog` (thread-safe `ask_overwrite()`), `query_overwrite_dialog.h:20`, `:40` | ✅ |
| Sync overwrite dialog (selective) | ✅ `SyncOverwriteDialog` | ✅ `SyncOverwriteDialog` (per-file combo, game-origin), `sync_overwrite_dialog.h:23` | ✅ |
| Move to mod dialog | ❌ | 🚀 `MoveToModDialog` (destination picker), `move_to_mod_dialog.h:16` | 🚀 |
| U080 Overwrite row menu (Sync to Mods w/ count guard, Create/Move/Clear, Open in Explorer) | ✅ `modlistcontextmenu.cpp:402-420` | ⚠️ move-content-to-Mod picker exists `move_to_mod_dialog.h:16`; Sync-to-Mods guard unproven | ⚠️ |
| U175 QueryOverwriteDialog (Keep Backup / Merge / Replace / Rename / Cancel) | ✅ `queryoverwritedialog.ui` | ❌ | ❌ |
| U176 SyncOverwriteDialog columns (Name / Sync To per-file combo) | ✅ `syncoverwritedialog.ui` | ✅ `sync_overwrite_dialog.cpp:27` (Name + Sync-To combo columns) | ✅ |
| U177 OverwriteInfoDialog (Open in Explorer + drag&drop hint) | ✅ `overwriteinfodialog.ui` | ⚠️ OverwriteInfoDialog port exists `overwrite/overwrite_info_dialog.h:14` (context menu `:106`); Explorer button + hint text unproven | ⚠️ |

## 24. Save Game System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Save game model | ✅ | ✅ `SaveGame` `save_game.h:49` | ✅ |
| Skyrim SE/LE save parsing | ✅ | ✅ parsers registered `SkyrimSpecialEdition.cpp:112`, SE `SkyrimSESaveParser.cpp:150`, LE `SkyrimSaveGame.h:2` (was mis-cited: `parse_skyrim_save()`/`parse_skyrimse_save()` symbols absent) | ✅ |
| Save scanning | ✅ | ✅ `scan_saves()` `save_scanner.h:38` + save_extensions hook `game_knowledge.cpp:249`, userdata resolves `game_knowledge.cpp:213`, used `downloads_controller.cpp:444`, `:485` | ✅ |
| Save missing assets resolver | ✅ | ✅ `find_save_missing_assets()` `save_missing_assets.h:26` | ✅ |
| Local saves | ✅ | ✅ `local_saves` `local_saves.h:51` | ✅ |
| Pluggable save parser (per-game) | ❌ | 🚀 `SaveParserRegistry` `save_parser_registry.h:45` | 🚀 |
| Script extender file detection | ❌ | 🚀 `has_script_extender_file()` `save_game.h:100` | 🚀 |
| Save screenshot extraction (RGBA) | ❌ | 🚀 `SaveGame::screenshot` `save_game.h:69` | 🚀 |
| Save game list (QTreeWidget) | ✅ `SavesTab` | ✅ `saves_tab` QTableWidget (not tree) `saves_tab.h:143` | ✅ |
| Save game hover info popup | ✅ `GamebryoSaveGameInfoWidget` | ✅ `saves_tab` hover info popup `saves_tab.h:159` | ✅ |
| Save game background scan | ✅ | ✅ `SavesScanWorker` `saves_scan_worker.h:52` | ✅ |
| Save game delete | ✅ `SavesTab::deleteSavegame()` | ✅ `on_delete_key()` `saves_tab.h:131` | ✅ |
| Save game context menu | ✅ `SavesTab::onContextMenu()` | ✅ `saves_tab` context menu `saves_tab.h:133` | ✅ |
| Save game open in explorer | ✅ | ⚠️ open_explorer exists for mod files `mod_list_controller.cpp:2589`, not wired to saves | ⚠️ |
| Save game fix missing assets | ✅ `SavesTab::fixMods()` | ⚠️ detection + display `save_missing_assets.h:26` exists, no fix action | ⚠️ |
| Transfer saves dialog | ✅ `TransferSavesDialog` | ❌ | ❌ |
| Save game streaming (per-save entryReady, binary-insert sorted list) | ❌ | 🚀 `SavesScanWorker::entryReady` `saves_scan_worker.h:70` + `SavesTab::on_entry_ready` `saves_tab.h:123` | 🚀 |
| Save Information dialog (2-column details, thumbnail, plugin list) | ✅ `GamebryoSaveGameInfoWidget` | ✅ `SaveInfoDialog` `save_info_dialog.h:30` (2-column, thumbnail, plugin load-order status) | ✅ |
| Parallel save scan (parallel parse + provider indexing) | ❌ | 🚀 `parallel::for_each` + `SavesScanWorker` double-fire fix `saves_scan_worker.h:52` | 🚀 |
| Disabled-but-present plugins excluded from missing assets | ✅ | ✅ `find_save_missing_assets` `save_missing_assets.h:26` (enabled OR force_loaded check) | ✅ |
| U105 Save hover widget fields (SE data, missing ESP/ESH/ESL lists + N more) | ✅ `gamebryosavegameinfowidget.cpp:78-161` | ⚠️ hover info exists `saves_tab.h:27` (background scan + hover); exact field list unproven | ⚠️ |
| U115 Saves list columns (display name "%1, #%2, Level %3, %4" + relative path) | ✅ `gamebryosavegame.cpp:48`, `savestab.cpp:180-196` | ⚠️ saves tab scan/list exists `saves_tab.h:27`; display-name format + path column unproven | ⚠️ |
| U159 Saves context menu (Fix enabled mods gating, Delete %n save(s), Open in Explorer) | ✅ `savestab.cpp:244-280` | ⚠️ save missing-assets fix exists `save_missing_assets.h:26`; menu gating/plural-delete unproven | ⚠️ |
| U160 Save delete confirm (first 10 names + recycle-bin note) | ✅ `savestab.cpp:205-235` | ❌ | ❌ |
| U199 Save parsing error strings (open failed, wrong format) | ✅ `gamebryosavegame.cpp:102-112` | ❌ | ❌ |
| U227 Save list sorted by creation desc + streaming adds + relative path column | ✅ `savestab.cpp:180` | ❌ | ❌ |
| U239 SaveGameInfo feature (getMissingAssets used by saves Fix) | ✅ `savestab.cpp:244` | ✅ `saves_tab.cpp:329` SaveParserRegistry::parse_save (missing-assets fix path `save_missing_assets.h:26`) | ✅ |

## 25. Game Detection & Knowledge

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Game detection (Steam library) | ✅ | ✅ `detect_steam_games()` `game_detector.h:24` | ✅ |
| Per-game knowledge (key-value) | ✅ | ✅ `GameKnowledge` `game_knowledge.h:20` | ✅ |
| Game capabilities (tab display) | ❌ | 🚀 `GameCapabilities` `game_capabilities.h:31` (CapabilityInfo, visible_tabs_for `:60`) | 🚀 |
| Game feature registry (MO2 IGameFeatures port) | ❌ | 🚀 `GameFeatureRegistry` `game_feature_registry.h:110` (priority + replace, typed resolve_feature) | 🚀 |
| ModDataChecker feature | ❌ | 🚀 `ModDataContentFeature` `game_feature.h:112` (standard Bethesda catalog) | 🚀 |
| ScriptExtender feature | ❌ | 🚀 `ScriptExtenderFeature` `game_feature.h:205` (binary_name, loader_name) | 🚀 |
| DataArchives feature | ❌ | 🚀 `DataArchivesFeature` `game_feature.h:186` (vanilla archive list) | 🚀 |
| AnimationParser feature | ❌ | 🚀 `AnimationParserFeature` `game_feature.h:337` (frames, layers, RGBA pixels) | 🚀 |
| UnmanagedMods feature (DLC/CC) | ❌ | 🚀 `UnmanagedModsFeature` `game_feature.h:271` | 🚀 |
| BSAInvalidation feature | ❌ | 🚀 `BSAInvalidationFeature` `game_feature.h:288` | 🚀 |
| Game icons (download-on-demand) | ❌ | 🚀 `GameIconCache` `game_icon_cache.h:38` (async, placeholder avatars) | 🚀 |
| Multi-game detection | ❌ | 🚀 `detect_steam_games_multi()` `game_detector.h:29` | 🚀 |
| VDF/ACF parsing | ❌ | 🚀 `parse_library_folders()` `game_detector.h:34` + `parse_acf_value()` `game_detector.h:38` | 🚀 |
| U060 espTab/bsaTab removed when game lacks the feature | ✅ `mainwindow.cpp:340-348` | ⚠️ game feature registry exists `game_feature_registry.h:110`; per-tab removal wiring unproven | ⚠️ |
| U240 IGameFeatures gating tab visibility (GamePlugins/DataArchives/SaveGameInfo/ModDataContent) | ✅ `mainwindow.cpp:340-348` | ⚠️ GameFeatureRegistry exists `game_feature_registry.h:110`; tab-gating by registry unproven | ⚠️ |

## 26. Pipeline System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Pipeline with ordered stages | ❌ | 🚀 `Pipeline` `pipeline.h:178` + `PipelineContext` `pipeline.h:58` | 🚀 |
| Fetch stage (download) | ❌ | 🚀 `FetchStage` `fetch_stage.h:9` | 🚀 |
| Extract stage (archive) | ❌ | 🚀 `ExtractStage` `extract_stage.h:10` (low_priority) | 🚀 |
| FOMOD stage (wizard) | ❌ | 🚀 `FomodStage` `fomod_stage.h:15` | 🚀 |
| Install stage (deploy) | ❌ | 🚀 `InstallStage` `install_stage.h:26` | 🚀 |
| Deploy stage (symlink/overlay) | ❌ | 🚀 `DeployStage` `deploy_stage.h:9` | 🚀 |
| Resolve stage (path resolution) | ❌ | 🚀 `ResolveStage` `resolve_stage.h:7` | 🚀 |
| Sync stage (overwrite) | ❌ | 🚀 `SyncStage` `sync_stage.h:13` | 🚀 |
| Launch stage (game execution) | ❌ | 🚀 `LaunchStage` `launch_stage.h:7` | 🚀 |
| Plugin claim stage | ❌ | 🚀 `PluginClaimStage` `plugin_claim_stage.h:12` | 🚀 |
| Stage registry + hook registry | ❌ | 🚀 `StageRegistry` `stage_registry.h:24` + `HookRegistry` `hook_registry.h:22` | 🚀 |
| Overwrite decision (Merge/Replace/Rename/Cancel) | ❌ | 🚀 `OverwriteAction` enum `pipeline.h:23` | 🚀 |
| FOMOD decision (accept/manual/choices_json) | ❌ | 🚀 `FomodDecision` `pipeline.h:40` | 🚀 |
| Trace recorder (pipeline workflow) | ❌ | 🚀 `TraceRecorder` `trace_recorder.h:37` (flow_id, stages, durations) | 🚀 |
| Pipeline visualization (2D canvas) | ❌ | 🚀 `PipelineContentWidget` `pipeline_content_widget.h:31` (stage cards, arrows, status) | 🚀 |
| Pipeline worker (background) | ❌ | 🚀 `PipelineWorker` `pipeline_worker.h:76` | 🚀 |

## 27. Plugin Host System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| C ABI plugin loading (dlopen) | ❌ | 🚀 `PluginLoader` `plugin_loader.h:206` (load_plugin, load_directory `:214-215`) | 🚀 |
| v2 ABI registration | ❌ | 🚀 `gmm_register_v2()` `plugin_loader.cpp:1952` | 🚀 |
| Python plugin loader | ❌ | 🚀 `PythonLoader` `python_loader.h:15` | 🚀 |
| Tool registry (IPluginTool) | ❌ | 🚀 `ToolRegistry` `tool_registry.h:38` (tool_id, kind, fn) | 🚀 |
| Diagnostics registry | ❌ | 🚀 `DiagnosticsRegistry` `diagnostics_registry.h:21` + `DiagnoseRegistry` `diagnose_registry.h:26` | 🚀 |
| Deploy strategy registry | ❌ | 🚀 `DeployStrategyRegistry` `deploy_strategy_registry.h:24` (deploy/remove) | 🚀 |
| Hook registry (behavior injection) | ❌ | 🚀 `HookRegistry` `plugin_host/hook_registry.h:24` (tag-based, priority-ordered) | 🚀 |
| Save parser registry | ❌ | 🚀 `SaveParserRegistry` `save_parser_registry.h:45` | 🚀 |
| File mapper registry | ❌ | 🚀 `FileMapperRegistry` `file_mapper_registry.h:25` | 🚀 |
| Order encoding registry | ❌ | 🚀 `OrderEncodingRegistry` `order_encoding_registry.h:25` | 🚀 |
| Requirements registry | ❌ | 🚀 `RequirementsRegistry` `requirements_registry.h:42` | 🚀 |
| Plugin settings registry | ❌ | 🚀 `PluginSettingsRegistry` `plugin_settings_registry.h:43` | 🚀 |
| U144 Plugin disable warnings (game-required + dependent plugins) | ✅ `settingsdialogplugins.cpp:232-270` | ❌ | ❌ |
| U244 OrganizerProxy/plugin dependency-resolution dialog | ✅ `plugincontainer.cpp`, `settingsdialogplugins.cpp` | ❌ | ❌ |
| U274 PluginRequirements (uibase pluginrequirements.cpp) | ✅ `pluginrequirements.cpp` (uibase) | ⚠️ requirements registry wired into loader `plugin_loader.cpp:23`; uibase-shared API parity unproven | ⚠️ |
| U283 PluginListProxy / OrganizerProxy plugin proxies | ✅ `pluginlistproxy.cpp`, `organizerproxy.cpp` | ❌ | ❌ |

## 28. Sort System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Sort provider / registry | ❌ | 🚀 `Sorter::Interface` `interface.h:28` + `Sorter::Registry` `registry.h:12` | 🚀 |
| C ABI sort provider | ❌ | 🚀 `Sorter::Abi` `abi.h:18` | 🚀 |
| LOOT sorter | ❌ | 🚀 `Sorter::Loot` `loot/sorter.h:66` (run_sort with progress) | 🚀 |
| Masterlist manager | ❌ | 🚀 `MasterlistManager` `masterlists.h:21` (GitHub branch walk-down, 24h TTL) | 🚀 |

## 29. Instance Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Instance manager | ✅ `InstanceManager` | ✅ `Instance` `instance.h:25` (TOML, per-folder overrides `:53`) | ✅ |
| Create instance dialog | ✅ `CreateInstanceDialog` | ✅ `GameSelectionWidget` `game_selection_widget.h:22` (game cards + filter) | ✅ |
| Instance switcher | ✅ | ✅ `InstanceSwitcherDialog` `instance_switcher_dialog.h:19` + `InstanceSwitcherContentWidget` | ✅ |
| Instance TOML persistence | ❌ | 🚀 `parse_instance_toml()` `toml_utils.h:18` + JSON-to-TOML repair | 🚀 |
| Instance scan + last-used | ❌ | 🚀 `scan_instances()` + `read/write_last_instance()` `instance_utils.h:28-35` | 🚀 |
| Game icons (download-on-demand) | ❌ | 🚀 `GameIcons` `game_icons.h:30` (ensure_icon_cached) | 🚀 |
| Masterlist fetch (GitHub cache) | ❌ | 🚀 `MasterlistFetch` `masterlist_fetch.h:25` (branch walk-down) | 🚀 |
| Instance statistics dialog | ❌ | 🚀 `StatsContentWidget` `stats_content_widget.h:17` (sizes + open in explorer) | 🚀 |
| Instance options panel | ❌ | 🚀 `instance_options_panel` `instance_options_panel.h:28` | 🚀 |
| Create instance wizard (7-page) | ✅ `CreateInstanceDialog` (Intro, Type, Game, Variants, Name, Paths, Profiles, Nexus, Confirmation) | ❌ | ❌ |
| Game variant selection | ✅ `CreateInstanceDialog` (game variants) | ❌ | ❌ |
| Microsoft Store game handling | ✅ `InstanceManager` | ❌ | ❌ |
| U025 Manage Instances action hidden when change not allowed | ✅ `mainwindow.cpp:741-743` | ❌ | ❌ |
| U168 Instance manager dialog controls (create/explore/rename/delete/switch, filter, wiki link) | ✅ `instancemanagerdialog.ui` | ⚠️ instance switcher dialog exists `instance_switcher_dialog.h:19`; full control set unproven | ⚠️ |
| U169 Instance manager flows (validation errors, switching TaskDialog, delete guards) | ✅ `instancemanagerdialog.cpp:112-563` | ❌ | ❌ |
| U179 CreateInstanceDialog page copy (intro/type/game/edition/name/profile/paths) | ✅ `createinstancedialog.ui` | ❌ | ❌ |
| U286 createinstancedialogpages per-page validation logic | ✅ `createinstancedialogpages.cpp` | ❌ | ❌ |

## 30. UI Layer

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod list view | ✅ | ✅ `mod_table_view` `mod_table_view.h:157` | ✅ |
| Plugin list view | ✅ | ✅ `plugins_tab` `plugins_tab.h:23` | ✅ |
| Mod info dialog | ✅ `ModInfoDialog` | ✅ `mod_info_dialog` `mod_info_dialog.h:25` (15 tabs) | ✅ |
| Settings dialog | ✅ `SettingsDialog` | ✅ `settings_content_widget` `settings_content_widget.h:29` | ✅ |
| Toolbar | ✅ | ✅ `main_toolbar` `main_toolbar.h:12` | ✅ |
| Game lock overlay | ✅ `UILocker` | 🚀 `game_lock_overlay` `launch_controller.h:123` (create/show/hide) | 🚀 |
| Process Tree View | ❌ | 🚀 `process_tree_checkbox` `launch_controller.cpp:1598` | 🚀 |
| FOMOD wizard UI | ✅ | ✅ `fomod_wizard_dialog` `fomod_wizard_dialog.h:37` | ✅ |
| FOMOD image viewer | ✅ | ✅ `fomod_image_viewer` `fomod_image_viewer.h:14` | ✅ |
| Profile bar (combo + folders) | ❌ | 🚀 `ProfileBar` `profile_bar.h:28` (12 FolderKind, export/import) | 🚀 |
| Profile manager dialog | ✅ | ✅ `profile_manager_dialog` `profile_manager_dialog.h:22` | ✅ |
| Profile settings widget | ❌ | 🚀 `profile_settings_widget` `profile_settings_widget.h:18` | 🚀 |
| Console panel | ❌ | 🚀 `console_panel` `console_panel.h:11` | 🚀 |
| Debug window (Konami code) | ❌ | 🚀 `debug_window` `debug_window.h:42` (easter egg `settings_controller.cpp:1442`) | 🚀 |
| Preview system (images/text/video) | ✅ | ✅ `preview_registry` `preview_registry.h:43` + `preview_widget` `preview_widget.h:43` | ✅ |
| File viewer (image, video, 3D scene) | ❌ | 🚀 `ImageViewer` `image_viewer.h:23`, `VideoViewer` `video_viewer.h:18`, `SceneViewer` `scene_viewer.h:13` | 🚀 |
| Plugin-provided preview | ✅ | ✅ `preview_window` `preview_window.h:61` (v2 IPluginPreview) | 🚀 |
| ANM2 animation playback | ❌ | 🚀 `preview_window` `preview_window.h:61` (frame-based timer) | 🚀 |
| Variant browsing (prev/next) | ❌ | 🚀 `preview_window` `preview_window.h:61` multi-provider | 🚀 |
| Zoom/fit controls | ❌ | 🚀 `preview_window` zoom_by/set_fit `preview_window.h:106-107` | 🚀 |
| Smooth scroll | ❌ | 🚀 `smooth_scroll` `smooth_scroll.h:19` (SmoothScroller) | 🚀 |
| Zoom controls | ❌ | 🚀 `zoom_controls` `zoom_controls.h:18` (ZoomableView) | 🚀 |
| Column toggle header | ❌ | 🚀 `column_toggle_header` `column_toggle_header.h:7` | 🚀 |
| Game path banner | ❌ | 🚀 `game_path_banner` `game_path_banner.h:12` | 🚀 |
| Status bar (custom) | ✅ `StatusBar` | 🚀 `status_bar` `status_bar.h:15` | 🚀 |
| Notification backend | ❌ | 🚀 `notification_backend` `notification_backend.h:8` | 🚀 |
| Single instance guard | ❌ | 🚀 `MultiProcess` QLockFile `multi_process.h:11`, wired `core.cpp:331` (was mis-cited: `single_instance`/QtSingleApplication absent) | 🚀 |
| BBCode parser (Nexus descriptions) | ✅ | ✅ `bbcode` `bbcode.h:22` (bbcode_to_html) | ✅ |
| Menu bar (File/Edit/View/Tools/Help) | ✅ | ✅ `AppMenuBar` `menu_bar.h:24` (dynamic per-game tools) | ✅ |
| Data tab (virtual data browser) | ✅ `DataTab` | ✅ `data_tab` `data_tab.h:25` (dual view, background build worker, context menu) | ✅ |
| Downloads tab | ✅ `DownloadsTab` | ✅ `downloads_tab` `downloads_tab.h:46` (drag-drop, watcher, compact) | ✅ |
| Saves tab | ✅ `SavesTab` | ✅ `saves_tab` `saves_tab.h:27` (background scan, hover info) | ✅ |
| Conflicts tab | ✅ | ✅ `conflicts_tab` `conflicts_tab.h:22` (image diff) | ✅ |
| Archives tab | ✅ | ✅ `archives_tab` `archives_tab.h:9` | ✅ |
| Right panel tab system | ✅ | ✅ `right_panel` `right_panel.h:25` + `tab_panels` | ✅ |
| Main tab container (Full UI mode) | ❌ | 🚀 `MainTabContainer` `main_tab_container.h:18` (permanent Main + dynamic tabs) | 🚀 |
| Desktop shortcut management | ✅ `env::Shortcut` (IShellLink COM) | ❌ | ❌ |
| Shell context menu integration | ✅ `env::ShellMenu` / `env::ShellMenuCollection` (IContextMenu COM) | ❌ | ❌ |
| CopyEventFilter (Ctrl+C in views) | ✅ `CopyEventFilter` | ❌ | ❌ |
| Message dialog (fire-and-forget toast) | ✅ `MessageDialog` (borderless, auto-timeout) | ❌ | ❌ |
| About dialog | ✅ `AboutDialog` | ✅ `QMessageBox::about` (version string) `settings_controller.cpp:754` | ✅ |
| U001 Menu bar structure (MO2: File/View/Tools/Run/Help, no Edit menu); GMM ruling = generic "visit modding sites" action (site list knowledge/plugin-driven, NOT Nexus-tied) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.ui:1580-1596` | ❌ generic "visit modding sites" action not implemented (user ruling: knowledge/plugin-driven site list, NOT Nexus-tied) | ❌ |
| U002 Ctrl+M = Install Mod... | ✅ `mainwindow.ui:1655-1675` | ❌ | ❌ |
| U008 Help system (Ctrl+H dropdown); ruling = expansive thorough Help system - menu tree + docs content project | ✅ `mainwindow.ui:1838-1858` | ❌ expansive Help system (menu tree + docs content project - user ruling) not started | ❌ |
| U009 F5 = Refresh | ✅ `mainwindow.ui:1989` | ✅ `menu_bar.cpp:179` (QKeySequence::Refresh) | ✅ |
| U012 Help menu tree (Help on UI, Documentation, Wiki, Discord, Report Issue, Tutorials submenu, About) - see U008 ruling | ✅ `mainwindow.cpp:1080-1163` | ❌ | ❌ |
| U017 Toolbar right-align spacer before last separator | ✅ `mainwindow.cpp:713-744` | ❌ no right-align spacer found in main toolbar | ❌ |
| U018 Toolbar menu-buttons use QToolButton::InstantPopup | ✅ `mainwindow.cpp:746-753` | ❌ | ❌ |
| U019 View > Toolbars submenu (9 checkables: menu/toolbar/statusbar, 3 icon sizes, 3 style modes) | ✅ `mainwindow.ui:1558-1578`, `mainwindow.cpp:797-890` | ❌ | ❌ |
| U021 Popup menu on toolbar/central-widget edges (Toolbars + View Log) | ✅ `mainwindow.cpp:821-840`, `:906-924` | ❌ | ❌ |
| U023 Open Folder menu (12 entries: game/MyGames/INIs, instance/mods/profile/downloads, install/plugins/stylesheets/logs) | ✅ `mainwindow.cpp:2663-2693` | ⚠️ FolderKind handling for 11 folders `mod_list_controller.cpp:3544`; full 12-item menu layout unproven | ⚠️ |
| U031 Game Support Wiki first-run info dialog | ✅ `mainwindow.cpp:1268-1278` | ❌ | ❌ |
| U041 Right-click central widget edges shows popup menu | ✅ `mainwindow.cpp:906-924` | ❌ | ❌ |
| U048 Qt effects disabled at startup (menu/combo/tooltip animations) | ✅ `mainwindow.cpp:240-252` | ❌ | ❌ |
| U070 StatusBar carries Nexus API stats + user account (requestsChanged/credentialsReceived) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:270-276`, `:467-469` | ⚠️ status bar + API counter text exist `status_bar.cpp:101`; account/signal wiring unproven | ⚠️ |
| U100 Status bar "%1 - %2 - %3" game/instance/profile message | ✅ `statusbar.cpp:150-167` | ❌ | ❌ |
| U101 Status bar progress ("Loading...", 0-100, max width 300, spacers) | ✅ `statusbar.cpp:63-80` | ❌ | ❌ |
| U102 Status bar visibility compensates central-widget bottom margin | ✅ `statusbar.cpp:175-192` | ❌ | ❌ |
| U103 StatusBarAction icon+label wrapper | ✅ `statusbar.cpp:195+` | ❌ | ❌ |
| U187 PreviewDialog (Preview / Close buttons) | ✅ `previewdialog.ui` | ⚠️ preview window exists `preview_window.h:61`; modal Preview/Close dialog parity unproven | ⚠️ |
| U219 Menu aboutToShow refresh pattern (lazy rebuild) + wheel-block combo | ✅ `mainwindow.cpp:455-474` | ❌ | ❌ |
| U224 languageChange rebuilds help menu + resetActionIcons | ✅ `mainwindow.cpp:564-600`, `:2912-2948` | ❌ | ❌ |
| U241 IPreviewPlugin gating Preview menu | ✅ `filetree.cpp:741` | ⚠️ v2 IPluginPreview registry exists `preview/preview_registry.h:14` (lookup `preview_window.cpp:784`); Preview-menu gating unproven | ⚠️ |
| U264 SortableTreeWidget + setCustomizableColumns | ✅ `sortabletreewidget.cpp` (uibase) | ❌ | ❌ |
| U265 ExpanderWidget / LinkLabel / LineEditClear UI primitives | ✅ `uibase src` | ❌ | ❌ |
| U266 TaskProgressManager/TaskProgress (Windows taskbar progress) | ✅ `taskprogressmanager` (uibase) | ❌ | ❌ |
| U269 EventFilter generic event filter (uibase) | ✅ `eventfilter.cpp` (uibase) | ❌ | ❌ |
| U295 statusbar visibilityChanged margin compensation (see U102) | ✅ `statusbar.cpp` | ❌ | ❌ |

## 31. Log System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Log model (QAbstractItemModel) | ✅ `LogModel` | ✅ `Logger` `logger.h:20` (callback-based, replay buffer `:61`) | ✅ |
| Log list view | ✅ `LogList` | ✅ `console_panel` `console_panel.h:11` | ✅ |
| Log copy to clipboard | ✅ `LogList::copyToClipboard()` | ✅ `console_panel.cpp:37` (QShortcut Copy on output_) | ✅ |
| Log open logs folder | ✅ `LogList::openLogsFolder()` | ❌ | ❌ |
| Log clear | ✅ `LogList::clear()` | ✅ `ConsolePanel::clear()` `console_panel.h:18` | ✅ |
| Log highlighter | ✅ `LogHighlighter` | ❌ | ❌ |
| Log level filtering | ✅ | ✅ `Logger::set_level()` `logger.h:27` | ✅ |
| Group logging (begin/end) | ❌ | 🚀 `Logger::begin_group()/end_group()` `logger.h:31-32` | 🚀 |
| Replay buffer (256 entries) | ❌ | 🚀 `Logger` late subscriber replay `logger.cpp:50-56` | 🚀 |
| Fork-safe append | ❌ | 🚀 `Logger::raw_append()` `logger.h:47` | 🚀 |
| Log initialization (spdlog, UTC timestamps, pattern) | ✅ `initLogging()` | ⚠️ timestamps exist `logger.cpp:151` but localtime not UTC, no spdlog/pattern | ⚠️ |
| Log blacklisting (privacy - username masking) | ✅ `log::getDefault().addToBlacklist()` | ❌ | ❌ |
| Log to stdout (via Console attach) | ✅ `logToStdout()` | ✅ `logger.cpp:122` (fprintf stdout) | ✅ |
| U020 View > Log checkable action toggles log dock | ✅ `mainwindow.ui:1981`, `mainwindow.cpp:816-819` | ✅ `menu_bar.cpp:122` (Show Console toggle) | ✅ |
| U057 errorReported() scans newest log first 50000 lines for ERROR | ✅ `mainwindow.cpp:993-1022` | ❌ | ❌ |
| U157 Log list context menu (Copy/Copy all/Clear all/Open folder/Level submenu) | ✅ `loglist.cpp:250-290` | ⚠️ copy shortcut `console_panel.cpp:37` + level filtering `logger.h:27`; context menu + Level submenu unproven | ⚠️ |
| U158 Log file creation failure critical dialog | ✅ `loglist.cpp:384-385` | ❌ | ❌ |
| U294 logDock QDockWidget (area 8 bottom, View>Log toggle) | ✅ `mainwindow.ui:1510` | ⚠️ log dock toggle exists `menu_bar.cpp:122`; QDockWidget bottom-area parity unproven | ⚠️ |

## 32. System Tray

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| System tray icon | ✅ `SystemTrayManager` | ⚠️ `SystemTrayManager` `system_tray_manager.h:16` exists, context menu works `system_tray_manager.cpp:18`, not wired for minimize | ⚠️ |
| Minimize to system tray | ✅ `minimizeToSystemTray()` | ⚠️ `closeEvent()` `main_window.cpp:454` does not check minimize-to-tray setting | ⚠️ |
| Restore from system tray | ✅ `restoreFromSystemTray()` | ⚠️ tray icon Show action exists `system_tray_manager.cpp:18`, no restore-from-minimized logic | ⚠️ |
| Tray notification | ✅ `showNotification()` | ⚠️ `SystemTrayManager::show_notification()` `system_tray_manager.h:24` (exists, not wired up) | ⚠️ |
| U222 Finished-run while hidden -> restoreFromSystemTray | ✅ `mainwindow.cpp:~508` | ⚠️ tray restore path exists `system_tray_manager.cpp:18`; auto-restore-after-run unproven | ⚠️ |

## 33. Self Updater

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Self-update system (GitHub releases) | ✅ `SelfUpdater` | ✅ `SelfUpdater` `self_updater.h:30` with platform-specific updaters (Windows/macOS/Linux/Flatpak/AUR/AppImage) | ✅ |
| Update candidates (version-sorted) | ✅ `CandidatesMap` | ❌ | ❌ |
| Update backup + restart | ✅ `installUpdate()` + `restart()` | ⚠️ restart exists `self_updater.h:43`, no backup step found | ⚠️ |
| Check for updates setting | ✅ | ✅ `check_for_updates()` `settings.h:94` | ✅ |
| Update download with progress dialog | ✅ `selfupdater.cpp` showProgress(), QProgressDialog | ⚠️ progress_cb exists `self_updater.h:40` (per-updater e.g. `windows_self_updater.cpp:48`), no QProgressDialog | ⚠️ |
| Offline mode check before update | ✅ `selfupdater.cpp` testForUpdate() respects offline mode | ❌ (offline_mode exists in NetworkOptions, not consulted by SelfUpdater) | ❌ |
| U027 Update action disabled-by-default + tooltip flip | ✅ `mainwindow.ui:1799-1819`, `mainwindow.cpp:2964-2969` | ❌ | ❌ |
| U131 General > Updates group (Check for updates + Update to beta versions) | ✅ `settingsdialog.ui:199-232` | ⚠️ check-for-updates checkbox exists `settings_content_widget.cpp:111` (key `settings.cpp:230`); beta-updates toggle unproven | ⚠️ |
| U185 UpdateDialog (Changelog web view, Install/Cancel, version label) | ✅ `updatedialog.cpp:69`, `updatedialog.ui` | ❌ | ❌ |

## 34. Multi-Process / IPC

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Multi-process guard (shared memory) | ✅ `MOMultiProcess` (QSharedMemory + QLocalServer) | ✅ `MultiProcess` QLockFile + QLocalServer `multi_process.h:9-11`, wired `core.cpp:331` (was mis-cited: `single_instance`/QtSingleApplication absent) | ✅ |
| Ephemeral process (forward download) | ✅ `MOMultiProcess::ephemeral()` | ❌ | ❌ |
| Secondary instance (allow multiple) | ✅ `MOMultiProcess::secondary()` | ❌ | ❌ |
| Message passing between instances | ✅ `sendMessage()` / `messageSent()` | ✅ `nxm_ipc` (nxm:// forwarding) `nxm_ipc.h:9`, generic URL forwarder `nxm_ipc.h:44` | ✅ |
| Command-line global options (--pick, --multiple, --logs, -i, -p) | ✅ `CommandLine` global options | ❌ | ❌ |
| Forward to primary instance | ✅ `CommandLine::forwardToPrimary()` | ⚠️ URL forwarding to running instance works `core.cpp:473`, `:536`; no general CLI-arg forward | ⚠️ |
| NXM/moshortcut:// link protocol parsing | ✅ `CommandLine` handles moshortcut:// and nxm:// | ⚠️ nxm:// parsed `command_line.cpp:30`, modl:// `command_line.cpp:38`; moshortcut:// absent | ⚠️ |

## 35. Text Editor

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Text editor (line numbers, syntax, word wrap) | ✅ `TextEditor` | ✅ line numbers `generic_files_tab.cpp:49` (LineNumberPlainTextEdit) + syntax `generic_files_tab.cpp:59` (KSyntaxHighlighting), edit+save `generic_files_tab.cpp:211` | ✅ |
| HTML editor | ✅ `HTMLEditor` | ❌ (QWebEngineView renderer is a read-only viewer, `webview_description_renderer.h:5`) | ❌ |
| U190 TextViewer (multi-tab, per-file writable, Find, Save-per-page, save prompt) | ✅ `textviewer.cpp:60-276` | ⚠️ multi-file editor tab exists `generic_files_tab.cpp:49` (save_editor + write warning `:211`); Find actions unproven | ⚠️ |
| U191 TextViewer read-only INI write TaskDialog (Clear flag / Allow once / Skip) | ✅ `textviewer.cpp:173-192` | ❌ | ❌ |
| U192 FindDialog (find-only, Find Next, Close) | ✅ `finddialog.ui` | ❌ | ❌ |
| U248 TextEditor toolbar per-file (Save, Word wrap toggle, Open in Explorer) + dirty flag | ✅ `texteditor.cpp:468-491` | ⚠️ save button in editor bar `generic_files_tab.cpp:63` (editor `:49`); Word wrap/Open in Explorer actions unproven | ⚠️ |
| U249 Line-number gutter + current-line highlight + TextEditorHighlighter | ✅ `texteditor.cpp:13-14`, `:286-330` | ⚠️ syntax highlighter exists `generic_files_tab.cpp:59` (gutter via LineNumberPlainTextEdit `:49`); current-line highlight unproven | ⚠️ |
| U250 TextViewer multi-file tabs w/ per-tab Save + Find | ✅ `textviewer.cpp` | ❌ | ❌ |

## 36. Browser

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Integrated browser (QWebEngineView) | ✅ `BrowserDialog` | ⚠️ QWebEngineView present but description-renderer only `webview_description_renderer.cpp:110`; no browser UI - external browser launch via `custom_browser_command` `settings.h:157` | ⚠️ |
| Browser tabs | ✅ `BrowserDialog::m_Tabs` | ❌ | ❌ |
| Browser download interception | ✅ `unsupportedContent()` | ❌ | ❌ |
| U049 QWebEngineProfile config (no persistent cookies, 50MB cache, custom paths) | ✅ `mainwindow.cpp:254-260` | ❌ | ❌ |
| U245 BrowserDialog controls (closeable tabs, hidden urlEdit, nav buttons, new-tab titles) | ✅ `browserdialog.cpp:58-284` | ❌ | ❌ |
| U246 Browser URL bar toggle + returnPressed navigation | ✅ `browserdialog.cpp:270-284` | ❌ | ❌ |
| U247 guessFileName + requestDownload signal flow | ✅ `browserdialog.cpp:169-212`, `:204` | ❌ | ❌ |

## 37. Dialogs

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| About dialog | ✅ `AboutDialog` | ✅ `QMessageBox::about` (version string) `settings_controller.cpp:754` | ✅ |
| Update dialog (changelog) | ✅ `UpdateDialog` | ❌ | ❌ |
| MOTD dialog | ✅ `MotDDialog` | ❌ | ❌ |
| Problems dialog (guided fixes) | ✅ `ProblemsDialog` | ❌ | ❌ |
| Selection dialog (generic picker) | ✅ `SelectionDialog` | ✅ `ListDialog` `list_dialog.h:20` (filter, auto-select, geometry) | ✅ |
| Overwrite info dialog | ✅ | ✅ `OverwriteInfoDialog` `overwrite_info_dialog.h:20` (QFileSystemModel) | ✅ |
| Query overwrite dialog | ✅ | ✅ `QueryOverwriteDialog` `query_overwrite_dialog.h:20` (thread-safe) | ✅ |
| Sync overwrite dialog | ✅ | ✅ `SyncOverwriteDialog` `sync_overwrite_dialog.h:23` (per-file combo) | ✅ |
| Credentials dialog | ✅ `CredentialsDialog` | ❌ | ❌ |
| List dialog | ✅ `ListDialog` | ✅ `ListDialog` `list_dialog.h:20` | ✅ |
| Save text as dialog | ✅ `SaveTextAsDialog` | ❌ | ❌ |
| Message dialog (fire-and-forget toast) | ✅ `MessageDialog` | ❌ | ❌ |
| Category import dialog | ✅ `CategoryImportDialog` | ❌ | ❌ |
| Activate mods dialog (save-game asset resolution) | ✅ `ActivateModsDialog` | ❌ | ❌ |
| Disable proxy plugin dialog | ✅ `DisableProxyPluginDialog` | ❌ | ❌ |
| U174 DisableProxyPluginDialog detail (plugin table, restart note, Yes/No) | ✅ `disableproxyplugindialog.ui` | ❌ | ❌ |
| U178 ActivateModsDialog text + Missing ESP/Mod columns | ✅ `activatemodsdialog.ui`, `savestab.cpp:277-300` | ❌ | ❌ |
| U180 About dialog fields (Revision, usvfs, GitHub link, Used Software, Thanks, contributors) | ✅ `aboutdialog.ui`, `aboutdialog.cpp:117` | ❌ GMM uses QMessageBox::about only (`settings_controller.cpp:754`) | ❌ |
| U182 SelectionDialog (Select/Cancel + choice descriptions) | ✅ `selectiondialog.ui` | ⚠️ shared ListDialog choice picker exists `mod_actions.cpp:193`; per-choice description rows unproven | ⚠️ |
| U183 SaveTextAsDialog (Copy To Clipboard / Save As / Close) | ✅ `savetextasdialog.ui` | ❌ | ❌ |
| U184 CredentialsDialog (Nexus login, Remember/Never ask again) (NEXUS-LENS: genericize/provider-scope) | ✅ `credentialsdialog.ui` | ❌ | ❌ |
| U186 MotDDialog (Message of the Day + OK) | ✅ `motddialog.ui` | ❌ | ❌ |

## 38. Platform Abstraction

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Windows (native) | ✅ | ✅ `platform.h:141/:144` (home_dir/temp_dir) + windows_platform impl | ✅ |
| Windows (MSVC compile) | ✅ | ✅ `build-windows.ps1` (MSVC toolchain checks) | ✅ |
| Linux (native) | ❌ | 🚀 full Linux support `linux_platform.h:23-47` | 🚀 |
| Linux (OverlayFS) | ❌ | 🚀 `OverlayFsLauncher` `overlay_launcher.h:14` | 🚀 |
| Linux (cgroup v2) | ❌ | 🚀 `cgroup_is_empty` `launcher.h:142` | 🚀 |
| Linux (subreaper) | ❌ | 🚀 `PR_SET_CHILD_SUBREAPER` `launcher.cpp:193` | 🚀 |
| Linux (Proton/Wine) | ❌ | 🚀 `ProtonRuntime` `runtime.h:45` | 🚀 |
| macOS | ❌ | ⚠️ platform_interface stubs `macos_platform.h:30` (is_elevated/symlinks present) | ⚠️ |
| PlatformInterface (XDG, Steam, Proton) | ❌ | 🚀 `platform_interface.h` (home_dir `platform.h:141`, temp_dir `:144`, etc.) | 🚀 |
| PathResolver (canonical paths) | ❌ | 🚀 `PathResolver` `path_resolver.h:34` + `PathResolverRegistry` | 🚀 |
| Keyring (OS-backed + file fallback) | ❌ | 🚀 `Keyring` `keyring.h:11` + `FileKeyring` `keyring.h:26` (XOR+base64) | 🚀 |
| Thread priority (low) | ❌ | 🚀 `set_low_priority()` `thread_priority.h:16` | 🚀 |
| Headless launcher (CLI) | ❌ | 🚀 `cli::HeadlessLauncher` `headless_launcher.h:14` (`HeadlessLauncher::Config` `:16`) | 🚀 |
| Proton version discovery | ❌ | 🚀 `find_proton()`, `enumerate_proton_versions()` `proton_tools.h:37` | 🚀 |
| Wine binary discovery | ❌ | 🚀 `find_wine()` `platform.h:124`, linux impl `linux_platform.cpp:490` | 🚀 |
| Admin elevation check | ❌ | 🚀 `is_elevated()` `platform.h:132`, windows impl `windows_platform.cpp:187` | 🚀 |
| Symlink/junction capability check | ❌ | 🚀 `symlinks_available()` `platform.h:135` / `junctions_available()` `platform.h:138` | 🚀 |
| Environment variable management (get/set/path) | ✅ `env::get()`, `env::set()`, `env::path()` | ❌ | ❌ |
| PATH manipulation helpers (append/prepend/set) | ✅ `env::appendToPath()`, `prependToPath()` | ❌ | ❌ |

## 39. Theme System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Theme manager (QSS token substitution) | ❌ | 🚀 `ThemeManager` `theme_manager.h:19` (scan, load, apply, live-reload) | 🚀 |
| Icon manager | ❌ | 🚀 `IconManager` `icon_manager.h:40` | 🚀 |
| Style manager | ❌ | 🚀 `StyleManager` `style_manager.h:21` | 🚀 |
| U135 Theme tab (styleBox combo + Explore... + ColorTable + Reset Colors) | ✅ `settingsdialog.ui:415-531` | ⚠️ style manager exists `style_manager.h:21` + theme tab `settings_content_widget.cpp:222`; ColorTable/Reset Colors unproven | ⚠️ |

## 40. Event System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Event bus (subscribe/dispatch) | ❌ | 🚀 `EventBus` `event_bus.h:62` (17 canonical events) | 🚀 |
| Event history ring buffer | ❌ | 🚀 500-entry `EventRecord` history `event_bus.h:85` | 🚀 |
| Plugin-scoped unsubscription | ❌ | 🚀 `clear_source()` on plugin unload `event_bus.h:95` | 🚀 |
| JSON payload helpers | ❌ | 🚀 `json_obj()` `event_bus.h:120` | 🚀 |

## 41. External Tool System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| External tool registry | ❌ | 🚀 `ToolRegistry` `tool_registry.h:38` (Advisory/Workshop kinds) | 🚀 |
| Dynamic tools in menu bar | ❌ | 🚀 `AppMenuBar::update_tools_for_game()` `menu_bar.h:24` | 🚀 |
| Proton prefix tools | ❌ | 🚀 `run_proton_tool()` `proton_tools.h:37` (winetricks/protontricks) | 🚀 |
| U005 Ctrl+I = Tool Plugins (iconText "&Tools") | ✅ `mainwindow.ui:1718-1735` | ❌ | ❌ |
| U015 Tool Plugins menu (displayName "/" grouping, tooltips, error routing) | ✅ `mainwindow.cpp:1495-1566` | ❌ | ❌ |
| U067 Plugin tool exception routing (reportError queued) | ✅ `mainwindow.cpp:1495-1522` | ❌ | ❌ |
| U237 IPluginTool plugin API | ✅ `mainwindow.cpp:1495` (uibase) | ❌ | ❌ |

## 42. Packaging & Distribution

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| NSIS installer | ✅ | ⚠️ cmake target exists `CMakeLists.txt:331-339` (runs makensis when found) | ⚠️ |
| Standalone zip | ✅ | ❌ UNPROVEN (was: ✅ `package-windows-standalone` - target is a TODO echo stub, `Packaging/windows/standalone/CMakeLists.txt:11-12`) | ❌ |
| USVFS binaries in package | ✅ | ❌ UNPROVEN (was: ✅ `cmake/usvfs.cmake` - file absent, `projects/Core/cmake/` has only libloot scripts; USVFS = ticket Workspace-3br4) | ❌ |
| AppImage (Linux) | ❌ | ❌ UNPROVEN (was: 🚀 `linux/appimage` - target is a TODO echo stub, `Packaging/linux/appimage/CMakeLists.txt:11-12`) | ❌ |
| Flatpak (Linux) | ❌ | ❌ UNPROVEN (was: 🚀 `linux/flatpak` - target is a TODO echo stub, `Packaging/linux/flatpak/CMakeLists.txt:11-12`) | ❌ |
| DMG (macOS) | ❌ | ❌ UNPROVEN (was: 🚀 `macos/dmg` - target is a TODO echo stub, `Packaging/macos/dmg/CMakeLists.txt:11-12`) | ❌ |
| U289 About "Used Software" third-party licenses tab | ✅ `aboutdialog.ui` | ❌ | ❌ |
| U290 AppManifest (DPI awareness, UAC) + dlls.manifest.qt6 | ✅ `app.manifest`, `dlls.manifest.qt6` | ❌ | ❌ |
| U291 modorganizer.natvis debugger visualizers | ✅ `modorganizer.natvis` | ❌ no .natvis file found in repo | ❌ |
| U292 Build tooling dependency set (CMakePresets, vcpkg.json) | ✅ `vcpkg.json` | ⚠️ deps + presets proven `vcpkg.json:1` + `CMakePresets.json:1`; exact MO2 dep parity unproven | ⚠️ |

## 43. CLI Command System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| CLI crashdump command | ✅ `cl::CrashDumpCommand` (dump running MO process) | ❌ | ❌ |
| CLI launch command (spawn-wait) | ✅ `cl::LaunchCommand` (CreateProcessW + WaitForSingleObject) | ⚠️ `--launch` headless exists `command_line.cpp:22`, `headless_launcher.h:14`; no spawn-wait parity proven | ⚠️ |
| CLI run command (executable with USVFS) | ✅ `cl::RunCommand` (-e name, -a args, -c cwd) | ⚠️ `--exe` path option exists `command_line.cpp:26`; no USVFS (stub `launcher.cpp:841`) | ⚠️ |
| CLI reload-plugin command | ✅ `cl::ReloadPluginCommand` (hot-reload by name) | ❌ | ❌ |
| CLI download-file command | ✅ `cl::DownloadFileCommand` (URL + metadata, HTTPS validation) | ❌ | ❌ |
| CLI refresh command (F5 equivalent) | ✅ `cl::RefreshCommand` | ❌ | ❌ |
| CLI --help | ✅ `CommandLine::showHelp()` | ✅ `show_help` flag + rendered help `command_line.cpp:46`, `:67-108` | ✅ |
| CLI --multiple (allow multiple instances) | ✅ `CommandLine` --multiple flag | ❌ | ❌ |
| CLI --pick (show instance selector) | ✅ `CommandLine` --pick flag | ❌ | ❌ |
| CLI --logs (duplicate logs to stdout) | ✅ `CommandLine` --logs flag | ❌ (stdout logging always-on `logger.cpp:122`, but no --logs flag) | ❌ |
| CLI -i (instance selection) | ✅ `CommandLine` -i flag | ⚠️ `--instance` long option exists `command_line.cpp:18`; no `-i` short flag | ⚠️ |
| CLI -p (profile selection) | ✅ `CommandLine` -p flag | ❌ | ❌ |

## 44. Tutorial System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U028 First-run "Show tutorial?" dialog + Never ask checkbox | ✅ `mainwindow.cpp:1215-1230` | ❌ | ❌ |
| U068 Window tutorial hookup ("//WIN" headers, shouldStartTutorial, expose modList/espList) | ✅ `mainwindow.cpp:1187-1232` | ❌ | ❌ |
| U261 Tutorial system (TutorialManager, TutorialDialog, tabChanged, expose) | ✅ `mainwindow.cpp:1187-1232` | ❌ | ❌ |
| U288 Tutorial resources shipped (massage_messages.py, tutorials/*.js) | ✅ `src/massage_messages.py` | ❌ | ❌ |

## 45. TaskDialog Component

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U267 TaskDialog component (multi-button dialog reused by restart/delete/INI/overwrite flows) | ✅ `taskdialog.ui` (uibase) | ❌ | ❌ |

## 46. Notifications / Problems System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U026 Notifications action badge (badge_0..9/more pixmap, disabled at 0, tooltips, statusbar sync) | ✅ `mainwindow.cpp:926-991`, `mainwindow.ui:1820-1837` | ❌ | ❌ |
| U056 Problems check (500ms debounce + QtConcurrent + mutex + recheck flag) | ✅ `mainwindow.cpp:926-931`, `:1024-1054` | ❌ | ❌ |
| U181 Notifications/Problems dialog (per-item Fix button, empty placeholder) | ✅ `problemsdialog.cpp:60-75` | ❌ | ❌ |

## 47. Backup / Restore (Load Order + Mod List)

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U034 Load order backup/restore (timestamped plugins/loadorder/lockedorder, SelectionDialog, error strings) | ✅ `mainwindow.cpp:3841-3916` | ❌ | ❌ |
| U035 Mod list backup/restore (save/restore modlist.txt + toasts) | ✅ `mainwindow.cpp:3918-3940` | ❌ | ❌ |
| U036 Backup naming scheme (.yyyy_MM_dd_hh_mm_ss, keep 10 newest) | ✅ `mainwindow.cpp:3823-3840` | ❌ | ❌ |

## 48. File Tree Menu Protocol

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U152 File tree context menu protocol (Add as Executable, Reveal, Hide/Un-Hide, Open block, Preview bolding, Save/Refresh/Expand/Collapse) | ✅ `filetree.cpp:640-810` | ⚠️ Hide/Un-Hide + Refresh w/ status-tips exist `data_tab.cpp:811`, `:830` (tips `:815`, `:833`); Add-as-Executable/Preview/Open blocks unproven | ⚠️ |
| U153 MenuItem status-tip protocol (hint + "Disabled because:" + disabledHint) | ✅ `filetree.cpp:60-115`, `:647` | ⚠️ status-tip hints exist `data_tab.cpp:727` (menu tips `:815`/`:833`); the "Disabled because:" protocol unproven | ⚠️ |
| U154 File tree multi-select detail captions ("only has %1 file(s)") | ✅ `filetree.cpp:618-626` | ❌ | ❌ |

## 49. CLI Help Grammar

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U233 Full CommandLine grammar (visible options, -i list mode, positional subargs, help layout, error paths) | ✅ `commandline.cpp:311-465` | ❌ help TEXT, error paths, -i list mode, multi-process note all absent | ❌ |
| U234 cl:: commands grammar (crashdump/launch/run/reload-plugin/download/refresh + options/errors/forwarding) | ✅ `commandline.cpp:591-940` | ❌ options/errors/forwarding help unproven | ❌ |

## 50. My Games Resolution

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| My Games resolution (per-game Documents/My Games dir) | ✅ (per-game My Games path resolution) | ✅ `game_knowledge.cpp:181` resolve_mygames_dir() + `main_window.h:396` game_mygames_dir() (test `mygames_test.cpp:57`) | ✅ |

## 51. Conflict Scan Refresh

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Conflict scan refresh (rescan finishes via controller slot) | ✅ (conflict scan completion -> model refresh) | ✅ `mod_list_controller.cpp:2214` on_conflict_scan_finished() + settings wiring `settings_controller.cpp:208` | ✅ |

---

# Summary

[↑ Back to the top ↑](#feature-map---mo2-vs-gmm)

| Category | Matched ✅ | Partial ⚠️ | Surpasses 🚀 | Missing ❌ |
|----------|-----------|-----------|-------------|-----------|
| Virtual Filesystem | 5 | 2 | 5 | 5 |
| Launch Pipeline | 6 | 5 | 5 | 16 |
| Error Handling | 4 | 2 | 0 | 50 |
| Settings | 21 | 11 | 3 | 64 |
| Executable Management | 8 | 6 | 4 | 9 |
| Mod Management | 22 | 5 | 8 | 11 |
| Mod Categories | 6 | 4 | 0 | 11 |
| Mod Conflict Detection | 4 | 2 | 3 | 7 |
| Mod Content Analysis | 6 | 7 | 0 | 13 |
| Mod Info Dialog | 10 | 1 | 1 | 2 |
| Version & Update Management | 3 | 1 | 2 | 12 |
| Plugin Management | 25 | 6 | 3 | 16 |
| LOOT Integration | 10 | 2 | 1 | 7 |
| Profile Management | 26 | 6 | 3 | 6 |
| Download Management | 12 | 9 | 5 | 43 |
| Nexus Integration | 8 | 10 | 2 | 20 |
| Source Providers | 4 | 0 | 7 | 1 |
| Mod List Features | 17 | 8 | 3 | 20 |
| Mod Context Menu | 14 | 4 | 0 | 14 |
| Plugin Context Menu | 2 | 2 | 0 | 9 |
| Archive & Installation | 7 | 1 | 4 | 8 |
| Deploy System | 0 | 0 | 12 | 0 |
| Overwrite System | 7 | 2 | 3 | 1 |
| Save Game System | 13 | 5 | 5 | 4 |
| Game Detection & Knowledge | 2 | 2 | 11 | 0 |
| Pipeline System | 0 | 0 | 16 | 0 |
| Plugin Host System | 0 | 1 | 12 | 3 |
| Sort System | 0 | 0 | 4 | 0 |
| Instance Management | 3 | 1 | 6 | 7 |
| UI Layer | 19 | 4 | 19 | 26 |
| Log System | 7 | 3 | 3 | 5 |
| System Tray | 0 | 5 | 0 | 0 |
| Self Updater | 2 | 3 | 0 | 4 |
| Multi-Process / IPC | 2 | 2 | 0 | 3 |
| Text Editor | 1 | 3 | 0 | 4 |
| Browser | 0 | 1 | 0 | 6 |
| Dialogs | 6 | 1 | 0 | 15 |
| Platform Abstraction | 2 | 1 | 14 | 2 |
| Theme System | 0 | 1 | 3 | 0 |
| Event System | 0 | 0 | 4 | 0 |
| External Tool System | 0 | 0 | 3 | 4 |
| Packaging | 0 | 2 | 0 | 8 |
| CLI Command System | 1 | 3 | 0 | 8 |
| Tutorial System | 0 | 0 | 0 | 4 |
| TaskDialog Component | 0 | 0 | 0 | 1 |
| Notifications / Problems System | 0 | 0 | 0 | 3 |
| Backup / Restore (Load Order + Mod List) | 0 | 0 | 0 | 3 |
| File Tree Menu Protocol | 0 | 2 | 0 | 1 |
| CLI Help Grammar | 0 | 0 | 0 | 2 |
| My Games Resolution | 1 | 0 | 0 | 0 |
| Conflict Scan Refresh | 1 | 0 | 0 | 0 |
| **TOTAL** | **287** | **136** | **174** | **458** |
