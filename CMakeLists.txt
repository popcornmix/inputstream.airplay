cmake_minimum_required(VERSION 3.14)
project(inputstream.airplay)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Kodi REQUIRED)
find_package(PkgConfig REQUIRED)

# "/.." so <kodi/...> resolves; the same shape the other binary add-ons use.
include_directories(${KODI_INCLUDE_DIR}/..
                    ${PROJECT_SOURCE_DIR}/src)

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
# Point UXPLAY_SOURCE_DIR at an unpacked UxPlay to build without fetching.
# ---------------------------------------------------------------------------
set(UXPLAY_VERSION "462153392f2e30937424922039ff9f0cda5e7b1a")
set(UXPLAY_SHA256 "0f5bf6f2ab3d1ac3ccaf4418afe00137ec1ba97c2a345710202fca40f6a1fb85")

set(UXPLAY_SOURCE_DIR "" CACHE PATH "Unpacked UxPlay source, instead of fetching")

if(UXPLAY_SOURCE_DIR)
  if(NOT EXISTS "${UXPLAY_SOURCE_DIR}/lib/raop.h")
    message(FATAL_ERROR "UXPLAY_SOURCE_DIR=${UXPLAY_SOURCE_DIR} does not look like UxPlay")
  endif()
  set(uxplay_SOURCE_DIR "${UXPLAY_SOURCE_DIR}")
  message(STATUS "inputstream.airplay: using UxPlay from ${uxplay_SOURCE_DIR}")
else()
  include(FetchContent)
  FetchContent_Declare(uxplay
    URL      "https://github.com/FDH2/UxPlay/archive/${UXPLAY_VERSION}.tar.gz"
    URL_HASH SHA256=${UXPLAY_SHA256})
  FetchContent_GetProperties(uxplay)
  if(NOT uxplay_POPULATED)
    message(STATUS "inputstream.airplay: fetching UxPlay ${UXPLAY_VERSION}")
    FetchContent_Populate(uxplay)  # populate only: its top level needs GStreamer
  endif()
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
target_link_libraries(airplay-receiver PRIVATE airplay pthread)  # libm arrives with playfair

# ---------------------------------------------------------------------------
# Kodi add-on
# ---------------------------------------------------------------------------
set(AIRPLAY_SOURCES src/addon/InputStreamAirPlay.cpp)
set(AIRPLAY_HEADERS src/addon/InputStreamAirPlay.h
                    src/proto.h)

# Ships inside the add-on directory alongside the library.
set(AIRPLAY_ADDITIONAL_BINARY_EXE $<TARGET_FILE:airplay-receiver>)

build_addon(inputstream.airplay AIRPLAY DEPLIBS)

include(CPack)
