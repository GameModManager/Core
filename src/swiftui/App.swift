import SwiftUI

@main
struct GameModManagerSwiftUIApp: App {
    @StateObject private var state = BrowserState()
    var body: some Scene {
        WindowGroup("GameModManager") { RootView().environmentObject(state).onAppear { state.refresh() } }
    }
}

/// Sidebar pages — the Qt popups/dialogs promoted to navigation destinations.
enum SidebarPage: String, Hashable {
    case main
    case instanceOptions
    case executables
    case settings
    case diagnostics
    case changeInstance
}

struct RootView: View {
    @EnvironmentObject private var state: BrowserState
    var body: some View {
        NavigationSplitView {
            List(selection: $state.page) {
                Label("Main", systemImage: "house").tag(SidebarPage.main)
                Label("Instance Options", systemImage: "slider.horizontal.3").tag(SidebarPage.instanceOptions)
                Label("Executables", systemImage: "app.badge").tag(SidebarPage.executables)
                Label("Settings", systemImage: "gearshape").tag(SidebarPage.settings)
                Label("Diagnostics", systemImage: "stethoscope").tag(SidebarPage.diagnostics)

                // Change Instance LAST — full instance manager (Qt's
                // InstanceSwitcher), not a dropdown.
                Label("Change Instance", systemImage: "arrow.left.arrow.right")
                    .tag(SidebarPage.changeInstance)
            }
            .navigationTitle("GameModManager")
        } detail: {
            switch state.page {
            case .main: BrowserView()
            case .changeInstance: InstancesView()
            case .instanceOptions:
                StubView(title: "Instance Options", systemImage: "slider.horizontal.3",
                         note: "Deploy strategy, folder overrides, Proton runner and instance paths.")
            case .executables:
                StubView(title: "Executables", systemImage: "app.badge",
                         note: "Add, edit and pin executables — title, path, arguments, environment, working directory.")
            case .settings:
                StubView(title: "Settings", systemImage: "gearshape",
                         note: "Data root, sources, categories, theme and keyring.")
            case .diagnostics:
                StubView(title: "Diagnostics", systemImage: "stethoscope",
                         note: "Logs, traces and notifications.")
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .gmmRefresh)) { _ in state.refresh() }
            .onChange(of: state.selectedInstance) { _, _ in state.refresh() }
    }
}

/// Full instance manager (Qt's InstanceSwitcherContentWidget parity): the
/// complete instance list plus creation entry points. Selecting an instance
/// loads it and returns to Main.
struct InstancesView: View {
    @EnvironmentObject private var state: BrowserState
    var body: some View {
        VStack(alignment: .leading) {
            HStack {
                Text("Instances").font(.title2)
                Spacer()
                Button("Add Installed Instance…") { }
                    .disabled(true)
                Button("Add Portable Instance…") { }
                    .disabled(true)
            }
            if state.instances.isEmpty && !state.isLoading {
                ContentUnavailableView(
                    "No instances", systemImage: "square.stack.3d.up.slash",
                    description: Text("Create an installed or portable instance to get started."))
            } else {
                List(state.instances, id: \.self, selection: $state.selectedInstance) { name in
                    HStack {
                        Label(name, systemImage: "square.stack.3d.up")
                        Spacer()
                        if name == state.selectedInstance {
                            Image(systemName: "checkmark").foregroundStyle(.tint)
                        }
                    }
                    .contentShape(Rectangle())
                    .onTapGesture { state.selectInstance(name) }
                }
            }
        }.padding()
        .overlay {
            // Creation flows (game detection, per-game setup) land here after
            // the shell — stubbed so the layout is reviewable now.
            if false { EmptyView() }
        }
    }
}

/// Placeholder for a page whose features are ported after the UI shell is
/// complete.
struct StubView: View {
    let title: String
    let systemImage: String
    let note: String
    var body: some View {
        ContentUnavailableView {
            Label(title, systemImage: systemImage)
        } description: {
            Text("\(note)\n\nThis page is a stub — features land here after the shell is finished.")
        }
    }
}

struct BrowserView: View {
    @EnvironmentObject private var state: BrowserState
    var body: some View {
        VStack(alignment: .leading) {
            HStack {
                profileMenu
                Spacer()
                executableMenu
                if state.runningPID != nil {
                    ProgressView().controlSize(.small)
                    Text("Running…").foregroundStyle(.secondary)
                }
                Button("Launch") {
                    if let exe = state.selectedExecutable { state.launch(executable: exe) }
                }
                .disabled(state.selectedExecutable == nil || state.isBusy || state.runningPID != nil)
                Button(state.isLoading ? "Cancel" : "Refresh") { state.isLoading ? state.cancel() : state.refresh() }
            }
            if state.isLoading { ProgressView("Scanning…") }
            if let actionError = state.actionError { Text(actionError).foregroundStyle(.red).font(.caption) }
            if let error = state.error, !state.isLoading {
                ContentUnavailableView("Could not load", systemImage: "exclamationmark.triangle", description: Text(error))
            } else if let snapshot = state.snapshot, snapshot.mods.isEmpty {
                ContentUnavailableView("No mods", systemImage: "shippingbox")
            } else {
                let mods = state.snapshot?.mods ?? []
                List(Array(mods.enumerated()), id: \.element.id) { index, mod in
                    HStack {
                        Text("\(mod.order)").frame(width: 40, alignment: .trailing)
                        Text(mod.id)
                        Spacer()
                        Text(mod.enabled ? "Enabled" : "Disabled")
                        Button { state.moveMod(id: mod.id, delta: -1) } label: { Image(systemName: "chevron.up") }
                            .buttonStyle(.borderless).disabled(index == 0 || state.isBusy)
                        Button { state.moveMod(id: mod.id, delta: 1) } label: { Image(systemName: "chevron.down") }
                            .buttonStyle(.borderless).disabled(index == mods.count - 1 || state.isBusy)
                    }
                }
            }
        }.padding()
        .alert("New Profile", isPresented: $state.isCreatingProfile) {
            TextField("Name", text: $state.profileNameInput)
            Button("Create") {
                state.createProfile(name: state.profileNameInput)
                state.profileNameInput = ""
            }
            Button("Cancel", role: .cancel) { state.profileNameInput = "" }
        } message: { Text("Creates an empty profile.") }
        .alert("Rename Profile", isPresented: $state.isRenamingProfile) {
            TextField("Name", text: $state.profileNameInput)
            Button("Rename") {
                state.renameSelectedProfile(to: state.profileNameInput)
                state.profileNameInput = ""
            }
            Button("Cancel", role: .cancel) { state.profileNameInput = "" }
        }
        .confirmationDialog(
            "Delete Profile", isPresented: $state.confirmingDeleteProfile,
            titleVisibility: .visible
        ) {
            ForEach(state.snapshot?.profiles.filter { $0 != state.selectedProfile } ?? [], id: \.self) {
                name in
                Button("Delete \"\(name)\"", role: .destructive) { state.deleteProfile(name) }
            }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text("Removes the profile directory and all profile-specific files.")
        }
    }

    /// Executable picker (saved instance.toml entries, or the game plugin's
    /// known executables on first launch).
    private var executableMenu: some View {
        Menu {
            ForEach(state.snapshot?.executables ?? [], id: \.self) { path in
                Button {
                    state.selectedExecutable = path
                } label: {
                    if path == state.selectedExecutable {
                        Label(fileName(of: path), systemImage: "checkmark")
                    } else { Text(fileName(of: path)) }
                }
            }
        } label: {
            Label(state.selectedExecutable.map { fileName(of: $0) } ?? "No Executable",
                  systemImage: "app.badge")
        }
        .disabled((state.snapshot?.executables.isEmpty ?? true) || state.isBusy)
    }

    private func fileName(of path: String) -> String {
        (path as NSString).lastPathComponent
    }

    /// Profile switcher plus lifecycle actions (create/rename/delete).
    private var profileMenu: some View {
        Menu {
            Section("Switch Profile") {
                ForEach(state.snapshot?.profiles ?? [], id: \.self) { name in
                    Button {
                        state.selectProfile(name)
                    } label: {
                        if name == state.selectedProfile {
                            Label(name, systemImage: "checkmark")
                        } else { Text(name) }
                    }
                }
            }
            Divider()
            Button("New Profile…") {
                state.profileNameInput = ""
                state.isCreatingProfile = true
            }
            .disabled(!state.canMutateProfiles || (state.snapshot?.profiles.isEmpty ?? true))
            Button("Rename…") {
                state.profileNameInput = state.selectedProfile ?? ""
                state.isRenamingProfile = true
            }
            .disabled(!state.canMutateProfiles || state.selectedProfile == nil)
            Button("Delete Other Profile…") { state.confirmingDeleteProfile = true }
            .disabled(!state.canMutateProfiles || (state.snapshot?.profiles.count ?? 0) < 2)
        } label: {
            Label(state.selectedProfile ?? state.snapshot?.profileID ?? "Profile",
                  systemImage: "folder.badge.gearshape")
        }
        .disabled(state.isBusy)
    }
}
