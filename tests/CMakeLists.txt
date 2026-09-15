# The `web` variant's backend, compiled NATIVELY and driven from a test.
#
# A standalone project rather than a target in the module's own CMakeLists: the
# module build is logos-module-builder's, generated from metadata.json, and this
# is not part of any artefact that ships. `nix/web-backend-test.nix` configures
# it; the header of `web_backend_test.cpp` says what it asserts.
#
# THE SOURCE UNDER TEST IS THE SHIPPED ONE — ../src/wallet_ui_web_backend.cpp,
# the same translation unit the Qt-wasm image compiles. What is swapped is the
# one door it reaches the container through (tests/fake_door.cpp), which is the
# only symbol in it that a native binary has no implementation of.
cmake_minimum_required(VERSION 3.21)
project(wallet_ui_web_backend_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core RemoteObjects)

# logos_web_module_call.h — logos-module-builder's `wasm/` directory. Declared
# there and implemented only inside the Wasm host, which is exactly why this
# build supplies its own implementation.
if(NOT LOGOS_WEB_DOOR_DIR)
  message(FATAL_ERROR
    "LOGOS_WEB_DOOR_DIR must point at logos-module-builder's wasm/ directory "
    "(it holds logos_web_module_call.h)")
endif()

add_executable(web_backend_test
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/wallet_ui_web_backend.cpp
  fake_door.cpp
  web_backend_test.cpp
)

# The SOURCE side of the contract, which is what a backend implements. The same
# generated base class the plugin and the wasm image compile against.
qt6_add_repc_sources(web_backend_test ${CMAKE_CURRENT_SOURCE_DIR}/../src/wallet_ui.rep)

target_include_directories(web_backend_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
  # Where repc writes rep_wallet_ui_source.h, which the backend's header
  # includes by name exactly as the module build does.
  ${CMAKE_CURRENT_BINARY_DIR}
  ${LOGOS_WEB_DOOR_DIR}
)
target_link_libraries(web_backend_test PRIVATE Qt6::Core Qt6::RemoteObjects)
