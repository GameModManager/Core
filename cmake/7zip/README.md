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
