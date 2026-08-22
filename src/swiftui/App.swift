import SwiftUI
import UniformTypeIdentifiers
import AppKit

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
            case .diagnostics: DiagnosticsView()
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
    // Pinned toolbar shortcuts: executable paths, comma-separated.
    @AppStorage("gmm.pinnedExecutables") private var pinnedExecutables = ""
    @State private var showingShortcutsSheet = false
    @State private var isImporting = false
    @State private var isExporting = false
    @State private var isNamingSeparator = false
    @State private var separatorNameInput = ""

    private var pins: [String] {
        pinnedExecutables.split(separator: ",").map(String.init)
            .filter { state.snapshot?.executables.contains($0) ?? false }
    }

    var body: some View {
        HSplitView {
            ModListPanel(
                onImport: { isImporting = true },
                onExport: { isExporting = true },
                onCreateSeparator: { separatorNameInput = ""; isNamingSeparator = true }
            )
            .frame(minWidth: 380, idealWidth: 480)
            ExecPanel(showingShortcutsSheet: $showingShortcutsSheet)
                .frame(minWidth: 420, idealWidth: 560)
        }
        .toolbar {
            // Only pinned shortcuts live here (SwiftUI parity decision).
            ToolbarItemGroup {
                ForEach(pins, id: \.self) { path in
                    Button {
                        state.launch(executable: path)
                    } label: {
                        Label(fileName(of: path), systemImage: "play.circle")
                    }
                    .disabled(state.isBusy || state.runningPID != nil)
                    .help("Run \(fileName(of: path))")
                }
            }
        }
        .sheet(isPresented: $showingShortcutsSheet) { ShortcutsSheet(pins: $pinnedExecutables) }
        .fileImporter(isPresented: $isImporting, allowedContentTypes: [.commaSeparatedText, .plainText]) { result in
            if case .success(let url) = result { importModlist(from: url) }
        }
        .fileExporter(isPresented: $isExporting,
                      document: ModlistDocument(rows: state.snapshot?.mods ?? []),
                      contentType: .commaSeparatedText,
                      defaultFilename: "modlist.csv") { _ in }
        .alert("New Separator", isPresented: $isNamingSeparator) {
            TextField("Name", text: $separatorNameInput)
            Button("Create") {
                state.createSeparator(name: separatorNameInput)
                separatorNameInput = ""
            }
            Button("Cancel", role: .cancel) { separatorNameInput = "" }
        } message: { Text("Creates a separator folder in the mods directory.") }
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

    /// Reads an exported modlist CSV (Qt-compatible columns; folder_name is
    /// column 7) and applies its order.
    private func importModlist(from url: URL) {
        guard let csv = try? String(contentsOf: url, encoding: .utf8) else { return }
        let folders = csv.split(separator: "\n").compactMap { line -> String? in
            let fields = line.split(separator: ",", omittingEmptySubsequences: false)
            guard fields.count >= 7, fields[0].trimmingCharacters(in: .whitespaces) == "mod"
            else { return nil }
            return fields[6].trimmingCharacters(in: CharacterSet(charactersIn: "\" \r"))
        }.filter { !$0.isEmpty }
        state.importModlistOrder(folders)
    }

    fileprivate func fileName(of path: String) -> String {
        (path as NSString).lastPathComponent
    }
}

/// CSV document for modlist export (Qt ImportModlist format).
struct ModlistDocument: FileDocument {
    static let readableContentTypes: [UTType] = [.commaSeparatedText]
    var rows: [ModRow]

    init(rows: [ModRow]) { self.rows = rows }
    init(configuration: ReadConfiguration) throws { rows = [] }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        var csv = "type,priority,name,source_link,color,modid,folder_name\n"
        for row in rows.sorted(by: { $0.order > $1.order }) {
            let name = row.id.hasSuffix("_separator")
                ? String(row.id.dropLast("_separator".count))
                : row.id
            csv += "\(row.id.hasSuffix("_separator") ? "separator" : "mod"),\(row.order),\(name),,,,\(row.id)\n"
        }
        var wrapper = FileWrapper(regularFileWithContents: Data(csv.utf8))
        wrapper.preferredFilename = "modlist.csv"
        return wrapper
    }
}

/// Left pane: profile selector, folder/action row, mod list with checkbox,
/// drag & drop reorder, and columns.
struct ModListPanel: View {
    @EnvironmentObject private var state: BrowserState
    let onImport: () -> Void
    let onExport: () -> Void
    let onCreateSeparator: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                profileMenu
                Spacer()
                Button(state.isLoading ? "Cancel" : "Refresh") {
                    state.isLoading ? state.cancel() : state.refresh()
                }
            }
            actionRow
            if state.isLoading { ProgressView("Scanning…") }
            if let error = state.error, !state.isLoading {
                ContentUnavailableView("Could not load", systemImage: "exclamationmark.triangle",
                                       description: Text(error))
            } else {
                modList
            }
            if let actionError = state.actionError {
                Text(actionError).foregroundStyle(.red).font(.caption)
            }
        }.padding(8)
    }

    /// Open-folder buttons (mods / profiles / game), import/export, separator.
    private var actionRow: some View {
        HStack(spacing: 10) {
            Button { open(state.snapshot?.modsDir) } label: { Image(systemName: "shippingbox") }
                .help("Open Mods Folder").disabled(state.snapshot?.modsDir.isEmpty ?? true)
            Button { open(state.snapshot?.profilesDir) } label: { Image(systemName: "person.2") }
                .help("Open Profiles Folder").disabled(state.snapshot?.profilesDir.isEmpty ?? true)
            Button { open(state.snapshot?.gameDir) } label: { Image(systemName: "gamecontroller") }
                .help("Open Game Folder").disabled(state.snapshot?.gameDir.isEmpty ?? true)
            Divider().frame(height: 16)
            Button { onImport() } label: { Label("Import", systemImage: "square.and.arrow.down") }
                .disabled(!state.canMutateProfiles)
            Button { onExport() } label: { Label("Export", systemImage: "square.and.arrow.up") }
                .disabled((state.snapshot?.mods.isEmpty ?? true))
            Divider().frame(height: 16)
            Button { onCreateSeparator() } label: { Label("Separator", systemImage: "plus") }
                .disabled(!state.canMutateProfiles)
            Spacer()
        }
    }

    private func open(_ path: String?) {
        guard let path else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: path, isDirectory: true))
    }

    private var modList: some View {
        List {
            // Column header (MO2-style table, priority descending).
            HStack {
                Text("").frame(width: 28)
                Text("Mod").frame(minWidth: 0, maxWidth: .infinity, alignment: .leading)
                Text("Priority").frame(width: 64, alignment: .trailing)
            }
            .font(.caption).foregroundStyle(.secondary).listRowSeparator(.hidden)
            ForEach(displayedMods, id: \.id) { mod in
                ModRowView(mod: mod)
            }
            .onMove { source, destination in
                guard let id = source.first.flatMap({ displayedMods[$0].id }),
                      displayedMods.indices.contains(destination) else { return }
                state.moveModTo(id: id, destination: destination)
            }
        }
        .listStyle(.inset)
        .overlay {
            if (state.snapshot?.mods.isEmpty ?? true) && !state.isLoading {
                ContentUnavailableView("No mods", systemImage: "shippingbox")
            }
        }
    }

    /// Display order: ascending — priority 1 (lowest) first.
    private var displayedMods: [ModRow] {
        (state.snapshot?.mods.sorted(by: { $0.order < $1.order })) ?? []
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

/// One mod row: enable checkbox, name, priority column (descending display).
struct ModRowView: View {
    @EnvironmentObject private var state: BrowserState
    let mod: ModRow

    var body: some View {
        HStack {
            Toggle("", isOn: Binding(
                get: { mod.enabled },
                set: { state.toggleMod(id: mod.id, enabled: $0) }
            ))
            .labelsHidden().toggleStyle(.checkbox)
            .disabled(state.isBusy)
            if mod.id.hasSuffix("_separator") {
                Label(String(mod.id.dropLast("_separator".count)), systemImage: "line.3.horizontal")
                    .frame(minWidth: 0, maxWidth: .infinity, alignment: .leading)
            } else {
                Text(mod.id)
                    .frame(minWidth: 0, maxWidth: .infinity, alignment: .leading)
            }
            Text("\(mod.order + 1)")
                .frame(width: 64, alignment: .trailing)
                .foregroundStyle(.secondary).font(.callout.monospacedDigit())
        }
        .contentShape(Rectangle())
    }
}

/// Right pane: executable dropdown + Run + shortcuts button above the tabbed
/// Data / Plugins / Downloads view.
struct ExecPanel: View {
    @EnvironmentObject private var state: BrowserState
    @Binding var showingShortcutsSheet: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                executableMenu
                Button {
                    if let exe = state.selectedExecutable { state.launch(executable: exe) }
                } label: {
                    Label("Run", systemImage: "play.fill")
                }
                .keyboardShortcut(.return, modifiers: [])
                .disabled(state.selectedExecutable == nil || state.isBusy || state.runningPID != nil)
                Button {
                    showingShortcutsSheet = true
                } label: { Image(systemName: "bookmark") }
                    .help("Manage pinned shortcuts")
                if state.runningPID != nil {
                    ProgressView().controlSize(.small)
                    Text("Running…").foregroundStyle(.secondary)
                }
                Spacer()
            }
            TabView {
                StubView(title: "Data", systemImage: "externaldrive",
                         note: "Deploy management for this instance.")
                    .tabItem { Label("Data", systemImage: "externaldrive") }
                StubView(title: "Plugins", systemImage: "puzzlepiece.extension",
                         note: "Plugin management and load order.")
                    .tabItem { Label("Plugins", systemImage: "puzzlepiece.extension") }
                StubView(title: "Downloads", systemImage: "arrow.down.circle",
                         note: "Downloads, sources and nxm:// handling.")
                    .tabItem { Label("Downloads", systemImage: "arrow.down.circle") }
            }
        }.padding(8)
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
}

/// Pin/unpin executables as toolbar shortcuts.
struct ShortcutsSheet: View {
    @EnvironmentObject private var state: BrowserState
    @Binding var pins: String
    @Environment(\.dismiss) private var dismiss

    private var pinned: Set<String> {
        Set(pins.split(separator: ",").map(String.init))
    }

    var body: some View {
        NavigationStack {
            List(state.snapshot?.executables ?? [], id: \.self) { path in
                Toggle(isOn: Binding(
                    get: { pinned.contains(path) },
                    set: { on in
                        var current = pinned
                        if on { current.insert(path) } else { current.remove(path) }
                        pins = current.sorted().joined(separator: ",")
                    }
                )) {
                    Label((path as NSString).lastPathComponent, systemImage: "app.badge")
                        .help(path)
                }
            }
            .navigationTitle("Pinned Shortcuts")
            .toolbar {
                ToolbarItem { Button("Done") { dismiss() } }
            }
        }
        .frame(minWidth: 380, minHeight: 280)
    }
}

/// Diagnostics: live engine log stream (replayed history + live tail).
struct DiagnosticsView: View {
    @StateObject private var log = LogStore()
    @State private var autoScroll = true
    private let levels = ["Debug", "Info", "Warn", "Error"]

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Picker("Level", selection: $log.minLevel) {
                    ForEach(0..<4) { Text(levels[$0]).tag($0) }
                }
                .pickerStyle(.segmented).frame(width: 260)
                Toggle("Auto-scroll", isOn: $autoScroll)
                Spacer()
                Button("Clear") { log.clear() }
            }.padding(8)
            ScrollViewReader { proxy in
                List(log.filtered) { entry in
                    HStack(alignment: .top, spacing: 8) {
                        Text(entry.levelLabel)
                            .font(.caption.bold().monospaced())
                            .foregroundStyle(levelColor(entry.level))
                            .frame(width: 44, alignment: .leading)
                        Text(entry.timestamp)
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                            .frame(width: 150, alignment: .leading)
                        Text(entry.message)
                            .font(.system(size: 11, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(minWidth: 0, maxWidth: .infinity, alignment: .leading)
                    }
                    .id(entry.id)
                }
                .onChange(of: log.entries.count) { _, _ in
                    guard autoScroll, let last = log.entries.last else { return }
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
        }
    }

    private func levelColor(_ level: Int) -> Color {
        switch level {
        case 3: .red
        case 2: .orange
        case 1: .primary
        default: .secondary
        }
    }
}
