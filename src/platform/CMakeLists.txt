# Platform module — Qt-free static library. Compiles only the current OS's
# implementation (LinuxPlatform / WindowsPlatform behind PlatformInterface).

add_library(gmm_platform STATIC
    ${GMM_PLATFORM}/${GMM_PLATFORM}_platform.cpp
)

target_include_directories(gmm_platform PUBLIC ${PROJECT_SOURCE_DIR}/src)
