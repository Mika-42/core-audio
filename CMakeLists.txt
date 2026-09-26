cmake_minimum_required(VERSION 4.0)

project(mka-core-audio VERSION 0.1.0
DESCRIPTION "Backend-agnostic real-time audio framework"
LANGUAGES CXX
)

# ============================================================
# Project options
# ============================================================

option(MKA_BUILD_TESTS
"Build unit and integration tests"
ON
)

option(MKA_BUILD_ALSA
"Build the ALSA backend"
ON
)

option(MKA_BUILD_PIPEWIRE
"Build the PipeWire backend"
OFF
)

option(MKA_BUILD_JACK
"Build the JACK backend"
OFF
)


# ============================================================
# Global C++ configuration
# ============================================================

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# Core library
# ============================================================

add_library(mka-core-audio)

target_sources(mka-core-audio
PUBLIC
FILE_SET CXX_MODULES
BASE_DIRS
src
FILES
src/core/abstract_backend.cppm
src/core/audio_callback.cppm
src/core/endpoint.cppm
src/core/constants.cppm
src/core/error.cppm
)

target_compile_features(
mka-core-audio
PUBLIC
cxx_std_26
)

# ============================================================
# ALSA backend
# ============================================================

if(MKA_BUILD_ALSA)

find_package(ALSA REQUIRED)

add_library(mka-audio-alsa)

target_sources(mka-audio-alsa
PUBLIC
FILE_SET CXX_MODULES
BASE_DIRS
src
FILES
src/backend/linux/alsa.cppm
)

target_link_libraries(mka-audio-alsa
PRIVATE
mka-core-audio
ALSA::ALSA
)

target_compile_features(
mka-audio-alsa
PRIVATE
cxx_std_26
)

endif()

# ============================================================
# PipeWire backend
# ============================================================

if(MKA_BUILD_PIPEWIRE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

add_library(mka-audio-pipewire)

target_sources(mka-audio-pipewire
PUBLIC
FILE_SET CXX_MODULES
BASE_DIRS
src
FILES
src/backend/linux/pipewire.cppm
)

target_link_libraries(mka-audio-pipewire
PRIVATE
mka-core-audio
PkgConfig::PIPEWIRE
)

target_compile_features(
mka-audio-pipewire
PRIVATE
cxx_std_26
)

endif()

# ============================================================
# JACK backend
# ============================================================

if(MKA_BUILD_JACK)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)

add_library(mka-audio-jack)

target_sources(mka-audio-jack
PUBLIC
FILE_SET CXX_MODULES
BASE_DIRS
src
FILES
src/backend/linux/jack.cppm
)

target_link_libraries(mka-audio-jack
PRIVATE
mka-core-audio
PkgConfig::JACK
)

target_compile_features(
mka-audio-jack
PRIVATE
cxx_std_26
)

endif()

# ============================================================
# Tests
# ============================================================

if(MKA_BUILD_TESTS)

enable_testing()

find_package(GTest REQUIRED)

# --------------------------------------------------------
# Core tests
# --------------------------------------------------------

add_executable(mka-core-audio-tests
        test/test_alsa.cpp
        test/test_jack.cpp
        test/test_pipewire.cpp
)

target_link_libraries(mka-core-audio-tests
PRIVATE
mka-core-audio
GTest::gtest_main
)

target_compile_features(
mka-core-audio-tests
PRIVATE
cxx_std_26
)

include(GoogleTest)

gtest_discover_tests(
mka-core-audio-tests
TEST_PREFIX "core."
)

# --------------------------------------------------------
# ALSA tests
# --------------------------------------------------------

if(MKA_BUILD_ALSA)

add_executable(mka-audio-alsa-tests
        test/test_alsa.cpp
)

target_link_libraries(mka-audio-alsa-tests
PRIVATE
mka-audio-alsa
GTest::gtest_main
)

target_compile_features(
mka-audio-alsa-tests
PRIVATE
cxx_std_26
)

gtest_discover_tests(
mka-audio-alsa-tests
TEST_PREFIX "alsa."
)

endif()

# --------------------------------------------------------
# PipeWire tests
# --------------------------------------------------------

if(MKA_BUILD_PIPEWIRE)

add_executable(mka-audio-pipewire-tests
        test/test_pipewire.cpp
)

target_link_libraries(mka-audio-pipewire-tests
PRIVATE
mka-audio-pipewire
GTest::gtest_main
)

target_compile_features(
mka-audio-pipewire-tests
PRIVATE
cxx_std_26
)

gtest_discover_tests(
mka-audio-pipewire-tests
TEST_PREFIX "pipewire."
)

endif()

# --------------------------------------------------------
# JACK tests
# --------------------------------------------------------

if(MKA_BUILD_JACK)

add_executable(mka-audio-jack-tests
        test/test_jack.cpp
)

target_link_libraries(mka-audio-jack-tests
PRIVATE
mka-audio-jack
GTest::gtest_main
)

target_compile_features(
mka-audio-jack-tests
PRIVATE
cxx_std_26
)

gtest_discover_tests(
mka-audio-jack-tests
TEST_PREFIX "jack."
)

endif()
endif()