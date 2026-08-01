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

    def register_stage_claim(self, stage_name: str, priority: int = 0) -> None:
        """Claim exclusive ownership of a pipeline stage.

        Highest priority wins for a given (stage, game_id) pair.
        Engine falls back to generic logic if nothing claims it.
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
