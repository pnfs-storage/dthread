app_main: testing join of detached child thread
rem_loop: running
app_main: detach child
app_main: join detached child
app_main: detached child join returned correct einval
app_main: cancel detached child
no zombie needed
app_main: testing detach interrupting pending join
rem_loop2: running
app_main: join child before it detaches
mgr: JOINED case 3
rem_loop2: detaching self
had a joinwait to cancel
app_main: detached child join returned correct einval
rem_loop2: tick 9...
app_main: DONE
