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

    def register_stage_claim(self, stage_name: str, priority: int = 0) -> None:
        """Claim exclusive ownership of a pipeline stage.

        Highest priority wins for a given (stage, game_id) pair.
        Engine falls back to generic logic if nothing claims it.
        """
        ...

    def register_diagnostics(self, fn: Callable[[str], str | list[str]]) -> None:
        """Register a diagnostics provider for this plugin's game.

        Called once per plugin on every Plugins-tab refresh with the plugin's
        file name; return a message string or list of messages to append to
        that plugin's hover tooltip (below an <hr> separator).
        """
        ...

    def register_order_encoding_hook(self) -> None:
        """Register a load-order writer (plugins.txt / metadata.xml style)."""
        ...

    def register_deploy_strategy(self) -> None:
        """Register a custom deployment strategy (symlink / hardlink / vfs)."""
        ...

    def register_image_diff(self) -> None:
        """Register an image diff provider for merging conflicting sprite files.
        
        Launched from the Conflicts tab context menu on conflicting files.
        """
        ...

    def register_tool(self, tool_id: str, kind: str) -> None:
        """Register an external tool (LOOT, BodySlide, etc.).

        kind: "advisory" (output feeds into pipeline) or "workshop" (user launches directly).
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
