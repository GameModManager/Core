#include "engine/gmmpack/schema_validator.h"

#include <algorithm>
#include <regex>
#include <sstream>

namespace engine::gmmpack {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void SchemaValidator::add_error(Diagnostics &diag, const std::string &path,
                                const std::string &message) {
  diag.push_back({Diagnostic::Severity::Error, path, message});
}

const nlohmann::json &SchemaValidator::resolve_ref(const nlohmann::json &ref,
                                                   const nlohmann::json &root_schema) {
  // Only local $ref like "#/$defs/foo"
  std::string ref_str = ref.get<std::string>();
  if (ref_str.rfind("#/$defs/", 0) != 0) {
    static const nlohmann::json null_json;
    return null_json;
  }
  std::string def_name = ref_str.substr(8);
  if (root_schema.contains("$defs") && root_schema["$defs"].contains(def_name)) {
    return root_schema["$defs"][def_name];
  }
  static const nlohmann::json null_json;
  return null_json;
}

// ---------------------------------------------------------------------------
// Main validate entry points
// ---------------------------------------------------------------------------

Diagnostics SchemaValidator::validate(const nlohmann::json &value,
                                      const nlohmann::json &schema) {
  return validate_impl(value, schema, schema, "");
}

Diagnostics SchemaValidator::validate(const nlohmann::json &value,
                                      const nlohmann::json &schema,
                                      const nlohmann::json &root_schema) {
  return validate_impl(value, schema, root_schema, "");
}

// ---------------------------------------------------------------------------
// Type name helper
// ---------------------------------------------------------------------------

static std::string type_name(const nlohmann::json &v) {
  switch (v.type()) {
  case nlohmann::json::value_t::null:
    return "null";
  case nlohmann::json::value_t::boolean:
    return "boolean";
  case nlohmann::json::value_t::number_integer:
  case nlohmann::json::value_t::number_unsigned:
    return "integer";
  case nlohmann::json::value_t::number_float:
    return "number";
  case nlohmann::json::value_t::string:
    return "string";
  case nlohmann::json::value_t::array:
    return "array";
  case nlohmann::json::value_t::object:
    return "object";
  default:
    return "unknown";
  }
}

static bool is_integer(const nlohmann::json &v) {
  return v.is_number_integer();
}

static bool is_number(const nlohmann::json &v) {
  return v.is_number();
}

// Check if JSON value matches JSON Schema type string
static bool matches_type(const nlohmann::json &value,
                         const nlohmann::json &type_schema) {
  if (type_schema.is_string()) {
    std::string t = type_schema.get<std::string>();
    if (t == "string")
      return value.is_string();
    if (t == "integer")
      return is_integer(value);
    if (t == "number")
      return is_number(value);
    if (t == "boolean")
      return value.is_boolean();
    if (t == "array")
      return value.is_array();
    if (t == "object")
      return value.is_object();
    if (t == "null")
      return value.is_null();
    return false;
  }
  if (type_schema.is_array()) {
    for (const auto &t : type_schema) {
      if (matches_type(value, t))
        return true;
    }
    return false;
  }
  return true;  // no type constraint
}

// ---------------------------------------------------------------------------
// Recursive validator
// ---------------------------------------------------------------------------

Diagnostics SchemaValidator::validate_impl(const nlohmann::json &value,
                                           const nlohmann::json &schema,
                                           const nlohmann::json &root_schema,
                                           const std::string &path) {
  Diagnostics diag;

  if (schema.is_null() || schema.empty()) {
    return diag;
  }

  // Resolve $ref
  if (schema.contains("$ref")) {
    const auto &resolved = resolve_ref(schema["$ref"], root_schema);
    if (resolved.is_null()) {
      add_error(diag, path, "unresolved $ref: " + schema["$ref"].get<std::string>());
      return diag;
    }
    return validate_impl(value, resolved, root_schema, path);
  }

  // --- type ---
  if (schema.contains("type")) {
    if (!matches_type(value, schema["type"])) {
      add_error(diag, path,
                "expected type " + schema["type"].dump() + ", got " + type_name(value));
      return diag;  // type mismatch, skip further checks
    }
  }

  // --- const ---
  if (schema.contains("const")) {
    if (value != schema["const"]) {
      add_error(diag, path,
                "expected const " + schema["const"].dump() + ", got " + value.dump());
    }
    return diag;
  }

  // --- enum ---
  if (schema.contains("enum")) {
    bool found = false;
    for (const auto &v : schema["enum"]) {
      if (value == v) {
        found = true;
        break;
      }
    }
    if (!found) {
      add_error(diag, path,
                "value " + value.dump() + " not in enum " + schema["enum"].dump());
    }
    return diag;
  }

  // --- object-specific ---
  if (value.is_object()) {
    // minProperties / maxProperties
    if (schema.contains("minProperties")) {
      if (value.size() < schema["minProperties"].get<size_t>()) {
        add_error(diag, path,
                  "object too short: " + std::to_string(value.size()) +
                      " < minProperties " + schema["minProperties"].dump());
      }
    }
    if (schema.contains("maxProperties")) {
      if (value.size() > schema["maxProperties"].get<size_t>()) {
        add_error(diag, path,
                  "object too long: " + std::to_string(value.size()) +
                      " > maxProperties " + schema["maxProperties"].dump());
      }
    }
    // required
    if (schema.contains("required")) {
      for (const auto &req : schema["required"]) {
        std::string key = req.get<std::string>();
        if (!value.contains(key)) {
          add_error(diag, path, "missing required property: " + key);
        }
      }
    }

    // properties + additionalProperties
    if (schema.contains("properties") || schema.contains("additionalProperties")) {
      const auto &props = schema.contains("properties") ? schema["properties"]
                                                        : nlohmann::json::object();
      bool additional   = true;
      if (schema.contains("additionalProperties")) {
        if (schema["additionalProperties"].is_boolean()) {
          additional = schema["additionalProperties"].get<bool>();
        }
      }

      for (auto it = value.begin(); it != value.end(); ++it) {
        std::string key = it.key();
        if (props.contains(key)) {
          auto child_diag = validate_impl(it.value(), props[key], root_schema,
                                          path.empty() ? key : path + "." + key);
          diag.insert(diag.end(), child_diag.begin(), child_diag.end());
        } else if (!additional) {
          add_error(diag, path, "unknown property: " + key);
        }
      }
    }
  }

  // --- array-specific ---
  if (value.is_array()) {
    if (schema.contains("minItems")) {
      if (value.size() < schema["minItems"].get<size_t>()) {
        add_error(diag, path,
                  "array too short: " + std::to_string(value.size()) + " < " +
                      schema["minItems"].dump());
      }
    }
    if (schema.contains("maxItems")) {
      if (value.size() > schema["maxItems"].get<size_t>()) {
        add_error(diag, path,
                  "array too long: " + std::to_string(value.size()) + " > " +
                      schema["maxItems"].dump());
      }
    }
    if (schema.contains("uniqueItems") && schema["uniqueItems"].get<bool>()) {
      for (size_t i = 0; i < value.size(); ++i) {
        for (size_t j = i + 1; j < value.size(); ++j) {
          if (value[i] == value[j]) {
            add_error(diag, path,
                      "duplicate items at index " + std::to_string(i) + " and " +
                          std::to_string(j));
            break;
          }
        }
      }
    }
    // items validation
    if (schema.contains("items")) {
      for (size_t i = 0; i < value.size(); ++i) {
        std::string child_path = path + "[" + std::to_string(i) + "]";
        auto child_diag =
            validate_impl(value[i], schema["items"], root_schema, child_path);
        diag.insert(diag.end(), child_diag.begin(), child_diag.end());
      }
    }
  }

  // --- string-specific ---
  if (value.is_string()) {
    std::string s = value.get<std::string>();
    if (schema.contains("minLength")) {
      if (s.size() < schema["minLength"].get<size_t>()) {
        add_error(diag, path,
                  "string too short: " + std::to_string(s.size()) + " < " +
                      schema["minLength"].dump());
      }
    }
    if (schema.contains("maxLength")) {
      if (s.size() > schema["maxLength"].get<size_t>()) {
        add_error(diag, path,
                  "string too long: " + std::to_string(s.size()) + " > " +
                      schema["maxLength"].dump());
      }
    }
    if (schema.contains("pattern")) {
      std::string pat = schema["pattern"].get<std::string>();
      try {
        std::regex re(pat);
        if (!std::regex_match(s, re)) {
          add_error(diag, path, "string does not match pattern: " + pat);
        }
      } catch (const std::regex_error &) {
        add_error(diag, path, "invalid regex pattern: " + pat);
      }
    }
    if (schema.contains("format")) {
      std::string fmt = schema["format"].get<std::string>();
      if (fmt == "uuid") {
        // Basic UUID check: 8-4-4-4-12 hex
        static const std::regex uuid_re("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                                        "[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
        if (!std::regex_match(s, uuid_re)) {
          add_error(diag, path, "invalid UUID format");
        }
      } else if (fmt == "uri") {
        // Basic URI check
        if (s.empty() ||
            (s.find("://") == std::string::npos && s.find(":") == std::string::npos)) {
          add_error(diag, path, "invalid URI format");
        }
      } else if (fmt == "date-time") {
        // Basic ISO 8601 check: YYYY-MM-DDTHH:MM:SS[.sss][Z|+HH:MM]
        static const std::regex dt_re("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}"
                                      "(\\.\\d+)?(Z|[+-]\\d{2}:\\d{2})?$");
        if (!std::regex_match(s, dt_re)) {
          add_error(diag, path, "invalid date-time format");
        }
      }
    }
  }

  // --- number-specific ---
  if (is_number(value)) {
    if (schema.contains("minimum")) {
      double min_val = schema["minimum"].get<double>();
      if (value.get<double>() < min_val) {
        add_error(diag, path,
                  "value " + value.dump() + " < minimum " + schema["minimum"].dump());
      }
    }
    if (schema.contains("maximum")) {
      double max_val = schema["maximum"].get<double>();
      if (value.get<double>() > max_val) {
        add_error(diag, path,
                  "value " + value.dump() + " > maximum " + schema["maximum"].dump());
      }
    }
    if (schema.contains("exclusiveMinimum")) {
      double min_val = schema["exclusiveMinimum"].get<double>();
      if (value.get<double>() <= min_val) {
        add_error(diag, path,
                  "value " + value.dump() + " <= exclusiveMinimum " +
                      schema["exclusiveMinimum"].dump());
      }
    }
  }

  // --- oneOf ---
  if (schema.contains("oneOf")) {
    int match_count = 0;
    for (size_t i = 0; i < schema["oneOf"].size(); ++i) {
      auto sub_diag   = validate_impl(value, schema["oneOf"][i], root_schema, path);
      bool has_errors = false;
      for (const auto &d : sub_diag) {
        if (d.severity == Diagnostic::Severity::Error) {
          has_errors = true;
          break;
        }
      }
      if (!has_errors)
        match_count++;
    }
    if (match_count != 1) {
      add_error(diag, path,
                "oneOf: expected exactly 1 match, got " + std::to_string(match_count));
    }
  }

  // --- allOf ---
  if (schema.contains("allOf")) {
    for (size_t i = 0; i < schema["allOf"].size(); ++i) {
      auto sub_diag = validate_impl(value, schema["allOf"][i], root_schema, path);
      diag.insert(diag.end(), sub_diag.begin(), sub_diag.end());
    }
  }

  // --- anyOf ---
  if (schema.contains("anyOf")) {
    int match_count = 0;
    for (size_t i = 0; i < schema["anyOf"].size(); ++i) {
      auto sub_diag   = validate_impl(value, schema["anyOf"][i], root_schema, path);
      bool has_errors = false;
      for (const auto &d : sub_diag) {
        if (d.severity == Diagnostic::Severity::Error) {
          has_errors = true;
          break;
        }
      }
      if (!has_errors)
        match_count++;
    }
    if (match_count == 0) {
      add_error(diag, path, "anyOf: no match found");
    }
  }

  // --- if/then/else ---
  if (schema.contains("if")) {
    auto if_diag   = validate_impl(value, schema["if"], root_schema, path);
    bool if_passed = true;
    for (const auto &d : if_diag) {
      if (d.severity == Diagnostic::Severity::Error) {
        if_passed = false;
        break;
      }
    }
    if (if_passed && schema.contains("then")) {
      auto then_diag = validate_impl(value, schema["then"], root_schema, path);
      diag.insert(diag.end(), then_diag.begin(), then_diag.end());
    } else if (!if_passed && schema.contains("else")) {
      auto else_diag = validate_impl(value, schema["else"], root_schema, path);
      diag.insert(diag.end(), else_diag.begin(), else_diag.end());
    }
  }

  return diag;
}

}  // namespace engine::gmmpack
