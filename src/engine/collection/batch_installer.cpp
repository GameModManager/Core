#include "engine/collection/batch_installer.h"

namespace engine::Collection {

namespace {

// Strip an optional "sha256:" prefix; empty stays empty.
std::string normalize_hash(std::string h) {
    constexpr const char* kPrefix = "sha256:";
    if (h.rfind(kPrefix, 0) == 0)
        h.erase(0, 7);
    return h;
}

struct HashExtractor {
    std::optional<std::string> operator()(const SourceNexus& s) const {
        return s.sha256.empty()
                   ? std::nullopt
                   : std::optional<std::string>{normalize_hash(s.sha256)};
    }
    std::optional<std::string> operator()(const SourceLoversLab& s) const {
        return s.sha256.empty()
                   ? std::nullopt
                   : std::optional<std::string>{normalize_hash(s.sha256)};
    }
    std::optional<std::string> operator()(const SourceModPub& s) const {
        return s.sha256.empty()
                   ? std::nullopt
                   : std::optional<std::string>{normalize_hash(s.sha256)};
    }
    // Steam Workshop: version/hash always unknown ahead of time.
    std::optional<std::string> operator()(const SourceSteamWorkshop&) const {
        return std::nullopt;
    }
    std::optional<std::string> operator()(const SourceDirect& s) const {
        return s.sha256.empty()
                   ? std::nullopt
                   : std::optional<std::string>{normalize_hash(s.sha256)};
    }
};

} // namespace

std::optional<std::string> expected_hash(const ModSource& source) {
    return std::visit(HashExtractor{}, source);
}

BatchInstaller::BatchInstaller(BatchInstallerDeps deps)
    : deps_(std::move(deps)) {}

BatchResult BatchInstaller::install(const ResolvedCollection& collection) const {
    BatchResult result;
    const int total = static_cast<int>(collection.mods.size());
    result.mods.reserve(static_cast<size_t>(total));

    // Flatten phase buckets into install order (phase 0 first, manifest
    // order within each phase). The resolver places every mod in exactly
    // one bucket; an entry missing from all buckets is a resolver bug, so
    // fall back to mods[] order for anything not yet emitted.
    std::vector<const ResolvedMod*> ordered;
    ordered.reserve(static_cast<size_t>(total));
    for (const auto& bucket : collection.phases)
        for (const ResolvedMod* rm : bucket)
            if (rm)
                ordered.push_back(rm);
    if (static_cast<int>(ordered.size()) < total) {
        for (const auto& rm : collection.mods) {
            bool seen = false;
            for (const ResolvedMod* o : ordered)
                if (o == &rm) {
                    seen = true;
                    break;
                }
            if (!seen)
                ordered.push_back(&rm);
        }
    }

    int done = 0;
    auto report = [&](const std::string& id) {
        ++done;
        if (deps_.on_progress)
            deps_.on_progress(done, total, id);
    };

    for (size_t i = 0; i < ordered.size(); ++i) {
        const ResolvedMod& rm = *ordered[i];
        const std::string id = rm.entry ? rm.entry->id : rm.mod.id;

        // 1. Unresolvable entries: skip with warning, batch continues.
        if (!rm.resolvable) {
            result.mods.push_back({id, ModVerdict::SkippedUnresolvable,
                                   rm.error.empty() ? "Unknown source provider"
                                                    : rm.error});
            ++result.skipped_unresolvable;
            report(id);
            continue;
        }

        // 2. Up-to-date check: expected hash matches the installed hash.
        if (deps_.installed_hash_for && rm.entry) {
            if (auto want = expected_hash(rm.entry->source)) {
                if (auto have = deps_.installed_hash_for(id)) {
                    if (normalize_hash(*have) == *want) {
                        result.mods.push_back({id, ModVerdict::SkippedUpToDate,
                                               "Installed hash matches manifest"});
                        ++result.skipped_up_to_date;
                        report(id);
                        continue;
                    }
                }
            }
        }

        // 3. Run the single-mod pipeline on a copy (the pipeline mutates
        // Mod: files, archive name, state - the collection stays pristine).
        if (!deps_.install_one) {
            result.mods.push_back({id, ModVerdict::Failed,
                                   "No installer wired"});
            ++result.failed;
            report(id);
            continue;
        }
        ::engine::Mod mod = rm.mod;
        const PipelineResult outcome = deps_.install_one(mod);
        if (outcome == PipelineResult::Success) {
            result.mods.push_back({id, ModVerdict::Installed, {}});
            ++result.installed;
            report(id);
            continue;
        }
        if (outcome == PipelineResult::Canceled) {
            result.mods.push_back({id, ModVerdict::Canceled,
                                   "Install canceled by user"});
            result.canceled = true;
            report(id);
            // Everything after a user cancel was never attempted.
            for (size_t j = i + 1; j < ordered.size(); ++j) {
                const std::string rest =
                    ordered[j]->entry ? ordered[j]->entry->id
                                      : ordered[j]->mod.id;
                result.mods.push_back({rest, ModVerdict::NotAttempted,
                                       "Batch stopped after cancel"});
                report(rest);
            }
            break;
        }
        result.mods.push_back({id, ModVerdict::Failed, "Pipeline failed"});
        ++result.failed;
        report(id);
    }

    return result;
}

} // namespace engine::Collection
