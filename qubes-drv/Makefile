include ../../../Makefile.inc

CFLAGS += -fPIC -I. -I../common/ -I$(CUAPI_INCLUDE_PATH) -I../../../include/ -I/usr/include/pixman-1 -I/usr/include/xorg
LDFLAGS += -shared
EXEC := qubes_drv.so

.PHONY: strip

all: $(EXEC)

strip: all
	$(STRIP) $(EXEC)

qubes_drv.so: qubes.o
	$(CC) -o $@ $^ $(LDFLAGS)

.o: %.c
	$(CC) -o $@ -c $< $(CFLAGS)

clean:
	rm -f *.o $(EXEC)
