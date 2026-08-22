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
                    Text(state.snapshot?.profileID ?? "Profile").font(.title2)
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
        }.onReceive(NotificationCenter.default.publisher(for: .gmmRefresh)) { _ in state.refresh() }
            .onChange(of: state.selectedInstance) { _, _ in state.refresh() }
    }
}
