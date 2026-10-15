# `--list-games` - the supported-games document

`gamemodmanager --list-games` prints every game GMM can act on, grouped per
source, as JSON on stdout, and exits. Nothing else is written to stdout: a
consumer never has to strip prose off it. Diagnostics, if any, go to stderr.

This is the data contract for the nxmproxy integration and for the standalone
GUI. It is derived, never maintained by hand: the game list comes from what the
loaded plugins registered (`register_identity` / `register_game`, plus the
per-game `download_sources` hook), unioned with the user's
`managed_games.json`. Adding a game to GMM means shipping a plugin that
registers it, so the document cannot advertise a game GMM cannot handle.

## Shape

```json
{
  "schema": "gmm.supported-games/1",
  "games": [
    {
      "game_id": "SkyrimSpecialEdition",
      "name": "Skyrim Special Edition",
      "plugin": "SkyrimSpecialEdition.so",
      "provenance": "plugin",
      "identifiers": {
        "steam_appid": 489830,
        "nexus_domain": "skyrimspecialedition",
        "gog_id": null,
        "epic_namespace": null
      },
      "sources": [
        { "source_id": "nexus",     "name": "Nexus Mods",      "provenance": "plugin", "identifier": "skyrimspecialedition" },
        { "source_id": "steam",     "name": "Steam Workshop",  "provenance": "plugin", "identifier": "489830" },
        { "source_id": "loverslab", "name": "LoversLab",       "provenance": "plugin", "identifier": null }
      ]
    }
  ]
}
```

### Why this shape

**An object, not a bare array.** The document has to carry its own version, and
it needs somewhere to put fields that do not exist yet. A bare array has
nowhere to put either. Consumers read `schema` first and refuse what they do
not recognise.

**`games` is an array sorted by `game_id`, not an object keyed by game_id.**
Plugin load order is a filesystem directory walk, which differs between
machines. Sorting makes the output byte-identical on two hosts with the same
plugins, so a consumer can diff it.

**Sources are nested under the game, not a top-level source-to-games map.** The
useful per-source facts are properties of the (game, source) pair: the nxm://
domain, the Steam appid. A top-level map would have to split that back out
again. A source a game does not offer is simply absent from that game's
`sources`, which is what makes "supported on Nexus but not on LoversLab"
answerable without a second document.

**`identifier` is a string, never a number.** It is an nxm:// domain on one row
and a Steam appid on another. One JSON type, no consumer-side union handling.

**`source_id` may be null.** A plugin may declare a source by display name for
which GMM ships no provider. The declaration is reported (it is a real fact
about the plugin) but no id is invented for it, because a wrong id would send a
consumer to a source that does not exist. A null `source_id` means "named by
the plugin, no machine id".

**`provenance` is per source as well as per game.** A game's sources can come
from the plugin and from the user's file at once. A game is `user` only when no
plugin claims it.

**Ordering within `sources` is meaningful only in that it is stable**: Nexus and
Steam first (from the plugin's identity registration), then the remaining
declared sources in the order the plugin listed them. Do not depend on it.

## Fields

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | string | `gmm.supported-games/<major>`. Check this first. |
| `games[].game_id` | string | GMM's own id. What an instance carries in `instance.toml`. |
| `games[].name` | string | Display name. For a user-only game this is the `game_id`. |
| `games[].plugin` | string | The GMM plugin (compiled, in `plugins/`) that registered the game. Absent when `provenance` is `user`. |
| `games[].provenance` | `"plugin"` \| `"user"` | `plugin`: a loaded GMM plugin registered game support. Authoritative. `user`: only the user's `managed_games.json` knows it. Inferred from user config - GMM did not verify it. |
| `games[].identifiers.steam_appid` | number \| null | |
| `games[].identifiers.nexus_domain` | string \| null | The nxm:// domain. This is the routing key nxmproxy needs. |
| `games[].identifiers.gog_id` | string \| null | |
| `games[].identifiers.epic_namespace` | string \| null | |
| `games[].sources[]` | array | Sources this game is offered on. May be empty: a plugin may register a game with no download source at all. |
| `games[].sources[].source_id` | string \| null | GMM's source id (`nexus`, `loverslab`, `modpub`, `steam`) - the same strings the Sources tab and `Source::Interface::source_type()` use. Null when no provider matches. |
| `games[].sources[].name` | string | Display name. |
| `games[].sources[].provenance` | `"plugin"` \| `"user"` | Where this source row came from. |
| `games[].sources[].identifier` | string \| null | The per-game handle on that source: the nxm:// domain, or the Steam appid as a string. |
| `games[].sources[].website_url` | string | Only on `user` rows. The host as the user's `managed_games.json` wrote it (`"nexusmods.com"`), verbatim - GMM does not normalise or validate it. |

There is no `detected` provenance. Detection (`GameDetector`) finds *installs*
of games a plugin already registered; it never adds a game identity, so a game
cannot come from it.

## What is stable

Under `gmm.supported-games/1`:

- The top-level keys `schema` and `games`.
- The keys and JSON types listed in the table above. Every key is always
  present unless the table says otherwise; absent means null.
- `game_id`, `source_id` and `identifier` values, which are the same strings
  the rest of GMM uses. An `nxm://` URL whose domain matches a `nexus` row's
  `identifier` is a game GMM supports.
- Sorted-by-`game_id` ordering of `games`.
- The `provenance` vocabulary.

## What may change without a version bump

- New keys added to any object.
- New `source_id` values (a new provider ships and starts appearing).
- New `provenance` values.
- New games and new sources per game, of course - that is the point.
- The `name` strings, which follow the plugin and the provider.

Consumers should therefore ignore unknown keys, and should not treat an unknown
`source_id` or `provenance` value as an error.

## What is NOT part of this contract

- The GMM version. Run `gamemodmanager --version`; the document deliberately
  does not carry it, so bumping the release does not churn this file.
- `sources[].website_url` values. They are the user's own config, verbatim.
- Anything read from the plugin *tab* registration. GMM's C++ plugin ABI does
  not wire that slot up, so those values would be fiction for the plugins we
  ship. The tab `source_id` slot that exists in the plugins is therefore
  ignored here.

## Consuming it

### A script

```sh
gamemodmanager --list-games | jq -r '.games[]
  | select(any(.sources[]; .source_id == "nexus"))
  | "\(.game_id)\t\(.name)"'
```

### nxmproxy

The routing question is "which game does this nxm:// domain belong to". Build a
map from every `source_id == "nexus"` row's `identifier` to its `game_id`. A
domain that appears in no row is a game GMM cannot handle.

### The GUI (only when nxmproxy is installed)

Do not shell out and re-parse. `engine::build_supported_games(const PluginLoader &,
const ManagedGames &, const Source::Registry &)` in
`src/engine/source/game_catalog.h` is engine code and Qt-free: a GUI already
holding the loaded plugins and the managed-games registry calls it in-process and
gets the identical document the CLI prints. Serve `doc.dump(2)` verbatim from
whatever endpoint it needs and there is exactly one shape in the project.

The GUI is conditional on nxmproxy being installed, and no GUI ships with this
feature yet.