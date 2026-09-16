#include "engine/gmmpack/bsdiff.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <zlib.h>

namespace engine::gmmpack {
namespace {

constexpr char kMagic[8] = {'B', 'S', 'D', 'I', 'F', 'F', 'Z', '1'};
constexpr size_t kHeaderLen = 32;

void put_le64(std::vector<uint8_t>& out, int64_t v) {
  uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((u >> (i * 8)) & 0xFF));
  }
}

bool get_le64(const uint8_t* p, size_t avail, int64_t& v) {
  if (avail < 8) return false;
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) {
    u |= static_cast<uint64_t>(p[i]) << (i * 8);
  }
  v = static_cast<int64_t>(u);
  return true;
}

bool zlib_compress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                   std::string& error) {
  uLong bound = compressBound(static_cast<uLong>(in.size()));
  out.resize(bound);
  uLong dest_len = bound;
  int rc = compress2(out.data(), &dest_len, in.data(),
                     static_cast<uLong>(in.size()), Z_DEFAULT_COMPRESSION);
  if (rc != Z_OK) {
    error = "zlib compress failed";
    return false;
  }
  out.resize(dest_len);
  return true;
}

bool zlib_decompress(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out,
                     std::string& error) {
  out.clear();
  z_stream strm{};
  strm.next_in = const_cast<Bytef*>(in);
  strm.avail_in = static_cast<uInt>(in_len);
  if (inflateInit(&strm) != Z_OK) {
    error = "zlib inflateInit failed";
    return false;
  }
  std::vector<uint8_t> chunk(65536);
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    strm.next_out = chunk.data();
    strm.avail_out = static_cast<uInt>(chunk.size());
    rc = inflate(&strm, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) {
      inflateEnd(&strm);
      error = "zlib inflate failed: corrupt patch block";
      return false;
    }
    out.insert(out.end(), chunk.data(),
               chunk.data() + (chunk.size() - strm.avail_out));
    if (out.size() > static_cast<size_t>(1) << 31) {
      inflateEnd(&strm);
      error = "patch block unreasonably large";
      return false;
    }
  }
  inflateEnd(&strm);
  return true;
}

// Suffix array by prefix doubling (O(n log^2 n)).
// ponytail: QSufSort (linear) if profiling ever blames patch creation.
std::vector<int64_t> build_suffix_array(const uint8_t* s, size_t n) {
  std::vector<int64_t> sa(n), rank(n), tmp(n);
  std::iota(sa.begin(), sa.end(), 0);
  for (size_t i = 0; i < n; ++i) rank[i] = s[i];
  for (size_t k = 1; k < n; k *= 2) {
    auto cmp = [&](int64_t i, int64_t j) {
      if (rank[i] != rank[j]) return rank[i] < rank[j];
      int64_t ri = (i + k < n) ? rank[i + k] : -1;
      int64_t rj = (j + k < n) ? rank[j + k] : -1;
      return ri < rj;
    };
    std::sort(sa.begin(), sa.end(), cmp);
    tmp[sa[0]] = 0;
    for (size_t i = 1; i < n; ++i) {
      tmp[sa[i]] = tmp[sa[i - 1]] + (cmp(sa[i - 1], sa[i]) ? 1 : 0);
    }
    rank = tmp;
    if (rank[sa[n - 1]] == static_cast<int64_t>(n - 1)) break;
  }
  return sa;
}

int64_t matchlen(const uint8_t* old_data, int64_t old_len, const uint8_t* new_data,
                 int64_t new_len) {
  int64_t i = 0;
  while (i < old_len && i < new_len && old_data[i] == new_data[i]) ++i;
  return i;
}

// Binary search of `suffixes` for the longest prefix match of new_data.
int64_t search_suffix(const std::vector<int64_t>& suffixes, const uint8_t* old_data,
                      int64_t old_len, const uint8_t* new_data, int64_t new_len,
                      int64_t st, int64_t en, int64_t& pos) {
  if (en - st < 2) {
    int64_t x = matchlen(old_data + suffixes[st], old_len - suffixes[st],
                         new_data, new_len);
    int64_t y = matchlen(old_data + suffixes[en], old_len - suffixes[en],
                         new_data, new_len);
    if (x > y) {
      pos = suffixes[st];
      return x;
    }
    pos = suffixes[en];
    return y;
  }
  int64_t x = st + (en - st) / 2;
  int64_t avail = old_len - suffixes[x];
  int64_t cmp_len = avail < new_len ? avail : new_len;
  if (std::memcmp(old_data + suffixes[x], new_data,
                  static_cast<size_t>(cmp_len)) < 0) {
    return search_suffix(suffixes, old_data, old_len, new_data, new_len, x, en,
                         pos);
  }
  return search_suffix(suffixes, old_data, old_len, new_data, new_len, st, x,
                       pos);
}

}  // namespace

bool bsdiff_create(const uint8_t* old_data, size_t old_len, const uint8_t* new_data,
                   size_t new_len, std::vector<uint8_t>& out_patch,
                   std::string& error) {
  const int64_t olds = static_cast<int64_t>(old_len);
  const int64_t news = static_cast<int64_t>(new_len);
  std::vector<uint8_t> ctrl_raw, diff_raw, extra_raw;

  if (news > 0) {
    if (olds == 0) {
      // No base to diff against: one segment, everything literal.
      put_le64(ctrl_raw, 0);
      put_le64(ctrl_raw, news);
      put_le64(ctrl_raw, 0);
      extra_raw.insert(extra_raw.end(), new_data, new_data + new_len);
    } else {
      std::vector<int64_t> suffixes = build_suffix_array(old_data, old_len);
      int64_t scan = 0, len = 0, lastscan = 0, lastpos = 0, lastoffset = 0;
      while (scan < news) {
        int64_t oldscore = 0, scb = 0, pos = 0;
        for (scb = scan += len; scan < news; ++scan) {
          len = search_suffix(suffixes, old_data, olds, new_data + scan,
                              news - scan, 0, olds - 1, pos);
          for (; scb < scan + len; ++scb) {
            int64_t o = scb + lastoffset;
            if (o >= 0 && o < olds && old_data[o] == new_data[scb]) ++oldscore;
          }
          if ((len == oldscore && len != 0) || len > oldscore + 8) break;
          int64_t o = scan + lastoffset;
          if (o >= 0 && o < olds && old_data[o] == new_data[scan]) --oldscore;
        }
        if (len != oldscore || scan == news) {
          int64_t s = 0, sf = 0, lenf = 0;
          for (int64_t i = 0; lastscan + i < scan && lastpos + i < olds; ++i) {
            if (old_data[lastpos + i] == new_data[lastscan + i]) ++s;
            if (s * 2 - i > sf * 2 - lenf) {
              sf = s;
              lenf = i + 1;
            }
          }
          int64_t lenb = 0;
          if (scan < news) {
            int64_t s2 = 0, sb = 0;
            for (int64_t i = 1; scan >= lastscan + i && pos >= i; ++i) {
              if (old_data[pos - i] == new_data[scan - i]) ++s2;
              if (s2 * 2 - i > sb * 2 - lenb) {
                sb = s2;
                lenb = i;
              }
            }
          }
          if (lastscan + lenf > scan - lenb) {
            int64_t overlap = (lastscan + lenf) - (scan - lenb);
            int64_t s3 = 0, ss = 0, lens = 0;
            for (int64_t i = 0; i < overlap; ++i) {
              if (new_data[lastscan + lenf - overlap + i] ==
                  old_data[lastpos + lenf - overlap + i])
                ++s3;
              if (new_data[scan - lenb + i] == old_data[pos - lenb + i]) --s3;
              if (s3 > ss) {
                ss = s3;
                lens = i + 1;
              }
            }
            lenf += lens - overlap;
            lenb -= lens;
          }
          put_le64(ctrl_raw, lenf);
          put_le64(ctrl_raw, (scan - lenb) - (lastscan + lenf));
          put_le64(ctrl_raw, (pos - lenb) - (lastpos + lenf));
          for (int64_t i = 0; i < lenf; ++i) {
            diff_raw.push_back(static_cast<uint8_t>(new_data[lastscan + i] -
                                                    old_data[lastpos + i]));
          }
          for (int64_t i = 0; i < (scan - lenb) - (lastscan + lenf); ++i) {
            extra_raw.push_back(new_data[lastscan + lenf + i]);
          }
          lastscan = scan - lenb;
          lastpos = pos - lenb;
          lastoffset = pos - scan;
        }
      }
    }
  }

  std::vector<uint8_t> ctrl, diff, extra;
  if (!zlib_compress(ctrl_raw, ctrl, error)) return false;
  if (!zlib_compress(diff_raw, diff, error)) return false;
  if (!zlib_compress(extra_raw, extra, error)) return false;

  out_patch.clear();
  out_patch.insert(out_patch.end(), kMagic, kMagic + 8);
  put_le64(out_patch, static_cast<int64_t>(ctrl.size()));
  put_le64(out_patch, static_cast<int64_t>(diff.size()));
  put_le64(out_patch, news);
  out_patch.insert(out_patch.end(), ctrl.begin(), ctrl.end());
  out_patch.insert(out_patch.end(), diff.begin(), diff.end());
  out_patch.insert(out_patch.end(), extra.begin(), extra.end());
  return true;
}

bool bsdiff_apply(const uint8_t* old_data, size_t old_len, const uint8_t* patch,
                  size_t patch_len, std::vector<uint8_t>& out_new,
                  std::string& error) {
  if (patch_len < kHeaderLen || std::memcmp(patch, kMagic, 8) != 0) {
    error = "not a GMM bsdiff patch (bad magic or truncated header)";
    return false;
  }
  int64_t ctrl_len = 0, diff_len = 0, new_size = 0;
  get_le64(patch + 8, 8, ctrl_len);
  get_le64(patch + 16, 8, diff_len);
  get_le64(patch + 24, 8, new_size);
  if (ctrl_len < 0 || diff_len < 0 || new_size < 0 ||
      static_cast<uint64_t>(ctrl_len) + static_cast<uint64_t>(diff_len) >
          patch_len - kHeaderLen) {
    error = "corrupt patch header (bad block lengths)";
    return false;
  }
  const uint8_t* ctrl_cmp = patch + kHeaderLen;
  const uint8_t* diff_cmp = ctrl_cmp + ctrl_len;
  const uint8_t* extra_cmp = diff_cmp + diff_len;
  size_t extra_len = patch_len - kHeaderLen - ctrl_len - diff_len;

  std::vector<uint8_t> ctrl, diff, extra;
  if (!zlib_decompress(ctrl_cmp, ctrl_len, ctrl, error)) return false;
  if (!zlib_decompress(diff_cmp, diff_len, diff, error)) return false;
  if (!zlib_decompress(extra_cmp, extra_len, extra, error)) return false;
  if (ctrl.size() % 24 != 0) {
    error = "corrupt patch (control block misaligned)";
    return false;
  }

  const int64_t olds = static_cast<int64_t>(old_len);
  std::vector<uint8_t> fresh(static_cast<size_t>(new_size));
  int64_t oldpos = 0, newpos = 0, ctrlpos = 0, diffpos = 0, extrapos = 0;
  const int64_t ctrls = static_cast<int64_t>(ctrl.size());
  const int64_t diffs = static_cast<int64_t>(diff.size());
  const int64_t extras = static_cast<int64_t>(extra.size());
  while (newpos < new_size) {
    if (ctrlpos + 24 > ctrls) {
      error = "corrupt patch (control block overrun)";
      return false;
    }
    int64_t x = 0, y = 0, z = 0;
    get_le64(ctrl.data() + ctrlpos, 8, x);
    get_le64(ctrl.data() + ctrlpos + 8, 8, y);
    get_le64(ctrl.data() + ctrlpos + 16, 8, z);
    ctrlpos += 24;
    if (x < 0 || y < 0 || newpos + x > new_size || diffpos + x > diffs ||
        oldpos + x > olds || extrapos + y > extras) {
      error = "corrupt patch (segment out of range - wrong base file?)";
      return false;
    }
    for (int64_t i = 0; i < x; ++i) {
      fresh[newpos + i] =
          static_cast<uint8_t>(diff[diffpos + i] + old_data[oldpos + i]);
    }
    newpos += x;
    oldpos += x;
    diffpos += x;
    for (int64_t i = 0; i < y; ++i) {
      fresh[newpos + i] = extra[extrapos + i];
    }
    newpos += y;
    extrapos += y;
    oldpos += z;
    if (oldpos < 0 || oldpos > olds) {
      error = "corrupt patch (base offset out of range)";
      return false;
    }
  }
  out_new = std::move(fresh);
  return true;
}

}  // namespace engine::gmmpack
