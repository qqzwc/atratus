# staticly linked tests
#
# Copyright (C) 2012 Mike McCormack
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
#

all:

CFLAGS += -Wall -fno-stack-protector
CFLAGS += -m32
CC = gcc

TARGETS = \
	args \
	brk \
	cat \
	date \
	dir \
	dir64 \
	echo \
	exec \
	exit \
	fork \
	gettimeofday \
	gs \
	hw \
	mmap \
	pipe \
	select \
	sleep \
	stack \
	stat \
	stat64 \
	uname

all: $(TARGETS)

# no libc
%: %.c
	@echo lin32 cc $<
	@$(CC) $(CFLAGS) -o $@ -static -nostartfiles -nodefaultlibs $<

clean:
	rm -f $(TARGETS) *.o
