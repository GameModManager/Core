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
        return try Self.readSnapshot(snapshot)
    }

    /// Moves a mod to a new priority within a profile. Serialized by the actor;
    /// returns the refreshed immutable snapshot on success.
    func moveMod(instanceID: String, profileID: String, modID: String, newPriority: Int) throws
        -> BrowserSnapshot
    {
        if Task.isCancelled { throw CancellationError() }
        let operation = gmm_swift_operation_create()
        defer { gmm_swift_operation_destroy(operation) }
        let result = instanceID.withCString { instance in
            profileID.withCString { profile in
                modID.withCString { mod in
                    gmm_swift_move_mod(self.engine, instance, profile, mod, Int32(newPriority), operation)
                }
            }
        }
        guard let result else { throw BridgeError.message("move failed") }
        defer { gmm_swift_result_destroy(result) }
        switch gmm_swift_result_code(result) {
        case GMM_SWIFT_RESULT_OK:
            guard let snapshot = gmm_swift_result_snapshot(result) else {
                throw BridgeError.message("move returned no snapshot")
            }
            return try Self.readSnapshot(snapshot)
        case GMM_SWIFT_RESULT_CANCELLED:
            throw CancellationError()
        default:
            let message = gmm_swift_result_error(result).map(String.init(cString:)) ?? "move failed"
            throw BridgeError.message(message)
        }
    }

    private static func readSnapshot(_ handle: GmmSwiftSnapshotHandle) throws -> BrowserSnapshot {
        let mods = (0..<gmm_swift_snapshot_mod_count(handle)).compactMap { index -> ModRow? in
            let mod = gmm_swift_snapshot_mod_at(handle, index)
            guard let id = mod.id else { return nil }
            return ModRow(id: String(cString: id), order: Int(mod.order), enabled: mod.enabled != 0)
        }
        return BrowserSnapshot(
            instanceID: String(cString: gmm_swift_snapshot_instance_id(handle)!),
            gameID: String(cString: gmm_swift_snapshot_game_id(handle)!),
            profileID: String(cString: gmm_swift_snapshot_profile_id(handle)!), mods: mods)
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
    @Published var isMutating = false
    @Published var actionError: String?
    var isBusy: Bool { isLoading || isMutating }
    private var generation = 0
    private var work: Task<Void, Never>?
    let client = EngineClient()

    func cancel() {
        generation += 1
        work?.cancel()
        isLoading = false
    }

    /// Moves a mod up (delta -1) or down (delta +1) in the profile order.
    /// Bumps the generation so any in-flight refresh result is rejected as stale.
    func moveMod(id: String, delta: Int) {
        guard !isBusy, let snapshot = snapshot,
              let index = snapshot.mods.firstIndex(where: { $0.id == id }),
              snapshot.mods.indices.contains(index + delta) else { return }
        generation += 1
        work?.cancel()
        let current = generation
        let priority = snapshot.mods[index + delta].order
        isMutating = true
        actionError = nil
        Task {
            defer { if current == generation { isMutating = false } }
            do {
                let updated = try await client.moveMod(
                    instanceID: snapshot.instanceID, profileID: snapshot.profileID,
                    modID: id, newPriority: priority)
                guard current == generation else { return }
                self.snapshot = updated
            } catch is CancellationError {
            } catch {
                if current == generation { actionError = error.localizedDescription }
            }
        }
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
