# Manifest Schema

The `.gmmpack` format is specified in **[gmmpack-format-v1.md](gmmpack-format-v1.md)**,
and `manifest.json` is the first section of it ([`## manifest.json`](gmmpack-format-v1.md#manifestjson)).

The machine-readable schemas live one directory up, in [`../schemas/`](../schemas/):

| Schema                              | Applies to                          |
| ----------------------------------- | ----------------------------------- |
| `manifest.schema.json`              | `manifest.json`                     |
| `mod.schema.json`                   | `mods/<id>.json`                    |
| `executable.schema.json`            | `executables/<id>.json`             |
| `patch.schema.json`                 | `patches/<id>[-N].json`             |
| `ini.schema.json`                   | `ini/<targetFile>.json`             |
| `tree.schema.json`                  | `tree.json`                         |

GMM validates an archive against all six before it will open it
(`engine/modpack/gmmpack/schema_validator.cpp`, driven by `unpack_gmmpack`), and
the exporter runs the same validation on what it just built
(`create_gmmpack` in `engine/modpack/gmmpack/packer.cpp`) so it cannot emit a
pack its own importer would refuse. The schemas are installed to
`share/gamemodmanager/schemas/` next to the binary; `find_schema_dir()` in
`engine/modpack/gmmpack/unpacker.cpp` is the single lookup both paths use.