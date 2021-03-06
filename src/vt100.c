/*
 * vt100 terminal emulation in a windows console
 *
 * Copyright (C) 2011 - 2013 Mike McCormack
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

#include <windows.h>
#include <winnt.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <assert.h>
#include "filp.h"
#include "process.h"
#include "tty.h"
#include "linux-errno.h"
#include "linux-defines.h"
#include "debug.h"
#include "tty.h"
#include "vt100.h"

#define MAX_VT100_PARAMS 8

struct vt100_filp
{
	struct tty_filp con;
	int state;
	int num[MAX_VT100_PARAMS];
	int num_count;
	int fg_color;
	int bg_color;
	int brightness;
	int reverse;
	int cursor_key_mode;
	int alternate_key_mode;
	int ansi;
	unsigned int unicode;
	int utf8_count;
	int charset;
	int scroll_start;
	int scroll_end;
	WCHAR saved_char;
	WORD saved_attr;
	HANDLE thread;
	HANDLE in;
	HANDLE out;
	CRITICAL_SECTION cs;
	CONSOLE_SCREEN_BUFFER_INFO info;
	bool read_info;
};

/*
 * GetConsoleScreenBufferInfo appears to be very slow
 * Avoid calling it as much as possible.
 */
static void vt100_get_info(struct vt100_filp *vt)
{
	if (vt->read_info)
	{
		GetConsoleScreenBufferInfo(vt->out, &vt->info);
		vt->read_info = 0;
	}
}

static void vt100_get_cursor_pos(struct vt100_filp *vt, int *x, int *y)
{
	vt100_get_info(vt);
	*x = vt->info.dwCursorPosition.X - vt->info.srWindow.Left + 1;
	*y = vt->info.dwCursorPosition.Y - vt->info.srWindow.Top + 1;
}

static int vt100_set_cursor_pos(struct vt100_filp *vt, int x, int y)
{
	COORD coord;
	DWORD height, width;
	BOOL r;

	dprintf("cursor to %d,%d\n", x, y);

	vt100_get_info(vt);

	height = vt->info.srWindow.Bottom - vt->info.srWindow.Top + 1;
	width = vt->info.srWindow.Right - vt->info.srWindow.Left + 1;
	if (x >= width)
		x = width;
	if (y >= height)
		y = height;
	if (y < 1)
		y = 1;
	if (x < 1)
		x = 1;
	coord.X = vt->info.srWindow.Left + x - 1;
	coord.Y = vt->info.srWindow.Top + y - 1;
	dprintf("cursor at %d,%d\n", coord.X, coord.Y);
	r = SetConsoleCursorPosition(vt->out, coord);
	if (!r)
		dprintf("failed to set cursor\n");
	vt->info.dwCursorPosition.X = coord.X;
	vt->info.dwCursorPosition.Y = coord.Y;
	return 0;
}

static DWORD vt100_get_attributes(struct vt100_filp *vt)
{
	DWORD dwAttribute = 0;

	if (vt->brightness)
		dwAttribute |= FOREGROUND_INTENSITY;

	switch (vt->reverse ? vt->bg_color : vt->fg_color)
	{
	case 30: /* Black */
		break;
	case 31: /* Red */
		dwAttribute |= FOREGROUND_RED;
		break;
	case 32: /* Green */
		dwAttribute |= FOREGROUND_GREEN;
		break;
	case 33: /* Yellow */
		dwAttribute |= FOREGROUND_RED | FOREGROUND_GREEN;
		break;
	case 34: /* Blue */
		dwAttribute |= FOREGROUND_BLUE;
		break;
	case 35: /* Magenta */
		dwAttribute |= FOREGROUND_RED | FOREGROUND_BLUE;
		break;
	case 36: /* Cyan */
		dwAttribute |= FOREGROUND_GREEN | FOREGROUND_BLUE;
		break;
	case 37: /* White */
		dwAttribute |= FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		break;
	}

	switch (vt->reverse ? vt->fg_color : vt->bg_color)
	{
	case 40: /* Black */
		break;
	case 41: /* Red */
		dwAttribute |= BACKGROUND_RED;
		break;
	case 42: /* Green */
		dwAttribute |= BACKGROUND_GREEN;
		break;
	case 43: /* Yellow */
		dwAttribute |= BACKGROUND_RED | BACKGROUND_GREEN;
		break;
	case 44: /* Blue */
		dwAttribute |= BACKGROUND_BLUE;
		break;
	case 45: /* Magenta */
		dwAttribute |= BACKGROUND_RED | BACKGROUND_BLUE;
		break;
	case 46: /* Cyan */
		dwAttribute |= BACKGROUND_GREEN | BACKGROUND_BLUE;
		break;
	case 47: /* White */
		dwAttribute |= BACKGROUND_BLUE | BACKGROUND_GREEN | BACKGROUND_RED;
		break;
	}

	return dwAttribute;
}

static int vt100_set_color(struct vt100_filp *vt)
{
	DWORD dwAttribute;
	int i;

	for (i = 0; i <= vt->num_count; i++)
	{
		int code = vt->num[i];

		if (code >= 30 && code <= 37)
			vt->fg_color = code;
		else if (code >= 40 && code <= 47)
			vt->bg_color = code;
		else if (code == 0)
		{
			vt->fg_color = 37;
			vt->bg_color = 40;
			vt->brightness = 0;
			vt->reverse = 0;
		}
		else if (code == 1)
			vt->brightness = code;
		else if (code == 7)
			vt->reverse = 1;
		else
			dprintf("Unhandled color code %d\n", code);
	}

	dwAttribute = vt100_get_attributes(vt);

	SetConsoleTextAttribute(vt->out, dwAttribute);
	return 0;
}

/* carriage return.  Move the cursor to the start of the line */
static void vt100_do_cr(struct vt100_filp *vt)
{
	int x, y;
	vt100_get_cursor_pos(vt, &x, &y);
	vt100_set_cursor_pos(vt, 0, y);
}

static void vt100_scroll(struct vt100_filp *vt, int up)
{
	SMALL_RECT rect;
	COORD newpos;
	CHAR_INFO ci;
	DWORD width;
	int start, end, target;

	vt100_get_info(vt);

	width = vt->info.srWindow.Right - vt->info.srWindow.Left + 1;
	if (width > 256)
	{
		dprintf("scroll area too wide: %ld\n", width);
		return;
	}

	if (up)
	{
		start =	vt->scroll_start + 1;
		end = vt->scroll_end;
		target = vt->scroll_start;
	}
	else
	{
		start = vt->scroll_start;
		end = vt->scroll_end - 1;
		target = start + 1;
	}

	dprintf("scroll: (%d-%d) moved to %d\n",
		start, end, target);

	/* select the region to move */
	rect.Top = vt->info.srWindow.Top + start - 1;
	rect.Bottom = vt->info.srWindow.Top + end - 1;
	rect.Left = vt->info.srWindow.Left;
	rect.Right = vt->info.srWindow.Right;

	/* select the new place */
	newpos.X = vt->info.srWindow.Left;
	newpos.Y = vt->info.srWindow.Top + target - 1;

	ci.Char.AsciiChar = ' ';
	ci.Attributes = vt100_get_attributes(vt);
	ScrollConsoleScreenBuffer(vt->out, &rect, NULL,
				 newpos, &ci);
	vt->read_info = 1;
}

/* line feed. Go down one line, but not to the start */
static void vt100_do_lf(struct vt100_filp *vt)
{
	int x, y;

	vt100_get_cursor_pos(vt, &x, &y);

	if (y >= vt->scroll_end)
	{
		vt100_scroll(vt, 1);
	}
	else
	{
		y++;
		vt100_set_cursor_pos(vt, x, y);
	}
}

static void vt100_move_up(struct vt100_filp *vt)
{
	int x, y;
	vt100_get_cursor_pos(vt, &x, &y);
	vt100_set_cursor_pos(vt, x, vt->scroll_end);
	vt100_scroll(vt, 0);
}

static void vt100_move_down(struct vt100_filp *vt)
{
	int x, y;
	vt100_get_cursor_pos(vt, &x, &y);
	vt100_set_cursor_pos(vt, x, vt->scroll_start);
	vt100_do_lf(vt);
}

static void vt100_do_backspace(struct vt100_filp *vt)
{
	int x, y;

	dprintf("BS\n");

	vt100_get_cursor_pos(vt, &x, &y);
	x--;
	vt100_set_cursor_pos(vt, x, y);
}

static void vt100_do_vertical_tab(struct vt100_filp *vt)
{
	int x, y;

	dprintf("VT\n");

	vt100_get_cursor_pos(vt, &x, &y);
	y++;
	vt100_set_cursor_pos(vt, x, y);
}

static void vt100_do_tab(struct vt100_filp *vt)
{
	int x, y;

	dprintf("TAB\n");

	vt100_get_cursor_pos(vt, &x, &y);
	x += 8;
	x %= 8;
	vt100_set_cursor_pos(vt, x, y);
}

static int vt100_utf8_out(struct vt100_filp *vt, unsigned char ch)
{
	DWORD write_count = 0;
	BOOL r;
	WCHAR wch[3];
	int n = 0;

	/* tail char 10xxxxxx */
	if ((ch & 0xc0) == 0x80)
	{
		if (vt->utf8_count == 0)
			return 0;
		vt->unicode <<= 6;
		vt->unicode |= (ch & 0x3f);
		vt->utf8_count--;
		if (vt->utf8_count != 0)
			return 0;
	}

	/* 110xxxxx 10xxxxxx */
	else if ((ch & 0xe0) == 0xc0)
	{
		vt->unicode = (ch & 0x1f);
		vt->utf8_count = 1;
		return 0;
	}

	/* 1110xxxx 10xxxxxx 10xxxxxx */
	else if ((ch & 0xf0) == 0xe0)
	{
		vt->unicode = (ch & 0x0f);
		vt->utf8_count = 2;
		return 0;
	}

	/* 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
	else if ((ch & 0xf8) == 0xf0)
	{
		vt->unicode = (ch & 0x07);
		vt->utf8_count = 3;
		return 0;
	}

	/* 111110xx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx */
	else if ((ch & 0xfc) == 0xf8)
	{
		vt->unicode = (ch & 0x03);
		vt->utf8_count = 4;
		return 0;
	}

	/* 1111110x 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx */
	else if ((ch & 0xfe) == 0xfc)
	{
		vt->unicode = (ch & 0x01);
		vt->utf8_count = 5;
		return 0;
	}

	/* 0xxxxxxx */
	else
		vt->unicode = ch;

	/* recode to UTF-16 */
	if (vt->unicode <= 0xffff)
		wch[n++] = vt->unicode;
	else
	{
		vt->unicode -= 0x10000;
		wch[n++] = 0xd800 | ((vt->unicode & 0xffc00) >> 10);
		wch[n++] = 0xdc00 | (vt->unicode & 0x3ff);
	}

	r = WriteConsoleW(vt->out, &wch, n, &write_count, NULL);
	if (!r)
		return -_L(EIO);

	vt->info.dwCursorPosition.X++;
	if (vt->info.dwCursorPosition.X >= vt->info.srWindow.Right)
		vt->read_info = 1;

	return 0;
}

static int vt100_write_normal(struct vt100_filp *vt, unsigned char ch)
{
	switch (ch)
	{
	case 0x1b:
		vt->state = 1;
		break;
	default:
		return vt100_utf8_out(vt, ch);
	}

	return 0;
}

static void vt100_save_char(struct vt100_filp *vt)
{
	DWORD count;
	COORD pos;

	vt100_get_info(vt);

	pos = vt->info.dwCursorPosition;

	ReadConsoleOutputCharacterW(vt->out, &vt->saved_char, 1,
					vt->info.dwCursorPosition, &count);
	ReadConsoleOutputAttribute(vt->out, &vt->saved_attr, 1,
					pos, &count);
	dprintf("save '%c'\n", (char) vt->saved_char);
}

static void vt100_restore_char(struct vt100_filp *vt)
{
	DWORD count;
	COORD pos;

	vt100_get_info(vt);

	pos = vt->info.dwCursorPosition;

	/* doesn't move cursor ... */
	WriteConsoleOutputCharacterW(vt->out, &vt->saved_char, 1,
					pos, &count);
	WriteConsoleOutputAttribute(vt->out, &vt->saved_attr, 1,
					pos, &count);

	dprintf("restore '%c'\n", (char) vt->saved_char);
}

static void vt100_device_status(struct vt100_filp *vt, int req)
{
	char response[16];

	switch (req)
	{
	case 6: /* query cursor position */
		vt100_get_info(vt);
		sprintf(response, "\x1b[%d;%dR",
			vt->info.dwCursorPosition.Y+1,
			vt->info.dwCursorPosition.X+1);
		dprintf("response = %s\n", response+1);
		tty_input_add_string(&vt->con, response);
		break;
	case 5: /* query device status */
	default:
		dprintf("unknown request %d\n", req);
	}
}

static void vt100_erase(struct vt100_filp *vt, int mode)
{
	COORD pos;
	DWORD count = 0;
	WCHAR row[256];
	WORD attr[256];
	DWORD dwAttribute;
	int width;
	int i;

	vt100_get_info(vt);

	pos = vt->info.dwCursorPosition;

	switch (mode)
	{
	case 0: /* from current to end */
		width = vt->info.srWindow.Right - pos.X;
		break;
	case 1: /* from start to current */
		width = pos.X - vt->info.srWindow.Left;
		pos.X = vt->info.srWindow.Left;
		break;
	case 2: /* entire line */
		width = vt->info.srWindow.Right - vt->info.srWindow.Left + 1;
		pos.X = vt->info.srWindow.Left;
		break;
	default:
		dprintf("unknown line erase mode %d\n", mode);
		return;
	}

	/* fill with spaces using current attribute */
	dwAttribute = vt100_get_attributes(vt);
	for (i = 0; i < 256; i++)
	{
		row[i] = ' ';
		attr[i] = dwAttribute;
	}

	WriteConsoleOutputCharacterW(vt->out, row, width,
					pos, &count);
	WriteConsoleOutputAttribute(vt->out, attr, width,
					pos, &count);
}

static void vt100_clear(struct vt100_filp *vt, DWORD length, COORD pos)
{
	DWORD count = 0;
	DWORD attribute = vt100_get_attributes(vt);
	FillConsoleOutputCharacter(vt->out, ' ', length, pos, &count);
	FillConsoleOutputAttribute(vt->out, attribute, length, pos, &count);
}

static void vt100_erase_screen(struct vt100_filp *vt, int n)
{
	COORD pos, top;
	DWORD width, height;

	vt100_get_info(vt);

	pos = vt->info.dwCursorPosition;
	top.X = vt->info.srWindow.Left;
	top.Y = vt->info.srWindow.Top;

	width = vt->info.srWindow.Right - vt->info.srWindow.Left;
	height = vt->info.srWindow.Bottom - vt->info.srWindow.Top;

	switch (n)
	{
	case 0:
		/* erase from cursor to end */
		vt100_clear(vt, (vt->info.srWindow.Bottom - pos.Y) * width +
				(vt->info.srWindow.Right - pos.X), pos);
		break;
	case 1:
		/* erase from top to cursor */
		vt100_clear(vt, (pos.Y - top.Y) * width +
				(pos.X - top.X), top);
		break;
	case 2:
		/* erase entire screen */
		vt100_clear(vt, height * width, top);
		break;
	}
}

static void vt100_move_cursor(struct vt100_filp *vt, int delta_x, int delta_y)
{
	COORD pos;

	vt100_get_info(vt);

	pos = vt->info.dwCursorPosition;

	pos.X += delta_x;
	pos.Y += delta_y;

	if (pos.X < 0)
		pos.X = 0;
	if (pos.X >= vt->info.dwSize.X)
		pos.X = vt->info.dwSize.X - 1;
	if (pos.Y < 0)
		pos.Y = 0;
	if (pos.Y >= vt->info.dwSize.Y)
		pos.Y = vt->info.dwSize.Y - 1;

	SetConsoleCursorPosition(vt->out, pos);
	vt->info.dwCursorPosition.X = pos.X;
	vt->info.dwCursorPosition.Y = pos.Y;
}

static void vt100_cursor_home(struct vt100_filp *vt)
{
	COORD pos;

	vt100_get_info(vt);

	pos = vt->info.dwCursorPosition;

	pos.Y = vt->info.srWindow.Top;
	pos.X = vt->info.srWindow.Left;

	SetConsoleCursorPosition(vt->out, pos);
	vt->info.dwCursorPosition.X = pos.X;
	vt->info.dwCursorPosition.Y = pos.Y;
}

static void vt100_screen_alignment(struct vt100_filp *vt)
{
	/*
	 * We're meant to write Es all over the screen
	 * but the most important thing is to
	 * move the cursor to the top left.
	 */
	vt100_cursor_home(vt);
}

/*
 * wait for a double height/double width command
 * (These are probably not possible on a Windows console)
 */
static int vt100_write_wait_dhdw_mode(struct vt100_filp *vt, unsigned char ch)
{
	switch (ch)
	{
	case '8':	/* fill upper area of screen with E */
		vt100_screen_alignment(vt);
		break;
	case '3':	/* double height line, top half */
	case '4':	/* double height line, bottom half */
	case '5':	/* single line mode */
	case '6':	/* double line mode */
		break;
	}
	vt->state = 0;
	return 0;
}

static void vt_hide_cursor(struct vt100_filp *vt, int enable)
{
	CONSOLE_CURSOR_INFO cci;

	vt100_get_info(vt);
	cci.bVisible = enable;
	SetConsoleCursorInfo(vt->out, &cci);
}

/*
 *  ESC [ ? N h  - enable mode N
 *  ESC [ ? N l  - disable mode N
 */
static int vt100_write_wait_mode_switch_num(struct vt100_filp *vt, unsigned char ch)
{
	int enable = 0;
	if (ch >= '0' && ch <= '9')
	{
		vt->num[0] *= 10;
		vt->num[0] += ch - '0';
		return 0;
	}
	else if (ch == 'h')
		enable = 1;
	else if (ch == 'l')
		enable = 0;
	else
	{
		vt->state = 0;
		return 0;
	}

	dprintf("switch mode %d %s\n", vt->num[0], enable ? "on" : "off");

	switch (vt->num[0])
	{
	case 1:	/* cursor key mode */
		vt->cursor_key_mode = enable;
		break;
	case 2:	/* vt52/ANSI mode */
		vt->ansi = enable;
		break;
	case 3:	/* 132 column mode: 132/80 */
		break;
	case 4:	/* scrolling mode: smooth/jump */
		break;
	case 5:	/* screen: reverse/normal */
		break;
	case 6:	/* origin relative/absolute */
		break;
	case 7:	/* wraparound on/off */
		break;
	case 8:	/* autorepeat on/off */
		break;
	case 9:	/* interlace on/off */
		break;
	case 25:
		vt_hide_cursor(vt, enable);
		break;
	default:
		break;
	}
	vt->state = 0;
	return 0;
}

static int vt100_write_wait_char_set_g0(struct vt100_filp *vt, unsigned char ch)
{
	dprintf("switch to g0 %c - unsupported\n", ch);
	vt->state = 0;
	return 0;
}

static int vt100_write_wait_char_set_g1(struct vt100_filp *vt, unsigned char ch)
{
	dprintf("switch to g1 %c - unsupported\n", ch);
	vt->state = 0;
	return 0;
}

static void vt100_identify(struct vt100_filp *vt)
{
	const char ident[] = "\033[0n"; /* vt100 */
	tty_input_add_string(&vt->con, ident);
}

static void vt100_enable_scrolling(struct vt100_filp *vt, int start, int end)
{
	int max;

	vt100_get_info(vt);
	max = vt->info.srWindow.Bottom - vt->info.srWindow.Top + 1;

	/* handle unset case */
	if (!start)
		start = 1;
	if (!end)
		end = max;

	/* clamp */
	if (start > max)
		start = max;
	if (end > max)
		end = max;

	dprintf("enable scrolling(%d,%d)\n", start, end);
	vt->scroll_start = start;
	vt->scroll_end = end;
}

static int vt100_write_wait_number(struct vt100_filp *vt, unsigned char ch)
{
	switch (ch)
	{
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		vt->num[vt->num_count] *= 10;
		vt->num[vt->num_count] += (ch - '0');
		break;
	case ';':
		vt->num_count++;
		if (vt->num_count >= MAX_VT100_PARAMS)
		{
			dprintf("too many tty params\n");
			vt->state = 0;
		}
		break;
	case 'm':
		vt100_set_color(vt);
		vt->state = 0;
		break;
	case 'n':
		vt100_device_status(vt, vt->num[0]);
		vt->state = 0;
		break;
	case 'A':
		if (!vt->num[0])
			vt->num[0]++;
		vt100_move_cursor(vt, 0, -vt->num[0]);
		vt->state = 0;
		break;
	case 'B':
		if (!vt->num[0])
			vt->num[0]++;
		vt100_move_cursor(vt, 0, vt->num[0]);
		vt->state = 0;
		break;
	case 'C':
		if (!vt->num[0])
			vt->num[0]++;
		vt100_move_cursor(vt, vt->num[0], 0);
		vt->state = 0;
		break;
	case 'D':
		if (!vt->num[0])
			vt->num[0]++;
		vt100_move_cursor(vt, -vt->num[0], 0);
		vt->state = 0;
		break;
	case 'K':
		vt100_erase(vt, vt->num[0]);
		vt->state = 0;
		break;
	case 'J':
		vt100_erase_screen(vt, vt->num[0]);
		vt->state = 0;
		break;
	case 'H':
		vt100_set_cursor_pos(vt, vt->num[1], vt->num[0]);
		vt->state = 0;
		break;
	case 'r':
		vt100_enable_scrolling(vt, vt->num[0], vt->num[1]);
		vt->state = 0;
		break;
	case '?':
		vt->state = 4;
		break;
	case 'c':
		vt100_identify(vt);
		vt->state = 0;
		break;
	case 'f':
		vt100_set_cursor_pos(vt, vt->num[1], vt->num[0]);
		vt->state = 0;
		break;
	default:
		dprintf("unknown code ESC [ %c\n", ch);
		vt->state = 0;
	}
	return 0;
}

static void vt100_reset(struct vt100_filp *vt)
{
	vt->state = 0;
	vt->num_count = 0;
	vt->fg_color = 37;
	vt->bg_color = 40;
	vt->brightness = 0;
	vt->cursor_key_mode = 0;
	vt->alternate_key_mode = 0;
	vt->utf8_count = 0;
	vt->charset = 0;
	vt->saved_char = '?';
	vt->saved_attr = FOREGROUND_RED;
	vt->ansi = 1;
	vt->read_info = 1;
	vt100_enable_scrolling(vt, 0, 0);
}

static int vt100_write_wait_first_char(struct vt100_filp *vt, unsigned char ch)
{
	int i;

	for (i = 0; i < MAX_VT100_PARAMS; i++)
		vt->num[i] = 0;
	vt->num_count = 0;

	switch (ch)
	{
	case '[':
		vt->state = 2;
		break;
	case '#':
		vt->state = 3;
		break;
	case '(':
		vt->state = 6;
		break;
	case ')':
		vt->state = 7;
		break;
	case '7':
		vt100_save_char(vt);
		vt->state = 0;
		break;
	case '8':
		vt100_restore_char(vt);
		vt->state = 0;
		break;
	case 'M':
		vt100_move_up(vt);
		vt->state = 0;
		break;
	case 'D':
		vt100_move_down(vt);
		vt->state = 0;
		break;
	case 'E':
		vt100_move_cursor(vt, 0, 1);
		vt->state = 0;
		break;
	case 'c':
		vt100_reset(vt);
		vt100_set_cursor_pos(vt, 0, 0);
		vt100_erase_screen(vt, 2);
		vt_hide_cursor(vt, 0);
		vt->state = 0;
		break;
	case '<':
		vt->ansi ^= 1;
		vt->state = 0;
		break;
	case '>':
		vt->alternate_key_mode = 1;
		vt->state = 0;
		break;
	case '=':
		vt->alternate_key_mode = 0;
		vt->state = 0;
		break;
	default:
		dprintf("unknown escape code '%c'\n", ch);
		vt->state = 0;
	}

	return 0;
}

static void vt100_do_nul(struct vt100_filp *vt)
{
}

typedef int (*vt100_state_fn)(struct vt100_filp *vt, unsigned char ch);

static vt100_state_fn vt100_state_list[] = {
	vt100_write_normal,
	vt100_write_wait_first_char,
	vt100_write_wait_number,
	vt100_write_wait_dhdw_mode,
	vt100_write_wait_mode_switch_num,
	NULL,
	vt100_write_wait_char_set_g0,
	vt100_write_wait_char_set_g1,
};

typedef void (*vt100_control_fn)(struct vt100_filp *vt);

static void vt100_do_escape(struct vt100_filp *vt)
{
	vt->state = 1;
}

static void vt100_do_bel(struct vt100_filp *vt)
{
	Beep(2000, 200);
}

static void vt100_do_si(struct vt100_filp *vt)
{
	vt->charset = 0;
}

static void vt100_do_so(struct vt100_filp *vt)
{
	vt->charset = 1;
}

static vt100_control_fn vt100_control_list[0x20] =
{
	/* 0x00 - NUL */
	vt100_do_nul,
	NULL,
	NULL,
	NULL,

	/* 0x04 - ^D */
	NULL,
	NULL,
	NULL,
	vt100_do_bel,

	/* 0x08 - ^H */
	vt100_do_backspace,
	vt100_do_tab,
	vt100_do_lf,
	vt100_do_vertical_tab,

	/* 0x0c - ^L */
	NULL,
	vt100_do_cr,
	vt100_do_si,
	vt100_do_so,

	/* 0x10 - ^P */
	NULL,
	NULL,
	NULL,
	NULL,

	/* 0x14 - ^T */
	NULL,
	NULL,
	NULL,
	NULL,

	/* 0x18 - ^Y */
	NULL,
	NULL,
	NULL,
	vt100_do_escape,

	/* 0x1c */
	NULL,
	NULL,
	NULL,
	NULL,
};

static int vt100_write(struct tty_filp *con, unsigned char ch)
{
	struct vt100_filp *vt = (void*) con;
	int r = 0;

	if (ch < 0x20)
	{
		vt100_control_fn fn;
		fn = vt100_control_list[ch];
		if (fn)
			fn(vt);
		else
			dprintf("vt100 ^%c unhandled\n", '@' + ch);
	}
	else
	{
		vt100_state_fn fn;
		fn = vt100_state_list[vt->state];
		r = fn(vt, ch);
	}

	return r;
}

static void vt100_send_cursor_code(struct vt100_filp *vt, char ch)
{
	char cursor[4];
	int n = 0;

	dprintf("cursor %c, ansi = %d\n", ch, vt->ansi);

	cursor[n++] = '\033';
	if (vt->ansi)
	{
		if (vt->cursor_key_mode)
			cursor[n++] = 'O';
		else
			cursor[n++] = '[';
	}
	cursor[n++] = ch;
	cursor[n++] = 0;

	tty_input_add_string(&vt->con, cursor);
}

static DWORD WINAPI vt100_input_thread(LPVOID param)
{
	struct vt100_filp *vt = param;
	unsigned char ch = 0;
	DWORD count = 0;
	BOOL r;
	INPUT_RECORD ir;

	while (1)
	{
		r = ReadConsoleInput(vt->in, &ir, 1, &count);
		if (!r)
		{
			fprintf(stderr, "\nReadConsoleInput failed\n");
			break;
		}
		if (!count)
			break;

		if (ir.EventType != KEY_EVENT)
			continue;

		if (!ir.Event.KeyEvent.bKeyDown)
			continue;

		switch (ir.Event.KeyEvent.wVirtualKeyCode)
		{
		case VK_RETURN:
			tty_input_add_char(&vt->con, '\n');
			break;
		case VK_UP:
			vt100_send_cursor_code(vt, 'A');
			break;
		case VK_DOWN:
			vt100_send_cursor_code(vt, 'B');
			break;
		case VK_RIGHT:
			vt100_send_cursor_code(vt, 'C');
			break;
		case VK_LEFT:
			vt100_send_cursor_code(vt, 'D');
			break;
		case VK_DELETE:
			tty_input_add_char(&vt->con, 0x7f);
			break;
		default:
			ch = ir.Event.KeyEvent.uChar.AsciiChar;
			if (ch)
				tty_input_add_char(&vt->con, ch);
			break;
		}

	}

	fprintf(stderr, "\ninput thread died\n");

	return 0;
}

static void vt100_get_winsize(struct tty_filp *con, struct winsize *ws)
{
	struct vt100_filp *vt= (void*) con;

	vt100_get_info(vt);

	ws->ws_col = vt->info.srWindow.Right - vt->info.srWindow.Left + 1;
	ws->ws_row = vt->info.srWindow.Bottom - vt->info.srWindow.Top + 1;
	ws->ws_xpixel = 0;
	ws->ws_ypixel = 0;

	dprintf("winsize -> %d,%d\n", ws->ws_col, ws->ws_row);
}

static void vt100_lock(struct tty_filp *con)
{
	struct vt100_filp *vt = (void*) con;
	EnterCriticalSection(&vt->cs);
}

static void vt100_unlock(struct tty_filp *con)
{
	struct vt100_filp *vt = (void*) con;
	LeaveCriticalSection(&vt->cs);
}

struct con_ops vt100_ops = {
	.fn_lock = vt100_lock,
	.fn_unlock = vt100_unlock,
	.fn_write = vt100_write,
	.fn_get_winsize = vt100_get_winsize,
};

static struct vt100_filp *vt100_device;

static struct vt100_filp*
alloc_vt100_console(HANDLE in, HANDLE out, struct process *leader)
{
	struct vt100_filp *vt;
	struct tty_filp *con;
	DWORD id;

	vt = malloc(sizeof *vt);
	if (!vt)
		return NULL;
	memset(vt, 0, sizeof *vt);
	con = &vt->con;

	con->ops = &vt100_ops;

	tty_init(con, leader);

	vt->in = in;
	vt->out = out;
	vt100_reset(vt);
	if (!SetConsoleMode(in, 0))
		return NULL;
	if (!SetConsoleMode(out, ENABLE_PROCESSED_OUTPUT))
		return NULL;

	vt->read_info = 1;
	vt100_get_info(vt);

	InitializeCriticalSection(&vt->cs);

	vt->thread = CreateThread(NULL, 0, &vt100_input_thread, con, 0, &id);

	return vt;
}

struct filp*
get_vt100_console(HANDLE in, HANDLE out, struct process *leader)
{
	if (!vt100_device)
		vt100_device = alloc_vt100_console(in, out, leader);
	return &vt100_device->con.fp;
}
