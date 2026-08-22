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
    let profiles: [String]
    let gameDir: String
    let steamAppid: UInt32
    let executables: [String]
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

    func snapshot(instanceID: String, profileID: String? = nil) throws -> BrowserSnapshot {
        if Task.isCancelled { throw CancellationError() }
        let operation = gmm_swift_operation_create()
        defer { gmm_swift_operation_destroy(operation) }
        let handle = instanceID.withCString { instance in
            (profileID ?? "").withCString { profile in
                gmm_swift_snapshot_create_for_profile(self.engine, instance, profile, operation)
            }
        }
        guard let snapshot = handle else {
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
        try performMutation(instanceID: instanceID, viewProfile: profileID) { engine, instance, view, operation in
            modID.withCString { mod in
                gmm_swift_move_mod(engine, instance, view, mod, Int32(newPriority), operation)
            }
        }
    }

    func createProfile(instanceID: String, name: String, viewProfile: String?) throws -> BrowserSnapshot {
        try performMutation(instanceID: instanceID, viewProfile: viewProfile) { engine, instance, view, operation in
            name.withCString { gmm_swift_create_profile(engine, instance, $0, view, operation) }
        }
    }

    func renameProfile(instanceID: String, oldName: String, newName: String, viewProfile: String?)
        throws -> BrowserSnapshot
    {
        try performMutation(instanceID: instanceID, viewProfile: viewProfile) { engine, instance, view, operation in
            oldName.withCString { old in
                newName.withCString { new in
                    gmm_swift_rename_profile(engine, instance, old, new, view, operation)
                }
            }
        }
    }

    func deleteProfile(instanceID: String, name: String, isActive: Bool, viewProfile: String?)
        throws -> BrowserSnapshot
    {
        try performMutation(instanceID: instanceID, viewProfile: viewProfile) { engine, instance, view, operation in
            name.withCString { gmm_swift_delete_profile(engine, instance, $0, isActive ? 1 : 0, view, operation) }
        }
    }

    /// Shared C ABI call/result plumbing for mutations: runs `call` with fresh
    /// strings and an operation, then unwraps the owned result into an
    /// immutable snapshot. `viewProfile` may be nil — the C layer treats an
    /// empty string like null (fall back to the first profile). Serialized by
    /// the actor.
    private func performMutation(
        instanceID: String, viewProfile: String?,
        _ call: @escaping (GmmSwiftEngineHandle?, UnsafePointer<CChar>, UnsafePointer<CChar>, GmmSwiftOperationHandle?)
            -> GmmSwiftMutationResultHandle?
    ) throws -> BrowserSnapshot {
        if Task.isCancelled { throw CancellationError() }
        let operation = gmm_swift_operation_create()
        defer { gmm_swift_operation_destroy(operation) }
        let result = instanceID.withCString { instance in
            (viewProfile ?? "").withCString { view in call(self.engine, instance, view, operation) }
        }
        guard let result else { throw BridgeError.message("mutation failed") }
        defer { gmm_swift_result_destroy(result) }
        switch gmm_swift_result_code(result) {
        case GMM_SWIFT_RESULT_OK:
            guard let snapshot = gmm_swift_result_snapshot(result) else {
                throw BridgeError.message("mutation returned no snapshot")
            }
            return try Self.readSnapshot(snapshot)
        case GMM_SWIFT_RESULT_CANCELLED:
            throw CancellationError()
        default:
            let message = gmm_swift_result_error(result).map(String.init(cString:)) ?? "mutation failed"
            throw BridgeError.message(message)
        }
    }

    /// Launches through the canonical engine path (prepare_launch_params
    /// deploys enabled mods, then launch_game). The deploy runs on this actor
    /// thread — never MainActor. Returns the launched process id.
    func launch(instanceID: String, executable: String) throws -> Int64 {
        if Task.isCancelled { throw CancellationError() }
        let operation = gmm_swift_operation_create()
        defer { gmm_swift_operation_destroy(operation) }
        let result = instanceID.withCString { instance in
            executable.withCString { exe in
                gmm_swift_launch(self.engine, instance, exe, operation)
            }
        }
        guard let result else { throw BridgeError.message("launch failed") }
        defer { gmm_swift_launch_destroy(result) }
        switch gmm_swift_launch_code(result) {
        case GMM_SWIFT_RESULT_OK:
            let pid = gmm_swift_launch_pid(result)
            guard pid > 0 else { throw BridgeError.message("launch returned no process") }
            return pid
        case GMM_SWIFT_RESULT_CANCELLED:
            throw CancellationError()
        default:
            let message = gmm_swift_launch_error(result).map(String.init(cString:)) ?? "launch failed"
            throw BridgeError.message(message)
        }
    }

    func isProcessAlive(pid: Int64) -> Bool {
        gmm_swift_process_alive(pid) != 0
    }

    private static func readSnapshot(_ handle: GmmSwiftSnapshotHandle) throws -> BrowserSnapshot {
        let mods = (0..<gmm_swift_snapshot_mod_count(handle)).compactMap { index -> ModRow? in
            let mod = gmm_swift_snapshot_mod_at(handle, index)
            guard let id = mod.id else { return nil }
            return ModRow(id: String(cString: id), order: Int(mod.order), enabled: mod.enabled != 0)
        }
        let profiles = (0..<gmm_swift_snapshot_profile_count(handle)).compactMap { index -> String? in
            gmm_swift_snapshot_profile_at(handle, index).map(String.init(cString:))
        }
        let executables = (0..<gmm_swift_snapshot_executable_count(handle)).compactMap { index -> String? in
            gmm_swift_snapshot_executable_at(handle, index).map(String.init(cString:))
        }
        return BrowserSnapshot(
            instanceID: String(cString: gmm_swift_snapshot_instance_id(handle)!),
            gameID: String(cString: gmm_swift_snapshot_game_id(handle)!),
            profileID: String(cString: gmm_swift_snapshot_profile_id(handle)!),
            profiles: profiles,
            gameDir: gmm_swift_snapshot_game_dir(handle).map(String.init(cString:)) ?? "",
            steamAppid: gmm_swift_snapshot_steam_appid(handle),
            executables: executables, mods: mods)
    }
}

enum BridgeError: LocalizedError { case message(String); var errorDescription: String? { if case let .message(value) = self { return value }; return nil } }

@MainActor
final class BrowserState: ObservableObject {
    @Published var instances = [String]()
    @Published var selectedInstance: String?
    @Published var selectedProfile: String?
    @Published var selectedExecutable: String?
    @Published var runningPID: Int64?
    @Published var snapshot: BrowserSnapshot?
    @Published var isLoading = false
    @Published var error: String?
    @Published var isMutating = false
    @Published var actionError: String?
    // Profile action sheets (create/rename text input, delete confirmation).
    @Published var isCreatingProfile = false
    @Published var isRenamingProfile = false
    @Published var confirmingDeleteProfile = false
    @Published var profileNameInput = ""
    var isBusy: Bool { isLoading || isMutating }
    /// Mutations need a loaded snapshot and an idle engine.
    var canMutateProfiles: Bool { snapshot != nil && !isBusy }
    private var generation = 0
    private var work: Task<Void, Never>?
    let client = EngineClient()

    func cancel() {
        generation += 1
        work?.cancel()
        isLoading = false
    }

    /// Switches the viewed profile and re-reads its snapshot.
    func selectProfile(_ name: String) {
        guard name != selectedProfile else { return }
        selectedProfile = name
        refresh()
    }

    /// Shared mutation plumbing: bumps the generation so any in-flight or
    /// late-arriving result is rejected as stale, keeps work off MainActor,
    /// and assigns the refreshed immutable snapshot on success.
    private func runMutation(_ op: @escaping () async throws -> BrowserSnapshot) {
        guard snapshot != nil, !isMutating else { return }
        generation += 1
        work?.cancel()
        let current = generation
        isMutating = true
        actionError = nil
        Task {
            defer { if current == generation { isMutating = false } }
            do {
                let updated = try await op()
                guard current == generation else { return }
                self.snapshot = updated
                selectedProfile = updated.profileID
            } catch is CancellationError {
            } catch {
                if current == generation { actionError = error.localizedDescription }
            }
        }
    }

    func createProfile(name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard canMutateProfiles, !trimmed.isEmpty, let current = snapshot else { return }
        runMutation { try await self.client.createProfile(
            instanceID: current.instanceID, name: trimmed, viewProfile: current.profileID) }
    }

    /// Renames the viewed profile. Safe here even though Qt refuses it: the
    /// bridge holds no live Profile handles, so the returned snapshot simply
    /// targets the new name and the selection follows.
    func renameSelectedProfile(to newName: String) {
        let trimmed = newName.trimmingCharacters(in: .whitespaces)
        guard canMutateProfiles, !trimmed.isEmpty, let oldName = selectedProfile,
              oldName != trimmed, let current = snapshot else { return }
        runMutation { try await self.client.renameProfile(
            instanceID: current.instanceID, oldName: oldName, newName: trimmed,
            viewProfile: trimmed) }
    }

    /// Deletes another profile (MO2 parity: the viewed profile cannot be
    /// deleted — the caller switches to a survivor first).
    func deleteProfile(_ name: String) {
        guard canMutateProfiles, let viewed = selectedProfile, name != viewed,
              let current = snapshot, current.profiles.contains(name),
              current.profiles.count > 1 else { return }
        runMutation { try await self.client.deleteProfile(
            instanceID: current.instanceID, name: name, isActive: false,
            viewProfile: viewed) }
    }

    /// Launches the selected executable through the canonical engine path.
    /// Keeps isMutating until the process is up, then monitors it in the
    /// background and refreshes once it exits (post-exit state may have
    /// changed — overwrite capture, deferred disables).
    func launch(executable: String) {
        guard let snapshot, !isBusy, runningPID == nil, !executable.isEmpty,
              snapshot.executables.contains(executable) else { return }
        generation += 1
        work?.cancel()
        let current = generation
        let instanceID = snapshot.instanceID
        isMutating = true
        actionError = nil
        Task {
            do {
                let pid = try await client.launch(instanceID: instanceID, executable: executable)
                guard current == generation else { return }
                isMutating = false
                runningPID = pid
                monitor(pid: pid, generation: current)
            } catch is CancellationError {
                if current == generation { isMutating = false }
            } catch {
                if current == generation {
                    isMutating = false
                    actionError = error.localizedDescription
                }
            }
        }
    }

    /// Polls the launched process off MainActor until it exits or the user
    /// navigates away (generation bump), then refreshes the snapshot.
    private func monitor(pid: Int64, generation atGeneration: Int) {
        work?.cancel()
        work = Task {
            while await client.isProcessAlive(pid: pid) {
                if atGeneration != generation || Task.isCancelled { return }
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
            guard atGeneration == generation, !Task.isCancelled else { return }
            runningPID = nil
            refresh()
        }
    }

    /// Moves a mod up (delta -1) or down (delta +1) in the profile order.
    /// Bumps the generation so any in-flight refresh result is rejected as stale.
    func moveMod(id: String, delta: Int) {
        guard !isBusy, let snapshot = snapshot,
              let index = snapshot.mods.firstIndex(where: { $0.id == id }),
              snapshot.mods.indices.contains(index + delta) else { return }
        let priority = snapshot.mods[index + delta].order
        runMutation { try await self.client.moveMod(
            instanceID: snapshot.instanceID, profileID: snapshot.profileID,
            modID: id, newPriority: priority) }
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
                var result: BrowserSnapshot?
                if let selected {
                    do {
                        result = try await client.snapshot(instanceID: selected, profileID: selectedProfile)
                    } catch is CancellationError {
                        throw CancellationError()
                    } catch {
                        // A stale profile selection (instance switched/rename
                        // raced) falls back to the instance's default once.
                        if selectedProfile != nil {
                            selectedProfile = nil
                            result = try await client.snapshot(instanceID: selected)
                        } else { throw error }
                    }
                }
                guard current == generation else { return }
                instances = values; selectedInstance = selected; snapshot = result
                selectedProfile = result?.profileID
                if let execs = result?.executables, !execs.isEmpty {
                    if let chosen = selectedExecutable, execs.contains(chosen) { /* keep */ }
                    else { selectedExecutable = execs.first }
                } else {
                    selectedExecutable = nil
                }
                if selected != nil && result == nil { error = "Unable to read the selected instance." }
            } catch is CancellationError { }
            catch let failure { if current == generation { self.error = failure.localizedDescription } }
            if current == generation { isLoading = false }
        }
    }
}
