#
# CMakeLists.txt  cmake for xthread
# 21-Jul-2026  chuck@ece.cmu.edu
#

# add subdirs
add_subdirectory(libxthread)
if (DTHREAD_TESTING)
    add_subdirectory(testin)
endif()
