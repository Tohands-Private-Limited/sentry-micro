# sentry-micro — idf_basic
#
# The DSN, release and WiFi credentials come from the environment at configure time, the same
# way examples/zephyr_basic and the PlatformIO examples take them, so nothing secret lives in a
# file: not here, not in sdkconfig. They are read in main/CMakeLists.txt when CMake configures;
# change one and run `idf.py reconfigure` before the next build.
#
# sentry-micro itself comes from this repository's root, by path, through
# main/idf_component.yml.

cmake_minimum_required(VERSION 3.22)

# idf.py -DSENTRY_DEMO_CRASH=1 build: crash once after the boot event (for the crash path).
set(SENTRY_DEMO_CRASH 0 CACHE STRING "Crash deliberately after reporting the boot (0 or 1)")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build main, sentry-micro and what they depend on, not every component ESP-IDF ships.
idf_build_set_property(MINIMAL_BUILD ON)
project(sentry_micro_idf_basic)
