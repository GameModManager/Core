#pragma once

// Fetching a Nexus collection revision's own .zip and reading the
// collection.json inside it.
//
// A revision's mod list, status and external resources come from the v2 GraphQL
// query. Everything the curator declared *about* those mods - the per-file
// {path, md5} identities, the FOMOD selections, the load order, the per-mod
// notes, the opaque per-mod tag rules resolve on - exists only in the
// collection.json that ships inside the .zip. This is the half that fetches it.
//
// Engine layer - Qt-free.

#include <string>

namespace engine::Collection::Nexus {

// Download the .zip at `download_link` and return the text of the
// collection.json inside it. `error` is filled with a human-readable reason on
// every failure path and the caller is expected to surface it: the archive is
// an enrichment, not a requirement, so a failure here must leave the metadata
// import intact and merely say what is missing.
//
// Never throws. Both temp files (the .zip and its extraction directory) are
// removed on every path, success included.
bool fetch_collection_json(const std::string &download_link, std::string &out_json,
                           std::string &out_error);

}  // namespace engine::Collection::Nexus