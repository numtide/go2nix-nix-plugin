cmake_minimum_required(VERSION 3.16)
project(go2nix-nix-plugin)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NIX REQUIRED nix-expr>=2.33 nix-util>=2.33)

find_package(nlohmann_json REQUIRED)
find_package(tomlplusplus REQUIRED)

add_library(go2nix_nix_plugin MODULE
  helpers.cc
  resolve_go_modules.cc
  resolve_go_packages.cc
)
target_include_directories(go2nix_nix_plugin PUBLIC ${NIX_INCLUDE_DIRS})
target_compile_options(go2nix_nix_plugin PUBLIC ${NIX_CFLAGS_OTHER})
target_link_libraries(go2nix_nix_plugin nlohmann_json::nlohmann_json tomlplusplus::tomlplusplus)

if(APPLE)
  set_target_properties(go2nix_nix_plugin PROPERTIES
    LINK_FLAGS "-flat_namespace -undefined suppress")
endif()

install(TARGETS go2nix_nix_plugin DESTINATION lib/nix/plugins)
