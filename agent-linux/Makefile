include ../../../Makefile.inc

CFLAGS += -I../common -I$(CUAPI_INCLUDE_PATH) -I../../../include
LDFLAGS += -lX11 -lXext -lXcomposite -lXdamage -lrt -lXtst
EXEC := guiclient accept_override.so

.PHONY: strip

all: $(EXEC)

strip: all
	$(STRIP) $(EXEC)

guiclient: guiclient.o xevent.o message.o common.o ../common/gui_common.o ../common/keymap.o ../common/list.o ../../common/device_client.o ../../common/ring.o ../../common/xchan.o ../../../../userland/common/drop_priv.o ../../../../userland/common/error.o ../../../../userland/common/readall.o ../../../../userland/common/utils.o
	$(CC) -o $@ $^ $(LDFLAGS)

accept_override.so: accept_override.o
	$(CC) -o $@ $^ -ldl -fPIC -shared

accept_override.o: accept_override.c
	$(CC) -c -o $@ $^ $(CFLAGS) -fPIC

../../common/%.o:
	$(MAKE) -C ../../common $(notdir $@)

../../../../userland/common/%.o:
	$(MAKE) -C ../../../../userland/common $(notdir $@)

%.o: %.c
	$(CC) -o $@ -c $< $(CFLAGS)

clean:
	rm -f *.o $(EXEC)
