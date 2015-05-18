/*
 * Test emulation of %gs accesses
 *
 * Copyright (C)  2006-2012 Mike McCormack
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

void *__stack_chk_guard = 0;
void __stack_chk_fail_local(void) { return; }
void __stack_chk_fail(void) { return; }

/*
 * minwgcc and gcc have different definitions of __thread
 * errno should be per thread,
 * leave as global until we can load an ELF binary
 */
int errno;

static inline int set_errno(int r)
{
	if ((r & 0xfffff000) == 0xfffff000)
	{
		errno = -r;
		r = -1;
	}
	return r;
}


ssize_t write(int fd, const void *buffer, size_t len)
{
	int r;
	__asm__ __volatile__(
		"int $0x80"
	: "=a" (r)
	: "a" (SYS_write), "b" (fd), "c" (buffer), "d" (len)
	: "memory");
	return set_errno(r);
}

void exit(int code)
{
	while (1)
	{
		__asm__ __volatile__(
			"int $0x80"
		: : "a" (SYS_exit), "b" (code)
		: "memory" );
	}
}

/* copy the value of %fs to %gs if %gs isn't set already */
static inline int setup_gs(void *where)
{
	struct {
		unsigned int entry_number;
		unsigned long base_addr;
		unsigned int limit;
		unsigned int seg_32bit : 1;
		unsigned int contents : 2;
		unsigned int read_exec_only : 1;
		unsigned int limit_in_pages : 1;
		unsigned int seg_not_present : 1;
		unsigned int usable : 1;
		unsigned int garbage : 25;
	} ldt = {0};
	int r;

	ldt.entry_number = -1;
	ldt.base_addr = (unsigned long) where;
	ldt.limit = 0xfffff;	/* This is what a Windows TEB gives us */
	ldt.seg_32bit = true;
	ldt.usable = true;

	__asm__ __volatile__ (
		"\tpush %%ebx\n"
		"\tmovl %0,%%ebx\n"
		"\tmovl $243,%%eax\n"
		"\tint $0x80\n"
		"\tcmpl $0, %%eax\n"
		"\tjnz 1f\n"
		"\tmovl (%%ebx),%%eax\n"
		"\tmovl %%eax,%%ebx\n"
		"\tshl $3, %%ebx\n"
		"\torl $3, %%ebx\n"
		"\tmov %%bx, %%gs\n"
		"1:\n"
		"\tpop %%ebx\n"
	:"=a"(r) :"r"(&ldt): "memory");

	return r;
}

uint8_t tls[0x2000];

void _start(void)
{
	char msg[] = "%%gs accesses ok\n";
	uint32_t in, out, address;

	setup_gs(tls + 0x1000);

	/* test mov */
	in = 0xbead0101;
	address = 0;

	__asm__ __volatile__ (
		"\tmovl %%eax, %%gs:(%%ebx)\n"
		"\tmovl %%gs:(%%ebx), %%eax\n"
	:"=a"(out) :"b"(address), "a"(in): "memory");
	if (in != out)
		exit(1);

	in = 0xbead0102;
	address = 4;
	__asm__ __volatile__ (
		"\tmovl %%ecx, %%gs:(%%ebx)\n"
		"\tmovl %%gs:(%%ebx), %%eax\n"
	:"=a"(out) :"b"(address), "c"(in): "memory");

	if (in != out)
		exit(2);

	in = 0xbead0103;
	__asm__ __volatile__ (
		"\tmovl %%eax, %%gs:(8)\n"
		"\tmovl %%gs:(8), %%eax\n"
	:"=a"(out) :"a"(in): "memory");
	if (in != out)
		exit(3);

#if 0
	__asm__ __volatile__ (
		"\txor %%eax,%%eax\n"
		"\tcmpl $0xbead0103, %%gs:(8)\n"
		"\tjz 1f\n"
		"\tincl %%eax\n"
		"1:\n"
		"\tadc $0, %%eax"
	:"=a"(out) :: "memory");
	if (0 != out)
		exit(4);
#endif

	__asm__ __volatile__ (
		"\txor %%eax,%%eax\n"
		"\tmovl %%eax, %%gs:(8)\n"
		"\tcmpl $0, %%gs:(8)\n"
		"\tjz 1f\n"
		"\tincl %%eax\n"
		"1:\n"
		"\tadc $0, %%eax"
	:"=a"(out) :: "memory");
	if (0 != out)
		exit(5);

	__asm__ __volatile__ (
		"\tmovl $8, %%eax\n"
		"\tmovl $0xbead0104, %%gs:(%%eax)\n"
		"\tmovl %%gs:(8), %%eax\n"
	:"=a"(out) :: "memory");
	if (0xbead0104 != out)
		exit(6);

	write(1, msg, sizeof msg - 1);

	exit(0);
}
