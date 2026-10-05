#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::gmmpack {

// Genuine bsdiff (Colin Percival's suffix-sort + ctrl/diff/extra algorithm)
// used for the .gmmpack `algorithm: "bsdiff"` patch payloads.
//
// Framing note: classic BSDIFF40 compresses the ctrl/diff/extra blocks with
// bzip2, which GMM does not vendor on any platform. The layout here mirrors
// BSDIFF40 exactly - 8-byte magic, three LE int64 lengths, then the three
// blocks - but the blocks are zlib-compressed (zlib is already linked into
// the engine on every platform) and the magic is "BSDIFFZ1" so stock
// bspatch binaries fail loudly instead of mis-decoding. `bsdiff_create` is
// what the future pack-creator tool will use to author payloads, so producer
// and consumer can never drift apart.
//
// All functions are Qt-free and thread-safe (no shared state).
// Corrupt/truncated input always yields false + an error string, never a
// crash - patch payloads come from untrusted archives.

// Build a patch that turns `old_data` into `new_data`.
bool bsdiff_create(const uint8_t *old_data, size_t old_len, const uint8_t *new_data,
                   size_t new_len, std::vector<uint8_t> &out_patch, std::string &error);

// Reconstruct the new file from `old_data` and a patch made by
// bsdiff_create. Fails when the patch is corrupt OR when it was made
// against different base content (length/offset checks catch that).
bool bsdiff_apply(const uint8_t *old_data, size_t old_len, const uint8_t *patch,
                  size_t patch_len, std::vector<uint8_t> &out_new, std::string &error);

}  // namespace engine::gmmpack
