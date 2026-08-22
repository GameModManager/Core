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
                HStack { Text(state.snapshot?.profileID ?? "Profile").font(.title2); Spacer(); Button(state.isLoading ? "Cancel" : "Refresh") { state.isLoading ? state.cancel() : state.refresh() } }
                if state.isLoading { ProgressView("Scanning…") }
                else if let error = state.error { ContentUnavailableView("Could not load", systemImage: "exclamationmark.triangle", description: Text(error)) }
                else if let snapshot = state.snapshot, snapshot.mods.isEmpty { ContentUnavailableView("No mods", systemImage: "shippingbox") }
                else { List(state.snapshot?.mods ?? []) { mod in HStack { Text("\(mod.order)").frame(width: 40, alignment: .trailing); Text(mod.id); Spacer(); Text(mod.enabled ? "Enabled" : "Disabled") } } }
            }.padding()
        }.onReceive(NotificationCenter.default.publisher(for: .gmmRefresh)) { _ in state.refresh() }
            .onChange(of: state.selectedInstance) { _, _ in state.refresh() }
    }
}
