cmake_minimum_required(VERSION 3.21)
project(betterstats CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/pocketbook-sdk-qt6)

pocketbook_add_app(BetterStats
  SOURCES
    qt/src/main.cpp
    qt/src/inkview_bridge.cpp
    qt/src/stats_bridge.cpp
    qt/src/hardcover_bridge.cpp
    qt/src/hardcover_worker.cpp
    qt/src/installer.cpp
    src/tracker.c
    src/stats_db.c
    src/stats_model.c
    src/daemon.c
    src/paths.c
    src/file_handler_config.c
    src/autostart.c
    src/updater.c
    src/sha256.c
    qt/third_party/sqlite3.c
    qt/third_party/miniz.c
  QRC qt/qml/betterstats.qrc
)

set_target_properties(BetterStats PROPERTIES
  OUTPUT_NAME "betterstats-qt-softfp"
  SUFFIX ""
)

target_include_directories(BetterStats PRIVATE qt/third_party src qt/src)
target_compile_definitions(BetterStats PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)
target_link_libraries(BetterStats PRIVATE m pthread dl)

# Hardcover sync needs QNetworkAccessManager. Deliberately NOT calling
# find_package(Qt6 ... COMPONENTS Network): pocketbook-sdk-qt6's own
# add_subdirectory() above already hand-declares Qt6::<module> targets
# directly from .so files in the toolchain sysroot (confirmed by reading
# its own cmake/PocketBookQt.cmake) - Network is already one of its
# candidate modules. Calling find_package a second time conflicts with
# that (a real CMake/Qt bug: "some but not all targets in this export set
# were already defined"). Linking directly against the already-declared
# target instead.
target_link_libraries(BetterStats PRIVATE Qt6::Network)

# Qt-free fallback and daemon. The default builder produces the classic
# PocketBook softfp ABI; the hardfp target builds the same source list separately.
add_executable(BetterStatsInkView
  inkview/main.c
  src/tracker.c
  src/stats_db.c
  src/stats_model.c
  src/daemon.c
  src/paths.c
  src/file_handler_config.c
  src/autostart.c
  src/updater.c
  src/sha256.c
  qt/third_party/sqlite3.c
  qt/third_party/miniz.c
)
set_target_properties(BetterStatsInkView PROPERTIES
  OUTPUT_NAME "betterstats-inkview-softfp"
  SUFFIX ""
)
target_include_directories(BetterStatsInkView PRIVATE qt/third_party src)
target_compile_definitions(BetterStatsInkView PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)
target_link_libraries(BetterStatsInkView PRIVATE PocketBook::InkView m pthread dl)
