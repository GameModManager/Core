import Foundation
import Combine
import GmmSwiftBridge

extension Notification.Name { static let gmmRefresh = Notification.Name("gmm.refresh") }

private let refreshCallback: GmmSwiftRefreshFn = { event, payload, _ in
    // The bridge owns copied strings before this callback crosses to MainActor.
    let values = [event.map(String.init(cString:)) ?? "", payload.map(String.init(cString:)) ?? ""]
    Task { @MainActor in NotificationCenter.default.post(name: .gmmRefresh, object: values) }
}

struct ModRow: Identifiable, Sendable {
    let id: String
    let order: Int
    let enabled: Bool
}

struct BrowserSnapshot: Sendable {
    let instanceID: String
    let gameID: String
    let profileID: String
    let mods: [ModRow]
}

actor EngineClient {
    private let engine: GmmSwiftEngineHandle

    init() {
        let environment = ProcessInfo.processInfo.environment
        let instanceDir = environment["GMM_SWIFT_INSTANCES_DIR"]
        let pluginDir = environment["GMM_SWIFT_PLUGINS_DIR"]
        if let instanceDir {
            engine = instanceDir.withCString { instances in
                if let pluginDir { return pluginDir.withCString { plugins in gmm_swift_engine_create(instances, plugins) } }
                return gmm_swift_engine_create(instances, nil)
            }
        } else if let pluginDir {
            engine = pluginDir.withCString { plugins in gmm_swift_engine_create(nil, plugins) }
        } else {
            engine = gmm_swift_engine_create(nil, nil)
        }
        gmm_swift_subscribe_refresh(engine, refreshCallback, nil)
    }

    deinit { gmm_swift_engine_destroy(engine) }

    func instances() -> [String] {
        return (0..<gmm_swift_instance_count(engine)).compactMap {
            guard let value = gmm_swift_instance_id(engine, $0) else { return nil }
            return String(cString: value)
        }
    }

    func snapshot(instanceID: String) throws -> BrowserSnapshot {
        if Task.isCancelled { throw CancellationError() }
        let operation = gmm_swift_operation_create()
        defer { gmm_swift_operation_destroy(operation) }
        guard let snapshot = gmm_swift_snapshot_create(engine, instanceID, operation) else {
            if gmm_swift_operation_is_cancelled(operation) != 0 { throw CancellationError() }
            let ownedError = gmm_swift_last_error(engine)
            let message = ownedError.map(String.init(cString:)) ?? "snapshot failed"
            if let ownedError { gmm_swift_free_string(ownedError) }
            throw BridgeError.message(message)
        }
        defer { gmm_swift_snapshot_destroy(snapshot) }
        if Task.isCancelled { throw CancellationError() }
        let mods = (0..<gmm_swift_snapshot_mod_count(snapshot)).compactMap { index -> ModRow? in
            let mod = gmm_swift_snapshot_mod_at(snapshot, index)
            guard let id = mod.id else { return nil }
            return ModRow(id: String(cString: id), order: Int(mod.order), enabled: mod.enabled != 0)
        }
        return BrowserSnapshot(
            instanceID: String(cString: gmm_swift_snapshot_instance_id(snapshot)!),
            gameID: String(cString: gmm_swift_snapshot_game_id(snapshot)!),
            profileID: String(cString: gmm_swift_snapshot_profile_id(snapshot)!), mods: mods)
    }
}

enum BridgeError: LocalizedError { case message(String); var errorDescription: String? { if case let .message(value) = self { return value }; return nil } }

@MainActor
final class BrowserState: ObservableObject {
    @Published var instances = [String]()
    @Published var selectedInstance: String?
    @Published var snapshot: BrowserSnapshot?
    @Published var isLoading = false
    @Published var error: String?
    private var generation = 0
    private var work: Task<Void, Never>?
    let client = EngineClient()

    func cancel() {
        generation += 1
        work?.cancel()
        isLoading = false
    }

    func refresh() {
        work?.cancel()
        generation += 1
        let current = generation
        isLoading = true; error = nil
        work = Task {
            do {
                let values = await client.instances()
                let selected = selectedInstance.flatMap { values.contains($0) ? $0 : nil } ?? values.first
                let result: BrowserSnapshot?
                if let selected { result = try await client.snapshot(instanceID: selected) }
                else { result = nil }
                guard current == generation else { return }
                instances = values; selectedInstance = selected; snapshot = result
                if selected != nil && result == nil { error = "Unable to read the selected instance." }
            } catch is CancellationError { }
            catch let failure { if current == generation { self.error = failure.localizedDescription } }
            if current == generation { isLoading = false }
        }
    }
}
