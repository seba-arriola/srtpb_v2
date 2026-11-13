CC = gcc
CCFLAGS = -O2 -Wall
LIBFLAGS = -lm -lrt
LIBS = -L../ringserver/libmseed -L../ringserver/dalitool/libdali
INCLUDS = -I../ringserver/libmseed -I../ringserver/dalitool/libdali
LIBMSEED = -lmseed
LIBDALI  = -ldali
srtpbX = srtpb
sysswig := $(shell which swig)

all: srtpb msrtslice srtpbc

srtpb: srtpblib.i srtpblib.c
	swig -python srtpblib.i
	$(CC) $(CCFLAGS) $(LIBS) $(INCLUDS) -c srtpblib.c $(LIBFLAGS) $(LIBMSEED) $(LIBDALI) srtpblib_wrap.c $(shell python3-config --includes) -fPIC
	$(CC) $(CCFLAGS) -shared $(LIBS) $(INCLUDS) ../ringserver/dalitool/libdali/libdali.a ../ringserver/dalitool/libmseed/libmseed.a srtpblib.o srtpblib_wrap.o -o _srtpblib.so $(LIBFLAGS) $(LIBMSEED) $(LIBDALI)

msrtslice: msrtslice.c
	$(CC) $(CCFLAGS) $(LIBS) $(INCLUDS) ../ringserver/libmseed/libmseed.a ../ringserver/dalitool/libdali/libdali.a msrtslice.c -o msrtslice -lm  $(LIBMSEED) $(LIBDALI)

srtpbc: srtpbc.c
	$(CC) $(CCFLAGS) $(LIBS) $(INCLUDS) srtpbc.c ../ringserver/libmseed/libmseed.a ../ringserver/dalitool/libdali/libdali.a -o srtpbc -lm -lpthread -lrt


.IGNORE: clean
clean:
	rm -r *.o *_wrap.c *.so *.pyc srtpblib.py msrtslice srtpbc

  
