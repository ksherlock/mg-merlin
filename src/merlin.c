/*

merlin mode.
*/

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "def.h"
#include "funmap.h"
#include "kbd.h"

/* Pull in from modes.c */
extern int changemode(int, int, char *);

static int merlin_strip_trailp = TRUE;	/* Delete Trailing space? */
int merlin_uppercase = FALSE;

void merlin_init(void);
int merlin_lf(int f, int n);
int merlin_star(int f, int n);
int merlin_tab(int f, int n);
int merlin_space(int f, int n);
int merlin_toggle(int f, int n);
int merlin_comment_line(int f, int n);
int merlin_left_align_insert(int f, int n);


int merlin_gotobop(int f, int n);
int merlin_gotoeop(int f, int n);

// int merlin_set_tab_stops(int f, int n);
int merlin_toggle_uppercase(int f, int n);

int merlin_getcolpos(struct mgwin *wp);
int merlin_getgoal(struct line *dlp);
void merlin_render_line(struct line *lp, struct mgwin *wp, char *vtext, int lbound);


static int in_whitespace(struct line *lp, int len);


static PF merlin_pf_null[] = {
	NULL
};

static PF merlin_pf_insert[] = {
	selfinsert
};

static PF merlin_pf_space[] = {
	merlin_tab
};

static PF merlin_pf_left_insert[] = {
	merlin_left_align_insert
};

static PF merlin_cx_semi[] = {
	merlin_comment_line
};

static PF merlin_esc_bracket[] = {
	merlin_gotobop,
	rescan,
	merlin_gotoeop
};

static PF merlin_pf_cc[] = {
	merlin_tab,		/* ^I */
	enewline,		/* ^J */
	rescan,			/* ^K */
	rescan,			/* ^L */
	merlin_lf		/* ^M */
};

static struct KEYMAPE (1) merlin_cx_map = {
	1,
	1,
	rescan,
	{
		/* emacs uses control-x, control-;
		 * however, control-; is not tty-happy */
		{ ';', ';', merlin_cx_semi , NULL }
	}
};


static struct KEYMAPE (1) merlin_esc_map = {
	1,
	1,
	rescan,
	{
		{ '{', '}', merlin_esc_bracket , NULL },
	}
};

static struct KEYMAPE (11) merlin_map = {
	11,
	11,
	rescan,
	{
		{ CCHR('I'), CCHR('M'), merlin_pf_cc, NULL },
		{ CCHR('X'), CCHR('X'), merlin_pf_null, (KEYMAP *)&merlin_cx_map },
		{ CCHR('['), CCHR('['), merlin_pf_null, (KEYMAP *)&merlin_esc_map },

		{ ' ', ' ', merlin_pf_space, NULL },
		{')', ')', merlin_pf_insert, NULL },		/* don't match braces */
		{ '*', '*', merlin_pf_left_insert, NULL },	/* comment? */
		{ ':', ':', merlin_pf_left_insert, NULL },	/* local label? */
		{ ';', ';', merlin_pf_left_insert, NULL },	/* comment? */
		{ ']', ']', merlin_pf_left_insert, NULL },	/* local label? */
		// {']', ']', merlin_pf_insert, NULL },
		{'}', '}', merlin_pf_insert, NULL },		/* don't match braces */
	}
};

void
merlin_init(void)
{
	funmap_add(merlin_toggle, "merlin", 0);
	funmap_add(merlin_comment_line, "comment-line", 0);
	funmap_add(merlin_lf, "merlin-indent-and-newline", 0);
	funmap_add(merlin_toggle_uppercase, "merlin-uppercase", 0);
	funmap_add(merlin_gotobop, "merlin-backward-global-label", 0);
	funmap_add(merlin_gotoeop, "merlin-forward-global-label", 0);
	funmap_add(merlin_left_align_insert, "merlin-electric-insert", 0);
	funmap_add(merlin_tab, "merlin-electric-tab", 0);
	maps_add((KEYMAP *)&merlin_map, "merlin");
}

/*
 * Enable/toggle merlin-mode
 */
int
merlin_toggle(int f, int n)
{
	if (changemode(f, n, "merlin") == FALSE)
		return (FALSE);
	if (f & FFARG) {
		if (n <= 0)
			curbp->b_flag &= ~BFMERLIN;
		else
			curbp->b_flag |= BFMERLIN;
	} else
		curbp->b_flag ^= BFMERLIN;


	if ((curbp->b_flag & BFMERLIN) && curbp->b_tabw) {

		/* merlin 16+ 10, 16, 27 */
		/* quickedit cda 13, 19, 31 */
		/* lisa816 12, 22, 40 */
		/* orca 16, 25, 41 */
		curbp->b_tabw = 0;
		curbp->b_tabv[0] = (1 << 12) | (1 << 18) | (1 << 30);
		curbp->b_tabv[1] = 0;
		curbp->b_tabv[2] = 0;
		curbp->b_tabv[3] = 0;
	}

	/* redraw needed */
	curwp->w_rflag |= WFFRAME;
	return (TRUE);
}




/* convert tab to space.  if in a string or comment, insert.  otherwise collapse whitespace */
int
merlin_tab(int f, int n)
{
	int inwhitep = FALSE;	/* In leading whitespace? */

	if (n < 0)
		return (FALSE);
	if (n == 0)
		return (TRUE);

	inwhitep = in_whitespace(curwp->w_dotp,  curwp->w_doto); // llength(curwp->w_dotp));
	if (inwhitep) {
		/* advance to the next field */
		int i;
		for (i = curwp->w_doto, n = 0; i < llength(curwp->w_dotp); ++i, ++n) {
			int c = lgetc(curwp->w_dotp, i) & 0x7f;
			if (c > ' ') break;
		}
		if (n) forwchar(FFRAND, n);
		return (TRUE);
	} else {
		return linsert(1, ' ');
	}
}


/*
 * Indent-and-newline (technically, newline then indent)
 */
int
merlin_lf(int f, int n)
{
	char c;

	if (n < 0)
		return (FALSE);
	if (merlin_strip_trailp)
		deltrailwhite(FFRAND, 1);

	c = llength(curwp->w_dotp) ? lgetc(curwp->w_dotp, 0) & 0x7f : 0;
	if (enewline(FFRAND, 1) == FALSE)
		return (FALSE);

	/* if the previous line was a full-line comment, continue it */
	if (c == '*') {
		return linsert(1, '*');
	}
	return linsert(1, ' ');
}


/* remove leading white space and insert the character */
int merlin_left_align_insert(int f, int n) {
	int i;

	if (n < 0)
		return (FALSE);
	if (n == 0)
		return (TRUE);

	// if (n & FFARG)
		// return selfinsert(f, n);

	i = 0;
	while (i < curwp->w_doto && (lgetc(curwp->w_dotp, i) & 0x7f) <= ' ')
		++i;

	if (i > 0 && i == curwp->w_doto)
		delleadwhite(FFRAND, 1);
	return selfinsert(f, n);
}

/* star entered - if this is the first char in the line, delete leading whitespace */
int
merlin_star(int f, int n)
{
	int i;
	int comment = TRUE;

	for (i = 0; i < curwp->w_doto; ++i) {
		int c = lgetc(curwp->w_dotp, i) & 0x7f;
		if (c > ' ') {
			comment = FALSE;
			break;
		}
	}
	if (comment) {
		delleadwhite(f, n);
	}
	return selfinsert(f, n);
}

/* see region.c : getregion() */

/* returns c++ style range, ie, last is 1 beyond actual end. */
/* also moves dot to the start of the range */
static int getrange(struct line **first, struct line **last) {

	struct line *flp;
	struct line *blp;
	struct line *dotp;
	struct line *markp;
	struct line *headp;

	if (curwp->w_markp == NULL) return FALSE;

	markp = curwp->w_markp; 
	dotp = curwp->w_dotp;
	headp = curbp->b_headp;

	if (markp == dotp) {
		*first = dotp;
		*last = lforw(dotp);
		return TRUE;
	}

	flp = blp = dotp;
	for(;;) {
		if (flp == headp && blp == headp) return FALSE;

		if (flp != headp) {
			flp = lforw(flp);
			if (flp == markp) {
				*first = dotp;
				*last = lforw(markp);
				return TRUE;
			}
		}
		if (blp != headp) {
			blp = lback(blp);
			if (blp == markp) {
				*first = markp;
				*last = lforw(dotp);

				curwp->w_dotp = markp;
				curwp->w_doto = 0; //curwp->w_marko;
				curwp->w_dotline = curwp->w_markline;
				// curwp->w_rflag |= WFMOVE;
				return TRUE;
			}
		}


	}
	return FALSE;
}

/*
control-x control-;
comment or uncomment a line/region.
*/
int
merlin_comment_line(int f, int n)
{
	struct line *first = NULL;
	struct line *last = NULL;
	struct line *iter = NULL;
	struct line *dotp = curwp->w_dotp;
	int doto = curwp->w_doto;
	int dotline = curwp->w_dotline;

	if (curbp->b_flag & BFREADONLY) {
		dobeep();
		ewprintf("Buffer is read-only");
		return (FALSE);
	}

	if (f & FFARG) {
		if (n == 0) return TRUE;
	}


	if (!getrange(&first, &last)) {
		/* simple case - just do the current line */
		
		int ok;
		int l = llength(dotp);
		int c = l ? lgetc(dotp, 0) & 0x7f : 0;

		gotobol(FFRAND, 1); 
		if (c == '*') {
			ok = ldelete(1, KNONE);
			if (ok) --doto;
		} else {
			ok = linsert(1, '*');
			if (ok) ++doto;
		}

		/* move back to the starting point */
		forwchar(FFRAND, doto);
		return ok;
	}

	/* adding or deleting? just check the first line ... */
	int comment = llength(first) ? lgetc(first, 0) & 0x7f : 0;

	for (iter = first ; iter != last; iter = lforw(iter)) {
		gotobol(FFRAND, 1);

		int c = llength(iter) ? lgetc(iter, 0) & 0x7f : 0;

		gotobol(FFRAND, 1); 
		if (c == '*' && comment == '*') {
			ldelete(1, KNONE);
		}
		if (c != '*' && comment != '*') {
			linsert(1, '*');
		}
		forwline(FFRAND, 1);
	}
	/* restore original dot? */
	curwp->w_dotp = dotp;
	curwp->w_dotline = dotline;
	gotobol(FFRAND, 1);
	return TRUE;
}

#if 0
int
merlin_set_tab_stops(int f, int n)
{
	char	buf[32], *bufp;
	const char *errstr;
	char *token;
	int i;
	int stops[3];

#if 0
	if (f & FFARG) {
		if (n <= 0 || n > 16)
			return (FALSE);
		defb_tabw = n;
		return (TRUE);
	}
#endif

	if ((bufp = eread("Tab Stops: ", buf, sizeof(buf),
	    EFNUL | EFNEW | EFCR)) == NULL)
		return (ABORT);
	if (bufp[0] == '\0')
		return (ABORT);
	/* expect a list of 3 numbers */

	for (i = 0; i < 3; ++i) {
		token = strsep(&bufp, ",");
		if (!token) return dobeep_msg("Need 3 numbers");
		n = strtonum(token, 1, 80, &errstr);
		if (errstr)
			return (dobeep_msgs("Tab stop", errstr));
		stops[i] = n;
	}


	if (stops[0] >= stops[1] || stops[1] >= stops[2]) {
		return dobeep_msg("Bad tab stop");
	}

	curbp->b_tabv[0] = stops[0];
	curbp->b_tabv[1] = stops[1];
	curbp->b_tabv[2] = stops[2];

	curwp->w_rflag |= WFFRAME;
	return (TRUE);
}
#endif

int merlin_toggle_uppercase(int f, int n) {

	if (f & FFARG) {
		merlin_uppercase = n > 0;
	} else {
		merlin_uppercase = !merlin_uppercase;
	}
	curwp->w_rflag |= WFFRAME;
	return (TRUE);
}



/*
 * goto paragraph, but paragraphs are non-local labels
 */

static int is_global_line(struct line *lp) {
	int c;
	c = llength(lp) ? lgetc(lp, 0) & 0x7f: 0;
	return (c == '_' || isalpha(c));
}

int
merlin_gotobop(int f, int n)
{
	/* the other way... */
	if (n < 0)
		return (merlin_gotoeop(f, -n));

	/* if current line is a global, we want to skip over it */
	if (/*curwp->w_doto == 0 && */ is_global_line(curwp->w_dotp))
		++n;


	while (lback(curwp->w_dotp) != curbp->b_headp) {
		curwp->w_doto = 0;

		if (is_global_line(curwp->w_dotp)) {
			if (--n <= 0) break;
		}

		curwp->w_dotline--;
		curwp->w_dotp = lback(curwp->w_dotp);
	}
	/* force screen update */
	curwp->w_rflag |= WFMOVE;
	return (TRUE);
}



int
merlin_gotoeop(int f, int n)
{
	/* the other way... */
	if (n < 0)
		return (merlin_gotobop(f, -n));

	if ( /* curwp->w_doto == 0 && */ is_global_line(curwp->w_dotp))
		++n;


	while (lforw(curwp->w_dotp) != curbp->b_headp) {
		curwp->w_doto = 0;

		if (is_global_line(curwp->w_dotp)) {
			if (--n <= 0) break;
		}

		curwp->w_dotline++;
		curwp->w_dotp = lforw(curwp->w_dotp);
	}
	/* force screen update */
	curwp->w_rflag |= WFMOVE;
	return (TRUE);
}


/*
 * everything from here on down handles parsing and tabbing
 *
 *
 */


#define SEMI_ZERO_ALIGN 0
/*
 * n.b. - mg doesn't edit properly if we try to
 * display a leading ';' at the comments column; disable for now
 */

int
merlin_getcolpos(struct mgwin *wp)
{
	int i, c;
	int st = 0;
	int q = 0;
	int col = 0;

	int tabs[3];
	get_tab_stops(wp->w_bufp, tabs, 3);

	for (i = 0; i < wp->w_doto; ++i) {
		c = lgetc(wp->w_dotp, i);
		c &= 0x7f;
		if (c < ' ') c = ' ';
		switch (st) {
			case 0:
				switch(c) {
				case ' ':
					col = tabs[0];
					st = 2;
					break;
				case ';':
					#if SEMI_ZERO_ALIGN
					col = tabs[2] + 1;
					#else
					++col;
					#endif
					st = 7;
					break;
				case '*':
					st = 7;
					++col;
					break;
				default:
					++col;
					++st;
					break;
				}
				break;
			case 1:
				++col;
				if (c == ' ') {
					++st;
					if (col < tabs[0]) col = tabs[0];
				}
				break;
			case 2:
			case 4:
				if (c == ' ') break;
				++col;
				if (c == ';') {
					st = 7;
					if (col < tabs[2]) col = tabs[2];
					++col;
					break;
				}
				if (st == 4) {
					if (c == '"' || c == '\'')
						q = c;
				}
				++st;
				break;
			case 3:
				++col;
				if (c == ' ') {
					++st;
					if (col < tabs[1]) col = tabs[1];
				}
				break;

			case 5:
				++col;
				if (c == ' ' && !q) {
					++st;
					if (col < tabs[2]) col = tabs[2];
				}
				if (q) {
					if (q == c) q = 0;
				} else {
					if (c == '"' || c == '\'') q = c;
				}
				break;
			case 6:
				if (col == ' ') break;
				++col;
				++st;
				break;

			case 7:
				++col;
				break;
		}
	}
	return col;
}

/* this should be combined with getcolpos... */
int
merlin_getgoal(struct line *dlp)
{
	int c, i, col = 0;
	int st = 0;
	int q = 0;


	int tabs[3];
	get_tab_stops(curbp, tabs, 3);


	for (i = 0; i < llength(dlp); i++) {
		c = lgetc(dlp, i);
		c &= 0x7f;
		if (c < ' ') c = ' ';
		switch (st) {
			case 0:
				switch(c) {
				case ' ':
					col = tabs[0];
					st = 2;
					break;
				case ';':
					#if SEMI_ZERO_ALIGN
					col = tabs[2] + 1;
					#else
					++col;
					#endif
					st = 7;
					break;
				case '*':
					st = 7;
					++col;
					break;
				default:
					++col;
					++st;
					break;
				}
				break;
			case 1:
				++col;
				if (c == ' ') {
					++st;
					if (col < tabs[0]) col = tabs[0];
				}
				break;
			case 2:
			case 4:
				if (c == ' ') break;
				++col;
				if (c == ';') {
					st = 7;
					if (col < tabs[2]) col = tabs[2];
					++col;
					break;
				}
				if (st == 4) {
					if (c == '"' || c == '\'')
						q = c;
				}
				++st;
				break;
			case 3:
				++col;
				if (c == ' ') {
					++st;
					if (col < tabs[1]) col = tabs[1];
				}
				break;

			case 5:
				++col;
				if (c == ' ' && !q) {
					++st;
					if (col < tabs[2]) col = tabs[2];
				}
				if (q) {
					if (q == c) q = 0;
				} else {
					if (c == '"' || c == '\'') q = c;
				}
				break;
			case 6:
				if (col == ' ') break;
				++col;
				++st;
				break;

			case 7:
				++col;
				break;
		}
		if (col > curgoal) break;
	}
	return (i);
}


extern int vtcol;

#define vput(c) \
 if (vtcol >= ncol) { vtext[ncol - 1] = '$'; return; } \
 if (vtcol >= 0) vtext[vtcol] = c; \
 ++vtcol;

#define vtab(x) \
 do { \
   vput(' ') \
 } while ((vtcol + lbound) < (x))



void
merlin_render_line(struct line *lp, struct mgwin *wp, char *vtext, int lbound)
{
	int j;
	int st = 0;
	int q = 0;

	int tabs[3];
	get_tab_stops(wp->w_bufp, tabs, 3);

	for (j = 0; j < llength(lp); ++j) {
		int c = lgetc(lp, j);
		c &= 0x7f;
		if (c < ' ') c = ' ';

		switch(st) {
		case 0:
			switch(c) {
			case ';':
				#if SEMI_ZERO_ALIGN
				vtab(tabs[2]);
				#endif
			case '*':
				vput(c);
				st = 7;
				break;
			case ' ':
				vtab(tabs[0]);
				st = 2;
				break;
			default:
				if (merlin_uppercase) c = toupper(c);
				vput(c);
				st = 1;
				break;
			}
			break;


		case 1:
			if (c == ' ') {
				vtab(tabs[0]);
				++st;
			}
			else {
				if (merlin_uppercase) c = toupper(c);
				vput(c);
			}
			break;

		case 2:
		case 4:
			if (c == ' ') break;
			if (c == ';') {
				vtab(tabs[2]);
				vput(c);
				st = 7;
				break;
			}
			if (st == 4) {
				/* TODO -- also check for string opcode? */
				if (c == '"' || c == '\'')
					q = c;
			}
			++st;

		case 3:
			if (c == ' ') {
				vtab(tabs[1]);
				++st;
			}
			else {
				if (merlin_uppercase) c = toupper(c);
				vput(c);
			}
			break;

		case 5:
			if (c == ' ' && !q) {
				vtab(tabs[2]);
				++st;
				break;
			}

			if (q) {
				if (c == q) q = 0;
			}
			else {
				if (merlin_uppercase) c = toupper(c);
				if (c == '"' || c == '\'')
					q = c;
			}
			vput(c);
			break;

		case 6:
			if (c == ' ') break;
			++st;

		case 7:
			vput(c);
			break;
		}
	}
}



enum {
	WS_INSERT,
	WS_COLLAPSE
};

static int in_whitespace(struct line *lp, int len) {


	int i;
	int c;
	int st = 0;
	int q = 0;

	for (i = 0; i < len; ++i) {
		c = lgetc(lp, i) & 0x7f;
		if (c < ' ') c = ' ';

		switch (st) {
		case 0:
			if (c == '*' || c == ';') return WS_INSERT;
			if (c == ' ') st = 2;
			else ++st;
			break;
		case 1:
		case 3:
			if (c == ' ')
				++st;
			break;

		case 2:
		case 4:
		case 6:
			if (c == ';') return WS_INSERT;
			if (st == 4) {
				if (c == '"' || c == '\'') q = c;
			}
			if (c != ' ') ++st;
			break;

		case 5:
			if (q) {
				if (q == c) q = 0;
			} else {
				if (c == ' ') ++st;
				if (c == '"' || c == '\'') q = c;
			}
			break;

		case 7:
			return WS_INSERT;

		}
	}
	switch (st) {
	case 2:
	case 4:
	case 6:
		return WS_COLLAPSE;
	case 5:
		if (q) return WS_INSERT;
	case 0:
	case 1:
	case 3:
		/* peek at the next char to check if it's a space... */
		if (len >= llength(lp)) return WS_INSERT;
		c = lgetc(lp, len) & 0x7f;
		if (c <= ' ') return WS_COLLAPSE;
		return WS_INSERT;

	default:
		return WS_INSERT;
	}
}


