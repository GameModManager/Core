# Feature Map - MO2 vs GMM

Lightweight parity tracker. One row per feature, one line per cell: **a status
plus a pointer, nothing else.**

> ### USVFS is a PLANNED port, not a rejected feature
>
> USVFS is intended to be ported from MO2 and has not been started, so every row
> whose subject is USVFS machinery is `⏳`. **`⏳` is not `🚫`:** `⏳` is wanted
> work deferred behind the standing no-Windows-code-before-parity decision,
> `🚫` is a Windows-shaped mechanism with no Linux subject and is closed for
> good. **Absence is what an unstarted port looks like** - do not mark a USVFS
> row fabricated because its symbols are absent. Port tracked on
> `Workspace-3br4`.

**Cell format:** `<status> <what it is> - <file:line>`

| Marker | Meaning |
|--------|---------|
| `✅` | Shipped, done well |
| `🚀` | GMM better than MO2, or MO2 lacks it |
| `⚠️` | Partially there |
| `❌` | Missing, wanted now |
| `⏳` | Planned, not started - even when MO2 already has it |
| `🚫` | Not applicable / will not port |

**A `✅` is only for something GMM does WELL.** Matching a MO2 limitation is
never a `✅` - it is `🚀` if we do it better, or `❌`/`⏳` if we do not have it.

**Verification** (second token in Status): `✔` re-verified against the vendored
MO2 source and the live `src/` tree; `·` carried over unverified, a lead not a
finding. A `✔` means the row was checked, not that the feature is complete.

`[win]` = blocked by the standing decision not to write Windows code until MO2
parity. Excluded from the parity denominator.

---

### Reading the parity number

`MO2 parity` = `✅ / (✅ + ⚠️ + ❌)`, and it is a lower bound on the work, not
a measure of quality: a `✅` row means the behaviour matches, not that the
feature is finished. `🚀` rows are excluded (a GMM-only capability is neither a
parity success nor a failure) and `[win]` rows are excluded (blocked by
decision, not effort). A `·` row is a lead, not a finding.

### Windows-blocked items

`[win]` rows split by disposition, and the split matters for scheduling:

- `⏳` - wanted, not started, in the backlog. When the USVFS port starts they
  are the spec.
- `🚫` - genuinely Windows-shaped, no Linux subject, will never be done.

[Jump to **summary**](#summary)

---

## 1. Virtual Filesystem

`⏳` rows are the USVFS port; `🚀` rows are the Linux-native VFS GMM ships
instead. See the USVFS note at the top of this file.

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| USVFS controller (dll loading) | ✅ `usvfs_x64.dll` | ⏳ planned port, not started - Workspace-t83d | ⏳ ✔ [win] |
| VFS create/reset | ✅ `usvfsCreateVFS` | ⏳ planned port, not started - Workspace-y2tx | ⏳ · [win] |
| Directory-level virtual links | ✅ `usvfsVirtualLinkDirectoryStatic` | ⏳ planned port, not started - Workspace-y2tx | ⏳ · [win] |
| File-level virtual links | ✅ `usvfsVirtualLinkFile` | ⏳ planned port, not started - Workspace-mjgr | ⏳ · [win] |
| Priority-ordered mod mapping | ✅ `OrganizerCore::fileMapping` | ✅ staging lowerdir - `instance_utils.cpp:362` | ✅ · |
| Create-target (write destination) | ✅ `LINKFLAG_CREATETARGET` | ✅ upper dir - `overlay_launcher.cpp:118` | ✅ · |
| Custom overwrite target | ✅ `customOverwrite` param | ✅ `relay_output_to_mod` - `fs_utils.h:230` | ✅ · |
| Local saves redirect | ✅ `LocalSavegames::mappings` | ✅ bind-mount install - `overlay_launcher.cpp:358` | ✅ · |
| Plugin file-mapper mappings | ✅ `IPluginFileMapper::mappings` | ✅ `cb_v2_register_file_mapper` - `plugin_loader.cpp:1202` | ✅ · |
| VFS auto-mapping (BSA-aware) | ✅ `DirectoryEntry::addFromBSA` | ⏳ BSA mapping arrives with the port - Workspace-vuam | ⏳ · [win] |
| Archive load order injection | ✅ `enabledArchives` priority | ⚠️ `archives.txt` write only, no injection - `profile_switching.cpp:76` | ⚠️ · |
| Forced library loading | ✅ `usvfs::setForcedLibraries` | ⚠️ profile copy only, no runtime load - `profile_creation.cpp:199` | ⚠️ · |
| OverlayFS (Linux) | ❌ | 🚀 `OverlayFsLauncher` - `overlay_launcher.h:14` | 🚀 · |
| LD_PRELOAD intercept (Linux) | ❌ | 🚀 `PreloadInterceptor` - `preload_interceptor.h:23` | 🚀 · |
| Case-insensitive path resolution | ❌ | 🚀 `resolve_regular_file_ci` - `fs_utils.h:80` | 🚀 · |
| PathResolver registry | ❌ | 🚀 `PathResolverRegistry` - `path_resolver_registry.h:25` | 🚀 · |
| FUSE-based VFS (Linux) | ❌ | ❌ gone with libfuse3; the Linux VFS we ship is OverlayFS - `overlay_launcher.h:14` | ❌ · |

## 2. Launch Pipeline

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Hooked process creation | ✅ `usvfsCreateProcessHooked` | ⏳ Windows launch path is a stub - `launcher.cpp:843` - Workspace-hclp | ⏳ ✔ [win] |
| Plain process creation | ✅ `CreateProcessW` | ✅ execvp - `runtime.cpp:69` | ✅ · |
| Process monitoring (Job Object) | ✅ `CreateJobObjectW` | ❌ no `CreateJobObjectW` in tree | ❌ · [win] |
| Process monitoring (cgroup v2) | ❌ | 🚀 `cgroup_is_empty` + subreaper - `launcher.cpp:811` | 🚀 · |
| Exponential backoff | ✅ 50ms-2s | ❌ only network backoff - `network_manager.cpp:888` | ❌ · [win] |
| Interesting process selection | ✅ `findInterestingProcessInTrees` | ❌ no selection logic | ❌ · [win] |
| Hidden process filtering | ✅ `conhost.exe` + MO2 exe | ❌ no such filter | ❌ · [win] |
| Cancel / force-unlock | ✅ `UILocker::Session` | ✅ Unlock button - `launch_controller.cpp:1655` | ✅ · |
| Exit code capture | ✅ `GetExitCodeProcess` | ✅ `launch_controller.cpp:829` | ✅ · |
| Wait-for-all on app exit | ✅ `waitForAllUSVFSProcessesWithLock` | ⏳ waits on USVFS proxies - Workspace-68y8 | ⏳ · [win] |
| Process tree descendant walk | ✅ | ✅ `get_process_descendants()` - `launcher.cpp:689` | ✅ · [win] |
| Steam -- set SteamAPPId | ✅ `env::set("SteamAPPId", ...)` | ⚠️ Proton path only sets `STEAM_COMPAT_APP_ID` - `runtime.cpp:184` | ⚠️ · |
| Steam -- auto-start | ✅ `checkSteam` + registry `SteamExe` | ❌ | ❌ · [win] |
| Steam -- elevation mismatch | ✅ `canAccess` + admin dialog | ⚠️ generic admin check, not Steam-specific - `windows_platform.cpp:187` | ⚠️ · |
| Steam -- Proton/Wine compat | ❌ | 🚀 `STEAM_COMPAT_*` env - `runtime.cpp:181` | 🚀 · |
| Proton tooling (winetricks/protontricks) | ❌ | 🚀 `run_proton_tool()` fallback chain - `proton_tools.cpp:122` | 🚀 · |
| PATH manipulation | ✅ `env::appendToPath` | ❌ symbol absent repo-wide | ❌ · [win] |
| CWD resolution | ✅ `Executable::workingDirectory` | ✅ `weakly_canonical` + fallback - `launcher.cpp:315` | ✅ · |
| Virtualized binary in mods/ | ✅ `adjustForVirtualized` | ❌ | ❌ · [win] |
| File type dispatch (.bat, .jar) | ✅ `getFileExecutionContext` | ❌ | ❌ · [win] |
| Java detection for .jar | ✅ `findJavaInstallation` | ❌ | ❌ · [win] |
| CREATE_BREAKAWAY_FROM_JOB | ✅ | ❌ no BREAKAWAY handling | ❌ · [win] |
| Subreaper + supervisor | ❌ | 🚀 `PR_SET_CHILD_SUBREAPER` - `launcher.cpp:193` | 🚀 · |
| Wine runtime (non-Steam Windows exe) | ❌ | 🚀 `WineRuntime` - `wine_runtime.h:13` | 🚀 · |
| LaunchParams (structured) | ✅ `SpawnParameters` | ✅ `LaunchParams` (pid, overlay, cgroup, capture) - `launcher.h:12` | ✅ · |
| File association lookup | ✅ `env::getAssociation()` | ❌ | ❌ · [win] |
| Steam-related error dialogs | ✅ `badSteamReg()`, `startSteamFailed()`, `confirmStartSteam()` | ❌ | ❌ · [win] |
| U032 Exit confirm while downloads in progress | ✅ `mainwindow.cpp:1450-1470` | ❌ | ❌ · [win] |
| U170 UILocker dialog messages | ✅ `uilocker.cpp:295-349` | ❌ | ❌ · [win] |
| U235 Env vars set/read (SteamAPPId, credentials, USVFS_*) | ✅ `settingsdialogworkarounds.cpp:17-30` | ⚠️ Proton env only, no credentials or USVFS_* - `runtime.cpp:184` | ⚠️ · |
| U270 SteamUtility (steam process/VDF helpers) | ✅ `steamutility.cpp` (uibase) | ⚠️ VDF/ACF parsing only - `game_detector.h:34` | ⚠️ · |
| U284 processrunner (VFS, waiting UI, cancel, exit code) | ✅ `processrunner.cpp` | ⚠️ cancel + exit code only - `launch_controller.cpp:829` | ⚠️ · |

## 3. Error Handling & Diagnostics

Every row carries `✔` (re-verified 2026-09-30). The `⏳` rows are the USVFS
port - their cited GMM symbol names were invented, the features are not.

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| ERROR_INVALID_PARAMETER (AV quarantine) | ✅ `spawn.cpp:138` (Win32 `DWORD`) | ⏳ planned port - Workspace-udm5 (prior cite invented) | ⏳ ✔ [win] |
| ERROR_ACCESS_DENIED (AV blocking) | ✅ `spawn.cpp:145` (Win32 `DWORD`) | ⏳ planned port - Workspace-udm5 (prior cite invented) | ⏳ ✔ [win] |
| ERROR_FILE_NOT_FOUND (exe missing) | ✅ `spawn.cpp:150` (Win32 `DWORD`) | ⏳ planned port - Workspace-udm5; portable half ships | ⏳ ✔ [win] |
| ERROR_DIRECTORY (bad cwd) | ✅ `spawn.cpp:153` (Win32 `DWORD`) | ⏳ planned port - Workspace-udm5 (prior cite invented) | ⏳ ✔ [win] |
| ERROR_ELEVATION_REQUIRED (admin restart) | ✅ `confirmRestartAsAdmin` `spawn.cpp:250` | 🚫 Linux has no elevation prompt - Workspace-9z66 | 🚫 ✔ [win] |
| makeDetails (owner, ACL, DLL presence) | ✅ `spawn.cpp:66-134` (ACL, USVFS dll probe) | ⚠️ portable half only: exec path in details pane - `launch_controller.cpp:120` | ⚠️ ✔ |
| Blacklist warning dialog | ✅ `confirmBlacklisted` `spawn.cpp:361` | ⏳ blacklist is a VFS directive - Workspace-av1y | ⏳ ✔ [win] |
| Crash dump type selection | ✅ `CrashDumpsType` `usvfsconnector.cpp:106` (Win32 minidump flavours) | ⚠️ no reader; handler writes one fixed backtrace - `settings.cpp:567` | ⚠️ ✔ |
| USVFS child crash capture | ✅ `env::createMiniDump` `env.cpp:1200` (not the cited `usvfsCreateMiniDump`) | ⏳ planned port - Workspace-9nwe | ⏳ ✔ [win] |
| Crash dump pruning | ✅ `cycleDiagnostics` (`mainwindow.cpp`) | ✅ `prune_old_dumps(max_core_dumps())` - `app/core.cpp:116` | ✅ ✔ |
| USVFS log worker thread | ✅ `LogWorker` `usvfsconnector.h` (offloads USVFS log writes) | ⏳ second log stream does not exist yet - Workspace-3br4 | ⏳ ✔ [win] |
| USVFS log file output | ✅ USVFS writes `logs/usvfs-<ts>.log` | ⏳ GMM writes one log - `app/core.cpp:63` | ⏳ ✔ [win] |
| USVFS log viewer | ✅ `logDock` over the shared `MOBase::log` | ⏳ USVFS half only; our own log view ships - Workspace-myzy | ⏳ ✔ [win] |
| EventLog service check | ✅ `eventLogNotRunning` `spawn.cpp:335` | 🚫 Windows service, exists for USVFS | 🚫 ✔ [win] |
| Sanity checks on startup | ✅ `sanity::checkEnvironment` `sanitychecks.cpp:402` | 🚫 5 of 6 need modules, ACLs, GUIDs or ADS | 🚫 ✔ [win] |
| Sanity check: blocked files (Zone.Identifier ADS) | ✅ `sanity::checkBlocked()` `sanitychecks.cpp:159` | 🚫 NTFS ADS has no Linux equivalent | 🚫 ✔ [win] |
| Sanity check: missing files (AV deleted) | ✅ `sanity::checkMissingFiles()` `sanitychecks.cpp:178` | 🚫 6 of 8 are Windows helpers; loot half has a GMM equivalent | 🚫 ✔ |
| Sanity check: incompatible OSD/DLL modules | ✅ `sanity::checkBadOSDs()` `sanitychecks.cpp:205` | 🚫 iterates a Win32 PEB walk | 🚫 ✔ [win] |
| Sanity check: USVFS-incompatible DLLs | ✅ `sanity::checkUsvfsIncompatibilites()` `sanitychecks.cpp:259` | ⏳ meaningless without USVFS - Workspace-3br4 | ⏳ ✔ [win] |
| Sanity check: protected/system directory paths | ✅ `sanity::checkProtected()` `sanitychecks.cpp:339` | 🚫 `FOLDERID_*` known folders | 🚫 ✔ [win] |
| Sanity check: Microsoft Store game detection | ✅ `sanity::checkMicrosoftStore()` `sanitychecks.cpp:362` | 🚫 Windows Store | 🚫 ✔ [win] |
| Spawn error: makeContent (contextual) | ✅ `spawn::dialogs::makeContent()` `spawn.cpp:136` | ✅ `configure_executable_unreachable_dialog` - `launch_controller.cpp:114` | ✅ ✔ |
| Spawn error: spawnFailed dialog | ✅ `spawn::dialogs::spawnFailed()` `spawn.cpp:209` | ✅ `configure_launch_failed_dialog` - `launch_controller.cpp:124` | ✅ ✔ |
| Spawn error: helperFailed dialog | ✅ `spawn::dialogs::helperFailed()` `spawn.cpp:226` | 🚫 no helper binary to fail on Linux | 🚫 ✔ |
| Spawn error: confirmRestartAsAdmin | ✅ `spawn::dialogs::confirmRestartAsAdmin()` `spawn.cpp:250` | 🚫 Win32 elevation restart | 🚫 ✔ [win] |
| Spawn error: makeRightsDetails | ✅ `spawn::dialogs::makeRightsDetails()` `spawn.cpp:48` | 🚫 Win32 ACL; Posix perms are a different model | 🚫 ✔ [win] |
| Windows error formatting | ✅ `MOShared::windows_error` exception | 🚫 | 🚫 ✔ [win] |
| Windows compatibility mode detection | ✅ `WindowsInfo::compatibilityMode()` `envwindows.cpp` | 🚫 | 🚫 ✔ [win] |
| Windows version info collection | ✅ `WindowsInfo` (BuildLab, UBR) | 🚫 | 🚫 ✔ [win] |
| Process elevation detection | ✅ `WindowsInfo::isElevated()` | ⚠️ declared and overridden, no caller - `platform.h:132` | ⚠️ ✔ |
| Module detection and version info | ✅ `env::Module` `envmodule.h` | 🚫 | 🚫 ✔ [win] |
| Process enumeration and tree | ✅ `env::Process` `envmodule.h` | 🚫 | 🚫 ✔ [win] |
| DLL load notification (LdrRegisterDllNotification) | ✅ `Environment::onModuleLoaded()` | 🚫 | 🚫 ✔ [win] |
| Security product enumeration (WMI) | ✅ `env::getSecurityProducts()` `envsecurity.h` | 🚫 | 🚫 ✔ [win] |
| File security/permissions check | ✅ `env::getFileSecurity()` + `FileRights` | 🚫 | 🚫 ✔ [win] |
| Display metrics collection | ✅ `env::Metrics` + `env::Display` `envmetrics.h` | 🚫 | 🚫 ✔ [win] |
| NT API filesystem walker | ✅ `env::DirectoryWalker` | 🚫 | 🚫 ✔ [win] |
| Windows service status query | ✅ `env::getService()` `env.h:223` | 🚫 | 🚫 ✔ [win] |
| Registry cleanup | ✅ `env::deleteRegistryKeyIfEmpty()` `env.h:285` | 🚫 | 🚫 ✔ [win] |
| Full environment dump | ✅ `Environment::dump()` `envdump.h` | 🚫 | 🚫 ✔ [win] |
| Environment timezone collection | ✅ `Environment::timezone()` `env.h:190` | 🚫 a field of the environment dump above | 🚫 ✔ [win] |
| Core dump creation (self + other process) | ✅ `env::coredump()` / `coredumpOther()` `env.cpp:1242` | ⚠️ self only: POSIX handler writes a backtrace - `crash_handler.cpp:166` | ⚠️ ✔ |
| Log list (in-app viewer, 1000 entries) | ✅ `LogModel` + `LogList` `loglist.h`, `MaxLines = 1000` | ⚠️ `ConsolePanel` ships, now capped at 1000 - `console_panel.h` | ⚠️ ✔ |
| Log initialization and configuration | ✅ `initLogging()` (`main.cpp`) | ✅ reads setting, calls `Logger::set_level`; local time - `app/core.cpp:170` | ✅ ✔ |
| Log blacklisting (privacy - username masking) | ✅ `log::getDefault().addToBlacklist()` | ✅ `Logger::sanitize()` rewrites home dir to `{USER}` - `logger.cpp:139` | ✅ ✔ |
| Console attach/alloc (CLI) | ✅ `env::Console` (`AttachConsole`/`AllocConsole`) | 🚫 Win32 console API; `ConsolePanel` is a different thing | 🚫 ✔ [win] |
| CopyEventFilter (Ctrl+C in views) | ✅ `copyeventfilter.h` event filter | ✅ Qt copies rows natively; explicit Copy too - `console_panel.cpp:39` | ✅ ✔ |
| Problems dialog (plugin diagnostics) | ✅ `ProblemsDialog` `problemsdialog.h` | ⚠️ data path ships to a tooltip; no dialog or Fix - `plugin_loader.cpp:2147` | ⚠️ ✔ |
| Message dialog (fire-and-forget toast) | ✅ `MessageDialog` `messagedialog.h` | ⚠️ backend has zero producers and consumers - `notification_backend.h` | ⚠️ ✔ |
| Diagnostics settings tab | ✅ `DiagnosticsSettingsTab` `settingsdialogdiagnostics.h` | ✅ `build_diagnostics_tab()` - `settings_content_widget.cpp:1345` | ✅ ✔ |
| U033 Crash-on-exit dialog | ✅ `try`/`catch` around `delete ui` `mainwindow.cpp:644` | 🚫 guards a `delete ui` a hand-built tree does not have | 🚫 ✔ |
| U193 UILocker exit flow (canExit) | ✅ `Reasons::PreventExit` `organizercore.h:319` | ⏳ VFS half waits on USVFS proxies - Workspace-68y8 | ⏳ ✔ [win] |
| U238 IPluginDiagnose (activeProblems + Fix) | ✅ `mainwindow.cpp:1031-1054` | ⚠️ registry feeds a tooltip; no dialog or Fix - `plugin_loader.cpp:2147` | ⚠️ ✔ |
| U268 Report/reportError global error popup | ✅ `report.cpp` (uibase, not vendored) | ✅ `ui::report_error` / `critical_on_top` - `error_popup.h:41` | ✅ ✔ |
| U272 ErrorCodes shared error code mapping | ✅ `uibase`, not vendored | 🚫 GMM surfaces `std::error_code::message()` at point of failure | 🚫 ✔ |
| U273 DiagnosisReport report formatting | ✅ `uibase`, not vendored | ⚠️ returns `GmmDiagnosticProblem` structs, no report format - `plugin_loader.cpp:2147` | ⚠️ ✔ |

## 4. Settings & Configuration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Executables blacklist | ✅ `Settings::isExecutableBlacklisted` `settings.cpp:305` | ⚠️ line edit, zero readers - `settings_content_widget.cpp:1298` | ⚠️ ✔ |
| Skip file suffixes | ✅ `Settings::skipFileSuffixes` | ⚠️ line edit, no scanner reader - `settings_content_widget.cpp:1296` | ⚠️ ✔ |
| Skip directories | ✅ `Settings::skipDirectories` | ⚠️ line edit, zero readers - `settings_content_widget.cpp:1297` | ⚠️ ✔ |
| Force load libraries | ✅ `ExecutableForcedLoadSetting` | ⚠️ profile key copied, zero readers - `profile_creation.cpp:205` | ⚠️ ✔ |
| USVFS log level | ✅ `Settings::logLevel` | ✅ Logger + console panel - `app/core.cpp:170`, `console_panel.cpp:53` | ✅ ✔ |
| USVFS spawn delay | ✅ `Settings::spawnDelay` | 🚫 Windows-shaped, no Linux subject | 🚫 ✔ |
| Geometry persistence | ✅ `GeometrySettings` (window, splitter, toolbar) | ✅ 4 dialogs restore - `list_dialog.cpp:43`, `mod_info_dialog.cpp:333` | ✅ ✔ |
| Widget state persistence | ✅ `WidgetSettings` (tree expand, combo, tab index) | ✅ splitters, headers - `settings_controller.cpp:869` | ✅ ✔ |
| Color settings (conflict coloring) | ✅ `ColorSettings` (7 colors) | ⚠️ 5 of 7 read; 2 archive colors dead - `mod_list_model.cpp:2203` | ⚠️ ✔ |
| Plugin blacklist | ✅ `Settings::blacklisted` | ✅ `disabled_plugins` reaches the loader - `app/core.cpp:248` | ✅ ✔ |
| Network settings (proxy, offline mode) | ✅ `NetworkSettings` | ⚠️ offline+proxy live; custom browser dead - `network_options_bridge.cpp:31` | ⚠️ ✔ |
| Splash screen | ✅ `Settings::useSplash` | ⏳ cosmetic, no component | ⏳ ✔ |
| Prerelease updates toggle | ✅ `Settings::usePrereleases` | ⚠️ checkbox; whole updater has no callers | ⚠️ ✔ |
| Low-priority extraction | ✅ | 🚀 `pipeline_worker.cpp:199` `extraction_low_priority()` | 🚀 ✔ |
| Full UI mode (tabs vs popups) | ❌ | 🚀 `settings_content_widget.cpp:120` `full_ui_mode()` | 🚀 ✔ |
| Multi-core processing toggle | ❌ | 🚀 `settings.h:130` + `parallel::set_enabled` | 🚀 ✔ |
| Language selection (i18n picker) | ✅ `InterfaceSettings::language()` | ⚠️ picker + loader live, help link absent - `app/core.cpp:137` | ⚠️ ✔ |
| Style/Theme selection (QStyle + .qss) | ✅ `InterfaceSettings::styleName()` | ✅ theme, style, icon pack - `settings_controller.cpp:1203` | ✅ ✔ |
| Collapsible separators settings | ✅ `InterfaceSettings` (ascending, descending, highlight, icons) | ⚠️ 9 keys, zero readers; fold state itself real - `settings.h:46` | ⚠️ ✔ |
| Save filters toggle | ✅ `InterfaceSettings::saveFilters()` | ⚠️ dead; no mod list filter exists - `settings_content_widget.cpp:481` | ⚠️ ✔ |
| Auto-collapse on hover | ✅ `InterfaceSettings::autoCollapseOnHover()` | ⚠️ zero readers - `settings_content_widget.cpp:483` | ⚠️ ✔ |
| Display foreign mods | ✅ `InterfaceSettings::displayForeign()` | ⚠️ zero readers; unmanaged mods always shown - `settings_content_widget.cpp:479` | ⚠️ ✔ |
| Meta downloads display | ✅ `InterfaceSettings::metaDownloads()` | 🚫 no meta-download concept | 🚫 ✔ |
| Hide downloads after installation | ✅ `InterfaceSettings::hideDownloadsAfterInstallation()` | ⚠️ GMM filters rows, never auto-removes - `downloads_tab.cpp:905` | ⚠️ ✔ |
| Show download notifications | ✅ `InterfaceSettings::showDownloadNotifications()` | ✅ gates the status-bar notice - `downloads_controller.cpp:116` | ✅ ✔ |
| Hide API counter | ✅ `InterfaceSettings::hideAPICounter()` | ⚠️ hides a counter that does not exist - `source_pages.cpp:277` | ⚠️ ✔ |
| Lock GUI during executables | ✅ `InterfaceSettings::lockGUI()` | ⚠️ Locker ships for downloads, no setting or launch trigger - `ui_locker.cpp:14` | ⚠️ ✔ |
| Center dialogs on parent | ✅ `GeometrySettings::centerDialogs()` | ⚠️ zero readers - `settings_content_widget.cpp:211` | ⚠️ ✔ |
| Show change game confirmation | ✅ `InterfaceSettings::showChangeGameConfirmation()` | ❌ key only: no UI row, no reader | ❌ ✔ |
| Close to system tray instead of quitting | ❌ per-executable flag only, no global toggle | 🚀 checkbox, default off, ignored without a tray - `settings.cpp:221` | 🚀 ✔ |
| Show menubar on Alt | ✅ `InterfaceSettings::showMenubarOnAlt()` | ⏳ MO2 gates Alt on `UILocker`, GMM has no Alt reveal | ⏳ ✔ |
| Double-clicks open previews | ✅ `InterfaceSettings::doubleClicksOpenPreviews()` | ✅ three file trees - `filetree_tab.cpp:111` | ✅ ✔ |
| Tutorial completion tracking | ✅ `InterfaceSettings::isTutorialCompleted()` | 🚫 no tutorial content ships, submenu hidden - `menu_bar.cpp:379` | 🚫 ✔ |
| Filter widget options | ✅ `InterfaceSettings::filterOptions()` | 🚫 no mod list filter exists | 🚫 ✔ |
| Archive parsing toggle | ✅ `Settings::archiveParsing()` | ⚠️ zero readers - `settings_content_widget.cpp:1309` | ⚠️ ✔ |
| Keep backup on install | ✅ `Settings::keepBackupOnInstall()` | ✅ seeds the Mod Exists dialog - `query_overwrite_dialog.cpp:108` | ✅ ✔ |
| Profile default settings (local INIs, saves, archive invalidation) | ✅ `Settings::profileLocalInis()` etc. | ⚠️ 2 of 3; invalidation default hardcoded false - `profile_creation.cpp:143` | ⚠️ ✔ |
| Refresh thread count | ✅ `Settings::refreshThreadCount()` | ⏳ no setting, fixed thread pool | ⏳ ✔ |
| Force enable core files | ✅ `GameSettings::forceEnableCoreFiles()` | ⚠️ zero readers - `settings_content_widget.cpp:1307` | ⚠️ ✔ |
| Base directory variable (%BASE_DIR%) | ✅ `PathSettings::BaseDirVariable` | ✅ `$BASE_DIRECTORY`/`%BASE_DIR%` expanded in `expand_instance_path` - `instance.cpp:26` | ✅ ✔ |
| Recent directories | ✅ `PathSettings::recent()` | ⏳ no setting | ⏳ ✔ |
| Offline mode | ✅ `NetworkSettings::offlineMode()` | ✅ reaches NetworkOptions - `network_options_bridge.cpp:31` | ✅ ✔ |
| Custom browser command | ✅ `NetworkSettings::customBrowserCommand()` | ⚠️ dead; every link uses QDesktopServices - `settings_content_widget.cpp:1260` | ⚠️ ✔ |
| Download speed tracking per server | ✅ `NetworkSettings::setDownloadSpeed()` | 🚀 rolling average per mirror - `source/nexus/provider.cpp:441` | 🚀 ✔ |
| Server preference list | ✅ `NetworkSettings::servers()` | 🚀 rank orders mirror selection - `source/nexus/provider.cpp:245` | 🚀 ✔ |
| Nexus endorsement integration setting | ✅ `NexusSettings::endorsementIntegration()` | ✅ `settings.h:161` + applied `nexus_source_panel.cpp:41` | ✅ ✔ |
| Nexus tracked integration setting | ✅ `NexusSettings::trackedIntegration()` | ✅ `settings.h:163` + applied `nexus_source_panel.cpp:46` | ✅ ✔ |
| Nexus category mappings setting | ✅ `NexusSettings::categoryMappings()` | ⚠️ disabled "work in progress", never wired - `source_pages.cpp:275` | ⚠️ ✔ |
| NXM handler registration (settings) | ✅ `NexusSettings::registerAsNXMHandler()` | 🚫 Linux registers via xdg-mime - `source_pages.cpp:429` | 🚫 ✔ |
| MODL handler registration (settings) | ✅ `Settings::registerAsMODLHandler()` | 🚫 Windows-only file association | 🚫 ✔ |
| Download handler registration | ✅ `Settings::registerDownloadHandlers()` | 🚫 Windows-only shell integration | 🚫 ✔ |
| Steam app ID override | ✅ `SteamSettings::appID()` | 🚫 GMM has no Steam client | 🚫 ✔ |
| Steam login (credential store) | ✅ `SteamSettings::login()` (Windows Credential Store) | 🚫 Windows credential store only | 🚫 ✔ |
| First start detection | ✅ `Settings::firstStart()` | ⏳ no stored app version | ⏳ ✔ |
| MO version tracking in settings | ✅ `Settings::version()` | ⏳ `VERSION` is a compile define, never persisted | ⏳ ✔ |
| Settings migration (auto-upgrade between versions) | ✅ `Settings::processUpdates()` | ⏳ `processUpdates()` has 5 MO2 callers, 0 GMM | ⏳ ✔ |
| BSA date backdating | ✅ `settingsdialogworkarounds` (on_bsaDateBtn_clicked) | ⏳ no button, no equivalent | ⏳ ✔ |
| Reset geometry settings | ✅ `settingsdialogworkarounds` (on_resetGeometryBtn_clicked) | ✅ resets every stored dialog geometry - `settings_content_widget.cpp:226` | ✅ ✔ |
| Reset dialog choices | ✅ `settingsdialoggeneral` (onResetDialogs) | ✅ General-tab Reset button, `settings_content_widget.cpp:232` → `settings.cpp:373` | ✅ ✔ |
| Color separator scrollbar | ✅ `ColorSettings::colorSeparatorScrollbar()` | ✅ `settings.h:204` + gate `mod_list_model.cpp:134` | ✅ ✔ |
| Toolbar state persistence | ✅ `GeometrySettings::saveToolbars()` / `restoreToolbars()` | ✅ `settings_controller.cpp:869` / `:966` | ✅ ✔ |
| Dock state persistence | ✅ `GeometrySettings::saveDocks()` / `restoreDocks()` | 🚫 splitter layout, no docks | 🚫 ✔ |
| Widget visibility persistence | ✅ `GeometrySettings::saveVisibility()` / `restoreVisibility()` | ⏳ tabbed UI replaces the cheatsheet | ⏳ ✔ |
| Remember question dialog buttons | ✅ `WidgetSettings::QuestionBoxMemory` | ✅ `dialog_choice` short-circuits the dialog - `task_dialog.cpp:201` | ✅ ✔ |
| Tree expand/check state persistence | ✅ `WidgetSettings::saveTreeCheckState` / `saveTreeExpandState` | ✅ separator fold persisted per instance - `mod_list_controller.cpp:3995` | ✅ ✔ |
| Tab widget index persistence | ✅ `WidgetSettings::saveIndex(QTabWidget)` | ⏳ settings tab resets to General | ⏳ ✔ |
| Combobox index persistence | ✅ `WidgetSettings::saveIndex(QComboBox)` | 🚫 log level is already a setting | 🚫 ✔ |
| Checkable button state persistence | ✅ `WidgetSettings::saveChecked(QAbstractButton)` | ⏳ every meaningful checkbox is already a setting | ⏳ ✔ |
| Tab-based settings dialog (8 tabs) | ✅ `settingsdialog.cpp` (General, Theme, ModList, Paths, Diagnostics, Nexus, Plugins, Workarounds) | ✅ same 8, Nexus named Sources - `settings_content_widget.cpp:61` | ✅ ✔ |
| Settings change logging | ✅ `settingsutilities.h` `logChange()` | ⏳ no change log; ConsolePanel has no settings feed | ⏳ ✔ |
| Color table (visual color picker with delegates) | ✅ `colortable.h/cpp` | ⚠️ 7 swatch rows, no table or delegate previews - `settings_content_widget.cpp:392` | ⚠️ ✔ |
| Center on main window monitor | ✅ `GeometrySettings::centerOnMainWindowMonitor()` | ⏳ blocked behind the dead `center_dialogs` | ⏳ ✔ |
| U006 Ctrl+S = Settings shortcut | ✅ `mainwindow.ui:1736-1756` | ✅ Preferences + explicit Ctrl+S - `menu_bar.cpp:75` | ✅ ✔ |
| U040 Alt key reveals hidden menubar (showMenubarOnAlt, suppressed while UILocker locked) | ✅ `mainwindow.cpp:4052-4070` | ⏳ no menubar visibility state to reveal | ⏳ ✔ |
| U044 Restart-after-settings dialog (Restart / Continue variants) | ✅ `mainwindow.cpp:2768-2780` | ✅ Restart / Continue dialog - `settings_controller.cpp:110` | ✅ ✔ |
| U046 Network proxy activation progress dialog | ✅ `mainwindow.cpp:2115-2140` | ⏳ bridge applies options instantly, no wait | ⏳ ✔ |
| U047 Downgrade notice after version drop | ✅ `mainwindow.cpp:2219` | ⏳ no last-version value to compare | ⏳ ✔ |
| U061 dataTabShowFromArchives gated on archiveParsing setting | ✅ `mainwindow.cpp:532-542` | ⏳ data tab has no archives sub-filter | ⏳ ✔ |
| U129 General > Language group (languageBox + "Help translate" LinkLabel) | ✅ `settingsdialog.ui:68-124` | ⚠️ picker works, "Help translate" link absent - `settings_content_widget.cpp:78` | ⚠️ ✔ |
| U130 General > Download List group (4 checkboxes + MODL associate button) | ✅ `settingsdialog.ui:125-197` | ⚠️ compact only; hide-installed is on the downloads tab | ⚠️ ✔ |
| U133 General > Miscellaneous checkboxes (center dialogs, instance-change confirm, Alt menubar, previews on double-click) | ✅ `settingsdialog.ui:264-325` | ⚠️ 1 of 4: previews on double-click - `settings_content_widget.cpp:137` | ⚠️ ✔ |
| U134 General buttons (Reset Dialog Choices, Configure Mod Categories) | ✅ `settingsdialog.ui:343-372` | ✅ both: reset at `:232`, categories dialog `categories_dialog.h:21` | ✅ ✔ |
| U138 Paths tab (7 path rows + %BASE_DIR% hint + writability footer) | ✅ `settingsdialog.ui:846-1054` | ⚠️ 7 rows + hint + working variable expansion, no footer | ⚠️ ✔ |
| U139 Paths error strings (create failed, invalid game install) | ✅ `settingsdialogpaths.cpp:100-101`, `:236-237` | ⏳ commits silently, no mkdir, no warning | ⏳ ✔ |
| U140 Nexus settings tab full page (account, statistics, connection, options, servers groups) (NEXUS-LENS: genericize/provider-scope) | ✅ `settingsdialog.ui:1056-1504` | ⚠️ Sources tab stands in - `settings_content_widget.cpp:65` | ⚠️ ✔ |
| U142 Nexus custom browser picker file dialog | ✅ `settingsdialognexus.cpp:500-510` | ⏳ downstream of the dead custom-browser feature | ⏳ ✔ |
| U143 Settings > Plugins tab (plugin details, Enabled 3-state tooltip, settings table, blacklist) | ✅ `settingsdialog.ui:1506-1746` | ⚠️ details, table, blacklist real; 3-state tooltip absent - `settings_content_widget.cpp:894` | ⚠️ ✔ |
| U145 Workarounds options (force-enable game files, archives parsing, lock GUI) | ✅ `settingsdialog.ui:1810-1860` | ⚠️ 0 of 3: both checkboxes dead, lock GUI absent - `settings_content_widget.cpp:1307` | ⚠️ ✔ |
| U146 Workarounds > Steam group (AppID/username/password + log blacklist) | ✅ `settingsdialog.ui:1864-1928`, `settingsdialogworkarounds.cpp:17-30` | 🚫 Windows credential store, no Linux subject | 🚫 ✔ |
| U147 Workarounds > Network group (offline mode, system proxy, custom browser) | ✅ `settingsdialog.ui:1931-2010` | ⚠️ 2 of 3; custom browser dead - `settings_content_widget.cpp:1259` | ⚠️ ✔ |
| U148 Workarounds buttons (Reset Geometries, Back-date BSAs, Executables Blacklist, Skip Suffixes/Directories) | ✅ `settingsdialog.ui:2035-2144`, `settingsdialogworkarounds.cpp:96-200` | ⚠️ 1 of 5: Reset Geometries; 3 dead line edits, no back-date | ⚠️ ✔ |
| U149 Workarounds footer warning text | ✅ `settingsdialog.ui:2187` | ⏳ one static label | ⏳ ✔ |
| U150 Diagnostics tab controls (log level, crash dumps, max dumps, LOOT log level, links) | ✅ `settingsdialog.ui:2197-2320`, `settingsdialogdiagnostics.cpp:22-90` | ⚠️ log level + max dumps real; LOOT level, links absent - `settings_content_widget.cpp:1324` | ⚠️ ✔ |
| U151 Settings tabs use scroll areas with grouped GroupBoxes | ✅ `settingsdialog.ui:28`, `:1062` | ⏳ plain pages + GroupBoxes, no scroll areas | ⏳ ✔ |
| U223 Per-plugin translators (every loaded plugin file basename) | ✅ `mainwindow.cpp:2930-2931` | ⏳ one translator from `language()`, none per plugin - `app/core.cpp:136` | ⏳ ✔ |
| U262 QuestionBoxMemory per-dialog choice persistence (.ui + IDs) | ✅ `questionboxmemory` (uibase) | ✅ `dialog_choices/` keys - `settings.cpp:360` | ✅ ✔ |
| U263 FileDialogMemory::restore (remembers dir per named dialog) | ✅ `mainwindow.cpp:478` | ⏳ every QFileDialog opens in the field's own path | ⏳ ✔ |
| U287 splash.png + useSplash display component | ✅ `src/splash.png` | ⏳ cosmetic, no component | ⏳ ✔ |

## 5. Executable Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Custom executables list | ✅ `ExecutablesList` (CRUD) | ✅ `Entry` + `ExecControlsBar` - `executables_entry.h:26` | ✅ · |
| Per-executable arguments | ✅ `Executable::arguments` | ✅ `Entry::arguments` - `executables_entry.h:30` | ✅ · |
| Per-executable working directory | ✅ `Executable::workingDirectory` | ✅ `start_in_edit_` - `executables_content_widget.cpp:317` | ✅ · |
| Per-executable Steam App ID | ✅ `Executable::steamAppID` | ❌ no field on `Entry`; the MO2 importer that populated it is deleted - `executables_entry.h:24` | ❌ · |
| Per-executable custom overwrite | ✅ `Executable::customOverwrites` | ❌ | ❌ · |
| Per-executable forced libraries | ✅ `Executable::forcedLibraries` | ❌ | ❌ · |
| Per-executable environment variables | ❌ | 🚀 `parse_environment_text` (KEY=VALUE) - `executables_content_widget.cpp:45` | 🚀 · |
| Per-executable output-to-mod routing | ❌ | 🚀 `output_mod` load + combo - `executables_content_widget.cpp:119` | 🚀 · |
| Toolbar pinning | ✅ `ShowInToolbar` flag | ✅ `add_shortcut_to_toolbar` - `main_window.cpp:229` | ✅ · |
| Desktop shortcut creation | ❌ | 🚀 `add_shortcut_to_desktop` (.desktop file) - `main_window.cpp:232` | 🚀 · |
| Executable ordering (up/down) | ✅ `EditExecutablesDialog` | ✅ up/down + InternalMove drag - `executables_content_widget.cpp:268` | ✅ · |
| Clone executable | ✅ `EditExecutablesDialog::clone()` | ✅ `on_clone_selected` - `executables_content_widget.cpp:276` | ✅ · |
| JAR binary detection | ✅ `setJarBinary` + `findJavaInstallation` | ❌ | ❌ · |
| Icon extraction (wrestool/QFileIconProvider) | ❌ | 🚀 `extractExeIcon` - `exec_controls_bar.cpp:111` | 🚀 · |
| Executable editor widget | ✅ `EditExecutablesDialog` | ✅ `ContentWidget` (mode-agnostic) - `executables_entry.h:84` | ✅ · |
| Executables list proxy model | ✅ `ExecutablesListProxy` | ❌ | ❌ · |
| U004 Ctrl+E = Executables... | ✅ `mainwindow.ui:1697-1717` | ❌ | ❌ · |
| U016 Run menu (one action per pinned exe, statusTip, objectName) | ✅ `mainwindow.cpp:755-795` | ⚠️ exec controls bar only, Run menu unproven - `exec_controls_bar.cpp:224` | ⚠️ · |
| U022 Toolbar context menu "Remove '%1' from the toolbar" | ✅ `mainwindow.cpp:3785-3805` | ❌ | ❌ · |
| U024 Link button menu (Toolbar and Menu / Desktop / Start Menu shortcuts) | ✅ `mainwindow.cpp:360-367`, `:2695-2719` | ⚠️ desktop only - `launch_controller.cpp:1340` | ⚠️ · |
| U051 Executables combo sentinels ("<Edit...>", "(no executables)") | ✅ `mainwindow.cpp:1866-1920` | ✅ kAddNewEntryText = "<Edit...>" - `exec_controls_bar.h:22` | ✅ · |
| U069 Pinned exe toolbar actions (icon, statusTip, objectName) | ✅ `mainwindow.cpp:769-795` | ⚠️ action wiring only, trio unproven - `main_window.cpp:229` | ⚠️ · |
| U155 File tree "Enter Name" add-as-executable dialog | ✅ `filetree.cpp:280-301` | ❌ | ❌ · |
| U156 EditExecutables list context menu (Add from file / Add empty / Clone) | ✅ `editexecutablesdialog.cpp:91-99` | ⚠️ Clone only - `executables_content_widget.cpp:276` | ⚠️ · |
| U188 EditExecutablesDialog full control set (list, buttons, fields, AppID override) | ✅ `editexecutablesdialog.ui` | ⚠️ widget exists, exact control set unproven - `executables_content_widget.cpp:276` | ⚠️ · |
| U189 EditExecutables validation dialogs (empty output mod, reset confirm, Java required) | ✅ `editexecutablesdialog.cpp:214-887` | ❌ | ❌ · |
| U277 ExecutableInfo PE parsing of binaries | ✅ `executableinfo.cpp` (uibase) | ❌ | ❌ · |

## 6. Mod Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod priority ordering | ✅ `profile.getActiveMods` | ✅ `ModMeta::set_priority` - `mod_meta.cpp:484` | ✅ · |
| Mod enable/disable | ✅ `ModInfo::enabled` | ✅ `enable_mod` / `disable_mod` - `mod_scanner.cpp:786` | ✅ · |
| Overwrite directory | ✅ `Settings::paths().overwrite` | ✅ `overwrite_dir` + create - `instance.h:61` | ✅ · |
| Custom overwrite target | ✅ `customOverwrite` param | ✅ `relay_output_to_mod` - `fs_utils.h:230` | ✅ · |
| Mod data-to-game mapping | ✅ `getModMappings` | ✅ `knowledge_->get` - `debug_window.cpp:881` | ✅ · |
| Mod metadata | ✅ `ModInfo` | ✅ `ModMeta` - `mod_meta.h` | ✅ · |
| Core category sets | ❌ | 🚀 `CategorySetRegistry` + plugin hook - `category_set_registry.h:16` | 🚀 · |
| Plugin-contributed categories | ❌ | 🚀 `cb_register_categories` -> `Category::Factory::merge` - `plugin_loader.cpp:191` | 🚀 · |
| Mod file tree / conflict display | ✅ `DirectoryEntry` | ✅ `file_tree.h:3` + `conflict_engine.cpp:341` | ✅ · |
| BSA/archive extraction | ✅ `BSAExtractor` | ✅ `archive_extractor.cpp:79` | ✅ · |
| Case-insensitive mod matching | ✅ USVFS handles it | 🚀 `PathResolver` - `path_resolver.h:34` | 🚀 · [win] |
| Mod cache | ✅ | ✅ `ModCache` (SQLite) - `mod_cache.h:20` | ✅ · |
| Mod scanner | ✅ | ✅ `mod_scanner.cpp:434` | ✅ · |
| Mod renaming | ✅ `ModList::renameMod` | ✅ `rename_mod_inline` - `mod_actions.cpp:573` | ✅ · |
| Mod notes | ✅ `ModInfo::notes()` | ✅ `NotesTab` (comments, HTML, color) - `notes_tab.cpp:24` | ✅ · |
| Mod comments | ✅ `ModInfo::comments()` | ✅ `comments_` + `on_comments_edited` - `notes_tab.cpp:28` | ✅ · |
| Mod color coding | ✅ `ModInfo::color()` | ✅ Set/Reset color; sidecar key merged on scan - `mod_scan_worker.cpp:364` | ✅ · |
| Mod author/uploader metadata | ✅ `ModInfo::author()`, `uploader()` | ✅ from ESP CNAM - `esp_header.cpp:74` | ✅ · |
| Mod description | ✅ `ModInfo::getDescription()` | ✅ + BBCode pipeline - `plugin_database.cpp:339` | ✅ · |
| Mod creation time | ✅ `ModInfo::creationTime()` | ✅ `install_time`/`changed_time` - `mod_scanner.cpp:434` | ✅ · |
| Mod internal name | ✅ `ModInfo::internalName()` | ❌ | ❌ · |
| Mod validated flag | ✅ `ModInfo::markValidated` | ✅ `mark_validated` - `mod_scanner.cpp:853` | ✅ · |
| Mod repository tracking | ✅ `ModInfo::repository()` | ✅ `meta.source_type()` / `source_id` - `mod_list_controller.cpp:1927` | ✅ · |
| Plugin settings per mod | ✅ `ModInfoRegular::pluginSetting` | ✅ `plugin_setting()` + registry - `plugin_settings_registry.h:43` | ✅ · |
| Nexus file IDs tracking | ✅ `ModInfoRegular::installedFiles` | ✅ `download_nxm.file_id`, no class - `gmmpack/packer.cpp:194` | ✅ · |
| Mod tags (deprecated/note/warning/incompatible) | ❌ | 🚀 `ModTag` + `set_tags` - `mod_list_model.h:22` | 🚀 · |
| Visual nesting (parent_id, indent, fold) | ❌ | 🚀 indent + `set_folded` - `mod_list_model.cpp:156` | 🚀 · |
| Vendor icons (per-source badges) | ❌ | 🚀 nexusmods, loverslab, steam, moddb - `mod_list_model.cpp:46` | 🚀 · |
| MERGED pseudo-mod | ❌ | 🚀 `kMergedModId` - `mod_list_model.h:17` | 🚀 · |
| Game-native mod band | ❌ | 🚀 `native_band_first/last` - `mod_list_model.h:291` | 🚀 · |
| ModInfoForeign (non-official plugins) | ✅ `ModInfoForeign` | ❌ | ❌ · |
| ModInfoSeparator | ✅ `ModInfoSeparator` | ❌ | ❌ · |
| ModInfoWithConflictInfo (conflict data) | ✅ `ModInfoWithConflictInfo` | ❌ | ❌ · |
| Hidden file extension detection | ✅ `ModInfo::s_HiddenExt` | ❌ | ❌ · |
| getByName/getByIndex/getByModID accessors | ✅ `ModInfo::getByName()` etc. | ❌ | ❌ · |
| Import strategy (Merge/Overwrite/None) | ✅ `ImportStrategy` enum | ❌ | ❌ · |
| Activate mods dialog (save-game asset resolution) | ✅ `ActivateModsDialog` | ❌ | ❌ · |
| U055 5s periodic timer saving mod metas (saveModMetas) | ✅ `mainwindow.cpp:500-504` | 🚀 `DelayedFileWriter` 5s debounce + flush-on-destruct, in-folder metas written sync - `delayed_file_writer.h:33` | 🚀 · |
| U116 Rename toasts ("Invalid name", "Name is already in use") | ✅ `modlist.cpp:490-496` | ✅ 4 warnings wired through apply_rename - `mod_actions.cpp:627` | ✅ · |
| U117 Mod remove confirmation dialog | ✅ `modlist.cpp:1233-1234` | ✅ 3/3 destructive paths confirm, trashes + restore note - `mod_actions.cpp:115` | ✅ · |
| U201 meta.ini full key set (~29 keys incl. Endorsed/Abstained, tracked, nexus*) | ✅ `modinfo.cpp:80-330` | ⚠️ core keys only, 29-key set unproven - `mod_meta.cpp:117` | ⚠️ · |
| U202 meta.ini version / newestVersion / ignoredVersion keys | ✅ `modinforegular.cpp` | ⚠️ newestVersion only, ignoredVersion unproven - `nexus_source_panel.cpp:255` | ⚠️ · |
| U203 meta.ini modId / fileId / repository / gameName keys | ✅ `modinforegular.cpp` | ⚠️ modid + repository only - `mod_meta.cpp:736` | ⚠️ · |
| U204 meta.ini category (primary) / categories CSV | ✅ `modinfo.cpp` | ✅ category=0 write + CSV parse - `mod_meta.cpp:739` | ✅ · |
| U205 meta.ini comments / notes / color (hex) | ✅ `modinfo.cpp` | ⚠️ store exists, accessors unproven - `mod_meta.cpp:292` | ⚠️ · |
| U281 DirectoryRefresher progress + error signal | ✅ `directoryrefresher.cpp`, `mainwindow.cpp:2441` | ⚠️ progress signal only, error signal unproven - `directory_refresher.h:95` | ⚠️ · |

## 7. Mod Categories

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Category tree system | ✅ `Categories` (hierarchical) | ✅ `Category::Factory` + `load` - `category_factory.h:15` | ✅ · |
| Nexus category mapping | ✅ `NexusCategory` + `resolveNexusID` | ✅ `nexuscategory` key applied - `mod_list_controller.cpp:1996` | ✅ · |
| Category import/export | ✅ `CategoryImportDialog` | ❌ | ❌ · |
| Multi-category assignment | ✅ `ModInfo::setCategories` | ✅ `category_ids` CSV + `set_category_ids` - `mod_list_model.h:65` | ✅ · |
| Primary category | ✅ `ModInfo::primaryCategory` | ✅ first entry in category CSV - `mod_list_controller.cpp:2055` | ✅ · |
| Special filter categories | ✅ `SpecialCategories` (Checked, UpdateAvailable, Conflict) | ⚠️ Active + Conflict + Has hidden files, `category_filter_panel.h:24` - UpdateAvailable has no mod-update data to filter on | ⚠️ · |
| Category CRUD editor | ✅ | ✅ `CategoriesDialog` (full CRUD) - `categories_dialog.h:24` | ✅ · |
| Category filter panel | ✅ | ✅ `CategoryFilterPanel` (checkable tree) - `category_filter_panel.h:21` | ✅ · |
| Categories table view | ✅ `CategoriesTable` | ❌ | ❌ · |
| U029 Category setup: GMM ruling = opt-in per-instance Nexus mapping import, no MO2 first-run chooser (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1281-1300` | ⚠️ opt-in import exists, not wired - `source_pages.cpp:345` | ⚠️ · |
| U030 Category migration dialog (Import / Open / Disable Mappings / Don't show again) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1312-1345` | ❌ | ❌ · |
| U076 Category menus as QPushButton-with-menu (addMenuAsPushButton) | ✅ `modlistcontextmenu.cpp:264-271` | ❌ | ❌ · |
| U078 "Change Categories" menu (recursive checkable items, parent check icon, aboutToHide apply) | ✅ `modlistcontextmenu.cpp:110-156` | ⚠️ menus exist, apply-on-hide unproven - `mod_context_menu.cpp:459` | ⚠️ · |
| U079 "Primary Category" menu (QRadioButton per assigned category) | ✅ `modlistcontextmenu.cpp:180-215` | ⚠️ radio menu exists, apply-on-hide unproven - `mod_context_menu.cpp:541` | ⚠️ · |
| U112 Category column "Non-MO" for foreign + auto-unset on removal | ✅ `modlist.cpp:222-244` | ❌ | ❌ · |
| U126 Separator display strips "_separator" suffix | ✅ `modlist.cpp:110-122` | ❌ | ❌ · |
| U171 Categories dialog (Refresh from Nexus, import column, drag-assign pane) (NEXUS-LENS: genericize/provider-scope) | ✅ `categoriesdialog.ui` | ⚠️ CRUD exists, Nexus pane unproven - `categories_dialog.h:21` | ⚠️ · |
| U172 Category import dialog (Merge/Replace strategy + mapping options) | ✅ `categoryimportdialog.ui` | ❌ | ❌ · |
| U196 "No category found" install dialog (Proceed / Disable / Stop && Configure) (NEXUS-LENS: genericize/provider-scope) | ✅ `installationmanager.cpp:672-682` | ❌ | ❌ · |
| U229 Category menu commits on aboutToHide | ✅ `modlistcontextmenu.cpp:145-151` | ❌ | ❌ · |

## 8. Mod Conflict Detection

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Loose file conflicts | ✅ `EConflictFlag::OVERWRITE` | ✅ `conflict_engine.cpp:341` | ✅ · |
| Archive vs loose conflicts | ✅ `FLAG_ARCHIVE_LOOSE_CONFLICT_*` | ✅ four conflict color settings - `settings.h:216` | ✅ · |
| Archive vs archive conflicts | ✅ `FLAG_ARCHIVE_CONFLICT_*` | ❌ | ❌ · |
| Overwrite folder conflicts | ✅ `FLAG_OVERWRITE_CONFLICT` | ❌ | ❌ · |
| Conflict dialog (general) | ✅ `GeneralConflictsTab` with counters | ❌ | ❌ · |
| Conflict dialog (advanced) | ✅ `AdvancedConflictsTab` tree view | ❌ | ❌ · |
| Conflict context menu | ✅ open/run hooked/preview/explore/hide/goto | ✅ `on_custom_context_menu` + ImageDiff - `conflicts_tab.cpp:182` | ✅ · |
| Conflict highlighting | ✅ `EHighlight` (INVALID, CENTER, IMPORTANT, PLUGIN) | ✅ row tint + scrollbar marks - `mod_list_model.cpp:137` | ✅ · |
| Per-mod conflict stats | ❌ | 🚀 `set_conflict_stats(wins, losses)` - `mod_list_controller.cpp:2256` | 🚀 · |
| Blake2b fingerprint cache | ❌ | 🚀 blake2b + `scan(db_path)` - `conflict_index.h:92` | 🚀 · |
| Image diff (conflict comparison) | ❌ | 🚀 `image_diff_requested` - `main_window.cpp:283` | 🚀 · |
| ConflictListModel / ConflictItem | ✅ `ConflictListModel`, `ConflictItem` | ❌ | ❌ · |
| Data tab conflict mode | ✅ `dataTabShowOnlyConflicts` | ❌ | ❌ · |
| U094 Conflict flag tooltip texts (9 exact strings + FLAG_OVERWRITE_CONFLICT) | ✅ `modlist.cpp:156-180` | ⚠️ column plumbing, exact strings unproven - `mod_list_model.cpp:345` | ⚠️ · |
| U119 EConflictFlag enum (10 values: loose/archive/archive-loose + overwrite) | ✅ `modinfo.h:70-81` | ⚠️ result plumbing only, enum unproven - `mod_list_controller.h:84` | ⚠️ · |
| U120 EHighlight bit values (NONE/INVALID/CENTER/IMPORTANT/PLUGIN) | ✅ `modinfo.h:99-105` | ❌ | ❌ · |

## 9. Mod Content Analysis

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod data content types | ✅ `ModDataContentHolder` | ✅ `ModContentId` enum - `game_feature.h:154` | ✅ · |
| Mod flags (INVALID, BACKUP, SEPARATOR, etc.) | ✅ `EFlag` | ⚠️ pipeline state, not a flag enum - `mod_scanner.cpp:604` | ⚠️ · |
| Mod content icons | ✅ `ModContentIconDelegate` | ❌ | ❌ · |
| Mod conflict icons | ✅ `ModConflictIconDelegate` | ✅ `FlagsDelegate` (per-icon tooltips) - `mod_table_view.h:89` | ✅ · |
| Mod flag icons | ✅ `ModFlagIconDelegate` | ✅ `FlagsDelegate` (wrap + tooltips) - `mod_table_view.h:49` | ✅ · |
| Empty mod flag (dummy icon, tooltip) | ✅ | ✅ `set_empty` + `plugin-dummy` icon - `mod_list_model.cpp:1327` | ✅ · |
| Mod version delegate (color-coded) | ✅ `ModListVersionDelegate` | ❌ | ❌ · |
| INI tweaks detection | ✅ `ModInfo::getIniTweaks()` | ⚠️ profile initweaks.ini + tooltip - `profile_switching.cpp:36` | ⚠️ · |
| Archive listing per mod | ✅ `ModInfo::archives()` | ✅ `archives_html()` in plugin tooltip - `plugin_view.cpp:248` | ✅ · |
| ModDataContent updated signal | ✅ `ModDataContentUpdated` | ❌ | ❌ · |
| CombinedModDataContent | ✅ `CombinedModDataContent` | ❌ | ❌ · |
| U042 BSA list context menu "Extract..." | ✅ `mainwindow.cpp:3720-3728` | ❌ | ❌ · |
| U065 BSA extract errors (read/extract failures, invalid hashes warning) | ✅ `mainwindow.cpp:3614-3715` | ❌ | ❌ · |
| U093 Flag tooltip texts (8 exact strings incl. different-game + tracked warnings) | ✅ `modlist.cpp:124-153` | ⚠️ plumbing only, exact strings unproven - `mod_table_view.h:93` | ⚠️ · |
| U104 Mod content column per-icon tooltip (helpEvent -> contentsTooltip table) | ✅ `modcontenticondelegate.cpp:40-56` | ❌ | ❌ · |
| U118 EFlag enum (11 flags incl. PLUGIN_SELECTED, ALTERNATE_GAME, TRACKED) | ✅ `modinfo.h:84-97` | ⚠️ delegate roles only, enum unproven - `mod_table_view.h:96` | ⚠️ · |
| U121 Flag->emblem icon map (7 mapped, 4 without icons, unknown warns) | ✅ `modflagicondelegate.cpp:47-80` | ⚠️ roles only, exact 7-map unproven - `mod_table_view.h:49` | ⚠️ · |
| U122 Flag delegate returns zero icons for FLAG_OVERWRITE rows | ✅ `modflagicondelegate.cpp:20-24` | ❌ | ❌ · |
| U123 Flag icon sizeHint = count*40 x 20 clamped | ✅ `modflagicondelegate.cpp:90-110` | ❌ | ❌ · |
| U125 ModInfoRegular flag conditions (NOTENDORSED, TRACKED, INVALID, NOTES, PLUGIN_SELECTED, ALTERNATE_GAME, HIDDEN_FILES) | ✅ `modinforegular.cpp:689-711` | ⚠️ 2 of 7 conditions - `nexus_source_panel.cpp:41` | ⚠️ · |
| U128 Generic icon + no-edit delegates (beyond version delegate) | ✅ `modlistversiondelegate.cpp`, `genericicondelegate.cpp` | ❌ | ❌ · |
| U161 Data tab checkboxes + tooltips (conflicts/archives/hidden filters, Refresh tip) | ✅ `mainwindow.ui:1095-1195` | ⚠️ refresh + status-tips only - `data_tab.cpp:830` | ⚠️ · |
| U211 markConverted (converted/working flag) | ✅ `modlistviewactions` | ❌ | ❌ · |
| U212 validated flag (ignore missing data) | ✅ `modinfo` | ✅ writes `[General] validated=true`, read at `:599` - `mod_scanner.cpp:853` | ✅ · |
| U213 hidden files list (restoreHiddenFiles) | ✅ `modinforegular` | ❌ | ❌ · |
| U218 FileTree menu ordering + bold-first-enabled via doubleClicksOpenPreviews | ✅ `filetree.cpp:749-772` | ❌ | ❌ · |

## 10. Mod Info Dialog

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| File tree tab | ✅ `ModInfoDialogFileTree` | ✅ `filetree_tab` - `filetree_tab.cpp:117` | ✅ · |
| ESP/plugin tab | ✅ `ModInfoDialogEsps` | ✅ `esps_tab` - `mod_info_dialog.cpp:8` | ✅ · |
| Nexus tab (embedded page) | ✅ `ModInfoDialogNexus` (endorse/track) | ✅ `source_tab` - `mod_info_dialog.cpp:8` | ✅ · |
| Images tab (gallery, DDS) | ✅ `ModInfoDialogImages` | ✅ `images_tab` - `mod_info_dialog.cpp:8` | ✅ · |
| Text files tab | ✅ `ModInfoDialogTextFiles` | ✅ `text_files_tab` - `text_files_tab.cpp:16` | ✅ · |
| INI files tab | ✅ `IniFilesTab` | ✅ `config_files_tab` - `mod_info_dialog.cpp:8` | ✅ · |
| Categories tab | ✅ `ModInfoDialogCategories` | ✅ `categories_tab` - `categories_tab.cpp:83` | ✅ · |
| Conflicts tab | ✅ `ModInfoDialogConflicts` | ✅ `conflicts_tab` - `conflicts_tab.cpp:182` | ✅ · |
| Notes tab (comments + notes + color) | ✅ `ModInfoNotesTab` | ✅ `notes_tab` - `notes_tab.cpp:24` | ✅ · |
| Generic files tab | ❌ | 🚀 `generic_files_tab` - `generic_files_tab.cpp:1` | 🚀 · |
| Tab reordering | ✅ `onTabMoved` + `saveTabOrder` | ⚠️ internal `tab_order_` vector, not draggable - `mod_info_dialog.cpp:58` | ⚠️ · |
| Tab color coding (data presence) | ✅ `setTabsColors` | ❌ | ❌ · |
| Mod navigation (prev/next) | ✅ `onPreviousMod`, `onNextMod` | ✅ separator-aware nav - `mod_info_dialog.cpp:99` | ✅ · |
| Mod info tab order persistence | ✅ `GeometrySettings::modInfoTabOrder()` | ❌ | ❌ · |

## 11. Version & Update Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Version checking (installed vs newest) | ✅ `ModInfo::updateAvailable` | ✅ `has_update()` + `newestVersion` in meta - `mod_update_db_client.h:150` | ✅ · |
| Newest version tracking | ✅ `ModInfo::newestVersion()` | ✅ `ModInfoResult::newest_version` + meta key - `provider.h:16` | ✅ · |
| Ignored version | ✅ `ModInfo::ignoredVersion()` | ❌ | ❌ · |
| Downgrade detection | ✅ `ModInfo::downgradeAvailable()` | ❌ | ❌ · |
| Batch update check | ✅ `checkAllForUpdate` | ❌ | ❌ · |
| Check update after install | ✅ `Settings::checkUpdateAfterInstallation` | ❌ | ❌ · |
| ModUpdateDbClient (per-game DB poll, ETag/304, offline cache) | ❌ | 🚀 ISO 8601 compare, `by_game` index - `mod_update_db_client.h:124` | 🚀 · |
| Per-game index pipeline (by_game directory, manifest) | ❌ | 🚀 per-game index + shard lookup - `mod_update_db_client.h:164` | 🚀 · |
| GitHub releases API (not Nexus) | ✅ `SelfUpdater` queries GitHub API | ❌ | ❌ · |
| Prerelease filtering | ✅ `selfupdater.cpp` filters by draft/prerelease flags | ✅ filters by prerelease - `self_updater.cpp:107` | ✅ · |
| Update dialog with Markdown changelogs | ✅ `UpdateDialog` (version diff + release notes) | ❌ | ❌ · |
| MOTD (Message of the Day) | ✅ `motdAvailable` signal | ❌ | ❌ · |
| U064 Update check dialogs (no-recent-updates + rate-limit notices) | ✅ `mainwindow.cpp:3151-3170` | ❌ | ❌ · |
| U207 meta.ini nexus timestamps (nexusLastModified/nexusLastQuery/lastNexus*) | ✅ `modinforegular.cpp` | ⚠️ `[Nexusmods]` section exists, 4 keys unproven - `mod_meta.h:32` | ⚠️ · |
| U209 versioningScheme (changeVersioningScheme) | ✅ `modlistviewactions changeVersioningScheme` | ❌ | ❌ · |
| U210 ignoreUpdate flag (setIgnoreUpdate) | ✅ `modlistviewactions setIgnoreUpdate` | ❌ | ❌ · |
| U254 finishUpdateInfo batch flow (per-mod newest versions, update-all) | ✅ `mainwindow.cpp:3151-3310` | ❌ | ❌ · |
| U271 GuessedValue/Versioning version comparison rules | ✅ `versioning.cpp` (uibase) | ❌ | ❌ · |

## 12. Plugin Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Plugin list | ✅ `plugins.txt` | ✅ `plugin_database` - `plugin_database.h:18` | ✅ · |
| Load order sorting (LOOT) | ✅ `LOOT` | ✅ `gmm_lootcli` - `mod_list_controller.cpp:2909` | ✅ · |
| Plugin enable/disable | ✅ | ✅ `plugin_database.cpp:719` | ✅ · |
| ESP header parsing | ✅ `ESPInfo` | ✅ `esp_header` - `esp_header.cpp:1` | ✅ · |
| Plugin diagnostics | ✅ | ✅ `diagnose_registry` - `diagnostics_registry.cpp:10` | ✅ · |
| Plugin file mapper | ✅ `IPluginFileMapper` | ✅ `file_mapper_registry` - `file_mapper_registry.cpp:1` | ✅ · |
| Plugin save parser | ✅ | ✅ `save_parser_registry` - `save_parser_registry.cpp:8` | ✅ · |
| Plugin requirements check | ✅ | ✅ `requirements_registry` - `requirements_registry.h:42` | ✅ · |
| Plugin order encoding | ✅ | ✅ `order_encoding_registry` - `order_encoding_registry.h:25` | ✅ · |
| Master/Light/Medium/Blueprint flags | ✅ `isMasterFlagged`, `isLightFlagged` | ⚠️ 3 of 4, Blueprint unsupported - `plugin_info.h:52` | ⚠️ · |
| Missing masters detection | ✅ `testMasters` + `missingMasters` | ✅ `set_missing_masters()` + emblem - `plugin_view.cpp:183` | ✅ · |
| Plugin lock (pin at position) | ✅ `isESPLocked`, `lockESPIndex` | ✅ `set_locked()` + lockedorder.txt - `profile.h:63` | ✅ · |
| Plugin relationship fix | ✅ `fixPluginRelationships` | ❌ | ❌ · |
| Plugin priority shift | ✅ `shiftPluginsPriority` | ❌ | ❌ · |
| Plugin send to priority | ✅ `sendToPriority` | ⚠️ highest/lowest only, no arbitrary position - `mod_actions.cpp:234` | ⚠️ · |
| Enable/disable all plugins | ✅ `setEnabledAll` | ✅ `set_all_enabled()` - `mod_list_controller.cpp:2864` | ✅ · |
| Plugin index generation (FE/FD) | ✅ `generatePluginIndexes` | ✅ `generate_mod_indexes()` - `mod_list_controller.cpp:2869` | ✅ · |
| LOOT messages per plugin | ✅ `Plugin::messages` | ✅ rendered in tooltip - `plugin_info.h:23` | ✅ · |
| LOOT dirty info (ITM, deleted refs) | ✅ `Dirty` struct | ✅ `DirtyEntry` (ITM/refs/navmesh) - `plugin_info.h:24` | ✅ · |
| LOOT incompatibilities | ✅ `Plugin::incompatibilities` | ✅ rendered - `plugin_info.h:17` | ✅ · |
| LOOT stats (time, version) | ✅ `Stats` | ❌ | ❌ · |
| Plugin author/description display | ✅ `pluginlist.h` | ✅ in tooltip HTML - `plugin_info.h:78` | ✅ · |
| Plugin FormVersion/HeaderVersion | ✅ `formVersion`, `headerVersion` | ✅ `esp_header` rendered - `plugin_view.cpp:292` | ✅ · |
| Plugin archive loading detection | ✅ `loadsArchive` | ⚠️ implicit detection only - `plugin_view.cpp:248` | ⚠️ · |
| Drag-and-drop plugin reorder | ✅ `dropMimeData` | ✅ `InternalMove` + `on_reorder` - `plugin_view.cpp:486` | ✅ · |
| Plugin foreground coloring (LOOT-based) | ✅ `foregroundData()` | ✅ state-based (locked/missing/master) - `plugin_view.cpp:576` | ✅ · |
| Plugin tooltip data (LOOT messages) | ✅ `tooltipData()` | ✅ `plugin_tooltip_html()` - `plugin_view.cpp:271` | ✅ · |
| Plugin highlight from mod selection | ✅ `highlightPlugins()` | ✅ `set_contained_plugins()` / `set_master_plugins()` - `plugin_view.cpp:795` | ✅ · |
| Transitive master enable/disable | ❌ | 🚀 `set_enabled()` closes over masters - `plugin_database.cpp:732` | 🚀 · |
| Band reassertion (native+CC invariant) | ❌ | 🚀 `reassert_band()` - `plugin_database.cpp:460` | 🚀 · |
| Locked order application | ❌ | 🚀 `apply_locked_order()` - `plugin_database.cpp:871` | 🚀 · |
| Plugin type classification | ✅ | ✅ Regular/Master/Light/Medium enum - `plugin_view.h:85` | ✅ · |
| Plugin counter (by type) | ✅ `ModCounters` | ✅ active/total breakdown - `plugins_tab.cpp:67` | ✅ · |
| Zero-record plugin dummy icon | ✅ | ✅ `plugin-dummy` for HEDR count == 0 - `plugin_view.cpp:356` | ✅ · |
| Plugin list sort proxy | ✅ `pluginlistsortproxy.h` (column filtering, custom sorting) | ❌ | ❌ · |
| Plugin list highlight masters | ✅ `PluginList::highlightMasters()` | ❌ | ❌ · |
| Plugin list ChangeBracket (RAII layout notifications) | ✅ `PluginList::ChangeBracket` | ❌ | ❌ · |
| Plugin list view (filter, counter, keyboard nav) | ✅ `pluginlistview.h/cpp` (Space, Ctrl+Up/Down, Ctrl+Enter, Ctrl+F/Esc) | ⚠️ no plugin-side filter or keyboard handler - `plugin_view.cpp:519` | ⚠️ ✔ |
| Plugin list context menu | ✅ `pluginlistcontextmenu.cpp:24-112` (9 groups incl. Send to, Open Origin) | ⚠️ 2 of 9: Lock / Unlock load order - `plugin_context_menu.cpp:24` | ⚠️ ✔ |
| Plugin list model (metadata, type flags) | ✅ `pluginlist.h` (form/header version, author, description) | ✅ all parsed and rendered, in the tooltip not columns - `plugin_info.h:78` | ✅ ✔ |
| U043 BSA enabled-in-INI warning | ✅ `mainwindow.cpp:2071` | ❌ | ❌ · |
| U059 Filter shortcut wiring also on espList + downloadView | ✅ `mainwindow.cpp:495-497` | ❌ | ❌ · |
| U090 Plugin tooltip full block (Loads Archives/INI, ESL/ESH, blueprint, dummy, force-disabled) | ✅ `pluginlist.cpp:1492-1662` | ⚠️ full field list unproven - `plugin_view.cpp:271` | ⚠️ · |
| U107 Plugin list 8 columns (Name..Description) | ✅ `pluginlist.cpp:88-106` = Name, Priority, Mod Index, Flags, Form/Header Version, Author, Description | ⚠️ 5 shipped; 4 columns missing, data already parsed - `plugin_view.cpp:521` | ⚠️ ✔ |
| U114 Plugin header tooltips (8 exact strings) | ✅ `pluginlist.cpp:114-133` | ⚠️ 5 shipped, 4 verbatim, Locked is ours - `right_panel.cpp:222` | ⚠️ ✔ |
| Plugin list double-click opens the owning mod | ✅ `pluginlistview.cpp:310-330` | ✅ Ctrl+double-click reveals the mod folder - `plugin_view.cpp:447` | ✅ ✔ |
| U197 Plugin invalid-names warning + Workarounds plugin settings dialogs | ✅ `gamebryogameplugins.cpp:131` | ❌ | ❌ · |
| U198 PluginList reportError strings (5 exact) | ✅ `pluginlist.cpp:296`, `:513`, `:843` | ❌ | ❌ · |
| U220 Check-BSA single-shot debounce timer | ✅ `mainwindow.cpp:~497` | ❌ | ❌ · |
| U251 BSA list + extract flow (SortableTreeWidget, progress, errors) | ✅ `mainwindow.cpp:1922-2090` | ❌ | ❌ · |
| U293 OBJ Group / LCD counters / managedArchiveLabel hover QToolTip | ✅ `mainwindow.cpp:3942-3945` | ❌ | ❌ · |

## 13. LOOT Integration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| LOOT sort execution | ✅ `Loot::sort()` | ✅ `Sorter::Loot` - `mod_list_controller.cpp:2909` | ✅ · |
| LOOT report generation | ✅ `Loot::createReport()` | ⚠️ JSON only, no HTML/markdown - `sorter.cpp:26` | ⚠️ · |
| LOOT report viewer (markdown + web) | ✅ `LootDialog` + `MarkdownDocument` | ❌ | ❌ · |
| LOOT progress display | ✅ `LootDialog::setProgress()` | ✅ `on_loot_progress(int, QString)` - `mod_list_controller.cpp:2973` | ✅ · |
| LOOT dirty info details | ✅ `Dirty` (CRC, ITM, deleted refs, navmesh, utility) | ⚠️ no CRC field - `plugin_info.h:24` | ⚠️ · |
| LOOT incompatibilities details | ✅ `File` (name + displayName) | ✅ name + displayName - `plugin_info.h:17` | ✅ · |
| LOOT missing masters | ✅ `Plugin::missingMasters` | ✅ `missing_masters_html()` - `plugin_view.cpp:183` | ✅ · |
| Masterlist manager (GitHub walk-down) | ✅ | 🚀 branch walk, 24h TTL, offline fallback - `masterlists.h:21` | 🚀 · |
| LOOT sorted plugin list application | ✅ `lootdialog.cpp` applySortedLoadOrder() | ✅ `apply_load_order(result.sorted_names)` - `mod_list_controller.cpp:3001` | ✅ · |
| LOOT sorted plugin list Markdown rendering | ✅ `loot.h` getSortedPluginListMarkdown() | ❌ | ❌ · |
| LOOT clean info display | ✅ `Plugin::clean` vector ("Verified clean by X") | ✅ `LootReport::clean` - `plugin_info.h:32` | ✅ · |
| LOOT plugin flags (loadsArchive, isMaster, isLightMaster) | ✅ `Plugin` struct per-plugin metadata | ✅ master/light/medium + archives - `plugin_info.h:52` | ✅ · |
| LOOT cancel/terminate | ✅ `Loot::cancel()` terminates lootcli process | ❌ | ❌ · |
| LOOT statistics (timing, versions) | ✅ `Stats` struct (execution time, lootcli version) | ❌ | ❌ · |
| LOOT general messages | ✅ `Report::messages` (non-plugin-specific) | ✅ rendered - `plugin_view.cpp:162` | ✅ · |
| LOOT PluginList integration (addLootReport) | ✅ `PluginList::addLootReport()` | ✅ `set_loot_reports()` side-map - `plugin_database.h:192` | ✅ · |
| LOOT log level setting | ✅ `DiagnosticsSettings::lootLogLevel()` | ❌ | ❌ · |
| LOOT async pipe communication | ✅ `AsyncPipe` (Windows Named Pipe IPC) | ❌ | ❌ · |
| U054 sortButton LOOT tooltip variants | ✅ `mainwindow.cpp:3064-3075` | ❌ | ❌ · |
| U091 makeLootTooltip strings ("Incompatible with %1", Warning/Error prefixes) | ✅ `pluginlist.cpp:1665-1700` | ✅ `plugin_view.cpp:181` | ✅ · |

## 14. Profile Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Profile creation | ✅ `Profile` | ✅ `create_fresh_profile` - `profile_creation.h:48` | ✅ · |
| Profile switching | ✅ | ✅ `switch_profile()` - `profile_switching.cpp:109` | ✅ · |
| Local saves per profile | ✅ `localSavesEnabled` | ✅ `resolve_local_saves()` + UI - `local_saves.h:51` | ✅ · |
| Mod order persistence | ✅ | ✅ `do_write_modlist()` + priority map - `profile.cpp:512` | ✅ · |
| Plugin order persistence | ✅ | ✅ `save_profile()` / `load_profile()` - `plugin_database.cpp:1076` | ✅ · |
| Delayed file writer | ✅ `DelayedFileWriter` | ✅ `delayed_file_writer` - `delayed_file_writer.cpp:1` | ✅ · |
| Safe write file | ✅ | ✅ `safe_write_file` - `safe_write_file.cpp:50` | ✅ · |
| Local INI settings | ✅ `Profile::localSettingsEnabled` | ✅ `local_settings()` + UI checkbox - `profile.cpp:357` | ✅ · |
| Profile INI tweaks | ✅ `Profile::getProfileTweaks` | ✅ `write_tweaked_ini()` + `initweaks.ini` - `profile_switching.cpp:34` | ✅ · |
| Archive invalidation toggle | ✅ `Profile::invalidationActive` | ✅ `automatic_archive_invalidation()` + UI - `profile.cpp:360` | ✅ · |
| Profile locking (plugin order) | ✅ `Profile::getLockedOrderFileName` | ✅ `read/write_locked_order()` - `profile.cpp:543` | ✅ · |
| Profile transfer saves | ✅ `TransferSavesDialog` | ❌ | ❌ · |
| Profile rename | ✅ `Profile::rename` | ✅ `rename_profile()` - `profile_creation.cpp:213` | ✅ · |
| Profile copy | ✅ `Profile::createPtrFrom` | ✅ `copy_profile()` - `profile_creation.cpp:154` | ✅ · |
| Profile forced libraries | ✅ `Profile::determineForcedLibraries` | ⚠️ profile copy only, no runtime load - `profile_creation.cpp:199` | ⚠️ · |
| Profile switch result + callbacks | ❌ | 🚀 `ProfileSwitchResult` + `Callbacks` - `profile_switching.h:22` | 🚀 · |
| Profile switch EventBus emission | ❌ | 🚀 `kProfileChanged` - `event_bus.h:41` | 🚀 · |
| Profile bar (combo + folder shortcuts) | ❌ | 🚀 `ProfileBar` (12 FolderKind, export/import) - `profile_bar.h:28` | 🚀 · |
| Profile deletion | ✅ `on_removeProfileButton_clicked()` | ✅ `on_delete_profile()` + active guard - `profile_manager_dialog.cpp:229` | ✅ · |
| Active profile protection (cannot rename/delete active) | ✅ `profilesdialog.cpp` | ✅ active-profile guard - `profile_manager_dialog.cpp:204` | ✅ · |
| Profile creation with default vs copy choice | ✅ `ProfileInputDialog` (getPreferDefaultSettings) | ✅ copy-source combo ("(fresh)" + profiles) - `profile_create_dialog.h:10` | ✅ · |
| Mod priority management per profile | ✅ `Profile::setModPriority()`, `setModsEnabled()` | ✅ `set_mod_priority()` + `priority_of()` - `profile.h:185` | ✅ · |
| Mod status signal | ✅ `Profile::modStatusChanged` signal | ✅ EventBus `kModStateChanged` - `event_bus.h:36` | ✅ · |
| Tweaked INI creation | ✅ `Profile::createTweakedIniFile()` | ✅ `write_tweaked_ini()` - `profile_switching.cpp:34` | ✅ · |
| Profile settings as arbitrary key/value | ✅ `Profile::setting()`, `storeSetting()` | ✅ `get_setting()` / `set_setting()` - `profile.h:225` | ✅ · |
| Rename mod across all profiles | ✅ `Profile::renameModInAllProfiles()` | ❌ | ❌ · |
| Active mods retrieval | ✅ `Profile::getActiveMods()` | ❌ | ❌ · |
| Profile existence check | ✅ `Profile::exists()` | ✅ `Profile::exists()` - `profile.h:101` | ✅ · |
| Profile find settings (auto-detect) | ✅ `Profile::findProfileSettings()` | ✅ `detect_local_settings()` - `profile_creation.cpp:38` | ✅ · |
| Modlist write cancellation | ✅ `Profile::cancelModlistWrite()` | ✅ `cancel_modlist_write()` - `profile.cpp:510` | ✅ · |
| Profile debug dump | ✅ `Profile::debugDump()` | ✅ profile display - `debug_window.cpp:852` | ✅ · |
| Profile INI files (full set - 7 file paths) | ✅ `Profile` (plugins, loadorder, lockedorder, modlist, archives, ini, tweaks) | ✅ path accessors + `initweaks.ini` - `profile.h:212` | ✅ · |
| U003 Ctrl+P = Profiles... | ✅ `mainwindow.ui:1676-1696` | ❌ | ❌ · |
| U050 profileBox "<Manage...>" sentinel entry opening profiles dialog | ✅ `mainwindow.cpp:1822-1864` | ✅ kManageProfilesText, sentinel at index 0 - `profile_bar.cpp:19` | ✅ · |
| U132 General > Profile Defaults group (Local INIs / Saves / Archive Invalidation) | ✅ `settingsdialog.ui:234-261` | ⚠️ checkboxes mapped, invalidation parity unproven - `profile_settings_widget.h:11` | ⚠️ · |
| U166 Profiles dialog full control set (3 checkables + 7 buttons w/ tooltips) | ✅ `profilesdialog.ui` | ⚠️ create/copy/rename/delete only - `profile_manager_dialog.h:13` | ⚠️ · |
| U167 Profiles dialog messages (invalid name, active guards, broken profile) | ✅ `profilesdialog.cpp:78-300` | ❌ | ❌ · |
| U173 ProfileInputDialog (name prompt + Default Game INI Settings checkbox) | ✅ `profileinputdialog.ui` | ⚠️ dialog exists, checkbox parity unproven - `profile_create_dialog.h:10` | ⚠️ · |
| U214 Profile 7-file set + settingsByGroup arbitrary keys + meta timer | ✅ `profile.cpp` | ✅ 7 accessors + 5s modlist debounce proven - `profile.h:81` | ✅ · |
| U215 Profile initweaks / invalidation archive-list interactions | ✅ `mainwindow.cpp:2585-2603` | ⚠️ writers exist, invalidationActive toggling unproven - `profile_switching.cpp:76` | ⚠️ · |
| U221 Archive list saved via DelayedFileWriter on modPrioritiesChanged | ✅ `mainwindow.cpp:564-566` | ❌ | ❌ · |

## 15. Download Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Download state machine | ✅ `DownloadState` (14 states) | ⚠️ 6 states, `Removed` reserved and never assigned - `downloads_tab.h:35` | ⚠️ ✔ |
| Pause/resume downloads | ✅ `pauseDownload`, `resumeDownload` | ✅ pause = menu only; resume also row double-click - `downloads_controller.cpp:402` | ✅ ✔ |
| Cancel downloads | ✅ `cancelDownload` | ⚠️ no Cancel action or state; Pause is the only abort - `network_manager.cpp:1231` | ⚠️ ✔ |
| Download speed tracking | ✅ `downloadSpeed` rolling average | ⚠️ instantaneous only, no rolling average or history - `downloads_tab.cpp:461` | ⚠️ ✔ |
| MD5 lookup | ✅ `queryInfoMd5` | ❌ nothing computes an MD5 - `collection/nexus/parser.cpp:136` | ❌ ✔ |
| Download meta files (sidecar) | ✅ `createMetaFile` | ⚠️ written for installed mods, never next to a download - `downloads_tab.cpp:775` | ⚠️ ✔ |
| Hidden downloads | ✅ `isHidden`, `restoreDownload` | ❌ no download `isHidden`, no restore path | ❌ ✔ |
| Automatic retry (3x) | ✅ `AUTOMATIC_RETRIES` | ✅ retries, backoff, Retry-After - `network_options_bridge.cpp:25` | ✅ ✔ |
| Pending download queue | ✅ `PendingDownload` | ✅ `pending_` drained FIFO; `nexus_queue_downloads` is GMM-only - `pipeline_worker.cpp:317` | ✅ ✔ |
| Multi-URL fallback | ✅ `m_Urls`, `m_CurrentUrl` | 🚫 one signed URL per download; re-request is the retry - `Mod::download_url` | 🚫 ✔ |
| Hide after install | ✅ `InterfaceSettings::hideDownloadsAfterInstallation()` | 🚫 a toggled view filter, not post-install auto-hide - `downloads_tab.cpp:208` | 🚫 ✔ |
| Download notifications | ✅ `showDownloadNotifications` (tray `downloadmanager.cpp:1825`) | ✅ gates a status-bar message; all four EventBus names dispatched - `downloads_controller.cpp:88` | ✅ ✔ |
| Compact downloads view | ✅ `compactDownloads` | ✅ `apply_compact_style` / `row_height` - `downloads_tab.cpp:302` | ✅ ✔ |
| Drag-and-drop import | ❌ | 🚀 drop moves/copies + "Manual" row; single link -> Add-from-URL - `downloads_tab.cpp:844` | 🚀 ✔ |
| Directory watcher (auto-detect) | ❌ | 🚀 `QFileSystemWatcher`, 200 ms debounce, re-armed on switch - `downloads_tab.cpp:289` | 🚀 ✔ |
| Manifest serialization (JSON) | ❌ | 🚀 `serialize`/`deserialize` + legacy-label repairs - `downloads_tab.cpp:1186` | 🚀 ✔ |
| Content-Disposition filename parsing | ❌ | 🚀 RFC 6266/5987 incl. `filename*`, `..` reject - `curl_download.cpp:136` | 🚀 ✔ |
| HTTP Range resume | ❌ | ⚠️ `CURLOPT_RESUME_FROM_LARGE`; repeat link refused first - `downloads_controller.cpp:826` | ⚠️ ✔ |
| NXM protocol download handler | ✅ `addNXMDownload()` (nxm:// link processing) | ✅ `NxmIpcServer` -> `NxmRouter::parse` -> `handle_nxm_download`; `modl://` shares it - `nxm_router.cpp:57` | ✅ ✔ |
| Plugin download API (startDownloadURLs, etc.) | ✅ `IDownloadManager` plugin API | ❌ ABI-shaped: no download surface in v1/v2, needs a v3 decision - `projects/ABI/include/` | ❌ ✔ |
| Nexus collection link rejection | ✅ `nxmInfo.isCollection()` "Collections Not Supported" `downloadmanager.cpp:740` | ⚠️ rejects malformed collection links; collection import is a planned epic - `downloads_controller.cpp:691` - Workspace-kdfn | ⚠️ ✔ |
| Duplicate download detection (file name + auto-rename) | ✅ `getDownloadFileName(baseName, renameToUnique)` | ✅ Overwrite/Rename/Ignore, MO2's 1-based `N_` naming - `downloads_tab.cpp:800` | ✅ ✔ |
| Duplicate NXM prevention (mod+file ID dedup) | ✅ checks pending+active for (modID, fileID) `downloadmanager.cpp:788` | ✅ `blocks_refetch` refuses Downloading/Complete/Installed at all 3 link entry points - `downloads_controller.cpp:826` | ✅ ✔ |
| .unfinished extension for in-progress files | ✅ `UNFINISHED[]` `downloadmanager.cpp:57` | ❌ writes the final name; scan guards on `has_active_download()` - `downloads_tab.cpp:682` | ❌ ✔ |
| HTTP/2 download support | ✅ `QHttp2Configuration` (16 MiB windows, Windows QNAM) | 🚀 HTTP/3 via `use_http3` with ALPN fallback to HTTP/2 - `network_manager.cpp:800` | 🚀 ✔ |
| Login-gated resume | ✅ `m_core.loggedInAction(...)` `downloadstab.cpp:112` | ❌ no session check before resume | ❌ ✔ |
| Empty URL resume prevention | ✅ "No known download urls." `downloadmanager.cpp:1072` | ✅ refuses to resume, now posts to the status bar - `downloads_controller.cpp:421` | ✅ ✔ |
| Orphan meta file cleanup | ✅ `refreshList()` `downloadmanager.cpp` | 🚫 no download sidecar is written, so no orphan class | 🚫 ✔ |
| Server preference sorting (mirror priority) | ✅ `ServerByPreference`, `evaluateFileInfoMap()` | 🚫 one signed CDN URL per request, no mirror list to order | 🚫 ✔ |
| Per-server speed history (persistent stats) | ✅ `ServerInfo` rolling speed list | 🚫 nothing identifies which server served a byte | 🚫 ✔ |
| Download speed signal for stats persistence | ✅ `downloadSpeed(serverName, bytesPerSecond)` | 🚫 feeds the per-server history; no server name to put on it | 🚫 ✔ |
| Open meta file (sidecar viewer) | ✅ `openMetaFile()` `downloadmanager.cpp:1299` | ❌ no download sidecar to open | ❌ ✔ |
| Manual metadata re-query (info + MD5) | ✅ `queryInfo()`, `queryInfoMd5()` | ❌ no per-row Query Info action | ❌ ✔ |
| Batch metadata query for all incomplete | ✅ `queryDownloadListInfo()` | ❌ folder watcher re-reads disk, not Nexus metadata - `downloads_tab.cpp:677` | ❌ ✔ |
| MD5 multi-game namespace search | ✅ `queryInfoMd5()` alternate game short names | ❌ follows from there being no MD5 lookup | ❌ ✔ |
| MD5 result disambiguation | ✅ filename + active file matching `downloadmanager.cpp` | ❌ same consequence | ❌ ✔ |
| File type classification display | ✅ `getFileTypeString()` (Main/Update/Optional) | ❌ Main-vs-Update never recorded; no Filetype column - `downloads_tab.cpp:127` | ❌ ✔ |
| Mark installed / mark uninstalled | ✅ `markInstalled()` `downloadmanager.cpp:1540` | ✅ both; uninstalled re-lands on Complete, archive still on disk - `downloads_tab.cpp:577` | ✅ ✔ |
| Install from download (double-click / context menu) | ✅ `downloadlistview.cpp` install action | ✅ row double-click + menu; origin provenance via `source_info_for` - `downloads_tab.cpp:922` | ✅ ✔ |
| Download status color coding | ✅ `downloadlist.cpp:205-211` (darkGreen/Yellow/Red) | ⚠️ 6 states to 3 colours, hex not palette - `downloads_tab.cpp:522` | ⚠️ ✔ |
| Remaining time estimation | ✅ `downloadmanager.cpp` ETA calculation | ❌ renders `"%p% - <speed>"`, no time component - `downloads_tab.cpp:476` | ❌ ✔ |
| Batch delete operations (all/installed/uninstalled) | ✅ `downloadlistview.cpp:300-306` | ❌ per-row only, to trash with a restore note - `downloads_tab.cpp:1136` | ❌ ✔ |
| Batch hide operations + un-hide all | ✅ `downloadlistview.cpp:312-322` (4 batch actions) | ❌ one toggled `Hide installed` filter instead - `downloads_tab.cpp:208` | ❌ ✔ |
| Filter widget for downloads | ✅ `FilterWidget` (fuzzy match) | ⚠️ shared right-panel bar, composed with the installed filter - `right_panel.cpp:175` | ⚠️ ✔ |
| Meta/display name toggle setting | ✅ `metaDownloads` `downloadlist.cpp` | ❌ one name per row, no toggle | ❌ ✔ |
| Keyboard shortcuts (Enter=install, Delete=remove, Space=pause) | ✅ `downloadlistview.cpp` | ❌ no `keyPressEvent`/`eventFilter`/`QShortcut` on the tab | ❌ ✔ |
| Visit on Nexus (from download) | ✅ `visitOnNexus()` `downloadmanager.cpp:1234` | ✅ "Open on <Source>" from stored domain + mod id - `downloads_tab.cpp:1095` | ✅ ✔ |
| Visit uploader profile | ✅ `visitUploaderProfile()` `downloadmanager.cpp` | ❌ `uploader` absent from `src/`; no `queryInfo` equivalent | ❌ ✔ |
| Plugin download callbacks (onDownloadComplete/Paused/Failed/Removed) | ✅ `IDownloadManager` boost::signals2 | ❌ ABI-shaped: internal EventBus has all four, the ABI has none - `event_bus.h:45` | ❌ ✔ |
| TaskProgress integration (Windows taskbar progress) | ✅ `TaskProgressManager` `downloadmanager.cpp` | 🚫 no Linux counterpart; the per-row bar carries it | 🚫 ✔ |
| S3 signed URL filename extraction | ✅ `response-content-disposition=` `downloadmanager.cpp` | ✅ same parser as Content-Disposition - `curl_download.cpp:136` | ✅ ✔ |
| File time fallback chain (birthTime -> metadataChangeTime -> lastModified) | ✅ `getFileTime()` `downloadmanager.cpp` | ✅ `update_filetime` walks the same chain - `downloads_tab.cpp:418` | ✅ ✔ |
| U037 Drop-onto-downloads duplicate dialog (Overwrite / Rename / Ignore) | ✅ `mainwindow.cpp:4002-4017` | ✅ `downloads_tab.cpp:800` with dialog at `:266` | ✅ ✔ |
| U038 dragEnterEvent accepts Copy/Move + supported archives | ✅ `mainwindow.cpp:3947-3991` | ✅ `accepts_url_drop` gates local file + 8 extensions - `downloads_tab.cpp:89` | ✅ ✔ |
| U039 dropEvent (TargetMoveAction coercion, shellCopy/Move, URL) | ✅ `mainwindow.cpp:4034-4050` | ✅ coerces MoveAction, copies, routes URL via `import_dropped_file` - `downloads_tab.cpp:856` | ✅ ✔ |
| U045 "Can't change download directory while downloads are in progress" toast | ✅ `mainwindow.cpp:2803` | ❌ dir is derived from the instance, no mid-session setting | ❌ ✔ |
| U062 showHiddenBox toggles downloadManager setShowHidden | ✅ `mainwindow.cpp:3818-3821` | ❌ no hidden-download state to toggle | ❌ ✔ |
| U096 Download row tooltip (filename, info-missing hint, modName version desc, Pending) | ✅ `downloadlist.cpp:213-232` | ❌ no `Qt::ToolTipRole` handler at all - `downloads_tab.cpp` | ❌ ✔ |
| U097 Download status colors (READY/UNINSTALLED/PAUSED foregrounds) | ✅ `downloadlist.cpp:205-211` | ⚠️ mapping not one-to-one: 6 GMM states vs 9 - `downloads_tab.cpp:1105` | ⚠️ ✔ |
| U108 Download list 8 columns (4 hidden by default, ini override) | ✅ `downloadlist.h:38-51` = NAME, STATUS, SIZE, FILETIME, MODNAME, VERSION, ID ("Nexus ID"), SOURCEGAME | ⚠️ 6 shipped; Mod name, Version, Source Game absent, Source is ours - `downloads_tab.cpp:127` | ⚠️ ✔ |
| U109 Downloads header right-click per-column checkbox menu (QWidgetAction) | ✅ `downloadlistview.cpp:178-210` | ✅ `ColumnToggleHeaderView` labels from `column_names()` - `right_panel.cpp:277` | ✅ ✔ |
| U124 Warning_16 icon on download Name cell when metadata incomplete | ✅ `downloadlist.cpp:233-240` | ❌ no `DecorationRole` branch, no incomplete state | ❌ ✔ |
| U162 Downloads bar buttons (Refresh / Query download info / show-hidden tips) | ✅ `mainwindow.ui:1319-1430` | ⚠️ no bar; F5 refresh + watcher cover 2 of 3 jobs - `menu_bar.cpp:192` | ⚠️ ✔ |
| U216 Double-click download row (READY->install, PAUSED->resume) | ✅ `downloadlistview.cpp:164-177` | ✅ `on_cell_double_clicked()` past Complete installs - `downloads_tab.cpp:922` | ✅ ✔ |
| Download list header tooltips | ❌ (MO2 has none) | 🚀 6, one per shipped column - `downloads_tab.cpp:158` | 🚀 ✔ |
| Download context menu | ✅ `downloadlistview.cpp:231-322` (incl. 7 batch Delete/Hide entries) | ⚠️ 5 entries; Query Info, Cancel, Un-Hide and all batch ops absent - `downloads_tab.cpp:1024` | ⚠️ ✔ |
| U217 Downloads keyboard Enter/Delete state gating | ✅ `downloadlistview.cpp:326+` | ❌ no `keyPressEvent`; Up/Down navigate natively | ❌ ✔ |
| U225 Downloads drag accepted only over downloadTab rect | ✅ `mainwindow.cpp:3947` | 🚫 accepts over the whole tab widget, a deliberate superset - `downloads_tab.cpp:261` | 🚫 ✔ |
| U226 MoveAction -> TargetMoveAction coercion on drop | ✅ `mainwindow.cpp:4034-4044` | 🚫 already shipped - `downloads_tab.cpp:870` | 🚫 ✔ |
| U243 IDownloadManager API + downloadmanagerproxy requestDownload slot | ✅ `mainwindow.cpp:1576` | ❌ ABI-shaped, same v3 decision as the plugin-API rows | ❌ ✔ |
| U279 ServerInfo per-server speed history + preferred-servers drag lists (NEXUS-LENS: genericize/provider-scope) | ✅ `serverinfo.cpp`, `settingsdialognexus.cpp:366` | 🚫 no mirror list, so nothing to rank or time | 🚫 ✔ |

## 16. Nexus Integration

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| NXM handler registration | ✅ `registerAsNXMHandler` | ✅ `Source::Router` + `nxm_ipc` - `source/router.h:46` | ✅ · |
| Nexus account / auth | ✅ | ✅ `nexus_account` + `nexus_auth` - `nexus/auth.h:39` | ✅ · |
| Nexus HTTP API | ✅ | ✅ `nexus_http` + `nexus_servers` - `nexus/http.h:15` | ✅ · |
| Endorsement system | ✅ `ModInfo::endorse()` | ⚠️ UI + setting only, opens browser - `nexus_source_panel.cpp:41` | ⚠️ · |
| Tracking system | ✅ `ModInfo::track()` | ⚠️ UI + setting only, opens browser - `nexus_source_panel.cpp:46` | ⚠️ · |
| "Never endorse" option | ✅ `ModInfo::setNeverEndorse` | ❌ | ❌ · |
| Nexus description (HTML) | ✅ `ModInfo::getNexusDescription` | ✅ meta key + BBCode rendering - `mod_meta.cpp:118` | ✅ · |
| Nexus category ID tracking | ✅ `ModInfo::getNexusCategory` | ✅ meta key, read/write - `mod_meta.cpp:117` | ✅ · |
| Nexus update timestamps | ✅ `getLastNexusUpdate/Query` | ⚠️ key recognized in meta, not actively used - `mod_meta.cpp:115` | ⚠️ · |
| OAuth login flow | ✅ `NexusOAuthLogin` | ❌ | ❌ · |
| Nexus user account info | ✅ `ApiUserAccount` | ✅ `NexusUserInfo` - `nexus/auth.h:29` | ✅ · |
| Rate limit tracking | ✅ | ✅ `RateLimitInfo` (hourly + daily) - `nexus/auth.h:15` | ✅ · |
| Download mirror registry | ❌ | 🚀 `NexusServers` (speed samples, preferred ordering) - `nexus/servers.h:28` | 🚀 · |
| Nexus API key manual entry | ✅ | ✅ `NexusManualKeyDialog` - `source_pages.h:37` | ✅ · |
| Tier-derived queue defaults | ❌ | 🚀 `nexus_queue_default_for()` (Regular/Premium) - `source_pages.h:24` | 🚀 · |
| Nexus connection UI (reusable component) | ✅ `NexusConnectionUI` (shared Settings/CreateInstance) | ❌ | ❌ · |
| Endorsement state tracking | ✅ `EndorsementState` enum (Accepted/Refused/NoDecision) | ❌ | ❌ · |
| Nexus FileStatus (REMOVED, ARCHIVED) | ✅ `NexusInterface::FileStatus` | ❌ | ❌ · |
| U007 Visit Nexus action (Ctrl+N; genericized to "visit modding sites" per U001 ruling) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.ui:1757-1777` | ❌ | ❌ · |
| U013 Endorse ModOrganizer submenu (Endorse / Won't Endorse) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1061-1078` | ❌ | ❌ · |
| U014 Browse Mod Page menu (IPluginModPage entries + "Visit <game> on Nexus") (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1568-1686` | ❌ | ❌ · |
| U058 Endorse-MO dialogs + toasts (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:2995-3020` | ❌ | ❌ · |
| U063 Nexus failure dialogs (blocked action, mod ID gone, request failed) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:3577-3612` | ❌ | ❌ · |
| U066 "Browse Mod Page" action exists but hidden by default | ✅ `mainwindow.ui:1778-1798` | ❌ | ❌ · |
| U098 API counter tooltip (pools + exhaustion warning paragraph) (NEXUS-LENS: genericize/provider-scope) | ✅ `statusbar.cpp:44-50` | ❌ | ❌ · |
| U099 API counter text/colors (Queued/Daily/Hourly + 500/200 thresholds) (NEXUS-LENS: genericize/provider-scope) | ✅ `statusbar.cpp:96-135` | ⚠️ "Queued:" counter only; colors + hide flag unproven - `status_bar.cpp:101` | ⚠️ · |
| U141 Nexus connection state machine strings + Connect->Cancel flip (NEXUS-LENS: genericize/provider-scope) | ✅ `settingsdialognexus.cpp:117-334` | ❌ | ❌ · |
| U206 meta.ini endorsed / tracked states (NEXUS-LENS: genericize/provider-scope) | ✅ `modinforegular.cpp:689-711` | ⚠️ integration gating only, enum unproven - `nexus_source_panel.cpp:41` | ⚠️ · |
| U236 IPluginModPage plugin API (pageURL, useIntegratedBrowser, icon) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:1568` (uibase) | ❌ | ❌ · |
| U242 IPluginGame fields (getSupportURL, primarySources/validShortNames, blueprintPrefix, steamAPPId...) | ✅ `mainwindow.cpp:1060-1660` | ⚠️ registry layer exists; support-URL + primarySources absent - `game_feature_registry.h:110` | ⚠️ · |
| U252 NexusInterface full API surface (every request type signalled + userData routing) (NEXUS-LENS: genericize/provider-scope) | ✅ `nexusinterface.cpp` | ⚠️ auth/queue/settings bridge only - `source_pages.cpp:472` | ⚠️ · |
| U253 nxm*Available signal handlers w/ per-error UI (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:3077-3612` | ❌ | ❌ · |
| U255 NXMAccessManager (OAuth flow, credentialsReceived title update, cookie jar) (NEXUS-LENS: genericize/provider-scope) | ✅ `nxmaccessmanager.cpp`, `mainwindow.cpp:465` | ❌ API-key auth only, no OAuth flow - `source_pages.cpp:472` | ❌ · |
| U256 API account shown in window title (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:653-668` | ❌ | ❌ · |
| U257 API stats -> StatusBar::setAPI via requestsChanged (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:467-469` | ⚠️ counter text exists, wiring unproven - `status_bar.cpp:101` | ⚠️ · |
| U258 Per-mod endorsement state from API (EndorsedState incl. unknown-disabled) (NEXUS-LENS: genericize/provider-scope) | ✅ `modlistcontextmenu.cpp:553-575` | ❌ | ❌ · |
| U259 Nexus FileStatus incl. ARCHIVED_HIDDEN (NEXUS-LENS: genericize/provider-scope) | ✅ `modlist.cpp:434-436` | ❌ | ❌ · |
| U260 Nexus manual API-key dialog + disconnect clears stored auth (NEXUS-LENS: genericize/provider-scope) | ✅ `settingsdialog.ui:1241` | ⚠️ dialog exists, disconnect-clears unproven - `source_pages.cpp:98` | ⚠️ · |
| U276 NxmUrl parsing helpers (uibase nxmurl.cpp) (NEXUS-LENS: genericize/provider-scope) | ✅ `nxmurl.cpp` (uibase) | ⚠️ URLs arrive parsed; encode helpers unproven - `downloads_controller.cpp:223` | ⚠️ · |
| U278 PersistentCookieJar (Nexus login cookie persistence) (NEXUS-LENS: genericize/provider-scope) | ✅ `persistentcookiejar.cpp` | ❌ | ❌ · |

## 17. Source Providers

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Nexus Mods integration | ✅ | ✅ `NexusProvider` - `nexus_provider.h` | ✅ · |
| Steam Workshop | ✅ | ✅ `SteamWorkshopProvider` - `steam_workshop_provider.h` | ✅ · |
| LOVERS LAB | ❌ | 🚀 `LoversLabProvider` - `loverslab/provider.h:35` | 🚀 · |
| Download manager | ✅ `DownloadManager` | ✅ `curl_download` - `download/curl_download.cpp` | ✅ · |
| Remote cache | ✅ | ✅ `MasterlistManager` (24h TTL, temp-file download, offline cache reuse) - `sort/sorter/loot/masterlists.h:21` | ✅ · |
| Steam Workshop client (Web API) | ❌ | 🚀 `WorkshopClient` (SQLite cache, dead IDs) - `steam/workshop_client.h:29` | 🚀 · |
| LoversLab session-cookie auth | ❌ | 🚀 `LoversLabAuth` (Cloudflare stripping) - `loverslab/auth.h:41` | 🚀 · |
| Managed games tracking | ❌ | 🚀 `ManagedGames` (source_id, website_url, nexus_domain) - `nxm/managed_games.h:23` | 🚀 · |
| modl:// protocol handler (mod.pub / MO2 modlhandler) | ❌ | 🚀 `modl://` (Win registry + XDG desktop) - `nxm_router.cpp:239` | 🚀 · |
| ModPub metadata provider (mod.pub API) | ❌ | 🚀 `ModPubProvider` (page scrape, mod_id, Refresh) - `modpub/provider.h:64` | 🚀 · |
| modl:// transport-protocol distinction (origin attribution) | ❌ | 🚀 transport not source; real origin attributed - `install_stage.cpp:331` | 🚀 · |
| U275 ModRepositoryFileInfo + imodrepositorybridge repository abstraction | ✅ `imodrepositorybridge.cpp` (uibase) | ❌ | ❌ · |

## 18. Mod List Features

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Multi-criteria sorting | ✅ `ModListSortProxy` | ⚠️ game sort provider only, no user criteria - `mod_list_controller.cpp:1097` | ⚠️ · |
| Category/content/special filtering | ✅ `FilterList` (special + content + category, 3-state, And/Or) | ⚠️ category tree + 6-item combo; no special/content criteria - `category_filter_panel.h:21` | ⚠️ ✔ |
| Grouping (by separator, category, Nexus ID) | ✅ `QtGroupingProxy` | ✅ visual nesting (parent_id, indent, fold) - `mod_list_model.h:57` | ✅ · |
| Drag-and-drop reorder | ✅ `ModList::dropMimeData` | ✅ drop support - `mod_table_view.h:185` | ✅ · |
| Scroll markers | ✅ `ViewMarkingScrollBar` | ✅ `ModMarkingScrollBar` (separator marks) - `mod_table_view.h:116` | ✅ · |
| CSV export | ✅ `exportModListCSV` | ❌ (removed; combined import/export instead) | ❌ · |
| Bulk enable/disable | ✅ `setActive(indices)` | ✅ `toggle_selected_mods()` - `mod_list_controller.h:140` | ✅ · |
| Priority shift (bulk) | ✅ `shiftModsPriority` | ✅ `priority_move_selected()` - `mod_list_controller.h:139` | ✅ · |
| Send to top/bottom/priority | ✅ `sendModsToTop/Bottom/Priority` | ✅ `send_to_highest/lowest_priority()` - `mod_list_controller.h:135` | ✅ · |
| Send to separator | ✅ `sendModsToSeparator` | ✅ `move_to_separator()` - `mod_list_controller.h:133` | ✅ · |
| Send to First/Last Conflict | ✅ `sendModsToFirstConflict/LastConflict` | ❌ | ❌ · |
| Collapseable separators | ✅ `collapsibleSeparators` | ✅ 10+ settings - `settings.h:35` | ✅ · |
| Auto-collapse on hover | ✅ `autoCollapseOnHover` | ✅ `auto_collapse_on_hover()` - `settings.h:33` | ✅ · |
| Filter persistence | ✅ `saveFilters` | ✅ `save_filters()` - `settings.h:31` | ✅ · |
| Filter AND/OR mode | ✅ `FilterAnd`/`FilterOr` | ✅ `engine::filter::Mode` + 2 radios - `filter_combine.h:14` | ✅ · |
| Column visibility toggle | ✅ `setColumnVisible()` | ✅ `ColumnToggleHeaderView` - `column_toggle_header.h:7` | ✅ · |
| Mod counter display | ✅ `ModCounters` (LCD) | ✅ QLCDNumber - `mod_list_controller.cpp:648` | ✅ · |
| Create separator | ✅ | ✅ `create_separator()` / `create_separator_named()` - `mod_actions.h:63` | ✅ · |
| Create empty mod | ✅ `createEmptyMod` | ✅ `create_empty_mod()` - `mod_actions.h:65` | ✅ · |
| Import archives | ✅ | ✅ `import_archives()` - `mod_list_controller.cpp:3348` | ✅ · |
| Export/import modlist | ✅ | ✅ `export_modlist()` / `import_modlist()` - `mod_list_controller.h:59` | ✅ · |
| Overwrite file drop-to-mod | ❌ | 🚀 `overwrite_files_dropped` signal - `mod_table_view.h:178` | 🚀 · |
| IndentDelegate (nesting) | ❌ | 🚀 `IndentDelegate` for Name column - `mod_table_view.h:138` | 🚀 · |
| FlagsDelegate with tooltips | ❌ | 🚀 per-emblem hover text - `mod_table_view.h:89` | 🚀 · |
| ModListSortProxy criteria system | ✅ `ModListSortProxy::Criteria` | ❌ | ❌ · |
| ModListSortProxy separator mode | ✅ `ModListSortProxy::SeparatorMode` | ❌ | ❌ · |
| ModList column roles (IndexRole, PriorityRole) | ✅ `ModList::IndexRole`, `PriorityRole` | ❌ | ❌ · |
| ModList signals (showMessage, modRenamed, modUninstalled, fileMoved, modPrioritiesChanged) | ✅ `ModList` signals | ❌ | ❌ · |
| ModListProxy / ModListByPriorityProxy | ✅ proxy models | ❌ | ❌ · |
| U010 Ctrl+F focuses+selects filter (modList, espList, downloadView) | ✅ `mainwindow.cpp:217`, `:495-497` | ✅ one window-scoped `QShortcut`, same 3 surfaces - `main_window.cpp:348` | ✅ ✔ |
| U011 Escape clears filter and returns focus to list | ✅ `mainwindow.cpp:224` | ✅ per-pane `WidgetWithChildrenShortcut`, focus decides which filter answers - `main_window.cpp:376` | ✅ ✔ |
| Double-click maps the clicked column to a Mod Info tab | ✅ `modlistview.cpp` (Name→Source, Priority→Conflicts, Category→Categories) | ⚠️ Conflicts/Flags, Category, Source mapped; rest → last-used tab - `mod_list_controller.cpp:469` | ⚠️ ✔ |
| Double-click a separator toggles its fold | ✅ `modlistview.cpp` | ✅ plus an anti-bounce guard - `mod_table_view.cpp:383` | ✅ ✔ |
| U053 Wheel-scroll blocked on groupCombo/profileBox | ✅ `mainwindow.cpp:378-385` | ✅ `EventFilter` on both - `mod_filter_bar.cpp:63`, `profile_bar.cpp:46` | ✅ · |
| U074 Row-with-children menu (Collapse all / others / Expand all) | ✅ `modlistcontextmenu.cpp:236-243` | ❌ | ❌ · |
| U077 "Send to..." conditionality (priority-sort gating + First/Last conflict flags) | ✅ `modlistcontextmenu.cpp:273-332` | ⚠️ conditional send-to tree; conflict flags unproven - `mod_context_menu.cpp:280` | ⚠️ · |
| U092 Mod-list cell tooltips per column (flags/conflicts/name/version/category/notes) | ✅ `modlist.cpp:384-480` | ⚠️ 5 of 6; Version and Notes need untracked data - `mod_list_model.cpp:194` | ⚠️ ✔ |
| U095 Mod list header tooltips (13 exact strings) | ✅ `modlist.cpp:1345-1387` | ⚠️ 11 shipped, 7 verbatim; Source ID wording is ours - `mod_list_model.cpp:553` | ⚠️ ✔ |
| U106 Mod list 13 columns | ✅ `modlist.h:83-96` (COL_NAME…COL_NOTES) | ⚠️ 11 shipped; Content, Author, Uploader, Source Game, Notes absent - `mod_list_model.h:115` | ⚠️ ✔ |
| Mod list default-hidden column set | ✅ `modlistview.cpp:817-824` | ⚠️ hides Source ID, Installation + own Source, Changed - `mod_list_model.cpp:576` | ⚠️ ✔ |
| U110 Editable-cell rules (priority/version/ModID; foreign guards; auto-priority) | ✅ `modlist.cpp:620-650` | ❌ | ❌ · |
| U111 Version column "?" when empty + canBeUpdated | ✅ `modlist.cpp:198-206` | ❌ | ❌ · |
| U113 Priority cell hidden for automatic-priority mods | ✅ `modlist.cpp:207-213` | ❌ | ❌ · |
| U127 HIGHLIGHT_CENTER centers Name cell alignment | ✅ `modlist.cpp:640-647` | ❌ | ❌ · |
| U136 Mod List settings (separator colors, out-of-MO mods, remember filters, update-on-install, drag collapse) | ✅ `settingsdialog.ui:557-635` | ⚠️ tab exists, 5 checkboxes unproven - `settings_content_widget.cpp:450` | ⚠️ · |
| U137 Collapsible Separators settings group (sort-direction, highlights, icon toggles, per-profile) | ✅ `settingsdialog.ui:653-830` | ⚠️ 12 keys shipped, plugin-highlight nuance unproven - `settings.h:35` | ⚠️ · |
| U163 Filter panel controls (Clear/Edit, And/Or radios, filters tree) | ✅ `mainwindow.ui:137-200`, `:505` | ❌ | ❌ · |
| U164 openFolderMenu / listOptionsBtn / displayCategoriesBtn tooltips+roles | ✅ `mainwindow.ui:283-460` | ⚠️ folders menu only - `profile_bar.h:14` | ⚠️ · |
| U165 LCD counters ("Active:" activeMods/activePlugins QLCDNumber) | ✅ `mainwindow.ui:354`, `:880` | ⚠️ counters exist, "Active:" labels unproven - `mod_list_controller.cpp:648` | ⚠️ · |
| U232 Escape/Filter shortcut scope = WidgetWithChildren, autoRepeat off | ✅ `mainwindow.cpp:217-231` | ✅ both, one pair per pane - `main_window.cpp:376` | ✅ ✔ |
| U280 csvbuilder for exportModListCSV | ✅ `csvbuilder.cpp` | ❌ | ❌ · |

## 19. Mod Context Menu

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Visit on Nexus | ✅ `visitOnNexus` | ✅ `source_visit_info` (Nexus/LoversLab) - `mod_context_menu.cpp:398` | ✅ · |
| Visit web page | ✅ `visitWebPage` | ✅ source-aware context menu - `mod_context_menu.cpp:402` | ✅ · |
| Reinstall mod | ✅ `reinstallMod` | ❌ | ❌ · |
| Create backup | ✅ `createBackup` | ⚠️ deploy-level only, no user action - `deploy_utils.cpp:165` | ⚠️ · |
| Restore backup | ✅ `restoreBackup` | ⚠️ deploy-level only, no user action - `deploy_utils.cpp:195` | ⚠️ · |
| Restore hidden files | ✅ `restoreHiddenFiles` | ❌ | ❌ · |
| Mark as converted | ✅ `markConverted` | ❌ | ❌ · |
| Ignore missing data | ✅ `ignoreMissingData` | ✅ `mark_validated()` - `mod_context_menu.cpp:339` | ✅ · |
| Ignore update | ✅ `setIgnoreUpdate` | ❌ | ❌ · |
| Set color | ✅ `setColor`, `resetColor` | ✅ Set/Reset color - `notes_tab.h:27` | ✅ · |
| Open in Explorer | ✅ `openExplorer` | ✅ Ctrl+double-click; pseudo-rows correctly do nothing - `mod_list_controller.cpp:509` | ✅ ✔ |
| Create empty mod | ✅ `createEmptyMod` | ✅ `create_empty_mod()` - `mod_actions.h:65` | ✅ · |
| Create separator | ✅ `createSeparator` | ✅ `create_separator()` - `mod_actions.h:64` | ✅ · |
| Overwrite: create mod from overwrite | ✅ `createModFromOverwrite` | ✅ `create_mod_from_overwrite()` - `overwrite_controller.h:21` | ✅ · |
| Overwrite: move to existing mod | ✅ `moveOverwriteContentToExistingMod` | ✅ `move_overwrite_content_to_mod()` - `overwrite_controller.h:22` | ✅ · |
| Overwrite: clear | ✅ `clearOverwrite` | ✅ `clear_overwrite()` - `overwrite_controller.h:20` | ✅ · |
| Set categories (batch) | ✅ `setCategories`, `setPrimaryCategory` | ✅ `add_category_menus()` (checkable + radio) - `mod_context_menu.h:39` | ✅ · |
| Rename mod | ✅ `renameMod` | ✅ `rename_mod_inline()` - `mod_list_controller.h:63` | ✅ · |
| Remove mod | ✅ | ✅ `remove_selected_mods()` (folder to trash) - `mod_actions.cpp:158` | ✅ · |
| Root override toggle | ✅ | ✅ `toggle_root_override()` - `mod_list_controller.h:142` | ✅ · |
| U052 listOptionsBtn hosts ModListGlobalContextMenu | ✅ `mainwindow.cpp:374-376` | ❌ | ❌ · |
| U071 ModListGlobalContextMenu full tree (install/create above-below-inside, collapse, enable-matching, update, auto-categories, refresh, csv) | ✅ `modlistcontextmenu.cpp:33-102` | ⚠️ subset only; filter-aware labels + position entries unproven - `mod_context_menu.cpp:75` | ⚠️ · |
| U072 Type-dispatched context menus (Overwrite/Backup/Separator/Foreign/Regular trees) | ✅ `modlistcontextmenu.cpp:225-245` | ❌ no per-row-type menu variants | ❌ · |
| U073 "All Mods" submenu (global menu nested in row menu) | ✅ `modlistcontextmenu.cpp:230-234` | ❌ | ❌ · |
| U075 "Information..." default (bold) action, omitted for foreign | ✅ `modlistcontextmenu.cpp:247-255` | ⚠️ action exists, bolding unproven - `mod_context_menu.cpp:114` | ⚠️ · |
| U081 Separator row menu (rename/remove, color select/reset, send-to) | ✅ `modlistcontextmenu.cpp:422-447` | ❌ | ❌ · |
| U082 Foreign row menu (Send to... only - no Information) | ✅ `modlistcontextmenu.cpp:449-454` | ❌ | ❌ · |
| U083 Backup row menu (Restore/Remove Backup, Ignore missing, Mark converted, Visit blocks, Explorer) (NEXUS-LENS: genericize/provider-scope) | ✅ `modlistcontextmenu.cpp:456-497` | ❌ | ❌ · |
| U084 Regular row menu full order (versioning, force-check, endorse/track blocks, colors, visits) (NEXUS-LENS: genericize/provider-scope) | ✅ `modlistcontextmenu.cpp:499-637` | ❌ | ❌ · |
| U208 modid-less custom URL / installationFile / uploader fields (Visit on host) | ✅ `modinforegular.cpp` | ❌ | ❌ · |
| U230 Global context menu rebuilt on aboutToShow | ✅ `modlistcontextmenu.cpp:22-30` | ❌ | ❌ · |
| U231 Position-aware installMod (above/below/inside by sort) | ✅ `modlistcontextmenu.cpp:33-70` | ❌ | ❌ · |

## 20. Plugin Context Menu

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Set ESP lock (from context) | ✅ `setESPLock` | ✅ `lock_requested` signal - `plugin_context_menu.h:35` | ✅ · |
| Open origin explorer | ✅ `openOriginExplorer` | ❌ | ❌ · |
| Open origin information | ✅ `openOriginInformation` | ❌ | ❌ · |
| Enable/disable plugin | ✅ `PluginListContextMenu` | ❌ | ❌ · |
| Send-to priority | ✅ `sendToPriority` | ❌ | ❌ · |
| Lock/unlock plugin | ✅ `setESPLock` | ✅ Lock/Unlock load order - `plugin_context_menu.cpp:24` | ✅ · |
| U085 Plugin "Enable all"/"Disable all" + confirm dialog | ✅ `pluginlistcontextmenu.cpp:36-48` | ⚠️ bulk enable + confirm unproven - `plugin_context_menu.cpp:24` | ⚠️ · |
| U086 Lock/Unlock load-order labels conditional on lock state | ✅ `pluginlistcontextmenu.cpp:56-77` | ⚠️ labels not state-conditional - `plugin_context_menu.cpp:24` | ⚠️ · |
| U087 Plugin Send to... (Top/Bottom/Priority QInputDialog) | ✅ `pluginlistcontextmenu.cpp:111-133` | ❌ | ❌ · |
| U088 "Open Origin in Explorer" gated on origin resolving | ✅ `pluginlistcontextmenu.cpp:80-95` | ❌ | ❌ · |
| U089 "Open Origin Info..." default action (single non-foreign) | ✅ `pluginlistcontextmenu.cpp:96-107` | ❌ | ❌ · |
| U228 Lock conditionals consider only enabled plugins | ✅ `pluginlistcontextmenu.cpp:56-77` | ❌ | ❌ · |

## 21. Archive & Installation

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| FOMOD installer | ✅ `IPluginInstaller` | ✅ `FomodInstaller` plugin + engine - `fomod_view_model.h:51` | ✅ ✔ |
| FOMOD XML parsing | ✅ | ✅ `module_config` (GroupType, Dependency, PluginType) - `module_config.h` | ✅ ✔ |
| FOMOD condition tester | ✅ | ✅ `FomodConditionTester` - `condition_tester.h:36` | ✅ ✔ |
| FOMOD C# script detection | ✅ | ✅ `hasCSharpScript()` - `module_config.h:327` | ✅ ✔ |
| FOMOD view model (step nav) | ✅ | ✅ `FomodViewModel` (step fwd/back, flag map) - `fomod_view_model.h:51` | ✅ ✔ |
| FOMOD file installer | ✅ | ✅ `FomodFileInstaller::apply()` - `file_installer.h:32` | ✅ ✔ |
| Archive password support | ✅ `queryPassword` `installationmanager.cpp:126` | ✅ in-process for both readers; never on argv/env - `archive_extractor.h:25` | ✅ ✔ |
| Installation merge/replace | ✅ `merged`, `replaced` `installationmanager.h:40` | ✅ `OverwriteAction` 4 branches; port of `queryoverwritedialog.ui` - `install_stage.cpp:166` | ✅ ✔ |
| Backup on install | ✅ `keepBackupOnInstall` `settings.cpp:412` | ✅ taken, and the checkbox remembers the answer - `query_overwrite_dialog.cpp:99` | ✅ ✔ |
| Installation result tracking | ✅ `InstallationResult` `installationmanager.h:40` | ⚠️ only `replaced_archive` shipped; the rest are behaviour, not flags - `pipeline.h:128` | ⚠️ ✔ |
| Staging layout normalization | ❌ | 🚀 `analyze_staging_layout()` + `normalize_staging_root()` - `staging_layout.h:51` | 🚀 ✔ |
| BSA/BA2 archive listing | ❌ | 🚀 `DataArchive` (libarchive-backed) - `data_archive.h:25` | 🚀 ✔ |
| Install name dialog (smart candidates) | ❌ | 🚀 `InstallNameDialog` (editable combobox) - `install_name_dialog.h:18` | 🚀 ✔ |
| Install progress dialog (modeless) | ❌ | 🚀 `InstallProgressDialog` (300 ms show delay) - `install_progress_dialog.h:19` | 🚀 ✔ |
| IPluginInstaller::EInstallResult | ✅ `iplugininstaller.h:41-50` (7 values) | ❌ a competing-installer chain has no meaning over stage claims - Workspace-l5xa | ❌ ✔ |
| Archive file tree representation | ✅ `ArchiveFileTree`, `ArchiveFileEntry` | ✅ `ArchiveFileTree` - `engine/mod/filetree/archive_file_tree.h:19` | ✅ ✔ |
| U194 Installer error/progress strings (extraction failed, invalid name, no installer plugins, password prompt...) | ✅ `installationmanager.cpp:73-880` | ⚠️ most shipped; "no installer plugins" is unreachable - `downloads_controller.cpp:170` | ⚠️ ✔ |
| U195 7z error code strings (9 exact) | ✅ `installationmanager.cpp:886-913` (3 name `7z.dll`/COM) | ❌ keyed on MO2's in-process COM archiver; we use lib7zip reasons - `sevenzip_backend.cpp:604` - Workspace-U195 | ❌ ✔ |
| U282 VirtualFileTree/qdirfiletree/archivefiletree abstractions | ✅ `virtualfiletree.cpp`, `qdirfiletree.cpp`, `archivefiletree.cpp` | ⚠️ 2 of 3: `DirFileTree` + `ArchiveFileTree`, no memoized virtual tree - `filetree_test.cpp:3` | ⚠️ ✔ |

## 22. Deploy System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Symlink strategy | ❌ | 🚀 `SymlinkStrategy` (CI target resolution) - `strategy.h:10` | 🚀 · |
| Direct deploy strategy | ❌ | 🚀 `DirectDeployStrategy` (ledger + backup) - `strategy_direct.h:7` | 🚀 · |
| Hardlink strategy | ❌ | 🚫 never reachable, removed - the factory builds only overlayfs and symlink - `deploy/core.cpp:16` | 🚫 · |
| Junction strategy (Windows) | ❌ | 🚫 removed - Windows-shaped with no Linux subject - `deploy/core.cpp:16` | 🚫 · |
| OverlayFS deploy strategy | ❌ | 🚀 `OverlayFsDeploy` (O(1) reorder) - `overlay_fs_deploy.h:14` | 🚀 · |
| VFS deploy strategy (USVFS / OverlayFS) | ✅ `usvfsCreateVFS` | ⚠️ Linux `OverlayFsDeploy` - `overlay_fs_deploy.h:14`; Windows USVFS is a launcher stub, not written - `launcher.cpp:825` | ⚠️ · |
| Deploy ledger (incremental tracking) | ❌ | 🚀 `DeployLedger` (diff for priority changes) - `deploy_ledger.h:8` | 🚀 · |
| Parallel deploy (thread pool) | ❌ | 🚀 `deploy_all_enabled_mods_parallel()` - `deploy_utils.h:128` | 🚀 · |
| Root override ([General] rootOverride) | ❌ | 🚀 `RootOverride` + `classify_registry_path()` - `root_override.h:34` | 🚀 · |
| Case-insensitive deploy aliases | ❌ | 🚀 `add_case_insensitive_aliases()` - `deploy_utils.h:219` | 🚀 · |
| Deploy backup + restore | ❌ | 🚀 `remove_deployed_files()` restores originals - `deploy_utils.h:194` | 🚀 · |
| Binary detection (PE/ELF/SH) | ❌ | 🚀 `is_executable_binary()` - `deploy_utils.h:86` | 🚀 · |

## 23. Overwrite System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Move overwrite to mod | ✅ | ✅ `move_overwrite_to_mod()` - `overwrite_utils.cpp:391` | ✅ · |
| Sync overwrite file | ✅ | ✅ `sync_overwrite_file()` - `overwrite_utils.h:73` | ✅ · |
| Clear overwrite (trash) | ✅ | ✅ `clear_overwrite()` - `overwrite_controller.h:20` | ✅ · |
| CI directory merge (overlay captures) | ❌ | 🚀 `normalize_overwrite_casing()` - `launch_controller.cpp:1194` | 🚀 · |
| Overwrite sync plan (apply/preview) | ❌ | 🚀 `apply_sync_plan()` - `overwrite_utils.h:124` | 🚀 · |
| Overwrite info dialog (file browser) | ✅ `OverwriteInfoDialog` | ✅ `OverwriteInfoDialog` (QFileSystemModel, context menu) - `overwrite_info_dialog.h:20` | ✅ · |
| Query overwrite dialog (merge/replace) | ✅ `QueryOverwriteDialog` | ✅ thread-safe `ask_overwrite()` - `query_overwrite_dialog.h:20` | ✅ · |
| Sync overwrite dialog (selective) | ✅ `SyncOverwriteDialog` | ✅ per-file combo, game-origin - `sync_overwrite_dialog.h:23` | ✅ · |
| Move to mod dialog | ❌ | 🚀 `MoveToModDialog` (destination picker) - `move_to_mod_dialog.h:16` | 🚀 · |
| U080 Overwrite row menu (Sync to Mods w/ count guard, Create/Move/Clear, Open in Explorer) | ✅ `modlistcontextmenu.cpp:402-420` | ⚠️ move-to-Mod picker only, Sync guard unproven - `move_to_mod_dialog.h:16` | ⚠️ · |
| U175 QueryOverwriteDialog (Keep Backup / Merge / Replace / Rename / Cancel) | ✅ `queryoverwritedialog.ui` | ✅ all five controls, Rename default - `query_overwrite_dialog.cpp:57` | ✅ ✔ |
| U176 SyncOverwriteDialog columns (Name / Sync To per-file combo) | ✅ `syncoverwritedialog.ui` | ✅ Name + Sync-To combo columns - `sync_overwrite_dialog.cpp:27` | ✅ · |
| U177 OverwriteInfoDialog (Open in Explorer + drag&drop hint) | ✅ `overwriteinfodialog.ui` | ⚠️ port exists, Explorer button + hint unproven - `overwrite_info_dialog.h:14` | ⚠️ · |

## 24. Save Game System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Save game model | ✅ | ✅ `SaveGame` - `save_game.h:49` | ✅ · |
| Skyrim SE/LE save parsing | ✅ | ✅ parsers registered - `SkyrimSESaveParser.cpp:150` | ✅ · |
| Save scanning | ✅ | ✅ `scan_saves()` + save_extensions hook - `save_scanner.h:38` | ✅ · |
| Save missing assets resolver | ✅ | ✅ `find_save_missing_assets()` - `save_missing_assets.h:26` | ✅ · |
| Local saves | ✅ | ✅ `local_saves` - `local_saves.h:51` | ✅ · |
| Pluggable save parser (per-game) | ❌ | 🚀 `SaveParserRegistry` - `save_parser_registry.h:45` | 🚀 · |
| Script extender file detection | ❌ | 🚀 `has_script_extender_file()` - `save_game.h:100` | 🚀 · |
| Save screenshot extraction (RGBA) | ❌ | 🚀 `SaveGame::screenshot` - `save_game.h:69` | 🚀 · |
| Save game list (QTreeWidget) | ✅ `SavesTab` | ✅ QTableWidget, not a tree - `saves_tab.h:143` | ✅ · |
| Save game hover info popup | ✅ `GamebryoSaveGameInfoWidget` | ✅ hover info popup - `saves_tab.h:159` | ✅ · |
| Save game background scan | ✅ | ✅ `SavesScanWorker` - `saves_scan_worker.h:52` | ✅ · |
| Save game delete | ✅ `SavesTab::deleteSavegame()` | ✅ `on_delete_key()` - `saves_tab.h:131` | ✅ · |
| Save game context menu | ✅ `SavesTab::onContextMenu()` | ✅ context menu - `saves_tab.h:133` | ✅ · |
| Save game open in explorer | ✅ | ⚠️ `open_explorer` is for mod files only - `mod_list_controller.cpp:2589` | ⚠️ · |
| Save game fix missing assets | ✅ `SavesTab::fixMods()` | ⚠️ detection + display, no fix action - `save_missing_assets.h:26` | ⚠️ · |
| Transfer saves dialog | ✅ `TransferSavesDialog` | ❌ | ❌ · |
| Save game streaming (per-save entryReady, binary-insert sorted list) | ❌ | 🚀 `entryReady` + `on_entry_ready` - `saves_scan_worker.h:70` | 🚀 · |
| Save Information dialog (2-column details, thumbnail, plugin list) | ✅ `GamebryoSaveGameInfoWidget` | ✅ `SaveInfoDialog` (thumbnail, load-order status) - `save_info_dialog.h:30` | ✅ · |
| Parallel save scan (parallel parse + provider indexing) | ❌ | 🚀 `parallel::for_each` + double-fire fix - `saves_scan_worker.h:52` | 🚀 · |
| Disabled-but-present plugins excluded from missing assets | ✅ | ✅ enabled OR force_loaded check - `save_missing_assets.h:26` | ✅ · |
| U105 Save hover widget fields (SE data, missing ESP/ESH/ESL lists + N more) | ✅ `gamebryosavegameinfowidget.cpp:78-161` | ⚠️ hover info exists, exact field list unproven - `saves_tab.h:27` | ⚠️ · |
| U115 Saves list columns (display name "%1, #%2, Level %3, %4" + relative path) | ✅ `savestab.cpp:180-196` | ✅ format built in `display_name()` - `save_game.cpp:23`; File column is the path relative to the saves dir | ✅ · |
| U159 Saves context menu (Fix enabled mods gating, Delete %n save(s), Open in Explorer) | ✅ `savestab.cpp:244-280` | ⚠️ fix path exists, gating/plural-delete unproven - `save_missing_assets.h:26` | ⚠️ · |
| U160 Save delete confirm (first 10 names + recycle-bin note) | ✅ `savestab.cpp:205-235` | ✅ all three parts, Delete key + context menu - `saves_tab.cpp:515` | ✅ · |
| U199 Save parsing error strings (open failed, wrong format) | ✅ `gamebryosavegame.cpp:102-112` | ❌ | ❌ · |
| U227 Save list sorted by creation desc + streaming adds + relative path column | ✅ `savestab.cpp:180` | ✅ sort `save_scanner.cpp:104`, streaming binary insert `saves_tab.cpp:296` | ✅ · |
| U239 SaveGameInfo feature (getMissingAssets used by saves Fix) | ✅ `savestab.cpp:244` | ✅ `SaveParserRegistry::parse_save` - `saves_tab.cpp:329` | ✅ · |

## 25. Game Detection & Knowledge

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Game detection (Steam library) | ✅ | ✅ `detect_steam_games()` - `game_detector.h:24` | ✅ · |
| Per-game knowledge (key-value) | ✅ | ✅ `GameKnowledge` - `game_knowledge.h:20` | ✅ · |
| Game capabilities (tab display) | ❌ | 🚀 `GameCapabilities` + `visible_tabs_for` - `game_capabilities.h:31` | 🚀 · |
| Game feature registry (MO2 IGameFeatures port) | ❌ | 🚀 `GameFeatureRegistry` (priority + replace) - `game_feature_registry.h:110` | 🚀 · |
| ModDataChecker feature | ❌ | 🚀 `ModDataContentFeature` - `game_feature.h:112` | 🚀 · |
| ScriptExtender feature | ❌ | 🚀 `ScriptExtenderFeature` - `game_feature.h:205` | 🚀 · |
| DataArchives feature | ❌ | 🚀 `DataArchivesFeature` - `game_feature.h:186` | 🚀 · |
| AnimationParser feature | ❌ | 🚀 `AnimationParserFeature` (frames, layers, RGBA) - `game_feature.h:337` | 🚀 · |
| UnmanagedMods feature (DLC/CC) | ❌ | 🚀 `UnmanagedModsFeature` - `game_feature.h:271` | 🚀 · |
| BSAInvalidation feature | ❌ | 🚀 `BSAInvalidationFeature` - `game_feature.h:288` | 🚀 · |
| Game icons (download-on-demand) | ❌ | 🚀 `GameIconCache` (async, placeholders) - `game_icon_cache.h:38` | 🚀 · |
| Multi-game detection | ❌ | 🚀 `detect_steam_games_multi()` - `game_detector.h:29` | 🚀 · |
| VDF/ACF parsing | ❌ | 🚀 `parse_library_folders()` + `parse_acf_value()` - `game_detector.h:34` | 🚀 · |
| U060 espTab/bsaTab removed when game lacks the feature | ✅ `mainwindow.cpp:340-348` | ⚠️ registry exists, per-tab removal unproven - `game_feature_registry.h:110` | ⚠️ · |
| U240 IGameFeatures gating tab visibility (GamePlugins/DataArchives/SaveGameInfo/ModDataContent) | ✅ `mainwindow.cpp:340-348` | ⚠️ registry exists, tab-gating unproven - `game_feature_registry.h:110` | ⚠️ · |

## 26. Pipeline System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Pipeline with ordered stages | ❌ | 🚀 `Pipeline` + `PipelineContext` - `pipeline.h:178` | 🚀 · |
| Fetch stage (download) | ❌ | 🚀 `FetchStage` - `fetch_stage.h:9` | 🚀 · |
| Extract stage (archive) | ❌ | 🚀 `ExtractStage` (low_priority) - `extract_stage.h:10` | 🚀 · |
| FOMOD stage (wizard) | ❌ | 🚀 `FomodStage` - `fomod_stage.h:15` | 🚀 · |
| Install stage (deploy) | ❌ | 🚀 `InstallStage` - `install_stage.h:26` | 🚀 · |
| Deploy stage (symlink/overlay) | ❌ | 🚀 `DeployStage` - `deploy_stage.h:9` | 🚀 · |
| Resolve stage (path resolution) | ❌ | 🚀 `ResolveStage` - `resolve_stage.h:7` | 🚀 · |
| Sync stage (overwrite) | ❌ | 🚀 `SyncStage` - `sync_stage.h:13` | 🚀 · |
| Launch stage (game execution) | ❌ | 🚀 `LaunchStage` - `launch_stage.h:7` | 🚀 · |
| Plugin claim stage | ❌ | 🚀 `PluginClaimStage` - `plugin_claim_stage.h:12` | 🚀 · |
| Stage registry + hook registry | ❌ | 🚀 `StageRegistry` + `HookRegistry` - `stage_registry.h:24` | 🚀 · |
| Overwrite decision (Merge/Replace/Rename/Cancel) | ❌ | 🚀 `OverwriteAction` enum - `pipeline.h:23` | 🚀 · |
| FOMOD decision (accept/manual/choices_json) | ❌ | 🚀 `FomodDecision` - `pipeline.h:40` | 🚀 · |
| Trace recorder (pipeline workflow) | ❌ | 🚀 `TraceRecorder` (flow_id, stages, durations) - `trace_recorder.h:37` | 🚀 · |
| Pipeline visualization (2D canvas) | ❌ | 🚀 `PipelineContentWidget` (stage cards, arrows) - `pipeline_content_widget.h:31` | 🚀 · |
| Pipeline worker (background) | ❌ | 🚀 `PipelineWorker` - `pipeline_worker.h:76` | 🚀 · |

## 27. Plugin Host System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| C ABI plugin loading (dlopen) | ❌ | 🚀 `PluginLoader` (load_plugin, load_directory) - `plugin_loader.h:206` | 🚀 · |
| v2 ABI registration | ❌ | 🚀 `gmm_register_v2()` - `plugin_loader.cpp:1952` | 🚀 · |
| Python plugin loader | ❌ | 🚀 `PythonLoader` - `python_loader.h:15` | 🚀 · |
| Tool registry (IPluginTool) | ❌ | 🚀 `ToolRegistry` (tool_id, kind, fn) - `tool_registry.h:38` | 🚀 · |
| Diagnostics registry | ❌ | 🚀 `DiagnosticsRegistry` + `DiagnoseRegistry` - `diagnostics_registry.h:21` | 🚀 · |
| Deploy strategy registry | ❌ | 🚀 `DeployStrategyRegistry` (deploy/remove) - `deploy_strategy_registry.h:24` | 🚀 · |
| Hook registry (behavior injection) | ❌ | 🚀 `HookRegistry` (tag-based, priority-ordered) - `hook_registry.h:24` | 🚀 · |
| Save parser registry | ❌ | 🚀 `SaveParserRegistry` - `save_parser_registry.h:45` | 🚀 · |
| File mapper registry | ❌ | 🚀 `FileMapperRegistry` - `file_mapper_registry.h:25` | 🚀 · |
| Order encoding registry | ❌ | 🚀 `OrderEncodingRegistry` - `order_encoding_registry.h:25` | 🚀 · |
| Requirements registry | ❌ | 🚀 `RequirementsRegistry` - `requirements_registry.h:42` | 🚀 · |
| Plugin settings registry | ❌ | 🚀 `PluginSettingsRegistry` - `plugin_settings_registry.h:43` | 🚀 · |
| U144 Plugin disable warnings (game-required + dependent plugins) | ✅ `settingsdialogplugins.cpp:232-270` | ❌ | ❌ · |
| U244 OrganizerProxy/plugin dependency-resolution dialog | ✅ `plugincontainer.cpp`, `settingsdialogplugins.cpp` | ❌ | ❌ · |
| U274 PluginRequirements (uibase pluginrequirements.cpp) | ✅ `pluginrequirements.cpp` (uibase) | ⚠️ registry wired in, uibase API parity unproven - `plugin_loader.cpp:23` | ⚠️ · |
| U283 PluginListProxy / OrganizerProxy plugin proxies | ✅ `pluginlistproxy.cpp`, `organizerproxy.cpp` | ❌ | ❌ · |

## 28. Sort System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Sort provider / registry | ❌ | 🚀 `Sorter::Interface` + `Sorter::Registry` - `interface.h:28` | 🚀 · |
| C ABI sort provider | ❌ | 🚀 `Sorter::Abi` - `abi.h:18` | 🚀 · |
| LOOT sorter | ❌ | 🚀 `Sorter::Loot` (run_sort with progress) - `loot/sorter.h:66` | 🚀 · |
| Masterlist manager | ❌ | 🚀 `MasterlistManager` (24h TTL) - `masterlists.h:21` | 🚀 · |
| Game-native sort round-trip (Isaac and other workshop games) | ❌ (MO2 has no game-native mod band) | 🚀 writes the game's own mods dir; phantom rows skipped - `mod_list_controller.cpp:1102` | 🚀 ✔ |

## 29. Instance Management

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Instance manager | ✅ `InstanceManager` | ✅ `Instance` + `instance_utils` free functions, no manager object - `instance.h:39` | ✅ · |
| Create instance dialog | ✅ `CreateInstanceDialog` | ✅ `GameSelectionWidget` (cards + filter) - `game_selection_widget.h:22` | ✅ · |
| Instance switcher | ✅ | ✅ `InstanceSwitcherDialog` - `instance_switcher_dialog.h:19` | ✅ · |
| Instance TOML persistence | ❌ | 🚀 `parse_instance_toml()` + JSON-to-TOML repair - `toml_utils.h:18` | 🚀 · |
| Instance scan + last-used | ❌ | 🚀 `scan_instances()` + `read/write_last_instance()` - `instance_utils.h:28` | 🚀 · |
| Game icons (download-on-demand) | ❌ | 🚀 `GameIcons` (ensure_icon_cached) - `game_icons.h:30` | 🚀 · |
| Masterlist fetch (GitHub cache) | ❌ | 🚀 `ensure_masterlist_cached()` free functions, prefetched off-thread at startup - `masterlist_fetch.h:30`; the LOOT masterlist+prelude pair is a separate `MasterlistManager` - `sort/sorter/loot/masterlists.h:21` | 🚀 · |
| Instance statistics dialog | ❌ | 🚀 `StatsContentWidget` (sizes + explorer) - `stats_content_widget.h:17` | 🚀 · |
| Instance options panel | ❌ | 🚀 `instance_options_panel` - `instance_options_panel.h:28` | 🚀 · |
| Create instance wizard (7-page) | ✅ `CreateInstanceDialog` (Intro, Type, Game, Variants, Name, Paths, Profiles, Nexus, Confirmation) | ❌ | ❌ · |
| Game variant selection | ✅ `CreateInstanceDialog` (game variants) | ❌ | ❌ · |
| Microsoft Store game handling | ✅ `InstanceManager` | ❌ | ❌ · |
| U025 Manage Instances action hidden when change not allowed | ✅ `mainwindow.cpp:741-743` | ❌ | ❌ · |
| U168 Instance manager dialog controls (create/explore/rename/delete/switch, filter, wiki link) | ✅ `instancemanagerdialog.ui` | ⚠️ switcher dialog only, full set unproven - `instance_switcher_dialog.h:19` | ⚠️ · |
| U169 Instance manager flows (validation errors, switching TaskDialog, delete guards) | ✅ `instancemanagerdialog.cpp:112-563` | ❌ | ❌ · |
| U179 CreateInstanceDialog page copy (intro/type/game/edition/name/profile/paths) | ✅ `createinstancedialog.ui` | ❌ | ❌ · |
| U286 createinstancedialogpages per-page validation logic | ✅ `createinstancedialogpages.cpp` | ❌ | ❌ · |

## 30. UI Layer

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Mod list view | ✅ | ✅ `mod_table_view` - `mod_table_view.h:157` | ✅ · |
| Plugin list view | ✅ | ✅ `plugins_tab` - `plugins_tab.h:23` | ✅ · |
| Mod info dialog | ✅ `ModInfoDialog` | ✅ `mod_info_dialog` (15 tabs) - `mod_info_dialog.h:25` | ✅ · |
| Settings dialog | ✅ `SettingsDialog` | ✅ `settings_content_widget` - `settings_content_widget.h:29` | ✅ · |
| Toolbar | ✅ | ✅ `main_toolbar` - `main_toolbar.h:12` | ✅ · |
| Game lock overlay | ✅ `UILocker` | 🚀 `game_lock_overlay` - `launch_controller.h:123` | 🚀 · |
| Process Tree View | ❌ | 🚀 `process_tree_checkbox` - `launch_controller.cpp:1598` | 🚀 · |
| FOMOD wizard UI | ✅ | ✅ `fomod_wizard_dialog` - `fomod_wizard_dialog.h:37` | ✅ · |
| FOMOD image viewer | ✅ | ✅ `fomod_image_viewer` - `fomod_image_viewer.h:14` | ✅ · |
| Profile bar (combo + folders) | ❌ | 🚀 `ProfileBar` (12 FolderKind, export/import) - `profile_bar.h:28` | 🚀 · |
| Profile manager dialog | ✅ | ✅ `profile_manager_dialog` - `profile_manager_dialog.h:22` | ✅ · |
| Profile settings widget | ❌ | 🚀 `profile_settings_widget` - `profile_settings_widget.h:18` | 🚀 · |
| Console panel | ❌ | 🚀 `console_panel` - `console_panel.h:11` | 🚀 · |
| Debug window (Konami code) | ❌ | 🚀 `debug_window` - `debug_window.h:42` | 🚀 · |
| Preview system (images/text/video) | ✅ | ✅ `preview_registry` + `preview_widget` - `preview_registry.h:43` | ✅ · |
| File viewer (image, video, 3D scene) | ❌ | 🚀 `ImageViewer` / `VideoViewer` / `SceneViewer` - `image_viewer.h:23` | 🚀 · |
| Plugin-provided preview | ✅ | ✅ `preview_window` (v2 IPluginPreview) - `preview_window.h:61` | ✅ · |
| ANM2 animation playback | ❌ | 🚀 `preview_window` (frame-based timer) - `preview_window.h:61` | 🚀 · |
| Variant browsing (prev/next) | ❌ | 🚀 `preview_window` multi-provider - `preview_window.h:61` | 🚀 · |
| Zoom/fit controls | ❌ | 🚀 `zoom_by` / `set_fit` - `preview_window.h:106` | 🚀 · |
| Smooth scroll | ❌ | 🚀 `SmoothScroller` - `smooth_scroll.h:19` | 🚀 · |
| Zoom controls | ❌ | 🚀 `ZoomableView` - `zoom_controls.h:18` | 🚀 · |
| Column toggle header | ❌ | 🚀 `column_toggle_header` - `column_toggle_header.h:7` | 🚀 · |
| Game path banner | ❌ | 🚀 `game_path_banner` - `game_path_banner.h:12` | 🚀 · |
| Status bar (custom) | ✅ `StatusBar` | 🚀 context label, transient status, pipeline button, per-source meters - `status_bar.h:23` | 🚀 ✔ |
| Notification backend | ❌ | 🚀 `notification_backend` - `notification_backend.h:8` | 🚀 · |
| BBCode parser (Nexus descriptions) | ✅ | ✅ `bbcode_to_html` - `bbcode.h:22` | ✅ · |
| Menu bar (File/Edit/View/Tools/Help) | ✅ | ✅ `AppMenuBar` (dynamic per-game tools) - `menu_bar.h:24` | ✅ · |
| MO2's eight keyboard binds (Ctrl+M, Ctrl+P, Ctrl+E, Ctrl+I, Ctrl+S, Ctrl+N, Ctrl+H, F5) | ✅ `mainwindow.ui` | ✅ all eight bound - `menu_bar.cpp:47`, `:193` | ✅ ✔ |
| What's This / context help on the main-window surfaces | ✅ 26 `whatsThis` properties in `mainwindow.ui` (MO2 never calls `setWhatsThis()` in C++) | ⚠️ 19 call sites, 17 files, all on containers so children inherit - `main_window.cpp` | ⚠️ ✔ |
| Data tab (virtual data browser) | ✅ `DataTab` | ✅ `data_tab` (dual view, build worker, menu) - `data_tab.h:25` | ✅ · |
| File-tree modifier swap (Alt swaps preview/open, Ctrl reveals) | ✅ `filetree.cpp:249`, `modinfodialogconflicts.cpp:203` | ✅ pure `resolve_double_click(setting, alt, ctrl)` on all 3 trees - `data_tab.h:77` | ✅ ✔ |
| Downloads tab | ✅ `DownloadsTab` | ✅ `downloads_tab` (drag-drop, watcher, compact) - `downloads_tab.h:46` | ✅ · |
| Saves tab | ✅ `SavesTab` | ✅ `saves_tab` (background scan, hover info) - `saves_tab.h:27` | ✅ · |
| Conflicts tab | ✅ | ✅ `conflicts_tab` (image diff) - `conflicts_tab.h:22` | ✅ · |
| Archives tab | ✅ | ✅ `archives_tab` - `archives_tab.h:9` | ✅ · |
| Right panel tab system | ✅ | ✅ `right_panel` + `tab_panels` - `right_panel.h:25` | ✅ · |
| Main tab container (Full UI mode) | ❌ | 🚀 `MainTabContainer` - `main_tab_container.h:18` | 🚀 · |
| Desktop shortcut management | ✅ `env::Shortcut` (IShellLink COM) | 🚫 COM shell link, no Linux subject | 🚫 · [win] |
| Shell context menu integration | ✅ `env::ShellMenu` (IContextMenu COM) | 🚫 COM context menu, no Linux subject | 🚫 · [win] |
| U001 Menu bar structure (MO2: File/View/Tools/Run/Help, no Edit menu); GMM ruling = generic "visit modding sites" action (site list knowledge/plugin-driven, NOT Nexus-tied) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.ui:1580-1596` | ⚠️ Run, Endorse and the generic visit action absent; Edit is ours - `menu_bar.cpp:29` | ⚠️ ✔ |
| U002 Ctrl+M = Install Mod... | ✅ `mainwindow.ui:1655-1675` | ✅ Ctrl+Shift+I and Ctrl+M - `menu_bar.cpp:46` | ✅ ✔ |
| U008 Help system (Ctrl+H dropdown); ruling = expansive thorough Help system - menu tree + docs content project | ✅ `mainwindow.ui:1838-1858` | ⚠️ binding + tree exist, docs content not started - `menu_bar.cpp:331` | ⚠️ ✔ |
| U009 F5 = Refresh | ✅ `mainwindow.ui:1989` | ✅ QKeySequence::Refresh - `menu_bar.cpp:193` | ✅ ✔ |
| U012 Help menu tree (Help on UI, Documentation, Wiki, Discord, Report Issue, Tutorials submenu, About) - see U008 ruling | ✅ `mainwindow.cpp:1096-1162` (flat) | ⚠️ 6 of 7; Discord absent, 4 fold into More, Tutorials gated - `menu_bar.cpp:331` | ⚠️ ✔ |
| U017 Toolbar right-align spacer before last separator | ✅ `mainwindow.cpp:713-744` | ❌ no right-align spacer | ❌ · |
| U018 Toolbar menu-buttons use QToolButton::InstantPopup | ✅ `mainwindow.cpp:746-753` | ❌ | ❌ · |
| U019 View > Toolbars submenu (9 checkables: menu/toolbar/statusbar, 3 icon sizes, 3 style modes) | ✅ `mainwindow.ui:1558-1578` | ❌ | ❌ · |
| U021 Popup menu on toolbar/central-widget edges (Toolbars + View Log) | ✅ `mainwindow.cpp:821-840` | ❌ | ❌ · |
| U023 Open Folder menu (12 entries: game/MyGames/INIs, instance/mods/profile/downloads, install/plugins/stylesheets/logs) | ✅ `mainwindow.cpp:2663-2693` | ⚠️ 11 FolderKinds handled, menu layout unproven - `mod_list_controller.cpp:3544` | ⚠️ · |
| U031 Game Support Wiki first-run info dialog | ✅ `mainwindow.cpp:1268-1278` | ❌ | ❌ · |
| U041 Right-click central widget edges shows popup menu | ✅ `mainwindow.cpp:906-924` | ❌ | ❌ · |
| U048 Qt effects disabled at startup (menu/combo/tooltip animations) | ✅ `mainwindow.cpp:240-252` | ❌ | ❌ · |
| U070 StatusBar carries Nexus API stats + user account (requestsChanged/credentialsReceived) (NEXUS-LENS: genericize/provider-scope) | ✅ `mainwindow.cpp:270-276`; one `m_api` label + tooltip | ⚠️ per-source metered budgets, no account half - `status_bar.h:30` | ⚠️ ✔ |
| U100 Status bar "%1 - %2 - %3" game/instance/profile message | ✅ `statusbar.cpp:144-162` | ✅ `context_label_text()`, transient text restores it - `status_bar.h:19` | ✅ ✔ |
| U101 Status bar progress ("Loading...", 0-100, max width 300, spacers) | ✅ `statusbar.cpp:22-38`, `:63-80` | ❌ MO2 only drives it with `setProgress(int)`; we ship no QProgressBar | ❌ ✔ |
| U102 Status bar visibility compensates central-widget bottom margin | ✅ `statusbar.cpp:175-192` | ❌ | ❌ · |
| U103 StatusBarAction icon+label wrapper | ✅ `statusbar.cpp:195+` | ❌ | ❌ · |
| U187 PreviewDialog (Preview / Close buttons) | ✅ `previewdialog.ui` | ⚠️ window exists, modal dialog parity unproven - `preview_window.h:61` | ⚠️ · |
| U219 Menu aboutToShow refresh pattern (lazy rebuild) + wheel-block combo | ✅ `mainwindow.cpp:455-474` | ❌ | ❌ · |
| U224 languageChange rebuilds help menu + resetActionIcons | ✅ `mainwindow.cpp:564-600` | ❌ | ❌ · |
| U241 IPreviewPlugin gating Preview menu | ✅ `filetree.cpp:741` | ⚠️ registry exists, Preview-menu gating unproven - `preview_registry.h:14` | ⚠️ · |
| U264 SortableTreeWidget + setCustomizableColumns | ✅ `sortabletreewidget.cpp` (uibase) | ❌ | ❌ · |
| U265 ExpanderWidget / LinkLabel / LineEditClear UI primitives | ✅ `uibase src` | ❌ | ❌ · |
| U269 EventFilter generic event filter (uibase) | ✅ `eventfilter.cpp` (uibase) | ❌ | ❌ · |
| U295 statusbar visibilityChanged margin compensation (see U102) | ✅ `statusbar.cpp` | ❌ | ❌ · |

## 31. Log System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Log model (QAbstractItemModel) | ✅ `LogModel` | ✅ `Logger` (callback-based, replay buffer) - `logger.h:20` | ✅ · |
| Log copy to clipboard | ✅ `LogList::copyToClipboard()` | ✅ QShortcut Copy on output_ - `console_panel.cpp:37` | ✅ · |
| Log open logs folder | ✅ `LogList::openLogsFolder()` | ✅ menu item + profile-bar item, both to the logger's own dir - `console_panel.cpp:164` | ✅ · |
| Log clear | ✅ `LogList::clear()` | ✅ `ConsolePanel::clear()` - `console_panel.h:18` | ✅ · |
| Log highlighter | ✅ `LogHighlighter` | ❌ | ❌ · |
| Log level filtering | ✅ | ✅ `Logger::set_level()` - `logger.h:27` | ✅ · |
| Group logging (begin/end) | ❌ | 🚀 `begin_group()` / `end_group()` - `logger.h:31` | 🚀 · |
| Replay buffer (256 entries) | ❌ | 🚀 late subscriber replay - `logger.cpp:50` | 🚀 · |
| Fork-safe append | ❌ | 🚀 `Logger::raw_append()` - `logger.h:47` | 🚀 · |
| Log initialization (spdlog, UTC timestamps, pattern) | ✅ `initLogging()` | ⚠️ localtime not UTC, no spdlog/pattern - `logger.cpp:151` | ⚠️ · |
| Log to stdout (via Console attach) | ✅ `logToStdout()` | ✅ fprintf stdout - `logger.cpp:122` | ✅ · |
| U020 View > Log checkable action toggles log dock | ✅ `mainwindow.cpp:816-819` | ✅ Show Console toggle - `menu_bar.cpp:122` | ✅ · |
| U057 errorReported() scans newest log first 50000 lines for ERROR | ✅ `mainwindow.cpp:993-1022` | ❌ | ❌ · |
| U157 Log list context menu (Copy/Copy all/Clear all/Open folder/Level submenu) | ✅ `loglist.cpp:250-290` | ✅ all five, level re-renders from the replay buffer - `console_panel.cpp:171` | ✅ · |
| U158 Log file creation failure critical dialog | ✅ `loglist.cpp:384-385` | ❌ | ❌ · |
| U294 logDock QDockWidget (area 8 bottom, View>Log toggle) | ✅ `mainwindow.ui:1510` | ⚠️ toggle exists, QDockWidget bottom area unproven - `menu_bar.cpp:122` | ⚠️ · |

## 32. System Tray

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| System tray icon | ✅ `SystemTrayManager` | ✅ shown at startup, context menu Show/Quit - `system_tray_manager.cpp:14` | ✅ ✔ |
| Minimize to system tray | ✅ `minimizeToSystemTray()` (per-executable, hides while the exe runs) | ⚠️ global setting hides on close, no per-executable hide-on-launch - `main_window.cpp:578` | ⚠️ ✔ |
| Restore from tray | ✅ `restoreFromSystemTray()` | ✅ tray click, Show menu and second launch all restore - `main_window.cpp:572` | ✅ ✔ |
| Tray notification | ✅ `showNotification()` (download complete/fail) | ✅ same two events, status bar kept - `main_window.cpp:457` | ✅ ✔ |
| U222 Finished-run while hidden -> restoreFromSystemTray | ✅ `mainwindow.cpp:488-492` | ✅ `kGameFinished` restores if hidden - `main_window.cpp:258` | ✅ ✔ |

MO2 has no close-to-tray: its `closeEvent` always quits, and the flag is a
per-Executable bit hiding the window for the duration of a game run. The GMM
toggle is therefore global and opt-in, and it yields to a missing tray so the
app can never end up unreachable - see `tray_decision.h:66`.

## 33. Self Updater

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Self-update system (GitHub releases) | ✅ `SelfUpdater` | ✅ per-platform updaters (Win/macOS/Linux/Flatpak/AUR/AppImage) - `self_updater.h:30` | ✅ · |
| Update candidates (version-sorted) | ✅ `CandidatesMap` | ❌ | ❌ · |
| Update backup + restart | ✅ `installUpdate()` + `restart()` | ⚠️ restart exists, no backup step - `self_updater.h:43` | ⚠️ · |
| Check for updates setting | ✅ | ✅ `check_for_updates()` - `settings.h:94` | ✅ · |
| Update download with progress dialog | ✅ `showProgress()`, QProgressDialog | ⚠️ `progress_cb` exists, no QProgressDialog - `self_updater.h:40` | ⚠️ · |
| Offline mode check before update | ✅ `testForUpdate()` respects offline mode | ❌ offline_mode not consulted by SelfUpdater | ❌ · |
| U027 Update action disabled-by-default + tooltip flip | ✅ `mainwindow.ui:1799-1819` | ❌ | ❌ · |
| U131 General > Updates group (Check for updates + Update to beta versions) | ✅ `settingsdialog.ui:199-232` | ⚠️ check-for-updates only, beta toggle unproven - `settings_content_widget.cpp:111` | ⚠️ · |
| U185 UpdateDialog (Changelog web view, Install/Cancel, version label) | ✅ `updatedialog.ui` | ❌ | ❌ · |

## 34. Multi-Process / IPC

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Multi-process guard (shared memory) | ✅ `MOMultiProcess` (QSharedMemory + QLocalServer) | ✅ QLockFile + QLocalServer - `multi_process.h:9` | ✅ · |
| Ephemeral process (forward download) | ✅ `MOMultiProcess::ephemeral()` | ❌ | ❌ · |
| Secondary instance (allow multiple) | ✅ `MOMultiProcess::secondary()` | ❌ | ❌ · |
| Message passing between instances | ✅ `sendMessage()` / `messageSent()` | ✅ `nxm_ipc` + generic URL forwarder - `nxm_ipc.h:44` | ✅ · |
| Command-line global options (--pick, --multiple, --logs, -i, -p) | ✅ `CommandLine` global options | ❌ | ❌ · |
| Forward to primary instance | ✅ `CommandLine::forwardToPrimary()` | ⚠️ URLs only, no general CLI-arg forward - `app/core.cpp:495` | ⚠️ · |
| NXM/moshortcut:// link protocol parsing | ✅ `CommandLine` handles moshortcut:// and nxm:// | ⚠️ nxm:// + modl://, moshortcut:// absent - `command_line.cpp:30` | ⚠️ · |

## 35. Text Editor

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Text editor (line numbers, syntax, word wrap) | ✅ `TextEditor` | ✅ line numbers, KSyntaxHighlighting, edit+save - `generic_files_tab.cpp:49` | ✅ · |
| HTML editor | ✅ `HTMLEditor` | ❌ QWebEngineView is a read-only viewer - `webview_description_renderer.h:5` | ❌ · |
| U190 TextViewer (multi-tab, per-file writable, Find, Save-per-page, save prompt) | ✅ `textviewer.cpp:60-276` | ⚠️ multi-file tab + write warning + Find - `generic_files_tab.cpp:49` | ⚠️ · |
| U191 TextViewer read-only INI write TaskDialog (Clear flag / Allow once / Skip) | ✅ `textviewer.cpp:173-192` | ❌ no read-only surface to gate: the only text editor lists files under the mod folder, all writable - `generic_files_tab.cpp:121` | ❌ · |
| U192 FindDialog (find-only, Find Next, Close) | ✅ `finddialog.ui` | ✅ pattern + Match case + Find Next + Close, wraps - `find_dialog.h:22` | ✅ · |
| U248 TextEditor toolbar per-file (Save, Word wrap toggle, Open in Explorer) + dirty flag | ✅ `texteditor.cpp:468-491` | ⚠️ save only, wrap/Explorer unproven - `generic_files_tab.cpp:63` | ⚠️ · |
| U249 Line-number gutter + current-line highlight + TextEditorHighlighter | ✅ `texteditor.cpp:13-14` | ⚠️ highlighter + gutter, current-line unproven - `generic_files_tab.cpp:59` | ⚠️ · |
| U250 TextViewer multi-file tabs w/ per-tab Save + Find | ✅ `textviewer.cpp` | ❌ | ❌ · |

## 36. Browser

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Integrated browser (QWebEngineView) | ✅ `BrowserDialog` | ⚠️ description-renderer only, external browser otherwise - `webview_description_renderer.cpp:110` | ⚠️ · |
| Browser tabs | ✅ `BrowserDialog::m_Tabs` | ❌ | ❌ · |
| Browser download interception | ✅ `unsupportedContent()` | ❌ | ❌ · |
| U049 QWebEngineProfile config (no persistent cookies, 50MB cache, custom paths) | ✅ `mainwindow.cpp:254-260` | ❌ | ❌ · |
| U245 BrowserDialog controls (closeable tabs, hidden urlEdit, nav buttons, new-tab titles) | ✅ `browserdialog.cpp:58-284` | ❌ | ❌ · |
| U246 Browser URL bar toggle + returnPressed navigation | ✅ `browserdialog.cpp:270-284` | ❌ | ❌ · |
| U247 guessFileName + requestDownload signal flow | ✅ `browserdialog.cpp:169-212` | ❌ | ❌ · |

## 37. Dialogs

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| About dialog | ✅ `AboutDialog` | ✅ `QMessageBox::about` (version string) - `settings_controller.cpp:754` | ✅ · |
| Selection dialog (generic picker) | ✅ `SelectionDialog` | ✅ `ListDialog` (filter, auto-select, geometry) - `list_dialog.h:20` | ✅ · |
| Credentials dialog | ✅ `CredentialsDialog` | ❌ | ❌ · |
| List dialog | ✅ `ListDialog` | ✅ `ListDialog` - `list_dialog.h:20` | ✅ · |
| Save text as dialog | ✅ `SaveTextAsDialog` | ❌ | ❌ · |
| Disable proxy plugin dialog | ✅ `DisableProxyPluginDialog` | ❌ | ❌ · |
| U174 DisableProxyPluginDialog detail (plugin table, restart note, Yes/No) | ✅ `disableproxyplugindialog.ui` | ❌ | ❌ · |
| U178 ActivateModsDialog text + Missing ESP/Mod columns | ✅ `activatemodsdialog.ui` | ❌ | ❌ · |
| U180 About dialog fields (Revision, usvfs, GitHub link, Used Software, Thanks, contributors) | ✅ `aboutdialog.ui` | ❌ `QMessageBox::about` has no fields - `settings_controller.cpp:754` | ❌ · [win] |
| U182 SelectionDialog (Select/Cancel + choice descriptions) | ✅ `selectiondialog.ui` | ⚠️ picker exists, per-choice descriptions unproven - `mod_actions.cpp:193` | ⚠️ · |
| U183 SaveTextAsDialog (Copy To Clipboard / Save As / Close) | ✅ `savetextasdialog.ui` | ❌ | ❌ · |
| U184 CredentialsDialog (Nexus login, Remember/Never ask again) (NEXUS-LENS: genericize/provider-scope) | ✅ `credentialsdialog.ui` | ❌ | ❌ · |

## 38. Platform Abstraction

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Windows (native) | ✅ | ✅ `home_dir`/`temp_dir` + windows_platform impl - `platform.h:141` | ✅ · |
| Windows (MSVC compile) | ✅ | ✅ `build-windows.ps1` (MSVC toolchain checks) | ✅ · |
| Linux (native) | ❌ | 🚀 full Linux support - `linux_platform.h:23` | 🚀 · |
| Linux (OverlayFS) | ❌ | 🚀 `OverlayFsLauncher` - `overlay_launcher.h:14` | 🚀 · |
| Linux (cgroup v2) | ❌ | 🚀 `cgroup_is_empty` - `launcher.h:142` | 🚀 · |
| Linux (subreaper) | ❌ | 🚀 `PR_SET_CHILD_SUBREAPER` - `launcher.cpp:193` | 🚀 · |
| Linux (Proton/Wine) | ❌ | 🚀 `ProtonRuntime` - `runtime.h:45` | 🚀 · |
| macOS | ❌ | ⚠️ stub `Platform` impls - `macos_platform.h:30` | ⚠️ · |
| Platform abstraction (XDG, Steam, Proton) | ❌ | 🚀 `Platform` base class (home_dir, temp_dir) - `platform.h:141` | 🚀 · |
| PathResolver (canonical paths) | ❌ | 🚀 `PathResolver` + `PathResolverRegistry` - `path_resolver.h:34` | 🚀 · |
| Keyring (OS-backed + file fallback) | ❌ | 🚀 `Keyring` + `FileKeyring` (XOR+base64) - `keyring.h:11` | 🚀 · |
| Thread priority (low) | ❌ | 🚀 `set_low_priority()` - `thread_priority.h:16` | 🚀 · |
| Headless launcher (CLI) | ❌ | 🚀 `cli::HeadlessLauncher` + `Config` - `headless_launcher.h:14` | 🚀 · |
| Proton version discovery | ❌ | 🚀 `find_proton()` / `enumerate_proton_versions()` - `proton_tools.h:37` | 🚀 · |
| Wine binary discovery | ❌ | 🚀 `find_wine()` - `linux_platform.cpp:490` | 🚀 · |
| Admin elevation check | ❌ | 🚀 `is_elevated()` on all 3 platforms - `platform.h:132` | 🚀 · |
| Symlink/junction capability check | ❌ | 🚀 `symlinks_available()` / `junctions_available()` - `platform.h:135` | 🚀 · |
| Environment variable management (get/set/path) | ✅ `env::get()`, `env::set()`, `env::path()` | ❌ | ❌ · |

## 39. Theme System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Theme manager (QSS token substitution) | ❌ | 🚀 `ThemeManager` (scan, load, apply, live-reload) - `theme_manager.h:19` | 🚀 · |
| Icon manager | ❌ | 🚀 `IconManager` - `icon_manager.h:40` | 🚀 · |
| Style manager | ❌ | 🚀 `StyleManager` - `style_manager.h:21` | 🚀 · |
| U135 Theme tab (styleBox combo + Explore... + ColorTable + Reset Colors) | ✅ `settingsdialog.ui:415-531` | ⚠️ tab + style manager; ColorTable/Reset unproven - `settings_content_widget.cpp:222` | ⚠️ · |

## 40. Event System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Event bus (subscribe/dispatch) | ❌ | 🚀 `EventBus` (17 canonical events) - `event_bus.h:62` | 🚀 · |
| Event history ring buffer | ❌ | 🚀 500-entry `EventRecord` - `event_bus.h:85` | 🚀 · |
| Plugin-scoped unsubscription | ❌ | 🚀 `clear_source()` on plugin unload - `event_bus.h:95` | 🚀 · |
| JSON payload helpers | ❌ | 🚀 `json_obj()` - `event_bus.h:120` | 🚀 · |

## 41. External Tool System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| External tool registry | ❌ | 🚀 `ToolRegistry` (Advisory/Workshop kinds) - `tool_registry.h:38` | 🚀 · |
| Dynamic tools in menu bar | ❌ | 🚀 `AppMenuBar::update_tools_for_game()` - `menu_bar.h:24` | 🚀 · |
| Proton prefix tools | ❌ | 🚀 `run_proton_tool()` - `proton_tools.h:37` | 🚀 · |
| U005 Ctrl+I = Tool Plugins (iconText "&Tools") | ✅ `mainwindow.ui:1718-1735` | ❌ | ❌ · |
| U015 Tool Plugins menu (displayName "/" grouping, tooltips, error routing) | ✅ `mainwindow.cpp:1495-1566` | ❌ | ❌ · |
| U067 Plugin tool exception routing (reportError queued) | ✅ `mainwindow.cpp:1495-1522` | ❌ | ❌ · |
| U237 IPluginTool plugin API | ✅ `mainwindow.cpp:1495` (uibase) | ❌ | ❌ · |

## 42. Packaging & Distribution

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| NSIS installer | ✅ | ⚠️ cmake target, runs makensis when found - `CMakeLists.txt:331` | ⚠️ · |
| Standalone zip | ✅ | ❌ target is a TODO echo stub - `Packaging/windows/standalone/CMakeLists.txt:11` | ❌ · |
| USVFS binaries in package | ✅ | ⏳ planned port, not started - Workspace-l0pz | ⏳ · [win] |
| AppImage (Linux) | ❌ | ❌ target is a TODO echo stub - `Packaging/linux/appimage/CMakeLists.txt:11` | ❌ · |
| Flatpak (Linux) | ❌ | ❌ target is a TODO echo stub - `Packaging/linux/flatpak/CMakeLists.txt:11` | ❌ · |
| DMG (macOS) | ❌ | ❌ target is a TODO echo stub - `Packaging/macos/dmg/CMakeLists.txt:11` | ❌ · |
| U289 About "Used Software" third-party licenses tab | ✅ `aboutdialog.ui` | ❌ | ❌ · |
| U290 AppManifest (DPI awareness, UAC) + dlls.manifest.qt6 | ✅ `app.manifest`, `dlls.manifest.qt6` | ❌ | ❌ · |
| U291 modorganizer.natvis debugger visualizers | ✅ `modorganizer.natvis` | ❌ no .natvis in repo | ❌ · |
| U292 Build tooling dependency set (CMakePresets, vcpkg.json) | ✅ `vcpkg.json` | ⚠️ deps + presets present, dep parity unproven - `vcpkg.json:1` | ⚠️ · |

## 43. CLI Command System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| CLI crashdump command | ✅ `cl::CrashDumpCommand` (dump running MO process) | ❌ | ❌ · |
| CLI launch command (spawn-wait) | ✅ `cl::LaunchCommand` (CreateProcessW + WaitForSingleObject) | ⚠️ `--launch` headless only, no spawn-wait - `command_line.cpp:22` | ⚠️ · |
| CLI run command (executable with USVFS) | ✅ `cl::RunCommand` (-e name, -a args, -c cwd) | ⚠️ `--exe` only, no USVFS - `command_line.cpp:26` | ⚠️ · |
| CLI reload-plugin command | ✅ `cl::ReloadPluginCommand` (hot-reload by name) | ❌ | ❌ · |
| CLI download-file command | ✅ `cl::DownloadFileCommand` (URL + metadata, HTTPS validation) | ❌ | ❌ · |
| CLI refresh command (F5 equivalent) | ✅ `cl::RefreshCommand` | ❌ | ❌ · |
| CLI --help | ✅ `CommandLine::showHelp()` | ✅ `show_help` flag + rendered help - `command_line.cpp:46` | ✅ · |
| CLI --multiple (allow multiple instances) | ✅ `CommandLine` --multiple flag | ❌ | ❌ · |
| CLI --pick (show instance selector) | ✅ `CommandLine` --pick flag | ❌ | ❌ · |
| CLI --logs (duplicate logs to stdout) | ✅ `CommandLine` --logs flag | ❌ stdout always on, no flag - `logger.cpp:122` | ❌ · |
| CLI -i (instance selection) | ✅ `CommandLine` -i flag | ⚠️ `--instance` long option, no `-i` - `command_line.cpp:18` | ⚠️ · |
| CLI -p (profile selection) | ✅ `CommandLine` -p flag | ❌ | ❌ · |

## 44. Tutorial System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U028 First-run "Show tutorial?" dialog + Never ask checkbox | ✅ `mainwindow.cpp:1215-1230` | ❌ | ❌ · |
| U068 Window tutorial hookup ("//WIN" headers, shouldStartTutorial, expose modList/espList) | ✅ `mainwindow.cpp:1187-1232` | ❌ | ❌ · |
| U261 Tutorial system (TutorialManager, TutorialDialog, tabChanged, expose) | ✅ `mainwindow.cpp:1187-1232` | ❌ | ❌ · |
| U288 Tutorial resources shipped (massage_messages.py, tutorials/*.js) | ✅ `src/massage_messages.py` | ❌ | ❌ · |

## 45. TaskDialog Component

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U267 TaskDialog component (multi-button dialog reused by restart/delete/INI/overwrite flows) | ✅ `taskdialog.ui` (uibase) | ✅ `TaskDialog`, TaskDialogButton ported 1:1; 10 call sites across launch/mod_actions/settings/overwrite - `task_dialog.h:57` | ✅ · |

## 46. Notifications / Problems System

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U026 Notifications action badge (badge_0..9/more pixmap, disabled at 0, tooltips, statusbar sync) | ✅ `mainwindow.cpp:926-991`, `mainwindow.ui:1820-1837` | ❌ | ❌ · |
| U056 Problems check (500ms debounce + QtConcurrent + mutex + recheck flag) | ✅ `mainwindow.cpp:926-931`, `:1024-1054` | ❌ | ❌ · |
| U181 Notifications/Problems dialog (per-item Fix button, empty placeholder) | ✅ `problemsdialog.cpp:60-75` | ❌ | ❌ · |

## 47. Backup / Restore (Load Order + Mod List)

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U034 Load order backup/restore (timestamped plugins/loadorder/lockedorder, SelectionDialog, error strings) | ✅ `mainwindow.cpp:3841-3916` | 🚀 same 3 files + confirm, per-file safety copy, per-file error lines; a failed copy is spared, not overwritten - `backup_actions.cpp:214` | 🚀 ✔ |
| U035 Mod list backup/restore (save/restore modlist.txt + toasts) | ✅ `mainwindow.cpp:3918-3940` | 🚀 `modlist.txt` pair; restore drops the pending debounced write and reloads from disk instead of flushing it back - `backup_actions.cpp:214` | 🚀 ✔ |
| U036 Backup naming scheme (.yyyy_MM_dd_hh_mm_ss, keep 10 newest) | ✅ `mainwindow.cpp:3823-3840` | 🚀 same stamp; retention by PARSED stamp + copy index, MO2 name-sorts and evicts "-10" before "-2" - `backup_service.cpp:184` | 🚀 ✔ |

## 48. File Tree Menu Protocol

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U152 File tree context menu protocol (Add as Executable, Reveal, Hide/Un-Hide, Open block, Preview bolding, Save/Refresh/Expand/Collapse) | ✅ `filetree.cpp:640-810` | ⚠️ Hide/Un-Hide + Refresh w/ tips; other blocks unproven - `data_tab.cpp:811` | ⚠️ · |
| U153 MenuItem status-tip protocol (hint + "Disabled because:" + disabledHint) | ✅ `filetree.cpp:60-115` | ⚠️ tips exist, "Disabled because:" unproven - `data_tab.cpp:815` | ⚠️ · |
| U154 File tree multi-select detail captions ("only has %1 file(s)") | ✅ `filetree.cpp:618-626` | ❌ | ❌ · |

## 49. CLI Help Grammar

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| U233 Full CommandLine grammar (visible options, -i list mode, positional subargs, help layout, error paths) | ✅ `commandline.cpp:311-465` | ❌ help text, error paths, -i list mode all absent | ❌ · |
| U234 cl:: commands grammar (crashdump/launch/run/reload-plugin/download/refresh + options/errors/forwarding) | ✅ `commandline.cpp:591-940` | ❌ options/errors/forwarding help unproven | ❌ · |

## 50. My Games Resolution

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| My Games resolution (per-game Documents/My Games dir) | ✅ (per-game My Games path resolution) | ✅ `resolve_mygames_dir()` + `game_mygames_dir()` - `game_knowledge.cpp:181` | ✅ · |

## 51. Conflict Scan Refresh

| Feature | MO2 | GMM | Status |
|---------|-----|-----|--------|
| Conflict scan refresh (rescan finishes via controller slot) | ✅ (conflict scan completion -> model refresh) | ✅ `on_conflict_scan_finished()` - `mod_list_controller.cpp:2214` | ✅ · |

---

# Summary

[↑ Back to the top ↑](#feature-map---mo2-vs-gmm)

Counted from the rows actually present. `[win]` is a tag, not a seventh
disposition, so a row appears in `[win]` **and** in exactly one disposition
column.

| Section | ✅ | ⚠️ | 🚀 | ❌ | 🚫 | ⏳ | `[win]` |
|---------|---|---|---|---|---|---|---|
| 1. Virtual Filesystem | 5 | 2 | 4 | 1 | 0 | 5 | 5 |
| 2. Launch Pipeline | 6 | 5 | 5 | 14 | 0 | 2 | 17 |
| 3. Error Handling & Diagnostics | 8 | 9 | 0 | 0 | 28 | 11 | 35 |
| 4. Settings & Configuration | 23 | 32 | 6 | 1 | 12 | 24 | 0 |
| 5. Executable Management | 8 | 5 | 4 | 10 | 0 | 0 | 0 |
| 6. Mod Management | 24 | 5 | 9 | 8 | 0 | 0 | 1 |
| 7. Mod Categories | 6 | 5 | 0 | 9 | 0 | 0 | 0 |
| 8. Mod Conflict Detection | 4 | 2 | 3 | 7 | 0 | 0 | 0 |
| 9. Mod Content Analysis | 6 | 7 | 0 | 13 | 0 | 0 | 0 |
| 10. Mod Info Dialog | 10 | 1 | 1 | 2 | 0 | 0 | 0 |
| 11. Version & Update Management | 3 | 1 | 2 | 12 | 0 | 0 | 0 |
| 12. Plugin Management | 27 | 8 | 3 | 13 | 0 | 0 | 0 |
| 13. LOOT Integration | 10 | 2 | 1 | 7 | 0 | 0 | 0 |
| 14. Profile Management | 27 | 5 | 3 | 6 | 0 | 0 | 0 |
| 15. Download Management | 19 | 12 | 6 | 24 | 10 | 0 | 0 |
| 16. Nexus Integration | 8 | 10 | 2 | 20 | 0 | 0 | 0 |
| 17. Source Providers | 4 | 0 | 7 | 1 | 0 | 0 | 0 |
| 18. Mod List Features | 22 | 12 | 3 | 14 | 0 | 0 | 0 |
| 19. Mod Context Menu | 14 | 4 | 0 | 14 | 0 | 0 | 0 |
| 20. Plugin Context Menu | 2 | 2 | 0 | 8 | 0 | 0 | 0 |
| 21. Archive & Installation | 10 | 3 | 4 | 2 | 0 | 0 | 0 |
| 22. Deploy System | 0 | 1 | 9 | 0 | 2 | 0 | 0 |
| 23. Overwrite System | 8 | 2 | 3 | 0 | 0 | 0 | 0 |
| 24. Save Game System | 16 | 4 | 5 | 2 | 0 | 0 | 0 |
| 25. Game Detection & Knowledge | 2 | 2 | 11 | 0 | 0 | 0 | 0 |
| 26. Pipeline System | 0 | 0 | 16 | 0 | 0 | 0 | 0 |
| 27. Plugin Host System | 0 | 1 | 12 | 3 | 0 | 0 | 0 |
| 28. Sort System | 0 | 0 | 5 | 0 | 0 | 0 | 0 |
| 29. Instance Management | 3 | 1 | 6 | 7 | 0 | 0 | 0 |
| 30. UI Layer | 23 | 8 | 17 | 16 | 2 | 0 | 2 |
| 31. Log System | 8 | 2 | 3 | 3 | 0 | 0 | 0 |
| 32. System Tray | 4 | 1 | 0 | 0 | 0 | 0 | 0 |
| 33. Self Updater | 2 | 3 | 0 | 4 | 0 | 0 | 0 |
| 34. Multi-Process / IPC | 2 | 2 | 0 | 3 | 0 | 0 | 0 |
| 35. Text Editor | 2 | 3 | 0 | 3 | 0 | 0 | 0 |
| 36. Browser | 0 | 1 | 0 | 6 | 0 | 0 | 0 |
| 37. Dialogs | 3 | 1 | 0 | 8 | 0 | 0 | 1 |
| 38. Platform Abstraction | 2 | 1 | 14 | 1 | 0 | 0 | 0 |
| 39. Theme System | 0 | 1 | 3 | 0 | 0 | 0 | 0 |
| 40. Event System | 0 | 0 | 4 | 0 | 0 | 0 | 0 |
| 41. External Tool System | 0 | 0 | 3 | 4 | 0 | 0 | 0 |
| 42. Packaging & Distribution | 0 | 2 | 0 | 7 | 0 | 1 | 1 |
| 43. CLI Command System | 1 | 3 | 0 | 8 | 0 | 0 | 0 |
| 44. Tutorial System | 0 | 0 | 0 | 4 | 0 | 0 | 0 |
| 45. TaskDialog Component | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| 46. Notifications / Problems System | 0 | 0 | 0 | 3 | 0 | 0 | 0 |
| 47. Backup / Restore (Load Order + Mod List) | 0 | 0 | 3 | 0 | 0 | 0 | 0 |
| 48. File Tree Menu Protocol | 0 | 2 | 0 | 1 | 0 | 0 | 0 |
| 49. CLI Help Grammar | 0 | 0 | 0 | 2 | 0 | 0 | 0 |
| 50. My Games Resolution | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| 51. Conflict Scan Refresh | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| **TOTAL** | **325** | **173** | **177** | **271** | **54** | **43** | **62** |

### The arithmetic

```
rows in file                          1043
scored rows (ok + part + miss)         769

MO2 parity        ok   / scored        325 /  769  = 42.3%
partial           part / scored        173 /  769  = 22.5%
missing           miss / scored        271 /  769  = 35.2%
GMM-exclusive     surp / all rows      177 / 1043  = 17.0%   (not scored)
```

**Parity is 325 / 769 = 42.3%.** 271 rows are outright missing and 173 partial.
The largest untouched surfaces are **44. Tutorial** and **46. Notifications /
Problems** (zero matched rows), then **16. Nexus** and **15. Downloads**, which
carry the most missing rows in absolute terms.

**4. Settings** was re-verified row by row and is no longer a missing-row
surface: 57 `❌` became 1. What it actually holds is 26 `⚠️` rows where a
control is visible and correct but nothing reads the value, plus 24 `⏳` rows
that are real MO2 features with no GMM consumer to attach to. The count of
absent features was never the real gap in that section.

### Confidence

285 of 1043 rows carry `✔` (both sides re-read); the rest carry `·` and are
leads, not findings. The `✔` rows concentrate in sections 3, 4, 15, 21, 18 and
30. Section 45's `TaskDialog` row is a stale `❌` corrected here; uibase's
`taskdialog.ui` is not vendored under `references/`, so its MO2 half is still
`·`.
