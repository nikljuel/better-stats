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
    qt/src/installer.cpp
    src/tracker.c
    src/stats_db.c
    src/stats_model.c
    src/daemon.c
    src/daemon_singleton.c
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

# Qt-free fallback and daemon. The default builder produces the classic
# PocketBook softfp ABI; the hardfp target builds the same source list separately.
add_executable(BetterStatsInkView
  inkview/main.c
  src/tracker.c
  src/stats_db.c
  src/stats_model.c
  src/daemon.c
  src/daemon_singleton.c
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
