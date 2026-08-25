cmake_minimum_required(VERSION 3.14)
project(inputstream.airplay)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Kodi REQUIRED)
# Not used directly here. UxPlay's lib/CMakeLists.txt calls pkg_check_modules
# for libplist, and finding PkgConfig is the including project's job.
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# "/.." so <kodi/...> resolves; the same shape the other binary add-ons use.
include_directories(${KODI_INCLUDE_DIR}/..
                    ${PROJECT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Optional Kodi features
#
# Starting a live stream at its live edge is not in Kodi yet. Where the running
# Kodi's headers know about it the add-on asks for it, and where they do not it
# simply does not, so the same source builds against either.
# ---------------------------------------------------------------------------
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${KODI_INCLUDE_DIR}/..)
check_cxx_source_compiles("
#include <kodi/c-api/addon-instance/inputstream.h>
int main() { return INPUTSTREAM_LOW_LATENCY_LIVE; }
" HAVE_INPUTSTREAM_LOW_LATENCY_LIVE)
unset(CMAKE_REQUIRED_INCLUDES)

if(HAVE_INPUTSTREAM_LOW_LATENCY_LIVE)
  add_compile_definitions(HAVE_INPUTSTREAM_LOW_LATENCY_LIVE)
endif()

# ---------------------------------------------------------------------------
# AirPlay protocol library
#
# UxPlay's lib/, pinned by commit and checksum. Nothing here is patched: the
# fixes this add-on needed are upstream, and keeping it that way means a newer
# UxPlay is a two line change rather than a rebasing exercise.
#
# Only the protocol half is built. UxPlay's renderers/ and uxplay.cpp are
# GStreamer, and the point of this add-on is to hand the streams to Kodi
# instead, so its top level CMakeLists is never used.
#
# The source comes from whichever of these knows about it:
#
#  - UXPLAY_SOURCE_DIR, for a distribution that fetches its own sources.
#  - Kodi's add-on buildsystem, which builds depends/common/uxplay first from
#    the tarball cache or mirrors.kodi.tv. That is how the binary add-on build
#    farm expects a third-party source to arrive, and the commit and checksum
#    live in depends/common/uxplay/ -- keep them in step with the two below.
#  - Otherwise a plain download, so a bare cmake build works on its own.
# ---------------------------------------------------------------------------
set(UXPLAY_VERSION "462153392f2e30937424922039ff9f0cda5e7b1a")
set(UXPLAY_SHA256 "0f5bf6f2ab3d1ac3ccaf4418afe00137ec1ba97c2a345710202fca40f6a1fb85")

set(UXPLAY_SOURCE_DIR "" CACHE PATH "Unpacked UxPlay source, instead of fetching")

# Also from the environment. Kodi's add-on buildsystem hands user arguments to
# an add-on's *depends* rather than to the add-on itself, so -D on the outer
# cmake never arrives here; the environment does, and it saves a distribution
# threading the value through dh_auto_configure as well.
if(NOT UXPLAY_SOURCE_DIR AND DEFINED ENV{UXPLAY_SOURCE_DIR})
  set(UXPLAY_SOURCE_DIR "$ENV{UXPLAY_SOURCE_DIR}")
endif()

if(UXPLAY_SOURCE_DIR)
  if(NOT EXISTS "${UXPLAY_SOURCE_DIR}/lib/raop.h")
    message(FATAL_ERROR "UXPLAY_SOURCE_DIR=${UXPLAY_SOURCE_DIR} does not look like UxPlay")
  endif()
  set(uxplay_SOURCE_DIR "${UXPLAY_SOURCE_DIR}")
  message(STATUS "inputstream.airplay: using UxPlay from ${uxplay_SOURCE_DIR}")
elseif(ADDON_DEPENDS_PATH AND EXISTS "${ADDON_DEPENDS_PATH}/src/uxplay/lib/raop.h")
  set(uxplay_SOURCE_DIR "${ADDON_DEPENDS_PATH}/src/uxplay")
  message(STATUS "inputstream.airplay: using UxPlay from the add-on depends at ${uxplay_SOURCE_DIR}")
else()
  include(FetchContent)
  message(STATUS "inputstream.airplay: fetching UxPlay ${UXPLAY_VERSION}")
  # SOURCE_SUBDIR names a directory with no CMakeLists.txt in it, which is how
  # MakeAvailable is told to unpack and stop -- UxPlay's own top level needs
  # GStreamer. The alternative, FetchContent_Populate(uxplay), is deprecated
  # as of CMake 3.30 and warns.
  FetchContent_Declare(uxplay
    URL           "https://github.com/FDH2/UxPlay/archive/${UXPLAY_VERSION}.tar.gz"
    URL_HASH      SHA256=${UXPLAY_SHA256}
    SOURCE_SUBDIR nothing-to-build-here)
  FetchContent_MakeAvailable(uxplay)
endif()

set(NO_MARCH_NATIVE ON CACHE BOOL "" FORCE)  # upstream turns this on for x86

# The subdirectories lib/ expects, which UxPlay's top level would normally add.
foreach(_part llhttp playfair mdnsd)
  add_subdirectory(${uxplay_SOURCE_DIR}/lib/${_part} uxplay-${_part})
endforeach()
add_subdirectory(${uxplay_SOURCE_DIR}/lib uxplay-lib)

# playfair's modified MD5 calls sin(), which upstream never declares. Passing
# -lm further along the link line is not enough: a linker using --as-needed
# has already discarded libm by the time the reference turns up, so say it
# here, where it is true, and let it propagate.
if(UNIX AND NOT APPLE)
  target_link_libraries(playfair PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# airplay-receiver
#
# The protocol runs in a process of its own. It parses hostile network input,
# and a fault there stops a helper rather than taking Kodi with it.
# ---------------------------------------------------------------------------
add_executable(airplay-receiver src/receiver/receiver.c)
target_include_directories(airplay-receiver PRIVATE
                           ${uxplay_SOURCE_DIR}/lib
                           ${uxplay_SOURCE_DIR}/lib/mdnsd
                           ${PROJECT_SOURCE_DIR}/src)
# Threads::Threads rather than a bare pthread: it also puts -pthread on the
# compile line, which is what the toolchain actually wants. libm arrives with
# playfair.
target_link_libraries(airplay-receiver PRIVATE airplay Threads::Threads)

# ---------------------------------------------------------------------------
# Kodi add-on
# ---------------------------------------------------------------------------
set(AIRPLAY_SOURCES src/addon/InputStreamAirPlay.cpp)
set(AIRPLAY_HEADERS src/addon/InputStreamAirPlay.h
                    src/proto.h)

# Ships inside the add-on directory alongside the library.
set(AIRPLAY_ADDITIONAL_BINARY_EXE $<TARGET_FILE:airplay-receiver>)

build_addon(inputstream.airplay AIRPLAY DEPLIBS)

# Strict warnings for the code written here, and only for it. Setting them
# through CMAKE_C_FLAGS instead would apply them to UxPlay's sources too, which
# do not build warning-free and are not ours to fix.
option(AIRPLAY_STRICT_WARNINGS "Build this add-on's own sources with -Wall -Wextra -Werror" OFF)
# From the environment as well, for the same reason UXPLAY_SOURCE_DIR is: -D on
# the outer cmake reaches an add-on's depends, not the add-on.
if(NOT AIRPLAY_STRICT_WARNINGS AND DEFINED ENV{AIRPLAY_STRICT_WARNINGS})
  set(AIRPLAY_STRICT_WARNINGS "$ENV{AIRPLAY_STRICT_WARNINGS}")
endif()
if(AIRPLAY_STRICT_WARNINGS)
  message(STATUS "inputstream.airplay: strict warnings on for this add-on's own sources")
  foreach(_target airplay-receiver inputstream.airplay)
    if(TARGET ${_target})
      target_compile_options(${_target} PRIVATE -Wall -Wextra -Werror)
    endif()
  endforeach()
endif()

include(CPack)
