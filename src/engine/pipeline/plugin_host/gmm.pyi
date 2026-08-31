"""
GameModManager Python plugin API - type stubs

Usage in a plugin:
    import gmm

    def register(ctx: gmm.RegistrationContext) -> None:
        ctx.register_identity(
            steam_appid=489830,
            nexus_domain="skyrimspecialedition",
        )
        ctx.register_capability("plugins", display_name="Plugins", data_path="Data/")
"""

from typing import Callable

class RegistrationContext:
    """Context passed to a plugin's register() function.

    All parameters are keyword-only with sensible defaults.
    Only call the methods relevant to your game module.
    """

    @property
    def game_id(self) -> str:
        """The game ID this plugin is registered for (derived from filename)."""
        ...

    def register_identity(
        self,
        steam_appid: int = 0,
        gog_id: str = "",
        epic_namespace: str = "",
        nexus_domain: str = "",
        exe_windows: str = "",
        exe_linux: str = "",
        exe_macos: str = "",
    ) -> None:
        """Register this game module's identity - pure data, no behavior."""
        ...

    def register_meta(
        self,
        author: str = "",
        version: str = "",
        description: str = "",
    ) -> None:
        """Register optional metadata for the Plugins settings tab.

        All fields are optional; empty strings leave them unset.
        """
        ...

    def register_category(self, category: str = "") -> None:
        """Declare this plugin's category shown as a foldable group in the
        Plugins settings tab (e.g. "Game Support", "Installer", "Tool").

        Empty string or a category outside the known set -> "Uncategorized".
        """
        ...

    def register_settings(self, settings: list[tuple[str, str]] = []) -> None:
        """Declare this plugin's user-facing options as plain key:value pairs.

        Rendered as editable rows in a scrollable container in the Plugins
        settings tab. Each entry is (key, default value). Persisted overrides
        are read back at startup; edits from the UI are written back.

        Source providers must NOT use this - their settings live on the
        Sources tab instead.
        """
        ...

    def register_settings_tab(
        self,
        title: str,
        settings: list[tuple[str, str, str, object | None]] = [],
    ) -> None:
        """Declare a typed settings tab rendered as native host widgets
        (P1.5). Each entry is (key, type, default, options):

          ("show_previews", "bool",   "1",    None)      -> checkbox
          ("max_threads",   "int",    "4",    "1:8")     -> spinbox (range)
          ("mod_name_prefix","string","mod_", None)      -> line edit
          ("install_mode",  "choice", "Full", ["Full", "Compact", "Minimal"])
                                                        -> combo box

        The host adds a Settings-dialog tab titled by `title` (one per
        plugin) after the fixed tabs. Edits persist through the same
        plugins/settings/<basename>/<key> store as register_settings; keys
        declared here stop rendering as raw key:value rows in the Plugins
        tab's info pane. `options` is None except: "choice" (list of
        candidate values) and "int" (the "min:max" range string).
        """
        ...

    def register_stage_claim(self, stage_name: str, priority: int = 0) -> None:
        """Claim exclusive ownership of a pipeline stage for this plugin's game.

        Highest priority wins for a given (stage, game_id) pair.
        Engine falls back to generic logic if nothing claims it.
        """
        ...

    def register_wildcard_stage_claim(
        self,
        game_id: str = "",
        stage_name: str = "",
        priority: int = 0,
    ) -> None:
        """Claim exclusive ownership of a pipeline stage for any/specific game.

        Like register_stage_claim but with an explicit game_id so a plugin
        can claim a stage for ANY game (game_id="" or omitted = wildcard)
        or for a specific game different from its own.  Resolution priority:
        exact-match claims beat wildcard claims at equal priority, so a
        game-specific plugin always wins over a generic wildcard.
        """
        ...

    def register_diagnostics(
        self, game_id: str = "", fn: Callable[[], str | list[str] | list[object]] | None = None
    ) -> None:
        """Register a diagnostics provider (IPluginDiagnose, v2).

        fn() is called with no arguments and must return either:
          - a string / list[str] of short problem messages, or
          - a list of problems, each a (short, full) tuple or a dict with keys
            "short_description", "full_description", "has_guided_fix" (int 0/1),
            and optionally "start_guided_fix" (a callable invoked to fix it).

        game_id scopes the provider to one game ("" = this plugin's game).
        Registered into both the v1 DiagnosticsRegistry (Plugins-tab tooltip)
        and the v2 DiagnoseRegistry.
        """
        ...

    def subscribe_event(
        self, event_id: str, fn: Callable[[str, dict[str, str]], None]
    ) -> None:
        """Subscribe to a host event bus event (MO2 signal analogue).

        fn is called as fn(event_id, payload) whenever the host emits the
        event; payload is a plain dict of str -> str (the bus payload is a
        JSON object of string values, decoded by the bridge). Canonical
        event ids and their payload keys — see gmm_abi_v1.h subscribe_event:

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

        Handlers run on the emitting thread; keep them short and non-blocking.
        """
        ...

    def register_order_encoding(self, fn: Callable[[list[str], str], int]) -> None:
        """Register a load-order writer (IPluginGame, v2).

        fn(ordered_mod_ids: list[str], output_path: str) -> int — write the
        game's load-order file (plugins.txt / metadata.xml, ...). Return 1 on
        success, 0 on failure. Registered into the OrderEncodingRegistry; the
        pipeline calls it when writing load order instead of the built-in hook.
        """
        ...

    def register_deploy_strategy(
        self,
        deploy_fn: Callable[[str, str], int],
        remove_fn: Callable[[str], int] | None = None,
    ) -> None:
        """Register a custom deployment strategy (IPluginGame, v2).

        deploy_fn(source: str, target: str) -> int places a single mod file
        into the game data directory; remove_fn(target: str) -> int removes it.
        Return 1 on success, 0 on failure. Registered into the
        DeployStrategyRegistry; the pipeline uses it instead of the built-in
        Deploy::Interface.
        """
        ...

    def register_image_diff(self) -> None:
        """Register an image diff provider for merging conflicting sprite files.
        
        Launched from the Conflicts tab context menu on conflicting files.
        """
        ...

    def register_tool(
        self, tool_id: str, kind: str, fn: Callable[[], None] | None = None
    ) -> None:
        """Register an external tool (IPluginTool, v2).

        kind: "advisory" (output feeds into pipeline) or "workshop" (user
        launches directly). fn, if given, is invoked when the tool runs
        (registered into the v2 PluginToolRegistry and the Tools menu).
        """
        ...

    def register_requirements(
        self, fn: Callable[[], list[tuple[str, str, str]]]
    ) -> None:
        """Declare plugin requirements (IPlugin requirements, v2).

        fn() -> list of (type, name, message) tuples, where type is one of
        "plugin", "game", "diagnose". The loader evaluates them once all plugins
        are loaded and surfaces any that are unmet.
        """
        ...

    def register_file_mapper(
        self, game_id: str = "", fn: Callable[[], list[tuple[str, str]]] = ...
    ) -> None:
        """Register a virtual file overlay mapper (IPluginFileMapper, v2).

        fn() -> list of (source, target) virtual-path pairs that the deploy
        pipeline turns into virtual file overlays. game_id scopes the mapper
        ("" = this plugin's game). Registered into the FileMapperRegistry.
        """
        ...

    def register_save_parser(
        self,
        game_id: str = "",
        fn: Callable[[str, str], dict] | None = None,
        priority: int = 0,
    ) -> None:
        """Register a save-game parser (IPluginSaveParser, v2).

        fn(path: str, game_id: str) -> dict | None — parse a save file and
        return a dict with keys file_path, game_id, creation_time (int),
        pc_name, pc_level (int), pc_location, save_number (int), plugins
        (list[str]), light_plugins (list[str]). Return None to decline. Higher
        priority wins on resolve. Registered into the SaveParserRegistry.
        """
        ...

    def register_hook(
        self,
        tag: str,
        data: str = "",
        fn: Callable[[str, str], None] | None = None,
        priority: int = 0,
    ) -> None:
        """Register a behavior-injection hook (IPluginGame, v2).

        fn(tag: str, data: str) is fired at pipeline points (before_deploy,
        after_scan, conflict_resolution) in priority order. Also stored as game
        knowledge (data payload). Registered into the v2 HookRegistry.
        """
        ...

    def register_preview(
        self, extension: str, fn: Callable[[str], int]
    ) -> None:
        """Register a file preview generator (IPluginPreview, v2).

        fn(file_path: str) -> int returns the raw QWidget* (e.g. obtained via
        shiboken6.getCppPointer(widget)[0] when building the widget with
        PySide6). The engine embeds the widget in the preview panel exactly like
        a C plugin. Registered into the UI Registry.
        """
        ...

    def register_modpage(
        self, url: str, fn: Callable[[str, str], int]
    ) -> None:
        """Register a ModPage download handler (IPluginModPage, v2).

        fn(url: str, output_path: str) -> int downloads the page/archive to
        output_path. Return 1 on success, 0 on failure.
        """
        ...

    def register_sort_provider(
        self, fn: Callable[[list[str]], list[str]]
    ) -> None:
        """Register a load-order sort provider (IPluginGame, v2).

        fn(mod_folders: list[str]) -> list[str] returns the sorted mod folder
        names. Registered into the SortRegistry for this plugin's game.
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
    ) -> None:
        """Register this plugin as a game module (IPluginGame identity, v2).

        Sets the game id, display name, and store/app ids. Marks the plugin as
        providing game support (so it can back an instance).
        """
        ...

    def register_capability(
        self,
        capability: str,
        display_name: str = "",
        data_path: str = "",
        description: str = "",
        protocol_handler: str = "",
        website_domain: str = "",
        supported_platforms: str = "",
    ) -> None:
        """Register a game capability (non-tab feature).

        capability: "plugins", "archives", "saves", "downloads"
        display_name: label for the capability
        data_path: relative path where these are stored (e.g. "Data/")
        For downloads: protocol_handler ("nxm", "workshop"), website_domain, supported_platforms (comma-separated)
        """

    def register_game_feature(
        self,
        game_id: str = "",
        feature_type: str = "",
        priority: int = 0,
        folder_names: list[str] = [],
        file_extensions: list[str] = [],
    ) -> None:
        """Register or override a per-game behavior feature (MO2 IGameFeatures).

        feature_type: "mod_data_checker" (first). The game's own feature
            registers at the LOWEST priority; registering the same type at a
            higher priority overrides/augments it (MO2 combines all checkers:
            ANY checker VALID -> VALID, so the union's allow-sets drive the mod
            list's "No valid game data" flag).
        game_id: the game this feature serves ("" = this plugin's own game).
        folder_names/file_extensions: mod_data_checker allow-sets (top-level
            directory names / file extensions that count as real game data).
        """
        ...

    def register_game_feature_data(
        self,
        game_id: str = "",
        feature_type: str = "",
        priority: int = 0,
        data: dict[str, str] = {},
    ) -> None:
        """Register a per-game feature whose payload is key/value pairs (the
        7 structured-data feature types; register_game_feature covers the two
        array-payload types). Same priority + replace semantics.

        Keys per feature_type (see gmm_abi_v1.h register_game_feature_data):
          "mod_data_content"  — "enabled": comma-separated catalog content IDs
                                (plugin, optional, interface, mesh, bsa,
                                script, skse, skse_files, skyproc, sound,
                                texture, mcm, ini, facegen, modgroup);
                                "content:<id>": "name|icon|filter_only".
          "data_archives"     — "vanilla_archives": comma-separated archive names.
          "script_extender"   — "binary", "plugin_path", "loader_name",
                                "savegame_extension".
          "save_game_info"    — "extensions": comma-separated save extensions.
          "local_savegames"   — "saves_subpath", "ini_file".
          "unmanaged_mods"    — "mods": comma-separated internal mod names.
          "bsa_invalidation"  — "bsa_name", "bsa_version".
        game_id: the game this feature serves ("" = this plugin's own game).
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
    ) -> None:
        """Register a UI tab for this game with ordering.

        capability: "plugins", "archives", "saves", "downloads", "conflicts", "data"
        display_name: tab label shown in the right panel
        insert_before: capability_id this tab should appear before
        insert_after: capability_id this tab should appear after
        """
        ...
