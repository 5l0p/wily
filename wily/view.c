/*******************************************
 *	View methods
 *******************************************/

#include "wily.h"
#include "view.h"
#include <ctype.h>
#include <string.h>
#include <termios.h>
#include <pty.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "text.h"

static ulong
text_endOfLine(Text *t, ulong pos)
{
	ulong line_start = pos;
	ulong line_end = pos;
	
	/* Find start of line */
	while (line_start > 0) {
		ulong test_pos = line_start - 1;
		Tgetcset(t, test_pos);
		if (Tgetc(t) == NEWLINE) {
			line_start = test_pos + 1;
			break;
		}
		line_start = test_pos;
	}
	
	/* Find end of line */
	line_end = line_start;
	while (line_end < text_length(t)) {
		Tgetcset(t, line_end);
		if (Tgetc(t) == NEWLINE) {
			break;
		}
		line_end++;
	}
	
	return line_end;
}

static Rectangle nullrect = { {0,0}, {0,0}};

/* ---- VT100 / ANSI terminal emulation for terminal views ----
 *
 * A terminal view keeps its contents in a Text.  Outside the alternate
 * screen the Text is the whole scrollback and the screen is its last
 * 'rows' lines.  On the alternate screen the Text holds only the screen,
 * starting at line 0.  Positions are character (rune) offsets; lines are
 * found by scanning for NEWLINE.
 */

#define TERM_ESC	0x1b
#define TERM_BEL	0x07
#define TERM_SO		0x0e
#define TERM_SI		0x0f
#define TERM_NOLINE	((ulong)-1)
#define TERM_DEFROWS	24
#define TERM_DEFCOLS	80

/* DEC special graphics character set (ESC ( 0), for bytes 0x5f..0x7e */
static char *dec_graphics[32] = {
	" ",		/* 5f */
	"\xe2\x97\x86",	/* 60 diamond */
	"\xe2\x96\x92",	/* 61 checkerboard */
	"\xe2\x90\x89",	/* 62 HT */
	"\xe2\x90\x8c",	/* 63 FF */
	"\xe2\x90\x8d",	/* 64 CR */
	"\xe2\x90\x8a",	/* 65 LF */
	"\xc2\xb0",	/* 66 degree */
	"\xc2\xb1",	/* 67 plus-minus */
	"\xe2\x90\xa4",	/* 68 NL */
	"\xe2\x90\x8b",	/* 69 VT */
	"\xe2\x94\x98",	/* 6a lower right corner */
	"\xe2\x94\x90",	/* 6b upper right corner */
	"\xe2\x94\x8c",	/* 6c upper left corner */
	"\xe2\x94\x94",	/* 6d lower left corner */
	"\xe2\x94\xbc",	/* 6e crossing */
	"\xe2\x8e\xba",	/* 6f scan 1 */
	"\xe2\x8e\xbb",	/* 70 scan 3 */
	"\xe2\x94\x80",	/* 71 horizontal line */
	"\xe2\x8e\xbc",	/* 72 scan 7 */
	"\xe2\x8e\xbd",	/* 73 scan 9 */
	"\xe2\x94\x9c",	/* 74 left T */
	"\xe2\x94\xa4",	/* 75 right T */
	"\xe2\x94\xb4",	/* 76 bottom T */
	"\xe2\x94\xac",	/* 77 top T */
	"\xe2\x94\x82",	/* 78 vertical line */
	"\xe2\x89\xa4",	/* 79 <= */
	"\xe2\x89\xa5",	/* 7a >= */
	"\xcf\x80",	/* 7b pi */
	"\xe2\x89\xa0",	/* 7c not equal */
	"\xc2\xa3",	/* 7d pound */
	"\xc2\xb7"	/* 7e middle dot */
};

static int
term_rows(View *v)
{
	return (v->term && v->term->rows > 0) ? v->term->rows : TERM_DEFROWS;
}

static int
term_cols(View *v)
{
	return (v->term && v->term->cols > 0) ? v->term->cols : TERM_DEFCOLS;
}

/* Number of lines in the text: a trailing newline starts another line */
static ulong
term_nlines(Text *t)
{
	ulong n = 1;
	int c;

	Tgetcset(t, 0);
	while ((c = Tgetc(t)) != -1)
		if (c == NEWLINE)
			n++;
	return n;
}

/* Position of the first character of line 'line', or TERM_NOLINE */
static ulong
term_linepos(Text *t, ulong line)
{
	ulong pos = 0;
	ulong len = text_length(t);
	ulong l;

	for (l = 0; l < line; l++) {
		ulong le = text_endOfLine(t, pos);
		if (le >= len)
			return TERM_NOLINE;
		pos = le + 1;
	}
	return pos;
}

/* Index of the line containing position 'p' */
static ulong
term_lineof(Text *t, ulong p)
{
	ulong line = 0;
	ulong i;

	Tgetcset(t, 0);
	for (i = 0; i < p; i++) {
		int c = Tgetc(t);
		if (c == -1)
			break;
		if (c == NEWLINE)
			line++;
	}
	return line;
}

/* Replace 'r' in 'v's text with the 'n' bytes of UTF-8 in 's' */
static void
term_put(View *v, Range r, const char *s, int n)
{
	char *buf;

	if (n <= 0) {
		if (RLEN(r))
			text_replace(v->t, r, rstring(0, 0));
		return;
	}
	buf = salloc(n + 1);
	memcpy(buf, s, n);
	buf[n] = '\0';
	text_replaceutf(v->t, r, buf);
	free(buf);
}

/* A string of 'n' spaces, to be freed by the caller */
static char *
term_blanks(ulong n)
{
	char *b = salloc((int)n + 1);
	memset(b, ' ', n);
	b[n] = '\0';
	return b;
}

/* Overwrite 'n' characters from 'p' with spaces; the cursor is unchanged */
static void
term_blank(View *v, ulong p, ulong n)
{
	char *b;

	if (n == 0)
		return;
	b = term_blanks(n);
	term_put(v, range(p, p + n), b, (int)n);
	free(b);
}

/* Place the cursor at 'p', clamped to the text */
static void
term_setcursor(View *v, ulong p)
{
	ulong len = text_length(v->t);
	if (p > len)
		p = len;
	v->sel = range(p, p);
}

static ulong
term_curline(View *v)
{
	return term_lineof(v->t, v->sel.p0);
}

static ulong
term_curcol(View *v)
{
	return v->sel.p0 - text_startOfLine(v->t, v->sel.p0);
}

/* Index of the first line of the screen */
static ulong
term_screentop(View *v)
{
	ulong n;

	if (v->term->altscreen)
		return 0;
	n = term_nlines(v->t);
	return (n > (ulong)term_rows(v)) ? n - term_rows(v) : 0;
}

/* Position of the first character of the screen */
static ulong
term_screenstart(View *v)
{
	ulong p = term_linepos(v->t, term_screentop(v));
	return (p == TERM_NOLINE) ? text_length(v->t) : p;
}

/* Scroll region as absolute line numbers, inclusive */
static void
term_region(View *v, ulong *top, ulong *bot)
{
	ulong st = term_screentop(v);
	int t = v->term->top ? v->term->top : 1;
	int b = v->term->bottom ? v->term->bottom : term_rows(v);

	*top = st + t - 1;
	*bot = st + b - 1;
}

/* Make sure line 'line' exists, adding newlines at the end if needed */
static void
term_pad(View *v, ulong line)
{
	ulong n = term_nlines(v->t);

	for (; n <= line; n++)
		term_put(v, range(text_length(v->t), text_length(v->t)), "\n", 1);
}

/* Put the cursor at column 'col' of the line starting at 'ls'.  A real
 * terminal lets the cursor move into blank cells past the end of the text,
 * so the line is padded with spaces when needed.
 */
static void
term_gotocol(View *v, ulong ls, ulong col)
{
	ulong le = text_endOfLine(v->t, ls);
	ulong len = le - ls;
	ulong cols = term_cols(v);

	if (col > cols - 1)
		col = cols - 1;
	if (col > len) {
		char *b = term_blanks(col - len);
		term_put(v, range(le, le), b, (int)(col - len));
		free(b);
	}
	term_setcursor(v, ls + col);
}

/* Put the cursor at column 'col' of line 'line', creating lines as needed */
static void
term_gotoline(View *v, ulong line, ulong col)
{
	term_pad(v, line);
	term_gotocol(v, term_linepos(v->t, line), col);
}


/* Insert a blank line before line 'line' */
static void
term_insline(View *v, ulong line)
{
	ulong p;

	if (line > 0)
		term_pad(v, line - 1);
	p = term_linepos(v->t, line);
	if (p == TERM_NOLINE)
		p = text_length(v->t);
	term_put(v, range(p, p), "\n", 1);
}

/* Delete line 'line', including its newline */
static void
term_delline(View *v, ulong line)
{
	ulong len = text_length(v->t);
	ulong p = term_linepos(v->t, line);
	ulong le;

	if (p == TERM_NOLINE)
		return;
	le = text_endOfLine(v->t, p);
	if (le < len)
		term_put(v, range(p, le + 1), "", 0);	/* line and its newline */
	else if (p > 0)
		term_put(v, range(p - 1, len), "", 0);	/* last line: take the newline before it */
	else
		term_put(v, range(0, len), "", 0);
}

/* Scroll the region [top,bot] up one line: blank line appears at bot */
static void
term_scrollup(View *v, ulong top, ulong bot)
{
	term_delline(v, top);
	term_insline(v, bot);
}

/* Scroll the region [top,bot] down one line: blank line appears at top */
static void
term_scrolldown(View *v, ulong top, ulong bot)
{
	term_insline(v, top);
	term_delline(v, bot + 1);
}

/* Cursor up or down 'count' lines, stopping at the scroll region or
 * the edge of the screen.  Column is kept.
 */
static void
term_vmove(View *v, int delta, ulong count)
{
	ulong top, bot, line, col, target;
	ulong st = term_screentop(v);
	ulong scrbot = st + term_rows(v) - 1;

	term_region(v, &top, &bot);
	line = term_curline(v);
	col = term_curcol(v);
	if (delta < 0) {
		ulong lower = (line >= top && line <= bot) ? top : st;
		if (line <= lower)
			target = line;
		else if (line - lower >= count)
			target = line - count;
		else
			target = lower;
	} else {
		ulong upper = (line >= top && line <= bot) ? bot : scrbot;
		if (line >= upper)
			target = line;
		else if (upper - line >= count)
			target = line + count;
		else
			target = upper;
	}
	term_gotoline(v, target, col);
}

/* Cursor left or right 'count' characters, staying on the line */
static void
term_hmove(View *v, int delta, ulong count)
{
	ulong ls = text_startOfLine(v->t, v->sel.p0);
	ulong col = term_curcol(v);

	if (delta > 0)
		term_gotocol(v, ls, col + count);
	else
		term_gotocol(v, ls, (col < count) ? 0 : col - count);
}

/* Line feed (LF) and index (IND): down a line, scrolling at the bottom */
static void
term_linefeed(View *v)
{
	TermState *ts = v->term;

	if (!ts->altscreen && ts->top == 0 && ts->bottom == 0) {
		/* normal screen: the text grows downwards */
		ulong p = v->sel.p0;
		ulong eol = text_endOfLine(v->t, p);

		if (eol >= text_length(v->t)) {
			term_put(v, range(eol, eol), "\n", 1);
			term_setcursor(v, eol + 1);
		} else {
			term_gotoline(v, term_curline(v) + 1, term_curcol(v));
		}
	} else {
		ulong top, bot, line = term_curline(v);
		term_region(v, &top, &bot);
		if (line == bot) {
			term_scrollup(v, top, bot);
			term_gotoline(v, line, term_curcol(v));
		} else {
			term_vmove(v, 1, 1);
		}
	}
}

/* Reverse index (RI): up a line, scrolling at the top */
static void
term_reverseindex(View *v)
{
	ulong top, bot, line;

	term_region(v, &top, &bot);
	line = term_curline(v);
	if (line == top) {
		term_scrolldown(v, top, bot);
		term_gotoline(v, line, term_curcol(v));
	} else {
		term_vmove(v, -1, 1);
	}
}

/* Number of bytes taken by the first 'k' characters of 's' */
static int
term_byteoffset(const char *s, int n, ulong k)
{
	int i = 0;
	ulong count;

	for (count = 0; count < k && i < n; count++) {
		i++;
		while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80)
			i++;
	}
	return i;
}

/* Number of characters in a UTF-8 buffer */
static ulong
term_nrunes(const char *s, int n)
{
	ulong count = 0;
	int i;

	for (i = 0; i < n; i++)
		if (((unsigned char)s[i] & 0xC0) != 0x80)
			count++;
	return count;
}

/* Write 'n' bytes at the cursor, overwriting, and wrapping at the right
 * margin (autowrap).  The line is never made longer than the screen.
 */
static void
term_write(View *v, const char *s, int n)
{
	int cols = term_cols(v);

	while (n > 0) {
		ulong pos = v->sel.p0;
		ulong col = term_curcol(v);
		ulong k, over, eol;
		int bytes;

		if (col >= (ulong)cols) {
			term_linefeed(v);
			term_setcursor(v, text_startOfLine(v->t, v->sel.p0));
			continue;
		}
		k = term_nrunes(s, n);
		if (k > (ulong)cols - col)
			k = (ulong)cols - col;
		if (k == 0)
			k = 1;	/* stray continuation bytes */
		bytes = term_byteoffset(s, n, k);

		eol = text_endOfLine(v->t, pos);
		over = eol - pos;
		if (over > k)
			over = k;
		term_put(v, range(pos, pos + over), s, bytes);
		term_setcursor(v, pos + k);

		s += bytes;
		n -= bytes;
	}
}

/* Insert 'n' blanks at the cursor, pushing the rest of the line right */
static void
term_ich(View *v, ulong n)
{
	ulong p = v->sel.p0;
	ulong ls = text_startOfLine(v->t, p);
	ulong cols = term_cols(v);
	ulong le;

	if (n > cols)
		n = cols;
	if (n > 0) {
		char *b = term_blanks(n);
		term_put(v, range(p, p), b, (int)n);
		free(b);
	}
	/* keep the line within the screen width */
	le = text_endOfLine(v->t, p);
	if (le - ls > cols)
		term_put(v, range(ls + cols, le), "", 0);
	term_setcursor(v, p);
}

/* Delete 'n' characters at the cursor, pulling the rest of the line left */
static void
term_dch(View *v, ulong n)
{
	ulong p = v->sel.p0;
	ulong le = text_endOfLine(v->t, p);

	if (n > le - p)
		n = le - p;
	term_put(v, range(p, p + n), "", 0);
	term_setcursor(v, p);
}

/* Erase 'n' characters from the cursor, without moving anything */
static void
term_ech(View *v, ulong n)
{
	ulong p = v->sel.p0;
	ulong le = text_endOfLine(v->t, p);

	if (n > le - p)
		n = le - p;
	term_blank(v, p, n);
	term_setcursor(v, p);
}

/* Insert 'n' blank lines at the cursor line, within the scroll region */
static void
term_insertlines(View *v, ulong n)
{
	ulong top, bot, line = term_curline(v);
	ulong k;

	term_region(v, &top, &bot);
	if (line < top || line > bot)
		return;
	for (k = 0; k < n; k++) {
		term_delline(v, bot);
		term_insline(v, line);
	}
	term_gotoline(v, line, 0);
}

/* Delete 'n' lines at the cursor line, within the scroll region */
static void
term_deletelines(View *v, ulong n)
{
	ulong top, bot, line = term_curline(v);
	ulong k;

	term_region(v, &top, &bot);
	if (line < top || line > bot)
		return;
	for (k = 0; k < n; k++) {
		term_delline(v, line);
		term_insline(v, bot);
	}
	term_gotoline(v, line, 0);
}

/* Erase in display (ED).  Erasing keeps the layout, so the screen
 * still has all its lines afterwards.
 */
static void
term_erase_display(View *v, int mode)
{
	ulong p = v->sel.p0;
	ulong len = text_length(v->t);
	ulong s0 = term_screenstart(v);

	switch (mode) {
	case 1:		/* start of screen to cursor */
		if (p > s0)
			term_blank(v, s0, p - s0);
		break;
	case 2:		/* whole screen */
		term_put(v, range(s0, len), "", 0);
		if (p > s0)
			term_setcursor(v, s0);
		break;
	case 3:		/* whole screen and scrollback */
		term_put(v, range(0, len), "", 0);
		term_setcursor(v, 0);
		break;
	default:	/* cursor to end of screen */
		term_put(v, range(p, len), "", 0);
		term_setcursor(v, p);
		break;
	}
}

/* Erase in line (EL) */
static void
term_erase_line(View *v, int mode)
{
	ulong p = v->sel.p0;
	ulong ls = text_startOfLine(v->t, p);
	ulong le = text_endOfLine(v->t, p);

	switch (mode) {
	case 1:		/* start of line to cursor */
		term_blank(v, ls, p - ls);
		break;
	case 2:		/* whole line */
		term_blank(v, ls, le - ls);
		break;
	default:	/* cursor to end of line */
		term_put(v, range(p, le), "", 0);
		term_setcursor(v, p);
		break;
	}
}

/* Switch between the normal and the alternate screen */
static void
term_altscreen(View *v, int on)
{
	TermState *ts = v->term;
	Range all;

	if (on == ts->altscreen)
		return;
	all = text_all(v->t);
	if (on) {
		ts->altsave = text_duputf(v->t, all);
		ts->altcursor = v->sel.p0;
		ts->altscreen = 1;
		ts->top = ts->bottom = 0;
		term_put(v, all, "", 0);
		term_setcursor(v, 0);
	} else {
		int n = ts->altsave ? (int)strlen(ts->altsave) : 0;
		term_put(v, all, ts->altsave ? ts->altsave : "", n);
		free(ts->altsave);
		ts->altsave = 0;
		ts->altscreen = 0;
		ts->top = ts->bottom = 0;
		term_setcursor(v, ts->altcursor);
	}
}

/* Send 's' to the child process, as if typed */
static void
term_reply(View *v, const char *s)
{
	if (v->ptyfd >= 0 && write(v->ptyfd, s, strlen(s)) < 0)
		return;
}

/* Dispatch a complete CSI sequence.  'priv' is the private marker
 * ('?' etc.) or 0.
 */
static void
term_csi(View *v, int priv, int *params, int np, int final)
{
	TermState *ts = v->term;
	int p0 = (np > 0) ? params[0] : 0;
	int a = (p0 > 0) ? p0 : 1;	/* default count / row */
	ulong st, top, bot;
	int i;

	if (priv == '?') {
		if (final == 'h' || final == 'l')
			for (i = 0; i < np; i++)
				if (params[i] == 47 || params[i] == 1047 || params[i] == 1049)
					term_altscreen(v, final == 'h');
		return;	/* other DEC private modes are accepted and ignored */
	}
	if (priv)
		return;

	switch (final) {
	case 'A':	/* CUU */
		term_vmove(v, -1, a);
		break;
	case 'B':	/* CUD */
	case 'e':
		term_vmove(v, 1, a);
		break;
	case 'C':	/* CUF */
	case 'a':
		term_hmove(v, 1, a);
		break;
	case 'D':	/* CUB */
		term_hmove(v, -1, a);
		break;
	case 'E':	/* CNL */
		term_vmove(v, 1, a);
		term_setcursor(v, text_startOfLine(v->t, v->sel.p0));
		break;
	case 'F':	/* CPL */
		term_vmove(v, -1, a);
		term_setcursor(v, text_startOfLine(v->t, v->sel.p0));
		break;
	case 'G':	/* CHA */
	case '`':
		term_gotoline(v, term_curline(v), a - 1);
		break;
	case 'd':	/* VPA */
		st = term_screentop(v);
		term_gotoline(v, st + a - 1, term_curcol(v));
		break;
	case 'H':	/* CUP */
	case 'f': {	/* HVP */
		int col = (np > 1 && params[1] > 0) ? params[1] : 1;
		st = term_screentop(v);
		term_gotoline(v, st + a - 1, col - 1);
		break;
	}
	case 'J':	/* ED */
		term_erase_display(v, p0);
		break;
	case 'K':	/* EL */
		term_erase_line(v, p0);
		break;
	case '@':	/* ICH */
		term_ich(v, a);
		break;
	case 'P':	/* DCH */
		term_dch(v, a);
		break;
	case 'X':	/* ECH */
		term_ech(v, a);
		break;
	case 'L':	/* IL */
		term_insertlines(v, a);
		break;
	case 'M':	/* DL */
		term_deletelines(v, a);
		break;
	case 'S':	/* SU: scroll region up */
		term_region(v, &top, &bot);
		for (i = 0; i < a; i++)
			term_scrollup(v, top, bot);
		break;
	case 'T':	/* SD: scroll region down */
		term_region(v, &top, &bot);
		for (i = 0; i < a; i++)
			term_scrolldown(v, top, bot);
		break;
	case 'r': {	/* DECSTBM */
		int rows = term_rows(v);
		int t = (np > 0 && params[0] > 0) ? params[0] : 1;
		int b = (np > 1 && params[1] > 0) ? params[1] : rows;
		if (b > rows)
			b = rows;
		if (t >= b) {
			ts->top = ts->bottom = 0;
		} else if (t == 1 && b == rows) {
			ts->top = ts->bottom = 0;
		} else {
			ts->top = t;
			ts->bottom = b;
		}
		term_gotoline(v, term_screentop(v), 0);
		break;
	}
	case 's':	/* SCOSC */
		ts->savedline = term_curline(v);
		ts->savedcol = term_curcol(v);
		break;
	case 'u':	/* SCORC */
		term_gotoline(v, ts->savedline, ts->savedcol);
		break;
	case 'n':	/* DSR */
		if (p0 == 5) {
			term_reply(v, "\033[0n");
		} else if (p0 == 6) {
			char buf[32];
			sprintf(buf, "\033[%lu;%luR",
				term_curline(v) - term_screentop(v) + 1,
				term_curcol(v) + 1);
			term_reply(v, buf);
		}
		break;
	case 'c':	/* DA: report a VT100 with advanced video */
		if (p0 == 0)
			term_reply(v, "\033[?1;2c");
		break;
	default:
		/* SGR (m), modes (h/l) and anything else: accepted and ignored,
		 * since the view has no colours */
		break;
	}
}

/* Handle the escape sequence starting at s[i] == ESC.  Returns the index
 * of the first byte after it, or -1 if the sequence is incomplete.
 */
static int
term_escape(View *v, const char *s, int n, int i)
{
	TermState *ts = v->term;

	i++;	/* skip ESC */
	if (i >= n)
		return -1;

	switch (s[i]) {
	case '[': {	/* CSI */
		int priv = 0;
		int params[16];
		int np = 0;
		int cur = 0;
		int have = 0;
		int final;

		i++;
		if (i < n && (s[i] == '?' || s[i] == '>' || s[i] == '<' || s[i] == '=')) {
			priv = s[i];
			i++;
		}
		while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == ';' || s[i] == ':')) {
			if (s[i] == ';' || s[i] == ':') {
				if (np < 16)
					params[np++] = have ? cur : 0;
				cur = 0;
				have = 0;
			} else {
				if (cur < 100000)
					cur = cur * 10 + (s[i] - '0');
				have = 1;
			}
			i++;
		}
		if ((have || np > 0) && np < 16)
			params[np++] = cur;
		while (i < n && s[i] >= 0x20 && s[i] <= 0x2f)	/* intermediates */
			i++;
		if (i >= n)
			return -1;
		final = (unsigned char)s[i];
		if (final < 0x20)
			return i;	/* control character: abandon the sequence */
		i++;
		if (final >= 0x40 && final <= 0x7e)
			term_csi(v, priv, params, np, final);
		return i;
	}
	case ']': {	/* OSC: skip until BEL or ST (ESC \) */
		i++;
		while (i < n) {
			if (s[i] == TERM_BEL)
				return i + 1;
			if (s[i] == TERM_ESC && i + 1 < n && s[i + 1] == '\\')
				return i + 2;
			i++;
		}
		return -1;
	}
	case '(':	/* character set designation */
	case ')': {
		if (i + 1 >= n)
			return -1;
		if (s[i] == '(')
			ts->g[0] = (s[i + 1] == '0');
		else
			ts->g[1] = (s[i + 1] == '0');
		return i + 2;
	}
	case '*':
	case '+':
	case '#':
	case '%':
		if (i + 1 >= n)
			return -1;
		return i + 2;
	case 'D':	/* IND */
		term_linefeed(v);
		return i + 1;
	case 'M':	/* RI */
		term_reverseindex(v);
		return i + 1;
	case 'E':	/* NEL */
		term_linefeed(v);
		term_setcursor(v, text_startOfLine(v->t, v->sel.p0));
		return i + 1;
	case '7':	/* DECSC */
		ts->savedline = term_curline(v);
		ts->savedcol = term_curcol(v);
		return i + 1;
	case '8':	/* DECRC */
		term_gotoline(v, ts->savedline, ts->savedcol);
		return i + 1;
	default:
		/* ESC = and ESC > (keypad modes), ESC c, etc.: ignored */
		return i + 1;
	}
}

/* Length of an incomplete UTF-8 sequence at the end of s[start..n), or
 * n if there is none.  Its bytes are held over until the next read.
 */
static int
term_utf8tail(const char *s, int start, int n)
{
	int j;

	for (j = n - 1; j >= start && j >= n - 4; j--) {
		unsigned char c = (unsigned char)s[j];
		if ((c & 0xC0) != 0x80) {
			int need;
			if (c < 0xC0)
				need = 1;
			else if (c < 0xE0)
				need = 2;
			else if (c < 0xF0)
				need = 3;
			else
				need = 4;
			return (j + need > n) ? j : n;
		}
	}
	return n;
}

/* Process 'n' bytes of pty output.  Returns how many bytes were used;
 * the rest is an incomplete sequence to be processed with the next read.
 */
static int
term_process(View *v, const char *s, int n)
{
	TermState *ts = v->term;
	int i = 0;

	while (i < n) {
		unsigned char c = (unsigned char)s[i];

		if (c == TERM_ESC) {
			int r = term_escape(v, s, n, i);
			if (r < 0)
				return i;
			i = r;
		} else if (c == '\r') {
			term_setcursor(v, text_startOfLine(v->t, v->sel.p0));
			i++;
		} else if (c == '\n') {
			term_linefeed(v);
			i++;
		} else if (c == '\b') {
			ulong p = v->sel.p0;
			if (p > text_startOfLine(v->t, p))
				term_setcursor(v, p - 1);
			i++;
		} else if (c == '\t') {
			ulong col = term_curcol(v);
			int spaces = 8 - (int)(col % 8);
			term_write(v, "        ", spaces);
			i++;
		} else if (c == TERM_SO) {
			ts->shift = 1;
			i++;
		} else if (c == TERM_SI) {
			ts->shift = 0;
			i++;
		} else if (c < 0x20 || c == 0x7f) {
			/* BEL and other control characters: ignored */
			i++;
		} else if (ts->g[ts->shift] && c >= 0x5f && c <= 0x7e) {
			char *m = dec_graphics[c - 0x5f];
			term_write(v, m, (int)strlen(m));
			i++;
		} else {
			/* run of printable bytes (including UTF-8) */
			int start = i;
			int gfx = ts->g[ts->shift];
			int end;

			while (i < n) {
				unsigned char d = (unsigned char)s[i];
				if (d < 0x20 || d == 0x7f)
					break;
				if (gfx && d >= 0x5f && d <= 0x7e)
					break;
				i++;
			}
			end = (i == n) ? term_utf8tail(s, start, n) : i;
			term_write(v, s + start, end - start);
			if (end < i)
				return end;	/* incomplete UTF-8: keep it for the next read */
			i = end;
		}
	}
	return n;
}

/* Process pty output for a terminal view.  Bytes left over from an
 * incomplete sequence in the previous read are prepended.
 */
static void
process_vt100(View *v, char *s, int n)
{
	TermState *ts = v->term;
	char *buf;
	int total, used, left;

	if (!ts)
		return;
	total = ts->npend + n;
	buf = salloc(total + 1);
	memcpy(buf, ts->pend, ts->npend);
	memcpy(buf + ts->npend, s, n);
	buf[total] = '\0';

	used = term_process(v, buf, total);

	left = total - used;
	if (left > (int)sizeof ts->pend)
		left = 0;	/* a runaway sequence: give up on it */
	memcpy(ts->pend, buf + used, left);
	ts->npend = left;
	free(buf);

	view_show(v, v->sel);
}

/* Tell the pty the size of a terminal view, so that programs such as
 * screen and tmux lay themselves out correctly.
 */
void
view_termresize(View *v)
{
	struct winsize ws;
	TermState *ts = v->term;
	int rows, cols;

	if (!v->isterm || !ts || v->ptyfd < 0 || !ISVISIBLE(v))
		return;
	rows = v->f.maxlines;
	cols = Dx(v->f.r) / charwidth(v->f.font, '0');
	if (rows < 1 || cols < 1)
		return;
	ts->rows = rows;
	ts->cols = cols;
	ts->top = ts->bottom = 0;

	memset(&ws, 0, sizeof ws);
	ws.ws_row = rows;
	ws.ws_col = cols;
	ioctl(v->ptyfd, TIOCSWINSZ, &ws);
}

/* Store string representation of 'dot' for 'v' into 'buf'.
 * if 'isLine', we want a line address, otherwise we want
 * a character address.
 */
void
view_getdot(View *v, char*buf, Bool isLine) {
	if(isLine) {
		sprintf(buf, ":%d,.", text_linenumber(v->t, v->sel.p0));
	} else {
		/* character address */
		sprintf(buf, ":#%lu,.", v->sel.p0);
	}
}

Range
view_expand(View *v, Range r, char *s) {
	assert(ROK(r));
	if(RLEN(r))
		return r;
	if (RLEN(v->sel) && v->sel.p0 <= r.p0 && v->sel.p1 >= r.p1)
		return v->sel;
	else
		return text_expand(v->t, r, s);
}

/*****************************************
 	Allocate, deallocate, invariants 
 *****************************************/

/* Allocate and return a new View.  Doesn't attempt to draw it. */
View*
view_new(Font *f, Bool istag, Text *text, Tile *tile)
{
	View*v = NEW(View);
	Bitmap*b = 0;
	ulong 	length;
	ulong	sel;

	length = text_length(text);
	v->r = nullrect;
	frinit(&v->f, nullrect, f, b);
	v->visible = range(0,0);
	sel = istag? length : 0;
	v->sel = range(sel, sel);
	v->anchor = length;
	v->t = text;
	v->next = 0;
	v->selecting = false;
	v->autoindent = autoindent_enabled;
	v->isterm = false;
	v->ptyfd = -1;
	v->term = 0;
	text_addview(text, v);
	v->tile = tile;
	v->scroll = istag? 0 : scroll_alloc(b, nullrect);
	assert(view_invariants(v));
	return v;
}

/* Delete v and free its resources.  Return 0 for success */
int
view_delete(View *v){
	if (text_rmview(v->t, v))
		return -1;
	if(v==last_selection)
		view_setlastselection(0);
	if (v == last_focus)
		last_focus = 0;
	frclear(&v->f);
	if(v->scroll)
		free(v->scroll);
	if (v->term) {
		free(v->term->altsave);
		free(v->term);
		v->term = 0;
	}
	return 0;
}

Bool
view_invariants(View*v)
{
	Range	r;
	ulong	length;

	if(!v->f.b)
		return true;	/* all bets are off if we're not visible */

	length = text_length(v->t);
	/* The selection and visible region are sane */
	assert(ROK(v->sel));
	assert(ROK(v->visible));
	assert(v->sel.p1 <= length);
	assert(v->visible.p1 <= length);

	/* We're either displaying at least one line, or we're quite hidden */
	assert(RLEN(v->visible)==v->f.nchars);
	assert(v->f.nchars <= length);
	
	/* View height == integral number of lines
	assert(Dy(v->r) == 2*INSET + v->f.maxlines * v->f.font->height);
	 */

	if(!v->selecting) {
		/* The visible part of the View selection == the frame selection */
		r = rclip(v->sel, v->visible);
		assert( (r.p0 - v->visible.p0) == v->f.p0);
		assert( (r.p1 - v->visible.p0) == v->f.p1);
	}
	return true;
}

/*****************************************
 	Simple data hiding stuff
 *****************************************/

/* Return the 'Data' associated with 'v', or 0. */
Data*
view_data(View *v)
{
	return v ? text_data(v->t):  0;
}

/* Body associated with 'v' */
View*
view_body(View*v) {
	if(!v)
		return 0;
	return ISBODY(v) ? v : tile_body(v->tile);
}

/* The window 'v' is a part of, or 0. */
Tile*
view_win(View*v) {
	return v? tile_win(v->tile)  : 0;
}

/* The tile 'v' is part of, or 0. */
Tile*
view_tile(View*v) {
	return v? v->tile : 0;
}

Text*
view_text(View*v) {
	return v? v->t : 0;
}

Range
view_getsel(View*v) {
	return v->sel;
}

int
view_height(View*v) {
	return (v && v->f.b) ? Dy(v->r) : 0;
}

void
view_paste(View*v){
	undo_break(v->t);
	view_select(v, paste(v->t, v->sel));
	view_show(v, v->sel);
}

/*
 * Copy the range to the snarf buffer (unless it's empty).
 * Delete the text, update the View
 */
void
view_cut(View*v, Range r) {
	assert(ROK(r));
	if(RLEN(r)){
		snarf(v->t, r);
		undo_break(v->t);
		text_replace(v->t, r, rstring(0,0));
	}
	view_show(v, range(r.p0, r.p0));
}

void
view_append(View *v, char *s, int n) {
	Text	*t;
	ulong	len;
	Range	end;
	
	t = v->t;
	len = text_length(t);
	end = range(len,len);
	n = stripnulls(s, n);
	s[n] = 0;
	
	text_replaceutf(v->t, end, s);
}

/* Append 'n' bytes at 's' to 'v'.  If 'first' time, we replace
 * the selection, otherwise we append to the selection
 */
void
view_pipe(View *v, Bool *first, char *s, int n)
{
	ulong	p0 = v->sel.p0;
	Range	r;

	if (*first) {
		r = v->sel;
		*first = false;
	} else {
		r = range(v->sel.p1, v->sel.p1);
	}

	n = stripnulls(s, n);
	s[n]=0;
	
	/*
	 * If this is a terminal view, process VT100 escape sequences
	 * instead of inserting raw text
	 */
	if (v->isterm) {
		process_vt100(v, s, n);
		return;
	}
	
	text_replaceutf(v->t,  r,  s);
	r = range(p0, v->sel.p1);

	view_select(v, r);
}

/*
 * Change v's font.  If 'arg' is not null, use it, otherwise
 * toggle between Fonts 'fixed' and 'font'
 */
void
view_setfont(View *v, char*arg)
{
	frfont(&v->f, (v->f.font==font)? fixed: font);
	if(ISVISIBLE(v))
		view_reshaped(v, v->r);
}

/*
 * Perform the actual drawing of the border that indicates the
 * view is the last_selection.
 */

void
view_border(View *v, Bool set)
{
	Rectangle r;
	
	assert(v);
	r = v->r;
	r.min.x += SCROLLWIDTH + 4;
	
	if (tile_hidden(view_tile(v)))
		return;
	if (set)
		border(&screen, r, SELECTEDBORDER, F);
	else
		border(&screen, r, SELECTEDBORDER, 0);
	border(&screen, v->r, 1, F);
}

/* Indicate visually that 'v' is the 'last_selection' */
void
view_setlastselection(View *v)
{
	if (v == last_selection)
		return;

	if (last_selection)
		view_border(last_selection, false);
	if (v)
		view_border(v, true);
	last_selection = v;
}

/* Set the selection in 'v' to 'r'.  Indicate on screen if necessary */
void
view_select(View*v, Range r)
{
	assert(ROK(r));
	assert(view_invariants(v));

	frselectp(&v->f, F&~D);
	v->sel = r;
	r = rclip(v->sel, v->visible);
	v->f.p0 = r.p0 - v->visible.p0;
	v->f.p1 = r.p1 - v->visible.p0;
	frselectp(&v->f, F&~D);

	assert(view_invariants(v));
}

/* Warp the cursor to selection 'r' in 'v'.
 * PRE: 'v' is visible.  Some of 'r' is visible.
 */
void
view_warp (View *v, Range r)
{
	Point pt;

	assert(view_invariants(v));
	assert(ROK(r));
	assert(RINTERSECT(r, v->visible));
	assert(r.p1 <= text_length(v->t));
	assert(ISVISIBLE(v));
	
	pt = frptofchar(&v->f, r.p0 - v->visible.p0);
	pt.y += v->f.font->height/2;	/* middle of char */
	cursorset(pt);
}

/*
 * Replace Range 'r' with 's' in 'v'.  We've already changed the underlying
 * text buffer, we now just have to change the display, and update
 * v->visible and v->sel.
 */
static void
view_replace(View *v, Range r, Rstring s)
{
	Range	q = intersect(r, v->visible);	/* visible part of change */
	int	len;
	Bool	visible = ISVISIBLE(v);

	/* view_invariants don't hold at present because
	 * v->t->length has changed but v->sel hasn't
	 */
	assert(ROK(r));
	assert (RSOK(s));
	assert(RLEN(r) || RSLEN(s));	/* or we wouldn't get here */
	assert(r.p0 <= text_length(v->t));	/* even if we're deleting text, this will hold */

	if(visible && q.p1 >= q.p0) {	
		/* some of the replaced text is visible */
		Frame		*f = &v->f;

		q.p0 -= v->visible.p0;
		q.p1 -= v->visible.p0;
		
		if(RLEN(q))
			frdelete(f, q.p0 , q.p1);
		if(RSLEN(s))
			frinsert(f, s.r0,  s.r1, q.p0);
	}

	/* adjust our counters */
	len = RSLEN(s);
	v->visible.p0 = ladjust(v->visible.p0, r,len);
	v->visible.p1 = radjust(v->visible.p1, r,len);
	v->sel.p0 = 	radjust(v->sel.p0, r,len);
	v->sel.p1 =	radjust(v->sel.p1, r,len);
	v->anchor= 	ladjust(v->anchor, r,len);

	if(visible)
		fill(v);
	assert(view_invariants(v));
}

/* Refresh v because its text has changed arbitrarily. */
static void
view_refresh(View*v)
{
	if(ISVISIBLE(v)) {
		v->visible.p0 = 0;
		v->sel = range(v->visible.p0, v->visible.p0);
		frdelete(&v->f, 0, v->f.nchars);
		fill(v);
	}
}

void
viewlist_refresh(View*v) {
	for(;v; v = v->next)
		view_refresh(v);
}

void
viewlist_replace(View*v, Range r, Rstring s) {
	for(;v; v = v->next)
		view_replace(v, r, s);
}
