#
# Copyright (c) 2026, Carnegie Mellon University.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. Neither the name of the University nor the names of its contributors
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
# HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
# OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
# AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
# WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#

#
# CMakeLists.txt  cmake for dthread
# 14-Apr-2026  chuck@ece.cmu.edu
#

#
# set CMAKE_INSTALL_PREFIX to specify an installation directory.
# set CMAKE_PREFIX_PATH as a search path for external packages.
#
cmake_minimum_required(VERSION 3.10)
project (dthread C)

#
# packages we always want to look for, typically required
#
find_package(Threads REQUIRED)		# Threads::Threads pthreads target
find_package(MPI MODULE REQUIRED)	# MPI::MPI_C target

#
# helpful cmake functions
#

# tryoption: like ON/OFF option(), but with TRY/ON/OFF.
function(tryoption optname helptext defvalue)
    set(allowed on off try)
    set(${optname} ${defvalue} CACHE STRING "${helptext}")
    set_property(CACHE ${optname} PROPERTY STRINGS ${allowed})  # for ccmake
    string(TOLOWER "${${optname}}" ${optname})
    if (NOT "${${optname}}" IN_LIST allowed)
        message(FATAL_ERROR "${optname} has invalid value ${${optname}} - "
                "allowed values: ${allowed}")
    endif()
    set(${optname} "${${optname}}" PARENT_SCOPE)  # push up to parent
endfunction()

#
# compile time configuration options
#

#
# synchronization operation to compile in:
#   options: ON (required), OFF (disabled), TRY (enable if found)
#
set(DTHREAD_SYNCOPS "")      # put internal list of syncops to compile in here

# pshared: pthread-based sync ops (all threads must be on one node)
tryoption(DTHREAD_SYNC_PSHARED "pthread-based sync ops" on)
if (NOT DTHREAD_SYNC_PSHARED STREQUAL off)
    # already did find_package on Threads
    if (DTHREAD_SYNC_PSHARED STREQUAL on AND NOT TARGET Threads::Threads)
        message(FATAL_ERROR "Threads target required for pshared sync ops")
    endif()
    # overwrite 'try' with on or off for '#cmakedefine' in dthread_config.h.in
    if (TARGET Threads::Threads)
        list(APPEND DTHREAD_SYNCOPS pshared)
        set(DTHREAD_SYNC_PSHARED on)
    else()
        set(DTHREAD_SYNC_PSHARED off)
    endif()
endif()

# check and report config back to user
if (NOT DTHREAD_SYNCOPS)
    message(FATAL_ERROR "no dthread synchronization ops found and enabled")
endif()
message(STATUS "enabled dthread synchronization ops: ${DTHREAD_SYNCOPS}")

#
# mlog: optional logging for debugging/testing dthread libs, currently
#       mainly for developers, so not intended for production builds.
#
option(DTHREAD_MLOG "Build with mlog for debugging" OFF)
option(DTHREAD_MLOG_EXPORT "Install mlog headers" OFF)
mark_as_advanced(DTHREAD_MLOG DTHREAD_MLOG_EXPORT)
if (DTHREAD_MLOG_EXPORT AND NOT DTHREAD_MLOG)
    message(FATAL_ERROR "DTHREAD_MLOG_EXPORT requires setting DTHREAD_MLOG")
endif()

# testing...
option(DTHREAD_TESTING "Enable dthread testing" OFF)
if (DTHREAD_TESTING)
    enable_testing()
endif()

# xthread lib
option(DTHREAD_XTHREAD "Enable xthread pthread/dthread adaptor lib" ON)

# cmake build type, shared lib option
if (NOT CMAKE_BUILD_TYPE)
    set (CMAKE_BUILD_TYPE Release
         CACHE STRING "Choose the type of build." FORCE)
    set_property (CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS   # for ccmake
                  "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif ()
option(BUILD_SHARED_LIBS "Build with shared libs" ON)

# globally enable -Wall if we are using clang or gcc
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall)
endif()

# avoid having to set LD_LIBRARY_PATH for non-standard install dirs
set (CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# add subdirs
add_subdirectory(libdthread)
if (DTHREAD_TESTING)
    add_subdirectory(testin)
endif()
if (DTHREAD_XTHREAD)
    add_subdirectory(xthread)
endif()
