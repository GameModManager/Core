#include "engine/modpack/ini_edits.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace engine::modpack {
namespace {

constexpr const char* kEolLf = "\n";
constexpr const char* kEolCrlf = "\r\n";

// ASCII lowercase copy.
std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() &&
           std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

// Case-insensitive match key for target paths, sections, and keys.
std::string norm_path(const std::string& p) {
    std::string out = p;
    std::replace(out.begin(), out.end(), '\\', '/');
    return lower(trim(out));
}

std::string norm_token(const std::string& t) { return lower(trim(t)); }

bool same_source(const std::optional<std::string>& a,
                 const std::optional<std::string>& b) {
    if (!a.has_value() != !b.has_value())
        return false;
    return !a.has_value() || *a == *b;
}

// A ';' starts a trailing comment only at the value start or after
// whitespace (a ';' inside a value such as a path is kept as-is).
size_t comment_pos(const std::string& rhs) {
    for (size_t i = 0; i < rhs.size(); ++i) {
        if (rhs[i] != ';')
            continue;
        if (i == 0 || rhs[i - 1] == ' ' || rhs[i - 1] == '\t')
            return i;
    }
    return std::string::npos;
}

// Line-preserving INI document: every line keeps its kind so apply/retract
// rewrite values without touching order, comments, or blank lines.
struct DocLine {
    enum class Kind { Blank, Comment, Section, Key, Other };
    Kind kind = Kind::Other;
    std::string text;      // raw line (no EOL)
    std::string section;   // for Key: enclosing section, raw casing
    std::string lhs;       // for Key: raw text before '=' (key + spacing)
    std::string value;     // for Key: trimmed value without trailing comment
    std::string comment;   // for Key: trailing comment incl. ';', may be empty
};

struct IniDoc {
    std::vector<DocLine> lines;
    std::string eol = kEolLf;
};

IniDoc parse_doc(const std::string& text) {
    IniDoc doc;
    doc.eol = text.find(kEolCrlf) != std::string::npos ? kEolCrlf : kEolLf;
    std::string cur;
    std::string section;
    auto flush = [&] {
        DocLine line;
        line.text = cur;
        const std::string t = trim(cur);
        if (t.empty()) {
            line.kind = DocLine::Kind::Blank;
        } else if (t[0] == ';' || t[0] == '#') {
            line.kind = DocLine::Kind::Comment;
        } else if (t.front() == '[' && t.back() == ']') {
            line.kind = DocLine::Kind::Section;
            section = trim(t.substr(1, t.size() - 2));
        } else if (const size_t eq = cur.find('='); eq != std::string::npos) {
            line.kind = DocLine::Kind::Key;
            line.section = section;
            line.lhs = cur.substr(0, eq);
            // An empty key name is not a setting (e.g. a stray "= x" line).
            if (trim(line.lhs).empty()) {
                line.kind = DocLine::Kind::Other;
                line.lhs.clear();
            } else {
                std::string rhs = cur.substr(eq + 1);
                if (const size_t c = comment_pos(rhs); c != std::string::npos) {
                    line.comment = rhs.substr(c);
                    rhs = rhs.substr(0, c);
                }
                line.value = trim(rhs);
            }
        }
        doc.lines.push_back(std::move(line));
        cur.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue;  // CRLF pair handled at '\n'
        if (text[i] == '\n') {
            flush();
        } else {
            cur.push_back(text[i]);
        }
    }
    // A trailing newline terminates the last line; leftover text without a
    // terminator is still a line (missing final EOL is preserved as-is).
    if (!cur.empty() || (!text.empty() && text.back() == '\n'))
        flush();
    return doc;
}

std::string render_doc(const IniDoc& doc) {
    std::string out;
    for (const auto& line : doc.lines) {
        out += line.text;
        out += doc.eol;
    }
    return out;
}

// First key line in section (both case-insensitive), or npos.
size_t find_key(const IniDoc& doc, const std::string& section,
                const std::string& key) {
    const std::string want_s = norm_token(section);
    const std::string want_k = norm_token(key);
    for (size_t i = 0; i < doc.lines.size(); ++i) {
        const auto& line = doc.lines[i];
        if (line.kind != DocLine::Kind::Key)
            continue;
        if (norm_token(line.section) == want_s &&
            norm_token(trim(line.lhs)) == want_k)
            return i;
    }
    return std::string::npos;
}

std::string make_key_line(const std::string& key, const std::string& value) {
    return key + "=" + value;
}

void set_key_line(DocLine& line, const std::string& value) {
    // Keep the original "key = " prefix and trailing comment, swap the value.
    std::string text = line.lhs + "=" + value;
    if (!line.comment.empty()) {
        if (!value.empty() && text.back() != ' ' && text.back() != '\t')
            text += ' ';
        text += line.comment;
    }
    // Re-derive the cached fields from the new text.
    const size_t eq = text.find('=');
    std::string rhs = text.substr(eq + 1);
    line.comment.clear();
    if (const size_t c = comment_pos(rhs); c != std::string::npos) {
        line.comment = rhs.substr(c);
        rhs = rhs.substr(0, c);
    }
    line.value = trim(rhs);
    line.text = std::move(text);
}

// Insert "key=value" at the end of section (before the next section header
// or EOF), creating the section header when missing.
void insert_key(IniDoc& doc, const std::string& section, const std::string& key,
                const std::string& value) {
    const std::string want = norm_token(section);
    size_t header = std::string::npos;
    size_t insert_at = doc.lines.size();
    for (size_t i = 0; i < doc.lines.size(); ++i) {
        if (doc.lines[i].kind != DocLine::Kind::Section)
            continue;
        const std::string t = doc.lines[i].text;
        const std::string name = trim(t.substr(1, t.size() - 2));
        if (norm_token(name) == want) {
            header = i;
            insert_at = i + 1;  // last matching section wins
        } else if (header != std::string::npos) {
            insert_at = i;  // next section after the last match
            break;
        }
    }
    DocLine line;
    line.kind = DocLine::Kind::Key;
    line.section = section;
    line.lhs = key;
    line.value = value;
    line.text = make_key_line(key, value);
    if (header == std::string::npos) {
        if (!doc.lines.empty()) {
            DocLine blank;
            blank.kind = DocLine::Kind::Blank;
            doc.lines.push_back(blank);
        }
        DocLine hdr;
        hdr.kind = DocLine::Kind::Section;
        hdr.text = "[" + section + "]";
        doc.lines.push_back(hdr);
        doc.lines.push_back(line);
    } else {
        doc.lines.insert(doc.lines.begin() + insert_at, line);
    }
}

}  // namespace

std::optional<IniEditFile> parse_ini_edit_file(const std::string& json_text,
                                               std::string* error) {
    auto fail = [&](const std::string& msg) -> std::optional<IniEditFile> {
        if (error)
            *error = msg;
        return std::nullopt;
    };
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json_text);
    } catch (const std::exception& e) {
        return fail(std::string("invalid JSON: ") + e.what());
    }
    if (!doc.is_object())
        return fail("root must be an object");
    if (!doc.contains("targetFile") || !doc["targetFile"].is_string())
        return fail("missing string 'targetFile'");
    if (!doc.contains("edits") || !doc["edits"].is_array())
        return fail("missing array 'edits'");
    IniEditFile file;
    file.target_file = doc["targetFile"].get<std::string>();
    if (trim(file.target_file).empty())
        return fail("'targetFile' must not be empty");
    size_t idx = 0;
    for (const auto& e : doc["edits"]) {
        auto efail = [&](const std::string& msg) {
            return fail("edits[" + std::to_string(idx) + "]: " + msg);
        };
        if (!e.is_object())
            return efail("must be an object");
        for (const char* f : {"section", "key", "value", "sourceModId"}) {
            if (!e.contains(f))
                return efail(std::string("missing '") + f + "'");
        }
        if (!e["section"].is_string() || !e["key"].is_string() ||
            !e["value"].is_string())
            return efail("'section'/'key'/'value' must be strings");
        if (!e["sourceModId"].is_null() && !e["sourceModId"].is_string())
            return efail("'sourceModId' must be a string or null");
        IniEdit edit;
        edit.section = e["section"].get<std::string>();
        edit.key = e["key"].get<std::string>();
        edit.value = e["value"].get<std::string>();
        if (trim(edit.key).empty())
            return efail("'key' must not be empty");
        if (e["sourceModId"].is_string())
            edit.source_mod_id = e["sourceModId"].get<std::string>();
        file.edits.push_back(std::move(edit));
        ++idx;
    }
    return file;
}

IniDirLoad load_ini_dir(const std::filesystem::path& ini_dir) {
    IniDirLoad out;
    std::error_code ec;
    if (!std::filesystem::exists(ini_dir, ec))
        return out;  // pack without INI edits
    std::map<std::string, std::filesystem::path> ordered;  // filename order
    for (std::filesystem::directory_iterator it(ini_dir, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        if (it->path().extension() != ".json")
            continue;
        ordered[it->path().filename().string()] = it->path();
    }
    if (ec) {
        out.errors.push_back(ini_dir.string() + ": cannot list directory");
        return out;
    }
    for (const auto& [name, path] : ordered) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            out.errors.push_back(name + ": cannot open file");
            continue;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string err;
        if (auto file = parse_ini_edit_file(ss.str(), &err)) {
            out.files.push_back(std::move(*file));
        } else {
            out.errors.push_back(name + ": " + err);
        }
    }
    return out;
}

std::vector<MergedTarget> merge_ini_edits(
    const std::vector<IniEditFile>& files) {
    // Group files by normalized target, keeping first-seen order and casing.
    std::vector<MergedTarget> targets;
    std::map<std::string, size_t> by_target;
    struct KeySlot {
        size_t edit_idx;  // index into MergedTarget::edits
        size_t conflict_idx;  // index into MergedTarget::conflicts, or npos
    };
    std::vector<std::map<std::pair<std::string, std::string>, KeySlot>> slots;
    for (const auto& file : files) {
        const std::string norm_t = norm_path(file.target_file);
        size_t ti;
        if (auto it = by_target.find(norm_t); it != by_target.end()) {
            ti = it->second;
        } else {
            ti = targets.size();
            by_target[norm_t] = ti;
            targets.push_back(MergedTarget{file.target_file, {}, {}});
            slots.emplace_back();
        }
        for (const auto& edit : file.edits) {
            const auto k =
                std::make_pair(norm_token(edit.section), norm_token(edit.key));
            auto found = slots[ti].find(k);
            if (found == slots[ti].end()) {
                slots[ti][k] = KeySlot{targets[ti].edits.size(),
                                       std::string::npos};
                targets[ti].edits.push_back(edit);
                continue;
            }
            // Same target+section+key again: pack-author beats mod-attributed,
            // otherwise last in input order wins.
            KeySlot& slot = found->second;
            IniEdit& cur = targets[ti].edits[slot.edit_idx];
            const bool incoming_author = !edit.source_mod_id.has_value();
            const bool current_author = !cur.source_mod_id.has_value();
            const IniEdit* winner = &edit;
            const IniEdit* loser = &cur;
            if (current_author && !incoming_author) {
                winner = &cur;
                loser = &edit;
            }
            if (trim(winner->value) == trim(loser->value) &&
                same_source(winner->source_mod_id, loser->source_mod_id))
                continue;  // identical duplicate, no conflict
            if (slot.conflict_idx == std::string::npos) {
                slot.conflict_idx = targets[ti].conflicts.size();
                IniConflict c;
                c.target_file = targets[ti].target_file;
                c.section = winner->section;
                c.key = winner->key;
                c.winner = *winner;
                c.overridden.push_back(*loser);
                targets[ti].conflicts.push_back(std::move(c));
            } else {
                IniConflict& c = targets[ti].conflicts[slot.conflict_idx];
                c.overridden.push_back(*loser);
                c.winner = *winner;
                c.section = winner->section;
                c.key = winner->key;
            }
            cur = *winner;
        }
    }
    return targets;
}

ApplyOutcome apply_ini_edits(const std::string& current_text,
                             const MergedTarget& merged,
                             const std::vector<AppliedEdit>& prior_state) {
    IniDoc doc = parse_doc(current_text);
    const std::string norm_t = norm_path(merged.target_file);
    ApplyOutcome out;
    out.text = current_text;

    // Index this target's prior entries by (section, key).
    std::map<std::pair<std::string, std::string>, const AppliedEdit*> prior;
    // New state: start with all of this target's prior entries (skipped
    // user-modified keys keep their record); overwritten below on write.
    std::map<std::pair<std::string, std::string>, AppliedEdit> next;
    for (const auto& p : prior_state) {
        if (norm_path(p.target_file) != norm_t)
            continue;
        const auto k =
            std::make_pair(norm_token(p.section), norm_token(p.key));
        prior[k] = &p;
        next[k] = p;
    }

    for (const auto& edit : merged.edits) {
        const auto k =
            std::make_pair(norm_token(edit.section), norm_token(edit.key));
        const size_t at = find_key(doc, edit.section, edit.key);
        const std::string current =
            at == std::string::npos ? "" : doc.lines[at].value;
        const bool exists = at != std::string::npos;
        if (auto it = prior.find(k); it != prior.end()) {
            const AppliedEdit* p = it->second;
            if (exists && trim(current) != trim(p->applied_value) &&
                trim(current) != trim(edit.value)) {
                // User (or game) changed the value after us: flag, keep.
                out.user_modified.push_back(UserModified{
                    merged.target_file, edit.section, edit.key,
                    p->applied_value, current});
                continue;
            }
        }
        AppliedEdit rec;
        rec.target_file = merged.target_file;
        rec.section = edit.section;
        rec.key = edit.key;
        if (auto it = prior.find(k); it != prior.end()) {
            // Re-apply keeps the ORIGINAL pre-first-apply value so a later
            // retract restores what was there before us, not an intermediate.
            rec.had_prior = it->second->had_prior;
            rec.prior_value = it->second->prior_value;
        } else {
            rec.had_prior = exists;
            rec.prior_value = current;
        }
        rec.applied_value = edit.value;
        rec.source_mod_id = edit.source_mod_id;
        if (exists) {
            if (trim(current) != trim(edit.value))
                set_key_line(doc.lines[at], edit.value);
        } else {
            insert_key(doc, edit.section, edit.key, edit.value);
        }
        next[k] = std::move(rec);
    }
    for (const auto& [k, rec] : next)
        out.applied.push_back(rec);
    // Carry other targets' entries through untouched.
    for (const auto& p : prior_state) {
        if (norm_path(p.target_file) != norm_t)
            out.applied.push_back(p);
    }
    out.text = render_doc(doc);
    return out;
}

RetractOutcome retract_ini_edits(
    const std::string& current_text, const std::vector<AppliedEdit>& state,
    const std::optional<std::string>& source) {
    IniDoc doc = parse_doc(current_text);
    RetractOutcome out;
    for (const auto& rec : state) {
        if (!same_source(rec.source_mod_id, source)) {
            out.applied.push_back(rec);  // other target/source: untouched
            continue;
        }
        const size_t at = find_key(doc, rec.section, rec.key);
        if (at == std::string::npos)
            continue;  // already gone: drop the record, nothing to do
        if (trim(doc.lines[at].value) != trim(rec.applied_value)) {
            // User changed it after us: leave the value, drop the record.
            out.skipped.push_back(UserModified{rec.target_file, rec.section,
                                              rec.key, rec.applied_value,
                                              doc.lines[at].value});
            continue;
        }
        if (rec.had_prior) {
            set_key_line(doc.lines[at], rec.prior_value);
        } else {
            doc.lines.erase(doc.lines.begin() + at);
        }
    }
    out.text = render_doc(doc);
    return out;
}

std::string applied_state_to_json(const std::vector<AppliedEdit>& state) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : state) {
        nlohmann::json o;
        o["targetFile"] = r.target_file;
        o["section"] = r.section;
        o["key"] = r.key;
        o["hadPrior"] = r.had_prior;
        o["priorValue"] = r.prior_value;
        o["appliedValue"] = r.applied_value;
        if (r.source_mod_id.has_value())
            o["sourceModId"] = *r.source_mod_id;
        else
            o["sourceModId"] = nullptr;
        arr.push_back(std::move(o));
    }
    return arr.dump(2);
}

std::vector<AppliedEdit> applied_state_from_json(const std::string& json_text,
                                                 std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error)
            *error = msg;
        return std::vector<AppliedEdit>{};
    };
    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(json_text);
    } catch (const std::exception& e) {
        return fail(std::string("invalid JSON: ") + e.what());
    }
    if (!arr.is_array())
        return fail("root must be an array");
    std::vector<AppliedEdit> out;
    size_t idx = 0;
    for (const auto& o : arr) {
        if (!o.is_object())
            return fail("entry " + std::to_string(idx) + ": must be object");
        AppliedEdit r;
        try {
            r.target_file = o.at("targetFile").get<std::string>();
            r.section = o.at("section").get<std::string>();
            r.key = o.at("key").get<std::string>();
            r.had_prior = o.at("hadPrior").get<bool>();
            r.prior_value = o.at("priorValue").get<std::string>();
            r.applied_value = o.at("appliedValue").get<std::string>();
            const auto& src = o.at("sourceModId");
            if (src.is_string())
                r.source_mod_id = src.get<std::string>();
            else if (!src.is_null())
                return fail("entry " + std::to_string(idx) +
                            ": 'sourceModId' must be string or null");
        } catch (const std::exception& e) {
            return fail("entry " + std::to_string(idx) + ": " + e.what());
        }
        out.push_back(std::move(r));
        ++idx;
    }
    return out;
}

}  // namespace engine::modpack
