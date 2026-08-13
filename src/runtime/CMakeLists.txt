# Runtime — Qt-free static library (Proton/Wine runtime discovery and launch).
# Depends on the platform interface for Steam root / prefix lookups.

add_library(gmm_runtime STATIC
    runtime.cpp
    wine_runtime.cpp
)

target_include_directories(gmm_runtime PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(gmm_runtime PRIVATE gmm_platform)
