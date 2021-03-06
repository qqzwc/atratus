/*
 * fork - fork and print some messages to show what's going on
 *
 * Copyright (C)  2012 - 2013 Mike McCormack
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

struct pollfd
{
	int fd;
	short events;
	short revents;
};

void __stack_chk_fail(void)
{
}

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

void exit(int status)
{
	while (1)
	{
		__asm__ __volatile__ (
			"\tmov $1, %%eax\n"
			"\tint $0x80\n"
		:: "b"(status) : "memory");
	}
}

int read(int fd, char *buffer, size_t length)
{
	int r;
	__asm__ __volatile__ (
		"\tmov $3, %%eax\n"
		"\tint $0x80\n"
	:"=a"(r): "b"(fd), "c"(buffer), "d"(length) : "memory");
	return set_errno(r);
}

int write(int fd, const char *buffer, size_t length)
{
	int r;
	__asm__ __volatile__ (
		"\tmov $4, %%eax\n"
		"\tint $0x80\n"
	:"=a"(r): "b"(fd), "c"(buffer), "d"(length) : "memory");

	return set_errno(r);
}

int open(const char *filename, int flags)
{
	int r;
	__asm__ __volatile__ (
		"\tint $0x80\n"
	:"=a"(r): "a"(5), "b"(filename), "c"(flags) : "memory");

	return set_errno(r);
}

int close(int fd)
{
	int r;
	__asm__ __volatile__ (
		"\tint $0x80\n"
	:"=a"(r): "a"(6), "b"(fd) : "memory");

	return set_errno(r);
}

int execve(const char *filename,
	char **argv,
	char **env)
{
	int r;
	__asm__ __volatile__ (
		"\tint $0x80\n"
	: "=a"(r)
	: "a"(11), "b"(filename), "c"(argv), "d"(env)
	: "memory");

	return set_errno(r);
}

int fork(void)
{
	int r;
	__asm__ __volatile__ (
		"\tint $0x80\n"
	: "=a"(r)
	: "a"(2)
	: "memory");
	return set_errno(r);
}

int poll(struct pollfd *pfds, int nfds, int timeout)
{
	int r;
	__asm__ __volatile__ (
		"\tint $0x80\n"
	: "=a"(r)
	: "a"(168), "b"(pfds), "c"(nfds), "d"(timeout)
	: "memory");

	return set_errno(r);
}

int waitpid(int pid, int *status, int options)
{
	int r;
	__asm__ __volatile__ (
		"\tint $0x80\n"
	: "=a"(r)
	: "a"(7), "b"(pid), "c"(status), "d"(options)
	: "memory");
	return set_errno(r);
}

int main(int argc, char **argv, char **envp)
{
	int r;
	r = fork();
	if (r < 0)
	{
		char msg[] = "fork() failed\n";
		write(2, msg, sizeof msg - 1);
	}
	else if (r == 0)
	{
		char msg[] = "child!\n";
		char msg2[] = "child exitting...\n";
		write(2, msg, sizeof msg - 1);
		poll(0, 0, 2000);
		write(2, msg2, sizeof msg2 - 1);
	}
	else
	{
		int status = 0;
		char msg[] = "parent!\n";
		char msg2[] = "child exitted\n";

		write(2, msg, sizeof msg - 1);
		waitpid(-1, &status, 0);

		write(2, msg2, sizeof msg2 - 1);
	}
	return 0;
}

__asm__ (
	"\n"
".globl _start\n"
"_start:\n"
	"\tmovl 0(%esp), %eax\n"
	"\tlea 4(%esp), %ebx\n"
	"\tmov %ebx, %ecx\n"
"1:\n"
	"\tcmpl $0, 0(%ecx)\n"
	"\tlea 4(%ecx), %ecx\n"
	"\tjnz 1b\n"
	"\tpush %ecx\n"
	"\tmov %ecx, %edx\n"
"2:\n"
	"\tcmpl $0, 0(%ecx)\n"
	"\tlea 4(%ecx), %ecx\n"
	"\tjnz 2b\n"
	"\tpush %ecx\n"
	"\tpush %edx\n"
	"\tpush %ebx\n"
	"\tpush %eax\n"
	"\tcall main\n"
	"\tpush %eax\n"
	"\tcall exit\n"
);
