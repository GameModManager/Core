#include "engine/source/http_util.h"

#include <cctype>

namespace engine::Source::Http {

std::string encode_url_path(const std::string &url) {
  const size_t scheme = url.find("://");
  size_t path_start   = std::string::npos;
  if (scheme != std::string::npos)
    path_start = url.find('/', scheme + 3);
  if (path_start == std::string::npos)
    return url;

  size_t query_start    = url.find('?', path_start);
  const size_t path_end = (query_start == std::string::npos) ? url.size() : query_start;

  auto is_hex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  const char *hex = "0123456789ABCDEF";

  std::string out = url.substr(0, path_start + 1);
  for (size_t i = path_start + 1; i < path_end; ++i) {
    unsigned char c = static_cast<unsigned char>(url[i]);
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
      out += static_cast<char>(c);
    } else if (c == '%' && i + 2 < path_end && is_hex(url[i + 1]) &&
               is_hex(url[i + 2])) {
      out += c;
      out += url[i + 1];
      out += url[i + 2];
      i += 2;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0xF];
    }
  }
  if (query_start != std::string::npos)
    out += url.substr(query_start);
  return out;
}

namespace {

  // Append a Unicode code point as UTF-8. Out-of-range values (> 0x10FFFF)
  // and UTF-16 surrogates are dropped by returning false; the caller then
  // leaves the original entity text untouched.
  bool append_utf8(std::string &out, unsigned cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return true;
  }

}  // namespace

std::string decode_html_entities(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  const size_t n = in.size();
  size_t i       = 0;
  while (i < n) {
    if (in[i] != '&') {
      out += in[i++];
      continue;
    }
    // Need a closing ';' within a reasonable window (covers &#x10FFFF;,
    // the longest form we decode).
    const size_t semi = in.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 10) {
      out += in[i++];
      continue;
    }
    const std::string body = in.substr(i + 1, semi - i - 1);
    bool done              = false;
    if (!body.empty() && body[0] == '#') {
      // Numeric: &#NNN; or &#xHH;.
      unsigned cp = 0;
      bool ok     = false;
      if (body.size() > 2 && (body[1] == 'x' || body[1] == 'X')) {
        unsigned v = 0;
        ok         = true;
        for (size_t k = 2; k < body.size(); ++k) {
          const char c = body[k];
          v *= 16;
          if (c >= '0' && c <= '9')
            v += static_cast<unsigned>(c - '0');
          else if (c >= 'a' && c <= 'f')
            v += static_cast<unsigned>(c - 'a' + 10);
          else if (c >= 'A' && c <= 'F')
            v += static_cast<unsigned>(c - 'A' + 10);
          else {
            ok = false;
            break;
          }
        }
        cp = v;
      } else {
        unsigned v = 0;
        ok         = true;
        for (size_t k = 1; k < body.size(); ++k) {
          const char c = body[k];
          if (c < '0' || c > '9') {
            ok = false;
            break;
          }
          v = v * 10 + static_cast<unsigned>(c - '0');
        }
        cp = v;
      }
      if (ok && append_utf8(out, cp)) {
        i    = semi + 1;
        done = true;
      }
    } else if (body == "amp") {
      out += '&';
      i    = semi + 1;
      done = true;
    } else if (body == "lt") {
      out += '<';
      i    = semi + 1;
      done = true;
    } else if (body == "gt") {
      out += '>';
      i    = semi + 1;
      done = true;
    } else if (body == "quot") {
      out += '"';
      i    = semi + 1;
      done = true;
    } else if (body == "apos") {
      out += '\'';
      i    = semi + 1;
      done = true;
    } else if (body == "nbsp") {
      out += ' ';
      i    = semi + 1;
      done = true;
    }
    if (!done) {
      // Not an entity - emit the '&' verbatim, advance one.
      out += in[i++];
    }
  }
  return out;
}

}  // namespace engine::Source::Http
