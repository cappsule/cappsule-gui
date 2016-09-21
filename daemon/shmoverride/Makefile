#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
# Copyright (C) 2010  Joanna Rutkowska <joanna@invisiblethingslab.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
#

include ../../../../Makefile.inc

CFLAGS += -fPIC -I../../common/ -I$(CUAPI_INCLUDE_PATH) -I../../../../../userland/include
LDFLAGS += -ldl
EXEC := shmoverride.so X-wrapper-qubes

.PHONY: strip

all: $(EXEC)

strip: all
	$(STRIP) $(EXEC)

shmoverride.so: shmoverride.o list.o ../../../../common/error.o ../../../../common/filesystem.o
	$(CC) -o $@ $^ $(LDFLAGS) -fPIC -shared

list.o: ../../common/list.c
	$(CC) $(CFLAGS) -shared -c -o $@ $^

X-wrapper-qubes: X-wrapper-qubes.o

../../../../common/%.o:
	$(MAKE) -C ../../../../common $(notdir $@)

%.o: %.c
	$(CC) -o $@ -c $< $(CFLAGS)

clean:
	rm -f *.o $(EXEC)
