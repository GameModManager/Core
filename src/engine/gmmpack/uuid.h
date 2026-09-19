#pragma once

// Shared UUID v4 generator (RFC 4122, random).
//
// Owned by the gmmpack module but usable anywhere in the engine: instance
// identity (Instance::Info::modpack_id) and pack manifests both need it.
// Previously a file-local helper in packer.cpp; moved here so the export
// call-site can lazily assign a stable id without depending on the packer.

#include <string>

namespace engine {

// Random UUID v4 string ("xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx", with the
// RFC 4122 variant bits). Never empty; each call returns a fresh value.
std::string generate_uuid_v4();

}  // namespace engine
