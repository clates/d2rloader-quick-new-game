cmake_minimum_required(VERSION 3.29)
project(D2RackLogPlugins LANGUAGES CXX RC)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build on Linux with:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain-xwin.cmake -DCMAKE_BUILD_TYPE=Release
#   /usr/bin/ninja -C build
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(PluginSDK)

add_library(QuickNewGame SHARED quick-new-game/quick_new_game.cpp quick-new-game/quick_new_game.rc)
target_link_libraries(QuickNewGame PRIVATE D2RLPlugin::D2RLPlugin)
set_target_properties(QuickNewGame PROPERTIES OUTPUT_NAME "quick-new-game")
target_compile_options(QuickNewGame PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W3;/permissive-;/EHsc>")
