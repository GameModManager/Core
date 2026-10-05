# Updater design

Status: design only. No production code has been written against this document.

Scope: how GameModManager updates itself across every distribution we ship,
and what the three orphaned updater settings are actually allowed to mean.

---

## 1. What already exists

`projects/Core/src/engine/update/` is 2039 lines. That number flatters it.
931 of those lines are `mod_update_db_client.{h,cpp}`, which is a different
feature (per-mod update polling against a public dataset, has a test, unrelated
to self-update). The self-updater subsystem proper is **1108 lines**, of which
the design verdict is below.

Nothing outside `self_updater.cpp` calls any of it. `SelfUpdater::create()` has
never been invoked. The feature has never run once.

### Per-file verdict

| File | Lines | Verdict | Why |
|---|---|---|---|
| `self_updater.h` | 54 | **fixable** | The strategy shape is right. `UpdateInfo` has no `prerelease` field and no `channel`, which is the interface defect that blocks the setting. Keep the shape, change the payload. |
| `self_updater_p.h` | 20 | **fixable** | Private helper decl. Fine. |
| `self_updater.cpp` | 238 | **fixable, three real bugs** | See below. `fetch_update_info`, `find_asset_url` and `parse_version` all have defects. The factory and `detect_distro_type` are structurally usable. |
| `windows_self_updater.cpp` | 94 | **fixable, one fatal bug** | Runs the NSIS installer while our own exe is still running and locked. Will fail or prompt. Also cannot tell installer from portable. |
| `linux_self_updater.cpp` | 90 | **dead. Delete.** | Downloads a `.tar.gz` to `/tmp/gmm_update`, chmods it, returns `success = true`. It never extracts and never replaces anything. It reports success having done nothing. Also has a stray no-op `exe.native();` at line 81. |
| `macos_self_updater.cpp` | 124 | **fixable, one fatal bug + two cases unhandled** | `cp -R "/Volumes/.../GameModManager.app" "/Applications/GameModManager.app"` nests the bundle inside the existing one on any second run. No read-only-volume check, no translocation check. |
| `flatpak_updater.cpp` | 71 | **dead. Delete.** | Line 44 runs `std::system("flatpak update --assumeyes ...")` from inside a sandbox where the `flatpak` binary does not exist. It cannot work, ever. See section 6. |
| `appimage_updater.cpp` | 107 | **fixable, works, wrong mechanism** | The rename-over-the-running-AppImage is the right shape and is the documented `AppImageUpdate` behaviour. It ignores zsync entirely and ignores the `update-information` field embedded in the AppImage, which is the whole point of the format. |
| `deb_rpm_updater.cpp` | 99 | **dead, and wrong on purpose** | `pkexec apt install -y` / `pkexec rpm -Uvh` asks the user for a root password because an application asked it to. Wrong shape, see section 4. |
| `aur_updater.cpp` | 85 | **dead, and wrong on purpose** | `yay -S gamemodmanager --noconfirm` from inside a running game mod manager. Also shells out twice just to find the helper. |
| 7 headers, one per updater | ~122 | **collapsible** | Six one-method subclasses with a factory `extern`-declaration dance. The subclasses carry no state worth the vtable. |

### The three real bugs in `self_updater.cpp`

1. **`include_prereleases` is dead.** `kGitHubApiUrl` is the `/releases/latest`
   endpoint. Per the GitHub REST docs that endpoint returns "the most recent
   **non-prerelease, non-draft** release" by definition
   (https://docs.github.com/en/rest/releases/releases?apiVersion=latest). So
   `prerelease` in the response is always `false`, the guard at line 107 can
   never fire, and the parameter can never change the outcome. Honouring
   `use_prereleases` requires switching to `/releases` and filtering client-side.
2. **`parse_version` cannot parse a prerelease tag.** It uses
   `std::regex_match` (full-string) on `(\d+)\.(\d+)\.(\d+)` after stripping a
   leading `v`. `v0.6.0-rc1` fails to match. The concept is unimplementable
   without fixing this.
3. **`find_asset_url("")` matches the first asset, arbitrarily.** The empty-suffix
   call is an unconditional hit: a zero-length suffix compares equal to any name.
   `flatpak_updater` and `aur_updater` both call `fetch_update_info("")`, so both
   get whichever asset GitHub happens to list first.

### How much is real

Roughly **250 lines of real value** out of 1108: the strategy interface shape,
the `curl_download`-with-progress plumbing (which is the same four times over
and should be one function), the version-compare intent, and the factory
dispatch skeleton. Everything else is scaffolding written against a packaging
plan that never landed, and two files (`linux_self_updater`, `flatpak_updater`)
are worse than dead: they are plausible-looking code that cannot succeed.

---

## 2. What we are shipping

### The finding: `projects/Packaging/` has almost no plan

`projects/Packaging/` is 16 files, 5 commits, and it is 90 percent `CMakeLists.txt`
stubs. Concretely, of the targets it declares:

| Target | State in `projects/Packaging/` |
|---|---|
| `common/` | Real, but tiny. Version stamp + a glob helper + a comment saying signing helpers are a placeholder. |
| `linux/installer/` | **The only real implementation.** `assemble_installer.sh` is complete: stages the binary, substitutes `@GMM_VERSION@`, calls `binarycreator`, and with `--online` also calls `repogen` and builds the online installer. `config.xml` and `packages/com.gamemodmanager.core/meta/package.xml` are real. |
| `windows/installer/` | **Stub.** Body is `echo "TODO: invoke makensis / iscc with the generated script"`. There is no `.nsi` file anywhere in the repo. |
| `windows/standalone/` | **Stub.** `echo "TODO: collect binaries + deps into a staging dir, then zip"`. |
| `linux/standalone/` | **Stub.** `echo "TODO: stage into AppDir, create tar.gz"`. |
| `linux/appimage/` | **Stub.** `echo "TODO: run linuxdeployqt / linuxdeploy to produce .AppImage"`. No manifest, no zsync, no `updateinformation`. |
| `linux/flatpak/` | **Stub.** `echo "TODO: run flatpak-builder with com.gamemodmanager.yml"`. The manifest it names does not exist. |
| `macos/dmg/` | **Stub.** `echo "TODO: invoke create-dmg or hdiutil to produce .dmg"`. |

What is genuinely absent across the whole repo:

- No `.desktop` entry.
- No AppStream `metainfo.xml` or `appdata` file.
- No Flatpak manifest, and therefore no `app-id` is chosen.
- No AppImage build script and no `appimagetool -u` update information.
- No winget, Scoop, or Chocolatey manifest.
- No Homebrew cask.
- No deb, no rpm, no spec file, no PKGBUILD.
- No reference to any version *channel* anywhere.

The one real artifact is not in `projects/Packaging/` at all. It is
`projects/Core/.github/workflows/release.yml` plus
`projects/Packaging/assemble_portable.sh`, which produce a
`GameModManager-Portable/` directory and a `.tar.gz` of it, uploaded as a GitHub
release. That is the single distribution with a working end-to-end build.

### The list of six does not match the Packaging repo

The shipping plan names: Windows installer, Windows portable, Linux distro
packages (deb/rpm/aur), Flatpak, AppImage, macOS .dmg.

`projects/Packaging/` targets: Windows NSIS installer, Windows standalone zip,
**Linux QtIFW .run**, **Linux standalone tarball**, AppImage, Flatpak, macOS DMG.

Two real mismatches:

1. **deb / rpm / AUR has zero presence in the packaging plan.** The Linux story
   in `projects/Packaging/` is a Qt Installer Framework `.run` installer plus a
   tarball, not distro packages. The updater has three updaters (`deb_rpm`,
   `aur`) for a distribution nobody has planned to build.
2. **The two Linux targets that ARE planned (QtIFW, standalone tarball) are not
   in the list of six.** They are omitted from the shipping plan, and the QtIFW
   one is the only packaging target with a working script.

This needs a decision before any of the install-method work can be trusted.
The honest reading of `projects/Packaging/` is a **seven**-target plan of which
one builds, and the shipping plan is a **six**-target plan of which none have a
declared install path. They are not the same plan.

### Release feed state

- App version: `0.5.27`, from `project(GameModManager VERSION 0.5.27)` in
  `projects/Core/CMakeLists.txt:15`, propagated as the `VERSION` compile
  definition in `projects/Core/src/engine/CMakeLists.txt:223`.
- Git tags in `projects/Core`: one, `v0.4.3`.
- GitHub releases: none. `gh release list --repo GameModManager/Core` is empty.
- `release.yml` is `on: workflow_dispatch` only. There is no tag-triggered
  release.

So the version, the tag history, and the release feed are three inconsistent
things. There is currently no feed to read.

### Assumptions this section is forced into

Everything below is an **assumption**, not a lookup, unless marked otherwise.

- **A1.** Windows installer install path: **undecided.** `projects/Core/CMakeLists.txt:343-355`
  references `packaging/windows/installer.nsi` for the `package_installer`
  target, but no such file exists in either repo. NSIS `MultiUser` defaults to
  per-machine (Program Files, elevation required) unless the script says
  otherwise (https://nsis.sourceforge.io/Docs/MultiUser/Readme.html). We get to
  choose. Recommendation: **per-user only**, `$LOCALAPPDATA\Programs\GameModManager`.
  It removes the elevation problem entirely and matches what an updater can do
  without a helper service.
- **A2.** Windows portable install path: **user-chosen directory**, containing
  `gamemodmanager.exe` plus the `windeployqt` output and `deps/`. This one is
  real, from `.github/workflows/build-windows.yml`.
- **A3.** Linux QtIFW install path: `$HOME/GameModManager/`, with the app at
  `$HOME/GameModManager/gamemodmanager`. This is **real**, from
  `linux/installer/config.xml` `<TargetDir>@HomeDir@/GameModManager</TargetDir>`.
- **A4.** Linux standalone install path: a user-chosen extracted directory
  containing `gamemodmanager`, `gamemodmanager.sh`, and `lib/`. Real, from
  `.github/workflows/build-linux.yml` (the `gamemodmanager.sh` `LD_LIBRARY_PATH`
  wrapper is generated there).
- **A5.** Flatpak `app-id`: **undecided.** `linux/flatpak/CMakeLists.txt` names
  `com.gamemodmanager.yml` in a TODO string; nothing else exists. The
  `linux/installer` package id is `com.gamemodmanager.core`. The `app-id` is the
  single most consequential undecided value in the Flatpak case, because the
  portal's `Update` method acts on the **calling app** and there is no way to
  pass an id.
- **A6.** AppImage install path: a user-chosen file path, e.g.
  `~/Applications/GameModManager-0.5.27-amd64.AppImage`.
- **A7.** macOS: `.app` bundle name **undecided** (`macos/dmg/CMakeLists.txt` is
  a stub). Install path is `/Applications/GameModManager.app` by convention.
  Not ours to choose yet.
- **A8.** deb / rpm / AUR install path: **no plan exists.** `/usr/bin/gamemodmanager`
  is the convention if it is ever built.
- **A9.** A release channel concept **does not exist** in the packaging plan.
  See section 5.

---

## 3. Install-method detection

This is the load-bearing decision. The current
`SelfUpdater::detect_distro_type()` cannot be the basis for it: it spawns up to
three subprocesses (`popen("pacman -Q ...")`, `popen("dpkg -l ...")`,
`popen("rpm -q ...")`) on a per-launch path, mislabels any pacman-installed
package as "aur", and returns `"unknown"` for every case that is actually a
first-class distribution (Windows portable, Linux QtIFW, Linux standalone,
macOS). It is a Linux-only guess, not a detector.

Proposed detector: **`InstallMethod detect_install_method()`**, a single pure
function in the engine, cheap enough to call on every launch, ordered
cheapest-and-most-definitive first, and returning one of the enum values below.
No subprocesses on the hot path. One `stat` and one registry read at most.

| Enum | Distribution | Concrete runtime signal | Notes |
|---|---|---|---|
| `WindowsInstaller` | Windows, NSIS per-user | Uninstall registry entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\{our-guid}` whose `InstallLocation` equals the directory of the running exe | Requires the installer to write the entry. Costs one `WriteRegStr` in `installer.nsi`, which does not exist yet (A1). |
| `WindowsPortable` | Windows, zip | Running exe directory has no uninstall entry pointing at it | Residual case, so it is last. Correct by elimination as long as `WindowsInstaller` is checked first. |
| `LinuxQtIfw` | Linux, QtIFW `.run` | Sibling `installerbase` executable next to the running binary, and the binary is at `$HOME/GameModManager/gamemodmanager` | Two signals, both real (A3). `installerbase` is installed by default in both offline and online QtIFW installers (https://doc.qt.io/qtinstallerframework/ifw-tools.html). |
| `LinuxStandalone` | Linux, tarball | Sibling `gamemodmanager.sh` and a `lib/` directory next to the running binary | Real, from `build-linux.yml` (A4). |
| `Flatpak` | Flatpak | `/.flatpak-info` exists **and** `$FLATPAK_ID` is set and non-empty | `/.flatpak-info` is the definitive marker. `$FLATPAK_ID` alone is what the current code uses; keeping both means an env var a user sets by hand cannot force us down the Flatpak path. |
| `AppImage` | AppImage | `$APPIMAGE` is set and non-empty **and** the path exists | This is the documented contract, and the value is also what we hand to `AppImageUpdate`. |
| `Deb` | deb | `/var/lib/dpkg/info/gamemodmanager.list` exists | Direct filesystem read of the dpkg database, no `dpkg-query` subprocess. |
| `Rpm` | rpm | `rpm -qf <running exe>` succeeds and reports a non-AUR owner | One subprocess, but only reached when the cheaper signals all miss, and the result is worth it because it separates AUR from a repo package. |
| `Aur` | AUR | Same as `Rpm` but the owning package is the AUR one | The AUR PKGBUILD must exist (A8). Until it does, `Aur` is unreachable. |
| `MacOsDmg` | macOS, installed | Running bundle is under `/Applications` or `~/Applications`, **and** the containing volume is writable | Writability is the part that matters and the part the current code omits. |
| `MacOsMountedImage` | macOS, running from a `.dmg` | Running bundle is under `/Volumes/...` | The read-only case. Detected separately because the correct action is a refusal, not an install. |
| `Unknown` | anything else | fallback | Hand off to the download page. Never guess. |

Ordering matters and is a correctness property, not a style choice:

- `Flatpak` before `LinuxQtIfw` / `LinuxStandalone`. Inside a sandbox, `HOME` is
  `~/.var/app/$ID`, so path shapes differ, but a build that happens to bundle a
  `gamemodmanager.sh` should still be identified as Flatpak. The Flatpak signal
  is definitive, so it goes first.
- `AppImage` before everything else on Linux. Same reasoning: `$APPIMAGE` is
  definitive when set.
- `WindowsInstaller` before `WindowsPortable`. Same reasoning, inverted: the
  portable case is a residual, so it must be last.
- `MacOsMountedImage` before `MacOsDmg`, because the action differs (refuse vs
  install), not because the detection is ambiguous.

What the detector must **not** do: infer from a `gamemodmanager` file name. The
name is identical across all seven targets, so it carries zero information.

---

## 4. Install method per distribution

"Can self-update" means: the app can replace its own files without a package
manager, a portal, or an external helper, and without asking for credentials.

| Distribution | Self-update? | Method | Target the update runs |
|---|---|---|---|
| **Windows installer** (NSIS, per-user, A1) | No, not directly | Download the new `Setup.exe` to `%TEMP%`, exit, and hand off to the installer's own silent mode | New installer overwrites the install dir. NSIS `/S` for silent (https://nsis.sourceforge.net/Docs/Chapter3.html) |
| **Windows portable** | Yes, with a helper | Download the new zip, extract, write a `.cmd` that waits for our PID to exit then swaps the directory | `$APPIMAGE`-equivalent: the user-chosen dir |
| **Linux QtIFW** | No | Hand off to the installed maintenance tool | `~/GameModManager/installerbase check-updates` / `update` (https://doc.qt.io/qtinstallerframework/ifw-cli.html). Requires an **online** build, because `assemble_installer.sh` strips `<RemoteRepositories>` on offline builds. An offline `.run` install has nothing to check against. |
| **Linux standalone** | No, in place | Extract the new tarball over the install dir via a post-exit helper, same shape as Windows portable | The user-chosen dir |
| **deb / rpm / AUR** | **No, and never** | Detect and report only. Do not install. | Nothing. Surface "version 0.5.28 is available from your package manager", optionally with a copyable command. |
| **Flatpak** | No | `org.freedesktop.portal.Flatpak.UpdateMonitor.Update`, then relaunch via the portal `Spawn` with `FLATPAK_SPAWN_FLAGS_LATEST_VERSION` | Flatpak's own deployment. See section 6. |
| **AppImage** | Yes, this is the format's purpose | Delegate to the ecosystem: `appimageupdatetool` bundled in the AppImage, or link `libappimageupdate` | The running `.AppImage`, via zsync delta. Update information is embedded by `appimagetool -u` (https://docs.appimage.org/packaging-guide/optional/updates.html) |
| **macOS `.dmg`** | No, not from a mounted image | If running from `/Volumes/...`: download the new `.dmg`, open it, tell the user to drag. If running from `/Applications`: download, mount, replace, unmount. | `/Applications/GameModManager.app` |

Three of these deserve their reasoning stated.

**Package-managed Linux is a refusal, not an install.** The current
`deb_rpm_updater` shells out to `pkexec apt install -y`, which means the running
game mod manager asks the user for a root password in order to update itself.
That is the behaviour of a malware sample, not a desktop application. The
ecosystem rule is that the package manager owns the package, and
https://github.com/AppImageCommunity/AppImageUpdate states the reasoning
directly: an in-app updater "would need to configure and would need root to
update App". A user who installed from apt wants apt to decide when the version
changes, so apt decides. This also removes the elevation prompt, the
`package.xml`-version-mismatch problem, and the whole `deb_rpm`/`aur` pair of
subclasses.

**Windows installer and Windows portable need opposite behaviour.** The
installer path is a managed install, so the correct update is to re-run the
vendor installer and let it own the directory. The portable path is an
unmanaged directory, so the correct update is to write into it ourselves. The
current code does the installer behaviour unconditionally, and does it while
the exe is still running and therefore locked by Windows.

**macOS from a mounted `.dmg` genuinely cannot self-update.** A mounted disk
image is a read-only volume; there is no writable path for a replacement bundle.
Sparkle, the de-facto macOS update framework, treats this as a first-class
detection and by default does not even notify the user about it
(https://sparkle-project.github.io/documentation/). App Translocation
complicates it further: a quarantined app is moved to a read-only mount under
`/private/var/folders/...` and the original location is not reliable. The
mitigation the ecosystem settled on is a symlink to `/Applications` inside the
DMG, which is an instruction to the user, not a code path.

---

## 5. The three settings

All three live in `projects/Core/src/ui/settings/settings.cpp` and are surfaced
in `projects/Core/src/ui/settings/settings_content_widget.cpp`. All three have a
getter, a setter, and a checkbox, and no reader. Each currently carries a
tooltip admitting it is not wired (lines 112-119 and 531-534 of the widget
file). That honesty is the right default and the design below preserves it.

### `check_for_updates` (default `true`)

Real semantics: a **periodic** background check, not a launch check, and not
only-on-demand. MO2's cadence is a single check at startup, triggered from
`OrganizerCore::checkForUpdates()` on core init
(`references/modorganizer/src/organizercore.cpp:240`) and again whenever the
setting is toggled in the dialog (`mainwindow.cpp:2847-2851`). MO2 has no timer.

**Recommendation: deviate from MO2.** A network call on every launch is a real
cost and a real privacy question. Every launch is an unauthenticated request to
`api.github.com` that reveals the user's IP, the app version, and the fact that
they run this app, to a third party, without consent beyond a default-true
checkbox. The honest default is **off**, with the check triggered:

- explicitly, from Help > Check for Updates, and
- on a timer, at most once every 24 hours, only when `offline_mode` is off.

Two more gates, both of which are non-negotiable rather than optional:
`Network::` already short-circuits every request when `offline_mode` is set
(`projects/Core/src/engine/network/network_manager.cpp:1118-1120`), so the
updater gets that for free by going through the facade. And a `last_checked`
timestamp must be stored so the timer means something and so the UI can show
"last checked: <time>", the shape MO2 uses for its MOTD dialog
(`mainwindow.cpp:2971-2979`).

**Name verdict:** the name promises "on startup" and the checkbox says "Check
for updates on startup". If the design becomes periodic, the label must change
to match. A control whose label describes a cadence we do not implement is the
exact defect the standing rule forbids.

### `use_prereleases` (default `false`)

**Honest answer: a prerelease concept does not exist on our feed, and inventing
it would be dishonest.** Concretely:

- There are no GitHub releases at all. `gh release list` is empty.
- There is one git tag, `v0.4.3`, and it is not a release.
- The packaging plan has no channel concept of any kind, in any file.
- The current implementation cannot honour the setting even in principle,
  because `/releases/latest` never returns a prerelease (section 1, bug 1), and
  because `parse_version` cannot parse a prerelease tag (section 1, bug 2).

What prerelease *would* mean once a feed exists: GitHub exposes a boolean
`prerelease` on each release, and MO2 filters on exactly that
(`references/modorganizer/src/selfupdater.cpp:115-116`). That is the whole
mechanism. Our `UpdateInfo` carries no such field, so honouring the setting means
adding one and changing the interface all seven updaters implement.

**Recommendation:** gate this. Either (a) implement it properly, which means
`/releases` not `/releases/latest`, a client-side draft-plus-prerelease filter, a
prerelease-aware version parse, and an `UpdateInfo` field, or (b) keep the
checkbox disabled with a tooltip naming the prerequisite. What must not happen is
leaving it enabled and unread, which is where it is now. Option (a) is small
once the feed exists; option (b) is zero. Do (b) now, (a) when there is a second
prerelease to distribute.

### `check_update_after_install` (default `true`)

**This is not an app-update setting. It is a mod-update setting, and the name is
about to mean something misleading.**

MO2's setting is `InterfaceSettings::checkUpdateAfterInstallation`
(`references/modorganizer/src/settings.cpp:2265`) and its only consumer is
`MainWindow::modInstalled` (`mainwindow.cpp:2461-2476`), which calls
`checkModsForUpdates` for the mod that was just installed. It is a **mod**
update check triggered by a **mod** install. Our key name,
`interface/check_update_after_install`, and our checkbox, "Check for updates
after install", both read as app updates. They are not, and if the app updater
were wired naively a reader would wire it to the wrong thing.

Confirming the two features are separate:

- App self-update: this document. Release feed, per-distribution install method.
- Mod update: `ModUpdateDbClient` in the same directory (931 lines, has a test,
  also has no UI caller yet), plus the LoversLab provider for per-mod refresh.

They share no code, no feed, and no trigger. They are two features that happen
to sit in one directory.

**Recommendation:** rename to `check_update_after_mod_install`, relabel the
checkbox "Check for mods for updates after install", and wire it to the **mod**
update path when that lands. Keep it out of the app updater entirely. If the mod
update UI does not exist yet, the honest state is a disabled control with that
tooltip, not a live checkbox that promises a mod check.

---

## 6. Risks and open questions

### The Flatpak recommendation (the important one)

**Recommendation: use the Flatpak portal, `org.freedesktop.portal.Flatpak`, via
`CreateUpdateMonitor` then `UpdateMonitor.Update`, and relaunch with the
portal's `Spawn` using `FLATPAK_SPAWN_FLAGS_LATEST_VERSION`. Never touch
`/app`. Never shell out to `flatpak`. Never request `org.freedesktop.Flatpak`
(`flatpak-session-helper`) or `--allow=devel`.**

Sourced findings:

1. **A sandboxed app cannot overwrite its own binary, and this is not a bug to
   work around.** `/app` is a bind mount of the application, and the Flatpak
   sandbox spec lists it under "All mounts are read-only", with `/var`, the root
   tmpfs, `/proc`, `/dev/pts` and `/dev/shm` as the only writable exceptions.
   Making `/app` writable appears only in the sandbox page's list of *optional*
   build-time flags ("useful when building apps")
   (https://github.com/flatpak/flatpak/wiki/Sandbox). So the write fails `EROFS`
   and always has.
2. **The `flatpak` binary is not inside the sandbox.** The Flatpak command
   reference lists the commands available inside the sandbox as
   `flatpak-spawn(1)` and nothing else
   (https://docs.flatpak.org/en/latest/flatpak-command-reference.html). `PATH`
   inside the sandbox is `/app/bin:/usr/bin`, where `/usr` is the runtime, and
   the runtime does not carry the flatpak binary. So
   `std::system("flatpak update --assumeyes $FLATPAK_ID")`, which is exactly
   what `flatpak_updater.cpp:44` does, fails 100 percent of the time. It has
   never been run, so nobody has noticed.
3. **`flatpak-spawn --host` is the wrong tool and is a Flathub problem.**
   `flatpak-spawn --host` routes to `org.freedesktop.Flatpak`, the
   `flatpak-session-helper` `Development.HostCommand` method, which is arbitrary
   command execution on the host
   (https://github.com/flatpak/flatpak-xdg-utils/blob/master/src/flatpak-spawn.c).
   It is not in the default session-bus allowlist. Getting it requires
   `--talk-name=org.freedesktop.Flatpak`, which is a permission Flathub pushes
   back on and which widens the app's attack surface for no benefit, since
   `Spawn` in the sandbox is sufficient for a relaunch.
4. **The portal is in the default allowlist and is built for exactly this.**
   The default D-Bus policy allows an app to talk to "portal APIs of the form
   `org.freedesktop.portal.*`"
   (https://docs.flatpak.org/en/latest/sandbox-permissions.html). The portal
   exposes `CreateUpdateMonitor` (added in interface version 2, flatpak 1.5.0+)
   and, on the returned object, `Update`, `UpdateAvailable`, `Progress`, `Close`
   (https://github.com/flatpak/flatpak/blob/master/data/org.freedesktop.portal.Flatpak.xml).
   A shipped project uses this exact path:
   https://github.com/prokopto-dev/nparse-plus/pull/159.
5. **The ecosystem convention agrees, and the GNOME maintainer said so in
   writing.** "We do think that updating software is a system responsibility
   that should be controlled by global policies and be under the users control,
   so we haven't quite followed the request", and the portal is offered as the
   answer (https://blogs.gnome.org/mclasen/2018/08/02/on-flatpak-updates/).
   Same post documents the `/app/.updated` marker flatpak leaves behind for a
   running instance that was updated, which is how a well-behaved app offers
   "restart to update" without asking the portal anything.
6. **`--allow=devel` is irrelevant here and should not be requested.** It grants
   `ptrace` and `perf` (https://github.com/flatpak/flatpak/releases/tag/0.6.10).
   It does nothing for self-update, and it is a debugger permission in a
   shipping package. Same for `--socket=session-bus`, which the docs call a
   security risk and reserve for development tools.

**Cost of being wrong, in both directions:**

- *If we ship the current `flatpak update` shelling:* every Flatpak user clicks
  Update, gets a shell "command not found" and a nonzero exit, and the updater
  reports a failure. There is no good outcome available and no way for the user
  to fix it. The Flatpak build is worse than useless: it advertises updating and
  cannot.
- *If we ship an in-place self-overwrite:* the write fails `EROFS` on every
  attempt, and if it ever did not (a manifest with `/app` made writable, or a
  `--filesystem=host` grant), it would corrupt the deployment's integrity and
  desynchronise the app from its OSTree commit and its runtime. `flatpak repair`
  becomes the user's problem. This is the failure mode worth spending a design
  pass to avoid.
- *If we ship `flatpak-spawn --host`:* the Flathub submission is at risk, and we
  have granted a game mod manager arbitrary host command execution. Unacceptable
  for the benefit.
- *If we ship the portal:* the one real limitation is stated in the interface
  XML: "updates are only allowed if the new version has the same permissions (or
  less) than the currently installed version. If the new version requires a new
  permission then the operation will fail with
  `org.freedesktop.DBus.Error.NotSupported` and updates has to be done with the
  system tools." So a release that adds a `finish-arg` cannot be pushed in
  place. That is a **release-process** constraint, not a code bug, and it is
  cheap to honour: any release that changes `finish-args` says so in its notes,
  and the updater surfaces the `NotSupported` error as "update this with your
  package manager" rather than as a crash. Net: the portal can fail, but every
  failure mode is a message the user can act on.

**Cost of the recommendation being untested.** No CI runner here has a portal in
a sandbox, so the first Flatpak-Update click in the wild is the first real test.
That is acceptable **only** because the failure mode is a message, not a
corrupted deployment. Degrade explicitly: if the portal is unreachable (not
sandboxed, no session bus, portal older than 2, D-Bus policy refused), fall back
to the read-only path: report the available version and tell the user to run
`flatpak update <app-id>`. Never fall back to something that writes.

### The A1 problem: there is no Windows installer script

`projects/Core/CMakeLists.txt:343-355` defines a `package_installer` target that
invokes `makensis` on `packaging/windows/installer.nsi`, and that file does not
exist. On a Windows machine with NSIS installed, the target fails. Everything
about the Windows installer case, including the registry key the detector reads,
depends on a file we have not written.

### The A5 problem: there is no Flatpak app-id

`linux/flatpak/CMakeLists.txt` names `com.gamemodmanager.yml` inside a TODO
string. The Flatpak portal's `Update` acts on the **calling app** and takes no
id argument, so the app-id is not a cosmetic choice: it is the identity the
update is applied to, it is what a user types into `flatpak update`, and it is
what the release must publish to a remote for the portal to find anything. This
needs deciding before the Flatpak work starts, not during it.

### The release feed does not exist

`release.yml` is `workflow_dispatch`-only, there are no releases, and the one
tag is `v0.4.3` while the app is `0.5.27`. An updater with nothing to check
against will either be a permanent no-op or will misreport. Phase 1 below does
not fix this, which means Phase 1 is testable against a locally-published
release and not against anything real.

### The `libappimageupdate` dependency question

The correct AppImage update is zsync delta via the AppImage ecosystem tooling.
That means either bundling `appimageupdatetool` inside the AppImage (no build
system coupling, subprocess, works) or linking `libappimageupdate` (in-process,
a real dependency to vendor and keep ABI-current). The lazy and more robust
choice is the bundled tool invoked as a subprocess, which is also what the
ecosystem recommends, including for apps in other languages
(https://github.com/AppImageCommunity/AppImageUpdate/wiki/Self-updating-AppImages).
It does mean the AppImage build has to stop being a stub.

### The offline QtIFW case has nothing to check against

`assemble_installer.sh` strips `<RemoteRepositories>` from `config.xml` for
offline builds. An offline `.run` install therefore has a maintenance tool with
no repository. The updater must detect that and say so, rather than invoking
`check-updates` and reporting a failure.

### Do we adopt Sparkle on macOS

The `.dmg` case has a well-solved ecosystem answer (Sparkle: signed feed, delta
updates, read-only-volume detection, `/Applications` symlink guidance) and no
good one of our own. Adopting it means a new third-party dependency plus a
release-signing key plus a feed format. Doing it ourselves means a DMG mount,
a bundle replacement, an authorization prompt, and translocation handling, all
of which have known sharp edges. This is a real fork in the road and it should be
an explicit decision, not a default. The lazy version is: download the new DMG,
open it, tell the user to drag. It is less polished and it is correct.

---

## 7. Implementation plan

Sizes are rough and assume serial work in the main checkout.

### Phase 0 - decide, do not build

Nothing to code. Outputs:

- Resolve the six-versus-seven mismatch in section 2. Either deb/rpm/AUR joins
  the packaging plan, or the three package updaters are deleted and detection
  keeps only the `Deb`/`Rpm`/`Aur` enum values for a future plan.
- Write `packaging/windows/installer.nsi` (A1), choosing per-user, writing the
  uninstall registry key the detector reads.
- Choose the Flatpak `app-id` (A5) and write `packaging/linux/flatpak/com.gamemodmanager.yml`.
- Decide the macOS bundle name (A7) and whether Sparkle is adopted.
- Fix the release pipeline so a tag produces a GitHub release with named assets,
  and reconcile `0.5.27` against `v0.4.3`.

Phase 1 is blocked on the first two. Say so rather than starting.

### Phase 1 - detection and read-only reporting

This is the phase that makes the Windows cases shippable.

- Add `InstallMethod` enum and `detect_install_method()` to the engine, as
  designed in section 3, with **no subprocesses** on the hot path.
- Add a `release_feed` query that goes through `Network::` (so `offline_mode`
  gates it for free) and reads the correct endpoint. Return version, tag,
  changelog, per-platform asset list, and a `prerelease` flag.
- Wire Help > Check for Updates (see section 5 for the cadence and the label
  change) to a "version X.Y.Z is available" notification. **No install action
  yet.**
- Fix the three settings: rename `check_update_after_install` to
  `check_update_after_mod_install` and relabel it; gate `use_prereleases`; relabel
  `check_for_updates` to match the cadence actually implemented.
- Store and display `last_checked`.
- Delete `linux_self_updater.cpp` and `flatpak_updater.cpp`, and their headers and
  their `CMakeLists.txt` entries. Both are code that cannot succeed.

Outcome: the three checkboxes have readers, the version comparison is correct
against a real feed, and detection is testable without running an installer.
Cost of being wrong here is a wrong label or a wrong "you are up to date", which
is cheap. This is the phase to ship first.

### Phase 2 - Windows installer and Windows portable installs

**This is the phase that delivers a working updater for the first two shipped
distributions.**

- `WindowsInstaller`: download `Setup.exe` to `%TEMP%`, show progress, then exit
  and let the installer run `/S`. Order matters: the app must be gone before the
  installer writes, because Windows locks a running exe. A launched-from-`system`
  silent install with the app still resident will fail or prompt.
- `WindowsPortable`: download the zip, extract to a staging dir, write a `.cmd`
  that loops on our PID and then swaps the directory, launch it detached, exit.
  The wait loop is the whole trick and it is about fifteen lines.
- Reuse one download-with-progress helper for both, replacing the four copies.
- Two tests per case: the right `InstallMethod` is detected from a synthetic
  layout, and the update plan is produced without touching the filesystem.

Outcome: Windows installer and Windows portable both update. These are the
first ship and the easiest, which is the right place to start.

### Phase 3 - AppImage, Linux standalone, Linux QtIFW

- AppImage: bundle `appimageupdatetool` in the AppImage (a Packaging change
  first), call it with `-j` to check and `$APPIMAGE` to apply, honour the embedded
  update information instead of hardcoding `.AppImage`. Deletes the zsync-ignoring
  download-then-rename in `appimage_updater.cpp`.
- Linux standalone: same post-exit helper shape as Windows portable.
- Linux QtIFW: invoke the sibling `installerbase` with `check-updates` then
  `update`. Detect the offline case and say "this install has no update
  repository" instead of reporting a failure.

### Phase 4 - Flatpak, via the portal

- D-Bus client for `org.freedesktop.portal.Flatpak`: `CreateUpdateMonitor`,
  `UpdateMonitor.Update`, `Progress` signals, then `Spawn` with
  `FLATPAK_SPAWN_FLAGS_LATEST_VERSION`.
- Degrade on every unreachable path to the read-only message, per section 6.
- Surface `NotSupported` as "update with your package manager", which is the
  documented permissions-changed case.
- Also watch `/app/.updated` and offer a restart, which is the
  cheapest-correct path when something else updated us.
- No `finish-args` beyond the default. Verify the manifest grants neither
  `org.freedesktop.Flatpak` nor `--allow=devel`, with a test that fails if either
  appears.

This is last on purpose. It is the only one whose failure can leave a broken
installation, so it ships after the detection layer that can identify it is even
needed.

### Phase 5 - macOS

Only after the bundle name and the Sparkle question are settled. If Sparkle is
adopted, this phase is mostly manifest and feed plumbing. If not, it is
download-and-open-the-DMG plus the `/Applications` symlink, and the updater
refuses cleanly when running from a mounted image.

---

## Sources

Workspace files read:

- `projects/Packaging/README.md`, `CMakeLists.txt`, `common/CMakeLists.txt`,
  `assemble_portable.sh`, `linux/installer/CMakeLists.txt`,
  `linux/installer/assemble_installer.sh`, `linux/installer/config.xml`,
  `linux/installer/packages/com.gamemodmanager.core/meta/package.xml`,
  `linux/standalone/CMakeLists.txt`, `linux/appimage/CMakeLists.txt`,
  `linux/flatpak/CMakeLists.txt`, `windows/installer/CMakeLists.txt`,
  `windows/standalone/CMakeLists.txt`, `macos/dmg/CMakeLists.txt`
- `projects/Core/CMakeLists.txt`, `projects/Core/src/engine/CMakeLists.txt`,
  `projects/Core/src/engine/update/` (all 15 files),
  `projects/Core/src/engine/github.{h,cpp}`,
  `projects/Core/src/engine/network/network_manager.cpp`,
  `projects/Core/src/ui/settings/settings.{h,cpp}`,
  `projects/Core/src/ui/settings/settings_content_widget.cpp`,
  `projects/Core/src/ui/widgets/menu_bar.cpp`,
  `projects/Core/.github/workflows/{release,build-linux,build-windows}.yml`,
  `projects/Core/docs/featuremap.md`
- `references/modorganizer/src/selfupdater.{h,cpp}`,
  `references/modorganizer/src/organizercore.cpp`,
  `references/modorganizer/src/mainwindow.cpp`,
  `references/modorganizer/src/settings.cpp`,
  `references/modorganizer/src/settingsdialoggeneral.cpp`

External sources:

- GitHub REST API, releases: https://docs.github.com/en/rest/releases/releases?apiVersion=latest
  (`/releases/latest` returns the most recent non-prerelease, non-draft release)
- Flatpak sandbox wiki (read-only `/app`, optional writable `/app` is a build
  flag): https://github.com/flatpak/flatpak/wiki/Sandbox
- Flatpak sandbox permissions and the default session-bus D-Bus allowlist:
  https://docs.flatpak.org/en/latest/sandbox-permissions.html
- Flatpak command reference (only `flatpak-spawn` is available inside the
  sandbox): https://docs.flatpak.org/en/latest/flatpak-command-reference.html
- Flatpak portal interface XML (`CreateUpdateMonitor`, `Update`, `Spawn`,
  `FLATPAK_SPAWN_FLAGS_LATEST_VERSION`, the same-permissions-or-fewer rule):
  https://github.com/flatpak/flatpak/blob/master/data/org.freedesktop.portal.Flatpak.xml
- ashpd `UpdateMonitor` binding: https://fractal-1f518c.pages.gitlab.gnome.org/ashpd/flatpak/struct.UpdateMonitor.html
- Matthias Clasen, "On Flatpak updates" (ecosystem convention, `/app/.updated`):
  https://blogs.gnome.org/mclasen/2018/08/02/on-flatpak-updates/
- `flatpak-spawn` source (`--host` routes to `Development.HostCommand` on
  `org.freedesktop.Flatpak`):
  https://github.com/flatpak/flatpak-xdg-utils/blob/master/src/flatpak-spawn.c
- A shipping project using the portal path:
  https://github.com/prokopto-dev/nparse-plus/pull/159
- `--allow=devel` grants ptrace and perf:
  https://github.com/flatpak/flatpak/releases/tag/0.6.10
- Flatpak user versus system install, and `flatpak update` per installation:
  https://docs.flathub.org/docs/for-users/user-vs-system-install
- AppImage updateability, `appimagetool -u`, zsync:
  https://docs.appimage.org/packaging-guide/optional/updates.html
- AppImageUpdate self-updating guidance and the root-requires reasoning:
  https://github.com/AppImageCommunity/AppImageUpdate/wiki/Self-updating-AppImages
  and https://github.com/AppImageCommunity/AppImageUpdate
- Qt Installer Framework CLI (`check-updates`, `update`):
  https://doc.qt.io/qtinstallerframework/ifw-cli.html
- Qt Installer Framework tools and the maintenance tool shipped by default:
  https://doc.qt.io/qtinstallerframework/ifw-tools.html
- Qt Installer Framework update repositories:
  https://doc.qt.io/qtinstallerframework/ifw-updates.html
- NSIS multi-user install modes and the Program Files per-machine default:
  https://nsis.sourceforge.io/Docs/MultiUser/Readme.html
- NSIS command line, `/S` silent:
  https://nsis.sourceforge.net/Docs/Chapter3.html
- Sparkle documentation (read-only volume and translocation are not notified by
  default; `/Applications` symlink in the DMG):
  https://sparkle-project.github.io/documentation/
