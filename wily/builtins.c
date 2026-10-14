/*******************************************
 *	Builtin functions
 *******************************************/

#include "wily.h"
#include "view.h"
#include "data.h"
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <pty.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdlib.h>

/* Each of these functions takes a View used as the context
 * of the command, and an argument.  The argument may be null.
 */
/*
 * If given arguments, Kill all the external processes matching
 * the arguments.
 *
 * If given no arguments, generate a list of possible 'Kill' commands.
 */
static void
builtin_kill(View*v, char*s) {
	if (s)
		kill_all(s);
	else
		kill_list();
}

/*****************************************************
	Dotfiles, Font, Indent
	Toggle operations.  These probably _should_
	all act similarly, but right now they don't.  todo.
*****************************************************/

static void
dotfiles(View *v, char *arg) {
	show_dot_files = !show_dot_files;
}

static void
builtin_font(View *v, char *arg) {
	tile_setfont(view_tile(v), arg);
}

static void
builtin_autoindent(View *v, char *arg) {
	View *body;
	
	if ((body = view_body(v))) {
		body->autoindent = ! body->autoindent;
	} else {
		autoindent_enabled = !autoindent_enabled;
	}
}

/*****************************************************
	Undo, redo.  If called with an argument, they
	each go 'all the way'. 
*****************************************************/
static void
undo_ops(View *v, char*arg, Range (*undofn)(Text*, Bool))
{
	Range	r;

	if(!(v = view_body(v)))
		return;
	r = (*undofn)(v->t, arg!=0);
	if ROK(r) {
		view_show(v, r);
		view_select(v, r);
	}
}

static void undo(View *v, char *arg){undo_ops(v, arg, undo_undo);}
static void redo(View *v, char *arg) {undo_ops(v, arg, undo_redo);}

/*****************************************************
	Get, Put.  If called with an argument, they
	each go 'all the way'.
*****************************************************/
/* Read or write the window associated with 'v', possibly using 'arg' */
static void
getorput(View*v, char*arg, int(*fn)(Data*,char*)) {
	Data *d;
	Path	dest;
	char	*s;

	if (!(d = view_data(v)))
		return;

	if (arg && strlen(arg)) {
		data_addcontext(d, dest, arg);
		s = dest;
	} else {
		s = 0;
	}
	(*fn)(d, s);
}

static void	put(View *v, char *arg)	{getorput(v, arg, data_put);}
static void	get(View *v, char *arg)	{getorput(v, arg, data_get);}

/*****************************************************
	Quit/Putall ignore the context.
*****************************************************/
static void
putall(View *v, char *arg)
{
	data_putall();
}

static void
quit(View *v, char *arg) {
	cleanup_and_die(0);
}

/*****************************************************
	Del, Delcol delete the window or column 
	'v' belongs to.
*****************************************************/
static void
del(View *v, char *arg) {
	win_del(view_win(v));
}

static void
delcol(View*v, char *arg) {
	col_del(tile_col(v->tile));
}

/*****************************************************
	Cut/Snarf/Paste all act on last_selection
*****************************************************/
static void
cut(View *v, char *arg) {
	if((v=last_selection))
		view_cut(v, v->sel);
}

static void
builtin_snarf(View *v, char *arg) {
	if((v=last_selection))
		snarf(v->t, v->sel);
}

static void
builtin_paste(View *v, char *arg) {
	if((v=last_selection))
		view_paste(v);
}

/*****************************************************
	Anchor, Split act on either the window of the current
	context, or the last selection.
*****************************************************/
static void
anchor(View *v, char *arg) {
	Tile	*win;

	if ( (win=view_win(v)) || (win=view_win(last_selection)) ) {
		win_anchor(win, arg);
	}
}

/* If 'v' or 'last_selection' are in a window, split that window */
static void
split(View *v, char *arg)
{
	Tile	*win;

	if( (win = view_win(v)) || (win = view_win(last_selection)) ) {
		win_clone(win);
	}
}

void
builtin_term(View *v, char *arg)
{
	char	label[MAXPATH];
	Data	*d;
	View	*newv;
	int		pty_master, pty_slave;
	pid_t	pid;

	if (!arg || !arg[0])
		strcpy(label, "Term");
	else
		strcpy(label, arg);
	
	/* Create a new data object */
	d = data_alloc();
	if (!d) {
		diag(0, "Failed to allocate data for terminal");
		return;
	}
	
	/* Initialize text buffers */
	d->t = text_alloc(d, true);    /* body */
	d->tag = text_alloc(d, false); /* tag */
	if (!d->t || !d->tag) {
		diag(0, "Failed to allocate text buffers for terminal");
		return;
	}
	
	/* Set up the data object */
	strcpy(d->label, label);
	strcpy(d->cachedlabel, label);
	d->has_stat = false;
	d->names = 0;
	d->backupto = 0;
	
	/* Create the window first - this creates the views */
	win_new(label, d->tag, d->t);
	
	/* Now get the view for the body */
	newv = text_view(d->t);
	if (!newv) {
		diag(0, "Failed to create view for terminal");
		return;
	}
	
	/* Initialize with empty content (like a new file) */
	text_replace(d->t, text_all(d->t), rstring(0,0));
	
	/* Set up PTY for terminal I/O */
	if (openpty(&pty_master, &pty_slave, NULL, NULL, NULL) < 0) {
		diag(0, "Failed to create PTY: %s", strerror(errno));
		return;
	}

	/* Emulator state, and the pty size, must be in place before the
	 * shell starts so that it sees the right window size */
	newv->term = calloc(1, sizeof(TermState));
	newv->isterm = true;
	newv->ptyfd = pty_master;
	view_show(newv, newv->sel);
	view_termresize(newv);
	
	/* Fork the shell process */
	pid = fork();
	if (pid < 0) {
		diag(0, "Failed to fork process");
		close(pty_master);
		close(pty_slave);
		return;
	} else if (pid == 0) {  /* Child process */
		/* Close the master end in child */
		close(pty_master);
		
		/* Make the slave PTY the controlling terminal */
		setsid();
		ioctl(pty_slave, TIOCSCTTY, 1);
		
		/* Redirect stdin, stdout, stderr to the slave PTY */
		dup2(pty_slave, 0);  /* stdin */
		dup2(pty_slave, 1);  /* stdout */
		dup2(pty_slave, 2);  /* stderr */
		
		/* Close the slave fd (duplicated above) */
		if (pty_slave > 2) {
			close(pty_slave);
		}
		
		/* Set environment variables for terminal */
		setenv("TERM", "vt100", 1);
		setenv("NOCOLOR", "1", 1);
		
		/* Execute the shell */
		execlp("sh", "sh", "-i", (char*)0);
		
		/* If we get here, exec failed */
		diag(0, "Failed to exec shell");
		_exit(1);
	} else {  /* Parent process */
		/* Close the slave end in parent */
		close(pty_slave);
		
		/* Store the PTY master fd in the view */
		newv->isterm = true;
		newv->ptyfd = pty_master;
		
		/* Start monitoring the PTY for output from the shell */
		if (event_outputstart(pty_master, pid, "sh", label, newv) < 0) {
			diag(0, "Failed to start event monitoring for PTY: %s", strerror(errno));
			close(pty_master);
			newv->isterm = false;
			newv->ptyfd = -1;
			return;
		}
		
		/* Add the shell to the running processes list */
		addrunning("sh");
	}
	
	/* Show and select the new view */
	view_show(newv, newv->sel);
	view_select(newv, newv->sel);
}

/*****************************************************
	Clear, Look act on the body of the current
	context.
*****************************************************/
static void
clear(View *v, char *arg) {
	View	*body;

	if ((body = view_body(v))) 
		text_replace(body->t, text_all(body->t), rstring(0,0));
}

/* Look for 'arg' or the current selection in the body of 'v',
 * select and show the found thing if you find it.
 */
static void
look(View *v, char *arg) {
	if((v=view_body(v)))
		view_look(v, arg);
}

/*****************************************************
	New window, possibly called 'arg'.
	Try to use a window for context.
*****************************************************/
static void
new(View *v, char *arg) {
	Path	label;

	if(!arg)
		arg = "New";
		
	/* If 'v' isn't part of a window, maybe last_selection is */
	if(!view_win(v))
		v = last_selection;
	data_addcontext(view_data(v), label, arg);
	if(!data_find(label))
		data_open(label, true);
}

typedef struct Cmd Cmd;
struct Cmd {
	char	*name;
	void	(*cmd)(View *, char *);
};

/* _Must_ be kept in sorted order, we use bsearch on it */
static Cmd builtins[] = {
	{"Anchor", anchor},
	{"Clear", clear},
	{"Cut", cut},
	{"Del", del},
	{"Delcol", delcol},
	{"Dotfiles", dotfiles},
	{"Font", builtin_font},
	{"Get", get},
	{"Indent", builtin_autoindent},
	{"Kill", builtin_kill},
	{"Look", look},
	{"New", new},
	{"Newcol", col_new},
	{"Paste", builtin_paste},
	{"Put", put},
	{"Putall", putall},
	{"Quit", quit},
	{"Redo", redo},
	{"Snarf", builtin_snarf},
	{"Split", split},
	{"Term", builtin_term},
	{"Undo", undo},
};

static int
cmd_compare(Cmd *a,Cmd *b)
{
	return strcmp(a->name, b->name);
}

/*
 * PRE: 'v' is the context, 'cmd' the command.
 * POST: return true if we recognise the builtin, false otherwise.
 */
Bool
builtin(View *v, char *cmd, char *arg)
{
	Cmd	key, *c;

	assert(v);
	assert(cmd[0] && !isspace(cmd[0]));

	key.name = cmd;
	c = bsearch( &key, builtins, sizeof(builtins)/sizeof(Cmd),
				 sizeof(Cmd), 
			(int	(*)(const void *,const void*))cmd_compare);

	if (c) 
		(*c->cmd)(v, arg);
	return (Bool) c;
}
