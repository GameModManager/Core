import SwiftUI

@main
struct GameModManagerSwiftUIApp: App {
    @StateObject private var state = BrowserState()
    var body: some Scene {
        WindowGroup("GameModManager") { BrowserView().environmentObject(state).onAppear { state.refresh() } }
    }
}

struct BrowserView: View {
    @EnvironmentObject private var state: BrowserState
    var body: some View {
        NavigationSplitView {
            List(state.instances, id: \.self, selection: $state.selectedInstance) { Text($0) }
                .navigationTitle("Instances")
        } detail: {
            VStack(alignment: .leading) {
                HStack {
                    profileMenu
                    if state.isMutating { ProgressView().controlSize(.small) }
                    Spacer()
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
        }.onReceive(NotificationCenter.default.publisher(for: .gmmRefresh)) { _ in state.refresh() }
            .onChange(of: state.selectedInstance) { _, _ in state.refresh() }
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
