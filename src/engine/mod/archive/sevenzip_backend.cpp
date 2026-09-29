#include "engine/mod/archive/sevenzip_backend.h"

#include "engine/core/util/fs_utils.h"

// 7-Zip's own COM-style interfaces. These are confined to this file: the header
// and everything above the engine layer stay free of 7-Zip types, which is what
// keeps the engine Qt-free and 7-Zip-free in its public surface.
#include "CPP/Common/MyCom.h"
#include "CPP/7zip/Archive/IArchive.h"
#include "CPP/7zip/IPassword.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <new>
#include <string>
#include <system_error>
#include <utility>

// The three entry points lib7zip.so exports for a consumer. GetHandlerProperty2
// is the modern spelling; GetHandlerProperty is the deprecated per-index one and
// is not used. See cmake/7zip/README.md for why the handlers are looked up by
// name rather than by a hardcoded GUID.
extern "C" {
UInt32 GetNumberOfFormats(UInt32 *numFormats);
UInt32 GetHandlerProperty2(UInt32 formatIndex, PROPID propID, PROPVARIANT *value);
HRESULT CreateObject(const GUID *clsID, const GUID *iid, void **outObject);
}

namespace engine {

namespace {

  // A BSTR out of 7-Zip is `wchar_t *` and this build does not pass
  // -fshort-wchar, so on Linux a BSTR is UTF-32, not the UTF-16 a Windows
  // reader assumes. Every conversion here goes through wchar_t and never
  // touches the bytes behind a BSTR directly. See cmake/7zip/README.md.
  std::string to_utf8(const wchar_t *s) {
    std::string out;
    if (!s)
      return out;
    for (; *s; ++s) {
      const uint32_t c = static_cast<uint32_t>(*s);
      if (c < 0x80) {
        out.push_back(static_cast<char>(c));
      } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
      } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
      }
    }
    return out;
  }

  // Narrow UTF-8 to a 7-Zip BSTR. A BSTR here is UTF-32, so an ASCII byte is
  // not a character: widening has to be real, and the allocation has to be
  // 7-Zip's own SysAllocString so the length header in the allocation matches
  // the string 7-Zip reads back.
  BSTR widen_to_bstr(const std::string &utf8) {
    std::wstring wide;
    wide.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
      const unsigned char c = static_cast<unsigned char>(utf8[i]);
      uint32_t cp           = c;
      size_t extra          = 0;
      if (c >= 0xF0) {
        cp   = c & 0x07u;
        extra = 3;
      } else if (c >= 0xE0) {
        cp   = c & 0x0Fu;
        extra = 2;
      } else if (c >= 0xC0) {
        cp   = c & 0x1Fu;
        extra = 1;
      }
      ++i;
      for (size_t k = 0; k < extra && i < utf8.size(); ++k, ++i)
        cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3Fu);
      wide.push_back(static_cast<wchar_t>(cp));
    }
    return SysAllocString(wide.c_str());
  }

  // Owns a 7-Zip callback outright for the duration of an Open/Extract.
  //
  // 7-Zip does not balance its references: it Releases the open callback and
  // the input stream on paths where it never took a reference on them, so a
  // callback whose Release deletes on reaching zero is freed while the library
  // is still using it - which shows up as a crash inside a std::filesystem
  // path destructor a few frames later. The callbacks that outlive a single
  // call therefore never delete themselves; the holder does, once, when the
  // library is done with them. The two callbacks 7-Zip genuinely owns - the
  // per-entry output stream and a multi-volume input stream - are separate
  // classes that do refcount properly.
  template <class T> class ComHolder
  {
  public:
    explicit ComHolder(T *p) : p_(p) {}
    ~ComHolder() { delete p_; }
    ComHolder(const ComHolder &)            = delete;
    ComHolder &operator=(const ComHolder &) = delete;
    T *get() const { return p_; }
    T &operator*() const { return *p_; }
    T *operator->() const { return p_; }

  private:
    T *p_;
  };

  // A seekable 7-Zip input stream over one file. 7-Zip reads the archive more
  // than once for solid and multi-volume archives, so the stream has to support
  // Seek; a sequential-only stream would make RAR5 solid archives unreadable.
  //
  // Two lifetimes, hence two classes: OwnedFileInStream is the archive this
  // function opened, which the holder destroys; FileInStream is the sibling
  // volume 7-Zip asks for, which 7-Zip releases and therefore has to refcount.
  class FileInStreamBase : public IInStream, public CMyUnknownImp
  {
  public:
    explicit FileInStreamBase(std::FILE *f) : file_(f) {}
    // The stream owns the handle it was handed, so the archive file is closed
    // exactly once whichever lifetime destroys it.
    ~FileInStreamBase() {
      if (file_)
        std::fclose(file_);
    }

    HRESULT Read(void *data, UInt32 size, UInt32 *processed) noexcept override {
      if (processed)
        *processed = 0;
      if (size == 0)
        return S_OK;
      const size_t n = std::fread(data, 1, size, file_);
      if (processed)
        *processed = static_cast<UInt32>(n);
      if (n == size)
        return S_OK;
      // A short read is only an error at end of stream; std::ferror tells the
      // two apart, and reporting S_FALSE for a clean end is what the interface
      // contract asks for.
      return std::ferror(file_) ? E_FAIL : S_FALSE;
    }

    HRESULT Seek(Int64 offset, UInt32 origin, UInt64 *newPos) noexcept override {
      int whence = SEEK_SET;
      if (origin == 1)
        whence = SEEK_CUR;
      else if (origin == 2)
        whence = SEEK_END;
      if (std::fseek(file_, static_cast<long>(offset), whence) != 0)
        return E_FAIL;
      if (newPos)
        *newPos = static_cast<UInt64>(std::ftell(file_));
      return S_OK;
    }

  protected:
    std::FILE *file_;
  };

  // The volume 7-Zip asked for by name. It took a reference and gives it back.
  class FileInStream final : public FileInStreamBase
  {
  public:
    using FileInStreamBase::FileInStreamBase;
    Z7_COM_QI_BEGIN
    Z7_COM_QI_ENTRY_UNKNOWN(IInStream)
    Z7_COM_QI_ENTRY(IInStream)
    Z7_COM_QI_ENTRY(ISequentialInStream)
    Z7_COM_QI_END
    Z7_COM_ADDREF_RELEASE
  };

  // One output file. 7-Zip's GetStream hands back one of these per entry and
  // writes into it; the traversal guard runs before the file is created, so a
  // hostile entry name never reaches the filesystem.
  class FileOutStream final : public ISequentialOutStream, public CMyUnknownImp
  {
  public:
    // The one reference here is the one 7-Zip takes ownership of: it adopts
    // this stream from GetStream and releases it when the entry is written,
    // which is also what closes the handle in the destructor below.
    FileOutStream(std::FILE *f, int64_t &written, int64_t total,
                  const ExtractProgressFn &on_progress)
        : file_(f), written_(written), total_(total), on_progress_(on_progress) {
      AddRef();
    }

    // stdio flushes at close, not at the last Write, so an output stream that
    // does not close its handle leaves a zero-length file on disk. 7-Zip
    // releases the stream once the entry is written, which lands here.
    ~FileOutStream() {
      if (file_)
        std::fclose(file_);
    }


    Z7_COM_QI_BEGIN
    Z7_COM_QI_ENTRY_UNKNOWN(ISequentialOutStream)
    Z7_COM_QI_ENTRY(ISequentialOutStream)
    Z7_COM_QI_END
    Z7_COM_ADDREF_RELEASE

  public:
    HRESULT Write(const void *data, UInt32 size, UInt32 *processed) noexcept override {
      if (processed)
        *processed = 0;
      if (size == 0)
        return S_OK;
      const size_t n = std::fwrite(data, 1, size, file_);
      if (processed)
        *processed = static_cast<UInt32>(n);
      if (n != size)
        return E_FAIL;
      written_ += static_cast<int64_t>(size);
      // Throttled to roughly one report per 128 KiB: a solid archive delivers
      // data in small blocks and an unthrottled callback would flood the UI
      // thread. The same threshold the libarchive path uses.
      if (on_progress_ && total_ > 0) {
        const int64_t step = 128 * 1024;
        if (written_ - last_reported_ >= step) {
          last_reported_ = written_;
          on_progress_(written_, total_);
        }
      }
      return S_OK;
    }

  private:
    std::FILE *file_;
    int64_t &written_;
    int64_t total_;
    const ExtractProgressFn &on_progress_;
    int64_t last_reported_ = 0;
  };

  // The archive stream this function opened. Holder-owned, so its Release
  // balances the count 7-Zip hands out without ever destroying the object.
  class OwnedFileInStream final : public FileInStreamBase
  {
  public:
    using FileInStreamBase::FileInStreamBase;
    Z7_COM_QI_BEGIN
    Z7_COM_QI_ENTRY_UNKNOWN(IInStream)
    Z7_COM_QI_ENTRY(IInStream)
    Z7_COM_QI_ENTRY(ISequentialInStream)
    Z7_COM_QI_END

    ULONG AddRef() noexcept override { return ++_m_RefCount; }
    // Never deletes: ComHolder owns this object. See the note on ComHolder.
    ULONG Release() noexcept override { return --_m_RefCount; }
  };

  // The open callback. It also implements ICryptoGetTextPassword because
  // archive-level encryption is requested from here, and
  // IArchiveOpenVolumeCallback because a multi-volume archive asks it for the
  // sibling volumes by name. Both are queried with QueryInterface and a miss is
  // recorded as a generic failure, not a named one, so all three are here.
  class OpenCallback final : public IArchiveOpenCallback,
                             public IArchiveOpenVolumeCallback,
                             public ICryptoGetTextPassword,
                             public CMyUnknownImp
  {
  public:
    OpenCallback(std::filesystem::path path,
                 const std::function<bool(const std::string &, std::string &)> &ask)
        : path_(std::move(path)), ask_(ask) {}

    // IArchiveOpenVolumeCallback is here for the same reason as
    // ICryptoGetTextPassword: 7-Zip queries for it, and a miss is recorded as
    // an unnamed failure rather than a named one.
    Z7_COM_QI_BEGIN
    Z7_COM_QI_ENTRY_UNKNOWN(IArchiveOpenCallback)
    Z7_COM_QI_ENTRY(IArchiveOpenCallback)
    Z7_COM_QI_ENTRY(IArchiveOpenVolumeCallback)
    Z7_COM_QI_ENTRY(ICryptoGetTextPassword)
    Z7_COM_QI_END

  public:
    ULONG AddRef() noexcept override { return ++_m_RefCount; }
    // Never deletes: ComHolder owns this object. See the note on ComHolder.
    ULONG Release() noexcept override { return --_m_RefCount; }

    HRESULT SetTotal(const UInt64 *, const UInt64 *) noexcept override { return S_OK; }
    HRESULT SetCompleted(const UInt64 *, const UInt64 *) noexcept override { return S_OK; }

    // A password was asked for and the user dismissed the prompt, or there was
    // no prompt to show. Returning E_ABORT is how a refusal reaches 7-Zip; the
    // caller's own retry loop decides whether to ask again.
    HRESULT CryptoGetTextPassword(BSTR *password) noexcept override {
      std::string answer;
      if (!ask_ || !ask_(path_.filename().string(), answer) || answer.empty()) {
        refused = true;
        return E_ABORT;
      }
      BSTR b = widen_to_bstr(answer);
      if (!b)
        return E_OUTOFMEMORY;
      *password = b;
      asked = true;
      return S_OK;
    }

    HRESULT GetProperty(PROPID propID, PROPVARIANT *value) noexcept override {
      if (propID == kpidName) {
        const BSTR b = widen_to_bstr(path_.filename().string());
        if (!b)
          return E_OUTOFMEMORY;
        value->bstrVal = b;
        value->vt      = VT_BSTR;
        return S_OK;
      }
      // Any other property is one 7-Zip does not need from a single-file
      // archive; VT_EMPTY is the documented "not set" answer.
      value->vt = VT_EMPTY;
      return S_OK;
    }

    HRESULT GetStream(const wchar_t *name, IInStream **inStream) noexcept override {
      *inStream = nullptr;
      if (!name)
        return S_FALSE;
      const std::filesystem::path sibling = path_.parent_path() / to_utf8(name);
      std::FILE *f = std::fopen(sibling.c_str(), "rb");
      if (!f)
        return S_FALSE;  // no such volume: S_FALSE is how the search stops
      FileInStream *stream = new (std::nothrow) FileInStream(f);
      if (!stream) {
        std::fclose(f);
        return E_OUTOFMEMORY;
      }
      *inStream = stream;
      return S_OK;
    }

    bool asked    = false;
    bool refused = false;

  private:
    std::filesystem::path path_;
    const std::function<bool(const std::string &, std::string &)> &ask_;
  };

  // The extract callback. Implements IProgress (through IArchiveExtractCallback)
  // and ICryptoGetTextPassword, the latter because file-level encryption is
  // requested from here and not from the open callback.
  class ExtractCallback final : public IArchiveExtractCallback,
                                public ICryptoGetTextPassword,
                                public CMyUnknownImp
  {
  public:
    ExtractCallback(IInArchive *archive, const std::filesystem::path &dest,
                    const std::string &archive_name,
                    const std::function<bool(const std::string &, std::string &)> &ask,
                    const ExtractProgressFn &on_progress)
        : archive_(archive),
          dest_(dest),
          archive_name_(archive_name),
          ask_(ask),
          on_progress_(on_progress) {}

    Z7_COM_QI_BEGIN
    Z7_COM_QI_ENTRY_UNKNOWN(IArchiveExtractCallback)
    Z7_COM_QI_ENTRY(IArchiveExtractCallback)
    Z7_COM_QI_ENTRY(IProgress)
    Z7_COM_QI_ENTRY(ICryptoGetTextPassword)
    Z7_COM_QI_END

  public:
    ULONG AddRef() noexcept override { return ++_m_RefCount; }
    // Never deletes: ComHolder owns this object. See the note on ComHolder.
    ULONG Release() noexcept override { return --_m_RefCount; }

    // 7-Zip calls SetCompleted with the archive's own progress. The bytes it
    // reports are the decompressed size for some formats and the packed size
    // for others, and the extractor's contract is bytes written, so the count
    // that reaches the callback is accumulated in FileOutStream instead and
    // this only has to not fail.
    HRESULT SetTotal(UInt64) noexcept override { return S_OK; }
    HRESULT SetCompleted(const UInt64 *) noexcept override { return S_OK; }
    HRESULT PrepareOperation(Int32) noexcept override { return S_OK; }

    // The per-entry outcome. A non-zero opRes here is a data error, a CRC
    // failure or a wrong password; it is recorded rather than returned, because
    // 7-Zip keeps extracting the remaining entries and a silent partial
    // extraction is exactly the failure mode worth surfacing.
    HRESULT SetOperationResult(Int32 opRes) noexcept override {
      if (opRes != NArchive::NExtract::NOperationResult::kOK)
        operation_result = opRes;
      return S_OK;
    }

    HRESULT GetStream(UInt32 index, ISequentialOutStream **outStream,
                      Int32 ask_extract_mode) noexcept override {
      *outStream = nullptr;
      if (ask_extract_mode != NArchive::NExtract::NAskMode::kExtract)
        return S_OK;  // test mode: 7-Zip is checking, not writing

      PROPVARIANT pv;
      pv.vt = VT_EMPTY;
      if (FAILED(archive_->GetProperty(index, kpidPath, &pv)) || pv.vt != VT_BSTR) {
        VariantClear(&pv);
        set_error("cannot read the name of entry " + std::to_string(index));
        return S_FALSE;
      }
      std::string raw = to_utf8(pv.bstrVal);
      VariantClear(&pv);

      // The path-traversal guard. Entry names are Windows-native, so they are
      // normalized the same way engine::resolve_path does - which is also what
      // makes a Windows-authored "dir\\" entry count as a directory. An
      // absolute path or a ".." component is refused outright: a single hostile
      // entry must not escape dest_dir, so the whole extraction fails.
      std::string name = engine::normalize_separators(raw);
      if (name.empty())
        return S_OK;  // nothing to write

      const std::filesystem::path rel(name);
      if (rel.is_absolute() ||
          std::any_of(rel.begin(), rel.end(), [](const auto &part) { return part == ".."; })) {
        set_error("archive entry has an unsafe path: " + name);
        return S_FALSE;
      }

      const bool is_dir = is_directory(index) || (!name.empty() && name.back() == '/');
      const std::filesystem::path dest_path = dest_ / rel;
      if (is_dir) {
        std::error_code ec;
        std::filesystem::create_directories(dest_path, ec);
        if (ec) {
          set_error("cannot create directory " + dest_path.string() + ": " + ec.message());
          return S_FALSE;
        }
        return S_OK;
      }

      std::error_code ec;
      std::filesystem::create_directories(dest_path.parent_path(), ec);
      if (ec) {
        set_error("cannot create parent directory " + dest_path.parent_path().string() + ": " +
                  ec.message());
        return S_FALSE;
      }

      std::FILE *f = std::fopen(dest_path.c_str(), "wb");
      if (!f) {
        set_error("cannot open for writing " + dest_path.string());
        return S_FALSE;
      }
      FileOutStream *stream = new (std::nothrow) FileOutStream(f, bytes_written_, total_, on_progress_);
      if (!stream) {
        std::fclose(f);
        return E_OUTOFMEMORY;
      }
      *outStream = stream;
      // The entry is recorded as extracted once its stream has been handed
      // over. A non-OK opRes later fails the whole extraction anyway, so a
      // half-written entry never reaches the caller.
      entries_[index] = ExtractedFile{name, dest_path};
      created_.push_back(dest_path);
      return S_OK;
    }

    HRESULT CryptoGetTextPassword(BSTR *password) noexcept override {
      std::string answer;
      if (!ask_ || !ask_(archive_name_, answer) || answer.empty()) {
        refused = true;
        return E_ABORT;
      }
      BSTR b = widen_to_bstr(answer);
      if (!b)
        return E_OUTOFMEMORY;
      *password = b;
      asked = true;
      return S_OK;
    }

    // The entries written, in archive order.
    std::vector<ExtractedFile> take_entries() const {
      std::vector<ExtractedFile> out;
      out.reserve(entries_.size());
      for (const auto &[index, file] : entries_) {
        (void)index;
        out.push_back(file);
      }
      return out;
    }

    void set_total(int64_t t) { total_ = t; }

    // 7-Zip asks for the password partway through an entry - after the output
    // stream has been handed over and the file created - so a wrong password or
    // a dismissed prompt leaves a truncated file behind. Nothing on disk is
    // better than a half-written mod, and the caller only ever sees a staging
    // directory it created, so removing exactly what this run created is safe.
    void remove_created() {
      std::error_code ec;
      for (auto it = created_.rbegin(); it != created_.rend(); ++it)
        std::filesystem::remove(*it, ec);
      created_.clear();
    }

    const std::string &error() const { return error_; }
    void set_error(std::string e) {
      if (error_.empty())
        error_ = std::move(e);
    }
    int operation_result = 0;
    bool asked           = false;
    bool refused         = false;

  private:
    bool is_directory(UInt32 index) const {
      PROPVARIANT pv;
      pv.vt = VT_EMPTY;
      const bool dir = SUCCEEDED(archive_->GetProperty(index, kpidIsDir, &pv)) &&
                       pv.vt == VT_BOOL && pv.boolVal == VARIANT_TRUE;
      VariantClear(&pv);
      return dir;
    }

    IInArchive *archive_;
    std::filesystem::path dest_;
    std::string archive_name_;
    const std::function<bool(const std::string &, std::string &)> &ask_;
    const ExtractProgressFn &on_progress_;
    int64_t bytes_written_ = 0;
    int64_t total_         = 0;
    std::string error_;
    std::map<UInt32, ExtractedFile> entries_;
    std::vector<std::filesystem::path> created_;
  };

  // The declared total, summed from the entry headers the same way the
  // libarchive path sums them: a header walk, no data read, so a solid or
  // multi-gigabyte archive costs a few milliseconds.
  int64_t sum_entry_sizes(IInArchive *archive) {
    UInt32 count = 0;
    if (FAILED(archive->GetNumberOfItems(&count)))
      return -1;
    int64_t total = 0;
    for (UInt32 i = 0; i < count; ++i) {
      PROPVARIANT pv;
      pv.vt = VT_EMPTY;
      if (SUCCEEDED(archive->GetProperty(i, kpidSize, &pv)) && pv.vt == VT_UI8)
        total += static_cast<int64_t>(pv.uhVal.QuadPart);
      VariantClear(&pv);
    }
    return total;
  }

  // The GUID that identifies a registered handler, found by its 7-Zip name.
  // Read through kName rather than hardcoded, so a handler that is renamed or
  // absent produces a named failure instead of a silent E_NOINTERFACE.
  bool handler_clsid(const std::string &want, GUID *out) {
    UInt32 count = 0;
    if (FAILED(GetNumberOfFormats(&count)))
      return false;
    for (UInt32 i = 0; i < count; ++i) {
      PROPVARIANT pv;
      pv.vt = VT_EMPTY;
      std::string name;
      if (SUCCEEDED(GetHandlerProperty2(i, NArchive::NHandlerPropID::kName, &pv)) &&
          pv.vt == VT_BSTR)
        name = to_utf8(pv.bstrVal);
      VariantClear(&pv);
      if (name != want)
        continue;
      // kClassID comes back as a BSTR holding the 16 raw GUID bytes. The bytes
      // are read as a GUID, not as a string: the BSTR is a byte blob here, not
      // a character string, so to_utf8 is the wrong reader for it.
      pv.vt = VT_EMPTY;
      if (FAILED(GetHandlerProperty2(i, NArchive::NHandlerPropID::kClassID, &pv)) ||
          pv.vt != VT_BSTR || !pv.bstrVal) {
        VariantClear(&pv);
        return false;
      }
      std::memcpy(out, pv.bstrVal, sizeof(GUID));
      VariantClear(&pv);
      return true;
    }
    return false;
  }

  // Human-readable reason from 7-Zip's per-entry result code. NOperationResult
  // is the value SetOperationResult reports, and kWrongPassword is the one a
  // mistyped passphrase shows up as - naming it is what lets the caller tell a
  // typo from a corrupt archive and re-prompt.
  std::string operation_result_text(int op) {
    namespace R = NArchive::NExtract::NOperationResult;
    switch (op) {
    case R::kUnsupportedMethod:
      return "encoding method unsupported";
    case R::kDataError:
      return "data error";
    case R::kCRCError:
      return "CRC error";
    case R::kUnavailable:
      return "unavailable";
    case R::kUnexpectedEnd:
      return "unexpected end of archive";
    case R::kDataAfterEnd:
      return "data after the end of the archive";
    case R::kIsNotArc:
      return "not a RAR archive";
    case R::kHeadersError:
      return "bad headers";
    case R::kWrongPassword:
      return "the archive is password protected, or the password is wrong";
    default:
      return "7-Zip reported error code " + std::to_string(op);
    }
  }

  // What to report when a password prompt came back empty. An answer that was
  // given and did not work is a rejection; never having been given one means
  // the archive is encrypted and there was no way to ask. Neither names the
  // password, and the caller clears the reason when the user was the one who
  // dismissed the prompt.
  std::string refusal_text(bool asked, bool refused) {
    if (!refused)
      return "7-Zip could not read the archive";
    return asked ? "the password was not accepted"
                 : "the archive is password protected";
  }

  // The leading bytes that decide the engine, read once and shared by the
  // routing decision and the handler lookup. RAR carries either generation's
  // signature in its first 8 bytes; 7z has a fixed 6-byte magic.
  struct Signature {
    bool rar  = false;
    bool rar5 = false;
    bool seven_z = false;
  };

  Signature read_signature(const std::filesystem::path &archive) {
    Signature sig;
    std::ifstream f(archive, std::ios::binary);
    if (!f)
      return sig;
    char magic[8] = {};
    f.read(magic, sizeof(magic));
    const std::streamsize got = f.gcount();
    static constexpr char kRar4[7] = {'R', 'a', 'r', '!', '\x1a', '\x07', '\x00'};
    static constexpr char kRar5[8] = {'R', 'a', 'r', '!', '\x1a', '\x07', '\x01', '\x00'};
    static constexpr char kSevenZ[6] = {'7', 'z', '\xBC', '\xAF', '\x27', '\x1C'};
    if (got >= 7 && std::memcmp(magic, kRar4, 7) == 0)
      sig.rar = true;
    if (got >= 8 && std::memcmp(magic, kRar5, 8) == 0)
      sig.rar5 = true;
    if (got >= 6 && std::memcmp(magic, kSevenZ, 6) == 0)
      sig.seven_z = true;
    return sig;
  }

}  // namespace

const char *archive_engine_name(ArchiveEngine engine) {
  return engine == ArchiveEngine::kSevenZip ? "7-Zip" : "libarchive";
}

ArchiveEngine route_archive(const std::filesystem::path &archive) {
  const Signature sig = read_signature(archive);
  // RAR, both generations, and 7z go to 7-Zip. Everything else stays on
  // libarchive, which is the incumbent, has the test coverage, and is not
  // behind 7-Zip on the formats both read. The decision is made from the
  // archive's own bytes, not its filename: a mod named .7z that is really a
  // zip is still a zip, and libarchive is the reader that gets it right.
  if (sig.rar || sig.rar5 || sig.seven_z)
    return ArchiveEngine::kSevenZip;
  return ArchiveEngine::kLibarchive;
}

std::string sevenzip_handler_for(const std::filesystem::path &archive) {
  const Signature sig = read_signature(archive);
  if (sig.rar5)
    return "Rar5";
  if (sig.rar)
    return "Rar";
  if (sig.seven_z)
    return "7z";
  return {};
}

bool extract_with_sevenzip(const std::filesystem::path &archive,
                           const std::filesystem::path &dest_dir,
                           std::vector<ExtractedFile> &out_files, std::string &error,
                           const ExtractProgressFn &on_progress,
                           const std::function<bool(const std::string &archive_name,
                                                    std::string &passphrase)> &ask_password) {
  error.clear();
  out_files.clear();

  const std::string handler_name = sevenzip_handler_for(archive);
  if (handler_name.empty()) {
    error = "7-Zip has no handler for " + archive.filename().string();
    return false;
  }

  GUID clsid;
  if (!handler_clsid(handler_name, &clsid)) {
    error = "this build of 7-Zip has no " + handler_name + " handler";
    return false;
  }

  IInArchive *in_archive = nullptr;
  if (FAILED(CreateObject(&clsid, &IID_IInArchive, reinterpret_cast<void **>(&in_archive))) ||
      !in_archive) {
    error = "7-Zip could not create its " + handler_name + " reader";
    return false;
  }

  std::FILE *f = std::fopen(archive.c_str(), "rb");
  if (!f) {
    in_archive->Release();
    error = "cannot open " + archive.string();
    return false;
  }

  const std::string archive_name = archive.filename().string();
  ComHolder<OwnedFileInStream> in_stream(new OwnedFileInStream(f));
  ComHolder<OpenCallback> open_cb(new OpenCallback(archive, ask_password));

  HRESULT hr = in_archive->Open(in_stream.get(), nullptr, open_cb.get());
  if (FAILED(hr)) {
    in_archive->Release();
    // A refusal always yields a reason. A dismissed prompt is a cancel and the
    // caller drops the reason; a spent attempt budget is a real failure and has
    // to leave one, so an empty string is never the answer here.
    error = refusal_text(open_cb->asked, open_cb->refused);
    out_files.clear();
    return false;
  }

  UInt32 num_items = 0;
  if (FAILED(in_archive->GetNumberOfItems(&num_items))) {
    in_archive->Close();
    in_archive->Release();
    error = "7-Zip could not enumerate " + archive.string();
    out_files.clear();
    return false;
  }

  if (num_items == 0) {
    // An archive with no entries at all is not an install, it is a failure the
    // user cannot otherwise see: a truncated or corrupt RAR opens cleanly, runs
    // to completion with nothing to do, and would otherwise be reported as a
    // successful install that installed nothing.
    in_archive->Close();
    in_archive->Release();
    error = archive.filename().string() +
            " contains no entries - it is empty or corrupt";
    out_files.clear();
    return false;
  }

  // Header walk for the progress total, the same shape as the libarchive
  // pre-pass: it costs one property read per entry and no data.
  const int64_t total = on_progress ? sum_entry_sizes(in_archive) : -1;

  ComHolder<ExtractCallback> extract_cb(
      new ExtractCallback(in_archive, dest_dir, archive_name, ask_password, on_progress));
  extract_cb->set_total(total);
  hr = in_archive->Extract(nullptr, static_cast<UInt32>(-1), 0, extract_cb.get());

  in_archive->Close();
  in_archive->Release();

  // Every exit below this point is a failure, and all of them look the same
  // from the caller's point of view: the entries written so far are removed and
  // the file list is dropped, so a failed extraction never leaves a partially
  // written mod behind or reports files that are not there.
  if (!extract_cb->error().empty() ||
      extract_cb->operation_result != NArchive::NExtract::NOperationResult::kOK ||
      extract_cb->refused || FAILED(hr)) {
    if (!extract_cb->error().empty())
      error = extract_cb->error();
    else if (extract_cb->refused)
      // The prompt was refused partway through, so the entries after it were
      // never written. Reported as a reason, and the caller turns it into a
      // cancel when the user was the one who dismissed it.
      error = refusal_text(extract_cb->asked, extract_cb->refused);
    else if (extract_cb->operation_result != NArchive::NExtract::NOperationResult::kOK)
      // 7-Zip walked every entry but reported a failure on one of them: a data
      // error, a CRC mismatch or a wrong password. A partial extraction
      // reported as success is how an install ends up silently missing files,
      // so this is a failure and the reason names which one it was.
      error = operation_result_text(extract_cb->operation_result);
    else
      error = "7-Zip could not extract " + archive.string();
    extract_cb->remove_created();
    out_files.clear();
    return false;
  }

  // Final tick so a small archive still reaches 100%. A total that could not be
  // determined reports (0, 0), which is the caller's indeterminate signal.
  if (on_progress)
    on_progress(total > 0 ? total : 0, total > 0 ? total : 0);
  out_files = extract_cb->take_entries();
  return true;
}

}  // namespace engine
