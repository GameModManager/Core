"""
GameModManager Python plugin API - type stubs.

The `gmm` module is version-agnostic: a single `import gmm` works against
any ABI version the host loads. New ABI features (register_* slots, hooks,
event ids) appear here automatically; there is no v2 / v3 split.

Two plugin patterns are supported:

  1. Legacy flat function::

         def register(ctx: gmm.RegistrationContext) -> None:
             ctx.register_identity(steam_appid=489830)
             ctx.register_tab("plugins", display_name="Plugins", data_path="Data/")

  2. Declarative Plugin subclass + module-level ``gmm.register(...)``::

         class MyPlugin(gmm.Plugin):
             def game_info(self) -> dict: ...
             def tabs(self) -> list[dict]: ...
             def features(self, ctx: gmm.RegistrationContext) -> None: ...

         gmm.register(MyPlugin)

Every ``register_*`` method on :class:`RegistrationContext` returns the
context itself, so calls can be chained fluently::

    ctx.register_identity(steam_appid=1, nexus_domain="skyrimse") \\
       .register_meta(author="me", version="1.0") \\
       .register_tab("plugins", display_name="Plugins", data_path="Data/")
"""

from typing import Any, Callable, Self

# -- Plugin base class ---------------------------------------------------------

class Plugin:
    """Base class for declarative GMM plugins.

    Subclass and override the methods you need. The loader instantiates the
    class and calls:

      - game_info()  -> dict with game_id, display_name, steam_appid, etc.
      - tabs()       -> list of tab dicts (capability, display_name, ...)
      - categories() -> list of category strings
      - features(ctx) -> register tools, parsers, etc. on ctx
      - hooks(ctx)    -> register game-dependent hooks on ctx
      - events(ctx)   -> subscribe to events on ctx

    Then pass the class (or an instance) to :func:`register`::

        gmm.register(MyPlugin)
    """

    def game_info(self) -> dict[str, Any]:
        """Return a dict describing the game. Must be overridden.

        Recognized keys: game_id, display_name, steam_appid, nexus_domain,
        gog_id, epic_namespace, exe_windows, exe_linux, exe_macos.
        """
        ...

    def tabs(self) -> list[dict[str, Any]]:
        """Return the list of UI tabs to register. Optional, defaults to []. """
        ...

    def categories(self) -> list[str]:
        """Return the list of categories this plugin belongs to. Optional."""
        ...

    def features(self, ctx: RegistrationContext) -> None:
        """Register tools, parsers, hooks, events, etc. on ``ctx``. Optional."""
        ...

    def hooks(self, ctx: RegistrationContext) -> None:
        """Register game-dependent hooks on ``ctx``. Optional."""
        ...

    def events(self, ctx: RegistrationContext) -> None:
        """Subscribe to host events on ``ctx``. Optional."""
        ...

# -- Module-level registration entry point ------------------------------------

def register(plugin: type[Plugin] | Plugin) -> None:
    """Hand a Plugin class (or pre-built instance) to the host loader.

    The loader pops the last entry from ``gmm._registered_plugins`` on the
    next ``python_load_plugin`` call. Pass a class for the loader to
    instantiate, or a pre-built instance if you need to share state.
    Passing anything that is not a :class:`Plugin` subclass (when it is a
    class) logs a warning and is ignored.
    """

# -- RegistrationContext -------------------------------------------------------

class RegistrationContext:
    """Context passed to a plugin's ``register()`` function (or
    :meth:`Plugin.features` / ``hooks`` / ``events``).

    All ``register_*`` methods return the context itself for fluent
    chaining. Parameters are keyword-friendly with sensible defaults.
    """

    @property
    def game_id(self) -> str:
        """The game id this plugin is registered for (derived from
        the module filename or :meth:`register_game` / :meth:`register_identity`).
        """
        ...

    # -- Identity / metadata ----------------------------------------------------

    def register_identity(
        self,
        steam_appid: int = 0,
        gog_id: str = "",
        epic_namespace: str = "",
        nexus_domain: str = "",
        display_name: str = "",
        exe_windows: str = "",
        exe_linux: str = "",
        exe_macos: str = "",
    ) -> Self:
        """Register this plugin's game identity. Pure data, no behavior.

        Setting ``steam_appid`` > 0 or a non-empty ``nexus_domain`` marks
        the plugin as providing game support (so it can back an instance).
        """
        ...

    def register_game(
        self,
        game_id: str,
        display_name: str = "",
        steam_appid: int = 0,
        nexus_domain: str = "",
        gog_id: str = "",
        epic_namespace: str = "",
        exe_windows: str = "",
        exe_linux: str = "",
        exe_macos: str = "",
    ) -> Self:
        """Register this plugin as a game module (IPluginGame identity).

        Sets the game id, display name, and store/app ids. Marks the plugin
        as providing game support.
        """
        ...

    def register_meta(
        self,
        author: str = "",
        version: str = "",
        description: str = "",
    ) -> Self:
        """Register optional metadata for the Plugins settings tab.

        All fields are optional; empty strings leave them unset.
        """
        ...

    def register_category(self, category: str = "") -> Self:
        """Declare this plugin's primary category (legacy single-string form).

        The first entry of :meth:`register_categories` is mirrored here for
        legacy consumers.
        """
        ...

    def register_categories(self, categories: list[str]) -> Self:
        """Declare this plugin's categories as a list (batch form).

        All entries are persisted; the first non-empty entry is also
        mirrored into the single-string :meth:`register_category` field.
        """
        ...

    # -- Settings ---------------------------------------------------------------

    def register_settings(
        self, settings: list[tuple[str, str]] = []
    ) -> Self:
        """Declare this plugin's user-facing options as plain key:value pairs.

        Each entry is (key, default value). Rendered as editable rows in a
        scrollable container in the Plugins settings tab. Persisted
        overrides are read back at startup; edits from the UI are written
        back.

        Source providers must NOT use this - their settings live on the
        Sources tab instead.
        """
        ...

    def register_settings_tab(
        self,
        title: str,
        settings: list[tuple[str, str, str, object | None]] = [],
    ) -> Self:
        """Declare a typed settings tab rendered as native host widgets.

        Each entry is (key, type, default, options):

          ("show_previews", "bool",   "1",    None)      -> checkbox
          ("max_threads",   "int",    "4",    "1:8")     -> spinbox (range)
          ("mod_name_prefix","string","mod_", None)      -> line edit
          ("install_mode",  "choice", "Full", ["Full", "Compact", "Minimal"])
                                                        -> combo box

        The host adds a Settings-dialog tab titled by ``title`` (one per
        plugin) after the fixed tabs. Edits persist through the same
        plugins/settings/<basename>/<key> store as :meth:`register_settings`;
        keys declared here stop rendering as raw key:value rows. ``options``
        is None except: "choice" (list of candidate values) and "int" (the
        "min:max" range string).
        """
        ...

    # -- Stage / animation claims ----------------------------------------------

    def register_stage_claim(
        self, stage_name: str, fn: Callable[..., bool], priority: int = 100,
    ) -> Self:
        """Claim exclusive ownership of a pipeline stage for this plugin's game.

        ``fn`` is the bridge to the host stage pipeline. It receives opaque
        integer handles (mod/instance/conflict_index/profile); see the
        stage-claim bridge docs for the ABI. Highest priority wins for a
        given (stage, game_id) pair. The engine falls back to generic
        logic if nothing claims it.
        """
        ...

    def register_wildcard_stage_claim(
        self,
        game_id: str = "",
        stage_name: str = "",
        fn: Callable[..., bool] = ...,
        priority: int = 100,
    ) -> Self:
        """Claim exclusive ownership of a pipeline stage for any / specific game.

        Like :meth:`register_stage_claim` but with an explicit ``game_id``
        so a plugin can claim a stage for ANY game (``game_id=""`` =
        wildcard) or for a specific game different from its own.

        Resolution priority: exact-match claims beat wildcard claims at
        equal priority, so a game-specific plugin always wins over a
        generic wildcard.
        """
        ...

    def register_animation_parser(
        self,
        game_id: str,
        extension: str,
        fn: Callable[[str, str], dict | None],
        priority: int = 100,
    ) -> Self:
        """Register an animation file parser for the UI preview widget.

        ``fn(file_path, base_dir)`` returns a dict that the engine
        converts into animation data (frames, layers, canvas size). The
        ``extension`` is a documentation breadcrumb: the
        Game::Features::Registry keys this feature as
        ``(game_id, "animation_parser")`` with no extension discriminator,
        so two parsers for the same game at different extensions collide
        and ``priority`` wins.
        """
        ...

    # -- Diagnostics / events ---------------------------------------------------

    def register_requirements(
        self, fn: Callable[[], list[tuple[str, str, str]]]
    ) -> Self:
        """Declare plugin requirements.

        ``fn()`` returns a list of (type, name, message) tuples, where
        ``type`` is one of "plugin", "game", "diagnose". The loader
        evaluates them once all plugins are loaded and surfaces any that
        are unmet.
        """
        ...

    def register_diagnostics(
        self,
        game_id: str = "",
        fn: Callable[[], str | list[str] | list[object]] | None = None,
    ) -> Self:
        """Register a diagnostics provider.

        ``fn()`` is called with no arguments and returns either:
          - a string / list[str] of short problem messages, or
          - a list of problems, each a (short, full) tuple or a dict with
            keys "short_description", "full_description",
            "has_guided_fix" (int 0/1), and optionally "start_guided_fix"
            (a callable invoked to fix it).

        ``game_id`` scopes the provider to one game ("" = this plugin's
        game). Registered into both the v1 DiagnosticsRegistry and the v2
        DiagnoseRegistry.
        """
        ...

    def subscribe_event(
        self, event_id: str, fn: Callable[[str, dict[str, str]], None]
    ) -> Self:
        """Subscribe to a host event bus event (MO2 signal analogue).

        ``fn`` is called as ``fn(event_id, payload)`` whenever the host
        emits the event; ``payload`` is a plain dict of str -> str
        (the bus payload is a JSON object of string values, decoded by
        the bridge). Canonical event ids and their payload keys:

          mod_installed      {"mod", "name"}
          mod_removed        {"mod"}
          mod_state_changed  {"mod", "enabled"}
          mod_moved          {"mod", "from", "to"}
          profile_created    {"profile"}
          profile_renamed    {"old_name", "new_name"}
          profile_removed    {"profile"}
          profile_changed    {"profile"}
          plugin_list_refreshed  {}
          plugin_state_changed   {"plugin", "enabled"}
          plugin_moved       {"plugin", "from", "to"}
          download_complete  {"id", "file"}
          download_paused    {"id"}
          download_failed    {"id"}
          download_removed   {"id"}
          game_launched      {"exe", "args"}
          game_finished      {"exit_code"}

        Handlers run on the emitting thread; keep them short and
        non-blocking.
        """
        ...

    # -- IPluginGame / v2 -------------------------------------------------------

    def register_order_encoding(
        self, fn: Callable[[list[str], str], int]
    ) -> Self:
        """Register a load-order writer.

        ``fn(ordered_mod_ids, output_path)`` writes the game's load-order
        file (plugins.txt / metadata.xml, ...). Return 1 on success, 0 on
        failure. Registered into the OrderEncodingRegistry; the pipeline
        calls it when writing load order instead of the built-in hook.
        """
        ...

    def register_deploy_strategy(
        self,
        deploy_fn: Callable[[str, str], int],
        remove_fn: Callable[[str], int] | None = None,
    ) -> Self:
        """Register a custom deployment strategy.

        ``deploy_fn(source, target)`` places a single mod file into the
        game data directory; ``remove_fn(target)`` removes it. Return 1 on
        success, 0 on failure. Registered into the DeployStrategyRegistry;
        the pipeline uses it instead of the built-in Deploy::Interface.
        """
        ...

    def register_image_diff(self) -> Self:
        """Register an image diff provider for merging conflicting sprite files.

        Launched from the Conflicts tab context menu on conflicting files.
        """
        ...

    def register_tool(
        self,
        tool_id: str,
        kind: str,
        fn: Callable[[], None] | None = None,
    ) -> Self:
        """Register an external tool.

        ``kind``: "advisory" (output feeds into pipeline) or "workshop"
        (user launches directly). ``fn``, if given, is invoked when the
        tool runs (registered into the v2 PluginToolRegistry and the Tools
        menu).
        """
        ...

    def register_file_mapper(
        self,
        game_id: str = "",
        fn: Callable[[], list[tuple[str, str]]] = ...,
    ) -> Self:
        """Register a virtual file overlay mapper.

        ``fn()`` returns a list of (source, target) virtual-path pairs that
        the deploy pipeline turns into virtual file overlays. ``game_id``
        scopes the mapper ("" = this plugin's game). Registered into the
        FileMapperRegistry.
        """
        ...

    def register_save_parser(
        self,
        game_id: str = "",
        fn: Callable[[str, str], dict | None] | None = None,
        priority: int = 0,
    ) -> Self:
        """Register a save-game parser.

        ``fn(path, game_id)`` parses a save file and returns a dict with
        keys file_path, game_id, creation_time (int), pc_name, pc_level
        (int), pc_location, save_number (int), plugins (list[str]),
        light_plugins (list[str]). Return None to decline. Higher priority
        wins on resolve. Registered into the SaveParserRegistry.
        """
        ...

    def register_hook(
        self,
        tag: str,
        data: str = "",
        fn: Callable[[str, str], None] | None = None,
        priority: int = 0,
    ) -> Self:
        """Register a behavior-injection hook.

        ``fn(tag, data)`` is fired at pipeline points (before_deploy,
        after_scan, conflict_resolution) in priority order. Also stored as
        game knowledge (data payload). Registered into the v2 HookRegistry.
        """
        ...

    def register_preview(
        self, extension: str, fn: Callable[[str], int]
    ) -> Self:
        """Register a file preview generator.

        ``fn(file_path)`` returns the raw QWidget* (e.g. obtained via
        shiboken6.getCppPointer(widget)[0] when building the widget with
        PySide6). The engine embeds the widget in the preview panel
        exactly like a C plugin. Registered into the UI Registry.
        """
        ...

    def register_modpage(
        self, url: str, fn: Callable[[str, str], int]
    ) -> Self:
        """Register a ModPage download handler.

        ``fn(url, output_path)`` downloads the page/archive to
        ``output_path``. Return 1 on success, 0 on failure.
        """
        ...

    def register_sort_provider(
        self, fn: Callable[[list[str]], list[str]]
    ) -> Self:
        """Register a load-order sort provider.

        ``fn(mod_folders)`` returns the sorted mod folder names.
        Registered into the SortRegistry for this plugin's game.
        """
        ...

    # -- Game features (MO2 IGameFeatures parity) ------------------------------

    def register_game_feature(
        self,
        game_id: str = "",
        feature_type: str = "",
        priority: int = 0,
        folder_names: list[str] = [],
        file_extensions: list[str] = [],
    ) -> Self:
        """Register or override a per-game behavior feature.

        ``feature_type``: "mod_data_checker" (first). The game's own
        feature registers at the LOWEST priority; registering the same
        type at a higher priority overrides/augments it (the engine
        combines all checkers: ANY checker VALID -> VALID, so the union's
        allow-sets drive the mod list's "No valid game data" flag).
        ``game_id``: the game this feature serves ("" = this plugin's own
        game). ``folder_names``/``file_extensions``: mod_data_checker
        allow-sets (top-level directory names / file extensions that count
        as real game data).
        """
        ...

    def register_game_feature_data(
        self,
        game_id: str = "",
        feature_type: str = "",
        priority: int = 0,
        data: dict[str, str] = {},
    ) -> Self:
        """Register a per-game feature whose payload is key/value pairs.

        Covers the 7 structured-data feature types;
        :meth:`register_game_feature` covers the two array-payload types.
        Same priority + replace semantics.

        Keys per ``feature_type``:
          "mod_data_content"  - "enabled": comma-separated catalog content
                                IDs (plugin, optional, interface, mesh,
                                bsa, script, skse, skse_files, skyproc,
                                sound, texture, mcm, ini, facegen,
                                modgroup); "content:<id>":
                                "name|icon|filter_only".
          "data_archives"     - "vanilla_archives": comma-separated
                                archive names.
          "script_extender"   - "binary", "plugin_path", "loader_name",
                                "savegame_extension".
          "save_game_info"    - "extensions": comma-separated save
                                extensions.
          "local_savegames"   - "saves_subpath", "ini_file".
          "unmanaged_mods"    - "mods": comma-separated internal mod
                                names.
          "bsa_invalidation"  - "bsa_name", "bsa_version".
        """
        ...

    # -- UI tabs / capabilities -------------------------------------------------

    def register_capability(
        self,
        capability: str,
        display_name: str = "",
        data_path: str = "",
        description: str = "",
        protocol_handler: str = "",
        website_domain: str = "",
        supported_platforms: str = "",
    ) -> Self:
        """Register a game capability (non-tab feature).

        ``capability``: "plugins", "archives", "saves", "downloads".
        ``display_name``: label for the capability. ``data_path``: relative
        path where these are stored (e.g. "Data/"). For downloads:
        ``protocol_handler`` ("nxm", "workshop"), ``website_domain``,
        ``supported_platforms`` (comma-separated).
        """
        ...

    def register_tab(
        self,
        capability: str,
        display_name: str = "",
        data_path: str = "",
        description: str = "",
        protocol_handler: str = "",
        website_domain: str = "",
        supported_platforms: str = "",
        insert_before: str = "",
        insert_after: str = "",
    ) -> Self:
        """Register a UI tab for this game with ordering.

        ``capability``: "plugins", "archives", "saves", "downloads",
        "conflicts", "data". ``display_name``: tab label shown in the
        right panel. ``insert_before``/``insert_after``: capability_id
        this tab should appear before/after.
        """
        ...

    def register_tabs(self, tabs: list[dict[str, Any]]) -> Self:
        """Batched :meth:`register_tab` (plural form).

        Each item is a dict with the same keys accepted by
        :meth:`register_tab`. Items may also be 1-/2-/3-tuples
        ``(capability, display_name?, data_path?)`` for the common case.
        """
        ...

    # -- Host services ----------------------------------------------------------

    def resolve_file(self, root: str, relative_path: str) -> str:
        """Resolve ``relative_path`` against ``root`` via the host's VFS.

        Returns the absolute filesystem path on success, or an empty
        string on failure. The path is NOT guaranteed to be contained
        within ``root`` - the VFS resolver enforces containment
        independently.
        """
        ...
