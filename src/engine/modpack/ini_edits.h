#pragma once

// INI tweak engine for .gmmpack modpacks (ini/<targetFile>.json, see
// input/ini.schema.json in the Workspace repo).
//
// Each edit file carries {targetFile, tweaks[]} where every tweak has
// {id, name, status, enabled, sourceModId, content}. content is plain INI
// text ([section] headers plus key=value lines); the engine parses it into
// key/value edits internally, so there is exactly ONE INI grammar in this
// file (see parse_ini_content). sourceModId null (empty here) means a
// pack-author tweak; non-null attributes the tweak to that mod so it can be
// auto-retracted when the owning mod is removed.
//
// Matching is ASCII case-insensitive for target paths, sections, and keys.
// The engine is source-agnostic: it never interprets mod ids, it only
// compares them. Qt-free; persists no state itself - the caller owns the
// AppliedEdit vector (see applied_state_to_json()) and passes it back to
// apply/retract.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace engine::modpack {

enum class TweakStatus { Required, Recommended };

// One named, toggleable unit from a tweak's content after parsing.
// source_mod_id nullopt = pack-author edit; tweak_id names the tweak this
// edit was expanded from (empty when the edit was built by hand, e.g. in
// tests or from pre-tweak state).
struct IniEdit {
    std::string section;
    std::string key;
    std::string value;
    std::optional<std::string> source_mod_id;
    std::optional<std::string> tweak_id;
};

// One author-defined tweak: plain INI text plus its metadata.
struct IniTweak {
    std::string id;  // stable slug; the key for diffing/state/retract
    std::string name;  // human-readable label
    TweakStatus status = TweakStatus::Recommended;
    bool enabled = true;  // author default; instance state owns it later
    std::optional<std::string> source_mod_id;
    std::string content;  // plain INI text
};

// Parsed content of one ini/<targetFile>.json file.
struct IniEditFile {
    std::string target_file;
    std::vector<IniTweak> tweaks;
};

// Same target+section+key edited more than once with different values.
// winner is what merge kept; overridden lists the losers in input order.
// winner_tweak_id names the winning tweak; overridden_tweak_id runs parallel
// to overridden[] (empty entries = edits without tweak provenance).
struct IniConflict {
    std::string target_file;
    std::string section;
    std::string key;
    IniEdit winner;
    std::vector<IniEdit> overridden;
    std::optional<std::string> winner_tweak_id;
    std::vector<std::string> overridden_tweak_id;
};

// One target file after merging: winners plus the conflicts between them.
struct MergedTarget {
    std::string target_file;  // first-seen casing
    std::vector<IniEdit> edits;
    std::vector<IniConflict> conflicts;
};

// What apply wrote for one key: previous value (or absent) plus the value
// and source we set. The caller persists this vector; retract and later
// apply calls need it to restore values and to detect user edits.
// tweak_id is additive: absent (nullopt) for pre-tweak state files.
struct AppliedEdit {
    std::string target_file;
    std::string section;
    std::string key;
    bool had_prior = false;
    std::string prior_value;
    std::string applied_value;
    std::optional<std::string> source_mod_id;
    std::optional<std::string> tweak_id;
};

// A key whose on-disk value no longer matches what we recorded as applied:
// the user (or game) changed it after us. Flagged, never overwritten.
struct UserModified {
    std::string target_file;
    std::string section;
    std::string key;
    std::string expected;  // value we last applied
    std::string actual;  // value found on disk
};

struct ApplyOutcome {
    std::string text;  // new INI content
    std::vector<AppliedEdit> applied;  // full new state for this target
    std::vector<UserModified> user_modified;  // flagged, left untouched
};

struct RetractOutcome {
    std::string text;  // new INI content
    std::vector<AppliedEdit> applied;  // remaining state for this target
    std::vector<UserModified> skipped;  // owned but user-changed, left alone
};

// Loading of the ini/ directory inside an unpacked .gmmpack. Missing dir is
// fine (pack without INI edits); per-file failures are collected, not fatal.
struct IniDirLoad {
    std::vector<IniEditFile> files;
    std::vector<std::string> errors;  // "file: reason"
};

// Parse one tweak's plain INI text into key/value edits. Blank lines and
// ;/# comments are skipped; values use the same trailing-comment rule as the
// document model. Duplicate keys within one content resolve last-wins with
// one warning per key. Returns nullopt on invalid input (a key outside any
// section, a line without '=', an empty key); *error gets the reason.
// This is the single INI grammar: every INI parsing routes through here.
[[nodiscard]] std::optional<std::vector<IniEdit>> parse_ini_content(
    const std::string& content, std::string* error = nullptr,
    std::vector<std::string>* warnings = nullptr);

// Parse one ini/<targetFile>.json document. Returns nullopt on invalid input
// (bad JSON, missing/wrong-typed fields, duplicate tweak id/name, empty
// content, unparsable tweak content); *error gets the reason.
[[nodiscard]] std::optional<IniEditFile> parse_ini_edit_file(
    const std::string& json_text, std::string* error = nullptr);

// Read every *.json file in ini_dir (non-recursive, filename order).
[[nodiscard]] IniDirLoad load_ini_dir(const std::filesystem::path& ini_dir);

// Merge tweaks from all files. Disabled tweaks are skipped; enabled tweaks
// expand to edits carrying tweak provenance. Files targeting the same path
// (case-insensitive) merge into one MergedTarget; same section+key resolves
// with source attribution: a pack-author edit beats a mod-attributed one,
// otherwise the last edit in input order wins. Same-valued duplicates are
// not conflicts.
[[nodiscard]] std::vector<MergedTarget> merge_ini_edits(
    const std::vector<IniEditFile>& files);

// Apply one target's merged edits to its current INI text, preserving line
// order, comments, and blank lines (CRLF preserved when present). Keys whose
// current value differs from both the recorded state and the desired value
// are reported as user_modified and left alone. prior_state holds this
// target's AppliedEdit entries (entries for other targets are ignored); the
// returned applied vector is the full new state to persist.
[[nodiscard]] ApplyOutcome apply_ini_edits(
    const std::string& current_text, const MergedTarget& merged,
    const std::vector<AppliedEdit>& prior_state = {});

// Retract one source's edits from INI text. source nullopt retracts
// pack-author edits, otherwise the named mod's. A retracted key is restored
// to its prior value (or its line removed when we added it); keys the user
// changed since are left alone and reported as skipped. Non-matching state
// entries (other targets/sources) pass through into applied untouched.
[[nodiscard]] RetractOutcome retract_ini_edits(
    const std::string& current_text, const std::vector<AppliedEdit>& state,
    const std::optional<std::string>& source);

// Retract one tweak's edits from INI text. Only state entries carrying that
// tweak_id are touched; everything else (including entries without tweak
// provenance) passes through untouched. Same restore/skip semantics as
// retract_ini_edits. Disabling a tweak must go through here: re-merging
// without the tweak and re-applying would leave its keys lingering with no
// remaining enabled writer.
[[nodiscard]] RetractOutcome retract_ini_tweak(
    const std::string& current_text, const std::vector<AppliedEdit>& state,
    const std::string& tweak_id);

// JSON round-trip for persisting the AppliedEdit vector between sessions.
// tweakId is omitted as null when absent; readers default a missing tweakId
// to empty so pre-tweak state files still load.
[[nodiscard]] std::string applied_state_to_json(
    const std::vector<AppliedEdit>& state);
[[nodiscard]] std::vector<AppliedEdit> applied_state_from_json(
    const std::string& json_text, std::string* error = nullptr);

}  // namespace engine::modpack
