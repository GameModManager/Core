# CLI - Qt-free headless launcher (the `--launch` mode of gamemodmanager).
# Compiled as a static library and linked into the app executable so the CLI
# and GUI share one binary, exactly as before the CMake split.

add_library(gmm_cli STATIC
    headless_launcher.cpp
)

target_include_directories(gmm_cli PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(gmm_cli PRIVATE gmm_engine gmm_runtime gmm_platform)
