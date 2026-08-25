# Replaces UxPlay's own top level, which the add-on buildsystem copies over it.
#
# Nothing is built here. UxPlay's build wants GStreamer and produces a player
# this add-on does not use; only the protocol library under lib/ is wanted, and
# that is added to the add-on's own build so it is compiled with the same flags
# as the rest of it. So all this has to do is put the source somewhere the
# add-on can find it.
#
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.14)
project(uxplay NONE)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib
        DESTINATION src/uxplay)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        DESTINATION src/uxplay
        OPTIONAL)
