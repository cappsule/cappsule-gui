include ../../../Makefile.inc

CFLAGS += -I../common/ -I$(CUAPI_INCLUDE_PATH) -I../../../../userland/include
LDFLAGS += -lX11 -lXext -lXcomposite -lXdamage -lrt -lcrypto -lXtst
EXEC := guiserver shmoverride

.PHONY: shmoverride strip

all: $(EXEC)

strip: all
	$(STRIP) guiserver

guiserver: guiserver.o xevent.o message.o server_common.o ../common/gui_common.o ../common/keymap.o ../common/list.o ../../common/child.o ../../common/infos.o ../../common/ring.o ../../common/xchan.o ../../../../userland/common/error.o ../../../../userland/common/filesystem.o ../../../../userland/common/json.o ../../../../userland/common/log.o ../../../../userland/common/policy.o ../../../../userland/common/readall.o ../../../../userland/common/utils.o ../../../../userland/common/uuid.o
	$(CC) -o $@ $^ $(LDFLAGS) $(shell pkg-config --libs json-c cairo)

../../common/%.o:
	$(MAKE) -C ../../common $(notdir $@)

../../../../userland/common/%.o:
	$(MAKE) -C ../../../../userland/common $(notdir $@)

%.o: %.c
	$(CC) -o $@ -c $< $(CFLAGS)

shmoverride:
	$(MAKE) -C shmoverride

clean:
	rm -f *.o guiserver
	$(MAKE) -C shmoverride clean
