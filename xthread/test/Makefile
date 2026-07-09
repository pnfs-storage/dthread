CC = mpicc
CFLAGS = -g -O3 -Wall -I/usr/local/include
LDFLAGS = -L/usr/local/lib -Wl,-rpath,/usr/local/lib

TARGETS = hello_pthread hello_dthread hello_xthread

all: $(TARGETS)

hello_pthread: hello_pthread.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lpthread

hello_dthread: hello_dthread.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -ldthread -lrt

hello_xthread: hello_xthread.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -ldthread -lxthread -lpthread -lrt

clean:
	rm -f $(TARGETS) *.o
