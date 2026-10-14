/*
 * Both tags and bodies use a View as a display widget.  The difference
 * between them is that bodies have scrollbars, whereas tags don't (but
 * tags automatically grow as required).  A View uses a Text to keep
 * track of the text that is currently not on screen, and a Frame to
 * display text.  A view keeps track of the visible area, the selected
 * area, and the place where we began typing (if we're in the middle of
 * typing).
 *
 * The View controls all aspects of viewing and selecting text, but has
 * no knowledge of how we store all the text.

 * If a view is not visible, v->r.max.y == v->r.min.y, and it's frame
 * will have been frcleared, i.e. not usable
 */

/* Emulator state for a terminal view (see process_vt100 in view.c) */
typedef struct TermState TermState;
struct TermState {
	int		rows, cols;	/* size of the screen, as sent to the pty */
	int		top, bottom;	/* scroll region, 1-based; 0,0 = whole screen */
	int		altscreen;	/* on the alternate screen? */
	char		*altsave;	/* text saved while on the alternate screen */
	ulong		altcursor;	/* cursor saved on entry to alternate screen */
	ulong		savedline, savedcol;	/* DECSC cursor */
	int		g[2];		/* 1 = DEC special graphics in G0 / G1 */
	int		shift;		/* 0 = G0 active, 1 = G1 active (SO/SI) */
	int		npend;		/* bytes of an incomplete sequence held over */
	char		pend[64];
};

struct View {
	Rectangle		r;		/* of whole view */
	Text			*t;
	Frame		f;
	Range		visible, sel;		/* visible, selected area */
	ulong		anchor;	/* where we most recently started typing */
	Scroll		*scroll;	/* 0 for tag */
	Tile			*tile;
	View			*next;	/* list of views displaying same Data */
	Bool			selecting;	/* we're busy dragging out a selection */
	Bool			autoindent;	/* autoindent in this view? */
	Bool			isterm;		/* is this a terminal view? */
	int				ptyfd;		/* PTY master fd for terminal I/O */
	TermState		*term;		/* terminal emulator state, or 0 */
};

/* Tell the pty about the current size of a terminal view */
void	view_termresize(View *v);
#define ISTAG(v) 	((v) && !(v)->scroll)
#define ISBODY(v)	( (v) && (v)->scroll)
#define ISVISIBLE(v) ( (v)->f.b!=0 )

/* Return true if 'p' is contained in 'v's scrollbar */
#define POINTINSCROLL(p,v) ( (p).x < (v)->r.min.x + SCROLLWIDTH + INSET )
