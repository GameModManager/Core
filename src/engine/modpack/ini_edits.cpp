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

  // ASCII lowercase copy.
  std::string lower(std::string s) {
    for (auto &c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  std::string trim(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
      ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
      --e;
    return s.substr(b, e - b);
  }

  // Case-insensitive match key for target paths, sections, and keys.
  std::string norm_path(const std::string &p) {
    std::string out = p;
    std::replace(out.begin(), out.end(), '\\', '/');
    return lower(trim(out));
  }

  std::string norm_token(const std::string &t) {
    return lower(trim(t));
  }

  bool same_source(const std::optional<std::string> &a,
                   const std::optional<std::string> &b) {
    if (!a.has_value() != !b.has_value())
      return false;
    return !a.has_value() || *a == *b;
  }

  // A ';' starts a trailing comment only at the value start or after
  // whitespace (a ';' inside a value such as a path is kept as-is).
  size_t comment_pos(const std::string &rhs) {
    for (size_t i = 0; i < rhs.size(); ++i) {
      if (rhs[i] != ';')
        continue;
      if (i == 0 || rhs[i - 1] == ' ' || rhs[i - 1] == '\t')
        return i;
    }
    return std::string::npos;
  }

  // The single INI line grammar. Classifies one raw line; shared by the
  // document model (parse_doc) and the tweak content parser
  // (parse_ini_content) so there is exactly one definition of what an INI
  // line means.
  struct IniLine {
    enum class Kind { Blank, Comment, Section, Key, Other };
    Kind kind = Kind::Other;
    std::string section_name;  // Section: name between brackets, trimmed
    std::string key;           // Key: trimmed key text (empty = stray "= x" line)
    std::string value;         // Key: trimmed value, trailing comment stripped
    std::string comment;       // Key: trailing comment incl. ';', may be empty
    size_t eq_pos = std::string::npos;  // Key: '=' offset in the raw line
  };

  IniLine classify_ini_line(const std::string &raw) {
    IniLine line;
    const std::string t = trim(raw);
    if (t.empty()) {
      line.kind = IniLine::Kind::Blank;
    } else if (t[0] == ';' || t[0] == '#') {
      line.kind = IniLine::Kind::Comment;
    } else if (t.front() == '[' && t.back() == ']') {
      line.kind         = IniLine::Kind::Section;
      line.section_name = trim(t.substr(1, t.size() - 2));
    } else if (const size_t eq = raw.find('='); eq != std::string::npos) {
      line.kind       = IniLine::Kind::Key;
      line.eq_pos     = eq;
      line.key        = trim(raw.substr(0, eq));
      std::string rhs = raw.substr(eq + 1);
      if (const size_t c = comment_pos(rhs); c != std::string::npos) {
        line.comment = rhs.substr(c);
        rhs          = rhs.substr(0, c);
      }
      line.value = trim(rhs);
    }
    return line;
  }

  // Line-preserving INI document: every line keeps its kind so apply/retract
  // rewrite values without touching order, comments, or blank lines.
  struct DocLine {
    enum class Kind { Blank, Comment, Section, Key, Other };
    Kind kind = Kind::Other;
    std::string text;     // raw line (no EOL)
    std::string section;  // for Key: enclosing section, raw casing
    std::string lhs;      // for Key: raw text before '=' (key + spacing)
    std::string value;    // for Key: trimmed value without trailing comment
    std::string comment;  // for Key: trailing comment incl. ';', may be empty
  };

  constexpr const char *kEolLf   = "\n";
  constexpr const char *kEolCrlf = "\r\n";

  struct IniDoc {
    std::vector<DocLine> lines;
    std::string eol = kEolLf;
  };

  IniDoc parse_doc(const std::string &text) {
    IniDoc doc;
    doc.eol = text.find(kEolCrlf) != std::string::npos ? kEolCrlf : kEolLf;
    std::string cur;
    std::string section;
    auto flush = [&] {
      const IniLine parsed = classify_ini_line(cur);
      DocLine line;
      line.text = cur;
      switch (parsed.kind) {
      case IniLine::Kind::Blank:
        line.kind = DocLine::Kind::Blank;
        break;
      case IniLine::Kind::Comment:
        line.kind = DocLine::Kind::Comment;
        break;
      case IniLine::Kind::Section:
        line.kind = DocLine::Kind::Section;
        section   = parsed.section_name;
        break;
      case IniLine::Kind::Key:
        // An empty key name is not a setting (e.g. a stray "= x").
        if (parsed.key.empty()) {
          line.kind = DocLine::Kind::Other;
        } else {
          line.kind    = DocLine::Kind::Key;
          line.section = section;
          line.lhs     = cur.substr(0, parsed.eq_pos);
          line.value   = parsed.value;
          line.comment = parsed.comment;
        }
        break;
      case IniLine::Kind::Other:
        break;
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

  std::string render_doc(const IniDoc &doc) {
    std::string out;
    for (const auto &line : doc.lines) {
      out += line.text;
      out += doc.eol;
    }
    return out;
  }

  // First key line in section (both case-insensitive), or npos.
  size_t find_key(const IniDoc &doc, const std::string &section,
                  const std::string &key) {
    const std::string want_s = norm_token(section);
    const std::string want_k = norm_token(key);
    for (size_t i = 0; i < doc.lines.size(); ++i) {
      const auto &line = doc.lines[i];
      if (line.kind != DocLine::Kind::Key)
        continue;
      if (norm_token(line.section) == want_s && norm_token(trim(line.lhs)) == want_k)
        return i;
    }
    return std::string::npos;
  }

  std::string make_key_line(const std::string &key, const std::string &value) {
    return key + "=" + value;
  }

  void set_key_line(DocLine &line, const std::string &value) {
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
      rhs          = rhs.substr(0, c);
    }
    line.value = trim(rhs);
    line.text  = std::move(text);
  }

  // Insert "key=value" at the end of section (before the next section header
  // or EOF), creating the section header when missing.
  void insert_key(IniDoc &doc, const std::string &section, const std::string &key,
                  const std::string &value) {
    const std::string want = norm_token(section);
    size_t header          = std::string::npos;
    size_t insert_at       = doc.lines.size();
    for (size_t i = 0; i < doc.lines.size(); ++i) {
      if (doc.lines[i].kind != DocLine::Kind::Section)
        continue;
      const std::string t    = doc.lines[i].text;
      const std::string name = trim(t.substr(1, t.size() - 2));
      if (norm_token(name) == want) {
        header    = i;
        insert_at = i + 1;  // last matching section wins
      } else if (header != std::string::npos) {
        insert_at = i;  // next section after the last match
        break;
      }
    }
    DocLine line;
    line.kind    = DocLine::Kind::Key;
    line.section = section;
    line.lhs     = key;
    line.value   = value;
    line.text    = make_key_line(key, value);
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

  // Shared retract core: removes every state entry matching pred, restoring
  // prior values (or dropping added lines); user-changed values are left alone
  // and reported as skipped. Non-matching entries pass through untouched.
  template <typename Pred>
  RetractOutcome retract_matching(const std::string &current_text,
                                  const std::vector<AppliedEdit> &state, Pred match) {
    IniDoc doc = parse_doc(current_text);
    RetractOutcome out;
    for (const auto &rec : state) {
      if (!match(rec)) {
        out.applied.push_back(rec);
        continue;
      }
      const size_t at = find_key(doc, rec.section, rec.key);
      if (at == std::string::npos)
        continue;  // already gone: drop the record, nothing to do
      if (trim(doc.lines[at].value) != trim(rec.applied_value)) {
        // User changed it after us: leave the value, drop the record.
        out.skipped.push_back(UserModified{rec.target_file, rec.section, rec.key,
                                           rec.applied_value, doc.lines[at].value});
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

}  // namespace

std::optional<std::vector<IniEdit>>
parse_ini_content(const std::string &content, std::string *error,
                  std::vector<std::string> *warnings) {
  auto fail = [&](const std::string &msg) -> std::optional<std::vector<IniEdit>> {
    if (error)
      *error = msg;
    return std::nullopt;
  };
  std::vector<IniEdit> edits;
  std::map<std::pair<std::string, std::string>, size_t> slot;  // last-wins
  std::string section;
  bool have_section = false;
  std::string cur;
  size_t line_no = 0;
  auto flush     = [&]() -> bool {
    ++line_no;
    const IniLine parsed = classify_ini_line(cur);
    cur.clear();
    switch (parsed.kind) {
    case IniLine::Kind::Blank:
    case IniLine::Kind::Comment:
      break;
    case IniLine::Kind::Section:
      section      = parsed.section_name;
      have_section = true;
      break;
    case IniLine::Kind::Key: {
      if (!have_section) {
        fail("line " + std::to_string(line_no) + ": key outside any section");
        return false;
      }
      if (parsed.key.empty()) {
        fail("line " + std::to_string(line_no) + ": empty key");
        return false;
      }
      const auto k = std::make_pair(norm_token(section), norm_token(parsed.key));
      IniEdit edit{section, parsed.key, parsed.value, std::nullopt, std::nullopt};
      if (auto it = slot.find(k); it != slot.end()) {
        edits[it->second] = edit;  // last wins
        if (warnings)
          warnings->push_back("line " + std::to_string(line_no) + ": duplicate key '" +
                              parsed.key + "' in section '" + section +
                              "': earlier value overridden");
      } else {
        slot[k] = edits.size();
        edits.push_back(std::move(edit));
      }
      break;
    }
    case IniLine::Kind::Other:
      fail("line " + std::to_string(line_no) + ": expected key=value");
      return false;
    }
    return true;
  };
  for (size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n')
      continue;  // CRLF pair handled at '\n'
    if (content[i] == '\n') {
      if (!flush())
        return std::nullopt;
    } else {
      cur.push_back(content[i]);
    }
  }
  if (!cur.empty()) {
    if (!flush())
      return std::nullopt;
  }
  return edits;
}

std::optional<IniEditFile> parse_ini_edit_file(const std::string &json_text,
                                               std::string *error) {
  auto fail = [&](const std::string &msg) -> std::optional<IniEditFile> {
    if (error)
      *error = msg;
    return std::nullopt;
  };
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(json_text);
  } catch (const std::exception &e) {
    return fail(std::string("invalid JSON: ") + e.what());
  }
  if (!doc.is_object())
    return fail("root must be an object");
  if (!doc.contains("targetFile") || !doc["targetFile"].is_string())
    return fail("missing string 'targetFile'");
  if (!doc.contains("tweaks") || !doc["tweaks"].is_array())
    return fail("missing array 'tweaks'");
  IniEditFile file;
  file.target_file = doc["targetFile"].get<std::string>();
  if (trim(file.target_file).empty())
    return fail("'targetFile' must not be empty");
  std::map<std::string, size_t> seen_id;
  std::map<std::string, size_t> seen_name;
  size_t idx = 0;
  for (const auto &t : doc["tweaks"]) {
    auto tfail = [&](const std::string &msg) {
      return fail("tweaks[" + std::to_string(idx) + "]: " + msg);
    };
    if (!t.is_object())
      return tfail("must be an object");
    for (const char *f :
         {"id", "name", "status", "enabled", "sourceModId", "content"}) {
      if (!t.contains(f))
        return tfail(std::string("missing '") + f + "'");
    }
    if (!t["id"].is_string() || !t["name"].is_string() || !t["status"].is_string() ||
        !t["content"].is_string())
      return tfail("'id'/'name'/'status'/'content' must be strings");
    if (!t["enabled"].is_boolean())
      return tfail("'enabled' must be a boolean");
    if (!t["sourceModId"].is_null() && !t["sourceModId"].is_string())
      return tfail("'sourceModId' must be a string or null");
    IniTweak tweak;
    tweak.id   = t["id"].get<std::string>();
    tweak.name = t["name"].get<std::string>();
    if (trim(tweak.id).empty())
      return tfail("'id' must not be empty");
    if (trim(tweak.name).empty())
      return tfail("'name' must not be empty");
    if (auto it = seen_id.find(tweak.id); it != seen_id.end())
      return tfail("duplicate id '" + tweak.id + "' (first at index " +
                   std::to_string(it->second) + ")");
    if (auto it = seen_name.find(tweak.name); it != seen_name.end())
      return tfail("duplicate name '" + tweak.name + "' (first at index " +
                   std::to_string(it->second) + ")");
    seen_id[tweak.id]        = idx;
    seen_name[tweak.name]    = idx;
    const std::string status = t["status"].get<std::string>();
    if (status == "required") {
      tweak.status = TweakStatus::Required;
    } else if (status == "recommended") {
      tweak.status = TweakStatus::Recommended;
    } else {
      return tfail("'status' must be 'required' or 'recommended'");
    }
    tweak.enabled = t["enabled"].get<bool>();
    if (t["sourceModId"].is_string())
      tweak.source_mod_id = t["sourceModId"].get<std::string>();
    tweak.content = t["content"].get<std::string>();
    if (tweak.content.empty())
      return tfail("'content' must not be empty");
    std::string content_err;
    if (!parse_ini_content(tweak.content, &content_err)) {
      return tfail("tweak '" + tweak.id + "': " + content_err);
    }
    file.tweaks.push_back(std::move(tweak));
    ++idx;
  }
  return file;
}

IniDirLoad load_ini_dir(const std::filesystem::path &ini_dir) {
  IniDirLoad out;
  std::error_code ec;
  if (!std::filesystem::exists(ini_dir, ec))
    return out;                                          // pack without INI edits
  std::map<std::string, std::filesystem::path> ordered;  // filename order
  for (std::filesystem::directory_iterator it(ini_dir, ec), end; it != end && !ec;
       it.increment(ec)) {
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
  for (const auto &[name, path] : ordered) {
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

std::vector<MergedTarget> merge_ini_edits(const std::vector<IniEditFile> &files) {
  // Phase A: expand enabled tweaks to edits carrying tweak provenance.
  struct Provenance {
    std::string target_file;  // file's casing
    IniEdit edit;
  };
  std::vector<Provenance> expanded;
  for (const auto &file : files) {
    for (const auto &tweak : file.tweaks) {
      if (!tweak.enabled)
        continue;
      // Content was validated at load; hand-built files must be valid.
      auto edits = parse_ini_content(tweak.content);
      if (!edits)
        continue;
      for (auto &edit : *edits) {
        edit.source_mod_id = tweak.source_mod_id;
        edit.tweak_id      = tweak.id;
        expanded.push_back(Provenance{file.target_file, edit});
      }
    }
  }
  // Phase B: key-level merge, unchanged semantics (pack-author beats
  // mod-attributed, otherwise last in input order wins).
  std::vector<MergedTarget> targets;
  std::map<std::string, size_t> by_target;
  struct KeySlot {
    size_t edit_idx;      // index into MergedTarget::edits
    size_t conflict_idx;  // index into MergedTarget::conflicts, or npos
  };
  std::vector<std::map<std::pair<std::string, std::string>, KeySlot>> slots;
  for (const auto &item : expanded) {
    const std::string norm_t = norm_path(item.target_file);
    size_t ti;
    if (auto it = by_target.find(norm_t); it != by_target.end()) {
      ti = it->second;
    } else {
      ti                = targets.size();
      by_target[norm_t] = ti;
      targets.push_back(MergedTarget{item.target_file, {}, {}});
      slots.emplace_back();
    }
    const IniEdit &edit = item.edit;
    const auto k = std::make_pair(norm_token(edit.section), norm_token(edit.key));
    auto found   = slots[ti].find(k);
    if (found == slots[ti].end()) {
      slots[ti][k] = KeySlot{targets[ti].edits.size(), std::string::npos};
      targets[ti].edits.push_back(edit);
      continue;
    }
    KeySlot &slot              = found->second;
    IniEdit &cur               = targets[ti].edits[slot.edit_idx];
    const bool incoming_author = !edit.source_mod_id.has_value();
    const bool current_author  = !cur.source_mod_id.has_value();
    const IniEdit *winner      = &edit;
    const IniEdit *loser       = &cur;
    if (current_author && !incoming_author) {
      winner = &cur;
      loser  = &edit;
    }
    if (trim(winner->value) == trim(loser->value) &&
        same_source(winner->source_mod_id, loser->source_mod_id))
      continue;  // identical duplicate, no conflict
    if (slot.conflict_idx == std::string::npos) {
      slot.conflict_idx = targets[ti].conflicts.size();
      IniConflict c;
      c.target_file = targets[ti].target_file;
      c.section     = winner->section;
      c.key         = winner->key;
      c.winner      = *winner;
      c.overridden.push_back(*loser);
      c.winner_tweak_id = winner->tweak_id;
      c.overridden_tweak_id.push_back(loser->tweak_id.value_or(std::string{}));
      targets[ti].conflicts.push_back(std::move(c));
    } else {
      IniConflict &c = targets[ti].conflicts[slot.conflict_idx];
      c.overridden.push_back(*loser);
      c.overridden_tweak_id.push_back(loser->tweak_id.value_or(std::string{}));
      c.winner          = *winner;
      c.winner_tweak_id = winner->tweak_id;
      c.section         = winner->section;
      c.key             = winner->key;
    }
    cur = *winner;
  }
  return targets;
}

ApplyOutcome apply_ini_edits(const std::string &current_text,
                             const MergedTarget &merged,
                             const std::vector<AppliedEdit> &prior_state) {
  IniDoc doc               = parse_doc(current_text);
  const std::string norm_t = norm_path(merged.target_file);
  ApplyOutcome out;
  out.text = current_text;

  // Index this target's prior entries by (section, key).
  std::map<std::pair<std::string, std::string>, const AppliedEdit *> prior;
  // New state: start with all of this target's prior entries (skipped
  // user-modified keys keep their record); overwritten below on write.
  std::map<std::pair<std::string, std::string>, AppliedEdit> next;
  for (const auto &p : prior_state) {
    if (norm_path(p.target_file) != norm_t)
      continue;
    const auto k = std::make_pair(norm_token(p.section), norm_token(p.key));
    prior[k]     = &p;
    next[k]      = p;
  }

  for (const auto &edit : merged.edits) {
    const auto k    = std::make_pair(norm_token(edit.section), norm_token(edit.key));
    const size_t at = find_key(doc, edit.section, edit.key);
    const std::string current = at == std::string::npos ? "" : doc.lines[at].value;
    const bool exists         = at != std::string::npos;
    if (auto it = prior.find(k); it != prior.end()) {
      const AppliedEdit *p = it->second;
      if (exists && trim(current) != trim(p->applied_value) &&
          trim(current) != trim(edit.value)) {
        // User (or game) changed the value after us: flag, keep.
        out.user_modified.push_back(UserModified{merged.target_file, edit.section,
                                                 edit.key, p->applied_value, current});
        continue;
      }
    }
    AppliedEdit rec;
    rec.target_file = merged.target_file;
    rec.section     = edit.section;
    rec.key         = edit.key;
    if (auto it = prior.find(k); it != prior.end()) {
      // Re-apply keeps the ORIGINAL pre-first-apply value so a later
      // retract restores what was there before us, not an intermediate.
      rec.had_prior   = it->second->had_prior;
      rec.prior_value = it->second->prior_value;
    } else {
      rec.had_prior   = exists;
      rec.prior_value = current;
    }
    rec.applied_value = edit.value;
    rec.source_mod_id = edit.source_mod_id;
    rec.tweak_id      = edit.tweak_id;
    if (exists) {
      if (trim(current) != trim(edit.value))
        set_key_line(doc.lines[at], edit.value);
    } else {
      insert_key(doc, edit.section, edit.key, edit.value);
    }
    next[k] = std::move(rec);
  }
  for (const auto &[k, rec] : next)
    out.applied.push_back(rec);
  // Carry other targets' entries through untouched.
  for (const auto &p : prior_state) {
    if (norm_path(p.target_file) != norm_t)
      out.applied.push_back(p);
  }
  out.text = render_doc(doc);
  return out;
}

RetractOutcome retract_ini_edits(const std::string &current_text,
                                 const std::vector<AppliedEdit> &state,
                                 const std::optional<std::string> &source) {
  return retract_matching(current_text, state, [&](const AppliedEdit &rec) {
    return same_source(rec.source_mod_id, source);
  });
}

RetractOutcome retract_ini_tweak(const std::string &current_text,
                                 const std::vector<AppliedEdit> &state,
                                 const std::string &tweak_id) {
  return retract_matching(current_text, state, [&](const AppliedEdit &rec) {
    return rec.tweak_id.has_value() && *rec.tweak_id == tweak_id;
  });
}

std::string applied_state_to_json(const std::vector<AppliedEdit> &state) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &r : state) {
    nlohmann::json o;
    o["targetFile"]   = r.target_file;
    o["section"]      = r.section;
    o["key"]          = r.key;
    o["hadPrior"]     = r.had_prior;
    o["priorValue"]   = r.prior_value;
    o["appliedValue"] = r.applied_value;
    if (r.source_mod_id.has_value())
      o["sourceModId"] = *r.source_mod_id;
    else
      o["sourceModId"] = nullptr;
    if (r.tweak_id.has_value())
      o["tweakId"] = *r.tweak_id;
    else
      o["tweakId"] = nullptr;
    arr.push_back(std::move(o));
  }
  return arr.dump(2);
}

std::vector<AppliedEdit> applied_state_from_json(const std::string &json_text,
                                                 std::string *error) {
  auto fail = [&](const std::string &msg) {
    if (error)
      *error = msg;
    return std::vector<AppliedEdit>{};
  };
  nlohmann::json arr;
  try {
    arr = nlohmann::json::parse(json_text);
  } catch (const std::exception &e) {
    return fail(std::string("invalid JSON: ") + e.what());
  }
  if (!arr.is_array())
    return fail("root must be an array");
  std::vector<AppliedEdit> out;
  size_t idx = 0;
  for (const auto &o : arr) {
    if (!o.is_object())
      return fail("entry " + std::to_string(idx) + ": must be object");
    AppliedEdit r;
    try {
      r.target_file   = o.at("targetFile").get<std::string>();
      r.section       = o.at("section").get<std::string>();
      r.key           = o.at("key").get<std::string>();
      r.had_prior     = o.at("hadPrior").get<bool>();
      r.prior_value   = o.at("priorValue").get<std::string>();
      r.applied_value = o.at("appliedValue").get<std::string>();
      const auto &src = o.at("sourceModId");
      if (src.is_string())
        r.source_mod_id = src.get<std::string>();
      else if (!src.is_null())
        return fail("entry " + std::to_string(idx) +
                    ": 'sourceModId' must be string or null");
      // tweakId is additive: pre-tweak state files omit it entirely.
      if (o.contains("tweakId")) {
        const auto &tw = o.at("tweakId");
        if (tw.is_string())
          r.tweak_id = tw.get<std::string>();
        else if (!tw.is_null())
          return fail("entry " + std::to_string(idx) +
                      ": 'tweakId' must be string or null");
      }
    } catch (const std::exception &e) {
      return fail("entry " + std::to_string(idx) + ": " + e.what());
    }
    out.push_back(std::move(r));
    ++idx;
  }
  return out;
}

}  // namespace engine::modpack
