# Contributing

## Commit scopes

"Plugin" is three different things in this codebase. A commit scope that says
`plugins` without saying which one is ambiguous, so pick the specific term:

- **GMM plugin** - a compiled game plugin: `external/plugins`, the
  `projects/Plugins` repo, the Plugin Host. Scope `feat(plugin_host)` /
  `feat(gmm_plugin)`.
- **Bethesda plugin file** - a `.esm` / `.esp` / `.esl` file in a mod folder,
  and the table that lists them (`PluginView`, `PluginContextMenu`,
  `PluginsTab`). Scope `feat(plugins_tab)` / `feat(bethesda_plugin)`.
- **Plugins tab** - the right-side UI panel listing those files. Same surface as
  the row above; scope it `feat(plugins_tab)`.

A commit that changes the `.esm`/`.esp`/`.esl` table is not a GMM-plugin commit.