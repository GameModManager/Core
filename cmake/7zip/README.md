# Vendored 7-Zip build module

Builds [7-Zip](https://www.7-zip.org) 26.03 as a shared library (`lib7zip.so`).

## Why this is vendored rather than downloaded

7-Zip 26.03 publishes no shared library. Every Linux release asset
(`7z2603-linux-x64.tar.xz` and friends) is the `7zz` CLI binary; there is no
`.so`, no `.a` and no dev package. The library therefore has to be built, and
`cmake/7zip.cmake` builds it from a SHA512-pinned source tarball.

## Files

| File | Origin |
| --- | --- |
| `CMakeLists.txt` | verbatim copy of `ports/7zip/CMakeLists.txt` from microsoft/vcpkg |
| `7zip-config.cmake.in` | verbatim copy of the same port |
| `LICENSE.txt` | verbatim copy of 7-Zip 26.03 `DOC/License.txt` |
| `unRarLicense.txt` | verbatim copy of 7-Zip 26.03 `DOC/unRarLicense.txt` |

`CMakeLists.txt` is upstream's file, copied unmodified. The Windows branches
(`ASM_MASM`, `Archive2.def`) are part of the upstream build, not local code.
The vcpkg variables it branches on are supplied by `cmake/7zip.cmake`; the file
itself is never edited, so it can be re-copied from upstream when 7-Zip is
bumped.

The build system is available to a consumer as `PkgConfig::SEVENZIP`, matching
every other dependency in the top-level `CMakeLists.txt`.

## Source pin

```
https://github.com/ip7z/7zip/archive/26.03.tar.gz
SHA512 3d205b1a8fb91a622eaf27cbf81b5eb1e7c96fd3f044779405a6794ccea644b1017e64bdc973f9629567529aa625ec846a0d0265c5ea13357d485b5d9197e4eb
```

That is the same pin microsoft/vcpkg's `ports/7zip` carries, so this build and a
vcpkg build consume byte-identical source.

## Licence

7-Zip is **LGPL-2.1-or-later**, with the **unRAR restriction** applying to
`CPP/7zip/Compress/Rar*` files. Per-file terms are listed in `LICENSE.txt`;
`unRarLicense.txt` is the restriction in full.

Two obligations follow, and both are met:

- The library is built and distributed **shared**, not static. Static linking
  would additionally trigger LGPL-2.1 section 6's requirement to ship the
  object files needed for relinking.
- The RAR code is used to **decompress** RAR only. The unRAR restriction
  forbids using these sources to re-create the RAR compression algorithm, which
  is what a RAR-compatible compressor would do. Nothing here creates a RAR
  archive.

## API traps

None of the three below fail at compile time. Each one compiles, links and runs,
and each surfaces as a wrong answer or a generic failure code rather than as a
build error, so they are written down here rather than left to be rediscovered.

### 1. A BSTR is `wchar_t`, so UTF-32, not UTF-16

`OLECHAR` is `WCHAR`, i.e. `wchar_t`, and this build does **not** pass
`-fshort-wchar`. On Linux `sizeof(wchar_t) == 4`, so a BSTR is a UTF-32 string,
not the UTF-16 a Windows reader would assume.

`CPP/Common/MyWindows.cpp` stores a **byte** count in the 4-byte header before
the string (`CBstrSizeType` is `UINT32`; `SysAllocStringLen` writes
`len * sizeof(OLECHAR)`), and `SysStringLen` divides it back down by
`sizeof(OLECHAR)`. So a BSTR of `n` characters occupies `4n` bytes, and
`memcpy`-ing `n` ASCII bytes into it leaves three uninitialised bytes per
character. Nothing detects that: the types line up, the size is right, and the
result arrives as a **wrong-password error** rather than as a buffer bug. Widen
to `wchar_t` and let 7-Zip allocate, so the length header matches:

```c++
// correct
BSTR b = SysAllocString(widened.c_str());
// ... use b ...
SysFreeString(b);
```

`SysAllocStringLen(s, n)` also takes a `wchar_t*` and a **character** count, so
it is equally safe. `SysAllocStringByteLen` is not: it manages the allocation
size correctly but still packs one character per byte, so the string the
library reads back is wrong.

### 2. The password is asked for on both callbacks

Archive-level encryption is requested from `IArchiveOpenCallback`, file-level
encryption from `IArchiveExtractCallback`. `Rar5Handler.cpp` queries
`IID_ICryptoGetTextPassword` off the open callback at line 2261 and off the
extract callback at line 3295, and there is nothing in between that would let
one stand in for the other.

Implement only one and the other `QueryInterface` fails, which 7-Zip records as
`opRes = 1` (`NOperationResult::kUnsupportedMethod`, `IArchive.h:136-137`) -
a code that does not name the callback you forgot. **Both callbacks must
implement `ICryptoGetTextPassword`.**

### 3. `Z7_IFACE_CONSTR_ARCHIVE(i, n)` puts the groupId at 6, not at `n`

`Z7_IFACE_CONSTR_ARCHIVE` expands to `Z7_DECL_IFACE_7ZIP_SUB(i, IUnknown, 6, n)`
(`CPP/7zip/Archive/IArchive.h:13-18`), and `Z7_DECL_IFACE_7ZIP_SUB`
(`CPP/7zip/IDecl.h:18-23`) builds the last field as
`{0, 0, 0, groupId, 0, subId, 0, 0}`. So the `n` you pass is the **subId**, and
the groupId is always 6. Reading the macro as "the id is `n`" gives the wrong
`Data4` and every `QueryInterface` for that interface returns `E_NOINTERFACE`.

`IID_IInArchive` is exactly:

```
{0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x60, 0x00, 0x00}}
```

with `subId = 0x60`, **not** `0x06` in that slot.

### Finding handlers by name instead

Hardcoding these GUIDs is the fragile option, and the alternative should be
known before anyone reaches for one. The library exports
`GetNumberOfFormats` and `GetHandlerProperty2`; calling the latter with `kName`
(`IArchive.h:103`, `PROPID 0`, `VT_BSTR`) for each index returns that handler's
name, which can be compared as a string. The RAR handlers are `Rar5` and `Rar`.

This is how the handlers were identified when the library was brought up, and
it is what a consumer should prefer: a mistyped GUID is invisible, a handler
name that does not match is not.

## Known gap: no RAR4 fixture

The `Rar` handler (7-Zip's name for RAR 1.5 to 4.x) compiles in and enumerates
from `GetNumberOfFormats` as expected, but there is **no valid RAR4 test
fixture** in the repository. 7-Zip cannot create RAR - that is the unRAR
restriction above - and no `rar` binary is available on this machine, so
neither the library nor the CLI can produce one.

This is not blocking. The 64 MiB dictionary cap that motivates the 7-Zip
backend is a RAR5 problem; RAR4 archives are not affected by it. A RAR4 fixture
is still wanted before the RAR4 path can be called tested.
