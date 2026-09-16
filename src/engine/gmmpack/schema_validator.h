#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/gmmpack/types.h"

namespace engine::gmmpack {

// JSON Schema draft 2020-12 validator (focused subset for gmmpack schemas).
// Loads schema JSON documents and validates nlohmann::json values against them.
class SchemaValidator {
public:
    // Validate a JSON value against a JSON schema. Returns diagnostics for any
    // violations. Empty diagnostics means valid.
    Diagnostics validate(const nlohmann::json& value,
                         const nlohmann::json& schema);

    // Validate with a named root schema (for $ref resolution across a schema
    // document's own $defs).
    Diagnostics validate(const nlohmann::json& value,
                         const nlohmann::json& schema,
                         const nlohmann::json& root_schema);

private:
    Diagnostics validate_impl(const nlohmann::json& value,
                              const nlohmann::json& schema,
                              const nlohmann::json& root_schema,
                              const std::string& path);

    const nlohmann::json& resolve_ref(const nlohmann::json& ref,
                                      const nlohmann::json& root_schema);

    void add_error(Diagnostics& diag, const std::string& path,
                   const std::string& message);
};

}  // namespace engine::gmmpack
