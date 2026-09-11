cmake_minimum_required(VERSION 3.20)
project(duck-pngtuber VERSION 1.0.0 LANGUAGES C)

# Point CMAKE_PREFIX_PATH at the libobs CMake package from your OBS build.
# Example:
#   -DCMAKE_PREFIX_PATH=C:/path/to/obs-studio/build_x64/libobs

find_package(libobs REQUIRED)

add_library(duck-pngtuber MODULE
    src/duck-pngtuber.c
)

target_link_libraries(duck-pngtuber PRIVATE OBS::libobs)

target_compile_features(duck-pngtuber PRIVATE c_std_11)

# OBS plugins are loaded as modules, not executables.
set_target_properties(duck-pngtuber PROPERTIES
    PREFIX ""
    OUTPUT_NAME "duck-pngtuber"
)

# Optional: set this to your OBS install's obs-plugins/64bit directory.
set(OBS_PLUGIN_OUTPUT_DIR "" CACHE PATH
    "Directory where the built OBS plugin DLL should be placed")

if(OBS_PLUGIN_OUTPUT_DIR)
    set_target_properties(duck-pngtuber PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${OBS_PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${OBS_PLUGIN_OUTPUT_DIR}"
    )
endif()
