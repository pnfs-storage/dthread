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
# CMakeLists.txt  cmake for dthread tests
# 16-Jun-2026  chuck@ece.cmu.edu
#

#
# helper function to cut back on verbosity of test setup
#
function(dthread_test testname)

    # parse args
    set(options )
    set(options1v NPROC TESTSCRIPT TIMEOUT)
    set(optionsmultiv COMMAND)
    cmake_parse_arguments(PARSE_ARGV 1 DTT
                          "${options}" "${options1v}" "${optionsmultiv}")

    # handle args needed to define test
    if (NOT DEFINED DTT_NPROC)
        set(DTT_NPROC 1)
    elseif (NOT ${DTT_NPROC} GREATER 0)
        message(FATAL_ERROR "dthread_test ${testname} bad NPROC value")
    endif()

    # disable testscript w/o mlog -- testscript assumes parsable mlog output
    if (DTT_TESTSCRIPT AND NOT DTHREAD_MLOG)
        unset(DTT_TESTSCRIPT)
    endif()

    if (DTT_TESTSCRIPT)
        if (NOT EXISTS "${DTT_TESTSCRIPT}")
           if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${DTT_TESTSCRIPT}")
               set(DTT_TESTSCRIPT
                    "${CMAKE_CURRENT_SOURCE_DIR}/${DTT_TESTSCRIPT}")
           else()
               message(FATAL_ERROR
               "dthread_test: no testscript ${DTT_TESTSCRIPT}")
           endif()
        endif()
    endif()

    if (DTT_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "dthread_test: bad args: ${DTT_UNPARSED_ARGUMENTS}")
    endif()

    # create test
    if (DTT_TESTSCRIPT)
        add_test(NAME ${testname}
                 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/dt-testwrap -e -p
                     -s ${DTT_TESTSCRIPT}
                     ${MPIEXEC_EXECUTABLE} -n ${DTT_NPROC} ${DTT_COMMAND})
    else()
        add_test(NAME ${testname}
                 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/dt-testwrap -p
                     ${MPIEXEC_EXECUTABLE} -n ${DTT_NPROC} ${DTT_COMMAND})
    endif()

    # handle args that modify test properites
    if (DTT_TIMEOUT GREATER 0)
        set_tests_properties(${testname} PROPERTIES TIMEOUT ${DTT_TIMEOUT})
    else()
        set_tests_properties(${testname} PROPERTIES TIMEOUT 60)
    endif()

    if (DTT_TESTSCRIPT)
        set(envs DTHREAD_MLOG_STDERRPRI=DBUG DTHREAD_MLOG_DEFPRI=ERR
                 DTHREAD_MLOG_DIR=/tmp DTHREAD_MLOG_MASK=MGR=DBUG)
        set_tests_properties(${testname} PROPERTIES ENVIRONMENT "${envs}")
    endif()
endfunction()

#
# dt-testwrap is a helper program to parse/check expected output of a run
#
add_executable(dt-testwrap dt-testwrap.c)

#
# test driver programs
#
add_executable(t-cancel t-cancel.c)
target_link_libraries(t-cancel dthread)
add_executable(t-combo t-combo.c)
target_link_libraries(t-combo dthread)
add_executable(t-detach t-detach.c)
target_link_libraries(t-detach dthread)
add_executable(t-exit t-exit.c)
target_link_libraries(t-exit dthread)
add_executable(t-join t-join.c)
target_link_libraries(t-join dthread)
add_executable(t-mal-break t-mal-break.c)
target_link_libraries(t-mal-break dthread)
add_executable(t-mal-bsd t-mal-bsd.c)
target_link_libraries(t-mal-bsd dthread)
add_executable(t-natargs t-natargs.c)
target_link_libraries(t-natargs dthread)
add_executable(t-pthargs t-pthargs.c)
target_link_libraries(t-pthargs dthread)
add_executable(t-startup t-startup.c)
target_link_libraries(t-startup dthread)

#
# tests
#
if (DTHREAD_SYNC_PSHARED)
dthread_test(startup COMMAND ./t-startup -s DT_SHM_PATH)
dthread_test(startup4 NPROC 4 COMMAND ./t-startup -s DT_SHM_PATH)
dthread_test(exit-mainret NPROC 4 COMMAND ./t-exit -s DT_SHM_PATH
             -t 1 -m return
             TESTSCRIPT t-exit-mainret.sc)
dthread_test(exit-maindtexit NPROC 4 COMMAND ./t-exit -s DT_SHM_PATH
             -t 1 -m dtexit
             TESTSCRIPT t-exit-maindtexit.sc)
dthread_test(exit-mainptexit NPROC 4 COMMAND ./t-exit -s DT_SHM_PATH
             -t 1 -m ptexit
             TESTSCRIPT t-exit-mainptexit.sc)
dthread_test(exit-childret NPROC 4 COMMAND ./t-exit -s DT_SHM_PATH
             -c return
             TESTSCRIPT t-exit-childret.sc)
dthread_test(exit-childdtexit NPROC 4 COMMAND ./t-exit -s DT_SHM_PATH
             -c dtexit
             TESTSCRIPT t-exit-childdtexit.sc)
dthread_test(exit-childptexit NPROC 4 COMMAND ./t-exit -s DT_SHM_PATH
             -c ptexit
             TESTSCRIPT t-exit-childptexit.sc)
dthread_test(join NPROC 4 COMMAND ./t-join -s DT_SHM_PATH TESTSCRIPT t-join.sc)
dthread_test(detach NPROC 4 COMMAND ./t-detach -s DT_SHM_PATH
             TESTSCRIPT t-detach.sc)
dthread_test(cancel NPROC 4 COMMAND ./t-cancel -s DT_SHM_PATH
             TESTSCRIPT t-cancel.sc)
dthread_test(natargs NPROC 4 COMMAND ./t-natargs -s DT_SHM_PATH)
dthread_test(pthargs NPROC 4 COMMAND ./t-pthargs -s DT_SHM_PATH)
dthread_test(combo NPROC 4 COMMAND ./t-combo -s DT_SHM_PATH
             TESTSCRIPT t-combo.sc)
dthread_test(mal-break NPROC 4 COMMAND ./t-mal-break -s DT_SHM_PATH)
dthread_test(mal-bsd NPROC 4 COMMAND ./t-mal-bsd -s DT_SHM_PATH)
else()
    message(STATUS "tests disabled, they require DTHREAD_SYNC_PSHARED")
endif()
