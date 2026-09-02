// A very small subset of ncurses, over ANSI escapes.
//
// 3d-ascii-viewer needs seventeen curses calls. Porting ncurses to get
// them would mean a terminfo database, a compiled terminal description
// and a large library, none of which NeoOS has -- and all of which exist
// to abstract over terminals that differ. NeoOS has exactly one
// terminal, whose escape handling is in userland/term/vt.c, so the
// abstraction has nothing to abstract.
//
// So this is not "ncurses for NeoOS". It is the seventeen calls this
// program makes, implemented directly. Anything else is deliberately
// absent rather than stubbed: a missing function is a compile error that
// says which line to look at, while a stub is a silent wrong answer.
#ifndef NEOOS_NCURSES_SHIM_H
#define NEOOS_NCURSES_SHIM_H

#include <stdbool.h>

typedef struct { int _unused; } WINDOW;

extern WINDOW *stdscr;
extern int COLS, LINES;
extern int COLORS, COLOR_PAIRS;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define OK  0
#define ERR (-1)

// Attributes. Only colour pairs are used; the pair number is carried in
// the high bits the way ncurses does it, so COLOR_PAIR(n) can be OR'd
// into an attribute word.
#define A_COLOR      0x0000ff00
#define COLOR_PAIR(n) (((n) << 8) & A_COLOR)
#define PAIR_NUMBER(a) (((a) & A_COLOR) >> 8)

// Keys, in the private range above any byte value.
#define KEY_DOWN   0x102
#define KEY_UP     0x103
#define KEY_LEFT   0x104
#define KEY_RIGHT  0x105
#define KEY_RESIZE 0x19a

int  initscr_shim(void);
#define initscr() (initscr_shim(), stdscr)

int endwin(void);
int refresh(void);
int move(int y, int x);
int printw(const char *fmt, ...);
int getch(void);
int timeout_shim(int ms);
#define timeout(ms) timeout_shim(ms)
int noecho(void);
int curs_set(int visibility);
int keypad(WINDOW *win, bool enable);
int start_color(void);
int has_colors(void);
int init_color(short color, short r, short g, short b);
int color_content(short color, short *r, short *g, short *b);
int can_change_color(void);
int init_pair(short pair, short fg, short bg);
int attron(int attrs);
int attroff(int attrs);

// ncurses' getmaxyx writes through its arguments rather than taking
// pointers, so it has to stay a macro.
#define getmaxyx(win, y, x) do { (void)(win); (y) = LINES; (x) = COLS; } while (0)

#endif
