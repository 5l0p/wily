/*******************************************
 *	View methods
 *******************************************/

#include "wily.h"
#include "view.h"
#include <ctype.h>
#include <termios.h>
#include <pty.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "text.h"

/* Helper functions for text navigation */
static ulong
text_linestart(Text *t, int line)
{
	ulong pos = 0;
	int current_line = 0;
	
	while (current_line < line && pos < text_length(t)) {
		int c;
		Tgetcset(t, pos);
		c = Tgetc(t);
		if (c == -1) break;
		if (c == NEWLINE) current_line++;
		pos++;
	}
	
	return pos;
}

static ulong
text_nextutf(Text *t, ulong pos, int count)
{
	ulong result = pos;
	int i;
	for (i = 0; i < count && result < text_length(t); i++) {
		if (Tgetc(t) != -1) {
			result++;
		} else {
			break;
		}
	}
	return result;
}

static ulong
text_prevutf(Text *t, ulong pos)
{
	if (pos > 0) {
		return pos - 1;
	}
	return pos;
}

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

/* Process VT100 escape sequences for terminal views */
static void
process_vt100(View *v, char *s, int n)
{
	char	*buf;
	int		bufsize;
	int		i, j;
	
	/* Allocate buffer for processed text (worst case: no escape sequences) */
	bufsize = n * UTFmax + 1;
	buf = salloc(bufsize);
	
	/* Process the input, stripping/handling escape sequences */
	for (i = 0, j = 0; i < n && j < bufsize - 1; ) {
		if (s[i] == '\033') {  /* ESC character */
			/* Handle escape sequences */
			i++;  /* skip ESC */
			if (i < n && s[i] == '[') {  /* CSI sequence */
				i++;  /* skip '[' */
				/* Parse parameters */
				int params[16];
				int nparams = 0;
				char param_buf[16];
				int p = 0;
				
				while (i < n && nparams < 16) {
					if (s[i] >= '0' && s[i] <= '9') {
						param_buf[p++] = s[i++];
						if (p >= 15) p = 15;
					} else if (s[i] == ';' && p > 0) {
						param_buf[p] = '\0';
						params[nparams++] = atoi(param_buf);
						p = 0;
						i++;
					} else if (s[i] == ';' && p == 0) {
						params[nparams++] = 0;
						i++;
					} else {
						break;
					}
				}
				if (p > 0) {
					param_buf[p] = '\0';
					params[nparams++] = atoi(param_buf);
				}
				
				if (i < n) {
					char final = s[i++];
					
					switch (final) {
					case 'H':  /* Cursor Position (CUP) */
					case 'f':  /* Horizontal and Vertical Position (HVP) */
						/* Move cursor to row, col (1-indexed) */
						{
							int row = (nparams > 0 && params[0] > 0) ? params[0] - 1 : 0;
							int col = (nparams > 1 && params[1] > 0) ? params[1] - 1 : 0;
							ulong pos = text_linestart(v->t, row);
							if (col > 0) {
								pos = text_nextutf(v->t, pos, col);
							}
							v->sel = range(pos, pos);
							view_show(v, v->sel);
						}
						break;
						
					case 'J':  /* Erase in Display (ED) */
						{
							int param = (nparams > 0) ? params[0] : 0;
							if (param == 2 || param == 3) {
								/* Clear entire screen and scrollback */
								text_replace(v->t, text_all(v->t), rstring(0, 0));
							} else if (param == 1) {
								/* Clear from start to cursor */
								text_replace(v->t, range(0, v->sel.p0), rstring(0, 0));
							} else {
								/* Clear from cursor to end (default) */
								text_replace(v->t, range(v->sel.p0, text_length(v->t)), rstring(0, 0));
							}
							view_show(v, v->sel);
						}
						break;
						
					case 'K':  /* Erase in Line (EL) */
						{
							int param = (nparams > 0) ? params[0] : 0;
							ulong line_start = text_startOfLine(v->t, v->sel.p0);
							ulong line_end = text_endOfLine(v->t, v->sel.p0);
							if (param == 1) {
								/* Clear from start of line to cursor */
								text_replace(v->t, range(line_start, v->sel.p0), rstring(0, 0));
							} else if (param == 2) {
								/* Clear entire line */
								text_replace(v->t, range(line_start, line_end), rstring(0, 0));
							} else {
								/* Clear from cursor to end of line (default) */
								text_replace(v->t, range(v->sel.p0, line_end), rstring(0, 0));
							}
							view_show(v, v->sel);
						}
						break;
						
					case 'm':  /* Select Graphic Rendition (SGR) - colors, bold, etc. */
						/* Strip all SGR sequences - we don't support colors */
						break;
						
					case 'A':  /* Cursor Up (CUU) */
						{
							int nlines = (nparams > 0 && params[0] > 0) ? params[0] : 1;
							ulong pos = v->sel.p0;
							for (int k = 0; k < nlines; k++) {
								pos = text_nl(v->t, pos, -1);
							}
							v->sel = range(pos, pos);
							view_show(v, v->sel);
						}
						break;
						
					case 'B':  /* Cursor Down (CUD) */
						{
							int nlines = (nparams > 0 && params[0] > 0) ? params[0] : 1;
							ulong pos = v->sel.p0;
							for (int k = 0; k < nlines; k++) {
								pos = text_nl(v->t, pos, 1);
							}
							v->sel = range(pos, pos);
							view_show(v, v->sel);
						}
						break;
						
					case 'C':  /* Cursor Forward (CUF) */
						{
							int ncols = (nparams > 0 && params[0] > 0) ? params[0] : 1;
							ulong pos = text_nextutf(v->t, v->sel.p0, ncols);
							v->sel = range(pos, pos);
							view_show(v, v->sel);
						}
						break;
						
					case 'D':  /* Cursor Backward (CUB) */
						{
							int ncols = (nparams > 0 && params[0] > 0) ? params[0] : 1;
							ulong pos = v->sel.p0;
							for (int k = 0; k < ncols; k++) {
								if (pos > 0) pos = text_prevutf(v->t, pos);
							}
							v->sel = range(pos, pos);
							view_show(v, v->sel);
						}
						break;
						
					case 's':  /* Save Cursor Position (SCP) */
						/* Save cursor position - store in view if needed */
						break;
						
					case 'u':  /* Restore Cursor Position (RCP) */
						/* Restore cursor position */
						break;
						
					case 'h':  /* Set Mode (SM) */
					case 'l':  /* Reset Mode (RM) */
						/* Handle modes like alternate screen buffer, etc. */
						/* For alternate screen (1049, 47), we'd need to save/restore buffer */
						break;
						
					case 'c':  /* Device Attributes (DA) */
						/* Ignore */
						break;
						
					case 'n':  /* Device Status Report (DSR) */
						/* Ignore */
						break;
						
					case 'r':  /* Set Scrolling Region (DECSTBM) */
						/* Ignore for now */
						break;
						
					default:
						/* Unhandled CSI sequence - skip it */
						break;
					}
				}
			} else if (i < n && s[i] == ']') {  /* OSC sequence */
				i++;  /* skip ']' */
				/* Skip until ST (ESC \) or BEL */
				while (i < n) {
					if (s[i] == '\033' && i + 1 < n && s[i + 1] == '\\') {
						i += 2;
						break;
					} else if (s[i] == '\007') {  /* BEL */
						i++;
						break;
					} else {
						i++;
					}
				}
			} else if (i < n && (s[i] == '7' || s[i] == '8')) {  /* DECSC/DECRC - save/restore cursor */
				i++;
			} else if (i < n && (s[i] == 'D' || s[i] == 'E' || s[i] == 'M')) {  /* Index, Next Line, Reverse Index */
				/* Simple cursor movements */
				i++;
			} else {
				/* Other ESC sequences - skip one character */
				if (i < n) i++;
			}
		} else {
			/* Copy regular character */
			int k;
			for (k = 0; k < UTFmax && i + k < n && j + k < bufsize - 1; k++) {
				buf[j + k] = s[i + k];
			}
			j += k;
			i += k;
		}
	}
	buf[j] = '\0';
	
	/* Insert the processed text */
	text_replaceutf(v->t, v->sel, buf);
	view_show(v, v->sel);
	
	free(buf);
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
