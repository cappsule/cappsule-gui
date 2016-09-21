include ../../../Makefile.inc

CFLAGS += -I../../../../include -I../../../../userland/include -I$(CUAPI_INCLUDE_PATH)
OBJ := gui_common.o keymap.o list.o

.PHONY: strip

all: $(OBJ)

strip: all

%.o: %.c
	$(CC) -o $@ -c $< $(CFLAGS)

clean:
	rm -f $(OBJ)
